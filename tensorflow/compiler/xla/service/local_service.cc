/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/local_service.h"

#include <string>
#include <utility>
#include <vector>

#include "absl/memory/memory.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "tensorflow/compiler/xla/client/executable_build_options.h"
#include "tensorflow/compiler/xla/client/xla_computation.h"
#include "tensorflow/compiler/xla/execution_options_util.h"
#include "tensorflow/compiler/xla/service/backend.h"
#include "tensorflow/compiler/xla/service/computation_layout.h"
#include "tensorflow/compiler/xla/service/executable.h"
#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_execution_profile.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/service/hlo_module_config.h"
#include "tensorflow/compiler/xla/service/hlo_module_util.h"
#include "tensorflow/compiler/xla/service/platform_util.h"
#include "tensorflow/compiler/xla/shape_layout.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/lib/gtl/cleanup.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/stream_executor_no_cuda.h"

namespace xla {

/* static */ StatusOr<std::unique_ptr<LocalService>> LocalService::NewService(
    const ServiceOptions& options) {
  se::Platform* platform = options.platform();
  if (platform == nullptr) {
    TF_ASSIGN_OR_RETURN(platform, PlatformUtil::GetDefaultPlatform());
  }

  BackendOptions backend_options;
  backend_options.set_platform(platform)
      .set_intra_op_parallelism_threads(options.intra_op_parallelism_threads())
      .set_allowed_devices(options.allowed_devices());

  TF_ASSIGN_OR_RETURN(std::unique_ptr<Backend> backend,
                      Backend::CreateBackend(backend_options));

  std::unique_ptr<LocalService> service(
      new LocalService(options, std::move(backend)));
  return std::move(service);
}

LocalService::LocalService(const ServiceOptions& options,
                           std::unique_ptr<Backend> execute_backend)
    : Service(options, std::move(execute_backend)) {}

namespace {

// Retrieves the parameter metadata for the given computation and parameter
// number.
//
// If the parameter number is invalid for this computation, nullopt is
// returned. When the return value has_value(), nullptr will never be
// the held value.
std::optional<const OpMetadata*> ParameterMetadata(
    const XlaComputation& computation, int parameter_number) {
  for (const HloComputationProto& comp : computation.proto().computations()) {
    if (comp.id() == computation.proto().entry_computation_id()) {
      for (const HloInstructionProto& instr : comp.instructions()) {
        if (instr.opcode() == HloOpcodeString(HloOpcode::kParameter) &&
            instr.parameter_number() == parameter_number) {
          if (!instr.has_metadata()) {
            return std::nullopt;
          }
          return &instr.metadata();
        }
      }
    }
  }
  return std::nullopt;
}

}  // namespace

StatusOr<std::unique_ptr<HloModuleConfig>> LocalService::GetHloModuleConfig(
    const XlaComputation& computation,
    const absl::Span<const Shape* const> argument_layouts,
    const ExecutableBuildOptions& build_options) {
  const HloModuleProto& proto = computation.proto();
  TF_RET_CHECK(proto.has_host_program_shape());
  ProgramShape program_shape(proto.host_program_shape());

  // Validate incoming layouts.
  if (argument_layouts.size() != program_shape.parameters_size()) {
    return InvalidArgument(
        "Invalid number of arguments for computation: expected %d, got %u.",
        program_shape.parameters_size(), argument_layouts.size());
  }

  for (int i = 0; i < argument_layouts.size(); ++i) {
    const Shape& argument_shape = *argument_layouts[i];
    TF_RETURN_IF_ERROR(
        ShapeUtil::ValidateShapeWithOptionalLayout(argument_shape));
    if (!ShapeUtil::Compatible(argument_shape, program_shape.parameters(i))) {
      std::optional<const OpMetadata*> metadata =
          ParameterMetadata(computation, /*parameter_number=*/i);
      auto metadata_string = [&metadata]() -> std::string {
        if (!metadata.has_value()) {
          return "";
        }
        CHECK(metadata.value() != nullptr);
        const OpMetadata& m = *metadata.value();
        if (!m.source_file().empty()) {
          return absl::StrFormat(" (%s:%d)", m.source_file(), m.source_line());
        }
        return "";
      };
      return InvalidArgument(
          "Invalid argument shape for argument %d%s, expected %s, got %s.", i,
          metadata_string(),
          ShapeUtil::HumanString(program_shape.parameters(i)),
          ShapeUtil::HumanString(argument_shape));
    }
  }
  if (build_options.result_layout() != nullptr) {
    TF_RETURN_IF_ERROR(ValidateResultShape(*build_options.result_layout(),
                                           program_shape.result()));
  }

  ExecutionOptions execution_options =
      CreateExecutionOptions(build_options, &program_shape);

  return CreateModuleConfig(program_shape, argument_layouts,
                            &execution_options);
}

StatusOr<std::vector<std::unique_ptr<Executable>>>
LocalService::CompileExecutables(
    const XlaComputation& computation,
    const absl::Span<const Shape* const> argument_layouts,
    const ExecutableBuildOptions& build_options) {
  TF_ASSIGN_OR_RETURN(
      std::unique_ptr<HloModuleConfig> module_config,
      GetHloModuleConfig(computation, argument_layouts, build_options));

  VLOG(3) << "Computation Layout: "
          << module_config->entry_computation_layout().ToString();

  TF_ASSIGN_OR_RETURN(
      se::StreamExecutor * executor,
      execute_backend_->stream_executor(build_options.device_ordinal()));

  // TODO(cjfj): Investigate why there are a couple of test failures when the
  // single partition computations are built using `BuildExecutables`, fix it,
  // and remove this special case (provided the performance if similar).
  if (build_options.num_partitions() == 1) {
    TF_ASSIGN_OR_RETURN(
        std::unique_ptr<Executable> executable,
        BuildExecutable(computation.proto(), std::move(module_config),
                        execute_backend_.get(), executor,
                        {build_options.device_allocator(),
                         build_options.compile_thread_pool()},
                        build_options.run_backend_only()));
    std::vector<std::unique_ptr<Executable>> executables;
    executables.push_back(std::move(executable));
    return executables;
  } else {
    std::vector<std::unique_ptr<HloModuleConfig>> module_configs;
    module_configs.push_back(std::move(module_config));
    // BuildExecutables uses the executors length to determine the number of
    // cores per module, but otherwise only uses the first executor.
    std::vector<se::StreamExecutor*> executors(build_options.num_partitions(),
                                               executor);

    return BuildExecutables(
        /*module_protos=*/{&computation.proto()}, std::move(module_configs),
        execute_backend_.get(), {executors},
        Compiler::CompileOptions{build_options.device_allocator(),
                                 build_options.compile_thread_pool()},
        build_options.run_backend_only());
  }
}

StatusOr<std::vector<std::unique_ptr<AotCompilationResult>>>
LocalService::CompileAotResults(
    const XlaComputation& computation,
    const absl::Span<const Shape* const> argument_layouts,
    const ExecutableBuildOptions& build_options) {
  TF_ASSIGN_OR_RETURN(
      std::unique_ptr<HloModuleConfig> module_config,
      GetHloModuleConfig(computation, argument_layouts, build_options));

  TF_ASSIGN_OR_RETURN(
      se::StreamExecutor * executor,
      execute_backend_->stream_executor(build_options.device_ordinal()));

  std::vector<std::unique_ptr<HloModuleConfig>> module_configs;
  module_configs.push_back(std::move(module_config));
  // BuildAotResults uses the executors length to determine the number of
  // cores per module, but otherwise only uses the first executor.
  std::vector<se::StreamExecutor*> executors(build_options.num_partitions(),
                                             executor);

  return BuildAotResults(
      /*module_protos=*/{&computation.proto()}, std::move(module_configs),
      execute_backend_.get(), {executors},
      Compiler::CompileOptions{build_options.device_allocator(),
                               build_options.compile_thread_pool()},
      build_options.run_backend_only());
}

StatusOr<int> LocalService::ReplicaNumberToDeviceOrdinal(int replica_number) {
  return backend().computation_placer()->DeviceId(
      replica_number, /*computation=*/0, options_.number_of_replicas(),
      /*computation_count=*/1);
}

StatusOr<const ShapedBuffer*> LocalService::GlobalDataToShapedBuffer(
    const GlobalDataHandle& data, int replica_number) {
  TF_ASSIGN_OR_RETURN(auto buffers, allocation_tracker_.Resolve(data));
  if (replica_number >= buffers.size()) {
    return InvalidArgument(
        "replica_number %d out of range; must be less than num_replicas = %u.",
        replica_number, buffers.size());
  }
  return buffers[replica_number];
}

StatusOr<GlobalDataHandle> LocalService::RegisterReplicatedBuffers(
    std::vector<ScopedShapedBuffer> replicated_buffers,
    const std::string& tag) {
  return allocation_tracker_.RegisterReplicatedBuffers(
      std::move(replicated_buffers), tag);
}

}  // namespace xla

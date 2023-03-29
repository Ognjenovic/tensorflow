/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include "llvm/Target/TargetMachine.h"
#include "tensorflow/compiler/xla/service/gpu/gpu_compiler.h"
#include "tensorflow/compiler/xla/service/gpu/gpu_device_info.h"
#include "tensorflow/compiler/xla/service/gpu/llvm_gpu_backend/gpu_backend_lib.h"
#if GOOGLE_CUDA
#include "tensorflow/compiler/xla/service/gpu/nvptx_compiler.h"
#endif
#include "tensorflow/compiler/xla/hlo/ir/hlo_module.h"
#include "tensorflow/compiler/xla/service/gpu/target_constants.h"
#include "tensorflow/compiler/xla/status.h"
#include "tensorflow/compiler/xla/stream_executor/cuda/cuda_platform_id.h"
#include "tensorflow/compiler/xla/tools/hlo_module_loader.h"
#include "tensorflow/tsl/platform/init_main.h"
#include "tensorflow/tsl/platform/logging.h"
#include "tensorflow/tsl/util/command_line_flags.h"

const char* const kUsage = R"(
This tool reads in an HloModule from a file, compiles it using the NVPTX
compiler and prints out the LLVM IR generated by the IR emitter.  The LLVM IR is
not optimized by the LLVM pass pipeline, so this tool can be used to unit test
the XLA GPU IR emitters.

Note that the LLVM IR does not contain the *full* module, but only parts that
will be code generated into PTX.  The NVPTX compiler also generates a
GpuExecutable on the side that is not printed.

When passed the parameter `--ptx`, the LLVM IR will be optimized and PTX
will be emitted and printed instead of the non-optimized LLVM.
By default SM 70 is targeted. But this can be changed with `--sm=SM`.)";

namespace {
xla::Status CompileAndPrintLlvmIr(const std::string& hlo_text,
                                  bool generate_ptx, int sm) {
  TF_ASSIGN_OR_RETURN(
      std::unique_ptr<xla::HloModule> hlo_module,
      xla::LoadModuleFromData(/*data=*/hlo_text, /*format=*/"hlo"));
  llvm::LLVMContext llvm_context;
  // For now we pretend we're compiling for V100.  This can be generalized
  // later.

  xla::gpu::GpuDeviceInfo gpu_device_info{};
  gpu_device_info.threads_per_block_limit = 1024;
  gpu_device_info.threads_per_warp = 32;
  gpu_device_info.shared_memory_per_block = 49152;
  gpu_device_info.core_count = 80;
  gpu_device_info.threads_per_core_limit = 2048;
  gpu_device_info.block_dim_limit_x = 2147483647;
  gpu_device_info.block_dim_limit_y = 65535;
  gpu_device_info.block_dim_limit_z = 65535;

  tensorflow::se::CudaComputeCapability cuda_compute_capability;
  cuda_compute_capability.major = sm / 10;
  cuda_compute_capability.minor = sm % 10;
  tensorflow::se::RocmComputeCapability rocm_compute_capability("gfx908");
  std::string target_triple = "nvptx64-nvidia-cuda";
  std::string datalayout = "nvptx64-nvidia-cuda";
  std::string platform_name = "CUDA";
  stream_executor::Platform::Id platform_id =
      stream_executor::cuda::kCudaPlatformId;
  TF_ASSIGN_OR_RETURN(std::unique_ptr<llvm::Module> llvm_module,
                      xla::gpu::CompileModuleToLlvmIr(
                          hlo_module.get(), &llvm_context,
                          /*target_triple=*/xla::gpu::nvptx::TargetTriple(),
                          /*data_layout=*/xla::gpu::nvptx::DataLayout(),
                          /*platform_name=*/platform_name,
                          /*platform_id=*/platform_id, gpu_device_info,
                          cuda_compute_capability, rocm_compute_capability,
                          /*pointer_size=*/8));

  if (!generate_ptx) {
    llvm_module->print(llvm::outs(), nullptr);
  } else {
#if GOOGLE_CUDA
    TF_ASSIGN_OR_RETURN(
        std::string ptx,
        xla::gpu::nvptx::CompileToPtx(
            llvm_module.get(), cuda_compute_capability, hlo_module->config()));
    std::cout << ptx << std::endl;
#else
    return {absl::StatusCode::kUnimplemented,
            "Feature not yet implemented in ROCm"};
#endif
  }
  return xla::OkStatus();
}

xla::Status CompileAndPrintLlvmIrFromFile(const std::string& file_name,
                                          bool ptx, int sm) {
  std::string full_text;
  TF_RETURN_IF_ERROR(
      tsl::ReadFileToString(tsl::Env::Default(), file_name, &full_text));

  std::vector<std::string> hlo_module_texts =
      absl::StrSplit(full_text, "// -----");
  for (const std::string& hlo_module_text : hlo_module_texts) {
    TF_RETURN_IF_ERROR(CompileAndPrintLlvmIr(hlo_module_text, ptx, sm));
  }

  return xla::OkStatus();
}
}  // namespace

int main(int argc, char** argv) {
  bool ptx = false;
  int sm = 70;
  std::vector<tsl::Flag> flag_list;
  xla::AppendDebugOptionsFlags(&flag_list);
  flag_list.emplace_back("ptx", &ptx,
                         "Print PTX instead of not optimized LLVM.");
  flag_list.emplace_back("sm", &sm,
                         "Specify the SM to target (useful only with --ptx).");
  // The usage string includes the message at the top of the file, the
  // DebugOptions flags and the flags defined above.
  const std::string kUsageString =
      absl::StrCat(kUsage, "\n\n", tsl::Flags::Usage(argv[0], flag_list));
  bool parse_ok = tsl::Flags::Parse(&argc, argv, flag_list);
  tsl::port::InitMain(kUsageString.c_str(), &argc, &argv);
  if (!parse_ok) {
    LOG(QFATAL) << kUsageString;
  }

  QCHECK(argc == 2) << "Must specify a single input file";
  TF_CHECK_OK(CompileAndPrintLlvmIrFromFile(argv[1], ptx, sm));

  return 0;
}

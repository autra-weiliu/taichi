// The AMDGPU backend
#pragma once

#include "taichi/codegen/codegen.h"
#include "taichi/codegen/llvm/codegen_llvm.h"

namespace taichi {
namespace lang {

class KernelCodeGenAMDGPU : public KernelCodeGen {
 public:
  KernelCodeGenAMDGPU(const CompileConfig &config,
                      const Kernel *kernel,
                      IRNode *ir,
                      TaichiLLVMContext &tlctx)
      : KernelCodeGen(config, kernel, ir, tlctx) {
  }

// TODO: Stop defining this macro guards in the headers
#ifdef TI_WITH_LLVM
  LLVMCompiledTask compile_task(
      const CompileConfig &config,
      std::unique_ptr<llvm::Module> &&module = nullptr,
      OffloadedStmt *stmt = nullptr) override;
#endif  // TI_WITH_LLVM
};

class AMDGPUModuleToFunctionConverter : public ModuleToFunctionConverter {
 public:
  explicit AMDGPUModuleToFunctionConverter(TaichiLLVMContext *tlctx,
                                           LlvmRuntimeExecutor *executor)
      : ModuleToFunctionConverter(tlctx, executor) {
  }
  using ModuleToFunctionConverter::convert;

  FunctionType convert(const std::string &kernel_name,
                       const std::vector<Callable::Parameter> &args,
                       LLVMCompiledKernel data) const override;
};

}  // namespace lang
}  // namespace taichi

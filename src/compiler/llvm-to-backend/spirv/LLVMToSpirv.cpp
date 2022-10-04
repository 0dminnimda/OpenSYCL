/*
 * This file is part of hipSYCL, a SYCL implementation based on CUDA/HIP
 *
 * Copyright (c) 2019-2022 Aksel Alpay
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "hipSYCL/compiler/llvm-to-backend/spirv/LLVMToSpirv.hpp"
#include "hipSYCL/compiler/llvm-to-backend/Utils.hpp"
#include "hipSYCL/compiler/sscp/IRConstantReplacer.hpp"
#include "hipSYCL/glue/llvm-sscp/s2_ir_constants.hpp"
#include <llvm/ADT/SmallVector.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Program.h>
#include <memory>
#include <cassert>
#include <system_error>
#include <vector>

#include <iostream>
namespace hipsycl {
namespace compiler {

LLVMToSpirvTranslator::LLVMToSpirvTranslator(const std::vector<std::string> &KN)
    : LLVMToBackendTranslator{sycl::sscp::backend::spirv, KN}, KernelNames{KN} {}


bool LLVMToSpirvTranslator::toBackendFlavor(llvm::Module &M) {
  M.setTargetTriple("spir64-unknown-unknown");
  M.setDataLayout(
      "e-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024");

  for(auto KernelName : KernelNames) {
    if(auto* F = M.getFunction(KernelName)) {
      F->setCallingConv(llvm::CallingConv::SPIR_KERNEL);
    }
  }

  constructPassBuilderAndMAM([&](auto& PB, auto& MAM){
    S2IRConstant::optimizeCodeAfterConstantModification(M, MAM);

    // Other transform code here

    llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
    MPM.run(M, MAM);
  });

  return true;
}


template<class F>
class AtScopeExit {
public:
  AtScopeExit(F&& f)
  : Handler(f) {}

  ~AtScopeExit() {Handler();}
private:
  std::function<void()> Handler;
};

bool LLVMToSpirvTranslator::translateToBackendFormat(llvm::Module &FlavoredModule, std::string &out) {

  auto InputFile = llvm::sys::fs::TempFile::create("hipsycl-sscp-spirv-%%%%%%.bc");
  
  llvm::SmallVector<char> OutputFilenameSmallVec;
  llvm::sys::fs::createTemporaryFile("hipsycl-sscp-spirv", "spv", OutputFilenameSmallVec);
  std::string OutputFilename;
  for(auto c : OutputFilenameSmallVec)
    OutputFilename += c;
  
  auto E = InputFile.takeError();
  if(E){
    this->registerError("Could not create temp file: "+InputFile->TmpName);
    return false;
  }

  AtScopeExit DestroyInputFile([&]() { auto Err = InputFile->discard(); });
  AtScopeExit DestroyOutputFile([&]() { llvm::sys::fs::remove(OutputFilename); });

  std::error_code EC;
  llvm::raw_fd_ostream InputStream{InputFile->FD, false};
  
  llvm::WriteBitcodeToFile(FlavoredModule, InputStream);
  InputStream.flush();

  std::string LLVMSpirVTranslator = "/usr/bin/llvm-spirv";
  int R = llvm::sys::ExecuteAndWait(
      LLVMSpirVTranslator, {LLVMSpirVTranslator, "-o=" + OutputFilename, InputFile->TmpName});
  if(R != 0) {
    this->registerError("llvm-spirv invocation failed with exit code " + std::to_string(R));
    return false;
  }
  
  auto ReadResult =
      llvm::MemoryBuffer::getFile(OutputFilename);
  
  if(auto Err = ReadResult.getError()) {
    this->registerError("Could not read result file"+Err.message());
    return false;
  }
  
  out = ReadResult->get()->getBuffer();

  return true;
}

std::unique_ptr<LLVMToBackendTranslator>
createLLVMToSpirvTranslator(const std::vector<std::string> &KernelNames) {
  return std::make_unique<LLVMToSpirvTranslator>(KernelNames);
}

}
}
//===-- VETargetMachine.cpp - Define TargetMachine for VE -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//
//===----------------------------------------------------------------------===//

#include "VETargetMachine.h"
#include "VE.h"
// #include "VETargetObjectFile.h"
#include "VETargetTransformInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Support/TargetRegistry.h"
using namespace llvm;

extern "C" void LLVMInitializeVETarget() {
  // Register the target.
  RegisterTargetMachine<VETargetMachine> X(getTheVETarget());
}

static std::string computeDataLayout(const Triple &T) {
  // Aurora VE is little endian
  std::string Ret = "e";

  // Use ELF mangling
  Ret += "-m:e";

  // Alignments for 64 bit integers.
  Ret += "-i64:64";

  // VE supports 32 bit and 64 bits integer on registers
  Ret += "-n32:64";

  // Stack alignment is 64 bits
  Ret += "-S64";

  // Vector alignments are 64 bits
  // Need to define all of them.  Otherwise, each alignment becomes
  // the size of each data by default.
  Ret += "-v64:64:64";          // for v2f32
  Ret += "-v128:64:64";
  Ret += "-v256:64:64";
  Ret += "-v512:64:64";
  Ret += "-v1024:64:64";
  Ret += "-v2048:64:64";
  Ret += "-v4096:64:64";
  Ret += "-v8192:64:64";
  Ret += "-v16384:64:64";       // for v256f64

  return Ret;
}

static Reloc::Model getEffectiveRelocModel(Optional<Reloc::Model> RM) {
  if (!RM.hasValue())
    return Reloc::Static;
  return *RM;
}

class VEELFTargetObjectFile : public TargetLoweringObjectFileELF {
  void Initialize(MCContext &Ctx, const TargetMachine &TM) override {
    TargetLoweringObjectFileELF::Initialize(Ctx, TM);
    InitializeELF(TM.Options.UseInitArray);
  }
};

static std::unique_ptr<TargetLoweringObjectFile> createTLOF()
{
    return std::make_unique<VEELFTargetObjectFile>();
}

/// Create an Aurora VE architecture model
VETargetMachine::VETargetMachine(
    const Target &T, const Triple &TT, StringRef CPU, StringRef FS,
    const TargetOptions &Options, Optional<Reloc::Model> RM,
    Optional<CodeModel::Model> CM, CodeGenOpt::Level OL, bool JIT)
    : LLVMTargetMachine(
          T, computeDataLayout(TT), TT, CPU, FS, Options,
          getEffectiveRelocModel(RM),
          getEffectiveCodeModel(CM, CodeModel::Small),
          OL),
      //TLOF(make_unique<TargetLoweringObjectFileELF>()),
      TLOF(createTLOF()),
      Subtarget(TT, CPU, FS, *this) {
  initAsmInfo();
}

VETargetMachine::~VETargetMachine() {}

TargetTransformInfo VETargetMachine::getTargetTransformInfo(const Function &F) {
  return TargetTransformInfo(VETTIImpl(this, F));
}

namespace {
/// VE Code Generator Pass Configuration Options.
class VEPassConfig : public TargetPassConfig {
public:
  VEPassConfig(VETargetMachine &TM, PassManagerBase &PM)
    : TargetPassConfig(TM, PM) {}

  VETargetMachine &getVETargetMachine() const {
    return getTM<VETargetMachine>();
  }

  void addIRPasses() override;
  bool addInstSelector() override;
  void addPreRegAlloc() override;
  void addPreEmitPass() override;
};
} // namespace

TargetPassConfig *VETargetMachine::createPassConfig(PassManagerBase &PM) {
  return new VEPassConfig(*this, PM);
}

void VEPassConfig::addIRPasses() {
  addPass(createAtomicExpandPass());
  TargetPassConfig::addIRPasses();
}

bool VEPassConfig::addInstSelector() {
  addPass(createVEISelDag(getVETargetMachine()));
  return false;
}

void VEPassConfig::addPreRegAlloc() {
  addPass(createVEPromoteToI1Pass());
}

void VEPassConfig::addPreEmitPass(){
  // LVLGen should be called after scheduling and register allocation
  addPass(createLVLGenPass());
#if 0
  addPass(createVEDelaySlotFillerPass());
#endif
}


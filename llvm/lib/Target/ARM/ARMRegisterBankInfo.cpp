//===- ARMRegisterBankInfo.cpp -----------------------------------*- C++ -*-==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
/// This file implements the targeting of the RegisterBankInfo class for ARM.
/// \todo This should be generated by TableGen.
//===----------------------------------------------------------------------===//

#include "ARMRegisterBankInfo.h"
#include "ARMInstrInfo.h" // For the register classes
#include "llvm/CodeGen/GlobalISel/RegisterBank.h"
#include "llvm/CodeGen/GlobalISel/RegisterBankInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Target/TargetRegisterInfo.h"

using namespace llvm;

#ifndef LLVM_BUILD_GLOBAL_ISEL
#error "You shouldn't build this"
#endif

// FIXME: TableGen this.
// If it grows too much and TableGen still isn't ready to do the job, extract it
// into an ARMGenRegisterBankInfo.def (similar to AArch64).
namespace llvm {
namespace ARM {
const uint32_t GPRCoverageData[] = {
    // Classes 0-31
    (1u << ARM::GPRRegClassID) | (1u << ARM::GPRwithAPSRRegClassID) |
        (1u << ARM::GPRnopcRegClassID) | (1u << ARM::rGPRRegClassID) |
        (1u << ARM::hGPRRegClassID) | (1u << ARM::tGPRRegClassID) |
        (1u << ARM::GPRnopc_and_hGPRRegClassID) |
        (1u << ARM::hGPR_and_rGPRRegClassID) | (1u << ARM::tcGPRRegClassID) |
        (1u << ARM::tGPR_and_tcGPRRegClassID) | (1u << ARM::GPRspRegClassID) |
        (1u << ARM::hGPR_and_tcGPRRegClassID),
    // Classes 32-63
    0,
    // Classes 64-96
    0,
    // FIXME: Some of the entries below this point can be safely removed once
    // this is tablegenerated. It's only needed because of the hardcoded
    // register class limit.
    // Classes 97-128
    0,
    // Classes 129-160
    0,
    // Classes 161-192
    0,
    // Classes 193-224
    0,
};

// FIXME: The 200 will be replaced by the number of register classes when this is
//        tablegenerated.
RegisterBank GPRRegBank(ARM::GPRRegBankID, "GPRB", 32, ARM::GPRCoverageData, 200);
RegisterBank *RegBanks[] = {&GPRRegBank};

RegisterBankInfo::PartialMapping GPRPartialMapping{0, 32, GPRRegBank};

RegisterBankInfo::ValueMapping ValueMappings[] = {
    {&GPRPartialMapping, 1}, {&GPRPartialMapping, 1}, {&GPRPartialMapping, 1}};
} // end namespace arm
} // end namespace llvm

ARMRegisterBankInfo::ARMRegisterBankInfo(const TargetRegisterInfo &TRI)
    : RegisterBankInfo(ARM::RegBanks, ARM::NumRegisterBanks) {
  static bool AlreadyInit = false;
  // We have only one set of register banks, whatever the subtarget
  // is. Therefore, the initialization of the RegBanks table should be
  // done only once. Indeed the table of all register banks
  // (ARM::RegBanks) is unique in the compiler. At some point, it
  // will get tablegen'ed and the whole constructor becomes empty.
  if (AlreadyInit)
    return;
  AlreadyInit = true;

  const RegisterBank &RBGPR = getRegBank(ARM::GPRRegBankID);
  (void)RBGPR;
  assert(&ARM::GPRRegBank == &RBGPR && "The order in RegBanks is messed up");

  // Initialize the GPR bank.
  assert(RBGPR.covers(*TRI.getRegClass(ARM::GPRRegClassID)) &&
         "Subclass not added?");
  assert(RBGPR.covers(*TRI.getRegClass(ARM::GPRwithAPSRRegClassID)) &&
         "Subclass not added?");
  assert(RBGPR.covers(*TRI.getRegClass(ARM::GPRnopcRegClassID)) &&
         "Subclass not added?");
  assert(RBGPR.covers(*TRI.getRegClass(ARM::rGPRRegClassID)) &&
         "Subclass not added?");
  assert(RBGPR.covers(*TRI.getRegClass(ARM::tGPRRegClassID)) &&
         "Subclass not added?");
  assert(RBGPR.covers(*TRI.getRegClass(ARM::tcGPRRegClassID)) &&
         "Subclass not added?");
  assert(RBGPR.covers(*TRI.getRegClass(ARM::tGPR_and_tcGPRRegClassID)) &&
         "Subclass not added?");
  assert(RBGPR.getSize() == 32 && "GPRs should hold up to 32-bit");
}

const RegisterBank &ARMRegisterBankInfo::getRegBankFromRegClass(
    const TargetRegisterClass &RC) const {
  using namespace ARM;

  switch (RC.getID()) {
  case GPRRegClassID:
  case GPRnopcRegClassID:
  case tGPR_and_tcGPRRegClassID:
    return getRegBank(ARM::GPRRegBankID);
  default:
    llvm_unreachable("Unsupported register kind");
  }

  llvm_unreachable("Switch should handle all register classes");
}

RegisterBankInfo::InstructionMapping
ARMRegisterBankInfo::getInstrMapping(const MachineInstr &MI) const {
  auto Opc = MI.getOpcode();

  // Try the default logic for non-generic instructions that are either copies
  // or already have some operands assigned to banks.
  if (!isPreISelGenericOpcode(Opc)) {
    InstructionMapping Mapping = getInstrMappingImpl(MI);
    if (Mapping.isValid())
      return Mapping;
  }

  using namespace TargetOpcode;

  unsigned NumOperands = MI.getNumOperands();
  const ValueMapping *OperandsMapping = &ARM::ValueMappings[0];

  switch (Opc) {
  case G_ADD:
  case G_LOAD:
  case G_SEXT:
  case G_ZEXT:
    // FIXME: We're abusing the fact that everything lives in a GPR for now; in
    // the real world we would use different mappings.
    OperandsMapping = &ARM::ValueMappings[0];
    break;
  case G_FRAME_INDEX:
    OperandsMapping = getOperandsMapping({&ARM::ValueMappings[0], nullptr});
    break;
  default:
    return InstructionMapping{};
  }

  return InstructionMapping{DefaultMappingID, /*Cost=*/1, OperandsMapping,
                            NumOperands};
}

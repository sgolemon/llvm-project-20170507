//===- CodeGenPrepare.cpp - Prepare a function for code generation --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass munges the code in the input function to better prepare it for
// SelectionDAG-based code generation. This works around limitations in it's
// basic-block-at-a-time approach. It should eventually be removed.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "codegenprepare"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/InlineAsm.h"
#include "llvm/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/Target/TargetAsmInfo.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/GetElementPtrTypeIterator.h"
#include "llvm/Support/PatternMatch.h"
using namespace llvm;
using namespace llvm::PatternMatch;

namespace {
  class VISIBILITY_HIDDEN CodeGenPrepare : public FunctionPass {
    /// TLI - Keep a pointer of a TargetLowering to consult for determining
    /// transformation profitability.
    const TargetLowering *TLI;
  public:
    static char ID; // Pass identification, replacement for typeid
    explicit CodeGenPrepare(const TargetLowering *tli = 0)
      : FunctionPass(&ID), TLI(tli) {}
    bool runOnFunction(Function &F);

  private:
    bool EliminateMostlyEmptyBlocks(Function &F);
    bool CanMergeBlocks(const BasicBlock *BB, const BasicBlock *DestBB) const;
    void EliminateMostlyEmptyBlock(BasicBlock *BB);
    bool OptimizeBlock(BasicBlock &BB);
    bool OptimizeMemoryInst(Instruction *I, Value *Addr, const Type *AccessTy,
                            DenseMap<Value*,Value*> &SunkAddrs);
    bool OptimizeInlineAsmInst(Instruction *I, CallSite CS,
                               DenseMap<Value*,Value*> &SunkAddrs);
    bool OptimizeExtUses(Instruction *I);
  };
}

char CodeGenPrepare::ID = 0;
static RegisterPass<CodeGenPrepare> X("codegenprepare",
                                      "Optimize for code generation");

FunctionPass *llvm::createCodeGenPreparePass(const TargetLowering *TLI) {
  return new CodeGenPrepare(TLI);
}


bool CodeGenPrepare::runOnFunction(Function &F) {
  bool EverMadeChange = false;

  // First pass, eliminate blocks that contain only PHI nodes and an
  // unconditional branch.
  EverMadeChange |= EliminateMostlyEmptyBlocks(F);

  bool MadeChange = true;
  while (MadeChange) {
    MadeChange = false;
    for (Function::iterator BB = F.begin(), E = F.end(); BB != E; ++BB)
      MadeChange |= OptimizeBlock(*BB);
    EverMadeChange |= MadeChange;
  }
  return EverMadeChange;
}

/// EliminateMostlyEmptyBlocks - eliminate blocks that contain only PHI nodes
/// and an unconditional branch.  Passes before isel (e.g. LSR/loopsimplify)
/// often split edges in ways that are non-optimal for isel.  Start by
/// eliminating these blocks so we can split them the way we want them.
bool CodeGenPrepare::EliminateMostlyEmptyBlocks(Function &F) {
  bool MadeChange = false;
  // Note that this intentionally skips the entry block.
  for (Function::iterator I = ++F.begin(), E = F.end(); I != E; ) {
    BasicBlock *BB = I++;

    // If this block doesn't end with an uncond branch, ignore it.
    BranchInst *BI = dyn_cast<BranchInst>(BB->getTerminator());
    if (!BI || !BI->isUnconditional())
      continue;

    // If the instruction before the branch isn't a phi node, then other stuff
    // is happening here.
    BasicBlock::iterator BBI = BI;
    if (BBI != BB->begin()) {
      --BBI;
      if (!isa<PHINode>(BBI)) continue;
    }

    // Do not break infinite loops.
    BasicBlock *DestBB = BI->getSuccessor(0);
    if (DestBB == BB)
      continue;

    if (!CanMergeBlocks(BB, DestBB))
      continue;

    EliminateMostlyEmptyBlock(BB);
    MadeChange = true;
  }
  return MadeChange;
}

/// CanMergeBlocks - Return true if we can merge BB into DestBB if there is a
/// single uncond branch between them, and BB contains no other non-phi
/// instructions.
bool CodeGenPrepare::CanMergeBlocks(const BasicBlock *BB,
                                    const BasicBlock *DestBB) const {
  // We only want to eliminate blocks whose phi nodes are used by phi nodes in
  // the successor.  If there are more complex condition (e.g. preheaders),
  // don't mess around with them.
  BasicBlock::const_iterator BBI = BB->begin();
  while (const PHINode *PN = dyn_cast<PHINode>(BBI++)) {
    for (Value::use_const_iterator UI = PN->use_begin(), E = PN->use_end();
         UI != E; ++UI) {
      const Instruction *User = cast<Instruction>(*UI);
      if (User->getParent() != DestBB || !isa<PHINode>(User))
        return false;
      // If User is inside DestBB block and it is a PHINode then check
      // incoming value. If incoming value is not from BB then this is
      // a complex condition (e.g. preheaders) we want to avoid here.
      if (User->getParent() == DestBB) {
        if (const PHINode *UPN = dyn_cast<PHINode>(User))
          for (unsigned I = 0, E = UPN->getNumIncomingValues(); I != E; ++I) {
            Instruction *Insn = dyn_cast<Instruction>(UPN->getIncomingValue(I));
            if (Insn && Insn->getParent() == BB &&
                Insn->getParent() != UPN->getIncomingBlock(I))
              return false;
          }
      }
    }
  }

  // If BB and DestBB contain any common predecessors, then the phi nodes in BB
  // and DestBB may have conflicting incoming values for the block.  If so, we
  // can't merge the block.
  const PHINode *DestBBPN = dyn_cast<PHINode>(DestBB->begin());
  if (!DestBBPN) return true;  // no conflict.

  // Collect the preds of BB.
  SmallPtrSet<const BasicBlock*, 16> BBPreds;
  if (const PHINode *BBPN = dyn_cast<PHINode>(BB->begin())) {
    // It is faster to get preds from a PHI than with pred_iterator.
    for (unsigned i = 0, e = BBPN->getNumIncomingValues(); i != e; ++i)
      BBPreds.insert(BBPN->getIncomingBlock(i));
  } else {
    BBPreds.insert(pred_begin(BB), pred_end(BB));
  }

  // Walk the preds of DestBB.
  for (unsigned i = 0, e = DestBBPN->getNumIncomingValues(); i != e; ++i) {
    BasicBlock *Pred = DestBBPN->getIncomingBlock(i);
    if (BBPreds.count(Pred)) {   // Common predecessor?
      BBI = DestBB->begin();
      while (const PHINode *PN = dyn_cast<PHINode>(BBI++)) {
        const Value *V1 = PN->getIncomingValueForBlock(Pred);
        const Value *V2 = PN->getIncomingValueForBlock(BB);

        // If V2 is a phi node in BB, look up what the mapped value will be.
        if (const PHINode *V2PN = dyn_cast<PHINode>(V2))
          if (V2PN->getParent() == BB)
            V2 = V2PN->getIncomingValueForBlock(Pred);

        // If there is a conflict, bail out.
        if (V1 != V2) return false;
      }
    }
  }

  return true;
}


/// EliminateMostlyEmptyBlock - Eliminate a basic block that have only phi's and
/// an unconditional branch in it.
void CodeGenPrepare::EliminateMostlyEmptyBlock(BasicBlock *BB) {
  BranchInst *BI = cast<BranchInst>(BB->getTerminator());
  BasicBlock *DestBB = BI->getSuccessor(0);

  DOUT << "MERGING MOSTLY EMPTY BLOCKS - BEFORE:\n" << *BB << *DestBB;

  // If the destination block has a single pred, then this is a trivial edge,
  // just collapse it.
  if (BasicBlock *SinglePred = DestBB->getSinglePredecessor()) {
    // Remember if SinglePred was the entry block of the function.  If so, we
    // will need to move BB back to the entry position.
    bool isEntry = SinglePred == &SinglePred->getParent()->getEntryBlock();
    MergeBasicBlockIntoOnlyPred(DestBB);

    if (isEntry && BB != &BB->getParent()->getEntryBlock())
      BB->moveBefore(&BB->getParent()->getEntryBlock());
    
    DOUT << "AFTER:\n" << *DestBB << "\n\n\n";
    return;
  }

  // Otherwise, we have multiple predecessors of BB.  Update the PHIs in DestBB
  // to handle the new incoming edges it is about to have.
  PHINode *PN;
  for (BasicBlock::iterator BBI = DestBB->begin();
       (PN = dyn_cast<PHINode>(BBI)); ++BBI) {
    // Remove the incoming value for BB, and remember it.
    Value *InVal = PN->removeIncomingValue(BB, false);

    // Two options: either the InVal is a phi node defined in BB or it is some
    // value that dominates BB.
    PHINode *InValPhi = dyn_cast<PHINode>(InVal);
    if (InValPhi && InValPhi->getParent() == BB) {
      // Add all of the input values of the input PHI as inputs of this phi.
      for (unsigned i = 0, e = InValPhi->getNumIncomingValues(); i != e; ++i)
        PN->addIncoming(InValPhi->getIncomingValue(i),
                        InValPhi->getIncomingBlock(i));
    } else {
      // Otherwise, add one instance of the dominating value for each edge that
      // we will be adding.
      if (PHINode *BBPN = dyn_cast<PHINode>(BB->begin())) {
        for (unsigned i = 0, e = BBPN->getNumIncomingValues(); i != e; ++i)
          PN->addIncoming(InVal, BBPN->getIncomingBlock(i));
      } else {
        for (pred_iterator PI = pred_begin(BB), E = pred_end(BB); PI != E; ++PI)
          PN->addIncoming(InVal, *PI);
      }
    }
  }

  // The PHIs are now updated, change everything that refers to BB to use
  // DestBB and remove BB.
  BB->replaceAllUsesWith(DestBB);
  BB->eraseFromParent();

  DOUT << "AFTER:\n" << *DestBB << "\n\n\n";
}


/// SplitEdgeNicely - Split the critical edge from TI to its specified
/// successor if it will improve codegen.  We only do this if the successor has
/// phi nodes (otherwise critical edges are ok).  If there is already another
/// predecessor of the succ that is empty (and thus has no phi nodes), use it
/// instead of introducing a new block.
static void SplitEdgeNicely(TerminatorInst *TI, unsigned SuccNum, Pass *P) {
  BasicBlock *TIBB = TI->getParent();
  BasicBlock *Dest = TI->getSuccessor(SuccNum);
  assert(isa<PHINode>(Dest->begin()) &&
         "This should only be called if Dest has a PHI!");

  // As a hack, never split backedges of loops.  Even though the copy for any
  // PHIs inserted on the backedge would be dead for exits from the loop, we
  // assume that the cost of *splitting* the backedge would be too high.
  if (Dest == TIBB)
    return;

  /// TIPHIValues - This array is lazily computed to determine the values of
  /// PHIs in Dest that TI would provide.
  SmallVector<Value*, 32> TIPHIValues;

  // Check to see if Dest has any blocks that can be used as a split edge for
  // this terminator.
  for (pred_iterator PI = pred_begin(Dest), E = pred_end(Dest); PI != E; ++PI) {
    BasicBlock *Pred = *PI;
    // To be usable, the pred has to end with an uncond branch to the dest.
    BranchInst *PredBr = dyn_cast<BranchInst>(Pred->getTerminator());
    if (!PredBr || !PredBr->isUnconditional() ||
        // Must be empty other than the branch.
        &Pred->front() != PredBr ||
        // Cannot be the entry block; its label does not get emitted.
        Pred == &(Dest->getParent()->getEntryBlock()))
      continue;

    // Finally, since we know that Dest has phi nodes in it, we have to make
    // sure that jumping to Pred will have the same affect as going to Dest in
    // terms of PHI values.
    PHINode *PN;
    unsigned PHINo = 0;
    bool FoundMatch = true;
    for (BasicBlock::iterator I = Dest->begin();
         (PN = dyn_cast<PHINode>(I)); ++I, ++PHINo) {
      if (PHINo == TIPHIValues.size())
        TIPHIValues.push_back(PN->getIncomingValueForBlock(TIBB));

      // If the PHI entry doesn't work, we can't use this pred.
      if (TIPHIValues[PHINo] != PN->getIncomingValueForBlock(Pred)) {
        FoundMatch = false;
        break;
      }
    }

    // If we found a workable predecessor, change TI to branch to Succ.
    if (FoundMatch) {
      Dest->removePredecessor(TIBB);
      TI->setSuccessor(SuccNum, Pred);
      return;
    }
  }

  SplitCriticalEdge(TI, SuccNum, P, true);
}

/// OptimizeNoopCopyExpression - If the specified cast instruction is a noop
/// copy (e.g. it's casting from one pointer type to another, int->uint, or
/// int->sbyte on PPC), sink it into user blocks to reduce the number of virtual
/// registers that must be created and coalesced.
///
/// Return true if any changes are made.
///
static bool OptimizeNoopCopyExpression(CastInst *CI, const TargetLowering &TLI){
  // If this is a noop copy,
  MVT SrcVT = TLI.getValueType(CI->getOperand(0)->getType());
  MVT DstVT = TLI.getValueType(CI->getType());

  // This is an fp<->int conversion?
  if (SrcVT.isInteger() != DstVT.isInteger())
    return false;

  // If this is an extension, it will be a zero or sign extension, which
  // isn't a noop.
  if (SrcVT.bitsLT(DstVT)) return false;

  // If these values will be promoted, find out what they will be promoted
  // to.  This helps us consider truncates on PPC as noop copies when they
  // are.
  if (TLI.getTypeAction(SrcVT) == TargetLowering::Promote)
    SrcVT = TLI.getTypeToTransformTo(SrcVT);
  if (TLI.getTypeAction(DstVT) == TargetLowering::Promote)
    DstVT = TLI.getTypeToTransformTo(DstVT);

  // If, after promotion, these are the same types, this is a noop copy.
  if (SrcVT != DstVT)
    return false;

  BasicBlock *DefBB = CI->getParent();

  /// InsertedCasts - Only insert a cast in each block once.
  DenseMap<BasicBlock*, CastInst*> InsertedCasts;

  bool MadeChange = false;
  for (Value::use_iterator UI = CI->use_begin(), E = CI->use_end();
       UI != E; ) {
    Use &TheUse = UI.getUse();
    Instruction *User = cast<Instruction>(*UI);

    // Figure out which BB this cast is used in.  For PHI's this is the
    // appropriate predecessor block.
    BasicBlock *UserBB = User->getParent();
    if (PHINode *PN = dyn_cast<PHINode>(User)) {
      unsigned OpVal = UI.getOperandNo()/2;
      UserBB = PN->getIncomingBlock(OpVal);
    }

    // Preincrement use iterator so we don't invalidate it.
    ++UI;

    // If this user is in the same block as the cast, don't change the cast.
    if (UserBB == DefBB) continue;

    // If we have already inserted a cast into this block, use it.
    CastInst *&InsertedCast = InsertedCasts[UserBB];

    if (!InsertedCast) {
      BasicBlock::iterator InsertPt = UserBB->getFirstNonPHI();

      InsertedCast =
        CastInst::Create(CI->getOpcode(), CI->getOperand(0), CI->getType(), "",
                         InsertPt);
      MadeChange = true;
    }

    // Replace a use of the cast with a use of the new cast.
    TheUse = InsertedCast;
  }

  // If we removed all uses, nuke the cast.
  if (CI->use_empty()) {
    CI->eraseFromParent();
    MadeChange = true;
  }

  return MadeChange;
}

/// OptimizeCmpExpression - sink the given CmpInst into user blocks to reduce
/// the number of virtual registers that must be created and coalesced.  This is
/// a clear win except on targets with multiple condition code registers
///  (PowerPC), where it might lose; some adjustment may be wanted there.
///
/// Return true if any changes are made.
static bool OptimizeCmpExpression(CmpInst *CI) {
  BasicBlock *DefBB = CI->getParent();

  /// InsertedCmp - Only insert a cmp in each block once.
  DenseMap<BasicBlock*, CmpInst*> InsertedCmps;

  bool MadeChange = false;
  for (Value::use_iterator UI = CI->use_begin(), E = CI->use_end();
       UI != E; ) {
    Use &TheUse = UI.getUse();
    Instruction *User = cast<Instruction>(*UI);

    // Preincrement use iterator so we don't invalidate it.
    ++UI;

    // Don't bother for PHI nodes.
    if (isa<PHINode>(User))
      continue;

    // Figure out which BB this cmp is used in.
    BasicBlock *UserBB = User->getParent();

    // If this user is in the same block as the cmp, don't change the cmp.
    if (UserBB == DefBB) continue;

    // If we have already inserted a cmp into this block, use it.
    CmpInst *&InsertedCmp = InsertedCmps[UserBB];

    if (!InsertedCmp) {
      BasicBlock::iterator InsertPt = UserBB->getFirstNonPHI();

      InsertedCmp =
        CmpInst::Create(CI->getOpcode(), CI->getPredicate(), CI->getOperand(0),
                        CI->getOperand(1), "", InsertPt);
      MadeChange = true;
    }

    // Replace a use of the cmp with a use of the new cmp.
    TheUse = InsertedCmp;
  }

  // If we removed all uses, nuke the cmp.
  if (CI->use_empty())
    CI->eraseFromParent();

  return MadeChange;
}

//===----------------------------------------------------------------------===//
// Addressing Mode Analysis and Optimization
//===----------------------------------------------------------------------===//

namespace {
  /// ExtAddrMode - This is an extended version of TargetLowering::AddrMode
  /// which holds actual Value*'s for register values.
  struct ExtAddrMode : public TargetLowering::AddrMode {
    Value *BaseReg;
    Value *ScaledReg;
    ExtAddrMode() : BaseReg(0), ScaledReg(0) {}
    void print(OStream &OS) const;
    void dump() const {
      print(cerr);
      cerr << '\n';
    }
  };
} // end anonymous namespace

static inline OStream &operator<<(OStream &OS, const ExtAddrMode &AM) {
  AM.print(OS);
  return OS;
}

void ExtAddrMode::print(OStream &OS) const {
  bool NeedPlus = false;
  OS << "[";
  if (BaseGV)
    OS << (NeedPlus ? " + " : "")
       << "GV:%" << BaseGV->getName(), NeedPlus = true;

  if (BaseOffs)
    OS << (NeedPlus ? " + " : "") << BaseOffs, NeedPlus = true;

  if (BaseReg)
    OS << (NeedPlus ? " + " : "")
       << "Base:%" << BaseReg->getName(), NeedPlus = true;
  if (Scale)
    OS << (NeedPlus ? " + " : "")
       << Scale << "*%" << ScaledReg->getName(), NeedPlus = true;

  OS << ']';
}

namespace {
/// AddressingModeMatcher - This class exposes a single public method, which is
/// used to construct a "maximal munch" of the addressing mode for the target
/// specified by TLI for an access to "V" with an access type of AccessTy.  This
/// returns the addressing mode that is actually matched by value, but also
/// returns the list of instructions involved in that addressing computation in
/// AddrModeInsts.
class AddressingModeMatcher {
  SmallVectorImpl<Instruction*> &AddrModeInsts;
  const TargetLowering &TLI;

  /// AccessTy/MemoryInst - This is the type for the access (e.g. double) and
  /// the memory instruction that we're computing this address for.
  const Type *AccessTy;
  Instruction *MemoryInst;
  
  /// AddrMode - This is the addressing mode that we're building up.  This is
  /// part of the return value of this addressing mode matching stuff.
  ExtAddrMode &AddrMode;
  
  /// IgnoreProfitability - This is set to true when we should not do
  /// profitability checks.  When true, IsProfitableToFoldIntoAddressingMode
  /// always returns true.
  bool IgnoreProfitability;
  
  AddressingModeMatcher(SmallVectorImpl<Instruction*> &AMI,
                        const TargetLowering &T, const Type *AT,
                        Instruction *MI, ExtAddrMode &AM)
    : AddrModeInsts(AMI), TLI(T), AccessTy(AT), MemoryInst(MI), AddrMode(AM) {
    IgnoreProfitability = false;
  }
public:
  
  /// Match - Find the maximal addressing mode that a load/store of V can fold,
  /// give an access type of AccessTy.  This returns a list of involved
  /// instructions in AddrModeInsts.
  static ExtAddrMode Match(Value *V, const Type *AccessTy,
                           Instruction *MemoryInst,
                           SmallVectorImpl<Instruction*> &AddrModeInsts,
                           const TargetLowering &TLI) {
    ExtAddrMode Result;

    bool Success = 
      AddressingModeMatcher(AddrModeInsts, TLI, AccessTy,
                            MemoryInst, Result).MatchAddr(V, 0);
    Success = Success; assert(Success && "Couldn't select *anything*?");
    return Result;
  }
private:
  bool MatchScaledValue(Value *ScaleReg, int64_t Scale, unsigned Depth);
  bool MatchAddr(Value *V, unsigned Depth);
  bool MatchOperationAddr(User *Operation, unsigned Opcode, unsigned Depth);
  bool IsProfitableToFoldIntoAddressingMode(Instruction *I,
                                            ExtAddrMode &AMBefore,
                                            ExtAddrMode &AMAfter);
  bool ValueAlreadyLiveAtInst(Value *Val, Value *KnownLive1, Value *KnownLive2);
};
} // end anonymous namespace

/// MatchScaledValue - Try adding ScaleReg*Scale to the current addressing mode.
/// Return true and update AddrMode if this addr mode is legal for the target,
/// false if not.
bool AddressingModeMatcher::MatchScaledValue(Value *ScaleReg, int64_t Scale,
                                             unsigned Depth) {
  // If Scale is 1, then this is the same as adding ScaleReg to the addressing
  // mode.  Just process that directly.
  if (Scale == 1)
    return MatchAddr(ScaleReg, Depth);
  
  // If the scale is 0, it takes nothing to add this.
  if (Scale == 0)
    return true;
  
  // If we already have a scale of this value, we can add to it, otherwise, we
  // need an available scale field.
  if (AddrMode.Scale != 0 && AddrMode.ScaledReg != ScaleReg)
    return false;

  ExtAddrMode TestAddrMode = AddrMode;

  // Add scale to turn X*4+X*3 -> X*7.  This could also do things like
  // [A+B + A*7] -> [B+A*8].
  TestAddrMode.Scale += Scale;
  TestAddrMode.ScaledReg = ScaleReg;

  // If the new address isn't legal, bail out.
  if (!TLI.isLegalAddressingMode(TestAddrMode, AccessTy))
    return false;

  // It was legal, so commit it.
  AddrMode = TestAddrMode;
  
  // Okay, we decided that we can add ScaleReg+Scale to AddrMode.  Check now
  // to see if ScaleReg is actually X+C.  If so, we can turn this into adding
  // X*Scale + C*Scale to addr mode.
  ConstantInt *CI; Value *AddLHS;
  if (match(ScaleReg, m_Add(m_Value(AddLHS), m_ConstantInt(CI)))) {
    TestAddrMode.ScaledReg = AddLHS;
    TestAddrMode.BaseOffs += CI->getSExtValue()*TestAddrMode.Scale;
      
    // If this addressing mode is legal, commit it and remember that we folded
    // this instruction.
    if (TLI.isLegalAddressingMode(TestAddrMode, AccessTy)) {
      AddrModeInsts.push_back(cast<Instruction>(ScaleReg));
      AddrMode = TestAddrMode;
      return true;
    }
  }

  // Otherwise, not (x+c)*scale, just return what we have.
  return true;
}

/// MightBeFoldableInst - This is a little filter, which returns true if an
/// addressing computation involving I might be folded into a load/store
/// accessing it.  This doesn't need to be perfect, but needs to accept at least
/// the set of instructions that MatchOperationAddr can.
static bool MightBeFoldableInst(Instruction *I) {
  switch (I->getOpcode()) {
  case Instruction::BitCast:
    // Don't touch identity bitcasts.
    if (I->getType() == I->getOperand(0)->getType())
      return false;
    return isa<PointerType>(I->getType()) || isa<IntegerType>(I->getType());
  case Instruction::PtrToInt:
    // PtrToInt is always a noop, as we know that the int type is pointer sized.
    return true;
  case Instruction::IntToPtr:
    // We know the input is intptr_t, so this is foldable.
    return true;
  case Instruction::Add:
    return true;
  case Instruction::Mul:
  case Instruction::Shl:
    // Can only handle X*C and X << C.
    return isa<ConstantInt>(I->getOperand(1));
  case Instruction::GetElementPtr:
    return true;
  default:
    return false;
  }
}


/// MatchOperationAddr - Given an instruction or constant expr, see if we can
/// fold the operation into the addressing mode.  If so, update the addressing
/// mode and return true, otherwise return false without modifying AddrMode.
bool AddressingModeMatcher::MatchOperationAddr(User *AddrInst, unsigned Opcode,
                                               unsigned Depth) {
  // Avoid exponential behavior on extremely deep expression trees.
  if (Depth >= 5) return false;
  
  switch (Opcode) {
  case Instruction::PtrToInt:
    // PtrToInt is always a noop, as we know that the int type is pointer sized.
    return MatchAddr(AddrInst->getOperand(0), Depth);
  case Instruction::IntToPtr:
    // This inttoptr is a no-op if the integer type is pointer sized.
    if (TLI.getValueType(AddrInst->getOperand(0)->getType()) ==
        TLI.getPointerTy())
      return MatchAddr(AddrInst->getOperand(0), Depth);
    return false;
  case Instruction::BitCast:
    // BitCast is always a noop, and we can handle it as long as it is
    // int->int or pointer->pointer (we don't want int<->fp or something).
    if ((isa<PointerType>(AddrInst->getOperand(0)->getType()) ||
         isa<IntegerType>(AddrInst->getOperand(0)->getType())) &&
        // Don't touch identity bitcasts.  These were probably put here by LSR,
        // and we don't want to mess around with them.  Assume it knows what it
        // is doing.
        AddrInst->getOperand(0)->getType() != AddrInst->getType())
      return MatchAddr(AddrInst->getOperand(0), Depth);
    return false;
  case Instruction::Add: {
    // Check to see if we can merge in the RHS then the LHS.  If so, we win.
    ExtAddrMode BackupAddrMode = AddrMode;
    unsigned OldSize = AddrModeInsts.size();
    if (MatchAddr(AddrInst->getOperand(1), Depth+1) &&
        MatchAddr(AddrInst->getOperand(0), Depth+1))
      return true;
    
    // Restore the old addr mode info.
    AddrMode = BackupAddrMode;
    AddrModeInsts.resize(OldSize);
    
    // Otherwise this was over-aggressive.  Try merging in the LHS then the RHS.
    if (MatchAddr(AddrInst->getOperand(0), Depth+1) &&
        MatchAddr(AddrInst->getOperand(1), Depth+1))
      return true;
    
    // Otherwise we definitely can't merge the ADD in.
    AddrMode = BackupAddrMode;
    AddrModeInsts.resize(OldSize);
    break;
  }
  //case Instruction::Or:
  // TODO: We can handle "Or Val, Imm" iff this OR is equivalent to an ADD.
  //break;
  case Instruction::Mul:
  case Instruction::Shl: {
    // Can only handle X*C and X << C.
    ConstantInt *RHS = dyn_cast<ConstantInt>(AddrInst->getOperand(1));
    if (!RHS) return false;
    int64_t Scale = RHS->getSExtValue();
    if (Opcode == Instruction::Shl)
      Scale = 1 << Scale;
    
    return MatchScaledValue(AddrInst->getOperand(0), Scale, Depth);
  }
  case Instruction::GetElementPtr: {
    // Scan the GEP.  We check it if it contains constant offsets and at most
    // one variable offset.
    int VariableOperand = -1;
    unsigned VariableScale = 0;
    
    int64_t ConstantOffset = 0;
    const TargetData *TD = TLI.getTargetData();
    gep_type_iterator GTI = gep_type_begin(AddrInst);
    for (unsigned i = 1, e = AddrInst->getNumOperands(); i != e; ++i, ++GTI) {
      if (const StructType *STy = dyn_cast<StructType>(*GTI)) {
        const StructLayout *SL = TD->getStructLayout(STy);
        unsigned Idx =
          cast<ConstantInt>(AddrInst->getOperand(i))->getZExtValue();
        ConstantOffset += SL->getElementOffset(Idx);
      } else {
        uint64_t TypeSize = TD->getABITypeSize(GTI.getIndexedType());
        if (ConstantInt *CI = dyn_cast<ConstantInt>(AddrInst->getOperand(i))) {
          ConstantOffset += CI->getSExtValue()*TypeSize;
        } else if (TypeSize) {  // Scales of zero don't do anything.
          // We only allow one variable index at the moment.
          if (VariableOperand != -1)
            return false;
          
          // Remember the variable index.
          VariableOperand = i;
          VariableScale = TypeSize;
        }
      }
    }
    
    // A common case is for the GEP to only do a constant offset.  In this case,
    // just add it to the disp field and check validity.
    if (VariableOperand == -1) {
      AddrMode.BaseOffs += ConstantOffset;
      if (ConstantOffset == 0 || TLI.isLegalAddressingMode(AddrMode, AccessTy)){
        // Check to see if we can fold the base pointer in too.
        if (MatchAddr(AddrInst->getOperand(0), Depth+1))
          return true;
      }
      AddrMode.BaseOffs -= ConstantOffset;
      return false;
    }

    // Save the valid addressing mode in case we can't match.
    ExtAddrMode BackupAddrMode = AddrMode;
    
    // Check that this has no base reg yet.  If so, we won't have a place to
    // put the base of the GEP (assuming it is not a null ptr).
    bool SetBaseReg = true;
    if (isa<ConstantPointerNull>(AddrInst->getOperand(0)))
      SetBaseReg = false;   // null pointer base doesn't need representation.
    else if (AddrMode.HasBaseReg)
      return false;  // Base register already specified, can't match GEP.
    else {
      // Otherwise, we'll use the GEP base as the BaseReg.
      AddrMode.HasBaseReg = true;
      AddrMode.BaseReg = AddrInst->getOperand(0);
    }
    
    // See if the scale and offset amount is valid for this target.
    AddrMode.BaseOffs += ConstantOffset;
    
    if (!MatchScaledValue(AddrInst->getOperand(VariableOperand), VariableScale,
                          Depth)) {
      AddrMode = BackupAddrMode;
      return false;
    }
    
    // If we have a null as the base of the GEP, folding in the constant offset
    // plus variable scale is all we can do.
    if (!SetBaseReg) return true;
      
    // If this match succeeded, we know that we can form an address with the
    // GepBase as the basereg.  Match the base pointer of the GEP more
    // aggressively by zeroing out BaseReg and rematching.  If the base is
    // (for example) another GEP, this allows merging in that other GEP into
    // the addressing mode we're forming.
    AddrMode.HasBaseReg = false;
    AddrMode.BaseReg = 0;
    bool Success = MatchAddr(AddrInst->getOperand(0), Depth+1);
    assert(Success && "MatchAddr should be able to fill in BaseReg!");
    Success=Success;
    return true;
  }
  }
  return false;
}

/// MatchAddr - If we can, try to add the value of 'Addr' into the current
/// addressing mode.  If Addr can't be added to AddrMode this returns false and
/// leaves AddrMode unmodified.  This assumes that Addr is either a pointer type
/// or intptr_t for the target.
///
bool AddressingModeMatcher::MatchAddr(Value *Addr, unsigned Depth) {
  if (ConstantInt *CI = dyn_cast<ConstantInt>(Addr)) {
    // Fold in immediates if legal for the target.
    AddrMode.BaseOffs += CI->getSExtValue();
    if (TLI.isLegalAddressingMode(AddrMode, AccessTy))
      return true;
    AddrMode.BaseOffs -= CI->getSExtValue();
  } else if (GlobalValue *GV = dyn_cast<GlobalValue>(Addr)) {
    // If this is a global variable, try to fold it into the addressing mode.
    if (AddrMode.BaseGV == 0) {
      AddrMode.BaseGV = GV;
      if (TLI.isLegalAddressingMode(AddrMode, AccessTy))
        return true;
      AddrMode.BaseGV = 0;
    }
  } else if (Instruction *I = dyn_cast<Instruction>(Addr)) {
    ExtAddrMode BackupAddrMode = AddrMode;
    unsigned OldSize = AddrModeInsts.size();

    // Check to see if it is possible to fold this operation.
    if (MatchOperationAddr(I, I->getOpcode(), Depth)) {
      // Okay, it's possible to fold this.  Check to see if it is actually
      // *profitable* to do so.  We use a simple cost model to avoid increasing
      // register pressure too much.
      if (I->hasOneUse() ||
          IsProfitableToFoldIntoAddressingMode(I, BackupAddrMode, AddrMode)) {
        AddrModeInsts.push_back(I);
        return true;
      }
      
      // It isn't profitable to do this, roll back.
      //cerr << "NOT FOLDING: " << *I;
      AddrMode = BackupAddrMode;
      AddrModeInsts.resize(OldSize);
    }
  } else if (ConstantExpr *CE = dyn_cast<ConstantExpr>(Addr)) {
    if (MatchOperationAddr(CE, CE->getOpcode(), Depth))
      return true;
  } else if (isa<ConstantPointerNull>(Addr)) {
    // Null pointer gets folded without affecting the addressing mode.
    return true;
  }

  // Worse case, the target should support [reg] addressing modes. :)
  if (!AddrMode.HasBaseReg) {
    AddrMode.HasBaseReg = true;
    AddrMode.BaseReg = Addr;
    // Still check for legality in case the target supports [imm] but not [i+r].
    if (TLI.isLegalAddressingMode(AddrMode, AccessTy))
      return true;
    AddrMode.HasBaseReg = false;
    AddrMode.BaseReg = 0;
  }

  // If the base register is already taken, see if we can do [r+r].
  if (AddrMode.Scale == 0) {
    AddrMode.Scale = 1;
    AddrMode.ScaledReg = Addr;
    if (TLI.isLegalAddressingMode(AddrMode, AccessTy))
      return true;
    AddrMode.Scale = 0;
    AddrMode.ScaledReg = 0;
  }
  // Couldn't match.
  return false;
}


/// IsOperandAMemoryOperand - Check to see if all uses of OpVal by the specified
/// inline asm call are due to memory operands.  If so, return true, otherwise
/// return false.
static bool IsOperandAMemoryOperand(CallInst *CI, InlineAsm *IA, Value *OpVal,
                                    const TargetLowering &TLI) {
  std::vector<InlineAsm::ConstraintInfo>
  Constraints = IA->ParseConstraints();
  
  unsigned ArgNo = 1;   // ArgNo - The operand of the CallInst.
  for (unsigned i = 0, e = Constraints.size(); i != e; ++i) {
    TargetLowering::AsmOperandInfo OpInfo(Constraints[i]);
    
    // Compute the value type for each operand.
    switch (OpInfo.Type) {
      case InlineAsm::isOutput:
        if (OpInfo.isIndirect)
          OpInfo.CallOperandVal = CI->getOperand(ArgNo++);
        break;
      case InlineAsm::isInput:
        OpInfo.CallOperandVal = CI->getOperand(ArgNo++);
        break;
      case InlineAsm::isClobber:
        // Nothing to do.
        break;
    }
    
    // Compute the constraint code and ConstraintType to use.
    TLI.ComputeConstraintToUse(OpInfo, SDValue(),
                             OpInfo.ConstraintType == TargetLowering::C_Memory);
    
    // If this asm operand is our Value*, and if it isn't an indirect memory
    // operand, we can't fold it!
    if (OpInfo.CallOperandVal == OpVal &&
        (OpInfo.ConstraintType != TargetLowering::C_Memory ||
         !OpInfo.isIndirect))
      return false;
  }
  
  return true;
}


/// FindAllMemoryUses - Recursively walk all the uses of I until we find a
/// memory use.  If we find an obviously non-foldable instruction, return true.
/// Add the ultimately found memory instructions to MemoryUses.
static bool FindAllMemoryUses(Instruction *I,
                SmallVectorImpl<std::pair<Instruction*,unsigned> > &MemoryUses,
                              SmallPtrSet<Instruction*, 16> &ConsideredInsts,
                              const TargetLowering &TLI) {
  // If we already considered this instruction, we're done.
  if (!ConsideredInsts.insert(I))
    return false;
  
  // If this is an obviously unfoldable instruction, bail out.
  if (!MightBeFoldableInst(I))
    return true;

  // Loop over all the uses, recursively processing them.
  for (Value::use_iterator UI = I->use_begin(), E = I->use_end();
       UI != E; ++UI) {
    if (LoadInst *LI = dyn_cast<LoadInst>(*UI)) {
      MemoryUses.push_back(std::make_pair(LI, UI.getOperandNo()));
      continue;
    }
    
    if (StoreInst *SI = dyn_cast<StoreInst>(*UI)) {
      if (UI.getOperandNo() == 0) return true; // Storing addr, not into addr.
      MemoryUses.push_back(std::make_pair(SI, UI.getOperandNo()));
      continue;
    }
    
    if (CallInst *CI = dyn_cast<CallInst>(*UI)) {
      InlineAsm *IA = dyn_cast<InlineAsm>(CI->getCalledValue());
      if (IA == 0) return true;
      
      // If this is a memory operand, we're cool, otherwise bail out.
      if (!IsOperandAMemoryOperand(CI, IA, I, TLI))
        return true;
      continue;
    }
    
    if (FindAllMemoryUses(cast<Instruction>(*UI), MemoryUses, ConsideredInsts,
                          TLI))
      return true;
  }

  return false;
}


/// ValueAlreadyLiveAtInst - Retrn true if Val is already known to be live at
/// the use site that we're folding it into.  If so, there is no cost to
/// include it in the addressing mode.  KnownLive1 and KnownLive2 are two values
/// that we know are live at the instruction already.
bool AddressingModeMatcher::ValueAlreadyLiveAtInst(Value *Val,Value *KnownLive1,
                                                   Value *KnownLive2) {
  // If Val is either of the known-live values, we know it is live!
  if (Val == 0 || Val == KnownLive1 || Val == KnownLive2)
    return true;
  
  // All values other than instructions and arguments (e.g. constants) are live.
  if (!isa<Instruction>(Val) && !isa<Argument>(Val)) return true;
  
  // If Val is a constant sized alloca in the entry block, it is live, this is
  // true because it is just a reference to the stack/frame pointer, which is
  // live for the whole function.
  if (AllocaInst *AI = dyn_cast<AllocaInst>(Val))
    if (AI->isStaticAlloca())
      return true;
  
  // Check to see if this value is already used in the memory instruction's
  // block.  If so, it's already live into the block at the very least, so we
  // can reasonably fold it.
  BasicBlock *MemBB = MemoryInst->getParent();
  for (Value::use_iterator UI = Val->use_begin(), E = Val->use_end();
       UI != E; ++UI)
    // We know that uses of arguments and instructions have to be instructions.
    if (cast<Instruction>(*UI)->getParent() == MemBB)
      return true;
  
  return false;
}



/// IsProfitableToFoldIntoAddressingMode - It is possible for the addressing
/// mode of the machine to fold the specified instruction into a load or store
/// that ultimately uses it.  However, the specified instruction has multiple
/// uses.  Given this, it may actually increase register pressure to fold it
/// into the load.  For example, consider this code:
///
///     X = ...
///     Y = X+1
///     use(Y)   -> nonload/store
///     Z = Y+1
///     load Z
///
/// In this case, Y has multiple uses, and can be folded into the load of Z
/// (yielding load [X+2]).  However, doing this will cause both "X" and "X+1" to
/// be live at the use(Y) line.  If we don't fold Y into load Z, we use one
/// fewer register.  Since Y can't be folded into "use(Y)" we don't increase the
/// number of computations either.
///
/// Note that this (like most of CodeGenPrepare) is just a rough heuristic.  If
/// X was live across 'load Z' for other reasons, we actually *would* want to
/// fold the addressing mode in the Z case.  This would make Y die earlier.
bool AddressingModeMatcher::
IsProfitableToFoldIntoAddressingMode(Instruction *I, ExtAddrMode &AMBefore,
                                     ExtAddrMode &AMAfter) {
  if (IgnoreProfitability) return true;
  
  // AMBefore is the addressing mode before this instruction was folded into it,
  // and AMAfter is the addressing mode after the instruction was folded.  Get
  // the set of registers referenced by AMAfter and subtract out those
  // referenced by AMBefore: this is the set of values which folding in this
  // address extends the lifetime of.
  //
  // Note that there are only two potential values being referenced here,
  // BaseReg and ScaleReg (global addresses are always available, as are any
  // folded immediates).
  Value *BaseReg = AMAfter.BaseReg, *ScaledReg = AMAfter.ScaledReg;
  
  // If the BaseReg or ScaledReg was referenced by the previous addrmode, their
  // lifetime wasn't extended by adding this instruction.
  if (ValueAlreadyLiveAtInst(BaseReg, AMBefore.BaseReg, AMBefore.ScaledReg))
    BaseReg = 0;
  if (ValueAlreadyLiveAtInst(ScaledReg, AMBefore.BaseReg, AMBefore.ScaledReg))
    ScaledReg = 0;

  // If folding this instruction (and it's subexprs) didn't extend any live
  // ranges, we're ok with it.
  if (BaseReg == 0 && ScaledReg == 0)
    return true;

  // If all uses of this instruction are ultimately load/store/inlineasm's,
  // check to see if their addressing modes will include this instruction.  If
  // so, we can fold it into all uses, so it doesn't matter if it has multiple
  // uses.
  SmallVector<std::pair<Instruction*,unsigned>, 16> MemoryUses;
  SmallPtrSet<Instruction*, 16> ConsideredInsts;
  if (FindAllMemoryUses(I, MemoryUses, ConsideredInsts, TLI))
    return false;  // Has a non-memory, non-foldable use!
  
  // Now that we know that all uses of this instruction are part of a chain of
  // computation involving only operations that could theoretically be folded
  // into a memory use, loop over each of these uses and see if they could
  // *actually* fold the instruction.
  SmallVector<Instruction*, 32> MatchedAddrModeInsts;
  for (unsigned i = 0, e = MemoryUses.size(); i != e; ++i) {
    Instruction *User = MemoryUses[i].first;
    unsigned OpNo = MemoryUses[i].second;
    
    // Get the access type of this use.  If the use isn't a pointer, we don't
    // know what it accesses.
    Value *Address = User->getOperand(OpNo);
    if (!isa<PointerType>(Address->getType()))
      return false;
    const Type *AddressAccessTy =
      cast<PointerType>(Address->getType())->getElementType();
    
    // Do a match against the root of this address, ignoring profitability. This
    // will tell us if the addressing mode for the memory operation will
    // *actually* cover the shared instruction.
    ExtAddrMode Result;
    AddressingModeMatcher Matcher(MatchedAddrModeInsts, TLI, AddressAccessTy,
                                  MemoryInst, Result);
    Matcher.IgnoreProfitability = true;
    bool Success = Matcher.MatchAddr(Address, 0);
    Success = Success; assert(Success && "Couldn't select *anything*?");

    // If the match didn't cover I, then it won't be shared by it.
    if (std::find(MatchedAddrModeInsts.begin(), MatchedAddrModeInsts.end(),
                  I) == MatchedAddrModeInsts.end())
      return false;
    
    MatchedAddrModeInsts.clear();
  }
  
  return true;
}


//===----------------------------------------------------------------------===//
// Memory Optimization
//===----------------------------------------------------------------------===//

/// IsNonLocalValue - Return true if the specified values are defined in a
/// different basic block than BB.
static bool IsNonLocalValue(Value *V, BasicBlock *BB) {
  if (Instruction *I = dyn_cast<Instruction>(V))
    return I->getParent() != BB;
  return false;
}

/// OptimizeMemoryInst - Load and Store Instructions have often have
/// addressing modes that can do significant amounts of computation.  As such,
/// instruction selection will try to get the load or store to do as much
/// computation as possible for the program.  The problem is that isel can only
/// see within a single block.  As such, we sink as much legal addressing mode
/// stuff into the block as possible.
///
/// This method is used to optimize both load/store and inline asms with memory
/// operands.
bool CodeGenPrepare::OptimizeMemoryInst(Instruction *MemoryInst, Value *Addr,
                                        const Type *AccessTy,
                                        DenseMap<Value*,Value*> &SunkAddrs) {
  // Figure out what addressing mode will be built up for this operation.
  SmallVector<Instruction*, 16> AddrModeInsts;
  ExtAddrMode AddrMode = AddressingModeMatcher::Match(Addr, AccessTy,MemoryInst,
                                                      AddrModeInsts, *TLI);

  // Check to see if any of the instructions supersumed by this addr mode are
  // non-local to I's BB.
  bool AnyNonLocal = false;
  for (unsigned i = 0, e = AddrModeInsts.size(); i != e; ++i) {
    if (IsNonLocalValue(AddrModeInsts[i], MemoryInst->getParent())) {
      AnyNonLocal = true;
      break;
    }
  }

  // If all the instructions matched are already in this BB, don't do anything.
  if (!AnyNonLocal) {
    DEBUG(cerr << "CGP: Found      local addrmode: " << AddrMode << "\n");
    return false;
  }

  // Insert this computation right after this user.  Since our caller is
  // scanning from the top of the BB to the bottom, reuse of the expr are
  // guaranteed to happen later.
  BasicBlock::iterator InsertPt = MemoryInst;

  // Now that we determined the addressing expression we want to use and know
  // that we have to sink it into this block.  Check to see if we have already
  // done this for some other load/store instr in this block.  If so, reuse the
  // computation.
  Value *&SunkAddr = SunkAddrs[Addr];
  if (SunkAddr) {
    DEBUG(cerr << "CGP: Reusing nonlocal addrmode: " << AddrMode << "\n");
    if (SunkAddr->getType() != Addr->getType())
      SunkAddr = new BitCastInst(SunkAddr, Addr->getType(), "tmp", InsertPt);
  } else {
    DEBUG(cerr << "CGP: SINKING nonlocal addrmode: " << AddrMode << "\n");
    const Type *IntPtrTy = TLI->getTargetData()->getIntPtrType();

    Value *Result = 0;
    // Start with the scale value.
    if (AddrMode.Scale) {
      Value *V = AddrMode.ScaledReg;
      if (V->getType() == IntPtrTy) {
        // done.
      } else if (isa<PointerType>(V->getType())) {
        V = new PtrToIntInst(V, IntPtrTy, "sunkaddr", InsertPt);
      } else if (cast<IntegerType>(IntPtrTy)->getBitWidth() <
                 cast<IntegerType>(V->getType())->getBitWidth()) {
        V = new TruncInst(V, IntPtrTy, "sunkaddr", InsertPt);
      } else {
        V = new SExtInst(V, IntPtrTy, "sunkaddr", InsertPt);
      }
      if (AddrMode.Scale != 1)
        V = BinaryOperator::CreateMul(V, ConstantInt::get(IntPtrTy,
                                                          AddrMode.Scale),
                                      "sunkaddr", InsertPt);
      Result = V;
    }

    // Add in the base register.
    if (AddrMode.BaseReg) {
      Value *V = AddrMode.BaseReg;
      if (V->getType() != IntPtrTy)
        V = new PtrToIntInst(V, IntPtrTy, "sunkaddr", InsertPt);
      if (Result)
        Result = BinaryOperator::CreateAdd(Result, V, "sunkaddr", InsertPt);
      else
        Result = V;
    }

    // Add in the BaseGV if present.
    if (AddrMode.BaseGV) {
      Value *V = new PtrToIntInst(AddrMode.BaseGV, IntPtrTy, "sunkaddr",
                                  InsertPt);
      if (Result)
        Result = BinaryOperator::CreateAdd(Result, V, "sunkaddr", InsertPt);
      else
        Result = V;
    }

    // Add in the Base Offset if present.
    if (AddrMode.BaseOffs) {
      Value *V = ConstantInt::get(IntPtrTy, AddrMode.BaseOffs);
      if (Result)
        Result = BinaryOperator::CreateAdd(Result, V, "sunkaddr", InsertPt);
      else
        Result = V;
    }

    if (Result == 0)
      SunkAddr = Constant::getNullValue(Addr->getType());
    else
      SunkAddr = new IntToPtrInst(Result, Addr->getType(), "sunkaddr",InsertPt);
  }

  MemoryInst->replaceUsesOfWith(Addr, SunkAddr);

  if (Addr->use_empty())
    RecursivelyDeleteTriviallyDeadInstructions(Addr);
  return true;
}

/// OptimizeInlineAsmInst - If there are any memory operands, use
/// OptimizeMemoryInst to sink their address computing into the block when
/// possible / profitable.
bool CodeGenPrepare::OptimizeInlineAsmInst(Instruction *I, CallSite CS,
                                           DenseMap<Value*,Value*> &SunkAddrs) {
  bool MadeChange = false;
  InlineAsm *IA = cast<InlineAsm>(CS.getCalledValue());

  // Do a prepass over the constraints, canonicalizing them, and building up the
  // ConstraintOperands list.
  std::vector<InlineAsm::ConstraintInfo>
    ConstraintInfos = IA->ParseConstraints();

  /// ConstraintOperands - Information about all of the constraints.
  std::vector<TargetLowering::AsmOperandInfo> ConstraintOperands;
  unsigned ArgNo = 0;   // ArgNo - The argument of the CallInst.
  for (unsigned i = 0, e = ConstraintInfos.size(); i != e; ++i) {
    ConstraintOperands.
      push_back(TargetLowering::AsmOperandInfo(ConstraintInfos[i]));
    TargetLowering::AsmOperandInfo &OpInfo = ConstraintOperands.back();

    // Compute the value type for each operand.
    switch (OpInfo.Type) {
    case InlineAsm::isOutput:
      if (OpInfo.isIndirect)
        OpInfo.CallOperandVal = CS.getArgument(ArgNo++);
      break;
    case InlineAsm::isInput:
      OpInfo.CallOperandVal = CS.getArgument(ArgNo++);
      break;
    case InlineAsm::isClobber:
      // Nothing to do.
      break;
    }

    // Compute the constraint code and ConstraintType to use.
    TLI->ComputeConstraintToUse(OpInfo, SDValue(),
                             OpInfo.ConstraintType == TargetLowering::C_Memory);

    if (OpInfo.ConstraintType == TargetLowering::C_Memory &&
        OpInfo.isIndirect) {
      Value *OpVal = OpInfo.CallOperandVal;
      MadeChange |= OptimizeMemoryInst(I, OpVal, OpVal->getType(), SunkAddrs);
    }
  }

  return MadeChange;
}

bool CodeGenPrepare::OptimizeExtUses(Instruction *I) {
  BasicBlock *DefBB = I->getParent();

  // If both result of the {s|z}xt and its source are live out, rewrite all
  // other uses of the source with result of extension.
  Value *Src = I->getOperand(0);
  if (Src->hasOneUse())
    return false;

  // Only do this xform if truncating is free.
  if (TLI && !TLI->isTruncateFree(I->getType(), Src->getType()))
    return false;

  // Only safe to perform the optimization if the source is also defined in
  // this block.
  if (!isa<Instruction>(Src) || DefBB != cast<Instruction>(Src)->getParent())
    return false;

  bool DefIsLiveOut = false;
  for (Value::use_iterator UI = I->use_begin(), E = I->use_end();
       UI != E; ++UI) {
    Instruction *User = cast<Instruction>(*UI);

    // Figure out which BB this ext is used in.
    BasicBlock *UserBB = User->getParent();
    if (UserBB == DefBB) continue;
    DefIsLiveOut = true;
    break;
  }
  if (!DefIsLiveOut)
    return false;

  // Make sure non of the uses are PHI nodes.
  for (Value::use_iterator UI = Src->use_begin(), E = Src->use_end();
       UI != E; ++UI) {
    Instruction *User = cast<Instruction>(*UI);
    BasicBlock *UserBB = User->getParent();
    if (UserBB == DefBB) continue;
    // Be conservative. We don't want this xform to end up introducing
    // reloads just before load / store instructions.
    if (isa<PHINode>(User) || isa<LoadInst>(User) || isa<StoreInst>(User))
      return false;
  }

  // InsertedTruncs - Only insert one trunc in each block once.
  DenseMap<BasicBlock*, Instruction*> InsertedTruncs;

  bool MadeChange = false;
  for (Value::use_iterator UI = Src->use_begin(), E = Src->use_end();
       UI != E; ++UI) {
    Use &TheUse = UI.getUse();
    Instruction *User = cast<Instruction>(*UI);

    // Figure out which BB this ext is used in.
    BasicBlock *UserBB = User->getParent();
    if (UserBB == DefBB) continue;

    // Both src and def are live in this block. Rewrite the use.
    Instruction *&InsertedTrunc = InsertedTruncs[UserBB];

    if (!InsertedTrunc) {
      BasicBlock::iterator InsertPt = UserBB->getFirstNonPHI();

      InsertedTrunc = new TruncInst(I, Src->getType(), "", InsertPt);
    }

    // Replace a use of the {s|z}ext source with a use of the result.
    TheUse = InsertedTrunc;

    MadeChange = true;
  }

  return MadeChange;
}

// In this pass we look for GEP and cast instructions that are used
// across basic blocks and rewrite them to improve basic-block-at-a-time
// selection.
bool CodeGenPrepare::OptimizeBlock(BasicBlock &BB) {
  bool MadeChange = false;

  // Split all critical edges where the dest block has a PHI and where the phi
  // has shared immediate operands.
  TerminatorInst *BBTI = BB.getTerminator();
  if (BBTI->getNumSuccessors() > 1) {
    for (unsigned i = 0, e = BBTI->getNumSuccessors(); i != e; ++i)
      if (isa<PHINode>(BBTI->getSuccessor(i)->begin()) &&
          isCriticalEdge(BBTI, i, true))
        SplitEdgeNicely(BBTI, i, this);
  }


  // Keep track of non-local addresses that have been sunk into this block.
  // This allows us to avoid inserting duplicate code for blocks with multiple
  // load/stores of the same address.
  DenseMap<Value*, Value*> SunkAddrs;

  for (BasicBlock::iterator BBI = BB.begin(), E = BB.end(); BBI != E; ) {
    Instruction *I = BBI++;

    if (CastInst *CI = dyn_cast<CastInst>(I)) {
      // If the source of the cast is a constant, then this should have
      // already been constant folded.  The only reason NOT to constant fold
      // it is if something (e.g. LSR) was careful to place the constant
      // evaluation in a block other than then one that uses it (e.g. to hoist
      // the address of globals out of a loop).  If this is the case, we don't
      // want to forward-subst the cast.
      if (isa<Constant>(CI->getOperand(0)))
        continue;

      bool Change = false;
      if (TLI) {
        Change = OptimizeNoopCopyExpression(CI, *TLI);
        MadeChange |= Change;
      }

      if (!Change && (isa<ZExtInst>(I) || isa<SExtInst>(I)))
        MadeChange |= OptimizeExtUses(I);
    } else if (CmpInst *CI = dyn_cast<CmpInst>(I)) {
      MadeChange |= OptimizeCmpExpression(CI);
    } else if (LoadInst *LI = dyn_cast<LoadInst>(I)) {
      if (TLI)
        MadeChange |= OptimizeMemoryInst(I, I->getOperand(0), LI->getType(),
                                         SunkAddrs);
    } else if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
      if (TLI)
        MadeChange |= OptimizeMemoryInst(I, SI->getOperand(1),
                                         SI->getOperand(0)->getType(),
                                         SunkAddrs);
    } else if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(I)) {
      if (GEPI->hasAllZeroIndices()) {
        /// The GEP operand must be a pointer, so must its result -> BitCast
        Instruction *NC = new BitCastInst(GEPI->getOperand(0), GEPI->getType(),
                                          GEPI->getName(), GEPI);
        GEPI->replaceAllUsesWith(NC);
        GEPI->eraseFromParent();
        MadeChange = true;
        BBI = NC;
      }
    } else if (CallInst *CI = dyn_cast<CallInst>(I)) {
      // If we found an inline asm expession, and if the target knows how to
      // lower it to normal LLVM code, do so now.
      if (TLI && isa<InlineAsm>(CI->getCalledValue()))
        if (const TargetAsmInfo *TAI =
            TLI->getTargetMachine().getTargetAsmInfo()) {
          if (TAI->ExpandInlineAsm(CI))
            BBI = BB.begin();
          else
            // Sink address computing for memory operands into the block.
            MadeChange |= OptimizeInlineAsmInst(I, &(*CI), SunkAddrs);
        }
    }
  }

  return MadeChange;
}

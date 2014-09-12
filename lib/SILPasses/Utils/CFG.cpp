//===--- CFG.cpp - Utilities for SIL CFG transformations --------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SIL/Dominance.h"
#include "swift/SIL/LoopInfo.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SILPasses/Utils/CFG.h"

using namespace swift;

/// \brief Adds a new argument to an edge between a branch and a destination
/// block.
///
/// \param Branch The terminator to add the argument to.
/// \param Dest The destination block of the edge.
/// \param Val The value to the arguments of the branch.
/// \return The created branch. The old branch is deleted.
/// The argument is appended at the end of the argument tuple.
TermInst *swift::addNewEdgeValueToBranch(TermInst *Branch, SILBasicBlock *Dest,
                                         SILValue Val) {
  SILBuilder Builder(Branch);
  TermInst *NewBr = nullptr;

  if (CondBranchInst *CBI = dyn_cast<CondBranchInst>(Branch)) {
    SmallVector<SILValue, 8> TrueArgs;
    SmallVector<SILValue, 8> FalseArgs;

    for (auto A : CBI->getTrueArgs())
      TrueArgs.push_back(A);

    for (auto A : CBI->getFalseArgs())
      FalseArgs.push_back(A);

    if (Dest == CBI->getTrueBB()) {
      TrueArgs.push_back(Val);
      assert(TrueArgs.size() == Dest->getNumBBArg());
    }
    if (Dest == CBI->getFalseBB()) {
      FalseArgs.push_back(Val);
      assert(FalseArgs.size() == Dest->getNumBBArg());
    }

    NewBr = Builder.createCondBranch(CBI->getLoc(), CBI->getCondition(),
                                    CBI->getTrueBB(), TrueArgs,
                                    CBI->getFalseBB(), FalseArgs);
  } else if (BranchInst *BI = dyn_cast<BranchInst>(Branch)) {
    SmallVector<SILValue, 8> Args;

    for (auto A : BI->getArgs())
      Args.push_back(A);

    Args.push_back(Val);
    assert(Args.size() == Dest->getNumBBArg());
    NewBr = Builder.createBranch(BI->getLoc(), BI->getDestBB(), Args);
  } else {
    NewBr->dump();
    // At the moment we can only add arguments to br and cond_br.
    llvm_unreachable("Can't add argument to terminator");
    return NewBr;
  }

  Branch->dropAllReferences();
  Branch->eraseFromParent();

  return NewBr;
}

/// \brief Changes the edge value between a branch and destination basic block
/// at the specified index. Changes all edges from \p Branch to \p Dest to carry
/// the value.
///
/// \param Branch The branch to modify.
/// \param Dest The destination of the edge.
/// \param Idx The index of the argument to modify.
/// \param Val The new value to use.
/// \return The new branch. Deletes the old one.
/// Changes the edge value between a branch and destination basic block at the
/// specified index.
TermInst *swift::changeEdgeValue(TermInst *Branch, SILBasicBlock *Dest,
                                 size_t Idx, SILValue Val) {
  SILBuilder Builder(Branch);

  if (CondBranchInst *CBI = dyn_cast<CondBranchInst>(Branch)) {
    SmallVector<SILValue, 8> TrueArgs;
    SmallVector<SILValue, 8> FalseArgs;

    OperandValueArrayRef OldTrueArgs = CBI->getTrueArgs();
    bool BranchOnTrue = CBI->getTrueBB() == Dest;
    assert((!BranchOnTrue || Idx < OldTrueArgs.size()) && "Not enough edges");

    // Copy the edge values overwritting the edge at Idx.
    for (unsigned i = 0, e = OldTrueArgs.size(); i != e; ++i) {
      if (BranchOnTrue && Idx == i)
        TrueArgs.push_back(Val);
      else
        TrueArgs.push_back(OldTrueArgs[i]);
    }
    assert(TrueArgs.size() == CBI->getTrueBB()->getNumBBArg() &&
           "Destination block's number of arguments must match");

    OperandValueArrayRef OldFalseArgs = CBI->getFalseArgs();
    bool BranchOnFalse = CBI->getFalseBB() == Dest;
    assert((!BranchOnFalse || Idx < OldFalseArgs.size()) && "Not enough edges");

    // Copy the edge values overwritting the edge at Idx.
    for (unsigned i = 0, e = OldFalseArgs.size(); i != e; ++i) {
      if (BranchOnFalse && Idx == i)
        FalseArgs.push_back(Val);
      else
        FalseArgs.push_back(OldFalseArgs[i]);
    }
    assert(FalseArgs.size() == CBI->getFalseBB()->getNumBBArg() &&
           "Destination block's number of arguments must match");

    CBI = Builder.createCondBranch(CBI->getLoc(), CBI->getCondition(),
                                    CBI->getTrueBB(), TrueArgs,
                                    CBI->getFalseBB(), FalseArgs);
    Branch->dropAllReferences();
    Branch->eraseFromParent();
    return CBI;
  }

  if (BranchInst *BI = dyn_cast<BranchInst>(Branch)) {
    SmallVector<SILValue, 8> Args;

    assert(Idx < BI->getNumArgs() && "Not enough edges");
    OperandValueArrayRef OldArgs = BI->getArgs();

    // Copy the edge values overwritting the edge at Idx.
    for (unsigned i = 0, e = OldArgs.size(); i != e; ++i) {
      if (Idx == i)
        Args.push_back(Val);
      else
        Args.push_back(OldArgs[i]);
    }
    assert(Args.size() == Dest->getNumBBArg());

    BI = Builder.createBranch(BI->getLoc(), BI->getDestBB(), Args);
    Branch->dropAllReferences();
    Branch->eraseFromParent();
    return BI;
  }

  llvm_unreachable("Unhandled terminator leading to merge block");
}

/// \brief Check if the edge from the terminator is critical.
bool swift::isCriticalEdge(TermInst *T, unsigned EdgeIdx) {
  assert(T->getSuccessors().size() > EdgeIdx && "Not enough successors");

  auto SrcSuccs = T->getSuccessors();
  if (SrcSuccs.size() <= 1)
    return false;

  SILBasicBlock *DestBB = SrcSuccs[EdgeIdx];
  assert(!DestBB->pred_empty() && "There should be a predecessor");
  if (DestBB->getSinglePredecessor())
    return false;

  return true;
}

static void changeBranchTargetAndStripArgs(TermInst *T, unsigned EdgeIdx,
                                           SILBasicBlock *NewDest) {
  SILBuilder B(T);

  if (auto Br = dyn_cast<BranchInst>(T)) {
    B.createBranch(T->getLoc(), NewDest);
    Br->dropAllReferences();
    Br->eraseFromParent();
    return;
  }

  if (auto CondBr = dyn_cast<CondBranchInst>(T)) {
    if (EdgeIdx) {
      SmallVector<SILValue, 8> Args;
      for (auto Arg : CondBr->getTrueArgs())
        Args.push_back(Arg);
      B.createCondBranch(CondBr->getLoc(), CondBr->getCondition(),
                         CondBr->getTrueBB(), Args, NewDest,
                         llvm::None);
    } else {
      SmallVector<SILValue, 8> Args;
      for (auto Arg : CondBr->getFalseArgs())
        Args.push_back(Arg);
      B.createCondBranch(CondBr->getLoc(), CondBr->getCondition(), NewDest,
                         llvm::None, CondBr->getFalseBB(), Args);
    }
    CondBr->dropAllReferences();
    CondBr->eraseFromParent();
    return;
  }

  // For now this utility is only used to split critical edges involving
  // cond_br.
  llvm_unreachable("Not yet implemented");
}

static OperandValueArrayRef getEdgeArgs(TermInst *T, unsigned EdgeIdx) {
  if (auto Br = dyn_cast<BranchInst>(T)) {
    return Br->getArgs();
  }

  if (auto CondBr = dyn_cast<CondBranchInst>(T)) {
    assert(EdgeIdx < 2);
    return EdgeIdx ? CondBr->getFalseArgs() : CondBr->getTrueArgs();
  }

  // For now this utility is only used to split critical edges involving
  // cond_br.
  llvm_unreachable("Not yet implemented");
}

/// Splits the basic block at the iterator with an unconditional branch and
/// updates the dominator tree and loop info.
SILBasicBlock *swift::splitBasicBlockAndBranch(SILInstruction *SplitBeforeInst,
                                               DominanceInfo *DT,
                                               SILLoopInfo *LI) {
  auto *OrigBB = SplitBeforeInst->getParent();
  auto *NewBB = OrigBB->splitBasicBlockAndBranch(SplitBeforeInst,
                                                 SplitBeforeInst->getLoc());

  // Update the dominator tree.
  if (DT) {
    auto OrigBBDTNode = DT->getNode(OrigBB);
    if (OrigBBDTNode) {
      auto NewBBDTNode = DT->addNewBlock(NewBB, OrigBB);
      for (auto *Child : *OrigBBDTNode)
        if (Child != NewBBDTNode)
          DT->changeImmediateDominator(Child, NewBBDTNode);
    }
  }

  // Update loop info.
  if (LI)
    if (auto *OrigBBLoop = LI->getLoopFor(OrigBB)) {
      OrigBBLoop->addBasicBlockToLoop(NewBB, LI->getBase());
    }

  return NewBB;
}
/// Splits the n-th critical edge from the terminator and updates dominance and
/// loop info if set.
/// Returns the newly created basic block on success or nullptr otherwise (if
/// the edge was not critical.
SILBasicBlock *swift::splitCriticalEdge(TermInst *T, unsigned EdgeIdx,
                                        DominanceInfo *DT, SILLoopInfo *LI) {
  if (!isCriticalEdge(T, EdgeIdx))
    return nullptr;

  auto *SrcBB = T->getParent();
  auto *Fn = SrcBB->getParent();

  SILBasicBlock *DestBB = T->getSuccessors()[EdgeIdx];

  auto EdgeArgs = getEdgeArgs(T, EdgeIdx);
  SmallVector<SILValue, 8> Args;
  for (auto Arg : EdgeArgs)
    Args.push_back(Arg);

  // Create a new basic block in the edge, and insert it after the SrcBB.
  auto *EdgeBB = new (Fn->getModule()) SILBasicBlock(Fn, SrcBB);

  // Connect it to the successor with the args of the old edge.
  auto &InstList = EdgeBB->getInstList();
  InstList.insert(InstList.end(),
                  BranchInst::create(T->getLoc(), DestBB, Args, *Fn));

  // Strip the arguments and rewire the branch in the source block.
  changeBranchTargetAndStripArgs(T, EdgeIdx, EdgeBB);

  if (!DT && !LI)
    return EdgeBB;

  // Update the dominator tree.
  if (DT) {
    auto *DestBBNode = DT->getNode(DestBB);
    // Unreachable code could result in a null return here.
    if (DestBBNode) {
      // The new block is dominated by the SrcBB.
      auto *EdgeBBNode = DT->addNewBlock(EdgeBB, SrcBB);

      // Are all predecessors of DestBB dominated by SrcBB?
      bool OldSrcBBDominatesAllPreds = std::all_of(
          DestBB->pred_begin(), DestBB->pred_end(), [=](SILBasicBlock *B) {
            if (DT->dominates(SrcBB, B))
              return true;
            return false;
          });

      // If so, the new bb dominates DestBB now.
      if (OldSrcBBDominatesAllPreds)
        DT->changeImmediateDominator(DestBBNode, EdgeBBNode);
    }
  }

  if (!LI)
    return EdgeBB;

  // Update loop info. Both blocks must be in a loop otherwise the split block
  // is outside the loop.
  SILLoop *SrcBBLoop = LI->getLoopFor(SrcBB);
  if (!SrcBBLoop)
    return EdgeBB;
  SILLoop *DstBBLoop = LI->getLoopFor(DestBB);
  if (!DstBBLoop)
    return EdgeBB;

  // Same loop.
  if (DstBBLoop == SrcBBLoop) {
    DstBBLoop->addBasicBlockToLoop(EdgeBB, LI->getBase());
    return EdgeBB;
  }

  // Edge from inner to outer loop.
  if (DstBBLoop->contains(SrcBBLoop)) {
    DstBBLoop->addBasicBlockToLoop(EdgeBB, LI->getBase());
    return EdgeBB;
  }

  // Edge from outer to inner loop.
  if (SrcBBLoop->contains(DstBBLoop)) {
    SrcBBLoop->addBasicBlockToLoop(EdgeBB, LI->getBase());
    return EdgeBB;
  }

  // Neither loop contains the other. The destination must be the header of its
  // loop. Otherwise, we would be creating irreducable control flow.
  assert(DstBBLoop->getHeader() == DestBB &&
         "Creating irreducible control flow?");

  // Add to outer loop if there is one.
  if (auto *Parent = DstBBLoop->getParentLoop())
    Parent->addBasicBlockToLoop(EdgeBB, LI->getBase());

  return EdgeBB;
}

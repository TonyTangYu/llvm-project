//===- SwitchLoweringUtils.cpp - Switch Lowering --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains switch inst lowering optimizations and utilities for
// codegen, so that it can be used for both SelectionDAG and GlobalISel.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/SwitchLoweringUtils.h"
#include "llvm/CodeGen/FunctionLoweringInfo.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/Target/TargetMachine.h"
#if defined(ENABLE_AUTOTUNER)
#include "llvm/AutoTuner/AutoTuning.h"
#endif

using namespace llvm;
using namespace SwitchCG;

uint64_t SwitchCG::getJumpTableRange(const CaseClusterVector &Clusters,
                                     unsigned First, unsigned Last) {
  assert(Last >= First);
  const APInt &LowCase = Clusters[First].Low->getValue();
  const APInt &HighCase = Clusters[Last].High->getValue();
  assert(LowCase.getBitWidth() == HighCase.getBitWidth());

  // FIXME: A range of consecutive cases has 100% density, but only requires one
  // comparison to lower. We should discriminate against such consecutive ranges
  // in jump tables.
  return (HighCase - LowCase).getLimitedValue((UINT64_MAX - 1) / 100) + 1;
}

uint64_t
SwitchCG::getJumpTableNumCases(const SmallVectorImpl<unsigned> &TotalCases,
                               unsigned First, unsigned Last) {
  assert(Last >= First);
  assert(TotalCases[Last] >= TotalCases[First]);
  uint64_t NumCases =
      TotalCases[Last] - (First == 0 ? 0 : TotalCases[First - 1]);
  return NumCases;
}

void SwitchCG::SwitchLowering::findJumpTables(CaseClusterVector &Clusters,
                                              const SwitchInst *SI,
                                              MachineBasicBlock *DefaultMBB,
                                              ProfileSummaryInfo *PSI,
                                              BlockFrequencyInfo *BFI) {
#ifndef NDEBUG
  // Clusters must be non-empty, sorted, and only contain Range clusters.
  assert(!Clusters.empty());
  for (CaseCluster &C : Clusters)
    assert(C.Kind == CC_Range);
  for (unsigned i = 1, e = Clusters.size(); i < e; ++i)
    assert(Clusters[i - 1].High->getValue().slt(Clusters[i].Low->getValue()));
#endif

  assert(TLI && "TLI not set!");
  if (!TLI->areJTsAllowed(SI->getParent()->getParent()))
    return;

#if defined(ENABLE_AUTOTUNER)
  unsigned MinJumpTableEntries = TLI->getMinimumJumpTableEntries();
  // Overwrite MinJumpTableEntries when it is set by Autotuner
  if (autotuning::Engine.isEnabled()) {
    autotuning::Engine.initContainer(SI->ATESwitchInst.get(),
                                     "switch-lowering");

    int NewValue = 0; // the int value is set by lookUpParams()
    bool Changed =
        SI->ATESwitchInst->lookUpParams<int>("MinJumpTableEntries", NewValue);
    if (Changed)
      MinJumpTableEntries = NewValue;
  }
#else
  const unsigned MinJumpTableEntries = TLI->getMinimumJumpTableEntries();
#endif

  const unsigned SmallNumberOfEntries = MinJumpTableEntries / 2;

  // Bail if not enough cases.
  const int64_t N = Clusters.size();
  if (N < 2 || N < MinJumpTableEntries)
    return;

  // Accumulated number of cases in each cluster and those prior to it.
  SmallVector<unsigned, 8> TotalCases(N);
  for (unsigned i = 0; i < N; ++i) {
    const APInt &Hi = Clusters[i].High->getValue();
    const APInt &Lo = Clusters[i].Low->getValue();
    TotalCases[i] = (Hi - Lo).getLimitedValue() + 1;
    if (i != 0)
      TotalCases[i] += TotalCases[i - 1];
  }

  uint64_t Range = getJumpTableRange(Clusters,0, N - 1);
  uint64_t NumCases = getJumpTableNumCases(TotalCases, 0, N - 1);
  assert(NumCases < UINT64_MAX / 100);
  assert(Range >= NumCases);

  // Cheap case: the whole range may be suitable for jump table.
  if (TLI->isSuitableForJumpTable(SI, NumCases, Range, PSI, BFI)) {
    CaseCluster JTCluster;
    if (buildJumpTable(Clusters, 0, N - 1, SI, DefaultMBB, JTCluster)) {
      Clusters[0] = JTCluster;
      Clusters.resize(1);
      return;
    }
  }

  // The algorithm below is not suitable for -O0.
  if (TM->getOptLevel() == CodeGenOpt::None)
    return;

  // Split Clusters into minimum number of dense partitions. The algorithm uses
  // the same idea as Kannan & Proebsting "Correction to 'Producing Good Code
  // for the Case Statement'" (1994), but builds the MinPartitions array in
  // reverse order to make it easier to reconstruct the partitions in ascending
  // order. In the choice between two optimal partitionings, it picks the one
  // which yields more jump tables.

  // MinPartitions[i] is the minimum nbr of partitions of Clusters[i..N-1].
  SmallVector<unsigned, 8> MinPartitions(N);
  // LastElement[i] is the last element of the partition starting at i.
  SmallVector<unsigned, 8> LastElement(N);
  // PartitionsScore[i] is used to break ties when choosing between two
  // partitionings resulting in the same number of partitions.
  SmallVector<unsigned, 8> PartitionsScore(N);
  // For PartitionsScore, a small number of comparisons is considered as good as
  // a jump table and a single comparison is considered better than a jump
  // table.
  enum PartitionScores : unsigned {
    NoTable = 0,
    Table = 1,
    FewCases = 1,
    SingleCase = 2
  };

  // Base case: There is only one way to partition Clusters[N-1].
  MinPartitions[N - 1] = 1;
  LastElement[N - 1] = N - 1;
  PartitionsScore[N - 1] = PartitionScores::SingleCase;

  // Note: loop indexes are signed to avoid underflow.
  for (int64_t i = N - 2; i >= 0; i--) {
    // Find optimal partitioning of Clusters[i..N-1].
    // Baseline: Put Clusters[i] into a partition on its own.
    MinPartitions[i] = MinPartitions[i + 1] + 1;
    LastElement[i] = i;
    PartitionsScore[i] = PartitionsScore[i + 1] + PartitionScores::SingleCase;

    // Search for a solution that results in fewer partitions.
    for (int64_t j = N - 1; j > i; j--) {
      // Try building a partition from Clusters[i..j].
      Range = getJumpTableRange(Clusters, i, j);
      NumCases = getJumpTableNumCases(TotalCases, i, j);
      assert(NumCases < UINT64_MAX / 100);
      assert(Range >= NumCases);

      if (TLI->isSuitableForJumpTable(SI, NumCases, Range, PSI, BFI)) {
        unsigned NumPartitions = 1 + (j == N - 1 ? 0 : MinPartitions[j + 1]);
        unsigned Score = j == N - 1 ? 0 : PartitionsScore[j + 1];
        int64_t NumEntries = j - i + 1;

        if (NumEntries == 1)
          Score += PartitionScores::SingleCase;
        else if (NumEntries <= SmallNumberOfEntries)
          Score += PartitionScores::FewCases;
        else if (NumEntries >= MinJumpTableEntries)
          Score += PartitionScores::Table;

        // If this leads to fewer partitions, or to the same number of
        // partitions with better score, it is a better partitioning.
        if (NumPartitions < MinPartitions[i] ||
            (NumPartitions == MinPartitions[i] && Score > PartitionsScore[i])) {
          MinPartitions[i] = NumPartitions;
          LastElement[i] = j;
          PartitionsScore[i] = Score;
        }
      }
    }
  }

  // Iterate over the partitions, replacing some with jump tables in-place.
  unsigned DstIndex = 0;
  for (unsigned First = 0, Last; First < N; First = Last + 1) {
    Last = LastElement[First];
    assert(Last >= First);
    assert(DstIndex <= First);
    unsigned NumClusters = Last - First + 1;

    CaseCluster JTCluster;
    if (NumClusters >= MinJumpTableEntries &&
        buildJumpTable(Clusters, First, Last, SI, DefaultMBB, JTCluster)) {
      Clusters[DstIndex++] = JTCluster;
    } else {
      for (unsigned I = First; I <= Last; ++I)
        std::memmove(&Clusters[DstIndex++], &Clusters[I], sizeof(Clusters[I]));
    }
  }
  Clusters.resize(DstIndex);
}

bool SwitchCG::SwitchLowering::buildJumpTable(const CaseClusterVector &Clusters,
                                              unsigned First, unsigned Last,
                                              const SwitchInst *SI,
                                              MachineBasicBlock *DefaultMBB,
                                              CaseCluster &JTCluster) {
  assert(First <= Last);

  auto Prob = BranchProbability::getZero();
  unsigned NumCmps = 0;
  std::vector<MachineBasicBlock*> Table;
  DenseMap<MachineBasicBlock*, BranchProbability> JTProbs;

  // Initialize probabilities in JTProbs.
  for (unsigned I = First; I <= Last; ++I)
    JTProbs[Clusters[I].MBB] = BranchProbability::getZero();

  for (unsigned I = First; I <= Last; ++I) {
    assert(Clusters[I].Kind == CC_Range);
    Prob += Clusters[I].Prob;
    const APInt &Low = Clusters[I].Low->getValue();
    const APInt &High = Clusters[I].High->getValue();
    NumCmps += (Low == High) ? 1 : 2;
    if (I != First) {
      // Fill the gap between this and the previous cluster.
      const APInt &PreviousHigh = Clusters[I - 1].High->getValue();
      assert(PreviousHigh.slt(Low));
      uint64_t Gap = (Low - PreviousHigh).getLimitedValue() - 1;
      for (uint64_t J = 0; J < Gap; J++)
        Table.push_back(DefaultMBB);
    }
    uint64_t ClusterSize = (High - Low).getLimitedValue() + 1;
    for (uint64_t J = 0; J < ClusterSize; ++J)
      Table.push_back(Clusters[I].MBB);
    JTProbs[Clusters[I].MBB] += Clusters[I].Prob;
  }

  unsigned NumDests = JTProbs.size();
  if (TLI->isSuitableForBitTests(NumDests, NumCmps,
                                 Clusters[First].Low->getValue(),
                                 Clusters[Last].High->getValue(), *DL)) {
    // Clusters[First..Last] should be lowered as bit tests instead.
    return false;
  }

  // Create the MBB that will load from and jump through the table.
  // Note: We create it here, but it's not inserted into the function yet.
  MachineFunction *CurMF = FuncInfo.MF;
  MachineBasicBlock *JumpTableMBB =
      CurMF->CreateMachineBasicBlock(SI->getParent());

  // Add successors. Note: use table order for determinism.
  SmallPtrSet<MachineBasicBlock *, 8> Done;
  for (MachineBasicBlock *Succ : Table) {
    if (Done.count(Succ))
      continue;
    addSuccessorWithProb(JumpTableMBB, Succ, JTProbs[Succ]);
    Done.insert(Succ);
  }
  JumpTableMBB->normalizeSuccProbs();

  unsigned JTI = CurMF->getOrCreateJumpTableInfo(TLI->getJumpTableEncoding())
                     ->createJumpTableIndex(Table);

  // Set up the jump table info.
  JumpTable JT(-1U, JTI, JumpTableMBB, nullptr);
  JumpTableHeader JTH(Clusters[First].Low->getValue(),
                      Clusters[Last].High->getValue(), SI->getCondition(),
                      nullptr, false);
  JTCases.emplace_back(std::move(JTH), std::move(JT));

  JTCluster = CaseCluster::jumpTable(Clusters[First].Low, Clusters[Last].High,
                                     JTCases.size() - 1, Prob);
  return true;
}

void SwitchCG::SwitchLowering::findBitTestClusters(CaseClusterVector &Clusters,
                                                   const SwitchInst *SI) {
  // Partition Clusters into as few subsets as possible, where each subset has a
  // range that fits in a machine word and has <= 3 unique destinations.

#ifndef NDEBUG
  // Clusters must be sorted and contain Range or JumpTable clusters.
  assert(!Clusters.empty());
  assert(Clusters[0].Kind == CC_Range || Clusters[0].Kind == CC_JumpTable);
  for (const CaseCluster &C : Clusters)
    assert(C.Kind == CC_Range || C.Kind == CC_JumpTable);
  for (unsigned i = 1; i < Clusters.size(); ++i)
    assert(Clusters[i-1].High->getValue().slt(Clusters[i].Low->getValue()));
#endif

  // The algorithm below is not suitable for -O0.
  if (TM->getOptLevel() == CodeGenOpt::None)
    return;

  // If target does not have legal shift left, do not emit bit tests at all.
  EVT PTy = TLI->getPointerTy(*DL);
  if (!TLI->isOperationLegal(ISD::SHL, PTy))
    return;

  int BitWidth = PTy.getSizeInBits();
  const int64_t N = Clusters.size();

  // MinPartitions[i] is the minimum nbr of partitions of Clusters[i..N-1].
  SmallVector<unsigned, 8> MinPartitions(N);
  // LastElement[i] is the last element of the partition starting at i.
  SmallVector<unsigned, 8> LastElement(N);

  // FIXME: This might not be the best algorithm for finding bit test clusters.

  // Base case: There is only one way to partition Clusters[N-1].
  MinPartitions[N - 1] = 1;
  LastElement[N - 1] = N - 1;

  // Note: loop indexes are signed to avoid underflow.
  for (int64_t i = N - 2; i >= 0; --i) {
    // Find optimal partitioning of Clusters[i..N-1].
    // Baseline: Put Clusters[i] into a partition on its own.
    MinPartitions[i] = MinPartitions[i + 1] + 1;
    LastElement[i] = i;

    // Search for a solution that results in fewer partitions.
    // Note: the search is limited by BitWidth, reducing time complexity.
    for (int64_t j = std::min(N - 1, i + BitWidth - 1); j > i; --j) {
      // Try building a partition from Clusters[i..j].

      // Check the range.
      if (!TLI->rangeFitsInWord(Clusters[i].Low->getValue(),
                                Clusters[j].High->getValue(), *DL))
        continue;

      // Check nbr of destinations and cluster types.
      // FIXME: This works, but doesn't seem very efficient.
      bool RangesOnly = true;
      BitVector Dests(FuncInfo.MF->getNumBlockIDs());
      for (int64_t k = i; k <= j; k++) {
        if (Clusters[k].Kind != CC_Range) {
          RangesOnly = false;
          break;
        }
        Dests.set(Clusters[k].MBB->getNumber());
      }
      if (!RangesOnly || Dests.count() > 3)
        break;

      // Check if it's a better partition.
      unsigned NumPartitions = 1 + (j == N - 1 ? 0 : MinPartitions[j + 1]);
      if (NumPartitions < MinPartitions[i]) {
        // Found a better partition.
        MinPartitions[i] = NumPartitions;
        LastElement[i] = j;
      }
    }
  }

  // Iterate over the partitions, replacing with bit-test clusters in-place.
  unsigned DstIndex = 0;
  for (unsigned First = 0, Last; First < N; First = Last + 1) {
    Last = LastElement[First];
    assert(First <= Last);
    assert(DstIndex <= First);

    CaseCluster BitTestCluster;
    if (buildBitTests(Clusters, First, Last, SI, BitTestCluster)) {
      Clusters[DstIndex++] = BitTestCluster;
    } else {
      size_t NumClusters = Last - First + 1;
      std::memmove(&Clusters[DstIndex], &Clusters[First],
                   sizeof(Clusters[0]) * NumClusters);
      DstIndex += NumClusters;
    }
  }
  Clusters.resize(DstIndex);
}

bool SwitchCG::SwitchLowering::buildBitTests(CaseClusterVector &Clusters,
                                             unsigned First, unsigned Last,
                                             const SwitchInst *SI,
                                             CaseCluster &BTCluster) {
  assert(First <= Last);
  if (First == Last)
    return false;

  BitVector Dests(FuncInfo.MF->getNumBlockIDs());
  unsigned NumCmps = 0;
  for (int64_t I = First; I <= Last; ++I) {
    assert(Clusters[I].Kind == CC_Range);
    Dests.set(Clusters[I].MBB->getNumber());
    NumCmps += (Clusters[I].Low == Clusters[I].High) ? 1 : 2;
  }
  unsigned NumDests = Dests.count();

  APInt Low = Clusters[First].Low->getValue();
  APInt High = Clusters[Last].High->getValue();
  assert(Low.slt(High));

  if (!TLI->isSuitableForBitTests(NumDests, NumCmps, Low, High, *DL))
    return false;

  APInt LowBound;
  APInt CmpRange;

  const int BitWidth = TLI->getPointerTy(*DL).getSizeInBits();
  assert(TLI->rangeFitsInWord(Low, High, *DL) &&
         "Case range must fit in bit mask!");

  // Check if the clusters cover a contiguous range such that no value in the
  // range will jump to the default statement.
  bool ContiguousRange = true;
  for (int64_t I = First + 1; I <= Last; ++I) {
    if (Clusters[I].Low->getValue() != Clusters[I - 1].High->getValue() + 1) {
      ContiguousRange = false;
      break;
    }
  }

  if (Low.isStrictlyPositive() && High.slt(BitWidth)) {
    // Optimize the case where all the case values fit in a word without having
    // to subtract minValue. In this case, we can optimize away the subtraction.
    LowBound = APInt::getZero(Low.getBitWidth());
    CmpRange = High;
    ContiguousRange = false;
  } else {
    LowBound = Low;
    CmpRange = High - Low;
  }

  CaseBitsVector CBV;
  auto TotalProb = BranchProbability::getZero();
  for (unsigned i = First; i <= Last; ++i) {
    // Find the CaseBits for this destination.
    unsigned j;
    for (j = 0; j < CBV.size(); ++j)
      if (CBV[j].BB == Clusters[i].MBB)
        break;
    if (j == CBV.size())
      CBV.push_back(
          CaseBits(0, Clusters[i].MBB, 0, BranchProbability::getZero()));
    CaseBits *CB = &CBV[j];

    // Update Mask, Bits and ExtraProb.
    uint64_t Lo = (Clusters[i].Low->getValue() - LowBound).getZExtValue();
    uint64_t Hi = (Clusters[i].High->getValue() - LowBound).getZExtValue();
    assert(Hi >= Lo && Hi < 64 && "Invalid bit case!");
    CB->Mask |= (-1ULL >> (63 - (Hi - Lo))) << Lo;
    CB->Bits += Hi - Lo + 1;
    CB->ExtraProb += Clusters[i].Prob;
    TotalProb += Clusters[i].Prob;
  }

  BitTestInfo BTI;
  llvm::sort(CBV, [](const CaseBits &a, const CaseBits &b) {
    // Sort by probability first, number of bits second, bit mask third.
    if (a.ExtraProb != b.ExtraProb)
      return a.ExtraProb > b.ExtraProb;
    if (a.Bits != b.Bits)
      return a.Bits > b.Bits;
    return a.Mask < b.Mask;
  });

  for (auto &CB : CBV) {
    MachineBasicBlock *BitTestBB =
        FuncInfo.MF->CreateMachineBasicBlock(SI->getParent());
    BTI.push_back(BitTestCase(CB.Mask, BitTestBB, CB.BB, CB.ExtraProb));
  }
  BitTestCases.emplace_back(std::move(LowBound), std::move(CmpRange),
                            SI->getCondition(), -1U, MVT::Other, false,
                            ContiguousRange, nullptr, nullptr, std::move(BTI),
                            TotalProb);

  BTCluster = CaseCluster::bitTests(Clusters[First].Low, Clusters[Last].High,
                                    BitTestCases.size() - 1, TotalProb);
  return true;
}

void SwitchCG::sortAndRangeify(CaseClusterVector &Clusters) {
#ifndef NDEBUG
  for (const CaseCluster &CC : Clusters)
    assert(CC.Low == CC.High && "Input clusters must be single-case");
#endif

  llvm::sort(Clusters, [](const CaseCluster &a, const CaseCluster &b) {
    return a.Low->getValue().slt(b.Low->getValue());
  });

  // Merge adjacent clusters with the same destination.
  const unsigned N = Clusters.size();
  unsigned DstIndex = 0;
  for (unsigned SrcIndex = 0; SrcIndex < N; ++SrcIndex) {
    CaseCluster &CC = Clusters[SrcIndex];
    const ConstantInt *CaseVal = CC.Low;
    MachineBasicBlock *Succ = CC.MBB;

    if (DstIndex != 0 && Clusters[DstIndex - 1].MBB == Succ &&
        (CaseVal->getValue() - Clusters[DstIndex - 1].High->getValue()) == 1) {
      // If this case has the same successor and is a neighbour, merge it into
      // the previous cluster.
      Clusters[DstIndex - 1].High = CaseVal;
      Clusters[DstIndex - 1].Prob += CC.Prob;
    } else {
      std::memmove(&Clusters[DstIndex++], &Clusters[SrcIndex],
                   sizeof(Clusters[SrcIndex]));
    }
  }
  Clusters.resize(DstIndex);
}

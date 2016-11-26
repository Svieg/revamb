/// \file reachingdefinitions.cpp
/// \brief Implementation of the ReachingDefinitionsPass

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// Standard includes
#include <array>
#include <cstdint>
#include <iomanip>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

// LLVM includes
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"

// Local includes
#include "datastructures.h"
#include "debug.h"
#include "ir-helpers.h"
#include "reachingdefinitions.h"

// #include "valgrind/callgrind.h"

using namespace llvm;

using std::pair;
using std::queue;
using std::set;
using std::tie;
using std::unordered_map;
using std::vector;

using IndexesVector = SmallVector<int32_t, 2>;

template<class BBI, ReachingDefinitionsResult R>
const vector<LoadInst *> &
ReachingDefinitionsImplPass<BBI, R>::getReachedLoads(Instruction *Definition) {
  assert(R == ReachingDefinitionsResult::ReachedLoads);
  return ReachedLoads[Definition];
}

template<class BBI, ReachingDefinitionsResult R>
const vector<Instruction *> &
ReachingDefinitionsImplPass<BBI, R>::getReachingDefinitions(LoadInst *Load) {
  return ReachingDefinitions[Load];
}

template<class B, ReachingDefinitionsResult R>
unsigned
ReachingDefinitionsImplPass<B, R>::getReachingDefinitionsCount(LoadInst *Load) {
  assert(R == ReachingDefinitionsResult::ReachedLoads);
  return ReachingDefinitionsCount[Load];
}

using RDP = ReachingDefinitionsResult;
template class ReachingDefinitionsImplPass<BasicBlockInfo,
                                           RDP::ReachingDefinitions>;
template class ReachingDefinitionsImplPass<BasicBlockInfo,
                                           RDP::ReachedLoads>;

template<class BBI, ReachingDefinitionsResult R>
char ReachingDefinitionsImplPass<BBI, R>::ID = 0;

static RegisterPass<ReachingDefinitionsPass> X1("rdp",
                                                "Reaching Definitions Pass",
                                                true,
                                                true);

static RegisterPass<ReachedLoadsPass> X2("rlp",
                                         "Reaching Definitions Pass",
                                         true,
                                         true);

// ReachingDefinitionsPass methods implementation

template<>
const IndexesVector &
ReachingDefinitionsPass::getDefinedConditions(BasicBlock *BB) {
  return ConditionNumberingPass::NoDefinedConditions;
}

template<>
int32_t ReachingDefinitionsPass::getConditionIndex(TerminatorInst *V) {
  return 0;
}

template<>
void ReachingDefinitionsPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
}

// ReachedLoadsPass methods implementations

template<>
const IndexesVector &
ReachedLoadsPass::getDefinedConditions(BasicBlock *BB) {
  return ConditionNumberingPass::NoDefinedConditions;
}

template<>
int32_t ReachedLoadsPass::getConditionIndex(TerminatorInst *V) {
  return 0;
}

template<>
void ReachedLoadsPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
}

template class ReachingDefinitionsImplPass<ConditionalBasicBlockInfo,
                                           RDP::ReachingDefinitions>;
template class ReachingDefinitionsImplPass<ConditionalBasicBlockInfo,
                                           RDP::ReachedLoads>;

static RegisterPass<ConditionalReachingDefinitionsPass> Y1("crdp",
                                                           "Conditional"
                                                           " Reaching"
                                                           " Definitions Pass",
                                                           true,
                                                           true);

static RegisterPass<ConditionalReachedLoadsPass> Y2("crlp",
                                                    "Conditional"
                                                    " Reaching"
                                                    " Definitions Pass",
                                                    true,
                                                    true);

// ConditionalReachingDefinitionsPass methods implementations

template<>
const IndexesVector &
ConditionalReachingDefinitionsPass::getDefinedConditions(BasicBlock *BB) {
  return getAnalysis<ConditionNumberingPass>().getDefinedConditions(BB);
}

// TODO: this duplication sucks
template<>
int32_t
ConditionalReachingDefinitionsPass::getConditionIndex(TerminatorInst *T) {
  auto *Branch = dyn_cast<BranchInst>(T);
  if (Branch == nullptr || !Branch->isConditional())
    return 0;

  return getAnalysis<ConditionNumberingPass>().getConditionIndex(T);
}

template<>
void
ConditionalReachingDefinitionsPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<ConditionNumberingPass>();
}

template<>
int32_t
ConditionalReachedLoadsPass::getConditionIndex(TerminatorInst *T) {
  auto *Branch = dyn_cast<BranchInst>(T);
  if (Branch == nullptr || !Branch->isConditional())
    return 0;

  return getAnalysis<ConditionNumberingPass>().getConditionIndex(T);
}

// ConditionalReachedLoadsPass methods implementation

template<>
const IndexesVector &
ConditionalReachedLoadsPass::getDefinedConditions(BasicBlock *BB) {
  return getAnalysis<ConditionNumberingPass>().getDefinedConditions(BB);
}

template<>
void
ConditionalReachedLoadsPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<ConditionNumberingPass>();
}

static size_t combine(size_t A, size_t B) {
  return (A << 1 | A >> 31) ^ B;
}

static size_t combine(size_t A, void *Ptr) {
  return combine(A, reinterpret_cast<intptr_t>(Ptr));
}

static bool isSupportedOperator(unsigned Opcode) {
  switch (Opcode) {
  case Instruction::Xor:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::ICmp:
    return true;
  default:
    return false;
  }
}

class ConditionHash {
public:
  ConditionHash(ReachingDefinitionsPass &RDP) : RDP(RDP) { }

  size_t operator()(BranchInst * const& V) const;

private:
  ReachingDefinitionsPass &RDP;
};

size_t ConditionHash::operator()(BranchInst * const& B) const {
  Value *V = B->getCondition();
  size_t Hash = 0;
  queue<Value *> WorkList;
  WorkList.push(V);
  while (!WorkList.empty()) {
    Value *V;
    V = WorkList.front();
    WorkList.pop();

    bool IsStore = isa<StoreInst>(V);
    bool IsLoad = isa<LoadInst>(V);
    if (IsStore || IsLoad) {
      // Load/store vs load/store
      if (IsStore) {
        Hash = combine(Hash, cast<StoreInst>(V)->getPointerOperand());
      } else {
        for (Instruction *I : RDP.getReachingDefinitions(cast<LoadInst>(V))) {
          if (auto *Store = dyn_cast<StoreInst>(I))
            Hash = combine(Hash, Store->getPointerOperand());
          else if (auto *Load = dyn_cast<LoadInst>(I))
            Hash = combine(Hash, Load->getPointerOperand());
        }
      }
    } else if (auto *I = dyn_cast<Instruction>(V)) {
      // Instruction
      if (!isSupportedOperator(I->getOpcode())) {
        Hash = combine(Hash, V);
      } else {
        Hash = combine(Hash, I->getOpcode());
        Hash = combine(Hash, I->getNumOperands());
        for (unsigned C = 0; C < I->getNumOperands(); C++)
          WorkList.push(I->getOperand(C));
      }
    } else {
      Hash = combine(Hash, V);
    }

  }

  return Hash;
}

class ConditionEqualTo {
public:
  ConditionEqualTo(ReachingDefinitionsPass &RDP) : RDP(RDP) { }

  bool operator()(BranchInst * const& A, BranchInst * const& B) const;

private:
  ReachingDefinitionsPass &RDP;
};

bool ConditionEqualTo::operator()(BranchInst * const& BA,
                                  BranchInst * const& BB) const {
  Value *A = BA->getCondition();
  Value *B = BB->getCondition();
  queue<pair<Value *, Value *>> WorkList;
  WorkList.push({A, B});
  while (!WorkList.empty()) {
    Value *AV, *BV;
    tie(AV, BV) = WorkList.front();
    WorkList.pop();

    // Early continue in case they're exactly the same value
    if (AV == BV)
      continue;

    bool AIsStore = isa<StoreInst>(AV);
    bool AIsLoad = isa<LoadInst>(AV);
    bool BIsStore = isa<StoreInst>(BV);
    bool BIsLoad = isa<LoadInst>(BV);
    if ((AIsStore || AIsLoad) && (BIsStore || BIsLoad)) {
      // Load/store vs load/store
      vector<Instruction *> AStores;
      if (AIsStore)
        AStores.push_back(cast<StoreInst>(AV));
      else
        AStores = RDP.getReachingDefinitions(cast<LoadInst>(AV));

      vector<Instruction *> BStores;
      if (BIsStore)
        BStores.push_back(cast<StoreInst>(BV));
      else
        BStores = RDP.getReachingDefinitions(cast<LoadInst>(BV));

      if (AStores != BStores)
        return false;
    } else if (auto *AI = dyn_cast<Instruction>(AV)) {
      // Instruction
      auto *BI = dyn_cast<Instruction>(BV);
      if (BI == nullptr
          || AI->getOpcode() != BI->getOpcode()
          || AI->getNumOperands() != BI->getNumOperands()
          || !isSupportedOperator(AI->getOpcode()))
        return false;

      for (unsigned I = 0; I < AI->getNumOperands(); I++)
        WorkList.push({ AI->getOperand(I), BI->getOperand(I) });
    } else {
      return false;
    }

  }
  return true;
}

static SmallSet<BasicBlock *, 2>
definingBasicBlocks(ReachingDefinitionsPass &RDP, BranchInst * const& Branch) {
  SmallSet<BasicBlock *, 2> Result;
  Value *A = Branch->getCondition();
  queue<Value *> WorkList;
  WorkList.push(A);
  while (!WorkList.empty()) {
    Value *AV;
    AV = WorkList.front();
    WorkList.pop();

    bool AIsStore = isa<StoreInst>(AV);
    bool AIsLoad = isa<LoadInst>(AV);
    if (AIsStore || AIsLoad) {
      // Load/store vs load/store
      vector<Instruction *> AStores;
      if (AIsStore)
        Result.insert(cast<StoreInst>(AV)->getParent());
      else
        for (Instruction *I : RDP.getReachingDefinitions(cast<LoadInst>(AV)))
          Result.insert(I->getParent());

    } else if (auto *AI = dyn_cast<Instruction>(AV)) {
      // Instruction
      if (!isSupportedOperator(AI->getOpcode()))
        return { };

      for (unsigned I = 0; I < AI->getNumOperands(); I++)
        WorkList.push(AI->getOperand(I));
    } else if (!isa<Constant>(AV)) {
      return { };
    }

  }
  return Result;
}

char ConditionNumberingPass::ID = 0;
const IndexesVector ConditionNumberingPass::NoDefinedConditions;
static RegisterPass<ConditionNumberingPass> Z("cnp",
                                              "Condition Numbering Pass",
                                              true,
                                              true);
template<typename C, typename T>
static bool pushIfAbsent(C &Container, T Element) {
  auto It = std::find(Container.begin(), Container.end(), Element);
  bool Result = It != Container.end();
  if (!Result)
    Container.push_back(Element);
  return Result;
}

bool ConditionNumberingPass::runOnFunction(Function &F) {
  DBG("passes", { dbg << "Starting ConditionNumberingPass\n"; });

  auto &RDP = getAnalysis<ReachingDefinitionsPass>();
  unordered_map<BranchInst *,
                SmallVector<BranchInst *, 1>,
                ConditionHash,
                ConditionEqualTo> Conditions(10,
                                             ConditionHash(RDP),
                                             ConditionEqualTo(RDP));

  // Group conditions together
  for (BasicBlock &BB : F)
    if (auto *Branch = dyn_cast<BranchInst>(BB.getTerminator()))
      if (Branch->isConditional())
        Conditions[Branch].push_back(Branch);

  // Save the interesting results
  uint32_t ConditionIndex = 0;
  for (auto &P : Conditions) {
    if (P.second.size() > 1) {
      // 0 is a reserved value
      ConditionIndex++;
      for (BranchInst *B : P.second) {
        BranchConditionNumberMap[B] = ConditionIndex;
        for (BasicBlock *Definer : definingBasicBlocks(RDP, B)) {
          pushIfAbsent(DefinedConditions[Definer], ConditionIndex);
        }
      }

      DBG("cnp",
          {
            dbg << std::dec << ConditionIndex << ":";
            for (BranchInst *B : P.second)
              dbg << " " << getName(B);

            auto It = P.second.begin();
            if (It != P.second.end()) {
              dbg << " (defined by:";
              for (BasicBlock *Definer : definingBasicBlocks(RDP, *It)) {
                dbg << " " << getName(Definer);
              }
              dbg << ")";
            }
            dbg << "\n";
          });

    }
  }

  DBG("passes", { dbg << "Ending ConditionNumberingPass\n"; });
  return false;
}

void BasicBlockInfo::dump(std::ostream& Output) {
  set<Instruction *> Printed;
  for (const MemoryInstruction &MI : Reaching) {
    Instruction *V = MI.I;
    if (Printed.count(V) == 0) {
      Printed.insert(V);
      Output << " " << getName(V);
    }
  }
}

void BasicBlockInfo::newDefinition(StoreInst *Store, TypeSizeProvider &TSP) {
  // Remove all the aliased reaching definitions
  MemoryAccess TargetMA(Store, TSP);
  removeDefinitions([&TargetMA] (MemoryInstruction &MI) {
      return TargetMA.mayAlias(MI.MA);
    });

  // Add this definition
  Definitions.push_back(MemoryInstruction(Store, TSP));
}

LoadDefinitionType BasicBlockInfo::newDefinition(LoadInst *Load,
                                                 TypeSizeProvider &TSP) {
  LoadDefinitionType Result = NoReachingDefinitions;

  // Check if it's a self-referencing load
  MemoryAccess TargetMA(Load, TSP);
  for (auto &MI : Definitions) {
    auto *Definition = MI.I;
    if (Definition == Load) {
      // It's self-referencing, suppress all the matching loads
      removeDefinitions([&TargetMA] (MemoryInstruction &MI) {
          return isa<LoadInst>(MI.I) && TargetMA == MI.MA;
        });
      Result = SelfReaching;
      break;
    } else if (TargetMA == MI.MA) {
      Result = HasReachingDefinitions;
    }
  }

  // Add this definition
  if (Result == NoReachingDefinitions)
    Definitions.push_back(MemoryInstruction(Load, TSP));

  return Result;
}

bool BasicBlockInfo::propagateTo(BasicBlockInfo &Target,
                                 TypeSizeProvider &TSP,
                                 const IndexesVector &,
                                 int32_t NewConditionIndex) {
  bool Changed = false;
  for (MemoryInstruction &Definition : Definitions)
    Changed |= Target.Reaching.insert(Definition).second;

  return Changed;
}

vector<pair<Instruction *, MemoryAccess>>
BasicBlockInfo::getReachingDefinitions(set<LoadInst *> &WhiteList,
                                       TypeSizeProvider &TSP) {
  vector<pair<Instruction *, MemoryAccess>> Result;
  for (const MemoryInstruction &MI : Reaching) {
    Instruction *I = MI.I;
    if (auto *Load = dyn_cast<LoadInst>(I)) {
      // If it's a load check it's whitelisted
      if (WhiteList.count(Load) != 0)
        Result.push_back({ Load, MI.MA });
    } else {
      // It's a store
      Result.push_back({ I, MI.MA });
    }
  }

  freeContainer(Reaching);
  assert(Reaching.size() == 0);

  return Result;
}

void ConditionalBasicBlockInfo::dump(std::ostream& Output) {
  set<Instruction *> Printed;
  for (auto &P : Reaching) {
    Instruction *I = P.first.I;
    if (Printed.count(I) == 0) {
      Printed.insert(I);
      Output << " " << getName(I);
    }
  }
}

void ConditionalBasicBlockInfo::newDefinition(StoreInst *Store,
                                              TypeSizeProvider &TSP) {
  // Remove all the aliased reaching definitions
  MemoryAccess TargetMA(Store, TSP);
  removeDefinitions([&TargetMA] (CondDefPair &P) {
      // TODO: don't erase if conditions are complementary
      return TargetMA.mayAlias(P.second.MA);
    });

  // Perform the merge
  // Note that the new definition absorbes all the conditions holding in the
  // current basic block
  mergeDefinition({ Conditions, MemoryInstruction(Store, TSP) },
                  Definitions,
                  TSP);
}

LoadDefinitionType
ConditionalBasicBlockInfo::newDefinition(LoadInst *Load,
                                         TypeSizeProvider &TSP) {
  LoadDefinitionType Result = NoReachingDefinitions;

  // Check if it's a self-referencing load
  MemoryAccess TargetMA(Load, TSP);
  for (auto &P : Definitions) {
    auto *Definition = P.second.I;
    if (Definition == Load) {
      // It's self-referencing, suppress all the matching loads
      removeDefinitions([&TargetMA] (CondDefPair &P) {
          // TODO: can we embed if it's a load or a store in
          //       MemoryInstruction?
          return isa<LoadInst>(P.second.I) && P.second.MA == TargetMA;
        });
      Result = SelfReaching;
      break;
    } else if (TargetMA == P.second.MA) {
      Result = HasReachingDefinitions;
    }
  }

  // Add this definition
  if (Result == NoReachingDefinitions)
    mergeDefinition({ Conditions, MemoryInstruction(Load, TSP) },
                    Definitions,
                    TSP);

  return Result;
}

vector<pair<Instruction *, MemoryAccess>>
ConditionalBasicBlockInfo::getReachingDefinitions(set<LoadInst *> &WhiteList,
                                                  TypeSizeProvider &TSP) {
  vector<pair<Instruction *, MemoryAccess>> Result;
  for (auto &P : Reaching) {
    Instruction *I = P.first.I;
    if (auto *Load = dyn_cast<LoadInst>(I)) {
      // If it's a load check it's whitelisted
      if (WhiteList.count(Load) != 0)
        Result.push_back({ Load, P.first.MA });
    } else {
      // It's a store
      Result.push_back({ I, P.first.MA });
    }
  }

  freeContainer(Reaching);

  return Result;
}

bool ConditionalBasicBlockInfo::setIndexIfSeen(BitVector &Target,
                                               int32_t Index) const {
  auto ConditionIt = std::find(SeenConditions.begin(),
                               SeenConditions.end(),
                               Index);

  // If present set the corresponding bit in Defined
  if (ConditionIt != SeenConditions.end()) {
    Target.set(ConditionIt - SeenConditions.begin());
    return true;
  }

  return false;
}

bool
ConditionalBasicBlockInfo::propagateTo(ConditionalBasicBlockInfo &Target,
                                       TypeSizeProvider &TSP,
                                       const IndexesVector &DefinedIndexes,
                                       int32_t NewConditionIndex) {
  bool Changed = false;

  // Get (and insert, if necessary) the bit associated to the new
  // condition. This bit will be set in all the defintions being propagated.
  DBG("rdp-propagation", dbg << "  Adding conditions:");
  unsigned NewConditionBitIndex = Target.getConditionIndex(NewConditionIndex);
  if (NewConditionIndex != 0 && !Target.Conditions[NewConditionBitIndex]) {
    Target.Conditions.set(NewConditionBitIndex);
    DBG("rdp-propagation", dbg << " " << NewConditionIndex);
    Changed = true;
  }

  // Condition propgation
  for (int SetBitIndex = Conditions.find_first();
       SetBitIndex != -1;
       SetBitIndex = Conditions.find_next(SetBitIndex)) {
    int32_t ToPropagate = SeenConditions[SetBitIndex];

    // Do not propagate the condition if:
    //
    // * it's defined in the target basic block
    // * it's the condition associated to the current branch
    // * the target basic block already has it
    auto It = std::find_if(DefinedIndexes.begin(),
                           DefinedIndexes.end(),
                           [ToPropagate] (int32_t Defined) {
                             return Defined == ToPropagate
                             || Defined == -ToPropagate;
                           });

    if (ToPropagate != NewConditionIndex
        && ToPropagate != -NewConditionIndex
        && It == DefinedIndexes.end()
        && !Target.hasCondition(ToPropagate)) {
      Target.addCondition(ToPropagate);
      DBG("rdp-propagation", dbg << "  " << ToPropagate);
      Changed = true;
    }

  }
  DBG("rdp-propagation", dbg << "\n");

  // Compute a bit vector with all the conditions that are incompatible with the
  // target
  BitVector Banned(SeenConditions.size());
  DBG("rdp-propagation", dbg << "  Banned conditions:");

  // For each set bit in the target's conditions
  for (int SetBitIndex = Target.Conditions.find_first();
       SetBitIndex != -1;
       SetBitIndex = Target.Conditions.find_next(SetBitIndex)) {

    // Consider the opposite condition as banned
    int32_t BannedIndex = -Target.SeenConditions[SetBitIndex];
    DBG("rdp-propagation", dbg << " " << BannedIndex);

    // Check BannedIndex is not explicitly allowed
    auto BannedIt = std::find(Target.SeenConditions.begin(),
                              Target.SeenConditions.end(),
                              BannedIndex);
    bool IsAllowed = BannedIt != Target.SeenConditions.end()
      && Target.Conditions[BannedIt - Target.SeenConditions.begin()];
    if (!IsAllowed)
      setIndexIfSeen(Banned, BannedIndex);

  }

  DBG("rdp-propagation", dbg << "\n");

  // Create a BitVector for conditions defined in the target basic block, so
  // that we can later exclude them
  BitVector Defined(SeenConditions.size());
  for (int32_t DefinedIndex : DefinedIndexes) {
    setIndexIfSeen(Defined, DefinedIndex);
    setIndexIfSeen(Defined, -DefinedIndex);
  }
  BitVector NotDefined = Defined;
  NotDefined.flip();

  for (auto &Definition : Definitions) {
    BitVector DefinitionConditions = Definition.first;
    DBG("rdp-propagation", {
        dbg << "  Propagate " << getName(Definition.second.I);

        if (auto *Load = dyn_cast<LoadInst>(Definition.second.I))
          dbg << " about " << Load->getPointerOperand()->getName().str();
        else if (auto *Store = dyn_cast<StoreInst>(Definition.second.I))
          dbg << " about " << Store->getPointerOperand()->getName().str();

        if (DefinitionConditions.any()) {
          dbg << " (conditions:";
          for (int I = DefinitionConditions.find_first();
               I != -1;
               I = DefinitionConditions.find_next(I)) {
            dbg << " " << SeenConditions[I];
          }
          dbg << ")";
        }

        dbg << "? ";
      });

    // Reset all the conditions that are defined in the target basic block
    DefinitionConditions &= NotDefined;

    // Check if this definition is compatible with the target basic block
    auto BannedConditions = DefinitionConditions;
    BannedConditions &= Banned;
    if (BannedConditions.any()) {
      DBG("rdp-propagation", dbg << "no\n");
      continue;
    }

    DBG("rdp-propagation", dbg << "yes\n");

    // Translate the conditions bitvector to the context of the target BBI
    BitVector Translated(Target.SeenConditions.size());

    for (int I = DefinitionConditions.find_first();
         I != -1;
         I = DefinitionConditions.find_next(I)) {
      // Make sure the target BBI knows about all the necessary conditinos
      assert(I < static_cast<int>(SeenConditions.size()));
      unsigned Index = Target.getConditionIndex(SeenConditions[I]);
      unsigned OppositeIndex = Target.getConditionIndex(-SeenConditions[I]);

      // Keep the size of the new bitvector in sync
      if (Target.SeenConditions.size() != Translated.size())
        Translated.resize(Target.SeenConditions.size());

      Translated.set(Index);
      Translated.reset(OppositeIndex);
  }

    // Add the condition of this branch
    if (NewConditionIndex != 0)
      Translated.set(NewConditionBitIndex);

    Changed |= Target.mergeDefinition({ Translated, Definition.second },
                                      Target.Reaching,
                                      TSP);
  }

  return Changed;
}

ConditionalBasicBlockInfo::ConditionsComparison
ConditionalBasicBlockInfo::mergeConditionBits(BitVector &Target,
                                              BitVector &NewConditions) const {
  // Find the different bits
  BitVector DifferentBits = Target;
  DifferentBits ^= NewConditions;

  // If they are identical, quit
  int FirstBit = DifferentBits.find_first();
  if (FirstBit == -1)
    return Identical;

  // Ensure we only have two non-zero bits
  int SecondBit = DifferentBits.find_next(FirstBit);
  if (SecondBit == -1 || DifferentBits.find_next(SecondBit) != -1)
    return Different;

  // Check if the only two different bits are complementary conditions
  if (SeenConditions[FirstBit] == -SeenConditions[SecondBit]) {
    NewConditions.reset(FirstBit);
    NewConditions.reset(SecondBit);
    return Complementary;
  } else {
    return Different;
  }
}

bool ConditionalBasicBlockInfo::mergeDefinition(CondDefPair NewDefinition,
                                                vector<CondDefPair> &Targets,
                                                TypeSizeProvider &TSP) const {
  BitVector &NewConditionsBV = NewDefinition.first;
  assert(NewConditionsBV.size() == SeenConditions.size());

  bool Again = false;
  bool Result = false;
  do {
    Again = false;
    for (auto TargetIt = Targets.begin();
         TargetIt != Targets.end();
         TargetIt++) {
      CondDefPair &Target = *TargetIt;
      // Note that we copy the BitVector, since we're going to modify it
      if (Target.second.I == NewDefinition.second.I) {
        switch (mergeConditionBits(Target.first, NewConditionsBV)) {
        case Identical:
          return Result;
        case Complementary:
          Targets.erase(TargetIt);
          Again = true;
          Result = true;
          break;
        case Different:
          break;
        }
      }
    }
  } while (Again);

  Targets.push_back(NewDefinition);

  return true;
}

bool ConditionalBasicBlockInfo::mergeDefinition(CondDefPair NewDefinition,
                                                ReachingType &Targets,
                                                TypeSizeProvider &TSP) const {
  BitVector &NewConditionsBV = NewDefinition.first;
  assert(NewConditionsBV.size() == SeenConditions.size());

  bool Again = false;
  bool Result = false;
  SmallVector<BitVector, 2> &BVs = Targets[NewDefinition.second];

  do {
    Again = false;
    for (auto TargetIt = BVs.begin(); TargetIt != BVs.end(); TargetIt++) {
      assert(TargetIt != BVs.end());
      assert(BVs.size() != 0);
      switch (mergeConditionBits(*TargetIt, NewConditionsBV)) {
      case Identical:
        return Result;
      case Complementary:
        BVs.erase(TargetIt);
        Again = true;
        Result = true;
        break;
      case Different:
        break;
      }

      if (Again)
        break;
    }
  } while (Again);

  BVs.push_back(NewDefinition.first);

  return true;
}

static bool isSupportedPointer(Value *V) {
  if (auto *Global = dyn_cast<GlobalVariable>(V))
    if (Global->getName() != "env")
      return true;

  if (isa<AllocaInst>(V))
    return true;

  return false;
}

template<class BBI, ReachingDefinitionsResult R>
bool ReachingDefinitionsImplPass<BBI, R>::runOnFunction(Function &F) {

  DBG("passes", {
      if (std::is_same<BBI, ConditionalBasicBlockInfo>::value)
        dbg << "Starting ConditionalReachingDefinitionsPass\n";
      else
        dbg << "Starting ReachingDefinitionsPass\n";
    });

  for (auto &BB : F) {
    if (!BB.empty()) {
      if (auto *Call = dyn_cast<CallInst>(&*BB.begin())) {
        Function *Callee = Call->getCalledFunction();
        // TODO: comparing with "newpc" string is sad
        if (Callee != nullptr && Callee->getName() == "newpc")
          break;
      }
    }
    BasicBlockBlackList.insert(&BB);
  }

  TypeSizeProvider TSP(F.getParent()->getDataLayout());

  // Initialize queue
  unsigned BasicBlockCount = 0;
  unsigned BasicBlockVisits = 0;
  ReversePostOrderTraversal<Function *> RPOT(&F);
  UniquedStack<BasicBlock *> ToVisit;
  for (BasicBlock *BB : RPOT) {
    ToVisit.insert(BB);
    BasicBlockCount++;
  }
  ToVisit.reverse();

  while (!ToVisit.empty()) {
    BasicBlockVisits++;
    BasicBlock *BB = ToVisit.pop();

    BBI &Info = DefinitionsMap[BB];
    Info.resetDefinitions(TSP);

    // Find all the definitions
    for (Instruction &I : *BB) {
      auto *Store = dyn_cast<StoreInst>(&I);
      auto *Load = dyn_cast<LoadInst>(&I);

      if (Store != nullptr
          && isSupportedPointer(Store->getPointerOperand())) {

        // Record new definition
        Info.newDefinition(Store, TSP);

      } else if (Load != nullptr
                 && isSupportedPointer(Load->getPointerOperand())) {

        // Check if it's a new definition and record it
        auto LoadType = Info.newDefinition(Load, TSP);
        switch (LoadType) {
        case NoReachingDefinitions:
          NRDLoads.insert(Load);
          break;
        case SelfReaching:
          SelfReachingLoads.insert(Load);
          break;
        case HasReachingDefinitions:
          NRDLoads.erase(Load);
          break;
        }

      }
    }

    bool IsCall = false;
    bool StorePCFound = false;
    SmallVector<uint64_t, 3> ConstantStores;
    auto It = BB->getTerminator()->getIterator();
    while (It != BB->begin()) {
      It--;
      Instruction *I = &*It;
      if (auto *Store = dyn_cast<StoreInst>(I)) {
        Value *V = Store->getValueOperand();
        if (Store->getPointerOperand()->getName() == "pc") {
          StorePCFound = true;
        } else if (auto *Constant = dyn_cast<ConstantInt>(V)) {
          ConstantStores.push_back(Constant->getLimitedValue());
        }
      } else if (auto *Call = dyn_cast<CallInst>(I)) {
        auto *Callee = Call->getCalledFunction();
        if (Callee != nullptr && Callee->getName() == "newpc") {
          uint64_t PC = getLimitedValue(Call->getArgOperand(0));
          uint64_t Size = getLimitedValue(Call->getArgOperand(1));
          auto RAIt = std::find(ConstantStores.begin(),
                                ConstantStores.end(),
                                PC + Size);
          IsCall = StorePCFound && RAIt != ConstantStores.end();
          break;
        }
      }
    }

    // TODO: this is an hack and should be replaced once we integrate calling
    //       convention and call graph in the basic block harvesting process
    unsigned SuccessorsCount = succ_end(BB) - succ_begin(BB);
    unsigned Size = Info.size();
    if (!IsCall && Size * SuccessorsCount <= 5000) {
      // Get the identifier of the conditional instruction
      int32_t ConditionIndex = getConditionIndex(BB->getTerminator());
      assert(ConditionIndex == 0 || ConditionIndex > 0);

      // Propagate definitions to successors, checking if actually we changed
      // something, and if so re-enqueue them
      for (BasicBlock *Successor : successors(BB)) {
        if (BasicBlockBlackList.count(Successor) != 0)
          continue;

        const IndexesVector &DefinedConditions =
          getDefinedConditions(Successor);

        BBI &SuccessorInfo = DefinitionsMap[Successor];

        DBG("rdp-propagation", {
            dbg << "Propagating from " << getName(BB)
                << " to " << getName(Successor);

            if (DefinedConditions.size() > 0) {
              dbg << " (defining conditions: ";
              for (int32_t ConditionIndex : DefinedConditions)
                dbg << " " << ConditionIndex;
              dbg << ")";
            }

            if (ConditionIndex != 0)
              dbg << ", using a " << ConditionIndex << " branch";

            dbg << "\n";
          });

        // Enqueue the successor only if the propagation actually did something
        unsigned Old = SuccessorInfo.size();
        if (Info.propagateTo(SuccessorInfo,
                             TSP,
                             DefinedConditions,
                             ConditionIndex))
          ToVisit.insert(Successor);

        DBG("rdp-propagation",
            dbg << getName(Successor) << std::dec
            << " got " << (SuccessorInfo.size() - Old) << " new reachers "
            << "from " << getName(BB) << " (had " << Old << ")\n");

        // Add the condition relative to the current branch instruction (if any)
        if (ConditionIndex != 0) {
          // If ConditionIndex is positive we're in the true branch, prepare
          // ConditionIndex for the false branch
          if (ConditionIndex > 0)
            ConditionIndex = -ConditionIndex;
        }

      }

      // We no longer need to keep track of the definitions
      Info.clearDefinitions();
    }
  }

  // Collect final information
  std::set<LoadInst *> &FreeLoads = NRDLoads;
  FreeLoads.insert(SelfReachingLoads.begin(), SelfReachingLoads.end());

  for (auto &P : DefinitionsMap) {
    BasicBlock *BB = P.first;
    BBI &Info = P.second;

    // TODO: use a list?
    vector<pair<Instruction *, MemoryAccess>> Definitions;
    Definitions = Info.getReachingDefinitions(FreeLoads, TSP);
    for (Instruction &I : *BB) {
      auto *Store = dyn_cast<StoreInst>(&I);
      auto *Load = dyn_cast<LoadInst>(&I);

      using IMP = pair<Instruction *, MemoryAccess>;
      if (Store != nullptr
          && isSupportedPointer(Store->getPointerOperand())) {

        // Remove all the reaching definitions aliased by this store
        MemoryAccess TargetMA(Store, TSP);
        erase_if(Definitions, [&TargetMA] (IMP &P) {
            return TargetMA.mayAlias(P.second);
          });
        Definitions.push_back({ Store, TargetMA });

      } else if (Load != nullptr
                 && isSupportedPointer(Load->getPointerOperand())) {

        // Record all the relevant reaching defininitions
        MemoryAccess TargetMA(Load, TSP);
        if (FreeLoads.count(Load) != 0) {

          // If it's a free load, remove all the matching loads
          erase_if(Definitions, [&TargetMA, &TSP] (IMP &P) {
              Instruction *I = P.first;
              return isa<LoadInst>(I) && MemoryAccess(I, TSP) == TargetMA;
            });

        } else {

          if (R == ReachingDefinitionsResult::ReachedLoads) {
            for (auto &Definition : Definitions) {
              if (TargetMA == Definition.second) {
                ReachedLoads[Definition.first].push_back(Load);
                ReachingDefinitionsCount[Load]++;
              }
            }
          }

          std::vector<Instruction *> LoadDefinitions;
          for (auto &Definition : Definitions)
            if (TargetMA == Definition.second)
              LoadDefinitions.push_back(Definition.first);

          // Save them in ReachingDefinitions
          std::sort(LoadDefinitions.begin(), LoadDefinitions.end());
          DBG("rdp",
              {
                dbg << getName(Load) << " is reached by:";
                for (auto *Definition : LoadDefinitions)
                  dbg << " " << getName(Definition);
                dbg << "\n";
              });
          ReachingDefinitions[Load] = std::move(LoadDefinitions);

        }

      }

    }
  }

  DBG("rdp",
      {
        dbg << "Basic blocks: " << std::dec << BasicBlockCount << "\n"
            << "Visited: " << std::dec << BasicBlockVisits << "\n"
            << "Average visits per basic block: " << std::setprecision(2)
            << float(BasicBlockVisits) / BasicBlockCount << "\n";
      });

  if (R == ReachingDefinitionsResult::ReachedLoads) {
    DBG("rdp",
        for (auto P : ReachedLoads) {
          dbg << getName(P.first) << " reaches";
          for (auto *Load : P.second)
            dbg << " " << getName(Load);
          dbg << "\n";
        });
  }

  // Clear all the temporary data that is not part of the analysis result
  freeContainer(DefinitionsMap);
  freeContainer(FreeLoads);
  freeContainer(BasicBlockBlackList);
  freeContainer(NRDLoads);
  freeContainer(SelfReachingLoads);

  DBG("passes", {
      if (std::is_same<BBI, ConditionalBasicBlockInfo>::value)
        dbg << "Ending ConditionalReachingDefinitionsPass\n";
      else
        dbg << "Ending ReachingDefinitionsPass\n";
    });

  return false;
}

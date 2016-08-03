/// \file simplifycomparison.cpp
/// \brief Implementation of the SimplifyComparisonPass

// Standard includes
#include <queue>
#include <tuple>

// LLVM includes
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"

// Local include
#include "debug.h"
#include "ir-helpers.h"
#include "simplifycomparison.h"

using namespace llvm;

char SimplifyComparisonPass::ID = 0;

using std::array;
using std::pair;
using std::queue;
using std::tie;
using std::tuple;
using std::unique_ptr;
using std::vector;
using Predicate = CmpInst::Predicate;

// TODO: expand this
static const array<tuple<unsigned, unsigned, Predicate>, 1> KnownTruthTables {
  { std::make_tuple(0b010110010U, 8U, CmpInst::ICMP_SGE) }
};

static const unsigned MaxDepth = 10;

/// \brief Base class for all the types of terms of a boolean expression
class Term {
public:
  virtual bool evaluate(unsigned Assignments) const { assert(false); }
};

/// \brief A free-operand term (a variable)
/// It has to be associated to the index of the variable
class VariableTerm : public Term {
public:
  VariableTerm(unsigned Index) : VariableIndex(Index) { }
  VariableTerm() : VariableIndex(0) { }

  virtual bool evaluate(unsigned Assignments) const override;

private:
  unsigned VariableIndex;
};

class BinaryTerm;

/// \brief Simple data structure associating a Term to a BinaryTerm operand
class TermUse {
public:
  TermUse(BinaryTerm *Op, unsigned OpIndex) : T(Op), OpIndex(OpIndex) { }
  TermUse() : T(nullptr), OpIndex(0) { }

  void set(Term *Operand);

private:
  BinaryTerm *T;
  unsigned OpIndex;
};

/// \brief Term representing a binary operation
class BinaryTerm : public Term {
public:
  BinaryTerm() : Opcode(0), Operands({ nullptr, nullptr }) {
  }

  BinaryTerm(unsigned Opcode) : Opcode(Opcode),
                                Operands({ nullptr, nullptr }) {
  }

  void setOperand(unsigned Index, Term *T) {
    Operands[Index] = T;
  }

  TermUse getOperandUse(unsigned OperandIndex) {
    assert(OperandIndex < 2);
    return TermUse(this, OperandIndex);
  }

  Term *getOperand(unsigned OperandIndex) {
    assert(OperandIndex < 2);
    return Operands[OperandIndex];
  }

  static bool isSupported(unsigned Opcode) {
    switch (Opcode) {
    case Instruction::Xor:
    case Instruction::And:
    case Instruction::Or:
      return true;
    default:
      return false;
    }
  }

  virtual bool evaluate(unsigned Assignments) const override;

private:
  unsigned Opcode;
  array<Term *, 2> Operands;
};

void TermUse::set(Term *Operand) {
  T->setOperand(OpIndex, Operand);
}

bool VariableTerm::evaluate(unsigned Assignments) const {
  return Assignments & (1 << VariableIndex);
}

bool BinaryTerm::evaluate(unsigned Assignments) const {
  bool A = Operands[0]->evaluate(Assignments);
  bool B = Operands[1]->evaluate(Assignments);
  switch (Opcode) {
  case Instruction::Xor:
    return A ^ B;
  case Instruction::And:
    return A & B;
  case Instruction::Or:
    return A | B;
  default:
    assert(false);
  }
}

/// \brief If \p V is a LoadInst, looks for the last time it was written
// TODO: use MemoryAccess?
static Value *findOldest(Value *V) {
  if (auto *Load = dyn_cast<LoadInst>(V))
    for (auto &I : backward_range(Load))
      if (auto *Store = dyn_cast<StoreInst>(&I))
        if (Store->getPointerOperand() == Load->getPointerOperand())
          return Store->getValueOperand();

  return V;
}

/// \brief Find the subtraction of the comparison
static BinaryOperator *findSubtraction(User *Cmp) {
  queue<pair<unsigned, Value *>> WorkList;
  WorkList.push({ 0, Cmp->getOperand(0) });
  while (!WorkList.empty()) {
    Value *V = nullptr;
    unsigned Depth = 0;
    tie(Depth, V) = WorkList.front();
    WorkList.pop();

    if (Depth > MaxDepth)
      continue;

    if (auto *BinOp = dyn_cast<BinaryOperator>(V)) {
      auto Opcode = BinOp->getOpcode();
      if (Opcode == Instruction::Sub) {
        return BinOp;
      } else if (BinaryTerm::isSupported(Opcode)) {
        WorkList.push({ Depth + 1, BinOp->getOperand(0) });
        WorkList.push({ Depth + 1, BinOp->getOperand(1) });
      }
    } else if (auto *Load = dyn_cast<LoadInst>(V)) {
      // TODO: extend to unique predecessors
      if (isa<GlobalVariable>(Load->getPointerOperand())) {
        auto *Oldest = findOldest(Load);
        if (Oldest != Load)
          WorkList.push({ Depth, Oldest });
      }
    }
  }

  return nullptr;
}

/// \brief Overload the meaning of an existing predicate as a failure mark
static const auto NoEquivalentPredicate = CmpInst::FCMP_FALSE;

/// Obtain the predicate equivalent to the boolean expression associated to Cmp
/// and whose operands come from \p Subtraction
static Predicate getEquivalentPredicate(CmpInst *Cmp,
                                        BinaryOperator *Subtraction) {
  array<Value *, 3> Variables = {
    findOldest(Subtraction->getOperand(0)),
    findOldest(Subtraction->getOperand(1)),
    Subtraction
  };
  const unsigned OpsCount = std::tuple_size<decltype(Variables)>::value;
  array<VariableTerm, OpsCount> VariableTerms;
  for (unsigned I = 0; I < OpsCount; I++)
    VariableTerms[I] = VariableTerm(I);

  vector<unique_ptr<BinaryTerm>> BinaryTerms;
  BinaryTerm Start = BinaryTerm(Instruction::Or);

  queue<pair<TermUse, Value *>> WorkList;
  WorkList.push({ Start.getOperandUse(0), Cmp->getOperand(0) });

  while (!WorkList.empty()) {
    TermUse PlaceholderUse;
    Value *Operand;
    tie(PlaceholderUse, Operand) = WorkList.front();
    WorkList.pop();

    Operand = findOldest(Operand);

    unsigned OpIndex = 0;
    for (; OpIndex < OpsCount; OpIndex++)
      if (Operand == Variables[OpIndex])
        break;

    if (OpIndex < OpsCount) {
      // It matches one of the variables
      PlaceholderUse.set(&VariableTerms[OpIndex]);
    } else if (auto *BinOp = dyn_cast<BinaryOperator>(Operand)) {
      // It's not a variable, is it a supported operation?
      if (!BinaryTerm::isSupported(BinOp->getOpcode()))
        return NoEquivalentPredicate;

      BinaryTerms.emplace_back(new BinaryTerm(BinOp->getOpcode()));
      BinaryTerm *NewOp = BinaryTerms.back().get();
      PlaceholderUse.set(NewOp);
      WorkList.push({ NewOp->getOperandUse(0), BinOp->getOperand(0) });
      WorkList.push({ NewOp->getOperandUse(1), BinOp->getOperand(1) });
    } else {
      // It's something we don't handle
      return NoEquivalentPredicate;
    }
  }

  // Build the truth table
  assert(OpsCount < 8 * sizeof(unsigned));
  unsigned TruthTable = 0;
  unsigned TruthTableSize = 1 << OpsCount;
  for (unsigned Assignment = 0; Assignment < TruthTableSize; Assignment++)
    if (Start.getOperand(0)->evaluate(Assignment))
      TruthTable |= 1 << Assignment;

  DBG("sc", dbg << "Found truth table "
      << "0b" << std::bitset<8 * sizeof(unsigned)>(TruthTable)
      << " at " << Cmp->getParent()->getName().data() << "\n");

  // Compare with known truth tables
  for (auto &P : KnownTruthTables)
    if (std::get<0>(P) == TruthTable && std::get<1>(P) == TruthTableSize)
      return std::get<2>(P);

  return NoEquivalentPredicate;
}

bool SimplifyComparisonPass::runOnFunction(Function &F) {
  bool Result = false;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (auto *Cmp = isa_with_op<CmpInst, Value, ConstantInt>(&I)) {
        uint64_t N = getLimitedValue(Cmp->getOperand(1));
        if (Cmp->getPredicate() == CmpInst::ICMP_SGE && N == 0) {
          if (BinaryOperator *Subtraction = findSubtraction(Cmp)) {
            auto Predicate = getEquivalentPredicate(Cmp, Subtraction);
            if (Predicate != NoEquivalentPredicate) {
              Result = true;
              Cmp->setOperand(0, Subtraction->getOperand(0));
              Cmp->setOperand(1, Subtraction->getOperand(1));
              Cmp->setPredicate(Predicate);
            }
          }
        }
      }
    }
  }

  return Result;
}

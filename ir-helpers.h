#ifndef _IRHELPERS_H
#define _IRHELPERS_H

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// Standard includes
#include <set>
#include <queue>
#include <sstream>

// LLVM includes
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/Interval.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"

static inline void replaceInstruction(llvm::Instruction *Old,
                                      llvm::Instruction *New) {
  Old->replaceAllUsesWith(New);

  llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 2> Metadata;
  Old->getAllMetadata(Metadata);
  for (auto& MDPair : Metadata)
    New->setMetadata(MDPair.first, MDPair.second);

  Old->eraseFromParent();
}

/// Helper function to destroy an unconditional branch and, in case, the target
/// basic block, if it doesn't have any predecessors left.
static inline void purgeBranch(llvm::BasicBlock::iterator I) {
  auto *DeadBranch = llvm::dyn_cast<llvm::BranchInst>(I);
  // We allow only a branch and nothing else
  assert(DeadBranch != nullptr &&
         ++I == DeadBranch->getParent()->end());

  std::set<llvm::BasicBlock *> Successors;
  for (unsigned C = 0; C < DeadBranch->getNumSuccessors(); C++)
    Successors.insert(DeadBranch->getSuccessor(C));

  // Destroy the dead branch
  DeadBranch->eraseFromParent();

  // Check if someone else was jumping there and then destroy
  for (llvm::BasicBlock *BB : Successors)
    if (llvm::pred_empty(BB))
      BB->eraseFromParent();
}

static inline llvm::ConstantInt *getConstValue(llvm::Constant *C,
                                               const llvm::DataLayout &DL) {
  while (auto *Expr = llvm::dyn_cast<llvm::ConstantExpr>(C)) {
    C = ConstantFoldConstantExpression(Expr, DL);

    if (Expr->getOpcode() == llvm::Instruction::IntToPtr
        || Expr->getOpcode() == llvm::Instruction::PtrToInt)
      C = Expr->getOperand(0);
  }

  if (llvm::isa<llvm::ConstantPointerNull>(C)) {
    auto *Ptr = llvm::IntegerType::get(C->getType()->getContext(),
                                       DL.getPointerSizeInBits());
    return llvm::ConstantInt::get(Ptr, 0);
  }

  auto *Integer = llvm::cast<llvm::ConstantInt>(C);
  return Integer;
}

static inline uint64_t getSExtValue(llvm::Constant *C,
                                    const llvm::DataLayout &DL){
  return getConstValue(C, DL)->getSExtValue();
}

static inline uint64_t getZExtValue(llvm::Constant *C,
                                    const llvm::DataLayout &DL){
  return getConstValue(C, DL)->getZExtValue();
}

static inline uint64_t getExtValue(llvm::Constant *C,
                                   bool Sign,
                                   const llvm::DataLayout &DL){
  if (Sign)
    return getSExtValue(C, DL);
  else
    return getZExtValue(C, DL);
}

static inline uint64_t getLimitedValue(const llvm::Value *V) {
  return llvm::cast<llvm::ConstantInt>(V)->getLimitedValue();
}

static inline llvm::iterator_range<llvm::Interval::pred_iterator>
predecessors(llvm::Interval *BB) {
  return make_range(pred_begin(BB), pred_end(BB));
}

static inline llvm::iterator_range<llvm::Interval::succ_iterator>
successors(llvm::Interval *BB) {
  return make_range(succ_begin(BB), succ_end(BB));
}

template<typename T, unsigned I>
static inline bool findOperand(llvm::Value *Op, T &Result) {
  return false;
}

template<typename T, unsigned I, typename Head, typename... Tail>
static inline bool findOperand(llvm::Value *Op, T &Result) {
  using VT = typename std::remove_pointer<Head>::type;
  if (auto *Casted = llvm::dyn_cast<VT>(Op)) {
    std::get<I>(Result) = Casted;
    return true;
  } else {
    return findOperand<T, I + 1, Tail...>(Op, Result);
  }
}

/// \brief Return a tuple of \p V's operands of the requested types
/// \return a tuple with the operands of the specified type in the specified
///         order, or, if not possible, a nullptr tuple.
template<typename... T>
static inline std::tuple<T...> operandsByType(llvm::User *V) {
  std::tuple<T...> Result;
  unsigned OpCount = V->getNumOperands();
  assert(OpCount == sizeof...(T));

  for (llvm::Value *Op : V->operands())
    if (!findOperand<std::tuple<T...>, 0, T...>(Op, Result))
      return std::tuple<T...> { };

  return Result;
}

/// \brief Checks the instruction type and its operands
/// \return the instruction casted to I, or nullptr if not possible.
template<typename I, typename F, typename S>
static inline I *isa_with_op(llvm::Instruction *Inst) {
  if (auto *Casted = llvm::dyn_cast<I>(Inst)) {
    assert(Casted->getNumOperands() == 2);
    if (llvm::isa<F>(Casted->getOperand(0))
        && llvm::isa<S>(Casted->getOperand(1))) {
      return Casted;
    } else if (llvm::isa<F>(Casted->getOperand(0))
               && llvm::isa<S>(Casted->getOperand(1))) {
      assert(Casted->isCommutative());
      Casted->swapOperands();
      return Casted;
    }
  }

  return nullptr;
}

/// \brief Return an range iterating backward from the given instruction
static inline llvm::iterator_range<llvm::BasicBlock::reverse_iterator>
backward_range(llvm::Instruction *I) {
  return llvm::make_range(llvm::make_reverse_iterator(I->getIterator()),
                          I->getParent()->rend());
}

/// \brief Possible way to continue (or stop) exploration in a breadth-first
///        visit
enum VisitAction {
  Continue, ///< Visit also the successor basic blocks
  NoSuccessors, ///< Do not visit the successors of this basic block
  ExhaustQueueAndStop, ///< Prevent adding visiting other basic blocks except
                       ///  those already pending
  StopNow ///< Interrupt immediately the visit
};

using BasicBlockRange = llvm::iterator_range<llvm::BasicBlock::iterator>;
using VisitorFunction = std::function<VisitAction(BasicBlockRange)>;

/// Performs a breadth-first visit of the instruction after \p I and in the
/// successor basic blocks
///
/// \param I the instruction from where to the visit should start
/// \param Ignore a set of basic block to ignore during the visit
/// \param Visitor the visitor function, see VisitAction to understand what this
///        function should return
static inline void visitSuccessors(llvm::Instruction *I,
                                   const std::set<llvm::BasicBlock *> &Ignore,
                                   VisitorFunction Visitor) {
  std::set<llvm::BasicBlock *> Visited = Ignore;

  llvm::BasicBlock::iterator It(I);
  It++;

  std::queue<llvm::iterator_range<llvm::BasicBlock::iterator>> Queue;
  Queue.push(make_range(It, I->getParent()->end()));

  bool ExhaustOnly = false;

  while (!Queue.empty()) {
    auto Range = Queue.front();
    Queue.pop();

    switch (Visitor(Range)) {
    case Continue:
      if (!ExhaustOnly) {
        for (auto *Successor : successors(Range.begin()->getParent())) {
          if (Visited.count(Successor) == 0) {
            Visited.insert(Successor);
            Queue.push(make_range(Successor->begin(), Successor->end()));
          }
        }
      }
      break;
    case NoSuccessors:
      break;
    case ExhaustQueueAndStop:
      ExhaustOnly = true;
      break;
    case StopNow:
      return;
    default:
      assert(false);
    }
  }
}

static inline void visitSuccessors(llvm::Instruction *I,
                                   llvm::BasicBlock *Ignore,
                                   VisitorFunction Visitor) {
  std::set<llvm::BasicBlock *> IgnoreSet;
  IgnoreSet.insert(Ignore);
  visitSuccessors(I, IgnoreSet, Visitor);
}

using RBasicBlockRange =
  llvm::iterator_range<llvm::BasicBlock::reverse_iterator>;
using RVisitorFunction = std::function<bool(RBasicBlockRange)>;

// TODO: factor with visitSuccessors
static inline void visitPredecessors(llvm::Instruction *I,
                                     RVisitorFunction Visitor,
                                     llvm::BasicBlock *Ignore) {
  llvm::BasicBlock *Parent = I->getParent();
  std::set<llvm::BasicBlock *> Visited;
  Visited.insert(Parent);

  llvm::BasicBlock::reverse_iterator It(make_reverse_iterator(I));
  if (It == Parent->rend())
    return;
  // It++;

  std::queue<llvm::iterator_range<llvm::BasicBlock::reverse_iterator>> Queue;
  Queue.push(llvm::make_range(It, Parent->rend()));
  bool Stop = false;

  while (!Queue.empty()) {
    auto Range = Queue.front();
    Queue.pop();
    auto *lol = Range.begin()->getParent();

    if (Visitor(Range))
      Stop = true;

    for (auto *Predecessor : predecessors(lol)) {
      if (Visited.count(Predecessor) == 0 && Predecessor != Ignore) {
        Visited.insert(Predecessor);
        if (!Stop && !Predecessor->empty())
          Queue.push(make_range(Predecessor->rbegin(), Predecessor->rend()));
      }
    }

  }

}

/// \brief Return a sensible name for the given basic block
/// \return the name of the basic block, if available, its pointer value
///         otherwise.
static inline std::string getName(const llvm::BasicBlock *BB) {
  llvm::StringRef Result = BB->getName();
  if (!Result.empty()) {
    return Result.str();
  } else {
    std::stringstream SS;
    SS << "0x" << std::hex << intptr_t(BB);
    return SS.str();
  }
}

/// \brief Return a sensible name for the given instruction
/// \return the name of the instruction, if available, a
///         [basic blockname]:[instruction index] string otherwise.
static inline std::string getName(const llvm::Instruction *I) {
  llvm::StringRef Result = I->getName();
  if (!Result.empty()) {
    return Result.str();
  } else {
    const llvm::BasicBlock *Parent = I->getParent();
    return getName(Parent) + ":"
      + std::to_string(1 + std::distance(Parent->begin(), I->getIterator()));
  }
}

/// \brief Return a sensible name for the given Value
/// \return if \p V is an Instruction, call the appropriate getName function,
///         otherwise return a pointer to \p V.
static inline std::string getName(const llvm::Value *V) {
  if (V != nullptr)
    if (auto *I = llvm::dyn_cast<llvm::Instruction>(V))
      return getName(I);
  std::stringstream SS;
  SS << "0x" << std::hex << intptr_t(V);
  return SS.str();
}

// TODO: this function assumes 0 is not a valid PC
static inline uint64_t getBasicBlockPC(llvm::BasicBlock *BB) {
  auto It = BB->begin();
  assert(It != BB->end());
  if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&*It)) {
    auto *Callee = Call->getCalledFunction();
    if (Callee && Callee->getName() == "newpc")
      return getLimitedValue(Call->getOperand(0));
  }

  return 0;
}

#endif // _IRHELPERS_H

#ifndef _OSRA_H
#define _OSRA_H

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// Standard includes
#include <cstdint>
#include <stack>
#include <limits>
#include <map>
#include <set>
#include <vector>

// LLVM includes
#include "llvm/Pass.h"

// Local includes
#include "ir-helpers.h"
#include "reachingdefinitions.h"
#include "simplifycomparisons.h"

// Forward declarations
namespace llvm {
class BasicBlock;
class formatted_raw_ostream;
class Function;
class Instruction;
class LLVMContext;
class LoadInst;
class Module;
class SwitchInst;
class StoreInst;
class Value;
}

/// \brief DFA to represent values as a + b * x, with c < x < d
class OSRAPass : public llvm::FunctionPass {
public:
  static char ID;

  OSRAPass() : llvm::FunctionPass(ID) { }

  bool runOnFunction(llvm::Function &F) override;

  void describe(llvm::formatted_raw_ostream &O,
                const llvm::Instruction *I) const;
  void describe(llvm::formatted_raw_ostream &O,
                const llvm::BasicBlock *BB) const;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.addRequired<ConditionalReachedLoadsPass>();
    AU.addRequired<SimplifyComparisonsPass>();
    AU.setPreservesAll();
  }

public:
  /// \brief Represent an SSA value within a (negated) range and its signedness
  class BoundedValue {
  public:
    BoundedValue(const llvm::Value *V) :
      Value(V),
      LowerBound(0),
      UpperBound(0),
      Sign(UnknownSignedness),
      Bottom(false),
      Negated(false) {
        if (auto *Constant = llvm::dyn_cast<llvm::ConstantInt>(V)) {
          LowerBound = UpperBound = getLimitedValue(Constant);
          Sign = AnySignedness;
        }
      }

    BoundedValue() :
      Value(nullptr),
      LowerBound(0),
      UpperBound(0),
      Sign(UnknownSignedness),
      Bottom(false),
      Negated(false) { }


    /// \brief Notify about a usage of the SSA value with a certain signedness
    ///
    /// This function can alter the signedness of the BV:
    /// `UnknownSignedness -- IsSigned --> Signed`;
    /// `UnknownSignedness -- !IsSigned --> Unsigned`;
    /// `Signed -- IsSigned --> Signed`;
    /// `Signed -- !IsSigned --> InconsistentSignedness`;
    /// `Unsigned -- !IsSigned --> Unsigned`;
    /// `Unsigned -- IsSigned --> InconsistentSignedness`;
    ///
    /// InconsistentSignedness is a sink state.
    // TODO: update users in case the sign changed
    void setSignedness(bool IsSigned);

    void describe(llvm::formatted_raw_ostream &O) const;

    /// \brief Merge policies for BVs
    enum MergeType {
      And, ///< Intersection of the ranges
      Or ///< Union of the ranges
    };
    enum Bound { Lower, Upper };

    bool isUninitialized() const { return Sign == UnknownSignedness; }
    bool hasSignedness() const {
      return Sign != UnknownSignedness && Sign != AnySignedness;
    }

    bool isConstant() const {
      return !isUninitialized() && !Bottom && LowerBound == UpperBound;
    }

    uint64_t constant() const {
      assert(isConstant());
      return LowerBound;
    }

    /// \brief Merge \p Other using the \p MT policy
    template<MergeType MT=And>
    bool merge(const BoundedValue &Other,
               const llvm::DataLayout &DL,
               llvm::Type *Int64);

    /// \brief Sets a boundary for the current BV using the \p Type policy
    template<Bound B, MergeType Type=And>
    bool setBound(llvm::Constant *NewValue, const llvm::DataLayout &DL);

    /// \brief Accessor to the SSA value represent by this BV
    const llvm::Value *value() const { return Value; }

    bool isSigned() const {
      assert(Sign != UnknownSignedness
             && Sign != AnySignedness
             && !Bottom);
      return Sign != Unsigned;
    }

    llvm::Constant *lower(llvm::Type *Int64) const {
      return llvm::ConstantInt::get(Int64, LowerBound, isSigned());
    }

    llvm::Constant *upper(llvm::Type *Int64) const {
      return llvm::ConstantInt::get(Int64, UpperBound, isSigned());
    }

    /// \brief If the BV is limited, return its bounds considering negation
    ///
    /// Do not invoke this method on unlimited BVs.
    // TODO: should this method perform a cast to the type of Value?
    std::pair<llvm::Constant *, llvm::Constant *>
    actualBoundaries(llvm::Type *Int64) const {
      assert(!(Negated && isConstant()));

      using CI = llvm::ConstantInt;
      if (!Negated)
        return std::make_pair(lower(Int64), upper(Int64));
      else if (LowerBound == lowerExtreme())
        return std::make_pair(CI::get(Int64, UpperBound + 1, isSigned()),
                              CI::get(Int64, upperExtreme(), isSigned()));
      else if (UpperBound == upperExtreme())
        return std::make_pair(CI::get(Int64, lowerExtreme(), isSigned()),
                              CI::get(Int64, LowerBound - 1, isSigned()));

      assert(false && "The BV is unlimited");
    }

    bool operator ==(const BoundedValue &Other) const {
      if (Bottom || Other.Bottom)
        return Bottom == Other.Bottom;

      return (Value == Other.Value
              && LowerBound == Other.LowerBound
              && UpperBound == Other.UpperBound
              && Sign == Other.Sign
              && Bottom == Other.Bottom
              && Negated == Other.Negated);
    }

    bool operator !=(const BoundedValue &Other) {
      return !(*this == Other);
    }

    /// \brief Negates this BV
    void flip() {
      Negated = !Negated;
      return;
    }

    /// \brief Set this BV to bottom
    void setBottom() {
      assert(!Bottom);
      Bottom = true;
    }

    bool isBottom() const { return Bottom; }

    /// \brief Return if this BV is top
    ///
    /// A BV is top if it's uninitialized or it's not negated and both its
    /// boundaries are at their respective extremes.
    bool isTop() const {
      return (!isConstant()
              && Sign != AnySignedness
              && (isUninitialized()
                  || (!Negated
                      && LowerBound == lowerExtreme()
                      && UpperBound == upperExtreme())));
    }

    /// \brief Return the size of the range constraining this BV
    ///
    /// Do not invoke this method on unlimited BVs.
    uint64_t size() const {
      assert(!(Negated && isConstant()));
      if (!Negated)
        return UpperBound - LowerBound;
      else if (LowerBound == lowerExtreme())
        return upperExtreme() - (UpperBound + 1);
      else if (UpperBound == upperExtreme())
        return (LowerBound - 1) - lowerExtreme();

      llvm_unreachable("The BV is unlimited");
    }

    static BoundedValue createGE(const llvm::Value *V,
                                 uint64_t Value,
                                 bool Sign) {
      BoundedValue Result(V);
      Result.setSignedness(Sign);
      Result.LowerBound = Value;
      Result.UpperBound = Result.upperExtreme();
      return Result;
    }

    static BoundedValue createLE(const llvm::Value *V,
				 uint64_t Value,
				 bool Sign) {
      BoundedValue Result(V);
      Result.setSignedness(Sign);
      Result.LowerBound = Result.lowerExtreme();
      Result.UpperBound = Value;
      return Result;
    }

    static BoundedValue createEQ(const llvm::Value *V,
                                 uint64_t Value,
                                 bool Sign) {
      BoundedValue Result(V);
      Result.setSignedness(Sign);
      Result.LowerBound = Value;
      Result.UpperBound = Value;
      return Result;
    }

    static BoundedValue createNE(const llvm::Value *V,
                                 uint64_t Value,
                                 bool Sign) {
      BoundedValue Result(V);
      Result.setSignedness(Sign);
      Result.LowerBound = Value;
      Result.UpperBound = Value;
      Result.Negated = true;
      return Result;
    }

    static BoundedValue createConstant(const llvm::Value *V,
                                       uint64_t Value) {
      BoundedValue Result(V);
      Result.LowerBound = Value;
      Result.UpperBound = Value;
      Result.Sign = AnySignedness;
      return Result;
    }

    /// \brief Set the BV to top
    ///
    /// Set the boundaries of the BV to their extreme values.
    void setTop() {
      if (isUninitialized())
        return;

      if (Sign == AnySignedness) {
        LowerBound = 0;
        UpperBound = 0;
        Sign = UnknownSignedness;
        Negated = false;
        Bottom = false;
        return;
      }

      LowerBound = lowerExtreme();
      UpperBound = upperExtreme();
      Negated = false;
      Bottom = false;
    }

    /// \brief Return true if the BV is not unlimited
    bool isSingleRange() const {
      if (!Negated)
        return true;
      else if (Negated && isConstant())
        return false;
      else
        return LowerBound == lowerExtreme() || UpperBound == upperExtreme();
    }

    /// Produce a new BV relative to \p V with boundaries multiplied by \p
    /// Multiplier and then adding \p Offset
    BoundedValue moveTo(llvm::Value *V,
                        const llvm::DataLayout &DL,
                        uint64_t Offset=0,
                        uint64_t Multiplier=0) const;

  private:
    uint64_t lowerExtreme() const {
      switch (Sign) {
      case Unsigned:
        return std::numeric_limits<uint64_t>::min();
      case Signed:
        return std::numeric_limits<int64_t>::min();
      case InconsistentSignedness:
        return std::numeric_limits<uint64_t>::min();
      default:
        llvm_unreachable("Unexpected signedness");
      }
    }

    uint64_t upperExtreme() const {
      switch (Sign) {
      case Unsigned:
        return std::numeric_limits<uint64_t>::max();
      case Signed:
        return std::numeric_limits<int64_t>::max();
      case InconsistentSignedness:
        return std::numeric_limits<int64_t>::max();
      default:
        llvm_unreachable("Unexpected signedness");
      }
    }

    /// \brief Performs a binary operation using signedness and type of the BV
    uint64_t performOp(uint64_t Op1,
                       unsigned Opcode,
                       uint64_t Op2,
                       const llvm::DataLayout &DL) const;
  public:
    const llvm::Value *Value;
    uint64_t LowerBound;
    uint64_t UpperBound;

    /// \brief Possible states for signedness
    enum Signedness : uint8_t {
      UnknownSignedness, ///< Nothing is known about the signedness
      AnySignedness, ///< Nothing is known about the signedness
      Unsigned,
      Signed,
      InconsistentSignedness ///< The BV is used both as signed and unsigned
    };

    Signedness Sign;
    uint8_t Bottom;
    uint8_t Negated;
  };

  /// \brief An OSR represents an expression a + b * x, x being a BoundedValue
  class OSR {
  public:

    /// \brief Constructor for a basic OSR
    OSR(const BoundedValue *Value) : Base(0), Factor(1), BV(Value) { }

    OSR() : Base(0), Factor(1), BV(nullptr) { }

    OSR(const OSR &Other) :
      Base(Other.Base),
      Factor(Other.Factor),
      BV(Other.BV) { }

    uint64_t constant() const {
      return BV->constant();
    }

    /// \brief Combine this OSR with \p Operand through \p Opcode
    ///
    /// \p Operand is always assumed to be the second operand, never invoke this
    /// method with a non-commutative \p Opcode if \p Operand is the first
    /// operand.
    ///
    /// \param Opcode LLVM opcode describing the operation.
    /// \param Operand the constant Operand with which combine the OSR.
    /// \param FreeOpIndex the index of the non-constant operator.
    ///
    /// \return true if the OSR has been modified.
    bool combine(unsigned Opcode, llvm::Constant *Operand,
                 unsigned FreeOpIndex,
                 const llvm::DataLayout &DL);

    /// \brief Compute the solution of integer equation `a + b * x = k`
    ///
    /// \param KnownTerm the right-hand side of the equation.
    /// \param CeilingRounding the rounding mode, round for excess if true.
    ///
    /// \return the solution of the integer equation using the specified
    ///         rounding mode.
    llvm::Constant *solveEquation(llvm::Constant *KnownTerm,
                                  bool CeilingRounding,
                                  const llvm::DataLayout &DL);

    /// \brief Checks if this OSR is relative to \p V
    bool isRelativeTo(const llvm::Value *V) const {
      return BV->value() == V;
    }

    /// \brief Change the BoundedValue associated to this OSR
    void setBoundedValue(BoundedValue *NewBV) {
      BV = NewBV;
    }

    /// \brief Accessor method to BoundedValue associate to this OSR
    const BoundedValue *boundedValue() const {
      assert(BV != nullptr);
      return BV;
    }

    bool operator ==(const OSR& Other) const {
      return Base == Other.Base && Factor == Other.Factor && BV == Other.BV;
    }

    bool operator !=(const OSR& Other) const {
      return !(*this == Other);
    }

    void describe(llvm::formatted_raw_ostream &O) const;

    /// \brief Return true if the OSR doesn't have a BoundedValue or is
    ///        ininfluent
    bool isConstant() const {
      assert(BV != nullptr);
      return BV->isConstant();
      return (BV == nullptr || !BV->isBottom()) && Factor == 0;
    }

    /// \brief Helper function to performe the comparison \p P with \p C
    bool compare(unsigned short P,
                 llvm::Constant *C,
                 const llvm::DataLayout &DL,
                 llvm::Type *Int64);

    /// \brief Compute `a + b * Value`
    llvm::Constant *evaluate(llvm::Constant *Value,
                             llvm::Type *Int64) const;

    /// \brief Compute the boundaries value
    ///
    /// This method basically evaluates `a + b * c` and `a + b * d` being `c`
    /// and `d` the boundaries of the associated BoundedValue.
    ///
    /// \return a pair of `Constant` representing the lower and upper bounds.
    std::pair<llvm::Constant *,
              llvm::Constant *> boundaries(llvm::Type *Int64,
                                           const llvm::DataLayout &DL) const;

    /// \brief Return the size of the associated BoundedValue
    uint64_t size() const { return BV->size(); }

    /// \brief Accessor to the factor value of this OSR (`b`)
    uint64_t factor() const { return Factor; }

    // TODO: bad name
    BoundedValue apply(const BoundedValue &Target,
        llvm::Value *V,
        const llvm::DataLayout &DL) const {
      if (Target.isBottom() || Target.isTop() || !Target.hasSignedness())
        return Target;

      return Target.moveTo(V, DL, Base, Factor);
    }
  private:
    uint64_t Base;
    uint64_t Factor;
    const BoundedValue *BV;
  };

private:
  class BVMap {
  private:
    using MapIndex = std::pair<llvm::BasicBlock *, const llvm::Value *>;
    using BVWithOrigin = std::pair<llvm::BasicBlock *, BoundedValue>;
    struct MapValue {
      BoundedValue Summary;
      std::vector<BVWithOrigin> Components;
    };

  public:
    BVMap() : BlockBlackList(nullptr), DL(nullptr), Int64(nullptr) { }
    BVMap(std::set<llvm::BasicBlock *> *BlackList,
          const llvm::DataLayout *DL,
          llvm::Type *Int64) :
      BlockBlackList(BlackList), DL(DL), Int64(Int64) { }

    void describe(llvm::formatted_raw_ostream &O,
                  const llvm::BasicBlock *BB) const;

    BoundedValue &get(llvm::BasicBlock *BB, const llvm::Value *V) {
      auto Index = std::make_pair(BB, V);
      auto MapIt = TheMap.find(Index);
      if (MapIt == TheMap.end()) {
        MapValue NewBVOVector;
        NewBVOVector.Summary = BoundedValue(V);
        auto It = TheMap.insert(std::make_pair(Index, NewBVOVector)).first;
        return summarize(BB, &It->second);
      }

      MapValue &BVOs = MapIt->second;
      return BVOs.Summary;
    }

    BoundedValue *getEdge(llvm::BasicBlock *BB,
                          llvm::BasicBlock *Predecessor,
                          const llvm::Value *V) {
      auto MapIt = TheMap.find({ BB, V });
      if (MapIt != TheMap.end())
        for (auto &Component : MapIt->second.Components)
          if (Component.first == Predecessor)
            return &Component.second;

      return nullptr;
    }

    void setSignedness(llvm::BasicBlock *BB,
                       const llvm::Value *V,
                       bool IsSigned) {
      auto Index = std::make_pair(BB, V);
      auto MapIt = TheMap.find(Index);
      assert(MapIt != TheMap.end());

      MapValue &BVOVector = MapIt->second;
      BVOVector.Summary.setSignedness(IsSigned);
      for (BVWithOrigin &BVO : BVOVector.Components)
        BVO.second.setSignedness(IsSigned);

      summarize(BB, &MapIt->second);
    }

    /// Associate to basic block \p Target a new constraint \p NewBV coming from
    /// \p Origin
    ///
    /// \return a pair containing a boolean to indicate whether there was any
    ///         change and a reference to the updated BV
    std::pair<bool, BoundedValue &> update(llvm::BasicBlock *Target,
                                           llvm::BasicBlock *Origin,
                                           BoundedValue NewBV);

    void prepareDescribe() const {
      BBMap.clear();
      for (auto Pair : TheMap) {
        auto *BB = Pair.first.first;
        if (BBMap.find(BB) == BBMap.end())
          BBMap[BB] = std::vector<MapValue> { Pair.second };
        else
          BBMap[BB].push_back(Pair.second);
      }
    }

    BoundedValue &forceBV(llvm::Instruction *V, BoundedValue BV) {
      MapIndex I { V->getParent(), V };
      MapValue NewValue;
      NewValue.Summary = BV;
      TheMap[I] = NewValue;
      return TheMap[I].Summary;
    }

    BoundedValue &forceBV(llvm::BasicBlock *BB,
                          llvm::Value *V,
                          BoundedValue BV) {
      MapIndex I { BB, V };
      MapValue NewValue;
      NewValue.Summary = BV;
      TheMap[I] = NewValue;
      return TheMap[I].Summary;
    }

    void clear() {
      freeContainer(TheMap);
      freeContainer(BBMap);
    }

  private:
    BoundedValue &summarize(llvm::BasicBlock *Target,
                            MapValue *BVOVectorLoopInfoWrapperPass);

    bool isForced(std::map<MapIndex, MapValue>::iterator &It) const {
      const MapIndex &Index = It->first;

      if (auto *I = llvm::dyn_cast<llvm::Instruction>(Index.second)) {
        return I->getParent() == Index.first
          && It->second.Components.size() == 0;
      } else {
        return false;
      }
    }

  private:
    std::set<llvm::BasicBlock *> *BlockBlackList;
    const llvm::DataLayout *DL;
    llvm::Type *Int64;
    std::map<MapIndex, MapValue> TheMap;
    mutable std::map<const llvm::BasicBlock *, std::vector<MapValue>> BBMap;
  };

public:
  const OSR *getOSR(const llvm::Value *V) {
    auto *I = llvm::dyn_cast<llvm::Instruction>(V);

    if (I == nullptr)
      return nullptr;

    auto It = OSRs.find(I);
    if (It == OSRs.end())
      return nullptr;
    else
      return &It->second;
  }

  std::pair<llvm::Constant *, llvm::Value *>
    identifyOperands(const llvm::Instruction *I,
                     const llvm::DataLayout &DL);

  /// \brief Return true if \p I is stored in the CPU state but never read again
  bool isDead(llvm::Instruction *I) const;

  virtual void releaseMemory() override {
    DBG("release", { dbg << "OSRAPass is releasing memory\n"; });
    freeContainer(OSRs);
    BVs.clear();
  }

private:

  OSR switchBlock(OSR Base, llvm::BasicBlock *BB) {
    Base.setBoundedValue(&BVs.get(BB, Base.boundedValue()->value()));
    return Base;
  }

  /// Return a copy of the OSR associated with \p V, or if it does not exist,
  /// create a new one. In both cases the return value will refer to a bounded
  /// value in the context of \p BB.
  ///
  /// Note: after invoking this function you should always check if the result
  ///       is not expressed in terms of the instruction you're analyzing
  ///       itself, otherwise we could create (possibly infinite) loops we're
  ///       not really interested in.
  ///
  /// \return the newly created OSR, possibly expressed in terms of \p V itself.
  OSR createOSR(llvm::Value *V, llvm::BasicBlock *BB);

  /// Compute a BV for \p Reached by collecting constraints on the reaching
  /// definitions over all the paths from \p Reached to them
  BoundedValue pathSensitiveMerge(llvm::LoadInst *Reached);

  llvm::pred_iterator getValidPred(llvm::BasicBlock *BB) {
    llvm::pred_iterator Result = llvm::pred_begin(BB);
    nextValidPred(Result, llvm::pred_end(BB));
    return Result;
  }

  llvm::pred_iterator &nextValidPred(llvm::pred_iterator &It,
                                     llvm::pred_iterator End) {
    while (It != End && BlockBlackList.count(*It) != 0)
      It++;

    return It;
  }

  bool updateLoadReacher(llvm::LoadInst *Load,
                         llvm::Instruction *I,
                         OSR NewOSR);
  void mergeLoadReacher(llvm::LoadInst *Load);

public:
  using BVVector = llvm::SmallVector<BoundedValue, 2>;

private:
  // TODO: why value and not instruction?
  std::map<const llvm::Value *, const OSR> OSRs;
  BVMap BVs;
  std::map<const llvm::Instruction *, BVVector> Constraints;
  using InstructionOSRVector = std::vector<std::pair<llvm::Instruction *, OSR>>;
  std::map<const llvm::LoadInst *, InstructionOSRVector> LoadReachers;
  std::set<llvm::BasicBlock *> BlockBlackList;

  /// Keeps track of those instruction that need to be updated when the reachers
  /// of a certain Load are updated
  using SubscribersType = llvm::SmallSet<llvm::Instruction *, 3>;
  std::map<const llvm::LoadInst *, SubscribersType> Subscriptions;
  ConditionalReachedLoadsPass *RDP;
};

#endif // _OSRA_H

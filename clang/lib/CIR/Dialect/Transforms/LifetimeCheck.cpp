//===- Lifetimecheck.cpp - emit diagnostic checks for lifetime violations -===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PassDetail.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "clang/Basic/Stack.h"
#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Dialect/Passes.h"

#include "clang/CIR/Interfaces/CIRLoopOpInterface.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/SaveAndRestore.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace mlir;
using namespace cir;

namespace mlir {
#define GEN_PASS_DEF_LIFETIMECHECK
#include "clang/CIR/Dialect/Passes.h.inc"
} // namespace mlir

namespace {

struct LocOrdering {
  bool operator()(mlir::Location L1, mlir::Location L2) const {
    return std::less<const void *>()(L1.getAsOpaquePointer(),
                                     L2.getAsOpaquePointer());
  }
};

using CallParameterKinds = SmallVector<std::optional<LifetimeParameterKind>, 8>;
using FunctionLabelMap = DenseMap<StringAttr, Block *>;

static LifetimeLibraryKind getRecordLibraryKind(Type type, ModuleOp module) {
  auto record = dyn_cast<RecordType>(type);
  auto facts =
      record ? getLifetimeRecord(record, module) : LifetimeRecordAttr{};
  return facts ? facts.getLibraryKind() : LifetimeLibraryKind::None;
}

static LifetimeLibraryKind getLibraryKind(Type type, ModuleOp module) {
  if (auto pointer = dyn_cast<PointerType>(type))
    type = pointer.getPointee();
  return getRecordLibraryKind(type, module);
}

static bool hasEmptyMovedFromState(LifetimeLibraryKind kind) {
  return kind == LifetimeLibraryKind::UniquePointer ||
         kind == LifetimeLibraryKind::SharedPointer ||
         kind == LifetimeLibraryKind::WeakPointer;
}

struct LifetimeCheckPass : public impl::LifetimeCheckBase<LifetimeCheckPass> {
  LifetimeCheckPass() = default;
  void runOnOperation() override;

  void checkFunc(cir::FuncOp fnOp);
  void checkCall(CallOp callOp,
                 ArrayRef<std::optional<LifetimeParameterKind>> parameterKinds);
  void checkReturn(ReturnOp retOp);

  bool isOwnerType(mlir::Type type);
  bool isPointerType(mlir::Type type);

  void updatePointsTo(mlir::Value addr, mlir::Value data, mlir::Location loc);
  void updatePointsToForConstRecord(mlir::Value addr,
                                    cir::ConstRecordAttr value,
                                    mlir::Location loc);
  void updatePointsToForConstPointer(mlir::Value addr, mlir::TypedAttr value,
                                     mlir::Location loc);
  void updatePointsToForZeroRecord(mlir::Value addr, RecordType sTy,
                                   mlir::Location loc);

  enum DerefStyle {
    Direct,
    RetLambda,
    CallParam,
    NullableCallParam,
    UseAfterMove,
  };
  bool checkPointerDeref(mlir::Value addr, mlir::Location loc,
                         DerefStyle derefStyle = DerefStyle::Direct);
  void checkCoroTaskStore(StoreOp storeOp);
  void checkLambdaCaptureStore(StoreOp storeOp);
  void trackCallToCoroutine(CallOp callOp);

  void checkCtor(CallOp callOp, cir::CXXCtorAttr ctor,
                 ArrayRef<std::optional<LifetimeParameterKind>> parameterKinds);
  void checkMoveCtor(CallOp callOp, cir::CXXCtorAttr ctor);
  void checkMoveAssignment(CallOp callOp, cir::LifetimeFunctionAttr m);
  void checkCopyAssignment(CallOp callOp, cir::LifetimeFunctionAttr m);
  void checkNonConstUseOfOwner(mlir::Value ownerAddr, mlir::Location loc);
  void markOwnerAsMovedFrom(mlir::Value addr, mlir::Location loc);
  // Returns true if the value represents a temporary that should be skipped
  // for move tracking purposes.
  //
  // Temporaries are generally skipped because they're destroyed at the end of
  // the full expression and cannot be used after being moved from. However,
  // coroutine task temporaries are an exception - they need lifetime tracking
  // even as temporaries because they may be captured by the coroutine frame.
  //
  // Note: Currently uses "ref.tmp" prefix detection which is not fully
  // reliable. See FIXME comment in implementation for future improvements.
  bool isSkippableTemporary(mlir::Value v);

  // Helper methods for checking value states and tracking moved-from values
  bool isValueType(mlir::Value addr);
  bool hasInvalidState(mlir::Value addr);
  void markPointerOrValueTypeAsMovedFrom(mlir::Value addr, mlir::Location loc);

  bool isSmartPointerType(mlir::Type type);
  bool isSmartPointerSafeMethod(llvm::StringRef methodName);
  void checkOperators(CallOp callOp, cir::LifetimeFunctionAttr m);
  void checkOtherMethodsAndFunctions(
      CallOp callOp, cir::LifetimeFunctionAttr m,
      ArrayRef<std::optional<LifetimeParameterKind>> parameterKinds);
  void checkForOwnerAndPointerArguments(
      CallOp callOp, unsigned firstArgIdx,
      ArrayRef<std::optional<LifetimeParameterKind>> parameterKinds,
      std::optional<unsigned> borrowedOwnerArg = {},
      llvm::function_ref<bool(unsigned)> skip = nullptr,
      bool mutateOwners = true);

  // TODO: merge both methods below and pass down an enum.
  //
  // Check if a method's 'this' pointer (first arg) is tracked as
  // a pointer category. Assumes the CallOp in question represents a method
  // and returns the actual value associated with the tracked 'this' or an
  // empty value if none is found.
  mlir::Value getThisParamPointerCategory(CallOp callOp);
  // Check if a method's 'this' pointer (first arg) is tracked as
  // a owner category. Assumes the CallOp in question represents a method
  // and returns the actual value associated with the tracked 'this' or an
  // empty value if none is found.
  mlir::Value getThisParamOwnerCategory(CallOp callOp);

  // Tracks current module.
  ModuleOp theModule;
  // The module is read-only throughout lifetime analysis.
  mlir::SymbolTable *moduleSymbols = nullptr;
  const FunctionLabelMap *labelTargets = nullptr;
  // Track current function under analysis
  std::optional<FuncOp> currFunc;

  // Common helpers.
  bool isCtorInitPointerFromOwner(CallOp callOp);
  mlir::Value getNonConstUseOfOwner(CallOp callOp, cir::LifetimeFunctionAttr m);
  bool isOwnerOrPointerClassMethod(CallOp callOp, cir::LifetimeFunctionAttr m);

  // Diagnostic helpers.
  void emitInvalidHistory(mlir::InFlightDiagnostic &D, mlir::Value histKey,
                          mlir::Location warningLoc,
                          DerefStyle derefStyle = DerefStyle::Direct);

  ///
  /// Pass options handling
  /// ---------------------

  struct Options {
    enum : unsigned {
      None = 0,
      // Emit pset remarks only detecting invalid derefs
      RemarkPsetInvalid = 1,
      // Emit pset remarks for all derefs
      RemarkPsetAlways = 1 << 1,
      RemarkAll = 1 << 2,
      HistoryNull = 1 << 3,
      HistoryInvalid = 1 << 4,
      HistoryAll = 1 << 5,
    };
    unsigned val = None;
    unsigned histLimit = 1;
    bool isOptionsParsed = false;
    std::optional<std::string> error;

    void parseOptions(ArrayRef<llvm::StringRef> remarks,
                      ArrayRef<llvm::StringRef> hist, unsigned hist_limit) {
      if (isOptionsParsed)
        return;
      isOptionsParsed = true;

      for (auto &remark : remarks) {
        unsigned option = llvm::StringSwitch<unsigned>(remark)
                              .Case("pset-invalid", RemarkPsetInvalid)
                              .Case("pset-always", RemarkPsetAlways)
                              .Case("all", RemarkAll)
                              .Default(None);
        if (option == None) {
          error = "invalid value '" + remark.str() +
                  "' for lifetime-check option 'remarks'";
          return;
        }
        val |= option;
      }
      for (auto &h : hist) {
        unsigned option = llvm::StringSwitch<unsigned>(h)
                              .Case("invalid", HistoryInvalid)
                              .Case("null", HistoryNull)
                              .Case("all", HistoryAll)
                              .Default(None);
        if (option == None) {
          error = "invalid value '" + h.str() +
                  "' for lifetime-check option 'history'";
          return;
        }
        val |= option;
      }
      histLimit = hist_limit;
    }

    void parseOptions(LifetimeCheckPass &pass) {
      llvm::SmallVector<llvm::StringRef, 4> remarks;
      llvm::SmallVector<llvm::StringRef, 4> hists;

      for (auto &r : pass.remarksList)
        remarks.push_back(r);

      for (auto &h : pass.historyList)
        hists.push_back(h);

      parseOptions(remarks, hists, pass.historyLimit);
    }

    bool emitRemarkAll() { return val & RemarkAll; }
    bool emitRemarkPsetInvalid() {
      return emitRemarkAll() || val & RemarkPsetInvalid;
    }
    bool emitRemarkPsetAlways() {
      return emitRemarkAll() || val & RemarkPsetAlways;
    }

    bool emitHistoryAll() { return val & HistoryAll; }
    bool emitHistoryNull() { return emitHistoryAll() || val & HistoryNull; }
    bool emitHistoryInvalid() {
      return emitHistoryAll() || val & HistoryInvalid;
    }
  } opts;

  ///
  /// State
  /// -----

  // Represents the state of an element in a pointer set (pset)
  struct State {
    using DataTy = enum {
      Invalid,
      NullPtr,
      Global,
      // FIXME: currently only supports one level of OwnedBy!
      OwnedBy,
      LocalValue,
      NumKindsMinusOne = LocalValue
    };
    State() { val.setInt(Invalid); }
    State(DataTy d) { val.setInt(d); }
    State(mlir::Value v, DataTy d = LocalValue) {
      assert((d == LocalValue || d == OwnedBy) && "expected value or owned");
      val.setPointerAndInt(v, d);
    }

    static constexpr int KindBits = 3;
    static_assert((1 << KindBits) > NumKindsMinusOne,
                  "Not enough room for kind!");
    llvm::PointerIntPair<mlir::Value, KindBits> val;

    /// Provide less/equal than operator for sorting / set ops.
    bool operator<(const State &RHS) const {
      if (val.getInt() != RHS.val.getInt())
        return val.getInt() < RHS.val.getInt();
      return hasValue() && std::less<const void *>()(
                               val.getPointer().getAsOpaquePointer(),
                               RHS.val.getPointer().getAsOpaquePointer());
    }
    bool operator==(const State &RHS) const {
      return val.getInt() == RHS.val.getInt() &&
             (!hasValue() || val.getPointer() == RHS.val.getPointer());
    }

    bool isLocalValue() const { return val.getInt() == LocalValue; }
    bool isOwnedBy() const { return val.getInt() == OwnedBy; }
    bool hasValue() const { return isLocalValue() || isOwnedBy(); }

    mlir::Value getData() const {
      assert(hasValue() && "data type does not hold a mlir::Value");
      return val.getPointer();
    }

    void dump(llvm::raw_ostream &OS = llvm::errs(), int ownedGen = 0);

    static State getInvalid() { return {Invalid}; }
    static State getNullPtr() { return {NullPtr}; }
    static State getGlobal() { return {Global}; }
    static State getLocalValue(mlir::Value v) { return {v, LocalValue}; }
    static State getOwnedBy(mlir::Value v) { return {v, State::OwnedBy}; }
  };

  ///
  /// Invalid and null history tracking
  /// ---------------------------------
  enum InvalidStyle {
    Unknown,
    EndOfScope,
    NotInitialized,
    MovedFrom,
    NonConstUseOfOwner,
  };

  struct InvalidHistEntry {
    InvalidStyle style = Unknown;
    std::optional<mlir::Location> loc;
    std::optional<mlir::Value> val;
    InvalidHistEntry() = default;
    InvalidHistEntry(InvalidStyle s, std::optional<mlir::Location> l,
                     std::optional<mlir::Value> v)
        : style(s), loc(l), val(v) {}
    bool operator==(const InvalidHistEntry &other) const {
      return style == other.style && loc == other.loc && val == other.val;
    }
  };

  struct InvalidHist {
    llvm::SmallVector<InvalidHistEntry, 0> entries;
    void add(mlir::Value ptr, InvalidStyle invalidStyle, mlir::Location loc,
             std::optional<mlir::Value> val = {}) {
      InvalidHistEntry entry(invalidStyle, loc, val);
      if (!llvm::is_contained(entries, entry))
        entries.push_back(entry);
    }
    bool operator==(const InvalidHist &other) const {
      return entries == other.entries;
    }
    bool operator!=(const InvalidHist &other) const {
      return !(*this == other);
    }
  };

  llvm::DenseMap<mlir::Value, InvalidHist> invalidHist;

  using PMapNullHistType =
      llvm::DenseMap<mlir::Value, std::optional<mlir::Location>>;
  PMapNullHistType pmapNullHist;

  // Track emitted diagnostics, and do not repeat them.
  DenseMap<Value, llvm::SmallSet<Location, 8, LocOrdering>>
      emittedObjectDiagnostics;

  ///
  /// Pointer Map and Pointer Set
  /// ---------------------------

  using PSetType = llvm::SmallSet<State, 4>;
  // FIXME: this should be a ScopedHashTable for consistency.
  using PMapType = llvm::DenseMap<mlir::Value, PSetType>;

  // Object identities are stable for the duration of a function analysis. In
  // particular, a load of an address is a value, not a new storage identity.
  using ObjectId = mlir::Value;
  struct ObjectTargets {
    llvm::TinyPtrVector<ObjectId> objects;
    bool unknown = false;
    bool mayBeNull = false;
    bool mayBeInvalid = false;
    bool isDefinite() const {
      return objects.size() == 1 && !unknown && !mayBeNull && !mayBeInvalid;
    }
    bool operator==(const ObjectTargets &other) const {
      return ArrayRef<ObjectId>(objects) == ArrayRef<ObjectId>(other.objects) &&
             unknown == other.unknown && mayBeNull == other.mayBeNull &&
             mayBeInvalid == other.mayBeInvalid;
    }
    bool operator!=(const ObjectTargets &other) const {
      return !(*this == other);
    }
  };
  struct PointerValueSummary {
    ObjectTargets targets;
    bool mayBeNonNull = false;
    bool unknownNullness = false;
    llvm::TinyPtrVector<ObjectId> borrowedOwners;
    InvalidHist invalidHistory;
    std::optional<Location> nullOrigin;
    bool operator==(const PointerValueSummary &other) const {
      return targets == other.targets && mayBeNonNull == other.mayBeNonNull &&
             unknownNullness == other.unknownNullness &&
             ArrayRef<ObjectId>(borrowedOwners) ==
                 ArrayRef<ObjectId>(other.borrowedOwners) &&
             invalidHistory == other.invalidHistory &&
             nullOrigin == other.nullOrigin;
    }
    bool operator!=(const PointerValueSummary &other) const {
      return !(*this == other);
    }
  };
  struct OccupancySummary {
    bool mayBeEmpty = false;
    bool mayBePresent = false;
    bool unknown = true;
    SmallVector<Location, 0> emptyOrigins;
    bool operator==(const OccupancySummary &other) const {
      return mayBeEmpty == other.mayBeEmpty &&
             mayBePresent == other.mayBePresent && unknown == other.unknown &&
             emptyOrigins == other.emptyOrigins;
    }
    bool operator!=(const OccupancySummary &other) const {
      return !(*this == other);
    }
  };
  struct ValuePredicate {
    enum Domain {
      PointerNullness,
      OwnerOccupancy,
      OptionalEngagement,
      ConditionalMove
    };
    struct ObjectSnapshot {
      ObjectId object;
      PSetType pset;
      InvalidHist history;
      std::optional<Location> nullOrigin;
      std::optional<PointerValueSummary> pointer;
      OccupancySummary occupancy;
      std::optional<OccupancySummary> valueOccupancy;
      SmallVector<ObjectId, 2> ownershipGroup;
      bool valid = true;
      bool ownershipValid = true;
      Operation *expectedWrite = nullptr;
      bool operator==(const ObjectSnapshot &other) const {
        return object == other.object && pset == other.pset &&
               history == other.history && nullOrigin == other.nullOrigin &&
               pointer == other.pointer && occupancy == other.occupancy &&
               valueOccupancy == other.valueOccupancy &&
               ownershipGroup == other.ownershipGroup && valid == other.valid &&
               ownershipValid == other.ownershipValid &&
               expectedWrite == other.expectedWrite;
      }
    };
    // Pointer values loaded before an insertion also participate in the
    // conditional resource transfer. Keep their payloads separately from
    // storage facts, so refinement cannot undo an intervening assignment.
    struct PointerSnapshot {
      Value value;
      SmallVector<unsigned, 0> tuplePath;
      std::optional<PointerValueSummary> pointer;
      bool stored = false;
      bool valid = true;
      Operation *expectedWrite = nullptr;
      bool operator==(const PointerSnapshot &other) const {
        return value == other.value && tuplePath == other.tuplePath &&
               pointer == other.pointer && stored == other.stored &&
               valid == other.valid && expectedWrite == other.expectedWrite;
      }
    };
    Domain domain = PointerNullness;
    ObjectTargets objects;
    bool trueMeansPresent = true;
    Operation *conditionSite = nullptr;
    SmallVector<ObjectSnapshot, 0> before;
    SmallVector<ObjectSnapshot, 0> after;
    SmallVector<PointerSnapshot, 0> pointersBefore;
    SmallVector<PointerSnapshot, 0> pointersAfter;
    bool operator==(const ValuePredicate &other) const {
      return domain == other.domain && objects == other.objects &&
             trueMeansPresent == other.trueMeansPresent &&
             conditionSite == other.conditionSite && before == other.before &&
             after == other.after && pointersBefore == other.pointersBefore &&
             pointersAfter == other.pointersAfter;
    }
    bool operator!=(const ValuePredicate &other) const {
      return !(*this == other);
    }
  };
  using PredicatePtr = std::shared_ptr<const ValuePredicate>;
  static bool equalPredicates(const PredicatePtr &a, const PredicatePtr &b) {
    return a == b || (a && b && *a == *b);
  }
  struct ValueSummary {
    std::optional<PointerValueSummary> pointer;
    llvm::TinyPtrVector<ObjectId> readOrigins;
    llvm::TinyPtrVector<ObjectId> xvalueOrigins;
    SmallVector<ObjectTargets, 0> tupleElements;
    std::vector<ValueSummary> tupleValues;
    // Path copies share immutable predicates. Mutation creates a new payload,
    // keeping snapshots independent without reserving one in every SSA value.
    PredicatePtr predicate;
    // A known scalar value is separate from a library predicate: it remains
    // the value of this SSA evaluation even if the observed object changes.
    std::optional<bool> booleanValue;
    std::optional<OccupancySummary> objectOccupancy;
    SmallVector<std::optional<ValuePredicate>, 0> elementPredicates;
    bool externallyAliasedPointer = false;
    bool unknown = false;
    bool operator==(const ValueSummary &other) const {
      return pointer == other.pointer &&
             ArrayRef<ObjectId>(readOrigins) ==
                 ArrayRef<ObjectId>(other.readOrigins) &&
             ArrayRef<ObjectId>(xvalueOrigins) ==
                 ArrayRef<ObjectId>(other.xvalueOrigins) &&
             tupleElements == other.tupleElements &&
             tupleValues == other.tupleValues &&
             equalPredicates(predicate, other.predicate) &&
             booleanValue == other.booleanValue &&
             objectOccupancy == other.objectOccupancy &&
             elementPredicates == other.elementPredicates &&
             externallyAliasedPointer == other.externallyAliasedPointer &&
             unknown == other.unknown;
    }
    bool operator!=(const ValueSummary &other) const {
      return !(*this == other);
    }
  };
  struct DiagnosticWitness {
    Operation *operation = nullptr;
    ObjectId object;
    Location location;
    DerefStyle style = Direct;
    PSetType pset;
    InvalidHist history;
    std::optional<Location> nullOrigin;
    DenseMap<ObjectId, unsigned> ownerGenerations;
    DiagnosticWitness(Operation *operation, ObjectId object, Location location,
                      DerefStyle style)
        : operation(operation), object(object), location(location),
          style(style) {}
  };
  struct PathState {
    PMapType pmap;
    DenseMap<ObjectId, InvalidHist> invalidHist;
    PMapNullHistType nullHist;
    DenseMap<Value, ValueSummary> values;
    DenseMap<ObjectId, ValueSummary> storedValues;
    DenseMap<ObjectId, OccupancySummary> occupancy;
    SmallVector<ObjectId, 16> liveObjects;
    SmallVector<DiagnosticWitness, 0> diagnostics;
    DenseMap<ObjectId, Operation *> lastWrites;
    DenseMap<ObjectId, SmallVector<Operation *, 2>> ownerMutations;
    // An entry records holders proven to own the same resource on every path.
    // Pointer occupancy is independent: an aliasing shared_ptr can own a
    // resource while its stored pointer is null.
    DenseMap<ObjectId, SmallVector<ObjectId, 2>> ownershipGroups;
  };
  struct ExceptionalState {
    PathState state;
    Type thrownType;
    FlatSymbolRefAttr typeInfo;
    ExceptionalState(PathState state, Type thrownType = {},
                     FlatSymbolRefAttr typeInfo = {})
        : state(std::move(state)), thrownType(thrownType), typeInfo(typeInfo) {}
  };
  struct GotoState {
    PathState state;
    StringAttr label;
  };
  struct FlowResult {
    std::optional<PathState> normal;
    SmallVector<PathState, 0> breaks;
    SmallVector<PathState, 0> continues;
    SmallVector<PathState, 0> returns;
    SmallVector<ExceptionalState, 0> exceptions;
    SmallVector<GotoState, 0> gotos;
    FlowResult() = default;
    FlowResult(PathState state) : normal(std::move(state)) {}
  };
  struct CallInfo {
    Operation *op = nullptr;
    FuncOp callee;
    LifetimeFunctionAttr lifetime;
    SmallVector<Value> args;
    CallParameterKinds parameterKinds;
    ObjectTargets receiver;
    bool mayThrow = false;
  };
  struct ObjectInfo {
    enum Category { Scalar, Pointer, Owner, Aggregate, Record };
    Type type;
    Category category = Scalar;
    bool callerOwned = false;
    unsigned fieldDepth = 0;
    ObjectId storageRoot;
    bool unknownIncomingNullness = false;
  };

  DenseMap<ObjectId, ObjectInfo> objectInfo;
  DenseMap<Value, unsigned> valueOrder;
  DenseMap<Value, ObjectId> canonicalObjects;
  DenseMap<Operation *, SmallVector<ObjectId, 0>> scopeLocals;
  PathState *activeState = nullptr;
  Operation *activeOperation = nullptr;
  bool emittingWitnesses = false;
  SmallVector<DiagnosticWitness, 16> functionWitnesses;
  FlatSymbolRefAttr currentExceptionType;
  Type currentExceptionIRType;

  ObjectTargets resolveAddress(Value, const PathState &) const;
  ValueSummary summarizeValue(Value, const PathState &) const;
  void setValueSummary(PathState &, Value, ValueSummary);
  void storeSummary(PathState &, const ObjectTargets &, const ValueSummary &);
  OccupancySummary getOccupancy(const PathState &, const ObjectTargets &) const;
  void setOccupancy(PathState &, const ObjectTargets &,
                    const OccupancySummary &, Location);
  void reinitializeObjects(PathState &, const ObjectTargets &, Location);
  void invalidateOwnerContents(PathState &, const ObjectTargets &, Location);
  void transferOwnerContents(PathState &, const ObjectTargets &,
                             const ObjectTargets &, bool sharedCopy);
  void releaseOwnerContents(PathState &, const ObjectTargets &, Location);
  void endOwnerContents(PathState &, const ObjectTargets &, Location,
                        InvalidStyle);
  void retargetOwnerContents(PathState &, ObjectId, ObjectId,
                             bool retainSource = false);
  void detachOwner(PathState &, ObjectId);
  void replaceOwnerContents(PathState &, const ObjectTargets &,
                            const OccupancySummary &, Operation *);
  // An opaque mutation can replace a holder's contents. Besides invalidating
  // borrowers, forget the occupancy, payload, predicates, and co-ownership
  // that a library contract would otherwise maintain. Only a definite target
  // is strongly updated.
  void mutateOwnerContents(PathState &, const ObjectTargets &, Location);
  // Applies the generic per-argument effects of a call: a reference to a
  // moved-from record is a use, and an rvalue reference consumes its object on
  // both the normal and the exceptional continuation. `skip` excludes
  // arguments whose effects a more specific rule already applied.
  void applyArgumentEffects(const CallInfo &, PathState &normal,
                            PathState *exceptional,
                            llvm::function_ref<bool(unsigned)> skip);
  // checkCall applies the owner and pointer method rules only to a definite
  // GSL receiver. Apply the same effects to every candidate of a may-alias
  // receiver, and to a definite standard-library holder outside those
  // categories.
  void applyReceiverEffects(const CallInfo &, PathState &, Attribute special);
  bool hasLibraryContents(ObjectId) const;
  void markMoved(PathState &, const ObjectTargets &, Location);
  bool checkUse(PathState &, const ObjectTargets &, Location,
                DerefStyle = Direct);
  std::optional<ObjectInfo> getObjectInfo(ObjectId) const;
  // A rejected model leaves state unchanged. A recognized model transfers
  // it into the returned flow, so the caller must not use it again.
  std::optional<FlowResult> modelLibraryCall(const CallInfo &, PathState &);
  ValuePredicate makeConditionalMovePredicate(const PathState &,
                                              const PathState &,
                                              const ObjectTargets &,
                                              Operation *);
  void setConditionalMoveSite(PathState &, const ValuePredicate &);
  void invalidatePredicates(PathState &, const ObjectTargets &);
  void invalidateConditionalDependencies(PathState &, ObjectId, bool storage);
  void invalidateConditionalOwnership(PathState &, ObjectId);
  void copyConditionalPointerSnapshots(PathState &, Value source,
                                       bool sourceStored, Value destination,
                                       bool destinationStored);
  static void
  visitPredicates(PathState &,
                  llvm::function_ref<void(std::optional<ValuePredicate> &)>);

  void joinStateInto(PathState &, const PathState &);
  PathState joinStates(ArrayRef<const PathState *>);
  bool equivalentStates(const PathState &, const PathState &);
  void mergeFlow(FlowResult &, FlowResult);
  std::optional<bool> evaluateCondition(Value, const PathState &);
  PathState refineCondition(Value, bool, PathState);
  FlowResult transferOperation(Operation *, PathState);
  LLVM_ATTRIBUTE_NOINLINE FlowResult transferOtherOperation(Operation *,
                                                            PathState);
  FlowResult analyzeRegion(Region &, PathState, bool scoped = false);
  LLVM_ATTRIBUTE_NOINLINE FlowResult analyzeRegionImpl(Region &, PathState,
                                                       bool scoped);
  FlowResult analyzeBlock(Block &, PathState);
  FlowResult analyzeLoop(LoopOpInterface, PathState);
  FlowResult analyzeCall(Operation *, PathState);
  FlowResult leaveScope(FlowResult, ArrayRef<ObjectId>, Location);
  PathState finishRegionOperation(Operation *, ValueRange, PathState);
  FlowResult leaveOperationScope(Operation *, FlowResult);
  void indexObjects(FuncOp);
  void emitWitnesses(const FlowResult &);
  void recordWitness(PathState &, DiagnosticWitness);
  void initializeObject(PathState &, ObjectId, Location);
  void transferLoad(LoadOp, PathState &);
  void transferStore(StoreOp, PathState &);
  void forgetAggregateCallFields(PathState &, const ObjectTargets &);
  void transferCopy(CopyOp, PathState &);
  void checkAddressUse(Value, Location, PathState &);
  void synchronizeSummaries(PathState &);
  ValueSummary joinValues(const ValueSummary &, const ValueSummary &) const;
  static void
  visitValueSummary(ValueSummary &summary,
                    llvm::function_ref<void(ValueSummary &)> visit) {
    visit(summary);
    for (auto &element : summary.tupleValues)
      visitValueSummary(element, visit);
  }
  static void clearBooleanValues(ValueSummary &summary) {
    visitValueSummary(summary,
                      [](ValueSummary &value) { value.booleanValue.reset(); });
  }
  static void clearStoredBooleanValues(PathState &state) {
    for (auto &entry : state.storedValues)
      clearBooleanValues(entry.second);
  }
  unsigned getValueOrder(Value value) const {
    auto found = valueOrder.find(value);
    if (found == valueOrder.end())
      llvm::report_fatal_error("unindexed value in CIR lifetime analysis");
    return found->second;
  }
  template <class Range> void sortValues(Range &values) const {
    llvm::sort(values, [&](Value a, Value b) {
      return getValueOrder(a) < getValueOrder(b);
    });
  }
  ObjectTargets joinTargets(const ObjectTargets &, const ObjectTargets &) const;

  // The legacy lifetime rules operate on the currently installed map. Install
  // all path-dependent maps together while applying one of those rules.
  class StateGuard {
    LifetimeCheckPass &pass;
    PathState &state;
    PMapType *oldMap;
    PathState *oldState;
    DenseMap<ObjectId, InvalidHist> oldHistory;
    PMapNullHistType oldNullHistory;

  public:
    StateGuard(LifetimeCheckPass &pass, PathState &state)
        : pass(pass), state(state), oldMap(pass.currPmap),
          oldState(pass.activeState), oldHistory(std::move(pass.invalidHist)),
          oldNullHistory(std::move(pass.pmapNullHist)) {
      pass.currPmap = &state.pmap;
      pass.activeState = &state;
      pass.invalidHist = std::move(state.invalidHist);
      pass.pmapNullHist = std::move(state.nullHist);
    }
    ~StateGuard() {
      state.invalidHist = std::move(pass.invalidHist);
      state.nullHist = std::move(pass.pmapNullHist);
      pass.invalidHist = std::move(oldHistory);
      pass.pmapNullHist = std::move(oldNullHistory);
      pass.currPmap = oldMap;
      pass.activeState = oldState;
    }
  };

  // FIXME: we probably don't need to track it at this level, perhaps
  // just tracking at the scope level should be enough?
  PMapType *currPmap = nullptr;
  PMapType &getPmap() { return *currPmap; }
  void markPsetInvalid(mlir::Value ptr, InvalidStyle invalidStyle,
                       mlir::Location loc,
                       std::optional<mlir::Value> extraVal = {}) {
    auto &pset = getPmap()[ptr];

    // If pset is already invalid, don't bother.
    if (pset.count(State::getInvalid()))
      return;

    // 2.3 - putting invalid into pset(x) is said to invalidate it
    pset.insert(State::getInvalid());
    invalidHist[ptr].add(ptr, invalidStyle, loc, extraVal);
  }

  void markPsetNull(mlir::Value addr, mlir::Location loc,
                    InvalidStyle invalidStyle = InvalidStyle::Unknown) {
    getPmap()[addr].clear();
    getPmap()[addr].insert(State::getNullPtr());
    invalidHist.erase(addr);
    pmapNullHist[addr] = loc;
    // Also record in invalidHist for diagnostic history
    if (invalidStyle != InvalidStyle::Unknown)
      invalidHist[addr].add(addr, invalidStyle, loc);
  }

  void copyPset(mlir::Value dst, mlir::Value src);

  // Provides p1179's 'KILL' functionality. See implementation for more
  // information.
  void kill(const State &s, InvalidStyle invalidStyle, mlir::Location loc);
  void killInPset(mlir::Value ptrKey, const State &s, InvalidStyle invalidStyle,
                  mlir::Location loc, std::optional<mlir::Value> extraVal);

  // Local pointers
  SmallPtrSet<mlir::Value, 8> ptrs;

  // Local owners. We use a map instead of a set to track the current generation
  // for this owner type internal pointee's. For instance, this allows tracking
  // subsequent reuse of owner storage when a non-const use happens.
  DenseMap<mlir::Value, unsigned> owners;
  void addOwner(mlir::Value o) { owners.try_emplace(o, 0); }
  void incOwner(mlir::Value o) {
    assert(owners.count(o) && "entry expected");
    if (activeState) {
      auto &sites = activeState->ownerMutations[o];
      if (!llvm::is_contained(sites, activeOperation))
        sites.push_back(activeOperation);
      owners[o] = sites.size();
    } else {
      ++owners[o];
    }
  }

  // Aggregates and exploded fields.
  using ExplodedFieldsTy = llvm::SmallVector<mlir::Value, 4>;
  DenseMap<mlir::Value, ExplodedFieldsTy> aggregates;
  // Useful helpers for debugging
  void printPset(PSetType &pset, llvm::raw_ostream &OS = llvm::errs());
  LLVM_DUMP_METHOD void dumpPmap(PMapType &pmap);
  LLVM_DUMP_METHOD void dumpCurrentPmap();

  ///
  /// Coroutine tasks (promise_type)
  /// ------------------------------

  // Track types we already know to be a coroutine task (promise_type)
  llvm::DenseMap<mlir::Type, bool> IsTaskTyCache;
  // Is the type associated with taskVal a coroutine task? Use IsTaskTyCache
  // or the serialized lifetime metadata.
  bool isTaskType(mlir::Value taskVal);
  // Addresses of coroutine Tasks found in the current function.
  SmallPtrSet<mlir::Value, 8> tasks;

  ///
  /// Lambdas
  /// -------

  // Track types we already know to be a lambda
  llvm::DenseMap<mlir::Type, bool> IsLambdaTyCache;
  // Track types we already know to be smart pointers
  llvm::DenseMap<mlir::Type, bool> IsSmartPointerTyCache;
  // Check if a given cir type is a record containing a lambda
  bool isLambdaType(mlir::Type ty);
  // Get the lambda record from a member access to it.
  mlir::Value getLambdaFromMemberAccess(mlir::Value addr);

  ///
  /// Scope, context and guards
  /// -------------------------

  // Represents the scope context for IR operations (cir.scope, cir.if,
  // then/else regions, etc). Tracks the declaration of variables in the current
  // local scope.
  struct LexicalScopeContext {
    LexicalScopeContext() = delete;

    llvm::PointerUnion<mlir::Region *, mlir::Operation *> parent;
    LexicalScopeContext(mlir::Region *R) : parent(R) {}
    LexicalScopeContext(mlir::Operation *Op) : parent(Op) {}
    ~LexicalScopeContext() = default;

    // Track all local values added in this scope
    SmallPtrSet<mlir::Value, 4> localValues;

    // Track the result of temporaries with coroutine call results,
    // they are used to initialize a task.
    //
    // Value must come directly out of a cir.call to a cir.func which
    // is a coroutine.
    SmallPtrSet<mlir::Value, 2> localTempTasks;

    // Track seen lambdas that escape out of the current scope
    // (e.g. lambdas returned out of functions).
    DenseMap<mlir::Value, mlir::Location> localRetLambdas;

    LLVM_DUMP_METHOD void dumpLocalValues();
  };

  LexicalScopeContext *currScope = nullptr;
};
} // namespace

static std::string getVarNameFromValue(mlir::Value v) {
  auto srcOp = v.getDefiningOp();
  if (!srcOp) {
    auto blockArg = cast<BlockArgument>(v);
    if (!blockArg.getOwner()->isEntryBlock())
      return "<unnamed>";
    bool isReferenceParameter = false;
    if (auto function = dyn_cast<FuncOp>(blockArg.getOwner()->getParentOp())) {
      auto metadata = function.getLifetimeAttr();
      if (metadata && metadata.hasParameterInfo())
        for (unsigned i = 0; i < metadata.getNumParams(); ++i)
          isReferenceParameter |=
              metadata.getParamOperandIndex(i) == blockArg.getArgNumber() &&
              metadata.getParameterKinds().asArrayRef()[i] != 0;
    }
    if (isReferenceParameter)
      for (Operation *user : v.getUsers())
        if (auto store = dyn_cast<StoreOp>(user))
          if (store.getValue() == v)
            if (auto slot = store.getAddr().getDefiningOp<AllocaOp>())
              return slot.getName().str();
    llvm::SmallString<128> finalName;
    llvm::raw_svector_ostream Out(finalName);
    Out << "fn_arg:" << blockArg.getArgNumber();
    return Out.str().str();
  }

  if (auto allocaOp = dyn_cast<AllocaOp>(srcOp))
    return allocaOp.getName().str();
  if (auto memberOp = dyn_cast<GetMemberOp>(srcOp))
    return getVarNameFromValue(memberOp.getAddr()) + "." +
           memberOp.getName().str();
  if (auto callOp = dyn_cast<CallOp>(srcOp)) {
    if (callOp.getCallee()) {
      llvm::SmallString<128> finalName;
      llvm::raw_svector_ostream Out(finalName);
      Out << "call:" << callOp.getCallee()->str();
      return Out.str().str();
    }
  }
  // Analysis state is keyed by SSA values, including values with no source
  // variable name. A diagnostic label must not restrict which values it holds.
  return "<unnamed>";
}

static Location getEndLoc(Location loc, int idx = 1) {
  auto fusedLoc = llvm::dyn_cast<FusedLoc>(loc);
  if (!fusedLoc)
    return loc;
  auto locations = fusedLoc.getLocations();
  return idx < static_cast<int>(locations.size()) ? locations[idx] : loc;
}

static Location getEndLocForHist(Operation *op) {
  // Compiler-generated cleanup scopes can have an unknown location. Use the
  // enclosing source scope for their end-of-lifetime history when available.
  for (Operation *parent = op; parent; parent = parent->getParentOp()) {
    if (!isa<UnknownLoc>(parent->getLoc()))
      return getEndLoc(parent->getLoc());
  }
  return op->getLoc();
}

void LifetimeCheckPass::killInPset(mlir::Value ptrKey, const State &s,
                                   InvalidStyle invalidStyle,
                                   mlir::Location loc,
                                   std::optional<mlir::Value> extraVal) {
  auto &pset = getPmap()[ptrKey];
  if (pset.contains(s)) {
    pset.erase(s);
    markPsetInvalid(ptrKey, invalidStyle, loc, extraVal);
  }
}

// 2.3 - KILL(x) means to replace all occurrences of x and x' and x'' (etc.)
// in the pmap with invalid. For example, if pmap is {(p1,{a}), (p2,{a'})},
// KILL(a') would invalidate only p2, and KILL(a) would invalidate both p1 and
// p2.
void LifetimeCheckPass::kill(const State &s, InvalidStyle invalidStyle,
                             mlir::Location loc) {
  assert(s.hasValue() && "does not know how to kill other data types");
  mlir::Value v = s.getData();
  std::optional<mlir::Value> extraVal;
  if (invalidStyle == InvalidStyle::EndOfScope)
    extraVal = v;

  // Ending the holder's storage does not end a shared resource while another
  // proven holder remains alive. Retarget its borrowers before killing the
  // storage itself; the recursive OwnedBy kill cannot re-enter this branch.
  if (activeState && s.isLocalValue() && owners.count(v))
    endOwnerContents(*activeState, ObjectTargets{{v}}, loc, invalidStyle);

  // Captured SSA pointer values continue to refer to the same storage even
  // after their source slot changes. Invalidate these snapshots along with
  // the pointer map, including values loaded before an owner mutation.
  if (activeState) {
    invalidateConditionalDependencies(*activeState, v, s.isLocalValue());
    auto invalidate = [&](ValueSummary &value) {
      if (!value.pointer)
        return;
      bool affected = llvm::is_contained(value.pointer->borrowedOwners, v) ||
                      (s.isLocalValue() &&
                       llvm::is_contained(value.pointer->targets.objects, v));
      if (affected) {
        value.pointer->targets.mayBeInvalid = true;
        value.pointer->invalidHistory.add(v, invalidStyle, loc, extraVal);
      }
    };
    for (auto &entry : activeState->values)
      visitValueSummary(entry.second, invalidate);
    for (auto &entry : activeState->storedValues)
      visitValueSummary(entry.second, invalidate);
  }

  for (auto &mapEntry : getPmap()) {
    auto ptr = mapEntry.first;

    // We are deleting this entry anyways, nothing to do here.
    if (v == ptr)
      continue;

    // ... replace all occurrences of x and x' and x''. Start with the primes
    // so we first remove uses and then users.
    //
    // FIXME: add x'', x''', etc...
    if (s.isLocalValue() && owners.count(v))
      killInPset(ptr, State::getOwnedBy(v), invalidStyle, loc, extraVal);
    killInPset(ptr, s, invalidStyle, loc, extraVal);
  }

  // Delete the local value from pmap, since its scope has ended.
  if (invalidStyle == InvalidStyle::EndOfScope) {
    if (activeState)
      llvm::erase(activeState->liveObjects, v);
  }
}

void LifetimeCheckPass::checkFunc(cir::FuncOp fnOp) {
  currFunc = fnOp;
  pmapNullHist.clear();
  invalidHist.clear();
  emittedObjectDiagnostics.clear();
  functionWitnesses.clear();
  indexObjects(fnOp);
  PathState initial;
  LexicalScopeContext scope{fnOp.getOperation()};
  llvm::SaveAndRestore<LexicalScopeContext *> scopeGuard(currScope, &scope);
  for (auto entry : objectInfo)
    if (entry.second.callerOwned)
      initializeObject(initial, entry.first, fnOp.getLoc());
  auto result = analyzeRegion(fnOp.getBody(), std::move(initial));
  if (!result.gotos.empty()) {
    mlir::emitError(
        fnOp.getLoc(),
        "unsupported jump into a nested region in lifetime analysis");
    signalPassFailure();
  }
  result = leaveOperationScope(fnOp, std::move(result));
  emitWitnesses(result);
  currFunc.reset();
}

void LifetimeCheckPass::copyPset(mlir::Value dst, mlir::Value src) {
  if (dst == src)
    return;

  assert(getPmap().count(src) && "expected tracked source pointer");
  auto pset = getPmap().lookup(src);
  if (pset.count(State::getNullPtr())) {
    auto it = pmapNullHist.find(src);
    assert(it != pmapNullHist.end() && "expected source nullptr history");
    // Copy before insertion, which can invalidate DenseMap iterators.
    auto history = it->second;
    pmapNullHist[dst] = history;
  }
  if (pset.count(State::getInvalid())) {
    auto it = invalidHist.find(src);
    assert(it != invalidHist.end() && "expected source invalid history");
    auto history = it->second;
    invalidHist[dst] = std::move(history);
  }

  // Histories are shared across control-flow paths. Keep histories for states
  // absent from this source: another path may contribute them at the join.
  getPmap()[dst] = std::move(pset);
}

void LifetimeCheckPass::checkReturn(ReturnOp retOp) {
  // Upon return invalidate all local values. Since some return
  // values might depend on other local address, check for the
  // dangling aspects for this.
  if (retOp.getNumOperands() == 0)
    return;

  auto retTy = retOp.getOperand(0).getType();
  // FIXME: this can be extended to cover more leaking/dandling
  // semantics out of functions.
  if (!isLambdaType(retTy))
    return;

  // The return value is loaded from the return slot before
  // returning.
  auto loadOp = retOp.getOperand(0).getDefiningOp<cir::LoadOp>();
  assert(loadOp && "expected cir.load");
  if (!loadOp.getAddr().getDefiningOp<cir::AllocaOp>())
    return;

  // A fixed point can revisit the same return slot. Its static source site
  // remains the same, while each path carries its own captured lifetimes.
  currScope->localRetLambdas.try_emplace(loadOp.getAddr(), loadOp.getLoc());
}

bool LifetimeCheckPass::isOwnerType(mlir::Type ty) {
  // From 2.1:
  //
  // An Owner uniquely owns another object (cannot dangle). An Owner type is
  // expressed using the annotation [[gsl::Owner(DerefType)]] where DerefType is
  // the owned type (and (DerefType) may be omitted and deduced as below). For
  // example:
  //
  // template<class T> class [[gsl::Owner(T)]] my_unique_smart_pointer;
  //
  // TODO: The following standard or other types are treated as-if annotated as
  // Owners, if not otherwise annotated and if not SharedOwners:
  //
  // - Every type that satisfies the standard Container requirements and has a
  // user-provided destructor. (Example: vector.) DerefType is ::value_type.
  // - Every type that provides unary * and has a user-provided destructor.
  // (Example: unique_ptr.) DerefType is the ref-unqualified return type of
  // operator*.
  // - Every type that has a data member or public base class of an Owner type.
  // Additionally, for convenient adoption without modifying existing standard
  // library headers, the following well known standard types are treated as-if
  // annotated as Owners: stack, queue, priority_queue, optional, variant, any,
  // and regex.
  auto recordType = dyn_cast<cir::RecordType>(ty);
  if (!recordType)
    return false;
  auto lifetime = cir::getLifetimeRecord(recordType, theModule);
  return lifetime && lifetime.getOwner();
}

bool LifetimeCheckPass::isSmartPointerType(mlir::Type ty) {
  auto originalTy = ty;
  if (IsSmartPointerTyCache.count(originalTy))
    return IsSmartPointerTyCache[originalTy];

  if (auto ptrType = dyn_cast<cir::PointerType>(ty))
    ty = ptrType.getPointee();
  auto recordType = dyn_cast<cir::RecordType>(ty);
  auto lifetime = recordType ? cir::getLifetimeRecord(recordType, theModule)
                             : cir::LifetimeRecordAttr{};
  bool result = lifetime && lifetime.getSmartPointer();
  IsSmartPointerTyCache[originalTy] = result;
  return result;
}

bool LifetimeCheckPass::isSkippableTemporary(mlir::Value v) {
  auto allocaOp = v.getDefiningOp<cir::AllocaOp>();
  if (!allocaOp)
    return false;

  auto name = allocaOp.getName();
  // Temporaries have names starting with "ref.tmp"
  // FIXME: "ref.tmp" naming is not reliable. Consider adding a unit attribute
  // is_temporary to AllocaOp that is set by CIRGen to definitively mark
  // temporaries. This would be more robust than string prefix matching.
  if (!name.starts_with("ref.tmp"))
    return false;

  // Don't skip coroutine tasks - they need lifetime tracking even as
  // temporaries since they may be captured by the coroutine frame
  if (isTaskType(v))
    return false;

  return true;
}

static bool containsPointerElts(cir::RecordType s) {
  auto members = s.getMembers();
  return std::any_of(members.begin(), members.end(), [](mlir::Type t) {
    return mlir::isa<cir::PointerType>(t);
  });
}

static bool isAggregateType(LifetimeCheckPass *pass, mlir::Type agg) {
  auto t = mlir::dyn_cast<cir::RecordType>(agg);
  if (!t)
    return false;
  // Lambdas have their special handling, and shall not be considered as
  // aggregate types.
  if (pass->isLambdaType(agg))
    return false;
  // FIXME: For now we handle this in a more naive way: any pointer
  // element we find is enough to consider this an aggregate. But in
  // reality it should be as defined in 2.1:
  //
  // An Aggregate is a type that is not an Indirection and is a class type with
  // public data members none of which are references (& or &&) and no
  // user-provided copy or move operations, and no base class that is not also
  // an Aggregate. The elements of an Aggregate are its public data members.
  return containsPointerElts(t);
}

bool LifetimeCheckPass::isPointerType(mlir::Type t) {
  // From 2.1:
  //
  // A Pointer is not an Owner and provides indirect access to an object it does
  // not own (can dangle). A Pointer type is expressed using the annotation
  // [[gsl::Pointer(DerefType)]] where DerefType is the pointed-to type (and
  // (Dereftype) may be omitted and deduced as below). For example:
  //
  // template<class T> class [[gsl::Pointer(T)]] my_span;
  //
  // TODO: The following standard or other types are treated as-if annotated as
  // Pointer, if not otherwise annotated and if not Owners:
  //
  // - Every type that satisfies the standard Iterator requirements. (Example:
  // regex_iterator.) DerefType is the ref-unqualified return type of operator*.
  // - Every type that satisfies the Ranges TS Range concept. (Example:
  // basic_string_view.) DerefType is the ref-unqualified type of *begin().
  // - Every type that satisfies the following concept. DerefType is the
  // ref-unqualified return type of operator*.
  //
  //  template<typename T> concept
  //  TriviallyCopyableAndNonOwningAndDereferenceable =
  //  std::is_trivially_copyable_v<T> && std::is_copy_constructible_v<T> &&
  //  std::is_copy_assignable_v<T> && requires(T t) { *t; };
  //
  // - Every closure type of a lambda that captures by reference or captures a
  // Pointer by value. DerefType is void.
  // - Every type that has a data member or public base class of a Pointer type.
  // Additionally, for convenient adoption without modifying existing standard
  // library headers, the following well- known standard types are treated as-if
  // annotated as Pointers, in addition to raw pointers and references: ref-
  // erence_wrapper, and vector<bool>::reference.
  if (mlir::isa<cir::PointerType>(t))
    return true;
  auto recordType = dyn_cast<cir::RecordType>(t);
  if (!recordType)
    return false;
  auto lifetime = cir::getLifetimeRecord(recordType, theModule);
  return lifetime && lifetime.getPointer();
}

void LifetimeCheckPass::checkCoroTaskStore(StoreOp storeOp) {
  // Given:
  //  auto task = [init task];
  // Extend pset(task) such that:
  //  pset(task) = pset(task) U {any local values used to init task}
  auto taskTmp = storeOp.getValue();
  // FIXME: check it's initialization 'init' attr.
  auto taskAddr = storeOp.getAddr();

  // Take the following coroutine creation pattern:
  //
  //   %task = cir.alloca ...
  //   cir.scope {
  //     %arg0 = cir.alloca ...
  //     ...
  //     %tmp_task = cir.call @corotine_call(%arg0, %arg1, ...)
  //     cir.store %tmp_task, %task
  //     ...
  //   }
  //
  // Bind values that are coming from alloca's (like %arg0 above) to the
  // pset of %task - this effectively leads to some invalidation of %task
  // when %arg0 finishes its lifetime at the end of the enclosing cir.scope.
  if (auto call = taskTmp.getDefiningOp<cir::CallOp>()) {
    bool potentialTaintedTask = false;
    for (auto arg : call.getArgOperands()) {
      auto alloca = arg.getDefiningOp<cir::AllocaOp>();
      if (alloca && currScope->localValues.count(alloca)) {
        getPmap()[taskAddr].insert(State::getLocalValue(alloca));
        potentialTaintedTask = true;
      }
    }

    // Task are only interesting when there are local addresses leaking
    // via the coroutine creation, only track those.
    if (potentialTaintedTask)
      tasks.insert(taskAddr);
    return;
  }
  llvm_unreachable("expecting cir.call defining op");
}

mlir::Value LifetimeCheckPass::getLambdaFromMemberAccess(mlir::Value addr) {
  // FIXME: we likely want to consider more indirections here...
  auto op = addr.getDefiningOp<cir::GetMemberOp>();
  if (!op)
    return nullptr;
  auto allocaOp = op->getOperand(0).getDefiningOp<cir::AllocaOp>();
  if (!allocaOp || !isLambdaType(allocaOp.getAllocaType()))
    return nullptr;
  return allocaOp;
}

void LifetimeCheckPass::checkLambdaCaptureStore(StoreOp storeOp) {
  auto localByRefAddr = storeOp.getValue();
  auto lambdaCaptureAddr = storeOp.getAddr();

  if (!localByRefAddr.getDefiningOp<cir::AllocaOp>())
    return;
  auto lambdaAddr = getLambdaFromMemberAccess(lambdaCaptureAddr);
  if (!lambdaAddr)
    return;

  if (currScope->localValues.count(localByRefAddr))
    getPmap()[lambdaAddr].insert(State::getLocalValue(localByRefAddr));
}

void LifetimeCheckPass::updatePointsToForConstRecord(mlir::Value addr,
                                                     cir::ConstRecordAttr value,
                                                     mlir::Location loc) {
  assert(aggregates.count(addr) && "expected association with aggregate");
  int memberIdx = 0;
  for (auto &attr : value.getMembers()) {
    auto ta = mlir::dyn_cast<mlir::TypedAttr>(attr);
    assert(ta && "expected typed attribute");
    auto fieldAddr = aggregates[addr][memberIdx];
    // Unseen fields are not tracked.
    if (fieldAddr && mlir::isa<cir::PointerType>(ta.getType())) {
      updatePointsToForConstPointer(fieldAddr, ta, loc);
    }
    memberIdx++;
  }
}

void LifetimeCheckPass::updatePointsToForConstPointer(mlir::Value addr,
                                                      mlir::TypedAttr value,
                                                      mlir::Location loc) {
  if (isa<cir::UndefAttr, cir::PoisonAttr>(value)) {
    getPmap()[addr].clear();
    markPsetInvalid(addr, InvalidStyle::NotInitialized, loc);
    return;
  }
  if (auto ptr = dyn_cast<cir::ConstPtrAttr>(value); ptr && ptr.isNullValue()) {
    markPsetNull(addr, loc);
    return;
  }

  // Global views (including references bound to constexpr variables) and
  // non-null constant addresses do not refer to automatic storage. This
  // records their lifetime, not whether an arbitrary address is
  // dereferenceable.
  getPmap()[addr].clear();
  getPmap()[addr].insert(State::getGlobal());
}

void LifetimeCheckPass::updatePointsToForZeroRecord(mlir::Value addr,
                                                    RecordType sTy,
                                                    mlir::Location loc) {
  assert(aggregates.count(addr) && "expected association with aggregate");
  int memberIdx = 0;
  for (auto &t : sTy.getMembers()) {
    auto fieldAddr = aggregates[addr][memberIdx];
    // Unseen fields are not tracked.
    if (fieldAddr && mlir::isa<cir::PointerType>(t)) {
      markPsetNull(fieldAddr, loc);
    }
    memberIdx++;
  }
}

static mlir::Operation *ignoreBitcasts(mlir::Operation *op) {
  while (auto bitcast = dyn_cast<CastOp>(op)) {
    if (bitcast.getKind() != CastKind::bitcast)
      return op;
    auto b = bitcast.getSrc().getDefiningOp();
    // Do not handle block arguments just yet.
    if (!b)
      return op;
    op = b;
  }
  return op;
}

void LifetimeCheckPass::updatePointsTo(mlir::Value addr, mlir::Value data,
                                       mlir::Location loc) {

  auto getArrayFromSubscript = [&](PtrStrideOp strideOp) -> mlir::Value {
    auto castOp = strideOp.getBase().getDefiningOp<cir::CastOp>();
    if (!castOp)
      return {};
    if (castOp.getKind() != cir::CastKind::array_to_ptrdecay)
      return {};
    return castOp.getSrc();
  };

  auto dataSrcOp = data.getDefiningOp();

  // Handle function arguments but not all block arguments just yet.
  if (!dataSrcOp) {
    auto blockArg = cast<BlockArgument>(data);
    if (!blockArg.getOwner()->isEntryBlock())
      return;
    getPmap()[addr].clear();
    getPmap()[addr].insert(State::getLocalValue(data));
    return;
  }

  // Ignore chains of bitcasts and update data source. Note that when
  // dataSrcOp gets updated, `data` might not be the most updated resource
  // to use, so avoid using it directly, and instead get things from newer
  // dataSrcOp.
  dataSrcOp = ignoreBitcasts(dataSrcOp);

  // 2.4.2 - If the declaration includes an initialization, the
  // initialization is treated as a separate operation
  if (auto cstOp = dyn_cast<ConstantOp>(dataSrcOp)) {
    // Aggregates can be bulk materialized in CIR, handle proper update of
    // individual exploded fields.
    if (aggregates.count(addr)) {
      if (auto constRecord =
              mlir::dyn_cast<cir::ConstRecordAttr>(cstOp.getValue())) {
        updatePointsToForConstRecord(addr, constRecord, loc);
        return;
      }

      if (auto zero = mlir::dyn_cast<cir::ZeroAttr>(cstOp.getValue())) {
        if (auto zeroRecordTy = dyn_cast<RecordType>(zero.getType())) {
          updatePointsToForZeroRecord(addr, zeroRecordTy, loc);
          return;
        }
      }
      return;
    }

    assert(getPmap().count(addr) && "address should always be valid");
    // 2.4.2 - If the initialization is default initialization or zero
    // initialization, set pset(p) = {null}; for example:
    //
    //  int* p; => pset(p) == {invalid}
    //  int* p{}; or string_view p; => pset(p) == {null}.
    //  int *p = nullptr; => pset(p) == {nullptr} => pset(p) == {null}
    updatePointsToForConstPointer(addr, cast<mlir::TypedAttr>(cstOp.getValue()),
                                  loc);
    return;
  }

  if (auto allocaOp = dyn_cast<AllocaOp>(dataSrcOp)) {
    // p = &x;
    getPmap()[addr].clear();
    getPmap()[addr].insert(State::getLocalValue(allocaOp.getAddr()));
    return;
  }

  if (auto ptrStrideOp = dyn_cast<PtrStrideOp>(dataSrcOp)) {
    // p = &a[0];
    auto array = getArrayFromSubscript(ptrStrideOp);
    if (array) {
      getPmap()[addr].clear();
      getPmap()[addr].insert(State::getLocalValue(array));
    }
    return;
  }

  if (auto getElemOp = dyn_cast<GetElementOp>(dataSrcOp)) {
    getPmap()[addr].clear();
    getPmap()[addr].insert(State::getLocalValue(getElemOp.getBase()));
    return;
  }

  // Initializes ptr types out of known lib calls marked with pointer
  // attributes. TODO: find a better way to tag this.
  if (auto callOp = dyn_cast<CallOp>(dataSrcOp)) {
    // iter = vector<T>::begin()
    getPmap()[addr].clear();
    getPmap()[addr].insert(State::getLocalValue(callOp.getResult()));
  }

  if (auto loadOp = dyn_cast<LoadOp>(dataSrcOp)) {
    // handle indirections through a load, a common example are temporaries
    // copying the 'this' param to a subsequent call.
    updatePointsTo(addr, loadOp.getAddr(), loc);
    return;
  }

  // What should we add next?
}

void LifetimeCheckPass::emitInvalidHistory(mlir::InFlightDiagnostic &D,
                                           mlir::Value histKey,
                                           mlir::Location warningLoc,
                                           DerefStyle derefStyle) {
  assert(invalidHist.count(histKey) && "expected invalid hist");
  auto &hist = invalidHist[histKey];
  unsigned limit = opts.histLimit;

  for (int lastIdx = hist.entries.size() - 1; limit > 0 && lastIdx >= 0;
       lastIdx--, limit--) {
    auto &info = hist.entries[lastIdx];

    switch (info.style) {
    case InvalidStyle::NotInitialized: {
      D.attachNote(info.loc) << "uninitialized here";
      break;
    }
    case InvalidStyle::EndOfScope: {
      if (tasks.count(histKey)) {
        llvm::StringRef resource = "resource";
        if (auto allocaOp = info.val->getDefiningOp<cir::AllocaOp>()) {
          if (isLambdaType(allocaOp.getAllocaType()))
            resource = "lambda";
        }
        D.attachNote((*info.val).getLoc())
            << "coroutine bound to " << resource << " with expired lifetime";
        D.attachNote(info.loc) << "at the end of scope or full-expression";
      } else if (derefStyle == DerefStyle::RetLambda) {
        assert(currFunc && "expected function");
        llvm::StringRef parent = currFunc->getLambda() ? "lambda" : "function";
        D.attachNote(info.val->getLoc())
            << "declared here but invalid after enclosing " << parent
            << " ends";
      } else {
        auto outOfScopeVarName = getVarNameFromValue(*info.val);
        D.attachNote(info.loc) << "pointee '" << outOfScopeVarName
                               << "' invalidated at end of scope";
      }
      break;
    }
    case InvalidStyle::NonConstUseOfOwner: {
      D.attachNote(info.loc) << "invalidated by non-const use of owner type";
      break;
    }
    case InvalidStyle::MovedFrom: {
      D.attachNote(info.loc) << "moved here via std::move or rvalue reference";
      break;
    }
    default:
      llvm_unreachable("unknown history style");
    }
  }
}

bool LifetimeCheckPass::checkPointerDeref(mlir::Value addr, mlir::Location loc,
                                          DerefStyle derefStyle) {
  bool hasInvalid = getPmap()[addr].count(State::getInvalid());
  bool hasNullptr = getPmap()[addr].count(State::getNullPtr());

  if (activeState && !emittingWitnesses) {
    bool invalid = hasInvalid || hasNullptr;
    if (derefStyle == UseAfterMove && isSkippableTemporary(addr))
      invalid = false;
    if (derefStyle == NullableCallParam && !hasInvalid)
      invalid = false;
    if (invalid || opts.emitRemarkPsetAlways()) {
      DiagnosticWitness witness(activeOperation, addr, loc, derefStyle);
      witness.pset = getPmap()[addr];
      witness.history = invalidHist.lookup(addr);
      witness.nullOrigin = pmapNullHist.lookup(addr);
      for (const State &fact : witness.pset)
        if (fact.isOwnedBy())
          witness.ownerGenerations[fact.getData()] =
              activeState->ownerMutations.lookup(fact.getData()).size();
      recordWitness(*activeState, std::move(witness));
    }
    return invalid;
  }

  auto emitPsetRemark = [&] {
    llvm::SmallString<128> psetStr;
    llvm::raw_svector_ostream Out(psetStr);
    printPset(getPmap()[addr], Out);
    emitRemark(loc) << "pset => " << Out.str();
  };

  bool psetRemarkEmitted = false;
  if (opts.emitRemarkPsetAlways()) {
    emitPsetRemark();
    psetRemarkEmitted = true;
  }

  // Only skip temporaries in use-after-move paths — it's possible this can
  // be generalized, but currently only tested for use-after-move.
  if (derefStyle == DerefStyle::UseAfterMove && isSkippableTemporary(addr))
    return false;

  // 2.4.2 - On every dereference of a Pointer p, enforce that p is valid.
  if (!hasInvalid && !hasNullptr)
    return false; // No error

  // TODO: create verbosity/accuracy levels, for now use deref styles directly
  // to decide when not to emit a warning.

  // Neither an indirect call nor binding an lvalue reference proves that the
  // callee dereferences pointer contents. Still check known invalidation.
  if (derefStyle == DerefStyle::NullableCallParam && !hasInvalid)
    return false; // No error

  // Do not emit the same warning twice or more (check AFTER we know there's an
  // error)
  if (emittedObjectDiagnostics[addr].count(loc))
    return true; // Already reported

  // Ok, filtered out questionable warnings, take the bad path leading to this
  // deference point and diagnose it.
  auto varName = getVarNameFromValue(addr);
  auto D = emitWarning(loc);
  emittedObjectDiagnostics[addr].insert(loc);

  bool isValueType = this->isValueType(
      addr); // Use helper method, keep lowerCamel style for locals

  if (tasks.count(addr))
    D << "use of coroutine '" << varName << "' with dangling reference";
  else if (derefStyle == DerefStyle::RetLambda)
    D << "returned lambda captures local variable";
  else if (derefStyle == DerefStyle::CallParam ||
           derefStyle == DerefStyle::NullableCallParam) {
    bool isAgg = addr.getDefiningOp<cir::GetMemberOp>();
    D << "passing ";
    if (!isAgg)
      D << (isValueType ? "moved-from value" : "invalid pointer");
    else
      D << "aggregate containing invalid pointer member";
    D << " '" << varName << "'";
  } else
    D << "use of " << (isValueType ? "moved-from" : "invalid")
      << (isValueType ? " value '" : " pointer '") << varName << "'";

  // TODO: add accuracy levels, different combinations of invalid and null
  // could have different ratios of false positives.
  if (hasInvalid && opts.emitHistoryInvalid())
    emitInvalidHistory(D, addr, loc, derefStyle);

  if (hasNullptr && opts.emitHistoryNull()) {
    if (auto note = pmapNullHist.lookup(addr))
      D.attachNote(*note) << "'nullptr' invalidated here";
  }

  if (!psetRemarkEmitted && opts.emitRemarkPsetInvalid())
    emitPsetRemark();

  return true; // Error was reported
}

static FuncOp getCalleeFromSymbol(mlir::SymbolTable &symbols,
                                  llvm::StringRef name) {
  auto global = symbols.lookup(name);
  assert(global && "expected to find symbol for function");
  return dyn_cast<FuncOp>(global);
}

static cir::LifetimeFunctionAttr getMethod(mlir::SymbolTable &symbols,
                                           CallOp callOp) {
  if (!callOp.getCallee())
    return nullptr;
  llvm::StringRef name = *callOp.getCallee();
  auto method = getCalleeFromSymbol(symbols, name);
  if (!method || method.getBuiltin())
    return nullptr;
  auto lifetime = method.getLifetimeAttr();
  return lifetime && lifetime.getIsMethod() ? lifetime
                                            : cir::LifetimeFunctionAttr{};
}

mlir::Value LifetimeCheckPass::getThisParamPointerCategory(CallOp callOp) {
  if (callOp.getNumArgOperands() == 0)
    return {};
  auto thisptr = callOp.getArgOperand(0);
  if (activeState) {
    auto targets = resolveAddress(thisptr, *activeState);
    if (targets.isDefinite() && ptrs.count(targets.objects.front()))
      return targets.objects.front();
    return {};
  }
  if (ptrs.count(thisptr))
    return thisptr;
  if (auto loadOp = thisptr.getDefiningOp<cir::LoadOp>()) {
    if (ptrs.count(loadOp.getAddr()))
      return loadOp.getAddr();
  }
  // TODO: add a remark to spot 'this' indirections we currently not track.
  return {};
}

mlir::Value LifetimeCheckPass::getThisParamOwnerCategory(CallOp callOp) {
  if (callOp.getNumArgOperands() == 0)
    return {};
  auto thisptr = callOp.getArgOperand(0);
  if (activeState) {
    auto targets = resolveAddress(thisptr, *activeState);
    if (targets.isDefinite() && owners.count(targets.objects.front()))
      return targets.objects.front();
    return {};
  }
  if (owners.count(thisptr))
    return thisptr;
  if (auto loadOp = thisptr.getDefiningOp<cir::LoadOp>()) {
    if (owners.count(loadOp.getAddr()))
      return loadOp.getAddr();
  }
  // TODO: add a remark to spot 'this' indirections we currently not track.
  return {};
}

void LifetimeCheckPass::checkMoveAssignment(CallOp callOp,
                                            cir::LifetimeFunctionAttr m) {
  if (callOp.getNumArgOperands() < 2)
    return;
  // MyPointer::operator=(MyPointer&&)(%dst, %src)
  // or
  // MyOwner::operator=(MyOwner&&)(%dst, %src)
  auto dst = getThisParamPointerCategory(callOp);
  auto src = callOp.getArgOperand(1);

  // Move assignments between pointer categories.
  if (dst && ptrs.count(src)) {
    if (hasInvalidState(src)) {
      checkPointerDeref(src, callOp.getLoc(), DerefStyle::UseAfterMove);
      return;
    }
    // Note that the current pattern here usually comes from a xvalue in src
    // where all the initialization is done, and this move assignment is
    // where we finally materialize it back to the original pointer category.
    copyPset(dst, src);

    // 2.4.2 - It is an error to use a moved-from object.
    // To that intent we mark src's pset with invalid.
    markPointerOrValueTypeAsMovedFrom(src, callOp.getLoc());
    return;
  }

  // Move assignments between owner categories.
  dst = getThisParamOwnerCategory(callOp);
  if (dst && owners.count(src)) {
    if (checkPointerDeref(src, callOp.getLoc(), DerefStyle::UseAfterMove))
      return;
    // Handle as a non const use of owner, invalidating pointers.
    checkNonConstUseOfOwner(dst, callOp.getLoc());

    // 2.4.2 - It is an error to use a moved-from object.
    markOwnerAsMovedFrom(src, callOp.getLoc());
  }
}

void LifetimeCheckPass::checkCopyAssignment(CallOp callOp,
                                            cir::LifetimeFunctionAttr m) {
  if (callOp.getNumArgOperands() < 2)
    return;
  // MyIntOwner::operator=(MyIntOwner&)(%dst, %src)
  auto dst = getThisParamOwnerCategory(callOp);
  auto src = callOp.getArgOperand(1);

  // Copy assignment between owner categories.
  if (dst && owners.count(src))
    return checkNonConstUseOfOwner(dst, callOp.getLoc());

  // Copy assignment between pointer categories.
  dst = getThisParamPointerCategory(callOp);
  if (dst && ptrs.count(src)) {
    copyPset(dst, src);
    return;
  }
}

// User defined ctors that initialize from owner types is one
// way of tracking owned pointers.
//
// Example:
//  MyIntPointer::MyIntPointer(MyIntOwner const&)(%5, %4)
//
bool LifetimeCheckPass::isCtorInitPointerFromOwner(CallOp callOp) {
  if (callOp.getNumArgOperands() < 2)
    return false;

  // The borrowing convention binds the first explicit argument. Resolve that
  // argument just like other address operands: a reference alias does not
  // change which owner's contents are borrowed.
  if (!getThisParamPointerCategory(callOp))
    return false;
  auto owner = callOp.getArgOperand(1);
  if (activeState) {
    auto targets = resolveAddress(owner, *activeState);
    return llvm::any_of(targets.objects,
                        [&](ObjectId object) { return owners.count(object); });
  }
  return owners.count(owner);
}

void LifetimeCheckPass::checkCtor(
    CallOp callOp, cir::CXXCtorAttr ctor,
    ArrayRef<std::optional<LifetimeParameterKind>> parameterKinds) {
  if (callOp.getNumArgOperands() == 0)
    return;
  if ((ctor.getCtorKind() == cir::CtorKind::Copy ||
       ctor.getCtorKind() == cir::CtorKind::Move) &&
      callOp.getNumArgOperands() < 2)
    return;
  // TODO: zero init
  // 2.4.2 if the initialization is default initialization or zero
  // initialization, example:
  //
  //    int* p{};
  //    string_view p;
  //
  // both results in pset(p) == {null}
  if (ctor.getCtorKind() == cir::CtorKind::Default) {
    // First argument passed is always the alloca for the 'this' ptr.

    // Currently two possible actions:
    // 1. Skip Owner category initialization.
    // 2. Initialize Pointer categories.
    auto addr = getThisParamOwnerCategory(callOp);
    if (addr)
      return;

    addr = getThisParamPointerCategory(callOp);
    if (!addr)
      return;

    // Not interested in block/function arguments or any indirect
    // provided alloca address.
    if (!addr.getDefiningOp<cir::AllocaOp>())
      return;

    markPsetNull(addr, callOp.getLoc());
    return;
  }

  // Copying an owner constructs a distinct owner without invalidating the
  // source. A copied pointer retains the source's pointee and its history.
  if (ctor.getCtorKind() == cir::CtorKind::Copy) {
    auto dst = getThisParamPointerCategory(callOp);
    auto src = callOp.getArgOperand(1);
    if (dst && ptrs.count(src))
      copyPset(dst, src);
    return;
  }

  // Move constructor - mark source as moved-from
  checkMoveCtor(callOp, ctor);

  bool borrowsOwner = isCtorInitPointerFromOwner(callOp);
  // A custom constructor passes its arguments like any other call. A Pointer
  // constructed from an Owner borrows it; that binding is not a non-const use
  // of the owner, so it must not invalidate the owner's other borrowers.
  if (ctor.getCtorKind() == cir::CtorKind::Custom)
    checkForOwnerAndPointerArguments(callOp, /*firstArgIdx=*/1, parameterKinds,
                                     borrowsOwner ? std::optional<unsigned>(1)
                                                  : std::nullopt);

  if (borrowsOwner) {
    auto addr = getThisParamPointerCategory(callOp);
    assert(addr && "expected pointer category");
    auto owner = callOp.getArgOperand(1);
    getPmap()[addr].clear();
    if (activeState) {
      for (ObjectId object : resolveAddress(owner, *activeState).objects)
        if (owners.count(object))
          getPmap()[addr].insert(State::getOwnedBy(object));
    } else {
      getPmap()[addr].insert(State::getOwnedBy(owner));
    }
    return;
  }
}

void LifetimeCheckPass::checkOperators(CallOp callOp,
                                       cir::LifetimeFunctionAttr m) {
  auto addr = getThisParamOwnerCategory(callOp);

  if (addr) {
    // Smart pointers need special handling even for const methods
    // (operator*, operator-> are const but unsafe when null)
    if (isSmartPointerType(addr.getType())) {
      // Get declaration name (works for operators too)
      llvm::StringRef methodName = m.getMethodName();

      if (isSmartPointerSafeMethod(methodName))
        return; // Safe operation

      // Unsafe: operator*, operator-> will fall through to deref check
      checkPointerDeref(addr, callOp.getLoc());
      return;
    }

    // const access to the owner is fine (for non-smart-pointers).
    if (m.getIsConst())
      return;

    // TODO: this is a place where we can hook in some idiom recocgnition
    // so we don't need to use actual source code annotation to make assumptions
    // on methods we understand and know to behave nicely.
    //
    // In P1179, section 2.5.7.12, the use of [[gsl::lifetime_const]] is
    // suggested, but it's not part of clang (will it ever?)
    return checkNonConstUseOfOwner(addr, callOp.getLoc());
  }

  addr = getThisParamPointerCategory(callOp);
  if (addr) {
    // The assumption is that method calls on pointer types should trigger
    // deref checking.
    checkPointerDeref(addr, callOp.getLoc());
    return;
  }

  // FIXME: we also need to look at operators from non owner or pointer
  // types that could be using Owner/Pointer types as parameters.
}

mlir::Value
LifetimeCheckPass::getNonConstUseOfOwner(CallOp callOp,
                                         cir::LifetimeFunctionAttr m) {
  if (m.getIsConst())
    return {};
  return getThisParamOwnerCategory(callOp);
}

void LifetimeCheckPass::checkNonConstUseOfOwner(mlir::Value ownerAddr,
                                                    mlir::Location loc) {
  // The path state also holds library facts about the owner's contents. The
  // mutation applies the KILL and generation update below and forgets them.
  if (activeState)
    return mutateOwnerContents(*activeState, ObjectTargets{{ownerAddr}}, loc);

  // 2.4.2 - On every non-const use of a local Owner o:
  //
  // - For each entry e in pset(s): Remove e from pset(s), and if no other
  // Owner’s pset contains only e, then KILL(e).
  kill(State::getOwnedBy(ownerAddr), InvalidStyle::NonConstUseOfOwner, loc);

  // - Set pset(o) = {o__N'}, where N is one higher than the highest
  // previously used suffix. For example, initially pset(o) is {o__1'}, on
  // o’s first non-const use pset(o) becomes {o__2'}, on o’s second non-const
  // use pset(o) becomes {o__3'}, and so on.
  incOwner(ownerAddr);
  return;
}

void LifetimeCheckPass::markOwnerAsMovedFrom(mlir::Value addr,
                                             mlir::Location loc) {
  // Don't mark temporaries as moved-from - they're about to be destroyed
  if (isSkippableTemporary(addr))
    return;

  // All owner types are marked invalid after move. Safe operations on specific
  // owner types (e.g., smart pointer get/reset) are handled separately in
  // isSmartPointerSafeMethod.
  markPsetInvalid(addr, InvalidStyle::MovedFrom, loc);
}

bool LifetimeCheckPass::isValueType(mlir::Value addr) {
  return !owners.count(addr) && !ptrs.count(addr);
}

bool LifetimeCheckPass::hasInvalidState(mlir::Value addr) {
  return getPmap().count(addr) && getPmap()[addr].count(State::getInvalid());
}

void LifetimeCheckPass::markPointerOrValueTypeAsMovedFrom(mlir::Value addr,
                                                          mlir::Location loc) {
  // Don't mark temporaries as moved-from
  if (isSkippableTemporary(addr))
    return;

  markPsetInvalid(addr, InvalidStyle::MovedFrom, loc);
}

// Returns true if the method can be safely called on a moved-from (null)
// smart pointer.
//
// Safe methods for std::unique_ptr and std::shared_ptr:
// - get(): Returns the raw pointer (nullptr for moved-from pointer)
// - release(): Releases ownership and returns pointer (nullptr if empty)
// - reset(): Resets to a new pointer (handles nullptr gracefully)
// - operator bool: Checks if pointer is non-null (returns false if moved-from)
//
// Unsafe methods that require valid (non-null) pointers:
// - operator*: Dereferences pointer (undefined behavior if null)
// - operator->: Member access through pointer (undefined behavior if null)
//
// This is specific to std::unique_ptr and std::shared_ptr because they have
// well-defined null-after-move semantics. Other owner types may have different
// contracts for moved-from state.
bool LifetimeCheckPass::isSmartPointerSafeMethod(llvm::StringRef methodName) {
  return methodName == "get" || methodName == "release" ||
         methodName == "reset" || methodName == "operator bool";
}

void LifetimeCheckPass::checkMoveCtor(CallOp callOp, cir::CXXCtorAttr ctor) {
  // Move constructors are already marked via CXXCtorAttr with CtorKind::Move
  // (set during CIRGen in CIRGenModule.cpp). This attribute is part of
  // CIR_CXXSpecialMemberAttr attached to FuncOp, which also includes move
  // assignments (CXXAssignAttr with AssignKind::Move). See checkCall for
  // dispatch logic.
  if (ctor.getCtorKind() != cir::CtorKind::Move)
    return;

  // Get the source parameter (second argument after 'this')
  if (callOp.getNumArgOperands() < 2)
    return;

  auto srcArg = callOp.getArgOperand(1);

  // For move constructors, the source might be passed as a load
  // Try to get the address from a LoadOp
  mlir::Value src = srcArg;
  if (auto loadOp = srcArg.getDefiningOp<cir::LoadOp>())
    src = loadOp.getAddr();

  // Check if this is an Owner type move constructor
  auto addr = getThisParamOwnerCategory(callOp);
  if (addr && owners.count(src)) {
    if (checkPointerDeref(src, callOp.getLoc(), DerefStyle::UseAfterMove))
      return;
    markOwnerAsMovedFrom(src, callOp.getLoc());
    return;
  }

  // Check if this is a Pointer type move constructor
  addr = getThisParamPointerCategory(callOp);
  if (addr && ptrs.count(src)) {
    if (hasInvalidState(src)) {
      checkPointerDeref(src, callOp.getLoc(), DerefStyle::UseAfterMove);
      return;
    }
    markPointerOrValueTypeAsMovedFrom(src, callOp.getLoc());
    return;
  }
}

static CallParameterKinds
resolveCallParameterKinds(CallOp callOp, LifetimeFunctionAttr lifetime) {
  CallParameterKinds kinds(callOp.getNumArgOperands());
  if (lifetime && lifetime.hasParameterInfo())
    for (unsigned param = 0; param < lifetime.getNumParams(); ++param) {
      unsigned index = lifetime.getParamOperandIndex(param);
      if (index < kinds.size())
        kinds[index] = static_cast<LifetimeParameterKind>(
            lifetime.getParameterKinds()[param]);
    }
  // Direct source mappings take precedence. Missing call-site facts stay
  // unknown; a lowered pointer type does not establish a reference binding.
  if (auto attrs = callOp.getArgAttrsAttr())
    for (auto [index, attr] : llvm::enumerate(attrs)) {
      if (index >= kinds.size() || kinds[index])
        continue;
      if (auto kind =
              cast<DictionaryAttr>(attr).getAs<LifetimeParameterKindAttr>(
                  getLifetimeParameterKindAttrName()))
        kinds[index] = kind.getValue();
    }
  return kinds;
}

void LifetimeCheckPass::checkForOwnerAndPointerArguments(
    CallOp callOp, unsigned firstArgIdx,
    ArrayRef<std::optional<LifetimeParameterKind>> parameterKinds,
    std::optional<unsigned> borrowedOwnerArg,
    llvm::function_ref<bool(unsigned)> skip, bool mutateOwners) {
  auto numOperands = callOp.getNumArgOperands();
  if (firstArgIdx >= numOperands)
    return;

  llvm::SmallSetVector<mlir::Value, 8> ownersToInvalidate, ptrsToDeref,
      nullablePtrsToCheck;
  SmallVector<ObjectTargets, 2> mayAliasOwners;
  for (unsigned i = firstArgIdx, e = numOperands; i != e; ++i) {
    if (skip && skip(i))
      continue;
    auto arg = callOp.getArgOperand(i);
    // Parameter kinds describe binding, not whether an indirect callee reads
    // pointee contents. Preserve nullable indirect checks even with known
    // kinds.
    bool useDirectPolicy = callOp.getCallee().has_value();
    auto collectPointerCheck = [&](Value pointer, bool nullable = false) {
      if (useDirectPolicy && !nullable)
        ptrsToDeref.insert(pointer);
      else
        nullablePtrsToCheck.insert(pointer);
    };
    // FIXME: apply p1179 rules as described in 2.5. Very conservative for now:
    //
    // - Owners: always invalidate.
    // - Pointers: check for deref unless passed by lvalue reference. Binding
    //   a reference alone does not dereference null contents or reinitialize
    //   the object; moved-from and dangling states are still checked.
    // - Coroutine tasks: check the task for deref when calling methods of
    //   the task, but also when the passing the task around to other functions.
    // - Aggregates: check ptr subelements for deref.
    //
    // FIXME: even before 2.5 we should only invalidate non-const param types.
    bool nullable = parameterKinds[i] == LifetimeParameterKind::LValueReference;
    if (activeState && isa<PointerType>(arg.getType())) {
      // An address argument can denote several objects, or storage whose
      // identity is only known through the path state. Standard-library
      // holders participate like owners. Only a definite object is strongly
      // updated; scalar storage is neither an owner nor a pointer.
      ObjectTargets targets = resolveAddress(arg, *activeState);
      ObjectTargets mutated = targets;
      mutated.objects.clear();
      for (ObjectId object : targets.objects) {
        if (mutateOwners && borrowedOwnerArg != i &&
            (owners.count(object) || hasLibraryContents(object)))
          mutated.objects.push_back(object);
        if (ptrs.count(object))
          collectPointerCheck(object, nullable);
      }
      if (!targets.isDefinite() && mutated.isDefinite())
        mutated.unknown = true;
      if (mutated.isDefinite())
        ownersToInvalidate.insert(mutated.objects.front());
      else if (!mutated.objects.empty())
        mayAliasOwners.push_back(std::move(mutated));
    } else {
      if (mutateOwners && borrowedOwnerArg != i && owners.count(arg))
        ownersToInvalidate.insert(arg);
      if (ptrs.count(arg))
        collectPointerCheck(arg, nullable);
    }
    if (tasks.count(arg))
      collectPointerCheck(arg);
    if (aggregates.count(arg)) {
      int memberIdx = 0;
      auto sTy = mlir::dyn_cast<RecordType>(
          mlir::cast<PointerType>(arg.getType()).getPointee());
      assert(sTy && "expected record type");
      for (auto m : sTy.getMembers()) {
        auto ptrMemberAddr = aggregates[arg][memberIdx];
        if (mlir::isa<PointerType>(m) && ptrMemberAddr) {
          collectPointerCheck(ptrMemberAddr);
        }
        memberIdx++;
      }
    }
  }

  // FIXME: CIR should track source info on the passed args, so we can get
  // accurate location for why the invalidation happens.
  for (auto o : ownersToInvalidate)
    checkNonConstUseOfOwner(o, callOp.getLoc());
  for (const auto &targets : mayAliasOwners)
    mutateOwnerContents(*activeState, targets, callOp.getLoc());
  for (auto p : ptrsToDeref)
    checkPointerDeref(p, callOp.getLoc(), DerefStyle::CallParam);
  for (auto p : nullablePtrsToCheck)
    if (!ptrsToDeref.count(p))
      checkPointerDeref(p, callOp.getLoc(), DerefStyle::NullableCallParam);
}

void LifetimeCheckPass::checkOtherMethodsAndFunctions(
    CallOp callOp, cir::LifetimeFunctionAttr m,
    ArrayRef<std::optional<LifetimeParameterKind>> parameterKinds) {
  unsigned firstArgIdx = 0;

  // Looks at a method 'this' pointer:
  // - If a method call to a class we consider interesting, like a method
  //   call on a coroutine task (promise_type).
  // - Skip the 'this' for any other method.
  if (m && m.getIsMethod() && m.getHasImplicitObject() &&
      callOp.getNumArgOperands() > firstArgIdx &&
      !tasks.count(callOp.getArgOperand(firstArgIdx)))
    firstArgIdx++;
  checkForOwnerAndPointerArguments(callOp, firstArgIdx, parameterKinds);
}

bool LifetimeCheckPass::isOwnerOrPointerClassMethod(
    CallOp callOp, cir::LifetimeFunctionAttr m) {
  // For the sake of analysis, these behave like regular functions
  if (!m || !m.getIsMethod() || !m.getHasImplicitObject())
    return false;
  // Check the object for owner/pointer by looking at the 'this' pointer.
  return getThisParamPointerCategory(callOp) ||
         getThisParamOwnerCategory(callOp);
}

bool LifetimeCheckPass::isLambdaType(mlir::Type ty) {
  if (IsLambdaTyCache.count(ty))
    return IsLambdaTyCache[ty];

  IsLambdaTyCache[ty] = false;
  auto taskTy = mlir::dyn_cast<cir::RecordType>(ty);
  if (!taskTy)
    return false;
  auto lifetime = cir::getLifetimeRecord(taskTy, theModule);
  if (lifetime && lifetime.getLambda())
    IsLambdaTyCache[ty] = true;

  return IsLambdaTyCache[ty];
}

bool LifetimeCheckPass::isTaskType(mlir::Value taskVal) {
  auto ty = taskVal.getType();
  if (IsTaskTyCache.count(ty))
    return IsTaskTyCache[ty];

  bool result = [&] {
    auto taskTy = mlir::dyn_cast<cir::RecordType>(taskVal.getType());
    if (!taskTy)
      return false;
    auto lifetime = cir::getLifetimeRecord(taskTy, theModule);
    return lifetime && lifetime.getPromiseType();
  }();

  IsTaskTyCache[ty] = result;
  return result;
}

void LifetimeCheckPass::trackCallToCoroutine(CallOp callOp) {
  if (auto fnName = callOp.getCallee()) {
    auto calleeFuncOp = getCalleeFromSymbol(*moduleSymbols, *fnName);
    if (calleeFuncOp &&
        (calleeFuncOp.getCoroutine() ||
         (calleeFuncOp.isDeclaration() && callOp->getNumResults() > 0 &&
          isTaskType(callOp->getResult(0))))) {
      currScope->localTempTasks.insert(callOp->getResult(0));
    }
    return;
  }
  // Handle indirect calls to coroutines, for instance when
  // lambda coroutines are involved with invokers.
  if (callOp->getNumResults() > 0 && isTaskType(callOp->getResult(0))) {
    // FIXME: get more guarantees to prevent false positives (perhaps
    // apply some tracking analysis before this pass and check for lambda
    // idioms).
    currScope->localTempTasks.insert(callOp->getResult(0));
  }
}

void LifetimeCheckPass::checkCall(
    CallOp callOp,
    ArrayRef<std::optional<LifetimeParameterKind>> parameterKinds) {
  if (callOp.getNumArgOperands() == 0)
    return;

  // Identify calls to coroutines and track returning temporary task types.
  //
  // Note that we can't reliably know if a function is a coroutine only as
  // part of declaration
  trackCallToCoroutine(callOp);

  auto methodDecl = getMethod(*moduleSymbols, callOp);
  bool isOwnerOrPointerMethod = isOwnerOrPointerClassMethod(callOp, methodDecl);
  mlir::Attribute cxxSpecialMember;
  if (isOwnerOrPointerMethod) {
    if (auto fnName = callOp.getCallee()) {
      auto calleeFuncOp = getCalleeFromSymbol(*moduleSymbols, *fnName);
      if (calleeFuncOp)
        cxxSpecialMember = calleeFuncOp.getFuncInfoAttr();
    }
  }

  if (!isOwnerOrPointerMethod)
    return checkOtherMethodsAndFunctions(callOp, methodDecl, parameterKinds);

  // From this point on only owner and pointer class methods handling,
  // starting from special methods.
  if (auto cxxCtor = dyn_cast_if_present<cir::CXXCtorAttr>(cxxSpecialMember))
    return checkCtor(callOp, cxxCtor, parameterKinds);

  if (auto cxxAssign =
          dyn_cast_if_present<cir::CXXAssignAttr>(cxxSpecialMember)) {
    switch (cxxAssign.getAssignKind()) {
    case cir::AssignKind::Move:
      return checkMoveAssignment(callOp, methodDecl);
    case cir::AssignKind::Copy:
      return checkCopyAssignment(callOp, methodDecl);
    }
  }
  if (methodDecl.getIsOverloadedOperator())
    return checkOperators(callOp, methodDecl);

  // For any other methods...

  // Non-const member call to a Owner invalidates any of its users.
  if (auto owner = getNonConstUseOfOwner(callOp, methodDecl)) {
    return checkNonConstUseOfOwner(owner, callOp.getLoc());
  }

  // Take a pset(Ptr) = { Ownr' } where Own got invalidated, this will become
  // invalid access to Ptr if any of its methods are used.
  auto addr = getThisParamPointerCategory(callOp);
  if (addr)
    checkPointerDeref(addr, callOp.getLoc());
}

// Only the switch shell is traversed in order. Each case body uses the normal
// CIR region worklist, so labels and backedges inside a case need not be
// linear.
static bool hasLinearSwitchShell(Region &region) {
  bool reachedExit = false;
  for (Block &block : region) {
    if (block.empty() || block.getNumArguments())
      return false;
    if (reachedExit) {
      // CIRGen can leave an unreachable continuation after a terminator.
      if (!block.hasNoPredecessors() || !llvm::hasSingleElement(block) ||
          !isa<YieldOp, BreakOp, ContinueOp>(block.front()))
        return false;
      continue;
    }
    Operation *terminator = block.getTerminator();
    if (auto branch = dyn_cast<cir::BrOp>(terminator)) {
      if (!branch.getDestOperands().empty() ||
          branch.getDest() != block.getNextNode())
        return false;
    } else if (terminator->getNumSuccessors()) {
      return false;
    } else {
      reachedExit = true;
    }
  }
  return true;
}

static LogicalResult validateLifetimeControlFlow(FuncOp function,
                                                 FunctionLabelMap &labels) {
  bool hasSwitch = false;
  bool hasSymbolicJump = false;
  auto indexed = function.walk([&](Operation *op) -> WalkResult {
    hasSwitch |= isa<SwitchOp>(op);
    hasSymbolicJump |= isa<GotoOp, IndirectGotoOp>(op);
    if (auto label = dyn_cast<LabelOp>(op)) {
      if (label->getBlock()->getNumArguments()) {
        mlir::emitError(label.getLoc(),
                        "unsupported control flow in lifetime analysis: "
                        "symbolic label block must not take arguments");
        return WalkResult::interrupt();
      }
      if (!labels.try_emplace(label.getLabelAttr(), label->getBlock()).second) {
        mlir::emitError(label.getLoc(),
                        "unsupported control flow in lifetime analysis: "
                        "duplicate symbolic label");
        return WalkResult::interrupt();
      }
    }
    return WalkResult::advance();
  });
  if (indexed.wasInterrupted())
    return failure();
  if (!hasSwitch && !hasSymbolicJump)
    return success();

  auto result = function.walk([&](Operation *op) -> WalkResult {
    if (auto jump = dyn_cast<GotoOp>(op)) {
      auto target = labels.find(jump.getLabelAttr());
      if (target == labels.end()) {
        mlir::emitError(jump.getLoc(),
                        "unsupported control flow in lifetime analysis: "
                        "unresolved symbolic goto target");
        return WalkResult::interrupt();
      }
      Region *targetRegion = target->second->getParent();
      Region *sourceRegion = jump->getParentRegion();
      // Compare regions, not their owning operations: sibling cases and if
      // branches can share an owner but cannot be entered from each other.
      if (targetRegion != sourceRegion &&
          !targetRegion->isProperAncestor(sourceRegion)) {
        mlir::emitError(jump.getLoc(),
                        "unsupported control flow in lifetime analysis: "
                        "goto enters a nested or sibling region");
        return WalkResult::interrupt();
      }
    } else if (isa<IndirectGotoOp>(op)) {
      mlir::emitError(op->getLoc(),
                      "unsupported control flow in lifetime analysis: "
                      "indirect goto");
      return WalkResult::interrupt();
    }

    // Preserve function-local failure and diagnostic replay for unsupported
    // successors in switch-free functions. Switches retain their preflight
    // shape validation; BrOp is handled by the existing CIR region worklist.
    if (hasSwitch && op->getNumSuccessors() && !isa<BrOp>(op)) {
      mlir::emitError(op->getLoc(),
                      "unsupported control flow in lifetime analysis: "
                      "unsupported successor terminator in switch function");
      return WalkResult::interrupt();
    }
    if (auto switchOp = dyn_cast<SwitchOp>(op)) {
      Region &body = switchOp.getBody();
      if (body.empty())
        return WalkResult::advance();
      SmallVector<CaseOp> cases;
      if (!hasLinearSwitchShell(body) || !switchOp.isSimpleForm(cases) ||
          llvm::any_of(cases, [&](CaseOp caseOp) {
            return caseOp.getRegion().empty() ||
                   caseOp->getParentOp() != switchOp.getOperation();
          })) {
        mlir::emitError(switchOp.getLoc(),
                        "unsupported control flow in lifetime analysis: "
                        "switch requires a simple direct-case shell");
        return WalkResult::interrupt();
      }
    }
    return WalkResult::advance();
  });
  return failure(result.wasInterrupted());
}

//===----------------------------------------------------------------------===//
// Path-sensitive storage and value transfer
//===----------------------------------------------------------------------===//

template <class T, unsigned N>
static void appendUnique(SmallVector<T, N> &to, ArrayRef<T> from) {
  for (const T &value : from)
    if (!llvm::is_contained(to, value))
      to.push_back(value);
}

template <class T>
static void appendUnique(llvm::TinyPtrVector<T> &to, ArrayRef<T> from) {
  for (T value : from)
    if (!llvm::is_contained(to, value))
      to.push_back(value);
}

// These object lists can grow with the function. Keep their established
// order while avoiding a full destination scan for every incoming object.
static void appendUnique(SmallVector<Value, 16> &to, ArrayRef<Value> from) {
  if (from.empty())
    return;
  llvm::SmallDenseSet<Value, 16> seen(to.begin(), to.end());
  for (Value value : from)
    if (seen.insert(value).second)
      to.push_back(value);
}

LifetimeCheckPass::ObjectTargets
LifetimeCheckPass::joinTargets(const ObjectTargets &a,
                               const ObjectTargets &b) const {
  ObjectTargets result = a;
  appendUnique(result.objects, ArrayRef<ObjectId>(b.objects));
  sortValues(result.objects);
  result.unknown |= b.unknown;
  result.mayBeNull |= b.mayBeNull;
  result.mayBeInvalid |= b.mayBeInvalid;
  return result;
}

LifetimeCheckPass::ValueSummary
LifetimeCheckPass::joinValues(const ValueSummary &a,
                              const ValueSummary &b) const {
  ValueSummary result = a;
  if (a.pointer && b.pointer) {
    result.pointer->targets =
        joinTargets(a.pointer->targets, b.pointer->targets);
    result.pointer->mayBeNonNull |= b.pointer->mayBeNonNull;
    result.pointer->unknownNullness |= b.pointer->unknownNullness;
    appendUnique(result.pointer->borrowedOwners,
                 ArrayRef<ObjectId>(b.pointer->borrowedOwners));
    appendUnique(result.pointer->invalidHistory.entries,
                 ArrayRef<InvalidHistEntry>(b.pointer->invalidHistory.entries));
    if (!result.pointer->nullOrigin)
      result.pointer->nullOrigin = b.pointer->nullOrigin;
  } else if (b.pointer) {
    result.pointer = b.pointer;
    result.pointer->targets.unknown = true;
    result.pointer->unknownNullness = true;
  } else if (a.pointer) {
    result.pointer->targets.unknown = true;
    result.pointer->unknownNullness = true;
  }
  appendUnique(result.readOrigins, ArrayRef<ObjectId>(b.readOrigins));
  appendUnique(result.xvalueOrigins, ArrayRef<ObjectId>(b.xvalueOrigins));
  if (a.tupleElements.size() == b.tupleElements.size()) {
    for (unsigned i = 0; i < result.tupleElements.size(); ++i)
      result.tupleElements[i] =
          joinTargets(a.tupleElements[i], b.tupleElements[i]);
  } else {
    result.tupleElements.clear();
  }
  if (a.tupleValues.size() == b.tupleValues.size()) {
    for (unsigned i = 0; i < result.tupleValues.size(); ++i)
      result.tupleValues[i] = joinValues(a.tupleValues[i], b.tupleValues[i]);
  } else {
    result.tupleValues.clear();
  }
  if (!equalPredicates(a.predicate, b.predicate))
    result.predicate.reset();
  if (a.booleanValue != b.booleanValue)
    result.booleanValue.reset();
  if (!(a.objectOccupancy == b.objectOccupancy)) {
    if (a.objectOccupancy && b.objectOccupancy) {
      auto occupancy = *a.objectOccupancy;
      occupancy.mayBeEmpty |= b.objectOccupancy->mayBeEmpty;
      occupancy.mayBePresent |= b.objectOccupancy->mayBePresent;
      occupancy.unknown |= b.objectOccupancy->unknown;
      appendUnique(occupancy.emptyOrigins,
                   ArrayRef<Location>(b.objectOccupancy->emptyOrigins));
      result.objectOccupancy = std::move(occupancy);
    } else {
      result.objectOccupancy.reset();
    }
  }
  if (a.elementPredicates.size() == b.elementPredicates.size()) {
    for (unsigned i = 0; i < result.elementPredicates.size(); ++i)
      if (!(a.elementPredicates[i] == b.elementPredicates[i]))
        result.elementPredicates[i].reset();
  } else {
    result.elementPredicates.clear();
  }
  result.externallyAliasedPointer |= b.externallyAliasedPointer;
  result.unknown |= b.unknown;
  return result;
}

std::optional<LifetimeCheckPass::ObjectInfo>
LifetimeCheckPass::getObjectInfo(ObjectId object) const {
  auto found = objectInfo.find(object);
  if (found == objectInfo.end())
    return std::nullopt;
  return found->second;
}

LifetimeCheckPass::ObjectTargets
LifetimeCheckPass::resolveAddress(Value address, const PathState &state) const {
  auto canonical = canonicalObjects.find(address);
  if (canonical != canonicalObjects.end())
    return {{canonical->second}};
  if (auto found = state.values.find(address); found != state.values.end()) {
    if (found->second.pointer)
      return found->second.pointer->targets;
  }
  if (auto cast = address.getDefiningOp<CastOp>()) {
    if (isa<cir::PointerType>(cast.getSrc().getType()) &&
        isa<cir::PointerType>(address.getType()))
      return resolveAddress(cast.getSrc(), state);
  }
  if (auto stride = address.getDefiningOp<PtrStrideOp>())
    return resolveAddress(stride.getBase(), state);
  if (auto element = address.getDefiningOp<GetElementOp>())
    return resolveAddress(element.getBase(), state);
  // Never peel a load here: its pointer value was captured when it executed.
  auto summary = summarizeValue(address, state);
  if (summary.pointer)
    return summary.pointer->targets;
  ObjectTargets result;
  result.unknown = true;
  return result;
}

LifetimeCheckPass::ValueSummary
LifetimeCheckPass::summarizeValue(Value value, const PathState &state) const {
  if (auto found = state.values.find(value); found != state.values.end())
    return found->second;
  ValueSummary summary;
  if (auto found = canonicalObjects.find(value);
      found != canonicalObjects.end()) {
    summary.pointer.emplace();
    summary.pointer->targets.objects.push_back(found->second);
    // A symbolic incoming referent establishes identity, not nullness. Keep
    // it definite for alias/write-through tracking without pruning null
    // branches of an owner constructed from an ordinary pointer parameter.
    summary.pointer->mayBeNonNull =
        !objectInfo.lookup(found->second).unknownIncomingNullness;
    summary.pointer->unknownNullness =
        objectInfo.lookup(found->second).unknownIncomingNullness;
    return summary;
  }
  if (auto argument = dyn_cast<BlockArgument>(value)) {
    if (isa<FuncOp>(argument.getOwner()->getParentOp())) {
      if (auto record = dyn_cast<RecordType>(value.getType())) {
        auto properties = cir::getLifetimeRecord(record, theModule);
        if (properties && properties.getPointer()) {
          // A pointer-like record arrives as an initialized by-value object.
          // Its conceptual pointee is caller-owned; this does not turn the
          // record SSA value into an address or prove non-null contents.
          summary.pointer.emplace();
          summary.pointer->targets.objects.push_back(value);
          summary.pointer->unknownNullness = true;
          return summary;
        }
      }
    }
  }
  if (auto constant = value.getDefiningOp<ConstantOp>()) {
    if (auto boolean = dyn_cast<cir::BoolAttr>(constant.getValue())) {
      summary.booleanValue = boolean.getValue();
      return summary;
    }
    if (auto pointer = dyn_cast<ConstPtrAttr>(constant.getValue())) {
      summary.pointer.emplace();
      summary.pointer->targets.mayBeNull = pointer.isNullValue();
      summary.pointer->mayBeNonNull = !pointer.isNullValue();
      summary.pointer->targets.unknown = !pointer.isNullValue();
      if (pointer.isNullValue())
        summary.pointer->nullOrigin = constant.getLoc();
      return summary;
    }
    if (isa<cir::PointerType>(value.getType())) {
      summary.pointer.emplace();
      if (isa<UndefAttr, PoisonAttr>(constant.getValue())) {
        summary.pointer->targets.mayBeInvalid = true;
        summary.pointer->invalidHistory.add(value, NotInitialized,
                                            constant.getLoc());
      } else {
        summary.pointer->mayBeNonNull = true;
        summary.pointer->targets.unknown = true;
      }
      return summary;
    }
  }
  if (value.getDefiningOp<GetGlobalOp>()) {
    summary.pointer.emplace();
    summary.pointer->mayBeNonNull = true;
    // This is a real alternative to a local address, even though globals do
    // not need automatic-storage lifetime tracking. Without the unknown
    // identity bit, joining &local with &global would become a definite local.
    summary.pointer->targets.unknown = true;
    return summary;
  }
  if (isa<cir::PointerType>(value.getType())) {
    summary.pointer.emplace();
    summary.pointer->targets.unknown = true;
    summary.pointer->unknownNullness = true;
  }
  summary.unknown = true;
  return summary;
}

void LifetimeCheckPass::setValueSummary(PathState &state, Value value,
                                        ValueSummary summary) {
  // An SSA operation can execute again in a loop. A predicate from an older
  // iteration must not restore the old execution's value at the same SSA ID.
  visitPredicates(state, [&](std::optional<ValuePredicate> &predicate) {
    if (!predicate || predicate->domain != ValuePredicate::ConditionalMove)
      return;
    for (auto *snapshots :
         {&predicate->pointersBefore, &predicate->pointersAfter})
      for (auto &snapshot : *snapshots)
        if (!snapshot.stored && snapshot.value == value)
          snapshot.valid = false;
  });
  state.values[value] = std::move(summary);
}

void LifetimeCheckPass::storeSummary(PathState &state,
                                     const ObjectTargets &targets,
                                     const ValueSummary &summary) {
  invalidatePredicates(state, targets);
  for (ObjectId object : targets.objects) {
    if (targets.isDefinite())
      state.storedValues[object] = summary;
    else
      state.storedValues[object] =
          joinValues(state.storedValues[object], summary);
  }
}

LifetimeCheckPass::OccupancySummary
LifetimeCheckPass::getOccupancy(const PathState &state,
                                const ObjectTargets &targets) const {
  OccupancySummary result;
  result.unknown = targets.unknown || targets.mayBeInvalid || targets.mayBeNull;
  if (targets.objects.empty())
    result.unknown = true;
  for (ObjectId object : targets.objects) {
    auto found = state.occupancy.find(object);
    if (found == state.occupancy.end()) {
      result.unknown = true;
      continue;
    }
    result.unknown |= found->second.unknown;
    result.mayBeEmpty |= found->second.mayBeEmpty;
    result.mayBePresent |= found->second.mayBePresent;
    appendUnique(result.emptyOrigins,
                 ArrayRef<Location>(found->second.emptyOrigins));
  }
  return result;
}

void LifetimeCheckPass::setOccupancy(PathState &state,
                                     const ObjectTargets &targets,
                                     const OccupancySummary &occupancy,
                                     Location loc) {
  invalidatePredicates(state, targets);
  for (ObjectId object : targets.objects) {
    auto value = occupancy;
    if (value.mayBeEmpty && value.emptyOrigins.empty())
      value.emptyOrigins.push_back(loc);
    if (!targets.isDefinite()) {
      auto old = getOccupancy(state, ObjectTargets{{object}});
      value.mayBeEmpty |= old.mayBeEmpty;
      value.mayBePresent |= old.mayBePresent;
      value.unknown |= old.unknown;
      appendUnique(value.emptyOrigins, ArrayRef<Location>(old.emptyOrigins));
    }
    state.occupancy[object] = std::move(value);
  }
}

void LifetimeCheckPass::reinitializeObjects(PathState &state,
                                            const ObjectTargets &targets,
                                            Location loc) {
  invalidatePredicates(state, targets);
  for (ObjectId object : targets.objects) {
    auto info = getObjectInfo(object);
    if (!info ||
        (!info->callerOwned && !llvm::is_contained(state.liveObjects, object)))
      continue;
    PSetType initialized;
    if (info->category == ObjectInfo::Owner)
      initialized.insert(State::getOwnedBy(object));
    else if (info->category != ObjectInfo::Pointer)
      initialized.insert(State::getLocalValue(object));
    if (targets.isDefinite()) {
      state.pmap[object] = std::move(initialized);
      (activeState == &state ? invalidHist : state.invalidHist).erase(object);
      (activeState == &state ? pmapNullHist : state.nullHist).erase(object);
    } else {
      llvm::set_union(state.pmap[object], initialized);
    }
  }
}

void LifetimeCheckPass::detachOwner(PathState &state, ObjectId owner) {
  invalidateConditionalOwnership(state, owner);
  state.ownershipGroups.erase(owner);
  for (auto &entry : state.ownershipGroups)
    llvm::erase(entry.second, owner);
  SmallVector<ObjectId, 4> singletons;
  for (const auto &entry : state.ownershipGroups)
    if (entry.second.size() < 2)
      singletons.push_back(entry.first);
  for (ObjectId object : singletons)
    state.ownershipGroups.erase(object);
}

void LifetimeCheckPass::retargetOwnerContents(PathState &state, ObjectId source,
                                              ObjectId destination,
                                              bool retainSource) {
  invalidateConditionalDependencies(state, source, false);
  for (auto &entry : state.pmap) {
    // The holder's own initializedness is separate from pointers borrowing its
    // resource. In particular, a transfer must not make its storage an alias
    // of the destination's storage.
    if (entry.first == source || !entry.second.count(State::getOwnedBy(source)))
      continue;
    if (!retainSource)
      entry.second.erase(State::getOwnedBy(source));
    entry.second.insert(destination ? State::getOwnedBy(destination)
                                    : State::getGlobal());
  }
  auto retarget = [&](ValueSummary &summary) {
    if (!summary.pointer ||
        !llvm::is_contained(summary.pointer->borrowedOwners, source))
      return;
    auto &holders = summary.pointer->borrowedOwners;
    if (!retainSource)
      llvm::erase(holders, source);
    if (destination && !llvm::is_contained(holders, destination))
      holders.push_back(destination);
    // Detaching ownership preserves the already observed pointer value. It
    // does not prove that an unknown or nullable pointer is non-null.
  };
  for (auto &entry : state.values)
    visitValueSummary(entry.second, retarget);
  for (auto &entry : state.storedValues)
    visitValueSummary(entry.second, retarget);
}

void LifetimeCheckPass::transferOwnerContents(PathState &state,
                                              const ObjectTargets &destinations,
                                              const ObjectTargets &sources,
                                              bool sharedCopy) {
  if (!destinations.isDefinite() || !sources.isDefinite()) {
    // An uncertain move can associate a borrower with either holder. Retain
    // both possible dependencies; neither is a guaranteed co-owner.
    for (ObjectId destination : destinations.objects) {
      detachOwner(state, destination);
      for (ObjectId source : sources.objects) {
        if (source == destination)
          continue;
        if (!sharedCopy)
          retargetOwnerContents(state, source, destination, true);
      }
    }
    return;
  }
  ObjectId source = sources.objects.front();
  ObjectId destination = destinations.objects.front();
  if (source == destination)
    return;
  SmallVector<ObjectId, 2> holders = state.ownershipGroups.lookup(source);
  if (holders.empty())
    holders.push_back(source);
  detachOwner(state, destination);
  if (!sharedCopy) {
    retargetOwnerContents(state, source, destination);
    detachOwner(state, source);
    llvm::erase(holders, source);
  }
  if (!llvm::is_contained(holders, destination))
    holders.push_back(destination);
  sortValues(holders);
  if (holders.size() > 1)
    for (ObjectId holder : holders)
      state.ownershipGroups[holder] = holders;
}

void LifetimeCheckPass::endOwnerContents(PathState &state,
                                         const ObjectTargets &targets,
                                         Location loc, InvalidStyle style) {
  auto end = [&] {
    for (ObjectId owner : targets.objects) {
      ObjectId survivor;
      // With a may-target write, all listed holders might lose their resource;
      // only a holder outside that set can justify keeping borrowers alive.
      for (ObjectId candidate : state.ownershipGroups.lookup(owner)) {
        auto info = getObjectInfo(candidate);
        if (candidate != owner &&
            !llvm::is_contained(targets.objects, candidate) &&
            ((info && info->callerOwned) ||
             llvm::is_contained(state.liveObjects, candidate))) {
          survivor = candidate;
          break;
        }
      }
      detachOwner(state, owner);
      if (survivor)
        retargetOwnerContents(state, owner, survivor);
      else
        kill(State::getOwnedBy(owner), style, loc);
    }
  };
  if (activeState == &state)
    end();
  else {
    StateGuard guard(*this, state);
    end();
  }
}

void LifetimeCheckPass::releaseOwnerContents(PathState &state,
                                             const ObjectTargets &targets,
                                             Location loc) {
  invalidatePredicates(state, targets);
  // Only a definite release can remove a dependency. A may-release leaves the
  // original dependency because the resource may still belong to that holder.
  if (!targets.isDefinite())
    return;
  ObjectId owner = targets.objects.front();
  ObjectId survivor;
  for (ObjectId candidate : state.ownershipGroups.lookup(owner))
    if (candidate != owner &&
        llvm::is_contained(state.liveObjects, candidate)) {
      survivor = candidate;
      break;
    }
  detachOwner(state, owner);
  retargetOwnerContents(state, owner, survivor);
}

void LifetimeCheckPass::invalidateOwnerContents(PathState &state,
                                                const ObjectTargets &targets,
                                                Location loc) {
  endOwnerContents(state, targets, loc, InvalidStyle::NonConstUseOfOwner);
  auto advance = [&] {
    for (ObjectId owner : targets.objects)
      if (owners.count(owner))
        incOwner(owner);
  };
  if (activeState == &state)
    advance();
  else {
    StateGuard guard(*this, state);
    advance();
  }
}

void LifetimeCheckPass::replaceOwnerContents(PathState &state,
                                             const ObjectTargets &targets,
                                             const OccupancySummary &occupancy,
                                             Operation *operation) {
  invalidateOwnerContents(state, targets, operation->getLoc());
  reinitializeObjects(state, targets, operation->getLoc());
  setOccupancy(state, targets, occupancy, operation->getLoc());
}

void LifetimeCheckPass::mutateOwnerContents(PathState &state,
                                                const ObjectTargets &targets,
                                                Location loc) {
  // Borrowers survive only through a proven co-owner; ownership groups that
  // include a mutated holder are no longer must facts.
  invalidateOwnerContents(state, targets, loc);
  invalidatePredicates(state, targets);
  ValueSummary unknown;
  unknown.unknown = true;
  for (ObjectId object : targets.objects) {
    // A missing occupancy entry is unknown, also at joins. A missing payload
    // is not: joins retain the other path's payload, so store an unknown one.
    if (targets.isDefinite()) {
      state.occupancy.erase(object);
      state.storedValues[object] = unknown;
      continue;
    }
    if (auto found = state.occupancy.find(object);
        found != state.occupancy.end())
      found->second.unknown = true;
    auto &stored = state.storedValues[object];
    stored = joinValues(stored, unknown);
  }
}

bool LifetimeCheckPass::hasLibraryContents(ObjectId object) const {
  auto info = getObjectInfo(object);
  auto record = info ? dyn_cast<RecordType>(info->type) : RecordType{};
  auto facts =
      record ? getLifetimeRecord(record, theModule) : LifetimeRecordAttr{};
  return facts && facts.getLibraryKind() != LifetimeLibraryKind::None;
}

void LifetimeCheckPass::markMoved(PathState &state,
                                  const ObjectTargets &targets, Location loc) {
  invalidatePredicates(state, targets);
  auto &history = activeState == &state ? invalidHist : state.invalidHist;
  for (ObjectId object : targets.objects) {
    if (isSkippableTemporary(object))
      continue;
    auto info = getObjectInfo(object);
    // Whole-object moves and local capture initialization are modeled here;
    // moving arbitrary class subobjects is outside this analysis.
    if (!info || info->fieldDepth)
      continue;
    // An opaque consuming call can take this holder away. A later reset must
    // not rely on the moved-from object to keep a shared resource alive.
    // Modeled owner transfers already detached their source before this step.
    if (state.ownershipGroups.count(object))
      endOwnerContents(state, ObjectTargets{{object}}, loc, MovedFrom);
    // Retain the reviewed policy for repeated owner/pointer moves. Smart
    // pointer empty-state facts are maintained by the library model.
    state.pmap[object].insert(State::getInvalid());
    if (info->category == ObjectInfo::Pointer) {
      auto stored = state.storedValues.find(object);
      if (stored != state.storedValues.end() && stored->second.pointer) {
        stored->second.pointer->targets.mayBeInvalid = true;
        stored->second.pointer->invalidHistory.add(object, MovedFrom, loc);
      }
    }
    history[object].add(object, InvalidStyle::MovedFrom, loc);
    // An opaque rvalue-reference call may consume a standard smart pointer.
    // Preserve the unconsumed alternative, but do not leave it known-present:
    // a later guard must retain an empty edge with the move history.
    if (hasEmptyMovedFromState(getRecordLibraryKind(info->type, theModule))) {
      auto &contents = state.occupancy[object];
      contents.mayBeEmpty = true;
      if (!llvm::is_contained(contents.emptyOrigins, loc))
        contents.emptyOrigins.push_back(loc);
    }
  }
}

bool LifetimeCheckPass::checkUse(PathState &state, const ObjectTargets &targets,
                                 Location loc, DerefStyle style) {
  auto check = [&] {
    bool invalid = false;
    for (ObjectId object : targets.objects)
      invalid |= checkPointerDeref(object, loc, style);
    return invalid;
  };
  if (activeState == &state)
    return check();
  StateGuard guard(*this, state);
  return check();
}

void LifetimeCheckPass::indexObjects(FuncOp function) {
  objectInfo.clear();
  valueOrder.clear();
  // Number SSA values in structural IR order. Pointer addresses and hash-map
  // iteration order are not stable across processes or serialization formats.
  unsigned nextValue = 0;
  function.walk<WalkOrder::PreOrder>([&](Operation *operation) {
    for (Region &region : operation->getRegions())
      for (Block &block : region)
        for (BlockArgument argument : block.getArguments())
          valueOrder.try_emplace(argument, nextValue++);
    for (Value result : operation->getResults())
      valueOrder.try_emplace(result, nextValue++);
  });
  canonicalObjects.clear();
  scopeLocals.clear();
  ptrs.clear();
  owners.clear();
  aggregates.clear();
  tasks.clear();
  auto classify = [&](ObjectId object, Type type, bool callerOwned = false) {
    ObjectInfo info{type, ObjectInfo::Scalar, callerOwned, 0, object};
    if (isPointerType(type)) {
      info.category = ObjectInfo::Pointer;
      ptrs.insert(object);
    } else if (isOwnerType(type)) {
      info.category = ObjectInfo::Owner;
      addOwner(object);
    } else if (isAggregateType(this, type)) {
      info.category = ObjectInfo::Aggregate;
    } else if (isa<RecordType>(type)) {
      info.category = ObjectInfo::Record;
    }
    objectInfo.try_emplace(object, info);
    canonicalObjects[object] = object;
  };
  function.walk([&](AllocaOp alloca) {
    classify(alloca.getAddr(), alloca.getAllocaType());
  });
  // Retain the baseline nested aggregate pointer-lifetime model and give
  // direct local lambda captures distinct slots. These identities do not
  // enable compound move tracking: markMoved excludes all subobjects.
  function.walk([&](GetMemberOp member) {
    auto found = canonicalObjects.find(member.getAddr());
    if (found == canonicalObjects.end())
      return;
    auto parent = objectInfo.find(found->second);
    if (parent == objectInfo.end() ||
        (parent->second.category != ObjectInfo::Aggregate &&
         !isLambdaType(parent->second.type)))
      return;
    auto record = dyn_cast<RecordType>(parent->second.type);
    if (!record || member.getIndex() >= record.getMembers().size())
      return;
    auto &fields = aggregates[found->second];
    fields.resize(record.getMembers().size());
    ObjectId &field = fields[member.getIndex()];
    if (!field) {
      unsigned depth = parent->second.fieldDepth + 1;
      ObjectId storageRoot = parent->second.storageRoot;
      field = member.getResult();
      classify(field, member.getType().getPointee());
      objectInfo[field].fieldDepth = depth;
      objectInfo[field].storageRoot = storageRoot;
    }
    canonicalObjects[member.getResult()] = field;
  });
  if (function.getBody().empty())
    return;
  // Pointer entry arguments denote caller-owned storage. Only a reference
  // binding or an implicit object parameter also guarantees a non-null
  // address. Missing source metadata must not invent that guarantee.
  auto metadata = function.getLifetimeAttr();
  for (BlockArgument argument : function.getBody().front().getArguments()) {
    if (auto pointer = dyn_cast<cir::PointerType>(argument.getType())) {
      classify(argument, pointer.getPointee(), true);
      bool nonnull = metadata && metadata.getHasImplicitObject() &&
                     argument.getArgNumber() == 0;
      if (metadata && metadata.hasParameterInfo())
        for (unsigned index = 0; index < metadata.getNumParams(); ++index)
          if (metadata.getParamOperandIndex(index) == argument.getArgNumber() &&
              metadata.getParameterKinds().asArrayRef()[index] !=
                  static_cast<int32_t>(LifetimeParameterKind::Value))
            nonnull = true;
      objectInfo[argument].unknownIncomingNullness = !nonnull;
    }
  }

  // Index completed object identities by their storage root's nearest
  // allocation scope, including scopes handled by structured-region analysis.
  // Memoize ancestor chains so nested scopes do not repeatedly walk them.
  DenseMap<Operation *, Operation *> enclosingScopes;
  auto findAllocationScope = [&](Operation *parent) {
    SmallVector<Operation *, 8> ancestors;
    Operation *scope = nullptr;
    while (parent) {
      auto found = enclosingScopes.find(parent);
      if (found != enclosingScopes.end()) {
        scope = found->second;
        break;
      }
      if (parent->hasTrait<OpTrait::AutomaticAllocationScope>()) {
        scope = parent;
        enclosingScopes.try_emplace(parent, scope);
        break;
      }
      ancestors.push_back(parent);
      parent = parent->getParentOp();
    }
    for (Operation *ancestor : ancestors)
      enclosingScopes.try_emplace(ancestor, scope);
    return scope;
  };
  // leaveScope orders these objects structurally before invalidation.
  for (const auto &entry : objectInfo) {
    if (entry.second.callerOwned)
      continue;
    auto *definition = entry.second.storageRoot.getDefiningOp();
    if (!definition)
      continue;
    if (auto *scope = findAllocationScope(definition->getParentOp()))
      scopeLocals[scope].push_back(entry.first);
  }
}

void LifetimeCheckPass::initializeObject(PathState &state, ObjectId object,
                                         Location loc) {
  auto info = getObjectInfo(object);
  if (!info)
    return;
  invalidatePredicates(state, ObjectTargets{{object}});
  state.pmap[object].clear();
  state.invalidHist.erase(object);
  state.nullHist.erase(object);
  state.storedValues.erase(object);
  state.occupancy.erase(object);
  detachOwner(state, object);
  if (!llvm::is_contained(state.liveObjects, object))
    state.liveObjects.push_back(object);
  if (info->category == ObjectInfo::Pointer && !info->callerOwned) {
    state.pmap[object].insert(State::getInvalid());
    state.invalidHist[object].add(object, InvalidStyle::NotInitialized, loc);
    ValueSummary summary;
    summary.pointer.emplace();
    summary.pointer->targets.mayBeInvalid = true;
    summary.pointer->invalidHistory = state.invalidHist[object];
    state.storedValues[object] = std::move(summary);
  } else if (info->category == ObjectInfo::Owner) {
    state.pmap[object].insert(State::getOwnedBy(object));
  } else {
    state.pmap[object].insert(State::getLocalValue(object));
  }
  if (currScope && !info->callerOwned)
    currScope->localValues.insert(object);
  if (auto fields = aggregates.find(object); fields != aggregates.end())
    for (ObjectId field : fields->second)
      if (field)
        initializeObject(state, field, loc);
}

void LifetimeCheckPass::synchronizeSummaries(PathState &state) {
  for (ObjectId pointer : ptrs) {
    auto found = state.pmap.find(pointer);
    if (found == state.pmap.end())
      continue;
    PointerValueSummary payload;
    if (auto stored = state.storedValues.find(pointer);
        stored != state.storedValues.end() && stored->second.pointer)
      payload.unknownNullness = stored->second.pointer->unknownNullness;
    for (const State &fact : found->second) {
      if (fact.isOwnedBy())
        payload.borrowedOwners.push_back(fact.getData());
      else if (fact.hasValue())
        payload.targets.objects.push_back(fact.getData());
      else if (fact == State::getNullPtr())
        payload.targets.mayBeNull = true;
      else if (fact == State::getInvalid())
        payload.targets.mayBeInvalid = true;
      else if (fact == State::getGlobal())
        payload.mayBeNonNull = true;
    }
    if (found->second.empty()) {
      payload.targets.unknown = true;
      payload.unknownNullness = true;
    }
    sortValues(payload.targets.objects);
    sortValues(payload.borrowedOwners);
    payload.invalidHistory = state.invalidHist.lookup(pointer);
    payload.nullOrigin = state.nullHist.lookup(pointer);
    state.storedValues[pointer].pointer = std::move(payload);
  }
}

void LifetimeCheckPass::checkAddressUse(Value address, Location loc,
                                        PathState &state) {
  auto summary = summarizeValue(address, state);
  if (!summary.pointer || (!summary.pointer->targets.mayBeNull &&
                           !summary.pointer->targets.mayBeInvalid))
    return;
  // The SSA payload, not the current value of its source slot, determines
  // whether this address is usable. A later write cannot repair this snapshot.
  ObjectId label = address;
  for (ObjectId origin : summary.readOrigins)
    if (ptrs.count(origin)) {
      label = origin;
      break;
    }
  DiagnosticWitness witness(activeOperation, label, loc, Direct);
  // A dereference-tagged pointer load and its following access describe one
  // source use. Keep its SSA snapshot, but share the witness identity.
  if (auto load = address.getDefiningOp<LoadOp>(); load && load.getIsDeref()) {
    witness.operation = load.getOperation();
    witness.location = load.getLoc();
  }
  witness.history = summary.pointer->invalidHistory;
  if (summary.pointer->targets.mayBeInvalid) {
    witness.pset.insert(State::getInvalid());
    if (witness.history.entries.empty())
      witness.history.add(label, NotInitialized, address.getLoc());
  }
  if (summary.pointer->targets.mayBeNull) {
    witness.pset.insert(State::getNullPtr());
    witness.nullOrigin = summary.pointer->nullOrigin.value_or(address.getLoc());
  }
  ptrs.insert(label);
  recordWitness(state, std::move(witness));
}

void LifetimeCheckPass::transferLoad(LoadOp load, PathState &state) {
  checkAddressUse(load.getAddr(), load.getLoc(), state);
  if (auto field = load.getAddr().getDefiningOp<GetMemberOp>()) {
    auto parents = resolveAddress(field.getAddr(), state);
    for (ObjectId object : parents.objects) {
      auto info = getObjectInfo(object);
      if (info && !tasks.count(object) &&
          (info->category == ObjectInfo::Record ||
           info->category == ObjectInfo::Aggregate) &&
          state.pmap[object].count(State::getInvalid()))
        checkUse(state, ObjectTargets{{object}}, load.getLoc());
    }
    if (parents.isDefinite()) {
      auto summary = state.storedValues.lookup(parents.objects.front());
      if (field.getIndex() < summary.elementPredicates.size()) {
        ValueSummary result;
        if (const auto &predicate = summary.elementPredicates[field.getIndex()])
          result.predicate = std::make_shared<const ValuePredicate>(*predicate);
        setValueSummary(state, load.getResult(), std::move(result));
        return;
      }
    }
  }
  ObjectTargets targets = resolveAddress(load.getAddr(), state);
  ValueSummary summary;
  bool first = true;
  for (ObjectId object : targets.objects) {
    auto info = getObjectInfo(object);
    bool pointer = info && info->category == ObjectInfo::Pointer;
    if ((pointer && load.getIsDeref()) ||
        (!pointer && state.pmap[object].count(State::getInvalid())))
      checkUse(state, ObjectTargets{{object}}, load.getLoc());
    auto found = state.storedValues.find(object);
    ValueSummary value =
        found == state.storedValues.end() ? ValueSummary{} : found->second;
    if (first)
      summary = std::move(value);
    else
      summary = joinValues(summary, value);
    first = false;
    if (!llvm::is_contained(summary.readOrigins, object))
      summary.readOrigins.push_back(object);
    if (load.getSourceIsXvalue() &&
        !llvm::is_contained(summary.xvalueOrigins, object))
      summary.xvalueOrigins.push_back(object);
  }
  if (isa<cir::PointerType>(load.getType()) && !summary.pointer) {
    summary.pointer.emplace();
    summary.pointer->targets.unknown = true;
    summary.pointer->unknownNullness = true;
  }
  summary.unknown |=
      targets.unknown || targets.mayBeInvalid || targets.mayBeNull;
  // An unknown address alternative or an observable load can produce a value
  // other than the constant last stored in one of the known local objects.
  if (summary.unknown || load.getIsVolatile() || load.getMemOrder())
    clearBooleanValues(summary);
  if (summary.pointer) {
    summary.pointer->targets.unknown |= summary.unknown;
    summary.pointer->unknownNullness |= summary.unknown;
  }
  setValueSummary(state, load.getResult(), std::move(summary));
  if (targets.isDefinite())
    copyConditionalPointerSnapshots(state, targets.objects.front(), true,
                                    load.getResult(), false);
}

void LifetimeCheckPass::forgetAggregateCallFields(
    PathState &state, const ObjectTargets &targets) {
  // A returned record overwrites the destination's old member contents. Only
  // forget fields already tracked by the pointer-lifetime model; the opaque
  // callee supplies no proof of non-nullness or of a particular pointee.
  ValueSummary unknown;
  unknown.unknown = true;
  unknown.pointer.emplace();
  unknown.pointer->targets.unknown = true;
  unknown.pointer->unknownNullness = true;
  for (ObjectId object : targets.objects) {
    auto fields = aggregates.find(object);
    if (fields == aggregates.end())
      continue;
    for (ObjectId field : fields->second) {
      if (!field)
        continue;
      ObjectTargets fieldTargets{{field}, !targets.isDefinite()};
      auto info = getObjectInfo(field);
      if (info && info->category == ObjectInfo::Pointer) {
        storeSummary(state, fieldTargets, unknown);
        if (fieldTargets.isDefinite()) {
          state.pmap[field].clear();
          state.invalidHist.erase(field);
          state.nullHist.erase(field);
        }
      } else if (info && info->category == ObjectInfo::Aggregate) {
        forgetAggregateCallFields(state, fieldTargets);
      }
    }
  }
}

void LifetimeCheckPass::transferStore(StoreOp store, PathState &state) {
  checkAddressUse(store.getAddr(), store.getLoc(), state);
  ObjectTargets targets = resolveAddress(store.getAddr(), state);
  ValueSummary summary = summarizeValue(store.getValue(), state);
  // An unknown pointer may designate previously escaped local storage even
  // when its target set contains no concrete object for storeSummary to visit.
  if (targets.unknown)
    clearStoredBooleanValues(state);
  bool spill = false;
  if (targets.isDefinite())
    if (auto alloca = targets.objects.front().getDefiningOp<AllocaOp>())
      spill = alloca->hasAttr("lifetime_value_spill");
  if (spill) {
    storeSummary(state, targets, summary);
    copyConditionalPointerSnapshots(state, store.getValue(), false,
                                    targets.objects.front(), true);
    return;
  }
  // Evaluating the address and RHS precedes replacing the destination. An
  // overwritten object is not tainted just because the RHS was diagnosed.
  if (isa<cir::IntType, cir::BoolType, cir::FPTypeInterface>(
          store.getValue().getType())) {
    if (!summary.xvalueOrigins.empty())
      markMoved(state, ObjectTargets{summary.xvalueOrigins}, store.getLoc());
    reinitializeObjects(state, targets, store.getLoc());
  }
  summary.xvalueOrigins.clear();
  summary.readOrigins.clear();
  storeSummary(state, targets, summary);
  if (isa<RecordType>(store.getValue().getType()) &&
      store.getValue().getDefiningOp<CallOp>())
    forgetAggregateCallFields(state, targets);
  for (ObjectId object : targets.objects) {
    auto info = getObjectInfo(object);
    if (!info)
      continue;
    if (info->category == ObjectInfo::Pointer && summary.pointer) {
      PSetType replacement;
      for (ObjectId pointee : summary.pointer->targets.objects)
        replacement.insert(State::getLocalValue(pointee));
      for (ObjectId owner : summary.pointer->borrowedOwners)
        replacement.insert(State::getOwnedBy(owner));
      if (summary.pointer->targets.mayBeNull)
        replacement.insert(State::getNullPtr());
      if (summary.pointer->targets.mayBeInvalid)
        replacement.insert(State::getInvalid());
      if (summary.pointer->mayBeNonNull &&
          summary.pointer->targets.objects.empty() &&
          summary.pointer->borrowedOwners.empty())
        replacement.insert(State::getGlobal());
      if (targets.isDefinite()) {
        state.pmap[object] = replacement;
        state.invalidHist.erase(object);
        state.nullHist.erase(object);
      } else {
        llvm::set_union(state.pmap[object], replacement);
      }
      if (replacement.count(State::getNullPtr()))
        state.nullHist[object] =
            summary.pointer->nullOrigin.value_or(store.getValue().getLoc());
      if (replacement.count(State::getInvalid()))
        state.invalidHist[object] = summary.pointer->invalidHistory;
    }
  }
  // Keep the existing aggregate-constant and escaping coroutine/lambda rules.
  if (targets.isDefinite()) {
    ObjectId object = targets.objects.front();
    bool aggregateConstant = aggregates.count(object) &&
                             store.getValue().getDefiningOp<ConstantOp>();
    {
      StateGuard guard(*this, state);
      if (aggregateConstant)
        updatePointsTo(object, store.getValue(), store.getValue().getLoc());
      if (currScope && currScope->localTempTasks.count(store.getValue()))
        checkCoroTaskStore(store);
      else if (currScope)
        checkLambdaCaptureStore(store);
    }
    if (aggregateConstant)
      synchronizeSummaries(state);
    copyConditionalPointerSnapshots(state, store.getValue(), false, object,
                                    true);
  }
}

void LifetimeCheckPass::transferCopy(CopyOp copy, PathState &state) {
  // The existing copy model transfers pointer-like record payloads, not
  // arbitrary boolean subobjects. A bulk write can overwrite those subobjects
  // or previously escaped storage, including when the copy itself is unmodeled.
  clearStoredBooleanValues(state);
  auto record = dyn_cast<RecordType>(copy.getType().getPointee());
  if (!record)
    return;
  checkAddressUse(copy.getSrc(), copy.getLoc(), state);
  checkAddressUse(copy.getDst(), copy.getLoc(), state);
  ObjectTargets sources = resolveAddress(copy.getSrc(), state);
  // A whole-record copy reads the source even when no memberwise transfer is
  // modeled. Null pointer contents may be copied; invalid source facts retain
  // the existing move/lifetime diagnostics at the source read.
  for (ObjectId source : sources.objects)
    if (state.pmap.lookup(source).count(State::getInvalid()))
      checkUse(state, ObjectTargets{{source}}, copy.getLoc());
  // Trivial copies of pointer-like records are lowered to cir.copy, rather
  // than a copy constructor call. Transfer the existing whole-object pointer
  // facts; this is not memberwise analysis of an arbitrary aggregate.
  auto properties = cir::getLifetimeRecord(record, theModule);
  if (!properties || !properties.getPointer())
    return;
  ObjectTargets destinations = resolveAddress(copy.getDst(), state);
  if (sources.isDefinite() && destinations.isDefinite() &&
      sources.objects.front() == destinations.objects.front())
    return;
  PSetType copiedSet;
  InvalidHist history;
  std::optional<Location> nullOrigin;
  ValueSummary summary;
  bool first = true;
  for (ObjectId source : sources.objects) {
    llvm::set_union(copiedSet, state.pmap.lookup(source));
    if (auto found = state.invalidHist.find(source);
        found != state.invalidHist.end())
      appendUnique(history.entries,
                   ArrayRef<InvalidHistEntry>(found->second.entries));
    if (!nullOrigin)
      nullOrigin = state.nullHist.lookup(source);
    auto value = state.storedValues.lookup(source);
    summary = first ? value : joinValues(summary, value);
    first = false;
  }
  if (!summary.pointer) {
    summary.pointer.emplace();
    for (const State &fact : copiedSet) {
      if (fact.isOwnedBy())
        summary.pointer->borrowedOwners.push_back(fact.getData());
      else if (fact.hasValue())
        summary.pointer->targets.objects.push_back(fact.getData());
      else if (fact == State::getNullPtr())
        summary.pointer->targets.mayBeNull = true;
      else if (fact == State::getInvalid())
        summary.pointer->targets.mayBeInvalid = true;
      else if (fact == State::getGlobal())
        summary.pointer->mayBeNonNull = true;
    }
    sortValues(summary.pointer->targets.objects);
    sortValues(summary.pointer->borrowedOwners);
    summary.pointer->unknownNullness = true;
  }
  // Unknown source storage yields unknown contents. An empty pset is not
  // evidence of either an invalid object or a non-null conceptual pointee.
  summary.pointer->targets.unknown |= sources.unknown || first;
  summary.pointer->unknownNullness |= sources.unknown || first;
  summary.pointer->targets.mayBeInvalid |= copiedSet.count(State::getInvalid());
  summary.pointer->targets.mayBeNull |= copiedSet.count(State::getNullPtr());
  summary.pointer->invalidHistory = history;
  summary.pointer->nullOrigin = nullOrigin;
  summary.readOrigins.clear();
  summary.xvalueOrigins.clear();
  storeSummary(state, destinations, summary);
  for (ObjectId destination : destinations.objects) {
    auto info = getObjectInfo(destination);
    if (!info || info->category != ObjectInfo::Pointer)
      continue;
    if (destinations.isDefinite()) {
      state.pmap[destination] = copiedSet;
      state.invalidHist[destination] = history;
      state.nullHist[destination] = nullOrigin;
    } else {
      llvm::set_union(state.pmap[destination], copiedSet);
      appendUnique(state.invalidHist[destination].entries,
                   ArrayRef<InvalidHistEntry>(history.entries));
      if (nullOrigin)
        state.nullHist[destination] = nullOrigin;
    }
  }
  if (sources.isDefinite() && destinations.isDefinite())
    copyConditionalPointerSnapshots(state, sources.objects.front(), true,
                                    destinations.objects.front(), true);
}

void LifetimeCheckPass::visitPredicates(
    PathState &state,
    llvm::function_ref<void(std::optional<ValuePredicate> &)> visit) {
  auto visitValue = [&](ValueSummary &value) {
    if (value.predicate) {
      std::optional<ValuePredicate> predicate = *value.predicate;
      visit(predicate);
      if (!predicate)
        value.predicate.reset();
      else if (*predicate != *value.predicate)
        value.predicate =
            std::make_shared<const ValuePredicate>(std::move(*predicate));
    }
    for (auto &predicate : value.elementPredicates)
      visit(predicate);
  };
  for (auto &entry : state.values)
    visitValueSummary(entry.second, visitValue);
  for (auto &entry : state.storedValues)
    visitValueSummary(entry.second, visitValue);
}

void LifetimeCheckPass::invalidatePredicates(PathState &state,
                                             const ObjectTargets &targets) {
  auto affected = [&](ObjectId object) {
    return targets.unknown || llvm::is_contained(targets.objects, object);
  };
  visitPredicates(state, [&](std::optional<ValuePredicate> &predicate) {
    if (!predicate)
      return;
    if (predicate->domain != ValuePredicate::ConditionalMove) {
      if (llvm::any_of(predicate->objects.objects, affected))
        predicate.reset();
      return;
    }
    // A write invalidates only facts about the overwritten storage. It need
    // not invalidate a still-useful result predicate for another object.
    for (auto *snapshots : {&predicate->before, &predicate->after})
      for (auto &snapshot : *snapshots) {
        if (affected(snapshot.object))
          snapshot.valid = false;
        if (affected(snapshot.object) ||
            llvm::any_of(snapshot.ownershipGroup, affected))
          snapshot.ownershipValid = false;
      }
    for (auto *snapshots :
         {&predicate->pointersBefore, &predicate->pointersAfter})
      for (auto &snapshot : *snapshots)
        if (snapshot.stored && affected(snapshot.value))
          snapshot.valid = false;
  });
  for (ObjectId object : targets.objects)
    state.lastWrites[object] = activeOperation;
}

void LifetimeCheckPass::invalidateConditionalDependencies(PathState &state,
                                                          ObjectId object,
                                                          bool storage) {
  auto affected = [&](const std::optional<PointerValueSummary> &pointer) {
    return pointer &&
           (llvm::is_contained(pointer->borrowedOwners, object) ||
            (storage && llvm::is_contained(pointer->targets.objects, object)));
  };
  visitPredicates(state, [&](std::optional<ValuePredicate> &predicate) {
    if (!predicate || predicate->domain != ValuePredicate::ConditionalMove)
      return;
    // Test dependencies in each saved alternative, not in their joined
    // current payload. Clearing the container destroys the success-side
    // resource, but cannot destroy the failure-side source resource.
    for (auto *snapshots : {&predicate->before, &predicate->after})
      for (auto &snapshot : *snapshots)
        if (affected(snapshot.pointer) ||
            snapshot.pset.count(State::getOwnedBy(object)) ||
            (storage && (snapshot.object == object ||
                         snapshot.pset.count(State::getLocalValue(object)))))
          snapshot.valid = false;
    for (auto *snapshots :
         {&predicate->pointersBefore, &predicate->pointersAfter})
      for (auto &snapshot : *snapshots)
        if (affected(snapshot.pointer))
          snapshot.valid = false;
  });
}

void LifetimeCheckPass::invalidateConditionalOwnership(PathState &state,
                                                       ObjectId object) {
  visitPredicates(state, [&](std::optional<ValuePredicate> &predicate) {
    if (!predicate || predicate->domain != ValuePredicate::ConditionalMove)
      return;
    for (auto *snapshots : {&predicate->before, &predicate->after})
      for (auto &snapshot : *snapshots)
        if (snapshot.object == object ||
            llvm::is_contained(snapshot.ownershipGroup, object))
          snapshot.ownershipValid = false;
  });
}

void LifetimeCheckPass::copyConditionalPointerSnapshots(
    PathState &state, Value source, bool sourceStored, Value destination,
    bool destinationStored) {
  visitPredicates(state, [&](std::optional<ValuePredicate> &predicate) {
    if (!predicate || predicate->domain != ValuePredicate::ConditionalMove)
      return;
    for (bool after : {false, true}) {
      auto &objects = after ? predicate->after : predicate->before;
      auto &pointers =
          after ? predicate->pointersAfter : predicate->pointersBefore;
      SmallVector<ValuePredicate::PointerSnapshot, 0> copies;
      if (sourceStored)
        for (const auto &snapshot : objects)
          if (snapshot.object == source && snapshot.pointer) {
            ValuePredicate::PointerSnapshot copy;
            copy.pointer = snapshot.pointer;
            copy.valid = snapshot.valid && state.lastWrites.lookup(source) ==
                                               snapshot.expectedWrite;
            copies.push_back(std::move(copy));
          }
      for (const auto &snapshot : pointers)
        if (snapshot.value == source && snapshot.stored == sourceStored) {
          auto copy = snapshot;
          copy.valid &= !sourceStored || state.lastWrites.lookup(source) ==
                                             snapshot.expectedWrite;
          copies.push_back(std::move(copy));
        }
      for (auto &copy : copies) {
        copy.value = destination;
        copy.stored = destinationStored;
        copy.expectedWrite =
            destinationStored ? state.lastWrites.lookup(destination) : nullptr;
        auto info = getObjectInfo(destination);
        if (destinationStored && copy.tuplePath.empty() && copy.pointer &&
            info && info->category == ObjectInfo::Pointer) {
          // The new slot receives the saved branch payload, not the joined
          // payload that happened to be copied in the physical CIR order.
          ValuePredicate::ObjectSnapshot object;
          object.object = destination;
          object.pointer = copy.pointer;
          object.valid = copy.valid;
          object.expectedWrite = copy.expectedWrite;
          object.history = copy.pointer->invalidHistory;
          object.nullOrigin = copy.pointer->nullOrigin;
          object.occupancy = getOccupancy(state, ObjectTargets{{destination}});
          for (ObjectId pointee : copy.pointer->targets.objects)
            object.pset.insert(State::getLocalValue(pointee));
          for (ObjectId owner : copy.pointer->borrowedOwners)
            object.pset.insert(State::getOwnedBy(owner));
          if (copy.pointer->targets.mayBeNull)
            object.pset.insert(State::getNullPtr());
          if (copy.pointer->targets.mayBeInvalid)
            object.pset.insert(State::getInvalid());
          if (copy.pointer->mayBeNonNull && object.pset.empty())
            object.pset.insert(State::getGlobal());
          auto existing = llvm::find_if(objects, [&](const auto &snapshot) {
            return snapshot.object == destination;
          });
          if (existing == objects.end())
            objects.push_back(std::move(object));
          else
            *existing = std::move(object);
          continue;
        }
        auto existing = llvm::find_if(pointers, [&](const auto &snapshot) {
          return snapshot.value == destination &&
                 snapshot.stored == destinationStored &&
                 snapshot.tuplePath == copy.tuplePath;
        });
        if (existing == pointers.end())
          pointers.push_back(std::move(copy));
        else
          *existing = std::move(copy);
      }
    }
  });
}

LifetimeCheckPass::ValuePredicate
LifetimeCheckPass::makeConditionalMovePredicate(const PathState &before,
                                                const PathState &after,
                                                const ObjectTargets &targets,
                                                Operation *site) {
  ValuePredicate predicate;
  predicate.domain = ValuePredicate::ConditionalMove;
  predicate.objects = targets;
  predicate.conditionSite = site;
  auto captureObject = [&](const PathState &state, ObjectId object) {
    ValuePredicate::ObjectSnapshot snapshot;
    snapshot.object = object;
    snapshot.pset = state.pmap.lookup(object);
    snapshot.history = state.invalidHist.lookup(object);
    snapshot.nullOrigin = state.nullHist.lookup(object);
    auto value = state.storedValues.find(object);
    if (value != state.storedValues.end()) {
      snapshot.pointer = value->second.pointer;
      snapshot.valueOccupancy = value->second.objectOccupancy;
    }
    snapshot.occupancy = getOccupancy(state, ObjectTargets{{object}});
    snapshot.ownershipGroup = state.ownershipGroups.lookup(object);
    snapshot.expectedWrite = site;
    return snapshot;
  };
  SmallVector<ObjectId, 16> objects;
  for (const auto &entry : before.pmap)
    objects.push_back(entry.first);
  for (const auto &entry : after.pmap)
    if (!llvm::is_contained(objects, entry.first))
      objects.push_back(entry.first);
  appendUnique(objects, ArrayRef<ObjectId>(targets.objects));
  sortValues(objects);
  for (ObjectId object : objects) {
    auto first = captureObject(before, object);
    auto second = captureObject(after, object);
    if (first == second && !llvm::is_contained(targets.objects, object))
      continue;
    predicate.before.push_back(std::move(first));
    predicate.after.push_back(std::move(second));
  }
  // Capture changed SSA pointer payloads and logical tuple elements. The
  // payloads contain no predicates, avoiding recursively growing snapshots.
  auto capturePointers = [&](const auto &firstMap, const auto &secondMap,
                             bool stored) {
    SmallVector<Value, 16> values;
    for (const auto &entry : firstMap)
      if (secondMap.count(entry.first))
        values.push_back(entry.first);
    sortValues(values);
    for (Value value : values) {
      SmallVector<unsigned, 0> path;
      auto capture = [&](auto &&self, const ValueSummary &first,
                         const ValueSummary &second) -> void {
        // Root stored pointers are already in ObjectSnapshot.
        if ((!stored || !path.empty()) && first.pointer != second.pointer) {
          ValuePredicate::PointerSnapshot snapshot;
          snapshot.value = value;
          snapshot.tuplePath = path;
          snapshot.stored = stored;
          snapshot.expectedWrite = stored ? site : nullptr;
          snapshot.pointer = first.pointer;
          predicate.pointersBefore.push_back(snapshot);
          snapshot.pointer = second.pointer;
          predicate.pointersAfter.push_back(std::move(snapshot));
        }
        for (unsigned index = 0; index < std::min(first.tupleValues.size(),
                                                  second.tupleValues.size());
             ++index) {
          path.push_back(index);
          self(self, first.tupleValues[index], second.tupleValues[index]);
          path.pop_back();
        }
      };
      capture(capture, firstMap.find(value)->second,
              secondMap.find(value)->second);
    }
  };
  capturePointers(before.values, after.values, false);
  capturePointers(before.storedValues, after.storedValues, true);
  return predicate;
}

void LifetimeCheckPass::setConditionalMoveSite(
    PathState &state, const ValuePredicate &predicate) {
  for (const auto &snapshot : predicate.before)
    state.lastWrites[snapshot.object] = predicate.conditionSite;
  for (const auto &snapshot : predicate.pointersBefore)
    if (snapshot.stored)
      state.lastWrites[snapshot.value] = predicate.conditionSite;
}

void LifetimeCheckPass::joinStateInto(PathState &result,
                                      const PathState &state) {
  assert(&result != &state && "join inputs must not alias");
  for (const auto &entry : state.pmap)
    llvm::set_union(result.pmap[entry.first], entry.second);
  for (const auto &entry : state.invalidHist)
    appendUnique(result.invalidHist[entry.first].entries,
                 ArrayRef<InvalidHistEntry>(entry.second.entries));
  for (const auto &entry : state.nullHist)
    if (!result.nullHist.lookup(entry.first))
      result.nullHist[entry.first] = entry.second;
  for (auto &entry : result.values)
    if (!state.values.count(entry.first))
      clearBooleanValues(entry.second);
  for (const auto &entry : state.values) {
    auto found = result.values.find(entry.first);
    if (found == result.values.end()) {
      auto inserted = result.values.insert(entry);
      clearBooleanValues(inserted.first->second);
    } else
      found->second = joinValues(found->second, entry.second);
  }
  for (auto &entry : result.storedValues)
    if (!state.storedValues.count(entry.first))
      clearBooleanValues(entry.second);
  for (const auto &entry : state.storedValues) {
    auto found = result.storedValues.find(entry.first);
    if (found == result.storedValues.end()) {
      auto inserted = result.storedValues.insert(entry);
      clearBooleanValues(inserted.first->second);
    } else
      found->second = joinValues(found->second, entry.second);
  }
  // No map entry denotes unknown occupancy, not absence of an incoming
  // path. A replacement in just one branch cannot establish presence.
  for (auto &entry : result.occupancy)
    if (!state.occupancy.count(entry.first))
      entry.second.unknown = true;
  for (const auto &entry : state.occupancy) {
    auto found = result.occupancy.find(entry.first);
    if (found == result.occupancy.end()) {
      result.occupancy.insert(entry);
      result.occupancy[entry.first].unknown = true;
      continue;
    }
    found->second.mayBeEmpty |= entry.second.mayBeEmpty;
    found->second.mayBePresent |= entry.second.mayBePresent;
    found->second.unknown |= entry.second.unknown;
    appendUnique(found->second.emptyOrigins,
                 ArrayRef<Location>(entry.second.emptyOrigins));
  }
  appendUnique(result.liveObjects, ArrayRef<ObjectId>(state.liveObjects));
  for (auto &entry : result.lastWrites)
    if (state.lastWrites.lookup(entry.first) != entry.second)
      entry.second = nullptr;
  // Co-ownership is a must fact, unlike invalid/null may facts. A relation
  // absent from either incoming path cannot keep a resource alive.
  SmallVector<ObjectId, 4> forgottenGroups;
  for (auto &entry : result.ownershipGroups) {
    auto other = state.ownershipGroups.find(entry.first);
    if (other == state.ownershipGroups.end()) {
      forgottenGroups.push_back(entry.first);
      continue;
    }
    llvm::erase_if(entry.second, [&](ObjectId holder) {
      return !llvm::is_contained(other->second, holder);
    });
    if (entry.second.size() < 2)
      forgottenGroups.push_back(entry.first);
  }
  for (ObjectId object : forgottenGroups)
    result.ownershipGroups.erase(object);
  for (const auto &entry : state.ownerMutations)
    appendUnique(result.ownerMutations[entry.first],
                 ArrayRef<Operation *>(entry.second));
  for (const auto &witness : state.diagnostics) {
    auto found = llvm::find_if(result.diagnostics, [&](const auto &other) {
      return witness.operation == other.operation &&
             witness.object == other.object && witness.style == other.style;
    });
    if (found == result.diagnostics.end()) {
      result.diagnostics.push_back(witness);
    } else {
      llvm::set_union(found->pset, witness.pset);
      appendUnique(found->history.entries,
                   ArrayRef<InvalidHistEntry>(witness.history.entries));
      if (!found->nullOrigin)
        found->nullOrigin = witness.nullOrigin;
    }
  }
}

LifetimeCheckPass::PathState
LifetimeCheckPass::joinStates(ArrayRef<const PathState *> states) {
  if (states.empty())
    return {};
  PathState result = *states.front();
  for (const PathState *state : states.drop_front())
    joinStateInto(result, *state);
  return result;
}

bool LifetimeCheckPass::equivalentStates(const PathState &a,
                                         const PathState &b) {
  // Diagnostic accumulation does not drive the fixed point. Every witness is
  // retained separately and emitted once after solving the function.
  return a.pmap == b.pmap && a.invalidHist == b.invalidHist &&
         a.nullHist == b.nullHist && a.values == b.values &&
         a.storedValues == b.storedValues && a.occupancy == b.occupancy &&
         a.liveObjects == b.liveObjects && a.lastWrites == b.lastWrites &&
         a.ownerMutations == b.ownerMutations &&
         a.ownershipGroups == b.ownershipGroups;
}

void LifetimeCheckPass::mergeFlow(FlowResult &destination, FlowResult source) {
  if (source.normal) {
    if (destination.normal)
      joinStateInto(*destination.normal, *source.normal);
    else
      destination.normal = std::move(source.normal);
  }
  auto merge = [&](SmallVectorImpl<PathState> &to,
                   SmallVectorImpl<PathState> &from) {
    if (from.empty())
      return;
    to.append(std::make_move_iterator(from.begin()),
              std::make_move_iterator(from.end()));
    if (to.size() > 1) {
      PathState joined = std::move(to.front());
      for (const PathState &state : llvm::drop_begin(to))
        joinStateInto(joined, state);
      to.clear();
      to.push_back(std::move(joined));
    }
  };
  merge(destination.breaks, source.breaks);
  merge(destination.continues, source.continues);
  merge(destination.returns, source.returns);
  for (auto &jump : source.gotos) {
    auto found = llvm::find_if(destination.gotos, [&](const auto &other) {
      return other.label == jump.label;
    });
    if (found == destination.gotos.end())
      destination.gotos.push_back(std::move(jump));
    else
      joinStateInto(found->state, jump.state);
  }
  for (auto &exception : source.exceptions) {
    auto found = llvm::find_if(destination.exceptions, [&](const auto &other) {
      return other.thrownType == exception.thrownType &&
             other.typeInfo == exception.typeInfo;
    });
    if (found == destination.exceptions.end())
      destination.exceptions.push_back(std::move(exception));
    else
      joinStateInto(found->state, exception.state);
  }
}

std::optional<bool>
LifetimeCheckPass::evaluateCondition(Value condition, const PathState &state) {
  if (auto constant = condition.getDefiningOp<ConstantOp>())
    if (auto value = dyn_cast<cir::BoolAttr>(constant.getValue()))
      return value.getValue();
  // A captured SSA result is authoritative even if a library predicate used
  // to compute it has changed since that evaluation.
  if (auto recorded = state.values.find(condition);
      recorded != state.values.end() && recorded->second.booleanValue)
    return recorded->second.booleanValue;
  if (auto negate = condition.getDefiningOp<NotOp>())
    if (auto known = evaluateCondition(negate.getInput(), state))
      return !*known;
  auto summary = summarizeValue(condition, state);
  if (summary.booleanValue)
    return summary.booleanValue;
  if (!summary.predicate)
    return std::nullopt;
  const auto &predicate = *summary.predicate;
  if (predicate.domain == ValuePredicate::ConditionalMove)
    return std::nullopt;
  auto occupancy = getOccupancy(state, predicate.objects);
  if (occupancy.unknown || occupancy.mayBeEmpty == occupancy.mayBePresent)
    return std::nullopt;
  return occupancy.mayBePresent == predicate.trueMeansPresent;
}

LifetimeCheckPass::PathState
LifetimeCheckPass::refineCondition(Value condition, bool truth,
                                   PathState state) {
  auto summary = summarizeValue(condition, state);
  if (!summary.predicate)
    return state;
  const auto &predicate = *summary.predicate;
  if (predicate.domain == ValuePredicate::ConditionalMove) {
    bool consumed = truth == predicate.trueMeansPresent;
    const auto &snapshots = consumed ? predicate.after : predicate.before;
    auto valid = [&](const ValuePredicate::ObjectSnapshot &snapshot) {
      return snapshot.valid &&
             state.lastWrites.lookup(snapshot.object) == snapshot.expectedWrite;
    };
    for (const auto &snapshot : snapshots) {
      if (!valid(snapshot))
        continue;
      state.pmap[snapshot.object] = snapshot.pset;
      state.invalidHist[snapshot.object] = snapshot.history;
      state.nullHist[snapshot.object] = snapshot.nullOrigin;
      state.storedValues[snapshot.object].pointer = snapshot.pointer;
      state.storedValues[snapshot.object].objectOccupancy =
          snapshot.valueOccupancy;
      state.occupancy[snapshot.object] = snapshot.occupancy;
    }
    for (const auto &snapshot :
         consumed ? predicate.pointersAfter : predicate.pointersBefore) {
      if (!snapshot.valid ||
          (snapshot.stored &&
           state.lastWrites.lookup(snapshot.value) != snapshot.expectedWrite))
        continue;
      auto &values = snapshot.stored ? state.storedValues : state.values;
      auto found = values.find(snapshot.value);
      if (found == values.end())
        continue;
      ValueSummary *value = &found->second;
      for (unsigned index : snapshot.tuplePath) {
        if (index >= value->tupleValues.size()) {
          value = nullptr;
          break;
        }
        value = &value->tupleValues[index];
      }
      if (value)
        value->pointer = snapshot.pointer;
    }
    // Re-establish only complete, unchanged ownership relations. Restoring a
    // single peer after another holder was reset would resurrect a resource.
    auto validGroup = [&](const ValuePredicate::ObjectSnapshot &snapshot) {
      if (!valid(snapshot) || !snapshot.ownershipValid)
        return false;
      return llvm::all_of(snapshot.ownershipGroup, [&](ObjectId holder) {
        auto peer = llvm::find_if(snapshots, [&](const auto &candidate) {
          return candidate.object == holder;
        });
        return peer != snapshots.end() && valid(*peer) &&
               peer->ownershipValid &&
               peer->ownershipGroup == snapshot.ownershipGroup;
      });
    };
    for (const auto &snapshot : snapshots) {
      if (!validGroup(snapshot))
        continue;
      state.ownershipGroups.erase(snapshot.object);
      for (auto &entry : state.ownershipGroups)
        llvm::erase(entry.second, snapshot.object);
    }
    for (const auto &snapshot : snapshots)
      if (validGroup(snapshot) && snapshot.ownershipGroup.size() > 1)
        state.ownershipGroups[snapshot.object] = snapshot.ownershipGroup;
    SmallVector<ObjectId, 4> singletons;
    for (const auto &entry : state.ownershipGroups)
      if (entry.second.size() < 2)
        singletons.push_back(entry.first);
    for (ObjectId object : singletons)
      state.ownershipGroups.erase(object);
    return state;
  }
  if (!predicate.objects.isDefinite())
    return state;
  ObjectId object = predicate.objects.objects.front();
  bool present = truth == predicate.trueMeansPresent;
  auto &occupancy = state.occupancy[object];
  occupancy.unknown = false;
  occupancy.mayBePresent = present;
  occupancy.mayBeEmpty = !present;
  if (present) {
    occupancy.emptyOrigins.clear();
    // Presence proves a null fact impossible. A standard smart pointer is
    // empty after a move, so its nonempty branch also excludes moved-from
    // alternatives at a join. Other invalidity, including dangling storage,
    // and moved-from records without that contract remain possible.
    state.pmap[object].erase(State::getNullPtr());
    state.nullHist.erase(object);
    auto info = getObjectInfo(object);
    if (predicate.domain == ValuePredicate::OwnerOccupancy && info &&
        hasEmptyMovedFromState(getRecordLibraryKind(info->type, theModule))) {
      auto history = state.invalidHist.find(object);
      if (history != state.invalidHist.end()) {
        auto &entries = history->second.entries;
        bool hadHistory = !entries.empty();
        llvm::erase_if(entries, [](const auto &entry) {
          return entry.style == InvalidStyle::MovedFrom;
        });
        if (hadHistory && entries.empty()) {
          state.pmap[object].erase(State::getInvalid());
          state.invalidHist.erase(history);
        }
      }
    }
  }
  return state;
}

LifetimeCheckPass::PathState
LifetimeCheckPass::finishRegionOperation(Operation *operation,
                                         ValueRange yielded, PathState state) {
  for (auto pair : llvm::zip(operation->getResults(), yielded)) {
    setValueSummary(state, std::get<0>(pair),
                    summarizeValue(std::get<1>(pair), state));
    copyConditionalPointerSnapshots(state, std::get<1>(pair), false,
                                    std::get<0>(pair), false);
  }
  return state;
}

void LifetimeCheckPass::applyArgumentEffects(
    const CallInfo &info, PathState &normal, PathState *exceptional,
    llvm::function_ref<bool(unsigned)> skip) {
  const Location location = info.op->getLoc();
  for (auto [index, kind] : llvm::enumerate(info.parameterKinds)) {
    if (!kind || skip(index))
      continue;
    auto targets = resolveAddress(info.args[index], normal);
    if (*kind != LifetimeParameterKind::RValueReference) {
      // Passing a pointer by value does not access its pointee. In particular,
      // destroying a moved-from record through its pointer remains valid.
      if (*kind != LifetimeParameterKind::LValueReference)
        continue;
      for (ObjectId object : targets.objects) {
        auto type = getObjectInfo(object);
        if (type && !tasks.count(object) &&
            (type->category == ObjectInfo::Record ||
             type->category == ObjectInfo::Aggregate) &&
            normal.pmap[object].count(State::getInvalid()))
          checkUse(normal, ObjectTargets{{object}}, location);
      }
      continue;
    }
    checkUse(normal, targets, location, UseAfterMove);
    // Obtaining an element reference does not consume the containing tuple.
    // Keep the generic source checks, result summary, and exceptional flow.
    if (info.lifetime &&
        info.lifetime.getLibraryFunction() == LifetimeLibraryFunction::TupleGet)
      continue;
    markMoved(normal, targets, location);
    if (exceptional)
      markMoved(*exceptional, targets, location);
  }
}

void LifetimeCheckPass::applyReceiverEffects(const CallInfo &info,
                                                 PathState &state,
                                                 Attribute special) {
  assert(activeState == &state && "expected the installed path state");
  LifetimeFunctionAttr lifetime = info.lifetime;
  if (!lifetime || !lifetime.getIsMethod() ||
      !lifetime.getHasImplicitObject() ||
      isa_and_present<CXXCtorAttr, CXXDtorAttr>(special))
    return;
  const bool assignment = isa_and_present<CXXAssignAttr>(special);
  const bool definite = info.receiver.isDefinite();
  const Location location = info.op->getLoc();
  ObjectTargets mutated = info.receiver;
  mutated.objects.clear();
  SmallVector<ObjectId, 4> pointers;
  for (ObjectId object : info.receiver.objects) {
    bool owner = owners.count(object);
    bool pointer = ptrs.count(object);
    // checkCall already applied the legacy rules to a definite GSL receiver.
    // Coroutine tasks keep their argument-style checks.
    if (tasks.count(object) || (definite && (owner || pointer)))
      continue;
    if (pointer)
      pointers.push_back(object);
    else if ((owner || hasLibraryContents(object)) &&
             (assignment || !lifetime.getIsConst()))
      mutated.objects.push_back(object);
  }
  if (!definite && mutated.isDefinite())
    mutated.unknown = true;
  if (!mutated.objects.empty())
    mutateOwnerContents(state, mutated, location);
  if (pointers.empty())
    return;
  if (!assignment) {
    for (ObjectId pointer : pointers)
      checkPointerDeref(pointer, location);
    return;
  }
  if (info.args.size() < 2)
    return;
  // Each candidate either keeps its value or receives the source's value.
  ObjectTargets source = resolveAddress(info.args[1], state);
  for (ObjectId pointer : pointers)
    for (ObjectId object : source.objects) {
      if (object == pointer)
        continue;
      // Copy before insertion, which can invalidate DenseMap references.
      PSetType pset = getPmap().lookup(object);
      InvalidHist history = invalidHist.lookup(object);
      std::optional<Location> nullOrigin = pmapNullHist.lookup(object);
      llvm::set_union(getPmap()[pointer], pset);
      appendUnique(invalidHist[pointer].entries,
                   ArrayRef<InvalidHistEntry>(history.entries));
      if (nullOrigin && !pmapNullHist.lookup(pointer))
        pmapNullHist[pointer] = nullOrigin;
    }
}

LifetimeCheckPass::FlowResult
LifetimeCheckPass::analyzeCall(Operation *operation, PathState state) {
  auto call = dyn_cast<CallOp>(operation);
  if (!call)
    return {std::move(state)};
  CallInfo info;
  info.op = operation;
  if (call.getCallee())
    info.callee = getCalleeFromSymbol(*moduleSymbols, *call.getCallee());
  if (info.callee)
    info.lifetime = info.callee.getLifetimeAttr();
  info.args.append(call.getArgOperands().begin(), call.getArgOperands().end());
  info.parameterKinds = resolveCallParameterKinds(call, info.lifetime);
  info.mayThrow = !call.getNothrow() && !call.getNounwind();
  // CIRGen omits unwinding when exceptions are disabled. Honor that serialized
  // module fact after text/bytecode reload as well. Missing metadata remains
  // conservative; a noexcept function can still contain an internal catch.
  if (auto langOptions = theModule->getAttrOfType<LoweringLangOptionsAttr>(
          CIRDialect::getLoweringLangOptionsAttrName()))
    info.mayThrow &= langOptions.getExceptions();
  if (info.lifetime && info.lifetime.getIsMethod() &&
      !info.lifetime.getIsStatic() && !info.args.empty())
    info.receiver = resolveAddress(info.args.front(), state);
  // A call can modify previously escaped storage even without a pointer
  // argument at this call site. Without escape/effect proofs, forget stored
  // boolean constants on both normal and exceptional continuations. Already
  // evaluated SSA values (including argument values above) remain snapshots.
  clearStoredBooleanValues(state);
  if (auto modeled = modelLibraryCall(info, state))
    return std::move(*modeled);

  std::optional<PathState> exceptional;
  if (info.mayThrow)
    exceptional.emplace(state);
  Attribute special = info.callee ? info.callee.getFuncInfoAttr() : Attribute{};
  auto ctor = dyn_cast_if_present<CXXCtorAttr>(special);
  auto assignment = dyn_cast_if_present<CXXAssignAttr>(special);
  bool moveSpecial =
      (ctor && ctor.getCtorKind() == CtorKind::Move) ||
      (assignment && assignment.getAssignKind() == AssignKind::Move);
  bool copySpecial =
      (ctor && ctor.getCtorKind() == CtorKind::Copy) ||
      (assignment && assignment.getAssignKind() == AssignKind::Copy);
  // Copying through an uncertain receiver still reads the source. The
  // definite special-member path below and the generic record-argument path
  // cover their own categories; GSL owners need the same source validation.
  if (copySpecial && !info.receiver.isDefinite() && info.args.size() >= 2) {
    auto source = resolveAddress(info.args[1], state);
    for (ObjectId object : source.objects) {
      auto type = getObjectInfo(object);
      if (type && type->category == ObjectInfo::Owner &&
          !hasLibraryContents(object) &&
          state.pmap[object].count(State::getInvalid()))
        checkUse(state, ObjectTargets{{object}}, call.getLoc());
    }
  }
  bool handledSpecial = false;
  if ((ctor || assignment) && info.receiver.isDefinite() &&
      info.args.size() >= 2) {
    ObjectTargets source = resolveAddress(info.args[1], state);
    ObjectId destination = info.receiver.objects.front();
    auto destinationInfo = getObjectInfo(destination);
    if (destinationInfo && !source.objects.empty()) {
      bool sourceInvalid = llvm::any_of(source.objects, [&](ObjectId object) {
        return state.pmap[object].count(State::getInvalid());
      });
      if ((copySpecial && destinationInfo->category != ObjectInfo::Pointer &&
           sourceInvalid) ||
          (moveSpecial &&
           (destinationInfo->category != ObjectInfo::Pointer || sourceInvalid)))
        checkUse(state, source, call.getLoc(),
                 copySpecial ? Direct : UseAfterMove);
      // Snapshot the source before destination mutation, including self move.
      PSetType sourceSet;
      InvalidHist sourceHistory;
      std::optional<Location> nullOrigin;
      ValueSummary sourceValue;
      bool first = true;
      for (ObjectId object : source.objects) {
        llvm::set_union(sourceSet, state.pmap[object]);
        appendUnique(
            sourceHistory.entries,
            ArrayRef<InvalidHistEntry>(state.invalidHist[object].entries));
        if (!nullOrigin)
          nullOrigin = state.nullHist.lookup(object);
        auto value = state.storedValues.lookup(object);
        sourceValue = first ? value : joinValues(sourceValue, value);
        first = false;
      }
      if (assignment && destinationInfo->category == ObjectInfo::Owner)
        invalidateOwnerContents(state, info.receiver, call.getLoc());
      reinitializeObjects(state, info.receiver, call.getLoc());
      if (destinationInfo->category == ObjectInfo::Pointer) {
        state.pmap[destination] = std::move(sourceSet);
        state.invalidHist[destination] = std::move(sourceHistory);
        if (nullOrigin)
          state.nullHist[destination] = nullOrigin;
        else
          state.nullHist.erase(destination);
        storeSummary(state, info.receiver, sourceValue);
      }
      if (moveSpecial) {
        markMoved(state, source, call.getLoc());
        if (exceptional)
          markMoved(*exceptional, source, call.getLoc());
      }
      // A custom constructor still passes its arguments. Copying an owner's
      // pset above makes a Pointer borrow it; skip that owner's invalidation.
      if (ctor && ctor.getCtorKind() == CtorKind::Custom) {
        StateGuard guard(*this, state);
        checkForOwnerAndPointerArguments(
            call, /*firstArgIdx=*/1, info.parameterKinds,
            isCtorInitPointerFromOwner(call) ? std::optional<unsigned>(1)
                                             : std::nullopt);
      }
      handledSpecial = true;
    }
  }
  if (!handledSpecial) {
    StateGuard guard(*this, state);
    checkCall(call, info.parameterKinds);
    applyReceiverEffects(info, state, special);
  }
  // Receiver candidates share one static type, hence one category. A
  // may-alias receiver is reinitialized weakly.
  if ((ctor || assignment) && !handledSpecial &&
      llvm::any_of(info.receiver.objects, [&](ObjectId object) {
        auto type = getObjectInfo(object);
        return type && type->category != ObjectInfo::Pointer;
      }))
    reinitializeObjects(state, info.receiver, call.getLoc());

  if (exceptional && ctor && ctor.getCtorKind() == CtorKind::Custom) {
    // Argument mutation can precede a throw even though construction of the
    // receiver never finishes. Apply only the common argument effects here;
    // copying the normal state would incorrectly initialize the receiver.
    // Witnesses from both paths are merged by operation, object and style.
    StateGuard guard(*this, *exceptional);
    checkForOwnerAndPointerArguments(
        call, /*firstArgIdx=*/1, info.parameterKinds,
        isCtorInitPointerFromOwner(call) ? std::optional<unsigned>(1)
                                         : std::nullopt);
  }
  applyArgumentEffects(
      info, state, exceptional ? &*exceptional : nullptr, [&](unsigned index) {
        // Copy/move special members already handled their source operand.
        // A custom constructor still validates and consumes that argument.
        return handledSpecial && index == 1 &&
               (!ctor || ctor.getCtorKind() != CtorKind::Custom);
      });
  if (info.lifetime && info.lifetime.getIsMethod() && !ctor && !assignment &&
      !dyn_cast_if_present<CXXDtorAttr>(special)) {
    for (ObjectId object : info.receiver.objects) {
      auto type = getObjectInfo(object);
      if (type && !tasks.count(object) &&
          (type->category == ObjectInfo::Record ||
           type->category == ObjectInfo::Aggregate) &&
          state.pmap[object].count(State::getInvalid()))
        checkUse(state, ObjectTargets{{object}}, call.getLoc());
    }
  }
  synchronizeSummaries(state);
  for (Value result : call->getResults()) {
    ValueSummary summary = summarizeValue(result, state);
    if (!summary.pointer && isPointerType(result.getType()))
      summary.pointer.emplace();
    if (summary.pointer && summary.pointer->targets.objects.empty())
      summary.pointer->targets.objects.push_back(result);
    if (info.lifetime && info.lifetime.getReturnsNonnull() && summary.pointer) {
      summary.pointer->targets.unknown = false;
      summary.pointer->mayBeNonNull = true;
      summary.pointer->unknownNullness = false;
      summary.unknown = false;
    }
    setValueSummary(state, result, std::move(summary));
  }
  FlowResult result{std::move(state)};
  if (exceptional) {
    // A successful assignment/reinitialization is not assumed on an
    // exceptional path; operand evaluation and possible source consumption
    // already occurred. Other calls retain both possible effect states.
    if (!ctor && !assignment)
      joinStateInto(*exceptional, *result.normal);
    result.exceptions.push_back({std::move(*exceptional), {}});
  }
  return result;
}

LifetimeCheckPass::FlowResult
LifetimeCheckPass::leaveScope(FlowResult flow, ArrayRef<ObjectId> locals,
                                  Location loc) {
  // Both pre-indexed scopes and structured regions can collect locals through
  // hash sets. Apply one stable order to every exit, including loop cleanups.
  SmallVector<ObjectId, 8> ordered;
  if (locals.size() > 1) {
    ordered.append(locals.begin(), locals.end());
    llvm::sort(ordered, [&](Value a, Value b) {
      return getValueOrder(a) < getValueOrder(b);
    });
    locals = ordered;
  }
  auto end = [&](PathState &state) {
    StateGuard guard(*this, state);
    for (ObjectId object : locals) {
      if (!llvm::is_contained(state.liveObjects, object))
        continue;
      // KILL also invalidates captured SSA and stored pointer payloads that
      // target or borrow from the object, with the same end-of-scope history.
      kill(State::getLocalValue(object), InvalidStyle::EndOfScope, loc);
    }
    if (currScope)
      for (auto returned : currScope->localRetLambdas)
        checkPointerDeref(returned.first, returned.second, RetLambda);
  };
  if (flow.normal)
    end(*flow.normal);
  for (auto &state : flow.breaks)
    end(state);
  for (auto &state : flow.continues)
    end(state);
  for (auto &state : flow.returns)
    end(state);
  for (auto &exception : flow.exceptions)
    end(exception.state);
  for (auto &jump : flow.gotos)
    end(jump.state);
  return flow;
}

LifetimeCheckPass::FlowResult
LifetimeCheckPass::leaveOperationScope(Operation *scope, FlowResult flow) {
  if (flow.normal && scope->getNumResults() && scope->getNumRegions()) {
    Region &body = scope->getRegion(0);
    if (!body.empty() && !body.back().empty())
      if (auto yield = dyn_cast<YieldOp>(body.back().back()))
        flow.normal = finishRegionOperation(scope, yield.getArgs(),
                                            std::move(*flow.normal));
  }
  if (auto cleanup = dyn_cast<CleanupScopeOp>(scope)) {
    FlowResult cleaned;
    auto run = [&](PathState state, unsigned kind, Type thrownType = {},
                   FlatSymbolRefAttr typeInfo = {}, StringAttr label = {}) {
      bool unwind = kind == 4;
      bool applies = unwind ? cleanup.getCleanupKindAttr().isEH()
                            : cleanup.getCleanupKindAttr().isNormal();
      FlowResult result =
          applies ? analyzeRegion(cleanup.getCleanupRegion(), std::move(state))
                  : FlowResult{std::move(state)};
      if (result.normal) {
        switch (kind) {
        case 0:
          break;
        case 1:
          result.breaks.push_back(std::move(*result.normal));
          result.normal.reset();
          break;
        case 2:
          result.continues.push_back(std::move(*result.normal));
          result.normal.reset();
          break;
        case 3:
          result.returns.push_back(std::move(*result.normal));
          result.normal.reset();
          break;
        case 4:
          result.exceptions.push_back(
              {std::move(*result.normal), thrownType, typeInfo});
          result.normal.reset();
          break;
        case 5:
          result.gotos.push_back({std::move(*result.normal), label});
          result.normal.reset();
          break;
        }
      }
      mergeFlow(cleaned, std::move(result));
    };
    if (flow.normal)
      run(std::move(*flow.normal), 0);
    for (auto &state : flow.breaks)
      run(std::move(state), 1);
    for (auto &state : flow.continues)
      run(std::move(state), 2);
    for (auto &state : flow.returns)
      run(std::move(state), 3);
    for (auto &exception : flow.exceptions)
      run(std::move(exception.state), 4, exception.thrownType,
          exception.typeInfo);
    for (auto &jump : flow.gotos)
      run(std::move(jump.state), 5, {}, {}, jump.label);
    flow = std::move(cleaned);
  }
  ArrayRef<ObjectId> locals;
  if (auto found = scopeLocals.find(scope); found != scopeLocals.end())
    locals = found->second;
  return leaveScope(std::move(flow), locals, getEndLocForHist(scope));
}

LifetimeCheckPass::FlowResult LifetimeCheckPass::analyzeBlock(Block &block,
                                                              PathState state) {
  FlowResult result{std::move(state)};
  for (Operation &operation : block) {
    if (!result.normal)
      break;
    FlowResult current =
        transferOperation(&operation, std::move(*result.normal));
    result.normal.reset();
    mergeFlow(result, std::move(current));
    if (operation.hasTrait<OpTrait::IsTerminator>())
      break;
  }
  return result;
}

LifetimeCheckPass::FlowResult
LifetimeCheckPass::analyzeRegion(Region &region, PathState state, bool scoped) {
  // Full expressions can contain thousands of nested temporary cleanups.
  // Continue the same CIR analysis on a fresh stack when necessary; the
  // enclosing invocation waits, so path state and diagnostics remain serial.
  std::optional<FlowResult> result;
  bool canContinue = true;
  clang::runWithSufficientStackSpace(
      [&] {
#if !LLVM_ENABLE_THREADS
        canContinue = false;
#endif
      },
      [&] {
        if (canContinue)
          result = analyzeRegionImpl(region, std::move(state), scoped);
      });
  // A non-threaded build cannot extend the stack. Also do not silently accept
  // an empty result if an enclosing crash-recovery context interrupted it.
  if (!result) {
    mlir::emitError(region.getLoc(),
                    "CIR lifetime analysis could not continue on a sufficient "
                    "stack");
    signalPassFailure();
    return {};
  }
  return std::move(*result);
}

LifetimeCheckPass::FlowResult
LifetimeCheckPass::analyzeRegionImpl(Region &region, PathState state,
                                     bool scoped) {
  if (region.empty())
    return {std::move(state)};
  LexicalScopeContext local{&region};
  auto *previousScope = currScope;
  if (scoped)
    currScope = &local;
  FlowResult result;
  // A label-free block without incoming or outgoing edges cannot be
  // re-entered within this region. Consume its state directly: retaining an
  // entry snapshot at every nested cleanup scope copies growing state prefixes.
  // Symbolic gotos without a local label still propagate to the enclosing
  // region, and nested regions keep their own control-flow handling.
  if (llvm::hasSingleElement(region) && region.front().hasNoPredecessors() &&
      llvm::none_of(region.front(), [](Operation &operation) {
        return isa<LabelOp>(operation) || operation.getNumSuccessors() != 0;
      })) {
    result = analyzeBlock(region.front(), std::move(state));
  } else {
    // CIR is normally structured here. Follow explicit block edges as well:
    // unreachable continuation blocks after return/throw are not executable.
    DenseMap<Block *, PathState> entries;
    SmallVector<Block *> worklist;
    auto enqueue = [&](Block *successor, ValueRange operands, PathState next) {
      for (auto pair : llvm::zip(successor->getArguments(), operands))
        setValueSummary(next, std::get<0>(pair),
                        summarizeValue(std::get<1>(pair), next));
      auto found = entries.find(successor);
      if (found == entries.end()) {
        entries[successor] = std::move(next);
        worklist.push_back(successor);
      } else {
        auto joined = joinStates({&found->second, &next});
        if (!equivalentStates(found->second, joined)) {
          found->second = std::move(joined);
          worklist.push_back(successor);
        }
      }
    };
    enqueue(&region.front(), {}, std::move(state));
    while (!worklist.empty()) {
      Block *block = worklist.pop_back_val();
      FlowResult blockFlow = analyzeBlock(*block, entries[block]);
      SmallVector<GotoState, 0> outgoing;
      for (auto &jump : blockFlow.gotos) {
        auto target = labelTargets->find(jump.label);
        if (target != labelTargets->end() &&
            target->second->getParent() == &region)
          enqueue(target->second, {}, std::move(jump.state));
        else
          outgoing.push_back(std::move(jump));
      }
      blockFlow.gotos = std::move(outgoing);
      if (blockFlow.normal && !block->empty() &&
          block->back().getNumSuccessors()) {
        if (auto branch = dyn_cast<BrOp>(block->getTerminator()))
          enqueue(branch.getDest(), branch.getDestOperands(),
                  *blockFlow.normal);
        else {
          mlir::emitError(block->getTerminator()->getLoc(),
                          "unsupported CFG terminator in lifetime analysis");
          signalPassFailure();
        }
        blockFlow.normal.reset();
      }
      mergeFlow(result, std::move(blockFlow));
    }
  }
  if (scoped) {
    SmallVector<ObjectId> locals(local.localValues.begin(),
                                 local.localValues.end());
    result = leaveScope(std::move(result), locals,
                        getEndLocForHist(region.getParentOp()));
    currScope = previousScope;
  }
  return result;
}

LifetimeCheckPass::FlowResult
LifetimeCheckPass::analyzeLoop(LoopOpInterface loop, PathState input) {
  PathState head = input;
  FlowResult exits;
  bool bodyFirst = &loop.getEntry() == &loop.getBody();
  auto cleanup = [&](FlowResult flow) {
    auto *region = loop.maybeGetCleanup();
    auto kind = loop.maybeGetCleanupKind();
    if (!region || !kind)
      return flow;
    // Loop cleanup applies on every iteration exit, including continue, break,
    // false condition, and exceptional unwinding.
    FlowResult result;
    auto run = [&](PathState state, unsigned exit, Type thrown = {},
                   FlatSymbolRefAttr typeInfo = {}, StringAttr label = {}) {
      bool applies = exit == 4 ? kind.isEH() : kind.isNormal();
      FlowResult cleaned = applies ? analyzeRegion(*region, std::move(state))
                                   : FlowResult{std::move(state)};
      if (cleaned.normal && exit) {
        if (exit == 1)
          cleaned.breaks.push_back(std::move(*cleaned.normal));
        if (exit == 2)
          cleaned.continues.push_back(std::move(*cleaned.normal));
        if (exit == 3)
          cleaned.returns.push_back(std::move(*cleaned.normal));
        if (exit == 4)
          cleaned.exceptions.push_back(
              {std::move(*cleaned.normal), thrown, typeInfo});
        if (exit == 5)
          cleaned.gotos.push_back({std::move(*cleaned.normal), label});
        cleaned.normal.reset();
      }
      mergeFlow(result, std::move(cleaned));
    };
    if (flow.normal)
      run(std::move(*flow.normal), 0);
    for (auto &state : flow.breaks)
      run(std::move(state), 1);
    for (auto &state : flow.continues)
      run(std::move(state), 2);
    for (auto &state : flow.returns)
      run(std::move(state), 3);
    for (auto &exception : flow.exceptions)
      run(std::move(exception.state), 4, exception.thrownType,
          exception.typeInfo);
    for (auto &jump : flow.gotos)
      run(std::move(jump.state), 5, {}, {}, jump.label);
    return result;
  };
  for (unsigned iteration = 0;; ++iteration) {
    if (iteration == 4096) {
      mlir::emitError(loop->getLoc(),
                      "lifetime loop analysis did not converge");
      signalPassFailure();
      return exits;
    }
    FlowResult current{head};
    auto condition = [&](FlowResult flow) {
      if (!flow.normal)
        return flow;
      auto checked = analyzeRegion(loop.getCond(), std::move(*flow.normal));
      flow.normal.reset();
      if (checked.normal) {
        Value value;
        if (!loop.getCond().empty())
          if (auto cond =
                  dyn_cast<ConditionOp>(loop.getCond().back().getTerminator()))
            value = cond.getCondition();
        auto known =
            value ? evaluateCondition(value, *checked.normal) : std::nullopt;
        if (!known || !*known) {
          PathState falseState =
              value ? refineCondition(value, false, *checked.normal)
                    : *checked.normal;
          FlowResult falseExit = cleanup(FlowResult{std::move(falseState)});
          mergeFlow(exits, std::move(falseExit));
        }
        if (known && !*known)
          checked.normal.reset();
        else if (value)
          checked.normal =
              refineCondition(value, true, std::move(*checked.normal));
      }
      mergeFlow(flow, std::move(checked));
      return flow;
    };
    if (!bodyFirst)
      current = condition(std::move(current));
    if (current.normal) {
      auto body = analyzeRegion(loop.getBody(), std::move(*current.normal));
      current.normal.reset();
      mergeFlow(current, std::move(body));
    }
    // Continue reaches the increment (and the condition in a do-while), while
    // break and return do not. Always evaluate the first increment.
    for (auto &continued : current.continues) {
      if (current.normal)
        joinStateInto(*current.normal, continued);
      else
        current.normal = std::move(continued);
    }
    current.continues.clear();
    if (current.normal && loop.maybeGetStep()) {
      auto step =
          analyzeRegion(*loop.maybeGetStep(), std::move(*current.normal));
      current.normal.reset();
      mergeFlow(current, std::move(step));
    }
    if (bodyFirst)
      current = condition(std::move(current));
    current = cleanup(std::move(current));
    auto backedge = std::move(current.normal);
    current.normal.reset();
    for (auto &broken : current.breaks)
      mergeFlow(exits, FlowResult{std::move(broken)});
    current.breaks.clear();
    mergeFlow(exits, std::move(current));
    if (!backedge)
      break;
    PathState next = joinStates({&input, &head, &*backedge});
    if (equivalentStates(head, next))
      break;
    head = std::move(next);
  }
  return exits;
}

LifetimeCheckPass::FlowResult
LifetimeCheckPass::transferOperation(Operation *operation, PathState state) {
  llvm::SaveAndRestore<Operation *> operationGuard(activeOperation, operation);
  // These operations can form long, finite chains (for example, else-if
  // dispatch). Keep leaf value summaries and other transfer scratch in an
  // outlined fallback so that every recursive level does not reserve them.
  if (auto ifOp = dyn_cast<IfOp>(operation)) {
    FlowResult result;
    auto known = evaluateCondition(ifOp.getCondition(), state);
    if (!known || *known)
      mergeFlow(result,
                analyzeRegion(ifOp.getThenRegion(),
                              refineCondition(ifOp.getCondition(), true, state),
                              true));
    if (!known || !*known)
      mergeFlow(result, analyzeRegion(ifOp.getElseRegion(),
                                      refineCondition(ifOp.getCondition(),
                                                      false, std::move(state)),
                                      true));
    return result;
  } else if (auto ternary = dyn_cast<TernaryOp>(operation)) {
    FlowResult result;
    auto known = evaluateCondition(ternary.getCond(), state);
    if (!known || *known)
      mergeFlow(result,
                analyzeRegion(ternary.getTrueRegion(),
                              refineCondition(ternary.getCond(), true, state)));
    if (!known || !*known)
      mergeFlow(result, analyzeRegion(ternary.getFalseRegion(),
                                      refineCondition(ternary.getCond(), false,
                                                      std::move(state))));
    return result;
  } else if (isa<ScopeOp, CleanupScopeOp>(operation)) {
    LexicalScopeContext local{operation};
    llvm::SaveAndRestore<LexicalScopeContext *> scopeGuard(currScope, &local);
    auto flow = analyzeRegion(operation->getRegion(0), std::move(state));
    return leaveOperationScope(operation, std::move(flow));
  }
  return transferOtherOperation(operation, std::move(state));
}

LifetimeCheckPass::FlowResult
LifetimeCheckPass::transferOtherOperation(Operation *operation, PathState state) {
  if (auto alloca = dyn_cast<AllocaOp>(operation)) {
    initializeObject(state, alloca.getAddr(), alloca.getLoc());
  } else if (auto load = dyn_cast<LoadOp>(operation)) {
    transferLoad(load, state);
  } else if (auto store = dyn_cast<StoreOp>(operation)) {
    transferStore(store, state);
  } else if (auto copy = dyn_cast<CopyOp>(operation)) {
    transferCopy(copy, state);
  } else if (auto call = dyn_cast<CallOp>(operation)) {
    auto result = analyzeCall(operation, std::move(state));
    bool noReturn = call->hasAttr(CIRDialect::getNoReturnAttrName());
    if (!noReturn && call.getCallee())
      if (auto callee = getCalleeFromSymbol(*moduleSymbols, *call.getCallee()))
        noReturn = callee->hasAttr(CIRDialect::getNoReturnAttrName());
    // CIR cleanup regions may end in yield after a noreturn destructor.
    // The call still has argument/diagnostic effects, and can throw, but its
    // normal continuation cannot reach another statement or loop iteration.
    if (noReturn)
      result.normal.reset();
    return result;
  } else if (auto cast = dyn_cast<CastOp>(operation)) {
    // Preserve identity through address casts. Arithmetic/numeric conversions
    // do not make the source of an arbitrary expression a consumed object.
    if (isa<cir::PointerType>(cast.getSrc().getType()) &&
        isa<cir::PointerType>(cast.getResult().getType())) {
      setValueSummary(state, cast.getResult(),
                      summarizeValue(cast.getSrc(), state));
      copyConditionalPointerSnapshots(state, cast.getSrc(), false,
                                      cast.getResult(), false);
    }
  } else if (auto negate = dyn_cast<NotOp>(operation)) {
    if (isa<cir::BoolType>(negate.getType())) {
      ValueSummary summary;
      summary.booleanValue = evaluateCondition(negate.getInput(), state);
      if (summary.booleanValue)
        summary.booleanValue = !*summary.booleanValue;
      summary.predicate = summarizeValue(negate.getInput(), state).predicate;
      if (summary.predicate) {
        auto predicate = *summary.predicate;
        predicate.trueMeansPresent = !predicate.trueMeansPresent;
        summary.predicate =
            std::make_shared<const ValuePredicate>(std::move(predicate));
      }
      setValueSummary(state, negate.getResult(), std::move(summary));
    }
  } else if (auto member = dyn_cast<GetMemberOp>(operation)) {
    auto canonical = canonicalObjects.find(member.getResult());
    if (canonical != canonicalObjects.end()) {
      ValueSummary address;
      address.pointer.emplace();
      address.pointer->targets.objects.push_back(canonical->second);
      address.pointer->mayBeNonNull = true;
      setValueSummary(state, member.getResult(), std::move(address));
    }
  } else if (auto stride = dyn_cast<PtrStrideOp>(operation)) {
    setValueSummary(state, stride.getResult(),
                    summarizeValue(stride.getBase(), state));
  } else if (auto element = dyn_cast<GetElementOp>(operation)) {
    setValueSummary(state, element.getResult(),
                    summarizeValue(element.getBase(), state));
  } else if (auto yield = dyn_cast<YieldOp>(operation)) {
    state = finishRegionOperation(operation->getParentOp(), yield.getArgs(),
                                  std::move(state));
  } else if (auto select = dyn_cast<SelectOp>(operation)) {
    auto known = evaluateCondition(select.getCondition(), state);
    ValueSummary summary;
    if (known)
      summary = summarizeValue(
          *known ? select.getTrueValue() : select.getFalseValue(), state);
    else
      summary = joinValues(summarizeValue(select.getTrueValue(), state),
                           summarizeValue(select.getFalseValue(), state));
    setValueSummary(state, select.getResult(), std::move(summary));
  } else if (auto loop = dyn_cast<LoopOpInterface>(operation)) {
    return analyzeLoop(loop, std::move(state));
  } else if (auto switchOp = dyn_cast<SwitchOp>(operation)) {
    SmallVector<CaseOp> cases;
    if (!switchOp.isSimpleForm(cases))
      return {std::move(state)};
    FlowResult result;
    if (llvm::none_of(
            cases, [](CaseOp c) { return c.getKind() == CaseOpKind::Default; }))
      result.normal = state;
    for (unsigned start = 0; start < cases.size(); ++start) {
      // An empty entry executes the same suffix from the same incoming state
      // as the following case. Keep the final entry, which can be an identity
      // exit, and determine default coverage from the original cases above.
      Region &entry = cases[start].getRegion();
      if (start + 1 < cases.size() && llvm::hasSingleElement(entry) &&
          entry.front().getNumArguments() == 0 &&
          llvm::hasSingleElement(entry.front())) {
        auto yield = dyn_cast<YieldOp>(entry.front().front());
        if (yield && yield.getArgs().empty())
          continue;
      }
      FlowResult path{state};
      for (unsigned index = start; index < cases.size() && path.normal;
           ++index) {
        auto next =
            analyzeRegion(cases[index].getRegion(), std::move(*path.normal));
        path.normal.reset();
        mergeFlow(path, std::move(next));
      }
      for (auto &broken : path.breaks)
        mergeFlow(result, FlowResult{std::move(broken)});
      path.breaks.clear();
      mergeFlow(result, std::move(path));
    }
    return result;
  } else if (auto tryOp = dyn_cast<TryOp>(operation)) {
    auto result = analyzeRegion(tryOp.getTryRegion(), std::move(state), true);
    auto exceptions = std::move(result.exceptions);
    result.exceptions.clear();
    for (auto &exception : exceptions) {
      bool caught = false;
      auto kinds = tryOp.getHandlerTypes();
      for (unsigned index = 0; kinds && index < kinds.size(); ++index) {
        Attribute kind = kinds[index];
        bool all = isa<CatchAllAttr>(kind);
        bool unwind = isa<UnwindAttr, EhFilterAttr, EhUnexpectedAttr>(kind);
        auto typed = dyn_cast<GlobalViewAttr>(kind);
        bool exact = typed && exception.typeInfo &&
                     typed.getSymbol() == exception.typeInfo;
        // Distinct class RTTI does not prove a mismatch: a base-class handler
        // can catch a derived object. Pointer exceptions also permit standard
        // conversions. Without a serialized class hierarchy, retain both the
        // matching-handler and later-handler alternatives for these cases.
        bool scalar = exception.thrownType &&
                      isa<cir::IntType, cir::BoolType, cir::FPTypeInterface>(
                          exception.thrownType);
        if (typed && exception.typeInfo && !exact && scalar)
          continue;
        llvm::SaveAndRestore<FlatSymbolRefAttr> exceptionGuard(
            currentExceptionType, exception.typeInfo);
        llvm::SaveAndRestore<Type> exceptionTypeGuard(currentExceptionIRType,
                                                      exception.thrownType);
        auto handler = analyzeRegion(tryOp.getHandlerRegions()[index],
                                     exception.state, true);
        if (unwind && handler.normal) {
          handler.exceptions.push_back({std::move(*handler.normal),
                                        exception.thrownType,
                                        exception.typeInfo});
          handler.normal.reset();
        }
        mergeFlow(result, std::move(handler));
        if (all || exact || unwind) {
          caught = true;
          break;
        }
      }
      if (!caught)
        result.exceptions.push_back(std::move(exception));
    }
    return result;
  } else if (auto throwOp = dyn_cast<ThrowOp>(operation)) {
    FlowResult result;
    Type thrownType = currentExceptionIRType;
    if (auto exception = throwOp.getExceptionPtr())
      if (auto pointer = dyn_cast<cir::PointerType>(exception.getType()))
        thrownType = pointer.getPointee();
    result.exceptions.push_back({std::move(state), thrownType,
                                 throwOp.getTypeInfoAttr()
                                     ? throwOp.getTypeInfoAttr()
                                     : currentExceptionType});
    return result;
  } else if (isa<ResumeOp>(operation)) {
    FlowResult result;
    result.exceptions.push_back({std::move(state), {}});
    return result;
  } else if (auto coroutine = dyn_cast<CoroutineOp>(operation)) {
    FlowResult result{std::move(state)};
    for (Region *region :
         {&coroutine.getInitialSuspend(), &coroutine.getBody(),
          &coroutine.getFinalSuspend(), &coroutine.getExit()}) {
      if (!result.normal)
        break;
      auto next = analyzeRegion(*region, std::move(*result.normal));
      result.normal.reset();
      mergeFlow(result, std::move(next));
    }
    return result;
  } else if (auto await = dyn_cast<AwaitOp>(operation)) {
    FlowResult result;
    for (Region &region : await->getRegions())
      mergeFlow(result, analyzeRegion(region, state));
    return result;
  } else if (auto ret = dyn_cast<ReturnOp>(operation)) {
    {
      StateGuard guard(*this, state);
      checkReturn(ret);
    }
    FlowResult result;
    result.returns.push_back(std::move(state));
    return result;
  } else if (auto jump = dyn_cast<GotoOp>(operation)) {
    FlowResult result;
    result.gotos.push_back({std::move(state), jump.getLabelAttr()});
    return result;
  } else if (isa<BreakOp>(operation)) {
    FlowResult result;
    result.breaks.push_back(std::move(state));
    return result;
  } else if (isa<ContinueOp>(operation)) {
    FlowResult result;
    result.continues.push_back(std::move(state));
    return result;
  } else if (isa<UnreachableOp>(operation)) {
    return {};
  } else if (isa<ConditionOp, BrOp, LabelOp>(operation)) {
    // These operations carry control without mutating storage, even though
    // they do not all expose a memory-effect interface.
    return {std::move(state)};
  } else if (auto effects = dyn_cast<MemoryEffectOpInterface>(operation)) {
    SmallVector<MemoryEffects::EffectInstance> instances;
    effects.getEffects(instances);
    if (llvm::any_of(instances, [](const auto &effect) {
          return isa<MemoryEffects::Write, MemoryEffects::Free>(
              effect.getEffect());
        }))
      clearStoredBooleanValues(state);
  } else if (!mlir::isMemoryEffectFree(operation)) {
    // An unmodeled operation without a memory-effect contract cannot preserve
    // a must fact about mutable storage. This does not reset lifetime facts.
    clearStoredBooleanValues(state);
  }
  return {std::move(state)};
}

void LifetimeCheckPass::recordWitness(PathState &state,
                                      DiagnosticWitness witness) {
  auto record = [&](auto &witnesses) {
    auto found = llvm::find_if(witnesses, [&](const auto &other) {
      return witness.operation == other.operation &&
             witness.object == other.object && witness.style == other.style;
    });
    if (found == witnesses.end()) {
      witnesses.push_back(witness);
      return;
    }
    llvm::set_union(found->pset, witness.pset);
    appendUnique(found->history.entries,
                 ArrayRef<InvalidHistEntry>(witness.history.entries));
    if (!found->nullOrigin)
      found->nullOrigin = witness.nullOrigin;
  };
  record(state.diagnostics);
  record(functionWitnesses);
}

void LifetimeCheckPass::emitWitnesses(const FlowResult &flow) {
  SmallVector<const PathState *, 4> states;
  if (flow.normal)
    states.push_back(&*flow.normal);
  for (const PathState &state : flow.breaks)
    states.push_back(&state);
  for (const PathState &state : flow.continues)
    states.push_back(&state);
  for (const PathState &state : flow.returns)
    states.push_back(&state);
  for (const auto &exception : flow.exceptions)
    states.push_back(&exception.state);
  PathState observed;
  observed.diagnostics.assign(functionWitnesses.begin(),
                              functionWitnesses.end());
  states.push_back(&observed);
  PathState joined = joinStates(states);
  emittingWitnesses = true;
  for (const auto &witness : joined.diagnostics) {
    PathState state;
    state.pmap[witness.object] = witness.pset;
    state.invalidHist[witness.object] = witness.history;
    if (witness.nullOrigin)
      state.nullHist[witness.object] = witness.nullOrigin;
    StateGuard guard(*this, state);
    auto generations = owners;
    for (auto generation : witness.ownerGenerations)
      owners[generation.first] = generation.second;
    checkPointerDeref(witness.object, witness.location, witness.style);
    owners = std::move(generations);
  }
  emittingWitnesses = false;
}

#include "LifetimeCheckLibrary.inc"

void LifetimeCheckPass::runOnOperation() {
  // cir-opt does not initialize Clang's stack accounting on its own.
  clang::noteBottomOfStack();
  opts.parseOptions(*this);
  Operation *op = getOperation();
  if (opts.error) {
    mlir::emitError(op->getLoc()) << *opts.error;
    return signalPassFailure();
  }
  theModule = cast<ModuleOp>(op);
  SmallVector<FuncOp> functions(theModule.getOps<FuncOp>());
  std::vector<FunctionLabelMap> labels(functions.size());

  mlir::SymbolTable symbols(theModule);
  llvm::SaveAndRestore<mlir::SymbolTable *> symbolGuard(moduleSymbols,
                                                        &symbols);
  bool validationFailed = false;
  for (auto [index, function] : llvm::enumerate(functions)) {
    if (failed(validateLifetimeControlFlow(function, labels[index]))) {
      validationFailed = true;
      continue;
    }
    llvm::SaveAndRestore<const FunctionLabelMap *> labelGuard(labelTargets,
                                                            &labels[index]);
    checkFunc(function);
  }
  if (validationFailed)
    signalPassFailure();
}

std::unique_ptr<Pass> mlir::createLifetimeCheckPass() {
  return std::make_unique<LifetimeCheckPass>();
}

std::unique_ptr<Pass>
mlir::createLifetimeCheckPass(ArrayRef<llvm::StringRef> remark,
                              ArrayRef<llvm::StringRef> hist,
                              unsigned hist_limit) {
  auto lifetime = std::make_unique<LifetimeCheckPass>();
  lifetime->opts.parseOptions(remark, hist, hist_limit);
  return lifetime;
}

//===----------------------------------------------------------------------===//
// Dump & print helpers
//===----------------------------------------------------------------------===//

void LifetimeCheckPass::LexicalScopeContext::dumpLocalValues() {
  llvm::errs() << "Local values: { ";
  for (auto value : localValues) {
    llvm::errs() << getVarNameFromValue(value);
    llvm::errs() << ", ";
  }
  llvm::errs() << "}\n";
}

void LifetimeCheckPass::State::dump(llvm::raw_ostream &OS, int ownedGen) {
  switch (val.getInt()) {
  case Invalid:
    OS << "invalid";
    break;
  case NullPtr:
    OS << "nullptr";
    break;
  case Global:
    OS << "global";
    break;
  case LocalValue:
    OS << getVarNameFromValue(val.getPointer());
    break;
  case OwnedBy:
    ownedGen++; // Start from 1.
    OS << getVarNameFromValue(val.getPointer()) << "__" << ownedGen << "'";
    break;
  default:
    llvm_unreachable("Not handled");
  }
}

void LifetimeCheckPass::printPset(PSetType &pset, llvm::raw_ostream &OS) {
  OS << "{ ";
  auto size = pset.size();
  for (auto s : pset) {
    int ownerGen = 0;
    if (s.isOwnedBy())
      ownerGen = owners[s.getData()];
    s.dump(OS, ownerGen);
    size--;
    if (size > 0)
      OS << ", ";
  }
  OS << " }";
}

void LifetimeCheckPass::dumpCurrentPmap() { dumpPmap(*currPmap); }

void LifetimeCheckPass::dumpPmap(PMapType &pmap) {
  llvm::errs() << "pmap {\n";
  int entry = 0;
  for (auto &mapEntry : pmap) {
    llvm::errs() << "  " << entry << ": " << getVarNameFromValue(mapEntry.first)
                 << "  " << "=> ";
    printPset(mapEntry.second);
    llvm::errs() << "\n";
    entry++;
  }
  llvm::errs() << "}\n";
}

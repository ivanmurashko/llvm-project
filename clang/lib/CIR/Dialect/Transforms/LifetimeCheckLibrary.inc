//===- LifetimeCheckLibrary.inc - Library lifetime contracts -------*- C++
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Included after LifetimeCheckPass's object and path-state helpers. Library
// contracts operate on those same states; they do not maintain another alias
// analysis or recover source declarations from symbol spellings.
//===----------------------------------------------------------------------===//

static LifetimeLibraryKind getLibraryKind(Type type, ModuleOp module) {
  if (auto pointer = dyn_cast<PointerType>(type))
    type = pointer.getPointee();
  auto record = dyn_cast<RecordType>(type);
  auto facts =
      record ? getLifetimeRecord(record, module) : LifetimeRecordAttr{};
  return facts ? facts.getLibraryKind() : LifetimeLibraryKind::None;
}

// An empty optional or owner is initialized, but a contents access has an
// additional precondition. Queue the precondition witness without changing
// the initialized object's state or inventing a move event.
static void checkLibraryContents(
    LifetimeCheckPass &pass, LifetimeCheckPass::PathState &state,
    const LifetimeCheckPass::ObjectTargets &targets, Location location) {
  auto occupancy = pass.getOccupancy(state, targets);
  if (!occupancy.mayBeEmpty) {
    pass.checkUse(state, targets, location);
    return;
  }
  auto diagnosticState = state;
  size_t previousDiagnostics = diagnosticState.diagnostics.size();
  for (auto object : targets.objects) {
    // Existing move/null provenance already explains an invalid contents
    // access. Do not invent a second empty-state cause for the same event.
    if (diagnosticState.pmap[object].count(
            LifetimeCheckPass::State::getInvalid()) ||
        diagnosticState.pmap[object].count(
            LifetimeCheckPass::State::getNullPtr()))
      continue;
    diagnosticState.pmap[object].insert(LifetimeCheckPass::State::getNullPtr());
    if (!occupancy.emptyOrigins.empty())
      diagnosticState.nullHist[object] = occupancy.emptyOrigins.front();
  }
  pass.checkUse(diagnosticState, targets, location);
  state.diagnostics.append(diagnosticState.diagnostics.begin() +
                               previousDiagnostics,
                           diagnosticState.diagnostics.end());
}

std::optional<LifetimeCheckPass::FlowResult>
LifetimeCheckPass::modelLibraryCall(const CallInfo &call, PathState &state) {
  using Kind = LifetimeLibraryKind;
  using Effect = LifetimeLibraryFunction;
  if (!call.lifetime)
    return std::nullopt;
  const Effect effect = call.lifetime.getLibraryFunction();
  if (effect == Effect::TupleGet ||
      (effect == Effect::None && !call.lifetime.getReinitializes()))
    return std::nullopt;

  // Returning nullopt above must leave the caller's state untouched. Once
  // recognized, this model consumes state through finish().
  const Location location = call.op->getLoc();
  std::optional<PathState> before;
  if (call.mayThrow)
    before.emplace(state);
  FlowResult result;
  auto finish = [&](PathState normal, bool mayConsumeOnThrow = false) {
    if (before) {
      // Successful replacement is not assumed on an exceptional edge. A
      // throwing move may nevertheless have consumed part of its source.
      if (mayConsumeOnThrow)
        joinStateInto(*before, normal);
      result.exceptions.push_back({std::move(*before), Type{}});
    }
    result.normal = std::move(normal);
    return std::move(result);
  };
  auto argument = [&](unsigned parameter) -> Value {
    if (!call.lifetime.hasParameterInfo() ||
        parameter >= call.lifetime.getNumParams())
      return {};
    unsigned index = call.lifetime.getParamOperandIndex(parameter);
    return index < call.args.size() ? call.args[index] : Value{};
  };
  auto widenUnknownContents = [&](ValueSummary &summary,
                                  auto &widenUnknownContents) -> void {
    summary.unknown = true;
    if (summary.pointer) {
      summary.pointer->targets.unknown = true;
      summary.pointer->unknownNullness = true;
    }
    // An empty target slot paired with a value snapshot denotes a value
    // element, not an unknown reference. Preserve that discriminator while
    // retaining all known aliases and recursively widening value payloads.
    for (auto [index, targets] : llvm::enumerate(summary.tupleElements)) {
      bool valueElement = index < summary.tupleValues.size() &&
                          targets.objects.empty() && !targets.unknown;
      if (!valueElement)
        targets.unknown = true;
    }
    for (auto &value : summary.tupleValues)
      widenUnknownContents(value, widenUnknownContents);
    summary.booleanValue.reset();
    summary.predicate.reset();
    summary.elementPredicates.clear();
    if (summary.objectOccupancy)
      summary.objectOccupancy->unknown = true;
  };
  auto argumentSummary = [&](unsigned parameter) {
    Value value = argument(parameter);
    if (!value) {
      ValueSummary unknown;
      unknown.unknown = true;
      return unknown;
    }
    if (call.lifetime.getParameterKinds()[parameter] ==
        static_cast<int32_t>(LifetimeParameterKind::Value))
      return summarizeValue(value, state);

    // A reference argument is an address, not the value stored there. In
    // particular, an incoming tuple with unknown contents must not make its
    // fresh copy point back to the source object. Consult current contents:
    // tuple assignment also uses this helper after invalidating an owner.
    auto targets = resolveAddress(value, state);
    ValueSummary summary, unknownValue;
    unknownValue.unknown = true;
    bool first = true;
    bool unknown = targets.unknown || targets.mayBeNull || targets.mayBeInvalid;
    for (auto object : targets.objects) {
      auto found = state.storedValues.find(object);
      if (found == state.storedValues.end() ||
          found->second == ValueSummary{} || found->second == unknownValue) {
        unknown = true;
        continue;
      }
      summary = first ? found->second : joinValues(summary, found->second);
      first = false;
    }
    if (first &&
        isa<PointerType>(cast<PointerType>(value.getType()).getPointee()))
      summary.pointer.emplace();
    // Joining with a shapeless unknown would discard a known tuple's element
    // arrays. Widen in place instead, retaining possible bad pointer facts.
    if (unknown || first)
      widenUnknownContents(summary, widenUnknownContents);
    return summary;
  };
  auto setResult = [&](const ValueSummary &summary) {
    for (Value value : call.op->getResults())
      setValueSummary(state, value, summary);
  };
  auto empty = [&] {
    OccupancySummary contents;
    contents.unknown = false;
    contents.mayBeEmpty = true;
    contents.emptyOrigins.push_back(location);
    return contents;
  };
  auto present = [&] {
    OccupancySummary contents;
    contents.unknown = false;
    contents.mayBePresent = true;
    return contents;
  };
  auto pointerContents = [&](const ValueSummary &summary) {
    OccupancySummary contents;
    if (!summary.pointer)
      return contents;
    const auto &pointer = *summary.pointer;
    contents.mayBeEmpty = pointer.targets.mayBeNull;
    contents.mayBePresent =
        pointer.mayBeNonNull || !pointer.targets.objects.empty();
    contents.unknown = pointer.unknownNullness || pointer.targets.mayBeInvalid ||
                       (pointer.targets.unknown && !pointer.mayBeNonNull) ||
                       (!pointer.mayBeNonNull && !pointer.targets.mayBeNull);
    if (!contents.unknown && !contents.mayBeEmpty && !contents.mayBePresent)
      contents.unknown = true;
    if (contents.mayBeEmpty)
      contents.emptyOrigins.push_back(location);
    return contents;
  };
  auto checkMovedSource = [&](const ObjectTargets &source) {
    // Moving a freshly empty smart pointer is valid. Preserve the existing
    // repeated-move policy only when there is actual move provenance.
    for (auto object : source.objects) {
      auto history = state.invalidHist.find(object);
      if (history != state.invalidHist.end() &&
          llvm::any_of(history->second.entries,
                       [](const InvalidHistEntry &entry) {
                         return entry.style == InvalidStyle::MovedFrom;
                       }))
        checkUse(state, ObjectTargets{{object}}, location, UseAfterMove);
    }
  };

  Kind kind = Kind::None;
  for (auto object : call.receiver.objects)
    if (auto info = getObjectInfo(object)) {
      kind = getLibraryKind(info->type, theModule);
      if (kind != Kind::None)
        break;
    }
  if (kind == Kind::None && !call.args.empty() &&
      call.lifetime.getHasImplicitObject())
    kind = getLibraryKind(call.args.front().getType(), theModule);
  const bool smart = kind == Kind::UniquePointer || kind == Kind::SharedPointer;
  const bool nullableMove = smart || kind == Kind::WeakPointer;

  if (effect == Effect::None) {
    invalidateOwnerContents(state, call.receiver, location);
    reinitializeObjects(state, call.receiver, location);
    setOccupancy(state, call.receiver, OccupancySummary{}, location);
    return finish(std::move(state));
  }

  if (effect == Effect::Tie) {
    ValueSummary tuple;
    for (unsigned i = 0; i < call.lifetime.getNumParams(); ++i) {
      Value value = argument(i);
      tuple.tupleElements.push_back(value ? resolveAddress(value, state)
                                          : ObjectTargets{});
    }
    setResult(tuple);
    return finish(std::move(state));
  }

  if (effect == Effect::MakeTuple ||
      (kind == Kind::Tuple && effect == Effect::Construct)) {
    ValueSummary tuple;
    bool consumes = false;
    auto mapping = call.lifetime.getTupleElementParams();
    Type tupleType;
    if (effect == Effect::MakeTuple && call.op->getNumResults() == 1)
      tupleType = call.op->getResult(0).getType();
    else if (!call.args.empty())
      if (auto pointer = dyn_cast<PointerType>(call.args.front().getType()))
        tupleType = pointer.getPointee();
    auto record = dyn_cast_or_null<RecordType>(tupleType);
    auto facts =
        record ? getLifetimeRecord(record, theModule) : LifetimeRecordAttr{};
    auto kinds = facts ? facts.getTupleElementKinds() : DenseI32ArrayAttr{};
    if (mapping && kinds && mapping.size() == kinds.size()) {
      tuple.tupleElements.resize(mapping.size());
      tuple.tupleValues.resize(mapping.size());
      for (auto [element, parameter] : llvm::enumerate(mapping.asArrayRef())) {
        Value value = argument(parameter);
        auto targets = value ? resolveAddress(value, state) : ObjectTargets{};
        if (kinds[element] !=
            static_cast<int32_t>(LifetimeParameterKind::Value)) {
          tuple.tupleElements[element] = targets;
          tuple.tupleValues[element].unknown = true;
          continue;
        }
        auto snapshot = argumentSummary(parameter);
        if (call.lifetime.getParameterKinds()[parameter] !=
            static_cast<int32_t>(LifetimeParameterKind::Value)) {
          for (auto object : targets.objects)
            if (auto info = getObjectInfo(object)) {
              if (info->category == ObjectInfo::Pointer)
                checkMovedSource(ObjectTargets{{object}});
              else
                checkUse(state, ObjectTargets{{object}}, location);
            }
          if (call.lifetime.isParamRValueReference(parameter)) {
            markMoved(state, targets, location);
            consumes = true;
          }
        }
        snapshot.readOrigins.clear();
        snapshot.xvalueOrigins.clear();
        tuple.tupleValues[element] = std::move(snapshot);
      }
    } else if (call.lifetime.getNumParams() == 1 && argument(0) &&
               getLibraryKind(argument(0).getType(), theModule) ==
                   Kind::Tuple) {
      tuple = argumentSummary(0);
      auto source = resolveAddress(argument(0), state);
      checkMovedSource(source);
      if (call.lifetime.isParamRValueReference(0)) {
        markMoved(state, source, location);
        consumes = true;
      }
    } else {
      tuple.unknown = true;
    }
    if (effect == Effect::MakeTuple)
      setResult(tuple);
    else {
      reinitializeObjects(state, call.receiver, location);
      storeSummary(state, call.receiver, tuple);
    }
    return finish(std::move(state), consumes);
  }

  if (effect == Effect::TupleAssign) {
    ValueSummary destination;
    bool firstDestination = true;
    for (auto object : call.receiver.objects)
      if (auto found = state.storedValues.find(object);
          found != state.storedValues.end()) {
        destination = firstDestination ? found->second
                                       : joinValues(destination, found->second);
        firstDestination = false;
      }
    ValueSummary source = argumentSummary(0);
    if (Value value = argument(0))
      checkMovedSource(resolveAddress(value, state));
    auto receiverType = cast<PointerType>(call.args.front().getType());
    auto facts = getLifetimeRecord(cast<RecordType>(receiverType.getPointee()),
                                   theModule);
    auto kinds = facts.getTupleElementKinds();
    auto sourceType = dyn_cast<PointerType>(argument(0).getType());
    auto sourceRecord = sourceType
                            ? dyn_cast<RecordType>(sourceType.getPointee())
                            : RecordType{};
    auto sourceFacts = sourceRecord ? getLifetimeRecord(sourceRecord, theModule)
                                    : LifetimeRecordAttr{};
    auto sourceKinds =
        sourceFacts ? sourceFacts.getTupleElementKinds() : DenseI32ArrayAttr{};
    unsigned arity = kinds ? kinds.size() : destination.tupleElements.size();
    destination.tupleElements.resize(arity);
    destination.tupleValues.resize(arity);
    SmallVector<ValueSummary, 0> replacements(arity);
    SmallVector<ObjectTargets, 0> writes(arity);
    auto readElement = [&](const ValueSummary &tuple, unsigned index,
                           bool diagnose) {
      ValueSummary replacement;
      replacement.unknown = index >= tuple.tupleElements.size();
      if (index < tuple.tupleValues.size() &&
          (index >= tuple.tupleElements.size() ||
           (tuple.tupleElements[index].objects.empty() &&
            !tuple.tupleElements[index].unknown))) {
        replacement = tuple.tupleValues[index];
      } else if (index < tuple.tupleElements.size()) {
        const auto &reads = tuple.tupleElements[index];
        bool firstSource = true;
        // Snapshot reads before any writes. Copying a null pointer is valid;
        // reading a moved scalar is not.
        for (auto object : reads.objects) {
          if (diagnose)
            if (auto info = getObjectInfo(object)) {
              if (info->category == ObjectInfo::Pointer)
                checkMovedSource(ObjectTargets{{object}});
              else
                checkUse(state, ObjectTargets{{object}}, location);
            }
          if (auto found = state.storedValues.find(object);
              found != state.storedValues.end()) {
            replacement = firstSource ? found->second
                                      : joinValues(replacement, found->second);
            firstSource = false;
          }
        }
        replacement.unknown |= reads.unknown || firstSource;
      }
      replacement.readOrigins.clear();
      replacement.xvalueOrigins.clear();
      return replacement;
    };
    for (unsigned index = 0; index < arity; ++index) {
      auto &targets = destination.tupleElements[index];
      auto &supported = writes[index];
      supported.unknown = targets.unknown || !targets.isDefinite();
      // Arbitrary user assignment operators may leave their object moved.
      for (auto object : targets.objects)
        if (auto info = getObjectInfo(object))
          if (info->category == ObjectInfo::Scalar ||
              isa<PointerType>(info->type) ||
              getLibraryKind(info->type, theModule) != Kind::None)
            supported.objects.push_back(object);
      replacements[index] = readElement(source, index, true);
    }

    // The tuple contract does not impose a portable left-to-right element
    // assignment order. Reference reads can observe another element's write.
    // Close those dependencies over the finite set of RHS snapshots. This
    // deliberately joins possible orders rather than inventing a simultaneous
    // swap for tie(p, q) = tie(q, p).
    auto overlaps = [](const ObjectTargets &a, const ObjectTargets &b) {
      return a.unknown || b.unknown ||
             llvm::any_of(a.objects, [&](ObjectId object) {
               return llvm::is_contained(b.objects, object);
             });
    };
    for (unsigned round = 0; round < arity; ++round) {
      bool changed = false;
      for (unsigned i = 0; i < arity; ++i) {
        if (i >= source.tupleElements.size())
          continue;
        const auto &reads = source.tupleElements[i];
        if (reads.objects.empty() && !reads.unknown)
          continue;
        for (unsigned j = 0; j < arity; ++j) {
          if (writes[j].objects.empty() || !overlaps(reads, writes[j]))
            continue;
          auto joined = joinValues(replacements[i], replacements[j]);
          changed |= joined != replacements[i];
          replacements[i] = std::move(joined);
        }
      }
      if (!changed)
        break;
    }

    // More than one element can write the same object. Any such assignment
    // may be last, even when its RHS is an independent value snapshot.
    DenseMap<ObjectId, ValueSummary> objectWrites;
    DenseMap<ObjectId, unsigned> soleAssignments;
    SmallVector<ObjectId, 4> writeOrder;
    SmallPtrSet<ObjectId, 4> definiteWrites;
    for (unsigned i = 0; i < arity; ++i) {
      if (kinds &&
          kinds[i] == static_cast<int32_t>(LifetimeParameterKind::Value))
        destination.tupleValues[i] = replacements[i];
      for (auto object : writes[i].objects) {
        auto [it, inserted] = objectWrites.try_emplace(object, replacements[i]);
        if (inserted)
          writeOrder.push_back(object);
        else
          it->second = joinValues(it->second, replacements[i]);
        auto [assignment, unique] = soleAssignments.try_emplace(object, i);
        if (!unique) {
          unsigned prior = assignment->second;
          // Repeating the same shared-reference copy has the same final
          // ownership relation whichever element is assigned last.
          if (prior >= arity || i >= source.tupleElements.size() ||
              prior >= source.tupleElements.size() || !sourceKinds ||
              i >= sourceKinds.size() || prior >= sourceKinds.size() ||
              sourceKinds[i] != static_cast<int32_t>(
                                    LifetimeParameterKind::LValueReference) ||
              sourceKinds[prior] != sourceKinds[i] ||
              source.tupleElements[prior] != source.tupleElements[i] ||
              !writes[prior].isDefinite() || !writes[i].isDefinite())
            assignment->second = arity;
        }
        if (writes[i].isDefinite())
          definiteWrites.insert(object);
      }
    }
    storeSummary(state, call.receiver, destination);
    for (auto object : writeOrder) {
      if (call.mayThrow)
        result.exceptions.push_back({state, Type{}});
      ObjectTargets target{{object}, !definiteWrites.contains(object)};
      auto &replacement = objectWrites.find(object)->second;
      auto info = getObjectInfo(object);
      unsigned element = soleAssignments.lookup(object);
      ObjectTargets copiedOwner;
      if (target.isDefinite() && info &&
          getLibraryKind(info->type, theModule) == Kind::SharedPointer &&
          element < arity && element < source.tupleElements.size()) {
        const auto &reads = source.tupleElements[element];
        bool independent = reads.isDefinite();
        for (unsigned i = 0; i < arity && independent; ++i)
          if (i != element && !writes[i].objects.empty() &&
              overlaps(reads, writes[i]))
            independent = false;
        // A tuple of lvalue references still copies its elements when the
        // tuple itself is an rvalue. Do not infer a value-element owner from
        // a payload snapshot: memberwise ownership is outside this model.
        bool copy =
            !call.lifetime.isParamRValueReference(0) ||
            (sourceKinds && element < sourceKinds.size() &&
             sourceKinds[element] ==
                 static_cast<int32_t>(LifetimeParameterKind::LValueReference));
        bool selfCopy = reads.isDefinite() && reads.objects.front() == object;
        if ((independent || selfCopy) && copy)
          if (auto sourceInfo = getObjectInfo(reads.objects.front());
              sourceInfo && getLibraryKind(sourceInfo->type, theModule) ==
                                Kind::SharedPointer)
            copiedOwner = reads;
      }
      if (copiedOwner.isDefinite() && copiedOwner.objects.front() == object)
        continue;
      invalidateOwnerContents(state, target, location);
      reinitializeObjects(state, target, location);
      if (copiedOwner.isDefinite())
        transferOwnerContents(state, target, copiedOwner, true);
      // Replacing an owner can invalidate another element's pending pointer
      // payload. Re-read the tracked snapshots after invalidation and retain
      // those facts alongside the joined tuple-element assignment alternatives.
      auto currentSource = argumentSummary(0);
      for (unsigned i = 0; i < arity; ++i)
        if (llvm::is_contained(writes[i].objects, object))
          replacement =
              joinValues(replacement, readElement(currentSource, i, false));
      storeSummary(state, target, replacement);
      setOccupancy(state, target,
                   replacement.objectOccupancy.value_or(OccupancySummary{}),
                   location);
      if (replacement.pointer && info && isa<PointerType>(info->type)) {
        PSetType payload;
        for (auto pointee : replacement.pointer->targets.objects)
          payload.insert(State::getLocalValue(pointee));
        for (auto owner : replacement.pointer->borrowedOwners)
          payload.insert(State::getOwnedBy(owner));
        if (replacement.pointer->targets.mayBeNull)
          payload.insert(State::getNullPtr());
        if (replacement.pointer->targets.mayBeInvalid)
          payload.insert(State::getInvalid());
        if (replacement.pointer->mayBeNonNull && payload.empty())
          payload.insert(State::getGlobal());
        if (target.isDefinite())
          state.pmap[object] = std::move(payload);
        else
          llvm::set_union(state.pmap[object], payload);
        if (replacement.pointer->targets.mayBeNull)
          state.nullHist[object] =
              replacement.pointer->nullOrigin.value_or(location);
        if (replacement.pointer->targets.mayBeInvalid)
          state.invalidHist[object] = replacement.pointer->invalidHistory;
      }
    }
    // The returned reference refers to the tuple, not to its first element.
    ValueSummary reference;
    reference.pointer = PointerValueSummary{};
    reference.pointer->targets = call.receiver;
    reference.pointer->mayBeNonNull = call.receiver.isDefinite();
    setResult(reference);
    return finish(std::move(state), true);
  }

  if (effect == Effect::TryEmplace || effect == Effect::TryEmplaceHint) {
    PathState consumed = state;
    ObjectTargets sources;
    for (unsigned i = 0; i < call.lifetime.getNumParams(); ++i) {
      if (!call.lifetime.isParamRValueReference(i))
        continue;
      Value value = argument(i);
      if (!value)
        continue;
      auto targets = resolveAddress(value, consumed);
      sources = joinTargets(sources, targets);
      PathState unconsumed = consumed;
      checkUse(consumed, targets, location, UseAfterMove);
      if (call.lifetime.isParamGuaranteedMove(i))
        transferOwnerContents(consumed, call.receiver, targets, false);
      markMoved(consumed, targets, location);
      for (auto object : targets.objects)
        if (auto info = getObjectInfo(object)) {
          Kind sourceKind = getLibraryKind(info->type, theModule);
          if (sourceKind == Kind::UniquePointer ||
              sourceKind == Kind::SharedPointer ||
              sourceKind == Kind::WeakPointer)
            setOccupancy(consumed,
                         ObjectTargets{{object}, !targets.isDefinite()},
                         empty(), location);
        }
      if (!call.lifetime.isParamGuaranteedMove(i)) {
        joinStateInto(unconsumed, consumed);
        consumed = std::move(unconsumed);
      }
    }
    // Successful insertion proves nonempty contents. References to existing
    // associative-container elements survive insertion; do not destroy them.
    // The no-insertion path remains possible.
    setOccupancy(consumed, call.receiver, present(), location);
    auto predicate =
        makeConditionalMovePredicate(state, consumed, sources, call.op);
    joinStateInto(state, consumed);
    ValueSummary insertionResult;
    if (effect == Effect::TryEmplace) {
      setConditionalMoveSite(state, predicate);
      unsigned index = call.lifetime.getInsertionResultIndex();
      insertionResult.elementPredicates.resize(index + 1);
      insertionResult.elementPredicates[index] = std::move(predicate);
    }
    setResult(insertionResult);
    return finish(std::move(state), true);
  }

  if (effect == Effect::Construct || effect == Effect::Assign) {
    auto callee = call.callee;
    Attribute special = callee ? callee.getFuncInfoAttr() : Attribute{};
    bool isCopy = false, isMove = false;
    if (auto constructor = dyn_cast_or_null<CXXCtorAttr>(special)) {
      isCopy = constructor.getCtorKind() == CtorKind::Copy;
      isMove = constructor.getCtorKind() == CtorKind::Move;
    } else if (auto assignment = dyn_cast_or_null<CXXAssignAttr>(special)) {
      isCopy = assignment.getAssignKind() == AssignKind::Copy;
      isMove = assignment.getAssignKind() == AssignKind::Move;
    }
    Value sourceValue = argument(0);
    ObjectTargets source =
        sourceValue ? resolveAddress(sourceValue, state) : ObjectTargets{};
    // Converting library constructors/assignments need not be C++ special
    // members. Their reference category still describes a standard transfer
    // when the source is the same library family (or unique -> shared).
    Kind sourceKind = sourceValue
                          ? getLibraryKind(sourceValue.getType(), theModule)
                          : Kind::None;
    if (!isCopy && !isMove && sourceKind != Kind::None &&
        (sourceKind == kind ||
         (kind == Kind::SharedPointer && sourceKind == Kind::UniquePointer)) &&
        call.lifetime.getNumParams() == 1) {
      isMove = call.lifetime.isParamRValueReference(0);
      isCopy = !isMove &&
               call.lifetime.getParameterKinds()[0] ==
                   static_cast<int32_t>(LifetimeParameterKind::LValueReference);
    }
    ValueSummary summary = argumentSummary(0);
    OccupancySummary contents;
    if (isCopy || isMove) {
      contents = getOccupancy(state, source);
      if (source.objects.empty() && summary.objectOccupancy)
        contents = *summary.objectOccupancy;
      checkMovedSource(source);
    } else if (!sourceValue) {
      contents = empty();
    } else if (smart) {
      contents = pointerContents(summary);
    } else if (kind == Kind::Optional || kind == Kind::Any) {
      // A value constructor establishes engagement. Copy/move constructors
      // were handled above and preserve their source engagement alternatives.
      contents = present();
    } else if (kind == Kind::Container) {
      // A count of zero establishes an empty container. Other overloads and
      // ranges remain unknown unless their contents are explicitly known.
      if (auto constant = sourceValue.getDefiningOp<ConstantOp>())
        if (auto integer = dyn_cast<cir::IntAttr>(constant.getValue()))
          contents = integer.getValue().isZero() ? empty() : present();
    }

    const bool overlaps = llvm::any_of(source.objects, [&](ObjectId object) {
      return llvm::is_contained(call.receiver.objects, object);
    });
    if (isCopy && kind == Kind::SharedPointer && source.isDefinite() &&
        call.receiver.isDefinite() && overlaps) {
      // A self-copy of a shared owner does not release its resource.
    } else if (isMove && overlaps) {
      // Self transfer must not manufacture fresh ownership. Retain the
      // incoming facts alongside the possible moved-from state.
      PathState moved = state;
      markMoved(moved, source, location);
      joinStateInto(state, moved);
    } else {
      replaceOwnerContents(state, call.receiver, contents, call.op);
      if ((isMove && (smart || kind == Kind::Container)) ||
          (isCopy && kind == Kind::SharedPointer))
        transferOwnerContents(state, call.receiver, source,
                              isCopy && kind == Kind::SharedPointer);
      summary.objectOccupancy = contents;
      storeSummary(state, call.receiver, summary);
      if (isMove) {
        markMoved(state, source, location);
        if (nullableMove)
          setOccupancy(state, source, empty(), location);
        else if (kind == Kind::Any || kind == Kind::Container)
          setOccupancy(state, source, OccupancySummary{}, location);
        // optional moves do not disengage the source.
      }
    }
    if (effect == Effect::Assign) {
      ValueSummary reference;
      reference.pointer = PointerValueSummary{};
      reference.pointer->targets = call.receiver;
      reference.pointer->mayBeNonNull = call.receiver.isDefinite();
      setResult(reference);
    }
    return finish(std::move(state), isMove);
  }

  switch (effect) {
  case Effect::SharedAliasConstruct: {
    ValueSummary pointer = argumentSummary(1);
    auto contents = pointerContents(pointer);
    ObjectTargets source = resolveAddress(argument(0), state);
    checkMovedSource(source);
    replaceOwnerContents(state, call.receiver, contents, call.op);
    bool move = call.lifetime.isParamRValueReference(0);
    transferOwnerContents(state, call.receiver, source, !move);
    pointer.objectOccupancy = contents;
    pointer.externallyAliasedPointer = true;
    storeSummary(state, call.receiver, pointer);
    if (move) {
      markMoved(state, source, location);
      setOccupancy(state, source, empty(), location);
    }
    return finish(std::move(state), move);
  }
  case Effect::MapSubscript: {
    // operator[] inserts and value-initializes an absent mapped value. It has
    // no nonempty-container precondition, including on a moved-from map.
    if (call.lifetime.isParamRValueReference(0)) {
      PathState inserted = state;
      auto key = resolveAddress(argument(0), inserted);
      checkMovedSource(key);
      markMoved(inserted, key, location);
      joinStateInto(state, inserted);
    }
    reinitializeObjects(state, call.receiver, location);
    setOccupancy(state, call.receiver, present(), location);
    ValueSummary reference;
    reference.pointer.emplace();
    reference.pointer->targets.unknown = true;
    reference.pointer->mayBeNonNull = true;
    reference.pointer->borrowedOwners.insert(
        reference.pointer->borrowedOwners.end(), call.receiver.objects.begin(),
        call.receiver.objects.end());
    setResult(reference);
    return finish(std::move(state), true);
  }
  case Effect::Disengage: {
    replaceOwnerContents(state, call.receiver, empty(), call.op);
    ValueSummary summary;
    summary.objectOccupancy = empty();
    storeSummary(state, call.receiver, summary);
    ValueSummary reference;
    reference.pointer = PointerValueSummary{};
    reference.pointer->targets = call.receiver;
    reference.pointer->mayBeNonNull = call.receiver.isDefinite();
    setResult(reference);
    return finish(std::move(state));
  }
  case Effect::Reset: {
    ValueSummary summary = argument(0) ? argumentSummary(0) : ValueSummary{};
    auto contents = argument(0) ? pointerContents(summary) : empty();
    replaceOwnerContents(state, call.receiver, contents, call.op);
    summary.objectOccupancy = contents;
    storeSummary(state, call.receiver, summary);
    return finish(std::move(state));
  }
  case Effect::Clear: {
    replaceOwnerContents(state, call.receiver, empty(), call.op);
    ValueSummary summary;
    summary.objectOccupancy = empty();
    storeSummary(state, call.receiver, summary);
    return finish(std::move(state));
  }
  case Effect::Get:
  case Effect::Release:
  case Effect::Data: {
    auto occupancy = getOccupancy(state, call.receiver);
    ValueSummary pointer;
    pointer.pointer = PointerValueSummary{};
    // An aliasing shared_ptr's stored pointer need not point into the resource
    // kept alive by its control block. Preserve actual pointee provenance.
    bool externalAlias = false;
    bool firstPayload = true;
    for (auto object : call.receiver.objects)
      if (auto stored = state.storedValues.find(object);
          stored != state.storedValues.end() && stored->second.pointer) {
        pointer =
            firstPayload ? stored->second : joinValues(pointer, stored->second);
        externalAlias |= stored->second.externallyAliasedPointer;
        firstPayload = false;
      }
    if (occupancy.mayBeEmpty && !occupancy.mayBePresent && !occupancy.unknown) {
      pointer.pointer = PointerValueSummary{};
      firstPayload = true;
    }
    if (firstPayload)
      pointer.pointer->targets.unknown =
          occupancy.unknown || occupancy.mayBePresent;
    pointer.pointer->targets.mayBeNull = occupancy.mayBeEmpty && smart;
    pointer.pointer->mayBeNonNull =
        occupancy.mayBePresent && !occupancy.unknown;
    pointer.pointer->unknownNullness = occupancy.unknown;
    if (pointer.pointer->targets.mayBeNull) {
      if (!occupancy.emptyOrigins.empty())
        pointer.pointer->nullOrigin = occupancy.emptyOrigins.front();
      // A get() call is valid on a moved-from smart pointer. If its result is
      // later dereferenced, retain the move that established its nullness.
      for (auto object : call.receiver.objects)
        if (auto history = state.invalidHist.find(object);
            history != state.invalidHist.end())
          for (const auto &entry : history->second.entries)
            if (!llvm::is_contained(pointer.pointer->invalidHistory.entries,
                                    entry))
              pointer.pointer->invalidHistory.entries.push_back(entry);
    }
    if (effect != Effect::Release && !externalAlias)
      pointer.pointer->borrowedOwners.insert(
          pointer.pointer->borrowedOwners.end(), call.receiver.objects.begin(),
          call.receiver.objects.end());
    if (effect == Effect::Release) {
      releaseOwnerContents(state, call.receiver, location);
      reinitializeObjects(state, call.receiver, location);
      setOccupancy(state, call.receiver, empty(), location);
    }
    setResult(pointer);
    return finish(std::move(state));
  }
  case Effect::Bool:
  case Effect::HasValue:
  case Effect::Empty:
  case Effect::Expired: {
    ValueSummary value;
    ValuePredicate predicate;
    predicate.domain = kind == Kind::Optional
                           ? ValuePredicate::OptionalEngagement
                           : ValuePredicate::OwnerOccupancy;
    predicate.objects = call.receiver;
    predicate.trueMeansPresent =
        effect != Effect::Empty && effect != Effect::Expired;
    value.predicate =
        std::make_shared<const ValuePredicate>(std::move(predicate));
    setResult(value);
    return finish(std::move(state));
  }
  case Effect::Size:
    return finish(std::move(state));
  case Effect::Dereference:
  case Effect::Access: {
    checkLibraryContents(*this, state, call.receiver, location);
    ValueSummary reference;
    reference.pointer = PointerValueSummary{};
    reference.pointer->targets.unknown = true;
    reference.pointer->mayBeNonNull = true;
    bool externalAlias = false;
    for (auto object : call.receiver.objects)
      if (auto stored = state.storedValues.find(object);
          stored != state.storedValues.end() && stored->second.pointer &&
          stored->second.externallyAliasedPointer) {
        if (!externalAlias)
          reference = stored->second;
        else
          reference = joinValues(reference, stored->second);
        externalAlias = true;
      }
    // A null contents access was diagnosed above. The resulting reference
    // must retain its pointee identity without issuing a duplicate null use.
    reference.pointer->targets.mayBeNull = false;
    reference.pointer->mayBeNonNull = true;
    reference.pointer->unknownNullness = false;
    if (!externalAlias)
      reference.pointer->borrowedOwners.insert(
          reference.pointer->borrowedOwners.end(),
          call.receiver.objects.begin(), call.receiver.objects.end());
    setResult(reference);
    return finish(std::move(state));
  }
  case Effect::Lock: {
    ValueSummary value;
    auto occupancy = getOccupancy(state, call.receiver);
    value.objectOccupancy =
        occupancy.mayBeEmpty && !occupancy.mayBePresent && !occupancy.unknown
            ? empty()
            : OccupancySummary{};
    setResult(value);
    return finish(std::move(state));
  }
  case Effect::Destroy:
    // Destruction is valid after a move. Lexical/lifetime transfers perform
    // the storage KILL; destruction invalidates borrows of owned contents.
    invalidateOwnerContents(state, call.receiver, location);
    return finish(std::move(state));
  case Effect::None:
  case Effect::Construct:
  case Effect::Assign:
  case Effect::Tie:
  case Effect::TupleAssign:
  case Effect::TryEmplace:
  case Effect::TryEmplaceHint:
  case Effect::MakeTuple:
  case Effect::TupleGet:
    llvm_unreachable("handled before the library effect switch");
  }
  llvm_unreachable("invalid library effect");
}

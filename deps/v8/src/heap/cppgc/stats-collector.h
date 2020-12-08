// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_CPPGC_STATS_COLLECTOR_H_
#define V8_HEAP_CPPGC_STATS_COLLECTOR_H_

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "src/base/macros.h"
#include "src/base/platform/time.h"
#include "src/heap/cppgc/garbage-collector.h"
#include "src/heap/cppgc/heap-base.h"
#include "src/heap/cppgc/trace-event.h"

namespace cppgc {
namespace internal {

#define CPPGC_FOR_ALL_SCOPES(V)             \
  V(AtomicMark)                             \
  V(AtomicSweep)                            \
  V(AtomicCompact)                          \
  V(IncrementalMark)                        \
  V(IncrementalSweep)                       \
  V(MarkIncrementalStart)                   \
  V(MarkIncrementalFinalize)                \
  V(MarkAtomicPrologue)                     \
  V(MarkAtomicEpilogue)                     \
  V(MarkTransitiveClosure)                  \
  V(MarkTransitiveClosureWithDeadline)      \
  V(MarkFlushEphemerons)                    \
  V(MarkProcessBailOutObjects)              \
  V(MarkProcessMarkingWorklist)             \
  V(MarkProcessWriteBarrierWorklist)        \
  V(MarkProcessNotFullyconstructedWorklist) \
  V(MarkProcessEphemerons)                  \
  V(MarkVisitRoots)                         \
  V(MarkVisitNotFullyConstructedObjects)    \
  V(MarkVisitPersistents)                   \
  V(MarkVisitCrossThreadPersistents)        \
  V(MarkVisitStack)                         \
  V(MarkVisitRememberedSets)                \
  V(WeakInvokeCallbacks)                    \
  V(SweepInvokePreFinalizers)               \
  V(SweepIdleStep)                          \
  V(SweepOnAllocation)                      \
  V(SweepFinalize)

#define CPPGC_FOR_ALL_CONCURRENT_SCOPES(V) \
  V(ConcurrentMarkProcessEphemerons)       \
  V(ConcurrentMark)                        \
  V(ConcurrentSweep)

// Sink for various time and memory statistics.
class V8_EXPORT_PRIVATE StatsCollector final {
  using CollectionType = GarbageCollector::Config::CollectionType;
  using IsForcedGC = GarbageCollector::Config::IsForcedGC;

 public:
#if defined(CPPGC_DECLARE_ENUM)
  static_assert(false, "CPPGC_DECLARE_ENUM macro is already defined");
#endif

  enum ScopeId {
#define CPPGC_DECLARE_ENUM(name) k##name,
    CPPGC_FOR_ALL_SCOPES(CPPGC_DECLARE_ENUM)
#undef CPPGC_DECLARE_ENUM
        kNumScopeIds,
  };

  enum ConcurrentScopeId {
#define CPPGC_DECLARE_ENUM(name) k##name,
    CPPGC_FOR_ALL_CONCURRENT_SCOPES(CPPGC_DECLARE_ENUM)
#undef CPPGC_DECLARE_ENUM
        kNumConcurrentScopeIds
  };

  // POD to hold interesting data accumulated during a garbage collection cycle.
  //
  // The event is always fully populated when looking at previous events but
  // may only be partially populated when looking at the current event.
  struct Event final {
    V8_EXPORT_PRIVATE explicit Event();

    v8::base::TimeDelta scope_data[kNumScopeIds];
    v8::base::Atomic32 concurrent_scope_data[kNumConcurrentScopeIds]{0};

    size_t epoch = -1;
    CollectionType collection_type = CollectionType::kMajor;
    IsForcedGC is_forced_gc = IsForcedGC::kNotForced;
    // Marked bytes collected during marking.
    size_t marked_bytes = 0;
  };

 private:
#if defined(CPPGC_CASE)
  static_assert(false, "CPPGC_CASE macro is already defined");
#endif

  constexpr static const char* GetScopeName(ScopeId id, CollectionType type) {
    switch (id) {
#define CPPGC_CASE(name)                                   \
  case k##name:                                            \
    return type == CollectionType::kMajor ? "CppGC." #name \
                                          : "CppGC." #name ".Minor";
      CPPGC_FOR_ALL_SCOPES(CPPGC_CASE)
#undef CPPGC_CASE
      default:
        return nullptr;
    }
  }

  constexpr static const char* GetScopeName(ConcurrentScopeId id,
                                            CollectionType type) {
    switch (id) {
#define CPPGC_CASE(name)                                   \
  case k##name:                                            \
    return type == CollectionType::kMajor ? "CppGC." #name \
                                          : "CppGC." #name ".Minor";
      CPPGC_FOR_ALL_CONCURRENT_SCOPES(CPPGC_CASE)
#undef CPPGC_CASE
      default:
        return nullptr;
    }
  }

  enum TraceCategory { kEnabled, kDisabled };
  enum ScopeContext { kMutatorThread, kConcurrentThread };

  // Trace a particular scope. Will emit a trace event and record the time in
  // the corresponding StatsCollector.
  template <TraceCategory trace_category, ScopeContext scope_category>
  class V8_NODISCARD InternalScope {
    using ScopeIdType = std::conditional_t<scope_category == kMutatorThread,
                                           ScopeId, ConcurrentScopeId>;

   public:
    template <typename... Args>
    InternalScope(HeapBase& heap, ScopeIdType scope_id, Args... args)
        : heap_(heap),
          stats_collector_(heap_.stats_collector()),
          start_time_(v8::base::TimeTicks::Now()),
          scope_id_(scope_id) {
      DCHECK_LE(0, scope_id_);
      DCHECK_LT(static_cast<int>(scope_id_),
                scope_category == kMutatorThread
                    ? static_cast<int>(kNumScopeIds)
                    : static_cast<int>(kNumConcurrentScopeIds));
      StartTrace(args...);
    }

    ~InternalScope() {
      StopTrace();
      IncreaseScopeTime();
    }

    InternalScope(const InternalScope&) = delete;
    InternalScope& operator=(const InternalScope&) = delete;

   private:
    void* operator new(size_t, void*) = delete;
    void* operator new(size_t) = delete;

    inline constexpr static const char* TraceCategory();

    template <typename... Args>
    inline void StartTrace(Args... args);
    inline void StopTrace();

    inline void StartTraceImpl();
    template <typename Value1>
    inline void StartTraceImpl(const char* k1, Value1 v1);
    template <typename Value1, typename Value2>
    inline void StartTraceImpl(const char* k1, Value1 v1, const char* k2,
                               Value2 v2);
    inline void StopTraceImpl();

    inline void IncreaseScopeTime();

    HeapBase& heap_;
    StatsCollector* const stats_collector_;
    const v8::base::TimeTicks start_time_;
    const ScopeIdType scope_id_;
  };

 public:
  using DisabledScope = InternalScope<kDisabled, kMutatorThread>;
  using EnabledScope = InternalScope<kEnabled, kMutatorThread>;
  using DisabledConcurrentScope = InternalScope<kDisabled, kConcurrentThread>;
  using EnabledConcurrentScope = InternalScope<kEnabled, kConcurrentThread>;

  // Observer for allocated object size. May be used to implement heap growing
  // heuristics.
  class AllocationObserver {
   public:
    // Called after observing at least
    // StatsCollector::kAllocationThresholdBytes changed bytes through
    // allocation or explicit free. Reports both, negative and positive
    // increments, to allow observer to decide whether absolute values or only
    // the deltas is interesting.
    //
    // May trigger GC.
    virtual void AllocatedObjectSizeIncreased(size_t) = 0;
    virtual void AllocatedObjectSizeDecreased(size_t) = 0;

    // Called when the exact size of allocated object size is known. In
    // practice, this is after marking when marked bytes == allocated bytes.
    //
    // Must not trigger GC synchronously.
    virtual void ResetAllocatedObjectSize(size_t) = 0;
  };

  // Observers are implemented using virtual calls. Avoid notifications below
  // reasonably interesting sizes.
  static constexpr size_t kAllocationThresholdBytes = 1024;

  StatsCollector() = default;
  StatsCollector(const StatsCollector&) = delete;
  StatsCollector& operator=(const StatsCollector&) = delete;

  void RegisterObserver(AllocationObserver*);
  void UnregisterObserver(AllocationObserver*);

  void NotifyAllocation(size_t);
  void NotifyExplicitFree(size_t);
  // Safepoints should only be invoked when garabge collections are possible.
  // This is necessary as increments and decrements are reported as close to
  // their actual allocation/reclamation as possible.
  void NotifySafePointForConservativeCollection();

  // Indicates a new garbage collection cycle.
  void NotifyMarkingStarted(CollectionType, IsForcedGC);
  // Indicates that marking of the current garbage collection cycle is
  // completed.
  void NotifyMarkingCompleted(size_t marked_bytes);
  // Indicates the end of a garbage collection cycle. This means that sweeping
  // is finished at this point.
  void NotifySweepingCompleted();

  // Size of live objects in bytes  on the heap. Based on the most recent marked
  // bytes and the bytes allocated since last marking.
  size_t allocated_object_size() const;

  double GetRecentAllocationSpeedInBytesPerMs() const;

  const Event& GetPreviousEventForTesting() const { return previous_; }

 private:
  enum class GarbageCollectionState : uint8_t {
    kNotRunning,
    kMarking,
    kSweeping
  };

  // Invokes |callback| for all registered observers.
  template <typename Callback>
  void ForAllAllocationObservers(Callback callback);

  void AllocatedObjectSizeSafepointImpl();

  // Allocated bytes since the end of marking. These bytes are reset after
  // marking as they are accounted in marked_bytes then. May be negative in case
  // an object was explicitly freed that was marked as live in the previous
  // cycle.
  int64_t allocated_bytes_since_end_of_marking_ = 0;
  v8::base::TimeTicks time_of_last_end_of_marking_ = v8::base::TimeTicks::Now();
  // Counters for allocation and free. The individual values are never negative
  // but their delta may be because of the same reason the overall
  // allocated_bytes_since_end_of_marking_ may be negative. Keep integer
  // arithmetic for simplicity.
  int64_t allocated_bytes_since_safepoint_ = 0;
  int64_t explicitly_freed_bytes_since_safepoint_ = 0;

  // vector to allow fast iteration of observers. Register/Unregisters only
  // happens on startup/teardown.
  std::vector<AllocationObserver*> allocation_observers_;

  GarbageCollectionState gc_state_ = GarbageCollectionState::kNotRunning;

  // The event being filled by the current GC cycle between NotifyMarkingStarted
  // and NotifySweepingFinished.
  Event current_;
  // The previous GC event which is populated at NotifySweepingFinished.
  Event previous_;
};

template <typename Callback>
void StatsCollector::ForAllAllocationObservers(Callback callback) {
  for (AllocationObserver* observer : allocation_observers_) {
    callback(observer);
  }
}

template <StatsCollector::TraceCategory trace_category,
          StatsCollector::ScopeContext scope_category>
constexpr const char*
StatsCollector::InternalScope<trace_category, scope_category>::TraceCategory() {
  switch (trace_category) {
    case kEnabled:
      return "cppgc";
    case kDisabled:
      return TRACE_DISABLED_BY_DEFAULT("cppgc");
  }
}

template <StatsCollector::TraceCategory trace_category,
          StatsCollector::ScopeContext scope_category>
template <typename... Args>
void StatsCollector::InternalScope<trace_category, scope_category>::StartTrace(
    Args... args) {
  if (trace_category == StatsCollector::TraceCategory::kEnabled)
    StartTraceImpl(args...);
}

template <StatsCollector::TraceCategory trace_category,
          StatsCollector::ScopeContext scope_category>
void StatsCollector::InternalScope<trace_category,
                                   scope_category>::StopTrace() {
  if (trace_category == StatsCollector::TraceCategory::kEnabled)
    StopTraceImpl();
}

template <StatsCollector::TraceCategory trace_category,
          StatsCollector::ScopeContext scope_category>
void StatsCollector::InternalScope<trace_category,
                                   scope_category>::StartTraceImpl() {
  TRACE_EVENT_BEGIN0(
      TraceCategory(),
      GetScopeName(scope_id_, stats_collector_->current_.collection_type));
}

template <StatsCollector::TraceCategory trace_category,
          StatsCollector::ScopeContext scope_category>
template <typename Value1>
void StatsCollector::InternalScope<
    trace_category, scope_category>::StartTraceImpl(const char* k1, Value1 v1) {
  TRACE_EVENT_BEGIN1(
      TraceCategory(),
      GetScopeName(scope_id_, stats_collector_->current_.collection_type), k1,
      v1);
}

template <StatsCollector::TraceCategory trace_category,
          StatsCollector::ScopeContext scope_category>
template <typename Value1, typename Value2>
void StatsCollector::InternalScope<
    trace_category, scope_category>::StartTraceImpl(const char* k1, Value1 v1,
                                                    const char* k2, Value2 v2) {
  TRACE_EVENT_BEGIN2(
      TraceCategory(),
      GetScopeName(scope_id_, stats_collector_->current_.collection_type), k1,
      v1, k2, v2);
}

template <StatsCollector::TraceCategory trace_category,
          StatsCollector::ScopeContext scope_category>
void StatsCollector::InternalScope<trace_category,
                                   scope_category>::StopTraceImpl() {
  TRACE_EVENT_END2(
      TraceCategory(),
      GetScopeName(scope_id_, stats_collector_->current_.collection_type),
      "epoch", stats_collector_->current_.epoch, "forced",
      stats_collector_->current_.is_forced_gc == IsForcedGC::kForced);
}

template <StatsCollector::TraceCategory trace_category,
          StatsCollector::ScopeContext scope_category>
void StatsCollector::InternalScope<trace_category,
                                   scope_category>::IncreaseScopeTime() {
  DCHECK_NE(GarbageCollectionState::kNotRunning, stats_collector_->gc_state_);
  v8::base::TimeDelta time = v8::base::TimeTicks::Now() - start_time_;
  if (scope_category == StatsCollector::ScopeContext::kMutatorThread) {
    stats_collector_->current_.scope_data[scope_id_] += time;
    return;
  }
  // scope_category == StatsCollector::ScopeContext::kConcurrentThread
  using Atomic32 = v8::base::Atomic32;
  const int64_t ms = time.InMicroseconds();
  DCHECK(ms <= std::numeric_limits<Atomic32>::max());
  v8::base::Relaxed_AtomicIncrement(
      &stats_collector_->current_.concurrent_scope_data[scope_id_],
      static_cast<Atomic32>(ms));
}

}  // namespace internal
}  // namespace cppgc

#endif  // V8_HEAP_CPPGC_STATS_COLLECTOR_H_

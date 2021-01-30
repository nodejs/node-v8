// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/cppgc-js/cpp-heap.h"

#include "include/cppgc/heap-consistency.h"
#include "include/cppgc/platform.h"
#include "include/v8-platform.h"
#include "include/v8.h"
#include "src/base/macros.h"
#include "src/base/platform/time.h"
#include "src/execution/isolate.h"
#include "src/flags/flags.h"
#include "src/heap/base/stack.h"
#include "src/heap/cppgc-js/cpp-snapshot.h"
#include "src/heap/cppgc-js/unified-heap-marking-state.h"
#include "src/heap/cppgc-js/unified-heap-marking-verifier.h"
#include "src/heap/cppgc-js/unified-heap-marking-visitor.h"
#include "src/heap/cppgc/concurrent-marker.h"
#include "src/heap/cppgc/gc-info-table.h"
#include "src/heap/cppgc/heap-base.h"
#include "src/heap/cppgc/heap-object-header.h"
#include "src/heap/cppgc/marker.h"
#include "src/heap/cppgc/marking-state.h"
#include "src/heap/cppgc/marking-visitor.h"
#include "src/heap/cppgc/object-allocator.h"
#include "src/heap/cppgc/prefinalizer-handler.h"
#include "src/heap/cppgc/stats-collector.h"
#include "src/heap/cppgc/sweeper.h"
#include "src/heap/marking-worklist.h"
#include "src/heap/sweeper.h"
#include "src/init/v8.h"
#include "src/profiler/heap-profiler.h"

namespace v8 {

cppgc::AllocationHandle& CppHeap::GetAllocationHandle() {
  return internal::CppHeap::From(this)->object_allocator();
}

cppgc::HeapHandle& CppHeap::GetHeapHandle() {
  return *internal::CppHeap::From(this);
}

void CppHeap::Terminate() { internal::CppHeap::From(this)->Terminate(); }

void JSHeapConsistency::DijkstraMarkingBarrierSlow(
    cppgc::HeapHandle& heap_handle, const TracedReferenceBase& ref) {
  auto& heap_base = cppgc::internal::HeapBase::From(heap_handle);
  static_cast<JSVisitor*>(&heap_base.marker()->Visitor())->Trace(ref);
}

void JSHeapConsistency::CheckWrapper(v8::Local<v8::Object>& wrapper,
                                     int wrapper_index, const void* wrappable) {
  CHECK_EQ(wrappable,
           wrapper->GetAlignedPointerFromInternalField(wrapper_index));
}

namespace internal {

namespace {

class CppgcPlatformAdapter final : public cppgc::Platform {
 public:
  explicit CppgcPlatformAdapter(v8::Isolate* isolate)
      : platform_(V8::GetCurrentPlatform()), isolate_(isolate) {}

  CppgcPlatformAdapter(const CppgcPlatformAdapter&) = delete;
  CppgcPlatformAdapter& operator=(const CppgcPlatformAdapter&) = delete;

  PageAllocator* GetPageAllocator() final {
    return platform_->GetPageAllocator();
  }

  double MonotonicallyIncreasingTime() final {
    return platform_->MonotonicallyIncreasingTime();
  }

  std::shared_ptr<TaskRunner> GetForegroundTaskRunner() final {
    return platform_->GetForegroundTaskRunner(isolate_);
  }

  std::unique_ptr<JobHandle> PostJob(TaskPriority priority,
                                     std::unique_ptr<JobTask> job_task) final {
    return platform_->PostJob(priority, std::move(job_task));
  }

  TracingController* GetTracingController() override {
    return platform_->GetTracingController();
  }

 private:
  v8::Platform* platform_;
  v8::Isolate* isolate_;
};

class UnifiedHeapConcurrentMarker
    : public cppgc::internal::ConcurrentMarkerBase {
 public:
  UnifiedHeapConcurrentMarker(
      cppgc::internal::HeapBase& heap,
      cppgc::internal::MarkingWorklists& marking_worklists,
      cppgc::internal::IncrementalMarkingSchedule& incremental_marking_schedule,
      cppgc::Platform* platform,
      UnifiedHeapMarkingState& unified_heap_marking_state)
      : cppgc::internal::ConcurrentMarkerBase(
            heap, marking_worklists, incremental_marking_schedule, platform),
        unified_heap_marking_state_(unified_heap_marking_state) {}

  std::unique_ptr<cppgc::Visitor> CreateConcurrentMarkingVisitor(
      cppgc::internal::ConcurrentMarkingState&) const final;

 private:
  UnifiedHeapMarkingState& unified_heap_marking_state_;
};

std::unique_ptr<cppgc::Visitor>
UnifiedHeapConcurrentMarker::CreateConcurrentMarkingVisitor(
    cppgc::internal::ConcurrentMarkingState& marking_state) const {
  return std::make_unique<ConcurrentUnifiedHeapMarkingVisitor>(
      heap(), marking_state, unified_heap_marking_state_);
}

class UnifiedHeapMarker final : public cppgc::internal::MarkerBase {
 public:
  UnifiedHeapMarker(Key, Heap& v8_heap, cppgc::internal::HeapBase& cpp_heap,
                    cppgc::Platform* platform, MarkingConfig config);

  ~UnifiedHeapMarker() final = default;

  void AddObject(void*);

 protected:
  cppgc::Visitor& visitor() final { return marking_visitor_; }
  cppgc::internal::ConservativeTracingVisitor& conservative_visitor() final {
    return conservative_marking_visitor_;
  }
  ::heap::base::StackVisitor& stack_visitor() final {
    return conservative_marking_visitor_;
  }

 private:
  UnifiedHeapMarkingState unified_heap_marking_state_;
  MutatorUnifiedHeapMarkingVisitor marking_visitor_;
  cppgc::internal::ConservativeMarkingVisitor conservative_marking_visitor_;
};

UnifiedHeapMarker::UnifiedHeapMarker(Key key, Heap& v8_heap,
                                     cppgc::internal::HeapBase& heap,
                                     cppgc::Platform* platform,
                                     MarkingConfig config)
    : cppgc::internal::MarkerBase(key, heap, platform, config),
      unified_heap_marking_state_(v8_heap),
      marking_visitor_(heap, mutator_marking_state_,
                       unified_heap_marking_state_),
      conservative_marking_visitor_(heap, mutator_marking_state_,
                                    marking_visitor_) {
  concurrent_marker_ = std::make_unique<UnifiedHeapConcurrentMarker>(
      heap_, marking_worklists_, schedule_, platform_,
      unified_heap_marking_state_);
}

void UnifiedHeapMarker::AddObject(void* object) {
  mutator_marking_state_.MarkAndPush(
      cppgc::internal::HeapObjectHeader::FromPayload(object));
}

}  // namespace

CppHeap::CppHeap(
    v8::Isolate* isolate,
    const std::vector<std::unique_ptr<cppgc::CustomSpaceBase>>& custom_spaces,
    std::unique_ptr<cppgc::internal::MetricRecorder> metric_recorder)
    : cppgc::internal::HeapBase(std::make_shared<CppgcPlatformAdapter>(isolate),
                                custom_spaces,
                                cppgc::internal::HeapBase::StackSupport::
                                    kSupportsConservativeStackScan,
                                std::move(metric_recorder)),
      isolate_(*reinterpret_cast<Isolate*>(isolate)) {
  if (isolate_.heap_profiler()) {
    isolate_.heap_profiler()->AddBuildEmbedderGraphCallback(
        &CppGraphBuilder::Run, this);
  }
  stats_collector()->RegisterObserver(this);
}

CppHeap::~CppHeap() {
  if (isolate_.heap_profiler()) {
    isolate_.heap_profiler()->RemoveBuildEmbedderGraphCallback(
        &CppGraphBuilder::Run, this);
  }
}

void CppHeap::Terminate() {
  FinalizeIncrementalGarbageCollectionIfNeeded(
      cppgc::Heap::StackState::kNoHeapPointers);
  // Any future garbage collections will ignore the V8->C++ references.
  isolate()->SetEmbedderHeapTracer(nullptr);
  // Gracefully terminate the C++ heap invoking destructors.
  HeapBase::Terminate();
}

void CppHeap::RegisterV8References(
    const std::vector<std::pair<void*, void*> >& embedder_fields) {
  DCHECK(marker_);
  for (auto& tuple : embedder_fields) {
    // First field points to type.
    // Second field points to object.
    static_cast<UnifiedHeapMarker*>(marker_.get())->AddObject(tuple.second);
  }
  marking_done_ = false;
}

void CppHeap::TracePrologue(TraceFlags flags) {
  // Finish sweeping in case it is still running.
  sweeper_.FinishIfRunning();

  const UnifiedHeapMarker::MarkingConfig marking_config{
      UnifiedHeapMarker::MarkingConfig::CollectionType::kMajor,
      cppgc::Heap::StackState::kNoHeapPointers,
      cppgc::Heap::MarkingType::kIncrementalAndConcurrent,
      flags == TraceFlags::kForced
          ? UnifiedHeapMarker::MarkingConfig::IsForcedGC::kForced
          : UnifiedHeapMarker::MarkingConfig::IsForcedGC::kNotForced};
  if ((flags == TraceFlags::kReduceMemory) || (flags == TraceFlags::kForced)) {
    // Only enable compaction when in a memory reduction garbage collection as
    // it may significantly increase the final garbage collection pause.
    compactor_.InitializeIfShouldCompact(marking_config.marking_type,
                                         marking_config.stack_state);
  }
  marker_ =
      cppgc::internal::MarkerFactory::CreateAndStartMarking<UnifiedHeapMarker>(
          *isolate_.heap(), AsBase(), platform_.get(), marking_config);
  marking_done_ = false;
}

bool CppHeap::AdvanceTracing(double deadline_in_ms) {
  // TODO(chromium:1154636): The kAtomicMark/kIncrementalMark scope below is
  // needed for recording all cpp marking time. Note that it can lead to double
  // accounting since this scope is also accounted under an outer v8 scope.
  // Make sure to only account this scope once.
  cppgc::internal::StatsCollector::EnabledScope stats_scope(
      AsBase(), in_atomic_pause_
                    ? cppgc::internal::StatsCollector::kAtomicMark
                    : cppgc::internal::StatsCollector::kIncrementalMark);
  v8::base::TimeDelta deadline =
      in_atomic_pause_ ? v8::base::TimeDelta::Max()
                       : v8::base::TimeDelta::FromMillisecondsD(deadline_in_ms);
  // TODO(chromium:1056170): Replace when unified heap transitions to
  // bytes-based deadline.
  marking_done_ = marker_->AdvanceMarkingWithMaxDuration(deadline);
  DCHECK_IMPLIES(in_atomic_pause_, marking_done_);
  return marking_done_;
}

bool CppHeap::IsTracingDone() { return marking_done_; }

void CppHeap::EnterFinalPause(EmbedderStackState stack_state) {
  CHECK(!in_disallow_gc_scope());
  cppgc::internal::StatsCollector::EnabledScope stats_scope(
      AsBase(), cppgc::internal::StatsCollector::kAtomicMark);
  in_atomic_pause_ = true;
  marker_->EnterAtomicPause(stack_state);
  if (compactor_.CancelIfShouldNotCompact(cppgc::Heap::MarkingType::kAtomic,
                                          stack_state)) {
    marker_->NotifyCompactionCancelled();
  }
}

void CppHeap::TraceEpilogue(TraceSummary* trace_summary) {
  CHECK(in_atomic_pause_);
  CHECK(marking_done_);
  {
    cppgc::internal::StatsCollector::EnabledScope stats_scope(
        AsBase(), cppgc::internal::StatsCollector::kAtomicMark);
    cppgc::subtle::DisallowGarbageCollectionScope disallow_gc_scope(*this);
    marker_->LeaveAtomicPause();
  }
  {
    cppgc::subtle::DisallowGarbageCollectionScope disallow_gc_scope(*this);
    prefinalizer_handler()->InvokePreFinalizers();
  }
  marker_.reset();
  // TODO(chromium:1056170): replace build flag with dedicated flag.
#if DEBUG
  UnifiedHeapMarkingVerifier verifier(*this);
  verifier.Run(cppgc::Heap::StackState::kNoHeapPointers);
#endif

  {
    cppgc::subtle::NoGarbageCollectionScope no_gc(*this);
    cppgc::internal::Sweeper::SweepingConfig::CompactableSpaceHandling
        compactable_space_handling = compactor_.CompactSpacesIfEnabled();
    const cppgc::internal::Sweeper::SweepingConfig sweeping_config{
        cppgc::internal::Sweeper::SweepingConfig::SweepingType::
            kIncrementalAndConcurrent,
        compactable_space_handling};
    sweeper().Start(sweeping_config);
  }
  in_atomic_pause_ = false;
  sweeper().NotifyDoneIfNeeded();
}

void CppHeap::AllocatedObjectSizeIncreased(size_t bytes) {
  buffered_allocated_bytes_ += static_cast<int64_t>(bytes);
  ReportBufferedAllocationSizeIfPossible();
}

void CppHeap::AllocatedObjectSizeDecreased(size_t bytes) {
  buffered_allocated_bytes_ -= static_cast<int64_t>(bytes);
  ReportBufferedAllocationSizeIfPossible();
}

void CppHeap::ReportBufferedAllocationSizeIfPossible() {
  // Avoid reporting to V8 in the following conditions as that may trigger GC
  // finalizations where not allowed.
  // - Recursive sweeping.
  // - GC forbidden scope.
  if (sweeper().IsSweepingOnMutatorThread() || in_no_gc_scope()) {
    return;
  }

  if (buffered_allocated_bytes_ < 0) {
    DecreaseAllocatedSize(static_cast<size_t>(-buffered_allocated_bytes_));
  } else {
    IncreaseAllocatedSize(static_cast<size_t>(buffered_allocated_bytes_));
  }
  buffered_allocated_bytes_ = 0;
}

}  // namespace internal
}  // namespace v8

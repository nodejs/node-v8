// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <set>
#include <vector>

#include "include/cppgc/allocation.h"
#include "include/cppgc/platform.h"
#include "include/v8-platform.h"
#include "src/heap/cppgc/globals.h"
#include "src/heap/cppgc/heap-object-header-inl.h"
#include "src/heap/cppgc/heap-page.h"
#include "src/heap/cppgc/heap-space.h"
#include "src/heap/cppgc/heap-visitor.h"
#include "src/heap/cppgc/page-memory-inl.h"
#include "src/heap/cppgc/raw-heap.h"
#include "src/heap/cppgc/sweeper.h"
#include "test/unittests/heap/cppgc/test-platform.h"
#include "test/unittests/heap/cppgc/tests.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace cppgc {
namespace internal {

namespace {

class ResetLocalAllocationBufferVisitor final
    : public HeapVisitor<ResetLocalAllocationBufferVisitor> {
 public:
  bool VisitLargePageSpace(LargePageSpace*) { return true; }
  bool VisitNormalPageSpace(NormalPageSpace* space) {
    space->ResetLinearAllocationBuffer();
    return true;
  }
};

void ResetLocalAllocationBuffers(Heap* heap) {
  ResetLocalAllocationBufferVisitor visitor;
  visitor.Traverse(&heap->raw_heap());
}

size_t g_destructor_callcount;

template <size_t Size>
class Finalizable : public GarbageCollected<Finalizable<Size>> {
 public:
  Finalizable() : creation_thread_{v8::base::OS::GetCurrentThreadId()} {}

  virtual ~Finalizable() {
    ++g_destructor_callcount;
    EXPECT_EQ(creation_thread_, v8::base::OS::GetCurrentThreadId());
  }

  virtual void Trace(cppgc::Visitor*) const {}

 private:
  char array_[Size];
  int creation_thread_;
};

using NormalFinalizable = Finalizable<32>;
using LargeFinalizable = Finalizable<kLargeObjectSizeThreshold * 2>;

template <size_t Size>
class NonFinalizable : public GarbageCollected<NonFinalizable<Size>> {
 public:
  virtual void Trace(cppgc::Visitor*) const {}

 private:
  char array_[Size];
};

using NormalNonFinalizable = NonFinalizable<32>;
using LargeNonFinalizable = NonFinalizable<kLargeObjectSizeThreshold * 2>;

class ConcurrentSweeperTest : public testing::TestWithHeap {
 public:
  ConcurrentSweeperTest() { g_destructor_callcount = 0; }

  void StartSweeping() {
    Heap* heap = Heap::From(GetHeap());
    ResetLocalAllocationBuffers(heap);
    Sweeper& sweeper = heap->sweeper();
    sweeper.Start(Sweeper::Config::kIncrementalAndConcurrent);
  }

  void FinishSweeping() {
    Heap* heap = Heap::From(GetHeap());
    Sweeper& sweeper = heap->sweeper();
    sweeper.Finish();
  }

  const RawHeap& GetRawHeap() const {
    const Heap* heap = Heap::From(GetHeap());
    return heap->raw_heap();
  }

  void CheckFreeListEntries(const std::vector<void*>& objects) {
    const Heap* heap = Heap::From(GetHeap());
    const PageBackend* backend = heap->page_backend();

    for (auto* object : objects) {
      // The corresponding page could be removed.
      if (!backend->Lookup(static_cast<ConstAddress>(object))) continue;

      const auto* header =
          BasePage::FromPayload(object)->TryObjectHeaderFromInnerAddress(
              object);
      // TryObjectHeaderFromInnerAddress returns nullptr for freelist entries.
      EXPECT_EQ(nullptr, header);
    }
  }

  void CheckPageRemoved(const BasePage* page) {
    const Heap* heap = Heap::From(GetHeap());
    const PageBackend* backend = heap->page_backend();
    EXPECT_EQ(nullptr, backend->Lookup(reinterpret_cast<ConstAddress>(page)));
  }

  bool FreeListContains(const BaseSpace* space,
                        const std::vector<void*>& objects) {
    const Heap* heap = Heap::From(GetHeap());
    const PageBackend* backend = heap->page_backend();
    const auto& freelist = NormalPageSpace::From(space)->free_list();

    for (void* object : objects) {
      // The corresponding page could be removed.
      if (!backend->Lookup(static_cast<ConstAddress>(object))) continue;

      if (!freelist.Contains({object, 0})) return false;
    }

    return true;
  }
};

}  // namespace

TEST_F(ConcurrentSweeperTest, BackgroundSweepOfNormalPage) {
  // Non finalizable objects are swept right away.
  using GCedType = NormalNonFinalizable;

  auto* unmarked_object = MakeGarbageCollected<GCedType>(GetHeap());
  auto* marked_object = MakeGarbageCollected<GCedType>(GetHeap());
  HeapObjectHeader::FromPayload(marked_object).TryMarkAtomic();

  auto* page = BasePage::FromPayload(unmarked_object);
  auto* space = page->space();

  // The test requires objects to be allocated on the same page;
  ASSERT_EQ(page, BasePage::FromPayload(marked_object));

  StartSweeping();

  // Wait for concurrent sweeping to finish.
  GetPlatform().WaitAllBackgroundTasks();

  // Check that the marked object was unmarked.
  EXPECT_FALSE(HeapObjectHeader::FromPayload(marked_object).IsMarked());

  // Check that free list entries are created right away for non-finalizable
  // objects, but not immediately returned to the space's freelist.
  CheckFreeListEntries({unmarked_object});
  EXPECT_FALSE(FreeListContains(space, {unmarked_object}));

  FinishSweeping();

  // Check that finalizable objects are swept and put into the freelist of the
  // corresponding space.
  EXPECT_TRUE(FreeListContains(space, {unmarked_object}));
}

TEST_F(ConcurrentSweeperTest, BackgroundSweepOfLargePage) {
  // Non finalizable objects are swept right away.
  using GCedType = LargeNonFinalizable;

  auto* unmarked_object = MakeGarbageCollected<GCedType>(GetHeap());
  auto* marked_object = MakeGarbageCollected<GCedType>(GetHeap());
  HeapObjectHeader::FromPayload(marked_object).TryMarkAtomic();

  auto* unmarked_page = BasePage::FromPayload(unmarked_object);
  auto* marked_page = BasePage::FromPayload(marked_object);
  auto* space = unmarked_page->space();

  ASSERT_EQ(space, marked_page->space());

  StartSweeping();

  // Wait for concurrent sweeping to finish.
  GetPlatform().WaitAllBackgroundTasks();

  // Check that the marked object was unmarked.
  EXPECT_FALSE(HeapObjectHeader::FromPayload(marked_object).IsMarked());

  // Check that free list entries are created right away for non-finalizable
  // objects, but not immediately returned to the space's freelist.
  CheckPageRemoved(unmarked_page);

  // Check that marked pages are returned to space right away.
  EXPECT_NE(space->end(), std::find(space->begin(), space->end(), marked_page));

  FinishSweeping();
}

TEST_F(ConcurrentSweeperTest, DeferredFinalizationOfNormalPage) {
  static constexpr size_t kNumberOfObjects = 10;
  // Finalizable types are left intact by concurrent sweeper.
  using GCedType = NormalFinalizable;

  std::set<BasePage*> pages;
  std::vector<void*> objects;

  BaseSpace* space = nullptr;
  for (size_t i = 0; i < kNumberOfObjects; ++i) {
    auto* object = MakeGarbageCollected<GCedType>(GetHeap());
    objects.push_back(object);
    auto* page = BasePage::FromPayload(object);
    pages.insert(page);
    if (!space) space = page->space();
  }

  StartSweeping();

  // Wait for concurrent sweeping to finish.
  GetPlatform().WaitAllBackgroundTasks();

  // Check that pages are not returned right away.
  for (auto* page : pages) {
    EXPECT_EQ(space->end(), std::find(space->begin(), space->end(), page));
  }
  // Check that finalizable objects are left intact in pages.
  EXPECT_FALSE(FreeListContains(space, objects));
  // No finalizers have been executed.
  EXPECT_EQ(0u, g_destructor_callcount);

  FinishSweeping();

  // Check that finalizable objects are swept and turned into freelist entries.
  CheckFreeListEntries(objects);
  // Check that space's freelist contains these entries.
  EXPECT_TRUE(FreeListContains(space, objects));
  // Check that finalizers have been executed.
  EXPECT_EQ(kNumberOfObjects, g_destructor_callcount);
}

TEST_F(ConcurrentSweeperTest, DeferredFinalizationOfLargePage) {
  using GCedType = LargeFinalizable;

  auto* object = MakeGarbageCollected<GCedType>(GetHeap());

  auto* page = BasePage::FromPayload(object);
  auto* space = page->space();

  StartSweeping();

  // Wait for concurrent sweeping to finish.
  GetPlatform().WaitAllBackgroundTasks();

  // Check that the page is not returned to the space.
  EXPECT_EQ(space->end(), std::find(space->begin(), space->end(), page));
  // Check that no destructors have been executed yet.
  EXPECT_EQ(0u, g_destructor_callcount);

  FinishSweeping();

  // Check that the destructor was executed.
  EXPECT_EQ(1u, g_destructor_callcount);
  // Check that page was unmapped.
  CheckPageRemoved(page);
}

TEST_F(ConcurrentSweeperTest, IncrementalSweeping) {
  testing::TestPlatform::DisableBackgroundTasksScope disable_concurrent_sweeper(
      &GetPlatform());

  auto task_runner = GetPlatform().GetForegroundTaskRunner();

  // Create two unmarked objects.
  MakeGarbageCollected<NormalFinalizable>(GetHeap());
  MakeGarbageCollected<LargeFinalizable>(GetHeap());

  // Create two marked objects.
  auto* marked_normal_object =
      MakeGarbageCollected<NormalFinalizable>(GetHeap());
  auto* marked_large_object = MakeGarbageCollected<LargeFinalizable>(GetHeap());

  auto& marked_normal_header =
      HeapObjectHeader::FromPayload(marked_normal_object);
  auto& marked_large_header =
      HeapObjectHeader::FromPayload(marked_large_object);

  marked_normal_header.TryMarkAtomic();
  marked_large_header.TryMarkAtomic();

  StartSweeping();

  EXPECT_EQ(0u, g_destructor_callcount);
  EXPECT_TRUE(marked_normal_header.IsMarked());
  EXPECT_TRUE(marked_large_header.IsMarked());

  // Wait for incremental sweeper to finish.
  GetPlatform().WaitAllForegroundTasks();

  EXPECT_EQ(2u, g_destructor_callcount);
  EXPECT_FALSE(marked_normal_header.IsMarked());
  EXPECT_FALSE(marked_large_header.IsMarked());

  FinishSweeping();
}

}  // namespace internal
}  // namespace cppgc

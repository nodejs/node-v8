// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/concurrent-allocator.h"

#include "src/heap/concurrent-allocator-inl.h"
#include "src/heap/local-heap.h"
#include "src/heap/marking.h"

namespace v8 {
namespace internal {

Address ConcurrentAllocator::PerformCollectionAndAllocateAgain(
    int object_size, AllocationAlignment alignment, AllocationOrigin origin) {
  Heap* heap = local_heap_->heap();
  local_heap_->allocation_failed_ = true;

  for (int i = 0; i < 3; i++) {
    {
      ParkedScope scope(local_heap_);
      heap->RequestAndWaitForCollection();
    }

    AllocationResult result = Allocate(object_size, alignment, origin);
    if (!result.IsRetry()) {
      local_heap_->allocation_failed_ = false;
      return result.ToObjectChecked().address();
    }
  }

  heap->FatalProcessOutOfMemory("ConcurrentAllocator: allocation failed");
}

void ConcurrentAllocator::FreeLinearAllocationArea() {
  lab_.CloseAndMakeIterable();
}

void ConcurrentAllocator::MakeLinearAllocationAreaIterable() {
  lab_.MakeIterable();
}

void ConcurrentAllocator::MarkLinearAllocationAreaBlack() {
  Address top = lab_.top();
  Address limit = lab_.limit();

  if (top != kNullAddress && top != limit) {
    Page::FromAllocationAreaAddress(top)->CreateBlackAreaBackground(top, limit);
  }
}

void ConcurrentAllocator::UnmarkLinearAllocationArea() {
  Address top = lab_.top();
  Address limit = lab_.limit();

  if (top != kNullAddress && top != limit) {
    Page::FromAllocationAreaAddress(top)->DestroyBlackAreaBackground(top,
                                                                     limit);
  }
}

AllocationResult ConcurrentAllocator::AllocateOutsideLab(
    int object_size, AllocationAlignment alignment, AllocationOrigin origin) {
  auto result = space_->SlowGetLinearAllocationAreaBackground(
      local_heap_, object_size, object_size, alignment, origin);

  if (result) {
    HeapObject object = HeapObject::FromAddress(result->first);

    if (local_heap_->heap()->incremental_marking()->black_allocation()) {
      local_heap_->heap()->incremental_marking()->MarkBlackBackground(
          object, object_size);
    }

    return AllocationResult(object);
  } else {
    return AllocationResult::Retry(OLD_SPACE);
  }
}

}  // namespace internal
}  // namespace v8

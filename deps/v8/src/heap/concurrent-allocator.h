// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_CONCURRENT_ALLOCATOR_H_
#define V8_HEAP_CONCURRENT_ALLOCATOR_H_

#include "src/common/globals.h"
#include "src/heap/heap.h"
#include "src/heap/spaces.h"

namespace v8 {
namespace internal {

class LocalHeap;

// Concurrent allocator for allocation from background threads/tasks.
// Allocations are served from a TLAB if possible.
class ConcurrentAllocator {
 public:
  static const int kLabSize = 4 * KB;
  static const int kMaxLabSize = 32 * KB;
  static const int kMaxLabObjectSize = 2 * KB;

  explicit ConcurrentAllocator(LocalHeap* local_heap, PagedSpace* space)
      : local_heap_(local_heap),
        space_(space),
        lab_(LocalAllocationBuffer::InvalidBuffer()) {}

  inline AllocationResult Allocate(int object_size,
                                   AllocationAlignment alignment,
                                   AllocationOrigin origin);

  inline Address AllocateOrFail(int object_size, AllocationAlignment alignment,
                                AllocationOrigin origin);

  void FreeLinearAllocationArea();
  void MakeLinearAllocationAreaIterable();
  void MarkLinearAllocationAreaBlack();
  void UnmarkLinearAllocationArea();

 private:
  inline bool EnsureLab(AllocationOrigin origin);
  inline AllocationResult AllocateInLab(int object_size,
                                        AllocationAlignment alignment,
                                        AllocationOrigin origin);

  V8_EXPORT_PRIVATE AllocationResult AllocateOutsideLab(
      int object_size, AllocationAlignment alignment, AllocationOrigin origin);

  V8_EXPORT_PRIVATE Address PerformCollectionAndAllocateAgain(
      int object_size, AllocationAlignment alignment, AllocationOrigin origin);

  LocalHeap* const local_heap_;
  PagedSpace* const space_;
  LocalAllocationBuffer lab_;
};

}  // namespace internal
}  // namespace v8

#endif  // V8_HEAP_CONCURRENT_ALLOCATOR_H_

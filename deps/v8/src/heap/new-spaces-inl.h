// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_NEW_SPACES_INL_H_
#define V8_HEAP_NEW_SPACES_INL_H_

#include "src/heap/new-spaces.h"
#include "src/heap/spaces-inl.h"
#include "src/objects/tagged-impl.h"
#include "src/sanitizer/msan.h"

namespace v8 {
namespace internal {

// -----------------------------------------------------------------------------
// SemiSpace

bool SemiSpace::Contains(HeapObject o) const {
  MemoryChunk* memory_chunk = MemoryChunk::FromHeapObject(o);
  if (memory_chunk->IsLargePage()) return false;
  return id_ == kToSpace ? memory_chunk->IsToPage()
                         : memory_chunk->IsFromPage();
}

bool SemiSpace::Contains(Object o) const {
  return o.IsHeapObject() && Contains(HeapObject::cast(o));
}

bool SemiSpace::ContainsSlow(Address a) const {
  for (const Page* p : *this) {
    if (p == MemoryChunk::FromAddress(a)) return true;
  }
  return false;
}

// --------------------------------------------------------------------------
// NewSpace

bool NewSpace::Contains(Object o) const {
  return o.IsHeapObject() && Contains(HeapObject::cast(o));
}

bool NewSpace::Contains(HeapObject o) const {
  return MemoryChunk::FromHeapObject(o)->InNewSpace();
}

bool NewSpace::ContainsSlow(Address a) const {
  return from_space_.ContainsSlow(a) || to_space_.ContainsSlow(a);
}

bool NewSpace::ToSpaceContainsSlow(Address a) const {
  return to_space_.ContainsSlow(a);
}

bool NewSpace::ToSpaceContains(Object o) const { return to_space_.Contains(o); }
bool NewSpace::FromSpaceContains(Object o) const {
  return from_space_.Contains(o);
}

// -----------------------------------------------------------------------------
// SemiSpaceObjectIterator

HeapObject SemiSpaceObjectIterator::Next() {
  while (current_ != limit_) {
    if (Page::IsAlignedToPageSize(current_)) {
      Page* page = Page::FromAllocationAreaAddress(current_);
      page = page->next_page();
      DCHECK(page);
      current_ = page->area_start();
      if (current_ == limit_) return HeapObject();
    }
    HeapObject object = HeapObject::FromAddress(current_);
    current_ += object.Size();
    if (!object.IsFreeSpaceOrFiller()) {
      return object;
    }
  }
  return HeapObject();
}

// -----------------------------------------------------------------------------
// NewSpace

AllocationResult NewSpace::AllocateRawAligned(int size_in_bytes,
                                              AllocationAlignment alignment,
                                              AllocationOrigin origin) {
  Address top = allocation_info_.top();
  int filler_size = Heap::GetFillToAlign(top, alignment);
  int aligned_size_in_bytes = size_in_bytes + filler_size;

  if (allocation_info_.limit() - top <
      static_cast<uintptr_t>(aligned_size_in_bytes)) {
    // See if we can create room.
    if (!EnsureAllocation(size_in_bytes, alignment)) {
      return AllocationResult::Retry();
    }

    top = allocation_info_.top();
    filler_size = Heap::GetFillToAlign(top, alignment);
    aligned_size_in_bytes = size_in_bytes + filler_size;
  }

  HeapObject obj = HeapObject::FromAddress(top);
  allocation_info_.set_top(top + aligned_size_in_bytes);
  DCHECK_SEMISPACE_ALLOCATION_INFO(allocation_info_, to_space_);

  if (filler_size > 0) {
    obj = Heap::PrecedeWithFiller(ReadOnlyRoots(heap()), obj, filler_size);
  }

  MSAN_ALLOCATED_UNINITIALIZED_MEMORY(obj.address(), size_in_bytes);

  if (FLAG_trace_allocations_origins) {
    UpdateAllocationOrigins(origin);
  }

  return obj;
}

AllocationResult NewSpace::AllocateRawUnaligned(int size_in_bytes,
                                                AllocationOrigin origin) {
  Address top = allocation_info_.top();
  if (allocation_info_.limit() < top + size_in_bytes) {
    // See if we can create room.
    if (!EnsureAllocation(size_in_bytes, kWordAligned)) {
      return AllocationResult::Retry();
    }

    top = allocation_info_.top();
  }

  HeapObject obj = HeapObject::FromAddress(top);
  allocation_info_.set_top(top + size_in_bytes);
  DCHECK_SEMISPACE_ALLOCATION_INFO(allocation_info_, to_space_);

  MSAN_ALLOCATED_UNINITIALIZED_MEMORY(obj.address(), size_in_bytes);

  if (FLAG_trace_allocations_origins) {
    UpdateAllocationOrigins(origin);
  }

  return obj;
}

AllocationResult NewSpace::AllocateRaw(int size_in_bytes,
                                       AllocationAlignment alignment,
                                       AllocationOrigin origin) {
  if (top() < top_on_previous_step_) {
    // Generated code decreased the top() pointer to do folded allocations
    DCHECK_EQ(Page::FromAllocationAreaAddress(top()),
              Page::FromAllocationAreaAddress(top_on_previous_step_));
    top_on_previous_step_ = top();
  }
#ifdef V8_HOST_ARCH_32_BIT
  return alignment != kWordAligned
             ? AllocateRawAligned(size_in_bytes, alignment, origin)
             : AllocateRawUnaligned(size_in_bytes, origin);
#else
#ifdef V8_COMPRESS_POINTERS
  // TODO(ishell, v8:8875): Consider using aligned allocations once the
  // allocation alignment inconsistency is fixed. For now we keep using
  // unaligned access since both x64 and arm64 architectures (where pointer
  // compression is supported) allow unaligned access to doubles and full words.
#endif  // V8_COMPRESS_POINTERS
  return AllocateRawUnaligned(size_in_bytes, origin);
#endif
}

V8_WARN_UNUSED_RESULT inline AllocationResult NewSpace::AllocateRawSynchronized(
    int size_in_bytes, AllocationAlignment alignment, AllocationOrigin origin) {
  base::MutexGuard guard(&mutex_);
  return AllocateRaw(size_in_bytes, alignment, origin);
}

}  // namespace internal
}  // namespace v8

#endif  // V8_HEAP_NEW_SPACES_INL_H_

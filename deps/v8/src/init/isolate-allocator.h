// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_INIT_ISOLATE_ALLOCATOR_H_
#define V8_INIT_ISOLATE_ALLOCATOR_H_

#include <memory>

#include "src/base/bounded-page-allocator.h"
#include "src/base/page-allocator.h"
#include "src/common/globals.h"
#include "src/utils/allocation.h"

namespace v8 {

// Forward declarations.
namespace base {
class BoundedPageAllocator;
}  // namespace base

namespace internal {

// IsolateAllocator object is responsible for allocating memory for one (!)
// Isolate object. Depending on the whether pointer compression is enabled,
// the memory can be allocated
// 1) in the C++ heap (when pointer compression is disabled)
// 2) in a proper part of a properly aligned region of a reserved address space
//   (when pointer compression is enabled).
//
// Isolate::New() first creates IsolateAllocator object which allocates the
// memory and then it constructs Isolate object in this memory. Once it's done
// the Isolate object takes ownership of the IsolateAllocator object to keep
// the memory alive.
// Isolate::Delete() takes care of the proper order of the objects destruction.
class V8_EXPORT_PRIVATE IsolateAllocator final {
 public:
  IsolateAllocator();
  ~IsolateAllocator();
  IsolateAllocator(const IsolateAllocator&) = delete;
  IsolateAllocator& operator=(const IsolateAllocator&) = delete;

  void* isolate_memory() const { return isolate_memory_; }

  v8::PageAllocator* page_allocator() const { return page_allocator_; }

 private:
  Address InitReservation();
  void CommitPagesForIsolate(Address heap_reservation_address);

  // The allocated memory for Isolate instance.
  void* isolate_memory_ = nullptr;
  v8::PageAllocator* page_allocator_ = nullptr;
  std::unique_ptr<base::BoundedPageAllocator> page_allocator_instance_;
  VirtualMemory reservation_;
};

}  // namespace internal
}  // namespace v8

#endif  // V8_INIT_ISOLATE_ALLOCATOR_H_

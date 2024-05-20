// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_UNITTESTS_HEAP_CPPGC_JS_UNIFIED_HEAP_UTILS_H_
#define V8_UNITTESTS_HEAP_CPPGC_JS_UNIFIED_HEAP_UTILS_H_

#include "include/cppgc/heap.h"
#include "include/v8-cppgc.h"
#include "include/v8-local-handle.h"
#include "src/objects/js-objects.h"
#include "test/unittests/heap/heap-utils.h"

namespace v8 {

class CppHeap;

namespace internal {

class CppHeap;

class UnifiedHeapTest : public TestWithHeapInternalsAndContext {
 public:
  UnifiedHeapTest();
  explicit UnifiedHeapTest(
      std::vector<std::unique_ptr<cppgc::CustomSpaceBase>>);
  ~UnifiedHeapTest() override = default;

  void CollectGarbageWithEmbedderStack(cppgc::Heap::SweepingType sweeping_type =
                                           cppgc::Heap::SweepingType::kAtomic);
  void CollectGarbageWithoutEmbedderStack(
      cppgc::Heap::SweepingType sweeping_type =
          cppgc::Heap::SweepingType::kAtomic);

  void CollectYoungGarbageWithEmbedderStack(
      cppgc::Heap::SweepingType sweeping_type =
          cppgc::Heap::SweepingType::kAtomic);
  void CollectYoungGarbageWithoutEmbedderStack(
      cppgc::Heap::SweepingType sweeping_type =
          cppgc::Heap::SweepingType::kAtomic);

  CppHeap& cpp_heap() const;
  cppgc::AllocationHandle& allocation_handle();

 private:
  std::unique_ptr<v8::CppHeap> cpp_heap_;
};

// Helpers for the deprecated traditional-style wrappers using embedder fields.
class DeprecatedWrapperHelper {
 public:
  static constexpr size_t kWrappableTypeEmbedderIndex = 0;
  static constexpr size_t kWrappableInstanceEmbedderIndex = 1;
  // Id that identifies types that should be traced.
  static constexpr uint16_t kTracedEmbedderId = uint16_t{0xA50F};

  static constexpr WrapperDescriptor DefaultWrapperDescriptor() {
    START_ALLOW_USE_DEPRECATED()
    return WrapperDescriptor(kWrappableTypeEmbedderIndex,
                             kWrappableInstanceEmbedderIndex,
                             kTracedEmbedderId);
    END_ALLOW_USE_DEPRECATED()
  }

  // Sets up a V8 API object so that it points back to a C++ object. The setup
  // used is recognized by the GC and references will be followed for liveness
  // analysis (marking) as well as tooling (snapshot).
  static v8::Local<v8::Object> CreateWrapper(v8::Local<v8::Context> context,
                                             void* wrappable_type,
                                             void* wrappable_object,
                                             const char* class_name = "");

  // Resets the connection of a wrapper (JS) to its wrappable (C++), meaning
  // that the wrappable object is not longer kept alive by the wrapper object.
  static void ResetWrappableConnection(v8::Local<v8::Object> api_object);

  // Sets up the connection of a wrapper (JS) to its wrappable (C++). Does not
  // emit any possibly needed write barrier.
  static void SetWrappableConnection(v8::Local<v8::Object> api_object, void*,
                                     void*);

  template <typename T>
  static T* UnwrapAs(v8::Local<v8::Object> api_object) {
    return reinterpret_cast<T*>(api_object->GetAlignedPointerFromInternalField(
        kWrappableInstanceEmbedderIndex));
  }
};

// Helpers for managed wrappers using a single header field.
class WrapperHelper {
 public:
  // Sets up a V8 API object so that it points back to a C++ object. The setup
  // used is recognized by the GC and references will be followed for liveness
  // analysis (marking) as well as tooling (snapshot).
  static v8::Local<v8::Object> CreateWrapper(v8::Local<v8::Context> context,
                                             void* wrappable_object,
                                             const char* class_name = nullptr);

  // Resets the connection of a wrapper (JS) to its wrappable (C++), meaning
  // that the wrappable object is not longer kept alive by the wrapper object.
  static void ResetWrappableConnection(v8::Isolate* isolate,
                                       v8::Local<v8::Object> api_object);

  // Sets up the connection of a wrapper (JS) to its wrappable (C++). Does not
  // emit any possibly needed write barrier.
  static void SetWrappableConnection(v8::Isolate* isolate,
                                     v8::Local<v8::Object> api_object, void*);

  template <typename T>
  static T* UnwrapAs(v8::Isolate* isolate, v8::Local<v8::Object> api_object) {
    return reinterpret_cast<T*>(ReadWrappablePointer(isolate, api_object));
  }

 private:
  static void* ReadWrappablePointer(v8::Isolate* isolate,
                                    v8::Local<v8::Object> api_object);
};

}  // namespace internal
}  // namespace v8

#endif  // V8_UNITTESTS_HEAP_CPPGC_JS_UNIFIED_HEAP_UTILS_H_

// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test/unittests/heap/cppgc-js/unified-heap-utils.h"

#include "include/cppgc/platform.h"
#include "include/v8-cppgc.h"
#include "include/v8-function.h"
#include "src/api/api-inl.h"
#include "src/heap/cppgc-js/cpp-heap.h"
#include "src/heap/heap.h"
#include "src/objects/js-objects.h"
#include "src/objects/objects-inl.h"
#include "test/unittests/heap/heap-utils.h"

namespace v8 {
namespace internal {

UnifiedHeapTest::UnifiedHeapTest()
    : UnifiedHeapTest(std::vector<std::unique_ptr<cppgc::CustomSpaceBase>>()) {}

START_ALLOW_USE_DEPRECATED()
UnifiedHeapTest::UnifiedHeapTest(
    std::vector<std::unique_ptr<cppgc::CustomSpaceBase>> custom_spaces)
    : cpp_heap_(v8::CppHeap::Create(
          V8::GetCurrentPlatform(),
          CppHeapCreateParams{
              std::move(custom_spaces),
              // TODO(338411141): Switch to the non-deprecated setup when
              // removing DeprecatedWrapperHelper and all the tests prefixed
              // with "Deprectated_".
              DeprecatedWrapperHelper::DefaultWrapperDescriptor()})) {
  // --stress-incremental-marking may have started an incremental GC at this
  // point already.
  InvokeAtomicMajorGC();
  isolate()->heap()->AttachCppHeap(cpp_heap_.get());
}
END_ALLOW_USE_DEPRECATED()

void UnifiedHeapTest::CollectGarbageWithEmbedderStack(
    cppgc::Heap::SweepingType sweeping_type) {
  EmbedderStackStateScope stack_scope(
      heap(), EmbedderStackStateOrigin::kExplicitInvocation,
      StackState::kMayContainHeapPointers);
  InvokeMajorGC();
  if (sweeping_type == cppgc::Heap::SweepingType::kAtomic) {
    cpp_heap().AsBase().sweeper().FinishIfRunning();
  }
}

void UnifiedHeapTest::CollectGarbageWithoutEmbedderStack(
    cppgc::Heap::SweepingType sweeping_type) {
  EmbedderStackStateScope stack_scope(
      heap(), EmbedderStackStateOrigin::kExplicitInvocation,
      StackState::kNoHeapPointers);
  InvokeMajorGC();
  if (sweeping_type == cppgc::Heap::SweepingType::kAtomic) {
    cpp_heap().AsBase().sweeper().FinishIfRunning();
  }
}

void UnifiedHeapTest::CollectYoungGarbageWithEmbedderStack(
    cppgc::Heap::SweepingType sweeping_type) {
  EmbedderStackStateScope stack_scope(
      heap(), EmbedderStackStateOrigin::kExplicitInvocation,
      StackState::kMayContainHeapPointers);
  InvokeMinorGC();
  if (sweeping_type == cppgc::Heap::SweepingType::kAtomic) {
    cpp_heap().AsBase().sweeper().FinishIfRunning();
  }
}
void UnifiedHeapTest::CollectYoungGarbageWithoutEmbedderStack(
    cppgc::Heap::SweepingType sweeping_type) {
  EmbedderStackStateScope stack_scope(
      heap(), EmbedderStackStateOrigin::kExplicitInvocation,
      StackState::kNoHeapPointers);
  InvokeMinorGC();
  if (sweeping_type == cppgc::Heap::SweepingType::kAtomic) {
    cpp_heap().AsBase().sweeper().FinishIfRunning();
  }
}

CppHeap& UnifiedHeapTest::cpp_heap() const {
  return *CppHeap::From(isolate()->heap()->cpp_heap());
}

cppgc::AllocationHandle& UnifiedHeapTest::allocation_handle() {
  return cpp_heap().object_allocator();
}

// static
v8::Local<v8::Object> DeprecatedWrapperHelper::CreateWrapper(
    v8::Local<v8::Context> context, void* wrappable_type,
    void* wrappable_object, const char* class_name) {
  v8::EscapableHandleScope scope(context->GetIsolate());
  v8::Local<v8::FunctionTemplate> function_t =
      v8::FunctionTemplate::New(context->GetIsolate());
  if (strlen(class_name) != 0) {
    function_t->SetClassName(
        v8::String::NewFromUtf8(v8::Isolate::GetCurrent(), class_name)
            .ToLocalChecked());
  }
  v8::Local<v8::ObjectTemplate> instance_t = function_t->InstanceTemplate();
  instance_t->SetInternalFieldCount(2);
  v8::Local<v8::Function> function =
      function_t->GetFunction(context).ToLocalChecked();
  v8::Local<v8::Object> instance =
      function->NewInstance(context).ToLocalChecked();
  SetWrappableConnection(instance, wrappable_type, wrappable_object);
  CHECK(!instance.IsEmpty());
  i::Handle<i::JSReceiver> js_obj = v8::Utils::OpenHandle(*instance);
  CHECK_EQ(i::JS_API_OBJECT_TYPE, js_obj->map()->instance_type());
  return scope.Escape(instance);
}

// static
void DeprecatedWrapperHelper::ResetWrappableConnection(
    v8::Local<v8::Object> api_object) {
  api_object->SetAlignedPointerInInternalField(kWrappableTypeEmbedderIndex,
                                               nullptr);
  api_object->SetAlignedPointerInInternalField(kWrappableInstanceEmbedderIndex,
                                               nullptr);
}

// static
void DeprecatedWrapperHelper::SetWrappableConnection(
    v8::Local<v8::Object> api_object, void* type, void* instance) {
  api_object->SetAlignedPointerInInternalField(kWrappableTypeEmbedderIndex,
                                               type);
  api_object->SetAlignedPointerInInternalField(kWrappableInstanceEmbedderIndex,
                                               instance);
}

// static
v8::Local<v8::Object> WrapperHelper::CreateWrapper(
    v8::Local<v8::Context> context, void* wrappable_object,
    const char* class_name) {
  v8::EscapableHandleScope scope(context->GetIsolate());
  v8::Local<v8::FunctionTemplate> function_t =
      v8::FunctionTemplate::New(context->GetIsolate());
  if (class_name && strlen(class_name) != 0) {
    function_t->SetClassName(
        v8::String::NewFromUtf8(v8::Isolate::GetCurrent(), class_name)
            .ToLocalChecked());
  }
  v8::Local<v8::Function> function =
      function_t->GetFunction(context).ToLocalChecked();
  v8::Local<v8::Object> instance =
      function->NewInstance(context).ToLocalChecked();
  SetWrappableConnection(context->GetIsolate(), instance, wrappable_object);
  CHECK(!instance.IsEmpty());
  CHECK_EQ(wrappable_object,
           ReadWrappablePointer(context->GetIsolate(), instance));
  i::Handle<i::JSReceiver> js_obj = v8::Utils::OpenHandle(*instance);
  CHECK_EQ(i::JS_API_OBJECT_TYPE, js_obj->map()->instance_type());
  return scope.Escape(instance);
}

// static
void WrapperHelper::ResetWrappableConnection(v8::Isolate* isolate,
                                             v8::Local<v8::Object> api_object) {
  i::Handle<i::JSReceiver> js_obj = v8::Utils::OpenHandle(*api_object);
  JSApiWrapper(JSObject::cast(*js_obj))
      .SetCppHeapWrappable<kExternalObjectValueTag>(
          reinterpret_cast<i::Isolate*>(isolate), nullptr);
}

// static
void WrapperHelper::SetWrappableConnection(v8::Isolate* isolate,
                                           v8::Local<v8::Object> api_object,
                                           void* instance) {
  i::Handle<i::JSReceiver> js_obj = v8::Utils::OpenHandle(*api_object);
  JSApiWrapper(JSObject::cast(*js_obj))
      .SetCppHeapWrappable<kExternalObjectValueTag>(
          reinterpret_cast<i::Isolate*>(isolate), instance);
}

// static
void* WrapperHelper::ReadWrappablePointer(v8::Isolate* isolate,
                                          v8::Local<v8::Object> api_object) {
  i::Handle<i::JSReceiver> js_obj = v8::Utils::OpenHandle(*api_object);
  return JSApiWrapper(JSObject::cast(*js_obj))
      .GetCppHeapWrappable<kExternalObjectValueTag>(
          reinterpret_cast<i::Isolate*>(isolate));
}

}  // namespace internal
}  // namespace v8

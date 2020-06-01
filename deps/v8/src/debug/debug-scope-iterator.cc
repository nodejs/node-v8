// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/debug/debug-scope-iterator.h"

#include "src/api/api-inl.h"
#include "src/debug/debug.h"
#include "src/debug/liveedit.h"
#include "src/execution/frames-inl.h"
#include "src/execution/isolate.h"
#include "src/objects/js-generator-inl.h"
#include "src/wasm/wasm-debug.h"
#include "src/wasm/wasm-objects-inl.h"

namespace v8 {

std::unique_ptr<debug::ScopeIterator> debug::ScopeIterator::CreateForFunction(
    v8::Isolate* v8_isolate, v8::Local<v8::Function> v8_func) {
  internal::Handle<internal::JSReceiver> receiver =
      internal::Handle<internal::JSReceiver>::cast(Utils::OpenHandle(*v8_func));

  // Besides JSFunction and JSBoundFunction, {v8_func} could be an
  // ObjectTemplate with a CallAsFunctionHandler. We only handle plain
  // JSFunctions.
  if (!receiver->IsJSFunction()) return nullptr;

  internal::Handle<internal::JSFunction> function =
      internal::Handle<internal::JSFunction>::cast(receiver);

  // Blink has function objects with callable map, JS_SPECIAL_API_OBJECT_TYPE
  // but without context on heap.
  if (!function->has_context()) return nullptr;
  return std::unique_ptr<debug::ScopeIterator>(new internal::DebugScopeIterator(
      reinterpret_cast<internal::Isolate*>(v8_isolate), function));
}

std::unique_ptr<debug::ScopeIterator>
debug::ScopeIterator::CreateForGeneratorObject(
    v8::Isolate* v8_isolate, v8::Local<v8::Object> v8_generator) {
  internal::Handle<internal::Object> generator =
      Utils::OpenHandle(*v8_generator);
  DCHECK(generator->IsJSGeneratorObject());
  return std::unique_ptr<debug::ScopeIterator>(new internal::DebugScopeIterator(
      reinterpret_cast<internal::Isolate*>(v8_isolate),
      internal::Handle<internal::JSGeneratorObject>::cast(generator)));
}

namespace internal {

DebugScopeIterator::DebugScopeIterator(Isolate* isolate,
                                       FrameInspector* frame_inspector)
    : iterator_(
          isolate, frame_inspector,
          ::v8::internal::ScopeIterator::ReparseStrategy::kFunctionLiteral) {
  if (!Done() && ShouldIgnore()) Advance();
}

DebugScopeIterator::DebugScopeIterator(Isolate* isolate,
                                       Handle<JSFunction> function)
    : iterator_(isolate, function) {
  if (!Done() && ShouldIgnore()) Advance();
}

DebugScopeIterator::DebugScopeIterator(Isolate* isolate,
                                       Handle<JSGeneratorObject> generator)
    : iterator_(isolate, generator) {
  if (!Done() && ShouldIgnore()) Advance();
}

bool DebugScopeIterator::Done() { return iterator_.Done(); }

void DebugScopeIterator::Advance() {
  DCHECK(!Done());
  iterator_.Next();
  while (!Done() && ShouldIgnore()) {
    iterator_.Next();
  }
}

bool DebugScopeIterator::ShouldIgnore() {
  if (GetType() == debug::ScopeIterator::ScopeTypeLocal) return false;
  return !iterator_.DeclaresLocals(i::ScopeIterator::Mode::ALL);
}

v8::debug::ScopeIterator::ScopeType DebugScopeIterator::GetType() {
  DCHECK(!Done());
  return static_cast<v8::debug::ScopeIterator::ScopeType>(iterator_.Type());
}

v8::Local<v8::Object> DebugScopeIterator::GetObject() {
  DCHECK(!Done());
  Handle<JSObject> value = iterator_.ScopeObject(i::ScopeIterator::Mode::ALL);
  return Utils::ToLocal(value);
}

int DebugScopeIterator::GetScriptId() {
  DCHECK(!Done());
  return iterator_.GetScript()->id();
}

v8::Local<v8::Value> DebugScopeIterator::GetFunctionDebugName() {
  DCHECK(!Done());
  Handle<Object> name = iterator_.GetFunctionDebugName();
  return Utils::ToLocal(name);
}

bool DebugScopeIterator::HasLocationInfo() {
  return iterator_.HasPositionInfo();
}

debug::Location DebugScopeIterator::GetStartLocation() {
  DCHECK(!Done());
  return ToApiHandle<v8::debug::Script>(iterator_.GetScript())
      ->GetSourceLocation(iterator_.start_position());
}

debug::Location DebugScopeIterator::GetEndLocation() {
  DCHECK(!Done());
  return ToApiHandle<v8::debug::Script>(iterator_.GetScript())
      ->GetSourceLocation(iterator_.end_position());
}

bool DebugScopeIterator::SetVariableValue(v8::Local<v8::String> name,
                                          v8::Local<v8::Value> value) {
  DCHECK(!Done());
  return iterator_.SetVariableValue(Utils::OpenHandle(*name),
                                    Utils::OpenHandle(*value));
}

DebugWasmScopeIterator::DebugWasmScopeIterator(Isolate* isolate,
                                               WasmFrame* frame)
    : isolate_(isolate),
      frame_(frame),
      type_(debug::ScopeIterator::ScopeTypeModule) {}

bool DebugWasmScopeIterator::Done() {
  return type_ == debug::ScopeIterator::ScopeTypeWith;
}

void DebugWasmScopeIterator::Advance() {
  DCHECK(!Done());
  switch (type_) {
    case ScopeTypeModule:
      // Skip local scope and expression stack scope if the frame is not
      // inspectable.
      type_ = frame_->is_inspectable() ? debug::ScopeIterator::ScopeTypeLocal
                                       : debug::ScopeIterator::ScopeTypeWith;
      break;
    case ScopeTypeLocal:
      type_ = debug::ScopeIterator::ScopeTypeWasmExpressionStack;
      break;
    case ScopeTypeWasmExpressionStack:
      // We use ScopeTypeWith type as marker for done.
      type_ = debug::ScopeIterator::ScopeTypeWith;
      break;
    default:
      UNREACHABLE();
  }
}

v8::debug::ScopeIterator::ScopeType DebugWasmScopeIterator::GetType() {
  DCHECK(!Done());
  return type_;
}

v8::Local<v8::Object> DebugWasmScopeIterator::GetObject() {
  DCHECK(!Done());
  switch (type_) {
    case debug::ScopeIterator::ScopeTypeModule: {
      Handle<WasmInstanceObject> instance =
          FrameSummary::GetTop(frame_).AsWasm().wasm_instance();
      return Utils::ToLocal(wasm::GetModuleScopeObject(instance));
    }
    case debug::ScopeIterator::ScopeTypeLocal: {
      DCHECK(frame_->is_wasm());
      wasm::DebugInfo* debug_info = frame_->native_module()->GetDebugInfo();
      return Utils::ToLocal(debug_info->GetLocalScopeObject(
          isolate_, frame_->pc(), frame_->fp(), frame_->callee_fp()));
    }
    case debug::ScopeIterator::ScopeTypeWasmExpressionStack: {
      DCHECK(frame_->is_wasm());
      wasm::DebugInfo* debug_info = frame_->native_module()->GetDebugInfo();
      return Utils::ToLocal(debug_info->GetStackScopeObject(
          isolate_, frame_->pc(), frame_->fp(), frame_->callee_fp()));
    }
    default:
      return {};
  }
}

int DebugWasmScopeIterator::GetScriptId() {
  DCHECK(!Done());
  return -1;
}

v8::Local<v8::Value> DebugWasmScopeIterator::GetFunctionDebugName() {
  DCHECK(!Done());
  return Utils::ToLocal(isolate_->factory()->empty_string());
}

bool DebugWasmScopeIterator::HasLocationInfo() { return false; }

debug::Location DebugWasmScopeIterator::GetStartLocation() {
  DCHECK(!Done());
  return debug::Location();
}

debug::Location DebugWasmScopeIterator::GetEndLocation() {
  DCHECK(!Done());
  return debug::Location();
}

bool DebugWasmScopeIterator::SetVariableValue(v8::Local<v8::String> name,
                                              v8::Local<v8::Value> value) {
  DCHECK(!Done());
  return false;
}
}  // namespace internal
}  // namespace v8

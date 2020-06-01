// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/base/memory.h"
#include "src/common/message-template.h"
#include "src/compiler/wasm-compiler.h"
#include "src/debug/debug.h"
#include "src/execution/arguments-inl.h"
#include "src/execution/frame-constants.h"
#include "src/execution/frames.h"
#include "src/heap/factory.h"
#include "src/logging/counters.h"
#include "src/numbers/conversions.h"
#include "src/objects/frame-array-inl.h"
#include "src/objects/objects-inl.h"
#include "src/runtime/runtime-utils.h"
#include "src/trap-handler/trap-handler.h"
#include "src/wasm/module-compiler.h"
#include "src/wasm/value-type.h"
#include "src/wasm/wasm-code-manager.h"
#include "src/wasm/wasm-constants.h"
#include "src/wasm/wasm-debug.h"
#include "src/wasm/wasm-engine.h"
#include "src/wasm/wasm-objects.h"
#include "src/wasm/wasm-value.h"

namespace v8 {
namespace internal {

namespace {

template <typename FrameType, StackFrame::Type... skipped_frame_types>
class FrameFinder {
  static_assert(sizeof...(skipped_frame_types) > 0,
                "Specify at least one frame to skip");

 public:
  explicit FrameFinder(Isolate* isolate)
      : frame_iterator_(isolate, isolate->thread_local_top()) {
    for (auto type : {skipped_frame_types...}) {
      DCHECK_EQ(type, frame_iterator_.frame()->type());
      USE(type);
      frame_iterator_.Advance();
    }
    // Type check the frame where the iterator stopped now.
    DCHECK_NOT_NULL(frame());
  }

  FrameType* frame() { return FrameType::cast(frame_iterator_.frame()); }

 private:
  StackFrameIterator frame_iterator_;
};

WasmInstanceObject GetWasmInstanceOnStackTop(Isolate* isolate) {
  return FrameFinder<WasmFrame, StackFrame::EXIT>(isolate)
      .frame()
      ->wasm_instance();
}

Context GetNativeContextFromWasmInstanceOnStackTop(Isolate* isolate) {
  return GetWasmInstanceOnStackTop(isolate).native_context();
}

class ClearThreadInWasmScope {
 public:
  ClearThreadInWasmScope() {
    DCHECK_EQ(trap_handler::IsTrapHandlerEnabled(),
              trap_handler::IsThreadInWasm());
    trap_handler::ClearThreadInWasm();
  }
  ~ClearThreadInWasmScope() {
    DCHECK(!trap_handler::IsThreadInWasm());
    trap_handler::SetThreadInWasm();
  }
};

Object ThrowWasmError(Isolate* isolate, MessageTemplate message) {
  HandleScope scope(isolate);
  Handle<JSObject> error_obj = isolate->factory()->NewWasmRuntimeError(message);
  JSObject::AddProperty(isolate, error_obj,
                        isolate->factory()->wasm_uncatchable_symbol(),
                        isolate->factory()->true_value(), NONE);
  return isolate->Throw(*error_obj);
}
}  // namespace

RUNTIME_FUNCTION(Runtime_WasmIsValidFuncRefValue) {
  // This code is called from wrappers, so the "thread is wasm" flag is not set.
  DCHECK(!trap_handler::IsThreadInWasm());
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  CONVERT_ARG_HANDLE_CHECKED(Object, function, 0);

  if (function->IsNull(isolate)) {
    return Smi::FromInt(true);
  }
  if (WasmExternalFunction::IsWasmExternalFunction(*function)) {
    return Smi::FromInt(true);
  }
  return Smi::FromInt(false);
}

RUNTIME_FUNCTION(Runtime_WasmMemoryGrow) {
  ClearThreadInWasmScope flag_scope;
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  CONVERT_ARG_HANDLE_CHECKED(WasmInstanceObject, instance, 0);
  // {delta_pages} is checked to be a positive smi in the WasmMemoryGrow builtin
  // which calls this runtime function.
  CONVERT_UINT32_ARG_CHECKED(delta_pages, 1);

  int ret = WasmMemoryObject::Grow(
      isolate, handle(instance->memory_object(), isolate), delta_pages);
  // The WasmMemoryGrow builtin which calls this runtime function expects us to
  // always return a Smi.
  return Smi::FromInt(ret);
}

RUNTIME_FUNCTION(Runtime_ThrowWasmError) {
  ClearThreadInWasmScope clear_wasm_flag;
  DCHECK_EQ(1, args.length());
  CONVERT_SMI_ARG_CHECKED(message_id, 0);
  return ThrowWasmError(isolate, MessageTemplateFromInt(message_id));
}

RUNTIME_FUNCTION(Runtime_ThrowWasmStackOverflow) {
  ClearThreadInWasmScope clear_wasm_flag;
  SealHandleScope shs(isolate);
  DCHECK_LE(0, args.length());
  return isolate->StackOverflow();
}

RUNTIME_FUNCTION(Runtime_WasmThrowTypeError) {
  // This runtime function is called both from wasm and from e.g. js-to-js
  // functions. Hence the "thread in wasm" flag can be either set or not. Both
  // is OK, since throwing will trigger unwinding anyway, which sets the flag
  // correctly depending on the handler.
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());
  THROW_NEW_ERROR_RETURN_FAILURE(
      isolate, NewTypeError(MessageTemplate::kWasmTrapTypeError));
}

RUNTIME_FUNCTION(Runtime_WasmThrowCreate) {
  ClearThreadInWasmScope clear_wasm_flag;
  // TODO(kschimpf): Can this be replaced with equivalent TurboFan code/calls.
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  DCHECK(isolate->context().is_null());
  isolate->set_context(GetNativeContextFromWasmInstanceOnStackTop(isolate));
  CONVERT_ARG_CHECKED(WasmExceptionTag, tag_raw, 0);
  CONVERT_SMI_ARG_CHECKED(size, 1);
  // TODO(wasm): Manually box because parameters are not visited yet.
  Handle<Object> tag(tag_raw, isolate);
  Handle<Object> exception = isolate->factory()->NewWasmRuntimeError(
      MessageTemplate::kWasmExceptionError);
  CHECK(!Object::SetProperty(isolate, exception,
                             isolate->factory()->wasm_exception_tag_symbol(),
                             tag, StoreOrigin::kMaybeKeyed,
                             Just(ShouldThrow::kThrowOnError))
             .is_null());
  Handle<FixedArray> values = isolate->factory()->NewFixedArray(size);
  CHECK(!Object::SetProperty(isolate, exception,
                             isolate->factory()->wasm_exception_values_symbol(),
                             values, StoreOrigin::kMaybeKeyed,
                             Just(ShouldThrow::kThrowOnError))
             .is_null());
  return *exception;
}

RUNTIME_FUNCTION(Runtime_WasmStackGuard) {
  ClearThreadInWasmScope wasm_flag;
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());

  // Check if this is a real stack overflow.
  StackLimitCheck check(isolate);
  if (check.JsHasOverflowed()) return isolate->StackOverflow();

  return isolate->stack_guard()->HandleInterrupts();
}

RUNTIME_FUNCTION(Runtime_WasmCompileLazy) {
  ClearThreadInWasmScope wasm_flag;
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  CONVERT_ARG_HANDLE_CHECKED(WasmInstanceObject, instance, 0);
  CONVERT_SMI_ARG_CHECKED(func_index, 1);

#ifdef DEBUG
  FrameFinder<WasmCompileLazyFrame, StackFrame::EXIT> frame_finder(isolate);
  DCHECK_EQ(*instance, frame_finder.frame()->wasm_instance());
#endif

  DCHECK(isolate->context().is_null());
  isolate->set_context(instance->native_context());
  auto* native_module = instance->module_object().native_module();
  bool success = wasm::CompileLazy(isolate, native_module, func_index);
  if (!success) {
    DCHECK(isolate->has_pending_exception());
    return ReadOnlyRoots(isolate).exception();
  }

  Address entrypoint = native_module->GetCallTargetForFunction(func_index);

  return Object(entrypoint);
}

// Should be called from within a handle scope
Handle<JSArrayBuffer> GetSharedArrayBuffer(Handle<WasmInstanceObject> instance,
                                           Isolate* isolate, uint32_t address) {
  DCHECK(instance->has_memory_object());
  Handle<JSArrayBuffer> array_buffer(instance->memory_object().array_buffer(),
                                     isolate);

  // Validation should have failed if the memory was not shared.
  DCHECK(array_buffer->is_shared());

  // Should have trapped if address was OOB
  DCHECK_LT(address, array_buffer->byte_length());
  return array_buffer;
}

RUNTIME_FUNCTION(Runtime_WasmAtomicNotify) {
  ClearThreadInWasmScope clear_wasm_flag;
  HandleScope scope(isolate);
  DCHECK_EQ(3, args.length());
  CONVERT_ARG_HANDLE_CHECKED(WasmInstanceObject, instance, 0);
  CONVERT_NUMBER_CHECKED(uint32_t, address, Uint32, args[1]);
  CONVERT_NUMBER_CHECKED(uint32_t, count, Uint32, args[2]);
  Handle<JSArrayBuffer> array_buffer =
      GetSharedArrayBuffer(instance, isolate, address);
  return FutexEmulation::Wake(array_buffer, address, count);
}

RUNTIME_FUNCTION(Runtime_WasmI32AtomicWait) {
  ClearThreadInWasmScope clear_wasm_flag;
  HandleScope scope(isolate);
  DCHECK_EQ(4, args.length());
  CONVERT_ARG_HANDLE_CHECKED(WasmInstanceObject, instance, 0);
  CONVERT_NUMBER_CHECKED(uint32_t, address, Uint32, args[1]);
  CONVERT_NUMBER_CHECKED(int32_t, expected_value, Int32, args[2]);
  CONVERT_ARG_HANDLE_CHECKED(BigInt, timeout_ns, 3);

  Handle<JSArrayBuffer> array_buffer =
      GetSharedArrayBuffer(instance, isolate, address);
  return FutexEmulation::WaitWasm32(isolate, array_buffer, address,
                                    expected_value, timeout_ns->AsInt64());
}

RUNTIME_FUNCTION(Runtime_WasmI64AtomicWait) {
  ClearThreadInWasmScope clear_wasm_flag;
  HandleScope scope(isolate);
  DCHECK_EQ(4, args.length());
  CONVERT_ARG_HANDLE_CHECKED(WasmInstanceObject, instance, 0);
  CONVERT_NUMBER_CHECKED(uint32_t, address, Uint32, args[1]);
  CONVERT_ARG_HANDLE_CHECKED(BigInt, expected_value, 2);
  CONVERT_ARG_HANDLE_CHECKED(BigInt, timeout_ns, 3);

  Handle<JSArrayBuffer> array_buffer =
      GetSharedArrayBuffer(instance, isolate, address);
  return FutexEmulation::WaitWasm64(isolate, array_buffer, address,
                                    expected_value->AsInt64(),
                                    timeout_ns->AsInt64());
}

namespace {
Object ThrowTableOutOfBounds(Isolate* isolate,
                             Handle<WasmInstanceObject> instance) {
  // Handle out-of-bounds access here in the runtime call, rather
  // than having the lower-level layers deal with JS exceptions.
  if (isolate->context().is_null()) {
    isolate->set_context(instance->native_context());
  }
  Handle<Object> error_obj = isolate->factory()->NewWasmRuntimeError(
      MessageTemplate::kWasmTrapTableOutOfBounds);
  return isolate->Throw(*error_obj);
}
}  // namespace

RUNTIME_FUNCTION(Runtime_WasmRefFunc) {
  ClearThreadInWasmScope flag_scope;
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  CONVERT_ARG_HANDLE_CHECKED(WasmInstanceObject, instance, 0);
  CONVERT_UINT32_ARG_CHECKED(function_index, 1);

  Handle<WasmExternalFunction> function =
      WasmInstanceObject::GetOrCreateWasmExternalFunction(isolate, instance,
                                                          function_index);

  return *function;
}

RUNTIME_FUNCTION(Runtime_WasmFunctionTableGet) {
  ClearThreadInWasmScope flag_scope;
  HandleScope scope(isolate);
  DCHECK_EQ(3, args.length());
  CONVERT_ARG_HANDLE_CHECKED(WasmInstanceObject, instance, 0);
  CONVERT_UINT32_ARG_CHECKED(table_index, 1);
  CONVERT_UINT32_ARG_CHECKED(entry_index, 2);
  DCHECK_LT(table_index, instance->tables().length());
  auto table = handle(
      WasmTableObject::cast(instance->tables().get(table_index)), isolate);
  // We only use the runtime call for lazily initialized function references.
  DCHECK_EQ(table->type(), wasm::kWasmFuncRef);

  if (!WasmTableObject::IsInBounds(isolate, table, entry_index)) {
    return ThrowWasmError(isolate, MessageTemplate::kWasmTrapTableOutOfBounds);
  }

  return *WasmTableObject::Get(isolate, table, entry_index);
}

RUNTIME_FUNCTION(Runtime_WasmFunctionTableSet) {
  ClearThreadInWasmScope flag_scope;
  HandleScope scope(isolate);
  DCHECK_EQ(4, args.length());
  CONVERT_ARG_HANDLE_CHECKED(WasmInstanceObject, instance, 0);
  CONVERT_UINT32_ARG_CHECKED(table_index, 1);
  CONVERT_UINT32_ARG_CHECKED(entry_index, 2);
  CONVERT_ARG_CHECKED(Object, element_raw, 3);
  // TODO(wasm): Manually box because parameters are not visited yet.
  Handle<Object> element(element_raw, isolate);
  DCHECK_LT(table_index, instance->tables().length());
  auto table = handle(
      WasmTableObject::cast(instance->tables().get(table_index)), isolate);
  // We only use the runtime call for function references.
  DCHECK_EQ(table->type(), wasm::kWasmFuncRef);

  if (!WasmTableObject::IsInBounds(isolate, table, entry_index)) {
    return ThrowWasmError(isolate, MessageTemplate::kWasmTrapTableOutOfBounds);
  }
  WasmTableObject::Set(isolate, table, entry_index, element);
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_WasmTableInit) {
  ClearThreadInWasmScope flag_scope;
  HandleScope scope(isolate);
  DCHECK_EQ(6, args.length());
  CONVERT_ARG_HANDLE_CHECKED(WasmInstanceObject, instance, 0);
  CONVERT_UINT32_ARG_CHECKED(table_index, 1);
  CONVERT_UINT32_ARG_CHECKED(elem_segment_index, 2);
  static_assert(
      wasm::kV8MaxWasmTableSize < kSmiMaxValue,
      "Make sure clamping to Smi range doesn't make an invalid call valid");
  CONVERT_UINT32_ARG_CHECKED(dst, 3);
  CONVERT_UINT32_ARG_CHECKED(src, 4);
  CONVERT_UINT32_ARG_CHECKED(count, 5);

  DCHECK(!isolate->context().is_null());

  bool oob = !WasmInstanceObject::InitTableEntries(
      isolate, instance, table_index, elem_segment_index, dst, src, count);
  if (oob) return ThrowTableOutOfBounds(isolate, instance);
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_WasmTableCopy) {
  ClearThreadInWasmScope flag_scope;
  HandleScope scope(isolate);
  DCHECK_EQ(6, args.length());
  CONVERT_ARG_HANDLE_CHECKED(WasmInstanceObject, instance, 0);
  CONVERT_UINT32_ARG_CHECKED(table_dst_index, 1);
  CONVERT_UINT32_ARG_CHECKED(table_src_index, 2);
  static_assert(
      wasm::kV8MaxWasmTableSize < kSmiMaxValue,
      "Make sure clamping to Smi range doesn't make an invalid call valid");
  CONVERT_UINT32_ARG_CHECKED(dst, 3);
  CONVERT_UINT32_ARG_CHECKED(src, 4);
  CONVERT_UINT32_ARG_CHECKED(count, 5);

  DCHECK(!isolate->context().is_null());

  bool oob = !WasmInstanceObject::CopyTableEntries(
      isolate, instance, table_dst_index, table_src_index, dst, src, count);
  if (oob) return ThrowTableOutOfBounds(isolate, instance);
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_WasmTableGrow) {
  ClearThreadInWasmScope flag_scope;
  HandleScope scope(isolate);
  DCHECK_EQ(3, args.length());
  auto instance =
      Handle<WasmInstanceObject>(GetWasmInstanceOnStackTop(isolate), isolate);
  CONVERT_UINT32_ARG_CHECKED(table_index, 0);
  CONVERT_ARG_CHECKED(Object, value_raw, 1);
  // TODO(wasm): Manually box because parameters are not visited yet.
  Handle<Object> value(value_raw, isolate);
  CONVERT_UINT32_ARG_CHECKED(delta, 2);

  Handle<WasmTableObject> table(
      WasmTableObject::cast(instance->tables().get(table_index)), isolate);
  int result = WasmTableObject::Grow(isolate, table, delta, value);

  return Smi::FromInt(result);
}

RUNTIME_FUNCTION(Runtime_WasmTableFill) {
  ClearThreadInWasmScope flag_scope;
  HandleScope scope(isolate);
  DCHECK_EQ(4, args.length());
  auto instance =
      Handle<WasmInstanceObject>(GetWasmInstanceOnStackTop(isolate), isolate);
  CONVERT_UINT32_ARG_CHECKED(table_index, 0);
  CONVERT_UINT32_ARG_CHECKED(start, 1);
  CONVERT_ARG_CHECKED(Object, value_raw, 2);
  // TODO(wasm): Manually box because parameters are not visited yet.
  Handle<Object> value(value_raw, isolate);
  CONVERT_UINT32_ARG_CHECKED(count, 3);

  Handle<WasmTableObject> table(
      WasmTableObject::cast(instance->tables().get(table_index)), isolate);

  uint32_t table_size = table->current_length();

  if (start > table_size) {
    return ThrowTableOutOfBounds(isolate, instance);
  }

  // Even when table.fill goes out-of-bounds, as many entries as possible are
  // put into the table. Only afterwards we trap.
  uint32_t fill_count = std::min(count, table_size - start);
  if (fill_count < count) {
    return ThrowTableOutOfBounds(isolate, instance);
  }
  WasmTableObject::Fill(isolate, table, start, value, fill_count);

  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_WasmDebugBreak) {
  ClearThreadInWasmScope flag_scope;
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());
  FrameFinder<WasmFrame, StackFrame::EXIT, StackFrame::WASM_DEBUG_BREAK>
      frame_finder(isolate);
  auto instance = handle(frame_finder.frame()->wasm_instance(), isolate);
  int position = frame_finder.frame()->position();
  isolate->set_context(instance->native_context());

  // Enter the debugger.
  DebugScope debug_scope(isolate->debug());

  const auto undefined = ReadOnlyRoots(isolate).undefined_value();
  WasmFrame* frame = frame_finder.frame();
  auto* debug_info = frame->native_module()->GetDebugInfo();
  if (debug_info->IsStepping(frame)) {
    debug_info->ClearStepping(isolate);
    isolate->debug()->ClearStepping();
    isolate->debug()->OnDebugBreak(isolate->factory()->empty_fixed_array());
    return undefined;
  }

  // Check whether we hit a breakpoint.
  Handle<Script> script(instance->module_object().script(), isolate);
  Handle<FixedArray> breakpoints;
  if (WasmScript::CheckBreakPoints(isolate, script, position)
          .ToHandle(&breakpoints)) {
    debug_info->ClearStepping(isolate);
    isolate->debug()->ClearStepping();
    if (isolate->debug()->break_points_active()) {
      // We hit one or several breakpoints. Notify the debug listeners.
      isolate->debug()->OnDebugBreak(breakpoints);
    }
  } else {
    // Unused breakpoint. Possible scenarios:
    // 1. We hit a breakpoint that was already removed,
    // 2. We hit a stepping breakpoint after resuming,
    // 3. We hit a stepping breakpoint during a stepOver on a recursive call.
    // 4. The breakpoint was set in a different isolate.
    // We can handle the first three cases by simply removing the breakpoint (if
    // it exists), since this will also recompile the function without the
    // stepping breakpoints.
    // TODO(thibaudm/clemensb): handle case 4.
    debug_info->RemoveBreakpoint(frame->function_index(), position, isolate);
  }

  return undefined;
}

}  // namespace internal
}  // namespace v8

// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/wasm/wasm-debug-evaluate.h"

#include <algorithm>
#include <limits>

#include "src/api/api-inl.h"
#include "src/base/platform/wrappers.h"
#include "src/codegen/machine-type.h"
#include "src/compiler/wasm-compiler.h"
#include "src/execution/frames-inl.h"
#include "src/wasm/value-type.h"
#include "src/wasm/wasm-arguments.h"
#include "src/wasm/wasm-constants.h"
#include "src/wasm/wasm-debug.h"
#include "src/wasm/wasm-module.h"
#include "src/wasm/wasm-objects.h"
#include "src/wasm/wasm-result.h"
#include "src/wasm/wasm-value.h"

namespace v8 {
namespace internal {
namespace wasm {
namespace {

static Handle<String> V8String(Isolate* isolate, const char* str) {
  return isolate->factory()->NewStringFromAsciiChecked(str);
}

static bool CheckSignature(ValueType return_type,
                           std::initializer_list<ValueType> argument_types,
                           const FunctionSig* sig, ErrorThrower* thrower) {
  if (sig->return_count() != 1 && return_type != kWasmBottom) {
    thrower->CompileError("Invalid return type. Got none, expected %s",
                          return_type.name().c_str());
    return false;
  }

  if (sig->return_count() == 1) {
    if (sig->GetReturn(0) != return_type) {
      thrower->CompileError("Invalid return type. Got %s, expected %s",
                            sig->GetReturn(0).name().c_str(),
                            return_type.name().c_str());
      return false;
    }
  }

  if (sig->parameter_count() != argument_types.size()) {
    thrower->CompileError("Invalid number of arguments. Expected %zu, got %zu",
                          sig->parameter_count(), argument_types.size());
    return false;
  }
  size_t p = 0;
  for (ValueType argument_type : argument_types) {
    if (sig->GetParam(p) != argument_type) {
      thrower->CompileError(
          "Invalid argument type for argument %zu. Got %s, expected %s", p,
          sig->GetParam(p).name().c_str(), argument_type.name().c_str());
      return false;
    }
    ++p;
  }
  return true;
}

static bool CheckRangeOutOfBounds(uint32_t offset, uint32_t size,
                                  size_t allocation_size,
                                  wasm::ErrorThrower* thrower) {
  if (size > std::numeric_limits<uint32_t>::max() - offset) {
    thrower->RuntimeError("Overflowing memory range\n");
    return true;
  }
  if (offset + size > allocation_size) {
    thrower->RuntimeError("Illegal access to out-of-bounds memory");
    return true;
  }
  return false;
}

class DebugEvaluatorProxy {
 public:
  explicit DebugEvaluatorProxy(Isolate* isolate, CommonFrame* frame)
      : isolate_(isolate), frame_(frame) {}

  static void GetMemoryTrampoline(
      const v8::FunctionCallbackInfo<v8::Value>& args) {
    DebugEvaluatorProxy& proxy = GetProxy(args);

    uint32_t offset = proxy.GetArgAsUInt32(args, 0);
    uint32_t size = proxy.GetArgAsUInt32(args, 1);
    uint32_t result = proxy.GetArgAsUInt32(args, 2);

    proxy.GetMemory(offset, size, result);
  }

  // void __getMemory(uint32_t offset, uint32_t size, void* result);
  void GetMemory(uint32_t offset, uint32_t size, uint32_t result) {
    wasm::ScheduledErrorThrower thrower(isolate_, "debug evaluate proxy");
    // Check all overflows.
    if (CheckRangeOutOfBounds(offset, size, debuggee_->memory_size(),
                              &thrower) ||
        CheckRangeOutOfBounds(result, size, evaluator_->memory_size(),
                              &thrower)) {
      return;
    }

    std::memcpy(&evaluator_->memory_start()[result],
                &debuggee_->memory_start()[offset], size);
  }

  // void* __sbrk(intptr_t increment);
  uint32_t Sbrk(uint32_t increment) {
    if (increment > 0 && evaluator_->memory_size() <=
                             std::numeric_limits<uint32_t>::max() - increment) {
      Handle<WasmMemoryObject> memory(evaluator_->memory_object(), isolate_);
      uint32_t new_pages =
          (increment - 1 + wasm::kWasmPageSize) / wasm::kWasmPageSize;
      WasmMemoryObject::Grow(isolate_, memory, new_pages);
    }
    return static_cast<uint32_t>(evaluator_->memory_size());
  }

  static void SbrkTrampoline(const v8::FunctionCallbackInfo<v8::Value>& args) {
    DebugEvaluatorProxy& proxy = GetProxy(args);
    uint32_t size = proxy.GetArgAsUInt32(args, 0);

    uint32_t result = proxy.Sbrk(size);
    args.GetReturnValue().Set(result);
  }

  // void __getLocal(uint32_t local,  void* result);
  void GetLocal(uint32_t local, uint32_t result_offset) {
    DCHECK(frame_->is_wasm());
    wasm::DebugInfo* debug_info =
        WasmFrame::cast(frame_)->native_module()->GetDebugInfo();
    WasmValue result = debug_info->GetLocalValue(
        local, frame_->pc(), frame_->fp(), frame_->callee_fp());
    WriteResult(result, result_offset);
  }

  void GetGlobal(uint32_t global, uint32_t result_offset) {
    DCHECK(frame_->is_wasm());

    const WasmGlobal& global_variable =
        WasmFrame::cast(frame_)->native_module()->module()->globals.at(global);

    Handle<WasmInstanceObject> instance(
        WasmFrame::cast(frame_)->wasm_instance(), isolate_);
    WasmValue result =
        WasmInstanceObject::GetGlobalValue(instance, global_variable);
    WriteResult(result, result_offset);
  }

  void GetOperand(uint32_t operand, uint32_t result_offset) {
    DCHECK(frame_->is_wasm());
    wasm::DebugInfo* debug_info =
        WasmFrame::cast(frame_)->native_module()->GetDebugInfo();
    WasmValue result = debug_info->GetStackValue(
        operand, frame_->pc(), frame_->fp(), frame_->callee_fp());

    WriteResult(result, result_offset);
  }

  static void GetLocalTrampoline(
      const v8::FunctionCallbackInfo<v8::Value>& args) {
    DebugEvaluatorProxy& proxy = GetProxy(args);
    uint32_t local = proxy.GetArgAsUInt32(args, 0);
    uint32_t result = proxy.GetArgAsUInt32(args, 1);

    proxy.GetLocal(local, result);
  }

  static void GetGlobalTrampoline(
      const v8::FunctionCallbackInfo<v8::Value>& args) {
    DebugEvaluatorProxy& proxy = GetProxy(args);
    uint32_t global = proxy.GetArgAsUInt32(args, 0);
    uint32_t result = proxy.GetArgAsUInt32(args, 1);

    proxy.GetGlobal(global, result);
  }

  static void GetOperandTrampoline(
      const v8::FunctionCallbackInfo<v8::Value>& args) {
    DebugEvaluatorProxy& proxy = GetProxy(args);
    uint32_t operand = proxy.GetArgAsUInt32(args, 0);
    uint32_t result = proxy.GetArgAsUInt32(args, 1);

    proxy.GetOperand(operand, result);
  }

  Handle<JSObject> CreateImports() {
    Handle<JSObject> imports_obj =
        isolate_->factory()->NewJSObject(isolate_->object_function());
    Handle<JSObject> import_module_obj =
        isolate_->factory()->NewJSObject(isolate_->object_function());
    Object::SetProperty(isolate_, imports_obj, V8String(isolate_, "env"),
                        import_module_obj)
        .Assert();

    AddImport(import_module_obj, "__getOperand",
              DebugEvaluatorProxy::GetOperandTrampoline);
    AddImport(import_module_obj, "__getGlobal",
              DebugEvaluatorProxy::GetGlobalTrampoline);
    AddImport(import_module_obj, "__getLocal",
              DebugEvaluatorProxy::GetLocalTrampoline);
    AddImport(import_module_obj, "__getMemory",
              DebugEvaluatorProxy::GetMemoryTrampoline);
    AddImport(import_module_obj, "__sbrk", DebugEvaluatorProxy::SbrkTrampoline);

    return imports_obj;
  }

  void SetInstances(Handle<WasmInstanceObject> evaluator,
                    Handle<WasmInstanceObject> debuggee) {
    evaluator_ = evaluator;
    debuggee_ = debuggee;
  }

 private:
  template <typename T>
  void WriteResultImpl(const WasmValue& result, uint32_t result_offset) {
    wasm::ScheduledErrorThrower thrower(isolate_, "debug evaluate proxy");
    T val = result.to<T>();
    STATIC_ASSERT(static_cast<uint32_t>(sizeof(T)) == sizeof(T));
    if (CheckRangeOutOfBounds(result_offset, sizeof(T),
                              evaluator_->memory_size(), &thrower)) {
      return;
    }
    base::Memcpy(&evaluator_->memory_start()[result_offset], &val, sizeof(T));
  }

  void WriteResult(const WasmValue& result, uint32_t result_offset) {
    switch (result.type().kind()) {
      case ValueType::kI32:
        WriteResultImpl<uint32_t>(result, result_offset);
        break;
      case ValueType::kI64:
        WriteResultImpl<int64_t>(result, result_offset);
        break;
      case ValueType::kF32:
        WriteResultImpl<float>(result, result_offset);
        break;
      case ValueType::kF64:
        WriteResultImpl<double>(result, result_offset);
        break;
      default:
        UNIMPLEMENTED();
    }
  }

  uint32_t GetArgAsUInt32(const v8::FunctionCallbackInfo<v8::Value>& args,
                          int index) {
    // No type/range checks needed on his because this is only called for {args}
    // where we have performed a signature check via {VerifyEvaluatorInterface}
    double number = Utils::OpenHandle(*args[index])->Number();
    return static_cast<uint32_t>(number);
  }

  static DebugEvaluatorProxy& GetProxy(
      const v8::FunctionCallbackInfo<v8::Value>& args) {
    return *reinterpret_cast<DebugEvaluatorProxy*>(
        args.Data().As<v8::External>()->Value());
  }

  template <typename CallableT>
  void AddImport(Handle<JSObject> import_module_obj, const char* function_name,
                 CallableT callback) {
    v8::Isolate* api_isolate = reinterpret_cast<v8::Isolate*>(isolate_);
    v8::Local<v8::Context> context = api_isolate->GetCurrentContext();
    std::string data;
    v8::Local<v8::Function> v8_function =
        v8::Function::New(context, callback,
                          v8::External::New(api_isolate, this))
            .ToLocalChecked();

    Handle<JSReceiver> wrapped_function = Utils::OpenHandle(*v8_function);

    Object::SetProperty(isolate_, import_module_obj,
                        V8String(isolate_, function_name), wrapped_function)
        .Assert();
  }

  Isolate* isolate_;
  CommonFrame* frame_;
  Handle<WasmInstanceObject> evaluator_;
  Handle<WasmInstanceObject> debuggee_;
};

static bool VerifyEvaluatorInterface(const WasmModule* raw_module,
                                     const ModuleWireBytes& bytes,
                                     ErrorThrower* thrower) {
  for (const WasmImport imported : raw_module->import_table) {
    if (imported.kind != ImportExportKindCode::kExternalFunction) continue;
    const WasmFunction& F = raw_module->functions.at(imported.index);
    std::string module_name(bytes.start() + imported.module_name.offset(),
                            bytes.start() + imported.module_name.end_offset());
    std::string field_name(bytes.start() + imported.field_name.offset(),
                           bytes.start() + imported.field_name.end_offset());

    if (module_name == "env") {
      if (field_name == "__getMemory") {
        // void __getMemory(uint32_t offset, uint32_t size, void* result);
        if (CheckSignature(kWasmBottom, {kWasmI32, kWasmI32, kWasmI32}, F.sig,
                           thrower)) {
          continue;
        }
      } else if (field_name == "__getOperand") {
        // void __getOperand(uint32_t local,  void* result)
        if (CheckSignature(kWasmBottom, {kWasmI32, kWasmI32}, F.sig, thrower)) {
          continue;
        }
      } else if (field_name == "__getGlobal") {
        // void __getGlobal(uint32_t local,  void* result)
        if (CheckSignature(kWasmBottom, {kWasmI32, kWasmI32}, F.sig, thrower)) {
          continue;
        }
      } else if (field_name == "__getLocal") {
        // void __getLocal(uint32_t local,  void* result)
        if (CheckSignature(kWasmBottom, {kWasmI32, kWasmI32}, F.sig, thrower)) {
          continue;
        }
      } else if (field_name == "__debug") {
        // void __debug(uint32_t flag, uint32_t value)
        if (CheckSignature(kWasmBottom, {kWasmI32, kWasmI32}, F.sig, thrower)) {
          continue;
        }
      } else if (field_name == "__sbrk") {
        // uint32_t __sbrk(uint32_t increment)
        if (CheckSignature(kWasmI32, {kWasmI32}, F.sig, thrower)) {
          continue;
        }
      }
    }

    if (!thrower->error()) {
      thrower->LinkError("Unknown import \"%s\" \"%s\"", module_name.c_str(),
                         field_name.c_str());
    }

    return false;
  }
  for (const WasmExport& exported : raw_module->export_table) {
    if (exported.kind != ImportExportKindCode::kExternalFunction) continue;
    const WasmFunction& F = raw_module->functions.at(exported.index);
    std::string field_name(bytes.start() + exported.name.offset(),
                           bytes.start() + exported.name.end_offset());
    if (field_name == "wasm_format") {
      if (!CheckSignature(kWasmI32, {}, F.sig, thrower)) return false;
    }
  }
  return true;
}
}  // namespace

Maybe<std::string> DebugEvaluateImpl(
    Vector<const byte> snippet, Handle<WasmInstanceObject> debuggee_instance,
    CommonFrame* frame) {
  Isolate* isolate = debuggee_instance->GetIsolate();
  HandleScope handle_scope(isolate);
  WasmEngine* engine = isolate->wasm_engine();
  wasm::ErrorThrower thrower(isolate, "wasm debug evaluate");

  // Create module object.
  wasm::ModuleWireBytes bytes(snippet);
  wasm::WasmFeatures features = wasm::WasmFeatures::FromIsolate(isolate);
  Handle<WasmModuleObject> evaluator_module;
  if (!engine->SyncCompile(isolate, features, &thrower, bytes)
           .ToHandle(&evaluator_module)) {
    return Nothing<std::string>();
  }

  // Verify interface.
  const WasmModule* raw_module = evaluator_module->module();
  if (!VerifyEvaluatorInterface(raw_module, bytes, &thrower)) {
    return Nothing<std::string>();
  }

  // Set up imports.
  DebugEvaluatorProxy proxy(isolate, frame);
  Handle<JSObject> imports = proxy.CreateImports();

  // Instantiate Module.
  Handle<WasmInstanceObject> evaluator_instance;
  if (!engine->SyncInstantiate(isolate, &thrower, evaluator_module, imports, {})
           .ToHandle(&evaluator_instance)) {
    return Nothing<std::string>();
  }

  proxy.SetInstances(evaluator_instance, debuggee_instance);

  Handle<JSObject> exports_obj(evaluator_instance->exports_object(), isolate);
  Handle<Object> entry_point_obj;
  bool get_property_success =
      Object::GetProperty(isolate, exports_obj,
                          V8String(isolate, "wasm_format"))
          .ToHandle(&entry_point_obj);
  if (!get_property_success ||
      !WasmExportedFunction::IsWasmExportedFunction(*entry_point_obj)) {
    thrower.LinkError("Missing export: \"wasm_format\"");
    return Nothing<std::string>();
  }
  Handle<WasmExportedFunction> entry_point =
      Handle<WasmExportedFunction>::cast(entry_point_obj);

  // TODO(wasm): Cache this code.
  Handle<Code> wasm_entry = compiler::CompileCWasmEntry(
      isolate, entry_point->sig(), debuggee_instance->module());

  CWasmArgumentsPacker packer(4 /* uint32_t return value, no parameters. */);
  Execution::CallWasm(isolate, wasm_entry, entry_point->GetWasmCallTarget(),
                      evaluator_instance, packer.argv());
  if (isolate->has_pending_exception()) return Nothing<std::string>();

  uint32_t offset = packer.Pop<uint32_t>();
  if (CheckRangeOutOfBounds(offset, 0, evaluator_instance->memory_size(),
                            &thrower)) {
    return Nothing<std::string>();
  }

  // Copy the zero-terminated string result but don't overflow.
  std::string result;
  byte* heap = evaluator_instance->memory_start() + offset;
  for (; offset < evaluator_instance->memory_size(); ++offset, ++heap) {
    if (*heap == 0) return Just(result);
    result.push_back(*heap);
  }

  thrower.RuntimeError("The evaluation returned an invalid result");
  return Nothing<std::string>();
}

MaybeHandle<String> DebugEvaluate(Vector<const byte> snippet,
                                  Handle<WasmInstanceObject> debuggee_instance,
                                  CommonFrame* frame) {
  Maybe<std::string> result =
      DebugEvaluateImpl(snippet, debuggee_instance, frame);
  if (result.IsNothing()) return {};
  std::string result_str = result.ToChecked();
  return V8String(debuggee_instance->GetIsolate(), result_str.c_str());
}

}  // namespace wasm
}  // namespace internal
}  // namespace v8

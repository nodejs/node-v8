// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/wasm/wasm-js.h"

#include <cinttypes>
#include <cstring>

#include "src/api/api-inl.h"
#include "src/api/api-natives.h"
#include "src/ast/ast.h"
#include "src/base/logging.h"
#include "src/base/overflowing-math.h"
#include "src/base/platform/wrappers.h"
#include "src/common/assert-scope.h"
#include "src/execution/execution.h"
#include "src/execution/frames-inl.h"
#include "src/execution/isolate.h"
#include "src/handles/handles.h"
#include "src/heap/factory.h"
#include "src/init/v8.h"
#include "src/objects/js-collection-inl.h"
#include "src/objects/js-promise-inl.h"
#include "src/objects/objects-inl.h"
#include "src/objects/templates.h"
#include "src/parsing/parse-info.h"
#include "src/tasks/task-utils.h"
#include "src/trap-handler/trap-handler.h"
#include "src/wasm/function-compiler.h"
#include "src/wasm/streaming-decoder.h"
#include "src/wasm/value-type.h"
#include "src/wasm/wasm-debug.h"
#include "src/wasm/wasm-engine.h"
#include "src/wasm/wasm-limits.h"
#include "src/wasm/wasm-objects-inl.h"
#include "src/wasm/wasm-serialization.h"
#include "src/wasm/wasm-value.h"

using v8::internal::wasm::ErrorThrower;
using v8::internal::wasm::ScheduledErrorThrower;

namespace v8 {

class WasmStreaming::WasmStreamingImpl {
 public:
  WasmStreamingImpl(
      Isolate* isolate, const char* api_method_name,
      std::shared_ptr<internal::wasm::CompilationResultResolver> resolver)
      : isolate_(isolate), resolver_(std::move(resolver)) {
    i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate_);
    auto enabled_features = i::wasm::WasmFeatures::FromIsolate(i_isolate);
    streaming_decoder_ = i_isolate->wasm_engine()->StartStreamingCompilation(
        i_isolate, enabled_features, handle(i_isolate->context(), i_isolate),
        api_method_name, resolver_);
  }

  void OnBytesReceived(const uint8_t* bytes, size_t size) {
    streaming_decoder_->OnBytesReceived(i::VectorOf(bytes, size));
  }
  void Finish() { streaming_decoder_->Finish(); }

  void Abort(MaybeLocal<Value> exception) {
    i::HandleScope scope(reinterpret_cast<i::Isolate*>(isolate_));
    streaming_decoder_->Abort();

    // If no exception value is provided, we do not reject the promise. This can
    // happen when streaming compilation gets aborted when no script execution
    // is allowed anymore, e.g. when a browser tab gets refreshed.
    if (exception.IsEmpty()) return;

    resolver_->OnCompilationFailed(
        Utils::OpenHandle(*exception.ToLocalChecked()));
  }

  bool SetCompiledModuleBytes(const uint8_t* bytes, size_t size) {
    if (!i::wasm::IsSupportedVersion({bytes, size})) return false;
    return streaming_decoder_->SetCompiledModuleBytes({bytes, size});
  }

  void SetClient(std::shared_ptr<Client> client) {
    streaming_decoder_->SetModuleCompiledCallback(
        [client, streaming_decoder = streaming_decoder_](
            const std::shared_ptr<i::wasm::NativeModule>& native_module) {
          i::Vector<const char> url = streaming_decoder->url();
          auto compiled_wasm_module =
              CompiledWasmModule(native_module, url.begin(), url.size());
          client->OnModuleCompiled(compiled_wasm_module);
        });
  }

  void SetUrl(internal::Vector<const char> url) {
    streaming_decoder_->SetUrl(url);
  }

 private:
  Isolate* const isolate_;
  std::shared_ptr<internal::wasm::StreamingDecoder> streaming_decoder_;
  std::shared_ptr<internal::wasm::CompilationResultResolver> resolver_;
};

WasmStreaming::WasmStreaming(std::unique_ptr<WasmStreamingImpl> impl)
    : impl_(std::move(impl)) {
  TRACE_EVENT0("v8.wasm", "wasm.InitializeStreaming");
}

// The destructor is defined here because we have a unique_ptr with forward
// declaration.
WasmStreaming::~WasmStreaming() = default;

void WasmStreaming::OnBytesReceived(const uint8_t* bytes, size_t size) {
  TRACE_EVENT1("v8.wasm", "wasm.OnBytesReceived", "bytes", size);
  impl_->OnBytesReceived(bytes, size);
}

void WasmStreaming::Finish() {
  TRACE_EVENT0("v8.wasm", "wasm.FinishStreaming");
  impl_->Finish();
}

void WasmStreaming::Abort(MaybeLocal<Value> exception) {
  TRACE_EVENT0("v8.wasm", "wasm.AbortStreaming");
  impl_->Abort(exception);
}

bool WasmStreaming::SetCompiledModuleBytes(const uint8_t* bytes, size_t size) {
  TRACE_EVENT0("v8.wasm", "wasm.SetCompiledModuleBytes");
  return impl_->SetCompiledModuleBytes(bytes, size);
}

void WasmStreaming::SetClient(std::shared_ptr<Client> client) {
  TRACE_EVENT0("v8.wasm", "wasm.WasmStreaming.SetClient");
  impl_->SetClient(client);
}

void WasmStreaming::SetUrl(const char* url, size_t length) {
  TRACE_EVENT0("v8.wasm", "wasm.SetUrl");
  impl_->SetUrl(internal::VectorOf(url, length));
}

// static
std::shared_ptr<WasmStreaming> WasmStreaming::Unpack(Isolate* isolate,
                                                     Local<Value> value) {
  TRACE_EVENT0("v8.wasm", "wasm.WasmStreaming.Unpack");
  i::HandleScope scope(reinterpret_cast<i::Isolate*>(isolate));
  auto managed =
      i::Handle<i::Managed<WasmStreaming>>::cast(Utils::OpenHandle(*value));
  return managed->get();
}

namespace {

#define ASSIGN(type, var, expr)                      \
  Local<type> var;                                   \
  do {                                               \
    if (!expr.ToLocal(&var)) {                       \
      DCHECK(i_isolate->has_scheduled_exception());  \
      return;                                        \
    } else {                                         \
      DCHECK(!i_isolate->has_scheduled_exception()); \
    }                                                \
  } while (false)

i::Handle<i::String> v8_str(i::Isolate* isolate, const char* str) {
  return isolate->factory()->NewStringFromAsciiChecked(str);
}
Local<String> v8_str(Isolate* isolate, const char* str) {
  return Utils::ToLocal(v8_str(reinterpret_cast<i::Isolate*>(isolate), str));
}

#define GET_FIRST_ARGUMENT_AS(Type)                                  \
  i::MaybeHandle<i::Wasm##Type##Object> GetFirstArgumentAs##Type(    \
      const v8::FunctionCallbackInfo<v8::Value>& args,               \
      ErrorThrower* thrower) {                                       \
    i::Handle<i::Object> arg0 = Utils::OpenHandle(*args[0]);         \
    if (!arg0->IsWasm##Type##Object()) {                             \
      thrower->TypeError("Argument 0 must be a WebAssembly." #Type); \
      return {};                                                     \
    }                                                                \
    Local<Object> obj = Local<Object>::Cast(args[0]);                \
    return i::Handle<i::Wasm##Type##Object>::cast(                   \
        v8::Utils::OpenHandle(*obj));                                \
  }

GET_FIRST_ARGUMENT_AS(Module)
GET_FIRST_ARGUMENT_AS(Memory)
GET_FIRST_ARGUMENT_AS(Table)
GET_FIRST_ARGUMENT_AS(Global)

#undef GET_FIRST_ARGUMENT_AS

i::wasm::ModuleWireBytes GetFirstArgumentAsBytes(
    const v8::FunctionCallbackInfo<v8::Value>& args, ErrorThrower* thrower,
    bool* is_shared) {
  const uint8_t* start = nullptr;
  size_t length = 0;
  v8::Local<v8::Value> source = args[0];
  if (source->IsArrayBuffer()) {
    // A raw array buffer was passed.
    Local<ArrayBuffer> buffer = Local<ArrayBuffer>::Cast(source);
    auto backing_store = buffer->GetBackingStore();

    start = reinterpret_cast<const uint8_t*>(backing_store->Data());
    length = backing_store->ByteLength();
    *is_shared = buffer->IsSharedArrayBuffer();
  } else if (source->IsTypedArray()) {
    // A TypedArray was passed.
    Local<TypedArray> array = Local<TypedArray>::Cast(source);
    Local<ArrayBuffer> buffer = array->Buffer();

    auto backing_store = buffer->GetBackingStore();

    start = reinterpret_cast<const uint8_t*>(backing_store->Data()) +
            array->ByteOffset();
    length = array->ByteLength();
    *is_shared = buffer->IsSharedArrayBuffer();
  } else {
    thrower->TypeError("Argument 0 must be a buffer source");
  }
  DCHECK_IMPLIES(length, start != nullptr);
  if (length == 0) {
    thrower->CompileError("BufferSource argument is empty");
  }
  size_t max_length = i::wasm::max_module_size();
  if (length > max_length) {
    thrower->RangeError("buffer source exceeds maximum size of %zu (is %zu)",
                        max_length, length);
  }
  if (thrower->error()) return i::wasm::ModuleWireBytes(nullptr, nullptr);
  return i::wasm::ModuleWireBytes(start, start + length);
}

i::MaybeHandle<i::JSReceiver> GetValueAsImports(Local<Value> arg,
                                                ErrorThrower* thrower) {
  if (arg->IsUndefined()) return {};

  if (!arg->IsObject()) {
    thrower->TypeError("Argument 1 must be an object");
    return {};
  }
  Local<Object> obj = Local<Object>::Cast(arg);
  return i::Handle<i::JSReceiver>::cast(v8::Utils::OpenHandle(*obj));
}

namespace {
// This class resolves the result of WebAssembly.compile. It just places the
// compilation result in the supplied {promise}.
class AsyncCompilationResolver : public i::wasm::CompilationResultResolver {
 public:
  AsyncCompilationResolver(i::Isolate* isolate, i::Handle<i::JSPromise> promise)
      : promise_(isolate->global_handles()->Create(*promise)) {
    i::GlobalHandles::AnnotateStrongRetainer(promise_.location(),
                                             kGlobalPromiseHandle);
  }

  ~AsyncCompilationResolver() override {
    i::GlobalHandles::Destroy(promise_.location());
  }

  void OnCompilationSucceeded(i::Handle<i::WasmModuleObject> result) override {
    if (finished_) return;
    finished_ = true;
    i::MaybeHandle<i::Object> promise_result =
        i::JSPromise::Resolve(promise_, result);
    CHECK_EQ(promise_result.is_null(),
             promise_->GetIsolate()->has_pending_exception());
  }

  void OnCompilationFailed(i::Handle<i::Object> error_reason) override {
    if (finished_) return;
    finished_ = true;
    i::MaybeHandle<i::Object> promise_result =
        i::JSPromise::Reject(promise_, error_reason);
    CHECK_EQ(promise_result.is_null(),
             promise_->GetIsolate()->has_pending_exception());
  }

 private:
  static constexpr char kGlobalPromiseHandle[] =
      "AsyncCompilationResolver::promise_";
  bool finished_ = false;
  i::Handle<i::JSPromise> promise_;
};

constexpr char AsyncCompilationResolver::kGlobalPromiseHandle[];

// This class resolves the result of WebAssembly.instantiate(module, imports).
// It just places the instantiation result in the supplied {promise}.
class InstantiateModuleResultResolver
    : public i::wasm::InstantiationResultResolver {
 public:
  InstantiateModuleResultResolver(i::Isolate* isolate,
                                  i::Handle<i::JSPromise> promise)
      : promise_(isolate->global_handles()->Create(*promise)) {
    i::GlobalHandles::AnnotateStrongRetainer(promise_.location(),
                                             kGlobalPromiseHandle);
  }

  ~InstantiateModuleResultResolver() override {
    i::GlobalHandles::Destroy(promise_.location());
  }

  void OnInstantiationSucceeded(
      i::Handle<i::WasmInstanceObject> instance) override {
    i::MaybeHandle<i::Object> promise_result =
        i::JSPromise::Resolve(promise_, instance);
    CHECK_EQ(promise_result.is_null(),
             promise_->GetIsolate()->has_pending_exception());
  }

  void OnInstantiationFailed(i::Handle<i::Object> error_reason) override {
    i::MaybeHandle<i::Object> promise_result =
        i::JSPromise::Reject(promise_, error_reason);
    CHECK_EQ(promise_result.is_null(),
             promise_->GetIsolate()->has_pending_exception());
  }

 private:
  static constexpr char kGlobalPromiseHandle[] =
      "InstantiateModuleResultResolver::promise_";
  i::Handle<i::JSPromise> promise_;
};

constexpr char InstantiateModuleResultResolver::kGlobalPromiseHandle[];

// This class resolves the result of WebAssembly.instantiate(bytes, imports).
// For that it creates a new {JSObject} which contains both the provided
// {WasmModuleObject} and the resulting {WebAssemblyInstanceObject} itself.
class InstantiateBytesResultResolver
    : public i::wasm::InstantiationResultResolver {
 public:
  InstantiateBytesResultResolver(i::Isolate* isolate,
                                 i::Handle<i::JSPromise> promise,
                                 i::Handle<i::WasmModuleObject> module)
      : isolate_(isolate),
        promise_(isolate_->global_handles()->Create(*promise)),
        module_(isolate_->global_handles()->Create(*module)) {
    i::GlobalHandles::AnnotateStrongRetainer(promise_.location(),
                                             kGlobalPromiseHandle);
    i::GlobalHandles::AnnotateStrongRetainer(module_.location(),
                                             kGlobalModuleHandle);
  }

  ~InstantiateBytesResultResolver() override {
    i::GlobalHandles::Destroy(promise_.location());
    i::GlobalHandles::Destroy(module_.location());
  }

  void OnInstantiationSucceeded(
      i::Handle<i::WasmInstanceObject> instance) override {
    // The result is a JSObject with 2 fields which contain the
    // WasmInstanceObject and the WasmModuleObject.
    i::Handle<i::JSObject> result =
        isolate_->factory()->NewJSObject(isolate_->object_function());

    i::Handle<i::String> instance_name =
        isolate_->factory()->NewStringFromStaticChars("instance");

    i::Handle<i::String> module_name =
        isolate_->factory()->NewStringFromStaticChars("module");

    i::JSObject::AddProperty(isolate_, result, instance_name, instance,
                             i::NONE);
    i::JSObject::AddProperty(isolate_, result, module_name, module_, i::NONE);

    i::MaybeHandle<i::Object> promise_result =
        i::JSPromise::Resolve(promise_, result);
    CHECK_EQ(promise_result.is_null(), isolate_->has_pending_exception());
  }

  void OnInstantiationFailed(i::Handle<i::Object> error_reason) override {
    i::MaybeHandle<i::Object> promise_result =
        i::JSPromise::Reject(promise_, error_reason);
    CHECK_EQ(promise_result.is_null(), isolate_->has_pending_exception());
  }

 private:
  static constexpr char kGlobalPromiseHandle[] =
      "InstantiateBytesResultResolver::promise_";
  static constexpr char kGlobalModuleHandle[] =
      "InstantiateBytesResultResolver::module_";
  i::Isolate* isolate_;
  i::Handle<i::JSPromise> promise_;
  i::Handle<i::WasmModuleObject> module_;
};

constexpr char InstantiateBytesResultResolver::kGlobalPromiseHandle[];
constexpr char InstantiateBytesResultResolver::kGlobalModuleHandle[];

// This class is the {CompilationResultResolver} for
// WebAssembly.instantiate(bytes, imports). When compilation finishes,
// {AsyncInstantiate} is started on the compilation result.
class AsyncInstantiateCompileResultResolver
    : public i::wasm::CompilationResultResolver {
 public:
  AsyncInstantiateCompileResultResolver(
      i::Isolate* isolate, i::Handle<i::JSPromise> promise,
      i::MaybeHandle<i::JSReceiver> maybe_imports)
      : isolate_(isolate),
        promise_(isolate_->global_handles()->Create(*promise)),
        maybe_imports_(maybe_imports.is_null()
                           ? maybe_imports
                           : isolate_->global_handles()->Create(
                                 *maybe_imports.ToHandleChecked())) {
    i::GlobalHandles::AnnotateStrongRetainer(promise_.location(),
                                             kGlobalPromiseHandle);
    if (!maybe_imports_.is_null()) {
      i::GlobalHandles::AnnotateStrongRetainer(
          maybe_imports_.ToHandleChecked().location(), kGlobalImportsHandle);
    }
  }

  ~AsyncInstantiateCompileResultResolver() override {
    i::GlobalHandles::Destroy(promise_.location());
    if (!maybe_imports_.is_null()) {
      i::GlobalHandles::Destroy(maybe_imports_.ToHandleChecked().location());
    }
  }

  void OnCompilationSucceeded(i::Handle<i::WasmModuleObject> result) override {
    if (finished_) return;
    finished_ = true;
    isolate_->wasm_engine()->AsyncInstantiate(
        isolate_,
        std::make_unique<InstantiateBytesResultResolver>(isolate_, promise_,
                                                         result),
        result, maybe_imports_);
  }

  void OnCompilationFailed(i::Handle<i::Object> error_reason) override {
    if (finished_) return;
    finished_ = true;
    i::MaybeHandle<i::Object> promise_result =
        i::JSPromise::Reject(promise_, error_reason);
    CHECK_EQ(promise_result.is_null(), isolate_->has_pending_exception());
  }

 private:
  static constexpr char kGlobalPromiseHandle[] =
      "AsyncInstantiateCompileResultResolver::promise_";
  static constexpr char kGlobalImportsHandle[] =
      "AsyncInstantiateCompileResultResolver::module_";
  bool finished_ = false;
  i::Isolate* isolate_;
  i::Handle<i::JSPromise> promise_;
  i::MaybeHandle<i::JSReceiver> maybe_imports_;
};

constexpr char AsyncInstantiateCompileResultResolver::kGlobalPromiseHandle[];
constexpr char AsyncInstantiateCompileResultResolver::kGlobalImportsHandle[];

std::string ToString(const char* name) { return std::string(name); }

std::string ToString(const i::Handle<i::String> name) {
  return std::string("Property '") + name->ToCString().get() + "'";
}

// Web IDL: '[EnforceRange] unsigned long'
// Previously called ToNonWrappingUint32 in the draft WebAssembly JS spec.
// https://heycam.github.io/webidl/#EnforceRange
template <typename T>
bool EnforceUint32(T argument_name, Local<v8::Value> v, Local<Context> context,
                   ErrorThrower* thrower, uint32_t* res) {
  double double_number;

  if (!v->NumberValue(context).To(&double_number)) {
    thrower->TypeError("%s must be convertible to a number",
                       ToString(argument_name).c_str());
    return false;
  }
  if (!std::isfinite(double_number)) {
    thrower->TypeError("%s must be convertible to a valid number",
                       ToString(argument_name).c_str());
    return false;
  }
  if (double_number < 0) {
    thrower->TypeError("%s must be non-negative",
                       ToString(argument_name).c_str());
    return false;
  }
  if (double_number > std::numeric_limits<uint32_t>::max()) {
    thrower->TypeError("%s must be in the unsigned long range",
                       ToString(argument_name).c_str());
    return false;
  }

  *res = static_cast<uint32_t>(double_number);
  return true;
}
}  // namespace

// WebAssembly.compile(bytes) -> Promise
void WebAssemblyCompile(const v8::FunctionCallbackInfo<v8::Value>& args) {
  constexpr const char* kAPIMethodName = "WebAssembly.compile()";
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);

  HandleScope scope(isolate);
  ScheduledErrorThrower thrower(i_isolate, kAPIMethodName);

  if (!i::wasm::IsWasmCodegenAllowed(i_isolate, i_isolate->native_context())) {
    thrower.CompileError("Wasm code generation disallowed by embedder");
  }

  Local<Context> context = isolate->GetCurrentContext();
  ASSIGN(Promise::Resolver, promise_resolver, Promise::Resolver::New(context));
  Local<Promise> promise = promise_resolver->GetPromise();
  v8::ReturnValue<v8::Value> return_value = args.GetReturnValue();
  return_value.Set(promise);

  std::shared_ptr<i::wasm::CompilationResultResolver> resolver(
      new AsyncCompilationResolver(i_isolate, Utils::OpenHandle(*promise)));

  bool is_shared = false;
  auto bytes = GetFirstArgumentAsBytes(args, &thrower, &is_shared);
  if (thrower.error()) {
    resolver->OnCompilationFailed(thrower.Reify());
    return;
  }
  // Asynchronous compilation handles copying wire bytes if necessary.
  auto enabled_features = i::wasm::WasmFeatures::FromIsolate(i_isolate);
  i_isolate->wasm_engine()->AsyncCompile(i_isolate, enabled_features,
                                         std::move(resolver), bytes, is_shared,
                                         kAPIMethodName);
}

void WasmStreamingCallbackForTesting(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);

  HandleScope scope(isolate);
  ScheduledErrorThrower thrower(i_isolate, "WebAssembly.compile()");

  std::shared_ptr<v8::WasmStreaming> streaming =
      v8::WasmStreaming::Unpack(args.GetIsolate(), args.Data());

  bool is_shared = false;
  i::wasm::ModuleWireBytes bytes =
      GetFirstArgumentAsBytes(args, &thrower, &is_shared);
  if (thrower.error()) {
    streaming->Abort(Utils::ToLocal(thrower.Reify()));
    return;
  }
  streaming->OnBytesReceived(bytes.start(), bytes.length());
  streaming->Finish();
  CHECK(!thrower.error());
}

void WasmStreamingPromiseFailedCallback(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  std::shared_ptr<v8::WasmStreaming> streaming =
      v8::WasmStreaming::Unpack(args.GetIsolate(), args.Data());
  streaming->Abort(args[0]);
}

// WebAssembly.compileStreaming(Response | Promise<Response>)
//   -> Promise<WebAssembly.Module>
void WebAssemblyCompileStreaming(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  HandleScope scope(isolate);
  const char* const kAPIMethodName = "WebAssembly.compileStreaming()";
  ScheduledErrorThrower thrower(i_isolate, kAPIMethodName);
  Local<Context> context = isolate->GetCurrentContext();

  // Create and assign the return value of this function.
  ASSIGN(Promise::Resolver, result_resolver, Promise::Resolver::New(context));
  Local<Promise> promise = result_resolver->GetPromise();
  v8::ReturnValue<v8::Value> return_value = args.GetReturnValue();
  return_value.Set(promise);

  // Prepare the CompilationResultResolver for the compilation.
  auto resolver = std::make_shared<AsyncCompilationResolver>(
      i_isolate, Utils::OpenHandle(*promise));

  if (!i::wasm::IsWasmCodegenAllowed(i_isolate, i_isolate->native_context())) {
    thrower.CompileError("Wasm code generation disallowed by embedder");
    resolver->OnCompilationFailed(thrower.Reify());
    return;
  }

  // Allocate the streaming decoder in a Managed so we can pass it to the
  // embedder.
  i::Handle<i::Managed<WasmStreaming>> data =
      i::Managed<WasmStreaming>::Allocate(
          i_isolate, 0,
          std::make_unique<WasmStreaming::WasmStreamingImpl>(
              isolate, kAPIMethodName, resolver));

  DCHECK_NOT_NULL(i_isolate->wasm_streaming_callback());
  ASSIGN(
      v8::Function, compile_callback,
      v8::Function::New(context, i_isolate->wasm_streaming_callback(),
                        Utils::ToLocal(i::Handle<i::Object>::cast(data)), 1));
  ASSIGN(
      v8::Function, reject_callback,
      v8::Function::New(context, WasmStreamingPromiseFailedCallback,
                        Utils::ToLocal(i::Handle<i::Object>::cast(data)), 1));

  // The parameter may be of type {Response} or of type {Promise<Response>}.
  // Treat either case of parameter as Promise.resolve(parameter)
  // as per https://www.w3.org/2001/tag/doc/promises-guide#resolve-arguments

  // Ending with:
  //    return Promise.resolve(parameter).then(compile_callback);
  ASSIGN(Promise::Resolver, input_resolver, Promise::Resolver::New(context));
  if (!input_resolver->Resolve(context, args[0]).IsJust()) return;

  // We do not have any use of the result here. The {compile_callback} will
  // start streaming compilation, which will eventually resolve the promise we
  // set as result value.
  USE(input_resolver->GetPromise()->Then(context, compile_callback,
                                         reject_callback));
}

// WebAssembly.validate(bytes) -> bool
void WebAssemblyValidate(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  HandleScope scope(isolate);
  ScheduledErrorThrower thrower(i_isolate, "WebAssembly.validate()");

  bool is_shared = false;
  auto bytes = GetFirstArgumentAsBytes(args, &thrower, &is_shared);

  v8::ReturnValue<v8::Value> return_value = args.GetReturnValue();

  if (thrower.error()) {
    if (thrower.wasm_error()) thrower.Reset();  // Clear error.
    return_value.Set(v8::False(isolate));
    return;
  }

  auto enabled_features = i::wasm::WasmFeatures::FromIsolate(i_isolate);
  bool validated = false;
  if (is_shared) {
    // Make a copy of the wire bytes to avoid concurrent modification.
    std::unique_ptr<uint8_t[]> copy(new uint8_t[bytes.length()]);
    base::Memcpy(copy.get(), bytes.start(), bytes.length());
    i::wasm::ModuleWireBytes bytes_copy(copy.get(),
                                        copy.get() + bytes.length());
    validated = i_isolate->wasm_engine()->SyncValidate(
        i_isolate, enabled_features, bytes_copy);
  } else {
    // The wire bytes are not shared, OK to use them directly.
    validated = i_isolate->wasm_engine()->SyncValidate(i_isolate,
                                                       enabled_features, bytes);
  }

  return_value.Set(Boolean::New(isolate, validated));
}

// new WebAssembly.Module(bytes) -> WebAssembly.Module
void WebAssemblyModule(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  if (i_isolate->wasm_module_callback()(args)) return;

  HandleScope scope(isolate);
  ScheduledErrorThrower thrower(i_isolate, "WebAssembly.Module()");

  if (!args.IsConstructCall()) {
    thrower.TypeError("WebAssembly.Module must be invoked with 'new'");
    return;
  }
  if (!i::wasm::IsWasmCodegenAllowed(i_isolate, i_isolate->native_context())) {
    thrower.CompileError("Wasm code generation disallowed by embedder");
    return;
  }

  bool is_shared = false;
  auto bytes = GetFirstArgumentAsBytes(args, &thrower, &is_shared);

  if (thrower.error()) {
    return;
  }
  auto enabled_features = i::wasm::WasmFeatures::FromIsolate(i_isolate);
  i::MaybeHandle<i::Object> module_obj;
  if (is_shared) {
    // Make a copy of the wire bytes to avoid concurrent modification.
    std::unique_ptr<uint8_t[]> copy(new uint8_t[bytes.length()]);
    base::Memcpy(copy.get(), bytes.start(), bytes.length());
    i::wasm::ModuleWireBytes bytes_copy(copy.get(),
                                        copy.get() + bytes.length());
    module_obj = i_isolate->wasm_engine()->SyncCompile(
        i_isolate, enabled_features, &thrower, bytes_copy);
  } else {
    // The wire bytes are not shared, OK to use them directly.
    module_obj = i_isolate->wasm_engine()->SyncCompile(
        i_isolate, enabled_features, &thrower, bytes);
  }

  if (module_obj.is_null()) return;

  v8::ReturnValue<v8::Value> return_value = args.GetReturnValue();
  return_value.Set(Utils::ToLocal(module_obj.ToHandleChecked()));
}

// WebAssembly.Module.imports(module) -> Array<Import>
void WebAssemblyModuleImports(const v8::FunctionCallbackInfo<v8::Value>& args) {
  HandleScope scope(args.GetIsolate());
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  ScheduledErrorThrower thrower(i_isolate, "WebAssembly.Module.imports()");

  auto maybe_module = GetFirstArgumentAsModule(args, &thrower);
  if (thrower.error()) return;
  auto imports = i::wasm::GetImports(i_isolate, maybe_module.ToHandleChecked());
  args.GetReturnValue().Set(Utils::ToLocal(imports));
}

// WebAssembly.Module.exports(module) -> Array<Export>
void WebAssemblyModuleExports(const v8::FunctionCallbackInfo<v8::Value>& args) {
  HandleScope scope(args.GetIsolate());
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  ScheduledErrorThrower thrower(i_isolate, "WebAssembly.Module.exports()");

  auto maybe_module = GetFirstArgumentAsModule(args, &thrower);
  if (thrower.error()) return;
  auto exports = i::wasm::GetExports(i_isolate, maybe_module.ToHandleChecked());
  args.GetReturnValue().Set(Utils::ToLocal(exports));
}

// WebAssembly.Module.customSections(module, name) -> Array<Section>
void WebAssemblyModuleCustomSections(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  HandleScope scope(args.GetIsolate());
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  ScheduledErrorThrower thrower(i_isolate,
                                "WebAssembly.Module.customSections()");

  auto maybe_module = GetFirstArgumentAsModule(args, &thrower);
  if (thrower.error()) return;

  if (args[1]->IsUndefined()) {
    thrower.TypeError("Argument 1 is required");
    return;
  }

  i::MaybeHandle<i::Object> maybe_name =
      i::Object::ToString(i_isolate, Utils::OpenHandle(*args[1]));
  i::Handle<i::Object> name;
  if (!maybe_name.ToHandle(&name)) return;
  auto custom_sections =
      i::wasm::GetCustomSections(i_isolate, maybe_module.ToHandleChecked(),
                                 i::Handle<i::String>::cast(name), &thrower);
  if (thrower.error()) return;
  args.GetReturnValue().Set(Utils::ToLocal(custom_sections));
}

MaybeLocal<Value> WebAssemblyInstantiateImpl(Isolate* isolate,
                                             Local<Value> module,
                                             Local<Value> ffi) {
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);

  i::MaybeHandle<i::Object> instance_object;
  {
    ScheduledErrorThrower thrower(i_isolate, "WebAssembly.Instance()");

    // TODO(ahaas): These checks on the module should not be necessary here They
    // are just a workaround for https://crbug.com/837417.
    i::Handle<i::Object> module_obj = Utils::OpenHandle(*module);
    if (!module_obj->IsWasmModuleObject()) {
      thrower.TypeError("Argument 0 must be a WebAssembly.Module object");
      return {};
    }

    i::MaybeHandle<i::JSReceiver> maybe_imports =
        GetValueAsImports(ffi, &thrower);
    if (thrower.error()) return {};

    instance_object = i_isolate->wasm_engine()->SyncInstantiate(
        i_isolate, &thrower, i::Handle<i::WasmModuleObject>::cast(module_obj),
        maybe_imports, i::MaybeHandle<i::JSArrayBuffer>());
  }

  DCHECK_EQ(instance_object.is_null(), i_isolate->has_scheduled_exception());
  if (instance_object.is_null()) return {};
  return Utils::ToLocal(instance_object.ToHandleChecked());
}

// new WebAssembly.Instance(module, imports) -> WebAssembly.Instance
void WebAssemblyInstance(const v8::FunctionCallbackInfo<v8::Value>& args) {
  Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  i_isolate->CountUsage(
      v8::Isolate::UseCounterFeature::kWebAssemblyInstantiation);

  HandleScope scope(args.GetIsolate());
  if (i_isolate->wasm_instance_callback()(args)) return;

  ScheduledErrorThrower thrower(i_isolate, "WebAssembly.Instance()");
  if (!args.IsConstructCall()) {
    thrower.TypeError("WebAssembly.Instance must be invoked with 'new'");
    return;
  }

  GetFirstArgumentAsModule(args, &thrower);
  if (thrower.error()) return;

  // If args.Length < 2, this will be undefined - see FunctionCallbackInfo.
  // We'll check for that in WebAssemblyInstantiateImpl.
  Local<Value> data = args[1];

  Local<Value> instance;
  if (WebAssemblyInstantiateImpl(isolate, args[0], data).ToLocal(&instance)) {
    args.GetReturnValue().Set(instance);
  }
}

// WebAssembly.instantiateStreaming(Response | Promise<Response> [, imports])
//   -> Promise<ResultObject>
// (where ResultObject has a "module" and an "instance" field)
void WebAssemblyInstantiateStreaming(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  i_isolate->CountUsage(
      v8::Isolate::UseCounterFeature::kWebAssemblyInstantiation);

  HandleScope scope(isolate);
  Local<Context> context = isolate->GetCurrentContext();
  const char* const kAPIMethodName = "WebAssembly.instantiateStreaming()";
  ScheduledErrorThrower thrower(i_isolate, kAPIMethodName);

  // Create and assign the return value of this function.
  ASSIGN(Promise::Resolver, result_resolver, Promise::Resolver::New(context));
  Local<Promise> promise = result_resolver->GetPromise();
  v8::ReturnValue<v8::Value> return_value = args.GetReturnValue();
  return_value.Set(promise);

  // Create an InstantiateResultResolver in case there is an issue with the
  // passed parameters.
  std::unique_ptr<i::wasm::InstantiationResultResolver> resolver(
      new InstantiateModuleResultResolver(i_isolate,
                                          Utils::OpenHandle(*promise)));

  if (!i::wasm::IsWasmCodegenAllowed(i_isolate, i_isolate->native_context())) {
    thrower.CompileError("Wasm code generation disallowed by embedder");
    resolver->OnInstantiationFailed(thrower.Reify());
    return;
  }

  // If args.Length < 2, this will be undefined - see FunctionCallbackInfo.
  Local<Value> ffi = args[1];
  i::MaybeHandle<i::JSReceiver> maybe_imports =
      GetValueAsImports(ffi, &thrower);

  if (thrower.error()) {
    resolver->OnInstantiationFailed(thrower.Reify());
    return;
  }

  // We start compilation now, we have no use for the
  // {InstantiationResultResolver}.
  resolver.reset();

  std::shared_ptr<i::wasm::CompilationResultResolver> compilation_resolver(
      new AsyncInstantiateCompileResultResolver(
          i_isolate, Utils::OpenHandle(*promise), maybe_imports));

  // Allocate the streaming decoder in a Managed so we can pass it to the
  // embedder.
  i::Handle<i::Managed<WasmStreaming>> data =
      i::Managed<WasmStreaming>::Allocate(
          i_isolate, 0,
          std::make_unique<WasmStreaming::WasmStreamingImpl>(
              isolate, kAPIMethodName, compilation_resolver));

  DCHECK_NOT_NULL(i_isolate->wasm_streaming_callback());
  ASSIGN(
      v8::Function, compile_callback,
      v8::Function::New(context, i_isolate->wasm_streaming_callback(),
                        Utils::ToLocal(i::Handle<i::Object>::cast(data)), 1));
  ASSIGN(
      v8::Function, reject_callback,
      v8::Function::New(context, WasmStreamingPromiseFailedCallback,
                        Utils::ToLocal(i::Handle<i::Object>::cast(data)), 1));

  // The parameter may be of type {Response} or of type {Promise<Response>}.
  // Treat either case of parameter as Promise.resolve(parameter)
  // as per https://www.w3.org/2001/tag/doc/promises-guide#resolve-arguments

  // Ending with:
  //    return Promise.resolve(parameter).then(compile_callback);
  ASSIGN(Promise::Resolver, input_resolver, Promise::Resolver::New(context));
  if (!input_resolver->Resolve(context, args[0]).IsJust()) return;

  // We do not have any use of the result here. The {compile_callback} will
  // start streaming compilation, which will eventually resolve the promise we
  // set as result value.
  USE(input_resolver->GetPromise()->Then(context, compile_callback,
                                         reject_callback));
}

// WebAssembly.instantiate(module, imports) -> WebAssembly.Instance
// WebAssembly.instantiate(bytes, imports) ->
//     {module: WebAssembly.Module, instance: WebAssembly.Instance}
void WebAssemblyInstantiate(const v8::FunctionCallbackInfo<v8::Value>& args) {
  constexpr const char* kAPIMethodName = "WebAssembly.instantiate()";
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  i_isolate->CountUsage(
      v8::Isolate::UseCounterFeature::kWebAssemblyInstantiation);

  ScheduledErrorThrower thrower(i_isolate, kAPIMethodName);

  HandleScope scope(isolate);

  Local<Context> context = isolate->GetCurrentContext();

  ASSIGN(Promise::Resolver, promise_resolver, Promise::Resolver::New(context));
  Local<Promise> promise = promise_resolver->GetPromise();
  args.GetReturnValue().Set(promise);

  std::unique_ptr<i::wasm::InstantiationResultResolver> resolver(
      new InstantiateModuleResultResolver(i_isolate,
                                          Utils::OpenHandle(*promise)));

  Local<Value> first_arg_value = args[0];
  i::Handle<i::Object> first_arg = Utils::OpenHandle(*first_arg_value);
  if (!first_arg->IsJSObject()) {
    thrower.TypeError(
        "Argument 0 must be a buffer source or a WebAssembly.Module object");
    resolver->OnInstantiationFailed(thrower.Reify());
    return;
  }

  // If args.Length < 2, this will be undefined - see FunctionCallbackInfo.
  Local<Value> ffi = args[1];
  i::MaybeHandle<i::JSReceiver> maybe_imports =
      GetValueAsImports(ffi, &thrower);

  if (thrower.error()) {
    resolver->OnInstantiationFailed(thrower.Reify());
    return;
  }

  if (first_arg->IsWasmModuleObject()) {
    i::Handle<i::WasmModuleObject> module_obj =
        i::Handle<i::WasmModuleObject>::cast(first_arg);

    i_isolate->wasm_engine()->AsyncInstantiate(i_isolate, std::move(resolver),
                                               module_obj, maybe_imports);
    return;
  }

  bool is_shared = false;
  auto bytes = GetFirstArgumentAsBytes(args, &thrower, &is_shared);
  if (thrower.error()) {
    resolver->OnInstantiationFailed(thrower.Reify());
    return;
  }

  // We start compilation now, we have no use for the
  // {InstantiationResultResolver}.
  resolver.reset();

  std::shared_ptr<i::wasm::CompilationResultResolver> compilation_resolver(
      new AsyncInstantiateCompileResultResolver(
          i_isolate, Utils::OpenHandle(*promise), maybe_imports));

  // The first parameter is a buffer source, we have to check if we are allowed
  // to compile it.
  if (!i::wasm::IsWasmCodegenAllowed(i_isolate, i_isolate->native_context())) {
    thrower.CompileError("Wasm code generation disallowed by embedder");
    compilation_resolver->OnCompilationFailed(thrower.Reify());
    return;
  }

  // Asynchronous compilation handles copying wire bytes if necessary.
  auto enabled_features = i::wasm::WasmFeatures::FromIsolate(i_isolate);
  i_isolate->wasm_engine()->AsyncCompile(i_isolate, enabled_features,
                                         std::move(compilation_resolver), bytes,
                                         is_shared, kAPIMethodName);
}

bool GetIntegerProperty(v8::Isolate* isolate, ErrorThrower* thrower,
                        Local<Context> context, v8::Local<v8::Value> value,
                        i::Handle<i::String> property_name, int64_t* result,
                        int64_t lower_bound, uint64_t upper_bound) {
  uint32_t number;
  if (!EnforceUint32(property_name, value, context, thrower, &number)) {
    return false;
  }
  if (number < lower_bound) {
    thrower->RangeError("Property '%s': value %" PRIu32
                        " is below the lower bound %" PRIx64,
                        property_name->ToCString().get(), number, lower_bound);
    return false;
  }
  if (number > upper_bound) {
    thrower->RangeError("Property '%s': value %" PRIu32
                        " is above the upper bound %" PRIu64,
                        property_name->ToCString().get(), number, upper_bound);
    return false;
  }

  *result = static_cast<int64_t>(number);
  return true;
}

bool GetOptionalIntegerProperty(v8::Isolate* isolate, ErrorThrower* thrower,
                                Local<Context> context,
                                Local<v8::Object> object,
                                Local<String> property, bool* has_property,
                                int64_t* result, int64_t lower_bound,
                                uint64_t upper_bound) {
  v8::Local<v8::Value> value;
  if (!object->Get(context, property).ToLocal(&value)) {
    return false;
  }

  // Web IDL: dictionary presence
  // https://heycam.github.io/webidl/#dfn-present
  if (value->IsUndefined()) {
    if (has_property != nullptr) *has_property = false;
    return true;
  }

  if (has_property != nullptr) *has_property = true;
  i::Handle<i::String> property_name = v8::Utils::OpenHandle(*property);

  return GetIntegerProperty(isolate, thrower, context, value, property_name,
                            result, lower_bound, upper_bound);
}

// Fetch 'initial' or 'minimum' property from object. If both are provided,
// 'initial' is used.
// TODO(aseemgarg): change behavior when the following bug is resolved:
// https://github.com/WebAssembly/js-types/issues/6
bool GetInitialOrMinimumProperty(v8::Isolate* isolate, ErrorThrower* thrower,
                                 Local<Context> context,
                                 Local<v8::Object> object, int64_t* result,
                                 int64_t lower_bound, uint64_t upper_bound) {
  bool has_initial = false;
  if (!GetOptionalIntegerProperty(isolate, thrower, context, object,
                                  v8_str(isolate, "initial"), &has_initial,
                                  result, lower_bound, upper_bound)) {
    return false;
  }
  auto enabled_features = i::wasm::WasmFeatures::FromFlags();
  if (!has_initial && enabled_features.has_type_reflection()) {
    if (!GetOptionalIntegerProperty(isolate, thrower, context, object,
                                    v8_str(isolate, "minimum"), &has_initial,
                                    result, lower_bound, upper_bound)) {
      return false;
    }
  }
  if (!has_initial) {
    // TODO(aseemgarg): update error message when the spec issue is resolved.
    thrower->TypeError("Property 'initial' is required");
    return false;
  }
  return true;
}

// new WebAssembly.Table(args) -> WebAssembly.Table
void WebAssemblyTable(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  HandleScope scope(isolate);
  ScheduledErrorThrower thrower(i_isolate, "WebAssembly.Module()");
  if (!args.IsConstructCall()) {
    thrower.TypeError("WebAssembly.Table must be invoked with 'new'");
    return;
  }
  if (!args[0]->IsObject()) {
    thrower.TypeError("Argument 0 must be a table descriptor");
    return;
  }
  Local<Context> context = isolate->GetCurrentContext();
  Local<v8::Object> descriptor = Local<Object>::Cast(args[0]);
  i::wasm::ValueType type;
  // The descriptor's 'element'.
  {
    v8::MaybeLocal<v8::Value> maybe =
        descriptor->Get(context, v8_str(isolate, "element"));
    v8::Local<v8::Value> value;
    if (!maybe.ToLocal(&value)) return;
    v8::Local<v8::String> string;
    if (!value->ToString(context).ToLocal(&string)) return;
    auto enabled_features = i::wasm::WasmFeatures::FromFlags();
    // The JS api uses 'anyfunc' instead of 'funcref'.
    if (string->StringEquals(v8_str(isolate, "anyfunc"))) {
      type = i::wasm::kWasmFuncRef;
    } else if (enabled_features.has_reftypes() &&
               string->StringEquals(v8_str(isolate, "externref"))) {
      type = i::wasm::kWasmExternRef;
    } else {
      thrower.TypeError(
          "Descriptor property 'element' must be a WebAssembly reference type");
      return;
    }
  }

  int64_t initial = 0;
  if (!GetInitialOrMinimumProperty(isolate, &thrower, context, descriptor,
                                   &initial, 0,
                                   i::wasm::max_table_init_entries())) {
    return;
  }
  // The descriptor's 'maximum'.
  int64_t maximum = -1;
  bool has_maximum = true;
  if (!GetOptionalIntegerProperty(isolate, &thrower, context, descriptor,
                                  v8_str(isolate, "maximum"), &has_maximum,
                                  &maximum, initial,
                                  std::numeric_limits<uint32_t>::max())) {
    return;
  }

  i::Handle<i::FixedArray> fixed_array;
  i::Handle<i::JSObject> table_obj =
      i::WasmTableObject::New(i_isolate, i::Handle<i::WasmInstanceObject>(),
                              type, static_cast<uint32_t>(initial), has_maximum,
                              static_cast<uint32_t>(maximum), &fixed_array);
  v8::ReturnValue<v8::Value> return_value = args.GetReturnValue();
  return_value.Set(Utils::ToLocal(table_obj));
}

void WebAssemblyMemory(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  HandleScope scope(isolate);
  ScheduledErrorThrower thrower(i_isolate, "WebAssembly.Memory()");
  if (!args.IsConstructCall()) {
    thrower.TypeError("WebAssembly.Memory must be invoked with 'new'");
    return;
  }
  if (!args[0]->IsObject()) {
    thrower.TypeError("Argument 0 must be a memory descriptor");
    return;
  }
  Local<Context> context = isolate->GetCurrentContext();
  Local<v8::Object> descriptor = Local<Object>::Cast(args[0]);

  int64_t initial = 0;
  if (!GetInitialOrMinimumProperty(isolate, &thrower, context, descriptor,
                                   &initial, 0, i::wasm::max_mem_pages())) {
    return;
  }
  // The descriptor's 'maximum'.
  int64_t maximum = -1;
  if (!GetOptionalIntegerProperty(isolate, &thrower, context, descriptor,
                                  v8_str(isolate, "maximum"), nullptr, &maximum,
                                  initial, i::wasm::max_mem_pages())) {
    return;
  }

  auto shared = i::SharedFlag::kNotShared;
  auto enabled_features = i::wasm::WasmFeatures::FromIsolate(i_isolate);
  if (enabled_features.has_threads()) {
    // Shared property of descriptor
    Local<String> shared_key = v8_str(isolate, "shared");
    v8::MaybeLocal<v8::Value> maybe_value =
        descriptor->Get(context, shared_key);
    v8::Local<v8::Value> value;
    if (maybe_value.ToLocal(&value)) {
      shared = value->BooleanValue(isolate) ? i::SharedFlag::kShared
                                            : i::SharedFlag::kNotShared;
    } else {
      DCHECK(i_isolate->has_scheduled_exception());
      return;
    }

    // Throw TypeError if shared is true, and the descriptor has no "maximum"
    if (shared == i::SharedFlag::kShared && maximum == -1) {
      thrower.TypeError(
          "If shared is true, maximum property should be defined.");
      return;
    }
  }

  i::Handle<i::JSObject> memory_obj;
  if (!i::WasmMemoryObject::New(i_isolate, static_cast<uint32_t>(initial),
                                static_cast<uint32_t>(maximum), shared)
           .ToHandle(&memory_obj)) {
    thrower.RangeError("could not allocate memory");
    return;
  }
  if (shared == i::SharedFlag::kShared) {
    i::Handle<i::JSArrayBuffer> buffer(
        i::Handle<i::WasmMemoryObject>::cast(memory_obj)->array_buffer(),
        i_isolate);
    Maybe<bool> result =
        buffer->SetIntegrityLevel(buffer, i::FROZEN, i::kDontThrow);
    if (!result.FromJust()) {
      thrower.TypeError(
          "Status of setting SetIntegrityLevel of buffer is false.");
      return;
    }
  }
  args.GetReturnValue().Set(Utils::ToLocal(memory_obj));
}

// Determines the type encoded in a value type property (e.g. type reflection).
// Returns false if there was an exception, true upon success. On success the
// outgoing {type} is set accordingly, or set to {wasm::kWasmStmt} in case the
// type could not be properly recognized.
bool GetValueType(Isolate* isolate, MaybeLocal<Value> maybe,
                  Local<Context> context, i::wasm::ValueType* type,
                  i::wasm::WasmFeatures enabled_features) {
  v8::Local<v8::Value> value;
  if (!maybe.ToLocal(&value)) return false;
  v8::Local<v8::String> string;
  if (!value->ToString(context).ToLocal(&string)) return false;
  if (string->StringEquals(v8_str(isolate, "i32"))) {
    *type = i::wasm::kWasmI32;
  } else if (string->StringEquals(v8_str(isolate, "f32"))) {
    *type = i::wasm::kWasmF32;
  } else if (string->StringEquals(v8_str(isolate, "i64"))) {
    *type = i::wasm::kWasmI64;
  } else if (string->StringEquals(v8_str(isolate, "f64"))) {
    *type = i::wasm::kWasmF64;
  } else if (enabled_features.has_reftypes() &&
             string->StringEquals(v8_str(isolate, "externref"))) {
    *type = i::wasm::kWasmExternRef;
  } else if (enabled_features.has_reftypes() &&
             string->StringEquals(v8_str(isolate, "anyfunc"))) {
    // The JS api spec uses 'anyfunc' instead of 'funcref'.
    *type = i::wasm::kWasmFuncRef;
  } else if (enabled_features.has_eh() &&
             string->StringEquals(v8_str(isolate, "exnref"))) {
    *type = i::wasm::kWasmExnRef;
  } else if (enabled_features.has_gc() &&
             string->StringEquals(v8_str(isolate, "eqref"))) {
    *type = i::wasm::kWasmEqRef;
  } else {
    // Unrecognized type.
    *type = i::wasm::kWasmStmt;
  }
  return true;
}

// WebAssembly.Global
void WebAssemblyGlobal(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  HandleScope scope(isolate);
  ScheduledErrorThrower thrower(i_isolate, "WebAssembly.Global()");
  if (!args.IsConstructCall()) {
    thrower.TypeError("WebAssembly.Global must be invoked with 'new'");
    return;
  }
  if (!args[0]->IsObject()) {
    thrower.TypeError("Argument 0 must be a global descriptor");
    return;
  }
  Local<Context> context = isolate->GetCurrentContext();
  Local<v8::Object> descriptor = Local<Object>::Cast(args[0]);
  auto enabled_features = i::wasm::WasmFeatures::FromIsolate(i_isolate);

  // The descriptor's 'mutable'.
  bool is_mutable = false;
  {
    Local<String> mutable_key = v8_str(isolate, "mutable");
    v8::MaybeLocal<v8::Value> maybe = descriptor->Get(context, mutable_key);
    v8::Local<v8::Value> value;
    if (maybe.ToLocal(&value)) {
      is_mutable = value->BooleanValue(isolate);
    } else {
      DCHECK(i_isolate->has_scheduled_exception());
      return;
    }
  }

  // The descriptor's type, called 'value'. It is called 'value' because this
  // descriptor is planned to be re-used as the global's type for reflection,
  // so calling it 'type' is redundant.
  i::wasm::ValueType type;
  {
    v8::MaybeLocal<v8::Value> maybe =
        descriptor->Get(context, v8_str(isolate, "value"));
    if (!GetValueType(isolate, maybe, context, &type, enabled_features)) return;
    if (type == i::wasm::kWasmStmt) {
      thrower.TypeError(
          "Descriptor property 'value' must be a WebAssembly type");
      return;
    }
  }

  const uint32_t offset = 0;
  i::MaybeHandle<i::WasmGlobalObject> maybe_global_obj =
      i::WasmGlobalObject::New(i_isolate, i::Handle<i::WasmInstanceObject>(),
                               i::MaybeHandle<i::JSArrayBuffer>(),
                               i::MaybeHandle<i::FixedArray>(), type, offset,
                               is_mutable);

  i::Handle<i::WasmGlobalObject> global_obj;
  if (!maybe_global_obj.ToHandle(&global_obj)) {
    thrower.RangeError("could not allocate memory");
    return;
  }

  // Convert value to a WebAssembly value, the default value is 0.
  Local<v8::Value> value = Local<Value>::Cast(args[1]);
  switch (type.kind()) {
    case i::wasm::ValueType::kI32: {
      int32_t i32_value = 0;
      if (!value->IsUndefined()) {
        v8::Local<v8::Int32> int32_value;
        if (!value->ToInt32(context).ToLocal(&int32_value)) return;
        if (!int32_value->Int32Value(context).To(&i32_value)) return;
      }
      global_obj->SetI32(i32_value);
      break;
    }
    case i::wasm::ValueType::kI64: {
      int64_t i64_value = 0;
      if (!value->IsUndefined()) {
        if (!enabled_features.has_bigint()) {
          thrower.TypeError("Can't set the value of i64 WebAssembly.Global");
          return;
        }

        v8::Local<v8::BigInt> bigint_value;
        if (!value->ToBigInt(context).ToLocal(&bigint_value)) return;
        i64_value = bigint_value->Int64Value();
      }
      global_obj->SetI64(i64_value);
      break;
    }
    case i::wasm::ValueType::kF32: {
      float f32_value = 0;
      if (!value->IsUndefined()) {
        double f64_value = 0;
        v8::Local<v8::Number> number_value;
        if (!value->ToNumber(context).ToLocal(&number_value)) return;
        if (!number_value->NumberValue(context).To(&f64_value)) return;
        f32_value = i::DoubleToFloat32(f64_value);
      }
      global_obj->SetF32(f32_value);
      break;
    }
    case i::wasm::ValueType::kF64: {
      double f64_value = 0;
      if (!value->IsUndefined()) {
        v8::Local<v8::Number> number_value;
        if (!value->ToNumber(context).ToLocal(&number_value)) return;
        if (!number_value->NumberValue(context).To(&f64_value)) return;
      }
      global_obj->SetF64(f64_value);
      break;
    }
    case i::wasm::ValueType::kRef:
    case i::wasm::ValueType::kOptRef: {
      switch (type.heap_representation()) {
        case i::wasm::HeapType::kExtern:
        case i::wasm::HeapType::kExn:
        case i::wasm::HeapType::kAny: {
          if (args.Length() < 2) {
            // When no initial value is provided, we have to use the WebAssembly
            // default value 'null', and not the JS default value 'undefined'.
            global_obj->SetExternRef(i_isolate->factory()->null_value());
            break;
          }
          global_obj->SetExternRef(Utils::OpenHandle(*value));
          break;
        }
        case i::wasm::HeapType::kFunc: {
          if (args.Length() < 2) {
            // When no initial value is provided, we have to use the WebAssembly
            // default value 'null', and not the JS default value 'undefined'.
            global_obj->SetFuncRef(i_isolate,
                                   i_isolate->factory()->null_value());
            break;
          }

          if (!global_obj->SetFuncRef(i_isolate, Utils::OpenHandle(*value))) {
            thrower.TypeError(
                "The value of funcref globals must be null or an "
                "exported function");
          }
          break;
        }
        case i::wasm::HeapType::kEq:
        default:
          // TODO(7748): Implement these.
          UNIMPLEMENTED();
      }
      break;
    }
    case i::wasm::ValueType::kRtt:
      // TODO(7748): Implement.
      UNIMPLEMENTED();
    case i::wasm::ValueType::kI8:
    case i::wasm::ValueType::kI16:
    case i::wasm::ValueType::kStmt:
    case i::wasm::ValueType::kS128:
    case i::wasm::ValueType::kBottom:
      UNREACHABLE();
  }

  i::Handle<i::JSObject> global_js_object(global_obj);
  args.GetReturnValue().Set(Utils::ToLocal(global_js_object));
}

// WebAssembly.Exception
void WebAssemblyException(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  HandleScope scope(isolate);
  ScheduledErrorThrower thrower(i_isolate, "WebAssembly.Exception()");
  thrower.TypeError("WebAssembly.Exception cannot be called");
}

namespace {

uint32_t GetIterableLength(i::Isolate* isolate, Local<Context> context,
                           Local<Object> iterable) {
  Local<String> length = Utils::ToLocal(isolate->factory()->length_string());
  MaybeLocal<Value> property = iterable->Get(context, length);
  if (property.IsEmpty()) return i::kMaxUInt32;
  MaybeLocal<Uint32> number = property.ToLocalChecked()->ToArrayIndex(context);
  if (number.IsEmpty()) return i::kMaxUInt32;
  DCHECK_NE(i::kMaxUInt32, number.ToLocalChecked()->Value());
  return number.ToLocalChecked()->Value();
}

}  // namespace

// WebAssembly.Function
void WebAssemblyFunction(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  HandleScope scope(isolate);
  ScheduledErrorThrower thrower(i_isolate, "WebAssembly.Function()");
  if (!args.IsConstructCall()) {
    thrower.TypeError("WebAssembly.Function must be invoked with 'new'");
    return;
  }
  if (!args[0]->IsObject()) {
    thrower.TypeError("Argument 0 must be a function type");
    return;
  }
  Local<Object> function_type = Local<Object>::Cast(args[0]);
  Local<Context> context = isolate->GetCurrentContext();
  auto enabled_features = i::wasm::WasmFeatures::FromIsolate(i_isolate);

  // Load the 'parameters' property of the function type.
  Local<String> parameters_key = v8_str(isolate, "parameters");
  v8::MaybeLocal<v8::Value> parameters_maybe =
      function_type->Get(context, parameters_key);
  v8::Local<v8::Value> parameters_value;
  if (!parameters_maybe.ToLocal(&parameters_value)) return;
  if (!parameters_value->IsObject()) {
    thrower.TypeError("Argument 0 must be a function type with 'parameters'");
    return;
  }
  Local<Object> parameters = parameters_value.As<Object>();
  uint32_t parameters_len = GetIterableLength(i_isolate, context, parameters);
  if (parameters_len == i::kMaxUInt32) {
    thrower.TypeError("Argument 0 contains parameters without 'length'");
    return;
  }
  if (parameters_len > i::wasm::kV8MaxWasmFunctionParams) {
    thrower.TypeError("Argument 0 contains too many parameters");
    return;
  }

  // Load the 'results' property of the function type.
  Local<String> results_key = v8_str(isolate, "results");
  v8::MaybeLocal<v8::Value> results_maybe =
      function_type->Get(context, results_key);
  v8::Local<v8::Value> results_value;
  if (!results_maybe.ToLocal(&results_value)) return;
  if (!results_value->IsObject()) {
    thrower.TypeError("Argument 0 must be a function type with 'results'");
    return;
  }
  Local<Object> results = results_value.As<Object>();
  uint32_t results_len = GetIterableLength(i_isolate, context, results);
  if (results_len == i::kMaxUInt32) {
    thrower.TypeError("Argument 0 contains results without 'length'");
    return;
  }
  if (results_len > (enabled_features.has_mv()
                         ? i::wasm::kV8MaxWasmFunctionMultiReturns
                         : i::wasm::kV8MaxWasmFunctionReturns)) {
    thrower.TypeError("Argument 0 contains too many results");
    return;
  }

  // Decode the function type and construct a signature.
  i::Zone zone(i_isolate->allocator(), ZONE_NAME);
  i::wasm::FunctionSig::Builder builder(&zone, results_len, parameters_len);
  for (uint32_t i = 0; i < parameters_len; ++i) {
    i::wasm::ValueType type;
    MaybeLocal<Value> maybe = parameters->Get(context, i);
    if (!GetValueType(isolate, maybe, context, &type, enabled_features)) return;
    if (type == i::wasm::kWasmStmt) {
      thrower.TypeError(
          "Argument 0 parameter type at index #%u must be a value type", i);
      return;
    }
    builder.AddParam(type);
  }
  for (uint32_t i = 0; i < results_len; ++i) {
    i::wasm::ValueType type;
    MaybeLocal<Value> maybe = results->Get(context, i);
    if (!GetValueType(isolate, maybe, context, &type, enabled_features)) return;
    if (type == i::wasm::kWasmStmt) {
      thrower.TypeError(
          "Argument 0 result type at index #%u must be a value type", i);
      return;
    }
    builder.AddReturn(type);
  }

  if (!args[1]->IsFunction()) {
    thrower.TypeError("Argument 1 must be a function");
    return;
  }
  const i::wasm::FunctionSig* sig = builder.Build();

  i::Handle<i::JSReceiver> callable =
      Utils::OpenHandle(*args[1].As<Function>());
  if (i::WasmExportedFunction::IsWasmExportedFunction(*callable)) {
    if (*i::Handle<i::WasmExportedFunction>::cast(callable)->sig() == *sig) {
      args.GetReturnValue().Set(Utils::ToLocal(callable));
      return;
    }

    thrower.TypeError(
        "The signature of Argument 1 (a WebAssembly function) does "
        "not match the signature specified in Argument 0");
    return;
  }

  if (i::WasmJSFunction::IsWasmJSFunction(*callable)) {
    if (i::Handle<i::WasmJSFunction>::cast(callable)->MatchesSignature(sig)) {
      args.GetReturnValue().Set(Utils::ToLocal(callable));
      return;
    }

    thrower.TypeError(
        "The signature of Argument 1 (a WebAssembly function) does "
        "not match the signature specified in Argument 0");
    return;
  }

  i::Handle<i::JSFunction> result =
      i::WasmJSFunction::New(i_isolate, sig, callable);
  args.GetReturnValue().Set(Utils::ToLocal(result));
}

// WebAssembly.Function.type(WebAssembly.Function) -> FunctionType
void WebAssemblyFunctionType(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  HandleScope scope(isolate);
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  ScheduledErrorThrower thrower(i_isolate, "WebAssembly.Function.type()");

  const i::wasm::FunctionSig* sig;
  i::Zone zone(i_isolate->allocator(), ZONE_NAME);
  i::Handle<i::Object> arg0 = Utils::OpenHandle(*args[0]);
  if (i::WasmExportedFunction::IsWasmExportedFunction(*arg0)) {
    sig = i::Handle<i::WasmExportedFunction>::cast(arg0)->sig();
  } else if (i::WasmJSFunction::IsWasmJSFunction(*arg0)) {
    sig = i::Handle<i::WasmJSFunction>::cast(arg0)->GetSignature(&zone);
  } else {
    thrower.TypeError("Argument 0 must be a WebAssembly.Function");
    return;
  }

  auto type = i::wasm::GetTypeForFunction(i_isolate, sig);
  args.GetReturnValue().Set(Utils::ToLocal(type));
}

constexpr const char* kName_WasmGlobalObject = "WebAssembly.Global";
constexpr const char* kName_WasmMemoryObject = "WebAssembly.Memory";
constexpr const char* kName_WasmInstanceObject = "WebAssembly.Instance";
constexpr const char* kName_WasmTableObject = "WebAssembly.Table";

#define EXTRACT_THIS(var, WasmType)                                  \
  i::Handle<i::WasmType> var;                                        \
  {                                                                  \
    i::Handle<i::Object> this_arg = Utils::OpenHandle(*args.This()); \
    if (!this_arg->Is##WasmType()) {                                 \
      thrower.TypeError("Receiver is not a %s", kName_##WasmType);   \
      return;                                                        \
    }                                                                \
    var = i::Handle<i::WasmType>::cast(this_arg);                    \
  }

void WebAssemblyInstanceGetExports(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  HandleScope scope(isolate);
  ScheduledErrorThrower thrower(i_isolate, "WebAssembly.Instance.exports()");
  EXTRACT_THIS(receiver, WasmInstanceObject);
  i::Handle<i::JSObject> exports_object(receiver->exports_object(), i_isolate);
  args.GetReturnValue().Set(Utils::ToLocal(exports_object));
}

void WebAssemblyTableGetLength(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  HandleScope scope(isolate);
  ScheduledErrorThrower thrower(i_isolate, "WebAssembly.Table.length()");
  EXTRACT_THIS(receiver, WasmTableObject);
  args.GetReturnValue().Set(
      v8::Number::New(isolate, receiver->current_length()));
}

// WebAssembly.Table.grow(num, init_value = null) -> num
void WebAssemblyTableGrow(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  HandleScope scope(isolate);
  ScheduledErrorThrower thrower(i_isolate, "WebAssembly.Table.grow()");
  Local<Context> context = isolate->GetCurrentContext();
  EXTRACT_THIS(receiver, WasmTableObject);

  uint32_t grow_by;
  if (!EnforceUint32("Argument 0", args[0], context, &thrower, &grow_by)) {
    return;
  }

  i::Handle<i::Object> init_value = i_isolate->factory()->null_value();
  auto enabled_features = i::wasm::WasmFeatures::FromIsolate(i_isolate);
  if (enabled_features.has_typed_funcref()) {
    if (args.Length() >= 2 && !args[1]->IsUndefined()) {
      init_value = Utils::OpenHandle(*args[1]);
    }
    if (!i::WasmTableObject::IsValidElement(i_isolate, receiver, init_value)) {
      thrower.TypeError("Argument 1 must be a valid type for the table");
      return;
    }
  }

  int old_size =
      i::WasmTableObject::Grow(i_isolate, receiver, grow_by, init_value);

  if (old_size < 0) {
    thrower.RangeError("failed to grow table by %u", grow_by);
    return;
  }
  v8::ReturnValue<v8::Value> return_value = args.GetReturnValue();
  return_value.Set(old_size);
}

// WebAssembly.Table.get(num) -> JSFunction
void WebAssemblyTableGet(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  HandleScope scope(isolate);
  ScheduledErrorThrower thrower(i_isolate, "WebAssembly.Table.get()");
  Local<Context> context = isolate->GetCurrentContext();
  EXTRACT_THIS(receiver, WasmTableObject);

  uint32_t index;
  if (!EnforceUint32("Argument 0", args[0], context, &thrower, &index)) {
    return;
  }
  if (!i::WasmTableObject::IsInBounds(i_isolate, receiver, index)) {
    thrower.RangeError("invalid index %u into function table", index);
    return;
  }

  i::Handle<i::Object> result =
      i::WasmTableObject::Get(i_isolate, receiver, index);

  v8::ReturnValue<v8::Value> return_value = args.GetReturnValue();
  return_value.Set(Utils::ToLocal(result));
}

// WebAssembly.Table.set(num, JSFunction)
void WebAssemblyTableSet(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  HandleScope scope(isolate);
  ScheduledErrorThrower thrower(i_isolate, "WebAssembly.Table.set()");
  Local<Context> context = isolate->GetCurrentContext();
  EXTRACT_THIS(table_object, WasmTableObject);

  // Parameter 0.
  uint32_t index;
  if (!EnforceUint32("Argument 0", args[0], context, &thrower, &index)) {
    return;
  }
  if (!i::WasmTableObject::IsInBounds(i_isolate, table_object, index)) {
    thrower.RangeError("invalid index %u into function table", index);
    return;
  }

  i::Handle<i::Object> element = Utils::OpenHandle(*args[1]);
  if (!i::WasmTableObject::IsValidElement(i_isolate, table_object, element)) {
    thrower.TypeError(
        "Argument 1 must be null or a WebAssembly function of type compatible "
        "to 'this'");
    return;
  }
  i::WasmTableObject::Set(i_isolate, table_object, index, element);
}

// WebAssembly.Table.type(WebAssembly.Table) -> TableType
void WebAssemblyTableType(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  HandleScope scope(isolate);
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  ScheduledErrorThrower thrower(i_isolate, "WebAssembly.Table.type()");

  auto maybe_table = GetFirstArgumentAsTable(args, &thrower);
  if (thrower.error()) return;
  i::Handle<i::WasmTableObject> table = maybe_table.ToHandleChecked();
  base::Optional<uint32_t> max_size;
  if (!table->maximum_length().IsUndefined()) {
    uint64_t max_size64 = table->maximum_length().Number();
    DCHECK_LE(max_size64, std::numeric_limits<uint32_t>::max());
    max_size.emplace(static_cast<uint32_t>(max_size64));
  }
  auto type = i::wasm::GetTypeForTable(i_isolate, table->type(),
                                       table->current_length(), max_size);
  args.GetReturnValue().Set(Utils::ToLocal(type));
}

// WebAssembly.Memory.grow(num) -> num
void WebAssemblyMemoryGrow(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  HandleScope scope(isolate);
  ScheduledErrorThrower thrower(i_isolate, "WebAssembly.Memory.grow()");
  Local<Context> context = isolate->GetCurrentContext();
  EXTRACT_THIS(receiver, WasmMemoryObject);

  uint32_t delta_size;
  if (!EnforceUint32("Argument 0", args[0], context, &thrower, &delta_size)) {
    return;
  }

  uint64_t max_size64 = receiver->maximum_pages();
  if (max_size64 > uint64_t{i::wasm::max_mem_pages()}) {
    max_size64 = i::wasm::max_mem_pages();
  }
  i::Handle<i::JSArrayBuffer> old_buffer(receiver->array_buffer(), i_isolate);

  DCHECK_LE(max_size64, std::numeric_limits<uint32_t>::max());

  uint64_t old_size64 = old_buffer->byte_length() / i::wasm::kWasmPageSize;
  uint64_t new_size64 = old_size64 + static_cast<uint64_t>(delta_size);

  if (new_size64 > max_size64) {
    thrower.RangeError("Maximum memory size exceeded");
    return;
  }

  int32_t ret = i::WasmMemoryObject::Grow(i_isolate, receiver, delta_size);
  if (ret == -1) {
    thrower.RangeError("Unable to grow instance memory.");
    return;
  }
  v8::ReturnValue<v8::Value> return_value = args.GetReturnValue();
  return_value.Set(ret);
}

// WebAssembly.Memory.buffer -> ArrayBuffer
void WebAssemblyMemoryGetBuffer(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  HandleScope scope(isolate);
  ScheduledErrorThrower thrower(i_isolate, "WebAssembly.Memory.buffer");
  EXTRACT_THIS(receiver, WasmMemoryObject);

  i::Handle<i::Object> buffer_obj(receiver->array_buffer(), i_isolate);
  DCHECK(buffer_obj->IsJSArrayBuffer());
  i::Handle<i::JSArrayBuffer> buffer(i::JSArrayBuffer::cast(*buffer_obj),
                                     i_isolate);
  if (buffer->is_shared()) {
    // TODO(gdeepti): More needed here for when cached buffer, and current
    // buffer are out of sync, handle that here when bounds checks, and Grow
    // are handled correctly.
    Maybe<bool> result =
        buffer->SetIntegrityLevel(buffer, i::FROZEN, i::kDontThrow);
    if (!result.FromJust()) {
      thrower.TypeError(
          "Status of setting SetIntegrityLevel of buffer is false.");
    }
  }
  v8::ReturnValue<v8::Value> return_value = args.GetReturnValue();
  return_value.Set(Utils::ToLocal(buffer));
}

// WebAssembly.Memory.type(WebAssembly.Memory) -> MemoryType
void WebAssemblyMemoryType(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  HandleScope scope(isolate);
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  ScheduledErrorThrower thrower(i_isolate, "WebAssembly.Memory.type()");

  auto maybe_memory = GetFirstArgumentAsMemory(args, &thrower);
  if (thrower.error()) return;
  i::Handle<i::WasmMemoryObject> memory = maybe_memory.ToHandleChecked();
  i::Handle<i::JSArrayBuffer> buffer(memory->array_buffer(), i_isolate);
  size_t curr_size = buffer->byte_length() / i::wasm::kWasmPageSize;
  DCHECK_LE(curr_size, std::numeric_limits<uint32_t>::max());
  uint32_t min_size = static_cast<uint32_t>(curr_size);
  base::Optional<uint32_t> max_size;
  if (memory->has_maximum_pages()) {
    uint64_t max_size64 = memory->maximum_pages();
    DCHECK_LE(max_size64, std::numeric_limits<uint32_t>::max());
    max_size.emplace(static_cast<uint32_t>(max_size64));
  }
  auto type = i::wasm::GetTypeForMemory(i_isolate, min_size, max_size);
  args.GetReturnValue().Set(Utils::ToLocal(type));
}

void WebAssemblyGlobalGetValueCommon(
    const v8::FunctionCallbackInfo<v8::Value>& args, const char* name) {
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  HandleScope scope(isolate);
  ScheduledErrorThrower thrower(i_isolate, name);
  EXTRACT_THIS(receiver, WasmGlobalObject);

  v8::ReturnValue<v8::Value> return_value = args.GetReturnValue();

  switch (receiver->type().kind()) {
    case i::wasm::ValueType::kI32:
      return_value.Set(receiver->GetI32());
      break;
    case i::wasm::ValueType::kI64: {
      auto enabled_features = i::wasm::WasmFeatures::FromIsolate(i_isolate);
      if (enabled_features.has_bigint()) {
        Local<BigInt> value = BigInt::New(isolate, receiver->GetI64());
        return_value.Set(value);
      } else {
        thrower.TypeError("Can't get the value of i64 WebAssembly.Global");
      }
      break;
    }
    case i::wasm::ValueType::kF32:
      return_value.Set(receiver->GetF32());
      break;
    case i::wasm::ValueType::kF64:
      return_value.Set(receiver->GetF64());
      break;
    case i::wasm::ValueType::kS128:
      thrower.TypeError("Can't get the value of s128 WebAssembly.Global");
      break;
    case i::wasm::ValueType::kRef:
    case i::wasm::ValueType::kOptRef:
      switch (receiver->type().heap_representation()) {
        case i::wasm::HeapType::kExtern:
        case i::wasm::HeapType::kFunc:
        case i::wasm::HeapType::kExn:
        case i::wasm::HeapType::kAny:
          return_value.Set(Utils::ToLocal(receiver->GetRef()));
          break;
        case i::wasm::HeapType::kEq:
        default:
          // TODO(7748): Implement these.
          UNIMPLEMENTED();
          break;
      }
      break;
    case i::wasm::ValueType::kRtt:
      UNIMPLEMENTED();  // TODO(7748): Implement.
      break;
    case i::wasm::ValueType::kI8:
    case i::wasm::ValueType::kI16:
    case i::wasm::ValueType::kBottom:
    case i::wasm::ValueType::kStmt:
      UNREACHABLE();
  }
}

// WebAssembly.Global.valueOf() -> num
void WebAssemblyGlobalValueOf(const v8::FunctionCallbackInfo<v8::Value>& args) {
  return WebAssemblyGlobalGetValueCommon(args, "WebAssembly.Global.valueOf()");
}

// get WebAssembly.Global.value -> num
void WebAssemblyGlobalGetValue(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  return WebAssemblyGlobalGetValueCommon(args, "get WebAssembly.Global.value");
}

// set WebAssembly.Global.value(num)
void WebAssemblyGlobalSetValue(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  HandleScope scope(isolate);
  Local<Context> context = isolate->GetCurrentContext();
  ScheduledErrorThrower thrower(i_isolate, "set WebAssembly.Global.value");
  EXTRACT_THIS(receiver, WasmGlobalObject);

  if (!receiver->is_mutable()) {
    thrower.TypeError("Can't set the value of an immutable global.");
    return;
  }
  if (args[0]->IsUndefined()) {
    thrower.TypeError("Argument 0 is required");
    return;
  }

  switch (receiver->type().kind()) {
    case i::wasm::ValueType::kI32: {
      int32_t i32_value = 0;
      if (!args[0]->Int32Value(context).To(&i32_value)) return;
      receiver->SetI32(i32_value);
      break;
    }
    case i::wasm::ValueType::kI64: {
      auto enabled_features = i::wasm::WasmFeatures::FromIsolate(i_isolate);
      if (enabled_features.has_bigint()) {
        v8::Local<v8::BigInt> bigint_value;
        if (!args[0]->ToBigInt(context).ToLocal(&bigint_value)) return;
        receiver->SetI64(bigint_value->Int64Value());
      } else {
        thrower.TypeError("Can't set the value of i64 WebAssembly.Global");
      }
      break;
    }
    case i::wasm::ValueType::kF32: {
      double f64_value = 0;
      if (!args[0]->NumberValue(context).To(&f64_value)) return;
      receiver->SetF32(i::DoubleToFloat32(f64_value));
      break;
    }
    case i::wasm::ValueType::kF64: {
      double f64_value = 0;
      if (!args[0]->NumberValue(context).To(&f64_value)) return;
      receiver->SetF64(f64_value);
      break;
    }
    case i::wasm::ValueType::kS128:
      thrower.TypeError("Can't set the value of s128 WebAssembly.Global");
      break;
    case i::wasm::ValueType::kRef:
    case i::wasm::ValueType::kOptRef:
      switch (receiver->type().heap_representation()) {
        case i::wasm::HeapType::kExtern:
        case i::wasm::HeapType::kExn:
        case i::wasm::HeapType::kAny:
          receiver->SetExternRef(Utils::OpenHandle(*args[0]));
          break;
        case i::wasm::HeapType::kFunc: {
          if (!receiver->SetFuncRef(i_isolate, Utils::OpenHandle(*args[0]))) {
            thrower.TypeError(
                "value of an funcref reference must be either null or an "
                "exported function");
          }
          break;
        }

        case i::wasm::HeapType::kEq:
        default:
          // TODO(7748): Implement these.
          UNIMPLEMENTED();
          break;
      }
      break;
    case i::wasm::ValueType::kRtt:
      // TODO(7748): Implement.
      UNIMPLEMENTED();
      break;
    case i::wasm::ValueType::kI8:
    case i::wasm::ValueType::kI16:
    case i::wasm::ValueType::kBottom:
    case i::wasm::ValueType::kStmt:
      UNREACHABLE();
  }
}

// WebAssembly.Global.type(WebAssembly.Global) -> GlobalType
void WebAssemblyGlobalType(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  HandleScope scope(isolate);
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  ScheduledErrorThrower thrower(i_isolate, "WebAssembly.Global.type()");

  auto maybe_global = GetFirstArgumentAsGlobal(args, &thrower);
  if (thrower.error()) return;
  i::Handle<i::WasmGlobalObject> global = maybe_global.ToHandleChecked();
  auto type = i::wasm::GetTypeForGlobal(i_isolate, global->is_mutable(),
                                        global->type());
  args.GetReturnValue().Set(Utils::ToLocal(type));
}

}  // namespace

// TODO(titzer): we use the API to create the function template because the
// internal guts are too ugly to replicate here.
static i::Handle<i::FunctionTemplateInfo> NewFunctionTemplate(
    i::Isolate* i_isolate, FunctionCallback func, bool has_prototype,
    SideEffectType side_effect_type = SideEffectType::kHasSideEffect) {
  Isolate* isolate = reinterpret_cast<Isolate*>(i_isolate);
  Local<FunctionTemplate> templ = FunctionTemplate::New(
      isolate, func, {}, {}, 0, ConstructorBehavior::kAllow, side_effect_type);
  has_prototype ? templ->ReadOnlyPrototype() : templ->RemovePrototype();
  return v8::Utils::OpenHandle(*templ);
}

static i::Handle<i::ObjectTemplateInfo> NewObjectTemplate(
    i::Isolate* i_isolate) {
  Isolate* isolate = reinterpret_cast<Isolate*>(i_isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  return v8::Utils::OpenHandle(*templ);
}

namespace internal {

Handle<JSFunction> CreateFunc(
    Isolate* isolate, Handle<String> name, FunctionCallback func,
    bool has_prototype,
    SideEffectType side_effect_type = SideEffectType::kHasSideEffect) {
  Handle<FunctionTemplateInfo> temp =
      NewFunctionTemplate(isolate, func, has_prototype, side_effect_type);
  Handle<JSFunction> function =
      ApiNatives::InstantiateFunction(temp, name).ToHandleChecked();
  DCHECK(function->shared().HasSharedName());
  return function;
}

Handle<JSFunction> InstallFunc(
    Isolate* isolate, Handle<JSObject> object, const char* str,
    FunctionCallback func, int length, bool has_prototype = false,
    PropertyAttributes attributes = NONE,
    SideEffectType side_effect_type = SideEffectType::kHasSideEffect) {
  Handle<String> name = v8_str(isolate, str);
  Handle<JSFunction> function =
      CreateFunc(isolate, name, func, has_prototype, side_effect_type);
  function->shared().set_length(length);
  JSObject::AddProperty(isolate, object, name, function, attributes);
  return function;
}

Handle<JSFunction> InstallConstructorFunc(Isolate* isolate,
                                          Handle<JSObject> object,
                                          const char* str,
                                          FunctionCallback func) {
  return InstallFunc(isolate, object, str, func, 1, true, DONT_ENUM);
}

Handle<String> GetterName(Isolate* isolate, Handle<String> name) {
  return Name::ToFunctionName(isolate, name, isolate->factory()->get_string())
      .ToHandleChecked();
}

void InstallGetter(Isolate* isolate, Handle<JSObject> object, const char* str,
                   FunctionCallback func) {
  Handle<String> name = v8_str(isolate, str);
  Handle<JSFunction> function =
      CreateFunc(isolate, GetterName(isolate, name), func, false,
                 SideEffectType::kHasNoSideEffect);

  Utils::ToLocal(object)->SetAccessorProperty(Utils::ToLocal(name),
                                              Utils::ToLocal(function),
                                              Local<Function>(), v8::None);
}

Handle<String> SetterName(Isolate* isolate, Handle<String> name) {
  return Name::ToFunctionName(isolate, name, isolate->factory()->set_string())
      .ToHandleChecked();
}

void InstallGetterSetter(Isolate* isolate, Handle<JSObject> object,
                         const char* str, FunctionCallback getter,
                         FunctionCallback setter) {
  Handle<String> name = v8_str(isolate, str);
  Handle<JSFunction> getter_func =
      CreateFunc(isolate, GetterName(isolate, name), getter, false);
  Handle<JSFunction> setter_func =
      CreateFunc(isolate, SetterName(isolate, name), setter, false);
  setter_func->shared().set_length(1);

  Utils::ToLocal(object)->SetAccessorProperty(
      Utils::ToLocal(name), Utils::ToLocal(getter_func),
      Utils::ToLocal(setter_func), v8::None);
}

// Assigns a dummy instance template to the given constructor function. Used to
// make sure the implicit receivers for the constructors in this file have an
// instance type different from the internal one, they allocate the resulting
// object explicitly and ignore implicit receiver.
void SetDummyInstanceTemplate(Isolate* isolate, Handle<JSFunction> fun) {
  Handle<ObjectTemplateInfo> instance_template = NewObjectTemplate(isolate);
  FunctionTemplateInfo::SetInstanceTemplate(
      isolate, handle(fun->shared().get_api_func_data(), isolate),
      instance_template);
}

// static
void WasmJs::Install(Isolate* isolate, bool exposed_on_global_object) {
  Handle<JSGlobalObject> global = isolate->global_object();
  Handle<Context> context(global->native_context(), isolate);
  // Install the JS API once only.
  Object prev = context->get(Context::WASM_MODULE_CONSTRUCTOR_INDEX);
  if (!prev.IsUndefined(isolate)) {
    DCHECK(prev.IsJSFunction());
    return;
  }

  Factory* factory = isolate->factory();

  // Setup WebAssembly
  Handle<String> name = v8_str(isolate, "WebAssembly");
  // Not supposed to be called, hence using the kIllegal builtin as code.
  Handle<SharedFunctionInfo> info =
      factory->NewSharedFunctionInfoForBuiltin(name, Builtins::kIllegal);
  info->set_language_mode(LanguageMode::kStrict);

  Handle<JSFunction> cons =
      Factory::JSFunctionBuilder{isolate, info, context}.Build();
  JSFunction::SetPrototype(cons, isolate->initial_object_prototype());
  Handle<JSObject> webassembly =
      factory->NewJSObject(cons, AllocationType::kOld);

  PropertyAttributes ro_attributes =
      static_cast<PropertyAttributes>(DONT_ENUM | READ_ONLY);
  JSObject::AddProperty(isolate, webassembly, factory->to_string_tag_symbol(),
                        name, ro_attributes);
  InstallFunc(isolate, webassembly, "compile", WebAssemblyCompile, 1);
  InstallFunc(isolate, webassembly, "validate", WebAssemblyValidate, 1);
  InstallFunc(isolate, webassembly, "instantiate", WebAssemblyInstantiate, 1);

  if (FLAG_wasm_test_streaming) {
    isolate->set_wasm_streaming_callback(WasmStreamingCallbackForTesting);
  }

  if (isolate->wasm_streaming_callback() != nullptr) {
    InstallFunc(isolate, webassembly, "compileStreaming",
                WebAssemblyCompileStreaming, 1);
    InstallFunc(isolate, webassembly, "instantiateStreaming",
                WebAssemblyInstantiateStreaming, 1);
  }

  // Expose the API on the global object if configured to do so.
  if (exposed_on_global_object) {
    JSObject::AddProperty(isolate, global, name, webassembly, DONT_ENUM);
  }

  // Setup Module
  Handle<JSFunction> module_constructor =
      InstallConstructorFunc(isolate, webassembly, "Module", WebAssemblyModule);
  context->set_wasm_module_constructor(*module_constructor);
  SetDummyInstanceTemplate(isolate, module_constructor);
  JSFunction::EnsureHasInitialMap(module_constructor);
  Handle<JSObject> module_proto(
      JSObject::cast(module_constructor->instance_prototype()), isolate);
  Handle<Map> module_map = isolate->factory()->NewMap(
      i::WASM_MODULE_OBJECT_TYPE, WasmModuleObject::kHeaderSize);
  JSFunction::SetInitialMap(module_constructor, module_map, module_proto);
  InstallFunc(isolate, module_constructor, "imports", WebAssemblyModuleImports,
              1);
  InstallFunc(isolate, module_constructor, "exports", WebAssemblyModuleExports,
              1);
  InstallFunc(isolate, module_constructor, "customSections",
              WebAssemblyModuleCustomSections, 2);
  JSObject::AddProperty(isolate, module_proto, factory->to_string_tag_symbol(),
                        v8_str(isolate, "WebAssembly.Module"), ro_attributes);

  // Setup Instance
  Handle<JSFunction> instance_constructor = InstallConstructorFunc(
      isolate, webassembly, "Instance", WebAssemblyInstance);
  context->set_wasm_instance_constructor(*instance_constructor);
  SetDummyInstanceTemplate(isolate, instance_constructor);
  JSFunction::EnsureHasInitialMap(instance_constructor);
  Handle<JSObject> instance_proto(
      JSObject::cast(instance_constructor->instance_prototype()), isolate);
  Handle<Map> instance_map = isolate->factory()->NewMap(
      i::WASM_INSTANCE_OBJECT_TYPE, WasmInstanceObject::kHeaderSize);
  JSFunction::SetInitialMap(instance_constructor, instance_map, instance_proto);
  InstallGetter(isolate, instance_proto, "exports",
                WebAssemblyInstanceGetExports);
  JSObject::AddProperty(isolate, instance_proto,
                        factory->to_string_tag_symbol(),
                        v8_str(isolate, "WebAssembly.Instance"), ro_attributes);

  // The context is not set up completely yet. That's why we cannot use
  // {WasmFeatures::FromIsolate} and have to use {WasmFeatures::FromFlags}
  // instead.
  auto enabled_features = i::wasm::WasmFeatures::FromFlags();

  // Setup Table
  Handle<JSFunction> table_constructor =
      InstallConstructorFunc(isolate, webassembly, "Table", WebAssemblyTable);
  context->set_wasm_table_constructor(*table_constructor);
  SetDummyInstanceTemplate(isolate, table_constructor);
  JSFunction::EnsureHasInitialMap(table_constructor);
  Handle<JSObject> table_proto(
      JSObject::cast(table_constructor->instance_prototype()), isolate);
  Handle<Map> table_map = isolate->factory()->NewMap(
      i::WASM_TABLE_OBJECT_TYPE, WasmTableObject::kHeaderSize);
  JSFunction::SetInitialMap(table_constructor, table_map, table_proto);
  InstallGetter(isolate, table_proto, "length", WebAssemblyTableGetLength);
  InstallFunc(isolate, table_proto, "grow", WebAssemblyTableGrow, 1);
  InstallFunc(isolate, table_proto, "get", WebAssemblyTableGet, 1);
  InstallFunc(isolate, table_proto, "set", WebAssemblyTableSet, 2);
  if (enabled_features.has_type_reflection()) {
    InstallFunc(isolate, table_constructor, "type", WebAssemblyTableType, 1);
  }
  JSObject::AddProperty(isolate, table_proto, factory->to_string_tag_symbol(),
                        v8_str(isolate, "WebAssembly.Table"), ro_attributes);

  // Setup Memory
  Handle<JSFunction> memory_constructor =
      InstallConstructorFunc(isolate, webassembly, "Memory", WebAssemblyMemory);
  context->set_wasm_memory_constructor(*memory_constructor);
  SetDummyInstanceTemplate(isolate, memory_constructor);
  JSFunction::EnsureHasInitialMap(memory_constructor);
  Handle<JSObject> memory_proto(
      JSObject::cast(memory_constructor->instance_prototype()), isolate);
  Handle<Map> memory_map = isolate->factory()->NewMap(
      i::WASM_MEMORY_OBJECT_TYPE, WasmMemoryObject::kHeaderSize);
  JSFunction::SetInitialMap(memory_constructor, memory_map, memory_proto);
  InstallFunc(isolate, memory_proto, "grow", WebAssemblyMemoryGrow, 1);
  InstallGetter(isolate, memory_proto, "buffer", WebAssemblyMemoryGetBuffer);
  if (enabled_features.has_type_reflection()) {
    InstallFunc(isolate, memory_constructor, "type", WebAssemblyMemoryType, 1);
  }
  JSObject::AddProperty(isolate, memory_proto, factory->to_string_tag_symbol(),
                        v8_str(isolate, "WebAssembly.Memory"), ro_attributes);

  // Setup Global
  Handle<JSFunction> global_constructor =
      InstallConstructorFunc(isolate, webassembly, "Global", WebAssemblyGlobal);
  context->set_wasm_global_constructor(*global_constructor);
  SetDummyInstanceTemplate(isolate, global_constructor);
  JSFunction::EnsureHasInitialMap(global_constructor);
  Handle<JSObject> global_proto(
      JSObject::cast(global_constructor->instance_prototype()), isolate);
  Handle<Map> global_map = isolate->factory()->NewMap(
      i::WASM_GLOBAL_OBJECT_TYPE, WasmGlobalObject::kHeaderSize);
  JSFunction::SetInitialMap(global_constructor, global_map, global_proto);
  InstallFunc(isolate, global_proto, "valueOf", WebAssemblyGlobalValueOf, 0);
  InstallGetterSetter(isolate, global_proto, "value", WebAssemblyGlobalGetValue,
                      WebAssemblyGlobalSetValue);
  if (enabled_features.has_type_reflection()) {
    InstallFunc(isolate, global_constructor, "type", WebAssemblyGlobalType, 1);
  }
  JSObject::AddProperty(isolate, global_proto, factory->to_string_tag_symbol(),
                        v8_str(isolate, "WebAssembly.Global"), ro_attributes);

  // Setup Exception
  if (enabled_features.has_eh()) {
    Handle<JSFunction> exception_constructor = InstallConstructorFunc(
        isolate, webassembly, "Exception", WebAssemblyException);
    context->set_wasm_exception_constructor(*exception_constructor);
    SetDummyInstanceTemplate(isolate, exception_constructor);
    JSFunction::EnsureHasInitialMap(exception_constructor);
    Handle<JSObject> exception_proto(
        JSObject::cast(exception_constructor->instance_prototype()), isolate);
    Handle<Map> exception_map = isolate->factory()->NewMap(
        i::WASM_EXCEPTION_OBJECT_TYPE, WasmExceptionObject::kHeaderSize);
    JSFunction::SetInitialMap(exception_constructor, exception_map,
                              exception_proto);
  }

  // Setup Function
  if (enabled_features.has_type_reflection()) {
    Handle<JSFunction> function_constructor = InstallConstructorFunc(
        isolate, webassembly, "Function", WebAssemblyFunction);
    SetDummyInstanceTemplate(isolate, function_constructor);
    JSFunction::EnsureHasInitialMap(function_constructor);
    Handle<JSObject> function_proto(
        JSObject::cast(function_constructor->instance_prototype()), isolate);
    Handle<Map> function_map = isolate->factory()->CreateSloppyFunctionMap(
        FUNCTION_WITHOUT_PROTOTYPE, MaybeHandle<JSFunction>());
    CHECK(JSObject::SetPrototype(
              function_proto,
              handle(context->function_function().prototype(), isolate), false,
              kDontThrow)
              .FromJust());
    JSFunction::SetInitialMap(function_constructor, function_map,
                              function_proto);
    InstallFunc(isolate, function_constructor, "type", WebAssemblyFunctionType,
                1);
    // Make all exported functions an instance of {WebAssembly.Function}.
    context->set_wasm_exported_function_map(*function_map);
  } else {
    // Make all exported functions an instance of {Function}.
    Handle<Map> function_map = isolate->sloppy_function_without_prototype_map();
    context->set_wasm_exported_function_map(*function_map);
  }

  // Setup errors
  Handle<JSFunction> compile_error(
      isolate->native_context()->wasm_compile_error_function(), isolate);
  JSObject::AddProperty(isolate, webassembly,
                        isolate->factory()->CompileError_string(),
                        compile_error, DONT_ENUM);
  Handle<JSFunction> link_error(
      isolate->native_context()->wasm_link_error_function(), isolate);
  JSObject::AddProperty(isolate, webassembly,
                        isolate->factory()->LinkError_string(), link_error,
                        DONT_ENUM);
  Handle<JSFunction> runtime_error(
      isolate->native_context()->wasm_runtime_error_function(), isolate);
  JSObject::AddProperty(isolate, webassembly,
                        isolate->factory()->RuntimeError_string(),
                        runtime_error, DONT_ENUM);
}

namespace {
void SetMapValue(Isolate* isolate, Handle<JSMap> map, Handle<Object> key,
                 Handle<Object> value) {
  DCHECK(!map.is_null() && !key.is_null() && !value.is_null());
  Handle<Object> argv[] = {key, value};
  Execution::CallBuiltin(isolate, isolate->map_set(), map, arraysize(argv),
                         argv)
      .Check();
}

Handle<Object> GetMapValue(Isolate* isolate, Handle<JSMap> map,
                           Handle<Object> key) {
  DCHECK(!map.is_null() && !key.is_null());
  Handle<Object> argv[] = {key};
  return Execution::CallBuiltin(isolate, isolate->map_get(), map,
                                arraysize(argv), argv)
      .ToHandleChecked();
}

Handle<WasmInstanceObject> GetInstance(Isolate* isolate,
                                       Handle<JSObject> handler) {
  Handle<Object> instance =
      JSObject::GetProperty(isolate, handler, "instance").ToHandleChecked();
  DCHECK(instance->IsWasmInstanceObject());
  return Handle<WasmInstanceObject>::cast(instance);
}

// Populate a JSMap with name->index mappings from an ordered list of names.
Handle<JSMap> GetNameTable(Isolate* isolate,
                           const std::vector<Handle<String>>& names) {
  Factory* factory = isolate->factory();
  Handle<JSMap> name_table = factory->NewJSMap();

  for (size_t i = 0; i < names.size(); ++i) {
    SetMapValue(isolate, name_table, names[i], factory->NewNumberFromInt64(i));
  }
  return name_table;
}

// Look up a  JSMap with name->index mappings from an ordered list of names.
Handle<JSMap> GetOrCreateNameTable(
    Handle<WasmInstanceObject> instance, const char* table_name,
    Handle<JSMap> (*generate_names_callback)(Handle<WasmInstanceObject>)) {
  Isolate* isolate = instance->GetIsolate();
  Handle<Object> tables;
  Handle<Object> name_table;
  Handle<String> table_name_string =
      isolate->factory()->InternalizeUtf8String(table_name);
  Handle<Name> symbol = isolate->factory()->wasm_debug_proxy_name_tables();
  bool has_tables =
      Object::GetProperty(isolate, instance, symbol).ToHandle(&tables) &&
      !tables->IsUndefined();
  if (has_tables) {
    if (Object::GetProperty(isolate, tables, table_name_string)
            .ToHandle(&name_table)) {
      DCHECK(name_table->IsUndefined() || name_table->IsJSMap());
      if (!name_table->IsUndefined()) return Handle<JSMap>::cast(name_table);
    }
  } else {
    tables = isolate->factory()->NewJSObjectWithNullProto();
    Object::SetProperty(isolate, instance, symbol, tables).Check();
  }

  name_table = generate_names_callback(instance);
  Object::SetProperty(isolate, tables, table_name_string, name_table).Check();
  return Handle<JSMap>::cast(name_table);
}

// Look up a name in a name table. Name tables are stored under the "names"
// property of the handler and map names to index.
base::Optional<int> ResolveValueSelector(
    Isolate* isolate, Handle<Name> property, Handle<JSObject> handler,
    bool enable_index_lookup, const char* table_name = nullptr,
    Handle<JSMap> (*generate_names_callback)(Handle<WasmInstanceObject>) =
        nullptr) {
  size_t index = 0;
  if (enable_index_lookup && property->AsIntegerIndex(&index)) {
    if (index < kMaxInt) return static_cast<int>(index);
    return {};
  }

  Handle<Object> name_table =
      JSObject::GetProperty(isolate, handler, "names").ToHandleChecked();
  if (name_table->IsUndefined(isolate)) {
    name_table = GetOrCreateNameTable(GetInstance(isolate, handler), table_name,
                                      generate_names_callback);
    JSObject::AddProperty(isolate, handler, "names", name_table, DONT_ENUM);
  }
  DCHECK(name_table->IsJSMap());

  Handle<Object> object =
      GetMapValue(isolate, Handle<JSMap>::cast(name_table), property);
  if (object->IsUndefined()) return {};
  DCHECK(object->IsNumeric());
  return NumberToInt32(*object);
}

// Helper for unpacking a maybe name that makes a default with an index if
// the name is empty. If the name is not empty, it's prefixed with a $.
Handle<String> GetNameOrDefault(Isolate* isolate,
                                MaybeHandle<String> maybe_name,
                                const char* default_name_prefix, int index) {
  Handle<String> name;
  if (maybe_name.ToHandle(&name)) {
    return isolate->factory()
        ->NewConsString(isolate->factory()->NewStringFromAsciiChecked("$"),
                        name)
        .ToHandleChecked();
  }

  // Maximum length of the default names: $memory-2147483648\0
  static constexpr int kMaxStrLen = 19;
  EmbeddedVector<char, kMaxStrLen> value;
  DCHECK_LT(strlen(default_name_prefix) + /*strlen(kMinInt)*/ 11, kMaxStrLen);
  int len = SNPrintF(value, "%s%d", default_name_prefix, index);
  return isolate->factory()->InternalizeString(value.SubVector(0, len));
}

// Generate names for the locals. Names either come from the name table,
// otherwise the default $varX is used.
std::vector<Handle<String>> GetLocalNames(Handle<WasmInstanceObject> instance,
                                          Address pc) {
  wasm::NativeModule* native_module = instance->module_object().native_module();
  wasm::DebugInfo* debug_info = native_module->GetDebugInfo();
  int num_locals = debug_info->GetNumLocals(pc);
  auto* isolate = instance->GetIsolate();

  wasm::ModuleWireBytes module_wire_bytes(
      instance->module_object().native_module()->wire_bytes());
  const wasm::WasmFunction& function = debug_info->GetFunctionAtAddress(pc);

  std::vector<Handle<String>> names;
  for (int i = 0; i < num_locals; ++i) {
    wasm::WireBytesRef local_name_ref =
        debug_info->GetLocalName(function.func_index, i);
    DCHECK(module_wire_bytes.BoundsCheck(local_name_ref));
    Vector<const char> name_vec =
        module_wire_bytes.GetNameOrNull(local_name_ref);
    names.emplace_back(GetNameOrDefault(
        isolate,
        name_vec.empty() ? MaybeHandle<String>()
                         : isolate->factory()->NewStringFromUtf8(name_vec),
        "$var", i));
  }

  return names;
}

// Generate names for the globals. Names either come from the name table,
// otherwise the default $globalX is used.
Handle<JSMap> GetGlobalNames(Handle<WasmInstanceObject> instance) {
  Isolate* isolate = instance->GetIsolate();
  auto& globals = instance->module()->globals;
  Handle<JSMap> names = isolate->factory()->NewJSMap();
  for (uint32_t i = 0; i < globals.size(); ++i) {
    HandleScope scope(isolate);
    SetMapValue(isolate, names,
                GetNameOrDefault(isolate,
                                 WasmInstanceObject::GetGlobalNameOrNull(
                                     isolate, instance, i),
                                 "$global", i),
                isolate->factory()->NewNumberFromUint(i));
  }
  return names;
}

// Generate names for the functions.
Handle<JSMap> GetFunctionNames(Handle<WasmInstanceObject> instance) {
  Isolate* isolate = instance->GetIsolate();
  auto* module = instance->module();

  wasm::ModuleWireBytes wire_bytes(
      instance->module_object().native_module()->wire_bytes());

  Handle<JSMap> names = isolate->factory()->NewJSMap();
  for (auto& function : module->functions) {
    HandleScope scope(isolate);
    wasm::WireBytesRef name_ref =
        module->lazily_generated_names.LookupFunctionName(
            wire_bytes, function.func_index, VectorOf(module->export_table));
    DCHECK(wire_bytes.BoundsCheck(name_ref));
    Vector<const char> name_vec = wire_bytes.GetNameOrNull(name_ref);
    Handle<String> name = GetNameOrDefault(
        isolate,
        name_vec.empty() ? MaybeHandle<String>()
                         : isolate->factory()->NewStringFromUtf8(name_vec),
        "$func", function.func_index);
    SetMapValue(isolate, names, name,
                isolate->factory()->NewNumberFromUint(function.func_index));
  }

  return names;
}

// Generate names for the imports.
Handle<JSMap> GetImportNames(Handle<WasmInstanceObject> instance) {
  Isolate* isolate = instance->GetIsolate();
  const wasm::WasmModule* module = instance->module();
  Handle<WasmModuleObject> module_object(instance->module_object(), isolate);
  size_t num_imports = module->import_table.size();

  Handle<JSMap> names = isolate->factory()->NewJSMap();
  for (size_t index = 0; index < num_imports; ++index) {
    HandleScope scope(isolate);

    const wasm::WasmImport& import = module->import_table[index];
    SetMapValue(isolate, names,
                WasmModuleObject::ExtractUtf8StringFromModuleBytes(
                    isolate, module_object, import.field_name, kInternalize),
                isolate->factory()->NewNumberFromSize(index));
  }

  return names;
}

// Generate names for the memories.
Handle<JSMap> GetMemoryNames(Handle<WasmInstanceObject> instance) {
  Isolate* isolate = instance->GetIsolate();

  Handle<JSMap> names = isolate->factory()->NewJSMap();
  uint32_t memory_count = instance->has_memory_object() ? 1 : 0;
  for (uint32_t memory_index = 0; memory_index < memory_count; ++memory_index) {
    SetMapValue(isolate, names,
                GetNameOrDefault(isolate,
                                 WasmInstanceObject::GetMemoryNameOrNull(
                                     isolate, instance, memory_index),
                                 "$memory", memory_index),
                isolate->factory()->NewNumberFromUint(memory_index));
  }

  return names;
}

// Generate names for the tables.
Handle<JSMap> GetTableNames(Handle<WasmInstanceObject> instance) {
  Isolate* isolate = instance->GetIsolate();
  auto tables = handle(instance->tables(), isolate);

  Handle<JSMap> names = isolate->factory()->NewJSMap();
  for (int table_index = 0; table_index < tables->length(); ++table_index) {
    auto func_table =
        handle(WasmTableObject::cast(tables->get(table_index)), isolate);
    if (!func_table->type().is_reference_to(wasm::HeapType::kFunc)) continue;

    SetMapValue(isolate, names,
                GetNameOrDefault(isolate,
                                 WasmInstanceObject::GetTableNameOrNull(
                                     isolate, instance, table_index),
                                 "$table", table_index),
                isolate->factory()->NewNumberFromInt(table_index));
  }
  return names;
}

// Generate names for the exports
Handle<JSMap> GetExportNames(Handle<WasmInstanceObject> instance) {
  Isolate* isolate = instance->GetIsolate();
  const wasm::WasmModule* module = instance->module();
  Handle<WasmModuleObject> module_object(instance->module_object(), isolate);
  size_t num_exports = module->export_table.size();

  Handle<JSMap> names = isolate->factory()->NewJSMap();
  for (size_t index = 0; index < num_exports; ++index) {
    const wasm::WasmExport& exp = module->export_table[index];
    SetMapValue(isolate, names,
                WasmModuleObject::ExtractUtf8StringFromModuleBytes(
                    isolate, module_object, exp.name, kInternalize),
                isolate->factory()->NewNumberFromSize(index));
  }
  return names;
}

Address GetPC(Isolate* isolate, Handle<JSObject> handler) {
  Handle<Object> pc =
      JSObject::GetProperty(isolate, handler, "pc").ToHandleChecked();
  DCHECK(pc->IsBigInt());
  return Handle<BigInt>::cast(pc)->AsUint64();
}

Address GetFP(Isolate* isolate, Handle<JSObject> handler) {
  Handle<Object> fp =
      JSObject::GetProperty(isolate, handler, "fp").ToHandleChecked();
  DCHECK(fp->IsBigInt());
  return Handle<BigInt>::cast(fp)->AsUint64();
}

Address GetCalleeFP(Isolate* isolate, Handle<JSObject> handler) {
  Handle<Object> callee_fp =
      JSObject::GetProperty(isolate, handler, "callee_fp").ToHandleChecked();
  DCHECK(callee_fp->IsBigInt());
  return Handle<BigInt>::cast(callee_fp)->AsUint64();
}

// Convert a WasmValue to an appropriate JS representation.
static Handle<Object> WasmValueToObject(Isolate* isolate,
                                        wasm::WasmValue value) {
  auto* factory = isolate->factory();
  switch (value.type().kind()) {
    case wasm::ValueType::kI32:
      return factory->NewNumberFromInt(value.to_i32());
    case wasm::ValueType::kI64:
      return BigInt::FromInt64(isolate, value.to_i64());
    case wasm::ValueType::kF32:
      return factory->NewNumber(value.to_f32());
    case wasm::ValueType::kF64:
      return factory->NewNumber(value.to_f64());
    case wasm::ValueType::kS128: {
      wasm::Simd128 s128 = value.to_s128();
      Handle<JSArrayBuffer> buffer;
      if (!isolate->factory()
               ->NewJSArrayBufferAndBackingStore(
                   kSimd128Size, InitializedFlag::kUninitialized)
               .ToHandle(&buffer)) {
        isolate->FatalProcessOutOfHeapMemory(
            "failed to allocate backing store");
      }

      base::Memcpy(buffer->allocation_base(), s128.bytes(),
                   buffer->byte_length());
      return isolate->factory()->NewJSTypedArray(kExternalUint8Array, buffer, 0,
                                                 buffer->byte_length());
    }
    case wasm::ValueType::kRef:
      return value.to_externref();
    default:
      break;
  }
  return factory->undefined_value();
}

base::Optional<int> HasLocalImpl(Isolate* isolate, Handle<Name> property,
                                 Handle<JSObject> handler,
                                 bool enable_index_lookup) {
  Handle<WasmInstanceObject> instance = GetInstance(isolate, handler);

  base::Optional<int> index =
      ResolveValueSelector(isolate, property, handler, enable_index_lookup);
  if (!index) return index;
  Address pc = GetPC(isolate, handler);

  wasm::DebugInfo* debug_info =
      instance->module_object().native_module()->GetDebugInfo();
  int num_locals = debug_info->GetNumLocals(pc);
  if (0 <= index && index < num_locals) return index;
  return {};
}

Handle<Object> GetLocalImpl(Isolate* isolate, Handle<Name> property,
                            Handle<JSObject> handler,
                            bool enable_index_lookup) {
  Factory* factory = isolate->factory();
  Handle<WasmInstanceObject> instance = GetInstance(isolate, handler);

  base::Optional<int> index =
      HasLocalImpl(isolate, property, handler, enable_index_lookup);
  if (!index) return factory->undefined_value();
  Address pc = GetPC(isolate, handler);
  Address fp = GetFP(isolate, handler);
  Address callee_fp = GetCalleeFP(isolate, handler);

  wasm::DebugInfo* debug_info =
      instance->module_object().native_module()->GetDebugInfo();
  wasm::WasmValue value = debug_info->GetLocalValue(*index, pc, fp, callee_fp);
  return WasmValueToObject(isolate, value);
}

base::Optional<int> HasGlobalImpl(Isolate* isolate, Handle<Name> property,
                                  Handle<JSObject> handler,
                                  bool enable_index_lookup) {
  Handle<WasmInstanceObject> instance = GetInstance(isolate, handler);
  base::Optional<int> index =
      ResolveValueSelector(isolate, property, handler, enable_index_lookup,
                           "globals", GetGlobalNames);
  if (!index) return index;

  const std::vector<wasm::WasmGlobal>& globals = instance->module()->globals;
  if (globals.size() <= kMaxInt && 0 <= *index &&
      *index < static_cast<int>(globals.size())) {
    return index;
  }
  return {};
}

Handle<Object> GetGlobalImpl(Isolate* isolate, Handle<Name> property,
                             Handle<JSObject> handler,
                             bool enable_index_lookup) {
  Handle<WasmInstanceObject> instance = GetInstance(isolate, handler);
  base::Optional<int> index =
      HasGlobalImpl(isolate, property, handler, enable_index_lookup);
  if (!index) return isolate->factory()->undefined_value();

  const std::vector<wasm::WasmGlobal>& globals = instance->module()->globals;
  return WasmValueToObject(
      isolate, WasmInstanceObject::GetGlobalValue(instance, globals[*index]));
}

base::Optional<int> HasMemoryImpl(Isolate* isolate, Handle<Name> property,
                                  Handle<JSObject> handler,
                                  bool enable_index_lookup) {
  Handle<WasmInstanceObject> instance = GetInstance(isolate, handler);
  base::Optional<int> index =
      ResolveValueSelector(isolate, property, handler, enable_index_lookup,
                           "memories", GetMemoryNames);
  if (index && *index == 0 && instance->has_memory_object()) return index;
  return {};
}

Handle<Object> GetMemoryImpl(Isolate* isolate, Handle<Name> property,
                             Handle<JSObject> handler,
                             bool enable_index_lookup) {
  Handle<WasmInstanceObject> instance = GetInstance(isolate, handler);
  base::Optional<int> index =
      HasMemoryImpl(isolate, property, handler, enable_index_lookup);
  if (index) return handle(instance->memory_object(), isolate);
  return isolate->factory()->undefined_value();
}

base::Optional<int> HasFunctionImpl(Isolate* isolate, Handle<Name> property,
                                    Handle<JSObject> handler,
                                    bool enable_index_lookup) {
  Handle<WasmInstanceObject> instance = GetInstance(isolate, handler);
  base::Optional<int> index =
      ResolveValueSelector(isolate, property, handler, enable_index_lookup,
                           "functions", GetFunctionNames);
  if (!index) return index;
  const std::vector<wasm::WasmFunction>& functions =
      instance->module()->functions;
  if (functions.size() <= kMaxInt && 0 <= *index &&
      *index < static_cast<int>(functions.size())) {
    return index;
  }
  return {};
}

Handle<Object> GetFunctionImpl(Isolate* isolate, Handle<Name> property,
                               Handle<JSObject> handler,
                               bool enable_index_lookup) {
  Handle<WasmInstanceObject> instance = GetInstance(isolate, handler);
  base::Optional<int> index =
      HasFunctionImpl(isolate, property, handler, enable_index_lookup);
  if (!index) return isolate->factory()->undefined_value();

  return WasmInstanceObject::GetOrCreateWasmExternalFunction(isolate, instance,
                                                             *index);
}

base::Optional<int> HasTableImpl(Isolate* isolate, Handle<Name> property,
                                 Handle<JSObject> handler,
                                 bool enable_index_lookup) {
  Handle<WasmInstanceObject> instance = GetInstance(isolate, handler);
  base::Optional<int> index = ResolveValueSelector(
      isolate, property, handler, enable_index_lookup, "tables", GetTableNames);
  if (!index) return index;
  Handle<FixedArray> tables(instance->tables(), isolate);
  int num_tables = tables->length();
  if (*index < 0 || *index >= num_tables) return {};

  Handle<WasmTableObject> func_table(WasmTableObject::cast(tables->get(*index)),
                                     isolate);
  if (func_table->type().is_reference_to(wasm::HeapType::kFunc)) return index;
  return {};
}

Handle<Object> GetTableImpl(Isolate* isolate, Handle<Name> property,
                            Handle<JSObject> handler,
                            bool enable_index_lookup) {
  Handle<WasmInstanceObject> instance = GetInstance(isolate, handler);
  base::Optional<int> index =
      HasTableImpl(isolate, property, handler, enable_index_lookup);
  if (!index) return isolate->factory()->undefined_value();

  Handle<WasmTableObject> func_table(
      WasmTableObject::cast(instance->tables().get(*index)), isolate);
  return func_table;
}

base::Optional<int> HasImportImpl(Isolate* isolate, Handle<Name> property,
                                  Handle<JSObject> handler,
                                  bool enable_index_lookup) {
  Handle<WasmInstanceObject> instance = GetInstance(isolate, handler);
  base::Optional<int> index =
      ResolveValueSelector(isolate, property, handler, enable_index_lookup,
                           "imports", GetImportNames);
  if (!index) return index;
  const wasm::WasmModule* module = instance->module();
  Handle<WasmModuleObject> module_object(instance->module_object(), isolate);
  int num_imports = static_cast<int>(module->import_table.size());
  if (0 <= *index && *index < num_imports) return index;
  return {};
}

Handle<JSObject> GetExternalObject(Isolate* isolate,
                                   wasm::ImportExportKindCode kind,
                                   uint32_t index) {
  Handle<JSObject> result = isolate->factory()->NewJSObjectWithNullProto();
  Handle<Object> value = isolate->factory()->NewNumberFromUint(index);
  switch (kind) {
    case wasm::kExternalFunction:
      JSObject::AddProperty(isolate, result, "func", value, NONE);
      break;
    case wasm::kExternalGlobal:
      JSObject::AddProperty(isolate, result, "global", value, NONE);
      break;
    case wasm::kExternalTable:
      JSObject::AddProperty(isolate, result, "table", value, NONE);
      break;
    case wasm::kExternalMemory:
      JSObject::AddProperty(isolate, result, "mem", value, NONE);
      break;
    case wasm::kExternalException:
      JSObject::AddProperty(isolate, result, "exn", value, NONE);
      break;
  }
  return result;
}

Handle<Object> GetImportImpl(Isolate* isolate, Handle<Name> property,
                             Handle<JSObject> handler,
                             bool enable_index_lookup) {
  Handle<WasmInstanceObject> instance = GetInstance(isolate, handler);
  base::Optional<int> index =
      HasImportImpl(isolate, property, handler, enable_index_lookup);
  if (!index) return isolate->factory()->undefined_value();

  const wasm::WasmImport& imp = instance->module()->import_table[*index];
  return GetExternalObject(isolate, imp.kind, imp.index);
}

base::Optional<int> HasExportImpl(Isolate* isolate, Handle<Name> property,
                                  Handle<JSObject> handler,
                                  bool enable_index_lookup) {
  Handle<WasmInstanceObject> instance = GetInstance(isolate, handler);
  base::Optional<int> index =
      ResolveValueSelector(isolate, property, handler, enable_index_lookup,
                           "exports", GetExportNames);
  if (!index) return index;

  const wasm::WasmModule* module = instance->module();
  Handle<WasmModuleObject> module_object(instance->module_object(), isolate);
  int num_exports = static_cast<int>(module->export_table.size());
  if (0 <= *index && *index < num_exports) return index;
  return {};
}

Handle<Object> GetExportImpl(Isolate* isolate, Handle<Name> property,
                             Handle<JSObject> handler,
                             bool enable_index_lookup) {
  Handle<WasmInstanceObject> instance = GetInstance(isolate, handler);
  base::Optional<int> index =
      HasExportImpl(isolate, property, handler, enable_index_lookup);
  if (!index) return isolate->factory()->undefined_value();

  const wasm::WasmExport& exp = instance->module()->export_table[*index];
  return GetExternalObject(isolate, exp.kind, exp.index);
}

// Generic has trap callback for the index space proxies.
template <base::Optional<int> Impl(Isolate*, Handle<Name>, Handle<JSObject>,
                                   bool)>
void HasTrapCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  DCHECK_GE(args.Length(), 2);
  Isolate* isolate = reinterpret_cast<Isolate*>(args.GetIsolate());
  DCHECK(args.This()->IsObject());
  Handle<JSObject> handler =
      Handle<JSObject>::cast(Utils::OpenHandle(*args.This()));

  DCHECK(args[1]->IsName());
  Handle<Name> property = Handle<Name>::cast(Utils::OpenHandle(*args[1]));
  args.GetReturnValue().Set(Impl(isolate, property, handler, true).has_value());
}

// Generic get trap callback for the index space proxies.
template <Handle<Object> Impl(Isolate*, Handle<Name>, Handle<JSObject>, bool)>
void GetTrapCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  DCHECK_GE(args.Length(), 2);
  Isolate* isolate = reinterpret_cast<Isolate*>(args.GetIsolate());
  DCHECK(args.This()->IsObject());
  Handle<JSObject> handler =
      Handle<JSObject>::cast(Utils::OpenHandle(*args.This()));

  DCHECK(args[1]->IsName());
  Handle<Name> property = Handle<Name>::cast(Utils::OpenHandle(*args[1]));
  args.GetReturnValue().Set(
      Utils::ToLocal(Impl(isolate, property, handler, true)));
}

template <typename ReturnT>
ReturnT DelegateToplevelCall(Isolate* isolate, Handle<JSObject> target,
                             Handle<Name> property, const char* index_space,
                             ReturnT (*impl)(Isolate*, Handle<Name>,
                                             Handle<JSObject>, bool)) {
  Handle<Object> namespace_proxy =
      JSObject::GetProperty(isolate, target, index_space).ToHandleChecked();
  DCHECK(namespace_proxy->IsJSProxy());
  Handle<JSObject> namespace_handler(
      JSObject::cast(Handle<JSProxy>::cast(namespace_proxy)->handler()),
      isolate);
  return impl(isolate, property, namespace_handler, false);
}

template <typename ReturnT>
using DelegateCallback = ReturnT (*)(Isolate*, Handle<Name>, Handle<JSObject>,
                                     bool);

// Has trap callback for the top-level proxy.
void ToplevelHasTrapCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  DCHECK_GE(args.Length(), 2);
  Isolate* isolate = reinterpret_cast<Isolate*>(args.GetIsolate());
  DCHECK(args[0]->IsObject());
  Handle<JSObject> target = Handle<JSObject>::cast(Utils::OpenHandle(*args[0]));

  DCHECK(args[1]->IsName());
  Handle<Name> property = Handle<Name>::cast(Utils::OpenHandle(*args[1]));

  // First check if the property exists on the target.
  if (JSObject::HasProperty(target, property).FromMaybe(false)) {
    args.GetReturnValue().Set(true);
    return;
  }

  // All the properties in the delegates below are starting with $.
  if (!property->IsString()) {
    args.GetReturnValue().Set(false);
    return;
  }
  Handle<String> property_string = Handle<String>::cast(property);
  if (property_string->length() < 2 || property_string->Get(0) != '$') {
    args.GetReturnValue().Set(false);
    return;
  }

  // Now check the index space proxies in order if they know the property.
  constexpr std::pair<const char*, DelegateCallback<base::Optional<int>>>
      kDelegates[] = {{"memories", HasMemoryImpl},
                      {"locals", HasLocalImpl},
                      {"tables", HasTableImpl},
                      {"functions", HasFunctionImpl},
                      {"globals", HasGlobalImpl}};
  for (auto& delegate : kDelegates) {
    if (DelegateToplevelCall(isolate, target, property, delegate.first,
                             delegate.second)) {
      args.GetReturnValue().Set(true);
      return;
    }
    args.GetReturnValue().Set(false);
  }
}

// Get trap callback for the top-level proxy.
void ToplevelGetTrapCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  DCHECK_GE(args.Length(), 2);
  Isolate* isolate = reinterpret_cast<Isolate*>(args.GetIsolate());
  DCHECK(args[0]->IsObject());
  Handle<JSObject> target = Handle<JSObject>::cast(Utils::OpenHandle(*args[0]));

  DCHECK(args[1]->IsName());
  Handle<Name> property = Handle<Name>::cast(Utils::OpenHandle(*args[1]));

  // First, check if the property is a proper property on the target. If so,
  // return its value.
  Handle<Object> value =
      JSObject::GetProperty(isolate, target, property).ToHandleChecked();
  if (!value->IsUndefined()) {
    args.GetReturnValue().Set(Utils::ToLocal(value));
    return;
  }

  // All the properties in the delegates below are starting with $.
  if (!property->IsString()) {
    return;
  }
  Handle<String> property_string = Handle<String>::cast(property);
  if (property_string->length() < 0 || property_string->Get(0) != '$') {
    return;
  }

  // Try the index space proxies in the correct disambiguation order.
  constexpr std::pair<const char*, DelegateCallback<Handle<Object>>>
      kDelegates[] = {{"memories", GetMemoryImpl},
                      {"locals", GetLocalImpl},
                      {"tables", GetTableImpl},
                      {"functions", GetFunctionImpl},
                      {"globals", GetGlobalImpl}};
  for (auto& delegate : kDelegates) {
    value = DelegateToplevelCall(isolate, target, property, delegate.first,
                                 delegate.second);
    if (!value->IsUndefined()) {
      args.GetReturnValue().Set(Utils::ToLocal(value));
      return;
    }
  }
}

// Produce a JSProxy with a given name table and get and has trap handlers.
Handle<JSProxy> GetJSProxy(
    WasmFrame* frame, MaybeHandle<JSMap> maybe_name_table,
    void (*get_callback)(const v8::FunctionCallbackInfo<v8::Value>&),
    void (*has_callback)(const v8::FunctionCallbackInfo<v8::Value>&)) {
  Isolate* isolate = frame->isolate();
  Factory* factory = isolate->factory();
  Handle<JSObject> target = factory->NewJSObjectWithNullProto();
  Handle<JSObject> handler = factory->NewJSObjectWithNullProto();

  // Besides the name table, the get and has traps need access to the instance
  // and frame information.
  Handle<JSMap> name_table;
  if (maybe_name_table.ToHandle(&name_table)) {
    JSObject::AddProperty(isolate, handler, "names", name_table, DONT_ENUM);
  }
  Handle<WasmInstanceObject> instance(frame->wasm_instance(), isolate);
  JSObject::AddProperty(isolate, handler, "instance", instance, DONT_ENUM);
  Handle<BigInt> pc = BigInt::FromInt64(isolate, frame->pc());
  JSObject::AddProperty(isolate, handler, "pc", pc, DONT_ENUM);
  Handle<BigInt> fp = BigInt::FromInt64(isolate, frame->fp());
  JSObject::AddProperty(isolate, handler, "fp", fp, DONT_ENUM);
  Handle<BigInt> callee_fp = BigInt::FromInt64(isolate, frame->callee_fp());
  JSObject::AddProperty(isolate, handler, "callee_fp", callee_fp, DONT_ENUM);

  InstallFunc(isolate, handler, "get", get_callback, 3, false, READ_ONLY,
              SideEffectType::kHasNoSideEffect);
  InstallFunc(isolate, handler, "has", has_callback, 2, false, READ_ONLY,
              SideEffectType::kHasNoSideEffect);

  return factory->NewJSProxy(target, handler);
}

Handle<JSObject> GetStackObject(WasmFrame* frame) {
  Isolate* isolate = frame->isolate();
  Handle<JSObject> object = isolate->factory()->NewJSObjectWithNullProto();
  wasm::DebugInfo* debug_info =
      frame->wasm_instance().module_object().native_module()->GetDebugInfo();
  int num_values = debug_info->GetStackDepth(frame->pc());
  for (int i = 0; i < num_values; ++i) {
    wasm::WasmValue value = debug_info->GetStackValue(
        i, frame->pc(), frame->fp(), frame->callee_fp());
    JSObject::AddDataElement(object, i, WasmValueToObject(isolate, value),
                             NONE);
  }
  return object;
}
}  // namespace

// This function generates the JS debug proxy for a given Wasm frame. The debug
// proxy is used when evaluating debug JS expressions on a wasm frame and let's
// the developer inspect the engine state from JS. The proxy provides the
// following interface:
//
// type WasmSimdValue = Uint8Array;
// type WasmValue = number | bigint | object | WasmSimdValue;
// type WasmFunction = (... args : WasmValue[]) = > WasmValue;
// type WasmExport = {name : string} & ({func : number} | {table : number} |
//                                      {mem : number} | {global : number});
// type WasmImport = {name : string, module : string} &
//                   ({func : number} | {table : number} | {mem : number} |
//                    {global : number});
// interface WasmInterface {
//   $globalX: WasmValue;
//   $varX: WasmValue;
//   $funcX(a : WasmValue /*, ...*/) : WasmValue;
//   readonly $memoryX : WebAssembly.Memory;
//   readonly $tableX : WebAssembly.Table;
//   readonly memories : {[nameOrIndex:string | number] : WebAssembly.Memory};
//   readonly tables : {[nameOrIndex:string | number] : WebAssembly.Table};
//   readonly stack : WasmValue[];
//   readonly imports : {[nameOrIndex:string | number] : WasmImport};
//   readonly exports : {[nameOrIndex:string | number] : WasmExport};
//   readonly globals : {[nameOrIndex:string | number] : WasmValue};
//   readonly locals : {[nameOrIndex:string | number] : WasmValue};
//   readonly functions : {[nameOrIndex:string | number] : WasmFunction};
// }
//
// The wasm index spaces memories, tables, imports, exports, globals, locals
// functions are JSProxies that lazily produce values either by index or by
// name. A top level JSProxy is wrapped around those for top-level lookup of
// names in the disambiguation order  memory, local, table, function, global.
// Import and export names are not globally resolved.

Handle<JSProxy> WasmJs::GetJSDebugProxy(WasmFrame* frame) {
  Isolate* isolate = frame->isolate();
  Factory* factory = isolate->factory();
  Handle<WasmInstanceObject> instance(frame->wasm_instance(), isolate);

  // The top level proxy delegates lookups to the index space proxies.
  Handle<JSObject> handler = factory->NewJSObjectWithNullProto();
  InstallFunc(isolate, handler, "get", ToplevelGetTrapCallback, 3, false,
              READ_ONLY, SideEffectType::kHasNoSideEffect);
  InstallFunc(isolate, handler, "has", ToplevelHasTrapCallback, 2, false,
              READ_ONLY, SideEffectType::kHasNoSideEffect);

  Handle<JSObject> target = factory->NewJSObjectWithNullProto();

  // Generate JSMaps per index space for name->index lookup. Every index space
  // proxy is associated with its table for local name lookup.

  auto local_name_table =
      GetNameTable(isolate, GetLocalNames(instance, frame->pc()));
  auto locals =
      GetJSProxy(frame, local_name_table, GetTrapCallback<GetLocalImpl>,
                 HasTrapCallback<HasLocalImpl>);
  JSObject::AddProperty(isolate, target, "locals", locals, READ_ONLY);

  auto globals = GetJSProxy(frame, {}, GetTrapCallback<GetGlobalImpl>,
                            HasTrapCallback<HasGlobalImpl>);
  JSObject::AddProperty(isolate, target, "globals", globals, READ_ONLY);

  auto functions = GetJSProxy(frame, {}, GetTrapCallback<GetFunctionImpl>,
                              HasTrapCallback<HasFunctionImpl>);
  JSObject::AddProperty(isolate, target, "functions", functions, READ_ONLY);

  auto memories = GetJSProxy(frame, {}, GetTrapCallback<GetMemoryImpl>,
                             HasTrapCallback<HasMemoryImpl>);
  JSObject::AddProperty(isolate, target, "memories", memories, READ_ONLY);

  auto tables = GetJSProxy(frame, {}, GetTrapCallback<GetTableImpl>,
                           HasTrapCallback<HasTableImpl>);
  JSObject::AddProperty(isolate, target, "tables", tables, READ_ONLY);

  auto imports = GetJSProxy(frame, {}, GetTrapCallback<GetImportImpl>,
                            HasTrapCallback<HasImportImpl>);
  JSObject::AddProperty(isolate, target, "imports", imports, READ_ONLY);

  auto exports = GetJSProxy(frame, {}, GetTrapCallback<GetExportImpl>,
                            HasTrapCallback<HasExportImpl>);
  JSObject::AddProperty(isolate, target, "exports", exports, READ_ONLY);

  auto stack = GetStackObject(frame);
  JSObject::AddProperty(isolate, target, "stack", stack, READ_ONLY);

  return factory->NewJSProxy(target, handler);
}

#undef ASSIGN
#undef EXTRACT_THIS

}  // namespace internal
}  // namespace v8

// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/wasm/module-compiler.h"

#include <atomic>

#include "src/api.h"
#include "src/asmjs/asm-js.h"
#include "src/assembler-inl.h"
#include "src/base/optional.h"
#include "src/base/template-utils.h"
#include "src/base/utils/random-number-generator.h"
#include "src/code-stubs.h"
#include "src/compiler/wasm-compiler.h"
#include "src/counters.h"
#include "src/property-descriptor.h"
#include "src/trap-handler/trap-handler.h"
#include "src/wasm/compilation-manager.h"
#include "src/wasm/module-decoder.h"
#include "src/wasm/wasm-code-manager.h"
#include "src/wasm/wasm-code-specialization.h"
#include "src/wasm/wasm-engine.h"
#include "src/wasm/wasm-js.h"
#include "src/wasm/wasm-memory.h"
#include "src/wasm/wasm-objects-inl.h"
#include "src/wasm/wasm-result.h"

#define TRACE(...)                                      \
  do {                                                  \
    if (FLAG_trace_wasm_instances) PrintF(__VA_ARGS__); \
  } while (false)

#define TRACE_CHAIN(instance)        \
  do {                               \
    instance->PrintInstancesChain(); \
  } while (false)

#define TRACE_COMPILE(...)                             \
  do {                                                 \
    if (FLAG_trace_wasm_compiler) PrintF(__VA_ARGS__); \
  } while (false)

#define TRACE_STREAMING(...)                            \
  do {                                                  \
    if (FLAG_trace_wasm_streaming) PrintF(__VA_ARGS__); \
  } while (false)

#define TRACE_LAZY(...)                                        \
  do {                                                         \
    if (FLAG_trace_wasm_lazy_compilation) PrintF(__VA_ARGS__); \
  } while (false)

namespace v8 {
namespace internal {
namespace wasm {

static constexpr int kInvalidSigIndex = -1;

enum class CompilationEvent : uint8_t {
  kFinishedBaselineCompilation,
  kFailedCompilation
};

enum class NotifyCompilationCallback : uint8_t { kNotify, kNoNotify };

// The CompilationState keeps track of the compilation state of the
// owning NativeModule, i.e. which functions are left to be compiled.
// It contains a task manager to allow parallel and asynchronous background
// compilation of functions.
class CompilationState {
 public:
  class CodeGenerationSchedule {
   public:
    explicit CodeGenerationSchedule(
        base::RandomNumberGenerator* random_number_generator,
        size_t max_memory = 0);

    void Schedule(std::unique_ptr<compiler::WasmCompilationUnit>&& item);

    bool IsEmpty() const { return schedule_.empty(); }

    std::unique_ptr<compiler::WasmCompilationUnit> GetNext();

    bool CanAcceptWork() const;

    bool ShouldIncreaseWorkload() const;

    void EnableThrottling() { throttle_ = true; }

   private:
    size_t GetRandomIndexInSchedule();

    base::RandomNumberGenerator* random_number_generator_ = nullptr;
    std::vector<std::unique_ptr<compiler::WasmCompilationUnit>> schedule_;
    const size_t max_memory_;
    bool throttle_ = false;
    base::AtomicNumber<size_t> allocated_memory_{0};
  };

  explicit CompilationState(internal::Isolate* isolate);
  ~CompilationState();

  // Needs to be set before {AddCompilationUnits} is run, which triggers
  // {StartCompilation}.
  void SetNumberOfFunctionsToCompile(size_t num_functions);
  void AddCallback(
      std::function<void(CompilationEvent, Handle<Object>)> callback);

  // Inserts new functions to compile and kicks off compilation.
  void AddCompilationUnits(
      std::vector<std::unique_ptr<compiler::WasmCompilationUnit>>& units);
  std::unique_ptr<compiler::WasmCompilationUnit> GetNextCompilationUnit();
  std::unique_ptr<compiler::WasmCompilationUnit> GetNextExecutedUnit();
  bool HasCompilationUnitToFinish();

  void OnError(Handle<Object> error, NotifyCompilationCallback notify);
  void OnFinishedUnit(NotifyCompilationCallback notify);
  void ScheduleUnitForFinishing(
      std::unique_ptr<compiler::WasmCompilationUnit>& unit);

  void CancelAndWait();
  void OnBackgroundTaskStopped();
  void RestartBackgroundTasks();
  // Only one foreground thread (finisher) is allowed to run at a time.
  // {SetFinisherIsRunning} returns whether the flag changed its state.
  bool SetFinisherIsRunning(bool value);
  void ScheduleFinisherTask();

  bool CanAcceptWork() const { return executed_units_.CanAcceptWork(); }
  void EnableThrottling() { executed_units_.EnableThrottling(); }
  bool ShouldIncreaseWorkload() {
    if (executed_units_.ShouldIncreaseWorkload()) {
      // Check if it actually makes sense to increase the workload.
      base::LockGuard<base::Mutex> guard(&compilation_units_mutex_);
      return !compilation_units_.empty();
    }
    return false;
  }

  void Abort();

  Isolate* isolate() { return isolate_; }

  WasmCompiledModule* compiled_module() const {
    DCHECK_NOT_NULL(compiled_module_);
    return *compiled_module_;
  }

  void SetCompiledModule(Handle<WasmCompiledModule> compiled_module) {
    compiled_module_ =
        isolate_->global_handles()->Create(*compiled_module).location();
    GlobalHandles::MakeWeak(reinterpret_cast<Object***>(&compiled_module_));
  }

  bool failed() { return failed_; }

 private:
  void StartCompilation(size_t num_functions);

  void NotifyOnEvent(CompilationEvent event, Handle<Object> error);

  Isolate* isolate_;

  // A phantom reference to the {WasmCompiledModule}. It is intentionally not
  // typed {Handle<WasmCompiledModule>} because this location will be cleared
  // when the phantom reference is cleared.
  WasmCompiledModule** compiled_module_ = nullptr;

  std::vector<std::unique_ptr<compiler::WasmCompilationUnit>>
      compilation_units_;
  std::vector<std::function<void(CompilationEvent, Handle<Object>)>> callbacks_;
  base::Mutex compilation_units_mutex_;

  CodeGenerationSchedule executed_units_;
  // Should only be set when result_mutex_ is taken.
  bool finisher_is_running_ = false;
  base::Mutex result_mutex_;

  // When canceling the background_task_manager_, use {CancelAndWait} on
  // the CompilationState in order to cleanly clean up.
  CancelableTaskManager background_task_manager_;
  CancelableTaskManager foreground_task_manager_;
  std::shared_ptr<v8::TaskRunner> background_task_runner_;
  std::shared_ptr<v8::TaskRunner> foreground_task_runner_;

  size_t num_background_tasks_ = 0;
  size_t stopped_compilation_tasks_ = 0;
  base::Mutex tasks_mutex_;

  std::atomic<bool> failed_{false};

  size_t outstanding_units_ = 0;
};

namespace {

class JSToWasmWrapperCache {
 public:
  void SetContextAddress(Address context_address) {
    // Prevent to have different context addresses in the cache.
    DCHECK(code_cache_.empty());
    context_address_ = context_address;
  }

  Handle<Code> CloneOrCompileJSToWasmWrapper(Isolate* isolate,
                                             wasm::WasmModule* module,
                                             wasm::WasmCode* wasm_code,
                                             uint32_t index,
                                             bool use_trap_handler) {
    const wasm::WasmFunction* func = &module->functions[index];
    int cached_idx = sig_map_.Find(func->sig);
    if (cached_idx >= 0) {
      Handle<Code> code = isolate->factory()->CopyCode(code_cache_[cached_idx]);
      // Now patch the call to wasm code.
      RelocIterator it(*code, RelocInfo::ModeMask(RelocInfo::JS_TO_WASM_CALL));
      DCHECK(!it.done());
      it.rinfo()->set_js_to_wasm_address(
          wasm_code == nullptr ? nullptr : wasm_code->instructions().start());
      return code;
    }

    Handle<Code> code = compiler::CompileJSToWasmWrapper(
        isolate, module, wasm_code, index, context_address_, use_trap_handler);
    uint32_t new_cache_idx = sig_map_.FindOrInsert(func->sig);
    DCHECK_EQ(code_cache_.size(), new_cache_idx);
    USE(new_cache_idx);
    code_cache_.push_back(code);
    return code;
  }

 private:
  // sig_map_ maps signatures to an index in code_cache_.
  wasm::SignatureMap sig_map_;
  std::vector<Handle<Code>> code_cache_;
  Address context_address_ = nullptr;
};

// A helper class to simplify instantiating a module from a compiled module.
// It closes over the {Isolate}, the {ErrorThrower}, the {WasmCompiledModule},
// etc.
class InstanceBuilder {
 public:
  InstanceBuilder(Isolate* isolate, ErrorThrower* thrower,
                  Handle<WasmModuleObject> module_object,
                  MaybeHandle<JSReceiver> ffi,
                  MaybeHandle<JSArrayBuffer> memory);

  // Build an instance, in all of its glory.
  MaybeHandle<WasmInstanceObject> Build();
  // Run the start function, if any.
  bool ExecuteStartFunction();

 private:
  // Represents the initialized state of a table.
  struct TableInstance {
    Handle<WasmTableObject> table_object;  // WebAssembly.Table instance
    Handle<FixedArray> js_wrappers;        // JSFunctions exported
    Handle<FixedArray> function_table;     // internal array of <sig,code> pairs
  };

  // A pre-evaluated value to use in import binding.
  struct SanitizedImport {
    Handle<String> module_name;
    Handle<String> import_name;
    Handle<Object> value;
  };

  Isolate* isolate_;
  WasmModule* const module_;
  const std::shared_ptr<Counters> async_counters_;
  ErrorThrower* thrower_;
  Handle<WasmModuleObject> module_object_;
  MaybeHandle<JSReceiver> ffi_;
  MaybeHandle<JSArrayBuffer> memory_;
  Handle<JSArrayBuffer> globals_;
  Handle<WasmCompiledModule> compiled_module_;
  std::vector<TableInstance> table_instances_;
  std::vector<Handle<JSFunction>> js_wrappers_;
  Handle<WasmExportedFunction> start_function_;
  JSToWasmWrapperCache js_to_wasm_cache_;
  std::vector<SanitizedImport> sanitized_imports_;

  const std::shared_ptr<Counters>& async_counters() const {
    return async_counters_;
  }
  Counters* counters() const { return async_counters().get(); }

  bool use_trap_handler() const { return compiled_module_->use_trap_handler(); }

// Helper routines to print out errors with imports.
#define ERROR_THROWER_WITH_MESSAGE(TYPE)                                      \
  void Report##TYPE(const char* error, uint32_t index,                        \
                    Handle<String> module_name, Handle<String> import_name) { \
    thrower_->TYPE("Import #%d module=\"%s\" function=\"%s\" error: %s",      \
                   index, module_name->ToCString().get(),                     \
                   import_name->ToCString().get(), error);                    \
  }                                                                           \
                                                                              \
  MaybeHandle<Object> Report##TYPE(const char* error, uint32_t index,         \
                                   Handle<String> module_name) {              \
    thrower_->TYPE("Import #%d module=\"%s\" error: %s", index,               \
                   module_name->ToCString().get(), error);                    \
    return MaybeHandle<Object>();                                             \
  }

  ERROR_THROWER_WITH_MESSAGE(LinkError)
  ERROR_THROWER_WITH_MESSAGE(TypeError)

#undef ERROR_THROWER_WITH_MESSAGE

  // Look up an import value in the {ffi_} object.
  MaybeHandle<Object> LookupImport(uint32_t index, Handle<String> module_name,
                                   Handle<String> import_name);

  // Look up an import value in the {ffi_} object specifically for linking an
  // asm.js module. This only performs non-observable lookups, which allows
  // falling back to JavaScript proper (and hence re-executing all lookups) if
  // module instantiation fails.
  MaybeHandle<Object> LookupImportAsm(uint32_t index,
                                      Handle<String> import_name);

  uint32_t EvalUint32InitExpr(const WasmInitExpr& expr);

  // Load data segments into the memory.
  void LoadDataSegments(WasmContext* wasm_context);

  void WriteGlobalValue(WasmGlobal& global, Handle<Object> value);

  void SanitizeImports();

  Handle<FixedArray> SetupWasmToJSImportsTable(
      Handle<WasmInstanceObject> instance);

  // Process the imports, including functions, tables, globals, and memory, in
  // order, loading them from the {ffi_} object. Returns the number of imported
  // functions.
  int ProcessImports(Handle<WasmInstanceObject> instance);

  template <typename T>
  T* GetRawGlobalPtr(WasmGlobal& global);

  // Process initialization of globals.
  void InitGlobals();

  // Allocate memory for a module instance as a new JSArrayBuffer.
  Handle<JSArrayBuffer> AllocateMemory(uint32_t num_pages);

  bool NeedsWrappers() const;

  // Process the exports, creating wrappers for functions, tables, memories,
  // and globals.
  void ProcessExports(Handle<WasmInstanceObject> instance,
                      Handle<WasmCompiledModule> compiled_module);

  void InitializeTables(Handle<WasmInstanceObject> instance,
                        CodeSpecialization* code_specialization);

  void LoadTableSegments(Handle<WasmInstanceObject> instance);
};

// This is used in ProcessImports.
// When importing other modules' exports, we need to ask
// the exporter for a WasmToWasm wrapper. To do that, we need to
// switch that module to RW. To avoid flip-floping the same module
// RW <->RX, we create a scope for a set of NativeModules.
class SetOfNativeModuleModificationScopes final {
 public:
  void Add(NativeModule* module) {
    if (native_modules_.insert(module).second) {
      module->SetExecutable(false);
    }
  }

  ~SetOfNativeModuleModificationScopes() {
    for (NativeModule* module : native_modules_) {
      module->SetExecutable(true);
    }
  }

 private:
  std::unordered_set<NativeModule*> native_modules_;
};

void EnsureWasmContextTable(WasmContext* wasm_context, int table_size) {
  if (wasm_context->table) return;
  wasm_context->table_size = table_size;
  wasm_context->table = reinterpret_cast<IndirectFunctionTableEntry*>(
      calloc(table_size, sizeof(IndirectFunctionTableEntry)));
  for (int i = 0; i < table_size; i++) {
    wasm_context->table[i].sig_id = kInvalidSigIndex;
  }
}

}  // namespace

MaybeHandle<WasmInstanceObject> InstantiateToInstanceObject(
    Isolate* isolate, ErrorThrower* thrower,
    Handle<WasmModuleObject> module_object, MaybeHandle<JSReceiver> imports,
    MaybeHandle<JSArrayBuffer> memory) {
  InstanceBuilder builder(isolate, thrower, module_object, imports, memory);
  auto instance = builder.Build();
  if (!instance.is_null() && builder.ExecuteStartFunction()) {
    return instance;
  }
  return {};
}

Address CompileLazy(Isolate* isolate) {
  HistogramTimerScope lazy_time_scope(
      isolate->counters()->wasm_lazy_compilation_time());

  // Find the wasm frame which triggered the lazy compile, to get the wasm
  // instance.
  StackFrameIterator it(isolate);
  // First frame: C entry stub.
  DCHECK(!it.done());
  DCHECK_EQ(StackFrame::EXIT, it.frame()->type());
  it.Advance();
  // Second frame: WasmCompileLazy builtin.
  DCHECK(!it.done());
  Handle<WasmInstanceObject> instance;
  Maybe<uint32_t> func_index_to_compile = Nothing<uint32_t>();
  Handle<Object> exp_deopt_data_entry;
  const wasm::WasmCode* lazy_stub_or_copy =
      isolate->wasm_engine()->code_manager()->LookupCode(it.frame()->pc());
  DCHECK_EQ(wasm::WasmCode::kLazyStub, lazy_stub_or_copy->kind());
  if (!lazy_stub_or_copy->IsAnonymous()) {
    // Then it's an indirect call or via JS->wasm wrapper.
    instance = handle(lazy_stub_or_copy->native_module()
                          ->compiled_module()
                          ->owning_instance(),
                      isolate);
    func_index_to_compile = Just(lazy_stub_or_copy->index());
    exp_deopt_data_entry =
        handle(instance->compiled_module()->lazy_compile_data()->get(
                   static_cast<int>(lazy_stub_or_copy->index())),
               isolate);
  }
  it.Advance();
  // Third frame: The calling wasm code (direct or indirect), or js-to-wasm
  // wrapper.
  DCHECK(!it.done());
  DCHECK(it.frame()->is_js_to_wasm() || it.frame()->is_wasm_compiled());
  Handle<Code> js_to_wasm_caller_code;
  const WasmCode* wasm_caller_code = nullptr;
  Maybe<uint32_t> offset = Nothing<uint32_t>();
  if (it.frame()->is_js_to_wasm()) {
    DCHECK(!instance.is_null());
    js_to_wasm_caller_code = handle(it.frame()->LookupCode(), isolate);
  } else {
    wasm_caller_code =
        isolate->wasm_engine()->code_manager()->LookupCode(it.frame()->pc());
    offset = Just(static_cast<uint32_t>(
        it.frame()->pc() - wasm_caller_code->instructions().start()));
    if (instance.is_null()) {
      // Then this is a direct call (otherwise we would have attached the
      // instance via deopt data to the lazy compile stub). Just use the
      // instance of the caller.
      instance = handle(wasm_caller_code->native_module()
                            ->compiled_module()
                            ->owning_instance(),
                        isolate);
    }
  }

  Handle<WasmCompiledModule> compiled_module(instance->compiled_module());

  wasm::LazyCompilationOrchestrator* orchestrator =
      Managed<wasm::LazyCompilationOrchestrator>::cast(
          compiled_module->shared()->lazy_compilation_orchestrator())
          ->get();
  DCHECK(!orchestrator->IsFrozenForTesting());

  NativeModuleModificationScope native_module_modification_scope(
      compiled_module->GetNativeModule());

  const wasm::WasmCode* result = nullptr;
  // The caller may be js to wasm calling a function
  // also available for indirect calls.
  if (!js_to_wasm_caller_code.is_null()) {
    result = orchestrator->CompileFromJsToWasm(
        isolate, instance, js_to_wasm_caller_code,
        func_index_to_compile.ToChecked());
  } else {
    DCHECK_NOT_NULL(wasm_caller_code);
    if (func_index_to_compile.IsNothing() ||
        (!exp_deopt_data_entry.is_null() &&
         !exp_deopt_data_entry->IsFixedArray())) {
      result = orchestrator->CompileDirectCall(
          isolate, instance, func_index_to_compile, wasm_caller_code,
          offset.ToChecked());
    } else {
      result = orchestrator->CompileIndirectCall(
          isolate, instance, func_index_to_compile.ToChecked());
    }
  }
  DCHECK_NOT_NULL(result);

  int func_index = static_cast<int>(result->index());
  if (!exp_deopt_data_entry.is_null() && exp_deopt_data_entry->IsFixedArray()) {
    Handle<FixedArray> exp_deopt_data =
        Handle<FixedArray>::cast(exp_deopt_data_entry);

    TRACE_LAZY("Patching %d position(s) in function tables.\n",
               exp_deopt_data->length() / 2);

    // See EnsureExportedLazyDeoptData: exp_deopt_data[0...(len-1)] are pairs
    // of <export_table, index> followed by undefined values. Use this
    // information here to patch all export tables.
    Address target = result->instructions().start();
    for (int idx = 0, end = exp_deopt_data->length(); idx < end; idx += 2) {
      if (exp_deopt_data->get(idx)->IsUndefined(isolate)) break;
      DisallowHeapAllocation no_gc;
      int exp_index = Smi::ToInt(exp_deopt_data->get(idx + 1));

      // TODO(titzer): patching of function tables for lazy compilation
      // only works for a single instance.
      instance->wasm_context()->get()->table[exp_index].target = target;
    }
    // After processing, remove the list of exported entries, such that we don't
    // do the patching redundantly.
    compiled_module->lazy_compile_data()->set(
        func_index, isolate->heap()->undefined_value());
  }

  return result->instructions().start();
}

compiler::ModuleEnv CreateModuleEnvFromCompiledModule(
    Isolate* isolate, Handle<WasmCompiledModule> compiled_module) {
  DisallowHeapAllocation no_gc;
  WasmModule* module = compiled_module->shared()->module();
  compiler::ModuleEnv result(module, std::vector<Address>{},
                             compiled_module->use_trap_handler());
  return result;
}

const wasm::WasmCode* LazyCompilationOrchestrator::CompileFunction(
    Isolate* isolate, Handle<WasmInstanceObject> instance, int func_index) {
  base::ElapsedTimer compilation_timer;
  compilation_timer.Start();
  Handle<WasmCompiledModule> compiled_module(instance->compiled_module(),
                                             isolate);

  // TODO(wasm): Refactor this to only get the name if it is really needed for
  // tracing / debugging.
  std::string func_name;
  {
    WasmName name = Vector<const char>::cast(
        compiled_module->shared()->GetRawFunctionName(func_index));
    // Copy to std::string, because the underlying string object might move on
    // the heap.
    func_name.assign(name.start(), static_cast<size_t>(name.length()));
  }

  TRACE_LAZY("Compiling function %s, %d.\n", func_name.c_str(), func_index);

  wasm::WasmCode* existing_code = compiled_module->GetNativeModule()->GetCode(
      static_cast<uint32_t>(func_index));
  if (existing_code != nullptr &&
      existing_code->kind() == wasm::WasmCode::kFunction) {
    TRACE_LAZY("Function %d already compiled.\n", func_index);
    return existing_code;
  }

  compiler::ModuleEnv module_env =
      CreateModuleEnvFromCompiledModule(isolate, compiled_module);

  const uint8_t* module_start =
      compiled_module->shared()->module_bytes()->GetChars();

  const WasmFunction* func = &module_env.module->functions[func_index];
  FunctionBody body{func->sig, func->code.offset(),
                    module_start + func->code.offset(),
                    module_start + func->code.end_offset()};

  ErrorThrower thrower(isolate, "WasmLazyCompile");
  compiler::WasmCompilationUnit unit(isolate, &module_env,
                                     compiled_module->GetNativeModule(), body,
                                     CStrVector(func_name.c_str()), func_index,
                                     CEntryStub(isolate, 1).GetCode());
  unit.ExecuteCompilation();
  wasm::WasmCode* wasm_code = unit.FinishCompilation(&thrower);

  if (wasm::WasmCode::ShouldBeLogged(isolate)) wasm_code->LogCode(isolate);

  // If there is a pending error, something really went wrong. The module was
  // verified before starting execution with lazy compilation.
  // This might be OOM, but then we cannot continue execution anyway.
  // TODO(clemensh): According to the spec, we can actually skip validation at
  // module creation time, and return a function that always traps here.
  CHECK(!thrower.error());

  // Now specialize the generated code for this instance.
  Zone specialization_zone(isolate->allocator(), ZONE_NAME);
  CodeSpecialization code_specialization(isolate, &specialization_zone);
  code_specialization.RelocateDirectCalls(compiled_module->GetNativeModule());
  code_specialization.ApplyToWasmCode(wasm_code, SKIP_ICACHE_FLUSH);
  int64_t func_size =
      static_cast<int64_t>(func->code.end_offset() - func->code.offset());
  int64_t compilation_time = compilation_timer.Elapsed().InMicroseconds();

  auto counters = isolate->counters();
  counters->wasm_lazily_compiled_functions()->Increment();

  Assembler::FlushICache(wasm_code->instructions().start(),
                         wasm_code->instructions().size());
  counters->wasm_generated_code_size()->Increment(
      static_cast<int>(wasm_code->instructions().size()));
  counters->wasm_reloc_size()->Increment(
      static_cast<int>(wasm_code->reloc_info().size()));

  counters->wasm_lazy_compilation_throughput()->AddSample(
      compilation_time != 0 ? static_cast<int>(func_size / compilation_time)
                            : 0);
  return wasm_code;
}

namespace {

int AdvanceSourcePositionTableIterator(SourcePositionTableIterator& iterator,
                                       int offset) {
  DCHECK(!iterator.done());
  int byte_pos;
  do {
    byte_pos = iterator.source_position().ScriptOffset();
    iterator.Advance();
  } while (!iterator.done() && iterator.code_offset() <= offset);
  return byte_pos;
}

const WasmCode* WasmExtractWasmToWasmCallee(const WasmCodeManager* code_manager,
                                            const WasmCode* wasm_to_wasm) {
  DCHECK_EQ(WasmCode::kWasmToWasmWrapper, wasm_to_wasm->kind());
  // Find the one code target in this wrapper.
  RelocIterator it(wasm_to_wasm->instructions(), wasm_to_wasm->reloc_info(),
                   wasm_to_wasm->constant_pool(),
                   RelocInfo::ModeMask(RelocInfo::JS_TO_WASM_CALL));
  DCHECK(!it.done());
  const WasmCode* callee =
      code_manager->LookupCode(it.rinfo()->js_to_wasm_address());
#ifdef DEBUG
  it.next();
  DCHECK(it.done());
#endif
  return callee;
}

// TODO(mtrofin): this should be a function again, when chromium:761307
// is addressed. chromium:771171 is also related.
#define WasmPatchWasmToWasmWrapper(isolate, wasm_to_wasm, new_target)          \
  do {                                                                         \
    TRACE_LAZY("Patching wasm-to-wasm wrapper.\n");                            \
    DCHECK_EQ(WasmCode::kWasmToWasmWrapper, wasm_to_wasm->kind());             \
    NativeModuleModificationScope scope(wasm_to_wasm->native_module());        \
    RelocIterator it(wasm_to_wasm->instructions(), wasm_to_wasm->reloc_info(), \
                     wasm_to_wasm->constant_pool(),                            \
                     RelocInfo::ModeMask(RelocInfo::JS_TO_WASM_CALL));         \
    DCHECK(!it.done());                                                        \
    DCHECK_EQ(WasmCode::kLazyStub,                                             \
              isolate->wasm_engine()                                           \
                  ->code_manager()                                             \
                  ->GetCodeFromStartAddress(it.rinfo()->js_to_wasm_address())  \
                  ->kind());                                                   \
    it.rinfo()->set_js_to_wasm_address(new_target->instructions().start());    \
    it.next();                                                                 \
    DCHECK(it.done());                                                         \
  } while (0)

}  // namespace

const wasm::WasmCode* LazyCompilationOrchestrator::CompileFromJsToWasm(
    Isolate* isolate, Handle<WasmInstanceObject> instance,
    Handle<Code> js_to_wasm_caller, uint32_t exported_func_index) {
  Decoder decoder(nullptr, nullptr);
  Handle<WasmCompiledModule> compiled_module(instance->compiled_module(),
                                             isolate);

  TRACE_LAZY(
      "Starting lazy compilation (func %u, js_to_wasm: true, patch caller: "
      "true). \n",
      exported_func_index);
  CompileFunction(isolate, instance, exported_func_index);
  {
    DisallowHeapAllocation no_gc;
    int patched = 0;
    CodeSpaceMemoryModificationScope modification_scope(isolate->heap());
    RelocIterator it(*js_to_wasm_caller,
                     RelocInfo::ModeMask(RelocInfo::JS_TO_WASM_CALL));
    DCHECK(!it.done());
    wasm::WasmCode* current_callee =
        isolate->wasm_engine()->code_manager()->LookupCode(
            it.rinfo()->js_to_wasm_address());
    const wasm::WasmCode* callee_compiled =
        compiled_module->GetNativeModule()->GetCode(exported_func_index);
    DCHECK_NOT_NULL(callee_compiled);
    if (current_callee->kind() == WasmCode::kWasmToWasmWrapper) {
      WasmPatchWasmToWasmWrapper(isolate, current_callee, callee_compiled);
      ++patched;
    } else {
      DCHECK_EQ(WasmCode::kLazyStub,
                isolate->wasm_engine()
                    ->code_manager()
                    ->GetCodeFromStartAddress(it.rinfo()->js_to_wasm_address())
                    ->kind());
      it.rinfo()->set_js_to_wasm_address(
          callee_compiled->instructions().start());
      ++patched;
    }
    DCHECK_LT(0, patched);
    TRACE_LAZY("Patched %d location(s) in the caller.\n", patched);
    USE(patched);

#ifdef DEBUG
    it.next();
    DCHECK(it.done());
#endif
  }

  wasm::WasmCode* ret =
      compiled_module->GetNativeModule()->GetCode(exported_func_index);
  DCHECK_NOT_NULL(ret);
  DCHECK_EQ(wasm::WasmCode::kFunction, ret->kind());
  return ret;
}

const wasm::WasmCode* LazyCompilationOrchestrator::CompileIndirectCall(
    Isolate* isolate, Handle<WasmInstanceObject> instance,
    uint32_t func_index) {
  TRACE_LAZY(
      "Starting lazy compilation (func %u, js_to_wasm: false, patch caller: "
      "false). \n",
      func_index);
  return CompileFunction(isolate, instance, func_index);
}

const wasm::WasmCode* LazyCompilationOrchestrator::CompileDirectCall(
    Isolate* isolate, Handle<WasmInstanceObject> instance,
    Maybe<uint32_t> maybe_func_to_return_idx, const wasm::WasmCode* wasm_caller,
    int call_offset) {
  std::vector<Maybe<uint32_t>> non_compiled_functions;
  Decoder decoder(nullptr, nullptr);
  WasmCode* last_callee = nullptr;

  {
    DisallowHeapAllocation no_gc;
    Handle<WasmCompiledModule> caller_module(
        wasm_caller->native_module()->compiled_module(), isolate);
    SeqOneByteString* module_bytes = caller_module->shared()->module_bytes();
    uint32_t caller_func_index = wasm_caller->index();
    SourcePositionTableIterator source_pos_iterator(
        wasm_caller->source_positions());

    const byte* func_bytes =
        module_bytes->GetChars() + caller_module->shared()
                                       ->module()
                                       ->functions[caller_func_index]
                                       .code.offset();
    int num_non_compiled_functions = 0;
    for (RelocIterator it(wasm_caller->instructions(),
                          wasm_caller->reloc_info(),
                          wasm_caller->constant_pool(),
                          RelocInfo::ModeMask(RelocInfo::WASM_CALL));
         !it.done(); it.next()) {
      // TODO(clemensh): Introduce safe_cast<T, bool> which (D)CHECKS
      // (depending on the bool) against limits of T and then static_casts.
      size_t offset_l = it.rinfo()->pc() - wasm_caller->instructions().start();
      DCHECK_GE(kMaxInt, offset_l);
      int offset = static_cast<int>(offset_l);
      int byte_pos =
          AdvanceSourcePositionTableIterator(source_pos_iterator, offset);

      WasmCode* callee = isolate->wasm_engine()->code_manager()->LookupCode(
          it.rinfo()->target_address());
      if (offset < call_offset) last_callee = callee;
      if (callee->kind() != WasmCode::kLazyStub) {
        non_compiled_functions.push_back(Nothing<uint32_t>());
        continue;
      }
      ++num_non_compiled_functions;

      uint32_t called_func_index =
          ExtractDirectCallIndex(decoder, func_bytes + byte_pos);
      DCHECK_LT(called_func_index,
                caller_module->GetNativeModule()->FunctionCount());
      non_compiled_functions.push_back(Just(called_func_index));
      // Call offset one instruction after the call. Remember the last called
      // function before that offset.
      if (offset < call_offset) {
        maybe_func_to_return_idx = Just(called_func_index);
      }
    }

    TRACE_LAZY("Found %d non-compiled functions in caller.\n",
               num_non_compiled_functions);
    USE(num_non_compiled_functions);
  }
  uint32_t func_to_return_idx = 0;

  if (last_callee->kind() == WasmCode::kWasmToWasmWrapper) {
    const WasmCode* actual_callee = WasmExtractWasmToWasmCallee(
        isolate->wasm_engine()->code_manager(), last_callee);
    func_to_return_idx = actual_callee->index();
  } else {
    func_to_return_idx = maybe_func_to_return_idx.ToChecked();
  }

  TRACE_LAZY(
      "Starting lazy compilation (func %u @%d, js_to_wasm: false, patch "
      "caller: true). \n",
      func_to_return_idx, call_offset);

  // TODO(clemensh): compile all functions in non_compiled_functions in
  // background, wait for func_to_return_idx.
  const WasmCode* ret = CompileFunction(isolate, instance, func_to_return_idx);
  DCHECK_NOT_NULL(ret);

  int patched = 0;
  if (last_callee->kind() == WasmCode::kWasmToWasmWrapper) {
    // We can finish it all here by compiling the target wasm function and
    // patching the wasm_to_wasm caller.
    WasmPatchWasmToWasmWrapper(isolate, last_callee, ret);
    ++patched;
  } else {
    Handle<WasmCompiledModule> compiled_module(instance->compiled_module(),
                                               isolate);
    DisallowHeapAllocation no_gc;
    // Now patch the code object with all functions which are now compiled. This
    // will pick up any other compiled functions, not only {ret}.
    size_t idx = 0;
    for (RelocIterator
             it(wasm_caller->instructions(), wasm_caller->reloc_info(),
                wasm_caller->constant_pool(),
                RelocInfo::ModeMask(RelocInfo::WASM_CALL));
         !it.done(); it.next(), ++idx) {
      auto& info = non_compiled_functions[idx];
      if (info.IsNothing()) continue;
      uint32_t lookup = info.ToChecked();
      const WasmCode* callee_compiled =
          compiled_module->GetNativeModule()->GetCode(lookup);
      if (callee_compiled->kind() != WasmCode::kFunction) continue;
      DCHECK_EQ(WasmCode::kLazyStub,
                isolate->wasm_engine()
                    ->code_manager()
                    ->GetCodeFromStartAddress(it.rinfo()->wasm_call_address())
                    ->kind());
      it.rinfo()->set_wasm_call_address(
          callee_compiled->instructions().start());
      ++patched;
    }
    DCHECK_EQ(non_compiled_functions.size(), idx);
  }

  DCHECK_LT(0, patched);
  TRACE_LAZY("Patched %d location(s) in the caller.\n", patched);
  USE(patched);

  return ret;
}

namespace {
bool compile_lazy(const WasmModule* module) {
  return FLAG_wasm_lazy_compilation ||
         (FLAG_asm_wasm_lazy_compilation && module->is_asm_js());
}

void FlushICache(const wasm::NativeModule* native_module) {
  for (uint32_t i = 0, e = native_module->FunctionCount(); i < e; ++i) {
    const wasm::WasmCode* code = native_module->GetCode(i);
    if (code == nullptr) continue;
    Assembler::FlushICache(code->instructions().start(),
                           code->instructions().size());
  }
}

void FlushICache(Handle<FixedArray> functions) {
  for (int i = 0, e = functions->length(); i < e; ++i) {
    if (!functions->get(i)->IsCode()) continue;
    Code* code = Code::cast(functions->get(i));
    Assembler::FlushICache(code->instruction_start(), code->instruction_size());
  }
}

byte* raw_buffer_ptr(MaybeHandle<JSArrayBuffer> buffer, int offset) {
  return static_cast<byte*>(buffer.ToHandleChecked()->backing_store()) + offset;
}

void RecordStats(const Code* code, Counters* counters) {
  counters->wasm_generated_code_size()->Increment(code->body_size());
  counters->wasm_reloc_size()->Increment(code->relocation_info()->length());
}

void RecordStats(const wasm::WasmCode* code, Counters* counters) {
  counters->wasm_generated_code_size()->Increment(
      static_cast<int>(code->instructions().size()));
  counters->wasm_reloc_size()->Increment(
      static_cast<int>(code->reloc_info().size()));
}

void RecordStats(Handle<FixedArray> functions, Counters* counters) {
  DisallowHeapAllocation no_gc;
  for (int i = 0; i < functions->length(); ++i) {
    Object* val = functions->get(i);
    if (val->IsCode()) RecordStats(Code::cast(val), counters);
  }
}

void RecordStats(const wasm::NativeModule* native_module, Counters* counters) {
  for (uint32_t i = 0, e = native_module->FunctionCount(); i < e; ++i) {
    const wasm::WasmCode* code = native_module->GetCode(i);
    if (code != nullptr) RecordStats(code, counters);
  }
}

// Ensure that the code object in <code_table> at offset <func_index> has
// deoptimization data attached. This is needed for lazy compile stubs which are
// called from JS_TO_WASM functions or via exported function tables. The deopt
// data is used to determine which function this lazy compile stub belongs to.
wasm::WasmCode* EnsureExportedLazyDeoptData(Isolate* isolate,
                                            wasm::NativeModule* native_module,
                                            uint32_t func_index) {
  wasm::WasmCode* code = native_module->GetCode(func_index);
  // {code} will be nullptr when exporting imports.
  if (code == nullptr || code->kind() != wasm::WasmCode::kLazyStub ||
      !code->IsAnonymous()) {
    return code;
  }
  // Clone the lazy builtin into the native module.
  return native_module->CloneLazyBuiltinInto(code, func_index,
                                             WasmCode::kFlushICache);
}

// Ensure that the code object in <code_table> at offset <func_index> has
// deoptimization data attached. This is needed for lazy compile stubs which are
// called from JS_TO_WASM functions or via exported function tables. The deopt
// data is used to determine which function this lazy compile stub belongs to.
wasm::WasmCode* EnsureTableExportLazyDeoptData(
    Isolate* isolate, wasm::NativeModule* native_module, uint32_t func_index,
    Handle<FixedArray> export_table, int export_index,
    std::unordered_map<uint32_t, uint32_t>* num_table_exports) {
  wasm::WasmCode* code =
      EnsureExportedLazyDeoptData(isolate, native_module, func_index);
  if (code == nullptr || code->kind() != wasm::WasmCode::kLazyStub) return code;

  // deopt_data:
  // [#0: export table
  //  #1: export table index]
  // [#2: export table
  //  #3: export table index]
  // ...
  // num_table_exports counts down and determines the index for the new
  // export table entry.
  auto table_export_entry = num_table_exports->find(func_index);
  DCHECK(table_export_entry != num_table_exports->end());
  DCHECK_LT(0, table_export_entry->second);
  --table_export_entry->second;
  uint32_t this_idx = 2 * table_export_entry->second;
  int int_func_index = static_cast<int>(func_index);
  Object* deopt_entry =
      native_module->compiled_module()->lazy_compile_data()->get(
          int_func_index);
  FixedArray* deopt_data = nullptr;
  if (!deopt_entry->IsFixedArray()) {
    // we count indices down, so we enter here first for the
    // largest index.
    deopt_data = *isolate->factory()->NewFixedArray(this_idx + 2, TENURED);
    native_module->compiled_module()->lazy_compile_data()->set(int_func_index,
                                                               deopt_data);
  } else {
    deopt_data = FixedArray::cast(deopt_entry);
    DCHECK_LE(this_idx + 2, deopt_data->length());
  }
  DCHECK(deopt_data->get(this_idx)->IsUndefined(isolate));
  DCHECK(deopt_data->get(this_idx + 1)->IsUndefined(isolate));
  deopt_data->set(this_idx, *export_table);
  deopt_data->set(this_idx + 1, Smi::FromInt(export_index));
  return code;
}

bool in_bounds(uint32_t offset, uint32_t size, uint32_t upper) {
  return offset + size <= upper && offset + size >= offset;
}

using WasmInstanceMap =
    IdentityMap<Handle<WasmInstanceObject>, FreeStoreAllocationPolicy>;

wasm::WasmCode* MakeWasmToWasmWrapper(
    Isolate* isolate, Handle<WasmExportedFunction> imported_function,
    FunctionSig* expected_sig, FunctionSig** sig,
    WasmInstanceMap* imported_instances, Handle<WasmInstanceObject> instance,
    uint32_t index) {
  // TODO(wasm): cache WASM-to-WASM wrappers by signature and clone+patch.
  Handle<WasmInstanceObject> imported_instance(imported_function->instance(),
                                               isolate);
  imported_instances->Set(imported_instance, imported_instance);
  WasmContext* new_wasm_context = imported_instance->wasm_context()->get();
  Address new_wasm_context_address =
      reinterpret_cast<Address>(new_wasm_context);
  *sig = imported_instance->module()
             ->functions[imported_function->function_index()]
             .sig;
  if (expected_sig && !expected_sig->Equals(*sig)) return {};

  Handle<Code> code = compiler::CompileWasmToWasmWrapper(
      isolate, imported_function->GetWasmCode(), *sig,
      new_wasm_context_address);
  return instance->compiled_module()->GetNativeModule()->AddCodeCopy(
      code, wasm::WasmCode::kWasmToWasmWrapper, index);
}

wasm::WasmCode* UnwrapExportOrCompileImportWrapper(
    Isolate* isolate, FunctionSig* sig, Handle<JSReceiver> target,
    uint32_t import_index, ModuleOrigin origin,
    WasmInstanceMap* imported_instances, Handle<FixedArray> js_imports_table,
    Handle<WasmInstanceObject> instance) {
  if (WasmExportedFunction::IsWasmExportedFunction(*target)) {
    FunctionSig* unused = nullptr;
    return MakeWasmToWasmWrapper(
        isolate, Handle<WasmExportedFunction>::cast(target), sig, &unused,
        imported_instances, instance, import_index);
  }
  // No wasm function or being debugged. Compile a new wrapper for the new
  // signature.
  Handle<Code> temp_code = compiler::CompileWasmToJSWrapper(
      isolate, target, sig, import_index, origin,
      instance->compiled_module()->use_trap_handler(), js_imports_table);
  return instance->compiled_module()->GetNativeModule()->AddCodeCopy(
      temp_code, wasm::WasmCode::kWasmToJsWrapper, import_index);
}

void FunctionTableFinalizer(const v8::WeakCallbackInfo<void>& data) {
  GlobalHandles::Destroy(reinterpret_cast<Object**>(
      reinterpret_cast<JSObject**>(data.GetParameter())));
}

std::unique_ptr<compiler::ModuleEnv> CreateDefaultModuleEnv(
    Isolate* isolate, WasmModule* module) {
  std::vector<GlobalHandleAddress> function_tables;

  for (size_t i = module->function_tables.size(); i > 0; --i) {
    Handle<Object> func_table =
        isolate->global_handles()->Create(isolate->heap()->undefined_value());
    GlobalHandles::MakeWeak(func_table.location(), func_table.location(),
                            &FunctionTableFinalizer,
                            v8::WeakCallbackType::kFinalizer);
    function_tables.push_back(func_table.address());
  }

  // TODO(kschimpf): Add module-specific policy handling here (see v8:7143)?
  bool use_trap_handler = trap_handler::IsTrapHandlerEnabled();
  return base::make_unique<compiler::ModuleEnv>(module, function_tables,
                                                use_trap_handler);
}

Handle<WasmCompiledModule> NewCompiledModule(Isolate* isolate,
                                             WasmModule* module,
                                             Handle<FixedArray> export_wrappers,
                                             compiler::ModuleEnv* env) {
  Handle<WasmCompiledModule> compiled_module =
      WasmCompiledModule::New(isolate, module, export_wrappers,
                              env->function_tables, env->use_trap_handler);
  return compiled_module;
}

}  // namespace

namespace {

size_t GetMaxUsableMemorySize(Isolate* isolate) {
  return isolate->heap()->memory_allocator()->code_range()->valid()
             ? isolate->heap()->memory_allocator()->code_range()->size()
             : isolate->heap()->code_space()->Capacity();
}

double MonotonicallyIncreasingTimeInMs() {
  return V8::GetCurrentPlatform()->MonotonicallyIncreasingTime() *
         base::Time::kMillisecondsPerSecond;
}

// The CompilationUnitBuilder builds compilation units and stores them in an
// internal buffer. The buffer is moved into the working queue of the
// CompilationState when {Commit} is called.
class CompilationUnitBuilder {
 public:
  explicit CompilationUnitBuilder(NativeModule* native_module,
                                  compiler::ModuleEnv* module_env,
                                  Handle<Code> centry_stub)
      : native_module_(native_module),
        compilation_state_(native_module->compilation_state()),
        module_env_(module_env),
        centry_stub_(centry_stub) {}

  void AddUnit(const WasmFunction* function, uint32_t buffer_offset,
               Vector<const uint8_t> bytes, WasmName name) {
    units_.emplace_back(new compiler::WasmCompilationUnit(
        compilation_state_->isolate(), module_env_, native_module_,
        wasm::FunctionBody{function->sig, buffer_offset, bytes.begin(),
                           bytes.end()},
        name, function->func_index, centry_stub_,
        compiler::WasmCompilationUnit::GetDefaultCompilationMode(),
        compilation_state_->isolate()->async_counters().get()));
  }

  bool Commit() {
    if (units_.empty()) return false;
    compilation_state_->AddCompilationUnits(units_);
    units_.clear();
    return true;
  }

  void Clear() { units_.clear(); }

 private:
  NativeModule* native_module_;
  CompilationState* compilation_state_;
  compiler::ModuleEnv* module_env_;
  Handle<Code> centry_stub_;
  std::vector<std::unique_ptr<compiler::WasmCompilationUnit>> units_;
};

// Run by each compilation task and by the main thread (i.e. in both
// foreground and background threads). The no_finisher_callback is called
// within the result_mutex_ lock when no finishing task is running, i.e. when
// the finisher_is_running_ flag is not set.
bool FetchAndExecuteCompilationUnit(CompilationState* compilation_state) {
  DisallowHeapAllocation no_allocation;
  DisallowHandleAllocation no_handles;
  DisallowHandleDereference no_deref;
  DisallowCodeDependencyChange no_dependency_change;

  std::unique_ptr<compiler::WasmCompilationUnit> unit =
      compilation_state->GetNextCompilationUnit();
  if (unit == nullptr) return false;

  unit->ExecuteCompilation();
  compilation_state->ScheduleUnitForFinishing(unit);

  return true;
}

size_t GetNumFunctionsToCompile(const std::vector<WasmFunction>& functions,
                                compiler::ModuleEnv* module_env) {
  // TODO(kimanh): Remove, FLAG_skip_compiling_wasm_funcs: previously used for
  // debugging, and now not necessarily working anymore.
  uint32_t start = module_env->module->num_imported_functions +
                   FLAG_skip_compiling_wasm_funcs;
  uint32_t num_funcs = static_cast<uint32_t>(functions.size());
  uint32_t funcs_to_compile = start > num_funcs ? 0 : num_funcs - start;
  return funcs_to_compile;
}

void InitializeCompilationUnits(const std::vector<WasmFunction>& functions,
                                const ModuleWireBytes& wire_bytes,
                                compiler::ModuleEnv* module_env,
                                Handle<Code> centry_stub,
                                NativeModule* native_module) {
  uint32_t start = module_env->module->num_imported_functions +
                   FLAG_skip_compiling_wasm_funcs;
  uint32_t num_funcs = static_cast<uint32_t>(functions.size());

  CompilationUnitBuilder builder(native_module, module_env, centry_stub);
  for (uint32_t i = start; i < num_funcs; ++i) {
    const WasmFunction* func = &functions[i];
    uint32_t buffer_offset = func->code.offset();
    Vector<const uint8_t> bytes(wire_bytes.start() + func->code.offset(),
                                func->code.end_offset() - func->code.offset());

    WasmName name = wire_bytes.GetName(func, module_env->module);
    DCHECK_NOT_NULL(native_module);
    builder.AddUnit(func, buffer_offset, bytes, name);
  }
  builder.Commit();
}

wasm::WasmCode* FinishCompilationUnit(CompilationState* compilation_state,
                                      ErrorThrower* thrower, int* func_index) {
  std::unique_ptr<compiler::WasmCompilationUnit> unit =
      compilation_state->GetNextExecutedUnit();
  if (unit == nullptr) return {};
  *func_index = unit->func_index();
  DCHECK_LE(0, *func_index);
  return unit->FinishCompilation(thrower);
}

void FinishCompilationUnits(CompilationState* compilation_state,
                            ErrorThrower* thrower) {
  while (true) {
    if (compilation_state->failed()) break;
    int func_index = -1;
    wasm::WasmCode* result =
        FinishCompilationUnit(compilation_state, thrower, &func_index);

    if (func_index < 0) break;

    // Update the compilation state.
    compilation_state->OnFinishedUnit(NotifyCompilationCallback::kNoNotify);
    DCHECK_IMPLIES(result == nullptr, thrower->error());
    if (result == nullptr) break;
  }
  if (compilation_state->ShouldIncreaseWorkload() &&
      !compilation_state->failed()) {
    compilation_state->RestartBackgroundTasks();
  }
}

void CompileInParallel(Isolate* isolate, NativeModule* native_module,
                       const ModuleWireBytes& wire_bytes,
                       compiler::ModuleEnv* module_env,
                       Handle<Code> centry_stub, ErrorThrower* thrower) {
  const WasmModule* module = module_env->module;
  // Data structures for the parallel compilation.

  //-----------------------------------------------------------------------
  // For parallel compilation:
  // 1) The main thread allocates a compilation unit for each wasm function
  //    and stores them in the vector {compilation_units} within the
  //    {compilation_state}. By adding units to the {compilation_state}, new
  //    {BackgroundCompileTasks} instances are spawned which run on
  //    the background threads.
  // 2.a) The background threads and the main thread pick one compilation
  //      unit at a time and execute the parallel phase of the compilation
  //      unit. After finishing the execution of the parallel phase, the
  //      result is enqueued in {executed_units}.
  // 2.b) If {executed_units} contains a compilation unit, the main thread
  //      dequeues it and finishes the compilation.
  // 3) After the parallel phase of all compilation units has started, the
  //    main thread waits for all {BackgroundCompileTasks} instances to finish.
  // 4) The main thread finishes the compilation.

  // Turn on the {CanonicalHandleScope} so that the background threads can
  // use the node cache.
  CanonicalHandleScope canonical(isolate);

  CompilationState* compilation_state = native_module->compilation_state();
  compilation_state->EnableThrottling();
  // Make sure that no foreground task is spawned for finishing
  // the compilation units. This foreground thread will be
  // responsible for finishing compilation.
  compilation_state->SetFinisherIsRunning(true);
  size_t functions_count =
      GetNumFunctionsToCompile(module->functions, module_env);
  compilation_state->SetNumberOfFunctionsToCompile(functions_count);

  // 1) The main thread allocates a compilation unit for each wasm function
  //    and stores them in the vector {compilation_units} within the
  //    {compilation_state}. By adding units to the {compilation_state}, new
  //    {BackgroundCompileTask} instances are spawned which run on
  //    background threads.
  InitializeCompilationUnits(module->functions, wire_bytes, module_env,
                             centry_stub, native_module);

  // 2.a) The background threads and the main thread pick one compilation
  //      unit at a time and execute the parallel phase of the compilation
  //      unit. After finishing the execution of the parallel phase, the
  //      result is enqueued in {executed_units}.
  //      The foreground task bypasses waiting on memory threshold, because
  //      its results will immediately be converted to code (below).
  while (FetchAndExecuteCompilationUnit(compilation_state)) {
    // 2.b) If {executed_units} contains a compilation unit, the main thread
    //      dequeues it and finishes the compilation unit. Compilation units
    //      are finished concurrently to the background threads to save
    //      memory.
    FinishCompilationUnits(compilation_state, thrower);

    if (compilation_state->failed()) break;
  }

  // 3) After the parallel phase of all compilation units has started, the
  //    main thread waits for all {BackgroundCompileTasks} instances to finish -
  //    which happens once they all realize there's no next work item to
  //    process. If compilation already failed, all background tasks have
  //    already been canceled in {FinishCompilationUnits}, and there are
  //    no units to finish.
  if (!compilation_state->failed()) {
    compilation_state->CancelAndWait();

    // 4) Finish all compilation units which have been executed while we waited.
    FinishCompilationUnits(compilation_state, thrower);
  }
}

void CompileSequentially(Isolate* isolate, NativeModule* native_module,
                         const ModuleWireBytes& wire_bytes,
                         compiler::ModuleEnv* module_env,
                         ErrorThrower* thrower) {
  DCHECK(!thrower->error());

  const WasmModule* module = module_env->module;
  for (uint32_t i = FLAG_skip_compiling_wasm_funcs;
       i < module->functions.size(); ++i) {
    const WasmFunction& func = module->functions[i];
    if (func.imported) continue;  // Imports are compiled at instantiation time.

    // Compile the function.
    wasm::WasmCode* code = compiler::WasmCompilationUnit::CompileWasmFunction(
        native_module, thrower, isolate, wire_bytes, module_env, &func);
    if (code == nullptr) {
      TruncatedUserString<> name(wire_bytes.GetName(&func, module));
      thrower->CompileError("Compilation of #%d:%.*s failed.", i, name.length(),
                            name.start());
      break;
    }
  }
}

void ValidateSequentially(Isolate* isolate, const ModuleWireBytes& wire_bytes,
                          compiler::ModuleEnv* module_env,
                          ErrorThrower* thrower) {
  DCHECK(!thrower->error());

  const WasmModule* module = module_env->module;
  for (uint32_t i = 0; i < module->functions.size(); ++i) {
    const WasmFunction& func = module->functions[i];
    if (func.imported) continue;

    const byte* base = wire_bytes.start();
    FunctionBody body{func.sig, func.code.offset(), base + func.code.offset(),
                      base + func.code.end_offset()};
    DecodeResult result = VerifyWasmCodeWithStats(
        isolate->allocator(), module, body, module->is_wasm(),
        isolate->async_counters().get());
    if (result.failed()) {
      TruncatedUserString<> name(wire_bytes.GetName(&func, module));
      thrower->CompileError("Compiling function #%d:%.*s failed: %s @+%u", i,
                            name.length(), name.start(),
                            result.error_msg().c_str(), result.error_offset());
      break;
    }
  }
}

MaybeHandle<WasmModuleObject> CompileToModuleObjectInternal(
    Isolate* isolate, ErrorThrower* thrower, std::unique_ptr<WasmModule> module,
    const ModuleWireBytes& wire_bytes, Handle<Script> asm_js_script,
    Vector<const byte> asm_js_offset_table_bytes) {
  WasmModule* wasm_module = module.get();
  Handle<Code> centry_stub = CEntryStub(isolate, 1).GetCode();
  TimedHistogramScope wasm_compile_module_time_scope(
      wasm_module->is_wasm()
          ? isolate->async_counters()->wasm_compile_wasm_module_time()
          : isolate->async_counters()->wasm_compile_asm_module_time());
  // TODO(6792): No longer needed once WebAssembly code is off heap. Use
  // base::Optional to be able to close the scope before notifying the debugger.
  base::Optional<CodeSpaceMemoryModificationScope> modification_scope(
      base::in_place_t(), isolate->heap());

  // Check whether lazy compilation is enabled for this module.
  bool lazy_compile = compile_lazy(wasm_module);

  Factory* factory = isolate->factory();
  // Create heap objects for script, module bytes and asm.js offset table to
  // be stored in the shared module data.
  Handle<Script> script;
  Handle<ByteArray> asm_js_offset_table;
  if (asm_js_script.is_null()) {
    script = CreateWasmScript(isolate, wire_bytes);
  } else {
    script = asm_js_script;
    asm_js_offset_table =
        isolate->factory()->NewByteArray(asm_js_offset_table_bytes.length());
    asm_js_offset_table->copy_in(0, asm_js_offset_table_bytes.start(),
                                 asm_js_offset_table_bytes.length());
  }
  // TODO(wasm): only save the sections necessary to deserialize a
  // {WasmModule}. E.g. function bodies could be omitted.
  Handle<String> module_bytes =
      factory
          ->NewStringFromOneByte({wire_bytes.start(), wire_bytes.length()},
                                 TENURED)
          .ToHandleChecked();
  DCHECK(module_bytes->IsSeqOneByteString());

  // The {module_wrapper} will take ownership of the {WasmModule} object,
  // and it will be destroyed when the GC reclaims the wrapper object.
  Handle<WasmModuleWrapper> module_wrapper =
      WasmModuleWrapper::From(isolate, module.release());

  // Create the shared module data.
  // TODO(clemensh): For the same module (same bytes / same hash), we should
  // only have one WasmSharedModuleData. Otherwise, we might only set
  // breakpoints on a (potentially empty) subset of the instances.

  Handle<WasmSharedModuleData> shared = WasmSharedModuleData::New(
      isolate, module_wrapper, Handle<SeqOneByteString>::cast(module_bytes),
      script, asm_js_offset_table);
  if (lazy_compile) WasmSharedModuleData::PrepareForLazyCompilation(shared);

  Handle<Code> init_builtin = lazy_compile
                                  ? BUILTIN_CODE(isolate, WasmCompileLazy)
                                  : BUILTIN_CODE(isolate, Illegal);

  int export_wrappers_size =
      static_cast<int>(wasm_module->num_exported_functions);
  Handle<FixedArray> export_wrappers =
      factory->NewFixedArray(static_cast<int>(export_wrappers_size), TENURED);
  for (int i = 0, e = export_wrappers->length(); i < e; ++i) {
    export_wrappers->set(i, *init_builtin);
  }
  auto env = CreateDefaultModuleEnv(isolate, wasm_module);

  // Create the compiled module object and populate with compiled functions
  // and information needed at instantiation time. This object needs to be
  // serializable. Instantiation may occur off a deserialized version of this
  // object.
  Handle<WasmCompiledModule> compiled_module =
      NewCompiledModule(isolate, shared->module(), export_wrappers, env.get());
  NativeModule* native_module = compiled_module->GetNativeModule();
  compiled_module->set_shared(*shared);
  if (lazy_compile) {
    Handle<FixedArray> lazy_compile_data = isolate->factory()->NewFixedArray(
        static_cast<int>(wasm_module->functions.size()), TENURED);
    compiled_module->set_lazy_compile_data(*lazy_compile_data);
  }

  if (!lazy_compile) {
    size_t funcs_to_compile =
        wasm_module->functions.size() - wasm_module->num_imported_functions;
    bool compile_parallel =
        !FLAG_trace_wasm_decoder && FLAG_wasm_num_compilation_tasks > 0 &&
        funcs_to_compile > 1 &&
        V8::GetCurrentPlatform()->NumberOfWorkerThreads() > 0;

    if (compile_parallel) {
      CompileInParallel(isolate, native_module, wire_bytes, env.get(),
                        centry_stub, thrower);
    } else {
      CompileSequentially(isolate, native_module, wire_bytes, env.get(),
                          thrower);
    }
    if (thrower->error()) return {};

    RecordStats(native_module, isolate->async_counters().get());
  } else {
    if (wasm_module->is_wasm()) {
      // Validate wasm modules for lazy compilation. Don't validate asm.js
      // modules, they are valid by construction (otherwise a CHECK will fail
      // during lazy compilation).
      // TODO(clemensh): According to the spec, we can actually skip validation
      // at module creation time, and return a function that always traps at
      // (lazy) compilation time.
      ValidateSequentially(isolate, wire_bytes, env.get(), thrower);
    }

    native_module->SetLazyBuiltin(BUILTIN_CODE(isolate, WasmCompileLazy));
  }
  if (thrower->error()) return {};

  // Compile JS->wasm wrappers for exported functions.
  CompileJsToWasmWrappers(isolate, compiled_module,
                          isolate->async_counters().get());

  Handle<WasmModuleObject> result =
      WasmModuleObject::New(isolate, compiled_module);

  // If we created a wasm script, finish it now and make it public to the
  // debugger.
  if (asm_js_script.is_null()) {
    // Close the CodeSpaceMemoryModificationScope before calling into the
    // debugger.
    modification_scope.reset();
    script->set_wasm_compiled_module(*compiled_module);
    isolate->debug()->OnAfterCompile(script);
  }

  return result;
}

// The runnable task that finishes compilation in foreground (e.g. updating
// the NativeModule, the code table, etc.).
class FinishCompileTask : public CancelableTask {
 public:
  explicit FinishCompileTask(CompilationState* compilation_state,
                             CancelableTaskManager* task_manager)
      : CancelableTask(task_manager), compilation_state_(compilation_state) {}

  void RunInternal() override {
    Isolate* isolate = compilation_state_->isolate();
    HandleScope scope(isolate);
    SaveContext saved_context(isolate);
    isolate->set_context(
        compilation_state_->compiled_module()->native_context());

    TRACE_COMPILE("(4a) Finishing compilation units...\n");
    if (compilation_state_->failed()) {
      compilation_state_->SetFinisherIsRunning(false);
      return;
    }

    ErrorThrower thrower(compilation_state_->isolate(), "AsyncCompile");

    // We execute for 1 ms and then reschedule the task, same as the GC.
    double deadline = MonotonicallyIncreasingTimeInMs() + 1.0;
    while (true) {
      if (compilation_state_->ShouldIncreaseWorkload()) {
        compilation_state_->RestartBackgroundTasks();
      }

      int func_index = -1;
      wasm::WasmCode* result =
          FinishCompilationUnit(compilation_state_, &thrower, &func_index);

      if (thrower.error()) {
        DCHECK_NULL(result);
        USE(result);
        Handle<Object> error = thrower.Reify();
        compilation_state_->OnError(error, NotifyCompilationCallback::kNotify);
        compilation_state_->SetFinisherIsRunning(false);
        break;
      }

      if (func_index < 0) {
        // It might happen that a background task just scheduled a unit to be
        // finished, but did not start a finisher task since the flag was still
        // set. Check for this case, and continue if there is more work.
        compilation_state_->SetFinisherIsRunning(false);
        if (compilation_state_->HasCompilationUnitToFinish() &&
            compilation_state_->SetFinisherIsRunning(true)) {
          continue;
        }
        break;
      }

      // Update the compilation state, and possibly notify
      // threads waiting for events.
      compilation_state_->OnFinishedUnit(NotifyCompilationCallback::kNotify);

      if (deadline < MonotonicallyIncreasingTimeInMs()) {
        // We reached the deadline. We reschedule this task and return
        // immediately. Since we rescheduled this task already, we do not set
        // the FinisherIsRunning flag to false.
        compilation_state_->ScheduleFinisherTask();
        return;
      }
    }
  }

 private:
  CompilationState* compilation_state_;
};

// The runnable task that performs compilations in the background.
class BackgroundCompileTask : public CancelableTask {
 public:
  explicit BackgroundCompileTask(CompilationState* compilation_state,
                                 CancelableTaskManager* task_manager)
      : CancelableTask(task_manager), compilation_state_(compilation_state) {}

  void RunInternal() override {
    TRACE_COMPILE("(3b) Compiling...\n");
    while (compilation_state_->CanAcceptWork()) {
      if (compilation_state_->failed()) break;
      DisallowHandleAllocation no_handle;
      DisallowHeapAllocation no_allocation;

      if (!FetchAndExecuteCompilationUnit(compilation_state_)) {
        break;
      }
    }

    compilation_state_->OnBackgroundTaskStopped();
  }

 private:
  CompilationState* compilation_state_;
};
}  // namespace

MaybeHandle<WasmModuleObject> CompileToModuleObject(
    Isolate* isolate, ErrorThrower* thrower, std::unique_ptr<WasmModule> module,
    const ModuleWireBytes& wire_bytes, Handle<Script> asm_js_script,
    Vector<const byte> asm_js_offset_table_bytes) {
  return CompileToModuleObjectInternal(isolate, thrower, std::move(module),
                                       wire_bytes, asm_js_script,
                                       asm_js_offset_table_bytes);
}

InstanceBuilder::InstanceBuilder(Isolate* isolate, ErrorThrower* thrower,
                                 Handle<WasmModuleObject> module_object,
                                 MaybeHandle<JSReceiver> ffi,
                                 MaybeHandle<JSArrayBuffer> memory)
    : isolate_(isolate),
      module_(module_object->compiled_module()->shared()->module()),
      async_counters_(isolate->async_counters()),
      thrower_(thrower),
      module_object_(module_object),
      ffi_(ffi),
      memory_(memory) {
  sanitized_imports_.reserve(module_->import_table.size());
}

// Build an instance, in all of its glory.
MaybeHandle<WasmInstanceObject> InstanceBuilder::Build() {
  // Check that an imports argument was provided, if the module requires it.
  // No point in continuing otherwise.
  if (!module_->import_table.empty() && ffi_.is_null()) {
    thrower_->TypeError(
        "Imports argument must be present and must be an object");
    return {};
  }

  SanitizeImports();
  if (thrower_->error()) return {};

  // TODO(6792): No longer needed once WebAssembly code is off heap.
  CodeSpaceMemoryModificationScope modification_scope(isolate_->heap());
  // From here on, we expect the build pipeline to run without exiting to JS.
  DisallowJavascriptExecution no_js(isolate_);
  // Record build time into correct bucket, then build instance.
  TimedHistogramScope wasm_instantiate_module_time_scope(
      module_->is_wasm() ? counters()->wasm_instantiate_wasm_module_time()
                         : counters()->wasm_instantiate_asm_module_time());
  Factory* factory = isolate_->factory();

  //--------------------------------------------------------------------------
  // Reuse the compiled module (if no owner), otherwise clone.
  //--------------------------------------------------------------------------
  Handle<FixedArray> wrapper_table;
  wasm::NativeModule* native_module = nullptr;
  // Root the old instance, if any, in case later allocation causes GC,
  // to prevent the finalizer running for the old instance.
  MaybeHandle<WasmInstanceObject> old_instance;

  TRACE("Starting new module instantiation\n");
  {
    Handle<WasmCompiledModule> original =
        handle(module_object_->compiled_module());
    if (original->has_weak_owning_instance()) {
      old_instance = handle(original->owning_instance());
      // Clone, but don't insert yet the clone in the instances chain.
      // We do that last. Since we are holding on to the old instance,
      // the owner + original state used for cloning and patching
      // won't be mutated by possible finalizer runs.
      TRACE("Cloning from %zu\n", original->GetNativeModule()->instance_id);
      compiled_module_ = WasmCompiledModule::Clone(isolate_, original);
      native_module = compiled_module_->GetNativeModule();
      wrapper_table = handle(compiled_module_->export_wrappers(), isolate_);
      for (int i = 0; i < wrapper_table->length(); ++i) {
        Handle<Code> orig_code(Code::cast(wrapper_table->get(i)), isolate_);
        DCHECK_EQ(orig_code->kind(), Code::JS_TO_WASM_FUNCTION);
        Handle<Code> code = factory->CopyCode(orig_code);
        wrapper_table->set(i, *code);
      }
      RecordStats(native_module, counters());
      RecordStats(wrapper_table, counters());
    } else {
      // No instance owned the original compiled module.
      compiled_module_ = original;
      wrapper_table = handle(compiled_module_->export_wrappers(), isolate_);
      native_module = compiled_module_->GetNativeModule();
      TRACE("Reusing existing instance %zu\n",
            compiled_module_->GetNativeModule()->instance_id);
    }
    Handle<WeakCell> weak_native_context =
        isolate_->factory()->NewWeakCell(isolate_->native_context());
    compiled_module_->set_weak_native_context(*weak_native_context);
  }
  base::Optional<wasm::NativeModuleModificationScope>
      native_module_modification_scope;
  if (native_module != nullptr) {
    native_module_modification_scope.emplace(native_module);
  }

  //--------------------------------------------------------------------------
  // Create the WebAssembly.Instance object.
  //--------------------------------------------------------------------------
  Zone instantiation_zone(isolate_->allocator(), ZONE_NAME);
  CodeSpecialization code_specialization(isolate_, &instantiation_zone);
  Handle<WasmInstanceObject> instance =
      WasmInstanceObject::New(isolate_, compiled_module_);

  //--------------------------------------------------------------------------
  // Set up the globals for the new instance.
  //--------------------------------------------------------------------------
  WasmContext* wasm_context = instance->wasm_context()->get();
  uint32_t globals_size = module_->globals_size;
  if (globals_size > 0) {
    constexpr bool enable_guard_regions = false;
    Handle<JSArrayBuffer> global_buffer =
        NewArrayBuffer(isolate_, globals_size, enable_guard_regions);
    globals_ = global_buffer;
    if (globals_.is_null()) {
      thrower_->RangeError("Out of memory: wasm globals");
      return {};
    }
    wasm_context->globals_start =
        reinterpret_cast<byte*>(global_buffer->backing_store());
    instance->set_globals_buffer(*global_buffer);
  }

  //--------------------------------------------------------------------------
  // Reserve the metadata for indirect function tables.
  //--------------------------------------------------------------------------
  int function_table_count = static_cast<int>(module_->function_tables.size());
  table_instances_.reserve(module_->function_tables.size());
  for (int index = 0; index < function_table_count; ++index) {
    table_instances_.emplace_back();
  }

  //--------------------------------------------------------------------------
  // Process the imports for the module.
  //--------------------------------------------------------------------------
  int num_imported_functions = ProcessImports(instance);
  if (num_imported_functions < 0) return {};

  //--------------------------------------------------------------------------
  // Process the initialization for the module's globals.
  //--------------------------------------------------------------------------
  InitGlobals();

  //--------------------------------------------------------------------------
  // Initialize the indirect tables.
  //--------------------------------------------------------------------------
  if (function_table_count > 0) {
    InitializeTables(instance, &code_specialization);
  }

  //--------------------------------------------------------------------------
  // Allocate the memory array buffer.
  //--------------------------------------------------------------------------
  uint32_t initial_pages = module_->initial_pages;
  (module_->is_wasm() ? counters()->wasm_wasm_min_mem_pages_count()
                      : counters()->wasm_asm_min_mem_pages_count())
      ->AddSample(initial_pages);

  if (!memory_.is_null()) {
    // Set externally passed ArrayBuffer non neuterable.
    Handle<JSArrayBuffer> memory = memory_.ToHandleChecked();
    memory->set_is_neuterable(false);

    DCHECK_IMPLIES(use_trap_handler(), module_->is_asm_js() ||
                                           memory->is_wasm_memory() ||
                                           memory->backing_store() == nullptr);
  } else if (initial_pages > 0 || use_trap_handler()) {
    // We need to unconditionally create a guard region if using trap handlers,
    // even when the size is zero to prevent null-derefence issues
    // (e.g. https://crbug.com/769637).
    // Allocate memory if the initial size is more than 0 pages.
    memory_ = AllocateMemory(initial_pages);
    if (memory_.is_null()) return {};  // failed to allocate memory
  }

  //--------------------------------------------------------------------------
  // Create the WebAssembly.Memory object.
  //--------------------------------------------------------------------------
  if (module_->has_memory) {
    if (!instance->has_memory_object()) {
      // No memory object exists. Create one.
      Handle<WasmMemoryObject> memory_object = WasmMemoryObject::New(
          isolate_, memory_,
          module_->maximum_pages != 0 ? module_->maximum_pages : -1);
      instance->set_memory_object(*memory_object);
    }

    // Add the instance object to the list of instances for this memory.
    Handle<WasmMemoryObject> memory_object(instance->memory_object(), isolate_);
    WasmMemoryObject::AddInstance(isolate_, memory_object, instance);

    if (!memory_.is_null()) {
      // Double-check the {memory} array buffer matches the context.
      Handle<JSArrayBuffer> memory = memory_.ToHandleChecked();
      uint32_t mem_size = 0;
      CHECK(memory->byte_length()->ToUint32(&mem_size));
      CHECK_EQ(wasm_context->mem_size, mem_size);
      CHECK_EQ(wasm_context->mem_start, memory->backing_store());
    }
  }

  //--------------------------------------------------------------------------
  // Check that indirect function table segments are within bounds.
  //--------------------------------------------------------------------------
  for (WasmTableInit& table_init : module_->table_inits) {
    DCHECK(table_init.table_index < table_instances_.size());
    uint32_t base = EvalUint32InitExpr(table_init.offset);
    uint32_t table_size =
        table_instances_[table_init.table_index].function_table->length() /
        compiler::kFunctionTableEntrySize;
    if (!in_bounds(base, static_cast<uint32_t>(table_init.entries.size()),
                   table_size)) {
      thrower_->LinkError("table initializer is out of bounds");
      return {};
    }
  }

  //--------------------------------------------------------------------------
  // Check that memory segments are within bounds.
  //--------------------------------------------------------------------------
  for (WasmDataSegment& seg : module_->data_segments) {
    uint32_t base = EvalUint32InitExpr(seg.dest_addr);
    if (!in_bounds(base, seg.source.length(), wasm_context->mem_size)) {
      thrower_->LinkError("data segment is out of bounds");
      return {};
    }
  }

  // Set the WasmContext address in wrappers.
  // TODO(wasm): the wasm context should only appear as a constant in wrappers;
  //             this code specialization is applied to the whole instance.
  Address wasm_context_address = reinterpret_cast<Address>(wasm_context);
  code_specialization.RelocateWasmContextReferences(wasm_context_address);
  js_to_wasm_cache_.SetContextAddress(wasm_context_address);

  //--------------------------------------------------------------------------
  // Set up the exports object for the new instance.
  //--------------------------------------------------------------------------
  ProcessExports(instance, compiled_module_);
  if (thrower_->error()) return {};

  //--------------------------------------------------------------------------
  // Initialize the indirect function tables.
  //--------------------------------------------------------------------------
  if (function_table_count > 0) {
    LoadTableSegments(instance);
  }

  //--------------------------------------------------------------------------
  // Initialize the memory by loading data segments.
  //--------------------------------------------------------------------------
  if (module_->data_segments.size() > 0) {
    LoadDataSegments(wasm_context);
  }

  // Patch all code with the relocations registered in code_specialization.
  code_specialization.RelocateDirectCalls(native_module);
  code_specialization.ApplyToWholeModule(native_module, SKIP_ICACHE_FLUSH);

  FlushICache(native_module);
  FlushICache(wrapper_table);

  //--------------------------------------------------------------------------
  // Unpack and notify signal handler of protected instructions.
  //--------------------------------------------------------------------------
  if (use_trap_handler()) {
    UnpackAndRegisterProtectedInstructions(isolate_, native_module);
  }

  //--------------------------------------------------------------------------
  // Insert the compiled module into the weak list of compiled modules.
  //--------------------------------------------------------------------------
  {
    Handle<WeakCell> link_to_owning_instance = factory->NewWeakCell(instance);
    if (!old_instance.is_null()) {
      // Publish the new instance to the instances chain.
      DisallowHeapAllocation no_gc;
      compiled_module_->InsertInChain(*module_object_);
    }
    module_object_->set_compiled_module(*compiled_module_);
    compiled_module_->set_weak_owning_instance(*link_to_owning_instance);
    WasmInstanceObject::InstallFinalizer(isolate_, instance);
  }

  //--------------------------------------------------------------------------
  // Debugging support.
  //--------------------------------------------------------------------------
  // Set all breakpoints that were set on the shared module.
  WasmSharedModuleData::SetBreakpointsOnNewInstance(
      handle(compiled_module_->shared(), isolate_), instance);

  if (FLAG_wasm_interpret_all && module_->is_wasm()) {
    Handle<WasmDebugInfo> debug_info =
        WasmInstanceObject::GetOrCreateDebugInfo(instance);
    std::vector<int> func_indexes;
    for (int func_index = num_imported_functions,
             num_wasm_functions = static_cast<int>(module_->functions.size());
         func_index < num_wasm_functions; ++func_index) {
      func_indexes.push_back(func_index);
    }
    WasmDebugInfo::RedirectToInterpreter(
        debug_info, Vector<int>(func_indexes.data(),
                                static_cast<int>(func_indexes.size())));
  }

  //--------------------------------------------------------------------------
  // Create a wrapper for the start function.
  //--------------------------------------------------------------------------
  if (module_->start_function_index >= 0) {
    int start_index = module_->start_function_index;
    wasm::WasmCode* start_code =
        EnsureExportedLazyDeoptData(isolate_, native_module, start_index);
    FunctionSig* sig = module_->functions[start_index].sig;
    Handle<Code> wrapper_code = js_to_wasm_cache_.CloneOrCompileJSToWasmWrapper(
        isolate_, module_, start_code, start_index,
        compiled_module_->use_trap_handler());
    start_function_ = WasmExportedFunction::New(
        isolate_, instance, MaybeHandle<String>(), start_index,
        static_cast<int>(sig->parameter_count()), wrapper_code);
    RecordStats(start_code, counters());
  }

  DCHECK(!isolate_->has_pending_exception());
  TRACE("Successfully built instance %zu\n",
        compiled_module_->GetNativeModule()->instance_id);
  TRACE_CHAIN(module_object_->compiled_module());
  return instance;
}

bool InstanceBuilder::ExecuteStartFunction() {
  if (start_function_.is_null()) return true;  // No start function.

  HandleScope scope(isolate_);
  // Call the JS function.
  Handle<Object> undefined = isolate_->factory()->undefined_value();
  MaybeHandle<Object> retval =
      Execution::Call(isolate_, start_function_, undefined, 0, nullptr);

  if (retval.is_null()) {
    DCHECK(isolate_->has_pending_exception());
    return false;
  }
  return true;
}

// Look up an import value in the {ffi_} object.
MaybeHandle<Object> InstanceBuilder::LookupImport(uint32_t index,
                                                  Handle<String> module_name,

                                                  Handle<String> import_name) {
  // We pre-validated in the js-api layer that the ffi object is present, and
  // a JSObject, if the module has imports.
  DCHECK(!ffi_.is_null());

  // Look up the module first.
  MaybeHandle<Object> result =
      Object::GetPropertyOrElement(ffi_.ToHandleChecked(), module_name);
  if (result.is_null()) {
    return ReportTypeError("module not found", index, module_name);
  }

  Handle<Object> module = result.ToHandleChecked();

  // Look up the value in the module.
  if (!module->IsJSReceiver()) {
    return ReportTypeError("module is not an object or function", index,
                           module_name);
  }

  result = Object::GetPropertyOrElement(module, import_name);
  if (result.is_null()) {
    ReportLinkError("import not found", index, module_name, import_name);
    return MaybeHandle<JSFunction>();
  }

  return result;
}

// Look up an import value in the {ffi_} object specifically for linking an
// asm.js module. This only performs non-observable lookups, which allows
// falling back to JavaScript proper (and hence re-executing all lookups) if
// module instantiation fails.
MaybeHandle<Object> InstanceBuilder::LookupImportAsm(
    uint32_t index, Handle<String> import_name) {
  // Check that a foreign function interface object was provided.
  if (ffi_.is_null()) {
    return ReportLinkError("missing imports object", index, import_name);
  }

  // Perform lookup of the given {import_name} without causing any observable
  // side-effect. We only accept accesses that resolve to data properties,
  // which is indicated by the asm.js spec in section 7 ("Linking") as well.
  Handle<Object> result;
  LookupIterator it = LookupIterator::PropertyOrElement(
      isolate_, ffi_.ToHandleChecked(), import_name);
  switch (it.state()) {
    case LookupIterator::ACCESS_CHECK:
    case LookupIterator::INTEGER_INDEXED_EXOTIC:
    case LookupIterator::INTERCEPTOR:
    case LookupIterator::JSPROXY:
    case LookupIterator::ACCESSOR:
    case LookupIterator::TRANSITION:
      return ReportLinkError("not a data property", index, import_name);
    case LookupIterator::NOT_FOUND:
      // Accepting missing properties as undefined does not cause any
      // observable difference from JavaScript semantics, we are lenient.
      result = isolate_->factory()->undefined_value();
      break;
    case LookupIterator::DATA:
      result = it.GetDataValue();
      break;
  }

  return result;
}

uint32_t InstanceBuilder::EvalUint32InitExpr(const WasmInitExpr& expr) {
  switch (expr.kind) {
    case WasmInitExpr::kI32Const:
      return expr.val.i32_const;
    case WasmInitExpr::kGlobalIndex: {
      uint32_t offset = module_->globals[expr.val.global_index].offset;
      return *reinterpret_cast<uint32_t*>(raw_buffer_ptr(globals_, offset));
    }
    default:
      UNREACHABLE();
  }
}

// Load data segments into the memory.
void InstanceBuilder::LoadDataSegments(WasmContext* wasm_context) {
  Handle<SeqOneByteString> module_bytes(
      compiled_module_->shared()->module_bytes(), isolate_);
  for (const WasmDataSegment& segment : module_->data_segments) {
    uint32_t source_size = segment.source.length();
    // Segments of size == 0 are just nops.
    if (source_size == 0) continue;
    uint32_t dest_offset = EvalUint32InitExpr(segment.dest_addr);
    DCHECK(in_bounds(dest_offset, source_size, wasm_context->mem_size));
    byte* dest = wasm_context->mem_start + dest_offset;
    const byte* src = reinterpret_cast<const byte*>(
        module_bytes->GetCharsAddress() + segment.source.offset());
    memcpy(dest, src, source_size);
  }
}

void InstanceBuilder::WriteGlobalValue(WasmGlobal& global,
                                       Handle<Object> value) {
  double num = value->Number();
  TRACE("init [globals_start=%p + %u] = %lf, type = %s\n",
        reinterpret_cast<void*>(raw_buffer_ptr(globals_, 0)), global.offset,
        num, WasmOpcodes::TypeName(global.type));
  switch (global.type) {
    case kWasmI32:
      *GetRawGlobalPtr<int32_t>(global) = static_cast<int32_t>(num);
      break;
    case kWasmI64:
      // TODO(titzer): initialization of imported i64 globals.
      UNREACHABLE();
      break;
    case kWasmF32:
      *GetRawGlobalPtr<float>(global) = static_cast<float>(num);
      break;
    case kWasmF64:
      *GetRawGlobalPtr<double>(global) = static_cast<double>(num);
      break;
    default:
      UNREACHABLE();
  }
}

void InstanceBuilder::SanitizeImports() {
  Handle<SeqOneByteString> module_bytes(
      module_object_->compiled_module()->shared()->module_bytes());
  for (size_t index = 0; index < module_->import_table.size(); ++index) {
    WasmImport& import = module_->import_table[index];

    Handle<String> module_name;
    MaybeHandle<String> maybe_module_name =
        WasmSharedModuleData::ExtractUtf8StringFromModuleBytes(
            isolate_, module_bytes, import.module_name);
    if (!maybe_module_name.ToHandle(&module_name)) {
      thrower_->LinkError("Could not resolve module name for import %zu",
                          index);
      return;
    }

    Handle<String> import_name;
    MaybeHandle<String> maybe_import_name =
        WasmSharedModuleData::ExtractUtf8StringFromModuleBytes(
            isolate_, module_bytes, import.field_name);
    if (!maybe_import_name.ToHandle(&import_name)) {
      thrower_->LinkError("Could not resolve import name for import %zu",
                          index);
      return;
    }

    int int_index = static_cast<int>(index);
    MaybeHandle<Object> result =
        module_->is_asm_js()
            ? LookupImportAsm(int_index, import_name)
            : LookupImport(int_index, module_name, import_name);
    if (thrower_->error()) {
      thrower_->LinkError("Could not find value for import %zu", index);
      return;
    }
    Handle<Object> value = result.ToHandleChecked();
    sanitized_imports_.push_back({module_name, import_name, value});
  }
}

Handle<FixedArray> InstanceBuilder::SetupWasmToJSImportsTable(
    Handle<WasmInstanceObject> instance) {
  // The js_imports_table is set up so that index 0 has isolate->native_context
  // and for every index, 3*index+1 has the JSReceiver, 3*index+2 has function's
  // global proxy and 3*index+3 has function's context. Hence, the fixed array's
  // size is 3*import_table.size+1.
  int size = static_cast<int>(module_->import_table.size());
  CHECK_LE(size, (kMaxInt - 1) / 3);
  Handle<FixedArray> func_table =
      isolate_->factory()->NewFixedArray(3 * size + 1, TENURED);
  Handle<FixedArray> js_imports_table =
      isolate_->global_handles()->Create(*func_table);
  GlobalHandles::MakeWeak(
      reinterpret_cast<Object**>(js_imports_table.location()),
      js_imports_table.location(), &FunctionTableFinalizer,
      v8::WeakCallbackType::kFinalizer);
  instance->set_js_imports_table(*func_table);
  js_imports_table->set(0, *isolate_->native_context());
  return js_imports_table;
}

// Process the imports, including functions, tables, globals, and memory, in
// order, loading them from the {ffi_} object. Returns the number of imported
// functions.
int InstanceBuilder::ProcessImports(Handle<WasmInstanceObject> instance) {
  using compiler::kFunctionTableSignatureOffset;
  using compiler::kFunctionTableCodeOffset;
  using compiler::kFunctionTableEntrySize;
  int num_imported_functions = 0;
  int num_imported_tables = 0;
  Handle<FixedArray> js_imports_table = SetupWasmToJSImportsTable(instance);
  WasmInstanceMap imported_wasm_instances(isolate_->heap());
  SetOfNativeModuleModificationScopes set_of_native_module_scopes;

  DCHECK_EQ(module_->import_table.size(), sanitized_imports_.size());
  for (int index = 0; index < static_cast<int>(module_->import_table.size());
       ++index) {
    WasmImport& import = module_->import_table[index];

    Handle<String> module_name = sanitized_imports_[index].module_name;
    Handle<String> import_name = sanitized_imports_[index].import_name;
    Handle<Object> value = sanitized_imports_[index].value;

    switch (import.kind) {
      case kExternalFunction: {
        // Function imports must be callable.
        if (!value->IsCallable()) {
          ReportLinkError("function import requires a callable", index,
                          module_name, import_name);
          return -1;
        }
        wasm::WasmCode* import_code = UnwrapExportOrCompileImportWrapper(
            isolate_, module_->functions[import.index].sig,
            Handle<JSReceiver>::cast(value), num_imported_functions,
            module_->origin(), &imported_wasm_instances, js_imports_table,
            instance);
        if (import_code == nullptr) {
          ReportLinkError("imported function does not match the expected type",
                          index, module_name, import_name);
          return -1;
        }
        RecordStats(import_code, counters());
        num_imported_functions++;
        break;
      }
      case kExternalTable: {
        if (!value->IsWasmTableObject()) {
          ReportLinkError("table import requires a WebAssembly.Table", index,
                          module_name, import_name);
          return -1;
        }
        WasmIndirectFunctionTable& table =
            module_->function_tables[num_imported_tables];
        TableInstance& table_instance = table_instances_[num_imported_tables];
        table_instance.table_object = Handle<WasmTableObject>::cast(value);
        instance->set_table_object(*table_instance.table_object);
        table_instance.js_wrappers = Handle<FixedArray>(
            table_instance.table_object->functions(), isolate_);

        int imported_cur_size = table_instance.js_wrappers->length();
        if (imported_cur_size < static_cast<int>(table.initial_size)) {
          thrower_->LinkError(
              "table import %d is smaller than initial %d, got %u", index,
              table.initial_size, imported_cur_size);
          return -1;
        }

        if (table.has_maximum_size) {
          int64_t imported_maximum_size =
              table_instance.table_object->maximum_length()->Number();
          if (imported_maximum_size < 0) {
            thrower_->LinkError(
                "table import %d has no maximum length, expected %d", index,
                table.maximum_size);
            return -1;
          }
          if (imported_maximum_size > table.maximum_size) {
            thrower_->LinkError(
                " table import %d has a larger maximum size %" PRIx64
                " than the module's declared maximum %u",
                index, imported_maximum_size, table.maximum_size);
            return -1;
          }
        }

        // Allocate a new dispatch table, containing <smi(sig), code> pairs.
        CHECK_GE(kMaxInt / kFunctionTableEntrySize, imported_cur_size);
        int table_size = kFunctionTableEntrySize * imported_cur_size;
        table_instance.function_table =
            isolate_->factory()->NewFixedArray(table_size);
        for (int i = kFunctionTableSignatureOffset; i < table_size;
             i += kFunctionTableEntrySize) {
          table_instance.function_table->set(i, Smi::FromInt(kInvalidSigIndex));
        }
        WasmContext* wasm_context = instance->wasm_context()->get();
        EnsureWasmContextTable(wasm_context, imported_cur_size);
        // Initialize the dispatch table with the (foreign) JS functions
        // that are already in the table.
        for (int i = 0; i < imported_cur_size; ++i) {
          Handle<Object> val(table_instance.js_wrappers->get(i), isolate_);
          // TODO(mtrofin): this is the same logic as WasmTableObject::Set:
          // insert in the local table a wrapper from the other module, and add
          // a reference to the owning instance of the other module.
          if (!val->IsJSFunction()) continue;
          if (!WasmExportedFunction::IsWasmExportedFunction(*val)) {
            thrower_->LinkError("table import %d[%d] is not a wasm function",
                                index, i);
            return -1;
          }
          // Look up the signature's canonical id. If there is no canonical
          // id, then the signature does not appear at all in this module,
          // so putting {-1} in the table will cause checks to always fail.
          auto target = Handle<WasmExportedFunction>::cast(val);
          Handle<WasmInstanceObject> imported_instance =
              handle(target->instance());
          const wasm::WasmCode* exported_code = target->GetWasmCode();
          FunctionSig* sig = imported_instance->module()
                                 ->functions[exported_code->index()]
                                 .sig;
          auto& entry = wasm_context->table[i];
          entry.context = imported_instance->wasm_context()->get();
          entry.sig_id = module_->signature_map.Find(sig);
          entry.target = exported_code->instructions().start();
        }

        num_imported_tables++;
        break;
      }
      case kExternalMemory: {
        // Validation should have failed if more than one memory object was
        // provided.
        DCHECK(!instance->has_memory_object());
        if (!value->IsWasmMemoryObject()) {
          ReportLinkError("memory import must be a WebAssembly.Memory object",
                          index, module_name, import_name);
          return -1;
        }
        auto memory = Handle<WasmMemoryObject>::cast(value);
        instance->set_memory_object(*memory);
        Handle<JSArrayBuffer> buffer(memory->array_buffer(), isolate_);
        memory_ = buffer;
        uint32_t imported_cur_pages = static_cast<uint32_t>(
            buffer->byte_length()->Number() / kWasmPageSize);
        if (imported_cur_pages < module_->initial_pages) {
          thrower_->LinkError(
              "memory import %d is smaller than initial %u, got %u", index,
              module_->initial_pages, imported_cur_pages);
        }
        int32_t imported_maximum_pages = memory->maximum_pages();
        if (module_->has_maximum_pages) {
          if (imported_maximum_pages < 0) {
            thrower_->LinkError(
                "memory import %d has no maximum limit, expected at most %u",
                index, imported_maximum_pages);
            return -1;
          }
          if (static_cast<uint32_t>(imported_maximum_pages) >
              module_->maximum_pages) {
            thrower_->LinkError(
                "memory import %d has a larger maximum size %u than the "
                "module's declared maximum %u",
                index, imported_maximum_pages, module_->maximum_pages);
            return -1;
          }
        }
        if (module_->has_shared_memory != buffer->is_shared()) {
          thrower_->LinkError(
              "mismatch in shared state of memory, declared = %d, imported = "
              "%d",
              module_->has_shared_memory, buffer->is_shared());
          return -1;
        }

        break;
      }
      case kExternalGlobal: {
        // Global imports are converted to numbers and written into the
        // {globals_} array buffer.
        if (module_->globals[import.index].type == kWasmI64) {
          ReportLinkError("global import cannot have type i64", index,
                          module_name, import_name);
          return -1;
        }
        if (module_->is_asm_js()) {
          // Accepting {JSFunction} on top of just primitive values here is a
          // workaround to support legacy asm.js code with broken binding. Note
          // that using {NaN} (or Smi::kZero) here is what using the observable
          // conversion via {ToPrimitive} would produce as well.
          // TODO(mstarzinger): Still observable if Function.prototype.valueOf
          // or friends are patched, we might need to check for that as well.
          if (value->IsJSFunction()) value = isolate_->factory()->nan_value();
          if (value->IsPrimitive() && !value->IsSymbol()) {
            if (module_->globals[import.index].type == kWasmI32) {
              value = Object::ToInt32(isolate_, value).ToHandleChecked();
            } else {
              value = Object::ToNumber(value).ToHandleChecked();
            }
          }
        }
        if (!value->IsNumber()) {
          ReportLinkError("global import must be a number", index, module_name,
                          import_name);
          return -1;
        }
        WriteGlobalValue(module_->globals[import.index], value);
        break;
      }
      default:
        UNREACHABLE();
        break;
    }
  }

  if (!imported_wasm_instances.empty()) {
    WasmInstanceMap::IteratableScope iteratable_scope(&imported_wasm_instances);
    Handle<FixedArray> instances_array = isolate_->factory()->NewFixedArray(
        imported_wasm_instances.size(), TENURED);
    instance->set_directly_called_instances(*instances_array);
    int index = 0;
    for (auto it = iteratable_scope.begin(), end = iteratable_scope.end();
         it != end; ++it, ++index) {
      instances_array->set(index, ***it);
    }
  }

  return num_imported_functions;
}

template <typename T>
T* InstanceBuilder::GetRawGlobalPtr(WasmGlobal& global) {
  return reinterpret_cast<T*>(raw_buffer_ptr(globals_, global.offset));
}

// Process initialization of globals.
void InstanceBuilder::InitGlobals() {
  for (auto global : module_->globals) {
    switch (global.init.kind) {
      case WasmInitExpr::kI32Const:
        *GetRawGlobalPtr<int32_t>(global) = global.init.val.i32_const;
        break;
      case WasmInitExpr::kI64Const:
        *GetRawGlobalPtr<int64_t>(global) = global.init.val.i64_const;
        break;
      case WasmInitExpr::kF32Const:
        *GetRawGlobalPtr<float>(global) = global.init.val.f32_const;
        break;
      case WasmInitExpr::kF64Const:
        *GetRawGlobalPtr<double>(global) = global.init.val.f64_const;
        break;
      case WasmInitExpr::kGlobalIndex: {
        // Initialize with another global.
        uint32_t new_offset = global.offset;
        uint32_t old_offset =
            module_->globals[global.init.val.global_index].offset;
        TRACE("init [globals+%u] = [globals+%d]\n", global.offset, old_offset);
        size_t size = (global.type == kWasmI64 || global.type == kWasmF64)
                          ? sizeof(double)
                          : sizeof(int32_t);
        memcpy(raw_buffer_ptr(globals_, new_offset),
               raw_buffer_ptr(globals_, old_offset), size);
        break;
      }
      case WasmInitExpr::kNone:
        // Happens with imported globals.
        break;
      default:
        UNREACHABLE();
        break;
    }
  }
}

// Allocate memory for a module instance as a new JSArrayBuffer.
Handle<JSArrayBuffer> InstanceBuilder::AllocateMemory(uint32_t num_pages) {
  if (num_pages > FLAG_wasm_max_mem_pages) {
    thrower_->RangeError("Out of memory: wasm memory too large");
    return Handle<JSArrayBuffer>::null();
  }
  const bool enable_guard_regions = use_trap_handler();
  const bool is_shared_memory =
      module_->has_shared_memory && i::FLAG_experimental_wasm_threads;
  Handle<JSArrayBuffer> mem_buffer = NewArrayBuffer(
      isolate_, num_pages * kWasmPageSize, enable_guard_regions,
      is_shared_memory ? i::SharedFlag::kShared : i::SharedFlag::kNotShared);

  if (mem_buffer.is_null()) {
    thrower_->RangeError("Out of memory: wasm memory");
  }
  return mem_buffer;
}

bool InstanceBuilder::NeedsWrappers() const {
  if (module_->num_exported_functions > 0) return true;
  for (auto& table_instance : table_instances_) {
    if (!table_instance.js_wrappers.is_null()) return true;
  }
  for (auto& table : module_->function_tables) {
    if (table.exported) return true;
  }
  return false;
}

// Process the exports, creating wrappers for functions, tables, memories,
// and globals.
void InstanceBuilder::ProcessExports(
    Handle<WasmInstanceObject> instance,
    Handle<WasmCompiledModule> compiled_module) {
  Handle<FixedArray> wrapper_table(compiled_module->export_wrappers(),
                                   isolate_);
  if (NeedsWrappers()) {
    // Fill the table to cache the exported JSFunction wrappers.
    js_wrappers_.insert(js_wrappers_.begin(), module_->functions.size(),
                        Handle<JSFunction>::null());

    // If an imported WebAssembly function gets exported, the exported function
    // has to be identical to to imported function. Therefore we put all
    // imported WebAssembly functions into the js_wrappers_ list.
    for (int index = 0, end = static_cast<int>(module_->import_table.size());
         index < end; ++index) {
      WasmImport& import = module_->import_table[index];
      if (import.kind == kExternalFunction) {
        Handle<Object> value = sanitized_imports_[index].value;
        if (WasmExportedFunction::IsWasmExportedFunction(*value)) {
          js_wrappers_[import.index] = Handle<JSFunction>::cast(value);
        }
      }
    }
  }

  Handle<JSObject> exports_object;
  if (module_->is_wasm()) {
    // Create the "exports" object.
    exports_object = isolate_->factory()->NewJSObjectWithNullProto();
  } else if (module_->is_asm_js()) {
    Handle<JSFunction> object_function = Handle<JSFunction>(
        isolate_->native_context()->object_function(), isolate_);
    exports_object = isolate_->factory()->NewJSObject(object_function);
  } else {
    UNREACHABLE();
  }
  instance->set_exports_object(*exports_object);

  Handle<String> single_function_name =
      isolate_->factory()->InternalizeUtf8String(AsmJs::kSingleFunctionName);

  PropertyDescriptor desc;
  desc.set_writable(module_->is_asm_js());
  desc.set_enumerable(true);
  desc.set_configurable(module_->is_asm_js());

  // Store weak references to all exported functions.
  Handle<FixedArray> weak_exported_functions;
  if (compiled_module->has_weak_exported_functions()) {
    weak_exported_functions =
        handle(compiled_module->weak_exported_functions(), isolate_);
  } else {
    int export_count = 0;
    for (WasmExport& exp : module_->export_table) {
      if (exp.kind == kExternalFunction) ++export_count;
    }
    weak_exported_functions = isolate_->factory()->NewFixedArray(export_count);
    compiled_module->set_weak_exported_functions(*weak_exported_functions);
  }

  // Process each export in the export table.
  int export_index = 0;  // Index into {weak_exported_functions}.
  for (WasmExport& exp : module_->export_table) {
    Handle<String> name =
        WasmSharedModuleData::ExtractUtf8StringFromModuleBytes(
            isolate_, handle(compiled_module_->shared(), isolate_), exp.name)
            .ToHandleChecked();
    Handle<JSObject> export_to;
    if (module_->is_asm_js() && exp.kind == kExternalFunction &&
        String::Equals(name, single_function_name)) {
      export_to = instance;
    } else {
      export_to = exports_object;
    }

    switch (exp.kind) {
      case kExternalFunction: {
        // Wrap and export the code as a JSFunction.
        WasmFunction& function = module_->functions[exp.index];
        Handle<JSFunction> js_function = js_wrappers_[exp.index];
        if (js_function.is_null()) {
          // Wrap the exported code as a JSFunction.
          Handle<Code> export_code =
              wrapper_table->GetValueChecked<Code>(isolate_, export_index);
          MaybeHandle<String> func_name;
          if (module_->is_asm_js()) {
            // For modules arising from asm.js, honor the names section.
            WireBytesRef func_name_ref =
                module_->LookupName(compiled_module_->shared()->module_bytes(),
                                    function.func_index);
            func_name =
                WasmSharedModuleData::ExtractUtf8StringFromModuleBytes(
                    isolate_, handle(compiled_module_->shared(), isolate_),
                    func_name_ref)
                    .ToHandleChecked();
          }
          js_function = WasmExportedFunction::New(
              isolate_, instance, func_name, function.func_index,
              static_cast<int>(function.sig->parameter_count()), export_code);
          js_wrappers_[exp.index] = js_function;
        }
        desc.set_value(js_function);
        Handle<WeakCell> weak_export =
            isolate_->factory()->NewWeakCell(js_function);
        DCHECK_GT(weak_exported_functions->length(), export_index);
        weak_exported_functions->set(export_index, *weak_export);
        export_index++;
        break;
      }
      case kExternalTable: {
        // Export a table as a WebAssembly.Table object.
        TableInstance& table_instance = table_instances_[exp.index];
        WasmIndirectFunctionTable& table = module_->function_tables[exp.index];
        if (table_instance.table_object.is_null()) {
          uint32_t maximum = table.has_maximum_size ? table.maximum_size
                                                    : FLAG_wasm_max_table_size;
          table_instance.table_object =
              WasmTableObject::New(isolate_, table.initial_size, maximum,
                                   &table_instance.js_wrappers);
        }
        desc.set_value(table_instance.table_object);
        break;
      }
      case kExternalMemory: {
        // Export the memory as a WebAssembly.Memory object. A WasmMemoryObject
        // should already be available if the module has memory, since we always
        // create or import it when building an WasmInstanceObject.
        DCHECK(instance->has_memory_object());
        desc.set_value(
            Handle<WasmMemoryObject>(instance->memory_object(), isolate_));
        break;
      }
      case kExternalGlobal: {
        // Export the value of the global variable as a number.
        WasmGlobal& global = module_->globals[exp.index];
        double num = 0;
        switch (global.type) {
          case kWasmI32:
            num = *GetRawGlobalPtr<int32_t>(global);
            break;
          case kWasmF32:
            num = *GetRawGlobalPtr<float>(global);
            break;
          case kWasmF64:
            num = *GetRawGlobalPtr<double>(global);
            break;
          case kWasmI64:
            thrower_->LinkError(
                "export of globals of type I64 is not allowed.");
            return;
          default:
            UNREACHABLE();
        }
        desc.set_value(isolate_->factory()->NewNumber(num));
        break;
      }
      default:
        UNREACHABLE();
        break;
    }

    v8::Maybe<bool> status = JSReceiver::DefineOwnProperty(
        isolate_, export_to, name, &desc, kThrowOnError);
    if (!status.IsJust()) {
      TruncatedUserString<> trunc_name(name->GetCharVector<uint8_t>());
      thrower_->LinkError("export of %.*s failed.", trunc_name.length(),
                          trunc_name.start());
      return;
    }
  }
  DCHECK_EQ(export_index, weak_exported_functions->length());

  if (module_->is_wasm()) {
    v8::Maybe<bool> success =
        JSReceiver::SetIntegrityLevel(exports_object, FROZEN, kDontThrow);
    DCHECK(success.FromMaybe(false));
    USE(success);
  }
}

void InstanceBuilder::InitializeTables(
    Handle<WasmInstanceObject> instance,
    CodeSpecialization* code_specialization) {
  size_t function_table_count = module_->function_tables.size();

  // function_table_count is 0 or 1, so we just create these objects even if not
  // needed for native wasm.

  // These go on the instance.
  Handle<FixedArray> rooted_function_tables =
      isolate_->factory()->NewFixedArray(static_cast<int>(function_table_count),
                                         TENURED);

  instance->set_function_tables(*rooted_function_tables);

  for (size_t index = 0; index < function_table_count; ++index) {
    WasmIndirectFunctionTable& table = module_->function_tables[index];
    TableInstance& table_instance = table_instances_[index];
    // The table holds <smi(sig), code> pairs.
    CHECK_GE(kMaxInt / compiler::kFunctionTableEntrySize, table.initial_size);
    int num_table_entries = static_cast<int>(table.initial_size);
    int table_size = compiler::kFunctionTableEntrySize * num_table_entries;

    WasmContext* wasm_context = instance->wasm_context()->get();
    EnsureWasmContextTable(wasm_context, num_table_entries);

    if (table_instance.function_table.is_null()) {
      // Create a new dispatch table if necessary.
      table_instance.function_table =
          isolate_->factory()->NewFixedArray(table_size);
      for (int i = compiler::kFunctionTableSignatureOffset; i < table_size;
           i += compiler::kFunctionTableEntrySize) {
        // Fill the table with invalid signature indexes so that
        // uninitialized entries will always fail the signature check.
        table_instance.function_table->set(i, Smi::FromInt(kInvalidSigIndex));
      }
    }
    int int_index = static_cast<int>(index);

    Handle<FixedArray> global_func_table =
        isolate_->global_handles()->Create(*table_instance.function_table);
    // Make the handles weak. The table objects are rooted on the instance, as
    // they belong to it. We need the global handles in order to have stable
    // pointers to embed in the instance's specialization (wasm compiled code).
    // The order of finalization doesn't matter, in that the instance finalizer
    // may be called before each table's finalizer, or vice-versa.
    // This is because values used for embedding are only interesting should we
    // {Reset} a specialization, in which case they are interesting as values,
    // they are not dereferenced.
    GlobalHandles::MakeWeak(
        reinterpret_cast<Object**>(global_func_table.location()),
        global_func_table.location(), &FunctionTableFinalizer,
        v8::WeakCallbackType::kFinalizer);

    rooted_function_tables->set(int_index, *global_func_table);
  }
}

void InstanceBuilder::LoadTableSegments(Handle<WasmInstanceObject> instance) {
  wasm::NativeModule* native_module = compiled_module_->GetNativeModule();
  int function_table_count = static_cast<int>(module_->function_tables.size());
  for (int index = 0; index < function_table_count; ++index) {
    TableInstance& table_instance = table_instances_[index];

    // Count the number of table exports for each function (needed for lazy
    // compilation).
    std::unordered_map<uint32_t, uint32_t> num_table_exports;
    if (compile_lazy(module_)) {
      for (auto& table_init : module_->table_inits) {
        for (uint32_t func_index : table_init.entries) {
          const wasm::WasmCode* code = native_module->GetCode(func_index);
          // Only increase the counter for lazy compile builtins (it's not
          // needed otherwise).
          if (code->kind() != wasm::WasmCode::kLazyStub) {
            DCHECK(code->kind() == wasm::WasmCode::kFunction ||
                   code->kind() == wasm::WasmCode::kWasmToJsWrapper ||
                   code->kind() == wasm::WasmCode::kWasmToWasmWrapper);
            continue;
          }
          ++num_table_exports[func_index];
        }
      }
    }

    // TODO(titzer): this does redundant work if there are multiple tables,
    // since initializations are not sorted by table index.
    for (auto& table_init : module_->table_inits) {
      uint32_t base = EvalUint32InitExpr(table_init.offset);
      uint32_t num_entries = static_cast<uint32_t>(table_init.entries.size());
      DCHECK(in_bounds(base, num_entries,
                       table_instance.function_table->length() /
                           compiler::kFunctionTableEntrySize));
      for (uint32_t i = 0; i < num_entries; ++i) {
        uint32_t func_index = table_init.entries[i];
        WasmFunction* function = &module_->functions[func_index];
        int table_index = static_cast<int>(i + base);

        // Update the local dispatch table first.
        uint32_t sig_id = module_->signature_ids[function->sig_index];
        table_instance.function_table->set(
            compiler::FunctionTableSigOffset(table_index),
            Smi::FromInt(sig_id));
        wasm::WasmCode* wasm_code = EnsureTableExportLazyDeoptData(
            isolate_, native_module, func_index, table_instance.function_table,
            table_index, &num_table_exports);
        Handle<Foreign> as_foreign = isolate_->factory()->NewForeign(
            wasm_code->instructions().start(), TENURED);
        table_instance.function_table->set(
            compiler::FunctionTableCodeOffset(table_index), *as_foreign);

        WasmContext* wasm_context = instance->wasm_context()->get();
        auto& entry = wasm_context->table[table_index];
        entry.sig_id = sig_id;
        entry.context = wasm_context;
        entry.target = wasm_code->instructions().start();

        if (!table_instance.table_object.is_null()) {
          // Update the table object's other dispatch tables.
          if (js_wrappers_[func_index].is_null()) {
            // No JSFunction entry yet exists for this function. Create one.
            // TODO(titzer): We compile JS->wasm wrappers for functions are
            // not exported but are in an exported table. This should be done
            // at module compile time and cached instead.

            Handle<Code> wrapper_code =
                js_to_wasm_cache_.CloneOrCompileJSToWasmWrapper(
                    isolate_, module_, wasm_code, func_index,
                    instance->compiled_module()->use_trap_handler());
            MaybeHandle<String> func_name;
            if (module_->is_asm_js()) {
              // For modules arising from asm.js, honor the names section.
              WireBytesRef func_name_ref = module_->LookupName(
                  compiled_module_->shared()->module_bytes(), func_index);
              func_name =
                  WasmSharedModuleData::ExtractUtf8StringFromModuleBytes(
                      isolate_, handle(compiled_module_->shared(), isolate_),
                      func_name_ref)
                      .ToHandleChecked();
            }
            Handle<WasmExportedFunction> js_function =
                WasmExportedFunction::New(
                    isolate_, instance, func_name, func_index,
                    static_cast<int>(function->sig->parameter_count()),
                    wrapper_code);
            js_wrappers_[func_index] = js_function;
          }
          table_instance.js_wrappers->set(table_index,
                                          *js_wrappers_[func_index]);
          // UpdateDispatchTables() should update this instance as well.
          WasmTableObject::UpdateDispatchTables(
              isolate_, table_instance.table_object, table_index, function->sig,
              instance, wasm_code, func_index);
        }
      }
    }

#ifdef DEBUG
    // Check that the count of table exports was accurate. The entries are
    // decremented on each export, so all should be zero now.
    for (auto e : num_table_exports) {
      DCHECK_EQ(0, e.second);
    }
#endif

    // TODO(titzer): we add the new dispatch table at the end to avoid
    // redundant work and also because the new instance is not yet fully
    // initialized.
    if (!table_instance.table_object.is_null()) {
      // Add the new dispatch table to the WebAssembly.Table object.
      WasmTableObject::AddDispatchTable(isolate_, table_instance.table_object,
                                        instance, index,
                                        table_instance.function_table);
    }
  }
}

AsyncCompileJob::AsyncCompileJob(Isolate* isolate,
                                 std::unique_ptr<byte[]> bytes_copy,
                                 size_t length, Handle<Context> context,
                                 Handle<JSPromise> promise)
    : isolate_(isolate),
      async_counters_(isolate->async_counters()),
      bytes_copy_(std::move(bytes_copy)),
      wire_bytes_(bytes_copy_.get(), bytes_copy_.get() + length) {
  v8::Isolate* v8_isolate = reinterpret_cast<v8::Isolate*>(isolate);
  v8::Platform* platform = V8::GetCurrentPlatform();
  foreground_task_runner_ = platform->GetForegroundTaskRunner(v8_isolate);
  background_task_runner_ = platform->GetWorkerThreadsTaskRunner(v8_isolate);
  // The handles for the context and promise must be deferred.
  DeferredHandleScope deferred(isolate);
  context_ = Handle<Context>(*context);
  module_promise_ = Handle<JSPromise>(*promise);
  deferred_handles_.push_back(deferred.Detach());
}

void AsyncCompileJob::Start() {
  DoAsync<DecodeModule>();  // --
}

void AsyncCompileJob::Abort() {
  background_task_manager_.CancelAndWait();
  if (!compiled_module_.is_null()) {
    compiled_module_->GetNativeModule()->compilation_state()->Abort();
  }
  if (num_pending_foreground_tasks_ == 0) {
    // No task is pending, we can just remove the AsyncCompileJob.
    isolate_->wasm_engine()->compilation_manager()->RemoveJob(this);
  } else {
    // There is still a compilation task in the task queue. We enter the
    // AbortCompilation state and wait for this compilation task to abort the
    // AsyncCompileJob.
    NextStep<AbortCompilation>();
  }
}

class AsyncStreamingProcessor final : public StreamingProcessor {
 public:
  explicit AsyncStreamingProcessor(AsyncCompileJob* job);

  bool ProcessModuleHeader(Vector<const uint8_t> bytes,
                           uint32_t offset) override;

  bool ProcessSection(SectionCode section_code, Vector<const uint8_t> bytes,
                      uint32_t offset) override;

  bool ProcessCodeSectionHeader(size_t functions_count,
                                uint32_t offset) override;

  bool ProcessFunctionBody(Vector<const uint8_t> bytes,
                           uint32_t offset) override;

  void OnFinishedChunk() override;

  void OnFinishedStream(std::unique_ptr<uint8_t[]> bytes,
                        size_t length) override;

  void OnError(DecodeResult result) override;

  void OnAbort() override;

 private:
  // Finishes the AsyncCOmpileJob with an error.
  void FinishAsyncCompileJobWithError(ResultBase result);

  void CommitCompilationUnits();

  ModuleDecoder decoder_;
  AsyncCompileJob* job_;
  std::unique_ptr<CompilationUnitBuilder> compilation_unit_builder_;
  uint32_t next_function_ = 0;
};

std::shared_ptr<StreamingDecoder> AsyncCompileJob::CreateStreamingDecoder() {
  DCHECK_NULL(stream_);
  stream_.reset(
      new StreamingDecoder(base::make_unique<AsyncStreamingProcessor>(this)));
  return stream_;
}

AsyncCompileJob::~AsyncCompileJob() {
  background_task_manager_.CancelAndWait();
  for (auto d : deferred_handles_) delete d;
}

void AsyncCompileJob::AsyncCompileFailed(Handle<Object> error_reason) {
  if (stream_) stream_->NotifyError();
  // {job} keeps the {this} pointer alive.
  std::shared_ptr<AsyncCompileJob> job =
      isolate_->wasm_engine()->compilation_manager()->RemoveJob(this);
  MaybeHandle<Object> promise_result =
      JSPromise::Reject(module_promise_, error_reason);
  CHECK_EQ(promise_result.is_null(), isolate_->has_pending_exception());
}

void AsyncCompileJob::AsyncCompileSucceeded(Handle<Object> result) {
  // {job} keeps the {this} pointer alive.
  std::shared_ptr<AsyncCompileJob> job =
      isolate_->wasm_engine()->compilation_manager()->RemoveJob(this);
  MaybeHandle<Object> promise_result =
      JSPromise::Resolve(module_promise_, result);
  CHECK_EQ(promise_result.is_null(), isolate_->has_pending_exception());
}

// A closure to run a compilation step (either as foreground or background
// task) and schedule the next step(s), if any.
class AsyncCompileJob::CompileStep {
 public:
  explicit CompileStep(int num_background_tasks = 0)
      : num_background_tasks_(num_background_tasks) {}

  virtual ~CompileStep() {}

  void Run(bool on_foreground) {
    if (on_foreground) {
      HandleScope scope(job_->isolate_);
      --job_->num_pending_foreground_tasks_;
      DCHECK_EQ(0, job_->num_pending_foreground_tasks_);
      SaveContext saved_context(job_->isolate_);
      job_->isolate_->set_context(*job_->context_);
      RunInForeground();
    } else {
      RunInBackground();
    }
  }

  virtual void RunInForeground() { UNREACHABLE(); }
  virtual void RunInBackground() { UNREACHABLE(); }

  int NumberOfBackgroundTasks() { return num_background_tasks_; }

  AsyncCompileJob* job_ = nullptr;
  const int num_background_tasks_;
};

class AsyncCompileJob::CompileTask : public CancelableTask {
 public:
  CompileTask(AsyncCompileJob* job, bool on_foreground)
      // We only manage the background tasks with the {CancelableTaskManager} of
      // the {AsyncCompileJob}. Foreground tasks are managed by the system's
      // {CancelableTaskManager}. Background tasks cannot spawn tasks managed by
      // their own task manager.
      : CancelableTask(on_foreground ? job->isolate_->cancelable_task_manager()
                                     : &job->background_task_manager_),
        job_(job),
        on_foreground_(on_foreground) {}

  void RunInternal() override { job_->step_->Run(on_foreground_); }

 private:
  AsyncCompileJob* job_;
  bool on_foreground_;
};

void AsyncCompileJob::StartForegroundTask() {
  ++num_pending_foreground_tasks_;
  DCHECK_EQ(1, num_pending_foreground_tasks_);

  foreground_task_runner_->PostTask(base::make_unique<CompileTask>(this, true));
}

template <typename Step, typename... Args>
void AsyncCompileJob::DoSync(Args&&... args) {
  NextStep<Step>(std::forward<Args>(args)...);
  StartForegroundTask();
}

void AsyncCompileJob::StartBackgroundTask() {
  background_task_runner_->PostTask(
      base::make_unique<CompileTask>(this, false));
}

template <typename Step, typename... Args>
void AsyncCompileJob::DoAsync(Args&&... args) {
  NextStep<Step>(std::forward<Args>(args)...);
  int end = step_->NumberOfBackgroundTasks();
  for (int i = 0; i < end; ++i) {
    StartBackgroundTask();
  }
}

template <typename Step, typename... Args>
void AsyncCompileJob::NextStep(Args&&... args) {
  step_.reset(new Step(std::forward<Args>(args)...));
  step_->job_ = this;
}

//==========================================================================
// Step 1: (async) Decode the module.
//==========================================================================
class AsyncCompileJob::DecodeModule : public AsyncCompileJob::CompileStep {
 public:
  DecodeModule() : CompileStep(1) {}

  void RunInBackground() override {
    ModuleResult result;
    {
      DisallowHandleAllocation no_handle;
      DisallowHeapAllocation no_allocation;
      // Decode the module bytes.
      TRACE_COMPILE("(1) Decoding module...\n");
      result = AsyncDecodeWasmModule(job_->isolate_, job_->wire_bytes_.start(),
                                     job_->wire_bytes_.end(), false,
                                     kWasmOrigin, job_->async_counters());
    }
    if (result.failed()) {
      // Decoding failure; reject the promise and clean up.
      job_->DoSync<DecodeFail>(std::move(result));
    } else {
      // Decode passed.
      job_->module_ = std::move(result.val);
      job_->DoSync<PrepareAndStartCompile>(job_->module_.get(), true);
    }
  }
};

//==========================================================================
// Step 1b: (sync) Fail decoding the module.
//==========================================================================
class AsyncCompileJob::DecodeFail : public CompileStep {
 public:
  explicit DecodeFail(ModuleResult result) : result_(std::move(result)) {}

 private:
  ModuleResult result_;
  void RunInForeground() override {
    TRACE_COMPILE("(1b) Decoding failed.\n");
    ErrorThrower thrower(job_->isolate_, "AsyncCompile");
    thrower.CompileFailed("Wasm decoding failed", result_);
    // {job_} is deleted in AsyncCompileFailed, therefore the {return}.
    return job_->AsyncCompileFailed(thrower.Reify());
  }
};

//==========================================================================
// Step 2 (sync): Create heap-allocated data and start compile.
//==========================================================================
class AsyncCompileJob::PrepareAndStartCompile : public CompileStep {
 public:
  explicit PrepareAndStartCompile(WasmModule* module, bool start_compilation)
      : module_(module), start_compilation_(start_compilation) {}

 private:
  WasmModule* module_;
  bool start_compilation_;

  void RunInForeground() override {
    TRACE_COMPILE("(2) Prepare and start compile...\n");

    // Make sure all compilation tasks stopped running. Decoding (async step)
    // is done.
    job_->background_task_manager_.CancelAndWait();

    Isolate* isolate = job_->isolate_;

    job_->module_env_ = CreateDefaultModuleEnv(isolate, module_);

    Handle<Code> centry_stub = CEntryStub(isolate, 1).GetCode();
    {
      // Now reopen the handles in a deferred scope in order to use
      // them in the concurrent steps.
      DeferredHandleScope deferred(isolate);
      job_->centry_stub_ = Handle<Code>(*centry_stub, isolate);
      job_->deferred_handles_.push_back(deferred.Detach());
    }

    DCHECK_LE(module_->num_imported_functions, module_->functions.size());
    // Create the compiled module object and populate with compiled functions
    // and information needed at instantiation time. This object needs to be
    // serializable. Instantiation may occur off a deserialized version of
    // this object.
    int export_wrapper_size = static_cast<int>(module_->num_exported_functions);
    Handle<FixedArray> export_wrappers =
        job_->isolate_->factory()->NewFixedArray(export_wrapper_size, TENURED);

    job_->compiled_module_ = NewCompiledModule(
        job_->isolate_, module_, export_wrappers, job_->module_env_.get());
    job_->compiled_module_->GetNativeModule()
        ->compilation_state()
        ->EnableThrottling();

    {
      DeferredHandleScope deferred(job_->isolate_);
      job_->compiled_module_ = handle(*job_->compiled_module_, job_->isolate_);
      job_->deferred_handles_.push_back(deferred.Detach());
    }
    size_t num_functions =
        module_->functions.size() - module_->num_imported_functions;

    if (num_functions == 0) {
      // Degenerate case of an empty module.
      job_->DoSync<FinishCompile>();
      return;
    }

    CompilationState* compilation_state =
        job_->compiled_module_->GetNativeModule()->compilation_state();
    {
      // Instance field {job_} cannot be captured by copy, therefore
      // we need to add a local helper variable {job}. We want to
      // capture the {job} pointer by copy, as it otherwise is dependent
      // on the current step we are in.
      AsyncCompileJob* job = job_;
      compilation_state->AddCallback(
          [job](CompilationEvent event, Handle<Object> error) {
            switch (event) {
              case CompilationEvent::kFinishedBaselineCompilation:
                if (job->DecrementAndCheckFinisherCount()) {
                  job->DoSync<FinishCompile>();
                }
                return;
              case CompilationEvent::kFailedCompilation:
                DeferredHandleScope deferred(job->isolate());
                error = handle(*error, job->isolate());
                job->deferred_handles_.push_back(deferred.Detach());
                job->DoSync<CompileFailed>(error);
                return;
            }
            UNREACHABLE();
          });
    }
    if (start_compilation_) {
      // TODO(ahaas): Try to remove the {start_compilation_} check when
      // streaming decoding is done in the background. If
      // InitializeCompilationUnits always returns 0 for streaming compilation,
      // then DoAsync would do the same as NextStep already.

      size_t functions_count =
          GetNumFunctionsToCompile(module_->functions, job_->module_env_.get());
      compilation_state->SetNumberOfFunctionsToCompile(functions_count);
      // Add compilation units and kick off compilation.
      InitializeCompilationUnits(module_->functions, job_->wire_bytes_,
                                 job_->module_env_.get(), job_->centry_stub_,
                                 job_->compiled_module_->GetNativeModule());
    }
  }
};

//==========================================================================
// Step 4b (sync): Compilation failed. Reject Promise.
//==========================================================================
class AsyncCompileJob::CompileFailed : public CompileStep {
 public:
  explicit CompileFailed(Handle<Object> error_reason)
      : error_reason_(error_reason) {}

  void RunInForeground() override {
    TRACE_COMPILE("(4b) Compilation Failed...\n");
    return job_->AsyncCompileFailed(error_reason_);
  }

 private:
  Handle<Object> error_reason_;
};

//==========================================================================
// Step 5 (sync): Finish heap-allocated data structures.
//==========================================================================
class AsyncCompileJob::FinishCompile : public CompileStep {
  void RunInForeground() override {
    TRACE_COMPILE("(5b) Finish compile...\n");
    RecordStats(job_->compiled_module_->GetNativeModule(), job_->counters());

    // Create heap objects for script and module bytes to be stored in the
    // shared module data. Asm.js is not compiled asynchronously.
    Handle<Script> script = CreateWasmScript(job_->isolate_, job_->wire_bytes_);
    Handle<ByteArray> asm_js_offset_table;
    // TODO(wasm): Improve efficiency of storing module wire bytes.
    //   1. Only store relevant sections, not function bodies
    //   2. Don't make a second copy of the bytes here; reuse the copy made
    //      for asynchronous compilation and store it as an external one
    //      byte string for serialization/deserialization.
    Handle<String> module_bytes =
        job_->isolate_->factory()
            ->NewStringFromOneByte(
                {job_->wire_bytes_.start(), job_->wire_bytes_.length()},
                TENURED)
            .ToHandleChecked();
    DCHECK(module_bytes->IsSeqOneByteString());

    // The {module_wrapper} will take ownership of the {WasmModule} object,
    // and it will be destroyed when the GC reclaims the wrapper object.
    Handle<WasmModuleWrapper> module_wrapper =
        WasmModuleWrapper::From(job_->isolate_, job_->module_.release());

    // Create the shared module data.
    // TODO(clemensh): For the same module (same bytes / same hash), we should
    // only have one WasmSharedModuleData. Otherwise, we might only set
    // breakpoints on a (potentially empty) subset of the instances.

    Handle<WasmSharedModuleData> shared =
        WasmSharedModuleData::New(job_->isolate_, module_wrapper,
                                  Handle<SeqOneByteString>::cast(module_bytes),
                                  script, asm_js_offset_table);
    job_->compiled_module_->set_shared(*shared);
    script->set_wasm_compiled_module(*job_->compiled_module_);

    // Finish the wasm script now and make it public to the debugger.
    job_->isolate_->debug()->OnAfterCompile(
        handle(job_->compiled_module_->shared()->script()));

    // TODO(wasm): compiling wrappers should be made async as well.
    job_->DoSync<CompileWrappers>();
  }
};

//==========================================================================
// Step 6 (sync): Compile JS->wasm wrappers.
//==========================================================================
class AsyncCompileJob::CompileWrappers : public CompileStep {
  // TODO(wasm): Compile all wrappers here, including the start function wrapper
  // and the wrappers for the function table elements.
  void RunInForeground() override {
    TRACE_COMPILE("(6) Compile wrappers...\n");
    // TODO(6792): No longer needed once WebAssembly code is off heap.
    CodeSpaceMemoryModificationScope modification_scope(job_->isolate_->heap());
    // Compile JS->wasm wrappers for exported functions.
    CompileJsToWasmWrappers(job_->isolate_, job_->compiled_module_,
                            job_->counters());
    job_->DoSync<FinishModule>();
  }
};

//==========================================================================
// Step 7 (sync): Finish the module and resolve the promise.
//==========================================================================
class AsyncCompileJob::FinishModule : public CompileStep {
  void RunInForeground() override {
    TRACE_COMPILE("(7) Finish module...\n");
    Handle<WasmModuleObject> result =
        WasmModuleObject::New(job_->isolate_, job_->compiled_module_);
    // {job_} is deleted in AsyncCompileSucceeded, therefore the {return}.
    return job_->AsyncCompileSucceeded(result);
  }
};

class AsyncCompileJob::AbortCompilation : public CompileStep {
  void RunInForeground() override {
    TRACE_COMPILE("Abort asynchronous compilation ...\n");
    job_->isolate_->wasm_engine()->compilation_manager()->RemoveJob(job_);
  }
};

AsyncStreamingProcessor::AsyncStreamingProcessor(AsyncCompileJob* job)
    : job_(job), compilation_unit_builder_(nullptr) {}

void AsyncStreamingProcessor::FinishAsyncCompileJobWithError(ResultBase error) {
  // Make sure all background tasks stopped executing before we change the state
  // of the AsyncCompileJob to DecodeFail.
  job_->background_task_manager_.CancelAndWait();

  // Create a ModuleResult from the result we got as parameter. Since there was
  // no error, we don't have to provide a real wasm module to the ModuleResult.
  ModuleResult result(nullptr);
  result.MoveErrorFrom(error);

  // Check if there is already a CompiledModule, in which case we have to clean
  // up the CompilationState as well.
  if (!job_->compiled_module_.is_null()) {
    job_->compiled_module_->GetNativeModule()->compilation_state()->Abort();

    if (job_->num_pending_foreground_tasks_ == 0) {
      job_->DoSync<AsyncCompileJob::DecodeFail>(std::move(result));
    } else {
      job_->NextStep<AsyncCompileJob::DecodeFail>(std::move(result));
    }

    // Clear the {compilation_unit_builder_} if it exists. This is needed
    // because there is a check in the destructor of the
    // {CompilationUnitBuilder} that it is empty.
    if (compilation_unit_builder_) compilation_unit_builder_->Clear();
  } else {
    job_->DoSync<AsyncCompileJob::DecodeFail>(std::move(result));
  }
}

// Process the module header.
bool AsyncStreamingProcessor::ProcessModuleHeader(Vector<const uint8_t> bytes,
                                                  uint32_t offset) {
  TRACE_STREAMING("Process module header...\n");
  decoder_.StartDecoding(job_->isolate());
  decoder_.DecodeModuleHeader(bytes, offset);
  if (!decoder_.ok()) {
    FinishAsyncCompileJobWithError(decoder_.FinishDecoding(false));
    return false;
  }
  return true;
}

// Process all sections except for the code section.
bool AsyncStreamingProcessor::ProcessSection(SectionCode section_code,
                                             Vector<const uint8_t> bytes,
                                             uint32_t offset) {
  TRACE_STREAMING("Process section %d ...\n", section_code);
  if (compilation_unit_builder_) {
    // We reached a section after the code section, we do not need the
    // compilation_unit_builder_ anymore.
    CommitCompilationUnits();
    compilation_unit_builder_.reset();
  }
  if (section_code == SectionCode::kUnknownSectionCode) {
    Decoder decoder(bytes, offset);
    section_code = ModuleDecoder::IdentifyUnknownSection(
        decoder, bytes.start() + bytes.length());
    if (section_code == SectionCode::kUnknownSectionCode) {
      // Skip unknown sections that we do not know how to handle.
      return true;
    }
    // Remove the unknown section tag from the payload bytes.
    offset += decoder.position();
    bytes = bytes.SubVector(decoder.position(), bytes.size());
  }
  constexpr bool verify_functions = false;
  decoder_.DecodeSection(section_code, bytes, offset, verify_functions);
  if (!decoder_.ok()) {
    FinishAsyncCompileJobWithError(decoder_.FinishDecoding(false));
    return false;
  }
  return true;
}

// Start the code section.
bool AsyncStreamingProcessor::ProcessCodeSectionHeader(size_t functions_count,
                                                       uint32_t offset) {
  TRACE_STREAMING("Start the code section with %zu functions...\n",
                  functions_count);
  if (!decoder_.CheckFunctionsCount(static_cast<uint32_t>(functions_count),
                                    offset)) {
    FinishAsyncCompileJobWithError(decoder_.FinishDecoding(false));
    return false;
  }
  job_->NextStep<AsyncCompileJob::PrepareAndStartCompile>(decoder_.module(),
                                                          false);
  // Execute the PrepareAndStartCompile step immediately and not in a separate
  // task. The step expects to be run on a separate foreground thread though, so
  // we to increment {num_pending_foreground_tasks_} to look like one.
  ++job_->num_pending_foreground_tasks_;
  DCHECK_EQ(1, job_->num_pending_foreground_tasks_);
  constexpr bool on_foreground = true;
  job_->step_->Run(on_foreground);

  NativeModule* native_module = job_->compiled_module_->GetNativeModule();
  native_module->compilation_state()->SetNumberOfFunctionsToCompile(
      functions_count);

  // Set outstanding_finishers_ to 2, because both the AsyncCompileJob and the
  // AsyncStreamingProcessor have to finish.
  job_->outstanding_finishers_.SetValue(2);
  compilation_unit_builder_.reset(new CompilationUnitBuilder(
      native_module, job_->module_env_.get(), job_->centry_stub_));
  return true;
}

// Process a function body.
bool AsyncStreamingProcessor::ProcessFunctionBody(Vector<const uint8_t> bytes,
                                                  uint32_t offset) {
  TRACE_STREAMING("Process function body %d ...\n", next_function_);

  if (next_function_ >= FLAG_skip_compiling_wasm_funcs) {
    decoder_.DecodeFunctionBody(
        next_function_, static_cast<uint32_t>(bytes.length()), offset, false);

    uint32_t index = next_function_ + decoder_.module()->num_imported_functions;
    const WasmFunction* func = &decoder_.module()->functions[index];
    WasmName name = {nullptr, 0};
    compilation_unit_builder_->AddUnit(func, offset, bytes, name);
  }
  ++next_function_;
  // This method always succeeds. The return value is necessary to comply with
  // the StreamingProcessor interface.
  return true;
}

void AsyncStreamingProcessor::CommitCompilationUnits() {
  DCHECK(compilation_unit_builder_);
  compilation_unit_builder_->Commit();
}

void AsyncStreamingProcessor::OnFinishedChunk() {
  TRACE_STREAMING("FinishChunk...\n");
  if (compilation_unit_builder_) CommitCompilationUnits();
}

// Finish the processing of the stream.
void AsyncStreamingProcessor::OnFinishedStream(std::unique_ptr<uint8_t[]> bytes,
                                               size_t length) {
  TRACE_STREAMING("Finish stream...\n");
  job_->bytes_copy_ = std::move(bytes);
  job_->wire_bytes_ = ModuleWireBytes(job_->bytes_copy_.get(),
                                      job_->bytes_copy_.get() + length);
  ModuleResult result = decoder_.FinishDecoding(false);
  DCHECK(result.ok());
  job_->module_ = std::move(result.val);
  if (job_->DecrementAndCheckFinisherCount()) {
    if (job_->compiled_module_.is_null()) {
      // We are processing a WebAssembly module without code section. We need to
      // prepare compilation first before we can finish it.
      // {PrepareAndStartCompile} will call {FinishCompile} by itself if there
      // is no code section.
      job_->DoSync<AsyncCompileJob::PrepareAndStartCompile>(job_->module_.get(),
                                                            true);
    } else {
      job_->DoSync<AsyncCompileJob::FinishCompile>();
    }
  }
}

// Report an error detected in the StreamingDecoder.
void AsyncStreamingProcessor::OnError(DecodeResult result) {
  TRACE_STREAMING("Stream error...\n");
  FinishAsyncCompileJobWithError(std::move(result));
}

void AsyncStreamingProcessor::OnAbort() {
  TRACE_STREAMING("Abort stream...\n");
  job_->Abort();
}

CompilationState::CodeGenerationSchedule::CodeGenerationSchedule(
    base::RandomNumberGenerator* random_number_generator, size_t max_memory)
    : random_number_generator_(random_number_generator),
      max_memory_(max_memory) {
  DCHECK_NOT_NULL(random_number_generator_);
  DCHECK_GT(max_memory_, 0);
}

void CompilationState::CodeGenerationSchedule::Schedule(
    std::unique_ptr<compiler::WasmCompilationUnit>&& item) {
  size_t cost = item->memory_cost();
  schedule_.push_back(std::move(item));
  allocated_memory_.Increment(cost);
}

bool CompilationState::CodeGenerationSchedule::CanAcceptWork() const {
  return !throttle_ || allocated_memory_.Value() <= max_memory_;
}

bool CompilationState::CodeGenerationSchedule::ShouldIncreaseWorkload() const {
  // Half the memory is unused again, we can increase the workload again.
  return !throttle_ || allocated_memory_.Value() <= max_memory_ / 2;
}

std::unique_ptr<compiler::WasmCompilationUnit>
CompilationState::CodeGenerationSchedule::GetNext() {
  DCHECK(!IsEmpty());
  size_t index = GetRandomIndexInSchedule();
  auto ret = std::move(schedule_[index]);
  std::swap(schedule_[schedule_.size() - 1], schedule_[index]);
  schedule_.pop_back();
  allocated_memory_.Decrement(ret->memory_cost());
  return ret;
}

size_t CompilationState::CodeGenerationSchedule::GetRandomIndexInSchedule() {
  double factor = random_number_generator_->NextDouble();
  size_t index = (size_t)(factor * schedule_.size());
  DCHECK_GE(index, 0);
  DCHECK_LT(index, schedule_.size());
  return index;
}

void CompilationStateDeleter::operator()(
    CompilationState* compilation_state) const {
  delete compilation_state;
}

std::unique_ptr<CompilationState, CompilationStateDeleter> NewCompilationState(
    Isolate* isolate) {
  return std::unique_ptr<CompilationState, CompilationStateDeleter>(
      new CompilationState(isolate));
}

void SetCompiledModule(CompilationState* compilation_state,
                       Handle<WasmCompiledModule> compiled_module) {
  compilation_state->SetCompiledModule(compiled_module);
}

CompilationState::CompilationState(internal::Isolate* isolate)
    : isolate_(isolate),
      executed_units_(isolate->random_number_generator(),
                      GetMaxUsableMemorySize(isolate) / 2) {
  v8::Isolate* v8_isolate = reinterpret_cast<v8::Isolate*>(isolate_);
  v8::Platform* platform = V8::GetCurrentPlatform();
  foreground_task_runner_ = platform->GetForegroundTaskRunner(v8_isolate);
  background_task_runner_ = platform->GetWorkerThreadsTaskRunner(v8_isolate);

  // Register task manager for clean shutdown in case of an isolate shutdown.
  isolate_->wasm_engine()->Register(&background_task_manager_);
}

CompilationState::~CompilationState() {
  CancelAndWait();
  foreground_task_manager_.CancelAndWait();

  if (compiled_module_ != nullptr) {
    isolate_->global_handles()->Destroy(
        reinterpret_cast<Object**>(compiled_module_));
    compiled_module_ = nullptr;
  }
}

void CompilationState::SetNumberOfFunctionsToCompile(size_t num_functions) {
  if (!failed_) {
    outstanding_units_ = num_functions;
  }
}

void CompilationState::AddCallback(
    std::function<void(CompilationEvent, Handle<Object>)> callback) {
  callbacks_.push_back(callback);
}

void CompilationState::AddCompilationUnits(
    std::vector<std::unique_ptr<compiler::WasmCompilationUnit>>& units) {
  {
    base::LockGuard<base::Mutex> guard(&compilation_units_mutex_);
    compilation_units_.insert(compilation_units_.end(),
                              std::make_move_iterator(units.begin()),
                              std::make_move_iterator(units.end()));
  }
  StartCompilation(units.size());
}

std::unique_ptr<compiler::WasmCompilationUnit>
CompilationState::GetNextCompilationUnit() {
  base::LockGuard<base::Mutex> guard(&compilation_units_mutex_);
  if (!compilation_units_.empty()) {
    std::unique_ptr<compiler::WasmCompilationUnit> unit =
        std::move(compilation_units_.back());
    compilation_units_.pop_back();
    return unit;
  }

  return std::unique_ptr<compiler::WasmCompilationUnit>();
}

std::unique_ptr<compiler::WasmCompilationUnit>
CompilationState::GetNextExecutedUnit() {
  base::LockGuard<base::Mutex> guard(&result_mutex_);
  if (!executed_units_.IsEmpty()) {
    return executed_units_.GetNext();
  }

  return std::unique_ptr<compiler::WasmCompilationUnit>();
}

bool CompilationState::HasCompilationUnitToFinish() {
  base::LockGuard<base::Mutex> guard(&result_mutex_);
  return !executed_units_.IsEmpty();
}

void CompilationState::OnError(Handle<Object> error,
                               NotifyCompilationCallback notify) {
  failed_ = true;
  CancelAndWait();
  if (notify == NotifyCompilationCallback::kNotify) {
    NotifyOnEvent(CompilationEvent::kFailedCompilation, error);
  }
}

void CompilationState::OnFinishedUnit(NotifyCompilationCallback notify) {
  DCHECK_GT(outstanding_units_, 0);
  --outstanding_units_;

  if (outstanding_units_ == 0) {
    CancelAndWait();
    if (notify == NotifyCompilationCallback::kNotify) {
      NotifyOnEvent(CompilationEvent::kFinishedBaselineCompilation,
                    Handle<Object>::null());
    }
  }
}

void CompilationState::ScheduleUnitForFinishing(
    std::unique_ptr<compiler::WasmCompilationUnit>& unit) {
  base::LockGuard<base::Mutex> guard(&result_mutex_);
  executed_units_.Schedule(std::move(unit));

  if (!finisher_is_running_ && !failed_) {
    ScheduleFinisherTask();
    // We set the flag here so that not more than one finisher is started.
    finisher_is_running_ = true;
  }
}

void CompilationState::CancelAndWait() {
  background_task_manager_.CancelAndWait();
  isolate_->wasm_engine()->Unregister(&background_task_manager_);
}

void CompilationState::OnBackgroundTaskStopped() {
  base::LockGuard<base::Mutex> guard(&tasks_mutex_);
  ++stopped_compilation_tasks_;
  DCHECK_LE(stopped_compilation_tasks_, num_background_tasks_);
}

void CompilationState::RestartBackgroundTasks() {
  base::LockGuard<base::Mutex> guard(&tasks_mutex_);
  // TODO(wasm): Do not start more background tasks than the number of available
  // units in {compilation_units_}.
  for (; stopped_compilation_tasks_ > 0; --stopped_compilation_tasks_) {
    background_task_runner_->PostTask(base::make_unique<BackgroundCompileTask>(
        this, &background_task_manager_));
  }
}

bool CompilationState::SetFinisherIsRunning(bool value) {
  base::LockGuard<base::Mutex> guard(&result_mutex_);
  if (finisher_is_running_ == value) return false;
  finisher_is_running_ = value;
  return true;
}

void CompilationState::ScheduleFinisherTask() {
  foreground_task_runner_->PostTask(
      base::make_unique<FinishCompileTask>(this, &foreground_task_manager_));
}

void CompilationState::Abort() {
  CancelAndWait();
  failed_ = true;
}

void CompilationState::StartCompilation(size_t num_functions) {
  if (num_background_tasks_ == 0) {
    DCHECK_EQ(stopped_compilation_tasks_, num_background_tasks_);

    // {outstanding_units_} has to be initialized before entering.
    DCHECK_LE(num_functions, outstanding_units_);

    // First set up. Initialize {num_background_tasks_} and
    // {stopped_compilation_tasks_}
    num_background_tasks_ =
        Max(1, Min(static_cast<int>(outstanding_units_),
                   Min(FLAG_wasm_num_compilation_tasks,
                       V8::GetCurrentPlatform()->NumberOfWorkerThreads())));
    {
      base::LockGuard<base::Mutex> guard(&tasks_mutex_);
      stopped_compilation_tasks_ = num_background_tasks_;
    }
  }

  RestartBackgroundTasks();
}

void CompilationState::NotifyOnEvent(CompilationEvent event,
                                     Handle<Object> error) {
  for (auto& callback_function : callbacks_) {
    callback_function(event, error);
  }
}

void CompileJsToWasmWrappers(Isolate* isolate,
                             Handle<WasmCompiledModule> compiled_module,
                             Counters* counters) {
  JSToWasmWrapperCache js_to_wasm_cache;
  int wrapper_index = 0;
  Handle<FixedArray> export_wrappers(compiled_module->export_wrappers(),
                                     isolate);
  NativeModule* native_module = compiled_module->GetNativeModule();
  for (auto exp : compiled_module->shared()->module()->export_table) {
    if (exp.kind != kExternalFunction) continue;
    wasm::WasmCode* wasm_code =
        EnsureExportedLazyDeoptData(isolate, native_module, exp.index);
    Handle<Code> wrapper_code = js_to_wasm_cache.CloneOrCompileJSToWasmWrapper(
        isolate, compiled_module->shared()->module(), wasm_code, exp.index,
        compiled_module->use_trap_handler());
    export_wrappers->set(wrapper_index, *wrapper_code);
    RecordStats(*wrapper_code, counters);
    ++wrapper_index;
  }
}

Handle<Script> CreateWasmScript(Isolate* isolate,
                                const ModuleWireBytes& wire_bytes) {
  Handle<Script> script =
      isolate->factory()->NewScript(isolate->factory()->empty_string());
  script->set_context_data(isolate->native_context()->debug_context_id());
  script->set_type(Script::TYPE_WASM);

  int hash = StringHasher::HashSequentialString(
      reinterpret_cast<const char*>(wire_bytes.start()),
      static_cast<int>(wire_bytes.length()), kZeroHashSeed);

  const int kBufferSize = 32;
  char buffer[kBufferSize];
  int url_chars = SNPrintF(ArrayVector(buffer), "wasm://wasm/%08x", hash);
  DCHECK(url_chars >= 0 && url_chars < kBufferSize);
  MaybeHandle<String> url_str = isolate->factory()->NewStringFromOneByte(
      Vector<const uint8_t>(reinterpret_cast<uint8_t*>(buffer), url_chars),
      TENURED);
  script->set_source_url(*url_str.ToHandleChecked());

  int name_chars = SNPrintF(ArrayVector(buffer), "wasm-%08x", hash);
  DCHECK(name_chars >= 0 && name_chars < kBufferSize);
  MaybeHandle<String> name_str = isolate->factory()->NewStringFromOneByte(
      Vector<const uint8_t>(reinterpret_cast<uint8_t*>(buffer), name_chars),
      TENURED);
  script->set_name(*name_str.ToHandleChecked());

  return script;
}

}  // namespace wasm
}  // namespace internal
}  // namespace v8

#undef WasmPatchWasmToWasmWrapper
#undef TRACE
#undef TRACE_CHAIN
#undef TRACE_COMPILE
#undef TRACE_STREAMING
#undef TRACE_LAZY

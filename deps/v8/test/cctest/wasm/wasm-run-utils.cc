// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test/cctest/wasm/wasm-run-utils.h"

#include "src/api.h"
#include "src/assembler-inl.h"
#include "src/wasm/wasm-memory.h"
#include "src/wasm/wasm-objects-inl.h"

namespace v8 {
namespace internal {
namespace wasm {

TestingModuleBuilder::TestingModuleBuilder(
    Zone* zone, WasmExecutionMode mode,
    compiler::RuntimeExceptionSupport exception_support, LowerSimd lower_simd)
    : test_module_ptr_(&test_module_),
      isolate_(CcTest::InitIsolateOnce()),
      global_offset(0),
      mem_start_(nullptr),
      mem_size_(0),
      interpreter_(nullptr),
      execution_mode_(mode),
      runtime_exception_support_(exception_support),
      lower_simd_(lower_simd) {
  WasmJs::Install(isolate_, true);
  test_module_.globals_size = kMaxGlobalsSize;
  memset(globals_data_, 0, sizeof(globals_data_));
  instance_object_ = InitInstanceObject();
  if (mode == kExecuteInterpreter) {
    interpreter_ = WasmDebugInfo::SetupForTesting(instance_object_);
  }
}

byte* TestingModuleBuilder::AddMemory(uint32_t size) {
  CHECK(!test_module_.has_memory);
  CHECK_NULL(mem_start_);
  CHECK_EQ(0, mem_size_);
  DCHECK(!instance_object_->has_memory_object());
  test_module_.has_memory = true;
  const bool enable_guard_regions =
      trap_handler::IsTrapHandlerEnabled() && test_module_.is_wasm();
  uint32_t alloc_size =
      enable_guard_regions ? RoundUp(size, CommitPageSize()) : size;
  Handle<JSArrayBuffer> new_buffer =
      wasm::NewArrayBuffer(isolate_, alloc_size, enable_guard_regions);
  CHECK(!new_buffer.is_null());
  mem_start_ = reinterpret_cast<byte*>(new_buffer->backing_store());
  mem_size_ = size;
  CHECK(size == 0 || mem_start_);
  memset(mem_start_, 0, size);

  // Create the WasmMemoryObject.
  Handle<WasmMemoryObject> memory_object = WasmMemoryObject::New(
      isolate_, new_buffer,
      (test_module_.maximum_pages != 0) ? test_module_.maximum_pages : -1);
  instance_object_->set_memory_object(*memory_object);
  WasmMemoryObject::AddInstance(isolate_, memory_object, instance_object_);
  // TODO(wasm): Delete the following two lines when test-run-wasm will use a
  // multiple of kPageSize as memory size. At the moment, the effect of these
  // two lines is used to shrink the memory for testing purposes.
  instance_object_->wasm_context()->get()->SetRawMemory(mem_start_, mem_size_);
  return mem_start_;
}

uint32_t TestingModuleBuilder::AddFunction(FunctionSig* sig, const char* name) {
  if (test_module_.functions.size() == 0) {
    // TODO(titzer): Reserving space here to avoid the underlying WasmFunction
    // structs from moving.
    test_module_.functions.reserve(kMaxFunctions);
  }
  uint32_t index = static_cast<uint32_t>(test_module_.functions.size());
  native_module_->ResizeCodeTableForTest(index);
  test_module_.functions.push_back({sig, index, 0, {0, 0}, false, false});
  if (name) {
    Vector<const byte> name_vec = Vector<const byte>::cast(CStrVector(name));
    test_module_.AddNameForTesting(
        index, {AddBytes(name_vec), static_cast<uint32_t>(name_vec.length())});
  }
  if (interpreter_) {
    interpreter_->AddFunctionForTesting(&test_module_.functions.back());
  }
  DCHECK_LT(index, kMaxFunctions);  // limited for testing.
  return index;
}

uint32_t TestingModuleBuilder::AddJsFunction(
    FunctionSig* sig, const char* source, Handle<FixedArray> js_imports_table) {
  Handle<JSFunction> jsfunc = Handle<JSFunction>::cast(v8::Utils::OpenHandle(
      *v8::Local<v8::Function>::Cast(CompileRun(source))));
  uint32_t index = AddFunction(sig, nullptr);
  js_imports_table->set(0, *isolate_->native_context());
  // TODO(6792): No longer needed once WebAssembly code is off heap.
  CodeSpaceMemoryModificationScope modification_scope(isolate_->heap());
  Handle<Code> code = compiler::CompileWasmToJSWrapper(
      isolate_, jsfunc, sig, index, test_module_.origin(),
      trap_handler::IsTrapHandlerEnabled(), js_imports_table);
  native_module_->ResizeCodeTableForTest(index);
  native_module_->AddCodeCopy(code, wasm::WasmCode::kWasmToJsWrapper, index);
  return index;
}

Handle<JSFunction> TestingModuleBuilder::WrapCode(uint32_t index) {
  // Wrap the code so it can be called as a JS function.
  Link();
  wasm::WasmCode* code = native_module_->GetCode(index);
  byte* context_address =
      test_module_.has_memory
          ? reinterpret_cast<byte*>(instance_object_->wasm_context()->get())
          : nullptr;
  Handle<Code> ret_code = compiler::CompileJSToWasmWrapper(
      isolate_, &test_module_, code, index, context_address,
      trap_handler::IsTrapHandlerEnabled());
  Handle<JSFunction> ret = WasmExportedFunction::New(
      isolate_, instance_object(), MaybeHandle<String>(),
      static_cast<int>(index),
      static_cast<int>(test_module_.functions[index].sig->parameter_count()),
      ret_code);

  // Add weak reference to exported functions.
  Handle<WasmCompiledModule> compiled_module(
      instance_object()->compiled_module(), isolate_);
  Handle<FixedArray> old_arr(compiled_module->weak_exported_functions(),
                             isolate_);
  Handle<FixedArray> new_arr =
      isolate_->factory()->NewFixedArray(old_arr->length() + 1);
  old_arr->CopyTo(0, *new_arr, 0, old_arr->length());
  Handle<WeakCell> weak_fn = isolate_->factory()->NewWeakCell(ret);
  new_arr->set(old_arr->length(), *weak_fn);
  compiled_module->set_weak_exported_functions(*new_arr);

  return ret;
}

void TestingModuleBuilder::AddIndirectFunctionTable(
    const uint16_t* function_indexes, uint32_t table_size) {
  test_module_.function_tables.emplace_back();
  WasmIndirectFunctionTable& table = test_module_.function_tables.back();
  table.initial_size = table_size;
  table.maximum_size = table_size;
  table.has_maximum_size = true;
  for (uint32_t i = 0; i < table_size; ++i) {
    table.values.push_back(function_indexes[i]);
  }

  FixedArray* func_table = *isolate_->factory()->NewFixedArray(
      table_size * compiler::kFunctionTableEntrySize);
  function_tables_.push_back(
      isolate_->global_handles()->Create(func_table).address());

  WasmContext* wasm_context = instance_object()->wasm_context()->get();
  wasm_context->table = reinterpret_cast<IndirectFunctionTableEntry*>(
      calloc(table_size, sizeof(IndirectFunctionTableEntry)));
  wasm_context->table_size = table_size;
  for (uint32_t i = 0; i < table_size; i++) {
    wasm_context->table[i].sig_id = -1;
  }
}

void TestingModuleBuilder::PopulateIndirectFunctionTable() {
  if (interpret()) return;
  // Initialize the fixed arrays in instance->function_tables.
  WasmContext* wasm_context = instance_object()->wasm_context()->get();
  for (uint32_t i = 0; i < function_tables_.size(); i++) {
    WasmIndirectFunctionTable& table = test_module_.function_tables[i];
    Handle<FixedArray> function_table(
        reinterpret_cast<FixedArray**>(function_tables_[i]));
    int table_size = static_cast<int>(table.values.size());
    for (int j = 0; j < table_size; j++) {
      WasmFunction& function = test_module_.functions[table.values[j]];
      int sig_id = test_module_.signature_map.Find(function.sig);
      function_table->set(compiler::FunctionTableSigOffset(j),
                          Smi::FromInt(sig_id));
      auto start =
          native_module_->GetCode(function.func_index)->instructions().start();
      wasm_context->table[j].context = wasm_context;
      wasm_context->table[j].sig_id = sig_id;
      wasm_context->table[j].target = start;
    }
  }
}

uint32_t TestingModuleBuilder::AddBytes(Vector<const byte> bytes) {
  Handle<WasmSharedModuleData> shared(
      instance_object_->compiled_module()->shared(), isolate_);
  Handle<SeqOneByteString> old_bytes(shared->module_bytes(), isolate_);
  uint32_t old_size = static_cast<uint32_t>(old_bytes->length());
  // Avoid placing strings at offset 0, this might be interpreted as "not
  // set", e.g. for function names.
  uint32_t bytes_offset = old_size ? old_size : 1;
  ScopedVector<byte> new_bytes(bytes_offset + bytes.length());
  memcpy(new_bytes.start(), old_bytes->GetChars(), old_size);
  memcpy(new_bytes.start() + bytes_offset, bytes.start(), bytes.length());
  Handle<SeqOneByteString> new_bytes_str = Handle<SeqOneByteString>::cast(
      isolate_->factory()->NewStringFromOneByte(new_bytes).ToHandleChecked());
  shared->set_module_bytes(*new_bytes_str);
  return bytes_offset;
}

compiler::ModuleEnv TestingModuleBuilder::CreateModuleEnv() {
  return {&test_module_, function_tables_,
          trap_handler::IsTrapHandlerEnabled()};
}

const WasmGlobal* TestingModuleBuilder::AddGlobal(ValueType type) {
  byte size = WasmOpcodes::MemSize(WasmOpcodes::MachineTypeFor(type));
  global_offset = (global_offset + size - 1) & ~(size - 1);  // align
  test_module_.globals.push_back(
      {type, true, WasmInitExpr(), global_offset, false, false});
  global_offset += size;
  // limit number of globals.
  CHECK_LT(global_offset, kMaxGlobalsSize);
  return &test_module_.globals.back();
}

Handle<WasmInstanceObject> TestingModuleBuilder::InitInstanceObject() {
  Handle<SeqOneByteString> empty_string = Handle<SeqOneByteString>::cast(
      isolate_->factory()->NewStringFromOneByte({}).ToHandleChecked());
  // The lifetime of the wasm module is tied to this object's, and we cannot
  // rely on the mechanics of Managed<T>.
  Handle<Foreign> module_wrapper = isolate_->factory()->NewForeign(
      reinterpret_cast<Address>(&test_module_ptr_));
  Handle<Script> script =
      isolate_->factory()->NewScript(isolate_->factory()->empty_string());
  script->set_type(Script::TYPE_WASM);
  Handle<WasmSharedModuleData> shared_module_data =
      WasmSharedModuleData::New(isolate_, module_wrapper, empty_string, script,
                                Handle<ByteArray>::null());
  Handle<FixedArray> export_wrappers = isolate_->factory()->NewFixedArray(0);
  Handle<WasmCompiledModule> compiled_module = WasmCompiledModule::New(
      isolate_, test_module_ptr_, export_wrappers, function_tables_,
      trap_handler::IsTrapHandlerEnabled());
  compiled_module->set_shared(*shared_module_data);
  // This method is called when we initialize TestEnvironment. We don't
  // have a memory yet, so we won't create it here. We'll update the
  // interpreter when we get a memory. We do have globals, though.
  native_module_ = compiled_module->GetNativeModule();

  Handle<FixedArray> weak_exported = isolate_->factory()->NewFixedArray(0);
  compiled_module->set_weak_exported_functions(*weak_exported);
  DCHECK(compiled_module->IsWasmCompiledModule());
  script->set_wasm_compiled_module(*compiled_module);
  auto instance = WasmInstanceObject::New(isolate_, compiled_module);
  instance->wasm_context()->get()->globals_start = globals_data_;
  Handle<WeakCell> weak_instance = isolate()->factory()->NewWeakCell(instance);
  compiled_module->set_weak_owning_instance(*weak_instance);
  return instance;
}

void TestBuildingGraphWithBuilder(compiler::WasmGraphBuilder* builder,
                                  Zone* zone, FunctionSig* sig,
                                  const byte* start, const byte* end) {
  DecodeResult result =
      BuildTFGraph(zone->allocator(), builder, sig, start, end);
  if (result.failed()) {
#ifdef DEBUG
    if (!FLAG_trace_wasm_decoder) {
      // Retry the compilation with the tracing flag on, to help in debugging.
      FLAG_trace_wasm_decoder = true;
      result = BuildTFGraph(zone->allocator(), builder, sig, start, end);
    }
#endif

    uint32_t pc = result.error_offset();
    FATAL("Verification failed; pc = +%x, msg = %s", pc,
          result.error_msg().c_str());
  }
  builder->LowerInt64();
  if (!CpuFeatures::SupportsWasmSimd128()) {
    builder->SimdScalarLoweringForTesting();
  }
}

void TestBuildingGraph(
    Zone* zone, compiler::JSGraph* jsgraph, compiler::ModuleEnv* module,
    FunctionSig* sig, compiler::SourcePositionTable* source_position_table,
    const byte* start, const byte* end,
    compiler::RuntimeExceptionSupport runtime_exception_support) {
  if (module) {
    compiler::WasmGraphBuilder builder(
        module, zone, jsgraph, CEntryStub(jsgraph->isolate(), 1).GetCode(), sig,
        source_position_table, runtime_exception_support);
    TestBuildingGraphWithBuilder(&builder, zone, sig, start, end);
  } else {
    compiler::WasmGraphBuilder builder(
        nullptr, zone, jsgraph, CEntryStub(jsgraph->isolate(), 1).GetCode(),
        sig, source_position_table, runtime_exception_support);
    TestBuildingGraphWithBuilder(&builder, zone, sig, start, end);
  }
}

WasmFunctionWrapper::WasmFunctionWrapper(Zone* zone, int num_params)
    : GraphAndBuilders(zone),
      inner_code_node_(nullptr),
      context_address_(nullptr),
      signature_(nullptr) {
  // One additional parameter for the pointer to the return value memory.
  Signature<MachineType>::Builder sig_builder(zone, 1, num_params + 1);

  sig_builder.AddReturn(MachineType::Int32());
  for (int i = 0; i < num_params + 1; i++) {
    sig_builder.AddParam(MachineType::Pointer());
  }
  signature_ = sig_builder.Build();
}

void WasmFunctionWrapper::Init(CallDescriptor* call_descriptor,
                               MachineType return_type,
                               Vector<MachineType> param_types) {
  DCHECK_NOT_NULL(call_descriptor);
  DCHECK_EQ(signature_->parameter_count(), param_types.length() + 1);

  // Create the TF graph for the wrapper.

  // Function, context_address, effect, and control.
  Node** parameters = zone()->NewArray<Node*>(param_types.length() + 4);
  graph()->SetStart(graph()->NewNode(common()->Start(7)));
  Node* effect = graph()->start();
  int parameter_count = 0;

  // Dummy node which gets replaced in SetInnerCode.
  inner_code_node_ = graph()->NewNode(common()->Int32Constant(0));
  parameters[parameter_count++] = inner_code_node_;

  // Dummy node that gets replaced in SetContextAddress.
  context_address_ = graph()->NewNode(IntPtrConstant(0));
  parameters[parameter_count++] = context_address_;

  int param_idx = 0;
  for (MachineType t : param_types) {
    DCHECK_NE(MachineType::None(), t);
    parameters[parameter_count] = graph()->NewNode(
        machine()->Load(t),
        graph()->NewNode(common()->Parameter(param_idx++), graph()->start()),
        graph()->NewNode(common()->Int32Constant(0)), effect, graph()->start());
    effect = parameters[parameter_count++];
  }

  parameters[parameter_count++] = effect;
  parameters[parameter_count++] = graph()->start();
  Node* call = graph()->NewNode(common()->Call(call_descriptor),
                                parameter_count, parameters);

  if (!return_type.IsNone()) {
    effect = graph()->NewNode(
        machine()->Store(compiler::StoreRepresentation(
            return_type.representation(), WriteBarrierKind::kNoWriteBarrier)),
        graph()->NewNode(common()->Parameter(param_types.length()),
                         graph()->start()),
        graph()->NewNode(common()->Int32Constant(0)), call, effect,
        graph()->start());
  }
  Node* zero = graph()->NewNode(common()->Int32Constant(0));
  Node* r = graph()->NewNode(
      common()->Return(), zero,
      graph()->NewNode(common()->Int32Constant(WASM_WRAPPER_RETURN_VALUE)),
      effect, graph()->start());
  graph()->SetEnd(graph()->NewNode(common()->End(1), r));
}

Handle<Code> WasmFunctionWrapper::GetWrapperCode() {
  if (code_.is_null()) {
    Isolate* isolate = CcTest::InitIsolateOnce();

    auto call_descriptor =
        compiler::Linkage::GetSimplifiedCDescriptor(zone(), signature_, true);

    if (kPointerSize == 4) {
      size_t num_params = signature_->parameter_count();
      // One additional parameter for the pointer of the return value.
      Signature<MachineRepresentation>::Builder rep_builder(zone(), 1,
                                                            num_params + 1);

      rep_builder.AddReturn(MachineRepresentation::kWord32);
      for (size_t i = 0; i < num_params + 1; i++) {
        rep_builder.AddParam(MachineRepresentation::kWord32);
      }
      compiler::Int64Lowering r(graph(), machine(), common(), zone(),
                                rep_builder.Build());
      r.LowerGraph();
    }

    CompilationInfo info(ArrayVector("testing"), graph()->zone(),
                         Code::C_WASM_ENTRY);
    code_ = compiler::Pipeline::GenerateCodeForTesting(
        &info, isolate, call_descriptor, graph(), nullptr);
    CHECK(!code_.is_null());
#ifdef ENABLE_DISASSEMBLER
    if (FLAG_print_opt_code) {
      CodeTracer::Scope tracing_scope(isolate->GetCodeTracer());
      OFStream os(tracing_scope.file());

      code_->Disassemble("wasm wrapper", os);
    }
#endif
  }

  return code_;
}

void WasmFunctionCompiler::Build(const byte* start, const byte* end) {
  size_t locals_size = local_decls.Size();
  size_t total_size = end - start + locals_size + 1;
  byte* buffer = static_cast<byte*>(zone()->New(total_size));
  // Prepend the local decls to the code.
  local_decls.Emit(buffer);
  // Emit the code.
  memcpy(buffer + locals_size, start, end - start);
  // Append an extra end opcode.
  buffer[total_size - 1] = kExprEnd;

  start = buffer;
  end = buffer + total_size;

  CHECK_GE(kMaxInt, end - start);
  int len = static_cast<int>(end - start);
  function_->code = {builder_->AddBytes(Vector<const byte>(start, len)),
                     static_cast<uint32_t>(len)};

  if (interpreter_) {
    // Add the code to the interpreter.
    interpreter_->SetFunctionCodeForTesting(function_, start, end);
  }

  Handle<WasmCompiledModule> compiled_module(
      builder_->instance_object()->compiled_module(), isolate());
  NativeModule* native_module = compiled_module->GetNativeModule();
  native_module->ResizeCodeTableForTest(function_->func_index);
  Handle<SeqOneByteString> wire_bytes(compiled_module->shared()->module_bytes(),
                                      isolate());

  compiler::ModuleEnv module_env = builder_->CreateModuleEnv();
  ErrorThrower thrower(isolate(), "WasmFunctionCompiler::Build");
  ScopedVector<uint8_t> func_wire_bytes(function_->code.length());
  memcpy(func_wire_bytes.start(),
         wire_bytes->GetChars() + function_->code.offset(),
         func_wire_bytes.length());
  WireBytesRef func_name_ref =
      module_env.module->LookupName(*wire_bytes, function_->func_index);
  ScopedVector<char> func_name(func_name_ref.length());
  memcpy(func_name.start(), wire_bytes->GetChars() + func_name_ref.offset(),
         func_name_ref.length());

  FunctionBody func_body{function_->sig, function_->code.offset(),
                         func_wire_bytes.start(), func_wire_bytes.end()};
  compiler::WasmCompilationUnit::CompilationMode comp_mode =
      builder_->execution_mode() == WasmExecutionMode::kExecuteLiftoff
          ? compiler::WasmCompilationUnit::CompilationMode::kLiftoff
          : compiler::WasmCompilationUnit::CompilationMode::kTurbofan;
  compiler::WasmCompilationUnit unit(
      isolate(), &module_env, native_module, func_body, func_name,
      function_->func_index, CEntryStub(isolate(), 1).GetCode(), comp_mode,
      isolate()->counters(), builder_->runtime_exception_support(),
      builder_->lower_simd());
  unit.ExecuteCompilation();
  wasm::WasmCode* wasm_code = unit.FinishCompilation(&thrower);
  if (wasm::WasmCode::ShouldBeLogged(isolate())) {
    wasm_code->LogCode(isolate());
  }
  CHECK(!thrower.error());
  if (trap_handler::IsTrapHandlerEnabled()) {
    UnpackAndRegisterProtectedInstructions(isolate(), native_module);
  }
}

WasmFunctionCompiler::WasmFunctionCompiler(Zone* zone, FunctionSig* sig,
                                           TestingModuleBuilder* builder,
                                           const char* name)
    : GraphAndBuilders(zone),
      jsgraph(builder->isolate(), this->graph(), this->common(), nullptr,
              nullptr, this->machine()),
      sig(sig),
      descriptor_(nullptr),
      builder_(builder),
      local_decls(zone, sig),
      source_position_table_(this->graph()),
      interpreter_(builder->interpreter()) {
  // Get a new function from the testing module.
  int index = builder->AddFunction(sig, name);
  function_ = builder_->GetFunctionAt(index);
}

WasmFunctionCompiler::~WasmFunctionCompiler() {}

FunctionSig* WasmRunnerBase::CreateSig(MachineType return_type,
                                       Vector<MachineType> param_types) {
  int return_count = return_type.IsNone() ? 0 : 1;
  int param_count = param_types.length();

  // Allocate storage array in zone.
  ValueType* sig_types = zone_.NewArray<ValueType>(return_count + param_count);

  // Convert machine types to local types, and check that there are no
  // MachineType::None()'s in the parameters.
  int idx = 0;
  if (return_count) sig_types[idx++] = WasmOpcodes::ValueTypeFor(return_type);
  for (MachineType param : param_types) {
    CHECK_NE(MachineType::None(), param);
    sig_types[idx++] = WasmOpcodes::ValueTypeFor(param);
  }
  return new (&zone_) FunctionSig(return_count, param_count, sig_types);
}

// static
bool WasmRunnerBase::trap_happened;

}  // namespace wasm
}  // namespace internal
}  // namespace v8

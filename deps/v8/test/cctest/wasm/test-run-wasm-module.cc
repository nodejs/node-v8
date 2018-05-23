// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdlib.h>
#include <string.h>

#include "src/api.h"
#include "src/objects-inl.h"
#include "src/snapshot/code-serializer.h"
#include "src/version.h"
#include "src/wasm/module-decoder.h"
#include "src/wasm/wasm-engine.h"
#include "src/wasm/wasm-memory.h"
#include "src/wasm/wasm-module-builder.h"
#include "src/wasm/wasm-module.h"
#include "src/wasm/wasm-objects-inl.h"
#include "src/wasm/wasm-opcodes.h"

#include "test/cctest/cctest.h"
#include "test/common/wasm/flag-utils.h"
#include "test/common/wasm/test-signatures.h"
#include "test/common/wasm/wasm-macro-gen.h"
#include "test/common/wasm/wasm-module-runner.h"

namespace v8 {
namespace internal {
namespace wasm {

using testing::CompileAndInstantiateForTesting;

namespace {
void Cleanup(Isolate* isolate = nullptr) {
  // By sending a low memory notifications, we will try hard to collect all
  // garbage and will therefore also invoke all weak callbacks of actually
  // unreachable persistent handles.
  if (!isolate) {
    isolate = CcTest::InitIsolateOnce();
  }
  reinterpret_cast<v8::Isolate*>(isolate)->LowMemoryNotification();
}

void TestModule(Zone* zone, WasmModuleBuilder* builder,
                int32_t expected_result) {
  ZoneBuffer buffer(zone);
  builder->WriteTo(buffer);

  Isolate* isolate = CcTest::InitIsolateOnce();
  HandleScope scope(isolate);
  testing::SetupIsolateForWasmModule(isolate);
  int32_t result =
      testing::CompileAndRunWasmModule(isolate, buffer.begin(), buffer.end());
  CHECK_EQ(expected_result, result);
}

void TestModuleException(Zone* zone, WasmModuleBuilder* builder) {
  ZoneBuffer buffer(zone);
  builder->WriteTo(buffer);

  Isolate* isolate = CcTest::InitIsolateOnce();
  HandleScope scope(isolate);
  testing::SetupIsolateForWasmModule(isolate);
  v8::TryCatch try_catch(reinterpret_cast<v8::Isolate*>(isolate));
  testing::CompileAndRunWasmModule(isolate, buffer.begin(), buffer.end());
  CHECK(try_catch.HasCaught());
  isolate->clear_pending_exception();
}

void ExportAsMain(WasmFunctionBuilder* f) {
  f->builder()->AddExport(CStrVector("main"), f);
}

#define EMIT_CODE_WITH_END(f, code)  \
  do {                               \
    f->EmitCode(code, sizeof(code)); \
    f->Emit(kExprEnd);               \
  } while (false)

}  // namespace

TEST(Run_WasmModule_Return114) {
  {
    static const int32_t kReturnValue = 114;
    TestSignatures sigs;
    v8::internal::AccountingAllocator allocator;
    Zone zone(&allocator, ZONE_NAME);

    WasmModuleBuilder* builder = new (&zone) WasmModuleBuilder(&zone);
    WasmFunctionBuilder* f = builder->AddFunction(sigs.i_v());
    ExportAsMain(f);
    byte code[] = {WASM_I32V_2(kReturnValue)};
    EMIT_CODE_WITH_END(f, code);
    TestModule(&zone, builder, kReturnValue);
  }
  Cleanup();
}

TEST(Run_WasmModule_CallAdd) {
  {
    v8::internal::AccountingAllocator allocator;
    Zone zone(&allocator, ZONE_NAME);
    TestSignatures sigs;

    WasmModuleBuilder* builder = new (&zone) WasmModuleBuilder(&zone);

    WasmFunctionBuilder* f1 = builder->AddFunction(sigs.i_ii());
    uint16_t param1 = 0;
    uint16_t param2 = 1;
    byte code1[] = {
        WASM_I32_ADD(WASM_GET_LOCAL(param1), WASM_GET_LOCAL(param2))};
    EMIT_CODE_WITH_END(f1, code1);

    WasmFunctionBuilder* f2 = builder->AddFunction(sigs.i_v());

    ExportAsMain(f2);
    byte code2[] = {
        WASM_CALL_FUNCTION(f1->func_index(), WASM_I32V_2(77), WASM_I32V_1(22))};
    EMIT_CODE_WITH_END(f2, code2);
    TestModule(&zone, builder, 99);
  }
  Cleanup();
}

TEST(Run_WasmModule_ReadLoadedDataSegment) {
  {
    static const byte kDataSegmentDest0 = 12;
    v8::internal::AccountingAllocator allocator;
    Zone zone(&allocator, ZONE_NAME);
    TestSignatures sigs;

    WasmModuleBuilder* builder = new (&zone) WasmModuleBuilder(&zone);
    WasmFunctionBuilder* f = builder->AddFunction(sigs.i_v());

    ExportAsMain(f);
    byte code[] = {
        WASM_LOAD_MEM(MachineType::Int32(), WASM_I32V_1(kDataSegmentDest0))};
    EMIT_CODE_WITH_END(f, code);
    byte data[] = {0xAA, 0xBB, 0xCC, 0xDD};
    builder->AddDataSegment(data, sizeof(data), kDataSegmentDest0);
    TestModule(&zone, builder, 0xDDCCBBAA);
  }
  Cleanup();
}

TEST(Run_WasmModule_CheckMemoryIsZero) {
  {
    static const int kCheckSize = 16 * 1024;
    v8::internal::AccountingAllocator allocator;
    Zone zone(&allocator, ZONE_NAME);
    TestSignatures sigs;

    WasmModuleBuilder* builder = new (&zone) WasmModuleBuilder(&zone);
    WasmFunctionBuilder* f = builder->AddFunction(sigs.i_v());

    uint16_t localIndex = f->AddLocal(kWasmI32);
    ExportAsMain(f);
    byte code[] = {WASM_BLOCK_I(
        WASM_WHILE(
            WASM_I32_LTS(WASM_GET_LOCAL(localIndex), WASM_I32V_3(kCheckSize)),
            WASM_IF_ELSE(
                WASM_LOAD_MEM(MachineType::Int32(), WASM_GET_LOCAL(localIndex)),
                WASM_BRV(3, WASM_I32V_1(-1)),
                WASM_INC_LOCAL_BY(localIndex, 4))),
        WASM_I32V_1(11))};
    EMIT_CODE_WITH_END(f, code);
    TestModule(&zone, builder, 11);
  }
  Cleanup();
}

TEST(Run_WasmModule_CallMain_recursive) {
  {
    v8::internal::AccountingAllocator allocator;
    Zone zone(&allocator, ZONE_NAME);
    TestSignatures sigs;

    WasmModuleBuilder* builder = new (&zone) WasmModuleBuilder(&zone);
    WasmFunctionBuilder* f = builder->AddFunction(sigs.i_v());

    uint16_t localIndex = f->AddLocal(kWasmI32);
    ExportAsMain(f);
    byte code[] = {
        WASM_SET_LOCAL(localIndex,
                       WASM_LOAD_MEM(MachineType::Int32(), WASM_ZERO)),
        WASM_IF_ELSE_I(WASM_I32_LTS(WASM_GET_LOCAL(localIndex), WASM_I32V_1(5)),
                       WASM_SEQ(WASM_STORE_MEM(MachineType::Int32(), WASM_ZERO,
                                               WASM_INC_LOCAL(localIndex)),
                                WASM_CALL_FUNCTION0(0)),
                       WASM_I32V_1(55))};
    EMIT_CODE_WITH_END(f, code);
    TestModule(&zone, builder, 55);
  }
  Cleanup();
}

TEST(Run_WasmModule_Global) {
  {
    v8::internal::AccountingAllocator allocator;
    Zone zone(&allocator, ZONE_NAME);
    TestSignatures sigs;

    WasmModuleBuilder* builder = new (&zone) WasmModuleBuilder(&zone);
    uint32_t global1 = builder->AddGlobal(kWasmI32, 0);
    uint32_t global2 = builder->AddGlobal(kWasmI32, 0);
    WasmFunctionBuilder* f1 = builder->AddFunction(sigs.i_v());
    byte code1[] = {
        WASM_I32_ADD(WASM_GET_GLOBAL(global1), WASM_GET_GLOBAL(global2))};
    EMIT_CODE_WITH_END(f1, code1);
    WasmFunctionBuilder* f2 = builder->AddFunction(sigs.i_v());
    ExportAsMain(f2);
    byte code2[] = {WASM_SET_GLOBAL(global1, WASM_I32V_1(56)),
                    WASM_SET_GLOBAL(global2, WASM_I32V_1(41)),
                    WASM_RETURN1(WASM_CALL_FUNCTION0(f1->func_index()))};
    EMIT_CODE_WITH_END(f2, code2);
    TestModule(&zone, builder, 97);
  }
  Cleanup();
}

// Approximate gtest TEST_F style, in case we adopt gtest.
class WasmSerializationTest {
 public:
  WasmSerializationTest() : zone_(&allocator_, ZONE_NAME) {
    // Don't call here if we move to gtest.
    SetUp();
  }

  static void BuildWireBytes(Zone* zone, ZoneBuffer* buffer) {
    WasmModuleBuilder* builder = new (zone) WasmModuleBuilder(zone);
    TestSignatures sigs;

    WasmFunctionBuilder* f = builder->AddFunction(sigs.i_i());
    byte code[] = {WASM_GET_LOCAL(0), kExprI32Const, 1, kExprI32Add};
    EMIT_CODE_WITH_END(f, code);
    builder->AddExport(CStrVector(kFunctionName), f);

    builder->WriteTo(*buffer);
  }

  void ClearSerializedData() {
    serialized_bytes_.first = nullptr;
    serialized_bytes_.second = 0;
  }

  void InvalidateVersion() {
    uint32_t* slot = reinterpret_cast<uint32_t*>(
        const_cast<uint8_t*>(serialized_bytes_.first) +
        SerializedCodeData::kVersionHashOffset);
    *slot = Version::Hash() + 1;
  }

  void InvalidateWireBytes() {
    memset(const_cast<uint8_t*>(wire_bytes_.first), '\0',
           wire_bytes_.second / 2);
  }

  void InvalidateLength() {
    uint32_t* slot = reinterpret_cast<uint32_t*>(
        const_cast<uint8_t*>(serialized_bytes_.first) +
        SerializedCodeData::kPayloadLengthOffset);
    *slot = 0u;
  }

  v8::MaybeLocal<v8::WasmCompiledModule> Deserialize() {
    ErrorThrower thrower(current_isolate(), "");
    v8::MaybeLocal<v8::WasmCompiledModule> deserialized =
        v8::WasmCompiledModule::DeserializeOrCompile(
            current_isolate_v8(), serialized_bytes(), wire_bytes());
    return deserialized;
  }

  void DeserializeAndRun() {
    ErrorThrower thrower(current_isolate(), "");
    v8::Local<v8::WasmCompiledModule> deserialized_module;
    CHECK(Deserialize().ToLocal(&deserialized_module));
    Handle<WasmModuleObject> module_object = Handle<WasmModuleObject>::cast(
        v8::Utils::OpenHandle(*deserialized_module));
    {
      DisallowHeapAllocation assume_no_gc;
      CHECK_EQ(
          memcmp(
              reinterpret_cast<const uint8_t*>(
                  module_object->shared()->module_bytes()->GetCharsAddress()),
              wire_bytes().first, wire_bytes().second),
          0);
    }
    Handle<WasmInstanceObject> instance =
        current_isolate()
            ->wasm_engine()
            ->SyncInstantiate(current_isolate(), &thrower, module_object,
                              Handle<JSReceiver>::null(),
                              MaybeHandle<JSArrayBuffer>())
            .ToHandleChecked();
    Handle<Object> params[1] = {
        Handle<Object>(Smi::FromInt(41), current_isolate())};
    int32_t result = testing::CallWasmFunctionForTesting(
        current_isolate(), instance, &thrower, kFunctionName, 1, params);
    CHECK_EQ(42, result);
  }

  Isolate* current_isolate() {
    return reinterpret_cast<Isolate*>(current_isolate_v8_);
  }

  ~WasmSerializationTest() {
    // Don't call from here if we move to gtest
    TearDown();
  }

  v8::Isolate* current_isolate_v8() { return current_isolate_v8_; }

 private:
  static const char* kFunctionName;

  Zone* zone() { return &zone_; }
  const v8::WasmCompiledModule::CallerOwnedBuffer& wire_bytes() const {
    return wire_bytes_;
  }

  const v8::WasmCompiledModule::CallerOwnedBuffer& serialized_bytes() const {
    return serialized_bytes_;
  }

  void SetUp() {
    ZoneBuffer buffer(&zone_);
    WasmSerializationTest::BuildWireBytes(zone(), &buffer);

    Isolate* serialization_isolate = CcTest::InitIsolateOnce();
    ErrorThrower thrower(serialization_isolate, "");
    uint8_t* bytes = nullptr;
    size_t bytes_size = 0;
    {
      HandleScope scope(serialization_isolate);
      testing::SetupIsolateForWasmModule(serialization_isolate);

      MaybeHandle<WasmModuleObject> maybe_module_object =
          serialization_isolate->wasm_engine()->SyncCompile(
              serialization_isolate, &thrower,
              ModuleWireBytes(buffer.begin(), buffer.end()));
      Handle<WasmModuleObject> module_object =
          maybe_module_object.ToHandleChecked();

      Handle<WasmCompiledModule> compiled_module(
          module_object->compiled_module());
      Handle<FixedArray> export_wrappers(module_object->export_wrappers());
      Handle<WasmSharedModuleData> shared(module_object->shared());
      Handle<JSObject> module_obj = WasmModuleObject::New(
          serialization_isolate, compiled_module, export_wrappers, shared);
      v8::Local<v8::Object> v8_module_obj = v8::Utils::ToLocal(module_obj);
      CHECK(v8_module_obj->IsWebAssemblyCompiledModule());

      v8::Local<v8::WasmCompiledModule> v8_compiled_module =
          v8_module_obj.As<v8::WasmCompiledModule>();
      v8::Local<v8::String> uncompiled_bytes =
          v8_compiled_module->GetWasmWireBytes();
      bytes_size = static_cast<size_t>(uncompiled_bytes->Length());
      bytes = zone()->NewArray<uint8_t>(bytes_size);
      uncompiled_bytes->WriteOneByte(bytes, 0, uncompiled_bytes->Length(),
                                     v8::String::NO_NULL_TERMINATION);
      // keep alive data_ until the end
      data_ = v8_compiled_module->Serialize();
    }

    wire_bytes_ = {const_cast<const uint8_t*>(bytes), bytes_size};

    serialized_bytes_ = {data_.first.get(), data_.second};

    v8::Isolate::CreateParams create_params;
    create_params.array_buffer_allocator =
        serialization_isolate->array_buffer_allocator();

    current_isolate_v8_ = v8::Isolate::New(create_params);
    v8::HandleScope new_scope(current_isolate_v8());
    v8::Local<v8::Context> deserialization_context =
        v8::Context::New(current_isolate_v8());
    deserialization_context->Enter();
    testing::SetupIsolateForWasmModule(current_isolate());
  }

  void TearDown() {
    current_isolate_v8()->Dispose();
    current_isolate_v8_ = nullptr;
  }

  v8::internal::AccountingAllocator allocator_;
  Zone zone_;
  v8::WasmCompiledModule::SerializedModule data_;
  v8::WasmCompiledModule::CallerOwnedBuffer wire_bytes_;
  v8::WasmCompiledModule::CallerOwnedBuffer serialized_bytes_;
  v8::Isolate* current_isolate_v8_;
};

const char* WasmSerializationTest::kFunctionName = "increment";

TEST(DeserializeValidModule) {
  WasmSerializationTest test;
  {
    HandleScope scope(test.current_isolate());
    test.DeserializeAndRun();
  }
  Cleanup(test.current_isolate());
  Cleanup();
}

TEST(DeserializeMismatchingVersion) {
  WasmSerializationTest test;
  {
    HandleScope scope(test.current_isolate());
    test.InvalidateVersion();
    test.DeserializeAndRun();
  }
  Cleanup(test.current_isolate());
  Cleanup();
}

TEST(DeserializeNoSerializedData) {
  WasmSerializationTest test;
  {
    HandleScope scope(test.current_isolate());
    test.ClearSerializedData();
    test.DeserializeAndRun();
  }
  Cleanup(test.current_isolate());
  Cleanup();
}

TEST(DeserializeInvalidLength) {
  WasmSerializationTest test;
  {
    HandleScope scope(test.current_isolate());
    test.InvalidateLength();
    test.DeserializeAndRun();
  }
  Cleanup(test.current_isolate());
  Cleanup();
}

TEST(DeserializeWireBytesAndSerializedDataInvalid) {
  WasmSerializationTest test;
  {
    HandleScope scope(test.current_isolate());
    test.InvalidateVersion();
    test.InvalidateWireBytes();
    test.Deserialize();
  }
  Cleanup(test.current_isolate());
  Cleanup();
}

bool False(v8::Local<v8::Context> context, v8::Local<v8::String> source) {
  return false;
}

TEST(BlockWasmCodeGenAtDeserialization) {
  WasmSerializationTest test;
  {
    HandleScope scope(test.current_isolate());
    test.current_isolate_v8()->SetAllowCodeGenerationFromStringsCallback(False);
    v8::MaybeLocal<v8::WasmCompiledModule> nothing = test.Deserialize();
    CHECK(nothing.IsEmpty());
  }
  Cleanup(test.current_isolate());
  Cleanup();
}

TEST(TransferrableWasmModules) {
  v8::internal::AccountingAllocator allocator;
  Zone zone(&allocator, ZONE_NAME);

  ZoneBuffer buffer(&zone);
  WasmSerializationTest::BuildWireBytes(&zone, &buffer);

  Isolate* from_isolate = CcTest::InitIsolateOnce();
  ErrorThrower thrower(from_isolate, "");
  std::vector<v8::WasmCompiledModule::TransferrableModule> store;
  {
    HandleScope scope(from_isolate);
    testing::SetupIsolateForWasmModule(from_isolate);

    MaybeHandle<WasmModuleObject> module_object =
        from_isolate->wasm_engine()->SyncCompile(
            from_isolate, &thrower,
            ModuleWireBytes(buffer.begin(), buffer.end()));
    v8::Local<v8::WasmCompiledModule> v8_module =
        v8::Local<v8::WasmCompiledModule>::Cast(v8::Utils::ToLocal(
            Handle<JSObject>::cast(module_object.ToHandleChecked())));
    store.push_back(v8_module->GetTransferrableModule());
  }

  {
    v8::Isolate::CreateParams create_params;
    create_params.array_buffer_allocator =
        from_isolate->array_buffer_allocator();
    v8::Isolate* to_isolate = v8::Isolate::New(create_params);
    {
      v8::HandleScope new_scope(to_isolate);
      v8::Local<v8::Context> deserialization_context =
          v8::Context::New(to_isolate);
      deserialization_context->Enter();
      v8::MaybeLocal<v8::WasmCompiledModule> mod =
          v8::WasmCompiledModule::FromTransferrableModule(to_isolate, store[0]);
      CHECK(!mod.IsEmpty());
    }
    to_isolate->Dispose();
  }
}

TEST(MemorySize) {
  {
    // Initial memory size is 16, see wasm-module-builder.cc
    static const int kExpectedValue = 16;
    TestSignatures sigs;
    v8::internal::AccountingAllocator allocator;
    Zone zone(&allocator, ZONE_NAME);

    WasmModuleBuilder* builder = new (&zone) WasmModuleBuilder(&zone);
    WasmFunctionBuilder* f = builder->AddFunction(sigs.i_v());
    ExportAsMain(f);
    byte code[] = {WASM_MEMORY_SIZE};
    EMIT_CODE_WITH_END(f, code);
    TestModule(&zone, builder, kExpectedValue);
  }
  Cleanup();
}

TEST(Run_WasmModule_MemSize_GrowMem) {
  {
    // Initial memory size = 16 + GrowMemory(10)
    static const int kExpectedValue = 26;
    TestSignatures sigs;
    v8::internal::AccountingAllocator allocator;
    Zone zone(&allocator, ZONE_NAME);

    WasmModuleBuilder* builder = new (&zone) WasmModuleBuilder(&zone);
    WasmFunctionBuilder* f = builder->AddFunction(sigs.i_v());
    ExportAsMain(f);
    byte code[] = {WASM_GROW_MEMORY(WASM_I32V_1(10)), WASM_DROP,
                   WASM_MEMORY_SIZE};
    EMIT_CODE_WITH_END(f, code);
    TestModule(&zone, builder, kExpectedValue);
  }
  Cleanup();
}

TEST(GrowMemoryZero) {
  {
    // Initial memory size is 16, see wasm-module-builder.cc
    static const int kExpectedValue = 16;
    TestSignatures sigs;
    v8::internal::AccountingAllocator allocator;
    Zone zone(&allocator, ZONE_NAME);

    WasmModuleBuilder* builder = new (&zone) WasmModuleBuilder(&zone);
    WasmFunctionBuilder* f = builder->AddFunction(sigs.i_v());
    ExportAsMain(f);
    byte code[] = {WASM_GROW_MEMORY(WASM_I32V(0))};
    EMIT_CODE_WITH_END(f, code);
    TestModule(&zone, builder, kExpectedValue);
  }
  Cleanup();
}

class InterruptThread : public v8::base::Thread {
 public:
  explicit InterruptThread(Isolate* isolate, int32_t* memory)
      : Thread(Options("TestInterruptLoop")),
        isolate_(isolate),
        memory_(memory) {}

  static void OnInterrupt(v8::Isolate* isolate, void* data) {
    int32_t* m = reinterpret_cast<int32_t*>(data);
    // Set the interrupt location to 0 to break the loop in {TestInterruptLoop}.
    Address ptr = reinterpret_cast<Address>(&m[interrupt_location_]);
    WriteLittleEndianValue<int32_t>(ptr, interrupt_value_);
  }

  virtual void Run() {
    // Wait for the main thread to write the signal value.
    int32_t val = 0;
    do {
      val = memory_[0];
      val = ReadLittleEndianValue<int32_t>(reinterpret_cast<Address>(&val));
    } while (val != signal_value_);
    isolate_->RequestInterrupt(&OnInterrupt, const_cast<int32_t*>(memory_));
  }

  Isolate* isolate_;
  volatile int32_t* memory_;
  static const int32_t interrupt_location_ = 10;
  static const int32_t interrupt_value_ = 154;
  static const int32_t signal_value_ = 1221;
};

TEST(TestInterruptLoop) {
  {
    // Do not dump the module of this test because it contains an infinite loop.
    if (FLAG_dump_wasm_module) return;

    // This test tests that WebAssembly loops can be interrupted, i.e. that if
    // an
    // InterruptCallback is registered by {Isolate::RequestInterrupt}, then the
    // InterruptCallback is eventually called even if a loop in WebAssembly code
    // is executed.
    // Test setup:
    // The main thread executes a WebAssembly function with a loop. In the loop
    // {signal_value_} is written to memory to signal a helper thread that the
    // main thread reached the loop in the WebAssembly program. When the helper
    // thread reads {signal_value_} from memory, it registers the
    // InterruptCallback. Upon exeution, the InterruptCallback write into the
    // WebAssemblyMemory to end the loop in the WebAssembly program.
    TestSignatures sigs;
    Isolate* isolate = CcTest::InitIsolateOnce();
    v8::internal::AccountingAllocator allocator;
    Zone zone(&allocator, ZONE_NAME);

    WasmModuleBuilder* builder = new (&zone) WasmModuleBuilder(&zone);
    WasmFunctionBuilder* f = builder->AddFunction(sigs.i_v());
    ExportAsMain(f);
    byte code[] = {
        WASM_LOOP(
            WASM_IFB(WASM_NOT(WASM_LOAD_MEM(
                         MachineType::Int32(),
                         WASM_I32V(InterruptThread::interrupt_location_ * 4))),
                     WASM_STORE_MEM(MachineType::Int32(), WASM_ZERO,
                                    WASM_I32V(InterruptThread::signal_value_)),
                     WASM_BR(1))),
        WASM_I32V(121)};
    EMIT_CODE_WITH_END(f, code);
    ZoneBuffer buffer(&zone);
    builder->WriteTo(buffer);

    HandleScope scope(isolate);
    testing::SetupIsolateForWasmModule(isolate);
    ErrorThrower thrower(isolate, "Test");
    const Handle<WasmInstanceObject> instance =
        CompileAndInstantiateForTesting(
            isolate, &thrower, ModuleWireBytes(buffer.begin(), buffer.end()))
            .ToHandleChecked();

    Handle<JSArrayBuffer> memory(instance->memory_object()->array_buffer(),
                                 isolate);
    int32_t* memory_array = reinterpret_cast<int32_t*>(memory->backing_store());

    InterruptThread thread(isolate, memory_array);
    thread.Start();
    testing::RunWasmModuleForTesting(isolate, instance, 0, nullptr);
    Address address = reinterpret_cast<Address>(
        &memory_array[InterruptThread::interrupt_location_]);
    CHECK_EQ(InterruptThread::interrupt_value_,
             ReadLittleEndianValue<int32_t>(address));
  }
  Cleanup();
}

TEST(Run_WasmModule_GrowMemoryInIf) {
  {
    TestSignatures sigs;
    v8::internal::AccountingAllocator allocator;
    Zone zone(&allocator, ZONE_NAME);
    WasmModuleBuilder* builder = new (&zone) WasmModuleBuilder(&zone);
    WasmFunctionBuilder* f = builder->AddFunction(sigs.i_v());
    ExportAsMain(f);
    byte code[] = {WASM_IF_ELSE_I(WASM_I32V(0), WASM_GROW_MEMORY(WASM_I32V(1)),
                                  WASM_I32V(12))};
    EMIT_CODE_WITH_END(f, code);
    TestModule(&zone, builder, 12);
  }
  Cleanup();
}

TEST(Run_WasmModule_GrowMemOobOffset) {
  {
    static const int kPageSize = 0x10000;
    // Initial memory size = 16 + GrowMemory(10)
    static const int index = kPageSize * 17 + 4;
    int value = 0xACED;
    TestSignatures sigs;
    v8::internal::AccountingAllocator allocator;
    Zone zone(&allocator, ZONE_NAME);

    WasmModuleBuilder* builder = new (&zone) WasmModuleBuilder(&zone);
    WasmFunctionBuilder* f = builder->AddFunction(sigs.i_v());
    ExportAsMain(f);
    byte code[] = {WASM_GROW_MEMORY(WASM_I32V_1(1)),
                   WASM_STORE_MEM(MachineType::Int32(), WASM_I32V(index),
                                  WASM_I32V(value))};
    EMIT_CODE_WITH_END(f, code);
    TestModuleException(&zone, builder);
  }
  Cleanup();
}

TEST(Run_WasmModule_GrowMemOobFixedIndex) {
  {
    static const int kPageSize = 0x10000;
    // Initial memory size = 16 + GrowMemory(10)
    static const int index = kPageSize * 26 + 4;
    int value = 0xACED;
    TestSignatures sigs;
    Isolate* isolate = CcTest::InitIsolateOnce();
    Zone zone(isolate->allocator(), ZONE_NAME);

    WasmModuleBuilder* builder = new (&zone) WasmModuleBuilder(&zone);
    WasmFunctionBuilder* f = builder->AddFunction(sigs.i_i());
    ExportAsMain(f);
    byte code[] = {WASM_GROW_MEMORY(WASM_GET_LOCAL(0)), WASM_DROP,
                   WASM_STORE_MEM(MachineType::Int32(), WASM_I32V(index),
                                  WASM_I32V(value)),
                   WASM_LOAD_MEM(MachineType::Int32(), WASM_I32V(index))};
    EMIT_CODE_WITH_END(f, code);

    HandleScope scope(isolate);
    ZoneBuffer buffer(&zone);
    builder->WriteTo(buffer);
    testing::SetupIsolateForWasmModule(isolate);

    ErrorThrower thrower(isolate, "Test");
    Handle<WasmInstanceObject> instance =
        CompileAndInstantiateForTesting(
            isolate, &thrower, ModuleWireBytes(buffer.begin(), buffer.end()))
            .ToHandleChecked();

    // Initial memory size is 16 pages, should trap till index > MemSize on
    // consecutive GrowMem calls
    for (uint32_t i = 1; i < 5; i++) {
      Handle<Object> params[1] = {Handle<Object>(Smi::FromInt(i), isolate)};
      v8::TryCatch try_catch(reinterpret_cast<v8::Isolate*>(isolate));
      testing::RunWasmModuleForTesting(isolate, instance, 1, params);
      CHECK(try_catch.HasCaught());
      isolate->clear_pending_exception();
    }

    Handle<Object> params[1] = {Handle<Object>(Smi::FromInt(1), isolate)};
    int32_t result =
        testing::RunWasmModuleForTesting(isolate, instance, 1, params);
    CHECK_EQ(0xACED, result);
  }
  Cleanup();
}

TEST(Run_WasmModule_GrowMemOobVariableIndex) {
  {
    static const int kPageSize = 0x10000;
    int value = 0xACED;
    TestSignatures sigs;
    Isolate* isolate = CcTest::InitIsolateOnce();
    v8::internal::AccountingAllocator allocator;
    Zone zone(&allocator, ZONE_NAME);

    WasmModuleBuilder* builder = new (&zone) WasmModuleBuilder(&zone);
    WasmFunctionBuilder* f = builder->AddFunction(sigs.i_i());
    ExportAsMain(f);
    byte code[] = {WASM_GROW_MEMORY(WASM_I32V_1(1)), WASM_DROP,
                   WASM_STORE_MEM(MachineType::Int32(), WASM_GET_LOCAL(0),
                                  WASM_I32V(value)),
                   WASM_LOAD_MEM(MachineType::Int32(), WASM_GET_LOCAL(0))};
    EMIT_CODE_WITH_END(f, code);

    HandleScope scope(isolate);
    ZoneBuffer buffer(&zone);
    builder->WriteTo(buffer);
    testing::SetupIsolateForWasmModule(isolate);

    ErrorThrower thrower(isolate, "Test");
    Handle<WasmInstanceObject> instance =
        CompileAndInstantiateForTesting(
            isolate, &thrower, ModuleWireBytes(buffer.begin(), buffer.end()))
            .ToHandleChecked();

    // Initial memory size is 16 pages, should trap till index > MemSize on
    // consecutive GrowMem calls
    for (int i = 1; i < 5; i++) {
      Handle<Object> params[1] = {
          Handle<Object>(Smi::FromInt((16 + i) * kPageSize - 3), isolate)};
      v8::TryCatch try_catch(reinterpret_cast<v8::Isolate*>(isolate));
      testing::RunWasmModuleForTesting(isolate, instance, 1, params);
      CHECK(try_catch.HasCaught());
      isolate->clear_pending_exception();
    }

    for (int i = 1; i < 5; i++) {
      Handle<Object> params[1] = {
          Handle<Object>(Smi::FromInt((20 + i) * kPageSize - 4), isolate)};
      int32_t result =
          testing::RunWasmModuleForTesting(isolate, instance, 1, params);
      CHECK_EQ(0xACED, result);
    }

    v8::TryCatch try_catch(reinterpret_cast<v8::Isolate*>(isolate));
    Handle<Object> params[1] = {
        Handle<Object>(Smi::FromInt(25 * kPageSize), isolate)};
    testing::RunWasmModuleForTesting(isolate, instance, 1, params);
    CHECK(try_catch.HasCaught());
    isolate->clear_pending_exception();
  }
  Cleanup();
}

TEST(Run_WasmModule_Global_init) {
  {
    v8::internal::AccountingAllocator allocator;
    Zone zone(&allocator, ZONE_NAME);
    TestSignatures sigs;

    WasmModuleBuilder* builder = new (&zone) WasmModuleBuilder(&zone);
    uint32_t global1 =
        builder->AddGlobal(kWasmI32, false, false, WasmInitExpr(777777));
    uint32_t global2 =
        builder->AddGlobal(kWasmI32, false, false, WasmInitExpr(222222));
    WasmFunctionBuilder* f1 = builder->AddFunction(sigs.i_v());
    byte code[] = {
        WASM_I32_ADD(WASM_GET_GLOBAL(global1), WASM_GET_GLOBAL(global2))};
    EMIT_CODE_WITH_END(f1, code);
    ExportAsMain(f1);
    TestModule(&zone, builder, 999999);
  }
  Cleanup();
}

template <typename CType>
static void RunWasmModuleGlobalInitTest(ValueType type, CType expected) {
  {
    v8::internal::AccountingAllocator allocator;
    Zone zone(&allocator, ZONE_NAME);
    TestSignatures sigs;

    ValueType types[] = {type};
    FunctionSig sig(1, 0, types);

    for (int padding = 0; padding < 5; padding++) {
      // Test with a simple initializer
      WasmModuleBuilder* builder = new (&zone) WasmModuleBuilder(&zone);

      for (int i = 0; i < padding; i++) {  // pad global before
        builder->AddGlobal(kWasmI32, false, false, WasmInitExpr(i + 20000));
      }
      uint32_t global =
          builder->AddGlobal(type, false, false, WasmInitExpr(expected));
      for (int i = 0; i < padding; i++) {  // pad global after
        builder->AddGlobal(kWasmI32, false, false, WasmInitExpr(i + 30000));
      }

      WasmFunctionBuilder* f1 = builder->AddFunction(&sig);
      byte code[] = {WASM_GET_GLOBAL(global)};
      EMIT_CODE_WITH_END(f1, code);
      ExportAsMain(f1);
      TestModule(&zone, builder, expected);
    }
  }
  Cleanup();
}

TEST(Run_WasmModule_Global_i32) {
  RunWasmModuleGlobalInitTest<int32_t>(kWasmI32, -983489);
  RunWasmModuleGlobalInitTest<int32_t>(kWasmI32, 11223344);
}

TEST(Run_WasmModule_Global_f32) {
  RunWasmModuleGlobalInitTest<float>(kWasmF32, -983.9f);
  RunWasmModuleGlobalInitTest<float>(kWasmF32, 1122.99f);
}

TEST(Run_WasmModule_Global_f64) {
  RunWasmModuleGlobalInitTest<double>(kWasmF64, -833.9);
  RunWasmModuleGlobalInitTest<double>(kWasmF64, 86374.25);
}

TEST(InitDataAtTheUpperLimit) {
  {
    Isolate* isolate = CcTest::InitIsolateOnce();
    HandleScope scope(isolate);
    testing::SetupIsolateForWasmModule(isolate);

    ErrorThrower thrower(isolate, "Run_WasmModule_InitDataAtTheUpperLimit");

    const byte data[] = {
        WASM_MODULE_HEADER,   // --
        kMemorySectionCode,   // --
        U32V_1(4),            // section size
        ENTRY_COUNT(1),       // --
        kHasMaximumFlag,      // --
        1,                    // initial size
        2,                    // maximum size
        kDataSectionCode,     // --
        U32V_1(9),            // section size
        ENTRY_COUNT(1),       // --
        0,                    // linear memory index
        WASM_I32V_3(0xFFFF),  // destination offset
        kExprEnd,
        U32V_1(1),  // source size
        'c'         // data bytes
    };

    CompileAndInstantiateForTesting(
        isolate, &thrower, ModuleWireBytes(data, data + arraysize(data)));
    if (thrower.error()) {
      thrower.Reify()->Print();
      FATAL("compile or instantiate error");
    }
  }
  Cleanup();
}

TEST(EmptyMemoryNonEmptyDataSegment) {
  {
    Isolate* isolate = CcTest::InitIsolateOnce();
    HandleScope scope(isolate);
    testing::SetupIsolateForWasmModule(isolate);

    ErrorThrower thrower(isolate, "Run_WasmModule_InitDataAtTheUpperLimit");

    const byte data[] = {
        WASM_MODULE_HEADER,  // --
        kMemorySectionCode,  // --
        U32V_1(4),           // section size
        ENTRY_COUNT(1),      // --
        kHasMaximumFlag,     // --
        0,                   // initial size
        0,                   // maximum size
        kDataSectionCode,    // --
        U32V_1(7),           // section size
        ENTRY_COUNT(1),      // --
        0,                   // linear memory index
        WASM_I32V_1(8),      // destination offset
        kExprEnd,
        U32V_1(1),  // source size
        'c'         // data bytes
    };

    CompileAndInstantiateForTesting(
        isolate, &thrower, ModuleWireBytes(data, data + arraysize(data)));
    // It should not be possible to instantiate this module.
    CHECK(thrower.error());
  }
  Cleanup();
}

TEST(EmptyMemoryEmptyDataSegment) {
  {
    Isolate* isolate = CcTest::InitIsolateOnce();
    HandleScope scope(isolate);
    testing::SetupIsolateForWasmModule(isolate);

    ErrorThrower thrower(isolate, "Run_WasmModule_InitDataAtTheUpperLimit");

    const byte data[] = {
        WASM_MODULE_HEADER,  // --
        kMemorySectionCode,  // --
        U32V_1(4),           // section size
        ENTRY_COUNT(1),      // --
        kHasMaximumFlag,     // --
        0,                   // initial size
        0,                   // maximum size
        kDataSectionCode,    // --
        U32V_1(6),           // section size
        ENTRY_COUNT(1),      // --
        0,                   // linear memory index
        WASM_I32V_1(0),      // destination offset
        kExprEnd,
        U32V_1(0),  // source size
    };

    CompileAndInstantiateForTesting(
        isolate, &thrower, ModuleWireBytes(data, data + arraysize(data)));
    // It should be possible to instantiate this module.
    CHECK(!thrower.error());
  }
  Cleanup();
}

TEST(MemoryWithOOBEmptyDataSegment) {
  {
    Isolate* isolate = CcTest::InitIsolateOnce();
    HandleScope scope(isolate);
    testing::SetupIsolateForWasmModule(isolate);

    ErrorThrower thrower(isolate, "Run_WasmModule_InitDataAtTheUpperLimit");

    const byte data[] = {
        WASM_MODULE_HEADER,      // --
        kMemorySectionCode,      // --
        U32V_1(4),               // section size
        ENTRY_COUNT(1),          // --
        kHasMaximumFlag,         // --
        1,                       // initial size
        1,                       // maximum size
        kDataSectionCode,        // --
        U32V_1(9),               // section size
        ENTRY_COUNT(1),          // --
        0,                       // linear memory index
        WASM_I32V_4(0x2468ACE),  // destination offset
        kExprEnd,
        U32V_1(0),  // source size
    };

    CompileAndInstantiateForTesting(
        isolate, &thrower, ModuleWireBytes(data, data + arraysize(data)));
    // It should not be possible to instantiate this module.
    CHECK(thrower.error());
  }
  Cleanup();
}

// Utility to free the allocated memory for a buffer that is manually
// externalized in a test.
struct ManuallyExternalizedBuffer {
  Isolate* isolate_;
  Handle<JSArrayBuffer> buffer_;
  void* allocation_base_;
  size_t allocation_length_;
  bool const should_free_;

  ManuallyExternalizedBuffer(JSArrayBuffer* buffer, Isolate* isolate)
      : isolate_(isolate),
        buffer_(buffer, isolate),
        allocation_base_(buffer->allocation_base()),
        allocation_length_(buffer->allocation_length()),
        should_free_(!isolate_->wasm_engine()->memory_tracker()->IsWasmMemory(
            buffer->backing_store())) {
    if (!isolate_->wasm_engine()->memory_tracker()->IsWasmMemory(
            buffer->backing_store())) {
      v8::Utils::ToLocal(buffer_)->Externalize();
    }
  }
  ~ManuallyExternalizedBuffer() {
    if (should_free_) {
      buffer_->FreeBackingStoreFromMainThread();
    }
  }
};

TEST(Run_WasmModule_Buffer_Externalized_GrowMem) {
  {
    Isolate* isolate = CcTest::InitIsolateOnce();
    HandleScope scope(isolate);
    TestSignatures sigs;
    v8::internal::AccountingAllocator allocator;
    Zone zone(&allocator, ZONE_NAME);

    WasmModuleBuilder* builder = new (&zone) WasmModuleBuilder(&zone);
    WasmFunctionBuilder* f = builder->AddFunction(sigs.i_v());
    ExportAsMain(f);
    byte code[] = {WASM_GROW_MEMORY(WASM_I32V_1(6)), WASM_DROP,
                   WASM_MEMORY_SIZE};
    EMIT_CODE_WITH_END(f, code);

    ZoneBuffer buffer(&zone);
    builder->WriteTo(buffer);
    testing::SetupIsolateForWasmModule(isolate);
    ErrorThrower thrower(isolate, "Test");
    const Handle<WasmInstanceObject> instance =
        CompileAndInstantiateForTesting(
            isolate, &thrower, ModuleWireBytes(buffer.begin(), buffer.end()))
            .ToHandleChecked();
    Handle<WasmMemoryObject> memory_object(instance->memory_object(), isolate);

    // Fake the Embedder flow by externalizing the array buffer.
    ManuallyExternalizedBuffer buffer1(memory_object->array_buffer(), isolate);

    // Grow using the API.
    uint32_t result = WasmMemoryObject::Grow(isolate, memory_object, 4);
    CHECK_EQ(16, result);
    CHECK(buffer1.buffer_->was_neutered());  // growing always neuters
    CHECK_EQ(0, buffer1.buffer_->byte_length()->Number());

    CHECK_NE(*buffer1.buffer_, memory_object->array_buffer());

    // Fake the Embedder flow by externalizing the array buffer.
    ManuallyExternalizedBuffer buffer2(memory_object->array_buffer(), isolate);

    // Grow using an internal WASM bytecode.
    result = testing::RunWasmModuleForTesting(isolate, instance, 0, nullptr);
    CHECK_EQ(26, result);
    CHECK(buffer2.buffer_->was_neutered());  // growing always neuters
    CHECK_EQ(0, buffer2.buffer_->byte_length()->Number());
    CHECK_NE(*buffer2.buffer_, memory_object->array_buffer());
  }
  Cleanup();
}

TEST(Run_WasmModule_Buffer_Externalized_GrowMemMemSize) {
  {
    Isolate* isolate = CcTest::InitIsolateOnce();
    HandleScope scope(isolate);
    Handle<JSArrayBuffer> buffer;
    CHECK(wasm::NewArrayBuffer(isolate, 16 * kWasmPageSize).ToHandle(&buffer));
    Handle<WasmMemoryObject> mem_obj =
        WasmMemoryObject::New(isolate, buffer, 100);
    auto const contents = v8::Utils::ToLocal(buffer)->Externalize();
    int32_t result = WasmMemoryObject::Grow(isolate, mem_obj, 0);
    CHECK_EQ(16, result);
    constexpr bool is_wasm_memory = true;
    const JSArrayBuffer::Allocation allocation{
        contents.AllocationBase(), contents.AllocationLength(), contents.Data(),
        contents.AllocationMode(), is_wasm_memory};
    JSArrayBuffer::FreeBackingStore(isolate, allocation);
  }
  Cleanup();
}

TEST(Run_WasmModule_Buffer_Externalized_Detach) {
  {
    // Regression test for
    // https://bugs.chromium.org/p/chromium/issues/detail?id=731046
    Isolate* isolate = CcTest::InitIsolateOnce();
    HandleScope scope(isolate);
    Handle<JSArrayBuffer> buffer;
    CHECK(wasm::NewArrayBuffer(isolate, 16 * kWasmPageSize).ToHandle(&buffer));
    auto const contents = v8::Utils::ToLocal(buffer)->Externalize();
    wasm::DetachMemoryBuffer(isolate, buffer, true);
    constexpr bool is_wasm_memory = true;
    const JSArrayBuffer::Allocation allocation{
        contents.AllocationBase(), contents.AllocationLength(), contents.Data(),
        contents.AllocationMode(), is_wasm_memory};
    JSArrayBuffer::FreeBackingStore(isolate, allocation);
  }
  Cleanup();
}

TEST(Run_WasmModule_Buffer_Externalized_Regression_UseAfterFree) {
  // Regresion test for https://crbug.com/813876
  Isolate* isolate = CcTest::InitIsolateOnce();
  HandleScope scope(isolate);
  Handle<JSArrayBuffer> buffer;
  CHECK(wasm::NewArrayBuffer(isolate, 16 * kWasmPageSize).ToHandle(&buffer));
  Handle<WasmMemoryObject> mem = WasmMemoryObject::New(isolate, buffer, 128);
  auto contents = v8::Utils::ToLocal(buffer)->Externalize();
  WasmMemoryObject::Grow(isolate, mem, 0);
  constexpr bool is_wasm_memory = true;
  JSArrayBuffer::FreeBackingStore(
      isolate, JSArrayBuffer::Allocation(
                   contents.AllocationBase(), contents.AllocationLength(),
                   contents.Data(), contents.AllocationMode(), is_wasm_memory));
  // Make sure we can write to the buffer without crashing
  uint32_t* int_buffer =
      reinterpret_cast<uint32_t*>(mem->array_buffer()->backing_store());
  int_buffer[0] = 0;
}

#if V8_TARGET_ARCH_64_BIT
TEST(Run_WasmModule_Reclaim_Memory) {
  // Make sure we can allocate memories without running out of address space.
  Isolate* isolate = CcTest::InitIsolateOnce();
  Handle<JSArrayBuffer> buffer;
  for (int i = 0; i < 256; ++i) {
    HandleScope scope(isolate);
    CHECK(NewArrayBuffer(isolate, kWasmPageSize, SharedFlag::kNotShared)
              .ToHandle(&buffer));
  }
}
#endif

TEST(AtomicOpDisassembly) {
  {
    EXPERIMENTAL_FLAG_SCOPE(threads);
    TestSignatures sigs;
    Isolate* isolate = CcTest::InitIsolateOnce();
    v8::internal::AccountingAllocator allocator;
    Zone zone(&allocator, ZONE_NAME);

    WasmModuleBuilder* builder = new (&zone) WasmModuleBuilder(&zone);
    builder->SetHasSharedMemory();
    builder->SetMaxMemorySize(16);
    WasmFunctionBuilder* f = builder->AddFunction(sigs.i_i());
    ExportAsMain(f);
    byte code[] = {
        WASM_ATOMICS_STORE_OP(kExprI32AtomicStore, WASM_ZERO, WASM_GET_LOCAL(0),
                              MachineRepresentation::kWord32),
        WASM_ATOMICS_LOAD_OP(kExprI32AtomicLoad, WASM_ZERO,
                             MachineRepresentation::kWord32)};
    EMIT_CODE_WITH_END(f, code);

    HandleScope scope(isolate);
    ZoneBuffer buffer(&zone);
    builder->WriteTo(buffer);
    testing::SetupIsolateForWasmModule(isolate);

    ErrorThrower thrower(isolate, "Test");
    MaybeHandle<WasmModuleObject> module_object =
        isolate->wasm_engine()->SyncCompile(
            isolate, &thrower, ModuleWireBytes(buffer.begin(), buffer.end()));

    module_object.ToHandleChecked()->shared()->DisassembleFunction(0);
  }
  Cleanup();
}

#undef EMIT_CODE_WITH_END

}  // namespace wasm
}  // namespace internal
}  // namespace v8

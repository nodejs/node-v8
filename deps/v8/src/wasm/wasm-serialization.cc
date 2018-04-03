// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/wasm/wasm-serialization.h"

#include "src/assembler-inl.h"
#include "src/code-stubs.h"
#include "src/external-reference-table.h"
#include "src/objects-inl.h"
#include "src/objects.h"
#include "src/snapshot/code-serializer.h"
#include "src/snapshot/serializer-common.h"
#include "src/utils.h"
#include "src/version.h"
#include "src/wasm/module-compiler.h"
#include "src/wasm/module-decoder.h"
#include "src/wasm/wasm-code-manager.h"
#include "src/wasm/wasm-module.h"
#include "src/wasm/wasm-objects-inl.h"
#include "src/wasm/wasm-objects.h"
#include "src/wasm/wasm-result.h"

namespace v8 {
namespace internal {
namespace wasm {

namespace {

class Writer {
 public:
  explicit Writer(Vector<byte> buffer) : buffer_(buffer) {}
  template <typename T>
  void Write(const T& value) {
    if (FLAG_wasm_trace_serialization) {
      OFStream os(stdout);
      os << "wrote: " << (size_t)value << " sized: " << sizeof(T) << std::endl;
    }
    DCHECK_GE(buffer_.size(), sizeof(T));
    WriteUnalignedValue(buffer_.start(), value);
    buffer_ = buffer_ + sizeof(T);
  }

  void WriteVector(const Vector<const byte> data) {
    DCHECK_GE(buffer_.size(), data.size());
    if (data.size() > 0) {
      memcpy(buffer_.start(), data.start(), data.size());
      buffer_ = buffer_ + data.size();
    }
    if (FLAG_wasm_trace_serialization) {
      OFStream os(stdout);
      os << "wrote vector of " << data.size() << " elements" << std::endl;
    }
  }
  Vector<byte> current_buffer() const { return buffer_; }

 private:
  Vector<byte> buffer_;
};

class Reader {
 public:
  explicit Reader(Vector<const byte> buffer) : buffer_(buffer) {}

  template <typename T>
  T Read() {
    DCHECK_GE(buffer_.size(), sizeof(T));
    T ret = ReadUnalignedValue<T>(buffer_.start());
    buffer_ = buffer_ + sizeof(T);
    if (FLAG_wasm_trace_serialization) {
      OFStream os(stdout);
      os << "read: " << (size_t)ret << " sized: " << sizeof(T) << std::endl;
    }
    return ret;
  }

  Vector<const byte> GetSubvector(size_t size) {
    Vector<const byte> ret = {buffer_.start(), size};
    buffer_ = buffer_ + size;
    return ret;
  }

  void ReadIntoVector(const Vector<byte> data) {
    if (data.size() > 0) {
      DCHECK_GE(buffer_.size(), data.size());
      memcpy(data.start(), buffer_.start(), data.size());
      buffer_ = buffer_ + data.size();
    }
    if (FLAG_wasm_trace_serialization) {
      OFStream os(stdout);
      os << "read vector of " << data.size() << " elements" << std::endl;
    }
  }

  Vector<const byte> current_buffer() const { return buffer_; }

 private:
  Vector<const byte> buffer_;
};

constexpr size_t kVersionSize = 4 * sizeof(uint32_t);

// Start from 1 so an encoded stub id is not confused with an encoded builtin.
constexpr int kFirstStubId = 1;

void WriteVersion(Isolate* isolate, Vector<byte> buffer) {
  DCHECK_GE(buffer.size(), kVersionSize);
  Writer writer(buffer);
  writer.Write(SerializedData::ComputeMagicNumber(
      isolate->heap()->external_reference_table()));
  writer.Write(Version::Hash());
  writer.Write(static_cast<uint32_t>(CpuFeatures::SupportedFeatures()));
  writer.Write(FlagList::Hash());
}

bool IsSupportedVersion(Isolate* isolate, const Vector<const byte> buffer) {
  if (buffer.size() < kVersionSize) return false;
  byte version[kVersionSize];
  WriteVersion(isolate, {version, kVersionSize});
  if (memcmp(buffer.start(), version, kVersionSize) == 0) return true;
  return false;
}

// On Intel, call sites are encoded as a displacement. For linking
// and for serialization/deserialization, we want to store/retrieve
// a tag (the function index). On Intel, that means accessing the
// raw displacement. Everywhere else, that simply means accessing
// the target address.
void SetWasmCalleeTag(RelocInfo* rinfo, uint32_t tag) {
#if V8_TARGET_ARCH_X64 || V8_TARGET_ARCH_IA32
  *(reinterpret_cast<uint32_t*>(rinfo->target_address_address())) = tag;
#else
  rinfo->set_target_address(reinterpret_cast<Address>(tag), SKIP_WRITE_BARRIER,
                            SKIP_ICACHE_FLUSH);
#endif
}

uint32_t GetWasmCalleeTag(RelocInfo* rinfo) {
#if V8_TARGET_ARCH_X64 || V8_TARGET_ARCH_IA32
  return *(reinterpret_cast<uint32_t*>(rinfo->target_address_address()));
#else
  return static_cast<uint32_t>(
      reinterpret_cast<size_t>(rinfo->target_address()));
#endif
}

}  // namespace

enum SerializationSection { Init, Metadata, Stubs, CodeSection, Done };

class V8_EXPORT_PRIVATE NativeModuleSerializer {
 public:
  explicit NativeModuleSerializer(Isolate*, const NativeModule*);
  size_t Measure() const;
  size_t Write(Vector<byte>);
  bool IsDone() const { return state_ == Done; }

 private:
  size_t MeasureHeader() const;
  static size_t GetCodeHeaderSize();
  size_t MeasureCode(const WasmCode*) const;
  size_t MeasureCopiedStubs() const;

  void BufferHeader();
  // we buffer all the stubs because they are small
  void BufferCopiedStubs();
  void BufferCodeInAllocatedScratch(const WasmCode*);
  void BufferCurrentWasmCode();
  size_t DrainBuffer(Vector<byte> dest);
  uint32_t EncodeBuiltinOrStub(Address);

  Isolate* const isolate_ = nullptr;
  const NativeModule* const native_module_ = nullptr;
  SerializationSection state_ = Init;
  uint32_t index_ = 0;
  std::vector<byte> scratch_;
  Vector<byte> remaining_;
  // wasm and copied stubs reverse lookup
  std::map<Address, uint32_t> wasm_targets_lookup_;
  // immovable builtins and runtime entries lookup
  std::map<Address, uint32_t> reference_table_lookup_;
  std::map<Address, uint32_t> stub_lookup_;
  std::map<Address, uint32_t> builtin_lookup_;
};

class V8_EXPORT_PRIVATE NativeModuleDeserializer {
 public:
  explicit NativeModuleDeserializer(Isolate*, NativeModule*);
  // Currently, we don't support streamed reading, yet albeit the
  // API suggests that.
  bool Read(Vector<const byte>);

 private:
  void ExpectHeader();
  void Expect(size_t size);
  bool ReadHeader();
  bool ReadCode();
  bool ReadStubs();
  Address GetTrampolineOrStubFromTag(uint32_t);

  Isolate* const isolate_ = nullptr;
  NativeModule* const native_module_ = nullptr;
  std::vector<byte> scratch_;
  std::vector<Address> stubs_;
  Vector<const byte> unread_;
  size_t current_expectation_ = 0;
  uint32_t index_ = 0;
};

NativeModuleSerializer::NativeModuleSerializer(Isolate* isolate,
                                               const NativeModule* module)
    : isolate_(isolate), native_module_(module) {
  DCHECK_NOT_NULL(isolate_);
  DCHECK_NOT_NULL(native_module_);
  // TODO(mtrofin): persist the export wrappers. Ideally, we'd only persist
  // the unique ones, i.e. the cache.
  ExternalReferenceTable* table = isolate_->heap()->external_reference_table();
  for (uint32_t i = 0; i < table->size(); ++i) {
    Address addr = table->address(i);
    reference_table_lookup_.insert(std::make_pair(addr, i));
  }
  // defer populating the stub_lookup_ to when we buffer the stubs
  for (auto pair : native_module_->trampolines_) {
    v8::internal::Code* code = Code::GetCodeFromTargetAddress(pair.first);
    int builtin_index = code->builtin_index();
    if (builtin_index >= 0) {
      uint32_t tag = static_cast<uint32_t>(builtin_index);
      builtin_lookup_.insert(std::make_pair(pair.second, tag));
    }
  }
  BufferHeader();
  state_ = Metadata;
}

size_t NativeModuleSerializer::MeasureHeader() const {
  return sizeof(uint32_t) +  // total wasm fct count
         sizeof(uint32_t);  // imported fcts - i.e. index of first wasm function
}

void NativeModuleSerializer::BufferHeader() {
  size_t metadata_size = MeasureHeader();
  scratch_.resize(metadata_size);
  remaining_ = {scratch_.data(), metadata_size};
  Writer writer(remaining_);
  writer.Write(native_module_->FunctionCount());
  writer.Write(native_module_->num_imported_functions());
}

size_t NativeModuleSerializer::GetCodeHeaderSize() {
  return sizeof(size_t) +         // size of this section
         sizeof(size_t) +         // offset of constant pool
         sizeof(size_t) +         // offset of safepoint table
         sizeof(size_t) +         // offset of handler table
         sizeof(uint32_t) +       // stack slots
         sizeof(size_t) +         // code size
         sizeof(size_t) +         // reloc size
         sizeof(size_t) +         // source positions size
         sizeof(size_t) +         // protected instructions size
         sizeof(WasmCode::Tier);  // tier
}

size_t NativeModuleSerializer::MeasureCode(const WasmCode* code) const {
  return GetCodeHeaderSize() + code->instructions().size() +  // code
         code->reloc_info().size() +                          // reloc info
         code->source_positions().size() +                    // source pos.
         code->protected_instructions().size() *              // protected inst.
             sizeof(trap_handler::ProtectedInstructionData);
}

size_t NativeModuleSerializer::Measure() const {
  size_t ret = MeasureHeader() + MeasureCopiedStubs();
  for (uint32_t i = native_module_->num_imported_functions(),
                e = native_module_->FunctionCount();
       i < e; ++i) {
    ret += MeasureCode(native_module_->GetCode(i));
  }
  return ret;
}

size_t NativeModuleSerializer::DrainBuffer(Vector<byte> dest) {
  size_t to_write = std::min(dest.size(), remaining_.size());
  memcpy(dest.start(), remaining_.start(), to_write);
  DCHECK_GE(remaining_.size(), to_write);
  remaining_ = remaining_ + to_write;
  return to_write;
}

size_t NativeModuleSerializer::MeasureCopiedStubs() const {
  size_t ret = sizeof(uint32_t) +  // number of stubs
               native_module_->stubs_.size() * sizeof(uint32_t);  // stub keys
  for (auto pair : native_module_->trampolines_) {
    v8::internal::Code* code = Code::GetCodeFromTargetAddress(pair.first);
    int builtin_index = code->builtin_index();
    if (builtin_index < 0) ret += sizeof(uint32_t);
  }
  return ret;
}

void NativeModuleSerializer::BufferCopiedStubs() {
  // We buffer all the stubs together, because they are very likely
  // few and small. Each stub is buffered like a WasmCode would,
  // and in addition prefaced by its stub key. The whole section is prefaced
  // by the number of stubs.
  size_t buff_size = MeasureCopiedStubs();
  scratch_.resize(buff_size);
  remaining_ = {scratch_.data(), buff_size};
  Writer writer(remaining_);
  writer.Write(
      static_cast<uint32_t>((buff_size - sizeof(uint32_t)) / sizeof(uint32_t)));
  uint32_t stub_id = kFirstStubId;

  for (auto pair : native_module_->stubs_) {
    uint32_t key = pair.first;
    writer.Write(key);
    stub_lookup_.insert(
        std::make_pair(pair.second->instructions().start(), stub_id));
    ++stub_id;
  }

  for (auto pair : native_module_->trampolines_) {
    v8::internal::Code* code = Code::GetCodeFromTargetAddress(pair.first);
    int builtin_index = code->builtin_index();
    if (builtin_index < 0) {
      stub_lookup_.insert(std::make_pair(pair.second, stub_id));
      writer.Write(code->stub_key());
      ++stub_id;
    }
  }
}

void NativeModuleSerializer::BufferCurrentWasmCode() {
  const WasmCode* code = native_module_->GetCode(index_);
  size_t size = MeasureCode(code);
  scratch_.resize(size);
  remaining_ = {scratch_.data(), size};
  BufferCodeInAllocatedScratch(code);
}

void NativeModuleSerializer::BufferCodeInAllocatedScratch(
    const WasmCode* code) {
  // We write the address, the size, and then copy the code as-is, followed
  // by reloc info, followed by source positions.
  Writer writer(remaining_);
  // write the header
  writer.Write(MeasureCode(code));
  writer.Write(code->constant_pool_offset());
  writer.Write(code->safepoint_table_offset());
  writer.Write(code->handler_table_offset());
  writer.Write(code->stack_slots());
  writer.Write(code->instructions().size());
  writer.Write(code->reloc_info().size());
  writer.Write(code->source_positions().size());
  writer.Write(code->protected_instructions().size());
  writer.Write(code->tier());
  // next is the code, which we have to reloc.
  Address serialized_code_start = writer.current_buffer().start();
  // write the code and everything else
  writer.WriteVector(code->instructions());
  writer.WriteVector(code->reloc_info());
  writer.WriteVector(code->source_positions());
  writer.WriteVector(
      {reinterpret_cast<const byte*>(code->protected_instructions().data()),
       sizeof(trap_handler::ProtectedInstructionData) *
           code->protected_instructions().size()});
  // now relocate the code
  int mask = RelocInfo::ModeMask(RelocInfo::CODE_TARGET) |
             RelocInfo::ModeMask(RelocInfo::WASM_CALL) |
             RelocInfo::ModeMask(RelocInfo::RUNTIME_ENTRY);
  RelocIterator orig_iter(code->instructions(), code->reloc_info(),
                          code->constant_pool(), mask);
  for (RelocIterator
           iter({serialized_code_start, code->instructions().size()},
                code->reloc_info(),
                serialized_code_start + code->constant_pool_offset(), mask);
       !iter.done(); iter.next(), orig_iter.next()) {
    RelocInfo::Mode mode = orig_iter.rinfo()->rmode();
    switch (mode) {
      case RelocInfo::CODE_TARGET: {
        Address orig_target = orig_iter.rinfo()->target_address();
        uint32_t tag = EncodeBuiltinOrStub(orig_target);
        SetWasmCalleeTag(iter.rinfo(), tag);
      } break;
      case RelocInfo::WASM_CALL: {
        Address orig_target = orig_iter.rinfo()->wasm_call_address();
        uint32_t tag = wasm_targets_lookup_[orig_target];
        SetWasmCalleeTag(iter.rinfo(), tag);
      } break;
      case RelocInfo::RUNTIME_ENTRY: {
        Address orig_target = orig_iter.rinfo()->target_address();
        uint32_t tag = reference_table_lookup_[orig_target];
        SetWasmCalleeTag(iter.rinfo(), tag);
      } break;
      default:
        UNREACHABLE();
    }
  }
}

uint32_t NativeModuleSerializer::EncodeBuiltinOrStub(Address address) {
  auto builtin_iter = builtin_lookup_.find(address);
  uint32_t tag = 0;
  if (builtin_iter != builtin_lookup_.end()) {
    uint32_t id = builtin_iter->second;
    DCHECK_LT(id, std::numeric_limits<uint16_t>::max());
    tag = id << 16;
  } else {
    auto stub_iter = stub_lookup_.find(address);
    DCHECK(stub_iter != stub_lookup_.end());
    uint32_t id = stub_iter->second;
    DCHECK_LT(id, std::numeric_limits<uint16_t>::max());
    tag = id & 0x0000FFFF;
  }
  return tag;
}

size_t NativeModuleSerializer::Write(Vector<byte> dest) {
  Vector<byte> original = dest;
  while (dest.size() > 0) {
    switch (state_) {
      case Metadata: {
        dest = dest + DrainBuffer(dest);
        if (remaining_.size() == 0) {
          BufferCopiedStubs();
          state_ = Stubs;
        }
        break;
      }
      case Stubs: {
        dest = dest + DrainBuffer(dest);
        if (remaining_.size() == 0) {
          index_ = native_module_->num_imported_functions();
          if (index_ < native_module_->FunctionCount()) {
            BufferCurrentWasmCode();
            state_ = CodeSection;
          } else {
            state_ = Done;
          }
        }
        break;
      }
      case CodeSection: {
        dest = dest + DrainBuffer(dest);
        if (remaining_.size() == 0) {
          ++index_;  // Move to next code object.
          if (index_ < native_module_->FunctionCount()) {
            BufferCurrentWasmCode();
          } else {
            state_ = Done;
          }
        }
        break;
      }
      default:
        UNREACHABLE();
    }
  }
  DCHECK_GE(original.size(), dest.size());
  return original.size() - dest.size();
}

// static
std::pair<std::unique_ptr<const byte[]>, size_t> SerializeNativeModule(
    Isolate* isolate, Handle<WasmCompiledModule> compiled_module) {
  NativeModule* native_module = compiled_module->GetNativeModule();
  NativeModuleSerializer serializer(isolate, native_module);
  size_t version_size = kVersionSize;
  size_t buff_size = serializer.Measure() + version_size;
  std::unique_ptr<byte[]> ret(new byte[buff_size]);
  WriteVersion(isolate, {ret.get(), buff_size});

  size_t written =
      serializer.Write({ret.get() + version_size, buff_size - version_size});
  if (written != buff_size - version_size) return {};

  return {std::move(ret), buff_size};
}

NativeModuleDeserializer::NativeModuleDeserializer(Isolate* isolate,
                                                   NativeModule* native_module)
    : isolate_(isolate), native_module_(native_module) {}

void NativeModuleDeserializer::Expect(size_t size) {
  scratch_.resize(size);
  current_expectation_ = size;
  unread_ = {scratch_.data(), size};
}

bool NativeModuleDeserializer::Read(Vector<const byte> data) {
  unread_ = data;
  if (!ReadHeader()) return false;
  if (!ReadStubs()) return false;
  index_ = native_module_->num_imported_functions();
  for (; index_ < native_module_->FunctionCount(); ++index_) {
    if (!ReadCode()) return false;
  }
  return data.size() - unread_.size();
}

bool NativeModuleDeserializer::ReadHeader() {
  size_t start_size = unread_.size();
  Reader reader(unread_);
  size_t functions = reader.Read<uint32_t>();
  size_t imports = reader.Read<uint32_t>();
  bool ok = functions == native_module_->FunctionCount() &&
            imports == native_module_->num_imported_functions();
  if (!ok) return false;

  unread_ = unread_ + (start_size - reader.current_buffer().size());
  return true;
}

bool NativeModuleDeserializer::ReadStubs() {
  size_t start_size = unread_.size();
  Reader reader(unread_);
  size_t nr_stubs = reader.Read<uint32_t>();
  stubs_.reserve(nr_stubs);
  for (size_t i = 0; i < nr_stubs; ++i) {
    uint32_t key = reader.Read<uint32_t>();
    v8::internal::Code* stub =
        *(v8::internal::CodeStub::GetCode(isolate_, key).ToHandleChecked());
    stubs_.push_back(native_module_->GetLocalAddressFor(handle(stub)));
  }
  unread_ = unread_ + (start_size - reader.current_buffer().size());
  return true;
}

bool NativeModuleDeserializer::ReadCode() {
  size_t start_size = unread_.size();
  Reader reader(unread_);
  size_t code_section_size = reader.Read<size_t>();
  USE(code_section_size);
  size_t constant_pool_offset = reader.Read<size_t>();
  size_t safepoint_table_offset = reader.Read<size_t>();
  size_t handler_table_offset = reader.Read<size_t>();
  uint32_t stack_slot_count = reader.Read<uint32_t>();
  size_t code_size = reader.Read<size_t>();
  size_t reloc_size = reader.Read<size_t>();
  size_t source_position_size = reader.Read<size_t>();
  size_t protected_instructions_size = reader.Read<size_t>();
  WasmCode::Tier tier = reader.Read<WasmCode::Tier>();

  std::shared_ptr<ProtectedInstructions> protected_instructions(
      new ProtectedInstructions(protected_instructions_size));
  DCHECK_EQ(protected_instructions_size, protected_instructions->size());

  Vector<const byte> code_buffer = reader.GetSubvector(code_size);
  std::unique_ptr<byte[]> reloc_info;
  if (reloc_size > 0) {
    reloc_info.reset(new byte[reloc_size]);
    reader.ReadIntoVector({reloc_info.get(), reloc_size});
  }
  std::unique_ptr<byte[]> source_pos;
  if (source_position_size > 0) {
    source_pos.reset(new byte[source_position_size]);
    reader.ReadIntoVector({source_pos.get(), source_position_size});
  }
  WasmCode* ret = native_module_->AddOwnedCode(
      code_buffer, std::move(reloc_info), reloc_size, std::move(source_pos),
      source_position_size, Just(index_), WasmCode::kFunction,
      constant_pool_offset, stack_slot_count, safepoint_table_offset,
      handler_table_offset, protected_instructions, tier,
      WasmCode::kNoFlushICache);
  native_module_->code_table_[index_] = ret;

  // now relocate the code
  int mask = RelocInfo::ModeMask(RelocInfo::EMBEDDED_OBJECT) |
             RelocInfo::ModeMask(RelocInfo::CODE_TARGET) |
             RelocInfo::ModeMask(RelocInfo::RUNTIME_ENTRY);
  for (RelocIterator iter(ret->instructions(), ret->reloc_info(),
                          ret->constant_pool(), mask);
       !iter.done(); iter.next()) {
    RelocInfo::Mode mode = iter.rinfo()->rmode();
    switch (mode) {
      case RelocInfo::EMBEDDED_OBJECT: {
        // We only expect {undefined}. We check for that when we add code.
        iter.rinfo()->set_target_object(isolate_->heap()->undefined_value(),
                                        SKIP_WRITE_BARRIER, SKIP_ICACHE_FLUSH);
        break;
      }
      case RelocInfo::CODE_TARGET: {
        uint32_t tag = GetWasmCalleeTag(iter.rinfo());
        Address target = GetTrampolineOrStubFromTag(tag);
        iter.rinfo()->set_target_address(target, SKIP_WRITE_BARRIER,
                                         SKIP_ICACHE_FLUSH);
        break;
      }
      case RelocInfo::RUNTIME_ENTRY: {
        uint32_t tag = GetWasmCalleeTag(iter.rinfo());
        Address address =
            isolate_->heap()->external_reference_table()->address(tag);
        iter.rinfo()->set_target_runtime_entry(address, SKIP_WRITE_BARRIER,
                                               SKIP_ICACHE_FLUSH);
        break;
      }
      default:
        break;
    }
  }
  // Flush the i-cache here instead of in AddOwnedCode, to include the changes
  // made while iterating over the RelocInfo above.
  Assembler::FlushICache(ret->instructions().start(),
                         ret->instructions().size());

  if (protected_instructions_size > 0) {
    reader.ReadIntoVector(
        {reinterpret_cast<byte*>(protected_instructions->data()),
         sizeof(trap_handler::ProtectedInstructionData) *
             protected_instructions->size()});
  }
  unread_ = unread_ + (start_size - reader.current_buffer().size());
  return true;
}

Address NativeModuleDeserializer::GetTrampolineOrStubFromTag(uint32_t tag) {
  if ((tag & 0x0000FFFF) == 0) {
    int builtin_id = static_cast<int>(tag >> 16);
    v8::internal::Code* builtin = isolate_->builtins()->builtin(builtin_id);
    return native_module_->GetLocalAddressFor(handle(builtin));
  } else {
    DCHECK_EQ(tag & 0xFFFF0000, 0);
    return stubs_[tag - kFirstStubId];
  }
}

MaybeHandle<WasmCompiledModule> DeserializeNativeModule(
    Isolate* isolate, Vector<const byte> data, Vector<const byte> wire_bytes) {
  if (!IsWasmCodegenAllowed(isolate, isolate->native_context())) {
    return {};
  }
  if (!IsSupportedVersion(isolate, data)) {
    return {};
  }
  data = data + kVersionSize;
  ModuleResult decode_result =
      SyncDecodeWasmModule(isolate, wire_bytes.start(), wire_bytes.end(), false,
                           i::wasm::kWasmOrigin);
  if (!decode_result.ok()) return {};
  CHECK_NOT_NULL(decode_result.val);
  Handle<String> module_bytes =
      isolate->factory()
          ->NewStringFromOneByte(
              {wire_bytes.start(), static_cast<size_t>(wire_bytes.length())},
              TENURED)
          .ToHandleChecked();
  DCHECK(module_bytes->IsSeqOneByteString());
  // The {module_wrapper} will take ownership of the {WasmModule} object,
  // and it will be destroyed when the GC reclaims the wrapper object.
  Handle<WasmModuleWrapper> module_wrapper =
      WasmModuleWrapper::From(isolate, decode_result.val.release());
  Handle<Script> script = CreateWasmScript(isolate, wire_bytes);
  Handle<WasmSharedModuleData> shared = WasmSharedModuleData::New(
      isolate, module_wrapper, Handle<SeqOneByteString>::cast(module_bytes),
      script, Handle<ByteArray>::null());
  int export_wrappers_size =
      static_cast<int>(shared->module()->num_exported_functions);
  Handle<FixedArray> export_wrappers = isolate->factory()->NewFixedArray(
      static_cast<int>(export_wrappers_size), TENURED);

  Handle<WasmCompiledModule> compiled_module =
      WasmCompiledModule::New(isolate, shared->module(), export_wrappers,
                              std::vector<wasm::GlobalHandleAddress>(),
                              trap_handler::IsTrapHandlerEnabled());
  compiled_module->set_shared(*shared);
  script->set_wasm_compiled_module(*compiled_module);
  NativeModuleDeserializer deserializer(isolate,
                                        compiled_module->GetNativeModule());
  if (!deserializer.Read(data)) return {};

  // TODO(6792): Wrappers below might be cloned using {Factory::CopyCode}. This
  // requires unlocking the code space here. This should be moved into the
  // allocator eventually.
  CodeSpaceMemoryModificationScope modification_scope(isolate->heap());
  CompileJsToWasmWrappers(isolate, compiled_module, isolate->counters());
  WasmCompiledModule::ReinitializeAfterDeserialization(isolate,
                                                       compiled_module);
  return compiled_module;
}

}  // namespace wasm
}  // namespace internal
}  // namespace v8

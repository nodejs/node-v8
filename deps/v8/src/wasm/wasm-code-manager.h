// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_WASM_WASM_CODE_MANAGER_H_
#define V8_WASM_WASM_CODE_MANAGER_H_

#include <functional>
#include <list>
#include <map>
#include <unordered_map>

#include "src/base/macros.h"
#include "src/handles.h"
#include "src/trap-handler/trap-handler.h"
#include "src/vector.h"
#include "src/wasm/module-compiler.h"

namespace v8 {
class Isolate;
namespace internal {

struct CodeDesc;
class Code;
class WasmCompiledModule;

namespace wasm {

using GlobalHandleAddress = Address;
class NativeModule;
struct WasmModule;

struct AddressHasher {
  size_t operator()(const Address& addr) const {
    return std::hash<intptr_t>()(reinterpret_cast<intptr_t>(addr));
  }
};

// Sorted, disjoint and non-overlapping memory ranges. A range is of the
// form [start, end). So there's no [start, end), [end, other_end),
// because that should have been reduced to [start, other_end).
using AddressRange = std::pair<Address, Address>;
class V8_EXPORT_PRIVATE DisjointAllocationPool final {
 public:
  enum ExtractionMode : bool { kAny = false, kContiguous = true };
  DisjointAllocationPool() {}

  explicit DisjointAllocationPool(Address, Address);

  DisjointAllocationPool(DisjointAllocationPool&& other) = default;
  DisjointAllocationPool& operator=(DisjointAllocationPool&& other) = default;

  // Merge the ranges of the parameter into this object. Ordering is
  // preserved. The assumption is that the passed parameter is
  // not intersecting this object - for example, it was obtained
  // from a previous Allocate{Pool}.
  void Merge(DisjointAllocationPool&&);

  // Allocate a contiguous range of size {size}. Return an empty pool on
  // failure.
  DisjointAllocationPool Allocate(size_t size) {
    return Extract(size, kContiguous);
  }

  // Allocate a sub-pool of size {size}. Return an empty pool on failure.
  DisjointAllocationPool AllocatePool(size_t size) {
    return Extract(size, kAny);
  }

  bool IsEmpty() const { return ranges_.empty(); }
  const std::list<AddressRange>& ranges() const { return ranges_; }

 private:
  // Extract out a total of {size}. By default, the return may
  // be more than one range. If kContiguous is passed, the return
  // will be one range. If the operation fails, this object is
  // unchanged, and the return {IsEmpty()}
  DisjointAllocationPool Extract(size_t size, ExtractionMode mode);

  std::list<AddressRange> ranges_;

  DISALLOW_COPY_AND_ASSIGN(DisjointAllocationPool)
};

using ProtectedInstructions =
    std::vector<trap_handler::ProtectedInstructionData>;

class V8_EXPORT_PRIVATE WasmCode final {
 public:
  enum Kind {
    kFunction,
    kWasmToWasmWrapper,
    kWasmToJsWrapper,
    kLazyStub,
    kInterpreterStub,
    kCopiedStub,
    kTrampoline
  };

  // kOther is used if we have WasmCode that is neither
  // liftoff- nor turbofan-compiled, i.e. if Kind is
  // not a kFunction.
  enum Tier : int8_t { kLiftoff, kTurbofan, kOther };

  Vector<byte> instructions() const { return instructions_; }
  Vector<const byte> reloc_info() const {
    return {reloc_info_.get(), reloc_size_};
  }
  Vector<const byte> source_positions() const {
    return {source_position_table_.get(), source_position_size_};
  }

  uint32_t index() const { return index_.ToChecked(); }
  // Anonymous functions are functions that don't carry an index, like
  // trampolines.
  bool IsAnonymous() const { return index_.IsNothing(); }
  Kind kind() const { return kind_; }
  NativeModule* native_module() const { return native_module_; }
  Tier tier() const { return tier_; }
  Address constant_pool() const;
  size_t constant_pool_offset() const { return constant_pool_offset_; }
  size_t safepoint_table_offset() const { return safepoint_table_offset_; }
  size_t handler_table_offset() const { return handler_table_offset_; }
  uint32_t stack_slots() const { return stack_slots_; }
  bool is_liftoff() const { return tier_ == kLiftoff; }

  size_t trap_handler_index() const;
  void set_trap_handler_index(size_t);
  bool HasTrapHandlerIndex() const;
  void ResetTrapHandlerIndex();

  const ProtectedInstructions& protected_instructions() const {
    // TODO(mstarzinger): Code that doesn't have trapping instruction should
    // not be required to have this vector, make it possible to be null.
    DCHECK_NOT_NULL(protected_instructions_);
    return *protected_instructions_.get();
  }

  void Print(Isolate* isolate) const;
  void Disassemble(const char* name, Isolate* isolate, std::ostream& os) const;

  static bool ShouldBeLogged(Isolate* isolate);
  void LogCode(Isolate* isolate) const;

  ~WasmCode();

  enum FlushICache : bool { kFlushICache = true, kNoFlushICache = false };

 private:
  friend class NativeModule;

  WasmCode(Vector<byte> instructions,
           std::unique_ptr<const byte[]>&& reloc_info, size_t reloc_size,
           std::unique_ptr<const byte[]>&& source_pos, size_t source_pos_size,
           NativeModule* native_module, Maybe<uint32_t> index, Kind kind,
           size_t constant_pool_offset, uint32_t stack_slots,
           size_t safepoint_table_offset, size_t handler_table_offset,
           std::shared_ptr<ProtectedInstructions> protected_instructions,
           Tier tier)
      : instructions_(instructions),
        reloc_info_(std::move(reloc_info)),
        reloc_size_(reloc_size),
        source_position_table_(std::move(source_pos)),
        source_position_size_(source_pos_size),
        native_module_(native_module),
        index_(index),
        kind_(kind),
        constant_pool_offset_(constant_pool_offset),
        stack_slots_(stack_slots),
        safepoint_table_offset_(safepoint_table_offset),
        handler_table_offset_(handler_table_offset),
        protected_instructions_(std::move(protected_instructions)),
        tier_(tier) {
    DCHECK_LE(safepoint_table_offset, instructions.size());
    DCHECK_LE(constant_pool_offset, instructions.size());
    DCHECK_LE(handler_table_offset, instructions.size());
  }

  Vector<byte> instructions_;
  std::unique_ptr<const byte[]> reloc_info_;
  size_t reloc_size_ = 0;
  std::unique_ptr<const byte[]> source_position_table_;
  size_t source_position_size_ = 0;
  NativeModule* native_module_ = nullptr;
  Maybe<uint32_t> index_;
  Kind kind_;
  size_t constant_pool_offset_ = 0;
  uint32_t stack_slots_ = 0;
  // we care about safepoint data for wasm-to-js functions,
  // since there may be stack/register tagged values for large number
  // conversions.
  size_t safepoint_table_offset_ = 0;
  size_t handler_table_offset_ = 0;
  intptr_t trap_handler_index_ = -1;
  std::shared_ptr<ProtectedInstructions> protected_instructions_;
  Tier tier_;

  DISALLOW_COPY_AND_ASSIGN(WasmCode);
};

// Return a textual description of the kind.
const char* GetWasmCodeKindAsString(WasmCode::Kind);

class WasmCodeManager;

// Note that we currently need to add code on the main thread, because we may
// trigger a GC if we believe there's a chance the GC would clear up native
// modules. The code is ready for concurrency otherwise, we just need to be
// careful about this GC consideration. See WouldGCHelp and
// WasmCodeManager::Commit.
class V8_EXPORT_PRIVATE NativeModule final {
 public:
  // Helper class to selectively clone and patch code from a
  // {source_native_module} into a {cloning_native_module}.
  class CloneCodeHelper {
   public:
    explicit CloneCodeHelper(NativeModule* source_native_module,
                             NativeModule* cloning_native_module);

    void SelectForCloning(int32_t code_index);

    void CloneAndPatchCode(bool patch_stub_to_stub_calls);

   private:
    void PatchStubToStubCalls();

    NativeModule* source_native_module_;
    NativeModule* cloning_native_module_;
    std::vector<uint32_t> selection_;
    std::unordered_map<Address, Address, AddressHasher> reverse_lookup_;
  };

  std::unique_ptr<NativeModule> Clone();

  WasmCode* AddCode(const CodeDesc& desc, uint32_t frame_count, uint32_t index,
                    size_t safepoint_table_offset, size_t handler_table_offset,
                    std::unique_ptr<ProtectedInstructions>,
                    Handle<ByteArray> source_position_table,
                    WasmCode::Tier tier);

  // A way to copy over JS-allocated code. This is because we compile
  // certain wrappers using a different pipeline.
  WasmCode* AddCodeCopy(Handle<Code> code, WasmCode::Kind kind, uint32_t index);

  // Add an interpreter wrapper. For the same reason as AddCodeCopy, we
  // currently compile these using a different pipeline and we can't get a
  // CodeDesc here. When adding interpreter wrappers, we do not insert them in
  // the code_table, however, we let them self-identify as the {index} function
  WasmCode* AddInterpreterWrapper(Handle<Code> code, uint32_t index);

  // When starting lazy compilation, provide the WasmLazyCompile builtin by
  // calling SetLazyBuiltin. It will initialize the code table with it. Copies
  // of it might be cloned from them later when creating entries for exported
  // functions and indirect callable functions, so that they may be identified
  // by the runtime.
  void SetLazyBuiltin(Handle<Code> code);

  // FunctionCount is WasmModule::functions.size().
  uint32_t FunctionCount() const;
  WasmCode* GetCode(uint32_t index) const;

  // We special-case lazy cloning because we currently rely on making copies
  // of the lazy builtin, to be able to identify, in the runtime, which function
  // the lazy builtin is a placeholder of. If we used trampolines, we would call
  // the runtime function from a common pc. We could, then, figure who the
  // caller was if the trampolines called rather than jumped to the common
  // builtin. The logic for seeking though frames would change, though.
  // TODO(mtrofin): perhaps we can do exactly that - either before or after
  // this change.
  WasmCode* CloneLazyBuiltinInto(const WasmCode* code, uint32_t index,
                                 WasmCode::FlushICache);

  bool SetExecutable(bool executable);

  // For cctests, where we build both WasmModule and the runtime objects
  // on the fly, and bypass the instance builder pipeline.
  void ResizeCodeTableForTest(size_t);

  CompilationState* compilation_state() { return compilation_state_.get(); }

  // TODO(mstarzinger): needed until we sort out source positions, which are
  // still on the  GC-heap.
  WasmCompiledModule* compiled_module() const;
  void SetCompiledModule(Handle<WasmCompiledModule>);

  uint32_t num_imported_functions() const { return num_imported_functions_; }

  size_t committed_memory() const { return committed_memory_; }
  const size_t instance_id = 0;
  ~NativeModule();

 private:
  friend class WasmCodeManager;
  friend class NativeModuleSerializer;
  friend class NativeModuleDeserializer;
  friend class NativeModuleModificationScope;

  static base::AtomicNumber<size_t> next_id_;
  NativeModule(uint32_t num_functions, uint32_t num_imports,
               bool can_request_more, VirtualMemory* vmem,
               WasmCodeManager* code_manager);

  WasmCode* AddAnonymousCode(Handle<Code>, WasmCode::Kind kind);
  Address AllocateForCode(size_t size);

  // Primitive for adding code to the native module. All code added to a native
  // module is owned by that module. Various callers get to decide on how the
  // code is obtained (CodeDesc vs, as a point in time, Code*), the kind,
  // whether it has an index or is anonymous, etc.
  WasmCode* AddOwnedCode(Vector<const byte> orig_instructions,
                         std::unique_ptr<const byte[]> reloc_info,
                         size_t reloc_size,
                         std::unique_ptr<const byte[]> source_pos,
                         size_t source_pos_size, Maybe<uint32_t> index,
                         WasmCode::Kind kind, size_t constant_pool_offset,
                         uint32_t stack_slots, size_t safepoint_table_offset,
                         size_t handler_table_offset,
                         std::shared_ptr<ProtectedInstructions>, WasmCode::Tier,
                         WasmCode::FlushICache);
  WasmCode* CloneCode(const WasmCode*, WasmCode::FlushICache);
  void CloneTrampolinesAndStubs(const NativeModule* other,
                                WasmCode::FlushICache);
  WasmCode* Lookup(Address);
  Address GetLocalAddressFor(Handle<Code>);
  Address CreateTrampolineTo(Handle<Code>);

  // Holds all allocated code objects, is maintained to be in ascending order
  // according to the codes instruction start address to allow lookups.
  std::vector<std::unique_ptr<WasmCode>> owned_code_;

  std::vector<WasmCode*> code_table_;
  uint32_t num_imported_functions_;

  // Maps from instruction start of an immovable code object to instruction
  // start of the trampoline.
  std::unordered_map<Address, Address, AddressHasher> trampolines_;

  // Maps from stub key to wasm code (containing a copy of that stub).
  std::unordered_map<uint32_t, WasmCode*> stubs_;

  std::unique_ptr<CompilationState, CompilationStateDeleter> compilation_state_;

  DisjointAllocationPool free_memory_;
  DisjointAllocationPool allocated_memory_;
  std::list<VirtualMemory> owned_memory_;
  WasmCodeManager* wasm_code_manager_;
  base::Mutex allocation_mutex_;
  Handle<WasmCompiledModule> compiled_module_;
  size_t committed_memory_ = 0;
  bool can_request_more_memory_;
  bool is_executable_ = false;
  int modification_scope_depth_ = 0;

  DISALLOW_COPY_AND_ASSIGN(NativeModule);
};

class V8_EXPORT_PRIVATE WasmCodeManager final {
 public:
  // The only reason we depend on Isolate is to report native memory used
  // and held by a GC-ed object. We'll need to mitigate that when we
  // start sharing wasm heaps.
  WasmCodeManager(v8::Isolate*, size_t max_committed);
  // Create a new NativeModule. The caller is responsible for its
  // lifetime. The native module will be given some memory for code,
  // which will be page size aligned. The size of the initial memory
  // is determined with a heuristic based on the total size of wasm
  // code. The native module may later request more memory.
  std::unique_ptr<NativeModule> NewNativeModule(const WasmModule&);
  std::unique_ptr<NativeModule> NewNativeModule(size_t memory_estimate,
                                                uint32_t num_functions,
                                                uint32_t num_imported_functions,
                                                bool can_request_more);

  WasmCode* LookupCode(Address pc) const;
  WasmCode* GetCodeFromStartAddress(Address pc) const;
  intptr_t remaining_uncommitted() const;

 private:
  friend class NativeModule;

  void TryAllocate(size_t size, VirtualMemory*, void* hint = nullptr);
  bool Commit(Address, size_t);
  // Currently, we uncommit a whole module, so all we need is account
  // for the freed memory size. We do that in FreeNativeModuleMemories.
  // There's no separate Uncommit.

  void FreeNativeModuleMemories(NativeModule*);
  void Free(VirtualMemory* mem);
  void AssignRanges(void* start, void* end, NativeModule*);
  size_t GetAllocationChunk(const WasmModule& module);
  bool WouldGCHelp() const;

  std::map<Address, std::pair<Address, NativeModule*>> lookup_map_;
  // count of NativeModules not yet collected. Helps determine if it's
  // worth requesting a GC on memory pressure.
  size_t active_ = 0;
  base::AtomicNumber<intptr_t> remaining_uncommitted_;

  // TODO(mtrofin): remove the dependency on isolate.
  v8::Isolate* isolate_;

  DISALLOW_COPY_AND_ASSIGN(WasmCodeManager);
};

// Within the scope, the native_module is writable and not executable.
// At the scope's destruction, the native_module is executable and not writable.
// The states inside the scope and at the scope termination are irrespective of
// native_module's state when entering the scope.
// We currently mark the entire module's memory W^X:
//  - for AOT, that's as efficient as it can be.
//  - for Lazy, we don't have a heuristic for functions that may need patching,
//    and even if we did, the resulting set of pages may be fragmented.
//    Currently, we try and keep the number of syscalls low.
// -  similar argument for debug time.
class NativeModuleModificationScope final {
 public:
  explicit NativeModuleModificationScope(NativeModule* native_module);
  ~NativeModuleModificationScope();

 private:
  NativeModule* native_module_;
};

}  // namespace wasm
}  // namespace internal
}  // namespace v8

#endif  // V8_WASM_WASM_CODE_MANAGER_H_

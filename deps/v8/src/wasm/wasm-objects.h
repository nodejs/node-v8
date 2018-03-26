// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_WASM_WASM_OBJECTS_H_
#define V8_WASM_WASM_OBJECTS_H_

#include "src/base/bits.h"
#include "src/debug/debug.h"
#include "src/debug/interface-types.h"
#include "src/managed.h"
#include "src/objects.h"
#include "src/objects/script.h"
#include "src/wasm/decoder.h"
#include "src/wasm/wasm-interpreter.h"
#include "src/wasm/wasm-limits.h"
#include "src/wasm/wasm-module.h"

#include "src/heap/heap.h"

// Has to be the last include (doesn't have include guards)
#include "src/objects/object-macros.h"

namespace v8 {
namespace internal {
namespace wasm {
class InterpretedFrame;
class NativeModule;
class WasmCode;
struct WasmModule;
class SignatureMap;
typedef Address GlobalHandleAddress;
using ValueType = MachineRepresentation;
using FunctionSig = Signature<ValueType>;
}  // namespace wasm

class WasmCompiledModule;
class WasmDebugInfo;
class WasmInstanceObject;

#define DECL_OPTIONAL_ACCESSORS(name, type) \
  INLINE(bool has_##name());                \
  DECL_ACCESSORS(name, type)

// An entry in an indirect dispatch table.
struct IndirectFunctionTableEntry {
  int32_t sig_id = 0;
  WasmContext* context = nullptr;
  Address target = nullptr;

  MOVE_ONLY_WITH_DEFAULT_CONSTRUCTORS(IndirectFunctionTableEntry)
};

// Wasm context used to store the mem_size and mem_start address of the linear
// memory. These variables can be accessed at C++ level at graph build time
// (e.g., initialized during instance building / changed at runtime by
// grow_memory). The address of the WasmContext is provided to the wasm entry
// functions using a RelocatableIntPtrConstant, then the address is passed as
// parameter to the other wasm functions.
// Note that generated code can directly read from instances of this struct.
struct WasmContext {
  byte* mem_start = nullptr;
  uint32_t mem_size = 0;  // TODO(titzer): uintptr_t?
  uint32_t mem_mask = 0;  // TODO(titzer): uintptr_t?
  byte* globals_start = nullptr;
  // TODO(wasm): pad these entries to a power of two.
  IndirectFunctionTableEntry* table = nullptr;
  uint32_t table_size = 0;

  void SetRawMemory(void* mem_start, size_t mem_size) {
    DCHECK_LE(mem_size, wasm::kV8MaxWasmMemoryPages * wasm::kWasmPageSize);
    this->mem_start = static_cast<byte*>(mem_start);
    this->mem_size = static_cast<uint32_t>(mem_size);
    this->mem_mask = base::bits::RoundUpToPowerOfTwo32(this->mem_size) - 1;
    DCHECK_LE(mem_size, this->mem_mask + 1);
  }

  ~WasmContext() {
    if (table) free(table);
    mem_start = nullptr;
    mem_size = 0;
    mem_mask = 0;
    globals_start = nullptr;
    table = nullptr;
    table_size = 0;
  }
};

// Representation of a WebAssembly.Module JavaScript-level object.
class WasmModuleObject : public JSObject {
 public:
  DECL_CAST(WasmModuleObject)

  // Shared compiled code between multiple WebAssembly.Module objects.
  DECL_ACCESSORS(compiled_module, WasmCompiledModule)

// Layout description.
#define WASM_MODULE_OBJECT_FIELDS(V)     \
  V(kCompiledModuleOffset, kPointerSize) \
  V(kSize, 0)

  DEFINE_FIELD_OFFSET_CONSTANTS(JSObject::kHeaderSize,
                                WASM_MODULE_OBJECT_FIELDS)
#undef WASM_MODULE_OBJECT_FIELDS

  static Handle<WasmModuleObject> New(
      Isolate* isolate, Handle<WasmCompiledModule> compiled_module);

  static void ValidateStateForTesting(Isolate* isolate,
                                      Handle<WasmModuleObject> module);
};

// Representation of a WebAssembly.Table JavaScript-level object.
class WasmTableObject : public JSObject {
 public:
  DECL_CAST(WasmTableObject)

  DECL_ACCESSORS(functions, FixedArray)
  // TODO(titzer): introduce DECL_I64_ACCESSORS macro
  DECL_ACCESSORS(maximum_length, Object)
  DECL_ACCESSORS(dispatch_tables, FixedArray)

// Layout description.
#define WASM_TABLE_OBJECT_FIELDS(V)      \
  V(kFunctionsOffset, kPointerSize)      \
  V(kMaximumLengthOffset, kPointerSize)  \
  V(kDispatchTablesOffset, kPointerSize) \
  V(kSize, 0)

  DEFINE_FIELD_OFFSET_CONSTANTS(JSObject::kHeaderSize, WASM_TABLE_OBJECT_FIELDS)
#undef WASM_TABLE_OBJECT_FIELDS

  inline uint32_t current_length();
  void Grow(Isolate* isolate, uint32_t count);

  static Handle<WasmTableObject> New(Isolate* isolate, uint32_t initial,
                                     int64_t maximum,
                                     Handle<FixedArray>* js_functions);
  static void AddDispatchTable(Isolate* isolate, Handle<WasmTableObject> table,
                               Handle<WasmInstanceObject> instance,
                               int table_index,
                               Handle<FixedArray> function_table);

  static void Set(Isolate* isolate, Handle<WasmTableObject> table,
                  int32_t index, Handle<JSFunction> function);

  static void UpdateDispatchTables(Isolate* isolate,
                                   Handle<WasmTableObject> table,
                                   int table_index, wasm::FunctionSig* sig,
                                   Handle<WasmInstanceObject> from_instance,
                                   wasm::WasmCode* wasm_code, int func_index);

  static void ClearDispatchTables(Handle<WasmTableObject> table, int index);
};

// Representation of a WebAssembly.Memory JavaScript-level object.
class WasmMemoryObject : public JSObject {
 public:
  DECL_CAST(WasmMemoryObject)

  DECL_ACCESSORS(array_buffer, JSArrayBuffer)
  DECL_INT_ACCESSORS(maximum_pages)
  DECL_OPTIONAL_ACCESSORS(instances, FixedArrayOfWeakCells)
  DECL_ACCESSORS(wasm_context, Managed<WasmContext>)

// Layout description.
#define WASM_MEMORY_OBJECT_FIELDS(V)   \
  V(kArrayBufferOffset, kPointerSize)  \
  V(kMaximumPagesOffset, kPointerSize) \
  V(kInstancesOffset, kPointerSize)    \
  V(kWasmContextOffset, kPointerSize)  \
  V(kSize, 0)

  DEFINE_FIELD_OFFSET_CONSTANTS(JSObject::kHeaderSize,
                                WASM_MEMORY_OBJECT_FIELDS)
#undef WASM_MEMORY_OBJECT_FIELDS

  // Add an instance to the internal (weak) list. amortized O(n).
  static void AddInstance(Isolate* isolate, Handle<WasmMemoryObject> memory,
                          Handle<WasmInstanceObject> object);
  // Remove an instance from the internal (weak) list. O(n).
  static void RemoveInstance(Isolate* isolate, Handle<WasmMemoryObject> memory,
                             Handle<WasmInstanceObject> object);
  uint32_t current_pages();
  inline bool has_maximum_pages();

  V8_EXPORT_PRIVATE static Handle<WasmMemoryObject> New(
      Isolate* isolate, MaybeHandle<JSArrayBuffer> buffer, int32_t maximum);

  static int32_t Grow(Isolate*, Handle<WasmMemoryObject>, uint32_t pages);
};

// A WebAssembly.Instance JavaScript-level object.
class WasmInstanceObject : public JSObject {
 public:
  DECL_CAST(WasmInstanceObject)

  DECL_ACCESSORS(wasm_context, Managed<WasmContext>)
  DECL_ACCESSORS(compiled_module, WasmCompiledModule)
  DECL_ACCESSORS(exports_object, JSObject)
  DECL_OPTIONAL_ACCESSORS(memory_object, WasmMemoryObject)
  DECL_OPTIONAL_ACCESSORS(globals_buffer, JSArrayBuffer)
  DECL_OPTIONAL_ACCESSORS(debug_info, WasmDebugInfo)
  DECL_OPTIONAL_ACCESSORS(table_object, WasmTableObject)
  DECL_OPTIONAL_ACCESSORS(function_tables, FixedArray)
  DECL_PRIMITIVE_ACCESSORS(memory_start, byte*)
  DECL_PRIMITIVE_ACCESSORS(memory_size, uintptr_t)
  DECL_PRIMITIVE_ACCESSORS(memory_mask, uintptr_t)
  DECL_PRIMITIVE_ACCESSORS(globals_start, byte*)
  DECL_PRIMITIVE_ACCESSORS(indirect_function_table, IndirectFunctionTableEntry*)
  DECL_PRIMITIVE_ACCESSORS(indirect_function_table_size, uintptr_t)

  // FixedArray of all instances whose code was imported
  DECL_ACCESSORS(directly_called_instances, FixedArray)
  DECL_ACCESSORS(js_imports_table, FixedArray)

// Layout description.
#define WASM_INSTANCE_OBJECT_FIELDS(V)                             \
  V(kWasmContextOffset, kPointerSize)                              \
  V(kCompiledModuleOffset, kPointerSize)                           \
  V(kExportsObjectOffset, kPointerSize)                            \
  V(kMemoryObjectOffset, kPointerSize)                             \
  V(kGlobalsBufferOffset, kPointerSize)                            \
  V(kDebugInfoOffset, kPointerSize)                                \
  V(kTableObjectOffset, kPointerSize)                              \
  V(kFunctionTablesOffset, kPointerSize)                           \
  V(kDirectlyCalledInstancesOffset, kPointerSize)                  \
  V(kJsImportsTableOffset, kPointerSize)                           \
  V(kFirstUntaggedOffset, 0)                        /* marker */   \
  V(kMemoryStartOffset, kPointerSize)               /* untagged */ \
  V(kMemorySizeOffset, kPointerSize)                /* untagged */ \
  V(kMemoryMaskOffset, kPointerSize)                /* untagged */ \
  V(kGlobalsStartOffset, kPointerSize)              /* untagged */ \
  V(kIndirectFunctionTableOffset, kPointerSize)     /* untagged */ \
  V(kIndirectFunctionTableSizeOffset, kPointerSize) /* untagged */ \
  V(kSize, 0)

  DEFINE_FIELD_OFFSET_CONSTANTS(JSObject::kHeaderSize,
                                WASM_INSTANCE_OBJECT_FIELDS)
#undef WASM_INSTANCE_OBJECT_FIELDS

  WasmModuleObject* module_object();
  V8_EXPORT_PRIVATE wasm::WasmModule* module();

  bool EnsureIndirectFunctionTableWithMinimumSize(size_t minimum_size);

  IndirectFunctionTableEntry* indirect_function_table_entry_at(int index);

  void SetRawMemory(byte* mem_start, size_t mem_size);

  // Get the debug info associated with the given wasm object.
  // If no debug info exists yet, it is created automatically.
  static Handle<WasmDebugInfo> GetOrCreateDebugInfo(Handle<WasmInstanceObject>);

  static Handle<WasmInstanceObject> New(Isolate*, Handle<WasmCompiledModule>);

  // Assumed to be called with a code object associated to a wasm module
  // instance. Intended to be called from runtime functions. Returns nullptr on
  // failing to get owning instance.
  static WasmInstanceObject* GetOwningInstance(const wasm::WasmCode* code);

  static void ValidateInstancesChainForTesting(
      Isolate* isolate, Handle<WasmModuleObject> module_obj,
      int instance_count);

  static void ValidateOrphanedInstanceForTesting(
      Isolate* isolate, Handle<WasmInstanceObject> instance);

  static void InstallFinalizer(Isolate* isolate,
                               Handle<WasmInstanceObject> instance);

  // Iterates all fields in the object except the untagged fields.
  class BodyDescriptor;
  // No weak fields.
  typedef BodyDescriptor BodyDescriptorWeak;
};

// A WASM function that is wrapped and exported to JavaScript.
class WasmExportedFunction : public JSFunction {
 public:
  WasmInstanceObject* instance();
  V8_EXPORT_PRIVATE int function_index();

  V8_EXPORT_PRIVATE static WasmExportedFunction* cast(Object* object);
  static bool IsWasmExportedFunction(Object* object);

  static Handle<WasmExportedFunction> New(Isolate* isolate,
                                          Handle<WasmInstanceObject> instance,
                                          MaybeHandle<String> maybe_name,
                                          int func_index, int arity,
                                          Handle<Code> export_wrapper);

  wasm::WasmCode* GetWasmCode();
};

// Information shared by all WasmCompiledModule objects for the same module.
class WasmSharedModuleData : public Struct {
 public:
  DECL_ACCESSORS(module_wrapper, Object)
  wasm::WasmModule* module() const;
  DECL_ACCESSORS(module_bytes, SeqOneByteString)
  DECL_ACCESSORS(script, Script)
  DECL_OPTIONAL_ACCESSORS(asm_js_offset_table, ByteArray)
  DECL_OPTIONAL_ACCESSORS(breakpoint_infos, FixedArray)
  inline void reset_breakpoint_infos();

  DECL_CAST(WasmSharedModuleData)

  // Dispatched behavior.
  DECL_PRINTER(WasmSharedModuleData)
  DECL_VERIFIER(WasmSharedModuleData)

// Layout description.
#define WASM_SHARED_MODULE_DATA_FIELDS(V)             \
  V(kModuleWrapperOffset, kPointerSize)               \
  V(kModuleBytesOffset, kPointerSize)                 \
  V(kScriptOffset, kPointerSize)                      \
  V(kAsmJsOffsetTableOffset, kPointerSize)            \
  V(kBreakPointInfosOffset, kPointerSize)             \
  V(kLazyCompilationOrchestratorOffset, kPointerSize) \
  V(kSize, 0)

  DEFINE_FIELD_OFFSET_CONSTANTS(HeapObject::kHeaderSize,
                                WASM_SHARED_MODULE_DATA_FIELDS)
#undef WASM_SHARED_MODULE_DATA_FIELDS

  // Check whether this module was generated from asm.js source.
  bool is_asm_js();

  static void AddBreakpoint(Handle<WasmSharedModuleData>, int position,
                            Handle<BreakPoint> break_point);

  static void SetBreakpointsOnNewInstance(Handle<WasmSharedModuleData>,
                                          Handle<WasmInstanceObject>);

  static void PrepareForLazyCompilation(Handle<WasmSharedModuleData>);

  static Handle<WasmSharedModuleData> New(
      Isolate* isolate, Handle<Foreign> module_wrapper,
      Handle<SeqOneByteString> module_bytes, Handle<Script> script,
      Handle<ByteArray> asm_js_offset_table);

  // Get the module name, if set. Returns an empty handle otherwise.
  static MaybeHandle<String> GetModuleNameOrNull(Isolate*,
                                                 Handle<WasmSharedModuleData>);

  // Get the function name of the function identified by the given index.
  // Returns a null handle if the function is unnamed or the name is not a valid
  // UTF-8 string.
  static MaybeHandle<String> GetFunctionNameOrNull(Isolate*,
                                                   Handle<WasmSharedModuleData>,
                                                   uint32_t func_index);

  // Get the function name of the function identified by the given index.
  // Returns "<WASM UNNAMED>" if the function is unnamed or the name is not a
  // valid UTF-8 string.
  static Handle<String> GetFunctionName(Isolate*, Handle<WasmSharedModuleData>,
                                        uint32_t func_index);

  // Get the raw bytes of the function name of the function identified by the
  // given index.
  // Meant to be used for debugging or frame printing.
  // Does not allocate, hence gc-safe.
  Vector<const uint8_t> GetRawFunctionName(uint32_t func_index);

  // Return the byte offset of the function identified by the given index.
  // The offset will be relative to the start of the module bytes.
  // Returns -1 if the function index is invalid.
  int GetFunctionOffset(uint32_t func_index);

  // Returns the function containing the given byte offset.
  // Returns -1 if the byte offset is not contained in any function of this
  // module.
  int GetContainingFunction(uint32_t byte_offset);

  // Translate from byte offset in the module to function number and byte offset
  // within that function, encoded as line and column in the position info.
  // Returns true if the position is valid inside this module, false otherwise.
  bool GetPositionInfo(uint32_t position, Script::PositionInfo* info);

  // Get the source position from a given function index and byte offset,
  // for either asm.js or pure WASM modules.
  static int GetSourcePosition(Handle<WasmSharedModuleData>,
                               uint32_t func_index, uint32_t byte_offset,
                               bool is_at_number_conversion);

  // Compute the disassembly of a wasm function.
  // Returns the disassembly string and a list of <byte_offset, line, column>
  // entries, mapping wasm byte offsets to line and column in the disassembly.
  // The list is guaranteed to be ordered by the byte_offset.
  // Returns an empty string and empty vector if the function index is invalid.
  debug::WasmDisassembly DisassembleFunction(int func_index);

  // Extract a portion of the wire bytes as UTF-8 string.
  // Returns a null handle if the respective bytes do not form a valid UTF-8
  // string.
  static MaybeHandle<String> ExtractUtf8StringFromModuleBytes(
      Isolate* isolate, Handle<WasmSharedModuleData>, wasm::WireBytesRef ref);
  static MaybeHandle<String> ExtractUtf8StringFromModuleBytes(
      Isolate* isolate, Handle<SeqOneByteString> module_bytes,
      wasm::WireBytesRef ref);

  // Get a list of all possible breakpoints within a given range of this module.
  bool GetPossibleBreakpoints(const debug::Location& start,
                              const debug::Location& end,
                              std::vector<debug::BreakLocation>* locations);

  // Return an empty handle if no breakpoint is hit at that location, or a
  // FixedArray with all hit breakpoint objects.
  static MaybeHandle<FixedArray> CheckBreakPoints(Isolate*,
                                                  Handle<WasmSharedModuleData>,
                                                  int position);

  DECL_OPTIONAL_ACCESSORS(lazy_compilation_orchestrator, Foreign)
};

// This represents the set of wasm compiled functions, together
// with all the information necessary for re-specializing them.
//
// We specialize wasm functions to their instance by embedding:
//   - raw pointer to the wasm_context, that contains the size of the
//     memory and the pointer to the backing store of the array buffer
//     used as memory of a particular WebAssembly.Instance object. This
//     information are then used at runtime to access memory / verify bounds
//     check limits.
//   - the objects representing the function tables and signature tables
//
// Even without instantiating, we need values for all of these parameters.
// We need to track these values to be able to create new instances and
// to be able to serialize/deserialize.
// The design decisions for how we track these values is not too immediate,
// and it deserves a summary. The "tricky" ones are: memory, globals, and
// the tables (signature and functions).
// For tables, we need to hold a reference to the JS Heap object, because
// we embed them as objects, and they may move.
class WasmCompiledModule : public Struct {
 public:
  DECL_CAST(WasmCompiledModule)

  // Dispatched behavior.
  DECL_PRINTER(WasmCompiledModule)
  DECL_VERIFIER(WasmCompiledModule)

// Layout description.
#define WASM_COMPILED_MODULE_FIELDS(V)          \
  V(kSharedOffset, kPointerSize)                \
  V(kNativeContextOffset, kPointerSize)         \
  V(kExportWrappersOffset, kPointerSize)        \
  V(kWeakExportedFunctionsOffset, kPointerSize) \
  V(kNextInstanceOffset, kPointerSize)          \
  V(kPrevInstanceOffset, kPointerSize)          \
  V(kOwningInstanceOffset, kPointerSize)        \
  V(kWasmModuleOffset, kPointerSize)            \
  V(kNativeModuleOffset, kPointerSize)          \
  V(kLazyCompileDataOffset, kPointerSize)       \
  V(kUseTrapHandlerOffset, kPointerSize)        \
  V(kSize, 0)

  DEFINE_FIELD_OFFSET_CONSTANTS(HeapObject::kHeaderSize,
                                WASM_COMPILED_MODULE_FIELDS)
#undef WASM_COMPILED_MODULE_FIELDS

#define WCM_OBJECT_OR_WEAK(TYPE, NAME, SETTER_MODIFIER) \
 public:                                                \
  inline TYPE* NAME() const;                            \
  inline bool has_##NAME() const;                       \
  inline void reset_##NAME();                           \
                                                        \
  SETTER_MODIFIER:                                      \
  inline void set_##NAME(TYPE* value,                   \
                         WriteBarrierMode mode = UPDATE_WRITE_BARRIER);

#define WCM_OBJECT(TYPE, NAME) WCM_OBJECT_OR_WEAK(TYPE, NAME, public)

#define WCM_CONST_OBJECT(TYPE, NAME) WCM_OBJECT_OR_WEAK(TYPE, NAME, private)

#define WCM_SMALL_CONST_NUMBER(TYPE, NAME) \
 public:                                   \
  inline TYPE NAME() const;                \
                                           \
 private:                                  \
  inline void set_##NAME(TYPE value);

#define WCM_WEAK_LINK(TYPE, NAME)                   \
  WCM_OBJECT_OR_WEAK(WeakCell, weak_##NAME, public) \
                                                    \
 public:                                            \
  inline TYPE* NAME() const;

  // Add values here if they are required for creating new instances or
  // for deserialization, and if they are serializable.
  // By default, instance values go to WasmInstanceObject, however, if
  // we embed the generated code with a value, then we track that value here.
  WCM_OBJECT(WasmSharedModuleData, shared)
  WCM_WEAK_LINK(Context, native_context)
  WCM_CONST_OBJECT(FixedArray, export_wrappers)
  WCM_OBJECT(FixedArray, weak_exported_functions)
  WCM_CONST_OBJECT(WasmCompiledModule, next_instance)
  WCM_CONST_OBJECT(WasmCompiledModule, prev_instance)
  WCM_WEAK_LINK(WasmInstanceObject, owning_instance)
  WCM_WEAK_LINK(WasmModuleObject, wasm_module)
  WCM_OBJECT(Foreign, native_module)
  WCM_OBJECT(FixedArray, lazy_compile_data)
  // TODO(mstarzinger): Make {use_trap_handler} smaller.
  WCM_SMALL_CONST_NUMBER(bool, use_trap_handler)

 public:
  static Handle<WasmCompiledModule> New(
      Isolate* isolate, wasm::WasmModule* module,
      Handle<FixedArray> export_wrappers,
      const std::vector<wasm::GlobalHandleAddress>& function_tables,
      bool use_trap_hander);

  static Handle<WasmCompiledModule> Clone(Isolate* isolate,
                                          Handle<WasmCompiledModule> module);
  static void Reset(Isolate* isolate, WasmCompiledModule* module);

  wasm::NativeModule* GetNativeModule() const;
  void InsertInChain(WasmModuleObject*);
  void RemoveFromChain();

  DECL_ACCESSORS(raw_next_instance, Object);
  DECL_ACCESSORS(raw_prev_instance, Object);

  void PrintInstancesChain();

  static void ReinitializeAfterDeserialization(Isolate*,
                                               Handle<WasmCompiledModule>);

  // Set a breakpoint on the given byte position inside the given module.
  // This will affect all live and future instances of the module.
  // The passed position might be modified to point to the next breakable
  // location inside the same function.
  // If it points outside a function, or behind the last breakable location,
  // this function returns false and does not set any breakpoint.
  static bool SetBreakPoint(Handle<WasmCompiledModule>, int* position,
                            Handle<BreakPoint> break_point);

  void LogWasmCodes(Isolate* isolate);

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(WasmCompiledModule);
};

class WasmDebugInfo : public Struct {
 public:
  DECL_ACCESSORS(wasm_instance, WasmInstanceObject)
  DECL_ACCESSORS(interpreter_handle, Object);
  DECL_ACCESSORS(interpreted_functions, Object);
  DECL_OPTIONAL_ACCESSORS(locals_names, FixedArray)
  DECL_OPTIONAL_ACCESSORS(c_wasm_entries, FixedArray)
  DECL_OPTIONAL_ACCESSORS(c_wasm_entry_map, Managed<wasm::SignatureMap>)

  DECL_CAST(WasmDebugInfo)

  // Dispatched behavior.
  DECL_PRINTER(WasmDebugInfo)
  DECL_VERIFIER(WasmDebugInfo)

// Layout description.
#define WASM_DEBUG_INFO_FIELDS(V)              \
  V(kInstanceOffset, kPointerSize)             \
  V(kInterpreterHandleOffset, kPointerSize)    \
  V(kInterpretedFunctionsOffset, kPointerSize) \
  V(kLocalsNamesOffset, kPointerSize)          \
  V(kCWasmEntriesOffset, kPointerSize)         \
  V(kCWasmEntryMapOffset, kPointerSize)        \
  V(kSize, 0)

  DEFINE_FIELD_OFFSET_CONSTANTS(HeapObject::kHeaderSize, WASM_DEBUG_INFO_FIELDS)
#undef WASM_DEBUG_INFO_FIELDS

  static Handle<WasmDebugInfo> New(Handle<WasmInstanceObject>);

  // Setup a WasmDebugInfo with an existing WasmInstance struct.
  // Returns a pointer to the interpreter instantiated inside this
  // WasmDebugInfo.
  // Use for testing only.
  V8_EXPORT_PRIVATE static wasm::WasmInterpreter* SetupForTesting(
      Handle<WasmInstanceObject>);

  // Set a breakpoint in the given function at the given byte offset within that
  // function. This will redirect all future calls to this function to the
  // interpreter and will always pause at the given offset.
  static void SetBreakpoint(Handle<WasmDebugInfo>, int func_index, int offset);

  // Make a set of functions always execute in the interpreter without setting
  // breakpoints.
  static void RedirectToInterpreter(Handle<WasmDebugInfo>,
                                    Vector<int> func_indexes);

  void PrepareStep(StepAction);

  // Execute the specified function in the interpreter. Read arguments from
  // arg_buffer.
  // The frame_pointer will be used to identify the new activation of the
  // interpreter for unwinding and frame inspection.
  // Returns true if exited regularly, false if a trap occurred. In the latter
  // case, a pending exception will have been set on the isolate.
  bool RunInterpreter(Address frame_pointer, int func_index,
                      uint8_t* arg_buffer);

  // Get the stack of the wasm interpreter as pairs of <function index, byte
  // offset>. The list is ordered bottom-to-top, i.e. caller before callee.
  std::vector<std::pair<uint32_t, int>> GetInterpretedStack(
      Address frame_pointer);

  wasm::WasmInterpreter::FramePtr GetInterpretedFrame(Address frame_pointer,
                                                      int frame_index);

  // Unwind the interpreted stack belonging to the passed interpreter entry
  // frame.
  void Unwind(Address frame_pointer);

  // Returns the number of calls / function frames executed in the interpreter.
  uint64_t NumInterpretedCalls();

  // Get scope details for a specific interpreted frame.
  // This returns a JSArray of length two: One entry for the global scope, one
  // for the local scope. Both elements are JSArrays of size
  // ScopeIterator::kScopeDetailsSize and layout as described in debug-scopes.h.
  // The global scope contains information about globals and the memory.
  // The local scope contains information about parameters, locals, and stack
  // values.
  static Handle<JSObject> GetScopeDetails(Handle<WasmDebugInfo>,
                                          Address frame_pointer,
                                          int frame_index);
  static Handle<JSObject> GetGlobalScopeObject(Handle<WasmDebugInfo>,
                                               Address frame_pointer,
                                               int frame_index);
  static Handle<JSObject> GetLocalScopeObject(Handle<WasmDebugInfo>,
                                              Address frame_pointer,
                                              int frame_index);

  static Handle<JSFunction> GetCWasmEntry(Handle<WasmDebugInfo>,
                                          wasm::FunctionSig*);
};

#undef DECL_OPTIONAL_ACCESSORS
#undef WCM_CONST_OBJECT
#undef WCM_LARGE_NUMBER
#undef WCM_OBJECT
#undef WCM_OBJECT_OR_WEAK
#undef WCM_SMALL_CONST_NUMBER
#undef WCM_WEAK_LINK

#include "src/objects/object-macros-undef.h"

}  // namespace internal
}  // namespace v8

#endif  // V8_WASM_WASM_OBJECTS_H_

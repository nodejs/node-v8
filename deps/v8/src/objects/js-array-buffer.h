// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_OBJECTS_JS_ARRAY_BUFFER_H_
#define V8_OBJECTS_JS_ARRAY_BUFFER_H_

#include "src/objects/backing-store.h"
#include "src/objects/js-objects.h"
#include "torque-generated/bit-fields-tq.h"

// Has to be the last include (doesn't have include guards):
#include "src/objects/object-macros.h"

namespace v8 {
namespace internal {

class ArrayBufferExtension;

class JSArrayBuffer : public JSObject {
 public:
// The maximum length for JSArrayBuffer's supported by V8.
// On 32-bit architectures we limit this to 2GiB, so that
// we can continue to use CheckBounds with the Unsigned31
// restriction for the length.
#if V8_HOST_ARCH_32_BIT
  static constexpr size_t kMaxByteLength = kMaxInt;
#else
  static constexpr size_t kMaxByteLength = kMaxSafeInteger;
#endif

  // [byte_length]: length in bytes
  DECL_PRIMITIVE_ACCESSORS(byte_length, size_t)

  // [backing_store]: backing memory for this array
  DECL_PRIMITIVE_ACCESSORS(backing_store, void*)

  // [extension]: extension object used for GC
  DECL_PRIMITIVE_ACCESSORS(extension, ArrayBufferExtension*)

  // For non-wasm, allocation_length and allocation_base are byte_length and
  // backing_store, respectively.
  inline size_t allocation_length() const;
  inline void* allocation_base() const;

  // [bit_field]: boolean flags
  DECL_PRIMITIVE_ACCESSORS(bit_field, uint32_t)

  // Clear uninitialized padding space. This ensures that the snapshot content
  // is deterministic. Depending on the V8 build mode there could be no padding.
  V8_INLINE void clear_padding();

// Bit positions for [bit_field].
  DEFINE_TORQUE_GENERATED_JS_ARRAY_BUFFER_FLAGS()

  // [is_external]: true indicates that the embedder is in charge of freeing the
  // backing_store, while is_external == false means that v8 will free the
  // memory block once all ArrayBuffers referencing it are collected by the GC.
  DECL_BOOLEAN_ACCESSORS(is_external)

  // [is_detachable]: false => this buffer cannot be detached.
  DECL_BOOLEAN_ACCESSORS(is_detachable)

  // [was_detached]: true => the buffer was previously detached.
  DECL_BOOLEAN_ACCESSORS(was_detached)

  // [is_asmjs_memory]: true => this buffer was once used as asm.js memory.
  DECL_BOOLEAN_ACCESSORS(is_asmjs_memory)

  // [is_shared]: tells whether this is an ArrayBuffer or a SharedArrayBuffer.
  DECL_BOOLEAN_ACCESSORS(is_shared)

  DECL_CAST(JSArrayBuffer)

  // Initializes the fields of the ArrayBuffer. The provided backing_store can
  // be nullptr. If it is not nullptr, then the function registers it with
  // src/heap/array-buffer-tracker.h.
  V8_EXPORT_PRIVATE void Setup(SharedFlag shared,
                               std::shared_ptr<BackingStore> backing_store);

  // Attaches the backing store to an already constructed empty ArrayBuffer.
  // This is intended to be used only in ArrayBufferConstructor builtin.
  V8_EXPORT_PRIVATE void Attach(std::shared_ptr<BackingStore> backing_store);
  // Detach the backing store from this array buffer if it is detachable.
  // This sets the internal pointer and length to 0 and unregisters the backing
  // store from the array buffer tracker. If the array buffer is not detachable,
  // this is a nop.
  //
  // Array buffers that wrap wasm memory objects are special in that they
  // are normally not detachable, but can become detached as a side effect
  // of growing the underlying memory object. The {force_for_wasm_memory} flag
  // is used by the implementation of Wasm memory growth in order to bypass the
  // non-detachable check.
  V8_EXPORT_PRIVATE void Detach(bool force_for_wasm_memory = false);

  // Get a reference to backing store of this array buffer, if there is a
  // backing store. Returns nullptr if there is no backing store (e.g. detached
  // or a zero-length array buffer).
  std::shared_ptr<BackingStore> GetBackingStore();

  // Allocates an ArrayBufferExtension for this array buffer, unless it is
  // already associated with an extension.
  ArrayBufferExtension* EnsureExtension();

  // Frees the associated ArrayBufferExtension and returns its backing store.
  std::shared_ptr<BackingStore> RemoveExtension();

  // Marks ArrayBufferExtension
  void MarkExtension();
  void YoungMarkExtension();
  void YoungMarkExtensionPromoted();

  inline size_t PerIsolateAccountingLength();

  // Dispatched behavior.
  DECL_PRINTER(JSArrayBuffer)
  DECL_VERIFIER(JSArrayBuffer)

  // Layout description.
  DEFINE_FIELD_OFFSET_CONSTANTS(JSObject::kHeaderSize,
                                TORQUE_GENERATED_JS_ARRAY_BUFFER_FIELDS)
  static constexpr int kEndOfTaggedFieldsOffset = JSObject::kHeaderSize;

  static const int kSizeWithEmbedderFields =
      kHeaderSize +
      v8::ArrayBuffer::kEmbedderFieldCount * kEmbedderDataSlotSize;

  class BodyDescriptor;

  OBJECT_CONSTRUCTORS(JSArrayBuffer, JSObject);

 private:
  inline ArrayBufferExtension** extension_location() const;

#if V8_COMPRESS_POINTERS
  static const int kUninitializedTagMask = 1;

  inline uint32_t* extension_lo() const;
  inline uint32_t* extension_hi() const;
#endif
};

// Each JSArrayBuffer (with a backing store) has a corresponding native-heap
// allocated ArrayBufferExtension for GC purposes and storing the backing store.
// When marking a JSArrayBuffer, the GC also marks the native
// extension-object. The GC periodically iterates all extensions concurrently
// and frees unmarked ones.
// https://docs.google.com/document/d/1-ZrLdlFX1nXT3z-FAgLbKal1gI8Auiaya_My-a0UJ28/edit
class ArrayBufferExtension : public Malloced {
  enum class GcState : uint8_t { Dead = 0, Copied, Promoted };

  std::atomic<bool> marked_;
  std::atomic<GcState> young_gc_state_;
  std::shared_ptr<BackingStore> backing_store_;
  ArrayBufferExtension* next_;
  std::size_t accounting_length_;

  GcState young_gc_state() {
    return young_gc_state_.load(std::memory_order_relaxed);
  }

  void set_young_gc_state(GcState value) {
    young_gc_state_.store(value, std::memory_order_relaxed);
  }

 public:
  ArrayBufferExtension()
      : marked_(false),
        young_gc_state_(GcState::Dead),
        backing_store_(std::shared_ptr<BackingStore>()),
        next_(nullptr),
        accounting_length_(0) {}
  explicit ArrayBufferExtension(std::shared_ptr<BackingStore> backing_store)
      : marked_(false),
        young_gc_state_(GcState::Dead),
        backing_store_(backing_store),
        next_(nullptr),
        accounting_length_(0) {}

  void Mark() { marked_.store(true, std::memory_order_relaxed); }
  void Unmark() { marked_.store(false, std::memory_order_relaxed); }
  bool IsMarked() { return marked_.load(std::memory_order_relaxed); }

  void YoungMark() { set_young_gc_state(GcState::Copied); }
  void YoungMarkPromoted() { set_young_gc_state(GcState::Promoted); }
  void YoungUnmark() { set_young_gc_state(GcState::Dead); }
  bool IsYoungMarked() { return young_gc_state() != GcState::Dead; }

  bool IsYoungPromoted() { return young_gc_state() == GcState::Promoted; }

  std::shared_ptr<BackingStore> backing_store() { return backing_store_; }
  BackingStore* backing_store_raw() { return backing_store_.get(); }

  size_t accounting_length() { return accounting_length_; }

  void set_accounting_length(size_t accounting_length) {
    accounting_length_ = accounting_length;
  }

  std::shared_ptr<BackingStore> RemoveBackingStore() {
    return std::move(backing_store_);
  }

  void set_backing_store(std::shared_ptr<BackingStore> backing_store) {
    backing_store_ = std::move(backing_store);
  }

  void reset_backing_store() { backing_store_.reset(); }

  ArrayBufferExtension* next() { return next_; }
  void set_next(ArrayBufferExtension* extension) { next_ = extension; }
};

class JSArrayBufferView : public JSObject {
 public:
  // [buffer]: ArrayBuffer that this typed array views.
  DECL_ACCESSORS(buffer, Object)

  // [byte_offset]: offset of typed array in bytes.
  DECL_PRIMITIVE_ACCESSORS(byte_offset, size_t)

  // [byte_length]: length of typed array in bytes.
  DECL_PRIMITIVE_ACCESSORS(byte_length, size_t)

  DECL_CAST(JSArrayBufferView)

  DECL_VERIFIER(JSArrayBufferView)

  inline bool WasDetached() const;

// Layout description.
#define JS_ARRAY_BUFFER_VIEW_FIELDS(V) \
  V(kBufferOffset, kTaggedSize)        \
  V(kEndOfTaggedFieldsOffset, 0)       \
  /* Raw data fields. */               \
  V(kByteOffsetOffset, kUIntptrSize)   \
  V(kByteLengthOffset, kUIntptrSize)   \
  /* Header size. */                   \
  V(kHeaderSize, 0)

  DEFINE_FIELD_OFFSET_CONSTANTS(JSObject::kHeaderSize,
                                JS_ARRAY_BUFFER_VIEW_FIELDS)
#undef JS_ARRAY_BUFFER_VIEW_FIELDS

  STATIC_ASSERT(IsAligned(kByteOffsetOffset, kUIntptrSize));
  STATIC_ASSERT(IsAligned(kByteLengthOffset, kUIntptrSize));

  OBJECT_CONSTRUCTORS(JSArrayBufferView, JSObject);
};

class JSTypedArray : public JSArrayBufferView {
 public:
  // TODO(v8:4153): This should be equal to JSArrayBuffer::kMaxByteLength
  // eventually.
  static constexpr size_t kMaxLength = v8::TypedArray::kMaxLength;

  // [length]: length of typed array in elements.
  DECL_PRIMITIVE_ACCESSORS(length, size_t)

  // ES6 9.4.5.3
  V8_WARN_UNUSED_RESULT static Maybe<bool> DefineOwnProperty(
      Isolate* isolate, Handle<JSTypedArray> o, Handle<Object> key,
      PropertyDescriptor* desc, Maybe<ShouldThrow> should_throw);

  DECL_CAST(JSTypedArray)

  ExternalArrayType type();
  V8_EXPORT_PRIVATE size_t element_size();

  V8_EXPORT_PRIVATE Handle<JSArrayBuffer> GetBuffer();

  // Use with care: returns raw pointer into heap.
  inline void* DataPtr();

  inline void SetOffHeapDataPtr(void* base, Address offset);
  inline void SetOnHeapDataPtr(HeapObject base, Address offset);

  // Whether the buffer's backing store is on-heap or off-heap.
  inline bool is_on_heap() const;

  // Note: this is a pointer compression specific optimization.
  // Normally, on-heap typed arrays contain HeapObject value in |base_pointer|
  // field and an offset in |external_pointer|.
  // When pointer compression is enabled we want to combine decompression with
  // the offset addition. In order to do that we add an isolate root to the
  // |external_pointer| value and therefore the data pointer computation can
  // is a simple addition of a (potentially sign-extended) |base_pointer| loaded
  // as Tagged_t value and an |external_pointer| value.
  // For full-pointer mode the compensation value is zero.
  static inline Address ExternalPointerCompensationForOnHeapArray(
      const Isolate* isolate);

  // Subtracts external pointer compensation from the external pointer value.
  inline void RemoveExternalPointerCompensationForSerialization();

  static inline MaybeHandle<JSTypedArray> Validate(Isolate* isolate,
                                                   Handle<Object> receiver,
                                                   const char* method_name);

  // Dispatched behavior.
  DECL_PRINTER(JSTypedArray)
  DECL_VERIFIER(JSTypedArray)

// Layout description.
#define JS_TYPED_ARRAY_FIELDS(V)                \
  /* Raw data fields. */                        \
  V(kLengthOffset, kUIntptrSize)                \
  V(kExternalPointerOffset, kSystemPointerSize) \
  V(kBasePointerOffset, kTaggedSize)            \
  /* Header size. */                            \
  V(kHeaderSize, 0)

  DEFINE_FIELD_OFFSET_CONSTANTS(JSArrayBufferView::kHeaderSize,
                                JS_TYPED_ARRAY_FIELDS)
#undef JS_TYPED_ARRAY_FIELDS

  STATIC_ASSERT(IsAligned(kLengthOffset, kUIntptrSize));
  STATIC_ASSERT(IsAligned(kExternalPointerOffset, kSystemPointerSize));

  static const int kSizeWithEmbedderFields =
      kHeaderSize +
      v8::ArrayBufferView::kEmbedderFieldCount * kEmbedderDataSlotSize;

  class BodyDescriptor;

#ifdef V8_TYPED_ARRAY_MAX_SIZE_IN_HEAP
  static constexpr size_t kMaxSizeInHeap = V8_TYPED_ARRAY_MAX_SIZE_IN_HEAP;
#else
  static constexpr size_t kMaxSizeInHeap = 64;
#endif

 private:
  friend class Deserializer;

  // [base_pointer]: TODO(v8:4153)
  DECL_ACCESSORS(base_pointer, Object)

  // [external_pointer]: TODO(v8:4153)
  DECL_PRIMITIVE_ACCESSORS(external_pointer, Address)

  OBJECT_CONSTRUCTORS(JSTypedArray, JSArrayBufferView);
};

class JSDataView : public JSArrayBufferView {
 public:
  // [data_pointer]: pointer to the actual data.
  DECL_PRIMITIVE_ACCESSORS(data_pointer, void*)

  DECL_CAST(JSDataView)

  // Dispatched behavior.
  DECL_PRINTER(JSDataView)
  DECL_VERIFIER(JSDataView)

  // Layout description.
#define JS_DATA_VIEW_FIELDS(V)       \
  /* Raw data fields. */             \
  V(kDataPointerOffset, kIntptrSize) \
  /* Header size. */                 \
  V(kHeaderSize, 0)

  DEFINE_FIELD_OFFSET_CONSTANTS(JSArrayBufferView::kHeaderSize,
                                JS_DATA_VIEW_FIELDS)
#undef JS_DATA_VIEW_FIELDS

  STATIC_ASSERT(IsAligned(kDataPointerOffset, kUIntptrSize));

  static const int kSizeWithEmbedderFields =
      kHeaderSize +
      v8::ArrayBufferView::kEmbedderFieldCount * kEmbedderDataSlotSize;

  class BodyDescriptor;

  OBJECT_CONSTRUCTORS(JSDataView, JSArrayBufferView);
};

}  // namespace internal
}  // namespace v8

#include "src/objects/object-macros-undef.h"

#endif  // V8_OBJECTS_JS_ARRAY_BUFFER_H_

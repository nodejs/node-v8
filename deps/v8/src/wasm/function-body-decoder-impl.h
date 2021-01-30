// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_WASM_FUNCTION_BODY_DECODER_IMPL_H_
#define V8_WASM_FUNCTION_BODY_DECODER_IMPL_H_

// Do only include this header for implementing new Interface of the
// WasmFullDecoder.

#include <inttypes.h>

#include "src/base/platform/elapsed-timer.h"
#include "src/base/platform/wrappers.h"
#include "src/base/small-vector.h"
#include "src/utils/bit-vector.h"
#include "src/wasm/decoder.h"
#include "src/wasm/function-body-decoder.h"
#include "src/wasm/value-type.h"
#include "src/wasm/wasm-features.h"
#include "src/wasm/wasm-limits.h"
#include "src/wasm/wasm-module.h"
#include "src/wasm/wasm-opcodes.h"
#include "src/wasm/wasm-subtyping.h"

namespace v8 {
namespace internal {
namespace wasm {

struct WasmGlobal;
struct WasmException;

#define TRACE(...)                                    \
  do {                                                \
    if (FLAG_trace_wasm_decoder) PrintF(__VA_ARGS__); \
  } while (false)

#define TRACE_INST_FORMAT "  @%-8d #%-20s|"

// Return the evaluation of `condition` if validate==true, DCHECK that it's
// true and always return true otherwise.
#define VALIDATE(condition)                \
  (validate ? V8_LIKELY(condition) : [&] { \
    DCHECK(condition);                     \
    return true;                           \
  }())

#define CHECK_PROTOTYPE_OPCODE(feat)                                       \
  DCHECK(this->module_->origin == kWasmOrigin);                            \
  if (!VALIDATE(this->enabled_.has_##feat())) {                            \
    this->DecodeError(                                                     \
        "Invalid opcode 0x%x (enable with --experimental-wasm-" #feat ")", \
        opcode);                                                           \
    return 0;                                                              \
  }                                                                        \
  this->detected_->Add(kFeature_##feat);

#define ATOMIC_OP_LIST(V)                \
  V(AtomicNotify, Uint32)                \
  V(I32AtomicWait, Uint32)               \
  V(I64AtomicWait, Uint64)               \
  V(I32AtomicLoad, Uint32)               \
  V(I64AtomicLoad, Uint64)               \
  V(I32AtomicLoad8U, Uint8)              \
  V(I32AtomicLoad16U, Uint16)            \
  V(I64AtomicLoad8U, Uint8)              \
  V(I64AtomicLoad16U, Uint16)            \
  V(I64AtomicLoad32U, Uint32)            \
  V(I32AtomicAdd, Uint32)                \
  V(I32AtomicAdd8U, Uint8)               \
  V(I32AtomicAdd16U, Uint16)             \
  V(I64AtomicAdd, Uint64)                \
  V(I64AtomicAdd8U, Uint8)               \
  V(I64AtomicAdd16U, Uint16)             \
  V(I64AtomicAdd32U, Uint32)             \
  V(I32AtomicSub, Uint32)                \
  V(I64AtomicSub, Uint64)                \
  V(I32AtomicSub8U, Uint8)               \
  V(I32AtomicSub16U, Uint16)             \
  V(I64AtomicSub8U, Uint8)               \
  V(I64AtomicSub16U, Uint16)             \
  V(I64AtomicSub32U, Uint32)             \
  V(I32AtomicAnd, Uint32)                \
  V(I64AtomicAnd, Uint64)                \
  V(I32AtomicAnd8U, Uint8)               \
  V(I32AtomicAnd16U, Uint16)             \
  V(I64AtomicAnd8U, Uint8)               \
  V(I64AtomicAnd16U, Uint16)             \
  V(I64AtomicAnd32U, Uint32)             \
  V(I32AtomicOr, Uint32)                 \
  V(I64AtomicOr, Uint64)                 \
  V(I32AtomicOr8U, Uint8)                \
  V(I32AtomicOr16U, Uint16)              \
  V(I64AtomicOr8U, Uint8)                \
  V(I64AtomicOr16U, Uint16)              \
  V(I64AtomicOr32U, Uint32)              \
  V(I32AtomicXor, Uint32)                \
  V(I64AtomicXor, Uint64)                \
  V(I32AtomicXor8U, Uint8)               \
  V(I32AtomicXor16U, Uint16)             \
  V(I64AtomicXor8U, Uint8)               \
  V(I64AtomicXor16U, Uint16)             \
  V(I64AtomicXor32U, Uint32)             \
  V(I32AtomicExchange, Uint32)           \
  V(I64AtomicExchange, Uint64)           \
  V(I32AtomicExchange8U, Uint8)          \
  V(I32AtomicExchange16U, Uint16)        \
  V(I64AtomicExchange8U, Uint8)          \
  V(I64AtomicExchange16U, Uint16)        \
  V(I64AtomicExchange32U, Uint32)        \
  V(I32AtomicCompareExchange, Uint32)    \
  V(I64AtomicCompareExchange, Uint64)    \
  V(I32AtomicCompareExchange8U, Uint8)   \
  V(I32AtomicCompareExchange16U, Uint16) \
  V(I64AtomicCompareExchange8U, Uint8)   \
  V(I64AtomicCompareExchange16U, Uint16) \
  V(I64AtomicCompareExchange32U, Uint32)

#define ATOMIC_STORE_OP_LIST(V) \
  V(I32AtomicStore, Uint32)     \
  V(I64AtomicStore, Uint64)     \
  V(I32AtomicStore8U, Uint8)    \
  V(I32AtomicStore16U, Uint16)  \
  V(I64AtomicStore8U, Uint8)    \
  V(I64AtomicStore16U, Uint16)  \
  V(I64AtomicStore32U, Uint32)

// Decoder error with explicit PC and format arguments.
template <Decoder::ValidateFlag validate, typename... Args>
void DecodeError(Decoder* decoder, const byte* pc, const char* str,
                 Args&&... args) {
  CHECK(validate == Decoder::kFullValidation ||
        validate == Decoder::kBooleanValidation);
  STATIC_ASSERT(sizeof...(Args) > 0);
  if (validate == Decoder::kBooleanValidation) {
    decoder->MarkError();
  } else {
    decoder->errorf(pc, str, std::forward<Args>(args)...);
  }
}

// Decoder error with explicit PC and no format arguments.
template <Decoder::ValidateFlag validate>
void DecodeError(Decoder* decoder, const byte* pc, const char* str) {
  CHECK(validate == Decoder::kFullValidation ||
        validate == Decoder::kBooleanValidation);
  if (validate == Decoder::kBooleanValidation) {
    decoder->MarkError();
  } else {
    decoder->error(pc, str);
  }
}

// Decoder error without explicit PC, but with format arguments.
template <Decoder::ValidateFlag validate, typename... Args>
void DecodeError(Decoder* decoder, const char* str, Args&&... args) {
  CHECK(validate == Decoder::kFullValidation ||
        validate == Decoder::kBooleanValidation);
  STATIC_ASSERT(sizeof...(Args) > 0);
  if (validate == Decoder::kBooleanValidation) {
    decoder->MarkError();
  } else {
    decoder->errorf(str, std::forward<Args>(args)...);
  }
}

// Decoder error without explicit PC and without format arguments.
template <Decoder::ValidateFlag validate>
void DecodeError(Decoder* decoder, const char* str) {
  CHECK(validate == Decoder::kFullValidation ||
        validate == Decoder::kBooleanValidation);
  if (validate == Decoder::kBooleanValidation) {
    decoder->MarkError();
  } else {
    decoder->error(str);
  }
}

namespace value_type_reader {

V8_INLINE WasmFeature feature_for_heap_type(HeapType heap_type) {
  switch (heap_type.representation()) {
    case HeapType::kFunc:
    case HeapType::kExtern:
      return WasmFeature::kFeature_reftypes;
    case HeapType::kEq:
    case HeapType::kI31:
    case HeapType::kData:
    case HeapType::kAny:
      return WasmFeature::kFeature_gc;
    case HeapType::kBottom:
      UNREACHABLE();
  }
}

// If {module} is not null, the read index will be checked against the module's
// type capacity.
template <Decoder::ValidateFlag validate>
HeapType read_heap_type(Decoder* decoder, const byte* pc,
                        uint32_t* const length, const WasmModule* module,
                        const WasmFeatures& enabled) {
  int64_t heap_index = decoder->read_i33v<validate>(pc, length, "heap type");
  if (heap_index < 0) {
    int64_t min_1_byte_leb128 = -64;
    if (!VALIDATE(heap_index >= min_1_byte_leb128)) {
      DecodeError<validate>(decoder, pc, "Unknown heap type %" PRId64,
                            heap_index);
      return HeapType(HeapType::kBottom);
    }
    uint8_t uint_7_mask = 0x7F;
    uint8_t code = static_cast<ValueTypeCode>(heap_index) & uint_7_mask;
    switch (code) {
      case kFuncRefCode:
      case kEqRefCode:
      case kExternRefCode:
      case kI31RefCode:
      case kDataRefCode:
      case kAnyRefCode: {
        HeapType result = HeapType::from_code(code);
        if (!VALIDATE(enabled.contains(feature_for_heap_type(result)))) {
          DecodeError<validate>(
              decoder, pc,
              "invalid heap type '%s', enable with --experimental-wasm-%s",
              result.name().c_str(),
              WasmFeatures::name_for_feature(feature_for_heap_type(result)));
          return HeapType(HeapType::kBottom);
        }
        return result;
      }
      default:
        DecodeError<validate>(decoder, pc, "Unknown heap type %" PRId64,
                              heap_index);
        return HeapType(HeapType::kBottom);
    }
    UNREACHABLE();
  } else {
    if (!VALIDATE(enabled.has_typed_funcref())) {
      DecodeError<validate>(decoder, pc,
                            "Invalid indexed heap type, enable with "
                            "--experimental-wasm-typed-funcref");
      return HeapType(HeapType::kBottom);
    }
    uint32_t type_index = static_cast<uint32_t>(heap_index);
    if (!VALIDATE(type_index < kV8MaxWasmTypes)) {
      DecodeError<validate>(
          decoder, pc,
          "Type index %u is greater than the maximum number %zu "
          "of type definitions supported by V8",
          type_index, kV8MaxWasmTypes);
      return HeapType(HeapType::kBottom);
    }
    // We use capacity over size so this works mid-DecodeTypeSection.
    if (!VALIDATE(module == nullptr || type_index < module->types.capacity())) {
      DecodeError<validate>(decoder, pc, "Type index %u is out of bounds",
                            type_index);
      return HeapType(HeapType::kBottom);
    }
    return HeapType(type_index);
  }
}

// Read a value type starting at address {pc} using {decoder}.
// No bytes are consumed.
// The length of the read value type is written in {length}.
// Registers an error for an invalid type only if {validate} is not
// kNoValidate.
template <Decoder::ValidateFlag validate>
ValueType read_value_type(Decoder* decoder, const byte* pc,
                          uint32_t* const length, const WasmModule* module,
                          const WasmFeatures& enabled) {
  *length = 1;
  byte val = decoder->read_u8<validate>(pc, "value type opcode");
  if (decoder->failed()) {
    *length = 0;
    return kWasmBottom;
  }
  ValueTypeCode code = static_cast<ValueTypeCode>(val);
  switch (code) {
    case kFuncRefCode:
    case kEqRefCode:
    case kExternRefCode:
    case kI31RefCode:
    case kDataRefCode:
    case kAnyRefCode: {
      HeapType heap_type = HeapType::from_code(code);
      Nullability nullability = code == kI31RefCode || code == kDataRefCode
                                    ? kNonNullable
                                    : kNullable;
      ValueType result = ValueType::Ref(heap_type, nullability);
      if (!VALIDATE(enabled.contains(feature_for_heap_type(heap_type)))) {
        DecodeError<validate>(
            decoder, pc,
            "invalid value type '%s', enable with --experimental-wasm-%s",
            result.name().c_str(),
            WasmFeatures::name_for_feature(feature_for_heap_type(heap_type)));
        return kWasmBottom;
      }
      return result;
    }
    case kI32Code:
      return kWasmI32;
    case kI64Code:
      return kWasmI64;
    case kF32Code:
      return kWasmF32;
    case kF64Code:
      return kWasmF64;
    case kRefCode:
    case kOptRefCode: {
      Nullability nullability = code == kOptRefCode ? kNullable : kNonNullable;
      if (!VALIDATE(enabled.has_typed_funcref())) {
        DecodeError<validate>(decoder, pc,
                              "Invalid type '(ref%s <heaptype>)', enable with "
                              "--experimental-wasm-typed-funcref",
                              nullability == kNullable ? " null" : "");
        return kWasmBottom;
      }
      HeapType heap_type =
          read_heap_type<validate>(decoder, pc + 1, length, module, enabled);
      *length += 1;
      return heap_type.is_bottom() ? kWasmBottom
                                   : ValueType::Ref(heap_type, nullability);
    }
    case kRttWithDepthCode: {
      if (!VALIDATE(enabled.has_gc())) {
        DecodeError<validate>(
            decoder, pc,
            "invalid value type 'rtt', enable with --experimental-wasm-gc");
        return kWasmBottom;
      }
      uint32_t depth = decoder->read_u32v<validate>(pc + 1, length, "depth");
      *length += 1;
      if (!VALIDATE(depth <= kV8MaxRttSubtypingDepth)) {
        DecodeError<validate>(
            decoder, pc,
            "subtyping depth %u is greater than the maximum depth "
            "%u supported by V8",
            depth, kV8MaxRttSubtypingDepth);
        return kWasmBottom;
      }
      uint32_t type_index_length;
      uint32_t type_index =
          decoder->read_u32v<validate>(pc + *length, &type_index_length);
      *length += type_index_length;
      if (!VALIDATE(type_index < kV8MaxWasmTypes)) {
        DecodeError<validate>(
            decoder, pc,
            "Type index %u is greater than the maximum number %zu "
            "of type definitions supported by V8",
            type_index, kV8MaxWasmTypes);
        return kWasmBottom;
      }
      // We use capacity over size so this works mid-DecodeTypeSection.
      if (!VALIDATE(module == nullptr ||
                    type_index < module->types.capacity())) {
        DecodeError<validate>(decoder, pc, "Type index %u is out of bounds",
                              type_index);
        return kWasmBottom;
      }
      return ValueType::Rtt(type_index, depth);
    }
    case kRttCode: {
      if (!VALIDATE(enabled.has_gc())) {
        DecodeError<validate>(
            decoder, pc,
            "invalid value type 'rtt', enable with --experimental-wasm-gc");
        return kWasmBottom;
      }
      uint32_t type_index = decoder->read_u32v<validate>(pc + 1, length);
      *length += 1;
      if (!VALIDATE(type_index < kV8MaxWasmTypes)) {
        DecodeError<validate>(
            decoder, pc,
            "Type index %u is greater than the maximum number %zu "
            "of type definitions supported by V8",
            type_index, kV8MaxWasmTypes);
        return kWasmBottom;
      }
      // We use capacity over size so this works mid-DecodeTypeSection.
      if (!VALIDATE(module == nullptr ||
                    type_index < module->types.capacity())) {
        DecodeError<validate>(decoder, pc, "Type index %u is out of bounds",
                              type_index);
        return kWasmBottom;
      }
      return ValueType::Rtt(type_index);
    }
    case kS128Code: {
      if (!VALIDATE(enabled.has_simd())) {
        DecodeError<validate>(
            decoder, pc,
            "invalid value type 's128', enable with --experimental-wasm-simd");
        return kWasmBottom;
      }
      return kWasmS128;
    }
    // Although these codes are included in ValueTypeCode, they technically
    // do not correspond to value types and are only used in specific
    // contexts. The caller of this function is responsible for handling them.
    case kVoidCode:
    case kI8Code:
    case kI16Code:
      if (validate) {
        DecodeError<validate>(decoder, pc, "invalid value type 0x%x", code);
      }
      return kWasmBottom;
  }
  // Anything that doesn't match an enumeration value is an invalid type code.
  if (validate) {
    DecodeError<validate>(decoder, pc, "invalid value type 0x%x", code);
  }
  return kWasmBottom;
}
}  // namespace value_type_reader

// Helpers for decoding different kinds of immediates which follow bytecodes.
template <Decoder::ValidateFlag validate>
struct LocalIndexImmediate {
  uint32_t index;
  uint32_t length;

  inline LocalIndexImmediate(Decoder* decoder, const byte* pc) {
    index = decoder->read_u32v<validate>(pc, &length, "local index");
  }
};

template <Decoder::ValidateFlag validate>
struct ExceptionIndexImmediate {
  uint32_t index;
  const WasmException* exception = nullptr;
  uint32_t length;

  inline ExceptionIndexImmediate(Decoder* decoder, const byte* pc) {
    index = decoder->read_u32v<validate>(pc, &length, "exception index");
  }
};

template <Decoder::ValidateFlag validate>
struct ImmI32Immediate {
  int32_t value;
  uint32_t length;
  inline ImmI32Immediate(Decoder* decoder, const byte* pc) {
    value = decoder->read_i32v<validate>(pc, &length, "immi32");
  }
};

template <Decoder::ValidateFlag validate>
struct ImmI64Immediate {
  int64_t value;
  uint32_t length;
  inline ImmI64Immediate(Decoder* decoder, const byte* pc) {
    value = decoder->read_i64v<validate>(pc, &length, "immi64");
  }
};

template <Decoder::ValidateFlag validate>
struct ImmF32Immediate {
  float value;
  uint32_t length = 4;
  inline ImmF32Immediate(Decoder* decoder, const byte* pc) {
    // We can't use bit_cast here because calling any helper function that
    // returns a float would potentially flip NaN bits per C++ semantics, so we
    // have to inline the memcpy call directly.
    uint32_t tmp = decoder->read_u32<validate>(pc, "immf32");
    base::Memcpy(&value, &tmp, sizeof(value));
  }
};

template <Decoder::ValidateFlag validate>
struct ImmF64Immediate {
  double value;
  uint32_t length = 8;
  inline ImmF64Immediate(Decoder* decoder, const byte* pc) {
    // Avoid bit_cast because it might not preserve the signalling bit of a NaN.
    uint64_t tmp = decoder->read_u64<validate>(pc, "immf64");
    base::Memcpy(&value, &tmp, sizeof(value));
  }
};

template <Decoder::ValidateFlag validate>
struct GlobalIndexImmediate {
  uint32_t index;
  ValueType type = kWasmStmt;
  const WasmGlobal* global = nullptr;
  uint32_t length;

  inline GlobalIndexImmediate(Decoder* decoder, const byte* pc) {
    index = decoder->read_u32v<validate>(pc, &length, "global index");
  }
};

template <Decoder::ValidateFlag validate>
struct SelectTypeImmediate {
  uint32_t length;
  ValueType type;

  inline SelectTypeImmediate(const WasmFeatures& enabled, Decoder* decoder,
                             const byte* pc, const WasmModule* module) {
    uint8_t num_types =
        decoder->read_u32v<validate>(pc, &length, "number of select types");
    if (!VALIDATE(num_types == 1)) {
      DecodeError<validate>(
          decoder, pc + 1,
          "Invalid number of types. Select accepts exactly one type");
      return;
    }
    uint32_t type_length;
    type = value_type_reader::read_value_type<validate>(
        decoder, pc + length, &type_length, module, enabled);
    length += type_length;
  }
};

template <Decoder::ValidateFlag validate>
struct BlockTypeImmediate {
  uint32_t length = 1;
  ValueType type = kWasmStmt;
  uint32_t sig_index = 0;
  const FunctionSig* sig = nullptr;

  inline BlockTypeImmediate(const WasmFeatures& enabled, Decoder* decoder,
                            const byte* pc, const WasmModule* module) {
    int64_t block_type =
        decoder->read_i33v<validate>(pc, &length, "block type");
    if (block_type < 0) {
      // All valid negative types are 1 byte in length, so we check against the
      // minimum 1-byte LEB128 value.
      constexpr int64_t min_1_byte_leb128 = -64;
      if (!VALIDATE(block_type >= min_1_byte_leb128)) {
        DecodeError<validate>(decoder, pc, "invalid block type %" PRId64,
                              block_type);
        return;
      }
      if (static_cast<ValueTypeCode>(block_type & 0x7F) == kVoidCode) return;
      type = value_type_reader::read_value_type<validate>(decoder, pc, &length,
                                                          module, enabled);
    } else {
      if (!VALIDATE(enabled.has_mv())) {
        DecodeError<validate>(decoder, pc,
                              "invalid block type %" PRId64
                              ", enable with --experimental-wasm-mv",
                              block_type);
        return;
      }
      type = kWasmBottom;
      sig_index = static_cast<uint32_t>(block_type);
    }
  }

  uint32_t in_arity() const {
    if (type != kWasmBottom) return 0;
    return static_cast<uint32_t>(sig->parameter_count());
  }
  uint32_t out_arity() const {
    if (type == kWasmStmt) return 0;
    if (type != kWasmBottom) return 1;
    return static_cast<uint32_t>(sig->return_count());
  }
  ValueType in_type(uint32_t index) {
    DCHECK_EQ(kWasmBottom, type);
    return sig->GetParam(index);
  }
  ValueType out_type(uint32_t index) {
    if (type == kWasmBottom) return sig->GetReturn(index);
    DCHECK_NE(kWasmStmt, type);
    DCHECK_EQ(0, index);
    return type;
  }
};

template <Decoder::ValidateFlag validate>
struct BranchDepthImmediate {
  uint32_t depth;
  uint32_t length;
  inline BranchDepthImmediate(Decoder* decoder, const byte* pc) {
    depth = decoder->read_u32v<validate>(pc, &length, "branch depth");
  }
};

template <Decoder::ValidateFlag validate>
struct BranchOnExceptionImmediate {
  BranchDepthImmediate<validate> depth;
  ExceptionIndexImmediate<validate> index;
  uint32_t length = 0;
  inline BranchOnExceptionImmediate(Decoder* decoder, const byte* pc)
      : depth(BranchDepthImmediate<validate>(decoder, pc)),
        index(ExceptionIndexImmediate<validate>(decoder, pc + depth.length)) {
    length = depth.length + index.length;
  }
};

template <Decoder::ValidateFlag validate>
struct FunctionIndexImmediate {
  uint32_t index = 0;
  uint32_t length = 1;
  inline FunctionIndexImmediate(Decoder* decoder, const byte* pc) {
    index = decoder->read_u32v<validate>(pc, &length, "function index");
  }
};

template <Decoder::ValidateFlag validate>
struct MemoryIndexImmediate {
  uint32_t index = 0;
  uint32_t length = 1;
  inline MemoryIndexImmediate() = default;
  inline MemoryIndexImmediate(Decoder* decoder, const byte* pc) {
    index = decoder->read_u8<validate>(pc, "memory index");
    if (!VALIDATE(index == 0)) {
      DecodeError<validate>(decoder, pc, "expected memory index 0, found %u",
                            index);
    }
  }
};

template <Decoder::ValidateFlag validate>
struct TableIndexImmediate {
  uint32_t index = 0;
  uint32_t length = 1;
  inline TableIndexImmediate() = default;
  inline TableIndexImmediate(Decoder* decoder, const byte* pc) {
    index = decoder->read_u32v<validate>(pc, &length, "table index");
  }
};

template <Decoder::ValidateFlag validate>
struct TypeIndexImmediate {
  uint32_t index = 0;
  uint32_t length = 1;
  inline TypeIndexImmediate(Decoder* decoder, const byte* pc) {
    index = decoder->read_u32v<validate>(pc, &length, "type index");
  }
};

// TODO(jkummerow): Introduce a common superclass for StructIndexImmediate and
// ArrayIndexImmediate? Maybe even FunctionIndexImmediate too?
template <Decoder::ValidateFlag validate>
struct StructIndexImmediate {
  uint32_t index = 0;
  uint32_t length = 0;
  const StructType* struct_type = nullptr;
  inline StructIndexImmediate(Decoder* decoder, const byte* pc) {
    index = decoder->read_u32v<validate>(pc, &length, "struct index");
  }
};

template <Decoder::ValidateFlag validate>
struct FieldIndexImmediate {
  StructIndexImmediate<validate> struct_index;
  uint32_t index = 0;
  uint32_t length = 0;
  inline FieldIndexImmediate(Decoder* decoder, const byte* pc)
      : struct_index(decoder, pc) {
    index = decoder->read_u32v<validate>(pc + struct_index.length, &length,
                                         "field index");
    length += struct_index.length;
  }
};

template <Decoder::ValidateFlag validate>
struct ArrayIndexImmediate {
  uint32_t index = 0;
  uint32_t length = 0;
  const ArrayType* array_type = nullptr;
  inline ArrayIndexImmediate(Decoder* decoder, const byte* pc) {
    index = decoder->read_u32v<validate>(pc, &length, "array index");
  }
};

template <Decoder::ValidateFlag validate>
struct CallIndirectImmediate {
  uint32_t table_index;
  uint32_t sig_index;
  const FunctionSig* sig = nullptr;
  uint32_t length = 0;
  inline CallIndirectImmediate(const WasmFeatures enabled, Decoder* decoder,
                               const byte* pc) {
    uint32_t len = 0;
    sig_index = decoder->read_u32v<validate>(pc, &len, "signature index");
    TableIndexImmediate<validate> table(decoder, pc + len);
    if (!VALIDATE((table.index == 0 && table.length == 1) ||
                  enabled.has_reftypes())) {
      DecodeError<validate>(decoder, pc + len,
                            "expected table index 0, found %u", table.index);
    }
    table_index = table.index;
    length = len + table.length;
  }
};

template <Decoder::ValidateFlag validate>
struct CallFunctionImmediate {
  uint32_t index;
  const FunctionSig* sig = nullptr;
  uint32_t length;
  inline CallFunctionImmediate(Decoder* decoder, const byte* pc) {
    index = decoder->read_u32v<validate>(pc, &length, "function index");
  }
};

template <Decoder::ValidateFlag validate>
struct BranchTableImmediate {
  uint32_t table_count;
  const byte* start;
  const byte* table;
  inline BranchTableImmediate(Decoder* decoder, const byte* pc) {
    start = pc;
    uint32_t len = 0;
    table_count = decoder->read_u32v<validate>(pc, &len, "table count");
    table = pc + len;
  }
};

// A helper to iterate over a branch table.
template <Decoder::ValidateFlag validate>
class BranchTableIterator {
 public:
  uint32_t cur_index() { return index_; }
  bool has_next() { return VALIDATE(decoder_->ok()) && index_ <= table_count_; }
  uint32_t next() {
    DCHECK(has_next());
    index_++;
    uint32_t length;
    uint32_t result =
        decoder_->read_u32v<validate>(pc_, &length, "branch table entry");
    pc_ += length;
    return result;
  }
  // length, including the length of the {BranchTableImmediate}, but not the
  // opcode.
  uint32_t length() {
    while (has_next()) next();
    return static_cast<uint32_t>(pc_ - start_);
  }
  const byte* pc() { return pc_; }

  BranchTableIterator(Decoder* decoder,
                      const BranchTableImmediate<validate>& imm)
      : decoder_(decoder),
        start_(imm.start),
        pc_(imm.table),
        table_count_(imm.table_count) {}

 private:
  Decoder* const decoder_;
  const byte* start_;
  const byte* pc_;
  uint32_t index_ = 0;          // the current index.
  const uint32_t table_count_;  // the count of entries, not including default.
};

template <Decoder::ValidateFlag validate>
class WasmDecoder;

template <Decoder::ValidateFlag validate>
struct MemoryAccessImmediate {
  uint32_t alignment;
  uint64_t offset;
  uint32_t length = 0;
  inline MemoryAccessImmediate(Decoder* decoder, const byte* pc,
                               uint32_t max_alignment, bool is_memory64) {
    uint32_t alignment_length;
    alignment =
        decoder->read_u32v<validate>(pc, &alignment_length, "alignment");
    if (!VALIDATE(alignment <= max_alignment)) {
      DecodeError<validate>(
          decoder, pc,
          "invalid alignment; expected maximum alignment is %u, "
          "actual alignment is %u",
          max_alignment, alignment);
    }
    uint32_t offset_length;
    offset = is_memory64 ? decoder->read_u64v<validate>(
                               pc + alignment_length, &offset_length, "offset")
                         : decoder->read_u32v<validate>(
                               pc + alignment_length, &offset_length, "offset");
    length = alignment_length + offset_length;
  }
  // Defined below, after the definition of WasmDecoder.
  inline MemoryAccessImmediate(WasmDecoder<validate>* decoder, const byte* pc,
                               uint32_t max_alignment);
};

// Immediate for SIMD lane operations.
template <Decoder::ValidateFlag validate>
struct SimdLaneImmediate {
  uint8_t lane;
  uint32_t length = 1;

  inline SimdLaneImmediate(Decoder* decoder, const byte* pc) {
    lane = decoder->read_u8<validate>(pc, "lane");
  }
};

// Immediate for SIMD S8x16 shuffle operations.
template <Decoder::ValidateFlag validate>
struct Simd128Immediate {
  uint8_t value[kSimd128Size] = {0};

  inline Simd128Immediate(Decoder* decoder, const byte* pc) {
    for (uint32_t i = 0; i < kSimd128Size; ++i) {
      value[i] = decoder->read_u8<validate>(pc + i, "value");
    }
  }
};

template <Decoder::ValidateFlag validate>
struct MemoryInitImmediate {
  uint32_t data_segment_index = 0;
  MemoryIndexImmediate<validate> memory;
  unsigned length = 0;

  inline MemoryInitImmediate(Decoder* decoder, const byte* pc) {
    uint32_t len = 0;
    data_segment_index =
        decoder->read_u32v<validate>(pc, &len, "data segment index");
    memory = MemoryIndexImmediate<validate>(decoder, pc + len);
    length = len + memory.length;
  }
};

template <Decoder::ValidateFlag validate>
struct DataDropImmediate {
  uint32_t index;
  unsigned length;

  inline DataDropImmediate(Decoder* decoder, const byte* pc) {
    index = decoder->read_u32v<validate>(pc, &length, "data segment index");
  }
};

template <Decoder::ValidateFlag validate>
struct MemoryCopyImmediate {
  MemoryIndexImmediate<validate> memory_src;
  MemoryIndexImmediate<validate> memory_dst;
  unsigned length = 0;

  inline MemoryCopyImmediate(Decoder* decoder, const byte* pc) {
    memory_src = MemoryIndexImmediate<validate>(decoder, pc);
    memory_dst =
        MemoryIndexImmediate<validate>(decoder, pc + memory_src.length);
    length = memory_src.length + memory_dst.length;
  }
};

template <Decoder::ValidateFlag validate>
struct TableInitImmediate {
  uint32_t elem_segment_index = 0;
  TableIndexImmediate<validate> table;
  unsigned length = 0;

  inline TableInitImmediate(Decoder* decoder, const byte* pc) {
    uint32_t len = 0;
    elem_segment_index =
        decoder->read_u32v<validate>(pc, &len, "elem segment index");
    table = TableIndexImmediate<validate>(decoder, pc + len);
    length = len + table.length;
  }
};

template <Decoder::ValidateFlag validate>
struct ElemDropImmediate {
  uint32_t index;
  unsigned length;

  inline ElemDropImmediate(Decoder* decoder, const byte* pc) {
    index = decoder->read_u32v<validate>(pc, &length, "elem segment index");
  }
};

template <Decoder::ValidateFlag validate>
struct TableCopyImmediate {
  TableIndexImmediate<validate> table_dst;
  TableIndexImmediate<validate> table_src;
  unsigned length = 0;

  inline TableCopyImmediate(Decoder* decoder, const byte* pc) {
    table_dst = TableIndexImmediate<validate>(decoder, pc);
    table_src = TableIndexImmediate<validate>(decoder, pc + table_dst.length);
    length = table_src.length + table_dst.length;
  }
};

template <Decoder::ValidateFlag validate>
struct HeapTypeImmediate {
  uint32_t length = 1;
  HeapType type = HeapType(HeapType::kBottom);
  inline HeapTypeImmediate(const WasmFeatures& enabled, Decoder* decoder,
                           const byte* pc, const WasmModule* module) {
    type = value_type_reader::read_heap_type<validate>(decoder, pc, &length,
                                                       module, enabled);
  }
};

template <Decoder::ValidateFlag validate>
struct PcForErrors {
  PcForErrors(const byte* /* pc */) {}

  const byte* pc() const { return nullptr; }
};

template <>
struct PcForErrors<Decoder::kFullValidation> {
  const byte* pc_for_errors = nullptr;

  PcForErrors(const byte* pc) : pc_for_errors(pc) {}

  const byte* pc() const { return pc_for_errors; }
};

// An entry on the value stack.
template <Decoder::ValidateFlag validate>
struct ValueBase : public PcForErrors<validate> {
  ValueType type = kWasmStmt;

  ValueBase(const byte* pc, ValueType type)
      : PcForErrors<validate>(pc), type(type) {}
};

template <typename Value>
struct Merge {
  uint32_t arity = 0;
  union {  // Either multiple values or a single value.
    Value* array;
    Value first;
  } vals = {nullptr};  // Initialize {array} with {nullptr}.

  // Tracks whether this merge was ever reached. Uses precise reachability, like
  // Reachability::kReachable.
  bool reached;

  explicit Merge(bool reached = false) : reached(reached) {}

  Value& operator[](uint32_t i) {
    DCHECK_GT(arity, i);
    return arity == 1 ? vals.first : vals.array[i];
  }
};

enum ControlKind : uint8_t {
  kControlIf,
  kControlIfElse,
  kControlBlock,
  kControlLoop,
  kControlLet,
  kControlTry,
  kControlTryCatch,
  kControlTryCatchAll
};

enum Reachability : uint8_t {
  // reachable code.
  kReachable,
  // reachable code in unreachable block (implies normal validation).
  kSpecOnlyReachable,
  // code unreachable in its own block (implies polymorphic validation).
  kUnreachable
};

// An entry on the control stack (i.e. if, block, loop, or try).
template <typename Value, Decoder::ValidateFlag validate>
struct ControlBase : public PcForErrors<validate> {
  ControlKind kind = kControlBlock;
  uint32_t locals_count = 0;
  uint32_t stack_depth = 0;  // stack height at the beginning of the construct.
  Reachability reachability = kReachable;

  // Values merged into the start or end of this control construct.
  Merge<Value> start_merge;
  Merge<Value> end_merge;

  MOVE_ONLY_NO_DEFAULT_CONSTRUCTOR(ControlBase);

  ControlBase(ControlKind kind, uint32_t locals_count, uint32_t stack_depth,
              const uint8_t* pc, Reachability reachability)
      : PcForErrors<validate>(pc),
        kind(kind),
        locals_count(locals_count),
        stack_depth(stack_depth),
        reachability(reachability),
        start_merge(reachability == kReachable) {
    DCHECK(kind == kControlLet || locals_count == 0);
  }

  // Check whether the current block is reachable.
  bool reachable() const { return reachability == kReachable; }

  // Check whether the rest of the block is unreachable.
  // Note that this is different from {!reachable()}, as there is also the
  // "indirect unreachable state", for which both {reachable()} and
  // {unreachable()} return false.
  bool unreachable() const { return reachability == kUnreachable; }

  // Return the reachability of new control structs started in this block.
  Reachability innerReachability() const {
    return reachability == kReachable ? kReachable : kSpecOnlyReachable;
  }

  bool is_if() const { return is_onearmed_if() || is_if_else(); }
  bool is_onearmed_if() const { return kind == kControlIf; }
  bool is_if_else() const { return kind == kControlIfElse; }
  bool is_block() const { return kind == kControlBlock; }
  bool is_let() const { return kind == kControlLet; }
  bool is_loop() const { return kind == kControlLoop; }
  bool is_incomplete_try() const { return kind == kControlTry; }
  bool is_try_catch() const { return kind == kControlTryCatch; }
  bool is_try_catchall() const { return kind == kControlTryCatchAll; }
  bool is_try() const {
    return is_incomplete_try() || is_try_catch() || is_try_catchall();
  }

  inline Merge<Value>* br_merge() {
    return is_loop() ? &this->start_merge : &this->end_merge;
  }
};

// This is the list of callback functions that an interface for the
// WasmFullDecoder should implement.
// F(Name, args...)
#define INTERFACE_FUNCTIONS(F)                                                 \
  /* General: */                                                               \
  F(StartFunction)                                                             \
  F(StartFunctionBody, Control* block)                                         \
  F(FinishFunction)                                                            \
  F(OnFirstError)                                                              \
  F(NextInstruction, WasmOpcode)                                               \
  /* Control: */                                                               \
  F(Block, Control* block)                                                     \
  F(Loop, Control* block)                                                      \
  F(Try, Control* block)                                                       \
  F(If, const Value& cond, Control* if_block)                                  \
  F(FallThruTo, Control* c)                                                    \
  F(PopControl, Control* block)                                                \
  F(EndControl, Control* block)                                                \
  /* Instructions: */                                                          \
  F(UnOp, WasmOpcode opcode, const Value& value, Value* result)                \
  F(BinOp, WasmOpcode opcode, const Value& lhs, const Value& rhs,              \
    Value* result)                                                             \
  F(I32Const, Value* result, int32_t value)                                    \
  F(I64Const, Value* result, int64_t value)                                    \
  F(F32Const, Value* result, float value)                                      \
  F(F64Const, Value* result, double value)                                     \
  F(RefNull, ValueType type, Value* result)                                    \
  F(RefFunc, uint32_t function_index, Value* result)                           \
  F(RefAsNonNull, const Value& arg, Value* result)                             \
  F(Drop)                                                                      \
  F(DoReturn)                                                                  \
  F(LocalGet, Value* result, const LocalIndexImmediate<validate>& imm)         \
  F(LocalSet, const Value& value, const LocalIndexImmediate<validate>& imm)    \
  F(LocalTee, const Value& value, Value* result,                               \
    const LocalIndexImmediate<validate>& imm)                                  \
  F(AllocateLocals, Vector<Value> local_values)                                \
  F(DeallocateLocals, uint32_t count)                                          \
  F(GlobalGet, Value* result, const GlobalIndexImmediate<validate>& imm)       \
  F(GlobalSet, const Value& value, const GlobalIndexImmediate<validate>& imm)  \
  F(TableGet, const Value& index, Value* result,                               \
    const TableIndexImmediate<validate>& imm)                                  \
  F(TableSet, const Value& index, const Value& value,                          \
    const TableIndexImmediate<validate>& imm)                                  \
  F(Unreachable)                                                               \
  F(NopForTestingUnsupportedInLiftoff)                                         \
  F(Select, const Value& cond, const Value& fval, const Value& tval,           \
    Value* result)                                                             \
  F(BrOrRet, uint32_t depth)                                                   \
  F(BrIf, const Value& cond, uint32_t depth)                                   \
  F(BrTable, const BranchTableImmediate<validate>& imm, const Value& key)      \
  F(Else, Control* if_block)                                                   \
  F(LoadMem, LoadType type, const MemoryAccessImmediate<validate>& imm,        \
    const Value& index, Value* result)                                         \
  F(LoadTransform, LoadType type, LoadTransformationKind transform,            \
    const MemoryAccessImmediate<validate>& imm, const Value& index,            \
    Value* result)                                                             \
  F(LoadLane, LoadType type, const Value& value, const Value& index,           \
    const MemoryAccessImmediate<validate>& imm, const uint8_t laneidx,         \
    Value* result)                                                             \
  F(Prefetch, const MemoryAccessImmediate<validate>& imm, const Value& index,  \
    bool temporal)                                                             \
  F(StoreMem, StoreType type, const MemoryAccessImmediate<validate>& imm,      \
    const Value& index, const Value& value)                                    \
  F(StoreLane, StoreType type, const MemoryAccessImmediate<validate>& imm,     \
    const Value& index, const Value& value, const uint8_t laneidx)             \
  F(CurrentMemoryPages, Value* result)                                         \
  F(MemoryGrow, const Value& value, Value* result)                             \
  F(CallDirect, const CallFunctionImmediate<validate>& imm,                    \
    const Value args[], Value returns[])                                       \
  F(CallIndirect, const Value& index,                                          \
    const CallIndirectImmediate<validate>& imm, const Value args[],            \
    Value returns[])                                                           \
  F(CallRef, const Value& func_ref, const FunctionSig* sig,                    \
    uint32_t sig_index, const Value args[], const Value returns[])             \
  F(ReturnCallRef, const Value& func_ref, const FunctionSig* sig,              \
    uint32_t sig_index, const Value args[])                                    \
  F(ReturnCall, const CallFunctionImmediate<validate>& imm,                    \
    const Value args[])                                                        \
  F(ReturnCallIndirect, const Value& index,                                    \
    const CallIndirectImmediate<validate>& imm, const Value args[])            \
  F(BrOnNull, const Value& ref_object, uint32_t depth)                         \
  F(SimdOp, WasmOpcode opcode, Vector<Value> args, Value* result)              \
  F(SimdLaneOp, WasmOpcode opcode, const SimdLaneImmediate<validate>& imm,     \
    const Vector<Value> inputs, Value* result)                                 \
  F(S128Const, const Simd128Immediate<validate>& imm, Value* result)           \
  F(Simd8x16ShuffleOp, const Simd128Immediate<validate>& imm,                  \
    const Value& input0, const Value& input1, Value* result)                   \
  F(Throw, const ExceptionIndexImmediate<validate>& imm,                       \
    const Vector<Value>& args)                                                 \
  F(Rethrow, Control* block)                                                   \
  F(CatchException, const ExceptionIndexImmediate<validate>& imm,              \
    Control* block, Vector<Value> caught_values)                               \
  F(Delegate, uint32_t depth, Control* block)                                  \
  F(CatchAll, Control* block)                                                  \
  F(AtomicOp, WasmOpcode opcode, Vector<Value> args,                           \
    const MemoryAccessImmediate<validate>& imm, Value* result)                 \
  F(AtomicFence)                                                               \
  F(MemoryInit, const MemoryInitImmediate<validate>& imm, const Value& dst,    \
    const Value& src, const Value& size)                                       \
  F(DataDrop, const DataDropImmediate<validate>& imm)                          \
  F(MemoryCopy, const MemoryCopyImmediate<validate>& imm, const Value& dst,    \
    const Value& src, const Value& size)                                       \
  F(MemoryFill, const MemoryIndexImmediate<validate>& imm, const Value& dst,   \
    const Value& value, const Value& size)                                     \
  F(TableInit, const TableInitImmediate<validate>& imm, Vector<Value> args)    \
  F(ElemDrop, const ElemDropImmediate<validate>& imm)                          \
  F(TableCopy, const TableCopyImmediate<validate>& imm, Vector<Value> args)    \
  F(TableGrow, const TableIndexImmediate<validate>& imm, const Value& value,   \
    const Value& delta, Value* result)                                         \
  F(TableSize, const TableIndexImmediate<validate>& imm, Value* result)        \
  F(TableFill, const TableIndexImmediate<validate>& imm, const Value& start,   \
    const Value& value, const Value& count)                                    \
  F(StructNewWithRtt, const StructIndexImmediate<validate>& imm,               \
    const Value& rtt, const Value args[], Value* result)                       \
  F(StructNewDefault, const StructIndexImmediate<validate>& imm,               \
    const Value& rtt, Value* result)                                           \
  F(StructGet, const Value& struct_object,                                     \
    const FieldIndexImmediate<validate>& field, bool is_signed, Value* result) \
  F(StructSet, const Value& struct_object,                                     \
    const FieldIndexImmediate<validate>& field, const Value& field_value)      \
  F(ArrayNewWithRtt, const ArrayIndexImmediate<validate>& imm,                 \
    const Value& length, const Value& initial_value, const Value& rtt,         \
    Value* result)                                                             \
  F(ArrayNewDefault, const ArrayIndexImmediate<validate>& imm,                 \
    const Value& length, const Value& rtt, Value* result)                      \
  F(ArrayGet, const Value& array_obj,                                          \
    const ArrayIndexImmediate<validate>& imm, const Value& index,              \
    bool is_signed, Value* result)                                             \
  F(ArraySet, const Value& array_obj,                                          \
    const ArrayIndexImmediate<validate>& imm, const Value& index,              \
    const Value& value)                                                        \
  F(ArrayLen, const Value& array_obj, Value* result)                           \
  F(I31New, const Value& input, Value* result)                                 \
  F(I31GetS, const Value& input, Value* result)                                \
  F(I31GetU, const Value& input, Value* result)                                \
  F(RttCanon, uint32_t type_index, Value* result)                              \
  F(RttSub, uint32_t type_index, const Value& parent, Value* result)           \
  F(RefTest, const Value& obj, const Value& rtt, Value* result)                \
  F(RefCast, const Value& obj, const Value& rtt, Value* result)                \
  F(BrOnCast, const Value& obj, const Value& rtt, Value* result_on_branch,     \
    uint32_t depth)                                                            \
  F(RefIsData, const Value& object, Value* result)                             \
  F(RefAsData, const Value& object, Value* result)                             \
  F(BrOnData, const Value& object, Value* value_on_branch, uint32_t br_depth)  \
  F(RefIsFunc, const Value& object, Value* result)                             \
  F(RefAsFunc, const Value& object, Value* result)                             \
  F(BrOnFunc, const Value& object, Value* value_on_branch, uint32_t br_depth)  \
  F(RefIsI31, const Value& object, Value* result)                              \
  F(RefAsI31, const Value& object, Value* result)                              \
  F(BrOnI31, const Value& object, Value* value_on_branch, uint32_t br_depth)   \
  F(Forward, const Value& from, Value* to)

// Generic Wasm bytecode decoder with utilities for decoding immediates,
// lengths, etc.
template <Decoder::ValidateFlag validate>
class WasmDecoder : public Decoder {
 public:
  WasmDecoder(Zone* zone, const WasmModule* module, const WasmFeatures& enabled,
              WasmFeatures* detected, const FunctionSig* sig, const byte* start,
              const byte* end, uint32_t buffer_offset = 0)
      : Decoder(start, end, buffer_offset),
        local_types_(zone),
        module_(module),
        enabled_(enabled),
        detected_(detected),
        sig_(sig) {
    if (sig_ && sig_->return_count() > 1) detected_->Add(kFeature_mv);
  }

  Zone* zone() const { return local_types_.get_allocator().zone(); }

  uint32_t num_locals() const {
    DCHECK_EQ(num_locals_, local_types_.size());
    return num_locals_;
  }

  ValueType local_type(uint32_t index) const { return local_types_[index]; }

  void InitializeLocalsFromSig() {
    DCHECK_NOT_NULL(sig_);
    DCHECK_EQ(0, this->local_types_.size());
    local_types_.assign(sig_->parameters().begin(), sig_->parameters().end());
    num_locals_ = static_cast<uint32_t>(sig_->parameters().size());
  }

  // Decodes local definitions in the current decoder.
  // Returns the number of newly defined locals, or -1 if decoding failed.
  // Writes the total length of decoded locals in {total_length}.
  // If {insert_position} is defined, the decoded locals will be inserted into
  // the {this->local_types_}. The decoder's pc is not advanced.
  int DecodeLocals(const byte* pc, uint32_t* total_length,
                   const base::Optional<uint32_t> insert_position) {
    uint32_t length;
    *total_length = 0;
    int total_count = 0;

    // The 'else' value is useless, we pass it for convenience.
    auto insert_iterator = insert_position.has_value()
                               ? local_types_.begin() + insert_position.value()
                               : local_types_.begin();

    // Decode local declarations, if any.
    uint32_t entries = read_u32v<validate>(pc, &length, "local decls count");
    if (!VALIDATE(ok())) {
      DecodeError(pc + *total_length, "invalid local decls count");
      return -1;
    }
    *total_length += length;
    TRACE("local decls count: %u\n", entries);

    while (entries-- > 0) {
      if (!VALIDATE(more())) {
        DecodeError(end(),
                    "expected more local decls but reached end of input");
        return -1;
      }

      uint32_t count =
          read_u32v<validate>(pc + *total_length, &length, "local count");
      if (!VALIDATE(ok())) {
        DecodeError(pc + *total_length, "invalid local count");
        return -1;
      }
      DCHECK_LE(local_types_.size(), kV8MaxWasmFunctionLocals);
      if (!VALIDATE(count <= kV8MaxWasmFunctionLocals - local_types_.size())) {
        DecodeError(pc + *total_length, "local count too large");
        return -1;
      }
      *total_length += length;

      ValueType type = value_type_reader::read_value_type<validate>(
          this, pc + *total_length, &length, this->module_, enabled_);
      if (!VALIDATE(type != kWasmBottom)) return -1;
      *total_length += length;
      total_count += count;

      if (insert_position.has_value()) {
        // Move the insertion iterator to the end of the newly inserted locals.
        insert_iterator =
            local_types_.insert(insert_iterator, count, type) + count;
        num_locals_ += count;
      }
    }

    DCHECK(ok());
    return total_count;
  }

  // Shorthand that forwards to the {DecodeError} functions above, passing our
  // {validate} flag.
  template <typename... Args>
  void DecodeError(Args... args) {
    wasm::DecodeError<validate>(this, std::forward<Args>(args)...);
  }

  // Returns a BitVector of length {locals_count + 1} representing the set of
  // variables that are assigned in the loop starting at {pc}. The additional
  // position at the end of the vector represents possible assignments to
  // the instance cache.
  static BitVector* AnalyzeLoopAssignment(WasmDecoder* decoder, const byte* pc,
                                          uint32_t locals_count, Zone* zone) {
    if (pc >= decoder->end()) return nullptr;
    if (*pc != kExprLoop) return nullptr;
    // The number of locals_count is augmented by 1 so that the 'locals_count'
    // index can be used to track the instance cache.
    BitVector* assigned = zone->New<BitVector>(locals_count + 1, zone);
    int depth = -1;  // We will increment the depth to 0 when we decode the
                     // starting 'loop' opcode.
    // Since 'let' can add additional locals at the beginning of the locals
    // index space, we need to track this offset for every depth up to the
    // current depth.
    base::SmallVector<uint32_t, 8> local_offsets(8);
    // Iteratively process all AST nodes nested inside the loop.
    while (pc < decoder->end() && VALIDATE(decoder->ok())) {
      WasmOpcode opcode = static_cast<WasmOpcode>(*pc);
      switch (opcode) {
        case kExprLoop:
        case kExprIf:
        case kExprBlock:
        case kExprTry:
          depth++;
          local_offsets.resize_no_init(depth + 1);
          // No additional locals.
          local_offsets[depth] = depth > 0 ? local_offsets[depth - 1] : 0;
          break;
        case kExprLet: {
          depth++;
          local_offsets.resize_no_init(depth + 1);
          BlockTypeImmediate<validate> imm(WasmFeatures::All(), decoder, pc + 1,
                                           nullptr);
          uint32_t locals_length;
          int new_locals_count = decoder->DecodeLocals(
              pc + 1 + imm.length, &locals_length, base::Optional<uint32_t>());
          local_offsets[depth] = local_offsets[depth - 1] + new_locals_count;
          break;
        }
        case kExprLocalSet:
        case kExprLocalTee: {
          LocalIndexImmediate<validate> imm(decoder, pc + 1);
          // Unverified code might have an out-of-bounds index.
          if (imm.index >= local_offsets[depth] &&
              imm.index - local_offsets[depth] < locals_count) {
            assigned->Add(imm.index - local_offsets[depth]);
          }
          break;
        }
        case kExprMemoryGrow:
        case kExprCallFunction:
        case kExprCallIndirect:
        case kExprCallRef:
          // Add instance cache to the assigned set.
          assigned->Add(locals_count);
          break;
        case kExprEnd:
          depth--;
          break;
        default:
          break;
      }
      if (depth < 0) break;
      pc += OpcodeLength(decoder, pc);
    }
    return VALIDATE(decoder->ok()) ? assigned : nullptr;
  }

  inline bool Validate(const byte* pc, LocalIndexImmediate<validate>& imm) {
    if (!VALIDATE(imm.index < num_locals())) {
      DecodeError(pc, "invalid local index: %u", imm.index);
      return false;
    }
    return true;
  }

  inline bool Complete(ExceptionIndexImmediate<validate>& imm) {
    if (!VALIDATE(imm.index < module_->exceptions.size())) return false;
    imm.exception = &module_->exceptions[imm.index];
    return true;
  }

  inline bool Validate(const byte* pc, ExceptionIndexImmediate<validate>& imm) {
    if (!Complete(imm)) {
      DecodeError(pc, "Invalid exception index: %u", imm.index);
      return false;
    }
    return true;
  }

  inline bool Validate(const byte* pc, GlobalIndexImmediate<validate>& imm) {
    if (!VALIDATE(imm.index < module_->globals.size())) {
      DecodeError(pc, "invalid global index: %u", imm.index);
      return false;
    }
    imm.global = &module_->globals[imm.index];
    imm.type = imm.global->type;
    return true;
  }

  inline bool Complete(StructIndexImmediate<validate>& imm) {
    if (!VALIDATE(module_->has_struct(imm.index))) return false;
    imm.struct_type = module_->struct_type(imm.index);
    return true;
  }

  inline bool Validate(const byte* pc, StructIndexImmediate<validate>& imm) {
    if (Complete(imm)) return true;
    DecodeError(pc, "invalid struct index: %u", imm.index);
    return false;
  }

  inline bool Validate(const byte* pc, FieldIndexImmediate<validate>& imm) {
    if (!Validate(pc, imm.struct_index)) return false;
    if (!VALIDATE(imm.index < imm.struct_index.struct_type->field_count())) {
      DecodeError(pc + imm.struct_index.length, "invalid field index: %u",
                  imm.index);
      return false;
    }
    return true;
  }

  inline bool Validate(const byte* pc, TypeIndexImmediate<validate>& imm) {
    if (!VALIDATE(module_->has_type(imm.index))) {
      DecodeError(pc, "invalid type index: %u", imm.index);
      return false;
    }
    return true;
  }

  inline bool Complete(ArrayIndexImmediate<validate>& imm) {
    if (!VALIDATE(module_->has_array(imm.index))) return false;
    imm.array_type = module_->array_type(imm.index);
    return true;
  }

  inline bool Validate(const byte* pc, ArrayIndexImmediate<validate>& imm) {
    if (!Complete(imm)) {
      DecodeError(pc, "invalid array index: %u", imm.index);
      return false;
    }
    return true;
  }

  inline bool CanReturnCall(const FunctionSig* target_sig) {
    if (target_sig == nullptr) return false;
    size_t num_returns = sig_->return_count();
    if (num_returns != target_sig->return_count()) return false;
    for (size_t i = 0; i < num_returns; ++i) {
      if (sig_->GetReturn(i) != target_sig->GetReturn(i)) return false;
    }
    return true;
  }

  inline bool Complete(CallFunctionImmediate<validate>& imm) {
    if (!VALIDATE(imm.index < module_->functions.size())) return false;
    imm.sig = module_->functions[imm.index].sig;
    if (imm.sig->return_count() > 1) {
      this->detected_->Add(kFeature_mv);
    }
    return true;
  }

  inline bool Validate(const byte* pc, CallFunctionImmediate<validate>& imm) {
    if (!Complete(imm)) {
      DecodeError(pc, "invalid function index: %u", imm.index);
      return false;
    }
    return true;
  }

  inline bool Complete(CallIndirectImmediate<validate>& imm) {
    if (!VALIDATE(module_->has_signature(imm.sig_index))) return false;
    imm.sig = module_->signature(imm.sig_index);
    if (imm.sig->return_count() > 1) {
      this->detected_->Add(kFeature_mv);
    }
    return true;
  }

  inline bool Validate(const byte* pc, CallIndirectImmediate<validate>& imm) {
    if (!VALIDATE(imm.table_index < module_->tables.size())) {
      DecodeError(pc, "call_indirect: table index immediate out of bounds");
      return false;
    }
    ValueType table_type = module_->tables[imm.table_index].type;
    if (!VALIDATE(IsSubtypeOf(table_type, kWasmFuncRef, module_))) {
      DecodeError(
          pc, "call_indirect: immediate table #%u is not of a function type",
          imm.table_index);
      return false;
    }
    if (!Complete(imm)) {
      DecodeError(pc, "invalid signature index: #%u", imm.sig_index);
      return false;
    }
    // Check that the dynamic signature for this call is a subtype of the static
    // type of the table the function is defined in.
    ValueType immediate_type = ValueType::Ref(imm.sig_index, kNonNullable);
    if (!VALIDATE(IsSubtypeOf(immediate_type, table_type, module_))) {
      DecodeError(pc,
                  "call_indirect: Immediate signature #%u is not a subtype of "
                  "immediate table #%u",
                  imm.sig_index, imm.table_index);
    }
    return true;
  }

  inline bool Validate(const byte* pc, BranchDepthImmediate<validate>& imm,
                       size_t control_depth) {
    if (!VALIDATE(imm.depth < control_depth)) {
      DecodeError(pc, "invalid branch depth: %u", imm.depth);
      return false;
    }
    return true;
  }

  inline bool Validate(const byte* pc, BranchTableImmediate<validate>& imm,
                       size_t block_depth) {
    if (!VALIDATE(imm.table_count <= kV8MaxWasmFunctionBrTableSize)) {
      DecodeError(pc, "invalid table count (> max br_table size): %u",
                  imm.table_count);
      return false;
    }
    return checkAvailable(imm.table_count);
  }

  inline bool Validate(const byte* pc,
                       BranchOnExceptionImmediate<validate>& imm,
                       size_t control_size) {
    return Validate(pc, imm.depth, control_size) &&
           Validate(pc + imm.depth.length, imm.index);
  }

  inline bool Validate(const byte* pc, WasmOpcode opcode,
                       SimdLaneImmediate<validate>& imm) {
    uint8_t num_lanes = 0;
    switch (opcode) {
      case kExprF64x2ExtractLane:
      case kExprF64x2ReplaceLane:
      case kExprI64x2ExtractLane:
      case kExprI64x2ReplaceLane:
      case kExprS128Load64Lane:
      case kExprS128Store64Lane:
        num_lanes = 2;
        break;
      case kExprF32x4ExtractLane:
      case kExprF32x4ReplaceLane:
      case kExprI32x4ExtractLane:
      case kExprI32x4ReplaceLane:
      case kExprS128Load32Lane:
      case kExprS128Store32Lane:
      case kExprI32x4WidenI8x16S:
      case kExprI32x4WidenI8x16U:
        num_lanes = 4;
        break;
      case kExprI16x8ExtractLaneS:
      case kExprI16x8ExtractLaneU:
      case kExprI16x8ReplaceLane:
      case kExprS128Load16Lane:
      case kExprS128Store16Lane:
        num_lanes = 8;
        break;
      case kExprI8x16ExtractLaneS:
      case kExprI8x16ExtractLaneU:
      case kExprI8x16ReplaceLane:
      case kExprS128Load8Lane:
      case kExprS128Store8Lane:
        num_lanes = 16;
        break;
      default:
        UNREACHABLE();
        break;
    }
    if (!VALIDATE(imm.lane >= 0 && imm.lane < num_lanes)) {
      DecodeError(pc, "invalid lane index");
      return false;
    } else {
      return true;
    }
  }

  inline bool Validate(const byte* pc, Simd128Immediate<validate>& imm) {
    uint8_t max_lane = 0;
    for (uint32_t i = 0; i < kSimd128Size; ++i) {
      max_lane = std::max(max_lane, imm.value[i]);
    }
    // Shuffle indices must be in [0..31] for a 16 lane shuffle.
    if (!VALIDATE(max_lane < 2 * kSimd128Size)) {
      DecodeError(pc, "invalid shuffle mask");
      return false;
    }
    return true;
  }

  inline bool Complete(BlockTypeImmediate<validate>& imm) {
    if (imm.type != kWasmBottom) return true;
    if (!VALIDATE(module_->has_signature(imm.sig_index))) return false;
    imm.sig = module_->signature(imm.sig_index);
    if (imm.sig->return_count() > 1) {
      this->detected_->Add(kFeature_mv);
    }
    return true;
  }

  inline bool Validate(const byte* pc, BlockTypeImmediate<validate>& imm) {
    if (!Complete(imm)) {
      DecodeError(pc, "block type index %u is not a signature definition",
                  imm.sig_index);
      return false;
    }
    return true;
  }

  inline bool Validate(const byte* pc, FunctionIndexImmediate<validate>& imm) {
    if (!VALIDATE(imm.index < module_->functions.size())) {
      DecodeError(pc, "invalid function index: %u", imm.index);
      return false;
    }
    if (!VALIDATE(module_->functions[imm.index].declared)) {
      DecodeError(pc, "undeclared reference to function #%u", imm.index);
      return false;
    }
    return true;
  }

  inline bool Validate(const byte* pc, MemoryIndexImmediate<validate>& imm) {
    if (!VALIDATE(module_->has_memory)) {
      DecodeError(pc, "memory instruction with no memory");
      return false;
    }
    return true;
  }

  inline bool Validate(const byte* pc, MemoryInitImmediate<validate>& imm) {
    if (!VALIDATE(imm.data_segment_index <
                  module_->num_declared_data_segments)) {
      DecodeError(pc, "invalid data segment index: %u", imm.data_segment_index);
      return false;
    }
    if (!Validate(pc + imm.length - imm.memory.length, imm.memory))
      return false;
    return true;
  }

  inline bool Validate(const byte* pc, DataDropImmediate<validate>& imm) {
    if (!VALIDATE(imm.index < module_->num_declared_data_segments)) {
      DecodeError(pc, "invalid data segment index: %u", imm.index);
      return false;
    }
    return true;
  }

  inline bool Validate(const byte* pc, MemoryCopyImmediate<validate>& imm) {
    return Validate(pc, imm.memory_src) &&
           Validate(pc + imm.memory_src.length, imm.memory_dst);
  }

  inline bool Validate(const byte* pc, TableIndexImmediate<validate>& imm) {
    if (!VALIDATE(imm.index < module_->tables.size())) {
      DecodeError(pc, "invalid table index: %u", imm.index);
      return false;
    }
    return true;
  }

  inline bool Validate(const byte* pc, TableInitImmediate<validate>& imm) {
    if (!VALIDATE(imm.elem_segment_index < module_->elem_segments.size())) {
      DecodeError(pc, "invalid element segment index: %u",
                  imm.elem_segment_index);
      return false;
    }
    if (!Validate(pc + imm.length - imm.table.length, imm.table)) {
      return false;
    }
    ValueType elem_type = module_->elem_segments[imm.elem_segment_index].type;
    if (!VALIDATE(IsSubtypeOf(elem_type, module_->tables[imm.table.index].type,
                              module_))) {
      DecodeError(pc, "table %u is not a super-type of %s", imm.table.index,
                  elem_type.name().c_str());
      return false;
    }
    return true;
  }

  inline bool Validate(const byte* pc, ElemDropImmediate<validate>& imm) {
    if (!VALIDATE(imm.index < module_->elem_segments.size())) {
      DecodeError(pc, "invalid element segment index: %u", imm.index);
      return false;
    }
    return true;
  }

  inline bool Validate(const byte* pc, TableCopyImmediate<validate>& imm) {
    if (!Validate(pc, imm.table_src)) return false;
    if (!Validate(pc + imm.table_src.length, imm.table_dst)) return false;
    ValueType src_type = module_->tables[imm.table_src.index].type;
    if (!VALIDATE(IsSubtypeOf(
            src_type, module_->tables[imm.table_dst.index].type, module_))) {
      DecodeError(pc, "table %u is not a super-type of %s", imm.table_dst.index,
                  src_type.name().c_str());
      return false;
    }
    return true;
  }

  // Returns the length of the opcode under {pc}.
  static uint32_t OpcodeLength(WasmDecoder* decoder, const byte* pc) {
    WasmOpcode opcode = static_cast<WasmOpcode>(*pc);
    // We don't have information about the module here, so we just assume that
    // memory64 is enabled when parsing memory access immediates. This is
    // backwards-compatible; decode errors will be detected at another time when
    // actually decoding that opcode.
    constexpr bool kConservativelyAssumeMemory64 = true;
    switch (opcode) {
      /********** Control opcodes **********/
      case kExprUnreachable:
      case kExprNop:
      case kExprNopForTestingUnsupportedInLiftoff:
      case kExprElse:
      case kExprEnd:
      case kExprReturn:
        return 1;
      case kExprTry:
      case kExprIf:
      case kExprLoop:
      case kExprBlock: {
        BlockTypeImmediate<validate> imm(WasmFeatures::All(), decoder, pc + 1,
                                         nullptr);
        return 1 + imm.length;
      }
      case kExprRethrow:
      case kExprBr:
      case kExprBrIf:
      case kExprBrOnNull:
      case kExprDelegate: {
        BranchDepthImmediate<validate> imm(decoder, pc + 1);
        return 1 + imm.length;
      }
      case kExprBrTable: {
        BranchTableImmediate<validate> imm(decoder, pc + 1);
        BranchTableIterator<validate> iterator(decoder, imm);
        return 1 + iterator.length();
      }
      case kExprThrow:
      case kExprCatch: {
        ExceptionIndexImmediate<validate> imm(decoder, pc + 1);
        return 1 + imm.length;
      }
      case kExprLet: {
        BlockTypeImmediate<validate> imm(WasmFeatures::All(), decoder, pc + 1,
                                         nullptr);
        uint32_t locals_length;
        int new_locals_count = decoder->DecodeLocals(
            pc + 1 + imm.length, &locals_length, base::Optional<uint32_t>());
        return 1 + imm.length + ((new_locals_count >= 0) ? locals_length : 0);
      }

      /********** Misc opcodes **********/
      case kExprCallFunction:
      case kExprReturnCall: {
        CallFunctionImmediate<validate> imm(decoder, pc + 1);
        return 1 + imm.length;
      }
      case kExprCallIndirect:
      case kExprReturnCallIndirect: {
        CallIndirectImmediate<validate> imm(WasmFeatures::All(), decoder,
                                            pc + 1);
        return 1 + imm.length;
      }
      case kExprCallRef:
      case kExprReturnCallRef:
      case kExprDrop:
      case kExprSelect:
      case kExprCatchAll:
        return 1;
      case kExprSelectWithType: {
        SelectTypeImmediate<validate> imm(WasmFeatures::All(), decoder, pc + 1,
                                          nullptr);
        return 1 + imm.length;
      }

      case kExprLocalGet:
      case kExprLocalSet:
      case kExprLocalTee: {
        LocalIndexImmediate<validate> imm(decoder, pc + 1);
        return 1 + imm.length;
      }
      case kExprGlobalGet:
      case kExprGlobalSet: {
        GlobalIndexImmediate<validate> imm(decoder, pc + 1);
        return 1 + imm.length;
      }
      case kExprTableGet:
      case kExprTableSet: {
        TableIndexImmediate<validate> imm(decoder, pc + 1);
        return 1 + imm.length;
      }
      case kExprI32Const: {
        ImmI32Immediate<validate> imm(decoder, pc + 1);
        return 1 + imm.length;
      }
      case kExprI64Const: {
        ImmI64Immediate<validate> imm(decoder, pc + 1);
        return 1 + imm.length;
      }
      case kExprF32Const:
        return 5;
      case kExprF64Const:
        return 9;
      case kExprRefNull: {
        HeapTypeImmediate<validate> imm(WasmFeatures::All(), decoder, pc + 1,
                                        nullptr);
        return 1 + imm.length;
      }
      case kExprRefIsNull: {
        return 1;
      }
      case kExprRefFunc: {
        FunctionIndexImmediate<validate> imm(decoder, pc + 1);
        return 1 + imm.length;
      }
      case kExprRefAsNonNull:
        return 1;

#define DECLARE_OPCODE_CASE(name, opcode, sig) case kExpr##name:
        // clang-format off
      /********** Simple and memory opcodes **********/
      FOREACH_SIMPLE_OPCODE(DECLARE_OPCODE_CASE)
      FOREACH_SIMPLE_PROTOTYPE_OPCODE(DECLARE_OPCODE_CASE)
        return 1;
      FOREACH_LOAD_MEM_OPCODE(DECLARE_OPCODE_CASE)
      FOREACH_STORE_MEM_OPCODE(DECLARE_OPCODE_CASE) {
        MemoryAccessImmediate<validate> imm(decoder, pc + 1, UINT32_MAX,
                                            kConservativelyAssumeMemory64);
        return 1 + imm.length;
      }
      // clang-format on
      case kExprMemoryGrow:
      case kExprMemorySize: {
        MemoryIndexImmediate<validate> imm(decoder, pc + 1);
        return 1 + imm.length;
      }

      /********** Prefixed opcodes **********/
      case kNumericPrefix: {
        uint32_t length = 0;
        opcode = decoder->read_prefixed_opcode<validate>(pc, &length);
        switch (opcode) {
          case kExprI32SConvertSatF32:
          case kExprI32UConvertSatF32:
          case kExprI32SConvertSatF64:
          case kExprI32UConvertSatF64:
          case kExprI64SConvertSatF32:
          case kExprI64UConvertSatF32:
          case kExprI64SConvertSatF64:
          case kExprI64UConvertSatF64:
            return length;
          case kExprMemoryInit: {
            MemoryInitImmediate<validate> imm(decoder, pc + length);
            return length + imm.length;
          }
          case kExprDataDrop: {
            DataDropImmediate<validate> imm(decoder, pc + length);
            return length + imm.length;
          }
          case kExprMemoryCopy: {
            MemoryCopyImmediate<validate> imm(decoder, pc + length);
            return length + imm.length;
          }
          case kExprMemoryFill: {
            MemoryIndexImmediate<validate> imm(decoder, pc + length);
            return length + imm.length;
          }
          case kExprTableInit: {
            TableInitImmediate<validate> imm(decoder, pc + length);
            return length + imm.length;
          }
          case kExprElemDrop: {
            ElemDropImmediate<validate> imm(decoder, pc + length);
            return length + imm.length;
          }
          case kExprTableCopy: {
            TableCopyImmediate<validate> imm(decoder, pc + length);
            return length + imm.length;
          }
          case kExprTableGrow:
          case kExprTableSize:
          case kExprTableFill: {
            TableIndexImmediate<validate> imm(decoder, pc + length);
            return length + imm.length;
          }
          default:
            if (validate) {
              decoder->DecodeError(pc, "invalid numeric opcode");
            }
            return length;
        }
      }
      case kSimdPrefix: {
        uint32_t length = 0;
        opcode = decoder->read_prefixed_opcode<validate>(pc, &length);
        switch (opcode) {
          // clang-format off
          FOREACH_SIMD_0_OPERAND_OPCODE(DECLARE_OPCODE_CASE)
            return length;
          FOREACH_SIMD_1_OPERAND_OPCODE(DECLARE_OPCODE_CASE)
            return length + 1;
          FOREACH_SIMD_MEM_OPCODE(DECLARE_OPCODE_CASE)
          case kExprPrefetchT:
          case kExprPrefetchNT: {
            MemoryAccessImmediate<validate> imm(decoder, pc + length,
                                                UINT32_MAX,
                                                kConservativelyAssumeMemory64);
            return length + imm.length;
          }
          FOREACH_SIMD_MEM_1_OPERAND_OPCODE(DECLARE_OPCODE_CASE) {
            MemoryAccessImmediate<validate> imm(
                decoder, pc + length, UINT32_MAX,
                kConservativelyAssumeMemory64);
            // 1 more byte for lane index immediate.
            return length + imm.length + 1;
          }
          // clang-format on
          // Shuffles require a byte per lane, or 16 immediate bytes.
          case kExprS128Const:
          case kExprI8x16Shuffle:
            return length + kSimd128Size;
          default:
            if (validate) {
              decoder->DecodeError(pc, "invalid SIMD opcode");
            }
            return length;
        }
      }
      case kAtomicPrefix: {
        uint32_t length = 0;
        opcode = decoder->read_prefixed_opcode<validate>(pc, &length,
                                                         "atomic_index");
        switch (opcode) {
          FOREACH_ATOMIC_OPCODE(DECLARE_OPCODE_CASE) {
            MemoryAccessImmediate<validate> imm(decoder, pc + length,
                                                UINT32_MAX,
                                                kConservativelyAssumeMemory64);
            return length + imm.length;
          }
          FOREACH_ATOMIC_0_OPERAND_OPCODE(DECLARE_OPCODE_CASE) {
            return length + 1;
          }
          default:
            if (validate) {
              decoder->DecodeError(pc, "invalid Atomics opcode");
            }
            return length;
        }
      }
      case kGCPrefix: {
        uint32_t length = 0;
        opcode =
            decoder->read_prefixed_opcode<validate>(pc, &length, "gc_index");
        switch (opcode) {
          case kExprStructNewWithRtt:
          case kExprStructNewDefault: {
            StructIndexImmediate<validate> imm(decoder, pc + length);
            return length + imm.length;
          }
          case kExprStructGet:
          case kExprStructGetS:
          case kExprStructGetU:
          case kExprStructSet: {
            FieldIndexImmediate<validate> imm(decoder, pc + length);
            return length + imm.length;
          }
          case kExprArrayNewWithRtt:
          case kExprArrayNewDefault:
          case kExprArrayGet:
          case kExprArrayGetS:
          case kExprArrayGetU:
          case kExprArraySet:
          case kExprArrayLen: {
            ArrayIndexImmediate<validate> imm(decoder, pc + length);
            return length + imm.length;
          }
          case kExprBrOnCast: {
            BranchDepthImmediate<validate> imm(decoder, pc + length);
            return length + imm.length;
          }
          case kExprRttCanon:
          case kExprRttSub: {
            TypeIndexImmediate<validate> imm(decoder, pc + length);
            return length + imm.length;
          }
          case kExprI31New:
          case kExprI31GetS:
          case kExprI31GetU:
            return length;
          case kExprRefTest:
          case kExprRefCast: {
            TypeIndexImmediate<validate> ht(decoder, pc + length);
            return length + ht.length;
          }
          default:
            // This is unreachable except for malformed modules.
            if (validate) {
              decoder->DecodeError(pc, "invalid gc opcode");
            }
            return length;
        }
      }

        // clang-format off
      /********** Asmjs opcodes **********/
      FOREACH_ASMJS_COMPAT_OPCODE(DECLARE_OPCODE_CASE)
        return 1;

      // Prefixed opcodes (already handled, included here for completeness of
      // switch)
      FOREACH_SIMD_OPCODE(DECLARE_OPCODE_CASE)
      FOREACH_NUMERIC_OPCODE(DECLARE_OPCODE_CASE)
      FOREACH_ATOMIC_OPCODE(DECLARE_OPCODE_CASE)
      FOREACH_ATOMIC_0_OPERAND_OPCODE(DECLARE_OPCODE_CASE)
      FOREACH_GC_OPCODE(DECLARE_OPCODE_CASE)
        UNREACHABLE();
        // clang-format on
#undef DECLARE_OPCODE_CASE
    }
    // Invalid modules will reach this point.
    if (validate) {
      decoder->DecodeError(pc, "invalid opcode");
    }
    return 1;
  }

  // TODO(clemensb): This is only used by the interpreter; move there.
  V8_EXPORT_PRIVATE std::pair<uint32_t, uint32_t> StackEffect(const byte* pc) {
    WasmOpcode opcode = static_cast<WasmOpcode>(*pc);
    // Handle "simple" opcodes with a fixed signature first.
    const FunctionSig* sig = WasmOpcodes::Signature(opcode);
    if (!sig) sig = WasmOpcodes::AsmjsSignature(opcode);
    if (sig) return {sig->parameter_count(), sig->return_count()};

#define DECLARE_OPCODE_CASE(name, opcode, sig) case kExpr##name:
    // clang-format off
    switch (opcode) {
      case kExprSelect:
      case kExprSelectWithType:
        return {3, 1};
      case kExprTableSet:
      FOREACH_STORE_MEM_OPCODE(DECLARE_OPCODE_CASE)
        return {2, 0};
      FOREACH_LOAD_MEM_OPCODE(DECLARE_OPCODE_CASE)
      case kExprTableGet:
      case kExprLocalTee:
      case kExprMemoryGrow:
      case kExprRefAsNonNull:
      case kExprBrOnNull:
      case kExprRefIsNull:
        return {1, 1};
      case kExprLocalSet:
      case kExprGlobalSet:
      case kExprDrop:
      case kExprBrIf:
      case kExprBrTable:
      case kExprIf:
      case kExprRethrow:
        return {1, 0};
      case kExprLocalGet:
      case kExprGlobalGet:
      case kExprI32Const:
      case kExprI64Const:
      case kExprF32Const:
      case kExprF64Const:
      case kExprRefNull:
      case kExprRefFunc:
      case kExprMemorySize:
        return {0, 1};
      case kExprCallFunction: {
        CallFunctionImmediate<validate> imm(this, pc + 1);
        CHECK(Complete(imm));
        return {imm.sig->parameter_count(), imm.sig->return_count()};
      }
      case kExprCallIndirect: {
        CallIndirectImmediate<validate> imm(this->enabled_, this, pc + 1);
        CHECK(Complete(imm));
        // Indirect calls pop an additional argument for the table index.
        return {imm.sig->parameter_count() + 1,
                imm.sig->return_count()};
      }
      case kExprThrow: {
        ExceptionIndexImmediate<validate> imm(this, pc + 1);
        CHECK(Complete(imm));
        DCHECK_EQ(0, imm.exception->sig->return_count());
        return {imm.exception->sig->parameter_count(), 0};
      }
      case kExprBr:
      case kExprBlock:
      case kExprLoop:
      case kExprEnd:
      case kExprElse:
      case kExprTry:
      case kExprCatch:
      case kExprNop:
      case kExprNopForTestingUnsupportedInLiftoff:
      case kExprReturn:
      case kExprReturnCall:
      case kExprReturnCallIndirect:
      case kExprUnreachable:
        return {0, 0};
      case kExprLet:
        // TODO(7748): Implement
        return {0, 0};
      case kNumericPrefix:
      case kAtomicPrefix:
      case kSimdPrefix: {
        opcode = this->read_prefixed_opcode<validate>(pc);
        switch (opcode) {
          FOREACH_SIMD_1_OPERAND_1_PARAM_OPCODE(DECLARE_OPCODE_CASE)
          FOREACH_SIMD_POST_MVP_ONE_OPERAND_OPCODE(DECLARE_OPCODE_CASE)
            return {1, 1};
          FOREACH_SIMD_1_OPERAND_2_PARAM_OPCODE(DECLARE_OPCODE_CASE)
          FOREACH_SIMD_MASK_OPERAND_OPCODE(DECLARE_OPCODE_CASE)
            return {2, 1};
          FOREACH_SIMD_CONST_OPCODE(DECLARE_OPCODE_CASE)
            return {0, 1};
          default: {
            sig = WasmOpcodes::Signature(opcode);
            if (sig) {
              return {sig->parameter_count(), sig->return_count()};
            } else {
              UNREACHABLE();
            }
          }
        }
      }
      case kGCPrefix: {
        opcode = this->read_prefixed_opcode<validate>(pc);
        switch (opcode) {
          case kExprStructNewDefault:
          case kExprStructGet:
          case kExprStructGetS:
          case kExprStructGetU:
          case kExprI31New:
          case kExprI31GetS:
          case kExprI31GetU:
          case kExprArrayLen:
          case kExprRttSub:
            return {1, 1};
          case kExprStructSet:
            return {2, 0};
          case kExprArrayNewDefault:
          case kExprArrayGet:
          case kExprArrayGetS:
          case kExprArrayGetU:
          case kExprRefTest:
          case kExprRefCast:
          case kExprBrOnCast:
            return {2, 1};
          case kExprArraySet:
            return {3, 0};
          case kExprRttCanon:
            return {0, 1};
          case kExprArrayNewWithRtt:
            return {3, 1};
          case kExprStructNewWithRtt: {
            StructIndexImmediate<validate> imm(this, this->pc_ + 2);
            this->Complete(imm);
            return {imm.struct_type->field_count() + 1, 1};
          }
          default:
            UNREACHABLE();
        }
      }
      default:
        FATAL("unimplemented opcode: %x (%s)", opcode,
              WasmOpcodes::OpcodeName(opcode));
        return {0, 0};
    }
#undef DECLARE_OPCODE_CASE
    // clang-format on
  }

  // The {Zone} is implicitly stored in the {ZoneAllocator} which is part of
  // this {ZoneVector}. Hence save one field and just get it from there if
  // needed (see {zone()} accessor below).
  ZoneVector<ValueType> local_types_;

  // Cached value, for speed (yes, it's measurably faster to load this value
  // than to load the start and end pointer from a vector, subtract and shift).
  uint32_t num_locals_ = 0;

  const WasmModule* module_;
  const WasmFeatures enabled_;
  WasmFeatures* detected_;
  const FunctionSig* sig_;
};

template <Decoder::ValidateFlag validate>
MemoryAccessImmediate<validate>::MemoryAccessImmediate(
    WasmDecoder<validate>* decoder, const byte* pc, uint32_t max_alignment)
    : MemoryAccessImmediate(decoder, pc, max_alignment,
                            decoder->module_->is_memory64) {}

#define CALL_INTERFACE(name, ...) interface_.name(this, ##__VA_ARGS__)
#define CALL_INTERFACE_IF_REACHABLE(name, ...)            \
  do {                                                    \
    DCHECK(!control_.empty());                            \
    DCHECK_EQ(current_code_reachable_,                    \
              this->ok() && control_.back().reachable()); \
    if (current_code_reachable_) {                        \
      interface_.name(this, ##__VA_ARGS__);               \
    }                                                     \
  } while (false)
#define CALL_INTERFACE_IF_PARENT_REACHABLE(name, ...)           \
  do {                                                          \
    DCHECK(!control_.empty());                                  \
    if (VALIDATE(this->ok()) &&                                 \
        (control_.size() == 1 || control_at(1)->reachable())) { \
      interface_.name(this, ##__VA_ARGS__);                     \
    }                                                           \
  } while (false)

template <Decoder::ValidateFlag validate, typename Interface>
class WasmFullDecoder : public WasmDecoder<validate> {
  using Value = typename Interface::Value;
  using Control = typename Interface::Control;
  using ArgVector = base::SmallVector<Value, 8>;

  // All Value types should be trivially copyable for performance. We push, pop,
  // and store them in local variables.
  ASSERT_TRIVIALLY_COPYABLE(Value);

 public:
  template <typename... InterfaceArgs>
  WasmFullDecoder(Zone* zone, const WasmModule* module,
                  const WasmFeatures& enabled, WasmFeatures* detected,
                  const FunctionBody& body, InterfaceArgs&&... interface_args)
      : WasmDecoder<validate>(zone, module, enabled, detected, body.sig,
                              body.start, body.end, body.offset),
        interface_(std::forward<InterfaceArgs>(interface_args)...),
        control_(zone) {}

  Interface& interface() { return interface_; }

  bool Decode() {
    DCHECK_EQ(stack_end_, stack_);
    DCHECK(control_.empty());
    DCHECK_LE(this->pc_, this->end_);
    DCHECK_EQ(this->num_locals(), 0);

    this->InitializeLocalsFromSig();
    uint32_t params_count = static_cast<uint32_t>(this->num_locals());
    uint32_t locals_length;
    this->DecodeLocals(this->pc(), &locals_length, params_count);
    this->consume_bytes(locals_length);
    for (uint32_t index = params_count; index < this->num_locals(); index++) {
      if (!VALIDATE(this->local_type(index).is_defaultable())) {
        this->DecodeError(
            "Cannot define function-level local of non-defaultable type %s",
            this->local_type(index).name().c_str());
        return this->TraceFailed();
      }
    }

    CALL_INTERFACE(StartFunction);
    DecodeFunctionBody();
    if (this->failed()) return TraceFailed();

    if (!VALIDATE(control_.empty())) {
      if (control_.size() > 1) {
        this->DecodeError(control_.back().pc(),
                          "unterminated control structure");
      } else {
        this->DecodeError("function body must end with \"end\" opcode");
      }
      return TraceFailed();
    }
    CALL_INTERFACE(FinishFunction);
    if (this->failed()) return TraceFailed();

    TRACE("wasm-decode ok\n\n");
    return true;
  }

  bool TraceFailed() {
    if (this->error_.offset()) {
      TRACE("wasm-error module+%-6d func+%d: %s\n\n", this->error_.offset(),
            this->GetBufferRelativeOffset(this->error_.offset()),
            this->error_.message().c_str());
    } else {
      TRACE("wasm-error: %s\n\n", this->error_.message().c_str());
    }
    return false;
  }

  const char* SafeOpcodeNameAt(const byte* pc) {
    if (!pc) return "<null>";
    if (pc >= this->end_) return "<end>";
    WasmOpcode opcode = static_cast<WasmOpcode>(*pc);
    if (!WasmOpcodes::IsPrefixOpcode(opcode)) {
      return WasmOpcodes::OpcodeName(static_cast<WasmOpcode>(opcode));
    }
    opcode = this->template read_prefixed_opcode<Decoder::kFullValidation>(pc);
    return WasmOpcodes::OpcodeName(opcode);
  }

  inline WasmCodePosition position() {
    int offset = static_cast<int>(this->pc_ - this->start_);
    DCHECK_EQ(this->pc_ - this->start_, offset);  // overflows cannot happen
    return offset;
  }

  inline uint32_t control_depth() const {
    return static_cast<uint32_t>(control_.size());
  }

  inline Control* control_at(uint32_t depth) {
    DCHECK_GT(control_.size(), depth);
    return &control_.back() - depth;
  }

  inline uint32_t stack_size() const {
    DCHECK_GE(stack_end_, stack_);
    DCHECK_GE(kMaxUInt32, stack_end_ - stack_);
    return static_cast<uint32_t>(stack_end_ - stack_);
  }

  inline Value* stack_value(uint32_t depth) {
    DCHECK_LT(0, depth);
    DCHECK_GE(stack_size(), depth);
    return stack_end_ - depth;
  }

  void SetSucceedingCodeDynamicallyUnreachable() {
    Control* current = &control_.back();
    if (current->reachable()) {
      current->reachability = kSpecOnlyReachable;
      current_code_reachable_ = false;
    }
  }

 private:
  Interface interface_;

  // The value stack, stored as individual pointers for maximum performance.
  Value* stack_ = nullptr;
  Value* stack_end_ = nullptr;
  Value* stack_capacity_end_ = nullptr;
  ASSERT_TRIVIALLY_COPYABLE(Value);

  // stack of blocks, loops, and ifs.
  ZoneVector<Control> control_;

  // Controls whether code should be generated for the current block (basically
  // a cache for {ok() && control_.back().reachable()}).
  bool current_code_reachable_ = true;

  static Value UnreachableValue(const uint8_t* pc) {
    return Value{pc, kWasmBottom};
  }

  bool CheckHasMemory() {
    if (!VALIDATE(this->module_->has_memory)) {
      this->DecodeError(this->pc_ - 1, "memory instruction with no memory");
      return false;
    }
    return true;
  }

  bool CheckSimdPostMvp(WasmOpcode opcode) {
    if (!FLAG_wasm_simd_post_mvp && WasmOpcodes::IsSimdPostMvpOpcode(opcode)) {
      this->DecodeError(
          "simd opcode not available, enable with --wasm-simd-post-mvp");
      return false;
    }
    return true;
  }

#ifdef DEBUG
  class TraceLine {
   public:
    explicit TraceLine(WasmFullDecoder* decoder) : decoder_(decoder) {
      WasmOpcode opcode = static_cast<WasmOpcode>(*decoder->pc());
      if (!WasmOpcodes::IsPrefixOpcode(opcode)) AppendOpcode(opcode);
    }

    void AppendOpcode(WasmOpcode opcode) {
      DCHECK(!WasmOpcodes::IsPrefixOpcode(opcode));
      Append(TRACE_INST_FORMAT, decoder_->startrel(decoder_->pc_),
             WasmOpcodes::OpcodeName(opcode));
    }

    ~TraceLine() {
      if (!FLAG_trace_wasm_decoder) return;
      AppendStackState();
      PrintF("%.*s\n", len_, buffer_);
    }

    // Appends a formatted string.
    PRINTF_FORMAT(2, 3)
    void Append(const char* format, ...) {
      if (!FLAG_trace_wasm_decoder) return;
      va_list va_args;
      va_start(va_args, format);
      size_t remaining_len = kMaxLen - len_;
      Vector<char> remaining_msg_space(buffer_ + len_, remaining_len);
      int len = VSNPrintF(remaining_msg_space, format, va_args);
      va_end(va_args);
      len_ += len < 0 ? remaining_len : len;
    }

   private:
    void AppendStackState() {
      DCHECK(FLAG_trace_wasm_decoder);
      Append(" ");
      for (Control& c : decoder_->control_) {
        switch (c.kind) {
          case kControlIf:
            Append("I");
            break;
          case kControlBlock:
            Append("B");
            break;
          case kControlLoop:
            Append("L");
            break;
          case kControlTry:
            Append("T");
            break;
          case kControlIfElse:
          case kControlTryCatch:
          case kControlTryCatchAll:
          case kControlLet:  // TODO(7748): Implement
            break;
        }
        if (c.start_merge.arity) Append("%u-", c.start_merge.arity);
        Append("%u", c.end_merge.arity);
        if (!c.reachable()) Append("%c", c.unreachable() ? '*' : '#');
      }
      Append(" | ");
      for (size_t i = 0; i < decoder_->stack_size(); ++i) {
        Value& val = decoder_->stack_[i];
        Append(" %c", val.type.short_name());
      }
    }

    static constexpr int kMaxLen = 512;

    char buffer_[kMaxLen];
    int len_ = 0;
    WasmFullDecoder* const decoder_;
  };
#else
  class TraceLine {
   public:
    explicit TraceLine(WasmFullDecoder*) {}

    void AppendOpcode(WasmOpcode) {}

    PRINTF_FORMAT(2, 3)
    void Append(const char* format, ...) {}
  };
#endif

#define DECODE(name)                                                     \
  static int Decode##name(WasmFullDecoder* decoder, WasmOpcode opcode) { \
    TraceLine trace_msg(decoder);                                        \
    return decoder->Decode##name##Impl(&trace_msg, opcode);              \
  }                                                                      \
  V8_INLINE int Decode##name##Impl(TraceLine* trace_msg, WasmOpcode opcode)

  DECODE(Nop) { return 1; }

  DECODE(NopForTestingUnsupportedInLiftoff) {
    if (!VALIDATE(FLAG_enable_testing_opcode_in_wasm)) {
      this->DecodeError("Invalid opcode 0x%x", opcode);
      return 0;
    }
    CALL_INTERFACE_IF_REACHABLE(NopForTestingUnsupportedInLiftoff);
    return 1;
  }

#define BUILD_SIMPLE_OPCODE(op, _, sig) \
  DECODE(op) { return BuildSimpleOperator_##sig(kExpr##op); }
  FOREACH_SIMPLE_OPCODE(BUILD_SIMPLE_OPCODE)
#undef BUILD_SIMPLE_OPCODE

  DECODE(Block) {
    BlockTypeImmediate<validate> imm(this->enabled_, this, this->pc_ + 1,
                                     this->module_);
    if (!this->Validate(this->pc_ + 1, imm)) return 0;
    ArgVector args = PopArgs(imm.sig);
    Control* block = PushControl(kControlBlock);
    SetBlockType(block, imm, args.begin());
    CALL_INTERFACE_IF_REACHABLE(Block, block);
    PushMergeValues(block, &block->start_merge);
    return 1 + imm.length;
  }

  DECODE(Rethrow) {
    CHECK_PROTOTYPE_OPCODE(eh);
    BranchDepthImmediate<validate> imm(this, this->pc_ + 1);
    if (!this->Validate(this->pc_ + 1, imm, control_.size())) return 0;
    Control* c = control_at(imm.depth);
    if (!VALIDATE(c->is_try_catchall() || c->is_try_catch())) {
      this->error("rethrow not targeting catch or catch-all");
      return 0;
    }
    CALL_INTERFACE_IF_REACHABLE(Rethrow, c);
    EndControl();
    return 1 + imm.length;
  }

  DECODE(Throw) {
    CHECK_PROTOTYPE_OPCODE(eh);
    ExceptionIndexImmediate<validate> imm(this, this->pc_ + 1);
    if (!this->Validate(this->pc_ + 1, imm)) return 0;
    ArgVector args = PopArgs(imm.exception->ToFunctionSig());
    CALL_INTERFACE_IF_REACHABLE(Throw, imm, VectorOf(args));
    EndControl();
    return 1 + imm.length;
  }

  DECODE(Try) {
    CHECK_PROTOTYPE_OPCODE(eh);
    BlockTypeImmediate<validate> imm(this->enabled_, this, this->pc_ + 1,
                                     this->module_);
    if (!this->Validate(this->pc_ + 1, imm)) return 0;
    ArgVector args = PopArgs(imm.sig);
    Control* try_block = PushControl(kControlTry);
    SetBlockType(try_block, imm, args.begin());
    CALL_INTERFACE_IF_REACHABLE(Try, try_block);
    PushMergeValues(try_block, &try_block->start_merge);
    return 1 + imm.length;
  }

  DECODE(Catch) {
    CHECK_PROTOTYPE_OPCODE(eh);
    ExceptionIndexImmediate<validate> imm(this, this->pc_ + 1);
    if (!this->Validate(this->pc_ + 1, imm)) return 0;
    if (!VALIDATE(!control_.empty())) {
      this->DecodeError("catch does not match any try");
      return 0;
    }
    Control* c = &control_.back();
    if (!VALIDATE(c->is_try())) {
      this->DecodeError("catch does not match any try");
      return 0;
    }
    if (!VALIDATE(!c->is_try_catchall())) {
      this->DecodeError("catch after catch-all for try");
      return 0;
    }
    c->kind = kControlTryCatch;
    FallThruTo(c);
    DCHECK_LE(stack_ + c->stack_depth, stack_end_);
    stack_end_ = stack_ + c->stack_depth;
    c->reachability = control_at(1)->innerReachability();
    const WasmExceptionSig* sig = imm.exception->sig;
    for (size_t i = 0, e = sig->parameter_count(); i < e; ++i) {
      Push(sig->GetParam(i));
    }
    Vector<Value> values(stack_ + c->stack_depth, sig->parameter_count());
    CALL_INTERFACE_IF_PARENT_REACHABLE(CatchException, imm, c, values);
    current_code_reachable_ = this->ok() && c->reachable();
    return 1 + imm.length;
  }

  DECODE(Delegate) {
    BranchDepthImmediate<validate> imm(this, this->pc_ + 1);
    // -1 because the current try block is not included in the count.
    if (!this->Validate(this->pc_ + 1, imm, control_depth() - 1)) return 0;
    Control* c = &control_.back();
    if (!VALIDATE(c->is_incomplete_try())) {
      this->DecodeError("delegate does not match a try");
      return 0;
    }
    // +1 because the current try block is not included in the count.
    Control* target = control_at(imm.depth + 1);
    if (imm.depth + 1 < control_depth() - 1 && !target->is_try()) {
      this->DecodeError(
          "delegate target must be a try block or the function block");
      return 0;
    }
    if (target->is_try_catch() || target->is_try_catchall()) {
      this->DecodeError(
          "cannot delegate inside the catch handler of the target");
    }
    FallThruTo(c);
    CALL_INTERFACE_IF_PARENT_REACHABLE(Delegate, imm.depth + 1, c);
    current_code_reachable_ = this->ok() && control_.back().reachable();
    EndControl();
    PopControl(c);
    return 1 + imm.length;
  }

  DECODE(CatchAll) {
    if (!VALIDATE(!control_.empty())) {
      this->error("catch-all does not match any try");
      return 0;
    }
    Control* c = &control_.back();
    if (!VALIDATE(c->is_try())) {
      this->error("catch-all does not match any try");
      return 0;
    }
    if (!VALIDATE(!c->is_try_catchall())) {
      this->error("catch-all already present for try");
      return 0;
    }
    c->kind = kControlTryCatchAll;
    FallThruTo(c);
    stack_end_ = stack_ + c->stack_depth;
    c->reachability = control_at(1)->innerReachability();
    CALL_INTERFACE_IF_PARENT_REACHABLE(CatchAll, c);
    current_code_reachable_ = this->ok() && c->reachable();
    return 1;
  }

  DECODE(BrOnNull) {
    CHECK_PROTOTYPE_OPCODE(typed_funcref);
    BranchDepthImmediate<validate> imm(this, this->pc_ + 1);
    if (!this->Validate(this->pc_ + 1, imm, control_.size())) return 0;
    Value ref_object = Pop(0);
    Control* c = control_at(imm.depth);
    TypeCheckBranchResult check_result = TypeCheckBranch(c, true);
    switch (ref_object.type.kind()) {
      case ValueType::kBottom:
        // We are in a polymorphic stack. No need to push an additional bottom
        // value.
        DCHECK(check_result != kReachableBranch);
        break;
      case ValueType::kRef: {
        // Simply forward the popped argument to the result.
        Value* result = Push(ref_object.type);
        if (V8_LIKELY(check_result == kReachableBranch)) {
          CALL_INTERFACE(Forward, ref_object, result);
        }
        break;
      }
      case ValueType::kOptRef: {
        if (V8_LIKELY(check_result == kReachableBranch)) {
          CALL_INTERFACE_IF_REACHABLE(BrOnNull, ref_object, imm.depth);
          Value* result =
              Push(ValueType::Ref(ref_object.type.heap_type(), kNonNullable));
          // The result of br_on_null has the same value as the argument (but a
          // non-nullable type).
          CALL_INTERFACE(Forward, ref_object, result);
          c->br_merge()->reached = true;
        } else {
          // Even in non-reachable code, we need to push a value of the correct
          // type to the stack.
          Push(ValueType::Ref(ref_object.type.heap_type(), kNonNullable));
        }
        break;
      }
      default:
        PopTypeError(0, ref_object, "object reference");
        return 0;
    }
    return 1 + imm.length;
  }

  DECODE(Let) {
    CHECK_PROTOTYPE_OPCODE(typed_funcref);
    BlockTypeImmediate<validate> imm(this->enabled_, this, this->pc_ + 1,
                                     this->module_);
    if (!this->Validate(this->pc_ + 1, imm)) return 0;
    // Temporarily add the let-defined values to the beginning of the function
    // locals.
    uint32_t locals_length;
    int new_locals_count =
        this->DecodeLocals(this->pc() + 1 + imm.length, &locals_length, 0);
    if (new_locals_count < 0) {
      return 0;
    }
    ArgVector let_local_values =
        PopArgs(static_cast<uint32_t>(imm.in_arity()),
                VectorOf(this->local_types_.data(), new_locals_count));
    ArgVector args = PopArgs(imm.sig);
    Control* let_block = PushControl(kControlLet, new_locals_count);
    SetBlockType(let_block, imm, args.begin());
    CALL_INTERFACE_IF_REACHABLE(Block, let_block);
    PushMergeValues(let_block, &let_block->start_merge);
    CALL_INTERFACE_IF_REACHABLE(AllocateLocals, VectorOf(let_local_values));
    return 1 + imm.length + locals_length;
  }

  DECODE(Loop) {
    BlockTypeImmediate<validate> imm(this->enabled_, this, this->pc_ + 1,
                                     this->module_);
    if (!this->Validate(this->pc_ + 1, imm)) return 0;
    ArgVector args = PopArgs(imm.sig);
    Control* block = PushControl(kControlLoop);
    SetBlockType(&control_.back(), imm, args.begin());
    CALL_INTERFACE_IF_REACHABLE(Loop, block);
    PushMergeValues(block, &block->start_merge);
    return 1 + imm.length;
  }

  DECODE(If) {
    BlockTypeImmediate<validate> imm(this->enabled_, this, this->pc_ + 1,
                                     this->module_);
    if (!this->Validate(this->pc_ + 1, imm)) return 0;
    Value cond = Pop(0, kWasmI32);
    ArgVector args = PopArgs(imm.sig);
    if (!VALIDATE(this->ok())) return 0;
    Control* if_block = PushControl(kControlIf);
    SetBlockType(if_block, imm, args.begin());
    CALL_INTERFACE_IF_REACHABLE(If, cond, if_block);
    PushMergeValues(if_block, &if_block->start_merge);
    return 1 + imm.length;
  }

  DECODE(Else) {
    if (!VALIDATE(!control_.empty())) {
      this->DecodeError("else does not match any if");
      return 0;
    }
    Control* c = &control_.back();
    if (!VALIDATE(c->is_if())) {
      this->DecodeError("else does not match an if");
      return 0;
    }
    if (!VALIDATE(c->is_onearmed_if())) {
      this->DecodeError("else already present for if");
      return 0;
    }
    if (!TypeCheckFallThru()) return 0;
    c->kind = kControlIfElse;
    CALL_INTERFACE_IF_PARENT_REACHABLE(Else, c);
    if (c->reachable()) c->end_merge.reached = true;
    PushMergeValues(c, &c->start_merge);
    c->reachability = control_at(1)->innerReachability();
    current_code_reachable_ = this->ok() && c->reachable();
    return 1;
  }

  DECODE(End) {
    if (!VALIDATE(!control_.empty())) {
      this->DecodeError("end does not match any if, try, or block");
      return 0;
    }
    Control* c = &control_.back();
    if (!VALIDATE(!c->is_incomplete_try())) {
      this->DecodeError("missing catch or catch-all in try");
      return 0;
    }
    if (c->is_onearmed_if()) {
      if (!VALIDATE(c->end_merge.arity == c->start_merge.arity)) {
        this->DecodeError(
            c->pc(), "start-arity and end-arity of one-armed if must match");
        return 0;
      }
      if (!TypeCheckOneArmedIf(c)) return 0;
    }
    if (c->is_try_catch()) {
      // Emulate catch-all + re-throw.
      FallThruTo(c);
      c->reachability = control_at(1)->innerReachability();
      CALL_INTERFACE_IF_PARENT_REACHABLE(CatchAll, c);
      current_code_reachable_ = this->ok() && control_.back().reachable();
      CALL_INTERFACE_IF_REACHABLE(Rethrow, c);
      EndControl();
    }

    if (c->is_let()) {
      this->local_types_.erase(this->local_types_.begin(),
                               this->local_types_.begin() + c->locals_count);
      this->num_locals_ -= c->locals_count;
      CALL_INTERFACE_IF_REACHABLE(DeallocateLocals, c->locals_count);
    }
    if (!TypeCheckFallThru()) return 0;

    if (control_.size() == 1) {
      // If at the last (implicit) control, check we are at end.
      if (!VALIDATE(this->pc_ + 1 == this->end_)) {
        this->DecodeError(this->pc_ + 1, "trailing code after function end");
        return 0;
      }
      // The result of the block is the return value.
      trace_msg->Append("\n" TRACE_INST_FORMAT, startrel(this->pc_),
                        "(implicit) return");
      DoReturn();
      control_.clear();
      return 1;
    }
    PopControl(c);
    return 1;
  }

  DECODE(Select) {
    Value cond = Pop(2, kWasmI32);
    Value fval = Pop(1);
    Value tval = Pop(0, fval.type);
    ValueType type = tval.type == kWasmBottom ? fval.type : tval.type;
    if (!VALIDATE(!type.is_reference_type())) {
      this->DecodeError(
          "select without type is only valid for value type inputs");
      return 0;
    }
    Value* result = Push(type);
    CALL_INTERFACE_IF_REACHABLE(Select, cond, fval, tval, result);
    return 1;
  }

  DECODE(SelectWithType) {
    CHECK_PROTOTYPE_OPCODE(reftypes);
    SelectTypeImmediate<validate> imm(this->enabled_, this, this->pc_ + 1,
                                      this->module_);
    if (this->failed()) return 0;
    Value cond = Pop(2, kWasmI32);
    Value fval = Pop(1, imm.type);
    Value tval = Pop(0, imm.type);
    Value* result = Push(imm.type);
    CALL_INTERFACE_IF_REACHABLE(Select, cond, fval, tval, result);
    return 1 + imm.length;
  }

  DECODE(Br) {
    BranchDepthImmediate<validate> imm(this, this->pc_ + 1);
    if (!this->Validate(this->pc_ + 1, imm, control_.size())) return 0;
    Control* c = control_at(imm.depth);
    TypeCheckBranchResult check_result = TypeCheckBranch(c, false);
    if (V8_LIKELY(check_result == kReachableBranch)) {
      CALL_INTERFACE_IF_REACHABLE(BrOrRet, imm.depth);
      c->br_merge()->reached = true;
    }
    EndControl();
    return 1 + imm.length;
  }

  DECODE(BrIf) {
    BranchDepthImmediate<validate> imm(this, this->pc_ + 1);
    if (!this->Validate(this->pc_ + 1, imm, control_.size())) return 0;
    Value cond = Pop(0, kWasmI32);
    Control* c = control_at(imm.depth);
    TypeCheckBranchResult check_result = TypeCheckBranch(c, true);
    if (V8_LIKELY(check_result == kReachableBranch)) {
      CALL_INTERFACE_IF_REACHABLE(BrIf, cond, imm.depth);
      c->br_merge()->reached = true;
    }
    return 1 + imm.length;
  }

  DECODE(BrTable) {
    BranchTableImmediate<validate> imm(this, this->pc_ + 1);
    BranchTableIterator<validate> iterator(this, imm);
    Value key = Pop(0, kWasmI32);
    if (this->failed()) return 0;
    if (!this->Validate(this->pc_ + 1, imm, control_.size())) return 0;

    // Cache the branch targets during the iteration, so that we can set
    // all branch targets as reachable after the {CALL_INTERFACE} call.
    std::vector<bool> br_targets(control_.size());

    // The result types of the br_table instruction. We have to check the
    // stack against these types. Only needed during validation.
    std::vector<ValueType> result_types;

    while (iterator.has_next()) {
      const uint32_t index = iterator.cur_index();
      const byte* pos = iterator.pc();
      uint32_t target = iterator.next();
      if (!VALIDATE(ValidateBrTableTarget(target, pos, index))) return 0;
      // Avoid redundant branch target checks.
      if (br_targets[target]) continue;
      br_targets[target] = true;

      if (validate) {
        if (index == 0) {
          // With the first branch target, initialize the result types.
          result_types = InitializeBrTableResultTypes(target);
        } else if (!UpdateBrTableResultTypes(&result_types, target, pos,
                                             index)) {
          return 0;
        }
      }
    }

    if (!VALIDATE(TypeCheckBrTable(result_types))) return 0;

    DCHECK(this->ok());

    if (current_code_reachable_) {
      CALL_INTERFACE(BrTable, imm, key);

      for (int i = 0, e = control_depth(); i < e; ++i) {
        if (!br_targets[i]) continue;
        control_at(i)->br_merge()->reached = true;
      }
    }

    EndControl();
    return 1 + iterator.length();
  }

  DECODE(Return) {
    if (V8_LIKELY(current_code_reachable_)) {
      if (!VALIDATE(TypeCheckReturn())) return 0;
      DoReturn();
    } else {
      // We pop all return values from the stack to check their type.
      // Since we deal with unreachable code, we do not have to keep the
      // values.
      int num_returns = static_cast<int>(this->sig_->return_count());
      for (int i = num_returns - 1; i >= 0; --i) {
        Pop(i, this->sig_->GetReturn(i));
      }
    }

    EndControl();
    return 1;
  }

  DECODE(Unreachable) {
    CALL_INTERFACE_IF_REACHABLE(Unreachable);
    EndControl();
    return 1;
  }

  DECODE(I32Const) {
    ImmI32Immediate<validate> imm(this, this->pc_ + 1);
    Value* value = Push(kWasmI32);
    CALL_INTERFACE_IF_REACHABLE(I32Const, value, imm.value);
    return 1 + imm.length;
  }

  DECODE(I64Const) {
    ImmI64Immediate<validate> imm(this, this->pc_ + 1);
    Value* value = Push(kWasmI64);
    CALL_INTERFACE_IF_REACHABLE(I64Const, value, imm.value);
    return 1 + imm.length;
  }

  DECODE(F32Const) {
    ImmF32Immediate<validate> imm(this, this->pc_ + 1);
    Value* value = Push(kWasmF32);
    CALL_INTERFACE_IF_REACHABLE(F32Const, value, imm.value);
    return 1 + imm.length;
  }

  DECODE(F64Const) {
    ImmF64Immediate<validate> imm(this, this->pc_ + 1);
    Value* value = Push(kWasmF64);
    CALL_INTERFACE_IF_REACHABLE(F64Const, value, imm.value);
    return 1 + imm.length;
  }

  DECODE(RefNull) {
    CHECK_PROTOTYPE_OPCODE(reftypes);
    HeapTypeImmediate<validate> imm(this->enabled_, this, this->pc_ + 1,
                                    this->module_);
    if (!VALIDATE(this->ok())) return 0;
    ValueType type = ValueType::Ref(imm.type, kNullable);
    Value* value = Push(type);
    CALL_INTERFACE_IF_REACHABLE(RefNull, type, value);
    return 1 + imm.length;
  }

  DECODE(RefIsNull) {
    CHECK_PROTOTYPE_OPCODE(reftypes);
    Value value = Pop(0);
    Value* result = Push(kWasmI32);
    switch (value.type.kind()) {
      case ValueType::kOptRef:
        CALL_INTERFACE_IF_REACHABLE(UnOp, kExprRefIsNull, value, result);
        return 1;
      case ValueType::kBottom:
        // We are in unreachable code, the return value does not matter.
      case ValueType::kRef:
        // For non-nullable references, the result is always false.
        CALL_INTERFACE_IF_REACHABLE(I32Const, result, 0);
        return 1;
      default:
        if (validate) {
          PopTypeError(0, value, "reference type");
          return 0;
        }
        UNREACHABLE();
    }
  }

  DECODE(RefFunc) {
    CHECK_PROTOTYPE_OPCODE(reftypes);
    FunctionIndexImmediate<validate> imm(this, this->pc_ + 1);
    if (!this->Validate(this->pc_ + 1, imm)) return 0;
    HeapType heap_type(this->enabled_.has_typed_funcref()
                           ? this->module_->functions[imm.index].sig_index
                           : HeapType::kFunc);
    Value* value = Push(ValueType::Ref(heap_type, kNonNullable));
    CALL_INTERFACE_IF_REACHABLE(RefFunc, imm.index, value);
    return 1 + imm.length;
  }

  DECODE(RefAsNonNull) {
    CHECK_PROTOTYPE_OPCODE(typed_funcref);
    Value value = Pop(0);
    switch (value.type.kind()) {
      case ValueType::kBottom:
        // We are in unreachable code. Forward the bottom value.
      case ValueType::kRef: {
        Value* result = Push(value.type);
        CALL_INTERFACE_IF_REACHABLE(Forward, value, result);
        return 1;
      }
      case ValueType::kOptRef: {
        Value* result =
            Push(ValueType::Ref(value.type.heap_type(), kNonNullable));
        CALL_INTERFACE_IF_REACHABLE(RefAsNonNull, value, result);
        return 1;
      }
      default:
        if (validate) {
          PopTypeError(0, value, "reference type");
        }
        return 0;
    }
  }

  DECODE(LocalGet) {
    LocalIndexImmediate<validate> imm(this, this->pc_ + 1);
    if (!this->Validate(this->pc_ + 1, imm)) return 0;
    Value* value = Push(this->local_type(imm.index));
    CALL_INTERFACE_IF_REACHABLE(LocalGet, value, imm);
    return 1 + imm.length;
  }

  DECODE(LocalSet) {
    LocalIndexImmediate<validate> imm(this, this->pc_ + 1);
    if (!this->Validate(this->pc_ + 1, imm)) return 0;
    Value value = Pop(0, this->local_type(imm.index));
    CALL_INTERFACE_IF_REACHABLE(LocalSet, value, imm);
    return 1 + imm.length;
  }

  DECODE(LocalTee) {
    LocalIndexImmediate<validate> imm(this, this->pc_ + 1);
    if (!this->Validate(this->pc_ + 1, imm)) return 0;
    Value value = Pop(0, this->local_type(imm.index));
    Value* result = Push(value.type);
    CALL_INTERFACE_IF_REACHABLE(LocalTee, value, result, imm);
    return 1 + imm.length;
  }

  DECODE(Drop) {
    Pop(0);
    CALL_INTERFACE_IF_REACHABLE(Drop);
    return 1;
  }

  DECODE(GlobalGet) {
    GlobalIndexImmediate<validate> imm(this, this->pc_ + 1);
    if (!this->Validate(this->pc_ + 1, imm)) return 0;
    Value* result = Push(imm.type);
    CALL_INTERFACE_IF_REACHABLE(GlobalGet, result, imm);
    return 1 + imm.length;
  }

  DECODE(GlobalSet) {
    GlobalIndexImmediate<validate> imm(this, this->pc_ + 1);
    if (!this->Validate(this->pc_ + 1, imm)) return 0;
    if (!VALIDATE(imm.global->mutability)) {
      this->DecodeError("immutable global #%u cannot be assigned", imm.index);
      return 0;
    }
    Value value = Pop(0, imm.type);
    CALL_INTERFACE_IF_REACHABLE(GlobalSet, value, imm);
    return 1 + imm.length;
  }

  DECODE(TableGet) {
    CHECK_PROTOTYPE_OPCODE(reftypes);
    TableIndexImmediate<validate> imm(this, this->pc_ + 1);
    if (!this->Validate(this->pc_ + 1, imm)) return 0;
    Value index = Pop(0, kWasmI32);
    Value* result = Push(this->module_->tables[imm.index].type);
    CALL_INTERFACE_IF_REACHABLE(TableGet, index, result, imm);
    return 1 + imm.length;
  }

  DECODE(TableSet) {
    CHECK_PROTOTYPE_OPCODE(reftypes);
    TableIndexImmediate<validate> imm(this, this->pc_ + 1);
    if (!this->Validate(this->pc_ + 1, imm)) return 0;
    Value value = Pop(1, this->module_->tables[imm.index].type);
    Value index = Pop(0, kWasmI32);
    CALL_INTERFACE_IF_REACHABLE(TableSet, index, value, imm);
    return 1 + imm.length;
  }

  DECODE(LoadMem) {
    // Hard-code the list of load types. The opcodes are highly unlikely to
    // ever change, and we have some checks here to guard against that.
    static_assert(sizeof(LoadType) == sizeof(uint8_t), "LoadType is compact");
    static constexpr uint8_t kMinOpcode = kExprI32LoadMem;
    static constexpr uint8_t kMaxOpcode = kExprI64LoadMem32U;
    static constexpr LoadType kLoadTypes[] = {
        LoadType::kI32Load,    LoadType::kI64Load,    LoadType::kF32Load,
        LoadType::kF64Load,    LoadType::kI32Load8S,  LoadType::kI32Load8U,
        LoadType::kI32Load16S, LoadType::kI32Load16U, LoadType::kI64Load8S,
        LoadType::kI64Load8U,  LoadType::kI64Load16S, LoadType::kI64Load16U,
        LoadType::kI64Load32S, LoadType::kI64Load32U};
    STATIC_ASSERT(arraysize(kLoadTypes) == kMaxOpcode - kMinOpcode + 1);
    DCHECK_LE(kMinOpcode, opcode);
    DCHECK_GE(kMaxOpcode, opcode);
    return DecodeLoadMem(kLoadTypes[opcode - kMinOpcode]);
  }

  DECODE(StoreMem) {
    // Hard-code the list of store types. The opcodes are highly unlikely to
    // ever change, and we have some checks here to guard against that.
    static_assert(sizeof(StoreType) == sizeof(uint8_t), "StoreType is compact");
    static constexpr uint8_t kMinOpcode = kExprI32StoreMem;
    static constexpr uint8_t kMaxOpcode = kExprI64StoreMem32;
    static constexpr StoreType kStoreTypes[] = {
        StoreType::kI32Store,  StoreType::kI64Store,   StoreType::kF32Store,
        StoreType::kF64Store,  StoreType::kI32Store8,  StoreType::kI32Store16,
        StoreType::kI64Store8, StoreType::kI64Store16, StoreType::kI64Store32};
    STATIC_ASSERT(arraysize(kStoreTypes) == kMaxOpcode - kMinOpcode + 1);
    DCHECK_LE(kMinOpcode, opcode);
    DCHECK_GE(kMaxOpcode, opcode);
    return DecodeStoreMem(kStoreTypes[opcode - kMinOpcode]);
  }

  DECODE(MemoryGrow) {
    if (!CheckHasMemory()) return 0;
    MemoryIndexImmediate<validate> imm(this, this->pc_ + 1);
    if (!VALIDATE(this->module_->origin == kWasmOrigin)) {
      this->DecodeError("grow_memory is not supported for asmjs modules");
      return 0;
    }
    Value value = Pop(0, kWasmI32);
    Value* result = Push(kWasmI32);
    CALL_INTERFACE_IF_REACHABLE(MemoryGrow, value, result);
    return 1 + imm.length;
  }

  DECODE(MemorySize) {
    if (!CheckHasMemory()) return 0;
    MemoryIndexImmediate<validate> imm(this, this->pc_ + 1);
    ValueType result_type = this->module_->is_memory64 ? kWasmI64 : kWasmI32;
    Value* result = Push(result_type);
    CALL_INTERFACE_IF_REACHABLE(CurrentMemoryPages, result);
    return 1 + imm.length;
  }

  DECODE(CallFunction) {
    CallFunctionImmediate<validate> imm(this, this->pc_ + 1);
    if (!this->Validate(this->pc_ + 1, imm)) return 0;
    ArgVector args = PopArgs(imm.sig);
    Value* returns = PushReturns(imm.sig);
    CALL_INTERFACE_IF_REACHABLE(CallDirect, imm, args.begin(), returns);
    return 1 + imm.length;
  }

  DECODE(CallIndirect) {
    CallIndirectImmediate<validate> imm(this->enabled_, this, this->pc_ + 1);
    if (!this->Validate(this->pc_ + 1, imm)) return 0;
    Value index = Pop(0, kWasmI32);
    ArgVector args = PopArgs(imm.sig);
    Value* returns = PushReturns(imm.sig);
    CALL_INTERFACE_IF_REACHABLE(CallIndirect, index, imm, args.begin(),
                                returns);
    return 1 + imm.length;
  }

  DECODE(ReturnCall) {
    CHECK_PROTOTYPE_OPCODE(return_call);
    CallFunctionImmediate<validate> imm(this, this->pc_ + 1);
    if (!this->Validate(this->pc_ + 1, imm)) return 0;
    if (!VALIDATE(this->CanReturnCall(imm.sig))) {
      this->DecodeError("%s: %s", WasmOpcodes::OpcodeName(kExprReturnCall),
                        "tail call return types mismatch");
      return 0;
    }
    ArgVector args = PopArgs(imm.sig);
    CALL_INTERFACE_IF_REACHABLE(ReturnCall, imm, args.begin());
    EndControl();
    return 1 + imm.length;
  }

  DECODE(ReturnCallIndirect) {
    CHECK_PROTOTYPE_OPCODE(return_call);
    CallIndirectImmediate<validate> imm(this->enabled_, this, this->pc_ + 1);
    if (!this->Validate(this->pc_ + 1, imm)) return 0;
    if (!VALIDATE(this->CanReturnCall(imm.sig))) {
      this->DecodeError("%s: %s",
                        WasmOpcodes::OpcodeName(kExprReturnCallIndirect),
                        "tail call return types mismatch");
      return 0;
    }
    Value index = Pop(0, kWasmI32);
    ArgVector args = PopArgs(imm.sig);
    CALL_INTERFACE_IF_REACHABLE(ReturnCallIndirect, index, imm, args.begin());
    EndControl();
    return 1 + imm.length;
  }

  DECODE(CallRef) {
    CHECK_PROTOTYPE_OPCODE(typed_funcref);
    Value func_ref = Pop(0);
    ValueType func_type = func_ref.type;
    if (func_type == kWasmBottom) {
      // We are in unreachable code, maintain the polymorphic stack.
      return 1;
    }
    if (!VALIDATE(func_type.is_object_reference_type() &&
                  func_type.has_index() &&
                  this->module_->has_signature(func_type.ref_index()))) {
      PopTypeError(0, func_ref, "function reference");
      return 0;
    }
    const FunctionSig* sig = this->module_->signature(func_type.ref_index());
    ArgVector args = PopArgs(sig);
    Value* returns = PushReturns(sig);
    CALL_INTERFACE_IF_REACHABLE(CallRef, func_ref, sig, func_type.ref_index(),
                                args.begin(), returns);
    return 1;
  }

  DECODE(ReturnCallRef) {
    CHECK_PROTOTYPE_OPCODE(typed_funcref);
    CHECK_PROTOTYPE_OPCODE(return_call);
    Value func_ref = Pop(0);
    ValueType func_type = func_ref.type;
    if (func_type == kWasmBottom) {
      // We are in unreachable code, maintain the polymorphic stack.
      return 1;
    }
    if (!VALIDATE(func_type.is_object_reference_type() &&
                  func_type.has_index() &&
                  this->module_->has_signature(func_type.ref_index()))) {
      PopTypeError(0, func_ref, "function reference");
      return 0;
    }
    const FunctionSig* sig = this->module_->signature(func_type.ref_index());
    ArgVector args = PopArgs(sig);
    CALL_INTERFACE_IF_REACHABLE(ReturnCallRef, func_ref, sig,
                                func_type.ref_index(), args.begin());
    EndControl();
    return 1;
  }

  DECODE(Numeric) {
    uint32_t opcode_length = 0;
    WasmOpcode full_opcode = this->template read_prefixed_opcode<validate>(
        this->pc_, &opcode_length, "numeric index");
    if (full_opcode == kExprTableGrow || full_opcode == kExprTableSize ||
        full_opcode == kExprTableFill) {
      CHECK_PROTOTYPE_OPCODE(reftypes);
    }
    trace_msg->AppendOpcode(full_opcode);
    return DecodeNumericOpcode(full_opcode, opcode_length);
  }

  DECODE(Simd) {
    CHECK_PROTOTYPE_OPCODE(simd);
    uint32_t opcode_length = 0;
    WasmOpcode full_opcode = this->template read_prefixed_opcode<validate>(
        this->pc_, &opcode_length);
    if (!VALIDATE(this->ok())) return 0;
    trace_msg->AppendOpcode(full_opcode);
    if (!CheckSimdPostMvp(full_opcode)) {
      return 0;
    }
    return DecodeSimdOpcode(full_opcode, opcode_length);
  }

  DECODE(Atomic) {
    CHECK_PROTOTYPE_OPCODE(threads);
    uint32_t opcode_length = 0;
    WasmOpcode full_opcode = this->template read_prefixed_opcode<validate>(
        this->pc_, &opcode_length, "atomic index");
    trace_msg->AppendOpcode(full_opcode);
    return DecodeAtomicOpcode(full_opcode, opcode_length);
  }

  DECODE(GC) {
    CHECK_PROTOTYPE_OPCODE(gc);
    uint32_t opcode_length = 0;
    WasmOpcode full_opcode = this->template read_prefixed_opcode<validate>(
        this->pc_, &opcode_length, "gc index");
    trace_msg->AppendOpcode(full_opcode);
    return DecodeGCOpcode(full_opcode, opcode_length);
  }

#define SIMPLE_PROTOTYPE_CASE(name, opc, sig) \
  DECODE(name) { return BuildSimplePrototypeOperator(opcode); }
  FOREACH_SIMPLE_PROTOTYPE_OPCODE(SIMPLE_PROTOTYPE_CASE)
#undef SIMPLE_PROTOTYPE_CASE

  DECODE(UnknownOrAsmJs) {
    // Deal with special asmjs opcodes.
    if (!VALIDATE(is_asmjs_module(this->module_))) {
      this->DecodeError("Invalid opcode 0x%x", opcode);
      return 0;
    }
    const FunctionSig* sig = WasmOpcodes::AsmjsSignature(opcode);
    DCHECK_NOT_NULL(sig);
    return BuildSimpleOperator(opcode, sig);
  }

#undef DECODE

  using OpcodeHandler = int (*)(WasmFullDecoder*, WasmOpcode);

  // Ideally we would use template specialization for the different opcodes, but
  // GCC does not allow to specialize templates in class scope
  // (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=85282), and specializing
  // outside the class is not allowed for non-specialized classes.
  // Hence just list all implementations explicitly here, which also gives more
  // freedom to use the same implementation for different opcodes.
#define DECODE_IMPL(opcode) DECODE_IMPL2(kExpr##opcode, opcode)
#define DECODE_IMPL2(opcode, name) \
  if (idx == opcode) return &WasmFullDecoder::Decode##name

  static constexpr OpcodeHandler GetOpcodeHandlerTableEntry(size_t idx) {
    DECODE_IMPL(Nop);
#define BUILD_SIMPLE_OPCODE(op, _, sig) DECODE_IMPL(op);
    FOREACH_SIMPLE_OPCODE(BUILD_SIMPLE_OPCODE)
#undef BUILD_SIMPLE_OPCODE
    DECODE_IMPL(Block);
    DECODE_IMPL(Rethrow);
    DECODE_IMPL(Throw);
    DECODE_IMPL(Try);
    DECODE_IMPL(Catch);
    DECODE_IMPL(Delegate);
    DECODE_IMPL(CatchAll);
    DECODE_IMPL(BrOnNull);
    DECODE_IMPL(Let);
    DECODE_IMPL(Loop);
    DECODE_IMPL(If);
    DECODE_IMPL(Else);
    DECODE_IMPL(End);
    DECODE_IMPL(Select);
    DECODE_IMPL(SelectWithType);
    DECODE_IMPL(Br);
    DECODE_IMPL(BrIf);
    DECODE_IMPL(BrTable);
    DECODE_IMPL(Return);
    DECODE_IMPL(Unreachable);
    DECODE_IMPL(NopForTestingUnsupportedInLiftoff);
    DECODE_IMPL(I32Const);
    DECODE_IMPL(I64Const);
    DECODE_IMPL(F32Const);
    DECODE_IMPL(F64Const);
    DECODE_IMPL(RefNull);
    DECODE_IMPL(RefIsNull);
    DECODE_IMPL(RefFunc);
    DECODE_IMPL(RefAsNonNull);
    DECODE_IMPL(LocalGet);
    DECODE_IMPL(LocalSet);
    DECODE_IMPL(LocalTee);
    DECODE_IMPL(Drop);
    DECODE_IMPL(GlobalGet);
    DECODE_IMPL(GlobalSet);
    DECODE_IMPL(TableGet);
    DECODE_IMPL(TableSet);
#define DECODE_LOAD_MEM(op, ...) DECODE_IMPL2(kExpr##op, LoadMem);
    FOREACH_LOAD_MEM_OPCODE(DECODE_LOAD_MEM)
#undef DECODE_LOAD_MEM
#define DECODE_STORE_MEM(op, ...) DECODE_IMPL2(kExpr##op, StoreMem);
    FOREACH_STORE_MEM_OPCODE(DECODE_STORE_MEM)
#undef DECODE_LOAD_MEM
    DECODE_IMPL(MemoryGrow);
    DECODE_IMPL(MemorySize);
    DECODE_IMPL(CallFunction);
    DECODE_IMPL(CallIndirect);
    DECODE_IMPL(ReturnCall);
    DECODE_IMPL(ReturnCallIndirect);
    DECODE_IMPL(CallRef);
    DECODE_IMPL(ReturnCallRef);
    DECODE_IMPL2(kNumericPrefix, Numeric);
    DECODE_IMPL2(kSimdPrefix, Simd);
    DECODE_IMPL2(kAtomicPrefix, Atomic);
    DECODE_IMPL2(kGCPrefix, GC);
#define SIMPLE_PROTOTYPE_CASE(name, opc, sig) DECODE_IMPL(name);
    FOREACH_SIMPLE_PROTOTYPE_OPCODE(SIMPLE_PROTOTYPE_CASE)
#undef SIMPLE_PROTOTYPE_CASE
    return &WasmFullDecoder::DecodeUnknownOrAsmJs;
  }

#undef DECODE_IMPL
#undef DECODE_IMPL2

  OpcodeHandler GetOpcodeHandler(uint8_t opcode) {
    static constexpr std::array<OpcodeHandler, 256> kOpcodeHandlers =
        base::make_array<256>(GetOpcodeHandlerTableEntry);
    return kOpcodeHandlers[opcode];
  }

  void DecodeFunctionBody() {
    TRACE("wasm-decode %p...%p (module+%u, %d bytes)\n", this->start(),
          this->end(), this->pc_offset(),
          static_cast<int>(this->end() - this->start()));

    // Set up initial function block.
    {
      Control* c = PushControl(kControlBlock);
      InitMerge(&c->start_merge, 0, [](uint32_t) -> Value { UNREACHABLE(); });
      InitMerge(&c->end_merge,
                static_cast<uint32_t>(this->sig_->return_count()),
                [&](uint32_t i) {
                  return Value{this->pc_, this->sig_->GetReturn(i)};
                });
      CALL_INTERFACE(StartFunctionBody, c);
    }

    // Decode the function body.
    while (this->pc_ < this->end_) {
      // Most operations only grow the stack by at least one element (unary and
      // binary operations, local.get, constants, ...). Thus check that there is
      // enough space for those operations centrally, and avoid any bounds
      // checks in those operations.
      EnsureStackSpace(1);
      uint8_t first_byte = *this->pc_;
      WasmOpcode opcode = static_cast<WasmOpcode>(first_byte);
      CALL_INTERFACE_IF_REACHABLE(NextInstruction, opcode);
      OpcodeHandler handler = GetOpcodeHandler(first_byte);
      int len = (*handler)(this, opcode);
      this->pc_ += len;
    }

    if (!VALIDATE(this->pc_ == this->end_)) {
      this->DecodeError("Beyond end of code");
    }
  }

  void EndControl() {
    DCHECK(!control_.empty());
    Control* current = &control_.back();
    DCHECK_LE(stack_ + current->stack_depth, stack_end_);
    stack_end_ = stack_ + current->stack_depth;
    CALL_INTERFACE_IF_REACHABLE(EndControl, current);
    current->reachability = kUnreachable;
    current_code_reachable_ = false;
  }

  template <typename func>
  void InitMerge(Merge<Value>* merge, uint32_t arity, func get_val) {
    merge->arity = arity;
    if (arity == 1) {
      merge->vals.first = get_val(0);
    } else if (arity > 1) {
      merge->vals.array = this->zone()->template NewArray<Value>(arity);
      for (uint32_t i = 0; i < arity; i++) {
        merge->vals.array[i] = get_val(i);
      }
    }
  }

  // Initializes start- and end-merges of {c} with values according to the
  // in- and out-types of {c} respectively.
  void SetBlockType(Control* c, BlockTypeImmediate<validate>& imm,
                    Value* args) {
    const byte* pc = this->pc_;
    InitMerge(&c->end_merge, imm.out_arity(), [pc, &imm](uint32_t i) {
      return Value{pc, imm.out_type(i)};
    });
    InitMerge(&c->start_merge, imm.in_arity(),
              [args](uint32_t i) { return args[i]; });
  }

  // Pops arguments as required by signature.
  V8_INLINE ArgVector PopArgs(const FunctionSig* sig) {
    int count = sig ? static_cast<int>(sig->parameter_count()) : 0;
    ArgVector args(count);
    for (int i = count - 1; i >= 0; --i) {
      args[i] = Pop(i, sig->GetParam(i));
    }
    return args;
  }

  V8_INLINE ArgVector PopArgs(const StructType* type) {
    int count = static_cast<int>(type->field_count());
    ArgVector args(count);
    for (int i = count - 1; i >= 0; i--) {
      args[i] = Pop(i, type->field(i).Unpacked());
    }
    return args;
  }

  V8_INLINE ArgVector PopArgs(uint32_t base_index,
                              Vector<ValueType> arg_types) {
    ArgVector args(arg_types.size());
    for (int i = static_cast<int>(arg_types.size()) - 1; i >= 0; i--) {
      args[i] = Pop(base_index + i, arg_types[i]);
    }
    return args;
  }

  ValueType GetReturnType(const FunctionSig* sig) {
    DCHECK_GE(1, sig->return_count());
    return sig->return_count() == 0 ? kWasmStmt : sig->GetReturn();
  }

  Control* PushControl(ControlKind kind, uint32_t locals_count = 0) {
    Reachability reachability =
        control_.empty() ? kReachable : control_.back().innerReachability();
    control_.emplace_back(kind, locals_count, stack_size(), this->pc_,
                          reachability);
    current_code_reachable_ = this->ok() && reachability == kReachable;
    return &control_.back();
  }

  void PopControl(Control* c) {
    DCHECK_EQ(c, &control_.back());
    CALL_INTERFACE_IF_PARENT_REACHABLE(PopControl, c);

    // A loop just leaves the values on the stack.
    if (!c->is_loop()) PushMergeValues(c, &c->end_merge);

    bool parent_reached =
        c->reachable() || c->end_merge.reached || c->is_onearmed_if();
    control_.pop_back();
    // If the parent block was reachable before, but the popped control does not
    // return to here, this block becomes "spec only reachable".
    if (!parent_reached) SetSucceedingCodeDynamicallyUnreachable();
    current_code_reachable_ = control_.back().reachable();
  }

  int DecodeLoadMem(LoadType type, int prefix_len = 1) {
    if (!CheckHasMemory()) return 0;
    MemoryAccessImmediate<validate> imm(this, this->pc_ + prefix_len,
                                        type.size_log_2());
    ValueType index_type = this->module_->is_memory64 ? kWasmI64 : kWasmI32;
    Value index = Pop(0, index_type);
    Value* result = Push(type.value_type());
    CALL_INTERFACE_IF_REACHABLE(LoadMem, type, imm, index, result);
    return prefix_len + imm.length;
  }

  int DecodeLoadTransformMem(LoadType type, LoadTransformationKind transform,
                             uint32_t opcode_length) {
    if (!CheckHasMemory()) return 0;
    // Load extends always load 64-bits.
    uint32_t max_alignment =
        transform == LoadTransformationKind::kExtend ? 3 : type.size_log_2();
    MemoryAccessImmediate<validate> imm(this, this->pc_ + opcode_length,
                                        max_alignment);
    ValueType index_type = this->module_->is_memory64 ? kWasmI64 : kWasmI32;
    Value index = Pop(0, index_type);
    Value* result = Push(kWasmS128);
    CALL_INTERFACE_IF_REACHABLE(LoadTransform, type, transform, imm, index,
                                result);
    return opcode_length + imm.length;
  }

  int DecodeLoadLane(WasmOpcode opcode, LoadType type, uint32_t opcode_length) {
    if (!CheckHasMemory()) return 0;
    MemoryAccessImmediate<validate> mem_imm(this, this->pc_ + opcode_length,
                                            type.size_log_2());
    SimdLaneImmediate<validate> lane_imm(
        this, this->pc_ + opcode_length + mem_imm.length);
    if (!this->Validate(this->pc_ + opcode_length, opcode, lane_imm)) return 0;
    Value v128 = Pop(1, kWasmS128);
    Value index = Pop(0, kWasmI32);

    Value* result = Push(kWasmS128);
    CALL_INTERFACE_IF_REACHABLE(LoadLane, type, v128, index, mem_imm,
                                lane_imm.lane, result);
    return opcode_length + mem_imm.length + lane_imm.length;
  }

  int DecodeStoreLane(WasmOpcode opcode, StoreType type,
                      uint32_t opcode_length) {
    if (!CheckHasMemory()) return 0;
    MemoryAccessImmediate<validate> mem_imm(this, this->pc_ + opcode_length,
                                            type.size_log_2());
    SimdLaneImmediate<validate> lane_imm(
        this, this->pc_ + opcode_length + mem_imm.length);
    if (!this->Validate(this->pc_ + opcode_length, opcode, lane_imm)) return 0;
    Value v128 = Pop(1, kWasmS128);
    Value index = Pop(0, kWasmI32);

    CALL_INTERFACE_IF_REACHABLE(StoreLane, type, mem_imm, index, v128,
                                lane_imm.lane);
    return opcode_length + mem_imm.length + lane_imm.length;
  }

  int DecodeStoreMem(StoreType store, int prefix_len = 1) {
    if (!CheckHasMemory()) return 0;
    MemoryAccessImmediate<validate> imm(this, this->pc_ + prefix_len,
                                        store.size_log_2());
    Value value = Pop(1, store.value_type());
    ValueType index_type = this->module_->is_memory64 ? kWasmI64 : kWasmI32;
    Value index = Pop(0, index_type);
    CALL_INTERFACE_IF_REACHABLE(StoreMem, store, imm, index, value);
    return prefix_len + imm.length;
  }

  bool ValidateBrTableTarget(uint32_t target, const byte* pos, int index) {
    if (!VALIDATE(target < this->control_.size())) {
      this->DecodeError(pos, "improper branch in br_table target %u (depth %u)",
                        index, target);
      return false;
    }
    return true;
  }

  std::vector<ValueType> InitializeBrTableResultTypes(uint32_t target) {
    Merge<Value>* merge = control_at(target)->br_merge();
    int br_arity = merge->arity;
    std::vector<ValueType> result(br_arity);
    for (int i = 0; i < br_arity; ++i) {
      result[i] = (*merge)[i].type;
    }
    return result;
  }

  bool UpdateBrTableResultTypes(std::vector<ValueType>* result_types,
                                uint32_t target, const byte* pos, int index) {
    Merge<Value>* merge = control_at(target)->br_merge();
    int br_arity = merge->arity;
    // First we check if the arities match.
    if (!VALIDATE(br_arity == static_cast<int>(result_types->size()))) {
      this->DecodeError(pos,
                        "inconsistent arity in br_table target %u (previous "
                        "was %zu, this one is %u)",
                        index, result_types->size(), br_arity);
      return false;
    }

    for (int i = 0; i < br_arity; ++i) {
      if (this->enabled_.has_reftypes()) {
        // The expected type is the biggest common sub type of all targets.
        (*result_types)[i] =
            CommonSubtype((*result_types)[i], (*merge)[i].type, this->module_);
      } else {
        // All target must have the same signature.
        if (!VALIDATE((*result_types)[i] == (*merge)[i].type)) {
          this->DecodeError(pos,
                            "inconsistent type in br_table target %u (previous "
                            "was %s, this one is %s)",
                            index, (*result_types)[i].name().c_str(),
                            (*merge)[i].type.name().c_str());
          return false;
        }
      }
    }
    return true;
  }

  bool TypeCheckBrTable(const std::vector<ValueType>& result_types) {
    int br_arity = static_cast<int>(result_types.size());
    if (V8_LIKELY(!control_.back().unreachable())) {
      int available =
          static_cast<int>(stack_size()) - control_.back().stack_depth;
      // There have to be enough values on the stack.
      if (!VALIDATE(available >= br_arity)) {
        this->DecodeError(
            "expected %u elements on the stack for branch to @%d, found %u",
            br_arity, startrel(control_.back().pc()), available);
        return false;
      }
      Value* stack_values = stack_end_ - br_arity;
      // Type-check the topmost br_arity values on the stack.
      for (int i = 0; i < br_arity; ++i) {
        Value& val = stack_values[i];
        if (!VALIDATE(IsSubtypeOf(val.type, result_types[i], this->module_))) {
          this->DecodeError("type error in merge[%u] (expected %s, got %s)", i,
                            result_types[i].name().c_str(),
                            val.type.name().c_str());
          return false;
        }
      }
    } else {  // !control_.back().reachable()
      // Pop values from the stack, accoring to the expected signature.
      for (int i = 0; i < br_arity; ++i) Pop(i + 1, result_types[i]);
    }
    return this->ok();
  }

  uint32_t SimdConstOp(uint32_t opcode_length) {
    Simd128Immediate<validate> imm(this, this->pc_ + opcode_length);
    auto* result = Push(kWasmS128);
    CALL_INTERFACE_IF_REACHABLE(S128Const, imm, result);
    return opcode_length + kSimd128Size;
  }

  uint32_t SimdExtractLane(WasmOpcode opcode, ValueType type,
                           uint32_t opcode_length) {
    SimdLaneImmediate<validate> imm(this, this->pc_ + opcode_length);
    if (this->Validate(this->pc_ + opcode_length, opcode, imm)) {
      Value inputs[] = {Pop(0, kWasmS128)};
      Value* result = Push(type);
      CALL_INTERFACE_IF_REACHABLE(SimdLaneOp, opcode, imm, ArrayVector(inputs),
                                  result);
    }
    return opcode_length + imm.length;
  }

  uint32_t SimdReplaceLane(WasmOpcode opcode, ValueType type,
                           uint32_t opcode_length) {
    SimdLaneImmediate<validate> imm(this, this->pc_ + opcode_length);
    if (this->Validate(this->pc_ + opcode_length, opcode, imm)) {
      Value inputs[2] = {UnreachableValue(this->pc_),
                         UnreachableValue(this->pc_)};
      inputs[1] = Pop(1, type);
      inputs[0] = Pop(0, kWasmS128);
      Value* result = Push(kWasmS128);
      CALL_INTERFACE_IF_REACHABLE(SimdLaneOp, opcode, imm, ArrayVector(inputs),
                                  result);
    }
    return opcode_length + imm.length;
  }

  uint32_t Simd8x16ShuffleOp(uint32_t opcode_length) {
    Simd128Immediate<validate> imm(this, this->pc_ + opcode_length);
    if (this->Validate(this->pc_ + opcode_length, imm)) {
      Value input1 = Pop(1, kWasmS128);
      Value input0 = Pop(0, kWasmS128);
      Value* result = Push(kWasmS128);
      CALL_INTERFACE_IF_REACHABLE(Simd8x16ShuffleOp, imm, input0, input1,
                                  result);
    }
    return opcode_length + 16;
  }

  uint32_t SimdPrefetch(uint32_t opcode_length, bool temporal) {
    if (!CheckHasMemory()) return 0;
    // Alignment doesn't matter, set to an arbitrary value.
    uint32_t max_alignment = 4;
    MemoryAccessImmediate<validate> imm(this, this->pc_ + opcode_length,
                                        max_alignment);
    ValueType index_type = this->module_->is_memory64 ? kWasmI64 : kWasmI32;
    Value index = Pop(0, index_type);
    CALL_INTERFACE_IF_REACHABLE(Prefetch, imm, index, temporal);
    return opcode_length + imm.length;
  }

  uint32_t DecodeSimdOpcode(WasmOpcode opcode, uint32_t opcode_length) {
    // opcode_length is the number of bytes that this SIMD-specific opcode takes
    // up in the LEB128 encoded form.
    switch (opcode) {
      case kExprF64x2ExtractLane:
        return SimdExtractLane(opcode, kWasmF64, opcode_length);
      case kExprF32x4ExtractLane:
        return SimdExtractLane(opcode, kWasmF32, opcode_length);
      case kExprI64x2ExtractLane:
        return SimdExtractLane(opcode, kWasmI64, opcode_length);
      case kExprI32x4ExtractLane:
      case kExprI16x8ExtractLaneS:
      case kExprI16x8ExtractLaneU:
      case kExprI8x16ExtractLaneS:
      case kExprI8x16ExtractLaneU:
        return SimdExtractLane(opcode, kWasmI32, opcode_length);
      case kExprF64x2ReplaceLane:
        return SimdReplaceLane(opcode, kWasmF64, opcode_length);
      case kExprF32x4ReplaceLane:
        return SimdReplaceLane(opcode, kWasmF32, opcode_length);
      case kExprI64x2ReplaceLane:
        return SimdReplaceLane(opcode, kWasmI64, opcode_length);
      case kExprI32x4ReplaceLane:
      case kExprI16x8ReplaceLane:
      case kExprI8x16ReplaceLane:
        return SimdReplaceLane(opcode, kWasmI32, opcode_length);
      case kExprI8x16Shuffle:
        return Simd8x16ShuffleOp(opcode_length);
      case kExprS128LoadMem:
        return DecodeLoadMem(LoadType::kS128Load, opcode_length);
      case kExprS128StoreMem:
        return DecodeStoreMem(StoreType::kS128Store, opcode_length);
      case kExprS128Load32Zero:
        return DecodeLoadTransformMem(LoadType::kI32Load,
                                      LoadTransformationKind::kZeroExtend,
                                      opcode_length);
      case kExprS128Load64Zero:
        return DecodeLoadTransformMem(LoadType::kI64Load,
                                      LoadTransformationKind::kZeroExtend,
                                      opcode_length);
      case kExprS128Load8Splat:
        return DecodeLoadTransformMem(LoadType::kI32Load8S,
                                      LoadTransformationKind::kSplat,
                                      opcode_length);
      case kExprS128Load16Splat:
        return DecodeLoadTransformMem(LoadType::kI32Load16S,
                                      LoadTransformationKind::kSplat,
                                      opcode_length);
      case kExprS128Load32Splat:
        return DecodeLoadTransformMem(
            LoadType::kI32Load, LoadTransformationKind::kSplat, opcode_length);
      case kExprS128Load64Splat:
        return DecodeLoadTransformMem(
            LoadType::kI64Load, LoadTransformationKind::kSplat, opcode_length);
      case kExprS128Load8x8S:
        return DecodeLoadTransformMem(LoadType::kI32Load8S,
                                      LoadTransformationKind::kExtend,
                                      opcode_length);
      case kExprS128Load8x8U:
        return DecodeLoadTransformMem(LoadType::kI32Load8U,
                                      LoadTransformationKind::kExtend,
                                      opcode_length);
      case kExprS128Load16x4S:
        return DecodeLoadTransformMem(LoadType::kI32Load16S,
                                      LoadTransformationKind::kExtend,
                                      opcode_length);
      case kExprS128Load16x4U:
        return DecodeLoadTransformMem(LoadType::kI32Load16U,
                                      LoadTransformationKind::kExtend,
                                      opcode_length);
      case kExprS128Load32x2S:
        return DecodeLoadTransformMem(LoadType::kI64Load32S,
                                      LoadTransformationKind::kExtend,
                                      opcode_length);
      case kExprS128Load32x2U:
        return DecodeLoadTransformMem(LoadType::kI64Load32U,
                                      LoadTransformationKind::kExtend,
                                      opcode_length);
      case kExprS128Load8Lane: {
        return DecodeLoadLane(opcode, LoadType::kI32Load8S, opcode_length);
      }
      case kExprS128Load16Lane: {
        return DecodeLoadLane(opcode, LoadType::kI32Load16S, opcode_length);
      }
      case kExprS128Load32Lane: {
        return DecodeLoadLane(opcode, LoadType::kI32Load, opcode_length);
      }
      case kExprS128Load64Lane: {
        return DecodeLoadLane(opcode, LoadType::kI64Load, opcode_length);
      }
      case kExprS128Store8Lane: {
        return DecodeStoreLane(opcode, StoreType::kI32Store8, opcode_length);
      }
      case kExprS128Store16Lane: {
        return DecodeStoreLane(opcode, StoreType::kI32Store16, opcode_length);
      }
      case kExprS128Store32Lane: {
        return DecodeStoreLane(opcode, StoreType::kI32Store, opcode_length);
      }
      case kExprS128Store64Lane: {
        return DecodeStoreLane(opcode, StoreType::kI64Store, opcode_length);
      }
      case kExprS128Const:
        return SimdConstOp(opcode_length);
      case kExprPrefetchT: {
        return SimdPrefetch(opcode_length, /*temporal=*/true);
      }
      case kExprPrefetchNT: {
        return SimdPrefetch(opcode_length, /*temporal=*/false);
      }
      case kExprI32x4WidenI8x16S:
      case kExprI32x4WidenI8x16U: {
        return SimdExtractLane(opcode, kWasmS128, opcode_length);
      }
      default: {
        const FunctionSig* sig = WasmOpcodes::Signature(opcode);
        if (!VALIDATE(sig != nullptr)) {
          this->DecodeError("invalid simd opcode");
          return 0;
        }
        ArgVector args = PopArgs(sig);
        Value* results =
            sig->return_count() == 0 ? nullptr : Push(GetReturnType(sig));
        CALL_INTERFACE_IF_REACHABLE(SimdOp, opcode, VectorOf(args), results);
        return opcode_length;
      }
    }
  }

  int DecodeGCOpcode(WasmOpcode opcode, uint32_t opcode_length) {
    switch (opcode) {
      case kExprStructNewWithRtt: {
        StructIndexImmediate<validate> imm(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, imm)) return 0;
        Value rtt = Pop(imm.struct_type->field_count());
        if (!VALIDATE(rtt.type.is_rtt() || rtt.type.is_bottom())) {
          PopTypeError(imm.struct_type->field_count(), rtt, "rtt");
          return 0;
        }
        // TODO(7748): Drop this check if {imm} is dropped from the proposal
        // à la https://github.com/WebAssembly/function-references/pull/31.
        if (!VALIDATE(
                rtt.type.is_bottom() ||
                (rtt.type.ref_index() == imm.index && rtt.type.has_depth()))) {
          PopTypeError(imm.struct_type->field_count(), rtt,
                       "rtt for type " + std::to_string(imm.index));
          return 0;
        }
        ArgVector args = PopArgs(imm.struct_type);
        Value* value = Push(ValueType::Ref(imm.index, kNonNullable));
        CALL_INTERFACE_IF_REACHABLE(StructNewWithRtt, imm, rtt, args.begin(),
                                    value);
        return opcode_length + imm.length;
      }
      case kExprStructNewDefault: {
        StructIndexImmediate<validate> imm(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, imm)) return 0;
        if (validate) {
          for (uint32_t i = 0; i < imm.struct_type->field_count(); i++) {
            ValueType ftype = imm.struct_type->field(i);
            if (!VALIDATE(ftype.is_defaultable())) {
              this->DecodeError(
                  "struct.new_default_with_rtt: immediate struct type %d has "
                  "field %d of non-defaultable type %s",
                  imm.index, i, ftype.name().c_str());
              return 0;
            }
          }
        }
        Value rtt = Pop(0);
        if (!VALIDATE(rtt.type.is_rtt() || rtt.type.is_bottom())) {
          PopTypeError(0, rtt, "rtt");
          return 0;
        }
        // TODO(7748): Drop this check if {imm} is dropped from the proposal
        // à la https://github.com/WebAssembly/function-references/pull/31.
        if (!VALIDATE(
                rtt.type.is_bottom() ||
                (rtt.type.ref_index() == imm.index && rtt.type.has_depth()))) {
          PopTypeError(0, rtt, "rtt for type " + std::to_string(imm.index));
          return 0;
        }
        Value* value = Push(ValueType::Ref(imm.index, kNonNullable));
        CALL_INTERFACE_IF_REACHABLE(StructNewDefault, imm, rtt, value);
        return opcode_length + imm.length;
      }
      case kExprStructGet: {
        FieldIndexImmediate<validate> field(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, field)) return 0;
        ValueType field_type =
            field.struct_index.struct_type->field(field.index);
        if (!VALIDATE(!field_type.is_packed())) {
          this->DecodeError(
              "struct.get: Immediate field %d of type %d has packed type %s. "
              "Use struct.get_s or struct.get_u instead.",
              field.index, field.struct_index.index, field_type.name().c_str());
          return 0;
        }
        Value struct_obj =
            Pop(0, ValueType::Ref(field.struct_index.index, kNullable));
        Value* value = Push(field_type);
        CALL_INTERFACE_IF_REACHABLE(StructGet, struct_obj, field, true, value);
        return opcode_length + field.length;
      }
      case kExprStructGetU:
      case kExprStructGetS: {
        FieldIndexImmediate<validate> field(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, field)) return 0;
        ValueType field_type =
            field.struct_index.struct_type->field(field.index);
        if (!VALIDATE(field_type.is_packed())) {
          this->DecodeError(
              "%s: Immediate field %d of type %d has non-packed type %s. Use "
              "struct.get instead.",
              WasmOpcodes::OpcodeName(opcode), field.index,
              field.struct_index.index, field_type.name().c_str());
          return 0;
        }
        Value struct_obj =
            Pop(0, ValueType::Ref(field.struct_index.index, kNullable));
        Value* value = Push(field_type.Unpacked());
        CALL_INTERFACE_IF_REACHABLE(StructGet, struct_obj, field,
                                    opcode == kExprStructGetS, value);
        return opcode_length + field.length;
      }
      case kExprStructSet: {
        FieldIndexImmediate<validate> field(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, field)) return 0;
        const StructType* struct_type = field.struct_index.struct_type;
        if (!VALIDATE(struct_type->mutability(field.index))) {
          this->DecodeError("struct.set: Field %d of type %d is immutable.",
                            field.index, field.struct_index.index);
          return 0;
        }
        Value field_value = Pop(1, struct_type->field(field.index).Unpacked());
        Value struct_obj =
            Pop(0, ValueType::Ref(field.struct_index.index, kNullable));
        CALL_INTERFACE_IF_REACHABLE(StructSet, struct_obj, field, field_value);
        return opcode_length + field.length;
      }
      case kExprArrayNewWithRtt: {
        ArrayIndexImmediate<validate> imm(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, imm)) return 0;
        Value rtt = Pop(2);
        if (!VALIDATE(rtt.type.is_rtt() || rtt.type.is_bottom())) {
          PopTypeError(2, rtt, "rtt");
          return 0;
        }
        // TODO(7748): Drop this check if {imm} is dropped from the proposal
        // à la https://github.com/WebAssembly/function-references/pull/31.
        if (!VALIDATE(
                rtt.type.is_bottom() ||
                (rtt.type.ref_index() == imm.index && rtt.type.has_depth()))) {
          PopTypeError(2, rtt, "rtt for type " + std::to_string(imm.index));
          return 0;
        }
        Value length = Pop(1, kWasmI32);
        Value initial_value = Pop(0, imm.array_type->element_type().Unpacked());
        Value* value = Push(ValueType::Ref(imm.index, kNonNullable));
        CALL_INTERFACE_IF_REACHABLE(ArrayNewWithRtt, imm, length, initial_value,
                                    rtt, value);
        return opcode_length + imm.length;
      }
      case kExprArrayNewDefault: {
        ArrayIndexImmediate<validate> imm(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, imm)) return 0;
        if (!VALIDATE(imm.array_type->element_type().is_defaultable())) {
          this->DecodeError(
              "array.new_default_with_rtt: immediate array type %d has "
              "non-defaultable element type %s",
              imm.index, imm.array_type->element_type().name().c_str());
          return 0;
        }
        Value rtt = Pop(1);
        if (!VALIDATE(rtt.type.is_rtt() || rtt.type.is_bottom())) {
          PopTypeError(1, rtt, "rtt");
          return 0;
        }
        // TODO(7748): Drop this check if {imm} is dropped from the proposal
        // à la https://github.com/WebAssembly/function-references/pull/31.
        if (!VALIDATE(
                rtt.type.is_bottom() ||
                (rtt.type.ref_index() == imm.index && rtt.type.has_depth()))) {
          PopTypeError(1, rtt, "rtt for type " + std::to_string(imm.index));
          return 0;
        }
        Value length = Pop(0, kWasmI32);
        Value* value = Push(ValueType::Ref(imm.index, kNonNullable));
        CALL_INTERFACE_IF_REACHABLE(ArrayNewDefault, imm, length, rtt, value);
        return opcode_length + imm.length;
      }
      case kExprArrayGetS:
      case kExprArrayGetU: {
        ArrayIndexImmediate<validate> imm(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, imm)) return 0;
        if (!VALIDATE(imm.array_type->element_type().is_packed())) {
          this->DecodeError(
              "%s: Immediate array type %d has non-packed type %s. Use "
              "array.get instead.",
              WasmOpcodes::OpcodeName(opcode), imm.index,
              imm.array_type->element_type().name().c_str());
          return 0;
        }
        Value index = Pop(1, kWasmI32);
        Value array_obj = Pop(0, ValueType::Ref(imm.index, kNullable));
        Value* value = Push(imm.array_type->element_type().Unpacked());
        CALL_INTERFACE_IF_REACHABLE(ArrayGet, array_obj, imm, index,
                                    opcode == kExprArrayGetS, value);
        return opcode_length + imm.length;
      }
      case kExprArrayGet: {
        ArrayIndexImmediate<validate> imm(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, imm)) return 0;
        if (!VALIDATE(!imm.array_type->element_type().is_packed())) {
          this->DecodeError(
              "array.get: Immediate array type %d has packed type %s. Use "
              "array.get_s or array.get_u instead.",
              imm.index, imm.array_type->element_type().name().c_str());
          return 0;
        }
        Value index = Pop(1, kWasmI32);
        Value array_obj = Pop(0, ValueType::Ref(imm.index, kNullable));
        Value* value = Push(imm.array_type->element_type());
        CALL_INTERFACE_IF_REACHABLE(ArrayGet, array_obj, imm, index, true,
                                    value);
        return opcode_length + imm.length;
      }
      case kExprArraySet: {
        ArrayIndexImmediate<validate> imm(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, imm)) return 0;
        if (!VALIDATE(imm.array_type->mutability())) {
          this->DecodeError("array.set: immediate array type %d is immutable",
                            imm.index);
          return 0;
        }
        Value value = Pop(2, imm.array_type->element_type().Unpacked());
        Value index = Pop(1, kWasmI32);
        Value array_obj = Pop(0, ValueType::Ref(imm.index, kNullable));
        CALL_INTERFACE_IF_REACHABLE(ArraySet, array_obj, imm, index, value);
        return opcode_length + imm.length;
      }
      case kExprArrayLen: {
        ArrayIndexImmediate<validate> imm(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, imm)) return 0;
        Value array_obj = Pop(0, ValueType::Ref(imm.index, kNullable));
        Value* value = Push(kWasmI32);
        CALL_INTERFACE_IF_REACHABLE(ArrayLen, array_obj, value);
        return opcode_length + imm.length;
      }
      case kExprI31New: {
        Value input = Pop(0, kWasmI32);
        Value* value = Push(kWasmI31Ref);
        CALL_INTERFACE_IF_REACHABLE(I31New, input, value);
        return opcode_length;
      }
      case kExprI31GetS: {
        Value i31 = Pop(0, kWasmI31Ref);
        Value* value = Push(kWasmI32);
        CALL_INTERFACE_IF_REACHABLE(I31GetS, i31, value);
        return opcode_length;
      }
      case kExprI31GetU: {
        Value i31 = Pop(0, kWasmI31Ref);
        Value* value = Push(kWasmI32);
        CALL_INTERFACE_IF_REACHABLE(I31GetU, i31, value);
        return opcode_length;
      }
      case kExprRttCanon: {
        TypeIndexImmediate<validate> imm(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, imm)) return 0;
        Value* value = Push(ValueType::Rtt(imm.index, 0));
        CALL_INTERFACE_IF_REACHABLE(RttCanon, imm.index, value);
        return opcode_length + imm.length;
      }
      case kExprRttSub: {
        TypeIndexImmediate<validate> imm(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, imm)) return 0;
        Value parent = Pop(0);
        if (parent.type.is_bottom()) {
          Push(kWasmBottom);
        } else {
          if (!VALIDATE(parent.type.is_rtt() &&
                        IsHeapSubtypeOf(imm.index, parent.type.ref_index(),
                                        this->module_))) {
            PopTypeError(
                0, parent,
                "rtt for a supertype of type " + std::to_string(imm.index));
            return 0;
          }
          Value* value =
              Push(ValueType::Rtt(imm.index, parent.type.depth() + 1));

          CALL_INTERFACE_IF_REACHABLE(RttSub, imm.index, parent, value);
        }
        return opcode_length + imm.length;
      }
      case kExprRefTest: {
        // "Tests whether {obj}'s runtime type is a runtime subtype of {rtt}."
        Value rtt = Pop(1);
        Value obj = Pop(0);
        Value* value = Push(kWasmI32);
        if (!VALIDATE(rtt.type.is_rtt() || rtt.type.is_bottom())) {
          PopTypeError(1, rtt, "rtt");
          return 0;
        }
        if (!VALIDATE(IsSubtypeOf(obj.type, kWasmFuncRef, this->module_) ||
                      IsSubtypeOf(obj.type,
                                  ValueType::Ref(HeapType::kData, kNullable),
                                  this->module_) ||
                      obj.type.is_bottom())) {
          PopTypeError(0, obj, "subtype of (ref null func) or (ref null data)");
          return 0;
        }
        if (!obj.type.is_bottom() && !rtt.type.is_bottom()) {
          if (!VALIDATE(IsSubtypeOf(
                  ValueType::Ref(rtt.type.ref_index(), kNonNullable), obj.type,
                  this->module_))) {
            PopTypeError(
                0, obj,
                "supertype of type " + std::to_string(rtt.type.ref_index()));
            return 0;
          }
          CALL_INTERFACE_IF_REACHABLE(RefTest, obj, rtt, value);
        }
        return opcode_length;
      }
      case kExprRefCast: {
        Value rtt = Pop(1);
        Value obj = Pop(0);
        if (!VALIDATE(rtt.type.is_rtt() || rtt.type.is_bottom())) {
          PopTypeError(1, rtt, "rtt");
          return 0;
        }
        if (!VALIDATE(IsSubtypeOf(obj.type, kWasmFuncRef, this->module_) ||
                      IsSubtypeOf(obj.type,
                                  ValueType::Ref(HeapType::kData, kNullable),
                                  this->module_) ||
                      obj.type.is_bottom())) {
          PopTypeError(0, obj, "subtype of (ref null func) or (ref null data)");
          return 0;
        }
        if (!obj.type.is_bottom() && !rtt.type.is_bottom()) {
          if (!VALIDATE(IsSubtypeOf(
                  ValueType::Ref(rtt.type.ref_index(), kNonNullable), obj.type,
                  this->module_))) {
            PopTypeError(
                0, obj,
                "supertype of type " + std::to_string(rtt.type.ref_index()));
            return 0;
          }
          Value* value = Push(
              ValueType::Ref(rtt.type.ref_index(), obj.type.nullability()));
          CALL_INTERFACE_IF_REACHABLE(RefCast, obj, rtt, value);
        }
        return opcode_length;
      }
      case kExprBrOnCast: {
        BranchDepthImmediate<validate> branch_depth(this,
                                                    this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, branch_depth,
                            control_.size())) {
          return 0;
        }
        Value rtt = Pop(1);
        if (!VALIDATE(rtt.type.is_rtt() || rtt.type.is_bottom())) {
          PopTypeError(1, rtt, "rtt");
          return 0;
        }
        Value obj = Pop(0);
        if (!VALIDATE(IsSubtypeOf(obj.type, kWasmFuncRef, this->module_) ||
                      IsSubtypeOf(obj.type,
                                  ValueType::Ref(HeapType::kData, kNullable),
                                  this->module_) ||
                      obj.type.is_bottom())) {
          PopTypeError(0, obj, "subtype of (ref null func) or (ref null data)");
          return 0;
        }
        // The static type of {obj} must be a supertype of {rtt}'s type.
        if (!VALIDATE(rtt.type.is_bottom() || obj.type.is_bottom() ||
                      IsHeapSubtypeOf(rtt.type.ref_index(),
                                      obj.type.heap_representation(),
                                      this->module_))) {
          PopTypeError(1, rtt, obj.type);
          return 0;
        }
        Control* c = control_at(branch_depth.depth);
        Value* result_on_branch =
            Push(rtt.type.is_bottom()
                     ? kWasmBottom
                     : ValueType::Ref(rtt.type.ref_index(), kNonNullable));
        TypeCheckBranchResult check_result = TypeCheckBranch(c, true);
        if (V8_LIKELY(check_result == kReachableBranch)) {
          CALL_INTERFACE(BrOnCast, obj, rtt, result_on_branch,
                         branch_depth.depth);
          c->br_merge()->reached = true;
        } else if (check_result == kInvalidStack) {
          return 0;
        }
        Pop(0);  // Drop {result_on_branch}, restore original value.
        Value* result_on_fallthrough = Push(obj.type);
        *result_on_fallthrough = obj;
        return opcode_length + branch_depth.length;
      }
#define ABSTRACT_TYPE_CHECK(heap_type)                          \
  case kExprRefIs##heap_type: {                                 \
    Value arg = Pop(0, kWasmAnyRef);                            \
    Value* result = Push(kWasmI32);                             \
    CALL_INTERFACE_IF_REACHABLE(RefIs##heap_type, arg, result); \
    return opcode_length;                                       \
  }

        ABSTRACT_TYPE_CHECK(Data)
        ABSTRACT_TYPE_CHECK(Func)
        ABSTRACT_TYPE_CHECK(I31)
#undef ABSTRACT_TYPE_CHECK

#define ABSTRACT_TYPE_CAST(heap_type)                                 \
  case kExprRefAs##heap_type: {                                       \
    Value arg = Pop(0, kWasmAnyRef);                                  \
    if (!arg.type.is_bottom()) {                                      \
      Value* result =                                                 \
          Push(ValueType::Ref(HeapType::k##heap_type, kNonNullable)); \
      CALL_INTERFACE_IF_REACHABLE(RefAs##heap_type, arg, result);     \
    }                                                                 \
    return opcode_length;                                             \
  }

        ABSTRACT_TYPE_CAST(Data)
        ABSTRACT_TYPE_CAST(Func)
        ABSTRACT_TYPE_CAST(I31)
#undef ABSTRACT_TYPE_CAST

      case kExprBrOnData:
      case kExprBrOnFunc:
      case kExprBrOnI31: {
        BranchDepthImmediate<validate> branch_depth(this,
                                                    this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, branch_depth,
                            control_.size())) {
          return 0;
        }

        Value obj = Pop(0, kWasmAnyRef);
        Control* c = control_at(branch_depth.depth);
        HeapType::Representation heap_type =
            opcode == kExprBrOnFunc
                ? HeapType::kFunc
                : opcode == kExprBrOnData ? HeapType::kData : HeapType::kI31;

        Value* result_on_branch = Push(ValueType::Ref(heap_type, kNonNullable));
        TypeCheckBranchResult check_result = TypeCheckBranch(c, true);
        if (V8_LIKELY(check_result == kReachableBranch)) {
          if (opcode == kExprBrOnFunc) {
            CALL_INTERFACE(BrOnFunc, obj, result_on_branch, branch_depth.depth);
          } else if (opcode == kExprBrOnData) {
            CALL_INTERFACE(BrOnData, obj, result_on_branch, branch_depth.depth);
          } else {
            CALL_INTERFACE(BrOnI31, obj, result_on_branch, branch_depth.depth);
          }
          c->br_merge()->reached = true;
        } else if (check_result == kInvalidStack) {
          return 0;
        }
        Pop(0);  // Drop {result_on_branch}, restore original value.
        Value* result_on_fallthrough = Push(obj.type);
        *result_on_fallthrough = obj;
        return opcode_length + branch_depth.length;
      }
      default:
        this->DecodeError("invalid gc opcode");
        return 0;
    }
  }

  uint32_t DecodeAtomicOpcode(WasmOpcode opcode, uint32_t opcode_length) {
    ValueType ret_type;
    const FunctionSig* sig = WasmOpcodes::Signature(opcode);
    if (!VALIDATE(sig != nullptr)) {
      this->DecodeError("invalid atomic opcode");
      return 0;
    }
    MachineType memtype;
    switch (opcode) {
#define CASE_ATOMIC_STORE_OP(Name, Type)          \
  case kExpr##Name: {                             \
    memtype = MachineType::Type();                \
    ret_type = kWasmStmt;                         \
    break; /* to generic mem access code below */ \
  }
      ATOMIC_STORE_OP_LIST(CASE_ATOMIC_STORE_OP)
#undef CASE_ATOMIC_OP
#define CASE_ATOMIC_OP(Name, Type)                \
  case kExpr##Name: {                             \
    memtype = MachineType::Type();                \
    ret_type = GetReturnType(sig);                \
    break; /* to generic mem access code below */ \
  }
      ATOMIC_OP_LIST(CASE_ATOMIC_OP)
#undef CASE_ATOMIC_OP
      case kExprAtomicFence: {
        byte zero =
            this->template read_u8<validate>(this->pc_ + opcode_length, "zero");
        if (!VALIDATE(zero == 0)) {
          this->DecodeError(this->pc_ + opcode_length,
                            "invalid atomic operand");
          return 0;
        }
        CALL_INTERFACE_IF_REACHABLE(AtomicFence);
        return 1 + opcode_length;
      }
      default:
        this->DecodeError("invalid atomic opcode");
        return 0;
    }
    if (!CheckHasMemory()) return 0;
    MemoryAccessImmediate<validate> imm(
        this, this->pc_ + opcode_length,
        ElementSizeLog2Of(memtype.representation()));
    // TODO(10949): Fix this for memory64 (index type should be kWasmI64
    // then).
    CHECK(!this->module_->is_memory64);
    ArgVector args = PopArgs(sig);
    Value* result = ret_type == kWasmStmt ? nullptr : Push(GetReturnType(sig));
    CALL_INTERFACE_IF_REACHABLE(AtomicOp, opcode, VectorOf(args), imm, result);
    return opcode_length + imm.length;
  }

  unsigned DecodeNumericOpcode(WasmOpcode opcode, uint32_t opcode_length) {
    const FunctionSig* sig = WasmOpcodes::Signature(opcode);
    if (!VALIDATE(sig != nullptr)) {
      this->DecodeError("invalid numeric opcode");
      return 0;
    }
    switch (opcode) {
      case kExprI32SConvertSatF32:
      case kExprI32UConvertSatF32:
      case kExprI32SConvertSatF64:
      case kExprI32UConvertSatF64:
      case kExprI64SConvertSatF32:
      case kExprI64UConvertSatF32:
      case kExprI64SConvertSatF64:
      case kExprI64UConvertSatF64: {
        BuildSimpleOperator(opcode, sig);
        return opcode_length;
      }
      case kExprMemoryInit: {
        MemoryInitImmediate<validate> imm(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, imm)) return 0;
        Value size = Pop(2, sig->GetParam(2));
        Value src = Pop(1, sig->GetParam(1));
        Value dst = Pop(0, sig->GetParam(0));
        CALL_INTERFACE_IF_REACHABLE(MemoryInit, imm, dst, src, size);
        return opcode_length + imm.length;
      }
      case kExprDataDrop: {
        DataDropImmediate<validate> imm(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, imm)) return 0;
        CALL_INTERFACE_IF_REACHABLE(DataDrop, imm);
        return opcode_length + imm.length;
      }
      case kExprMemoryCopy: {
        MemoryCopyImmediate<validate> imm(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, imm)) return 0;
        Value size = Pop(2, sig->GetParam(2));
        Value src = Pop(1, sig->GetParam(1));
        Value dst = Pop(0, sig->GetParam(0));
        CALL_INTERFACE_IF_REACHABLE(MemoryCopy, imm, dst, src, size);
        return opcode_length + imm.length;
      }
      case kExprMemoryFill: {
        MemoryIndexImmediate<validate> imm(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, imm)) return 0;
        Value size = Pop(2, sig->GetParam(2));
        Value value = Pop(1, sig->GetParam(1));
        Value dst = Pop(0, sig->GetParam(0));
        CALL_INTERFACE_IF_REACHABLE(MemoryFill, imm, dst, value, size);
        return opcode_length + imm.length;
      }
      case kExprTableInit: {
        TableInitImmediate<validate> imm(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, imm)) return 0;
        ArgVector args = PopArgs(sig);
        CALL_INTERFACE_IF_REACHABLE(TableInit, imm, VectorOf(args));
        return opcode_length + imm.length;
      }
      case kExprElemDrop: {
        ElemDropImmediate<validate> imm(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, imm)) return 0;
        CALL_INTERFACE_IF_REACHABLE(ElemDrop, imm);
        return opcode_length + imm.length;
      }
      case kExprTableCopy: {
        TableCopyImmediate<validate> imm(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, imm)) return 0;
        ArgVector args = PopArgs(sig);
        CALL_INTERFACE_IF_REACHABLE(TableCopy, imm, VectorOf(args));
        return opcode_length + imm.length;
      }
      case kExprTableGrow: {
        TableIndexImmediate<validate> imm(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, imm)) return 0;
        Value delta = Pop(1, sig->GetParam(1));
        Value value = Pop(0, this->module_->tables[imm.index].type);
        Value* result = Push(kWasmI32);
        CALL_INTERFACE_IF_REACHABLE(TableGrow, imm, value, delta, result);
        return opcode_length + imm.length;
      }
      case kExprTableSize: {
        TableIndexImmediate<validate> imm(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, imm)) return 0;
        Value* result = Push(kWasmI32);
        CALL_INTERFACE_IF_REACHABLE(TableSize, imm, result);
        return opcode_length + imm.length;
      }
      case kExprTableFill: {
        TableIndexImmediate<validate> imm(this, this->pc_ + opcode_length);
        if (!this->Validate(this->pc_ + opcode_length, imm)) return 0;
        Value count = Pop(2, sig->GetParam(2));
        Value value = Pop(1, this->module_->tables[imm.index].type);
        Value start = Pop(0, sig->GetParam(0));
        CALL_INTERFACE_IF_REACHABLE(TableFill, imm, start, value, count);
        return opcode_length + imm.length;
      }
      default:
        this->DecodeError("invalid numeric opcode");
        return 0;
    }
  }

  void DoReturn() {
    DCHECK_GE(stack_size(), this->sig_->return_count());
    CALL_INTERFACE_IF_REACHABLE(DoReturn);
  }

  V8_INLINE void EnsureStackSpace(int slots_needed) {
    if (V8_LIKELY(stack_capacity_end_ - stack_end_ >= slots_needed)) return;
    GrowStackSpace(slots_needed);
  }

  V8_NOINLINE void GrowStackSpace(int slots_needed) {
    size_t new_stack_capacity =
        std::max(size_t{8},
                 base::bits::RoundUpToPowerOfTwo(stack_size() + slots_needed));
    Value* new_stack =
        this->zone()->template NewArray<Value>(new_stack_capacity);
    if (stack_) {
      std::copy(stack_, stack_end_, new_stack);
      this->zone()->DeleteArray(stack_, stack_capacity_end_ - stack_);
    }
    stack_end_ = new_stack + (stack_end_ - stack_);
    stack_ = new_stack;
    stack_capacity_end_ = new_stack + new_stack_capacity;
  }

  V8_INLINE Value* Push(ValueType type) {
    DCHECK_NE(kWasmStmt, type);
    // {EnsureStackSpace} should have been called before, either in the central
    // decoding loop, or individually if more than one element is pushed.
    DCHECK_GT(stack_capacity_end_, stack_end_);
    *stack_end_ = Value{this->pc_, type};
    ++stack_end_;
    return stack_end_ - 1;
  }

  void PushMergeValues(Control* c, Merge<Value>* merge) {
    DCHECK_EQ(c, &control_.back());
    DCHECK(merge == &c->start_merge || merge == &c->end_merge);
    DCHECK_LE(stack_ + c->stack_depth, stack_end_);
    stack_end_ = stack_ + c->stack_depth;
    if (merge->arity == 1) {
      // {EnsureStackSpace} should have been called before in the central
      // decoding loop.
      DCHECK_GT(stack_capacity_end_, stack_end_);
      *stack_end_++ = merge->vals.first;
    } else {
      EnsureStackSpace(merge->arity);
      for (uint32_t i = 0; i < merge->arity; i++) {
        *stack_end_++ = merge->vals.array[i];
      }
    }
    DCHECK_EQ(c->stack_depth + merge->arity, stack_size());
  }

  Value* PushReturns(const FunctionSig* sig) {
    size_t return_count = sig->return_count();
    EnsureStackSpace(static_cast<int>(return_count));
    for (size_t i = 0; i < return_count; ++i) {
      Push(sig->GetReturn(i));
    }
    return stack_end_ - return_count;
  }

  // We do not inline these functions because doing so causes a large binary
  // size increase. Not inlining them should not create a performance
  // degradation, because their invocations are guarded by V8_LIKELY.
  V8_NOINLINE void PopTypeError(int index, Value val, const char* expected) {
    this->DecodeError(val.pc(), "%s[%d] expected %s, found %s of type %s",
                      SafeOpcodeNameAt(this->pc_), index, expected,
                      SafeOpcodeNameAt(val.pc()), val.type.name().c_str());
  }

  V8_NOINLINE void PopTypeError(int index, Value val, std::string expected) {
    PopTypeError(index, val, expected.c_str());
  }

  V8_NOINLINE void PopTypeError(int index, Value val, ValueType expected) {
    PopTypeError(index, val, ("type " + expected.name()).c_str());
  }

  V8_NOINLINE void NotEnoughArgumentsError(int index) {
    this->DecodeError(
        "not enough arguments on the stack for %s, expected %d more",
        SafeOpcodeNameAt(this->pc_), index + 1);
  }

  V8_INLINE Value Pop(int index, ValueType expected) {
    Value val = Pop(index);
    if (!VALIDATE(IsSubtypeOf(val.type, expected, this->module_) ||
                  val.type == kWasmBottom || expected == kWasmBottom)) {
      PopTypeError(index, val, expected);
    }
    return val;
  }

  V8_INLINE Value Pop(int index) {
    DCHECK(!control_.empty());
    uint32_t limit = control_.back().stack_depth;
    if (stack_size() <= limit) {
      // Popping past the current control start in reachable code.
      if (!VALIDATE(control_.back().unreachable())) {
        NotEnoughArgumentsError(index);
      }
      return UnreachableValue(this->pc_);
    }
    DCHECK_LT(stack_, stack_end_);
    stack_end_--;
    return *stack_end_;
  }

  // Pops values from the stack, as defined by {merge}. Thereby we type-check
  // unreachable merges. Afterwards the values are pushed again on the stack
  // according to the signature in {merge}. This is done so follow-up validation
  // is possible.
  bool TypeCheckUnreachableMerge(Merge<Value>& merge, bool conditional_branch) {
    int arity = merge.arity;
    // For conditional branches, stack value '0' is the condition of the branch,
    // and the result values start at index '1'.
    int index_offset = conditional_branch ? 1 : 0;
    for (int i = arity - 1; i >= 0; --i) Pop(index_offset + i, merge[i].type);
    // Push values of the correct type back on the stack.
    EnsureStackSpace(arity);
    for (int i = 0; i < arity; ++i) Push(merge[i].type);
    return this->ok();
  }

  int startrel(const byte* ptr) { return static_cast<int>(ptr - this->start_); }

  void FallThruTo(Control* c) {
    DCHECK_EQ(c, &control_.back());
    DCHECK_NE(c->kind, kControlLoop);
    if (!TypeCheckFallThru()) return;
    if (!c->reachable()) return;
    CALL_INTERFACE(FallThruTo, c);
    c->end_merge.reached = true;
  }

  bool TypeCheckMergeValues(Control* c, Merge<Value>* merge) {
    static_assert(validate, "Call this function only within VALIDATE");
    DCHECK(merge == &c->start_merge || merge == &c->end_merge);
    DCHECK_GE(stack_size(), c->stack_depth + merge->arity);
    // The computation of {stack_values} is only valid if {merge->arity} is >0.
    DCHECK_LT(0, merge->arity);
    Value* stack_values = stack_end_ - merge->arity;
    // Typecheck the topmost {merge->arity} values on the stack.
    for (uint32_t i = 0; i < merge->arity; ++i) {
      Value& val = stack_values[i];
      Value& old = (*merge)[i];
      if (!VALIDATE(IsSubtypeOf(val.type, old.type, this->module_))) {
        this->DecodeError("type error in merge[%u] (expected %s, got %s)", i,
                          old.type.name().c_str(), val.type.name().c_str());
        return false;
      }
    }

    return true;
  }

  bool TypeCheckOneArmedIf(Control* c) {
    static_assert(validate, "Call this function only within VALIDATE");
    DCHECK(c->is_onearmed_if());
    DCHECK_EQ(c->start_merge.arity, c->end_merge.arity);
    for (uint32_t i = 0; i < c->start_merge.arity; ++i) {
      Value& start = c->start_merge[i];
      Value& end = c->end_merge[i];
      if (!VALIDATE(IsSubtypeOf(start.type, end.type, this->module_))) {
        this->DecodeError("type error in merge[%u] (expected %s, got %s)", i,
                          end.type.name().c_str(), start.type.name().c_str());
        return false;
      }
    }

    return true;
  }

  bool TypeCheckFallThru() {
    static_assert(validate, "Call this function only within VALIDATE");
    Control& c = control_.back();
    if (V8_LIKELY(c.reachable())) {
      uint32_t expected = c.end_merge.arity;
      DCHECK_GE(stack_size(), c.stack_depth);
      uint32_t actual = stack_size() - c.stack_depth;
      // Fallthrus must match the arity of the control exactly.
      if (!VALIDATE(actual == expected)) {
        this->DecodeError(
            "expected %u elements on the stack for fallthru to @%d, found %u",
            expected, startrel(c.pc()), actual);
        return false;
      }
      if (expected == 0) return true;  // Fast path.

      return TypeCheckMergeValues(&c, &c.end_merge);
    }

    // Type-check an unreachable fallthru. First we do an arity check, then a
    // type check. Note that type-checking may require an adjustment of the
    // stack, if some stack values are missing to match the block signature.
    Merge<Value>& merge = c.end_merge;
    int arity = static_cast<int>(merge.arity);
    int available = static_cast<int>(stack_size()) - c.stack_depth;
    // For fallthrus, not more than the needed values should be available.
    if (!VALIDATE(available <= arity)) {
      this->DecodeError(
          "expected %u elements on the stack for fallthru to @%d, found %u",
          arity, startrel(c.pc()), available);
      return false;
    }
    // Pop all values from the stack for type checking of existing stack
    // values.
    return TypeCheckUnreachableMerge(merge, false);
  }

  enum TypeCheckBranchResult {
    kReachableBranch,
    kUnreachableBranch,
    kInvalidStack,
  };

  // If the type code is reachable, check if the current stack values are
  // compatible with a jump to {c}, based on their number and types.
  // Otherwise, we have a polymorphic stack: check if any values that may exist
  // on top of the stack are compatible with {c}, and push back to the stack
  // values based on the type of {c}.
  TypeCheckBranchResult TypeCheckBranch(Control* c, bool conditional_branch) {
    if (V8_LIKELY(control_.back().reachable())) {
      // We only do type-checking here. This is only needed during validation.
      if (!validate) return kReachableBranch;

      // Branches must have at least the number of values expected; can have
      // more.
      uint32_t expected = c->br_merge()->arity;
      if (expected == 0) return kReachableBranch;  // Fast path.
      DCHECK_GE(stack_size(), control_.back().stack_depth);
      uint32_t actual =
          static_cast<uint32_t>(stack_size()) - control_.back().stack_depth;
      if (!VALIDATE(actual >= expected)) {
        this->DecodeError(
            "expected %u elements on the stack for br to @%d, found %u",
            expected, startrel(c->pc()), actual);
        return kInvalidStack;
      }
      return TypeCheckMergeValues(c, c->br_merge()) ? kReachableBranch
                                                    : kInvalidStack;
    }

    return TypeCheckUnreachableMerge(*c->br_merge(), conditional_branch)
               ? kUnreachableBranch
               : kInvalidStack;
  }

  bool TypeCheckReturn() {
    int num_returns = static_cast<int>(this->sig_->return_count());
    // No type checking is needed if there are no returns.
    if (num_returns == 0) return true;

    // Returns must have at least the number of values expected; can have more.
    int num_available =
        static_cast<int>(stack_size()) - control_.back().stack_depth;
    if (!VALIDATE(num_available >= num_returns)) {
      this->DecodeError(
          "expected %u elements on the stack for return, found %u", num_returns,
          num_available);
      return false;
    }

    // Typecheck the topmost {num_returns} values on the stack.
    // This line requires num_returns > 0.
    Value* stack_values = stack_end_ - num_returns;
    for (int i = 0; i < num_returns; ++i) {
      Value& val = stack_values[i];
      ValueType expected_type = this->sig_->GetReturn(i);
      if (!VALIDATE(IsSubtypeOf(val.type, expected_type, this->module_))) {
        this->DecodeError("type error in return[%u] (expected %s, got %s)", i,
                          expected_type.name().c_str(),
                          val.type.name().c_str());
        return false;
      }
    }
    return true;
  }

  void onFirstError() override {
    this->end_ = this->pc_;  // Terminate decoding loop.
    this->current_code_reachable_ = false;
    TRACE(" !%s\n", this->error_.message().c_str());
    CALL_INTERFACE(OnFirstError);
  }

  int BuildSimplePrototypeOperator(WasmOpcode opcode) {
    if (opcode == kExprRefEq) {
      CHECK_PROTOTYPE_OPCODE(gc);
    }
    const FunctionSig* sig = WasmOpcodes::Signature(opcode);
    return BuildSimpleOperator(opcode, sig);
  }

  int BuildSimpleOperator(WasmOpcode opcode, const FunctionSig* sig) {
    DCHECK_GE(1, sig->return_count());
    ValueType ret = sig->return_count() == 0 ? kWasmStmt : sig->GetReturn(0);
    if (sig->parameter_count() == 1) {
      return BuildSimpleOperator(opcode, ret, sig->GetParam(0));
    } else {
      DCHECK_EQ(2, sig->parameter_count());
      return BuildSimpleOperator(opcode, ret, sig->GetParam(0),
                                 sig->GetParam(1));
    }
  }

  int BuildSimpleOperator(WasmOpcode opcode, ValueType return_type,
                          ValueType arg_type) {
    Value val = Pop(0, arg_type);
    Value* ret = return_type == kWasmStmt ? nullptr : Push(return_type);
    CALL_INTERFACE_IF_REACHABLE(UnOp, opcode, val, ret);
    return 1;
  }

  int BuildSimpleOperator(WasmOpcode opcode, ValueType return_type,
                          ValueType lhs_type, ValueType rhs_type) {
    Value rval = Pop(1, rhs_type);
    Value lval = Pop(0, lhs_type);
    Value* ret = return_type == kWasmStmt ? nullptr : Push(return_type);
    CALL_INTERFACE_IF_REACHABLE(BinOp, opcode, lval, rval, ret);
    return 1;
  }

#define DEFINE_SIMPLE_SIG_OPERATOR(sig, ...)         \
  int BuildSimpleOperator_##sig(WasmOpcode opcode) { \
    return BuildSimpleOperator(opcode, __VA_ARGS__); \
  }
  FOREACH_SIGNATURE(DEFINE_SIMPLE_SIG_OPERATOR)
#undef DEFINE_SIMPLE_SIG_OPERATOR
};

#undef CALL_INTERFACE
#undef CALL_INTERFACE_IF_REACHABLE
#undef CALL_INTERFACE_IF_PARENT_REACHABLE

class EmptyInterface {
 public:
  static constexpr Decoder::ValidateFlag validate = Decoder::kFullValidation;
  using Value = ValueBase<validate>;
  using Control = ControlBase<Value, validate>;
  using FullDecoder = WasmFullDecoder<validate, EmptyInterface>;

#define DEFINE_EMPTY_CALLBACK(name, ...) \
  void name(FullDecoder* decoder, ##__VA_ARGS__) {}
  INTERFACE_FUNCTIONS(DEFINE_EMPTY_CALLBACK)
#undef DEFINE_EMPTY_CALLBACK
};

#undef TRACE
#undef TRACE_INST_FORMAT
#undef VALIDATE
#undef CHECK_PROTOTYPE_OPCODE

}  // namespace wasm
}  // namespace internal
}  // namespace v8

#endif  // V8_WASM_FUNCTION_BODY_DECODER_IMPL_H_

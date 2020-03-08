// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_WASM_BASELINE_ARM64_LIFTOFF_ASSEMBLER_ARM64_H_
#define V8_WASM_BASELINE_ARM64_LIFTOFF_ASSEMBLER_ARM64_H_

#include "src/wasm/baseline/liftoff-assembler.h"

namespace v8 {
namespace internal {
namespace wasm {

namespace liftoff {

// Liftoff Frames.
//
//  slot      Frame
//       +--------------------+---------------------------
//  n+4  | optional padding slot to keep the stack 16 byte aligned.
//  n+3  |   parameter n      |
//  ...  |       ...          |
//   4   |   parameter 1      | or parameter 2
//   3   |   parameter 0      | or parameter 1
//   2   |  (result address)  | or parameter 0
//  -----+--------------------+---------------------------
//   1   | return addr (lr)   |
//   0   | previous frame (fp)|
//  -----+--------------------+  <-- frame ptr (fp)
//  -1   | 0xa: WASM_COMPILED |
//  -2   |     instance       |
//  -----+--------------------+---------------------------
//  -3   |     slot 0         |   ^
//  -4   |     slot 1         |   |
//       |                    | Frame slots
//       |                    |   |
//       |                    |   v
//       | optional padding slot to keep the stack 16 byte aligned.
//  -----+--------------------+  <-- stack ptr (sp)
//

constexpr int kInstanceOffset = 2 * kSystemPointerSize;

inline MemOperand GetStackSlot(int offset) { return MemOperand(fp, -offset); }

inline MemOperand GetInstanceOperand() { return GetStackSlot(kInstanceOffset); }

inline CPURegister GetRegFromType(const LiftoffRegister& reg, ValueType type) {
  switch (type) {
    case kWasmI32:
      return reg.gp().W();
    case kWasmI64:
      return reg.gp().X();
    case kWasmF32:
      return reg.fp().S();
    case kWasmF64:
      return reg.fp().D();
    case kWasmS128:
      return reg.fp().Q();
    default:
      UNREACHABLE();
  }
}

inline CPURegList PadRegList(RegList list) {
  if ((base::bits::CountPopulation(list) & 1) != 0) list |= padreg.bit();
  return CPURegList(CPURegister::kRegister, kXRegSizeInBits, list);
}

inline CPURegList PadVRegList(RegList list) {
  if ((base::bits::CountPopulation(list) & 1) != 0) list |= fp_scratch.bit();
  return CPURegList(CPURegister::kVRegister, kQRegSizeInBits, list);
}

inline CPURegister AcquireByType(UseScratchRegisterScope* temps,
                                 ValueType type) {
  switch (type) {
    case kWasmI32:
      return temps->AcquireW();
    case kWasmI64:
      return temps->AcquireX();
    case kWasmF32:
      return temps->AcquireS();
    case kWasmF64:
      return temps->AcquireD();
    default:
      UNREACHABLE();
  }
}

inline MemOperand GetMemOp(LiftoffAssembler* assm,
                           UseScratchRegisterScope* temps, Register addr,
                           Register offset, uint32_t offset_imm) {
  // Wasm memory is limited to a size <4GB.
  DCHECK(is_uint32(offset_imm));
  if (offset.is_valid()) {
    if (offset_imm == 0) return MemOperand(addr.X(), offset.W(), UXTW);
    Register tmp = temps->AcquireW();
    assm->Add(tmp, offset.W(), offset_imm);
    return MemOperand(addr.X(), tmp, UXTW);
  }
  return MemOperand(addr.X(), offset_imm);
}

}  // namespace liftoff

int LiftoffAssembler::PrepareStackFrame() {
  int offset = pc_offset();
  InstructionAccurateScope scope(this, 1);
  sub(sp, sp, 0);
  return offset;
}

void LiftoffAssembler::PatchPrepareStackFrame(int offset, int frame_size) {
  static_assert(kStackSlotSize == kXRegSize,
                "kStackSlotSize must equal kXRegSize");
  // The stack pointer is required to be quadword aligned.
  // Misalignment will cause a stack alignment fault.
  frame_size = RoundUp(frame_size, kQuadWordSizeInBytes);
  if (!IsImmAddSub(frame_size)) {
    // Round the stack to a page to try to fit a add/sub immediate.
    frame_size = RoundUp(frame_size, 0x1000);
    if (!IsImmAddSub(frame_size)) {
      // Stack greater than 4M! Because this is a quite improbable case, we
      // just fallback to TurboFan.
      bailout(kOtherReason, "Stack too big");
      return;
    }
  }
#ifdef USE_SIMULATOR
  // When using the simulator, deal with Liftoff which allocates the stack
  // before checking it.
  // TODO(arm): Remove this when the stack check mechanism will be updated.
  if (frame_size > KB / 2) {
    bailout(kOtherReason,
            "Stack limited to 512 bytes to avoid a bug in StackCheck");
    return;
  }
#endif
  PatchingAssembler patching_assembler(AssemblerOptions{},
                                       buffer_start_ + offset, 1);
#if V8_OS_WIN
  if (frame_size > kStackPageSize) {
    // Generate OOL code (at the end of the function, where the current
    // assembler is pointing) to do the explicit stack limit check (see
    // https://docs.microsoft.com/en-us/previous-versions/visualstudio/
    // visual-studio-6.0/aa227153(v=vs.60)).
    // At the function start, emit a jump to that OOL code (from {offset} to
    // {pc_offset()}).
    int ool_offset = pc_offset() - offset;
    patching_assembler.b(ool_offset >> kInstrSizeLog2);

    // Now generate the OOL code.
    Claim(frame_size, 1);
    // Jump back to the start of the function (from {pc_offset()} to {offset +
    // kInstrSize}).
    int func_start_offset = offset + kInstrSize - pc_offset();
    b(func_start_offset >> kInstrSizeLog2);
    return;
  }
#endif
  patching_assembler.PatchSubSp(frame_size);
}

void LiftoffAssembler::FinishCode() { ForceConstantPoolEmissionWithoutJump(); }

void LiftoffAssembler::AbortCompilation() { AbortedCodeGeneration(); }

// static
constexpr int LiftoffAssembler::StaticStackFrameSize() {
  return liftoff::kInstanceOffset;
}

int LiftoffAssembler::SlotSizeForType(ValueType type) {
  // TODO(zhin): Unaligned access typically take additional cycles, we should do
  // some performance testing to see how big an effect it will take.
  switch (type) {
    case kWasmS128:
      return ValueTypes::ElementSizeInBytes(type);
    default:
      return kStackSlotSize;
  }
}

bool LiftoffAssembler::NeedsAlignment(ValueType type) {
  switch (type) {
    case kWasmS128:
      return true;
    default:
      // No alignment because all other types are kStackSlotSize.
      return false;
  }
}

void LiftoffAssembler::LoadConstant(LiftoffRegister reg, WasmValue value,
                                    RelocInfo::Mode rmode) {
  switch (value.type()) {
    case kWasmI32:
      Mov(reg.gp().W(), Immediate(value.to_i32(), rmode));
      break;
    case kWasmI64:
      Mov(reg.gp().X(), Immediate(value.to_i64(), rmode));
      break;
    case kWasmF32:
      Fmov(reg.fp().S(), value.to_f32_boxed().get_scalar());
      break;
    case kWasmF64:
      Fmov(reg.fp().D(), value.to_f64_boxed().get_scalar());
      break;
    default:
      UNREACHABLE();
  }
}

void LiftoffAssembler::LoadFromInstance(Register dst, uint32_t offset,
                                        int size) {
  DCHECK_LE(offset, kMaxInt);
  Ldr(dst, liftoff::GetInstanceOperand());
  DCHECK(size == 4 || size == 8);
  if (size == 4) {
    Ldr(dst.W(), MemOperand(dst, offset));
  } else {
    Ldr(dst, MemOperand(dst, offset));
  }
}

void LiftoffAssembler::LoadTaggedPointerFromInstance(Register dst,
                                                     uint32_t offset) {
  DCHECK_LE(offset, kMaxInt);
  Ldr(dst, liftoff::GetInstanceOperand());
  LoadTaggedPointerField(dst, MemOperand(dst, offset));
}

void LiftoffAssembler::SpillInstance(Register instance) {
  Str(instance, liftoff::GetInstanceOperand());
}

void LiftoffAssembler::FillInstanceInto(Register dst) {
  Ldr(dst, liftoff::GetInstanceOperand());
}

void LiftoffAssembler::LoadTaggedPointer(Register dst, Register src_addr,
                                         Register offset_reg,
                                         uint32_t offset_imm,
                                         LiftoffRegList pinned) {
  UseScratchRegisterScope temps(this);
  MemOperand src_op =
      liftoff::GetMemOp(this, &temps, src_addr, offset_reg, offset_imm);
  LoadTaggedPointerField(dst, src_op);
}

void LiftoffAssembler::Load(LiftoffRegister dst, Register src_addr,
                            Register offset_reg, uint32_t offset_imm,
                            LoadType type, LiftoffRegList pinned,
                            uint32_t* protected_load_pc, bool is_load_mem) {
  UseScratchRegisterScope temps(this);
  MemOperand src_op =
      liftoff::GetMemOp(this, &temps, src_addr, offset_reg, offset_imm);
  if (protected_load_pc) *protected_load_pc = pc_offset();
  switch (type.value()) {
    case LoadType::kI32Load8U:
    case LoadType::kI64Load8U:
      Ldrb(dst.gp().W(), src_op);
      break;
    case LoadType::kI32Load8S:
      Ldrsb(dst.gp().W(), src_op);
      break;
    case LoadType::kI64Load8S:
      Ldrsb(dst.gp().X(), src_op);
      break;
    case LoadType::kI32Load16U:
    case LoadType::kI64Load16U:
      Ldrh(dst.gp().W(), src_op);
      break;
    case LoadType::kI32Load16S:
      Ldrsh(dst.gp().W(), src_op);
      break;
    case LoadType::kI64Load16S:
      Ldrsh(dst.gp().X(), src_op);
      break;
    case LoadType::kI32Load:
    case LoadType::kI64Load32U:
      Ldr(dst.gp().W(), src_op);
      break;
    case LoadType::kI64Load32S:
      Ldrsw(dst.gp().X(), src_op);
      break;
    case LoadType::kI64Load:
      Ldr(dst.gp().X(), src_op);
      break;
    case LoadType::kF32Load:
      Ldr(dst.fp().S(), src_op);
      break;
    case LoadType::kF64Load:
      Ldr(dst.fp().D(), src_op);
      break;
    case LoadType::kS128Load:
      Ldr(dst.fp().Q(), src_op);
      break;
    default:
      UNREACHABLE();
  }
}

void LiftoffAssembler::Store(Register dst_addr, Register offset_reg,
                             uint32_t offset_imm, LiftoffRegister src,
                             StoreType type, LiftoffRegList pinned,
                             uint32_t* protected_store_pc, bool is_store_mem) {
  UseScratchRegisterScope temps(this);
  MemOperand dst_op =
      liftoff::GetMemOp(this, &temps, dst_addr, offset_reg, offset_imm);
  if (protected_store_pc) *protected_store_pc = pc_offset();
  switch (type.value()) {
    case StoreType::kI32Store8:
    case StoreType::kI64Store8:
      Strb(src.gp().W(), dst_op);
      break;
    case StoreType::kI32Store16:
    case StoreType::kI64Store16:
      Strh(src.gp().W(), dst_op);
      break;
    case StoreType::kI32Store:
    case StoreType::kI64Store32:
      Str(src.gp().W(), dst_op);
      break;
    case StoreType::kI64Store:
      Str(src.gp().X(), dst_op);
      break;
    case StoreType::kF32Store:
      Str(src.fp().S(), dst_op);
      break;
    case StoreType::kF64Store:
      Str(src.fp().D(), dst_op);
      break;
    case StoreType::kS128Store:
      Str(src.fp().Q(), dst_op);
      break;
    default:
      UNREACHABLE();
  }
}

void LiftoffAssembler::AtomicLoad(LiftoffRegister dst, Register src_addr,
                                  Register offset_reg, uint32_t offset_imm,
                                  LoadType type, LiftoffRegList pinned) {
  bailout(kAtomics, "AtomicLoad");
}

void LiftoffAssembler::AtomicStore(Register dst_addr, Register offset_reg,
                                   uint32_t offset_imm, LiftoffRegister src,
                                   StoreType type, LiftoffRegList pinned) {
  bailout(kAtomics, "AtomicStore");
}

void LiftoffAssembler::AtomicAdd(Register dst_addr, Register offset_reg,
                                 uint32_t offset_imm, LiftoffRegister value,
                                 StoreType type) {
  bailout(kAtomics, "AtomicAdd");
}

void LiftoffAssembler::AtomicSub(Register dst_addr, Register offset_reg,
                                 uint32_t offset_imm, LiftoffRegister value,
                                 StoreType type) {
  bailout(kAtomics, "AtomicSub");
}

void LiftoffAssembler::AtomicAnd(Register dst_addr, Register offset_reg,
                                 uint32_t offset_imm, LiftoffRegister value,
                                 StoreType type) {
  bailout(kAtomics, "AtomicAnd");
}

void LiftoffAssembler::AtomicOr(Register dst_addr, Register offset_reg,
                                uint32_t offset_imm, LiftoffRegister value,
                                StoreType type) {
  bailout(kAtomics, "AtomicOr");
}

void LiftoffAssembler::AtomicXor(Register dst_addr, Register offset_reg,
                                 uint32_t offset_imm, LiftoffRegister value,
                                 StoreType type) {
  bailout(kAtomics, "AtomicXor");
}

void LiftoffAssembler::AtomicExchange(Register dst_addr, Register offset_reg,
                                      uint32_t offset_imm,
                                      LiftoffRegister value, StoreType type) {
  bailout(kAtomics, "AtomicExchange");
}

void LiftoffAssembler::AtomicCompareExchange(
    Register dst_addr, Register offset_reg, uint32_t offset_imm,
    LiftoffRegister expected, LiftoffRegister new_value, LiftoffRegister result,
    StoreType type) {
  bailout(kAtomics, "AtomicCompareExchange");
}

void LiftoffAssembler::AtomicFence() { Dmb(InnerShareable, BarrierAll); }

void LiftoffAssembler::LoadCallerFrameSlot(LiftoffRegister dst,
                                           uint32_t caller_slot_idx,
                                           ValueType type) {
  int32_t offset = (caller_slot_idx + 1) * LiftoffAssembler::kStackSlotSize;
  Ldr(liftoff::GetRegFromType(dst, type), MemOperand(fp, offset));
}

void LiftoffAssembler::MoveStackValue(uint32_t dst_offset, uint32_t src_offset,
                                      ValueType type) {
  UseScratchRegisterScope temps(this);
  CPURegister scratch = liftoff::AcquireByType(&temps, type);
  Ldr(scratch, liftoff::GetStackSlot(src_offset));
  Str(scratch, liftoff::GetStackSlot(dst_offset));
}

void LiftoffAssembler::Move(Register dst, Register src, ValueType type) {
  if (type == kWasmI32) {
    Mov(dst.W(), src.W());
  } else {
    DCHECK_EQ(kWasmI64, type);
    Mov(dst.X(), src.X());
  }
}

void LiftoffAssembler::Move(DoubleRegister dst, DoubleRegister src,
                            ValueType type) {
  if (type == kWasmF32) {
    Fmov(dst.S(), src.S());
  } else if (type == kWasmF64) {
    Fmov(dst.D(), src.D());
  } else {
    DCHECK_EQ(kWasmS128, type);
    Fmov(dst.Q(), src.Q());
  }
}

void LiftoffAssembler::Spill(int offset, LiftoffRegister reg, ValueType type) {
  RecordUsedSpillOffset(offset);
  MemOperand dst = liftoff::GetStackSlot(offset);
  Str(liftoff::GetRegFromType(reg, type), dst);
}

void LiftoffAssembler::Spill(int offset, WasmValue value) {
  RecordUsedSpillOffset(offset);
  MemOperand dst = liftoff::GetStackSlot(offset);
  UseScratchRegisterScope temps(this);
  CPURegister src = CPURegister::no_reg();
  switch (value.type()) {
    case kWasmI32:
      if (value.to_i32() == 0) {
        src = wzr;
      } else {
        src = temps.AcquireW();
        Mov(src.W(), value.to_i32());
      }
      break;
    case kWasmI64:
      if (value.to_i64() == 0) {
        src = xzr;
      } else {
        src = temps.AcquireX();
        Mov(src.X(), value.to_i64());
      }
      break;
    default:
      // We do not track f32 and f64 constants, hence they are unreachable.
      UNREACHABLE();
  }
  Str(src, dst);
}

void LiftoffAssembler::Fill(LiftoffRegister reg, int offset, ValueType type) {
  MemOperand src = liftoff::GetStackSlot(offset);
  Ldr(liftoff::GetRegFromType(reg, type), src);
}

void LiftoffAssembler::FillI64Half(Register, int offset, RegPairHalf) {
  UNREACHABLE();
}

void LiftoffAssembler::FillStackSlotsWithZero(int start, int size) {
  DCHECK_LT(0, size);
  DCHECK_EQ(0, size % 4);
  RecordUsedSpillOffset(start + size);

  int max_stp_offset = -start - size;
  if (size <= 12 * kStackSlotSize &&
      IsImmLSPair(max_stp_offset, kXRegSizeLog2)) {
    // Special straight-line code for up to 12 slots. Generates one
    // instruction per two slots (<= 7 instructions total).
    STATIC_ASSERT(kStackSlotSize == kSystemPointerSize);
    uint32_t remainder = size;
    for (; remainder >= 2 * kStackSlotSize; remainder -= 2 * kStackSlotSize) {
      stp(xzr, xzr, liftoff::GetStackSlot(start + remainder));
    }

    DCHECK_GE(12, remainder);
    switch (remainder) {
      case 12:
        str(xzr, liftoff::GetStackSlot(start + remainder));
        str(wzr, liftoff::GetStackSlot(start + remainder - 8));
        break;
      case 8:
        str(xzr, liftoff::GetStackSlot(start + remainder));
        break;
      case 4:
        str(wzr, liftoff::GetStackSlot(start + remainder));
        break;
      case 0:
        break;
      default:
        UNREACHABLE();
    }
  } else {
    // General case for bigger counts (5-8 instructions).
    UseScratchRegisterScope temps(this);
    Register address_reg = temps.AcquireX();
    // This {Sub} might use another temp register if the offset is too large.
    Sub(address_reg, fp, start + size);
    Register count_reg = temps.AcquireX();
    Mov(count_reg, size / 4);

    Label loop;
    bind(&loop);
    sub(count_reg, count_reg, 1);
    str(wzr, MemOperand(address_reg, kSystemPointerSize / 2, PostIndex));
    cbnz(count_reg, &loop);
  }
}

#define I32_BINOP(name, instruction)                             \
  void LiftoffAssembler::emit_##name(Register dst, Register lhs, \
                                     Register rhs) {             \
    instruction(dst.W(), lhs.W(), rhs.W());                      \
  }
#define I32_BINOP_I(name, instruction)                           \
  I32_BINOP(name, instruction)                                   \
  void LiftoffAssembler::emit_##name(Register dst, Register lhs, \
                                     int32_t imm) {              \
    instruction(dst.W(), lhs.W(), Immediate(imm));               \
  }
#define I64_BINOP(name, instruction)                                           \
  void LiftoffAssembler::emit_##name(LiftoffRegister dst, LiftoffRegister lhs, \
                                     LiftoffRegister rhs) {                    \
    instruction(dst.gp().X(), lhs.gp().X(), rhs.gp().X());                     \
  }
#define I64_BINOP_I(name, instruction)                                         \
  I64_BINOP(name, instruction)                                                 \
  void LiftoffAssembler::emit_##name(LiftoffRegister dst, LiftoffRegister lhs, \
                                     int32_t imm) {                            \
    instruction(dst.gp().X(), lhs.gp().X(), imm);                              \
  }
#define FP32_BINOP(name, instruction)                                        \
  void LiftoffAssembler::emit_##name(DoubleRegister dst, DoubleRegister lhs, \
                                     DoubleRegister rhs) {                   \
    instruction(dst.S(), lhs.S(), rhs.S());                                  \
  }
#define FP32_UNOP(name, instruction)                                           \
  void LiftoffAssembler::emit_##name(DoubleRegister dst, DoubleRegister src) { \
    instruction(dst.S(), src.S());                                             \
  }
#define FP32_UNOP_RETURN_TRUE(name, instruction)                               \
  bool LiftoffAssembler::emit_##name(DoubleRegister dst, DoubleRegister src) { \
    instruction(dst.S(), src.S());                                             \
    return true;                                                               \
  }
#define FP64_BINOP(name, instruction)                                        \
  void LiftoffAssembler::emit_##name(DoubleRegister dst, DoubleRegister lhs, \
                                     DoubleRegister rhs) {                   \
    instruction(dst.D(), lhs.D(), rhs.D());                                  \
  }
#define FP64_UNOP(name, instruction)                                           \
  void LiftoffAssembler::emit_##name(DoubleRegister dst, DoubleRegister src) { \
    instruction(dst.D(), src.D());                                             \
  }
#define FP64_UNOP_RETURN_TRUE(name, instruction)                               \
  bool LiftoffAssembler::emit_##name(DoubleRegister dst, DoubleRegister src) { \
    instruction(dst.D(), src.D());                                             \
    return true;                                                               \
  }
#define I32_SHIFTOP(name, instruction)                           \
  void LiftoffAssembler::emit_##name(Register dst, Register src, \
                                     Register amount) {          \
    instruction(dst.W(), src.W(), amount.W());                   \
  }                                                              \
  void LiftoffAssembler::emit_##name(Register dst, Register src, \
                                     int32_t amount) {           \
    instruction(dst.W(), src.W(), amount & 31);                  \
  }
#define I64_SHIFTOP(name, instruction)                                         \
  void LiftoffAssembler::emit_##name(LiftoffRegister dst, LiftoffRegister src, \
                                     Register amount) {                        \
    instruction(dst.gp().X(), src.gp().X(), amount.X());                       \
  }                                                                            \
  void LiftoffAssembler::emit_##name(LiftoffRegister dst, LiftoffRegister src, \
                                     int32_t amount) {                         \
    instruction(dst.gp().X(), src.gp().X(), amount & 63);                      \
  }

I32_BINOP_I(i32_add, Add)
I32_BINOP(i32_sub, Sub)
I32_BINOP(i32_mul, Mul)
I32_BINOP_I(i32_and, And)
I32_BINOP_I(i32_or, Orr)
I32_BINOP_I(i32_xor, Eor)
I32_SHIFTOP(i32_shl, Lsl)
I32_SHIFTOP(i32_sar, Asr)
I32_SHIFTOP(i32_shr, Lsr)
I64_BINOP_I(i64_add, Add)
I64_BINOP(i64_sub, Sub)
I64_BINOP(i64_mul, Mul)
I64_BINOP_I(i64_and, And)
I64_BINOP_I(i64_or, Orr)
I64_BINOP_I(i64_xor, Eor)
I64_SHIFTOP(i64_shl, Lsl)
I64_SHIFTOP(i64_sar, Asr)
I64_SHIFTOP(i64_shr, Lsr)
FP32_BINOP(f32_add, Fadd)
FP32_BINOP(f32_sub, Fsub)
FP32_BINOP(f32_mul, Fmul)
FP32_BINOP(f32_div, Fdiv)
FP32_BINOP(f32_min, Fmin)
FP32_BINOP(f32_max, Fmax)
FP32_UNOP(f32_abs, Fabs)
FP32_UNOP(f32_neg, Fneg)
FP32_UNOP_RETURN_TRUE(f32_ceil, Frintp)
FP32_UNOP_RETURN_TRUE(f32_floor, Frintm)
FP32_UNOP_RETURN_TRUE(f32_trunc, Frintz)
FP32_UNOP_RETURN_TRUE(f32_nearest_int, Frintn)
FP32_UNOP(f32_sqrt, Fsqrt)
FP64_BINOP(f64_add, Fadd)
FP64_BINOP(f64_sub, Fsub)
FP64_BINOP(f64_mul, Fmul)
FP64_BINOP(f64_div, Fdiv)
FP64_BINOP(f64_min, Fmin)
FP64_BINOP(f64_max, Fmax)
FP64_UNOP(f64_abs, Fabs)
FP64_UNOP(f64_neg, Fneg)
FP64_UNOP_RETURN_TRUE(f64_ceil, Frintp)
FP64_UNOP_RETURN_TRUE(f64_floor, Frintm)
FP64_UNOP_RETURN_TRUE(f64_trunc, Frintz)
FP64_UNOP_RETURN_TRUE(f64_nearest_int, Frintn)
FP64_UNOP(f64_sqrt, Fsqrt)

#undef I32_BINOP
#undef I64_BINOP
#undef FP32_BINOP
#undef FP32_UNOP
#undef FP64_BINOP
#undef FP64_UNOP
#undef FP64_UNOP_RETURN_TRUE
#undef I32_SHIFTOP
#undef I64_SHIFTOP

void LiftoffAssembler::emit_i32_clz(Register dst, Register src) {
  Clz(dst.W(), src.W());
}

void LiftoffAssembler::emit_i32_ctz(Register dst, Register src) {
  Rbit(dst.W(), src.W());
  Clz(dst.W(), dst.W());
}

bool LiftoffAssembler::emit_i32_popcnt(Register dst, Register src) {
  UseScratchRegisterScope temps(this);
  VRegister scratch = temps.AcquireV(kFormat8B);
  Fmov(scratch.S(), src.W());
  Cnt(scratch, scratch);
  Addv(scratch.B(), scratch);
  Fmov(dst.W(), scratch.S());
  return true;
}

void LiftoffAssembler::emit_i64_clz(LiftoffRegister dst, LiftoffRegister src) {
  Clz(dst.gp().X(), src.gp().X());
}

void LiftoffAssembler::emit_i64_ctz(LiftoffRegister dst, LiftoffRegister src) {
  Rbit(dst.gp().X(), src.gp().X());
  Clz(dst.gp().X(), dst.gp().X());
}

bool LiftoffAssembler::emit_i64_popcnt(LiftoffRegister dst,
                                       LiftoffRegister src) {
  UseScratchRegisterScope temps(this);
  VRegister scratch = temps.AcquireV(kFormat8B);
  Fmov(scratch.D(), src.gp().X());
  Cnt(scratch, scratch);
  Addv(scratch.B(), scratch);
  Fmov(dst.gp().X(), scratch.D());
  return true;
}

void LiftoffAssembler::emit_i32_divs(Register dst, Register lhs, Register rhs,
                                     Label* trap_div_by_zero,
                                     Label* trap_div_unrepresentable) {
  Register dst_w = dst.W();
  Register lhs_w = lhs.W();
  Register rhs_w = rhs.W();
  bool can_use_dst = !dst_w.Aliases(lhs_w) && !dst_w.Aliases(rhs_w);
  if (can_use_dst) {
    // Do div early.
    Sdiv(dst_w, lhs_w, rhs_w);
  }
  // Check for division by zero.
  Cbz(rhs_w, trap_div_by_zero);
  // Check for kMinInt / -1. This is unrepresentable.
  Cmp(rhs_w, -1);
  Ccmp(lhs_w, 1, NoFlag, eq);
  B(trap_div_unrepresentable, vs);
  if (!can_use_dst) {
    // Do div.
    Sdiv(dst_w, lhs_w, rhs_w);
  }
}

void LiftoffAssembler::emit_i32_divu(Register dst, Register lhs, Register rhs,
                                     Label* trap_div_by_zero) {
  // Check for division by zero.
  Cbz(rhs.W(), trap_div_by_zero);
  // Do div.
  Udiv(dst.W(), lhs.W(), rhs.W());
}

void LiftoffAssembler::emit_i32_rems(Register dst, Register lhs, Register rhs,
                                     Label* trap_div_by_zero) {
  Register dst_w = dst.W();
  Register lhs_w = lhs.W();
  Register rhs_w = rhs.W();
  // Do early div.
  // No need to check kMinInt / -1 because the result is kMinInt and then
  // kMinInt * -1 -> kMinInt. In this case, the Msub result is therefore 0.
  UseScratchRegisterScope temps(this);
  Register scratch = temps.AcquireW();
  Sdiv(scratch, lhs_w, rhs_w);
  // Check for division by zero.
  Cbz(rhs_w, trap_div_by_zero);
  // Compute remainder.
  Msub(dst_w, scratch, rhs_w, lhs_w);
}

void LiftoffAssembler::emit_i32_remu(Register dst, Register lhs, Register rhs,
                                     Label* trap_div_by_zero) {
  Register dst_w = dst.W();
  Register lhs_w = lhs.W();
  Register rhs_w = rhs.W();
  // Do early div.
  UseScratchRegisterScope temps(this);
  Register scratch = temps.AcquireW();
  Udiv(scratch, lhs_w, rhs_w);
  // Check for division by zero.
  Cbz(rhs_w, trap_div_by_zero);
  // Compute remainder.
  Msub(dst_w, scratch, rhs_w, lhs_w);
}

bool LiftoffAssembler::emit_i64_divs(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs,
                                     Label* trap_div_by_zero,
                                     Label* trap_div_unrepresentable) {
  Register dst_x = dst.gp().X();
  Register lhs_x = lhs.gp().X();
  Register rhs_x = rhs.gp().X();
  bool can_use_dst = !dst_x.Aliases(lhs_x) && !dst_x.Aliases(rhs_x);
  if (can_use_dst) {
    // Do div early.
    Sdiv(dst_x, lhs_x, rhs_x);
  }
  // Check for division by zero.
  Cbz(rhs_x, trap_div_by_zero);
  // Check for kMinInt / -1. This is unrepresentable.
  Cmp(rhs_x, -1);
  Ccmp(lhs_x, 1, NoFlag, eq);
  B(trap_div_unrepresentable, vs);
  if (!can_use_dst) {
    // Do div.
    Sdiv(dst_x, lhs_x, rhs_x);
  }
  return true;
}

bool LiftoffAssembler::emit_i64_divu(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs,
                                     Label* trap_div_by_zero) {
  // Check for division by zero.
  Cbz(rhs.gp().X(), trap_div_by_zero);
  // Do div.
  Udiv(dst.gp().X(), lhs.gp().X(), rhs.gp().X());
  return true;
}

bool LiftoffAssembler::emit_i64_rems(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs,
                                     Label* trap_div_by_zero) {
  Register dst_x = dst.gp().X();
  Register lhs_x = lhs.gp().X();
  Register rhs_x = rhs.gp().X();
  // Do early div.
  // No need to check kMinInt / -1 because the result is kMinInt and then
  // kMinInt * -1 -> kMinInt. In this case, the Msub result is therefore 0.
  UseScratchRegisterScope temps(this);
  Register scratch = temps.AcquireX();
  Sdiv(scratch, lhs_x, rhs_x);
  // Check for division by zero.
  Cbz(rhs_x, trap_div_by_zero);
  // Compute remainder.
  Msub(dst_x, scratch, rhs_x, lhs_x);
  return true;
}

bool LiftoffAssembler::emit_i64_remu(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs,
                                     Label* trap_div_by_zero) {
  Register dst_x = dst.gp().X();
  Register lhs_x = lhs.gp().X();
  Register rhs_x = rhs.gp().X();
  // Do early div.
  UseScratchRegisterScope temps(this);
  Register scratch = temps.AcquireX();
  Udiv(scratch, lhs_x, rhs_x);
  // Check for division by zero.
  Cbz(rhs_x, trap_div_by_zero);
  // Compute remainder.
  Msub(dst_x, scratch, rhs_x, lhs_x);
  return true;
}

void LiftoffAssembler::emit_u32_to_intptr(Register dst, Register src) {
  Uxtw(dst, src);
}

void LiftoffAssembler::emit_f32_copysign(DoubleRegister dst, DoubleRegister lhs,
                                         DoubleRegister rhs) {
  UseScratchRegisterScope temps(this);
  DoubleRegister scratch = temps.AcquireD();
  Ushr(scratch.V2S(), rhs.V2S(), 31);
  if (dst != lhs) {
    Fmov(dst.S(), lhs.S());
  }
  Sli(dst.V2S(), scratch.V2S(), 31);
}

void LiftoffAssembler::emit_f64_copysign(DoubleRegister dst, DoubleRegister lhs,
                                         DoubleRegister rhs) {
  UseScratchRegisterScope temps(this);
  DoubleRegister scratch = temps.AcquireD();
  Ushr(scratch.V1D(), rhs.V1D(), 63);
  if (dst != lhs) {
    Fmov(dst.D(), lhs.D());
  }
  Sli(dst.V1D(), scratch.V1D(), 63);
}

bool LiftoffAssembler::emit_type_conversion(WasmOpcode opcode,
                                            LiftoffRegister dst,
                                            LiftoffRegister src, Label* trap) {
  switch (opcode) {
    case kExprI32ConvertI64:
      if (src != dst) Mov(dst.gp().W(), src.gp().W());
      return true;
    case kExprI32SConvertF32:
      Fcvtzs(dst.gp().W(), src.fp().S());  // f32 -> i32 round to zero.
      // Check underflow and NaN.
      Fcmp(src.fp().S(), static_cast<float>(INT32_MIN));
      // Check overflow.
      Ccmp(dst.gp().W(), -1, VFlag, ge);
      B(trap, vs);
      return true;
    case kExprI32UConvertF32:
      Fcvtzu(dst.gp().W(), src.fp().S());  // f32 -> i32 round to zero.
      // Check underflow and NaN.
      Fcmp(src.fp().S(), -1.0);
      // Check overflow.
      Ccmp(dst.gp().W(), -1, ZFlag, gt);
      B(trap, eq);
      return true;
    case kExprI32SConvertF64: {
      // INT32_MIN and INT32_MAX are valid results, we cannot test the result
      // to detect the overflows. We could have done two immediate floating
      // point comparisons but it would have generated two conditional branches.
      UseScratchRegisterScope temps(this);
      VRegister fp_ref = temps.AcquireD();
      VRegister fp_cmp = temps.AcquireD();
      Fcvtzs(dst.gp().W(), src.fp().D());  // f64 -> i32 round to zero.
      Frintz(fp_ref, src.fp().D());        // f64 -> f64 round to zero.
      Scvtf(fp_cmp, dst.gp().W());         // i32 -> f64.
      // If comparison fails, we have an overflow or a NaN.
      Fcmp(fp_cmp, fp_ref);
      B(trap, ne);
      return true;
    }
    case kExprI32UConvertF64: {
      // INT32_MAX is a valid result, we cannot test the result to detect the
      // overflows. We could have done two immediate floating point comparisons
      // but it would have generated two conditional branches.
      UseScratchRegisterScope temps(this);
      VRegister fp_ref = temps.AcquireD();
      VRegister fp_cmp = temps.AcquireD();
      Fcvtzu(dst.gp().W(), src.fp().D());  // f64 -> i32 round to zero.
      Frintz(fp_ref, src.fp().D());        // f64 -> f64 round to zero.
      Ucvtf(fp_cmp, dst.gp().W());         // i32 -> f64.
      // If comparison fails, we have an overflow or a NaN.
      Fcmp(fp_cmp, fp_ref);
      B(trap, ne);
      return true;
    }
    case kExprI32ReinterpretF32:
      Fmov(dst.gp().W(), src.fp().S());
      return true;
    case kExprI64SConvertI32:
      Sxtw(dst.gp().X(), src.gp().W());
      return true;
    case kExprI64SConvertF32:
      Fcvtzs(dst.gp().X(), src.fp().S());  // f32 -> i64 round to zero.
      // Check underflow and NaN.
      Fcmp(src.fp().S(), static_cast<float>(INT64_MIN));
      // Check overflow.
      Ccmp(dst.gp().X(), -1, VFlag, ge);
      B(trap, vs);
      return true;
    case kExprI64UConvertF32:
      Fcvtzu(dst.gp().X(), src.fp().S());  // f32 -> i64 round to zero.
      // Check underflow and NaN.
      Fcmp(src.fp().S(), -1.0);
      // Check overflow.
      Ccmp(dst.gp().X(), -1, ZFlag, gt);
      B(trap, eq);
      return true;
    case kExprI64SConvertF64:
      Fcvtzs(dst.gp().X(), src.fp().D());  // f64 -> i64 round to zero.
      // Check underflow and NaN.
      Fcmp(src.fp().D(), static_cast<float>(INT64_MIN));
      // Check overflow.
      Ccmp(dst.gp().X(), -1, VFlag, ge);
      B(trap, vs);
      return true;
    case kExprI64UConvertF64:
      Fcvtzu(dst.gp().X(), src.fp().D());  // f64 -> i64 round to zero.
      // Check underflow and NaN.
      Fcmp(src.fp().D(), -1.0);
      // Check overflow.
      Ccmp(dst.gp().X(), -1, ZFlag, gt);
      B(trap, eq);
      return true;
    case kExprI64UConvertI32:
      Mov(dst.gp().W(), src.gp().W());
      return true;
    case kExprI64ReinterpretF64:
      Fmov(dst.gp().X(), src.fp().D());
      return true;
    case kExprF32SConvertI32:
      Scvtf(dst.fp().S(), src.gp().W());
      return true;
    case kExprF32UConvertI32:
      Ucvtf(dst.fp().S(), src.gp().W());
      return true;
    case kExprF32SConvertI64:
      Scvtf(dst.fp().S(), src.gp().X());
      return true;
    case kExprF32UConvertI64:
      Ucvtf(dst.fp().S(), src.gp().X());
      return true;
    case kExprF32ConvertF64:
      Fcvt(dst.fp().S(), src.fp().D());
      return true;
    case kExprF32ReinterpretI32:
      Fmov(dst.fp().S(), src.gp().W());
      return true;
    case kExprF64SConvertI32:
      Scvtf(dst.fp().D(), src.gp().W());
      return true;
    case kExprF64UConvertI32:
      Ucvtf(dst.fp().D(), src.gp().W());
      return true;
    case kExprF64SConvertI64:
      Scvtf(dst.fp().D(), src.gp().X());
      return true;
    case kExprF64UConvertI64:
      Ucvtf(dst.fp().D(), src.gp().X());
      return true;
    case kExprF64ConvertF32:
      Fcvt(dst.fp().D(), src.fp().S());
      return true;
    case kExprF64ReinterpretI64:
      Fmov(dst.fp().D(), src.gp().X());
      return true;
    default:
      UNREACHABLE();
  }
}

void LiftoffAssembler::emit_i32_signextend_i8(Register dst, Register src) {
  sxtb(dst, src);
}

void LiftoffAssembler::emit_i32_signextend_i16(Register dst, Register src) {
  sxth(dst, src);
}

void LiftoffAssembler::emit_i64_signextend_i8(LiftoffRegister dst,
                                              LiftoffRegister src) {
  sxtb(dst.gp(), src.gp());
}

void LiftoffAssembler::emit_i64_signextend_i16(LiftoffRegister dst,
                                               LiftoffRegister src) {
  sxth(dst.gp(), src.gp());
}

void LiftoffAssembler::emit_i64_signextend_i32(LiftoffRegister dst,
                                               LiftoffRegister src) {
  sxtw(dst.gp(), src.gp());
}

void LiftoffAssembler::emit_jump(Label* label) { B(label); }

void LiftoffAssembler::emit_jump(Register target) { Br(target); }

void LiftoffAssembler::emit_cond_jump(Condition cond, Label* label,
                                      ValueType type, Register lhs,
                                      Register rhs) {
  switch (type) {
    case kWasmI32:
      if (rhs.is_valid()) {
        Cmp(lhs.W(), rhs.W());
      } else {
        Cmp(lhs.W(), wzr);
      }
      break;
    case kWasmI64:
      if (rhs.is_valid()) {
        Cmp(lhs.X(), rhs.X());
      } else {
        Cmp(lhs.X(), xzr);
      }
      break;
    default:
      UNREACHABLE();
  }
  B(label, cond);
}

void LiftoffAssembler::emit_i32_eqz(Register dst, Register src) {
  Cmp(src.W(), wzr);
  Cset(dst.W(), eq);
}

void LiftoffAssembler::emit_i32_set_cond(Condition cond, Register dst,
                                         Register lhs, Register rhs) {
  Cmp(lhs.W(), rhs.W());
  Cset(dst.W(), cond);
}

void LiftoffAssembler::emit_i64_eqz(Register dst, LiftoffRegister src) {
  Cmp(src.gp().X(), xzr);
  Cset(dst.W(), eq);
}

void LiftoffAssembler::emit_i64_set_cond(Condition cond, Register dst,
                                         LiftoffRegister lhs,
                                         LiftoffRegister rhs) {
  Cmp(lhs.gp().X(), rhs.gp().X());
  Cset(dst.W(), cond);
}

void LiftoffAssembler::emit_f32_set_cond(Condition cond, Register dst,
                                         DoubleRegister lhs,
                                         DoubleRegister rhs) {
  Fcmp(lhs.S(), rhs.S());
  Cset(dst.W(), cond);
  if (cond != ne) {
    // If V flag set, at least one of the arguments was a Nan -> false.
    Csel(dst.W(), wzr, dst.W(), vs);
  }
}

void LiftoffAssembler::emit_f64_set_cond(Condition cond, Register dst,
                                         DoubleRegister lhs,
                                         DoubleRegister rhs) {
  Fcmp(lhs.D(), rhs.D());
  Cset(dst.W(), cond);
  if (cond != ne) {
    // If V flag set, at least one of the arguments was a Nan -> false.
    Csel(dst.W(), wzr, dst.W(), vs);
  }
}

void LiftoffAssembler::emit_f64x2_splat(LiftoffRegister dst,
                                        LiftoffRegister src) {
  Dup(dst.fp().V2D(), src.fp().D(), 0);
}

void LiftoffAssembler::emit_f32x4_splat(LiftoffRegister dst,
                                        LiftoffRegister src) {
  Dup(dst.fp().V4S(), src.fp().S(), 0);
}

void LiftoffAssembler::emit_f32x4_add(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  bailout(kSimd, "f32x4add");
}

void LiftoffAssembler::emit_i64x2_splat(LiftoffRegister dst,
                                        LiftoffRegister src) {
  Dup(dst.fp().V2D(), src.gp().X());
}

void LiftoffAssembler::emit_i32x4_splat(LiftoffRegister dst,
                                        LiftoffRegister src) {
  Dup(dst.fp().V4S(), src.gp().W());
}

void LiftoffAssembler::emit_i32x4_add(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  bailout(kSimd, "i32x4add");
}

void LiftoffAssembler::emit_i16x8_splat(LiftoffRegister dst,
                                        LiftoffRegister src) {
  Dup(dst.fp().V8H(), src.gp().W());
}

void LiftoffAssembler::emit_i16x8_add(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  bailout(kSimd, "i16x8add");
}

void LiftoffAssembler::emit_i8x16_splat(LiftoffRegister dst,
                                        LiftoffRegister src) {
  Dup(dst.fp().V16B(), src.gp().W());
}

void LiftoffAssembler::StackCheck(Label* ool_code, Register limit_address) {
  Ldr(limit_address, MemOperand(limit_address));
  Cmp(sp, limit_address);
  B(ool_code, ls);
}

void LiftoffAssembler::CallTrapCallbackForTesting() {
  CallCFunction(ExternalReference::wasm_call_trap_callback_for_testing(), 0);
}

void LiftoffAssembler::AssertUnreachable(AbortReason reason) {
  TurboAssembler::AssertUnreachable(reason);
}

void LiftoffAssembler::PushRegisters(LiftoffRegList regs) {
  PushCPURegList(liftoff::PadRegList(regs.GetGpList()));
  PushCPURegList(liftoff::PadVRegList(regs.GetFpList()));
}

void LiftoffAssembler::PopRegisters(LiftoffRegList regs) {
  PopCPURegList(liftoff::PadVRegList(regs.GetFpList()));
  PopCPURegList(liftoff::PadRegList(regs.GetGpList()));
}

void LiftoffAssembler::DropStackSlotsAndRet(uint32_t num_stack_slots) {
  DropSlots(num_stack_slots);
  Ret();
}

void LiftoffAssembler::CallC(const wasm::FunctionSig* sig,
                             const LiftoffRegister* args,
                             const LiftoffRegister* rets,
                             ValueType out_argument_type, int stack_bytes,
                             ExternalReference ext_ref) {
  // The stack pointer is required to be quadword aligned.
  int total_size = RoundUp(stack_bytes, kQuadWordSizeInBytes);
  // Reserve space in the stack.
  Claim(total_size, 1);

  int arg_bytes = 0;
  for (ValueType param_type : sig->parameters()) {
    Poke(liftoff::GetRegFromType(*args++, param_type), arg_bytes);
    arg_bytes += ValueTypes::MemSize(param_type);
  }
  DCHECK_LE(arg_bytes, stack_bytes);

  // Pass a pointer to the buffer with the arguments to the C function.
  Mov(x0, sp);

  // Now call the C function.
  constexpr int kNumCCallArgs = 1;
  CallCFunction(ext_ref, kNumCCallArgs);

  // Move return value to the right register.
  const LiftoffRegister* next_result_reg = rets;
  if (sig->return_count() > 0) {
    DCHECK_EQ(1, sig->return_count());
    constexpr Register kReturnReg = x0;
    if (kReturnReg != next_result_reg->gp()) {
      Move(*next_result_reg, LiftoffRegister(kReturnReg), sig->GetReturn(0));
    }
    ++next_result_reg;
  }

  // Load potential output value from the buffer on the stack.
  if (out_argument_type != kWasmStmt) {
    Peek(liftoff::GetRegFromType(*next_result_reg, out_argument_type), 0);
  }

  Drop(total_size, 1);
}

void LiftoffAssembler::CallNativeWasmCode(Address addr) {
  Call(addr, RelocInfo::WASM_CALL);
}

void LiftoffAssembler::CallIndirect(const wasm::FunctionSig* sig,
                                    compiler::CallDescriptor* call_descriptor,
                                    Register target) {
  // For Arm64, we have more cache registers than wasm parameters. That means
  // that target will always be in a register.
  DCHECK(target.is_valid());
  Call(target);
}

void LiftoffAssembler::CallRuntimeStub(WasmCode::RuntimeStubId sid) {
  // A direct call to a wasm runtime stub defined in this module.
  // Just encode the stub index. This will be patched at relocation.
  Call(static_cast<Address>(sid), RelocInfo::WASM_STUB_CALL);
}

void LiftoffAssembler::AllocateStackSlot(Register addr, uint32_t size) {
  // The stack pointer is required to be quadword aligned.
  size = RoundUp(size, kQuadWordSizeInBytes);
  Claim(size, 1);
  Mov(addr, sp);
}

void LiftoffAssembler::DeallocateStackSlot(uint32_t size) {
  // The stack pointer is required to be quadword aligned.
  size = RoundUp(size, kQuadWordSizeInBytes);
  Drop(size, 1);
}

void LiftoffStackSlots::Construct() {
  size_t num_slots = 0;
  for (auto& slot : slots_) {
    num_slots += slot.src_.type() == kWasmS128 ? 2 : 1;
  }
  // The stack pointer is required to be quadword aligned.
  asm_->Claim(RoundUp(num_slots, 2));
  size_t poke_offset = num_slots * kXRegSize;
  for (auto& slot : slots_) {
    poke_offset -= slot.src_.type() == kWasmS128 ? kXRegSize * 2 : kXRegSize;
    switch (slot.src_.loc()) {
      case LiftoffAssembler::VarState::kStack: {
        UseScratchRegisterScope temps(asm_);
        CPURegister scratch = liftoff::AcquireByType(&temps, slot.src_.type());
        asm_->Ldr(scratch, liftoff::GetStackSlot(slot.src_offset_));
        asm_->Poke(scratch, poke_offset);
        break;
      }
      case LiftoffAssembler::VarState::kRegister:
        asm_->Poke(liftoff::GetRegFromType(slot.src_.reg(), slot.src_.type()),
                   poke_offset);
        break;
      case LiftoffAssembler::VarState::kIntConst:
        DCHECK(slot.src_.type() == kWasmI32 || slot.src_.type() == kWasmI64);
        if (slot.src_.i32_const() == 0) {
          Register zero_reg = slot.src_.type() == kWasmI32 ? wzr : xzr;
          asm_->Poke(zero_reg, poke_offset);
        } else {
          UseScratchRegisterScope temps(asm_);
          Register scratch = slot.src_.type() == kWasmI32 ? temps.AcquireW()
                                                          : temps.AcquireX();
          asm_->Mov(scratch, int64_t{slot.src_.i32_const()});
          asm_->Poke(scratch, poke_offset);
        }
        break;
    }
  }
}

}  // namespace wasm
}  // namespace internal
}  // namespace v8

#endif  // V8_WASM_BASELINE_ARM64_LIFTOFF_ASSEMBLER_ARM64_H_

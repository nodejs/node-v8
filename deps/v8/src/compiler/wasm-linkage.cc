// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/assembler-inl.h"
#include "src/base/lazy-instance.h"
#include "src/macro-assembler.h"
#include "src/register-configuration.h"

#include "src/compiler/linkage.h"
#include "src/compiler/wasm-compiler.h"

#include "src/zone/zone.h"

namespace v8 {
namespace internal {
namespace compiler {

using wasm::ValueType;

namespace {

MachineType MachineTypeFor(ValueType type) {
  switch (type) {
    case wasm::kWasmI32:
      return MachineType::Int32();
    case wasm::kWasmI64:
      return MachineType::Int64();
    case wasm::kWasmF64:
      return MachineType::Float64();
    case wasm::kWasmF32:
      return MachineType::Float32();
    case wasm::kWasmS128:
      return MachineType::Simd128();
    default:
      UNREACHABLE();
  }
}

LinkageLocation stackloc(int i, MachineType type) {
  return LinkageLocation::ForCallerFrameSlot(i, type);
}


#if V8_TARGET_ARCH_IA32
// ===========================================================================
// == ia32 ===================================================================
// ===========================================================================
#define GP_PARAM_REGISTERS esi, eax, edx, ecx, ebx
#define GP_RETURN_REGISTERS eax, edx
#define FP_PARAM_REGISTERS xmm1, xmm2, xmm3, xmm4, xmm5, xmm6
#define FP_RETURN_REGISTERS xmm1, xmm2

#elif V8_TARGET_ARCH_X64
// ===========================================================================
// == x64 ====================================================================
// ===========================================================================
#define GP_PARAM_REGISTERS rsi, rax, rdx, rcx, rbx, rdi
#define GP_RETURN_REGISTERS rax, rdx
#define FP_PARAM_REGISTERS xmm1, xmm2, xmm3, xmm4, xmm5, xmm6
#define FP_RETURN_REGISTERS xmm1, xmm2

#elif V8_TARGET_ARCH_ARM
// ===========================================================================
// == arm ====================================================================
// ===========================================================================
#define GP_PARAM_REGISTERS r3, r0, r1, r2
#define GP_RETURN_REGISTERS r0, r1
#define FP_PARAM_REGISTERS d0, d1, d2, d3, d4, d5, d6, d7
#define FP_RETURN_REGISTERS d0, d1

#elif V8_TARGET_ARCH_ARM64
// ===========================================================================
// == arm64 ====================================================================
// ===========================================================================
#define GP_PARAM_REGISTERS x7, x0, x1, x2, x3, x4, x5, x6
#define GP_RETURN_REGISTERS x0, x1
#define FP_PARAM_REGISTERS d0, d1, d2, d3, d4, d5, d6, d7
#define FP_RETURN_REGISTERS d0, d1

#elif V8_TARGET_ARCH_MIPS
// ===========================================================================
// == mips ===================================================================
// ===========================================================================
#define GP_PARAM_REGISTERS a0, a1, a2, a3
#define GP_RETURN_REGISTERS v0, v1
#define FP_PARAM_REGISTERS f2, f4, f6, f8, f10, f12, f14
#define FP_RETURN_REGISTERS f2, f4

#elif V8_TARGET_ARCH_MIPS64
// ===========================================================================
// == mips64 =================================================================
// ===========================================================================
#define GP_PARAM_REGISTERS a0, a1, a2, a3, a4, a5, a6, a7
#define GP_RETURN_REGISTERS v0, v1
#define FP_PARAM_REGISTERS f2, f4, f6, f8, f10, f12, f14
#define FP_RETURN_REGISTERS f2, f4

#elif V8_TARGET_ARCH_PPC || V8_TARGET_ARCH_PPC64
// ===========================================================================
// == ppc & ppc64 ============================================================
// ===========================================================================
#define GP_PARAM_REGISTERS r10, r3, r4, r5, r6, r7, r8, r9
#define GP_RETURN_REGISTERS r3, r4
#define FP_PARAM_REGISTERS d1, d2, d3, d4, d5, d6, d7, d8
#define FP_RETURN_REGISTERS d1, d2

#elif V8_TARGET_ARCH_S390X
// ===========================================================================
// == s390x ==================================================================
// ===========================================================================
#define GP_PARAM_REGISTERS r6, r2, r3, r4, r5
#define GP_RETURN_REGISTERS r2, r3
#define FP_PARAM_REGISTERS d0, d2, d4, d6
#define FP_RETURN_REGISTERS d0, d2, d4, d6

#elif V8_TARGET_ARCH_S390
// ===========================================================================
// == s390 ===================================================================
// ===========================================================================
#define GP_PARAM_REGISTERS r6, r2, r3, r4, r5
#define GP_RETURN_REGISTERS r2, r3
#define FP_PARAM_REGISTERS d0, d2
#define FP_RETURN_REGISTERS d0, d2

#else
// ===========================================================================
// == unknown ================================================================
// ===========================================================================
// Do not use any registers, we will just always use the stack.
#define GP_PARAM_REGISTERS
#define GP_RETURN_REGISTERS
#define FP_PARAM_REGISTERS
#define FP_RETURN_REGISTERS

#endif


// Helper for allocating either an GP or FP reg, or the next stack slot.
struct Allocator {
  constexpr Allocator(const Register* gp, int gpc, const DoubleRegister* fp,
                      int fpc)
      : gp_count(gpc),
        gp_offset(0),
        gp_regs(gp),
        fp_count(fpc),
        fp_offset(0),
        fp_regs(fp),
        stack_offset(0) {}

  int gp_count;
  int gp_offset;
  const Register* gp_regs;

  int fp_count;
  int fp_offset;
  const DoubleRegister* fp_regs;

  int stack_offset;

  void AdjustStackOffset(int offset) { stack_offset += offset; }

  LinkageLocation Next(ValueType type) {
    if (IsFloatingPoint(type)) {
      // Allocate a floating point register/stack location.
      if (fp_offset < fp_count) {
        DoubleRegister reg = fp_regs[fp_offset++];
#if V8_TARGET_ARCH_ARM
        // Allocate floats using a double register, but modify the code to
        // reflect how ARM FP registers alias.
        // TODO(bbudge) Modify wasm linkage to allow use of all float regs.
        if (type == wasm::kWasmF32) {
          int float_reg_code = reg.code() * 2;
          DCHECK_GT(RegisterConfiguration::kMaxFPRegisters, float_reg_code);
          return LinkageLocation::ForRegister(
              DoubleRegister::from_code(float_reg_code).code(),
              MachineTypeFor(type));
        }
#endif
        return LinkageLocation::ForRegister(reg.code(), MachineTypeFor(type));
      } else {
        int offset = -1 - stack_offset;
        stack_offset += Words(type);
        return stackloc(offset, MachineTypeFor(type));
      }
    } else {
      // Allocate a general purpose register/stack location.
      if (gp_offset < gp_count) {
        return LinkageLocation::ForRegister(gp_regs[gp_offset++].code(),
                                            MachineTypeFor(type));
      } else {
        int offset = -1 - stack_offset;
        stack_offset += Words(type);
        return stackloc(offset, MachineTypeFor(type));
      }
    }
  }
  bool IsFloatingPoint(ValueType type) {
    return type == wasm::kWasmF32 || type == wasm::kWasmF64;
  }
  int Words(ValueType type) {
    if (kPointerSize < 8 &&
        (type == wasm::kWasmI64 || type == wasm::kWasmF64)) {
      return 2;
    }
    return 1;
  }
};

static constexpr Register kGPReturnRegisters[] = {GP_RETURN_REGISTERS};
static constexpr DoubleRegister kFPReturnRegisters[] = {FP_RETURN_REGISTERS};
static constexpr Register kGPParamRegisters[] = {GP_PARAM_REGISTERS};
static constexpr DoubleRegister kFPParamRegisters[] = {FP_PARAM_REGISTERS};
static constexpr Allocator return_registers(kGPReturnRegisters,
                                            arraysize(kGPReturnRegisters),
                                            kFPReturnRegisters,
                                            arraysize(kFPReturnRegisters));
static constexpr Allocator parameter_registers(kGPParamRegisters,
                                               arraysize(kGPParamRegisters),
                                               kFPParamRegisters,
                                               arraysize(kFPParamRegisters));

}  // namespace

// General code uses the above configuration data.
CallDescriptor* GetWasmCallDescriptor(Zone* zone, wasm::FunctionSig* fsig,
                                      bool use_retpoline) {
  // The '+ 1' here is to accomodate the wasm_context as first parameter.
  LocationSignature::Builder locations(zone, fsig->return_count(),
                                       fsig->parameter_count() + 1);

  // Add register and/or stack parameter(s).
  Allocator params = parameter_registers;

  // The wasm_context.
  locations.AddParam(params.Next(MachineType::PointerRepresentation()));

  const int parameter_count = static_cast<int>(fsig->parameter_count());
  for (int i = 0; i < parameter_count; i++) {
    ValueType param = fsig->GetParam(i);
    auto l = params.Next(param);
    locations.AddParam(l);
  }

  // Add return location(s).
  Allocator rets = return_registers;
  rets.AdjustStackOffset(params.stack_offset);

  const int return_count = static_cast<int>(locations.return_count_);
  for (int i = 0; i < return_count; i++) {
    ValueType ret = fsig->GetReturn(i);
    auto l = rets.Next(ret);
    locations.AddReturn(l);
  }

  const RegList kCalleeSaveRegisters = 0;
  const RegList kCalleeSaveFPRegisters = 0;

  // The target for wasm calls is always a code object.
  MachineType target_type = MachineType::Pointer();
  LinkageLocation target_loc = LinkageLocation::ForAnyRegister(target_type);

  CallDescriptor::Kind kind = CallDescriptor::kCallWasmFunction;

  return new (zone) CallDescriptor(       // --
      kind,                               // kind
      target_type,                        // target MachineType
      target_loc,                         // target location
      locations.Build(),                  // location_sig
      params.stack_offset,                // stack_parameter_count
      compiler::Operator::kNoProperties,  // properties
      kCalleeSaveRegisters,               // callee-saved registers
      kCalleeSaveFPRegisters,             // callee-saved fp regs
      use_retpoline ? CallDescriptor::kRetpoline
                    : CallDescriptor::kNoFlags,  // flags
      "wasm-call",                               // debug name
      0,                                         // allocatable registers
      rets.stack_offset - params.stack_offset);  // stack_return_count
}

CallDescriptor* ReplaceTypeInCallDescriptorWith(
    Zone* zone, CallDescriptor* call_descriptor, size_t num_replacements,
    MachineType input_type, MachineRepresentation output_type) {
  size_t parameter_count = call_descriptor->ParameterCount();
  size_t return_count = call_descriptor->ReturnCount();
  for (size_t i = 0; i < call_descriptor->ParameterCount(); i++) {
    if (call_descriptor->GetParameterType(i) == input_type) {
      parameter_count += num_replacements - 1;
    }
  }
  for (size_t i = 0; i < call_descriptor->ReturnCount(); i++) {
    if (call_descriptor->GetReturnType(i) == input_type) {
      return_count += num_replacements - 1;
    }
  }
  if (parameter_count == call_descriptor->ParameterCount() &&
      return_count == call_descriptor->ReturnCount()) {
    return call_descriptor;
  }

  LocationSignature::Builder locations(zone, return_count, parameter_count);

  Allocator params = parameter_registers;
  for (size_t i = 0; i < call_descriptor->ParameterCount(); i++) {
    if (call_descriptor->GetParameterType(i) == input_type) {
      for (size_t j = 0; j < num_replacements; j++) {
        locations.AddParam(params.Next(output_type));
      }
    } else {
      locations.AddParam(
          params.Next(call_descriptor->GetParameterType(i).representation()));
    }
  }

  Allocator rets = return_registers;
  rets.AdjustStackOffset(params.stack_offset);
  for (size_t i = 0; i < call_descriptor->ReturnCount(); i++) {
    if (call_descriptor->GetReturnType(i) == input_type) {
      for (size_t j = 0; j < num_replacements; j++) {
        locations.AddReturn(rets.Next(output_type));
      }
    } else {
      locations.AddReturn(
          rets.Next(call_descriptor->GetReturnType(i).representation()));
    }
  }

  return new (zone) CallDescriptor(               // --
      call_descriptor->kind(),                    // kind
      call_descriptor->GetInputType(0),           // target MachineType
      call_descriptor->GetInputLocation(0),       // target location
      locations.Build(),                          // location_sig
      params.stack_offset,                        // stack_parameter_count
      call_descriptor->properties(),              // properties
      call_descriptor->CalleeSavedRegisters(),    // callee-saved registers
      call_descriptor->CalleeSavedFPRegisters(),  // callee-saved fp regs
      call_descriptor->flags(),                   // flags
      call_descriptor->debug_name(),              // debug name
      call_descriptor->AllocatableRegisters(),    // allocatable registers
      rets.stack_offset - params.stack_offset);   // stack_return_count
}

CallDescriptor* GetI32WasmCallDescriptor(Zone* zone,
                                         CallDescriptor* call_descriptor) {
  return ReplaceTypeInCallDescriptorWith(zone, call_descriptor, 2,
                                         MachineType::Int64(),
                                         MachineRepresentation::kWord32);
}

CallDescriptor* GetI32WasmCallDescriptorForSimd(
    Zone* zone, CallDescriptor* call_descriptor) {
  return ReplaceTypeInCallDescriptorWith(zone, call_descriptor, 4,
                                         MachineType::Simd128(),
                                         MachineRepresentation::kWord32);
}

#undef GP_PARAM_REGISTERS
#undef GP_RETURN_REGISTERS
#undef FP_PARAM_REGISTERS
#undef FP_RETURN_REGISTERS

}  // namespace compiler
}  // namespace internal
}  // namespace v8

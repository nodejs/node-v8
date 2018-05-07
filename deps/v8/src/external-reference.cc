// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/external-reference.h"

#include "src/api.h"
#include "src/base/ieee754.h"
#include "src/codegen.h"
#include "src/compiler/code-assembler.h"
#include "src/counters.h"
#include "src/debug/debug.h"
#include "src/deoptimizer.h"
#include "src/elements.h"
#include "src/heap/heap.h"
#include "src/ic/stub-cache.h"
#include "src/interpreter/interpreter.h"
#include "src/isolate.h"
#include "src/objects-inl.h"
#include "src/regexp/regexp-stack.h"
#include "src/simulator-base.h"
#include "src/string-search.h"
#include "src/wasm/wasm-external-refs.h"

// Include native regexp-macro-assembler.
#ifndef V8_INTERPRETED_REGEXP
#if V8_TARGET_ARCH_IA32
#include "src/regexp/ia32/regexp-macro-assembler-ia32.h"  // NOLINT
#elif V8_TARGET_ARCH_X64
#include "src/regexp/x64/regexp-macro-assembler-x64.h"  // NOLINT
#elif V8_TARGET_ARCH_ARM64
#include "src/regexp/arm64/regexp-macro-assembler-arm64.h"  // NOLINT
#elif V8_TARGET_ARCH_ARM
#include "src/regexp/arm/regexp-macro-assembler-arm.h"  // NOLINT
#elif V8_TARGET_ARCH_PPC
#include "src/regexp/ppc/regexp-macro-assembler-ppc.h"  // NOLINT
#elif V8_TARGET_ARCH_MIPS
#include "src/regexp/mips/regexp-macro-assembler-mips.h"  // NOLINT
#elif V8_TARGET_ARCH_MIPS64
#include "src/regexp/mips64/regexp-macro-assembler-mips64.h"  // NOLINT
#elif V8_TARGET_ARCH_S390
#include "src/regexp/s390/regexp-macro-assembler-s390.h"  // NOLINT
#else  // Unknown architecture.
#error "Unknown architecture."
#endif  // Target architecture.
#endif  // V8_INTERPRETED_REGEXP

#ifdef V8_INTL_SUPPORT
#include "src/intl.h"
#endif  // V8_INTL_SUPPORT

namespace v8 {
namespace internal {

// -----------------------------------------------------------------------------
// Common double constants.

constexpr double double_min_int_constant = kMinInt;
constexpr double double_one_half_constant = 0.5;
constexpr double double_minus_one_half_constant = -0.5;
constexpr double double_negative_infinity_constant = -V8_INFINITY;
constexpr uint64_t double_the_hole_nan_constant = kHoleNanInt64;
constexpr double double_uint32_bias_constant =
    static_cast<double>(kMaxUInt32) + 1;

constexpr struct V8_ALIGNED(16) {
  uint32_t a;
  uint32_t b;
  uint32_t c;
  uint32_t d;
} float_absolute_constant = {0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF};

constexpr struct V8_ALIGNED(16) {
  uint32_t a;
  uint32_t b;
  uint32_t c;
  uint32_t d;
} float_negate_constant = {0x80000000, 0x80000000, 0x80000000, 0x80000000};

constexpr struct V8_ALIGNED(16) {
  uint64_t a;
  uint64_t b;
} double_absolute_constant = {uint64_t{0x7FFFFFFFFFFFFFFF},
                              uint64_t{0x7FFFFFFFFFFFFFFF}};

constexpr struct V8_ALIGNED(16) {
  uint64_t a;
  uint64_t b;
} double_negate_constant = {uint64_t{0x8000000000000000},
                            uint64_t{0x8000000000000000}};

// Implementation of ExternalReference

static ExternalReference::Type BuiltinCallTypeForResultSize(int result_size) {
  switch (result_size) {
    case 1:
      return ExternalReference::BUILTIN_CALL;
    case 2:
      return ExternalReference::BUILTIN_CALL_PAIR;
  }
  UNREACHABLE();
}

// static
ExternalReference ExternalReference::Create(
    ApiFunction* fun, Type type = ExternalReference::BUILTIN_CALL) {
  return ExternalReference(Redirect(fun->address(), type));
}

// static
ExternalReference ExternalReference::Create(Runtime::FunctionId id) {
  return Create(Runtime::FunctionForId(id));
}

// static
ExternalReference ExternalReference::Create(const Runtime::Function* f) {
  return ExternalReference(
      Redirect(f->entry, BuiltinCallTypeForResultSize(f->result_size)));
}

// static
ExternalReference ExternalReference::Create(Address address) {
  return ExternalReference(Redirect(address));
}

ExternalReference ExternalReference::isolate_address(Isolate* isolate) {
  return ExternalReference(isolate);
}

ExternalReference ExternalReference::builtins_address(Isolate* isolate) {
  return ExternalReference(isolate->builtins()->builtins_table_address());
}

ExternalReference ExternalReference::handle_scope_implementer_address(
    Isolate* isolate) {
  return ExternalReference(isolate->handle_scope_implementer_address());
}

ExternalReference ExternalReference::pending_microtask_count_address(
    Isolate* isolate) {
  return ExternalReference(isolate->pending_microtask_count_address());
}

ExternalReference ExternalReference::interpreter_dispatch_table_address(
    Isolate* isolate) {
  return ExternalReference(isolate->interpreter()->dispatch_table_address());
}

ExternalReference ExternalReference::interpreter_dispatch_counters(
    Isolate* isolate) {
  return ExternalReference(
      isolate->interpreter()->bytecode_dispatch_counters_table());
}

ExternalReference ExternalReference::bytecode_size_table_address() {
  return ExternalReference(
      interpreter::Bytecodes::bytecode_size_table_address());
}

// static
ExternalReference ExternalReference::Create(StatsCounter* counter) {
  return ExternalReference(
      reinterpret_cast<Address>(counter->GetInternalPointer()));
}

// static
ExternalReference ExternalReference::Create(IsolateAddressId id,
                                            Isolate* isolate) {
  return ExternalReference(isolate->get_address_from_id(id));
}

// static
ExternalReference ExternalReference::Create(const SCTableReference& table_ref) {
  return ExternalReference(table_ref.address());
}

ExternalReference
ExternalReference::incremental_marking_record_write_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(IncrementalMarking::RecordWriteFromCode)));
}

ExternalReference ExternalReference::store_buffer_overflow_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(StoreBuffer::StoreBufferOverflow)));
}

ExternalReference ExternalReference::delete_handle_scope_extensions() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(HandleScope::DeleteExtensions)));
}

ExternalReference ExternalReference::get_date_field_function() {
  return ExternalReference(Redirect(FUNCTION_ADDR(JSDate::GetField)));
}

ExternalReference ExternalReference::date_cache_stamp(Isolate* isolate) {
  return ExternalReference(isolate->date_cache()->stamp_address());
}

// static
ExternalReference
ExternalReference::runtime_function_table_address_for_unittests(
    Isolate* isolate) {
  return runtime_function_table_address(isolate);
}

// static
Address ExternalReference::Redirect(Address address, Type type) {
#ifdef USE_SIMULATOR
  return SimulatorBase::RedirectExternalReference(address, type);
#else
  return address;
#endif
}

ExternalReference ExternalReference::stress_deopt_count(Isolate* isolate) {
  return ExternalReference(isolate->stress_deopt_count_address());
}

ExternalReference ExternalReference::force_slow_path(Isolate* isolate) {
  return ExternalReference(isolate->force_slow_path_address());
}

ExternalReference ExternalReference::new_deoptimizer_function() {
  return ExternalReference(Redirect(FUNCTION_ADDR(Deoptimizer::New)));
}

ExternalReference ExternalReference::compute_output_frames_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(Deoptimizer::ComputeOutputFrames)));
}

ExternalReference ExternalReference::wasm_f32_trunc() {
  return ExternalReference(Redirect(FUNCTION_ADDR(wasm::f32_trunc_wrapper)));
}
ExternalReference ExternalReference::wasm_f32_floor() {
  return ExternalReference(Redirect(FUNCTION_ADDR(wasm::f32_floor_wrapper)));
}
ExternalReference ExternalReference::wasm_f32_ceil() {
  return ExternalReference(Redirect(FUNCTION_ADDR(wasm::f32_ceil_wrapper)));
}
ExternalReference ExternalReference::wasm_f32_nearest_int() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(wasm::f32_nearest_int_wrapper)));
}

ExternalReference ExternalReference::wasm_f64_trunc() {
  return ExternalReference(Redirect(FUNCTION_ADDR(wasm::f64_trunc_wrapper)));
}

ExternalReference ExternalReference::wasm_f64_floor() {
  return ExternalReference(Redirect(FUNCTION_ADDR(wasm::f64_floor_wrapper)));
}

ExternalReference ExternalReference::wasm_f64_ceil() {
  return ExternalReference(Redirect(FUNCTION_ADDR(wasm::f64_ceil_wrapper)));
}

ExternalReference ExternalReference::wasm_f64_nearest_int() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(wasm::f64_nearest_int_wrapper)));
}

ExternalReference ExternalReference::wasm_int64_to_float32() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(wasm::int64_to_float32_wrapper)));
}

ExternalReference ExternalReference::wasm_uint64_to_float32() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(wasm::uint64_to_float32_wrapper)));
}

ExternalReference ExternalReference::wasm_int64_to_float64() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(wasm::int64_to_float64_wrapper)));
}

ExternalReference ExternalReference::wasm_uint64_to_float64() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(wasm::uint64_to_float64_wrapper)));
}

ExternalReference ExternalReference::wasm_float32_to_int64() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(wasm::float32_to_int64_wrapper)));
}

ExternalReference ExternalReference::wasm_float32_to_uint64() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(wasm::float32_to_uint64_wrapper)));
}

ExternalReference ExternalReference::wasm_float64_to_int64() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(wasm::float64_to_int64_wrapper)));
}

ExternalReference ExternalReference::wasm_float64_to_uint64() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(wasm::float64_to_uint64_wrapper)));
}

ExternalReference ExternalReference::wasm_int64_div() {
  return ExternalReference(Redirect(FUNCTION_ADDR(wasm::int64_div_wrapper)));
}

ExternalReference ExternalReference::wasm_int64_mod() {
  return ExternalReference(Redirect(FUNCTION_ADDR(wasm::int64_mod_wrapper)));
}

ExternalReference ExternalReference::wasm_uint64_div() {
  return ExternalReference(Redirect(FUNCTION_ADDR(wasm::uint64_div_wrapper)));
}

ExternalReference ExternalReference::wasm_uint64_mod() {
  return ExternalReference(Redirect(FUNCTION_ADDR(wasm::uint64_mod_wrapper)));
}

ExternalReference ExternalReference::wasm_word32_ctz() {
  return ExternalReference(Redirect(FUNCTION_ADDR(wasm::word32_ctz_wrapper)));
}

ExternalReference ExternalReference::wasm_word64_ctz() {
  return ExternalReference(Redirect(FUNCTION_ADDR(wasm::word64_ctz_wrapper)));
}

ExternalReference ExternalReference::wasm_word32_popcnt() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(wasm::word32_popcnt_wrapper)));
}

ExternalReference ExternalReference::wasm_word64_popcnt() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(wasm::word64_popcnt_wrapper)));
}

ExternalReference ExternalReference::wasm_word32_rol() {
  return ExternalReference(Redirect(FUNCTION_ADDR(wasm::word32_rol_wrapper)));
}

ExternalReference ExternalReference::wasm_word32_ror() {
  return ExternalReference(Redirect(FUNCTION_ADDR(wasm::word32_ror_wrapper)));
}

static void f64_acos_wrapper(Address data) {
  double input = ReadUnalignedValue<double>(data);
  WriteUnalignedValue(data, base::ieee754::acos(input));
}

ExternalReference ExternalReference::f64_acos_wrapper_function() {
  return ExternalReference(Redirect(FUNCTION_ADDR(f64_acos_wrapper)));
}

static void f64_asin_wrapper(Address data) {
  double input = ReadUnalignedValue<double>(data);
  WriteUnalignedValue<double>(data, base::ieee754::asin(input));
}

ExternalReference ExternalReference::f64_asin_wrapper_function() {
  return ExternalReference(Redirect(FUNCTION_ADDR(f64_asin_wrapper)));
}

ExternalReference ExternalReference::wasm_float64_pow() {
  return ExternalReference(Redirect(FUNCTION_ADDR(wasm::float64_pow_wrapper)));
}

ExternalReference ExternalReference::wasm_set_thread_in_wasm_flag() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(wasm::set_thread_in_wasm_flag)));
}

ExternalReference ExternalReference::wasm_clear_thread_in_wasm_flag() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(wasm::clear_thread_in_wasm_flag)));
}

static void f64_mod_wrapper(Address data) {
  double dividend = ReadUnalignedValue<double>(data);
  double divisor = ReadUnalignedValue<double>(data + sizeof(dividend));
  WriteUnalignedValue<double>(data, Modulo(dividend, divisor));
}

ExternalReference ExternalReference::f64_mod_wrapper_function() {
  return ExternalReference(Redirect(FUNCTION_ADDR(f64_mod_wrapper)));
}

ExternalReference ExternalReference::wasm_call_trap_callback_for_testing() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(wasm::call_trap_callback_for_testing)));
}

ExternalReference ExternalReference::log_enter_external_function() {
  return ExternalReference(Redirect(FUNCTION_ADDR(Logger::EnterExternal)));
}

ExternalReference ExternalReference::log_leave_external_function() {
  return ExternalReference(Redirect(FUNCTION_ADDR(Logger::LeaveExternal)));
}

ExternalReference ExternalReference::roots_array_start(Isolate* isolate) {
  return ExternalReference(isolate->heap()->roots_array_start());
}

ExternalReference ExternalReference::allocation_sites_list_address(
    Isolate* isolate) {
  return ExternalReference(isolate->heap()->allocation_sites_list_address());
}

ExternalReference ExternalReference::address_of_stack_limit(Isolate* isolate) {
  return ExternalReference(isolate->stack_guard()->address_of_jslimit());
}

ExternalReference ExternalReference::address_of_real_stack_limit(
    Isolate* isolate) {
  return ExternalReference(isolate->stack_guard()->address_of_real_jslimit());
}

ExternalReference ExternalReference::store_buffer_top(Isolate* isolate) {
  return ExternalReference(isolate->heap()->store_buffer_top_address());
}

ExternalReference ExternalReference::heap_is_marking_flag_address(
    Isolate* isolate) {
  return ExternalReference(isolate->heap()->IsMarkingFlagAddress());
}

ExternalReference ExternalReference::new_space_allocation_top_address(
    Isolate* isolate) {
  return ExternalReference(isolate->heap()->NewSpaceAllocationTopAddress());
}

ExternalReference ExternalReference::new_space_allocation_limit_address(
    Isolate* isolate) {
  return ExternalReference(isolate->heap()->NewSpaceAllocationLimitAddress());
}

ExternalReference ExternalReference::old_space_allocation_top_address(
    Isolate* isolate) {
  return ExternalReference(isolate->heap()->OldSpaceAllocationTopAddress());
}

ExternalReference ExternalReference::old_space_allocation_limit_address(
    Isolate* isolate) {
  return ExternalReference(isolate->heap()->OldSpaceAllocationLimitAddress());
}

ExternalReference ExternalReference::handle_scope_level_address(
    Isolate* isolate) {
  return ExternalReference(HandleScope::current_level_address(isolate));
}

ExternalReference ExternalReference::handle_scope_next_address(
    Isolate* isolate) {
  return ExternalReference(HandleScope::current_next_address(isolate));
}

ExternalReference ExternalReference::handle_scope_limit_address(
    Isolate* isolate) {
  return ExternalReference(HandleScope::current_limit_address(isolate));
}

ExternalReference ExternalReference::scheduled_exception_address(
    Isolate* isolate) {
  return ExternalReference(isolate->scheduled_exception_address());
}

ExternalReference ExternalReference::address_of_pending_message_obj(
    Isolate* isolate) {
  return ExternalReference(isolate->pending_message_obj_address());
}

ExternalReference ExternalReference::address_of_min_int() {
  return ExternalReference(reinterpret_cast<Address>(&double_min_int_constant));
}

ExternalReference ExternalReference::address_of_one_half() {
  return ExternalReference(
      reinterpret_cast<Address>(&double_one_half_constant));
}

ExternalReference ExternalReference::address_of_minus_one_half() {
  return ExternalReference(
      reinterpret_cast<Address>(&double_minus_one_half_constant));
}

ExternalReference ExternalReference::address_of_negative_infinity() {
  return ExternalReference(
      reinterpret_cast<Address>(&double_negative_infinity_constant));
}

ExternalReference ExternalReference::address_of_the_hole_nan() {
  return ExternalReference(
      reinterpret_cast<Address>(&double_the_hole_nan_constant));
}

ExternalReference ExternalReference::address_of_uint32_bias() {
  return ExternalReference(
      reinterpret_cast<Address>(&double_uint32_bias_constant));
}

ExternalReference ExternalReference::address_of_float_abs_constant() {
  return ExternalReference(reinterpret_cast<Address>(&float_absolute_constant));
}

ExternalReference ExternalReference::address_of_float_neg_constant() {
  return ExternalReference(reinterpret_cast<Address>(&float_negate_constant));
}

ExternalReference ExternalReference::address_of_double_abs_constant() {
  return ExternalReference(
      reinterpret_cast<Address>(&double_absolute_constant));
}

ExternalReference ExternalReference::address_of_double_neg_constant() {
  return ExternalReference(reinterpret_cast<Address>(&double_negate_constant));
}

ExternalReference ExternalReference::is_profiling_address(Isolate* isolate) {
  return ExternalReference(isolate->is_profiling_address());
}

ExternalReference ExternalReference::invoke_function_callback() {
  Address thunk_address = FUNCTION_ADDR(&InvokeFunctionCallback);
  ExternalReference::Type thunk_type = ExternalReference::PROFILING_API_CALL;
  ApiFunction thunk_fun(thunk_address);
  return ExternalReference::Create(&thunk_fun, thunk_type);
}

ExternalReference ExternalReference::invoke_accessor_getter_callback() {
  Address thunk_address = FUNCTION_ADDR(&InvokeAccessorGetterCallback);
  ExternalReference::Type thunk_type = ExternalReference::PROFILING_GETTER_CALL;
  ApiFunction thunk_fun(thunk_address);
  return ExternalReference::Create(&thunk_fun, thunk_type);
}

#ifndef V8_INTERPRETED_REGEXP

ExternalReference ExternalReference::re_check_stack_guard_state(
    Isolate* isolate) {
  Address function;
#if V8_TARGET_ARCH_X64
  function = FUNCTION_ADDR(RegExpMacroAssemblerX64::CheckStackGuardState);
#elif V8_TARGET_ARCH_IA32
  function = FUNCTION_ADDR(RegExpMacroAssemblerIA32::CheckStackGuardState);
#elif V8_TARGET_ARCH_ARM64
  function = FUNCTION_ADDR(RegExpMacroAssemblerARM64::CheckStackGuardState);
#elif V8_TARGET_ARCH_ARM
  function = FUNCTION_ADDR(RegExpMacroAssemblerARM::CheckStackGuardState);
#elif V8_TARGET_ARCH_PPC
  function = FUNCTION_ADDR(RegExpMacroAssemblerPPC::CheckStackGuardState);
#elif V8_TARGET_ARCH_MIPS
  function = FUNCTION_ADDR(RegExpMacroAssemblerMIPS::CheckStackGuardState);
#elif V8_TARGET_ARCH_MIPS64
  function = FUNCTION_ADDR(RegExpMacroAssemblerMIPS::CheckStackGuardState);
#elif V8_TARGET_ARCH_S390
  function = FUNCTION_ADDR(RegExpMacroAssemblerS390::CheckStackGuardState);
#else
  UNREACHABLE();
#endif
  return ExternalReference(Redirect(function));
}

ExternalReference ExternalReference::re_grow_stack(Isolate* isolate) {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(NativeRegExpMacroAssembler::GrowStack)));
}

ExternalReference ExternalReference::re_case_insensitive_compare_uc16(
    Isolate* isolate) {
  return ExternalReference(Redirect(
      FUNCTION_ADDR(NativeRegExpMacroAssembler::CaseInsensitiveCompareUC16)));
}

ExternalReference ExternalReference::re_word_character_map(Isolate* isolate) {
  return ExternalReference(
      NativeRegExpMacroAssembler::word_character_map_address());
}

ExternalReference ExternalReference::address_of_static_offsets_vector(
    Isolate* isolate) {
  return ExternalReference(
      reinterpret_cast<Address>(isolate->jsregexp_static_offsets_vector()));
}

ExternalReference ExternalReference::address_of_regexp_stack_limit(
    Isolate* isolate) {
  return ExternalReference(isolate->regexp_stack()->limit_address());
}

ExternalReference ExternalReference::address_of_regexp_stack_memory_address(
    Isolate* isolate) {
  return ExternalReference(isolate->regexp_stack()->memory_address());
}

ExternalReference ExternalReference::address_of_regexp_stack_memory_size(
    Isolate* isolate) {
  return ExternalReference(isolate->regexp_stack()->memory_size_address());
}

#endif  // V8_INTERPRETED_REGEXP

ExternalReference ExternalReference::ieee754_acos_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(base::ieee754::acos), BUILTIN_FP_CALL));
}

ExternalReference ExternalReference::ieee754_acosh_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(base::ieee754::acosh), BUILTIN_FP_FP_CALL));
}

ExternalReference ExternalReference::ieee754_asin_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(base::ieee754::asin), BUILTIN_FP_CALL));
}

ExternalReference ExternalReference::ieee754_asinh_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(base::ieee754::asinh), BUILTIN_FP_FP_CALL));
}

ExternalReference ExternalReference::ieee754_atan_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(base::ieee754::atan), BUILTIN_FP_CALL));
}

ExternalReference ExternalReference::ieee754_atanh_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(base::ieee754::atanh), BUILTIN_FP_FP_CALL));
}

ExternalReference ExternalReference::ieee754_atan2_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(base::ieee754::atan2), BUILTIN_FP_FP_CALL));
}

ExternalReference ExternalReference::ieee754_cbrt_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(base::ieee754::cbrt), BUILTIN_FP_FP_CALL));
}

ExternalReference ExternalReference::ieee754_cos_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(base::ieee754::cos), BUILTIN_FP_CALL));
}

ExternalReference ExternalReference::ieee754_cosh_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(base::ieee754::cosh), BUILTIN_FP_CALL));
}

ExternalReference ExternalReference::ieee754_exp_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(base::ieee754::exp), BUILTIN_FP_CALL));
}

ExternalReference ExternalReference::ieee754_expm1_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(base::ieee754::expm1), BUILTIN_FP_FP_CALL));
}

ExternalReference ExternalReference::ieee754_log_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(base::ieee754::log), BUILTIN_FP_CALL));
}

ExternalReference ExternalReference::ieee754_log1p_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(base::ieee754::log1p), BUILTIN_FP_CALL));
}

ExternalReference ExternalReference::ieee754_log10_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(base::ieee754::log10), BUILTIN_FP_CALL));
}

ExternalReference ExternalReference::ieee754_log2_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(base::ieee754::log2), BUILTIN_FP_CALL));
}

ExternalReference ExternalReference::ieee754_sin_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(base::ieee754::sin), BUILTIN_FP_CALL));
}

ExternalReference ExternalReference::ieee754_sinh_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(base::ieee754::sinh), BUILTIN_FP_CALL));
}

ExternalReference ExternalReference::ieee754_tan_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(base::ieee754::tan), BUILTIN_FP_CALL));
}

ExternalReference ExternalReference::ieee754_tanh_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(base::ieee754::tanh), BUILTIN_FP_CALL));
}

void* libc_memchr(void* string, int character, size_t search_length) {
  return memchr(string, character, search_length);
}

ExternalReference ExternalReference::libc_memchr_function() {
  return ExternalReference(Redirect(FUNCTION_ADDR(libc_memchr)));
}

void* libc_memcpy(void* dest, const void* src, size_t n) {
  return memcpy(dest, src, n);
}

ExternalReference ExternalReference::libc_memcpy_function() {
  return ExternalReference(Redirect(FUNCTION_ADDR(libc_memcpy)));
}

void* libc_memmove(void* dest, const void* src, size_t n) {
  return memmove(dest, src, n);
}

ExternalReference ExternalReference::libc_memmove_function() {
  return ExternalReference(Redirect(FUNCTION_ADDR(libc_memmove)));
}

void* libc_memset(void* dest, int byte, size_t n) {
  DCHECK_EQ(static_cast<char>(byte), byte);
  return memset(dest, byte, n);
}

ExternalReference ExternalReference::libc_memset_function() {
  return ExternalReference(Redirect(FUNCTION_ADDR(libc_memset)));
}

ExternalReference ExternalReference::printf_function() {
  return ExternalReference(Redirect(FUNCTION_ADDR(std::printf)));
}

template <typename SubjectChar, typename PatternChar>
ExternalReference ExternalReference::search_string_raw() {
  auto f = SearchStringRaw<SubjectChar, PatternChar>;
  return ExternalReference(Redirect(FUNCTION_ADDR(f)));
}

ExternalReference ExternalReference::search_string_raw_one_one() {
  return search_string_raw<const uint8_t, const uint8_t>();
}

ExternalReference ExternalReference::search_string_raw_one_two() {
  return search_string_raw<const uint8_t, const uc16>();
}

ExternalReference ExternalReference::search_string_raw_two_one() {
  return search_string_raw<const uc16, const uint8_t>();
}

ExternalReference ExternalReference::search_string_raw_two_two() {
  return search_string_raw<const uc16, const uc16>();
}

ExternalReference ExternalReference::orderedhashmap_gethash_raw() {
  auto f = OrderedHashMap::GetHash;
  return ExternalReference(Redirect(FUNCTION_ADDR(f)));
}

ExternalReference ExternalReference::get_or_create_hash_raw(Isolate* isolate) {
  typedef Smi* (*GetOrCreateHash)(Isolate * isolate, Object * key);
  GetOrCreateHash f = Object::GetOrCreateHash;
  return ExternalReference(Redirect(FUNCTION_ADDR(f)));
}

ExternalReference ExternalReference::jsreceiver_create_identity_hash(
    Isolate* isolate) {
  typedef Smi* (*CreateIdentityHash)(Isolate * isolate, JSReceiver * key);
  CreateIdentityHash f = JSReceiver::CreateIdentityHash;
  return ExternalReference(Redirect(FUNCTION_ADDR(f)));
}

ExternalReference
ExternalReference::copy_fast_number_jsarray_elements_to_typed_array() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(CopyFastNumberJSArrayElementsToTypedArray)));
}

ExternalReference
ExternalReference::copy_typed_array_elements_to_typed_array() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(CopyTypedArrayElementsToTypedArray)));
}

ExternalReference ExternalReference::copy_typed_array_elements_slice() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(CopyTypedArrayElementsSlice)));
}

ExternalReference ExternalReference::try_internalize_string_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(StringTable::LookupStringIfExists_NoAllocate)));
}

ExternalReference ExternalReference::check_object_type() {
  return ExternalReference(Redirect(FUNCTION_ADDR(CheckObjectType)));
}

#ifdef V8_INTL_SUPPORT
ExternalReference ExternalReference::intl_convert_one_byte_to_lower() {
  return ExternalReference(Redirect(FUNCTION_ADDR(ConvertOneByteToLower)));
}

ExternalReference ExternalReference::intl_to_latin1_lower_table() {
  uint8_t* ptr = const_cast<uint8_t*>(ToLatin1LowerTable());
  return ExternalReference(reinterpret_cast<Address>(ptr));
}
#endif  // V8_INTL_SUPPORT

// Explicit instantiations for all combinations of 1- and 2-byte strings.
template ExternalReference
ExternalReference::search_string_raw<const uint8_t, const uint8_t>();
template ExternalReference
ExternalReference::search_string_raw<const uint8_t, const uc16>();
template ExternalReference
ExternalReference::search_string_raw<const uc16, const uint8_t>();
template ExternalReference
ExternalReference::search_string_raw<const uc16, const uc16>();

ExternalReference ExternalReference::page_flags(Page* page) {
  return ExternalReference(reinterpret_cast<Address>(page) +
                           MemoryChunk::kFlagsOffset);
}

ExternalReference ExternalReference::ForDeoptEntry(Address entry) {
  return ExternalReference(entry);
}

ExternalReference ExternalReference::cpu_features() {
  DCHECK(CpuFeatures::initialized_);
  return ExternalReference(&CpuFeatures::supported_);
}

ExternalReference ExternalReference::promise_hook_or_debug_is_active_address(
    Isolate* isolate) {
  return ExternalReference(isolate->promise_hook_or_debug_is_active_address());
}

ExternalReference ExternalReference::debug_is_active_address(Isolate* isolate) {
  return ExternalReference(isolate->debug()->is_active_address());
}

ExternalReference ExternalReference::debug_hook_on_function_call_address(
    Isolate* isolate) {
  return ExternalReference(isolate->debug()->hook_on_function_call_address());
}

ExternalReference ExternalReference::debug_execution_mode_address(
    Isolate* isolate) {
  return ExternalReference(isolate->debug_execution_mode_address());
}

ExternalReference ExternalReference::runtime_function_table_address(
    Isolate* isolate) {
  return ExternalReference(
      const_cast<Runtime::Function*>(Runtime::RuntimeFunctionTable(isolate)));
}

ExternalReference ExternalReference::invalidate_prototype_chains_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(JSObject::InvalidatePrototypeChains)));
}

double power_helper(Isolate* isolate, double x, double y) {
  int y_int = static_cast<int>(y);
  if (y == y_int) {
    return power_double_int(x, y_int);  // Returns 1 if exponent is 0.
  }
  if (y == 0.5) {
    lazily_initialize_fast_sqrt(isolate);
    return (std::isinf(x)) ? V8_INFINITY
                           : fast_sqrt(x + 0.0, isolate);  // Convert -0 to +0.
  }
  if (y == -0.5) {
    lazily_initialize_fast_sqrt(isolate);
    return (std::isinf(x)) ? 0
                           : 1.0 / fast_sqrt(x + 0.0,
                                             isolate);  // Convert -0 to +0.
  }
  return power_double_double(x, y);
}

// Helper function to compute x^y, where y is known to be an
// integer. Uses binary decomposition to limit the number of
// multiplications; see the discussion in "Hacker's Delight" by Henry
// S. Warren, Jr., figure 11-6, page 213.
double power_double_int(double x, int y) {
  double m = (y < 0) ? 1 / x : x;
  unsigned n = (y < 0) ? -y : y;
  double p = 1;
  while (n != 0) {
    if ((n & 1) != 0) p *= m;
    m *= m;
    if ((n & 2) != 0) p *= m;
    m *= m;
    n >>= 2;
  }
  return p;
}

double power_double_double(double x, double y) {
  // The checks for special cases can be dropped in ia32 because it has already
  // been done in generated code before bailing out here.
  if (std::isnan(y) || ((x == 1 || x == -1) && std::isinf(y))) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return Pow(x, y);
}

double modulo_double_double(double x, double y) { return Modulo(x, y); }

ExternalReference ExternalReference::power_double_double_function() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(power_double_double), BUILTIN_FP_FP_CALL));
}

ExternalReference ExternalReference::mod_two_doubles_operation() {
  return ExternalReference(
      Redirect(FUNCTION_ADDR(modulo_double_double), BUILTIN_FP_FP_CALL));
}

ExternalReference ExternalReference::debug_last_step_action_address(
    Isolate* isolate) {
  return ExternalReference(isolate->debug()->last_step_action_address());
}

ExternalReference ExternalReference::debug_suspended_generator_address(
    Isolate* isolate) {
  return ExternalReference(isolate->debug()->suspended_generator_address());
}

ExternalReference ExternalReference::debug_restart_fp_address(
    Isolate* isolate) {
  return ExternalReference(isolate->debug()->restart_fp_address());
}

ExternalReference ExternalReference::fixed_typed_array_base_data_offset() {
  return ExternalReference(reinterpret_cast<void*>(
      FixedTypedArrayBase::kDataOffset - kHeapObjectTag));
}

bool operator==(ExternalReference lhs, ExternalReference rhs) {
  return lhs.address() == rhs.address();
}

bool operator!=(ExternalReference lhs, ExternalReference rhs) {
  return !(lhs == rhs);
}

size_t hash_value(ExternalReference reference) {
  return base::hash<Address>()(reference.address());
}

std::ostream& operator<<(std::ostream& os, ExternalReference reference) {
  os << reinterpret_cast<const void*>(reference.address());
  const Runtime::Function* fn = Runtime::FunctionForEntry(reference.address());
  if (fn) os << "<" << fn->name << ".entry>";
  return os;
}

}  // namespace internal
}  // namespace v8

// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_OBJECTS_SHARED_FUNCTION_INFO_INL_H_
#define V8_OBJECTS_SHARED_FUNCTION_INFO_INL_H_

#include "src/heap/heap-inl.h"
#include "src/objects/scope-info.h"
#include "src/objects/shared-function-info.h"

// Has to be the last include (doesn't have include guards):
#include "src/objects/object-macros.h"

namespace v8 {
namespace internal {

CAST_ACCESSOR(PreParsedScopeData)
ACCESSORS(PreParsedScopeData, scope_data, PodArray<uint8_t>, kScopeDataOffset)
ACCESSORS(PreParsedScopeData, child_data, FixedArray, kChildDataOffset)

TYPE_CHECKER(SharedFunctionInfo, SHARED_FUNCTION_INFO_TYPE)
CAST_ACCESSOR(SharedFunctionInfo)
DEFINE_DEOPT_ELEMENT_ACCESSORS(SharedFunctionInfo, Object)

ACCESSORS(SharedFunctionInfo, name_or_scope_info, Object,
          kNameOrScopeInfoOffset)
ACCESSORS(SharedFunctionInfo, construct_stub, Code, kConstructStubOffset)
ACCESSORS(SharedFunctionInfo, feedback_metadata, FeedbackMetadata,
          kFeedbackMetadataOffset)
ACCESSORS(SharedFunctionInfo, function_data, Object, kFunctionDataOffset)
ACCESSORS(SharedFunctionInfo, script, Object, kScriptOffset)
ACCESSORS(SharedFunctionInfo, debug_info, Object, kDebugInfoOffset)
ACCESSORS(SharedFunctionInfo, function_identifier, Object,
          kFunctionIdentifierOffset)

BIT_FIELD_ACCESSORS(SharedFunctionInfo, raw_start_position_and_type,
                    is_named_expression,
                    SharedFunctionInfo::IsNamedExpressionBit)
BIT_FIELD_ACCESSORS(SharedFunctionInfo, raw_start_position_and_type,
                    is_toplevel, SharedFunctionInfo::IsTopLevelBit)

INT_ACCESSORS(SharedFunctionInfo, function_literal_id, kFunctionLiteralIdOffset)
#if V8_SFI_HAS_UNIQUE_ID
INT_ACCESSORS(SharedFunctionInfo, unique_id, kUniqueIdOffset)
#endif
INT_ACCESSORS(SharedFunctionInfo, length, kLengthOffset)
INT_ACCESSORS(SharedFunctionInfo, internal_formal_parameter_count,
              kFormalParameterCountOffset)
INT_ACCESSORS(SharedFunctionInfo, expected_nof_properties,
              kExpectedNofPropertiesOffset)
INT_ACCESSORS(SharedFunctionInfo, raw_end_position, kEndPositionOffset)
INT_ACCESSORS(SharedFunctionInfo, raw_start_position_and_type,
              kStartPositionAndTypeOffset)
INT_ACCESSORS(SharedFunctionInfo, function_token_position,
              kFunctionTokenPositionOffset)
INT_ACCESSORS(SharedFunctionInfo, compiler_hints, kCompilerHintsOffset)

bool SharedFunctionInfo::HasSharedName() const {
  Object* value = name_or_scope_info();
  if (value->IsScopeInfo()) {
    return ScopeInfo::cast(value)->HasSharedFunctionName();
  }
  return value != kNoSharedNameSentinel;
}

String* SharedFunctionInfo::Name() const {
  if (!HasSharedName()) return GetHeap()->empty_string();
  Object* value = name_or_scope_info();
  if (value->IsScopeInfo()) {
    if (ScopeInfo::cast(value)->HasFunctionName()) {
      return String::cast(ScopeInfo::cast(value)->FunctionName());
    }
    return GetHeap()->empty_string();
  }
  return String::cast(value);
}

void SharedFunctionInfo::SetName(String* name) {
  Object* maybe_scope_info = name_or_scope_info();
  if (maybe_scope_info->IsScopeInfo()) {
    ScopeInfo::cast(maybe_scope_info)->SetFunctionName(name);
  } else {
    DCHECK(maybe_scope_info->IsString() ||
           maybe_scope_info == kNoSharedNameSentinel);
    set_name_or_scope_info(name);
  }
  UpdateFunctionMapIndex();
}

AbstractCode* SharedFunctionInfo::abstract_code() {
  if (HasBytecodeArray()) {
    return AbstractCode::cast(bytecode_array());
  } else {
    return AbstractCode::cast(GetCode());
  }
}

BIT_FIELD_ACCESSORS(SharedFunctionInfo, compiler_hints, is_wrapped,
                    SharedFunctionInfo::IsWrappedBit)
BIT_FIELD_ACCESSORS(SharedFunctionInfo, compiler_hints, allows_lazy_compilation,
                    SharedFunctionInfo::AllowLazyCompilationBit)
BIT_FIELD_ACCESSORS(SharedFunctionInfo, compiler_hints,
                    has_duplicate_parameters,
                    SharedFunctionInfo::HasDuplicateParametersBit)
BIT_FIELD_ACCESSORS(SharedFunctionInfo, compiler_hints, is_declaration,
                    SharedFunctionInfo::IsDeclarationBit)

BIT_FIELD_ACCESSORS(SharedFunctionInfo, compiler_hints, native,
                    SharedFunctionInfo::IsNativeBit)
BIT_FIELD_ACCESSORS(SharedFunctionInfo, compiler_hints, is_asm_wasm_broken,
                    SharedFunctionInfo::IsAsmWasmBrokenBit)
BIT_FIELD_ACCESSORS(SharedFunctionInfo, compiler_hints,
                    requires_instance_fields_initializer,
                    SharedFunctionInfo::RequiresInstanceFieldsInitializer)

bool SharedFunctionInfo::optimization_disabled() const {
  return disable_optimization_reason() != BailoutReason::kNoReason;
}

BailoutReason SharedFunctionInfo::disable_optimization_reason() const {
  return DisabledOptimizationReasonBits::decode(compiler_hints());
}

LanguageMode SharedFunctionInfo::language_mode() {
  STATIC_ASSERT(LanguageModeSize == 2);
  return construct_language_mode(IsStrictBit::decode(compiler_hints()));
}

void SharedFunctionInfo::set_language_mode(LanguageMode language_mode) {
  STATIC_ASSERT(LanguageModeSize == 2);
  // We only allow language mode transitions that set the same language mode
  // again or go up in the chain:
  DCHECK(is_sloppy(this->language_mode()) || is_strict(language_mode));
  int hints = compiler_hints();
  hints = IsStrictBit::update(hints, is_strict(language_mode));
  set_compiler_hints(hints);
  UpdateFunctionMapIndex();
}

FunctionKind SharedFunctionInfo::kind() const {
  return FunctionKindBits::decode(compiler_hints());
}

void SharedFunctionInfo::set_kind(FunctionKind kind) {
  int hints = compiler_hints();
  hints = FunctionKindBits::update(hints, kind);
  hints = IsClassConstructorBit::update(hints, IsClassConstructor(kind));
  hints = IsDerivedConstructorBit::update(hints, IsDerivedConstructor(kind));
  set_compiler_hints(hints);
  UpdateFunctionMapIndex();
}

bool SharedFunctionInfo::needs_home_object() const {
  return NeedsHomeObjectBit::decode(compiler_hints());
}

void SharedFunctionInfo::set_needs_home_object(bool value) {
  int hints = compiler_hints();
  hints = NeedsHomeObjectBit::update(hints, value);
  set_compiler_hints(hints);
  UpdateFunctionMapIndex();
}

int SharedFunctionInfo::function_map_index() const {
  // Note: Must be kept in sync with the FastNewClosure builtin.
  int index = Context::FIRST_FUNCTION_MAP_INDEX +
              FunctionMapIndexBits::decode(compiler_hints());
  DCHECK_LE(index, Context::LAST_FUNCTION_MAP_INDEX);
  return index;
}

void SharedFunctionInfo::set_function_map_index(int index) {
  STATIC_ASSERT(Context::LAST_FUNCTION_MAP_INDEX <=
                Context::FIRST_FUNCTION_MAP_INDEX + FunctionMapIndexBits::kMax);
  DCHECK_LE(Context::FIRST_FUNCTION_MAP_INDEX, index);
  DCHECK_LE(index, Context::LAST_FUNCTION_MAP_INDEX);
  index -= Context::FIRST_FUNCTION_MAP_INDEX;
  set_compiler_hints(FunctionMapIndexBits::update(compiler_hints(), index));
}

void SharedFunctionInfo::clear_padding() {
  memset(this->address() + kSize, 0, kAlignedSize - kSize);
}

void SharedFunctionInfo::UpdateFunctionMapIndex() {
  int map_index = Context::FunctionMapIndex(
      language_mode(), kind(), true, HasSharedName(), needs_home_object());
  set_function_map_index(map_index);
}

BIT_FIELD_ACCESSORS(SharedFunctionInfo, debugger_hints,
                    name_should_print_as_anonymous,
                    SharedFunctionInfo::NameShouldPrintAsAnonymousBit)
BIT_FIELD_ACCESSORS(SharedFunctionInfo, debugger_hints, is_anonymous_expression,
                    SharedFunctionInfo::IsAnonymousExpressionBit)
BIT_FIELD_ACCESSORS(SharedFunctionInfo, debugger_hints, deserialized,
                    SharedFunctionInfo::IsDeserializedBit)
BIT_FIELD_ACCESSORS(SharedFunctionInfo, debugger_hints, has_no_side_effect,
                    SharedFunctionInfo::HasNoSideEffectBit)
BIT_FIELD_ACCESSORS(SharedFunctionInfo, debugger_hints,
                    computed_has_no_side_effect,
                    SharedFunctionInfo::ComputedHasNoSideEffectBit)
BIT_FIELD_ACCESSORS(SharedFunctionInfo, debugger_hints, debug_is_blackboxed,
                    SharedFunctionInfo::DebugIsBlackboxedBit)
BIT_FIELD_ACCESSORS(SharedFunctionInfo, debugger_hints,
                    computed_debug_is_blackboxed,
                    SharedFunctionInfo::ComputedDebugIsBlackboxedBit)
BIT_FIELD_ACCESSORS(SharedFunctionInfo, debugger_hints,
                    has_reported_binary_coverage,
                    SharedFunctionInfo::HasReportedBinaryCoverageBit)
BIT_FIELD_ACCESSORS(SharedFunctionInfo, debugger_hints, debugging_id,
                    SharedFunctionInfo::DebuggingIdBits)

void SharedFunctionInfo::DontAdaptArguments() {
  // TODO(leszeks): Revise this DCHECK now that the code field is gone.
  DCHECK(!HasCodeObject());
  set_internal_formal_parameter_count(kDontAdaptArgumentsSentinel);
}

BIT_FIELD_ACCESSORS(SharedFunctionInfo, raw_start_position_and_type,
                    raw_start_position, SharedFunctionInfo::StartPositionBits)

int SharedFunctionInfo::StartPosition() const {
  ScopeInfo* info = scope_info();
  if (!info->HasPositionInfo()) {
    // TODO(cbruni): use preparsed_scope_data
    return raw_start_position();
  }
  return info->StartPosition();
}

int SharedFunctionInfo::EndPosition() const {
  ScopeInfo* info = scope_info();
  if (!info->HasPositionInfo()) {
    // TODO(cbruni): use preparsed_scope_data
    return raw_end_position();
  }
  return info->EndPosition();
}

Code* SharedFunctionInfo::GetCode() const {
  // ======
  // NOTE: This chain of checks MUST be kept in sync with the equivalent CSA
  // GetSharedFunctionInfoCode method in code-stub-assembler.cc, and the
  // architecture-specific GetSharedFunctionInfoCode methods in builtins-*.cc.
  // ======

  Isolate* isolate = GetIsolate();
  Object* data = function_data();
  if (data->IsSmi()) {
    // Holding a Smi means we are a builtin.
    DCHECK(HasBuiltinId());
    return isolate->builtins()->builtin(builtin_id());
  } else if (data->IsBytecodeArray()) {
    // Having a bytecode array means we are a compiled, interpreted function.
    DCHECK(HasBytecodeArray());
    return isolate->builtins()->builtin(Builtins::kInterpreterEntryTrampoline);
  } else if (data->IsFixedArray()) {
    // Having a fixed array means we are an asm.js/wasm function.
    DCHECK(HasAsmWasmData());
    return isolate->builtins()->builtin(Builtins::kInstantiateAsmJs);
  } else if (data->IsPreParsedScopeData()) {
    // Having pre-parsed scope data means we need to compile.
    DCHECK(HasPreParsedScopeData());
    return isolate->builtins()->builtin(Builtins::kCompileLazy);
  } else if (data->IsFunctionTemplateInfo()) {
    // Having a function template info means we are an API function.
    DCHECK(IsApiFunction());
    return isolate->builtins()->builtin(Builtins::kHandleApiCall);
  } else if (data->IsCode()) {
    // Having a code object means we should run it.
    DCHECK(HasCodeObject());
    return Code::cast(data);
  }
  UNREACHABLE();
}

bool SharedFunctionInfo::IsInterpreted() const { return HasBytecodeArray(); }

ScopeInfo* SharedFunctionInfo::scope_info() const {
  Object* maybe_scope_info = name_or_scope_info();
  if (maybe_scope_info->IsScopeInfo()) {
    return ScopeInfo::cast(maybe_scope_info);
  }
  return ScopeInfo::Empty(GetIsolate());
}

void SharedFunctionInfo::set_scope_info(ScopeInfo* scope_info,
                                        WriteBarrierMode mode) {
  // TODO(cbruni): this code is no longer necessary once we store the positon
  // only on the ScopeInfo.
  if (scope_info->HasPositionInfo()) {
    scope_info->SetPositionInfo(raw_start_position(), raw_end_position());
  }
  // Move the existing name onto the ScopeInfo.
  Object* name = name_or_scope_info();
  if (name->IsScopeInfo()) {
    name = ScopeInfo::cast(name)->FunctionName();
  }
  DCHECK(name->IsString() || name == kNoSharedNameSentinel);
  // Only set the function name for function scopes.
  scope_info->SetFunctionName(name);
  WRITE_FIELD(this, kNameOrScopeInfoOffset,
              reinterpret_cast<Object*>(scope_info));
  CONDITIONAL_WRITE_BARRIER(GetHeap(), this, kNameOrScopeInfoOffset,
                            reinterpret_cast<Object*>(scope_info), mode);
}

ACCESSORS(SharedFunctionInfo, outer_scope_info, HeapObject,
          kOuterScopeInfoOffset)

bool SharedFunctionInfo::is_compiled() const {
  Object* data = function_data();
  return data != Smi::FromEnum(Builtins::kCompileLazy) &&
         !data->IsPreParsedScopeData();
}

int SharedFunctionInfo::GetLength() const {
  DCHECK(is_compiled());
  DCHECK(HasLength());
  return length();
}

bool SharedFunctionInfo::HasLength() const {
  DCHECK_IMPLIES(length() < 0, length() == kInvalidLength);
  return length() != kInvalidLength;
}

bool SharedFunctionInfo::has_simple_parameters() {
  return scope_info()->HasSimpleParameters();
}

bool SharedFunctionInfo::HasDebugInfo() const {
  bool has_debug_info = !debug_info()->IsSmi();
  DCHECK_EQ(debug_info()->IsStruct(), has_debug_info);
  return has_debug_info;
}

bool SharedFunctionInfo::IsApiFunction() const {
  return function_data()->IsFunctionTemplateInfo();
}

FunctionTemplateInfo* SharedFunctionInfo::get_api_func_data() {
  DCHECK(IsApiFunction());
  return FunctionTemplateInfo::cast(function_data());
}

bool SharedFunctionInfo::HasBytecodeArray() const {
  return function_data()->IsBytecodeArray();
}

BytecodeArray* SharedFunctionInfo::bytecode_array() const {
  DCHECK(HasBytecodeArray());
  return BytecodeArray::cast(function_data());
}

void SharedFunctionInfo::set_bytecode_array(BytecodeArray* bytecode) {
  DCHECK(function_data() == Smi::FromEnum(Builtins::kCompileLazy));
  set_function_data(bytecode);
}

void SharedFunctionInfo::ClearBytecodeArray() {
  DCHECK(function_data() == Smi::FromEnum(Builtins::kCompileLazy) ||
         HasBytecodeArray());
  set_builtin_id(Builtins::kCompileLazy);
}

bool SharedFunctionInfo::HasAsmWasmData() const {
  return function_data()->IsFixedArray();
}

FixedArray* SharedFunctionInfo::asm_wasm_data() const {
  DCHECK(HasAsmWasmData());
  return FixedArray::cast(function_data());
}

void SharedFunctionInfo::set_asm_wasm_data(FixedArray* data) {
  DCHECK(function_data() == Smi::FromEnum(Builtins::kCompileLazy) ||
         HasAsmWasmData());
  set_function_data(data);
}

void SharedFunctionInfo::ClearAsmWasmData() {
  DCHECK(HasAsmWasmData());
  set_builtin_id(Builtins::kCompileLazy);
}

bool SharedFunctionInfo::HasBuiltinId() const {
  return function_data()->IsSmi();
}

int SharedFunctionInfo::builtin_id() const {
  DCHECK(HasBuiltinId());
  int id = Smi::ToInt(function_data());
  DCHECK(Builtins::IsBuiltinId(id));
  return id;
}

void SharedFunctionInfo::set_builtin_id(int builtin_id) {
  DCHECK(Builtins::IsBuiltinId(builtin_id));
  DCHECK_NE(builtin_id, Builtins::kDeserializeLazy);
  set_function_data(Smi::FromInt(builtin_id), SKIP_WRITE_BARRIER);
}

bool SharedFunctionInfo::HasPreParsedScopeData() const {
  return function_data()->IsPreParsedScopeData();
}

PreParsedScopeData* SharedFunctionInfo::preparsed_scope_data() const {
  DCHECK(HasPreParsedScopeData());
  return PreParsedScopeData::cast(function_data());
}

void SharedFunctionInfo::set_preparsed_scope_data(
    PreParsedScopeData* preparsed_scope_data) {
  DCHECK(function_data() == Smi::FromEnum(Builtins::kCompileLazy));
  set_function_data(preparsed_scope_data);
}

void SharedFunctionInfo::ClearPreParsedScopeData() {
  DCHECK(function_data() == Smi::FromEnum(Builtins::kCompileLazy) ||
         HasPreParsedScopeData());
  set_builtin_id(Builtins::kCompileLazy);
}

bool SharedFunctionInfo::HasCodeObject() const {
  return function_data()->IsCode();
}

bool SharedFunctionInfo::HasBuiltinFunctionId() {
  return function_identifier()->IsSmi();
}

BuiltinFunctionId SharedFunctionInfo::builtin_function_id() {
  DCHECK(HasBuiltinFunctionId());
  return static_cast<BuiltinFunctionId>(Smi::ToInt(function_identifier()));
}

void SharedFunctionInfo::set_builtin_function_id(BuiltinFunctionId id) {
  set_function_identifier(Smi::FromInt(id));
}

bool SharedFunctionInfo::HasInferredName() {
  return function_identifier()->IsString();
}

String* SharedFunctionInfo::inferred_name() {
  if (HasInferredName()) {
    return String::cast(function_identifier());
  }
  DCHECK(function_identifier()->IsUndefined(GetIsolate()) ||
         HasBuiltinFunctionId());
  return GetHeap()->empty_string();
}

void SharedFunctionInfo::set_inferred_name(String* inferred_name) {
  DCHECK(function_identifier()->IsUndefined(GetIsolate()) || HasInferredName());
  set_function_identifier(inferred_name);
}

bool SharedFunctionInfo::IsUserJavaScript() {
  Object* script_obj = script();
  if (script_obj->IsUndefined(GetIsolate())) return false;
  Script* script = Script::cast(script_obj);
  return script->IsUserJavaScript();
}

bool SharedFunctionInfo::IsSubjectToDebugging() {
  return IsUserJavaScript() && !HasAsmWasmData();
}

}  // namespace internal
}  // namespace v8

#include "src/objects/object-macros-undef.h"

#endif  // V8_OBJECTS_SHARED_FUNCTION_INFO_INL_H_

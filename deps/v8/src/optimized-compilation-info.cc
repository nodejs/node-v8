// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/optimized-compilation-info.h"

#include "src/api.h"
#include "src/ast/ast.h"
#include "src/ast/scopes.h"
#include "src/debug/debug.h"
#include "src/isolate.h"
#include "src/objects-inl.h"
#include "src/parsing/parse-info.h"
#include "src/source-position.h"

namespace v8 {
namespace internal {

OptimizedCompilationInfo::OptimizedCompilationInfo(
    Zone* zone, Isolate* isolate, Handle<SharedFunctionInfo> shared,
    Handle<JSFunction> closure)
    : OptimizedCompilationInfo({}, AbstractCode::OPTIMIZED_FUNCTION, zone) {
  shared_info_ = shared;
  closure_ = closure;
  optimization_id_ = isolate->NextOptimizationId();
  dependencies_.reset(new CompilationDependencies(isolate, zone));

  SetFlag(kCalledWithCodeStartRegister);
  if (FLAG_function_context_specialization) MarkAsFunctionContextSpecializing();
  if (FLAG_turbo_splitting) MarkAsSplittingEnabled();
  if (!FLAG_turbo_disable_switch_jump_table) SetFlag(kSwitchJumpTableEnabled);
  if (FLAG_untrusted_code_mitigations) MarkAsPoisoningRegisterArguments();

  // TODO(yangguo): Disable this in case of debugging for crbug.com/826613
  if (FLAG_analyze_environment_liveness) {
    MarkAsAnalyzeEnvironmentLiveness();
  }

  // Collect source positions for optimized code when profiling or if debugger
  // is active, to be able to get more precise source positions at the price of
  // more memory consumption.
  if (isolate->NeedsSourcePositionsForProfiling()) {
    MarkAsSourcePositionsEnabled();
  }
}

OptimizedCompilationInfo::OptimizedCompilationInfo(
    Vector<const char> debug_name, Zone* zone, Code::Kind code_kind)
    : OptimizedCompilationInfo(
          debug_name, static_cast<AbstractCode::Kind>(code_kind), zone) {
  if (code_kind == Code::BYTECODE_HANDLER) {
    SetFlag(OptimizedCompilationInfo::kCalledWithCodeStartRegister);
  }
#if ENABLE_GDB_JIT_INTERFACE
#if DEBUG
  if (code_kind == Code::BUILTIN || code_kind == Code::STUB) {
    MarkAsSourcePositionsEnabled();
  }
#endif
#endif
}

OptimizedCompilationInfo::OptimizedCompilationInfo(
    Vector<const char> debug_name, AbstractCode::Kind code_kind, Zone* zone)
    : flags_(FLAG_untrusted_code_mitigations ? kUntrustedCodeMitigations : 0),
      code_kind_(code_kind),
      stub_key_(0),
      builtin_index_(Builtins::kNoBuiltinId),
      osr_offset_(BailoutId::None()),
      zone_(zone),
      deferred_handles_(nullptr),
      dependencies_(nullptr),
      bailout_reason_(BailoutReason::kNoReason),
      optimization_id_(-1),
      debug_name_(debug_name) {}

OptimizedCompilationInfo::~OptimizedCompilationInfo() {
  if (GetFlag(kDisableFutureOptimization) && has_shared_info()) {
    shared_info()->DisableOptimization(bailout_reason());
  }
  if (dependencies()) {
    dependencies()->Rollback();
  }
}

void OptimizedCompilationInfo::set_deferred_handles(
    std::shared_ptr<DeferredHandles> deferred_handles) {
  DCHECK_NULL(deferred_handles_);
  deferred_handles_.swap(deferred_handles);
}

void OptimizedCompilationInfo::set_deferred_handles(
    DeferredHandles* deferred_handles) {
  DCHECK_NULL(deferred_handles_);
  deferred_handles_.reset(deferred_handles);
}

void OptimizedCompilationInfo::ReopenHandlesInNewHandleScope() {
  if (!shared_info_.is_null()) {
    shared_info_ = Handle<SharedFunctionInfo>(*shared_info_);
  }
  if (!closure_.is_null()) {
    closure_ = Handle<JSFunction>(*closure_);
  }
}

std::unique_ptr<char[]> OptimizedCompilationInfo::GetDebugName() const {
  if (!shared_info().is_null()) {
    return shared_info()->DebugName()->ToCString();
  }
  Vector<const char> name_vec = debug_name_;
  if (name_vec.is_empty()) name_vec = ArrayVector("unknown");
  std::unique_ptr<char[]> name(new char[name_vec.length() + 1]);
  memcpy(name.get(), name_vec.start(), name_vec.length());
  name[name_vec.length()] = '\0';
  return name;
}

StackFrame::Type OptimizedCompilationInfo::GetOutputStackFrameType() const {
  switch (code_kind()) {
    case Code::STUB:
    case Code::BYTECODE_HANDLER:
    case Code::BUILTIN:
      return StackFrame::STUB;
    case Code::WASM_FUNCTION:
      return StackFrame::WASM_COMPILED;
    case Code::JS_TO_WASM_FUNCTION:
      return StackFrame::JS_TO_WASM;
    case Code::WASM_TO_JS_FUNCTION:
      return StackFrame::WASM_TO_JS;
    case Code::WASM_INTERPRETER_ENTRY:
      return StackFrame::WASM_INTERPRETER_ENTRY;
    default:
      UNIMPLEMENTED();
      return StackFrame::NONE;
  }
}

bool OptimizedCompilationInfo::has_context() const {
  return !closure().is_null();
}

Context* OptimizedCompilationInfo::context() const {
  return has_context() ? closure()->context() : nullptr;
}

bool OptimizedCompilationInfo::has_native_context() const {
  return !closure().is_null() && (closure()->native_context() != nullptr);
}

Context* OptimizedCompilationInfo::native_context() const {
  return has_native_context() ? closure()->native_context() : nullptr;
}

bool OptimizedCompilationInfo::has_global_object() const {
  return has_native_context();
}

JSGlobalObject* OptimizedCompilationInfo::global_object() const {
  return has_global_object() ? native_context()->global_object() : nullptr;
}

int OptimizedCompilationInfo::AddInlinedFunction(
    Handle<SharedFunctionInfo> inlined_function, SourcePosition pos) {
  int id = static_cast<int>(inlined_functions_.size());
  inlined_functions_.push_back(InlinedFunctionHolder(inlined_function, pos));
  return id;
}

}  // namespace internal
}  // namespace v8

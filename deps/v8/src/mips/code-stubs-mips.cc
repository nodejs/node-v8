// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if V8_TARGET_ARCH_MIPS

#include "src/api-arguments.h"
#include "src/base/bits.h"
#include "src/bootstrapper.h"
#include "src/code-stubs.h"
#include "src/frame-constants.h"
#include "src/frames.h"
#include "src/heap/heap-inl.h"
#include "src/ic/ic.h"
#include "src/ic/stub-cache.h"
#include "src/isolate.h"
#include "src/objects/api-callbacks.h"
#include "src/regexp/jsregexp.h"
#include "src/regexp/regexp-macro-assembler.h"
#include "src/runtime/runtime.h"

#include "src/mips/code-stubs-mips.h"  // Cannot be the first include.

namespace v8 {
namespace internal {

#define __ ACCESS_MASM(masm)

void ArrayNArgumentsConstructorStub::Generate(MacroAssembler* masm) {
  __ sll(t9, a0, kPointerSizeLog2);
  __ Addu(t9, sp, t9);
  __ sw(a1, MemOperand(t9, 0));
  __ Push(a1);
  __ Push(a2);
  __ Addu(a0, a0, Operand(3));
  __ TailCallRuntime(Runtime::kNewArray);
}

void CodeStub::GenerateStubsAheadOfTime(Isolate* isolate) {
  CommonArrayConstructorStub::GenerateStubsAheadOfTime(isolate);
  StoreFastElementStub::GenerateAheadOfTime(isolate);
}

void JSEntryStub::Generate(MacroAssembler* masm) {
  Label invoke, handler_entry, exit;
  Isolate* isolate = masm->isolate();

  {
    NoRootArrayScope no_root_array(masm);

    // Registers:
    // a0: entry address
    // a1: function
    // a2: receiver
    // a3: argc
    //
    // Stack:
    // 4 args slots
    // args

    ProfileEntryHookStub::MaybeCallEntryHook(masm);

    // Save callee saved registers on the stack.
    __ MultiPush(kCalleeSaved | ra.bit());

    // Save callee-saved FPU registers.
    __ MultiPushFPU(kCalleeSavedFPU);
    // Set up the reserved register for 0.0.
    __ Move(kDoubleRegZero, 0.0);

    __ InitializeRootRegister();
  }

  // Load argv in s0 register.
  int offset_to_argv = (kNumCalleeSaved + 1) * kPointerSize;
  offset_to_argv += kNumCalleeSavedFPU * kDoubleSize;

  __ lw(s0, MemOperand(sp, offset_to_argv + kCArgsSlotsSize));

  // We build an EntryFrame.
  __ li(t3, Operand(-1));  // Push a bad frame pointer to fail if it is used.
  StackFrame::Type marker = type();
  __ li(t2, Operand(StackFrame::TypeToMarker(marker)));
  __ li(t1, Operand(StackFrame::TypeToMarker(marker)));
  __ li(t0,
        ExternalReference::Create(IsolateAddressId::kCEntryFPAddress, isolate));
  __ lw(t0, MemOperand(t0));
  __ Push(t3, t2, t1, t0);
  // Set up frame pointer for the frame to be pushed.
  __ addiu(fp, sp, -EntryFrameConstants::kCallerFPOffset);

  // Registers:
  // a0: entry_address
  // a1: function
  // a2: receiver_pointer
  // a3: argc
  // s0: argv
  //
  // Stack:
  // caller fp          |
  // function slot      | entry frame
  // context slot       |
  // bad fp (0xFF...F)  |
  // callee saved registers + ra
  // 4 args slots
  // args

  // If this is the outermost JS call, set js_entry_sp value.
  Label non_outermost_js;
  ExternalReference js_entry_sp =
      ExternalReference::Create(IsolateAddressId::kJSEntrySPAddress, isolate);
  __ li(t1, js_entry_sp);
  __ lw(t2, MemOperand(t1));
  __ Branch(&non_outermost_js, ne, t2, Operand(zero_reg));
  __ sw(fp, MemOperand(t1));
  __ li(t0, Operand(StackFrame::OUTERMOST_JSENTRY_FRAME));
  Label cont;
  __ b(&cont);
  __ nop();   // Branch delay slot nop.
  __ bind(&non_outermost_js);
  __ li(t0, Operand(StackFrame::INNER_JSENTRY_FRAME));
  __ bind(&cont);
  __ push(t0);

  // Jump to a faked try block that does the invoke, with a faked catch
  // block that sets the pending exception.
  __ jmp(&invoke);
  __ bind(&handler_entry);
  handler_offset_ = handler_entry.pos();
  // Caught exception: Store result (exception) in the pending exception
  // field in the JSEnv and return a failure sentinel.  Coming in here the
  // fp will be invalid because the PushStackHandler below sets it to 0 to
  // signal the existence of the JSEntry frame.
  __ li(t0, ExternalReference::Create(
                IsolateAddressId::kPendingExceptionAddress, isolate));
  __ sw(v0, MemOperand(t0));  // We come back from 'invoke'. result is in v0.
  __ LoadRoot(v0, Heap::kExceptionRootIndex);
  __ b(&exit);  // b exposes branch delay slot.
  __ nop();   // Branch delay slot nop.

  // Invoke: Link this frame into the handler chain.
  __ bind(&invoke);
  __ PushStackHandler();
  // If an exception not caught by another handler occurs, this handler
  // returns control to the code after the bal(&invoke) above, which
  // restores all kCalleeSaved registers (including cp and fp) to their
  // saved values before returning a failure to C.

  // Invoke the function by calling through JS entry trampoline builtin.
  // Notice that we cannot store a reference to the trampoline code directly in
  // this stub, because runtime stubs are not traversed when doing GC.

  // Registers:
  // a0: entry_address
  // a1: function
  // a2: receiver_pointer
  // a3: argc
  // s0: argv
  //
  // Stack:
  // handler frame
  // entry frame
  // callee saved registers + ra
  // 4 args slots
  // args
  __ Call(EntryTrampoline(), RelocInfo::CODE_TARGET);

  // Unlink this frame from the handler chain.
  __ PopStackHandler();

  __ bind(&exit);  // v0 holds result
  // Check if the current stack frame is marked as the outermost JS frame.
  Label non_outermost_js_2;
  __ pop(t1);
  __ Branch(&non_outermost_js_2, ne, t1,
            Operand(StackFrame::OUTERMOST_JSENTRY_FRAME));
  __ li(t1, ExternalReference(js_entry_sp));
  __ sw(zero_reg, MemOperand(t1));
  __ bind(&non_outermost_js_2);

  // Restore the top frame descriptors from the stack.
  __ pop(t1);
  __ li(t0,
        ExternalReference::Create(IsolateAddressId::kCEntryFPAddress, isolate));
  __ sw(t1, MemOperand(t0));

  // Reset the stack to the callee saved registers.
  __ addiu(sp, sp, -EntryFrameConstants::kCallerFPOffset);

  // Restore callee-saved fpu registers.
  __ MultiPopFPU(kCalleeSavedFPU);

  // Restore callee saved registers from the stack.
  __ MultiPop(kCalleeSaved | ra.bit());
  // Return.
  __ Jump(ra);
}

void DirectCEntryStub::Generate(MacroAssembler* masm) {
  // Make place for arguments to fit C calling convention. Most of the callers
  // of DirectCEntryStub::GenerateCall are using EnterExitFrame/LeaveExitFrame
  // so they handle stack restoring and we don't have to do that here.
  // Any caller of DirectCEntryStub::GenerateCall must take care of dropping
  // kCArgsSlotsSize stack space after the call.
  __ Subu(sp, sp, Operand(kCArgsSlotsSize));
  // Place the return address on the stack, making the call
  // GC safe. The RegExp backend also relies on this.
  __ sw(ra, MemOperand(sp, kCArgsSlotsSize));
  __ Call(t9);  // Call the C++ function.
  __ lw(t9, MemOperand(sp, kCArgsSlotsSize));

  if (FLAG_debug_code && FLAG_enable_slow_asserts) {
    // In case of an error the return address may point to a memory area
    // filled with kZapValue by the GC.
    // Dereference the address and check for this.
    __ lw(t0, MemOperand(t9));
    __ Assert(ne, AbortReason::kReceivedInvalidReturnAddress, t0,
              Operand(reinterpret_cast<uint32_t>(kZapValue)));
  }
  __ Jump(t9);
}


void DirectCEntryStub::GenerateCall(MacroAssembler* masm,
                                    Register target) {
  intptr_t loc =
      reinterpret_cast<intptr_t>(GetCode().location());
  __ Move(t9, target);
  __ li(kScratchReg, Operand(loc, RelocInfo::CODE_TARGET), CONSTANT_SIZE);
  __ Call(kScratchReg);
}


void ProfileEntryHookStub::MaybeCallEntryHookDelayed(TurboAssembler* tasm,
                                                     Zone* zone) {
  if (tasm->isolate()->function_entry_hook() != nullptr) {
    tasm->push(ra);
    tasm->CallStubDelayed(new (zone) ProfileEntryHookStub(nullptr));
    tasm->pop(ra);
  }
}

void ProfileEntryHookStub::MaybeCallEntryHook(MacroAssembler* masm) {
  if (masm->isolate()->function_entry_hook() != nullptr) {
    ProfileEntryHookStub stub(masm->isolate());
    __ push(ra);
    __ CallStub(&stub);
    __ pop(ra);
  }
}


void ProfileEntryHookStub::Generate(MacroAssembler* masm) {
  // The entry hook is a "push ra" instruction, followed by a call.
  // Note: on MIPS "push" is 2 instruction
  const int32_t kReturnAddressDistanceFromFunctionStart =
      Assembler::kCallTargetAddressOffset + (2 * Assembler::kInstrSize);

  // This should contain all kJSCallerSaved registers.
  const RegList kSavedRegs =
     kJSCallerSaved |  // Caller saved registers.
     s5.bit();         // Saved stack pointer.

  // We also save ra, so the count here is one higher than the mask indicates.
  const int32_t kNumSavedRegs = kNumJSCallerSaved + 2;

  // Save all caller-save registers as this may be called from anywhere.
  __ MultiPush(kSavedRegs | ra.bit());

  // Compute the function's address for the first argument.
  __ Subu(a0, ra, Operand(kReturnAddressDistanceFromFunctionStart));

  // The caller's return address is above the saved temporaries.
  // Grab that for the second argument to the hook.
  __ Addu(a1, sp, Operand(kNumSavedRegs * kPointerSize));

  // Align the stack if necessary.
  int frame_alignment = masm->ActivationFrameAlignment();
  if (frame_alignment > kPointerSize) {
    __ mov(s5, sp);
    DCHECK(base::bits::IsPowerOfTwo(frame_alignment));
    __ And(sp, sp, Operand(-frame_alignment));
  }
  __ Subu(sp, sp, kCArgsSlotsSize);
#if defined(V8_HOST_ARCH_MIPS)
  int32_t entry_hook =
      reinterpret_cast<int32_t>(isolate()->function_entry_hook());
  __ li(t9, Operand(entry_hook));
#else
  // Under the simulator we need to indirect the entry hook through a
  // trampoline function at a known address.
  // It additionally takes an isolate as a third parameter.
  __ li(a2, ExternalReference::isolate_address(isolate()));

  ApiFunction dispatcher(FUNCTION_ADDR(EntryHookTrampoline));
  __ li(t9, ExternalReference::Create(&dispatcher,
                                      ExternalReference::BUILTIN_CALL));
#endif
  // Call C function through t9 to conform ABI for PIC.
  __ Call(t9);

  // Restore the stack pointer if needed.
  if (frame_alignment > kPointerSize) {
    __ mov(sp, s5);
  } else {
    __ Addu(sp, sp, kCArgsSlotsSize);
  }

  // Also pop ra to get Ret(0).
  __ MultiPop(kSavedRegs | ra.bit());
  __ Ret();
}


template<class T>
static void CreateArrayDispatch(MacroAssembler* masm,
                                AllocationSiteOverrideMode mode) {
  if (mode == DISABLE_ALLOCATION_SITES) {
    T stub(masm->isolate(), GetInitialFastElementsKind(), mode);
    __ TailCallStub(&stub);
  } else if (mode == DONT_OVERRIDE) {
    int last_index =
        GetSequenceIndexFromFastElementsKind(TERMINAL_FAST_ELEMENTS_KIND);
    for (int i = 0; i <= last_index; ++i) {
      ElementsKind kind = GetFastElementsKindFromSequenceIndex(i);
      T stub(masm->isolate(), kind);
      __ TailCallStub(&stub, eq, a3, Operand(kind));
    }

    // If we reached this point there is a problem.
    __ Abort(AbortReason::kUnexpectedElementsKindInArrayConstructor);
  } else {
    UNREACHABLE();
  }
}


static void CreateArrayDispatchOneArgument(MacroAssembler* masm,
                                           AllocationSiteOverrideMode mode) {
  // a2 - allocation site (if mode != DISABLE_ALLOCATION_SITES)
  // a3 - kind (if mode != DISABLE_ALLOCATION_SITES)
  // a0 - number of arguments
  // a1 - constructor?
  // sp[0] - last argument
    STATIC_ASSERT(PACKED_SMI_ELEMENTS == 0);
    STATIC_ASSERT(HOLEY_SMI_ELEMENTS == 1);
    STATIC_ASSERT(PACKED_ELEMENTS == 2);
    STATIC_ASSERT(HOLEY_ELEMENTS == 3);
    STATIC_ASSERT(PACKED_DOUBLE_ELEMENTS == 4);
    STATIC_ASSERT(HOLEY_DOUBLE_ELEMENTS == 5);

  if (mode == DISABLE_ALLOCATION_SITES) {
    ElementsKind initial = GetInitialFastElementsKind();
    ElementsKind holey_initial = GetHoleyElementsKind(initial);

    ArraySingleArgumentConstructorStub stub_holey(masm->isolate(),
                                                  holey_initial,
                                                  DISABLE_ALLOCATION_SITES);
    __ TailCallStub(&stub_holey);
  } else if (mode == DONT_OVERRIDE) {
    // is the low bit set? If so, we are holey and that is good.
    Label normal_sequence;
    __ And(kScratchReg, a3, Operand(1));
    __ Branch(&normal_sequence, ne, kScratchReg, Operand(zero_reg));

    // We are going to create a holey array, but our kind is non-holey.
    // Fix kind and retry (only if we have an allocation site in the slot).
    __ Addu(a3, a3, Operand(1));

    if (FLAG_debug_code) {
      __ lw(t1, FieldMemOperand(a2, 0));
      __ LoadRoot(kScratchReg, Heap::kAllocationSiteMapRootIndex);
      __ Assert(eq, AbortReason::kExpectedAllocationSite, t1,
                Operand(kScratchReg));
    }

    // Save the resulting elements kind in type info. We can't just store a3
    // in the AllocationSite::transition_info field because elements kind is
    // restricted to a portion of the field...upper bits need to be left alone.
    STATIC_ASSERT(AllocationSite::ElementsKindBits::kShift == 0);
    __ lw(t0, FieldMemOperand(
                  a2, AllocationSite::kTransitionInfoOrBoilerplateOffset));
    __ Addu(t0, t0, Operand(Smi::FromInt(kFastElementsKindPackedToHoley)));
    __ sw(t0, FieldMemOperand(
                  a2, AllocationSite::kTransitionInfoOrBoilerplateOffset));

    __ bind(&normal_sequence);
    int last_index =
        GetSequenceIndexFromFastElementsKind(TERMINAL_FAST_ELEMENTS_KIND);
    for (int i = 0; i <= last_index; ++i) {
      ElementsKind kind = GetFastElementsKindFromSequenceIndex(i);
      ArraySingleArgumentConstructorStub stub(masm->isolate(), kind);
      __ TailCallStub(&stub, eq, a3, Operand(kind));
    }

    // If we reached this point there is a problem.
    __ Abort(AbortReason::kUnexpectedElementsKindInArrayConstructor);
  } else {
    UNREACHABLE();
  }
}


template<class T>
static void ArrayConstructorStubAheadOfTimeHelper(Isolate* isolate) {
  int to_index =
      GetSequenceIndexFromFastElementsKind(TERMINAL_FAST_ELEMENTS_KIND);
  for (int i = 0; i <= to_index; ++i) {
    ElementsKind kind = GetFastElementsKindFromSequenceIndex(i);
    T stub(isolate, kind);
    stub.GetCode();
    if (AllocationSite::ShouldTrack(kind)) {
      T stub1(isolate, kind, DISABLE_ALLOCATION_SITES);
      stub1.GetCode();
    }
  }
}

void CommonArrayConstructorStub::GenerateStubsAheadOfTime(Isolate* isolate) {
  ArrayConstructorStubAheadOfTimeHelper<ArrayNoArgumentConstructorStub>(
      isolate);
  ArrayConstructorStubAheadOfTimeHelper<ArraySingleArgumentConstructorStub>(
      isolate);
  ArrayNArgumentsConstructorStub stub(isolate);
  stub.GetCode();
  ElementsKind kinds[2] = {PACKED_ELEMENTS, HOLEY_ELEMENTS};
  for (int i = 0; i < 2; i++) {
    // For internal arrays we only need a few things.
    InternalArrayNoArgumentConstructorStub stubh1(isolate, kinds[i]);
    stubh1.GetCode();
    InternalArraySingleArgumentConstructorStub stubh2(isolate, kinds[i]);
    stubh2.GetCode();
  }
}


void ArrayConstructorStub::GenerateDispatchToArrayStub(
    MacroAssembler* masm,
    AllocationSiteOverrideMode mode) {
  Label not_zero_case, not_one_case;
  __ And(kScratchReg, a0, a0);
  __ Branch(&not_zero_case, ne, kScratchReg, Operand(zero_reg));
  CreateArrayDispatch<ArrayNoArgumentConstructorStub>(masm, mode);

  __ bind(&not_zero_case);
  __ Branch(&not_one_case, gt, a0, Operand(1));
  CreateArrayDispatchOneArgument(masm, mode);

  __ bind(&not_one_case);
  ArrayNArgumentsConstructorStub stub(masm->isolate());
  __ TailCallStub(&stub);
}


void ArrayConstructorStub::Generate(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- a0 : argc (only if argument_count() is ANY or MORE_THAN_ONE)
  //  -- a1 : constructor
  //  -- a2 : AllocationSite or undefined
  //  -- a3 : Original constructor
  //  -- sp[0] : last argument
  // -----------------------------------

  if (FLAG_debug_code) {
    // The array construct code is only set for the global and natives
    // builtin Array functions which always have maps.

    // Initial map for the builtin Array function should be a map.
    __ lw(t0, FieldMemOperand(a1, JSFunction::kPrototypeOrInitialMapOffset));
    // Will both indicate a nullptr and a Smi.
    __ SmiTst(t0, kScratchReg);
    __ Assert(ne, AbortReason::kUnexpectedInitialMapForArrayFunction,
              kScratchReg, Operand(zero_reg));
    __ GetObjectType(t0, t0, t1);
    __ Assert(eq, AbortReason::kUnexpectedInitialMapForArrayFunction, t1,
              Operand(MAP_TYPE));

    // We should either have undefined in a2 or a valid AllocationSite
    __ AssertUndefinedOrAllocationSite(a2, t0);
  }

  // Enter the context of the Array function.
  __ lw(cp, FieldMemOperand(a1, JSFunction::kContextOffset));

  Label subclassing;
  __ Branch(&subclassing, ne, a1, Operand(a3));

  Label no_info;
  // Get the elements kind and case on that.
  __ LoadRoot(kScratchReg, Heap::kUndefinedValueRootIndex);
  __ Branch(&no_info, eq, a2, Operand(kScratchReg));

  __ lw(a3, FieldMemOperand(
                a2, AllocationSite::kTransitionInfoOrBoilerplateOffset));
  __ SmiUntag(a3);
  STATIC_ASSERT(AllocationSite::ElementsKindBits::kShift == 0);
  __ And(a3, a3, Operand(AllocationSite::ElementsKindBits::kMask));
  GenerateDispatchToArrayStub(masm, DONT_OVERRIDE);

  __ bind(&no_info);
  GenerateDispatchToArrayStub(masm, DISABLE_ALLOCATION_SITES);

  // Subclassing.
  __ bind(&subclassing);
  __ Lsa(kScratchReg, sp, a0, kPointerSizeLog2);
  __ sw(a1, MemOperand(kScratchReg));
  __ li(kScratchReg, Operand(3));
  __ addu(a0, a0, kScratchReg);
  __ Push(a3, a2);
  __ JumpToExternalReference(ExternalReference::Create(Runtime::kNewArray));
}


void InternalArrayConstructorStub::GenerateCase(
    MacroAssembler* masm, ElementsKind kind) {

  InternalArrayNoArgumentConstructorStub stub0(isolate(), kind);
  __ TailCallStub(&stub0, lo, a0, Operand(1));

  ArrayNArgumentsConstructorStub stubN(isolate());
  __ TailCallStub(&stubN, hi, a0, Operand(1));

  if (IsFastPackedElementsKind(kind)) {
    // We might need to create a holey array
    // look at the first argument.
    __ lw(kScratchReg, MemOperand(sp, 0));

    InternalArraySingleArgumentConstructorStub
        stub1_holey(isolate(), GetHoleyElementsKind(kind));
    __ TailCallStub(&stub1_holey, ne, kScratchReg, Operand(zero_reg));
  }

  InternalArraySingleArgumentConstructorStub stub1(isolate(), kind);
  __ TailCallStub(&stub1);
}


void InternalArrayConstructorStub::Generate(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- a0 : argc
  //  -- a1 : constructor
  //  -- sp[0] : return address
  //  -- sp[4] : last argument
  // -----------------------------------

  if (FLAG_debug_code) {
    // The array construct code is only set for the global and natives
    // builtin Array functions which always have maps.

    // Initial map for the builtin Array function should be a map.
    __ lw(a3, FieldMemOperand(a1, JSFunction::kPrototypeOrInitialMapOffset));
    // Will both indicate a nullptr and a Smi.
    __ SmiTst(a3, kScratchReg);
    __ Assert(ne, AbortReason::kUnexpectedInitialMapForArrayFunction,
              kScratchReg, Operand(zero_reg));
    __ GetObjectType(a3, a3, t0);
    __ Assert(eq, AbortReason::kUnexpectedInitialMapForArrayFunction, t0,
              Operand(MAP_TYPE));
  }

  // Figure out the right elements kind.
  __ lw(a3, FieldMemOperand(a1, JSFunction::kPrototypeOrInitialMapOffset));

  // Load the map's "bit field 2" into a3. We only need the first byte,
  // but the following bit field extraction takes care of that anyway.
  __ lbu(a3, FieldMemOperand(a3, Map::kBitField2Offset));
  // Retrieve elements_kind from bit field 2.
  __ DecodeField<Map::ElementsKindBits>(a3);

  if (FLAG_debug_code) {
    Label done;
    __ Branch(&done, eq, a3, Operand(PACKED_ELEMENTS));
    __ Assert(
        eq,
        AbortReason::kInvalidElementsKindForInternalArrayOrInternalPackedArray,
        a3, Operand(HOLEY_ELEMENTS));
    __ bind(&done);
  }

  Label fast_elements_case;
  __ Branch(&fast_elements_case, eq, a3, Operand(PACKED_ELEMENTS));
  GenerateCase(masm, HOLEY_ELEMENTS);

  __ bind(&fast_elements_case);
  GenerateCase(masm, PACKED_ELEMENTS);
}

static int AddressOffset(ExternalReference ref0, ExternalReference ref1) {
  return ref0.address() - ref1.address();
}


// Calls an API function.  Allocates HandleScope, extracts returned value
// from handle and propagates exceptions.  Restores context.  stack_space
// - space to be unwound on exit (includes the call JS arguments space and
// the additional space allocated for the fast call).
static void CallApiFunctionAndReturn(MacroAssembler* masm,
                                     Register function_address,
                                     ExternalReference thunk_ref,
                                     int stack_space,
                                     int32_t stack_space_offset,
                                     MemOperand return_value_operand) {
  Isolate* isolate = masm->isolate();
  ExternalReference next_address =
      ExternalReference::handle_scope_next_address(isolate);
  const int kNextOffset = 0;
  const int kLimitOffset = AddressOffset(
      ExternalReference::handle_scope_limit_address(isolate), next_address);
  const int kLevelOffset = AddressOffset(
      ExternalReference::handle_scope_level_address(isolate), next_address);

  DCHECK(function_address == a1 || function_address == a2);

  Label profiler_disabled;
  Label end_profiler_check;
  __ li(t9, ExternalReference::is_profiling_address(isolate));
  __ lb(t9, MemOperand(t9, 0));
  __ Branch(&profiler_disabled, eq, t9, Operand(zero_reg));

  // Additional parameter is the address of the actual callback.
  __ li(t9, Operand(thunk_ref));
  __ jmp(&end_profiler_check);

  __ bind(&profiler_disabled);
  __ mov(t9, function_address);
  __ bind(&end_profiler_check);

  // Allocate HandleScope in callee-save registers.
  __ li(s5, Operand(next_address));
  __ lw(s0, MemOperand(s5, kNextOffset));
  __ lw(s1, MemOperand(s5, kLimitOffset));
  __ lw(s2, MemOperand(s5, kLevelOffset));
  __ Addu(s2, s2, Operand(1));
  __ sw(s2, MemOperand(s5, kLevelOffset));

  if (FLAG_log_timer_events) {
    FrameScope frame(masm, StackFrame::MANUAL);
    __ PushSafepointRegisters();
    __ PrepareCallCFunction(1, a0);
    __ li(a0, ExternalReference::isolate_address(isolate));
    __ CallCFunction(ExternalReference::log_enter_external_function(), 1);
    __ PopSafepointRegisters();
  }

  // Native call returns to the DirectCEntry stub which redirects to the
  // return address pushed on stack (could have moved after GC).
  // DirectCEntry stub itself is generated early and never moves.
  DirectCEntryStub stub(isolate);
  stub.GenerateCall(masm, t9);

  if (FLAG_log_timer_events) {
    FrameScope frame(masm, StackFrame::MANUAL);
    __ PushSafepointRegisters();
    __ PrepareCallCFunction(1, a0);
    __ li(a0, ExternalReference::isolate_address(isolate));
    __ CallCFunction(ExternalReference::log_leave_external_function(), 1);
    __ PopSafepointRegisters();
  }

  Label promote_scheduled_exception;
  Label delete_allocated_handles;
  Label leave_exit_frame;
  Label return_value_loaded;

  // Load value from ReturnValue.
  __ lw(v0, return_value_operand);
  __ bind(&return_value_loaded);

  // No more valid handles (the result handle was the last one). Restore
  // previous handle scope.
  __ sw(s0, MemOperand(s5, kNextOffset));
  if (__ emit_debug_code()) {
    __ lw(a1, MemOperand(s5, kLevelOffset));
    __ Check(eq, AbortReason::kUnexpectedLevelAfterReturnFromApiCall, a1,
             Operand(s2));
  }
  __ Subu(s2, s2, Operand(1));
  __ sw(s2, MemOperand(s5, kLevelOffset));
  __ lw(kScratchReg, MemOperand(s5, kLimitOffset));
  __ Branch(&delete_allocated_handles, ne, s1, Operand(kScratchReg));

  // Leave the API exit frame.
  __ bind(&leave_exit_frame);

  if (stack_space_offset != kInvalidStackOffset) {
    // ExitFrame contains four MIPS argument slots after DirectCEntryStub call
    // so this must be accounted for.
    __ lw(s0, MemOperand(sp, stack_space_offset + kCArgsSlotsSize));
  } else {
    __ li(s0, Operand(stack_space));
  }
  __ LeaveExitFrame(false, s0, NO_EMIT_RETURN,
                    stack_space_offset != kInvalidStackOffset);

  // Check if the function scheduled an exception.
  __ LoadRoot(t0, Heap::kTheHoleValueRootIndex);
  __ li(kScratchReg, ExternalReference::scheduled_exception_address(isolate));
  __ lw(t1, MemOperand(kScratchReg));
  __ Branch(&promote_scheduled_exception, ne, t0, Operand(t1));

  __ Ret();

  // Re-throw by promoting a scheduled exception.
  __ bind(&promote_scheduled_exception);
  __ TailCallRuntime(Runtime::kPromoteScheduledException);

  // HandleScope limit has changed. Delete allocated extensions.
  __ bind(&delete_allocated_handles);
  __ sw(s1, MemOperand(s5, kLimitOffset));
  __ mov(s0, v0);
  __ mov(a0, v0);
  __ PrepareCallCFunction(1, s1);
  __ li(a0, ExternalReference::isolate_address(isolate));
  __ CallCFunction(ExternalReference::delete_handle_scope_extensions(), 1);
  __ mov(v0, s0);
  __ jmp(&leave_exit_frame);
}

void CallApiCallbackStub::Generate(MacroAssembler* masm) {
  // ----------- S t a t e -------------
  //  -- t0                  : call_data
  //  -- a2                  : holder
  //  -- a1                  : api_function_address
  //  -- cp                  : context
  //  --
  //  -- sp[0]               : last argument
  //  -- ...
  //  -- sp[(argc - 1)* 4]   : first argument
  //  -- sp[argc * 4]        : receiver
  // -----------------------------------

  Register call_data = t0;
  Register holder = a2;
  Register api_function_address = a1;

  typedef FunctionCallbackArguments FCA;

  STATIC_ASSERT(FCA::kArgsLength == 6);
  STATIC_ASSERT(FCA::kNewTargetIndex == 5);
  STATIC_ASSERT(FCA::kDataIndex == 4);
  STATIC_ASSERT(FCA::kReturnValueOffset == 3);
  STATIC_ASSERT(FCA::kReturnValueDefaultValueIndex == 2);
  STATIC_ASSERT(FCA::kIsolateIndex == 1);
  STATIC_ASSERT(FCA::kHolderIndex == 0);

  // new target
  __ PushRoot(Heap::kUndefinedValueRootIndex);

  // call data.
  __ Push(call_data);

  Register scratch = call_data;
  __ LoadRoot(scratch, Heap::kUndefinedValueRootIndex);
  // Push return value and default return value.
  __ Push(scratch, scratch);
  __ li(scratch, ExternalReference::isolate_address(masm->isolate()));
  // Push isolate and holder.
  __ Push(scratch, holder);

  // Prepare arguments.
  __ mov(scratch, sp);

  // Allocate the v8::Arguments structure in the arguments' space since
  // it's not controlled by GC.
  const int kApiStackSpace = 3;

  FrameScope frame_scope(masm, StackFrame::MANUAL);
  __ EnterExitFrame(false, kApiStackSpace);

  DCHECK(api_function_address != a0 && scratch != a0);
  // a0 = FunctionCallbackInfo&
  // Arguments is after the return address.
  __ Addu(a0, sp, Operand(1 * kPointerSize));
  // FunctionCallbackInfo::implicit_args_
  __ sw(scratch, MemOperand(a0, 0 * kPointerSize));
  // FunctionCallbackInfo::values_
  __ Addu(kScratchReg, scratch,
          Operand((FCA::kArgsLength - 1 + argc()) * kPointerSize));
  __ sw(kScratchReg, MemOperand(a0, 1 * kPointerSize));
  // FunctionCallbackInfo::length_ = argc
  __ li(kScratchReg, Operand(argc()));
  __ sw(kScratchReg, MemOperand(a0, 2 * kPointerSize));

  ExternalReference thunk_ref = ExternalReference::invoke_function_callback();

  AllowExternalCallThatCantCauseGC scope(masm);
  // Stores return the first js argument.
  int return_value_offset = 2 + FCA::kReturnValueOffset;
  MemOperand return_value_operand(fp, return_value_offset * kPointerSize);
  const int stack_space = argc() + FCA::kArgsLength + 1;
  // TODO(adamk): Why are we clobbering this immediately?
  const int32_t stack_space_offset = kInvalidStackOffset;
  CallApiFunctionAndReturn(masm, api_function_address, thunk_ref, stack_space,
                           stack_space_offset, return_value_operand);
}


void CallApiGetterStub::Generate(MacroAssembler* masm) {
  // Build v8::PropertyCallbackInfo::args_ array on the stack and push property
  // name below the exit frame to make GC aware of them.
  STATIC_ASSERT(PropertyCallbackArguments::kShouldThrowOnErrorIndex == 0);
  STATIC_ASSERT(PropertyCallbackArguments::kHolderIndex == 1);
  STATIC_ASSERT(PropertyCallbackArguments::kIsolateIndex == 2);
  STATIC_ASSERT(PropertyCallbackArguments::kReturnValueDefaultValueIndex == 3);
  STATIC_ASSERT(PropertyCallbackArguments::kReturnValueOffset == 4);
  STATIC_ASSERT(PropertyCallbackArguments::kDataIndex == 5);
  STATIC_ASSERT(PropertyCallbackArguments::kThisIndex == 6);
  STATIC_ASSERT(PropertyCallbackArguments::kArgsLength == 7);

  Register receiver = ApiGetterDescriptor::ReceiverRegister();
  Register holder = ApiGetterDescriptor::HolderRegister();
  Register callback = ApiGetterDescriptor::CallbackRegister();
  Register scratch = t0;
  DCHECK(!AreAliased(receiver, holder, callback, scratch));

  Register api_function_address = a2;

  // Here and below +1 is for name() pushed after the args_ array.
  typedef PropertyCallbackArguments PCA;
  __ Subu(sp, sp, (PCA::kArgsLength + 1) * kPointerSize);
  __ sw(receiver, MemOperand(sp, (PCA::kThisIndex + 1) * kPointerSize));
  __ lw(scratch, FieldMemOperand(callback, AccessorInfo::kDataOffset));
  __ sw(scratch, MemOperand(sp, (PCA::kDataIndex + 1) * kPointerSize));
  __ LoadRoot(scratch, Heap::kUndefinedValueRootIndex);
  __ sw(scratch, MemOperand(sp, (PCA::kReturnValueOffset + 1) * kPointerSize));
  __ sw(scratch, MemOperand(sp, (PCA::kReturnValueDefaultValueIndex + 1) *
                                    kPointerSize));
  __ li(scratch, ExternalReference::isolate_address(isolate()));
  __ sw(scratch, MemOperand(sp, (PCA::kIsolateIndex + 1) * kPointerSize));
  __ sw(holder, MemOperand(sp, (PCA::kHolderIndex + 1) * kPointerSize));
  // should_throw_on_error -> false
  DCHECK_NULL(Smi::kZero);
  __ sw(zero_reg,
        MemOperand(sp, (PCA::kShouldThrowOnErrorIndex + 1) * kPointerSize));
  __ lw(scratch, FieldMemOperand(callback, AccessorInfo::kNameOffset));
  __ sw(scratch, MemOperand(sp, 0 * kPointerSize));

  // v8::PropertyCallbackInfo::args_ array and name handle.
  const int kStackUnwindSpace = PropertyCallbackArguments::kArgsLength + 1;

  // Load address of v8::PropertyAccessorInfo::args_ array and name handle.
  __ mov(a0, sp);                              // a0 = Handle<Name>
  __ Addu(a1, a0, Operand(1 * kPointerSize));  // a1 = v8::PCI::args_

  const int kApiStackSpace = 1;
  FrameScope frame_scope(masm, StackFrame::MANUAL);
  __ EnterExitFrame(false, kApiStackSpace);

  // Create v8::PropertyCallbackInfo object on the stack and initialize
  // it's args_ field.
  __ sw(a1, MemOperand(sp, 1 * kPointerSize));
  __ Addu(a1, sp, Operand(1 * kPointerSize));  // a1 = v8::PropertyCallbackInfo&

  ExternalReference thunk_ref =
      ExternalReference::invoke_accessor_getter_callback();

  __ lw(scratch, FieldMemOperand(callback, AccessorInfo::kJsGetterOffset));
  __ lw(api_function_address,
        FieldMemOperand(scratch, Foreign::kForeignAddressOffset));

  // +3 is to skip prolog, return address and name handle.
  MemOperand return_value_operand(
      fp, (PropertyCallbackArguments::kReturnValueOffset + 3) * kPointerSize);
  CallApiFunctionAndReturn(masm, api_function_address, thunk_ref,
                           kStackUnwindSpace, kInvalidStackOffset,
                           return_value_operand);
}

#undef __

}  // namespace internal
}  // namespace v8

#endif  // V8_TARGET_ARCH_MIPS

// Copyright 2011 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "v8.h"

#include "codegen.h"
#include "deoptimizer.h"
#include "full-codegen.h"
#include "safepoint-table.h"

// Note: this file was taken from the X64 version. ARM has a partially working
// lithium implementation, but for now it is not ported to mips.

namespace v8 {
namespace internal {


int Deoptimizer::table_entry_size_ = 32;


int Deoptimizer::patch_size() {
  const int kCallInstructionSizeInWords = 4;
  return kCallInstructionSizeInWords * Assembler::kInstrSize;
}


void Deoptimizer::EnsureRelocSpaceForLazyDeoptimization(Handle<Code> code) {
  // Nothing to do. No new relocation information is written for lazy
  // deoptimization on MIPS.
}


void Deoptimizer::DeoptimizeFunction(JSFunction* function) {
  HandleScope scope;
  AssertNoAllocation no_allocation;

  if (!function->IsOptimized()) return;

  // Get the optimized code.
  Code* code = function->code();

  // Invalidate the relocation information, as it will become invalid by the
  // code patching below, and is not needed any more.
  code->InvalidateRelocation();

  // For each return after a safepoint insert an absolute call to the
  // corresponding deoptimization entry.
  ASSERT(patch_size() % Assembler::kInstrSize == 0);
  int call_size_in_words = patch_size() / Assembler::kInstrSize;
  unsigned last_pc_offset = 0;
  SafepointTable table(function->code());
  for (unsigned i = 0; i < table.length(); i++) {
    unsigned pc_offset = table.GetPcOffset(i);
    SafepointEntry safepoint_entry = table.GetEntry(i);
    int deoptimization_index = safepoint_entry.deoptimization_index();
    int gap_code_size = safepoint_entry.gap_code_size();
    // Check that we did not shoot past next safepoint.
    CHECK(pc_offset >= last_pc_offset);
#ifdef DEBUG
    // Destroy the code which is not supposed to be run again.
    int instructions = (pc_offset - last_pc_offset) / Assembler::kInstrSize;
    CodePatcher destroyer(code->instruction_start() + last_pc_offset,
                          instructions);
    for (int x = 0; x < instructions; x++) {
      destroyer.masm()->break_(0);
    }
#endif
    last_pc_offset = pc_offset;
    if (deoptimization_index != Safepoint::kNoDeoptimizationIndex) {
      last_pc_offset += gap_code_size;
      CodePatcher patcher(code->instruction_start() + last_pc_offset,
                          call_size_in_words);
      Address deoptimization_entry = Deoptimizer::GetDeoptimizationEntry(
          deoptimization_index, Deoptimizer::LAZY);
      patcher.masm()->Call(deoptimization_entry, RelocInfo::NONE);
      last_pc_offset += patch_size();
    }
  }

#ifdef DEBUG
  // Destroy the code which is not supposed to be run again.
  int instructions =
      (code->safepoint_table_offset() - last_pc_offset) / Assembler::kInstrSize;
  CodePatcher destroyer(code->instruction_start() + last_pc_offset,
                        instructions);
  for (int x = 0; x < instructions; x++) {
    destroyer.masm()->break_(0);
  }
#endif

  // Add the deoptimizing code to the list.
  DeoptimizingCodeListNode* node = new DeoptimizingCodeListNode(code);
  DeoptimizerData* data = code->GetIsolate()->deoptimizer_data();
  node->set_next(data->deoptimizing_code_list_);
  data->deoptimizing_code_list_ = node;

  // Set the code for the function to non-optimized version.
  function->ReplaceCode(function->shared()->code());

  if (FLAG_trace_deopt) {
    PrintF("[forced deoptimization: ");
    function->PrintName();
    PrintF(" / %x]\n", reinterpret_cast<uint32_t>(function));
#ifdef DEBUG
    if (FLAG_print_code) {
      code->PrintLn();
    }
#endif
  }
}


void Deoptimizer::PatchStackCheckCodeAt(Address pc_after,
                                        Code* check_code,
                                        Code* replacement_code) {
  UNIMPLEMENTED();
}


void Deoptimizer::RevertStackCheckCodeAt(Address pc_after,
                                         Code* check_code,
                                         Code* replacement_code) {
  UNIMPLEMENTED();
}


void Deoptimizer::DoComputeOsrOutputFrame() {
  UNIMPLEMENTED();
}


// This code is very similar to ia32/arm code, but relies on register names
//  (fp, sp) and how the frame is laid out.
void Deoptimizer::DoComputeFrame(TranslationIterator* iterator,
                                 int frame_index) {
  // Read the ast node id, function, and frame height for this output frame.
  Translation::Opcode opcode =
      static_cast<Translation::Opcode>(iterator->Next());
  USE(opcode);
  ASSERT(Translation::FRAME == opcode);
  int node_id = iterator->Next();
  JSFunction* function = JSFunction::cast(ComputeLiteral(iterator->Next()));
  unsigned height = iterator->Next();
  unsigned height_in_bytes = height * kPointerSize;
  if (FLAG_trace_deopt) {
    PrintF("  translating ");
    function->PrintName();
    PrintF(" => node=%d, height=%d\n", node_id, height_in_bytes);
  }

  // The 'fixed' part of the frame consists of the incoming parameters and
  // the part described by JavaScriptFrameConstants.
  unsigned fixed_frame_size = ComputeFixedSize(function);
  unsigned input_frame_size = input_->GetFrameSize();
  unsigned output_frame_size = height_in_bytes + fixed_frame_size;

  // Allocate and store the output frame description.
  FrameDescription* output_frame =
      new(output_frame_size) FrameDescription(output_frame_size, function);

  bool is_bottommost = (0 == frame_index);
  bool is_topmost = (output_count_ - 1 == frame_index);
  ASSERT(frame_index >= 0 && frame_index < output_count_);
  ASSERT(output_[frame_index] == NULL);
  output_[frame_index] = output_frame;

  // The top address for the bottommost output frame can be computed from
  // the input frame pointer and the output frame's height.  For all
  // subsequent output frames, it can be computed from the previous one's
  // top address and the current frame's size.
  uint32_t top_address;
  if (is_bottommost) {
    // 2 = context and function in the frame.
    // TODO(kalmard): top_address gets a wrong value and that causes an error at
    // the assertion around line 287. The adjustment from fp or the position of
    // fp is probably broken and needs to be checked.
    top_address =
        input_->GetRegister(fp.code()) - (2 * kPointerSize) - height_in_bytes;
  } else {
    top_address = output_[frame_index - 1]->GetTop() - output_frame_size;
  }
  output_frame->SetTop(top_address);

  // Compute the incoming parameter translation.
  int parameter_count = function->shared()->formal_parameter_count() + 1;
  unsigned output_offset = output_frame_size;
  unsigned input_offset = input_frame_size;
  for (int i = 0; i < parameter_count; ++i) {
    output_offset -= kPointerSize;
    DoTranslateCommand(iterator, frame_index, output_offset);
  }
  input_offset -= (parameter_count * kPointerSize);

  // There are no translation commands for the caller's pc and fp, the
  // context, and the function.  Synthesize their values and set them up
  // explicitly.
  //
  // The caller's pc for the bottommost output frame is the same as in the
  // input frame.  For all subsequent output frames, it can be read from the
  // previous one.  This frame's pc can be computed from the non-optimized
  // function code and AST id of the bailout.
  output_offset -= kPointerSize;
  input_offset -= kPointerSize;
  intptr_t value;
  if (is_bottommost) {
    value = input_->GetFrameSlot(input_offset);
  } else {
    value = output_[frame_index - 1]->GetPc();
  }
  output_frame->SetFrameSlot(output_offset, value);
  if (FLAG_trace_deopt) {
    PrintF("    0x%08x: [top + %d] <- 0x%08x ; caller's pc\n",
           top_address + output_offset, output_offset, value);
  }

  // The caller's frame pointer for the bottommost output frame is the same
  // as in the input frame.  For all subsequent output frames, it can be
  // read from the previous one.  Also compute and set this frame's frame
  // pointer.
  output_offset -= kPointerSize;
  input_offset -= kPointerSize;
  if (is_bottommost) {
    value = input_->GetFrameSlot(input_offset);
  } else {
    value = output_[frame_index - 1]->GetFp();
  }
  output_frame->SetFrameSlot(output_offset, value);
  intptr_t fp_value = top_address + output_offset;
  ASSERT(!is_bottommost || input_->GetRegister(fp.code()) == fp_value);
  output_frame->SetFp(fp_value);
  if (is_topmost) {
    output_frame->SetRegister(fp.code(), fp_value);
  }
  if (FLAG_trace_deopt) {
    PrintF("    0x%08x: [top + %d] <- 0x%08x ; caller's fp\n",
           fp_value, output_offset, value);
  }

  // For the bottommost output frame the context can be gotten from the input
  // frame. For all subsequent output frames it can be gotten from the function
  // so long as we don't inline functions that need local contexts.
  output_offset -= kPointerSize;
  input_offset -= kPointerSize;
  if (is_bottommost) {
    value = input_->GetFrameSlot(input_offset);
  } else {
    value = reinterpret_cast<intptr_t>(function->context());
  }
  output_frame->SetFrameSlot(output_offset, value);
  if (is_topmost) {
    output_frame->SetRegister(cp.code(), value);
  }
  if (FLAG_trace_deopt) {
    PrintF("    0x%08x: [top + %d] <- 0x%08x ; context\n",
           top_address + output_offset, output_offset, value);
  }

  // The function was mentioned explicitly in the BEGIN_FRAME.
  output_offset -= kPointerSize;
  input_offset -= kPointerSize;
  value = reinterpret_cast<uint32_t>(function);
  // The function for the bottommost output frame should also agree with the
  // input frame.
  ASSERT(!is_bottommost || input_->GetFrameSlot(input_offset) == value);
  output_frame->SetFrameSlot(output_offset, value);
  if (FLAG_trace_deopt) {
    PrintF("    0x%08x: [top + %d] <- 0x%08x ; function\n",
           top_address + output_offset, output_offset, value);
  }

  // Translate the rest of the frame.
  for (unsigned i = 0; i < height; ++i) {
    output_offset -= kPointerSize;
    DoTranslateCommand(iterator, frame_index, output_offset);
  }
  ASSERT(0 == output_offset);

  // Compute this frame's PC, state, and continuation.
  Code* non_optimized_code = function->shared()->code();
  FixedArray* raw_data = non_optimized_code->deoptimization_data();
  DeoptimizationOutputData* data = DeoptimizationOutputData::cast(raw_data);
  Address start = non_optimized_code->instruction_start();
  unsigned pc_and_state = GetOutputInfo(data, node_id, function->shared());
  unsigned pc_offset = FullCodeGenerator::PcField::decode(pc_and_state);
  uint32_t pc_value = reinterpret_cast<uint32_t>(start + pc_offset);
  output_frame->SetPc(pc_value);
  if (is_topmost) {
    // output_frame->SetRegister(pc.code(), pc_value);  // TODO(plind): BROKEN here, setting pc .......
    output_frame->SetRegister(ra.code(), pc_value);  // TODO(plind): HACKEDhere, just so it compiles .....
  }

  FullCodeGenerator::State state =
      FullCodeGenerator::StateField::decode(pc_and_state);
  output_frame->SetState(Smi::FromInt(state));


  // Set the continuation for the topmost frame.
  if (is_topmost) {
    Builtins* builtins = isolate_->builtins();
    Code* continuation = (bailout_type_ == EAGER)
        ? builtins->builtin(Builtins::kNotifyDeoptimized)
        : builtins->builtin(Builtins::kNotifyLazyDeoptimized);
    output_frame->SetContinuation(
        reinterpret_cast<uint32_t>(continuation->entry()));
  }

  if (output_count_ - 1 == frame_index) iterator->Done();
}


#define __ masm()->


// This code tries to be close to ia32 code so that any changes can be
// easily ported.
void Deoptimizer::EntryGenerator::Generate() {
  GeneratePrologue();

  Isolate* isolate = masm()->isolate();

  CpuFeatures::Scope scope(FPU);
  // Save all general purpose registers before messing with them.
  const int kNumberOfRegisters = Register::kNumRegisters;

  // Everything but ra and ip which will be saved but not restored.
  RegList restored_regs = kJSCallerSaved | kCalleeSaved | zero_reg.bit() | at.bit()
                        | k0.bit() | k1.bit() | gp.bit() ;   // TODO(plind): check this.......

  const int kDoubleRegsSize =
      kDoubleSize * FPURegister::kNumAllocatableRegisters;

  // Save all general purpose registers before messing with them.
  __ Subu(sp, sp, Operand(kDoubleRegsSize));
  for (int i = 0; i < FPURegister::kNumAllocatableRegisters; ++i) {
    FPURegister fpu_reg = FPURegister::FromAllocationIndex(i);
    int offset = i * kDoubleSize;
    __ sdc1(fpu_reg, MemOperand(sp, offset));
  }

  // Push all 32 registers (needed to populate FrameDescription::registers_).
  // TODO(plind): This seems WACKY to save all regs, like at, k0, k1, and junk..... revisit this.
  //              Maybe we want to save useful regs, but leave gaps ??
  __ MultiPush(restored_regs | sp.bit() | ra.bit());

  const int kSavedRegistersAreaSize =
      (kNumberOfRegisters * kPointerSize) + kDoubleRegsSize;

  // Get the bailout id from the stack.
  // TODO(kalmard): this adjustment by 8 is needed for some reason. This needs
  // to be revisited once the number and format of saved registers are finalized.
  // This may relate to the top_address issue in Deoptimizer::DoComputeFrame.
  __ lw(a2, MemOperand(sp, kSavedRegistersAreaSize - 8));

  // Get the address of the location in the code object if possible (a3) (return
  // address for lazy deoptimization) and compute the fp-to-sp delta in
  // register t0.
  if (type() == EAGER) {
    __ li(a3, Operand(0));
    // Correct one word for bailout id.
    __ Addu(t0, sp, Operand(kSavedRegistersAreaSize + (1 * kPointerSize)));
  } else if (type() == OSR) {
    __ mov(a3, ra);
    // Correct one word for bailout id.
    __ Addu(t0, sp, Operand(kSavedRegistersAreaSize + (1 * kPointerSize)));
  } else {
    __ mov(a3, ra);
    // Correct two words for bailout id and return address.
    __ Addu(t0, sp, Operand(kSavedRegistersAreaSize + (2 * kPointerSize)));
  }
  // TODO(kalmard): another adjustment by 8 to satisfy the Deoptimizer
  // constructor. See my comment above.
  //__ Subu(t0, fp, t0);
  __ li(t0, 8);

  // Allocate a new deoptimizer object.
  // Pass four arguments in a0 to a3 and fifth & sixth arguments on stack.
  __ PrepareCallCFunction(6, t1);
  __ lw(a0, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));
  __ li(a1, Operand(type()));  // bailout type,
  // a2: bailout id already loaded.
  // a3: code address or 0 already loaded.
  __ sw(t0, CFunctionArgumentOperand(5));  // Fp-to-sp delta.
  __ li(t1, Operand(ExternalReference::isolate_address()));
  __ sw(t1, CFunctionArgumentOperand(6));  // Isolate.
  // Call Deoptimizer::New().
  __ CallCFunction(ExternalReference::new_deoptimizer_function(isolate), 6);

  // Preserve "deoptimizer" object in register v0 and get the input
  // frame descriptor pointer to a1 (deoptimizer->input_);
  // Move deopt-obj to a0 for call to Deoptimizer::ComputeOutputFrames() below.
  __ mov(a0, v0);
  __ lw(a1, MemOperand(v0, Deoptimizer::input_offset()));

  // Copy core registers into FrameDescription::registers_[kNumRegisters].
  ASSERT(Register::kNumRegisters == kNumberOfRegisters);
  for (int i = 0; i < kNumberOfRegisters; i++) {
    int offset = (i * kPointerSize) + FrameDescription::registers_offset();
    __ lw(a2, MemOperand(sp, i * kPointerSize));
    __ sw(a2, MemOperand(a1, offset));
  }

  // Copy FPU registers to
  // double_registers_[DoubleRegister::kNumAllocatableRegisters]
  int double_regs_offset = FrameDescription::double_registers_offset();
  for (int i = 0; i < FPURegister::kNumAllocatableRegisters; ++i) {
    int dst_offset = i * kDoubleSize + double_regs_offset;
    int src_offset = i * kDoubleSize + kNumberOfRegisters * kPointerSize;
    __ ldc1(f0, MemOperand(sp, src_offset));
    __ sdc1(f0, MemOperand(a1, dst_offset));
  }

  // TODO(plind): If we are removing from the stack here, why did we push them,
  // rather than just save them to the FrameDescription::registers_ ? .........................???

  // Remove the bailout id, eventually return address, and the saved registers
  // from the stack.
  if (type() == EAGER || type() == OSR) {
    __ Addu(sp, sp, Operand(kSavedRegistersAreaSize + (1 * kPointerSize)));
  } else {
    __ Addu(sp, sp, Operand(kSavedRegistersAreaSize + (2 * kPointerSize)));
  }

  // Compute a pointer to the unwinding limit in register a2; that is
  // the first stack slot not part of the input frame.
  __ lw(a2, MemOperand(a1, FrameDescription::frame_size_offset()));
  __ Addu(a2, a2, sp);

  // Unwind the stack down to - but not including - the unwinding
  // limit and copy the contents of the activation frame to the input
  // frame description.
  __ Addu(a3, a1, Operand(FrameDescription::frame_content_offset()));
  Label pop_loop;
  __ bind(&pop_loop);
  __ pop(t0);
  __ sw(t0, MemOperand(a3, 0));
// __ cmp(r2, sp);
// __ b(ne, &pop_loop);
  __ Branch(USE_DELAY_SLOT, &pop_loop, ne, a2, Operand(sp));
  __ Addu(a3, a3, Operand(sizeof(uint32_t)));  // In delay slot.
  
  // Compute the output frame in the deoptimizer.
  __ push(a0);  // Preserve deoptimizer object across call.
  // a0: deoptimizer object; r1: scratch.
  __ PrepareCallCFunction(1, a1);
  // Call Deoptimizer::ComputeOutputFrames().
  __ CallCFunction(
      ExternalReference::compute_output_frames_function(isolate), 1);
  __ pop(a0);  // Restore deoptimizer object (class Deoptimizer).

  // Replace the current (input) frame with the output frames.
  Label outer_push_loop, inner_push_loop;
  // Outer loop state: a0 = current "FrameDescription** output_",
  // a1 = one past the last FrameDescription**.
  __ lw(a1, MemOperand(a0, Deoptimizer::output_count_offset()));
  __ lw(a0, MemOperand(a0, Deoptimizer::output_offset()));  // a0 is output_.
  // __ add(r1, r0, Operand(r1, LSL, 2));
  __ sll(a1, a1, kPointerSizeLog2);  // Count to offset.
  __ addu(a1, a0, a1);  // a1 = one past the last FrameDescription**.
  __ bind(&outer_push_loop);
  // Inner loop state: a2 = current FrameDescription*, a3 = loop index.
  __ lw(a2, MemOperand(a0, 0));  // output_[ix]
  __ lw(a3, MemOperand(a2, FrameDescription::frame_size_offset()));
  __ bind(&inner_push_loop);
  __ Subu(a3, a3, Operand(sizeof(uint32_t)));
  __ Addu(t2, a2, Operand(a3));
  __ lw(t3, MemOperand(t2, FrameDescription::frame_content_offset()));
  __ push(t3);
// __ cmp(r3, Operand(0));
// __ b(ne, &inner_push_loop);  // test for gt?
  __ Branch(&inner_push_loop, ne, a3, Operand(zero_reg));

  __ Addu(a0, a0, Operand(kPointerSize));
// __ cmp(r0, r1);
// __ b(lt, &outer_push_loop);
  __ Branch(&outer_push_loop, lt, a0, Operand(a1));
  

  // Push state, pc, and continuation from the last output frame.
  if (type() != OSR) {
    __ lw(t2, MemOperand(a2, FrameDescription::state_offset()));
    __ push(t2);
  }

  __ lw(t2, MemOperand(a2, FrameDescription::pc_offset()));
  __ push(t2);
  __ lw(t2, MemOperand(a2, FrameDescription::continuation_offset()));
  __ push(t2);

  // Push the registers from the last output frame.
  for (int i = kNumberOfRegisters - 1; i >= 0; i--) {
    int offset = (i * kPointerSize) + FrameDescription::registers_offset();
    __ lw(t2, MemOperand(a2, offset));
    __ push(t2);
  }

  // Restore the registers from the stack.
  __ MultiPop(restored_regs);  // All but pc registers.
  __ Drop(2);  // Remove sp and ra.

  // Set up the roots register.
  ExternalReference roots_address = ExternalReference::roots_address(isolate);
  __ li(roots, Operand(roots_address));

  __ pop(at);  // Remove pc.
  __ pop(t3);  // Get continuation, leave pc on stack.
  __ pop(ra);
  __ Jump(t3);
  __ stop("Unreachable.");
}


void Deoptimizer::TableEntryGenerator::GeneratePrologue() {
  Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm());

  // Create a sequence of deoptimization entries. Note that any
  // registers may be still live.

  // TODO (kalmard): This is pretty hacky. Instead of one big Branch that would
  // involve the trampoline pool, create a series of small ones. This helps if
  // table_entry_size_ gets larger but probably slows things down quite a bit.
  Vector<Label> skip = Vector<Label>::New(count() + 1);
  for (int i = 0; i < count(); i++) {
    int start = masm()->pc_offset();
    USE(start);
    if (type() != EAGER) {
      // Emulate ia32 like call by pushing return address to stack.
      __ push(ra);
    }
    __ li(at, Operand(i));
    __ push(at);
    __ bind(&skip[i]);
    __ Branch(&skip[i+1]);

    // Pad the rest of the code.
    while(table_entry_size_ > (masm()->pc_offset() - start)) {
      __ nop();
    }

    ASSERT_EQ(table_entry_size_, masm()->pc_offset() - start);
  }
  __ bind(&skip[count()]);
}

#undef __


} }  // namespace v8::internal

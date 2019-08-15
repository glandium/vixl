// Copyright 2015, VIXL authors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifdef VIXL_INCLUDE_SIMULATOR_AARCH64

#include <cmath>
#include <cstring>
#include <limits>

#include "simulator-aarch64.h"

namespace vixl {
namespace aarch64 {

using vixl::internal::SimFloat16;

const Instruction* Simulator::kEndOfSimAddress = NULL;

void SimSystemRegister::SetBits(int msb, int lsb, uint32_t bits) {
  int width = msb - lsb + 1;
  VIXL_ASSERT(IsUintN(width, bits) || IsIntN(width, bits));

  bits <<= lsb;
  uint32_t mask = ((1 << width) - 1) << lsb;
  VIXL_ASSERT((mask & write_ignore_mask_) == 0);

  value_ = (value_ & ~mask) | (bits & mask);
}


SimSystemRegister SimSystemRegister::DefaultValueFor(SystemRegister id) {
  switch (id) {
    case NZCV:
      return SimSystemRegister(0x00000000, NZCVWriteIgnoreMask);
    case FPCR:
      return SimSystemRegister(0x00000000, FPCRWriteIgnoreMask);
    default:
      VIXL_UNREACHABLE();
      return SimSystemRegister();
  }
}


Simulator::Simulator(Decoder* decoder, FILE* stream)
    : movprfx_(NULL), cpu_features_auditor_(decoder, CPUFeatures::All()) {
  // Ensure that shift operations act as the simulator expects.
  VIXL_ASSERT((static_cast<int32_t>(-1) >> 1) == -1);
  VIXL_ASSERT((static_cast<uint32_t>(-1) >> 1) == 0x7fffffff);

  instruction_stats_ = false;

  // Set up the decoder.
  decoder_ = decoder;
  decoder_->AppendVisitor(this);

  stream_ = stream;

  print_disasm_ = new PrintDisassembler(stream_);
  // The Simulator and Disassembler share the same available list, held by the
  // auditor. The Disassembler only annotates instructions with features that
  // are _not_ available, so registering the auditor should have no effect
  // unless the simulator is about to abort (due to missing features). In
  // practice, this means that with trace enabled, the simulator will crash just
  // after the disassembler prints the instruction, with the missing features
  // enumerated.
  print_disasm_->RegisterCPUFeaturesAuditor(&cpu_features_auditor_);

  SetColouredTrace(false);
  trace_parameters_ = LOG_NONE;

  // We have to configure the SVE vector register length before calling
  // ResetState().
  SetVectorLengthInBits(kZRegMinSize);

  ResetState();

  // Allocate and set up the simulator stack.
  stack_ = new byte[stack_size_];
  stack_limit_ = stack_ + stack_protection_size_;
  // Configure the starting stack pointer.
  //  - Find the top of the stack.
  byte* tos = stack_ + stack_size_;
  //  - There's a protection region at both ends of the stack.
  tos -= stack_protection_size_;
  //  - The stack pointer must be 16-byte aligned.
  tos = AlignDown(tos, 16);
  WriteSp(tos);

  instrumentation_ = NULL;

  // Print a warning about exclusive-access instructions, but only the first
  // time they are encountered. This warning can be silenced using
  // SilenceExclusiveAccessWarning().
  print_exclusive_access_warning_ = true;

  guard_pages_ = false;

  // Initialize the common state of RNDR and RNDRRS.
  uint16_t seed[3] = {11, 22, 33};
  VIXL_STATIC_ASSERT(sizeof(seed) == sizeof(rndr_state_));
  memcpy(rndr_state_, seed, sizeof(rndr_state_));
}

void Simulator::ResetSystemRegisters() {
  // Reset the system registers.
  nzcv_ = SimSystemRegister::DefaultValueFor(NZCV);
  fpcr_ = SimSystemRegister::DefaultValueFor(FPCR);
}

void Simulator::ResetRegisters() {
  for (unsigned i = 0; i < kNumberOfRegisters; i++) {
    WriteXRegister(i, 0xbadbeef);
  }
  // Returning to address 0 exits the Simulator.
  WriteLr(kEndOfSimAddress);
}

void Simulator::ResetVRegisters() {
  // Set SVE/FP registers to a value that is a NaN in both 32-bit and 64-bit FP.
  VIXL_ASSERT((GetVectorLengthInBytes() % kDRegSizeInBytes) == 0);
  int lane_count = GetVectorLengthInBytes() / kDRegSizeInBytes;
  for (unsigned i = 0; i < kNumberOfZRegisters; i++) {
    VIXL_ASSERT(vregisters_[i].GetSizeInBytes() == GetVectorLengthInBytes());
    vregisters_[i].NotifyAccessAsZ();
    for (int lane = 0; lane < lane_count; lane++) {
      // Encode the register number and (D-sized) lane into each NaN, to
      // make them easier to trace.
      uint64_t nan_bits = 0x7ff0f0007f80f000 | (0x0000000100000000 * i) |
                          (0x0000000000000001 * lane);
      VIXL_ASSERT(IsSignallingNaN(RawbitsToDouble(nan_bits & kDRegMask)));
      VIXL_ASSERT(IsSignallingNaN(RawbitsToFloat(nan_bits & kSRegMask)));
      vregisters_[i].Insert(lane, nan_bits);
    }
  }
}

void Simulator::ResetPRegisters() {
  VIXL_ASSERT((GetPredicateLengthInBytes() % kHRegSizeInBytes) == 0);
  int lane_count = GetPredicateLengthInBytes() / kHRegSizeInBytes;
  // Ensure the register configuration fits in this bit encoding.
  VIXL_STATIC_ASSERT(kNumberOfPRegisters <= UINT8_MAX);
  VIXL_ASSERT(lane_count <= UINT8_MAX);
  for (unsigned i = 0; i < kNumberOfPRegisters; i++) {
    VIXL_ASSERT(pregisters_[i].GetSizeInBytes() == GetPredicateLengthInBytes());
    for (int lane = 0; lane < lane_count; lane++) {
      // Encode the register number and (H-sized) lane into each lane slot.
      uint16_t bits = (0x0100 * lane) | i;
      pregisters_[i].Insert(lane, bits);
    }
  }
}

void Simulator::ResetState() {
  ResetSystemRegisters();
  ResetRegisters();
  ResetVRegisters();
  ResetPRegisters();

  pc_ = NULL;
  pc_modified_ = false;

  // BTI state.
  btype_ = DefaultBType;
  next_btype_ = DefaultBType;
}

void Simulator::SetVectorLengthInBits(unsigned vector_length) {
  VIXL_ASSERT((vector_length >= kZRegMinSize) &&
              (vector_length <= kZRegMaxSize));
  VIXL_ASSERT((vector_length % kZRegMinSize) == 0);
  vector_length_ = vector_length;

  for (unsigned i = 0; i < kNumberOfZRegisters; i++) {
    vregisters_[i].SetSizeInBytes(GetVectorLengthInBytes());
  }
  for (unsigned i = 0; i < kNumberOfPRegisters; i++) {
    pregisters_[i].SetSizeInBytes(GetPredicateLengthInBytes());
  }

  ResetVRegisters();
  ResetPRegisters();
}

Simulator::~Simulator() {
  delete[] stack_;
  // The decoder may outlive the simulator.
  decoder_->RemoveVisitor(print_disasm_);
  delete print_disasm_;

  decoder_->RemoveVisitor(instrumentation_);
  delete instrumentation_;
}


void Simulator::Run() {
  // Flush any written registers before executing anything, so that
  // manually-set registers are logged _before_ the first instruction.
  LogAllWrittenRegisters();

  while (pc_ != kEndOfSimAddress) {
    ExecuteInstruction();
  }
}


void Simulator::RunFrom(const Instruction* first) {
  WritePc(first, NoBranchLog);
  Run();
}


// clang-format off
const char* Simulator::xreg_names[] = {"x0",  "x1",  "x2",  "x3",  "x4",  "x5",
                                       "x6",  "x7",  "x8",  "x9",  "x10", "x11",
                                       "x12", "x13", "x14", "x15", "x16", "x17",
                                       "x18", "x19", "x20", "x21", "x22", "x23",
                                       "x24", "x25", "x26", "x27", "x28", "x29",
                                       "lr",  "xzr", "sp"};

const char* Simulator::wreg_names[] = {"w0",  "w1",  "w2",  "w3",  "w4",  "w5",
                                       "w6",  "w7",  "w8",  "w9",  "w10", "w11",
                                       "w12", "w13", "w14", "w15", "w16", "w17",
                                       "w18", "w19", "w20", "w21", "w22", "w23",
                                       "w24", "w25", "w26", "w27", "w28", "w29",
                                       "w30", "wzr", "wsp"};

const char* Simulator::hreg_names[] = {"h0",  "h1",  "h2",  "h3",  "h4",  "h5",
                                       "h6",  "h7",  "h8",  "h9",  "h10", "h11",
                                       "h12", "h13", "h14", "h15", "h16", "h17",
                                       "h18", "h19", "h20", "h21", "h22", "h23",
                                       "h24", "h25", "h26", "h27", "h28", "h29",
                                       "h30", "h31"};

const char* Simulator::sreg_names[] = {"s0",  "s1",  "s2",  "s3",  "s4",  "s5",
                                       "s6",  "s7",  "s8",  "s9",  "s10", "s11",
                                       "s12", "s13", "s14", "s15", "s16", "s17",
                                       "s18", "s19", "s20", "s21", "s22", "s23",
                                       "s24", "s25", "s26", "s27", "s28", "s29",
                                       "s30", "s31"};

const char* Simulator::dreg_names[] = {"d0",  "d1",  "d2",  "d3",  "d4",  "d5",
                                       "d6",  "d7",  "d8",  "d9",  "d10", "d11",
                                       "d12", "d13", "d14", "d15", "d16", "d17",
                                       "d18", "d19", "d20", "d21", "d22", "d23",
                                       "d24", "d25", "d26", "d27", "d28", "d29",
                                       "d30", "d31"};

const char* Simulator::vreg_names[] = {"v0",  "v1",  "v2",  "v3",  "v4",  "v5",
                                       "v6",  "v7",  "v8",  "v9",  "v10", "v11",
                                       "v12", "v13", "v14", "v15", "v16", "v17",
                                       "v18", "v19", "v20", "v21", "v22", "v23",
                                       "v24", "v25", "v26", "v27", "v28", "v29",
                                       "v30", "v31"};

const char* Simulator::zreg_names[] = {"z0",  "z1",  "z2",  "z3",  "z4",  "z5",
                                       "z6",  "z7",  "z8",  "z9",  "z10", "z11",
                                       "z12", "z13", "z14", "z15", "z16", "z17",
                                       "z18", "z19", "z20", "z21", "z22", "z23",
                                       "z24", "z25", "z26", "z27", "z28", "z29",
                                       "z30", "z31"};

const char* Simulator::preg_names[] = {"p0",  "p1",  "p2",  "p3",  "p4",  "p5",
                                       "p6",  "p7",  "p8",  "p9",  "p10", "p11",
                                       "p12", "p13", "p14", "p15"};
// clang-format on


const char* Simulator::WRegNameForCode(unsigned code, Reg31Mode mode) {
  VIXL_ASSERT(code < kNumberOfRegisters);
  // If the code represents the stack pointer, index the name after zr.
  if ((code == kZeroRegCode) && (mode == Reg31IsStackPointer)) {
    code = kZeroRegCode + 1;
  }
  return wreg_names[code];
}


const char* Simulator::XRegNameForCode(unsigned code, Reg31Mode mode) {
  VIXL_ASSERT(code < kNumberOfRegisters);
  // If the code represents the stack pointer, index the name after zr.
  if ((code == kZeroRegCode) && (mode == Reg31IsStackPointer)) {
    code = kZeroRegCode + 1;
  }
  return xreg_names[code];
}


const char* Simulator::HRegNameForCode(unsigned code) {
  VIXL_ASSERT(code < kNumberOfVRegisters);
  return hreg_names[code];
}


const char* Simulator::SRegNameForCode(unsigned code) {
  VIXL_ASSERT(code < kNumberOfVRegisters);
  return sreg_names[code];
}


const char* Simulator::DRegNameForCode(unsigned code) {
  VIXL_ASSERT(code < kNumberOfVRegisters);
  return dreg_names[code];
}


const char* Simulator::VRegNameForCode(unsigned code) {
  VIXL_ASSERT(code < kNumberOfVRegisters);
  return vreg_names[code];
}


const char* Simulator::ZRegNameForCode(unsigned code) {
  VIXL_ASSERT(code < kNumberOfZRegisters);
  return zreg_names[code];
}


const char* Simulator::PRegNameForCode(unsigned code) {
  VIXL_ASSERT(code < kNumberOfPRegisters);
  return preg_names[code];
}


#define COLOUR(colour_code) "\033[0;" colour_code "m"
#define COLOUR_BOLD(colour_code) "\033[1;" colour_code "m"
#define COLOUR_HIGHLIGHT "\033[43m"
#define NORMAL ""
#define GREY "30"
#define RED "31"
#define GREEN "32"
#define YELLOW "33"
#define BLUE "34"
#define MAGENTA "35"
#define CYAN "36"
#define WHITE "37"
void Simulator::SetColouredTrace(bool value) {
  coloured_trace_ = value;

  clr_normal = value ? COLOUR(NORMAL) : "";
  clr_flag_name = value ? COLOUR_BOLD(WHITE) : "";
  clr_flag_value = value ? COLOUR(NORMAL) : "";
  clr_reg_name = value ? COLOUR_BOLD(CYAN) : "";
  clr_reg_value = value ? COLOUR(CYAN) : "";
  clr_vreg_name = value ? COLOUR_BOLD(MAGENTA) : "";
  clr_vreg_value = value ? COLOUR(MAGENTA) : "";
  clr_preg_name = value ? COLOUR_BOLD(GREEN) : "";
  clr_preg_value = value ? COLOUR(GREEN) : "";
  clr_memory_address = value ? COLOUR_BOLD(BLUE) : "";
  clr_warning = value ? COLOUR_BOLD(YELLOW) : "";
  clr_warning_message = value ? COLOUR(YELLOW) : "";
  clr_printf = value ? COLOUR(GREEN) : "";
  clr_branch_marker = value ? COLOUR(GREY) COLOUR_HIGHLIGHT : "";

  if (value) {
    print_disasm_->SetCPUFeaturesPrefix("// Needs: " COLOUR_BOLD(RED));
    print_disasm_->SetCPUFeaturesSuffix(COLOUR(NORMAL));
  } else {
    print_disasm_->SetCPUFeaturesPrefix("// Needs: ");
    print_disasm_->SetCPUFeaturesSuffix("");
  }
}


void Simulator::SetTraceParameters(int parameters) {
  bool disasm_before = trace_parameters_ & LOG_DISASM;
  trace_parameters_ = parameters;
  bool disasm_after = trace_parameters_ & LOG_DISASM;

  if (disasm_before != disasm_after) {
    if (disasm_after) {
      decoder_->InsertVisitorBefore(print_disasm_, this);
    } else {
      decoder_->RemoveVisitor(print_disasm_);
    }
  }
}


void Simulator::SetInstructionStats(bool value) {
  if (value != instruction_stats_) {
    if (value) {
      if (instrumentation_ == NULL) {
        // Set the sample period to 10, as the VIXL examples and tests are
        // short.
        instrumentation_ = new Instrument("vixl_stats.csv", 10);
      }
      decoder_->AppendVisitor(instrumentation_);
    } else if (instrumentation_ != NULL) {
      decoder_->RemoveVisitor(instrumentation_);
    }
    instruction_stats_ = value;
  }
}

// Helpers ---------------------------------------------------------------------
uint64_t Simulator::AddWithCarry(unsigned reg_size,
                                 bool set_flags,
                                 uint64_t left,
                                 uint64_t right,
                                 int carry_in) {
  VIXL_ASSERT((carry_in == 0) || (carry_in == 1));
  VIXL_ASSERT((reg_size == kXRegSize) || (reg_size == kWRegSize));

  uint64_t max_uint = (reg_size == kWRegSize) ? kWMaxUInt : kXMaxUInt;
  uint64_t reg_mask = (reg_size == kWRegSize) ? kWRegMask : kXRegMask;
  uint64_t sign_mask = (reg_size == kWRegSize) ? kWSignMask : kXSignMask;

  left &= reg_mask;
  right &= reg_mask;
  uint64_t result = (left + right + carry_in) & reg_mask;

  if (set_flags) {
    ReadNzcv().SetN(CalcNFlag(result, reg_size));
    ReadNzcv().SetZ(CalcZFlag(result));

    // Compute the C flag by comparing the result to the max unsigned integer.
    uint64_t max_uint_2op = max_uint - carry_in;
    bool C = (left > max_uint_2op) || ((max_uint_2op - left) < right);
    ReadNzcv().SetC(C ? 1 : 0);

    // Overflow iff the sign bit is the same for the two inputs and different
    // for the result.
    uint64_t left_sign = left & sign_mask;
    uint64_t right_sign = right & sign_mask;
    uint64_t result_sign = result & sign_mask;
    bool V = (left_sign == right_sign) && (left_sign != result_sign);
    ReadNzcv().SetV(V ? 1 : 0);

    LogSystemRegister(NZCV);
  }
  return result;
}


int64_t Simulator::ShiftOperand(unsigned reg_size,
                                int64_t value,
                                Shift shift_type,
                                unsigned amount) const {
  VIXL_ASSERT((reg_size == kWRegSize) || (reg_size == kXRegSize));
  if (amount == 0) {
    return value;
  }
  uint64_t uvalue = static_cast<uint64_t>(value);
  uint64_t mask = kWRegMask;
  bool is_negative = (uvalue & kWSignMask) != 0;
  if (reg_size == kXRegSize) {
    mask = kXRegMask;
    is_negative = (uvalue & kXSignMask) != 0;
  }

  switch (shift_type) {
    case LSL:
      uvalue <<= amount;
      break;
    case LSR:
      uvalue >>= amount;
      break;
    case ASR:
      uvalue >>= amount;
      if (is_negative) {
        // Simulate sign-extension to 64 bits.
        uvalue |= ~UINT64_C(0) << (reg_size - amount);
      }
      break;
    case ROR: {
      uvalue = RotateRight(uvalue, amount, reg_size);
      break;
    }
    default:
      VIXL_UNIMPLEMENTED();
      return 0;
  }
  uvalue &= mask;

  int64_t result;
  memcpy(&result, &uvalue, sizeof(result));
  return result;
}


int64_t Simulator::ExtendValue(unsigned reg_size,
                               int64_t value,
                               Extend extend_type,
                               unsigned left_shift) const {
  switch (extend_type) {
    case UXTB:
      value &= kByteMask;
      break;
    case UXTH:
      value &= kHalfWordMask;
      break;
    case UXTW:
      value &= kWordMask;
      break;
    case SXTB:
      value &= kByteMask;
      if ((value & 0x80) != 0) {
        value |= ~UINT64_C(0) << 8;
      }
      break;
    case SXTH:
      value &= kHalfWordMask;
      if ((value & 0x8000) != 0) {
        value |= ~UINT64_C(0) << 16;
      }
      break;
    case SXTW:
      value &= kWordMask;
      if ((value & 0x80000000) != 0) {
        value |= ~UINT64_C(0) << 32;
      }
      break;
    case UXTX:
    case SXTX:
      break;
    default:
      VIXL_UNREACHABLE();
  }
  return ShiftOperand(reg_size, value, LSL, left_shift);
}


void Simulator::FPCompare(double val0, double val1, FPTrapFlags trap) {
  AssertSupportedFPCR();

  // TODO: This assumes that the C++ implementation handles comparisons in the
  // way that we expect (as per AssertSupportedFPCR()).
  bool process_exception = false;
  if ((IsNaN(val0) != 0) || (IsNaN(val1) != 0)) {
    ReadNzcv().SetRawValue(FPUnorderedFlag);
    if (IsSignallingNaN(val0) || IsSignallingNaN(val1) ||
        (trap == EnableTrap)) {
      process_exception = true;
    }
  } else if (val0 < val1) {
    ReadNzcv().SetRawValue(FPLessThanFlag);
  } else if (val0 > val1) {
    ReadNzcv().SetRawValue(FPGreaterThanFlag);
  } else if (val0 == val1) {
    ReadNzcv().SetRawValue(FPEqualFlag);
  } else {
    VIXL_UNREACHABLE();
  }
  LogSystemRegister(NZCV);
  if (process_exception) FPProcessException();
}


uint64_t Simulator::ComputeMemOperandAddress(const MemOperand& mem_op) const {
  VIXL_ASSERT(mem_op.IsValid());
  int64_t base = ReadRegister<int64_t>(mem_op.GetBaseRegister());
  if (mem_op.IsImmediateOffset()) {
    return base + mem_op.GetOffset();
  } else {
    VIXL_ASSERT(mem_op.GetRegisterOffset().IsValid());
    int64_t offset = ReadRegister<int64_t>(mem_op.GetRegisterOffset());
    unsigned shift_amount = mem_op.GetShiftAmount();
    if (mem_op.GetShift() != NO_SHIFT) {
      offset = ShiftOperand(kXRegSize, offset, mem_op.GetShift(), shift_amount);
    }
    if (mem_op.GetExtend() != NO_EXTEND) {
      offset = ExtendValue(kXRegSize, offset, mem_op.GetExtend(), shift_amount);
    }
    return static_cast<uint64_t>(base + offset);
  }
}


Simulator::PrintRegisterFormat Simulator::GetPrintRegisterFormatForSize(
    unsigned reg_size, unsigned lane_size) {
  VIXL_ASSERT(reg_size >= lane_size);

  uint32_t format = 0;
  if (reg_size != lane_size) {
    switch (reg_size) {
      default:
        VIXL_UNREACHABLE();
        break;
      case kQRegSizeInBytes:
        format = kPrintRegAsQVector;
        break;
      case kDRegSizeInBytes:
        format = kPrintRegAsDVector;
        break;
    }
  }

  switch (lane_size) {
    default:
      VIXL_UNREACHABLE();
      break;
    case kQRegSizeInBytes:
      format |= kPrintReg1Q;
      break;
    case kDRegSizeInBytes:
      format |= kPrintReg1D;
      break;
    case kSRegSizeInBytes:
      format |= kPrintReg1S;
      break;
    case kHRegSizeInBytes:
      format |= kPrintReg1H;
      break;
    case kBRegSizeInBytes:
      format |= kPrintReg1B;
      break;
  }
  // These sizes would be duplicate case labels.
  VIXL_STATIC_ASSERT(kXRegSizeInBytes == kDRegSizeInBytes);
  VIXL_STATIC_ASSERT(kWRegSizeInBytes == kSRegSizeInBytes);
  VIXL_STATIC_ASSERT(kPrintXReg == kPrintReg1D);
  VIXL_STATIC_ASSERT(kPrintWReg == kPrintReg1S);

  return static_cast<PrintRegisterFormat>(format);
}


Simulator::PrintRegisterFormat Simulator::GetPrintRegisterFormat(
    VectorFormat vform) {
  switch (vform) {
    default:
      VIXL_UNREACHABLE();
      return kPrintReg16B;
    case kFormat16B:
      return kPrintReg16B;
    case kFormat8B:
      return kPrintReg8B;
    case kFormat8H:
      return kPrintReg8H;
    case kFormat4H:
      return kPrintReg4H;
    case kFormat4S:
      return kPrintReg4S;
    case kFormat2S:
      return kPrintReg2S;
    case kFormat2D:
      return kPrintReg2D;
    case kFormat1D:
      return kPrintReg1D;

    case kFormatB:
      return kPrintReg1B;
    case kFormatH:
      return kPrintReg1H;
    case kFormatS:
      return kPrintReg1S;
    case kFormatD:
      return kPrintReg1D;
  }
}


Simulator::PrintRegisterFormat Simulator::GetPrintRegisterFormatFP(
    VectorFormat vform) {
  switch (vform) {
    default:
      VIXL_UNREACHABLE();
      return kPrintReg16B;
    case kFormat8H:
      return kPrintReg8HFP;
    case kFormat4H:
      return kPrintReg4HFP;
    case kFormat4S:
      return kPrintReg4SFP;
    case kFormat2S:
      return kPrintReg2SFP;
    case kFormat2D:
      return kPrintReg2DFP;
    case kFormat1D:
      return kPrintReg1DFP;
    case kFormatH:
      return kPrintReg1HFP;
    case kFormatS:
      return kPrintReg1SFP;
    case kFormatD:
      return kPrintReg1DFP;
  }
}


void Simulator::PrintWrittenRegisters() {
  for (unsigned i = 0; i < kNumberOfRegisters; i++) {
    if (registers_[i].WrittenSinceLastLog()) PrintRegister(i);
  }
}


void Simulator::PrintWrittenVRegisters() {
  bool has_sve = GetCPUFeatures()->Has(CPUFeatures::kSVE);
  for (unsigned i = 0; i < kNumberOfVRegisters; i++) {
    // At this point there is no type information, so print as a raw 1Q.
    if (vregisters_[i].WrittenSinceLastLog()) {
      // Z registers are initialised in the constructor before the user can
      // configure the CPU features, so we must also check for SVE here.
      if (vregisters_[i].AccessedAsZSinceLastLog() && has_sve) {
        PrintZRegister(i);
      } else {
        PrintVRegister(i, kPrintReg1Q);
      }
    }
  }
}


void Simulator::PrintWrittenPRegisters() {
  // P registers are initialised in the constructor before the user can
  // configure the CPU features, so we must check for SVE here.
  if (!GetCPUFeatures()->Has(CPUFeatures::kSVE)) return;
  for (unsigned i = 0; i < kNumberOfPRegisters; i++) {
    if (pregisters_[i].WrittenSinceLastLog()) {
      PrintPRegister(i);
    }
  }
}


void Simulator::PrintSystemRegisters() {
  PrintSystemRegister(NZCV);
  PrintSystemRegister(FPCR);
}


void Simulator::PrintRegisters() {
  for (unsigned i = 0; i < kNumberOfRegisters; i++) {
    PrintRegister(i);
  }
}


void Simulator::PrintVRegisters() {
  for (unsigned i = 0; i < kNumberOfVRegisters; i++) {
    // At this point there is no type information, so print as a raw 1Q.
    PrintVRegister(i, kPrintReg1Q);
  }
}


void Simulator::PrintZRegisters() {
  for (unsigned i = 0; i < kNumberOfZRegisters; i++) {
    PrintZRegister(i);
  }
}


// Print a register's name and raw value.
//
// Only the least-significant `size_in_bytes` bytes of the register are printed,
// but the value is aligned as if the whole register had been printed.
//
// For typical register updates, size_in_bytes should be set to kXRegSizeInBytes
// -- the default -- so that the whole register is printed. Other values of
// size_in_bytes are intended for use when the register hasn't actually been
// updated (such as in PrintWrite).
//
// No newline is printed. This allows the caller to print more details (such as
// a memory access annotation).
void Simulator::PrintRegisterRawHelper(unsigned code,
                                       Reg31Mode r31mode,
                                       int size_in_bytes) {
  // The template for all supported sizes.
  //   "# x{code}: 0xffeeddccbbaa9988"
  //   "# w{code}:         0xbbaa9988"
  //   "# w{code}<15:0>:       0x9988"
  //   "# w{code}<7:0>:          0x88"
  unsigned padding_chars = (kXRegSizeInBytes - size_in_bytes) * 2;

  const char* name = "";
  const char* suffix = "";
  switch (size_in_bytes) {
    case kXRegSizeInBytes:
      name = XRegNameForCode(code, r31mode);
      break;
    case kWRegSizeInBytes:
      name = WRegNameForCode(code, r31mode);
      break;
    case 2:
      name = WRegNameForCode(code, r31mode);
      suffix = "<15:0>";
      padding_chars -= strlen(suffix);
      break;
    case 1:
      name = WRegNameForCode(code, r31mode);
      suffix = "<7:0>";
      padding_chars -= strlen(suffix);
      break;
    default:
      VIXL_UNREACHABLE();
  }
  fprintf(stream_, "# %s%5s%s: ", clr_reg_name, name, suffix);

  // Print leading padding spaces.
  VIXL_ASSERT(padding_chars < (kXRegSizeInBytes * 2));
  for (unsigned i = 0; i < padding_chars; i++) {
    putc(' ', stream_);
  }

  // Print the specified bits in hexadecimal format.
  uint64_t bits = ReadRegister<uint64_t>(code, r31mode);
  bits &= kXRegMask >> ((kXRegSizeInBytes - size_in_bytes) * 8);
  VIXL_STATIC_ASSERT(sizeof(bits) == kXRegSizeInBytes);

  int chars = size_in_bytes * 2;
  fprintf(stream_,
          "%s0x%0*" PRIx64 "%s",
          clr_reg_value,
          chars,
          bits,
          clr_normal);
}


void Simulator::PrintRegister(unsigned code, Reg31Mode r31mode) {
  registers_[code].NotifyRegisterLogged();

  // Don't print writes into xzr.
  if ((code == kZeroRegCode) && (r31mode == Reg31IsZeroRegister)) {
    return;
  }

  // The template for all x and w registers:
  //   "# x{code}: 0x{value}"
  //   "# w{code}: 0x{value}"

  PrintRegisterRawHelper(code, r31mode);
  fprintf(stream_, "\n");
}


// Print a register's name and raw value.
//
// The `bytes` and `lsb` arguments can be used to limit the bytes that are
// printed. These arguments are intended for use in cases where register hasn't
// actually been updated (such as in PrintVWrite).
//
// No newline is printed. This allows the caller to print more details (such as
// a floating-point interpretation or a memory access annotation).
void Simulator::PrintVRegisterRawHelper(unsigned code, int bytes, int lsb) {
  // The template for vector types:
  //   "# v{code}: 0xffeeddccbbaa99887766554433221100".
  // An example with bytes=4 and lsb=8:
  //   "# v{code}:         0xbbaa9988                ".
  fprintf(stream_,
          "# %s%13s: %s",
          clr_vreg_name,
          VRegNameForCode(code),
          clr_vreg_value);

  int msb = lsb + bytes - 1;
  int byte = kQRegSizeInBytes - 1;

  // Print leading padding spaces. (Two spaces per byte.)
  while (byte > msb) {
    fprintf(stream_, "  ");
    byte--;
  }

  // Print the specified part of the value, byte by byte.
  qreg_t rawbits = ReadQRegister(code);
  fprintf(stream_, "0x");
  while (byte >= lsb) {
    fprintf(stream_, "%02x", rawbits.val[byte]);
    byte--;
  }

  // Print trailing padding spaces.
  while (byte >= 0) {
    fprintf(stream_, "  ");
    byte--;
  }
  fprintf(stream_, "%s", clr_normal);
}


void Simulator::PrintZRegisterRawHelper(
    unsigned code, int lane_size, int data_size, int bytes, int start_byte) {
  VIXL_ASSERT(lane_size >= data_size);
  // Currently only support printing of 128-bit length value and it must have
  // 128-bit alignement.
  VIXL_ASSERT((bytes % kQRegSizeInBytes) == 0);
  VIXL_ASSERT((start_byte % kQRegSizeInBytes) == 0);

  // The template for vector types:
  //   "# z{code}<m+127:m>: 0x33333333222222221111111100000000",
  // where m is multiple of 128b.
  // An example with bytes=16 starting from a bit 128:
  //   "# z{code}<255:128>: 0x77777777666666665555555544444444".
  // A qlane from a bit zero with lane=4, data=2, and bytes=16:
  //   "#   z{code}<127:0>: 0x    3333    2222    1111    0000".

  std::stringstream prefix;
  prefix << ZRegNameForCode(code) << "<"
         << ((start_byte + bytes) * kBitsPerByte) - 1 << ":"
         << (start_byte * kBitsPerByte) << ">";

  fprintf(stream_,
          "# %s%13s: %s0x",
          clr_vreg_name,
          prefix.str().c_str(),
          clr_vreg_value);

  // Print the 128-bit length of register, lane by lane.
  for (int i = kQRegSizeInBytes / lane_size; i > 0; i--) {
    VIXL_ASSERT((kQRegSizeInBytes % lane_size) == 0);
    // Skip the irrelevant part of value from lane if any.
    for (int skips = lane_size - data_size; skips > 0; skips--) {
      fprintf(stream_, "  ");
      bytes--;
    }

    // [`first_byte`, `last_byte`] represent the interval of bytes that are
    // printed in each lane.
    int last_byte = start_byte + bytes - 1;
    int first_byte = last_byte - data_size + 1;
    // Print the specified part of the value, byte by byte.
    int lane_idx = last_byte >> kQRegSizeInBytesLog2;
    qreg_t rawbits = vregisters_[code].GetLane<qreg_t>(lane_idx);
    for (int byte = last_byte; byte >= first_byte; --byte) {
      fprintf(stream_, "%02x", rawbits.val[byte % kQRegSizeInBytes]);
      bytes--;
    }
  }
  fprintf(stream_, "%s", clr_normal);
}


void Simulator::PrintPRegisterRawHelper(unsigned code, int lsb) {
  // There are no predicated store-predicate instructions, so we can always
  // print the full register, but this helper prints a single run of 16 bits
  // (from `lsb`).
  VIXL_ASSERT(code < kNumberOfPRegisters);
  int bits = kQRegSize / kZRegBitsPerPRegBit;
  int msb = lsb + bits - 1;
  VIXL_ASSERT(static_cast<unsigned>(msb) < pregisters_[code].GetSizeInBits());
  VIXL_ASSERT((lsb % bits) == 0);

  // The template for P registers:
  //   "# p{code}<m+15:m>:  0b 0 0 0 0 1 0 1 1 0 1 1 1 0 1 0 0",
  // where m is multiple of 16.

  // Each printed bit aligns with the least-significant nibble of the
  // corresponding Z-register lane, to make predicate behaviour easy to follow.

  std::stringstream prefix;
  prefix << PRegNameForCode(code) << "<" << msb << ":" << lsb << ">";

  fprintf(stream_,
          "# %s%13s: %s0b",
          clr_preg_name,
          prefix.str().c_str(),
          clr_preg_value);

  // Print the 16-bit length of register, lane by lane.
  for (int i = msb; i >= lsb; i--) {
    fprintf(stream_, " %c", pregisters_[code].GetBit(i) ? '1' : '0');
  }
  fprintf(stream_, "%s", clr_normal);
}


// Print each of the specified lanes of a register as a float or double value.
//
// The `lane_count` and `lslane` arguments can be used to limit the lanes that
// are printed. These arguments are intended for use in cases where register
// hasn't actually been updated (such as in PrintVWrite).
//
// No newline is printed. This allows the caller to print more details (such as
// a memory access annotation).
void Simulator::PrintVRegisterFPHelper(unsigned code,
                                       unsigned lane_size_in_bytes,
                                       int lane_count,
                                       int rightmost_lane) {
  VIXL_ASSERT((lane_size_in_bytes == kHRegSizeInBytes) ||
              (lane_size_in_bytes == kSRegSizeInBytes) ||
              (lane_size_in_bytes == kDRegSizeInBytes));

  unsigned msb = ((lane_count + rightmost_lane) * lane_size_in_bytes);
  VIXL_ASSERT(msb <= kQRegSizeInBytes);

  // For scalar types ((lane_count == 1) && (rightmost_lane == 0)), a register
  // name is used:
  //   " (h{code}: {value})"
  //   " (s{code}: {value})"
  //   " (d{code}: {value})"
  // For vector types, "..." is used to represent one or more omitted lanes.
  //   " (..., {value}, {value}, ...)"
  if (lane_size_in_bytes == kHRegSizeInBytes) {
    // TODO: Trace tests will fail until we regenerate them.
    return;
  }
  if ((lane_count == 1) && (rightmost_lane == 0)) {
    const char* name;
    switch (lane_size_in_bytes) {
      case kHRegSizeInBytes:
        name = HRegNameForCode(code);
        break;
      case kSRegSizeInBytes:
        name = SRegNameForCode(code);
        break;
      case kDRegSizeInBytes:
        name = DRegNameForCode(code);
        break;
      default:
        name = NULL;
        VIXL_UNREACHABLE();
    }
    fprintf(stream_, " (%s%s: ", clr_vreg_name, name);
  } else {
    if (msb < (kQRegSizeInBytes - 1)) {
      fprintf(stream_, " (..., ");
    } else {
      fprintf(stream_, " (");
    }
  }

  // Print the list of values.
  const char* separator = "";
  int leftmost_lane = rightmost_lane + lane_count - 1;
  for (int lane = leftmost_lane; lane >= rightmost_lane; lane--) {
    double value;
    switch (lane_size_in_bytes) {
      case kHRegSizeInBytes:
        value = ReadVRegister(code).GetLane<uint16_t>(lane);
        break;
      case kSRegSizeInBytes:
        value = ReadVRegister(code).GetLane<float>(lane);
        break;
      case kDRegSizeInBytes:
        value = ReadVRegister(code).GetLane<double>(lane);
        break;
      default:
        value = 0.0;
        VIXL_UNREACHABLE();
    }
    if (IsNaN(value)) {
      // The output for NaNs is implementation defined. Always print `nan`, so
      // that traces are coherent across different implementations.
      fprintf(stream_, "%s%snan%s", separator, clr_vreg_value, clr_normal);
    } else {
      fprintf(stream_,
              "%s%s%#g%s",
              separator,
              clr_vreg_value,
              value,
              clr_normal);
    }
    separator = ", ";
  }

  if (rightmost_lane > 0) {
    fprintf(stream_, ", ...");
  }
  fprintf(stream_, ")");
}


void Simulator::PrintVRegister(unsigned code, PrintRegisterFormat format) {
  vregisters_[code].NotifyRegisterLogged();

  int lane_size_log2 = format & kPrintRegLaneSizeMask;

  int reg_size_log2;
  if (format & kPrintRegAsQVector) {
    reg_size_log2 = kQRegSizeInBytesLog2;
  } else if (format & kPrintRegAsDVector) {
    reg_size_log2 = kDRegSizeInBytesLog2;
  } else {
    // Scalar types.
    reg_size_log2 = lane_size_log2;
  }

  int lane_count = 1 << (reg_size_log2 - lane_size_log2);
  int lane_size = 1 << lane_size_log2;

  // The template for vector types:
  //   "# v{code}: 0x{rawbits} (..., {value}, ...)".
  // The template for scalar types:
  //   "# v{code}: 0x{rawbits} ({reg}:{value})".
  // The values in parentheses after the bit representations are floating-point
  // interpretations. They are displayed only if the kPrintVRegAsFP bit is set.

  PrintVRegisterRawHelper(code);
  if (format & kPrintRegAsFP) {
    PrintVRegisterFPHelper(code, lane_size, lane_count);
  }

  fprintf(stream_, "\n");
}

void Simulator::PrintZRegister(unsigned code,
                               PrintRegisterFormat format,
                               int bytes,
                               int start_byte) {
  vregisters_[code].NotifyRegisterLogged();
  if (bytes == 0) {
    // If no byte size specified, print the whole length of register.
    bytes = GetVectorLengthInBytes();
  }

  int lane_size;
  switch (format) {
    case kPrintRegLaneSizeUnknown:
      // If no lane size specified, set to 128-bit lane by default.
      lane_size = kQRegSizeInBytes;
      break;
    case kPrintRegLaneSizeB:
    case kPrintRegLaneSizeH:
    case kPrintRegLaneSizeS:
    case kPrintRegLaneSizeD:
      lane_size = GetPrintRegLaneSizeInBytes(format);
      break;
    default:
      lane_size = 0;
      VIXL_UNIMPLEMENTED();
      break;
  }

  while (bytes > 0) {
    PrintZRegisterRawHelper(code,
                            lane_size,
                            lane_size,
                            kQRegSizeInBytes,
                            start_byte + bytes - kQRegSizeInBytes);
    bytes -= kQRegSizeInBytes;
    fprintf(stream_, "\n");
  }
}

void Simulator::PrintPRegister(unsigned code, PrintRegisterFormat format) {
  USE(format);
  pregisters_[code].NotifyRegisterLogged();
  // There are no predicated store-predicate instructions, so we can simply
  // print the full register.
  int bits_per_chunk = kQRegSize / kZRegBitsPerPRegBit;
  int bits = pregisters_[code].GetSizeInBits();
  for (int lsb = bits - bits_per_chunk; lsb >= 0; lsb -= bits_per_chunk) {
    PrintPRegisterRawHelper(code, lsb);
    fprintf(stream_, "\n");
  }
}

void Simulator::PrintSystemRegister(SystemRegister id) {
  switch (id) {
    case NZCV:
      fprintf(stream_,
              "# %sNZCV: %sN:%d Z:%d C:%d V:%d%s\n",
              clr_flag_name,
              clr_flag_value,
              ReadNzcv().GetN(),
              ReadNzcv().GetZ(),
              ReadNzcv().GetC(),
              ReadNzcv().GetV(),
              clr_normal);
      break;
    case FPCR: {
      static const char* rmode[] = {"0b00 (Round to Nearest)",
                                    "0b01 (Round towards Plus Infinity)",
                                    "0b10 (Round towards Minus Infinity)",
                                    "0b11 (Round towards Zero)"};
      VIXL_ASSERT(ReadFpcr().GetRMode() < ArrayLength(rmode));
      fprintf(stream_,
              "# %sFPCR: %sAHP:%d DN:%d FZ:%d RMode:%s%s\n",
              clr_flag_name,
              clr_flag_value,
              ReadFpcr().GetAHP(),
              ReadFpcr().GetDN(),
              ReadFpcr().GetFZ(),
              rmode[ReadFpcr().GetRMode()],
              clr_normal);
      break;
    }
    default:
      VIXL_UNREACHABLE();
  }
}


void Simulator::PrintRead(uintptr_t address,
                          unsigned reg_code,
                          PrintRegisterFormat format) {
  registers_[reg_code].NotifyRegisterLogged();

  USE(format);

  // The template is "# {reg}: 0x{value} <- {address}".
  PrintRegisterRawHelper(reg_code, Reg31IsZeroRegister);
  fprintf(stream_,
          " <- %s0x%016" PRIxPTR "%s\n",
          clr_memory_address,
          address,
          clr_normal);
}


void Simulator::PrintVRead(uintptr_t address,
                           unsigned reg_code,
                           PrintRegisterFormat format,
                           unsigned lane) {
  vregisters_[reg_code].NotifyRegisterLogged();

  // The template is "# v{code}: 0x{rawbits} <- address".
  PrintVRegisterRawHelper(reg_code);
  if (format & kPrintRegAsFP) {
    PrintVRegisterFPHelper(reg_code,
                           GetPrintRegLaneSizeInBytes(format),
                           GetPrintRegLaneCount(format),
                           lane);
  }
  fprintf(stream_,
          " <- %s0x%016" PRIxPTR "%s\n",
          clr_memory_address,
          address,
          clr_normal);
}

void Simulator::PrintZRead(uintptr_t address,
                           unsigned reg_code,
                           PrintRegisterFormat format,
                           unsigned data_size,
                           int bytes,
                           int start_byte) {
  vregisters_[reg_code].NotifyRegisterLogged();

  // The templates:
  //   "# v{code}<m:n>: 0x{rawbits} <- {address}"
  // An example that prints an unpredicated memory read from a particular memory
  // location to the specified portion of Z register.
  // 0x00007fff00000000: 0x11110000 0x33332222 0x55554444 0x77776666
  // 0x00007fff00000010: 0x99998888 0xbbbbaaaa 0xddddcccc 0xffffeeee
  // The corresponding output is:
  // Zt<255:128>: 0x77776666555544443333222211110000 <- 0x00007fff00000000
  // Zt<383:256>: 0xffffeeeeddddccccbbbbaaaa99998888 <- 0x00007fff00000010

  int lane_size = GetPrintRegLaneSizeInBytes(format);
  if (data_size == 0) {
    // Let the full lane of value are relevent.
    data_size = lane_size;
  }
  if (bytes == 0) {
    // If no byte size specified, print the whole length of register.
    bytes = GetVectorLengthInBytes();
  }

  const int last_byte = start_byte + bytes - 1;
  while (start_byte < last_byte) {
    PrintZRegisterRawHelper(reg_code,
                            lane_size,
                            data_size,
                            kQRegSizeInBytes,
                            start_byte);
    fprintf(stream_,
            " <- %s0x%016" PRIxPTR "%s\n",
            clr_memory_address,
            address,
            clr_normal);
    start_byte += kQRegSizeInBytes;
    address += kQRegSizeInBytes;
  }
}

void Simulator::PrintWrite(uintptr_t address,
                           unsigned reg_code,
                           PrintRegisterFormat format) {
  VIXL_ASSERT(GetPrintRegLaneCount(format) == 1);

  // The template is "# v{code}: 0x{value} -> {address}". To keep the trace tidy
  // and readable, the value is aligned with the values in the register trace.
  PrintRegisterRawHelper(reg_code,
                         Reg31IsZeroRegister,
                         GetPrintRegSizeInBytes(format));
  fprintf(stream_,
          " -> %s0x%016" PRIxPTR "%s\n",
          clr_memory_address,
          address,
          clr_normal);
}


void Simulator::PrintVWrite(uintptr_t address,
                            unsigned reg_code,
                            PrintRegisterFormat format,
                            unsigned lane) {
  // The templates:
  //   "# v{code}: 0x{rawbits} -> {address}"
  //   "# v{code}: 0x{rawbits} (..., {value}, ...) -> {address}".
  //   "# v{code}: 0x{rawbits} ({reg}:{value}) -> {address}"
  // Because this trace doesn't represent a change to the source register's
  // value, only the relevant part of the value is printed. To keep the trace
  // tidy and readable, the raw value is aligned with the other values in the
  // register trace.
  int lane_count = GetPrintRegLaneCount(format);
  int lane_size = GetPrintRegLaneSizeInBytes(format);
  int reg_size = GetPrintRegSizeInBytes(format);
  PrintVRegisterRawHelper(reg_code, reg_size, lane_size * lane);
  if (format & kPrintRegAsFP) {
    PrintVRegisterFPHelper(reg_code, lane_size, lane_count, lane);
  }
  fprintf(stream_,
          " -> %s0x%016" PRIxPTR "%s\n",
          clr_memory_address,
          address,
          clr_normal);
}

void Simulator::PrintZWrite(uintptr_t address,
                            unsigned reg_code,
                            PrintRegisterFormat format,
                            unsigned data_size,
                            int bytes,
                            int start_byte) {
  // The templates:
  //   "# v{code}<m:n>: 0x{rawbits} -> {address}"
  // An example that prints an unpredicated memory write from the specified
  // portion of Z register to a particular memory location.
  // Zt<255:128>: 0x77776666555544443333222211110000 -> 0x00007fff00000000
  // Zt<383:256>: 0xffffeeeeddddccccbbbbaaaa99998888 -> 0x00007fff00000010

  int lane_size = GetPrintRegLaneSizeInBytes(format);
  if (data_size == 0) {
    // If no data size was specified, print the whole of each lane.
    data_size = lane_size;
  }
  if (bytes == 0) {
    // If no byte size was specified, print the whole register.
    bytes = GetVectorLengthInBytes();
  }

  const int last_byte = start_byte + bytes - 1;
  while (start_byte < last_byte) {
    PrintZRegisterRawHelper(reg_code,
                            lane_size,
                            data_size,
                            kQRegSizeInBytes,
                            start_byte);
    fprintf(stream_,
            " -> %s0x%016" PRIxPTR "%s\n",
            clr_memory_address,
            address,
            clr_normal);
    start_byte += kQRegSizeInBytes;
    address += kQRegSizeInBytes;
  }
}


void Simulator::PrintTakenBranch(const Instruction* target) {
  fprintf(stream_,
          "# %sBranch%s to 0x%016" PRIx64 ".\n",
          clr_branch_marker,
          clr_normal,
          reinterpret_cast<uint64_t>(target));
}


// Visitors---------------------------------------------------------------------


void Simulator::VisitReserved(const Instruction* instr) {
  // UDF is the only instruction in this group, and the Decoder is precise here.
  VIXL_ASSERT(instr->Mask(ReservedMask) == UDF);

  printf("UDF (permanently undefined) instruction at %p: 0x%08" PRIx32 "\n",
         reinterpret_cast<const void*>(instr),
         instr->GetInstructionBits());
  VIXL_ABORT_WITH_MSG("UNDEFINED (UDF)\n");
}


void Simulator::VisitUnimplemented(const Instruction* instr) {
  printf("Unimplemented instruction at %p: 0x%08" PRIx32 "\n",
         reinterpret_cast<const void*>(instr),
         instr->GetInstructionBits());
  VIXL_UNIMPLEMENTED();
}


void Simulator::VisitUnallocated(const Instruction* instr) {
  printf("Unallocated instruction at %p: 0x%08" PRIx32 "\n",
         reinterpret_cast<const void*>(instr),
         instr->GetInstructionBits());
  VIXL_UNIMPLEMENTED();
}


void Simulator::VisitPCRelAddressing(const Instruction* instr) {
  VIXL_ASSERT((instr->Mask(PCRelAddressingMask) == ADR) ||
              (instr->Mask(PCRelAddressingMask) == ADRP));

  WriteRegister(instr->GetRd(), instr->GetImmPCOffsetTarget());
}


void Simulator::VisitUnconditionalBranch(const Instruction* instr) {
  switch (instr->Mask(UnconditionalBranchMask)) {
    case BL:
      WriteLr(instr->GetNextInstruction());
      VIXL_FALLTHROUGH();
    case B:
      WritePc(instr->GetImmPCOffsetTarget());
      break;
    default:
      VIXL_UNREACHABLE();
  }
}


void Simulator::VisitConditionalBranch(const Instruction* instr) {
  VIXL_ASSERT(instr->Mask(ConditionalBranchMask) == B_cond);
  if (ConditionPassed(instr->GetConditionBranch())) {
    WritePc(instr->GetImmPCOffsetTarget());
  }
}

BType Simulator::GetBTypeFromInstruction(const Instruction* instr) const {
  switch (instr->Mask(UnconditionalBranchToRegisterMask)) {
    case BLR:
    case BLRAA:
    case BLRAB:
    case BLRAAZ:
    case BLRABZ:
      return BranchAndLink;
    case BR:
    case BRAA:
    case BRAB:
    case BRAAZ:
    case BRABZ:
      if ((instr->GetRn() == 16) || (instr->GetRn() == 17) ||
          !PcIsInGuardedPage()) {
        return BranchFromUnguardedOrToIP;
      }
      return BranchFromGuardedNotToIP;
  }
  return DefaultBType;
}

void Simulator::VisitUnconditionalBranchToRegister(const Instruction* instr) {
  bool authenticate = false;
  bool link = false;
  uint64_t addr = ReadXRegister(instr->GetRn());
  uint64_t context = 0;

  switch (instr->Mask(UnconditionalBranchToRegisterMask)) {
    case BLR:
      link = true;
      VIXL_FALLTHROUGH();
    case BR:
    case RET:
      break;

    case BLRAAZ:
    case BLRABZ:
      link = true;
      VIXL_FALLTHROUGH();
    case BRAAZ:
    case BRABZ:
      authenticate = true;
      break;

    case BLRAA:
    case BLRAB:
      link = true;
      VIXL_FALLTHROUGH();
    case BRAA:
    case BRAB:
      authenticate = true;
      context = ReadXRegister(instr->GetRd());
      break;

    case RETAA:
    case RETAB:
      authenticate = true;
      addr = ReadXRegister(kLinkRegCode);
      context = ReadXRegister(31, Reg31IsStackPointer);
      break;
    default:
      VIXL_UNREACHABLE();
  }

  if (link) {
    WriteLr(instr->GetNextInstruction());
  }

  if (authenticate) {
    PACKey key = (instr->ExtractBit(10) == 0) ? kPACKeyIA : kPACKeyIB;
    addr = AuthPAC(addr, context, key, kInstructionPointer);

    int error_lsb = GetTopPACBit(addr, kInstructionPointer) - 2;
    if (((addr >> error_lsb) & 0x3) != 0x0) {
      VIXL_ABORT_WITH_MSG("Failed to authenticate pointer.");
    }
  }

  WritePc(Instruction::Cast(addr));
  WriteNextBType(GetBTypeFromInstruction(instr));
}


void Simulator::VisitTestBranch(const Instruction* instr) {
  unsigned bit_pos =
      (instr->GetImmTestBranchBit5() << 5) | instr->GetImmTestBranchBit40();
  bool bit_zero = ((ReadXRegister(instr->GetRt()) >> bit_pos) & 1) == 0;
  bool take_branch = false;
  switch (instr->Mask(TestBranchMask)) {
    case TBZ:
      take_branch = bit_zero;
      break;
    case TBNZ:
      take_branch = !bit_zero;
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }
  if (take_branch) {
    WritePc(instr->GetImmPCOffsetTarget());
  }
}


void Simulator::VisitCompareBranch(const Instruction* instr) {
  unsigned rt = instr->GetRt();
  bool take_branch = false;
  switch (instr->Mask(CompareBranchMask)) {
    case CBZ_w:
      take_branch = (ReadWRegister(rt) == 0);
      break;
    case CBZ_x:
      take_branch = (ReadXRegister(rt) == 0);
      break;
    case CBNZ_w:
      take_branch = (ReadWRegister(rt) != 0);
      break;
    case CBNZ_x:
      take_branch = (ReadXRegister(rt) != 0);
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }
  if (take_branch) {
    WritePc(instr->GetImmPCOffsetTarget());
  }
}


void Simulator::AddSubHelper(const Instruction* instr, int64_t op2) {
  unsigned reg_size = instr->GetSixtyFourBits() ? kXRegSize : kWRegSize;
  bool set_flags = instr->GetFlagsUpdate();
  int64_t new_val = 0;
  Instr operation = instr->Mask(AddSubOpMask);

  switch (operation) {
    case ADD:
    case ADDS: {
      new_val = AddWithCarry(reg_size,
                             set_flags,
                             ReadRegister(reg_size,
                                          instr->GetRn(),
                                          instr->GetRnMode()),
                             op2);
      break;
    }
    case SUB:
    case SUBS: {
      new_val = AddWithCarry(reg_size,
                             set_flags,
                             ReadRegister(reg_size,
                                          instr->GetRn(),
                                          instr->GetRnMode()),
                             ~op2,
                             1);
      break;
    }
    default:
      VIXL_UNREACHABLE();
  }

  WriteRegister(reg_size,
                instr->GetRd(),
                new_val,
                LogRegWrites,
                instr->GetRdMode());
}


void Simulator::VisitAddSubShifted(const Instruction* instr) {
  unsigned reg_size = instr->GetSixtyFourBits() ? kXRegSize : kWRegSize;
  int64_t op2 = ShiftOperand(reg_size,
                             ReadRegister(reg_size, instr->GetRm()),
                             static_cast<Shift>(instr->GetShiftDP()),
                             instr->GetImmDPShift());
  AddSubHelper(instr, op2);
}


void Simulator::VisitAddSubImmediate(const Instruction* instr) {
  int64_t op2 = instr->GetImmAddSub()
                << ((instr->GetShiftAddSub() == 1) ? 12 : 0);
  AddSubHelper(instr, op2);
}


void Simulator::VisitAddSubExtended(const Instruction* instr) {
  unsigned reg_size = instr->GetSixtyFourBits() ? kXRegSize : kWRegSize;
  int64_t op2 = ExtendValue(reg_size,
                            ReadRegister(reg_size, instr->GetRm()),
                            static_cast<Extend>(instr->GetExtendMode()),
                            instr->GetImmExtendShift());
  AddSubHelper(instr, op2);
}


void Simulator::VisitAddSubWithCarry(const Instruction* instr) {
  unsigned reg_size = instr->GetSixtyFourBits() ? kXRegSize : kWRegSize;
  int64_t op2 = ReadRegister(reg_size, instr->GetRm());
  int64_t new_val;

  if ((instr->Mask(AddSubOpMask) == SUB) ||
      (instr->Mask(AddSubOpMask) == SUBS)) {
    op2 = ~op2;
  }

  new_val = AddWithCarry(reg_size,
                         instr->GetFlagsUpdate(),
                         ReadRegister(reg_size, instr->GetRn()),
                         op2,
                         ReadC());

  WriteRegister(reg_size, instr->GetRd(), new_val);
}


void Simulator::VisitRotateRightIntoFlags(const Instruction* instr) {
  switch (instr->Mask(RotateRightIntoFlagsMask)) {
    case RMIF: {
      uint64_t value = ReadRegister<uint64_t>(instr->GetRn());
      unsigned shift = instr->GetImmRMIFRotation();
      unsigned mask = instr->GetNzcv();
      uint64_t rotated = RotateRight(value, shift, kXRegSize);

      ReadNzcv().SetFlags((rotated & mask) | (ReadNzcv().GetFlags() & ~mask));
      break;
    }
  }
}


void Simulator::VisitEvaluateIntoFlags(const Instruction* instr) {
  uint32_t value = ReadRegister<uint32_t>(instr->GetRn());
  unsigned msb = (instr->Mask(EvaluateIntoFlagsMask) == SETF16) ? 15 : 7;

  unsigned sign_bit = (value >> msb) & 1;
  unsigned overflow_bit = (value >> (msb + 1)) & 1;
  ReadNzcv().SetN(sign_bit);
  ReadNzcv().SetZ((value << (31 - msb)) == 0);
  ReadNzcv().SetV(sign_bit ^ overflow_bit);
}


void Simulator::VisitLogicalShifted(const Instruction* instr) {
  unsigned reg_size = instr->GetSixtyFourBits() ? kXRegSize : kWRegSize;
  Shift shift_type = static_cast<Shift>(instr->GetShiftDP());
  unsigned shift_amount = instr->GetImmDPShift();
  int64_t op2 = ShiftOperand(reg_size,
                             ReadRegister(reg_size, instr->GetRm()),
                             shift_type,
                             shift_amount);
  if (instr->Mask(NOT) == NOT) {
    op2 = ~op2;
  }
  LogicalHelper(instr, op2);
}


void Simulator::VisitLogicalImmediate(const Instruction* instr) {
  LogicalHelper(instr, instr->GetImmLogical());
}


void Simulator::LogicalHelper(const Instruction* instr, int64_t op2) {
  unsigned reg_size = instr->GetSixtyFourBits() ? kXRegSize : kWRegSize;
  int64_t op1 = ReadRegister(reg_size, instr->GetRn());
  int64_t result = 0;
  bool update_flags = false;

  // Switch on the logical operation, stripping out the NOT bit, as it has a
  // different meaning for logical immediate instructions.
  switch (instr->Mask(LogicalOpMask & ~NOT)) {
    case ANDS:
      update_flags = true;
      VIXL_FALLTHROUGH();
    case AND:
      result = op1 & op2;
      break;
    case ORR:
      result = op1 | op2;
      break;
    case EOR:
      result = op1 ^ op2;
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }

  if (update_flags) {
    ReadNzcv().SetN(CalcNFlag(result, reg_size));
    ReadNzcv().SetZ(CalcZFlag(result));
    ReadNzcv().SetC(0);
    ReadNzcv().SetV(0);
    LogSystemRegister(NZCV);
  }

  WriteRegister(reg_size,
                instr->GetRd(),
                result,
                LogRegWrites,
                instr->GetRdMode());
}


void Simulator::VisitConditionalCompareRegister(const Instruction* instr) {
  unsigned reg_size = instr->GetSixtyFourBits() ? kXRegSize : kWRegSize;
  ConditionalCompareHelper(instr, ReadRegister(reg_size, instr->GetRm()));
}


void Simulator::VisitConditionalCompareImmediate(const Instruction* instr) {
  ConditionalCompareHelper(instr, instr->GetImmCondCmp());
}


void Simulator::ConditionalCompareHelper(const Instruction* instr,
                                         int64_t op2) {
  unsigned reg_size = instr->GetSixtyFourBits() ? kXRegSize : kWRegSize;
  int64_t op1 = ReadRegister(reg_size, instr->GetRn());

  if (ConditionPassed(instr->GetCondition())) {
    // If the condition passes, set the status flags to the result of comparing
    // the operands.
    if (instr->Mask(ConditionalCompareMask) == CCMP) {
      AddWithCarry(reg_size, true, op1, ~op2, 1);
    } else {
      VIXL_ASSERT(instr->Mask(ConditionalCompareMask) == CCMN);
      AddWithCarry(reg_size, true, op1, op2, 0);
    }
  } else {
    // If the condition fails, set the status flags to the nzcv immediate.
    ReadNzcv().SetFlags(instr->GetNzcv());
    LogSystemRegister(NZCV);
  }
}


void Simulator::VisitLoadStoreUnsignedOffset(const Instruction* instr) {
  int offset = instr->GetImmLSUnsigned() << instr->GetSizeLS();
  LoadStoreHelper(instr, offset, Offset);
}


void Simulator::VisitLoadStoreUnscaledOffset(const Instruction* instr) {
  LoadStoreHelper(instr, instr->GetImmLS(), Offset);
}


void Simulator::VisitLoadStorePreIndex(const Instruction* instr) {
  LoadStoreHelper(instr, instr->GetImmLS(), PreIndex);
}


void Simulator::VisitLoadStorePostIndex(const Instruction* instr) {
  LoadStoreHelper(instr, instr->GetImmLS(), PostIndex);
}


template <typename T1, typename T2>
void Simulator::LoadAcquireRCpcUnscaledOffsetHelper(const Instruction* instr) {
  unsigned rt = instr->GetRt();
  unsigned rn = instr->GetRn();

  unsigned element_size = sizeof(T2);
  uint64_t address = ReadRegister<uint64_t>(rn, Reg31IsStackPointer);
  int offset = instr->GetImmLS();
  address += offset;

  // Verify that the address is available to the host.
  VIXL_ASSERT(address == static_cast<uintptr_t>(address));

  // Check the alignment of `address`.
  if (AlignDown(address, 16) != AlignDown(address + element_size - 1, 16)) {
    VIXL_ALIGNMENT_EXCEPTION();
  }

  WriteRegister<T1>(rt, static_cast<T1>(Memory::Read<T2>(address)));

  // Approximate load-acquire by issuing a full barrier after the load.
  __sync_synchronize();

  LogRead(address, rt, GetPrintRegisterFormat(element_size));
}


template <typename T>
void Simulator::StoreReleaseUnscaledOffsetHelper(const Instruction* instr) {
  unsigned rt = instr->GetRt();
  unsigned rn = instr->GetRn();

  unsigned element_size = sizeof(T);
  uint64_t address = ReadRegister<uint64_t>(rn, Reg31IsStackPointer);
  int offset = instr->GetImmLS();
  address += offset;

  // Verify that the address is available to the host.
  VIXL_ASSERT(address == static_cast<uintptr_t>(address));

  // Check the alignment of `address`.
  if (AlignDown(address, 16) != AlignDown(address + element_size - 1, 16)) {
    VIXL_ALIGNMENT_EXCEPTION();
  }

  // Approximate store-release by issuing a full barrier after the load.
  __sync_synchronize();

  Memory::Write<T>(address, ReadRegister<T>(rt));

  LogWrite(address, rt, GetPrintRegisterFormat(element_size));
}


void Simulator::VisitLoadStoreRCpcUnscaledOffset(const Instruction* instr) {
  switch (instr->Mask(LoadStoreRCpcUnscaledOffsetMask)) {
    case LDAPURB:
      LoadAcquireRCpcUnscaledOffsetHelper<uint8_t, uint8_t>(instr);
      break;
    case LDAPURH:
      LoadAcquireRCpcUnscaledOffsetHelper<uint16_t, uint16_t>(instr);
      break;
    case LDAPUR_w:
      LoadAcquireRCpcUnscaledOffsetHelper<uint32_t, uint32_t>(instr);
      break;
    case LDAPUR_x:
      LoadAcquireRCpcUnscaledOffsetHelper<uint64_t, uint64_t>(instr);
      break;
    case LDAPURSB_w:
      LoadAcquireRCpcUnscaledOffsetHelper<int32_t, int8_t>(instr);
      break;
    case LDAPURSB_x:
      LoadAcquireRCpcUnscaledOffsetHelper<int64_t, int8_t>(instr);
      break;
    case LDAPURSH_w:
      LoadAcquireRCpcUnscaledOffsetHelper<int32_t, int16_t>(instr);
      break;
    case LDAPURSH_x:
      LoadAcquireRCpcUnscaledOffsetHelper<int64_t, int16_t>(instr);
      break;
    case LDAPURSW:
      LoadAcquireRCpcUnscaledOffsetHelper<int64_t, int32_t>(instr);
      break;
    case STLURB:
      StoreReleaseUnscaledOffsetHelper<uint8_t>(instr);
      break;
    case STLURH:
      StoreReleaseUnscaledOffsetHelper<uint16_t>(instr);
      break;
    case STLUR_w:
      StoreReleaseUnscaledOffsetHelper<uint32_t>(instr);
      break;
    case STLUR_x:
      StoreReleaseUnscaledOffsetHelper<uint64_t>(instr);
      break;
  }
}


void Simulator::VisitLoadStorePAC(const Instruction* instr) {
  unsigned dst = instr->GetRt();
  unsigned addr_reg = instr->GetRn();

  uint64_t address = ReadXRegister(addr_reg, Reg31IsStackPointer);

  PACKey key = (instr->ExtractBit(23) == 0) ? kPACKeyDA : kPACKeyDB;
  address = AuthPAC(address, 0, key, kDataPointer);

  int error_lsb = GetTopPACBit(address, kInstructionPointer) - 2;
  if (((address >> error_lsb) & 0x3) != 0x0) {
    VIXL_ABORT_WITH_MSG("Failed to authenticate pointer.");
  }


  if ((addr_reg == 31) && ((address % 16) != 0)) {
    // When the base register is SP the stack pointer is required to be
    // quadword aligned prior to the address calculation and write-backs.
    // Misalignment will cause a stack alignment fault.
    VIXL_ALIGNMENT_EXCEPTION();
  }

  int64_t offset = instr->GetImmLSPAC();
  address += offset;

  if (instr->Mask(LoadStorePACPreBit) == LoadStorePACPreBit) {
    // Pre-index mode.
    VIXL_ASSERT(offset != 0);
    WriteXRegister(addr_reg, address, LogRegWrites, Reg31IsStackPointer);
  }

  uintptr_t addr_ptr = static_cast<uintptr_t>(address);

  // Verify that the calculated address is available to the host.
  VIXL_ASSERT(address == addr_ptr);

  WriteXRegister(dst, Memory::Read<uint64_t>(addr_ptr), NoRegLog);
  unsigned access_size = 1 << 3;
  LogRead(addr_ptr, dst, GetPrintRegisterFormatForSize(access_size));
}


void Simulator::VisitLoadStoreRegisterOffset(const Instruction* instr) {
  Extend ext = static_cast<Extend>(instr->GetExtendMode());
  VIXL_ASSERT((ext == UXTW) || (ext == UXTX) || (ext == SXTW) || (ext == SXTX));
  unsigned shift_amount = instr->GetImmShiftLS() * instr->GetSizeLS();

  int64_t offset =
      ExtendValue(kXRegSize, ReadXRegister(instr->GetRm()), ext, shift_amount);
  LoadStoreHelper(instr, offset, Offset);
}


void Simulator::LoadStoreHelper(const Instruction* instr,
                                int64_t offset,
                                AddrMode addrmode) {
  unsigned srcdst = instr->GetRt();
  uintptr_t address = AddressModeHelper(instr->GetRn(), offset, addrmode);

  LoadStoreOp op = static_cast<LoadStoreOp>(instr->Mask(LoadStoreMask));
  switch (op) {
    case LDRB_w:
      WriteWRegister(srcdst, Memory::Read<uint8_t>(address), NoRegLog);
      break;
    case LDRH_w:
      WriteWRegister(srcdst, Memory::Read<uint16_t>(address), NoRegLog);
      break;
    case LDR_w:
      WriteWRegister(srcdst, Memory::Read<uint32_t>(address), NoRegLog);
      break;
    case LDR_x:
      WriteXRegister(srcdst, Memory::Read<uint64_t>(address), NoRegLog);
      break;
    case LDRSB_w:
      WriteWRegister(srcdst, Memory::Read<int8_t>(address), NoRegLog);
      break;
    case LDRSH_w:
      WriteWRegister(srcdst, Memory::Read<int16_t>(address), NoRegLog);
      break;
    case LDRSB_x:
      WriteXRegister(srcdst, Memory::Read<int8_t>(address), NoRegLog);
      break;
    case LDRSH_x:
      WriteXRegister(srcdst, Memory::Read<int16_t>(address), NoRegLog);
      break;
    case LDRSW_x:
      WriteXRegister(srcdst, Memory::Read<int32_t>(address), NoRegLog);
      break;
    case LDR_b:
      WriteBRegister(srcdst, Memory::Read<uint8_t>(address), NoRegLog);
      break;
    case LDR_h:
      WriteHRegister(srcdst, Memory::Read<uint16_t>(address), NoRegLog);
      break;
    case LDR_s:
      WriteSRegister(srcdst, Memory::Read<float>(address), NoRegLog);
      break;
    case LDR_d:
      WriteDRegister(srcdst, Memory::Read<double>(address), NoRegLog);
      break;
    case LDR_q:
      WriteQRegister(srcdst, Memory::Read<qreg_t>(address), NoRegLog);
      break;

    case STRB_w:
      Memory::Write<uint8_t>(address, ReadWRegister(srcdst));
      break;
    case STRH_w:
      Memory::Write<uint16_t>(address, ReadWRegister(srcdst));
      break;
    case STR_w:
      Memory::Write<uint32_t>(address, ReadWRegister(srcdst));
      break;
    case STR_x:
      Memory::Write<uint64_t>(address, ReadXRegister(srcdst));
      break;
    case STR_b:
      Memory::Write<uint8_t>(address, ReadBRegister(srcdst));
      break;
    case STR_h:
      Memory::Write<uint16_t>(address, ReadHRegisterBits(srcdst));
      break;
    case STR_s:
      Memory::Write<float>(address, ReadSRegister(srcdst));
      break;
    case STR_d:
      Memory::Write<double>(address, ReadDRegister(srcdst));
      break;
    case STR_q:
      Memory::Write<qreg_t>(address, ReadQRegister(srcdst));
      break;

    // Ignore prfm hint instructions.
    case PRFM:
      break;

    default:
      VIXL_UNIMPLEMENTED();
  }

  unsigned access_size = 1 << instr->GetSizeLS();
  if (instr->IsLoad()) {
    if ((op == LDR_s) || (op == LDR_d)) {
      LogVRead(address, srcdst, GetPrintRegisterFormatForSizeFP(access_size));
    } else if ((op == LDR_b) || (op == LDR_h) || (op == LDR_q)) {
      LogVRead(address, srcdst, GetPrintRegisterFormatForSize(access_size));
    } else {
      LogRead(address, srcdst, GetPrintRegisterFormatForSize(access_size));
    }
  } else if (instr->IsStore()) {
    if ((op == STR_s) || (op == STR_d)) {
      LogVWrite(address, srcdst, GetPrintRegisterFormatForSizeFP(access_size));
    } else if ((op == STR_b) || (op == STR_h) || (op == STR_q)) {
      LogVWrite(address, srcdst, GetPrintRegisterFormatForSize(access_size));
    } else {
      LogWrite(address, srcdst, GetPrintRegisterFormatForSize(access_size));
    }
  } else {
    VIXL_ASSERT(op == PRFM);
  }

  local_monitor_.MaybeClear();
}


void Simulator::VisitLoadStorePairOffset(const Instruction* instr) {
  LoadStorePairHelper(instr, Offset);
}


void Simulator::VisitLoadStorePairPreIndex(const Instruction* instr) {
  LoadStorePairHelper(instr, PreIndex);
}


void Simulator::VisitLoadStorePairPostIndex(const Instruction* instr) {
  LoadStorePairHelper(instr, PostIndex);
}


void Simulator::VisitLoadStorePairNonTemporal(const Instruction* instr) {
  LoadStorePairHelper(instr, Offset);
}


void Simulator::LoadStorePairHelper(const Instruction* instr,
                                    AddrMode addrmode) {
  unsigned rt = instr->GetRt();
  unsigned rt2 = instr->GetRt2();
  int element_size = 1 << instr->GetSizeLSPair();
  int64_t offset = instr->GetImmLSPair() * element_size;
  uintptr_t address = AddressModeHelper(instr->GetRn(), offset, addrmode);
  uintptr_t address2 = address + element_size;

  LoadStorePairOp op =
      static_cast<LoadStorePairOp>(instr->Mask(LoadStorePairMask));

  // 'rt' and 'rt2' can only be aliased for stores.
  VIXL_ASSERT(((op & LoadStorePairLBit) == 0) || (rt != rt2));

  switch (op) {
    // Use NoRegLog to suppress the register trace (LOG_REGS, LOG_FP_REGS). We
    // will print a more detailed log.
    case LDP_w: {
      WriteWRegister(rt, Memory::Read<uint32_t>(address), NoRegLog);
      WriteWRegister(rt2, Memory::Read<uint32_t>(address2), NoRegLog);
      break;
    }
    case LDP_s: {
      WriteSRegister(rt, Memory::Read<float>(address), NoRegLog);
      WriteSRegister(rt2, Memory::Read<float>(address2), NoRegLog);
      break;
    }
    case LDP_x: {
      WriteXRegister(rt, Memory::Read<uint64_t>(address), NoRegLog);
      WriteXRegister(rt2, Memory::Read<uint64_t>(address2), NoRegLog);
      break;
    }
    case LDP_d: {
      WriteDRegister(rt, Memory::Read<double>(address), NoRegLog);
      WriteDRegister(rt2, Memory::Read<double>(address2), NoRegLog);
      break;
    }
    case LDP_q: {
      WriteQRegister(rt, Memory::Read<qreg_t>(address), NoRegLog);
      WriteQRegister(rt2, Memory::Read<qreg_t>(address2), NoRegLog);
      break;
    }
    case LDPSW_x: {
      WriteXRegister(rt, Memory::Read<int32_t>(address), NoRegLog);
      WriteXRegister(rt2, Memory::Read<int32_t>(address2), NoRegLog);
      break;
    }
    case STP_w: {
      Memory::Write<uint32_t>(address, ReadWRegister(rt));
      Memory::Write<uint32_t>(address2, ReadWRegister(rt2));
      break;
    }
    case STP_s: {
      Memory::Write<float>(address, ReadSRegister(rt));
      Memory::Write<float>(address2, ReadSRegister(rt2));
      break;
    }
    case STP_x: {
      Memory::Write<uint64_t>(address, ReadXRegister(rt));
      Memory::Write<uint64_t>(address2, ReadXRegister(rt2));
      break;
    }
    case STP_d: {
      Memory::Write<double>(address, ReadDRegister(rt));
      Memory::Write<double>(address2, ReadDRegister(rt2));
      break;
    }
    case STP_q: {
      Memory::Write<qreg_t>(address, ReadQRegister(rt));
      Memory::Write<qreg_t>(address2, ReadQRegister(rt2));
      break;
    }
    default:
      VIXL_UNREACHABLE();
  }

  // Print a detailed trace (including the memory address) instead of the basic
  // register:value trace generated by set_*reg().
  if (instr->IsLoad()) {
    if ((op == LDP_s) || (op == LDP_d)) {
      LogVRead(address, rt, GetPrintRegisterFormatForSizeFP(element_size));
      LogVRead(address2, rt2, GetPrintRegisterFormatForSizeFP(element_size));
    } else if (op == LDP_q) {
      LogVRead(address, rt, GetPrintRegisterFormatForSize(element_size));
      LogVRead(address2, rt2, GetPrintRegisterFormatForSize(element_size));
    } else {
      LogRead(address, rt, GetPrintRegisterFormatForSize(element_size));
      LogRead(address2, rt2, GetPrintRegisterFormatForSize(element_size));
    }
  } else {
    if ((op == STP_s) || (op == STP_d)) {
      LogVWrite(address, rt, GetPrintRegisterFormatForSizeFP(element_size));
      LogVWrite(address2, rt2, GetPrintRegisterFormatForSizeFP(element_size));
    } else if (op == STP_q) {
      LogVWrite(address, rt, GetPrintRegisterFormatForSize(element_size));
      LogVWrite(address2, rt2, GetPrintRegisterFormatForSize(element_size));
    } else {
      LogWrite(address, rt, GetPrintRegisterFormatForSize(element_size));
      LogWrite(address2, rt2, GetPrintRegisterFormatForSize(element_size));
    }
  }

  local_monitor_.MaybeClear();
}


template <typename T>
void Simulator::CompareAndSwapHelper(const Instruction* instr) {
  unsigned rs = instr->GetRs();
  unsigned rt = instr->GetRt();
  unsigned rn = instr->GetRn();

  unsigned element_size = sizeof(T);
  uint64_t address = ReadRegister<uint64_t>(rn, Reg31IsStackPointer);

  CheckIsValidUnalignedAtomicAccess(rn, address, element_size);

  bool is_acquire = instr->ExtractBit(22) == 1;
  bool is_release = instr->ExtractBit(15) == 1;

  T comparevalue = ReadRegister<T>(rs);
  T newvalue = ReadRegister<T>(rt);

  // The architecture permits that the data read clears any exclusive monitors
  // associated with that location, even if the compare subsequently fails.
  local_monitor_.Clear();

  T data = Memory::Read<T>(address);
  if (is_acquire) {
    // Approximate load-acquire by issuing a full barrier after the load.
    __sync_synchronize();
  }

  if (data == comparevalue) {
    if (is_release) {
      // Approximate store-release by issuing a full barrier before the store.
      __sync_synchronize();
    }
    Memory::Write<T>(address, newvalue);
    LogWrite(address, rt, GetPrintRegisterFormatForSize(element_size));
  }
  WriteRegister<T>(rs, data);
  LogRead(address, rs, GetPrintRegisterFormatForSize(element_size));
}


template <typename T>
void Simulator::CompareAndSwapPairHelper(const Instruction* instr) {
  VIXL_ASSERT((sizeof(T) == 4) || (sizeof(T) == 8));
  unsigned rs = instr->GetRs();
  unsigned rt = instr->GetRt();
  unsigned rn = instr->GetRn();

  VIXL_ASSERT((rs % 2 == 0) && (rs % 2 == 0));

  unsigned element_size = sizeof(T);
  uint64_t address = ReadRegister<uint64_t>(rn, Reg31IsStackPointer);

  CheckIsValidUnalignedAtomicAccess(rn, address, element_size * 2);

  uint64_t address2 = address + element_size;

  bool is_acquire = instr->ExtractBit(22) == 1;
  bool is_release = instr->ExtractBit(15) == 1;

  T comparevalue_high = ReadRegister<T>(rs + 1);
  T comparevalue_low = ReadRegister<T>(rs);
  T newvalue_high = ReadRegister<T>(rt + 1);
  T newvalue_low = ReadRegister<T>(rt);

  // The architecture permits that the data read clears any exclusive monitors
  // associated with that location, even if the compare subsequently fails.
  local_monitor_.Clear();

  T data_high = Memory::Read<T>(address);
  T data_low = Memory::Read<T>(address2);

  if (is_acquire) {
    // Approximate load-acquire by issuing a full barrier after the load.
    __sync_synchronize();
  }

  bool same =
      (data_high == comparevalue_high) && (data_low == comparevalue_low);
  if (same) {
    if (is_release) {
      // Approximate store-release by issuing a full barrier before the store.
      __sync_synchronize();
    }

    Memory::Write<T>(address, newvalue_high);
    Memory::Write<T>(address2, newvalue_low);
  }

  WriteRegister<T>(rs + 1, data_high);
  WriteRegister<T>(rs, data_low);

  LogRead(address, rs + 1, GetPrintRegisterFormatForSize(element_size));
  LogRead(address2, rs, GetPrintRegisterFormatForSize(element_size));

  if (same) {
    LogWrite(address, rt + 1, GetPrintRegisterFormatForSize(element_size));
    LogWrite(address2, rt, GetPrintRegisterFormatForSize(element_size));
  }
}


void Simulator::PrintExclusiveAccessWarning() {
  if (print_exclusive_access_warning_) {
    fprintf(stderr,
            "%sWARNING:%s VIXL simulator support for "
            "load-/store-/clear-exclusive "
            "instructions is limited. Refer to the README for details.%s\n",
            clr_warning,
            clr_warning_message,
            clr_normal);
    print_exclusive_access_warning_ = false;
  }
}


void Simulator::VisitLoadStoreExclusive(const Instruction* instr) {
  LoadStoreExclusive op =
      static_cast<LoadStoreExclusive>(instr->Mask(LoadStoreExclusiveMask));

  switch (op) {
    case CAS_w:
    case CASA_w:
    case CASL_w:
    case CASAL_w:
      CompareAndSwapHelper<uint32_t>(instr);
      break;
    case CAS_x:
    case CASA_x:
    case CASL_x:
    case CASAL_x:
      CompareAndSwapHelper<uint64_t>(instr);
      break;
    case CASB:
    case CASAB:
    case CASLB:
    case CASALB:
      CompareAndSwapHelper<uint8_t>(instr);
      break;
    case CASH:
    case CASAH:
    case CASLH:
    case CASALH:
      CompareAndSwapHelper<uint16_t>(instr);
      break;
    case CASP_w:
    case CASPA_w:
    case CASPL_w:
    case CASPAL_w:
      CompareAndSwapPairHelper<uint32_t>(instr);
      break;
    case CASP_x:
    case CASPA_x:
    case CASPL_x:
    case CASPAL_x:
      CompareAndSwapPairHelper<uint64_t>(instr);
      break;
    default:
      PrintExclusiveAccessWarning();

      unsigned rs = instr->GetRs();
      unsigned rt = instr->GetRt();
      unsigned rt2 = instr->GetRt2();
      unsigned rn = instr->GetRn();

      bool is_exclusive = !instr->GetLdStXNotExclusive();
      bool is_acquire_release =
          !is_exclusive || instr->GetLdStXAcquireRelease();
      bool is_load = instr->GetLdStXLoad();
      bool is_pair = instr->GetLdStXPair();

      unsigned element_size = 1 << instr->GetLdStXSizeLog2();
      unsigned access_size = is_pair ? element_size * 2 : element_size;
      uint64_t address = ReadRegister<uint64_t>(rn, Reg31IsStackPointer);

      CheckIsValidUnalignedAtomicAccess(rn, address, access_size);

      if (is_load) {
        if (is_exclusive) {
          local_monitor_.MarkExclusive(address, access_size);
        } else {
          // Any non-exclusive load can clear the local monitor as a side
          // effect. We don't need to do this, but it is useful to stress the
          // simulated code.
          local_monitor_.Clear();
        }

        // Use NoRegLog to suppress the register trace (LOG_REGS, LOG_FP_REGS).
        // We will print a more detailed log.
        switch (op) {
          case LDXRB_w:
          case LDAXRB_w:
          case LDARB_w:
          case LDLARB:
            WriteWRegister(rt, Memory::Read<uint8_t>(address), NoRegLog);
            break;
          case LDXRH_w:
          case LDAXRH_w:
          case LDARH_w:
          case LDLARH:
            WriteWRegister(rt, Memory::Read<uint16_t>(address), NoRegLog);
            break;
          case LDXR_w:
          case LDAXR_w:
          case LDAR_w:
          case LDLAR_w:
            WriteWRegister(rt, Memory::Read<uint32_t>(address), NoRegLog);
            break;
          case LDXR_x:
          case LDAXR_x:
          case LDAR_x:
          case LDLAR_x:
            WriteXRegister(rt, Memory::Read<uint64_t>(address), NoRegLog);
            break;
          case LDXP_w:
          case LDAXP_w:
            WriteWRegister(rt, Memory::Read<uint32_t>(address), NoRegLog);
            WriteWRegister(rt2,
                           Memory::Read<uint32_t>(address + element_size),
                           NoRegLog);
            break;
          case LDXP_x:
          case LDAXP_x:
            WriteXRegister(rt, Memory::Read<uint64_t>(address), NoRegLog);
            WriteXRegister(rt2,
                           Memory::Read<uint64_t>(address + element_size),
                           NoRegLog);
            break;
          default:
            VIXL_UNREACHABLE();
        }

        if (is_acquire_release) {
          // Approximate load-acquire by issuing a full barrier after the load.
          __sync_synchronize();
        }

        LogRead(address, rt, GetPrintRegisterFormatForSize(element_size));
        if (is_pair) {
          LogRead(address + element_size,
                  rt2,
                  GetPrintRegisterFormatForSize(element_size));
        }
      } else {
        if (is_acquire_release) {
          // Approximate store-release by issuing a full barrier before the
          // store.
          __sync_synchronize();
        }

        bool do_store = true;
        if (is_exclusive) {
          do_store = local_monitor_.IsExclusive(address, access_size) &&
                     global_monitor_.IsExclusive(address, access_size);
          WriteWRegister(rs, do_store ? 0 : 1);

          //  - All exclusive stores explicitly clear the local monitor.
          local_monitor_.Clear();
        } else {
          //  - Any other store can clear the local monitor as a side effect.
          local_monitor_.MaybeClear();
        }

        if (do_store) {
          switch (op) {
            case STXRB_w:
            case STLXRB_w:
            case STLRB_w:
            case STLLRB:
              Memory::Write<uint8_t>(address, ReadWRegister(rt));
              break;
            case STXRH_w:
            case STLXRH_w:
            case STLRH_w:
            case STLLRH:
              Memory::Write<uint16_t>(address, ReadWRegister(rt));
              break;
            case STXR_w:
            case STLXR_w:
            case STLR_w:
            case STLLR_w:
              Memory::Write<uint32_t>(address, ReadWRegister(rt));
              break;
            case STXR_x:
            case STLXR_x:
            case STLR_x:
            case STLLR_x:
              Memory::Write<uint64_t>(address, ReadXRegister(rt));
              break;
            case STXP_w:
            case STLXP_w:
              Memory::Write<uint32_t>(address, ReadWRegister(rt));
              Memory::Write<uint32_t>(address + element_size,
                                      ReadWRegister(rt2));
              break;
            case STXP_x:
            case STLXP_x:
              Memory::Write<uint64_t>(address, ReadXRegister(rt));
              Memory::Write<uint64_t>(address + element_size,
                                      ReadXRegister(rt2));
              break;
            default:
              VIXL_UNREACHABLE();
          }

          LogWrite(address, rt, GetPrintRegisterFormatForSize(element_size));
          if (is_pair) {
            LogWrite(address + element_size,
                     rt2,
                     GetPrintRegisterFormatForSize(element_size));
          }
        }
      }
  }
}

template <typename T>
void Simulator::AtomicMemorySimpleHelper(const Instruction* instr) {
  unsigned rs = instr->GetRs();
  unsigned rt = instr->GetRt();
  unsigned rn = instr->GetRn();

  bool is_acquire = (instr->ExtractBit(23) == 1) && (rt != kZeroRegCode);
  bool is_release = instr->ExtractBit(22) == 1;

  unsigned element_size = sizeof(T);
  uint64_t address = ReadRegister<uint64_t>(rn, Reg31IsStackPointer);

  CheckIsValidUnalignedAtomicAccess(rn, address, element_size);

  T value = ReadRegister<T>(rs);

  T data = Memory::Read<T>(address);

  if (is_acquire) {
    // Approximate load-acquire by issuing a full barrier after the load.
    __sync_synchronize();
  }

  T result = 0;
  switch (instr->Mask(AtomicMemorySimpleOpMask)) {
    case LDADDOp:
      result = data + value;
      break;
    case LDCLROp:
      VIXL_ASSERT(!std::numeric_limits<T>::is_signed);
      result = data & ~value;
      break;
    case LDEOROp:
      VIXL_ASSERT(!std::numeric_limits<T>::is_signed);
      result = data ^ value;
      break;
    case LDSETOp:
      VIXL_ASSERT(!std::numeric_limits<T>::is_signed);
      result = data | value;
      break;

    // Signed/Unsigned difference is done via the templated type T.
    case LDSMAXOp:
    case LDUMAXOp:
      result = (data > value) ? data : value;
      break;
    case LDSMINOp:
    case LDUMINOp:
      result = (data > value) ? value : data;
      break;
  }

  if (is_release) {
    // Approximate store-release by issuing a full barrier before the store.
    __sync_synchronize();
  }

  Memory::Write<T>(address, result);
  WriteRegister<T>(rt, data, NoRegLog);

  LogRead(address, rt, GetPrintRegisterFormatForSize(element_size));
  LogWrite(address, rs, GetPrintRegisterFormatForSize(element_size));
}

template <typename T>
void Simulator::AtomicMemorySwapHelper(const Instruction* instr) {
  unsigned rs = instr->GetRs();
  unsigned rt = instr->GetRt();
  unsigned rn = instr->GetRn();

  bool is_acquire = (instr->ExtractBit(23) == 1) && (rt != kZeroRegCode);
  bool is_release = instr->ExtractBit(22) == 1;

  unsigned element_size = sizeof(T);
  uint64_t address = ReadRegister<uint64_t>(rn, Reg31IsStackPointer);

  CheckIsValidUnalignedAtomicAccess(rn, address, element_size);

  T data = Memory::Read<T>(address);
  if (is_acquire) {
    // Approximate load-acquire by issuing a full barrier after the load.
    __sync_synchronize();
  }

  if (is_release) {
    // Approximate store-release by issuing a full barrier before the store.
    __sync_synchronize();
  }
  Memory::Write<T>(address, ReadRegister<T>(rs));

  WriteRegister<T>(rt, data);

  LogRead(address, rt, GetPrintRegisterFormat(element_size));
  LogWrite(address, rs, GetPrintRegisterFormat(element_size));
}

template <typename T>
void Simulator::LoadAcquireRCpcHelper(const Instruction* instr) {
  unsigned rt = instr->GetRt();
  unsigned rn = instr->GetRn();

  unsigned element_size = sizeof(T);
  uint64_t address = ReadRegister<uint64_t>(rn, Reg31IsStackPointer);

  CheckIsValidUnalignedAtomicAccess(rn, address, element_size);

  WriteRegister<T>(rt, Memory::Read<T>(address));

  // Approximate load-acquire by issuing a full barrier after the load.
  __sync_synchronize();

  LogRead(address, rt, GetPrintRegisterFormat(element_size));
}

#define ATOMIC_MEMORY_SIMPLE_UINT_LIST(V) \
  V(LDADD)                                \
  V(LDCLR)                                \
  V(LDEOR)                                \
  V(LDSET)                                \
  V(LDUMAX)                               \
  V(LDUMIN)

#define ATOMIC_MEMORY_SIMPLE_INT_LIST(V) \
  V(LDSMAX)                              \
  V(LDSMIN)

void Simulator::VisitAtomicMemory(const Instruction* instr) {
  switch (instr->Mask(AtomicMemoryMask)) {
// clang-format off
#define SIM_FUNC_B(A) \
    case A##B:        \
    case A##AB:       \
    case A##LB:       \
    case A##ALB:
#define SIM_FUNC_H(A) \
    case A##H:        \
    case A##AH:       \
    case A##LH:       \
    case A##ALH:
#define SIM_FUNC_w(A) \
    case A##_w:       \
    case A##A_w:      \
    case A##L_w:      \
    case A##AL_w:
#define SIM_FUNC_x(A) \
    case A##_x:       \
    case A##A_x:      \
    case A##L_x:      \
    case A##AL_x:

    ATOMIC_MEMORY_SIMPLE_UINT_LIST(SIM_FUNC_B)
      AtomicMemorySimpleHelper<uint8_t>(instr);
      break;
    ATOMIC_MEMORY_SIMPLE_INT_LIST(SIM_FUNC_B)
      AtomicMemorySimpleHelper<int8_t>(instr);
      break;
    ATOMIC_MEMORY_SIMPLE_UINT_LIST(SIM_FUNC_H)
      AtomicMemorySimpleHelper<uint16_t>(instr);
      break;
    ATOMIC_MEMORY_SIMPLE_INT_LIST(SIM_FUNC_H)
      AtomicMemorySimpleHelper<int16_t>(instr);
      break;
    ATOMIC_MEMORY_SIMPLE_UINT_LIST(SIM_FUNC_w)
      AtomicMemorySimpleHelper<uint32_t>(instr);
      break;
    ATOMIC_MEMORY_SIMPLE_INT_LIST(SIM_FUNC_w)
      AtomicMemorySimpleHelper<int32_t>(instr);
      break;
    ATOMIC_MEMORY_SIMPLE_UINT_LIST(SIM_FUNC_x)
      AtomicMemorySimpleHelper<uint64_t>(instr);
      break;
    ATOMIC_MEMORY_SIMPLE_INT_LIST(SIM_FUNC_x)
      AtomicMemorySimpleHelper<int64_t>(instr);
      break;
    // clang-format on

    case SWPB:
    case SWPAB:
    case SWPLB:
    case SWPALB:
      AtomicMemorySwapHelper<uint8_t>(instr);
      break;
    case SWPH:
    case SWPAH:
    case SWPLH:
    case SWPALH:
      AtomicMemorySwapHelper<uint16_t>(instr);
      break;
    case SWP_w:
    case SWPA_w:
    case SWPL_w:
    case SWPAL_w:
      AtomicMemorySwapHelper<uint32_t>(instr);
      break;
    case SWP_x:
    case SWPA_x:
    case SWPL_x:
    case SWPAL_x:
      AtomicMemorySwapHelper<uint64_t>(instr);
      break;
    case LDAPRB:
      LoadAcquireRCpcHelper<uint8_t>(instr);
      break;
    case LDAPRH:
      LoadAcquireRCpcHelper<uint16_t>(instr);
      break;
    case LDAPR_w:
      LoadAcquireRCpcHelper<uint32_t>(instr);
      break;
    case LDAPR_x:
      LoadAcquireRCpcHelper<uint64_t>(instr);
      break;
  }
}


void Simulator::VisitLoadLiteral(const Instruction* instr) {
  unsigned rt = instr->GetRt();
  uint64_t address = instr->GetLiteralAddress<uint64_t>();

  // Verify that the calculated address is available to the host.
  VIXL_ASSERT(address == static_cast<uintptr_t>(address));

  switch (instr->Mask(LoadLiteralMask)) {
    // Use NoRegLog to suppress the register trace (LOG_REGS, LOG_VREGS), then
    // print a more detailed log.
    case LDR_w_lit:
      WriteWRegister(rt, Memory::Read<uint32_t>(address), NoRegLog);
      LogRead(address, rt, kPrintWReg);
      break;
    case LDR_x_lit:
      WriteXRegister(rt, Memory::Read<uint64_t>(address), NoRegLog);
      LogRead(address, rt, kPrintXReg);
      break;
    case LDR_s_lit:
      WriteSRegister(rt, Memory::Read<float>(address), NoRegLog);
      LogVRead(address, rt, kPrintSReg);
      break;
    case LDR_d_lit:
      WriteDRegister(rt, Memory::Read<double>(address), NoRegLog);
      LogVRead(address, rt, kPrintDReg);
      break;
    case LDR_q_lit:
      WriteQRegister(rt, Memory::Read<qreg_t>(address), NoRegLog);
      LogVRead(address, rt, kPrintReg1Q);
      break;
    case LDRSW_x_lit:
      WriteXRegister(rt, Memory::Read<int32_t>(address), NoRegLog);
      LogRead(address, rt, kPrintWReg);
      break;

    // Ignore prfm hint instructions.
    case PRFM_lit:
      break;

    default:
      VIXL_UNREACHABLE();
  }

  local_monitor_.MaybeClear();
}


uintptr_t Simulator::AddressModeHelper(unsigned addr_reg,
                                       int64_t offset,
                                       AddrMode addrmode) {
  uint64_t address = ReadXRegister(addr_reg, Reg31IsStackPointer);

  if ((addr_reg == 31) && ((address % 16) != 0)) {
    // When the base register is SP the stack pointer is required to be
    // quadword aligned prior to the address calculation and write-backs.
    // Misalignment will cause a stack alignment fault.
    VIXL_ALIGNMENT_EXCEPTION();
  }

  if ((addrmode == PreIndex) || (addrmode == PostIndex)) {
    VIXL_ASSERT(offset != 0);
    // Only preindex should log the register update here. For Postindex, the
    // update will be printed automatically by LogWrittenRegisters _after_ the
    // memory access itself is logged.
    RegLogMode log_mode = (addrmode == PreIndex) ? LogRegWrites : NoRegLog;
    WriteXRegister(addr_reg, address + offset, log_mode, Reg31IsStackPointer);
  }

  if ((addrmode == Offset) || (addrmode == PreIndex)) {
    address += offset;
  }

  // Verify that the calculated address is available to the host.
  VIXL_ASSERT(address == static_cast<uintptr_t>(address));

  return static_cast<uintptr_t>(address);
}


void Simulator::VisitMoveWideImmediate(const Instruction* instr) {
  MoveWideImmediateOp mov_op =
      static_cast<MoveWideImmediateOp>(instr->Mask(MoveWideImmediateMask));
  int64_t new_xn_val = 0;

  bool is_64_bits = instr->GetSixtyFourBits() == 1;
  // Shift is limited for W operations.
  VIXL_ASSERT(is_64_bits || (instr->GetShiftMoveWide() < 2));

  // Get the shifted immediate.
  int64_t shift = instr->GetShiftMoveWide() * 16;
  int64_t shifted_imm16 = static_cast<int64_t>(instr->GetImmMoveWide())
                          << shift;

  // Compute the new value.
  switch (mov_op) {
    case MOVN_w:
    case MOVN_x: {
      new_xn_val = ~shifted_imm16;
      if (!is_64_bits) new_xn_val &= kWRegMask;
      break;
    }
    case MOVK_w:
    case MOVK_x: {
      unsigned reg_code = instr->GetRd();
      int64_t prev_xn_val =
          is_64_bits ? ReadXRegister(reg_code) : ReadWRegister(reg_code);
      new_xn_val = (prev_xn_val & ~(INT64_C(0xffff) << shift)) | shifted_imm16;
      break;
    }
    case MOVZ_w:
    case MOVZ_x: {
      new_xn_val = shifted_imm16;
      break;
    }
    default:
      VIXL_UNREACHABLE();
  }

  // Update the destination register.
  WriteXRegister(instr->GetRd(), new_xn_val);
}


void Simulator::VisitConditionalSelect(const Instruction* instr) {
  uint64_t new_val = ReadXRegister(instr->GetRn());

  if (ConditionFailed(static_cast<Condition>(instr->GetCondition()))) {
    new_val = ReadXRegister(instr->GetRm());
    switch (instr->Mask(ConditionalSelectMask)) {
      case CSEL_w:
      case CSEL_x:
        break;
      case CSINC_w:
      case CSINC_x:
        new_val++;
        break;
      case CSINV_w:
      case CSINV_x:
        new_val = ~new_val;
        break;
      case CSNEG_w:
      case CSNEG_x:
        new_val = -new_val;
        break;
      default:
        VIXL_UNIMPLEMENTED();
    }
  }
  unsigned reg_size = instr->GetSixtyFourBits() ? kXRegSize : kWRegSize;
  WriteRegister(reg_size, instr->GetRd(), new_val);
}


// clang-format off
#define PAUTH_MODES(V)                                       \
  V(IA,  ReadXRegister(src), kPACKeyIA, kInstructionPointer) \
  V(IB,  ReadXRegister(src), kPACKeyIB, kInstructionPointer) \
  V(IZA, 0x00000000,         kPACKeyIA, kInstructionPointer) \
  V(IZB, 0x00000000,         kPACKeyIB, kInstructionPointer) \
  V(DA,  ReadXRegister(src), kPACKeyDA, kDataPointer)        \
  V(DB,  ReadXRegister(src), kPACKeyDB, kDataPointer)        \
  V(DZA, 0x00000000,         kPACKeyDA, kDataPointer)        \
  V(DZB, 0x00000000,         kPACKeyDB, kDataPointer)
// clang-format on

void Simulator::VisitDataProcessing1Source(const Instruction* instr) {
  unsigned dst = instr->GetRd();
  unsigned src = instr->GetRn();

  switch (instr->Mask(DataProcessing1SourceMask)) {
#define DEFINE_PAUTH_FUNCS(SUFFIX, MOD, KEY, D)     \
  case PAC##SUFFIX: {                               \
    uint64_t ptr = ReadXRegister(dst);              \
    WriteXRegister(dst, AddPAC(ptr, MOD, KEY, D));  \
    break;                                          \
  }                                                 \
  case AUT##SUFFIX: {                               \
    uint64_t ptr = ReadXRegister(dst);              \
    WriteXRegister(dst, AuthPAC(ptr, MOD, KEY, D)); \
    break;                                          \
  }

    PAUTH_MODES(DEFINE_PAUTH_FUNCS)
#undef DEFINE_PAUTH_FUNCS

    case XPACI:
      WriteXRegister(dst, StripPAC(ReadXRegister(dst), kInstructionPointer));
      break;
    case XPACD:
      WriteXRegister(dst, StripPAC(ReadXRegister(dst), kDataPointer));
      break;
    case RBIT_w:
      WriteWRegister(dst, ReverseBits(ReadWRegister(src)));
      break;
    case RBIT_x:
      WriteXRegister(dst, ReverseBits(ReadXRegister(src)));
      break;
    case REV16_w:
      WriteWRegister(dst, ReverseBytes(ReadWRegister(src), 1));
      break;
    case REV16_x:
      WriteXRegister(dst, ReverseBytes(ReadXRegister(src), 1));
      break;
    case REV_w:
      WriteWRegister(dst, ReverseBytes(ReadWRegister(src), 2));
      break;
    case REV32_x:
      WriteXRegister(dst, ReverseBytes(ReadXRegister(src), 2));
      break;
    case REV_x:
      WriteXRegister(dst, ReverseBytes(ReadXRegister(src), 3));
      break;
    case CLZ_w:
      WriteWRegister(dst, CountLeadingZeros(ReadWRegister(src)));
      break;
    case CLZ_x:
      WriteXRegister(dst, CountLeadingZeros(ReadXRegister(src)));
      break;
    case CLS_w:
      WriteWRegister(dst, CountLeadingSignBits(ReadWRegister(src)));
      break;
    case CLS_x:
      WriteXRegister(dst, CountLeadingSignBits(ReadXRegister(src)));
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }
}


uint32_t Simulator::Poly32Mod2(unsigned n, uint64_t data, uint32_t poly) {
  VIXL_ASSERT((n > 32) && (n <= 64));
  for (unsigned i = (n - 1); i >= 32; i--) {
    if (((data >> i) & 1) != 0) {
      uint64_t polysh32 = (uint64_t)poly << (i - 32);
      uint64_t mask = (UINT64_C(1) << i) - 1;
      data = ((data & mask) ^ polysh32);
    }
  }
  return data & 0xffffffff;
}


template <typename T>
uint32_t Simulator::Crc32Checksum(uint32_t acc, T val, uint32_t poly) {
  unsigned size = sizeof(val) * 8;  // Number of bits in type T.
  VIXL_ASSERT((size == 8) || (size == 16) || (size == 32));
  uint64_t tempacc = static_cast<uint64_t>(ReverseBits(acc)) << size;
  uint64_t tempval = static_cast<uint64_t>(ReverseBits(val)) << 32;
  return ReverseBits(Poly32Mod2(32 + size, tempacc ^ tempval, poly));
}


uint32_t Simulator::Crc32Checksum(uint32_t acc, uint64_t val, uint32_t poly) {
  // Poly32Mod2 cannot handle inputs with more than 32 bits, so compute
  // the CRC of each 32-bit word sequentially.
  acc = Crc32Checksum(acc, (uint32_t)(val & 0xffffffff), poly);
  return Crc32Checksum(acc, (uint32_t)(val >> 32), poly);
}


void Simulator::VisitDataProcessing2Source(const Instruction* instr) {
  Shift shift_op = NO_SHIFT;
  int64_t result = 0;
  unsigned reg_size = instr->GetSixtyFourBits() ? kXRegSize : kWRegSize;

  switch (instr->Mask(DataProcessing2SourceMask)) {
    case SDIV_w: {
      int32_t rn = ReadWRegister(instr->GetRn());
      int32_t rm = ReadWRegister(instr->GetRm());
      if ((rn == kWMinInt) && (rm == -1)) {
        result = kWMinInt;
      } else if (rm == 0) {
        // Division by zero can be trapped, but not on A-class processors.
        result = 0;
      } else {
        result = rn / rm;
      }
      break;
    }
    case SDIV_x: {
      int64_t rn = ReadXRegister(instr->GetRn());
      int64_t rm = ReadXRegister(instr->GetRm());
      if ((rn == kXMinInt) && (rm == -1)) {
        result = kXMinInt;
      } else if (rm == 0) {
        // Division by zero can be trapped, but not on A-class processors.
        result = 0;
      } else {
        result = rn / rm;
      }
      break;
    }
    case UDIV_w: {
      uint32_t rn = static_cast<uint32_t>(ReadWRegister(instr->GetRn()));
      uint32_t rm = static_cast<uint32_t>(ReadWRegister(instr->GetRm()));
      if (rm == 0) {
        // Division by zero can be trapped, but not on A-class processors.
        result = 0;
      } else {
        result = rn / rm;
      }
      break;
    }
    case UDIV_x: {
      uint64_t rn = static_cast<uint64_t>(ReadXRegister(instr->GetRn()));
      uint64_t rm = static_cast<uint64_t>(ReadXRegister(instr->GetRm()));
      if (rm == 0) {
        // Division by zero can be trapped, but not on A-class processors.
        result = 0;
      } else {
        result = rn / rm;
      }
      break;
    }
    case LSLV_w:
    case LSLV_x:
      shift_op = LSL;
      break;
    case LSRV_w:
    case LSRV_x:
      shift_op = LSR;
      break;
    case ASRV_w:
    case ASRV_x:
      shift_op = ASR;
      break;
    case RORV_w:
    case RORV_x:
      shift_op = ROR;
      break;
    case PACGA: {
      uint64_t dst = static_cast<uint64_t>(ReadXRegister(instr->GetRn()));
      uint64_t src = static_cast<uint64_t>(
          ReadXRegister(instr->GetRm(), Reg31IsStackPointer));
      uint64_t code = ComputePAC(dst, src, kPACKeyGA);
      result = code & 0xffffffff00000000;
      break;
    }
    case CRC32B: {
      uint32_t acc = ReadRegister<uint32_t>(instr->GetRn());
      uint8_t val = ReadRegister<uint8_t>(instr->GetRm());
      result = Crc32Checksum(acc, val, CRC32_POLY);
      break;
    }
    case CRC32H: {
      uint32_t acc = ReadRegister<uint32_t>(instr->GetRn());
      uint16_t val = ReadRegister<uint16_t>(instr->GetRm());
      result = Crc32Checksum(acc, val, CRC32_POLY);
      break;
    }
    case CRC32W: {
      uint32_t acc = ReadRegister<uint32_t>(instr->GetRn());
      uint32_t val = ReadRegister<uint32_t>(instr->GetRm());
      result = Crc32Checksum(acc, val, CRC32_POLY);
      break;
    }
    case CRC32X: {
      uint32_t acc = ReadRegister<uint32_t>(instr->GetRn());
      uint64_t val = ReadRegister<uint64_t>(instr->GetRm());
      result = Crc32Checksum(acc, val, CRC32_POLY);
      reg_size = kWRegSize;
      break;
    }
    case CRC32CB: {
      uint32_t acc = ReadRegister<uint32_t>(instr->GetRn());
      uint8_t val = ReadRegister<uint8_t>(instr->GetRm());
      result = Crc32Checksum(acc, val, CRC32C_POLY);
      break;
    }
    case CRC32CH: {
      uint32_t acc = ReadRegister<uint32_t>(instr->GetRn());
      uint16_t val = ReadRegister<uint16_t>(instr->GetRm());
      result = Crc32Checksum(acc, val, CRC32C_POLY);
      break;
    }
    case CRC32CW: {
      uint32_t acc = ReadRegister<uint32_t>(instr->GetRn());
      uint32_t val = ReadRegister<uint32_t>(instr->GetRm());
      result = Crc32Checksum(acc, val, CRC32C_POLY);
      break;
    }
    case CRC32CX: {
      uint32_t acc = ReadRegister<uint32_t>(instr->GetRn());
      uint64_t val = ReadRegister<uint64_t>(instr->GetRm());
      result = Crc32Checksum(acc, val, CRC32C_POLY);
      reg_size = kWRegSize;
      break;
    }
    default:
      VIXL_UNIMPLEMENTED();
  }

  if (shift_op != NO_SHIFT) {
    // Shift distance encoded in the least-significant five/six bits of the
    // register.
    int mask = (instr->GetSixtyFourBits() == 1) ? 0x3f : 0x1f;
    unsigned shift = ReadWRegister(instr->GetRm()) & mask;
    result = ShiftOperand(reg_size,
                          ReadRegister(reg_size, instr->GetRn()),
                          shift_op,
                          shift);
  }
  WriteRegister(reg_size, instr->GetRd(), result);
}


void Simulator::VisitDataProcessing3Source(const Instruction* instr) {
  unsigned reg_size = instr->GetSixtyFourBits() ? kXRegSize : kWRegSize;

  uint64_t result = 0;
  // Extract and sign- or zero-extend 32-bit arguments for widening operations.
  uint64_t rn_u32 = ReadRegister<uint32_t>(instr->GetRn());
  uint64_t rm_u32 = ReadRegister<uint32_t>(instr->GetRm());
  int64_t rn_s32 = ReadRegister<int32_t>(instr->GetRn());
  int64_t rm_s32 = ReadRegister<int32_t>(instr->GetRm());
  uint64_t rn_u64 = ReadXRegister(instr->GetRn());
  uint64_t rm_u64 = ReadXRegister(instr->GetRm());
  switch (instr->Mask(DataProcessing3SourceMask)) {
    case MADD_w:
    case MADD_x:
      result = ReadXRegister(instr->GetRa()) + (rn_u64 * rm_u64);
      break;
    case MSUB_w:
    case MSUB_x:
      result = ReadXRegister(instr->GetRa()) - (rn_u64 * rm_u64);
      break;
    case SMADDL_x:
      result = ReadXRegister(instr->GetRa()) +
               static_cast<uint64_t>(rn_s32 * rm_s32);
      break;
    case SMSUBL_x:
      result = ReadXRegister(instr->GetRa()) -
               static_cast<uint64_t>(rn_s32 * rm_s32);
      break;
    case UMADDL_x:
      result = ReadXRegister(instr->GetRa()) + (rn_u32 * rm_u32);
      break;
    case UMSUBL_x:
      result = ReadXRegister(instr->GetRa()) - (rn_u32 * rm_u32);
      break;
    case UMULH_x:
      result =
          internal::MultiplyHigh<64>(ReadRegister<uint64_t>(instr->GetRn()),
                                     ReadRegister<uint64_t>(instr->GetRm()));
      break;
    case SMULH_x:
      result = internal::MultiplyHigh<64>(ReadXRegister(instr->GetRn()),
                                          ReadXRegister(instr->GetRm()));
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }
  WriteRegister(reg_size, instr->GetRd(), result);
}


void Simulator::VisitBitfield(const Instruction* instr) {
  unsigned reg_size = instr->GetSixtyFourBits() ? kXRegSize : kWRegSize;
  int64_t reg_mask = instr->GetSixtyFourBits() ? kXRegMask : kWRegMask;
  int R = instr->GetImmR();
  int S = instr->GetImmS();
  int diff = S - R;
  uint64_t mask;
  if (diff >= 0) {
    mask = ~UINT64_C(0) >> (64 - (diff + 1));
    mask = (static_cast<unsigned>(diff) < (reg_size - 1)) ? mask : reg_mask;
  } else {
    mask = ~UINT64_C(0) >> (64 - (S + 1));
    mask = RotateRight(mask, R, reg_size);
    diff += reg_size;
  }

  // inzero indicates if the extracted bitfield is inserted into the
  // destination register value or in zero.
  // If extend is true, extend the sign of the extracted bitfield.
  bool inzero = false;
  bool extend = false;
  switch (instr->Mask(BitfieldMask)) {
    case BFM_x:
    case BFM_w:
      break;
    case SBFM_x:
    case SBFM_w:
      inzero = true;
      extend = true;
      break;
    case UBFM_x:
    case UBFM_w:
      inzero = true;
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }

  uint64_t dst = inzero ? 0 : ReadRegister(reg_size, instr->GetRd());
  uint64_t src = ReadRegister(reg_size, instr->GetRn());
  // Rotate source bitfield into place.
  uint64_t result = RotateRight(src, R, reg_size);
  // Determine the sign extension.
  uint64_t topbits = (diff == 63) ? 0 : (~UINT64_C(0) << (diff + 1));
  uint64_t signbits = extend && ((src >> S) & 1) ? topbits : 0;

  // Merge sign extension, dest/zero and bitfield.
  result = signbits | (result & mask) | (dst & ~mask);

  WriteRegister(reg_size, instr->GetRd(), result);
}


void Simulator::VisitExtract(const Instruction* instr) {
  unsigned lsb = instr->GetImmS();
  unsigned reg_size = (instr->GetSixtyFourBits() == 1) ? kXRegSize : kWRegSize;
  uint64_t low_res =
      static_cast<uint64_t>(ReadRegister(reg_size, instr->GetRm())) >> lsb;
  uint64_t high_res =
      (lsb == 0) ? 0 : ReadRegister<uint64_t>(reg_size, instr->GetRn())
                           << (reg_size - lsb);
  WriteRegister(reg_size, instr->GetRd(), low_res | high_res);
}


void Simulator::VisitFPImmediate(const Instruction* instr) {
  AssertSupportedFPCR();
  unsigned dest = instr->GetRd();
  switch (instr->Mask(FPImmediateMask)) {
    case FMOV_h_imm:
      WriteHRegister(dest, Float16ToRawbits(instr->GetImmFP16()));
      break;
    case FMOV_s_imm:
      WriteSRegister(dest, instr->GetImmFP32());
      break;
    case FMOV_d_imm:
      WriteDRegister(dest, instr->GetImmFP64());
      break;
    default:
      VIXL_UNREACHABLE();
  }
}


void Simulator::VisitFPIntegerConvert(const Instruction* instr) {
  AssertSupportedFPCR();

  unsigned dst = instr->GetRd();
  unsigned src = instr->GetRn();

  FPRounding round = ReadRMode();

  switch (instr->Mask(FPIntegerConvertMask)) {
    case FCVTAS_wh:
      WriteWRegister(dst, FPToInt32(ReadHRegister(src), FPTieAway));
      break;
    case FCVTAS_xh:
      WriteXRegister(dst, FPToInt64(ReadHRegister(src), FPTieAway));
      break;
    case FCVTAS_ws:
      WriteWRegister(dst, FPToInt32(ReadSRegister(src), FPTieAway));
      break;
    case FCVTAS_xs:
      WriteXRegister(dst, FPToInt64(ReadSRegister(src), FPTieAway));
      break;
    case FCVTAS_wd:
      WriteWRegister(dst, FPToInt32(ReadDRegister(src), FPTieAway));
      break;
    case FCVTAS_xd:
      WriteXRegister(dst, FPToInt64(ReadDRegister(src), FPTieAway));
      break;
    case FCVTAU_wh:
      WriteWRegister(dst, FPToUInt32(ReadHRegister(src), FPTieAway));
      break;
    case FCVTAU_xh:
      WriteXRegister(dst, FPToUInt64(ReadHRegister(src), FPTieAway));
      break;
    case FCVTAU_ws:
      WriteWRegister(dst, FPToUInt32(ReadSRegister(src), FPTieAway));
      break;
    case FCVTAU_xs:
      WriteXRegister(dst, FPToUInt64(ReadSRegister(src), FPTieAway));
      break;
    case FCVTAU_wd:
      WriteWRegister(dst, FPToUInt32(ReadDRegister(src), FPTieAway));
      break;
    case FCVTAU_xd:
      WriteXRegister(dst, FPToUInt64(ReadDRegister(src), FPTieAway));
      break;
    case FCVTMS_wh:
      WriteWRegister(dst, FPToInt32(ReadHRegister(src), FPNegativeInfinity));
      break;
    case FCVTMS_xh:
      WriteXRegister(dst, FPToInt64(ReadHRegister(src), FPNegativeInfinity));
      break;
    case FCVTMS_ws:
      WriteWRegister(dst, FPToInt32(ReadSRegister(src), FPNegativeInfinity));
      break;
    case FCVTMS_xs:
      WriteXRegister(dst, FPToInt64(ReadSRegister(src), FPNegativeInfinity));
      break;
    case FCVTMS_wd:
      WriteWRegister(dst, FPToInt32(ReadDRegister(src), FPNegativeInfinity));
      break;
    case FCVTMS_xd:
      WriteXRegister(dst, FPToInt64(ReadDRegister(src), FPNegativeInfinity));
      break;
    case FCVTMU_wh:
      WriteWRegister(dst, FPToUInt32(ReadHRegister(src), FPNegativeInfinity));
      break;
    case FCVTMU_xh:
      WriteXRegister(dst, FPToUInt64(ReadHRegister(src), FPNegativeInfinity));
      break;
    case FCVTMU_ws:
      WriteWRegister(dst, FPToUInt32(ReadSRegister(src), FPNegativeInfinity));
      break;
    case FCVTMU_xs:
      WriteXRegister(dst, FPToUInt64(ReadSRegister(src), FPNegativeInfinity));
      break;
    case FCVTMU_wd:
      WriteWRegister(dst, FPToUInt32(ReadDRegister(src), FPNegativeInfinity));
      break;
    case FCVTMU_xd:
      WriteXRegister(dst, FPToUInt64(ReadDRegister(src), FPNegativeInfinity));
      break;
    case FCVTPS_wh:
      WriteWRegister(dst, FPToInt32(ReadHRegister(src), FPPositiveInfinity));
      break;
    case FCVTPS_xh:
      WriteXRegister(dst, FPToInt64(ReadHRegister(src), FPPositiveInfinity));
      break;
    case FCVTPS_ws:
      WriteWRegister(dst, FPToInt32(ReadSRegister(src), FPPositiveInfinity));
      break;
    case FCVTPS_xs:
      WriteXRegister(dst, FPToInt64(ReadSRegister(src), FPPositiveInfinity));
      break;
    case FCVTPS_wd:
      WriteWRegister(dst, FPToInt32(ReadDRegister(src), FPPositiveInfinity));
      break;
    case FCVTPS_xd:
      WriteXRegister(dst, FPToInt64(ReadDRegister(src), FPPositiveInfinity));
      break;
    case FCVTPU_wh:
      WriteWRegister(dst, FPToUInt32(ReadHRegister(src), FPPositiveInfinity));
      break;
    case FCVTPU_xh:
      WriteXRegister(dst, FPToUInt64(ReadHRegister(src), FPPositiveInfinity));
      break;
    case FCVTPU_ws:
      WriteWRegister(dst, FPToUInt32(ReadSRegister(src), FPPositiveInfinity));
      break;
    case FCVTPU_xs:
      WriteXRegister(dst, FPToUInt64(ReadSRegister(src), FPPositiveInfinity));
      break;
    case FCVTPU_wd:
      WriteWRegister(dst, FPToUInt32(ReadDRegister(src), FPPositiveInfinity));
      break;
    case FCVTPU_xd:
      WriteXRegister(dst, FPToUInt64(ReadDRegister(src), FPPositiveInfinity));
      break;
    case FCVTNS_wh:
      WriteWRegister(dst, FPToInt32(ReadHRegister(src), FPTieEven));
      break;
    case FCVTNS_xh:
      WriteXRegister(dst, FPToInt64(ReadHRegister(src), FPTieEven));
      break;
    case FCVTNS_ws:
      WriteWRegister(dst, FPToInt32(ReadSRegister(src), FPTieEven));
      break;
    case FCVTNS_xs:
      WriteXRegister(dst, FPToInt64(ReadSRegister(src), FPTieEven));
      break;
    case FCVTNS_wd:
      WriteWRegister(dst, FPToInt32(ReadDRegister(src), FPTieEven));
      break;
    case FCVTNS_xd:
      WriteXRegister(dst, FPToInt64(ReadDRegister(src), FPTieEven));
      break;
    case FCVTNU_wh:
      WriteWRegister(dst, FPToUInt32(ReadHRegister(src), FPTieEven));
      break;
    case FCVTNU_xh:
      WriteXRegister(dst, FPToUInt64(ReadHRegister(src), FPTieEven));
      break;
    case FCVTNU_ws:
      WriteWRegister(dst, FPToUInt32(ReadSRegister(src), FPTieEven));
      break;
    case FCVTNU_xs:
      WriteXRegister(dst, FPToUInt64(ReadSRegister(src), FPTieEven));
      break;
    case FCVTNU_wd:
      WriteWRegister(dst, FPToUInt32(ReadDRegister(src), FPTieEven));
      break;
    case FCVTNU_xd:
      WriteXRegister(dst, FPToUInt64(ReadDRegister(src), FPTieEven));
      break;
    case FCVTZS_wh:
      WriteWRegister(dst, FPToInt32(ReadHRegister(src), FPZero));
      break;
    case FCVTZS_xh:
      WriteXRegister(dst, FPToInt64(ReadHRegister(src), FPZero));
      break;
    case FCVTZS_ws:
      WriteWRegister(dst, FPToInt32(ReadSRegister(src), FPZero));
      break;
    case FCVTZS_xs:
      WriteXRegister(dst, FPToInt64(ReadSRegister(src), FPZero));
      break;
    case FCVTZS_wd:
      WriteWRegister(dst, FPToInt32(ReadDRegister(src), FPZero));
      break;
    case FCVTZS_xd:
      WriteXRegister(dst, FPToInt64(ReadDRegister(src), FPZero));
      break;
    case FCVTZU_wh:
      WriteWRegister(dst, FPToUInt32(ReadHRegister(src), FPZero));
      break;
    case FCVTZU_xh:
      WriteXRegister(dst, FPToUInt64(ReadHRegister(src), FPZero));
      break;
    case FCVTZU_ws:
      WriteWRegister(dst, FPToUInt32(ReadSRegister(src), FPZero));
      break;
    case FCVTZU_xs:
      WriteXRegister(dst, FPToUInt64(ReadSRegister(src), FPZero));
      break;
    case FCVTZU_wd:
      WriteWRegister(dst, FPToUInt32(ReadDRegister(src), FPZero));
      break;
    case FCVTZU_xd:
      WriteXRegister(dst, FPToUInt64(ReadDRegister(src), FPZero));
      break;
    case FJCVTZS:
      WriteWRegister(dst, FPToFixedJS(ReadDRegister(src)));
      break;
    case FMOV_hw:
      WriteHRegister(dst, ReadWRegister(src) & kHRegMask);
      break;
    case FMOV_wh:
      WriteWRegister(dst, ReadHRegisterBits(src));
      break;
    case FMOV_xh:
      WriteXRegister(dst, ReadHRegisterBits(src));
      break;
    case FMOV_hx:
      WriteHRegister(dst, ReadXRegister(src) & kHRegMask);
      break;
    case FMOV_ws:
      WriteWRegister(dst, ReadSRegisterBits(src));
      break;
    case FMOV_xd:
      WriteXRegister(dst, ReadDRegisterBits(src));
      break;
    case FMOV_sw:
      WriteSRegisterBits(dst, ReadWRegister(src));
      break;
    case FMOV_dx:
      WriteDRegisterBits(dst, ReadXRegister(src));
      break;
    case FMOV_d1_x:
      LogicVRegister(ReadVRegister(dst))
          .SetUint(kFormatD, 1, ReadXRegister(src));
      break;
    case FMOV_x_d1:
      WriteXRegister(dst, LogicVRegister(ReadVRegister(src)).Uint(kFormatD, 1));
      break;

    // A 32-bit input can be handled in the same way as a 64-bit input, since
    // the sign- or zero-extension will not affect the conversion.
    case SCVTF_dx:
      WriteDRegister(dst, FixedToDouble(ReadXRegister(src), 0, round));
      break;
    case SCVTF_dw:
      WriteDRegister(dst, FixedToDouble(ReadWRegister(src), 0, round));
      break;
    case UCVTF_dx:
      WriteDRegister(dst, UFixedToDouble(ReadXRegister(src), 0, round));
      break;
    case UCVTF_dw: {
      WriteDRegister(dst,
                     UFixedToDouble(ReadRegister<uint32_t>(src), 0, round));
      break;
    }
    case SCVTF_sx:
      WriteSRegister(dst, FixedToFloat(ReadXRegister(src), 0, round));
      break;
    case SCVTF_sw:
      WriteSRegister(dst, FixedToFloat(ReadWRegister(src), 0, round));
      break;
    case UCVTF_sx:
      WriteSRegister(dst, UFixedToFloat(ReadXRegister(src), 0, round));
      break;
    case UCVTF_sw: {
      WriteSRegister(dst, UFixedToFloat(ReadRegister<uint32_t>(src), 0, round));
      break;
    }
    case SCVTF_hx:
      WriteHRegister(dst, FixedToFloat16(ReadXRegister(src), 0, round));
      break;
    case SCVTF_hw:
      WriteHRegister(dst, FixedToFloat16(ReadWRegister(src), 0, round));
      break;
    case UCVTF_hx:
      WriteHRegister(dst, UFixedToFloat16(ReadXRegister(src), 0, round));
      break;
    case UCVTF_hw: {
      WriteHRegister(dst,
                     UFixedToFloat16(ReadRegister<uint32_t>(src), 0, round));
      break;
    }

    default:
      VIXL_UNREACHABLE();
  }
}


void Simulator::VisitFPFixedPointConvert(const Instruction* instr) {
  AssertSupportedFPCR();

  unsigned dst = instr->GetRd();
  unsigned src = instr->GetRn();
  int fbits = 64 - instr->GetFPScale();

  FPRounding round = ReadRMode();

  switch (instr->Mask(FPFixedPointConvertMask)) {
    // A 32-bit input can be handled in the same way as a 64-bit input, since
    // the sign- or zero-extension will not affect the conversion.
    case SCVTF_dx_fixed:
      WriteDRegister(dst, FixedToDouble(ReadXRegister(src), fbits, round));
      break;
    case SCVTF_dw_fixed:
      WriteDRegister(dst, FixedToDouble(ReadWRegister(src), fbits, round));
      break;
    case UCVTF_dx_fixed:
      WriteDRegister(dst, UFixedToDouble(ReadXRegister(src), fbits, round));
      break;
    case UCVTF_dw_fixed: {
      WriteDRegister(dst,
                     UFixedToDouble(ReadRegister<uint32_t>(src), fbits, round));
      break;
    }
    case SCVTF_sx_fixed:
      WriteSRegister(dst, FixedToFloat(ReadXRegister(src), fbits, round));
      break;
    case SCVTF_sw_fixed:
      WriteSRegister(dst, FixedToFloat(ReadWRegister(src), fbits, round));
      break;
    case UCVTF_sx_fixed:
      WriteSRegister(dst, UFixedToFloat(ReadXRegister(src), fbits, round));
      break;
    case UCVTF_sw_fixed: {
      WriteSRegister(dst,
                     UFixedToFloat(ReadRegister<uint32_t>(src), fbits, round));
      break;
    }
    case SCVTF_hx_fixed:
      WriteHRegister(dst, FixedToFloat16(ReadXRegister(src), fbits, round));
      break;
    case SCVTF_hw_fixed:
      WriteHRegister(dst, FixedToFloat16(ReadWRegister(src), fbits, round));
      break;
    case UCVTF_hx_fixed:
      WriteHRegister(dst, UFixedToFloat16(ReadXRegister(src), fbits, round));
      break;
    case UCVTF_hw_fixed: {
      WriteHRegister(dst,
                     UFixedToFloat16(ReadRegister<uint32_t>(src),
                                     fbits,
                                     round));
      break;
    }
    case FCVTZS_xd_fixed:
      WriteXRegister(dst,
                     FPToInt64(ReadDRegister(src) * std::pow(2.0, fbits),
                               FPZero));
      break;
    case FCVTZS_wd_fixed:
      WriteWRegister(dst,
                     FPToInt32(ReadDRegister(src) * std::pow(2.0, fbits),
                               FPZero));
      break;
    case FCVTZU_xd_fixed:
      WriteXRegister(dst,
                     FPToUInt64(ReadDRegister(src) * std::pow(2.0, fbits),
                                FPZero));
      break;
    case FCVTZU_wd_fixed:
      WriteWRegister(dst,
                     FPToUInt32(ReadDRegister(src) * std::pow(2.0, fbits),
                                FPZero));
      break;
    case FCVTZS_xs_fixed:
      WriteXRegister(dst,
                     FPToInt64(ReadSRegister(src) * std::pow(2.0f, fbits),
                               FPZero));
      break;
    case FCVTZS_ws_fixed:
      WriteWRegister(dst,
                     FPToInt32(ReadSRegister(src) * std::pow(2.0f, fbits),
                               FPZero));
      break;
    case FCVTZU_xs_fixed:
      WriteXRegister(dst,
                     FPToUInt64(ReadSRegister(src) * std::pow(2.0f, fbits),
                                FPZero));
      break;
    case FCVTZU_ws_fixed:
      WriteWRegister(dst,
                     FPToUInt32(ReadSRegister(src) * std::pow(2.0f, fbits),
                                FPZero));
      break;
    case FCVTZS_xh_fixed: {
      double output =
          static_cast<double>(ReadHRegister(src)) * std::pow(2.0, fbits);
      WriteXRegister(dst, FPToInt64(output, FPZero));
      break;
    }
    case FCVTZS_wh_fixed: {
      double output =
          static_cast<double>(ReadHRegister(src)) * std::pow(2.0, fbits);
      WriteWRegister(dst, FPToInt32(output, FPZero));
      break;
    }
    case FCVTZU_xh_fixed: {
      double output =
          static_cast<double>(ReadHRegister(src)) * std::pow(2.0, fbits);
      WriteXRegister(dst, FPToUInt64(output, FPZero));
      break;
    }
    case FCVTZU_wh_fixed: {
      double output =
          static_cast<double>(ReadHRegister(src)) * std::pow(2.0, fbits);
      WriteWRegister(dst, FPToUInt32(output, FPZero));
      break;
    }
    default:
      VIXL_UNREACHABLE();
  }
}


void Simulator::VisitFPCompare(const Instruction* instr) {
  AssertSupportedFPCR();

  FPTrapFlags trap = DisableTrap;
  switch (instr->Mask(FPCompareMask)) {
    case FCMPE_h:
      trap = EnableTrap;
      VIXL_FALLTHROUGH();
    case FCMP_h:
      FPCompare(ReadHRegister(instr->GetRn()),
                ReadHRegister(instr->GetRm()),
                trap);
      break;
    case FCMPE_s:
      trap = EnableTrap;
      VIXL_FALLTHROUGH();
    case FCMP_s:
      FPCompare(ReadSRegister(instr->GetRn()),
                ReadSRegister(instr->GetRm()),
                trap);
      break;
    case FCMPE_d:
      trap = EnableTrap;
      VIXL_FALLTHROUGH();
    case FCMP_d:
      FPCompare(ReadDRegister(instr->GetRn()),
                ReadDRegister(instr->GetRm()),
                trap);
      break;
    case FCMPE_h_zero:
      trap = EnableTrap;
      VIXL_FALLTHROUGH();
    case FCMP_h_zero:
      FPCompare(ReadHRegister(instr->GetRn()), SimFloat16(0.0), trap);
      break;
    case FCMPE_s_zero:
      trap = EnableTrap;
      VIXL_FALLTHROUGH();
    case FCMP_s_zero:
      FPCompare(ReadSRegister(instr->GetRn()), 0.0f, trap);
      break;
    case FCMPE_d_zero:
      trap = EnableTrap;
      VIXL_FALLTHROUGH();
    case FCMP_d_zero:
      FPCompare(ReadDRegister(instr->GetRn()), 0.0, trap);
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }
}


void Simulator::VisitFPConditionalCompare(const Instruction* instr) {
  AssertSupportedFPCR();

  FPTrapFlags trap = DisableTrap;
  switch (instr->Mask(FPConditionalCompareMask)) {
    case FCCMPE_h:
      trap = EnableTrap;
      VIXL_FALLTHROUGH();
    case FCCMP_h:
      if (ConditionPassed(instr->GetCondition())) {
        FPCompare(ReadHRegister(instr->GetRn()),
                  ReadHRegister(instr->GetRm()),
                  trap);
      } else {
        ReadNzcv().SetFlags(instr->GetNzcv());
        LogSystemRegister(NZCV);
      }
      break;
    case FCCMPE_s:
      trap = EnableTrap;
      VIXL_FALLTHROUGH();
    case FCCMP_s:
      if (ConditionPassed(instr->GetCondition())) {
        FPCompare(ReadSRegister(instr->GetRn()),
                  ReadSRegister(instr->GetRm()),
                  trap);
      } else {
        ReadNzcv().SetFlags(instr->GetNzcv());
        LogSystemRegister(NZCV);
      }
      break;
    case FCCMPE_d:
      trap = EnableTrap;
      VIXL_FALLTHROUGH();
    case FCCMP_d:
      if (ConditionPassed(instr->GetCondition())) {
        FPCompare(ReadDRegister(instr->GetRn()),
                  ReadDRegister(instr->GetRm()),
                  trap);
      } else {
        ReadNzcv().SetFlags(instr->GetNzcv());
        LogSystemRegister(NZCV);
      }
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }
}


void Simulator::VisitFPConditionalSelect(const Instruction* instr) {
  AssertSupportedFPCR();

  Instr selected;
  if (ConditionPassed(instr->GetCondition())) {
    selected = instr->GetRn();
  } else {
    selected = instr->GetRm();
  }

  switch (instr->Mask(FPConditionalSelectMask)) {
    case FCSEL_h:
      WriteHRegister(instr->GetRd(), ReadHRegister(selected));
      break;
    case FCSEL_s:
      WriteSRegister(instr->GetRd(), ReadSRegister(selected));
      break;
    case FCSEL_d:
      WriteDRegister(instr->GetRd(), ReadDRegister(selected));
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }
}


void Simulator::VisitFPDataProcessing1Source(const Instruction* instr) {
  AssertSupportedFPCR();

  FPRounding fpcr_rounding = static_cast<FPRounding>(ReadFpcr().GetRMode());
  VectorFormat vform;
  switch (instr->Mask(FPTypeMask)) {
    default:
      VIXL_UNREACHABLE_OR_FALLTHROUGH();
    case FP64:
      vform = kFormatD;
      break;
    case FP32:
      vform = kFormatS;
      break;
    case FP16:
      vform = kFormatH;
      break;
  }

  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());
  bool inexact_exception = false;
  FrintMode frint_mode = kFrintToInteger;

  unsigned fd = instr->GetRd();
  unsigned fn = instr->GetRn();

  switch (instr->Mask(FPDataProcessing1SourceMask)) {
    case FMOV_h:
      WriteHRegister(fd, ReadHRegister(fn));
      return;
    case FMOV_s:
      WriteSRegister(fd, ReadSRegister(fn));
      return;
    case FMOV_d:
      WriteDRegister(fd, ReadDRegister(fn));
      return;
    case FABS_h:
    case FABS_s:
    case FABS_d:
      fabs_(vform, ReadVRegister(fd), ReadVRegister(fn));
      // Explicitly log the register update whilst we have type information.
      LogVRegister(fd, GetPrintRegisterFormatFP(vform));
      return;
    case FNEG_h:
    case FNEG_s:
    case FNEG_d:
      fneg(vform, ReadVRegister(fd), ReadVRegister(fn));
      // Explicitly log the register update whilst we have type information.
      LogVRegister(fd, GetPrintRegisterFormatFP(vform));
      return;
    case FCVT_ds:
      WriteDRegister(fd, FPToDouble(ReadSRegister(fn), ReadDN()));
      return;
    case FCVT_sd:
      WriteSRegister(fd, FPToFloat(ReadDRegister(fn), FPTieEven, ReadDN()));
      return;
    case FCVT_hs:
      WriteHRegister(fd,
                     Float16ToRawbits(
                         FPToFloat16(ReadSRegister(fn), FPTieEven, ReadDN())));
      return;
    case FCVT_sh:
      WriteSRegister(fd, FPToFloat(ReadHRegister(fn), ReadDN()));
      return;
    case FCVT_dh:
      WriteDRegister(fd, FPToDouble(ReadHRegister(fn), ReadDN()));
      return;
    case FCVT_hd:
      WriteHRegister(fd,
                     Float16ToRawbits(
                         FPToFloat16(ReadDRegister(fn), FPTieEven, ReadDN())));
      return;
    case FSQRT_h:
    case FSQRT_s:
    case FSQRT_d:
      fsqrt(vform, rd, rn);
      // Explicitly log the register update whilst we have type information.
      LogVRegister(fd, GetPrintRegisterFormatFP(vform));
      return;
    case FRINT32X_s:
    case FRINT32X_d:
      inexact_exception = true;
      frint_mode = kFrintToInt32;
      break;  // Use FPCR rounding mode.
    case FRINT64X_s:
    case FRINT64X_d:
      inexact_exception = true;
      frint_mode = kFrintToInt64;
      break;  // Use FPCR rounding mode.
    case FRINT32Z_s:
    case FRINT32Z_d:
      inexact_exception = true;
      frint_mode = kFrintToInt32;
      fpcr_rounding = FPZero;
      break;
    case FRINT64Z_s:
    case FRINT64Z_d:
      inexact_exception = true;
      frint_mode = kFrintToInt64;
      fpcr_rounding = FPZero;
      break;
    case FRINTI_h:
    case FRINTI_s:
    case FRINTI_d:
      break;  // Use FPCR rounding mode.
    case FRINTX_h:
    case FRINTX_s:
    case FRINTX_d:
      inexact_exception = true;
      break;
    case FRINTA_h:
    case FRINTA_s:
    case FRINTA_d:
      fpcr_rounding = FPTieAway;
      break;
    case FRINTM_h:
    case FRINTM_s:
    case FRINTM_d:
      fpcr_rounding = FPNegativeInfinity;
      break;
    case FRINTN_h:
    case FRINTN_s:
    case FRINTN_d:
      fpcr_rounding = FPTieEven;
      break;
    case FRINTP_h:
    case FRINTP_s:
    case FRINTP_d:
      fpcr_rounding = FPPositiveInfinity;
      break;
    case FRINTZ_h:
    case FRINTZ_s:
    case FRINTZ_d:
      fpcr_rounding = FPZero;
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }

  // Only FRINT* instructions fall through the switch above.
  frint(vform, rd, rn, fpcr_rounding, inexact_exception, frint_mode);
  // Explicitly log the register update whilst we have type information.
  LogVRegister(fd, GetPrintRegisterFormatFP(vform));
}


void Simulator::VisitFPDataProcessing2Source(const Instruction* instr) {
  AssertSupportedFPCR();

  VectorFormat vform;
  switch (instr->Mask(FPTypeMask)) {
    default:
      VIXL_UNREACHABLE_OR_FALLTHROUGH();
    case FP64:
      vform = kFormatD;
      break;
    case FP32:
      vform = kFormatS;
      break;
    case FP16:
      vform = kFormatH;
      break;
  }
  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());
  SimVRegister& rm = ReadVRegister(instr->GetRm());

  switch (instr->Mask(FPDataProcessing2SourceMask)) {
    case FADD_h:
    case FADD_s:
    case FADD_d:
      fadd(vform, rd, rn, rm);
      break;
    case FSUB_h:
    case FSUB_s:
    case FSUB_d:
      fsub(vform, rd, rn, rm);
      break;
    case FMUL_h:
    case FMUL_s:
    case FMUL_d:
      fmul(vform, rd, rn, rm);
      break;
    case FNMUL_h:
    case FNMUL_s:
    case FNMUL_d:
      fnmul(vform, rd, rn, rm);
      break;
    case FDIV_h:
    case FDIV_s:
    case FDIV_d:
      fdiv(vform, rd, rn, rm);
      break;
    case FMAX_h:
    case FMAX_s:
    case FMAX_d:
      fmax(vform, rd, rn, rm);
      break;
    case FMIN_h:
    case FMIN_s:
    case FMIN_d:
      fmin(vform, rd, rn, rm);
      break;
    case FMAXNM_h:
    case FMAXNM_s:
    case FMAXNM_d:
      fmaxnm(vform, rd, rn, rm);
      break;
    case FMINNM_h:
    case FMINNM_s:
    case FMINNM_d:
      fminnm(vform, rd, rn, rm);
      break;
    default:
      VIXL_UNREACHABLE();
  }
  // Explicitly log the register update whilst we have type information.
  LogVRegister(instr->GetRd(), GetPrintRegisterFormatFP(vform));
}


void Simulator::VisitFPDataProcessing3Source(const Instruction* instr) {
  AssertSupportedFPCR();

  unsigned fd = instr->GetRd();
  unsigned fn = instr->GetRn();
  unsigned fm = instr->GetRm();
  unsigned fa = instr->GetRa();

  switch (instr->Mask(FPDataProcessing3SourceMask)) {
    // fd = fa +/- (fn * fm)
    case FMADD_h:
      WriteHRegister(fd,
                     FPMulAdd(ReadHRegister(fa),
                              ReadHRegister(fn),
                              ReadHRegister(fm)));
      break;
    case FMSUB_h:
      WriteHRegister(fd,
                     FPMulAdd(ReadHRegister(fa),
                              -ReadHRegister(fn),
                              ReadHRegister(fm)));
      break;
    case FMADD_s:
      WriteSRegister(fd,
                     FPMulAdd(ReadSRegister(fa),
                              ReadSRegister(fn),
                              ReadSRegister(fm)));
      break;
    case FMSUB_s:
      WriteSRegister(fd,
                     FPMulAdd(ReadSRegister(fa),
                              -ReadSRegister(fn),
                              ReadSRegister(fm)));
      break;
    case FMADD_d:
      WriteDRegister(fd,
                     FPMulAdd(ReadDRegister(fa),
                              ReadDRegister(fn),
                              ReadDRegister(fm)));
      break;
    case FMSUB_d:
      WriteDRegister(fd,
                     FPMulAdd(ReadDRegister(fa),
                              -ReadDRegister(fn),
                              ReadDRegister(fm)));
      break;
    // Negated variants of the above.
    case FNMADD_h:
      WriteHRegister(fd,
                     FPMulAdd(-ReadHRegister(fa),
                              -ReadHRegister(fn),
                              ReadHRegister(fm)));
      break;
    case FNMSUB_h:
      WriteHRegister(fd,
                     FPMulAdd(-ReadHRegister(fa),
                              ReadHRegister(fn),
                              ReadHRegister(fm)));
      break;
    case FNMADD_s:
      WriteSRegister(fd,
                     FPMulAdd(-ReadSRegister(fa),
                              -ReadSRegister(fn),
                              ReadSRegister(fm)));
      break;
    case FNMSUB_s:
      WriteSRegister(fd,
                     FPMulAdd(-ReadSRegister(fa),
                              ReadSRegister(fn),
                              ReadSRegister(fm)));
      break;
    case FNMADD_d:
      WriteDRegister(fd,
                     FPMulAdd(-ReadDRegister(fa),
                              -ReadDRegister(fn),
                              ReadDRegister(fm)));
      break;
    case FNMSUB_d:
      WriteDRegister(fd,
                     FPMulAdd(-ReadDRegister(fa),
                              ReadDRegister(fn),
                              ReadDRegister(fm)));
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }
}


bool Simulator::FPProcessNaNs(const Instruction* instr) {
  unsigned fd = instr->GetRd();
  unsigned fn = instr->GetRn();
  unsigned fm = instr->GetRm();
  bool done = false;

  if (instr->Mask(FP64) == FP64) {
    double result = FPProcessNaNs(ReadDRegister(fn), ReadDRegister(fm));
    if (IsNaN(result)) {
      WriteDRegister(fd, result);
      done = true;
    }
  } else if (instr->Mask(FP32) == FP32) {
    float result = FPProcessNaNs(ReadSRegister(fn), ReadSRegister(fm));
    if (IsNaN(result)) {
      WriteSRegister(fd, result);
      done = true;
    }
  } else {
    VIXL_ASSERT(instr->Mask(FP16) == FP16);
    VIXL_UNIMPLEMENTED();
  }

  return done;
}


void Simulator::SysOp_W(int op, int64_t val) {
  switch (op) {
    case IVAU:
    case CVAC:
    case CVAU:
    case CVAP:
    case CVADP:
    case CIVAC: {
      // Perform a dummy memory access to ensure that we have read access
      // to the specified address.
      volatile uint8_t y = Memory::Read<uint8_t>(val);
      USE(y);
      // TODO: Implement "case ZVA:".
      break;
    }
    default:
      VIXL_UNIMPLEMENTED();
  }
}


// clang-format off
#define PAUTH_SYSTEM_MODES(V)                                     \
  V(A1716, 17, ReadXRegister(16),                      kPACKeyIA) \
  V(B1716, 17, ReadXRegister(16),                      kPACKeyIB) \
  V(AZ,    30, 0x00000000,                             kPACKeyIA) \
  V(BZ,    30, 0x00000000,                             kPACKeyIB) \
  V(ASP,   30, ReadXRegister(31, Reg31IsStackPointer), kPACKeyIA) \
  V(BSP,   30, ReadXRegister(31, Reg31IsStackPointer), kPACKeyIB)
// clang-format on


void Simulator::VisitSystem(const Instruction* instr) {
  // Some system instructions hijack their Op and Cp fields to represent a
  // range of immediates instead of indicating a different instruction. This
  // makes the decoding tricky.
  if (instr->GetInstructionBits() == XPACLRI) {
    WriteXRegister(30, StripPAC(ReadXRegister(30), kInstructionPointer));
  } else if (instr->Mask(SystemPStateFMask) == SystemPStateFixed) {
    switch (instr->Mask(SystemPStateMask)) {
      case CFINV:
        ReadNzcv().SetC(!ReadC());
        break;
      case AXFLAG:
        ReadNzcv().SetN(0);
        ReadNzcv().SetZ(ReadNzcv().GetZ() | ReadNzcv().GetV());
        ReadNzcv().SetC(ReadNzcv().GetC() & ~ReadNzcv().GetV());
        ReadNzcv().SetV(0);
        break;
      case XAFLAG: {
        // Can't set the flags in place due to the logical dependencies.
        uint32_t n = (~ReadNzcv().GetC() & ~ReadNzcv().GetZ()) & 1;
        uint32_t z = ReadNzcv().GetZ() & ReadNzcv().GetC();
        uint32_t c = ReadNzcv().GetC() | ReadNzcv().GetZ();
        uint32_t v = ~ReadNzcv().GetC() & ReadNzcv().GetZ();
        ReadNzcv().SetN(n);
        ReadNzcv().SetZ(z);
        ReadNzcv().SetC(c);
        ReadNzcv().SetV(v);
        break;
      }
    }
  } else if (instr->Mask(SystemPAuthFMask) == SystemPAuthFixed) {
    // Check BType allows PACI[AB]SP instructions.
    if (PcIsInGuardedPage()) {
      Instr i = instr->Mask(SystemPAuthMask);
      if ((i == PACIASP) || (i == PACIBSP)) {
        switch (ReadBType()) {
          case BranchFromGuardedNotToIP:
          // TODO: This case depends on the value of SCTLR_EL1.BT0, which we
          // assume here to be zero. This allows execution of PACI[AB]SP when
          // BTYPE is BranchFromGuardedNotToIP (0b11).
          case DefaultBType:
          case BranchFromUnguardedOrToIP:
          case BranchAndLink:
            break;
        }
      }
    }

    switch (instr->Mask(SystemPAuthMask)) {
#define DEFINE_PAUTH_FUNCS(SUFFIX, DST, MOD, KEY)                              \
  case PACI##SUFFIX:                                                           \
    WriteXRegister(DST,                                                        \
                   AddPAC(ReadXRegister(DST), MOD, KEY, kInstructionPointer)); \
    break;                                                                     \
  case AUTI##SUFFIX:                                                           \
    WriteXRegister(DST,                                                        \
                   AuthPAC(ReadXRegister(DST),                                 \
                           MOD,                                                \
                           KEY,                                                \
                           kInstructionPointer));                              \
    break;

      PAUTH_SYSTEM_MODES(DEFINE_PAUTH_FUNCS)
#undef DEFINE_PAUTH_FUNCS
    }
  } else if (instr->Mask(SystemExclusiveMonitorFMask) ==
             SystemExclusiveMonitorFixed) {
    VIXL_ASSERT(instr->Mask(SystemExclusiveMonitorMask) == CLREX);
    switch (instr->Mask(SystemExclusiveMonitorMask)) {
      case CLREX: {
        PrintExclusiveAccessWarning();
        ClearLocalMonitor();
        break;
      }
    }
  } else if (instr->Mask(SystemSysRegFMask) == SystemSysRegFixed) {
    switch (instr->Mask(SystemSysRegMask)) {
      case MRS: {
        switch (instr->GetImmSystemRegister()) {
          case NZCV:
            WriteXRegister(instr->GetRt(), ReadNzcv().GetRawValue());
            break;
          case FPCR:
            WriteXRegister(instr->GetRt(), ReadFpcr().GetRawValue());
            break;
          case RNDR:
          case RNDRRS: {
            uint64_t high = jrand48(rndr_state_);
            uint64_t low = jrand48(rndr_state_);
            uint64_t rand_num = (high << 32) | (low & 0xffffffff);
            WriteXRegister(instr->GetRt(), rand_num);
            // Simulate successful random number generation.
            // TODO: Return failure occasionally as a random number cannot be
            // returned in a period of time.
            ReadNzcv().SetRawValue(NoFlag);
            LogSystemRegister(NZCV);
            break;
          }
          default:
            VIXL_UNIMPLEMENTED();
        }
        break;
      }
      case MSR: {
        switch (instr->GetImmSystemRegister()) {
          case NZCV:
            ReadNzcv().SetRawValue(ReadWRegister(instr->GetRt()));
            LogSystemRegister(NZCV);
            break;
          case FPCR:
            ReadFpcr().SetRawValue(ReadWRegister(instr->GetRt()));
            LogSystemRegister(FPCR);
            break;
          default:
            VIXL_UNIMPLEMENTED();
        }
        break;
      }
    }
  } else if (instr->Mask(SystemHintFMask) == SystemHintFixed) {
    VIXL_ASSERT(instr->Mask(SystemHintMask) == HINT);
    switch (instr->GetImmHint()) {
      case NOP:
      case ESB:
      case CSDB:
      case BTI_jc:
        break;
      case BTI:
        if (PcIsInGuardedPage() && (ReadBType() != DefaultBType)) {
          VIXL_ABORT_WITH_MSG("Executing BTI with wrong BType.");
        }
        break;
      case BTI_c:
        if (PcIsInGuardedPage() && (ReadBType() == BranchFromGuardedNotToIP)) {
          VIXL_ABORT_WITH_MSG("Executing BTI c with wrong BType.");
        }
        break;
      case BTI_j:
        if (PcIsInGuardedPage() && (ReadBType() == BranchAndLink)) {
          VIXL_ABORT_WITH_MSG("Executing BTI j with wrong BType.");
        }
        break;
      default:
        VIXL_UNIMPLEMENTED();
    }
  } else if (instr->Mask(MemBarrierFMask) == MemBarrierFixed) {
    __sync_synchronize();
  } else if ((instr->Mask(SystemSysFMask) == SystemSysFixed)) {
    switch (instr->Mask(SystemSysMask)) {
      case SYS:
        SysOp_W(instr->GetSysOp(), ReadXRegister(instr->GetRt()));
        break;
      default:
        VIXL_UNIMPLEMENTED();
    }
  } else {
    VIXL_UNIMPLEMENTED();
  }
}


void Simulator::VisitException(const Instruction* instr) {
  switch (instr->Mask(ExceptionMask)) {
    case HLT:
      switch (instr->GetImmException()) {
        case kUnreachableOpcode:
          DoUnreachable(instr);
          return;
        case kTraceOpcode:
          DoTrace(instr);
          return;
        case kLogOpcode:
          DoLog(instr);
          return;
        case kPrintfOpcode:
          DoPrintf(instr);
          return;
        case kRuntimeCallOpcode:
          DoRuntimeCall(instr);
          return;
        case kSetCPUFeaturesOpcode:
        case kEnableCPUFeaturesOpcode:
        case kDisableCPUFeaturesOpcode:
          DoConfigureCPUFeatures(instr);
          return;
        case kSaveCPUFeaturesOpcode:
          DoSaveCPUFeatures(instr);
          return;
        case kRestoreCPUFeaturesOpcode:
          DoRestoreCPUFeatures(instr);
          return;
        default:
          HostBreakpoint();
          return;
      }
    case BRK:
      HostBreakpoint();
      return;
    default:
      VIXL_UNIMPLEMENTED();
  }
}


void Simulator::VisitCrypto2RegSHA(const Instruction* instr) {
  VisitUnimplemented(instr);
}


void Simulator::VisitCrypto3RegSHA(const Instruction* instr) {
  VisitUnimplemented(instr);
}


void Simulator::VisitCryptoAES(const Instruction* instr) {
  VisitUnimplemented(instr);
}


void Simulator::VisitNEON2RegMisc(const Instruction* instr) {
  NEONFormatDecoder nfd(instr);
  VectorFormat vf = nfd.GetVectorFormat();

  static const NEONFormatMap map_lp =
      {{23, 22, 30}, {NF_4H, NF_8H, NF_2S, NF_4S, NF_1D, NF_2D}};
  VectorFormat vf_lp = nfd.GetVectorFormat(&map_lp);

  static const NEONFormatMap map_fcvtl = {{22}, {NF_4S, NF_2D}};
  VectorFormat vf_fcvtl = nfd.GetVectorFormat(&map_fcvtl);

  static const NEONFormatMap map_fcvtn = {{22, 30},
                                          {NF_4H, NF_8H, NF_2S, NF_4S}};
  VectorFormat vf_fcvtn = nfd.GetVectorFormat(&map_fcvtn);

  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());

  if (instr->Mask(NEON2RegMiscOpcode) <= NEON_NEG_opcode) {
    // These instructions all use a two bit size field, except NOT and RBIT,
    // which use the field to encode the operation.
    switch (instr->Mask(NEON2RegMiscMask)) {
      case NEON_REV64:
        rev64(vf, rd, rn);
        break;
      case NEON_REV32:
        rev32(vf, rd, rn);
        break;
      case NEON_REV16:
        rev16(vf, rd, rn);
        break;
      case NEON_SUQADD:
        suqadd(vf, rd, rn);
        break;
      case NEON_USQADD:
        usqadd(vf, rd, rn);
        break;
      case NEON_CLS:
        cls(vf, rd, rn);
        break;
      case NEON_CLZ:
        clz(vf, rd, rn);
        break;
      case NEON_CNT:
        cnt(vf, rd, rn);
        break;
      case NEON_SQABS:
        abs(vf, rd, rn).SignedSaturate(vf);
        break;
      case NEON_SQNEG:
        neg(vf, rd, rn).SignedSaturate(vf);
        break;
      case NEON_CMGT_zero:
        cmp(vf, rd, rn, 0, gt);
        break;
      case NEON_CMGE_zero:
        cmp(vf, rd, rn, 0, ge);
        break;
      case NEON_CMEQ_zero:
        cmp(vf, rd, rn, 0, eq);
        break;
      case NEON_CMLE_zero:
        cmp(vf, rd, rn, 0, le);
        break;
      case NEON_CMLT_zero:
        cmp(vf, rd, rn, 0, lt);
        break;
      case NEON_ABS:
        abs(vf, rd, rn);
        break;
      case NEON_NEG:
        neg(vf, rd, rn);
        break;
      case NEON_SADDLP:
        saddlp(vf_lp, rd, rn);
        break;
      case NEON_UADDLP:
        uaddlp(vf_lp, rd, rn);
        break;
      case NEON_SADALP:
        sadalp(vf_lp, rd, rn);
        break;
      case NEON_UADALP:
        uadalp(vf_lp, rd, rn);
        break;
      case NEON_RBIT_NOT:
        vf = nfd.GetVectorFormat(nfd.LogicalFormatMap());
        switch (instr->GetFPType()) {
          case 0:
            not_(vf, rd, rn);
            break;
          case 1:
            rbit(vf, rd, rn);
            break;
          default:
            VIXL_UNIMPLEMENTED();
        }
        break;
    }
  } else {
    VectorFormat fpf = nfd.GetVectorFormat(nfd.FPFormatMap());
    FPRounding fpcr_rounding = static_cast<FPRounding>(ReadFpcr().GetRMode());
    bool inexact_exception = false;
    FrintMode frint_mode = kFrintToInteger;

    // These instructions all use a one bit size field, except XTN, SQXTUN,
    // SHLL, SQXTN and UQXTN, which use a two bit size field.
    switch (instr->Mask(NEON2RegMiscFPMask)) {
      case NEON_FABS:
        fabs_(fpf, rd, rn);
        return;
      case NEON_FNEG:
        fneg(fpf, rd, rn);
        return;
      case NEON_FSQRT:
        fsqrt(fpf, rd, rn);
        return;
      case NEON_FCVTL:
        if (instr->Mask(NEON_Q)) {
          fcvtl2(vf_fcvtl, rd, rn);
        } else {
          fcvtl(vf_fcvtl, rd, rn);
        }
        return;
      case NEON_FCVTN:
        if (instr->Mask(NEON_Q)) {
          fcvtn2(vf_fcvtn, rd, rn);
        } else {
          fcvtn(vf_fcvtn, rd, rn);
        }
        return;
      case NEON_FCVTXN:
        if (instr->Mask(NEON_Q)) {
          fcvtxn2(vf_fcvtn, rd, rn);
        } else {
          fcvtxn(vf_fcvtn, rd, rn);
        }
        return;

      // The following instructions break from the switch statement, rather
      // than return.
      case NEON_FRINT32X:
        inexact_exception = true;
        frint_mode = kFrintToInt32;
        break;  // Use FPCR rounding mode.
      case NEON_FRINT32Z:
        inexact_exception = true;
        frint_mode = kFrintToInt32;
        fpcr_rounding = FPZero;
        break;
      case NEON_FRINT64X:
        inexact_exception = true;
        frint_mode = kFrintToInt64;
        break;  // Use FPCR rounding mode.
      case NEON_FRINT64Z:
        inexact_exception = true;
        frint_mode = kFrintToInt64;
        fpcr_rounding = FPZero;
        break;
      case NEON_FRINTI:
        break;  // Use FPCR rounding mode.
      case NEON_FRINTX:
        inexact_exception = true;
        break;
      case NEON_FRINTA:
        fpcr_rounding = FPTieAway;
        break;
      case NEON_FRINTM:
        fpcr_rounding = FPNegativeInfinity;
        break;
      case NEON_FRINTN:
        fpcr_rounding = FPTieEven;
        break;
      case NEON_FRINTP:
        fpcr_rounding = FPPositiveInfinity;
        break;
      case NEON_FRINTZ:
        fpcr_rounding = FPZero;
        break;

      case NEON_FCVTNS:
        fcvts(fpf, rd, rn, FPTieEven);
        return;
      case NEON_FCVTNU:
        fcvtu(fpf, rd, rn, FPTieEven);
        return;
      case NEON_FCVTPS:
        fcvts(fpf, rd, rn, FPPositiveInfinity);
        return;
      case NEON_FCVTPU:
        fcvtu(fpf, rd, rn, FPPositiveInfinity);
        return;
      case NEON_FCVTMS:
        fcvts(fpf, rd, rn, FPNegativeInfinity);
        return;
      case NEON_FCVTMU:
        fcvtu(fpf, rd, rn, FPNegativeInfinity);
        return;
      case NEON_FCVTZS:
        fcvts(fpf, rd, rn, FPZero);
        return;
      case NEON_FCVTZU:
        fcvtu(fpf, rd, rn, FPZero);
        return;
      case NEON_FCVTAS:
        fcvts(fpf, rd, rn, FPTieAway);
        return;
      case NEON_FCVTAU:
        fcvtu(fpf, rd, rn, FPTieAway);
        return;
      case NEON_SCVTF:
        scvtf(fpf, rd, rn, 0, fpcr_rounding);
        return;
      case NEON_UCVTF:
        ucvtf(fpf, rd, rn, 0, fpcr_rounding);
        return;
      case NEON_URSQRTE:
        ursqrte(fpf, rd, rn);
        return;
      case NEON_URECPE:
        urecpe(fpf, rd, rn);
        return;
      case NEON_FRSQRTE:
        frsqrte(fpf, rd, rn);
        return;
      case NEON_FRECPE:
        frecpe(fpf, rd, rn, fpcr_rounding);
        return;
      case NEON_FCMGT_zero:
        fcmp_zero(fpf, rd, rn, gt);
        return;
      case NEON_FCMGE_zero:
        fcmp_zero(fpf, rd, rn, ge);
        return;
      case NEON_FCMEQ_zero:
        fcmp_zero(fpf, rd, rn, eq);
        return;
      case NEON_FCMLE_zero:
        fcmp_zero(fpf, rd, rn, le);
        return;
      case NEON_FCMLT_zero:
        fcmp_zero(fpf, rd, rn, lt);
        return;
      default:
        if ((NEON_XTN_opcode <= instr->Mask(NEON2RegMiscOpcode)) &&
            (instr->Mask(NEON2RegMiscOpcode) <= NEON_UQXTN_opcode)) {
          switch (instr->Mask(NEON2RegMiscMask)) {
            case NEON_XTN:
              xtn(vf, rd, rn);
              return;
            case NEON_SQXTN:
              sqxtn(vf, rd, rn);
              return;
            case NEON_UQXTN:
              uqxtn(vf, rd, rn);
              return;
            case NEON_SQXTUN:
              sqxtun(vf, rd, rn);
              return;
            case NEON_SHLL:
              vf = nfd.GetVectorFormat(nfd.LongIntegerFormatMap());
              if (instr->Mask(NEON_Q)) {
                shll2(vf, rd, rn);
              } else {
                shll(vf, rd, rn);
              }
              return;
            default:
              VIXL_UNIMPLEMENTED();
          }
        } else {
          VIXL_UNIMPLEMENTED();
        }
    }

    // Only FRINT* instructions fall through the switch above.
    frint(fpf, rd, rn, fpcr_rounding, inexact_exception, frint_mode);
  }
}


void Simulator::VisitNEON2RegMiscFP16(const Instruction* instr) {
  static const NEONFormatMap map_half = {{30}, {NF_4H, NF_8H}};
  NEONFormatDecoder nfd(instr);
  VectorFormat fpf = nfd.GetVectorFormat(&map_half);

  FPRounding fpcr_rounding = static_cast<FPRounding>(ReadFpcr().GetRMode());

  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());

  switch (instr->Mask(NEON2RegMiscFP16Mask)) {
    case NEON_SCVTF_H:
      scvtf(fpf, rd, rn, 0, fpcr_rounding);
      return;
    case NEON_UCVTF_H:
      ucvtf(fpf, rd, rn, 0, fpcr_rounding);
      return;
    case NEON_FCVTNS_H:
      fcvts(fpf, rd, rn, FPTieEven);
      return;
    case NEON_FCVTNU_H:
      fcvtu(fpf, rd, rn, FPTieEven);
      return;
    case NEON_FCVTPS_H:
      fcvts(fpf, rd, rn, FPPositiveInfinity);
      return;
    case NEON_FCVTPU_H:
      fcvtu(fpf, rd, rn, FPPositiveInfinity);
      return;
    case NEON_FCVTMS_H:
      fcvts(fpf, rd, rn, FPNegativeInfinity);
      return;
    case NEON_FCVTMU_H:
      fcvtu(fpf, rd, rn, FPNegativeInfinity);
      return;
    case NEON_FCVTZS_H:
      fcvts(fpf, rd, rn, FPZero);
      return;
    case NEON_FCVTZU_H:
      fcvtu(fpf, rd, rn, FPZero);
      return;
    case NEON_FCVTAS_H:
      fcvts(fpf, rd, rn, FPTieAway);
      return;
    case NEON_FCVTAU_H:
      fcvtu(fpf, rd, rn, FPTieAway);
      return;
    case NEON_FRINTI_H:
      frint(fpf, rd, rn, fpcr_rounding, false);
      return;
    case NEON_FRINTX_H:
      frint(fpf, rd, rn, fpcr_rounding, true);
      return;
    case NEON_FRINTA_H:
      frint(fpf, rd, rn, FPTieAway, false);
      return;
    case NEON_FRINTM_H:
      frint(fpf, rd, rn, FPNegativeInfinity, false);
      return;
    case NEON_FRINTN_H:
      frint(fpf, rd, rn, FPTieEven, false);
      return;
    case NEON_FRINTP_H:
      frint(fpf, rd, rn, FPPositiveInfinity, false);
      return;
    case NEON_FRINTZ_H:
      frint(fpf, rd, rn, FPZero, false);
      return;
    case NEON_FABS_H:
      fabs_(fpf, rd, rn);
      return;
    case NEON_FNEG_H:
      fneg(fpf, rd, rn);
      return;
    case NEON_FSQRT_H:
      fsqrt(fpf, rd, rn);
      return;
    case NEON_FRSQRTE_H:
      frsqrte(fpf, rd, rn);
      return;
    case NEON_FRECPE_H:
      frecpe(fpf, rd, rn, fpcr_rounding);
      return;
    case NEON_FCMGT_H_zero:
      fcmp_zero(fpf, rd, rn, gt);
      return;
    case NEON_FCMGE_H_zero:
      fcmp_zero(fpf, rd, rn, ge);
      return;
    case NEON_FCMEQ_H_zero:
      fcmp_zero(fpf, rd, rn, eq);
      return;
    case NEON_FCMLE_H_zero:
      fcmp_zero(fpf, rd, rn, le);
      return;
    case NEON_FCMLT_H_zero:
      fcmp_zero(fpf, rd, rn, lt);
      return;
    default:
      VIXL_UNIMPLEMENTED();
      return;
  }
}


void Simulator::VisitNEON3Same(const Instruction* instr) {
  NEONFormatDecoder nfd(instr);
  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());
  SimVRegister& rm = ReadVRegister(instr->GetRm());

  if (instr->Mask(NEON3SameLogicalFMask) == NEON3SameLogicalFixed) {
    VectorFormat vf = nfd.GetVectorFormat(nfd.LogicalFormatMap());
    switch (instr->Mask(NEON3SameLogicalMask)) {
      case NEON_AND:
        and_(vf, rd, rn, rm);
        break;
      case NEON_ORR:
        orr(vf, rd, rn, rm);
        break;
      case NEON_ORN:
        orn(vf, rd, rn, rm);
        break;
      case NEON_EOR:
        eor(vf, rd, rn, rm);
        break;
      case NEON_BIC:
        bic(vf, rd, rn, rm);
        break;
      case NEON_BIF:
        bif(vf, rd, rn, rm);
        break;
      case NEON_BIT:
        bit(vf, rd, rn, rm);
        break;
      case NEON_BSL:
        bsl(vf, rd, rn, rm);
        break;
      default:
        VIXL_UNIMPLEMENTED();
    }
  } else if (instr->Mask(NEON3SameFPFMask) == NEON3SameFPFixed) {
    VectorFormat vf = nfd.GetVectorFormat(nfd.FPFormatMap());
    switch (instr->Mask(NEON3SameFPMask)) {
      case NEON_FADD:
        fadd(vf, rd, rn, rm);
        break;
      case NEON_FSUB:
        fsub(vf, rd, rn, rm);
        break;
      case NEON_FMUL:
        fmul(vf, rd, rn, rm);
        break;
      case NEON_FDIV:
        fdiv(vf, rd, rn, rm);
        break;
      case NEON_FMAX:
        fmax(vf, rd, rn, rm);
        break;
      case NEON_FMIN:
        fmin(vf, rd, rn, rm);
        break;
      case NEON_FMAXNM:
        fmaxnm(vf, rd, rn, rm);
        break;
      case NEON_FMINNM:
        fminnm(vf, rd, rn, rm);
        break;
      case NEON_FMLA:
        fmla(vf, rd, rn, rm);
        break;
      case NEON_FMLS:
        fmls(vf, rd, rn, rm);
        break;
      case NEON_FMULX:
        fmulx(vf, rd, rn, rm);
        break;
      case NEON_FACGE:
        fabscmp(vf, rd, rn, rm, ge);
        break;
      case NEON_FACGT:
        fabscmp(vf, rd, rn, rm, gt);
        break;
      case NEON_FCMEQ:
        fcmp(vf, rd, rn, rm, eq);
        break;
      case NEON_FCMGE:
        fcmp(vf, rd, rn, rm, ge);
        break;
      case NEON_FCMGT:
        fcmp(vf, rd, rn, rm, gt);
        break;
      case NEON_FRECPS:
        frecps(vf, rd, rn, rm);
        break;
      case NEON_FRSQRTS:
        frsqrts(vf, rd, rn, rm);
        break;
      case NEON_FABD:
        fabd(vf, rd, rn, rm);
        break;
      case NEON_FADDP:
        faddp(vf, rd, rn, rm);
        break;
      case NEON_FMAXP:
        fmaxp(vf, rd, rn, rm);
        break;
      case NEON_FMAXNMP:
        fmaxnmp(vf, rd, rn, rm);
        break;
      case NEON_FMINP:
        fminp(vf, rd, rn, rm);
        break;
      case NEON_FMINNMP:
        fminnmp(vf, rd, rn, rm);
        break;
      default:
        // FMLAL{2} and FMLSL{2} have special-case encodings.
        switch (instr->Mask(NEON3SameFHMMask)) {
          case NEON_FMLAL:
            fmlal(vf, rd, rn, rm);
            break;
          case NEON_FMLAL2:
            fmlal2(vf, rd, rn, rm);
            break;
          case NEON_FMLSL:
            fmlsl(vf, rd, rn, rm);
            break;
          case NEON_FMLSL2:
            fmlsl2(vf, rd, rn, rm);
            break;
          default:
            VIXL_UNIMPLEMENTED();
        }
    }
  } else {
    VectorFormat vf = nfd.GetVectorFormat();
    switch (instr->Mask(NEON3SameMask)) {
      case NEON_ADD:
        add(vf, rd, rn, rm);
        break;
      case NEON_ADDP:
        addp(vf, rd, rn, rm);
        break;
      case NEON_CMEQ:
        cmp(vf, rd, rn, rm, eq);
        break;
      case NEON_CMGE:
        cmp(vf, rd, rn, rm, ge);
        break;
      case NEON_CMGT:
        cmp(vf, rd, rn, rm, gt);
        break;
      case NEON_CMHI:
        cmp(vf, rd, rn, rm, hi);
        break;
      case NEON_CMHS:
        cmp(vf, rd, rn, rm, hs);
        break;
      case NEON_CMTST:
        cmptst(vf, rd, rn, rm);
        break;
      case NEON_MLS:
        mls(vf, rd, rd, rn, rm);
        break;
      case NEON_MLA:
        mla(vf, rd, rd, rn, rm);
        break;
      case NEON_MUL:
        mul(vf, rd, rn, rm);
        break;
      case NEON_PMUL:
        pmul(vf, rd, rn, rm);
        break;
      case NEON_SMAX:
        smax(vf, rd, rn, rm);
        break;
      case NEON_SMAXP:
        smaxp(vf, rd, rn, rm);
        break;
      case NEON_SMIN:
        smin(vf, rd, rn, rm);
        break;
      case NEON_SMINP:
        sminp(vf, rd, rn, rm);
        break;
      case NEON_SUB:
        sub(vf, rd, rn, rm);
        break;
      case NEON_UMAX:
        umax(vf, rd, rn, rm);
        break;
      case NEON_UMAXP:
        umaxp(vf, rd, rn, rm);
        break;
      case NEON_UMIN:
        umin(vf, rd, rn, rm);
        break;
      case NEON_UMINP:
        uminp(vf, rd, rn, rm);
        break;
      case NEON_SSHL:
        sshl(vf, rd, rn, rm);
        break;
      case NEON_USHL:
        ushl(vf, rd, rn, rm);
        break;
      case NEON_SABD:
        absdiff(vf, rd, rn, rm, true);
        break;
      case NEON_UABD:
        absdiff(vf, rd, rn, rm, false);
        break;
      case NEON_SABA:
        saba(vf, rd, rn, rm);
        break;
      case NEON_UABA:
        uaba(vf, rd, rn, rm);
        break;
      case NEON_UQADD:
        add(vf, rd, rn, rm).UnsignedSaturate(vf);
        break;
      case NEON_SQADD:
        add(vf, rd, rn, rm).SignedSaturate(vf);
        break;
      case NEON_UQSUB:
        sub(vf, rd, rn, rm).UnsignedSaturate(vf);
        break;
      case NEON_SQSUB:
        sub(vf, rd, rn, rm).SignedSaturate(vf);
        break;
      case NEON_SQDMULH:
        sqdmulh(vf, rd, rn, rm);
        break;
      case NEON_SQRDMULH:
        sqrdmulh(vf, rd, rn, rm);
        break;
      case NEON_UQSHL:
        ushl(vf, rd, rn, rm).UnsignedSaturate(vf);
        break;
      case NEON_SQSHL:
        sshl(vf, rd, rn, rm).SignedSaturate(vf);
        break;
      case NEON_URSHL:
        ushl(vf, rd, rn, rm).Round(vf);
        break;
      case NEON_SRSHL:
        sshl(vf, rd, rn, rm).Round(vf);
        break;
      case NEON_UQRSHL:
        ushl(vf, rd, rn, rm).Round(vf).UnsignedSaturate(vf);
        break;
      case NEON_SQRSHL:
        sshl(vf, rd, rn, rm).Round(vf).SignedSaturate(vf);
        break;
      case NEON_UHADD:
        add(vf, rd, rn, rm).Uhalve(vf);
        break;
      case NEON_URHADD:
        add(vf, rd, rn, rm).Uhalve(vf).Round(vf);
        break;
      case NEON_SHADD:
        add(vf, rd, rn, rm).Halve(vf);
        break;
      case NEON_SRHADD:
        add(vf, rd, rn, rm).Halve(vf).Round(vf);
        break;
      case NEON_UHSUB:
        sub(vf, rd, rn, rm).Uhalve(vf);
        break;
      case NEON_SHSUB:
        sub(vf, rd, rn, rm).Halve(vf);
        break;
      default:
        VIXL_UNIMPLEMENTED();
    }
  }
}


void Simulator::VisitNEON3SameFP16(const Instruction* instr) {
  NEONFormatDecoder nfd(instr);
  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());
  SimVRegister& rm = ReadVRegister(instr->GetRm());

  VectorFormat vf = nfd.GetVectorFormat(nfd.FP16FormatMap());
  switch (instr->Mask(NEON3SameFP16Mask)) {
#define SIM_FUNC(A, B) \
  case NEON_##A##_H:   \
    B(vf, rd, rn, rm); \
    break;
    SIM_FUNC(FMAXNM, fmaxnm);
    SIM_FUNC(FMLA, fmla);
    SIM_FUNC(FADD, fadd);
    SIM_FUNC(FMULX, fmulx);
    SIM_FUNC(FMAX, fmax);
    SIM_FUNC(FRECPS, frecps);
    SIM_FUNC(FMINNM, fminnm);
    SIM_FUNC(FMLS, fmls);
    SIM_FUNC(FSUB, fsub);
    SIM_FUNC(FMIN, fmin);
    SIM_FUNC(FRSQRTS, frsqrts);
    SIM_FUNC(FMAXNMP, fmaxnmp);
    SIM_FUNC(FADDP, faddp);
    SIM_FUNC(FMUL, fmul);
    SIM_FUNC(FMAXP, fmaxp);
    SIM_FUNC(FDIV, fdiv);
    SIM_FUNC(FMINNMP, fminnmp);
    SIM_FUNC(FABD, fabd);
    SIM_FUNC(FMINP, fminp);
#undef SIM_FUNC
    case NEON_FCMEQ_H:
      fcmp(vf, rd, rn, rm, eq);
      break;
    case NEON_FCMGE_H:
      fcmp(vf, rd, rn, rm, ge);
      break;
    case NEON_FACGE_H:
      fabscmp(vf, rd, rn, rm, ge);
      break;
    case NEON_FCMGT_H:
      fcmp(vf, rd, rn, rm, gt);
      break;
    case NEON_FACGT_H:
      fabscmp(vf, rd, rn, rm, gt);
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitNEON3SameExtra(const Instruction* instr) {
  NEONFormatDecoder nfd(instr);
  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());
  SimVRegister& rm = ReadVRegister(instr->GetRm());
  int rot = 0;
  VectorFormat vf = nfd.GetVectorFormat();
  if (instr->Mask(NEON3SameExtraFCMLAMask) == NEON_FCMLA) {
    rot = instr->GetImmRotFcmlaVec();
    fcmla(vf, rd, rn, rm, rot);
  } else if (instr->Mask(NEON3SameExtraFCADDMask) == NEON_FCADD) {
    rot = instr->GetImmRotFcadd();
    fcadd(vf, rd, rn, rm, rot);
  } else {
    switch (instr->Mask(NEON3SameExtraMask)) {
      case NEON_SDOT:
        sdot(vf, rd, rn, rm);
        break;
      case NEON_SQRDMLAH:
        sqrdmlah(vf, rd, rn, rm);
        break;
      case NEON_UDOT:
        udot(vf, rd, rn, rm);
        break;
      case NEON_SQRDMLSH:
        sqrdmlsh(vf, rd, rn, rm);
        break;
      default:
        VIXL_UNIMPLEMENTED();
        break;
    }
  }
}


void Simulator::VisitNEON3Different(const Instruction* instr) {
  NEONFormatDecoder nfd(instr);
  VectorFormat vf = nfd.GetVectorFormat();
  VectorFormat vf_l = nfd.GetVectorFormat(nfd.LongIntegerFormatMap());

  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());
  SimVRegister& rm = ReadVRegister(instr->GetRm());

  switch (instr->Mask(NEON3DifferentMask)) {
    case NEON_PMULL:
      pmull(vf_l, rd, rn, rm);
      break;
    case NEON_PMULL2:
      pmull2(vf_l, rd, rn, rm);
      break;
    case NEON_UADDL:
      uaddl(vf_l, rd, rn, rm);
      break;
    case NEON_UADDL2:
      uaddl2(vf_l, rd, rn, rm);
      break;
    case NEON_SADDL:
      saddl(vf_l, rd, rn, rm);
      break;
    case NEON_SADDL2:
      saddl2(vf_l, rd, rn, rm);
      break;
    case NEON_USUBL:
      usubl(vf_l, rd, rn, rm);
      break;
    case NEON_USUBL2:
      usubl2(vf_l, rd, rn, rm);
      break;
    case NEON_SSUBL:
      ssubl(vf_l, rd, rn, rm);
      break;
    case NEON_SSUBL2:
      ssubl2(vf_l, rd, rn, rm);
      break;
    case NEON_SABAL:
      sabal(vf_l, rd, rn, rm);
      break;
    case NEON_SABAL2:
      sabal2(vf_l, rd, rn, rm);
      break;
    case NEON_UABAL:
      uabal(vf_l, rd, rn, rm);
      break;
    case NEON_UABAL2:
      uabal2(vf_l, rd, rn, rm);
      break;
    case NEON_SABDL:
      sabdl(vf_l, rd, rn, rm);
      break;
    case NEON_SABDL2:
      sabdl2(vf_l, rd, rn, rm);
      break;
    case NEON_UABDL:
      uabdl(vf_l, rd, rn, rm);
      break;
    case NEON_UABDL2:
      uabdl2(vf_l, rd, rn, rm);
      break;
    case NEON_SMLAL:
      smlal(vf_l, rd, rn, rm);
      break;
    case NEON_SMLAL2:
      smlal2(vf_l, rd, rn, rm);
      break;
    case NEON_UMLAL:
      umlal(vf_l, rd, rn, rm);
      break;
    case NEON_UMLAL2:
      umlal2(vf_l, rd, rn, rm);
      break;
    case NEON_SMLSL:
      smlsl(vf_l, rd, rn, rm);
      break;
    case NEON_SMLSL2:
      smlsl2(vf_l, rd, rn, rm);
      break;
    case NEON_UMLSL:
      umlsl(vf_l, rd, rn, rm);
      break;
    case NEON_UMLSL2:
      umlsl2(vf_l, rd, rn, rm);
      break;
    case NEON_SMULL:
      smull(vf_l, rd, rn, rm);
      break;
    case NEON_SMULL2:
      smull2(vf_l, rd, rn, rm);
      break;
    case NEON_UMULL:
      umull(vf_l, rd, rn, rm);
      break;
    case NEON_UMULL2:
      umull2(vf_l, rd, rn, rm);
      break;
    case NEON_SQDMLAL:
      sqdmlal(vf_l, rd, rn, rm);
      break;
    case NEON_SQDMLAL2:
      sqdmlal2(vf_l, rd, rn, rm);
      break;
    case NEON_SQDMLSL:
      sqdmlsl(vf_l, rd, rn, rm);
      break;
    case NEON_SQDMLSL2:
      sqdmlsl2(vf_l, rd, rn, rm);
      break;
    case NEON_SQDMULL:
      sqdmull(vf_l, rd, rn, rm);
      break;
    case NEON_SQDMULL2:
      sqdmull2(vf_l, rd, rn, rm);
      break;
    case NEON_UADDW:
      uaddw(vf_l, rd, rn, rm);
      break;
    case NEON_UADDW2:
      uaddw2(vf_l, rd, rn, rm);
      break;
    case NEON_SADDW:
      saddw(vf_l, rd, rn, rm);
      break;
    case NEON_SADDW2:
      saddw2(vf_l, rd, rn, rm);
      break;
    case NEON_USUBW:
      usubw(vf_l, rd, rn, rm);
      break;
    case NEON_USUBW2:
      usubw2(vf_l, rd, rn, rm);
      break;
    case NEON_SSUBW:
      ssubw(vf_l, rd, rn, rm);
      break;
    case NEON_SSUBW2:
      ssubw2(vf_l, rd, rn, rm);
      break;
    case NEON_ADDHN:
      addhn(vf, rd, rn, rm);
      break;
    case NEON_ADDHN2:
      addhn2(vf, rd, rn, rm);
      break;
    case NEON_RADDHN:
      raddhn(vf, rd, rn, rm);
      break;
    case NEON_RADDHN2:
      raddhn2(vf, rd, rn, rm);
      break;
    case NEON_SUBHN:
      subhn(vf, rd, rn, rm);
      break;
    case NEON_SUBHN2:
      subhn2(vf, rd, rn, rm);
      break;
    case NEON_RSUBHN:
      rsubhn(vf, rd, rn, rm);
      break;
    case NEON_RSUBHN2:
      rsubhn2(vf, rd, rn, rm);
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }
}


void Simulator::VisitNEONAcrossLanes(const Instruction* instr) {
  NEONFormatDecoder nfd(instr);

  static const NEONFormatMap map_half = {{30}, {NF_4H, NF_8H}};

  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());

  if (instr->Mask(NEONAcrossLanesFP16FMask) == NEONAcrossLanesFP16Fixed) {
    VectorFormat vf = nfd.GetVectorFormat(&map_half);
    switch (instr->Mask(NEONAcrossLanesFP16Mask)) {
      case NEON_FMAXV_H:
        fmaxv(vf, rd, rn);
        break;
      case NEON_FMINV_H:
        fminv(vf, rd, rn);
        break;
      case NEON_FMAXNMV_H:
        fmaxnmv(vf, rd, rn);
        break;
      case NEON_FMINNMV_H:
        fminnmv(vf, rd, rn);
        break;
      default:
        VIXL_UNIMPLEMENTED();
    }
  } else if (instr->Mask(NEONAcrossLanesFPFMask) == NEONAcrossLanesFPFixed) {
    // The input operand's VectorFormat is passed for these instructions.
    VectorFormat vf = nfd.GetVectorFormat(nfd.FPFormatMap());

    switch (instr->Mask(NEONAcrossLanesFPMask)) {
      case NEON_FMAXV:
        fmaxv(vf, rd, rn);
        break;
      case NEON_FMINV:
        fminv(vf, rd, rn);
        break;
      case NEON_FMAXNMV:
        fmaxnmv(vf, rd, rn);
        break;
      case NEON_FMINNMV:
        fminnmv(vf, rd, rn);
        break;
      default:
        VIXL_UNIMPLEMENTED();
    }
  } else {
    VectorFormat vf = nfd.GetVectorFormat();

    switch (instr->Mask(NEONAcrossLanesMask)) {
      case NEON_ADDV:
        addv(vf, rd, rn);
        break;
      case NEON_SMAXV:
        smaxv(vf, rd, rn);
        break;
      case NEON_SMINV:
        sminv(vf, rd, rn);
        break;
      case NEON_UMAXV:
        umaxv(vf, rd, rn);
        break;
      case NEON_UMINV:
        uminv(vf, rd, rn);
        break;
      case NEON_SADDLV:
        saddlv(vf, rd, rn);
        break;
      case NEON_UADDLV:
        uaddlv(vf, rd, rn);
        break;
      default:
        VIXL_UNIMPLEMENTED();
    }
  }
}


void Simulator::VisitNEONByIndexedElement(const Instruction* instr) {
  NEONFormatDecoder nfd(instr);
  static const NEONFormatMap map_half = {{30}, {NF_4H, NF_8H}};
  VectorFormat vf_r = nfd.GetVectorFormat();
  VectorFormat vf_half = nfd.GetVectorFormat(&map_half);
  VectorFormat vf = nfd.GetVectorFormat(nfd.LongIntegerFormatMap());

  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());

  ByElementOp Op = NULL;

  int rm_reg = instr->GetRm();
  int rm_low_reg = instr->GetRmLow16();
  int index = (instr->GetNEONH() << 1) | instr->GetNEONL();
  int index_hlm = (index << 1) | instr->GetNEONM();

  switch (instr->Mask(NEONByIndexedElementFPLongMask)) {
    // These are oddballs and are best handled as special cases.
    // - Rm is encoded with only 4 bits (and must be in the lower 16 registers).
    // - The index is always H:L:M.
    case NEON_FMLAL_H_byelement:
      fmlal(vf_r, rd, rn, ReadVRegister(rm_low_reg), index_hlm);
      return;
    case NEON_FMLAL2_H_byelement:
      fmlal2(vf_r, rd, rn, ReadVRegister(rm_low_reg), index_hlm);
      return;
    case NEON_FMLSL_H_byelement:
      fmlsl(vf_r, rd, rn, ReadVRegister(rm_low_reg), index_hlm);
      return;
    case NEON_FMLSL2_H_byelement:
      fmlsl2(vf_r, rd, rn, ReadVRegister(rm_low_reg), index_hlm);
      return;
  }

  if (instr->GetNEONSize() == 1) {
    rm_reg = rm_low_reg;
    index = index_hlm;
  }

  switch (instr->Mask(NEONByIndexedElementMask)) {
    case NEON_MUL_byelement:
      Op = &Simulator::mul;
      vf = vf_r;
      break;
    case NEON_MLA_byelement:
      Op = &Simulator::mla;
      vf = vf_r;
      break;
    case NEON_MLS_byelement:
      Op = &Simulator::mls;
      vf = vf_r;
      break;
    case NEON_SQDMULH_byelement:
      Op = &Simulator::sqdmulh;
      vf = vf_r;
      break;
    case NEON_SQRDMULH_byelement:
      Op = &Simulator::sqrdmulh;
      vf = vf_r;
      break;
    case NEON_SDOT_byelement:
      Op = &Simulator::sdot;
      vf = vf_r;
      break;
    case NEON_SQRDMLAH_byelement:
      Op = &Simulator::sqrdmlah;
      vf = vf_r;
      break;
    case NEON_UDOT_byelement:
      Op = &Simulator::udot;
      vf = vf_r;
      break;
    case NEON_SQRDMLSH_byelement:
      Op = &Simulator::sqrdmlsh;
      vf = vf_r;
      break;
    case NEON_SMULL_byelement:
      if (instr->Mask(NEON_Q)) {
        Op = &Simulator::smull2;
      } else {
        Op = &Simulator::smull;
      }
      break;
    case NEON_UMULL_byelement:
      if (instr->Mask(NEON_Q)) {
        Op = &Simulator::umull2;
      } else {
        Op = &Simulator::umull;
      }
      break;
    case NEON_SMLAL_byelement:
      if (instr->Mask(NEON_Q)) {
        Op = &Simulator::smlal2;
      } else {
        Op = &Simulator::smlal;
      }
      break;
    case NEON_UMLAL_byelement:
      if (instr->Mask(NEON_Q)) {
        Op = &Simulator::umlal2;
      } else {
        Op = &Simulator::umlal;
      }
      break;
    case NEON_SMLSL_byelement:
      if (instr->Mask(NEON_Q)) {
        Op = &Simulator::smlsl2;
      } else {
        Op = &Simulator::smlsl;
      }
      break;
    case NEON_UMLSL_byelement:
      if (instr->Mask(NEON_Q)) {
        Op = &Simulator::umlsl2;
      } else {
        Op = &Simulator::umlsl;
      }
      break;
    case NEON_SQDMULL_byelement:
      if (instr->Mask(NEON_Q)) {
        Op = &Simulator::sqdmull2;
      } else {
        Op = &Simulator::sqdmull;
      }
      break;
    case NEON_SQDMLAL_byelement:
      if (instr->Mask(NEON_Q)) {
        Op = &Simulator::sqdmlal2;
      } else {
        Op = &Simulator::sqdmlal;
      }
      break;
    case NEON_SQDMLSL_byelement:
      if (instr->Mask(NEON_Q)) {
        Op = &Simulator::sqdmlsl2;
      } else {
        Op = &Simulator::sqdmlsl;
      }
      break;
    default:
      index = instr->GetNEONH();
      if (instr->GetFPType() == 0) {
        rm_reg &= 0xf;
        index = (index << 2) | (instr->GetNEONL() << 1) | instr->GetNEONM();
      } else if ((instr->GetFPType() & 1) == 0) {
        index = (index << 1) | instr->GetNEONL();
      }

      vf = nfd.GetVectorFormat(nfd.FPFormatMap());

      switch (instr->Mask(NEONByIndexedElementFPMask)) {
        case NEON_FMUL_H_byelement:
          vf = vf_half;
          VIXL_FALLTHROUGH();
        case NEON_FMUL_byelement:
          Op = &Simulator::fmul;
          break;
        case NEON_FMLA_H_byelement:
          vf = vf_half;
          VIXL_FALLTHROUGH();
        case NEON_FMLA_byelement:
          Op = &Simulator::fmla;
          break;
        case NEON_FMLS_H_byelement:
          vf = vf_half;
          VIXL_FALLTHROUGH();
        case NEON_FMLS_byelement:
          Op = &Simulator::fmls;
          break;
        case NEON_FMULX_H_byelement:
          vf = vf_half;
          VIXL_FALLTHROUGH();
        case NEON_FMULX_byelement:
          Op = &Simulator::fmulx;
          break;
        default:
          if (instr->GetNEONSize() == 2) {
            index = instr->GetNEONH();
          } else {
            index = (instr->GetNEONH() << 1) | instr->GetNEONL();
          }
          switch (instr->Mask(NEONByIndexedElementFPComplexMask)) {
            case NEON_FCMLA_byelement:
              vf = vf_r;
              fcmla(vf,
                    rd,
                    rn,
                    ReadVRegister(instr->GetRm()),
                    index,
                    instr->GetImmRotFcmlaSca());
              return;
            default:
              VIXL_UNIMPLEMENTED();
          }
      }
  }

  (this->*Op)(vf, rd, rn, ReadVRegister(rm_reg), index);
}


void Simulator::VisitNEONCopy(const Instruction* instr) {
  NEONFormatDecoder nfd(instr, NEONFormatDecoder::TriangularFormatMap());
  VectorFormat vf = nfd.GetVectorFormat();

  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());
  int imm5 = instr->GetImmNEON5();
  int tz = CountTrailingZeros(imm5, 32);
  int reg_index = imm5 >> (tz + 1);

  if (instr->Mask(NEONCopyInsElementMask) == NEON_INS_ELEMENT) {
    int imm4 = instr->GetImmNEON4();
    int rn_index = imm4 >> tz;
    ins_element(vf, rd, reg_index, rn, rn_index);
  } else if (instr->Mask(NEONCopyInsGeneralMask) == NEON_INS_GENERAL) {
    ins_immediate(vf, rd, reg_index, ReadXRegister(instr->GetRn()));
  } else if (instr->Mask(NEONCopyUmovMask) == NEON_UMOV) {
    uint64_t value = LogicVRegister(rn).Uint(vf, reg_index);
    value &= MaxUintFromFormat(vf);
    WriteXRegister(instr->GetRd(), value);
  } else if (instr->Mask(NEONCopyUmovMask) == NEON_SMOV) {
    int64_t value = LogicVRegister(rn).Int(vf, reg_index);
    if (instr->GetNEONQ()) {
      WriteXRegister(instr->GetRd(), value);
    } else {
      WriteWRegister(instr->GetRd(), (int32_t)value);
    }
  } else if (instr->Mask(NEONCopyDupElementMask) == NEON_DUP_ELEMENT) {
    dup_element(vf, rd, rn, reg_index);
  } else if (instr->Mask(NEONCopyDupGeneralMask) == NEON_DUP_GENERAL) {
    dup_immediate(vf, rd, ReadXRegister(instr->GetRn()));
  } else {
    VIXL_UNIMPLEMENTED();
  }
}


void Simulator::VisitNEONExtract(const Instruction* instr) {
  NEONFormatDecoder nfd(instr, NEONFormatDecoder::LogicalFormatMap());
  VectorFormat vf = nfd.GetVectorFormat();
  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());
  SimVRegister& rm = ReadVRegister(instr->GetRm());
  if (instr->Mask(NEONExtractMask) == NEON_EXT) {
    int index = instr->GetImmNEONExt();
    ext(vf, rd, rn, rm, index);
  } else {
    VIXL_UNIMPLEMENTED();
  }
}


void Simulator::NEONLoadStoreMultiStructHelper(const Instruction* instr,
                                               AddrMode addr_mode) {
  NEONFormatDecoder nfd(instr, NEONFormatDecoder::LoadStoreFormatMap());
  VectorFormat vf = nfd.GetVectorFormat();

  uint64_t addr_base = ReadXRegister(instr->GetRn(), Reg31IsStackPointer);
  int reg_size = RegisterSizeInBytesFromFormat(vf);

  int reg[4];
  uint64_t addr[4];
  for (int i = 0; i < 4; i++) {
    reg[i] = (instr->GetRt() + i) % kNumberOfVRegisters;
    addr[i] = addr_base + (i * reg_size);
  }
  int count = 1;
  bool log_read = true;

  // Bit 23 determines whether this is an offset or post-index addressing mode.
  // In offset mode, bits 20 to 16 should be zero; these bits encode the
  // register or immediate in post-index mode.
  if ((instr->ExtractBit(23) == 0) && (instr->ExtractBits(20, 16) != 0)) {
    VIXL_UNREACHABLE();
  }

  // We use the PostIndex mask here, as it works in this case for both Offset
  // and PostIndex addressing.
  switch (instr->Mask(NEONLoadStoreMultiStructPostIndexMask)) {
    case NEON_LD1_4v:
    case NEON_LD1_4v_post:
      ld1(vf, ReadVRegister(reg[3]), addr[3]);
      count++;
      VIXL_FALLTHROUGH();
    case NEON_LD1_3v:
    case NEON_LD1_3v_post:
      ld1(vf, ReadVRegister(reg[2]), addr[2]);
      count++;
      VIXL_FALLTHROUGH();
    case NEON_LD1_2v:
    case NEON_LD1_2v_post:
      ld1(vf, ReadVRegister(reg[1]), addr[1]);
      count++;
      VIXL_FALLTHROUGH();
    case NEON_LD1_1v:
    case NEON_LD1_1v_post:
      ld1(vf, ReadVRegister(reg[0]), addr[0]);
      break;
    case NEON_ST1_4v:
    case NEON_ST1_4v_post:
      st1(vf, ReadVRegister(reg[3]), addr[3]);
      count++;
      VIXL_FALLTHROUGH();
    case NEON_ST1_3v:
    case NEON_ST1_3v_post:
      st1(vf, ReadVRegister(reg[2]), addr[2]);
      count++;
      VIXL_FALLTHROUGH();
    case NEON_ST1_2v:
    case NEON_ST1_2v_post:
      st1(vf, ReadVRegister(reg[1]), addr[1]);
      count++;
      VIXL_FALLTHROUGH();
    case NEON_ST1_1v:
    case NEON_ST1_1v_post:
      st1(vf, ReadVRegister(reg[0]), addr[0]);
      log_read = false;
      break;
    case NEON_LD2_post:
    case NEON_LD2:
      ld2(vf, ReadVRegister(reg[0]), ReadVRegister(reg[1]), addr[0]);
      count = 2;
      break;
    case NEON_ST2:
    case NEON_ST2_post:
      st2(vf, ReadVRegister(reg[0]), ReadVRegister(reg[1]), addr[0]);
      count = 2;
      log_read = false;
      break;
    case NEON_LD3_post:
    case NEON_LD3:
      ld3(vf,
          ReadVRegister(reg[0]),
          ReadVRegister(reg[1]),
          ReadVRegister(reg[2]),
          addr[0]);
      count = 3;
      break;
    case NEON_ST3:
    case NEON_ST3_post:
      st3(vf,
          ReadVRegister(reg[0]),
          ReadVRegister(reg[1]),
          ReadVRegister(reg[2]),
          addr[0]);
      count = 3;
      log_read = false;
      break;
    case NEON_ST4:
    case NEON_ST4_post:
      st4(vf,
          ReadVRegister(reg[0]),
          ReadVRegister(reg[1]),
          ReadVRegister(reg[2]),
          ReadVRegister(reg[3]),
          addr[0]);
      count = 4;
      log_read = false;
      break;
    case NEON_LD4_post:
    case NEON_LD4:
      ld4(vf,
          ReadVRegister(reg[0]),
          ReadVRegister(reg[1]),
          ReadVRegister(reg[2]),
          ReadVRegister(reg[3]),
          addr[0]);
      count = 4;
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }

  // Explicitly log the register update whilst we have type information.
  for (int i = 0; i < count; i++) {
    // For de-interleaving loads, only print the base address.
    int lane_size = LaneSizeInBytesFromFormat(vf);
    PrintRegisterFormat format = GetPrintRegisterFormatTryFP(
        GetPrintRegisterFormatForSize(reg_size, lane_size));
    if (log_read) {
      LogVRead(addr_base, reg[i], format);
    } else {
      LogVWrite(addr_base, reg[i], format);
    }
  }

  if (addr_mode == PostIndex) {
    int rm = instr->GetRm();
    // The immediate post index addressing mode is indicated by rm = 31.
    // The immediate is implied by the number of vector registers used.
    addr_base += (rm == 31) ? RegisterSizeInBytesFromFormat(vf) * count
                            : ReadXRegister(rm);
    WriteXRegister(instr->GetRn(), addr_base);
  } else {
    VIXL_ASSERT(addr_mode == Offset);
  }
}


void Simulator::VisitNEONLoadStoreMultiStruct(const Instruction* instr) {
  NEONLoadStoreMultiStructHelper(instr, Offset);
}


void Simulator::VisitNEONLoadStoreMultiStructPostIndex(
    const Instruction* instr) {
  NEONLoadStoreMultiStructHelper(instr, PostIndex);
}


void Simulator::NEONLoadStoreSingleStructHelper(const Instruction* instr,
                                                AddrMode addr_mode) {
  uint64_t addr = ReadXRegister(instr->GetRn(), Reg31IsStackPointer);
  int rt = instr->GetRt();

  // Bit 23 determines whether this is an offset or post-index addressing mode.
  // In offset mode, bits 20 to 16 should be zero; these bits encode the
  // register or immediate in post-index mode.
  if ((instr->ExtractBit(23) == 0) && (instr->ExtractBits(20, 16) != 0)) {
    VIXL_UNREACHABLE();
  }

  // We use the PostIndex mask here, as it works in this case for both Offset
  // and PostIndex addressing.
  bool do_load = false;

  NEONFormatDecoder nfd(instr, NEONFormatDecoder::LoadStoreFormatMap());
  VectorFormat vf_t = nfd.GetVectorFormat();

  VectorFormat vf = kFormat16B;
  switch (instr->Mask(NEONLoadStoreSingleStructPostIndexMask)) {
    case NEON_LD1_b:
    case NEON_LD1_b_post:
    case NEON_LD2_b:
    case NEON_LD2_b_post:
    case NEON_LD3_b:
    case NEON_LD3_b_post:
    case NEON_LD4_b:
    case NEON_LD4_b_post:
      do_load = true;
      VIXL_FALLTHROUGH();
    case NEON_ST1_b:
    case NEON_ST1_b_post:
    case NEON_ST2_b:
    case NEON_ST2_b_post:
    case NEON_ST3_b:
    case NEON_ST3_b_post:
    case NEON_ST4_b:
    case NEON_ST4_b_post:
      break;

    case NEON_LD1_h:
    case NEON_LD1_h_post:
    case NEON_LD2_h:
    case NEON_LD2_h_post:
    case NEON_LD3_h:
    case NEON_LD3_h_post:
    case NEON_LD4_h:
    case NEON_LD4_h_post:
      do_load = true;
      VIXL_FALLTHROUGH();
    case NEON_ST1_h:
    case NEON_ST1_h_post:
    case NEON_ST2_h:
    case NEON_ST2_h_post:
    case NEON_ST3_h:
    case NEON_ST3_h_post:
    case NEON_ST4_h:
    case NEON_ST4_h_post:
      vf = kFormat8H;
      break;
    case NEON_LD1_s:
    case NEON_LD1_s_post:
    case NEON_LD2_s:
    case NEON_LD2_s_post:
    case NEON_LD3_s:
    case NEON_LD3_s_post:
    case NEON_LD4_s:
    case NEON_LD4_s_post:
      do_load = true;
      VIXL_FALLTHROUGH();
    case NEON_ST1_s:
    case NEON_ST1_s_post:
    case NEON_ST2_s:
    case NEON_ST2_s_post:
    case NEON_ST3_s:
    case NEON_ST3_s_post:
    case NEON_ST4_s:
    case NEON_ST4_s_post: {
      VIXL_STATIC_ASSERT((NEON_LD1_s | (1 << NEONLSSize_offset)) == NEON_LD1_d);
      VIXL_STATIC_ASSERT((NEON_LD1_s_post | (1 << NEONLSSize_offset)) ==
                         NEON_LD1_d_post);
      VIXL_STATIC_ASSERT((NEON_ST1_s | (1 << NEONLSSize_offset)) == NEON_ST1_d);
      VIXL_STATIC_ASSERT((NEON_ST1_s_post | (1 << NEONLSSize_offset)) ==
                         NEON_ST1_d_post);
      vf = ((instr->GetNEONLSSize() & 1) == 0) ? kFormat4S : kFormat2D;
      break;
    }

    case NEON_LD1R:
    case NEON_LD1R_post: {
      vf = vf_t;
      ld1r(vf, ReadVRegister(rt), addr);
      do_load = true;
      break;
    }

    case NEON_LD2R:
    case NEON_LD2R_post: {
      vf = vf_t;
      int rt2 = (rt + 1) % kNumberOfVRegisters;
      ld2r(vf, ReadVRegister(rt), ReadVRegister(rt2), addr);
      do_load = true;
      break;
    }

    case NEON_LD3R:
    case NEON_LD3R_post: {
      vf = vf_t;
      int rt2 = (rt + 1) % kNumberOfVRegisters;
      int rt3 = (rt2 + 1) % kNumberOfVRegisters;
      ld3r(vf, ReadVRegister(rt), ReadVRegister(rt2), ReadVRegister(rt3), addr);
      do_load = true;
      break;
    }

    case NEON_LD4R:
    case NEON_LD4R_post: {
      vf = vf_t;
      int rt2 = (rt + 1) % kNumberOfVRegisters;
      int rt3 = (rt2 + 1) % kNumberOfVRegisters;
      int rt4 = (rt3 + 1) % kNumberOfVRegisters;
      ld4r(vf,
           ReadVRegister(rt),
           ReadVRegister(rt2),
           ReadVRegister(rt3),
           ReadVRegister(rt4),
           addr);
      do_load = true;
      break;
    }
    default:
      VIXL_UNIMPLEMENTED();
  }

  PrintRegisterFormat print_format =
      GetPrintRegisterFormatTryFP(GetPrintRegisterFormat(vf));
  // Make sure that the print_format only includes a single lane.
  print_format =
      static_cast<PrintRegisterFormat>(print_format & ~kPrintRegAsVectorMask);

  int esize = LaneSizeInBytesFromFormat(vf);
  int index_shift = LaneSizeInBytesLog2FromFormat(vf);
  int lane = instr->GetNEONLSIndex(index_shift);
  int scale = 0;
  int rt2 = (rt + 1) % kNumberOfVRegisters;
  int rt3 = (rt2 + 1) % kNumberOfVRegisters;
  int rt4 = (rt3 + 1) % kNumberOfVRegisters;
  switch (instr->Mask(NEONLoadStoreSingleLenMask)) {
    case NEONLoadStoreSingle1:
      scale = 1;
      if (do_load) {
        ld1(vf, ReadVRegister(rt), lane, addr);
        LogVRead(addr, rt, print_format, lane);
      } else {
        st1(vf, ReadVRegister(rt), lane, addr);
        LogVWrite(addr, rt, print_format, lane);
      }
      break;
    case NEONLoadStoreSingle2:
      scale = 2;
      if (do_load) {
        ld2(vf, ReadVRegister(rt), ReadVRegister(rt2), lane, addr);
        LogVRead(addr, rt, print_format, lane);
        LogVRead(addr + esize, rt2, print_format, lane);
      } else {
        st2(vf, ReadVRegister(rt), ReadVRegister(rt2), lane, addr);
        LogVWrite(addr, rt, print_format, lane);
        LogVWrite(addr + esize, rt2, print_format, lane);
      }
      break;
    case NEONLoadStoreSingle3:
      scale = 3;
      if (do_load) {
        ld3(vf,
            ReadVRegister(rt),
            ReadVRegister(rt2),
            ReadVRegister(rt3),
            lane,
            addr);
        LogVRead(addr, rt, print_format, lane);
        LogVRead(addr + esize, rt2, print_format, lane);
        LogVRead(addr + (2 * esize), rt3, print_format, lane);
      } else {
        st3(vf,
            ReadVRegister(rt),
            ReadVRegister(rt2),
            ReadVRegister(rt3),
            lane,
            addr);
        LogVWrite(addr, rt, print_format, lane);
        LogVWrite(addr + esize, rt2, print_format, lane);
        LogVWrite(addr + (2 * esize), rt3, print_format, lane);
      }
      break;
    case NEONLoadStoreSingle4:
      scale = 4;
      if (do_load) {
        ld4(vf,
            ReadVRegister(rt),
            ReadVRegister(rt2),
            ReadVRegister(rt3),
            ReadVRegister(rt4),
            lane,
            addr);
        LogVRead(addr, rt, print_format, lane);
        LogVRead(addr + esize, rt2, print_format, lane);
        LogVRead(addr + (2 * esize), rt3, print_format, lane);
        LogVRead(addr + (3 * esize), rt4, print_format, lane);
      } else {
        st4(vf,
            ReadVRegister(rt),
            ReadVRegister(rt2),
            ReadVRegister(rt3),
            ReadVRegister(rt4),
            lane,
            addr);
        LogVWrite(addr, rt, print_format, lane);
        LogVWrite(addr + esize, rt2, print_format, lane);
        LogVWrite(addr + (2 * esize), rt3, print_format, lane);
        LogVWrite(addr + (3 * esize), rt4, print_format, lane);
      }
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }

  if (addr_mode == PostIndex) {
    int rm = instr->GetRm();
    int lane_size = LaneSizeInBytesFromFormat(vf);
    WriteXRegister(instr->GetRn(),
                   addr +
                       ((rm == 31) ? (scale * lane_size) : ReadXRegister(rm)));
  }
}


void Simulator::VisitNEONLoadStoreSingleStruct(const Instruction* instr) {
  NEONLoadStoreSingleStructHelper(instr, Offset);
}


void Simulator::VisitNEONLoadStoreSingleStructPostIndex(
    const Instruction* instr) {
  NEONLoadStoreSingleStructHelper(instr, PostIndex);
}


void Simulator::VisitNEONModifiedImmediate(const Instruction* instr) {
  SimVRegister& rd = ReadVRegister(instr->GetRd());
  int cmode = instr->GetNEONCmode();
  int cmode_3_1 = (cmode >> 1) & 7;
  int cmode_3 = (cmode >> 3) & 1;
  int cmode_2 = (cmode >> 2) & 1;
  int cmode_1 = (cmode >> 1) & 1;
  int cmode_0 = cmode & 1;
  int half_enc = instr->ExtractBit(11);
  int q = instr->GetNEONQ();
  int op_bit = instr->GetNEONModImmOp();
  uint64_t imm8 = instr->GetImmNEONabcdefgh();
  // Find the format and immediate value
  uint64_t imm = 0;
  VectorFormat vform = kFormatUndefined;
  switch (cmode_3_1) {
    case 0x0:
    case 0x1:
    case 0x2:
    case 0x3:
      vform = (q == 1) ? kFormat4S : kFormat2S;
      imm = imm8 << (8 * cmode_3_1);
      break;
    case 0x4:
    case 0x5:
      vform = (q == 1) ? kFormat8H : kFormat4H;
      imm = imm8 << (8 * cmode_1);
      break;
    case 0x6:
      vform = (q == 1) ? kFormat4S : kFormat2S;
      if (cmode_0 == 0) {
        imm = imm8 << 8 | 0x000000ff;
      } else {
        imm = imm8 << 16 | 0x0000ffff;
      }
      break;
    case 0x7:
      if (cmode_0 == 0 && op_bit == 0) {
        vform = q ? kFormat16B : kFormat8B;
        imm = imm8;
      } else if (cmode_0 == 0 && op_bit == 1) {
        vform = q ? kFormat2D : kFormat1D;
        imm = 0;
        for (int i = 0; i < 8; ++i) {
          if (imm8 & (1 << i)) {
            imm |= (UINT64_C(0xff) << (8 * i));
          }
        }
      } else {  // cmode_0 == 1, cmode == 0xf.
        if (half_enc == 1) {
          vform = q ? kFormat8H : kFormat4H;
          imm = Float16ToRawbits(instr->GetImmNEONFP16());
        } else if (op_bit == 0) {
          vform = q ? kFormat4S : kFormat2S;
          imm = FloatToRawbits(instr->GetImmNEONFP32());
        } else if (q == 1) {
          vform = kFormat2D;
          imm = DoubleToRawbits(instr->GetImmNEONFP64());
        } else {
          VIXL_ASSERT((q == 0) && (op_bit == 1) && (cmode == 0xf));
          VisitUnallocated(instr);
        }
      }
      break;
    default:
      VIXL_UNREACHABLE();
      break;
  }

  // Find the operation
  NEONModifiedImmediateOp op;
  if (cmode_3 == 0) {
    if (cmode_0 == 0) {
      op = op_bit ? NEONModifiedImmediate_MVNI : NEONModifiedImmediate_MOVI;
    } else {  // cmode<0> == '1'
      op = op_bit ? NEONModifiedImmediate_BIC : NEONModifiedImmediate_ORR;
    }
  } else {  // cmode<3> == '1'
    if (cmode_2 == 0) {
      if (cmode_0 == 0) {
        op = op_bit ? NEONModifiedImmediate_MVNI : NEONModifiedImmediate_MOVI;
      } else {  // cmode<0> == '1'
        op = op_bit ? NEONModifiedImmediate_BIC : NEONModifiedImmediate_ORR;
      }
    } else {  // cmode<2> == '1'
      if (cmode_1 == 0) {
        op = op_bit ? NEONModifiedImmediate_MVNI : NEONModifiedImmediate_MOVI;
      } else {  // cmode<1> == '1'
        if (cmode_0 == 0) {
          op = NEONModifiedImmediate_MOVI;
        } else {  // cmode<0> == '1'
          op = NEONModifiedImmediate_MOVI;
        }
      }
    }
  }

  // Call the logic function
  if (op == NEONModifiedImmediate_ORR) {
    orr(vform, rd, rd, imm);
  } else if (op == NEONModifiedImmediate_BIC) {
    bic(vform, rd, rd, imm);
  } else if (op == NEONModifiedImmediate_MOVI) {
    movi(vform, rd, imm);
  } else if (op == NEONModifiedImmediate_MVNI) {
    mvni(vform, rd, imm);
  } else {
    VisitUnimplemented(instr);
  }
}


void Simulator::VisitNEONScalar2RegMisc(const Instruction* instr) {
  NEONFormatDecoder nfd(instr, NEONFormatDecoder::ScalarFormatMap());
  VectorFormat vf = nfd.GetVectorFormat();

  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());

  if (instr->Mask(NEON2RegMiscOpcode) <= NEON_NEG_scalar_opcode) {
    // These instructions all use a two bit size field, except NOT and RBIT,
    // which use the field to encode the operation.
    switch (instr->Mask(NEONScalar2RegMiscMask)) {
      case NEON_CMEQ_zero_scalar:
        cmp(vf, rd, rn, 0, eq);
        break;
      case NEON_CMGE_zero_scalar:
        cmp(vf, rd, rn, 0, ge);
        break;
      case NEON_CMGT_zero_scalar:
        cmp(vf, rd, rn, 0, gt);
        break;
      case NEON_CMLT_zero_scalar:
        cmp(vf, rd, rn, 0, lt);
        break;
      case NEON_CMLE_zero_scalar:
        cmp(vf, rd, rn, 0, le);
        break;
      case NEON_ABS_scalar:
        abs(vf, rd, rn);
        break;
      case NEON_SQABS_scalar:
        abs(vf, rd, rn).SignedSaturate(vf);
        break;
      case NEON_NEG_scalar:
        neg(vf, rd, rn);
        break;
      case NEON_SQNEG_scalar:
        neg(vf, rd, rn).SignedSaturate(vf);
        break;
      case NEON_SUQADD_scalar:
        suqadd(vf, rd, rn);
        break;
      case NEON_USQADD_scalar:
        usqadd(vf, rd, rn);
        break;
      default:
        VIXL_UNIMPLEMENTED();
        break;
    }
  } else {
    VectorFormat fpf = nfd.GetVectorFormat(nfd.FPScalarFormatMap());
    FPRounding fpcr_rounding = static_cast<FPRounding>(ReadFpcr().GetRMode());

    // These instructions all use a one bit size field, except SQXTUN, SQXTN
    // and UQXTN, which use a two bit size field.
    switch (instr->Mask(NEONScalar2RegMiscFPMask)) {
      case NEON_FRECPE_scalar:
        frecpe(fpf, rd, rn, fpcr_rounding);
        break;
      case NEON_FRECPX_scalar:
        frecpx(fpf, rd, rn);
        break;
      case NEON_FRSQRTE_scalar:
        frsqrte(fpf, rd, rn);
        break;
      case NEON_FCMGT_zero_scalar:
        fcmp_zero(fpf, rd, rn, gt);
        break;
      case NEON_FCMGE_zero_scalar:
        fcmp_zero(fpf, rd, rn, ge);
        break;
      case NEON_FCMEQ_zero_scalar:
        fcmp_zero(fpf, rd, rn, eq);
        break;
      case NEON_FCMLE_zero_scalar:
        fcmp_zero(fpf, rd, rn, le);
        break;
      case NEON_FCMLT_zero_scalar:
        fcmp_zero(fpf, rd, rn, lt);
        break;
      case NEON_SCVTF_scalar:
        scvtf(fpf, rd, rn, 0, fpcr_rounding);
        break;
      case NEON_UCVTF_scalar:
        ucvtf(fpf, rd, rn, 0, fpcr_rounding);
        break;
      case NEON_FCVTNS_scalar:
        fcvts(fpf, rd, rn, FPTieEven);
        break;
      case NEON_FCVTNU_scalar:
        fcvtu(fpf, rd, rn, FPTieEven);
        break;
      case NEON_FCVTPS_scalar:
        fcvts(fpf, rd, rn, FPPositiveInfinity);
        break;
      case NEON_FCVTPU_scalar:
        fcvtu(fpf, rd, rn, FPPositiveInfinity);
        break;
      case NEON_FCVTMS_scalar:
        fcvts(fpf, rd, rn, FPNegativeInfinity);
        break;
      case NEON_FCVTMU_scalar:
        fcvtu(fpf, rd, rn, FPNegativeInfinity);
        break;
      case NEON_FCVTZS_scalar:
        fcvts(fpf, rd, rn, FPZero);
        break;
      case NEON_FCVTZU_scalar:
        fcvtu(fpf, rd, rn, FPZero);
        break;
      case NEON_FCVTAS_scalar:
        fcvts(fpf, rd, rn, FPTieAway);
        break;
      case NEON_FCVTAU_scalar:
        fcvtu(fpf, rd, rn, FPTieAway);
        break;
      case NEON_FCVTXN_scalar:
        // Unlike all of the other FP instructions above, fcvtxn encodes dest
        // size S as size<0>=1. There's only one case, so we ignore the form.
        VIXL_ASSERT(instr->ExtractBit(22) == 1);
        fcvtxn(kFormatS, rd, rn);
        break;
      default:
        switch (instr->Mask(NEONScalar2RegMiscMask)) {
          case NEON_SQXTN_scalar:
            sqxtn(vf, rd, rn);
            break;
          case NEON_UQXTN_scalar:
            uqxtn(vf, rd, rn);
            break;
          case NEON_SQXTUN_scalar:
            sqxtun(vf, rd, rn);
            break;
          default:
            VIXL_UNIMPLEMENTED();
        }
    }
  }
}


void Simulator::VisitNEONScalar2RegMiscFP16(const Instruction* instr) {
  VectorFormat fpf = kFormatH;
  FPRounding fpcr_rounding = static_cast<FPRounding>(ReadFpcr().GetRMode());

  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());

  switch (instr->Mask(NEONScalar2RegMiscFP16Mask)) {
    case NEON_FRECPE_H_scalar:
      frecpe(fpf, rd, rn, fpcr_rounding);
      break;
    case NEON_FRECPX_H_scalar:
      frecpx(fpf, rd, rn);
      break;
    case NEON_FRSQRTE_H_scalar:
      frsqrte(fpf, rd, rn);
      break;
    case NEON_FCMGT_H_zero_scalar:
      fcmp_zero(fpf, rd, rn, gt);
      break;
    case NEON_FCMGE_H_zero_scalar:
      fcmp_zero(fpf, rd, rn, ge);
      break;
    case NEON_FCMEQ_H_zero_scalar:
      fcmp_zero(fpf, rd, rn, eq);
      break;
    case NEON_FCMLE_H_zero_scalar:
      fcmp_zero(fpf, rd, rn, le);
      break;
    case NEON_FCMLT_H_zero_scalar:
      fcmp_zero(fpf, rd, rn, lt);
      break;
    case NEON_SCVTF_H_scalar:
      scvtf(fpf, rd, rn, 0, fpcr_rounding);
      break;
    case NEON_UCVTF_H_scalar:
      ucvtf(fpf, rd, rn, 0, fpcr_rounding);
      break;
    case NEON_FCVTNS_H_scalar:
      fcvts(fpf, rd, rn, FPTieEven);
      break;
    case NEON_FCVTNU_H_scalar:
      fcvtu(fpf, rd, rn, FPTieEven);
      break;
    case NEON_FCVTPS_H_scalar:
      fcvts(fpf, rd, rn, FPPositiveInfinity);
      break;
    case NEON_FCVTPU_H_scalar:
      fcvtu(fpf, rd, rn, FPPositiveInfinity);
      break;
    case NEON_FCVTMS_H_scalar:
      fcvts(fpf, rd, rn, FPNegativeInfinity);
      break;
    case NEON_FCVTMU_H_scalar:
      fcvtu(fpf, rd, rn, FPNegativeInfinity);
      break;
    case NEON_FCVTZS_H_scalar:
      fcvts(fpf, rd, rn, FPZero);
      break;
    case NEON_FCVTZU_H_scalar:
      fcvtu(fpf, rd, rn, FPZero);
      break;
    case NEON_FCVTAS_H_scalar:
      fcvts(fpf, rd, rn, FPTieAway);
      break;
    case NEON_FCVTAU_H_scalar:
      fcvtu(fpf, rd, rn, FPTieAway);
      break;
  }
}


void Simulator::VisitNEONScalar3Diff(const Instruction* instr) {
  NEONFormatDecoder nfd(instr, NEONFormatDecoder::LongScalarFormatMap());
  VectorFormat vf = nfd.GetVectorFormat();

  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());
  SimVRegister& rm = ReadVRegister(instr->GetRm());
  switch (instr->Mask(NEONScalar3DiffMask)) {
    case NEON_SQDMLAL_scalar:
      sqdmlal(vf, rd, rn, rm);
      break;
    case NEON_SQDMLSL_scalar:
      sqdmlsl(vf, rd, rn, rm);
      break;
    case NEON_SQDMULL_scalar:
      sqdmull(vf, rd, rn, rm);
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }
}


void Simulator::VisitNEONScalar3Same(const Instruction* instr) {
  NEONFormatDecoder nfd(instr, NEONFormatDecoder::ScalarFormatMap());
  VectorFormat vf = nfd.GetVectorFormat();

  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());
  SimVRegister& rm = ReadVRegister(instr->GetRm());

  if (instr->Mask(NEONScalar3SameFPFMask) == NEONScalar3SameFPFixed) {
    vf = nfd.GetVectorFormat(nfd.FPScalarFormatMap());
    switch (instr->Mask(NEONScalar3SameFPMask)) {
      case NEON_FMULX_scalar:
        fmulx(vf, rd, rn, rm);
        break;
      case NEON_FACGE_scalar:
        fabscmp(vf, rd, rn, rm, ge);
        break;
      case NEON_FACGT_scalar:
        fabscmp(vf, rd, rn, rm, gt);
        break;
      case NEON_FCMEQ_scalar:
        fcmp(vf, rd, rn, rm, eq);
        break;
      case NEON_FCMGE_scalar:
        fcmp(vf, rd, rn, rm, ge);
        break;
      case NEON_FCMGT_scalar:
        fcmp(vf, rd, rn, rm, gt);
        break;
      case NEON_FRECPS_scalar:
        frecps(vf, rd, rn, rm);
        break;
      case NEON_FRSQRTS_scalar:
        frsqrts(vf, rd, rn, rm);
        break;
      case NEON_FABD_scalar:
        fabd(vf, rd, rn, rm);
        break;
      default:
        VIXL_UNIMPLEMENTED();
    }
  } else {
    switch (instr->Mask(NEONScalar3SameMask)) {
      case NEON_ADD_scalar:
        add(vf, rd, rn, rm);
        break;
      case NEON_SUB_scalar:
        sub(vf, rd, rn, rm);
        break;
      case NEON_CMEQ_scalar:
        cmp(vf, rd, rn, rm, eq);
        break;
      case NEON_CMGE_scalar:
        cmp(vf, rd, rn, rm, ge);
        break;
      case NEON_CMGT_scalar:
        cmp(vf, rd, rn, rm, gt);
        break;
      case NEON_CMHI_scalar:
        cmp(vf, rd, rn, rm, hi);
        break;
      case NEON_CMHS_scalar:
        cmp(vf, rd, rn, rm, hs);
        break;
      case NEON_CMTST_scalar:
        cmptst(vf, rd, rn, rm);
        break;
      case NEON_USHL_scalar:
        ushl(vf, rd, rn, rm);
        break;
      case NEON_SSHL_scalar:
        sshl(vf, rd, rn, rm);
        break;
      case NEON_SQDMULH_scalar:
        sqdmulh(vf, rd, rn, rm);
        break;
      case NEON_SQRDMULH_scalar:
        sqrdmulh(vf, rd, rn, rm);
        break;
      case NEON_UQADD_scalar:
        add(vf, rd, rn, rm).UnsignedSaturate(vf);
        break;
      case NEON_SQADD_scalar:
        add(vf, rd, rn, rm).SignedSaturate(vf);
        break;
      case NEON_UQSUB_scalar:
        sub(vf, rd, rn, rm).UnsignedSaturate(vf);
        break;
      case NEON_SQSUB_scalar:
        sub(vf, rd, rn, rm).SignedSaturate(vf);
        break;
      case NEON_UQSHL_scalar:
        ushl(vf, rd, rn, rm).UnsignedSaturate(vf);
        break;
      case NEON_SQSHL_scalar:
        sshl(vf, rd, rn, rm).SignedSaturate(vf);
        break;
      case NEON_URSHL_scalar:
        ushl(vf, rd, rn, rm).Round(vf);
        break;
      case NEON_SRSHL_scalar:
        sshl(vf, rd, rn, rm).Round(vf);
        break;
      case NEON_UQRSHL_scalar:
        ushl(vf, rd, rn, rm).Round(vf).UnsignedSaturate(vf);
        break;
      case NEON_SQRSHL_scalar:
        sshl(vf, rd, rn, rm).Round(vf).SignedSaturate(vf);
        break;
      default:
        VIXL_UNIMPLEMENTED();
    }
  }
}

void Simulator::VisitNEONScalar3SameFP16(const Instruction* instr) {
  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());
  SimVRegister& rm = ReadVRegister(instr->GetRm());

  switch (instr->Mask(NEONScalar3SameFP16Mask)) {
    case NEON_FABD_H_scalar:
      fabd(kFormatH, rd, rn, rm);
      break;
    case NEON_FMULX_H_scalar:
      fmulx(kFormatH, rd, rn, rm);
      break;
    case NEON_FCMEQ_H_scalar:
      fcmp(kFormatH, rd, rn, rm, eq);
      break;
    case NEON_FCMGE_H_scalar:
      fcmp(kFormatH, rd, rn, rm, ge);
      break;
    case NEON_FCMGT_H_scalar:
      fcmp(kFormatH, rd, rn, rm, gt);
      break;
    case NEON_FACGE_H_scalar:
      fabscmp(kFormatH, rd, rn, rm, ge);
      break;
    case NEON_FACGT_H_scalar:
      fabscmp(kFormatH, rd, rn, rm, gt);
      break;
    case NEON_FRECPS_H_scalar:
      frecps(kFormatH, rd, rn, rm);
      break;
    case NEON_FRSQRTS_H_scalar:
      frsqrts(kFormatH, rd, rn, rm);
      break;
    default:
      VIXL_UNREACHABLE();
  }
}


void Simulator::VisitNEONScalar3SameExtra(const Instruction* instr) {
  NEONFormatDecoder nfd(instr, NEONFormatDecoder::ScalarFormatMap());
  VectorFormat vf = nfd.GetVectorFormat();

  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());
  SimVRegister& rm = ReadVRegister(instr->GetRm());

  switch (instr->Mask(NEONScalar3SameExtraMask)) {
    case NEON_SQRDMLAH_scalar:
      sqrdmlah(vf, rd, rn, rm);
      break;
    case NEON_SQRDMLSH_scalar:
      sqrdmlsh(vf, rd, rn, rm);
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }
}

void Simulator::VisitNEONScalarByIndexedElement(const Instruction* instr) {
  NEONFormatDecoder nfd(instr, NEONFormatDecoder::LongScalarFormatMap());
  VectorFormat vf = nfd.GetVectorFormat();
  VectorFormat vf_r = nfd.GetVectorFormat(nfd.ScalarFormatMap());

  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());
  ByElementOp Op = NULL;

  int rm_reg = instr->GetRm();
  int index = (instr->GetNEONH() << 1) | instr->GetNEONL();
  if (instr->GetNEONSize() == 1) {
    rm_reg &= 0xf;
    index = (index << 1) | instr->GetNEONM();
  }

  switch (instr->Mask(NEONScalarByIndexedElementMask)) {
    case NEON_SQDMULL_byelement_scalar:
      Op = &Simulator::sqdmull;
      break;
    case NEON_SQDMLAL_byelement_scalar:
      Op = &Simulator::sqdmlal;
      break;
    case NEON_SQDMLSL_byelement_scalar:
      Op = &Simulator::sqdmlsl;
      break;
    case NEON_SQDMULH_byelement_scalar:
      Op = &Simulator::sqdmulh;
      vf = vf_r;
      break;
    case NEON_SQRDMULH_byelement_scalar:
      Op = &Simulator::sqrdmulh;
      vf = vf_r;
      break;
    case NEON_SQRDMLAH_byelement_scalar:
      Op = &Simulator::sqrdmlah;
      vf = vf_r;
      break;
    case NEON_SQRDMLSH_byelement_scalar:
      Op = &Simulator::sqrdmlsh;
      vf = vf_r;
      break;
    default:
      vf = nfd.GetVectorFormat(nfd.FPScalarFormatMap());
      index = instr->GetNEONH();
      if (instr->GetFPType() == 0) {
        index = (index << 2) | (instr->GetNEONL() << 1) | instr->GetNEONM();
        rm_reg &= 0xf;
        vf = kFormatH;
      } else if ((instr->GetFPType() & 1) == 0) {
        index = (index << 1) | instr->GetNEONL();
      }
      switch (instr->Mask(NEONScalarByIndexedElementFPMask)) {
        case NEON_FMUL_H_byelement_scalar:
        case NEON_FMUL_byelement_scalar:
          Op = &Simulator::fmul;
          break;
        case NEON_FMLA_H_byelement_scalar:
        case NEON_FMLA_byelement_scalar:
          Op = &Simulator::fmla;
          break;
        case NEON_FMLS_H_byelement_scalar:
        case NEON_FMLS_byelement_scalar:
          Op = &Simulator::fmls;
          break;
        case NEON_FMULX_H_byelement_scalar:
        case NEON_FMULX_byelement_scalar:
          Op = &Simulator::fmulx;
          break;
        default:
          VIXL_UNIMPLEMENTED();
      }
  }

  (this->*Op)(vf, rd, rn, ReadVRegister(rm_reg), index);
}


void Simulator::VisitNEONScalarCopy(const Instruction* instr) {
  NEONFormatDecoder nfd(instr, NEONFormatDecoder::TriangularScalarFormatMap());
  VectorFormat vf = nfd.GetVectorFormat();

  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());

  if (instr->Mask(NEONScalarCopyMask) == NEON_DUP_ELEMENT_scalar) {
    int imm5 = instr->GetImmNEON5();
    int tz = CountTrailingZeros(imm5, 32);
    int rn_index = imm5 >> (tz + 1);
    dup_element(vf, rd, rn, rn_index);
  } else {
    VIXL_UNIMPLEMENTED();
  }
}


void Simulator::VisitNEONScalarPairwise(const Instruction* instr) {
  NEONFormatDecoder nfd(instr, NEONFormatDecoder::FPScalarPairwiseFormatMap());
  VectorFormat vf = nfd.GetVectorFormat();

  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());
  switch (instr->Mask(NEONScalarPairwiseMask)) {
    case NEON_ADDP_scalar: {
      // All pairwise operations except ADDP use bit U to differentiate FP16
      // from FP32/FP64 variations.
      NEONFormatDecoder nfd_addp(instr, NEONFormatDecoder::FPScalarFormatMap());
      addp(nfd_addp.GetVectorFormat(), rd, rn);
      break;
    }
    case NEON_FADDP_h_scalar:
    case NEON_FADDP_scalar:
      faddp(vf, rd, rn);
      break;
    case NEON_FMAXP_h_scalar:
    case NEON_FMAXP_scalar:
      fmaxp(vf, rd, rn);
      break;
    case NEON_FMAXNMP_h_scalar:
    case NEON_FMAXNMP_scalar:
      fmaxnmp(vf, rd, rn);
      break;
    case NEON_FMINP_h_scalar:
    case NEON_FMINP_scalar:
      fminp(vf, rd, rn);
      break;
    case NEON_FMINNMP_h_scalar:
    case NEON_FMINNMP_scalar:
      fminnmp(vf, rd, rn);
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }
}


void Simulator::VisitNEONScalarShiftImmediate(const Instruction* instr) {
  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());
  FPRounding fpcr_rounding = static_cast<FPRounding>(ReadFpcr().GetRMode());

  static const NEONFormatMap map = {{22, 21, 20, 19},
                                    {NF_UNDEF,
                                     NF_B,
                                     NF_H,
                                     NF_H,
                                     NF_S,
                                     NF_S,
                                     NF_S,
                                     NF_S,
                                     NF_D,
                                     NF_D,
                                     NF_D,
                                     NF_D,
                                     NF_D,
                                     NF_D,
                                     NF_D,
                                     NF_D}};
  NEONFormatDecoder nfd(instr, &map);
  VectorFormat vf = nfd.GetVectorFormat();

  int highestSetBit = HighestSetBitPosition(instr->GetImmNEONImmh());
  int immhimmb = instr->GetImmNEONImmhImmb();
  int right_shift = (16 << highestSetBit) - immhimmb;
  int left_shift = immhimmb - (8 << highestSetBit);
  switch (instr->Mask(NEONScalarShiftImmediateMask)) {
    case NEON_SHL_scalar:
      shl(vf, rd, rn, left_shift);
      break;
    case NEON_SLI_scalar:
      sli(vf, rd, rn, left_shift);
      break;
    case NEON_SQSHL_imm_scalar:
      sqshl(vf, rd, rn, left_shift);
      break;
    case NEON_UQSHL_imm_scalar:
      uqshl(vf, rd, rn, left_shift);
      break;
    case NEON_SQSHLU_scalar:
      sqshlu(vf, rd, rn, left_shift);
      break;
    case NEON_SRI_scalar:
      sri(vf, rd, rn, right_shift);
      break;
    case NEON_SSHR_scalar:
      sshr(vf, rd, rn, right_shift);
      break;
    case NEON_USHR_scalar:
      ushr(vf, rd, rn, right_shift);
      break;
    case NEON_SRSHR_scalar:
      sshr(vf, rd, rn, right_shift).Round(vf);
      break;
    case NEON_URSHR_scalar:
      ushr(vf, rd, rn, right_shift).Round(vf);
      break;
    case NEON_SSRA_scalar:
      ssra(vf, rd, rn, right_shift);
      break;
    case NEON_USRA_scalar:
      usra(vf, rd, rn, right_shift);
      break;
    case NEON_SRSRA_scalar:
      srsra(vf, rd, rn, right_shift);
      break;
    case NEON_URSRA_scalar:
      ursra(vf, rd, rn, right_shift);
      break;
    case NEON_UQSHRN_scalar:
      uqshrn(vf, rd, rn, right_shift);
      break;
    case NEON_UQRSHRN_scalar:
      uqrshrn(vf, rd, rn, right_shift);
      break;
    case NEON_SQSHRN_scalar:
      sqshrn(vf, rd, rn, right_shift);
      break;
    case NEON_SQRSHRN_scalar:
      sqrshrn(vf, rd, rn, right_shift);
      break;
    case NEON_SQSHRUN_scalar:
      sqshrun(vf, rd, rn, right_shift);
      break;
    case NEON_SQRSHRUN_scalar:
      sqrshrun(vf, rd, rn, right_shift);
      break;
    case NEON_FCVTZS_imm_scalar:
      fcvts(vf, rd, rn, FPZero, right_shift);
      break;
    case NEON_FCVTZU_imm_scalar:
      fcvtu(vf, rd, rn, FPZero, right_shift);
      break;
    case NEON_SCVTF_imm_scalar:
      scvtf(vf, rd, rn, right_shift, fpcr_rounding);
      break;
    case NEON_UCVTF_imm_scalar:
      ucvtf(vf, rd, rn, right_shift, fpcr_rounding);
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }
}


void Simulator::VisitNEONShiftImmediate(const Instruction* instr) {
  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());
  FPRounding fpcr_rounding = static_cast<FPRounding>(ReadFpcr().GetRMode());

  // 00010->8B, 00011->16B, 001x0->4H, 001x1->8H,
  // 01xx0->2S, 01xx1->4S, 1xxx1->2D, all others undefined.
  static const NEONFormatMap map = {{22, 21, 20, 19, 30},
                                    {NF_UNDEF, NF_UNDEF, NF_8B,    NF_16B,
                                     NF_4H,    NF_8H,    NF_4H,    NF_8H,
                                     NF_2S,    NF_4S,    NF_2S,    NF_4S,
                                     NF_2S,    NF_4S,    NF_2S,    NF_4S,
                                     NF_UNDEF, NF_2D,    NF_UNDEF, NF_2D,
                                     NF_UNDEF, NF_2D,    NF_UNDEF, NF_2D,
                                     NF_UNDEF, NF_2D,    NF_UNDEF, NF_2D,
                                     NF_UNDEF, NF_2D,    NF_UNDEF, NF_2D}};
  NEONFormatDecoder nfd(instr, &map);
  VectorFormat vf = nfd.GetVectorFormat();

  // 0001->8H, 001x->4S, 01xx->2D, all others undefined.
  static const NEONFormatMap map_l =
      {{22, 21, 20, 19},
       {NF_UNDEF, NF_8H, NF_4S, NF_4S, NF_2D, NF_2D, NF_2D, NF_2D}};
  VectorFormat vf_l = nfd.GetVectorFormat(&map_l);

  int highestSetBit = HighestSetBitPosition(instr->GetImmNEONImmh());
  int immhimmb = instr->GetImmNEONImmhImmb();
  int right_shift = (16 << highestSetBit) - immhimmb;
  int left_shift = immhimmb - (8 << highestSetBit);

  switch (instr->Mask(NEONShiftImmediateMask)) {
    case NEON_SHL:
      shl(vf, rd, rn, left_shift);
      break;
    case NEON_SLI:
      sli(vf, rd, rn, left_shift);
      break;
    case NEON_SQSHLU:
      sqshlu(vf, rd, rn, left_shift);
      break;
    case NEON_SRI:
      sri(vf, rd, rn, right_shift);
      break;
    case NEON_SSHR:
      sshr(vf, rd, rn, right_shift);
      break;
    case NEON_USHR:
      ushr(vf, rd, rn, right_shift);
      break;
    case NEON_SRSHR:
      sshr(vf, rd, rn, right_shift).Round(vf);
      break;
    case NEON_URSHR:
      ushr(vf, rd, rn, right_shift).Round(vf);
      break;
    case NEON_SSRA:
      ssra(vf, rd, rn, right_shift);
      break;
    case NEON_USRA:
      usra(vf, rd, rn, right_shift);
      break;
    case NEON_SRSRA:
      srsra(vf, rd, rn, right_shift);
      break;
    case NEON_URSRA:
      ursra(vf, rd, rn, right_shift);
      break;
    case NEON_SQSHL_imm:
      sqshl(vf, rd, rn, left_shift);
      break;
    case NEON_UQSHL_imm:
      uqshl(vf, rd, rn, left_shift);
      break;
    case NEON_SCVTF_imm:
      scvtf(vf, rd, rn, right_shift, fpcr_rounding);
      break;
    case NEON_UCVTF_imm:
      ucvtf(vf, rd, rn, right_shift, fpcr_rounding);
      break;
    case NEON_FCVTZS_imm:
      fcvts(vf, rd, rn, FPZero, right_shift);
      break;
    case NEON_FCVTZU_imm:
      fcvtu(vf, rd, rn, FPZero, right_shift);
      break;
    case NEON_SSHLL:
      vf = vf_l;
      if (instr->Mask(NEON_Q)) {
        sshll2(vf, rd, rn, left_shift);
      } else {
        sshll(vf, rd, rn, left_shift);
      }
      break;
    case NEON_USHLL:
      vf = vf_l;
      if (instr->Mask(NEON_Q)) {
        ushll2(vf, rd, rn, left_shift);
      } else {
        ushll(vf, rd, rn, left_shift);
      }
      break;
    case NEON_SHRN:
      if (instr->Mask(NEON_Q)) {
        shrn2(vf, rd, rn, right_shift);
      } else {
        shrn(vf, rd, rn, right_shift);
      }
      break;
    case NEON_RSHRN:
      if (instr->Mask(NEON_Q)) {
        rshrn2(vf, rd, rn, right_shift);
      } else {
        rshrn(vf, rd, rn, right_shift);
      }
      break;
    case NEON_UQSHRN:
      if (instr->Mask(NEON_Q)) {
        uqshrn2(vf, rd, rn, right_shift);
      } else {
        uqshrn(vf, rd, rn, right_shift);
      }
      break;
    case NEON_UQRSHRN:
      if (instr->Mask(NEON_Q)) {
        uqrshrn2(vf, rd, rn, right_shift);
      } else {
        uqrshrn(vf, rd, rn, right_shift);
      }
      break;
    case NEON_SQSHRN:
      if (instr->Mask(NEON_Q)) {
        sqshrn2(vf, rd, rn, right_shift);
      } else {
        sqshrn(vf, rd, rn, right_shift);
      }
      break;
    case NEON_SQRSHRN:
      if (instr->Mask(NEON_Q)) {
        sqrshrn2(vf, rd, rn, right_shift);
      } else {
        sqrshrn(vf, rd, rn, right_shift);
      }
      break;
    case NEON_SQSHRUN:
      if (instr->Mask(NEON_Q)) {
        sqshrun2(vf, rd, rn, right_shift);
      } else {
        sqshrun(vf, rd, rn, right_shift);
      }
      break;
    case NEON_SQRSHRUN:
      if (instr->Mask(NEON_Q)) {
        sqrshrun2(vf, rd, rn, right_shift);
      } else {
        sqrshrun(vf, rd, rn, right_shift);
      }
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }
}


void Simulator::VisitNEONTable(const Instruction* instr) {
  NEONFormatDecoder nfd(instr, NEONFormatDecoder::LogicalFormatMap());
  VectorFormat vf = nfd.GetVectorFormat();

  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());
  SimVRegister& rn2 = ReadVRegister((instr->GetRn() + 1) % kNumberOfVRegisters);
  SimVRegister& rn3 = ReadVRegister((instr->GetRn() + 2) % kNumberOfVRegisters);
  SimVRegister& rn4 = ReadVRegister((instr->GetRn() + 3) % kNumberOfVRegisters);
  SimVRegister& rm = ReadVRegister(instr->GetRm());

  switch (instr->Mask(NEONTableMask)) {
    case NEON_TBL_1v:
      tbl(vf, rd, rn, rm);
      break;
    case NEON_TBL_2v:
      tbl(vf, rd, rn, rn2, rm);
      break;
    case NEON_TBL_3v:
      tbl(vf, rd, rn, rn2, rn3, rm);
      break;
    case NEON_TBL_4v:
      tbl(vf, rd, rn, rn2, rn3, rn4, rm);
      break;
    case NEON_TBX_1v:
      tbx(vf, rd, rn, rm);
      break;
    case NEON_TBX_2v:
      tbx(vf, rd, rn, rn2, rm);
      break;
    case NEON_TBX_3v:
      tbx(vf, rd, rn, rn2, rn3, rm);
      break;
    case NEON_TBX_4v:
      tbx(vf, rd, rn, rn2, rn3, rn4, rm);
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }
}


void Simulator::VisitNEONPerm(const Instruction* instr) {
  NEONFormatDecoder nfd(instr);
  VectorFormat vf = nfd.GetVectorFormat();

  SimVRegister& rd = ReadVRegister(instr->GetRd());
  SimVRegister& rn = ReadVRegister(instr->GetRn());
  SimVRegister& rm = ReadVRegister(instr->GetRm());

  switch (instr->Mask(NEONPermMask)) {
    case NEON_TRN1:
      trn1(vf, rd, rn, rm);
      break;
    case NEON_TRN2:
      trn2(vf, rd, rn, rm);
      break;
    case NEON_UZP1:
      uzp1(vf, rd, rn, rm);
      break;
    case NEON_UZP2:
      uzp2(vf, rd, rn, rm);
      break;
    case NEON_ZIP1:
      zip1(vf, rd, rn, rm);
      break;
    case NEON_ZIP2:
      zip2(vf, rd, rn, rm);
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }
}

void Simulator::VisitSVEAddressGeneration(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEAddressGenerationMask)) {
    case ADR_z_az_d_s32_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case ADR_z_az_d_u32_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case ADR_z_az_sd_same_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEBitwiseImm(const Instruction* instr) {
  USE(instr);
  Instr op = instr->Mask(SVEBitwiseImmMask);
  switch (op) {
    case AND_z_zi:
    case DUPM_z_i:
    case EOR_z_zi:
    case ORR_z_zi: {
      int lane_size = instr->GetSVEBitwiseImmLaneSizeInBytesLog2();
      uint64_t imm = instr->GetSVEImmLogical();
      // Valid immediate is a non-zero bits
      VIXL_ASSERT(imm != 0);
      SVEBitwiseImmHelper(static_cast<SVEBitwiseImmOp>(op),
                          SVEFormatFromLaneSizeInBytesLog2(lane_size),
                          ReadVRegister(instr->GetRd()),
                          imm);
      break;
    }
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEBitwiseLogicalUnpredicated(const Instruction* instr) {
  USE(instr);
  SimVRegister& zd = ReadVRegister(instr->GetRd());
  SimVRegister& zn = ReadVRegister(instr->GetRn());
  SimVRegister& zm = ReadVRegister(instr->GetRm());
  Instr op = instr->Mask(SVEBitwiseLogicalUnpredicatedMask);

  LogicalOp logical_op;
  switch (op) {
    case AND_z_zz:
      logical_op = AND;
      break;
    case BIC_z_zz:
      logical_op = BIC;
      break;
    case EOR_z_zz:
      logical_op = EOR;
      break;
    case ORR_z_zz:
      logical_op = ORR;
      break;
    default:
      logical_op = LogicalOpMask;
      VIXL_UNIMPLEMENTED();
      break;
  }
  // Lane size of registers is irrelevant to the bitwise operations, so perform
  // the operation on D-sized lanes.
  SVEBitwiseLogicalUnpredicatedHelper(logical_op, kFormatVnD, zd, zn, zm);
}

void Simulator::VisitSVEBitwiseShiftPredicated(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEBitwiseShiftPredicatedMask)) {
    case ASRD_z_p_zi:
      VIXL_UNIMPLEMENTED();
      break;
    case ASRR_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case ASR_z_p_zi:
      VIXL_UNIMPLEMENTED();
      break;
    case ASR_z_p_zw:
      VIXL_UNIMPLEMENTED();
      break;
    case ASR_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case LSLR_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case LSL_z_p_zi:
      VIXL_UNIMPLEMENTED();
      break;
    case LSL_z_p_zw:
      VIXL_UNIMPLEMENTED();
      break;
    case LSL_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case LSRR_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case LSR_z_p_zi:
      VIXL_UNIMPLEMENTED();
      break;
    case LSR_z_p_zw:
      VIXL_UNIMPLEMENTED();
      break;
    case LSR_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEBitwiseShiftUnpredicated(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEBitwiseShiftUnpredicatedMask)) {
    case ASR_z_zi:
      VIXL_UNIMPLEMENTED();
      break;
    case ASR_z_zw:
      VIXL_UNIMPLEMENTED();
      break;
    case LSL_z_zi:
      VIXL_UNIMPLEMENTED();
      break;
    case LSL_z_zw:
      VIXL_UNIMPLEMENTED();
      break;
    case LSR_z_zi:
      VIXL_UNIMPLEMENTED();
      break;
    case LSR_z_zw:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEElementCount(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEElementCountMask)) {
    case CNTB_r_s:
      VIXL_UNIMPLEMENTED();
      break;
    case CNTD_r_s:
      VIXL_UNIMPLEMENTED();
      break;
    case CNTH_r_s:
      VIXL_UNIMPLEMENTED();
      break;
    case CNTW_r_s:
      VIXL_UNIMPLEMENTED();
      break;
    case DECB_r_rs:
      VIXL_UNIMPLEMENTED();
      break;
    case DECD_r_rs:
      VIXL_UNIMPLEMENTED();
      break;
    case DECD_z_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case DECH_r_rs:
      VIXL_UNIMPLEMENTED();
      break;
    case DECH_z_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case DECW_r_rs:
      VIXL_UNIMPLEMENTED();
      break;
    case DECW_z_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case INCB_r_rs:
      VIXL_UNIMPLEMENTED();
      break;
    case INCD_r_rs:
      VIXL_UNIMPLEMENTED();
      break;
    case INCD_z_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case INCH_r_rs:
      VIXL_UNIMPLEMENTED();
      break;
    case INCH_z_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case INCW_r_rs:
      VIXL_UNIMPLEMENTED();
      break;
    case INCW_z_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case SQDECB_r_rs_sx:
      VIXL_UNIMPLEMENTED();
      break;
    case SQDECB_r_rs_x:
      VIXL_UNIMPLEMENTED();
      break;
    case SQDECD_r_rs_sx:
      VIXL_UNIMPLEMENTED();
      break;
    case SQDECD_r_rs_x:
      VIXL_UNIMPLEMENTED();
      break;
    case SQDECD_z_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case SQDECH_r_rs_sx:
      VIXL_UNIMPLEMENTED();
      break;
    case SQDECH_r_rs_x:
      VIXL_UNIMPLEMENTED();
      break;
    case SQDECH_z_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case SQDECW_r_rs_sx:
      VIXL_UNIMPLEMENTED();
      break;
    case SQDECW_r_rs_x:
      VIXL_UNIMPLEMENTED();
      break;
    case SQDECW_z_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case SQINCB_r_rs_sx:
      VIXL_UNIMPLEMENTED();
      break;
    case SQINCB_r_rs_x:
      VIXL_UNIMPLEMENTED();
      break;
    case SQINCD_r_rs_sx:
      VIXL_UNIMPLEMENTED();
      break;
    case SQINCD_r_rs_x:
      VIXL_UNIMPLEMENTED();
      break;
    case SQINCD_z_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case SQINCH_r_rs_sx:
      VIXL_UNIMPLEMENTED();
      break;
    case SQINCH_r_rs_x:
      VIXL_UNIMPLEMENTED();
      break;
    case SQINCH_z_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case SQINCW_r_rs_sx:
      VIXL_UNIMPLEMENTED();
      break;
    case SQINCW_r_rs_x:
      VIXL_UNIMPLEMENTED();
      break;
    case SQINCW_z_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case UQDECB_r_rs_uw:
      VIXL_UNIMPLEMENTED();
      break;
    case UQDECB_r_rs_x:
      VIXL_UNIMPLEMENTED();
      break;
    case UQDECD_r_rs_uw:
      VIXL_UNIMPLEMENTED();
      break;
    case UQDECD_r_rs_x:
      VIXL_UNIMPLEMENTED();
      break;
    case UQDECD_z_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case UQDECH_r_rs_uw:
      VIXL_UNIMPLEMENTED();
      break;
    case UQDECH_r_rs_x:
      VIXL_UNIMPLEMENTED();
      break;
    case UQDECH_z_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case UQDECW_r_rs_uw:
      VIXL_UNIMPLEMENTED();
      break;
    case UQDECW_r_rs_x:
      VIXL_UNIMPLEMENTED();
      break;
    case UQDECW_z_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case UQINCB_r_rs_uw:
      VIXL_UNIMPLEMENTED();
      break;
    case UQINCB_r_rs_x:
      VIXL_UNIMPLEMENTED();
      break;
    case UQINCD_r_rs_uw:
      VIXL_UNIMPLEMENTED();
      break;
    case UQINCD_r_rs_x:
      VIXL_UNIMPLEMENTED();
      break;
    case UQINCD_z_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case UQINCH_r_rs_uw:
      VIXL_UNIMPLEMENTED();
      break;
    case UQINCH_r_rs_x:
      VIXL_UNIMPLEMENTED();
      break;
    case UQINCH_z_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case UQINCW_r_rs_uw:
      VIXL_UNIMPLEMENTED();
      break;
    case UQINCW_r_rs_x:
      VIXL_UNIMPLEMENTED();
      break;
    case UQINCW_z_zs:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEFPAccumulatingReduction(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEFPAccumulatingReductionMask)) {
    case FADDA_v_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEFPArithmeticPredicated(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEFPArithmeticPredicatedMask)) {
    case FABD_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FADD_z_p_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case FADD_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FDIVR_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FDIV_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FMAXNM_z_p_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case FMAXNM_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FMAX_z_p_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case FMAX_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FMINNM_z_p_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case FMINNM_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FMIN_z_p_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case FMIN_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FMULX_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FMUL_z_p_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case FMUL_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FSCALE_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FSUBR_z_p_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case FSUBR_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FSUB_z_p_zs:
      VIXL_UNIMPLEMENTED();
      break;
    case FSUB_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FTMAD_z_zzi:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEFPArithmeticUnpredicated(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEFPArithmeticUnpredicatedMask)) {
    case FADD_z_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FMUL_z_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FRECPS_z_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FRSQRTS_z_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FSUB_z_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FTSMUL_z_zz:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEFPCompareVectors(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEFPCompareVectorsMask)) {
    case FACGE_p_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FACGT_p_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FCMEQ_p_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FCMGE_p_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FCMGT_p_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FCMNE_p_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case FCMUO_p_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEFPCompareWithZero(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEFPCompareWithZeroMask)) {
    case FCMEQ_p_p_z0:
      VIXL_UNIMPLEMENTED();
      break;
    case FCMGE_p_p_z0:
      VIXL_UNIMPLEMENTED();
      break;
    case FCMGT_p_p_z0:
      VIXL_UNIMPLEMENTED();
      break;
    case FCMLE_p_p_z0:
      VIXL_UNIMPLEMENTED();
      break;
    case FCMLT_p_p_z0:
      VIXL_UNIMPLEMENTED();
      break;
    case FCMNE_p_p_z0:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEFPComplexAddition(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEFPComplexAdditionMask)) {
    case FCADD_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEFPComplexMulAdd(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEFPComplexMulAddMask)) {
    case FCMLA_z_p_zzz:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEFPComplexMulAddIndex(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEFPComplexMulAddIndexMask)) {
    case FCMLA_z_zzzi_h:
      VIXL_UNIMPLEMENTED();
      break;
    case FCMLA_z_zzzi_s:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEFPFastReduction(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEFPFastReductionMask)) {
    case FADDV_v_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case FMAXNMV_v_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case FMAXV_v_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case FMINNMV_v_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case FMINV_v_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEFPMulIndex(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEFPMulIndexMask)) {
    case FMUL_z_zzi_d:
      VIXL_UNIMPLEMENTED();
      break;
    case FMUL_z_zzi_h:
      VIXL_UNIMPLEMENTED();
      break;
    case FMUL_z_zzi_s:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEFPMulAdd(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEFPMulAddMask)) {
    case FMAD_z_p_zzz:
      VIXL_UNIMPLEMENTED();
      break;
    case FMLA_z_p_zzz:
      VIXL_UNIMPLEMENTED();
      break;
    case FMLS_z_p_zzz:
      VIXL_UNIMPLEMENTED();
      break;
    case FMSB_z_p_zzz:
      VIXL_UNIMPLEMENTED();
      break;
    case FNMAD_z_p_zzz:
      VIXL_UNIMPLEMENTED();
      break;
    case FNMLA_z_p_zzz:
      VIXL_UNIMPLEMENTED();
      break;
    case FNMLS_z_p_zzz:
      VIXL_UNIMPLEMENTED();
      break;
    case FNMSB_z_p_zzz:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEFPMulAddIndex(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEFPMulAddIndexMask)) {
    case FMLA_z_zzzi_d:
      VIXL_UNIMPLEMENTED();
      break;
    case FMLA_z_zzzi_h:
      VIXL_UNIMPLEMENTED();
      break;
    case FMLA_z_zzzi_s:
      VIXL_UNIMPLEMENTED();
      break;
    case FMLS_z_zzzi_d:
      VIXL_UNIMPLEMENTED();
      break;
    case FMLS_z_zzzi_h:
      VIXL_UNIMPLEMENTED();
      break;
    case FMLS_z_zzzi_s:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEFPUnaryOpPredicated(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEFPUnaryOpPredicatedMask)) {
    case FCVTZS_z_p_z_d2w:
      VIXL_UNIMPLEMENTED();
      break;
    case FCVTZS_z_p_z_d2x:
      VIXL_UNIMPLEMENTED();
      break;
    case FCVTZS_z_p_z_fp162h:
      VIXL_UNIMPLEMENTED();
      break;
    case FCVTZS_z_p_z_fp162w:
      VIXL_UNIMPLEMENTED();
      break;
    case FCVTZS_z_p_z_fp162x:
      VIXL_UNIMPLEMENTED();
      break;
    case FCVTZS_z_p_z_s2w:
      VIXL_UNIMPLEMENTED();
      break;
    case FCVTZS_z_p_z_s2x:
      VIXL_UNIMPLEMENTED();
      break;
    case FCVTZU_z_p_z_d2w:
      VIXL_UNIMPLEMENTED();
      break;
    case FCVTZU_z_p_z_d2x:
      VIXL_UNIMPLEMENTED();
      break;
    case FCVTZU_z_p_z_fp162h:
      VIXL_UNIMPLEMENTED();
      break;
    case FCVTZU_z_p_z_fp162w:
      VIXL_UNIMPLEMENTED();
      break;
    case FCVTZU_z_p_z_fp162x:
      VIXL_UNIMPLEMENTED();
      break;
    case FCVTZU_z_p_z_s2w:
      VIXL_UNIMPLEMENTED();
      break;
    case FCVTZU_z_p_z_s2x:
      VIXL_UNIMPLEMENTED();
      break;
    case FCVT_z_p_z_d2h:
      VIXL_UNIMPLEMENTED();
      break;
    case FCVT_z_p_z_d2s:
      VIXL_UNIMPLEMENTED();
      break;
    case FCVT_z_p_z_h2d:
      VIXL_UNIMPLEMENTED();
      break;
    case FCVT_z_p_z_h2s:
      VIXL_UNIMPLEMENTED();
      break;
    case FCVT_z_p_z_s2d:
      VIXL_UNIMPLEMENTED();
      break;
    case FCVT_z_p_z_s2h:
      VIXL_UNIMPLEMENTED();
      break;
    case FRECPX_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case FRINTA_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case FRINTI_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case FRINTM_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case FRINTN_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case FRINTP_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case FRINTX_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case FRINTZ_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case FSQRT_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case SCVTF_z_p_z_h2fp16:
      VIXL_UNIMPLEMENTED();
      break;
    case SCVTF_z_p_z_w2d:
      VIXL_UNIMPLEMENTED();
      break;
    case SCVTF_z_p_z_w2fp16:
      VIXL_UNIMPLEMENTED();
      break;
    case SCVTF_z_p_z_w2s:
      VIXL_UNIMPLEMENTED();
      break;
    case SCVTF_z_p_z_x2d:
      VIXL_UNIMPLEMENTED();
      break;
    case SCVTF_z_p_z_x2fp16:
      VIXL_UNIMPLEMENTED();
      break;
    case SCVTF_z_p_z_x2s:
      VIXL_UNIMPLEMENTED();
      break;
    case UCVTF_z_p_z_h2fp16:
      VIXL_UNIMPLEMENTED();
      break;
    case UCVTF_z_p_z_w2d:
      VIXL_UNIMPLEMENTED();
      break;
    case UCVTF_z_p_z_w2fp16:
      VIXL_UNIMPLEMENTED();
      break;
    case UCVTF_z_p_z_w2s:
      VIXL_UNIMPLEMENTED();
      break;
    case UCVTF_z_p_z_x2d:
      VIXL_UNIMPLEMENTED();
      break;
    case UCVTF_z_p_z_x2fp16:
      VIXL_UNIMPLEMENTED();
      break;
    case UCVTF_z_p_z_x2s:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEFPUnaryOpUnpredicated(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEFPUnaryOpUnpredicatedMask)) {
    case FRECPE_z_z:
      VIXL_UNIMPLEMENTED();
      break;
    case FRSQRTE_z_z:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEIncDecByPredicateCount(const Instruction* instr) {
  USE(instr);

  VectorFormat vform = instr->GetSVEVectorFormat();
  SimPRegister& pg = ReadPRegister(instr->ExtractBits(8, 5));

  int count = CountActiveLanes(vform, pg);

  if (instr->ExtractBit(11) == 0) {
    SimVRegister& zdn = ReadVRegister(instr->GetRd());
    switch (instr->Mask(SVEIncDecByPredicateCountMask)) {
      case DECP_z_p_z:
        sub(vform, zdn, zdn, count);
        break;
      case INCP_z_p_z:
        add(vform, zdn, zdn, count);
        break;
      case SQDECP_z_p_z:
        sub(vform, zdn, zdn, count).SignedSaturate(vform);
        break;
      case SQINCP_z_p_z:
        add(vform, zdn, zdn, count).SignedSaturate(vform);
        break;
      case UQDECP_z_p_z:
        sub(vform, zdn, zdn, count).UnsignedSaturate(vform);
        break;
      case UQINCP_z_p_z:
        add(vform, zdn, zdn, count).UnsignedSaturate(vform);
        break;
      default:
        VIXL_UNIMPLEMENTED();
        break;
    }
  } else {
    bool is_saturating = (instr->ExtractBit(18) == 0);
    bool decrement =
        is_saturating ? instr->ExtractBit(17) : instr->ExtractBit(16);
    bool is_signed = (instr->ExtractBit(16) == 0);
    bool sf = is_saturating ? (instr->ExtractBit(10) != 0) : true;
    unsigned width = sf ? kXRegSize : kWRegSize;

    switch (instr->Mask(SVEIncDecByPredicateCountMask)) {
      case DECP_r_p_r:
      case INCP_r_p_r:
      case SQDECP_r_p_r_sx:
      case SQDECP_r_p_r_x:
      case SQINCP_r_p_r_sx:
      case SQINCP_r_p_r_x:
      case UQDECP_r_p_r_uw:
      case UQDECP_r_p_r_x:
      case UQINCP_r_p_r_uw:
      case UQINCP_r_p_r_x:
        WriteXRegister(instr->GetRd(),
                       IncDecN(ReadXRegister(instr->GetRd()),
                               decrement ? -count : count,
                               width,
                               is_saturating,
                               is_signed));
        break;
      default:
        VIXL_UNIMPLEMENTED();
        break;
    }
  }
}

uint64_t Simulator::IncDecN(uint64_t acc,
                            int64_t delta,
                            unsigned n,
                            bool is_saturating,
                            bool is_signed) {
  VIXL_ASSERT(n <= 64);
  VIXL_ASSERT(IsIntN(n, delta));

  uint64_t sign_mask = UINT64_C(1) << (n - 1);
  uint64_t mask = GetUintMask(n);

  acc &= mask;  // Ignore initial accumulator high bits.
  uint64_t result = (acc + delta) & mask;

  bool acc_negative = ((acc & sign_mask) != 0);
  bool delta_negative = delta < 0;
  bool result_negative = ((result & sign_mask) != 0);

  if (is_saturating) {
    if (is_signed) {
      // If the signs of the operands are the same, but different from the
      // result, there was an overflow.
      if ((acc_negative == delta_negative) &&
          (acc_negative != result_negative)) {
        if (result_negative) {
          // Saturate to [..., INT<n>_MAX].
          result_negative = false;
          result = mask & ~sign_mask;  // E.g. 0x000000007fffffff
        } else {
          // Saturate to [INT<n>_MIN, ...].
          result_negative = true;
          result = ~mask | sign_mask;  // E.g. 0xffffffff80000000
        }
      }
    } else {
      if ((delta < 0) && (result > acc)) {
        // Saturate to [0, ...].
        result = 0;
      } else if ((delta > 0) && (result < acc)) {
        // Saturate to [..., UINT<n>_MAX].
        result = mask;
      }
    }
  }

  // Sign-extend if necessary.
  if (result_negative && is_signed) result |= ~mask;

  return result;
}

void Simulator::VisitSVEIndexGeneration(const Instruction* instr) {
  USE(instr);
  VectorFormat vform = instr->GetSVEVectorFormat();
  SimVRegister& zd = ReadVRegister(instr->GetRd());
  switch (instr->Mask(SVEIndexGenerationMask)) {
    case INDEX_z_ii:
    case INDEX_z_ir:
    case INDEX_z_ri:
    case INDEX_z_rr: {
      uint64_t start = instr->ExtractBit(10) ? ReadXRegister(instr->GetRn())
                                             : instr->ExtractSignedBits(9, 5);
      uint64_t step = instr->ExtractBit(11) ? ReadXRegister(instr->GetRm())
                                            : instr->ExtractSignedBits(20, 16);
      index(vform, zd, start, step);
      break;
    }
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEIntArithmeticUnpredicated(const Instruction* instr) {
  USE(instr);
  VectorFormat vform = instr->GetSVEVectorFormat();
  SimVRegister& zd = ReadVRegister(instr->GetRd());
  SimVRegister& zn = ReadVRegister(instr->GetRn());
  SimVRegister& zm = ReadVRegister(instr->GetRm());
  switch (instr->Mask(SVEIntArithmeticUnpredicatedMask)) {
    case ADD_z_zz:
      add(vform, zd, zn, zm);
      break;
    case SQADD_z_zz:
      add(vform, zd, zn, zm).SignedSaturate(vform);
      break;
    case SQSUB_z_zz:
      sub(vform, zd, zn, zm).SignedSaturate(vform);
      break;
    case SUB_z_zz:
      sub(vform, zd, zn, zm);
      break;
    case UQADD_z_zz:
      add(vform, zd, zn, zm).UnsignedSaturate(vform);
      break;
    case UQSUB_z_zz:
      sub(vform, zd, zn, zm).UnsignedSaturate(vform);
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEIntBinaryArithmeticPredicated(
    const Instruction* instr) {
  USE(instr);
  VectorFormat vform = instr->GetSVEVectorFormat();
  SimVRegister& zdn = ReadVRegister(instr->GetRd());
  SimVRegister& zm = ReadVRegister(instr->GetRn());
  SimPRegister& pg = ReadPRegister(instr->GetPgLow8());
  SimVRegister result;

  // Get the size specifier for division instructions.
  VectorFormat div_vform = kFormatUndefined;
  unsigned div_size = instr->ExtractBits(23, 22);
  if (div_size == 0) div_vform = kFormatVnS;
  if (div_size == 1) div_vform = kFormatVnD;

  switch (instr->Mask(SVEIntBinaryArithmeticPredicatedMask)) {
    case ADD_z_p_zz:
      add(vform, result, zdn, zm);
      break;
    case AND_z_p_zz:
      SVEBitwiseLogicalUnpredicatedHelper(AND, vform, result, zdn, zm);
      break;
    case BIC_z_p_zz:
      SVEBitwiseLogicalUnpredicatedHelper(BIC, vform, result, zdn, zm);
      break;
    case EOR_z_p_zz:
      SVEBitwiseLogicalUnpredicatedHelper(EOR, vform, result, zdn, zm);
      break;
    case MUL_z_p_zz:
      mul(vform, result, zdn, zm);
      break;
    case ORR_z_p_zz:
      SVEBitwiseLogicalUnpredicatedHelper(ORR, vform, result, zdn, zm);
      break;
    case SABD_z_p_zz:
      absdiff(vform, result, zdn, zm, true);
      break;
    case SDIVR_z_p_zz:
      vform = div_vform;
      sdiv(vform, result, zm, zdn);
      break;
    case SDIV_z_p_zz:
      vform = div_vform;
      sdiv(vform, result, zdn, zm);
      break;
    case SMAX_z_p_zz:
      smax(vform, result, zdn, zm);
      break;
    case SMIN_z_p_zz:
      smin(vform, result, zdn, zm);
      break;
    case SMULH_z_p_zz:
      smulh(vform, result, zdn, zm);
      break;
    case SUBR_z_p_zz:
      sub(vform, result, zm, zdn);
      break;
    case SUB_z_p_zz:
      sub(vform, result, zdn, zm);
      break;
    case UABD_z_p_zz:
      absdiff(vform, result, zdn, zm, false);
      break;
    case UDIVR_z_p_zz:
      vform = div_vform;
      udiv(vform, result, zm, zdn);
      break;
    case UDIV_z_p_zz:
      vform = div_vform;
      udiv(vform, result, zdn, zm);
      break;
    case UMAX_z_p_zz:
      umax(vform, result, zdn, zm);
      break;
    case UMIN_z_p_zz:
      umin(vform, result, zdn, zm);
      break;
    case UMULH_z_p_zz:
      umulh(vform, result, zdn, zm);
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
  mov_merging(vform, zdn, pg, result);
}

void Simulator::VisitSVEIntCompareScalars(const Instruction* instr) {
  USE(instr);
  unsigned rn_code = instr->GetRn();
  unsigned rm_code = instr->GetRm();

  if (instr->Mask(SVEIntCompareCountAndLimitScalarsFMask) ==
      SVEIntCompareCountAndLimitScalarsFixed) {
    SimPRegister& pd = ReadPRegister(instr->GetPd());
    VectorFormat vform = instr->GetSVEVectorFormat();
    bool is_64_bit = instr->ExtractBit(12) == 1;
    int64_t src1 = is_64_bit ? ReadXRegister(rn_code) : ReadWRegister(rn_code);
    int64_t src2 = is_64_bit ? ReadXRegister(rm_code) : ReadWRegister(rm_code);

    bool last = true;
    for (int lane = 0; lane < LaneCountFromFormat(vform); lane++) {
      bool cond = false;
      switch (instr->Mask(SVEIntCompareCountAndLimitScalarsMask)) {
        case WHILELT_p_p_rr:
          cond = src1 < src2;
          break;
        case WHILELE_p_p_rr:
          cond = src1 <= src2;
          break;
        case WHILELO_p_p_rr:
          cond = static_cast<uint64_t>(src1) < static_cast<uint64_t>(src2);
          break;
        case WHILELS_p_p_rr:
          cond = static_cast<uint64_t>(src1) <= static_cast<uint64_t>(src2);
          break;
        default:
          VIXL_UNIMPLEMENTED();
          break;
      }
      last = last && cond;
      LogicPRegister dst(pd);
      dst.SetActive(vform, lane, last);
      src1 += 1;
    }

    SimPRegister temp;
    LogicPRegister ones(temp);
    ones.SetAllBits();

    PredTest(vform, ones, pd);
  } else {
    VIXL_ASSERT(instr->Mask(SVEIntCompareCondTerminateScalarsFMask) ==
                SVEIntCompareCondTerminateScalarsFixed);
    bool is_64_bit = instr->ExtractBit(22) == 1;
    uint64_t src1 = is_64_bit ? ReadXRegister(rn_code) : ReadWRegister(rn_code);
    uint64_t src2 = is_64_bit ? ReadXRegister(rm_code) : ReadWRegister(rm_code);
    bool term;
    switch (instr->Mask(SVEIntCompareCondTerminateScalarsMask)) {
      case CTERMEQ_rr:
        term = src1 == src2;
        break;
      case CTERMNE_rr:
        term = src1 != src2;
        break;
      default:
        term = false;
        VIXL_UNIMPLEMENTED();
        break;
    }
    ReadNzcv().SetN(term ? 1 : 0);
    ReadNzcv().SetV(term ? 0 : !ReadC());
  }
  LogSystemRegister(NZCV);
}

void Simulator::VisitSVEIntCompareSignedImm(const Instruction* instr) {
  USE(instr);
  bool commute_inputs = false;
  Condition cond;
  switch (instr->Mask(SVEIntCompareSignedImmMask)) {
    case CMPEQ_p_p_zi:
      cond = eq;
      break;
    case CMPGE_p_p_zi:
      cond = ge;
      break;
    case CMPGT_p_p_zi:
      cond = gt;
      break;
    case CMPLE_p_p_zi:
      cond = ge;
      commute_inputs = true;
      break;
    case CMPLT_p_p_zi:
      cond = gt;
      commute_inputs = true;
      break;
    case CMPNE_p_p_zi:
      cond = ne;
      break;
    default:
      cond = al;
      VIXL_UNIMPLEMENTED();
      break;
  }

  VectorFormat vform = instr->GetSVEVectorFormat();
  SimVRegister src2;
  dup_immediate(vform,
                src2,
                ExtractSignedBitfield64(4, 0, instr->ExtractBits(20, 16)));
  SVEIntCompareVectorsHelper(cond,
                             vform,
                             ReadPRegister(instr->GetPd()),
                             ReadPRegister(instr->GetPgLow8()),
                             commute_inputs ? src2
                                            : ReadVRegister(instr->GetRn()),
                             commute_inputs ? ReadVRegister(instr->GetRn())
                                            : src2);
}

void Simulator::VisitSVEIntCompareUnsignedImm(const Instruction* instr) {
  USE(instr);
  bool commute_inputs = false;
  Condition cond;
  switch (instr->Mask(SVEIntCompareUnsignedImmMask)) {
    case CMPHI_p_p_zi:
      cond = hi;
      break;
    case CMPHS_p_p_zi:
      cond = hs;
      break;
    case CMPLO_p_p_zi:
      cond = hi;
      commute_inputs = true;
      break;
    case CMPLS_p_p_zi:
      cond = hs;
      commute_inputs = true;
      break;
    default:
      cond = al;
      VIXL_UNIMPLEMENTED();
      break;
  }

  VectorFormat vform = instr->GetSVEVectorFormat();
  SimVRegister src2;
  dup_immediate(vform, src2, instr->ExtractBits(20, 14));
  SVEIntCompareVectorsHelper(cond,
                             vform,
                             ReadPRegister(instr->GetPd()),
                             ReadPRegister(instr->GetPgLow8()),
                             commute_inputs ? src2
                                            : ReadVRegister(instr->GetRn()),
                             commute_inputs ? ReadVRegister(instr->GetRn())
                                            : src2);
}

void Simulator::VisitSVEIntCompareVectors(const Instruction* instr) {
  USE(instr);

  Instr op = instr->Mask(SVEIntCompareVectorsMask);
  bool is_wide_elements = false;
  switch (op) {
    case CMPEQ_p_p_zw:
    case CMPGE_p_p_zw:
    case CMPGT_p_p_zw:
    case CMPHI_p_p_zw:
    case CMPHS_p_p_zw:
    case CMPLE_p_p_zw:
    case CMPLO_p_p_zw:
    case CMPLS_p_p_zw:
    case CMPLT_p_p_zw:
    case CMPNE_p_p_zw:
      is_wide_elements = true;
      break;
  }

  Condition cond;
  switch (op) {
    case CMPEQ_p_p_zw:
    case CMPEQ_p_p_zz:
      cond = eq;
      break;
    case CMPGE_p_p_zw:
    case CMPGE_p_p_zz:
      cond = ge;
      break;
    case CMPGT_p_p_zw:
    case CMPGT_p_p_zz:
      cond = gt;
      break;
    case CMPHI_p_p_zw:
    case CMPHI_p_p_zz:
      cond = hi;
      break;
    case CMPHS_p_p_zw:
    case CMPHS_p_p_zz:
      cond = hs;
      break;
    case CMPNE_p_p_zw:
    case CMPNE_p_p_zz:
      cond = ne;
      break;
    case CMPLE_p_p_zw:
      cond = le;
      break;
    case CMPLO_p_p_zw:
      cond = lo;
      break;
    case CMPLS_p_p_zw:
      cond = ls;
      break;
    case CMPLT_p_p_zw:
      cond = lt;
      break;
    default:
      VIXL_UNIMPLEMENTED();
      cond = al;
      break;
  }

  SVEIntCompareVectorsHelper(cond,
                             instr->GetSVEVectorFormat(),
                             ReadPRegister(instr->GetPd()),
                             ReadPRegister(instr->GetPgLow8()),
                             ReadVRegister(instr->GetRn()),
                             ReadVRegister(instr->GetRm()),
                             is_wide_elements);
}

void Simulator::VisitSVEIntMiscUnpredicated(const Instruction* instr) {
  USE(instr);

  SimVRegister& zd = ReadVRegister(instr->GetRd());
  SimVRegister& zn = ReadVRegister(instr->GetRn());

  switch (instr->Mask(SVEIntMiscUnpredicatedMask)) {
    case FEXPA_z_z:
      VIXL_UNIMPLEMENTED();
      break;
    case FTSSEL_z_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case MOVPRFX_z_z:
      mov(kFormatVnD, zd, zn);  // The lane size is arbitrary.
      // Record the movprfx, so the next ExecuteInstruction() can check it.
      movprfx_ = instr;
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEIntMulAddPredicated(const Instruction* instr) {
  USE(instr);
  VectorFormat vform = instr->GetSVEVectorFormat();

  SimVRegister& zd = ReadVRegister(instr->GetRd());
  SimVRegister& zm = ReadVRegister(instr->GetRm());

  SimVRegister result;
  switch (instr->Mask(SVEIntMulAddPredicatedMask)) {
    case MLA_z_p_zzz:
      mla(vform, result, zd, ReadVRegister(instr->GetRn()), zm);
      break;
    case MLS_z_p_zzz:
      mls(vform, result, zd, ReadVRegister(instr->GetRn()), zm);
      break;
    case MAD_z_p_zzz:
      // 'za' is encoded in 'Rn'.
      mla(vform, result, ReadVRegister(instr->GetRn()), zd, zm);
      break;
    case MSB_z_p_zzz: {
      // 'za' is encoded in 'Rn'.
      mls(vform, result, ReadVRegister(instr->GetRn()), zd, zm);
      break;
    }
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
  mov_merging(vform, zd, ReadPRegister(instr->GetPgLow8()), result);
}

void Simulator::VisitSVEIntMulAddUnpredicated(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEIntMulAddUnpredicatedMask)) {
    case SDOT_z_zzz:
      VIXL_UNIMPLEMENTED();
      break;
    case UDOT_z_zzz:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEIntReduction(const Instruction* instr) {
  USE(instr);

  VectorFormat vform = instr->GetSVEVectorFormat();
  SimVRegister& zn = ReadVRegister(instr->GetRn());
  SimPRegister& pg = ReadPRegister(instr->GetPgLow8());

  if (instr->Mask(SVEIntReductionLogicalFMask) == SVEIntReductionLogicalFixed) {
    switch (instr->Mask(SVEIntReductionLogicalMask)) {
      case ANDV_r_p_z:
        VIXL_UNIMPLEMENTED();
        break;
      case EORV_r_p_z:
        VIXL_UNIMPLEMENTED();
        break;
      case ORV_r_p_z:
        VIXL_UNIMPLEMENTED();
        break;
      default:
        VIXL_UNIMPLEMENTED();
        break;
    }
  } else {
    switch (instr->Mask(SVEIntReductionMask)) {
      case MOVPRFX_z_p_z: {
        SimVRegister& zd = ReadVRegister(instr->GetRd());
        if (instr->ExtractBit(16)) {
          mov_merging(vform, zd, pg, zn);
        } else {
          mov_zeroing(vform, zd, pg, zn);
        }
        // Record the movprfx, so the next ExecuteInstruction() can check it.
        movprfx_ = instr;
        break;
      }
      case SADDV_r_p_z:
        VIXL_UNIMPLEMENTED();
        break;
      case SMAXV_r_p_z:
        VIXL_UNIMPLEMENTED();
        break;
      case SMINV_r_p_z:
        VIXL_UNIMPLEMENTED();
        break;
      case UADDV_r_p_z:
        VIXL_UNIMPLEMENTED();
        break;
      case UMAXV_r_p_z:
        VIXL_UNIMPLEMENTED();
        break;
      case UMINV_r_p_z:
        VIXL_UNIMPLEMENTED();
        break;
      default:
        VIXL_UNIMPLEMENTED();
        break;
    }
  }
}

void Simulator::VisitSVEIntUnaryArithmeticPredicated(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEIntUnaryArithmeticPredicatedMask)) {
    case ABS_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case CLS_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case CLZ_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case CNOT_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case CNT_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case FABS_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case FNEG_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case NEG_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case NOT_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case SXTB_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case SXTH_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case SXTW_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case UXTB_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case UXTH_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case UXTW_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEIntWideImmPredicated(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEIntWideImmPredicatedMask)) {
    case CPY_z_p_i:
      VIXL_UNIMPLEMENTED();
      break;
    case FCPY_z_p_i:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEIntWideImmUnpredicated(const Instruction* instr) {
  USE(instr);
  SimVRegister& zd = ReadVRegister(instr->GetRd());
  switch (instr->Mask(SVEIntWideImmUnpredicatedMask)) {
    case ADD_z_zi:
      VIXL_UNIMPLEMENTED();
      break;
    case DUP_z_i:
      dup_immediate(instr->GetSVEVectorFormat(),
                    zd,
                    instr->GetImmSVEIntWideSigned());
      break;
    case FDUP_z_i:
      VIXL_UNIMPLEMENTED();
      break;
    case MUL_z_zi:
      VIXL_UNIMPLEMENTED();
      break;
    case SMAX_z_zi:
      VIXL_UNIMPLEMENTED();
      break;
    case SMIN_z_zi:
      VIXL_UNIMPLEMENTED();
      break;
    case SQADD_z_zi:
      VIXL_UNIMPLEMENTED();
      break;
    case SQSUB_z_zi:
      VIXL_UNIMPLEMENTED();
      break;
    case SUBR_z_zi:
      VIXL_UNIMPLEMENTED();
      break;
    case SUB_z_zi:
      VIXL_UNIMPLEMENTED();
      break;
    case UMAX_z_zi:
      VIXL_UNIMPLEMENTED();
      break;
    case UMIN_z_zi:
      VIXL_UNIMPLEMENTED();
      break;
    case UQADD_z_zi:
      VIXL_UNIMPLEMENTED();
      break;
    case UQSUB_z_zi:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEMem32BitGatherAndUnsizedContiguous(
    const Instruction* instr) {
  USE(instr);
  if (instr->Mask(SVEMemUnsizedContiguousLoadPMask) == LDR_p_bi) {
    SimPRegister& pt = ReadPRegister(instr->GetPt());
    uint64_t address = ReadXRegister(instr->GetRn());
    if (instr->Mask(0x003f1c00)) {
      // TODO: Support the VL multiplier.
      VIXL_UNIMPLEMENTED();
    }
    for (unsigned i = 0; i < GetPredicateLengthInBytes(); i++) {
      pt.Insert(i, Memory::Read<uint8_t>(address + i));
    }
  } else {
    // TODO: This switch doesn't work because the mask needs to vary on a finer
    // granularity. Early implementations have already been pulled out, but we
    // need to re-organise the instruction groups.
    switch (instr->Mask(SVEMem32BitGatherAndUnsizedContiguousMask)) {
      case LD1B_z_p_ai_s:
      case LD1B_z_p_bz_s_x32_unscaled:
      case LD1H_z_p_ai_s:
      case LD1H_z_p_bz_s_x32_scaled:
      case LD1H_z_p_bz_s_x32_unscaled:
      case LD1RB_z_p_bi_u16:
      case LD1RB_z_p_bi_u32:
      case LD1RB_z_p_bi_u64:
      case LD1RB_z_p_bi_u8:
      case LD1RD_z_p_bi_u64:
      case LD1RH_z_p_bi_u16:
      case LD1RH_z_p_bi_u32:
      case LD1RH_z_p_bi_u64:
      case LD1RSB_z_p_bi_s16:
      case LD1RSB_z_p_bi_s32:
      case LD1RSB_z_p_bi_s64:
      case LD1RSH_z_p_bi_s32:
      case LD1RSH_z_p_bi_s64:
      case LD1RSW_z_p_bi_s64:
      case LD1RW_z_p_bi_u32:
      case LD1RW_z_p_bi_u64:
      case LD1SB_z_p_ai_s:
      case LD1SB_z_p_bz_s_x32_unscaled:
      case LD1SH_z_p_ai_s:
      case LD1SH_z_p_bz_s_x32_scaled:
      case LD1SH_z_p_bz_s_x32_unscaled:
      case LD1W_z_p_ai_s:
      case LD1W_z_p_bz_s_x32_scaled:
      case LD1W_z_p_bz_s_x32_unscaled:
      case LDFF1B_z_p_ai_s:
      case LDFF1B_z_p_bz_s_x32_unscaled:
      case LDFF1H_z_p_ai_s:
      case LDFF1H_z_p_bz_s_x32_scaled:
      case LDFF1H_z_p_bz_s_x32_unscaled:
      case LDFF1SB_z_p_ai_s:
      case LDFF1SB_z_p_bz_s_x32_unscaled:
      case LDFF1SH_z_p_ai_s:
      case LDFF1SH_z_p_bz_s_x32_scaled:
      case LDFF1SH_z_p_bz_s_x32_unscaled:
      case LDFF1W_z_p_ai_s:
      case LDFF1W_z_p_bz_s_x32_scaled:
      case LDFF1W_z_p_bz_s_x32_unscaled:
      case LDR_z_bi:
      case PRFB_i_p_ai_s:
      case PRFB_i_p_bi_s:
      case PRFB_i_p_br_s:
      case PRFB_i_p_bz_s_x32_scaled:
      case PRFD_i_p_ai_s:
      case PRFD_i_p_bi_s:
      case PRFD_i_p_br_s:
      case PRFD_i_p_bz_s_x32_scaled:
      case PRFH_i_p_ai_s:
      case PRFH_i_p_bi_s:
      case PRFH_i_p_br_s:
      case PRFH_i_p_bz_s_x32_scaled:
      case PRFW_i_p_ai_s:
      case PRFW_i_p_bi_s:
      case PRFW_i_p_br_s:
      case PRFW_i_p_bz_s_x32_scaled:
      default:
        VIXL_UNIMPLEMENTED();
        break;
    }
  }
  // TODO: LogRead
}

void Simulator::VisitSVEMem64BitGather(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEMem64BitGatherMask)) {
    case LD1B_z_p_ai_d:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1B_z_p_bz_d_64_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1B_z_p_bz_d_x32_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1D_z_p_ai_d:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1D_z_p_bz_d_64_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1D_z_p_bz_d_64_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1D_z_p_bz_d_x32_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1D_z_p_bz_d_x32_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1H_z_p_ai_d:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1H_z_p_bz_d_64_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1H_z_p_bz_d_64_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1H_z_p_bz_d_x32_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1H_z_p_bz_d_x32_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SB_z_p_ai_d:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SB_z_p_bz_d_64_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SB_z_p_bz_d_x32_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SH_z_p_ai_d:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SH_z_p_bz_d_64_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SH_z_p_bz_d_64_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SH_z_p_bz_d_x32_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SH_z_p_bz_d_x32_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SW_z_p_ai_d:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SW_z_p_bz_d_64_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SW_z_p_bz_d_64_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SW_z_p_bz_d_x32_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SW_z_p_bz_d_x32_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1W_z_p_ai_d:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1W_z_p_bz_d_64_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1W_z_p_bz_d_64_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1W_z_p_bz_d_x32_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1W_z_p_bz_d_x32_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1B_z_p_ai_d:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1B_z_p_bz_d_64_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1B_z_p_bz_d_x32_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1D_z_p_ai_d:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1D_z_p_bz_d_64_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1D_z_p_bz_d_64_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1D_z_p_bz_d_x32_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1D_z_p_bz_d_x32_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1H_z_p_ai_d:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1H_z_p_bz_d_64_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1H_z_p_bz_d_64_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1H_z_p_bz_d_x32_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1H_z_p_bz_d_x32_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1SB_z_p_ai_d:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1SB_z_p_bz_d_64_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1SB_z_p_bz_d_x32_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1SH_z_p_ai_d:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1SH_z_p_bz_d_64_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1SH_z_p_bz_d_64_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1SH_z_p_bz_d_x32_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1SH_z_p_bz_d_x32_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1SW_z_p_ai_d:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1SW_z_p_bz_d_64_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1SW_z_p_bz_d_64_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1SW_z_p_bz_d_x32_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1SW_z_p_bz_d_x32_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1W_z_p_ai_d:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1W_z_p_bz_d_64_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1W_z_p_bz_d_64_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1W_z_p_bz_d_x32_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1W_z_p_bz_d_x32_unscaled:
      VIXL_UNIMPLEMENTED();
      break;
    case PRFB_i_p_ai_d:
      VIXL_UNIMPLEMENTED();
      break;
    case PRFB_i_p_bz_d_64_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case PRFB_i_p_bz_d_x32_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case PRFD_i_p_ai_d:
      VIXL_UNIMPLEMENTED();
      break;
    case PRFD_i_p_bz_d_64_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case PRFD_i_p_bz_d_x32_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case PRFH_i_p_ai_d:
      VIXL_UNIMPLEMENTED();
      break;
    case PRFH_i_p_bz_d_64_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case PRFH_i_p_bz_d_x32_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case PRFW_i_p_ai_d:
      VIXL_UNIMPLEMENTED();
      break;
    case PRFW_i_p_bz_d_64_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    case PRFW_i_p_bz_d_x32_scaled:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEMemContiguousLoad(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEMemContiguousLoadMask)) {
    case LD1B_z_p_bi_u16:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1B_z_p_bi_u32:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1B_z_p_bi_u64:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1B_z_p_bi_u8:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1B_z_p_br_u16:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1B_z_p_br_u32:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1B_z_p_br_u64:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1B_z_p_br_u8:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1D_z_p_bi_u64:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1D_z_p_br_u64:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1H_z_p_bi_u16:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1H_z_p_bi_u32:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1H_z_p_bi_u64:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1H_z_p_br_u16:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1H_z_p_br_u32:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1H_z_p_br_u64:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1RQB_z_p_bi_u8:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1RQB_z_p_br_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1RQD_z_p_bi_u64:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1RQD_z_p_br_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1RQH_z_p_bi_u16:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1RQH_z_p_br_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1RQW_z_p_bi_u32:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1RQW_z_p_br_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SB_z_p_bi_s16:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SB_z_p_bi_s32:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SB_z_p_bi_s64:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SB_z_p_br_s16:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SB_z_p_br_s32:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SB_z_p_br_s64:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SH_z_p_bi_s32:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SH_z_p_bi_s64:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SH_z_p_br_s32:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SH_z_p_br_s64:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SW_z_p_bi_s64:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1SW_z_p_br_s64:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1W_z_p_bi_u32:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1W_z_p_bi_u64:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1W_z_p_br_u32:
      VIXL_UNIMPLEMENTED();
      break;
    case LD1W_z_p_br_u64:
      VIXL_UNIMPLEMENTED();
      break;
    case LD2B_z_p_bi_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD2B_z_p_br_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD2D_z_p_bi_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD2D_z_p_br_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD2H_z_p_bi_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD2H_z_p_br_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD2W_z_p_bi_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD2W_z_p_br_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD3B_z_p_bi_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD3B_z_p_br_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD3D_z_p_bi_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD3D_z_p_br_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD3H_z_p_bi_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD3H_z_p_br_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD3W_z_p_bi_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD3W_z_p_br_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD4B_z_p_bi_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD4B_z_p_br_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD4D_z_p_bi_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD4D_z_p_br_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD4H_z_p_bi_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD4H_z_p_br_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD4W_z_p_bi_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LD4W_z_p_br_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1B_z_p_br_u16:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1B_z_p_br_u32:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1B_z_p_br_u64:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1B_z_p_br_u8:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1D_z_p_br_u64:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1H_z_p_br_u16:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1H_z_p_br_u32:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1H_z_p_br_u64:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1SB_z_p_br_s16:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1SB_z_p_br_s32:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1SB_z_p_br_s64:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1SH_z_p_br_s32:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1SH_z_p_br_s64:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1SW_z_p_br_s64:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1W_z_p_br_u32:
      VIXL_UNIMPLEMENTED();
      break;
    case LDFF1W_z_p_br_u64:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNF1B_z_p_bi_u16:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNF1B_z_p_bi_u32:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNF1B_z_p_bi_u64:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNF1B_z_p_bi_u8:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNF1D_z_p_bi_u64:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNF1H_z_p_bi_u16:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNF1H_z_p_bi_u32:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNF1H_z_p_bi_u64:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNF1SB_z_p_bi_s16:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNF1SB_z_p_bi_s32:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNF1SB_z_p_bi_s64:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNF1SH_z_p_bi_s32:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNF1SH_z_p_bi_s64:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNF1SW_z_p_bi_s64:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNF1W_z_p_bi_u32:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNF1W_z_p_bi_u64:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNT1B_z_p_bi_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNT1B_z_p_br_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNT1D_z_p_bi_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNT1D_z_p_br_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNT1H_z_p_bi_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNT1H_z_p_br_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNT1W_z_p_bi_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    case LDNT1W_z_p_br_contiguous:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEMemStore(const Instruction* instr) {
  USE(instr);
  if (instr->Mask(SVEMemStorePMask) == STR_p_bi) {
    SimPRegister& pt = ReadPRegister(instr->GetPt());
    uint64_t address = ReadXRegister(instr->GetRn());
    if (instr->Mask(0x003f1c00)) {
      // TODO: Support the VL multiplier.
      VIXL_UNIMPLEMENTED();
    }
    for (unsigned i = 0; i < GetPredicateLengthInBytes(); i++) {
      Memory::Write(address + i, pt.GetLane<uint8_t>(i));
    }
  } else if (instr->Mask(SVEMemStoreZMask) == STR_z_bi) {
    SimVRegister& zt = ReadVRegister(instr->GetRt());
    uint64_t address = ReadXRegister(instr->GetRn());
    if (instr->Mask(0x003f1c00)) {
      // TODO: Support the VL multiplier.
      VIXL_UNIMPLEMENTED();
    }
    for (unsigned i = 0; i < GetVectorLengthInBytes(); i++) {
      Memory::Write(address + i, zt.GetLane<uint8_t>(i));
    }
  } else {
    // TODO: This switch doesn't work because the mask needs to vary on a finer
    // granularity. Early implementations have already been pulled out, but we
    // need to re-organise the instruction groups.
    switch (instr->Mask(SVEMemStoreMask)) {
      case ST1B_z_p_ai_d:
      case ST1B_z_p_ai_s:
      case ST1B_z_p_bi:
      case ST1B_z_p_br:
      case ST1B_z_p_bz_d_64_unscaled:
      case ST1B_z_p_bz_d_x32_unscaled:
      case ST1B_z_p_bz_s_x32_unscaled:
      case ST1D_z_p_ai_d:
      case ST1D_z_p_bi:
      // TODO: fix encoding alias issue with enum above.
      //    case ST1D_z_p_br:
      case ST1D_z_p_bz_d_64_scaled:
      case ST1D_z_p_bz_d_64_unscaled:
      case ST1D_z_p_bz_d_x32_scaled:
      case ST1D_z_p_bz_d_x32_unscaled:
      case ST1H_z_p_ai_d:
      case ST1H_z_p_ai_s:
      case ST1H_z_p_bi:
      case ST1H_z_p_br:
      case ST1H_z_p_bz_d_64_scaled:
      case ST1H_z_p_bz_d_64_unscaled:
      case ST1H_z_p_bz_d_x32_scaled:
      case ST1H_z_p_bz_d_x32_unscaled:
      case ST1H_z_p_bz_s_x32_scaled:
      case ST1H_z_p_bz_s_x32_unscaled:
      case ST1W_z_p_ai_d:
      case ST1W_z_p_ai_s:
      case ST1W_z_p_bi:
      case ST1W_z_p_br:
      case ST1W_z_p_bz_d_64_scaled:
      case ST1W_z_p_bz_d_64_unscaled:
      case ST1W_z_p_bz_d_x32_scaled:
      case ST1W_z_p_bz_d_x32_unscaled:
      case ST1W_z_p_bz_s_x32_scaled:
      case ST1W_z_p_bz_s_x32_unscaled:
      case ST2B_z_p_bi_contiguous:
      case ST2B_z_p_br_contiguous:
      case ST2D_z_p_bi_contiguous:
      case ST2D_z_p_br_contiguous:
      case ST2H_z_p_bi_contiguous:
      case ST2H_z_p_br_contiguous:
      case ST2W_z_p_bi_contiguous:
      case ST2W_z_p_br_contiguous:
      case ST3B_z_p_bi_contiguous:
      case ST3B_z_p_br_contiguous:
      case ST3D_z_p_bi_contiguous:
      case ST3D_z_p_br_contiguous:
      case ST3H_z_p_bi_contiguous:
      case ST3H_z_p_br_contiguous:
      case ST3W_z_p_bi_contiguous:
      case ST3W_z_p_br_contiguous:
      case ST4B_z_p_bi_contiguous:
      case ST4B_z_p_br_contiguous:
      case ST4D_z_p_bi_contiguous:
      case ST4D_z_p_br_contiguous:
      case ST4H_z_p_bi_contiguous:
      case ST4H_z_p_br_contiguous:
      case ST4W_z_p_bi_contiguous:
      case ST4W_z_p_br_contiguous:
      case STNT1B_z_p_bi_contiguous:
      case STNT1B_z_p_br_contiguous:
      case STNT1D_z_p_bi_contiguous:
      case STNT1D_z_p_br_contiguous:
      case STNT1H_z_p_bi_contiguous:
      case STNT1H_z_p_br_contiguous:
      case STNT1W_z_p_bi_contiguous:
      case STNT1W_z_p_br_contiguous:
      default:
        VIXL_UNIMPLEMENTED();
        break;
    }
  }
  // TODO: LogWrite
}

void Simulator::VisitSVEMulIndex(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEMulIndexMask)) {
    case SDOT_z_zzzi_d:
      VIXL_UNIMPLEMENTED();
      break;
    case SDOT_z_zzzi_s:
      VIXL_UNIMPLEMENTED();
      break;
    case UDOT_z_zzzi_d:
      VIXL_UNIMPLEMENTED();
      break;
    case UDOT_z_zzzi_s:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEPartitionBreak(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEPartitionBreakMask)) {
    case BRKAS_p_p_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case BRKA_p_p_p:
      VIXL_UNIMPLEMENTED();
      break;
    case BRKBS_p_p_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case BRKB_p_p_p:
      VIXL_UNIMPLEMENTED();
      break;
    case BRKNS_p_p_pp:
      VIXL_UNIMPLEMENTED();
      break;
    case BRKN_p_p_pp:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEPermutePredicate(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEPermutePredicateMask)) {
    case PUNPKHI_p_p:
      VIXL_UNIMPLEMENTED();
      break;
    case PUNPKLO_p_p:
      VIXL_UNIMPLEMENTED();
      break;
    case REV_p_p:
      VIXL_UNIMPLEMENTED();
      break;
    case TRN1_p_pp:
      VIXL_UNIMPLEMENTED();
      break;
    case TRN2_p_pp:
      VIXL_UNIMPLEMENTED();
      break;
    case UZP1_p_pp:
      VIXL_UNIMPLEMENTED();
      break;
    case UZP2_p_pp:
      VIXL_UNIMPLEMENTED();
      break;
    case ZIP1_p_pp:
      VIXL_UNIMPLEMENTED();
      break;
    case ZIP2_p_pp:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEPermuteVectorExtract(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEPermuteVectorExtractMask)) {
    case EXT_z_zi_des:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEPermuteVectorInterleaving(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEPermuteVectorInterleavingMask)) {
    case TRN1_z_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case TRN2_z_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case UZP1_z_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case UZP2_z_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case ZIP1_z_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case ZIP2_z_zz:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEPermuteVectorPredicated(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEPermuteVectorPredicatedMask)) {
    case CLASTA_r_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case CLASTA_v_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case CLASTA_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case CLASTB_r_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case CLASTB_v_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case CLASTB_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    case COMPACT_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case CPY_z_p_r:
      VIXL_UNIMPLEMENTED();
      break;
    case CPY_z_p_v:
      VIXL_UNIMPLEMENTED();
      break;
    case LASTA_r_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case LASTA_v_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case LASTB_r_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case LASTB_v_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case RBIT_z_p_z:
      VIXL_UNIMPLEMENTED();
      break;
    case REVB_z_z:
      VIXL_UNIMPLEMENTED();
      break;
    case REVH_z_z:
      VIXL_UNIMPLEMENTED();
      break;
    case REVW_z_z:
      VIXL_UNIMPLEMENTED();
      break;
    case SPLICE_z_p_zz_des:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEPermuteVectorUnpredicated(const Instruction* instr) {
  USE(instr);
  SimVRegister& zd = ReadVRegister(instr->GetRd());

  switch (instr->Mask(SVEPermuteVectorUnpredicatedDupTBLMask)) {
    case DUP_z_zi: {
      std::pair<int, int> index_and_lane_size =
          instr->GetSVEPermuteIndexAndLaneSizeLog2();
      int index = index_and_lane_size.first;
      int lane_size_in_bytes_log_2 = index_and_lane_size.second;
      VectorFormat vform =
          SVEFormatFromLaneSizeInBytesLog2(lane_size_in_bytes_log_2);
      if ((index < 0) || (index >= LaneCountFromFormat(vform))) {
        // Out of bounds, set the destination register to zero.
        dup_immediate(kFormatVnD, zd, 0);
      } else {
        dup_element(vform, zd, ReadVRegister(instr->GetRn()), index);
      }
      return;
    }
    case TBL_z_zz_1:
      Table(instr->GetSVEVectorFormat(),
            zd,
            ReadVRegister(instr->GetRn()),
            ReadVRegister(instr->GetRm()));
      return;
    default:
      break;
  }

  VectorFormat vform = instr->GetSVEVectorFormat();
  switch (instr->Mask(SVEPermuteVectorUnpredicatedMask)) {
    case DUP_z_r:
      dup_immediate(vform,
                    zd,
                    ReadXRegister(instr->GetRn(), Reg31IsStackPointer));
      break;
    case INSR_z_r:
      insr(vform, zd, ReadXRegister(instr->GetRn()));
      break;
    case INSR_z_v:
      insr(vform, zd, ReadDRegisterBits(instr->GetRn()));
      break;
    case REV_z_z:
      rev(vform, zd, ReadVRegister(instr->GetRn()));
      break;
    case SUNPKHI_z_z:
      unpk(vform, zd, ReadVRegister(instr->GetRn()), kHiHalf, kSignedExtend);
      break;
    case SUNPKLO_z_z:
      unpk(vform, zd, ReadVRegister(instr->GetRn()), kLoHalf, kSignedExtend);
      break;
    case UUNPKHI_z_z:
      unpk(vform, zd, ReadVRegister(instr->GetRn()), kHiHalf, kUnsignedExtend);
      break;
    case UUNPKLO_z_z:
      unpk(vform, zd, ReadVRegister(instr->GetRn()), kLoHalf, kUnsignedExtend);
      break;
    case TBL_z_zz_1:
    case DUP_z_zi:
      // Should be handled above.
      VIXL_UNREACHABLE();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEPredicateCount(const Instruction* instr) {
  USE(instr);

  VectorFormat vform = instr->GetSVEVectorFormat();
  SimPRegister& pg = ReadPRegister(instr->ExtractBits(13, 10));
  SimPRegister& pn = ReadPRegister(instr->GetPn());

  switch (instr->Mask(SVEPredicateCountMask)) {
    case CNTP_r_p_p: {
      WriteXRegister(instr->GetRd(), CountActiveAndTrueLanes(vform, pg, pn));
      break;
    }
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEPredicateLogicalOp(const Instruction* instr) {
  USE(instr);
  Instr op = instr->Mask(SVEPredicateLogicalOpMask);
  switch (op) {
    case ANDS_p_p_pp_z:
    case AND_p_p_pp_z:
    case BICS_p_p_pp_z:
    case BIC_p_p_pp_z:
    case EORS_p_p_pp_z:
    case EOR_p_p_pp_z:
    case NANDS_p_p_pp_z:
    case NAND_p_p_pp_z:
    case NORS_p_p_pp_z:
    case NOR_p_p_pp_z:
    case ORNS_p_p_pp_z:
    case ORN_p_p_pp_z:
    case ORRS_p_p_pp_z:
    case ORR_p_p_pp_z:
    case SEL_p_p_pp: {
      FlagsUpdate flags =
          instr->Mask(SVEPredicateLogicalSetFlagsBit) ? SetFlags : LeaveFlags;
      SVEPredicateLogicalHelper(static_cast<SVEPredicateLogicalOp>(op),
                                ReadPRegister(instr->GetPd()),
                                ReadPRegister(instr->ExtractBits(13, 10)),
                                ReadPRegister(instr->GetPn()),
                                ReadPRegister(instr->GetPm()),
                                flags);
      break;
    }
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEPredicateFirstActive(const Instruction* instr) {
  USE(instr);
  LogicPRegister pg = ReadPRegister(instr->ExtractBits(8, 5));
  LogicPRegister pdn = ReadPRegister(instr->GetPd());
  switch (instr->Mask(SVEPredicateFirstActiveMask)) {
    case PFIRST_p_p_p:
      pfirst(pdn, pg, pdn);
      // TODO: Is this broken when pg == pdn?
      PredTest(kFormatVnB, pg, pdn);
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEPredicateInitialize(const Instruction* instr) {
  USE(instr);
  // This group only contains PTRUE{S}, and there are no unallocated encodings.
  VIXL_STATIC_ASSERT(
      SVEPredicateInitializeMask ==
      (SVEPredicateInitializeFMask | SVEPredicateInitializeSetFlagsBit));
  VIXL_ASSERT((instr->Mask(SVEPredicateInitializeMask) == PTRUE_p_s) ||
              (instr->Mask(SVEPredicateInitializeMask) == PTRUES_p_s));

  LogicPRegister pdn = ReadPRegister(instr->GetPd());
  VectorFormat vform = instr->GetSVEVectorFormat();

  ptrue(vform, pdn, instr->GetImmSVEPredicateConstraint());
  if (instr->ExtractBit(16)) PredTest(vform, pdn, pdn);
}

void Simulator::VisitSVEPredicateNextActive(const Instruction* instr) {
  USE(instr);
  // This group only contains PNEXT, and there are no unallocated encodings.
  VIXL_STATIC_ASSERT(SVEPredicateNextActiveFMask == SVEPredicateNextActiveMask);
  VIXL_ASSERT(instr->Mask(SVEPredicateNextActiveMask) == PNEXT_p_p_p);

  LogicPRegister pg = ReadPRegister(instr->ExtractBits(8, 5));
  LogicPRegister pdn = ReadPRegister(instr->GetPd());
  VectorFormat vform = instr->GetSVEVectorFormat();

  pnext(vform, pdn, pg, pdn);
  // TODO: Is this broken when pg == pdn?
  PredTest(vform, pg, pdn);
}

void Simulator::VisitSVEPredicateReadFromFFR_Predicated(
    const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEPredicateReadFromFFR_PredicatedMask)) {
    case RDFFR_p_p_f:
    case RDFFRS_p_p_f:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEPredicateReadFromFFR_Unpredicated(
    const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEPredicateReadFromFFR_UnpredicatedMask)) {
    case RDFFR_p_f:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEPredicateTest(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEPredicateTestMask)) {
    case PTEST_p_p:
      PredTest(kFormatVnB,
               ReadPRegister(instr->ExtractBits(13, 10)),
               ReadPRegister(instr->GetPn()));
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEPredicateZero(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEPredicateZeroMask)) {
    case PFALSE_p:
      pfalse(ReadPRegister(instr->GetPd()));
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEPropagateBreak(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEPropagateBreakMask)) {
    case BRKPAS_p_p_pp:
      VIXL_UNIMPLEMENTED();
      break;
    case BRKPA_p_p_pp:
      VIXL_UNIMPLEMENTED();
      break;
    case BRKPBS_p_p_pp:
      VIXL_UNIMPLEMENTED();
      break;
    case BRKPB_p_p_pp:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEStackAllocation(const Instruction* instr) {
  USE(instr);

  int64_t scale = instr->GetImmSVEVLScale();
  switch (instr->Mask(SVEStackAllocationSizeMask)) {
    case RDVL_r_i:  // Rd = VL * scale
      WriteXRegister(instr->GetRd(), GetVectorLengthInBytes() * scale);
      return;
  }

  uint64_t base = ReadXRegister(instr->GetRm(), Reg31IsStackPointer);
  switch (instr->Mask(SVEStackAllocationMask)) {
    case ADDPL_r_ri:  // Rd = Rn + (PL * scale)
      WriteXRegister(instr->GetRd(),
                     base + (GetPredicateLengthInBytes() * scale),
                     LogRegWrites,
                     Reg31IsStackPointer);
      return;
    case ADDVL_r_ri:  // Rd = Rn + (VL * scale)
      WriteXRegister(instr->GetRd(),
                     base + (GetVectorLengthInBytes() * scale),
                     LogRegWrites,
                     Reg31IsStackPointer);
      return;
  }

  VIXL_UNIMPLEMENTED();
}

void Simulator::VisitSVEVectorSelect(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEVectorSelectMask)) {
    case SEL_z_p_zz:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::VisitSVEWriteFFR(const Instruction* instr) {
  USE(instr);
  switch (instr->Mask(SVEWriteFFRMask)) {
    case SETFFR_f:
      VIXL_UNIMPLEMENTED();
      break;
    case WRFFR_f_p:
      VIXL_UNIMPLEMENTED();
      break;
    default:
      VIXL_UNIMPLEMENTED();
      break;
  }
}

void Simulator::DoUnreachable(const Instruction* instr) {
  VIXL_ASSERT((instr->Mask(ExceptionMask) == HLT) &&
              (instr->GetImmException() == kUnreachableOpcode));

  fprintf(stream_,
          "Hit UNREACHABLE marker at pc=%p.\n",
          reinterpret_cast<const void*>(instr));
  abort();
}


void Simulator::DoTrace(const Instruction* instr) {
  VIXL_ASSERT((instr->Mask(ExceptionMask) == HLT) &&
              (instr->GetImmException() == kTraceOpcode));

  // Read the arguments encoded inline in the instruction stream.
  uint32_t parameters;
  uint32_t command;

  VIXL_STATIC_ASSERT(sizeof(*instr) == 1);
  memcpy(&parameters, instr + kTraceParamsOffset, sizeof(parameters));
  memcpy(&command, instr + kTraceCommandOffset, sizeof(command));

  switch (command) {
    case TRACE_ENABLE:
      SetTraceParameters(GetTraceParameters() | parameters);
      break;
    case TRACE_DISABLE:
      SetTraceParameters(GetTraceParameters() & ~parameters);
      break;
    default:
      VIXL_UNREACHABLE();
  }

  WritePc(instr->GetInstructionAtOffset(kTraceLength));
}


void Simulator::DoLog(const Instruction* instr) {
  VIXL_ASSERT((instr->Mask(ExceptionMask) == HLT) &&
              (instr->GetImmException() == kLogOpcode));

  // Read the arguments encoded inline in the instruction stream.
  uint32_t parameters;

  VIXL_STATIC_ASSERT(sizeof(*instr) == 1);
  memcpy(&parameters, instr + kTraceParamsOffset, sizeof(parameters));

  // We don't support a one-shot LOG_DISASM.
  VIXL_ASSERT((parameters & LOG_DISASM) == 0);
  // Print the requested information.
  if (parameters & LOG_SYSREGS) PrintSystemRegisters();
  if (parameters & LOG_REGS) PrintRegisters();
  if (parameters & LOG_VREGS) PrintVRegisters();

  WritePc(instr->GetInstructionAtOffset(kLogLength));
}


void Simulator::DoPrintf(const Instruction* instr) {
  VIXL_ASSERT((instr->Mask(ExceptionMask) == HLT) &&
              (instr->GetImmException() == kPrintfOpcode));

  // Read the arguments encoded inline in the instruction stream.
  uint32_t arg_count;
  uint32_t arg_pattern_list;
  VIXL_STATIC_ASSERT(sizeof(*instr) == 1);
  memcpy(&arg_count, instr + kPrintfArgCountOffset, sizeof(arg_count));
  memcpy(&arg_pattern_list,
         instr + kPrintfArgPatternListOffset,
         sizeof(arg_pattern_list));

  VIXL_ASSERT(arg_count <= kPrintfMaxArgCount);
  VIXL_ASSERT((arg_pattern_list >> (kPrintfArgPatternBits * arg_count)) == 0);

  // We need to call the host printf function with a set of arguments defined by
  // arg_pattern_list. Because we don't know the types and sizes of the
  // arguments, this is very difficult to do in a robust and portable way. To
  // work around the problem, we pick apart the format string, and print one
  // format placeholder at a time.

  // Allocate space for the format string. We take a copy, so we can modify it.
  // Leave enough space for one extra character per expected argument (plus the
  // '\0' termination).
  const char* format_base = ReadRegister<const char*>(0);
  VIXL_ASSERT(format_base != NULL);
  size_t length = strlen(format_base) + 1;
  char* const format = new char[length + arg_count];

  // A list of chunks, each with exactly one format placeholder.
  const char* chunks[kPrintfMaxArgCount];

  // Copy the format string and search for format placeholders.
  uint32_t placeholder_count = 0;
  char* format_scratch = format;
  for (size_t i = 0; i < length; i++) {
    if (format_base[i] != '%') {
      *format_scratch++ = format_base[i];
    } else {
      if (format_base[i + 1] == '%') {
        // Ignore explicit "%%" sequences.
        *format_scratch++ = format_base[i];
        i++;
        // Chunks after the first are passed as format strings to printf, so we
        // need to escape '%' characters in those chunks.
        if (placeholder_count > 0) *format_scratch++ = format_base[i];
      } else {
        VIXL_CHECK(placeholder_count < arg_count);
        // Insert '\0' before placeholders, and store their locations.
        *format_scratch++ = '\0';
        chunks[placeholder_count++] = format_scratch;
        *format_scratch++ = format_base[i];
      }
    }
  }
  VIXL_CHECK(placeholder_count == arg_count);

  // Finally, call printf with each chunk, passing the appropriate register
  // argument. Normally, printf returns the number of bytes transmitted, so we
  // can emulate a single printf call by adding the result from each chunk. If
  // any call returns a negative (error) value, though, just return that value.

  printf("%s", clr_printf);

  // Because '\0' is inserted before each placeholder, the first string in
  // 'format' contains no format placeholders and should be printed literally.
  int result = printf("%s", format);
  int pcs_r = 1;  // Start at x1. x0 holds the format string.
  int pcs_f = 0;  // Start at d0.
  if (result >= 0) {
    for (uint32_t i = 0; i < placeholder_count; i++) {
      int part_result = -1;

      uint32_t arg_pattern = arg_pattern_list >> (i * kPrintfArgPatternBits);
      arg_pattern &= (1 << kPrintfArgPatternBits) - 1;
      switch (arg_pattern) {
        case kPrintfArgW:
          part_result = printf(chunks[i], ReadWRegister(pcs_r++));
          break;
        case kPrintfArgX:
          part_result = printf(chunks[i], ReadXRegister(pcs_r++));
          break;
        case kPrintfArgD:
          part_result = printf(chunks[i], ReadDRegister(pcs_f++));
          break;
        default:
          VIXL_UNREACHABLE();
      }

      if (part_result < 0) {
        // Handle error values.
        result = part_result;
        break;
      }

      result += part_result;
    }
  }

  printf("%s", clr_normal);

  // Printf returns its result in x0 (just like the C library's printf).
  WriteXRegister(0, result);

  // The printf parameters are inlined in the code, so skip them.
  WritePc(instr->GetInstructionAtOffset(kPrintfLength));

  // Set LR as if we'd just called a native printf function.
  WriteLr(ReadPc());

  delete[] format;
}


#ifdef VIXL_HAS_SIMULATED_RUNTIME_CALL_SUPPORT
void Simulator::DoRuntimeCall(const Instruction* instr) {
  VIXL_STATIC_ASSERT(kRuntimeCallAddressSize == sizeof(uintptr_t));
  // The appropriate `Simulator::SimulateRuntimeCall()` wrapper and the function
  // to call are passed inlined in the assembly.
  uintptr_t call_wrapper_address =
      Memory::Read<uintptr_t>(instr + kRuntimeCallWrapperOffset);
  uintptr_t function_address =
      Memory::Read<uintptr_t>(instr + kRuntimeCallFunctionOffset);
  RuntimeCallType call_type = static_cast<RuntimeCallType>(
      Memory::Read<uint32_t>(instr + kRuntimeCallTypeOffset));
  auto runtime_call_wrapper =
      reinterpret_cast<void (*)(Simulator*, uintptr_t)>(call_wrapper_address);

  if (call_type == kCallRuntime) {
    WriteRegister(kLinkRegCode,
                  instr->GetInstructionAtOffset(kRuntimeCallLength));
  }
  runtime_call_wrapper(this, function_address);
  // Read the return address from `lr` and write it into `pc`.
  WritePc(ReadRegister<Instruction*>(kLinkRegCode));
}
#else
void Simulator::DoRuntimeCall(const Instruction* instr) {
  USE(instr);
  VIXL_UNREACHABLE();
}
#endif


void Simulator::DoConfigureCPUFeatures(const Instruction* instr) {
  VIXL_ASSERT(instr->Mask(ExceptionMask) == HLT);

  typedef ConfigureCPUFeaturesElementType ElementType;
  VIXL_ASSERT(CPUFeatures::kNumberOfFeatures <
              std::numeric_limits<ElementType>::max());

  // k{Set,Enable,Disable}CPUFeatures have the same parameter encoding.

  size_t element_size = sizeof(ElementType);
  size_t offset = kConfigureCPUFeaturesListOffset;

  // Read the kNone-terminated list of features.
  CPUFeatures parameters;
  while (true) {
    ElementType feature = Memory::Read<ElementType>(instr + offset);
    offset += element_size;
    if (feature == static_cast<ElementType>(CPUFeatures::kNone)) break;
    parameters.Combine(static_cast<CPUFeatures::Feature>(feature));
  }

  switch (instr->GetImmException()) {
    case kSetCPUFeaturesOpcode:
      SetCPUFeatures(parameters);
      break;
    case kEnableCPUFeaturesOpcode:
      GetCPUFeatures()->Combine(parameters);
      break;
    case kDisableCPUFeaturesOpcode:
      GetCPUFeatures()->Remove(parameters);
      break;
    default:
      VIXL_UNREACHABLE();
      break;
  }

  WritePc(instr->GetInstructionAtOffset(AlignUp(offset, kInstructionSize)));
}


void Simulator::DoSaveCPUFeatures(const Instruction* instr) {
  VIXL_ASSERT((instr->Mask(ExceptionMask) == HLT) &&
              (instr->GetImmException() == kSaveCPUFeaturesOpcode));
  USE(instr);

  saved_cpu_features_.push_back(*GetCPUFeatures());
}


void Simulator::DoRestoreCPUFeatures(const Instruction* instr) {
  VIXL_ASSERT((instr->Mask(ExceptionMask) == HLT) &&
              (instr->GetImmException() == kRestoreCPUFeaturesOpcode));
  USE(instr);

  SetCPUFeatures(saved_cpu_features_.back());
  saved_cpu_features_.pop_back();
}


}  // namespace aarch64
}  // namespace vixl

#endif  // VIXL_INCLUDE_SIMULATOR_AARCH64

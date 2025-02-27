#include <codegen/codegen_forward.h>
#include <codegen/generic_object.h>
#include <codegen/machine_ir.h>
#include <codegen/x86_64/arch_x86_64_common.h>
#include <codegen/x86_64/arch_x86_64_isel.h>
#include <codegen/x86_64/arch_x86_64_tgt_generic_object.h>
#include <ir/ir.h>
#include <module.h>
#include <utils.h>
#include <vector.h>

// NOTE: +rw indicates the lower three bits of the opcode byte are used
// to indicate the 16-bit register operand.
// For registers R8 through R15, the REX.b bit also needs set.
static uint8_t rw_encoding(RegisterDescriptor reg) {
  switch (reg) {
  case REG_RAX:
  case REG_R8:
    return 0;
  case REG_RCX:
  case REG_R9:
    return 1;
  case REG_RDX:
  case REG_R10:
    return 2;
  case REG_RBX:
  case REG_R11:
    return 3;
  case REG_RSP:
  case REG_R12:
    return 4;
  case REG_RBP:
  case REG_R13:
    return 5;
  case REG_RSI:
  case REG_R14:
    return 6;
  case REG_RDI:
  case REG_R15:
    return 7;
  }
  UNREACHABLE();
}

// NOTE: +rw indicates the lower three bits of the opcode byte are used
// to indicate the 32 or 64-bit register operand.
// For registers R8 through R15, the REX.b bit also needs set.
// EAX: 0
// ECX: 1
// EDX: 2
// EBX: 3
// ESP: 4
// EBP: 5
// ESI: 6
// EDI: 7
// R8D: REX.B, 0
// R9D: REX.B, 1
// R10D: REX.B, 2
// R11D: REX.B, 3
// R12D: REX.B, 4
// R13D: REX.B, 5
// R14D: REX.B, 6
// R15D: REX.B, 7
static uint8_t rd_encoding(RegisterDescriptor reg) {
  return rw_encoding(reg);
}

// NOTE: +rw indicates the lower three bits of the opcode byte are used
// to indicate the 32 or 64-bit register operand.
// For registers R8 through R15, the REX.b bit also needs set.
// AL: 0
// CL: 1
// DL: 2
// BL: 3
// SPL: REX.b, 4
// BPL: REX.b, 5
// SIL: REX.b, 6
// DIL: REX.b, 7
// R8B: REX.b, 0
// R9B: REX.b, 1
// R10B: REX.b, 2
// R11B: REX.b, 3
// R12B: REX.b, 4
// R13B: REX.b, 5
// R14B: REX.b, 6
// R15B: REX.b, 7
static uint8_t rb_encoding(RegisterDescriptor reg) {
  return rw_encoding(reg);
}

// Don't use me directly!
static uint8_t rex_byte(bool w, bool r, bool x, bool b) {
  return (uint8_t)(0b01000000 | ((uint8_t)w << 3) | ((uint8_t)r << 2) | ((uint8_t)x << 1) | (uint8_t)b);
}
/// REX.W prefix is commonly used to promote a 32-bit operation to 64-bit.
static uint8_t rexw_byte() {
  return rex_byte(true, false, false, false);
}

uint8_t regbits(RegisterDescriptor reg) {
  switch (reg) {
  case REG_RAX: return 0b0000;
  case REG_RCX: return 0b0001;
  case REG_RDX: return 0b0010;
  case REG_RBX: return 0b0011;
  case REG_RSP: return 0b0100;
  case REG_RBP: return 0b0101;
  case REG_RSI: return 0b0110;
  case REG_RDI: return 0b0111;
  case REG_R8:  return 0b1000;
  case REG_R9:  return 0b1001;
  case REG_R10: return 0b1010;
  case REG_R11: return 0b1011;
  case REG_R12: return 0b1100;
  case REG_R13: return 0b1101;
  case REG_R14: return 0b1110;
  case REG_R15: return 0b1111;
  default: ICE("Unhandled register in regbits: %s\n", register_name(reg));
  }
}

/// Suitable for use in an if condition to test if the top bit is set
/// in the Intel encoding of registers.
#define REGBITS_TOP(regbits) (regbits & 0b1000)

bool regbits_top(RegisterDescriptor reg) {
  return REGBITS_TOP(regbits(reg));
}

static uint8_t modrm_byte(uint8_t mod, uint8_t reg, uint8_t rm) {
  // Ensure no bits above the amount expected are set.
  ASSERT((mod & (~0b11)) == 0);
  // Top bit of register stored in REX bit(s), but may still be present here.
  ASSERT((reg & (~0b1111)) == 0);
  ASSERT((rm & (~0b1111)) == 0);
  return (uint8_t)((mod << 6) | ((reg & 0b111) << 3) | (rm & 0b111));
}

/// A SIB byte is needed in the following cases:
///   a. If a memory operand has two pointer or index registers,
///   b. If a memory operand has a scaled index register,
///   c. If a memory operand has the stack pointer (ESP or RSP) as base,
///   d. If a memory operand in 64-bit mode uses a 32-bit sign-extended
///      direct memory address rather than a RIP-relative address.
/// A SIB byte cannot be used in 16-bit addressing mode.
static uint8_t sib_byte(uint8_t scale_factor, uint8_t index, uint8_t base) {
  // Ensure no bits above the amount expected are set.
  ASSERT((scale_factor & (~0b11)) == 0);
  ASSERT((index & (~0b1111)) == 0);
  ASSERT((base & (~0b1111)) == 0);
  return (uint8_t)((scale_factor << 6) | ((index & 0b111) << 3) | (base & 0b111));
}

// TODO/FIXME: There are lots of issues regarding Chapter 2, Volume 2,
// Table 2-5 "Special Cases of REX Encodings" of the Intel SDM.
// - SIB byte also required for R12-based addressing.
//   I *think*, based on other things in the table, that this only
//   applies when mod != 0b11.
// - Using RBP or R13 without displacement must be done using mod = 01
//   with a displacement of 0.
// - SIB Byte base = 0101(EBP)
//   Base register is unused if mod = 0.
//   This requires explicit displacement to be used with EBP/RBP or
//   R13.
//   NOTE: I have no clue what this one means.

/// Should be used after every modrm byte with a mod not equal to 0b11
/// is written that may contain r12 in the r/m field.
/// Implicitly captures `address_register`, `context`, and `modrm`.
#define MCODE_SIB_IF_R12 do {                                           \
    if (address_register == REG_R12 && (modrm & 0b11000000) != 0b11) {  \
      uint8_t sib = sib_byte(0b00, 0b100, 0b100);                       \
      mcode_1(context->object, sib);                                    \
    }                                                                   \
  } while (0)

/// NOTE: Caller must first zero out the destination register unless `size` is r32 or r64.
static void mcode_imm_to_reg(CodegenContext *context, MIROpcodex86_64 inst, int64_t immediate, RegisterDescriptor destination_register, enum RegSize size) {
  if ((inst == MX64_SUB || inst == MX64_ADD) && immediate == 0) return;

  switch (inst) {
  case MX64_IMUL: {
    if (size == r8) {
      print("%35WARNING:%m IMUL of eight-byte register doesn't exist!\n");
      size = r16;
    }
    uint8_t destination_regbits = regbits(destination_register);
    switch (size) {
    default: ICE("Unhandled register size!");
    case r8: UNREACHABLE();
    case r16: {
      // 0x66 + 0x69 /r iw
      uint8_t modrm = modrm_byte(0b11, destination_regbits, destination_regbits);
      int16_t imm16 = (int16_t)immediate;
      if (REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(destination_regbits), false, REGBITS_TOP(destination_regbits));
        mcode_1(context->object, rex);
      }
      mcode_3(context->object, 0x66, 0x69, modrm);
      mcode_n(context->object, &imm16, 2);
    } break;
    case r32: {
      // 0x69 /r id
      uint8_t modrm = modrm_byte(0b11, destination_regbits, destination_regbits);
      int32_t imm32 = (int32_t)immediate;
      if (REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(destination_regbits), false, REGBITS_TOP(destination_regbits));
        mcode_1(context->object, rex);
      }
      mcode_2(context->object, 0x69, modrm);
      mcode_n(context->object, &imm32, 4);
    } break;
    case r64: {
      // REX.W + 0x69 /r id
      uint8_t modrm = modrm_byte(0b11, destination_regbits, destination_regbits);
      int32_t imm32 = (int32_t)immediate;
      uint8_t rex = rex_byte(true, REGBITS_TOP(destination_regbits), false, REGBITS_TOP(destination_regbits));
      mcode_3(context->object, rex, 0x69, modrm);
      mcode_n(context->object, &imm32, 4);
    } break;
    } // switch (size)
  } break;

  case MX64_MOV: {

    if (size == r64 && immediate > INT32_MIN && immediate < INT32_MAX)
      size = r32;

    switch (size) {
    default: ICE("Unhandled register size!");
    case r8: {
      // Move imm8 to r8
      // 0xb0+ rb ib

      // Encode a REX prefix if the ModRM register descriptor needs
      // the bit extension.
      uint8_t destination_regbits = regbits(destination_register);
      if (REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, false, false, REGBITS_TOP(destination_regbits));
        mcode_1(context->object, rex);
      }

      uint8_t op = 0xb0 + rb_encoding(destination_register);
      int8_t imm8 = (int8_t)immediate;
      mcode_2(context->object, op, (uint8_t)imm8);
    } break;
    case r16: {
      // Move imm16 to r16
      // 0x66 + 0xb8+ rw iw
      // Encode a REX prefix if the ModRM register descriptor needs
      // the bit extension.
      uint8_t destination_regbits = regbits(destination_register);
      if (REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, false, false, REGBITS_TOP(destination_regbits));
        mcode_1(context->object, rex);
      }
      uint8_t op = 0xb8 + rw_encoding(destination_register);
      int16_t imm16 = (int16_t)immediate;
      mcode_2(context->object, 0x66, op);
      mcode_n(context->object, &imm16, 2);
    } break;
    case r32: {
      // Move imm32 to r32
      // 0xb8+ rd id
      // Encode a REX prefix if the ModRM register descriptor needs
      // the bit extension.
      uint8_t destination_regbits = regbits(destination_register);
      if (REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, false, false, REGBITS_TOP(destination_regbits));
        mcode_1(context->object, rex);
      }
      uint8_t op = 0xb8 + rd_encoding(destination_register);
      int32_t imm32 = (int32_t)immediate;
      mcode_1(context->object, op);
      mcode_n(context->object, &imm32, 4);
    } break;
    case r64: {
      // Move imm64 to r64
      // REX.W + 0xb8+ rd io
      uint8_t rex = rex_byte(true, false, false, REGBITS_TOP(regbits(destination_register)));
      uint8_t op = 0xb8 + rd_encoding(destination_register);
      mcode_2(context->object, rex, op);
      mcode_n(context->object, &immediate, 8);
    } break;

    } // switch (size)

  } break; // case MX64_MOV

  case MX64_AND: FALLTHROUGH;
  case MX64_OR: FALLTHROUGH;
  case MX64_ADD: FALLTHROUGH;
  case MX64_CMP: FALLTHROUGH;
  case MX64_SUB: {

    // Immediate add/sub/and all share the same opcodes, just with a different opcode extension in ModRM:reg.
    const uint8_t add_extension = 0;
    const uint8_t or_extension  = 1;
    const uint8_t and_extension = 4;
    const uint8_t sub_extension = 5;
    const uint8_t cmp_extension = 7;
    uint8_t extension = cmp_extension;
    if (inst == MX64_SUB) extension = sub_extension;
    else if (inst == MX64_AND) extension = and_extension;
    else if (inst == MX64_OR) extension = or_extension;
    else if (inst == MX64_ADD) extension = add_extension;

    // Mod == 0b11  ->  register
    // Reg == Opcode Extension (7 for cmp, 5 for sub, 4 for and, 1 for or, 0 for add)
    // R/M == Destination
    uint8_t destination_regbits = regbits(destination_register);
    uint8_t modrm = modrm_byte(0b11, extension, destination_regbits);

    switch (size) {
    default: ICE("Unhandled register size!");
    case r8: {
      // 0x80 /5 ib
      int8_t imm8 = (int8_t)immediate;

      // Encode a REX prefix if the ModRM register descriptor needs
      // the bit extension.
      if (REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, false, false, REGBITS_TOP(destination_regbits));
        mcode_1(context->object, rex);
      }
      mcode_3(context->object, 0x80, modrm, (uint8_t)imm8);
    } break;
    case r16: {
      // 0x66 + 0x81 /5 iw
      mcode_1(context->object, 0x66);
    } FALLTHROUGH;
    case r32: {
      if (immediate >= INT8_MIN && immediate <= INT8_MAX) {
        // 0x83 /5 ib
        uint8_t op = 0x83;

        // Encode a REX prefix if the ModRM register descriptor needs
        // the bit extension.
        if (REGBITS_TOP(destination_regbits)) {
          uint8_t rex = rex_byte(false, false, false, REGBITS_TOP(destination_regbits));
          mcode_1(context->object, rex);
        }

        int8_t imm8 = (int8_t)immediate;
        mcode_3(context->object, op, modrm, (uint8_t)imm8);

      } else {
        // 0x81 /5 id
        uint8_t op = 0x81;

        // Encode a REX prefix if the ModRM register descriptor needs
        // the bit extension.
        if (REGBITS_TOP(destination_regbits)) {
          uint8_t rex = rex_byte(false, false, false, REGBITS_TOP(destination_regbits));
          mcode_1(context->object, rex);
        }

        mcode_2(context->object, op, modrm);
        if (size == r16) {
          int16_t imm16 = (int16_t)immediate;
          mcode_n(context->object, &imm16, 2);
        } else {
          int32_t imm32 = (int32_t)immediate;
          mcode_n(context->object, &imm32, 4);
        }
      }
    } break;

    case r64: {
      if (immediate >= INT8_MIN && immediate <= INT8_MAX) {
        // Subtract imm8 sign extended to 64-bits from r64
        // REX.W + 0x83 /5 ib
        uint8_t op = 0x83;
        uint8_t rex = rex_byte(true, false, false, REGBITS_TOP(destination_regbits));
        int8_t imm8 = (int8_t)immediate;
        mcode_4(context->object, rex, op, modrm, (uint8_t)imm8);
      } else {
        // Subtract imm32 sign extended to 64-bits from r64
        // REX.W + 0x81 /5 id
        uint8_t op = 0x81;
        uint8_t rex = rex_byte(true, false, false, REGBITS_TOP(destination_regbits));
        int32_t imm32 = (int32_t)immediate;

        mcode_3(context->object, rex, op, modrm);
        mcode_n(context->object, &imm32, 4);
      }
    } break;

    } // switch (size)

  } break; // case MX64_ADD/MX64_SUB

  default: ICE("ERROR: mcode_imm_to_reg(): Unsupported instruction %d (%s)", inst, mir_x86_64_opcode_mnemonic(inst));
  }
}

static void mcode_imm_to_mem(CodegenContext *context, MIROpcodex86_64 inst, int64_t immediate, RegisterDescriptor address_register, int64_t offset, RegSize size) {
  switch (inst) {

  case MX64_MOV: {

    switch (size) {
    default: ICE("Unhandled register size");
    case r8: {
      // 0xc6 /0 ib
      uint8_t address_regbits = regbits(address_register);
      int8_t imm8 = (int8_t)immediate;

      // Encode a REX prefix if the ModRM register descriptor needs
      // the bit extension.
      if (REGBITS_TOP(address_regbits)) {
        uint8_t rex = rex_byte(false, false, false, REGBITS_TOP(address_regbits));
        mcode_1(context->object, rex);
      }

      // Make output code smaller when possible by omitting zero displacements.
      if (offset == 0) {
        // Mod == 0b00  ->  (R/M)
        // Reg == Opcode Extension
        // R/M == Address
        uint8_t modrm = modrm_byte(0b00, 0, address_regbits);
        mcode_2(context->object, 0xc6, modrm);
        mcode_1(context->object, (uint8_t)imm8);
        break;
      }

      // Mod == 0b10  ->  (R/M+disp32)
      // Reg == Opcode Extension
      // R/M == Address
      uint8_t modrm = modrm_byte(0b10, 0, address_regbits);
      int32_t disp32 = (int32_t)offset;

      mcode_2(context->object, 0xc6, modrm);

      if (address_register == REG_RSP) {
        /// Scaling Factor == 0b00  ->  1
        /// Index == 0b100  ->  None
        /// Base == RSP bits (0b100)
        mcode_1(context->object, sib_byte(0b00, 0b100, address_regbits));
      }
      mcode_n(context->object, &disp32, 4);
      mcode_1(context->object, (uint8_t)imm8);
    } break;
    case r16: {
      // 0x66 + 0xc7 /0 iw
      mcode_1(context->object, 0x66);
    } FALLTHROUGH;
    case r32: {
      // 0xc7 /0 id
      uint8_t address_regbits = regbits(address_register);

      // Encode a REX prefix if the ModRM register descriptor needs
      // the bit extension.
      if (REGBITS_TOP(address_regbits)) {
        uint8_t rex = rex_byte(false, false, false, REGBITS_TOP(address_regbits));
        mcode_1(context->object, rex);
      }

      if (offset == 0) {
        // Mod == 0b00  ->  (R/M)
        // Reg == Opcode Extension
        // R/M == Address
        uint8_t modrm = modrm_byte(0b00, 0, address_regbits);
        mcode_2(context->object, 0xc7, modrm);
        if (size == r16) {
          int16_t imm16 = (int16_t)immediate;
          mcode_n(context->object, &imm16, 2);
        } else {
          int32_t imm32 = (int32_t)immediate;
          mcode_n(context->object, &imm32, 4);
        }
        break;
      }

      // Mod == 0b10  ->  (R/M+disp32)
      // Reg == Opcode Extension
      // R/M == Address
      uint8_t modrm = modrm_byte(0b10, 0, address_regbits);
      int32_t disp32 = (int32_t)offset;

      mcode_2(context->object, 0xc7, modrm);
      if (address_register == REG_RSP) {
        /// Scaling Factor == 0b00  ->  1
        /// Index == 0b100  ->  None
        /// Base == RSP bits (0b100)
        mcode_1(context->object, sib_byte(0b00, 0b100, address_regbits));
      }
      mcode_n(context->object, &disp32, 4);
      if (size == r16) {
        int16_t imm16 = (int16_t)immediate;
        mcode_n(context->object, &imm16, 2);
      } else {
        int32_t imm32 = (int32_t)immediate;
        mcode_n(context->object, &imm32, 4);
      }

    } break;
    case r64: {
      // REX.W + 0xc7 /0 id
      uint8_t address_regbits = regbits(address_register);
      uint8_t rex = rex_byte(true, false, false, REGBITS_TOP(address_regbits));
      int32_t imm32 = (int32_t)immediate;

      // Make output code smaller when possible by omitting zero displacements.
      if (offset == 0) {
        // Mod == 0b00  ->  R/M
        // Reg == Opcode Extension
        // R/M == Address
        uint8_t modrm = modrm_byte(0b00, 0, address_regbits);
        mcode_3(context->object, rex, 0xc7, modrm);
        mcode_n(context->object, &imm32, 4);
        break;
      }

      // Mod == 0b10  ->  (R/M+disp32)
      // Reg == Opcode Extension
      // R/M == Address
      uint8_t modrm = modrm_byte(0b10, 0, address_regbits);
      int32_t disp32 = (int32_t)offset;

      mcode_3(context->object, rex, 0xc7, modrm);
      if (address_register == REG_RSP) {
        /// Scaling Factor == 0b00  ->  1
        /// Index == 0b100  ->  None
        /// Base == RSP bits (0b100)
        mcode_1(context->object, sib_byte(0b00, 0b100, address_regbits));
      }
      mcode_n(context->object, &disp32, 4);
      mcode_n(context->object, &imm32, 4);
    } break;

    } // switch (size)
  } break; // case MX64_MOV

  case MX64_SUB: {
    // REX.W 0x81 /5 id
    ASSERT(size == r64, "Unhandled size");
    if (offset == 0) {
      uint8_t address_regbits = regbits(address_register);
      uint8_t rex = rex_byte(true, false, false, REGBITS_TOP(address_regbits));
      uint8_t modrm = modrm_byte(0b00, 5, address_regbits);
      int32_t imm32 = (int32_t)immediate;

      mcode_3(context->object, rex, 0x81, modrm);
      if (address_register == REG_RSP) {
        /// Scaling Factor == 0b00  ->  1
        /// Index == 0b100  ->  None
        /// Base == RSP bits (0b100)
        mcode_1(context->object, sib_byte(0b00, 0b100, address_regbits));
      }
      mcode_n(context->object, &imm32, 4);
      break;
    }

    uint8_t address_regbits = regbits(address_register);
    uint8_t rex = rex_byte(true, false, false, REGBITS_TOP(address_regbits));
    uint8_t modrm = modrm_byte(0b10, 5, address_regbits);
    int32_t imm32 = (int32_t)immediate;
    int32_t disp32 = (int32_t)offset;

    mcode_3(context->object, rex, 0x81, modrm);
    if (address_register == REG_RSP) {
      /// Scaling Factor == 0b00  ->  1
      /// Index == 0b100  ->  None
      /// Base == RSP bits (0b100)
      mcode_1(context->object, sib_byte(0b00, 0b100, address_regbits));
    }
    mcode_n(context->object, &disp32, 4);
    mcode_n(context->object, &imm32, 4);

  } break; // case MX64_SUB

  default: ICE("ERROR: mcode_imm_to_mem(): Unsupported instruction %d (%s)", inst, mir_x86_64_opcode_mnemonic(inst));
  }
}

static void mcode_imm_to_offset_name(CodegenContext *context, MIROpcodex86_64 inst, int64_t immediate, RegSize size, RegisterDescriptor address_register, const char *name, int64_t offset) {
  TODO("Implement instruction %d (%s) in `imm to offset name` form",
       inst, mir_x86_64_opcode_mnemonic(inst));
}

static void mcode_mem_to_reg(CodegenContext *context, MIROpcodex86_64 inst, RegisterDescriptor address_register, int64_t offset, RegisterDescriptor destination_register, enum RegSize size) {
  switch (inst) {

  case MX64_LEA: {

    switch (size) {
    default: ICE("Unhandled register size");
    case r8: ICE("x86_64 machine code backend: LEA does not have an 8-bit encoding.");
    case r16: {
      // 0x66 + 0x8d /r
      uint8_t sixteen_bit_prefix = 0x66;
      mcode_1(context->object, sixteen_bit_prefix);
    } FALLTHROUGH;
    case r32: {
      // 0x8d /r
      uint8_t op = 0x8d;

      // Encode a REX prefix if either of the ModRM register descriptors need
      // the bit extension.
      uint8_t address_regbits = regbits(address_register);
      uint8_t destination_regbits = regbits(destination_register);
      if (REGBITS_TOP(address_regbits) || REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(destination_regbits), false, REGBITS_TOP(address_regbits));
        mcode_1(context->object, rex);
      }

      // Mod == 0b10  ->  (R/M)+disp32
      // Reg == Destination
      // R/M == Address
      uint8_t modrm = modrm_byte(0b10, destination_regbits, address_regbits);

      mcode_2(context->object, op, modrm);
      int32_t disp32 = (int32_t)offset;
      mcode_n(context->object, &disp32, 4);

    } break;
    case r64: {
      // REX.W + 0x8d /r
      uint8_t op = 0x8d;

      uint8_t address_regbits = regbits(address_register);
      uint8_t destination_regbits = regbits(destination_register);
      uint8_t rex = rex_byte(true, REGBITS_TOP(destination_regbits), false, REGBITS_TOP(address_regbits));

      // Mod == 0b10  ->  (R/M)+disp32
      // Reg == Destination
      // R/M == Address
      uint8_t modrm = modrm_byte(0b10, destination_regbits, address_regbits);

      mcode_3(context->object, rex, op, modrm);
      int32_t disp32 = (int32_t)offset;
      mcode_n(context->object, &disp32, 4);

    } break;
    } // switch (size)

  } break; // case MX64_LEA

  case MX64_MOV: {

    uint8_t address_regbits = regbits(address_register);
    uint8_t destination_regbits = regbits(destination_register);

    // Each of these branches *must* assign modrm.

    // "Using RBP or R13 without displacement must be done using mod = 01 with a displacement of 0."
    //     ~ Intel Software Developer's Manual, p. 517, Vol. 2A, Ch. 2, Table 2-5, "Special Cases of REX Encodings"
    if (offset == 0 && (address_register != REG_RBP && address_register != REG_R13)) {
      // Mod == 0b00  (register)
      // Reg == Destination
      // R/M == Address
      uint8_t modrm = modrm_byte(0b00, destination_regbits, address_regbits);

      switch (size) {
      default: ICE("Unhandled register size");
      case r8: {
        // 0x8a /r
        if (REGBITS_TOP(address_regbits) || REGBITS_TOP(destination_regbits)) {
          uint8_t rex = rex_byte(false, REGBITS_TOP(destination_regbits), false, REGBITS_TOP(address_regbits));
          mcode_1(context->object, rex);
        }
        mcode_2(context->object, 0x8a, modrm);
        MCODE_SIB_IF_R12;
      } break;

      case r16: {
        // 0x66 + 0x8b /r
        mcode_1(context->object, 0x66);
      } FALLTHROUGH;
      case r32: {
        // 0x8b /r
        if (REGBITS_TOP(address_regbits) || REGBITS_TOP(destination_regbits)) {
          uint8_t rex = rex_byte(false, REGBITS_TOP(destination_regbits), false, REGBITS_TOP(address_regbits));
          mcode_1(context->object, rex);
        }
        mcode_2(context->object, 0x8b, modrm);
        MCODE_SIB_IF_R12;
      } break;

      case r64: {
        // REX.W + 0x8b /r
        uint8_t rex = rex_byte(true, REGBITS_TOP(destination_regbits), false, REGBITS_TOP(address_regbits));
        mcode_3(context->object, rex, 0x8b, modrm);
        MCODE_SIB_IF_R12;
      } break;

      } // switch (size)

    } else if (offset >= -128 && offset <= 127) {
      // Mod == 0b01  (register + disp8)
      // Reg == Destination
      // R/M == Address
      uint8_t modrm = modrm_byte(0b01, destination_regbits, address_regbits);
      int8_t disp8 = (int8_t)offset;

      switch (size) {
      default: ICE("Unhandled register size");
      case r8: {
        // 0x8a /r
        if (REGBITS_TOP(address_regbits) || REGBITS_TOP(destination_regbits)) {
          uint8_t rex = rex_byte(false, REGBITS_TOP(destination_regbits), false, REGBITS_TOP(address_regbits));
          mcode_1(context->object, rex);
        }
        mcode_3(context->object, 0x8a, modrm, (uint8_t)disp8);
        MCODE_SIB_IF_R12;
      } break;

      case r16: {
        // 0x66 + 0x8b /r
        mcode_1(context->object, 0x66);
      } FALLTHROUGH;
      case r32: {
        // 0x8b /r
        if (REGBITS_TOP(address_regbits) || REGBITS_TOP(destination_regbits)) {
          uint8_t rex = rex_byte(false, REGBITS_TOP(destination_regbits), false, REGBITS_TOP(address_regbits));
          mcode_1(context->object, rex);
        }
        mcode_3(context->object, 0x8b, modrm, (uint8_t)disp8);
        MCODE_SIB_IF_R12;
      } break;

      case r64: {
        // REX.W + 0x8b /r
        uint8_t rex = rex_byte(true, REGBITS_TOP(destination_regbits), false, REGBITS_TOP(address_regbits));
        mcode_4(context->object, rex, 0x8b, modrm, (uint8_t)disp8);
        MCODE_SIB_IF_R12;
      } break;

      } // switch (size)

    } else {
      // Mod == 0b10  (register + disp32)
      // Reg == Destination
      // R/M == Address
      uint8_t modrm = modrm_byte(0b10, destination_regbits, address_regbits);
      int32_t disp32 = (int32_t)offset;

      switch (size) {
      default: ICE("Unhandled register size");
      case r8: {
        // 0x8a /r
        if (REGBITS_TOP(address_regbits) || REGBITS_TOP(destination_regbits)) {
          uint8_t rex = rex_byte(false, REGBITS_TOP(destination_regbits), false, REGBITS_TOP(address_regbits));
          mcode_1(context->object, rex);
        }
        mcode_2(context->object, 0x8a, modrm);
        MCODE_SIB_IF_R12;
        mcode_n(context->object, &disp32, 4);
      } break;
      case r16: {
        // 0x66 + 0x8b /r
        uint8_t sixteen_bit_prefix = 0x66;
        mcode_1(context->object, sixteen_bit_prefix);
      } FALLTHROUGH;
      case r32: {
        // 0x8b /r
        if (REGBITS_TOP(address_regbits) || REGBITS_TOP(destination_regbits)) {
          uint8_t rex = rex_byte(false, REGBITS_TOP(destination_regbits), false, REGBITS_TOP(address_regbits));
          mcode_1(context->object, rex);
        }
        mcode_2(context->object, 0x8b, modrm);
        MCODE_SIB_IF_R12;
        mcode_n(context->object, &disp32, 4);
      } break;
      case r64: {
        // REX.W + 0x8b /r
        uint8_t rex = rex_byte(true, REGBITS_TOP(destination_regbits), false, REGBITS_TOP(address_regbits));
        mcode_3(context->object, rex, 0x8b, modrm);
        MCODE_SIB_IF_R12;
        mcode_n(context->object, &disp32, 4);
      } break;

      } // switch (size)
    }

  } break; // case MX64_MOV

  default: ICE("ERROR: mcode_mem_to_reg(): Unsupported instruction %d (%s)", inst, mir_x86_64_opcode_mnemonic(inst));
  }
}

/// Write x86_64 machine code for instruction `inst` with `name` offset
/// from `address_register` and store the result in register
/// `destination_register` with size `size`.
/// NOTE: Caller must first zero out the destination register unless `size` is r32 or r64.
static void mcode_name_to_reg(CodegenContext *context, MIROpcodex86_64 inst, RegisterDescriptor address_register, const char *name, RegisterDescriptor destination_register, enum RegSize size) {
  switch (inst) {

  case MX64_LEA: {

    switch (size) {
    default: ICE("Unhandled register size");
    case r8: ICE("x86_64 machine code backend: LEA does not have an 8-bit encoding.");
    case r16: {
      // 0x66 + 0x8d /r
      uint8_t sixteen_bit_prefix = 0x66;
      mcode_1(context->object, sixteen_bit_prefix);
    } FALLTHROUGH;
    case r32: {
      // 0x8d /r
      uint8_t op = 0x8d;

      // RIP-Relative Addressing
      if (address_register == REG_RIP) {

        uint8_t destination_regbits = regbits(destination_register);
        if (REGBITS_TOP(destination_regbits)) {
          uint8_t rex = rex_byte(false, REGBITS_TOP(destination_regbits), false, false);
          mcode_1(context->object, rex);
        }

        // Mod == 0b00
        // R/M == 0b101 (none)
        uint8_t modrm = modrm_byte(0b00, destination_regbits, 0b101);

        mcode_2(context->object, op, modrm);

        // Make RIP-relative disp32 relocation
        RelocationEntry reloc = {0};
        Section *sec_code = code_section(context->object);
        ASSERT(sec_code, "NO CODE SECTION, WHAT HAVE YOU DONE?");
        reloc.sym.byte_offset = sec_code->data.bytes.size;
        reloc.sym.name = strdup(name);
        reloc.sym.section_name = strdup(sec_code->name);
        reloc.type = RELOC_DISP32_PCREL;
        vector_push(context->object->relocs, reloc);

        int32_t disp32 = 0;
        mcode_n(context->object, &disp32, 4);

        break;
      }

      // Encode a REX prefix if either of the ModRM register descriptors need
      // the bit extension.
      uint8_t address_regbits = regbits(address_register);
      uint8_t destination_regbits = regbits(destination_register);
      if (REGBITS_TOP(address_regbits) || REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(destination_regbits), false, REGBITS_TOP(address_regbits));
        mcode_1(context->object, rex);
      }

      // Mod == 0b10  ->  (R/M)+disp32
      // Reg == Destination
      // R/M == Address
      uint8_t modrm = modrm_byte(0b10, destination_regbits, address_regbits);

      mcode_2(context->object, op, modrm);
      MCODE_SIB_IF_R12;

      // Make disp32 relocation to lea from symbol
      RelocationEntry reloc = {0};
      Section *sec_code = code_section(context->object);
      reloc.sym.byte_offset = sec_code->data.bytes.size;
      reloc.sym.name = strdup(name);
      reloc.sym.section_name = strdup(sec_code->name);
      reloc.type = RELOC_DISP32;
      vector_push(context->object->relocs, reloc);

      int32_t disp32 = 0;
      mcode_n(context->object, &disp32, 4);
    } break;
    case r64: {
      // REX.W + 0x8d /r
      uint8_t op = 0x8d;

      // RIP-Relative Addressing
      if (address_register == REG_RIP) {
        uint8_t destination_regbits = regbits(destination_register);
        uint8_t rex = rex_byte(true, REGBITS_TOP(destination_regbits), false, false);
        // Mod == 0b00
        // R/M == 0b101 (none)
        uint8_t modrm = modrm_byte(0b00, destination_regbits, 0b101);
        int32_t disp32 = 0;

        mcode_3(context->object, rex, op, modrm);
        MCODE_SIB_IF_R12;

        // Make RIP-relative disp32 relocation
        RelocationEntry reloc = {0};
        Section *sec_code = code_section(context->object);
        ASSERT(sec_code, "NO CODE SECTION, WHAT HAVE YOU DONE?");
        reloc.sym.byte_offset = sec_code->data.bytes.size;
        reloc.sym.name = strdup(name);
        reloc.sym.section_name = strdup(sec_code->name);
        reloc.type = RELOC_DISP32_PCREL;
        vector_push(context->object->relocs, reloc);

        mcode_n(context->object, &disp32, 4);
        break;
      }

      uint8_t address_regbits = regbits(address_register);
      uint8_t destination_regbits = regbits(destination_register);
      uint8_t rex = rex_byte(true, REGBITS_TOP(destination_regbits), false, REGBITS_TOP(address_regbits));

      // Mod == 0b10  ->  (R/M)+disp32
      // Reg == Destination
      // R/M == Address
      uint8_t modrm = modrm_byte(0b10, destination_regbits, address_regbits);

      mcode_3(context->object, rex, op, modrm);
      MCODE_SIB_IF_R12;

      // Make disp32 relocation to lea from symbol
      RelocationEntry reloc = {0};
      Section *sec_code = code_section(context->object);
      reloc.sym.byte_offset = sec_code->data.bytes.size;
      reloc.sym.name = strdup(name);
      reloc.sym.section_name = strdup(sec_code->name);
      reloc.type = RELOC_DISP32;
      vector_push(context->object->relocs, reloc);

      int32_t disp32 = 0;
      mcode_n(context->object, &disp32, 4);
    } break;

    } // switch (size)

  } break; // case MX64_LEA

  case MX64_MOV: {

    switch (size) {
    default: ICE("Unhandled register size");

    case r8: {
      // 0x8a /r
      uint8_t op = 0x8a;

      // RIP-Relative Addressing
      if (address_register == REG_RIP) {
        uint8_t destination_regbits = regbits(destination_register);
        if (REGBITS_TOP(destination_regbits)) {
          uint8_t rex = rex_byte(false, REGBITS_TOP(destination_regbits), false, false);
          mcode_1(context->object, rex);
        }

        // Mod == 0b00
        // R/M == 0b101 (none)
        uint8_t modrm = modrm_byte(0b00, destination_regbits, 0b101);

        mcode_2(context->object, op, modrm);
        MCODE_SIB_IF_R12;

        // Make RIP-relative disp32 relocation
        RelocationEntry reloc = {0};
        Section *sec_code = code_section(context->object);
        ASSERT(sec_code, "NO CODE SECTION, WHAT HAVE YOU DONE?");
        reloc.sym.byte_offset = sec_code->data.bytes.size;
        reloc.sym.name = strdup(name);
        reloc.sym.section_name = strdup(sec_code->name);
        reloc.type = RELOC_DISP32_PCREL;
        vector_push(context->object->relocs, reloc);

        int32_t disp32 = 0;
        mcode_n(context->object, &disp32, 4);
        break;
      }

      // Encode a REX prefix if either of the ModRM register descriptors need
      // the bit extension.
      uint8_t address_regbits = regbits(address_register);
      uint8_t destination_regbits = regbits(destination_register);
      if (REGBITS_TOP(address_regbits) || REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(address_regbits), false, REGBITS_TOP(destination_regbits));
        mcode_1(context->object, rex);
      }

      // Mod == 0b10  ->  (R/M)+disp32
      // Reg == Destination
      // R/M == Address
      uint8_t modrm = modrm_byte(0b10, destination_regbits, address_regbits);

      mcode_2(context->object, op, modrm);
      MCODE_SIB_IF_R12;

      // Make disp32 relocation to lea from symbol
      RelocationEntry reloc = {0};
      Section *sec_code = code_section(context->object);
      reloc.sym.byte_offset = sec_code->data.bytes.size;
      reloc.sym.name = strdup(name);
      reloc.sym.section_name = strdup(sec_code->name);
      reloc.type = RELOC_DISP32;
      vector_push(context->object->relocs, reloc);

      int32_t disp32 = 0;
      mcode_n(context->object, &disp32, 4);
    } break;
    case r16: {
      // 0x66 + 0x8b /r
      uint8_t sixteen_bit_prefix = 0x66;
      mcode_1(context->object, sixteen_bit_prefix);
    } FALLTHROUGH;
    case r32: {
      // 0x8b /r
      uint8_t op = 0x8b;

      // RIP-Relative Addressing
      if (address_register == REG_RIP) {
        uint8_t destination_regbits = regbits(destination_register);
        if (REGBITS_TOP(destination_regbits)) {
          uint8_t rex = rex_byte(false,  REGBITS_TOP(destination_regbits), false, false);
          mcode_1(context->object, rex);
        }

        // Mod == 0b00
        // R/M == 0b101 (none)
        uint8_t modrm = modrm_byte(0b00, destination_regbits, 0b101);

        mcode_2(context->object, op, modrm);
        MCODE_SIB_IF_R12;

        // Make RIP-relative disp32 relocation
        RelocationEntry reloc = {0};
        Section *sec_code = code_section(context->object);
        ASSERT(sec_code, "NO CODE SECTION, WHAT HAVE YOU DONE?");
        reloc.sym.byte_offset = sec_code->data.bytes.size;
        reloc.sym.name = strdup(name);
        reloc.sym.section_name = strdup(sec_code->name);
        reloc.type = RELOC_DISP32_PCREL;
        vector_push(context->object->relocs, reloc);

        int32_t disp32 = 0;
        mcode_n(context->object, &disp32, 4);
        break;
      }

      // Encode a REX prefix if either of the ModRM register descriptors need
      // the bit extension.
      uint8_t address_regbits = regbits(address_register);
      uint8_t destination_regbits = regbits(destination_register);
      if (REGBITS_TOP(address_regbits) || REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(address_regbits), false, REGBITS_TOP(destination_regbits));
        mcode_1(context->object, rex);
      }

      // Mod == 0b10  ->  (R/M)+disp32
      // Reg == Destination
      // R/M == Address
      uint8_t modrm = modrm_byte(0b10, destination_regbits, address_regbits);

      mcode_2(context->object, op, modrm);
      MCODE_SIB_IF_R12;

      // Make disp32 relocation to lea from symbol
      RelocationEntry reloc = {0};
      Section *sec_code = code_section(context->object);
      reloc.sym.byte_offset = sec_code->data.bytes.size;
      reloc.sym.name = strdup(name);
      reloc.sym.section_name = strdup(sec_code->name);
      reloc.type = RELOC_DISP32;
      vector_push(context->object->relocs, reloc);

      int32_t disp32 = 0;
      mcode_n(context->object, &disp32, 4);
    } break;
    case r64: {
      // REX.W + 0x8b /r
      uint8_t op = 0x8b;

      // RIP-Relative Addressing
      if (address_register == REG_RIP) {
        uint8_t destination_regbits = regbits(destination_register);
        uint8_t rex = rex_byte(true, REGBITS_TOP(destination_regbits), false, false);

        // Mod == 0b00
        // R/M == 0b101 (none)
        uint8_t modrm = modrm_byte(0b00, destination_regbits, 0b101);

        mcode_3(context->object, rex, op, modrm);
        MCODE_SIB_IF_R12;

        // Make RIP-relative disp32 relocation
        RelocationEntry reloc = {0};
        Section *sec_code = code_section(context->object);
        ASSERT(sec_code, "NO CODE SECTION, WHAT HAVE YOU DONE?");
        reloc.sym.byte_offset = sec_code->data.bytes.size;
        reloc.sym.name = strdup(name);
        reloc.sym.section_name = strdup(sec_code->name);
        reloc.type = RELOC_DISP32_PCREL;
        vector_push(context->object->relocs, reloc);

        int32_t disp32 = 0;
        mcode_n(context->object, &disp32, 4);
        break;
      }

      // Encode a REX prefix if either of the ModRM register descriptors need
      // the bit extension.
      uint8_t address_regbits = regbits(address_register);
      uint8_t destination_regbits = regbits(destination_register);
      uint8_t rex = rex_byte(true, REGBITS_TOP(address_regbits), false, REGBITS_TOP(destination_regbits));

      // Mod == 0b10  ->  (R/M)+disp32
      // Reg == Destination
      // R/M == Address
      uint8_t modrm = modrm_byte(0b10, destination_regbits, address_regbits);

      mcode_3(context->object, rex, op, modrm);
      MCODE_SIB_IF_R12;

      // Make disp32 relocation to lea from symbol
      RelocationEntry reloc = {0};
      Section *sec_code = code_section(context->object);
      reloc.sym.byte_offset = sec_code->data.bytes.size;
      reloc.sym.name = strdup(name);
      reloc.sym.section_name = strdup(sec_code->name);
      reloc.type = RELOC_DISP32;
      vector_push(context->object->relocs, reloc);

      int32_t disp32 = 0;
      mcode_n(context->object, &disp32, 4);
    } break;

    } // switch (size)

  } break; // case MX64_MOV

  default: ICE("ERROR: mcode_name_to_reg(): Unsupported instruction %d (%s)", inst, mir_x86_64_opcode_mnemonic(inst));
  }
}

static void mcode_reg_to_mem(CodegenContext *context, MIROpcodex86_64 inst, RegisterDescriptor source_register, enum RegSize size, RegisterDescriptor address_register, int64_t offset) {
  switch (inst) {

  case MX64_MOV: {

    switch (size) {
    default: ICE("Unhandled register size");
    case r8: {
      // Move r8 into m8
      // 0x88 /r
      uint8_t op = 0x88;

      // Encode a REX prefix if either of the ModRM register
      // descriptors need the bit extension.
      uint8_t source_regbits = regbits(source_register);
      uint8_t address_regbits = regbits(address_register);
      if (REGBITS_TOP(source_regbits) || REGBITS_TOP(address_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, REGBITS_TOP(address_regbits));
        mcode_1(context->object, rex);
      }

      if (offset >= -128 && offset <= 127) {

        // To cut down on code size, we encode offsets that fit into
        // one byte using a Mod value of 0b01, allowing for only one byte
        // to be written for the displacement.

        // Mod == 0b01  ->  register + disp8
        // Reg == Source
        // R/M == Address
        uint8_t modrm = modrm_byte(0b01, source_regbits, address_regbits);
        int8_t displacement = (int8_t)offset;

        mcode_2(context->object, op, modrm);
        MCODE_SIB_IF_R12;
        mcode_1(context->object, (uint8_t)displacement);

      } else if (offset) {

        // Mod == 0b10  ->  register + disp32
        // Reg == Source
        // R/M == Address
        uint8_t modrm = modrm_byte(0b10, source_regbits, address_regbits);
        int32_t displacement = (int32_t)offset;

        mcode_2(context->object, op, modrm);
        MCODE_SIB_IF_R12;
        mcode_n(context->object, &displacement, 4);

      } else {

        // If offset is zero, we can omit the displacement byte(s).
        // Mod == 0b00  ->  (R/M)
        // Reg == Source
        // R/M == Address
        uint8_t modrm = modrm_byte(0b00, source_regbits, address_regbits);

        mcode_2(context->object, op, modrm);
        MCODE_SIB_IF_R12;

      }

    } break;
    case r16: {
      // 0x66 + 0x89 /r
      uint8_t sixteen_bit_prefix = 0x66;
      mcode_1(context->object, sixteen_bit_prefix);
    } FALLTHROUGH;
    case r32: {
      // Move r32 into m32
      // 0x89 /r
      uint8_t op = 0x89;

      // Encode a REX prefix if either of the ModRM register descriptors need
      // the bit extension.
      uint8_t source_regbits = regbits(source_register);
      uint8_t address_regbits = regbits(address_register);
      if (REGBITS_TOP(source_regbits) || REGBITS_TOP(address_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, REGBITS_TOP(address_regbits));
        mcode_1(context->object, rex);
      }

      // Mod == 0b10  ->  register + disp32
      // Reg == Source
      // R/M == Address
      uint8_t modrm = modrm_byte(0b10, source_regbits, address_regbits);
      int32_t displacement = (int32_t)offset;

      mcode_2(context->object, op, modrm);
      MCODE_SIB_IF_R12;
      mcode_n(context->object, &displacement, 4);

    } break;
    case r64: {
      // Move r64 into m64
      // REX.W + 0x89 /r
      uint8_t op = 0x89;

      // Encode a REX.W prefix to promote operation to 64-bits, also
      // including ModRM register bit extension(s).
      uint8_t source_regbits = regbits(source_register);
      uint8_t address_regbits = regbits(address_register);
      uint8_t rex = rex_byte(true, REGBITS_TOP(source_regbits), false, REGBITS_TOP(address_regbits));

      // Make output code smaller when possible by omitting zero displacements.
      if (offset == 0) {
        // Mod == 0b00  ->  R/M
        // Reg == Source
        // R/M == Address
        uint8_t modrm = modrm_byte(0b00, source_regbits, address_regbits);

        mcode_3(context->object, rex, op, modrm);
        MCODE_SIB_IF_R12;
      } else {
        // Mod == 0b10  ->  R/M + disp32
        // Reg == Source
        // R/M == Address
        uint8_t modrm = modrm_byte(0b10, source_regbits, address_regbits);
        int32_t displacement = (int32_t)offset;

        mcode_3(context->object, rex, op, modrm);
        MCODE_SIB_IF_R12;
        mcode_n(context->object, &displacement, 4);
      }
    } break;
    }

  } break;

  default: ICE("ERROR: mcode_reg_to_mem(): Unsupported instruction %d (%s)", inst, mir_x86_64_opcode_mnemonic(inst));
  }
}

static void mcode_reg_to_reg
(CodegenContext *context,
 MIROpcodex86_64 inst,
 RegisterDescriptor source_register, enum RegSize source_size,
 RegisterDescriptor destination_register, enum RegSize destination_size
 )
{
  // Always optimise away moves from a register to itself
  if (inst == MX64_MOV && source_register == destination_register && source_size == destination_size) return;

  uint8_t source_regbits = regbits(source_register);
  uint8_t destination_regbits = regbits(destination_register);
  // Mod == 0b11  ->  Reg
  // Reg == Source
  // R/M == Destination
  uint8_t modrm = modrm_byte(0b11, source_regbits, destination_regbits);

  switch (inst) {

  case MX64_IMUL: {
    ASSERT(source_size == destination_size, "x86_64 machine code backend requires reg-to-reg imuls to be of equal size.");

    switch (source_size) {
    case r8: ICE("x86_64 doesn't have an IMUL r8, r8 opcode, sorry");

    case r16: {
      TODO("Signed multiply r16 into r16");
      // TODO:
      // Signed multiply r16 into r16
      // 0x66 + 0x0f 0xaf /r
      //mcode_1(context->object, 0x66);
    } FALLTHROUGH;
    case r32: {
      TODO("Signed multiply r32 into r32");
      // TODO:
      // Signed multiply r32 into r32
      // 0x0f 0xaf /r
    } break;

    case r64: {
      TODO("Signed multiply r64 into r64");
      // TODO:
      // Signed multiply r64 into r64
      // REX.W + 0x0f 0xaf /r
    } break;

    } // switch (size)

  } break; // case MX64_IMUL

  case MX64_MOVZX: {
    ASSERT(source_size < destination_size, "Zero extension requires source to be smaller than destination!");

    switch (source_size) {
    default: ICE("Unhandled register size");
    case r64: ICE("x86_64 movzx does not have a 64 bit source operand encoding");
    case r32: ICE("x86_64 movzx does not have a 32 bit source operand encoding");
    case r16: {
      switch (destination_size) {
      default: ICE("Unhandled register size");
      case r8: ICE("x86_64 movzx does not have a 16 bit to 8 bit operand encoding");
      case r16: ICE("x86_64 movzx does not have a 16 bit to 16 bit operand encoding");
      case r32: {
        // 0x0f + 0xb7 /r
        if (REGBITS_TOP(source_regbits) || REGBITS_TOP(destination_regbits)) {
          uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
          mcode_1(context->object, rex);
        }
        mcode_3(context->object, 0x0f, 0xb7, modrm);
      } break;
      case r64: {
        // REX.W + 0x0f + 0xb7 /r
        uint8_t rex = rex_byte(true, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
        mcode_4(context->object, rex, 0x0f, 0xb7, modrm);
      } break;
      }

    } break;
    case r8: {
      switch (destination_size) {
      default: ICE("Unhandled register size");
      case r8: ICE("x86_64 movzx does not have an 8 bit to 8 bit operand encoding");
      case r16: {
        // 0x66 + 0x0f + 0xb6 /r
        mcode_1(context->object, 0x66);
      } FALLTHROUGH;
      case r32: {
        // 0x0f + 0xb6 /r
        if (REGBITS_TOP(source_regbits) || REGBITS_TOP(destination_regbits)) {
          uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
          mcode_1(context->object, rex);
        }
        mcode_3(context->object, 0x0f, 0xb6, modrm);
      } break;
      case r64: {
        // REX.W + 0x0f + 0xb6 /r
        uint8_t rex = rex_byte(true, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
        mcode_4(context->object, rex, 0x0f, 0xb6, modrm);
      } break;
      } // switch (destination_size)

    } break; // case r8
    } // switch (source_size)

  } break; // MX64_MOVZX

  case MX64_MOVSX: {
    ASSERT(source_size < destination_size, "Sign extension requires source to be smaller than destination!");

    switch (source_size) {
    default: ICE("Unhandled register size");
    case r64: ICE("x86_64 movsx does not have a 64 bit source operand encoding");
    case r32: {
      ASSERT(destination_size == r64);
      // REX.W + 0x63 /r
      uint8_t rex = rex_byte(true, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
      mcode_3(context->object, rex, 0x63, modrm);
    } break; // case r32
    case r16: {
      ASSERT(destination_size >= r32);
      switch (destination_size) {
      case r8: ICE("x86_64 movsx does not have a 16 to 8 bit operand encoding");
      case r16: ICE("x86_64 movsx does not have a 16 to 16 bit operand encoding");
      case r32: {
        // 0x0f + 0xbf /r
        if (REGBITS_TOP(source_regbits) || REGBITS_TOP(destination_regbits)) {
          uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
          mcode_1(context->object, rex);
        }
        mcode_3(context->object, 0x0f, 0xbf, modrm);
      } break;
      case r64: {
        // REX.W + 0x0f + 0xbf /r
        uint8_t rex = rex_byte(true, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
        mcode_4(context->object, rex, 0x0f, 0xbf, modrm);
      } break;
      } // switch (destination_size)

    } break; // case r16
    case r8: {
      ASSERT(destination_size >= r16);
      switch (destination_size) {
      case r8: ICE("x86_64 movsx does not have an 8 to 8 bit operand encoding");
      case r16: {
        // 0x66 + 0x0f + 0xbe /r
        mcode_1(context->object, 0x66);
      } FALLTHROUGH;
      case r32: {
        // 0x0f + 0xbe /r
        if (REGBITS_TOP(source_regbits) || REGBITS_TOP(destination_regbits)) {
          uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
          mcode_1(context->object, rex);
        }
        mcode_3(context->object, 0x0f, 0xbe, modrm);
      } break;
      case r64: {
        // REX.W + 0x0f + 0xbe /r
        uint8_t rex = rex_byte(true, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
        mcode_4(context->object, rex, 0x0f, 0xbe, modrm);
      } break;
      } // switch (destination_size)

    } break; // case r8
    } // switch (source_size)
  } break; // case MX64_MOVSX

  case MX64_MOV: {
    ASSERT(source_size == destination_size, "x86_64 machine code backend requires reg-to-reg moves to be of equal size.");

    switch (source_size) {
    default: ICE("Unhandled register size");

    case r8: {
      // Move r8 to r8
      // 0x88 /r
      // Encode a REX prefix if either of the ModRM register
      // descriptors need the bit extension.
      if (REGBITS_TOP(source_regbits) || REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
        mcode_1(context->object, rex);
      }
      mcode_2(context->object, 0x88, modrm);
    } break;

    case r16: {
      // 0x66 + 0x89 /r
      mcode_1(context->object, 0x66);
    } FALLTHROUGH;
    case r32: {
      // 0x89 /r
      // Encode a REX prefix if either of the ModRM register descriptors need
      // the bit extension.
      if (REGBITS_TOP(source_regbits) || REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
        mcode_1(context->object, rex);
      }
      mcode_2(context->object, 0x89, modrm);
    } break;

    case r64: {
      // REX.W + 0x89 /r
      uint8_t rex = rex_byte(true, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
      mcode_3(context->object, rex, 0x89, modrm);
    } break;

    } // switch (size)

  } break; // case MX64_MOV

  case MX64_AND: {
    ASSERT(source_size == destination_size, "x86_64 machine code backend requires reg-to-reg ands to be of equal size.");

    switch (source_size) {
    default: ICE("Unhandled register size");
    case r8: {
      // Bitwise and r8 with r8
      // 0x20 /r
      if (REGBITS_TOP(source_regbits) || REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
        mcode_1(context->object, rex);
      }
      mcode_2(context->object, 0x20, modrm);
    } break;

    case r16: {
      // 0x66 + 0x21 /r
      mcode_1(context->object, 0x66);
    } FALLTHROUGH;
    case r32: {
      // 0x21 /r
      if (REGBITS_TOP(source_regbits) || REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
        mcode_1(context->object, rex);
      }
      mcode_2(context->object, 0x21, modrm);
    } break;

    case r64: {
      // REX.W + 0x21 /r
      uint8_t rex = rex_byte(true, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
      mcode_3(context->object, rex, 0x21, modrm);
    } break;

    } // switch (size)

  } break; // case MX64_AND

  case MX64_OR: {
    ASSERT(source_size == destination_size, "x86_64 machine code backend requires reg-to-reg ors to be of equal size.");

    switch (source_size) {
    default: ICE("Unhandled register size");
    case r8: {
      // Bitwise or r8 with r8
      // 0x08 /r
      if (REGBITS_TOP(source_regbits) || REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
        mcode_1(context->object, rex);
      }
      mcode_2(context->object, 0x08, modrm);
    } break;

    case r16: {
      // 0x66 + 0x09 /r
      mcode_1(context->object, 0x66);
    } FALLTHROUGH;
    case r32: {
      // 0x09 /r
      if (REGBITS_TOP(source_regbits) || REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
        mcode_1(context->object, rex);
      }
      mcode_2(context->object, 0x09, modrm);
    } break;

    case r64: {
      // REX.W + 0x09 /r
      uint8_t rex = rex_byte(true, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
      mcode_3(context->object, rex, 0x09, modrm);
    } break;

    } // switch (size)

  } break; // case MX64_OR

  case MX64_ADD: {

    ASSERT(source_size == destination_size, "x86_64 machine code backend requires reg-to-reg adds to be of equal size.");

    switch (source_size) {
    default: ICE("Unhandled register size");
    case r8: {
      // Add r8 to r8
      // 0x00 /r

      if (REGBITS_TOP(source_regbits) || REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
        mcode_1(context->object, rex);
      }

      mcode_2(context->object, 0x00, modrm);
    } break;

    case r16: {
      // 0x66 + 0x01 /r
      mcode_1(context->object, 0x66);
    } FALLTHROUGH;
    case r32: {
      // 0x01 /r

      if (REGBITS_TOP(source_regbits) || REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
        mcode_1(context->object, rex);
      }

      mcode_2(context->object, 0x01, modrm);
    } break;

    case r64: {
      // REX.W + 0x01 /r
      uint8_t rex = rex_byte(true, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
      mcode_3(context->object, rex, 0x01, modrm);
    } break;

    } // switch (size)

  } break; // case MX64_ADD

  case MX64_SUB: {

    ASSERT(source_size == destination_size, "x86_64 machine code backend requires reg-to-reg subs to be of equal size.");

    switch (source_size) {
    default: ICE("Unhandled register size");
    case r8: {
      // Subtract r8 from r8
      // 0x28 /r
      if (REGBITS_TOP(source_regbits) || REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
        mcode_1(context->object, rex);
      }
      mcode_2(context->object, 0x28, modrm);
    } break;

    case r16: {
      // 0x66 + 0x29 /r
      mcode_1(context->object, 0x66);
    } FALLTHROUGH;
    case r32: {
      // 0x29 /r
      if (REGBITS_TOP(source_regbits) || REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
        mcode_1(context->object, rex);
      }
      mcode_2(context->object, 0x29, modrm);
    } break;

    case r64: {
      // REX.W + 0x29 /r
      uint8_t rex = rex_byte(true, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
      mcode_3(context->object, rex, 0x29, modrm);
    } break;

    } // switch (size)

  } break; // case MX64_SUB

  case MX64_CMP: {

    ASSERT(source_size == destination_size, "x86_64 machine code backend requires reg-to-reg cmps to be of equal size.");

    switch (source_size) {
    default: ICE("Unhandled register size");
    case r8: {
      // 0x38 /r
      if (REGBITS_TOP(source_regbits) || REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
        mcode_1(context->object, rex);
      }
      mcode_2(context->object, 0x38, modrm);
    } break;

    case r16: {
      // 0x66 + 0x39 /r
      mcode_1(context->object, 0x66);
    } FALLTHROUGH;
    case r32: {
      // 0x39 /r
      if (REGBITS_TOP(source_regbits) || REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
        mcode_1(context->object, rex);
      }
      mcode_2(context->object, 0x39, modrm);
    } break;

    case r64: {
      // REX.W + 0x39 /r
      uint8_t rex = rex_byte(true, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
      mcode_3(context->object, rex, 0x39, modrm);
    } break;

    } // switch (size)

  } break; // case MX64_CMP

  case MX64_TEST: {
    ASSERT(source_size == destination_size, "x86_64 machine code backend requires reg-to-reg tests to be of equal size.");

    switch (source_size) {
    default: ICE("Unhandled register size");
    case r8: {
      // 0x84 /r
      if (REGBITS_TOP(source_regbits) || REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
        mcode_1(context->object, rex);
      }
      mcode_2(context->object, 0x84, modrm);
    } break;

    case r16: {
      // 0x66 + 0x85 /r
      uint8_t sixteen_bit_prefix = 0x66;
      mcode_1(context->object, sixteen_bit_prefix);
    } FALLTHROUGH;
    case r32: {
      // 0x85 /r
      if (REGBITS_TOP(source_regbits) || REGBITS_TOP(destination_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
        mcode_1(context->object, rex);
      }
      mcode_2(context->object, 0x85, modrm);
    } break;

    case r64: {
      // REX.W + 0x85 /r
      uint8_t rex = rex_byte(true, REGBITS_TOP(source_regbits), false, REGBITS_TOP(destination_regbits));
      mcode_3(context->object, rex, 0x85, modrm);
    } break;

    } // switch (size)

  } break; // case MX64_TEST

  default: ICE("ERROR: mcode_reg_to_reg(): Unsupported instruction %d (%s)", inst, mir_x86_64_opcode_mnemonic(inst));

  }
}

static void mcode_indirect_branch(CodegenContext *context, MIROpcodex86_64 inst, RegisterDescriptor address_register) {
  switch (inst) {
  case MX64_CALL: {
    // 0xff /2
    uint8_t address_regbits = regbits(address_register);
    if (REGBITS_TOP(address_regbits)) {
      uint8_t rex = rex_byte(false, REGBITS_TOP(address_regbits), false, false);
      mcode_1(context->object, rex);
    }
    uint8_t modrm = modrm_byte(0b11, 2, address_regbits);
    mcode_2(context->object, 0xff, modrm);
  } break;
  default: ICE("ERROR: mcode_indirect_branch(): Unsupported instruction %d (%s)", inst, mir_x86_64_opcode_mnemonic(inst));
  }
}

// Basically, with a shift instruction, use the `%given_reg << %cl` encoding.
static void mcode_reg_shift(CodegenContext *context, MIROpcodex86_64 inst, RegisterDescriptor register_to_shift, RegSize size) {
  switch (inst) {
  case MX64_SAR: FALLTHROUGH;
  case MX64_SHR: FALLTHROUGH;
  case MX64_SHL: {
    uint8_t rbits = regbits(register_to_shift);
    // [0x66] + 0xd2/0xd3 /4
    uint8_t opcode_extension = 0;
    if (inst == MX64_SHL) opcode_extension = 4;
    else if (inst == MX64_SHR) opcode_extension = 5;
    else if (inst == MX64_SAR) opcode_extension = 7;
    else ICE("Unhandled shift opcode");
    uint8_t modrm = modrm_byte(0b11, opcode_extension, rbits);

    switch (size) {
    default: ICE("Unhandled register size");
    case r8: {
      // 0xd2 /4
      if (REGBITS_TOP(rbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(rbits), false, false);
        mcode_1(context->object, rex);
      }
      mcode_2(context->object, 0xd2, modrm);
    } break;

    case r16: {
      // 0x66 + 0xd3 /4
      mcode_1(context->object, 0x66);
    } FALLTHROUGH;
    case r32: {
      // 0xd3 /4
      if (REGBITS_TOP(rbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(rbits), false, false);
        mcode_1(context->object, rex);
      }

      mcode_2(context->object, 0xd3, modrm);
    } break;

    case r64: {
      // REX.W + 0xd3 /4
      uint8_t rex = rex_byte(true, REGBITS_TOP(rbits), false, false);
      mcode_3(context->object, rex, 0xd3, modrm);
    } break;
    } // switch (size)

  } break; // case MX64_SHL

  default: ICE("ERROR: mcode_reg_shift(): Unsupported instruction %d (%s)", inst, mir_x86_64_opcode_mnemonic(inst));
  }
}

static void mcode_reg(CodegenContext *context, MIROpcodex86_64 inst, RegisterDescriptor reg, RegSize size) {
  if (inst == MX64_JMP || inst == MX64_CALL) {
    mcode_indirect_branch(context, inst, reg);
    return;
  }
  if (inst == MX64_SAL || inst == MX64_SAR || inst == MX64_SHL || inst == MX64_SHR) {
    mcode_reg_shift(context, inst, reg, size);
    return;
  }

  // NOTE: +rb/+rw/+rd/+ro indicate the lower three bits of the opcode byte are used to indicate the register operand.
  // In 64-bit mode, indicates the four bit field of REX.b and opcode[2:0] field encodes the register operand.

  uint8_t source_regbits = regbits(reg);

  switch (inst) {
  case MX64_PUSH: {
    switch (size) {
    default: ICE("Unhandled register size");
    case r8:
    case r32:
      ICE("ERROR: x86_64 doesn't support pushing %Z-byte registers to the stack.", regbytes_from_size(size));

    case r16: {
      // 0x66 + 0x50+rw
      uint8_t op = 0x50 + rw_encoding(reg);
      mcode_2(context->object, 0x66, op);
    } break;
    case r64: {
      // 0x50+rd
      if (REGBITS_TOP(source_regbits)) {
        uint8_t rex = rex_byte(false, false,false, REGBITS_TOP(source_regbits));
        mcode_1(context->object, rex);
      }
      uint8_t op = 0x50 + rd_encoding(reg);
      mcode_1(context->object, op);
    } break;
    } // switch (size)
  } break; // case MX64_PUSH

  case MX64_POP: {
    switch (size) {
    default: ICE("Unhandled register size");
    case r8:
    case r32:
      ICE("ERROR: x86_64 doesn't support pushing %Z-byte registers to the stack.", regbytes_from_size(size));

    case r16: {
      // 0x66 + 0x58+rw
      uint8_t op = 0x58 + rw_encoding(reg);
      mcode_2(context->object, 0x66, op);
    } break;
    case r64: {
      // 0x58+rd
      if (REGBITS_TOP(source_regbits)) {
        uint8_t rex = rex_byte(false, false,false, REGBITS_TOP(source_regbits));
        mcode_1(context->object, rex);
      }
      uint8_t op = 0x58 + rd_encoding(reg);
      mcode_1(context->object, op);
    } break;
    } // switch (size)
  } break; // case MX64_POP

  case MX64_IDIV: FALLTHROUGH;
  case MX64_NOT: {
    // idiv == [REX.W] + 0xf6/0xf7 /7
    // not  == [REX.W] + 0xf6/0xf7 /2
    // Only differ in opcode extension
    const uint8_t idiv_extension = 7;
    const uint8_t not_extension = 2;

    // Mod == 0b11  ->  register
    // Reg == Opcode Extension
    // R/M == Register Encoding
    uint8_t extension = idiv_extension;
    if (inst == MX64_NOT) extension = not_extension;
    uint8_t modrm = modrm_byte(0b11, extension, source_regbits);

    switch (size) {
    default: ICE("Unhandled register size");
    case r8: {
      // 0xf6 /2
      if (REGBITS_TOP(source_regbits)) {
        uint8_t rex = rex_byte(false, false,false, REGBITS_TOP(source_regbits));
        mcode_1(context->object, rex);
      }
      mcode_2(context->object, 0xf6, modrm);
    } break;
    case r16: {
      // 0x66 + 0xf7 /2
      mcode_1(context->object, 0x66);
    } FALLTHROUGH;
    case r32: {
      // 0xf7 /2
      if (REGBITS_TOP(source_regbits)) {
        uint8_t rex = rex_byte(false, false,false, REGBITS_TOP(source_regbits));
        mcode_1(context->object, rex);
      }
      mcode_2(context->object, 0xf7, modrm);
    } break;
    case r64: {
      // REX.W + 0xf7 /2
      uint8_t rex = rex_byte(true, false,false, REGBITS_TOP(source_regbits));
      mcode_3(context->object, rex, 0xf7, modrm);
    } break;
    } // switch (size)
  } break; // case MX64_NOT

  default:
    ICE("ERROR: mcode_reg(): Unsupported instruction %d (%s)", inst, mir_x86_64_opcode_mnemonic(inst));
  }
}

static void mcode_reg_to_name(CodegenContext *context, MIROpcodex86_64 inst, RegisterDescriptor source_register, enum RegSize size, RegisterDescriptor address_register, const char *name) {
  switch (inst) {

  case MX64_MOV: {

    switch (size) {
    default: ICE("Unhandled register size");
    case r8: {
      // 0x88 /r
      if (address_register == REG_RIP) {
        uint8_t source_regbits = regbits(source_register);
        // RIP-relative ModRM byte
        // Mod == 0b00
        // Reg == Source Register
        // R/M == 0b101
        uint8_t modrm = modrm_byte(0b00, source_regbits, 0b101);
        if (REGBITS_TOP(source_regbits)) {
          uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, false);
          mcode_1(context->object, rex);
        }
        mcode_2(context->object, 0x88, modrm);

        // Make RIP-relative disp32 relocation
        RelocationEntry reloc = {0};
        Section *sec_code = code_section(context->object);
        ASSERT(sec_code, "NO CODE SECTION, WHAT HAVE YOU DONE?");
        reloc.sym.byte_offset = sec_code->data.bytes.size;
        reloc.sym.name = strdup(name);
        reloc.sym.section_name = strdup(sec_code->name);
        reloc.type = RELOC_DISP32_PCREL;
        vector_push(context->object->relocs, reloc);

        int32_t disp32 = 0;
        mcode_n(context->object, &disp32, 4);
        break;
      }
      uint8_t source_regbits = regbits(source_register);
      uint8_t address_regbits = regbits(address_register);
      // Mod == 0b10  ->  (register + disp32)
      // Reg == Source
      // R/M == Address
      uint8_t modrm = modrm_byte(0b10, source_regbits, address_regbits);
      if (REGBITS_TOP(source_regbits) || REGBITS_TOP(address_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, REGBITS_TOP(address_regbits));
        mcode_1(context->object, rex);
      }
      mcode_2(context->object, 0x88, modrm);
      MCODE_SIB_IF_R12;

      // Generate disp32 relocation!
      RelocationEntry reloc = {0};
      Section *sec_code = code_section(context->object);
      reloc.sym.byte_offset = sec_code->data.bytes.size;
      reloc.sym.name = strdup(name);
      reloc.sym.section_name = strdup(sec_code->name);
      reloc.type = RELOC_DISP32;
      vector_push(context->object->relocs, reloc);

      int32_t disp32 = 0;
      mcode_n(context->object, &disp32, 4);
    } break;
    case r16: {
      // 0x66 + 0x89 /r
      mcode_1(context->object, 0x66);
    } FALLTHROUGH;
    case r32: {
      // 0x89 /r
      if (address_register == REG_RIP) {
        uint8_t source_regbits = regbits(source_register);
        // RIP-relative ModRM byte
        // Mod == 0b00
        // Reg == Source Register
        // R/M == 0b101
        uint8_t modrm = modrm_byte(0b00, source_regbits, 0b101);
        if (REGBITS_TOP(source_regbits)) {
          uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, false);
          mcode_1(context->object, rex);
        }

        mcode_2(context->object, 0x89, modrm);

        // Make RIP-relative disp32 relocation
        RelocationEntry reloc = {0};
        Section *sec_code = code_section(context->object);
        ASSERT(sec_code, "NO CODE SECTION, WHAT HAVE YOU DONE?");
        reloc.sym.byte_offset = sec_code->data.bytes.size;
        reloc.sym.name = strdup(name);
        reloc.sym.section_name = strdup(sec_code->name);
        reloc.type = RELOC_DISP32_PCREL;
        vector_push(context->object->relocs, reloc);

        int32_t disp32 = 0;
        mcode_n(context->object, &disp32, 4);
        break;
      }
      uint8_t source_regbits = regbits(source_register);
      uint8_t address_regbits = regbits(address_register);
      // Mod == 0b10  ->  (register + disp32)
      // Reg == Source
      // R/M == Address
      uint8_t modrm = modrm_byte(0b10, source_regbits, address_regbits);
      if (REGBITS_TOP(source_regbits) || REGBITS_TOP(address_regbits)) {
        uint8_t rex = rex_byte(false, REGBITS_TOP(source_regbits), false, REGBITS_TOP(address_regbits));
        mcode_1(context->object, rex);
      }
      mcode_2(context->object, 0x89, modrm);
      MCODE_SIB_IF_R12;

      // Generate disp32 relocation!
      RelocationEntry reloc = {0};
      Section *sec_code = code_section(context->object);
      reloc.sym.byte_offset = sec_code->data.bytes.size;
      reloc.sym.name = strdup(name);
      reloc.sym.section_name = strdup(sec_code->name);
      reloc.type = RELOC_DISP32;
      vector_push(context->object->relocs, reloc);

      int32_t disp32 = 0;
      mcode_n(context->object, &disp32, 4);
    } break;
    case r64: {
      // REX.W + 0x89 /r
      if (address_register == REG_RIP) {
        uint8_t source_regbits = regbits(source_register);
        // RIP-relative ModRM byte
        // Mod == 0b00
        // Reg == Source Register
        // R/M == 0b101
        uint8_t modrm = modrm_byte(0b00, source_regbits, 0b101);
        uint8_t rex = rex_byte(true, REGBITS_TOP(source_regbits), false, false);
        mcode_3(context->object, rex, 0x89, modrm);

        // Make RIP-relative disp32 relocation
        RelocationEntry reloc = {0};
        Section *sec_code = code_section(context->object);
        ASSERT(sec_code, "NO CODE SECTION, WHAT HAVE YOU DONE?");
        reloc.sym.byte_offset = sec_code->data.bytes.size;
        reloc.sym.name = strdup(name);
        reloc.sym.section_name = strdup(sec_code->name);
        reloc.type = RELOC_DISP32_PCREL;
        vector_push(context->object->relocs, reloc);

        int32_t disp32 = 0;
        mcode_n(context->object, &disp32, 4);
        break;
      }
      uint8_t source_regbits = regbits(source_register);
      uint8_t address_regbits = regbits(address_register);
      uint8_t rex = rex_byte(true, REGBITS_TOP(source_regbits), false, REGBITS_TOP(address_regbits));
      // Mod == 0b10  ->  (register + disp32)
      // Reg == Source
      // R/M == Address
      uint8_t modrm = modrm_byte(0b10, source_regbits, address_regbits);
      mcode_3(context->object, rex, 0x89, modrm);
      MCODE_SIB_IF_R12;

      // Generate disp32 relocation!
      RelocationEntry reloc = {0};
      Section *sec_code = code_section(context->object);
      reloc.sym.byte_offset = sec_code->data.bytes.size;
      reloc.sym.name = strdup(name);
      reloc.sym.section_name = strdup(sec_code->name);
      reloc.type = RELOC_DISP32;
      vector_push(context->object->relocs, reloc);

      int32_t disp32 = 0;
      mcode_n(context->object, &disp32, 4);
    } break;

    } // switch (size)

  } break; // case MX64_MOV

  default: ICE("ERROR: mcode_reg_to_name(): Unsupported instruction %d (%s)", inst, mir_x86_64_opcode_mnemonic(inst));
  }
}

static void mcode_reg_to_offset_name(CodegenContext *context, MIROpcodex86_64 inst, RegisterDescriptor source_register, enum RegSize size, RegisterDescriptor address_register, const char *name, usz offset) {
  // TODO: Generate a relocation entry that will end up in the object file...
  switch (inst) {
  default: ICE("ERROR: mcode_reg_to_offset_name(): Unsupported instruction %d (%s)", inst, mir_x86_64_opcode_mnemonic(inst));
  }
}

static void mcode_mem(CodegenContext *context, MIROpcodex86_64 inst, int64_t offset, RegisterDescriptor address_register) {
  switch (inst) {
  default: ICE("ERROR: mcode_mem(): Unsupported instruction %d (%s)", inst, mir_x86_64_opcode_mnemonic(inst));
  }
}

static void mcode_imm(CodegenContext *context, MIROpcodex86_64 inst, int64_t immediate) {
  switch (inst) {
  case MX64_PUSH: {
    // TODO: What size immediate to push?
    // PUSH imm8:  0x6a ib
    // PUSH imm16: 0x66 + 0x68 iw
    // PUSH imm32: 0x68 id
    uint8_t op = 0x68;
    int32_t immediate_value = (int32_t)immediate;
    mcode_1(context->object, op);
    mcode_n(context->object, &immediate_value, 4);
  } break;

  default:
    ICE("ERROR: mcode_imm(): Unsupported instruction %d (%s)", inst, mir_x86_64_opcode_mnemonic(inst));
  }
}

/// IS_FUNCTION should be true iff NAME is the symbol of a function.
static void mcode_name(CodegenContext *context, MIROpcodex86_64 inst, const char *name, bool is_function) {
  switch (inst) {

  case MX64_CALL: {
    uint8_t op = 0xe8;
    int32_t disp32 = 0;
    mcode_1(context->object, op);

    // Make disp32 relocation to lea from symbol
    RelocationEntry reloc = {0};
    Section *sec_code = code_section(context->object);
    // Current offset in machine code byte buffer
    if (is_function) reloc.sym.type = GOBJ_SYMTYPE_FUNCTION;
    reloc.sym.byte_offset = sec_code->data.bytes.size;
    reloc.sym.name = strdup(name);
    reloc.sym.section_name = strdup(sec_code->name);
    reloc.type = RELOC_DISP32_PCREL;
    vector_push(context->object->relocs, reloc);

    mcode_n(context->object, &disp32, 4);
  } break; // case MX64_CALL

  case MX64_JMP: {
    uint8_t op = 0xe9;
    int32_t disp32 = 0;
    mcode_1(context->object, op);

    // Make disp32 relocation to lea from symbol
    RelocationEntry reloc = {0};
    Section *sec_code = code_section(context->object);
    if (is_function) reloc.sym.type = GOBJ_SYMTYPE_FUNCTION;
    reloc.sym.byte_offset = sec_code->data.bytes.size;
    reloc.sym.name = strdup(name);
    reloc.sym.section_name = strdup(sec_code->name);
    reloc.type = RELOC_DISP32_PCREL;
    vector_push(context->object->relocs, reloc);

    mcode_n(context->object, &disp32, 4);
  } break; // case MX64_JMP

  default: ICE("ERROR: mcode_name(): Unsupported instruction %d (%s)", inst, mir_x86_64_opcode_mnemonic(inst));
  }
}

static void mcode_none(CodegenContext *context, MIROpcodex86_64 inst) {
  switch (inst) {
  case MX64_RET: { // 0xc3
    uint8_t op = 0xc3;
    mcode_1(context->object, op);
  } break;

  case MX64_CWD: { // 0x66 + 0x99
    uint8_t sixteen_bit_prefix = 0x66;
    mcode_1(context->object, sixteen_bit_prefix);
  } FALLTHROUGH;
  case MX64_CDQ: { // 0x99
    uint8_t op = 0x99;
    mcode_1(context->object, op);
  } break;
  case MX64_CQO: { // REX.W + 0x99
    uint8_t op = 0x99;
    uint8_t rexw = rexw_byte();
    mcode_2(context->object, rexw, op);
  } break;

  case MX64_SYSCALL: {
    mcode_2(context->object, 0x0F, 0x05);
  } break;

  case MX64_UD2: {
    mcode_2(context->object, 0x0F, 0x0B);
  } break;

  case MX64_INT3: {
    mcode_1(context->object, 0xcc);
  } break;

  default:
    ICE("ERROR: mcode_none(): Unsupported instruction %d (%s)", inst, mir_x86_64_opcode_mnemonic(inst));
  }
}

static void mcode_setcc(CodegenContext *context, enum ComparisonType comparison_type, RegisterDescriptor value_register) {
  uint8_t op = 0;
  switch (comparison_type) {
  case COMPARE_EQ: op = 0x94; break;
  case COMPARE_NE: op = 0x95; break;
  case COMPARE_GT: op = 0x9f; break;
  case COMPARE_LT: op = 0x9c; break;
  case COMPARE_GE: op = 0x9d; break;
  case COMPARE_LE: op = 0x9e; break;
  default: ICE("Invalid comparison type");
  }

  uint8_t destination_regbits = regbits(value_register);
  if (REGBITS_TOP(destination_regbits)) {
    uint8_t rex = rex_byte(false, false, false, REGBITS_TOP(destination_regbits));
    mcode_1(context->object, rex);
  }

  // Mod == 0b11  ->  register
  // R/M == Destination
  uint8_t modrm = modrm_byte(0b11, 0, destination_regbits);

  uint8_t op_escape = 0x0f;
  mcode_3(context->object, op_escape, op, modrm);
}

/// IS_FUNCTION should be true iff LABEL is the symbol of a function.
static void mcode_jcc(CodegenContext *context, IndirectJumpType type, const char *label, bool is_function) {
  uint8_t op = 0;
  switch (type) {
  case JUMP_TYPE_E:  op = 0x84; break;
  case JUMP_TYPE_NE: op = 0x85; break;
  case JUMP_TYPE_G:  op = 0x8f; break;
  case JUMP_TYPE_L:  op = 0x8c; break;
  case JUMP_TYPE_GE: op = 0x8d; break;
  case JUMP_TYPE_LE: op = 0x8e; break;
  default: ICE("Unhandled jump type: %d", (int)type);
  }

  uint8_t op_escape = 0x0f;
  int32_t disp32 = 0;

  mcode_2(context->object, op_escape, op);

  // Make disp32 relocation to lea from symbol
  RelocationEntry reloc = {0};
  Section *sec_code = code_section(context->object);
  if (is_function) reloc.sym.type = GOBJ_SYMTYPE_FUNCTION;
  reloc.sym.byte_offset = sec_code->data.bytes.size;
  reloc.sym.name = strdup(label);
  reloc.sym.section_name = strdup(sec_code->name);
  reloc.type = RELOC_DISP32_PCREL;
  vector_push(context->object->relocs, reloc);

  mcode_n(context->object, &disp32, 4);
}

void emit_x86_64_generic_object(CodegenContext *context, MIRFunctionVector machine_instructions) {
  DBGASSERT(context, "Invalid argument");
  ASSERT(context->object, "Cannot emit into NULL generic object");

  if (context->ast->is_module) {
    string module_cereal = serialise_module(context, context->ast);
    Section sec_module_metadata = {0};
    sec_module_metadata.name = strdup(INTC_MODULE_SECTION_NAME);
    sec_module_metadata.data.bytes.data = (uint8_t*)module_cereal.data;
    sec_module_metadata.data.bytes.size = module_cereal.size;
    sec_module_metadata.data.bytes.capacity = module_cereal.size;
    vector_push(context->object->sections, sec_module_metadata);
  }

  foreach_val (function, machine_instructions) {
    { // Function symbol
      GObjSymbol sym = {0};
      sym.type = !ir_func_is_definition(function->origin) ? GOBJ_SYMTYPE_EXTERNAL : GOBJ_SYMTYPE_FUNCTION;
      sym.name = strdup(function->name.data);
      sym.section_name = strdup(code_section(context->object)->name);
      sym.byte_offset = code_section(context->object)->data.bytes.size;
      vector_push(context->object->symbols, sym);
    }
    if (function->origin && !ir_func_is_definition(function->origin)) continue;

    // Calculate stack offsets, frame size
    isz frame_offset = 0;
    isz frame_size = 0;
    foreach (fo, function->frame_objects) {
      frame_size += (isz) fo->size;
      frame_offset -= (isz) fo->size;
      fo->offset = frame_offset;
    }

    STATIC_ASSERT(FRAME_COUNT == 3, "Exhaustive handling of x86_64 frame kinds");
    StackFrameKind frame_kind = stack_frame_kind(function);
    switch (frame_kind) {
    case FRAME_NONE: break;

    case FRAME_MINIMAL: {
      mcode_imm_to_reg(context, MX64_SUB, ALIGN_TO(frame_size, 16) + 8, REG_RSP, r64);
    } break;

    case FRAME_FULL: {
      // PUSH %RBP
      // MOV %RSP, %RBP
      mcode_reg(context, MX64_PUSH, REG_RBP, r64);
      mcode_reg_to_reg(context, MX64_MOV, REG_RSP, r64, REG_RBP, r64);
      if (frame_size) mcode_imm_to_reg(context, MX64_SUB, ALIGN_TO(frame_size, 16), REG_RSP, r64);
    } break;

    case FRAME_COUNT: FALLTHROUGH;
    default: UNREACHABLE();

    }

    foreach_index (block_index, function->blocks) {
      MIRBlock* block = function->blocks.data[block_index];
      { // Block label symbol
        GObjSymbol sym = {0};
        sym.type = GOBJ_SYMTYPE_STATIC;
        sym.name = strdup(block->name.data);
        sym.section_name = strdup(code_section(context->object)->name);
        sym.byte_offset = code_section(context->object)->data.bytes.size;
        vector_push(context->object->symbols, sym);
      }
      foreach_val (instruction, block->instructions) {
        if (instruction->opcode < MX64_START) {
          eprint("\n\n%31UNLOWERED INSTRUCTION:%m\n");
          print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
          ICE("It seems instruction selection has not lowered a general MIR instruction");
        }
        switch ((MIROpcodex86_64)instruction->opcode) {
        default: {
          print("Unhandled opcode (mcode): %u (%s)\n", instruction->opcode, mir_x86_64_opcode_mnemonic(instruction->opcode));
        } break;

        case MX64_IMUL: {
          // TODO: Three address versions of imul.
          if (mir_operand_kinds_match(instruction, 2, MIR_OP_IMMEDIATE, MIR_OP_REGISTER)) {
            // imm to reg | imm, dst
            MIROperand *imm = mir_get_op(instruction, 0);
            MIROperand *reg = mir_get_op(instruction, 1);
            if (!reg->value.reg.size) {
              putchar('\n');
              print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
              print("%35WARNING%m: Zero sized register, assuming 64-bit...\n\n");
              reg->value.reg.size = r64;
            }
            mcode_imm_to_reg(context, instruction->opcode, imm->value.imm, reg->value.reg.value, reg->value.reg.size);
          } else if (mir_operand_kinds_match(instruction, 2, MIR_OP_REGISTER, MIR_OP_REGISTER)) {
            // reg to reg | src, dst
            MIROperand *src = mir_get_op(instruction, 0);
            MIROperand *dst = mir_get_op(instruction, 1);
            mcode_reg_to_reg(context, instruction->opcode, src->value.reg.value, src->value.reg.size, dst->value.reg.value, dst->value.reg.size);
          } else {
            print("\n\nUNHANDLED INSTRUCTION:\n");
            print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
            ICE("[x86_64/CodeEmission]: Unhandled instruction, sorry");
          }
        } break; // case MX64_IMUL

        case MX64_NOT: FALLTHROUGH;
        case MX64_DIV: FALLTHROUGH;
        case MX64_IDIV: {
          if (mir_operand_kinds_match(instruction, 1, MIR_OP_REGISTER)) {
            MIROperand *reg = mir_get_op(instruction, 0);
            mcode_reg(context, instruction->opcode, reg->value.reg.value, reg->value.reg.size);
          } else {
            print("\n\nUNHANDLED INSTRUCTION:\n");
            print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
            ICE("[x86_64/CodeEmission]: Unhandled instruction, sorry");
          }
        } break; // case MX64_IDIV

        case MX64_AND: FALLTHROUGH;
        case MX64_OR: FALLTHROUGH;
        case MX64_ADD: FALLTHROUGH;
        case MX64_SUB: {
          if (mir_operand_kinds_match(instruction, 2, MIR_OP_IMMEDIATE, MIR_OP_REGISTER)) {
            // imm to reg | imm, dst
            MIROperand *imm = mir_get_op(instruction, 0);
            MIROperand *reg = mir_get_op(instruction, 1);
            if (!reg->value.reg.size) {
              putchar('\n');
              print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
              print("%35WARNING%m: Zero sized register, assuming 64-bit...\n\n");
              reg->value.reg.size = r64;
            }
            mcode_imm_to_reg(context, instruction->opcode, imm->value.imm, reg->value.reg.value, reg->value.reg.size);
          } else if (mir_operand_kinds_match(instruction, 2, MIR_OP_REGISTER, MIR_OP_REGISTER)) {
            // reg to reg | src, dst
            MIROperand *src = mir_get_op(instruction, 0);
            MIROperand *dst = mir_get_op(instruction, 1);
            mcode_reg_to_reg(context, instruction->opcode, src->value.reg.value, src->value.reg.size, dst->value.reg.value, dst->value.reg.size);
          } else if (mir_operand_kinds_match(instruction, 4, MIR_OP_IMMEDIATE, MIR_OP_REGISTER, MIR_OP_IMMEDIATE, MIR_OP_IMMEDIATE)) {
            // imm to mem | imm, address, offset, size
            MIROperand *imm = mir_get_op(instruction, 0);
            MIROperand *addr = mir_get_op(instruction, 1);
            MIROperand *offset = mir_get_op(instruction, 2);
            MIROperand *size = mir_get_op(instruction, 3);
            mcode_imm_to_mem(context, instruction->opcode, imm->value.imm, addr->value.reg.value, offset->value.imm, (RegSize)size->value.imm);
          } else {
            print("\n\nUNHANDLED INSTRUCTION:\n");
            print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
            ICE("[x86_64/CodeEmission]: Unhandled instruction, sorry");
          }
        } break; // case MX64_ADD

        case MX64_MOV: {
          if (mir_operand_kinds_match(instruction, 2, MIR_OP_IMMEDIATE, MIR_OP_REGISTER)) {
            // imm to reg | imm, dst
            MIROperand *imm = mir_get_op(instruction, 0);
            MIROperand *reg = mir_get_op(instruction, 1);
            if (!reg->value.reg.size) {
              putchar('\n');
              print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
              print("%35WARNING%m: Zero sized register, assuming 64-bit...\n");
              putchar('\n');
              reg->value.reg.size = r64;
            }
            mcode_imm_to_reg(context, MX64_MOV, imm->value.imm, reg->value.reg.value, reg->value.reg.size);
          } else if (mir_operand_kinds_match(instruction, 2, MIR_OP_IMMEDIATE, MIR_OP_LOCAL_REF)) {
            // imm to mem (local) | imm, local
            MIROperand *imm = mir_get_op(instruction, 0);
            MIROperand *local = mir_get_op(instruction, 1);
            ASSERT(local->value.local_ref < function->frame_objects.size,
                   "MX64_MOV(imm, local): local index %d is greater than amount of frame objects in function: %Z",
                   (int)local->value.local_ref, function->frame_objects.size);
            MIRFrameObject *fo = function->frame_objects.data + local->value.local_ref;
            mcode_imm_to_mem(context, MX64_MOV, imm->value.imm, REG_RBP, fo->offset, (RegSize)fo->size);
          } else if (mir_operand_kinds_match(instruction, 2, MIR_OP_IMMEDIATE, MIR_OP_STATIC_REF)) {
            // imm to mem (static) | imm, static
            MIROperand *imm = mir_get_op(instruction, 0);
            MIROperand *stc = mir_get_op(instruction, 1);
            mcode_imm_to_offset_name(context, MX64_MOV,
                                     imm->value.imm, (RegSize)type_sizeof(ir_static_ref_var(stc->value.static_ref)->type),
                                     REG_RIP, ir_static_ref_var(stc->value.static_ref)->name.data, 0);
          } else if (mir_operand_kinds_match(instruction, 2, MIR_OP_REGISTER, MIR_OP_REGISTER)) {
            // reg to reg | src, dst
            MIROperand *src = mir_get_op(instruction, 0);
            MIROperand *dst = mir_get_op(instruction, 1);
            if (dst->value.reg.size == r8 || dst->value.reg.size == r16)
              mcode_imm_to_reg(context, MX64_MOV, 0, dst->value.reg.value, r32);
            mcode_reg_to_reg(context, MX64_MOV, src->value.reg.value, src->value.reg.size, dst->value.reg.value, dst->value.reg.size);
          } else if (mir_operand_kinds_match(instruction, 2, MIR_OP_REGISTER, MIR_OP_STATIC_REF)) {
            // reg to mem (static) | src, static
            MIROperand *reg = mir_get_op(instruction, 0);
            MIROperand *stc = mir_get_op(instruction, 1);
            if (!reg->value.reg.size) {
              putchar('\n');
              print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
              print("%35WARNING%m: Zero sized register, assuming 64-bit...\n");
              putchar('\n');
              reg->value.reg.size = r64;
            }
            mcode_reg_to_name(context, MX64_MOV, reg->value.reg.value, reg->value.reg.size,
                              REG_RIP, ir_static_ref_var(stc->value.static_ref)->name.data);
          } else if (mir_operand_kinds_match(instruction, 2, MIR_OP_REGISTER, MIR_OP_LOCAL_REF)) {
            // reg to mem (local) | src, local
            MIROperand *reg = mir_get_op(instruction, 0);
            MIROperand *local = mir_get_op(instruction, 1);

            ASSERT(function->frame_objects.size,
                   "Cannot reference local at index %Z when there are no frame objects in this function",
                   local->value.local_ref);
            ASSERT(local->value.local_ref < function->frame_objects.size,
                   "Local reference index %Z is larger than maximum possible local index %Z",
                   local->value.local_ref, function->frame_objects.size - 1);

            if (!reg->value.reg.size) {
              putchar('\n');
              print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
              print("%35WARNING%m: Zero sized register, assuming 64-bit...\n\n");
              reg->value.reg.size = r64;
            }

            mcode_reg_to_mem(context, MX64_MOV, reg->value.reg.value, reg->value.reg.size,
                             REG_RBP, function->frame_objects.data[local->value.local_ref].offset);
          } else if (mir_operand_kinds_match(instruction, 3, MIR_OP_IMMEDIATE, MIR_OP_REGISTER, MIR_OP_IMMEDIATE)) {
            TODO("MOV(IMM, REG, IMM) would normally be in the 'imm to mem' form, but an extra size operand is required (how many bytes to store)");
          } else if (mir_operand_kinds_match(instruction, 4, MIR_OP_IMMEDIATE, MIR_OP_REGISTER, MIR_OP_IMMEDIATE, MIR_OP_IMMEDIATE)) {
            // imm to mem | imm, addr, offset, size
            MIROperand *imm = mir_get_op(instruction, 0);
            MIROperand *reg_address = mir_get_op(instruction, 1);
            MIROperand *offset = mir_get_op(instruction, 2);
            MIROperand *size = mir_get_op(instruction, 3);
            mcode_imm_to_mem(context, MX64_MOV, imm->value.imm, reg_address->value.reg.value, offset->value.imm, (RegSize)size->value.imm);
          } else if (mir_operand_kinds_match(instruction, 3, MIR_OP_REGISTER, MIR_OP_REGISTER, MIR_OP_IMMEDIATE)) {
            // reg to mem | src, addr, offset
            MIROperand *reg_source = mir_get_op(instruction, 0);
            MIROperand *reg_address = mir_get_op(instruction, 1);
            MIROperand *offset = mir_get_op(instruction, 2);
            mcode_reg_to_mem(context, MX64_MOV, reg_source->value.reg.value, reg_source->value.reg.size, reg_address->value.reg.value, offset->value.imm);
          } else if (mir_operand_kinds_match(instruction, 3, MIR_OP_REGISTER, MIR_OP_IMMEDIATE, MIR_OP_REGISTER)) {
            TODO("MOV(REG, IMM, REG) would normally be in the 'mem to reg' form, but an extra size operand is required (how many bytes to store)");
          } else if (mir_operand_kinds_match(instruction, 4, MIR_OP_REGISTER, MIR_OP_IMMEDIATE, MIR_OP_REGISTER, MIR_OP_IMMEDIATE)) {
            // mem to reg | addr, offset, dst, size
            MIROperand *reg_address = mir_get_op(instruction, 0);
            MIROperand *offset = mir_get_op(instruction, 1);
            MIROperand *reg_dst = mir_get_op(instruction, 2);
            MIROperand *size = mir_get_op(instruction, 3);
            mcode_mem_to_reg(context, MX64_MOV, reg_address->value.reg.value, offset->value.imm, reg_dst->value.reg.value, (RegSize)size->value.imm);
          } else if (mir_operand_kinds_match(instruction, 2, MIR_OP_LOCAL_REF, MIR_OP_REGISTER)) {
            // mem (local) to reg | local, src
            MIROperand *local = mir_get_op(instruction, 0);
            MIROperand *reg = mir_get_op(instruction, 1);

            ASSERT(function->frame_objects.size,
                   "Cannot reference local at index %Z when there are no frame objects in this function",
                   local->value.local_ref);
            ASSERT(local->value.local_ref < function->frame_objects.size,
                   "Local reference index %Z is larger than maximum possible local index %Z",
                   local->value.local_ref, function->frame_objects.size - 1);

            mcode_mem_to_reg(context, MX64_MOV,
                             REG_RBP, function->frame_objects.data[local->value.local_ref].offset,
                             reg->value.reg.value, reg->value.reg.size);
          } else if (mir_operand_kinds_match(instruction, 2, MIR_OP_STATIC_REF, MIR_OP_REGISTER)) {
            // mem (static) to reg | static, dst
            MIROperand *stc = mir_get_op(instruction, 0);
            MIROperand *dst = mir_get_op(instruction, 1);
            mcode_name_to_reg(context, MX64_MOV, REG_RIP, ir_static_ref_var(stc->value.static_ref)->name.data, dst->value.reg.value, dst->value.reg.size);
          } else {
            print("\n\nUNHANDLED INSTRUCTION:\n");
            print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
            ICE("[x86_64/CodeEmission]: Unhandled instruction, sorry");
          }

        } break; // case MX64_MOV

        case MX64_CALL: {
          MIROperand *dst = mir_get_op(instruction, 0);

          switch (dst->kind) {

          case MIR_OP_REGISTER: {
            mcode_indirect_branch(context, MX64_CALL, dst->value.reg.value);
          } break;
          case MIR_OP_NAME: {
            mcode_name(context, MX64_CALL, dst->value.name, false);
          } break;
          case MIR_OP_BLOCK: {
            mcode_name(context, MX64_CALL, dst->value.block->name.data, false);
          } break;
          case MIR_OP_FUNCTION: {
            mcode_name(context, MX64_CALL, dst->value.function->name.data, true);
          } break;

          default: ICE("Unhandled operand kind in CALL: %d (%s)", dst->kind, mir_operand_kind_string(dst->kind));

          } // switch (dst->kind)

        } break;

        case MX64_RET: {

          STATIC_ASSERT(FRAME_COUNT == 3, "Exhaustive handling of x86_64 frame kinds");
          switch (frame_kind) {
          case FRAME_NONE: break;

          case FRAME_FULL: {
            // MOV %RBP, %RSP
            // POP %RBP
            mcode_reg_to_reg(context, MX64_MOV, REG_RBP, r64, REG_RSP, r64);
            mcode_reg(context, MX64_POP, REG_RBP, r64);
          } break;

          case FRAME_MINIMAL: {
            mcode_imm_to_reg(context, MX64_ADD, ALIGN_TO(frame_size, 16) + 8, REG_RSP, r64);
          } break;

          case FRAME_COUNT: FALLTHROUGH;
          default: UNREACHABLE();

          }

          mcode_none(context, MX64_RET);

        } break;

        case MX64_SHL: FALLTHROUGH;
        case MX64_SAR: FALLTHROUGH;
        case MX64_SHR: {
          if (mir_operand_kinds_match(instruction, 1, MIR_OP_REGISTER)) {
            MIROperand *reg = mir_get_op(instruction, 0);
            mcode_reg(context, instruction->opcode, reg->value.reg.value, reg->value.reg.size);
          } else {
            print("\n\nUNHANDLED INSTRUCTION:\n");
            print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
            ICE("[x86_64/CodeEmission]: Unhandled instruction, sorry");
          }
        } break;

        case MX64_POP: FALLTHROUGH;
        case MX64_PUSH: {
          if (mir_operand_kinds_match(instruction, 1, MIR_OP_REGISTER)) {
            MIROperand *reg = mir_get_op(instruction, 0);
            mcode_reg(context, instruction->opcode, reg->value.reg.value, reg->value.reg.size);
          } else {
            print("\n\nUNHANDLED INSTRUCTION:\n");
            print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
            ICE("[x86_64/CodeEmission]: Unhandled instruction, sorry");
          }
        } break;

        case MX64_LEA: {
          if (mir_operand_kinds_match(instruction, 2, MIR_OP_LOCAL_REF, MIR_OP_REGISTER)) {
            MIROperand *local = mir_get_op(instruction, 0);
            MIROperand *reg = mir_get_op(instruction, 1);
            if (!reg->value.reg.size) {
              putchar('\n');
              print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
              print("%35WARNING%m: Zero sized register, assuming 64-bit...\n");
              putchar('\n');
              reg->value.reg.size = r64;
            }
            mcode_mem_to_reg(context, MX64_LEA, REG_RBP, function->frame_objects.data[local->value.local_ref].offset, reg->value.reg.value, reg->value.reg.size);
          } else if (mir_operand_kinds_match(instruction, 2, MIR_OP_STATIC_REF, MIR_OP_REGISTER)) {
            MIROperand *object = mir_get_op(instruction, 0);
            MIROperand *reg = mir_get_op(instruction, 1);
            if (!reg->value.reg.size) {
              putchar('\n');
              print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
              print("%35WARNING%m: Zero sized register, assuming 64-bit...\n");
              putchar('\n');
              reg->value.reg.size = r64;
            }
            if (reg->value.reg.size == r8 || reg->value.reg.size == r16)
              mcode_imm_to_reg(context, MX64_MOV, 0, reg->value.reg.value, r32);
            mcode_name_to_reg(context, MX64_LEA, REG_RIP, ir_static_ref_var(object->value.static_ref)->name.data, reg->value.reg.value, reg->value.reg.size);
          } else if (mir_operand_kinds_match(instruction, 2, MIR_OP_FUNCTION, MIR_OP_REGISTER)) {
            MIROperand *f = mir_get_op(instruction, 0);
            MIROperand *reg = mir_get_op(instruction, 1);
            if (!reg->value.reg.size) {
              putchar('\n');
              print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
              print("%35WARNING%m: Zero sized register, assuming 64-bit...\n");
              putchar('\n');
              reg->value.reg.size = r64;
            }
            if (reg->value.reg.size == r8 || reg->value.reg.size == r16)
              mcode_imm_to_reg(context, MX64_MOV, 0, reg->value.reg.value, r32);
            mcode_name_to_reg(context, MX64_LEA, REG_RIP, f->value.function->name.data, reg->value.reg.value, reg->value.reg.size);
          } else {
            print("\n\nUNHANDLED INSTRUCTION:\n");
            print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
            ICE("[x86_64/CodeEmission]: Unhandled instruction, sorry");
          }
        } break;

        case MX64_JMP: {
          if (mir_operand_kinds_match(instruction, 1, MIR_OP_BLOCK)) {
            MIROperand *destination = mir_get_op(instruction, 0);
            mcode_name(context, MX64_JMP, destination->value.block->name.data, false);
          } else if (mir_operand_kinds_match(instruction, 1, MIR_OP_FUNCTION)) {
            MIROperand *destination = mir_get_op(instruction, 0);
            mcode_name(context, MX64_JMP, destination->value.function->name.data, true);
          } else if (mir_operand_kinds_match(instruction, 1, MIR_OP_NAME)) {
            MIROperand *destination = mir_get_op(instruction, 0);
            mcode_name(context, MX64_JMP, destination->value.name, false);
          } else {
            print("\n\nUNHANDLED INSTRUCTION:\n");
            print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
            ICE("[x86_64/CodeEmission]: Unhandled instruction, sorry");
          }
        } break;

        case MX64_CMP: FALLTHROUGH;
        case MX64_TEST: {
          if (mir_operand_kinds_match(instruction, 2, MIR_OP_REGISTER, MIR_OP_REGISTER)) {
            MIROperand *lhs = mir_get_op(instruction, 0);
            MIROperand *rhs = mir_get_op(instruction, 1);
            if (!lhs->value.reg.size) {
              putchar('\n');
              print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
              print("%35WARNING%m: Zero sized register, assuming 64-bit...\n\n");
              lhs->value.reg.size = r64;
            }
            if (!rhs->value.reg.size) {
              putchar('\n');
              print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
              print("%35WARNING%m: Zero sized register, assuming 64-bit...\n\n");
              rhs->value.reg.size = r64;
            }
            mcode_reg_to_reg(context, instruction->opcode, lhs->value.reg.value, lhs->value.reg.size, rhs->value.reg.value, rhs->value.reg.size);
          } else if (mir_operand_kinds_match(instruction, 2, MIR_OP_IMMEDIATE, MIR_OP_REGISTER)) {
            MIROperand *imm = mir_get_op(instruction, 0);
            MIROperand *rhs = mir_get_op(instruction, 1);
            if (!rhs->value.reg.size) {
              putchar('\n');
              print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
              print("%35WARNING%m: Zero sized register, assuming 64-bit...\n\n");
              rhs->value.reg.size = r64;
            }
            mcode_imm_to_reg(context, instruction->opcode, imm->value.imm, rhs->value.reg.value, rhs->value.reg.size);
          } else {
            print("\n\nUNHANDLED INSTRUCTION:\n");
            print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
            ICE("[x86_64/CodeEmission]: Unhandled instruction, sorry");
          }
        } break;

        case MX64_SETCC: {
          if (mir_operand_kinds_match(instruction, 2, MIR_OP_IMMEDIATE, MIR_OP_REGISTER)) {
            MIROperand *compare_type = mir_get_op(instruction, 0);
            MIROperand *destination = mir_get_op(instruction, 1);
            ASSERT(compare_type->value.imm < COMPARE_COUNT, "Invalid compare type for setcc: %I", compare_type->value.imm);
            mcode_setcc(context, (enum ComparisonType)compare_type->value.imm, destination->value.reg.value);
          } else {
            print("\n\nUNHANDLED INSTRUCTION:\n");
            print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
            ICE("[x86_64/CodeEmission]: Unhandled instruction, sorry");
          }
        } break;

        case MX64_SYSCALL:
        case MX64_UD2:
        case MX64_INT3:
        case MX64_CWD:
        case MX64_CDQ:
        case MX64_CQO: {
          mcode_none(context, (MIROpcodex86_64)instruction->opcode);
        } break;

        case MX64_JCC: {
          if (mir_operand_kinds_match(instruction, 2, MIR_OP_IMMEDIATE, MIR_OP_BLOCK)) {
            MIROperand *jump_type = mir_get_op(instruction, 0);
            MIROperand *destination = mir_get_op(instruction, 1);
            ASSERT(jump_type->value.imm < JUMP_TYPE_COUNT, "Invalid jump type for jcc: %I", jump_type->value.imm);
            mcode_jcc(context, (IndirectJumpType)jump_type->value.imm, destination->value.block->name.data, false);
          } else {
            print("\n\nUNHANDLED INSTRUCTION:\n");
            print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
            ICE("[x86_64/CodeEmission]: Unhandled instruction, sorry");
          }
        } break;

        case MX64_MOVSX: FALLTHROUGH;
        case MX64_MOVZX: {
          if (mir_operand_kinds_match(instruction, 2, MIR_OP_REGISTER, MIR_OP_REGISTER)) {
            MIROperand *src = mir_get_op(instruction, 0);
            MIROperand *dst = mir_get_op(instruction, 1);
            mcode_reg_to_reg(context, instruction->opcode, src->value.reg.value, src->value.reg.size, dst->value.reg.value, dst->value.reg.size);
          } else {
            print("\n\nUNHANDLED INSTRUCTION:\n");
            print_mir_instruction_with_mnemonic(instruction, mir_x86_64_opcode_mnemonic);
            ICE("[x86_64/CodeEmission]: Unhandled instruction, sorry");
          }
        } break; // case MX64_MOVZX

        case MX64_XOR: FALLTHROUGH;
        case MX64_XCHG:
          TODO("Implement machine code emission from opcode %d (%s)", instruction->opcode, mir_x86_64_opcode_mnemonic(instruction->opcode));

        case MX64_START: FALLTHROUGH;
        case MX64_END: FALLTHROUGH;
        case MX64_COUNT: UNREACHABLE();

        } // switch (instruction->opcode)
      }
    }
  }

  // Resolve local label (".Lxxxx") relocations.
  Vector(size_t) relocations_to_remove = {0};
  foreach_index (idx, context->object->relocs) {
    RelocationEntry *reloc = context->object->relocs.data + idx;
    GObjSymbol *sym = &reloc->sym;
    if (strlen(sym->name) > 2 && memcmp(sym->name, ".L", 2) == 0) {
      // We have to go sym->byte_offset bytes into the ByteBuffer of
      // the code section and then fill in the next bytes depending on the
      // relocation type.
      GObjSymbol *label_sym = NULL;
      foreach (s, context->object->symbols) {
        if (strcmp(s->name, sym->name) == 0) {
          label_sym = s;
          break;
        }
      }
      if (!label_sym) ICE("Could not find local label referenced by relocation: \"%s\"", sym->name);

      // TODO: Handle endianess
      // NOTE: 4 == sizeof relocation displacement
      int32_t disp32 = (int32_t)label_sym->byte_offset - (4 + (int32_t)sym->byte_offset);
      uint8_t *src_it = (uint8_t*)&disp32;
      uint8_t *dst_it = code_section(context->object)->data.bytes.data + sym->byte_offset;
      for (int i = 0; i < 4; ++i)
        *dst_it++ = *src_it++;

      vector_push(relocations_to_remove, idx);
    }
  }
  foreach_rev (idx, relocations_to_remove) {
    vector_remove_index(context->object->relocs, *idx);
  }

  // Remove all local label symbols (".Lxxxx")
  Vector(size_t) symbols_to_remove = {0};
  foreach_index (idx, context->object->symbols) {
    GObjSymbol *sym = context->object->symbols.data + idx;
    if (strlen(sym->name) > 2 && memcmp(sym->name, ".L", 2) == 0)
      vector_push(symbols_to_remove, idx);
  }
  foreach_rev (idx, symbols_to_remove) {
    vector_remove_index(context->object->symbols, *idx);
  }
}

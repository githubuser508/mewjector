/*
 * ============================================================================
 *  mj_lde.h: x86-64 length disassembler for Mewgenics function prologues
 * ============================================================================
 *
 *  Recognizes the instruction forms that appear in the first 14 bytes of any
 *  Mewgenics.exe function entry, as enumerated by EnumeratePrologues.java.
 *
 *  Coverage: 172 opcode bytes (97 one-byte plus 75 0F-extended), drawn from
 *  MSVC-style function prologues. Opcodes outside that set return 0, which
 *  the chainloader's MJ_CreateSite treats as a refusal: the caller must
 *  supply explicit stolenBytes for that hook site.
 *
 *  Public API:
 *      int mj_lde(const uint8_t* p);
 *      Returns the byte length of the instruction starting at p, or 0 on
 *      failure (unknown opcode, malformed encoding, VEX/EVEX, etc.).
 *
 *      mj_lde_info mj_lde_ex(const uint8_t* p);
 *      Same as mj_lde, but also reports the position of any RIP-relative
 *      disp32 within the instruction (rip_rel_disp_offset, in bytes from
 *      the start of the instruction, or -1 if the instruction has no
 *      RIP-relative encoding). Used by the chainloader's stolen-byte
 *      fixup pass to relocate ModR/M [rip+disp32] operands. This includes
 *      the indirect-call (FF 15 ..) and indirect-jmp (FF 25 ..) encodings,
 *      which are just FF with mod=00, rm=101.
 *
 *  Single-header, no dependencies, safe to include from C and C++.
 * ============================================================================
 */
#ifndef MJ_LDE_H
#define MJ_LDE_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 *  Opcode-table flag layout
 * ---------------------------------------------------------------------------
 *
 *  Lower 4 bits encode the BASE immediate size in bytes (0, 1, 2, or 4).
 *  Upper bits are flags that adjust that size or signal special handling.
 */

#define MJ_LDE_M    0x10  /* Instruction has a ModR/M byte                   */
#define MJ_LDE_O    0x20  /* Operand-size override applies: 66 prefix shrinks
                             a base-4 immediate to 2 bytes                   */
#define MJ_LDE_W    0x40  /* REX.W extends the immediate from 4 to 8 bytes
                             (only used by the B8..BF MOV r,imm range)       */
#define MJ_LDE_G3   0x80  /* Group-3 opcode (F6/F7): the immediate exists
                             only when ModR/M.reg is 0 (TEST) or 1           */

#define MJ_LDE_UU   0xFF  /* Sentinel: opcode not covered by these tables    */

/* ---------------------------------------------------------------------------
 *  One-byte opcode table
 *  Indexed by the primary opcode after legacy prefixes and the optional REX.
 * --------------------------------------------------------------------------- */

static const uint8_t mj_lde_op1[256] = {
    /*       00    01    02    03    04    05    06    07            */
    /* 00 */ 0xFF, 0x10, 0xFF, 0x10, 0x01, 0x24, 0xFF, 0xFF,
    /* 08 */ 0xFF, 0x10, 0x10, 0x10, 0x01, 0x24, 0xFF, 0xFF,
    /* 10 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 18 */ 0xFF, 0xFF, 0xFF, 0x10, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 20 */ 0xFF, 0x10, 0xFF, 0x10, 0x01, 0x24, 0xFF, 0xFF,
    /* 28 */ 0xFF, 0x10, 0xFF, 0x10, 0x01, 0x24, 0xFF, 0xFF,
    /* 30 */ 0xFF, 0xFF, 0x10, 0x10, 0x01, 0xFF, 0xFF, 0xFF,
    /* 38 */ 0x10, 0x10, 0x10, 0x10, 0x01, 0x24, 0xFF, 0xFF,
    /* 40 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 48 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 50 */ 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 58 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF,
    /* 60 */ 0xFF, 0xFF, 0xFF, 0x10, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 68 */ 0xFF, 0x34, 0xFF, 0x11, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 70 */ 0xFF, 0xFF, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    /* 78 */ 0x01, 0x01, 0x01, 0xFF, 0x01, 0x01, 0x01, 0x01,
    /* 80 */ 0x11, 0x34, 0xFF, 0x11, 0x10, 0x10, 0xFF, 0x10,
    /* 88 */ 0x10, 0x10, 0x10, 0x10, 0xFF, 0x10, 0xFF, 0xFF,
    /* 90 */ 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 98 */ 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* A0 */ 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF,
    /* A8 */ 0x01, 0x24, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* B0 */ 0x01, 0x01, 0x01, 0x01, 0xFF, 0xFF, 0x01, 0x01,
    /* B8 */ 0x64, 0x64, 0x64, 0x64, 0x64, 0x64, 0x64, 0x64,
    /* C0 */ 0x11, 0x11, 0x02, 0x00, 0xFF, 0xFF, 0x11, 0x34,
    /* C8 */ 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF,
    /* D0 */ 0xFF, 0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* D8 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* E0 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* E8 */ 0x04, 0x04, 0xFF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,
    /* F0 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x91, 0xB4,
    /* F8 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x10, 0x10,
};

/* ---------------------------------------------------------------------------
 *  0F-extended opcode table
 *  Indexed by the second opcode byte (after the 0x0F escape).
 * --------------------------------------------------------------------------- */

static const uint8_t mj_lde_op0F[256] = {
    /*       00    01    02    03    04    05    06    07            */
    /* 00 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 08 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x10, 0xFF, 0xFF,
    /* 10 */ 0x10, 0x10, 0xFF, 0xFF, 0x10, 0x10, 0xFF, 0xFF,
    /* 18 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x10,
    /* 20 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 28 */ 0x10, 0x10, 0x10, 0xFF, 0x10, 0xFF, 0x10, 0x10,
    /* 30 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 38 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 40 */ 0xFF, 0xFF, 0x10, 0xFF, 0x10, 0x10, 0xFF, 0x10,
    /* 48 */ 0x10, 0xFF, 0xFF, 0xFF, 0x10, 0xFF, 0x10, 0x10,
    /* 50 */ 0xFF, 0x10, 0xFF, 0xFF, 0x10, 0xFF, 0xFF, 0x10,
    /* 58 */ 0x10, 0x10, 0x10, 0x10, 0x10, 0xFF, 0x10, 0x10,
    /* 60 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 68 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x10, 0x10,
    /* 70 */ 0x11, 0xFF, 0x11, 0xFF, 0xFF, 0xFF, 0x10, 0xFF,
    /* 78 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x10, 0x10,
    /* 80 */ 0xFF, 0xFF, 0xFF, 0x04, 0x04, 0x04, 0x04, 0x04,
    /* 88 */ 0x04, 0x04, 0xFF, 0xFF, 0x04, 0x04, 0x04, 0x04,
    /* 90 */ 0xFF, 0xFF, 0xFF, 0xFF, 0x10, 0x10, 0x10, 0x10,
    /* 98 */ 0xFF, 0x10, 0x10, 0xFF, 0xFF, 0x10, 0x10, 0x10,
    /* A0 */ 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* A8 */ 0xFF, 0xFF, 0xFF, 0x10, 0xFF, 0xFF, 0x10, 0x10,
    /* B0 */ 0xFF, 0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0x10, 0x10,
    /* B8 */ 0xFF, 0xFF, 0x11, 0xFF, 0x10, 0x10, 0x10, 0x10,
    /* C0 */ 0xFF, 0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0x11, 0xFF,
    /* C8 */ 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* D0 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* D8 */ 0xFF, 0xFF, 0xFF, 0x10, 0xFF, 0xFF, 0xFF, 0xFF,
    /* E0 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x10, 0xFF,
    /* E8 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x10,
    /* F0 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* F8 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

/* ---------------------------------------------------------------------------
 *  Length decoder (extended)
 *
 *  mj_lde_info is the extended return value: the instruction length plus
 *  the byte offset of any RIP-relative disp32 within the instruction
 *  (-1 if there is no RIP-relative encoding).  The plain mj_lde() below
 *  is a thin wrapper that returns just the .length field, kept for
 *  callers that don't need the disp offset.
 * --------------------------------------------------------------------------- */

typedef struct {
    int length;               /* instruction length in bytes, or 0 on failure */
    int rip_rel_disp_offset;  /* byte offset of disp32 within the instruction,
                                 or -1 if the instruction has no RIP-relative
                                 encoding. RIP-relative encoding is exactly
                                 ModR/M with mod==00 and rm==101, which covers
                                 LEA/MOV [rip+d], FF 15 [rip+d] (call indirect),
                                 FF 25 [rip+d] (jmp indirect), and similar.    */
} mj_lde_info;

static mj_lde_info mj_lde_ex(const uint8_t* p)
{
    mj_lde_info info;
    info.length              = 0;
    info.rip_rel_disp_offset = -1;

    const uint8_t* start = p;
    int operand_size_16 = 0;  /* set if a 66 prefix was consumed */
    int rex_w           = 0;  /* set if a REX byte with W bit was consumed */
    int prefix_loops    = 0;
    uint8_t op;
    uint8_t entry;
    int imm_size;

    /* ----- 1. Legacy prefixes -----------------------------------------
     * Mewgenics function prologues only ever use these five legacy
     * prefixes. We loop because the corpus contains a few doubled-66
     * "long NOP" forms (66 66 ..). Cap the loop to keep length walks
     * bounded.                                                          */
    while (prefix_loops < 4) {
        uint8_t b = *p;
        if      (b == 0x66) { operand_size_16 = 1; p++; }
        else if (b == 0xF0) { p++; }
        else if (b == 0xF2) { p++; }
        else if (b == 0xF3) { p++; }
        else if (b == 0x65) { p++; }
        else                { break; }
        prefix_loops++;
    }

    /* ----- 2. Optional REX prefix ------------------------------------- */
    if ((*p & 0xF0) == 0x40) {
        if (*p & 0x08) rex_w = 1;
        p++;
    }

    /* ----- 3. Opcode (1 or 2 bytes) ----------------------------------- */
    op = *p++;
    if (op == 0x0F) {
        /* Two-byte escape. We do not handle 0F 38 / 0F 3A in this scope. */
        op = *p++;
        if (op == 0x38 || op == 0x3A) return info;
        entry = mj_lde_op0F[op];
    } else {
        entry = mj_lde_op1[op];
    }

    if (entry == MJ_LDE_UU) return info;

    imm_size = entry & 0x0F;

    /* ----- 4. ModR/M, SIB, displacement ------------------------------- */
    if (entry & MJ_LDE_M) {
        int     modrm_off = (int)(p - start);
        uint8_t modrm     = *p++;
        uint8_t mod       = (uint8_t)((modrm >> 6) & 3);
        uint8_t reg       = (uint8_t)((modrm >> 3) & 7);
        uint8_t rm        = (uint8_t)( modrm       & 7);

        /* Group 3 (F6/F7): the immediate only exists for /0 (TEST) and
         * /1. Both opcodes carry the MJ_LDE_G3 flag and the table base
         * imm size matches the TEST imm size; we zero it out for any
         * other ModR/M.reg value (NEG/NOT/MUL/IMUL/DIV/IDIV).            */
        if (entry & MJ_LDE_G3) {
            if (reg != 0 && reg != 1) imm_size = 0;
        }

        if (mod != 3) {
            /* Memory operand: handle SIB and displacement. */
            int has_sib = 0;
            int disp    = 0;

            if (rm == 4) {
                has_sib = 1;
            }

            if (mod == 0) {
                if (rm == 5) {
                    /* RIP-relative disp32. The disp32 immediately follows
                     * the ModR/M byte, so its offset within the
                     * instruction is modrm_off + 1. The fixup pass in
                     * MJ_CreateSite uses this to relocate the disp32 so
                     * the trampoline copy resolves to the same absolute
                     * target as the original. Covers LEA/MOV [rip+d] and
                     * FF 15 / FF 25 indirect call/jmp. */
                    disp = 4;
                    info.rip_rel_disp_offset = modrm_off + 1;
                } else if (has_sib) {
                    /* SIB byte present; if base==5 the displacement is
                     * disp32 (no base register).                         */
                    uint8_t sib  = *p;
                    uint8_t base = (uint8_t)(sib & 7);
                    if (base == 5) disp = 4;
                }
            } else if (mod == 1) {
                disp = 1;
            } else if (mod == 2) {
                disp = 4;
            }

            if (has_sib) p++;
            p += disp;
        }
    }

    /* ----- 5. Immediate -----------------------------------------------
     * Operand-size override: a base-4 immediate becomes 2 bytes when the
     * 66 prefix is present. The B8..BF MOV-r-imm range additionally
     * extends to 8 bytes when REX.W is set.                              */
    if (imm_size == 4) {
        if ((entry & MJ_LDE_W) && rex_w) {
            imm_size = 8;
        } else if ((entry & MJ_LDE_O) && operand_size_16) {
            imm_size = 2;
        }
    }
    p += imm_size;

    info.length = (int)(p - start);
    return info;
}

/* Length-only wrapper, preserved for callers that don't care about
 * RIP-relative disp tracking. */
static int mj_lde(const uint8_t* p)
{
    return mj_lde_ex(p).length;
}

#undef MJ_LDE_M
#undef MJ_LDE_O
#undef MJ_LDE_W
#undef MJ_LDE_G3
#undef MJ_LDE_UU

#endif /* MJ_LDE_H */


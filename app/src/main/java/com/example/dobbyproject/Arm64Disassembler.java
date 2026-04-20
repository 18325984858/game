package com.example.dobbyproject;

/**
 * 极简 ARM64 (AArch64) 反汇编器, 用于内存读取页面的 "反汇编显示" 模式.
 *
 * 设计目标:
 *   - 只依赖标准 Java, 不引入 capstone/llvm 等大型依赖
 *   - 每条指令 4 字节, 输出 "ADDR  HH HH HH HH    mnemonic operands"
 *   - 覆盖常见指令类别 (分支/数据处理/Load-Store/系统), 未知一律降级为 ".inst 0x........"
 *
 * 仅支持 arm64-v8a (本项目 abiFilters 中的唯一 ABI).
 */
final class Arm64Disassembler {

    private Arm64Disassembler() {}

    /** 反汇编一段 4 字节对齐的指令流. */
    static String disassemble(byte[] data, long baseAddr) {
        StringBuilder sb = new StringBuilder(data.length * 4);

        // 4 字节对齐前置 padding
        int skip = (int) ((4 - (baseAddr & 3L)) & 3L);
        if (skip != 0) {
            sb.append(String.format("; warning: address 0x%X 未 4 字节对齐, 跳过 %d 字节\n", baseAddr, skip));
        }

        long pc = baseAddr + skip;
        int off = skip;
        while (off + 4 <= data.length) {
            int b0 = data[off]     & 0xFF;
            int b1 = data[off + 1] & 0xFF;
            int b2 = data[off + 2] & 0xFF;
            int b3 = data[off + 3] & 0xFF;
            int insn = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);

            sb.append(String.format("%016X  %02X %02X %02X %02X    ", pc, b0, b1, b2, b3));
            sb.append(decode(insn, pc));
            sb.append('\n');

            off += 4;
            pc  += 4;
        }
        int tail = data.length - off;
        if (tail > 0) {
            sb.append(String.format("; 末尾 %d 字节未对齐, 已忽略\n", tail));
        }
        return sb.toString();
    }

    // ──────────────────────────────────────────────────────────────────────
    // 顶层分发: 按 ARM ARM C4.1 op0 = bits[28:25]
    // ──────────────────────────────────────────────────────────────────────
    private static String decode(int insn, long pc) {
        // 常用 HINT 编码先快速识别
        if (insn == 0xD503201F) return "nop";
        if (insn == 0xD503203F) return "yield";
        if (insn == 0xD503205F) return "wfe";
        if (insn == 0xD503207F) return "wfi";
        if (insn == 0xD503209F) return "sev";
        if (insn == 0xD50320BF) return "sevl";

        int op0 = (insn >>> 25) & 0xF;
        // 100x  数据处理 -- 立即数
        if ((op0 & 0xE) == 0x8) return decodeDpImm(insn, pc);
        // 101x  分支 / 异常 / 系统
        if ((op0 & 0xE) == 0xA) return decodeBranches(insn, pc);
        // x1x0  Load / Store
        if ((op0 & 0x5) == 0x4) return decodeLoadStore(insn);
        // x101  数据处理 -- 寄存器
        if ((op0 & 0x7) == 0x5) return decodeDpReg(insn);
        // x111  浮点 / SIMD
        if ((op0 & 0x7) == 0x7) return ".inst 0x" + hex8(insn) + "  ; fp/simd";
        return ".inst 0x" + hex8(insn);
    }

    // ──────────────────────────────────────────────────────────────────────
    // 数据处理 -- 立即数
    // ──────────────────────────────────────────────────────────────────────
    private static String decodeDpImm(int insn, long pc) {
        int op0 = (insn >>> 23) & 0x3F; // bits[28:23]

        // ADR/ADRP: x 1 0 0 0 0   -> 10000 (op0 == 0b10000 == 0x10)
        if ((op0 & 0x3E) == 0x20) {
            // bits[28:24] = 10000  → op0 bits[28:23] = 10000x
            int op = (insn >>> 31) & 1;            // 0=ADR, 1=ADRP
            int immlo = (insn >>> 29) & 0x3;
            int immhi = (insn >>> 5) & 0x7FFFF;    // 19 bits
            int rd = insn & 0x1F;
            long imm = ((long) immhi << 2) | immlo;
            // 21-bit signed
            if ((imm & (1L << 20)) != 0) imm |= ~((1L << 21) - 1);
            long target;
            if (op == 1) {
                imm <<= 12;
                target = (pc & ~0xFFFL) + imm;
                return String.format("adrp %s, 0x%X", xreg(rd, true), target);
            } else {
                target = pc + imm;
                return String.format("adr  %s, 0x%X", xreg(rd, true), target);
            }
        }

        // 100010 add/sub immediate
        if ((op0 & 0x3E) == 0x22) {
            int sf = (insn >>> 31) & 1;
            int op = (insn >>> 30) & 1; // 0=add, 1=sub
            int s  = (insn >>> 29) & 1;
            int sh = (insn >>> 22) & 1;
            int imm12 = (insn >>> 10) & 0xFFF;
            int rn = (insn >>> 5) & 0x1F;
            int rd = insn & 0x1F;
            long imm = sh != 0 ? ((long) imm12 << 12) : imm12;
            String mnem;
            if (s == 0) mnem = (op == 0) ? "add " : "sub ";
            else        mnem = (op == 0) ? "adds" : "subs";
            // CMP/CMN 别名: SUBS/ADDS 且 Rd==31
            if (s == 1 && rd == 31) {
                String cmpMnem = (op == 0) ? "cmn " : "cmp ";
                return String.format("%s %s, #0x%X", cmpMnem, regSp(rn, sf == 1), imm);
            }
            // MOV (to/from SP) 别名: ADD imm, sh=0, imm12=0, 且 Rd 或 Rn == 31
            if (s == 0 && op == 0 && imm == 0 && (rd == 31 || rn == 31)) {
                return String.format("mov  %s, %s", regSp(rd, sf == 1), regSp(rn, sf == 1));
            }
            return String.format("%s %s, %s, #0x%X",
                    mnem, regSp(rd, sf == 1), regSp(rn, sf == 1), imm);
        }

        // 100100 logical immediate
        if ((op0 & 0x3E) == 0x24) {
            int sf  = (insn >>> 31) & 1;
            int opc = (insn >>> 29) & 0x3;
            int n   = (insn >>> 22) & 1;
            int immr = (insn >>> 16) & 0x3F;
            int imms = (insn >>> 10) & 0x3F;
            int rn  = (insn >>> 5) & 0x1F;
            int rd  = insn & 0x1F;
            Long imm = decodeBitmask(sf == 1, n, imms, immr);
            String mnem;
            switch (opc) {
                case 0: mnem = "and "; break;
                case 1: mnem = "orr "; break;
                case 2: mnem = "eor "; break;
                default: mnem = "ands"; break;
            }
            if (imm == null) return ".inst 0x" + hex8(insn) + "  ; bad bitmask imm";
            // ANDS Rd==31 → TST
            if (opc == 3 && rd == 31) {
                return String.format("tst  %s, #0x%X", reg(rn, sf == 1), imm);
            }
            // ORR Rn==31 → MOV (bitmask immediate)
            if (opc == 1 && rn == 31) {
                return String.format("mov  %s, #0x%X", reg(rd, sf == 1), imm);
            }
            return String.format("%s %s, %s, #0x%X",
                    mnem, regSp(rd, sf == 1), reg(rn, sf == 1), imm);
        }

        // 100101 move wide immediate
        if ((op0 & 0x3E) == 0x25) {
            int sf  = (insn >>> 31) & 1;
            int opc = (insn >>> 29) & 0x3; // 00 MOVN, 10 MOVZ, 11 MOVK
            int hw  = (insn >>> 21) & 0x3;
            int imm16 = (insn >>> 5) & 0xFFFF;
            int rd = insn & 0x1F;
            String mnem;
            switch (opc) {
                case 0: mnem = "movn"; break;
                case 2: mnem = "movz"; break;
                case 3: mnem = "movk"; break;
                default: return ".inst 0x" + hex8(insn);
            }
            int shift = hw * 16;
            if (shift == 0) {
                return String.format("%s %s, #0x%X", mnem, reg(rd, sf == 1), imm16);
            }
            return String.format("%s %s, #0x%X, lsl #%d", mnem, reg(rd, sf == 1), imm16, shift);
        }

        // 100110 / 100111 bitfield / extract — 简化为 .inst
        return ".inst 0x" + hex8(insn) + "  ; dp-imm";
    }

    // ──────────────────────────────────────────────────────────────────────
    // 分支 / 异常生成 / 系统
    // ──────────────────────────────────────────────────────────────────────
    private static String decodeBranches(int insn, long pc) {
        // B / BL: 0/1 00101 imm26
        if (((insn >>> 26) & 0x1F) == 0x05) {
            int op = (insn >>> 31) & 1;
            int imm26 = insn & 0x03FFFFFF;
            long off = imm26;
            if ((off & (1L << 25)) != 0) off |= ~((1L << 26) - 1);
            off <<= 2;
            long target = pc + off;
            return String.format("%s   0x%X", op == 1 ? "bl " : "b  ", target);
        }

        // Compare & branch (imm): sf 011010 op imm19 Rt
        if (((insn >>> 25) & 0x3F) == 0x1A) {
            int sf = (insn >>> 31) & 1;
            int op = (insn >>> 24) & 1;
            int imm19 = (insn >>> 5) & 0x7FFFF;
            int rt = insn & 0x1F;
            long off = imm19;
            if ((off & (1L << 18)) != 0) off |= ~((1L << 19) - 1);
            off <<= 2;
            long target = pc + off;
            return String.format("%s  %s, 0x%X",
                    op == 0 ? "cbz " : "cbnz", reg(rt, sf == 1), target);
        }

        // Test & branch (imm): b5 011011 op b40 imm14 Rt
        if (((insn >>> 25) & 0x3F) == 0x1B) {
            int b5 = (insn >>> 31) & 1;
            int op = (insn >>> 24) & 1;
            int b40 = (insn >>> 19) & 0x1F;
            int imm14 = (insn >>> 5) & 0x3FFF;
            int rt = insn & 0x1F;
            int bit = (b5 << 5) | b40;
            long off = imm14;
            if ((off & (1L << 13)) != 0) off |= ~((1L << 14) - 1);
            off <<= 2;
            long target = pc + off;
            return String.format("%s  %s, #%d, 0x%X",
                    op == 0 ? "tbz " : "tbnz", reg(rt, b5 == 1), bit, target);
        }

        // B.cond: 0101 0100 imm19 0 cond
        if ((insn & 0xFF000010) == 0x54000000) {
            int imm19 = (insn >>> 5) & 0x7FFFF;
            int cond  = insn & 0xF;
            long off = imm19;
            if ((off & (1L << 18)) != 0) off |= ~((1L << 19) - 1);
            off <<= 2;
            long target = pc + off;
            return String.format("b.%s 0x%X", cond(cond), target);
        }

        // 异常: 1101 0100 op 000 imm16 op2 LL
        if ((insn & 0xFF000000) == 0xD4000000) {
            int opc = (insn >>> 21) & 0x7;
            int ll  = insn & 0x3;
            int imm16 = (insn >>> 5) & 0xFFFF;
            String mnem = null;
            if (opc == 0 && ll == 1) mnem = "svc";
            else if (opc == 0 && ll == 2) mnem = "hvc";
            else if (opc == 0 && ll == 3) mnem = "smc";
            else if (opc == 1 && ll == 0) mnem = "brk";
            else if (opc == 2 && ll == 0) mnem = "hlt";
            if (mnem != null) return String.format("%s  #0x%X", mnem, imm16);
        }

        // 系统寄存器/HINT/ISB/DSB 等: 1101 0101 ...
        if ((insn & 0xFFC00000) == 0xD5000000) {
            // 简化处理: msr/mrs/sys 等统一降级
            // ISB: 1101 0101 0000 0011 0011 xxxx 1101 1111
            if ((insn & 0xFFFFF01F) == 0xD50330DF) return "isb";
            // DSB / DMB:  1101 0101 0000 0011 0011 CRm 1[01]1 1111
            if ((insn & 0xFFFFF09F) == 0xD503309F) {
                int crm = (insn >>> 8) & 0xF;
                int op2 = (insn >>> 5) & 0x7;
                String mnem = (op2 == 5) ? "dmb " : "dsb ";
                return String.format("%s %s", mnem, barrierOption(crm));
            }
            return ".inst 0x" + hex8(insn) + "  ; sys";
        }

        // 无条件分支(寄存器): 1101 0110 0 0 opc op2 op3 Rn op4
        if ((insn & 0xFE000000) == 0xD6000000) {
            int opc = (insn >>> 21) & 0xF;
            int rn  = (insn >>> 5) & 0x1F;
            int op4 = insn & 0x1F;
            switch (opc) {
                case 0: return String.format("br   %s", xreg(rn, true));
                case 1: return String.format("blr  %s", xreg(rn, true));
                case 2:
                    if (rn == 30 && op4 == 0) return "ret";
                    return String.format("ret  %s", xreg(rn, true));
                case 4: return "eret";
                case 5: return "drps";
                default: break;
            }
        }

        return ".inst 0x" + hex8(insn) + "  ; branch";
    }

    // ──────────────────────────────────────────────────────────────────────
    // 数据处理 -- 寄存器
    // ──────────────────────────────────────────────────────────────────────
    private static String decodeDpReg(int insn) {
        // Logical (shifted reg): sf opc 01010 shift N Rm imm6 Rn Rd
        if (((insn >>> 24) & 0x1F) == 0x0A) {
            int sf  = (insn >>> 31) & 1;
            int opc = (insn >>> 29) & 0x3;
            int sh  = (insn >>> 22) & 0x3;
            int n   = (insn >>> 21) & 1;
            int rm  = (insn >>> 16) & 0x1F;
            int imm6 = (insn >>> 10) & 0x3F;
            int rn  = (insn >>> 5) & 0x1F;
            int rd  = insn & 0x1F;
            String[] base = {"and", "orr", "eor", "ands"};
            String mnem = base[opc];
            if (n == 1) {
                String[] inv = {"bic", "orn", "eon", "bics"};
                mnem = inv[opc];
            }
            // MOV alias: ORR Rd, XZR, Rm (shift=0, imm6=0, n=0, rn=31, opc=1)
            if (n == 0 && opc == 1 && sh == 0 && imm6 == 0 && rn == 31) {
                return String.format("mov  %s, %s", reg(rd, sf == 1), reg(rm, sf == 1));
            }
            // TST alias: ANDS Rd==31
            if (opc == 3 && rd == 31 && n == 0) {
                if (sh == 0 && imm6 == 0) {
                    return String.format("tst  %s, %s", reg(rn, sf == 1), reg(rm, sf == 1));
                }
            }
            String tail = (sh == 0 && imm6 == 0) ? "" :
                    String.format(", %s #%d", shiftName(sh), imm6);
            return String.format("%s%s %s, %s, %s%s",
                    mnem, mnem.length() < 4 ? " " : "",
                    reg(rd, sf == 1), reg(rn, sf == 1), reg(rm, sf == 1), tail);
        }

        // Add/sub (shifted reg): sf op S 01011 shift 0 Rm imm6 Rn Rd
        if (((insn >>> 24) & 0x1F) == 0x0B && ((insn >>> 21) & 1) == 0) {
            int sf = (insn >>> 31) & 1;
            int op = (insn >>> 30) & 1;
            int s  = (insn >>> 29) & 1;
            int sh = (insn >>> 22) & 0x3;
            int rm = (insn >>> 16) & 0x1F;
            int imm6 = (insn >>> 10) & 0x3F;
            int rn = (insn >>> 5) & 0x1F;
            int rd = insn & 0x1F;
            String mnem;
            if (s == 0) mnem = (op == 0) ? "add " : "sub ";
            else        mnem = (op == 0) ? "adds" : "subs";
            // CMP/CMN aliases: S=1 && Rd==31
            if (s == 1 && rd == 31) {
                String cmpMnem = (op == 0) ? "cmn " : "cmp ";
                String tail = (sh == 0 && imm6 == 0) ? "" :
                        String.format(", %s #%d", shiftName(sh), imm6);
                return String.format("%s %s, %s%s",
                        cmpMnem, reg(rn, sf == 1), reg(rm, sf == 1), tail);
            }
            // NEG alias: SUB(S) Rd, XZR, Rm
            if (op == 1 && rn == 31) {
                String nmnem = (s == 0) ? "neg " : "negs";
                String tail = (sh == 0 && imm6 == 0) ? "" :
                        String.format(", %s #%d", shiftName(sh), imm6);
                return String.format("%s %s, %s%s",
                        nmnem, reg(rd, sf == 1), reg(rm, sf == 1), tail);
            }
            String tail = (sh == 0 && imm6 == 0) ? "" :
                    String.format(", %s #%d", shiftName(sh), imm6);
            return String.format("%s %s, %s, %s%s",
                    mnem, reg(rd, sf == 1), reg(rn, sf == 1), reg(rm, sf == 1), tail);
        }

        // Conditional select: sf op S 11010100 Rm cond op2 Rn Rd
        if (((insn >>> 21) & 0x7FF) == 0x6A2 ||
            ((insn >>> 21) & 0x7FF) == 0x2A2) {
            int sf  = (insn >>> 31) & 1;
            int op  = (insn >>> 30) & 1;
            int s   = (insn >>> 29) & 1;
            int rm  = (insn >>> 16) & 0x1F;
            int cond = (insn >>> 12) & 0xF;
            int op2 = (insn >>> 10) & 0x3;
            int rn  = (insn >>> 5) & 0x1F;
            int rd  = insn & 0x1F;
            if (s == 0) {
                String mnem = null;
                if (op == 0 && op2 == 0) mnem = "csel ";
                else if (op == 0 && op2 == 1) mnem = "csinc";
                else if (op == 1 && op2 == 0) mnem = "csinv";
                else if (op == 1 && op2 == 1) mnem = "csneg";
                if (mnem != null) {
                    return String.format("%s %s, %s, %s, %s",
                            mnem, reg(rd, sf == 1), reg(rn, sf == 1), reg(rm, sf == 1), cond(cond));
                }
            }
        }

        // Data-processing (3 source): sf 0011011 op31 Rm o0 Ra Rn Rd  → MADD/MSUB/MUL alias
        if (((insn >>> 24) & 0x1F) == 0x1B) {
            int sf  = (insn >>> 31) & 1;
            int o0  = (insn >>> 15) & 1;
            int ra  = (insn >>> 10) & 0x1F;
            int rm  = (insn >>> 16) & 0x1F;
            int rn  = (insn >>> 5) & 0x1F;
            int rd  = insn & 0x1F;
            int op31 = (insn >>> 21) & 0x7;
            if (op31 == 0) {
                String mnem = (o0 == 0) ? "madd" : "msub";
                if (ra == 31) mnem = (o0 == 0) ? "mul " : "mneg";
                if (ra == 31) {
                    return String.format("%s %s, %s, %s",
                            mnem, reg(rd, sf == 1), reg(rn, sf == 1), reg(rm, sf == 1));
                }
                return String.format("%s %s, %s, %s, %s",
                        mnem, reg(rd, sf == 1), reg(rn, sf == 1), reg(rm, sf == 1), reg(ra, sf == 1));
            }
        }

        return ".inst 0x" + hex8(insn) + "  ; dp-reg";
    }

    // ──────────────────────────────────────────────────────────────────────
    // Load / Store (常用形态)
    // ──────────────────────────────────────────────────────────────────────
    private static String decodeLoadStore(int insn) {
        // Load/Store pair: opc 101 V op3 L imm7 Rt2 Rn Rt
        // bits[28:25] = 1010, bit27=1, bit26=V
        if (((insn >>> 26) & 0x3F) == 0x28 || ((insn >>> 26) & 0x3F) == 0x29 ||
            ((insn >>> 26) & 0x3F) == 0x2A || ((insn >>> 26) & 0x3F) == 0x2B) {
            int opc = (insn >>> 30) & 0x3;
            int v   = (insn >>> 26) & 1;
            int op3 = (insn >>> 23) & 0x7;  // 001 post, 010 signed, 011 pre
            int l   = (insn >>> 22) & 1;
            int imm7 = (insn >>> 15) & 0x7F;
            int rt2 = (insn >>> 10) & 0x1F;
            int rn  = (insn >>> 5)  & 0x1F;
            int rt  = insn & 0x1F;
            if (v == 0 && (opc == 0 || opc == 2)) {
                boolean is64 = (opc == 2);
                int scale = is64 ? 8 : 4;
                long imm = imm7;
                if ((imm & 0x40) != 0) imm |= ~0x7FL;
                imm *= scale;
                String mnem = (l == 1) ? "ldp " : "stp ";
                String r1 = reg(rt, is64), r2 = reg(rt2, is64);
                String base = regSp(rn, true);
                switch (op3 & 0x3) {
                    case 1: // post-index
                        return String.format("%s %s, %s, [%s], #%d", mnem, r1, r2, base, imm);
                    case 3: // pre-index
                        return String.format("%s %s, %s, [%s, #%d]!", mnem, r1, r2, base, imm);
                    case 2: // signed offset
                        if (imm == 0) return String.format("%s %s, %s, [%s]", mnem, r1, r2, base);
                        return String.format("%s %s, %s, [%s, #%d]", mnem, r1, r2, base, imm);
                    default: break;
                }
            }
        }

        // LDR (literal): opc 011 V 00 imm19 Rt
        if ((insn & 0x3B000000) == 0x18000000) {
            int opc = (insn >>> 30) & 0x3;
            int v   = (insn >>> 26) & 1;
            int imm19 = (insn >>> 5) & 0x7FFFF;
            int rt = insn & 0x1F;
            long off = imm19;
            if ((off & (1L << 18)) != 0) off |= ~((1L << 19) - 1);
            off <<= 2;
            if (v == 0) {
                String mnem;
                boolean is64;
                switch (opc) {
                    case 0: mnem = "ldr "; is64 = false; break;
                    case 1: mnem = "ldr "; is64 = true;  break;
                    case 2: mnem = "ldrsw"; is64 = true; break;
                    default: return ".inst 0x" + hex8(insn);
                }
                return String.format("%s %s, .%+d", mnem, reg(rt, is64), off);
            }
        }

        // Load/Store register (immediate): bits[29:27]=111, bit26=V, bits[25:24]=00 (post/pre/unscaled)
        // Load/Store register (unsigned offset): bits[25:24]=01
        if (((insn >>> 27) & 0x7) == 0x7) {
            int size = (insn >>> 30) & 0x3;
            int v    = (insn >>> 26) & 1;
            int idx  = (insn >>> 24) & 0x3;
            int opc  = (insn >>> 22) & 0x3;
            int rn   = (insn >>> 5) & 0x1F;
            int rt   = insn & 0x1F;
            if (v == 0 && idx == 1) {
                // unsigned offset
                int imm12 = (insn >>> 10) & 0xFFF;
                long imm = ((long) imm12) << size;
                String mnem = lsMnem(size, opc);
                if (mnem == null) return ".inst 0x" + hex8(insn);
                boolean is64 = lsIs64(size, opc);
                String base = regSp(rn, true);
                if (imm == 0) return String.format("%s %s, [%s]", mnem, reg(rt, is64), base);
                return String.format("%s %s, [%s, #%d]", mnem, reg(rt, is64), base, imm);
            }
            if (v == 0 && idx == 0) {
                int op2 = (insn >>> 10) & 0x3;
                int imm9 = (insn >>> 12) & 0x1FF;
                long imm = imm9;
                if ((imm & 0x100) != 0) imm |= ~0x1FFL;
                String mnem = lsMnem(size, opc);
                if (mnem == null) return ".inst 0x" + hex8(insn);
                boolean is64 = lsIs64(size, opc);
                String base = regSp(rn, true);
                switch (op2) {
                    case 0: // unscaled (LDUR/STUR)
                        String umnem = "stur";
                        if (opc == 1) umnem = "ldur";
                        else if (opc == 2 || opc == 3) umnem = "ldurs";
                        if (imm == 0) return String.format("%s %s, [%s]", umnem, reg(rt, is64), base);
                        return String.format("%s %s, [%s, #%d]", umnem, reg(rt, is64), base, imm);
                    case 1: // post-index
                        return String.format("%s %s, [%s], #%d", mnem, reg(rt, is64), base, imm);
                    case 3: // pre-index
                        return String.format("%s %s, [%s, #%d]!", mnem, reg(rt, is64), base, imm);
                    default: break;
                }
            }
        }

        return ".inst 0x" + hex8(insn) + "  ; ldst";
    }

    private static String lsMnem(int size, int opc) {
        switch (opc) {
            case 0: // STR
                switch (size) {
                    case 0: return "strb";
                    case 1: return "strh";
                    case 2: case 3: return "str ";
                }
                break;
            case 1: // LDR
                switch (size) {
                    case 0: return "ldrb";
                    case 1: return "ldrh";
                    case 2: case 3: return "ldr ";
                }
                break;
            case 2: // LDRSx (64-bit dest)
                switch (size) {
                    case 0: return "ldrsb";
                    case 1: return "ldrsh";
                    case 2: return "ldrsw";
                }
                break;
            case 3: // LDRSx (32-bit dest)
                switch (size) {
                    case 0: return "ldrsb";
                    case 1: return "ldrsh";
                }
                break;
        }
        return null;
    }

    private static boolean lsIs64(int size, int opc) {
        if (opc == 2) return true;            // LDRSx 32-bit dest? No: opc=2 for 64-bit dest
        if (opc == 0 || opc == 1) return size == 3;
        // opc==3 → 32-bit dest LDRS
        return false;
    }

    // ──────────────────────────────────────────────────────────────────────
    // 工具
    // ──────────────────────────────────────────────────────────────────────
    /** Wn / Xn / WZR / XZR (zero register at index 31). */
    private static String reg(int idx, boolean is64) {
        char p = is64 ? 'x' : 'w';
        if (idx == 31) return is64 ? "xzr" : "wzr";
        return "" + p + idx;
    }

    /** 用于 SP 上下文 (ADD/SUB imm, load/store 基址等): 31 → sp. */
    private static String regSp(int idx, boolean is64) {
        if (idx == 31) return is64 ? "sp" : "wsp";
        return "" + (is64 ? 'x' : 'w') + idx;
    }

    private static String xreg(int idx, boolean is64) {
        return reg(idx, is64);
    }

    private static String shiftName(int sh) {
        switch (sh) {
            case 0: return "lsl";
            case 1: return "lsr";
            case 2: return "asr";
            case 3: return "ror";
        }
        return "?";
    }

    private static String cond(int c) {
        String[] names = {"eq","ne","cs","cc","mi","pl","vs","vc",
                          "hi","ls","ge","lt","gt","le","al","nv"};
        return names[c & 0xF];
    }

    private static String barrierOption(int crm) {
        switch (crm) {
            case 1:  return "oshld";
            case 2:  return "oshst";
            case 3:  return "osh";
            case 5:  return "nshld";
            case 6:  return "nshst";
            case 7:  return "nsh";
            case 9:  return "ishld";
            case 10: return "ishst";
            case 11: return "ish";
            case 13: return "ld";
            case 14: return "st";
            case 15: return "sy";
            default: return "#" + crm;
        }
    }

    private static String hex8(int v) {
        return String.format("%08X", v);
    }

    /**
     * 解码 ARM64 logical immediate 的 N:imms:immr 编码; 失败返回 null.
     * 参考 ARM ARM J1-7434 DecodeBitMasks() 简化实现 (不含 tmask, 仅 wmask).
     */
    private static Long decodeBitmask(boolean is64, int n, int imms, int immr) {
        int len = 31 - Integer.numberOfLeadingZeros((n << 6) | ((~imms) & 0x3F));
        if (len < 1) return null;
        int size = 1 << len;
        if (!is64 && size > 32) return null;
        int levels = size - 1;
        int s = imms & levels;
        int r = immr & levels;
        if (s == levels) return null; // reserved
        long welem = (1L << (s + 1)) - 1;
        // ROR welem by r within size
        long mask = (size == 64) ? -1L : ((1L << size) - 1);
        long rotated = ((welem >>> r) | (welem << (size - r))) & mask;
        // replicate to 64 (or 32) bits
        long out = 0;
        int width = is64 ? 64 : 32;
        for (int i = 0; i < width; i += size) {
            out |= rotated << i;
        }
        if (!is64) out &= 0xFFFFFFFFL;
        return out;
    }
}

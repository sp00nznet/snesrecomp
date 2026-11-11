#!/usr/bin/env python3
"""
autogen.py - 65816 -> C generator for snesrecomp titles (game-agnostic).

Lifts whole ROM routines into C that runs against the snesrecomp runtime
(g_cpu + bus, helpers in <snesrecomp/gen_ops.h>). Output is meant for a
gitignored gen/ directory: it is derived from the user's ROM and is never
committed or distributed.

  py autogen.py <rom> <list.txt> <out.c>

list.txt: one routine per line, "BBAAAA P JSR|JSL" (entry address, the P
register it is entered with - only M/X matter - and how it is called), as the
timed-recomp profiler prints them. '#' starts a comment.

out.c gets one gen_BBAAAA() per routine that could be lifted, plus
gen_register_all(), which registers them in the dispatch table (call it after
any hand-written registrations so generated bodies take over those addresses).
Routines that cannot be lifted are listed in a comment at the top with the
reason; they keep running genuine (or hand-ported).

What it models, and what makes a routine unsupported:
  - M/X are tracked statically along every path. REP/SEP change them; PLP
    restores the M/X of the matching PHP on the same path. A routine that
    reaches one instruction with two different widths, or pulls P it did not
    push, is unsupported.
  - Stack depth is tracked too, including local frames made with
    TSC : SEC : SBC #n : TCS. Pulling below the entry level (taking the return
    address, as jump-table dispatchers do), returning with bytes still pushed,
    any other TCS, TXS, XCE, BRK/COP/WAI/STP are unsupported.
  - JMP abs inside the bank is followed and emitted as a goto (a tail jump into
    another routine just inlines it). JML and indirect JMPs are tail calls.
  - JSR/JSL push the real return address on the emulated stack around the call,
    so a callee using stack-relative addressing sees the frame it expects.
  - Each instruction advances the timed clock by its estimated cost
    (recomp_tick), so a generated routine keeps the timed loop in phase.

Verify every routine before trusting it: lockstep validation
(recomp_timed_add_validate) compares it with the genuine routine call by call.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm65816 import OPCODES, rom_read, detect_mapping  # noqa: E402


class Unsupported(Exception):
    pass


BRANCH = {"BEQ": ("Z", True), "BNE": ("Z", False), "BCS": ("C", True), "BCC": ("C", False),
          "BMI": ("N", True), "BPL": ("N", False), "BVS": ("V", True), "BVC": ("V", False)}
RETURNS = {"RTS", "RTL", "RTI"}
REFUSE = {"BRK", "COP", "WAI", "STP", "XCE", "TXS"}
# Instructions after which A no longer holds a known stack-pointer value.
A_WRITERS = {"LDA", "PLA", "TXA", "TYA", "TDC", "XBA", "AND", "ORA", "EOR", "INC", "DEC",
             "ASL", "LSR", "ROL", "ROR", "JSR", "JSL", "MVN", "MVP"}


def decode_one(data, bank, pc, m8, x8):
    op = rom_read(data, bank, pc)
    name, size, mode = OPCODES[op]
    if mode == "immA":
        size = 1 if m8 else 2
    elif mode == "immX":
        size = 1 if x8 else 2
    raw = [rom_read(data, bank, (pc + i) & 0xFFFF) for i in range(1 + size)]
    val = 0
    for i in range(size):
        val |= raw[1 + i] << (8 * i)
    total = 1 + size
    target = None
    if mode == "rel8":
        target = (pc + total + (val - 256 if val >= 128 else val)) & 0xFFFF
    elif mode == "rel16":
        target = (pc + total + (val - 0x10000 if val >= 0x8000 else val)) & 0xFFFF
    return dict(pc=pc, op=op, name=name, mode=mode, val=val, total=total, target=target)


def push_size(ins, m8, x8):
    n = ins["name"]
    if n == "PHA": return 1 if m8 else 2
    if n in ("PHX", "PHY"): return 1 if x8 else 2
    if n in ("PHB", "PHK", "PHP"): return 1
    if n in ("PHD", "PEA", "PEI", "PER"): return 2
    if n == "PLA": return -(1 if m8 else 2)
    if n in ("PLX", "PLY"): return -(1 if x8 else 2)
    if n in ("PLB", "PLP"): return -1
    if n == "PLD": return -2
    return 0


def decode(data, bank, addr, P, limit=6000):
    """Walk every path from the entry. Returns {pc: insn} with per-instruction
    M/X state, and the set of pcs that need a label."""
    insns, labels = {}, set()
    # asym: stack depth that A holds after TSC (and SBC/ADC #n), so the local
    # frame idiom TSC : SEC : SBC #n : TCS ... TSC : CLC : ADC #n : TCS keeps the
    # depth known. carry: C when known from an immediately preceding SEC/CLC.
    work = [(addr, bool(P & 0x20), bool(P & 0x10), (), 0, None, None)]
    while work:
        pc, m8, x8, phps, depth, asym, carry = work.pop()
        state = (m8, x8, phps, depth)
        if pc in insns:
            if insns[pc]["state"] != state:
                raise Unsupported(f"${bank:02X}:{pc:04X} reached with two different M/X/stack states")
            continue
        if len(insns) >= limit:
            raise Unsupported("too large")
        ins = decode_one(data, bank, pc, m8, x8)
        ins.update(state=state, m8=m8, x8=x8, depth=depth)
        insns[pc] = ins
        name, nxt = ins["name"], (pc + ins["total"]) & 0xFFFF
        if name in REFUSE:
            raise Unsupported(f"{name} at ${bank:02X}:{pc:04X}")
        if name in RETURNS:
            if depth != 0:
                raise Unsupported(f"{name} at ${pc:04X} with {depth} bytes still pushed")
            continue
        if name in ("JML", "JMP") and ins["mode"] in ("long", "ind", "abxi", "indl"):
            if depth != 0:
                raise Unsupported(f"tail jump at ${pc:04X} with {depth} bytes pushed")
            continue
        nm, nx, nphps, nd = m8, x8, phps, depth + push_size(ins, m8, x8)
        nasym, ncarry = asym, None
        if name == "TSC":
            nasym = depth
        elif name in ("SEC", "CLC"):
            ncarry = name == "SEC"
        elif name in ("SBC", "ADC") and ins["mode"] == "immA" and not m8 and asym is not None and \
                carry == (name == "SBC"):
            nasym = asym + ins["val"] if name == "SBC" else asym - ins["val"]
        elif name == "TCS":
            if asym is None:
                raise Unsupported(f"TCS at ${pc:04X} with an unknown stack value")
            nd = asym
        elif name in A_WRITERS or name in ("SBC", "ADC"):
            nasym = None
        if nd < 0:
            raise Unsupported(f"{name} at ${pc:04X} pulls below the entry stack (return-address trick)")
        if name == "REP":
            nm, nx = m8 and not (ins["val"] & 0x20), x8 and not (ins["val"] & 0x10)
        elif name == "SEP":
            nm, nx = m8 or bool(ins["val"] & 0x20), x8 or bool(ins["val"] & 0x10)
        elif name == "PHP":
            nphps = phps + ((m8, x8),)
        elif name == "PLP":
            if not phps:
                raise Unsupported(f"PLP at ${pc:04X} without a matching PHP")
            (nm, nx), nphps = phps[-1], phps[:-1]
        if name == "JMP" and ins["mode"] == "abs":
            labels.add(ins["val"])
            work.append((ins["val"], nm, nx, nphps, nd, nasym, ncarry))
        elif name in ("BRA", "BRL"):
            labels.add(ins["target"])
            work.append((ins["target"], nm, nx, nphps, nd, nasym, ncarry))
        elif name in BRANCH:
            labels.add(ins["target"])
            work.append((ins["target"], nm, nx, nphps, nd, nasym, ncarry))
            work.append((nxt, nm, nx, nphps, nd, nasym, ncarry))
        else:
            work.append((nxt, nm, nx, nphps, nd, nasym, ncarry))
    return insns, labels


# ---- cycle estimate (master cycles), as mk's autogen: SlowROM below $80 ----
def _access_time(bank, addr):
    b, a = bank & 0xFF, addr & 0xFFFF
    if (b < 0x40 or 0x80 <= b < 0xC0) and a < 0x8000:
        if a < 0x2000 or a >= 0x6000: return 8
        if a < 0x4000 or a >= 0x4200: return 6
        return 12
    return 6 if b >= 0x80 else 8


def cycles(ins, bank):
    n, mode = ins["name"], ins["mode"]
    cyc = ins["total"] * _access_time(bank, ins["pc"])
    if mode in ("dp", "dpx", "dpy", "abs", "absx", "absy", "long", "longx", "dpi", "dpiy",
                "dpxi", "dpil", "dpily", "sr", "sriy") and n not in ("JMP", "JSR", "JML", "JSL", "PEA"):
        w = 1 if (ins["x8"] if n in ("LDX", "LDY", "STX", "STY", "CPX", "CPY") else ins["m8"]) else 2
        cyc += w * 8 * (2 if n in ("INC", "DEC", "ASL", "LSR", "ROL", "ROR", "TSB", "TRB") else 1)
    if mode == "imp" or n in ("JSR", "JSL", "RTS", "RTL"):
        cyc += 6
    return cyc


# ---- emission ------------------------------------------------------------
def ea(ins, bank):
    m, v = ins["mode"], ins["val"]
    simple = {"dp": "g_dp", "dpx": "g_dpx", "dpy": "g_dpy", "dpi": "g_dpi", "dpiy": "g_dpiy",
              "dpxi": "g_dpxi", "dpil": "g_dpil", "dpily": "g_dpily", "sr": "g_sr", "sriy": "g_sriy"}
    if m in simple: return f"{simple[m]}(0x{v:02X})"
    if m == "abs": return f"g_abs(0x{v:04X})"
    if m == "absx": return f"g_absx(0x{v:04X})"
    if m == "absy": return f"g_absy(0x{v:04X})"
    if m == "long": return f"0x{v:06X}u"
    if m == "longx": return f"g_longx(0x{v:06X}u)"
    raise Unsupported(f"addressing mode {m}")


def width(ins, kind):
    return 8 if (ins["m8"] if kind == "m" else ins["x8"]) else 16


def read(ins, bank, w):
    if ins["mode"] in ("immA", "immX", "imm8"):
        return f"0x{ins['val']:0{w // 4}X}"
    return f"g_rd{w}({ea(ins, bank)})"


def set_a(w, v):
    return f"g_cpu.C = (uint16_t)({v});" if w == 16 else f"g_seta8((uint8_t)({v}));"


def a_val(w):
    return "g_cpu.C" if w == 16 else "(uint8_t)g_cpu.C"


def body(ins, bank, is_long):
    n, mode, pc = ins["name"], ins["mode"], ins["pc"]
    wm, wx = width(ins, "m"), width(ins, "x")
    T = f"uint{wm}_t"
    nxt = (pc + ins["total"]) & 0xFFFF
    if n == "LDA": return f"{{ {T} _v = {read(ins, bank, wm)}; {set_a(wm, '_v')} g_nz{wm}(_v); }}"
    if n in ("LDX", "LDY"):
        r = n[2]
        return f"{{ uint{wx}_t _v = {read(ins, bank, wx)}; g_cpu.{r} = _v; g_nz{wx}(_v); }}"
    if n == "STA": return f"g_wr{wm}({ea(ins, bank)}, ({T}){a_val(wm)});"
    if n in ("STX", "STY"): return f"g_wr{wx}({ea(ins, bank)}, (uint{wx}_t)g_cpu.{n[2]});"
    if n == "STZ": return f"g_wr{wm}({ea(ins, bank)}, 0);"
    if n in ("ADC", "SBC"):
        return f"{set_a(wm, f'g_{n.lower()}{wm}({a_val(wm)}, {read(ins, bank, wm)})')}"
    if n in ("AND", "ORA", "EOR"):
        op = {"AND": "&", "ORA": "|", "EOR": "^"}[n]
        return f"{{ {T} _v = ({T})({a_val(wm)} {op} {read(ins, bank, wm)}); {set_a(wm, '_v')} g_nz{wm}(_v); }}"
    if n == "CMP": return f"g_cmp{wm}({a_val(wm)}, {read(ins, bank, wm)});"
    if n in ("CPX", "CPY"): return f"g_cmp{wx}((uint{wx}_t)g_cpu.{n[2]}, {read(ins, bank, wx)});"
    if n == "BIT": return f"g_bit{wm}({read(ins, bank, wm)}, {'true' if mode == 'immA' else 'false'});"
    if n in ("TSB", "TRB"):
        new = f"_v | {a_val(wm)}" if n == "TSB" else f"_v & ~{a_val(wm)}"
        return (f"{{ uint32_t _e = {ea(ins, bank)}; {T} _v = g_rd{wm}(_e); "
                f"g_cpu.flag_Z = ({a_val(wm)} & _v) == 0; g_wr{wm}(_e, ({T})({new})); }}")
    if n in ("INC", "DEC", "ASL", "LSR", "ROL", "ROR"):
        if n in ("INC", "DEC"):
            f = (lambda x: f"({T})({x} {'+' if n == 'INC' else '-'} 1)")
            nz = True
        else:
            f = (lambda x: f"g_{n.lower()}{wm}({x})")
            nz = False
        if mode == "imp":
            return f"{{ {T} _v = {f(a_val(wm))}; {set_a(wm, '_v')}{f' g_nz{wm}(_v);' if nz else ''} }}"
        return (f"{{ uint32_t _e = {ea(ins, bank)}; {T} _v = {f(f'g_rd{wm}(_e)')}; "
                f"g_wr{wm}(_e, _v);{f' g_nz{wm}(_v);' if nz else ''} }}")
    if n in ("INX", "INY", "DEX", "DEY"):
        r, d = n[2], ("+" if n[0] == "I" else "-")
        return f"{{ uint{wx}_t _v = (uint{wx}_t)(g_cpu.{r} {d} 1); g_cpu.{r} = _v; g_nz{wx}(_v); }}"
    if n in ("TAX", "TAY"):
        return f"{{ uint{wx}_t _v = (uint{wx}_t)g_cpu.C; g_cpu.{n[2]} = _v; g_nz{wx}(_v); }}"
    if n in ("TXA", "TYA"):
        return f"{{ {T} _v = ({T})g_cpu.{n[1]}; {set_a(wm, '_v')} g_nz{wm}(_v); }}"
    if n in ("TXY", "TYX"):
        return f"{{ uint{wx}_t _v = (uint{wx}_t)g_cpu.{n[1]}; g_cpu.{n[2]} = _v; g_nz{wx}(_v); }}"
    if n == "TSX": return f"{{ uint{wx}_t _v = (uint{wx}_t)g_cpu.S; g_cpu.X = _v; g_nz{wx}(_v); }}"
    if n == "TCD": return "g_cpu.DP = g_cpu.C; g_nz16(g_cpu.DP);"
    if n == "TDC": return "g_cpu.C = g_cpu.DP; g_nz16(g_cpu.C);"
    if n == "TSC": return "g_cpu.C = g_cpu.S; g_nz16(g_cpu.C);"
    if n == "TCS": return "g_cpu.S = g_cpu.C;"
    if n == "XBA": return "g_cpu.C = (uint16_t)((g_cpu.C >> 8) | (g_cpu.C << 8)); g_nz8((uint8_t)g_cpu.C);"
    if n == "PHA": return f"g_push{wm}(({T})g_cpu.C);"
    if n in ("PHX", "PHY"): return f"g_push{wx}((uint{wx}_t)g_cpu.{n[2]});"
    if n == "PLA": return f"{{ {T} _v = g_pull{wm}(); {set_a(wm, '_v')} g_nz{wm}(_v); }}"
    if n in ("PLX", "PLY"): return f"{{ uint{wx}_t _v = g_pull{wx}(); g_cpu.{n[2]} = _v; g_nz{wx}(_v); }}"
    if n == "PHB": return "g_push8(g_cpu.DB);"
    if n == "PLB": return "g_cpu.DB = g_pull8(); g_nz8(g_cpu.DB);"
    if n == "PHK": return f"g_push8(0x{bank:02X});"
    if n == "PHD": return "g_push16(g_cpu.DP);"
    if n == "PLD": return "g_cpu.DP = g_pull16(); g_nz16(g_cpu.DP);"
    if n == "PHP": return "g_push8(cpu_get_p());"
    if n == "PLP": return "g_set_p(g_pull8());"
    if n == "PEA": return f"g_push16(0x{ins['val']:04X});"
    if n == "PEI": return f"g_push16(g_rd16(g_dp(0x{ins['val']:02X})));"
    if n == "PER": return f"g_push16(0x{ins['target']:04X});"
    if n == "REP": return f"g_rep(0x{ins['val']:02X});"
    if n == "SEP": return f"g_sep(0x{ins['val']:02X});"
    flags = {"CLC": "g_cpu.flag_C = false;", "SEC": "g_cpu.flag_C = true;", "CLI": "g_cpu.flag_I = false;",
             "SEI": "g_cpu.flag_I = true;", "CLD": "g_cpu.flag_D = false;", "SED": "g_cpu.flag_D = true;",
             "CLV": "g_cpu.flag_V = false;", "NOP": ";", "WDM": ";"}
    if n in flags: return flags[n]
    if n in ("MVN", "MVP"):
        return f"g_{n.lower()}(0x{ins['val'] & 0xFF:02X}, 0x{ins['val'] >> 8:02X});"
    if n == "JSR" and mode == "abs":
        return (f"g_push16(0x{(nxt - 1) & 0xFFFF:04X}); func_table_call_jsr(0x{bank:02X}{ins['val']:04X}); "
                f"g_cpu.S += 2;")
    if n == "JSR" and mode == "abxi":
        return (f"{{ uint16_t _t = g_rd16(0x{bank:02X}0000u | (uint16_t)(0x{ins['val']:04X} + g_cpu.X)); "
                f"g_push16(0x{(nxt - 1) & 0xFFFF:04X}); func_table_call_jsr(0x{bank:02X}0000u | _t); g_cpu.S += 2; }}")
    if n == "JSL":
        return (f"g_push8(0x{bank:02X}); g_push16(0x{(nxt - 1) & 0xFFFF:04X}); "
                f"func_table_call(0x{ins['val']:06X}u); g_cpu.S += 3;")
    # tail jumps: the target returns for us
    call = "func_table_call" if is_long else "func_table_call_jsr"
    if n == "JML" and mode == "long": return f"{call}(0x{ins['val']:06X}u); return;"
    if n == "JMP" and mode == "ind":
        return f"{call}(0x{bank:02X}0000u | g_rd16(0x{ins['val']:04X}u)); return;"
    if n == "JMP" and mode == "abxi":
        return (f"{call}(0x{bank:02X}0000u | g_rd16(0x{bank:02X}0000u | "
                f"(uint16_t)(0x{ins['val']:04X} + g_cpu.X))); return;")
    if n == "JML" and mode == "indl": return f"{call}(g_rd24(0x{ins['val']:04X}u)); return;"
    raise Unsupported(f"no rule for {n} {mode} at ${pc:04X}")


def body_terminates(ins):
    n, mode = ins["name"], ins["mode"]
    return n in RETURNS or (n in ("JML", "JMP") and mode in ("long", "ind", "abxi", "indl"))


def emit_function(data, addr24, P, is_long):
    """Returns C text. Labels for fall-through gotos are guaranteed to exist
    because decode() always decodes the fall-through successor."""
    bank, addr = addr24 >> 16, addr24 & 0xFFFF
    insns, labels = decode(data, bank, addr, P)
    order = sorted(insns)
    # first pass: which fall-through edges need an explicit goto
    for i, pc in enumerate(order):
        ins = insns[pc]
        n = ins["name"]
        nxt = (pc + ins["total"]) & 0xFFFF
        straight = not (body_terminates(ins) or n in ("BRA", "BRL") or (n == "JMP" and ins["mode"] == "abs"))
        if straight and (i + 1 >= len(order) or order[i + 1] != nxt):
            labels.add(nxt)
            ins["goto_next"] = nxt
    # Shared blocks can sort before the actual call entry.
    labels.add(addr)
    lines = [f"/* ${addr24:06X}, entered with P=${P:02X} via {'JSL' if is_long else 'JSR'} */",
             f"void gen_{addr24:06X}(void) {{",
             f"    goto L_{addr:04X};"]
    for pc in order:
        ins = insns[pc]
        n = ins["name"]
        if pc in labels:
            lines.append(f"L_{pc:04X}:;")
        tick = f"recomp_tick({cycles(ins, bank)});"
        if n in RETURNS:
            stmt = "return;"
        elif n == "JMP" and ins["mode"] == "abs":
            stmt = f"goto L_{ins['val']:04X};"
        elif n in ("BRA", "BRL"):
            stmt = f"goto L_{ins['target']:04X};"
        elif n in BRANCH:
            f, want = BRANCH[n]
            stmt = f"if ({'' if want else '!'}g_cpu.flag_{f}) goto L_{ins['target']:04X};"
        else:
            stmt = body(ins, bank, is_long)
        lines.append(f"    {tick} {stmt}  /* {pc:04X} {n} */")
        if "goto_next" in ins:
            lines.append(f"    goto L_{ins['goto_next']:04X};")
    lines.append("}")
    return "\n".join(lines)


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 1
    rom, lst, out = sys.argv[1:]
    data = open(rom, "rb").read()
    if len(data) % 1024 == 512:
        data = data[512:]
    detect_mapping(data)
    entries = []
    for ln in open(lst):
        ln = ln.split("#")[0].split()
        if ln:
            entries.append((int(ln[0], 16), int(ln[1], 16), ln[2].upper() == "JSL"))
    done, skipped = [], []
    for addr24, P, is_long in entries:
        try:
            done.append((addr24, emit_function(data, addr24, P, is_long)))
        except Unsupported as e:
            skipped.append((addr24, str(e)))
    with open(out, "w", newline="\n") as f:
        f.write("/* Generated by snesrecomp tools/autogen.py from the user's ROM.\n"
                " * Derived from the ROM: never commit or distribute this file. */\n")
        for a, why in skipped:
            f.write(f"/* not lifted: ${a:06X}: {why} */\n")
        f.write("#include <snesrecomp/snesrecomp.h>\n#include <snesrecomp/gen_ops.h>\n\n")
        f.write("#if defined(_MSC_VER)\n#pragma warning(disable: 4102)  /* unreferenced labels */\n#endif\n\n")
        for _, text in done:
            f.write(text + "\n\n")
        f.write("void gen_register_all(void) {\n")
        for a, _ in done:
            f.write(f"    func_table_register(0x{a:06X}, gen_{a:06X});\n")
        f.write("}\n")
    print(f"autogen: {len(done)} lifted, {len(skipped)} not lifted -> {out}")
    for a, why in skipped:
        print(f"  ${a:06X}: {why}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

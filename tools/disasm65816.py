#!/usr/bin/env python3
"""
65816 disassembler for SNES static-recompilation projects (game-agnostic).
Tracks M/X flags for correct immediate sizing; LoROM/HiROM auto-detected.
Usage: py disasm65816.py <rom.sfc> <bank:addr> [count] [mx_hex] [lorom|hirom]
  mx_hex: M/X flag byte, e.g. "00"=16-bit both, "30"=8-bit both (default 30)
Part of the snesrecomp toolkit; also imported by callgraph.py.
"""

import sys
from pathlib import Path

# Addressing mode sizes (excluding opcode byte)
# 'b'=byte, 'w'=word, 'l'=long, 'r'=rel8, 'R'=rel16, 'i'=implied
# 'a'=accumulator-dependent, 'x'=index-dependent, 's'=stack-relative
OPCODES = {
    # BRK, COP
    0x00: ("BRK", 1, "imm8"),  0x02: ("COP", 1, "imm8"),
    # ORA
    0x01: ("ORA", 2, "dpxi"),  0x03: ("ORA", 1, "sr"),
    0x05: ("ORA", 1, "dp"),    0x07: ("ORA", 2, "dpil"),
    0x09: ("ORA", 0, "immA"),  0x0D: ("ORA", 2, "abs"),
    0x0F: ("ORA", 3, "long"),  0x11: ("ORA", 2, "dpiy"),
    0x12: ("ORA", 1, "dpi"),   0x13: ("ORA", 1, "sriy"),
    0x15: ("ORA", 1, "dpx"),   0x17: ("ORA", 2, "dpily"),
    0x19: ("ORA", 2, "absy"),  0x1D: ("ORA", 2, "absx"),
    0x1F: ("ORA", 3, "longx"),
    # AND
    0x21: ("AND", 2, "dpxi"),  0x23: ("AND", 1, "sr"),
    0x25: ("AND", 1, "dp"),    0x27: ("AND", 2, "dpil"),
    0x29: ("AND", 0, "immA"),  0x2D: ("AND", 2, "abs"),
    0x2F: ("AND", 3, "long"),  0x31: ("AND", 2, "dpiy"),
    0x32: ("AND", 1, "dpi"),   0x33: ("AND", 1, "sriy"),
    0x35: ("AND", 1, "dpx"),   0x37: ("AND", 2, "dpily"),
    0x39: ("AND", 2, "absy"),  0x3D: ("AND", 2, "absx"),
    0x3F: ("AND", 3, "longx"),
    # EOR
    0x41: ("EOR", 2, "dpxi"),  0x43: ("EOR", 1, "sr"),
    0x45: ("EOR", 1, "dp"),    0x47: ("EOR", 2, "dpil"),
    0x49: ("EOR", 0, "immA"),  0x4D: ("EOR", 2, "abs"),
    0x4F: ("EOR", 3, "long"),  0x51: ("EOR", 2, "dpiy"),
    0x52: ("EOR", 1, "dpi"),   0x53: ("EOR", 1, "sriy"),
    0x55: ("EOR", 1, "dpx"),   0x57: ("EOR", 2, "dpily"),
    0x59: ("EOR", 2, "absy"),  0x5D: ("EOR", 2, "absx"),
    0x5F: ("EOR", 3, "longx"),
    # ADC
    0x61: ("ADC", 2, "dpxi"),  0x63: ("ADC", 1, "sr"),
    0x65: ("ADC", 1, "dp"),    0x67: ("ADC", 2, "dpil"),
    0x69: ("ADC", 0, "immA"),  0x6D: ("ADC", 2, "abs"),
    0x6F: ("ADC", 3, "long"),  0x71: ("ADC", 2, "dpiy"),
    0x72: ("ADC", 1, "dpi"),   0x73: ("ADC", 1, "sriy"),
    0x75: ("ADC", 1, "dpx"),   0x77: ("ADC", 2, "dpily"),
    0x79: ("ADC", 2, "absy"),  0x7D: ("ADC", 2, "absx"),
    0x7F: ("ADC", 3, "longx"),
    # STA
    0x81: ("STA", 2, "dpxi"),  0x83: ("STA", 1, "sr"),
    0x85: ("STA", 1, "dp"),    0x87: ("STA", 2, "dpil"),
    0x8D: ("STA", 2, "abs"),   0x8F: ("STA", 3, "long"),
    0x91: ("STA", 2, "dpiy"),  0x92: ("STA", 1, "dpi"),
    0x93: ("STA", 1, "sriy"),  0x95: ("STA", 1, "dpx"),
    0x97: ("STA", 2, "dpily"), 0x99: ("STA", 2, "absy"),
    0x9D: ("STA", 2, "absx"),  0x9F: ("STA", 3, "longx"),
    # LDA
    0xA1: ("LDA", 2, "dpxi"),  0xA3: ("LDA", 1, "sr"),
    0xA5: ("LDA", 1, "dp"),    0xA7: ("LDA", 2, "dpil"),
    0xA9: ("LDA", 0, "immA"),  0xAD: ("LDA", 2, "abs"),
    0xAF: ("LDA", 3, "long"),  0xB1: ("LDA", 2, "dpiy"),
    0xB2: ("LDA", 1, "dpi"),   0xB3: ("LDA", 1, "sriy"),
    0xB5: ("LDA", 1, "dpx"),   0xB7: ("LDA", 2, "dpily"),
    0xB9: ("LDA", 2, "absy"),  0xBD: ("LDA", 2, "absx"),
    0xBF: ("LDA", 3, "longx"),
    # CMP
    0xC1: ("CMP", 2, "dpxi"),  0xC3: ("CMP", 1, "sr"),
    0xC5: ("CMP", 1, "dp"),    0xC7: ("CMP", 2, "dpil"),
    0xC9: ("CMP", 0, "immA"),  0xCD: ("CMP", 2, "abs"),
    0xCF: ("CMP", 3, "long"),  0xD1: ("CMP", 2, "dpiy"),
    0xD2: ("CMP", 1, "dpi"),   0xD3: ("CMP", 1, "sriy"),
    0xD5: ("CMP", 1, "dpx"),   0xD7: ("CMP", 2, "dpily"),
    0xD9: ("CMP", 2, "absy"),  0xDD: ("CMP", 2, "absx"),
    0xDF: ("CMP", 3, "longx"),
    # SBC
    0xE1: ("SBC", 2, "dpxi"),  0xE3: ("SBC", 1, "sr"),
    0xE5: ("SBC", 1, "dp"),    0xE7: ("SBC", 2, "dpil"),
    0xE9: ("SBC", 0, "immA"),  0xED: ("SBC", 2, "abs"),
    0xEF: ("SBC", 3, "long"),  0xF1: ("SBC", 2, "dpiy"),
    0xF2: ("SBC", 1, "dpi"),   0xF3: ("SBC", 1, "sriy"),
    0xF5: ("SBC", 1, "dpx"),   0xF7: ("SBC", 2, "dpily"),
    0xF9: ("SBC", 2, "absy"),  0xFD: ("SBC", 2, "absx"),
    0xFF: ("SBC", 3, "longx"),
    # LDX
    0xA2: ("LDX", 0, "immX"),  0xA6: ("LDX", 1, "dp"),
    0xAE: ("LDX", 2, "abs"),   0xB6: ("LDX", 1, "dpy"),
    0xBE: ("LDX", 2, "absy"),
    # LDY
    0xA0: ("LDY", 0, "immX"),  0xA4: ("LDY", 1, "dp"),
    0xAC: ("LDY", 2, "abs"),   0xB4: ("LDY", 1, "dpx"),
    0xBC: ("LDY", 2, "absx"),
    # STX
    0x86: ("STX", 1, "dp"),    0x8E: ("STX", 2, "abs"),
    0x96: ("STX", 1, "dpy"),
    # STY
    0x84: ("STY", 1, "dp"),    0x8C: ("STY", 2, "abs"),
    0x94: ("STY", 1, "dpx"),
    # STZ
    0x64: ("STZ", 1, "dp"),    0x74: ("STZ", 1, "dpx"),
    0x9C: ("STZ", 2, "abs"),   0x9E: ("STZ", 2, "absx"),
    # CPX, CPY
    0xE0: ("CPX", 0, "immX"),  0xE4: ("CPX", 1, "dp"),
    0xEC: ("CPX", 2, "abs"),
    0xC0: ("CPY", 0, "immX"),  0xC4: ("CPY", 1, "dp"),
    0xCC: ("CPY", 2, "abs"),
    # INC, DEC (accumulator and memory)
    0x1A: ("INC", 0, "imp"),   0x3A: ("DEC", 0, "imp"),
    0xE6: ("INC", 1, "dp"),    0xEE: ("INC", 2, "abs"),
    0xF6: ("INC", 1, "dpx"),   0xFE: ("INC", 2, "absx"),
    0xC6: ("DEC", 1, "dp"),    0xCE: ("DEC", 2, "abs"),
    0xD6: ("DEC", 1, "dpx"),   0xDE: ("DEC", 2, "absx"),
    # ASL, LSR, ROL, ROR
    0x0A: ("ASL", 0, "imp"),   0x06: ("ASL", 1, "dp"),
    0x0E: ("ASL", 2, "abs"),   0x16: ("ASL", 1, "dpx"),
    0x1E: ("ASL", 2, "absx"),
    0x4A: ("LSR", 0, "imp"),   0x46: ("LSR", 1, "dp"),
    0x4E: ("LSR", 2, "abs"),   0x56: ("LSR", 1, "dpx"),
    0x5E: ("LSR", 2, "absx"),
    0x2A: ("ROL", 0, "imp"),   0x26: ("ROL", 1, "dp"),
    0x2E: ("ROL", 2, "abs"),   0x36: ("ROL", 1, "dpx"),
    0x3E: ("ROL", 2, "absx"),
    0x6A: ("ROR", 0, "imp"),   0x66: ("ROR", 1, "dp"),
    0x6E: ("ROR", 2, "abs"),   0x76: ("ROR", 1, "dpx"),
    0x7E: ("ROR", 2, "absx"),
    # BIT
    0x24: ("BIT", 1, "dp"),    0x2C: ("BIT", 2, "abs"),
    0x34: ("BIT", 1, "dpx"),   0x3C: ("BIT", 2, "absx"),
    0x89: ("BIT", 0, "immA"),
    # TSB, TRB
    0x04: ("TSB", 1, "dp"),    0x0C: ("TSB", 2, "abs"),
    0x14: ("TRB", 1, "dp"),    0x1C: ("TRB", 2, "abs"),
    # Transfers
    0xE8: ("INX", 0, "imp"),   0xC8: ("INY", 0, "imp"),
    0xCA: ("DEX", 0, "imp"),   0x88: ("DEY", 0, "imp"),
    0xAA: ("TAX", 0, "imp"),   0xA8: ("TAY", 0, "imp"),
    0x8A: ("TXA", 0, "imp"),   0x98: ("TYA", 0, "imp"),
    0xBA: ("TSX", 0, "imp"),   0x9A: ("TXS", 0, "imp"),
    0x9B: ("TXY", 0, "imp"),   0xBB: ("TYX", 0, "imp"),
    0x1B: ("TCS", 0, "imp"),   0x3B: ("TSC", 0, "imp"),
    0x5B: ("TCD", 0, "imp"),   0x7B: ("TDC", 0, "imp"),
    # Stack
    0x48: ("PHA", 0, "imp"),   0x68: ("PLA", 0, "imp"),
    0xDA: ("PHX", 0, "imp"),   0xFA: ("PLX", 0, "imp"),
    0x5A: ("PHY", 0, "imp"),   0x7A: ("PLY", 0, "imp"),
    0x08: ("PHP", 0, "imp"),   0x28: ("PLP", 0, "imp"),
    0x8B: ("PHB", 0, "imp"),   0xAB: ("PLB", 0, "imp"),
    0x0B: ("PHD", 0, "imp"),   0x2B: ("PLD", 0, "imp"),
    0x4B: ("PHK", 0, "imp"),
    0xF4: ("PEA", 2, "abs"),   0xD4: ("PEI", 1, "dp"),
    0x62: ("PER", 2, "rel16"),
    # Flags
    0x18: ("CLC", 0, "imp"),   0x38: ("SEC", 0, "imp"),
    0x58: ("CLI", 0, "imp"),   0x78: ("SEI", 0, "imp"),
    0xD8: ("CLD", 0, "imp"),   0xF8: ("SED", 0, "imp"),
    0xB8: ("CLV", 0, "imp"),
    0xC2: ("REP", 1, "imm8"),  0xE2: ("SEP", 1, "imm8"),
    0xFB: ("XCE", 0, "imp"),   0xEB: ("XBA", 0, "imp"),
    # Jumps
    0x4C: ("JMP", 2, "abs"),   0x5C: ("JML", 3, "long"),
    0x6C: ("JMP", 2, "ind"),   0x7C: ("JMP", 2, "abxi"),
    0xDC: ("JML", 2, "indl"),
    0x20: ("JSR", 2, "abs"),   0x22: ("JSL", 3, "long"),
    0xFC: ("JSR", 2, "abxi"),
    0x60: ("RTS", 0, "imp"),   0x6B: ("RTL", 0, "imp"),
    0x40: ("RTI", 0, "imp"),
    # Branches
    0x80: ("BRA", 1, "rel8"),  0x82: ("BRL", 2, "rel16"),
    0xD0: ("BNE", 1, "rel8"),  0xF0: ("BEQ", 1, "rel8"),
    0x90: ("BCC", 1, "rel8"),  0xB0: ("BCS", 1, "rel8"),
    0x10: ("BPL", 1, "rel8"),  0x30: ("BMI", 1, "rel8"),
    0x50: ("BVC", 1, "rel8"),  0x70: ("BVS", 1, "rel8"),
    # Misc
    0xEA: ("NOP", 0, "imp"),   0xDB: ("STP", 0, "imp"),
    0xCB: ("WAI", 0, "imp"),   0x42: ("WDM", 1, "imm8"),
    0x44: ("MVP", 2, "mvn"),   0x54: ("MVN", 2, "mvn"),
}


# ROM mapping: "lorom" or "hirom". Set by detect_mapping()/set_mapping(); the
# crawler and disassembler read through rom_read() so both mappings work.
MAPPING = "lorom"


def set_mapping(m):
    global MAPPING
    assert m in ("lorom", "hirom")
    MAPPING = m


def detect_mapping(data):
    """Pick LoROM vs HiROM by the header checksum/complement test (checksum ^
    complement == 0xFFFF at the correct header location). Falls back to the map
    byte. Returns 'lorom' or 'hirom' and also sets the module mapping."""
    def ck_ok(comp_off, csum_off):
        if csum_off + 1 >= len(data):
            return False
        comp = data[comp_off] | (data[comp_off + 1] << 8)
        csum = data[csum_off] | (data[csum_off + 1] << 8)
        return (comp ^ csum) == 0xFFFF
    lo = ck_ok(0x7FDC, 0x7FDE)
    hi = ck_ok(0xFFDC, 0xFFDE)
    if lo and not hi:
        m = "lorom"
    elif hi and not lo:
        m = "hirom"
    elif len(data) > 0xFFD5:
        m = "lorom" if (data[0x7FD5] & 1) == 0 else "hirom"
    else:
        m = "lorom"
    set_mapping(m)
    return m


def rom_read(data, bank, addr):
    if MAPPING == "hirom":
        # HiROM: bank:addr maps directly; banks mirror in 64 KB, masked to size.
        off = (((bank & 0x3F) << 16) | (addr & 0xFFFF))
    else:
        # LoROM: banks $00-$7F (mirrored $80-$FF) expose $8000-$FFFF as a 32 KB chunk.
        off = ((bank & 0x7F) * 0x8000) + (addr & 0x7FFF)
    return data[off % len(data)]


def disasm(data, bank, addr, count=30, m8=True, x8=True):
    lines = []
    pc = addr
    for _ in range(count):
        op = rom_read(data, bank, pc)
        if op not in OPCODES:
            lines.append(f"  ${bank:02X}:{pc:04X}  {op:02X}             .db ${op:02X}")
            pc += 1
            continue

        name, operand_size, mode = OPCODES[op]

        # Variable-size immediates
        if mode == "immA":
            operand_size = 1 if m8 else 2
        elif mode == "immX":
            operand_size = 1 if x8 else 2

        total = 1 + operand_size
        raw = [rom_read(data, bank, pc + i) for i in range(total)]
        hex_str = " ".join(f"{b:02X}" for b in raw)

        # Format operand
        if operand_size == 0:
            operand = ""
        elif operand_size == 1:
            val = raw[1]
            if mode == "rel8":
                target = (pc + 2 + (val if val < 128 else val - 256)) & 0xFFFF
                operand = f"${target:04X}"
            elif mode in ("imm8", "immA", "immX"):
                operand = f"#${val:02X}"
            elif mode == "dp":
                operand = f"${val:02X}"
            elif mode == "dpx":
                operand = f"${val:02X},x"
            elif mode == "dpy":
                operand = f"${val:02X},y"
            elif mode == "dpi":
                operand = f"(${val:02X})"
            elif mode == "dpxi":
                operand = f"(${val:02X},x)"
            elif mode == "dpiy":
                operand = f"(${val:02X}),y"
            elif mode == "dpil":
                operand = f"[${val:02X}]"
            elif mode == "dpily":
                operand = f"[${val:02X}],y"
            elif mode == "sr":
                operand = f"${val:02X},s"
            elif mode == "sriy":
                operand = f"(${val:02X},s),y"
            else:
                operand = f"${val:02X}"
        elif operand_size == 2:
            val = raw[1] | (raw[2] << 8)
            if mode == "rel16":
                target = (pc + 3 + (val if val < 0x8000 else val - 0x10000)) & 0xFFFF
                operand = f"${target:04X}"
            elif mode == "mvn":
                operand = f"${raw[1]:02X},${raw[2]:02X}"
            elif mode == "abs":
                operand = f"${val:04X}"
            elif mode == "absx":
                operand = f"${val:04X},x"
            elif mode == "absy":
                operand = f"${val:04X},y"
            elif mode == "ind":
                operand = f"(${val:04X})"
            elif mode == "abxi":
                operand = f"(${val:04X},x)"
            elif mode == "indl":
                operand = f"[${val:04X}]"
            elif mode in ("immA", "immX"):
                operand = f"#${val:04X}"
            else:
                operand = f"${val:04X}"
        elif operand_size == 3:
            val = raw[1] | (raw[2] << 8) | (raw[3] << 16)
            if mode == "longx":
                operand = f"${val:06X},x"
            else:
                operand = f"${val:06X}"

        lines.append(f"  ${bank:02X}:{pc:04X}  {hex_str:<14s} {name} {operand}")

        # Track M/X flag changes
        if op == 0xC2:  # REP
            if raw[1] & 0x20: m8 = False
            if raw[1] & 0x10: x8 = False
        elif op == 0xE2:  # SEP
            if raw[1] & 0x20: m8 = True
            if raw[1] & 0x10: x8 = True

        pc += total

        # Stop at terminal instructions
        if op in (0x5C, 0x6B, 0x60, 0x40, 0xDB):
            break

    return lines, pc, m8, x8


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: py disasm65816.py <rom.sfc> <bank:addr> [count]")
        sys.exit(1)

    rom = Path(sys.argv[1]).read_bytes()
    if len(rom) % 1024 == 512:
        rom = rom[512:]   # strip copier header
    parts = sys.argv[2].split(":")
    bank = int(parts[0], 16)
    addr = int(parts[1], 16)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 40

    # Optional 4th arg: mx flags, e.g. "00" = 16-bit both, "30" = 8-bit both
    mx = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x30
    m8 = bool(mx & 0x20)
    x8 = bool(mx & 0x10)

    # Optional 5th arg forces mapping; otherwise auto-detect.
    if len(sys.argv) > 5 and sys.argv[5] in ("lorom", "hirom"):
        set_mapping(sys.argv[5])
    else:
        detect_mapping(rom)

    lines, end_pc, m8, x8 = disasm(rom, bank, addr, count, m8, x8)
    for line in lines:
        print(line)

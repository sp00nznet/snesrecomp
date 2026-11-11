"""Synthetic CFG regressions: python -B -m unittest discover -s tools -v.

Only hand-authored instruction buffers are used. Generated C is compiled with
the real gen_ops.h and CPU state, using a tiny bus/tick stub, then discarded.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

import autogen
import disasm65816


ROOT = Path(__file__).resolve().parents[1]
BANK = 0xC0  # HiROM bank with the entire synthetic 64 KiB address range.


def program(chunks):
    data = bytearray(0x10000)
    for address, code in chunks.items():
        for offset, value in enumerate(code):
            data[(address + offset) & 0xFFFF] = value
    return data


class SyntheticMapping(unittest.TestCase):
    def setUp(self):
        previous = disasm65816.MAPPING
        disasm65816.set_mapping("hirom")
        self.addCleanup(disasm65816.set_mapping, previous)


class CfgEntryTests(SyntheticMapping):
    @classmethod
    def setUpClass(cls):
        cls.cc = shutil.which(os.environ.get("CC", "gcc"))
        if cls.cc is None:
            raise unittest.SkipTest("Set CC to a GCC-compatible C17 compiler")
        cls.build = ROOT / "build" / "autogen-tests"
        cls.build.mkdir(parents=True, exist_ok=True)

    def execute(self, chunks, entry, expected, *, a=0, z=False, is_long=False):
        source = autogen.emit_function(program(chunks), (BANK << 16) | entry, 0, is_long)
        # Tick counts observe whether the actual entry instructions executed;
        # they are not assertions about the accuracy of cycle estimates.
        harness = r'''
#include "snesrecomp/gen_ops.h"
#include <stdio.h>
#include <stdlib.h>
static uint8_t memory[65536];
static unsigned ticks, data_writes;
uint8_t bus_read8(uint8_t bank, uint16_t address) {
    if (bank != 0) exit(2);
    return memory[address];
}
void bus_write8(uint8_t bank, uint16_t address, uint8_t value) {
    if (bank != 0) exit(2);
    memory[address] = value;
    if (address == 0x20 || address == 0x21) ++data_writes;
}
void recomp_tick(int cycles) {
    (void)cycles;
    if (++ticks > 64) exit(3);
}
'''
        harness += source + f'''
int main(void) {{
    recomp_cpu_reset();
    g_cpu.flag_E = false;
    cpu_set_p(0);
    g_cpu.S = 0x01ff;
    g_cpu.C = {a};
    g_cpu.flag_Z = {int(z)};
    gen_{BANK:02X}{entry:04X}();
    printf("%u %u %u %u %u\\n", (unsigned)g_cpu.C,
           (unsigned)(memory[0x20] | ((uint16_t)memory[0x21] << 8)),
           ticks, data_writes, (unsigned)g_cpu.S);
    return 0;
}}
'''
        with tempfile.TemporaryDirectory(prefix="cfg-", dir=self.build) as directory:
            directory = Path(directory)
            c_file = directory / "synthetic.c"
            exe = directory / ("synthetic.exe" if os.name == "nt" else "synthetic")
            c_file.write_text(harness, encoding="utf-8")
            compiled = subprocess.run(
                [self.cc, "-std=c17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 "-O0", "-I", str(ROOT / "include"), str(c_file),
                 str(ROOT / "src" / "cpu.c"), "-o", str(exe)],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=5)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            # C execution, memory writes and returns are checked before labels.
            actual = tuple(map(int, ran.stdout.split()))
            self.assertEqual(actual, (*expected, 2, 0x01FF), source)

        labels = re.findall(r"^\s*(L_[0-9A-F]{4}):", source, re.MULTILINE)
        self.assertIn(f"L_{entry:04X}", labels)
        self.assertEqual(len(labels), len(set(labels)), source)
        self.assertTrue(set(re.findall(r"goto (L_[0-9A-F]{4});", source)) <= set(labels))
        return source

    def test_sequential_lowest_entry(self):
        self.execute({0x8100: [0xA9, 0x34, 0x12, 0x8D, 0x20, 0x00, 0x6B]},
                     0x8100, (0x1234, 0x1234, 3), is_long=True)

    def test_high_entry_bra_backwards(self):
        self.execute({0x8100: [0x1A, 0x8D, 0x20, 0x00, 0x60],
                      0x8120: [0x80, 0xDE]}, 0x8120, (0x11, 0x11, 4), a=0x10)

    def test_high_entry_jmp_backwards(self):
        self.execute({0x8100: [0x1A, 0x8D, 0x20, 0x00, 0x60],
                      0x8120: [0x4C, 0x00, 0x81]}, 0x8120, (0x11, 0x11, 4), a=0x10)

    def test_high_entry_side_effect_precedes_shared_block(self):
        self.execute({0x8100: [0x8D, 0x20, 0x00, 0x60],
                      0x8120: [0xA9, 0xEF, 0xBE, 0x80, 0xDB]},
                     0x8120, (0xBEEF, 0xBEEF, 4), a=0x10)

    def test_conditional_branch_and_wrapping_fallthrough(self):
        chunks = {0x0000: [0xA9, 0x11, 0x11, 0x8D, 0x20, 0x00, 0x60],
                  0x0010: [0xA9, 0x22, 0x22, 0x8D, 0x20, 0x00, 0x60],
                  0xFFFC: [0xF0, 0x12, 0xEA, 0xEA]}
        for z, value, ticks in ((False, 0x1111, 6), (True, 0x2222, 4)):
            with self.subTest(z=z):
                source = self.execute(chunks, 0xFFFC, (value, value, ticks), z=z)
                self.assertIn("goto L_0000;", source)  # Non-contiguous sorted successor.

    def test_entry_already_has_loop_target_label(self):
        self.execute({0x8100: [0x3A, 0xD0, 0xFD, 0x8D, 0x20, 0x00, 0x60]},
                     0x8100, (0, 0, 6), a=2)

    def test_php_plp_restores_decode_width(self):
        chunks = {0x8100: [0x08, 0xE2, 0x30, 0xA9, 0x7F, 0x28,
                           0xA9, 0x34, 0x12, 0x8D, 0x20, 0x00, 0x60]}
        self.execute(chunks, 0x8100, (0x1234, 0x1234, 7))
        insns, _ = autogen.decode(program(chunks), BANK, 0x8100, 0)
        self.assertTrue(insns[0x8103]["m8"])
        self.assertEqual(insns[0x8106]["state"], (False, False, (), 0))

    def test_balanced_word_stack(self):
        self.execute({0x8100: [0x48, 0xA9, 0x34, 0x12, 0x68,
                               0x8D, 0x20, 0x00, 0x60]},
                     0x8100, (0x4567, 0x4567, 5), a=0x4567)


class DecoderRejectionTests(SyntheticMapping):
    def test_existing_rejections_remain(self):
        cases = (
            ([0x00, 0x00], 0, "BRK"),
            ([0x8B, 0x28, 0x60], 0, "without a matching PHP"),
            ([0x68, 0x60], 0, "pulls below the entry stack"),
            ([0x48, 0x60], 0, "bytes still pushed"),
            ([0x1B, 0x60], 0, "unknown stack value"),
            ([0xF0, 0x04, 0xC2, 0x20, 0x80, 0x02, 0xE2, 0x20, 0xEA, 0x60],
             0x20, "two different M/X/stack states"),
            ([0xF0, 0x01, 0x48, 0xEA, 0x68, 0x60],
             0, "two different M/X/stack states"),
        )
        for code, p, reason in cases:
            with self.subTest(reason=reason, code=code):
                with self.assertRaisesRegex(autogen.Unsupported, reason):
                    autogen.emit_function(program({0x8100: code}), 0xC08100, p, False)


if __name__ == "__main__":
    unittest.main()

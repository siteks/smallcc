"""
pytest plugin: discovers .c files in tests/cases/ as test items.

Each .c file may have magic // comments on its leading lines:
    // EXPECT_R0: 42
    // EXPECT_COMPILE_FAIL
    // EXPECT_STDOUT: hello
    // FILES: lib.c main.c   (multi-TU: all relative to the .c file's directory)

Each test runs against both cpu3 (via sim_c) and cpu4 (via cpu4/sim.py),
producing items named e.g. recursive_factorial[cpu3] and recursive_factorial[cpu4].
EXPECT_COMPILE_FAIL tests are arch-independent and run as a single item.
"""

import pytest
import subprocess
import re
import os
import tempfile
from pathlib import Path

ARCHES = ['cpu3', 'cpu4']
CPU4_MAXSTEPS = 100_000   # generous limit for recursive/expensive tests


def parse_meta(path):
    """Read // magic comments from the top of a .c file."""
    meta = {}
    with open(path) as f:
        for line in f:
            stripped = line.strip()
            if not stripped.startswith('//'):
                break
            m = re.match(r'//\s*(\w+)(?::\s*(.*))?', stripped)
            if m:
                meta[m.group(1)] = m.group(2) if m.group(2) is not None else True
    return meta


def pytest_collect_file(parent, file_path):
    if file_path.suffix == '.c':
        meta = parse_meta(file_path)
        if any(k.startswith('EXPECT') for k in meta):
            return CTestFile.from_parent(parent, path=file_path)


class CTestFile(pytest.File):
    def collect(self):
        meta = parse_meta(self.path)
        if 'EXPECT_COMPILE_FAIL' in meta:
            # Compile errors are arch-independent; run once
            yield CTestItem.from_parent(self, name=self.path.stem,
                                        path=self.path, arch='cpu3')
        else:
            for arch in ARCHES:
                yield CTestItem.from_parent(self, name=arch,
                                            path=self.path, arch=arch)


class CTestItem(pytest.Item):
    def __init__(self, *, path, arch, **kw):
        super().__init__(**kw)
        self.src_path = path
        self.arch = arch
        self.meta = parse_meta(path)

    def runtest(self):
        meta = self.meta
        src_dir = self.src_path.parent
        root = Path(__file__).parent.parent

        # Resolve file list
        if 'FILES' in meta:
            files = [str(src_dir / f) for f in meta['FILES'].split()]
        else:
            files = [str(self.src_path)]

        with tempfile.TemporaryDirectory() as tmp:
            asm = os.path.join(tmp, 'out.s')

            # Compile (arch flag only for cpu4)
            compile_cmd = [str(root / 'smallcc'), '-o', asm]
            if self.arch == 'cpu4':
                compile_cmd += ['-arch', 'cpu4']
            compile_cmd += files

            proc = subprocess.run(compile_cmd, capture_output=True, text=True)

            if 'EXPECT_COMPILE_FAIL' in meta:
                assert proc.returncode != 0, \
                    f"expected compile failure but smallcc exited 0\nstderr:\n{proc.stderr}"
                return

            assert proc.returncode == 0, \
                f"compile failed (exit {proc.returncode}):\n{proc.stderr}"

            # Simulate
            # Both simulators: putchar → stderr, register state → stdout
            if self.arch == 'cpu3':
                sim_cmd = [str(root / 'sim_c'), asm]
            else:
                sim_cmd = ['python3', str(root / 'cpu4' / 'sim.py'),
                           '--maxsteps', str(CPU4_MAXSTEPS), asm]

            sim = subprocess.run(sim_cmd, capture_output=True, text=True)

            if 'EXPECT_R0' in meta:
                m = re.search(r'r0:([0-9a-f]{8})', sim.stdout)
                assert m, f"no r0 in sim output: {sim.stdout!r}"
                r0 = int(m.group(1), 16)
                if r0 > 0x7fffffff:
                    r0 -= 0x100000000
                expected = int(meta['EXPECT_R0'])
                assert r0 == expected, \
                    f"r0={r0} (0x{r0 & 0xffffffff:08x}), expected {expected}"

            if 'EXPECT_STDOUT' in meta:
                expected_out = meta['EXPECT_STDOUT'].replace('\\n', '\n').replace('\\t', '\t')
                assert sim.stderr == expected_out, \
                    f"putchar output {sim.stderr!r}, expected {expected_out!r}"

    def repr_failure(self, excinfo):
        return str(excinfo.value)

    def reportinfo(self):
        return self.src_path, None, f"c::{self.name}"

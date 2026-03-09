"""
pytest plugin: discovers .c files in tests/cases/ as test items.

Each .c file may have magic // comments on its leading lines:
    // EXPECT_R0: 42
    // EXPECT_COMPILE_FAIL
    // EXPECT_STDOUT: hello
    // FILES: lib.c main.c   (multi-TU: all relative to the .c file's directory)
"""

import pytest
import subprocess
import re
import os
import tempfile
from pathlib import Path


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
        yield CTestItem.from_parent(self, name=self.path.stem, path=self.path)


class CTestItem(pytest.Item):
    def __init__(self, *, path, **kw):
        super().__init__(**kw)
        self.src_path = path
        self.meta = parse_meta(path)

    def runtest(self):
        meta = self.meta
        src_dir = self.src_path.parent

        # Resolve file list
        if 'FILES' in meta:
            files = [str(src_dir / f) for f in meta['FILES'].split()]
        else:
            files = [str(self.src_path)]

        # Find project root (two levels up from tests/cases/)
        root = Path(__file__).parent.parent

        with tempfile.TemporaryDirectory() as tmp:
            asm = os.path.join(tmp, 'out.s')

            # Compile
            proc = subprocess.run(
                [str(root / 'mycc'), '-o', asm] + files,
                capture_output=True, text=True
            )

            if 'EXPECT_COMPILE_FAIL' in meta:
                assert proc.returncode != 0, \
                    f"expected compile failure but mycc exited 0\nstderr:\n{proc.stderr}"
                return

            assert proc.returncode == 0, \
                f"compile failed (exit {proc.returncode}):\n{proc.stderr}"

            # Simulate: putchar → stderr, state line → stdout
            sim = subprocess.run(
                [str(root / 'sim_c'), asm],
                capture_output=True, text=True
            )

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
                # putchar output goes to stderr
                assert sim.stderr == meta['EXPECT_STDOUT'], \
                    f"putchar output {sim.stderr!r}, expected {meta['EXPECT_STDOUT']!r}"

    def repr_failure(self, excinfo):
        return str(excinfo.value)

    def reportinfo(self):
        return self.src_path, None, f"c::{self.name}"

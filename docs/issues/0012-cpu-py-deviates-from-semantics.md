# 0012 — cpu4/cpu.py deviates from the specified semantics

Status: RESOLVED 2026-09-28 — `cpu.py`'s executor is now generated from
`SEMANTICS` (`cpu4/exec_gen.py`), memory starts at zero, and random streams
run `cpu.py` and `sim_c` in lockstep (`make rig`). Found 2026-09-28 by the random instruction generator,
`tools/rig.py --cpupy`, and a per-instruction probe; see proposal 0005 in the
monorepo, `docs/proposals/0005-isa-specification-method.md`).

`cpu4/cpu.py` was the historical golden model. Checked against the semantics
lines in `cpu4/isa.py` (`SEMANTICS`), which `sim_c`'s hand-written executor,
its generated executor (`sim_c -gen`) and the RTL corpus all agree with, it
differs on eight instructions and on the reset contents of memory. None of
these shows up in the corpus because the corpus never runs on `cpu.py`, and the
compiler never divides by zero or shifts by 32 or more.

| Instruction | Specified | cpu.py |
|---|---|---|
| `div` | a zero divisor gives 0 | raises `ZeroDivisionError` |
| `shl`, `shr` | shift amount is `R[ry] & 31` | not masked: shifts of 32 or more give 0 |
| `mods`, `modsli` | truncating remainder (sign of the dividend) | Python floored modulo (sign of the divisor) |
| `modli`, `divli` | unsigned: `R[rx]` and the sign-extended immediate as unsigned values | divides by the signed immediate |
| `rdivli`, `rmodli` | unsigned: the sign-extended immediate as an unsigned dividend | signed dividend |

Memory reset contents: `cpu.py`'s `Mem` starts filled with `0xFF`; `sim_c`
starts with zeros. The semantics preamble did not say which is right; it now
says memory the program image does not cover reads as 0, which matches
`sim_c` and (to be confirmed) the RTL's BRAM initialisation.

Reproduce: `python3 tools/rig.py -n 50 --cpupy` fails on the first zero
divisor; the per-instruction probe used for the table lives in the proposal's
prototype notes.

Fix: do not patch the hand-written executor. Generate `cpu.py`'s execute step
from `SEMANTICS` (a Python back end for `cpu4/sem.py`, alongside the C one) or
retire `cpu.py` as an executor and keep it as a Python front end to the
assembler. Either way the random generator's `--cpupy` comparison then passes.

## Resolution

- The hand-written `CPU.step()` is gone; `cpu4/sem.py` generates
  `cpu4/exec_gen.py` from the same semantics lines as `sim_c`'s executor, and
  the float port moved to `cpu4/fpu_model.py`.
- `Mem` starts at zero.
- Two more differences surfaced once the executors ran in lockstep, both
  outside the instruction semantics and both fixed: `cpu.py`'s cycle counter
  (MMIO `0xFF00`) counted the current instruction before executing it, and its
  assembler (`cpu4/assembler.py`, also used by the hardware's `asm2hex.py`)
  read a data directive with several space-separated values (`long a b c`) as
  one unparseable value and emitted a single zero. The compiler emits one value
  per line, so compiled programs and the RTL images were never affected.
- Result: 3000 random programs (1.94M instructions, all 123 instructions)
  and 341 corpus programs identical instruction by instruction.

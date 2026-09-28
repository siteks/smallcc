# 0012 — cpu4/cpu.py deviates from the specified semantics

Status: OPEN (found 2026-09-28 by the random instruction generator,
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

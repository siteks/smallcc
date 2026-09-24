# 0003 — FPU results differ between sim_c and the RTL (rounding, fdiv)

Status: **RESOLVED 2026-09-24**, option (c): the RTL rounds `fadd`/`fsub`/`fmul` to nearest-even (guard/round/sticky in X2–W), `fdiv` is *defined* as `fmul(a, frecip(b))`, and `cpu4/fpu_model.h` is the bit-exact spec used by every simulator and the constant folder; `tests/cases/floats/fpu_vectors.c` holds the RTL to it. Original report follows.

`sim_c`, `cpu4/cpu.py` and `irsim` compute `fadd`/`fsub`/`fmul` with host
IEEE-754 single precision (round-to-nearest-even) and `fdiv` as exact
division. The ECP5 RTL (`hw/rtl/cpu4.v`) truncates the multiply and add
results (`w_fmul_mant = product[46:24]`, no guard/round/sticky) and
computes `fdiv` as `fmul(a, frecip(b))` with the 16-bit reciprocal seed
and no refinement (relative error about 2⁻¹⁶). The KU5P SoC carries the
same core.

The corpus has never failed on this: its float expectations (`floats/`)
are values that are exact under either policy, and no float-heavy program
has been compared bit-for-bit between the simulator and hardware. Every
hardware frame of the ray tracer therefore differs from the simulator's in
the low bits, and any new float op inherits the question.

Options and recommendation are in the monorepo's
`docs/proposals/0003-cpu4-accelerators.md` ("Prerequisite decision"):
(a) RTL rounds and divides exactly; (b) the spec adopts the RTL behaviour
and the simulators model it bit-exactly; (c) RNE for add/sub/mul in the RTL,
`fdiv` defined as the seed multiply. Recommended: (c).

(`fmadd`/`fmsub`, mentioned in the original report, were withdrawn with the
decision.)

To reproduce: compile any float program with `-ann`, run on `sim_c` and on
the RTL bench (`make -C hw test-sim` with a case that returns float bits),
compare `r0`; e.g. `(1.0f/3.0f)` bits.

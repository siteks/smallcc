# Handoff: new `dbnz` instruction (F3d) + `rdivli`/`rmodli` signedness bug

**From:** smallcc (compiler + sim_c + ISA spec)
**To:** cpu4_hardware
**Date:** 2026-07-02
**smallcc commits:** `8415082` (dbnz), `00a37d3` (rdivli/rmodli fix)

Two independent items; the first is a ratified ISA addition to implement,
the second is a probable RTL bug to check.

---

## 1. New instruction: `dbnz rx, disp10` (F3d, subopcode 0x02)

**What:** decrement-and-branch-if-nonzero — the counted-loop primitive.

```
dbnz rx, imm10:    rx -= 1;  if (rx != 0)  pc += sext10(imm10)
```

**Encoding:** F3d (escape from F3c when the opcode field is `1111`):

```
11011111 xxx ooo iiiiiiiiii
         rx  0x02  disp10
```

Identical field layout to `beqz` (0x00) / `bnez` (0x01); only the new
subopcode value 0x02. The displacement is PC-relative from the end of the
instruction, same as the other F3d/F3c branches.

**Semantics notes:**

- The decrement **wraps**: `dbnz` on `rx == 0` leaves `rx = 0xFFFFFFFF` and
  **takes** the branch (standard djnz behaviour — the test is on the
  post-decrement value). In RTL terms: same datapath as `dec` feeding the
  same zero-test as `bnez`, test taken on the decremented value.
- `rx` is written on every execution, branch taken or not.

**Reference implementations** (verified bit-identical on hand-written
loops): `cpu4/cpu.py` (canonical, one line next to `bnez`) and `sim_c.c`
(mnemonic table + executor case). Spec: `docs/isa/cpu4.md`, F3d table.

**Why:** the compiler now converts counted loops whose induction variable
exists only for trip counting into countdown form, and fuses the latch
(`dec rx; bnez rx, loop` — 5 bytes, 2 cycles) into one `dbnz` (3 bytes,
1 cycle). On the barrel pipeline this is worth exactly 1 cycle per loop
iteration per fused loop. CoreMark impact measured under sim_c: part of
the 461,978 → 436,562 cycle step (−5.4%); 4 fused loops (both matrix
inner loops, matrix_sum, crcu8).

**Acceptance tests:**

- `tests/cases/braun/downcount_dbnz.c` (EXPECT_R0: 385) — exercises the
  fused loop including the zero-trip guard; the emitted assembly contains
  one `dbnz`.
- Any current smallcc-compiled CoreMark: `grep -c dbnz coremark.s` → 4,
  correct CRCs (seedcrc 0xe9f5, crclist/crcfinal 0xe714, crcmatrix 0x1fd7,
  crcstate 0x8e3a).

**Hardware impact:** F3d decoder gains one subopcode; datapath reuses the
existing decrement and zero-test. No new register ports (read-modify-write
of one GPR, same as `dec`).

---

## 2. Probable RTL bug: `rdivli` / `rmodli` must be UNSIGNED

**What:** the reverse-operand F0b division forms:

| First byte | sub | Mnemonic | Correct semantics |
|---|---|---|---|
| 0x1a | 1 | `rdivli rd, rx, imm9` | rd = (u32)sext9(imm9) / rx — **unsigned** (0 if rx==0) |
| 0x1b | 0 | `rmodli rd, rx, imm9` | rd = (u32)sext9(imm9) % rx — **unsigned** (0 if rx==0) |
| 0x1b | 1 | `rdivsli rd, rx, imm9` | rd = sext9(imm9) / (s32)rx — signed (unchanged) |

The imm9 is sign-extended to form the 32-bit *bit pattern*, but for the
unsigned forms that pattern is then treated as an unsigned dividend —
exactly as `divli`/`modli` treat their register operand.

**The bug we found (in sim_c, our side):** sim_c executed `rdivli` and
`rmodli` with *signed* division, contradicting `docs/isa/cpu4.md` and the
IR semantics. Found by the differential fuzzer on 2026-07-02, fixed in
smallcc commit `00a37d3`. **If the RTL was written against sim_c's
behaviour rather than the spec, it has the same bug.** If it was written
against the spec/cpu.py, this is just a confirmation request.

**Why it matters now:** the compiler's P19 peephole emits `rdivli` for
`UDIV(const, x)` whenever the constant fits sext9 — including bit patterns
like `~239` = −240, where signed and unsigned division differ wildly
(−240/5 = −48 vs 4294967056/5 = 858993411).

**Acceptance test:** `tests/cases/ops/rdivli_unsigned.c` (EXPECT_R0: 13059)
— computes `(~239u)/5` through a P19-emitted `rdivli`; a signed
implementation returns 0x7FD0 (32720) instead.

**Hardware impact (if affected):** the div/mod unit's signedness select for
these two F0b slots; one-line decoder/control fix.

---

Both acceptance tests live in `tests/cases/` per coordination.md — the
hardware test runner picks them up through the shared corpus.

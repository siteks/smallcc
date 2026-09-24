# CoreMark (single-file port)

`coremark_single.c` is EEMBC CoreMark v1.0 concatenated into one translation
unit for smallcc's CPU4 target (ILP32, `printf` supplied by `lib/stdio.c`,
`main()` without argc/argv, µs counter at MMIO `0xFF00` as the clock). The
header comment inside the file lists exactly what was adapted from the
canonical sources; the benchmark bodies are verbatim. `LICENSE.md` is the
EEMBC licence that accompanies the original sources.

It is the compiler's driver benchmark: the pass catalogue and the cost model
in `docs/optimization-passes.md` were tuned against it.

## Run (from the repo root)

```bash
./smallcc -arch cpu4 -o bench/coremark/coremark.s bench/coremark/coremark_single.c
./sim_c  -arch cpu4 -maxsteps 4000000 bench/coremark/coremark.s
```

It is configured for one iteration of the 2 K performance run, so the
`"Errors detected"` line is only the 10-second minimum-runtime check failing
and is immaterial. The correctness signal is the CRC set:

| seedcrc | crclist / crcfinal | crcmatrix | crcstate |
|---|---|---|---|
| `0xe9f5` | `0xe714` | `0x1fd7` | `0x8e3a` |

`cycles:` on the final state line is the dynamic instruction count, which on
the barrel core equals aggregate cycles.

## Profile

```bash
./sim_c -arch cpu4 -maxsteps 4000000 -profile bench/coremark/coremark.s > bench/coremark/profile.s
```

Generated outputs (`*.s`, `profile.s`) are ignored by git.

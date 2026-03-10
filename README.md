# smallcc - a simple c compiler

Originally started as a hobby compiler because I wanted something retargettable to play with FPGA CPUs. There seems to
be a bit of a gap. Got quite a long way then abandoned due to lack of time. Now revived and mostly completed with
Claude Code.

## Features

Mostly C89, with some C99.

Deliberate deviations from C89 standard:
* No support for K&R style declarations
* No support for implicit functions and returns
* No support for struct bitfields
* C99 style declarations can occur anywhere in block
* C99 style comments allowed
* C99 for-init allowed

Translation units supported, linkage in memory, no object files. Very basic preprocessor functionality.

Targets a custom stack-based 32 bit processor with 16 bit address space. 

Detailed information in `docs/`.

## Compile and test

```
# Build compiler and run tests
make
make test_cases_v

# Compile and run hello world
$ ./smallcc -o hello.s -stats tests/cases/stdlib/hello.c 
TU    file        arena_before  arena_used
0     ./lib/stdio.c         0     53944
1     tests/cases/stdlib/hello.c     53944      9568
arena total: 63512 / 16777216 bytes (0.4%)
$ ./sim_c  hello.s 
Hello world
r0:00000000 sp:1000 bp:0000 lr:0229 pc:0007 H:1

```

## TODO

* Peephole optimiser
* Clean backend separation and alternative targets
  

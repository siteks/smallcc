
CFLAGS=-std=c11 -g

SRCS_COMMON = smallcc.c tokeniser.c parser.c types.c preprocess.c
SRCS_NEW    = target.c sx.c const.c lower.c ssa.c braun.c dom.c oos.c opt.c legalize.c alloc.c emit.c irsim.c verify.c

smallcc: $(SRCS_COMMON) $(SRCS_NEW) target.h cpu4/fpu_model.h cpu4/fpu_roms.h .isa-stamp
	$(CC) $(CFLAGS) -o smallcc $(SRCS_COMMON) $(SRCS_NEW) -lm
ISA_DEFS  = $(wildcard */isa.py)
ISA_TOOLS = isatool/gen.py isatool/model.py isatool/sem.py
ISA_GEN   = isatool/arches.h $(ISA_DEFS:isa.py=isa_table_c.h) $(ISA_DEFS:isa.py=exec_gen.h) $(ISA_DEFS:isa.py=exec_gen.py)

ISA_HDRS  = $(filter-out %/isa_table_c.h %/exec_gen.h,$(wildcard $(ISA_DEFS:isa.py=*.h)))

sim_c: sim_c.c $(ISA_HDRS) isatool/isa_types.h isatool/exec_common.h .isa-stamp
	$(CC) $(CFLAGS) -O2 -o sim_c sim_c.c -lm

# Every generated ISA file (tables, both executors, encoding docs) for every
# <arch>/isa.py; `make isa ARCH=cpu5` regenerates one ISA.
isa:
	python3 isatool/gen.py $(if $(ARCH),--arch $(ARCH))
	@touch .isa-stamp

isa-check:
	python3 isatool/gen.py --check

.isa-stamp: $(ISA_DEFS) $(ISA_TOOLS)
	python3 isatool/gen.py
	@touch .isa-stamp

test: smallcc sim_c isa-check rig-quick
	python3 -m pytest tests/cases/ -q
	python3 -m pytest tests/cases/ -q --irsim

test_v: smallcc sim_c
	python3 -m pytest tests/cases/ -v

test_p: smallcc sim_c
	python3 -m pytest tests/cases/ -n auto -q

test_irsim: smallcc
	python3 -m pytest tests/cases/ -q --irsim

test_irsim_v: smallcc
	python3 -m pytest tests/cases/ -v --irsim

test_irsim_p: smallcc
	python3 -m pytest tests/cases/ -n auto -q --irsim

fuzz: smallcc sim_c
	python3 tools/fuzz.py -n 200

clean:
	rm -f smallcc sim_c mycc_* *.o *~ tmp* _tmp*.c test.s *.lst error.log
	rm -rf .pytest_cache __pycache__ cpu4/__pycache__ isatool/__pycache__ .isa-stamp
	rm -rf *.dSYM

help:
	@echo "Build"
	@echo "  smallcc    Build the compiler"
	@echo "  sim_c      Build the C simulator"
	@echo ""
	@echo "Test"
	@echo "  test          Run all pytest cases quietly (cpu4 via sim_c)"
	@echo "  test_v        Run all pytest cases verbosely"
	@echo "  test_p        Run pytest cases in parallel (pytest-xdist)"
	@echo "  test_irsim    Run all pytest cases via -runoos and -runirc"
	@echo "  test_irsim_v  Same, verbose"
	@echo "  test_irsim_p  Same, parallel"
	@echo "  rig-quick     100 random instruction programs, sim_c vs the Python simulator (part of test)"
	@echo "  rig           1000 random programs with a fresh seed; rig-long: 10000, failures kept"
	@echo "  isa-check     Fail if any file generated from an <arch>/isa.py is stale (part of test)"
	@echo ""
	@echo "ISA"
	@echo "  isa        Regenerate tables, executors and docs from every <arch>/isa.py (ARCH=name for one)"
	@echo ""
	@echo "Misc"
	@echo "  clean      Remove compiler, simulator, and temp files"

.PHONY: isa isa-check rig rig-quick rig-long test test_v test_p test_irsim test_irsim_v test_irsim_p clean help

# Random instruction generator: constrained-random programs run in lockstep on
# sim_c and cpu4/cpu.py, whose executors are both generated from cpu4/isa.py SEMANTICS.
rig-quick: sim_c
	python3 tools/rig.py -n 100 -len 300 -seed 1
	@for a in $(filter-out cpu4,$(ISA_DEFS:/isa.py=)); do python3 tools/rig.py --arch $$a -n 30 -len 300 -seed 1 || exit 1; done
rig: sim_c
	python3 tools/rig.py -n 1000 -len 400 -seed $$(date +%s)
rig-long: sim_c
	python3 tools/rig.py -n 10000 -len 400 -seed $$(date +%s) --keep rig_failures


CFLAGS=-std=c11 -g

SRCS_COMMON = smallcc.c tokeniser.c parser.c types.c preprocess.c
SRCS_NEW    = sx.c lower.c ssa.c braun.c dom.c oos.c opt.c legalize.c alloc.c emit.c irsim.c verify.c

smallcc: $(SRCS_COMMON) $(SRCS_NEW) cpu4/fpu_model.h cpu4/fpu_roms.h
	$(CC) $(CFLAGS) -o smallcc $(SRCS_COMMON) $(SRCS_NEW) -lm
sim_c: sim_c.c cpu4/fpu_model.h cpu4/fpu_roms.h cpu4/isa_table_c.h cpu4/exec_gen.h
	$(CC) $(CFLAGS) -O2 -o sim_c sim_c.c -lm

isa:
	python3 cpu4/gen_isa.py

isa-check:
	python3 cpu4/gen_isa.py --check

cpu4/isa_table_c.h cpu4/isa_table.py cpu4/exec_gen.h: cpu4/isa.py cpu4/gen_isa.py cpu4/sem.py
	python3 cpu4/gen_isa.py

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
	rm -rf .pytest_cache __pycache__ cpu4/__pycache__
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
	@echo "  rig-quick     100 random instruction programs, hand vs generated executor (part of test)"
	@echo "  rig           1000 random programs with a fresh seed; rig-long: 10000, failures kept"
	@echo "  isa-check     Fail if cpu4/isa_table_c.h, cpu4/isa_table.py or"
	@echo "                docs/isa/cpu4-encoding.md is stale vs cpu4/isa.py (part of test)"
	@echo ""
	@echo "ISA"
	@echo "  isa        Regenerate the encoding tables from cpu4/isa.py (after editing it)"
	@echo ""
	@echo "Misc"
	@echo "  clean      Remove compiler, simulator, and temp files"

.PHONY: isa isa-check rig rig-quick rig-long test test_v test_p test_irsim test_irsim_v test_irsim_p clean help

# Random instruction generator: constrained-random programs run in lockstep on
# sim_c's hand-written executor and the one generated from cpu4/isa.py SEMANTICS.
rig-quick: sim_c
	python3 tools/rig.py -n 100 -len 300 -seed 1
rig: sim_c
	python3 tools/rig.py -n 1000 -len 400 -seed $$(date +%s)
rig-long: sim_c
	python3 tools/rig.py -n 10000 -len 400 -seed $$(date +%s) --keep rig_failures

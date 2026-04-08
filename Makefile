
CFLAGS=-std=c11 -g

SRCS_COMMON = smallcc.c tokeniser.c parser.c types.c preprocess.c
SRCS_CPU3   = codegen.c backend.c optimise.c
SRCS_NEW    = sx.c lower.c ssa.c braun.c dom.c oos.c alloc.c emit.c

smallcc: $(SRCS_COMMON) $(SRCS_CPU3) $(SRCS_NEW)
sim_c: sim_c.c
	$(CC) $(CFLAGS) -o sim_c sim_c.c -lm

test: smallcc sim_c
	python3 -m pytest tests/cases/ -q

test_v: smallcc sim_c
	python3 -m pytest tests/cases/ -v

test_p: smallcc sim_c
	python3 -m pytest tests/cases/ -n auto -q

clean:
	rm -f smallcc sim_c mycc_* *.o *~ tmp* _tmp*.c test.s *.lst error.log
	rm -rf .pytest_cache __pycache__ cpu3/__pycache__ cpu4/__pycache__
	rm -rf *.dSYM

help:
	@echo "Build"
	@echo "  smallcc    Build the compiler"
	@echo "  sim_c      Build the C simulator"
	@echo ""
	@echo "Test"
	@echo "  test       Run all pytest cases quietly"
	@echo "  test_v     Run all pytest cases verbosely"
	@echo "  test_p     Run pytest cases in parallel (pytest-xdist)"
	@echo ""
	@echo "Misc"
	@echo "  clean      Remove compiler, simulator, and temp files"

.PHONY: test test_v test_p clean help

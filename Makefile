
CFLAGS=-std=c11 -g

smallcc: smallcc.c tokeniser.c parser.c types.c codegen.c backend.c preprocess.c
sim_c: sim_c.c
	$(CC) $(CFLAGS) -o sim_c sim_c.c -lm
test_all: smallcc sim_c test_init test_ops test_logops test_func test_longs test_array test_struct test_loops test_goto test_struct_init test_floats test_compound test_remaining test_typedef test_strings test_enum test_variadic test_funcptr test_preprocess

test_init: smallcc
	(source ./test.sh && source tests/test_init.sh)

test_ops: smallcc
	(source ./test.sh && source tests/test_ops.sh)

test_logops: smallcc
	(source ./test.sh && source tests/test_logops.sh)

test_func:smallcc
	(source ./test.sh && source tests/test_func.sh)

test_longs:smallcc
	(source ./test.sh && source tests/test_longs.sh)

test_array:smallcc
	(source ./test.sh && source tests/test_array.sh)

test_struct:smallcc
	(source ./test.sh && source tests/test_struct.sh)

test_loops:smallcc
	(source ./test.sh && source tests/test_loops.sh)

test_goto:smallcc
	(source ./test.sh && source tests/test_goto.sh)

test_struct_init:smallcc
	(source ./test.sh && source tests/test_struct_init.sh)

test_floats:smallcc
	(source ./test.sh && source tests/test_floats.sh)

test_compound:smallcc
	(source ./test.sh && source tests/test_compound.sh)

test_remaining:smallcc
	(source ./test.sh && source tests/test_remaining.sh)

test_typedef:smallcc
	(source ./test.sh && source tests/test_typedef.sh)

test_strings:smallcc
	(source ./test.sh && source tests/test_strings.sh)

test_enum:smallcc
	(source ./test.sh && source tests/test_enum.sh)

test_variadic:smallcc
	(source ./test.sh && source tests/test_variadic.sh)

test_funcptr:smallcc
	(source ./test.sh && source tests/test_funcptr.sh)

test_preprocess: smallcc
	(source ./test.sh && source tests/test_preprocess.sh)

test_cases: smallcc sim_c
	python3 -m pytest tests/cases/ -q

test_cases_v: smallcc sim_c
	python3 -m pytest tests/cases/ -v

test_cases_parallel: smallcc sim_c
	python3 -m pytest tests/cases/ -n auto -q

clean:
	rm -f smallcc sim_c mycc_* *.o *~ tmp* _tmp*.c test.s *.lst error.log
	rm -rf .pytest_cache __pycache__ cpu3/__pycache__
	rm -rf *.dSYM

help:
	@echo "Build"
	@echo "  smallcc                 Build the compiler"
	@echo "  sim_c                Build the C simulator"
	@echo ""
	@echo "Test (bash suites, sequential)"
	@echo "  test_all             Run all bash suites"
	@echo "  test_init            Initializers"
	@echo "  test_ops             Operators and basic control flow"
	@echo "  test_logops          Logical and bitwise operators"
	@echo "  test_func            Functions and recursion"
	@echo "  test_longs           Long integers"
	@echo "  test_array           Arrays and pointer arithmetic"
	@echo "  test_struct          Structs and unions"
	@echo "  test_loops           for / do-while / switch / break / continue"
	@echo "  test_goto            goto and labels"
	@echo "  test_struct_init     Struct and union initializers"
	@echo "  test_floats          Floating-point arithmetic"
	@echo "  test_compound        Compound assignment operators"
	@echo "  test_remaining       %, ?:, sizeof, !, ~, &, ->"
	@echo "  test_typedef         typedef"
	@echo "  test_strings         String literals"
	@echo "  test_enum            Enumerations"
	@echo "  test_variadic        Variadic functions and ++/--"
	@echo "  test_funcptr         Function pointers"
	@echo "  test_preprocess      Preprocessor (#define, #ifdef, #include)"
	@echo ""
	@echo "Test (pytest, tests/cases/)"
	@echo "  test_cases           Run all pytest cases quietly"
	@echo "  test_cases_v         Run all pytest cases verbosely"
	@echo "  test_cases_parallel  Run pytest cases in parallel (pytest-xdist)"
	@echo ""
	@echo "Misc"
	@echo "  clean                Remove compiler, simulator, and temp files"

.PHONY: test_cases test_cases_v test_cases_parallel clean help
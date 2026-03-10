
CFLAGS=-std=c11 -g

mycc: mycc.c tokeniser.c parser.c types.c codegen.c backend.c preprocess.c
sim_c: sim_c.c
	$(CC) $(CFLAGS) -o sim_c sim_c.c -lm
test_all: mycc sim_c test_init test_ops test_logops test_func test_longs test_array test_struct test_loops test_goto test_struct_init test_floats test_compound test_remaining test_typedef test_strings test_enum test_variadic test_funcptr test_preprocess

test_init: mycc
	(source ./test.sh && source tests/test_init.sh)

test_ops: mycc
	(source ./test.sh && source tests/test_ops.sh)

test_logops: mycc
	(source ./test.sh && source tests/test_logops.sh)

test_func:mycc
	(source ./test.sh && source tests/test_func.sh)

test_longs:mycc
	(source ./test.sh && source tests/test_longs.sh)

test_array:mycc
	(source ./test.sh && source tests/test_array.sh)

test_struct:mycc
	(source ./test.sh && source tests/test_struct.sh)

test_loops:mycc
	(source ./test.sh && source tests/test_loops.sh)

test_goto:mycc
	(source ./test.sh && source tests/test_goto.sh)

test_struct_init:mycc
	(source ./test.sh && source tests/test_struct_init.sh)

test_floats:mycc
	(source ./test.sh && source tests/test_floats.sh)

test_compound:mycc
	(source ./test.sh && source tests/test_compound.sh)

test_remaining:mycc
	(source ./test.sh && source tests/test_remaining.sh)

test_typedef:mycc
	(source ./test.sh && source tests/test_typedef.sh)

test_strings:mycc
	(source ./test.sh && source tests/test_strings.sh)

test_enum:mycc
	(source ./test.sh && source tests/test_enum.sh)

test_variadic:mycc
	(source ./test.sh && source tests/test_variadic.sh)

test_funcptr:mycc
	(source ./test.sh && source tests/test_funcptr.sh)

test_preprocess: mycc
	(source ./test.sh && source tests/test_preprocess.sh)

test_cases: mycc sim_c
	python3 -m pytest tests/cases/ -q

test_cases_v: mycc sim_c
	python3 -m pytest tests/cases/ -v

test_cases_parallel: mycc sim_c
	python3 -m pytest tests/cases/ -n auto -q

clean:
	rm -f mycc sim_c *.o *~ tmp*

.PHONY: test_cases test_cases_v test_cases_parallel clean

CFLAGS=-std=c11 -g

mycc: mycc.c tokeniser.c parser.c types.c codegen.c
test_all: mycc test_init test_ops test_logops test_func test_longs test_array test_struct

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

lasttest: mycc
	(cat test.sh >tmp && echo `tail -1 tests/test1.sh` >>tmp && source tmp)




clean:
	rm -f 9cc *.o *~ tmp*

.PHONY: test clean
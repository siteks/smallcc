
CFLAGS=-std=c11 -g

mycc: mycc.c tokeniser.c parser.c codegen.c
test_all: mycc test_init
	(source ./test.sh && source tests/test1.sh)

test_init: mycc
	(source ./test.sh && source tests/test_init.sh)


lasttest: mycc
	(cat test.sh >tmp && echo `tail -1 tests/test1.sh` >>tmp && source tmp)




clean:
	rm -f 9cc *.o *~ tmp*

.PHONY: test clean
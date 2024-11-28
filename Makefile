
CFLAGS=-std=c11 -g

mycc: mycc.c tokeniser.c parser.c codegen.c
test: mycc
	./test.sh

clean:
	rm -f 9cc *.o *~ tmp*

.PHONY: test clean
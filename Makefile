
CFLAGS=-std=c11 -g -static

mycc: mycc.c
test: mycc
	./test.sh

clean:
	rm -f 9cc *.o *~ tmp*
	
.PHONY: test clean
#!/bin/bash
source test.sh

## Function pointers

# assign function to pointer and call
assert 7 "int add(int a,int b){return a+b;} int main(){int(*fp)(int,int);fp=add;return fp(3,4);}"

# call via dereferenced pointer
assert 5 "int id(int x){return x;} int main(){int(*fp)(int);fp=id;return (*fp)(5);}"

# pass function pointer as parameter
assert 9 "int sq(int x){return x*x;} int apply(int(*f)(int),int v){return f(v);} int main(){return apply(sq,3);}"

# function pointer double2 call
assert 8 "int double2(int x){return x+x;} int main(){int(*fp)(int);fp=double2;return fp(4);}"

# reassign function pointer
assert 2 "int one(int x){return 1;} int two(int x){return 2;} int main(){int(*fp)(int);fp=one;fp=two;return fp(0);}"

echo "funcptr tests OK"

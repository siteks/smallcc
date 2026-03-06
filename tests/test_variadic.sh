#!/bin/bash
source test.sh

## Variadic functions

# sum of ints
assert 60 "int sum(int n,...){va_list ap;int s;int i;s=0;va_start(ap,n);for(i=0;i<n;i++)s+=va_arg(ap,int);va_end(ap);return s;} int main(){return sum(3,10,20,30);}"

# single vararg
assert 7 "int first(int n,...){va_list ap;int v;va_start(ap,n);v=va_arg(ap,int);va_end(ap);return v;} int main(){return first(1,7);}"

# two varargs summed with named param
assert 9 "int add2(int a,...){va_list ap;int b;va_start(ap,a);b=va_arg(ap,int);va_end(ap);return a+b;} int main(){return add2(4,5);}"

# max of three varargs
assert 30 "int vmax(int n,...){va_list ap;int m;int i;int v;va_start(ap,n);m=va_arg(ap,int);for(i=1;i<n;i++){v=va_arg(ap,int);if(v>m)m=v;}va_end(ap);return m;} int main(){return vmax(3,10,30,20);}"

# multiple vararg types (char promotion to int)
assert 15 "int vadd3(int n,...){va_list ap;int s;int i;s=0;va_start(ap,n);for(i=0;i<n;i++)s+=va_arg(ap,int);va_end(ap);return s;} int main(){return vadd3(3,4,5,6);}"

# putchar builtin (r0 holds char value after call since putchar is a no-modify opcode)
assert 0 "int main(){putchar(65);return 0;}"

# post-increment: expression value is old
assert 5 "int main(){int a=5; int b; b=a++; return b;}"

# post-increment: variable is updated
assert 6 "int main(){int a=5; a++; return a;}"

# pre-increment: expression value is new
assert 6 "int main(){int a=5; int b; b=++a; return b;}"

# post-decrement
assert 4 "int main(){int a=5; a--; return a;}"

# pre-decrement
assert 4 "int main(){int a=5; int b; b=--a; return b;}"

# i++ in for loop
assert 10 "int main(){int s=0;int i;for(i=0;i<5;i++)s+=i;return s;}"

# i-- (post-decrement in loop, using positive-range check)
assert 10 "int main(){int s=0;int i;for(i=4;i>0;i--)s+=i;return s;}"

echo "variadic tests OK"

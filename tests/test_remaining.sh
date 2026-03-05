
## Remaining operators: %, ?:, comma, sizeof, ->, ~, !

# modulo
assert 1  "int main(){return 7%3;}"
assert 0  "int main(){return 6%3;}"
assert 2  "int main(){return 12%5;}"

# compound modulo
assert 1  "int main(){int a=7; a%=3; return a;}"

# ternary ?:
assert 1  "int main(){return 1?1:0;}"
assert 0  "int main(){return 0?1:0;}"
assert 5  "int main(){int x=3; return x>2?5:9;}"
assert 9  "int main(){int x=1; return x>2?5:9;}"

# nested ternary
assert 2  "int main(){int x=2; return x==1?1:x==2?2:3;}"

# comma operator
assert 3  "int main(){int a=1,b=2; return (a=3,b=5,a);}"
assert 5  "int main(){int a=1,b=2; return (a=3,b=5,b);}"

# sizeof type
assert 1  "int main(){return sizeof(char);}"
assert 2  "int main(){return sizeof(int);}"
assert 4  "int main(){return sizeof(long);}"
assert 4  "int main(){return sizeof(float);}"

# sizeof variable
assert 2  "int main(){int x; return sizeof(x);}"
assert 4  "int main(){long x; return sizeof(x);}"

# sizeof array
assert 6  "int main(){int a[3]; return sizeof(a);}"

# logical not !
assert 1  "int main(){return !0;}"
assert 0  "int main(){return !1;}"
assert 0  "int main(){return !5;}"
assert 1  "int main(){int x=0; return !x;}"

# bitwise not ~
assert 65534 "int main(){int a=1; return ~a & 0xffff;}"
assert 65529 "int main(){int a=6; return ~a & 0xffff;}"

# address-of &
assert 1  "int main(){int a=1; int *p=&a; return *p;}"
assert 7  "int main(){int a=5; int *p=&a; *p=7; return a;}"

# arrow operator ->
assert 1  "int main(){struct P{int x;int y;} s; struct P *p=&s; p->x=1; return p->x;}"
assert 5  "int main(){struct P{int a;int b;} s={3,5}; struct P *p=&s; return p->b;}"


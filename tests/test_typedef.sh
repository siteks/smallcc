## typedef

# simple typedef
assert 5  "int main(){typedef int myint; myint x=5; return x;}"

# typedef pointer
assert 7  "int main(){typedef int *ip; int a=7; ip p=&a; return *p;}"

# typedef struct
assert 3  "int main(){typedef struct{int a;int b;} P; P p; p.a=3; return p.a;}"

# typedef struct pointer
assert 9  "int main(){typedef struct{int x;int y;} S; typedef S *SP; S s; SP p=&s; s.y=9; return p->y;}"

# global typedef
assert 4  "typedef int myint; int main(){myint x=4; return x;}"

# typedef in sizeof
assert 2  "int main(){typedef int myint; return sizeof(myint);}"

# typedef alias of typedef
assert 1  "int main(){typedef int myint; typedef myint myint2; myint2 x=1; return x;}"

# nested scope typedef shadows outer
assert 5  "int main(){typedef int T; {typedef float T; T x=5.0; return (int)x;} return 0;}"

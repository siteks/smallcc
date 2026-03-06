## enum

# implicit values (A=0, B=1, C=2)
assert 1  "int main(){enum{A,B,C}; return B;}"

# explicit values
assert 7  "int main(){enum{X=7,Y=8}; return X;}"

# mixed (A=5, B=6, C=7)
assert 6  "int main(){enum{A=5,B,C}; return B;}"

# negative enum value
assert 253 "int main(){enum{A=-3}; return (unsigned char)A;}"

# enum used in expression
assert 3  "int main(){enum{A=1,B=2}; return A+B;}"

# enum variable declared and assigned
assert 2  "int main(){enum Color{RED,GREEN,BLUE}; enum Color c; c=BLUE; return c;}"

# enum as function argument
assert 1  "int f(int x){return x;} int main(){enum{A=0,B=1}; return f(B);}"

# tagged enum at file scope
assert 5  "enum E{A=5,B=6}; int main(){return A;}"

# enum in switch/case
assert 42 "int main(){enum{X=42}; int r=0; switch(1){case 1: r=X; break;} return r;}"

# enum constant in switch/case label
assert 99 "int main(){enum{V=99}; int r=0; switch(V){case 99: r=V; break;} return r;}"

# sizeof(enum E)
assert 2  "int main(){enum E{A,B}; return sizeof(enum E);}"

# anonymous enum (no tag)
assert 3  "int main(){enum{P=3,Q=4}; return P;}"

# enum comparison
assert 1  "int main(){enum{LO=1,HI=10}; return LO<HI;}"

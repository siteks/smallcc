
## Floating-point arithmetic and comparisons

# float literal cast to int
assert 3 "int main(){float x=3.0; return (int)x;}"

# float addition
assert 3 "int main(){float x=1.0; float y=2.0; float z=x+y; return (int)z;}"

# float subtraction
assert 1 "int main(){float x=2.5; float y=1.5; float z=x-y; return (int)z;}"

# float multiplication
assert 6 "int main(){float x=2.0; float y=3.0; float z=x*y; return (int)z;}"

# float division
assert 3 "int main(){float x=6.0; float y=2.0; float z=x/y; return (int)z;}"

# float comparison <
assert 1 "int main(){float x=1.5; float y=2.5; return x<y;}"

# float comparison >
assert 1 "int main(){float x=2.5; float y=1.5; return x>y;}"

# float comparison <=
assert 1 "int main(){float x=1.5; float y=1.5; return x<=y;}"

# float comparison >=
assert 1 "int main(){float x=2.0; float y=1.5; return x>=y;}"

# int to float cast
assert 5 "int main(){int i=5; float x=(float)i; return (int)x;}"

# float truncation toward zero
assert 3 "int main(){float x=3.9; return (int)x;}"

# global float
assert 3 "float g=3.0; int main(){return (int)g;}"

# mixed: int promoted to float
assert 4 "int main(){int i=2; float x=2.0; float z=x+(float)i; return (int)z;}"

# float unary negate
assert 1 "int main(){float x=-1.5; return x<0.0;}"

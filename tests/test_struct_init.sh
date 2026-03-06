
## struct and union initializers

# local struct initializer (2 int members)
assert 8 "int main(){struct P{int a;int b;} s={3,5}; return s.a+s.b;}"

# local struct initializer (3 int members)
assert 60 "int main(){struct P{int a;int b;int c;} s={10,20,30}; return s.a+s.b+s.c;}"

# global struct initializer
assert 30 "struct G{int a;int b;} g={10,20}; int main(){return g.a+g.b;}"

# partial initializer (remaining fields zero-initialized)
assert 5 "int main(){struct P{int a;int b;int c;} s={5}; return s.a+s.b+s.c;}"

# nested struct initializer with explicit braces
assert 6 "int main(){struct I{int x;int y;}; struct O{struct I p;int z;} s={{1,2},3}; return s.p.x+s.p.y+s.z;}"

# global nested struct initializer
assert 9 "struct I{int x;int y;}; struct O{struct I p;int z;} g={{3,3},3}; int main(){return g.p.x+g.p.y+g.z;}"




assert 1 "int main(){struct{int a;}b;b.a=1; return b.a;}"
assert 1 "int main(){struct{int a;int b;}c;c.b=1; return c.b;}"
# assert 2 "struct s1 {int a; int b;}; int main(){struct {struct s1 c; int d;}e; e.c.b=2;return e.c.b;}"
# assert 2 "struct s1 {int a; int b;}; int main(){struct {struct s1 c; int d;}e; e.d=2;return e.d;}"
# assert 3 "struct {int a; int b;} c={2,3}; int main(){return c.b;}"
assert 2 "int main(){struct {int a; int b;}c; c.a=2; c.b=3; return c.a;}"
assert 3 "int main(){struct {int a; int b;}c; c.a=2; c.b=3; return c.b;}"
assert 5 "int main(){struct {int a; int b;}c; c.a=2; c.b=3; return c.a+c.b;}"
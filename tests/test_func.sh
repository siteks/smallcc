
# assert 6 `cat tests/t1.c`

assert 1 "int main(int a){int b;return 1;}"
# assert 6 "int fact(int a) { if (a == 0 || a == 1) return 1; return fact(a - 1); } int main() { return fact(3); } "

assert 2 "int f(){return 2;} int main(){return f();}"
assert 3 "int f(int a){return a;} int main(){return f(3);}"

assert 24 "int f(int a){if(a<3)return a;return a*f(a-1);}int main(){return f(4);}"
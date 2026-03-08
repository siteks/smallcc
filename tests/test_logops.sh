
# Logical
assert 1 "int main(){return 0 || 1;}"
assert 1 "int main(){return 3 || 5;}"
assert 1 "int main(){return 3 &&  5;}"
assert 3 "int main(){return 1 | 2;}"
assert 0 "int main(){return 1 & 2;}"
assert 7 "int main(){return 15 & 7;}"
assert 0 "int main(){return 0 ^ 0;}"
assert 1 "int main(){return 1 ^ 0;}"
assert 9826 "int main(){return 0x1234 ^ 0x3456;}"
# Short-circuit: null pointer dereference must NOT happen
assert 1 "int main(){int *p = 0; int x = (p != 0 && *p > 0); return !x;}"
assert 1 "int main(){int *p = 0; int x = (1 || (*p = 99)); return x;}"

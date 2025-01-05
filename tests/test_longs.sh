

assert 22136 "int main(){long a = 0x12345678; return a;}"
assert 22136 "int main(){long a = 0x12345678; return a<<0;}"

assert 32768 "int main(){long a = 0x10000; return a>>1;}"


assert 0 "long main(){int a=1;return a<<16;}"
assert 65536 "long main(){int a=1;return (long)a<<16;}"
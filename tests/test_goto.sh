
## goto and labeled statements

# forward goto (skip return 1)
assert 0 "int main(){goto end; return 1; end: return 0;}"

# backward goto (counted loop)
assert 5 "int main(){int i=0; loop: i=i+1; if(i<5) goto loop; return i;}"

# goto skipping over code, label followed by another statement
assert 2 "int main(){int x=0; goto done; x=99; done: x=x+2; return x;}"

# multiple labels in one function
assert 3 "int main(){int x=1; goto b; a: return 10; b: x=x+1; goto c; a2: return 20; c: x=x+1; return x;}"

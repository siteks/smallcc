#!/bin/bash
source test.sh

## Preprocessor tests

# 1. Object-like macro in return value
assert 5 $'#define N 5\nint main(){return N;}'

# 2. Function-like macro
assert 7 $'#define ADD(a,b) ((a)+(b))\nint main(){return ADD(3,4);}'

# 3. Macro with expression body
assert 12 $'#define DOUBLE(x) ((x)*2)\nint main(){return DOUBLE(6);}'

# 4. #undef + redefine
assert 4 $'#define X 3\n#undef X\n#define X 4\nint main(){return X;}'

# 5. #ifdef — taken branch
assert 1 $'#define DEBUG\n#ifdef DEBUG\nint main(){return 1;}\n#else\nint main(){return 2;}\n#endif'

# 6. #ifdef — else branch taken (macro not defined)
assert 2 $'#ifdef NOTDEFINED\nint main(){return 1;}\n#else\nint main(){return 2;}\n#endif'

# 7. #ifndef — taken when not defined
assert 9 $'#ifndef NOTDEFINED\nint main(){return 9;}\n#endif'

# 8. #ifndef — skipped when defined
assert 3 $'#define GUARD\n#ifndef GUARD\nint main(){return 1;}\n#endif\nint main(){return 3;}'

# 9. Nested #ifdef (both defined)
assert 5 $'#define A\n#define B\n#ifdef A\n#ifdef B\nint main(){return 5;}\n#endif\n#endif'

# 10. Nested #ifdef — outer false skips inner
assert 6 $'#ifdef NOTDEFINED\n#define B\n#endif\n#ifndef B\nint main(){return 6;}\n#endif'

# 11. Line continuation in macro body
assert 10 $'#define TEN \\\n10\nint main(){return TEN;}'

# 12. Nested function-like macro calls
assert 15 $'#define MUL(a,b) ((a)*(b))\n#define SQ(x) MUL(x,x)\nint main(){return SQ(3)+MUL(2,3);}'

# 13. Object macro used multiple times
assert 8 $'#define V 4\nint main(){return V+V;}'

# 14. #include test — write header to _tmp_hdr.h, include it
printf '#define HEADERVAL 42\n' > _tmp_hdr.h
assert 42 $'#include "_tmp_hdr.h"\nint main(){return HEADERVAL;}'
rm -f _tmp_hdr.h

# 15. #include with macro that expands to expression
printf '#define LIMIT 100\n#define HALF (LIMIT/2)\n' > _tmp_hdr.h
assert 50 $'#include "_tmp_hdr.h"\nint main(){return HALF;}'
rm -f _tmp_hdr.h

echo "preprocess tests OK"

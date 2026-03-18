## string literals

# local char pointer
assert 104  "int main(){char *p=\"hello\"; return p[0];}"

# index into pointer
assert 101  "int main(){char *p=\"hello\"; return p[1];}"

# string literal subscript directly
assert 101  "int main(){return \"hello\"[1];}"

# adjacent string concatenation
assert 108  "int main(){char *p=\"hel\" \"lo\"; return p[3];}"

# string passed to function
assert 104  "int f(char *s){return s[0];} int main(){return f(\"hello\");}"

# global char pointer
assert 114  "char *g=\"world\"; int main(){return g[2];}"

# null terminator
assert 0    "int main(){char *p=\"hi\"; return p[2];}"

# escape sequence in string
assert 10   "int main(){return \"a\\nb\"[1];}"

# global char array initialized from string
assert 111  "char s[6]=\"hello\"; int main(){return s[4];}"

# global char array null terminator
assert 0    "char s[6]=\"hello\"; int main(){return s[5];}"

# local char array from string (via char s[] deduced size)
assert 104  "int main(){char s[]=\"hello\"; return s[0];}"

# multiple strings in one function
assert 1    "int main(){char *a=\"abc\"; char *b=\"def\"; return a[0]-96;}"

# global array of char* pointers initialised with string literals — was broken: gen_initlist
# emitted zeros for symbolic addresses; now correctly emits IR_WORD label per element
assert 97   "char *words[3]={\"foo\",\"bar\",\"baz\"}; int main(){return words[1][1];}"
assert 102  "char *words[3]={\"foo\",\"bar\",\"baz\"}; int main(){return words[0][0];}"
assert 98   "char *words[3]={\"foo\",\"bar\",\"baz\"}; int main(){return words[2][0];}"

# same pattern with casts — strips ND_CAST wrapper to find underlying string literal
assert 120  "typedef unsigned char u8; static u8 *p[3]={(u8*)\"abc\",(u8*)\"xyz\",(u8*)\"123\"}; int main(){return p[1][0];}"




assert 1 "int main() {char a[4] = {1,2,3,4}; return a[0];}"
assert 2 "int main() {char a[4] = {1,2,3,4}; return a[1];}"
assert 1 "int main() {char a[4] = {1,2,3,4}; return *a;}"
assert 3 "int main() {char a[4] = {1,2,3,4}; return *(a+2);}"
assert 1 "int main() {int a[4] = {1,2,3,4}; return a[0];}"
assert 2 "int main() {int a[4] = {1,2,3,4}; return a[1];}"
assert 1 "int main() {int a[4] = {1,2,3,4}; return *a;}"
assert 3 "int main() {int a[4] = {1,2,3,4}; return *(a+2);}"

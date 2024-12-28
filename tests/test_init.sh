assert 1 "int main() {int a=1; return a;}"
assert 3 "int main() {int a[2] = {3,4}; return a[0];}"
assert 4 "int main() {int a[2] = {3,4}; return a[1];}"
assert 0 "int main() {int a[2] = {3}; return a[1];}"



assert 1 "int main() {int a[3][3] = {1,{2,3},4}; return a[0][0];}"
assert 3 "int main() {int a[3][3] = {1,{2,3},4}; return a[1][1];}"
assert 3 "int main() {int a[2][2][2] = {1,5,{2,3}}; return a[1][0][1];}"
# assert 10 "int main() {int a[3][3] = {1,{2,3},4}; return a[0][0]+a[0][1]+a[0][2]+a[1][0]+a[1][1]+a[1][2]+a[2][0]+a[2][1]+a[2][2]"

assert 10 "int a=10; int main(){return a;}"

assert 3 "int a[10]={2,3};int main() {return a[1];}"

## Compound assignment operators

assert 7  "int main(){int a=5; a+=2; return a;}"
assert 3  "int main(){int a=5; a-=2; return a;}"
assert 10 "int main(){int a=5; a*=2; return a;}"
assert 2  "int main(){int a=6; a/=3; return a;}"
assert 1  "int main(){int a=1; a&=3; return a;}"
assert 7  "int main(){int a=5; a|=2; return a;}"
assert 6  "int main(){int a=5; a^=3; return a;}"
assert 8  "int main(){int a=1; a<<=3; return a;}"
assert 4  "int main(){int a=32; a>>=3; return a;}"

# chained
assert 10 "int main(){int a=1; a+=4; a*=2; return a;}"

# compound assign in loop
assert 10 "int main(){int s=0;int i=0;while(i<5){s+=i;i+=1;}return s;}"

# compound assign to array element
assert 9  "int main(){int a[3]={3,3,3}; a[1]+=6; return a[1];}"

# compound assign with side-effecting index: a[i++]+=10 must evaluate i++ once only
assert 14 "int main(){int a[3]={1,2,3}; int i=1; a[i++]+=10; return a[1]+i;}"

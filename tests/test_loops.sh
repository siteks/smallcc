
## Loops: for, do-while, switch, break, continue

# for loop basic
assert 10 "int main(){int i=0;int s=0;for(i=0;i<5;i=i+1)s=s+i;return s;}"

# do-while
assert 0 "int main(){int i=5;do{i=i-1;}while(i>0);return i;}"

# switch basic
assert 3 "int main(){int x=2;switch(x){case 1:return 1;case 2:return 3;default:return 0;}return 0;}"

# while with break
assert 7 "int main(){int i=0;while(1){i=i+1;if(i==7)break;}return i;}"

# for with break
assert 5 "int main(){int s=0;int i;for(i=0;i<10;i=i+1){if(i==5)break;s=s+1;}return s;}"

# for with continue (skip i==2, sum 0+1+3+4=8)
assert 8 "int main(){int s=0;int i;for(i=0;i<5;i=i+1){if(i==2)continue;s=s+i;}return s;}"

# switch with no match, no default
assert 0 "int main(){int x=5;switch(x){case 1:return 1;case 2:return 2;}return 0;}"

# switch fall-through (x=1: case 1 runs x+1=2, falls to case 2 runs x+1=3)
assert 3 "int main(){int x=1;switch(x){case 1:x=x+1;case 2:x=x+1;}return x;}"

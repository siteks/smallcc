#!/bin/bash

assert() {
    expected="$1"
    input="$2"
    ./mycc "$input" > tmp.s
    actual=`./cpu3/sim.py tmp.s |tail -1`
    # echo $actual
    final=`echo $actual|awk '{v=int("0x"substr($1,4));if (v>32767)v=v-65536;print v}'`

    if [[ $final = $expected ]]; then
        echo "$input => $final"
    else
        echo "$input => $expected expected, but got $final - $actual"
        ./cpu2/sim.py tmp.s -v >error.log
        exit 1
    fi
}



## Operators
# assert 5 "int main(){return 1+4;}"
# assert 1 "int main(){return 1<2;}"
# assert 0 "int main(){return 2<2;}"
# assert 0 "int main(){return 3<2;}"
# assert 1 "int main(){return 1<=2;}"
# assert 1 "int main(){return 2<=2;}"
# assert 0 "int main(){return 3<=2;}"
# assert 0 "int main(){return 1>2;}"
# assert 0 "int main(){return 2>2;}"
# assert 1 "int main(){return 3>2;}"
# assert 0 "int main(){return 1>=2;}"
# assert 1 "int main(){return 2>=2;}"
# assert 1 "int main(){return 3>=2;}"
# assert 1 "int main(){return 2==2;}"
# assert 0 "int main(){return 2==3;}"
# assert 0 "int main(){return 2!=2;}"
# assert 21 "int main(){int a=10;int b=11; return a+b;}"
# assert 9 "int main(){int a=2;int b=3; return a * b+3;}"
# assert 5 "int main(){if (2>3) return 4; return 5;}"
# assert 4 "int main(){if (2<3) return 4; return 5;}"
# assert 5 "int main(){if (2>3) return 4; else return 5;}"
# assert 4 "int main(){if (2<3) return 4; else return 5;}"

# assert 7 "int main(){int a; {a=5;a=a+2;return a;} }"
# assert 5 "int main(){int a=0; while(a < 5) a=a+1; return a;}"
# assert 11 "int main(){int a=0;int b=0; {a=a+1;b=b+a+10;} return b;}"
# assert 15 "int main(){int a=0;int b=0; while(a<5) {a=a+1;b=b+a;} return b;}"
# assert -1 "int main(){return -1;}"
# assert 1 "int main(){float b;return 1;}"

# Scope
# assert 2 "int main(){int a=1;{int a=2;return a;}}"
# assert 1 "int main(){int a=1;{int a=2;}return a;}"

# Pointers
# assert 1 "int main(){int *a;a=0x2000;*a=1;return *a;}"
# assert 1 "int main(){int *a=0x2000;*a=1;return *a;}"
assert 3 "int main(){int a[2]; a[0]=2;a[1]=3;return a[1];}"


echo OK

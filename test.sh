#!/bin/bash

assert() {
    expected="$1"
    input="$2"
    ./mycc "$input" > tmp.s
    actual=`./cpu2/sim.py tmp.s |tail -1`
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

# assert 0 0
# assert 76 76
# assert 5 5

# assert 21 "5+20-4"
# assert 43 " 7 + 205 - 1 - 168"
# assert 13 "(5+7) - (10-2) + (2+(3+4))"
# assert 6 "2*3"
# assert 115 "23 * 5"

# assert -1 "-1"
# assert 10 "9 - - 1"
# assert 0 "1==2"
# assert 1 "1==1"


# assert 1 "int main(){int a=1; return a;}"
# assert 0 "2 > 2"
# assert 1 "3 > 2"
# assert 1 "2 >= 1"
# assert 1 "2 >= 2"
# assert 0 "2 >= 3"
# assert 1 "1 < 2"
# assert 0 "2 < 2"
# assert 0 "3 < 2"
# assert 0 "2 <= 1"
# assert 1 "2 <= 2"
# assert 1 "2 <= 3"



# assert 5 "int main(){return 1+4;}"
# assert 21 "int main(){int a=10;int b=11; return a+b;}"
# assert 9 "int main(){int a=2;int b=3; return a* b+3;}"
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
# assert 5 "int main(){if (2>3) return 4; return 5;}"
# assert 4 "int main(){if (2<3) return 4; return 5;}"
# assert 5 "int main(){if (2>3) return 4; else return 5;}"
# assert 4 "int main(){if (2<3) return 4; else return 5;}"

# assert 7 "int main(){int a; {a=5;a=a+2;return a;} }"
# assert 5 "int main(){int a=0; while(a < 5) a=a+1; return a;}"
# assert 11 "int main(){int a=0;int b=0; {a=a+1;b=b+a+10;} return b;}"
assert 15 "int main(){int a=0;int b=0; while(a<5) {a=a+1;b=b+a;} return b;}"
# assert 5 "a=2;b=3;c=a+b; return c;"
# assert 6 "a=2;b=3;c=a*b; return c;"
# assert 10 "a=2;b=3;c=a*b; return c+4;"

echo OK

#!/bin/bash

assert() {
    expected="$1"
    input="$2"
    ./mycc "$input" > tmp.s
    actual=`./cpu2/sim.py tmp.s`
    # echo $actual
    final=`echo $actual|awk '{v=int("0x"substr($1,4));if (v>32767)v=v-65536;print v}'`

    if [[ $final = $expected ]]; then
        echo "$input => $final"
    else
        echo "$input => $expected expected, but got $final - $actual"
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


assert 0 "return 1 > 2;"
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

assert 1 "a=1;return a;"
assert 3 "a=1;b=2; return a+b;"
assert 6 "a=2;b=3; return a* b;"
assert 5 "a=2;b=3;c=a+b; return c;"
assert 6 "a=2;b=3;c=a*b; return c;"
assert 10 "a=2;b=3;c=a*b; return c+4;"

echo OK

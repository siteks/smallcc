#!/bin/bash

assert() {
    expected="$1"
    input="$2"
    ./mycc "$input" > tmp.s
    actual=`./cpu/sim.py tmp.s`
    final=`echo $actual|awk '{v=int("0x"substr($1,4));if (v>32767)v=v-65536;print v}'`

    if [[ $final = $expected ]]; then
        echo "$input => $final"
    else
        echo "$input => $expected expected, but got $final - $actual"
        exit 1
    fi
}

assert 0 0
assert 76 76
assert -5 -5

echo OK

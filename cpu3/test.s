
    ssp     0x1000
    immw    3
    push
    jl      main
    halt

main:
    enter   2
    lea     1
    lw
    push
    immw    2
    add
    ret



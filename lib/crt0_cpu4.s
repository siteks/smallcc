.text=0
    immw    r0, 0xf000
    ssp     r0
    clearmem _globals_start
    jl      main
    halt

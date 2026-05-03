.text=0
    immw    r0, 0xf000
    ssp     r0
    clearmem _bss_start, _bss_end
    jl      main
    halt

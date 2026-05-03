.text=0
    immw    r0, 0xf000
    ssp     r0
    ; Zero the BSS region [_bss_start .. _bss_end).
    ; On real hardware there is no load-time image magic, so clear explicitly.
    immw    r1, _bss_start
    immw    r2, _bss_end
    zero3
_bss_init_loop:
    beq     r1, r2, _bss_init_done
    slb     r3, r1, 0
    inc     r1
    j       _bss_init_loop
_bss_init_done:
    ; Fill the 80x30 framebuffer at 0xf000 with ASCII spaces.
    immw    r1, 0xf000
    immw    r2, 2400
    immw    r3, 32
_fb_init_loop:
    slb     r3, r1, 0
    inc     r1
    dec     r2
    jnz     r2, _fb_init_loop
    jl      main
    halt

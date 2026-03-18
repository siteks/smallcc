.text=0
    ssp     0x1000              ; F3a: set stack pointer
    jl      main                ; F3a: call main
    halt                        ; F0:  stop

main:
    enter   8                   ; F3a: save frame, allocate 8 bytes for locals
    immw    r1, 10              ; F3c: r1 = 10
    immw    r2, 20              ; F3c: r2 = 20
    add     r0, r1, r2          ; F1a: r0 = 30
    sw      r0, -1              ; F2:  mem[bp-2] = r0  (imm -1 * 2 = -2)
    lw      r3, -1              ; F2:  r3 = mem[bp-2] = 30
    beq     r0, r3, same        ; F3b: branch if r0==r3 (taken)
    immw    r0, 99              ; skipped
same:
    lea     r4, -2              ; F3c: r4 = bp - 2
    llw     r5, r4, 0           ; F3b: r5 = mem16[r4 + 0] = 30
    add     r0, r0, r5          ; F1a: r0 = 30 + 30 = 60
    ret                         ; F0:  return  (r0 = 60 = 0x3c)

@ The shader modes' .gxp bytes as the symbols win.c names (README).
    .section .rodata
    .balign 16
    .global lcd3x_v
lcd3x_v:
    .incbin "lcd3x_v.gxp"
    .balign 16
    .global lcd3x_f
lcd3x_f:
    .incbin "lcd3x_f.gxp"
    .balign 16
    .global fxaa_v
fxaa_v:
    .incbin "fxaa_v.gxp"
    .balign 16
    .global fxaa_f
fxaa_f:
    .incbin "fxaa_f.gxp"

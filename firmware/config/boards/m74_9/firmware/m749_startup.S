.syntax unified
.cpu cortex-m4
.thumb
.section .m749_startup,"ax",%progbits
.global Reset_Handler
.type Reset_Handler,%function
.thumb_func
Reset_Handler:
    cpsid i
    ldr r0, =0x08001000
    ldr r1, =0xE000ED08
    str r0, [r1]
    dsb
    isb
    b _crt0_entry
.size Reset_Handler, .-Reset_Handler

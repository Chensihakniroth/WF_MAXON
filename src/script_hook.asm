OPTION CASEMAP:NONE

EXTERN ScriptHookDispatch:PROC
PUBLIC ScriptHookStub

.code

; Context layout at RSP+40h:
;   +00..+78  rcx, rdx, r8, r9, rax, r10, r11, rbx, rbp, rdi, rsi, r12-r15, rsp
;   +80..+17F full 128-bit XMM0..XMM15 values
;   +180      skip byte
;   +188      original trampoline pointer
;
; The stub saves the full game context, calls ScriptHookDispatch, then either:
;   - returns to the game's caller (skip=true)
;   - jumps through the trampoline (skip=false)

ScriptHookStub PROC
    sub     rsp, 2F8h

    mov     qword ptr [rsp+40h+00h], rcx
    mov     qword ptr [rsp+40h+08h], rdx
    mov     qword ptr [rsp+40h+10h], r8
    mov     qword ptr [rsp+40h+18h], r9
    mov     qword ptr [rsp+40h+20h], rax
    mov     qword ptr [rsp+40h+28h], r10
    mov     qword ptr [rsp+40h+30h], r11
    mov     qword ptr [rsp+40h+38h], rbx
    mov     qword ptr [rsp+40h+40h], rbp
    mov     qword ptr [rsp+40h+48h], rdi
    mov     qword ptr [rsp+40h+50h], rsi
    mov     qword ptr [rsp+40h+58h], r12
    mov     qword ptr [rsp+40h+60h], r13
    mov     qword ptr [rsp+40h+68h], r14
    mov     qword ptr [rsp+40h+70h], r15

    lea     rax, [rsp+2F8h]
    mov     qword ptr [rsp+40h+78h], rax

    movdqu  xmmword ptr [rsp+0C0h], xmm0
    movdqu  xmmword ptr [rsp+0D0h], xmm1
    movdqu  xmmword ptr [rsp+0E0h], xmm2
    movdqu  xmmword ptr [rsp+0F0h], xmm3
    movdqu  xmmword ptr [rsp+100h], xmm4
    movdqu  xmmword ptr [rsp+110h], xmm5
    movdqu  xmmword ptr [rsp+120h], xmm6
    movdqu  xmmword ptr [rsp+130h], xmm7
    movdqu  xmmword ptr [rsp+140h], xmm8
    movdqu  xmmword ptr [rsp+150h], xmm9
    movdqu  xmmword ptr [rsp+160h], xmm10
    movdqu  xmmword ptr [rsp+170h], xmm11
    movdqu  xmmword ptr [rsp+180h], xmm12
    movdqu  xmmword ptr [rsp+190h], xmm13
    movdqu  xmmword ptr [rsp+1A0h], xmm14
    movdqu  xmmword ptr [rsp+1B0h], xmm15

    mov     byte ptr [rsp+1C0h], 0
    lea     rcx, [rsp+40h]
    call    ScriptHookDispatch

    lea     r10, [rsp+40h]
    cmp     byte ptr [r10+180h], 0
    jne     ScriptHookSkipOriginal

ScriptHookRunOriginal:
    ; Jump through trampoline to continue original code.
    mov     rax, qword ptr [r10+188h]
    mov     rcx, qword ptr [r10+78h]
    mov     qword ptr [rcx-8], rax
    mov     qword ptr [rsp+20h], rcx
    mov     qword ptr [rsp+28h], 1

    mov     rcx, qword ptr [r10+00h]
    mov     rdx, qword ptr [r10+08h]
    mov     r8,  qword ptr [r10+10h]
    mov     r9,  qword ptr [r10+18h]
    mov     rax, qword ptr [r10+20h]
    mov     r11, qword ptr [r10+30h]
    mov     rbx, qword ptr [r10+38h]
    mov     rbp, qword ptr [r10+40h]
    mov     rdi, qword ptr [r10+48h]
    mov     rsi, qword ptr [r10+50h]
    mov     r12, qword ptr [r10+58h]
    mov     r13, qword ptr [r10+60h]
    mov     r14, qword ptr [r10+68h]
    mov     r15, qword ptr [r10+70h]
    movdqu  xmm0, xmmword ptr [r10+80h]
    movdqu  xmm1, xmmword ptr [r10+90h]
    movdqu  xmm2, xmmword ptr [r10+0A0h]
    movdqu  xmm3, xmmword ptr [r10+0B0h]
    movdqu  xmm4, xmmword ptr [r10+0C0h]
    movdqu  xmm5, xmmword ptr [r10+0D0h]
    movdqu  xmm6, xmmword ptr [r10+0E0h]
    movdqu  xmm7, xmmword ptr [r10+0F0h]
    movdqu  xmm8, xmmword ptr [r10+100h]
    movdqu  xmm9, xmmword ptr [r10+110h]
    movdqu  xmm10, xmmword ptr [r10+120h]
    movdqu  xmm11, xmmword ptr [r10+130h]
    movdqu  xmm12, xmmword ptr [r10+140h]
    movdqu  xmm13, xmmword ptr [r10+150h]
    movdqu  xmm14, xmmword ptr [r10+160h]
    movdqu  xmm15, xmmword ptr [r10+170h]
    mov     r10, qword ptr [r10+28h]
    mov     rsp, qword ptr [rsp+20h]
    sub     rsp, 8
    ret

ScriptHookSkipOriginal:
    ; skip=true: restore registers, restore original stack pointer, and return to caller!
    mov     rcx, qword ptr [r10+78h]
    mov     qword ptr [rsp+20h], rcx

    mov     rcx, qword ptr [r10+00h]
    mov     rdx, qword ptr [r10+08h]
    mov     r8,  qword ptr [r10+10h]
    mov     r9,  qword ptr [r10+18h]
    mov     rax, qword ptr [r10+20h]
    mov     r11, qword ptr [r10+30h]
    mov     rbx, qword ptr [r10+38h]
    mov     rbp, qword ptr [r10+40h]
    mov     rdi, qword ptr [r10+48h]
    mov     rsi, qword ptr [r10+50h]
    mov     r12, qword ptr [r10+58h]
    mov     r13, qword ptr [r10+60h]
    mov     r14, qword ptr [r10+68h]
    mov     r15, qword ptr [r10+70h]
    movdqu  xmm0, xmmword ptr [r10+80h]
    movdqu  xmm1, xmmword ptr [r10+90h]
    movdqu  xmm2, xmmword ptr [r10+0A0h]
    movdqu  xmm3, xmmword ptr [r10+0B0h]
    movdqu  xmm4, xmmword ptr [r10+0C0h]
    movdqu  xmm5, xmmword ptr [r10+0D0h]
    movdqu  xmm6, xmmword ptr [r10+0E0h]
    movdqu  xmm7, xmmword ptr [r10+0F0h]
    movdqu  xmm8, xmmword ptr [r10+100h]
    movdqu  xmm9, xmmword ptr [r10+110h]
    movdqu  xmm10, xmmword ptr [r10+120h]
    movdqu  xmm11, xmmword ptr [r10+130h]
    movdqu  xmm12, xmmword ptr [r10+140h]
    movdqu  xmm13, xmmword ptr [r10+150h]
    movdqu  xmm14, xmmword ptr [r10+160h]
    movdqu  xmm15, xmmword ptr [r10+170h]
    mov     r10, qword ptr [r10+28h]
    mov     rsp, qword ptr [rsp+20h]
    ret
ScriptHookStub ENDP

END

OPTION CASEMAP:NONE
PUBLIC XmmProbeInvoke

.code

; Call a target while loading known full-width values into the nonvolatile
; XMM6..XMM15 registers, then copy the post-call values to the output buffer.
; The probe restores the caller's original nonvolatile XMM registers before
; returning, so it is ABI-neutral to the C++ test harness.
XmmProbeInvoke PROC
    sub     rsp, 0D8h
    mov     qword ptr [rsp+0C0h], rcx
    mov     qword ptr [rsp+0C8h], rdx

    movdqu  xmmword ptr [rsp+020h], xmm6
    movdqu  xmmword ptr [rsp+030h], xmm7
    movdqu  xmmword ptr [rsp+040h], xmm8
    movdqu  xmmword ptr [rsp+050h], xmm9
    movdqu  xmmword ptr [rsp+060h], xmm10
    movdqu  xmmword ptr [rsp+070h], xmm11
    movdqu  xmmword ptr [rsp+080h], xmm12
    movdqu  xmmword ptr [rsp+090h], xmm13
    movdqu  xmmword ptr [rsp+0A0h], xmm14
    movdqu  xmmword ptr [rsp+0B0h], xmm15

    lea     rax, Pattern6
    movdqu  xmm6, xmmword ptr [rax]
    lea     rax, Pattern7
    movdqu  xmm7, xmmword ptr [rax]
    lea     rax, Pattern8
    movdqu  xmm8, xmmword ptr [rax]
    lea     rax, Pattern9
    movdqu  xmm9, xmmword ptr [rax]
    lea     rax, Pattern10
    movdqu  xmm10, xmmword ptr [rax]
    lea     rax, Pattern11
    movdqu  xmm11, xmmword ptr [rax]
    lea     rax, Pattern12
    movdqu  xmm12, xmmword ptr [rax]
    lea     rax, Pattern13
    movdqu  xmm13, xmmword ptr [rax]
    lea     rax, Pattern14
    movdqu  xmm14, xmmword ptr [rax]
    lea     rax, Pattern15
    movdqu  xmm15, xmmword ptr [rax]

    mov     rcx, qword ptr [rsp+0C0h]
    call    rcx

    mov     rdx, qword ptr [rsp+0C8h]
    movdqu  xmmword ptr [rdx+000h], xmm6
    movdqu  xmmword ptr [rdx+010h], xmm7
    movdqu  xmmword ptr [rdx+020h], xmm8
    movdqu  xmmword ptr [rdx+030h], xmm9
    movdqu  xmmword ptr [rdx+040h], xmm10
    movdqu  xmmword ptr [rdx+050h], xmm11
    movdqu  xmmword ptr [rdx+060h], xmm12
    movdqu  xmmword ptr [rdx+070h], xmm13
    movdqu  xmmword ptr [rdx+080h], xmm14
    movdqu  xmmword ptr [rdx+090h], xmm15

    movdqu  xmm6, xmmword ptr [rsp+020h]
    movdqu  xmm7, xmmword ptr [rsp+030h]
    movdqu  xmm8, xmmword ptr [rsp+040h]
    movdqu  xmm9, xmmword ptr [rsp+050h]
    movdqu  xmm10, xmmword ptr [rsp+060h]
    movdqu  xmm11, xmmword ptr [rsp+070h]
    movdqu  xmm12, xmmword ptr [rsp+080h]
    movdqu  xmm13, xmmword ptr [rsp+090h]
    movdqu  xmm14, xmmword ptr [rsp+0A0h]
    movdqu  xmm15, xmmword ptr [rsp+0B0h]
    add     rsp, 0D8h
    ret
XmmProbeInvoke ENDP

.data
ALIGN 16
Pattern6  DB 00h, 11h, 22h, 33h, 44h, 55h, 66h, 77h, 88h, 99h, 0AAh, 0BBh, 0CCh, 0DDh, 0EEh, 0FFh
Pattern7  DB 10h, 32h, 54h, 76h, 98h, 0BAh, 0DCh, 0FEh, 01h, 23h, 45h, 67h, 89h, 0ABh, 0CDh, 0EFh
Pattern8  DB 0F0h, 0E1h, 0D2h, 0C3h, 0B4h, 0A5h, 096h, 087h, 078h, 069h, 05Ah, 04Bh, 03Ch, 02Dh, 01Eh, 00Fh
Pattern9  DB 01h, 23h, 45h, 67h, 89h, 0ABh, 0CDh, 0EFh, 10h, 32h, 54h, 76h, 98h, 0BAh, 0DCh, 0FEh
Pattern10 DB 0FFh, 0EEh, 0DDh, 0CCh, 0BBh, 0AAh, 099h, 088h, 077h, 066h, 055h, 044h, 033h, 022h, 011h, 00h
Pattern11 DB 05h, 0Ah, 0Fh, 014h, 019h, 01Eh, 023h, 028h, 02Dh, 032h, 037h, 03Ch, 041h, 046h, 04Bh, 050h
Pattern12 DB 0A5h, 05Ah, 0C3h, 03Ch, 096h, 069h, 0F0h, 00Fh, 069h, 096h, 03Ch, 0C3h, 05Ah, 0A5h, 0Fh, 0F0h
Pattern13 DB 03h, 06h, 09h, 0Ch, 0Fh, 012h, 015h, 018h, 01Bh, 01Eh, 021h, 024h, 027h, 02Ah, 02Dh, 030h
Pattern14 DB 0Fh, 01Eh, 02Dh, 03Ch, 04Bh, 05Ah, 069h, 078h, 087h, 096h, 0A5h, 0B4h, 0C3h, 0D2h, 0E1h, 0F0h
Pattern15 DB 0AAh, 0BBh, 0CCh, 0DDh, 0EEh, 0FFh, 00h, 011h, 022h, 033h, 044h, 055h, 066h, 077h, 088h, 099h

END

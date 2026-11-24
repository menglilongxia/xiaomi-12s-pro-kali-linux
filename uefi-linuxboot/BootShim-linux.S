/*
 * BootShim-linux.S — Overlap-safe BootShim for Xiaomi 12S Pro (unicorn) UEFI + Linux XNUP payload.
 * Copies XNUP payload (at _Payload + FD_SIZE) to PAYLOAD_BASE FIRST, then copies UEFI FD to FD_BASE,
 * then jumps to FD_BASE. Includes .ltorg before _Payload so literal pools never shift _Payload.
 */
_Head:
  adr   x1, _Payload
  b     _Start

_StackBase:
  .quad FD_BASE         /* Text Offset */

_StackSize:
  .quad FD_SIZE         /* Image Size */
  .quad 0               /* Flags */
  .quad 0               /* Reserved */
  .quad 0               /* Reserved */
  .quad 0               /* Reserved */
  .ascii "ARM\x64"      /* ARM64 Magic */
  .long 0               /* Reserved */

_Start:
  /* 1. Copy XNUP payload FIRST (x4 = _Payload + FD_SIZE -> PAYLOAD_BASE) */
  ldr   x6, =FD_SIZE
  add   x4, x1, x6
  ldr   w2, [x4]
  movz  w3, #0x4E58
  movk  w3, #0x5055, lsl #16
  cmp   w2, w3
  b.ne  _CopyFd

  ldr   w6, [x4, #4]    /* Payload total size from XNUP header */
  add   x6, x6, #23     /* +8 header + round up to 16 */
  and   x6, x6, #~15
  ldr   x5, =PAYLOAD_BASE

_PayloadLoop:
  ldp   x2, x3, [x4], #16
  stp   x2, x3, [x5], #16
  subs  x6, x6, #16
  b.ne  _PayloadLoop

_CopyFd:
  /* 2. Copy UEFI FD (_Payload -> FD_BASE) */
  mov   x4, x1
  ldr   x5, =FD_BASE
  cmp   x4, x5
  beq   _Entry
  ldr   x6, =FD_SIZE

_CopyLoop:
  ldp   x2, x3, [x4], #16
  stp   x2, x3, [x5], #16
  subs  x6, x6, #16
  b.ne  _CopyLoop

_Entry:
  ldr   x5, =FD_BASE
  br    x5

_Dead:
  b     _Dead

  .ltorg

.text
.align 4

_Payload:

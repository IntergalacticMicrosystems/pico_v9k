; ============================================================================
; keyboard.asm - Polled, interrupt-free, buffer-free Victor 9000 keyboard driver
; ============================================================================
; A tiny standalone module for the boot ROM.  Its job: let a future ROM feature
; do "Press F2 for setup" during the boot window, WITHOUT installing any
; interrupt vector, allocating any RAM, or disturbing the byte-exact main ROM.
;
; It is a synchronous, polled re-implementation of the DOS 3.1 keyboard driver
; (LLM-md-files/vic-src/dos31/KB.PLM).  Every VIA-4 interrupt source is masked
; (IER = 7Fh); we watch the Interrupt Flag Register (IFR) by polling instead of
; letting the PIC fire.  No buffers, no state variable in RAM - the 3-state
; keyboard handshake is walked to completion inside one call, all in registers.
; A 16-byte nibble-reverse lookup table lives in the code segment and is read
; with a CS: segment override (cs xlatb).
;
; Assembly model:
;   - Assembles standalone:  nasm -f bin -O0 keyboard.asm -o kb.bin
;   - Also usable via %include from universal.asm (no `org` of its own).
;
; Target: 8088/8086 only.  `cpu 8086` below makes NASM reject 186+ encodings
; (e.g. `shl al,4`); on the 8086 a shift is by 1 or by CL, so we use `mov cl,4`.
; ============================================================================

[bits 16]
[cpu 8086]

; ----------------------------------------------------------------------------
; Hardware: keyboard 6522 VIA ("VIA 4")
; ----------------------------------------------------------------------------
; Physical address 0E8040h.  Reached here as segment 0E000h : offset 8040h
; (matches EXTRA_IO_SEG / keyboard0 in bt1init_nasm.asm and via(4) in KB.PLM,
;  where via$ptr = 0E8000h and via(4) = via$ptr + 4*16 = 0E8040h).
;
; ES is loaded with 0E000h at the top of every public routine, so callers need
; not pre-set it (ES is trashed - documented per routine below).
; ----------------------------------------------------------------------------
KB_IO_SEG	equ 0E000h	; segment for all VIA-4 byte accesses (ES)
KB_BASE		equ 8040h	; offset of VIA-4 register 0 within KB_IO_SEG

; Byte register offsets from KB_BASE (6522 register map):
KB_ORB		equ KB_BASE+000h	; +0  ORB/IRB (port B). Access clears CB1/CB2 IFR flags
KB_ORA		equ KB_BASE+001h	; +1  ORA/IRA (port A, handshake). Read clears CA1/CA2 flags
KB_DDRB		equ KB_BASE+002h	; +2  data direction B
KB_DDRA		equ KB_BASE+003h	; +3  data direction A
KB_T2L		equ KB_BASE+008h	; +8  Timer 2 low  (read clears T2 IFR flag)
KB_T2H		equ KB_BASE+009h	; +9  Timer 2 high (write loads & starts T2 one-shot)
KB_SR		equ KB_BASE+00Ah	; +A  shift register (read clears SR IFR flag)
KB_ACR		equ KB_BASE+00Bh	; +B  auxiliary control (SR mode + T2 mode)
KB_PCR		equ KB_BASE+00Ch	; +C  peripheral control (CB1 edge sense)
KB_IFR		equ KB_BASE+00Dh	; +D  interrupt flag register (we POLL this)
KB_IER		equ KB_BASE+00Eh	; +E  interrupt enable register (we hold it masked)
;		KB_BASE+00Fh		; +F  ORA no-handshake (unused here)

; IFR / control bit masks (see KB.PLM equates):
KB_SR_FULL	equ 004h	; IFR bit2 : shift register full  (SR$intbit)
KB_CB1_EDGE	equ 010h	; IFR bit4 : CB1 (KB RDY) edge      (CB1$intbit / CB1$pos_edge in PCR)
KB_T2_FLAG	equ 020h	; IFR bit5 : Timer 2 timed out
KB_SR_ENABLE	equ 00Ch	; ACR bits2,3 : "shift in under external clock" (SR$enable)
KB_ACK		equ 002h	; ORB bit1 : KB ACK handshake output (kb$ackctl)
KB_DATABIT	equ 040h	; ORA bit6 : KB DATA level, read as stop-bit check (kb$databit)

; Timer 2 tick: PHI2 runs at 1 MHz (1 us/count; KB.PLM loads 50*1000 for a
; 50 ms tick, confirming the rate).  50000 counts = 50 ms one-shot interval.
KB_TICK		equ 50000	; = 0C350h  -> low 050h, high 0C3h
KB_TICK_LO	equ (KB_TICK & 0FFh)		; 050h
KB_TICK_HI	equ ((KB_TICK >> 8) & 0FFh)	; 0C3h

; Bounded spin for the two CB1-edge waits, so a wedged/unplugged keyboard can
; never hang the boot ROM.  ~65k iterations of {test mem; loop} ~= a few tens
; of ms on a 5 MHz 8088 - far longer than the real handshake, short enough to
; bail.  On timeout kb_poll_event re-arms and reports "no event".
; Overridable: %define KB_SPIN <n> before %include to shorten the bound
; (a caller that must not stall long, e.g. VIATERM's response drain).
%ifndef KB_SPIN
KB_SPIN		equ 0FFFFh
%endif

; F2 make code (see block comment at end of file):  0x82  (physical key 02h | 80h)
KB_F2_MAKE	equ 082h

; ============================================================================
; kb_init - one-time init + arm the SR for the first byte
; ----------------------------------------------------------------------------
; Polled variant of KB.PLM kb$init + kb$reset, with every IER-enable removed.
; Sets ES = KB_IO_SEG.  Trashes: AX, ES.  (AL only, plus ES load.)
; ============================================================================
kb_init:
	mov	ax, KB_IO_SEG
	mov	es, ax			; ES -> VIA-4 segment for [es:..] below

	and	byte [es:KB_ORB], 0FCh	; ORB &= ~3  : release KB ACK (bit1), clear bit0
	and	byte [es:KB_DDRA], 0BFh	; DDRA &= ~40h: PA6 (KB DATA) = input
	or	byte [es:KB_DDRB], KB_ACK ; DDRB |= 02h : PB1 (KB ACK) = output
	mov	byte [es:KB_IER], 07Fh	; mask ALL 7 VIA-4 interrupt sources (bit7=0 => disable)
	mov	byte [es:KB_IFR], 07Fh	; clear any stale interrupt flags
	mov	byte [es:KB_PCR], 000h	; PCR = 0
	mov	byte [es:KB_ACR], 000h	; ACR = 0  (SR off; T2 = one-shot timed-interrupt mode)

	call	kb_arm			; enable SR shift-in, ready for first scan code
	ret

; ============================================================================
; kb_arm - (internal) put the keyboard hardware back into "waiting for a byte"
; ----------------------------------------------------------------------------
; Polled kb$reset core: release ACK, enable SR shift-in-under-external-clock,
; and read SR once to clear any pending SR flag.  Assumes ES = KB_IO_SEG.
; Trashes: AL only (keeps AH intact - callers stash the raw byte there).
; ============================================================================
kb_arm:
	and	byte [es:KB_ORB], 0FDh	; ORB &= ~02h : release KB ACK
	or	byte [es:KB_ACR], KB_SR_ENABLE ; ACR |= 0Ch : shift in under external (KB) clock
	mov	al, [es:KB_SR]		; dummy read clears any pending SR flag
	ret

; ============================================================================
; kb_poll_event - poll for and, if present, fully receive one keyboard event
; ----------------------------------------------------------------------------
; If no byte is waiting (SR not full): returns immediately, CF=1 (no event).
; If a byte is waiting: walks the KB.PLM 3-state handshake to completion by
; polling IFR, decodes it, re-arms the hardware, and returns CF=0 with the
; event code in AL:  bit7=1 make (key down), bit7=0 break (key up), low 7 bits
; = physical key number.  On a stop-bit error or a handshake-edge timeout: the
; hardware is re-armed and CF=1 is returned (treated as "no event").
;
; Assumes nothing about ES (loads it).  Returns: AL, CF.  Trashes: AX, ES;
; preserves BX, CX, DX (pushed) so kb_wait_key's CX timeout survives.
; ============================================================================
kb_poll_event:
	push	cx
	push	bx
	push	dx

	mov	ax, KB_IO_SEG
	mov	es, ax			; ES -> VIA-4

	; --- Is a scan code waiting? (IFR bit2, SR full) -------------------
	test	byte [es:KB_IFR], KB_SR_FULL
	jz	.none			; nothing there -> no event

	; --- State 0 -> 1 : shift register full ---------------------------
	and	byte [es:KB_ACR], 0F3h	; ACR &= ~0Ch : disable shift register
	and	byte [es:KB_PCR], 0EFh	; PCR &= ~10h : CB1 sense = negative edge of KB RDY
	mov	al, [es:KB_SR]		; read raw scan code (clears SR flag)
	mov	ah, al			; stash raw byte in AH across the handshake
	or	byte [es:KB_ORB], KB_ACK ; assert KB ACK (ORB write also clears CB1 flag)

	; --- State 1 -> 2 : wait for negative edge of KB RDY (IFR bit4) ----
	; LATE-START NOTE: the keyboard runs its stop sequence (KB DATA + KB
	; RDY low, then park awaiting our ACK) unilaterally ~100us after the
	; 8th bit.  A caller that reacts to SR_FULL late finds that negative
	; edge already gone (it predates the ACK, so it was never latched with
	; the new sense).  The byte in the SR is still valid, and the keyboard
	; answers our ACK by raising KB DATA + KB RDY - so a timeout (or a
	; raced-ahead .got_neg) with PA6 HIGH means the handshake has in fact
	; completed: accept the byte instead of dropping it.
	mov	cx, KB_SPIN
.wait_neg:
	test	byte [es:KB_IFR], KB_CB1_EDGE
	jnz	.got_neg
	loop	.wait_neg
	test	byte [es:KB_ORA], KB_DATABIT ; no edge, but PA6 high =
	jnz	.accept			;   late start: byte completed anyway
	jmp	.rearm_err		; wedged keyboard -> bail, re-arm

.got_neg:
	test	byte [es:KB_ORA], KB_DATABIT ; PA6 already HIGH here = the
	jnz	.accept			;   keyboard raced ahead: accept
	or	byte [es:KB_PCR], 010h	; PCR |= 10h : CB1 sense = positive edge of KB RDY
	and	byte [es:KB_ORB], 0FDh	; release KB ACK (ORB write clears CB1 flag)

	; --- State 2 -> 0 : wait for positive edge of KB RDY (IFR bit4) ----
	mov	cx, KB_SPIN
.wait_pos:
	test	byte [es:KB_IFR], KB_CB1_EDGE
	jnz	.got_pos
	loop	.wait_pos
	jmp	.rearm_err		; wedged keyboard -> bail, re-arm

.got_pos:
	test	byte [es:KB_ORA], KB_DATABIT ; PA6 must be HIGH here...
	jz	.rearm_err		; ...else stop-bit error

	; --- Good event: re-arm for the next byte, then decode -------------
.accept:
	call	kb_arm			; (keeps AH = raw byte; releases ACK)

	; The SR shifts the byte in bit-reversed within each nibble; undo it
	; with the nibble table:  event = (Ctable[raw&0Fh] << 4) | Ctable[raw>>4]
	mov	bx, kb_ctable		; table base for xlat (offset in code seg)
	mov	al, ah
	and	al, 00Fh		; low nibble of raw
	cs	xlatb			; AL = Ctable[raw & 0Fh]   (CS-relative)
	mov	cl, 4
	shl	al, cl			; ...into the high nibble of the result
	mov	dl, al			; save it in DL
	mov	al, ah
	mov	cl, 4
	shr	al, cl			; high nibble of raw -> AL = raw >> 4
	cs	xlatb			; AL = Ctable[raw >> 4]
	or	al, dl			; AL = event code (make/break + key number)

	pop	dx
	pop	bx
	pop	cx
	clc				; CF=0 : event valid, code in AL
	ret

.rearm_err:				; stop-bit error or handshake timeout
	call	kb_arm			; put hardware back to state 0
.none:
	pop	dx
	pop	bx
	pop	cx
	stc				; CF=1 : no (valid) event
	ret

; ============================================================================
; kb_wait_key - wait up to CX*50ms for a key press, return the first MAKE
; ----------------------------------------------------------------------------
; The "press F2 within a few seconds" primitive.  Uses VIA-4's own Timer 2 as
; a polled 50 ms one-shot (no interrupts): each expiry decrements CX.
;
; Input:   CX = timeout in 50 ms ticks (e.g. CX=60 -> ~3 s).
; Returns: CF=0 and AL = make code of the first key pressed (bit7 set), or
;          CF=1 on timeout (CX exhausted).  Break events are ignored.
; Sets ES = KB_IO_SEG.  Trashes: AX, ES (and CX counts down); BX/DX preserved
; by kb_poll_event.
; ============================================================================
kb_wait_key:
	mov	ax, KB_IO_SEG
	mov	es, ax			; ES -> VIA-4 (for the T2 accesses below)
	jcxz	.timeout		; CX=0 would underflow to ~55 min of ticks

.tick:
	; Arm a fresh 50 ms Timer-2 one-shot: write low latch, then high byte
	; (writing T2H loads the counter, clears the T2 flag, and starts it).
	mov	byte [es:KB_T2L], KB_TICK_LO
	mov	byte [es:KB_T2H], KB_TICK_HI

.poll:
	call	kb_poll_event		; ES reloaded inside; CX preserved
	jc	.check_timer		; no event -> see if the tick elapsed
	test	al, 080h		; event present: is it a MAKE (key down)?
	jnz	.got_key		;   yes -> return it
	jmp	.poll			;   no (a break) -> keep waiting this tick

.check_timer:
	test	byte [es:KB_IFR], KB_T2_FLAG ; has the 50 ms one-shot expired?
	jz	.poll			; not yet -> keep polling within this tick
	mov	al, [es:KB_T2L]		; reading T2 low clears the T2 IFR flag
	dec	cx
	jz	.timeout		; ran out of ticks
	jmp	.tick			; start the next 50 ms tick

.got_key:
	clc				; CF=0 : AL = make code
	ret
.timeout:
	stc				; CF=1 : timed out, no key
	ret

; ============================================================================
; kb_ctable - nibble bit-reverse table (KB.PLM Ctable), read via cs xlatb
; ----------------------------------------------------------------------------
; Ctable[i] reverses the 4 bits of i (0->0, 1->8, 2->4, 3->C, ...).  Applied to
; each nibble of the SR byte to reconstruct the physical key number.
; ============================================================================
kb_ctable:
	db	000h, 008h, 004h, 00Ch, 002h, 00Ah, 006h, 00Eh
	db	001h, 009h, 005h, 00Dh, 003h, 00Bh, 007h, 00Fh

; ============================================================================
; F2 KEY CODE  (documentation)
; ----------------------------------------------------------------------------
; kb_poll_event / kb_wait_key return, per KB.PLM decode:
;     bit7 = 1 -> key pressed (make);  bit7 = 0 -> key released (break)
;     low 7 bits = physical key number
;
; F2's physical key number = 02h, so:
;     F2 make  = 82h   (02h | 80h)   <- what kb_wait_key returns for F2
;     F2 break = 02h
; (see KB_F2_MAKE equ above.)
;
; Sources (agree):
;   * MAME model - LLM-md-files/vic-src/mame-v9k/victor9k_kb.cpp:525
;         PORT_START("Y12") ... PORT_NAME("F2 [2]") PORT_CODE(KEYCODE_F2) // S02
;     i.e. F2 is switch S02.
;   * Victor KB emulator table - victor9k_remote_control/lib/victor9k_kb/
;         src/v9k_mappings.c:11   [KC_F2] = ONE(0x02)
;         src/v9k_target.c:5      #define V9K_MAKE 0x80  // make = code|0x80, break = bare code
;     i.e. F2 emits key number 0x02, make byte 0x82.
;   No source disagreement found; F2 -> 02h/82h is solid.
;
; Why F2 (bench fact): most surviving Victor keyboards physically lack F8-F10
; (see memory: victor-keyboard-function-keys), so an F2 "setup" hotkey is safe
; across the keyboards actually on the bench.  F1..F7 are all present.
; ============================================================================

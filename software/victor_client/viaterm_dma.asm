; ============================================================================
; viaterm.asm - VIATERM in pure 8086 assembly, zero DOS calls (ROM-bound).
; ============================================================================
; Same job as ../viaterm/viaterm.c: a Victor-side terminal to the v9k_flop
; console over the VIA bus (VIA-A T2/SR registers, phys 0xE80A8/A9/AA), with
; the VT100 response stream rendered by direct VRAM writes.  This version is
; the test vehicle for a future PRE-BOOT ROM feature, so it uses NO DOS
; services at all (the single INT 20h terminate is the DOS-hosted wrapper's
; exit, nothing else):
;   - keyboard: the ROM's own polled driver (%include ROM_NASM40/keyboard.asm,
;     kb_init/kb_poll_event - VIA-4 fully masked, handshake walked by polling
;     IFR).  TIMING CONTRACT (hard-won): the walk must START within ~100us of
;     SR_FULL - the keyboard runs its stop sequence unilaterally right after
;     the 8th bit, and a late starter finds the RDY edge already gone (0.5s
;     edge-wait burn -> the firmware's ~250ms full-FIFO patience -> [overflow]
;     + menu abort) or reads a torn SR (level-polling rewrite echoed shifted
;     garbage).  So: SR_FULL is checked every main-loop pass AND between every
;     drained byte, and KB_SPIN is shortened (%define below) so a rare late
;     start drops one key in ~8ms instead of stalling.  Key-number ->
;     ASCII layers inverted from the HW-verified
;     ASCII_TO_VICTOR table (victor9k_kb/src/v9k_mappings.c).  Alt IS the
;     Victor's Ctrl (Alt+C = 0x03 = quit); Alt+']' is the only bare ESC.
;   - screen: cells straight into VRAM at F000:0 (glyph = char + 0x64, attr
;     bits in the cell high byte: 80h reverse / 20h underline), hardware
;     cursor via CRTC R14/R15.  Cursor "hide" parks the cursor address at
;     2000 (off the displayed 0..1999 window) - CRTC R10 is never touched.
;   - exit restore: HW-verified 2026-07-12 that the Victor's HD46505 READS
;     BACK R12-R15 (start address + cursor; DEBUG stub, R12:13=01D0h after
;     57 scrolled lines - genuine 6845s are write-only here).  So entry saves
;     R12-R15 plus the full 2048-word VRAM (4KB, mirrored once - CON's
;     hw-scroll base wraps mod 4096 over it), and exit restores everything:
;     pixel-perfect DOS handback, no ESC E, no CON involvement.
;   - keyboard restore: VIA-4 IER/ACR/PCR saved before kb_init masks them,
;     restored at exit (IER: write 7Fh clear-all, then saved|80h set-saved),
;     handing the keyboard back to the DOS CI interrupt driver.
;
; Quit: type the line "exit" (intercepted locally, REPL mode only) or Alt+C
; (= Ctrl+C).  Arrows/CLR-HOME are sent (as VT100) only in menu mode, i.e.
; while the firmware has the cursor hidden - same rules as the C version.
;
; Build:  nasm -f bin -O0 -I<path-to-ROM_NASM40>/ viaterm.asm -o vtasm.com
; (see Makefile).  DOS 8.3 name on the Victor: VTASM.COM.
;
; -DROM_BUILD builds the ROM-loaded variant (the pre-boot feature itself):
; the boot ROM's F2 stub downloads it (prefixed by the full 4KB font) to
; 0:0C80 and far-jumps to 01B8h:0100h.  Nothing is inherited, so entry sets
; its own DS/ES/SS:SP; there is no DOS to hand back to, so the display/
; keyboard save+restore is dropped and both exits soft-boot the ROM
; (jmp 0FE00h:0).  It opens straight into the full-screen menu.
; ============================================================================

[bits 16]
[cpu 8086]
org 100h

; ---------------------------------------------------------------------------
; DMA-card host control channel.  DIFFERENT from the floppy card: the channel
; lives in the DMA register window at phys 0xEF340/50/60 (segment 0EF30h), NOT
; in the CRTC's 0E800h segment.  The CRTC and the keyboard VIA stay where they
; are (0E800h / 0E000h), so this file uses TWO IO segments: IO_SEG for the CRTC
; and CH_SEG for the channel (each channel access loads CH_SEG into ES; CRTC
; helpers load IO_SEG internally and push/pop ES).  Signature nibble is 0Dh so a
; client can tell a DMA card (0xD0) from a floppy card (0xB0).
; ---------------------------------------------------------------------------
IO_SEG		equ 0E800h	; CRTC segment (unchanged from the floppy client)
CH_SEG		equ 0EF30h	; DMA-card control-channel segment (phys 0xEF300)
RESP		equ 060h	; read: response byte ; write: ack/advance
STATUS		equ 050h	; read: sig|flags     ; write: soft reset
CMD		equ 040h	; read: complement echo ; write: input byte

ST_SIG		equ 0D0h	; high-nibble signature (DMA card)
ST_OVER		equ 008h	; OVERFLOW
ST_READY	equ 002h	; RESP_READY
ST_PHASE	equ 001h	; RESP_PHASE

; ---------------------------------------------------------------------------
; Screen: VRAM + CRTC (after igc src/screen.c; sizes per MAME victor9k.cpp:
; 4KB video_ram at F0000, mirrored once at F1000)
; ---------------------------------------------------------------------------
VRAM_SEG	equ 0F000h
CHAR_BASE	equ 064h	; glyph = char + CHAR_BASE
COLS		equ 80
ROWS		equ 25
VRAM_WORDS	equ 2048	; physical cells (wrap mask 7FFh)

A_REV		equ 080h	; cell attr bits (cell high byte)
A_UL		equ 020h

CRTC_SEL	equ 0		; CRTC register select (IO_SEG:0)
CRTC_DATA	equ 1		; CRTC register data   (IO_SEG:1)
R_START_H	equ 12
R_START_L	equ 13
R_CURS_H	equ 14
R_CURS_L	equ 15

BLANK_CELL	equ 020h+CHAR_BASE	; ' ' glyph, attr 0
CURS_PARK	equ 2000	; cursor address just past the visible window

%ifdef ROM_BUILD
ROM_SP		equ 02400h	; stack top: 01B8:2400 = phys 0x3F80, inside
%endif				;   the ROM-verified first 16KB, above .bss

; ---------------------------------------------------------------------------
; Keyboard: modifier key numbers (victor9k_kb/src/v9k_mappings.h) + nav keys
; ---------------------------------------------------------------------------
V_LSHIFT	equ 04Ah
V_RSHIFT	equ 056h
V_ALT		equ 05Fh	; the Victor's Ctrl
V_CAPS		equ 036h
V_UP		equ 058h
V_DOWN		equ 059h
V_LEFT		equ 062h
V_RIGHT		equ 063h
V_HOME		equ 00Bh	; CLR/HOME
V_WORDL		equ 02Fh	; Word-left  (KB_FORMAT.md pos 47) -> PgUp
V_WORDR		equ 030h	; Word-right (KB_FORMAT.md pos 48) -> PgDn
V_WORDL2	equ 043h	; alternate candidates (sources conflict on the
V_WORDR2	equ 044h	;   Word-key codes; all four slots are otherwise
				;   dead - trim once confirmed on a real keyboard)

MOD_SHIFT	equ 003h	; mods bits 0/1 = L/R shift down
MOD_ALT		equ 004h	; mods bit 2    = alt down

; VIA-4 (keyboard) registers we save/restore around the polled driver.
; keyboard.asm reaches them as E000:8040+n; same physical bytes.
KB4_ACR		equ 804Bh
KB4_PCR		equ 804Ch
KB4_IER		equ 804Eh

ESCB		equ 01Bh

OUT_NORMAL	equ 0
OUT_ESC		equ 1
OUT_CSI		equ 2

; ============================================================================
; Entry
; ============================================================================
start:
	cld
%ifdef ROM_BUILD
	; ROM-loaded: the stub far-jumped here with CS=01B8h and the ROM's
	; DS/SS.  Build the .COM-style world (CS=DS=ES=SS) ourselves.
	mov	ax, cs
	mov	ds, ax
	mov	es, ax
	cli			; (boot runs IF=0 anyway; never re-enabled)
	mov	ss, ax
	mov	sp, ROM_SP
%endif

	; --- Detection (side-effect-free beyond CMD writes) ----------------
	; Two-point complement echo: a real 6522 SR returns the value written
	; and pre-feature firmware returns FFh; neither can complement both.
	mov	ax, CH_SEG
	mov	es, ax
	mov	byte [es:CMD], 055h
	cmp	byte [es:CMD], 0AAh
	jne	fail
	mov	byte [es:CMD], 0AAh
	cmp	byte [es:CMD], 055h
	jne	fail
	; Stable 0Dh signature nibble on two consecutive reads.
	mov	al, [es:STATUS]
	and	al, 0F0h
	cmp	al, ST_SIG
	jne	fail
	mov	al, [es:STATUS]
	and	al, 0F0h
	cmp	al, ST_SIG
	jne	fail

%ifndef ROM_BUILD
	; --- Save the whole display state for the exit restore -------------
	mov	al, R_START_H
	call	crtc_read
	mov	[sv_r12], al
	mov	al, R_START_L
	call	crtc_read
	mov	[sv_r13], al
	mov	al, R_CURS_H
	call	crtc_read
	mov	[sv_r14], al
	mov	al, R_CURS_L
	call	crtc_read
	mov	[sv_r15], al

	push	ds
	mov	ax, VRAM_SEG
	mov	ds, ax
	xor	si, si
	mov	ax, cs
	mov	es, ax
	mov	di, vsave
	mov	cx, VRAM_WORDS
	rep	movsw
	pop	ds

	; --- Save VIA-4 keyboard state, then take the keyboard -------------
	mov	ax, KB_IO_SEG		; 0E000h (from keyboard.asm)
	mov	es, ax
	mov	al, [es:KB4_IER]
	mov	[sv_ier], al
	mov	al, [es:KB4_ACR]
	mov	[sv_acr], al
	mov	al, [es:KB4_PCR]
	mov	[sv_pcr], al
%endif
	call	kb_init			; masks all VIA-4 ints, arms the SR

	; --- Init state, take the screen ------------------------------------
	xor	al, al
	mov	[scr_x], al
	mov	[scr_y], al
	mov	[scr_attr], al
	mov	[cursor_hidden], al
	mov	[out_state], al
	mov	[mods], al
	mov	[caps], al
	mov	[llen], al
	mov	[quit_req], al

	mov	al, R_START_H		; linear display start (CON may have
	xor	ah, ah			;   left it hw-scrolled)
	call	crtc_write
	mov	al, R_START_L
	xor	ah, ah
	call	crtc_write
	call	scr_clear

	mov	si, banner
	call	scr_puts
	mov	al, 00Dh
	call	scr_putc
	mov	al, 00Ah
	call	scr_putc
	call	scr_sync

	; --- Open the channel ------------------------------------------------
	call	session_settle
	mov	ax, CH_SEG
	mov	es, ax
%ifdef ROM_BUILD
	mov	si, mcmd		; pre-boot: straight into the menu
.mnext:	lodsb
	or	al, al
	jz	.msent
	mov	[es:CMD], al
	jmp	.mnext
.msent:
%else
	mov	byte [es:CMD], 00Dh	; CR -> firmware emits a prompt
%endif

; ============================================================================
; Main loop: greedy response drain + a low-latency keyboard gate.
;
; Two constraints meet here (see the header note):
;  - the drain must never stall ~250ms (firmware full-FIFO patience) or the
;    channel overflows and a live menu is aborted;
;  - the keyboard walk must START within ~100us of SR_FULL or the byte is
;    unrecoverable (stop sequence already ran).
; So SR_FULL is tested between every drained byte and on every idle pass
; (each test ~3us), the on-time walk itself is brief (~200us), and KB_SPIN
; is shortened to ~8ms so even a late start can never approach the 250ms
; budget - it just drops that key.
; ============================================================================
mainloop:
	mov	ax, CH_SEG
	mov	es, ax
	mov	al, [es:STATUS]		; one read decides everything
	test	al, ST_OVER
	jz	.drain
	mov	si, ovmsg		; overflow: report + channel reset
	call	scr_puts
	mov	al, 00Dh
	call	scr_putc
	mov	al, 00Ah
	call	scr_putc
	call	scr_sync
	call	session_settle
	mov	byte [cursor_hidden], 0	; reset -> firmware is back in REPL (a
					;   live menu was aborted, no ?25h comes)
	jmp	mainloop

.drain:
	; Drain greedily so output isn't throttled to one byte per keypoll.
	; DH = latest STATUS; latch phase from the read we gated on.
	mov	dh, al
	mov	cx, 256
.dloop:
	test	dh, ST_READY
	jz	.done
	mov	dl, dh
	and	dl, ST_PHASE
	cmp	dl, [last_phase]
	je	.done
	mov	al, [es:RESP]		; grab the byte...
	mov	byte [es:RESP], 0	; ...ack at once (firmware stages the
					;    next byte while we render)
	mov	[last_phase], dl
	push	cx
	push	dx
	call	emit			; VT100 -> VRAM (trashes regs/ES)
	cmp	byte [quit_req], 0	; firmware-commanded quit (ESC[86q)?
	jne	.quitd
	mov	ax, KB_IO_SEG		; kb gate between bytes: a completed
	mov	es, ax			;   scan code must be walked promptly
	test	byte [es:KB_IFR], KB_SR_FULL
	jz	.nokb
	call	kb_step
	jc	.quitd
.nokb:
	pop	dx
	pop	cx
	mov	ax, CH_SEG
	mov	es, ax
	mov	dh, [es:STATUS]
	loop	.dloop
.done:
	cmp	cx, 256			; drained anything?
	je	.nodrain
	call	scr_sync
.nodrain:
	call	kb_step			; self-gates on SR_FULL (~us when idle)
	jc	quit			; CF=1 -> quit requested
	jmp	mainloop
.quitd:					; quit requested from inside the drain
	pop	dx
	pop	cx
	jmp	quit

; ============================================================================
; Quit: hand keyboard and screen back exactly as found (DOS build); the ROM
; build has nothing to hand back to - soft-boot re-inits everything.
; ============================================================================
quit:
%ifdef ROM_BUILD
	jmp	0FE00h:0000h		; soft boot: re-enter the ROM boot path
%else
	; Keyboard first: ACR/PCR back, then IER (7Fh = clear all enables,
	; then saved|80h = re-enable exactly the sources DOS had on).
	mov	ax, KB_IO_SEG
	mov	es, ax
	mov	al, [sv_acr]
	mov	[es:KB4_ACR], al
	mov	al, [sv_pcr]
	mov	[es:KB4_PCR], al
	mov	byte [es:KB4_IER], 07Fh
	mov	al, [sv_ier]
	or	al, 080h
	mov	[es:KB4_IER], al

	; Screen: full VRAM image, then display start + cursor address.
	mov	ax, VRAM_SEG
	mov	es, ax
	mov	si, vsave
	xor	di, di
	mov	cx, VRAM_WORDS
	rep	movsw
	mov	al, R_START_H
	mov	ah, [sv_r12]
	call	crtc_write
	mov	al, R_START_L
	mov	ah, [sv_r13]
	call	crtc_write
	mov	al, R_CURS_H
	mov	ah, [sv_r14]
	call	crtc_write
	mov	al, R_CURS_L
	mov	ah, [sv_r15]
	call	crtc_write

	int	20h			; DOS-hosted wrapper exit (only DOS use)
%endif

; ============================================================================
; Detection failure: say so on the CURRENTLY DISPLAYED top row (base is
; readable, so we can write into the visible window without taking over).
; ============================================================================
fail:
	mov	al, R_START_H
	call	crtc_read
	mov	bh, al
	mov	al, R_START_L
	call	crtc_read
	mov	bl, al			; BX = display start (word address)
	mov	ax, VRAM_SEG
	mov	es, ax
	mov	si, nomsg
.next:
	lodsb
	or	al, al
	jz	.donef
	xor	ah, ah
	add	ax, CHAR_BASE		; glyph, attr 0
	mov	di, bx
	and	di, VRAM_WORDS-1	; mirror wrap (CON base counts mod 4096)
	shl	di, 1
	mov	[es:di], ax
	inc	bx
	jmp	.next
.donef:
%ifdef ROM_BUILD
	jmp	0FE00h:0000h		; unreachable in practice (the stub
%else					;   already detected the card)
	int	20h
%endif

; ============================================================================
; Keyboard: one poll + translate + act.  Returns CF=1 to quit.
; kb_poll_event is the ROM's proven synchronous walker; with the shortened
; KB_SPIN its worst case is ~16ms, and callers gate it on SR_FULL so the
; on-time (common) case is a ~200us walk.
; ============================================================================
kb_step:
	call	kb_poll_event		; CF=1 no event, else AL = event
	jc	.out_ok
	mov	ah, al
	and	ah, 07Fh		; AH = key number
	test	al, 080h
	jnz	.make

	; --- break: only modifier releases matter ---------------------------
	cmp	ah, V_LSHIFT
	jne	.b_rsh
	and	byte [mods], ~001h
.b_rsh:	cmp	ah, V_RSHIFT
	jne	.b_alt
	and	byte [mods], ~002h
.b_alt:	cmp	ah, V_ALT
	jne	.out_ok
	and	byte [mods], ~MOD_ALT
.out_ok:
	clc
	ret

.make:
	cmp	ah, V_LSHIFT
	jne	.m_rsh
	or	byte [mods], 001h
	jmp	.out_ok
.m_rsh:	cmp	ah, V_RSHIFT
	jne	.m_alt
	or	byte [mods], 002h
	jmp	.out_ok
.m_alt:	cmp	ah, V_ALT
	jne	.m_cap
	or	byte [mods], MOD_ALT
	jmp	.out_ok
.m_cap:	cmp	ah, V_CAPS
	jne	.m_nav
	xor	byte [caps], 1
	jmp	.out_ok

	; --- nav keys -> VT100, menu mode (cursor hidden) only --------------
.m_nav:	cmp	ah, V_UP
	jne	.n1
	mov	al, 'A'
	jmp	.navseq
.n1:	cmp	ah, V_DOWN
	jne	.n2
	mov	al, 'B'
	jmp	.navseq
.n2:	cmp	ah, V_RIGHT
	jne	.n3
	mov	al, 'C'
	jmp	.navseq
.n3:	cmp	ah, V_LEFT
	jne	.n4
	mov	al, 'D'
	jmp	.navseq
.n4:	cmp	ah, V_HOME
	jne	.n5
	mov	al, 'H'
	jmp	.navseq
.n5:	cmp	ah, V_WORDL	; Word-left = Page Up (ESC[5~)
	je	.pgu
	cmp	ah, V_WORDL2
	jne	.n6
.pgu:	mov	al, '5'
	jmp	.pgseq
.n6:	cmp	ah, V_WORDR	; Word-right = Page Down (ESC[6~)
	je	.pgd
	cmp	ah, V_WORDR2
	jne	.m_tab
.pgd:	mov	al, '6'
.pgseq:
	cmp	byte [cursor_hidden], 0
	je	.out_ok			; REPL: drop nav keys (no line editor)
	mov	bl, al
	mov	al, ESCB
	call	send_byte
	mov	al, '['
	call	send_byte
	mov	al, bl
	call	send_byte
	mov	al, '~'
	call	send_byte
	jmp	.out_ok
.navseq:
	cmp	byte [cursor_hidden], 0
	je	.out_ok			; REPL: drop nav keys (no line editor)
	mov	bl, al
	mov	al, ESCB
	call	send_byte
	mov	al, '['
	call	send_byte
	mov	al, bl
	call	send_byte
	jmp	.out_ok

	; --- everything else: layered table lookup --------------------------
.m_tab:
	mov	bl, ah
	xor	bh, bh
	cmp	bl, 068h		; beyond the tables: drop
	jae	.out_ok
	test	byte [mods], MOD_ALT
	jz	.t_sh
	mov	al, [key_alt+bx]
	jmp	.t_got
.t_sh:	test	byte [mods], MOD_SHIFT
	jz	.t_ba
	mov	al, [key_shift+bx]
	or	al, al
	jnz	.t_got
.t_ba:	mov	al, [key_base+bx]
.t_got:
	or	al, al
	jz	.out_ok			; unmapped: drop

	; caps lock: swap letter case (not in alt/control mode)
	test	byte [mods], MOD_ALT
	jnz	.t_caps_done
	cmp	byte [caps], 0
	je	.t_caps_done
	cmp	al, 'a'
	jb	.t_upchk
	cmp	al, 'z'
	ja	.t_caps_done
	sub	al, 020h
	jmp	.t_caps_done
.t_upchk:
	cmp	al, 'A'
	jb	.t_caps_done
	cmp	al, 'Z'
	ja	.t_caps_done
	add	al, 020h
.t_caps_done:

	cmp	al, 003h		; Alt+C = Ctrl+C: quit, always
	jne	.t_esc
	stc
	ret
.t_esc:
	cmp	al, ESCB		; bare ESC (Alt+']'): menu-only
	jne	.t_line
	cmp	byte [cursor_hidden], 0
	je	.out_ok
	call	send_byte
	jmp	.out_ok

	; --- local "exit" line shadow (REPL mode only) -----------------------
	; The chars were already sent+echoed; quitting on the CR leaves
	; "exit" in the firmware's line buffer, dropped by the next reset.
.t_line:
	cmp	byte [cursor_hidden], 0
	jne	.t_send			; menu mode: e/x/i/t are menu keys
	cmp	al, 00Dh
	jne	.t_notcr
	cmp	byte [llen], 4
	jne	.t_clr
	mov	bx, [line]		; "ex" ?
	or	bx, 02020h
	cmp	bx, 'ex'
	jne	.t_clr
	mov	bx, [line+2]		; "it" ?
	or	bx, 02020h
	cmp	bx, 'it'
	jne	.t_clr
	stc				; typed line == "exit": quit
	ret
.t_clr:
	mov	byte [llen], 0
	jmp	.t_send
.t_notcr:
	cmp	al, 008h		; BS/DEL: retract shadow
	je	.t_bs
	cmp	al, 07Fh
	jne	.t_acc
.t_bs:	cmp	byte [llen], 0
	je	.t_send
	dec	byte [llen]
	jmp	.t_send
.t_acc:	cmp	byte [llen], 5		; >5 chars: stays "not exit"
	jae	.t_send
	mov	bl, [llen]
	xor	bh, bh
	mov	[line+bx], al
	inc	byte [llen]

.t_send:
	call	send_byte
	jmp	.out_ok

; --- send one byte (AL) to the firmware -------------------------------------
send_byte:
	push	es
	push	ax
	mov	ax, CH_SEG
	mov	es, ax
	pop	ax
	mov	[es:CMD], al
	pop	es
	ret

; ============================================================================
; Session channel reset: soft-reset, wait (bounded) for the CMD echo to
; re-arm to FFh (= flush ran; flag bits alone can't signal "reset done"),
; then seed last_phase.  See viaterm.c session_settle for the full rationale.
; ============================================================================
session_settle:
	mov	ax, CH_SEG
	mov	es, ax
	mov	byte [es:STATUS], 0	; any write = soft channel reset
	mov	bx, 4			; 4 x 50000 = 200000 bounded spins
.outer:
	mov	cx, 50000
.spin:
	cmp	byte [es:CMD], 0FFh	; echo re-armed = flush ran
	je	.settled
	loop	.spin
	dec	bx
	jnz	.outer
.settled:
	mov	al, [es:STATUS]
	and	al, ST_PHASE
	mov	[last_phase], al
	ret

; ============================================================================
; Output translator: VT100 (from mtui) -> VRAM.  AL = response byte.
; State persists across calls (a sequence can split across firmware reads).
; ============================================================================
emit:
	mov	bl, [out_state]
	or	bl, bl
	jnz	.st_esc_chk
	; --- OUT_NORMAL ------------------------------------------------------
	cmp	al, ESCB
	jne	scr_putc		; ordinary byte: render (tail call)
	mov	byte [out_state], OUT_ESC
	ret
.st_esc_chk:
	cmp	bl, OUT_ESC
	jne	.st_csi
	; --- OUT_ESC ---------------------------------------------------------
	cmp	al, '['
	jne	.to_normal		; unknown ESC-x: swallow both
	mov	byte [out_state], OUT_CSI
	xor	al, al
	mov	[csi_priv], al
	mov	[csi_seen], al
	mov	[csi_ni], al
	mov	di, csi_param		; zero 8 param bytes
	mov	cx, 4
	push	es
	mov	ax, cs
	mov	es, ax
	xor	ax, ax
	rep	stosw
	pop	es
	ret
.to_normal:
	mov	byte [out_state], OUT_NORMAL
	ret
.st_csi:
	; --- OUT_CSI ---------------------------------------------------------
	cmp	al, 040h
	jb	.not_final
	cmp	al, 07Eh
	ja	.not_final
	call	csi_dispatch		; final byte
	mov	byte [out_state], OUT_NORMAL
	ret
.not_final:
	cmp	al, '0'
	jb	.chk_semi
	cmp	al, '9'
	ja	.chk_semi
	sub	al, '0'			; digit: param[ni] = param[ni]*10 + d
	mov	bl, [csi_ni]
	xor	bh, bh
	mov	cl, al
	mov	al, [csi_param+bx]
	mov	dl, 10
	mul	dl
	add	al, cl
	mov	[csi_param+bx], al
	mov	byte [csi_seen], 1
	ret
.chk_semi:
	cmp	al, ';'
	jne	.chk_priv
	cmp	byte [csi_ni], 7
	jae	.semi_seen
	inc	byte [csi_ni]
	mov	bl, [csi_ni]
	xor	bh, bh
	mov	byte [csi_param+bx], 0
.semi_seen:
	mov	byte [csi_seen], 1
	ret
.chk_priv:
	cmp	al, '?'
	jne	.ignore
	mov	byte [csi_priv], 1
.ignore:
	ret				; other intermediates: keep collecting

; --- dispatch a completed CSI whose final byte is AL -------------------------
csi_dispatch:
	cmp	al, 'J'
	je	.c_J
	cmp	al, 'H'
	je	.c_H
	cmp	al, 'l'
	je	.c_l
	cmp	al, 'h'
	je	.c_h
	cmp	al, 'm'
	je	.c_m
	cmp	al, 'q'
	je	.c_q
	ret				; any other CSI: swallow

.c_q:					; ESC[86q (private): the firmware's
	cmp	byte [csi_param], 86	;   "Exit to boot" menu item - quit
	jne	.c_ret			;   (older clients swallow it)
	mov	byte [quit_req], 1
	ret

.c_J:					; ESC[2J -> clear + home
	cmp	byte [csi_param], 2
	jne	.c_ret
	jmp	scr_clear		; tail call
.c_H:					; home or absolute move
	cmp	byte [csi_seen], 0
	je	.c_home
	cmp	byte [csi_ni], 0
	jne	.c_move
	cmp	byte [csi_param], 0
	jne	.c_move
.c_home:
	xor	al, al
	mov	[scr_x], al
	mov	[scr_y], al
	ret
.c_move:
	mov	al, [csi_param]		; row 1..25
	or	al, al
	jnz	.c_r1
	mov	al, 1
.c_r1:	cmp	al, ROWS
	jbe	.c_r2
	mov	al, ROWS
.c_r2:	dec	al
	mov	[scr_y], al
	mov	al, [csi_param+1]	; col 1..80
	or	al, al
	jnz	.c_c1
	mov	al, 1
.c_c1:	cmp	al, COLS
	jbe	.c_c2
	mov	al, COLS
.c_c2:	dec	al
	mov	[scr_x], al
.c_ret:	ret
.c_l:					; ESC[?25l -> cursor hide (park)
	cmp	byte [csi_priv], 0
	je	.c_ret
	cmp	byte [csi_param], 25
	jne	.c_ret
	mov	byte [cursor_hidden], 1
	mov	al, R_CURS_H
	mov	ah, CURS_PARK >> 8
	call	crtc_write
	mov	al, R_CURS_L
	mov	ah, CURS_PARK & 0FFh
	call	crtc_write
	ret
.c_h:					; ESC[?25h -> cursor show
	cmp	byte [csi_priv], 0
	je	.c_ret
	cmp	byte [csi_param], 25
	jne	.c_ret
	mov	byte [cursor_hidden], 0
	jmp	scr_sync		; tail call
.c_m:					; SGR -> reverse/underline (mtui always
	xor	dl, dl			;   resets-and-rebuilds; colors ignored)
	mov	bl, [csi_ni]
	xor	bh, bh
.c_mlp:
	mov	al, [csi_param+bx]
	cmp	al, 7
	jne	.c_m4
	or	dl, A_REV
.c_m4:	cmp	al, 4
	jne	.c_mnx
	or	dl, A_UL
.c_mnx:
	or	bx, bx
	jz	.c_mdn
	dec	bx
	jmp	.c_mlp
.c_mdn:
	mov	[scr_attr], dl
	ret

; ============================================================================
; Screen layer
; ============================================================================

; --- render one byte at the cursor.  Column 80 clamps rather than wraps
; --- (mtui repositions every run; a wrap after the bottom-right menu cell
; --- would scroll the screen).
scr_putc:
	cmp	al, 00Dh
	jne	.p_lf
	mov	byte [scr_x], 0
	ret
.p_lf:	cmp	al, 00Ah
	jne	.p_bs
	cmp	byte [scr_y], ROWS-1
	jae	scr_scroll		; tail call
	inc	byte [scr_y]
	ret
.p_bs:	cmp	al, 008h
	jne	.p_ctl
	cmp	byte [scr_x], 0
	je	.p_ret
	dec	byte [scr_x]
.p_ret:	ret
.p_ctl:	cmp	al, 020h
	jb	.p_ret			; BEL etc: swallow
	xor	ah, ah
	add	ax, CHAR_BASE
	or	ah, [scr_attr]		; AX = cell
	mov	dx, ax
	mov	al, [scr_y]
	mov	cl, COLS
	mul	cl			; AX = y*80
	mov	cl, [scr_x]
	xor	ch, ch
	add	ax, cx
	shl	ax, 1
	mov	di, ax
	push	es
	mov	ax, VRAM_SEG
	mov	es, ax
	mov	[es:di], dx
	pop	es
	cmp	byte [scr_x], COLS-1
	jae	.p_ret
	inc	byte [scr_x]
	ret

; --- write ASCIZ at DS:SI through scr_putc ----------------------------------
scr_puts:
	lodsb
	or	al, al
	jz	.s_done
	push	si
	call	scr_putc
	pop	si
	jmp	scr_puts
.s_done:
	ret

; --- blank the visible page, home the software cursor -----------------------
scr_clear:
	push	es
	mov	ax, VRAM_SEG
	mov	es, ax
	xor	di, di
	mov	ax, BLANK_CELL
	mov	cx, ROWS*COLS
	rep	stosw
	pop	es
	xor	al, al
	mov	[scr_x], al
	mov	[scr_y], al
	ret

; --- scroll the page one row (LF on the bottom row) --------------------------
scr_scroll:
	push	ds
	push	es
	mov	ax, VRAM_SEG
	mov	ds, ax
	mov	es, ax
	mov	si, COLS*2
	xor	di, di
	mov	cx, (ROWS-1)*COLS
	rep	movsw
	mov	ax, BLANK_CELL
	mov	cx, COLS
	rep	stosw
	pop	es
	pop	ds
	ret

; --- move the HW cursor to the software cursor (skip while parked/hidden) ---
scr_sync:
	cmp	byte [cursor_hidden], 0
	jne	.y_ret
	mov	al, [scr_y]
	mov	cl, COLS
	mul	cl
	mov	cl, [scr_x]
	xor	ch, ch
	add	ax, cx			; AX = cursor word address
	mov	dx, ax
	mov	al, R_CURS_H
	mov	ah, dh
	call	crtc_write
	mov	al, R_CURS_L
	mov	ah, dl
	call	crtc_write
.y_ret:	ret

; ============================================================================
; CRTC access (settle delay between register accesses, as igc)
; ============================================================================
crtc_delay:
	push	cx
	mov	cx, 50
.d:	loop	.d
	pop	cx
	ret

; --- write CRTC register AL := AH -------------------------------------------
crtc_write:
	push	es
	push	ax
	mov	ax, IO_SEG
	mov	es, ax
	pop	ax
	mov	[es:CRTC_SEL], al
	call	crtc_delay
	mov	[es:CRTC_DATA], ah
	pop	es
	ret

; --- read CRTC register AL -> AL (HW-verified readable: R12-R15) ------------
crtc_read:
	push	es
	push	ax
	mov	ax, IO_SEG
	mov	es, ax
	pop	ax
	mov	[es:CRTC_SEL], al
	call	crtc_delay
	mov	al, [es:CRTC_DATA]
	pop	es
	ret

; ============================================================================
; The ROM's polled keyboard driver (kb_init / kb_poll_event / kb_ctable).
; Included verbatim - this program IS the DOS-hosted test vehicle for it.
; KB_SPIN shortened: an edge-wait must never stall the response drain toward
; the firmware's ~250ms overflow patience.  0400h ~= 8ms per wait on the
; 5 MHz 8088 - generous vs the real handshake (~200us), tiny vs the budget.
; ============================================================================
%define KB_SPIN 0400h
%include "keyboard.asm"

; ============================================================================
; Data
; ============================================================================
banner	db	'v9k DMA terminal (asm) - type ', 027h, 'exit', 027h
	db	' or Alt+C to quit.', 0
ovmsg	db	'[overflow]', 0
nomsg	db	'No DMA card detected.', 0
%ifdef ROM_BUILD
mcmd	db	'menu', 00Dh, 0
%endif

; --- key number -> ASCII, three layers (generated by inverting the
; --- HW-verified ASCII_TO_VICTOR table in victor9k_kb/src/v9k_mappings.c;
; --- alt layer = the Victor's control mode, cross-checked against the DOS
; --- KEYS.ASM alternate segment).  0 = key drops.
key_base:
	db	000h, 000h, 000h, 000h, 000h, 000h, 000h, 000h	; keys 00-07
	db	000h, 000h, 000h, 000h, 000h, 031h, 032h, 033h	; keys 08-0F
	db	034h, 035h, 036h, 037h, 038h, 039h, 030h, 02Dh	; keys 10-17
	db	03Dh, 008h, 000h, 07Fh, 000h, 000h, 02Fh, 02Ah	; keys 18-1F
	db	000h, 009h, 071h, 077h, 065h, 072h, 074h, 079h	; keys 20-27
	db	075h, 069h, 06Fh, 070h, 000h, 05Dh, 000h, 000h	; keys 28-2F
	db	000h, 037h, 038h, 039h, 02Dh, 000h, 000h, 061h	; keys 30-37
	db	073h, 064h, 066h, 067h, 068h, 06Ah, 06Bh, 06Ch	; keys 38-3F
	db	03Bh, 027h, 000h, 000h, 000h, 034h, 035h, 036h	; keys 40-47
	db	02Bh, 000h, 000h, 000h, 07Ah, 078h, 063h, 076h	; keys 48-4F
	db	062h, 06Eh, 06Dh, 02Ch, 02Eh, 02Fh, 000h, 00Dh	; keys 50-57
	db	000h, 000h, 031h, 032h, 033h, 00Dh, 000h, 000h	; keys 58-5F
	db	020h, 000h, 000h, 000h, 030h, 000h, 02Eh, 000h	; keys 60-67
key_shift:
	db	000h, 000h, 000h, 000h, 000h, 000h, 000h, 000h	; keys 00-07
	db	000h, 000h, 000h, 000h, 000h, 021h, 040h, 023h	; keys 08-0F
	db	024h, 025h, 000h, 026h, 02Ah, 028h, 029h, 05Fh	; keys 10-17
	db	02Bh, 000h, 000h, 000h, 000h, 000h, 000h, 000h	; keys 18-1F
	db	000h, 000h, 051h, 057h, 045h, 052h, 054h, 059h	; keys 20-27
	db	055h, 049h, 04Fh, 050h, 000h, 05Bh, 000h, 000h	; keys 28-2F
	db	000h, 000h, 000h, 000h, 000h, 000h, 000h, 041h	; keys 30-37
	db	053h, 044h, 046h, 047h, 048h, 04Ah, 04Bh, 04Ch	; keys 38-3F
	db	03Ah, 022h, 000h, 000h, 000h, 000h, 000h, 000h	; keys 40-47
	db	000h, 000h, 000h, 000h, 05Ah, 058h, 043h, 056h	; keys 48-4F
	db	042h, 04Eh, 04Dh, 000h, 000h, 03Fh, 000h, 000h	; keys 50-57
	db	000h, 000h, 000h, 000h, 000h, 000h, 000h, 000h	; keys 58-5F
	db	000h, 000h, 000h, 000h, 000h, 000h, 000h, 000h	; keys 60-67
key_alt:
	db	000h, 000h, 000h, 000h, 000h, 000h, 000h, 000h	; keys 00-07
	db	000h, 000h, 000h, 000h, 000h, 07Ch, 03Ch, 03Eh	; keys 08-0F
	db	000h, 000h, 000h, 05Eh, 060h, 07Bh, 07Dh, 07Eh	; keys 10-17
	db	05Ch, 000h, 000h, 000h, 000h, 000h, 000h, 000h	; keys 18-1F
	db	000h, 000h, 011h, 017h, 005h, 012h, 014h, 019h	; keys 20-27
	db	015h, 009h, 00Fh, 010h, 000h, 01Bh, 000h, 000h	; keys 28-2F
	db	000h, 000h, 000h, 000h, 000h, 000h, 000h, 001h	; keys 30-37
	db	013h, 004h, 006h, 007h, 008h, 00Ah, 00Bh, 00Ch	; keys 38-3F
	db	000h, 000h, 000h, 000h, 000h, 000h, 000h, 000h	; keys 40-47
	db	000h, 000h, 000h, 000h, 01Ah, 018h, 003h, 016h	; keys 48-4F
	db	002h, 00Eh, 00Dh, 000h, 000h, 000h, 000h, 000h	; keys 50-57
	db	000h, 000h, 000h, 000h, 000h, 000h, 000h, 000h	; keys 58-5F
	db	000h, 000h, 000h, 000h, 000h, 000h, 000h, 000h	; keys 60-67

; ============================================================================
; Uninitialized state (.bss: past the COM image, not in the file; the mutable
; ones are init'd at entry)
; ============================================================================
section .bss
%ifndef ROM_BUILD
sv_r12		resb 1		; saved CRTC display start / cursor address
sv_r13		resb 1
sv_r14		resb 1
sv_r15		resb 1
sv_ier		resb 1		; saved VIA-4 keyboard state
sv_acr		resb 1
sv_pcr		resb 1
%endif

last_phase	resb 1
cursor_hidden	resb 1		; mtui menu mode flag (gates nav keys, "exit")
scr_x		resb 1
scr_y		resb 1
scr_attr	resb 1

out_state	resb 1		; VT100 parser
csi_priv	resb 1
csi_seen	resb 1
csi_ni		resb 1
csi_param	resb 8

mods		resb 1		; live modifiers (bits 0/1 shift, 2 alt)
caps		resb 1
llen		resb 1		; local "exit" line shadow
line		resb 5
quit_req	resb 1		; set by ESC[86q (firmware "Exit to boot")

%ifndef ROM_BUILD
vsave		resb VRAM_WORDS*2	; entry snapshot of the full 4KB VRAM
%endif

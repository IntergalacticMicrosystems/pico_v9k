#!/usr/bin/env bash
# Build the DMA-card DOS management terminal DMATERM.EXE with OpenWatcom.
#
# dmaterm.c is a near-verbatim port of viclibc2/viaterm/viaterm.c: the VT100->
# VRAM renderer and the DOS/Z-19 keyboard path are shared byte-for-byte; only
# the control-channel registers moved (flop card 0xE800:0xAA/A9/A8 sig 0xB0 ->
# DMA card 0xEF30:0x40/0x50/0x60 sig 0xD0). The CRTC stays at 0xE800.
#
# Small-model 8086 real mode, no C runtime library (pure MMIO + a couple of
# INT 21h calls via bdos()). Same wcc/wlink invocation as viaterm's Makefile.
#
# Requirements:
#   export WATCOM=/opt/watcom
#   export PATH=$WATCOM/binl64:$PATH
#   export INCLUDE=$WATCOM/h
#
# Output: DMATERM.EXE (DOS 8.3 name for the Victor).
set -euo pipefail
cd "$(dirname "$0")"

export WATCOM=${WATCOM:-/opt/watcom}
export PATH="$WATCOM/binl64:$PATH"
export INCLUDE=${INCLUDE:-$WATCOM/h}

# -ms  small memory model   -0   8086 target      -zq  quiet
# -s   no stack checking    -ox  max optimization -w4  warning level 4
# -zp1 pack structs to 1B    (matches viaterm/Makefile CFLAGS)
wcc -ms -0 -zq -s -ox -w4 -zp1 -fo=dmaterm.obj dmaterm.c

# No library - pure C runtime + MMIO.
wlink system dos name DMATERM.EXE file dmaterm.obj

rm -f dmaterm.obj
ls -l DMATERM.EXE

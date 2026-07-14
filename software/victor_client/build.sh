#!/usr/bin/env bash
# Build the DMA-card VTASM terminal and regenerate the checked-in firmware blob
# header console/vtasm_dma_blob.h.  nasm is the only dependency (/usr/bin/nasm).
#
# Dependencies vendored into this directory (copied from the private repo so the
# pico_v9k tree builds standalone):
#   keyboard.asm  - ROM_NASM40's polled keyboard driver (%include'd verbatim)
#   NORMAL.CHR    - Victor 4KB glyph font (128-byte banner header stripped off)
#
# Outputs (all reproducible):
#   vtasm_dma.com      - DOS .COM test build (detects card, runs the terminal)
#   vtasm_dma_rom.bin  - ROM-build image (-DROM_BUILD; opens straight in menu)
#   vtasm_dma.rom      - [4096B font] ++ vtasm_dma_rom.bin (the download blob)
#   ../console/vtasm_dma_blob.h - C header the firmware serves on 0x1C
set -euo pipefail
cd "$(dirname "$0")"

NASM=${NASM:-nasm}
BLOBHDR=../console/vtasm_dma_blob.h

# DOS .COM build (channel + CRTC + keyboard live at their runtime segments).
"$NASM" -f bin -O0 -I./ viaterm_dma.asm -o vtasm_dma.com

# ROM-build image (the pre-boot feature itself).
"$NASM" -f bin -O0 -DROM_BUILD -I./ viaterm_dma.asm -o vtasm_dma_rom.bin

# Blob = 4096B glyph data (NORMAL.CHR minus its 128B banner header) ++ the
# ROM-build image.  Loaded contiguously at 0:0C80 by the boot ROM's F4 stub.
tail -c +129 NORMAL.CHR > font.tmp
cat font.tmp vtasm_dma_rom.bin > vtasm_dma.rom
rm -f font.tmp

python3 gen_blob.py vtasm_dma.rom "$BLOBHDR"
echo "wrote $BLOBHDR"

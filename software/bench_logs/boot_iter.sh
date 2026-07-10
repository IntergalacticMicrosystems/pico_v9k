#!/bin/bash
# boot_iter.sh <N> — one clean Phase B boot test iteration.
# Resets BOTH (emulator via sysresetreq, then Victor), waits for DOS,
# types 'dir', grabs screens. Diag logger must already own the UART.
set -u
N=$1
cd /root/sync/pico_v9k/software/bench_logs
R=./rctl.sh

echo "[iter $N] emulator sysresetreq"
openocd -f interface/cmsis-dap.cfg -c "adapter speed 5000" -f target/rp2350.cfg \
        -c init -c "reset run" -c shutdown >/dev/null 2>&1 || echo "[iter $N] OPENOCD RESET FAILED"
sleep 6   # SD must mount before Victor starts (BENCH: ~5s)

echo "[iter $N] victor reset"
$R reset >/dev/null 2>&1
sleep 28  # boot to prompt ~20-25s

$R grab boot${N}_prompt.png >/dev/null 2>&1
$R type 'dir{Enter}' >/dev/null 2>&1
sleep 5
$R grab boot${N}_dir.png >/dev/null 2>&1
echo "[iter $N] done"

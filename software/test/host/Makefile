# Host unit tests for the DMA-card host control channel (pico_victor/mgmt_chan.c).
# Portable C, no Pico SDK. Ported from v9k_flop's test/Makefile (test_via).
#
#   make check   - build + run both FIFO sizes
#   make clean
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O1 -g \
           -I../../pico_victor -I../../console
CHAN     = ../../pico_victor/mgmt_chan.c

# Default 1024-byte resp FIFO.
test_mgmt: test_mgmt.c $(CHAN)
	$(CC) $(CFLAGS) -pthread -o $@ test_mgmt.c $(CHAN)

# Same suite with an 8-byte resp FIFO (the tiny-FIFO no-loss case) — separate
# binary since mgmt_chan.c has file-scope statics.
test_mgmt_tiny: test_mgmt.c $(CHAN)
	$(CC) $(CFLAGS) -pthread -DMGMT_CHAN_RESP_SZ=8 -o $@ test_mgmt.c $(CHAN)

.PHONY: check clean
check: test_mgmt test_mgmt_tiny
	./test_mgmt
	./test_mgmt_tiny

clean:
	rm -f test_mgmt test_mgmt_tiny

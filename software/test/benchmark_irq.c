/*
 * benchmark_irq.c - Lightweight benchmark for current register FIFO payload decoding.
 *
 * The older benchmark compared removed dma_fast/dma_ultra_fast handlers.  This
 * target now stays aligned with the maintained cached register path by measuring
 * the small decode primitives used before ISR side effects.
 */

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/systick.h"
#include "hardware/uart.h"
#include <stdint.h>
#include <stdio.h>

#include "pico_victor/dma.h"

#define BENCH_UART_ID uart0
#define BENCH_BAUD_RATE 115200
#define BENCH_UART_TX_PIN 46

typedef enum {
    BENCH_PREFETCH,
    BENCH_READ_COMMIT,
    BENCH_WRITE,
} bench_op_t;

typedef struct {
    uint32_t address;
    uint8_t data;
    bench_op_t op;
    const char *description;
} bench_case_t;

static const bench_case_t test_cases[] = {
    {0xEF300, 0x15, BENCH_WRITE,       "write control"},
    {0xEF310, 0x55, BENCH_WRITE,       "write data"},
    {0xEF320, 0x00, BENCH_PREFETCH,    "prefetch status"},
    {0xEF330, 0x00, BENCH_READ_COMMIT, "commit status alias"},
    {0xEF380, 0x12, BENCH_WRITE,       "write addr low"},
    {0xEF3A0, 0x34, BENCH_WRITE,       "write addr mid"},
    {0xEF3C0, 0x0F, BENCH_WRITE,       "write addr high"},
    {0xEF380, 0x00, BENCH_PREFETCH,    "prefetch addr low"},
    {0xEF3A0, 0x00, BENCH_READ_COMMIT, "commit addr mid"},
};

static volatile uint32_t bench_sink;

static inline uint32_t board_fifo_encode_prefetch(uint32_t address) {
    return ((address & 0xFFFFFu) << 10) | ((uint32_t)FIFO_REG_PREFETCH << 30);
}

static uint32_t encode_case(const bench_case_t *test) {
    switch (test->op) {
        case BENCH_PREFETCH:
            return board_fifo_encode_prefetch(test->address);
        case BENCH_READ_COMMIT:
            return board_fifo_encode_read(test->address);
        case BENCH_WRITE:
            return dma_fifo_encode_write(test->address, test->data);
        default:
            return 0;
    }
}

static inline uint32_t decode_current_fifo_payload(uint32_t raw_value) {
    uint32_t payload_type = fifo_payload_type(raw_value);
    uint32_t address;
    uint32_t data = 0;

    if (payload_type == FIFO_REG_WRITE) {
        address = dma_fifo_write_address(raw_value);
        data = dma_fifo_write_data(raw_value);
    } else {
        address = board_fifo_read_address(raw_value);
    }

    uint32_t offset = dma_mask_offset(address - DMA_REGISTER_BASE) & 0xFFu;
    return (payload_type << 24) | (offset << 8) | data;
}

static void initialize_uart(void) {
    gpio_init(BENCH_UART_TX_PIN);
    gpio_set_function(BENCH_UART_TX_PIN, GPIO_FUNC_UART);
    uart_init(BENCH_UART_ID, BENCH_BAUD_RATE);
    uart_set_fifo_enabled(BENCH_UART_ID, false);
    stdio_uart_init_full(BENCH_UART_ID, BENCH_BAUD_RATE, BENCH_UART_TX_PIN, -1);
}

static void benchmark_case(const bench_case_t *test) {
    const uint32_t raw_value = encode_case(test);
    const int runs = 10000;
    uint32_t min_cycles = 0xFFFFFFFFu;
    uint32_t max_cycles = 0;
    uint64_t total_cycles = 0;

    for (int i = 0; i < 32; i++) {
        bench_sink ^= decode_current_fifo_payload(raw_value);
    }

    for (int i = 0; i < runs; i++) {
        uint32_t start = systick_hw->cvr;
        bench_sink ^= decode_current_fifo_payload(raw_value);
        uint32_t end = systick_hw->cvr;
        uint32_t cycles = (start >= end) ? (start - end) : (start + (0x00FFFFFFu - end));

        if (cycles < min_cycles) min_cycles = cycles;
        if (cycles > max_cycles) max_cycles = cycles;
        total_cycles += cycles;
    }

    uint32_t avg_cycles = (uint32_t)(total_cycles / runs);
    printf("%-20s raw=0x%08lX min=%3lu cyc avg=%3lu cyc max=%3lu cyc\n",
           test->description,
           (unsigned long)raw_value,
           (unsigned long)min_cycles,
           (unsigned long)avg_cycles,
           (unsigned long)max_cycles);
}

int main(void) {
    set_sys_clock_khz(200000, true);
    initialize_uart();
    sleep_ms(1000);

    systick_hw->csr = 0x5;
    systick_hw->rvr = 0x00FFFFFF;

    printf("\n=== Current Register FIFO Decode Benchmark ===\n");
    printf("System clock: 200MHz, 5ns per cycle\n\n");

    for (size_t i = 0; i < sizeof(test_cases) / sizeof(test_cases[0]); i++) {
        benchmark_case(&test_cases[i]);
    }

    printf("\nbench_sink=0x%08lX\n", (unsigned long)bench_sink);
    return 0;
}

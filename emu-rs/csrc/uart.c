/*
 * UART HAL shim for the Rust emulator.
 *
 * Keeps the firmware's Bafang RX byte-assembly state machine (caller declares
 * the expected reply length via uart_prime_rx), but delegates the actual byte
 * I/O to Rust, which owns the pty/serial port.
 *
 * Released under the GPL License, Version 3.
 */
#include <stdint.h>
#include <stddef.h>
#include "uart.h"

/* Implemented in Rust (src/serial.rs). */
extern void emu_serial_write(const uint8_t *buf, int len);
extern int  emu_serial_read_byte(uint8_t *out); /* 1 if a byte was read, else 0 */

static uint8_t  ui8_rx[UART_NUMBER_DATA_BYTES_TO_RECEIVE];
static uint8_t  ui8_rx_cnt = 0;
static uint8_t  ui8_tx_buffer[UART_NUMBER_DATA_BYTES_TO_SEND];
static uint8_t  ui8_expected_rx_len = 0;
volatile uint8_t ui8_received_package_flag = 0;

void uart_init(void) {}   /* Rust opens the port */

uint8_t *uart_get_tx_buffer(void) { return ui8_tx_buffer; }

void uart_prime_rx(uint8_t expected_len) {
    ui8_expected_rx_len = expected_len;
    ui8_rx_cnt = 0;
    ui8_received_package_flag = 0;
}

void uart_send_tx_buffer(uint8_t *tx_buffer, uint8_t ui8_len) {
    emu_serial_write(tx_buffer, ui8_len);
}

/* Bafang framing: no start byte, no length, no CRC — the caller primed the
 * expected length. Drain all bytes Rust has; return the buffer once complete. */
const uint8_t *uart_get_rx_buffer_rdy(void) {
    uint8_t c;
    while (emu_serial_read_byte(&c)) {
        if (ui8_expected_rx_len == 0 || ui8_received_package_flag)
            continue;                        /* nothing primed; drop the byte */
        if (ui8_rx_cnt < ui8_expected_rx_len) {
            ui8_rx[ui8_rx_cnt++] = c;
            if (ui8_rx_cnt == ui8_expected_rx_len) {
                ui8_received_package_flag = 1;
                return ui8_rx;
            }
        }
    }
    return NULL;
}

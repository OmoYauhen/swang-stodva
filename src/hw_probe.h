/*
 * Hardware probe (HW_PROBE=1 builds only) — diagnostic pages that answer
 * open hardware questions for the next firmware. See docs/hw-probe.md.
 *
 * Released under the GPL License, Version 3
 */
#pragma once

#ifdef HW_PROBE

#include <stdint.h>
#include <stdbool.h>
#include "ble.h"

// main.c: very first thing in main() — latches GPREGRET, starts TIMER1.
void hw_probe_early_init(void);

// main.c: around each ui_update() call.
void hw_probe_frame_begin(void);
void hw_probe_frame_end(uint32_t ticks_missed);

// lcd.c: around the blocking framebuffer flush.
uint32_t hw_probe_now_us(void);
void hw_probe_lcd_flush_done(uint32_t t0_us);

// ble_services.c: replaces softdevice_enable() so the SoftDevice's minimum
// app RAM base can be recorded.
uint32_t hw_probe_softdevice_enable(ble_enable_params_t *params);

// state.c: every motor reply that completed (before parsing).
void hw_probe_motor_rx(uint8_t slot, uint8_t opcode, const uint8_t *rx, uint8_t len);

// state.c: raw frame queued by a probe page, sent ahead of the next READ.
bool bafang_probe_queue_raw(const uint8_t *bytes, uint8_t len);

// lcd.c: raw SH1107 command bytes (display-orientation test).
void lcd_send_cmds(const uint8_t *cmds, uint32_t n);

#endif

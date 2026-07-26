/*
 * Desktop HAL shim for the Rust/ratatui emulator.
 *
 * Provides the environment symbols the SW102 firmware links against: the OLED
 * framebuffer, the button globals, the tick/time base, and a handful of stubs.
 * The Rust side drives the tick loop, reads `framebuffer`, sets the buttons,
 * and services the UART; this file is the thin C glue in between.
 *
 * Released under the GPL License, Version 3.
 */
#include <stdint.h>
#include <stdbool.h>

#include "lcd.h"
#include "button.h"
#include "state.h"
#include "rtc.h"      /* ui32_seconds_since_startup (defined in rtc.c) */

/* ---- OLED framebuffer -----------------------------------------------------
 * 128*64/8 = 1024 bytes. The Rust renderer reads this directly each frame. */
union framebuffer_t framebuffer;

void lcd_init(void) {}
void lcd_refresh(void) {}                       /* Rust renders every frame */
void lcd_set_backlight_intensity(uint8_t pct) { (void)pct; }

/* Accessors for Rust (avoid extern-static FFI). */
const uint8_t *emu_framebuffer(void) { return framebuffer.u8; }

/* ---- Buttons (PollButton lives in src/emu/button.c) ---------------------- */
Button buttonM, buttonDWN, buttonUP, buttonPWR;

/* Setter for Rust so it doesn't depend on the Button struct layout.
 * idx: 0=UP 1=DOWN 2=M/ENTER 3=PWR/ESC */
void emu_set_button(int idx, bool pressed) {
    switch (idx) {
        case 0: buttonUP.is_pressed  = pressed; break;
        case 1: buttonDWN.is_pressed = pressed; break;
        case 2: buttonM.is_pressed   = pressed; break;
        case 3: buttonPWR.is_pressed = pressed; break;
        default: break;
    }
}

/* ---- Time base ------------------------------------------------------------ */
#define MSEC_PER_TICK 20
volatile uint32_t gui_ticks;

uint32_t get_time_base_counter_1ms(void) { return gui_ticks * MSEC_PER_TICK; }
uint32_t get_seconds(void)               { return ui32_seconds_since_startup; }

/* One 20 ms firmware tick: advance the clock and run rt_processing() every
 * 100 ms. Rust calls this every 20 ms, then calls ui_update(). */
void rt_processing(void);
void emu_gui_tick(void) {
    gui_ticks++;
    if (gui_ticks % (1000 / MSEC_PER_TICK) == 0)
        ui32_seconds_since_startup++;
    if (gui_ticks % (100 / MSEC_PER_TICK) == 0)
        rt_processing();
}

/* ---- Power off ------------------------------------------------------------ */
volatile int emu_quit_flag = 0;
void lcd_power_off(uint8_t updateDistanceOdo) {
    (void)updateDistanceOdo;
    emu_quit_flag = 1;   /* Rust polls this and tears down the terminal */
}
int emu_should_quit(void) { return emu_quit_flag; }

/* ---- Misc stubs the firmware links against -------------------------------- */
void SW102_rt_processing_stop(void)  {}
void SW102_rt_processing_start(void) {}
void init_softdevice(void)           {}
void rt_graph_process(void)          {}
void ui_motor_stabilized(void)       {}
void set_conversions(void)           {}
uint8_t g_showNextScreenIndex, g_showNextScreenPreviousIndex;

/*
 * Bafang SW102 firmware
 *
 * Copyright (C) Casainho, 2018.
 *
 * Released under the GPL License, Version 3
 */

#include <math.h>
#include <stdbool.h>
#include "stdio.h"
#include "main.h"
#include "utils.h"
#include "uart.h"
#include "eeprom.h"
#include "buttons.h"
#include "state.h"
#include "lcd.h"
#include <stdlib.h>

uint8_t ui8_g_battery_soc;
volatile uint8_t ui8_g_motorVariablesStabilized = 0;

// Bafang display is master and the motor never speaks first, so no boot
// handshake is needed — we drop straight into the normal protocol loop.

// ---- Bafang UART protocol (source: bbs-fw/src/firmware/extcom.c) -----------
// The display is master. Every 100 ms tick we advance one step in a
// round-robin of READ opcodes: send the 2-byte request, prime RX for the
// opcode's fixed reply length, then next tick parse the reply and advance.

#define BAFANG_CAT_READ   0x11
#define BAFANG_CAT_WRITE  0x16

static const struct {
    uint8_t op;
    uint8_t reply_len;
} bafang_read_cycle[] = {
    { 0x08, 1 },  // STATUS
    { 0x0A, 2 },  // CURRENT      (amp_x2, chk = same byte)
    { 0x0F, 1 },  // BRAKE        (0x01 held / 0x00 released, no checksum)
    { 0x11, 2 },  // BATTERY      (percent, chk = same byte)
    { 0x20, 3 },  // SPEED        (rpm_hi, rpm_lo, chk = sum + 0x20)
    { 0x22, 3 },  // RANGE        (bbs-fw stuffs motor temperature here)
    { 0x24, 3 },  // CALORIES     (bbs-fw stuffs battery voltage x10 here)
    { 0x31, 2 },  // MOVING       (0x30 still / 0x31 moving, chk = same byte)
};
#define BAFANG_CYCLE_LEN (sizeof(bafang_read_cycle) / sizeof(bafang_read_cycle[0]))

static uint8_t bafang_cycle_pos = 0;
static uint8_t bafang_awaiting_reply = 0;
static uint16_t bafang_reply_timeout_ticks = 0;
#define BAFANG_REPLY_TIMEOUT_TICKS 5   // 5 x 100ms = 500 ms

// Parsed live state, populated from motor replies. Struct definition
// lives in src/state.h so the Technical config screen can
// render these as read-only diagnostics.
struct bafang_state_t g_bafang = { 0 };

// Nominal-voltage fallback options for the power calc, indexed by
// ui8_battery_voltage_option. Values are × 10 (i.e. 480 = 48.0 V).
const uint16_t battery_voltage_options_x10[] = { 360, 480, 520 };

static void bafang_send_read(uint8_t opcode, uint8_t reply_len) {
    uint8_t *tx = uart_get_tx_buffer();
    tx[0] = BAFANG_CAT_READ;
    tx[1] = opcode;
    uart_prime_rx(reply_len);
    uart_send_tx_buffer(tx, 2);
}

// Bafang PAS-level wire encoding is non-monotonic (per bbs-fw extcom.c):
// level_number → wire_code
static const uint8_t bafang_pas_encoding[10] = {
    0x00, // 0
    0x01, // 1
    0x0B, // 2
    0x0C, // 3
    0x0D, // 4
    0x02, // 5
    0x15, // 6
    0x16, // 7
    0x17, // 8
    0x03, // 9
};
#define BAFANG_ASSIST_PUSH  0x06   // ASSIST_PUSH (walk mode)
#define BAFANG_LIGHTS_ON    0xF1
#define BAFANG_LIGHTS_OFF   0xF0

static void bafang_send_write_pas(uint8_t wire_code) {
    // WRITE_PAS: [0x16, 0x0B, code, checksum = sum of first 3]
    uint8_t *tx = uart_get_tx_buffer();
    tx[0] = BAFANG_CAT_WRITE;
    tx[1] = 0x0B;
    tx[2] = wire_code;
    tx[3] = (uint8_t)(tx[0] + tx[1] + tx[2]);
    uart_send_tx_buffer(tx, 4);
    // WRITE_PAS has no reply.
}

static void bafang_send_write_lights(bool on) {
    // WRITE_LIGHTS: [0x16, 0x1A, 0xF1 | 0xF0]  (no checksum per bbs-fw docs)
    uint8_t *tx = uart_get_tx_buffer();
    tx[0] = BAFANG_CAT_WRITE;
    tx[1] = 0x1A;
    tx[2] = on ? BAFANG_LIGHTS_ON : BAFANG_LIGHTS_OFF;
    uart_send_tx_buffer(tx, 3);
    // No reply.
}

static void bafang_send_write_speed_limit(uint16_t kmh_x10) {
    // WRITE_SPEED_LIM: [0x16, 0x1F, hi, lo, checksum = sum of first 4].
    // On stock Bafang firmware the motor honors the limit; on bbs-fw the frame
    // is ack'd but discarded — noted in bbshd.md.
    uint8_t *tx = uart_get_tx_buffer();
    tx[0] = BAFANG_CAT_WRITE;
    tx[1] = 0x1F;
    tx[2] = (uint8_t)(kmh_x10 >> 8);
    tx[3] = (uint8_t)(kmh_x10 & 0xFF);
    tx[4] = (uint8_t)(tx[0] + tx[1] + tx[2] + tx[3]);
    uart_send_tx_buffer(tx, 5);
    // No reply.
}

// Last-sent wire values, so we only transmit on user-initiated change.
// 0xFF is a "never sent" sentinel that guarantees an initial sync.
static uint8_t bafang_last_pas_code = 0xFF;
static uint8_t bafang_last_lights   = 0xFF;

// Map the display's selectable assist level onto one of Bafang's 9 real PAS
// levels. The rider picks how many levels to cycle through (3/5/9); those get
// spread evenly across the motor's 1..9 range so the top selection always
// reaches full assist:
//   3 levels  -> real 1, 5, 9
//   5 levels  -> real 1, 3, 5, 7, 9
//   9 levels  -> real 1..9 (identity)
// Display level 0 always means off (real 0). Formula for level L in 1..N:
//   real = 1 + (L-1) * 8 / (N-1)
static uint8_t bafang_real_level(uint8_t level, uint8_t n) {
    if (level == 0)
        return 0;               // off
    if (level > n) level = n;
    if (n <= 1)
        return 9;               // degenerate config: single level -> full assist
    return (uint8_t)(1u + ((uint16_t)(level - 1u) * 8u) / (n - 1u));
}

// Compute the target Bafang PAS wire code for the current UI state.
static uint8_t bafang_desired_pas_code(void) {
    if (ui_vars.ui8_walk_assist)
        return BAFANG_ASSIST_PUSH;
    uint8_t real = bafang_real_level(ui_vars.ui8_assist_level,
                                     ui_vars.ui8_number_of_assist_levels);
    return bafang_pas_encoding[real];
}

// Try to send one pending WRITE. Returns true if a write was sent (in which
// case the caller should skip its READ for this tick to avoid overlap).
// Requires that we've received at least one reply — no point talking to a
// motor that isn't there yet.
static bool bafang_try_send_pending_write(void) {
    if (g_bafang.rx_count == 0) return false;

    uint8_t desired_pas = bafang_desired_pas_code();
    if (desired_pas != bafang_last_pas_code) {
        bafang_send_write_pas(desired_pas);
        bafang_last_pas_code = desired_pas;
        return true;
    }

    uint8_t desired_lights = ui_vars.ui8_lights ? BAFANG_LIGHTS_ON : BAFANG_LIGHTS_OFF;
    if (desired_lights != bafang_last_lights) {
        bafang_send_write_lights(ui_vars.ui8_lights);
        bafang_last_lights = desired_lights;
        return true;
    }

    return false;
}

// Convert wheel RPM to (kph × 10) using the configured wheel perimeter (mm):
//   kph = rpm * perimeter_mm * 60 / 1e6
//   kph_x10 = rpm * perimeter_mm * 6 / 10000
// For a 28" wheel (perimeter ≈ 2234 mm) at 164 rpm this yields 219 (= 21.9 kph).
static uint16_t rpm_to_kph_x10(uint16_t rpm, uint16_t perimeter_mm) {
    return (uint16_t)(((uint32_t)rpm * perimeter_mm * 6u) / 10000u);
}

static void bafang_parse_reply(uint8_t opcode, const uint8_t *rx) {
    switch (opcode) {
    case 0x08:  // STATUS: 1 byte, no checksum
        g_bafang.status = rx[0];
        // The Bafang status byte is a numeric status/error CODE — an ENUM, not a
        // bitfield (per bbs-fw app.h): 0x01 = NORMAL, 0x03 = BRAKING, and higher
        // values are motor faults (0x08 hall, 0x11 over-temp, 0x12 current-sense,
        // ...). It is NOT the TSDZ2 fault BITFIELD that screen_main's
        // draw_fault_states() decodes; feeding it through those bit tests made a
        // benign status (e.g. the braking code 0x03) render as an "initializing
        // motor" fault that blanked the main-screen chart until you rode again.
        // Until a proper Bafang error-code decoder exists, don't surface it as a
        // fault. g_bafang.status is still kept for the Technical diagnostics.
        rt_vars.ui8_error_states = 0;
        // Brake, firmware-agnostically: the STATUS code is the ONLY brake signal
        // bbs-fw puts on the wire (it never answers the dedicated 0x0F opcode —
        // its display switch DISCARDs unknown reads); stock firmware reports both.
        // Compare the exact enum value (NOT bit 0x02, which several fault codes
        // also set). Leave braking untouched for fault codes so a stock motor's
        // 0x0F reply still governs during a fault.
        if (rx[0] == 0x01) { g_bafang.braking = 0; rt_vars.ui8_braking = 0; }
        else if (rx[0] == 0x03) { g_bafang.braking = 1; rt_vars.ui8_braking = 1; }
        break;

    case 0x0A:  // CURRENT: amp_x2 + degenerate 1B checksum
        if (rx[0] != rx[1]) { g_bafang.chk_fail_count++; return; }
        g_bafang.current_amp_x2 = rx[0];
        // amp_x2 → amp_x5 conversion for the existing UI pipeline
        rt_vars.ui8_battery_current_x5 = (uint8_t)(((uint16_t)rx[0] * 5u) / 2u);
        break;

    case 0x0F:  // BRAKE: 1 byte, no checksum. 0x01 = lever held, 0x00 = released.
        // A dedicated brake flag reverse-engineered against a real (STOCK-firmware)
        // BBSHD with tools/brake_probe.py — the most direct signal. Note bbs-fw
        // does NOT implement this opcode (no reply), so on a bbs-fw-flashed motor
        // this poll just times out and braking comes from STATUS 0x03 above; the
        // cost is one ~500 ms reply-timeout per read cycle. Fine for stock motors.
        g_bafang.braking = (rx[0] != 0);
        rt_vars.ui8_braking = g_bafang.braking;
        break;

    case 0x11:  // BATTERY: percent + degenerate 1B checksum
        if (rx[0] != rx[1]) { g_bafang.chk_fail_count++; return; }
        g_bafang.battery_pct = rx[0];
        // ui8_g_battery_soc mirrors this in bafang_apply_directs().
        break;

    case 0x20:  // SPEED: rpm_hi, rpm_lo, chk = (sum + 0x20) & 0xFF
        if ((uint8_t)(rx[0] + rx[1] + 0x20) != rx[2]) { g_bafang.chk_fail_count++; return; }
        g_bafang.wheel_rpm = ((uint16_t)rx[0] << 8) | rx[1];
        rt_vars.ui16_wheel_speed_x10 =
            rpm_to_kph_x10(g_bafang.wheel_rpm, rt_vars.ui16_wheel_perimeter);
        break;

    case 0x22:  // RANGE hijack (motor temperature in °C by bbs-fw default)
        if ((uint8_t)(rx[0] + rx[1]) != rx[2]) { g_bafang.chk_fail_count++; return; }
        g_bafang.range_field = ((uint16_t)rx[0] << 8) | rx[1];
        // Motor temp fits in one byte; ignore hi byte unless someone configures
        // bbs-fw to report power in this slot instead.
        rt_vars.ui8_motor_temperature = (uint8_t)rx[1];
        break;

    case 0x24:  // CALORIES hijack: battery voltage × 10
        if ((uint8_t)(rx[0] + rx[1]) != rx[2]) { g_bafang.chk_fail_count++; return; }
        g_bafang.battery_voltage_x10 = ((uint16_t)rx[0] << 8) | rx[1];
        // The SW102 has no battery-voltage ADC on this port (it's bus-powered by
        // the motor), so ui16_adc_battery_voltage is never sampled. Back-convert
        // the motor-reported voltage into ADC units and feed it to the pipeline;
        // rt_low_pass_filter_battery_voltage_current_power() otherwise recomputes
        // ui16_battery_voltage_filtered_x10 from a zero ADC every cycle, clobbering
        // this reading and forcing battery power (the right-hand bar) to zero.
        //   adc = voltage_x10 * 1000 / ADC_BATTERY_VOLTAGE_PER_ADC_STEP_X10000
        rt_vars.ui16_adc_battery_voltage = (uint16_t)
            (((uint32_t)g_bafang.battery_voltage_x10 * 1000u)
                 / ADC_BATTERY_VOLTAGE_PER_ADC_STEP_X10000);
        rt_vars.ui16_battery_voltage_filtered_x10 = g_bafang.battery_voltage_x10;
        break;

    case 0x31:  // MOVING: 0x30/0x31 + degenerate 1B checksum
        if (rx[0] != rx[1]) { g_bafang.chk_fail_count++; return; }
        g_bafang.moving = (rx[0] == 0x31);
        break;
    }
    g_bafang.rx_count++;
}

// Post-processing hook: called at the very end of rt_processing().
// Copies direct-from-motor readings into the shared UI-facing state.
static void bafang_apply_directs(void) {
    // The motor reports battery percent directly; use it as the display SOC.
    if (g_bafang.rx_count > 0) {
        ui8_g_battery_soc = g_bafang.battery_pct;
    }
}

rt_vars_t rt_vars;
ui_vars_t ui_vars;

ui_vars_t* get_ui_vars(void) {
	return &ui_vars;
}

rt_vars_t* get_rt_vars(void) {
  return &rt_vars;
}

/// Set correct backlight brightness for current headlight state
void set_lcd_backlight() {
	lcd_set_backlight_intensity(
			ui_vars.ui8_lights ?
					ui_vars.ui8_lcd_backlight_on_brightness :
					ui_vars.ui8_lcd_backlight_off_brightness);
}

static uint16_t fake(uint16_t minv, uint16_t maxv) {
	static uint16_t seed = 1; // Just generate some slightly increasing data, scaled to fit the required range

	uint16_t numval = maxv - minv + 1;

	return (seed++ % numval) + minv;
}

/// Generate a fake value that slowly loops between min and max and then back to min.  You must provide static storage for this routine to use
static uint16_t fakeWave(uint32_t *storage, uint16_t minv, uint16_t maxv) {
	(*storage)++;

	uint16_t numval = maxv - minv + 1;

	return (*storage % numval) + minv;
}

/// Generate a fake value that randomly oscillates between min and max and then back to min.  You must provide static storage for this routine to use
static uint16_t fakeRandom(uint32_t *storage, uint16_t minv, uint16_t maxv) {
    int32_t rnd = (rand() - RAND_MAX / 2) % ((maxv - minv) / 5);
    if(*storage == 0)
	    *storage = (minv+maxv)/2;
    if(*storage == minv && rnd < 0)
	    rnd=-rnd;
    if(*storage == maxv && rnd > 0)
	    rnd=-rnd;
    (*storage) += rnd;
    if (*storage > maxv) {
        *storage = (uint32_t)maxv;
    }
    if (*storage < minv) {
        *storage = (uint32_t)minv;
    }
    return *storage;
}



void rt_low_pass_filter_battery_voltage_current_power(void) {
	static uint32_t ui32_battery_voltage_accumulated_x10000 = 0;
	static uint16_t ui16_battery_current_accumulated_x5 = 0;

	// low pass filter battery voltage
	ui32_battery_voltage_accumulated_x10000 -=
	    (ui32_battery_voltage_accumulated_x10000 >> BATTERY_VOLTAGE_FILTER_COEFFICIENT);

	ui32_battery_voltage_accumulated_x10000 +=
			((uint32_t) rt_vars.ui16_adc_battery_voltage * ADC_BATTERY_VOLTAGE_PER_ADC_STEP_X10000);

	rt_vars.ui16_battery_voltage_filtered_x10 =
			(((uint32_t) (ui32_battery_voltage_accumulated_x10000 >> BATTERY_VOLTAGE_FILTER_COEFFICIENT)) / 1000);

	// Fallback when the motor isn't reporting voltage (stock Bafang firmware —
	// bbs-fw hijacks READ_CALORIES to send voltage_x10; stock doesn't). Without
	// this the power calc reads 0 W. The setting is a nominal value; if the motor
	// starts reporting later (e.g. bbs-fw), the filter above takes over.
	if (g_bafang.battery_voltage_x10 == 0) {
		uint8_t opt = ui_vars.ui8_battery_voltage_option;
		if (opt >= BATTERY_VOLTAGE_OPTIONS_LEN) opt = BATTERY_VOLTAGE_OPTIONS_LEN - 1;
		rt_vars.ui16_battery_voltage_filtered_x10 = battery_voltage_options_x10[opt];
	}

	// low pass filter battery current
	ui16_battery_current_accumulated_x5 -= ui16_battery_current_accumulated_x5
			>> BATTERY_CURRENT_FILTER_COEFFICIENT;
	ui16_battery_current_accumulated_x5 +=
			(uint16_t) rt_vars.ui8_battery_current_x5;
	rt_vars.ui16_battery_current_filtered_x5 =
			ui16_battery_current_accumulated_x5
					>> BATTERY_CURRENT_FILTER_COEFFICIENT;

  // base battery power = I × V (no resistance-based loss term; the pack-resistance
  // config and its P = R·I² adder were removed as dead code).
  rt_vars.ui16_battery_power_filtered =
      (rt_vars.ui16_battery_current_filtered_x5 * rt_vars.ui16_battery_voltage_filtered_x10) / 50;
}

// Called at 10 Hz. Bafang reports instantaneous wheel RPM directly from the
// external wheel-speed sensor (magnet + reed on the frame), so we can integrate
// straight to distance without an intermediate tick counter:
//   revs_per_100ms   = rpm / 600            (60 rpm = 1 rev/s = 0.1 rev/100ms)
//   mm_per_100ms     = revs_per_100ms * perimeter_mm
//                    = (rpm * perimeter_mm) / 600
// We accumulate (rpm * perimeter_mm) directly and threshold at 100_000 * 600 =
// 60_000_000 (that's 100 m in "mm × 600"). Keeping the accumulator in the
// scaled unit avoids losing sub-mm at low speeds. Max per-tick term stays well
// below the threshold (e.g. 500 rpm × 2500 mm = 1.25M, ≈50 ticks headroom).
static void rt_calc_odometer(void) {
  static uint32_t mm_x600_accumulator = 0;
  mm_x600_accumulator += (uint32_t)g_bafang.wheel_rpm * rt_vars.ui16_wheel_perimeter;
  while (mm_x600_accumulator >= 60000000u) {  // 100_000 mm × 600 = 100 m of travel
    rt_vars.ui32_odometer_x10 += 1;
    mm_x600_accumulator -= 60000000u;
  }
}

// Metre-resolution session distance for wheel_perimeter calibration.
// Runs at 10 Hz so short reference rides show a live per-metre readout instead
// of waiting for the odometer's 100 m step. Same rpm×perimeter accumulator
// pattern as rt_calc_odometer but with a 1 m threshold instead of 100 m, kept
// as a sibling so odometer and session counter don't have to share bookkeeping.
static void rt_calc_session_distance(void) {
  static uint32_t mm_x600_accumulator = 0;
  mm_x600_accumulator += (uint32_t)g_bafang.wheel_rpm * rt_vars.ui16_wheel_perimeter;
  while (mm_x600_accumulator >= 600000u) {  // 1_000 mm × 600 = 1 m of travel
    rt_vars.ui32_session_distance_m += 1;
    mm_x600_accumulator -= 600000u;
  }
}

uint8_t rt_first_time_management(void) {
  static uint32_t ui32_counter = 0;
	static uint8_t ui8_motor_controller_init = 1;
	uint8_t ui8_status = 0;

  // wait 5 seconds to help motor variables data stabilize
  if (ui8_g_motorVariablesStabilized == 0)
    if (++ui32_counter > 50) {
      ui8_g_motorVariablesStabilized = 1;
    }

	// don't update LCD until we've received the first few replies from the motor
	// (Bafang: g_bafang.rx_count reaches 10 in ~1 second of successful round-robin)
	if (ui8_motor_controller_init
			&& (g_bafang.rx_count < 10)) {
		ui8_status = 1;
	}
	// this will be executed only 1 time at startup
  else if (ui8_motor_controller_init &&
      ui8_g_motorVariablesStabilized) {

    ui8_motor_controller_init = 0;

    // Push the configured speed limit down to the motor once the UART link has
    // stabilized. Protocol wants kmh × 10 (big-endian). Stock Bafang firmware
    // honors this; bbs-fw acks and discards it.
    bafang_send_write_speed_limit((uint16_t)ui_vars.ui8_street_mode_speed_limit * 10);
  }

	return ui8_status;
}

void rt_processing_stop(void) {
  SW102_rt_processing_stop();
}

void rt_processing_start(void) {
  SW102_rt_processing_start();
}

/**
 * Called from the main thread every 100ms
 *
 */
void copy_rt_to_ui_vars(void) {
	ui_vars.ui8_battery_current_x5 = rt_vars.ui8_battery_current_x5;
	ui_vars.ui8_duty_cycle = rt_vars.ui8_duty_cycle;
	ui_vars.ui8_error_states = rt_vars.ui8_error_states;
	ui_vars.ui16_wheel_speed_x10 = rt_vars.ui16_wheel_speed_x10;
	ui_vars.ui8_motor_temperature = rt_vars.ui8_motor_temperature;
	ui_vars.ui16_battery_voltage_filtered_x10 =
			rt_vars.ui16_battery_voltage_filtered_x10;
	ui_vars.ui16_battery_current_filtered_x5 =
			rt_vars.ui16_battery_current_filtered_x5;
	ui_vars.ui16_battery_power = rt_vars.ui16_battery_power_filtered;
	ui_vars.ui8_braking = rt_vars.ui8_braking;

	ui_vars.ui32_odometer_x10 = rt_vars.ui32_odometer_x10;
	ui_vars.ui32_session_distance_m = rt_vars.ui32_session_distance_m;

	rt_vars.ui8_assist_level = ui_vars.ui8_assist_level;
	rt_vars.ui8_lights = ui_vars.ui8_lights;
	rt_vars.ui8_walk_assist = ui_vars.ui8_walk_assist;
	rt_vars.ui16_wheel_perimeter = ui_vars.ui16_wheel_perimeter;
  rt_vars.ui8_street_mode_speed_limit = ui_vars.ui8_street_mode_speed_limit;
}

/// must be called from main() idle loop
void automatic_power_off_management(void) {
	static uint32_t ui16_lcd_power_off_time_counter = 0;

	if (ui_vars.ui8_lcd_power_off_time_minutes != 0) {
		// see if we should reset the automatic power off minutes counter
		if ((ui_vars.ui16_wheel_speed_x10 > 0) ||   // wheel speed > 0
				(ui_vars.ui8_battery_current_x5 > 0) || // battery current > 0
				(ui_vars.ui8_braking) ||                // braking
				buttons_get_events()) {                 // any button active
			ui16_lcd_power_off_time_counter = 0;
		} else {
			// increment the automatic power off ticks counter
			ui16_lcd_power_off_time_counter++;

			// check if we should power off the LCD
			if (ui16_lcd_power_off_time_counter
					>= (ui_vars.ui8_lcd_power_off_time_minutes * 10 * 60)) { // have we passed our timeout?
				lcd_power_off(1);
			}
		}
	} else {
		ui16_lcd_power_off_time_counter = 0;
	}
}

void communications(void) {
  // ---- Bafang round-robin: consume any pending reply, then send next request.
  if (bafang_awaiting_reply) {
    const uint8_t *rx = uart_get_rx_buffer_rdy();
    if (rx) {
      bafang_parse_reply(bafang_read_cycle[bafang_cycle_pos].op, rx);
      bafang_cycle_pos = (bafang_cycle_pos + 1) % BAFANG_CYCLE_LEN;
      bafang_awaiting_reply = 0;
      bafang_reply_timeout_ticks = 0;
    } else if (++bafang_reply_timeout_ticks >= BAFANG_REPLY_TIMEOUT_TICKS) {
      // No reply within timeout — resync to next opcode.
      g_bafang.timeout_count++;
      bafang_cycle_pos = (bafang_cycle_pos + 1) % BAFANG_CYCLE_LEN;
      bafang_awaiting_reply = 0;
      bafang_reply_timeout_ticks = 0;
    }
  }

  if (!bafang_awaiting_reply) {
    // WRITEs take priority over the next READ. If a state change is pending
    // (user just changed assist level, toggled lights, held walk assist),
    // send that first and skip this tick's READ — we'll pick up where the
    // round-robin left off on the next tick.
    if (bafang_try_send_pending_write())
      return;

    bafang_send_read(
        bafang_read_cycle[bafang_cycle_pos].op,
        bafang_read_cycle[bafang_cycle_pos].reply_len);
    bafang_awaiting_reply = 1;
  }
}

// Note: this called from ISR context every 100ms
void rt_processing(void)
{
  communications();

  /************************************************************************************************/
  // now do all the calculations that must be done every 100ms
  rt_low_pass_filter_battery_voltage_current_power();
  rt_calc_odometer();
  rt_calc_session_distance();
  /************************************************************************************************/
  rt_first_time_management();
  bafang_apply_directs();
}


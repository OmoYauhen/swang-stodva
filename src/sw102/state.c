/*
 * Bafang LCD 850C firmware
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
#include "rtc.h"
#include "uart.h"
#include "eeprom.h"
#include "buttons.h"
#include "state.h"
#include "adc.h"
#include "timer.h"
#include "lcd.h"
#include <stdlib.h>

uint8_t ui8_g_battery_soc;
volatile uint8_t ui8_g_motorVariablesStabilized = 0;

// Skip the TSDZ2 boot handshake: no ALIVE query, no firmware-version query,
// no CONFIGURATIONS exchange. The Bafang protocol has no such handshake
// (display is master, motor never speaks first), so those TSDZ2-specific
// states would otherwise deadlock the display at the boot animation forever.
// The RX/TX code in the READY state runs the normal protocol loop.
volatile motor_init_state_t g_motor_init_state = MOTOR_INIT_READY;
volatile motor_init_state_config_t g_motor_init_state_conf = MOTOR_INIT_CONFIG_SEND_CONFIG;
volatile motor_init_status_t ui8_g_motor_init_status = MOTOR_INIT_STATUS_RESET;

tsdz2_firmware_version_t g_tsdz2_firmware_version = { 0xff, 0, 0 };

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
// lives in include/state.h so the Technical config screen can
// render these as read-only diagnostics.
struct bafang_state_t g_bafang = { 0 };

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
    // BBSHD doesn't report pedal cadence over the display protocol (only
    // internal PAS pulse count is available, not RPM). Stub it at a
    // sentinel value so UI fields dependent on cadence render *something*
    // recognisable — revisit once we decide whether to synthesise it from
    // PAS state, expose it via bbs-fw's config-tool protocol, or hide the
    // cadence widgets entirely for BBSHD builds.
    rt_vars.ui8_pedal_cadence = 99;
    rt_vars.ui8_pedal_cadence_filtered = 99;
}

// The odometer, trip distance and trip average-speed integrators all key off
// ui32_wheel_speed_sensor_tick_counter — a cumulative wheel-revolution count
// the TSDZ2 motor used to report. The Bafang display protocol only reports
// instantaneous wheel RPM (opcode 0x20), so nothing advances that counter and
// distance would stay pinned at 0. Synthesise it here: at the 100 ms tick rate,
// accumulate revolutions from the current RPM (one revolution == 600 rpm-ticks,
// i.e. rpm/600 per 100 ms), keeping the sub-revolution remainder so slow speeds
// aren't lost.
static void bafang_synth_wheel_ticks(void) {
    static uint32_t rpm_accumulator;   // 600 accumulated rpm == one revolution
    rpm_accumulator += g_bafang.wheel_rpm;
    while (rpm_accumulator >= 600) {
        rpm_accumulator -= 600;
        rt_vars.ui32_wheel_speed_sensor_tick_counter++;
    }
}


void ui_motor_stabilized();
void ui_show_motor_status(motor_init_state_t state);

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
  static uint16_t ui16_motor_current_accumulated_x5 = 0;

	// low pass filter battery voltage
	ui32_battery_voltage_accumulated_x10000 -=
	    (ui32_battery_voltage_accumulated_x10000 >> BATTERY_VOLTAGE_FILTER_COEFFICIENT);

	ui32_battery_voltage_accumulated_x10000 +=
			((uint32_t) rt_vars.ui16_adc_battery_voltage * ADC_BATTERY_VOLTAGE_PER_ADC_STEP_X10000);

	rt_vars.ui16_battery_voltage_filtered_x10 =
			(((uint32_t) (ui32_battery_voltage_accumulated_x10000 >> BATTERY_VOLTAGE_FILTER_COEFFICIENT)) / 1000);

	// low pass filter battery current
	ui16_battery_current_accumulated_x5 -= ui16_battery_current_accumulated_x5
			>> BATTERY_CURRENT_FILTER_COEFFICIENT;
	ui16_battery_current_accumulated_x5 +=
			(uint16_t) rt_vars.ui8_battery_current_x5;
	rt_vars.ui16_battery_current_filtered_x5 =
			ui16_battery_current_accumulated_x5
					>> BATTERY_CURRENT_FILTER_COEFFICIENT;

  // low pass filter motor current
  ui16_motor_current_accumulated_x5 -= ui16_motor_current_accumulated_x5
      >> MOTOR_CURRENT_FILTER_COEFFICIENT;
  ui16_motor_current_accumulated_x5 +=
      (uint16_t) rt_vars.ui8_motor_current_x5;
  rt_vars.ui16_motor_current_filtered_x5 =
      ui16_motor_current_accumulated_x5
          >> MOTOR_CURRENT_FILTER_COEFFICIENT;

  // base battery power = I × V (no resistance-based loss term; the pack-resistance
  // config and its P = R·I² adder were removed as dead code).
  rt_vars.ui16_battery_power_filtered =
      (rt_vars.ui16_battery_current_filtered_x5 * rt_vars.ui16_battery_voltage_filtered_x10) / 50;
}

void rt_low_pass_filter_pedal_power(void) {
	static uint32_t ui32_pedal_power_accumulated = 0;

	// low pass filter
	ui32_pedal_power_accumulated -= ui32_pedal_power_accumulated
			>> PEDAL_POWER_FILTER_COEFFICIENT;
	ui32_pedal_power_accumulated += (uint32_t) rt_vars.ui16_pedal_power_x10
			/ 10;
	rt_vars.ui16_pedal_power_filtered =
			((uint32_t) (ui32_pedal_power_accumulated
					>> PEDAL_POWER_FILTER_COEFFICIENT));
}

static void rt_calc_odometer(void) {
  static uint8_t ui8_1s_timer_counter;
  static uint32_t ui32_remainder = 0;

	// calc at 1s rate
	if (++ui8_1s_timer_counter >= 10) {
		ui8_1s_timer_counter = 0;

		// calculate how many revolutions since last reset and convert to distance traveled
		uint32_t ui32_temp = (rt_vars.ui32_wheel_speed_sensor_tick_counter
				- rt_vars.ui32_wheel_speed_sensor_tick_counter_offset)
				* ((uint32_t) rt_vars.ui16_wheel_perimeter) + ui32_remainder;

		// if traveled distance is more than 100 meters update all distance variables and reset
		if (ui32_temp >= 100000) { // 100000 -> 100000 mm -> 0.1 km
			// update all distance variables
			// ui_vars.ui16_distance_since_power_on_x10 += 1;
			rt_vars.ui32_odometer_x10 += 1;
			ui32_remainder = ui32_temp - 100000;

			// reset the always incrementing value (up to motor controller power reset) by setting the offset to current value
			rt_vars.ui32_wheel_speed_sensor_tick_counter_offset =
					rt_vars.ui32_wheel_speed_sensor_tick_counter;
		}
	}
}

static void rt_calc_trips(void) {
  static uint8_t ui8_1s_timer_counter = 0;
  static uint8_t ui8_3s_timer_counter = 0;
  static uint32_t ui32_wheel_speed_sensor_tick_counter_offset = 0;
  static uint32_t ui32_remainder = 0;
  
  // used to determine if trip avg speed values have to be calculated :
  // - on first time this function is called ; so set by dfault to 1
  // - then every 1 meter traveled
  static uint8_t ui8_calc_avg_speed_flag = 1;

  // calculate how many revolutions since last reset ...
  uint32_t wheel_ticks = rt_vars.ui32_wheel_speed_sensor_tick_counter
      - ui32_wheel_speed_sensor_tick_counter_offset;

  // ... and convert to distance traveled
  uint32_t ui32_temp = wheel_ticks * ((uint32_t) rt_vars.ui16_wheel_perimeter) + ui32_remainder;

  // if traveled distance is more than 1 wheel turn update trip variables and reset
  if (wheel_ticks >= 1) { 
 
    ui8_calc_avg_speed_flag = 1;

    // update all trip distance variables
    rt_vars.ui32_trip_a_distance_x1000 += (ui32_temp / 1000);
    rt_vars.ui32_trip_b_distance_x1000 += (ui32_temp / 1000);
    ui32_remainder = ui32_temp % 1000;

    // update trip A max speed
    if (rt_vars.ui16_wheel_speed_x10 > rt_vars.ui16_trip_a_max_speed_x10)
      rt_vars.ui16_trip_a_max_speed_x10 = rt_vars.ui16_wheel_speed_x10;

    // update trip B max speed
    if (rt_vars.ui16_wheel_speed_x10 > rt_vars.ui16_trip_b_max_speed_x10)
      rt_vars.ui16_trip_b_max_speed_x10 = rt_vars.ui16_wheel_speed_x10;
    
    // reset the always incrementing value (up to motor controller power reset) by setting the offset to current value
    ui32_wheel_speed_sensor_tick_counter_offset =	rt_vars.ui32_wheel_speed_sensor_tick_counter;

  }

  // calculate trip A and B average speeds (every 3s)
  if (ui8_calc_avg_speed_flag == 1 && ++ui8_3s_timer_counter >= 30) {
    rt_vars.ui16_trip_a_avg_speed_x10 = rt_vars.ui32_trip_a_time ? (rt_vars.ui32_trip_a_distance_x1000 * 36) / rt_vars.ui32_trip_a_time : 0;
    rt_vars.ui16_trip_b_avg_speed_x10 = rt_vars.ui32_trip_b_time ? (rt_vars.ui32_trip_b_distance_x1000 * 36) / rt_vars.ui32_trip_b_time : 0;
    
    // reset 3s timer counter and flag
    ui8_calc_avg_speed_flag = 0;    
    ui8_3s_timer_counter = 0;
  }

  // at 1s rate : update all trip time variables if wheel is turning
  if (++ui8_1s_timer_counter >= 10) {
    if (rt_vars.ui16_wheel_speed_x10 > 0) {
      rt_vars.ui32_trip_a_time += 1;
      rt_vars.ui32_trip_b_time += 1;
    }
    ui8_1s_timer_counter = 0;
  }
}

static void rt_low_pass_filter_pedal_cadence(void) {
	static uint16_t ui16_pedal_cadence_accumulated = 0;

	// low pass filter
	ui16_pedal_cadence_accumulated -= (ui16_pedal_cadence_accumulated
			>> PEDAL_CADENCE_FILTER_COEFFICIENT);
	ui16_pedal_cadence_accumulated += (uint16_t) rt_vars.ui8_pedal_cadence;

	// consider the filtered value only for medium and high values of the unfiltered value
	if (rt_vars.ui8_pedal_cadence > 20) {
		rt_vars.ui8_pedal_cadence_filtered =
				(uint8_t) (ui16_pedal_cadence_accumulated
						>> PEDAL_CADENCE_FILTER_COEFFICIENT);
	} else {
		rt_vars.ui8_pedal_cadence_filtered = rt_vars.ui8_pedal_cadence;
	}
}

uint8_t rt_first_time_management(void) {
  static uint32_t ui32_counter = 0;
	static uint8_t ui8_motor_controller_init = 1;
	uint8_t ui8_status = 0;

  // wait 5 seconds to help motor variables data stabilize
  if (ui8_g_motorVariablesStabilized == 0 &&
      ((g_motor_init_state == MOTOR_INIT_READY) ||
      (g_motor_init_state == MOTOR_INIT_SIMULATING)))
    if (++ui32_counter > 50) {
      ui8_g_motorVariablesStabilized = 1;
      ui_motor_stabilized();
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

    if (ui_vars.ui8_offroad_feature_enabled
        && ui_vars.ui8_offroad_enabled_on_startup) {
      ui_vars.ui8_offroad_mode = 1;
    }
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
	ui_vars.ui16_adc_battery_voltage = rt_vars.ui16_adc_battery_voltage;
	ui_vars.ui8_battery_current_x5 = rt_vars.ui8_battery_current_x5;
	ui_vars.ui8_motor_current_x5 = rt_vars.ui8_motor_current_x5;
	ui_vars.ui8_throttle = rt_vars.ui8_throttle;
	ui_vars.ui16_adc_pedal_torque_sensor = rt_vars.ui16_adc_pedal_torque_sensor;
	ui_vars.ui8_pedal_weight_with_offset = rt_vars.ui8_pedal_weight_with_offset;
	ui_vars.ui8_pedal_weight = rt_vars.ui8_pedal_weight;
	ui_vars.ui8_duty_cycle = rt_vars.ui8_duty_cycle;
	ui_vars.ui8_error_states = rt_vars.ui8_error_states;
	ui_vars.ui16_wheel_speed_x10 = rt_vars.ui16_wheel_speed_x10;
	ui_vars.ui8_pedal_cadence = rt_vars.ui8_pedal_cadence;
	ui_vars.ui8_pedal_cadence_filtered = rt_vars.ui8_pedal_cadence_filtered;
	ui_vars.ui16_motor_speed_erps = rt_vars.ui16_motor_speed_erps;
	ui_vars.ui8_motor_hall_sensors = rt_vars.ui8_motor_hall_sensors;
	ui_vars.ui8_pas_pedal_right = rt_vars.ui8_pas_pedal_right;
	ui_vars.ui8_motor_temperature = rt_vars.ui8_motor_temperature;
	ui_vars.ui32_wheel_speed_sensor_tick_counter =
			rt_vars.ui32_wheel_speed_sensor_tick_counter;
	ui_vars.ui16_battery_voltage_filtered_x10 =
			rt_vars.ui16_battery_voltage_filtered_x10;
	ui_vars.ui16_battery_current_filtered_x5 =
			rt_vars.ui16_battery_current_filtered_x5;
  ui_vars.ui16_motor_current_filtered_x5 =
      rt_vars.ui16_motor_current_filtered_x5;
	ui_vars.ui16_battery_power = rt_vars.ui16_battery_power_filtered;
	ui_vars.ui16_pedal_power = rt_vars.ui16_pedal_power_filtered;
	ui_vars.ui8_braking = rt_vars.ui8_braking;
	ui_vars.ui8_foc_angle = (((uint16_t) rt_vars.ui8_foc_angle) * 14) / 10; // each units is equal to 1.4 degrees ((360 degrees / 256) = 1.4)

	ui_vars.ui32_trip_a_distance_x1000 = rt_vars.ui32_trip_a_distance_x1000;
  ui_vars.ui32_trip_a_distance_x100 = rt_vars.ui32_trip_a_distance_x1000 / 10;  
  ui_vars.ui32_trip_a_time = rt_vars.ui32_trip_a_time;
  ui_vars.ui16_trip_a_avg_speed_x10 = rt_vars.ui16_trip_a_avg_speed_x10;
  ui_vars.ui16_trip_a_max_speed_x10 = rt_vars.ui16_trip_a_max_speed_x10;

  ui_vars.ui32_trip_b_distance_x1000 = rt_vars.ui32_trip_b_distance_x1000;
  ui_vars.ui32_trip_b_distance_x100 = rt_vars.ui32_trip_b_distance_x1000 / 10;
  ui_vars.ui32_trip_b_time = rt_vars.ui32_trip_b_time;
  ui_vars.ui16_trip_b_avg_speed_x10 = rt_vars.ui16_trip_b_avg_speed_x10;
  ui_vars.ui16_trip_b_max_speed_x10 = rt_vars.ui16_trip_b_max_speed_x10;

	ui_vars.ui32_odometer_x10 = rt_vars.ui32_odometer_x10;
  ui_vars.ui16_adc_battery_current = rt_vars.ui16_adc_battery_current;

	rt_vars.ui8_assist_level = ui_vars.ui8_assist_level;
	for (uint8_t i = 0; i < ASSIST_LEVEL_NUMBER; i++) {
	  rt_vars.ui16_assist_level_factor[i] = ui_vars.ui16_assist_level_factor[i];
	}
  for (uint8_t i = 0; i < ASSIST_LEVEL_NUMBER; i++) {
    rt_vars.ui8_walk_assist_level_factor[i] = ui_vars.ui8_walk_assist_level_factor[i];
  }
	rt_vars.ui8_lights = ui_vars.ui8_lights;
	rt_vars.ui8_walk_assist = ui_vars.ui8_walk_assist;
	rt_vars.ui8_offroad_mode = ui_vars.ui8_offroad_mode;
	rt_vars.ui8_motor_current_min_adc = ui_vars.ui8_motor_current_min_adc;
	rt_vars.ui8_field_weakening = ui_vars.ui8_field_weakening;
	rt_vars.ui8_ramp_up_amps_per_second_x10 =
			ui_vars.ui8_ramp_up_amps_per_second_x10;
	rt_vars.ui8_target_max_battery_power_div25 = ui_vars.ui8_target_max_battery_power_div25;
	rt_vars.ui16_wheel_perimeter = ui_vars.ui16_wheel_perimeter;
	rt_vars.ui8_wheel_max_speed = ui_vars.wheel_max_speed_x10 / 10;
	rt_vars.ui8_motor_type = ui_vars.ui8_motor_type;
	rt_vars.ui8_motor_current_control_mode = ui_vars.ui8_motor_current_control_mode;
	rt_vars.ui8_motor_assistance_startup_without_pedal_rotation =
			ui_vars.ui8_motor_assistance_startup_without_pedal_rotation;
	rt_vars.ui8_temperature_limit_feature_enabled =
			ui_vars.ui8_temperature_limit_feature_enabled;
	rt_vars.ui8_startup_motor_power_boost_always =
			ui_vars.ui8_startup_motor_power_boost_always;
	rt_vars.ui8_startup_motor_power_boost_limit_power =
			ui_vars.ui8_startup_motor_power_boost_limit_power;
	rt_vars.ui8_startup_motor_power_boost_time =
			ui_vars.ui8_startup_motor_power_boost_time;
  for (uint8_t i = 0; i < 9; i++) {
    rt_vars.ui16_startup_motor_power_boost_factor[i] = ui_vars.ui16_startup_motor_power_boost_factor[i];
  }
	rt_vars.ui8_startup_motor_power_boost_fade_time =
			ui_vars.ui8_startup_motor_power_boost_fade_time;
	rt_vars.ui8_startup_motor_power_boost_feature_enabled =
			ui_vars.ui8_startup_motor_power_boost_feature_enabled;
	rt_vars.ui8_motor_temperature_min_value_to_limit =
			ui_vars.ui8_motor_temperature_min_value_to_limit;
	rt_vars.ui8_motor_temperature_max_value_to_limit =
			ui_vars.ui8_motor_temperature_max_value_to_limit;
	rt_vars.ui8_offroad_feature_enabled = ui_vars.ui8_offroad_feature_enabled;
	rt_vars.ui8_offroad_enabled_on_startup =
			ui_vars.ui8_offroad_enabled_on_startup;
	rt_vars.ui8_offroad_speed_limit = ui_vars.ui8_offroad_speed_limit;
	rt_vars.ui8_offroad_power_limit_enabled =
			ui_vars.ui8_offroad_power_limit_enabled;
	rt_vars.ui8_offroad_power_limit_div25 =
			ui_vars.ui8_offroad_power_limit_div25;
  rt_vars.ui8_torque_sensor_calibration_pedal_ground =
      ui_vars.ui8_torque_sensor_calibration_pedal_ground;

  rt_vars.ui8_torque_sensor_calibration_feature_enabled = ui_vars.ui8_torque_sensor_calibration_feature_enabled;
  rt_vars.ui8_torque_sensor_calibration_pedal_ground = ui_vars.ui8_torque_sensor_calibration_pedal_ground;

  rt_vars.ui8_street_mode_enabled = ui_vars.ui8_street_mode_enabled;
  rt_vars.ui8_street_mode_speed_limit = ui_vars.ui8_street_mode_speed_limit;

  rt_vars.ui8_pedal_cadence_fast_stop = ui_vars.ui8_pedal_cadence_fast_stop;
  rt_vars.ui8_coast_brake_adc = ui_vars.ui8_coast_brake_adc;
  rt_vars.ui8_adc_lights_current_offset = ui_vars.ui8_adc_lights_current_offset;
  rt_vars.ui8_throttle_virtual = ui_vars.ui8_throttle_virtual;
  rt_vars.ui8_torque_sensor_filter = ui_vars.ui8_torque_sensor_filter;
  rt_vars.ui8_torque_sensor_adc_threshold = ui_vars.ui8_torque_sensor_adc_threshold;
  rt_vars.ui8_coast_brake_enable = ui_vars.ui8_coast_brake_enable;
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

  if (!bafang_awaiting_reply && g_motor_init_state == MOTOR_INIT_READY) {
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
  bafang_synth_wheel_ticks();   // feed the distance integrators (no tick counter on the wire)
  rt_low_pass_filter_battery_voltage_current_power();
  rt_low_pass_filter_pedal_power();
  rt_low_pass_filter_pedal_cadence();
  rt_calc_odometer();
  rt_calc_trips();
  rt_graph_process();
  /************************************************************************************************/
  rt_first_time_management();
  bafang_apply_directs();
}

void prepare_torque_sensor_calibration_table(void) {
  static bool first_time = true;

  // we need to make this atomic
  rt_processing_stop();

  // at the very first time, copy the ADC values from one table to the other
  if (first_time) {
    first_time = false;

    for (uint8_t i = 0; i < 8; i++) {
      rt_vars.ui16_torque_sensor_calibration_table_left[i][0] = ui_vars.ui16_torque_sensor_calibration_table_left[i][1];
      rt_vars.ui16_torque_sensor_calibration_table_right[i][0] = ui_vars.ui16_torque_sensor_calibration_table_right[i][1];
    }
  }

  // get the delta values of ADC steps per kg
  for (uint8_t i = 1; i < 8; i++) {
    // get the deltas x100
    rt_vars.ui16_torque_sensor_calibration_table_left[i][1] =
        ((ui_vars.ui16_torque_sensor_calibration_table_left[i][0] - ui_vars.ui16_torque_sensor_calibration_table_left[i - 1][0]) * 100) /
        (ui_vars.ui16_torque_sensor_calibration_table_left[i][1] - ui_vars.ui16_torque_sensor_calibration_table_left[i - 1][1]);

    rt_vars.ui16_torque_sensor_calibration_table_right[i][1] =
        ((ui_vars.ui16_torque_sensor_calibration_table_right[i][0] - ui_vars.ui16_torque_sensor_calibration_table_right[i - 1][0]) * 100) /
        (ui_vars.ui16_torque_sensor_calibration_table_right[i][1] - ui_vars.ui16_torque_sensor_calibration_table_right[i - 1][1]);
  }
  // very first table value need to the calculated here
  rt_vars.ui16_torque_sensor_calibration_table_left[0][1] = rt_vars.ui16_torque_sensor_calibration_table_left[1][1]; // the first delta is equal the the second one
  rt_vars.ui16_torque_sensor_calibration_table_right[0][1] = rt_vars.ui16_torque_sensor_calibration_table_right[1][1]; // the first delta is equal the the second one


  rt_processing_start();
}

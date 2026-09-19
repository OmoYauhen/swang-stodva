#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct rt_vars_struct {
	uint16_t ui16_adc_battery_voltage;
	uint8_t ui8_battery_current_x5;
	uint8_t ui8_motor_current_x5;
	uint8_t ui8_duty_cycle;
	uint8_t ui8_error_states;
	uint16_t ui16_wheel_speed_x10;
	uint8_t ui8_pedal_cadence;
	uint8_t ui8_motor_temperature;
	uint32_t ui32_wheel_speed_sensor_tick_counter;
	uint16_t ui16_battery_voltage_filtered_x10;
	uint16_t ui16_battery_current_filtered_x5;
	uint16_t ui16_battery_power_filtered;
	uint8_t ui8_pedal_cadence_filtered;
	uint32_t ui32_wheel_speed_sensor_tick_counter_offset;

	uint8_t ui8_assist_level;
	uint8_t ui8_number_of_assist_levels;
	uint16_t ui16_wheel_perimeter;
	uint8_t ui8_units_type;
	uint8_t ui8_walk_assist_feature_enabled;
	uint8_t ui8_lcd_backlight_on_brightness;
	uint8_t ui8_lcd_backlight_off_brightness;
	uint32_t ui32_odometer_x10;

	uint32_t ui32_trip_a_distance_x1000;
	uint32_t ui32_trip_a_time;
	uint16_t ui16_trip_a_avg_speed_x10;
	uint16_t ui16_trip_a_max_speed_x10;

	uint32_t ui32_trip_b_distance_x1000;
	uint32_t ui32_trip_b_time;
  	uint16_t ui16_trip_b_avg_speed_x10;
  	uint16_t ui16_trip_b_max_speed_x10;

	uint8_t ui8_lights;
	uint8_t ui8_braking;
	uint8_t ui8_walk_assist;

  uint8_t ui8_street_mode_speed_limit;

} rt_vars_t;

typedef struct ui_vars_struct {
	uint8_t ui8_battery_current_x5;
	uint8_t ui8_duty_cycle;
	uint8_t ui8_error_states;
	uint16_t ui16_wheel_speed_x10;
	uint8_t ui8_pedal_cadence;
	uint8_t ui8_motor_temperature;
	uint32_t ui32_wheel_speed_sensor_tick_counter_offset;
	uint16_t ui16_battery_voltage_filtered_x10;
	uint16_t ui16_battery_current_filtered_x5;
	uint16_t ui16_battery_power;
	uint8_t ui8_pedal_cadence_filtered;

	uint8_t ui8_assist_level;
	uint8_t ui8_number_of_assist_levels;
	uint16_t ui16_wheel_perimeter;
	uint8_t ui8_units_type;
	uint8_t ui8_time_field_enable;
	uint8_t ui8_motor_power_option; // index into motor_power_options_w[]: 0=250W 1=500W 2=750W 3=1000W
	uint8_t ui8_ble_broadcast_enabled; // 0 = mute BLE telemetry notifications, 1 = broadcast
	uint8_t ui8_walk_assist_feature_enabled;
	uint8_t ui8_lcd_power_off_time_minutes;
	uint8_t ui8_lcd_backlight_on_brightness;
	uint8_t ui8_lcd_backlight_off_brightness;
	uint32_t ui32_odometer_x10;

	uint32_t ui32_trip_a_distance_x1000;
	uint32_t ui32_trip_a_distance_x100;
	uint32_t ui32_trip_a_time;
	uint16_t ui16_trip_a_avg_speed_x10;
	uint16_t ui16_trip_a_max_speed_x10;

	uint32_t ui32_trip_b_distance_x1000;
	uint32_t ui32_trip_b_distance_x100;
	uint32_t ui32_trip_b_time;
  	uint16_t ui16_trip_b_avg_speed_x10;
  	uint16_t ui16_trip_b_max_speed_x10;

	uint8_t ui8_lights;
	uint8_t ui8_braking;
	uint8_t ui8_walk_assist;

	uint8_t ui8_street_mode_speed_limit;
} ui_vars_t;

ui_vars_t* get_ui_vars(void);
rt_vars_t* get_rt_vars(void);

extern rt_vars_t rt_vars;
extern ui_vars_t ui_vars;

extern volatile uint8_t ui8_g_motorVariablesStabilized;

void rt_processing(void);
void rt_processing_stop(void);
void rt_processing_start(void);

/**
 * Called from the main thread every 100ms
 *
 */
void copy_rt_to_ui_vars(void);

/// must be called from main() idle loop
void automatic_power_off_management(void);

void lcd_power_off(uint8_t updateDistanceOdo); // provided by LCD

/// Set correct backlight brightness for current headlight state
void set_lcd_backlight();

extern uint8_t ui8_g_battery_soc;

// Live-parsed Bafang display-protocol state, populated by bafang_parse_reply()
// in state.c. Exposed here so the Technical config screen can render its
// fields as read-only diagnostics.
struct bafang_state_t {
    uint8_t  status;                // READ_STATUS (0x08)
    uint8_t  battery_pct;           // READ_BATTERY (0x11)
    uint16_t wheel_rpm;             // READ_SPEED (0x20)
    uint16_t battery_voltage_x10;   // READ_CALORIES (0x24) — bbs-fw voltage hijack
    uint16_t range_field;           // READ_RANGE (0x22) — motor temp/power hijack
    uint8_t  current_amp_x2;        // READ_CURRENT (0x0A), scaled: A * 2
    uint8_t  moving;                // READ_MOVING (0x31): 0 = still, 1 = moving
    uint8_t  braking;               // READ_BRAKE (0x0F): 0 = released, 1 = held
    uint32_t rx_count;              // successful replies received (all opcodes)
    uint32_t chk_fail_count;        // per-opcode checksum failures
    uint32_t timeout_count;         // request → reply timeouts
};
// Not marked volatile: g_bafang is only ever read and written from main
// context (bafang_parse_reply() is called from communications() which runs
// on the ui_update tick, not from the UART RX ISR — that ISR writes the
// raw byte buffer, and only main context turns those bytes into fields).
// Keeping it non-volatile lets the menu system take a plain void* pointer
// to any field via the PTRSIZE macro.
extern struct bafang_state_t g_bafang;

// Battery voltage (readed on motor controller):
#define ADC_BATTERY_VOLTAGE_PER_ADC_STEP_X10000 866

// Battery voltage (read from ADC):
// 30.0V --> 447 | 0.0671 volts per each ADC unit
// 40.0V --> 595 | 0.0672 volts per each ADC unit

// Possible values: 0, 1, 2, 3, 4, 5, 6
// 0 equal to no filtering and no delay, higher values will increase filtering but will also add bigger delay
#define BATTERY_VOLTAGE_FILTER_COEFFICIENT 3
#define BATTERY_CURRENT_FILTER_COEFFICIENT 2
#define PEDAL_POWER_FILTER_COEFFICIENT     3
#define PEDAL_CADENCE_FILTER_COEFFICIENT   3

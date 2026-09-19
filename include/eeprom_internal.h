/*
 * Bafang SW102 firmware
 *
 * Copyright (C) Casainho, 2018.
 *
 * Released under the GPL License, Version 3
 */

#ifndef _EEPROM_INTERNAL_H_
#define _EEPROM_INTERNAL_H_

#include "state.h"

// For compatible changes, just add new fields at the end of the table (they will be inited to 0xff for old eeprom images).  For incompatible
// changes bump up EEPROM_MIN_COMPAT_VERSION and the user's EEPROM settings will be discarded.
//
// 0x3E (swang-stodva): force a clean-defaults reset. Devices flashed over an
// earlier open-source SW102 firmware inherited the same 0x3D version byte but a
// different struct layout, so config fields were read misaligned — surfacing as
// e.g. number_of_assist_levels = 20 and a garbage auto-power-off timeout.
// 0x3F: added ui8_motor_power_option; battery/charge submenus removed from UI.
// 0x40: dead-code purge — removed battery/wh/resistance fields no longer surfaced
// by any UI or sent to the motor (cutoff, pack resistance, wh integrator, soc-enable,
// battery_max_current, motor_max_current, voltage_reset_wh_counter).
// 0x41: street-mode trim — removed enabled_on_startup, power_limit(_div25),
// throttle_enabled, hotkey_enabled. Kept function_enabled, enabled, speed_limit
// (speed_limit to be wired to the Bafang WRITE_SPEED_LIMIT command later).
// 0x42: menu restructure — added ble_broadcast_enabled, removed street_mode_function/enabled,
// wheel_max_speed, ramp_up_amps, temperature_limit fields.
// 0x43: TSDZ2 purge — removed torque_sensor_calibration_*, torque_sensor_filter,
// torque_sensor_adc_threshold, coast_brake_adc, coast_brake_enable fields
// (Bafang has no display-side torque calibration or coast-brake ADC concept).
// 0x44: TSDZ2 purge round 2 — removed remaining TSDZ2 controller settings that
// the display never transmits to a Bafang motor (motor_current_min_adc,
// field_weakening, target_max_battery_power_div25, motor_current_control_mode,
// motor_type, motor_assistance_startup_without_pedal_rotation,
// battery_soc_increment_decrement, buttons_up_down_invert), the startup-power-boost
// feature (feature_enabled, always, limit_power, time, fade_time,
// startup_motor_power_boost_factor[]), the TSDZ2 offroad/street-mode set
// (offroad_feature_enabled/enabled_on_startup/speed_limit/power_limit_enabled/
// power_limit_div25), 850C main-screen bookkeeping (field_selectors[],
// graphs_field_selectors[], x_axis_scale, showNextScreenIndex),
// miscellaneous TSDZ2 knobs (pedal_cadence_fast_stop, adc_lights_current_offset,
// throttle_virtual_step). Also dropped the entire assist_level_factor[] and
// walk_assist_level_factor[] arrays — Bafang delegates per-level power
// interpretation to the motor's own controller EEPROM (programmed via bbs-fw /
// Bafang Config Tool); the display only sends WRITE_PAS with a single level code.
#define EEPROM_MIN_COMPAT_VERSION 0x44
#define EEPROM_VERSION 0x44

typedef struct eeprom_data {
	uint8_t eeprom_version; // Used to detect changes in eeprom encoding, if != EEPROM_VERSION we will not use it

	uint8_t ui8_assist_level;
	uint16_t ui16_wheel_perimeter;
	uint8_t ui8_units_type;
	uint8_t ui8_time_field_enable;
	uint8_t ui8_number_of_assist_levels;
	uint8_t ui8_lcd_power_off_time_minutes;
	uint8_t ui8_lcd_backlight_on_brightness;
	uint8_t ui8_lcd_backlight_off_brightness;
	uint32_t ui32_odometer_x10;
	uint8_t ui8_walk_assist_feature_enabled;

  uint8_t ui8_street_mode_speed_limit;


  uint32_t ui32_trip_a_distance_x1000;
  uint32_t ui32_trip_a_time;
  uint16_t ui16_trip_a_max_speed_x10;

  uint32_t ui32_trip_b_distance_x1000;
  uint32_t ui32_trip_b_time;
  uint16_t ui16_trip_b_max_speed_x10;

  uint8_t ui8_motor_power_option; // 0=250W 1=500W 2=750W 3=1000W

  uint8_t ui8_ble_broadcast_enabled; // 0 = mute BLE telemetry notifications, 1 = broadcast

// FIXME align to 32 bit value by end of structure and pack other fields
} eeprom_data_t;

// *************************************************************************** //
// EEPROM memory variables default values
#define DEFAULT_VALUE_ASSIST_LEVEL                                  1
// Bafang PAS exposes a fixed choice of level counts: 3, 5 or 9.
#define DEFAULT_VALUE_NUMBER_OF_ASSIST_LEVELS                       9
#define DEFAULT_VALUE_WHEEL_PERIMETER                               2100 // 27.5'' wheel: 2100mm perimeter
#define DEFAULT_VALUE_UNITS_TYPE                                    0 // 0 = km/h
#define DEAFULT_VALUE_TIME_FIELD                                    1 // 1 i show clock
#define DEFAULT_VALUE_MOTOR_POWER_OPTION                            3  // 3 = 1000W (BBSHD stock)
#define DEFAULT_VALUE_BLE_BROADCAST_ENABLED                         1  // on by default
#define DEFAULT_VALUE_WALK_ASSIST_FEATURE_ENABLED                   1
#define DEFAULT_VALUE_LCD_POWER_OFF_TIME                            60 // 60 minutes, each unit 1 minute
#define DEFAULT_VALUE_LCD_BACKLIGHT_ON_BRIGHTNESS                   100 // 8 = 40%
#define DEFAULT_VALUE_LCD_BACKLIGHT_OFF_BRIGHTNESS                  20 // 20 = 100%
#define DEFAULT_VALUE_ODOMETER_X10                                  0
#define DEFAULT_STREET_MODE_SPEED_LIMIT                             25 // 25 km/h

#define DEFAULT_VALUE_TRIP_DISTANCE                                  0
#define DEFAULT_VALUE_TRIP_TIME                                      0
#define DEFAULT_VALUE_TRIP_MAX_SPEED                                 0

// *************************************************************************** //

// *************************************************************************** //
// BATTERY

// ADC Battery voltage
// 0.344 per ADC_8bits step: 17.9V --> ADC_8bits = 52; 40V --> ADC_8bits = 116; this signal atenuated by the opamp 358
#define ADC10BITS_BATTERY_VOLTAGE_PER_ADC_STEP_X512 44
#define ADC10BITS_BATTERY_VOLTAGE_PER_ADC_STEP_X256 (ADC10BITS_BATTERY_VOLTAGE_PER_ADC_STEP_X512 >> 1)
#define ADC8BITS_BATTERY_VOLTAGE_PER_ADC_STEP 0.344

// ADC Battery current
// 1A per 5 steps of ADC_10bits
#define ADC_BATTERY_CURRENT_PER_ADC_STEP_X512 102
// *************************************************************************** //

#endif /* _EEPROM_H_ */

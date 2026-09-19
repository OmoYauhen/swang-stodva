/*
 * Bafang SW102 firmware
 *
 * Copyright (C) Casainho, 2018.
 *
 * Released under the GPL License, Version 3
 */

#include "stdio.h"
#include <string.h>
#include "eeprom.h"
#include "eeprom_internal.h"
#include "eeprom_hw.h"
#include "main.h"

static eeprom_data_t m_eeprom_data;

// get rid of some copypasta with this little wrapper for copying arrays between structs
#define COPY_ARRAY(dest, src, field) memcpy((dest)->field, (src)->field, sizeof((dest)->field))

const eeprom_data_t m_eeprom_data_defaults = {
  .eeprom_version = EEPROM_VERSION,
  .ui8_assist_level = DEFAULT_VALUE_ASSIST_LEVEL,
  .ui16_wheel_perimeter = DEFAULT_VALUE_WHEEL_PERIMETER,
  .ui8_units_type = DEFAULT_VALUE_UNITS_TYPE,
  .ui8_time_field_enable = DEAFULT_VALUE_TIME_FIELD,
  .ui8_motor_power_option = DEFAULT_VALUE_MOTOR_POWER_OPTION,
  .ui8_ble_broadcast_enabled = DEFAULT_VALUE_BLE_BROADCAST_ENABLED,
  .ui8_number_of_assist_levels = DEFAULT_VALUE_NUMBER_OF_ASSIST_LEVELS,
  .ui8_lcd_power_off_time_minutes =
  DEFAULT_VALUE_LCD_POWER_OFF_TIME,
  .ui8_lcd_backlight_on_brightness =
  DEFAULT_VALUE_LCD_BACKLIGHT_ON_BRIGHTNESS,
  .ui8_lcd_backlight_off_brightness =
  DEFAULT_VALUE_LCD_BACKLIGHT_OFF_BRIGHTNESS,
  .ui32_odometer_x10 =
  DEFAULT_VALUE_ODOMETER_X10,
  .ui8_walk_assist_feature_enabled =
  DEFAULT_VALUE_WALK_ASSIST_FEATURE_ENABLED,

  .ui8_street_mode_speed_limit = DEFAULT_STREET_MODE_SPEED_LIMIT,
};

void eeprom_init() {
	eeprom_hw_init();

	// read the values from EEPROM to array
	memset(&m_eeprom_data, 0, sizeof(m_eeprom_data));

	// if eeprom is blank use defaults
	// if eeprom version is less than the min required version, wipe and use defaults
	// if eeprom version is greater than the current app version, user must have downgraded - wipe and use defaults
	if (!flash_read_words(&m_eeprom_data,
			sizeof(m_eeprom_data)
					/ sizeof(uint32_t))
	    || m_eeprom_data.eeprom_version < EEPROM_MIN_COMPAT_VERSION
	    || m_eeprom_data.eeprom_version > EEPROM_VERSION
	    )
		// If we are using default data it doesn't get written to flash until someone calls write
		memcpy(&m_eeprom_data, &m_eeprom_data_defaults,
				sizeof(m_eeprom_data_defaults));

//	// Perform whatever migrations we need to update old eeprom formats
//	if (m_eeprom_data.eeprom_version < EEPROM_VERSION) {
//
//		m_eeprom_data.ui8_lcd_backlight_on_brightness =
//				m_eeprom_data_defaults.ui8_lcd_backlight_on_brightness;
//		m_eeprom_data.ui8_lcd_backlight_off_brightness =
//				m_eeprom_data_defaults.ui8_lcd_backlight_off_brightness;
//
//		m_eeprom_data.eeprom_version = EEPROM_VERSION;
//	}

	eeprom_init_variables();
}

void eeprom_init_variables(void) {
	ui_vars_t *ui_vars = get_ui_vars();
	rt_vars_t *rt_vars = get_rt_vars();

	// copy data final variables
	ui_vars->ui8_assist_level = m_eeprom_data.ui8_assist_level;
	ui_vars->ui16_wheel_perimeter = m_eeprom_data.ui16_wheel_perimeter;
	ui_vars->ui8_units_type = m_eeprom_data.ui8_units_type;
  ui_vars->ui8_time_field_enable =
      m_eeprom_data.ui8_time_field_enable;
  ui_vars->ui8_motor_power_option =
      m_eeprom_data.ui8_motor_power_option;
  ui_vars->ui8_ble_broadcast_enabled =
      m_eeprom_data.ui8_ble_broadcast_enabled;
	ui_vars->ui8_number_of_assist_levels =
			m_eeprom_data.ui8_number_of_assist_levels;
	ui_vars->ui8_lcd_power_off_time_minutes =
			m_eeprom_data.ui8_lcd_power_off_time_minutes;
	ui_vars->ui8_lcd_backlight_on_brightness =
			m_eeprom_data.ui8_lcd_backlight_on_brightness;
	ui_vars->ui8_lcd_backlight_off_brightness =
			m_eeprom_data.ui8_lcd_backlight_off_brightness;
	rt_vars->ui32_odometer_x10 = m_eeprom_data.ui32_odometer_x10; // odometer value should reside on RT vars
	ui_vars->ui8_walk_assist_feature_enabled =
			m_eeprom_data.ui8_walk_assist_feature_enabled;

  ui_vars->ui8_street_mode_speed_limit =
      m_eeprom_data.ui8_street_mode_speed_limit;
}

void eeprom_write_variables(void) {
	ui_vars_t *ui_vars = get_ui_vars();
	m_eeprom_data.ui8_assist_level = ui_vars->ui8_assist_level;
	m_eeprom_data.ui16_wheel_perimeter = ui_vars->ui16_wheel_perimeter;
	m_eeprom_data.ui8_units_type = ui_vars->ui8_units_type;
  m_eeprom_data.ui8_time_field_enable =
      ui_vars->ui8_time_field_enable;
  m_eeprom_data.ui8_motor_power_option =
      ui_vars->ui8_motor_power_option;
  m_eeprom_data.ui8_ble_broadcast_enabled =
      ui_vars->ui8_ble_broadcast_enabled;
	m_eeprom_data.ui8_number_of_assist_levels =
			ui_vars->ui8_number_of_assist_levels;
	m_eeprom_data.ui8_lcd_power_off_time_minutes =
			ui_vars->ui8_lcd_power_off_time_minutes;
	m_eeprom_data.ui8_lcd_backlight_on_brightness =
			ui_vars->ui8_lcd_backlight_on_brightness;
	m_eeprom_data.ui8_lcd_backlight_off_brightness =
			ui_vars->ui8_lcd_backlight_off_brightness;
	m_eeprom_data.ui32_odometer_x10 = ui_vars->ui32_odometer_x10;
	m_eeprom_data.ui8_walk_assist_feature_enabled =
			ui_vars->ui8_walk_assist_feature_enabled;

  m_eeprom_data.ui8_street_mode_speed_limit =
      ui_vars->ui8_street_mode_speed_limit;

	flash_write_words(&m_eeprom_data, sizeof(m_eeprom_data) / sizeof(uint32_t));
}

void eeprom_init_defaults(void)
{
  memset(&m_eeprom_data, 0, sizeof(m_eeprom_data));
  memcpy(&m_eeprom_data,
      &m_eeprom_data_defaults,
      sizeof(m_eeprom_data_defaults));

  eeprom_init_variables();

  flash_write_words(&m_eeprom_data, sizeof(m_eeprom_data) / sizeof(uint32_t));
}

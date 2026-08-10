#include "screen_cfg_utils.h"
#include "state.h"
#include "eeprom.h"
#include "ui.h"

extern const struct screen screen_main;

static void do_reset_trip_a(const struct configtree_t *ign);
static void do_reset_trip_b(const struct configtree_t *ign);
static void do_reset_ble(const struct configtree_t *ign);
static void do_reset_all(const struct configtree_t *ign);
void cfg_push_assist_screen(const struct configtree_t *ign);
void cfg_push_walk_assist_screen(const struct configtree_t *ign);

static bool do_set_odometer(const struct configtree_t *ign, int wh);

static const char *disable_enable[] = { "disable", "enable", 0 };
static const char *off_on[] = { "off", "on", 0 };

static const struct configtree_t cfgroot[] = {
	{ "Trip memory", F_SUBMENU, .submenu = &(const struct scroller_config){ 20, 58, 18, 0, 128, (const struct configtree_t[]) {
		{ "Reset trip A", F_BUTTON, .action = do_reset_trip_a },
		{ "Reset trip B", F_BUTTON, .action = do_reset_trip_b },
		{},
	}}},
	{ "Wheel", F_SUBMENU, .submenu = &(const struct scroller_config){ 20, 58, 36, 0, 128, (const struct configtree_t[]) {
		{ "Max speed", F_NUMERIC, .numeric = &(const struct cfgnumeric_t){ PTRSIZE(ui_vars.wheel_max_speed_x10), 1, "km/h", 10, 990, 10 }},
		{ "Circumference", F_NUMERIC, .numeric = &(const struct cfgnumeric_t){ PTRSIZE(ui_vars.ui16_wheel_perimeter), 0, "mm", 750, 3000, 10 }},
		{},
	}}},
	{ "Motor", F_SUBMENU, .submenu = &(const struct scroller_config){ 20, 58, 36, 0, 128, (const struct configtree_t[]) {
		{ "Power", F_OPTIONS, .options = &(const struct cfgoptions_t) { PTRSIZE(ui_vars.ui8_motor_power_option), (const char*[]){ "250W", "500W", "750W", "1000W", 0 }}},
		{ "Current ramp", F_NUMERIC, .numeric = &(const struct cfgnumeric_t) { PTRSIZE(ui_vars.ui8_ramp_up_amps_per_second_x10), 1, "A", 4, 100 }},
		{},
	}}},
	// (BBSHD has no torque sensor — the whole Torque submenu, its calibration
	//  screen, and Motor's TSDZ2 tuning knobs — Motor voltage, Control mode,
	//  Min current, Field weakening — were removed when this display was
	//  ported to the Bafang protocol.)
	{ "Assist", F_BUTTON, .action = cfg_push_assist_screen },
	{ "Walk assist", F_BUTTON, .action = cfg_push_walk_assist_screen },
	{ "Temperature", F_SUBMENU, .submenu = &(const struct scroller_config){ 20, 58, 36, 0, 128, (const struct configtree_t[]) {
		{ "Temp. sensor", F_OPTIONS, .options = &(const struct cfgoptions_t) { PTRSIZE(ui_vars.ui8_temperature_limit_feature_enabled), disable_enable } },
		{ "Min limit", F_NUMERIC, .numeric = &(const struct cfgnumeric_t) { PTRSIZE(ui_vars.ui8_motor_temperature_min_value_to_limit), 0, "C", 30, 100 }},
		{ "Max limit", F_NUMERIC, .numeric = &(const struct cfgnumeric_t) { PTRSIZE(ui_vars.ui8_motor_temperature_max_value_to_limit), 0, "C", 30, 100 }},
		{},
	}}},
	{ "Street mode", F_SUBMENU, .submenu = &(const struct scroller_config){ 20, 58, 36, 0, 128, (const struct configtree_t[]) {
		{ "Feature", F_OPTIONS, .options = &(const struct cfgoptions_t) { PTRSIZE(ui_vars.ui8_street_mode_function_enabled), disable_enable } },
		{ "Current status", F_OPTIONS, .options = &(const struct cfgoptions_t) { PTRSIZE(ui_vars.ui8_street_mode_enabled), off_on } },
		{ "Speed limit", F_NUMERIC, .numeric = &(const struct cfgnumeric_t) { PTRSIZE(ui_vars.ui8_street_mode_speed_limit), 0, "km/h", 1, 99 }},
		{},
	}}},
	{ "Various", F_SUBMENU, .submenu = &(const struct scroller_config){ 20, 58, 36, 0, 128, (const struct configtree_t[]) {
		{ "Odometer", F_NUMERIC|F_CALLBACK, .numeric_cb = &(const struct cfgnumeric_cb_t) { { PTRSIZE(ui_vars.ui32_odometer_x10), 1, "km", 0, INT32_MAX-100 }, do_set_odometer }},
		{ "Auto power off", F_NUMERIC, .numeric = &(const struct cfgnumeric_t) { PTRSIZE(ui_vars.ui8_lcd_power_off_time_minutes), 0, "min", 0, 255 }},
		{ "Reset BLE peers", F_BUTTON, .action = do_reset_ble },
		{ "Reset all settings", F_SUBMENU, .submenu = &(const struct scroller_config){ 20, 58, 18, 0, 128, (const struct configtree_t[]) {
			{ "Confirm reset all", F_BUTTON, .action = do_reset_all },
			{}
		}}},
		{}
	}}},
	{ "Technical", F_SUBMENU, .submenu = &(const struct scroller_config){ 20, 58, 36, 0, 128, (const struct configtree_t[]) {
		// ---- Bafang READ replies (live from communications() round-robin) ----
		{ "Motor status",     F_NUMERIC|F_RO, .numeric = &(const struct cfgnumeric_t) { PTRSIZE(g_bafang.status), 0, "" }},
		{ "Wheel RPM",        F_NUMERIC|F_RO, .numeric = &(const struct cfgnumeric_t) { PTRSIZE(g_bafang.wheel_rpm), 0, "rpm" }},
		{ "Battery voltage",  F_NUMERIC|F_RO, .numeric = &(const struct cfgnumeric_t) { PTRSIZE(g_bafang.battery_voltage_x10), 1, "V" }},
		{ "Motor current",    F_NUMERIC|F_RO, .numeric = &(const struct cfgnumeric_t) { PTRSIZE(g_bafang.current_amp_x2), 0, "*0.5A" }},
		{ "Motor temp/pwr",   F_NUMERIC|F_RO, .numeric = &(const struct cfgnumeric_t) { PTRSIZE(g_bafang.range_field), 0, "" }},
		{ "Battery %",        F_NUMERIC|F_RO, .numeric = &(const struct cfgnumeric_t) { PTRSIZE(g_bafang.battery_pct), 0, "%" }},
		{ "Moving",           F_NUMERIC|F_RO, .numeric = &(const struct cfgnumeric_t) { PTRSIZE(g_bafang.moving), 0, "" }},
		// ---- UART link diagnostics ----
		{ "RX packets",       F_NUMERIC|F_RO, .numeric = &(const struct cfgnumeric_t) { PTRSIZE(g_bafang.rx_count), 0, "" }},
		{ "Checksum fails",   F_NUMERIC|F_RO, .numeric = &(const struct cfgnumeric_t) { PTRSIZE(g_bafang.chk_fail_count), 0, "" }},
		{ "Reply timeouts",   F_NUMERIC|F_RO, .numeric = &(const struct cfgnumeric_t) { PTRSIZE(g_bafang.timeout_count), 0, "" }},
		// ---- Stubs, revisit later ----
		// BBSHD reports no pedal cadence over the display protocol; this
		// currently reads a hard-coded 99. Revisit when we decide whether
		// to synthesise from PAS state or hide the field.
		{ "Cadence (stub)",   F_NUMERIC|F_RO, .numeric = &(const struct cfgnumeric_t) { PTRSIZE(ui_vars.ui8_pedal_cadence), 0, "rpm" }},
		{},
	}}},
	{}
};

const struct scroller_config cfg_root = { 20, 58, 18, 0, 128,  cfgroot };

// Bafang PAS: the rider only chooses how many discrete assist levels to cycle
// through (3/5/9). The motor firmware owns the power delivered at each level, so
// there is no per-level percentage tuning and no level-count interpolation to
// recompute (Bafang doesn't support either).
static void do_set_assist_levels_3(const struct configtree_t *ign);
static void do_set_assist_levels_5(const struct configtree_t *ign);
static void do_set_assist_levels_9(const struct configtree_t *ign);

const struct scroller_config cfg_assist = { 20, 58, 18, 0, 128, (const struct configtree_t[]) {
	{ "3 levels", F_BUTTON, .action = do_set_assist_levels_3 },
	{ "5 levels", F_BUTTON, .action = do_set_assist_levels_5 },
	{ "9 levels", F_BUTTON, .action = do_set_assist_levels_9 },
	{}
}};

static void set_assist_levels(int n)
{
	ui_vars.ui8_number_of_assist_levels = n;
	if(ui_vars.ui8_assist_level > n)
		ui_vars.ui8_assist_level = n;
	sstack_pop();
}
static void do_set_assist_levels_3(const struct configtree_t *ign) { set_assist_levels(3); }
static void do_set_assist_levels_5(const struct configtree_t *ign) { set_assist_levels(5); }
static void do_set_assist_levels_9(const struct configtree_t *ign) { set_assist_levels(9); }

// Walk assist is a plain on/off feature; present it the same way as the assist
// menu — a submenu listing the two choices as selectable rows.
static void do_set_walk_assist_off(const struct configtree_t *ign);
static void do_set_walk_assist_on(const struct configtree_t *ign);

const struct scroller_config cfg_walk_assist = { 20, 58, 18, 0, 128, (const struct configtree_t[]) {
	{ "Disabled", F_BUTTON, .action = do_set_walk_assist_off },
	{ "Enabled", F_BUTTON, .action = do_set_walk_assist_on },
	{}
}};

static void set_walk_assist(int enabled)
{
	ui_vars.ui8_walk_assist_feature_enabled = enabled;
	sstack_pop();
}
static void do_set_walk_assist_off(const struct configtree_t *ign) { set_walk_assist(0); }
static void do_set_walk_assist_on(const struct configtree_t *ign) { set_walk_assist(1); }

static void do_reset_trip_a(const struct configtree_t *ign)
{
	// FIXME is accessing rt_vars safe here?
	rt_vars.ui32_trip_a_distance_x1000 = 0;
	rt_vars.ui32_trip_a_time = 0;
	rt_vars.ui16_trip_a_avg_speed_x10 = 0;
	rt_vars.ui16_trip_a_max_speed_x10 = 0;
	sstack_pop();
}

static void do_reset_trip_b(const struct configtree_t *ign)
{
	rt_vars.ui32_trip_b_distance_x1000 = 0;
	rt_vars.ui32_trip_b_time = 0;
	rt_vars.ui16_trip_b_avg_speed_x10 = 0;
	rt_vars.ui16_trip_b_max_speed_x10 = 0;
	sstack_pop();
}

#if defined(NRF51)
#include "peer_manager.h"
#endif

static void do_reset_ble(const struct configtree_t *ign)
{
#if defined(NRF51)
	// TODO: fist disable any connection
	// Warning: Use this (pm_peers_delete) function only when not connected or connectable. If a peer is or becomes connected
	// or a PM_PEER_DATA_FUNCTIONS function is used during this procedure (until the success or failure event happens),
	// the behavior is undefined.
	pm_peers_delete();
#endif
	sstack_pop();
}

static void do_reset_all(const struct configtree_t *ign)
{
	eeprom_init_defaults();
	showScreen(&screen_main);
}

static bool do_set_odometer(const struct configtree_t *ign, int v)
{
	// FIXME rt_vars?
	rt_vars.ui32_odometer_x10 = v;
	return true;
}



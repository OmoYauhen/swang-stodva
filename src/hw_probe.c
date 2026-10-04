/*
 * Hardware probe (HW_PROBE=1 builds only).
 *
 * A "HW probe" entry at the top of the config menu opens a list of diagnostic
 * pages. Each page is a text console drawn with a 3x5 pixel font (16 columns x
 * 18 rows) so a phone photo of the screen captures everything. Page contents,
 * controls and test procedures are documented in docs/hw-probe.md.
 *
 * Common controls: PWR click = back, UP/DOWN click = previous/next screen on
 * pages longer than one screen. Page-specific controls are listed on the page.
 *
 * Released under the GPL License, Version 3
 */
#ifdef HW_PROBE

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hw_probe.h"
#include "screen_cfg_utils.h"
#include "ui.h"
#include "lcd.h"
#include "state.h"
#include "buttons.h"
#include "eeprom.h"
#include "main.h"
#include "custom_board.h"
#include "nrf.h"
#include "nrf_soc.h"
#include "nrf_nvic.h"
#include "fds.h"

extern bool useSoftDevice;
extern uint32_t stack_overflow_debug(void);
extern uint32_t __data_start__, __bss_end__, __HeapBase, __HeapLimit, __StackLimit, __StackTop;

// SDK 12 secure bootloader: GPREGRET value that makes it stay in DFU mode.
#define BOOTLOADER_DFU_START 0xB1

// ===========================================================================
// TIMER1: free-running 1 MHz, 16-bit hardware counter (the nRF51 TIMER1 has no
// 32-bit mode) extended to 32 bits by counting wraps in the COMPARE[0] IRQ.
// CC[1] is used for software captures, CC[2] for the 1 kHz button sampler.

static volatile uint16_t t1_wraps;
static void bounce_sample(void);

void TIMER1_IRQHandler(void)
{
	if (NRF_TIMER1->EVENTS_COMPARE[0]) {
		NRF_TIMER1->EVENTS_COMPARE[0] = 0;
		t1_wraps++;
	}
	if (NRF_TIMER1->EVENTS_COMPARE[2]) {
		NRF_TIMER1->EVENTS_COMPARE[2] = 0;
		NRF_TIMER1->CC[2] = (NRF_TIMER1->CC[2] + 1000) & 0xFFFF;
		bounce_sample();
	}
	(void)NRF_TIMER1->EVENTS_COMPARE[0]; // flush the event clear before returning
}

uint32_t hw_probe_now_us(void)
{
	uint32_t hi, lo, pending;
	do {
		hi = t1_wraps;
		NRF_TIMER1->TASKS_CAPTURE[1] = 1;
		lo = NRF_TIMER1->CC[1];
		pending = NRF_TIMER1->EVENTS_COMPARE[0];
	} while (hi != t1_wraps);
	// wrapped after we read t1_wraps but the IRQ hasn't run yet (we're in a
	// context at or above its priority): count the wrap ourselves
	if (pending && lo < 0x8000)
		hi++;
	return (hi << 16) | lo;
}

static uint8_t gpregret_at_boot;

void hw_probe_early_init(void)
{
	gpregret_at_boot = (uint8_t)NRF_POWER->GPREGRET;

	NRF_TIMER1->TASKS_STOP = 1;
	NRF_TIMER1->MODE = TIMER_MODE_MODE_Timer;
	NRF_TIMER1->BITMODE = TIMER_BITMODE_BITMODE_16Bit;
	NRF_TIMER1->PRESCALER = 4; // 16 MHz / 2^4 = 1 MHz
	NRF_TIMER1->CC[0] = 0;
	NRF_TIMER1->INTENSET = TIMER_INTENSET_COMPARE0_Msk; // COMPARE2 only while page 11 is open
	// SoftDevice isn't enabled yet, so plain CMSIS is fine. Priority 3 (APP_LOW)
	// is one the SoftDevice accepts when it is enabled later.
	NVIC_SetPriority(TIMER1_IRQn, 3);
	NVIC_ClearPendingIRQ(TIMER1_IRQn);
	NVIC_EnableIRQ(TIMER1_IRQn);
	NRF_TIMER1->TASKS_CLEAR = 1;
	NRF_TIMER1->TASKS_START = 1;
}

// ===========================================================================
// Frame timing

static uint32_t frame_t0, frame_flush_us;
static struct {
	uint32_t ui_sum, ui_max, lcd_sum, lcd_max, total_max;
	uint32_t frames, flushes, over20;
	uint32_t missed_base, missed_now;
} ft;
static volatile bool ft_reset_req = true;

void hw_probe_frame_begin(void)
{
	frame_flush_us = 0;
	frame_t0 = hw_probe_now_us();
}

void hw_probe_lcd_flush_done(uint32_t t0_us)
{
	uint32_t d = hw_probe_now_us() - t0_us;
	frame_flush_us += d;
	ft.lcd_sum += d;
	ft.flushes++;
	if (d > ft.lcd_max)
		ft.lcd_max = d;
}

void hw_probe_frame_end(uint32_t ticks_missed)
{
	uint32_t total = hw_probe_now_us() - frame_t0;
	uint32_t ui = total > frame_flush_us ? total - frame_flush_us : 0;

	if (ft_reset_req) {
		memset(&ft, 0, sizeof(ft));
		ft.missed_base = ticks_missed;
		ft_reset_req = false;
	}
	ft.missed_now = ticks_missed;
	ft.ui_sum += ui;
	ft.frames++;
	if (ui > ft.ui_max)
		ft.ui_max = ui;
	if (total > ft.total_max)
		ft.total_max = total;
	if (total > 20000)
		ft.over20++;
}

// ===========================================================================
// SoftDevice RAM requirement

static uint32_t sd_ram_in, sd_ram_min, sd_enable_err = 0xFFFFFFFF;

uint32_t hw_probe_softdevice_enable(ble_enable_params_t *params)
{
	sd_ram_in = (uint32_t)&__data_start__;
	sd_ram_min = sd_ram_in;
	sd_enable_err = sd_ble_enable(params, &sd_ram_min);
	return sd_enable_err;
}

// ===========================================================================
// Motor raw replies (called from the communications() IRQ context)

#define MOTOR_SLOTS 8
static struct {
	uint8_t op, len, seen;
	uint8_t raw[3];
	uint32_t count;
} motor_rx[MOTOR_SLOTS];
static uint16_t rpm_max;
#define STATUS_HIST_LEN 12
static struct { uint8_t value; uint32_t count; } status_hist[STATUS_HIST_LEN];
static uint8_t status_hist_n;
static uint32_t status_hist_overflow;

void hw_probe_motor_rx(uint8_t slot, uint8_t opcode, const uint8_t *rx, uint8_t len)
{
	if (slot >= MOTOR_SLOTS)
		return;
	if (len > 3)
		len = 3;
	motor_rx[slot].op = opcode;
	motor_rx[slot].len = len;
	memcpy(motor_rx[slot].raw, rx, len);
	motor_rx[slot].seen = 1;
	motor_rx[slot].count++;

	if (opcode == 0x20 && len >= 2) {
		uint16_t rpm = ((uint16_t)rx[0] << 8) | rx[1];
		if (rpm > rpm_max)
			rpm_max = rpm;
	}

	if (opcode == 0x08) {
		for (int i = 0; i < status_hist_n; i++) {
			if (status_hist[i].value == rx[0]) {
				status_hist[i].count++;
				return;
			}
		}
		if (status_hist_n < STATUS_HIST_LEN) {
			status_hist[status_hist_n].value = rx[0];
			status_hist[status_hist_n].count = 1;
			status_hist_n++;
		} else {
			status_hist_overflow++;
		}
	}
}

// ===========================================================================
// 3x5 font + text console (16 cols x 18 rows of 4x7 px cells)

static const struct { char c; uint8_t rows[5]; } tiny_font[] = {
	{ '0', {7,5,5,5,7} }, { '1', {2,6,2,2,7} }, { '2', {7,1,7,4,7} }, { '3', {7,1,3,1,7} },
	{ '4', {5,5,7,1,1} }, { '5', {7,4,7,1,7} }, { '6', {7,4,7,5,7} }, { '7', {7,1,1,2,2} },
	{ '8', {7,5,7,5,7} }, { '9', {7,5,7,1,7} },
	{ 'A', {2,5,7,5,5} }, { 'B', {6,5,6,5,6} }, { 'C', {3,4,4,4,3} }, { 'D', {6,5,5,5,6} },
	{ 'E', {7,4,6,4,7} }, { 'F', {7,4,6,4,4} }, { 'G', {3,4,5,5,3} }, { 'H', {5,5,7,5,5} },
	{ 'I', {7,2,2,2,7} }, { 'J', {1,1,1,5,2} }, { 'K', {5,6,4,6,5} }, { 'L', {4,4,4,4,7} },
	{ 'M', {5,7,7,5,5} }, { 'N', {6,5,5,5,5} }, { 'O', {2,5,5,5,2} }, { 'P', {6,5,6,4,4} },
	{ 'Q', {2,5,5,6,3} }, { 'R', {6,5,6,5,5} }, { 'S', {3,4,2,1,6} }, { 'T', {7,2,2,2,2} },
	{ 'U', {5,5,5,5,7} }, { 'V', {5,5,5,5,2} }, { 'W', {5,5,7,7,5} }, { 'X', {5,5,2,5,5} },
	{ 'Y', {5,5,2,2,2} }, { 'Z', {7,1,2,4,7} },
	{ ':', {0,2,0,2,0} }, { '.', {0,0,0,0,2} }, { '-', {0,0,7,0,0} }, { '/', {1,1,2,4,4} },
	{ '%', {5,1,2,4,5} }, { '+', {0,2,7,2,0} }, { '=', {0,7,0,7,0} }, { '>', {4,2,1,2,4} },
	{ '<', {1,2,4,2,1} }, { '(', {1,2,2,2,1} }, { ')', {4,2,2,2,4} }, { '_', {0,0,0,0,7} },
	{ '*', {0,5,2,5,0} }, { '#', {5,7,5,7,5} }, { '?', {6,1,2,0,2} }, { '!', {2,2,2,0,2} },
	{ ',', {0,0,0,2,4} }, { '^', {2,5,0,0,0} },
};

static const uint8_t *tiny_glyph(char c)
{
	if (c >= 'a' && c <= 'z')
		c -= 'a' - 'A';
	for (unsigned i = 0; i < sizeof(tiny_font) / sizeof(tiny_font[0]); i++)
		if (tiny_font[i].c == c)
			return tiny_font[i].rows;
	return NULL; // space and anything unknown draw as blank
}

// Portrait text, top-left at (x, y).
static void tiny_text(int x, int y, const char *s)
{
	for (; *s; s++, x += 4) {
		const uint8_t *g = tiny_glyph(*s);
		if (!g)
			continue;
		for (int r = 0; r < 5; r++)
			for (int c = 0; c < 3; c++)
				if (g[r] & (4 >> c)) {
					int px = x + c, py = y + r;
					if (px >= 0 && px < 64 && py >= 0 && py < 128)
						lcd_pset(px, py, true);
				}
	}
}

#define CON_ROWS 18
#define CON_PITCH 7
static int con_line, con_scroll, con_total;

static void con_printf(const char *fmt, ...)
{
	char buf[24];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	buf[16] = 0; // 16 columns
	int row = con_line - con_scroll;
	if (row >= 0 && row < CON_ROWS)
		tiny_text(0, 1 + row * CON_PITCH, buf);
	con_line++;
}

static void con_end(void)
{
	con_total = con_line;
	if (con_total > CON_ROWS) {
		// scrollbar in the free column x = 63
		int h = 128 * CON_ROWS / con_total;
		int y = 128 * con_scroll / con_total;
		for (int i = 0; i < h && y + i < 128; i++)
			lcd_pset(63, y + i, true);
	}
}

static void con_page(int dir)
{
	int step = CON_ROWS - 1;
	con_scroll += dir * step;
	if (con_scroll > con_total - CON_ROWS)
		con_scroll = con_total - CON_ROWS;
	if (con_scroll < 0)
		con_scroll = 0;
}

static void fmt_x10(char *out, size_t n, uint32_t v_x10) // "12.3"
{
	snprintf(out, n, "%lu.%lu", (unsigned long)(v_x10 / 10), (unsigned long)(v_x10 % 10));
}

// ===========================================================================
// Page 1: chip identity

#define REG(a) (*(const volatile uint32_t *)(a))

static void page_chip_draw(void)
{
	static const struct { const char *name; uint32_t addr; } regs[] = {
		{ "PGSIZE",  0x10000010 }, { "CODESZ",  0x10000014 }, { "CLENR0",  0x10000028 },
		{ "PPFC",    0x1000002C }, { "NRAMBLK", 0x10000034 }, { "RAMBLK0", 0x10000038 },
		{ "RAMBLK1", 0x1000003C }, { "RAMBLK2", 0x10000040 }, { "RAMBLK3", 0x10000044 },
		{ "CFGID",   0x1000005C }, { "DEVID0",  0x10000060 }, { "DEVID1",  0x10000064 },
		{ "ADDRTYP", 0x100000A0 }, { "DEVADR0", 0x100000A4 }, { "DEVADR1", 0x100000A8 },
		{ "PART",    0x10000100 }, { "VARIANT", 0x10000104 }, { "PACKAGE", 0x10000108 },
		{ "INFORAM", 0x1000010C }, { "INFOFLS", 0x10000110 }, { "BLADDR",  0x10001014 },
	};

	con_printf("1 CHIP ID");
	uint32_t nblk = REG(0x10000034);
	uint32_t ram = 0;
	for (uint32_t i = 0; i < nblk && i < 4; i++)
		ram += REG(0x10000038 + 4 * i);
	con_printf("RAM KB  %lu", (unsigned long)(ram / 1024));
	con_printf("FLASH KB %lu", (unsigned long)(REG(0x10000010) * REG(0x10000014) / 1024));

	uint32_t var = REG(0x10000104);
	char v[5] = { (char)(var >> 24), (char)(var >> 16), (char)(var >> 8), (char)var, 0 };
	for (int i = 0; i < 4; i++)
		if (v[i] < ' ' || v[i] > 'Z')
			v[i] = '?';
	con_printf("VARIANT %s", v);
	con_printf("HWID    %04lX", (unsigned long)(REG(0x1000005C) & 0xFFFF));

	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
		con_printf("%-7s %08lX", regs[i].name, (unsigned long)REG(regs[i].addr));
}

// ===========================================================================
// Page 2: RAM map

static void page_ram_draw(void)
{
	uint32_t stk = (uint32_t)&__StackTop - (uint32_t)&__StackLimit;
	uint32_t unused = stack_overflow_debug();
	uint32_t sp;
	__asm volatile ("mov %0, sp" : "=r" (sp));

	con_printf("2 RAM MAP");
	con_printf("DATA    %08lX", (unsigned long)&__data_start__);
	con_printf("BSSEND  %08lX", (unsigned long)&__bss_end__);
	con_printf("HEAPB   %08lX", (unsigned long)&__HeapBase);
	con_printf("HEAPL   %08lX", (unsigned long)&__HeapLimit);
	con_printf("STKLIM  %08lX", (unsigned long)&__StackLimit);
	con_printf("STKTOP  %08lX", (unsigned long)&__StackTop);
	con_printf("SP NOW  %08lX", (unsigned long)sp);
	con_printf("GAP B   %lu", (unsigned long)((uint32_t)&__StackLimit - (uint32_t)&__HeapLimit));
	con_printf("SD RAM BASE:");
	if (sd_enable_err == 0xFFFFFFFF) {
		con_printf(" NOT CALLED");
	} else {
		con_printf("IN      %08lX", (unsigned long)sd_ram_in);
		con_printf("MIN     %08lX", (unsigned long)sd_ram_min);
		con_printf("ERR     %lu", (unsigned long)sd_enable_err);
	}
	con_printf("STACK:");
	con_printf("SIZE    %lu", (unsigned long)stk);
	con_printf("USED    %lu", (unsigned long)(stk - unused));
	con_printf("FREE    %lu", (unsigned long)unused);
}

// ===========================================================================
// Page 3: frame timing

static uint8_t ft_held; // bit0 = UP, bit1 = DOWN

static void page_timing_draw(void)
{
	con_printf("3 FRAME TIME US");
	uint32_t f = ft.frames ? ft.frames : 1, l = ft.flushes ? ft.flushes : 1;
	con_printf("UI AVG  %lu", (unsigned long)(ft.ui_sum / f));
	con_printf("UI MAX  %lu", (unsigned long)ft.ui_max);
	con_printf("LCD AVG %lu", (unsigned long)(ft.lcd_sum / l));
	con_printf("LCD MAX %lu", (unsigned long)ft.lcd_max);
	con_printf("TOT MAX %lu", (unsigned long)ft.total_max);
	con_printf("FRAMES  %lu", (unsigned long)ft.frames);
	con_printf(">20MS   %lu", (unsigned long)ft.over20);
	con_printf("MISSED  %lu", (unsigned long)(ft.missed_now - ft.missed_base));
	con_printf("SECS    %lu", (unsigned long)(ft.frames / 50));
	con_printf("");
	con_printf("UP+DN OR M:RESET");
}

static void page_timing_button(int but)
{
	if (but & UP_PRESS) {
		if (ft_held & 2) ft_reset_req = true;
		ft_held |= 1;
	}
	if (but & DOWN_PRESS) {
		if (ft_held & 1) ft_reset_req = true;
		ft_held |= 2;
	}
	if (but & UP_RELEASE)
		ft_held &= ~1;
	if (but & DOWN_RELEASE)
		ft_held &= ~2;
	if (but & M_CLICK)
		ft_reset_req = true;
}

static void page_timing_enter(void) { ft_held = 0; }

// ===========================================================================
// Page 4: flash (FDS) timing

static uint32_t fds_last_us, fds_max_us, fds_writes;

static void page_fds_draw(void)
{
	char a[12], b[12];
	con_printf("4 FLASH FDS");
	con_printf("M: WRITE NOW");
	fmt_x10(a, sizeof(a), fds_last_us / 100);
	fmt_x10(b, sizeof(b), fds_max_us / 100);
	con_printf("LAST MS %s", a);
	con_printf("MAX MS  %s", b);
	con_printf("WRITES  %lu", (unsigned long)fds_writes);
	if (!useSoftDevice) {
		con_printf("NO SOFTDEVICE");
		return;
	}
	fds_stat_t st;
	uint32_t err = fds_stat(&st);
	con_printf("STAT ERR %lu", (unsigned long)err);
	con_printf("OPEN    %u", st.open_records);
	con_printf("VALID   %u", st.valid_records);
	con_printf("DIRTY   %u", st.dirty_records);
	con_printf("RESERVD %u", st.words_reserved);
	con_printf("USED W  %u", st.words_used);
	con_printf("LARGEST %u", st.largest_contig);
	con_printf("FREEABL %u", st.freeable_words);
}

static void page_fds_action(void)
{
	uint32_t t0 = hw_probe_now_us();
	eeprom_write_variables();
	fds_last_us = hw_probe_now_us() - t0;
	if (fds_last_us > fds_max_us)
		fds_max_us = fds_last_us;
	fds_writes++;
}

// ===========================================================================
// Page 5: motor raw values

static const char *const slot_names[MOTOR_SLOTS] = { "ST", "CUR", "BRK", "BAT", "SPD", "RNG", "CAL", "MOV" };

static void page_motor_draw(void)
{
	con_printf("5 MOTOR RAW");
	for (int i = 0; i < MOTOR_SLOTS; i++) {
		char hex[8] = "--";
		if (motor_rx[i].seen) {
			hex[0] = 0;
			for (int k = 0; k < motor_rx[i].len; k++)
				snprintf(hex + 2 * k, sizeof(hex) - 2 * k, "%02X", motor_rx[i].raw[k]);
		}
		con_printf("%-3s %-6s %lu", slot_names[i], hex, (unsigned long)motor_rx[i].count);
	}
	char a[12];
	uint8_t amp_x2 = g_bafang.current_amp_x2;
	snprintf(a, sizeof(a), "%u.%u", amp_x2 / 2, (amp_x2 & 1) * 5);
	con_printf("CUR A   %s", a);
	con_printf("CUR X5  %u", rt_vars.ui8_battery_current_x5);
	con_printf("RPM     %u", g_bafang.wheel_rpm);
	con_printf("RPM MAX %u", rpm_max);
	con_printf("STATUS HIST:");
	for (int i = 0; i < status_hist_n; i++)
		con_printf(" %02X: %lu", status_hist[i].value, (unsigned long)status_hist[i].count);
	if (status_hist_overflow)
		con_printf(" OVFL %lu", (unsigned long)status_hist_overflow);
}

// ===========================================================================
// Page 6: speed-limit encoding

static const uint8_t sl_values[] = { 15, 20, 25, 30 };
static uint8_t sl_idx = 2, sl_enc; // enc 0 = km/h x10, 1 = wheel RPM
static uint8_t sl_tx[5];
static uint32_t sl_sends;
static bool sl_sent_any;

static uint16_t sl_wire_value(void)
{
	uint32_t kmh = sl_values[sl_idx];
	if (sl_enc == 0)
		return (uint16_t)(kmh * 10);
	uint32_t perim = ui_vars.ui16_wheel_perimeter ? ui_vars.ui16_wheel_perimeter : 2165;
	// RPM = km/h * 1000 / 60 / perimeter_m, rounded
	return (uint16_t)((kmh * 1000000u + perim * 30u) / (perim * 60u));
}

static void send_speed_limit_frame(uint16_t v)
{
	uint8_t f[5] = { 0x16, 0x1F, (uint8_t)(v >> 8), (uint8_t)v, 0 };
	f[4] = (uint8_t)(f[0] + f[1] + f[2] + f[3]);
	if (bafang_probe_queue_raw(f, sizeof(f)))
		memcpy(sl_tx, f, sizeof(f));
}

static void page_speed_draw(void)
{
	char a[12];
	con_printf("6 SPEED LIMIT");
	con_printf("ENC  %s", sl_enc ? "WHEEL RPM" : "KMH X10");
	con_printf("SET  %u KMH", sl_values[sl_idx]);
	con_printf("WIRE %u", sl_wire_value());
	con_printf("PERIM %u", ui_vars.ui16_wheel_perimeter);
	if (sl_sent_any)
		con_printf("TX %02X%02X%02X%02X%02X", sl_tx[0], sl_tx[1], sl_tx[2], sl_tx[3], sl_tx[4]);
	else
		con_printf("TX --");
	con_printf("SENDS %lu", (unsigned long)sl_sends);
	fmt_x10(a, sizeof(a), ui_vars.ui16_wheel_speed_x10);
	con_printf("SPD  %s KMH", a);
	con_printf("RPM  %u", g_bafang.wheel_rpm);
	con_printf("PAS  %u", ui_vars.ui8_assist_level);
	con_printf("");
	con_printf("UP:VALUE DN:ENC");
	con_printf("M:SEND");
	con_printf("BACK RESTORES");
	con_printf("CFG LIMIT X10");
}

static void page_speed_button(int but)
{
	if (but & UP_CLICK)
		sl_idx = (sl_idx + 1) % sizeof(sl_values);
	if (but & DOWN_CLICK)
		sl_enc ^= 1;
	if (but & M_CLICK) {
		send_speed_limit_frame(sl_wire_value());
		sl_sends++;
		sl_sent_any = true;
	}
}

static void page_speed_leave(void)
{
	// Put the configured limit back the way normal boot sends it (km/h x 10).
	if (sl_sent_any)
		send_speed_limit_frame((uint16_t)ui_vars.ui8_street_mode_speed_limit * 10);
	sl_sent_any = false;
}

// ===========================================================================
// Page 7: walk-assist keep-alive

static uint8_t wa_repeat;
static bool wa_held;
static uint32_t wa_start_ms, wa_last_tx_ms, wa_last_hold_ms, wa_stop_ms, wa_tx;
static bool wa_moved;

static void page_walk_draw(void)
{
	char a[12];
	uint32_t now = get_time_base_counter_1ms();

	if (wa_held) {
		if (ui_vars.ui16_wheel_speed_x10 > 0)
			wa_moved = true;
		else if (wa_moved && !wa_stop_ms)
			wa_stop_ms = now - wa_start_ms;

		if (wa_repeat && now - wa_last_tx_ms >= 500) {
			static const uint8_t f[4] = { 0x16, 0x0B, 0x06, 0x27 };
			if (bafang_probe_queue_raw(f, sizeof(f))) {
				wa_last_tx_ms = now;
				wa_tx++;
			}
		}
	}

	con_printf("7 WALK ASSIST");
	con_printf("MODE %s", wa_repeat ? "REPEAT 500MS" : "ONCE");
	con_printf("HELD %s", wa_held ? "YES" : "NO");
	fmt_x10(a, sizeof(a), (wa_held ? now - wa_start_ms : wa_last_hold_ms) / 100);
	con_printf("TIME %s S", a);
	if (wa_stop_ms) {
		fmt_x10(a, sizeof(a), wa_stop_ms / 100);
		con_printf("STOP %s S", a);
	} else {
		con_printf("STOP --");
	}
	fmt_x10(a, sizeof(a), ui_vars.ui16_wheel_speed_x10);
	con_printf("SPD  %s KMH", a);
	con_printf("RPM  %u", g_bafang.wheel_rpm);
	con_printf("RESENDS %lu", (unsigned long)wa_tx);
	con_printf("");
	con_printf("M:MODE");
	con_printf("HOLD DN:WALK");
}

static void wa_stop(void)
{
	if (wa_held)
		wa_last_hold_ms = get_time_base_counter_1ms() - wa_start_ms;
	wa_held = false;
	ui_vars.ui8_walk_assist = 0;
}

static void page_walk_button(int but)
{
	if ((but & M_CLICK) && !wa_held)
		wa_repeat ^= 1;
	if (but & DOWN_PRESS) {
		wa_held = true;
		wa_moved = false;
		wa_stop_ms = 0;
		wa_tx = 0;
		wa_start_ms = wa_last_tx_ms = get_time_base_counter_1ms();
		ui_vars.ui8_walk_assist = 1; // normal path sends PAS 0x06 once
	}
	if (but & DOWN_RELEASE)
		wa_stop();
}

// ===========================================================================
// Page 8: DFU entry via GPREGRET

static void page_dfu_draw(void)
{
	con_printf("8 DFU ENTRY");
	con_printf("GPREGRET BOOT %02X", gpregret_at_boot);
	con_printf("VER NUM %lu", (unsigned long)VERSION_NUM);
	con_printf("SOFTDEV %s", useSoftDevice ? "YES" : "NO");
	con_printf("");
	con_printf("M: REBOOT TO DFU");
	con_printf("(GPREGRET=B1)");
	con_printf("");
	con_printf("BACK TO APP:");
	con_printf("POWER OFF, THEN");
	con_printf("LONG PRESS PWR");
}

static void page_dfu_action(void)
{
	if (useSoftDevice) {
		sd_power_gpregret_clr(0xFF);
		sd_power_gpregret_set(BOOTLOADER_DFU_START);
		sd_nvic_SystemReset();
	} else {
		NRF_POWER->GPREGRET = BOOTLOADER_DFU_START;
		NVIC_SystemReset();
	}
}

// ===========================================================================
// Page 9: landscape orientation test

static const uint8_t orient_seg[4] = { 0xA0, 0xA1, 0xA0, 0xA1 };
static const uint8_t orient_com[4] = { 0xC0, 0xC0, 0xC8, 0xC8 };
static uint8_t orient_idx;

// landscape framebuffer fb[64 rows][16 bytes], row-major, bit 0 = leftmost
static void lpset(int lx, int ly)
{
	if (lx >= 0 && lx < 128 && ly >= 0 && ly < 64)
		framebuffer.u8[ly * 16 + lx / 8] |= 1 << (lx & 7);
}

static void ltext(int lx, int ly, const char *s, int scale)
{
	for (; *s; s++, lx += 4 * scale) {
		const uint8_t *g = tiny_glyph(*s);
		if (!g)
			continue;
		for (int r = 0; r < 5 * scale; r++)
			for (int c = 0; c < 3 * scale; c++)
				if (g[r / scale] & (4 >> (c / scale)))
					lpset(lx + c, ly + r);
	}
}

static void orient_apply(uint8_t idx)
{
	uint8_t cmds[2] = { orient_seg[idx], orient_com[idx] };
	lcd_send_cmds(cmds, sizeof(cmds));
}

static void page_orient_draw(void)
{
	char name[8];
	memset(framebuffer.u8, 0, sizeof(framebuffer.u8));

	// "TOP" + an arrow pointing up, centred at the top
	ltext(64 - 17, 2, "TOP", 3);
	for (int y = 22; y < 54; y++) { lpset(63, y); lpset(64, y); }
	for (int i = 0; i < 8; i++) { lpset(63 - i, 22 + i); lpset(64 + i, 22 + i); }

	// origin pixel and far-corner 3x3 block
	lpset(0, 0);
	for (int y = 61; y < 64; y++)
		for (int x = 125; x < 128; x++)
			lpset(x, y);

	snprintf(name, sizeof(name), "%02X %02X", orient_seg[orient_idx], orient_com[orient_idx]);
	ltext(4, 50, name, 2);
	ltext(84, 52, "M:NEXT", 1);
}

static void page_orient_enter(void) { orient_idx = 0; orient_apply(0); }
static void page_orient_action(void) { orient_idx = (orient_idx + 1) % 4; orient_apply(orient_idx); }
static void page_orient_leave(void) { orient_apply(0); } // init sequence uses A0 / C0

// ===========================================================================
// Page 10: BLE identity

static void page_ble_draw(void)
{
	uint32_t a0 = REG(0x100000A4), a1 = REG(0x100000A8);
	uint8_t ficr[6] = { (uint8_t)a0, (uint8_t)(a0 >> 8), (uint8_t)(a0 >> 16), (uint8_t)(a0 >> 24),
	                    (uint8_t)a1, (uint8_t)((a1 >> 8) | 0xC0) }; // static random: top 2 bits set

	con_printf("10 BLE ADDR");
	con_printf("FICR TYPE %lu", (unsigned long)(REG(0x100000A0) & 1));
	con_printf("FICR %02X%02X%02X%02X%02X%02X", ficr[5], ficr[4], ficr[3], ficr[2], ficr[1], ficr[0]);
	if (!useSoftDevice) {
		con_printf("NO SOFTDEVICE");
		return;
	}
	ble_gap_addr_t addr;
	uint32_t err = sd_ble_gap_address_get(&addr);
	con_printf("GAP ERR %lu", (unsigned long)err);
	if (err)
		return;
	static const char *const types[] = { "PUBLIC", "RND STATIC", "RND RESOLV", "RND NONRES" };
	con_printf("GAP TYPE %u", addr.addr_type);
	con_printf(" %s", addr.addr_type < 4 ? types[addr.addr_type] : "?");
	const uint8_t *g = addr.addr;
	con_printf("GAP  %02X%02X%02X%02X%02X%02X", g[5], g[4], g[3], g[2], g[1], g[0]);
	con_printf("MATCH %s", memcmp(g, ficr, 6) ? "NO" : "YES");
}

// ===========================================================================
// Page 11: button bounce (1 kHz sampling on TIMER1 CC[2] while open)

#define BOUNCE_QUIET_MS 30
static const uint8_t bounce_pins[4] = { BUTTON_PWR__PIN, BUTTON_UP__PIN, BUTTON_DOWN__PIN, BUTTON_M__PIN };
static const char *const bounce_names[4] = { "PWR", "UP", "DN", "M" };
static struct {
	uint8_t level, edges, last_edges, max_edges;
	uint8_t quiet;
	uint16_t start_ms, last_edge_ms, max_span_ms;
	uint16_t transitions;
} bounce[4];
static uint16_t bounce_ms;

static void bounce_sample(void)
{
	uint32_t in = NRF_GPIO->IN;
	bounce_ms++;
	for (int i = 0; i < 4; i++) {
		uint8_t lvl = (in >> bounce_pins[i]) & 1;
		if (lvl != bounce[i].level) {
			bounce[i].level = lvl;
			if (bounce[i].quiet >= BOUNCE_QUIET_MS) {
				bounce[i].edges = 0;
				bounce[i].start_ms = bounce_ms;
			}
			bounce[i].edges++;
			bounce[i].last_edge_ms = bounce_ms;
			bounce[i].quiet = 0;
		} else if (bounce[i].quiet < 255) {
			if (++bounce[i].quiet == BOUNCE_QUIET_MS && bounce[i].edges) {
				// burst finished: one press or one release
				uint16_t span = bounce[i].last_edge_ms - bounce[i].start_ms;
				bounce[i].last_edges = bounce[i].edges;
				if (bounce[i].edges > bounce[i].max_edges)
					bounce[i].max_edges = bounce[i].edges;
				if (span > bounce[i].max_span_ms)
					bounce[i].max_span_ms = span;
				bounce[i].transitions++;
			}
		}
	}
}

static void page_bounce_enter(void)
{
	uint32_t in = NRF_GPIO->IN;
	memset(bounce, 0, sizeof(bounce));
	for (int i = 0; i < 4; i++) {
		bounce[i].level = (in >> bounce_pins[i]) & 1;
		bounce[i].quiet = 255;
	}
	NRF_TIMER1->TASKS_CAPTURE[1] = 1;
	NRF_TIMER1->CC[2] = (NRF_TIMER1->CC[1] + 1000) & 0xFFFF;
	NRF_TIMER1->EVENTS_COMPARE[2] = 0;
	NRF_TIMER1->INTENSET = TIMER_INTENSET_COMPARE2_Msk;
}

static void page_bounce_leave(void) { NRF_TIMER1->INTENCLR = TIMER_INTENCLR_COMPARE2_Msk; }

static void page_bounce_draw(void)
{
	con_printf("11 BUTTON BOUNCE");
	con_printf("    MX LS  MS  N");
	for (int i = 0; i < 4; i++)
		con_printf("%-3s%3u%3u%4u%3u", bounce_names[i], bounce[i].max_edges, bounce[i].last_edges,
		           bounce[i].max_span_ms, bounce[i].transitions % 1000);
	con_printf("");
	con_printf("MX/LS: MAX/LAST");
	con_printf("EDGES PER PRESS");
	con_printf("OR RELEASE. 1=OK");
	con_printf("MS: MAX FIRST TO");
	con_printf("LAST EDGE");
	con_printf("N: PRESS+RELEASE");
}

// ===========================================================================
// Page plumbing

struct probe_page {
	void (*draw)(void);
	void (*enter)(void);
	void (*leave)(void);
	void (*action)(void);        // M click (pages without custom keys)
	void (*button)(int but);     // custom keys: page owns UP/DOWN/M
};

static const struct probe_page pages[] = {
	{ page_chip_draw },
	{ page_ram_draw },
	{ page_timing_draw, page_timing_enter, NULL, NULL, page_timing_button },
	{ page_fds_draw, NULL, NULL, page_fds_action },
	{ page_motor_draw },
	{ page_speed_draw, NULL, page_speed_leave, NULL, page_speed_button },
	{ page_walk_draw, NULL, wa_stop, NULL, page_walk_button },
	{ page_dfu_draw, NULL, NULL, page_dfu_action },
	{ page_orient_draw, page_orient_enter, page_orient_leave, page_orient_action },
	{ page_ble_draw },
	{ page_bounce_draw, page_bounce_enter, page_bounce_leave },
};

struct probe_state { int page; };

static void probe_enter(void *it, bool pop)
{
	struct probe_state *s = it;
	if (!pop) {
		con_scroll = 0;
		con_total = 0;
		if (pages[s->page].enter)
			pages[s->page].enter();
	}
}

static void probe_leave(void *it, bool pop)
{
	struct probe_state *s = it;
	if (pop && pages[s->page].leave)
		pages[s->page].leave();
}

static void probe_idle(void *it)
{
	struct probe_state *s = it;
	con_line = 0;
	pages[s->page].draw();
	if (pages[s->page].draw != page_orient_draw)
		con_end();
}

static void probe_button(void *it, int but, int increment)
{
	struct probe_state *s = it;
	const struct probe_page *p = &pages[s->page];
	(void)increment;

	if (but & ONOFF_CLICK) {
		sstack_pop();
		return;
	}
	if (p->button) {
		p->button(but);
		return;
	}
	if (but & UP_CLICK)
		con_page(-1);
	if (but & DOWN_CLICK)
		con_page(1);
	if ((but & M_CLICK) && p->action)
		p->action();
}

static const struct stack_class probe_class = {
	sizeof(struct probe_state),
	.enter = probe_enter,
	.idle = probe_idle,
	.button = probe_button,
	.leave = probe_leave,
};

static void push_page(int idx)
{
	struct probe_state *s = sstack_alloc(&probe_class);
	s->page = idx;
	sstack_push();
}

#define PAGE_ACTION(n) static void open_page_##n(const struct configtree_t *ign) { (void)ign; push_page(n - 1); }
PAGE_ACTION(1) PAGE_ACTION(2) PAGE_ACTION(3) PAGE_ACTION(4) PAGE_ACTION(5) PAGE_ACTION(6)
PAGE_ACTION(7) PAGE_ACTION(8) PAGE_ACTION(9) PAGE_ACTION(10) PAGE_ACTION(11)

const struct scroller_config cfg_hw_probe = { 20, 58, 18, 0, 128, (const struct configtree_t[]) {
	{ "Chip ID", F_BUTTON, .action = open_page_1 },
	{ "RAM map", F_BUTTON, .action = open_page_2 },
	{ "Frame time", F_BUTTON, .action = open_page_3 },
	{ "Flash FDS", F_BUTTON, .action = open_page_4 },
	{ "Motor raw", F_BUTTON, .action = open_page_5 },
	{ "Speed limit", F_BUTTON, .action = open_page_6 },
	{ "Walk assist", F_BUTTON, .action = open_page_7 },
	{ "DFU entry", F_BUTTON, .action = open_page_8 },
	{ "Landscape", F_BUTTON, .action = open_page_9 },
	{ "BLE addr", F_BUTTON, .action = open_page_10 },
	{ "Btn bounce", F_BUTTON, .action = open_page_11 },
	{}
}};

#endif // HW_PROBE

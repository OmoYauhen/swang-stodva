#include "gfx.h"
#include "ui.h"
#include "lcd.h"
#include "state.h"

const
#include "sparkles.xbm"

DEFINE_IMAGE(sparkles);

extern const struct screen screen_main;

// Show the boot screen until the motor sends its first packet. The Bafang
// protocol has no boot handshake, so the transition is keyed off the first
// successfully parsed reply.
static void boot_idle()
{
	if (g_bafang.rx_count > 0) {
		showScreen(&screen_main);
		return;
	}

	fill_rect(0, 0, 64, 128, false);
	// image is 64px wide (full screen width); center it vertically on 128px
	img_draw(&img_sparkles, (64 - img_sparkles.w) / 2, (128 - img_sparkles.h) / 2);
	lcd_refresh();
}

const struct screen screen_boot = {
	.idle = boot_idle
};

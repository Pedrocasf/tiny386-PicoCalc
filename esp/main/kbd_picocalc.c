#ifdef KBD_I2C_SDA
/*
 * PicoCalc keyboard.
 *
 * The 67-key QWERTY matrix is scanned by an STM32 ("southbridge") that also
 * owns the two backlights, the battery gauge and the power button.  It speaks
 * I2C: write a register number, then read two bytes back.  Register 0x09 pops
 * one key event off its FIFO as [state, code], where state is 1 press, 2 hold
 * (auto-repeat) and 3 release.
 *
 * The catch is that "code" is the *resulting character*, not a scancode: the
 * MCU has already applied shift, caps and its own Sym layer, so pressing Sym+3
 * reports '#'.  A PC guest expects the opposite -- a scancode for the physical
 * key plus a shift state it applies itself.  So each character is mapped back
 * to the US-layout key that produces it, and shift is synthesised around the
 * keystroke when the character needs it and the user is not already holding
 * shift.  Caps Lock is deliberately never forwarded: the MCU has already
 * folded it into the character, and letting the guest track a second caps
 * state as well would cancel it back out.
 *
 * Modifier keys arrive as their own press/release events and are forwarded
 * directly.  Sym has no PC equivalent and is dropped, its effect having
 * already been applied by the MCU.
 */
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "common.h"
#include "../../i8042.h"

static const char *TAG = "kbd";

#ifndef KBD_I2C_PORT
#define KBD_I2C_PORT I2C_NUM_0
#endif
#ifndef KBD_I2C_HZ
/* The bus errors out at 100 kHz on this hardware; the stock firmware uses
 * 10-50 kHz. */
#define KBD_I2C_HZ 50000
#endif
#ifndef KBD_POLL_MS
#define KBD_POLL_MS 15
#endif

/* southbridge registers (| 0x80 to write) */
#define SB_REG_FIFO 0x09

/* event states */
#define SB_PRESS   1
#define SB_HOLD    2
#define SB_RELEASE 3

/* key codes that are not characters */
#define PK_BACKSPACE 0x08
#define PK_TAB       0x09
#define PK_ENTER     0x0a
#define PK_F1        0x81	/* .. 0x8a = F10 */
#define PK_F10ALT    0x90	/* some firmware revisions report F10 here */
#define PK_MOD_ALT   0xa1
#define PK_MOD_SHL   0xa2
#define PK_MOD_SHR   0xa3
#define PK_MOD_SYM   0xa4
#define PK_MOD_CTRL  0xa5
#define PK_ESC       0xb1
#define PK_LEFT      0xb4
#define PK_UP        0xb5
#define PK_DOWN      0xb6
#define PK_RIGHT     0xb7
#define PK_CAPS      0xc1
#define PK_BREAK     0xd0
#define PK_INS       0xd1
#define PK_HOME      0xd2
#define PK_DEL       0xd4
#define PK_END       0xd5
#define PK_PGUP      0xd6
#define PK_PGDN      0xd7

/* PS/2 set 1 scancodes, passed to ps2_put_keycode() as-is (it treats anything
 * below 96 as a raw set 1 code). */
#define SC_ESC       0x01
#define SC_BACKSPACE 0x0e
#define SC_TAB       0x0f
#define SC_ENTER     0x1c
#define SC_CTRL      0x1d
#define SC_LSHIFT    0x2a
#define SC_ALT       0x38
#define SC_SPACE     0x39
#define SC_F1        0x3b	/* .. 0x44 = F10 */
#define SC_SCROLL    0x46

/* Keys on the grey block send an e0-prefixed code.  ps2_put_keycode() takes
 * those as Linux input keycodes (96..127) and adds the prefix itself, along
 * with the delay some DOS software needs to re-read it. */
#define LK_HOME  102
#define LK_UP    103
#define LK_PGUP  104
#define LK_LEFT  105
#define LK_RIGHT 106
#define LK_END   107
#define LK_DOWN  108
#define LK_PGDN  109
#define LK_INS   110
#define LK_DEL   111

/*
 * Printable ASCII (0x20..0x7e) -> the US-layout key that produces it.
 * Low 7 bits are the set 1 scancode, 0x80 means the character needs shift.
 */
#define S 0x80
static const uint8_t ascii_to_sc[0x7f - 0x20 + 1] = {
	/* sp  !      "      #      $      %      &      '   */
	0x39, S|0x02, S|0x28, S|0x04, S|0x05, S|0x06, S|0x08, 0x28,
	/* (      )      *      +      ,     -     .     /   */
	S|0x0a, S|0x0b, S|0x09, S|0x0d, 0x33, 0x0c, 0x34, 0x35,
	/* 0     1     2     3     4     5     6     7     8     9   */
	0x0b, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
	/* :      ;     <      =     >      ?      @      */
	S|0x27, 0x27, S|0x33, 0x0d, S|0x34, S|0x35, S|0x03,
	/* A..M */
	S|0x1e, S|0x30, S|0x2e, S|0x20, S|0x12, S|0x21, S|0x22,
	S|0x23, S|0x17, S|0x24, S|0x25, S|0x26, S|0x32,
	/* N..Z */
	S|0x31, S|0x18, S|0x19, S|0x10, S|0x13, S|0x1f, S|0x14,
	S|0x16, S|0x2f, S|0x11, S|0x2d, S|0x15, S|0x2c,
	/* [     \     ]     ^      _      `   */
	0x1a, 0x2b, 0x1b, S|0x07, S|0x0c, 0x29,
	/* a..m */
	0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24,
	0x25, 0x26, 0x32,
	/* n..z */
	0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11,
	0x2d, 0x15, 0x2c,
	/* {      |      }      ~      del */
	S|0x1a, S|0x2b, S|0x1b, S|0x29, 0x00,
};
#undef S

static i2c_master_dev_handle_t s_dev;
static int s_shift_held;
static int s_boot_key;

/* 1 = got an event, 0 = FIFO empty, -1 = the MCU did not answer. */
static int read_event(uint8_t ev[2])
{
	uint8_t reg = SB_REG_FIFO;

	if (i2c_master_transmit(s_dev, &reg, 1, 100) != ESP_OK ||
	    i2c_master_receive(s_dev, ev, 2, 100) != ESP_OK)
		return -1;
	return ev[0] != 0 && ev[1] != 0;
}

#ifdef USB_MSC_KEY
#ifndef USB_MSC_KEY_PROBE_MS
/* Long enough for the MCU's auto-repeat to produce a second event while the
 * key is held, at the cost of delaying every normal boot by this much. */
#define USB_MSC_KEY_PROBE_MS 800
#endif
/*
 * Look for a key held during the first moment after start-up, before the
 * emulator exists, so it can pick a boot mode.
 *
 * The MCU is powered independently and keeps its FIFO across ESP resets, so
 * anything already queued may be minutes old -- draining it first is what makes
 * this "held now" rather than "pressed at some point".  A key that is actually
 * down keeps producing events, so it survives the drain.  Events for other keys
 * are dropped either way; there is nothing to type into yet.
 */
static void probe_boot_key(void)
{
	uint8_t ev[2];

	for (int i = 0; i < 64 && read_event(ev) == 1; i++)
		;

	for (int i = 0; i < USB_MSC_KEY_PROBE_MS / 10; i++) {
		while (read_event(ev) == 1) {
			if (ev[1] == USB_MSC_KEY &&
			    (ev[0] == SB_PRESS || ev[0] == SB_HOLD)) {
				ESP_LOGW(TAG, "boot key 0x%02x held",
					 USB_MSC_KEY);
				s_boot_key = 1;
				return;
			}
		}
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}
#endif

int kbd_picocalc_boot_key(void)
{
	return s_boot_key;
}

static void put(int is_down, int keycode)
{
	if (globals.kbd)
		ps2_put_keycode(globals.kbd, is_down, keycode);
}

/* Returns what to hand ps2_put_keycode() for a non-character key, or -1 to
 * ignore it: a raw set 1 scancode, or a Linux keycode (96..127) for the keys
 * that need the e0 prefix.  ps2_put_keycode() tells them apart by value. */
static int special_to_sc(uint8_t code)
{
	switch (code) {
	case PK_BACKSPACE: return SC_BACKSPACE;
	case PK_TAB:       return SC_TAB;
	case PK_ENTER:     return SC_ENTER;
	case '\r':         return SC_ENTER;
	case PK_ESC:       return SC_ESC;
	case PK_MOD_ALT:   return SC_ALT;
	case PK_MOD_SHL:
	case PK_MOD_SHR:   return SC_LSHIFT;
	case PK_MOD_CTRL:  return SC_CTRL;
	case PK_BREAK:     return SC_SCROLL;
	case PK_F10ALT:    return SC_F1 + 9;
	/* the MCU already applied Sym, and the guest has no caps state of its
	 * own here -- see the comment at the top */
	case PK_MOD_SYM:
	case PK_CAPS:      return -1;
	}

	if (code >= PK_F1 && code <= PK_F1 + 9)
		return SC_F1 + (code - PK_F1);

	switch (code) {
	case PK_UP:    return LK_UP;
	case PK_DOWN:  return LK_DOWN;
	case PK_LEFT:  return LK_LEFT;
	case PK_RIGHT: return LK_RIGHT;
	case PK_HOME:  return LK_HOME;
	case PK_END:   return LK_END;
	case PK_PGUP:  return LK_PGUP;
	case PK_PGDN:  return LK_PGDN;
	case PK_INS:   return LK_INS;
	case PK_DEL:   return LK_DEL;
	}
	return -1;
}

static void handle_event(uint8_t state, uint8_t code)
{
	int is_down = (state == SB_PRESS || state == SB_HOLD);
	int scancode;

	/* modifiers first: they carry state we need for the character below */
	if (code == PK_MOD_SHL || code == PK_MOD_SHR)
		s_shift_held = is_down;

	if (code >= 0x20 && code < 0x7f) {
		uint8_t e = ascii_to_sc[code - 0x20];
		int needs_shift = !!(e & 0x80);
		scancode = e & 0x7f;
		if (!scancode)
			return;

		/* Line up the guest's shift state with the character we were
		 * handed, then put it back.  Only around the make: by the time
		 * the key is released the guest has already taken the
		 * character. */
		if (is_down && needs_shift != s_shift_held) {
			put(needs_shift, SC_LSHIFT);
			put(is_down, scancode);
			put(s_shift_held, SC_LSHIFT);
		} else {
			put(is_down, scancode);
		}
		return;
	}

	scancode = special_to_sc(code);
	if (scancode >= 0)
		put(is_down, scancode);
}

static void kbd_task(void *arg)
{
	/* wait until the PC (and its 8042) exists */
	xEventGroupWaitBits(global_event_group,
			    BIT0,
			    pdFALSE,
			    pdFALSE,
			    portMAX_DELAY);
	ESP_LOGI(TAG, "polling southbridge at 0x%02x every %d ms",
		 KBD_I2C_ADDR, KBD_POLL_MS);

	int link = -1;
	for (;;) {
		uint8_t ev[2];
		int r = read_event(ev);
		int ok = r >= 0;

		if (ok != link) {
			/* a wiring or bus-speed problem shows up here */
			ESP_LOGW(TAG, "southbridge %s",
				 ok ? "responding" : "not responding");
			link = ok;
		}

		if (r == 1) {
#ifdef KBD_DEBUG
			ESP_LOGI(TAG, "state %d code 0x%02x", ev[0], ev[1]);
#endif
			handle_event(ev[0], ev[1]);
			/* something was queued, so come straight back for the
			 * next event instead of waiting out the poll period */
			continue;
		}
		vTaskDelay(pdMS_TO_TICKS(KBD_POLL_MS));
	}
}

void kbd_picocalc_main(void)
{
	i2c_master_bus_config_t bus_cfg = {
		.i2c_port = KBD_I2C_PORT,
		.sda_io_num = KBD_I2C_SDA,
		.scl_io_num = KBD_I2C_SCL,
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	i2c_master_bus_handle_t bus = NULL;
	if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
		ESP_LOGE(TAG, "cannot open I2C (sda=%d scl=%d)",
			 KBD_I2C_SDA, KBD_I2C_SCL);
		return;
	}

	i2c_device_config_t dev_cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = KBD_I2C_ADDR,
		.scl_speed_hz = KBD_I2C_HZ,
	};
	if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) {
		ESP_LOGE(TAG, "cannot add device 0x%02x", KBD_I2C_ADDR);
		return;
	}

#ifdef USE_USB_MSC
	/* before the polling task starts consuming events */
	probe_boot_key();
#endif
	xTaskCreatePinnedToCore(kbd_task, "kbd_task", 3072, NULL, 1, NULL, 0);
}
#endif

/*
 * ClockworkPi PicoCalc + ESP32-P4 in the Raspberry Pi Pico footprint.
 * Requires esp-idf v6.0.x (same as board_jc4880p443.h).
 *
 * STAGE 1: headless bring-up.  No PicoCalc peripheral is touched -- no LCD,
 * no keyboard, no SD card.  The emulator runs off the "storage" FAT partition
 * in flash (esp/flash_data/ is written there by `idf.py flash`) and everything
 * observable goes to the ESP console: SeaBIOS writes its log to port 0x402 and
 * pc.c turns that into putchar(), and with `enable_serial = 1` the emulated
 * COM1 lands there too.  See esp/main/lcd_stub.c for the fake panel.
 *
 * The PicoCalc side of the wiring, for stages 2+ (numbers are Raspberry Pi Pico
 * GPIOs, i.e. positions on the PicoCalc's Pico socket -- each must be
 * translated to whatever ESP32-P4 GPIO the board exposes at that position):
 *
 *   LCD (320x320, ILI9488/ST7365P, SPI):
 *       SCK 10, MOSI 11, MISO 12, CS 13, DC 14, RST 15
 *       accepts 0x3A=0x55 (RGB565), MADCTL 0x48, display inversion on
 *   SD card (SPI):  MISO 16, CS 17, SCK 18, MOSI 19, card detect 22
 *   Keyboard MCU:   I2C SDA 6, SCL 7, addr 0x1F, <= 50 kHz
 *   PWM audio:      left 26, right 27
 */
#define BUILD_ESP32

/*
 * Runs the interpreter benchmark instead of booting a guest: a fixed
 * instruction loop planted at the reset vector, reported as host cycles per
 * guest instruction, with the panel left idle so the memory system is quiet.
 * See cpu_bench() in esp_main.c.  Off by default -- it overwrites the reset
 * vector, so no guest can boot while it is on.
 */
//#define CPU_BENCH

/* Installs the UART0 driver.  Required whenever the ini sets enable_serial=1:
 * misc.c polls uart_get_buffered_data_len() for emulated COM1 input, which
 * spams "uart driver error" until the driver exists.  With it, typing into the
 * ESP console goes to the guest's COM1 -- the only input path until stage 3. */
#define ESPDEBUG

/*
 * Putting cpu_exec1() -- the interpreter's dispatch loop -- in internal RAM
 * would avoid running it from flash through the cache, but it does not fit:
 * the whole instruction set is inlined into that one function, 140180 bytes
 * of it, and the P4 has 768 KB of SRAM in total.  The linker gives up with
 * "Total discarded sections size is 89222 bytes" (63938 with L2 halved to
 * 256 KB, which frees SRAM at the cost of cache).  It would need the hot path
 * split out of the cold instruction bodies first.
 */
#define IRAM_ATTR_CPU_EXEC1

/*
 * Bump-allocator pool carved out of PSRAM, feeding exactly three things: guest
 * RAM (mem_size), VGA memory (vga_mem_size) and the 320x320x2 framebuffer.
 * 31 MB covers mem_size = 30M plus 512K of VGA memory and a 204K framebuffer,
 * with a little slack.
 *
 * The measured ceiling is the largest contiguous SPIRAM block, 33030144 bytes
 * on this board -- app_main prints it at boot.  Reserving this much leaves
 * barely 1 MB of PSRAM for anything else, which is fine while WiFi is unused
 * but is the first thing to lower if some later allocation starts failing.
 */
#define PSRAM_ALLOC_LEN (31 * 1024 * 1024)

#define BPP 16
#define FULL_UPDATE
/* 320x320 panel: SCALE_2_1 box-filters a 640x640 virtual canvas down, so a
 * 640x480 guest shows up as 320x240 centred.  9-dot text mode (720 px) does
 * not fit and would be dropped by vga.c, hence vga_force_8dm=1 in the ini. */
#define SCALE_2_1
/* Text mode bypasses the downscaler: 80x25 is drawn 1:1 with the 4x10 font in
 * font_small.h (320x250), instead of box-filtering 8x16 glyphs into a grey
 * 4x8 blur.  Modes too big for the panel still fall back to SCALE_2_1. */
#define SMALL_TEXT_FONT
/* RGB565 goes out MSB first on SPI, so vga.c stores it byte-swapped. */
#define SWAP_BYTEORDER_BPP16
#define LCD_WIDTH 320
#define LCD_HEIGHT 320

/* Stage 2: the real panel (esp/main/lcd_picocalc.c).
 * #define USE_LCD_STUB instead to go back to running headless. */
#define USE_LCD_PICOCALC
/* Colour bands for ~0.5 s before the emulator starts, so a wiring or
 * orientation problem is obvious.  Remove once the board is trusted. */
#define LCD_TEST_PATTERN

/* ESP32-P4 GPIOs at the PicoCalc panel's header positions.
 * MISO is GPIO8 on this board but stays out of the bus config: we only ever
 * write to the panel, and leaving it unclaimed keeps the pin free.
 * Note GPIO7/8 are the P4 boards' default I2C pins, so the stage 3 keyboard
 * I2C has to go somewhere else. */
#define LCD_PIN_SCLK 3
#define LCD_PIN_MOSI 2
#define LCD_PIN_CS 7
#define LCD_PIN_DC 24
#define LCD_PIN_RST 25

/* 80 MHz -> ~13 ms per full frame.  Lower to 40 or 25 MHz if pixels tear. */
#define LCD_PCLK_HZ (80 * 1000 * 1000)
#define LCD_SPI_HOST SPI2_HOST

/* Above the idle task, so the panel task actually runs when an SPI transfer
 * completes instead of round-robining with idle at priority 0. */
#define VGA_TASK_PRIO 2

/* No pacing sleep between slices -- it was costing ~29 ms of every frame.
 * The trade-off is coarser ST01 retrace timing for guests that poll it. */
#define REDRAW_SLICE_DELAY_US 0

/*
 * microSD in the ESP32-P4 board's own slot (not the PicoCalc's), 4-bit SDMMC.
 * These are the P4's standard SD pins and its internal LDO channel 4 for card
 * power -- the same values ESP-IDF's sdmmc example defaults to for esp32p4.
 *
 * USE_HOSTED_WIFI is required even though we never bring up WiFi: esp_hosted
 * starts from a system init hook, before main_task, and claims the one SDMMC
 * controller for its SDIO link to the WiFi 6 co-processor.  Initialising the
 * host again then fails with "no available sd host controller", so storage.c
 * has to reuse it (dummy init/deinit) and take slot 0 for the card.
 *
 * Careful: once the card mounts, storage.c deliberately skips mounting
 * /spiflash, so the card must carry tiny386.ini and the ROMs itself.  See
 * esp/tiny386_picocalc.ini.  With no card inserted it falls back to the flash
 * storage partition and esp/flash_data/ as before.
 */
#define SD_CLK 43
#define SD_CMD 44
#define SD_D0 39
#define SD_D1 40
#define SD_D2 41
#define SD_D3 42
#define SD_PWR_CTRL_LDO_IO_ID 4
#define USE_HOSTED_WIFI

/*
 * Keep /spiflash mounted even with a card present, so tiny386.ini can live in
 * the flash storage partition (esp/flash_data/, written by `idf.py flash`)
 * while the card carries only disk images.  The ROMs do not need this -- they
 * are read straight out of their own partitions, see the ini.
 *
 * An earlier attempt at this failed with ESP_ERR_NO_MEM, but that was with
 * three volumes mounted (both cards plus this); with one card it fits.  The
 * mount result is logged, so check the console if a config goes missing.
 *
 * Note esp_main.c still looks at the card first, so remove tiny386.ini from it
 * for the flash copy to take effect; the console prints which one was used.
 */
#define MOUNT_SPIFLASH_ALWAYS

/*
 * The PicoCalc has its own SD slot on the Pico header (SPI: SCK 46, MOSI 33,
 * MISO 48, CS 47, card detect 26 on this board) and it can be mounted next to
 * the SDMMC one, since they are different peripherals.  Left out for now --
 * note that a third volume is what pushed the /spiflash mount above out of
 * memory.
 */

/* Keyboard/backlight/battery MCU (esp/main/kbd_picocalc.c). */
#define KBD_I2C_SDA 50
#define KBD_I2C_SCL 49
#define KBD_I2C_ADDR 0x1f
/* Logs every raw key event on the console; drop once the mapping is trusted. */
#define KBD_DEBUG

/*
 * Hold F1 (key code 0x81) while powering on to export the microSD card to a
 * host PC as a USB drive instead of booting the emulator
 * (esp/main/usb_msc.c).  Needs CONFIG_TINYUSB_MSC_ENABLED in the sdkconfig,
 * and cannot coexist with the USB HID host (`enable_usb` in the ini), since
 * that is host mode on the same port.
 */
/*
 * USB disk mode: hold USB_MSC_KEY at power-on and the SD card is exported to a
 * host PC instead of the emulator starting.  Disabled here because this board
 * cannot use it: the Waveshare ESP32-P4-WIFI6 has a single USB socket and it
 * goes to a CH343 UART bridge, so the P4's own USB pins reach no host -- the
 * stack installs happily and then waits forever with no `host attached`.
 * Enabling it costs ~90 KB of flash for TinyUSB plus an 800 ms boot probe.
 * Re-enable on a board whose USB-C reaches the SoC; the code is unchanged, but
 * the espressif/esp_tinyusb dependency has to go back into idf_component.yml.
 */
//#define USE_USB_MSC
#define USB_MSC_KEY 0x81
/* Which USB socket the board wires to the P4: the default is the high-speed
 * OTG pins; define USB_MSC_FULL_SPEED for the full-speed (USB-Serial-JTAG)
 * pins instead.  Neither produced a `host attached` event on the board tested,
 * which pointed at the cabling rather than the port choice. */
//#define USB_MSC_FULL_SPEED

/*
 * Sound: 1-bit PDM per channel into the PicoCalc's RC filter and amplifier
 * (esp/main/i2s.c).  These are the P4 GPIOs at the header positions the
 * PicoCalc wires audio to (Pico GP26 left / GP27 right).
 */
#define AUDIO_PDM_DOUT 21	/* left */
#define AUDIO_PDM_DOUT2 22	/* right */
/* Guest audio is attenuated by this before it goes out; raise for more volume,
 * i.e. lower the divisor. */
#define MIXER_VOLUME_DIV 16
#define MIXER_BUF_LEN 512

/* No SD (SD_CLK / SD_SPI_MOSI undefined) -> storage.c mounts /spiflash.
 * No mouse: the PicoCalc has no pointer, so Win9x/NT still need either
 * keyboard-driven mouse emulation or a USB HID device (usb_input.c). */

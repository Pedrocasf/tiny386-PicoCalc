#ifdef USE_LCD_PICOCALC
/*
 * ClockworkPi PicoCalc panel: 320x320 IPS, ILI9488 or ST7365P, 4-wire SPI, BGR.
 *
 * The panel accepts COLMOD 0x55 (RGB565), so we do not pay ILI9488's usual
 * 18-bit/3-byte SPI penalty: a full frame is 320*320*2 = 200 KB.  RGB565 goes
 * out MSB first, which is why the board header sets SWAP_BYTEORDER_BPP16 and
 * lets vga.c store the framebuffer byte-swapped.
 *
 * We drive esp_lcd's SPI panel IO directly instead of an esp_lcd panel driver:
 * the only entry point tiny386 needs is lcd_draw(), which is CASET/RASET/RAMWR.
 *
 * Backlight is not a GPIO on this board -- it hangs off the keyboard MCU
 * (register 0x05 over I2C), which stage 3 owns.  The MCU lights the panel by
 * itself at power-on, so nothing here needs to touch it.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "common.h"

static const char *TAG = "lcd";

#ifndef LCD_SPI_HOST
#define LCD_SPI_HOST SPI2_HOST
#endif
#ifndef LCD_PCLK_HZ
/* The stock PicoCalc firmware runs 25 MHz and documents 50 MHz as workable.
 * Drop this to 25 MHz if the panel shows torn or shifted pixels. */
#define LCD_PCLK_HZ (40 * 1000 * 1000)
#endif

/* redraw() in esp_main.c hands us one NN-th of the frame at a time (NN == 32). */
#define SLICE_ROWS  (LCD_WIDTH / 32)
#define SLICE_BYTES (LCD_WIDTH * SLICE_ROWS * 2)

#define LCD_CMD_SWRESET 0x01
#define LCD_CMD_SLPOUT  0x11
#define LCD_CMD_INVON   0x21
#define LCD_CMD_DISPOFF 0x28
#define LCD_CMD_DISPON  0x29
#define LCD_CMD_CASET   0x2a
#define LCD_CMD_RASET   0x2b
#define LCD_CMD_RAMWR   0x2c
#define LCD_CMD_MADCTL  0x36
#define LCD_CMD_COLMOD  0x3a

#define MADCTL_BGR 0x08
#define MADCTL_MX  0x40

typedef struct {
	uint8_t cmd;
	uint8_t data[16];
	uint8_t len;
	uint16_t delay_ms;
} lcd_init_cmd_t;

/* ILI9488 vendor-recommended values; the ST7365P used on newer PicoCalc units
 * takes the same sequence. */
static const lcd_init_cmd_t init_cmds[] = {
	{LCD_CMD_SWRESET, {0}, 0, 120},
	{LCD_CMD_DISPOFF, {0}, 0, 0},
	/* positive/negative gamma */
	{0xe0, {0x00, 0x03, 0x09, 0x08, 0x16, 0x0a, 0x3f, 0x78,
		0x4c, 0x09, 0x0a, 0x08, 0x16, 0x1a, 0x0f}, 15, 0},
	{0xe1, {0x00, 0x16, 0x19, 0x03, 0x0f, 0x05, 0x32, 0x45,
		0x46, 0x04, 0x0e, 0x0d, 0x35, 0x37, 0x0f}, 15, 0},
	{0xc0, {0x17, 0x15}, 2, 0},		/* power control 1 */
	{0xc1, {0x41}, 1, 0},			/* power control 2 */
	{0xc5, {0x00, 0x12, 0x80}, 3, 0},	/* VCOM control */
	{LCD_CMD_MADCTL, {MADCTL_MX | MADCTL_BGR}, 1, 0},
	{LCD_CMD_COLMOD, {0x55}, 1, 0},		/* 16 bpp, RGB565 */
	{0xb0, {0x00}, 1, 0},			/* interface mode control */
	{0xb1, {0xa0}, 1, 0},			/* frame rate control */
	{LCD_CMD_INVON, {0}, 0, 0},
	{0xb4, {0x02}, 1, 0},			/* display inversion control */
	{0xb6, {0x02, 0x02, 0x3b}, 3, 0},	/* display function control */
	{0xb7, {0xc6}, 1, 0},			/* entry mode set */
	{0xe9, {0x00}, 1, 0},			/* set image function */
	{0xf7, {0xa9, 0x51, 0x2c, 0x82}, 4, 0},	/* adjust control 3 */
	{LCD_CMD_SLPOUT, {0}, 0, 120},
	{LCD_CMD_DISPON, {0}, 0, 120},
};

static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_tx_done;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
				esp_lcd_panel_io_event_data_t *edata,
				void *user_ctx)
{
	BaseType_t woken = pdFALSE;
	xSemaphoreGiveFromISR(s_tx_done, &woken);
	/* esp_lcd throws our return value away (lcd_spi_post_trans_color_cb),
	 * so ask for the context switch here.  Without it the waiting task
	 * sleeps until the next scheduler tick -- 10 ms at CONFIG_FREERTOS_HZ
	 * 100, for a transfer that takes well under 1 ms. */
	if (woken == pdTRUE)
		portYIELD_FROM_ISR();
	return woken == pdTRUE;
}

/* Counts frames, not slices: redraw() only starts one slice per frame at y 0.
 * Reported from the task loop, so silence means the task is stuck, not idle. */
static unsigned s_frames;
/* Where the time goes: total inside lcd_draw, and the part of it spent waiting
 * for the SPI transfer.  The rest of a refresh is rendering plus redraw()'s
 * memcpy and its 900 us sleep per slice. */
static int64_t s_draw_us;
static int64_t s_spi_us;

void lcd_draw(int x_start, int y_start, int x_end, int y_end, void *src)
{
	uint8_t param[4];

	if (!s_io)
		return;

	if (y_start == 0)
		s_frames++;

	int64_t t0 = esp_timer_get_time();

	param[0] = x_start >> 8;
	param[1] = x_start;
	param[2] = (x_end - 1) >> 8;
	param[3] = (x_end - 1);
	ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, LCD_CMD_CASET, param, 4));

	param[0] = y_start >> 8;
	param[1] = y_start;
	param[2] = (y_end - 1) >> 8;
	param[3] = (y_end - 1);
	ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, LCD_CMD_RASET, param, 4));

	size_t len = (size_t) (x_end - x_start) * (y_end - y_start) * 2;
	int64_t t1 = esp_timer_get_time();
	ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(s_io, LCD_CMD_RAMWR, src, len));

	/* The caller reuses its slice buffer the moment we return, so the
	 * transfer has to be finished by then. */
	xSemaphoreTake(s_tx_done, portMAX_DELAY);

	int64_t t2 = esp_timer_get_time();
	s_spi_us += t2 - t1;
	s_draw_us += t2 - t0;
}

/* Paint the whole panel one slice at a time out of a DMA scratch buffer.
 * With bands != NULL the buffer is filled with a colour per horizontal band,
 * which tells wiring/orientation problems apart from emulator problems. */
static void lcd_fill(const uint16_t *bands, int nbands)
{
	uint16_t *buf = heap_caps_malloc(SLICE_BYTES, MALLOC_CAP_DMA);
	if (!buf) {
		ESP_LOGE(TAG, "no DMA memory for %d bytes", SLICE_BYTES);
		return;
	}

	for (int i = 0; i < 32; i++) {
		int y = i * SLICE_ROWS;
		for (int row = 0; row < SLICE_ROWS; row++) {
			int band = nbands ? (y + row) * nbands / LCD_HEIGHT : 0;
			uint16_t c = bands ? bands[band] : 0;
			for (int x = 0; x < LCD_WIDTH; x++)
				buf[row * LCD_WIDTH + x] = c;
		}
		lcd_draw(0, y, LCD_WIDTH, y + SLICE_ROWS, buf);
	}

	free(buf);
}

void pc_vga_step(void *o);

void vga_task(void *arg)
{
	fprintf(stderr, "vga runs on core %d\n", esp_cpu_get_core_id());

#ifdef PICOCALC_PINS_PROVISIONAL
	ESP_LOGW(TAG, "LCD pin numbers are placeholders "
		      "(see esp/main/board_picocalc.h)");
#endif

	s_tx_done = xSemaphoreCreateBinary();
	assert(s_tx_done);

	ESP_LOGI(TAG, "reset panel (rst=%d)", LCD_PIN_RST);
	gpio_config_t rst_cfg = {
		.mode = GPIO_MODE_OUTPUT,
		.pin_bit_mask = 1ULL << LCD_PIN_RST,
	};
	ESP_ERROR_CHECK(gpio_config(&rst_cfg));
	gpio_set_level(LCD_PIN_RST, 1);
	vTaskDelay(pdMS_TO_TICKS(10));
	gpio_set_level(LCD_PIN_RST, 0);
	vTaskDelay(pdMS_TO_TICKS(10));
	gpio_set_level(LCD_PIN_RST, 1);
	vTaskDelay(pdMS_TO_TICKS(200));

	ESP_LOGI(TAG, "init SPI bus (sclk=%d mosi=%d) at %d MHz",
		 LCD_PIN_SCLK, LCD_PIN_MOSI, LCD_PCLK_HZ / 1000000);
	const spi_bus_config_t bus_cfg = {
		.sclk_io_num = LCD_PIN_SCLK,
		.mosi_io_num = LCD_PIN_MOSI,
		.miso_io_num = -1,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = SLICE_BYTES,
	};
	ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

	const esp_lcd_panel_io_spi_config_t io_cfg = {
		.cs_gpio_num = LCD_PIN_CS,
		.dc_gpio_num = LCD_PIN_DC,
		.spi_mode = 0,
		.pclk_hz = LCD_PCLK_HZ,
		.trans_queue_depth = 1,
		.on_color_trans_done = on_color_trans_done,
		.lcd_cmd_bits = 8,
		.lcd_param_bits = 8,
	};
	esp_lcd_panel_io_handle_t io = NULL;
	ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
				(esp_lcd_spi_bus_handle_t) LCD_SPI_HOST,
				&io_cfg, &io));

	for (int i = 0; i < sizeof(init_cmds) / sizeof(init_cmds[0]); i++) {
		const lcd_init_cmd_t *c = &init_cmds[i];
		ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(
					io, c->cmd,
					c->len ? c->data : NULL, c->len));
		if (c->delay_ms)
			vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
	}
	s_io = io;
	globals.panel = io;

#ifdef LCD_TEST_PATTERN
	/* Byte-swapped RGB565 (SWAP_BYTEORDER_BPP16), so these read as
	 * red, green, blue, white on a correctly wired BGR panel. */
	static const uint16_t bands[] = {0x00f8, 0xe007, 0x1f00, 0xffff};
	ESP_LOGI(TAG, "test pattern");
	lcd_fill(bands, sizeof(bands) / sizeof(bands[0]));
	vTaskDelay(pdMS_TO_TICKS(500));
#endif
	lcd_fill(NULL, 0);

	/* Panel is up: let i386_task build the PC, then wait for it to exist. */
	xEventGroupSetBits(global_event_group, BIT1);
	xEventGroupWaitBits(global_event_group,
			    BIT0,
			    pdFALSE,
			    pdFALSE,
			    portMAX_DELAY);

	int64_t mark = esp_timer_get_time();
	unsigned mark_frames = s_frames;
	int64_t step_us = 0, mark_draw = 0, mark_spi = 0;
	while (1) {
		int64_t t0 = esp_timer_get_time();
		pc_vga_step(globals.pc);
		int64_t now = esp_timer_get_time();
		step_us += now - t0;
		vTaskDelay(10 / portTICK_PERIOD_MS);

		now = esp_timer_get_time();
		if (now - mark >= 5000000) {
			ESP_LOGI(TAG, "%u frames in %lld ms: refresh %lld, "
				      "blit %lld (spi %lld)",
				 s_frames - mark_frames, (now - mark) / 1000,
				 step_us / 1000, (s_draw_us - mark_draw) / 1000,
				 (s_spi_us - mark_spi) / 1000);
			mark = now;
			mark_frames = s_frames;
			mark_draw = s_draw_us;
			mark_spi = s_spi_us;
			step_us = 0;
		}
	}
}
#endif

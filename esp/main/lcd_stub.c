#ifdef USE_LCD_STUB
/*
 * A panel that isn't there.
 *
 * Provides the same vga_task()/lcd_draw() contract as the real panel drivers,
 * but throws the pixels away and reports the frame rate on the console.  Used
 * to bring up a new board before its display driver exists: the cpu, memory,
 * VGA, storage and timing paths all run for real.
 */
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "common.h"

static const char *TAG = "lcd";

/* redraw() in esp_main.c splits every frame into NN slices; only the first one
 * starts at y == 0, so counting those counts frames. */
#define REPORT_FRAMES 32

static int frames;
static int64_t last_us;

void lcd_draw(int x_start, int y_start, int x_end, int y_end, void *src)
{
	if (y_start != 0)
		return;
	if (++frames < REPORT_FRAMES)
		return;

	int64_t now = esp_timer_get_time();
	ESP_LOGI(TAG, "no panel: %d frames in %lld ms (%.1f fps)",
		 frames, (now - last_us) / 1000,
		 frames * 1000000.0 / (double) (now - last_us));
	last_us = now;
	frames = 0;
}

void pc_vga_step(void *o);

void vga_task(void *arg)
{
	fprintf(stderr, "vga runs on core %d\n", esp_cpu_get_core_id());
	ESP_LOGW(TAG, "USE_LCD_STUB: headless, nothing is displayed");

	globals.panel = NULL;
	last_us = esp_timer_get_time();

	/* Same handshake as the real drivers: tell i386_task the "panel" is up,
	 * then wait until the PC exists before stepping the VGA. */
	xEventGroupSetBits(global_event_group, BIT1);
	xEventGroupWaitBits(global_event_group,
			    BIT0,
			    pdFALSE,
			    pdFALSE,
			    portMAX_DELAY);

	while (1) {
		pc_vga_step(globals.pc);
		vTaskDelay(10 / portTICK_PERIOD_MS);
	}
}
#endif

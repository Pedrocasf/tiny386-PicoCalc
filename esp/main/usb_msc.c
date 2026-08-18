#ifdef USE_USB_MSC
/*
 * USB disk mode: hand the microSD card to a host PC as a mass-storage device.
 *
 * This is a boot mode, not a background service.  The emulator keeps its disk
 * image open and cached, so letting a host PC write sectors underneath it would
 * corrupt both sides; entering this mode therefore skips starting the emulator
 * entirely.  Hold USB_MSC_KEY while powering on to get here (see
 * kbd_picocalc.c, which probes for it before the emulator starts).
 *
 * The card handle comes from storage.c, which has already initialised it -- the
 * SDMMC controller is shared with esp_hosted and must not be set up twice.  The
 * local /sdcard mount is dropped first so that FatFs on this side cannot fight
 * the host's writes.
 *
 * Note this is USB *device* mode and cannot share the port with the USB HID
 * host in usb_input.c.
 */
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

#include "common.h"

static const char *TAG = "usbmsc";

extern void *rawsd;	/* sdmmc_card_t *, set by storage.c */

/* Device-level USB events.  These are the ones that answer "is the host even
 * talking to us": nothing here at all means the cable is not in the P4's OTG
 * port (the console bridge is a different connector) or the port is not wired
 * to the USB PHY. */
static void usb_event(tinyusb_event_t *event, void *arg)
{
	switch (event->id) {
	case TINYUSB_EVENT_ATTACHED:
		ESP_LOGI(TAG, "host attached");
		break;
	case TINYUSB_EVENT_DETACHED:
		ESP_LOGI(TAG, "host detached");
		break;
	default:
		ESP_LOGI(TAG, "usb event %d", (int) event->id);
		break;
	}
}

static void msc_event(tinyusb_msc_storage_handle_t handle,
		      tinyusb_msc_event_t *event, void *arg)
{
	switch (event->id) {
	case TINYUSB_MSC_EVENT_MOUNT_COMPLETE:
		ESP_LOGI(TAG, "card now owned by %s",
			 event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB ?
			 "the USB host" : "us");
		break;
	case TINYUSB_MSC_EVENT_MOUNT_FAILED:
		ESP_LOGE(TAG, "mount failed");
		break;
	case TINYUSB_MSC_EVENT_FORMAT_REQUIRED:
		ESP_LOGW(TAG, "card has no filesystem (not formatting it)");
		break;
	default:
		break;
	}
}

void usb_msc_main(void)
{
	if (!rawsd) {
		ESP_LOGE(TAG, "no SD card to export");
		return;
	}

	/* Hand the card over cleanly: drop our FAT mount before the host gets
	 * block-level access to it. */
	esp_vfs_fat_sdcard_unmount("/sdcard", rawsd);

	const tinyusb_msc_driver_config_t drv_cfg = {
		.callback = msc_event,
	};
	ESP_ERROR_CHECK(tinyusb_msc_install_driver(&drv_cfg));

	const tinyusb_msc_storage_config_t storage_cfg = {
		.medium.card = rawsd,
		.fat_fs = {
			.base_path = NULL,
			/* never reformat the user's card */
			.do_not_format = true,
		},
		.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
	};
	ESP_ERROR_CHECK(tinyusb_msc_new_storage_sdmmc(&storage_cfg, NULL));

	/*
	 * The P4 has two USB peripherals: the high-speed OTG on its own
	 * USB2_DP/DM pins, and a full-speed one sharing pins with
	 * USB-Serial-JTAG.  Which of a board's USB-C sockets goes where is a
	 * board decision, so make it selectable: no `host attached` event
	 * usually means TinyUSB is driving the socket you did not plug into.
	 */
#ifdef USB_MSC_FULL_SPEED
	tinyusb_config_t tusb_cfg = TINYUSB_CONFIG_FULL_SPEED(usb_event, NULL);
	ESP_LOGI(TAG, "using the full-speed port");
#else
	tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(usb_event);
	ESP_LOGI(TAG, "using the high-speed port");
#endif
	ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

	ESP_LOGW(TAG, "USB disk mode: SD card exported, emulator not started");

	/* Heartbeat, so the console makes it obvious which mode we are in --
	 * otherwise disk mode is indistinguishable from a hung board. */
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(5000));
		ESP_LOGI(TAG, "USB disk mode, waiting for a host");
	}
}
#endif

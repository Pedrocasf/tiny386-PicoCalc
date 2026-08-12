#include <unistd.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#ifdef AUDIO_PDM_DOUT
#include "driver/i2s_pdm.h"
#endif
#include "common.h"

static i2s_chan_handle_t                tx_chan;        // I2S tx channel handler
void mixer_callback (void *opaque, uint8_t *stream, int free);

#ifndef MIXER_BUF_LEN
#define MIXER_BUF_LEN 128
#endif

/* Output attenuation.  The codec boards need a lot of it; boards driving an
 * amplifier directly may want less. */
#ifndef MIXER_VOLUME_DIV
#define MIXER_VOLUME_DIV 16
#endif

static void i2s_task(void *arg)
{
	int16_t buf[MIXER_BUF_LEN];
	int core_id = esp_cpu_get_core_id();
	fprintf(stderr, "i2s runs on core %d\n", core_id);

	xEventGroupWaitBits(global_event_group,
			    BIT0,
			    pdFALSE,
			    pdFALSE,
			    portMAX_DELAY);

	i2s_channel_enable(tx_chan);
	for (;;) {
		size_t bwritten;
		memset(buf, 0, MIXER_BUF_LEN * 2);
		mixer_callback(globals.pc, (uint8_t *) buf, MIXER_BUF_LEN * 2);
		for (int i = 0; i < MIXER_BUF_LEN; i++) {
			buf[i] = buf[i] / MIXER_VOLUME_DIV;
		}
		i2s_channel_write(tx_chan, buf, MIXER_BUF_LEN * 2, &bwritten, portMAX_DELAY);
	}
	i2s_channel_disable(tx_chan);
}

#ifndef I2S_NUM
#define I2S_NUM I2S_NUM_AUTO
#endif

#ifdef USE_ES8311
// adapted from: examples/peripherals/i2s/i2s_codec/i2s_es8311/main/i2s_es8311_example.c
static i2s_chan_handle_t                rx_chan;        // I2S rx channel handler (not used)
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_vol.h"
static const char *TAG = "i2s";

static esp_err_t es8311_codec_init(void)
{
	/* Initialize I2C peripheral */
	i2c_master_bus_handle_t i2c_bus_handle = NULL;
	i2c_master_bus_config_t i2c_mst_cfg = {
		.i2c_port = ES8311_I2C_NUM,
		.sda_io_num = ES8311_I2C_SDA,
		.scl_io_num = ES8311_I2C_SCL,
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		/* Pull-up internally for no external pull-up case.
		   Suggest to use external pull-up to ensure a strong enough pull-up. */
		.flags.enable_internal_pullup = true,
	};
	ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_cfg, &i2c_bus_handle));

	/* Create control interface with I2C bus handle */
	audio_codec_i2c_cfg_t i2c_cfg = {
		.port = ES8311_I2C_NUM,
		.addr = ES8311_CODEC_DEFAULT_ADDR,
		.bus_handle = i2c_bus_handle,
	};
	const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
	assert(ctrl_if);

	/* Create data interface with I2S bus handle */
	audio_codec_i2s_cfg_t i2s_cfg = {
		.port = I2S_NUM,
		.rx_handle = rx_chan,
		.tx_handle = tx_chan,
	};
	const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
	assert(data_if);

	/* Create ES8311 interface handle */
	const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
	assert(gpio_if);
	es8311_codec_cfg_t es8311_cfg = {
		.ctrl_if = ctrl_if,
		.gpio_if = gpio_if,
		.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
		.master_mode = false,
		.use_mclk = I2S_MCLK >= 0,
		.pa_pin = ES8311_PA,
		.pa_reverted = false,
		.hw_gain = {
			.pa_voltage = 5.0,
			.codec_dac_voltage = 3.3,
		},
		//.mclk_div = EXAMPLE_MCLK_MULTIPLE,
	};
	const audio_codec_if_t *es8311_if = es8311_codec_new(&es8311_cfg);
	assert(es8311_if);

	/* Create the top codec handle with ES8311 interface handle and data interface */
	esp_codec_dev_cfg_t dev_cfg = {
		.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
		.codec_if = es8311_if,
		.data_if = data_if,
	};
	esp_codec_dev_handle_t codec_handle = esp_codec_dev_new(&dev_cfg);
	assert(codec_handle);

	/* Specify the sample configurations and open the device */
	esp_codec_dev_sample_info_t sample_cfg = {
		.bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
		.channel = 2,
		.channel_mask = 0x03,
		.sample_rate = 44100,
	};
	if (esp_codec_dev_open(codec_handle, &sample_cfg) != ESP_CODEC_DEV_OK) {
		ESP_LOGE(TAG, "Open codec device failed");
		return ESP_FAIL;
	}

	/* Set the initial volume and gain */
	if (esp_codec_dev_set_out_vol(codec_handle, 80) != ESP_CODEC_DEV_OK) {
		ESP_LOGE(TAG, "set output volume failed");
		return ESP_FAIL;
	}
	return ESP_OK;
}
#endif

void i2s_main()
{
#ifdef AUDIO_PDM_DOUT
	/*
	 * PicoCalc-style analog output: no codec, the pins feed an RC filter and
	 * then the amplifier.  The ESP32-P4 has no DAC, but its I2S converts PCM
	 * to a 1-bit PDM stream in hardware (SOC_I2S_SUPPORTS_PCM2PDM) and can
	 * drive one line per channel, so the mixer keeps writing ordinary 16-bit
	 * stereo PCM and DMA does the rest -- no per-sample interrupt.
	 * Two-line DAC mode puts the left channel on dout and the right on
	 * dout2; the stereo DAC slot macro selects it, along with a 35.5 Hz
	 * high-pass and the dithering that keeps the noise floor sane.
	 * PDM TX must live on I2S0: it is the only unit with two TX lines.
	 */
	i2s_chan_config_t tx_chan_cfg =
		I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
	ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_chan, NULL));

	i2s_pdm_tx_config_t pdm_cfg = {
		.clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(44100),
		.slot_cfg = I2S_PDM_TX_SLOT_DAC_DEFAULT_CONFIG(
				I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
		.gpio_cfg = {
			.clk = I2S_GPIO_UNUSED,	/* nothing external is clocked */
			.dout = AUDIO_PDM_DOUT,
			.dout2 = AUDIO_PDM_DOUT2,
		},
	};
	/*
	 * The PDM clock has to run at 64 * oversample * fs, times a minimum
	 * bclk_div of 8, and the driver demands roughly twice that from the
	 * source clock.  For 44.1 kHz stereo that is ~90 MHz, which XTAL (40 MHz)
	 * cannot provide -- and I2S_CLK_SRC_PLL_160M only exists on P4 hw_ver3,
	 * so I2S_CLK_SRC_DEFAULT lands on XTAL here and init fails with "sample
	 * rate is too large".  APLL reaches 125 MHz and the driver programs it to
	 * fit, which keeps the mixer at its native 44.1 kHz.
	 */
	pdm_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;

	ESP_ERROR_CHECK(i2s_channel_init_pdm_tx_mode(tx_chan, &pdm_cfg));
	xTaskCreatePinnedToCore(i2s_task, "i2s_task", 4096, NULL, 0, NULL, 0);
#elif defined(I2S_MCLK)
	/* Setp 1: Determine the I2S channel configuration and allocate two channels one by one
	 * The default configuration can be generated by the helper macro,
	 * it only requires the I2S controller id and I2S role
	 * The tx and rx channels here are registered on different I2S controller,
	 * Except ESP32 and ESP32-S2, others allow to register two separate tx & rx channels on a same controller */
	i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
#ifdef USE_ES8311
	ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_chan, &rx_chan));
#else
	ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_chan, NULL));
#endif
	/* Step 2: Setting the configurations of standard mode and initialize each channels one by one
	 * The slot configuration and clock configuration can be generated by the macros
	 * These two helper macros is defined in 'i2s_std.h' which can only be used in STD mode.
	 * They can help to specify the slot and clock configurations for initialization or re-configuring */
	i2s_std_config_t tx_std_cfg = {
		.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
		.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
		.gpio_cfg = {
			.mclk = I2S_MCLK,
			.bclk = I2S_BCLK,
			.ws   = I2S_WS,
			.dout = I2S_DOUT,
			.din  = -1,
			.invert_flags = {
				.mclk_inv = false,
				.bclk_inv = false,
				.ws_inv   = false,
			},
		},
	};
	ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &tx_std_cfg));
#ifdef USE_ES8311
	ESP_ERROR_CHECK(es8311_codec_init());
#endif
	xTaskCreatePinnedToCore(i2s_task, "i2s_task", 4096, NULL, 0, NULL, 0);
#endif
}

#include <stdio.h>

#include "alert_audio.h"
#include "bsp/esp-bsp.h"
#include "ui.h"

void app_main(void)
{
	bsp_display_cfg_t cfg = {
		.lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
		.buffer_size = BSP_LCD_H_RES * 120,
		.double_buffer = true,
		.flags = {
			.buff_dma = false,
			.buff_spiram = true,
			.sw_rotate = false,
		},
	};

	lv_display_t *disp = bsp_display_start_with_config(&cfg);
	if (disp == NULL) {
		return;
	}

	bsp_display_backlight_on();
	bsp_display_lock(0);
	nurse_audio_init();
	ui_init();
	bsp_display_unlock();
}

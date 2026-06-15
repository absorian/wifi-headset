/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "common.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>

static const char *TAG = "appPRPH";

#define PERIPH_BTN_PRESSED_LEVEL 1
#define PERIPH_BTN_DEBOUNCE_MS 150
#define PERIPH_BTN_LONG_CLICK_MS 1000

#define PERIPH_MAIN_BTN_GPIO GPIO_NUM_4
#define PERIPH_STATUS_LED_GPIO GPIO_NUM_9

#define PERIPH_STATUS_LED_FLASH_INTV_MS 1000
#define PERIPH_STATUS_LED_BLINK_INTV_MS 500

#define PERIPH_WAKEUP_BIT (1 << 0)

typedef struct periph_btn_ctx periph_btn_ctx_t;

typedef void (*periph_btn_cb_t)(periph_btn_ctx_t *);

struct periph_btn_ctx {
	int last_level;
	int64_t debounce_tm;
	int64_t press_tm;

	bool long_press;
	bool long_click;
	int click_count;

	gpio_num_t io;
	periph_btn_cb_t cb;
	void *cb_ctx;
};

static TaskHandle_t s_task;

static periph_btn_ctx_t s_main_btn;
static atomic_int s_led_mode;
static EventGroupHandle_t s_wakeup_evt;

static void periph_btn_tick(periph_btn_ctx_t *ctx, uint32_t tick_tm)
{
	int level = gpio_get_level(ctx->io);

	if (level != ctx->last_level) {
		if (level == PERIPH_BTN_PRESSED_LEVEL &&
		    ctx->debounce_tm >= PERIPH_BTN_DEBOUNCE_MS) {
			ctx->press_tm = 0;
			ctx->last_level = level;
		}

		if (level != PERIPH_BTN_PRESSED_LEVEL) {
			ctx->click_count++;

			ctx->debounce_tm = 0;
			ctx->last_level = level;
		}
	} else if (level == PERIPH_BTN_PRESSED_LEVEL) {
		ctx->press_tm += tick_tm;
		if (!ctx->long_press &&
		    ctx->press_tm >= PERIPH_BTN_LONG_CLICK_MS) {
			ctx->long_press = true;
			ctx->cb(ctx);
		}
	}

	if (level != PERIPH_BTN_PRESSED_LEVEL &&
	    ctx->debounce_tm >= 2 * PERIPH_BTN_DEBOUNCE_MS) {
		ctx->long_click = ctx->long_press;
		ctx->long_press = false;
		if (ctx->click_count) {
			ctx->cb(ctx);
		}
		ctx->click_count = 0;
		ctx->long_click = false;
	}

	ctx->debounce_tm += tick_tm;
}

static void periph_status_led_tick(uint32_t tick_tm)
{
	static int64_t tm;
	static int level;

	switch (s_led_mode) {
	case STATUS_LED_BLINK:
		if (tm <= PERIPH_STATUS_LED_BLINK_INTV_MS)
			break;
		tm = 0;
		gpio_set_level(PERIPH_STATUS_LED_GPIO, level);
		level = !level;
		break;
	case STATUS_LED_FLASH:
		if (level == 0 && tm > PERIPH_STATUS_LED_FLASH_INTV_MS) {
			tm = 0;
			gpio_set_level(PERIPH_STATUS_LED_GPIO, 1);
			level++;
		} else if (level == 1 && tm > 20) {
			tm = 0;
			gpio_set_level(PERIPH_STATUS_LED_GPIO, 0);
			level = 0;
		}
		break;
	default:
		break;
	}
	tm += tick_tm;
}

static void periph_task(void *arg)
{
	const uint32_t tick_tm = 10;
	while (1) {
		periph_btn_tick(&s_main_btn, tick_tm);
		periph_status_led_tick(tick_tm);
		vTaskDelay(pdMS_TO_TICKS(tick_tm));
	}
}

static void periph_main_btn_handler(periph_btn_ctx_t *ctx)
{
	// ESP_LOGI(TAG, "main btn clicks=%d long_press=%d, long_click=%d",
	//     ctx->click_count, ctx->long_press, ctx->long_click);
	if (ctx->long_press && !ctx->click_count) {
		// just a long press
		xEventGroupSetBits(s_wakeup_evt, PERIPH_WAKEUP_BIT);
	}
	if (ctx->long_click) {
		switch (ctx->click_count) {
		case 1:
			// one long click
			esp_event_post_to(g_main_event_loop, APP_MAIN,
					  APP_POWER_SWITCH, NULL, 0,
					  portMAX_DELAY);
			break;
		case 3:
			// double click + one long click
			esp_event_post_to(g_main_event_loop, APP_MAIN,
					  APP_CONF_SWITCH, NULL, 0,
					  portMAX_DELAY);
			break;
		default:
			break;
		}
	}
}

void periph_init()
{
	gpio_config_t io_conf = {};
	EventBits_t bits;

	if (s_task != NULL)
		return;

	io_conf.mode = GPIO_MODE_INPUT;
	io_conf.pin_bit_mask = (1ULL << PERIPH_MAIN_BTN_GPIO);
	io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
	gpio_config(&io_conf);

	esp_deep_sleep_enable_gpio_wakeup(io_conf.pin_bit_mask,
					  ESP_GPIO_WAKEUP_GPIO_HIGH);

	s_main_btn = (periph_btn_ctx_t){};
	s_main_btn.last_level = !PERIPH_BTN_PRESSED_LEVEL;
	s_main_btn.io = PERIPH_MAIN_BTN_GPIO;
	s_main_btn.cb = periph_main_btn_handler;

	// Configure LED
	io_conf = (gpio_config_t){};
	io_conf.mode = GPIO_MODE_OUTPUT;
	io_conf.pin_bit_mask = (1ULL << PERIPH_STATUS_LED_GPIO);
	gpio_config(&io_conf);

	s_led_mode = STATUS_LED_OFF;
	esp_rom_gpio_pad_select_gpio(PERIPH_STATUS_LED_GPIO);
	gpio_deep_sleep_hold_en();

	s_wakeup_evt = xEventGroupCreate();
	xTaskCreate(periph_task, "app_periph", 2048, NULL, 0, &s_task);

	bits = xEventGroupWaitBits(s_wakeup_evt, PERIPH_WAKEUP_BIT, pdTRUE,
				   pdTRUE,
				   pdMS_TO_TICKS(PERIPH_BTN_LONG_CLICK_MS +
						 PERIPH_BTN_DEBOUNCE_MS * 2));

	if ((bits & PERIPH_WAKEUP_BIT) == 0) {
		periph_deinit();
		esp_deep_sleep_start();
	}
}

void periph_deinit()
{
	if (s_task == NULL)
		return;

	gpio_deep_sleep_hold_dis();
	vTaskDelete(s_task);
	s_task = NULL;
	periph_status_led_mode_set(STATUS_LED_OFF);
	vEventGroupDelete(s_wakeup_evt);
}

void periph_status_led_mode_set(enum periph_status_led_mode mode)
{
	s_led_mode = mode;
	switch (mode) {
	case STATUS_LED_OFF:
		gpio_set_level(PERIPH_STATUS_LED_GPIO, 0);
		gpio_hold_en(PERIPH_STATUS_LED_GPIO);
		break;
	case STATUS_LED_EN:
		gpio_set_level(PERIPH_STATUS_LED_GPIO, 1);
		gpio_hold_en(PERIPH_STATUS_LED_GPIO);
		break;
	default:
		gpio_hold_dis(PERIPH_STATUS_LED_GPIO);
		break;
	}
}

/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "common.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "appPERF";

#define PERIPH_BTN_PRESSED_LEVEL 0
#define PERIPH_BTN_DEBOUNCE_MS 150
#define PERIPH_BTN_long_click_MS 1000

#define PERIPH_MAIN_BTN_GPIO GPIO_NUM_4

typedef struct periph_btn_ctx periph_btn_ctx_t;

typedef void (*periph_btn_cb_t)(periph_btn_ctx_t *);

struct periph_btn_ctx {
    int last_level;
    int64_t debounce_tm;
    int64_t press_tm;

    bool long_click;
    int click_count;

    gpio_num_t io;
    periph_btn_cb_t cb;
    void *cb_ctx;
};

static TaskHandle_t s_task;
static periph_btn_ctx_t s_main_btn;

// Supports multiple_clicks+[long_click], in this order
static void periph_btn_tick(periph_btn_ctx_t *ctx, uint32_t tick_tm)
{
    int level = gpio_get_level(ctx->io);

    if (level != ctx->last_level) {
        if (level == PERIPH_BTN_PRESSED_LEVEL && ctx->debounce_tm >= PERIPH_BTN_DEBOUNCE_MS) {
            ctx->press_tm = 0;
            ctx->last_level = level;
        }

        if (level != PERIPH_BTN_PRESSED_LEVEL) {
            if (!ctx->long_click) {
                ctx->click_count++;
            }

            ctx->debounce_tm = 0;
            ctx->last_level = level;
        }
    }
    else if (level == PERIPH_BTN_PRESSED_LEVEL) {
        ctx->press_tm += tick_tm;
        if (ctx->press_tm >= PERIPH_BTN_long_click_MS) {
            ctx->long_click = true;
        }
    }

    if (level != PERIPH_BTN_PRESSED_LEVEL && ctx->debounce_tm >= 2 * PERIPH_BTN_DEBOUNCE_MS) {
        if (ctx->click_count || ctx->long_click) {
            ctx->cb(ctx);
        }
        ctx->click_count = 0;
        ctx->long_click = false;
    }

    ctx->debounce_tm += tick_tm;
}

static void periph_task(void* arg)
{
    while (1) {
        periph_btn_tick(&s_main_btn, 10);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void periph_main_btn_handler(periph_btn_ctx_t *ctx)
{
    if (ctx->long_click) {
        if (!ctx->click_count) {
	        esp_event_post_to(g_main_event_loop, APP_MAIN, APP_POWER_SWITCH,
					  NULL, 0, portMAX_DELAY);
        } else if (ctx->click_count == 2) {
	        esp_event_post_to(g_main_event_loop, APP_MAIN, APP_CONF_SWITCH,
					  NULL, 0, portMAX_DELAY);
        }
    }
}

void periph_init()
{
    gpio_config_t io_conf = {};

    if (s_task != NULL) return;

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << PERIPH_MAIN_BTN_GPIO);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    s_main_btn = (periph_btn_ctx_t) {};
    s_main_btn.last_level = !PERIPH_BTN_PRESSED_LEVEL;
    s_main_btn.io = PERIPH_MAIN_BTN_GPIO;
    s_main_btn.cb = periph_main_btn_handler;

    //start gpio task
    xTaskCreate(periph_task, "app_periph", 2048, NULL, 0, &s_task);
}

void periph_deinit()
{
    if (s_task == NULL) return;

    vTaskDelete(s_task);
    s_task = NULL;
}

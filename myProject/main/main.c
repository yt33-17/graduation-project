#include <assert.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LCD_WIDTH        320
#define LCD_HEIGHT       480
#define LCD_STRIPE_ROWS  40
#define LCD_SPI_CLOCK_HZ (20 * 1000 * 1000)

// ESP32-S3 example wiring. Change these values to match your board.
#define LCD_PIN_CS       GPIO_NUM_10
#define LCD_PIN_MOSI     GPIO_NUM_11
#define LCD_PIN_SCLK     GPIO_NUM_12
#define LCD_PIN_DC       GPIO_NUM_13
#define LCD_PIN_RST      GPIO_NUM_14
#define LCD_PIN_BL       GPIO_NUM_15
#define LCD_BL_ON_LEVEL  1

static const char *TAG = "lcd_test";

static bool lcd_transfer_done(esp_lcd_panel_io_handle_t io,
                              esp_lcd_panel_io_event_data_t *event,
                              void *user_ctx)
{
    SemaphoreHandle_t done = (SemaphoreHandle_t)user_ctx;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(done, &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

static void fill_stripe(uint8_t *buffer, uint16_t rgb565)
{
    // ST7796 receives the high byte of each RGB565 pixel first.
    for (int i = 0; i < LCD_WIDTH * LCD_STRIPE_ROWS; ++i) {
        buffer[2 * i] = rgb565 >> 8;
        buffer[2 * i + 1] = rgb565 & 0xff;
    }
}

static void show_solid_color(esp_lcd_panel_handle_t panel,
                             SemaphoreHandle_t transfer_done,
                             uint8_t *buffer,
                             uint16_t rgb565)
{
    fill_stripe(buffer, rgb565);
    for (int y = 0; y < LCD_HEIGHT; y += LCD_STRIPE_ROWS) {
        int bottom = y + LCD_STRIPE_ROWS;
        if (bottom > LCD_HEIGHT) {
            bottom = LCD_HEIGHT;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_WIDTH, bottom, buffer));
        // draw_bitmap queues a DMA transfer, so wait before reusing the buffer.
        assert(xSemaphoreTake(transfer_done, pdMS_TO_TICKS(1000)) == pdTRUE);
    }
}

void app_main(void)
{
    const spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_STRIPE_ROWS * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO));

    SemaphoreHandle_t transfer_done = xSemaphoreCreateBinary();
    assert(transfer_done != NULL);

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = LCD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_SPI_CLOCK_HZ,
        .trans_queue_depth = 1,
        .on_color_trans_done = lcd_transfer_done,
        .user_ctx = transfer_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                             &io_config, &io));

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    gpio_config_t bl_config = {
        .pin_bit_mask = 1ULL << LCD_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_config));
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BL, LCD_BL_ON_LEVEL));

    uint8_t *stripe = heap_caps_malloc(LCD_WIDTH * LCD_STRIPE_ROWS * 2,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    assert(stripe != NULL);

    const struct {
        const char *name;
        uint16_t rgb565;
    } colors[] = {
        {"RED",   0xF800},
        {"GREEN", 0x07E0},
        {"BLUE",  0x001F},
    };

    while (true) {
        for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
            show_solid_color(panel, transfer_done, stripe, colors[i].rgb565);
            ESP_LOGI(TAG, "%s", colors[i].name);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
}

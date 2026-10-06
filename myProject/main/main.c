#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include "led.h"
#include "rc522.h"

void app_main(void)
{
    led_init(LED_PIN);
    esp_err_t result = rc522_init();
    if (result != ESP_OK)
    {
        ESP_LOGE("RC522", "Init failed: %s", esp_err_to_name(result));
        return;
    }

    uint8_t last_uid[10] = { 0 };
    uint8_t last_size = 0;
    bool card_present = false;
    bool led_is_on = true;
    TickType_t last_blink = xTaskGetTickCount();
    led_on(LED_PIN);
    ESP_LOGI("LED", "LED is ON");

    while (1)
    {
        rc522_uid_t uid;
        result = rc522_read_uid(&uid);
        if (result == ESP_OK)
        {
            if (!card_present || uid.size != last_size || memcmp(uid.bytes, last_uid, uid.size) != 0)
            {
                char uid_text[30];
                size_t offset = 0;
                for (uint8_t i = 0; i < uid.size; i++)
                {
                    offset += snprintf(uid_text + offset, sizeof(uid_text) - offset,
                                       i == 0 ? "%02X" : " %02X", uid.bytes[i]);
                }
                ESP_LOGI("RC522", "UID: %s", uid_text);
                memcpy(last_uid, uid.bytes, uid.size);
                last_size = uid.size;
            }
            card_present = true;
        }
        else if (result == ESP_ERR_NOT_FOUND)
        {
            card_present = false;
        }
        else
        {
            ESP_LOGW("RC522", "Read failed: %s", esp_err_to_name(result));
        }

        if (xTaskGetTickCount() - last_blink >= pdMS_TO_TICKS(1000))
        {
            led_is_on = !led_is_on;
            if (led_is_on)
            {
                led_on(LED_PIN);
                ESP_LOGI("LED", "LED is ON");
            }
            else
            {
                led_off(LED_PIN);
                ESP_LOGI("LED", "LED is OFF");
            }
            last_blink = xTaskGetTickCount();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

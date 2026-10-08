#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include "led.h"

void app_main(void)
{
    led_init(LED_PIN);
    while(1)
    {
        led_on(LED_PIN);
        ESP_LOGI("LED", "LED ON");
        vTaskDelay(1000 / portTICK_PERIOD_MS);

        led_off(LED_PIN);
        ESP_LOGI("LED", "LED OFF");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

#include <driver/gpio.h>
#include "led.h"

void led_init(int led_num) 
{
    gpio_reset_pin(led_num);
    gpio_set_direction(led_num, GPIO_MODE_OUTPUT);
}

void led_on(int led_num) 
{
    gpio_set_level(led_num, 0);
}

void led_off(int led_num) 
{
    gpio_set_level(led_num, 1);
}

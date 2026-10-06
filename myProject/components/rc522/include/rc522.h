#pragma once

#include <stdint.h>
#include <esp_err.h>

#define RC522_PIN_SCK 12
#define RC522_PIN_MOSI 11
#define RC522_PIN_MISO 13
#define RC522_PIN_CS 10
#define RC522_PIN_RST 14

typedef struct
{
    uint8_t bytes[10];
    uint8_t size;
} rc522_uid_t;

esp_err_t rc522_init(void);
esp_err_t rc522_read_uid(rc522_uid_t *uid);

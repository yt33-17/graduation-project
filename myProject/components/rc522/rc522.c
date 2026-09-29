#include <string.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "rc522.h"

#define RC522_COMMAND_REG 0x01
#define RC522_COM_IRQ_REG 0x04
#define RC522_ERROR_REG 0x06
#define RC522_FIFO_DATA_REG 0x09
#define RC522_FIFO_LEVEL_REG 0x0A
#define RC522_CONTROL_REG 0x0C
#define RC522_BIT_FRAMING_REG 0x0D
#define RC522_MODE_REG 0x11
#define RC522_TX_CONTROL_REG 0x14
#define RC522_TX_ASK_REG 0x15
#define RC522_T_MODE_REG 0x2A
#define RC522_T_PRESCALER_REG 0x2B
#define RC522_T_RELOAD_H_REG 0x2C
#define RC522_T_RELOAD_L_REG 0x2D
#define RC522_VERSION_REG 0x37

#define RC522_CMD_IDLE 0x00
#define RC522_CMD_TRANSCEIVE 0x0C
#define RC522_CMD_SOFT_RESET 0x0F

static spi_device_handle_t rc522_spi;

#define RC522_CHECK(call) do { esp_err_t result = (call); if (result != ESP_OK) return result; } while (0)

static esp_err_t rc522_write_reg(uint8_t reg, uint8_t value)
{
    spi_transaction_t transaction = {
        .length = 16,
        .flags = SPI_TRANS_USE_TXDATA,
        .tx_data = { (uint8_t)((reg << 1) & 0x7E), value },
    };
    return spi_device_polling_transmit(rc522_spi, &transaction);
}

static esp_err_t rc522_read_reg(uint8_t reg, uint8_t *value)
{
    spi_transaction_t transaction = {
        .length = 16,
        .rxlength = 16,
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .tx_data = { (uint8_t)(((reg << 1) & 0x7E) | 0x80), 0 },
    };
    RC522_CHECK(spi_device_polling_transmit(rc522_spi, &transaction));
    *value = transaction.rx_data[1];
    return ESP_OK;
}

static esp_err_t rc522_set_bits(uint8_t reg, uint8_t bits)
{
    uint8_t value;
    RC522_CHECK(rc522_read_reg(reg, &value));
    return rc522_write_reg(reg, value | bits);
}

static uint16_t rc522_crc_a(const uint8_t *data, size_t size)
{
    uint16_t crc = 0x6363;

    for (size_t i = 0; i < size; i++)
    {
        uint8_t value = data[i] ^ (uint8_t)crc;
        value ^= value << 4;
        crc = (crc >> 8) ^ ((uint16_t)value << 8) ^ ((uint16_t)value << 3) ^ (value >> 4);
    }
    return crc;
}

static esp_err_t rc522_transceive(const uint8_t *send, size_t send_size, uint8_t last_bits,
                                  uint8_t *receive, size_t receive_capacity,
                                  size_t *receive_size, uint8_t *receive_last_bits)
{
    uint8_t irq;
    uint8_t error;
    uint8_t fifo_size;
    uint8_t control;

    RC522_CHECK(rc522_write_reg(RC522_COMMAND_REG, RC522_CMD_IDLE));
    RC522_CHECK(rc522_write_reg(RC522_COM_IRQ_REG, 0x7F));
    RC522_CHECK(rc522_write_reg(RC522_FIFO_LEVEL_REG, 0x80));
    for (size_t i = 0; i < send_size; i++)
    {
        RC522_CHECK(rc522_write_reg(RC522_FIFO_DATA_REG, send[i]));
    }
    RC522_CHECK(rc522_write_reg(RC522_BIT_FRAMING_REG, last_bits & 0x07));
    RC522_CHECK(rc522_write_reg(RC522_COMMAND_REG, RC522_CMD_TRANSCEIVE));
    RC522_CHECK(rc522_set_bits(RC522_BIT_FRAMING_REG, 0x80));

    int64_t start = esp_timer_get_time();
    do
    {
        RC522_CHECK(rc522_read_reg(RC522_COM_IRQ_REG, &irq));
        if (irq & 0x01)
        {
            return ESP_ERR_NOT_FOUND;
        }
        if (irq & 0x30)
        {
            break;
        }
    } while (esp_timer_get_time() - start < 40000);

    if (!(irq & 0x30))
    {
        return ESP_ERR_TIMEOUT;
    }
    RC522_CHECK(rc522_read_reg(RC522_ERROR_REG, &error));
    if (error & 0x1B)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    RC522_CHECK(rc522_read_reg(RC522_FIFO_LEVEL_REG, &fifo_size));
    if (fifo_size == 0 || fifo_size > receive_capacity)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    RC522_CHECK(rc522_read_reg(RC522_CONTROL_REG, &control));
    for (size_t i = 0; i < fifo_size; i++)
    {
        RC522_CHECK(rc522_read_reg(RC522_FIFO_DATA_REG, &receive[i]));
    }
    *receive_size = fifo_size;
    *receive_last_bits = control & 0x07;
    return ESP_OK;
}

static esp_err_t rc522_halt(void)
{
    uint8_t command[4] = { 0x50, 0x00 };
    uint8_t response[3];
    size_t response_size;
    uint8_t response_last_bits;
    uint16_t crc = rc522_crc_a(command, 2);
    command[2] = (uint8_t)crc;
    command[3] = (uint8_t)(crc >> 8);

    esp_err_t result = rc522_transceive(command, sizeof(command), 0, response,
                                        sizeof(response), &response_size, &response_last_bits);
    if (result == ESP_ERR_NOT_FOUND || result == ESP_ERR_TIMEOUT)
    {
        return ESP_OK;
    }
    return result;
}

esp_err_t rc522_init(void)
{
    if (rc522_spi != NULL)
    {
        return ESP_OK;
    }

    spi_bus_config_t bus = {
        .sclk_io_num = RC522_PIN_SCK,
        .mosi_io_num = RC522_PIN_MOSI,
        .miso_io_num = RC522_PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    spi_device_interface_config_t device = {
        .clock_speed_hz = 1000000,
        .mode = 0,
        .spics_io_num = RC522_PIN_CS,
        .queue_size = 1,
    };

    ESP_LOGI("RC522", "SPI pins: SCK=%d MOSI=%d MISO=%d CS=%d RST=%d",
             RC522_PIN_SCK, RC522_PIN_MOSI, RC522_PIN_MISO, RC522_PIN_CS, RC522_PIN_RST);

    RC522_CHECK(gpio_reset_pin(RC522_PIN_RST));
    RC522_CHECK(gpio_set_direction(RC522_PIN_RST, GPIO_MODE_OUTPUT));
    RC522_CHECK(gpio_set_level(RC522_PIN_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(10));
    RC522_CHECK(gpio_set_level(RC522_PIN_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(50));

    RC522_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED));
    esp_err_t result = spi_bus_add_device(SPI2_HOST, &device, &rc522_spi);
    if (result != ESP_OK)
    {
        spi_bus_free(SPI2_HOST);
        return result;
    }

    result = rc522_write_reg(RC522_COMMAND_REG, RC522_CMD_SOFT_RESET);
    if (result == ESP_OK)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
        uint8_t version = 0;
        uint8_t echo_55 = 0;
        uint8_t echo_aa = 0;
        result = rc522_read_reg(RC522_VERSION_REG, &version);
        if (result == ESP_OK) result = rc522_write_reg(RC522_T_RELOAD_L_REG, 0x55);
        if (result == ESP_OK) result = rc522_read_reg(RC522_T_RELOAD_L_REG, &echo_55);
        if (result == ESP_OK) result = rc522_write_reg(RC522_T_RELOAD_L_REG, 0xAA);
        if (result == ESP_OK) result = rc522_read_reg(RC522_T_RELOAD_L_REG, &echo_aa);
        if (result == ESP_OK)
        {
            ESP_LOGI("RC522", "VersionReg=0x%02X, SPI echo=0x%02X/0x%02X",
                     version, echo_55, echo_aa);
            if (echo_55 != 0x55 || echo_aa != 0xAA)
            {
                ESP_LOGE("RC522", "SPI register readback failed; check wiring and 3.3V power");
                result = ESP_ERR_INVALID_RESPONSE;
            }
            else if (version == 0x00 || version == 0xFF)
            {
                ESP_LOGW("RC522", "Unexpected chip version; SPI readback passed, continuing");
            }
        }
    }
    if (result == ESP_OK) result = rc522_write_reg(RC522_T_MODE_REG, 0x80);
    if (result == ESP_OK) result = rc522_write_reg(RC522_T_PRESCALER_REG, 0xA9);
    if (result == ESP_OK) result = rc522_write_reg(RC522_T_RELOAD_H_REG, 0x03);
    if (result == ESP_OK) result = rc522_write_reg(RC522_T_RELOAD_L_REG, 0xE8);
    if (result == ESP_OK) result = rc522_write_reg(RC522_TX_ASK_REG, 0x40);
    if (result == ESP_OK) result = rc522_write_reg(RC522_MODE_REG, 0x3D);
    if (result == ESP_OK) result = rc522_set_bits(RC522_TX_CONTROL_REG, 0x03);
    if (result != ESP_OK)
    {
        spi_bus_remove_device(rc522_spi);
        rc522_spi = NULL;
        spi_bus_free(SPI2_HOST);
    }
    return result;
}

esp_err_t rc522_read_uid(rc522_uid_t *uid)
{
    if (uid == NULL || rc522_spi == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    uid->size = 0;

    uint8_t wakeup = 0x52;
    uint8_t response[5];
    size_t response_size;
    uint8_t response_last_bits;
    esp_err_t result = rc522_transceive(&wakeup, 1, 7, response, sizeof(response),
                                       &response_size, &response_last_bits);
    if (result == ESP_ERR_TIMEOUT) ESP_LOGW("RC522", "WUPA timed out");
    if (result != ESP_OK) return result;
    if (response_size != 2 || response_last_bits != 0)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint8_t cascade_commands[] = { 0x93, 0x95, 0x97 };
    for (size_t level = 0; level < 3; level++)
    {
        uint8_t anticollision[] = { cascade_commands[level], 0x20 };
        result = rc522_transceive(anticollision, sizeof(anticollision), 0,
                                 response, sizeof(response), &response_size, &response_last_bits);
        if (result == ESP_ERR_TIMEOUT) ESP_LOGW("RC522", "Anticollision level %u timed out", (unsigned)(level + 1));
        if (result != ESP_OK) return result;
        if (response_size != 5 || response_last_bits != 0 ||
            (uint8_t)(response[0] ^ response[1] ^ response[2] ^ response[3]) != response[4])
        {
            return ESP_ERR_INVALID_RESPONSE;
        }

        uint8_t select[9] = { cascade_commands[level], 0x70 };
        memcpy(&select[2], response, 5);
        uint16_t crc = rc522_crc_a(select, 7);
        select[7] = (uint8_t)crc;
        select[8] = (uint8_t)(crc >> 8);
        result = rc522_transceive(select, sizeof(select), 0, response,
                                 sizeof(response), &response_size, &response_last_bits);
        if (result == ESP_ERR_TIMEOUT) ESP_LOGW("RC522", "Select level %u timed out", (unsigned)(level + 1));
        if (result != ESP_OK) return result;
        if (response_size != 3 || response_last_bits != 0 ||
            rc522_crc_a(response, 1) != ((uint16_t)response[1] | ((uint16_t)response[2] << 8)))
        {
            return ESP_ERR_INVALID_RESPONSE;
        }

        bool has_more = (response[0] & 0x04) != 0;
        bool has_cascade_tag = select[2] == 0x88;
        if (has_more != has_cascade_tag || (has_more && level == 2))
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
        uint8_t count = has_more ? 3 : 4;
        memcpy(&uid->bytes[uid->size], &select[2 + (has_more ? 1 : 0)], count);
        uid->size += count;
        if (!has_more)
        {
            return rc522_halt();
        }
    }
    return ESP_ERR_INVALID_RESPONSE;
}

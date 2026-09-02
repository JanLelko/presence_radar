#include "radar.h"
#include "board_pins.h"

#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

/* ================= KONFIGURACJA SPRZĘTOWA I2C ================= */
#define I2C_PORT            I2C_NUM_0
#define I2C_FREQ_HZ         100000

/* Adres I2C koprocesora STM32 na module XM125 (7-bitowy). */
#define RADAR_ADDR          0x52

/* Rejestry XM125 (16-bit adresy, 32-bit big-endian). Pełna mapa:
 * /home/j/main/projects/radar_esp/xm125/Inc/presence_reg_protocol.h */
#define REG_RESULT          0x0010   /* presence_reg | distance | intra | inter */
#define REG_COMMAND         0x0100   /* 1=apply config, 2=start, 3=stop */

/* Timeouty. Burst-read wyniku jest szybki; ścieżka komend może być wolniejsza
 * (STM32 rozciąga zegar przy kalibracji) — faza 1b podniesie ją do 1000 ms. */
#define TMO_CMD_MS          1000
#define TMO_READ_MS         100

/* Wartości komend na rejestr 0x0100 (patrz presence_reg_protocol.h). */
#define CMD_APPLY_CONFIG    0x00000001u
#define CMD_START_DETECTOR  0x00000002u
/* #define CMD_STOP_DETECTOR 0x00000003u  — użyty w fazie 2 (FSM) */

static const char *TAG = "RADAR";

/*
 * Zapis 32-bitowej wartości pod 16-bitowy adres rejestru XM125.
 * Ramka I2C: [REG_HI, REG_LO, VAL_B3, VAL_B2, VAL_B1, VAL_B0] — big-endian.
 */
static esp_err_t radar_write_u32(uint16_t reg, uint32_t value)
{
    uint8_t frame[6] = {
        (uint8_t)(reg   >> 8), (uint8_t)(reg   & 0xFF),
        (uint8_t)(value >> 24), (uint8_t)(value >> 16),
        (uint8_t)(value >> 8),  (uint8_t)(value & 0xFF),
    };
    return i2c_master_write_to_device(I2C_PORT, RADAR_ADDR, frame, sizeof(frame),
                                      TMO_CMD_MS / portTICK_PERIOD_MS);
}

esp_err_t radar_init(void)
{
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = PIN_I2C_SDA,
        .scl_io_num       = PIN_I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(I2C_PORT, &conf);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0);
}

esp_err_t radar_bringup_blind(void)
{
    /* 1) Apply config — zatwierdza domyślną konfigurację detektora. */
    esp_err_t err = radar_write_u32(REG_COMMAND, CMD_APPLY_CONFIG);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Apply config nie powiodło się: %s", esp_err_to_name(err));
        return err;
    }
    /* STM32 potrzebuje chwili na przeliczenie kalibracji wewnętrznej. */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* 2) Start — detektor w tryb ciągłego pomiaru. */
    err = radar_write_u32(REG_COMMAND, CMD_START_DETECTOR);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Start radaru nie powiódł się: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Radar uruchomiony (bring-up ślepy).");
    return ESP_OK;
}

esp_err_t radar_read_frame(radar_frame_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t reg_addr[2] = { (uint8_t)(REG_RESULT >> 8), (uint8_t)(REG_RESULT & 0xFF) };
    uint8_t data[16] = {0};

    esp_err_t err = i2c_master_write_read_device(
        I2C_PORT, RADAR_ADDR,
        reg_addr, sizeof(reg_addr),
        data,     sizeof(data),
        TMO_READ_MS / portTICK_PERIOD_MS);
    if (err != ESP_OK) {
        return err;
    }

    /* Big-endian -> little-endian: data[0] to MSB. Ręczne składanie z bajtów
     * (memcpy dałoby odwróconą kolejność na RISC-V little-endian). */
    uint32_t presence_reg = ((uint32_t)data[0]  << 24) | ((uint32_t)data[1]  << 16) |
                            ((uint32_t)data[2]  <<  8) | ((uint32_t)data[3]);
    uint32_t distance_mm  = ((uint32_t)data[4]  << 24) | ((uint32_t)data[5]  << 16) |
                            ((uint32_t)data[6]  <<  8) | ((uint32_t)data[7]);
    uint32_t intra_score  = ((uint32_t)data[8]  << 24) | ((uint32_t)data[9]  << 16) |
                            ((uint32_t)data[10] <<  8) | ((uint32_t)data[11]);
    uint32_t inter_score  = ((uint32_t)data[12] << 24) | ((uint32_t)data[13] << 16) |
                            ((uint32_t)data[14] <<  8) | ((uint32_t)data[15]);

    /* presence_reg pakuje trzy pola: bit0 = obecność, bit15 = błąd detektora,
     * bity 16..31 = temperatura koprocesora STM32 w °C. */
    out->is_present     = (presence_reg & 0x0001u) != 0;
    out->detector_error = (presence_reg & 0x8000u) != 0;
    out->chip_temp_c    = (uint16_t)(presence_reg >> 16);
    out->distance_mm    = distance_mm;
    out->intra_score    = intra_score;
    out->inter_score    = inter_score;

    return ESP_OK;
}

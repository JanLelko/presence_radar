#include "radar.h"

#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

/* ============== KONFIGURACJA SPRZĘTOWA I2C ============== */
#define I2C_MASTER_SDA_IO   10
#define I2C_MASTER_SCL_IO   11
#define I2C_MASTER_NUM      I2C_NUM_0
#define I2C_MASTER_FREQ_HZ  100000

/* Adres I2C koprocesora STM32 na module XM125 (7-bitowy). */
#define RADAR_ADDR          0x52

/* Rejestry XM125 (16-bitowe adresy, 32-bitowe wartości big-endian). */
#define REG_RESULT          0x0010   /* presence | distance | intra | inter */
#define REG_COMMAND         0x0100   /* 1=apply config, 2=start, 3=stop */
/* ========================================================= */

static const char *TAG = "RADAR";

/*
 * Pomocnik: zapis 32-bitowej wartości pod 16-bitowy adres rejestru XM125.
 * Ramka I2C: [REG_HI, REG_LO, VAL_BYTE3, VAL_BYTE2, VAL_BYTE1, VAL_BYTE0].
 * Koprocesor STM32 oczekuje wartości w kolejności big-endian (MSB first).
 */
static esp_err_t radar_write_reg32(uint16_t reg, uint32_t value)
{
    uint8_t frame[6] = {
        (uint8_t)(reg   >> 8), (uint8_t)(reg   & 0xFF),
        (uint8_t)(value >> 24), (uint8_t)(value >> 16),
        (uint8_t)(value >> 8),  (uint8_t)(value & 0xFF),
    };
    return i2c_master_write_to_device(I2C_MASTER_NUM, RADAR_ADDR,
                                      frame, sizeof(frame),
                                      1000 / portTICK_PERIOD_MS);
}

// konfiguracja i2c
esp_err_t init_i2c_master(void)
{
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = I2C_MASTER_SDA_IO,
        .scl_io_num       = I2C_MASTER_SCL_IO,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

// Sekwencja startowa odpalenia radaru
esp_err_t start_radar_sensor(void)
{
    /* 1) Apply config — zatwierdzamy domyślną konfigurację detektora. */
    esp_err_t err = radar_write_reg32(REG_COMMAND, 0x00000001);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Apply config nie powiodło się: %d", err);
        return err;
    }
    /* STM32 potrzebuje chwili na przeliczenie kalibracji wewnętrznej. */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* 2) Start — przełączamy detektor w tryb ciągłego pomiaru. */
    err = radar_write_reg32(REG_COMMAND, 0x00000002);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Start radaru nie powiódł się: %d", err);
        return err;
    }

    ESP_LOGI(TAG, "Radar uruchomiony.");
    return ESP_OK;
}

esp_err_t read_radar_data(radar_data_t *out_data)
{
    if (out_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Adres rejestru wysyłamy MSB-first — taka jest konwencja XM125. */
    uint8_t reg_addr[2] = { (uint8_t)(REG_RESULT >> 8),
                            (uint8_t)(REG_RESULT & 0xFF) };
    uint8_t data[16] = {0};

    esp_err_t err = i2c_master_write_read_device(
        I2C_MASTER_NUM, RADAR_ADDR,
        reg_addr, sizeof(reg_addr),
        data,     sizeof(data),
        100 / portTICK_PERIOD_MS);
    if (err != ESP_OK) {
        return err;
    }

    /* ============================================================
     * KONWERSJA BIG-ENDIAN -> LITTLE-ENDIAN (lekcja architektury)
     * ============================================================
     * Koprocesor STM32 na module XM125 to rdzeń ARM Cortex-M, który
     * dla rejestrów peryferyjnych XM125 wystawia dane na I2C w porządku
     * BIG-ENDIAN — czyli najbardziej znaczący bajt (MSB) leci pierwszy.
     *
     * ESP32-C6 to natomiast RISC-V w trybie LITTLE-ENDIAN — w pamięci
     * uint32_t trzyma w kolejności: LSB, ?, ?, MSB. Gdybyśmy zrobili
     * `memcpy(&u32, data, 4)`, dostalibyśmy bajty w odwrotnej kolejności
     * i odczyt byłby śmieciem.
     *
     * Dlatego ręcznie składamy 32-bitową liczbę z czterech bajtów,
     * używając przesunięć bitowych. `data[0]` to MSB, więc przesuwamy
     * go o 24 bity w lewo — wyląduje na najwyższej pozycji liczby
     * uint32_t. Bajty pośrednie maskujemy domyślnie (i tak są 8-bitowe),
     * a najmłodszy `data[3]` wchodzi bez przesunięcia.
     *
     * Trik: kompilator widzi że `data[i]` jest typu uint8_t — przed
     * przesunięciem awansuje go do `int`, więc maskowanie `& 0xFF` nie
     * jest tu konieczne, ale dodajemy je dla czytelności i odporności
     * gdyby w przyszłości typ bufora się zmienił.
     * ============================================================ */
    uint32_t presence_reg = ((uint32_t)data[0]  << 24) |
                            ((uint32_t)data[1]  << 16) |
                            ((uint32_t)data[2]  <<  8) |
                            ((uint32_t)data[3]);

    uint32_t distance_mm  = ((uint32_t)data[4]  << 24) |
                            ((uint32_t)data[5]  << 16) |
                            ((uint32_t)data[6]  <<  8) |
                            ((uint32_t)data[7]);

    uint32_t intra_score  = ((uint32_t)data[8]  << 24) |
                            ((uint32_t)data[9]  << 16) |
                            ((uint32_t)data[10] <<  8) |
                            ((uint32_t)data[11]);

    uint32_t inter_score  = ((uint32_t)data[12] << 24) |
                            ((uint32_t)data[13] << 16) |
                            ((uint32_t)data[14] <<  8) |
                            ((uint32_t)data[15]);

    /* presence_reg pakuje dwie różne wartości:
     *   - bit 0          -> flaga obecności (0/1)
     *   - bity 16..31    -> temperatura koprocesora STM32 w °C
     * To typowy "bit-packing" w embedded — jeden rejestr przenosi dwa
     * niezależne pola, żeby oszczędzić transakcje I2C. */
    out_data->is_present  = (presence_reg & 0x1u) != 0;
    out_data->chip_temp   = (uint16_t)(presence_reg >> 16);
    out_data->distance_mm = distance_mm;
    out_data->intra_score = intra_score;
    out_data->inter_score = inter_score;

    return ESP_OK;
}

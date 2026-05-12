#include <stdio.h>
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define I2C_MASTER_SDA_IO 10
#define I2C_MASTER_SCL_IO 11
#define I2C_MASTER_NUM I2C_NUM_0
#define RADAR_ADDR 0x52

static const char *TAG = "RADAR_ADVANCED";

void app_main(void) {
    // --- 1. KONFIGURACJA I2C ---
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);

    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "Inicjalizacja... wysyłam komendy START.");

    // --- 2. URUCHOMIENIE RADARU ---
    // Zastosuj konfigurację (Adres = 0x0100, Komenda = 1)
    uint8_t cmd_apply[6] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x01}; 
    i2c_master_write_to_device(I2C_MASTER_NUM, RADAR_ADDR, cmd_apply, 6, 1000 / portTICK_PERIOD_MS);
    vTaskDelay(pdMS_TO_TICKS(500)); 

    // Start detektora (Adres = 0x0100, Komenda = 2)
    uint8_t cmd_start[6] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x02}; 
    i2c_master_write_to_device(I2C_MASTER_NUM, RADAR_ADDR, cmd_start, 6, 1000 / portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "Skanowanie włączone!");
    vTaskDelay(pdMS_TO_TICKS(1000)); 

    // --- 3. PĘTLA ODCZYTU (WIELE REJESTRÓW NARAZ) ---
    while(1) {
        // Pytamy o adres 16 (0x0010) - początek danych wynikowych
        uint8_t reg_addr[2] = {0x00, 0x10}; 
        uint8_t data[16] = {0}; // Przygotowujemy miejsce na 16 bajtów (4 rejestry)
        
        esp_err_t err = i2c_master_write_read_device(I2C_MASTER_NUM, RADAR_ADDR, 
                                           reg_addr, 2, 
                                           data, 16, 
                                           100 / portTICK_PERIOD_MS);
        
        if (err == ESP_OK) {
            // Rejestr 16: Obecność i Temperatura
            uint32_t presence_reg = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
            
            // Rejestr 17: Odległość (Dystans w mm)
            uint32_t distance_mm  = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
            
            // Rejestr 18: Intra Score (Szybkie ruchy rąk, nóg)
            uint32_t intra_score  = (data[8] << 24) | (data[9] << 16) | (data[10] << 8) | data[11];
            
            // Rejestr 19: Inter Score (Wolne mikroruchy, np. klatka piersiowa przy oddychaniu)
            uint32_t inter_score  = (data[12] << 24) | (data[13] << 16) | (data[14] << 8) | data[15];

            // Dekodowanie specyficznych bitów
            uint16_t chip_temp = (presence_reg >> 16); // Przesuwamy o 16 bitów w prawo
            bool is_present = (presence_reg & 1);      // Maskujemy wszystko oprócz 1. bitu

            // Drukowanie wszystkich zebranych danych
            ESP_LOGI(TAG, "Ruch: %s | Dystans: %lu mm | Szybki ruch: %lu | Mikroruchy: %lu | Temp: %u C", 
                     (is_present ? "TAK" : "NIE"), distance_mm, intra_score, inter_score, chip_temp);
                     
        } else {
            ESP_LOGE(TAG, "Błąd odczytu I2C!");
        }

        vTaskDelay(pdMS_TO_TICKS(250)); // Czekamy ćwierć sekundy przed kolejnym odczytem
    }
}

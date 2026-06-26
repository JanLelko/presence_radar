#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mqtt_client.h"
#include "driver/i2c.h"
#include "driver/gpio.h"

// ================= USTAWIENIA WI-FI i MQTT =================
#include "mqtt_client.h" // Żeby plik wiedział, czym w ogóle jest ten typ danych
#include "wifi_mqtt.h"
#define MQTT_TOPIC "pokoj/101/radar"

// ================= USTAWIENIA RADARU ======================
#define I2C_MASTER_SDA_IO 2     // ZMIANA: SDA z GPIO10 na GPIO2
#define I2C_MASTER_SCL_IO 11
#define I2C_MASTER_NUM I2C_NUM_0
#define RADAR_ADDR 0x52

// ================= PINY PERYFERIÓW (nowy layout) ==========
#define PIN_MCU_INT 10          // wyjście INT z radaru (na razie nieużywane)
#define PIN_LED     18
#define PIN_BUTTON  9
#define PIN_BUZZER  3

const uint32_t PODLOGA_MM = 2500;
const uint32_t PROG_UPADKU = 1500;
const uint16_t MAX_TEMP = 60;
// ==========================================================

static const char *TAG = "RADAR_IOT";

// 3 krótkie sygnały: dioda + buzzer (potwierdzenie resetu konfiguracji)
static void sygnal_resetu_3x(void) {
    for (int i = 0; i < 3; i++) {
        gpio_set_level(PIN_LED, 1);
        gpio_set_level(PIN_BUZZER, 1);
        vTaskDelay(pdMS_TO_TICKS(150));
        gpio_set_level(PIN_LED, 0);
        gpio_set_level(PIN_BUZZER, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

// Kasuje zapisaną sieć/MQTT z NVS -> po restarcie urządzenie wejdzie w captive portal
static void kasuj_konfiguracje(void) {
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "wifi_ssid");
        nvs_erase_key(h, "wifi_pass");
        nvs_erase_key(h, "mqtt_uri");
        nvs_commit(h);
        nvs_close(h);
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Start systemu...");

    // 1. Uruchamiamy sieć i serwer
    setup_wifi_esp_idf();
    setup_mqtt();

    // Dajemy chwilę na połączenie z routerem
    vTaskDelay(pdMS_TO_TICKS(3000));

    // 2. Konfiguracja I2C (Znamy to na pamięć)
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

    // --- TEST PERYFERIÓW: LED + Buzzer wyzwalane przyciskiem ---
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << PIN_LED) | (1ULL << PIN_BUZZER),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_conf);
    gpio_set_level(PIN_LED, 0);
    gpio_set_level(PIN_BUZZER, 0);

    // Przycisk: wejście z WEWNĘTRZNYM pull-upem (przycisk między GPIO9 a GND -> wciśnięty = 0)
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << PIN_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_conf);

    // 3. Start Radaru
    uint8_t cmd_apply[6] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x01};
    i2c_master_write_to_device(I2C_MASTER_NUM, RADAR_ADDR, cmd_apply, 6, 1000 / portTICK_PERIOD_MS);
    vTaskDelay(pdMS_TO_TICKS(500));

    uint8_t cmd_start[6] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x02};
    i2c_master_write_to_device(I2C_MASTER_NUM, RADAR_ADDR, cmd_start, 6, 1000 / portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "Radar uruchomiony. Wchodzę w pętlę.");

    uint32_t czas_siedzenia = 0;
    uint32_t last_mqtt_send_time = 0;
    uint32_t button_hold = 0;   // ile pętli (po 250 ms) przycisk jest wciśnięty

    // 4. Pętla Główna
    while(1) {
            // --- Przytrzymanie przycisku ~3 s -> reset konfiguracji i powrót do captive portal ---
            if (gpio_get_level(PIN_BUTTON) == 0) {   // wciśnięty = stan niski
                button_hold++;
                if (button_hold >= 12) {             // 12 * 250 ms = 3 s
                    ESP_LOGW(TAG, "Reset konfiguracji - kasuję sieć i wracam do captive portal.");
                    sygnal_resetu_3x();
                    kasuj_konfiguracje();
                    esp_restart();
                }
            } else {
                button_hold = 0;
            }

            uint8_t reg_addr[2] = {0x00, 0x10};
            uint8_t data[16] = {0};

            esp_err_t err = i2c_master_write_read_device(I2C_MASTER_NUM, RADAR_ADDR, reg_addr, 2, data, 16, 100 / portTICK_PERIOD_MS);

        if (err == ESP_OK) {
                uint32_t presence_reg = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
                uint32_t distance_mm  = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
                uint32_t intra_score  = (data[8] << 24) | (data[9] << 16) | (data[10] << 8) | data[11];
                uint32_t inter_score  = (data[12] << 24) | (data[13] << 16) | (data[14] << 8) | data[15];

                uint16_t chip_temp = (presence_reg >> 16);
                bool is_present = (presence_reg & 1);
                char stan_alarmowy[32] = "OK";

                // LOGIKA EDGE COMPUTING
                if (chip_temp > MAX_TEMP) {
                    strcpy(stan_alarmowy, "POZAR");
                } else if (intra_score > PROG_UPADKU && distance_mm > (PODLOGA_MM - 400)) {
                    strcpy(stan_alarmowy, "UPADEK");
                }

                if (is_present && intra_score < 500 && inter_score > 1000) {
                    czas_siedzenia++;
                    if (czas_siedzenia > 1200) strcpy(stan_alarmowy, "ZASIEDZENIE");
                } else if (intra_score > 1000) {
                    czas_siedzenia = 0;
                } else {
                    czas_siedzenia = 0;
                }

                // WYSYŁKA MQTT (Tylko co 2 sekundy, żeby nie zabić serwera, chyba że jest pożar/upadek)
                uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
                if (current_time - last_mqtt_send_time > 2000 || strcmp(stan_alarmowy, "OK") != 0) {

                    // NOWY BLOK: Sprawdzamy bezpiecznik sieciowy!
                    if (wifi_ready) {
                        // JSON
                        char payload[200];
                        sprintf(payload, "{\"obecnosc\": %d, \"odleglosc\": %lu, \"mikro\": %lu, \"makro\": %lu, \"temp\": %u, \"alarm\": \"%s\"}",
                                is_present, distance_mm, inter_score, intra_score, chip_temp, stan_alarmowy);

                        // Wysyłka w świat!
                        if (mqtt_client != NULL) {
                            esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, payload, 0, 1, 1);
                            ESP_LOGI(TAG, "Wysłano MQTT: %s", payload);
                        }
                    } else {
                        // Log wysyłany co 2 sekundy, żeby nie zamrozić konsoli ścianą tekstu
                        ESP_LOGW(TAG, "Trwa pomiar, ale brak Wi-Fi (albo tryb panelu). MQTT zablokowane.");
                    }

                    last_mqtt_send_time = current_time;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(250)); // Szybkość odczytu z czujnika: 4x na sekundę
        }
    }

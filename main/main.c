#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "driver/i2c.h"

// ================= USTAWIENIA WI-FI i MQTT =================
#include "mqtt_client.h" // Żeby plik wiedział, czym w ogóle jest ten typ danych
#include "wifi_mqtt.h"
#define MQTT_TOPIC "pokoj/101/radar"

// ================= USTAWIENIA RADARU ======================
#define I2C_MASTER_SDA_IO 10
#define I2C_MASTER_SCL_IO 11
#define I2C_MASTER_NUM I2C_NUM_0
#define RADAR_ADDR 0x52

const uint32_t PODLOGA_MM = 2500;
const uint32_t PROG_UPADKU = 1500;
const uint16_t MAX_TEMP = 60;
// ==========================================================

static const char *TAG = "RADAR_IOT";

// --- FUNKCJA OBSŁUGI ZDARZEŃ WI-FI ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Rozłączono z Wi-Fi. Próba ponownego połączenia...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Połączono z Wi-Fi! IP: " IPSTR, IP2STR(&event->ip_info.ip));
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

    // 3. Start Radaru
    uint8_t cmd_apply[6] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x01};
    i2c_master_write_to_device(I2C_MASTER_NUM, RADAR_ADDR, cmd_apply, 6, 1000 / portTICK_PERIOD_MS);
    vTaskDelay(pdMS_TO_TICKS(500));

    uint8_t cmd_start[6] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x02};
    i2c_master_write_to_device(I2C_MASTER_NUM, RADAR_ADDR, cmd_start, 6, 1000 / portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "Radar uruchomiony. Wchodzę w pętlę.");

    uint32_t czas_siedzenia = 0;
    uint32_t last_mqtt_send_time = 0;

    // 4. Pętla Główna
    while(1) {
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

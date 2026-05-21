#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "mqtt_client.h"

#include "wifi_mqtt.h"
#include "radar.h"

/* ================= USTAWIENIA APLIKACJI ================= */
#define MQTT_TOPIC          "pokoj/101/radar"

/* Kalibracja Edge Computing */
static const uint32_t PODLOGA_MM   = 2500;   /* dystans radar -> podłoga w mm */
static const uint32_t PROG_UPADKU  = 1500;   /* min. intra_score uznawany za upadek */
static const uint16_t MAX_TEMP     = 60;     /* alarm temperatury chipa (°C) */
static const uint32_t PROG_ZASIEDZENIA = 1200;  /* tyle iteracji = ~5 min siedzenia */

/* Tempo pętli i throttling MQTT */
#define LOOP_PERIOD_MS      250
#define MQTT_INTERVAL_MS    2000
/* ========================================================= */

static const char *TAG = "APP";

void app_main(void)
{
    ESP_LOGI(TAG, "Start systemu...");

    /* 1. Stos sieciowy */
    setup_wifi_esp_idf();
    setup_mqtt();
    vTaskDelay(pdMS_TO_TICKS(3000));   /* daj routerowi i brokerowi chwilę */

    /* 2. Sprzęt: I2C + radar */
    ESP_ERROR_CHECK(init_i2c_master());
    ESP_ERROR_CHECK(start_radar_sensor());

    uint32_t czas_siedzenia      = 0;
    uint32_t last_mqtt_send_time = 0;

    /* 3. Pętla główna — odczyt -> klasyfikacja -> publikacja */
    while (1) {
        radar_data_t r;
        esp_err_t err = read_radar_data(&r);

        if (err == ESP_OK) {
            char stan_alarmowy[32] = "OK";

            /* === LOGIKA EDGE COMPUTING === */
            if (r.chip_temp > MAX_TEMP) {
                strcpy(stan_alarmowy, "POZAR");
            } else if (r.intra_score > PROG_UPADKU &&
                       r.distance_mm > (PODLOGA_MM - 400)) {
                strcpy(stan_alarmowy, "UPADEK");
            }

            /* Zasiedzenie: obecny + mało mikro-ruchu + dużo makro-ruchu
             * (np. oddech / drobne wiercenie się w fotelu). */
            if (r.is_present && r.intra_score < 500 && r.inter_score > 1000) {
                czas_siedzenia++;
                if (czas_siedzenia > PROG_ZASIEDZENIA) {
                    strcpy(stan_alarmowy, "ZASIEDZENIE");
                }
            } else if (r.intra_score > 1000) {
                czas_siedzenia = 0;
            } else {
                czas_siedzenia = 0;
            }

            /* === WYSYŁKA MQTT — co 2 s lub natychmiast przy alarmie === */
            uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            bool alarm = (strcmp(stan_alarmowy, "OK") != 0);

            if (alarm || (now - last_mqtt_send_time) > MQTT_INTERVAL_MS) {
                char payload[200];
                snprintf(payload, sizeof(payload),
                    "{\"obecnosc\": %d, \"odleglosc\": %lu, \"mikro\": %lu, "
                    "\"makro\": %lu, \"temp\": %u, \"alarm\": \"%s\"}",
                    r.is_present,
                    (unsigned long)r.distance_mm,
                    (unsigned long)r.inter_score,
                    (unsigned long)r.intra_score,
                    r.chip_temp,
                    stan_alarmowy);

                if (mqtt_client != NULL) {
                    esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC,
                                            payload, 0, 1, 0);
                    ESP_LOGI(TAG, "Wysłano MQTT: %s", payload);
                }
                last_mqtt_send_time = now;
            }
        } else {
            ESP_LOGW(TAG, "Błąd odczytu radaru: %d", err);
        }

        vTaskDelay(pdMS_TO_TICKS(LOOP_PERIOD_MS));
    }
}

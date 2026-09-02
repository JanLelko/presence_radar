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
#include "driver/gpio.h"

// ================= USTAWIENIA WI-FI i MQTT =================
#include "mqtt_client.h" // Żeby plik wiedział, czym w ogóle jest ten typ danych
#include "wifi_mqtt.h"
#include "status_led.h" // Sygnalizacja stanu na wbudowanym LED RGB (GPIO8)
#include "board_pins.h" // Wszystkie piny GPIO w jednym miejscu
#include "radar.h"      // Warstwa I2C do modułu XM125 (jedyny właściciel magistrali)
#include "radar_cfg.h"  // Model konfiguracji radaru (struct + presety)
#define MQTT_TOPIC "pokoj/101/radar"

// Piny (PIN_LED, PIN_BUTTON, PIN_BUZZER, PIN_RADAR_INT) — patrz board_pins.h.
// Magistrala I2C do radaru jest teraz w całości w radar.c.

const uint32_t PODLOGA_MM = 2500;
const uint32_t PROG_UPADKU = 1500;
// Próg POŻARU: alarm leci, gdy chip_temp > MAX_TEMP. Ustawione nisko (20),
// żeby dało się go wyzwolić chuchnięciem na czujnik (~21 °C i wyżej).
// UWAGA demonstracyjna: jeśli w spoczynku temp na wykresie pokazuje już ≥21,
// pożar będzie wisiał na stałe — podnieś tę wartość ~2-3 °C nad odczyt spoczynkowy.
const uint16_t MAX_TEMP = 20;
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

// ================= OBSŁUGA PRZYCISKU (GPIO9) ==============
// Przycisk NIE wisi na przerwaniu zewnętrznym: styk mechaniczny "dzwoni"
// (kilka-kilkanaście zboczy na jedno naciśnięcie), więc ISR na zboczu
// odpalałby się wielokrotnie. Zgodnie ze sztuką (Jack Ganssle, "A Guide to
// Debouncing") przycisk próbkujemy okresowo i filtrujemy programowo.
// Robimy to w osobnym tasku FreeRTOS, żeby reakcja (~30 ms) była niezależna
// od 250 ms taktu pętli radaru.
// UWAGA: tick FreeRTOS to 100 Hz (10 ms), więc BTN_SAMPLE_MS MUSI być
// wielokrotnością 10 — inaczej pdMS_TO_TICKS() zaokrągli w dół (5 ms -> 0)
// i task stałby się busy-loopem, a liczniki czasu by zwariowały. 20 ms =
// 2 ticki i mieści się w zalecanym przez Ganssle'a oknie 20-50 ms.
#define BTN_SAMPLE_MS    20     // okres próbkowania (= 2 ticki @100 Hz)
#define BTN_DEBOUNCE_MS  40     // stan musi być stabilny tyle, by go uznać
#define BTN_RESET_MS     3000   // przytrzymanie -> reset konfiguracji

static void button_task(void *arg) {
    bool stable = false;        // odfiltrowany stan (true = wciśnięty)
    bool last_raw = false;      // poprzedni surowy odczyt
    uint32_t stable_ms = 0;     // jak długo surowy odczyt się nie zmienia
    uint32_t held_ms = 0;       // jak długo przycisk jest wciśnięty (po debounce)
    bool reset_done = false;    // reset odpala raz na jedno przytrzymanie

    while (1) {
        bool raw = (gpio_get_level(PIN_BUTTON) == 0);   // wciśnięty = stan niski

        // Filtr: stan uznajemy za pewny dopiero, gdy surowy odczyt nie zmienia
        // się przez BTN_DEBOUNCE_MS.
        if (raw != last_raw) {
            last_raw = raw;
            stable_ms = 0;
        } else if (stable_ms < BTN_DEBOUNCE_MS) {
            stable_ms += BTN_SAMPLE_MS;
        }

        if (stable_ms >= BTN_DEBOUNCE_MS && raw != stable) {
            stable = raw;
            if (stable) {               // świeże, pewne wciśnięcie
                held_ms = 0;
                reset_done = false;
            } else if (!reset_done) {   // puszczenie przed 3 s = PRZEŁĄCZ alarm
                bool new_armed = !alarm_armed;
                alarm_armed = new_armed;
                ESP_LOGI(TAG, "Przycisk: alarm %s.", new_armed ? "UZBROJONY" : "rozbrojony");
                // Stan publikujemy retained, żeby panel zsynchronizował switch
                // (wspólne źródło prawdy = retained cmd). Wyciszenie buzzera/LED
                // zrobi pętla radaru w kolejnej iteracji (alarm_should=false).
                if (mqtt_client != NULL) {
                    esp_mqtt_client_publish(mqtt_client, MQTT_CMD_TOPIC,
                                            new_armed ? "{\"armed\":true}" : "{\"armed\":false}",
                                            0, 1, 1);
                }
            }
        }

        // Odliczanie przytrzymania -> reset konfiguracji i powrót do portalu.
        if (stable) {
            held_ms += BTN_SAMPLE_MS;
            if (!reset_done && held_ms >= BTN_RESET_MS) {
                reset_done = true;          // nie przełączaj alarmu po puszczeniu
                ESP_LOGW(TAG, "Reset konfiguracji - kasuję sieć i wracam do captive portal.");
                status_led_alarm(false);    // oddaj GPIO18, by task nie nadpisywał sygnału resetu
                sygnal_resetu_3x();
                kasuj_konfiguracje();
                esp_restart();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BTN_SAMPLE_MS));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Start systemu...");

    // Sygnalizacja RGB (GPIO8): autotest R->G->B + zadanie statusu.
    status_led_init();

    // 1. Uruchamiamy sieć i serwer
    setup_wifi_esp_idf();
    setup_mqtt();

    // Dajemy chwilę na połączenie z routerem
    vTaskDelay(pdMS_TO_TICKS(3000));

    // 2. Konfiguracja I2C — SDA=GPIO2, SCL=GPIO11, 100 kHz, pull-upy. Szczegóły w radar.c.
    ESP_ERROR_CHECK(radar_init());

    // --- BUZZER wyzwalany alarmem ---
    // Uwaga: dioda na PCB (PIN_LED, GPIO18) jest od teraz konfigurowana i
    // sterowana przez status_led.c, żeby migała ZGODNIE z RGB. Tutaj
    // konfigurujemy już tylko buzzer.
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << PIN_BUZZER),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_conf);
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

    // 3. Start Radaru — aplikujemy pełną konfigurację (write-all + RESET_MODULE +
    //    APPLY + START + weryfikacja). Preset CARE = udokumentowane defaulty POR.
    //    Faza 2 (test na sprzęcie): dopisz tu np. `boot_cfg.end_mm = 1200;` i
    //    sprawdź, że raportowany `odleglosc` saturuje na ~1200.
    //    Faza 3 nadpisze to configiem z NVS / MQTT.
    radar_cfg_t boot_cfg = radar_cfg_preset(CFG_PRESET_CARE);
    radar_cfg_clamp(&boot_cfg);
    radar_apply_config(&boot_cfg, NULL);
    ESP_LOGI(TAG, "Radar uruchomiony. Wchodzę w pętlę.");

    // Przycisk obsługuje osobny task (próbkowanie + debounce, niezależnie od pętli).
    xTaskCreate(button_task, "button", 3072, NULL, 4, NULL);

    uint32_t czas_siedzenia = 0;
    uint32_t last_mqtt_send_time = 0;

    // --- Stan lokalnego alarmu (buzzer/LED), sterowany nieblokująco w pętli ---
    int alarm_tick = 0;                 // licznik 0..11 (cykl 12*250ms = 3 s)
    uint32_t brak_obecnosci_licznik = 0;// ile pętli z rzędu nikogo nie ma
    bool alarm_should = false;          // czy alarm ma teraz wyć

    // 4. Pętla Główna
    while(1) {
            radar_frame_t fr;
            esp_err_t err = radar_read_frame(&fr);
            status_led_radar_err(err != ESP_OK);   // fiolet na RGB gdy I2C milczy

        if (err == ESP_OK) {
                uint32_t distance_mm = fr.distance_mm;
                uint32_t intra_score = fr.intra_score;
                uint32_t inter_score = fr.inter_score;
                uint16_t chip_temp   = fr.chip_temp_c;
                bool is_present      = fr.is_present;
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

                // --- DECYZJA O LOKALNYM ALARMIE (buzzer/LED) ---
                // Karencja na brak obecności (~1.5 s), jak ABSENCE_GRACE w panelu.
                if (!is_present) brak_obecnosci_licznik++;
                else            brak_obecnosci_licznik = 0;
                bool brak_obecnosci = (brak_obecnosci_licznik >= 6);

                // ZASIEDZENIE świadomie NIE wyzwala alarmu (tylko koloruje kartę).
                bool zdarzenie = (strcmp(stan_alarmowy, "UPADEK") == 0 ||
                                  strcmp(stan_alarmowy, "POZAR")  == 0);
                alarm_should = alarm_armed && (zdarzenie || brak_obecnosci);
                status_led_alarm(alarm_should);   // czerwień na RGB gdy alarm wyje

                // WYSYŁKA MQTT (Tylko co 2 sekundy, żeby nie zabić serwera, chyba że jest pożar/upadek)
                uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
                if (current_time - last_mqtt_send_time > 2000 || strcmp(stan_alarmowy, "OK") != 0) {

                    // NOWY BLOK: Sprawdzamy bezpiecznik sieciowy!
                    if (wifi_ready) {
                        // JSON
                        // UWAGA: nazwy pól są odwrócone względem semantyki radaru i tak
                        // zostaje (dashboard to konsumuje) — "mikro" niesie inter_score
                        // (ruch WOLNY), "makro" niesie intra_score (ruch SZYBKI). Rename
                        // po obu stronach = osobny commit (patrz plan, faza telemetrii).
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

            // --- SYGNALIZACJA ALARMU (nieblokująca, na liczniku ticków) ---
            // Cykl 3 s = 12 ticków po 250 ms: buzzer ON ticki 0..7 (2 s),
            // OFF 8..11 (1 s). Miganie diody (GPIO18) obsługuje status_led.c
            // w lockstep z RGB — tu sterujemy już tylko buzzerem.
            if (alarm_should) {
                alarm_tick = (alarm_tick + 1) % 12;
                gpio_set_level(PIN_BUZZER, alarm_tick < 8 ? 1 : 0);
            } else {
                alarm_tick = 0;
                gpio_set_level(PIN_BUZZER, 0);
            }

            vTaskDelay(pdMS_TO_TICKS(250)); // Szybkość odczytu z czujnika: 4x na sekundę
        }
    }

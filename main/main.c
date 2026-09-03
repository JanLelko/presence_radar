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
#include "esp_timer.h"
#include "freertos/queue.h"

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

// ================= KONFIGURACJA RADARU W LOCIE (faza 3) =================
// Pętla app_main jest jedynym właścicielem tych struktur i jedynym userem I2C,
// więc mutex nie jest potrzebny. Bufory robocze ścieżki cfg są file-scope, bo
// CONFIG_ESP_MAIN_TASK_STACK_SIZE = 3584 nie mieści ich na stosie.
static radar_cfg_t          g_applied_cfg;   // ostatnio zaaplikowany — baza delt + memcmp
static radar_apply_result_t g_last_apply;    // wynik ostatniego radar_apply_config()
static uint32_t             g_apply_ms;      // czas ostatniego apply [ms]
static bool                 g_state_announced;// czy po (re)connect wysłano status+cfg/state

static radar_cfg_t          s_cfg_target;
static radar_apply_result_t s_cfg_ar;
static char                 s_cfg_warn[4][RADAR_CFG_WARN_LEN];
static char                 s_cfg_state[1536];
static mqtt_msg_t           s_cfg_msg;       // bufor odbioru z kolejki (~560 B — nie na stos)

// Buduje i publikuje retained stan raportowany na pokoj/101/cfg/state.
// `result` może być NULL (pominięte); warn/nwarn opcjonalne (NULL/0).
static void publish_cfg_state(const char *st, const char *result,
                              int clamped, char warn[][RADAR_CFG_WARN_LEN], int nwarn)
{
    if (mqtt_client == NULL) {
        return;
    }
    size_t u = 0;
    #define APP(...) do { \
        if (u >= sizeof s_cfg_state) { return; } \
        int _w = snprintf(s_cfg_state + u, sizeof s_cfg_state - u, __VA_ARGS__); \
        if (_w < 0) { return; } \
        u += (size_t)_w; \
    } while (0)

    APP("{\"v\":1,\"st\":\"%s\"", st);
    if (result) {
        APP(",\"result\":\"%s\"", result);
    }
    APP(",\"clamped\":%d,\"det_status\":\"0x%08lx\",\"verify_ok\":%d,\"measuring\":%d"
        ",\"apply_ms\":%lu,\"fw\":\"%lu.%lu.%lu\",\"uptime_s\":%lu",
        clamped,
        (unsigned long)g_last_apply.detector_status,
        g_last_apply.verify_ok ? 1 : 0,
        g_last_apply.measuring ? 1 : 0,
        (unsigned long)g_apply_ms,
        (unsigned long)((g_last_apply.version >> 16) & 0xFFFF),
        (unsigned long)((g_last_apply.version >> 8)  & 0xFF),
        (unsigned long)( g_last_apply.version        & 0xFF),
        (unsigned long)(esp_timer_get_time() / 1000000));

    APP(",\"warn\":[");
    for (int i = 0; i < nwarn; i++) {
        APP("%s\"%s\"", i ? "," : "", warn[i]);
    }
    APP("],\"radar\":{");

    if (u >= sizeof s_cfg_state) {
        return;
    }
    int w = radar_cfg_to_json_fields(&g_applied_cfg, s_cfg_state + u, sizeof s_cfg_state - u);
    if (w < 0) {
        ESP_LOGW(TAG, "cfg/state: bufor za mały — nie publikuję.");
        return;
    }
    u += (size_t)w;
    APP("}}");
    #undef APP

    esp_mqtt_client_publish(mqtt_client, MQTT_CFG_STATE_TOPIC, s_cfg_state, 0, 1, 1);
}

// Konsumuje jeden komunikat cfg z kolejki: parsuje delty na aktualnym configu,
// clamp + sanitize, i jeśli coś się zmieniło — pełny RESET_MODULE + write-all
// przez radar_apply_config() (blokujące ~2-3 s; pętla stoi, telemetria wraca po).
static void apply_cfg_from_json(const mqtt_msg_t *m)
{
    s_cfg_target = g_applied_cfg;   // baza = aktualnie zaaplikowany config

    char jerr[48] = {0};
    if (!radar_cfg_from_json(&s_cfg_target, m->payload, jerr, sizeof jerr)) {
        ESP_LOGW(TAG, "cfg: odrzucony JSON (%s): %.120s", jerr, m->payload);
        publish_cfg_state("run", "bad_json", 0, NULL, 0);
        return;
    }

    int clamped = radar_cfg_clamp(&s_cfg_target);
    int nwarn   = radar_cfg_sanitize(&s_cfg_target, s_cfg_warn, 4);

    if (memcmp(&s_cfg_target, &g_applied_cfg, sizeof s_cfg_target) == 0) {
        ESP_LOGI(TAG, "cfg: identyczny z aktualnym — bez RESET/apply.");
        publish_cfg_state("run", "nochange", clamped, s_cfg_warn, nwarn);
        return;
    }

    ESP_LOGI(TAG, "cfg: nowa konfiguracja (clamped=%d, warn=%d) — RESET_MODULE + write-all.",
             clamped, nwarn);
    publish_cfg_state("cfg", NULL, clamped, s_cfg_warn, nwarn);

    int64_t t0 = esp_timer_get_time();
    esp_err_t e = radar_apply_config(&s_cfg_target, &s_cfg_ar);
    g_apply_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    if (e != ESP_OK) {
        ESP_LOGE(TAG, "cfg: radar_apply_config twardy błąd I2C (%s) — przywracam poprzednią.",
                 esp_err_to_name(e));
        radar_apply_config(&g_applied_cfg, &s_cfg_ar);   // best effort, stan bez zmiany
        g_last_apply = s_cfg_ar;
        publish_cfg_state("error", "apply_fail", clamped, s_cfg_warn, nwarn);
        return;
    }

    g_applied_cfg = s_cfg_target;
    g_last_apply  = s_cfg_ar;
    if (radar_cfg_save(&g_applied_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "cfg: zapis do NVS nieudany (config działa, ale nie przetrwa restartu).");
    }
    ESP_LOGI(TAG, "cfg: zaaplikowano w %lu ms (det_status=0x%08lx, verify=%d).",
             (unsigned long)g_apply_ms, (unsigned long)s_cfg_ar.detector_status,
             s_cfg_ar.verify_ok);
    publish_cfg_state("run", "ok", clamped, s_cfg_warn, nwarn);
}

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
        nvs_erase_key(h, "cfg_v1");   // reset fabryczny czyści też config radaru
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

    // Kolejka komend MQTT — MUSI istnieć zanim klient MQTT wystartuje, bo
    // handler zdarzeń wrzuca do niej komunikaty z tematu cfg.
    mqtt_cmd_queue = xQueueCreate(4, sizeof(mqtt_msg_t));
    if (mqtt_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Nie udało się utworzyć kolejki komend MQTT!");
    }

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

    // 3. Start Radaru — pełna konfiguracja (RESET_MODULE + write-all + APPLY +
    //    START + weryfikacja). Config z NVS, jeśli jest poprawny; wpp preset CARE
    //    (udokumentowane defaulty POR). Panel nadpisze to tematem pokoj/101/cfg.
    radar_cfg_t boot_cfg;
    if (radar_cfg_load(&boot_cfg) == ESP_OK) {
        ESP_LOGI(TAG, "Konfiguracja radaru wczytana z NVS.");
    } else {
        ESP_LOGI(TAG, "Brak/niepoprawny config w NVS — preset CARE.");
        boot_cfg = radar_cfg_preset(CFG_PRESET_CARE);
    }
    radar_cfg_clamp(&boot_cfg);
    int64_t boot_t0 = esp_timer_get_time();
    radar_apply_config(&boot_cfg, &g_last_apply);
    g_apply_ms = (uint32_t)((esp_timer_get_time() - boot_t0) / 1000);
    g_applied_cfg = boot_cfg;
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
            // --- Konfiguracja z panelu: opróżnij kolejkę (faza 3) ---
            // radar_apply_config() jest blokujące ~2-3 s; wyciszamy buzzer i
            // zerujemy licznik, żeby blokada nie zostawiła go włączonego w połowie
            // cyklu. Telemetria wraca w kolejnej iteracji.
            if (mqtt_cmd_queue != NULL && uxQueueMessagesWaiting(mqtt_cmd_queue) > 0) {
                gpio_set_level(PIN_BUZZER, 0);
                alarm_tick = 0;
                while (xQueueReceive(mqtt_cmd_queue, &s_cfg_msg, 0) == pdTRUE) {
                    apply_cfg_from_json(&s_cfg_msg);
                }
            }

            // Pierwsza publikacja statusu + stanu configu po (re)połączeniu MQTT.
            if (mqtt_connected && !g_state_announced) {
                if (mqtt_client != NULL) {
                    esp_mqtt_client_publish(mqtt_client, MQTT_STATUS_TOPIC,
                                            "{\"st\":\"online\"}", 0, 1, 1);
                }
                publish_cfg_state("run", "boot", 0, NULL, 0);
                g_state_announced = true;
            } else if (!mqtt_connected) {
                g_state_announced = false;
            }

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
                        // Pole "st": "run" gdy detektor zdrowy, "error" gdy ostatni
                        // apply zgłosił flagi błędu — panel odróżnia "cicho bo padło"
                        // od "cicho bo OK". Pętla nie działa w trakcie rekonfiguracji,
                        // więc "cfg" tu nie występuje.
                        const char *st_field = g_last_apply.had_errors ? "error" : "run";
                        char payload[240];
                        snprintf(payload, sizeof payload,
                                 "{\"obecnosc\": %d, \"odleglosc\": %lu, \"mikro\": %lu, \"makro\": %lu, \"temp\": %u, \"alarm\": \"%s\", \"st\": \"%s\"}",
                                 is_present, distance_mm, inter_score, intra_score, chip_temp, stan_alarmowy, st_field);

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

#include "status_led.h"
#include "wifi_mqtt.h"   // softap_mode, wifi_ready, mqtt_connected, mqtt_ever_connected, alarm_armed
#include "board_pins.h"  // PIN_RGB, PIN_LED

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"
#include "driver/gpio.h"

static const char *TAG = "STATUS_LED";

#define LED_GPIO     PIN_RGB   // wbudowany WS2812 na DevKitC-1
#define PCB_LED_GPIO PIN_LED   // dioda alarmowa na PCB — miga w zgodzie z RGB
#define TICK_MS      50        // okres pętli sygnalizacji
#define DIM          40        // bazowa jasność (na 255 dioda oślepia)

static led_strip_handle_t s_strip = NULL;

// Flagi ustawiane z main.c (radar) — reszta stanów czytana z wifi_mqtt.h.
static volatile bool s_alarm_firing = false;
static volatile bool s_radar_err    = false;

void status_led_alarm(bool firing) { s_alarm_firing = firing; }
void status_led_radar_err(bool err) { s_radar_err = err; }

// Ustawia kolor tylko gdy się zmienił (mniej odświeżeń RMT, brak migotania).
static void show(uint8_t r, uint8_t g, uint8_t b) {
    static int lr = -1, lg = -1, lb = -1;
    if (r == lr && g == lg && b == lb) return;
    lr = r; lg = g; lb = b;
    if (!s_strip) return;
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

// Krótki, blokujący błysk (używany w autoteście i przy świeżym MQTT).
static void flash(uint8_t r, uint8_t g, uint8_t b, int on_ms, int off_ms) {
    show(r, g, b);
    vTaskDelay(pdMS_TO_TICKS(on_ms));
    show(0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(off_ms));
}

// Stan wynikowy wg priorytetu (od najważniejszego).
typedef enum {
    ST_CAPTIVE, ST_ALARM, ST_WIFI, ST_NO_MQTT, ST_MQTT_LOST,
    ST_RADAR_ERR, ST_ARMED, ST_OK
} eff_state_t;

static eff_state_t resolve(void) {
    if (softap_mode)     return ST_CAPTIVE;      // tryb konfiguracji
    if (s_alarm_firing)  return ST_ALARM;        // bezpieczeństwo > reszta
    if (!wifi_ready)     return ST_WIFI;         // łączenie / utrata Wi-Fi
    if (!mqtt_connected) return mqtt_ever_connected ? ST_MQTT_LOST : ST_NO_MQTT;
    if (s_radar_err)     return ST_RADAR_ERR;    // I2C nie odpowiada
    if (alarm_armed)     return ST_ARMED;        // uzbrojony, spokój
    return ST_OK;                                // praca, rozbrojony
}

static void status_led_task(void *arg) {
    int phase = 0;            // licznik ticków do migania
    bool prev_mqtt = false;   // do wykrycia świeżego połączenia MQTT
    bool pcb_led_owned = false; // czy zadanie steruje teraz diodą na PCB

    while (1) {
        // Świeże połączenie MQTT -> 3x mignięcie zielone (jednorazowo).
        bool mqtt_now = mqtt_connected;
        if (mqtt_now && !prev_mqtt) {
            for (int i = 0; i < 3; i++) flash(0, DIM, 0, 120, 120);
        }
        prev_mqtt = mqtt_now;

        eff_state_t st = resolve();
        // Fazy migania: szybkie (alarm) i wolne (reszta).
        bool fast_on = (phase % 4) < 2;    // ~200 ms on / 200 ms off
        bool slow_on = (phase % 10) < 5;   // ~500 ms on / 500 ms off

        switch (st) {
            case ST_CAPTIVE:   show(0, 0, DIM); break;                          // niebieski stały
            case ST_ALARM:     show(fast_on ? 70 : 0, 0, 0); break;             // czerwony szybko
            case ST_WIFI:      show(slow_on ? DIM : 0, slow_on ? DIM/2 : 0, 0); break; // żółty miga
            case ST_NO_MQTT:   show(0, slow_on ? DIM : 0, slow_on ? DIM : 0); break;   // cyan miga
            case ST_MQTT_LOST: show(slow_on ? DIM : 0, 0, slow_on ? DIM : 0); break;   // magenta miga
            case ST_RADAR_ERR: show(slow_on ? DIM/2 : 0, 0, slow_on ? DIM : 0); break; // fiolet miga
            case ST_ARMED:     show(DIM, DIM/3, 0); break;                      // pomarańczowy stały
            case ST_OK:        show(0, DIM, 0); break;                          // zielony stały
        }

        // Dioda na PCB (GPIO18) miga w LOCKSTEP z RGB — ten sam licznik `phase`
        // i ten sam `fast_on`, więc identyczna częstotliwość ORAZ faza. Pin
        // dotykamy tylko w alarmie; przy wyjściu z alarmu raz zerujemy i go
        // oddajemy, żeby nie nadpisywać np. sygnału resetu (sygnal_resetu_3x).
        if (st == ST_ALARM) {
            gpio_set_level(PCB_LED_GPIO, fast_on);
            pcb_led_owned = true;
        } else if (pcb_led_owned) {
            gpio_set_level(PCB_LED_GPIO, 0);
            pcb_led_owned = false;
        }

        phase++;
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

void status_led_init(void) {
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,   // 10 MHz
        .flags = { .with_dma = false },
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Nie udało się zainicjalizować WS2812 (GPIO%d): %s",
                 LED_GPIO, esp_err_to_name(err));
        return;
    }

    // Dioda alarmowa na PCB (GPIO18) — od teraz właścicielem jest to zadanie,
    // żeby migała zgodnie (ta sama częstotliwość i faza) z RGB.
    gpio_config_t pcb_led_cfg = {
        .pin_bit_mask = (1ULL << PCB_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pcb_led_cfg);
    gpio_set_level(PCB_LED_GPIO, 0);

    // Autotest: R -> G -> B, żeby potwierdzić, że dioda i pin grają.
    flash(DIM, 0, 0, 400, 100);
    flash(0, DIM, 0, 400, 100);
    flash(0, 0, DIM, 400, 100);
    show(0, 0, 0);

    xTaskCreate(status_led_task, "status_led", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "Sygnalizacja RGB uruchomiona na GPIO%d.", LED_GPIO);
}

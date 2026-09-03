#ifndef WIFI_MQTT_H
#define WIFI_MQTT_H

#include <stdbool.h>
#include <stdint.h>
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/*
 * Globalny uchwyt do klienta MQTT — definiowany w wifi_mqtt.c, a tu jedynie
 * deklarowany jako `extern`, żeby inne pliki (np. main.c) mogły z niego
 * korzystać po inicjalizacji przez setup_mqtt().
 */
extern esp_mqtt_client_handle_t mqtt_client;

/* Inicjalizuje NVS, stos Wi-Fi, rejestruje handlery zdarzeń i łączy się
 * z punktem dostępowym zdefiniowanym w wifi_mqtt.c (WIFI_SSID / WIFI_PASS). */
void setup_wifi_esp_idf(void);

/* Inicjalizuje i uruchamia klienta MQTT pod adresem MQTT_BROKER_URI.
 * Należy wywołać DOPIERO po setup_wifi_esp_idf(). */
void setup_mqtt(void);

// Globalna flaga statusu połączenia
extern volatile bool wifi_ready;

// Flagi stanu dla sygnalizacji RGB (status_led.c):
extern volatile bool softap_mode;         // tryb konfiguracji (captive portal)
extern volatile bool mqtt_connected;      // klient MQTT aktualnie połączony
extern volatile bool mqtt_ever_connected; // czy MQTT połączył się choć raz

/* Flaga uzbrojenia alarmu, sterowana z panelu przez retained-temat
 * "pokoj/101/cmd" ({"armed":true|false}). Ustawiana w handlerze MQTT
 * w wifi_mqtt.c, czytana przez pętlę alarmu w main.c. */
extern volatile bool alarm_armed;

/* Temat poleceń panel -> radar (subskrypcja + publikacja rozbrojenia). */
#define MQTT_CMD_TOPIC "pokoj/101/cmd"

/* Konfiguracja radaru:
 *  - MQTT_CFG_TOPIC       panel -> radar, retained: delty configu (JSON płaski)
 *  - MQTT_CFG_STATE_TOPIC radar -> świat, retained: stan raportowany (panel się stąd wypełnia)
 *  - MQTT_STATUS_TOPIC    radar -> świat, retained: {"st":"online"|"offline"} (LWT) */
#define MQTT_CFG_TOPIC        "pokoj/101/cfg"
#define MQTT_CFG_STATE_TOPIC  "pokoj/101/cfg/state"
#define MQTT_STATUS_TOPIC     "pokoj/101/status"

/* Komunikat z brokera przekazywany z tasku MQTT do pętli radaru. Kopiowany
 * by-value — task MQTT nie dotyka I2C ani configu, tylko wrzuca do kolejki;
 * pętla w main.c jest jedynym konsumentem, więc mutex nie jest potrzebny. */
typedef struct {
    char     topic[48];
    uint16_t payload_len;
    char     payload[512];
} mqtt_msg_t;

/* Kolejka komend (głęb. 4, by-value). Tworzona w app_main PRZED setup_mqtt();
 * NULL, dopóki nie utworzona — handler MQTT musi to sprawdzić. */
extern QueueHandle_t mqtt_cmd_queue;

#endif /* WIFI_MQTT_H */

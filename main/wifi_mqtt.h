#ifndef WIFI_MQTT_H
#define WIFI_MQTT_H

#include "mqtt_client.h"

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

#endif /* WIFI_MQTT_H */

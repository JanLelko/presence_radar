#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdbool.h>

/*
 * Sygnalizacja stanu kontrolera na WBUDOWANYM adresowalnym LED-zie RGB
 * (WS2812) podłączonym fabrycznie do GPIO8 na ESP32-C6-DevKitC-1.
 *
 * Moduł sam uruchamia zadanie FreeRTOS, które co ~50 ms wylicza bieżący
 * stan na podstawie flag (tryb SoftAP, Wi-Fi, MQTT, alarm, błąd radaru)
 * i ustawia odpowiedni kolor / wzorzec migania. Stany ustawia się przez
 * globalne flagi w wifi_mqtt.h oraz settery poniżej — bez blokowania.
 */

// Inicjalizuje WS2812 na GPIO8, robi autotest R->G->B i startuje zadanie.
void status_led_init(void);

// Ustawiane z pętli głównej (main.c):
void status_led_alarm(bool firing);     // alarm wyje (UPADEK/POŻAR/brak obecności)
void status_led_radar_err(bool err);    // nieudany odczyt I2C z radaru

#endif /* STATUS_LED_H */

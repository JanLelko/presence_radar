#ifndef BOARD_PINS_H
#define BOARD_PINS_H

/*
 * Jedyne miejsce w projekcie z numerami pinów GPIO ESP32-C6.
 *
 * Historycznie piny były rozsiane po main.c, radar.c (z BŁĘDNYM SDA=GPIO10 —
 * to linia INT radaru) i status_led.c. Trzymanie ich tutaj sprawia, że rozjazd
 * typu "SDA na GPIO2 czy GPIO10?" jest strukturalnie niemożliwy.
 *
 * Layout płytki (patrz też README §1 i hardware/):
 *   I2C do modułu XM125: SDA=GPIO2, SCL=GPIO11, 100 kHz, wewn. pull-upy.
 */

#define PIN_I2C_SDA     2    // XM125 SDA
#define PIN_I2C_SCL     11   // XM125 SCL
#define PIN_RADAR_INT   10   // XM125 INT (MCU_INT) — podłączone, na razie nieużywane w kodzie
#define PIN_RGB         8    // wbudowany WS2812 na DevKitC-1 (status_led.c)
#define PIN_LED         18   // dioda alarmowa na PCB — miga w lockstep z RGB (status_led.c)
#define PIN_BUTTON      9    // przycisk między GPIO9 a GND, wewn. pull-up (wciśnięty = 0)
#define PIN_BUZZER      3    // buzzer alarmu

#endif /* BOARD_PINS_H */

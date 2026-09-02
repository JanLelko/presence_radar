#ifndef RADAR_H
#define RADAR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * Warstwa dostępu do modułu radarowego Acconeer XM125 (koprocesor STM32,
 * aplikacja "I2C Presence Detector") po magistrali I2C.
 *
 * Ten moduł jest JEDYNYM kodem w projekcie, który dotyka magistrali I2C0.
 * main.c operuje wyłącznie na funkcjach poniżej — nie zna adresów rejestrów
 * ani kolejności bajtów.
 *
 * Protokół XM125: slave 0x52, adresy rejestrów 16-bit MSB-first, wartości
 * 32-bit big-endian, każdy transfer to wielokrotność 4 bajtów danych.
 */

/* Zdekodowana (już little-endian, natywnie dla ESP32-C6) ramka pomiarowa —
 * 16 bajtów z rejestru 0x0010 (presence_reg | distance | intra | inter). */
typedef struct {
    bool     is_present;      /* rejestr 0x0010 bit 0 */
    bool     detector_error;  /* rejestr 0x0010 bit 15 — dekodowane, nieużywane do fazy 1b */
    uint16_t chip_temp_c;     /* rejestr 0x0010 bity 16..31 — temp. koprocesora w °C */
    uint32_t distance_mm;     /* rejestr 0x0011 — odległość celu [mm] */
    uint32_t intra_score;     /* rejestr 0x0012 — energia ruchu SZYBKIEGO (mikro/gest) */
    uint32_t inter_score;     /* rejestr 0x0013 — energia ruchu WOLNEGO (makro/obecność) */
} radar_frame_t;

/* Konfiguruje magistralę I2C0 jako master: SDA=PIN_I2C_SDA, SCL=PIN_I2C_SCL,
 * 100 kHz, wewnętrzne pull-upy. Woła się raz przy starcie. */
esp_err_t radar_init(void);

/* "Ślepy" bring-up jak w oryginalnym firmware: zapis 1 (apply config) na
 * rejestr 0x0100, 500 ms przerwy, zapis 2 (start detector). Nie czyta statusu
 * detektora — faza 1b zastąpi to wersją z weryfikacją. Blokuje na czas startu. */
esp_err_t radar_bringup_blind(void);

/* Odczytuje 16 bajtów z rejestru 0x0010 i wypełnia radar_frame_t.
 * Zwraca ESP_OK, jeśli transakcja I2C się powiodła. */
esp_err_t radar_read_frame(radar_frame_t *out);

#endif /* RADAR_H */

#ifndef RADAR_H
#define RADAR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * Struktura zawierająca zdekodowaną (już w formacie little-endian, natywnym
 * dla ESP32-C6) ramkę pomiarową z koprocesora radarowego XM125.
 */
typedef struct {
    bool     is_present;     /* bit 0 rejestru obecności */
    uint32_t distance_mm;    /* odległość celu w milimetrach */
    uint32_t intra_score;    /* energia ruchu szybkiego (mikro) */
    uint32_t inter_score;    /* energia ruchu wolnego (makro) */
    uint16_t chip_temp;      /* temperatura koprocesora w °C (górne 16 bitów presence_reg) */
} radar_data_t;

/* Konfiguruje magistralę I2C0 jako master (GPIO 10/11, 100 kHz, pull-up ON). */
esp_err_t init_i2c_master(void);

/* Wysyła do XM125 komendę apply (1) na rejestr 0x0100, czeka 500 ms,
 * następnie wysyła komendę start (2). Funkcja blokuje na czas startu radaru. */
esp_err_t start_radar_sensor(void);

/* Odczytuje 16 bajtów z rejestru 0x0010 i wypełnia strukturę radar_data_t.
 * Zwraca ESP_OK, jeśli transakcja I2C się powiodła. */
esp_err_t read_radar_data(radar_data_t *out_data);

#endif /* RADAR_H */

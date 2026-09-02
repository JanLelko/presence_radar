#ifndef RADAR_H
#define RADAR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "radar_cfg.h"

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
 * Pełna mapa rejestrów:
 *   /home/j/main/projects/radar_esp/xm125/Inc/presence_reg_protocol.h
 */

/* Zdekodowana (już little-endian, natywnie dla ESP32-C6) ramka pomiarowa —
 * 16 bajtów z rejestru 0x0010 (presence_reg | distance | intra | inter). */
typedef struct {
    bool     is_present;      /* rejestr 0x0010 bit 0 */
    bool     detector_error;  /* rejestr 0x0010 bit 15 — dekodowane, nieużywane do fazy 2 */
    uint16_t chip_temp_c;     /* rejestr 0x0010 bity 16..31 — temp. koprocesora w °C */
    uint32_t distance_mm;     /* rejestr 0x0011 — odległość celu [mm] */
    uint32_t intra_score;     /* rejestr 0x0012 — energia ruchu SZYBKIEGO (mikro/gest) */
    uint32_t inter_score;     /* rejestr 0x0013 — energia ruchu WOLNEGO (makro/obecność) */
} radar_frame_t;

/* Wynik aplikacji konfiguracji — do zalogowania / opublikowania (faza 3). */
typedef struct {
    uint32_t version;          /* rejestr 0x0000; 0 => WAKE_UP prawdopodobnie wisi */
    uint32_t detector_status;  /* rejestr 0x0003 po APPLY + START */
    bool     apply_ok;         /* maska bitów 0..7 statusu == 0xFF */
    bool     had_errors;       /* jakikolwiek bit błędu (16..23, 28) */
    bool     verify_ok;        /* odczyt zwrotny wybranych rejestrów zgadza się z cfg */
    bool     measuring;        /* measure counter (0x0002) rośnie => detektor liczy */
    char     status_str[160];  /* dekode detector_status: "RSS_REGISTER_OK|CONFIG_APPLY_OK|..." */
} radar_apply_result_t;

/* Konfiguruje magistralę I2C0 jako master: SDA=PIN_I2C_SDA, SCL=PIN_I2C_SCL,
 * 100 kHz, wewnętrzne pull-upy. Woła się raz przy starcie. */
esp_err_t radar_init(void);

/* Aplikuje pełną konfigurację do modułu i uruchamia detektor. Sekwencja:
 *   STOP -> RESET_MODULE -> czekaj na boot -> zapis WSZYSTKICH rejestrów z
 *   RADAR_CFG_DESC -> APPLY (1) -> poll BUSY -> START (2) -> poll BUSY ->
 *   odczyt zwrotny wybranych rejestrów + Measure Counter.
 *
 * RESET_MODULE cofa moduł do stanu POR, więc zapis MUSI objąć cały zestaw —
 * stąd write-all, nie tylko zmienione pola.
 *
 * NIE brickuje: błąd statusu / niezgodność odczytu zwrotnego są logowane
 * GŁOŚNO, ale funkcja i tak kończy w STARCIE. Zwraca ESP_OK, jeśli magistrala
 * I2C żyła; ESP_ERR_* tylko przy twardej awarii I2C lub gdy moduł nie wstał po
 * resecie. `cfg` jest kopiowane; `out` może być NULL. */
esp_err_t radar_apply_config(const radar_cfg_t *cfg, radar_apply_result_t *out);

/* Odczytuje 16 bajtów z rejestru 0x0010 i wypełnia radar_frame_t.
 * Zwraca ESP_OK, jeśli transakcja I2C się powiodła. */
esp_err_t radar_read_frame(radar_frame_t *out);

/* Dekoduje rejestr Detector Status (0x0003) do czytelnego stringu z ustawionymi
 * flagami rozdzielonymi '|'. Zwraca `buf`. */
const char *radar_status_decode(uint32_t status, char *buf, size_t n);

#endif /* RADAR_H */

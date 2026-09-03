#ifndef EDGE_CFG_H
#define EDGE_CFG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "radar_cfg.h"   /* RADAR_CFG_WARN_LEN, cfg_json_u32() */

/*
 * Progi edge-computingu (alarmy w main.c). NIE są to rejestry radaru — te pola
 * żyją tylko w firmware i sterują klasyfikatorem POZAR / UPADEK / ZASIEDZENIE.
 * Konfigurowane tym samym kanałem co radar (temat pokoj/101/cfg), pod kluczem
 * "edge":{...}. Trwałość: własny blob NVS (klucz "edge_v1").
 */

#define EDGE_CFG_N   4

typedef struct {
    uint32_t podloga_mm;      /* zmierzony dystans radar->podłoga [mm]; reguła upadku */
    uint32_t prog_upadku;     /* min. intra_score liczone jako upadek */
    uint32_t max_temp_c;      /* próg alarmu POZAR [°C] */
    uint32_t zasiedzenie_s;   /* ile sekund bez ruchu = ZASIEDZENIE */
} edge_cfg_t;

typedef struct {
    const char *key;
    uint16_t    off;
    uint32_t    min;
    uint32_t    max;
    uint32_t    def;
} edge_cfg_desc_t;

extern const edge_cfg_desc_t EDGE_CFG_DESC[EDGE_CFG_N];

/* Kompletny zestaw defaultów. */
edge_cfg_t edge_cfg_default(void);

/* Docina każde pole do [min,max]; zwraca liczbę zmienionych. */
int edge_cfg_clamp(edge_cfg_t *e);

/* Sprawdzenie międzypolowe: podloga_mm-400 poza zasięgiem radaru (end_mm)
 * po cichu wyłącza wykrywanie UPADKU — zgłasza to jako warn (nie poprawia,
 * bo to legalna konfiguracja). Zwraca liczbę wpisów warn. */
int edge_cfg_sanitize(const edge_cfg_t *e, uint32_t radar_end_mm,
                      char warn[][RADAR_CFG_WARN_LEN], int max_warn);

/* Nakłada delty "<klucz>": <liczba> z EDGE_CFG_DESC na *e. Zwraca liczbę
 * rozpoznanych kluczy (0 = payload nie dotyczył edge). */
int edge_cfg_from_json(edge_cfg_t *e, const char *json);

/* Serializuje 4 pola jako "klucz":wartość rozdzielone przecinkiem, bez klamer.
 * Zwraca liczbę znaków bez NUL, albo -1 przy zbyt małym buforze. */
int edge_cfg_to_json_fields(const edge_cfg_t *e, char *buf, size_t n);

/* NVS (namespace "storage", klucz "edge_v1"): blob magic+wersja+CRC32. */
esp_err_t edge_cfg_load(edge_cfg_t *out);
esp_err_t edge_cfg_save(const edge_cfg_t *e);

#endif /* EDGE_CFG_H */

#include "edge_cfg.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "esp_rom_crc.h"

#define E(field, mn, mx, dflt) \
    { .key = #field, .off = offsetof(edge_cfg_t, field), \
      .min = (mn), .max = (mx), .def = (dflt) }

const edge_cfg_desc_t EDGE_CFG_DESC[EDGE_CFG_N] = {
    /* podloga_mm min 1000: niższy dystans radar->podłoga nie ma fizycznego
     * sensu dla montażu i sprowadziłby bramkę upadku (podloga_mm-400) do zera,
     * przez co UPADEK wyzwalałby się na dowolny szybki ruch w pokoju. */
    E(podloga_mm,    1000, 6000, 2500),
    E(prog_upadku,      0, 5000, 1500),
    E(max_temp_c,       0,   90,   20),
    E(zasiedzenie_s,   10, 3600,  300),
};
#undef E

_Static_assert(sizeof(edge_cfg_t) / sizeof(uint32_t) == EDGE_CFG_N,
               "edge_cfg_t: dodano/usunieto pole -> zaktualizuj EDGE_CFG_DESC i EDGE_CFG_N");
_Static_assert(sizeof(EDGE_CFG_DESC) / sizeof(EDGE_CFG_DESC[0]) == EDGE_CFG_N,
               "EDGE_CFG_DESC: liczba wierszy != EDGE_CFG_N");

static inline uint32_t *field_ptr(edge_cfg_t *e, size_t i)
{
    return (uint32_t *)((char *)e + EDGE_CFG_DESC[i].off);
}

edge_cfg_t edge_cfg_default(void)
{
    edge_cfg_t e = {0};
    for (size_t i = 0; i < EDGE_CFG_N; i++) {
        *field_ptr(&e, i) = EDGE_CFG_DESC[i].def;
    }
    return e;
}

int edge_cfg_clamp(edge_cfg_t *e)
{
    int changed = 0;
    for (size_t i = 0; i < EDGE_CFG_N; i++) {
        uint32_t *p = field_ptr(e, i);
        uint32_t v = *p;
        if (v < EDGE_CFG_DESC[i].min) {
            v = EDGE_CFG_DESC[i].min;
        } else if (v > EDGE_CFG_DESC[i].max) {
            v = EDGE_CFG_DESC[i].max;
        }
        if (v != *p) {
            *p = v;
            changed++;
        }
    }
    return changed;
}

int edge_cfg_sanitize(const edge_cfg_t *e, uint32_t radar_end_mm,
                      char warn[][RADAR_CFG_WARN_LEN], int max_warn)
{
    int n = 0;
    /* Reguła upadku (main.c): intra > prog_upadku && distance_mm > podloga_mm-400.
     * Jeśli podloga_mm-400 wypada za końcem strefy radaru, warunek dystansu nigdy
     * nie zajdzie -> UPADEK martwy. To legalne (można celowo), więc tylko warn. */
    if (e->podloga_mm >= 400 && (e->podloga_mm - 400) >= radar_end_mm) {
        if (n < max_warn) {
            snprintf(warn[n], RADAR_CFG_WARN_LEN,
                     "podloga_mm-400 (%lu) >= end_mm radaru (%lu) — UPADEK nieaktywny",
                     (unsigned long)(e->podloga_mm - 400), (unsigned long)radar_end_mm);
            n++;
        }
    }
    return n;
}

int edge_cfg_from_json(edge_cfg_t *e, const char *json)
{
    if (e == NULL || json == NULL) {
        return 0;
    }
    int hits = 0;
    for (size_t i = 0; i < EDGE_CFG_N; i++) {
        uint32_t v;
        if (cfg_json_u32(json, EDGE_CFG_DESC[i].key, &v)) {
            *field_ptr(e, i) = v;
            hits++;
        }
    }
    return hits;
}

int edge_cfg_to_json_fields(const edge_cfg_t *e, char *buf, size_t n)
{
    size_t used = 0;
    for (size_t i = 0; i < EDGE_CFG_N; i++) {
        uint32_t v = *(const uint32_t *)((const char *)e + EDGE_CFG_DESC[i].off);
        int w = snprintf(buf + used, n - used, "%s\"%s\":%lu",
                         used ? "," : "", EDGE_CFG_DESC[i].key, (unsigned long)v);
        if (w < 0 || (size_t)w >= n - used) {
            return -1;
        }
        used += (size_t)w;
    }
    return (int)used;
}

/* ================= NVS ================= */

#define EDGE_NVS_NS    "storage"
#define EDGE_NVS_KEY   "edge_v1"
#define EDGE_MAGIC     0x47464345u   /* 'ECFG' */
#define EDGE_VERSION   1

typedef struct {
    uint32_t   magic;
    uint16_t   version;
    uint16_t   len;
    uint32_t   crc32;
    edge_cfg_t edge;
} edge_persist_t;

esp_err_t edge_cfg_save(const edge_cfg_t *e)
{
    if (e == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    edge_persist_t p = {
        .magic   = EDGE_MAGIC,
        .version = EDGE_VERSION,
        .len     = (uint16_t)sizeof(edge_cfg_t),
        .edge    = *e,
    };
    p.crc32 = esp_rom_crc32_le(0, (const uint8_t *)&p.edge, sizeof p.edge);

    nvs_handle_t h;
    esp_err_t err = nvs_open(EDGE_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, EDGE_NVS_KEY, &p, sizeof p);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t edge_cfg_load(edge_cfg_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(EDGE_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    edge_persist_t p;
    size_t len = sizeof p;
    err = nvs_get_blob(h, EDGE_NVS_KEY, &p, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }
    if (len != sizeof p || p.magic != EDGE_MAGIC ||
        p.version != EDGE_VERSION || p.len != sizeof(edge_cfg_t)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (esp_rom_crc32_le(0, (const uint8_t *)&p.edge, sizeof p.edge) != p.crc32) {
        return ESP_ERR_INVALID_CRC;
    }
    *out = p.edge;
    return ESP_OK;
}

#include "radar_cfg.h"

#include <stddef.h>
#include <stdio.h>

/*
 * Zakresy min/max wg "A121 Presence Detector User Guide", Tabela 3
 * (wartości RSS przeskalowane ×1000 tam, gdzie rejestr I2C tak robi).
 * frame_rate nie ma udokumentowanego sufitu — clamp [1000,30000] do czasu
 * pomiaru osiągalnej częstotliwości w fazie 6.
 */
#define D(field, reg_, mn, mx, dflt, boolean) \
    { .key = #field, .reg = (reg_), .off = offsetof(radar_cfg_t, field), \
      .min = (mn), .max = (mx), .def = (dflt), .is_bool = (boolean) }

const radar_cfg_desc_t RADAR_CFG_DESC[RADAR_CFG_N] = {
    D(sweeps_per_frame,                0x0040,    1,  4095,    16, false),
    D(inter_frame_presence_timeout_s,  0x0041,    0,    30,     3, false),
    D(intra_detection_enabled,         0x0043,    0,     1,     1, true ),
    D(inter_detection_enabled,         0x0044,    0,     1,     1, true ),
    D(frame_rate_mhz,                  0x0045, 1000, 30000, 12000, false),
    D(intra_detection_threshold,       0x0046,    0,  5000,  1300, false),
    D(inter_detection_threshold,       0x0047,    0,  5000,  1000, false),
    D(inter_frame_deviation_time_const,0x0048,   10, 20000,   500, false),
    D(inter_frame_fast_cutoff_mhz,     0x0049, 1000, 50000,  6000, false),
    D(inter_frame_slow_cutoff_mhz,     0x004a,   10,  1000,   200, false),
    D(intra_frame_time_const,          0x004b,    0,  1000,   150, false),
    D(intra_output_time_const,         0x004c,   10, 20000,   300, false),
    D(inter_output_time_const,         0x004d,   10, 20000,  2000, false),
    D(auto_profile_enabled,            0x004e,    0,     1,     1, true ),
    D(auto_step_length_enabled,        0x004f,    0,     1,     1, true ),
    D(manual_profile,                  0x0050,    1,     5,     4, false),
    D(manual_step_length,              0x0051,    1,  4095,    72, false),
    D(start_mm,                        0x0052,    0, 10000,   300, false),
    D(end_mm,                          0x0053,  100, 10000,  2500, false),
    D(reset_filters_on_prepare,        0x0054,    0,     1,     1, true ),
    D(hwaas,                           0x0055,    1,   511,    32, false),
    D(automatic_subsweeps,             0x0056,    0,     1,     1, true ),
    D(signal_quality,                  0x0057,    0, 35000, 15000, false),
    D(detection_on_gpio,               0x0080,    0,     1,     0, true ),
};
#undef D

/* Dodanie/usunięcie pola w radar_cfg_t bez aktualizacji tabeli i RADAR_CFG_N =
 * błąd kompilacji (rozmiar struktury != liczba wierszy). Semantyka write-all po
 * RESET_MODULE tego wymaga — niezapisany rejestr cicho wraca do defaultu POR.
 * Uwaga: asercje NIE łapią wiersza z powielonym .off; przed tym broni jeszcze
 * radar_cfg_t c = {0} w radar_cfg_preset() (najgorszy przypadek: zapis zera). */
_Static_assert(sizeof(radar_cfg_t) / sizeof(uint32_t) == RADAR_CFG_N,
               "radar_cfg_t: dodano/usunieto pole -> zaktualizuj RADAR_CFG_DESC i RADAR_CFG_N");
_Static_assert(sizeof(RADAR_CFG_DESC) / sizeof(RADAR_CFG_DESC[0]) == RADAR_CFG_N,
               "RADAR_CFG_DESC: liczba wierszy != RADAR_CFG_N");

static inline uint32_t *field_ptr(radar_cfg_t *c, size_t i)
{
    return (uint32_t *)((char *)c + RADAR_CFG_DESC[i].off);
}

radar_cfg_t radar_cfg_preset(radar_cfg_preset_t preset)
{
    radar_cfg_t c = {0};   /* siatka bezpieczeństwa, gdyby tabela nie pokryła pola */
    for (size_t i = 0; i < RADAR_CFG_N; i++) {
        *field_ptr(&c, i) = RADAR_CFG_DESC[i].def;
    }

    if (preset == CFG_PRESET_GAME) {
        /* Biurko, gest jako przycisk: krótki zasięg, wysoki frame rate, minimalne
         * stałe czasowe intra (najszybsza reakcja), inter wyłączony. Progi
         * detektora nisko — realne progowanie robi gesture.c (faza 5). */
        c.start_mm                  = 100;
        c.end_mm                    = 700;
        c.frame_rate_mhz            = 20000;
        c.sweeps_per_frame          = 8;
        c.hwaas                     = 16;
        c.intra_frame_time_const    = 0;
        c.intra_output_time_const   = 50;
        c.intra_detection_enabled   = 1;
        c.inter_detection_enabled   = 0;
        c.auto_profile_enabled      = 0;
        c.manual_profile            = 1;   /* profil 1 = najkrótszy zasięg, najlepsza rozdz. */
        /* auto_step_length zostaje 1 — krok wyprowadzany z profilu 1 (krótki),
         * co jest OK dla latencji; ręczny krok dostroimy pomiarowo w fazie 6. */
        c.reset_filters_on_prepare  = 1;
        c.intra_detection_threshold = 300;
    }

    return c;
}

int radar_cfg_clamp(radar_cfg_t *c)
{
    int changed = 0;
    for (size_t i = 0; i < RADAR_CFG_N; i++) {
        uint32_t *p = field_ptr(c, i);
        uint32_t v = *p;
        if (RADAR_CFG_DESC[i].is_bool) {
            v = (v != 0) ? 1 : 0;
        } else if (v < RADAR_CFG_DESC[i].min) {
            v = RADAR_CFG_DESC[i].min;
        } else if (v > RADAR_CFG_DESC[i].max) {
            v = RADAR_CFG_DESC[i].max;
        }
        if (v != *p) {
            *p = v;
            changed++;
        }
    }
    return changed;
}

int radar_cfg_sanitize(radar_cfg_t *c, char warn[][RADAR_CFG_WARN_LEN], int max_warn)
{
    int n = 0;
    #define PUSH_WARN(...) do { \
        if (n < max_warn) { snprintf(warn[n], RADAR_CFG_WARN_LEN, __VA_ARGS__); n++; } \
    } while (0)

    /* end musi być sensownie za start. */
    if (c->end_mm <= c->start_mm + 100) {
        uint32_t fixed = c->start_mm + 100;
        PUSH_WARN("end_mm (%lu) <= start_mm+100 — podnosze do %lu",
                  (unsigned long)c->end_mm, (unsigned long)fixed);
        c->end_mm = fixed;
    }

    /* Oba detektory wyłączone => detektor nic nie zwróci. */
    if (!c->intra_detection_enabled && !c->inter_detection_enabled) {
        PUSH_WARN("intra i inter detection wylaczone — wymuszam intra=1");
        c->intra_detection_enabled = 1;
    }

    /* manual_profile / manual_step_length są ignorowane przy auto=1 — tylko info. */
    if (c->auto_profile_enabled) {
        PUSH_WARN("auto_profile=1 — manual_profile ignorowany przez modul");
    }
    if (c->auto_step_length_enabled) {
        PUSH_WARN("auto_step_length=1 — manual_step_length ignorowany przez modul");
    }

    #undef PUSH_WARN
    return n;
}

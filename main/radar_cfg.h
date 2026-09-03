#ifndef RADAR_CFG_H
#define RADAR_CFG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/*
 * Model danych konfiguracji radaru XM125 — lustro zapisywalnych rejestrów
 * detektora obecności (0x0040..0x0080).
 *
 * KLUCZOWE (patrz plan, sekcja "Model konfiguracji"):
 * Rejestr Command modułu przyjmuje RESET_MODULE, który cofa STM32 do stanu
 * POR. Dlatego KAŻDA rekonfiguracja musi przepisać CAŁY zestaw rejestrów —
 * pole tej struktury bez wiersza w RADAR_CFG_DESC cicho wróciłoby do defaultu
 * sprzętowego. Bramkuje to _Static_assert w radar_cfg.c.
 *
 * Jednostki: część pól to wartość RSS ×1000 (progi, częstotliwości odcięcia,
 * stałe czasowe) — tak jak w rejestrach I2C ("factor 1000 larger than the RSS
 * value", A121 Presence Detector User Guide). start/end są w mm.
 */

/* Liczba rejestrów w modelu — używana też przez _Static_assert. */
#define RADAR_CFG_N   24

typedef struct {
    uint32_t sweeps_per_frame;               /* 0x40  liczba sweepów na ramkę */
    uint32_t inter_frame_presence_timeout_s;  /* 0x41  s, 0..30 */
    uint32_t intra_detection_enabled;         /* 0x43  0/1 */
    uint32_t inter_detection_enabled;         /* 0x44  0/1 */
    uint32_t frame_rate_mhz;                  /* 0x45  mHz (12000 = 12 Hz) */
    uint32_t intra_detection_threshold;       /* 0x46  RSS×1000, 0..5000 */
    uint32_t inter_detection_threshold;       /* 0x47  RSS×1000, 0..5000 */
    uint32_t inter_frame_deviation_time_const;/* 0x48  ms */
    uint32_t inter_frame_fast_cutoff_mhz;     /* 0x49  mHz */
    uint32_t inter_frame_slow_cutoff_mhz;     /* 0x4a  mHz */
    uint32_t intra_frame_time_const;          /* 0x4b  ms */
    uint32_t intra_output_time_const;         /* 0x4c  ms — największa dźwignia latencji */
    uint32_t inter_output_time_const;         /* 0x4d  ms */
    uint32_t auto_profile_enabled;            /* 0x4e  0/1 */
    uint32_t auto_step_length_enabled;        /* 0x4f  0/1 */
    uint32_t manual_profile;                  /* 0x50  enum 1..5 (gdy auto=0) */
    uint32_t manual_step_length;              /* 0x51  (gdy auto=0) */
    uint32_t start_mm;                        /* 0x52  początek strefy [mm] */
    uint32_t end_mm;                          /* 0x53  koniec strefy [mm], > start */
    uint32_t reset_filters_on_prepare;        /* 0x54  0/1 */
    uint32_t hwaas;                           /* 0x55  1..511 */
    uint32_t automatic_subsweeps;             /* 0x56  0/1 */
    uint32_t signal_quality;                  /* 0x57  RSS×1000 */
    uint32_t detection_on_gpio;               /* 0x80  0/1 — radar steruje pinem przy obecności */
} radar_cfg_t;

/* Deskryptor jednego rejestru: klucz JSON (faza 3), adres, offset w strukturze,
 * zakres i domyślna wartość, czy pole jest bool (0/1). */
typedef struct {
    const char *key;
    uint16_t    reg;
    uint16_t    off;
    uint32_t    min;
    uint32_t    max;
    uint32_t    def;
    bool        is_bool;
} radar_cfg_desc_t;

/* Jedyne źródło prawdy: clamp, zapis do rejestrów i (faza 3) parser/serializer
 * JSON chodzą po tej samej tabeli, więc nie mogą się rozjechać. */
extern const radar_cfg_desc_t RADAR_CFG_DESC[RADAR_CFG_N];

typedef enum {
    CFG_PRESET_CARE = 0,   /* udokumentowane defaulty POR — pełny nadzór pokoju */
    CFG_PRESET_GAME,        /* biurko, krótki zasięg, minimalna latencja (faza 6) */
} radar_cfg_preset_t;

/* Zwraca kompletny preset. */
radar_cfg_t radar_cfg_preset(radar_cfg_preset_t preset);

/* Docina każde pole do [min,max] z RADAR_CFG_DESC; bool wymusza na 0/1.
 * Zwraca liczbę pól, które zostały zmienione (0 = wszystko było w normie). */
int radar_cfg_clamp(radar_cfg_t *c);

/* Maks. długość jednego komunikatu ostrzeżenia z radar_cfg_sanitize(). */
#define RADAR_CFG_WARN_LEN   96

/* Sprawdzenia międzypolowe (np. end_mm <= start_mm, podłoga poza zasięgiem).
 * Poprawia to, co da się poprawić, resztę zgłasza tekstem do `warn`.
 * Zwraca liczbę wypełnionych wpisów `warn` (<= max_warn). */
int radar_cfg_sanitize(radar_cfg_t *c, char warn[][RADAR_CFG_WARN_LEN], int max_warn);

/* --- JSON (parser ręczny — IDF v6.1 nie dostarcza cJSON) --------------------
 * Payload konfiguracji jest płaski: pary "<klucz>": <liczba|true|false>, bez
 * zagnieżdżeń i tablic (patrz docs/parametry_radaru.md). */

/* Nakłada na *c znalezione pary "<klucz>": <wartość> dla kluczy z RADAR_CFG_DESC.
 * Baza (*c) to zwykle aktualnie zaaplikowany config — panel wysyła tylko delty.
 * Jeśli payload zawiera "preset":"care"|"game", najpierw przeładowuje *c z tego
 * presetu, potem nakłada delty. Zwraca false, gdy payload nie zaczyna się od '{'
 * albo nie ma ani presetu, ani żadnego znanego klucza; `err` (może być NULL)
 * dostaje krótki powód. Wartości nie są walidowane — to robi radar_cfg_clamp(). */
bool radar_cfg_from_json(radar_cfg_t *c, const char *json, char *err, size_t errn);

/* Serializuje wszystkie 24 pola jako "klucz":wartość rozdzielone przecinkiem,
 * BEZ klamer (wołający wstawia to do swojego obiektu). Zwraca liczbę zapisanych
 * znaków bez NUL, albo -1 przy zbyt małym buforze. */
int radar_cfg_to_json_fields(const radar_cfg_t *c, char *buf, size_t n);

/* --- Trwałość w NVS (namespace "storage", klucz "cfg_v1") ------------------
 * Blob: magic + wersja + długość + CRC32 nad radar_cfg_t. Wszystko-albo-nic:
 * brownout w połowie zapisu = blob nie przejdzie walidacji -> preset fabryczny. */

/* ESP_OK tylko gdy blob istnieje i przechodzi walidację; wpp *out nietknięte
 * (wołający używa wtedy radar_cfg_preset(CFG_PRESET_CARE)). */
esp_err_t radar_cfg_load(radar_cfg_t *out);

/* Zapisuje *c jako blob. Wołać dopiero po udanym radar_apply_config(). */
esp_err_t radar_cfg_save(const radar_cfg_t *c);

#endif /* RADAR_CFG_H */

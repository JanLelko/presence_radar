#include "radar.h"
#include "radar_cfg.h"
#include "board_pins.h"

#include <stdio.h>
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

/* ================= KONFIGURACJA SPRZĘTOWA I2C ================= */
#define I2C_PORT            I2C_NUM_0
#define I2C_FREQ_HZ         100000

/* Adres I2C koprocesora STM32 na module XM125 (7-bitowy). */
#define RADAR_ADDR          0x52

/* Rejestry XM125. Pełna mapa (adresy dziesiętne w nagłówku, tu hex):
 * /home/j/main/projects/radar_esp/xm125/Inc/presence_reg_protocol.h */
#define REG_VERSION         0x0000   /* [31:16] major, [15:8] minor, [7:0] patch */
#define REG_MEASURE_COUNTER 0x0002   /* rośnie z każdym pomiarem detektora */
#define REG_DETECTOR_STATUS 0x0003   /* flagi *_OK / *_ERROR / BUSY */
#define REG_RESULT          0x0010   /* presence_reg | distance | intra | inter */
#define REG_COMMAND         0x0100   /* patrz CMD_* */

/* Wartości komend na rejestr 0x0100 (presence_reg_protocol.h). */
#define CMD_APPLY_CONFIG    0x00000001u
#define CMD_START_DETECTOR  0x00000002u
#define CMD_STOP_DETECTOR   0x00000003u
#define CMD_RESET_MODULE    0x52535421u   /* ASCII "RST!" — cofa STM32 do stanu POR */

/* Detector Status (0x0003) — maski. */
#define ST_OK_MASK          0x000000FFu   /* bity 0..7 — wszystkie muszą być 1 po APPLY */
#define ST_ERR_MASK         0x00FF0000u   /* bity 16..23 — flagi *_ERROR */
#define ST_DETECTOR_ERROR   (1u << 28)
#define ST_BUSY             (1u << 31)

/* Timeouty. Burst-read wyniku jest szybki; ścieżka komend/statusu może być
 * wolna — STM32 rozciąga zegar podczas kalibracji po APPLY. */
#define TMO_CMD_MS          1000
#define TMO_READ_MS         100

/* Ile czekać na zejście flagi BUSY po APPLY / START. */
#define BUSY_TMO_APPLY_MS   3000
#define BUSY_TMO_START_MS   2000
#define BUSY_POLL_MS        20

/* Ile czekać, aż moduł odpowie po RESET_MODULE (NAK-uje póki STM32 się nie podniesie). */
#define BOOT_TMO_MS         2000
#define BOOT_POLL_MS        20

static const char *TAG = "RADAR";

/* Nazwy bitów Detector Status do dekodowania (bit -> etykieta). */
static const struct { uint8_t bit; const char *name; } k_status_bits[] = {
    { 0,  "RSS_REGISTER_OK"       }, { 1,  "CONFIG_CREATE_OK"      },
    { 2,  "SENSOR_CREATE_OK"      }, { 3,  "SENSOR_CALIBRATE_OK"   },
    { 4,  "DETECTOR_CREATE_OK"    }, { 5,  "DETECTOR_BUFFER_OK"    },
    { 6,  "SENSOR_BUFFER_OK"      }, { 7,  "CONFIG_APPLY_OK"       },
    { 16, "RSS_REGISTER_ERROR"    }, { 17, "CONFIG_CREATE_ERROR"   },
    { 18, "SENSOR_CREATE_ERROR"   }, { 19, "SENSOR_CALIBRATE_ERROR"},
    { 20, "DETECTOR_CREATE_ERROR" }, { 21, "DETECTOR_BUFFER_ERROR" },
    { 22, "SENSOR_BUFFER_ERROR"   }, { 23, "CONFIG_APPLY_ERROR"    },
    { 28, "DETECTOR_ERROR"        }, { 31, "BUSY"                  },
};

/* ---------- Prymitywy I2C ---------- */

/*
 * Zapis 32-bitowej wartości pod 16-bitowy adres rejestru XM125.
 * Ramka I2C: [REG_HI, REG_LO, VAL_B3, VAL_B2, VAL_B1, VAL_B0] — big-endian.
 */
static esp_err_t radar_write_u32(uint16_t reg, uint32_t value)
{
    uint8_t frame[6] = {
        (uint8_t)(reg   >> 8), (uint8_t)(reg   & 0xFF),
        (uint8_t)(value >> 24), (uint8_t)(value >> 16),
        (uint8_t)(value >> 8),  (uint8_t)(value & 0xFF),
    };
    return i2c_master_write_to_device(I2C_PORT, RADAR_ADDR, frame, sizeof(frame),
                                      TMO_CMD_MS / portTICK_PERIOD_MS);
}

/* Odczyt pojedynczego rejestru 32-bit (adres out MSB-first, wartość big-endian). */
static esp_err_t radar_read_u32(uint16_t reg, uint32_t *out)
{
    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    uint8_t data[4] = {0};
    esp_err_t err = i2c_master_write_read_device(I2C_PORT, RADAR_ADDR,
                                                 addr, sizeof(addr),
                                                 data, sizeof(data),
                                                 TMO_CMD_MS / portTICK_PERIOD_MS);
    if (err != ESP_OK) {
        return err;
    }
    *out = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] <<  8) | ((uint32_t)data[3]);
    return ESP_OK;
}

/* Czeka aż w Detector Status zgaśnie BUSY. Zwraca ostatni odczytany status
 * przez *status. ESP_ERR_TIMEOUT, jeśli nie zgasło w `tmo_ms`. */
static esp_err_t radar_wait_not_busy(uint32_t tmo_ms, uint32_t *status)
{
    uint32_t waited = 0;
    for (;;) {
        esp_err_t err = radar_read_u32(REG_DETECTOR_STATUS, status);
        if (err != ESP_OK) {
            return err;
        }
        if ((*status & ST_BUSY) == 0) {
            return ESP_OK;
        }
        if (waited >= tmo_ms) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(BUSY_POLL_MS));
        waited += BUSY_POLL_MS;
    }
}

/* ---------- API ---------- */

esp_err_t radar_init(void)
{
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = PIN_I2C_SDA,
        .scl_io_num       = PIN_I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(I2C_PORT, &conf);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0);
}

const char *radar_status_decode(uint32_t status, char *buf, size_t n)
{
    if (buf == NULL || n == 0) {
        return "";
    }
    buf[0] = '\0';
    size_t used = 0;
    for (size_t i = 0; i < sizeof(k_status_bits) / sizeof(k_status_bits[0]); i++) {
        if (!(status & (1u << k_status_bits[i].bit))) {
            continue;
        }
        int w = snprintf(buf + used, n - used, "%s%s",
                         used ? "|" : "", k_status_bits[i].name);
        if (w < 0 || (size_t)w >= n - used) {
            break;   /* bufor pełny — reszta flag urwana, i tak diagnostyka */
        }
        used += (size_t)w;
    }
    if (used == 0) {
        snprintf(buf, n, "(brak flag: 0x%08lx)", (unsigned long)status);
    }
    return buf;
}

/* Czeka, aż moduł odpowie po RESET_MODULE: Version czytelny i != 0xFFFFFFFF,
 * a Detector Status bez BUSY. NAK w tym oknie znaczy "jeszcze nie gotowe",
 * nie awarię. */
static esp_err_t radar_wait_boot(uint32_t *version_out)
{
    uint32_t waited = 0;
    for (;;) {
        uint32_t v = 0;
        if (radar_read_u32(REG_VERSION, &v) == ESP_OK && v != 0 && v != 0xFFFFFFFFu) {
            uint32_t st = 0;
            if (radar_read_u32(REG_DETECTOR_STATUS, &st) == ESP_OK && !(st & ST_BUSY)) {
                *version_out = v;
                return ESP_OK;
            }
        }
        if (waited >= BOOT_TMO_MS) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_MS));
        waited += BOOT_POLL_MS;
    }
}

/* Wspólny ogon bring-upu: APPLY -> poll BUSY -> ocena statusu -> START ->
 * poll BUSY -> Measure Counter. Wypełnia pola r; loguje. Nie brickuje przy złym
 * statusie — zwraca ESP_ERR_* tylko przy twardej awarii I2C. */
static esp_err_t radar_apply_and_start(radar_apply_result_t *r)
{
    uint32_t st = 0;

    esp_err_t err = radar_write_u32(REG_COMMAND, CMD_APPLY_CONFIG);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Zapis APPLY_CONFIGURATION: %s", esp_err_to_name(err));
        return err;
    }
    err = radar_wait_not_busy(BUSY_TMO_APPLY_MS, &st);
    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "BUSY nie zgasło w %d ms po APPLY — brnę dalej.", BUSY_TMO_APPLY_MS);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Poll statusu po APPLY: %s", esp_err_to_name(err));
        return err;
    }

    err = radar_write_u32(REG_COMMAND, CMD_START_DETECTOR);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Zapis START_DETECTOR: %s", esp_err_to_name(err));
        return err;
    }
    err = radar_wait_not_busy(BUSY_TMO_START_MS, &st);
    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "BUSY nie zgasło w %d ms po START — brnę dalej.", BUSY_TMO_START_MS);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Poll statusu po START: %s", esp_err_to_name(err));
        return err;
    }

    r->detector_status = st;
    radar_status_decode(st, r->status_str, sizeof(r->status_str));
    r->apply_ok   = (st & ST_OK_MASK) == ST_OK_MASK;
    r->had_errors = (st & (ST_ERR_MASK | ST_DETECTOR_ERROR)) != 0;
    if (r->had_errors) {
        ESP_LOGE(TAG, "BŁĄD detektora: 0x%08lx [%s]", (unsigned long)st, r->status_str);
    } else if (!r->apply_ok) {
        ESP_LOGW(TAG, "Nie wszystkie flagi *_OK: 0x%08lx [%s]", (unsigned long)st, r->status_str);
    } else {
        ESP_LOGI(TAG, "Detektor OK: 0x%08lx [%s]", (unsigned long)st, r->status_str);
    }

    uint32_t mc0 = 0, mc1 = 0;
    radar_read_u32(REG_MEASURE_COUNTER, &mc0);
    vTaskDelay(pdMS_TO_TICKS(500));
    radar_read_u32(REG_MEASURE_COUNTER, &mc1);
    r->measuring = (mc1 != mc0);
    if (r->measuring) {
        ESP_LOGI(TAG, "Measure Counter %lu -> %lu — detektor liczy.",
                 (unsigned long)mc0, (unsigned long)mc1);
    } else {
        ESP_LOGW(TAG, "Measure Counter zamrożony na %lu — detektor NIE liczy. Brnę dalej.",
                 (unsigned long)mc0);
    }
    return ESP_OK;
}

/* Odczyt zwrotny reprezentatywnych rejestrów — czy write-all + APPLY faktycznie
 * weszły. Loguje każdą niezgodność. */
static bool radar_verify_readback(const radar_cfg_t *cfg)
{
    const struct { uint16_t reg; uint32_t want; const char *key; } check[] = {
        { 0x0052, cfg->start_mm,                  "start_mm" },
        { 0x0053, cfg->end_mm,                    "end_mm" },
        { 0x0045, cfg->frame_rate_mhz,            "frame_rate_mhz" },
        { 0x0046, cfg->intra_detection_threshold, "intra_detection_threshold" },
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(check) / sizeof(check[0]); i++) {
        uint32_t got = 0;
        if (radar_read_u32(check[i].reg, &got) != ESP_OK) {
            ESP_LOGW(TAG, "Weryfikacja: odczyt 0x%04x (%s) nie powiódł się",
                     check[i].reg, check[i].key);
            ok = false;
        } else if (got != check[i].want) {
            ESP_LOGW(TAG, "Weryfikacja: %s (0x%04x) = %lu, oczekiwano %lu",
                     check[i].key, check[i].reg, (unsigned long)got,
                     (unsigned long)check[i].want);
            ok = false;
        }
    }
    return ok;
}

esp_err_t radar_apply_config(const radar_cfg_t *cfg, radar_apply_result_t *out)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    radar_apply_result_t r = {0};

    /* --- STOP + RESET_MODULE: cofnij moduł do POR, by przyjął nową konfigurację. --- */
    radar_write_u32(REG_COMMAND, CMD_STOP_DETECTOR);   /* błąd tu nieistotny */
    radar_write_u32(REG_COMMAND, CMD_RESET_MODULE);

    esp_err_t err = radar_wait_boot(&r.version);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Moduł nie odpowiada po RESET_MODULE (%d ms) — sprawdź WAKE_UP (3V3).",
                 BOOT_TMO_MS);
        if (out) { *out = r; }
        return err;
    }
    ESP_LOGI(TAG, "XM125 firmware v%lu.%lu.%lu",
             (unsigned long)((r.version >> 16) & 0xFFFF),
             (unsigned long)((r.version >> 8)  & 0xFF),
             (unsigned long)( r.version        & 0xFF));

    /* --- write-all: cały zestaw rejestrów (wymóg po RESET_MODULE). --- */
    for (size_t i = 0; i < RADAR_CFG_N; i++) {
        uint32_t v = *(const uint32_t *)((const char *)cfg + RADAR_CFG_DESC[i].off);
        err = radar_write_u32(RADAR_CFG_DESC[i].reg, v);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Zapis rejestru 0x%04x (%s): %s",
                     RADAR_CFG_DESC[i].reg, RADAR_CFG_DESC[i].key, esp_err_to_name(err));
            if (out) { *out = r; }
            return err;
        }
    }

    err = radar_apply_and_start(&r);
    if (err != ESP_OK) {
        if (out) { *out = r; }
        return err;
    }

    /* Odczyt zwrotny (OSTRZEGA, nie brickuje). */
    r.verify_ok = radar_verify_readback(cfg);

    if (out) { *out = r; }
    return ESP_OK;
}

esp_err_t radar_read_frame(radar_frame_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t reg_addr[2] = { (uint8_t)(REG_RESULT >> 8), (uint8_t)(REG_RESULT & 0xFF) };
    uint8_t data[16] = {0};

    esp_err_t err = i2c_master_write_read_device(
        I2C_PORT, RADAR_ADDR,
        reg_addr, sizeof(reg_addr),
        data,     sizeof(data),
        TMO_READ_MS / portTICK_PERIOD_MS);
    if (err != ESP_OK) {
        return err;
    }

    /* Big-endian -> little-endian: data[0] to MSB. Ręczne składanie z bajtów
     * (memcpy dałoby odwróconą kolejność na RISC-V little-endian). */
    uint32_t presence_reg = ((uint32_t)data[0]  << 24) | ((uint32_t)data[1]  << 16) |
                            ((uint32_t)data[2]  <<  8) | ((uint32_t)data[3]);
    uint32_t distance_mm  = ((uint32_t)data[4]  << 24) | ((uint32_t)data[5]  << 16) |
                            ((uint32_t)data[6]  <<  8) | ((uint32_t)data[7]);
    uint32_t intra_score  = ((uint32_t)data[8]  << 24) | ((uint32_t)data[9]  << 16) |
                            ((uint32_t)data[10] <<  8) | ((uint32_t)data[11]);
    uint32_t inter_score  = ((uint32_t)data[12] << 24) | ((uint32_t)data[13] << 16) |
                            ((uint32_t)data[14] <<  8) | ((uint32_t)data[15]);

    /* presence_reg pakuje trzy pola: bit0 = obecność, bit15 = błąd detektora,
     * bity 16..31 = temperatura koprocesora STM32 w °C. */
    out->is_present     = (presence_reg & 0x0001u) != 0;
    out->detector_error = (presence_reg & 0x8000u) != 0;
    out->chip_temp_c    = (uint16_t)(presence_reg >> 16);
    out->distance_mm    = distance_mm;
    out->intra_score    = intra_score;
    out->inter_score    = inter_score;

    return ESP_OK;
}

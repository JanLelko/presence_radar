#include "captive_portal.h"
#include <esp_http_server.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "CAPTIVE_PORTAL";

// Nasza strona HTML zapisana jako stały tekst (const char)
static const char *html_page =
    "<!DOCTYPE html><html>"
    "<head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    "<style>"
    "body{font-family:Arial,sans-serif;background-color:#121212;color:#ffffff;display:flex;justify-content:center;align-items:center;height:100vh;margin:0;}"
    ".card{background-color:#1e1e1e;padding:30px;border-radius:10px;box-shadow:0 4px 8px rgba(0,0,0,0.5);width:100%;max-width:350px;}"
    "h2{text-align:center;margin-top:0;color:#00e676;}"
    "label{display:block;margin-top:15px;font-size:14px;color:#b3b3b3;}"
    "input{width:100%;padding:10px;margin-top:5px;border:none;border-radius:5px;background-color:#333;color:#fff;box-sizing:border-box;}"
    "input:focus{outline:2px solid #00e676;}"
    "button{width:100%;padding:12px;margin-top:25px;border:none;border-radius:5px;background-color:#00e676;color:#000;font-weight:bold;font-size:16px;cursor:pointer;}"
    "button:hover{background-color:#00c853;}"
    "</style></head>"
    "<body>"
    "<div class=\"card\">"
    "<h2>Setup Radaru</h2>"
    "<form action=\"/save\" method=\"POST\">"
    "<label>Nazwa sieci (SSID):</label>"
    "<input type=\"text\" name=\"ssid\" required>"
    "<label>Hasło Wi-Fi:</label>"
    "<input type=\"password\" name=\"pass\">"
    "<label>Adres MQTT (URI):</label>"
    "<input type=\"text\" name=\"mqtt\" placeholder=\"mqtt://IP:1883\" required>"
    "<button type=\"submit\">Zapisz i Restartuj</button>"
    "</form></div></body></html>";


// Odkodowywanie znaków %XX z formularzy webowych
static void url_decode(char *str) {
    char *pstr = str, *buf = str;
    while (*pstr) {
        if (*pstr == '%') {
            if (pstr[1] && pstr[2]) {
                int c;
                sscanf(pstr + 1, "%2x", &c);
                *buf++ = (char)c;
                pstr += 3;
            } else break;
        } else if (*pstr == '+') {
            *buf++ = ' '; // Plusy w URL to często spacje
            pstr++;
        } else {
            *buf++ = *pstr++;
        }
    }
    *buf = '\0';
}

static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req) {
    char buf[256];
    int ret, remaining = req->content_len;

    if (remaining >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    char ssid[32] = {0};
    char pass[64] = {0};
    char mqtt[64] = {0};

    httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(buf, "pass", pass, sizeof(pass));
    httpd_query_key_value(buf, "mqtt", mqtt, sizeof(mqtt));

    // ODKODOWUJEMY WSZYSTKO Z POWROTEM! (Dodaj te 3 linijki)
    url_decode(ssid);
    url_decode(pass);
    url_decode(mqtt);

    ESP_LOGI(TAG, "Otrzymano konfigurację! SSID: %s, MQTT: %s", ssid, mqtt);

    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_str(my_handle, "wifi_ssid", ssid);
        nvs_set_str(my_handle, "wifi_pass", pass);
        nvs_set_str(my_handle, "mqtt_uri", mqtt);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }

    const char *resp = "Zapisano ustawienia! Urzadzenie zaraz sie zrestartuje...";
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();

    return ESP_OK;
}



void start_captive_portal(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
        httpd_register_uri_handler(server, &root_uri);

        httpd_uri_t save_uri = { .uri = "/save", .method = HTTP_POST, .handler = save_post_handler };
        httpd_register_uri_handler(server, &save_uri);

        ESP_LOGI(TAG, "Serwer WWW odpalony na porcie 80.");
    }
}

#include "captive_portal.h"
#include "dns_server.h"
#include <esp_http_server.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "CAPTIVE_PORTAL";

// Nasza strona HTML zapisana jako stały tekst (const char)
// Styl: biało-miętowy / turkusowy, medyczny, czysty minimalizm.
// Czcionka systemowa (portal działa offline — Google Fonts by się nie wczytały).
static const char *html_page =
    "<!DOCTYPE html><html lang=\"pl\">"
    "<head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    "<title>Konfiguracja Radaru</title>"
    "<style>"
    ":root{--mint:#14b8a6;--mint-dark:#0d9488;--ink:#0f3d3a;--muted:#5b7d79;--line:#d7eeea;}"
    "*{box-sizing:border-box;}"
    "body{font-family:system-ui,-apple-system,'Segoe UI',Roboto,'Helvetica Neue',Arial,sans-serif;"
    "background:linear-gradient(160deg,#f0fdfa 0%,#e3faf4 55%,#ccfbf1 100%);color:var(--ink);"
    "display:flex;justify-content:center;align-items:center;min-height:100vh;margin:0;padding:24px;}"
    ".card{background:#ffffff;padding:32px 28px;border-radius:20px;border:1px solid var(--line);"
    "box-shadow:0 18px 40px -18px rgba(20,184,166,0.45);width:100%;max-width:380px;}"
    ".logo{width:58px;height:58px;margin:0 auto 14px;border-radius:18px;display:flex;align-items:center;justify-content:center;"
    "background:linear-gradient(135deg,var(--mint),var(--mint-dark));box-shadow:0 10px 22px -10px rgba(20,184,166,0.7);}"
    ".logo svg{width:34px;height:34px;}"
    "h2{text-align:center;margin:0 0 4px;font-size:22px;font-weight:700;letter-spacing:-0.02em;}"
    ".sub{text-align:center;margin:0 0 22px;font-size:13px;color:var(--muted);}"
    "label{display:block;margin-top:16px;margin-bottom:6px;font-size:13px;font-weight:600;color:var(--muted);}"
    "input{width:100%;padding:13px 14px;border:1.5px solid var(--line);border-radius:12px;background:#f8fffe;"
    "color:var(--ink);font-size:15px;transition:border-color .15s,box-shadow .15s;}"
    "input::placeholder{color:#9cc3bd;}"
    "input:focus{outline:none;border-color:var(--mint);box-shadow:0 0 0 4px rgba(20,184,166,0.15);background:#fff;}"
    "button{width:100%;padding:14px;margin-top:26px;border:none;border-radius:12px;"
    "background:linear-gradient(135deg,var(--mint),var(--mint-dark));color:#fff;font-weight:700;font-size:16px;"
    "cursor:pointer;transition:transform .08s,box-shadow .15s;box-shadow:0 10px 22px -10px rgba(20,184,166,0.7);}"
    "button:hover{box-shadow:0 14px 26px -10px rgba(20,184,166,0.8);}"
    "button:active{transform:translateY(1px);}"
    ".foot{text-align:center;margin-top:18px;font-size:12px;color:#9cc3bd;}"
    "</style></head>"
    "<body>"
    "<div class=\"card\">"
    "<div class=\"logo\">"
    "<svg viewBox=\"0 0 48 48\" fill=\"none\" stroke=\"#fff\" stroke-width=\"2.4\" "
    "stroke-linecap=\"round\" stroke-linejoin=\"round\">"
    "<path d=\"M5 24 C14 13 34 13 43 24 C34 35 14 35 5 24 Z\"/>"
    "<circle cx=\"24\" cy=\"24\" r=\"6\"/>"
    "<circle cx=\"24\" cy=\"24\" r=\"1.6\" fill=\"#fff\" stroke=\"none\"/>"
    "</svg></div>"
    "<h2>Konfiguracja Radaru</h2>"
    "<p class=\"sub\">Podaj dane sieci, do której ma się połączyć urządzenie</p>"
    "<form action=\"/save\" method=\"POST\">"
    "<label>Nazwa sieci (SSID)</label>"
    "<input type=\"text\" name=\"ssid\" placeholder=\"Twoja sieć Wi-Fi\" required>"
    "<label>Hasło Wi-Fi</label>"
    "<input type=\"password\" name=\"pass\" placeholder=\"Hasło do sieci\">"
    "<label>Adres MQTT (URI)</label>"
    "<input type=\"text\" name=\"mqtt\" value=\"mqtt://10.42.0.1:1883\" required>"
    "<button type=\"submit\">Zapisz i uruchom</button>"
    "</form>"
    "<p class=\"foot\">Radar XM125 &middot; ESP32-C6</p>"
    "</div></body></html>";


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

    const char *resp =
        "<!DOCTYPE html><html lang=\"pl\">"
        "<head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
        "<title>Zapisano</title>"
        "<style>"
        "body{font-family:system-ui,-apple-system,'Segoe UI',Roboto,Arial,sans-serif;"
        "background:#ffffff;color:#0f3d3a;display:flex;flex-direction:column;justify-content:center;"
        "align-items:center;min-height:100vh;margin:0;padding:24px;text-align:center;}"
        "h1{font-size:26px;font-weight:700;margin:0 0 10px;}"
        "p{font-size:15px;color:#5b7d79;margin:4px 0;}"
        "</style></head>"
        "<body>"
        "<h1>Wszystko gotowe</h1>"
        "<p>Ustawienia zostały zapisane.</p>"
        "<p>Urządzenie zaraz się zrestartuje i połączy z Twoją siecią.</p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();

    return ESP_OK;
}



// Handler "nie znaleziono". Telefon, sprawdzajac internet, puka pod rozne
// adresy (np. /generate_204, /hotspot-detect.html, /ncsi.txt). Zamiast bledu
// 404 odsylamy przekierowanie 302 na nasza strone - to wlasnie sygnal, ktory
// kaze systemowi telefonu pokazac okno captive portala.
static esp_err_t http_404_redirect(httpd_req_t *req, httpd_err_code_t err) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, NULL, 0);
    ESP_LOGI(TAG, "Przekierowanie na portal: %s", req->uri);
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

        // Wszystko inne (adresy wykrywania portalu) -> przekierowanie na "/".
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_redirect);

        ESP_LOGI(TAG, "Serwer WWW odpalony na porcie 80.");
    }

    // Bez DNS telefon nie wie, ze ma pytac nas - dlatego startujemy go tutaj.
    start_dns_server();
}

#include "wifi_mqtt.h"
#include "captive_portal.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "dhcpserver/dhcpserver.h"   // OFFER_DNS - opcja DHCP "podaj klientowi DNS"
#include <string.h>

static const char *TAG = "WIFI_MQTT";
/* Definicja zmiennej globalnej zadeklarowanej w wifi_mqtt.h */
esp_mqtt_client_handle_t mqtt_client = NULL;

/* Kolejka komend panel -> pętla radaru. Tworzona w app_main przed setup_mqtt(). */
QueueHandle_t mqtt_cmd_queue = NULL;

// FLAGA ssid, password
volatile bool wifi_ready = false;

// Flaga uzbrojenia alarmu — sterowana z panelu przez retained-temat
// MQTT_CMD_TOPIC. Czytana przez pętlę alarmu w main.c.
volatile bool alarm_armed = false;

// Flagi stanu dla sygnalizacji RGB (status_led.c).
volatile bool softap_mode = false;
volatile bool mqtt_connected = false;
volatile bool mqtt_ever_connected = false;

// Handler zdarzeń klienta MQTT: po połączeniu subskrybuje temat poleceń,
// a w danych przychodzących odczytuje flagę "armed".
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT połączony — subskrybuję %s oraz %s",
                 MQTT_CMD_TOPIC, MQTT_CFG_TOPIC);
        mqtt_connected = true;
        mqtt_ever_connected = true;
        esp_mqtt_client_subscribe(event->client, MQTT_CMD_TOPIC, 1);
        esp_mqtt_client_subscribe(event->client, MQTT_CFG_TOPIC, 1);
        break;

    case MQTT_EVENT_DISCONNECTED:
        mqtt_connected = false;
        break;

    case MQTT_EVENT_DATA: {
        // event->topic jest ważny tylko na 1. fragmencie; fragmentowanych nie
        // obsługujemy (payload cfg trzymamy < 1 KB, cmd jest malutki).
        if (event->total_data_len != event->data_len) {
            ESP_LOGW(TAG, "MQTT: pominięto fragmentowany payload (%d/%d B)",
                     event->data_len, event->total_data_len);
            break;
        }

        // Ani topic, ani payload z brokera nie są null-terminowane — porównujemy
        // po długości, kopiujemy po długości.
        const bool is_cmd = (event->topic_len == (int)strlen(MQTT_CMD_TOPIC) &&
                             memcmp(event->topic, MQTT_CMD_TOPIC, event->topic_len) == 0);
        const bool is_cfg = (event->topic_len == (int)strlen(MQTT_CFG_TOPIC) &&
                             memcmp(event->topic, MQTT_CFG_TOPIC, event->topic_len) == 0);

        if (is_cmd) {
            // Bez zmian względem faz < 3: prosty odczyt flagi "armed".
            // Przeniesienie na kolejkę = faza 4 (osobny commit).
            char buf[64] = {0};
            int n = event->data_len < (int)sizeof(buf) - 1
                        ? event->data_len : (int)sizeof(buf) - 1;
            memcpy(buf, event->data, n);
            alarm_armed = (strstr(buf, "true") != NULL);
            ESP_LOGI(TAG, "Polecenie z panelu: alarm %s",
                     alarm_armed ? "UZBROJONY" : "rozbrojony");
        } else if (is_cfg) {
            if (mqtt_cmd_queue == NULL) {
                ESP_LOGW(TAG, "cfg: kolejka jeszcze nie gotowa — pomijam.");
                break;
            }
            // Handler jest jednowątkowy per klient -> static zamiast ~560 B na stosie.
            // Publikacji z tego tasku NIE robimy (kontencja o mutex klienta MQTT
            // = udokumentowany deadlock) — acki wysyła pętla radaru.
            static mqtt_msg_t m;
            int tl = event->topic_len < (int)sizeof(m.topic) - 1
                         ? event->topic_len : (int)sizeof(m.topic) - 1;
            memcpy(m.topic, event->topic, tl);
            m.topic[tl] = '\0';
            int pl = event->data_len < (int)sizeof(m.payload) - 1
                         ? event->data_len : (int)sizeof(m.payload) - 1;
            memcpy(m.payload, event->data, pl);
            m.payload[pl] = '\0';
            m.payload_len = (uint16_t)pl;
            if (xQueueSend(mqtt_cmd_queue, &m, 0) != pdTRUE) {
                ESP_LOGW(TAG, "cfg: kolejka pełna — komunikat odrzucony.");
            }
        } else {
            ESP_LOGD(TAG, "MQTT DATA z nieobsługiwanego tematu (topic_len=%d)",
                     event->topic_len);
        }
        break;
    }

    default:
        break;
    }
}

// Handler zdarzeń Wi-Fi + IP  --- ALERTY
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Rozłączono z Wi-Fi. Próba ponownego połączenia...");
        wifi_ready = false;   // sygnalizacja RGB: powrót do "łączenie"
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Połączono z Wi-Fi! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_ready = true;
    }
}

// Odczyt NVS (true = mamy prawidłowe dane)
static bool load_credentials(char* ssid, char* pass) {
    nvs_handle_t my_handle;

    if (nvs_open("storage", NVS_READONLY, &my_handle) != ESP_OK) {
        ESP_LOGW(TAG, "Pamięć NVS pusta (brak partycji storage).");
        return false;
    }

    size_t ssid_len = 32;
    size_t pass_len = 64;
    size_t mqtt_len = 64; // <--- NOWE

    memset(ssid, 0, ssid_len);
    memset(pass, 0, pass_len);

    char temp_mqtt[64] = {0}; // <--- NOWE (Tymczasowy bufor tylko do logów)

    esp_err_t err_ssid = nvs_get_str(my_handle, "wifi_ssid", ssid, &ssid_len);
    esp_err_t err_pass = nvs_get_str(my_handle, "wifi_pass", pass, &pass_len);
    esp_err_t err_mqtt = nvs_get_str(my_handle, "mqtt_uri", temp_mqtt, &mqtt_len); // <--- NOWE

    nvs_close(my_handle);

    // ==========================================
    // LOGI DIAGNOSTYCZNE - WYPISANIE NA EKRAN
    // ==========================================
    ESP_LOGI(TAG, "===================================");
    ESP_LOGI(TAG, "   ODCZYT Z PAMIĘCI PO RESTARCIE   ");

    if (err_ssid == ESP_OK) ESP_LOGI(TAG, "SSID: '%s'", ssid);
    else ESP_LOGI(TAG, "SSID: <brak>");

    if (err_pass == ESP_OK) ESP_LOGI(TAG, "PASS: '%s'", pass);
    else ESP_LOGI(TAG, "PASS: <brak>");

    // NOWE LOGI DLA MQTT:
    if (err_mqtt == ESP_OK) ESP_LOGI(TAG, "MQTT: '%s'", temp_mqtt);
    else ESP_LOGI(TAG, "MQTT: <brak>");

    ESP_LOGI(TAG, "===================================");

    // ==========================================
    // PRAWDZIWY TEST WERYFIKACYJNY
    // ==========================================
    if (err_ssid == ESP_OK && strlen(ssid) > 0) {
        return true;
    }

    return false;
}

// 3. LOGOWANIE - dane w nvs (STA)
static void start_wifi_station(const char* ssid, const char* pass) {
    ESP_LOGI(TAG, "Uruchamiam tryb Klienta (STA). Łączenie z: %s", ssid);
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, 32);
    strncpy((char *)wifi_config.sta.password, pass, 64);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// REJESTRACJA - Hotspot (SoftAP)
static void start_wifi_softap(void) {
    ESP_LOGI(TAG, "Brak sieci w NVS. Uruchamiam tryb Hotspot (SoftAP).");
    softap_mode = true;   // sygnalizacja RGB: tryb konfiguracji (niebieski)
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "Konfiguracja_Radaru",
            .ssid_len = strlen("Konfiguracja_Radaru"),
            .channel = 1,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Serwer DHCP (rozdaje telefonom adresy IP) domyslnie NIE podaje DNS-a.
    // Wlaczamy te opcje i wskazujemy nas (192.168.4.1) jako serwer DNS, zeby
    // telefon kierowal swoje zapytania DNS do nas - i dzieki temu zadzialal
    // captive portal. Opcje DHCP wolno zmieniac tylko przy zatrzymanym serwerze.
    esp_netif_dns_info_t dns = {
        .ip.type = ESP_IPADDR_TYPE_V4,
        .ip.u_addr.ip4.addr = ESP_IP4TOADDR(192, 168, 4, 1),
    };
    dhcps_offer_t dhcps_flag_dns = OFFER_DNS;
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
    ESP_ERROR_CHECK(esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                                           ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &dhcps_flag_dns, sizeof(dhcps_flag_dns)));
    ESP_ERROR_CHECK(esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

    // start_web_server();
    start_captive_portal();
}

// Łączenie z wifi
void setup_wifi_esp_idf(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    char saved_ssid[32] = {0};
    char saved_pass[64] = {0};

    if (load_credentials(saved_ssid, saved_pass)) {
        start_wifi_station(saved_ssid, saved_pass);
    } else {
        start_wifi_softap();
    }
}

// logowanie do brokera i utrzymanie połącznia
void setup_mqtt(void)
{
    char saved_mqtt_uri[64] = {0};

    // 1. Otwieramy pamięć NVS i próbujemy wyciągnąć zapisany adres MQTT
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        size_t len = sizeof(saved_mqtt_uri);
        nvs_get_str(my_handle, "mqtt_uri", saved_mqtt_uri, &len);
        nvs_close(my_handle);
    }

    // 2. Zabezpieczenie inżynierskie: co jeśli NVS jest puste (ktoś nie wpisał adresu)?
    if (strlen(saved_mqtt_uri) == 0) {
        ESP_LOGW(TAG, "Brak MQTT URI w pamięci! Używam domyślnego awaryjnego...");
        strcpy(saved_mqtt_uri, "mqtt://10.42.0.1:1883"); // Twój awaryjny adres
    }

    ESP_LOGI(TAG, "Uruchamiam klienta MQTT. Docelowy adres: %s", saved_mqtt_uri);

    // 3. Konfigurujemy klienta znalezionym (lub awaryjnym) adresem.
    //    LWT na osobny temat (NIE cfg/state — inaczej offline nadpisałby
    //    retained snapshot configu, z którego wypełnia się panel). Pola
    //    last_will MUSZĄ być ustawione przed esp_mqtt_client_init().
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = saved_mqtt_uri,
        .session.last_will = {
            .topic   = MQTT_STATUS_TOPIC,
            .msg     = "{\"st\":\"offline\"}",
            .msg_len = 0,      // 0 => strlen(msg)
            .qos     = 1,
            .retain  = true,
        },
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    // Rejestrujemy handler, żeby po połączeniu zasubskrybować temat poleceń
    // i odbierać uzbrojenie/rozbrojenie alarmu z panelu.
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

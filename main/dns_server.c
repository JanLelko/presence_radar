#include "dns_server.h"

#include <string.h>
#include <sys/param.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "DNS_SERVER";

#define DNS_PORT     53
#define DNS_MAX_LEN  256

// W naglowku DNS bity 11-14 to "opcode". Standardowe zapytanie ma opcode 0.
#define OPCODE_MASK  0x7800
// Bit QR ustawiony = pakiet jest ODPOWIEDZIA (a nie pytaniem).
#define QR_FLAG      0x8000
// Typ rekordu A = zwykle zapytanie "jaki jest adres IPv4 tej domeny".
#define QD_TYPE_A    0x0001
#define ANS_TTL_SEC  300

// Naglowek pakietu DNS - zawsze 12 bajtow na poczatku.
typedef struct __attribute__((__packed__)) {
    uint16_t id;        // identyfikator zapytania (odsylamy taki sam)
    uint16_t flags;     // flagi (QR, opcode, itd.)
    uint16_t qd_count;  // ile jest pytan
    uint16_t an_count;  // ile jest odpowiedzi
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

// To, co stoi w pytaniu zaraz po nazwie domeny.
typedef struct __attribute__((__packed__)) {
    uint16_t type;
    uint16_t class;
} dns_question_t;

// Rekord odpowiedzi, ktory doklejamy: "ta domena ma adres ip_addr".
typedef struct __attribute__((__packed__)) {
    uint16_t ptr_offset; // wskaznik na nazwe domeny z pytania (kompresja)
    uint16_t type;
    uint16_t class;
    uint32_t ttl;
    uint16_t addr_len;
    uint32_t ip_addr;
} dns_answer_t;

// Rozkodowuje nazwe domeny zapisana w formacie DNS (3www6google3com0)
// na czytelny tekst "www.google.com". Zwraca wskaznik na bajt zaraz za nazwa.
static char *parse_dns_name(char *raw_name, char *parsed_name, size_t max_len)
{
    char *label = raw_name;
    char *out = parsed_name;
    int total = 0;

    do {
        int sub_len = *label;             // pierwszy bajt = dlugosc etykiety
        total += (sub_len + 1);
        if (total > max_len) {
            return NULL;
        }
        memcpy(out, label + 1, sub_len);  // skopiuj etykiete bez bajtu dlugosci
        out[sub_len] = '.';
        out += (sub_len + 1);
        label += sub_len + 1;
    } while (*label != 0);

    parsed_name[total - 1] = '\0';        // zamien ostatnia '.' na koniec stringa
    return label + 1;                     // przeskocz konczace zero
}

// Z otrzymanego pytania DNS buduje odpowiedz kierujaca na nasz adres IP.
// Zwraca dlugosc gotowej odpowiedzi, albo <=0 przy bledzie.
static int build_dns_reply(char *req, size_t req_len, char *reply, size_t reply_max)
{
    if (req_len > reply_max) {
        return -1;
    }

    memset(reply, 0, reply_max);
    memcpy(reply, req, req_len);          // zaczynamy od kopii pytania

    dns_header_t *header = (dns_header_t *)reply;

    // Obslugujemy tylko zwykle zapytania.
    if ((ntohs(header->flags) & OPCODE_MASK) != 0) {
        return 0;
    }

    header->flags |= htons(QR_FLAG);                 // oznacz pakiet jako odpowiedz
    uint16_t qd_count = ntohs(header->qd_count);
    header->an_count = htons(qd_count);              // tyle odpowiedzi, ile pytan

    int reply_len = qd_count * sizeof(dns_answer_t) + req_len;
    if (reply_len > reply_max) {
        return -1;
    }

    // Adres IP naszego interfejsu AP - nim odpowiadamy na wszystko.
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info);

    char *cur_qd = reply + sizeof(dns_header_t); // pierwsze pytanie po naglowku
    char *cur_ans = reply + req_len;             // odpowiedzi doklejamy na koniec
    char name[128];

    for (int i = 0; i < qd_count; i++) {
        char *after_name = parse_dns_name(cur_qd, name, sizeof(name));
        if (after_name == NULL) {
            ESP_LOGW(TAG, "Nie udalo sie odczytac nazwy z pytania DNS");
            return -1;
        }

        dns_question_t *question = (dns_question_t *)after_name;
        uint16_t qd_type = ntohs(question->type);

        if (qd_type == QD_TYPE_A) {
            dns_answer_t *answer = (dns_answer_t *)cur_ans;
            // 0xC000 | offset = "nazwa jest tam, gdzie w pytaniu" (kompresja DNS)
            answer->ptr_offset = htons(0xC000 | (cur_qd - reply));
            answer->type = htons(QD_TYPE_A);
            answer->class = htons(1);            // klasa IN (internet)
            answer->ttl = htonl(ANS_TTL_SEC);
            answer->addr_len = htons(sizeof(answer->ip_addr));
            answer->ip_addr = ip_info.ip.addr;   // juz w kolejnosci sieciowej
            ESP_LOGD(TAG, "DNS: '%s' -> " IPSTR, name, IP2STR(&ip_info.ip));
        }

        cur_qd = (char *)question + sizeof(dns_question_t);
        cur_ans += sizeof(dns_answer_t);
    }
    return reply_len;
}

static void dns_server_task(void *pvParameters)
{
    char rx_buffer[DNS_MAX_LEN];
    char reply[DNS_MAX_LEN];

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(DNS_PORT);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Nie udalo sie utworzyc gniazda: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    if (bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        ESP_LOGE(TAG, "Nie udalo sie podpiac gniazda do portu %d: errno %d", DNS_PORT, errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Serwer DNS slucha na porcie %d", DNS_PORT);

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0,
                           (struct sockaddr *)&source_addr, &socklen);
        if (len < 0) {
            ESP_LOGE(TAG, "recvfrom blad: errno %d", errno);
            break;
        }

        int reply_len = build_dns_reply(rx_buffer, len, reply, sizeof(reply));
        if (reply_len > 0) {
            sendto(sock, reply, reply_len, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
        }
    }

    close(sock);
    vTaskDelete(NULL);
}

void start_dns_server(void)
{
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, NULL);
}

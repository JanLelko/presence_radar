#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Uruchamia serwer DNS (UDP, port 53), ktory na KAZDE zapytanie o dowolna
// domene odpowiada adresem IP naszego hotspota (192.168.4.1). Dzieki temu
// telefon, sprawdzajac dostep do internetu, trafia na nasz serwer WWW i sam
// pokazuje okno z konfiguracja (captive portal). Wywolac dopiero gdy AP dziala.
void start_dns_server(void);

#ifdef __cplusplus
}
#endif

# TODO — Radar XM125 / ESP32-C6

Stan: **działający prototyp** (Captive Portal + STA + MQTT + dashboard).
Cel tego pliku: zaplanować usprawnienia **nic nie psując**. Każdy punkt ma etykietę ryzyka.

Legenda ryzyka:
- 🟢 **ADDITIVE** — dodaje nowy kod obok istniejącego, nie rusza działającej ścieżki łączenia. Bezpieczne do wdrożenia po kolei.
- 🟠 **RYZYKO** — modyfikuje sprawdzoną ścieżkę (event handler, reconnect, start systemu). Wdrażać ostrożnie, najlepiej na osobnej gałęzi i z testem cofnięcia.

---

## ✅ Zrobione (czerwiec 2026)

- **Auto-popup portalu (pkt 1)** — własny serwer DNS (`dns_server.c`) kieruje cały ruch na `192.168.4.1`,
  a handler 404 przekierowuje sondy wykrywania (`/generate_204`, `/hotspot-detect.html`, `/ncsi.txt`…) na portal (302).
- **Reset konfiguracji przyciskiem (wariant pkt 2a)** — przytrzymanie GPIO9 ~3 s kasuje sieć/MQTT z NVS i restartuje
  do portalu (3× sygnał LED+buzzer). Powrót do konfiguracji bez reflashu.
- **Rozróżnienie offline ≠ pusty pokój (pkt 5)** — dashboard ma watchdog: brak danych >10 s lub pad brokera
  wyszarza kartę i pokazuje **OFFLINE**.
- **Alarm „brak obecności / utrata łączności” (nowe)** — przełącznik **Alarm** w panelu; po uzbrojeniu dźwięk
  (Web Audio) + czerwony baner, gdy nikogo nie ma pod czujnikiem albo gdy znika łączność. W całości po stronie przeglądarki.
- **Nowy wygląd Captive Portala** — biało-miętowy/turkusowy, minimalistyczny, ikona oka (`icon.svg`),
  osobna biała strona-potwierdzenie po zapisie.
- **Sprzątanie (część pkt 6)** — usunięta martwa `wifi_event_handler` z `main.c`.
- **`.gitignore` dla artefaktów KiCada** — autosave/backupy/cache/locki nie są już wersjonowane.
- **Sygnalizacja RGB stanu (`status_led.c`)** — wbudowany WS2812 (GPIO8) pokazuje stan: portal/Wi-Fi/MQTT/alarm/błąd radaru.
- **Synchronizacja migania diod** — dioda na PCB (GPIO18) miga teraz w lockstep z RGB (ta sama częstotliwość i faza);
  GPIO18 przejęte przez `status_led.c`, w `main.c` została tylko obsługa buzzera.
- **Dashboard: wykres jedzie na 0 przy offline** — watchdog dopisuje zerowe próbki, gdy czujnik milczy (zamiast zamrażać wykres).
- **Dashboard: godzina 24h** — wspólny `timeLabel()` (`pl-PL`, bez AM/PM) na wykresie i w logach.
- **Dashboard: chowane mini-wykresy temp + dystans** — osobne skale, domyślnie ukryte (przycisk `＋ temp / dystans`).
- **Próg POŻARU demonstracyjny** — `MAX_TEMP` obniżone do 20 °C (wyzwalane chuchnięciem); do kalibracji wg odczytu spoczynkowego.

---

## ⚠️ Najpierw przeczytaj: HTTPS na Captive Portal to pułapka

Życzenia „HTTPS” i „automatyczny popup z linkiem po połączeniu” **są ze sobą w sprzeczności**:

- Wykrywanie portalu przez system (to ono otwiera popup „Zaloguj się do sieci”) działa po **czystym HTTP**:
  Android pyta `http://.../generate_204`, Apple `.../hotspot-detect.html`, Windows `.../ncsi.txt`.
- Portal na **HTTPS z certyfikatem self-signed** nie odpowie na te sondy czysto → zamiast popupu telefon
  pokaże ostrzeżenie o certyfikacie. Do tego dochodzi koszt RAM/flash na C6.

**Rekomendacja:** portal zostaw na HTTP (żeby popup działał). Jeśli problemem jest hasło Wi-Fi lecące
otwartą siecią — lepszym rozwiązaniem niż HTTPS jest **hasło WPA2 na samym AP** (dziś `WIFI_AUTH_OPEN`):
szyfruje cały ruch, odsiewa obcych i **nie psuje** popupu. Decyzja należy do Ciebie — ale HTTPS nie jest „za darmo”.

---

## 1. Auto-popup portalu po połączeniu („link na telefonie”) — NAJWIĘKSZY zysk UX

Dziś po wejściu do sieci `Konfiguracja_Radaru` trzeba **ręcznie wpisać IP** w przeglądarce — bo brak serwera DNS
i brak endpointów wykrywania. To jest dokładnie to, czego brakuje do „alertu z linkiem”.

- [x] 🟢 ✅ Serwer **DNS** kierujący każde zapytanie na IP AP (`192.168.4.1`) — `dns_server.c`.
- [x] 🟢 ✅ Wykrywanie portalu: catch-all handler 404 → przekierowanie (302) na `/` (łapie `/generate_204`,
      `/hotspot-detect.html`, `/ncsi.txt`, `/connecttest.txt` itd.) — `captive_portal.c`.
- [~] 🟢 (N/A) Zamiast przykładu ESP-IDF napisano własny, lekki DNS + redirect — działa, temat zamknięty.
- [ ] 🟢 (Opcjonalnie, do weryfikacji) Nowsze iOS/Android wspierają **RFC 8910 / DHCP option 114**
      (URI portalu podany przez DHCP). Sprawdzić, czy nasza wersja ESP-IDF to udostępnia — to czystszy popup.
      Traktować jako „sprawdzić”, nie jako pewne API.

Efekt: telefon po połączeniu sam pokaże stronę konfiguracji — bez wpisywania IP.

---

## 2. „Płynniejsze i szybsze łączenie z Wi-Fi” + odporność

### 2a. 🟠 Martwy zaułek przy złym haśle / przeniesieniu urządzenia — NAJWAŻNIEJSZE
Dziś `wifi_event_handler` na każde rozłączenie woła `esp_wifi_connect()` **w nieskończoność**, a tryb AP/portal
startuje **tylko gdy NVS jest puste** (`wifi_mqtt.c`). Skutek: złe hasło albo zmiana routera = pętla bez wyjścia,
`wifi_ready` nigdy `true`, MQTT zablokowane na zawsze, **powrót do portalu wymaga reflashu**.

- [ ] 🟠 Dodać licznik prób (np. 5–10) → po przekroczeniu **fallback do SoftAP/portalu** zamiast pętli.
      **Wciąż otwarte i nadal najważniejsze** — reset przyciskiem nie zastępuje automatycznego fallbacku przy złym haśle.
- [x] 🟢 ✅ **Wejście do portalu przyciskiem** — przytrzymanie GPIO9 ~3 s kasuje SSID/hasło/MQTT z NVS i restartuje
      do Captive Portala (3× LED+buzzer). Zrobione w `main.c` (runtime, nie tylko przy starcie).

### 2b. 🟠 Ślepe `vTaskDelay(3000)` w `app_main` przed I2C
`main.c` czeka na sztywno 3 s licząc, że Wi-Fi wstanie. Czasem za mało (wolny DHCP), czasem niepotrzebnie wolno.

- [ ] 🟠 Zamienić na oczekiwanie na **EventGroup** (bit ustawiany w `IP_EVENT_STA_GOT_IP`) z timeoutem.
      Płynniejszy start, ale to ścieżka startowa — testować ostrożnie.

### 2c. 🟢 Drobiazgi łączenia
- [ ] 🟢 Rozważyć `wifi_config.sta.pmf_cfg` / szybsze skanowanie (`scan_method`, `sort_method`) dla szybszego STA.
- [ ] 🟢 Upewnić się, że `strncpy` SSID/hasła zawsze zostawia `\0` (przy dokładnie 32/64 znakach brak terminatora).

---

## 3. „Otwieranie strony na telefonie” — co dokładnie otwierać?

Doprecyzowanie: są dwie różne strony. Warto zaplanować obie.

- [ ] 🟢 **(a) Strona konfiguracji portalu** — patrz pkt 1 (auto-popup).
- [ ] 🟢 **(b) Live dashboard z radaru** — dziś `dashboard/index.html` to plik `file://` łączący się z brokerem
      przez MQTT-over-WebSockets. Opcja: **serwować dashboard prosto z ESP** (mały serwer HTTP w trybie STA),
      żeby telefon w tej samej sieci widział status pod `http://<ip-esp>/` bez brokera i bez pliku.
      ADDITIVE — osobny serwer/endpoint, nie rusza MQTT.

---

## 4. Bezpieczeństwo (powiązane z wątkiem „HTTPS”)
- [ ] 🟢 Ustawić **hasło WPA2 na AP** konfiguracyjnym (dziś `WIFI_AUTH_OPEN`). Realna ochrona hasła Wi-Fi
      w trakcie konfiguracji — zamiast HTTPS. (patrz sekcja u góry)
- [ ] 🟢 (Dalej) Jeśli kiedyś end-to-end TLS: dashboard przez `https` + MQTT przez `wss`/`mqtts`. Duży zakres, niski priorytet.

---

## 5. Z poprzedniego TODO (zachowane)
- [x] 🟢 ✅ **Rozróżnienie „brak łączności” ≠ „pusty pokój”.** Dashboard ma watchdog: timestamp ostatniej
      wiadomości per pokój, brak danych >10 s lub pad brokera → karta **OFFLINE** (wyszarzona) — `dashboard/index.html`.
      Dorzucony przełącznik **Alarm**: po uzbrojeniu dźwięk + baner przy braku obecności LUB utracie łączności.

---

## 6. Sprzątanie kodu (kosmetyka, nie pilne)
- [x] 🟢 ✅ Usunięto martwą, zduplikowaną `wifi_event_handler` z `main.c` (żywa wersja jest w `wifi_mqtt.c`).
- [ ] 🟢 Usunąć diagnostyczne `ESP_LOGI` wypisujące **hasło Wi-Fi otwartym tekstem** w `load_credentials`
      (`wifi_mqtt.c`) — wyciek w logu serialowym.
- [ ] 🟢 Zakomentowane `// start_web_server();` w `wifi_mqtt.c` — uprzątnąć.

---

### Sugerowana kolejność tego, co ZOSTAŁO (najpierw bezpieczne, największy zysk)
1. 🟢 Pkt 6 (reszta) — usunąć log hasła otwartym tekstem w `load_credentials` + zakomentowane `// start_web_server();`. Szybkie, zamyka wyciek.
2. 🟢 Pkt 4 — hasło WPA2 na AP (zamiast HTTPS).
3. 🟠 Pkt 2a — **licznik prób → fallback do portalu** (najważniejsza odporność; reset przyciskiem to nie to samo).
4. 🟢 Pkt 3b — serwowanie dashboardu prosto z ESP.
5. 🟠 Pkt 2b — EventGroup zamiast `delay(3000)`.
6. 🟢 Pkt 2c — drobiazgi łączenia (`strncpy` `\0`, szybsze skanowanie STA).

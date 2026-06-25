# TODO — Radar XM125 / ESP32-C6

Stan: **działający prototyp** (Captive Portal + STA + MQTT + dashboard).
Cel tego pliku: zaplanować usprawnienia **nic nie psując**. Każdy punkt ma etykietę ryzyka.

Legenda ryzyka:
- 🟢 **ADDITIVE** — dodaje nowy kod obok istniejącego, nie rusza działającej ścieżki łączenia. Bezpieczne do wdrożenia po kolei.
- 🟠 **RYZYKO** — modyfikuje sprawdzoną ścieżkę (event handler, reconnect, start systemu). Wdrażać ostrożnie, najlepiej na osobnej gałęzi i z testem cofnięcia.

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

- [ ] 🟢 Dodać serwer **DNS**, który każde zapytanie kieruje na IP AP (`192.168.4.1`).
- [ ] 🟢 Dodać handlery HTTP wykrywania portalu: `/generate_204`, `/hotspot-detect.html`, `/ncsi.txt`,
      `/connecttest.txt` → przekierowanie (302) na `/`.
- [ ] 🟢 Najprościej: oprzeć się o oficjalny przykład ESP-IDF **`protocols/http_server/captive_portal`**
      (ma już gotowy DNS→AP + endpointy). Skopiować wzorzec zamiast pisać własny hijack UDP DNS od zera.
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
- [ ] 🟢 Alternatywnie/dodatkowo: **przytrzymanie BOOT przy starcie** wymusza tryb konfiguracji (czyści/omija NVS).
      To wariant ADDITIVE — nie rusza logiki reconnectu, tylko dodaje wejście do portalu.

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
- [ ] 🟢 **Komunikat „błąd połączenia” zamiast „brak obecności”.** Gdy brak danych / stare dane z brokera,
      dashboard interpretuje to jako „brak obecności”. Dodać rozróżnienie: brak łączności ≠ pusty pokój
      (np. timestamp ostatniej wiadomości + status „offline” na karcie pokoju).

---

## 6. Sprzątanie kodu (kosmetyka, nie pilne)
- [ ] 🟢 W `main.c` jest **martwa, zduplikowana** funkcja `wifi_event_handler` (linie ~32–42) — nigdzie nie
      rejestrowana; żywa wersja jest w `wifi_mqtt.c`. Usunąć, żeby nie mylić.
- [ ] 🟢 Usunąć diagnostyczne `ESP_LOGI` wypisujące **hasło Wi-Fi otwartym tekstem** w `load_credentials`
      (`wifi_mqtt.c`) — wyciek w logu serialowym.
- [ ] 🟢 Zakomentowane `// start_web_server();` w `wifi_mqtt.c` — uprzątnąć.

---

### Sugerowana kolejność (najpierw bezpieczne, największy zysk)
1. 🟢 Pkt 1 — DNS + endpointy wykrywania (auto-popup). Największy efekt „wow”, zero ryzyka dla STA.
2. 🟢 Pkt 4 — hasło WPA2 na AP (zamiast HTTPS).
3. 🟠 Pkt 2a — fallback z pętli reconnectu (najważniejsza odporność, ale rusza ścieżkę połączenia → osobna gałąź).
4. 🟢 Pkt 3b / Pkt 5 — hosting dashboardu + rozróżnienie offline.
5. 🟠 Pkt 2b — EventGroup zamiast `delay(3000)`.
6. 🟢 Pkt 6 — sprzątanie.

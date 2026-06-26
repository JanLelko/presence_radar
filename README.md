# Radar obecności XM125 + ESP32-C6

Firmware ESP-IDF dla **ESP32-C6**, które czyta moduł radaru **Acconeer XM125**
(czujnik A121 + STM32) po I2C, stosuje lokalne reguły *edge computing*
(obecność / upadek / przegrzanie / zasiedzenie) i publikuje status w formacie
JSON do brokera **MQTT**. Statyczny dashboard (`dashboard/index.html`)
subskrybuje broker przez MQTT-over-WebSockets i pokazuje karty pokoi.

Konfiguracja Wi-Fi i adresu brokera odbywa się przez **Captive Portal** —
przy pierwszym uruchomieniu (albo po resecie konfiguracji) urządzenie stawia
własny hotspot, na którym z telefonu wpisujesz sieć i adres MQTT. Nic nie jest
zaszyte na sztywno w kodzie.

> Cały opis dotyczy Linuksa (Fedora/Ubuntu). Target to ESP32-C6 (RISC-V) —
> **nie** domyślny ESP32.

---

## 📑 Spis treści

1. [Podłączenie sprzętowe](#-1-podłączenie-sprzętowe-hardware)
2. [Wgrywanie firmware na radar (STM32)](#%EF%B8%8F-2-wgrywanie-firmware-na-radar-stm32)
3. [Test radaru na PC (opcjonalnie)](#-3-test-radaru-na-pc-opcjonalnie)
4. [Protokół I2C radaru](#-4-protokół-i2c-radaru)
5. [Budowanie i wgrywanie na ESP32-C6](#-5-budowanie-i-wgrywanie-na-esp32-c6)
6. [Pierwsze uruchomienie: Captive Portal](#-6-pierwsze-uruchomienie-captive-portal)
7. [Broker MQTT i dashboard](#-7-broker-mqtt-i-dashboard)
8. [Kalibracja i logika alarmów](#-8-kalibracja-i-logika-alarmów)
9. [Struktura projektu](#-9-struktura-projektu)
10. [Rozwiązywanie problemów](#-10-rozwiązywanie-problemów-troubleshooting)
11. [Linki](#-11-linki)

---

## 🔌 1. Podłączenie sprzętowe (Hardware)

Najważniejsza rzecz to pin **WAKE_UP**. Bez podciągnięcia go do 3.3V STM32 na
module śpi i odpowiada na I2C samymi zerami — co łatwo pomylić z błędem okablowania.

| ESP32-C6        | Moduł / element | Opis                                                              |
| :-------------- | :-------------- | :---------------------------------------------------------------- |
| **5V / VBUS**   | Radar 5V        | Zasilanie główne modułu                                           |
| **GND**         | Radar GND       | Masa                                                              |
| **GPIO 2**      | Radar SDA       | Linia danych I2C                                                  |
| **GPIO 11**     | Radar SCL       | Linia zegara I2C                                                  |
| **3.3V (VCC)**  | Radar WAKE_UP   | **KRYTYCZNE:** wymusza wybudzenie STM32 z Deep Sleep              |
| GPIO 10         | Radar INT       | Wyjście INT z radaru (zarezerwowane, na razie nieużywane w kodzie)|
| GPIO 18         | LED             | Sygnalizacja (np. potwierdzenie resetu konfiguracji)             |
| GPIO 9          | Przycisk        | Między GPIO9 a GND, wewnętrzny pull-up (wciśnięty = stan niski)   |
| GPIO 3          | Buzzer          | Sygnał dźwiękowy                                                  |

> Piny I2C są zdefiniowane na górze `main/main.c` (`I2C_MASTER_SDA_IO`,
> `I2C_MASTER_SCL_IO`). Jeśli zmienisz okablowanie — popraw tam.

---

## ⚡️ 2. Wgrywanie firmware na radar (STM32)

Żeby STM32 na module działał jako serwer I2C „Presence Detector”, trzeba go
najpierw sflashować oficjalnym narzędziem **Acconeer Exploration Tool**.

### Środowisko Python

Nie instaluj paczek globalnie — użyj dedykowanego venv:

```bash
mkdir -p ~/main/tools && cd ~/main/tools
python3 -m venv .venv
source .venv/bin/activate          # source wpisz w folderze, w którym jest .venv
pip install "acconeer-exptool[app]"
```

### Tryb bootloadera (hardware)

Zanim wgrasz soft, wprowadź moduł w tryb pobierania:

1. Wciśnij i **trzymaj** przycisk `BOOT`.
2. Kliknij raz `RST` (Reset).
3. Puść `BOOT`.

### Flashowanie przez aplikację

1. Uruchom narzędzie: `python -m acconeer.exptool.app`
2. Wejdź w zakładkę **Flash**.
3. Wybierz port (np. `/dev/ttyACM0`). Jeśli lista jest pusta, użyj **„Show all devices”**.
4. Wgraj firmware (np. `i2c_presence_detector`).

---

## 📊 3. Test radaru na PC (opcjonalnie)

Zanim przejdziesz do ESP32, możesz sprawdzić, czy radar w ogóle widzi obiekty:

1. **Zmień port** — układ CH342 tworzy dwa porty (np. `ACM0` i `ACM1`). Do
   odczytu danych użyj **drugiego** portu.
2. **Zmień baudrate** na `921600` (inaczej aplikacja zgłosi timeout).
3. Zresetuj moduł (`RST`).
4. Połącz się, wybierz konfigurację i kliknij **Start Measurement**.

---

## 🧠 4. Protokół I2C radaru

Moduł działa jako I2C **slave** pod adresem **`0x52`**:

- adresy rejestrów są **16-bitowe** (2 bajty, MSB first),
- wartości rejestrów są **32-bitowe big-endian** (4 bajty),
- każdy odczyt/zapis musi obejmować **wielokrotność 4 bajtów** danych.

### Najważniejsze rejestry

| Hex      | Dec | Nazwa     | R/W | Opis                                                            |
| :------- | :-- | :-------- | :-- | :------------------------------------------------------------- |
| `0x0000` | 0   | Wersja    | R   | Wersja firmware. Same zera ⇒ **WAKE_UP nie podłączony**        |
| `0x0003` | 3   | Status    | R   | Status detektora (`0xFF` = gotowy, ale nie wystartowany)        |
| `0x0010` | 16  | Wynik     | R   | Pierwszy z 4 rejestrów wyniku (obecność/dystans/intra/inter)    |
| `0x0100` | 256 | Komendy   | W   | Sterowanie (`1`=apply config, `2`=start, `3`=stop)              |

### Procedura startowa (init)

Radar po włączeniu zasilania nie mierzy — czeka na komendy. Pod rejestr `0x0100`
trzeba wysłać:

1. **Apply config** — wartość `1`.
2. **Start** — wartość `2`.

Adres idzie jako 2 bajty, wartość jako 4 bajty:

```c
// Komenda START (2) do rejestru 0x0100
uint8_t cmd_start[6] = {
    0x01, 0x00,             // adres rejestru 0x0100
    0x00, 0x00, 0x00, 0x02  // wartość (4 bajty, big-endian)
};
i2c_master_write_to_device(I2C_NUM_0, 0x52, cmd_start, 6, 1000);
```

### Odczyt wyników

Po starcie pytamy o rejestr `0x0010` i czytamy **16 bajtów** z rzędu — cztery
kolejne u32 big-endian:

| Bajty | Pole            | Znaczenie                                                  |
| :---- | :-------------- | :--------------------------------------------------------- |
| 0–3   | `presence_reg`  | bit 0 = obecność; **górne 16 bitów = temperatura chipa**   |
| 4–7   | `distance_mm`   | wykryta odległość [mm]                                     |
| 8–11  | `intra_score`   | energia ruchu szybkiego (mikro-ruch / dynamika)            |
| 12–15 | `inter_score`   | energia ruchu wolnego (makro-ruch / obecność statyczna)    |

---

## 💻 5. Budowanie i wgrywanie na ESP32-C6

W **każdym nowym terminalu** najpierw aktywuj ESP-IDF:

```bash
. $HOME/main/tools/esp-idf/export.sh
```

Build/flash/monitor uruchamiaj z **korzenia projektu** (tam, gdzie główny
`CMakeLists.txt` — **nie** z `main/`):

```bash
idf.py set-target esp32c6                 # raz na świeży katalog build
idf.py -p /dev/ttyACM0 build flash monitor
```

Wyjście z monitora: `Ctrl + ]`.

Jeśli po przebudowie / zmianie targetu sypią się błędy CMake — `rm -rf build/`
i kompiluj od nowa.

> Skrót: `./quickstart.sh` ubija zombie-brokery, startuje Mosquitto w nowym
> oknie, ładuje ESP-IDF i otwiera dashboard.

---

## 📶 6. Pierwsze uruchomienie: Captive Portal

Dane sieci i adres brokera **nie są** w kodzie — wpisujesz je przez portal.

1. Jeśli NVS jest puste, urządzenie stawia otwarty hotspot **`Konfiguracja_Radaru`**
   (SoftAP, `192.168.4.1`).
2. Połącz się telefonem z tym hotspotem. Wbudowany serwer DNS kieruje cały ruch
   na portal — na większości telefonów strona konfiguracji otworzy się sama.
   Jeśli nie, wejdź w przeglądarce na `http://192.168.4.1`.
3. Wpisz **SSID** i **hasło** swojej sieci Wi-Fi oraz **adres brokera MQTT**
   (np. `mqtt://10.42.0.1:1883`). Dane lądują w NVS.
4. Urządzenie restartuje się, łączy w trybie klienta (STA) i zaczyna publikować.

### Reset konfiguracji

Przytrzymaj **przycisk (GPIO9) ~3 s** — urządzenie potrójnie mrugnie LED-em
i piknie buzzerem, skasuje zapisaną sieć/MQTT z NVS i zrestartuje się z powrotem
do Captive Portalu. Reflash nie jest potrzebny.

> Awaryjny adres brokera (gdy w NVS brak MQTT URI) to `mqtt://10.42.0.1:1883`,
> zdefiniowany w `main/wifi_mqtt.c`.

---

## 🛰️ 7. Broker MQTT i dashboard

### Broker (Mosquitto)

Repo zawiera `mosquitto.conf`, który otwiera **1883 (MQTT)** i **9001
(WebSockets)** — listener WebSocket jest wymagany, bo `dashboard/index.html`
łączy się z brokerem prosto z przeglądarki przez `mqtt.min.js`.

```bash
mosquitto -c mosquitto.conf -v
```

Adres brokera musi być osiągalny dla ESP32 — domyślnie `10.42.0.1` (IP laptopa
udostępniającego hotspot). Ten adres wpisujesz w portalu (pkt 6).

### Dashboard

Nie trzeba serwera HTTP — otwórz plik bezpośrednio:

```bash
firefox dashboard/index.html
```

### Format wiadomości

Firmware publikuje na temat **`pokoj/101/radar`** JSON:

```json
{"obecnosc": 1, "odleglosc": 2310, "mikro": 1450, "makro": 220, "temp": 41, "alarm": "OK"}
```

(`mikro` = `inter_score`, `makro` = `intra_score`, `temp` = temperatura chipa °C,
`alarm` = `OK` / `POZAR` / `UPADEK` / `ZASIEDZENIE`.)

Publikacja jest dławiona do **raz na 2 s**, chyba że `alarm` ≠ `OK` — wtedy
wiadomość leci natychmiast. Podgląd bez dashboardu:

```bash
mosquitto_sub -h 127.0.0.1 -t 'pokoj/101/radar' -v
```

### Alarm „brak obecności” / utrata łączności

W nagłówku panelu jest przełącznik **Alarm**. Po uzbrojeniu dashboard reaguje
dźwiękiem (Web Audio — bez plików) i czerwonym banerem, gdy:

- **nikogo nie ma pod czujnikiem** — `obecnosc == 0` utrzymuje się dłużej niż
  krótka karencja (filtruje chwilowe mrugnięcia radaru), **albo**
- **znika łączność** — broker padł lub czujnik milczy > 10 s. Karty pokoi
  przechodzą wtedy w stan **OFFLINE** (wyszarzone), żeby odróżnić „pusty pokój”
  od „brak danych”.

Całość działa po stronie przeglądarki — bez zmian we firmware. Pierwsze kliknięcie
przełącznika odblokowuje dźwięk (polityka autoplay przeglądarki).

---

## 🎚️ 8. Kalibracja i logika alarmów

Stałe w `main/main.c`:

| Stała         | Domyślnie | Opis                                                                  |
| :------------ | :-------- | :-------------------------------------------------------------------- |
| `PODLOGA_MM`  | `2500`    | Odległość radaru od podłogi [mm] — używane przy detekcji upadku       |
| `PROG_UPADKU` | `1500`    | Min. `intra_score` (dynamika ruchu), by uznać upadek                  |
| `MAX_TEMP`    | `60`      | Próg temperatury chipa [°C] dla alarmu „POZAR”                        |

Klasyfikator (pętla co 250 ms) ustawia jeden ze stanów:

- **`POZAR`** — `temp > MAX_TEMP`.
- **`UPADEK`** — `intra_score > PROG_UPADKU` **oraz** `distance_mm > PODLOGA_MM − 400`
  (gwałtowny ruch przy podłodze).
- **`ZASIEDZENIE`** — obecność z niskim `intra_score` i wysokim `inter_score`
  utrzymująca się długo (licznik > 1200 pętli ≈ 5 min bez wyraźnego ruchu).
- **`OK`** — pozostałe przypadki.

---

## 📁 9. Struktura projektu

```
presence_radar/
├── main/
│   ├── main.c            # app_main: I2C, pętla odczytu radaru, logika alarmów, MQTT
│   ├── radar.c / .h      # warstwa I2C radaru XM125
│   ├── wifi_mqtt.c / .h  # Wi-Fi (STA/SoftAP), NVS, klient MQTT, wybór trybu
│   ├── captive_portal.c  # serwer HTTP portalu konfiguracji (styl miętowy, ikona oka)
│   ├── dns_server.c / .h # DNS przekierowujący ruch na portal (192.168.4.1)
│   └── CMakeLists.txt
├── dashboard/
│   ├── index.html        # panel pielęgniarki (MQTT-over-WebSockets) + przełącznik Alarm
│   ├── mqtt.min.js
│   └── chart.js.js
├── icon.svg              # logo projektu — minimalistyczne oko (używane też w portalu)
├── hardware/             # materiały sprzętowe (radar_xm125_esp32c6)
├── examples/             # standalone'y do bring-upu (nie wchodzą do builda)
├── components/           # placeholdery pod przyszły refaktor (puste)
├── mosquitto.conf        # broker: porty 1883 + 9001 (WebSockets)
├── quickstart.sh         # skrót: broker + ESP-IDF + dashboard jednym poleceniem
└── CMakeLists.txt        # top-level (uruchamiaj idf.py stąd)
```

> Stan: **działający prototyp** (Captive Portal + STA + MQTT + dashboard).
> Plany rozwoju i znane ograniczenia są w [`TODO.md`](TODO.md).

---

## 🚨 10. Rozwiązywanie problemów (Troubleshooting)

**1. `Permission denied` na `/dev/ttyACM0`**
Brak uprawnień w systemie. Nadaj ręcznie:
```bash
sudo chmod 666 /dev/ttyACM0
```

**2. Błędy kompilacji (`Unknown CMake command` / parse error)**
`idf.py` odpalony z niewłaściwego katalogu. Uruchamiaj go z korzenia projektu
(tam, gdzie główny `CMakeLists.txt`), nie z `main/`. W razie kryzysu:
`rm -rf build/` i od nowa.

**3. I2C zwraca same zera (`00 00 00 00`)**
STM32 śpi. Upewnij się, że pin **WAKE_UP** modułu jest połączony z **3.3V** ESP32.

**4. Dziwne odległości (np. `4008635904 mm`) lub brak reakcji na ruch**
Elektronika zasilona, ale antena nie wystartowała (pamięć zwraca śmieci).
Sprawdź, czy na pewno poszła komenda **START (`2`)** do `0x0100`. Jeśli wysłałeś
`3` — kazałeś radarowi się zatrzymać (Stop Detector).

**5. Telefon nie pokazuje strony konfiguracji**
Połącz się z hotspotem `Konfiguracja_Radaru` i wejdź ręcznie na
`http://192.168.4.1`. Jeśli urządzenie w ogóle nie stawia hotspotu — w NVS są
już zapisane dane; zresetuj konfigurację (przytrzymaj przycisk ~3 s).

**6. Brak publikacji MQTT mimo działającego radaru**
MQTT jest blokowane do czasu pełnego połączenia z Wi-Fi (`wifi_ready`).
Sprawdź log serialowy — jeśli widzisz pętlę „Rozłączono… próba ponownego
połączenia”, hasło/SSID jest błędne (zresetuj konfigurację) albo broker jest
nieosiągalny pod podanym adresem.

---

## 🔗 11. Linki

- Waveshare A121 Range Sensor — https://www.waveshare.com/wiki/A121_Range_Sensor#Documents
- Wideo poglądowe — https://www.youtube.com/watch?v=Puh9MMC1Ft0
- Acconeer Exploration Tool — https://docs.acconeer.com/en/latest/exploration_tool/index.html
- Datasheet ESP32-C6-WROOM-1 — https://documentation.espressif.com/esp32-c6-wroom-1_wroom-1u_datasheet_en.html#pcb_layout_recommendations
- ESP-IDF I2C API — https://docs.espressif.com/projects/esp-idf/en/stable/api-reference/peripherals/i2c.html

#linki
	- https://www.waveshare.com/wiki/A121_Range_Sensor#Documents
	- https://www.youtube.com/watch?v=Puh9MMC1Ft0
	- https://docs.acconeer.com/en/latest/exploration_tool/index.html
	- https://documentation.espressif.com/esp32-c6-wroom-1_wroom-1u_datasheet_en.html#pcb_layout_recommendations
	- https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/i2c.html

# Integracja Radaru Acconeer XM125 (STM32) z ESP32-C6 na Linuxie

Projekt ten opisuje pełen proces uruchomienia modułu radaru (opartego na układzie STM32 i czujniku A121) oraz zintegrowania go z mikrokontrolerem ESP32-C6 za pomocą magistrali I2C w środowisku Linux (Fedora/Ubuntu).

---

## 🔌 1. Podłączenie sprzętowe (Hardware)

Połączenie między ESP32-C6 a modułem radaru wymaga uwzględnienia jednej **krytycznej** rzeczy – pinu WAKE_UP. Bez niego radar sprzętowo odpowiada na I2C samymi zerami.

| ESP32-C6 Pin | Radar Pin | Opis |
| :--- | :--- | :--- |
| **5V / VBUS** | 5V | Zasilanie główne modułu |
| **GND** | GND | Masa |
| **GPIO 10** | SDA | Linia danych I2C |
| **GPIO 11** | SCL | Linia zegara I2C |
| **3.3V (VCC)**| **WAKE_UP** | **KRYTYCZNE:** Wymusza wybudzenie STM32 ze stanu Deep Sleep! |

---

## 🛠️ 2. Faza przygotowawcza: Oprogramowanie PC

Aby wgrać firmware I2C na radar, potrzebujesz oficjalnego narzędzia Acconeer Exploration Tool.

### Przygotowanie środowiska Python
Nie instaluj paczek globalnie. Stwórz dedykowane środowisko wirtualne:
```bash
mkdir -p ~/main/tools && cd ~/main/tools
python3 -m venv .venv
source .venv/bin/activate
(Pamiętaj, że komendę source musisz wpisać w folderze, w którym utworzyłeś .venv).Instalacja Exploration ToolBashpip install "acconeer-exptool[app]"
⚡ 3. Wgrywanie Firmware'u na Radar (STM32)Aby układ STM32 na radarze działał jako serwer I2C "Presence Detector", musisz go najpierw sflashować.Tryb Bootloadera (Hardware)Zanim spróbujesz wgrać soft, musisz wprowadzić moduł w tryb pobierania:Wciśnij i trzymaj przycisk BOOT.Kliknij raz przycisk RST (Reset).Puść przycisk BOOT.Flashowanie przez aplikacjęUruchom narzędzie: python -m acconeer.exptool.appPrzejdź do zakładki Flash.Wybierz port (np. /dev/ttyACM0). Użyj opcji "Show all devices" jeśli lista jest pusta.Wgraj plik z firmware (np. i2c_presence_detector).📊 4. Test Radaru na PC (Opcjonalnie)Zanim przejdziesz do ESP32, warto sprawdzić, czy radar widzi obiekty:Zmień port: Układ CH342 tworzy dwa porty (np. ACM0 i ACM1). Do odczytu danych użyj drugiego portu.Zmień Baudrate: Ustaw na 921600 (inaczej aplikacja zgłosi błąd timeoutu).Zresetuj moduł (kliknij RST).Połącz się, wybierz konfigurację i kliknij Start Measurement.🧠 5. Protokół I2C: Jak komunikować się z radarem?Moduł działa jako "Slave" pod adresem I2C 0x52. Posiada własną, specyficzną architekturę rejestrów:Adresy rejestrów są 16-bitowe (2 bajty).Dane w rejestrach są 32-bitowe (4 bajty). Zawsze musisz odczytać lub zapisać równe 4 bajty.Najważniejsze adresy (Rejestry)Adres (Dec)Adres (Hex)NazwaTypOpis00x0000WersjaRWersja firmware. Jeśli wczytujesz tu zera, WAKE_UP nie jest podłączony.30x0003StatusRStatus detektora (0xFF oznacza gotowość, ale brak startu).160x0010ObecnośćRWynik detekcji (Bit 0 określa ruch: 1=Ruch, 0=Pusto).170x0011OdległośćRWykryta odległość w milimetrach.2560x0100KomendyWRejestr sterujący stanem radaru.Procedura Startowa (Init Sequence)Radar po podłączeniu zasilania nie wykonuje pomiarów. Czeka na instrukcje. Aby wystartował, musisz wysłać dwie komendy pod adres 0x0100:Zastosuj konfigurację (Apply): Wyślij wartość 1.Start pomiarów (Start): Wyślij wartość 2.Przykład wysłania komendy I2C w ESP-IDF:Adres wysyłamy jako 2 bajty, a komendę jako 4 bajty.C// Wysłanie komendy START (2) do rejestru 0x0100
uint8_t cmd_start[6] = {
    0x01, 0x00, // Adres rejestru 0x0100
    0x00, 0x00, 0x00, 0x02  // Wartość (4 bajty)
}; 
i2c_master_write_to_device(I2C_NUM_0, 0x52, cmd_start, 6, 1000);
Odczyt wynikówPo wystartowaniu, pytasz radar o rejestr 0x0010 i czytasz 8 bajtów z rzędu (4 bajty obecności + 4 bajty odległości).💻 6. Środowisko ESP-IDF (ESP32-C6)KonfiguracjaW każdym nowym oknie terminala musisz aktywować ścieżki do narzędzi Espressif:Bash. $HOME/main/tools/esp-idf/export.sh
Zanim zaczniesz budować, musisz powiedzieć CMake'owi, że używasz procesora RISC-V (C6):Bashcd ~/main/projects/radar_esp/i2c_check
idf.py set-target esp32c6
Budowanie i WgrywanieAby uniknąć poszukiwań portu przez system, używaj flagi -p z adresem portu (zazwyczaj /dev/ttyACM0):Bashidf.py -p /dev/ttyACM0 build flash monitor
Aby wyjść z monitora, użyj skrótu: Ctrl + ].🚨 7. Rozwiązywanie problemów (Troubleshooting)1. Permission denied na porcie /dev/ttyACM0:Brak uprawnień w systemie Linux. Nadaj je ręcznie:sudo chmod 666 /dev/ttyACM02. Błędy kompilacji (Unknown CMake command / Parse error):System budowania ESP-IDF został wywołany z niewłaściwego katalogu.Zawsze uruchamiaj idf.py z "korzenia" projektu (tam gdzie jest główny CMakeLists.txt), a nie z folderu main/.W razie kryzysu: rm -rf build/ i zacznij kompilację od nowa.3. I2C wyrzuca same zera (00 00 00 00):STM32 śpi. Upewnij się, że pin WAKE_UP na module jest fizycznie połączony z 3.3V na ESP32.4. Dziwne wartości odległości (np. 4008635904 mm) lub brak reakcji na ruch:Radar ma zasiloną elektronikę, ale antena nie wystartowała (Pamięć zwraca śmieci).Sprawdź, czy na pewno wysłałeś komendę START (2) do rejestru 0x0100.Jeśli wysłałeś np. 3 – kazałeś mu się zatrzymać (Stop Detector).
3. Wgrywanie Firmware'u na Radar (STM32)

Aby układ STM32 na radarze działał jako serwer I2C "Presence Detector", musisz go najpierw sflashować.
Tryb Bootloadera (Hardware)

Zanim spróbujesz wgrać soft, musisz wprowadzić moduł w tryb pobierania:

    Wciśnij i trzymaj przycisk BOOT.

    Kliknij raz przycisk RST (Reset).

    Puść przycisk BOOT.

Flashowanie przez aplikację

    Uruchom narzędzie: python -m acconeer.exptool.app

    Przejdź do zakładki Flash.

    Wybierz port (np. /dev/ttyACM0). Użyj opcji "Show all devices" jeśli lista jest pusta.

    Wgraj plik z firmware (np. i2c_presence_detector).

📊 4. Test Radaru na PC (Opcjonalnie)

Zanim przejdziesz do ESP32, warto sprawdzić, czy radar widzi obiekty:

    Zmień port: Układ CH342 tworzy dwa porty (np. ACM0 i ACM1). Do odczytu danych użyj drugiego portu.

    Zmień Baudrate: Ustaw na 921600 (inaczej aplikacja zgłosi błąd timeoutu).

    Zresetuj moduł (kliknij RST).

    Połącz się, wybierz konfigurację i kliknij Start Measurement.
5. Protokół I2C: Jak komunikować się z radarem?

Moduł działa jako "Slave" pod adresem I2C 0x52. Posiada własną, specyficzną architekturę rejestrów:

    Adresy rejestrów są 16-bitowe (2 bajty).

    Dane w rejestrach są 32-bitowe (4 bajty). Zawsze musisz odczytać lub zapisać równe 4 bajty.

Najważniejsze adresy (Rejestry)
Adres (Dec)	Adres (Hex)	Nazwa	Typ	Opis
0	0x0000	Wersja	R	Wersja firmware. Jeśli wczytujesz tu zera, WAKE_UP nie jest podłączony.
3	0x0003	Status	R	Status detektora (0xFF oznacza gotowość, ale brak startu).
16	0x0010	Obecność	R	Wynik detekcji (Bit 0 określa ruch: 1=Ruch, 0=Pusto).
17	0x0011	Odległość	R	Wykryta odległość w milimetrach.
256	0x0100	Komendy	W	Rejestr sterujący stanem radaru.
Procedura Startowa (Init Sequence)

Radar po podłączeniu zasilania nie wykonuje pomiarów. Czeka na instrukcje. Aby wystartował, musisz wysłać dwie komendy pod adres 0x0100:

    Zastosuj konfigurację (Apply): Wyślij wartość 1.

    Start pomiarów (Start): Wyślij wartość 2.

Przykład wysłania komendy I2C w ESP-IDF:
Adres wysyłamy jako 2 bajty, a komendę jako 4 bajty.
C

// Wysłanie komendy START (2) do rejestru 0x0100
uint8_t cmd_start[6] = {
    0x01, 0x00, // Adres rejestru 0x0100
    0x00, 0x00, 0x00, 0x02  // Wartość (4 bajty)
}; 
i2c_master_write_to_device(I2C_NUM_0, 0x52, cmd_start, 6, 1000);

Odczyt wyników

Po wystartowaniu, pytasz radar o rejestr 0x0010 i czytasz 8 bajtów z rzędu (4 bajty obecności + 4 bajty odległości).
💻 6. Środowisko ESP-IDF (ESP32-C6)
Konfiguracja

W każdym nowym oknie terminala musisz aktywować ścieżki do narzędzi Espressif:
Bash

. $HOME/main/tools/esp-idf/export.sh

Zanim zaczniesz budować, musisz powiedzieć CMake'owi, że używasz procesora RISC-V (C6):
Bash

cd ~/main/projects/radar_esp/i2c_check
idf.py set-target esp32c6

Budowanie i Wgrywanie

Aby uniknąć poszukiwań portu przez system, używaj flagi -p z adresem portu (zazwyczaj /dev/ttyACM0):
Bash

idf.py -p /dev/ttyACM0 build flash monitor

Aby wyjść z monitora, użyj skrótu: Ctrl + ].
🚨 7. Rozwiązywanie problemów (Troubleshooting)

1. Permission denied na porcie /dev/ttyACM0:
Brak uprawnień w systemie Linux. Nadaj je ręcznie:
sudo chmod 666 /dev/ttyACM0

2. Błędy kompilacji (Unknown CMake command / Parse error):
System budowania ESP-IDF został wywołany z niewłaściwego katalogu.

    Zawsze uruchamiaj idf.py z "korzenia" projektu (tam gdzie jest główny CMakeLists.txt), a nie z folderu main/.

    W razie kryzysu: rm -rf build/ i zacznij kompilację od nowa.

3. I2C wyrzuca same zera (00 00 00 00):
STM32 śpi. Upewnij się, że pin WAKE_UP na module jest fizycznie połączony z 3.3V na ESP32.

4. Dziwne wartości odległości (np. 4008635904 mm) lub brak reakcji na ruch:
Radar ma zasiloną elektronikę, ale antena nie wystartowała (Pamięć zwraca śmieci).

    Sprawdź, czy na pewno wysłałeś komendę START (2) do rejestru 0x0100.

    Jeśli wysłałeś np. 3 – kazałeś mu się zatrzymać (Stop Detector).

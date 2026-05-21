## Quick Start

### 1. Uruchomienie Serwera MQTT (Fedora)
Serwer musi działać z włączoną obsługą WebSockets dla przeglądarki.
W głównym folderze projektu wpisz:
```bash
mosquitto -c mosquitto.conf -v
```
###2. Kompilacja i Wgrywanie na ESP32

Zanim zaczniesz, upewnij się, że załadowałeś środowisko ESP-IDF:
Bash

. $HOME/main/tools/esp-idf/export.sh

Wgranie kodu i odpalenie monitora (upewnij się, że port to np. /dev/ttyACM0):

```bash
idf.py fullclean
idf.py set-target esp32c6
idf.py build flash monitor
```
Wyjście z monitora,  Ctrl + ]
###3. Odpalenie Panelu Pielęgniarki (Dashboard)

Nie potrzebujesz serwera HTTP. Po prostu otwórz plik w przeglądarce:
Bash

firefox dashboard/index.html

###4. Zarządzanie Git / GitHub:
Wysyłanie nowych zmian na serwer:
```Bash
git add .
git commit -m "Opis tego, co zmieniłeś"
git push
```
###5.Zmienne Kalibracyjne (Do edycji w main.c)

    PODLOGA_MM (Domyślnie 2500) - Odległość radaru od podłogi (potrzebne do detekcji upadku).

    PROG_UPADKU (Domyślnie 1500) - Wymagana dynamika ruchu (Intra Score) do wyzwolenia alarmu.

    MAX_TEMP (Domyślnie 60°C) - Próg alarmu pożarowego dla chipa.

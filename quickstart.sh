
#!/bin/bash

echo "=== START ŚRODOWISKA RADAR IoT ==="

echo "[1/4] Bezwzględne czyszczenie portów (ubijanie zombie)..."
sudo systemctl stop mosquitto 2>/dev/null
sudo killall -9 mosquitto 2>/dev/null
sleep 1

echo "[2/4] Start brokera Mosquitto w nowym oknie..."
# To polecenie otworzy nowy terminal obok i odpali tam serwer
gnome-terminal --title="SERWER MQTT" -- bash -c "sudo mosquitto -c mosquitto.conf -v; exec bash"

echo "[3/4] Ładowanie środowiska ESP-IDF..."
. $HOME/main/tools/esp-idf/export.sh

echo "[4/4] Odpalanie panelu w przeglądarce..."
# Jeśli plik HTML masz w innym folderze, popraw ścieżkę poniżej:
xdg-open dashboard/index.html 2>/dev/null

echo ""
echo "=== SYSTEM GOTOWY ==="
echo "Wybierz, co chcesz teraz zrobić w tym oknie:"
echo "--------------------------------------------------------"
echo "Opcja A (MASZ KABEL): Wpisz ->  idf.py build flash  monitor"
echo "					. $HOME/main/tools/esp-idf/export.sh"
echo "Opcja B (BEZ KABLA) : Wpisz ->  mosquitto_sub -h 127.0.0.1 -t 'pokoj/101/radar' -v"
echo "--------------------------------------------------------"
echo "=== PAMIETAJ O FIREWALLD ===="
echo "sudo systemctl stop firewalld"

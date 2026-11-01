# Firmware STM32 modułu XM125

`i2c_presence_detector.bin` - gotowy firmware „I2C Presence Detector” dla STM32L431 na module XM125
(serwer I2C pod adresem `0x52`, wymagany przez firmware ESP32-C6).

Źródło: Acconeer A121 SDK dla XM125 (wydanie z października 2025), licencja BSD 3-Clause
(`LICENSE-Acconeer.txt`). Plik nie jest modyfikowany.

Wgranie: tryb bootloadera (`BOOT` + `RST`), następnie Acconeer Exploration Tool, zakładka **Flash**
(patrz główny [README](../../README.md#firmware-radaru-stm32)).

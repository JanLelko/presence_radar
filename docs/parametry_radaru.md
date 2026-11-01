# Parametry radaru XM125, opisy do panelu

Źródło: `main/radar_cfg.c` (tabela `RADAR_CFG_DESC`) + A121 Presence Detector User Guide,
mapa rejestrów `xm125/Inc/presence_reg_protocol.h`.

Ten plik jest tekstem pomocniczym do dymków „po najechaniu" w panelu konfiguracji
(faza 7). Kolumna **Klucz JSON** = to, co panel wysyła w `pokoj/101/cfg` -> `radar:{...}`.
Zakresy i defaulty muszą się zgadzać z tabelą w `radar_cfg.c`, jak zmienisz tam, zmień
i tu.

## Jednostki

- `*_mhz`, miliherce. `12000` = 12 Hz. (Rejestr trzyma częstotliwość ×1000.)
- `*_threshold`, `*_cutoff`, `signal_quality`, wartość RSS ×1000 (tak jak w rejestrze I2C).
- `*_time_const`, `*_timeout`, milisekundy / sekundy wg nazwy.
- `start_mm`, `end_mm`, milimetry.
- `*_enabled`, `*_on_*`, bool 0/1.

## Strefa i próbkowanie

| Klucz JSON | Rej | Default | Zakres | Co zmienia |
|---|---|---|---|---|
| `start_mm` | 0x52 | 300 | 0-10000 | Początek strefy wykrywania (odległość od radaru). Bliżej niż to = ignorowane. |
| `end_mm` | 0x53 | 2500 | 100-10000 | Koniec strefy. Dalej niż to = ignorowane. Musi być > `start_mm+100`. Węższa strefa = mniej fałszywych alarmów i szybszy pomiar. **Reguła upadku wymaga `end_mm` powyżej `podloga_mm-400`**, za ciasny `end_mm` po cichu wyłącza wykrywanie upadku. |
| `sweeps_per_frame` | 0x40 | 16 | 1-4095 | Ile „zamiotów" radaru składa się na jedną ramkę. Więcej = czulszy na słaby ruch i lepszy SNR, ale ramka trwa dłużej (niższy realny frame rate). |
| `hwaas` | 0x55 | 32 | 1-511 | *Hardware Accelerated Average Samples*, ile surowych impulsów uśrednia sprzęt na jeden punkt. Więcej = większy zasięg i mniej szumu, ale wolniej i większy pobór. Mniej = szybciej, krótszy zasięg. |
| `frame_rate_mhz` | 0x45 | 12000 | 1000-30000 | Docelowa częstotliwość ramek (12000 = 12 Hz). Wyżej = mniejsza latencja detekcji, ale moduł może nie nadążać przy dużym `sweeps_per_frame`/`hwaas` (sprawdzane realnie przez Measure Counter). |
| `signal_quality` | 0x57 | 15000 | 0-35000 | Żądana jakość sygnału (RSS×1000). Wyżej = radar sam dobiera więcej uśredniania/mocy dla czystszego sygnału, kosztem czasu. |

## Detektory: szybki (intra) vs wolny (inter)

Radar liczy dwie osobne „energie ruchu":
- **intra**, ruch szybki, w obrębie jednej ramki: machnięcie, gest, upadek. Rejestr wyniku `0x12`.
- **inter**, ruch wolny, między ramkami: ktoś stoi i lekko się porusza, oddycha. Rejestr wyniku `0x13`.

| Klucz JSON | Rej | Default | Zakres | Co zmienia |
|---|---|---|---|---|
| `intra_detection_enabled` | 0x43 | 1 | 0/1 | Włącza gałąź szybkiego ruchu. Wyłączona = brak reakcji na machnięcia/upadek. |
| `inter_detection_enabled` | 0x44 | 1 | 0/1 | Włącza gałąź wolnego ruchu / nieruchomej obecności. Wyłączona = radar „gubi" osobę, która przestała się ruszać. |
| `intra_detection_threshold` | 0x46 | 1300 | 0-5000 | Próg energii szybkiego ruchu. **Niżej = czulszy** na drobne gesty i upadek (i więcej fałszywych trafień). To główna „czułość" dla gry i wykrywania upadku. |
| `inter_detection_threshold` | 0x47 | 1000 | 0-5000 | Próg energii wolnego ruchu. Niżej = łatwiej utrzymać wykrycie stojącej/siedzącej osoby, ale rośnie ryzyko wykryć od firanki/wentylatora. |
| `inter_frame_presence_timeout_s` | 0x41 | 3 | 0-30 | Ile sekund po ostatnim ruchu radar dalej raportuje „obecność". Dłużej = stabilniej przy osobie, która znieruchomiała; krócej = szybciej gaśnie po wyjściu. |

## Filtry i stałe czasowe (wygładzanie)

Im mniejsze stałe czasowe, tym szybsza reakcja, ale bardziej „nerwowy" sygnał.

| Klucz JSON | Rej | Default | Zakres | Co zmienia |
|---|---|---|---|---|
| `intra_frame_time_const` | 0x4b | 150 | 0-1000 ms | Wygładzanie szybkiego sygnału wewnątrz ramki. Mniejsze = szybsza reakcja na gest. |
| `intra_output_time_const` | 0x4c | 300 | 10-20000 ms | Wygładzanie wyjścia gałęzi szybkiej. **Największa pojedyncza dźwignia latencji**, dla trybu gry schodzi do ~50. |
| `inter_output_time_const` | 0x4d | 2000 | 10-20000 ms | Wygładzanie wyjścia gałęzi wolnej. Duże = bardzo stabilna flaga obecności, ale wolno reaguje na wejście/wyjście. |
| `inter_frame_deviation_time_const` | 0x48 | 500 | 10-20000 ms | Jak szybko filtr „uczy się" tła między ramkami (odchylenie). |
| `inter_frame_fast_cutoff_mhz` | 0x49 | 6000 | 1000-50000 | Górna częstotliwość odcięcia filtru wolnego kanału (część szybka). |
| `inter_frame_slow_cutoff_mhz` | 0x4a | 200 | 10-1000 | Dolna częstotliwość odcięcia (część wolna), reaguje na bardzo powolne zmiany. |
| `reset_filters_on_prepare` | 0x54 | 1 | 0/1 | Czy zerować filtry przy każdym starcie detektora. 1 = czysty start bez „duchów" z poprzedniej sesji. |

## Profil i długość kroku (rozdzielczość vs zasięg)

Profil = zestaw parametrów radaru dobrany pod zasięg. 1 = najkrótszy zasięg, najlepsza
rozdzielczość; 5 = daleki zasięg, gorsza rozdzielczość.

| Klucz JSON | Rej | Default | Zakres | Co zmienia |
|---|---|---|---|---|
| `auto_profile_enabled` | 0x4e | 1 | 0/1 | 1 = radar sam dobiera profil z `start_mm`/`end_mm`. 0 = używa `manual_profile`. |
| `manual_profile` | 0x50 | 4 | 1-5 | Ręczny profil (gdy `auto_profile_enabled=0`). Niższy = krótki zasięg + ostrzejsza rozdzielczość odległości. |
| `auto_step_length_enabled` | 0x4f | 1 | 0/1 | 1 = radar sam dobiera odstęp między punktami pomiarowymi. 0 = `manual_step_length`. |
| `manual_step_length` | 0x51 | 72 | 1-4095 | Ręczny odstęp punktów (gdy auto=0). Mniejszy = gęstszy pomiar w odległości, wolniej. |
| `automatic_subsweeps` | 0x56 | 1 | 0/1 | 1 = radar sam dzieli strefę na podzakresy z różnymi ustawieniami. |

## GPIO

| Klucz JSON | Rej | Default | Zakres | Co zmienia |
|---|---|---|---|---|
| `detection_on_gpio` | 0x80 | 0 | 0/1 | 1 = moduł sam ustawia swój pin wyjściowy w stan wysoki przy wykrytej obecności (sprzętowy sygnał bez odpytywania po I2C, rozważane jako szybka ścieżka „hit" dla gry). |

## Presety (`radar_cfg_preset()` w `radar_cfg.c`)

Preset = kompletny zestaw wszystkich 24 wartości. Panel wysyła potem tylko **delty** na
wybranym presecie (`{"preset":"care","radar":{"end_mm":1200}}`).

- **`CARE`** (`CFG_PRESET_CARE`), wszystkie 24 pola = udokumentowane defaulty POR z tabeli
  wyżej. Normalny tryb projektu: nadzór całego pokoju, oba detektory, strefa 300-2500 mm,
  12 Hz. To jest to, co teraz leci po flashu.
- **`GAME`** (`CFG_PRESET_GAME`), te same 24 pola, ale ~13 nadpisanych pod gest-jako-przycisk
  na biurku: `start_mm 100`, `end_mm 700`, `frame_rate 20000`, `sweeps 8`, `hwaas 16`,
  `intra_frame_time_const 0`, `intra_output_time_const 50`, `inter_detection_enabled 0`,
  `auto_profile 0` + `manual_profile 1`, `intra_detection_threshold 300`. Cel: najniższa
  możliwa latencja, reszta (obecność, wolny ruch) wyłączona. Wchodzi w fazie 6.

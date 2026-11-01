# Raport końcowy, źródło

Prosty, czarno-biały raport. Treść z wcześniejszej wersji, przepisana zwięźle,
plus schemat/PCB z KiCada, kwestie bezpieczeństwa, ograniczenia i quick start.

## Pliki

- `raport_koncowy.tex`, źródło
- `raport_koncowy.pdf`, gotowy PDF (9 stron)
- `img/`, grafiki:
  - `schemat_ideowy.png`, schemat z KiCada (wyeksportowany `kicad-cli`)
  - `pcb_layout.png`, rozmieszczenie elementów PCB
  - `urzadzenie.jpg`, **DO WGRANIA**: zdjęcie prototypu (na razie placeholder)

## Jak edytować z podglądem (najprościej: Overleaf)

1. Wejdź na overleaf.com (darmowe konto).
2. New Project -> Upload Project -> wrzuć **cały folder `raport/`** (albo jego zip).
3. Overleaf od razu pokazuje PDF obok. Klikasz w tekst, piszesz, „Recompile".

Plik kompiluje się każdym silnikiem (pdfLaTeX / XeLaTeX / LuaLaTeX), więc
ustawienie „Compiler" w Overleaf nie ma znaczenia, działa domyślne.

### Wgranie zdjęcia urządzenia
W Overleaf: wejdź w folder `img/`, Upload, nazwij plik `urzadzenie.jpg`.
Placeholder sam zniknie, zdjęcie wskoczy jako Rysunek 3. (Lokalnie: po prostu
skopiuj plik jako `img/urzadzenie.jpg` i skompiluj ponownie.)

### Strona tytułowa (oficjalny druk RAu6)
Raport z `.tex` NIE ma własnej strony tytułowej, zaczyna się od tytułu i
streszczenia. Oficjalna okładka PŚl (`Raport projektu.docx`) jest doklejana
z przodu jako osobny PDF:

1. W Wordzie uzupełnij pola okładki: Rodzaj studiów, Przedmiot, Grupa, Sekcja,
   Imię, Nazwisko, zakreśl Prowadzącego, Temat projektu, Główne założenia,
   Data oddania.
2. Word -> *Zapisz jako -> PDF* -> `strona_tytulowa.pdf` (eksport z Worda jest
   wierniejszy niż z LibreOffice).
3. Scal:
   ```bash
   pdfunite strona_tytulowa.pdf raport_koncowy.pdf raport_KONCOWY_scalony.pdf
   ```
   (bez `pdfunite`: dowolny scalacz PDF online, albo `qpdf --empty --pages
   strona_tytulowa.pdf raport_koncowy.pdf -- out.pdf`)

W repo jest już `raport_KONCOWY_scalony.pdf`, ale z PUSTĄ okładką, do podmiany
po uzupełnieniu.

## Kompilacja lokalna (bez Overleafa)

```bash
tectonic raport_koncowy.tex
```

`tectonic` = jedna binarka (jest w `~/.local/bin`), sama pobiera pakiety.
Wymaga sieci przy pierwszym uruchomieniu.

## Regeneracja grafik z KiCada (gdyby projekt się zmienił)

```bash
SCH=../../hardware/radar_xm125_esp32c6/radar_xm125_esp32c6.kicad_sch
PCB=../../hardware/radar_xm125_esp32c6/radar_xm125_esp32c6.kicad_pcb
kicad-cli sch export svg --black-and-white --exclude-drawing-sheet --no-background-color -o /tmp/sch "$SCH"
kicad-cli pcb export svg --mode-single --exclude-drawing-sheet --page-size-mode 2 \
  --layers F.Cu,B.Cu,F.Silkscreen,Edge.Cuts --black-and-white -o /tmp/pcb.svg "$PCB"
magick -density 220 -background white /tmp/sch/*.svg -flatten -trim +repage -bordercolor white -border 30 img/schemat_ideowy.png
magick -density 320 -background white /tmp/pcb.svg -flatten -trim +repage -bordercolor white -border 26 img/pcb_layout.png
```

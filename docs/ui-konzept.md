# Bedienkonzept Front-Display – 4-Port Art-Net-4/RDM-Node

Stand: 2026-10-25 · beschreibt die Phase-1-Implementierung (`node/firmware/components/pm_ui/`)
Hardware: Waveshare ESP32-P4-WIFI6-POE-ETH, ST7735S 1,8" TN quer (160 × 128), Drehencoder mit Druck + BACK-Taster, keine LEDs.

Die Bildschirme in diesem Dokument rendert der Host-Renderer aus genau dem Code, der auch auf dem Gerät läuft
(`ui_core.c` ist reines C). Neu erzeugen:

```sh
make -C node/firmware/components/pm_ui/host run   # schreibt host/out/*.png + host/out/README.md (Szenen: host/scenes.c)
```

Die PNGs liegen in [`node/firmware/components/pm_ui/host/out/`](../node/firmware/components/pm_ui/host/out/README.md)
(4-fach vergrößert). Unter jedem Mockup steht das passende Bild. Weichen Mockup und PNG ab, gilt das PNG.

---

## 1. Leitprinzipien

1. **Der Startbildschirm ist die Front, das Menü ist die Werkstatt.** Alles, was man im Betrieb wissen muss (Name, IP, Ports, Adressen, Datenfluss), steht ohne einen einzigen Tastendruck auf dem Home-Screen. Menüs braucht man nur, um etwas zu ändern.
2. **Häufiges ist flach, Seltenes liegt tiefer.** Port-Adresse ändern: 3 Drücke. IP ablesen: 0. Namen eintippen oder Werksreset: tief und absichtlich etwas umständlich.
3. **Man weiß immer, wo man ist.** Jeder Screen außer Home hat eine Kopfzeile mit Pfad. Keine Kopfzeile bedeutet: Home. Ausnahmen sind nur Dialoge (Titel statt Pfad) und das Vollbild beim Node-Firmware-Update.
4. **Eine Regel pro Taste, überall gleich.** Drehen = bewegen/ändern, Drücken = „rein / fertig“, BACK kurz = eine Ebene zurück (bzw. Abbruch), BACK lang = Home. Es gibt keine Ausnahmen, nur Ergänzungen (Kombi-Griff für die Sperre, Halten für Löschen im Texteditor und für den Werksreset).
5. **Die Statusleiste ist heilig.** Sie ist immer sichtbar, immer gleich aufgebaut und ersetzt die LEDs. Nur dort wird geblinkt (Datenverlust, Identify, laufendes DHCP). Der Inhaltsbereich blinkt nie.
6. **Ereignisse stehlen keinen Fokus.** Datenverlust, Remote-Änderungen, Identify und RDM-Updates werden angezeigt (Statusleiste, Toast, Wert-Hervorhebung), reißen den Bediener aber nicht aus einer Eingabe. Die einzige Ausnahme ist ein Firmware-Update des Nodes selbst, weil das Gerät danach neu startet.
7. **Farbe ist nie die einzige Information.** Jeder Zustand hat zusätzlich ein Zeichen oder Kürzel. Das ist wegen TN-Blickwinkel und Farbfehlsichtigkeit nötig.
8. **Wer zuletzt schreibt, gewinnt – ohne Dialog.** Was Pixelmapper (oder eine andere Software) ändert, erscheint sofort am Gerät (Wert 3 s orange, Toast). Trifft eine Remote-Änderung einen Wert, den jemand am Gerät gerade bearbeitet, bleibt die Eingabe stehen und ein kurzer Hinweis „↻ changed by Pixelmapper“ zeigt den neuen Wert. Wer danach bestätigt, gewinnt. Es gibt keine Konfliktdialoge, Sperren oder Versionsnummern (wie in Pixelmapper und [contract.md](../protocol/contract.md) §1).

Muster von Rack-Funkempfängern (Shure ULX-D/Axient, Sennheiser EM-Serie), aus Erfahrung, nicht neu recherchiert: Der Home-Screen ist eine Status-Seite. Ein bearbeiteter Wert ist deutlich markiert, bis er bestätigt ist. EXIT verwirft. Die Frontsperre löst man über einen gehaltenen Tasten-Griff. Die Menüs sind flach und zeigen Werte direkt in der Liste („Label …… Wert“). Dieses Konzept übernimmt genau diese Muster und verzichtet auf alles darüber hinaus.

Die ganze Oberfläche ist **englisch**. Dieses Dokument ist deutsch, alle UI-Texte, Labels und Mockups sind englisch und
stehen so im Code.

---

## 2. Bildschirmaufbau

### 2.1 Physik und Lesbarkeit

- Die aktive Fläche ist etwa 35,0 × 28,0 mm groß, das sind rund **0,22 mm pro Pixel**.
- Eine 16-px-Zeile mit 8×16-Font hat etwa 10 px Versalhöhe, also rund 2,2 mm. Das ergibt aus 70 cm etwa 11 Bogenminuten: grenzwertig für Fließtext, aber ausreichend für kurze, bekannte Wörter. Aus der Bedienentfernung (Hand am Encoder, ca. 40–50 cm) sind es 15–19 Bogenminuten, das ist gut lesbar.
- **Daraus folgt:**
  - Aus 50–80 cm muss man lesen können: die Farbkacheln der Statusleiste, den Short Name und die Port-Zeilen auf Home (Adresse, fps).
  - Menütext liest man aus Bedienentfernung.
  - Die kleine Schrift nur für Nebensachen (Kopfzeile/Pfad, Hinweise, Toasts, IP-Zeile auf Home).

### 2.2 Fonts (genau zwei)

| Name | Zelle | Quelle | Einsatz | Zeichen/Zeile |
|---|---|---|---|---|
| **F-N** (Normal) | 8 × 16, fett | Terminus Font 4.49.1 Bold (`ter-u16b`), SIL OFL 1.1 | Menüs, Werte, Home-Port-Zeilen, Short Name, Dialogtexte, Statusleiste | 20 |
| **F-S** (Klein) | 6 × 8 | X11 misc-fixed `5x8` (Public Domain), linksbündig in eine 6-px-Zelle gesetzt | Kopfzeile/Pfad, Hinweiszeilen, Toasts, „changed by Pixelmapper“, IP-Zeile auf Home, Feldbeschriftungen | 26 |
| groß | 16 × 32 | F-N doppelt skaliert | nur die Prozentzahl beim Node-Update | 10 |

- Fett, also mit 2-px-Vertikalstrichen, weil 1-px-Striche auf TN aus schrägem Winkel ausfransen.
- Beide Fonts haben dieselbe Codepage: ASCII 0x20–0x7E plus UI-Symbole auf 0x80–0x96 (▸ ◂ – ↻ ↓ ✓ ⌫ ␣ ⚠ ▲ ✕ Schloss ● › … µ → ← ↑ ‹ · ☼ █). Ein Teil stammt aus den Quell-Fonts, der Rest ist für das Projekt gezeichnet.
- Einzige Änderung an einem ASCII-Zeichen: Der Punkt in F-S ist ein 2×2-Block, weil der Punkt von `5x8` in IP-Adressen wie „+“ aussah.
- Daten und Herkunft: `pm_ui/src/ui_font.h`, `pm_ui/fonts/README.md`; erzeugt mit `pm_ui/tools/gen_fonts.py`.

### 2.3 Raster (alle Screens außer Home)

```
y   0–15   Statusleiste              16 px (immer)
y  16–30   Kopfzeile                 F-S: eine Zeile (y 20) oder zwei Zeilen (y 16 / y 24)
y  31      Trennlinie                 1 px #505050
y  32–127  Inhalt                    96 px = 6 Zeilen à 16 px, F-N, 20 Zeichen
           └ Vollbild-Editoren: y 112–127 = zwei Hinweiszeilen F-S (oben cyan, unten grau)
x 157–159  Scrollbalken 3 px (nur wenn die Liste mehr als 6 Einträge hat)
```

- **Kopfzeile, Priorität:** Konflikthinweis „↻ changed by Pixelmapper“ (4 s, zwei Zeilen, orange) > Toast (3 s, eine Zeile) > Edit-Hinweis bei Inline-Edit (cyan „Turn: change  Push: OK“ + grau „BACK: cancel“) > Pfad (weiß). In Vollbild-Editoren steht der Konflikthinweis unten in den Hinweiszeilen, oben bleibt Toast oder Pfad.
- **Pfad:** Titel der Ebenen mit „ › “ verbunden, z. B. `Port 1 · Output › Address`. Ist er länger als 26 Zeichen, werden vordere Ebenen durch `… ›` ersetzt (`… › Devices (12) › P1 T-03`). Auf der Netzwerkseite mit vorgemerkten Werten steht rechts ein cyan „●“.
- **Listenzeile:** Label links ab x = 2, Wert rechtsbündig bis x = 156. Ein Untermenü hat „›“ als Wert. Das Label wird abgeschnitten, damit der Wert passt (lange Werte wie `192.168.100.217` lassen nur 2–4 Zeichen Label). Aktionen (`Apply…`, `Reset port…`) und Geräteeinträge nutzen die ganze Zeile.
- **Cursor:** weißer Balken x 0–156, Text schwarz. **Info-Zeilen** (graues Label, ohne Funktion) überspringt der Cursor. Eine reine Info-Seite hat keinen Cursor und scrollt beim Drehen.
- **Dialoge:** Titel in F-N mit ⚠ statt Pfad (gelb, rot bei Werksreset), bis zu 4 Textzeilen F-N, zwei Buttons à 74 × 16 px (x 4 und x 82, y 98), darunter eine Hinweiszeile F-S.
- **Home** hat ein eigenes Raster (siehe 4.1): keine Kopfzeile.

### 2.4 Farbsystem (RGB565, TN-tauglich)

Grundsatz: schwarzer Hintergrund, nur sehr helle oder voll gesättigte Vordergrundfarben, keine Mittelgrau-Abstufungen als Bedeutungsträger. TN-Panels kippen Mitteltöne bei Blickwinkeländerung stark und invertieren sie teils. Schwarz/Weiß und Primärfarben bleiben stabil. Die Werte stehen in `ui_gfx.h`.

| Rolle | Farbe | Hinweis |
|---|---|---|
| Hintergrund | Schwarz #000000 | Kein dunkelgrauer Hintergrund (TN-Kippen) |
| Text primär | Weiß #FFFFFF | |
| Text sekundär / Info | Hellgrau #A0A0A0 | Das einzige Grau mit Bedeutung. Nie dunkler. |
| Deaktiviert („off“) | Grau #606060 | Nur für eindeutig abgeschaltete Dinge |
| Trennlinien, Scrollbalken-Spur | #505050 | ohne Bedeutung |
| **Auswahl (Cursor)** | Weißer Balken, schwarzer Text (invertiert) | Robusteste Markierung auf TN |
| **Editieren / vorgemerkt** | Cyan #00DCFF, schwarzer Text | „Dieser Wert ist offen, noch nicht übernommen“; auch Quittung: Der Cursorbalken blitzt 120 ms cyan |
| **Remote-Änderung** | Orange #FF8C00, mit ↻ | Wert 3 s orange, Toast, Konflikthinweis, Remote-Sperre |
| **Warnung** (Dialog-Titel) | Gelb #FFE000 + ⚠ | |
| **Gefahr** (Werksreset) | Rot #FF2020 | |
| Status grün „sendet“ | #20FF20 | Nur in Statusleiste und Port-Badges |
| Status blau „Eingang“ | #3090FF | Heller als reines Blau, sonst zu dunkel auf TN |
| Status gelb „Failsafe/Verlust“ | #FFE000 | |
| Status violett „RDM“ | #C040FF | Aktivitätskante |
| Status rot „Fehler“ | #FF2020 | |
| Identify | Weiß #FFFFFF | ganze Statusleiste |

- **Text auf Kacheln:** Schwarz auf Grün, Gelb und Weiß. Weiß auf Blau und Rot.
- **Blickwinkel:** Das Panel ist eigentlich hochkant (128 × 160) gebaut. Quer eingebaut wandert die schlechte TN-Blickrichtung (meist „6 Uhr“) auf die linke oder rechte Seite. Das ist für ein Rack günstig, weil der Blickwinkel von oben/unten (Höhe im Rack) dann symmetrisch und gut ist.
- **Empfehlung:** Beide Querlagen (MADCTL 90° und 270°) am echten Modul testen und die nehmen, bei der man von schräg vorne oben (typische Rackhöhe Brust/Bauch) besser sieht. Die Gamma-Register (GMCTRP1/GMCTRN1) auf diese Einbaulage abstimmen (offen, siehe 8.2).

### 2.5 Zeichnen und Teil-Updates

- **Kein Vollbild-Framebuffer.** Jeder Screen beschreibt bei jedem Tick sein ganzes Bild als **Display-Liste** (Flächen und Texte in Malreihenfolge, `ui_gfx.h`).
- `ui_gfx_render()` rastert die Liste **Streifen für Streifen** in einen Puffer von **160 × 16 px = 5 KB** (DMA-fähig) und vergleicht pro **Kachel 32 × 16 px eine CRC** mit dem letzten Bild.
- Pro Streifen wird **nur der schmutzige Bereich** (von der ersten bis zur letzten geänderten Kachel) als ein CASET/RASET-Fenster per SPI-DMA gesendet. Ein unverändertes Bild kostet CPU, aber keinen SPI-Verkehr.
- Wo das zählt:
  - **Blinken** (Datenverlust, Identify): nur die Statusleiste, 1–2 Hz, vernachlässigbar.
  - **fps-Zähler** auf Home/Port-Seite: nur die betroffenen Kacheln.
  - **Encoder im Edit-Modus:** nur das Wertfeld.
  - **Scrollen in Listen:** alle 6 Inhaltszeilen, das sind 30 KB über SPI. Bei realistisch 15 MHz (ST7735S-Grenze) sind das etwa 17 ms. Kein Smooth-Scrolling, zeilenweise springen.
- Nach dem Einschalten des Panels (z. B. Ende von LED Mute) wird einmal alles gesendet.
- Keine Animationen außer Blinken und Fortschrittsbalken. Das ist ruhig, „clean“ und billig.

---

## 3. Statusleiste (y 0–15, immer sichtbar)

### 3.1 Layout

```
x: 0     24 25    49 50    74 75    99   104  118  132  146  159
   ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐    ┌────┬────┬────┬────┐
   │ 1 ▸  │ │ 2 ▸  │ │ 3 ◂  │ │ 4 –  │    │ ▲  │ D  │ ▣  │ ↓  │
   └──────┘ └──────┘ └──────┘ └──────┘    └────┴────┴────┴────┘
     Port-Kacheln 4 × 24 px, 1 px Abstand   System-Zone 4 × 14 px
```

**Port-Kachel (24 × 16 px, x = 25 · Port):**
- Ziffer (F-N) plus Richtungssymbol: ▸ = Ausgang, ◂ = Eingang, – = aus, ! = Fehler.
- Hintergrund = Zustandsfarbe.
- Die unteren 3 px sind die **Aktivitätskante**: Violett-Blitz bei RDM, Fortschrittsbalken bei einem Tube-Update.

**System-Zone (x 104–159, 4 Felder à 14 px):** Link, IP-Modus, Sperre, Update/Systemfehler. **Leere Felder bleiben schwarz.** Es wird nur angezeigt, was vom Normalzustand abweicht oder ihn bestätigt (Link, IP-Modus).

### 3.2 Port-Zustände (Priorität von oben nach unten, der höchste gewinnt)

| Zustand | Kachel | Zeichen | Blinken |
|---|---|---|---|
| Fehler (Ereignis vom Port, 10 s lang) | rot | Ziffer + „!“ weiß | nein |
| Datenverlust / Failsafe aktiv | gelb | Ziffer + Pfeil schwarz | 1 Hz: gelb ↔ schwarz mit gelbem Rahmen und gelber Schrift |
| Ausgang sendet | grün | Ziffer + ▸ schwarz | nein |
| Eingang empfängt DMX | blau | Ziffer + ◂ weiß | nein |
| aktiv, aber keine Daten (noch nie welche gehabt / Eingang ohne Signal) | schwarz, 1-px-Rahmen weiß | Ziffer + Pfeil weiß | nein |
| deaktiviert | schwarz, kein Rahmen | Ziffer + „–“ grau | nein |

**Aktivitätskante (unten, 3 px):**
- **RDM-Transaktion:** violett, mindestens 150 ms sichtbar (Pulsverlängerung). Folgen mehrere schnell hintereinander, bleibt sie an. Die Hauptfarbe bleibt sichtbar, weil RDM parallel zu DMX läuft.
- **Tube-Update auf dem Port:** Fortschrittsbalken cyan über die Kachelbreite. Er ersetzt den Violett-Blitz, der sonst während des Updates dauerhaft an wäre.

„Datenverlust“ heißt: Der Port hatte Art-Net-Daten, und der Timeout ist abgelaufen. Ab dann gilt der Failsafe-Modus des Ports. Gelb bleibt, bis wieder Daten kommen.

### 3.3 System-Zone

| Feld | Zustand | Darstellung |
|---|---|---|
| Link | Link ok | ▲ grün |
| | kein Link | ✕ rot |
| IP-Modus (nur mit Link) | DHCP erhalten | „D“ weiß |
| | Statisch | „S“ weiß |
| | Fallback 2.x.x.x | „F“ **gelb**, weil meist unbeabsichtigt |
| | DHCP läuft gerade (noch keine Adresse) | „D“ grau, blinkt 1 Hz |
| Sperre | entsperrt | leer |
| | lokal gesperrt | Schloss weiß |
| | von Pixelmapper gesperrt | Schloss orange |
| Update/System | nichts | leer |
| | Node-Firmware wird empfangen | ↓ cyan mit 2-px-Fortschritt an der Unterkante |
| | Tube-Update läuft (irgendein Port) | ↓ cyan (Details an der Port-Kante) |
| | Systemfehler (Ereignis ohne Port, Update fehlgeschlagen) | ! rot |

### 3.4 Identify (Art-Net LED Locate, RDM IDENTIFY_DEVICE, Node › Identify)

Solange Identify aktiv ist, wechselt die **ganze Statusleiste mit 2 Hz** zwischen einem weißen Balken mit schwarzem
`☼ IDENTIFY ☼` (F-N, zentriert) und der normalen Leiste. Der Inhalt bleibt, wie er ist (Home, Menü oder Editor). Es
gibt kein Identify-Vollbild. Art-Net LED Locate und RDM IDENTIFY_DEVICE an die Node-UID wirken gleich
([contract.md](../protocol/contract.md) §5); Identify weckt das Display (5.4).

### 3.5 LED Mute

Art-Net **LED Mute** (`ArtAddress` AcLedMute) schaltet das Display **ganz aus** (Backlight aus, kein SPI-Verkehr).
Es geht wieder an bei der nächsten Bedienung am Gerät (diese Eingabe weckt nur und löst nichts aus), bei LED Normal
oder bei LED Locate. Das ist die einzige Situation, in der das Display dunkel ist (siehe 8.1).

---

## 4. Startbildschirm und Menübaum

### 4.1 Home (Startbildschirm)

Raster: Statusleiste 16 px · Short Name 16 px (y 16–31) · IP-Zeile F-S (y 33) · 4 Port-Zeilen à 21 px (ab y 44).

- **Short Name** (F-N, weiß, bis 17 Zeichen). Bei mehreren gleichen Nodes in einem Rack ist der Name das Erste, was man sucht. Remote geändert: 3 s orange.
- **IP-Zeile** (F-S): `192.168.100.217 /24  DHCP`, Modus `DHCP`/`Static`/`Fallback`.
  - Fallback: gelb. Ohne Link: rot `No link`. DHCP läuft noch: grau `DHCP…`.
  - Sie ist zugleich die **Toast-Zeile von Home**: Remote-Änderungen und Ereignisse ersetzen sie 3 s lang.
- **Port-Zeile** (F-N, 20 Zeichen), feste Spalten:
  `P▸ ADRESSE··· fps TAG`
  - Spalte 1–2: Port-Badge. Ziffer und Pfeil auf der Zustandsfarbe (20 × 17 px), also dieselbe Farbe wie die Kachel oben. Ohne Daten: Ziffer und Pfeil weiß ohne Fläche.
  - ab Spalte 4: Port-Adresse im gewählten Format (`0:0:1` oder `4097`); remote geändert 3 s orange.
  - fps rechtsbündig bis Spalte 16 (max. 99), `--` ohne Daten.
  - Spalte 18–20: Kürzel, nach Priorität:
    - `ERR` rot
    - `HLD`/`ZER`/`FUL`/`SCN` gelb = Failsafe greift (zeigt, *was* gerade ausgegeben wird)
    - `↓41` cyan = Tube-Update 41 %
    - `MRG` = Merge aktiv (zwei Quellen)
    - `R12` = 12 RDM-Geräte (nur Ausgang mit RDM)
    - leer
  - Deaktiviert: `4  off` grau.

**Cursor auf Home:**
- Im Ruhezustand ist **kein** Cursor sichtbar (ruhiges Bild).
- Die erste Encoder-Raste blendet den Cursor auf dem zuletzt gewählten Ziel ein (initial Port 1).
- Ziele (von oben): Name → Hauptmenü, IP-Zeile → Netzwerk, Port 1–4 → Port-Seite.
- **Drücken ohne Cursor öffnet das Hauptmenü.**
- Nach 10 s ohne Eingabe oder mit BACK verschwindet der Cursor wieder.

**Begründung:** Auf Home ist alles maximal 1 Druck entfernt, was man im Betrieb anfasst: Ports, Netzwerk, Menü. Es gibt bewusst nur *einen* Home-Screen und keine umschaltbaren Ansichten. Wer weitere Ansichten durchdreht, verliert die Orientierung.

Art-Net-seitig meldet sich der Node mit einem Port pro `BindIndex` (4 ArtPollReplies); für die Oberfläche ändert das nichts.

### 4.2 Menübaum (vollständig, wie in `build()` in `ui_core.c`)

```
HOME
├─ [Name] / Drücken ohne Cursor ──► MENU
├─ [IP-Zeile] ──────────────────────► Network (dieselbe Seite wie Menu › Network)
├─ [Port 1…4] ──────────────────────► PORT n          Kopf: „Port n · Output|Input|Off“
│    (Info) Statuszeile                    44fps HTP 2src 12RDM · No data · Hold · 30fps input ·
│                                          No DMX signal · Port disabled · ↓ Tube update 41% · Port error 3
│    ├─ Address        0:0:1        Adress-Editor (Schnell-Edit auf Uni)        [+ Rückfrage, wenn Daten fließen]
│    ├─ Mode           Output       Off / Output / Input                        [+ Rückfrage, wenn Daten fließen]
│    ├─ Devices (12)   ›            RDM-Geräteliste des Ports                   [nur Output + RDM On]
│    ├─ Merge          HTP          HTP / LTP                                   [nur Output]
│    ├─ RDM            On           Off / On                                    [nur Output]
│    ├─ Failsafe       Hold         Hold / Zero / Full / Scene (Scene nur mit aufgenommener Szene) [nur Output]
│    └─ Advanced       ›
│         ├─ Output         Always       Always / Delta (Art-Net Style Constant/Delta)  [nur Output]
│         ├─ Refresh        40 Hz        1–44 Hz                                        [nur Output]
│         ├─ Break          176µs        176–352 µs, Schritt 4                          [nur Output]
│         ├─ MAB            16µs         12–88 µs, Schritt 4                            [nur Output]
│         ├─ Bg discov.     On           Off / On (Hintergrund-Discovery des Ports)     [nur Output + RDM]
│         ├─ Record scene…               Rückfrage; Failsafe-Szene = aktuelle Ausgabe   [nur Output]
│         └─ Reset port…                 Rückfrage; nur dieser Port auf Werkseinstellung
│
└─ MENU                               Kopf: „Menu“
     ├─ Network        ›                           Kopf: „Network“, „●“ wenn etwas vorgemerkt ist
     │    ├─ Mode           DHCP        DHCP / Static (vorgemerkt; Fallback ist ein Zustand, kein Modus)
     │    ├─ IP / Mask      …           bei DHCP: nur Anzeige (grau, Info-Zeilen)
     │    ├─ IP             …           bei Static: IP-Editor (Schnell-Edit auf letztem Oktett), vorgemerkt
     │    ├─ Mask           …           bei Static: Enum /8…/30, als Punktnotation angezeigt, vorgemerkt
     │    ├─ Gateway        …           bei Static: IP-Editor, 0.0.0.0 = keins, vorgemerkt
     │    ├─ Apply…                     nur wenn etwas vorgemerkt ist (cyan) → Rückfrage
     │    └─ Info           ›           Link, State (DHCP/Fallback/Static), IP, Mask, Gateway, MAC
     ├─ Devices (36)   ›           alle RDM-Geräte aller Ports, Zeile „1 001 T-01“ (Port, Adresse, Label)
     ├─ Node           ›
     │    ├─ Identify       Off         Off / On
     │    ├─ Short name     XBAR-NOD    Texteditor (17 Zeichen); in der Liste die ersten 8 Zeichen
     │    ├─ Long name      …           Texteditor (63 Zeichen)
     │    └─ UID            7FF0:8…     Info
     ├─ Display        ›
     │    ├─ Brightness     100 %       5–100 %, Schritt 5, Live-Vorschau (= DISPLAY_LEVEL)
     │    ├─ Dim after      1 min       Never / 30 s / 1 / 2 / 5 / 10 / 30 / 60 min (= DISPLAY_DIM_TIMEOUT)
     │    ├─ Addr format    N:S:U       N:S:U / 15-bit (= DISPLAY_ADDRESS_FORMAT)
     │    └─ Lock panel…                sperrt sofort, ohne Rückfrage (Alternative: Kombi-Griff)
     ├─ Discovery      3 s         1–600 s, Intervall der RDM-Hintergrundsuche, alle Ports (= RDM_BG_DISCOVERY_INTERVAL)
     └─ System         ›
          ├─ Info           ›           Firmware, HW revision, RDM UID, Uptime, ESTA/model, Art-Net OEM
          ├─ Events         ›           letzte 16 Ereignisse (Remote-Änderungen, Fehler, Link, Sperre …)
          ├─ Restart…                   Rückfrage
          └─ Factory reset… (rot)  ›    Keep network / Everything → Rückfrage + 3 s Halten
```

RDM-Geräteseite (aus `Devices`, Kopf `P1 T-03` bzw. `P1 <UID>`):

```
DEVICE
├─ Identify       Off         Off / On; aus, sobald die Seite verlassen wird
├─ DMX addr       007         1–512, sofort per RDM SET (nur bei Footprint > 0 oder gültiger Startadresse)
├─ Mode           1/4         Personality, Name aus PERSONALITY_DESCRIPTION wenn vorhanden („2 RGB“) → Rückfrage
│                             (nur bei mehr als einer Personality)
├─ (Info) Footprint  3 ch
├─ Label          T-03        Texteditor (32 Zeichen, DEVICE_LABEL)
└─ Info           ›           UID, Model, Software, Personality „1 of 4“, Line „Port 1“, ggf. „No response“ rot
```

**Regeln im Baum:**
- **Irrelevantes wird ausgeblendet, nicht ausgegraut.** Ein Eingangs-Port zeigt kein Merge, kein RDM, keine Geräte, keinen Failsafe und in Advanced nur `Reset port…`. Das hält Listen kurz.
  - Ausnahme Netzwerk bei DHCP: IP/Mask bleiben sichtbar (grau), weil man sie *ablesen* will.
- **„Aktiv/deaktiviert“ und „Richtung“ sind ein Enum `Mode: Off/Output/Input`.** Zwei Einstellungen, die man immer zusammen denkt, gehören in eine Zeile.
- **Werte stehen direkt in der Liste.** Man sieht den Zustand, ohne hineinzugehen.
- **Failsafe ist pro Port** (Art-Net `ArtAddress` adressiert ihn pro BindIndex), daher auf der Port-Seite und nicht im Hauptmenü. `Record scene…` liegt unter Advanced.
- **Kein Input-Broadcast.** Die Art-Net-Spec verbietet ihn; ein Eingang sendet per Unicast ([contract.md](../protocol/contract.md) §12). Es gibt deshalb keinen Menüpunkt dafür.

### 4.3 Was ist wie schnell erreichbar (Begründung der Tiefe)

| Aufgabe | Häufigkeit | Weg (D = Drücken, R = Drehen) |
|---|---|---|
| Name, IP, Port-Adressen, Datenfluss ablesen | ständig | Home, 0 Aktionen |
| Universe eines Ports ändern | häufig | R (→ Port), D (Port-Seite, Cursor auf Address), D (Schnell-Edit), R, D = **3 Drücke** |
| Port Ein/Aus/Richtung | gelegentlich | R, D, R (Mode), D, R, D (+ ggf. Rückfrage) |
| Tube adressieren | gelegentlich | Port → Devices → Gerät → DMX addr (Ebene 3) |
| Statische IP setzen | selten | IP-Zeile → Network → Mode/IP/Mask/Gateway → Apply… + Rückfrage |
| Failsafe-Modus | selten | Port → Failsafe (Wert sofort sichtbar) |
| DMX-Timing, Hintergrund-Discovery | sehr selten | Port → Advanced; Intervall: Menu → Discovery |
| Namen eintippen | sehr selten (meist per Software) | Menu → Node → Short name (Texteditor, mühsam: das ist ok) |
| Werksreset | fast nie | Menu → System → Factory reset → Auswahl → Dialog → 3 s halten |

---

## 5. Navigations- und Editierregeln

### 5.1 Grundgesten (`ui_input.c`)

| Eingabe | In Listen | Im Edit-Modus | In Dialogen |
|---|---|---|---|
| **Drehen** rechts/links | Cursor ab/auf | Wert +/− | Button wechseln |
| **Drücken** (beim Loslassen, < 600 ms) | öffnen / Edit starten | **fertig**: übernehmen | ausgewählten Button ausführen |
| **BACK kurz** (beim Loslassen, < 1 s) | eine Ebene zurück | eine Ebene zurück (Wert → Feldwahl → Abbruch) | = Abbrechen |
| **BACK lang** (nach 1 s, einmal) | Home | Home, offene Eingabe wird **verworfen** (Toast `Input discarded`) | Home, Abbruch |
| **BACK + Drücken, 3 s halten** | Panelsperre ein/aus (überall) | ebenso | ebenso |

- Rechtsdrehen bedeutet in Listen „nach unten“ und in Werten „mehr“.
- **Kein Umlauf (Wrap)** bei Listen, Zahlen, Enums und Buttons. Am Ende ist Schluss. Damit springt man nicht versehentlich von 0 auf 32767 oder von „Hold“ auf „Scene“. Einzige Ausnahme ist der Zeichen-Ring im Texteditor.
- **Cursor-Gedächtnis:** Jede Liste merkt sich ihre Cursor-Position, solange sie auf dem Stapel liegt. Home merkt sich das zuletzt gewählte Ziel.
- **Scrollen:** Der Cursor bewegt sich bis zur letzten sichtbaren Zeile, danach scrollt die Liste zeilenweise. Ab 7 Einträgen gibt es einen Scrollbalken rechts. Eine Info-Zeile am Listenanfang (Statuszeile der Port-Seite) bleibt sichtbar, solange der Cursor oben steht.
- Nach dem Kombi-Griff werden beide Loslass-Ereignisse verschluckt.

### 5.2 Editier-Modell: „Drücken = fertig mit dem, was ich gerade tue“

**Einfache Werte (Enum, Zahl, Ein/Aus) – Inline-Edit:**
- Drücken auf eine Zeile → der Wert wird **cyan hinterlegt** (Edit-Modus). Die Kopfzeile wird zur Hinweiszeile (`Turn: change  Push: OK` / `BACK: cancel`).
- Drehen ändert den Wert **nur in der Anzeige**.
- Drücken übernimmt (der Cursorbalken blitzt 120 ms cyan als Quittung). BACK verwirft und zeigt wieder den aktuellen Wert.
- Ein/Aus-Werte werden genauso behandelt, also nicht schon mit einem einzelnen Druck umgeschaltet. Konsistenz ist wichtiger als ein gesparter Druck.
- **Ausnahme Brightness:** Live-Vorschau beim Drehen (Hinweis `BACK: cancel  (preview)`), BACK stellt den alten Wert wieder her.
- Übernehmen schreibt eine frische Kopie der Einstellungen, in der **nur dieses Feld** geändert ist. Was Pixelmapper inzwischen an anderen Feldern geändert hat, bleibt erhalten.

**Wann eine Änderung wirksam wird:**
- **Sofort bei „fertig“:** alle Port-Einstellungen, Discovery, Anzeige, Namen, RDM-Gerätewerte (per RDM SET), Identify.
- **Vorgemerkt (staged):** **nur das Netzwerk**. Mode, IP, Mask und Gateway sammeln sich. Die Kopfzeile zeigt rechts ein cyan „●“, geänderte Werte stehen cyan statt weiß, und `Apply…` erscheint (cyan). Wirksam wird erst `Apply…` mit Rückfrage.
  - Verlässt man die Netzwerk-Seite mit BACK und vorgemerkten Werten, fragt der Dialog „Network changes“: `[Discard] [Apply]`, Fokus auf **Discard**. BACK im Dialog = auf der Netzwerkseite bleiben.
  - BACK lang verwirft vorgemerkte Werte (Toast `Network changes discarded`).

**Zahlen mit Beschleunigung:**
- Bis 50 Schritte Bereich: keine Beschleunigung.
- Darüber bestimmt der mittlere Abstand der letzten 3 Rasten den Faktor:
  - mehr als 80 ms: ×1
  - 30–80 ms: ×10
  - unter 30 ms: ×100
  - Kommen mehrere Rasten in einer Abfrage, gilt ×10 (2 Rasten) bzw. ×100 (ab 3).
  - Der Faktor ist auf 1/20 des Bereichs begrenzt.
- Bei Wechsel der Drehrichtung fällt der Faktor sofort auf ×1 zurück. Das verhindert Überschießen.
- Werte mit festem Raster (Break/MAB in 4-µs-Schritten, Brightness in 5 %) beschleunigen in Vielfachen ihres Rasters.

**Feld-Editor (Port-Address, IP, Gateway) – Vollbild:**
- Der **Schnell-Edit** ist der Einstieg: Drücken öffnet den Editor direkt im Edit-Modus des *wahrscheinlichsten* Felds. Bei der Port-Address ist das **Uni** (bzw. die 15-Bit-Zahl), bei IP und Gateway das **letzte Oktett**.
  - Drehen ändert dieses Feld, **Drücken übernimmt den ganzen Wert und schließt** den Editor.
  - Das ist der 3-Drücke-Weg für die häufigste Änderung.
- **BACK kurz** im Schnell-Edit führt in die **Feldwahl**. Ein weißer Rahmen springt per Drehen über `Net · Sub · Uni · OK` (bzw. `15-bit · OK` oder die 4 Oktette und `OK`).
  - Drücken auf ein Feld → Feld-Edit (cyan). Drücken beendet das Feld und führt zurück zur Feldwahl, BACK setzt nur dieses Feld zurück (`BACK: undo field`).
  - `OK` übernimmt alles. BACK in der Feldwahl bricht ab.
  - **BACK zweimal bricht also immer ab**, das ist konsistent mit „eine Ebene zurück“.
  - Hinweiszeilen unten: `Push: OK   BACK: fields` / `Turn: change (with carry)`, in der Feldwahl `Turn: field  Push: edit` / `BACK: cancel`.
- **Port-Address:**
  - **Der Editor zeigt immer beide Formate.** Im Format N:S:U stehen oben die drei Felder `Net` (0–127), `Sub` (0–15), `Uni` (0–15) und darunter grau die 15-Bit-Zahl. Im Format 15-bit steht oben die Zahl 0–32767 als einziges Feld und darunter grau N:S:U.
  - **Uni hat Übertrag:** Im Schnell-Edit zählt Drehen die ganze Port-Address (über 0:0:15 kommt 0:1:0, über 0:15:15 kommt 1:0:0). Damit ist das Uni-Feld zugleich ein 15-Bit-Zähler, und man kommt fast immer ohne Feldwahl aus. In der Feldwahl ändern Net/Sub/Uni nur sich selbst.
  - Unveränderter Wert + Drücken schließt ohne Schreiben.
- **IP / Gateway:**
  - Oktette 0–255 mit Beschleunigung, kein Übertrag. Über den Oktetten `Static IP address` bzw. `Gateway (0.0.0.0 = none)`, darunter grau `active: …` (was gerade wirklich gilt) und cyan `● staged`.
  - Ungültige Werte werden beim Übernehmen **abgelehnt**, der Editor bleibt offen und die Hinweiszeile zeigt gelb den Grund: `Invalid for /24` (0.0.0.0, erstes Oktett 0/127/≥ 224, Netz- oder Broadcast-Adresse zur Maske) bzw. für das Gateway `Not in the IP's /24 net`.
- **Mask:** keine Oktett-Eingabe, sondern ein Inline-Enum der Präfixe /8 bis /30, angezeigt als `255.255.255.0`. Ungültige Masken sind so unmöglich.

**Texteditor (Short name, Long name, RDM-Label) – Vollbild:**
- Zeile 1: der Text (F-N) mit einem cyan Unterstrich-Cursor am Ende. Längere Texte scrollen nach links mit „…“ vorne.
- Darunter rechts der Zähler `12/17` (F-S, grau) und eine Trennlinie.
- Der **Zeichen-Ring** zeigt 7 Zeichen, die Auswahl in der Mitte cyan:
  `✓ · ⌫ · ␣ · A–Z · a–z · 0–9 · - _ . / # + ( ) :`
  - Drehen wählt das Zeichen; der Ring läuft um (die einzige Stelle mit Wrap).
  - **Drücken hängt das Zeichen an.** Der Ring bleibt auf dem gewählten Zeichen stehen, so ist das nächste Zeichen meist nah.
  - Unter dem Ring steht bei Sonderzeichen ihr Name: `Done`, `Delete`, `Space`.
  - `⌫` löscht das letzte Zeichen. **Drücken und 1 s halten auf ⌫** löscht den ganzen Text (roter Balken füllt sich). Hinweis: `Push: delete  Hold: all`.
  - `✓` übernimmt (`Push: apply`). BACK kurz bricht ab. Bei geändertem Text kommt erst der Dialog `⚠ Discard text?` mit `[Keep] [Discard]`, Fokus auf Keep.
  - Beim Öffnen steht der Ring auf `✓`. Mit „D“ ist man also sofort wieder draußen, mit „R, D“ auf `⌫` korrigiert man das Ende.
- Mitten im Text editieren gibt es nicht, das ist bewusst so. Wer viel ändern will, löscht oder nutzt Pixelmapper.

### 5.3 Rückfragen

**Einheitlicher Dialog:**
- Statt des Pfads steht ein Titel in F-N mit ⚠: gelb, beim Werksreset rot.
- Bis zu 4 Zeilen Klartext sagen, *was passiert*.
- Zwei Buttons. **Der Fokus steht immer auf dem sicheren Button links** (`Cancel`, `Discard`, `Keep`).
- Hinweiszeile `Turn: choose  Push: OK`.

Rückfrage gibt es nur hier (sonst wird Bestätigen zur Gewohnheit). **Von Pixelmapper aus gibt es nie Rückfragen.**

| Aktion | Text / Zusatz |
|---|---|
| `Mode` eines Ports ändern, **während der Port Daten führt** | `⚠ Port mode`: „Port 2 is sending data (44 fps). Output stops.“ Ohne Datenfluss keine Rückfrage. |
| `Address` eines Ports ändern, **während der Port Daten führt** | `⚠ Port-Address`: „Port 2 is sending data (44 fps). New address 0:0:3“ |
| Network `Apply…` | `⚠ Change network`: „Changing the IP interrupts Art-Net connections.“ + neue IP/Präfix bzw. `New: DHCP` (cyan); Button `Apply` |
| Netzwerkseite mit vorgemerkten Werten verlassen | `⚠ Network changes`, `[Discard] [Apply]` |
| `Record scene…` | `⚠ Record scene`: „Stores the current output of port n as its failsafe scene.“ |
| `Reset port…` | `⚠ Reset port` |
| `Restart…` | `⚠ Restart`: „Output stops for a few seconds.“ |
| RDM-Gerät: Personality (`Mode`) ändern | `⚠ Personality`: „Footprint changes 3 → 12 channels. Check the patch.“ |
| `Factory reset…` | Vorher Auswahl `Keep network` / `Everything` (rot). Dann `⚠ Factory reset` (rot) und **zusätzlich 3 s Halten** des Encoders auf `Reset` (der Button füllt sich weiß von links). |

„Daten führen“ heißt: Ausgang mit fps > 0 und ohne Datenverlust, bzw. Eingang mit DMX-Signal.

**Undo:** Es gibt kein Undo nach dem Übernehmen. Das Verwerfen (BACK) *vor* dem Übernehmen ist das Undo. Die Ereignisliste zeigt, was sich geändert hat, damit man von Hand zurückstellen kann. Ein Undo-Stack wäre auf 20 Zeichen eher Verwirrung als Hilfe.

### 5.4 Timeouts, Dimmen, LED Mute

| Was | Zeit |
|---|---|
| Menü ohne Eingabe → Home | 60 s |
| Offene Eingabe (Inline-Edit, Vollbild-Editor) oder vorgemerktes Netzwerk → verwerfen, Home, Toast `Input discarded` (gelb), Ereignis | 120 s |
| Cursor auf Home ausblenden | 10 s |
| Toast | 3 s |
| Remote geänderter Wert orange | 3 s |
| Hinweis „↻ changed by Pixelmapper“ | 4 s |
| Sperr-Overlay nach Bedienversuch | 3 s (BACK schließt es sofort) |
| Port-Fehler rot (Kachel, `ERR`, Statuszeile) | 10 s nach dem Ereignis |
| Aufwecken nach Ereignis | 30 s |

- **Helligkeit** = `Display › Brightness` = Standard-PID **DISPLAY_LEVEL**, 5–100 % in 5-%-Schritten (nie unter 5 %).
- **Dimmen:** Nach `Dim after` (= **DISPLAY_DIM_TIMEOUT**; Never / 30 s / 1 / 2 / 5 / 10 / 30 / 60 min, Werkseinstellung 1 min) ohne Eingabe dimmt das Display auf das Minimum (~5 %). **Der Timeout schaltet nie ganz aus.** Die Statusleiste ersetzt die LEDs, ein schwarzes Display wäre ein Gerät ohne Statusanzeige.
- **Die erste Eingabe am gedimmten oder ausgeschalteten Display weckt nur auf und löst nichts aus.** Das verhindert Fehlbedienung im Dunkeln. Ausnahme: Der Kombi-Griff für die Sperre wirkt auch gedimmt.
- **Automatisches Aufwecken** (volle Helligkeit für 30 s, danach wieder dimmen):
  - Port- oder Systemfehler
  - Datenverlust
  - Link weg
  - Identify / LED Locate
  - Node-Update (während des Updates wird nicht gedimmt)
  - **Kein** Aufwecken bei Remote-Konfig-Änderungen, die würden im Showbetrieb dauernd flackern.
- **LED Mute** schaltet das Display ganz aus (3.5).
- LCD-Einbrennen ist bei TFT kein reales Problem. Ein Pixel-Shift ist nicht nötig.

### 5.5 Panelsperre

- **Sperren:** Kombi-Griff (BACK + Encoder drücken, 3 s halten), `Menu › Display › Lock panel…` oder remote per Hersteller-PID `PANEL_LOCK`.
  - Ab 150 ms Halten erscheint das Sperr-Overlay mit Fortschrittsbalken (`Locking…` bzw. `Unlocking…`). Loslassen vorher = nichts passiert.
- **Sperren schließt offene Editoren und Dialoge** (Inline-Edit, Vollbild-Editoren, Rückfragen); vorgemerkte Netzwerkwerte bleiben vorgemerkt.
- **Gesperrt heißt Nur-Ansehen:**
  - Navigieren, Listen ansehen und Ports öffnen bleibt möglich. Man soll im Fehlerfall sehen können, was los ist.
  - **Jeder Versuch zu editieren oder eine Aktion auszulösen** (Drücken auf einen Wert oder eine `…`-Zeile) zeigt das Sperr-Overlay für 3 s.
  - Auch Identify (Node und RDM-Geräte) ist gesperrt.
- **Entsperren:** derselbe Kombi-Griff, 3 s, mit Fortschrittsbalken im Overlay.
  - Das gilt **auch bei einer Sperre durch Pixelmapper**. Physischer Zugang hat Vorrang (z. B. Netzwerk tot und die Sperre muss weg). Kein PIN.
  - Das Aufheben wird gemeldet: Queued Message `PANEL_LOCK` + Statusmeldung **`PANEL_UNLOCKED_LOCAL`** (0x8003, [contract.md](../protocol/contract.md) §6.3), dazu ein Ereignis `Remote lock released`.
- Sperre aktiv: Schloss in der Statusleiste, weiß = lokal, orange = Pixelmapper. Das Overlay sagt dann zusätzlich `(by Pixelmapper)`.

---

## 6. ASCII-Mockups

**Notation:**
- Jede Zeile zwischen `|…|` hat genau **20 Zeichen** (F-N-Raster 8 × 16).
- Zeilen zwischen `¦…¦` sind **F-S** (6 × 8) und haben genau **26 Zeichen**: Kopfzeile/Pfad, Hinweise, Toasts, IP-Zeile auf Home, Feldbeschriftungen. Sie sind halb so hoch wie eine F-N-Zeile; zwei F-S-Zeilen füllen eine 16-px-Zeile.
- `>` vor dem Rahmen = Zeile invertiert (Cursor).
- `[x]` = Feld im Edit-Modus (cyan).
- `{x}` = Button mit Fokus (gefüllt), `(x)` = Button ohne Fokus (Rahmen).
- Farbangaben stehen rechts.
- Statusleisten-Zeichen:
  - `>` `<` `-` = ▸ ◂ – in den Port-Kacheln (Spalten 1–12)
  - `▲` = Link, `D/S/F` = IP-Modus, `▣` = Schloss, `↓` = Update (Spalten 14, 16, 18, 20)
- Die Trennlinie unter der Kopfzeile ist nicht gezeichnet.

### 6.1 Home

```
   |1> 2> 3< 4-  ▲ D    |  Kacheln: 1, 2 grün; 3 blau; 4 grau
   |XBAR-NODE-01        |  Short Name, weiß
   ¦192.168.100.217 /24  DHCP ¦  F-S, weiß
   |1> 0:0:0      44 R12|  Badge grün
   |2> 0:0:1      44 MRG|  Badge grün
   |3< 0:1:0      30    |  Badge blau
   |4  off              |  grau
```
![](../node/firmware/components/pm_ui/host/out/6.1-home.png)

Mit Cursor (nach erster Raste) und Datenverlust auf Port 2:
```
   |1> 2> 3< 4-  ▲ D    |  Kachel 2 blinkt gelb (1 Hz)
   |XBAR-NODE-01        |
   ¦192.168.100.217 /24  DHCP ¦
   |1> 0:0:0      44 R12|
  >|2> 0:0:1      -- HLD|  invertiert, ganze 21-px-Zeile
   |3< 0:1:0      30    |
   |4  off              |
```
![](../node/firmware/components/pm_ui/host/out/6.1-home-cursor-dataloss.png)

Varianten: Adressformat 15-bit ([6.1-home-15bit.png](../node/firmware/components/pm_ui/host/out/6.1-home-15bit.png)),
ohne Link `No link` rot ([home-no-link.png](../node/firmware/components/pm_ui/host/out/home-no-link.png)),
Fallback gelb ([home-fallback-ip.png](../node/firmware/components/pm_ui/host/out/home-fallback-ip.png)),
DHCP läuft ([home-dhcp-pending.png](../node/firmware/components/pm_ui/host/out/home-dhcp-pending.png)),
Fehler / Eingang ohne Signal / Port ohne Daten ([home-error-idle-ports.png](../node/firmware/components/pm_ui/host/out/home-error-idle-ports.png)).

### 6.2 Port-Übersicht

Die Port-Übersicht **ist** der Home-Screen (6.1). Es gibt bewusst keine zusätzliche Übersicht, die nur Home wiederholen würde. Wer alle Details aller Ports braucht, öffnet die Ports einzeln.

### 6.3 Port-Seite

```
   |1> 2> 3< 4-  ▲ D    |
   ¦Port 2 · Output           ¦  F-S, Pfad
   |44fps HTP 2src 2RDM |  Info-Zeile, grau (Datenverlust: gelb „No data · Hold“)
  >|Address        0:0:1|
   |Mode          Output|
   |Devices (2)        ›|
   |Merge            HTP|
   |RDM               On|  Scrollbalken: darunter Failsafe, Advanced ›
```
![](../node/firmware/components/pm_ui/host/out/6.3-port-detail.png)

Advanced (Ausgang):
```
   ¦Port 2 · Output › Advanced¦
  >|Output        Always|
   |Refresh        40 Hz|
   |Break          176µs|
   |MAB             16µs|
   |Bg discov.        On|
   |Record scene…       |  Scrollbalken: darunter Reset port…
```
![](../node/firmware/components/pm_ui/host/out/6.3-port-advanced.png)

Eingang und deaktivierter Port: [port-input-detail.png](../node/firmware/components/pm_ui/host/out/port-input-detail.png),
[port-off-detail.png](../node/firmware/components/pm_ui/host/out/port-off-detail.png).

### 6.4 Port-Address editieren (Schnell-Edit direkt nach Drücken)

```
   |1> 2> 3< 4-  ▲ D    |  Kachel 1: weißer Rahmen (aktiv, keine Daten)
   ¦Port 1 · Output › Address ¦
   ¦    Net      Sub     Uni  ¦  F-S, grau
   |     0 :   0 : [  0]|  Uni cyan
   |                    |
   | 15-bit           0 |  grau, läuft live mit
   |                    |
   ¦Push: OK   BACK: fields   ¦  cyan
   ¦Turn: change (with carry) ¦  grau
```
![](../node/firmware/components/pm_ui/host/out/6.4-address-quick-edit.png)

Nach BACK (Feldwahl) springt ein weißer Rahmen per Drehen über `Net`, `Sub`, `Uni` und den Button `OK` unten rechts
([6.4-address-field-select.png](../node/firmware/components/pm_ui/host/out/6.4-address-field-select.png),
[6.4-address-ok-focus.png](../node/firmware/components/pm_ui/host/out/6.4-address-ok-focus.png)).

Adressformat 15-bit (oben die Zahl, unten N:S:U):
```
   ¦Port 1 · Output › Address ¦
   |                    |
   | 15-bit      [ 1002]|  cyan
   ¦    Net      Sub     Uni  ¦  grau
   |     3 :  14 :   10 |  grau
   |                    |
   ¦Push: OK   BACK: fields   ¦
   ¦Turn: change              ¦
```
![](../node/firmware/components/pm_ui/host/out/6.4-address-15bit.png)

Führt der Port gerade Daten, fragt der Editor vor dem Schreiben:
```
   |1> 2> 3< 4-  ▲ D    |
   |⚠ Port-Address      |  gelb, F-N
   |Port 2 is sending   |
   |data (44 fps).      |
   |New address 0:0:3   |
   |                    |
   | {Cancel}  (Change) |  Fokus links
   ¦Turn: choose  Push: OK    ¦  grau
```
![](../node/firmware/components/pm_ui/host/out/6.4-address-confirm-data.png)

### 6.5 Netzwerk (Modus Static, Werte vorgemerkt)

```
   |1> 2> 3< 4-  ▲ D    |
   ¦Network                  ●¦  ● cyan = vorgemerkt
  >|Mode          Static|  (cyan, wenn nicht unter dem Cursor)
   |IP   192.168.100.217|  cyan
   |Mask   255.255.255.0|  cyan
   |Gateway      0.0.0.0|
   |Apply…              |  cyan
   |Info               ›|
```
![](../node/firmware/components/pm_ui/host/out/6.5-network-static-staged.png)

IP editieren (Schnell-Edit auf dem letzten Oktett):
```
   |1> 2> 3< 4-  ▲ D    |
   ¦Network › IP              ¦
   ¦Static IP address         ¦  grau
   |                    |
   | 192. 168. 100.[217]|  letztes Oktett cyan
   ¦active: 192.168.100.217   ¦  grau: was gerade wirklich gilt
   ¦● staged                  ¦  cyan
   ¦Push: OK   BACK: fields   ¦  cyan; bei ungültigem Wert gelb „Invalid for /24“
   ¦Turn: change last octet   ¦  grau
```
![](../node/firmware/components/pm_ui/host/out/6.5-ip-quick-edit.png)

Abgelehnte Adresse: [6.5-ip-invalid.png](../node/firmware/components/pm_ui/host/out/6.5-ip-invalid.png).
Netzwerk unter DHCP: [6.5-network-dhcp.png](../node/firmware/components/pm_ui/host/out/6.5-network-dhcp.png).
Verlassen mit vorgemerkten Werten: [6.5-network-leave-staged.png](../node/firmware/components/pm_ui/host/out/6.5-network-leave-staged.png).

### 6.6 Namen editieren

```
   |1> 2> 3< 4-  ▲ D    |
   ¦Menu › Node › Short name  ¦
   |XBAR-NODE-01_       |  Cursor-Unterstrich cyan
   ¦                     12/17¦  grau
   |                    |  (Trennlinie)
   | (  )  :[✓] ⌫  ␣  A |  Ring, Auswahl cyan
   ¦           Done           ¦
   ¦Push: apply               ¦  cyan
   ¦BACK: cancel              ¦  grau
```
![](../node/firmware/components/pm_ui/host/out/6.6-short-name.png)

Ring auf einem Buchstaben:
```
   | ␣  A  B[C] D  E  F |
   ¦Push: add character       ¦
```
![](../node/firmware/components/pm_ui/host/out/6.6-short-name-ring.png)

Ring auf `⌫`: `Push: delete  Hold: all`, Halten füllt einen roten Balken
([6.6-short-name-clear-hold.png](../node/firmware/components/pm_ui/host/out/6.6-short-name-clear-hold.png)).
BACK mit geändertem Text:
```
   |⚠ Discard text?     |  gelb
   |The changed text    |
   |will be lost.       |
   |                    |
   |                    |
   |  {Keep}  (Discard) |
   ¦Turn: choose  Push: OK    ¦
```
![](../node/firmware/components/pm_ui/host/out/6.6-discard-dialog.png)

### 6.7 RDM-Geräte

Geräteliste eines Ports (sortiert wie die TOD):
```
   |1> 2> 3< 4-  ▲ D    |  Kachel: violette Unterkante bei RDM-Verkehr
   ¦… › Devices (12)          ¦
  >|↻ Rescan            |  Full Discovery (Hintergrundsuche läuft ohnehin)
   |001 T-01            |  DMX-Adresse · Label (ohne Label: UID)
   |004 T-02            |
   |007!T-03            |  rot: Footprint überlappt
   |009!T-04            |  rot
   |013 T-05            |  Scrollbalken
```
![](../node/firmware/components/pm_ui/host/out/6.7-devices-port.png)

Alle Ports (`Menu › Devices`), vorne die Portnummer:
```
   ¦Menu › Devices (14)       ¦
  >|↻ Rescan            |
   |1 001 T-01          |
   |1 004 T-02          |
   |1 007!T-03          |  rot
```
![](../node/firmware/components/pm_ui/host/out/6.7-devices-all.png)

Gerät geöffnet:
```
   |1> 2> 3< 4-  ▲ D    |
   ¦… › Devices (12) › P1 T-03¦
  >|Identify         Off|  aus, sobald die Seite verlassen wird
   |DMX addr         007|  1–512, sofort per RDM SET; während des SET „…“
   |Mode             1/4|  Personality → Rückfrage
   |Footprint       3 ch|  grau, Info
   |Label           T-03|  Texteditor (32 Zeichen)
   |Info               ›|
```
![](../node/firmware/components/pm_ui/host/out/6.7-device-page.png)

- Identify an: In der Geräteliste steht ☼ hinter dem Gerät ([6.7-device-identify-on.png](../node/firmware/components/pm_ui/host/out/6.7-device-identify-on.png)).
- Kommt auf ein `SET` kein ACK (NACK/Timeout), bleibt der alte Wert stehen, Toast rot `Device not responding` bzw. `Device rejected (NACK)`, das Gerät wird in der Liste grau ([6.7-device-not-responding.png](../node/firmware/components/pm_ui/host/out/6.7-device-not-responding.png)).
- Jedes SET vom Display wird Pixelmapper gemeldet (`LOCAL_RDM_SET`, [contract.md](../protocol/contract.md) §7).
- Tube-Firmware-Updates werden hier nur angezeigt (6.10), nie gestartet.

Personality ändern:
```
   |⚠ Personality       |  gelb
   |Footprint changes   |
   |3 → 12 channels.    |
   |Check the patch.    |
   |                    |
   | {Cancel}  (Change) |
   ¦Turn: choose  Push: OK    ¦
```
![](../node/firmware/components/pm_ui/host/out/6.7-device-personality-confirm.png)

### 6.8 Rückfragen

Netzwerk übernehmen:
```
   |1> 2> 3< 4-  ▲ D    |
   |⚠ Change network    |  gelb
   |Changing the IP     |
   |interrupts Art-Net  |
   |connections.        |
   |192.168.100.212/24  |  cyan
   | {Cancel}  (Apply)  |
   ¦Turn: choose  Push: OK    ¦
```
![](../node/firmware/components/pm_ui/host/out/6.8-confirm-network.png)

Werksreset (nach Auswahl `Everything`, Encoder wird gehalten):
```
   |1> 2> 3< 4-  ▲ D    |
   |⚠ Factory reset     |  rot
   |All settings        |
   |incl. network       |
   |to factory state.   |
   |                    |
   | (Cancel)  {Reset}  |  Reset rot gefüllt; Halten füllt ihn weiß von links
   ¦Hold push 3 s on Reset    ¦
```
![](../node/firmware/components/pm_ui/host/out/6.8-factory-hold.png)

Auswahl davor: [6.8-factory-choice.png](../node/firmware/components/pm_ui/host/out/6.8-factory-choice.png),
Neustart: [6.8-restart-confirm.png](../node/firmware/components/pm_ui/host/out/6.8-restart-confirm.png).

### 6.9 Remote-Änderung und gleichzeitige Eingabe

Remote-Änderung, während man auf Home ist:
```
   |1> 2> 3< 4-  ▲ D    |
   |XBAR-NODE-01        |
   ¦↻ P2 Address → 0:1:4      ¦  Toast orange, 3 s, statt der IP-Zeile
   |1> 0:0:0      44 R12|
   |2> 0:1:4      44 MRG|  „0:1:4“ 3 s orange
   |3< 0:1:0      30    |
   |4  off              |
```
![](../node/firmware/components/pm_ui/host/out/6.9-remote-change-home.png)

Mehrere Felder in einer Änderung: `↻ 5 changes by Pixelmapper`
([6.9-remote-multi-change.png](../node/firmware/components/pm_ui/host/out/6.9-remote-multi-change.png)).

Man editiert Merge inline, und Pixelmapper setzt Merge. Der Hinweis steht zwei Zeilen hoch in der Kopfzeile:
```
   |1> 2> 3< 4-  ▲ D    |
   ¦↻ changed by Pixelmapper  ¦  orange, 4 s
   ¦now: LTP                  ¦  orange: der neue Wert
   |44fps LTP 12RDM     |
   |Address        0:0:0|
   |Mode          Output|
   |Devices (12)       ›|
   |Merge          [LTP]|  eigene Eingabe bleibt offen (cyan)
```
![](../node/firmware/components/pm_ui/host/out/6.9-conflict-inline.png)

Man editiert die Adresse von Port 1 im Vollbild-Editor, und Pixelmapper setzt eine neue. Der Hinweis steht unten in den
Hinweiszeilen, oben läuft der normale Toast:
```
   |1> 2> 3< 4-  ▲ D    |
   ¦↻ P1 Address → 0:1:4      ¦  Toast orange, 3 s
   ¦    Net      Sub     Uni  ¦
   |     0 :   0 : [  4]|  eigene Eingabe bleibt unangetastet (cyan)
   |                    |
   | 15-bit           4 |
   |                    |
   ¦↻ changed by Pixelmapper  ¦  orange, 4 s
   ¦now: 0:1:4                ¦  orange
```
![](../node/firmware/components/pm_ui/host/out/6.9-conflict-address-editor.png)

Drückt man danach, gilt der eigene Wert (wer zuletzt schreibt, gewinnt), und der Hinweis verschwindet
([6.9-conflict-local-wins.png](../node/firmware/components/pm_ui/host/out/6.9-conflict-local-wins.png)). BACK verwirft
die eigene Eingabe, dann bleibt der Wert von Pixelmapper. Es gibt keinen Dialog.

Verschwindet das bearbeitete Feld (z. B. Port wird auf Input gestellt, während man Merge editiert), schließt der
Inline-Edit und der Toast `↻ Page changed by Pixelmapper` erscheint
([remote-page-invalidated.png](../node/firmware/components/pm_ui/host/out/remote-page-invalidated.png)).

### 6.10 Update läuft

Node-Firmware (Vollbild ab dem ersten Fortschritt, Eingaben gesperrt, BACK/Drücken zeigen nur `Please wait`):
```
   |1> 2> 3< 4-  ▲ D   ↓|  ↓ cyan + 2-px-Fortschritt
   |Firmware update     |
   |                    |
   |        71%         |  F-N doppelt (16 × 32)
   |                    |
   |[██████████░░░░░░]  |  Balken cyan
   |  Do not power off  |  gelb
   ¦     Receiving image      ¦  grau; bei 100 % „Restarting…“
```
![](../node/firmware/components/pm_ui/host/out/6.10-node-firmware-update.png)

Tube-Update über RDM (nicht modal, nur Anzeige): Fortschritt an der Unterkante der Port-Kachel, ↓ in der System-Zone,
Home-Kürzel `↓41` ([6.10-tube-update-home.png](../node/firmware/components/pm_ui/host/out/6.10-tube-update-home.png)).
Auf der Port-Seite:
```
   |1> 2> 3< 4-  ▲ D   ↓|  Kachel 1: Unterkante cyan bis 41 %
   ¦Port 1 · Output           ¦
   |↓ Tube update 41%   |  cyan
  >|Address        0:0:0|
   |Mode          Output|
```
![](../node/firmware/components/pm_ui/host/out/6.10-tube-update-port.png)

### 6.11 Panel gesperrt (Overlay nach Bedienversuch)

```
   |1> 2> 3< 4-  ▲ D ▣  |  ▣ orange = von Pixelmapper gesperrt
   |                    |
   |         ▣          |
   |    Panel locked    |
   ¦     (by Pixelmapper)     ¦  orange, nur bei Remote-Sperre
   ¦                          ¦
   ¦ Unlock: hold BACK + push ¦  grau
   ¦      for 3 seconds       ¦  grau
```
![](../node/firmware/components/pm_ui/host/out/6.11-lock-overlay-remote.png)

Beim Halten von BACK + Drücken:
```
   |    Panel locked    |
   ¦     (by Pixelmapper)     ¦
   |  [██████░░░░░░░░]  |  Balken cyan, 3 s
   ¦        Unlocking…        ¦
```
![](../node/firmware/components/pm_ui/host/out/6.11-unlock-combo-progress.png)

Danach Toast `Panel unlocked` ([6.11-unlocked.png](../node/firmware/components/pm_ui/host/out/6.11-unlocked.png)).
Lokale Sperre (Schloss weiß): [lock-local.png](../node/firmware/components/pm_ui/host/out/lock-local.png).

### 6.12 Identify (Statusleiste)

```
   |    ☼ IDENTIFY ☼    |  ganze Leiste weiß, Text schwarz; im Wechsel (2 Hz) mit der normalen Leiste
   |XBAR-NODE-01        |
   ¦192.168.100.217 /24  DHCP ¦
   |1> 0:0:0      44 R12|
   |2> 0:0:1      44 MRG|
   |3< 0:1:0      30    |
   |4  off              |
```
![](../node/firmware/components/pm_ui/host/out/6.12-identify-bar-on.png)

Andere Halbperiode: [6.12-identify-bar-off.png](../node/firmware/components/pm_ui/host/out/6.12-identify-bar-off.png).
In einem Menü genauso, der Inhalt bleibt bedienbar:
```
   |    ☼ IDENTIFY ☼    |
   ¦Menu                      ¦
  >|Network            ›|
   |Devices (14)       ›|
   |Node               ›|
   |Display            ›|
   |Discovery        3 s|
   |System             ›|
```
![](../node/firmware/components/pm_ui/host/out/6.12-identify-in-menu.png)

### 6.13 Weitere Seiten

Hauptmenü [4.2-main-menu.png](../node/firmware/components/pm_ui/host/out/4.2-main-menu.png),
Display [4.2-display-menu.png](../node/firmware/components/pm_ui/host/out/4.2-display-menu.png),
Node [4.2-node-menu.png](../node/firmware/components/pm_ui/host/out/4.2-node-menu.png),
System [4.2-system-menu.png](../node/firmware/components/pm_ui/host/out/4.2-system-menu.png),
System-Info [4.2-system-info.png](../node/firmware/components/pm_ui/host/out/4.2-system-info.png),
Ereignisse [4.2-events.png](../node/firmware/components/pm_ui/host/out/4.2-events.png),
Netzwerk-Info [4.2-network-info.png](../node/firmware/components/pm_ui/host/out/4.2-network-info.png),
gedimmtes Display [display-dimmed-content.png](../node/firmware/components/pm_ui/host/out/display-dimmed-content.png).

---

## 7. Verhalten bei Ereignissen

| Ereignis | Statusleiste | Inhalt | Fokus | Display | Ereignisliste |
|---|---|---|---|---|---|
| **Remote-Konfig-Änderung** (ArtAddress, ArtIpProg, RDM SET an den Node) | – | Wert, falls sichtbar, sofort neu und 3 s orange. Toast orange (Home: statt IP-Zeile, sonst Kopfzeile), z. B. `↻ P2 Address → 0:1:4`, `↻ Name → …`, `↻ Discovery → 5 s`. Mehrere Felder auf einmal: `↻ 5 changes by Pixelmapper` | bleibt | kein Wecken | ja |
| **Remote-Änderung am gerade bearbeiteten Wert** (Inline-Edit, Adress-Editor, Namen, vorgemerktes Netzwerk) | – | siehe 6.9: Eingabe bleibt, Hinweis `↻ changed by Pixelmapper` + `now: …` 4 s; wer danach bestätigt, gewinnt | bleibt | – | ja |
| **Remote-Änderung macht das bearbeitete Feld ungültig** | – | Inline-Edit schließt, Toast `↻ Page changed by Pixelmapper` | Seite bleibt | – | – |
| **Datenverlust / Failsafe** | Kachel gelb blinkend | Home: fps `--` und `HLD`/`ZER`/`FUL`/`SCN`. Port-Seite: gelb `No data · Hold` | bleibt, kein Popup | Wecken | `P2 data lost` |
| **Daten wieder da** | zurück auf grün/blau | normal | – | – | `P2 data back` |
| **Merge** (zwei Quellen) | – | Home `MRG`, Port-Seite `2src` | – | – | – |
| **Identify Node** (RDM IDENTIFY_DEVICE, LED Locate, Node › Identify) | ganze Leiste blinkt weiß `☼ IDENTIFY ☼` (2 Hz) | unverändert | bleibt | Wecken | `Identify on/off`, `Locate (Art-Net)` |
| **LED Mute / LED Normal** | – | Display aus / wieder an | – | aus / an | – |
| **RDM-Identify eines Geräts** (vom Display) | – | ☼ in der Geräteliste; aus beim Verlassen der Geräteseite | – | – | – |
| **Port-Fehler** | Kachel rot, „!“ (10 s) | Home `ERR`, Port-Seite `Port error n` rot, Toast rot `P1 error n` | – | Wecken | ja |
| **Systemfehler** | „!“ rot | Toast rot `System error n` | – | Wecken | ja |
| **Link weg** | ✕ rot | IP-Zeile `No link` rot. Ausgänge laufen nach Timeout in Failsafe → gelb | – | Wecken | `Link down` |
| **Link wieder da** | ▲ grün, „D“ blinkt grau während DHCP | IP-Zeile `DHCP…`, dann die IP | – | – | `Link up` |
| **DHCP fehlgeschlagen → Fallback** | „F“ gelb | IP-Zeile gelb `2.x.x.x /8  Fallback` | – | – | – |
| **Node-Update** | ↓ cyan + Fortschritt | Vollbild 6.10 ab dem ersten Fortschritt, offene Eingaben verworfen, Eingaben aus; Fehler: Toast rot `Firmware update failed`, „!“ | ja | voll, kein Dimmen | Start / Fehler |
| **Tube-Update** (nur Anzeige) | ↓ cyan, Fortschritt an der Port-Kante | Home `↓41`, Port-Seite `↓ Tube update 41%` | – | – | Start |
| **BOOT_CATCH** | – | Toast `P1 boot catch caught` (gelb bei `timed out`) | – | – | ja |
| **Panelsperre durch Pixelmapper** | Schloss orange | offene Editoren schließen, Toast `↻ Panel locked` | eine Ebene hoch | – | `Locked by Pixelmapper` |
| **RDM SET vom Display scheitert** | – | alter Wert bleibt, Toast rot `Device not responding` / `Device rejected (NACK)` | – | – | – |

Die Ereignisliste (`System › Events`) hält die letzten 16 Einträge à 20 Zeichen im RAM, neueste oben.

---

## 8. Entscheidungen und offene Fragen

### 8.1 Entscheidungen (Stand Phase 1)

1. **UI-Sprache: Englisch**, alle Texte im Code und in den Mockups.
2. **Home:** Short Name oben (F-N), darunter IP + Modus (F-S), darunter alle 4 Ports. Keine Kopfzeile. Der Name hilft bei mehreren Nodes im Rack, die IP braucht man zum Verbinden; beides passt.
3. **Wer zuletzt schreibt, gewinnt – ohne Dialog.** Kein Konfliktdialog, kein „Remote behalten“. Remote-Änderungen an einem gerade bearbeiteten Wert zeigen nur den Hinweis `↻ changed by Pixelmapper` mit dem neuen Wert (orange; in Listen zwei Zeilen im Kopf, in Vollbild-Editoren unten). Die Eingabe bleibt, wer danach bestätigt, gewinnt.
4. **Display:** nach `Dim after` (DISPLAY_DIM_TIMEOUT) auf das Minimum gedimmt, nie durch den Timeout aus. Helligkeit = Standard-PID DISPLAY_LEVEL. **Art-Net LED Mute schaltet ganz aus**, bis zur nächsten Bedienung am Gerät oder LED Normal.
5. **Identify / LED Locate:** Die Statusleiste blinkt auffällig (ganze Leiste im Wechsel mit weißem `IDENTIFY`, 2 Hz). Kein Identify-Vollbild.
6. **Remote-Konfiguration nur über Art-Net-Opcodes und RDM-PIDs.** Kein ArtCommand, auch nicht als Notweg ([contract.md](../protocol/contract.md) §1). Panelsperre, Dimmen, Adressformat, DMX-Timing und Discovery-Intervall sind Hersteller-PIDs des Nodes, die Helligkeit ist DISPLAY_LEVEL.
7. **Adressformat:** Standard `Net:Sub:Uni`, umschaltbar auf 15-bit (DISPLAY_ADDRESS_FORMAT). Der Editor zeigt immer beide. Keine 1-basierte Variante.
8. **Panelsperre:** BACK + Drücken 3 s schaltet sie um, hebt auch eine Sperre von Pixelmapper auf, ohne PIN. Das Aufheben wird gemeldet (`PANEL_UNLOCKED_LOCAL`). Sperren schließt offene Editoren und Dialoge. Die E1.37-1-Lock-PIDs werden nicht genutzt.
9. **Rückfrage bei Port-Änderungen nur, wenn der Port gerade Daten führt** (Mode, Address). Weitere Rückfragen: Network Apply, Record scene, Reset port, Restart, Factory reset (+ 3 s Halten), RDM-Personality. Von Pixelmapper aus nie.
10. **RDM am Gerät:** Geräteliste pro Port und über alle Ports. Pro Gerät Identify, DMX-Startadresse, Personality, Label. Tube-Updates werden nur angezeigt, nie am Gerät gestartet. Identify eines Geräts geht aus, sobald seine Seite verlassen wird (auch mit BACK lang oder Timeout).
11. **Kein Input-Broadcast** (verboten durch die Art-Net-Spec), daher kein Menüpunkt.
12. **Ein Port pro BindIndex** (4 ArtPollReplies). Keine Auswirkung auf die UI.
13. **Failsafe pro Port** (ArtAddress pro BindIndex), Discovery-Intervall global im Hauptmenü, Hintergrund-Discovery pro Port unter Advanced.
14. **Hardware:** ESP32-P4 (Waveshare ESP32-P4-WIFI6-POE-ETH). Die UI läuft als Task mit niedrigster Priorität auf Kern 0.
15. **Fonts:** genau zwei, 8×16 fett (Terminus, OFL) und 6×8 (misc-fixed 5x8, Public Domain). Groß = 8×16 doppelt.
16. **Zeichnen:** Display-Liste + Streifenpuffer 160 × 16 px (5 KB), CRC pro Kachel 32 × 16 px, pro Streifen nur der schmutzige Bereich per DMA.
17. **TOD:** bis 512 Geräte pro Port (`PM_RDM_TOD_MAX`). Die UI hält Details (Adresse, Footprint, Personality, Label) nur für 32 Geräte in einem Cache und lädt die sichtbaren Zeilen per RDM nach.
18. **Node-Update ohne Bootloader-Anzeige:** Das OTA schreibt die inaktive Partition, während die Firmware läuft und das Vollbild 6.10 zeichnet. Danach folgt nur der Neustart; der Bootloader muss das Display nicht ansteuern.

### 8.2 Offene Fragen

1. **Einbaulage und Blickrichtung:** An welcher Rackhöhe steht das Gerät typischerweise? *Empfehlung:* Beide Querlagen am echten Modul testen, Gamma auf „von schräg oben“ optimieren. Das muss vor dem Frontplattenlayout passieren.
2. **Ereignisliste dauerhaft?** Sie liegt nur im RAM (16 Einträge) und ist nach einem Neustart leer. *Empfehlung:* so lassen, bis jemand sie nach einem Absturz vermisst; dann einen kleinen Ringpuffer im NVS.
3. **Systemfehler-„!“ quittieren:** Das „!“ in der System-Zone bleibt bis zum Neustart. *Empfehlung:* Beim Öffnen von `System › Events` quittieren.
4. **Overlap-Prüfung in der Geräteliste** vergleicht nur Geräte im Cache (32). Bei sehr vielen Geräten pro Port kann eine Überlappung außerhalb des Caches unerkannt bleiben. *Empfehlung:* für Phase 1 akzeptieren, Pixelmapper prüft den Patch vollständig.

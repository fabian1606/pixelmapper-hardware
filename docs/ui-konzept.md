# Bedienkonzept Front-Display – 4-Port Art-Net-4/RDM-Node

Stand: 2026-10-24 · nur Konzept, kein Code
Hardware: CH32V307VCT6, ST7735S 1,8" TN quer (160 × 128), Drehencoder mit Druck + BACK-Taster, keine LEDs.

---

## 1. Leitprinzipien

1. **Der Startbildschirm ist die Front, das Menü ist die Werkstatt.** Alles, was man im Betrieb wissen muss (Ports, Adressen, Datenfluss, IP), steht ohne einen einzigen Tastendruck auf dem Home-Screen. Menüs braucht man nur, um etwas zu ändern.
2. **Häufiges ist flach, Seltenes liegt tiefer.** Port-Adresse ändern: 3 Drücke. IP ablesen: 0. Namen eintippen oder Werksreset: tief und absichtlich etwas umständlich.
3. **Man weiß immer, wo man ist.** Jeder Screen außer Home hat eine Kopfzeile mit Pfad. Kein Pfad bedeutet: Home. Es gibt keine Screens ohne Kopfzeile außer Home und Vollbild-Ereignisse (Identify, Node-Update).
4. **Eine Regel pro Taste, überall gleich.** Drehen = bewegen/ändern, Drücken = „rein / fertig“, BACK kurz = eine Ebene zurück (bzw. Abbruch), BACK lang = Home. Es gibt keine Ausnahmen, nur Ergänzungen (Kombi-Griff für die Sperre, Halten für Werksreset).
5. **Die Statusleiste ist heilig.** Sie ist immer sichtbar, immer gleich aufgebaut und ersetzt die LEDs. Nur dort wird geblinkt. Der Inhaltsbereich blinkt nie, außer dem Identify-Vollbild.
6. **Ereignisse stehlen keinen Fokus.** Datenverlust, Remote-Änderungen und RDM-Updates werden angezeigt (Statusleiste, Toast, Wert-Hervorhebung), reißen den Bediener aber nicht aus einer Eingabe. Die einzige Ausnahme ist ein Firmware-Update des Nodes selbst, weil das Gerät dabei neu startet.
7. **Farbe ist nie die einzige Information.** Jeder Zustand hat zusätzlich ein Zeichen oder Kürzel. Das ist wegen TN-Blickwinkel und Farbfehlsichtigkeit nötig.
8. **Die Remote-Konfiguration ist gleichberechtigt.** Was die Lichtsoftware ändert, erscheint sofort am Gerät. Bei einer Kollision mit einer lokalen Eingabe entscheidet der Mensch am Gerät bewusst, mit „Remote behalten“ als sicherer Vorgabe.

Muster von Rack-Funkempfängern (Shure ULX-D/Axient, Sennheiser EM-Serie), aus Erfahrung, nicht neu recherchiert: Der Home-Screen ist eine Status-Seite. Ein bearbeiteter Wert ist deutlich markiert, bis er bestätigt ist. EXIT verwirft. Die Frontsperre löst man über einen gehaltenen Tasten-Griff. Die Menüs sind flach und zeigen Werte direkt in der Liste („Label …… Wert“). Dieses Konzept übernimmt genau diese Muster und verzichtet auf alles darüber hinaus.

---

## 2. Bildschirmaufbau

### 2.1 Physik und Lesbarkeit

- Die aktive Fläche ist etwa 35,0 × 28,0 mm groß, das sind rund **0,22 mm pro Pixel**.
- Eine 16-px-Zeile mit 8×16-Font hat etwa 11 px Versalhöhe, also rund 2,4 mm. Das ergibt aus 70 cm etwa 12 Bogenminuten: grenzwertig für Fließtext, aber ausreichend für kurze, bekannte Wörter. Aus der Bedienentfernung (Hand am Encoder, ca. 40–50 cm) sind es 17–20 Bogenminuten, das ist gut lesbar.
- **Daraus folgt:**
  - Aus 50–80 cm muss man lesen können: die Farbkacheln der Statusleiste und die Port-Zeilen auf Home (Adresse, fps).
  - Menütext liest man aus Bedienentfernung.
  - Fonts unter 8×16 nur für Nebensachen (Kopfzeile, Einheiten, Hinweiszeile).

### 2.2 Fonts (genau drei)

| Name | Zelle | Einsatz | Zeichen/Zeile |
|---|---|---|---|
| **F-M** (Medium) | 6 × 12, fett | Kopfzeile, Hinweiszeile, Toasts, Info-Texte | 26 |
| **F-N** (Normal) | 8 × 16, fett (Terminus-Bold-Stil) | Menüs, Home-Port-Zeilen, Werte | 20 |
| **F-L** (Groß) | 16 × 24 (F-N doppelt skaliert, mit geglätteten Kanten) | Identify, Update-Prozent, Sperr-Hinweis | 10 |

Fett, also mit 2-px-Vertikalstrichen, weil 1-px-Striche auf TN aus schrägem Winkel ausfransen. Die Statusleiste nutzt Ziffern aus F-N und 8×8-Icons.

### 2.3 Raster (alle Screens außer Home)

```
y   0–15   Statusleiste            16 px  (immer)
y  16      Trennlinie               1 px  (schwarz, Abstand)
y  17–30   Kopfzeile / Pfad        14 px  F-M, 26 Zeichen
y  31      Linie                    1 px  grau
y  32–127  Inhalt                  96 px  = 6 Zeilen à 16 px, F-N, 20 Zeichen
           ├ in Listen: alle 6 Zeilen Einträge
           └ in Editoren/Dialogen: Zeile 6 (y 112–127) = Hinweiszeile (F-M, grau)
x 157–159  Scrollbalken 3 px (nur wenn Liste > 6 Einträge)
```

- **Listenzeile:** Label links ab x = 2. Wert rechtsbündig, endet bei Spalte 19 (x ≈ 154). Ein Untermenü hat „›“ in Spalte 19. Das Label darf maximal 11 Zeichen haben, der Wert maximal 8. Diese Regel zwingt zu kurzen Begriffen, und das ist gewollt.
- **Home** hat ein eigenes Raster (siehe 4.1): keine Kopfzeile, 16 px Node-Zeile, 4 × 20 px Port-Zeilen, 16 px Menü-/Toast-Zeile.

### 2.4 Farbsystem (RGB565, TN-tauglich)

Grundsatz: schwarzer Hintergrund, nur sehr helle oder voll gesättigte Vordergrundfarben, keine Mittelgrau-Abstufungen als Bedeutungsträger. TN-Panels kippen Mitteltöne bei Blickwinkeländerung stark und invertieren sie teils. Schwarz/Weiß und Primärfarben bleiben stabil.

| Rolle | Farbe | Hinweis |
|---|---|---|
| Hintergrund | Schwarz #000000 | Kein dunkelgrauer Hintergrund (TN-Kippen) |
| Text primär | Weiß #FFFFFF | |
| Text sekundär / Info | Hellgrau #A0A0A0 | Das einzige Grau mit Bedeutung. Nie dunkler. |
| Deaktiviert („aus“) | Grau #606060 | Nur für eindeutig abgeschaltete Dinge |
| **Auswahl (Cursor)** | Weißer Balken, schwarzer Text (invertiert) | Robusteste Markierung auf TN |
| **Editieren** | Cyan-Balken #00DCFF, schwarzer Text | „Dieser Wert ist offen, noch nicht übernommen“ |
| **Remote-Änderung** | Orange #FF8C00 (Wert 3 s orange, mit ↻) | |
| **Warnung** (Dialog-Titel) | Gelb #FFE000 + ⚠ | |
| **Gefahr** (Werksreset-Button) | Rot #FF2020 | |
| Status grün „sendet“ | #20FF20 | Nur in Statusleiste und Port-Badges |
| Status blau „Eingang“ | #3090FF | Heller als reines Blau, sonst zu dunkel auf TN |
| Status gelb „Failsafe/Verlust“ | #FFE000 | |
| Status violett „RDM“ | #C040FF | |
| Status rot „Fehler“ | #FF2020 | |
| Status weiß „Identify“ | #FFFFFF | |

- **Text auf Kacheln:** Schwarz auf Grün, Gelb und Weiß. Weiß auf Blau, Violett und Rot.
- **Blickwinkel:** Das Panel ist eigentlich hochkant (128 × 160) gebaut. Quer eingebaut wandert die schlechte TN-Blickrichtung (meist „6 Uhr“) auf die linke oder rechte Seite. Das ist für ein Rack günstig, weil der Blickwinkel von oben/unten (Höhe im Rack) dann symmetrisch und gut ist.
- **Empfehlung:** Beide Querlagen (MADCTL 90° und 270°) am echten Modul testen und die nehmen, bei der man von schräg vorne oben (typische Rackhöhe Brust/Bauch) besser sieht. Die Gamma-Register (GMCTRP1/GMCTRN1) auf diese Einbaulage abstimmen.

### 2.5 RAM und Teil-Updates

- **Kein Vollbild-Framebuffer.** Gerendert wird in einen **Streifenpuffer 160 × 16 px = 5 KB**, optional zweimal (10 KB) für DMA-Pingpong. Ein Streifen entspricht genau einer Inhaltszeile oder der Statusleiste.
- **Dirty-Flags pro Zone:** Statusleiste (je Kachel einzeln: 24 × 16 px = 768 B), Node-Zeile, jede Inhaltszeile, Hinweiszeile. Nur geänderte Zonen werden per CASET/RASET-Fenster neu übertragen.
- Wo das konkret zählt:
  - **Blinken** (Failsafe, Identify): nur die betroffene Kachel, 1–2 Hz → vernachlässigbar.
  - **fps-Zähler** auf Home/Port-Detail: nur das 3-Zeichen-Feld (24 × 16 px), max. 2 Hz.
  - **Encoder im Edit-Modus:** nur das Wertfeld neu, nicht die ganze Zeile.
  - **Scrollen in Listen:** alle 6 Inhaltszeilen neu, das sind 30 KB über SPI. Bei realistisch 15 MHz (ST7735S-Grenze) sind das etwa 17 ms. Kein Smooth-Scrolling, zeilenweise springen.
  - **RDM-Violett-Blitz:** nur 3-px-Unterkante der Kachel.
- Keine Animationen außer Blinken und Fortschrittsbalken. Das ist ruhig, „clean“ und billig.

---

## 3. Statusleiste (y 0–15, immer sichtbar)

### 3.1 Layout

```
x: 0      24     48     72     96 100  116  132  148  159
   ┌──────┬──────┬──────┬──────┬───┬────┬────┬────┬────┐
   │ 1 ▸  │ 2 ▸  │ 3 ◂  │ 4 –  │   │ ▲  │ D  │ ▣  │ ↓  │
   └──────┴──────┴──────┴──────┴───┴────┴────┴────┴────┘
     Port-Kacheln 4 × 24 px            System-Zone 4 × 16 px
```

**Port-Kachel (24 × 16 px):**
- Ziffer (F-N) plus Richtungssymbol: ▸ = Ausgang, ◂ = Eingang, – = aus.
- Hintergrund = Zustandsfarbe.
- Die unteren 3 px sind die **Aktivitätskante**: Violett-Blitz bei RDM, Fortschrittsbalken bei einem Tube-Update.
- 1 px Abstand zwischen den Kacheln (schwarz).

**System-Zone (64 px, 4 Icon-Felder à 16 px):** Link, IP-Modus, Sperre, Update/Systemfehler. **Leere Felder bleiben schwarz.** Es wird nur angezeigt, was vom Normalzustand abweicht oder ihn bestätigt (Link, IP-Modus).

### 3.2 Port-Zustände (Priorität von oben nach unten, der höchste gewinnt)

| Zustand | Kachel | Zeichen | Blinken |
|---|---|---|---|
| Identify (Node) | weiß | Ziffer schwarz | 2 Hz zwischen weiß und normal |
| Fehler (Treiber, Kurzschluss, Konfig) | rot | Ziffer + „!“ statt Pfeil | nein |
| Datenverlust / Failsafe aktiv | gelb | Ziffer + Pfeil | 1 Hz (gelb ↔ schwarz mit gelbem Rand) |
| Ausgang sendet | grün | ▸ | nein |
| Eingang empfängt DMX | blau | ◂ | nein |
| aktiv, aber keine Daten (noch nie welche gehabt / Eingang ohne Signal) | schwarz, 1-px-Rahmen weiß | Ziffer + Pfeil weiß | nein |
| deaktiviert | schwarz, kein Rahmen | Ziffer + „–“ grau | nein |

**Aktivitätskante (unten, 3 px):**
- **RDM-Transaktion:** violett, mindestens 150 ms sichtbar (Pulsverlängerung). Folgen mehrere schnell hintereinander, bleibt sie an. Die Hauptfarbe bleibt sichtbar, weil RDM parallel zu DMX läuft und es nicht überdecken darf.
- **Tube-Update auf dem Port:** Fortschrittsbalken cyan über die Kachelbreite. Er ersetzt den Violett-Blitz, der sonst während des Updates dauerhaft an wäre.
- **RDM-Identify eines Geräts an diesem Port aktiv:** Kante weiß, 2 Hz.

„Datenverlust“ heißt: Der Port hatte Art-Net-Daten, und der Timeout ist abgelaufen. Ab dann gilt der Failsafe-Modus. Gelb bleibt, bis wieder Daten kommen.

### 3.3 System-Zone

| Feld | Zustand | Darstellung |
|---|---|---|
| Link | Link ok | ▲ grün (bzw. Ethernet-Icon) |
| | kein Link | ✕ rot |
| IP-Modus | DHCP erhalten | „D“ weiß |
| | Statisch | „S“ weiß |
| | Fallback 2.x.x.x | „F“ **gelb**, weil meist unbeabsichtigt |
| | DHCP läuft gerade (≤ 5 s) | „D“ grau, blinkt 1 Hz |
| Sperre | entsperrt | leer |
| | lokal gesperrt | ▣ weiß (Schloss-Icon) |
| | remote gesperrt | ▣ orange |
| Update/System | nichts | leer |
| | Node-Firmware wird empfangen | ↓ cyan mit Mini-Fortschritt (Unterkante) |
| | Tube-Update läuft (irgendein Port) | ↓ cyan (Details an der Port-Kante) |
| | Systemfehler/Warnung im Log, ungelesen | ! rot bzw. gelb |

Wird Art-Net **LED Mute** (AcLedMute) empfangen, wird die Statusleiste nicht abgeschaltet. Das Display dimmt auf Minimum und das Blinken stoppt. Details siehe offene Fragen.

---

## 4. Startbildschirm und Menübaum

### 4.1 Home (Startbildschirm)

Raster: Statusleiste 16 px · Node-Zeile 16 px · 4 Port-Zeilen à 20 px · Menü-Zeile 16 px.

- **Node-Zeile:** IP-Adresse und Modus (`192.168.100.217 DHCP`, max. 20 Zeichen).
  - Die IP ist im Rack wichtiger als der Name. Den Namen sieht man in der Menü-Kopfzeile und im Identify-Vollbild.
  - Ohne Link steht hier rot „Kein Link“.
- **Port-Zeile** (20 Zeichen, F-N), feste Spalten:
  `P▸ ADRESSE··· fps XXX`
  - Spalte 1–2: Port-Badge. Ziffer und Pfeil in der Zustandsfarbe, also dieselbe Farbe wie die Kachel oben.
  - Spalte 4–12: Port-Adresse im gewählten Format (Default `Net:Sub:Uni`, z. B. `0:0:1`).
  - Spalte 14–16: fps rechtsbündig, `--` ohne Daten.
  - Spalte 18–20: Zusatz-Kürzel, nach Priorität:
    - `ERR` rot
    - `HLD`/`ZER`/`FUL`/`SCN` gelb = Failsafe greift (zeigt, *was* gerade ausgegeben wird)
    - `↓41` cyan = Tube-Update 41 %
    - `M2` = Merge mit 2 Quellen
    - `R12` = 12 RDM-Geräte
    - leer
  - Deaktiviert: `3  aus` grau.
- **Menü-Zeile:** `Menü ›`. Sie dient auch als **Toast-Zeile**: Remote-Änderungen und Ereignisse erscheinen hier 3 s lang.

**Cursor auf Home:**
- Im Ruhezustand ist **kein** Cursor sichtbar (ruhiges Bild).
- Die erste Encoder-Raste blendet den Cursor auf dem zuletzt gewählten Eintrag ein (initial Port 1).
- Wählbar sind 6 Ziele: Node-Zeile → Netzwerk, Port 1–4 → Port-Detail, Menü → Hauptmenü.
- Nach 10 s ohne Eingabe verschwindet der Cursor wieder.

**Begründung:** Auf Home ist alles maximal 1 Druck entfernt, was man im Betrieb anfasst: Ports und Netzwerk. Es gibt bewusst nur *einen* Home-Screen und keine umschaltbaren Ansichten. Wer weitere Ansichten durchdreht, verliert die Orientierung.

### 4.2 Menübaum (vollständig)

```
HOME
├─ [Node-Zeile] ──────────────► Netzwerk (gleiche Seite wie Menü › Netzwerk)
├─ [Port 1…4] ────────────────► PORT n                          (Ebene 1)
│    (fix) Statuszeile: fps · Merge · Quellenzahl · RDM-Anzahl / Signal-Zustand
│    ├─ Adresse          0:0:1          Feld-Editor (Schnell-Edit auf Uni)
│    ├─ Modus            Ausgang        Aus / Ausgang / Eingang  (+ Bestätigung, wenn Daten fließen)
│    ├─ Geräte (12)      ›              RDM-Geräteliste  [nur Ausgang + RDM ein]
│    ├─ Merge            HTP            HTP / LTP                [nur Ausgang]
│    ├─ RDM              Ein            Ein / Aus                [nur Ausgang]
│    ├─ Broadcast        Aus            Ein / Aus                [nur Eingang]
│    ├─ Quellen          ›              Info: Quelle 1/2 IP, fps, letzte Daten vor x s
│    └─ Erweitert        ›                                        (Ebene 2)
│         ├─ Ausgabe         Immer       Immer / Bei Änderung (Art-Net Style Constant/Delta)
│         ├─ Refresh         44 Hz       1–44 Hz
│         ├─ Break           176 µs      92–1000 µs, Schritt 4
│         ├─ MAB             12 µs       12–500 µs, Schritt 4
│         ├─ Discovery       3 s         Aus / 1–60 s           [nur Ausgang + RDM]
│         └─ Port zurücksetzen …         Bestätigung; nur dieser Port auf Default
│
└─ [Menü] ────────────────────► HAUPTMENÜ  (Kopfzeile: „Menü · <Short Name>“)
     ├─ Netzwerk         ›
     │    ├─ Modus           DHCP        DHCP / Statisch   (Fallback ist kein Modus, sondern ein Zustand)
     │    ├─ IP              …           Feld-Editor; bei DHCP: nur Anzeige (grau, nicht wählbar)
     │    ├─ Maske           …           Enum /8…/30, als Punktnotation angezeigt
     │    ├─ Gateway         …           Feld-Editor
     │    ├─ Übernehmen …                erscheint nur, wenn etwas vorgemerkt ist → Bestätigung
     │    └─ Info            ›           Aktueller Zustand (DHCP/Statisch/Fallback), aktive IP/Maske/GW,
     │                                    MAC, Link (100M FD), DHCP-Server
     ├─ Geräte (36)      ›           [Vorschlag] alle RDM-Geräte aller Ports, gruppiert „P1 …“
     ├─ Failsafe         Hold        Wert direkt in der Zeile; Drücken → Untermenü:
     │    ├─ Modus           Hold        Hold / Zero / Full / Szene (Szene nur wählbar, wenn aufgenommen)
     │    └─ Szene aufnehmen …           Bestätigung („überschreibt“); zeigt danach „Szene: gespeichert“
     ├─ Node             ›
     │    ├─ Identify        Aus         Ein / Aus
     │    ├─ Short Name      ›           Texteditor (17)
     │    ├─ Long Name       ›           Texteditor (63)
     │    └─ RDM-UID         7FF0:80…    Info (vollständig in 2. Zeile, wenn gewählt)
     ├─ Anzeige & Sperre ›
     │    ├─ Helligkeit      80 %        10–100 %, Live-Vorschau beim Drehen
     │    ├─ Dimmen nach     2 min       Nie / 30 s / 1 / 2 / 5 / 10 min
     │    ├─ Dimm-Stufe      20 %        5–50 %
     │    ├─ Adressformat    N:S:U       N:S:U / 15-bit / 15-bit+1
     │    └─ Panel sperren   …           sperrt sofort (Alternative: Kombi-Griff)
     └─ System           ›
          ├─ Info            ›           Firmware, Bootloader, UID, MAC, Laufzeit, Art-Net-OEM/ESTA
          ├─ Ereignisse      ›           letzte 32 Ereignisse (Remote-Änderungen mit IP, Fehler, Link)
          ├─ Neustart …                  Bestätigung
          └─ Werksreset …                Bestätigung + 3 s Halten; Auswahl „inkl. Netzwerk ja/nein“
```

**Regeln im Baum:**
- **Irrelevantes wird ausgeblendet, nicht ausgegraut.** Ein Eingangs-Port zeigt kein Merge, kein RDM und keine Geräte, dafür Broadcast. Das hält Listen kurz.
  - Ausnahme Netzwerk bei DHCP: IP/Maske/GW bleiben sichtbar (grau), weil man sie *ablesen* will.
- **„Aktiv/deaktiviert“ und „Richtung“ werden zu einem Enum `Modus: Aus/Ausgang/Eingang` zusammengelegt.** Zwei Einstellungen, die man immer zusammen denkt, gehören in eine Zeile.
- **Werte stehen direkt in der Liste.** Man sieht den Zustand, ohne hineinzugehen. Das gilt auch für Failsafe im Hauptmenü.

### 4.3 Was ist wie schnell erreichbar (Begründung der Tiefe)

| Aufgabe | Häufigkeit | Weg (D = Drücken, R = Drehen) |
|---|---|---|
| IP, Port-Adressen, Datenfluss ablesen | ständig | Home, 0 Aktionen |
| Universe eines Ports ändern | häufig | R → Port, D, D (Schnell-Edit), R, D = **3 Drücke** |
| Port Ein/Aus/Richtung | gelegentlich | R, D, R (Modus), D, R, D (+ ggf. Bestätigung) |
| Tube adressieren | gelegentlich | Port → Geräte → Gerät → DMX-Adresse (Ebene 3) |
| Statische IP setzen | selten | Node-Zeile → Netzwerk → Modus/IP/Maske/GW → Übernehmen + Bestätigen |
| Failsafe-Modus | selten | Menü → Failsafe (Wert sofort sichtbar) |
| DMX-Timing, Discovery-Intervall | sehr selten | Port → Erweitert |
| Namen eintippen | sehr selten (meist per Software) | Menü → Node → Name (Texteditor, mühsam: das ist ok) |
| Werksreset | fast nie | Menü → System → Werksreset → Dialog → 3 s halten |

---

## 5. Navigations- und Editierregeln

### 5.1 Grundgesten

| Eingabe | In Listen | Im Edit-Modus | In Dialogen |
|---|---|---|---|
| **Drehen** rechts/links | Cursor ab/auf | Wert +/− | Button wechseln |
| **Drücken** (kurz, < 600 ms) | öffnen / Edit starten | **fertig**: übernehmen | ausgewählten Button ausführen |
| **BACK kurz** | eine Ebene zurück | eine Ebene zurück (Wert → Feldwahl → Abbruch) | = Abbrechen |
| **BACK lang** (≥ 1 s) | Home | Home, offene Eingabe wird **verworfen** (Toast „verworfen“) | Home, Abbruch |
| **BACK + Drücken, 3 s halten** | Panelsperre ein/aus (überall) | | |

- Rechtsdrehen bedeutet in Listen „nach unten“ und in Werten „mehr“.
- **Kein Umlauf (Wrap)** bei Listen, Zahlen und Enums. Am Ende ist Schluss. Damit springt man nicht versehentlich von 0 auf 32767 oder von „Hold“ auf „Scene“. Einzige Ausnahme ist der Zeichen-Ring im Texteditor.
- **Cursor-Gedächtnis:** Jede Liste merkt sich ihre Cursor-Position, bis der Bildschirmschoner-Timeout zurück auf Home führt.
- **Scrollen:** Der Cursor bewegt sich bis zur letzten sichtbaren Zeile, danach scrollt die Liste zeilenweise. Ab 7 Einträgen gibt es einen Scrollbalken rechts.
- **Info-Zeilen** (graue Werte ohne Funktion) überspringt der Cursor. Eine reine Info-Seite scrollt dagegen beim Drehen.

### 5.2 Editier-Modell: „Drücken = fertig mit dem, was ich gerade tue“

**Einfache Werte (Enum, Zahl, Ein/Aus):**
- Drücken auf eine Zeile → der Wert wird **cyan hinterlegt** (Edit-Modus), die Zeile darunter wird zur Hinweiszeile.
- Drehen ändert den Wert **nur in der Anzeige**.
- Drücken übernimmt (kurzer weißer Blitz als Quittung). BACK verwirft und zeigt wieder den alten Wert.
- Ein/Aus-Werte werden genauso behandelt, also nicht schon mit einem einzelnen Druck umgeschaltet. Konsistenz ist wichtiger als ein gesparter Druck.
- **Ausnahme Helligkeit:** Live-Vorschau beim Drehen, BACK stellt den alten Wert wieder her.

**Wann eine Änderung wirksam wird:**
- **Sofort bei „fertig“:** alle Port-Einstellungen, Failsafe-Modus, RDM-Gerätewerte, Anzeige.
- **Vorgemerkt (staged):** **nur das Netzwerk**. IP, Maske, Gateway und Modus sammeln sich. Die Kopfzeile zeigt „●“ in Cyan, vorgemerkte Werte stehen cyan statt weiß. Wirksam wird erst „Übernehmen …“ mit Bestätigung.
  - Verlässt man die Netzwerk-Seite mit vorgemerkten Werten, erscheint der Dialog „Netzwerk-Änderungen: [Verwerfen] [Übernehmen]“, mit Fokus auf Verwerfen.

**Zahlen mit Beschleunigung:**
- Unter 50 Werten Bereich: keine Beschleunigung.
- Darüber wird der Schritt aus dem Abstand der letzten 3 Rasten bestimmt:
  - mehr als 80 ms: ×1
  - 30–80 ms: ×10
  - unter 30 ms: ×100
  - der Schritt ist auf höchstens 1/20 des Bereichs begrenzt.
- Bei Wechsel der Drehrichtung fällt der Schritt sofort auf ×1 zurück. Das verhindert Überschießen.
- Werte mit festem Raster (Break/MAB in 4-µs-Schritten, Discovery in s) beschleunigen in Vielfachen ihres Rasters.

**Feld-Editor (Port-Adresse, IP, Gateway):**
- Der **Schnell-Edit** ist der Einstieg: Drücken öffnet den Editor direkt im Edit-Modus des *wahrscheinlichsten* Felds. Bei der Port-Adresse ist das **Uni**, bei IP und Gateway das **letzte Oktett**.
  - Drehen ändert dieses Feld, **Drücken übernimmt den ganzen Wert und schließt** den Editor.
  - Das ist der 3-Drücke-Weg für die häufigste Änderung.
- **BACK kurz** im Schnell-Edit führt in die **Feldwahl**. Ein weißer Rahmen springt per Drehen über `Net · Sub · Uni · [OK]` (bzw. die 4 Oktette und `[OK]`).
  - Drücken auf ein Feld → Feld-Edit (cyan). Drücken beendet das Feld und führt zurück zur Feldwahl.
  - `[OK]` übernimmt alles. BACK in der Feldwahl bricht ab.
  - **BACK zweimal bricht also immer ab**, das ist konsistent mit „eine Ebene zurück“.
- **Port-Adresse:**
  - Unter den drei Feldern steht live der 15-bit-Wert (`= 4097`), bzw. N:S:U, wenn das Format auf 15-bit steht.
  - **Uni hat Übertrag:** Drehen über 15 hinaus zählt Sub weiter (über 0:0:15 kommt 0:1:0). Damit ist das Uni-Feld zugleich ein 15-bit-Zähler, und man kommt fast immer ohne Feldwahl aus.
  - Steht das Adressformat auf „15-bit“, gibt es nur ein einziges Zahlenfeld 0–32767 mit Beschleunigung.
- **IP:**
  - Oktette 0–255 mit Beschleunigung, kein Übertrag.
  - Ungültige Kombinationen (Netz- oder Broadcast-Adresse bezogen auf die Maske, 0.0.0.0) werden beim Übernehmen **abgelehnt**. In der Hinweiszeile steht dann gelb „Ungültig für /24“, und der Editor bleibt offen.
- **Maske:** keine Oktett-Eingabe, sondern ein Enum der gültigen Präfixe /8 bis /30, angezeigt als `255.255.255.0`. Ungültige Masken sind so unmöglich.

**Texteditor (Short Name, Long Name, RDM-Label):**
- Zeile 1: der Text mit Cursor, ein Unterstrich-Block in Cyan. Long Name wird horizontal mitgescrollt, mit „…“ links.
- Zeile 2: Zähler `12/17` (grau, rechts).
- Zeile 4: der **Zeichen-Ring** mit Auswahl in der Mitte:
  `✓Fertig · ⌫ · ␣ · A–Z · a–z · 0–9 · - _ . / # + ( ) :`
  - Drehen wählt das Zeichen.
  - **Drücken setzt das Zeichen an der Cursorposition und rückt vor.** Der Ring bleibt auf dem gewählten Zeichen stehen, so ist das nächste Zeichen meist nah.
  - `⌫` löscht das Zeichen links vom Cursor. **Drücken und 1 s halten auf ⌫** löscht den ganzen Text. Das wird beim Wählen von ⌫ in der Hinweiszeile angezeigt.
  - `✓` übernimmt. BACK kurz bricht ab. Bei geändertem Text kommt erst „Änderung verwerfen? [Nein] [Ja]“.
  - Beim Öffnen steht der Cursor **am Textende** und der Ring auf `✓`. Mit „R, D“ ist man also sofort wieder draußen, mit `⌫` korrigiert man das Ende.
- Mitten im Text editieren gibt es nicht, das ist bewusst so. Wer viel ändern will, löscht oder nutzt die Lichtsoftware.

### 5.3 Bestätigung gefährlicher Aktionen

**Einheitlicher Dialog:**
- Die Kopfzeile wird zum gelben Titel mit ⚠.
- 3 Zeilen Klartext sagen, *was passiert*.
- Zwei Buttons. **Der Fokus steht immer auf dem sicheren Button links** („Abbrechen“).

Bestätigungspflichtig (und nur diese, sonst wird Bestätigen zur Gewohnheit):

| Aktion | Zusatz |
|---|---|
| Netzwerk übernehmen | Hinweis „trennt laufende Art-Net-Verbindungen“, zeigt neue IP |
| Port-Modus ändern, **während der Port Daten sendet/empfängt** | „Port 2 sendet gerade (44 fps). Ausgabe stoppt.“ Ohne Datenfluss keine Bestätigung. |
| Failsafe-Szene aufnehmen | „überschreibt gespeicherte Szene“ |
| RDM-Gerät: Personality ändern | „Footprint ändert sich 3 → 12 Kanäle“ |
| Neustart | |
| Port zurücksetzen | |
| Werksreset | **Zusätzlich 3 s Halten** des Encoders auf „Reset“ (Balken füllt sich im Button). Vorher Wahl „Netzwerk behalten / alles“, Default *Netzwerk behalten*. |

**Undo:** Es gibt kein Undo nach dem Übernehmen. Das Verwerfen (BACK) *vor* dem Übernehmen ist das Undo. Die Ereignisliste zeigt alte → neue Werte, damit man von Hand zurückstellen kann. Ein Undo-Stack wäre auf 20 Zeichen eher Verwirrung als Hilfe.

### 5.4 Timeouts, Bildschirmschoner, Backlight

- **60 s ohne Eingabe in einem Menü** → zurück auf Home.
  - Eine **offene Eingabe** (cyan) wird nicht nach 60 s, sondern nach **120 s verworfen**. Danach erscheint auf Home der Toast „Eingabe verworfen“.
  - Vorgemerkte Netzwerkwerte werden ebenfalls verworfen und im Ereignislog vermerkt.
- **Dimmen nach 2 min** (Default) auf die Dimm-Stufe (20 %). **Niemals ganz aus als Default.** Die Statusleiste ersetzt die LEDs, ein schwarzes Display wäre ein Gerät ohne Statusanzeige. „Aus“ ist als Option gar nicht erst vorgesehen (siehe Frage 4).
- **Die erste Eingabe am gedimmten Display weckt nur auf und löst nichts aus.** Das verhindert Fehlbedienung im Dunkeln.
- **Automatisches Aufwecken** (volle Helligkeit für 30 s, danach wieder dimmen):
  - Fehler (rot)
  - Datenverlust/Failsafe
  - Link weg
  - Identify
  - Node-Update
  - **Kein** Aufwecken bei Remote-Konfig-Änderungen, die würden im Showbetrieb dauernd flackern.
- LCD-Einbrennen ist bei TFT kein reales Problem. Ein Pixel-Shift ist nicht nötig.

### 5.5 Panelsperre

- **Sperren:** Kombi-Griff (BACK + Encoder drücken, 3 s halten) oder Menü → Anzeige & Sperre → Panel sperren oder remote.
  - Beim Halten zeigt die Hinweis-/Menüzeile einen Fortschrittsbalken „Sperren…“. Loslassen vorher = nichts passiert.
- **Gesperrt heißt Nur-Ansehen:**
  - Navigieren, Listen ansehen und Ports öffnen bleibt möglich. Man soll im Fehlerfall sehen können, was los ist.
  - **Jeder Versuch zu editieren** (Drücken auf einen Wert) zeigt das Sperr-Overlay für 3 s.
  - Auch Identify und RDM-Identify sind gesperrt.
- **Entsperren:** derselbe Kombi-Griff, 3 s, mit Fortschrittsbalken im Overlay.
  - Das gilt **auch bei Remote-Sperre**. Physischer Zugang hat Vorrang (z. B. Netzwerk tot und die Sperre muss weg). Das Entsperren wird an die Software gemeldet (ArtPollReply-Status/RDM) und geloggt.
  - Kein PIN, siehe Frage 2.
- Sperre aktiv: ▣ in der Statusleiste, weiß = lokal, orange = remote.

---

## 6. ASCII-Mockups

**Notation:**
- Jede Zeile zwischen `|…|` hat genau **20 Zeichen** (F-N-Raster). Kopf- und Hinweiszeilen sind in Wahrheit F-M mit 26 Zeichen. Sie sind hier auf 20 gekürzt dargestellt.
- `>` vor dem Rahmen = Zeile invertiert (Cursor).
- `[x]` = Feld im Edit-Modus (cyan).
- Farbangaben stehen rechts.
- Statusleisten-Zeichen:
  - `>` `<` `-` = Pfeil ▸ ◂ –
  - `▲` = Link
  - `D/S/F` = IP-Modus
  - `▣` = Sperre
  - `↓` = Update

### 6.1 Home

```
   |1> 2> 3< 4- ▲ D     |  Kacheln: 1 grün, 2 grün, 3 blau, 4 schwarz/grau
   |192.168.100.217 DHCP|  weiß
   |1> 0:0:0      44 R12|  20 px Zeilen; Badge grün
   |2> 0:0:1      44 M2 |  Badge grün
   |3< 0:1:0      30    |  Badge blau
   |4  aus              |  grau
   |Menü              › |  (Toast-Zeile)
```

Mit Cursor (nach erster Raste) und Datenverlust auf Port 2:
```
   |1> 2> 3< 4- ▲ D     |  Kachel 2 blinkt gelb
   |192.168.100.217 DHCP|
   |1> 0:0:0      44 R12|
  >|2> 0:0:1      -- HLD|  invertiert; „HLD“ gelb
   |3< 0:1:0      30    |
   |4  aus              |
   |Menü              › |
```

### 6.2 Port-Übersicht

Die Port-Übersicht **ist** der Home-Screen (6.1). Es gibt bewusst keine zusätzliche Übersicht, die nur Home wiederholen würde. Wer alle Details aller Ports braucht, öffnet die Ports einzeln.

### 6.3 Port-Detail

```
   |1> 2> 3< 4- ▲ D     |
   |Port 2 · Ausgang    |  Kopfzeile F-M; Badge-Farbe am „2“
   |44fps HTP 2Q  12RDM |  fixe Statuszeile, grau (bei Verlust: gelb „Signal weg 12s·Hold“)
  >|Adresse       0:0:1 |
   |Modus       Ausgang |
   |Geräte (12)       › |
   |Merge           HTP |
   |RDM             Ein▐|  Scrollbalken: weiter unten Quellen ›, Erweitert ›
```

### 6.4 Port-Adresse editieren (Schnell-Edit direkt nach Drücken)

```
   |1> 2> 3< 4- ▲ D     |
   |Port 2 › Adresse    |
   |                    |
   |  Net  Sub  Uni     |  grau
   |    0 :  0 : [ 1]   |  Uni cyan hinterlegt
   |  = 1 (15-bit)      |  grau, live mitlaufend
   |                    |
   |Drück:OK BACK:Felder|  Hinweiszeile grau
```
Nach BACK (Feldwahl): Der Rahmen springt per Drehen über `Net`, `Sub`, `Uni`, `[OK]`. Die Hinweiszeile lautet `Dreh:Feld Drück:ändern  BACK:Abbruch`.

### 6.5 IP editieren (Netzwerk, Modus Statisch)

```
   |1> 2> 3< 4- ▲ S     |
   |Netzwerk › IP       |
   |                    |
   |  192.168.100.[217] |  letztes Oktett cyan
   |                    |
   |  aktiv: 2.0.0.17   |  grau: was gerade wirklich gilt
   |  ● nicht übernommen|  cyan, erscheint, sobald etwas vorgemerkt ist
   |Drück:OK BACK:Felder|
```
Netzwerk-Liste danach:
```
   |Menü › Netzwerk  ●  |  ● cyan = vorgemerkt
   |Modus      Statisch |  cyan (vorgemerkt)
   |IP        10.0.0.50 |  cyan
   |Maske 255.255.255.0 |
   |Gateway    10.0.0.1 |
  >|Übernehmen …        |
   |Info              › |
```

### 6.6 Namen editieren

```
   |1> 2> 3< 4- ▲ D     |
   |Node › Short Name   |
   |XBAR-NODE-0_        |  Cursor-Block cyan
   |               12/17|  grau
   |                    |
   |  W  X  Y [Z] 0  1  |  Ring, Auswahl cyan
   |                    |
   |Drück=Zeichen setzen|
```
Ring auf `⌫`: Die Hinweiszeile lautet `Drück:löschen  Halten:alles`. Ring auf `✓`: `Drück:übernehmen`.

### 6.7 RDM-Geräteliste (Vorschlag)

```
   |1> 2> 3< 4- ▲ D     |  Kachel 2: violette Unterkante bei Discovery-Verkehr
   |Port 2 › Geräte (12)|
  >|↻ Neu suchen        |  Full Discovery (Hintergrund-Discovery läuft ohnehin)
   |001 T-01   XBAR500  |  DMX-Adresse · Label · Modell, sortiert nach Adresse
   |013 T-02   XBAR500  |
   |025 T-03   XBAR500  |
   |031!T-04   XBAR500  |  „!“ + rote Adresse = Footprint überlappt mit T-03
   |037 T-05   XBAR500 ▐|
```
Gerät geöffnet:
```
   |1> 2> 3< 4- ▲ D     |
   |…Geräte (12) › T-03 |
  >|Identify        Aus |  Ein/Aus; wird beim Verlassen der Seite automatisch Aus
   |DMX-Adresse     025 |  1–512, Feld mit Beschleunigung, sofort per RDM SET
   |Personality   2 RGB |  Enum aus RDM PERSONALITY_DESCRIPTION, Bestätigung
   |  Footprint: 3 Kan. |  grau, Info
   |Label          T-03 |  Texteditor (32 Zeichen, RDM DEVICE_LABEL)
   |Info              › |  UID, Modell, Hersteller, SW-Version, Fehler-/Status
```
- Kommt ein `SET` ohne ACK zurück (NACK/Timeout), bleibt der alte Wert stehen. Die Hinweiszeile zeigt rot „Gerät antwortet nicht“.
- Während des SET zeigt das Feld „…“.

### 6.8 Bestätigungsdialog

```
   |1> 2> 3< 4- ▲ S     |
   |⚠ Netzwerk ändern   |  gelb
   |IP-Änderung trennt  |
   |laufende Art-Net-   |
   |Verbindungen.       |
   |Neu: 10.0.0.50 /24  |  cyan
  >|[Abbrechen]  Ändern |  Fokus links (invertiert); „Ändern“ weiß
   |Dreh:wählen Drück:OK|
```
Werksreset (nach Wahl „alles“):
```
   |⚠ Werksreset        |  rot
   |Alle Einstellungen  |
   |inkl. Netzwerk auf  |
   |Werkszustand.       |
   |                    |
   | Abbrechen  [Reset] |  „Reset“ rot; Halten füllt Button von links weiß
   |Reset: 3 s halten   |
```

### 6.9 Remote-Änderung und Konflikt

Remote-Änderung, während man auf Home ist:
```
   |1> 2> 3< 4- ▲ D     |
   |192.168.100.217 DHCP|
   |1> 0:0:0      44 R12|
   |2> 0:1:4      44 M2 |  „0:1:4“ 3 s orange
   |3< 0:1:0      30    |
   |4  aus              |
   |↻ P2 Adr → 0:1:4    |  Toast orange, 3 s, danach wieder „Menü ›“
```
Konflikt: Man editiert die Adresse von Port 2, und remote kommt eine neue:
```
   |1> 2> 3< 4- ▲ D     |
   |Port 2 › Adresse  ↻ |  ↻ orange in der Kopfzeile
   |  Net  Sub  Uni     |
   |    0 :  0 : [ 5]   |  eigene Eingabe bleibt unangetastet (cyan)
   |                    |
   |↻ Remote: 0:1:4     |  orange
   |  von 10.0.0.5      |  grau
   |Drück:OK BACK:Felder|
```
Beim Drücken (Übernehmen) kommt der Pflicht-Dialog:
```
   |⚠ Konflikt          |
   |Remote hat 0:1:4    |
   |gesetzt (10.0.0.5). |
   |Dein Wert: 0:0:5    |
   |                    |
  >|[Remote]  Meinen    |  Fokus auf Remote (= nichts überschreiben)
   |Dreh:wählen Drück:OK|
```

### 6.10 Update läuft

Node-Firmware (Vollbild, Eingaben gesperrt, BACK zeigt nur „Bitte warten“):
```
   |1> 2> 3< 4- ▲ D   ↓ |
   |Firmware-Update     |
   |v1.3.0 → v1.4.0     |
   |                    |
   |██████████░░░░  71% |  Balken cyan; Prozent in F-L wäre schöner, passt aber
   |Schreibe Flash …    |  Phasen: Empfange · Prüfe · Schreibe · Neustart
   |NICHT AUSSCHALTEN   |  gelb (nur in Phase „Schreibe“)
   |Quelle 10.0.0.5     |
```
Tube-Update über RDM (nicht modal): Port-Kachel-Unterkante als Fortschritt, Home-Zusatzspalte `↓41`. Im Port-Detail lautet die Statuszeile:
```
   |Port 2 · Ausgang    |
   |↓ Tubes 3/12   41%  |  cyan
   |Adresse       0:0:1 |
```
Im Geräte-Listeneintrag hat das gerade aktualisierte Gerät „↓“ statt der Adresse, fertige Geräte haben kurz „✓“.

### 6.11 Panel gesperrt (Overlay nach Edit-Versuch)

```
   |1> 2> 3< 4- ▲ D ▣   |  ▣ orange = remote gesperrt
   |Port 2              |
   |                    |
   |   Panel gesperrt   |
   |  (von 10.0.0.5)    |  nur bei Remote-Sperre
   |                    |
   |Entsperren: BACK +  |
   |Drücken 3 s halten  |  wird beim Halten zum Fortschrittsbalken
```

### 6.12 Identify (Node, Vollbild auf Home)

```
   |1  2  3  4  ▲ D     |  alle Kacheln blinken weiß 2 Hz
   |                    |
   |     IDENTIFY       |  F-L, invertiert blinkend
   |   XBAR-NODE-01     |  Short Name
   |  192.168.100.217   |
   |   von 10.0.0.5     |  grau (bzw. „lokal“)
   |Drück: beenden      |
```

---

## 7. Verhalten bei Ereignissen

| Ereignis | Statusleiste | Inhalt | Fokus | Display | Log |
|---|---|---|---|---|---|
| **Remote-Konfig-Änderung** (ArtAddress, ArtCommand, RDM SET auf Node) | – | Wert, falls sichtbar, sofort neu, 3 s orange mit ↻. Toast in Menü-/Hinweiszeile „↻ P2 Adr → 0:1:4“. Mehrere Änderungen in < 1 s werden zu „↻ 5 Änderungen (10.0.0.5)“ zusammengefasst. | bleibt | kein Wecken | ja, mit Quell-IP und alt → neu |
| **Remote-Änderung am gerade editierten Wert** | – | siehe 6.9: Eingabe bleibt, Hinweis, Pflicht-Dialog beim Übernehmen, Default „Remote“ | bleibt | – | ja |
| **Remote-Änderung macht die Seite ungültig** (z. B. Port wird auf Eingang gestellt, während man Merge editiert; RDM-Gerät verschwindet) | – | Editor schließt, Sprung eine Ebene hoch, Toast „Port 2 remote geändert“ | erzwungen eine Ebene hoch | – | ja |
| **Remote-Netzwerkänderung** | Modus-Buchstabe ändert sich | Toast; falls vorgemerkte lokale Netzwerkwerte existieren, werden sie verworfen und Toast „Lokale Netzwerk-Änderung verworfen“ | bleibt | – | ja |
| **Datenverlust / Failsafe** | Kachel gelb blinkend | Home: fps `--` und Failsafe-Kürzel (HLD/ZER/FUL/SCN). Port-Detail: Statuszeile gelb „Signal weg 12 s · Hold“ | bleibt, kein Popup | Wecken | ja (Beginn und Ende) |
| **Daten wieder da** | zurück auf grün/blau | normal | – | – | ja |
| **Merge-Konflikt** (3. Quelle) | – | Port-Detail Statuszeile „3. Quelle ignoriert“ gelb, Quellen-Seite zeigt sie | – | – | ja |
| **Identify Node** (remote AcLedLocate/RDM oder lokal) | alle Kacheln weiß blinkend | auf Home: Vollbild 6.12. In einem Menü/Editor: **nur** Statusleiste blinkt plus Toast „Identify aktiv“ | Home: ja / sonst nein | 100 % | ja |
| **RDM-Identify eines Tubes** | weiße Unterkante der Port-Kachel | – | – | – | nein |
| **Port-Fehler** (Treiber, Dauer-Kollision, DMX-Eingang Framing) | Kachel rot, „!“ | Home `ERR`. Port-Detail Statuszeile mit Klartext („RDM: Leitung kurzgeschlossen?“) | – | Wecken | ja |
| **Systemfehler** (Konfig-CRC, Flash) | „!“ rot | einmaliger Dialog mit nur [OK] beim Start, z. B. „Konfiguration defekt – Standardwerte geladen“ | einmalig | Wecken | ja |
| **Link weg** | Link-Feld ✕ rot | Node-Zeile „Kein Link“ rot. Ausgangs-Ports laufen nach Timeout in Failsafe → gelb | – | Wecken | ja |
| **Link wieder da** | ▲ grün, „D“ blinkt grau während DHCP | Node-Zeile zeigt IP, sobald vorhanden | – | – | ja |
| **DHCP fehlgeschlagen → Fallback** | „F“ gelb | Toast „Kein DHCP → 2.x.x.x“ | – | – | ja |
| **Node-Update startet** | ↓ cyan | Empfangsphase: nur Statusleiste (Betrieb läuft weiter). Ab „Prüfe/Schreibe“: Vollbild 6.10, Eingaben aus | ab Schreiben ja | 100 % | ja |
| **Tube-Update** | ↓ cyan, Fortschritt an Port-Kante | Home `↓41`, Port-Detail Statuszeile | – | – | ja (Start/Ende/Fehler pro Gerät) |
| **Panelsperre remote gesetzt** | ▣ orange | Ein offener Editor wird verworfen, Toast „Remote gesperrt“ | eine Ebene hoch | – | ja |

---

## 8. Offene Fragen (priorisiert, jeweils mit Empfehlung)

1. **Adressformat-Default: `Net:Sub:Uni`, 15-bit (0-basiert) oder „Universe 1-basiert“?** Lichtpulte zählen teils 1-basiert, das Art-Net-Protokoll 0-basiert.
   *Empfehlung:* Default `N:S:U` (eindeutig, protokolltreu). 15-bit-Wert immer im Editor mitzeigen. Format umstellbar unter Anzeige. Die 1-basierte Variante nur, wenn deine Software (welche?) das so zeigt.
2. **Darf man eine Remote-Sperre am Gerät aufheben, und braucht es einen PIN?**
   *Empfehlung:* Ja, per 3-s-Kombi, ohne PIN. Das Aufheben wird remote gemeldet und geloggt. Ein PIN per Encoder ist umständlich und schützt im Rack vor niemandem, der ohnehin den Stecker ziehen kann.
3. **Konflikt-Default: „Remote gewinnt“ korrekt?**
   *Empfehlung:* Ja. Das Pult ist im Showbetrieb die Wahrheit. Lokal überschreiben geht nur bewusst über „Meinen“.
4. **Darf das Display ganz aus gehen (z. B. Theater-Blackout, Art-Net LED Mute)?**
   *Empfehlung:* Nein, Minimum 5 % Helligkeit. AcLedMute = minimal dimmen und Blinken stoppen. AcLedNormal = zurück. Ein komplett schwarzes Gerät ohne LEDs ist im Fehlerfall wertlos.
5. **Umfang der RDM-Gerätebedienung am Node (Vorschlag 6.7): reichen Identify, DMX-Adresse, Personality, Label?**
   *Empfehlung:* Ja, genau diese vier. Tube-Firmware-Updates **nicht** am Gerät starten, nur anzeigen (zu riskant per Encoder). Eine globale Liste „Geräte (alle Ports)“ im Hauptmenü zusätzlich zur Port-Liste lohnt sich bei vielen Tubes.
6. **RAM-Budget für die TOD inkl. Labels:** Wie viele Geräte pro Port sollen unterstützt werden?
   *Empfehlung:* 64 pro Port. UID, Adresse, Footprint und Personality werden fest gehalten (etwa 12 B pro Gerät = 3 KB). Labels (32 B) werden nur für die sichtbaren Zeilen nachgeladen und in einem kleinen LRU-Cache (32 Einträge, etwa 1 KB) gehalten. Modellnamen werden einmal pro Modell-ID gecacht.
7. **Bootloader-Anzeige während des Node-Updates:** Muss der Bootloader selbst das Display ansteuern?
   *Empfehlung:* Ja, minimal: Init plus ein fester Text „Update – nicht ausschalten“ und ein Balken, kein Font-System. Sonst zeigt das Gerät beim Flashen ein eingefrorenes oder weißes Bild.
8. **Remote-Mechanismus für Panelsperre, Display-Helligkeit, Discovery-Intervall, DMX-Timing:** ArtAddress/ArtCommand decken das nicht ab.
   *Empfehlung:* Herstellerspezifische RDM-PIDs am Node (RDM-Responder auf Art-Net-Seite, sauber und von RDM-Tools nutzbar). Zusätzlich ArtCommand-Textbefehle (`PanelLock=1&`) als Notweg.
9. **Einbaulage und Blickrichtung:** An welcher Rackhöhe steht das Gerät typischerweise?
   *Empfehlung:* Beide Querlagen am echten Modul testen, Gamma auf „von schräg oben“ optimieren. Das muss vor dem Frontplattenlayout passieren.
10. **UI-Sprache:**
    *Empfehlung:* Deutsch mit englischen Fachbegriffen, wie in den Mockups (Merge, Failsafe, Break, MAB, Universe). Rein englische Texte wären etwas kürzer, aber das 20-Zeichen-Raster passt mit den gewählten Begriffen bereits.
11. **Bestätigung bei Port-Modus-Änderung nur bei aktivem Datenfluss – oder immer?**
    *Empfehlung:* Nur bei Datenfluss. Ohne Signal kann nichts kaputtgehen, und unnötige Dialoge stumpfen ab.
12. **Name auf Home statt IP?**
    *Empfehlung:* IP. Den Namen sieht man in der Menü-Kopfzeile und im Identify-Vollbild. Bei mehreren gleichen Nodes in einem Rack wäre ein Name aber hilfreich. Dann den Short Name in die Menü-Zeile von Home setzen („XBAR-NODE-01 ›“ statt „Menü ›“). Das ist günstig und clean.

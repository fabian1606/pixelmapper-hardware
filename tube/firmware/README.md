# tube_fw – DMX512/RDM-Firmware für den ATmega8 in der LED-Tube

> Übernommen aus `ledTubeXBAR500/firmware/tube_fw` (Archiv). Pfade gelten ab Repo-Wurzel von `pixelmapper-hardware`:
> App `tube/firmware`, Bootloader `tube/boot_rdm`, Werkzeuge `tube/tools` (Abhängigkeiten: `tube/tools/requirements.txt`).


Eigene Firmware (avr-libc, ohne Arduino) für die 24-V-LED-Tube mit 4 RGB-Segmenten.
Alle Hardware-Details stehen in `include/config.h` – offene Punkte sind mit `TODO(pinout)` markiert.

Funktionen: DMX512-Empfang (nur benötigte Slots), 12 × 8 bit BCM-PWM (490 Hz, Gamma 2.2),
RDM-Responder (E1.20), 4 Personalities, Einstellungen im EEPROM (Magic + Version + CRC8),
Watchdog, Selbsttest beim Start (Segment 1 R, G, B, Segment 2 R, G, B, …).

| Personality | Kanäle | Belegung |
|---|---|---|
| 1 | 12 | 4 × RGB |
| 2 | 3 | RGB ganze Tube |
| 3 | 4 | RGB + Master-Dimmer |
| 4 | 13 | 4 × RGB + Master-Dimmer |

## Bauen & Flashen

```sh
cd tube/firmware
~/.platformio/penv/bin/pio run                 # bauen (Limit 6128 B: 2 KB Bootloader + 16 B Trailer reserviert)
                                               # -> .pio/build/isp/tube_fw-<version>.hex = App + kompletter Trailer
~/.platformio/penv/bin/pio run -e isp_1k       # gleiche App, Limit 7164 B für Tubes mit altem 1-KB-tube_boot
~/.platformio/penv/bin/pio run -e isp -t upload  # flashen über ArduinoISP (Uno an /dev/cu.usbmodem101)
```

Der Upload macht einen Chip-Erase (`-e`) – nötig, weil der Original-Chip gesperrt ist (Lock 0xE8).
**Achtung:** Chip-Erase löscht auch das EEPROM (EESAVE ist nicht gesetzt) → Geräte-ID danach neu schreiben.
**Achtung:** Ist der Bootloader installiert, löscht dieser Upload ihn mit (und schreibt keine Länge/CRC in den Trailer)
→ dann nur noch das kombinierte Image aus „Update über RDM“ per ISP flashen.

## Pinout-Checkliste (vor dem ersten Flash ausfüllen, `include/config.h`)

- [ ] Quarzfrequenz: ____ MHz → `board_build.f_cpu` in `platformio.ini` (8/16 MHz ok, sonst Build-Fehler wenn 250 kbaud > 2 % daneben)
- [ ] RS-485 DE/RE-Pin: Port __ Bit __, aktiv high/low (Default PD2) – oder `RS485_DE_PIN_NONE` (nur Empfang, kein RDM)
- [ ] RE separat verdrahtet? (dann zweiten Pin ergänzen)
- [ ] Anzahl Segmente (`LED_SEGMENTS`, Default 4)
- [ ] 12 LED-Kanäle (Seg 1–4 × R, G, B): jeweils Port/Bit + aktiv high/low (`LED_CHANNELS_X`, daraus
      `LED_CHANNEL_MAP` der App und die Identify-Pinmasken `LED_MASK_B/C/D` des Bootloaders)
- [ ] Welches Ende ist Segment 1?
- [ ] Freier ADC-Kanal für Zufalls-ID (`ADC_NOISE_CHANNEL`, Default ADC7)

Der Selbsttest nach dem Reset (je 150 ms pro Farbe, Segment für Segment) zeigt sofort, ob die Zuordnung stimmt.

## Test mit dem ESP32-Tool

```sh
python3 tube/tools/tool.py "rdm disc"                       # UID finden, z. B. 7FF0:12345678
python3 tube/tools/tool.py "rdm info 7FF0:12345678"         # Device-Info, Labels, Personalities
python3 tube/tools/tool.py "rdm set 7FF0:12345678 00E0 02"  # Personality 2 (3 Kanäle)
python3 tube/tools/tool.py "rdm set 7FF0:12345678 00F0 000A" # Startadresse 10
python3 tube/tools/tool.py "rdm identify 7FF0:12345678 1"   # weiß blinken
python3 tube/tools/tool.py "dmx 1=255 2=0 3=0" "dmx on"     # DMX senden (Seg 1 rot)
```

Bei DMX-Ausfall (> 1 s) gilt `DMX_LOSS_MODE` (Default: Werte halten; alternativ Blackout oder Weiß).

## Geräte-ID ins EEPROM schreiben

Die RDM-UID ist `7FF0:<ID>`; die 32-bit-ID steht big-endian ab EEPROM-Adresse 0.
Ist das EEPROM leer (`FFFFFFFF`), erzeugt die Firmware einmalig eine Zufalls-ID und speichert sie.
Feste ID pro Tube (nach dem Flashen, z. B. ID `00000001`):

```sh
AVRDUDE=~/.platformio/packages/tool-avrdude
$AVRDUDE/bin/avrdude -C $AVRDUDE/avrdude.conf -c stk500v1 -P /dev/cu.usbmodem101 -b 19200 -p m8 \
  -U eeprom:w:0x00,0x00,0x00,0x01:m
```

Die Einstellungen (Adresse, Personality, Label bis 32 Zeichen) liegen ab EEPROM-Adresse 0x10
(Magic 0x5A, Settings-Version 2 seit 0.5.0, CRC8) und werden bei ungültigem Inhalt auf Adresse 1 /
Personality 1 zurückgesetzt. **Achtung bei späteren RDM-Updates geschlossener Tubes:** Layout und
`SETTINGS_VERSION` nur mit Migration ändern – sonst verlieren alle Tubes beim Update ihre DMX-Adresse.

## Fuses (werden NICHT automatisch geschrieben)

Aktuell gelesen: **lfuse 0x2F, hfuse 0x8F, lock 0xE8** (ext. Quarz, BOD 4.0 V, WDTON aktiv, BOOTRST aus).

Mit RDM-Bootloader (siehe „Update über RDM“): hfuse **0x88** (BOOTRST + 2 KB Boot-Sektion ab 0x1800),
optional **0x80** (zusätzlich EESAVE). Alter 1-KB-Bootloader: 0x8A / 0x82.
WDTON bleibt aktiv – App und Bootloader rechnen damit.

> **Warnung:** SPIEN, RSTDISBL und CKSEL (lfuse) niemals ändern – sonst ist der Chip per ISP
> nicht mehr erreichbar bzw. läuft ohne passenden Takt nicht mehr.

Die Firmware rechnet mit WDTON: Watchdog ist immer an (Timeout 0.5 s, wird in der Hauptschleife zurückgesetzt).
Der herstellerspezifische RDM-PID `0x8000` (ENTER_BOOTLOADER) startet mit SET `424F4F54` („BOOT“) den Bootloader.

## Update über RDM (Bootloader `tube_boot_rdm`)

Flash-Aufteilung (Layout `BOOT_LAYOUT_2K`, `include/boot_shared.h`):

| Bereich | Inhalt |
|---|---|
| 0x0000–0x17EF | App (max. 6128 B) |
| 0x17F0–0x17FF | Trailer (16 B, siehe unten) |
| 0x1800–0x1FFF | `tube_boot_rdm` (2 KB = NRWW-Sektion, hfuse BOOTSZ=00) |

#### App-Trailer – stabiler Vertrag für Uploader (Pixelmapper, `flash_rdm.py`)

Jede fertige App-Datei `tube_fw-<version>.hex` (Build-Ausgabe in `.pio/build/isp/`) enthält App **und**
vollständigen Trailer. Ein Uploader berechnet nichts, er liest nur und überträgt das Image 1:1.
Alle Werte little-endian; Länge/CRC liegen an denselben Adressen wie im alten 4-Byte-Trailer.

| Adresse | Typ | Inhalt |
|---|---|---|
| 0x17F0 | u16 | `TRAILER_MAGIC` = 0x4D50 (Bytes `50 4D`, „PM“) |
| 0x17F2 | u8 | `TRAILER_FORMAT` = 1 |
| 0x17F3 | u8 | reserviert = 0xFF (in der CRC, wird nicht verglichen) |
| 0x17F4 | u16 | RDM-Hersteller-ID (0x7FF0) |
| 0x17F6 | u16 | RDM-Device-Model-ID (0x0001) |
| 0x17F8 | u32 | `FW_VERSION_ID` = 0x00MMmmpp, gleicher Wert wie DEVICE_INFO „Software Version ID“ (0.4.0 = 0x00000400) |
| 0x17FC | u16 | App-Länge in Byte (gerade, 1…6128) |
| 0x17FE | u16 | CRC16-CCITT (Poly 0x1021, Init 0xFFFF, nicht reflektiert) über Flash 0…Länge−1, **danach** über 0x17F0…0x17FB |

Prüfung durch den Uploader: Magic + Format, Länge ≤ 6128, CRC; dann Hersteller-/Model-ID gegen das Gerät
(App: Hersteller = obere 16 bit der UID, Model aus DEVICE_INFO; Bootloader: BOOT_INFO) und `FW_VERSION_ID`
gegen die installierte Version. Daten zwischen Länge und 0x17F0 gibt es nicht (0xFF).
Ein anderes Format bekommt eine neue `TRAILER_FORMAT`-Nummer; Format 1 bleibt so.

Quelle der Werte: `include/config.h` (`RDM_MANUFACTURER_ID`, `RDM_DEVICE_MODEL_ID`, `FW_VERSION_MAJOR/MINOR/PATCH`
→ `FW_VERSION_ID` und `FW_VERSION_LABEL`) – App (DEVICE_INFO), Trailer und Bootloader nutzen dieselben Konstanten.
Die Metadaten 0x17F0–0x17FB stehen als Section `.trailer` in `src/main.c` (Adresse setzt `extra_script.py`),
Länge + CRC trägt der Post-Build-Schritt in `extra_script.py` ein. Das Legacy-Env `isp_1k` hat keinen
erweiterten Trailer (alter 4-Byte-Trailer bei 0x1BFC, berechnet von `flash_dmx.py`).

Der Bootloader (`tube/boot_rdm`) ist ein RDM-Responder mit **derselben UID wie die App**
(7FF0 + Geräte-ID aus dem EEPROM). Updates laufen deshalb über jeden Standard-RDM-Controller bzw.
jede Art-Net-4-Node (ArtRdm) – der Bootloader antwortet auch auf die Discovery der Node, die UID
bleibt in der TOD. Er bleibt aktiv bei SET `0x8000 "BOOT"` an die App, bei ungültiger App oder wenn
in den ersten ~200 ms nach dem Reset ein GET `0x8010` an die UID kommt (Rettung). Nach FINISH oder
60 s ohne Bootloader-PID startet die App – jeweils nur, wenn der Trailer gültig ist (Magic/Format,
Hersteller-/Model-ID = eigene, Länge, CRC). DEVICE_INFO meldet im Bootloader Software-Version
`0xB0000003`, SOFTWARE_VERSION_LABEL `tube_boot_rdm 3`. Das Rettungsfenster liegt (im Emulator
gemessen) 26–225 ms nach dem Reset; davor prüft der Bootloader ~25 ms lang die App-CRC.

Bootloader v3 (gegenüber v2): SUPPORTED_PARAMETERS ist leer und PARAMETER_DESCRIPTION entfällt (die
Hersteller-PIDs werden nicht veröffentlicht, E1.20 10.4.1/Table A-3), IDENTIFY_DEVICE lässt alle
LED-Kanäle weiß blinken (10.11.1), DISC_MUTE/UN_MUTE setzen das Boot-Loader-Flag (0x0004), solange
keine gültige App installiert ist bzw. ein Update läuft (7.6.1), Hersteller-PIDs an FFFF:FFFFFFFF werden
ignoriert (6.2.10.2), Anfragen werden bis PDL 228 empfangen (vorher 66). BOOT_INFO-Format und PIDs
sind unverändert (`BOOT_RDM_PROTOCOL` 2), `flash_rdm.py` spricht v2 und v3.

| PID | | Daten |
|---|---|---|
| `8010` BOOT_INFO | GET | → 19 B, siehe unten |
| `8011` WRITE_PAGE | SET | Adresse (2 B) + 64 B → ACK, Erase/Write/Verify im Hintergrund; NACK BUFFER_FULL solange busy |
| `8012` APP_CRC | GET | Adresse (2 B) + Länge (2 B, ≤ 256) → CRC16 (in Stücken, 256 B ≈ 1,05 ms, damit die Antwort < 2 ms bleibt) |
| `8013` FINISH | SET | → ACK, dann Trailer-Prüfung: gültig → App, sonst bleibt er (letzter Fehler 2/3/4) |

BOOT_INFO-Antwort (PDL 19, Mehrbyte-Werte big-endian wie in RDM üblich):

| Byte | Inhalt |
|---|---|
| 0 | Version des Bootloaders (`BOOT_RDM_VERSION` = 3; Format seit 2) |
| 1 | Page-Größe (64) |
| 2–3 | Ende des App-Bereichs / Bootloader-Start (0x1800) |
| 4 | busy (1 = Seite wird noch geschrieben) |
| 5 | letzter Fehler, Lesen löscht ihn: 0 keiner, 1 Verify, 2 Länge/CRC falsch, 3 falsches Modell (Hersteller-/Model-ID), 4 unbekanntes Trailer-Format (oder leer) |
| 6–7 | RDM-Hersteller-ID (0x7FF0) |
| 8–9 | RDM-Device-Model-ID (0x0001) |
| 10–13 | Bootloader-Softwareversion (0xB0000003) |
| 14 | installierte App: Trailer gültig (0/1) |
| 15–18 | installierte App: `FW_VERSION_ID` aus ihrem Trailer |

Bytes 14–18 stammen aus der letzten Prüfung (Start, FINISH, 60-s-Timeout; die volle CRC dauert ~25 ms,
zu lang für eine RDM-Antwort). Ab dem ersten WRITE_PAGE ist Byte 14 = 0. So sieht ein Controller die
App-Version auch, wenn die Tube im Bootloader steht.

Update (`flash_rdm.py`, PIDs werden aus `include/rdm_boot.h` gelesen):

```sh
HEX=tube/firmware/.pio/build/isp/tube_fw-0.5.0.hex
python3 tube/tools/flash_rdm.py 7FF0:B06675AA $HEX                                              # über ESP32-Tool
python3 tube/tools/flash_rdm.py 7FF0:B06675AA $HEX --via artnet --node 2.0.0.10 --universe 0   # über Art-Net-Node
python3 tube/tools/flash_rdm.py 7FF0:B06675AA $HEX --allow-downgrade                            # ältere Version zulassen
python3 tube/tools/flash_rdm.py --tod --via artnet --node 2.0.0.10 --universe 0                # TOD der Node anzeigen
python3 tube/tools/flash_rdm.py --rescue manual 7FF0:B06675AA $HEX  # App tot, Tube zu: 24 V innerhalb 60 s selbst trennen/anstecken (Default bei nacktem --rescue)
python3 tube/tools/flash_rdm.py --rescue relay  7FF0:B06675AA $HEX  # App tot: Power-Cycle per Relais
python3 tube/tools/flash_rdm.py --rescue isp    7FF0:B06675AA $HEX  # App tot: Reset über Uno-ISP (avrdude-Signatur lesen, read-only)
python3 tube/tools/flash_rdm.py --selftest                            # Offline-Tests, keine Hardware
```

Ablauf: Trailer der Datei prüfen → DEVICE_INFO der App: Hersteller (UID) + Model müssen zum Trailer passen,
Version alt → neu ausgeben, Downgrade nur mit `--allow-downgrade` (sonst Abbruch, nichts geschrieben)
→ SET 0x8000 → BOOT_INFO pollen, Hersteller/Model des Bootloaders nochmals prüfen (bei `--rescue` ersetzt
das die App-Prüfung, Version dann aus BOOT_INFO) → Seiten schreiben (reine 0xFF-Seiten übersprungen,
Trailer-Seite zuerst mit leerem 16-B-Trailer, zuletzt echt – ein halb geschriebenes Image ist nie gültig)
→ APP_CRC je 256 B + Trailer vergleichen (abweichende Stücke einmal neu schreiben) → FINISH → nach 3 s
SOFTWARE_VERSION_LABEL/DEVICE_INFO der App prüfen (Version = Trailer). Ein Bootloader v1 (6-Byte-BOOT_INFO)
wird abgelehnt – er kennt den neuen Trailer nicht und muss einmal per ISP ersetzt werden.
Nach ENTER_BOOTLOADER schreibt die App zuerst noch offene EEPROM-Bytes (bis ~0,3 s), dann erst der Reset.
Die Art-Net-Node muss die UID in ihrer TOD haben (manche Nodes leiten RDM sonst nicht weiter).

### Einmalig per ISP: Umstellung auf den RDM-Bootloader

```sh
~/.platformio/penv/bin/pio run -d tube/boot_rdm
~/.platformio/penv/bin/pio run -d tube/firmware -e isp
python3 tube/tools/flash_rdm.py --hex-out tube_merged_rdm.hex \
  --boot tube/boot_rdm/.pio/build/boot/firmware.hex tube/firmware/.pio/build/isp/tube_fw-*.hex
AVRDUDE=~/.platformio/packages/tool-avrdude
$AVRDUDE/bin/avrdude -C $AVRDUDE/avrdude.conf -c stk500v1 -P /dev/cu.usbmodem101 -b 19200 -p m8 \
  -e -U flash:w:tube_merged_rdm.hex:i          # Chip-Erase löscht auch das EEPROM (Geräte-ID!)
```

Tuben, die schon `tube_boot_rdm 1` haben (hfuse bereits 0x88), bekommen dieses Image genauso einmal per ISP
(v1 prüft den alten 4-Byte-Trailer und würde die neue App ablehnen; `flash_rdm.py` verweigert v1). Die Fuse
bleibt dann unverändert.

**Danach, separat und erst nach erfolgreichem Verify:** Fuses lesen (lfuse muss 0x2F sein, hfuse 0x8F
oder 0x8A), dann **nur hfuse auf 0x88** schreiben (`-U hfuse:w:0x88:m`; mit EESAVE 0x80).

> **Warnung:** Es ändern sich nur BOOTSZ (→ 00) und BOOTRST. lfuse, SPIEN, RSTDISBL, CKSEL, WDTON
> niemals anfassen – sonst ist der Chip per ISP nicht mehr erreichbar oder läuft nicht mehr.
> Ohne die Fuse-Änderung startet direkt die App (kein Update über RDM möglich).

Agent für Serien-Umrüstung: `.claude/agents/tube-flasher-rdm.md`.

### E1.20-Konformität (App und Bootloader)

| Punkt | Umsetzung |
|---|---|
| ACK_TIMER / QUEUED_MESSAGE | Kein ACK_TIMER, Message Count immer 0 → QUEUED_MESSAGE/STATUS_MESSAGES nicht nötig. Bootloader „busy“ → NACK BUFFER_FULL (0x0007), Controller wiederholt; APP_CRC synchron in ≤ 256-B-Stücken (~1,05 ms + 180 µs Turnaround < 2 ms) |
| SUPPORTED_PARAMETERS | App: optionale PIDs + 0x8000, aber keine „Required“-PID aus Table A-3 (10.4.1: „shall not be reported“ – also auch nicht PARAMETER_DESCRIPTION/DMX_START_ADDRESS). Bootloader: leere Liste (Hersteller-PIDs „may or may not be included“) |
| PARAMETER_DESCRIPTION | App: 0x8000 (PDL 4, ASCII, SET). Bootloader: keine (seine Hersteller-PIDs sind nicht veröffentlicht → laut Table A-3 nicht nötig) |
| Pflicht-PIDs | DISC_UNIQUE_BRANCH/MUTE/UN_MUTE, SUPPORTED_PARAMETERS, DEVICE_INFO, SOFTWARE_VERSION_LABEL, IDENTIFY_DEVICE (GET/SET, sichtbar: weiß blinken); App zusätzlich PARAMETER_DESCRIPTION und DMX_START_ADDRESS (Bootloader: Footprint 0) |
| Control Field | App 0x0000; Bootloader 0x0004 (Boot-Loader-Flag), solange keine gültige App installiert ist bzw. ein Update läuft, sonst 0x0000 |
| Broadcast | FFFF:FFFFFFFF und 7FF0:FFFFFFFF: SET wird ausgeführt, nie beantwortet; GET an Broadcast ignoriert. Hersteller-PIDs an FFFF:FFFFFFFF werden ignoriert (6.2.10.2). App: SET 0x8000 per Broadcast/Vendorcast wird absichtlich *nicht* ausgeführt (nie alle Tubes gleichzeitig in den Bootloader) |
| Sub-Device | ≠ 0 → NACK SUB_DEVICE_OUT_OF_RANGE; 0xFFFF nur bei SET (wirkt auf Root, Antwort mit 0xFFFF) |
| NACK-Gründe | falsche PDL → FORMAT_ERROR, bekannte PID mit falscher CC → UNSUPPORTED_COMMAND_CLASS, unbekannte PID → UNKNOWN_PID, Wert außerhalb → DATA_OUT_OF_RANGE |
| Lange Anfragen | App empfängt jede zulässige Anfrage (PDL bis 231, Puffer 257 B) und antwortet (6.2.10.1). Bootloader bis PDL 228; 229–231 werden nicht empfangen (8-bit-Index, kein Bootloader-/Standard-PID braucht sie) |
| Antwortpaket | TN, Sub-Device, PID gespiegelt; Port-ID-Feld → Response Type; Prüfsumme 16 bit; Anfragen mit falscher Länge/Prüfsumme/Sub-Start-Code werden ignoriert |
| Timing | Antwort nach ≥ 180 µs Turnaround (< 2 ms), Break 200 µs (176–352), MAB 20 µs (12–88), Slots back-to-back; DUB-Antwort ohne Break, 7×FE + AA + kodierte UID/Prüfsumme |
| Treiber beim Start | App schaltet DE gleich nach dem Reset auf Empfang (vor dem 2-s-Selbsttest, 2.6); Bootloader ebenfalls als erstes |
| DEVICE_INFO | Protokoll 0x0100, Footprint je Personality, Startadresse (0xFFFF bei Footprint 0 = Bootloader), Personality 0/0 im Bootloader, 0 Sub-Devices, 0 Sensoren |

Im Emulator (zyklusgenau, echte Binärdateien, `tools/sim`) gemessen: App-Turnaround 241–458 µs, Break
200–243 µs, MAB 21–36 µs, Treiber aus 0,3–9 µs nach dem letzten Stoppbit; Bootloader 227–1335 µs
(APP_CRC über 256 B ist die längste Antwort), Break 200 µs, MAB 21 µs. Am Oszi noch nicht nachgemessen.

## Alter 1-KB-Bootloader `tube_boot` (nur noch geschlossene Tube)

Die geschlossene Tube A (UID laut Projektnotiz **7FF0:B06675AA**, hfuse 0x8A – vor Gebrauch mit
`rdm disc` bestätigen) behält den alten Bootloader. App dafür mit `pio run -e isp_1k` bauen und mit
`flash_dmx.py` laden – **nicht** mit `flash_rdm.py`. `firmware/tube_boot` liegt nur im Archiv `ledTubeXBAR500`.


Der alte Bootloader (`firmware/tube_boot`, 1 KB ab 0x1C00) läuft nach jedem Reset und startet die App,
wenn deren CRC-Trailer (Länge + CRC16 bei 0x1BFC) stimmt. Er bleibt aktiv bei RDM-SET `0x8000 "BOOT"`
(App setzt RAM-Magic, Watchdog-Reset), bei ungültiger App, oder wenn in den ersten ~150 ms nach dem
Reset ein HELLO an die eigene UID kommt (Rettung). Nach FINISH oder 30 s Ruhe → App.

Update im Betrieb (ESP32-Tool angeschlossen, Tube adressiert über ihre UID):

```sh
python3 tube/tools/flash_dmx.py 7FF0:B06675AA tube/firmware/.pio/build/isp_1k/firmware.hex
python3 tube/tools/flash_dmx.py --rescue 7FF0:B06675AA tube/firmware/.pio/build/isp_1k/firmware.hex  # App tot: Power-Cycle + HELLO
```

### (Alt) Einmalig per ISP: 1-KB-Bootloader + App flashen, dann Fuse – für neue Tubes nicht mehr verwenden

```sh
cd firmware/tube_boot && ~/.platformio/penv/bin/pio run && cd ../tube_fw && ~/.platformio/penv/bin/pio run -e isp_1k && cd ../..
# App + CRC-Trailer + Bootloader zu einem Image zusammenfügen
python3 tube/tools/flash_dmx.py --hex-out tube_merged.hex \
  --boot firmware/tube_boot/.pio/build/boot/firmware.hex tube/firmware/.pio/build/isp_1k/firmware.hex

AVRDUDE=~/.platformio/packages/tool-avrdude
# 1) Flash (mit Chip-Erase -e, löscht auch das EEPROM → danach Geräte-ID schreiben, s. o.)
$AVRDUDE/bin/avrdude -C $AVRDUDE/avrdude.conf -c stk500v1 -P /dev/cu.usbmodem101 -b 19200 -p m8 \
  -e -U flash:w:tube_merged.hex:i
```

**2) Fuse – separater Schritt, erst nach erfolgreichem Flash + Verify!**
Vorher lesen und prüfen, dass hfuse 0x8F und lfuse 0x2F sind:

```sh
$AVRDUDE/bin/avrdude -C $AVRDUDE/avrdude.conf -c stk500v1 -P /dev/cu.usbmodem101 -b 19200 -p m8 \
  -U lfuse:r:-:h -U hfuse:r:-:h
# NUR hfuse schreiben: 0x8F -> 0x8A (BOOTRST an, BOOTSZ=01 = 1 KB Boot-Sektion ab 0x1C00)
$AVRDUDE/bin/avrdude -C $AVRDUDE/avrdude.conf -c stk500v1 -P /dev/cu.usbmodem101 -b 19200 -p m8 \
  -U hfuse:w:0x8A:m
```

Optional **0x82** statt 0x8A: zusätzlich EESAVE, dann überlebt die Geräte-ID einen Chip-Erase.

> **Warnung:** lfuse nie anfassen, und in hfuse nur BOOTRST/BOOTSZ/EESAVE ändern.
> SPIEN, RSTDISBL und CKSEL niemals ändern – sonst ist der Chip per ISP nicht mehr erreichbar
> bzw. läuft ohne passenden Takt nicht mehr. Ohne die Fuse-Änderung startet direkt die App
> (Bootloader liegt dann brach, Update über DMX geht nicht).

Danach `rdm disc` – die App muss nach ~2 s Selbsttest antworten. Lock-Bits werden nicht gesetzt.

Protokoll (250000 8N2, jedes Paket nach BREAK; CRC16-CCITT 0x1021/0xFFFF big-endian über Startcode..Payload):

| | Aufbau |
|---|---|
| Anfrage | `F1` UID(6) cmd len payload CRC16 |
| Antwort | `F2` UID(6) cmd status len payload CRC16 (nach ~200 µs + 180 µs Break) |
| `01` HELLO | → Version, Page-Größe 64, App-Bereich 0x1C00 |
| `02` WRITE_PAGE | Adresse (2 B) + 64 B → Status (Erase + Write + Verify) |
| `03` READ_CRC | Länge (2 B) → CRC16 über Flash 0..Länge-1 |
| `04` FINISH | → Status, danach Start der App (nur wenn Trailer-CRC stimmt) |

Status: 0 OK, 1 falsches Argument, 2 Verify-Fehler, 3 App ungültig, 4 unbekanntes Kommando.
Mit dem ESP32-Tool lassen sich Pakete von Hand schicken: `bl <hex>` bzw. `blr <ms> <hex>` (wiederholt).

## RDM-Slot-Parameter (ab 0.2.0)

Neue PIDs (E1.20 / E1.37-1), alle nur GET, Ergebnis hängt von der aktuellen Personality ab
(Quelle: `pers_slot_*` in `src/personality.c`, gleiche Reihenfolge wie `pers_map`):

| PID | Name | Antwort |
|---|---|---|
| `0120` | SLOT_INFO | je Slot 5 B: Offset (2), Typ `00` (primary), Label-ID (2) |
| `0121` | SLOT_DESCRIPTION | Anfrage Slot-Nr. (2 B); Antwort Slot-Nr. + Text, z. B. "Seg 2 Green"; ungültig → NACK DATA_OUT_OF_RANGE |
| `0122` | DEFAULT_SLOT_VALUE | je Slot 3 B: Offset (2) + Default (0 Farbe, 255 Dimmer) |

Slots: P1 = Seg1 R,G,B … Seg4 R,G,B; P2 = R,G,B; P3 = R,G,B,Dimmer; P4 = 12 Farbkanäle, Dimmer zuletzt (Slot 12).
Label-IDs: Dimmer `0001`, Rot `0205`, Grün `0206`, Blau `0207`. Der RDM-Puffer hat seit 0.5.0 257 B: jede
zulässige Anfrage (PDL bis 231) wird empfangen und beantwortet; die längste Antwort ist SLOT_INFO (13 × 5 B).

Prüfen mit dem ESP32-Tool:

```sh
python3 tube/tools/tool.py "rdm get <uid> 0120"
python3 tube/tools/tool.py "rdm get <uid> 0121 0001"   # Slot 1
python3 tube/tools/tool.py "rdm get <uid> 0122"
```

Version 0.3.0: 2-KB-Layout (App ≤ 6140 B), PARAMETER_DESCRIPTION für 0x8000, strengere E1.20-NACKs.
Version 0.4.0: 16-B-Trailer mit Hersteller/Model/Version (App ≤ 6128 B), Bootloader `tube_boot_rdm 2` prüft
Modell + Format; Version nur noch in `config.h` (`FW_VERSION_MAJOR/MINOR/PATCH`).
Version 0.5.0 (E1.20-Audit 24.10.2026, Details in `../../DOKUMENTATION.md`): Anfragen bis PDL 231 werden
beantwortet (vorher > 32 B stumm verworfen), SUPPORTED_PARAMETERS ohne PARAMETER_DESCRIPTION, DEVICE_LABEL
32 Zeichen + NUL-Ende (Settings-Version 2), offene EEPROM-Schreibvorgänge vor dem Bootloader-Sprung, DE
sofort nach dem Reset auf Empfang. Dazu `tube_boot_rdm 3` (siehe „Update über RDM“).
Ausrollen über `flash_rdm.py` (RDM-Bootloader) bzw. `flash_dmx.py` + `isp_1k` (Tube A).
Die Fixture-Definition für die Open Fixture Library liegt in `../../fixture/xbar500-custom.json`.

# Pixelmapper Node – Plan

Stand: 2026-10-24. Ergebnis der Grilling-Session. Dieses Dokument hält die Entscheidungen fest, es ist noch keine Spezifikation.
UI-Details: [ui-konzept.md](ui-konzept.md).

> **Phase 0 (2026-10-24):** Nach der Prüfung gegen die Art-Net-4-Spec gelten einige Punkte dieses Plans geändert,
> maßgeblich ist [protocol/contract.md](../protocol/contract.md) §11/§12. Wichtigste Änderungen: kein Input-Broadcast
> (Spec verbietet ihn), ein Port pro `BindIndex`, ArtSync nur per Broadcast, Merge-Quelle = IP + Physical,
> Display-Helligkeit und Werksreset über die Standard-PIDs DISPLAY_LEVEL/FACTORY_DEFAULTS, Node-Modell 0x0002.

## Ziel

Ein eigener **4-Port Art-Net-4/RDM-Node** für Pixelmapper, zuerst als Werkzeug fürs eigene Lab (später rig-tauglich).
Er ersetzt den Aufbau aus ESP32 und `tool.py` zum Betreiben und Flashen der LED-Tubes. Er hält sich vollständig an Art-Net 4 und E1.20, damit auch fremde Software und Pulte ihn bedienen können.

## Hardware

| Thema | Entscheidung |
|---|---|
| MCU/Board | **Waveshare ESP32-P4-WIFI6-POE-ETH** (P4 Dual-Core 360 MHz, 32 MB PSRAM, IP101-PHY über RMII, 100 Mbit, PoE, ESP32-C6 über SDIO) |
| Warum nicht CH32V307 | Mit LED-Matrizen (32×32 RGB ≈ 6 Universes pro Matrix) reichen 10 Mbit nicht sicher; dazu kommen PoE, OTA und die zwei Kerne |
| ESP32-C6 | vorerst **aus** (Treiber mitgebaut, standardmäßig deaktiviert). Reserviert für ein späteres eigenes Funkprotokoll (W-DMX ist proprietär) |
| Gehäuse | **1 HE, halbe 19"-Breite** (Formfaktor wie eine Funkstrecke). Vorne Display, Encoder, BACK. Hinten 4× XLR-5 (female), RJ45, Strom |
| Ports | **4**, per Software Output (DMX + RDM) oder Input (nur DMX). Input braucht einen Gender Changer. Später ein Modus „Pixelprotokoll“ für Matrizen; ein Port kann dann mehrere Universes belegen |
| Transceiver | **3,3 V Pflicht** (P4-Pins vertragen keine 5 V): Prototyp MAX3485/SP3485, später **THVD1450** (schnell genug für Matrix-Ports). Kein MAX485 mit 5 V ohne Spannungsteiler an RO |
| Leitung | Der Controller sorgt für Line Bias und Terminierung nach E1.20 |
| Display | 1,8" TFT ST7735S, 128×160, SPI, quer eingebaut. Eine **Statusleiste ersetzt die Status-LEDs**. Der DSI-Port des Boards bleibt ungenutzt |
| Bedienung | Drehencoder mit Druckknopf + BACK-Taster (BACK lang = zurück zum Startbildschirm) |
| Strom | Prototyp über USB. Eigene Platine mit 9–30 V DC (verriegelnder Hohlstecker), USB-C und PoE (Dioden-ODER auf 5 V) |
| Board-Abstraktion | Pro Hardware-Revision eine Pin-Tabelle. Die Revision erkennt die Firmware beim Start (eigene Platine: Widerstands-Code an GPIOs, Prototyp: NVS). Eine Firmware für alle Revisionen |
| Pins Prototyp | 28 freie GPIOs. Bedarf ca. 22: 4×(TX, RX, DE), Display 6, Encoder + BACK 4 |

## Firmware-Architektur

| Thema | Entscheidung |
|---|---|
| Sprache/Build | **C, ESP-IDF direkt** (`idf.py`, aktuelle stabile Version mit P4-Unterstützung) |
| Kern 1 | **Nur die Port-Engine**: 4 Zustandsmaschinen für DMX und RDM, Discovery, Ausgabetakt, `ArtSync`-Übernahme, später das Matrix-Protokoll |
| Kern 0 | Ethernet/lwIP, Art-Net, Konfiguration, OTA, UI (niedrigste Priorität) |
| Kopplung zwischen den Kernen | Doppelpuffer pro Universe, Warteschlange für RDM-Anfragen und -Antworten, Ereignisse (TOD geändert, Signal verloren, Statistik) |
| DMX/RDM-Treiber | **Selbst geschrieben** auf UART im RS-485-Modus (`esp_dmx` nicht verwendet: P4-Unterstützung unbelegt, RDM-Umfang zu klein) |
| Einstellungen | ESP-IDF NVS |
| Sprache der UI | Englisch |

## Netzwerk & Art-Net 4

- **Nur Art-Net 4, vollständig.** Kein sACN; als weitere Datenquelle für einen Port kann es später dazukommen.
- **IP:** zuerst DHCP, ohne Antwort nach ca. 5 s Fallback auf 2.x.x.x/8. Die Einstellung wird gespeichert. Werksreset per Tastenkombination beim Einschalten.
- **Discovery:** `ArtPoll`/`ArtPollReply`. Bei mehr als 4 Universes (Matrizen) über `BindIndex`. Ändert sich etwas, schickt der Node von sich aus ein `ArtPollReply`.
- **Konfiguration:** `ArtAddress` (Namen, Port-Adresse, Merge, Richtung, RDM ein/aus, Output-Stil, Failsafe, LED Mute/Normal), `ArtIpProg`, `ArtInput`.
- **Output:** DMX läuft dauerhaft mit ca. 40 Hz. Merge **HTP** als Standard (LTP wählbar), höchstens 2 Quellen, eine Quelle verfällt nach 10 s. `ArtSync`; ohne Sync länger als 4 s gibt der Node direkt aus.
- **Failsafe:** alle 4 Modi (Hold/Zero/Full/Scene), Standard **Hold**. Datenverlust ist in der Statusleiste und im `ArtPollReply` sichtbar.
- **Input:** DMX → `ArtDmx` per **Unicast** an Nodes, die das Universe ausgeben (der Node pollt selbst alle ca. 3 s), und an Controller, die den Node in den letzten ca. 10 s gepollt haben. Broadcast per PID zuschaltbar. Gesendet wird bei Änderung und mindestens jede Sekunde als Keepalive. Statusbit „kein Signal“.
- **Diagnose:** `ArtDiagData` und die Statusfelder im PollReply.

## RDM

- **Discovery:** vollständig beim Start, bei Port-Aktivierung und bei `ArtTodControl` Flush. Danach **Hintergrundsuche alle ca. 3 s** (DUB ohne Un-Mute, also nur neue Geräte) mit Präsenzprüfung bekannter Geräte (`DISC_MUTE`, entfernt nach 3 Fehlversuchen). Bei Änderung `ArtTodData`. Intervall per PID einstellbar; per `ArtAddress` lässt sich RDM pro Port abschalten. Die DMX-Rate bleibt über ca. 30 Hz.
- **Weiterleitung:** **Jedes `ArtRdm` wird weitergeleitet**, auch an UIDs, die nicht in der TOD stehen, sowie an Broadcasts und Herstellerbroadcasts. Pro Port gibt es eine Warteschlange, jede Anfrage wird beantwortet bzw. läuft sauber in den Timeout.
- **TOD:** bis zu 512 Geräte pro Port, Labels im PSRAM zwischengespeichert.
- **Node als RDM-Responder:** UID `7FF0:8xxxxxxx` (die unteren 28 Bit der MAC). Er ist über `ArtRdm` an seine UID erreichbar und unterstützt die Standard-PIDs (DEVICE_INFO, Label, Software-Version, IDENTIFY usw.).
- **Hersteller-PIDs des Nodes:** alles, was Art-Net nicht abdeckt (DMX-Timing, Discovery-Intervall, Display, Panelsperre, Input-Broadcast, Adressformat …). `PARAMETER_DESCRIPTION` ist vollständig gepflegt, **damit Pixelmapper die Einstellungsseiten selbst erzeugen kann**. Wo E1.37-1 oder -2 passende Standard-PIDs haben, werden die genommen.
- **`BOOT_CATCH(port, uid)`:** Der Node schickt bis zu 60 s lang alle ca. 20 ms ein GET BOOT_INFO an die UID, damit er nach einem Power-Cycle das Boot-Fenster der Tube (ca. 200 ms) trifft. So geht die Rettung übers Netzwerk, ohne ESP32.
- **ESTA-ID:** `7FF0` ist der Prototypen-Bereich, OEM-Code ebenfalls für den Prototyp. Bevor Geräte weitergegeben werden, braucht es eine eigene ESTA-Hersteller-ID und einen eigenen Art-Net-OEM-Code.

## Firmware-Updates

| Ziel | Weg |
|---|---|
| **Node** | `ArtFirmwareMaster` **und** RDM → ESP-IDF-OTA in die inaktive Partition, **Rollback** wenn sich die neue Firmware nicht als gültig markiert. RDM über einen **eigenen PID-Satz mit 32-Bit-Offsets** (die Tube-PIDs `0x8010`–`0x8013` haben nur 16-Bit-Adressen und 64-B-Seiten), siehe [protocol/contract.md](../protocol/contract.md) |
| Node-Prüfung | Ein eigener Beschreibungsblock im Image (ESTA-Hersteller-ID, DEVICE_MODEL_ID, Liste der unterstützten HW-Revisionen) wird **schon im ersten Block** geprüft. Passt etwas nicht: Abbruch (`ArtFirmwareReply` FAIL bzw. NACK). Keine Signatur (vielleicht später) |
| Node-Notfall | USB-C über den ROM-Bootloader des P4 |
| **Tubes** | Unverändert per RDM über `ArtRdm`. Der Node reicht nur durch. Hersteller und Modell prüft `pixelmapper-net` **vor dem Senden** anhand des Image-Formats in `protocol/`. Optional prüft später auch der Tube-Bootloader |
| C6 | Vorerst nicht nötig (aus). Später über `esp_hosted` vom P4 aus |

## Konfiguration & Zusammenarbeit

- **Alles** lässt sich über Art-Net/RDM aus Pixelmapper einstellen, **und alles** auch am Gerät.
- **Wer zuletzt schreibt, gewinnt, ohne Dialog** (wie in Pixelmapper). Jede Änderung, ob lokal oder remote, wird sofort gemeldet (`ArtPollReply` + RDM-Queued-Message). Bearbeitet jemand am Gerät gerade einen Wert, den Pixelmapper ändert, zeigt der Editor kurz „↻ changed by Pixelmapper“ an.

## UI (Kurzfassung, Details siehe UI-Konzept)

- Raster: Statusleiste 16 px, Pfad-Kopfzeile, 6 Zeilen à 20 Zeichen (Schrift 8×16 fett). Gezeichnet wird mit einem Streifenpuffer, nur geänderte Bereiche werden neu gezeichnet.
- **Startseite:** Short Name, darunter IP + Modus (kleine Schrift), darunter alle 4 Ports (Badge, Adresse, fps, Statuskürzel). Ohne Kopfzeile, d. h. du bist auf Home.
- Port-Adresse: Standard `Net:Sub:Uni`, auf eine 15-Bit-Zahl umschaltbar, im Editor steht immer beides.
- **Panelsperre:** per PID oder BACK + Drücken für 3 s; die 3-s-Kombination hebt auch eine Sperre von Pixelmapper auf (ohne PIN, wird gemeldet).
- **Rückfrage** nur bei einem Port, der gerade Daten führt (Richtung, Deaktivieren, Universe ändern). Von Pixelmapper aus nie.
- **Display:** nach dem Timeout gedimmt, nie ganz aus. **Art-Net LED Mute schaltet es wirklich aus**, bis zur nächsten Bedienung oder bis LED Normal kommt.
- **RDM am Gerät:** Geräteliste pro Port und gesamt. Pro Gerät Identify, DMX-Startadresse, Personality, Label. Tube-Updates werden nur angezeigt, nicht am Gerät gestartet.
- **Zu tun im UI-Konzept:** den Vorschlag „ArtCommand als Notweg“ streichen (Entscheidung: nur RDM-PIDs), auf Englisch umstellen, die Startseite auf Name + IP umbauen, die Konfliktregel auf „zuletzt schreibt gewinnt“ ändern, „LED Mute = aus“ übernehmen.

## Repo & Werkzeuge

```
pixelmapper-hardware/            (neues Monorepo, git)
├── node/
│   └── firmware/                ESP-IDF-Projekt (C)
├── tube/
│   ├── firmware/                tube_fw (PlatformIO)
│   ├── boot_rdm/                tube_boot_rdm (PlatformIO)
│   └── tools/                   flash_rdm.py, tool.py, … (tube-spezifische Tests)
├── protocol/
│   ├── pids.toml                einzige Quelle für alle Hersteller-PIDs → erzeugt C-Header + Rust-Konstanten
│   ├── image-format.md          Images von Node und Tubes (Hersteller, Modell, HW-Revisionen, CRC)
│   └── vectors/*.json           gemeinsame Testvektoren (Art-Net + RDM)
└── crates/pixelmapper-net/      Rust: Art-Net 4, RDM, Discovery, Konfiguration, Firmware-Update + Kommandozeile `pmnet`
```

- Das jetzige Projekt `ledTubeXBAR500` bleibt als **Archiv** des Reverse Engineerings bestehen (Dumps, Logs, Kamerabilder, ESP32-Aufbau).
- `pmnet`: `poll`, `tod`, `rdm get/set`, `node config …`, `flash <uid> <image>`, `flash --rescue` (über `BOOT_CATCH`). Dazu ein simulierter Tube-Bootloader für Tests ohne Hardware.
- Pixelmapper (Backend Rust) bindet `pixelmapper-net` als Git- oder Pfad-Abhängigkeit ein.

## Abnahme („sauber nach Standard“)

| Ebene | Test |
|---|---|
| Protokoll-Code | C-Unit-Tests auf dem Rechner und Rust-Tests, beide gegen `protocol/vectors/` |
| Art-Net 4 | **DMX-Workshop** (Windows): Node finden, Ports, `ArtAddress`/`ArtIpProg`, `ArtFirmwareMaster`. Dazu eine zweite Software (QLC+ oder grandMA onPC) |
| RDM als Controller | **OLA** über den Node: Discovery, GET und SET an Tubes und an ein unabhängiges RDM-Gerät |
| RDM als Responder | `rdm_responder_test.py` von OLA gegen die Node-UID (und gegen die Tube-Firmware) |
| Unabhängiges RDM-Gerät | ESP32 mit einer RDM-Responder-Implementierung, die nicht von uns stammt |
| Timing | Pico als Logikanalysator (gusmanb LogicAnalyzer oder sigrok-pico + PulseView): Break, MAB, Abstände zwischen Slots, RDM-Antwortzeiten nach E1.11/E1.20 |
| Robustheit | Dauertest über 24 h: 4 Universes mit 44 Hz, gleichzeitig RDM im Hintergrund und Konfigurationsänderungen. Ein Tube-Update über den Node, während auf den anderen Ports DMX läuft |

## Vorgeschlagene Reihenfolge

1. Repo `pixelmapper-hardware` anlegen, Tube-Firmware und -Werkzeuge übernehmen, `protocol/pids.toml` mit Generator
2. Hardware Rev 0 löten (Waveshare + 4× MAX3485, Display, Encoder), Einbaulagen des Displays testen
3. Board-Abstraktion + Port-Engine: DMX-Output auf 4 Ports, Timing per Pico messen
4. Ethernet + Art-Net: `ArtPoll`/`ArtPollReply`, `ArtDmx`, IP-Logik (DHCP/Fallback), `ArtSync`, Merge, Failsafe
5. RDM-Controller: Discovery, Hintergrundsuche, TOD/`ArtTodData`, Weiterleitung von `ArtRdm`, `BOOT_CATCH`
6. Node als RDM-Responder + Hersteller-PIDs + `ArtAddress`/`ArtIpProg`/`ArtInput` vollständig
7. Input-Modus (DMX → Art-Net per Unicast)
8. OTA über `ArtFirmwareMaster` und RDM, Image-Prüfung, Rollback
9. UI nach Konzept
10. `pixelmapper-net` + `pmnet` parallel ab Schritt 4, Abnahmetests fortlaufend
11. Später: Matrix-Modus, eigene Platine (Rev 1: PoE, THVD1450, galvanische Trennung), ggf. sACN, Funk über den C6, signierte Images

## Offene Punkte

- Höhe des Waveshare-Boards mit PoE-Modul nachmessen (Einbau in 1 HE)
- Freie GPIOs aus dem Pinout bzw. Schaltplan von Waveshare zuordnen
- Maximale UART-Baudrate des P4 für den späteren Matrix-Modus prüfen
- DEVICE_MODEL_ID für den Node festlegen
- Matrix-Protokoll (später)

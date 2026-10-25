# Verdrahtung Rev 0 (Lötaufbau auf dem Waveshare ESP32-P4-WIFI6-POE-ETH)

Stand: 2026-10-25. Gilt für die Pin-Tabelle `s_rev0` in [src/pm_board_tables.c](src/pm_board_tables.c).
**Nichts davon ist auf Hardware geprüft.** Die Belegung stammt aus dem Waveshare-Schaltplan und ist nur auf dem Papier
abgeglichen.

Quellen:
- Schaltplan: <https://files.waveshare.com/wiki/ESP32-P4-WIFI6-POE-ETH/ESP32-P4-WIFI6-POE-ETH-Schematic.pdf>
  (Header P6, PHY U11 IP101GRI, Netze für C6, SD und Codec)
- Wiki: <https://www.waveshare.com/wiki/ESP32-P4-WIFI6-POE-ETH>
- Ethernet-Demo: `waveshareteam/ESP32-P4-Platform`, `examples/esp-idf/11_ethernetbasic/components/ethernet_init/Kconfig.projbuild`

> **Regel 1: keine 5-V-Signale an den P4.** Die P4-GPIOs sind nicht 5-V-tolerant. Alle Transceiver, das Display und
> der Encoder laufen an **3,3 V** (Header-Pin 2 oder 18). Ein MAX485 an 5 V ist verboten, auch „nur zum Testen“.
> Die 5-V-Pins 1 und 3 des Headers bleiben frei, höchstens die Display-Hintergrundbeleuchtung darf von dort versorgt
> werden (siehe unten).

## 1. Header P6 (2×20)

Waveshare zählt anders als der Raspberry Pi: **Pin 1 = 5 V, Pin 2 = 3V3**. Die ungeraden Pins bilden eine Reihe, die
geraden die andere. Auf dem Silkscreen sind die Reihen mit „1“ und „2“ markiert.

| Pin | Signal | Verwendung Rev 0 | | Pin | Signal | Verwendung Rev 0 |
|---|---|---|---|---|---|---|
| 1 | 5V | – | | 2 | 3V3 | Versorgung Transceiver, Display, Pull-ups |
| 3 | 5V | – | | 4 | GPIO7 | (I2C SDA, Codec/Kamera, frei lassen) |
| 5 | GND | GND | | 6 | GPIO8 | (I2C SCL, frei lassen) |
| 7 | GPIO37 | (UART0 TX, Konsole) | | 8 | GPIO23 | **Encoder A** |
| 9 | GPIO38 | (UART0 RX, Konsole) | | 10 | GND | GND |
| 11 | GPIO22 | **Port 3 DE** | | 12 | GPIO21 | **Port 3 RX** (RO) |
| 13 | GND | GND | | 14 | GPIO20 | **Port 3 TX** (DI) |
| 15 | GPIO5 | **Port 4 TX** (DI) | | 16 | GPIO6 | **Encoder B** (siehe C6) |
| 17 | GPIO4 | **Port 4 RX** (RO) | | 18 | 3V3 | Versorgung |
| 19 | GND | GND | | 20 | GPIO3 | **Port 4 DE** |
| 21 | GPIO1 | **BACK** | | 22 | GPIO2 | **Display BL** (PWM) |
| 23 | GPIO36 | **Display CS** (Strapping-Pin) | | 24 | GPIO0 | **Encoder-Taster** |
| 25 | GPIO32 | **Display DC** | | 26 | GND | GND |
| 27 | GPIO25 | **Port 1 TX** (DI) | | 28 | GPIO24 | **Port 1 RX** (RO) |
| 29 | GND | GND | | 30 | GPIO33 | **Port 1 DE** |
| 31 | GPIO54 | (C6_CHIP_PU, Firmware hält den C6 im Reset) | | 32 | GPIO26 | **Port 2 DE** |
| 33 | GND | GND | | 34 | GPIO48 | **Display SCLK** |
| 35 | GPIO46 | **Display MOSI** | | 36 | GPIO53 | (Verstärker-Enable PA_CTRL, frei lassen) |
| 37 | GPIO27 | **Port 2 TX** (DI) | | 38 | GPIO47 | **Port 2 RX** (RO) |
| 39 | GPIO45 | (SD-Karten-Strom, frei lassen) | | 40 | GND | GND |

### Zusammenfassung nach Funktion

| Funktion | GPIO | Header | Peripherie |
|---|---|---|---|
| Port 1 TX / RX / DE | 25 / 24 / 33 | 27 / 28 / 30 | UART1 |
| Port 2 TX / RX / DE | 27 / 47 / 26 | 37 / 38 / 32 | UART2 |
| Port 3 TX / RX / DE | 20 / 21 / 22 | 14 / 12 / 11 | UART3 |
| Port 4 TX / RX / DE | 5 / 4 / 3 | 15 / 17 / 20 | UART4 |
| Display SCLK / MOSI / CS / DC / BL | 48 / 46 / 36 / 32 / 2 | 34 / 35 / 23 / 25 / 22 | SPI2, BL = LEDC |
| Display RST | – | – | RC-Reset, Firmware sendet SWRESET |
| Encoder A / B / Taster, BACK | 23 / 6 / 0, 1 | 8 / 16 / 24, 21 | aktiv low |

### Warum diese Pins (Engpass)

Der Header hat nur **13 GPIOs ohne andere Verwendung**: 2, 3, 4, 5, 20, 21, 22, 23, 32, 33, 46, 47, 48. Gebraucht
werden 21 (4 × 3 für die Ports, 5 für das Display, 4 für die Bedienung). Die übrigen 8 kommen von Pins mit Nebenrolle:

| GPIO | Nebenrolle auf dem Board | Warum trotzdem nutzbar |
|---|---|---|
| 24/25 | interner USB-FS-PHY 0 (USB-Serial-JTAG), D− / D+ | Konsole und Flashen laufen über den CH343 an UART0, der OTG-Port nutzt den HS-PHY. IDF schaltet das USB-Pad ab, sobald der Pin als GPIO konfiguriert wird. **D+ (25) hat nach dem Reset einen USB-Pull-up**, deshalb liegt dort nur TX (Ruhepegel ohnehin high) |
| 26/27 | interner USB-FS-PHY 1 (OTG 1.1), D− / D+ | wie oben, TX auf D+ (27), DE auf D− (26) |
| 0/1 | Pads des 32-kHz-Quarzes Y2 (10 MΩ + 22 pF), ob bestückt, ist unklar | nur als Taster-Eingänge genutzt. Die 22 pF wirken als Entprellung. Voraussetzung: RTC-Takt bleibt `CONFIG_RTC_CLK_SRC_INT_RC` (IDF-Standard) |
| 6 | C6_IO2 über R52 (0 Ω) | Der C6 bleibt vorerst aus; `pm_board_init()` hält ihn über GPIO54 im Reset, dann sind seine Pins hochohmig. Nur als Eingang (Encoder B) genutzt, damit es auch bei laufendem C6 keinen Kurzschluss gibt. Wer den C6 später einschaltet, entfernt R52 oder verlegt Encoder B |
| 36 | Strapping-Pin (Download-Modus braucht GPIO36 = high, wenn GPIO35/BOOT = low) | Display-CS mit externem **10-kΩ-Pull-up nach 3V3**: Das ist der richtige Strap-Pegel und hält das Display während des Boots abgewählt |

GPIO2–5 sind auch die JTAG-Pins (MTCK/MTDI/MTMS/MTDO) des P4. Debuggt wird über USB-Serial-JTAG, nicht über diese
Pins. Für den Display-Reset ist kein Pin mehr frei. Wenn er doch einen braucht, ist GPIO53 der Kandidat (dann bleibt
der Lautsprecherverstärker dauerhaft an, ohne Lautsprecher ist das harmlos).

### Nicht anfassen

- **Ethernet (RMII, IP101GRI, PHY-Adresse 1):** MDC 31, MDIO 52, PHY-Reset 51, REF_CLK 50 (50 MHz vom PHY **in** den
  P4), TXD0 34, TXD1 35, TX_EN 49, RXD0 29, RXD1 30, CRS_DV 28. Im Schaltplan geprüft: identisch mit den
  ESP-IDF-Defaults und der Waveshare-Demo. Das Wiki nennt RXD0/RXD1 vertauscht (30/29). Das IO_MUX des P4 erlaubt
  RXD0 aber nur auf 29/46/52, also gilt 29/30. Keiner dieser Pins liegt auf dem Header.
- C6 über SDIO: 14–19. SD-Karte: 39–44, Strom 45. Codec/I2S: 9–13, I2C 7/8, Verstärker 53. UART0: 37/38. BOOT: 35.

### Keine Doppelbelegung (Display gegen RMII)

Die RMII-Pins stehen vollständig in `pm_board_eth_pins_t` (`s_rev0.eth` in
[src/pm_board_tables.c](src/pm_board_tables.c)); pm_net soll sie dort lesen, nicht aus Kconfig.

| Gruppe | GPIOs |
|---|---|
| RMII/PHY (on board) | MDC 31, MDIO 52, PHY-Reset 51, REF_CLK 50, TXD0 34, TXD1 35, TX_EN 49, RXD0 29, RXD1 30, CRS_DV 28 |
| Display | SCLK 48, MOSI 46, CS 36, DC 32, BL 2 |
| Ports | 25/24/33, 27/47/26, 20/21/22, 5/4/3 |
| Bedienung | 23, 6, 0, 1 |
| C6-Reset | 54 |

Geprüft (2026-10-25): Keine GPIO kommt zweimal vor; Display und RMII überschneiden sich in der Tabelle nicht.
Verwechslungsgefahr besteht nur mit den **alternativen** IO_MUX-Pins des EMAC: RXD0 46 (= Display MOSI), RXD1 47
(= Port 2 RX), TX_EN 33 (= Port 1 DE), CRS_DV 45. Diese Alternativen dürfen in keiner Pin-Tabelle und keinem
Kconfig-Default auftauchen, solange Rev 0 die Pins anders nutzt. Die Kconfig-Optionen `PM_NET_RMII_*` bieten sie noch
an (Paket A ersetzt sie durch die Tabelle).

Annahmen aus dem Waveshare-Schaltplan (nicht auf Hardware geprüft): Die Header-GPIOs 2, 32, 36, 46, 48 haben auf dem
Board keine weitere Beschaltung außer dem Strapping-Pull an GPIO36; die RMII-Netze liegen nicht auf dem Header; der
IP101GRI liefert REF_CLK (50 MHz) an GPIO50.

**Selbstprüfung:** `pm_board_pins_unique()` prüft jede Tabelle (Ports, Display, Bedienung, Ethernet inkl. REF_CLK,
C6-Reset) auf doppelte und ungültige GPIOs (≥ 55). Der Hosttest `test_board_pins` in `pm_port/test_host` (ctest
`pm_port`) läuft über alle Tabellen; `pm_board_init()` meldet eine kaputte Tabelle zusätzlich im Log.

## 2. RS-485-Ports (4×)

Transceiver für den Prototyp: **MAX3485 oder SP3485** (3,3 V, Halbduplex, 10 Mbit/s). Später THVD1450.

```
                 3V3                                   3V3
                  │                                     │
                 10k                              R_bias 270 Ω
 RX (GPIO) ──────┴──── 1 RO                            │
                       2 /RE ─┐              ┌── 6 A ──┴──┬──────── XLR Pin 3 (Data+)
 DE (GPIO) ────────┬── 3 DE ──┘   MAX3485    │            │
                  10k                        │          120 Ω  R_term
                   │                         │            │
                  GND        TX (GPIO) ── 4 DI   7 B ──┬──┴──────── XLR Pin 2 (Data−)
                                                       │
                       5 GND ── GND               R_bias 270 Ω
                       8 VCC ── 3V3 (+100 nF)          │
                                                      GND          XLR Pin 1 = GND/Schirm
```

| Verbindung | Wert | Zweck |
|---|---|---|
| /RE und DE gebrückt an den DE-GPIO | – | Die Firmware schaltet beides gemeinsam: high = senden, low = empfangen |
| DE → GND | 10 kΩ | **Pflicht.** Während Reset und Boot ist der GPIO hochohmig; ohne Pull-down würde der Transceiver die Leitung treiben |
| RO → 3V3 | 10 kΩ | RO ist hochohmig, solange /RE high ist. Der interne Pull-up ist zusätzlich aktiv |
| VCC | 3,3 V + 100 nF direkt am IC | **Nie 5 V** |
| Line Bias (Controller-Seite, E1.20) | A → 3V3 über 270 Ω, B → GND über 270 Ω | Definierter Ruhepegel „Mark“, solange niemand treibt (RDM-Umschaltung, Responder-Pausen) |
| Terminierung | 120 Ω zwischen A und B | E1.20: Der Controller-Port ist terminiert, das andere Ende der Linie trägt einen Terminator |
| XLR-5 female | Pin 1 GND/Schirm, Pin 2 Data− (B), Pin 3 Data+ (A), Pin 4/5 frei | E1.11 |
| ESD (empfohlen) | SM712 o. ä. an A/B gegen GND | Schutz bei offenen Leitungen |

**Bias-Rechnung:** Mit 120 Ω am Node und 120 Ω am Leitungsende liegen 60 Ω zwischen A und B. Die Differenzspannung
im Ruhezustand ist dann 3,3 V · 60 / (270 + 60 + 270) ≈ **0,33 V**. Das liegt über der Empfängerschwelle von
±200 mV der Transceiver. Querstrom: ca. 5,5 mA pro Port. Den genauen E1.20-Sollwert für die Bias-Spannung belegt
unser Digest nicht ([ungeprüft]). Vor der eigenen Platine an der Norm prüfen. Mit 5-V-Bias bleibt der Wert bei 680 Ω
etwa gleich, aber 5 V gibt es auf dem Signalpfad bei uns nicht.

Der Bias bleibt auch im Input-Modus aktiv (der Node ist dann das Ende der Linie). Ein Gender Changer ist nötig.

**Rev 0 ist nicht galvanisch getrennt.** GND der DMX-Leitung ist GND des Boards und damit über USB GND des Rechners.
Im Lab reicht das. Nur mit Geräten an derselben Steckdosenleiste betreiben. Rev 1 bekommt eine Trennung.

**Stromversorgung 3V3:** Ein MAX3485 zieht beim Senden in 60 Ω etwa 30–40 mA, dazu kommen Bias und Display. Alle vier
Ports zusammen brauchen bis zu ca. 200 mA aus dem 3V3-Regler des Boards. Dessen Belastbarkeit vor dem Aufbau im
Schaltplan nachsehen (offen).

## 3. Display ST7735S (1,8", 128×160)

| Modul-Pin | an | Bemerkung |
|---|---|---|
| VCC | 3V3 | Module mit eigenem LDO und Pegelwandler: trotzdem 3V3 einspeisen, nicht 5 V (sonst können 5-V-Pegel zurückkommen) |
| GND | GND | |
| SCK/SCL | GPIO48 | SPI2 SCLK, bis ca. 15 MHz |
| SDA/MOSI | GPIO46 | |
| CS | GPIO36 | **10 kΩ Pull-up nach 3V3** (Strapping, siehe oben) |
| A0/DC | GPIO32 | |
| RESET | RC-Glied: 10 kΩ nach 3V3, 1 µF nach GND | kein GPIO frei; die Firmware schickt SWRESET (0x01) beim Init |
| LED/BL | GPIO2 über NPN/N-MOSFET (z. B. 2N7002, Gate 100 Ω, Pull-down 100 kΩ) | aktiv high, PWM. Die LED darf aus 3V3 oder 5 V versorgt werden; nur der Transistor hängt am GPIO |

## 4. Encoder und BACK

| Signal | GPIO | Beschaltung |
|---|---|---|
| Encoder A | 23 | Kontakt nach GND, 10 kΩ Pull-up nach 3V3, 10 nF nach GND |
| Encoder B | 6 | wie A |
| Encoder-Taster | 0 | Taster nach GND, 10 kΩ Pull-up nach 3V3 |
| BACK | 1 | Taster nach GND, 10 kΩ Pull-up nach 3V3 |

Gemeinsamer Encoder-Pin (C) an GND. Die internen Pull-ups des P4 sind ebenfalls aktiv; die externen machen die Pegel
an den Quarz-Pads (GPIO0/1) und am C6-Pin (GPIO6) eindeutig.

## 5. Revision

- Rev 0 hat **keinen** Widerstands-Code. Die Firmware erkennt Rev 0, wenn `hw_revision_override` = 0xFF ist und kein
  Code-Pin definiert ist, oder wenn im NVS 0 steht.
- Rev 1 (eigene Platine) codiert die Revision auf bis zu 4 GPIOs: kräftiger Pull-up (Bit 1) oder Pull-down (Bit 0),
  z. B. 4,7 kΩ. Die Firmware liest jeden Pin einmal mit internem Pull-up und einmal mit internem Pull-down. Folgt der
  Pin dem Pull, ist er offen. Sind alle Pins offen, ist es Rev 0. Code 0 entspricht Rev 1. Ein Gemisch aus offenen
  und belegten Pins ist ungültig (`pm_board_decode_rev_code`, Hosttest in `pm_port/test_host`).
- **Bedingung für Rev 1:** Die Code-Pins müssen auf Rev 0 offen sein, also auf GPIOs liegen, die Rev 0 nicht nutzt
  oder die dort an reinen Eingängen hängen. Welche das sind, legt das Platinen-Layout fest.

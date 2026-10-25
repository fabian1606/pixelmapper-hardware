# pm_port – Messplan (Raspberry Pi Pico als Logikanalysator)

Stand: 2026-10-25. Die Port-Engine baut und besteht die Hosttests (`test_host/`, simulierte UART). **Auf Hardware ist
noch nichts gemessen.** Dieser Plan legt fest, was gemessen wird und welche Sollwerte gelten. Die Sollwerte kommen aus
`protocol/rdm-digest.md` §2 (E1.20/E1.11 über Ja Rule) und `protocol/contract.md` §6.3. Die Konstanten stehen in
`core/pm_port_line.h`.

## 1. Aufbau

- **Analysator:** Raspberry Pi Pico mit [gusmanb LogicAnalyzer](https://github.com/gusmanb/logicanalyzer) (24 Kanäle,
  bis 100 MS/s, Trigger) oder sigrok-pico + PulseView (PulseView bringt einen DMX512-Decoder mit, einen RDM-Decoder
  nicht).
- **Gemessen wird nur auf der 3,3-V-Seite** (TX, DE, RX am P4). Die Pico-Eingänge vertragen keine 5 V und keine
  RS-485-Differenzpegel. Für Messungen auf der Leitung einen zweiten MAX3485 als reinen Empfänger (DE = /RE = GND)
  an A/B hängen und dessen RO messen.
- **Gemeinsames GND** zwischen Pico und Waveshare-Board.
- Kanalbelegung (Pins siehe `pm_board/WIRING.md`):

| Kanal | Signal | Kanal | Signal | Kanal | Signal |
|---|---|---|---|---|---|
| 0 | TX1 (GPIO25) | 4 | DE1 (GPIO33) | 8 | RX1 (GPIO24) |
| 1 | TX2 (GPIO27) | 5 | DE2 (GPIO26) | 9 | RX2 (GPIO47) |
| 2 | TX3 (GPIO20) | 6 | DE3 (GPIO22) | 10 | RX3 (GPIO21) |
| 3 | TX4 (GPIO5) | 7 | DE4 (GPIO3) | 11 | RX4 (GPIO4) |

- **Abtastrate:** 10 MS/s (0,1 µs Auflösung) für Break/MAB/Turnaround; 1–2 MS/s für lange Aufnahmen (Rate, Discovery
  über mehrere Sekunden); ≥ 25 MS/s für den Baudraten-Test (M15).
- **Gegenstellen:** eine Tube mit RDM-Firmware (App und Bootloader), dazu ein fremder RDM-Responder (ESP32 mit
  fremder Implementierung, siehe `docs/plan.md` Abnahme), DMX-Quelle für den Input-Test: ein anderer Port des Nodes
  über ein XLR-Kabel mit Gender Changer.

## 2. Messungen und Sollwerte

„Soll“ ist die Grenze, die nie verletzt werden darf. „Ziel“ ist der erwartete Wert unserer Implementierung.

| Nr | Was | Wie | Soll | Ziel |
|---|---|---|---|---|
| M1 | Break, MAB (Standard) | TX1 low-Phase vor dem Startcode, danach high bis zum Startbit | Break 176–352 µs, MAB 12–88 µs | Break 176 µs ± 3, MAB 16 µs ± 3 |
| M2 | Break/MAB einstellbar | PORTn_DMX_BREAK = 352, PORTn_DMX_MAB = 88, dann 200/12 | innerhalb der Bereiche, Werte außerhalb werden auf 176–352 / 12–88 begrenzt | Sollwert ± 3 µs |
| M3 | Slot-Timing | Bitbreite, Stoppbits, Lücken zwischen Slots | 4,00 µs/Bit, 2 Stoppbits, 44 µs/Slot; Inter-Slot-Zeit < 1 s (E1.11) | keine Lücken (MBS = 0). Eine Lücke > 0 bedeutet FIFO-Unterlauf (ISR-Latenz): melden |
| M4 | Rate | Break-zu-Break über 10 s | ≥ 1,204 ms (E1.11); PORTn_DMX_RATE 1–44 Hz | 40 Hz → 25,0 ms ± 0,05. 44 Hz mit 512 Slots → Rücken an Rücken ≈ 22,96 ms (≈ 43,5 Hz, mehr geht physikalisch nicht) |
| M5 | MBS (`pm_port_cfg_t.mbs_us`) | mbs_us = 8, Abstand zwischen Slots | – | **Klärt die Semantik von `tx_idle_num`** (TRM: „duration between transfers“): Lücke nach jedem Slot (dann ist MBS nutzbar) oder nur nach FIFO-Nachladen (dann Funktion entfernen). Ergebnis in diese Datei |
| M6 | 4 Ports gleichzeitig | M1/M4 auf allen 4 TX, alle Ports Output, 512 Slots | wie M1/M4 | Break-Jitter ≤ 5 µs auch bei gleichzeitigen Breaks |
| M7 | Turnaround Controller | Ende letztes Stoppbit der Anfrage (TX) → DE fällt | DE darf erst nach dem letzten Stoppbit fallen; Responder darf ab 176 µs antworten | 0–10 µs nach dem Stoppbit; nie davor (abgeschnittene Prüfsumme) |
| M8 | Antwort (Tube, GET DEVICE_INFO) | Anfrage-Ende → Antwort-Break auf RX; DE bleibt low bis Antwort-Ende | Responder-Turnaround 176 µs–2,0 ms; Antwort-Break 176–352 µs, MAB 12–88 µs | Tube-App 241–458 µs, Bootloader 227–1335 µs (Emulator-Werte, tube README). Response kommt als OK (Stat. `rdm_requests` steigt, keine `rdm_collisions`) |
| M9 | Nächstes Paket nach Antwort | Ende Antwort → nächster Break (TX) | ≥ 176 µs | 176–300 µs |
| M10 | Timeout (UID ohne Gerät) | DE-low-Phase, Abstand Anfrage-Ende → nächster Break | Antwortfenster 2,8 ms; nächstes Paket ≥ 3,0 ms nach Anfrage-Ende | DE low 2,9 ms (2,8 + 0,1 Zustellreserve), nächster Break bei 3,0–3,2 ms |
| M11 | DUB | DE-low-Phase, Antwort ohne Break auf RX, Abstand zum nächsten Paket | Fenster 2,9 ms; nächstes Paket ≥ 5,8 ms nach DUB-Ende | DE low ≥ 3,0 ms (bzw. bis 200 µs nach dem letzten Byte), nächster Break bei 5,8–6,0 ms |
| M12 | DUB-Kollision | 2–3 Tubes, volle Discovery | Kollision → Bereich wird geteilt, alle UIDs gefunden | `rdm_collisions` > 0, TOD vollständig |
| M13 | Broadcast | SET an FFFF:FFFFFFFF | keine Umschaltung, nächstes Paket ≥ 176 µs | DE bleibt high; Abstand 176–300 µs |
| M14 | DMX-Rate unter RDM-Last | volle Discovery und BOOT_CATCH (alle 20 ms GET) bei 512 Slots, 40 Hz, 10 s aufnehmen | DMX ≥ 30 Hz (`docs/plan.md`) | Break-zu-Break ≤ 33,3 ms. Ausnahme: höchstens jeder 4. Abstand länger (Schutz gegen RDM-Aushungern, `PM_PORT_RDM_STARVE_FRAMES`); Hosttest-Simulation: schlimmster Abstand 27–31 ms |
| M15 | Input | Port 1 Output → Port 2 Input (Kabel + Gender Changer); 100, 512 Slots; dann Kabel ziehen | Break ≥ 88 µs wird erkannt; „kein Signal“ nach 1 s ohne Frame | `frames_in` = gesendete Frames, Werte identisch, fps wie Sender, `PM_PORT_EV_INPUT_SIGNAL` 0 nach 1,0–1,1 s, `rx_errors` bleibt 0 (der Framing-Fehler des Breaks wird nicht gezählt – prüfen!) |
| M16 | Boot | Aufnahme ab Power-On und ab Reset-Taster, DE1–4 | Keine DE-high-Phase vor `pm_port_start` | DE durchgehend low bis zum ersten Break |
| M17 | Flash-Schreiben | während DMX: NVS-Schreibschleife bzw. OTA-Upload | Break/MAB bleiben in den Grenzen | wie M1 (IRAM-sichere ISRs, `CONFIG_PM_PORT_ISR_CACHE_SAFE`) |
| M18 | Maximale Baudrate (Matrix-Modus) | UART1 TX → RX gebrückt (ohne Transceiver), Testfirmware sendet bei 1, 2, 4, 5 Mbit/s | Bitbreite exakt, keine Fehler | 5 Mbit/s fehlerfrei (siehe Abschnitt 4) |

Zu jeder Messung die Aufnahme (`.tlac` bzw. `.sr`) unter `docs/measurements/` ablegen (Verzeichnis gehört dem
Architekten, bei Bedarf anlegen lassen) und die Ergebnisse hier eintragen.

## 3. Gezielt zu prüfende Annahmen des Treibers

Diese Punkte sind aus dem IDF-Quellcode und der Register-Beschreibung abgeleitet und können sich auf dem Chip anders
verhalten:

1. **`err_wr_mask`** (`uart_ll_discard_error_data`): Bytes mit Framing-Fehler, auch das NUL-Zeichen des Breaks,
   landen nicht im RX-FIFO. Falls doch, steht vor jedem Input-Frame ein zusätzliches 0x00 (M15 zeigt dann 514 statt
   513 Bytes bzw. verschobene Werte).
2. **BRK_DET und FRAM_ERR** kommen im selben Interrupt. Nur dann maskiert die ISR den Framing-Fehler des Breaks
   (sonst steigt `rx_errors` mit jedem Frame und Input-Frames werden verworfen, M15).
3. **TX_DONE** kommt erst nach dem letzten Stoppbit oder wird durch die Warteschleife auf `uart_ll_is_tx_idle()`
   abgefangen (M7).
4. **Break per TXD-Inversion**: Die Umschaltung über `uart_ll_inverse_signal` wirkt sofort (Sync-Register,
   `uart_ll_update`). Die Flanken liegen dann genau auf den Timer-Deadlines (M1).
5. **`tx_idle_num`** und MBS (M5).
6. **ISR-Latenz** auf Kern 1: gptimer-Alarm → Flanke. Die Ziele in M1/M7 setzen ≤ 3 µs voraus.

## 4. Maximale UART-Baudrate des P4 (für den Modus „Pixelprotokoll“)

- `soc_caps.h` (IDF v5.5.6, esp32p4): `SOC_UART_BITRATE_MAX = 5000000`, also **5 Mbit/s**. Die HP-UARTs takten aus
  PLL_F80M (Standard), XTAL 40 MHz oder RC_FAST. Bei 80 MHz und 16-fachem Oversampling ergeben sich genau 5 Mbit/s.
- `SOC_UART_HP_NUM = 5` (UART0–4) und ein LP-UART. UART0 ist die Konsole, für die 4 Ports bleiben **UART1–4**.
  FIFO: 128 Byte pro Richtung.
- Transceiver: MAX3485/SP3485 bis 10 Mbit/s, THVD1450 bis 50 Mbit/s. Der Engpass ist die UART mit 5 Mbit/s.
- Einordnung: 5 Mbit/s mit 8N1 entsprechen 500 kB/s pro Port. Das sind etwa 166 000 RGB-Pixel pro Sekunde, eine
  32×32-Matrix also mit gut 150 fps oder 6 Matrizen mit 25 fps. Das ist das Zwanzigfache von DMX (250 kbit/s 8N2).
- Bei 5 Mbit/s dauert ein FIFO-Inhalt nur 256 µs; dann braucht es DMA (UHCI/GDMA) statt FIFO-ISR. Das gehört zum
  späteren Matrix-Modus. `PM_PORT_MODE_PIXEL` ist im Enum vorgesehen, `pm_port_configure` antwortet darauf mit
  `ESP_ERR_NOT_SUPPORTED`.
- Auf Hardware bestätigen: M18.

## 5. Designnotizen (warum der Treiber so aussieht)

- **RS-485-Modus der UART:** Der P4 hat einen Halbduplex-Modus (`uart_ll_set_mode_rs485_half_duplex`). In diesem
  Modus setzt die Hardware RTS, solange die UART sendet. Dazu gibt es Verzögerungen (dl0/dl1, `rs485_tx_dly_num`)
  und eine Kollisionserkennung (`RS485_CLASH`, nur im Collision-Detect-Modus). Er wird **nicht** genutzt:
  - Der Break entsteht durch TXD-Inversion außerhalb einer Übertragung, RTS wäre dabei nicht gesetzt.
  - IDFs eigener RS-485-Pfad misstraut TX_DONE (prüft zusätzlich `is_tx_idle`).
  - `uart_set_mode()` setzt den installierten IDF-Treiber voraus, den wir nicht verwenden.
  DE ist deshalb ein normaler GPIO und wird von der Zustandsmaschine geschaltet. Das ist auch die Grundlage für die
  exakte Umschaltung bei RDM.
- **Hardware-Break (`txd_brk`, `tx_brk_num`):** Die UART kann einen Break senden, aber nur *nach* den Daten und in
  Bitzeiten (4 µs). Er wird nicht genutzt; die TXD-Inversion mit Timer ist µs-genau.
- **Break-Erkennung beim Empfang:** Interrupt `BRK_DET` („receiver detects a 0 after the stop bit“). Die ISR liest
  erst den FIFO (Bytes vor dem Break gehören zum alten Paket) und meldet dann den Break.
- **Timer:** ein gptimer mit 1 MHz für alle 4 Ports; der Alarm steht immer auf der frühesten Deadline. ISRs und Task
  laufen auf Kern 1 unter einem Spinlock. Kern 0 sieht nur Queue und Dreifachpuffer.
- **RX-Schwelle:** 1 Byte im RDM-Antwortfenster (DUB-Ende und Inter-Slot-Timeout brauchen die Byte-Zeitpunkte),
  16 Byte beim DMX-Input. Die RDM-Deadlines enthalten 100 µs Zustellreserve (`PM_PORT_RX_LATENCY_US`).
- **Was auf dem Rechner getestet ist** (`test_host/test_pm_port.c`, Simulator mit 44 µs pro Slot und Responder-
  Skripten): Break-, MAB- und Rate-Timing, Delta-Modus, RDM-Anfrage mit Antwort, Timeout, späte und abgebrochene
  Antworten, Broadcast, DUB mit Antwort, Kollision und leerem Zweig, DMX-Rate unter Dauer-RDM-Last, Moduswechsel
  mit laufender Transaktion, Input (Startcode ≠ 0, 513-Byte-Frames, Framing-Fehler), Dreifachpuffer, Revisionscode.
  **Nicht auf dem Rechner testbar:** alles in `src/pm_port.c` (UART-LL, ISR, gptimer, Task, Queues) und damit alles
  aus Abschnitt 3.

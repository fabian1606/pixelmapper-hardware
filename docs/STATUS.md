# Pixelmapper Node – Stand

Stand: 2026-10-26, nach Phase 2 (Integration, virtueller Node, Ende-zu-Ende-Test, Review). Auf Hardware ist **noch
nichts** gelaufen. Alles unten unter „geprüft“ bezieht sich auf Host-Tests, den IDF-Build und den virtuellen Node.

## 1. Was ist fertig

| Teil | Stand |
|---|---|
| Protokoll (`protocol/`) | Vertrag, Digests, `pids.toml` (42 PIDs inkl. PORTn_DMX_MBS), Generator für C und Rust inkl. Statusnamen, Image-Format, Vektoren |
| `pm_protocol` | UID, CRC, Settings (Defaults/Sanitize/Diff), Image-Beschreibungsblock, Art-Net-Ersatz-IP |
| `pm_artnet` | Art-Net-4-Node: Poll/PollReply (4 BindIndex-Seiten), DMX mit Merge/Sync/Failsafe, ArtAddress, ArtIpProg (Command 0 = Abfrage), ArtInput, TOD, ArtRdm mit FifoAvail/FifoMax, ArtFirmwareMaster (.alf), Input-Unicast, Diag, Port-Snapshot für die UI |
| `pm_rdm` | Codec, Controller (`pm_rdm_ctrl`: Discovery, Hintergrundsuche 3× UN_MUTE + Präsenz-Mute + DUB, Queue, BOOT_CATCH, Statistik), Responder des Nodes (Standard-, E1.37-2- und Hersteller-PIDs, Queued Messages, Statusmeldungen, FW_*) |
| `pm_node` | Pure-C-Glue zwischen pm_artnet, Responder, Port, Config, UI: RDM-Routing, Token-Bit 31, Event-Dispatch, Settings-Fan-out (maskiert, atomar), Identify-Spiegelung, Tube-Update-Anzeige |
| `pm_port` / `pm_board` | Port-Engine auf Kern 1 (DMX/RDM-Leitungs-FSM, Zeitbudget für Discovery, Input), Pin-Tabelle Rev 0 mit Doppelbelegungs-Prüfung |
| `pm_net`, `pm_config`, `pm_ota` | Ethernet (RMII-Pins aus pm_board, IP_PKTINFO, DHCP/Fallback), NVS mit Migration und `pm_config_modify`, OTA-Sink mit Rollback |
| `pm_ui` | Front-Panel nach UI-Konzept, Host-Renderer |
| `main` | dünner ESP-IDF-Adapter um pm_node; Image wird im Netzwerk-Task nach 30 s als gültig markiert |
| `crates/pixelmapper-net` | Controller-Bibliothek + `pmnet` (Poll, TOD, RDM, Node-Config, ip_prog_query/change, Tube- und Node-Flash, BOOT_CATCH), Sim-Node |
| `crates/pmnet-bridge` | Pixelmapper-Bridge der Bridge-Session (eigenes Binary `pmnet-bridge serve`), unverändert gemergt |
| Virtueller Node | `node/firmware/test_host/virtual_node` (echter Code + Fake-Port über simulierten RS-485-Bus `pm_sim`, Fake-Config, Fake-Sink) |

## 2. Was ist auf dem Host bzw. im Ende-zu-Ende-Test geprüft

Befehle: siehe CLAUDE.md. Zahlen vom 2026-10-26 auf `main`:

- `python3 protocol/gen.py --check`: ok
- ctest (`node/firmware/test_host`): 6/6 – `pm_tests` 23 301 Checks (Protokoll, Art-Net, RDM, Responder, pm_node,
  Vektoren), `pm_port` 364 Checks (Leitungs-FSM im Simulator, Zeitbudget, Pins), `pm_net_tests` 384 Checks (IP-Logik,
  NVS-Store, OTA-Session), `pm_fuzz` (Zufalls-/Mutationstests aller Netzwerk-Parser unter ASan/UBSan),
  `pm_ui_render` (alle UI-Szenen), `virtual_node_smoke`
- Rust-Workspace: build, 213 Tests, clippy `-D warnings`, fmt – grün (die 18 E2E-Tests kehren ohne `PM_VIRTUAL_NODE`
  sofort zurück und zählen dann mit)
- `tools/e2e.sh`: 18/18 – pixelmapper-net gegen den virtuellen Node über UDP: Discovery (4 BindIndex), ArtAddress,
  ArtIpProg (Abfrage und Änderung), TOD inkl. mehrerer Blöcke und ungefragter Daten nach Node-Neustart, RDM GET/SET
  an Tubes und Node, describe_device/PARAMETER_DESCRIPTION, Queued Messages + Last-Writer-Wins mit zwei Controllern,
  DMX mit HTP/LTP-Merge, Sync, Failsafe, Tube-Flash mit BOOT_CATCH-Rettung, Node-Flash per .alf und FW_* inkl.
  Erstblock-Fehler, Input-Unicast, zwei Differenztests Firmware ↔ `sim::node`. Gefundene Abweichungen (15) und ihre
  Korrekturen: [e2e-findings.md](e2e-findings.md)
- `idf.py build` (esp32p4, IDF v5.5.6): ohne Warnungen, Image 0xB7F40 B (82 % der App-Partition frei), DIRAM
  207 KB belegt / 448 KB frei

**Grenzen dieser Prüfung:** Der RS-485-Bus und die Tubes sind in C und Rust simuliert (kein Differenztest Tube ↔
Tube). Alles in `pm_port/src/pm_port.c` (UART-LL, ISR, gptimer), `pm_net.c`, `pm_ota.c`, der NVS-/Lock-Teil von
`pm_config.c`, der ST7735-Treiber und `main.c` laufen nur unter ESP-IDF und sind nur gebaut, nicht ausgeführt.

## 3. Auf Hardware zu prüfen (Checkliste)

Board und Inbetriebnahme
- [ ] **Chip-Revision** (`esptool.py chip_id`): bei < v3.0 `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` lokal setzen (CLAUDE.md); Bericht port: vermutlich v1.3
- [ ] **Type-C-Buchse an UART0?** Die Konsole läuft nur noch über UART0 (`CONFIG_ESP_CONSOLE_SECONDARY_NONE`), weil GPIO 24/25 (USB-Serial-JTAG) Port-1-Pins sind. Hängt die Buchse an USB-JTAG, gibt es keine Konsole und kein Flashen über diese Buchse
- [ ] **Strombedarf 3V3**: Board + 4× MAX3485 + Display-Backlight gegen den 3V3-Regler des Boards (Prototyp über USB)
- [ ] Pinbelegung Rev 0 gegen Schaltplan (`pm_board/WIRING.md`), insbesondere RMII (RXD0 = 29, RXD1 = 30) und die alternativen EMAC-Pins 46/47/33
- [ ] Ethernet: Link, DHCP, Fallback 2.B.C.D nach 5 s, IP-Wechsel per ArtIpProg und E1.37-2
- [ ] PSRAM-.bss (`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`): UI läuft stabil

Port-Engine (Messplan `pm_port/MEASURE.md`, Pico als Logikanalysator)
- [ ] M1–M4, M6: Break/MAB/Slot-Timing/Rate, 4 Ports gleichzeitig
- [ ] **M5: Semantik von `tx_idle_num`** – davon hängt ab, ob PORTn_DMX_MBS bleibt oder entfernt wird
- [ ] M7–M13: RDM-Turnaround, Antwortfenster 2,8 ms, DUB 2,9/5,8 ms, Kollision, Broadcast
- [ ] M14: DMX ≥ 30 Hz unter Discovery + BOOT_CATCH (Host-Simulation: größter Abstand 30,6 ms); tatsächlicher BOOT_CATCH-Takt (nominal 20 ms, durch das Zeitbudget gebremst)
- [ ] M15: Input inkl. „kein Signal“ nach 1 s; M16: kein DE-high beim Booten; M17: Timing während NVS/OTA-Schreiben; M18: 5 Mbit/s
- [ ] MEASURE.md §3, Treiber-Annahmen: (1) `err_wr_mask` verwirft das Break-NUL, (2) BRK_DET und FRAM_ERR im selben Interrupt, (3) TX_DONE nach dem letzten Stoppbit (Busy-Wait bis 64 µs im ISR), (4) Break per TXD-Inversion wirkt sofort, (5) `tx_idle_num`, (6) ISR-Latenz ≤ 3 µs; dazu BRK_DET-Zeitpunkt relativ zum 2,8-ms-Fenster (Modell: ~44 µs nach Break-Beginn)

Front-Panel
- [ ] **Display-Lage** (quer eingebaut, Rotation/Offsets des ST7735S), SWRESET ohne Reset-Pin
- [ ] **Encoder-Rastung** (Schritte pro Raste, Beschleunigung), BACK kurz/lang, Werksreset-Kombination beim Einschalten
- [ ] LED Mute schaltet das Display aus, Locate/Identify-Anzeige

Firmware-Update und Abnahme
- [ ] OTA per ArtFirmwareMaster (DMX-Workshop) und per FW_*-PIDs (`pmnet`), Rollback ohne mark_valid, Update im Pending-Verify-Fenster
- [ ] BOOT_SOFTWARE_VERSION_ID/LABEL aus `esp_ota_get_bootloader_description` (nur gebaut)
- [ ] Abnahme nach `docs/plan.md`: DMX-Workshop, OLA (Controller + `rdm_responder_test.py`), fremder RDM-Responder, 24-h-Dauertest
- [ ] Tube-Update und BOOT_CATCH-Rettung mit einer echten Tube (Bootloader-Fenster 26–225 ms)

## 4. Offene Punkte

- **UI, Wettlauf beim Bearbeiten** (Review, niedrig): Der generische Editor des Panels (`settings_set`) schreibt die
  ganze Settings-Struktur zurück; eine gleichzeitige Remote-Änderung im Fenster von wenigen µs kann verloren gehen.
  Werksreset und Szenen-Aufnahme sind bereits atomar. Lösung: maskiertes Schreiben (`pm_config_modify` +
  `pm_settings_diff`) in der ui_core-Schnittstelle.
- **UI-Fragen an den Nutzer** (unbeantwortet, nichts umgesetzt): Geräteliste nach DMX-Adresse sortieren? Encoder-Druck
  auf Home öffnet das Hauptmenü? Vorgemerkte Netzwerkwerte bei Remote-Änderung stehen lassen? Wo wird „Sync“ angezeigt
  (UI-Konzept hat keinen Platz)? „Gateway“ wird bei langen IPs zu „Gatewa“ abgeschnitten.
- `pixelmapper-net`: empfangenes ArtDmx (Input-Ports) wird nicht als Event weitergegeben (E2E prüft per Roh-Socket);
  Neustart-Erkennung über den NodeReport-Zähler versagt, wenn der Zähler vor dem Neustart ≥ 9900 war; der
  ArtTodData-Decoder akzeptiert UidCount > 200 (nachsichtig).
- Die Bridge erwartet bei ip_prog/art_address/art_input „1 s, ein Resend“; `Controller` sendet nicht erneut – das
  muss der Adapter `PixelmapperNetBackend` der Bridge-Session übernehmen.
- Full/Scene-Failsafe greift auch 4 s nach dem Start ohne je empfangene Daten (Standard Hold, daher unkritisch).
- `s_port` in pm_port liegt mit ~67 KB in .data (internes RAM) – Kandidat, falls RAM knapp wird.
- ESTA-Normtexte (E1.20-2025, E1.37-1/-2) fehlen weiter; Digest-Punkte mit [ungeprüft] vor der Abnahme prüfen.

## 5. Nächste Schritte

1. Hardware Rev 0 aufbauen, Checkliste §3 von oben nach unten (Chip-Revision, Konsole, Strom, Pins zuerst).
2. Messplan MEASURE.md abarbeiten, Ergebnisse dort eintragen; MBS-Entscheidung nach M5.
3. Abnahmetests nach `docs/plan.md` (DMX-Workshop, OLA, 24-h-Test).
4. Bridge-Session: Adapter `PixelmapperNetBackend` gegen die jetzige pixelmapper-net-API (ip_prog_query/change,
   set_poll_targets, try_send_dmx/try_send_sync sind neu).
5. UI-Fragen klären, maskiertes Schreiben im Panel-Editor.
6. Danach Plan-Schritt 11: Matrix-Modus, eigene Platine Rev 1.

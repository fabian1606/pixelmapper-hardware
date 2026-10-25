# Ende-zu-Ende-Test: Abweichungen Rust ↔ C ↔ Vertrag

Stand: 2026-10-25 (Phase 3, Branch `phase3/e2e`). Test: `tools/e2e.sh` = `pixelmapper-net`
(`crates/pixelmapper-net/tests/e2e_virtual_node.rs`) gegen den virtuellen Node (`node/firmware/test_host/virtual_node`:
echtes `pm_node`/`pm_artnet`/`pm_rdm` + simulierte RS-485-Leitung, RAM-Konfiguration, `pm_ota_session` auf RAM).
Zwei Differenztests vergleichen außerdem Antwort für Antwort Firmware und `sim::node` (Rusts Node-Modell).

Entschieden wurde nach den Digests (höchste Instanz), dann `contract.md`; wo beide schweigen, gilt die Firmware
(sie läuft auf dem Gerät) und der Vertrag wurde präzisiert (§12.3).

| # | Symptom | Ursache | Korrektur (Ort) | Commit |
|---|---|---|---|---|
| 1 | Nach einem Neustart des Nodes (RESET_DEVICE, Strom) keine ungefragten ArtTodData mehr | Node vergisst die ArtTodRequest-Absender; `pixelmapper-net` forderte die TODs nur bei neuer Firmware-Version neu an (Vertrag §6.1 verlangt es nach jedem Neustart) | Rust `controller`: Neustart am NodeReport-Zähler erkannt (`state::report_counter*`, Unit-Test); Vertrag §3/§6.1 | 35e4e12, ea46e4b |
| 2 | ArtSync bei Merge zweier Physicals derselben IP: Firmware übernimmt, `sim::node` ignoriert | Spec S. 68 / digest §3.3: nur Streams **verschiedener IPs** blockieren; Vertrag §4 sagte „solange er mergt“ | `sim::node` (`merging_ips`, Test angepasst); Vertrag §4 | acce74e |
| 3 | BOOT_SOFTWARE_VERSION_ID/LABEL der Firmware 0 / leer | `main.c` füllte `boot_sw_version_*` der Responder-Identität nicht | Firmware: `main.c` liest `esp_ota_get_bootloader_description`, `pm_node_boot_version()` (Host-Test `test_node`) | 4a59cd9 |
| 4 | IPV4_STATIC_ADDRESS ohne gesetzte Adresse: Firmware 0.0.0.0, `sim::node` 2.B.C.D | Responder gab den Rohwert (0 = „Ersatzadresse“) aus | Firmware: Responder meldet `pm_ip_artnet_fallback()` (neu in `pm_protocol`, `pm_net` nutzt es; Host-Test `test_rdm_responder`); Vertrag §6.3 | 4a59cd9 |
| 5 | ArtRdm FifoMax 4 (`sim::node`) statt 8 | Sim-Standard ≠ Vertrag §6.2 (`PM_RDM_QUEUE_LEN`) | `sim::node` `SimConfig::fifo_max = 8` | ea46e4b |
| 6 | Failsafe-Modus: `sim::node` knotenweit, Firmware pro Port (Status3 je BindIndex) | Vertrag §5: ArtAddress „adressiert über BindIndex = Port“ | `sim::node` pro Port; Vertrag §12.3 | ea46e4b |
| 7 | ArtTodData für Port ohne RDM: `sim::node` TodFull mit 0 UIDs, Firmware TodNak | Vertrag „TOD leer“ unscharf; Spec: TodNak = TOD nicht verfügbar | `sim::node` TodNak; Vertrag §6.1 | ea46e4b |
| 8 | ArtPollReply: `sim::node` setzt SwIn auch bei Output-Ports | Vertrag §3 „SwOut bzw. SwIn“ | `sim::node`; Vertrag §3 | ea46e4b |
| 9 | ArtPollReply eines Input-Ports: `sim::node` meldet RDM an (GoodOutputB Bit 7 = 0) | Input-Port hat kein RDM (Vertrag §6.1) | `sim::node` | ea46e4b |
| 10 | AcnPriority 100 und aus ArtAddress übernommen (`sim::node`), Firmware 0 | kein sACN (Vertrag §1); Vertrag schwieg | `sim::node` 0, ArtAddress-AcnPriority ignoriert; Vertrag §3 | ea46e4b |
| 11 | ArtIpProgReply ProgPort 6454 (`sim::node`) statt 0 | Feld deprecated (digest §4.4) | `sim::node` 0 | ea46e4b |
| 12 | MANUFACTURER_LABEL „Pixelmapper“ (`sim::node`) statt „Prototype“ | Vertrag schwieg | `sim::node`; Vertrag §6.3 | ea46e4b |
| 13 | IPV4_DEFAULT_ROUTE ohne Gateway: `sim::node` Interface 1, Firmware 0 | digest §6.5: Interface 0 = keine Route | `sim::node`; Vertrag §6.3 | ea46e4b |
| 14 | FW_WRITE mit Lücke bzw. FW_WRITE/FW_FINISH ohne Sitzung: `sim::node` setzt FW_INFO.last_error = SEQUENCE, Firmware nicht | Responder prüft vor der Sink (`h_fw_write`), `pm_ota_session` ohne Sitzung unverändert | `sim::node` (Test angepasst); Vertrag §9.2 | ea46e4b |
| 15 | NodeReport nach gescheitertem Update: Firmware `#000e … Firmware update failed`, `sim::node` unverändert | `pm_node_fw_progress` setzt RcFirmwareFail | `sim::node`; Vertrag §3 | ea46e4b |

Keine Abweichung gefunden (Firmware und `pixelmapper-net` stimmen überein): Discovery (4 BindIndex-Seiten, alle
Felder aus §3), ArtAddress (Namen inkl. Kürzung, Port-Address/Net, Merge, Richtung, RDM, Output-Style, Failsafe +
Szene, Locate ↔ IDENTIFY_DEVICE, AcClearOp), ArtInput, ArtIpProg (Abfrage, Änderung, ungültige Werte, Reset), TOD
(mehrere Blöcke, ungefragt nur an Anforderer, AtcFlush/End/IncOn/IncOff), RDM (Tubes, Node über jede Port-Address,
Broadcast an Node + Leitung, FifoAvail bei 20 parallelen Anfragen, NACK-Gründe, PARAMETER_DESCRIPTION aller Node-PIDs
= `pids.toml`), Queued Messages (Inhalt und Reihenfolge identisch mit `sim::node`), Last-Writer-Wins, DMX (HTP/LTP,
dritte Quelle, ArtSync inkl. Unicast-Verbot und 4-s-Timeout, Failsafe nach 4 s, ungefragtes Reply bei Datenverlust),
Tube-Flash (inkl. NACK BUFFER_FULL durch ArtRdm) und BOOT_CATCH-Rettung, Node-Flash per RDM und `.alf` inkl. Fehler im
ersten Block, Input (Unicast an Poller und Abonnenten, kein Broadcast, Keepalive), E1.37-2.

Testhilfe, keine Abweichung: drei Testfehler waren Rennen im Test selbst (TOD-Ereignisse der Start-Discovery,
Tube-Zustand vor der Leitungs-Transaktion, Abschluss-ArtTodData der Discovery) und wurden im Test behoben.

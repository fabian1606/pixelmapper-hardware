# RDM-Digest (ANSI E1.20, E1.37-1, E1.37-2, kurz E1.37-5)

Arbeitsgrundlage für den ESP32-P4-Node: RDM-Controller auf 4 DMX-Ports und selbst RDM-Responder (über ArtRdm
erreichbar), dazu der Rust-Host-Crate. Testvektoren liegen in [`vectors/rdm/`](vectors/rdm/).

## 0. Quellen und Kennzeichnung

Jede Aussage trägt ein Quellen-Tag. Die ESTA-Normtexte selbst lagen **nicht** vor, deshalb ist kein Tag
„[E1.20 §x]“ aus eigener Lektüre. Paragraphen- und Tabellennummern stammen aus Zitaten in Code, der die Norm
umsetzt. Sie stehen als „via“ im Tag, z. B. [Ja Rule ← E1.20 Tab. 3-2].

| Tag | Quelle (heruntergeladen nach `scratchpad/rdm-src/`, Stand 2026-10-24) |
|---|---|
| [OLA RDMEnums.h] | `ola/include/ola/rdm/RDMEnums.h`: PID-, NACK-, Status-, Einheiten- und Präfixwerte |
| [OLA pids.proto] | `ola/data/rdm/pids.proto`: PDL-Layouts aller PIDs, Sub-Device-Bereiche |
| [OLA DiscoveryAgent] | `ola/common/rdm/DiscoveryAgent.cpp` und `.h` |
| [OLA RDMCommand.cpp] | `ola/common/rdm/RDMCommand.cpp`: Rahmenprüfung der Antworten |
| [OLA RDMPacket.h] | `ola/include/ola/rdm/RDMPacket.h`: START_CODE 0xCC, SUB_START_CODE 0x01 |
| [OLA ResponderTest] | `ola/tools/rdm/TestDefinitions.py`, `TestMixins.py`, `ResponderTest.py` (rdm_responder_test) |
| [OLA AdvDimmer] | `ola/common/rdm/AdvancedDimmerResponder.cpp`: Referenz für LOCK_PIN/LOCK_STATE |
| [OLA AckTimer] | `ola/common/rdm/AckTimerResponder.cpp`: Referenz für QUEUED_MESSAGE |
| [OLA RDMConstants.py] | `ola/python/ola/RDMConstants.py`: PID-Bereiche |
| [Ja Rule] | `OpenLightingProject/ja-rule/firmware/src/transceiver_timing.h`, `constants.h`, `rdm.h`, `rdm_responder.c`: Timing mit Tabellenverweis auf E1.20-2010 und E1.11-2008 |
| [tube rdm.c], [tube boot], [tube rdm_boot.h], [tube README], [tube config.h], [flash_rdm.py] | getestete Tube-Implementierung im Repo `ledTubeXBAR500` |
| [ESTA TSP] | `tsp.esta.org/tsp/documents/published_docs.php` (Indexseite) |
| [ungeprüft] | aus dem Gedächtnis, durch keine der Quellen oben belegt; vor Gebrauch an der Norm prüfen |

**ESTA-Normen:** Die TSP-Indexseite ist erreichbar und listet E1.20-2025, E1.20-2010, E1.37-1-2012 (R2022),
E1.37-2-2015 (R2021), E1.37-5-2024, E1.37-7-2019 und E1.11-2024 [ESTA TSP]. Der Download (`downloaddoc.php`)
verlangt eine Anmeldung mit E-Mail, Name, Firma und Telefon. Dieses Formular wurde nicht ausgefüllt, weil keine
persönlichen Daten an Dritte gehen sollten. Die PDFs lagen also nicht vor. **Todo:** Ein Mensch lädt E1.20-2025,
E1.37-1, E1.37-2 und E1.37-5 herunter und prüft die [ungeprüft]-Stellen.

---

## 1. Paketformat

Startcode `0xCC`, Sub-Startcode `0x01` [OLA RDMPacket.h]. Alle Mehrbytewerte sind big-endian [tube rdm.c],
[OLA pids.proto]. Die maximale PDL ist 231 [OLA RDMConstants.py `RDM_MAX_PARAM_DATA_LENGTH`],
[Ja Rule rdm.h `MAX_PARAM_DATA_SIZE`]. Ein Rahmen ist damit höchstens 257 Byte lang
[Ja Rule rdm.h `RDM_MAX_FRAME_SIZE`].

| Offset | Länge | Feld | Bemerkung |
|---|---|---|---|
| 0 | 1 | START Code | 0xCC |
| 1 | 1 | Sub-START Code | 0x01. Ein Rahmen mit anderem Wert wird verworfen [tube rdm.c], [OLA RDMCommand.cpp] |
| 2 | 1 | Message Length | = 24 + PDL. Zählt vom Startcode bis zum Ende der PD, **ohne** Prüfsumme [OLA RDMCommand.cpp], [tube rdm.c] |
| 3 | 6 | Destination UID | Hersteller-ID (2) + Geräte-ID (4) |
| 9 | 6 | Source UID | |
| 15 | 1 | Transaction Number (TN) | Der Controller zählt pro Anfrage hoch. Der Responder spiegelt die TN. OLA prüft die Gleichheit (`RDM_TRANSACTION_MISMATCH`) [OLA RDMCommand.cpp] |
| 16 | 1 | Port ID / Response Type | Anfrage: Port-ID des Controllers, 1..n [ungeprüft: Port-ID ≥ 1]. Antwort: Response Type (§4) |
| 17 | 1 | Message Count | In Anfragen 0 [ungeprüft]. In Antworten die Zahl der Queued Messages, 0..255 [OLA RDMEnums.h `MAX_QUEUED_MESSAGE_COUNT`] |
| 18 | 2 | Sub-Device | 0 = Root (§5) |
| 20 | 1 | Command Class | 0x10 DISCOVERY, 0x11 DISCOVERY_RESPONSE, 0x20 GET, 0x21 GET_RESPONSE, 0x30 SET, 0x31 SET_RESPONSE [OLA RDMEnums.h] |
| 21 | 2 | Parameter ID (PID) | |
| 23 | 1 | PDL | 0..231 |
| 24 | PDL | Parameter Data | |
| 24+PDL | 2 | Checksum | 16-Bit-Summe aller Bytes von Offset 0 bis 23+PDL, Startcode eingeschlossen, modulo 2^16, big-endian [OLA RDMCommand.cpp `CalculateChecksum` beginnt mit START_CODE], [tube rdm.c] |

Antwortregeln:
- Der Responder setzt Dest = Source der Anfrage und Source = eigene UID, spiegelt TN, Sub-Device und PID und setzt
  CC = Anfrage-CC + 1 [tube rdm.c].
- OLA prüft an jeder Antwort Dest-UID, Source-UID, TN, Sub-Device und CC. Ausnahmen gelten für Sub-Device 0xFFFF und
  für QUEUED_MESSAGE [OLA RDMCommand.cpp]. Ein Response Type > 3 ist ungültig [OLA RDMCommand.cpp].
- Eine Anfrage mit falscher Prüfsumme, falscher Länge (`PDL + 24 != Message Length`) oder falschem Sub-Startcode
  wird ohne Antwort verworfen [tube rdm.c], [tube README „E1.20-Konformität“].
- Der Responder muss jede zulässige Anfragegröße annehmen (PDL bis 231). Ist die PDL für die PID falsch, antwortet er
  NACK FORMAT_ERROR. OLA testet das mit `GetMaxPacketSize`. Erlaubt sind dort FORMAT_ERROR,
  PACKET_SIZE_UNSUPPORTED oder ACK, ein Timeout ergibt eine Advisory [OLA ResponderTest].
  Die Tube-App nimmt die volle Länge an [tube README, „6.2.10.1“].

---

## 2. Timing

Werte aus Ja Rule (Open Lighting Embedded) mit dessen Tabellenangaben aus E1.20-2010 und E1.11-2008.

| Größe | Wert | Quelle |
|---|---|---|
| Controller sendet Break | Ja-Rule-Default 176 µs. Zulässiger Bereich wohl 176–352 µs | [Ja Rule constants.h `DEFAULT_BREAK_TIME`]; Bereich [ungeprüft] |
| Controller sendet MAB | Ja-Rule-Default 12 µs. Zulässiger Bereich wohl 12–88 µs | [Ja Rule constants.h `DEFAULT_MARK_TIME`]; Bereich [ungeprüft] |
| Responder empfängt Break | ≥ 88 µs, ≤ 1 s | [Ja Rule ← E1.20 Tab. 3-3 Z. 1] |
| Responder empfängt MAB | ≥ 8 µs, ≤ 1 s | [Ja Rule ← E1.20 Tab. 3-3 Z. 1] |
| Inter-Slot-Zeit RDM (Empfang beim Responder und beim Controller) | ≤ 2,1 ms (Timeout) | [Ja Rule ← Tab. 3-3 Z. 1, Tab. 3-1 Z. 2] |
| **Responder-Turnaround** (Ende Anfrage → Beginn Antwort) | **176 µs … 2,0 ms** | [Ja Rule ← E1.20 Tab. 3-4 `MINIMUM/MAXIMUM_RESPONDER_DELAY`], [tube config.h `RDM_TURNAROUND_US 180`] |
| Antwort-Break des Responders | 176–352 µs | [tube config.h/README] |
| Antwort-MAB des Responders | 12–88 µs | [tube config.h/README] |
| Controller empfängt Antwort-Break | 88–352 µs (Ja Rule akzeptiert ab 88 µs) | [Ja Rule ← Tab. 3-1 Z. 2] |
| Controller empfängt Antwort-MAB | ≤ 88 µs | [Ja Rule ← Tab. 3-1 Z. 2] |
| **Controller-Timeout für eine Antwort** | **2,8 ms** (2 ms Responder + bis 704 µs Inline-Proxy-Verzögerung) | [Ja Rule constants.h ← Tab. 3-2 Z. 1 und 3] |
| DUB-Antwort: maximale Dauer bzw. Fenster | 2,9 ms | [Ja Rule `DEFAULT_RDM_DUB_RESPONSE_LIMIT` ← Tab. 3-3 Z. 3]. Ob Dauer oder Fenster gemeint ist: [ungeprüft] |
| **Pause nach DUB** bis zum nächsten Paket | **5,8 ms** | [Ja Rule `CONTROLLER_DUB_BACKOFF` ← Tab. 3-2 Z. 2] |
| Pause nach ausgebliebener Antwort | 3,0 ms | [Ja Rule ← Tab. 3-2 Z. 5] |
| Pause nach Broadcast-Anfrage | 176 µs (Ja Rule rundet auf 0,2 ms). Ja Rule lauscht trotzdem 2,8 ms, um fehlerhafte Responder zu erkennen | [Ja Rule ← Tab. 3-2 Z. 6; constants.h `DEFAULT_RDM_BROADCAST_TIMEOUT`] |
| Pause nach Nicht-RDM-Paket (DMX) vor der nächsten RDM-Anfrage | 176 µs | [Ja Rule ← Tab. 3-2 Z. 7] |
| „Responder Packet Time“ (Maximaldauer des ganzen Antwortpakets) | nicht belegt | [ungeprüft] |
| DMX (E1.11) Break-to-Break | ≥ 1,204 ms | [Ja Rule ← E1.11-2008 Tab. 6] |
| DMX Break senden | ≥ 92 µs (DMX512/1990: 88 µs) | [Ja Rule-Kommentar], [ungeprüft für den genauen E1.11-Tabellenwert] |
| DMX Break empfangen | ≥ 88 µs | [Ja Rule Tab. 3-3], [ungeprüft E1.11] |
| DMX MAB senden / empfangen | ≥ 12 µs / ≥ 8 µs | [ungeprüft] |
| DMX Inter-Slot-Timeout beim Empfang | 1 s | [Ja Rule ← E1.11-2008 Tab. 6] |

Die Tube hat diese Werte im zyklusgenauen Emulator gemessen: Turnaround 241–458 µs (App) bzw. 227–1335 µs
(Bootloader), Break 200 µs, MAB 21 µs [tube README]. Für den Node folgt daraus pro Port ein Antwortfenster von
2,8 ms, nach jedem DUB 5,8 ms Pause und nach Broadcasts ≥ 176 µs.

---

## 3. Discovery

### 3.1 DISC_UNIQUE_BRANCH (0x0001)
- Anfrage: CC 0x10 an Broadcast FFFF:FFFFFFFF, Sub-Device 0, PD = Lower-Bound-UID (6) + Upper-Bound-UID (6),
  also PDL 12 [OLA pids.proto], [tube rdm.c].
  OLA testet außerdem DUB an den eigenen Vendorcast und an die eigene Unicast-UID (`DUBPositiveVendorcast`,
  `DUBPositiveUnicast`) [OLA ResponderTest].
- Der Responder antwortet nur, wenn er nicht gemutet ist und `lower ≤ UID ≤ upper` gilt. Der Vergleich ist
  **vorzeichenlos** über 48 Bit [tube rdm.c `memcmp`], [OLA ResponderTest `DUBSignedComparisons`].
  Bei invertiertem Bereich (lower > upper) antwortet er nicht [OLA ResponderTest `DUBInverted*`].
- Antwort (Vektor `DUB_RESPONSE.json`): **ohne Break und ohne Startcode** [tube rdm.c `rs485_tx(r, 24, 0)`],
  [OLA DiscoveryAgent „raw response, excluding the start code“].

| Byte | Inhalt |
|---|---|
| 0–6 | Präambel 7 × 0xFE. Der Controller muss 0–7 Präambelbytes akzeptieren [OLA DiscoveryAgent `PREAMBLE_SIZE = 8` inkl. Separator; die Schleife erlaubt bis 7 × FE] |
| 7 | Separator 0xAA |
| 8–19 | EUID: jedes UID-Byte b als Paar (b \| 0xAA, b \| 0x55), Hersteller-ID zuerst [tube rdm.c], [OLA DiscoveryAgent] |
| 20–23 | ECS: 16-Bit-Summe der 12 EUID-Bytes, ebenfalls als (hi\|AA, hi\|55, lo\|AA, lo\|55) kodiert [tube rdm.c], [OLA DiscoveryAgent] |

Dekodieren: `uid[i] = e[2i] & e[2i+1]` und `checksum = (ecs0 & ecs1) << 8 | (ecs2 & ecs3)`. Die Prüfsumme wird
mit der Summe der 12 **kodierten** Bytes verglichen [OLA DiscoveryAgent], [flash_rdm.py `dub_decode`]. Weil
`(b|AA)+(b|55) = b + 0xFF` gilt, ist die Prüfsumme `sum(UID) + 6·0xFF`. Diese Abkürzung nutzt der Tube-Bootloader
[tube boot `send_dub_response`].

### 3.2 DISC_MUTE (0x0002) / DISC_UN_MUTE (0x0003)
- Anfrage: CC 0x10, PDL 0, Sub-Device 0. Bei Broadcast antwortet niemand [tube rdm.c]. Eine Anfrage mit PDL ≠ 0
  wird nicht beantwortet, OLA erwartet dann einen Timeout [OLA ResponderTest `MuteDeviceWithData` „Section 6.3.4“],
  [tube rdm.c]. Eine unbekannte DISCOVERY-PID wird ebenfalls nicht beantwortet [OLA `InvalidDiscoveryPID`].
- Antwort-PD: Control Field (u16), optional gefolgt von einer Binding-UID (6). Die PDL ist also 2 oder 8
  [OLA pids.proto].

| Bit im Control Field | Bedeutung | Quelle |
|---|---|---|
| 0x0001 | Managed Proxy Flag | [Ja Rule rdm.h `MUTE_MANAGED_PROXY_FLAG`] |
| 0x0002 | Sub-Device Flag (Gerät hat Sub-Devices) | [Ja Rule rdm.h; rdm_responder.c setzt das Bit bei sub_device_count > 0] |
| 0x0004 | Boot-Loader Flag (Gerät kann ohne Firmware-Upload nicht arbeiten) | [Ja Rule rdm.h], [tube boot „E1.20 7.6.1“] |
| 0x0008 | Proxied Device Flag | [Ja Rule rdm.h `MUTE_PROXY_FLAG`] |
| übrige | reserviert, 0 | [ungeprüft] |

- Binding-UID: Bei Geräten mit mehreren Ports/UIDs ist das die UID des „primären“ Ports
  [ungeprüft, sinngemäß E1.20 §7.6]. OLA warnt, wenn die Hersteller-ID der Binding-UID nicht zur Hersteller-ID des
  Geräts passt [OLA ResponderTest `MuteDevice`]. OLA erwartet außerdem gleiche Control Fields in MUTE- und
  UN_MUTE-Antwort [OLA ResponderTest `UnMuteDevice`].

### 3.3 Algorithmus (Binärsuche) – wie OLA ihn umsetzt [OLA DiscoveryAgent]
1. **Full:** Die UID-Liste wird geleert. **Incremental:** Die bekannten UIDs kommen in eine Mute-Liste.
2. Der Stapel startet mit dem Bereich (0000:00000000, FFFF:FFFFFFFF).
3. **UN_MUTE an Broadcast, 3-mal** (`BROADCAST_UNMUTE_REPEATS = 3`).
4. Nur bei Incremental wird jede bekannte UID per Unicast gemutet. Antwortet eine UID nicht, ist das Gerät weg und
   wird aus dem Set entfernt.
5. Schleife über den Bereich oben auf dem Stapel, mit DUB(lower, upper):
   - **Keine Antwort:** Der Bereich ist leer und wird vom Stapel genommen.
   - **Gültige Antwort** (Präambel, Separator und Prüfsumme stimmen): Die dekodierte UID wird per **DISC_MUTE an sie
     verifiziert**, mit bis zu 5 Versuchen (`MAX_MUTE_ATTEMPTS`). ACK → UID aufnehmen und **denselben Bereich erneut**
     DUBben. Kein ACK → UID als „bad“ merken; das kann eine Phantom-UID aus einer Kollision sein.
   - **Kaputte Antwort** (Länge, Präambel oder Prüfsumme falsch) = Kollision. Der Bereich wird bei `mid = (lo+hi)/2`
     in (lo, mid) und (mid+1, hi) geteilt, beide kommen auf den Stapel. Bei lo == hi steigt `failures` hoch,
     eine weitere Teilung ist nicht möglich.
   - Antwortet eine schon gemutete oder „bad“ UID weiter, ist der Responder defekt. OLA teilt den Bereich dann um
     diese UID herum (`SplitAroundBadUID`: lo..uid−1, uid+1..hi), jede UID nur einmal.
   - Abbruch für einen Bereich: 5 Fehlschläge (`MAX_BRANCH_FAILURES`), 5 leere Versuche ohne Fund
     (`MAX_EMPTY_BRANCH_ATTEMPTS`) oder „corrupt“. Der Elternbereich wird dann als corrupt markiert. Das Ergebnis
     meldet „tree corrupt“, liefert die gefundenen UIDs aber trotzdem.
6. Leerer Stapel bedeutet: fertig.

Für den Node:
- Pro Port eine eigene Discovery-State-Machine. Die 4 Ports laufen parallel, auf einem Port strikt sequentiell.
- Nach jedem DUB 5,8 ms warten (§2).
- Kurz vor dem Mute-ACK einer UID ist ein Unicast-GET DEVICE_INFO sinnvoll. Optional bestätigt es, dass die UID
  echt ist; OLA verlässt sich allein auf das Mute-ACK.
- Optimierung, die OLA nicht macht: Steht im Bereich nur eine einzelne UID, gleich DISC_MUTE an sie senden
  [eigene Idee, ungeprüft].

---

## 4. Response Types, NACK-Gründe, Queued/Status Messages

### 4.1 Response Types [Ja Rule rdm.h], [OLA RDMEnums.h `ACK_OVERFLOW = 3`]
| Wert | Typ | PD |
|---|---|---|
| 0x00 | RESPONSE_TYPE_ACK | Antwortdaten |
| 0x01 | ACK_TIMER | u16 geschätzte Zeit in **100-ms-Einheiten**, nach der die Antwort über QUEUED_MESSAGE abholbar ist [OLA AckTimer: `ack_time = 1 + ACK_TIMER_MS/100`] |
| 0x02 | NACK_REASON | u16 NACK-Grund |
| 0x03 | ACK_OVERFLOW | Teildaten. Der Controller wiederholt **dieselbe** Anfrage, bis ein ACK kommt, und hängt die PD aneinander [OLA RDMCommand.cpp „combine the data from two responses in an ACK_OVERFLOW“] |

Wer ACK_TIMER verwendet, muss QUEUED_MESSAGE und STATUS_MESSAGES unterstützen
[OLA ResponderTest `_HandleQueuedResponse`: „if we get an ACK_TIMER, QUEUED_MESSAGE should be supported“],
[tube rdm_boot.h „E1.20 10.3“]. Antworten auf Broadcasts gibt es nie, also auch kein ACK_TIMER (§5).

### 4.2 NACK-Gründe [OLA RDMEnums.h]
| Code | Name | Ursprung |
|---|---|---|
| 0x0000 | NR_UNKNOWN_PID | E1.20 |
| 0x0001 | NR_FORMAT_ERROR (falsche PDL oder unparsebare PD) | E1.20 |
| 0x0002 | NR_HARDWARE_FAULT | E1.20 |
| 0x0003 | NR_PROXY_REJECT | E1.20 |
| 0x0004 | NR_WRITE_PROTECT (z. B. gesperrt durch LOCK_STATE) | E1.20 |
| 0x0005 | NR_UNSUPPORTED_COMMAND_CLASS | E1.20 |
| 0x0006 | NR_DATA_OUT_OF_RANGE | E1.20 |
| 0x0007 | NR_BUFFER_FULL | E1.20 |
| 0x0008 | NR_PACKET_SIZE_UNSUPPORTED | E1.20 |
| 0x0009 | NR_SUB_DEVICE_OUT_OF_RANGE | E1.20 |
| 0x000A | NR_PROXY_BUFFER_FULL | E1.20 |
| 0x000B | NR_ACTION_NOT_SUPPORTED | E1.37-2 [ungeprüft] |
| 0x000C–0x0013 | ENDPOINT_NUMBER_INVALID, INVALID_ENDPOINT_MODE, UNKNOWN_UID, UNKNOWN_SCOPE, INVALID_STATIC_CONFIG_TYPE, INVALID_IPV4_ADDRESS, INVALID_IPV6_ADDRESS, INVALID_PORT | E1.37-7 / E1.33 [ungeprüft] |
| 0x0014–0x0019 | DEVICE_ABSENT, SENSOR_OUT_OF_RANGE, SENSOR_FAULT, PACKING_NOT_SUPPORTED, ERROR_IN_PACKED_LIST_TRANSACTION, PROXY_DROP | E1.37-5 / E1.20-2025 [ungeprüft] |
| 0x0020 | NR_ALL_CALL_SET_FAIL | E1.20-2025 [ungeprüft] |

Die Ursprungsspalte für 0x0000–0x000A ist [ungeprüft]. Belegt ist sie nur indirekt dadurch, dass die Tube
(E1.20-2010) 0x0000–0x0009 verwendet.

### 4.3 Message Count, QUEUED_MESSAGE, STATUS_MESSAGES
- **Message Count** (Offset 17) gibt in jeder Antwort an, wie viele Nachrichten in der Queue warten (max. 255).
  Ein Controller, der > 0 sieht, holt sie per GET QUEUED_MESSAGE ab [OLA RDMEnums.h], [ungeprüft: Abholpflicht].
- **Status-Typen** [OLA RDMEnums.h]: 0x00 NONE, 0x01 GET_LAST_MESSAGE, 0x02 ADVISORY, 0x03 WARNING, 0x04 ERROR,
  0x12 ADVISORY_CLEARED, 0x13 WARNING_CLEARED, 0x14 ERROR_CLEARED.
- **GET QUEUED_MESSAGE (0x0020)** [OLA pids.proto], [OLA AckTimer]:
  - Anfrage-PD ist der u8 Status-Typ. Erlaubt sind 1 (Last Message, holt die zuletzt gelieferte Nachricht erneut,
    für verlorene Antworten), 2, 3 oder 4. Nur Root-Device.
  - Antwort: die nächste Queue-Nachricht mit der **PID und CC der ursprünglichen Antwort**, z. B. PID
    DMX_START_ADDRESS mit CC SET_RESPONSE. OLA erlaubt deshalb bei QUEUED_MESSAGE eine abweichende CC und ein
    abweichendes Sub-Device [OLA RDMCommand.cpp].
  - **Queue leer:** ACK mit **PID STATUS_MESSAGES (0x0030), PDL 0**, also eine leere Status-Liste
    [OLA AckTimer `EmptyStatusMessage`]. Vektor: `RDM_QUEUED_MESSAGE.json`.
- **GET STATUS_MESSAGES (0x0030)**: Anfrage-PD u8 Status-Typ (0, 1, 2, 3, 4). Antwort-PD ist eine Liste aus
  9-Byte-Einträgen: `sub_device u16, status_type u8, status_message_id u16, data_value1 i16, data_value2 i16`
  [OLA pids.proto]. In 231 Byte passen damit höchstens 25 Einträge (Rechnung). Typ 0 bzw. eine leere Liste bedeutet
  „keine Meldungen“ [ungeprüft für Typ-0-Semantik].
- **STATUS_ID_DESCRIPTION (0x0031)**: GET mit u16 status_id → ASCII-Text ≤ 32 [OLA pids.proto].
- **CLEAR_STATUS_ID (0x0032)**: nur SET, PDL 0. Leert die Status-Queue [OLA pids.proto], [OLA ResponderTest `ClearStatusMessages`].
- **SUB_DEVICE_STATUS_REPORT_THRESHOLD (0x0033)**: GET nur für Sub-Devices, SET mit u8 Status-Typ (0, 2, 3, 4).
  Legt fest, ab welcher Schwere ein Sub-Device Meldungen erzeugt [OLA pids.proto].

**Standard-Status-Message-IDs** [OLA RDMEnums.h]. Die Platzhalter %d bzw. Data1/2 stehen in E1.20 Tab. B-2
[ungeprüft für die Data-Semantik].

| ID | Name | ID | Name |
|---|---|---|---|
| 0x0001 | CAL_FAIL | 0x0034 | UNDERCURRENT |
| 0x0002 | SENS_NOT_FOUND | 0x0035 | PHASE |
| 0x0003 | SENS_ALWAYS_ON | 0x0036 | PHASE_ERROR |
| 0x0004 | FEEDBACK_ERROR | 0x0037 | AMPS |
| 0x0005 | INDEX_ERROR | 0x0038 | VOLTS |
| 0x0011 | LAMP_DOUSED | 0x0041 | DIMSLOT_OCCUPIED |
| 0x0012 | LAMP_STRIKE | 0x0042 | BREAKER_TRIP |
| 0x0013 | LAMP_ACCESS_OPEN | 0x0043 | WATTS |
| 0x0014 | LAMP_ALWAYS_ON | 0x0044 | DIM_FAILURE |
| 0x0021 | OVERTEMP | 0x0045 | DIM_PANIC |
| 0x0022 | UNDERTEMP | 0x0046 | LOAD_FAILURE |
| 0x0023 | SENS_OUT_RANGE | 0x0050 | READY |
| 0x0031 | OVERVOLTAGE_PHASE | 0x0051 | NOT_READY |
| 0x0032 | UNDERVOLTAGE_PHASE | 0x0052 | LOW_FLUID |
| 0x0033 | OVERCURRENT | 0x0060 / 0x0061 / 0x0062 | EEPROM_ERROR / RAM_ERROR / FPGA_ERROR |
| 0x0070 | PROXY_BROADCAST_DROPPED | 0x0071 / 0x0072 | ASC_RXOK / ASC_DROPPED |
| 0x0080–0x0083 | DMXNSCNONE / DMXNSCLOSS / DMXNSCERROR / DMXNSC_OK (E1.37-5) | | |

Herstellerspezifische Status-IDs liegen in 0x8000–0xFFDF [ungeprüft]. Ihre Texte liefert STATUS_ID_DESCRIPTION.

---

## 5. Broadcast, Vendorcast, Sub-Devices

- **Broadcast** FFFF:FFFFFFFF an alle, **Vendorcast** mmmm:FFFFFFFF an alle Geräte des Herstellers mmmm
  [tube rdm.c], [OLA ResponderTest `SetVendorcast*`/`SetBroadcast*`].
- Auf Broadcast und Vendorcast wird **nie geantwortet**. Ein SET wird ausgeführt, ein GET ist ungültig und wird
  ignoriert [tube boot], [tube README]. OLA prüft das, indem es einen Broadcast-SET mit `BroadcastResult` sendet und
  danach per Unicast-GET kontrolliert [OLA ResponderTest `SetNonUnicastIdentifyMixin`].
- Den Vendorcast eines **fremden** Herstellers muss der Responder ignorieren [OLA ResponderTest
  `SetOtherVendorcastIdentifyDevice`, `DUBNegativeVendorcast`].
- Herstellerspezifische PIDs (≥ 0x8000) an FFFF:FFFFFFFF werden ignoriert. Sie gelten nur per Vendorcast oder
  Unicast [tube rdm_boot.h „E1.20 6.2.10.2“].
- Hersteller-IDs liegen in 0x0000–0x7FFF [OLA ResponderTest `DUBSignedComparisons` „Section 5.1 of E1.20“]. Die
  Tube verwendet 0x7FF0 aus dem ESTA-Prototypenbereich [tube config.h]. Der Bereich 0x7FF0–0x7FFF ist
  [ungeprüft]. 0xFFFF steht für „alle Hersteller“.
- **Sub-Devices:**
  - 0x0000 = Root, 0x0001–0x0200 (1–512) = Sub-Devices, 0xFFFF = ALL_SUB_DEVICES [OLA RDMEnums.h `MAX_SUBDEVICE_NUMBER = 0x0200`].
  - 0xFFFF ist nur bei **SET** erlaubt. Ein GET an 0xFFFF → NACK SUB_DEVICE_OUT_OF_RANGE
    [OLA ResponderTest `AllSubDevicesGetMixin` „E1.20 section 9.2.2“]. OLA akzeptiert auch UNKNOWN_PID, wenn die PID
    gar nicht unterstützt wird.
  - Die Antwort auf einen SET an 0xFFFF kommt mit Sub-Device 0xFFFF bzw. von Root. OLA vergleicht das Sub-Device in
    diesem Fall nicht [OLA RDMCommand.cpp], [tube README „Antwort mit 0xFFFF“].
  - Hat ein Gerät keine Sub-Devices, gilt: Sub-Device ≠ 0 → NACK SUB_DEVICE_OUT_OF_RANGE [tube rdm.c].
  - Discovery-Befehle gehen immer an Root [OLA pids.proto `discovery_sub_device_range: ROOT_DEVICE`].
  - Welche PID an Sub-Devices zulässig ist, steht pro PID in §6 (`get/set_sub_device_range`) [OLA pids.proto].

---

## 6. PID-Tabellen

Notation: **G** = GET, **S** = SET, **D** = DISCOVERY. „Anfrage“ und „Antwort“ geben die PD-Layouts an,
„—“ heißt PDL 0. Typen: u8/u16/u32 vorzeichenlos, i16 vorzeichenbehaftet, str ≤ n = ASCII ohne NUL.
SD-Spalte: R = nur Root, R/S = Root oder Sub-Device, alle = auch 0xFFFF (bei SET).
Layouts nach [OLA pids.proto], Werte nach [OLA RDMEnums.h], soweit nicht anders markiert.

**PID-Bereiche** [OLA RDMConstants.py]: ESTA 0x0000–0x7FDF, **Hersteller 0x8000–0xFFDF**. 0x7FE0–0x7FFF und
0xFFE0–0xFFFF sind reserviert [ungeprüft für die Reservierung].

### 6.1 E1.20: Netzwerk und Status
| PID | Name | CC | SD | Anfrage → Antwort |
|---|---|---|---|---|
| 0x0001 | DISC_UNIQUE_BRANCH | D | R | lower UID(6), upper UID(6) → DUB-Kodierung (§3.1) |
| 0x0002 | DISC_MUTE | D | R | — → control_field u16 [+ binding_uid UID] |
| 0x0003 | DISC_UN_MUTE | D | R | wie DISC_MUTE |
| 0x0010 | PROXIED_DEVICES | G | R | — → UID-Liste (6 B je Eintrag; mehr als 38 nur über ACK_OVERFLOW, Rechnung) |
| 0x0011 | PROXIED_DEVICE_COUNT | G | R | — → device_count u16, list_changed u8(bool) |
| 0x0015 | COMMS_STATUS | G/S | R | G: — → short_message u16, length_mismatch u16, checksum_fail u16. S: — → — (setzt die Zähler zurück) |
| 0x0020 | QUEUED_MESSAGE | G | R | status_type u8 → Queue-Nachricht bzw. STATUS_MESSAGES (§4.3) |
| 0x0030 | STATUS_MESSAGES | G | R | status_type u8 → n × {sub_device u16, status_type u8, message_id u16, data1 i16, data2 i16} |
| 0x0031 | STATUS_ID_DESCRIPTION | G | R | status_id u16 → str ≤ 32 |
| 0x0032 | CLEAR_STATUS_ID | S | alle | — → — |
| 0x0033 | SUB_DEVICE_STATUS_REPORT_THRESHOLD | G/S | G: nur Sub-Devices; S: alle | G: — → status_type u8. S: status_type u8 (0, 2, 3, 4) → — |

### 6.2 E1.20: RDM-Information und Produktinformation
| PID | Name | CC | SD | Anfrage → Antwort |
|---|---|---|---|---|
| 0x0050 | SUPPORTED_PARAMETERS | G | R/S | — → n × PID u16 |
| 0x0051 | PARAMETER_DESCRIPTION | G | R | pid u16 → siehe unten (20 B + Text ≤ 32) |
| 0x0060 | DEVICE_INFO | G | R/S | — → 19 B, siehe unten |
| 0x0070 | PRODUCT_DETAIL_ID_LIST | G | R/S | — → bis 6 × detail_id u16 (z. B. 0x0004 LED, 0x0601 ETHERNET_NODE, 0x0600 SPLITTER) |
| 0x0080 | DEVICE_MODEL_DESCRIPTION | G | R/S | — → str ≤ 32 |
| 0x0081 | MANUFACTURER_LABEL | G | R/S | — → str ≤ 32 |
| 0x0082 | DEVICE_LABEL | G/S | R/S, S: alle | — ↔ str ≤ 32. Ein NUL beendet den Text [tube rdm.c „E1.20 10.1“] |
| 0x0090 | FACTORY_DEFAULTS | G/S | R/S, S: alle | G: — → using_defaults bool. S: — → — (setzt auf Werkseinstellung) |
| 0x00A0 | LANGUAGE_CAPABILITIES | G | R/S | — → n × 2-Zeichen-Code (ISO 639-1) |
| 0x00B0 | LANGUAGE | G/S | R/S, S: alle | — ↔ str 2 |
| 0x00C0 | SOFTWARE_VERSION_LABEL | G | R/S | — → str ≤ 32 |
| 0x00C1 | BOOT_SOFTWARE_VERSION_ID | G | R/S | — → u32 |
| 0x00C2 | BOOT_SOFTWARE_VERSION_LABEL | G | R/S | — → str ≤ 32 |

**DEVICE_INFO, 19 Byte** [OLA pids.proto], [tube rdm.c]. Vektor: `RDM_GET_DEVICE_INFO.json`.
| Off | Typ | Feld | Bemerkung |
|---|---|---|---|
| 0 | u16 | RDM Protocol Version | 0x0100 [OLA RDMEnums.h `RDM_VERSION_1_0`] |
| 2 | u16 | Device Model ID | |
| 4 | u16 | Product Category | z. B. 0x0101 FIXTURE_FIXED, 0x0801 DATA_DISTRIBUTION, 0x0802 DATA_CONVERSION (passend für einen Art-Net-Node [ungeprüft, welche Kategorie für einen Node üblich ist]) |
| 6 | u32 | Software Version ID | |
| 10 | u16 | DMX512 Footprint | 0..512 |
| 12 | u8 | Current Personality | ≥ 1, wenn Footprint > 0 (sonst Warnung in OLA) [OLA ResponderTest `GetDeviceInfo`] |
| 13 | u8 | Personality Count | |
| 14 | u16 | DMX512 Start Address | 1..512. 0xFFFF bei Footprint 0 [OLA RDMEnums.h `ZERO_FOOTPRINT_DMX_ADDRESS`]. 0 ist ungültig [OLA ResponderTest] |
| 16 | u16 | Sub-Device Count | ≤ 512 |
| 18 | u8 | Sensor Count | |

**PARAMETER_DESCRIPTION, Antwort** [OLA pids.proto], [tube rdm.c `pdesc_boot`]:
`pid u16, pdl_size u8, data_type u8, command_class u8, type u8 (= 0, sonst Warnung in OLA), unit u8, prefix u8,
min_valid u32, max_valid u32, default u32, description str ≤ 32`. Das sind 20 B + Text.
Vektor: `RDM_GET_PARAMETER_DESCRIPTION.json`.

- *command_class:* 0x01 GET, 0x02 SET, 0x03 GET_SET [OLA RDMEnums.h `rdm_command_class`].
- *data_type* [OLA RDMEnums.h; Bereich 0x80–0xDF Hersteller laut OLA pids.proto]:

| Code | Typ | Code | Typ |
|---|---|---|---|
| 0x00 | DS_NOT_DEFINED | 0x0A | DS_INT64 |
| 0x01 | DS_BIT_FIELD | 0x0B | DS_GROUP |
| 0x02 | DS_ASCII | 0x0C | DS_UID |
| 0x03 | DS_UNSIGNED_BYTE | 0x0D | DS_BOOLEAN |
| 0x04 | DS_SIGNED_BYTE | 0x0E | DS_URL |
| 0x05 | DS_UNSIGNED_WORD | 0x0F | DS_MAC |
| 0x06 | DS_SIGNED_WORD | 0x10 | DS_IPV4 |
| 0x07 | DS_UNSIGNED_DWORD | 0x11 | DS_IPV6 |
| 0x08 | DS_SIGNED_DWORD | 0x12 | DS_ENUMERATION |
| 0x09 | DS_UINT64 | 0x80–0xDF | herstellerspezifisch |

  Die Codes 0x00–0x08 stammen aus E1.20-2010, 0x09–0x12 aus späteren Normen (E1.37-5 / E1.20-2025) [ungeprüft].

- *unit* [OLA RDMEnums.h; 0x80–0xFF Hersteller]: 00 NONE, 01 CENTIGRADE, 02 VOLTS_DC, 03 VOLTS_AC_PEAK,
  04 VOLTS_AC_RMS, 05 AMPERE_DC, 06 AMPERE_AC_PEAK, 07 AMPERE_AC_RMS, 08 HERTZ, 09 OHM, 0A WATT, 0B KILOGRAM,
  0C METERS, 0D METERS_SQUARED, 0E METERS_CUBED, 0F KILOGRAMMES_PER_METER_CUBED, 10 METERS_PER_SECOND,
  11 METERS_PER_SECOND_SQUARED, 12 NEWTON, 13 JOULE, 14 PASCAL, 15 SECOND, 16 DEGREE, 17 STERADIAN, 18 CANDELA,
  19 LUMEN, 1A LUX, 1B IRE, 1C BYTE, 1D DECIBEL, 1E DECIBEL_VOLT, 1F DECIBEL_WATT, 20 DECIBEL_METER, 21 PERCENT,
  22 MOLES_PER_METER_CUBED, 23 RPM, 24 BYTE_PER_SECOND.
- *prefix* [OLA RDMEnums.h]: 00 NONE, 01 DECI, 02 CENTI, 03 MILLI, 04 MICRO, 05 NANO, 06 PICO, 07 FEMTO,
  08 ATTO, 09 ZEPTO, 0A YOCTO, 11 DECA, 12 HECTO, 13 KILO, 14 MEGA, 15 GIGA, 16 TERA, 17 PETA, 18 EXA, 19 ZETTA,
  1A YOTTA.

### 6.3 E1.20: DMX-Setup, Sensoren, Power/Lamp, Display, Konfiguration, Control
| PID | Name | CC | Anfrage → Antwort |
|---|---|---|---|
| 0x00E0 | DMX_PERSONALITY | G/S | G: — → current u8, count u8. S: personality u8 (1..255) → — |
| 0x00E1 | DMX_PERSONALITY_DESCRIPTION | G | personality u8 → personality u8, slots_required u16, str ≤ 32 |
| 0x00F0 | DMX_START_ADDRESS | G/S | G: — → u16. S: u16 (1..512) → —. Die Tube prüft zusätzlich Start + Footprint − 1 ≤ 512 [tube rdm.c] |
| 0x0120 | SLOT_INFO | G | — → n × {slot_offset u16, slot_type u8, slot_label_id u16} (5 B) |
| 0x0121 | SLOT_DESCRIPTION | G | slot u16 → slot u16, str ≤ 32 |
| 0x0122 | DEFAULT_SLOT_VALUE | G | — → n × {slot_offset u16, value u8} |
| 0x0200 | SENSOR_DEFINITION | G | sensor u8 (0..254) → sensor u8, type u8, unit u8, prefix u8, range_min i16, range_max i16, normal_min i16, normal_max i16, recorded_support u8 (Bit 0 recorded value, Bit 1 lowest/highest [OLA RDMEnums.h]), str ≤ 32 |
| 0x0201 | SENSOR_VALUE | G/S | G: sensor u8 → sensor u8, present i16, lowest i16, highest i16, recorded i16. S: sensor u8 (0xFF = alle) → wie G (Reset) |
| 0x0202 | RECORD_SENSORS | S | sensor u8 (0xFF = alle) → — |
| 0x0400 | DEVICE_HOURS | G/S | u32 |
| 0x0401 | LAMP_HOURS | G/S | u32 |
| 0x0402 | LAMP_STRIKES | G/S | u32 |
| 0x0403 | LAMP_STATE | G/S | u8: 0 OFF, 1 ON, 2 STRIKE, 3 STANDBY, 4 NOT_PRESENT, 0x7F ERROR |
| 0x0404 | LAMP_ON_MODE | G/S | u8: 0 OFF, 1 DMX, 2 ON, 3 ON_AFTER_CAL |
| 0x0405 | DEVICE_POWER_CYCLES | G/S | u32 |
| 0x0500 | DISPLAY_INVERT | G/S | u8: 0 Off, 1 On, 2 Auto |
| 0x0501 | DISPLAY_LEVEL | G/S | u8 (0 aus … 0xFF voll [ungeprüft]) |
| 0x0600 / 0x0601 / 0x0602 | PAN_INVERT / TILT_INVERT / PAN_TILT_SWAP | G/S | bool |
| 0x0603 | REAL_TIME_CLOCK | G/S | year u16 (≥ 2003), month, day, hour, minute, second u8 |
| 0x1000 | IDENTIFY_DEVICE | G/S | bool. SET mit Wert > 1 → DATA_OUT_OF_RANGE [tube rdm.c], [OLA `SetOutOfRangeIdentifyDevice`] |
| 0x1001 | RESET_DEVICE | S | u8: 0x01 warm, 0xFF cold |
| 0x1010 | POWER_STATE | G/S | u8: 0 FULL_OFF, 1 SHUTDOWN, 2 STANDBY, 0xFF NORMAL |
| 0x1020 | PERFORM_SELFTEST | G/S | G: — → bool aktiv. S: test u8 (0 = aus, 0xFF = alle) |
| 0x1021 | SELF_TEST_DESCRIPTION | G | test u8 → test u8, str ≤ 32 |
| 0x1030 | CAPTURE_PRESET | S | scene u16 (1..65534), fade_up u16, fade_down u16, wait u16 (Zehntelsekunden [ungeprüft]) |
| 0x1031 | PRESET_PLAYBACK | G/S | mode u16 (0 OFF, 1..n Szene, 0xFFFF ALL), level u8 |

Sub-Device-Regel für alle Zeilen in 6.3: GET an R/S, SET auch an 0xFFFF [OLA pids.proto].
Slot-Typen und Slot-Label-IDs (ST_*, SD_*) stehen in [OLA RDMEnums.h], die Tube nutzt 0x0001, 0x0205, 0x0206 und
0x0207 [tube README]. Sensortypen 0x00–0x28, 0x7F OTHER, ab 0x80 Hersteller [OLA RDMEnums.h, pids.proto].

### 6.4 E1.37-1 (Dimmer und allgemeine Erweiterungen)
| PID | Name | CC | SD | Anfrage → Antwort |
|---|---|---|---|---|
| 0x0140 | DMX_BLOCK_ADDRESS | G/S | R | G: — → sub_device_footprint u16, base_dmx_address u16. S: base u16 (1..512) → —. Setzt alle Sub-Devices hintereinander |
| 0x0141 | DMX_FAIL_MODE | G/S | R/S | scene u16 (0 = fester Level), loss_of_signal_delay u16 (0xFFFF = unendlich), hold_time u16 (0xFFFF = unendlich), level u8. Zeiten in Zehntelsekunden [ungeprüft] |
| 0x0142 | DMX_STARTUP_MODE | G/S | R/S | scene u16, startup_delay u16, hold_time u16, level u8 (wie FAIL_MODE) |
| 0x0340 | DIMMER_INFO | G | R/S | — → min_level_lower u16, min_level_upper u16, max_level_lower u16, max_level_upper u16, number_curves u8, levels_resolution u8 (Bits, 0..16), split_levels_supported bool |
| 0x0341 | MINIMUM_LEVEL | G/S | R/S | min_increasing u16, min_decreasing u16, on_below_minimum bool |
| 0x0342 | MAXIMUM_LEVEL | G/S | R/S | u16 |
| 0x0343 | CURVE | G/S | R/S | G: current u8, count u8. S: curve u8 |
| 0x0344 | CURVE_DESCRIPTION | G | R/S | curve u8 → curve u8, str ≤ 32 |
| 0x0345 | OUTPUT_RESPONSE_TIME | G/S | R/S | G: current u8, count u8. S: u8 |
| 0x0346 | OUTPUT_RESPONSE_TIME_DESCRIPTION | G | R/S | u8 → u8, str ≤ 32 |
| 0x0347 | MODULATION_FREQUENCY | G/S | R/S | G: current u8, count u8. S: u8 |
| 0x0348 | MODULATION_FREQUENCY_DESCRIPTION | G | R/S | u8 → u8, frequency u32 (Hz), str ≤ 32 |
| 0x0440 | BURN_IN | G/S | R/S | G: hours_remaining u8. S: hours u8 |
| 0x0640 | LOCK_PIN | G/S | R/S | G: — → pin u16 (0..9999). S: new_pin u16, current_pin u16 → — |
| 0x0641 | LOCK_STATE | G/S | R/S | G: — → current_state u8 (0 = Unlocked), state_count u8. S: pin u16, state u8 → — |
| 0x0642 | LOCK_STATE_DESCRIPTION | G | R/S | state u8 (1..255) → state u8, str ≤ 32 |
| 0x1040 | IDENTIFY_MODE | G/S | R/S | u8: 0x00 QUIET, 0xFF LOUD |
| 0x1041 | PRESET_INFO | G | R/S | — → 6 × bool (level_field, preset_sequence, split_times, fail_infinite_delay, fail_infinite_hold, startup_infinite_hold), max_scene u16, dann 12 × u16 min/max für preset fade, preset wait, fail delay, fail hold, startup delay, startup hold (insgesamt 32 B) |
| 0x1042 | PRESET_STATUS | G/S | R/S | G: scene u16 → scene u16, up_fade u16, down_fade u16, wait u16, programmed u8 (0 nicht, 1 programmiert, 2 read-only). S: scene, up, down, wait, clear_preset bool |
| 0x1043 | PRESET_MERGEMODE | G/S | R/S | u8: 0 DEFAULT, 1 HTP, 2 LTP, 3 DMX_ONLY, 0xFF OTHER |
| 0x1044 | POWER_ON_SELF_TEST | G/S | R/S | bool |

**LOCK_PIN / LOCK_STATE: Semantik**
- *PIN:* u16 im Bereich 0..9999 [OLA RDMEnums.h `MAX_LOCK_PIN`].
  - SET LOCK_PIN braucht die aktuelle PIN. Ist sie falsch → NACK DATA_OUT_OF_RANGE.
  - Eine neue PIN > 9999 → NACK **FORMAT_ERROR**, nicht DATA_OUT_OF_RANGE. DATA_OUT_OF_RANGE ist für die falsche PIN
    reserviert [OLA ResponderTest `SetOutOfRangeLockPin`, Kommentar „E1.37-1 Section 3.9“], [OLA AdvDimmer `SetLockPin`].
  - GET LOCK_PIN darf ein Gerät mit NACK UNSUPPORTED_COMMAND_CLASS verweigern
    [OLA ResponderTest `GetLockPin` `ALLOWED_NACKS`].
  - Werks-PIN: [ungeprüft], in OLA-Referenzen 0.
- *LOCK_STATE:*
  - 0 = Unlocked. 1..n sind herstellerdefinierte Sperrstufen, beschrieben über LOCK_STATE_DESCRIPTION
    [OLA pids.proto], [OLA AdvDimmer: 1 „Start Address Locked“, 2 „Address and Personalities Locked“].
  - SET braucht die PIN. Ist sie falsch → DATA_OUT_OF_RANGE, ebenso bei einem unbekannten State
    [OLA AdvDimmer `SetWithPin`].
  - Ein durch den Lock gesperrter SET wird mit **NR_WRITE_PROTECT** abgelehnt [OLA AdvDimmer `SetDmxStartAddress`/`SetPersonality`].
  - GET bleibt immer erlaubt. SUPPORTED_PARAMETERS ändert sich durch den Lock nicht [OLA AdvDimmer, nur SETs werden geprüft].
  - OLA fordert: Wer LOCK_STATE unterstützt, muss auch LOCK_STATE_DESCRIPTION unterstützen
    [OLA ResponderTest `PID_DEPENDENCIES`]. current_state > state_count ist ein Fehler [OLA `GetLockState`].
- *Lokales Bedienfeld:* Ob ein Front-Panel die RDM-Sperre umgehen darf, bzw. ob die Sperre nur für RDM gilt, ist
  **[ungeprüft]**. Die OLA-Quellen sagen dazu nichts. Ein Herstellerdokument (ETC Selador) beschreibt nur den
  umgekehrten Fall: Die Panelsperre blockiert kein RDM.

### 6.5 E1.37-2 (IPv4/DNS-Konfiguration)
Interface-ID: u32 im Bereich 1..0xFFFFFF00 [OLA RDMConstants.py, RDMEnums.h]. Hardware-Typ 1 = Ethernet
[OLA pids.proto], 31 = IPsec [Ja Rule rdm.h]. Alle Zeilen: GET an R/S, SET auch an 0xFFFF [OLA pids.proto].

| PID | Name | CC | Anfrage → Antwort |
|---|---|---|---|
| 0x0700 | LIST_INTERFACES | G | — → n × {interface_id u32, hardware_type u16} |
| 0x0701 | INTERFACE_LABEL | G | id u32 → id u32, str ≤ 32 |
| 0x0702 | INTERFACE_HARDWARE_ADDRESS_TYPE1 | G | id u32 → id u32, MAC(6) |
| 0x0703 | IPV4_DHCP_MODE | G/S | G: id → id, bool. S: id, bool |
| 0x0704 | IPV4_ZEROCONF_MODE | G/S | wie DHCP_MODE |
| 0x0705 | IPV4_CURRENT_ADDRESS | G | id → id, ipv4(4) (0 = unkonfiguriert), netmask u8 (CIDR 0..32), dhcp_status u8 (0 inaktiv, 1 aktiv, 2 unbekannt) |
| 0x0706 | IPV4_STATIC_ADDRESS | G/S | G: id → id, ipv4, netmask u8. S: id, ipv4, netmask u8 |
| 0x0707 | INTERFACE_RENEW_DHCP | S | id u32 → — |
| 0x0708 | INTERFACE_RELEASE_DHCP | S | id u32 → — |
| 0x0709 | INTERFACE_APPLY_CONFIGURATION | S | id u32 → —. Erst dieser Befehl übernimmt die gesetzten DHCP-, Zeroconf- und Static-Werte [ungeprüft, sinngemäß E1.37-2] |
| 0x070A | IPV4_DEFAULT_ROUTE | G/S | G: — → id u32, ipv4 (0 = keine Route). S: id u32 (0 = keine), ipv4 |
| 0x070B | DNS_IPV4_NAME_SERVER | G/S | G: index u8 (0..2) → index, ipv4. S: index, ipv4 |
| 0x070C | DNS_HOSTNAME | G/S | str 1..63 |
| 0x070D | DNS_DOMAIN_NAME | G/S | str 0..231 |

OLA fordert LIST_INTERFACES, sobald irgendeine 0x0701–0x070D-PID unterstützt wird
[OLA ResponderTest `PID_REVERSE_DEPENDENCIES`]. Mit NR_ACTION_NOT_SUPPORTED (0x000B) lehnt das Gerät z. B. ein
DHCP-Renew bei inaktivem DHCP ab [ungeprüft].

### 6.6 E1.37-5 und neuere Ergänzungen (kurz)
OLA ordnet die folgenden PIDs E1.37-5 zu [OLA RDMEnums.h, Kommentar „E1.37-5 PIDs“]:
- MANUFACTURER_URL 0x00D0, PRODUCT_URL 0x00D1, FIRMWARE_URL 0x00D2: str (URL, ≥ 2 Zeichen)
- SERIAL_NUMBER 0x00D3: str ≤ 231
- DEVICE_INFO_OFFSTAGE 0x00D4
- DMX_PERSONALITY_ID 0x00E2
- TEST_DATA 0x0016, COMMS_STATUS_NSC 0x0017
- SENSOR_TYPE_CUSTOM 0x0210, SENSOR_UNIT_CUSTOM 0x0211
- SHIPPING_LOCK 0x0650, LIST_TAGS/ADD_TAG/REMOVE_TAG/CHECK_TAG/CLEAR_TAGS 0x0651–0x0655, DEVICE_UNIT_NUMBER 0x0656
- IDENTIFY_TIMEOUT 0x1050 (u16 s, 0 = aus), POWER_OFF_READY 0x1051

Außerdem führt OLA zwischen den E1.20-PIDs, also ohne E1.37-Kommentar (vermutlich E1.20-2025 [ungeprüft]):
- METADATA_PARAMETER_VERSION 0x0052, METADATA_JSON 0x0053, METADATA_JSON_URL 0x0054
- SUPPORTED_PARAMETERS_ENHANCED 0x0055, CONTROLLER_FLAG_SUPPORT 0x0056, NACK_DESCRIPTION 0x0057
- PACKED_PID_SUB 0x0058, PACKED_PID_INDEX 0x0059, ENUM_LABEL 0x005A
- SELFTEST_ENHANCED 0x1022

Für den Node sind vor allem relevant: MANUFACTURER_URL, FIRMWARE_URL, SERIAL_NUMBER und IDENTIFY_TIMEOUT, dazu
ENUM_LABEL bzw. METADATA_JSON zur Selbstbeschreibung von Hersteller-PIDs. Alles optional.

---

## 7. Pflicht-PIDs und was OLAs `rdm_responder_test.py` prüft

**Pflicht für jeden Responder** [tube README „Pflicht-PIDs“, „Table A-3“], [OLA ResponderTest `MANDATORY_PIDS`]:
- immer: DISC_UNIQUE_BRANCH, DISC_MUTE, DISC_UN_MUTE, DEVICE_INFO, SOFTWARE_VERSION_LABEL, IDENTIFY_DEVICE (G/S)
- SUPPORTED_PARAMETERS, sobald mehr als das Minimum unterstützt wird
- PARAMETER_DESCRIPTION, sobald Hersteller-PIDs in SUPPORTED_PARAMETERS stehen
- DMX_START_ADDRESS, sobald der Footprint > 0 ist
- QUEUED_MESSAGE und STATUS_MESSAGES, sobald ACK_TIMER benutzt wird [tube rdm_boot.h], [OLA ResponderTest]

Die genauen Bedingungen der Table A-3 sind [ungeprüft]; die Tube-Formulierung stammt aus ihrer Normlektüre.

**Häufige Fehler, die der Test findet** [OLA ResponderTest]:
1. *SUPPORTED_PARAMETERS:*
   - DISC_*-PIDs in der Liste → Warning.
   - Pflicht-PIDs in der Liste (SUPPORTED_PARAMETERS, PARAMETER_DESCRIPTION, DEVICE_INFO, SOFTWARE_VERSION_LABEL,
     DMX_START_ADDRESS, IDENTIFY_DEVICE) → Advisory. Laut Tube „shall not be reported“ (E1.20 10.4.1).
   - Duplikate → Advisory. PIDs außerhalb der ESTA- bzw. Herstellerbereiche → Warning.
   - Unvollständige Gruppen → Advisory: PERSONALITY ohne DESCRIPTION, SENSOR_DEFINITION ohne VALUE,
     LANGUAGE_CAPABILITIES ohne LANGUAGE, CURVE ohne CURVE_DESCRIPTION, LOCK_STATE ohne LOCK_STATE_DESCRIPTION,
     DEFAULT_SLOT_VALUE ohne SLOT_DESCRIPTION, PROXIED_DEVICE_COUNT ohne PROXIED_DEVICES usw.
2. *PARAMETER_DESCRIPTION:*
   - **Jede** gelistete Hersteller-PID muss ein ACK liefern, mit derselben PID im Echo, `type == 0`,
     command_class 1..3 und druckbarer Beschreibung.
   - Für eine Nicht-Hersteller-PID, z. B. DEVICE_INFO, wird NACK **DATA_OUT_OF_RANGE** erwartet.
   - Ohne PD oder mit Extra-Daten → FORMAT_ERROR.
3. *Falsche PDL:*
   - GET mit Daten bei PDL-0-PIDs → FORMAT_ERROR erwartet. Ein ACK gibt eine Warning, bei Pflicht-PIDs muss
     überhaupt geantwortet werden (`GetMandatoryPIDWithDataMixin`).
   - Fehlende Daten → FORMAT_ERROR (`GetWithNoDataMixin`).
4. *Falsche Command Class:* SET auf eine reine GET-PID → **UNSUPPORTED_COMMAND_CLASS**, nicht UNKNOWN_PID
   (`UnsupportedSetMixin`). Eine unbekannte PID → UNKNOWN_PID.
5. *Sub-Devices:* GET an 0xFFFF → SUB_DEVICE_OUT_OF_RANGE. SUPPORTED_PARAMETERS muss über alle Sub-Devices hinweg
   konsistent sein (`GetSubDeviceSupportedParameters`). DEVICE_INFO wird an jedes Sub-Device geschickt (`FindSubDevices`).
6. *Broadcast und Vendorcast:* SET DEVICE_LABEL, DMX_START_ADDRESS und IDENTIFY_DEVICE per Broadcast oder eigenem
   Vendorcast müssen wirken, ohne Antwort. Ein fremder Vendorcast darf nichts bewirken.
7. *Discovery:*
   - Antwortverhalten über 15 DUB-Fälle: Vollbereich, eigener Herstellerbereich, Einzel-UID, ±1-Grenzen, invertierte
     Bereiche, vorzeichenlose Vergleiche, fremder Vendorcast.
   - DISC_MUTE und UN_MUTE mit PD → keine Antwort.
   - Ein ungemutetes Gerät muss weiter normal auf GET antworten (`RequestsWhileUnmuted`).
8. *Maximale Paketgröße:* GET mit PDL 231 darf nicht zum Timeout oder zu einer kaputten Antwort führen. Sonst sucht
   der Test per Binärsuche die maximale PDL (`DetermineMaxPacketSize`).
9. *DEVICE_INFO-Plausibilität:*
   - Footprint ≤ 512.
   - Ist der Footprint > 0, dann gilt Personality ≥ 1 und Personality ≤ Count.
   - Startadresse in 1..512 oder 0xFFFF.
   - Sub-Device-Count ≤ 512.
10. *ACK_TIMER:* Danach muss die Antwort über QUEUED_MESSAGE kommen. Ein NACK auf QUEUED_MESSAGE ist ein Fail,
    eine leere STATUS_MESSAGES-Liste, bevor die Antwort kam, ebenfalls („ACK_TIMER issued but the response was never queued“).
11. *LOCK_PIN:* Eine Out-of-Range-PIN muss FORMAT_ERROR liefern, eine falsche PIN DATA_OUT_OF_RANGE (§6.4).

---

## 8. Was die Tube-Implementierung schon richtig macht / wovon wir lernen

Alles aus [tube rdm.c], [tube boot], [tube rdm_boot.h], [tube README], [flash_rdm.py].

- **Strikte Rahmenprüfung vor allem anderen:** Sub-Startcode, `PDL + 24 == Length` und die 16-Bit-Summe werden
  geprüft. Ist etwas falsch, wird still verworfen. Antworten entstehen *in place* im Empfangspuffer: Dest und
  Source tauschen, TN, Sub-Device und PID bleiben, CC + 1. Das ist billig und fehlerarm.
- **Adressierung in drei Klassen:** Unicast, Broadcast und eigener Vendorcast.
  - Broadcast und Vendorcast werden ausgeführt, aber nie beantwortet. Broadcast-GET wird ignoriert.
  - Hersteller-PIDs an FFFF:FFFFFFFF werden ignoriert (6.2.10.2).
  - Gefährliches wie ENTER_BOOTLOADER wird per Broadcast bewusst *nicht* ausgeführt.
- **NACK-Reihenfolge wie von OLA erwartet:**
  1. Sub-Device prüfen → SUB_DEVICE_OUT_OF_RANGE.
  2. PID unbekannt → UNKNOWN_PID.
  3. PID bekannt, CC falsch → UNSUPPORTED_COMMAND_CLASS.
  4. PDL falsch → FORMAT_ERROR.
  5. Wert falsch → DATA_OUT_OF_RANGE.

  Der Bootloader löst das tabellengetrieben mit einem `pids[]`-Array aus {PID, CC, PDL}: ein Lookup liefert alle drei
  Prüfungen. Gutes Muster für den Node-Responder.
- **SUPPORTED_PARAMETERS ohne Pflicht-PIDs.** Die App beschreibt ihre einzige gelistete Hersteller-PID (0x8000) per
  PARAMETER_DESCRIPTION. Der Bootloader listet seine Hersteller-PIDs gar nicht und braucht deshalb keine
  PARAMETER_DESCRIPTION. Das ist ein legaler Trick, um Code zu sparen.
- **Kein ACK_TIMER:** Ist das Gerät beschäftigt, gibt es NACK BUFFER_FULL und der Controller wiederholt. Lange Arbeit
  wird gestückelt: APP_CRC in Stücken ≤ 256 B bleibt unter 2 ms. Damit bleibt Message Count immer 0 und
  QUEUED_MESSAGE/STATUS_MESSAGES sind nicht nötig. Für den Node gilt dasselbe, solange er jede Anfrage in < 2 ms
  beantworten kann. Für die ArtRdm-Antworten der Ports gilt das nicht, die laufen asynchron über das Netz.
- **Boot-Loader-Flag 0x0004 im DISC_MUTE-Control-Field**, solange keine gültige App installiert ist. Dieselbe UID in
  App und Bootloader, damit Updates über jeden Standard-Controller bzw. ArtRdm laufen.
- **DUB richtig:** Vorzeichenloser 48-Bit-Vergleich per `memcmp` auf big-endian-UIDs. Keine Antwort, wenn gemutet.
  Kein Break vor der DUB-Antwort. Die Prüfsumme spart Rechenzeit über `sum(UID) + 6·0xFF`.
- **Timing mit Reserve:** 180 µs Turnaround (Minimum 176), Break 200 µs, MAB 20 µs. Der Treiber wird nach dem letzten
  Stoppbit sofort abgeschaltet. Nach dem Reset schaltet die Tube DE zuerst auf Empfang, damit sie den Bus nicht
  blockiert.
- **Host-Seite ([flash_rdm.py] `Rdm.call`):** Retries bei fehlender Antwort (3×), bei BUFFER_FULL und bei ACK_TIMER
  (bis 100× mit Wartezeit aus der ACK_TIMER-PD). Das passt als Vorlage für die Retry-Policy im Rust-Crate.
- **Selbsttests offline:** Ein Simulator prüft Prüfsumme, DUB-Kodierung (inkl. der Firmware-Abkürzung) und den
  kompletten Update-Ablauf. Die Vektoren in `vectors/rdm/` sind gegen genau diese Funktionen gegengeprüft (siehe unten).
- **Bekannte Grenze:** Der Bootloader empfängt nur bis PDL 228. Für den Node gilt: immer 231 annehmen.

---

## 9. Testvektoren (`vectors/rdm/`)

| Datei | Inhalt |
|---|---|
| `RDM_GET_DEVICE_INFO.json` | GET-Anfrage; ACK der Tube-App (0x0100, Modell 0x0001, Kategorie 0x0101, SW 0x00000500, Footprint 12, Personality 1/4, Start 1, 0 Sub-Devices, 0 Sensoren); ACK des Bootloaders (SW 0xB0000003, Footprint 0, Start 0xFFFF); Anfrage mit kaputter Prüfsumme (`valid:false`) |
| `RDM_DISC_UNIQUE_BRANCH.json` | DUB Vollbereich; DUB untere Hälfte nach Kollision |
| `DUB_RESPONSE.json` | DUB-Antwort für 7FF0:12345678 mit 7 und mit 0 Präambelbytes; verstümmelte Antwort (`valid:false`) |
| `RDM_DISC_MUTE.json` | DISC_MUTE-Anfrage; Antworten mit Control Field 0x0004 (Bootloader) und 0x0000; Antwort mit Binding-UID (PDL 8); UN_MUTE an Broadcast |
| `RDM_NACK.json` | GET DEVICE_HOURS → NACK UNKNOWN_PID; GET DEVICE_INFO mit 1 Byte PD → NACK FORMAT_ERROR |
| `RDM_GET_PARAMETER_DESCRIPTION.json` | GET für 0x8000 und die 49-Byte-Antwort der Tube-App |
| `RDM_QUEUED_MESSAGE.json` | GET QUEUED_MESSAGE (ADVISORY) → leere STATUS_MESSAGES-Antwort |
| `RDM_TUBE_BOOT_INFO.json` | GET 0x8010 → 19-Byte-BOOT_INFO von tube_boot_rdm |

Controller-UID 7FF0:A0000001 und Binding-UIDs sind synthetisch. Erzeugt mit einem Python-Skript, das nur die
Standardbibliothek nutzt. Geprüft wurden JSON-Format, Schlüssel nach `schema.json`, Länge, Prüfsumme wie in
`rdm.c` sowie ein Rebuild und Parse mit `flash_rdm.rdm_build`/`rdm_parse`/`dub_encode`/`dub_decode`. Ergebnis:
0 Fehler.

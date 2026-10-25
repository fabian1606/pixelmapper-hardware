# Pixelmapper Node – Protokollvertrag für Pixelmapper

Stand: 2026-10-25 (Phase 0, gegen die Art-Net-4-Spec Rev 1.4dp geprüft; Phase-2-Nachträge in §12.2). Für alle, die das Pixelmapper-Plugin bauen,
das mit dem Pixelmapper Node und den LED-Tubes spricht, und für die Firmware/`pixelmapper-net`.
Gesamtplan: [docs/plan.md](../docs/plan.md). UI am Gerät: [docs/ui-konzept.md](../docs/ui-konzept.md).
Geprüfte Auszüge: [artnet4-digest.md](artnet4-digest.md), [rdm-digest.md](rdm-digest.md).
Zahlen (PIDs, Konstanten, Fehlercodes): [pids.toml](pids.toml). Images: [image-format.md](image-format.md).

**Verbindlich sind die Spezifikationen** (Art-Net 4 von Artistic Licence, ANSI E1.20 RDM, E1.37-1/-2). Dieses Dokument
legt fest, **welche Teile wir nutzen, wie sich der Node verhält und was Pixelmapper tun muss**.

Kennzeichnung: **[FEST]** ist entschieden · **[VORLÄUFIG]** ist ein Vorschlag · **[OFFEN]** ist noch nicht entschieden.
**[KORR]** markiert Korrekturen aus der Spec-Prüfung von Phase 0 und aus der Umsetzung in Phase 1/2 (Liste in §12).
Was hier über das Verhalten des Nodes steht, ist das Soll; Firmware (`node/firmware`) und der simulierte Node in
`pixelmapper-net` (`sim::node`) setzen es um.

---

## 1. Grundsätze

- **[FEST] Nur Art-Net 4**, kein sACN. sACN kann später als weitere Datenquelle für einen Port dazukommen.
- **[FEST] Zwei Mechanismen, nicht mehr:**
  1. **Art-Net-Standard-Opcodes** für alles, was Art-Net vorsieht (Discovery, DMX, Node-Konfiguration, Firmware des Nodes).
  2. **RDM** (über `ArtRdm`) für alles andere: angeschlossene Geräte **und** der Node selbst (eigener RDM-Responder).
  `ArtCommand` und `ArtDataRequest` werden **nicht** für die Konfiguration verwendet.
- **[FEST] Einstellungsseiten generisch erzeugen** aus `SUPPORTED_PARAMETERS` + `PARAMETER_DESCRIPTION`. Hart codieren
  darf Pixelmapper nur, was in `protocol/pids.toml` steht. Strukturierte Hersteller-PIDs (Data Type `not_defined`,
  z. B. BOOT_CATCH, FW_*) erscheinen nicht als generische Felder. Der Tube-Bootloader veröffentlicht seine PIDs absichtlich nicht.
- **[FEST] Wer zuletzt schreibt, gewinnt.** Keine Sperren, Versionsnummern oder Konfliktdialoge. Jede Änderung wird sofort
  gemeldet (§7). Pixelmapper führt seinen Zustand aus den Meldungen nach, nicht aus dem, was es gesendet hat.
- **[FEST] Keine Authentifizierung** (Art-Net und RDM haben keine). Wer Zugang zum Netz hat, gilt als vertrauenswürdig.

## 2. Transport

| | |
|---|---|
| Protokoll | UDP, Port **6454** (0x1936), Quelle und Ziel. Header `"Art-Net\0"`, OpCode little-endian, ProtVer 14. Achtung **[KORR]**: fast alle anderen Mehrbyte-Felder sind big-endian, Ausnahmen `ArtPollReply.Port` und `ArtPollReply.EstaMan` (beide LE) – digest §0 |
| Netz | **[FEST]** Der Node startet mit **DHCP**. Ohne Antwort nach ca. 5 s nimmt er **2.B.C.D / 255.0.0.0** nach Spec: **B = MAC[3] + OemHi + OemLo (mod 256), C = MAC[4], D = MAC[5]** **[KORR]**. Status2 meldet DHCP-fähig/aktiv. (Die Spec nennt Class A als Werkszustand und DHCP als Option; DHCP-zuerst ist zulässig.) Pixelmapper muss beides können: Broadcast an das eigene Subnetz **und** an 2.255.255.255, wenn ein Interface in 2.0.0.0/8 liegt. 255.255.255.255 nicht verwenden |
| Bandbreite | 100 Mbit. Später belegen Matrix-Ports viele Universes |

## 3. Discovery der Nodes

**Pixelmapper → `ArtPoll` (0x2000)** [FEST]
- Broadcast etwa alle 2,5–3 s; nach einem Poll bis zu 3 s auf Replies warten (Nodes antworten mit Zufallsverzögerung bis 1 s) **[KORR]**.
- **Flags Bit 1 setzen** („ArtPollReply senden, wenn sich der Zustand des Nodes ändert“, Spec bestätigt). Darauf baut
  „wer zuletzt schreibt, gewinnt“ auf. Targeted Mode (Bit 5) unterstützt der Node.

**Node → `ArtPollReply` (0x2100)** [FEST]
- **Nur Unicast** an den Poller **[KORR]**. Zufallsverzögerung bis 1 s.
- **[KORR] Ein Port pro `BindIndex`:** Der Node schickt **4 Replies (BindIndex 1–4)**, gleiche IP, gleicher LongName,
  je `NumPorts = 1`. Grund: In Art-Net 4 sind alle ArtAddress-Kommandos für Port 1–3 *deprecated*; nur so lassen sich
  Merge, Richtung, RDM und Output-Style pro Port setzen, und jeder Port bekommt sein eigenes Net/Sub-Net.
  Pixelmapper fasst Replies über **`DefaultRespUID`** (bzw. IP + MAC) zu einem Gerät zusammen und adressiert einen Port
  über **IP + BindIndex**. Im Matrix-Modus kommen weitere BindIndex-Seiten dazu.
- Felder:
  - `EstaMan` = 0x7FF0 (ESTA-Prototypbereich; im Reply **Lo-Byte zuerst**)
  - `Oem` = **0x00FF** (Platzhalter, §11) **[FEST für den Prototyp]**
  - Short Name (PortName, ≤ 17 Zeichen) / Long Name (≤ 63 Zeichen)
  - `VersInfo` = `(major << 8) | minor` der Firmware
  - Port-Address: NetSwitch/SubSwitch + SwOut bzw. SwIn
  - **Aktuelle Richtung steht in `PortTypes[]`** (Status3 Bit 3 gesetzt: Richtung umschaltbar) **[KORR]**
  - GoodInput (Bit 7 Daten, Bit 3 Input disabled), GoodOutputA (Bit 7 Ausgabe, Bit 3 Merge, Bit 1 LTP),
    GoodOutputB (Bit 7 RDM aus, Bit 6 continuous, Bit 5 Discovery läuft nicht, Bit 4 Background-Discovery aus)
  - Status1 Bit 1 RDM-fähig, Bit 7–6 Indikatoren (Locate/Mute/Normal); Status2 Bit 3 15-Bit, Bit 2/1 DHCP;
    Status3 Bit 7–6 Failsafe-Modus, Bit 5 programmierbares Failsafe, Bit 3 Richtung umschaltbar; Bit 4/2 (LLRP/RDMnet) = 0
  - `DefaultRespUID` = RDM-UID des Nodes (Spec: „RDMnet & LLRP Default Responder UID“; wir nutzen das Feld für die
    UID des eingebauten RDM-Responders, LLRP/RDMnet gibt es nicht) **[FEST]**
  - MAC, BindIp = eigene IP, `RefreshRate` = 44
  - **[KORR]** Nur das Feld der aktuellen Richtung trägt das Universe (SwOut bei Output, SwIn bei Input, das andere 0);
    ein Input-Port meldet GoodOutputB Bit 7 (RDM aus). `AcnPriority` = 0 (kein sACN, §1; ArtAddress-AcnPriority wird
    ignoriert). `NodeReport` `#xxxx [zzzz] Text`: Zähler zählt jedes ArtPollReply und beginnt nach einem Neustart neu
    (daran erkennt `pixelmapper-net` einen Neustart, §6.1); nach einem gescheiterten Update `#000e … Firmware update
    failed` bis zum Neustart
- „Kein Signal“ am Input: GoodInput Bit 7 **nicht** gesetzt (ein eigenes Bit gibt es nicht) **[KORR]**.
- **Ungefragte Replies:** bei jeder Änderung an Konfiguration oder Status (auch durch andere Clients, auch Merge-Beginn)
  per Unicast an alle Controller, die mit Flags Bit 1 gepollt haben.

**Identität [FEST]**
- Der Node wird über die **RDM-UID** (`DefaultRespUID`) identifiziert, nicht über die IP.
- Node-UID `7FF0:8xxxxxxx`: oberstes Bit gesetzt, darunter die unteren 28 Bit der MAC (`NODE_UID_DEVICE_FLAG`).
- Tubes: `7FF0:<32-Bit-ID>` (oberstes Bit kann gesetzt sein). Unterscheidung über **DEVICE_MODEL_ID** in DEVICE_INFO:
  Tube = 0x0001, **Node = 0x0002** (`MODEL_NODE`) **[FEST]**.

## 4. DMX-Daten

**Output-Port (Pixelmapper → Node)** [FEST]
- `ArtDmx` (0x5000) **per Unicast** an die IP des Nodes, ein Paket pro Universe. `Sequence` 1–255 (0 = aus), Länge
  gerade, 2–512.
- **[KORR] `Physical` pro Universe konstant halten:** Gleiche IP mit anderem `Physical` zählt als zweite Merge-Quelle.
- **Port-Address 0 ist in Art-Net 4 deprecated** **[KORR]**. Werkseinstellung der Ports: Net 0, Sub-Net 0,
  Universe 1–4 (Port-Address 1–4).
- Optional **`ArtSync`** (0x5200) nach jedem Frame-Satz. **[KORR] ArtSync muss per Broadcast gesendet werden** (Unicast
  ist laut Spec nicht erlaubt). Der Node ignoriert ArtSync, solange er auf einem Port **Streams verschiedener IPs**
  mergt (zwei Physicals derselben IP blockieren ArtSync nicht, digest §3.3) oder wenn es nicht von der IP des letzten
  ArtDmx kommt **[KORR]**. Fehlt der Sync länger als 4 s, gibt der Node wieder direkt aus.
- **Der Node sendet DMX dauerhaft** mit ca. 40 Hz (PORTn_DMX_RATE). Datenverlust: **nach 4 s ohne ArtDmx greift
  Failsafe** (unsere Festlegung, die Spec nennt keine Zeit), Standard Hold. Wer „Zero“ nutzt, sendet ≥ 1×/s.
- **Merge:** höchstens 2 Quellen pro Port, unterschieden nach **IP + Physical** **[KORR]**. Standard HTP, LTP per
  ArtAddress. Eine Quelle verfällt nach 10 s. Eine dritte Quelle wird verworfen. Fallen beide aus, bleibt das letzte
  Merge-Ergebnis stehen (dann Failsafe nach 4 s).

**Input-Port (Node → Pixelmapper)** [FEST]
- Steht ein Port auf Input, sendet der Node `ArtDmx` per **Unicast** an
  - alle Nodes, die das Universe als Output melden (der Node pollt dafür selbst alle ca. 3 s, Spec: „Abonnenten“), und
  - **jeden Controller, der den Node in den letzten ca. 10 s gepollt hat** (unsere Erweiterung, von der Spec nicht verboten).
- Bei Änderung, mindestens 1×/s (Spec-Empfehlung 800–1000 ms). Kein ArtDmx, solange seit dem Einschalten keine Daten kamen.
- **[KORR] Kein Broadcast.** Die Spec verbietet Broadcast von ArtDmx ausdrücklich („There are no conditions in which
  broadcast is allowed“). Die im Plan vorgesehene PID „Input-Broadcast“ entfällt.

## 5. Node-Konfiguration über Art-Net

Der Node antwortet mit `ArtPollReply` (bei ArtAddress/ArtInput, Unicast an den Absender) bzw. `ArtIpProgReply` und
schickt allen Pollern mit Flags Bit 1 ein ungefragtes Reply.

| Was | Opcode |
|---|---|
| Short/Long Name, Net/Sub/Uni, Merge HTP/LTP, Richtung Input/Output, RDM ein/aus, Output-Style continuous/delta, Failsafe (Hold/Zero/Full/Scene) + Szene aufnehmen, AcCancelMerge, AcClearOp, **LED Mute/Normal/Locate** | `ArtAddress` (0x6000), **adressiert über BindIndex = Port** |
| IP, Maske, Gateway, DHCP ein/aus, Reset auf Standard; **Abfrage** (Command = 0) | `ArtIpProg` (0xF800) → `ArtIpProgReply` (0xF900) |
| **DMX-Input** deaktivieren/aktivieren | `ArtInput` (0x7000) **[KORR]**: wirkt nur auf Input-Ports. Output-Ports schaltet `PORTn_ENABLED` (RDM) |

- Programmierregeln der Spec **[KORR]**: Bit 7 gesetzt = Wert übernehmen, **0x00 = zurück auf Werkseinstellung**
  („Hardware-Schalter“), 0x7F = keine Änderung (Konvention, digest §4.1), leere Namen = keine Änderung, AcnPriority 255 =
  keine Änderung. AcDirectionRx leert die Abonnentenliste.
- **Locate/Identify [FEST]:** `ArtAddress` LED Locate/Normal, RDM `IDENTIFY_DEVICE` an die Node-UID und die
  Identify-Funktion am Gerät schalten **denselben Zustand** (Spiegelung in alle Richtungen): Status1 Bit 7–6 im
  ArtPollReply (ungefragt an die Poller), GET IDENTIFY_DEVICE, Anzeige am Panel. Ändert ArtAddress oder das Panel den
  Zustand, legt der Node eine Queued Message IDENTIFY_DEVICE an (`pm_rdm_responder_set_identify`); ein RDM-SET
  IDENTIFY_DEVICE selbst erzeugt keine (der Absender kennt den Wert).
- **LED Mute** schaltet das Display ganz aus, bis zur nächsten Bedienung am Gerät oder bis LED Normal (Erweiterung). **[FEST]**
- **[KORR] `ArtIpProg` mit Command = 0 (Bit 7 „Programmieren“ aus) ist eine reine Abfrage:** der Node ändert nichts
  und antwortet mit IP, Maske, Gateway und DHCP-Status (ArtIpProgReply Status Bit 6). So liest Pixelmapper Maske und
  Gateway, die im ArtPollReply fehlen (`pixelmapper-net`: `Controller::ip_prog_query`, Bridge: `ipProg` nur mit
  `nodeId`). Programmieren: Bit 7 + DHCP (Bit 6, niedrigere Bits ignoriert) oder Reset (Bit 3, danach die
  gesetzten Werte) + IP (Bit 2, schaltet DHCP aus) / Maske (Bit 1) / Gateway (Bit 4). Ungültige Werte (IP 0.0.0.0 oder
  255.255.255.255, nicht zusammenhängende oder leere Maske) ignoriert der Node.
- IP-Änderung per `ArtIpProg` wirkt sofort; das ArtIpProgReply geht noch von der alten IP und enthält die neuen Werte
  (Spec schweigt, unsere Festlegung). Danach Replies von der neuen IP mit **derselben UID**.
- Die IP-Konfiguration ist zusätzlich per E1.37-2 erreichbar (§6.3), beide Wege ändern dieselben Einstellungen.
  **[KORR] Unterschied:** SETs von IPV4_DHCP_MODE, IPV4_STATIC_ADDRESS und IPV4_DEFAULT_ROUTE werden sofort
  gespeichert (Queued Message), **wirksam werden sie erst mit INTERFACE_APPLY_CONFIGURATION** (E1.37-2); ArtIpProg
  wirkt sofort.

## 6. RDM über Art-Net

### 6.1 Geräteliste (TOD) [FEST]

| Richtung | Opcode | Bedeutung |
|---|---|---|
| Pixelmapper → Node | `ArtTodRequest` (0x8000) | TOD für die angegebenen Port-Adressen anfordern (startet **keine** Discovery) |
| Pixelmapper → Node | `ArtTodControl` (0x8200) | `AtcFlush` (0x01): TOD löschen + volle Discovery · `AtcEnd` (0x02) · `AtcIncOn`/`AtcIncOff` (0x03/0x04): Hintergrundsuche des Ports ein/aus **[KORR]** |
| Node → Pixelmapper | `ArtTodData` (0x8100) | UID-Liste, **höchstens 200 UIDs pro Paket** (`UidTotal`, `BlockCount`) **[KORR]**; `CommandResponse` 0xFF = TodNak (TOD nicht verfügbar/unvollständig) |

- **[KORR] Ungefragte `ArtTodData`** gehen per Unicast an **alle IPs, die vorher ArtTodData angefordert haben**
  (ArtTodRequest), nicht an alle Poller. Pixelmapper sendet deshalb nach dem Finden eines Nodes einmal `ArtTodRequest`
  pro Port (und nach einem Neustart des Nodes erneut).
- Volle Discovery beim Start, beim Aktivieren eines Ports und bei `AtcFlush`: TOD leeren, 3× DISC_UN_MUTE an
  Broadcast, binäre Suche mit DISC_UNIQUE_BRANCH über den ganzen UID-Raum, jeder Fund wird per DISC_MUTE bestätigt.
- Danach **Hintergrundsuche alle ca. 3 s** (`RDM_BG_DISCOVERY_INTERVAL`, pro Port `PORTn_RDM_BG_DISCOVERY` bzw.
  AtcIncOn/Off) **[KORR]**:
  1. **3× DISC_UN_MUTE an Broadcast**,
  2. **Präsenzprüfung:** DISC_MUTE an jede bekannte UID (das muted sie zugleich); nach 3 Fehlversuchen in Folge fällt
     ein Gerät aus der TOD,
  3. **DUB über den ganzen Raum:** antworten können nur noch Geräte, die nicht in der TOD stehen (neue oder
     zurückgekehrte). Die TOD wird nicht geleert.

  Abweichung vom Plan („DUB ohne Un-Mute“): Ohne Un-Mute bleibt ein Gerät, das einmal gemutet war und dann aus der TOD
  fiel (Wackelkontakt, drei verpasste Präsenzprüfungen) oder dessen Mute-Zustand ein anderer Controller gesetzt hat,
  gemutet und damit für jede Hintergrundsuche unsichtbar – bis zum nächsten Flush. Ein Gerät nach Reset/Wiederein-
  stecken startet zwar ungemutet, aber das Un-Mute macht die Suche unabhängig davon, in welchem Zustand Geräte auf der
  Leitung sind (OLA macht es genauso; `pm_rdm_ctrl.c`). Kosten: drei Broadcasts pro Lauf. Während BOOT_CATCH läuft,
  startet keine Hintergrundsuche (sie würde das ~200-ms-Fenster der Tube verfehlen).
- Bis zu **512 Geräte pro Port**. Port ohne RDM (aus, Input oder RDM aus): `ArtTodData` mit **TodNak** und ohne UIDs
  **[KORR]**.

### 6.2 RDM-Anfragen [FEST]

- **`ArtRdm` (0x8300)** per Unicast an die IP des Nodes, adressiert über Net + Address (Port-Address) bzw. BindIndex.
  Nutzlast = RDM-Paket **ohne** Start Code 0xCC (Spec bestätigt).
- **Jede Anfrage wird weitergeleitet**, auch an UIDs außerhalb der TOD, an Broadcasts und Herstellerbroadcasts.
- **Antworten** als `ArtRdm` per Unicast an den Absender. **Keine Antwort → keine Meldung.** Pixelmapper braucht einen
  eigenen Timeout: **[VORLÄUFIG]** 500 ms, 2 Wiederholungen, höchstens 4 offene Anfragen pro Port.
- **[KORR]** Der Node meldet seine Queue in `FifoAvail`/`FifoMax` (ArtRdm Offset 19/20, in Antworten). Pixelmapper soll
  nicht mehr Anfragen schicken, als `FifoAvail` erlaubt. **[FEST] Phase 2:** 8 Plätze pro Port (`PM_RDM_QUEUE_LEN`
  = FifoMax). Ist die Queue voll, verwirft der Node die Anfrage ohne Antwort (Pixelmapper läuft in seinen Timeout).
  Jede weitergeleitete Anfrage bekommt intern ein Token (gemerkt mit Absender-IP und Port), über das die Antwort
  genau an den Absender geht; andere Controller sehen sie nicht.
- Mehrere Anfragen an verschiedene Ports laufen parallel. ACK_TIMER, ACK_OVERFLOW, QUEUED_MESSAGE reicht der Node unverändert durch.
- **`ArtRdmSub` (0x8400): [FEST] nicht unterstützt.** Die Spec verlangt es nicht (es ist nur eine Ergänzung zu ArtRdm);
  weder Tubes noch Node haben Sub-Devices. Der Node ignoriert ArtRdmSub-Pakete.

### 6.3 Der Node als RDM-Gerät [FEST]

- Erreichbar per `ArtRdm` an **seine UID**, mit der Port-Address eines beliebigen seiner Ports. Die Anfrage geht nicht
  auf die DMX-Leitung. DEVICE_INFO: Modell 0x0002, Kategorie 0x0801 (DATA_DISTRIBUTION), Footprint 0, Startadresse
  0xFFFF, 0 Sub-Devices.
- **Standard-PIDs** (Liste in `pids.toml` → `[node.standard_pids]`):
  - Pflicht (nicht in SUPPORTED_PARAMETERS): DEVICE_INFO, SUPPORTED_PARAMETERS, PARAMETER_DESCRIPTION,
    SOFTWARE_VERSION_LABEL, IDENTIFY_DEVICE
  - E1.20: QUEUED_MESSAGE, STATUS_MESSAGES, STATUS_ID_DESCRIPTION, CLEAR_STATUS_ID, PRODUCT_DETAIL_ID_LIST
    (ETHERNET_NODE), DEVICE_MODEL_DESCRIPTION ("Pixelmapper Node"), MANUFACTURER_LABEL ("Prototype", wie die Tube),
    DEVICE_LABEL (= Short Name; längere Labels werden auf 17 Zeichen gekürzt), FACTORY_DEFAULTS (= Werksreset),
    BOOT_SOFTWARE_VERSION_ID/LABEL (ESP-IDF-Version des installierten Bootloaders: `0x00MMmmpp`,
    „ESP-IDF v5.5.6 bootloader“) **[KORR]**, **DISPLAY_LEVEL** (= Display-Helligkeit), RESET_DEVICE
  - E1.37-2: LIST_INTERFACES, INTERFACE_LABEL, INTERFACE_HARDWARE_ADDRESS_TYPE1, IPV4_DHCP_MODE, IPV4_CURRENT_ADDRESS,
    IPV4_STATIC_ADDRESS, INTERFACE_RENEW_DHCP, INTERFACE_APPLY_CONFIGURATION, IPV4_DEFAULT_ROUTE. **[KORR]**
    IPV4_STATIC_ADDRESS meldet ohne gesetzte Adresse die Art-Net-Ersatzadresse 2.B.C.D (§2), die eine statische
    Konfiguration dann nutzt; IPV4_DEFAULT_ROUTE ohne Gateway = Interface 0 + 0.0.0.0
  - **Nicht** genutzt: E1.37-1 LOCK_PIN/LOCK_STATE (verlangt PIN und sperrt RDM-SETs mit WRITE_PROTECT – unsere
    Panelsperre sperrt nur das Bedienfeld und lässt sich am Gerät ohne PIN lösen), DMX_START_ADDRESS (Footprint 0),
    die Dimmer-PIDs aus E1.37-1, DNS-PIDs aus E1.37-2.
- **Hersteller-PIDs des Nodes** (0x8100–0x81FF, vollständig in `pids.toml`, alle mit PARAMETER_DESCRIPTION):

| PID | Name | CC | Wert |
|---|---|---|---|
| 0x8100 | NODE_HW_REVISION | GET | u8 |
| 0x8101–0x8104 | PORT1..4_ENABLED | GET/SET | bool |
| 0x8110–0x8113 | PORT1..4_DMX_BREAK | GET/SET | u16 µs, 176–352, Standard 176 |
| 0x8114–0x8117 | PORT1..4_DMX_MAB | GET/SET | u16 µs, 12–88, Standard 16 |
| 0x8118–0x811B | PORT1..4_DMX_RATE | GET/SET | u8 Hz, 1–44, Standard 40 |
| 0x811C–0x811F | PORT1..4_DMX_MBS | GET/SET | u16 µs, 0–100, Standard 0 (Mark between Slots; Semantik auf Hardware zu bestätigen) |
| 0x8120–0x8123 | PORT1..4_RDM_BG_DISCOVERY | GET/SET | bool (= AtcIncOn/Off) |
| 0x8124 | RDM_BG_DISCOVERY_INTERVAL | GET/SET | u16 s, 1–600, Standard 3 |
| 0x8130 | DISPLAY_DIM_TIMEOUT | GET/SET | u16 s, 0 = nie dimmen, Standard 60 |
| 0x8131 | DISPLAY_ADDRESS_FORMAT | GET/SET | u8: 0 Net:Sub:Uni, 1 15 Bit |
| 0x8132 | PANEL_LOCK | GET/SET | bool |
| 0x8140 | BOOT_CATCH_START | SET | Port u8 + UID 6 B |
| 0x8141 | BOOT_CATCH_STATUS | GET | Zustand, Port, UID, ms, Versuche |
| 0x8142 | BOOT_CATCH_ABORT | SET | — |
| 0x8150 | LOCAL_RDM_SET | GET | Sequenz, Port, UID, Sub-Device, PID |
| 0x8180–0x8183 | FW_INFO / FW_BEGIN / FW_WRITE / FW_FINISH | GET/SET | §9.2 |

- **Änderungsmeldungen per RDM:** Ändert sich eine nur per RDM sichtbare Einstellung, legt der Node eine **Queued
  Message** an (Message Count in jeder Antwort des Nodes). Inhalt = GET-Antwort der geänderten PID. Pixelmapper holt sie
  mit `QUEUED_MESSAGE` (Status-Typ GET_LAST_MESSAGE/ADVISORY). Ist nichts mehr da, antwortet der Node mit leerem STATUS_MESSAGES.
- **[KORR] Eine Queue für alle Controller:** Der Node hat **eine** Queued-Message-Queue (16 PIDs + 8 Statusmeldungen),
  nicht eine pro Controller (E1.20 sieht nichts anderes vor). Wer zuerst abholt, leert sie. Pixelmapper darf sich
  deshalb nicht darauf verlassen, jede Änderung als Queued Message zu sehen (OLA, DMX-Workshop oder eine zweite
  Pixelmapper-Instanz können sie abholen) und liest **zusätzlich nach**: bei jedem ungefragten ArtPollReply des Nodes
  und regelmäßig (30–60 s) die Einstellungs-PIDs. Eine PID steht höchstens einmal in der Queue (eine erneute
  Änderung schiebt sie ans Ende, Inhalt = GET-Antwort zum Abholzeitpunkt); ist die Queue voll, fällt die älteste
  Meldung weg.
- **Panelsperre am Gerät aufgehoben:** Queued Message PANEL_LOCK + Statusmeldung `PANEL_UNLOCKED_LOCAL` (0x8003).

### 6.4 `BOOT_CATCH` (Rettung einer Tube übers Netzwerk) [FEST]

- SET `BOOT_CATCH_START` (0x8140) an die **UID des Nodes**: Port (u8, 1–4) + Ziel-UID (6 B). NACK DATA_OUT_OF_RANGE bei
  ungültigem Port, Port aus/Input oder Broadcast-UID. Ein laufender Catch wird ersetzt.
- Der Node schickt dann bis zu 60 s lang alle 20 ms (`BOOT_CATCH_DURATION_MS`, `BOOT_CATCH_PERIOD_MS`) lokal
  GET `BOOT_INFO` (0x8010) an die Ziel-UID und trifft so nach einem Power-Cycle das Rettungsfenster der Tube (26–225 ms).
  DMX auf dem Port läuft dabei weiter (zwischen den Anfragen).
- Antwortet die Tube: Zustand CAUGHT, Queued Message = GET-Antwort `BOOT_CATCH_STATUS`, dazu Statusmeldung
  `BOOT_CATCH_CAUGHT` (0x8001, Data1 = Port). Bei Ablauf: TIMEOUT + `BOOT_CATCH_TIMEOUT` (0x8002). Pixelmapper macht
  nach CAUGHT mit dem normalen Update weiter (§9.1 ab Schritt 4).
- Abbruch: SET `BOOT_CATCH_ABORT` (0x8142). Zustand jederzeit: GET `BOOT_CATCH_STATUS` (0x8141).

## 7. Wie Pixelmapper von Änderungen erfährt

| Änderung | Meldung |
|---|---|
| Konfiguration des Nodes (Art-Net-Felder) | ungefragtes `ArtPollReply` (ArtPoll Flags Bit 1 nötig) |
| Konfiguration des Nodes (nur per RDM) | Queued Message an der Node-UID (eine Queue für alle Controller, §6.3: zusätzlich nachlesen) |
| IP des Nodes | `ArtPollReply` von der neuen IP mit derselben UID |
| Geräte kommen dazu oder fallen weg | ungefragtes `ArtTodData` (an IPs, die ArtTodRequest geschickt haben) |
| **RDM-SET am Node-Display an ein angeschlossenes Gerät** | **[FEST]** Queued Message an der Node-UID = GET-Antwort `LOCAL_RDM_SET` (0x8150: Sequenz, Port, UID, Sub-Device, PID) + Statusmeldung `LOCAL_RDM_SET` (0x8004). Pixelmapper liest daraufhin genau diese PID am Gerät nach. Zusätzlich empfohlen: DEVICE_INFO/Labels bei TOD-Änderungen und alle 30–60 s nachlesen (Tubes haben keine Queued Messages) |
| Datenverlust/Failsafe, Signal am Input, Merge | Statusbits im `ArtPollReply` (ungefragt) |

## 8. Gemeinsame Definitionen (`protocol/`)

- `pids.toml` – einzige Quelle aller Hersteller-PIDs, Statusmeldungen und Konstanten. `python3 protocol/gen.py` erzeugt
  `node/firmware/components/pm_protocol/include/pm_pids.h` und `crates/pixelmapper-net/src/pids_generated.rs`;
  `--check` prüft zusätzlich `tube/firmware/include/rdm_boot.h` (bleibt handgeschrieben, `flash_rdm.py` parst ihn).
- `image-format.md` – Images von Tube und Node.
- `vectors/*.json` – Referenzpakete; C-Hosttests und Rust-Tests laufen gegen alle Dateien.
- `artnet4-digest.md`, `rdm-digest.md` – geprüfte Auszüge mit Seitenangaben/Quellen.

Die Crate **`crates/pixelmapper-net`** setzt dieses Dokument um (`pmnet` als Kommandozeile). **Das Plugin soll
`pixelmapper-net` verwenden und nicht selbst Pakete bauen.**

## 9. Firmware-Updates

### 9.1 Tubes [FEST] (unverändert)

Quelle: `tube/firmware/include/rdm_boot.h`, `tube/firmware/README.md`, Referenz `tube/tools/flash_rdm.py`.
Der Node reicht nur `ArtRdm` durch. **Hersteller und Modell prüft Pixelmapper vor dem Senden.**

Image: Intel-HEX mit 16-Byte-Trailer bei 0x17F0, siehe [image-format.md §1](image-format.md).

| PID | CC | Daten (BE) |
|---|---|---|
| 0x8000 ENTER_BOOTLOADER | SET (App) | PD `"BOOT"` → ACK, Reset in den Bootloader; per Broadcast nicht ausgeführt |
| 0x8010 BOOT_INFO | GET | → 19 B (Layout in `pids.toml`) |
| 0x8011 BOOT_WRITE_PAGE | SET | Adresse u16 + 64 B → ACK; NACK BUFFER_FULL = erneut senden |
| 0x8012 BOOT_APP_CRC | GET | Adresse u16 + Länge u16 (≤ 256) → CRC16 u16 |
| 0x8013 BOOT_FINISH | SET | → ACK, Trailer-Prüfung → App, sonst bleibt er (letzter Fehler 2/3/4) |

Ablauf: Trailer prüfen → DEVICE_INFO der App (Hersteller/Modell; Downgrade nur nach Bestätigung) → SET 0x8000 →
BOOT_INFO pollen, Hersteller/Modell erneut prüfen → Seiten schreiben (0xFF-Seiten überspringen; Trailer-Seite zuerst mit
leerem, ganz zum Schluss mit echtem Trailer) → APP_CRC in 256-B-Stücken vergleichen, Abweichungen einmal neu schreiben →
FINISH → nach ca. 3 s DEVICE_INFO/SOFTWARE_VERSION_LABEL prüfen.
Bootloader: gleiche UID wie die App, Boot-Loader-Flag 0x0004 in DISC_MUTE-Antworten, nach 60 s ohne Bootloader-PID
startet die App, PIDs nicht in SUPPORTED_PARAMETERS. **Rettung:** `BOOT_CATCH` (§6.4) + Power-Cycle, weiter ab BOOT_INFO.

### 9.2 Node [FEST]

**Weg A: `ArtFirmwareMaster` (0xF200) → `ArtFirmwareReply` (0xF300)**, Unicast. Typen FirmFirst/FirmCont/FirmLast
(0x00/0x01/0x02), Antworten FirmBlockGood/FirmAllGood/FirmFail (0x00/0x01/0xFF) – Spec bestätigt. **[KORR]** Datei im
Art-Net-`.alf`-Format (1060-Byte-Header mit OEM-Liste), 1024 Byte Daten pro Block, BlockId modulo 256, je 30 s Timeout
auf beiden Seiten. Unser Beschreibungsblock steckt im Spare-Bereich des Headers und wird so **im ersten Block** geprüft.

**Weg B: RDM an die Node-UID** – FW_INFO (0x8180), FW_BEGIN (0x8181, PD = 64-Byte-Beschreibungsblock), FW_WRITE
(0x8182, Offset u32 + ≤ 224 B), FW_FINISH (0x8183). PDL und Fehlercodes in `pids.toml`. **[KORR]** FW_WRITE mit
Lücke (Offset > empfangen) → NACK DATA_OUT_OF_RANGE, die Sitzung läuft weiter (ab `received` neu senden), FW_INFO
unverändert; FW_WRITE/FW_FINISH ohne Sitzung → NACK, FW_INFO unverändert.

**Für beide Wege:** Prüfung vor dem ersten Schreibzugriff (OEM-Liste bei Art-Net, ESTA-Hersteller, DEVICE_MODEL_ID,
HW-Revision, Größe; bei Art-Net zusätzlich `FirmwareLength` = 530 + ceil(image_length / 2), sonst `DESC_MISMATCH`
**[KORR]**), dann Abgleich mit dem Block im Image, CRC-32, ESP-IDF-Imageprüfung, Neustart, **Rollback** wenn
sich die neue Firmware nicht innerhalb der ersten Minute als gültig markiert. **Keine Signatur.** Format und Ablauf:
[image-format.md §2](image-format.md). Pixelmapper prüft vorher selbst Hersteller, Modell und HW-Revision
(`NODE_HW_REVISION`). **Fehler schon im ersten Block** (FW_BEGIN-NACK bzw. FirmFail auf FirmFirst) stehen ebenfalls in
`FW_INFO.last_error` (Zustand FAILED), auch wenn noch keine Sitzung lief – Pixelmapper liest nach jedem Fehler FW_INFO.

## 10. Für das Plugin schon jetzt sicher umsetzbar

1. Socket auf 6454, Broadcast auch in 2.x.x.x
2. `ArtPoll` mit Flag Bit 1; `ArtPollReply` (4 BindIndex-Seiten pro Node) über die UID zusammenführen
3. `ArtDmx` per Unicast mit Sequence und konstantem Physical, optional `ArtSync` per Broadcast
4. `ArtAddress` (pro BindIndex), `ArtIpProg`, `ArtInput`
5. TOD: `ArtTodRequest` (auch, um ungefragte ArtTodData zu abonnieren), `ArtTodControl`, `ArtTodData` (mehrere Pakete, TodNak)
6. `ArtRdm` mit eigenem Timeout, FifoAvail beachten, ACK_TIMER, ACK_OVERFLOW, Queued Messages
7. Generische Einstellungsseiten aus SUPPORTED_PARAMETERS und PARAMETER_DESCRIPTION
8. Tube-Update nach 9.1 (`tube/tools/flash_rdm.py --selftest` enthält einen simulierten Bootloader)
9. Last-Writer-Wins
10. Hersteller-PIDs des Nodes, BOOT_CATCH, Node-Update über RDM und `.alf` (Nummern und Formate stehen jetzt fest)

## 11. Entscheidungen aus Phase 0 (vorher [OFFEN])

| Punkt | Entscheidung | Begründung |
|---|---|---|
| DEVICE_MODEL_ID des Nodes | **0x0002** | eindeutig neben der Tube (0x0001), gleicher ESTA-Bereich |
| Art-Net-OEM-Code (Prototyp) | **0x00FF** (OemUnknown) als Platzhalter | Die Spec kennt keinen Prototyp-Code und verlangt Registrierung; 0x00FF ist Artistic Licence „Unknown“ ohne Produktbezug. Vor Weitergabe eigenen Code beantragen (kostenlos), ebenso eigene ESTA-ID |
| `ArtRdmSub` | nicht unterstützt | optional laut Spec, keine Sub-Devices bei Node und Tubes |
| E1.37-PIDs des Nodes | E1.37-2 IPv4-Satz (§6.3); E1.37-1 keine | IP-Konfiguration über Standard-PIDs für fremde Controller; LOCK_* passt nicht (PIN, sperrt RDM) |
| Display-Helligkeit, Werksreset | Standard-PIDs DISPLAY_LEVEL, FACTORY_DEFAULTS statt Hersteller-PIDs | Plan: „wo Standard-PIDs passen, werden sie genommen“ |
| Input-Broadcast-PID | **entfällt** | Spec verbietet Broadcast von ArtDmx |
| Meldung von RDM-SETs am Node-Display | Queued Message `LOCAL_RDM_SET` + Status 0x8004 | nutzt den vorhandenen Queued-Message-Weg, Pixelmapper muss nichts pollen |
| PID-Nummern/PDL Node, BOOT_CATCH, FW_* | `pids.toml` | |
| Dateiformat Node-Image | `image-format.md` §2 | |
| Ports im ArtPollReply | 1 Port pro BindIndex | Art-Net 4: ArtAddress für Port 1–3 deprecated |

## 12. Korrekturen

### 12.1 Spec-Prüfung Phase 0 (gegenüber dem Vertrag vom 2026-10-24 aus dem Gedächtnis)

Bestätigt: alle Opcodes; ArtPoll Flags Bit 1; ArtSync-Timeout 4 s; Merge-Timeout 10 s, 2 Quellen, HTP-Standard;
ArtRdm ohne Start Code; AtcFlush = 0x01; FirmFirst/Cont/Last und FirmBlockGood/AllGood/Fail; Input-Keepalive 1×/s.

Geändert (oben mit [KORR] markiert):
1. Input-Broadcast verboten → PID entfällt.
2. `Physical` gehört zur Merge-Quelle (IP + Physical).
3. ArtPollReply nur Unicast, Zufallsverzögerung bis 1 s; Controller wartet bis 3 s.
4. ArtSync nur als Broadcast; wird beim Mergen von Streams verschiedener IPs und von fremder IP ignoriert.
5. Ein Port pro BindIndex statt 4 Ports in BindIndex 1.
6. Aktuelle Richtung in `PortTypes[]`, nicht in GoodInput/GoodOutput; „kein Signal“ = GoodInput Bit 7 aus.
7. ArtInput wirkt nur auf Inputs.
8. Port-Address 0 deprecated → Werkseinstellung Universe 1–4.
9. Fallback-IP nach Spec-Formel (MAC + OEM).
10. Ungefragte ArtTodData nur an Anforderer; max. 200 UIDs pro Paket; TodNak; AtcEnd/AtcIncOn/AtcIncOff.
11. ArtRdm FifoAvail/FifoMax.
12. ArtRdmSub ist optional (nicht „laut Spec zu verarbeiten“).
13. Firmware: `.alf`-Header mit OEM-Liste, 30-s-Timeouts, 1024-B-Blöcke, BlockId 8 Bit.
14. Datenverlust-Zeit (4 s) ist unsere Festlegung, nicht Spec.
15. `DefaultRespUID` ist in der Spec die LLRP/RDMnet-Responder-UID; wir verwenden sie für den eingebauten Responder.

### 12.2 Nachträge aus Phase 1/2 (Umsetzung, 2026-10-25)

16. Hintergrundsuche = 3× DISC_UN_MUTE (Broadcast) + Präsenzprüfung per DISC_MUTE + DUB über den ganzen Raum (§6.1),
    statt „DUB ohne Un-Mute“ aus dem Plan.
17. Eine Queued-Message-Queue für alle Controller; Pixelmapper liest zusätzlich nach (§6.3, §7).
18. `ArtIpProg` Command = 0 ist eine reine Abfrage (Maske/Gateway lesen), §5.
19. E1.37-2-IP-SETs werden erst mit INTERFACE_APPLY_CONFIGURATION wirksam (ArtIpProg sofort), §5.
20. Identify-Spiegelung ArtAddress Locate ↔ IDENTIFY_DEVICE ↔ Panel, mit Queued Message bei Änderung über
    ArtAddress/Panel (§5).
21. Neue PIDs PORT1..4_DMX_MBS (0x811C–0x811F), §6.3.
22. ArtRdm: FifoMax = 8 pro Port, volle Queue = keine Antwort (§6.2).
23. `.alf`: FirmwareLength ≠ image_length → `DESC_MISMATCH`; Fehler im ersten Block in FW_INFO.last_error (§9.2,
    image-format.md §2.3/§2.5).
24. Statusmeldungs-Namen und -Texte werden aus `pids.toml` generiert (`pids::status_name`, `PM_STATUS_LIST`).

### 12.3 Nachträge aus dem Ende-zu-Ende-Test (Phase 3, `docs/e2e-findings.md`)

25. ArtSync wird nur beim Mergen von Streams **verschiedener IPs** ignoriert (§4, digest §3.3).
26. Pixelmapper fordert die TODs nach einem Neustart des Nodes erneut an; erkannt am NodeReport-Zähler (§3, §6.1).
27. ArtPollReply: SwIn/SwOut nur für die aktuelle Richtung, Input-Port meldet RDM aus, AcnPriority 0 (§3).
28. Port ohne RDM: ArtTodData mit TodNak (§6.1).
29. MANUFACTURER_LABEL „Prototype“, BOOT_SOFTWARE_VERSION_ID/LABEL aus dem Bootloader, IPV4_STATIC_ADDRESS
    ohne Adresse = 2.B.C.D, IPV4_DEFAULT_ROUTE ohne Gateway = 0/0 (§6.3).
30. FW_WRITE-Lücke und Anfragen ohne Sitzung lassen FW_INFO unverändert (§9.2).
31. Failsafe-Modus pro Port (ArtAddress über BindIndex, Status3 je Reply).

## 13. Offene Punkte

- ESTA-Normtexte (E1.20-2025, E1.37-1/-2) liegen nicht vor; der RDM-Digest stützt sich auf OLA/Ja Rule und ist dort
  mit [ungeprüft] markiert. Vor der Abnahme gegen die PDFs prüfen (Download bei tsp.esta.org nach Registrierung).
- Art-Net-Spec mehrdeutig: Blockgröße ArtFirmwareMaster, `.alf`-Checksumme, BlockId-Überlauf → mit DMX-Workshop testen.
- Verhalten von DMX-Workshop bei 4 BindIndex-Seiten und bei FirmFail prüfen (Abnahme).
- Matrix-Modus (mehrere Universes pro Port) – später.
- PORTn_DMX_MBS: Wirkung (UART-Idle zwischen Slots) auf der Hardware messen (`pm_port/MEASURE.md` M5), danach
  Bereich/Beschreibung in `pids.toml` bestätigen.
- Hintergrundsuche mit Un-Mute: auf der Hardware prüfen, dass die DMX-Rate ≥ 30 Hz bleibt und ein zweiter
  RDM-Controller am selben Strang nicht stört (das Un-Mute hebt auch dessen Mutes auf).
- Ende-zu-Ende-Test `pixelmapper-net` gegen den virtuellen Node: `tools/e2e.sh`, Abweichungen in
  `docs/e2e-findings.md`. Der virtuelle Node simuliert die RS-485-Leitung; Zeitverhalten am echten Bus bleibt auf der
  Hardware zu prüfen.

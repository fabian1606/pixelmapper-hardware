# Art-Net 4 – verifizierter Digest für den Pixelmapper Node

**Quelle:** *Art-Net 4 – Specification for the Art-Net 4 Ethernet Communication Protocol*, Artistic Licence,
„Art-Net 4 Protocol Release V1.4, **Document Revision 1.4dp 23/10/2025**“ (109 Seiten A5, PDF erstellt 23.10.2025).
Download am 2026-10-24 von <https://art-net.org.uk/downloads/art-net.pdf>. Seitenangaben „S. n“ = Seitenzahl im
Fußtext des PDFs (`- n -`), nicht die PDF-Seitennummer.

Weitere Primärquellen (nur wo ausdrücklich genannt):
- **[OEM-Liste]** <https://art-net.org.uk/oem-code-zone/> (Tabelle mit Stand „2026-09-24“, abgerufen 2026-10-24).
- **[ESTA]** ESTA TSP Manufacturer-ID-Liste <https://tsp.esta.org/tsp/working_groups/CP/mfctrIDs.php> (abgerufen 2026-10-24).

Alles in diesem Dokument ist aus diesen Quellen gelesen. Wo die Spezifikation schweigt oder mehrdeutig ist, steht
**[Spec schweigt]** bzw. **[mehrdeutig]** und unsere Festlegung ist als **Festlegung** markiert – das ist dann
kein Spec-Inhalt.

Testvektoren: [`vectors/artnet/*.json`](vectors/artnet/) (erzeugt mit einem stdlib-Python-Skript, jede Datei gegen
Layout und Schema geprüft).

---

## 0. Allgemeine Regeln (S. 12, 16, 96)

- Nur UDP, Quell- und Zielport **0x1936 (6454)** (S. 10, 12).
- Alle Felder sind vorzeichenlose Int8/Int16/Int32, **keine versteckten Padding-Bytes**, außer am Paketende
  (Aufrundung auf 2 oder 4 Byte erlaubt). **Zusätzliche Bytes am Ende eines gültigen Pakets werden ignoriert.**
  Es wird **nur eine Mindestlänge** geprüft (S. 12).
- Unbenutzte Bits: als 0 senden, beim Empfang **nicht testen** (S. 12).
- Datenintegrität: Empfänger sollen „ein Element prüfen: das Feld ID[8]“ (S. 96). Andere UDP-Pakete werden ignoriert (S. 16).
- **Endianness:** `OpCode` ist **little-endian** („Transmitted low byte first“). Fast alle anderen Mehrbyte-Felder sind
  als Hi/Lo-Bytepaare definiert, also **big-endian**. Ausnahmen (little-endian): `ArtPollReply.Port` (0x1936,
  „low byte first“) und **`ArtPollReply.EstaMan` (EstaManLo vor EstaManHi!)**. In `ArtPoll` steht EstaMan
  dagegen Hi zuerst. `ArtDmx`/`ArtNzs` haben `SubUni` (Low-Byte der Port-Address) vor `Net`.
- `ProtVerHi`/`ProtVerLo` = 0/14. „Controllers should ignore communication with nodes using a protocol version
  lower than 14“ (S. 18). `ArtPollReply` hat **kein** ProtVer-Feld.
- Header aller Pakete: `ID[8] = "Art-Net\0"` (Off 0–7), `OpCode` (Off 8–9, LE).

### Port-Address (S. 8–9)

| Bit 15 | Bits 14–8 | Bits 7–4 | Bits 3–0 |
|---|---|---|---|
| 0 | Net (7 bit) | Sub-Net (4 bit) | Universe (4 bit) |

- 128 Nets, 16 Sub-Nets je Net, 16 Universes je Sub-Net, 32 768 Port-Addresses.
- Zitat S. 9: „The Port-Address valid range is 1 – 32,767. **The value of zero is deprecated** to enhance sACN
  compatibility.“ (Revision DP: „Clarified that Port-Address = 0 is deprecated.“, S. 4)
- Pro Node (pro BindIndex) **ein** Net, **ein** Sub-Net, je Port ein Universe-Nibble (S. 9).

### IP-Adressen (S. 12–15)

- „By default, an Art-Net product will **factory start using a Class A IP address scheme**“ (S. 12); DHCP ist erlaubt
  und wird in `ArtPollReply.Status2` gemeldet (S. 13).
- Standard-Adresse: `A.B.C.D` mit A = 2 (Netzwerk-Schalter aus) bzw. 10 (an), **B = MAC[3] + OemHi + OemLo**,
  C = MAC[4], D = MAC[5], Maske 255.0.0.0 (S. 13–14; Beispiel MAC 12:45:78:98:34:76, OEM 0x0010 → 2.168.52.118).
  Die Spec sagt nicht, ob B modulo 256 gebildet wird **[Spec schweigt]**.
- Controller pollen per Default 2.255.255.255 und 10.255.255.255 (S. 15). Limited Broadcast 255.255.255.255
  „should not“ benutzt werden (S. 10) bzw. „Not recommended“ für ArtPoll (S. 16).

---

## 1. OpCodes (Table 1, S. 20–22)

| Name | Wert | | Name | Wert |
|---|---|---|---|---|
| OpPoll | 0x2000 | | OpRdm | 0x8300 |
| OpPollReply | 0x2100 | | OpRdmSub | 0x8400 |
| OpDiagData | 0x2300 | | OpVideoSetup / Palette / Data | 0xA010 / 0xA020 / 0xA040 |
| OpCommand | 0x2400 | | OpMacMaster / OpMacSlave | 0xF000 / 0xF100 (deprecated) |
| OpDataRequest | 0x2700 | | OpFirmwareMaster | 0xF200 |
| OpDataReply | 0x2800 | | OpFirmwareReply | 0xF300 |
| OpOutput / OpDmx | 0x5000 | | OpFileTnMaster / FnMaster / FnReply | 0xF400 / 0xF500 / 0xF600 |
| OpNzs | 0x5100 | | OpIpProg | 0xF800 |
| OpSync | 0x5200 | | OpIpProgReply | 0xF900 |
| OpAddress | 0x6000 | | OpMedia / MediaPatch / MediaControl / MediaContrlReply | 0x9000 / 0x9100 / 0x9200 / 0x9300 |
| OpInput | 0x7000 | | OpTimeCode | 0x9700 |
| OpTodRequest | 0x8000 | | OpTimeSync | 0x9800 |
| OpTodData | 0x8100 | | OpTrigger | 0x9900 |
| OpTodControl | 0x8200 | | OpDirectory / OpDirectoryReply | 0x9A00 / 0x9B00 |

Auf dem Draht: `OpPoll` = Bytes `00 20`, `OpFirmwareMaster` = `00 f2` usw.

---

## 2. Discovery

### 2.1 ArtPoll (S. 16–19) — 22 Byte, Mindestlänge **14**

| Off | Feld (Spec-Nr.) | Größe | Inhalt |
|---|---|---|---|
| 0 | ID (1) | 8 | `Art-Net\0` |
| 8 | OpCode (2) | 2 LE | 0x2000 |
| 10 | ProtVerHi/Lo (3,4) | 1+1 | 0, 14 |
| 12 | Flags (5) | 1 | s. u. |
| 13 | DiagPriority (6) | 1 | niedrigste gewünschte Diagnose-Priorität (Table 5) |
| 14 | TargetPortAddressTopHi/Lo (7,8) | 2 BE | Obergrenze Targeted Mode |
| 16 | TargetPortAddressBottomHi/Lo (9,10) | 2 BE | Untergrenze Targeted Mode |
| 18 | EstaManHi/Lo (11,12) | 2 **BE** | ESTA-Code des Absenders |
| 20 | OemHi/OemLo (13,14) | 2 BE | OEM-Code des Absenders |

Flags:

| Bit | Bedeutung |
|---|---|
| 7–6 | unbenutzt, 0 senden, nicht testen |
| 5 | 1 = **Targeted Mode** aktiv |
| 4 | 0 = VLC-Übertragung erlaubt, 1 = VLC aus |
| 3 | 0 = Diagnose per Broadcast, 1 = Diagnose per Unicast (nur wenn Bit 2) |
| 2 | 1 = „Send me diagnostics messages“ |
| 1 | 0 = ArtPollReply nur als Antwort auf ArtPoll **oder ArtAddress**; **1 = „Send ArtPollReply whenever Node conditions change“** |
| 0 | „0 = Deprecated.“ |

Regeln:
- **Mindestlänge 14 Byte** (ID…ProtVer); fehlende Felder gelten als 0 (S. 17).
- Controller broadcastet an 2.255.255.255 (Directed Broadcast); Unicast nur mit Targeted Mode „Allowed“ (S. 16).
- „It is a requirement of Art-Net that **all controllers broadcast an ArtPoll every 2.5 to 3 seconds**.“ Controller
  dürfen 3 s Timeout bis zu allen Replies annehmen; ohne Antwort gilt der Node als weg (S. 16). Der pollende Controller
  soll **sich selbst** (per Unicast) mit ArtPollReply antworten (S. 16).
- **Targeted Mode** (S. 17): Nodes antworten nur, wenn sie eine Port-Address **inklusive** im Bereich
  Bottom…Top abonniert haben (abonniert = in SwIn oder SwOut, S. 63).
- **Mehrere Controller** (S. 17): Fordert *irgendein* Controller Diagnose an, sendet der Node Diagnose. Fordern
  *mehrere* an, wird Diagnose **gebroadcastet** (Flags Bit 3 ignorieren). Es gilt die *niedrigste* DiagPriority.

### 2.2 ArtPollReply (S. 25–33) — 239 Byte, Mindestlänge **207**

Packet strategy: Empfang „No Art-Net action“; **Unicast: Allowed; Broadcast: Not Allowed** (S. 25; Art-Net-4-
Änderungsliste S. 8: „ArtPollReply broadcast not allowed“). „The device should **wait for a random delay of up to 1s**
before sending the reply“ (S. 25).

| Off | Feld (Nr.) | Größe | Inhalt |
|---|---|---|---|
| 0 | ID (1) | 8 | |
| 8 | OpCode (2) | 2 LE | 0x2100 |
| 10 | IP Address[4] (3) | 4 | MSB zuerst. Gebundene Nodes dürfen die IP des Root teilen |
| 14 | Port (4) | 2 **LE** | immer 0x1936 → Bytes `36 19` |
| 16 | VersInfoH/L (5,6) | 2 BE | Firmware-Version (höher = neuer; nur für Update-Entscheidung) |
| 18 | NetSwitch (7) | 1 | Bits 14–8 der Port-Address in Bits 6–0 |
| 19 | SubSwitch (8) | 1 | Bits 7–4 der Port-Address in Bits 3–0 |
| 20 | OemHi (9), Oem (10) | 2 BE | OEM-Code |
| 22 | Ubea Version (11) | 1 | 0 = kein UBEA |
| 23 | Status1 (12) | 1 | s. u. |
| 24 | **EstaManLo (13), EstaManHi (14)** | 2 **LE** | 0x7FF0 → Bytes `f0 7f` |
| 26 | PortName[18] (15) | 18 | „Short Name“, nullterminiert, max. 17 Zeichen |
| 44 | LongName[64] (16) | 64 | max. 63 Zeichen; **bei mehreren Binds identisch** |
| 108 | NodeReport[64] (17) | 64 | Format `#xxxx [yyyy] zzzz…` (xxxx = Hex-Code Table 3, yyyy = dezimaler Zähler pro gesendetem Reply, Überlauf bei 9999) |
| 172 | NumPortsHi/Lo (18,19) | 2 BE | Anzahl Ports, **max. 4**; Receiver dürfen ignorieren (steckt in PortTypes) |
| 174 | PortTypes[4] (20) | 4 | Bit 7 = kann ausgeben (Art-Net→DMX), Bit 6 = kann einlesen, Bits 5–0 Protokoll (0 = DMX512, 1 MIDI, 2 Avab, 3 CMX, 4 ADB 62.5, 5 Art-Net, 6 DALI) |
| 178 | GoodInput[4] (21) | 4 | 7 Daten empfangen · 6 Testpakete · 5 SIPs · 4 Textpakete · 3 **Input disabled** · 2 Empfangsfehler · 1 unbenutzt · 0 Input→sACN (clr = Art-Net) |
| 182 | GoodOutputA[4] (22) | 4 | 7 **ArtDmx/sACN wird als DMX ausgegeben** · 6 Test · 5 SIP · 4 Text · 3 **merging** · 2 Kurzschluss beim Einschalten · 1 **Merge LTP** · 0 Output von sACN (clr = Art-Net) |
| 186 | SwIn[4] (23) | 4 | Bits 3–0 der Port-Address pro Input |
| 190 | SwOut[4] (24) | 4 | Bits 3–0 der Port-Address pro Output |
| 194 | AcnPriority (25) | 1 | sACN-Priorität für aus DMX erzeugtes sACN |
| 195 | SwMacro (26) | 1 | Makro-Eingänge 1–8 (Bit 0–7), bei Flags-Bit-1-Modus Reply bei Key-down und Key-up |
| 196 | SwRemote (27) | 1 | Remote-Eingänge 1–8 |
| 197 | Spare ×3 (28–30) | 3 | 0 |
| 200 | Style (31) | 1 | Table 4: 0 StNode, 1 StController, 2 StMedia, 3 StRoute, 4 StBackup, 5 StConfig, 6 StVisual |
| 201 | MAC Hi…Lo (32–37) | 6 | 0 wenn unbekannt — **Ende der 207-Byte-Mindestlänge** |
| 207 | BindIp[4] (38) | 4 | IP des Root-Geräts |
| 211 | BindIndex (39) | 1 | Reihenfolge gebundener Geräte; **0 oder 1 = Root** |
| 212 | Status2 (40) | 1 | s. u. |
| 213 | GoodOutputB[4] (41) | 4 | 7 **RDM disabled** (clr = enabled) · 6 **Output continuous** (clr = delta) · 5 **Discovery läuft nicht** (clr = läuft) · 4 **Background-Discovery aus** · 3–0 0 |
| 217 | Status3 (42) | 1 | s. u. |
| 218 | DefaulRespUID Hi…Lo (43–48) | 6 BE | „RDMnet & LLRP Default Responder UID“ |
| 224 | UserHi/Lo (49,50) | 2 | frei |
| 226 | RefreshRateHi/Lo (51,52) | 2 BE | max. ArtDmx-Rate in Hz; 0–44 = DMX512-Maximum 44 Hz |
| 228 | BackgroundQueuePolicy (53) | 1 | 0 STATUS_NONE · 1 ADVISORY · 2 WARNING · 3 ERROR · 4 aus · 5–250 Hersteller · 251–255 reserviert; nur wenn Status3.1 = 1 |
| 229 | Filler (54) | 10 | 0 |

Status1: 7–6 Indikatoren (00 unbekannt, 01 Locate/Identify, 10 Mute, 11 Normal) · 5–4 Port-Address-Authority
(00 unbekannt, 01 Front-Panel, 10 Netz/Web, 11 n. b.) · 3 nicht implementiert · 2 1 = aus ROM gebootet ·
1 **RDM-fähig** · 0 UBEA vorhanden.

Status2: 7 RDM per ArtAddress steuerbar · 6 Output-Style per ArtAddress umschaltbar · 5 squawking ·
4 Art-Net/sACN umschaltbar · 3 **15-Bit-Port-Address** (Art-Net 3/4) · 2 **DHCP-fähig** · 1 **IP per DHCP** ·
0 Web-Konfiguration.

Status3: 7–6 **Failsafe-Zustand** (00 Hold, 01 Zero, 10 Full, 11 Szene) · 5 programmierbares Failsafe ·
4 LLRP · 3 **Port-Richtung umschaltbar („PortTypes[] shows the current direction“)** · 2 RDMnet ·
1 BackgroundQueue unterstützt · 0 Background-Discovery per ArtAddress abschaltbar.

Regeln:
- Antwort auf ArtPoll (in Targeted Mode nur bei Treffer) und auf **ArtAddress** und **ArtInput** (S. 38, 75).
- Mit Flags Bit 1: Reply bei jeder Zustandsänderung (S. 18), u. a. **wenn Merge beginnt** (S. 65) und bei
  SwMacro/SwRemote-Änderungen (S. 29). Da Broadcast verboten ist, geht dieses Reply per Unicast an die Controller,
  die Bit 1 gesetzt haben (Ableitung; die Spec nennt den Empfänger nicht explizit **[Spec schweigt]**).
- **BindIndex / mehrere Replies** (S. 6–8, 25, 31, 88): Ein Gerät mit mehr Ports schickt pro „Seite“ (BindIndex)
  ein eigenes Reply, alle dürfen dieselbe IP haben („ArtPollReply->BindAddress can now be identical in all
  BindIndex“). NumPorts ≤ 4 pro Reply. Art-Net 4 empfiehlt implizit **einen Port pro Reply**: „As most modern Art-Net
  gateways implement one universe per ArtPollReply, ArtTodData->Port will usually be set to a value of 1“ (S. 88),
  und alle ArtAddress-Kommandos für Port 1–3 sind „Deprecated in Art-Net 4“ (S. 41–46). Damit bekommt auch jeder
  Port ein eigenes Net/Sub-Net („developer can choose to identify each DMX port individually“, S. 6).
- Port-Address eines Outputs = `(NetSwitch & 0x7F) << 8 | (SubSwitch & 0x0F) << 4 | (SwOut[i] & 0x0F)`.

NodeReport-Codes (Table 3, S. 23–24): 0x0000 RcDebug, 0x0001 RcPowerOk, 0x0002 RcPowerFail, 0x0003 RcSocketWr1,
0x0004 RcParseFail, 0x0005 RcUdpFail, 0x0006 RcShNameOk, 0x0007 RcLoNameOk, 0x0008 RcDmxError, 0x0009 RcDmxUdpFull,
0x000A RcDmxRxFull, 0x000B RcSwitchErr, 0x000C RcConfigErr, 0x000D RcDmxShort, 0x000E RcFirmwareFail,
0x000F RcUserFail, 0x0010 RcFactoryRes.

---

## 3. DMX-Daten

### 3.1 ArtDmx (S. 62–65) — 18 + Length Byte

| Off | Feld | Größe | Inhalt |
|---|---|---|---|
| 0 | ID, OpCode 0x5000, ProtVer | 12 | |
| 12 | Sequence | 1 | 0x01–0xFF zum Umsortieren; **0x00 = Funktion aus** |
| 13 | Physical | 1 | physischer Input-Port der Quelle; **wird zum Merge-Unterscheiden benutzt** |
| 14 | SubUni | 1 | Low-Byte der Port-Address (Sub-Net<<4 \| Universe) |
| 15 | Net | 1 | Bits 14–8 der Port-Address |
| 16 | LengthHi/Length | 2 BE | „should be an **even number in the range 2 – 512**“ |
| 18 | Data[Length] | n | Slots 1…n, **ohne** Start Code |

Regeln:
- **Nur Unicast** an Abonnenten des Universes; „If there are no subscribers to a universe, the controller shall not
  send ArtDmx. **There are no conditions in which broadcast is allowed.**“ Abonniert = Universe steht in SwIn oder
  SwOut eines ArtPollReply; der Sender muss dafür regelmäßig pollen (S. 63).
- Ohne neue ArtDmx sendet jeder DMX-Ausgang den letzten Frame **kontinuierlich weiter** (S. 62).
- Ausgabeknoten dürfen immer 512 Kanäle ausgeben (S. 64).
- **Input-Ports** (S. 62): Erster vollständiger Frame wird gesendet, danach jeder Frame mit neuen Daten (andere Länge
  oder Inhalt). Kein ArtDmx für Inputs, die seit dem Einschalten nichts empfangen haben. Aktive, unveränderte Inputs
  senden „at approximately **4-second** intervals“, empfohlen ist aber **800 ms – 1000 ms** (sACN-Konvergenz). Ein
  ausgefallener DMX-Input sendet nicht weiter.
- RefreshRate (S. 64): DMX512-Gateways immer 44 Hz.

### 3.2 Merge (S. 64–66)

- Konflikt = gleiche Port-Address von **verschiedenen IPs oder gleicher IP mit verschiedenem `Physical`**.
- Node darf das als Fehler behandeln oder mergen (Merge bevorzugt; Verhalten im Handbuch dokumentieren).
- Merge **HTP (Default)** oder LTP, per ArtAddress. Während Merge: GoodOutputA Bit 3; mit Flags Bit 1 Reply bei
  Merge-Beginn.
- Ende: `AcCancelMerge` → das **nächste** ArtDmx beendet Merge, danach werden ArtDmx von anderen IPs verworfen.
- Fällt **eine** Quelle aus, bleibt sie **10 s** im Merge-Puffer; kommt sie zurück, wird weiter gemergt, sonst Ende
  des Merge. Fallen **beide** aus, hält der Ausgang das letzte Merge-Ergebnis.
- **Maximal 2 Quellen**, weitere werden ignoriert.
- Merge mehrerer eigener DMX-Inputs auf dieselbe Port-Address muss der Node selbst machen.

### 3.3 ArtSync (S. 66–68) — 14 Byte

| Off | Feld | Größe |
|---|---|---|
| 0 | ID, OpCode 0x5200, ProtVer | 12 |
| 12 | Aux1, Aux2 | 1+1 (0 senden) |

- Packet strategy: Controller **Directed Broadcast**; **Unicast Transmit „Not Allowed“** (S. 66). Ablauf: ArtDmx
  per Unicast, dann ArtSync per Broadcast (S. 67).
- Nach Power-On/Reset nicht-synchron. Erstes ArtSync → synchroner Modus (ArtDmx puffern, beim nächsten ArtSync
  ausgeben). **Kein ArtSync für ≥ 4 s → zurück in den nicht-synchronen Modus** (S. 67).
- ArtSync wird **ignoriert, wenn seine Quell-IP nicht die des letzten ArtDmx** ist, und **ignoriert, solange ein Port
  Streams verschiedener IPs merged** (S. 68).
- Mindestlänge nicht angegeben **[Spec schweigt]**.

### 3.4 Datenverlust / Failsafe

- ArtAddress-Kommandos AcFailHold/Zero/Full/Scene/Record (S. 41) und Status3 Bits 7–5 (S. 32) definieren das
  *Verhalten* „in the event of loss of network data“. **Eine Zeit, ab wann Daten als verloren gelten, nennt die Spec
  nicht [Spec schweigt].** Einzige Zeitangaben in der Nähe: Merge-Timeout 10 s, Sync-Timeout 4 s, Input-Keepalive
  ~4 s (empf. 0,8–1 s) und die Status-LED-Empfehlung „DMX Output … Timeout after 6 seconds“ (S. 96).

---

## 4. Konfiguration

### 4.1 ArtAddress (S. 38–47) — 107 Byte

Packet strategy: Controller Unicast; Node antwortet **per Unicast mit ArtPollReply** (S. 38).

| Off | Feld | Größe | Inhalt |
|---|---|---|---|
| 0 | ID, OpCode 0x6000, ProtVer | 12 | |
| 12 | NetSwitch | 1 | nur wirksam mit Bit 7 (0x87 setzt 7); **0x00 = zurück auf Hardware-Schalter** |
| 13 | BindIndex | 1 | Ziel-Bind (1 = Root) |
| 14 | PortName[18] | 18 | Short Name; leerer String = ignorieren |
| 32 | LongName[64] | 64 | leer = ignorieren |
| 96 | SwIn[4] | 4 | wie NetSwitch (Bit 7, 0x00 = Reset) |
| 100 | SwOut[4] | 4 | wie NetSwitch |
| 104 | SubSwitch | 1 | wie NetSwitch |
| 105 | AcnPriority | 1 | 0–200 gültig, **255 = keine Änderung** |
| 106 | Command | 1 | s. u. |

Wie „keine Änderung“ für Net/Sub/SwIn/SwOut kodiert wird, sagt die Spec nicht ausdrücklich: Werte ohne Bit 7
werden ignoriert, **außer 0x00** (= Reset). **Festlegung** (Vektoren): 0x7F = keine Änderung.

Command (S. 40–47); „(dep.)“ = „Deprecated in Art-Net 4“:

| Wert | Mnemonic | Wirkung |
|---|---|---|
| 0x00 | AcNone | nichts |
| 0x01 | AcCancelMerge | Merge beim nächsten ArtDmx beenden |
| 0x02 / 0x03 / 0x04 | AcLedNormal / AcLedMute / AcLedLocate | Indikatoren normal / aus / schnelles Blinken |
| 0x05 | AcResetRxFlags | SIP/Text/Test/Fehler-Flags zurücksetzen; Kurzschlusstest neu |
| 0x06 / 0x07 | AcAnalysisOn / Off | Analyse-/Debug-Modus |
| 0x08 / 0x09 / 0x0A / 0x0B | AcFailHold / Zero / Full / Scene | Failsafe-Verhalten (über Power-Cycle speichern) |
| 0x0C | AcFailRecord | aktuellen Ausgang als Failsafe-Szene speichern |
| 0x10 (0x11–0x13 dep.) | AcMergeLtp0…3 | Port n Merge LTP |
| 0x20 (0x21–0x23 dep.) | AcDirectionTx0…3 | Port n → Output |
| 0x30 (0x31–0x33 dep.) | AcDirectionRx0…3 | Port n → Input; **leert auch die Abonnentenliste des Ports** |
| 0x50 (0x51–0x53 dep.) | AcMergeHtp0…3 | Port n Merge HTP (Default) |
| 0x60 (0x61–0x63 dep.) | AcArtNetSel0…3 | Port n Art-Net für DMX und RDM (Default) |
| 0x70 (0x71–0x73 dep.) | AcAcnSel0…3 | Port n sACN für DMX, RDM weiter über Art-Net |
| 0x90 (0x91–0x93 dep.) | AcClearOp0…3 | DMX-Ausgangspuffer von Port n leeren |
| 0xA0 (0xA1–0xA3 dep.) | AcStyleDelta0…3 | Output nur bei ArtDmx (delta) |
| 0xB0 (0xB1–0xB3 dep.) | AcStyleConst0…3 | Output kontinuierlich |
| 0xC0 (0xC1–0xC3 dep.) | AcRdmEnable0…3 | RDM an |
| 0xD0 (0xD1–0xD3 dep.) | AcRdmDisable0…3 | RDM aus |
| 0xE0…0xEF | AcBqp0…15 | BackgroundQueuePolicy 0–15 (5–15 user defined) |

LTP/HTP und Richtung sollen über Power-Cycle erhalten bleiben (S. 41). Für Background-Discovery ein/aus gibt es
**kein** ArtAddress-Kommando in dieser Revision, obwohl Status3 Bit 0 darauf verweist **[Spec-Lücke]**
(nur ArtTodControl AtcIncOn/Off, 5.3).

### 4.2 ArtInput (S. 75–76) — 20 Byte

| Off | Feld | Größe | Inhalt |
|---|---|---|---|
| 0 | ID, OpCode 0x7000, ProtVer | 12 | |
| 12 | Filler1 | 1 | |
| 13 | BindIndex | 1 | |
| 14 | NumPortsHi/Lo | 2 BE | max. 4 |
| 16 | Input[4] | 4 | Bit 0 = **diesen Input** deaktivieren |

Alle Nodes starten mit allen Inputs aktiv. Antwort: ArtPollReply (S. 75). ArtInput betrifft nur **DMX-Inputs**.

### 4.3 ArtIpProg (S. 34–35) — 34 Byte

| Off | Feld | Größe | Inhalt |
|---|---|---|---|
| 0 | ID, OpCode 0xF800, ProtVer | 12 | |
| 12 | Filler1, Filler2 | 2 | |
| 14 | Command | 1 | s. u.; alle Bits 0 = nur Abfrage |
| 15 | Filler4 | 1 | |
| 16 | ProgIp | 4 | MSB zuerst |
| 20 | ProgSm | 4 | |
| 24 | ProgPort | 2 | deprecated |
| 26 | ProgDg | 4 | Default Gateway |
| 30 | Spare | 4 | 0 |

Command: 7 Programmieren erlauben · 6 **DHCP einschalten (dann untere Bits ignorieren)** · 5 unbenutzt ·
4 **Default Gateway programmieren** · 3 **alle drei Parameter auf Default** · 2 IP programmieren ·
1 Maske programmieren · 0 Port programmieren (deprecated).

Nur Unicast vom Controller; Node antwortet, **falls er IP-Programmierung unterstützt**, mit ArtIpProgReply,
„In all scenarios … unicast to the IP address of the sender“ (S. 34, 36). Ob die Antwort die alte oder die neue
IP enthält und von welcher Quell-IP sie kommt, sagt die Spec nicht **[Spec schweigt]**.

### 4.4 ArtIpProgReply (S. 36–37) — 34 Byte

| Off | Feld | Größe |
|---|---|---|
| 0 | ID, OpCode 0xF900, ProtVer | 12 |
| 12 | Filler1–4 | 4 |
| 16 | ProgIp (IP des Nodes) | 4 |
| 20 | ProgSm | 4 |
| 24 | ProgPort (deprecated) | 2 |
| 26 | Status: Bit 6 = DHCP enabled, Rest 0 | 1 |
| 27 | Spare2 | 1 |
| 28 | ProgDg | 4 |
| 32 | Spare7, Spare8 | 2 |

### 4.5 ArtDiagData (S. 52–53) — 18 + Length Byte

| Off | Feld | Größe |
|---|---|---|
| 0 | ID, OpCode 0x2300, ProtVer | 12 |
| 12 | Filler1 | 1 |
| 13 | DiagPriority | 1 |
| 14 | LogicalPort (0 = allgemein, nur informativ) | 1 |
| 15 | Filler3 | 1 |
| 16 | LengthHi/Lo | 2 BE |
| 18 | Data[Length] ASCII, nullterminiert, max. 512 inkl. NUL | n |

Priorities (Table 5): 0x10 DpLow, 0x40 DpMed, 0x80 DpHigh, 0xE0 DpCritical, 0xF0 DpVolatile.
Ziel (Broadcast/Unicast) wie im ArtPoll angefordert (2.1). Keine Vektoren (nicht verlangt).

---

## 5. RDM

Grundsatz (S. 83): Discovery wird **geproxyt** (Gateways halten eigene TOD), GET/SET sind **nicht geproxyt** und gehen
an die Endgeräte. Output-Gateway: volle Discovery beim Einschalten, inkrementelle als Hintergrundaufgabe. Nach der
ersten Discovery und bei jeder TOD-Änderung **unicastet** es ArtTodData „to all IP addresses that have previously
requested an ArtTodData packet“ (S. 84).

### 5.1 ArtTodRequest (S. 85–86) — 56 Byte

| Off | Feld | Größe | Inhalt |
|---|---|---|---|
| 0 | ID, OpCode 0x8000, ProtVer | 12 | |
| 12 | Filler1, 2 | 2 | |
| 14 | Spare1–7 | 7 | |
| 21 | Net | 1 | |
| 22 | Command | 1 | 0x00 TodFull |
| 23 | AddCount | 1 | benutzte Einträge, max. 32 |
| 24 | Address[32] | 32 | Low-Bytes der Port-Addresses |

„A Node receiving this packet **must not interpret it as forcing full discovery**. Full discovery is only initiated at
power on or when an ArtTodControl.AtcFlush is received. The response is ArtTodData.“ (S. 85). Controller darf
Unicast und Broadcast senden.

### 5.2 ArtTodData (S. 87–89) — 28 + 6·UidCount Byte

| Off | Feld | Größe | Inhalt |
|---|---|---|---|
| 0 | ID, OpCode 0x8100, ProtVer | 12 | |
| 12 | RdmVer | 1 | 0x00 RDM Draft, **0x01 RDM Standard V1.0** |
| 13 | Port | 1 | 1–4; Physischer Port = (BindIndex−1)·NumPortsLo + Port |
| 14 | Spare1–6 | 6 | |
| 20 | BindIndex | 1 | muss zum ArtPollReply passen |
| 21 | Net | 1 | |
| 22 | CommandResponse | 1 | 0x00 TodFull, 0xFF **TodNak** (TOD nicht verfügbar / Discovery unvollständig) |
| 23 | Address | 1 | Low-Byte der Port-Address |
| 24 | UidTotalHi/Lo | 2 BE | Gesamtzahl UIDs des Universes |
| 26 | BlockCount | 1 | 0 im ersten Paket, dann +1 |
| 27 | UidCount | 1 | UIDs in diesem Paket |
| 28 | ToD[UidCount] | 6 je | UID (BE) |

„When UidTotal exceeds **200**, multiple ArtTodData packets are used“ → max. 200 UIDs pro Paket (28 + 1200 =
1228 Byte). Broadcast „Not Allowed“ (Art-Net 4). Antwort auf ArtTodRequest und auf ArtTodControl (S. 85, 90).

### 5.3 ArtTodControl (S. 90–91) — 24 Byte

| Off | Feld | Größe |
|---|---|---|
| 0 | ID, OpCode 0x8200, ProtVer | 12 |
| 12 | Filler1, 2 | 2 |
| 14 | Spare1–7 | 7 |
| 21 | Net | 1 |
| 22 | Command | 1 |
| 23 | Address | 1 |

Command: 0x00 AtcNone · 0x01 **AtcFlush** (TOD leeren, volle Discovery) · 0x02 **AtcEnd** (laufende Discovery
beenden, TOD bleibt) · 0x03 **AtcIncOn** (inkrementelle Discovery an) · 0x04 **AtcIncOff** (aus).
Antwort immer ArtTodData (Revision BG, S. 1; S. 90).

### 5.4 ArtRdm (S. 92–93) — 24 + n Byte

| Off | Feld | Größe | Inhalt |
|---|---|---|---|
| 0 | ID, OpCode 0x8300, ProtVer | 12 | |
| 12 | RdmVer | 1 | 0x01 = RDM Standard V1.0 |
| 13 | Filler2 | 1 | |
| 14 | Spare1–5 | 5 | |
| 19 | **FifoAvail** | 1 | freie Plätze in der RDM-Sendewarteschlange des Gateways (0 = keine Queue) |
| 20 | **FifoMax** | 1 | Größe dieser Queue (0 = keine Queue) |
| 21 | Net | 1 | |
| 22 | Command | 1 | 0x00 ArProcess (einziger Wert) |
| 23 | Address | 1 | Low-Byte der Port-Address |
| 24 | RdmPacket[n] | n | „The RDM data packet **excluding the DMX StartCode**“ |

- Alle Nicht-Discovery-RDM-Messages; **Broadcast in Art-Net 4 nicht erlaubt** (Unicast „mandated“, S. 3, 92).
- RdmPacket beginnt also mit Sub-Start-Code 0x01; Message Length und Checksumme bleiben die E1.20-Werte, die den
  Start Code 0xCC mitzählen (E1.20, nicht Art-Net-Spec; im Vektor nachgerechnet).
- Wohin Antworten gehen, Timeouts, Verhalten ohne Antwort: **[Spec schweigt]**.

### 5.5 ArtRdmSub (S. 94–95) — 32 + 2·k Byte

| Off | Feld | Größe |
|---|---|---|
| 0 | ID, OpCode 0x8400, ProtVer | 12 |
| 12 | RdmVer | 1 |
| 13 | Filler2 | 1 |
| 14 | UID (Ziel) | 6 |
| 20 | Spare1 | 1 |
| 21 | CommandClass | 1 |
| 22 | ParameterId | 2 BE |
| 24 | SubDevice (0 = Root) | 2 BE |
| 26 | SubCount (0 illegal) | 2 BE |
| 28 | Spare2–5 | 4 |
| 32 | Data[k] 16-bit BE; k = 0 bei Get/SetResponse, SubCount bei Set/GetResponse | 2k |

„it is only acceptable to implement this packet **in addition to ArtRdm**. It must not be used instead of ArtRdm.“
Optionales Paket. Keine Vektoren (nicht verlangt).

---

## 6. Firmware-Update (S. 77–82)

Allgemein (S. 77): immer Unicast („Under no circumstances should the broadcast address be used“). Jedes
ArtFirmwareMaster wird mit ArtFirmwareReply bestätigt. **Controller wartet max. 30 s auf das Reply**, sonst Abbruch.
**Node wartet max. 30 s auf den nächsten Block**, sonst Abbruch, Rückkehr zur alten Firmware und Status/NodeReport
setzen. Die Datei enthält eine **Liste gültiger OEM-Codes**; der Controller muss sie vor dem Senden prüfen, der Node
**prüft sie beim ersten Paket** und schickt sonst eine Fehlerantwort.

### 6.1 ArtFirmwareMaster (S. 78–79) — 1064 Byte

| Off | Feld | Größe | Inhalt |
|---|---|---|---|
| 0 | ID, OpCode 0xF200, ProtVer | 12 | |
| 12 | Filler1, 2 | 2 | |
| 14 | Type | 1 | 0x00 FirmFirst · 0x01 FirmCont · 0x02 FirmLast · 0x03 UbeaFirst · 0x04 UbeaCont · 0x05 UbeaLast |
| 15 | BlockId | 1 | 0 beim First-Paket, dann fortlaufend |
| 16 | FirmwareLength3…0 | 4 BE | **Gesamtlänge der Datei in 16-Bit-Worten inkl. Header** (Beispiel: 32 K Worte + 530 Header-Worte = 0x00008212) |
| 20 | Spare[20] | 20 | 0 |
| 40 | Data[512] (Int16, hi byte first) | **1024** | Dateiinhalt, letzter Block mit 0 aufgefüllt |

**[mehrdeutig]** Data ist „Data[512] Int16“, der Text sagt aber „null packed if less than 512 bytes needed“.
Wir nehmen **512 Worte = 1024 Byte** pro Block (passt zur Wortzählung in FirmwareLength; libartnet/OLA – hier nicht
geprüft – machen es ebenfalls so). FirmwareLength wird im Text „This Int64 parameter“ genannt, besteht aber aus 4 Bytefeldern
(Length3…Length0); Rev BE (S. 1) meldet sogar „incorrectly defined FirmwareLength as Int32“ als korrigierten
Fehler. Maßgeblich sind die 4 Felder → 32 Bit BE **[mehrdeutig]**.

**[Spec schweigt]** BlockId ist nur 8 Bit: nach 256 Blöcken (256 KiB) läuft es über. Ein mehrere MB großes
ESP32-Image braucht ~3000+ Blöcke → Wrap-around (BlockId modulo 256) ist unsere **Festlegung**, Verhalten von
DMX-Workshop unbekannt.

### 6.2 ArtFirmwareReply (S. 80–81) — 36 Byte

| Off | Feld | Größe | Inhalt |
|---|---|---|---|
| 0 | ID, OpCode 0xF300, ProtVer | 12 | |
| 12 | Filler1, 2 | 2 | |
| 14 | Type | 1 | 0x00 **FirmBlockGood** · 0x01 **FirmAllGood** · 0xFF **FirmFail** (alle Fehler) |
| 15 | Spare[21] | 21 | 0 |

### 6.3 Firmware-Dateiformat (S. 81–82)

Endung `.alf` (Firmware) bzw. `.alu` (UBEA). Offsets 0-basiert (Spec zählt Bytes ab 1):

| Off | Feld | Größe | Inhalt |
|---|---|---|---|
| 0 | ChecksumHi/Lo | 2 BE | „16 bit, one's-complement checksum of the firmware data area“ |
| 2 | VersInfoHi/Lo | 2 BE | Firmware-Version (wie ArtPollReply.VersInfo) |
| 4 | UserName | 30 | Anzeige für den Controller, **muss nullterminiert sein**, vom Node nicht geprüft |
| 34 | Oem[256] | 512 | 256 Worte, **hi byte first**, unbenutzte = 0x0000 |
| 546 | Spare[255] | 510 | 0 |
| 1056 | Length3…0 | 4 BE | Länge der folgenden Firmware in **Worten** |
| 1060 | Data[] | 2·Length | Firmware als 16-Bit-Worte, hi byte first, Inhalt herstellerspezifisch |

Header = 1060 Byte = 530 Worte (passt zum Beispiel in 6.1). **[mehrdeutig]** „one's-complement checksum“:
ob Einerkomplement-Summe oder deren Komplement, und ob nur über Data[] – im Vektor als Komplement der
Einerkomplement-Summe über Data[] berechnet, **nicht verifiziert**.

---

## 7. OEM-Code und ESTA-Code

**OEM (Art-Net):**
- Spec S. 1: „Artistic Licence require that any manufacturer who implements this protocol **apply for an OemCode**“.
  Table 2, S. 23: „The OEM code … uniquely defines a product and **must be registered**.“
- **Die Spec nennt keinen reservierten Entwicklungs- oder Prototyp-OEM-Code.**
- [OEM-Liste] enthält zwei Einträge von Artistic Licence ohne Produkt: `OemUnknown 0x00ff` („Artistic Licence
  Engineering Ltd 0x414c OemUnknown 0 0 n n“) und `OemGlobal 0xffff`. Ein Zweck ist nicht angegeben. 0xFFFF hat in
  ArtTrigger die Bedeutung „alle/Art-Net-definiert“ (S. 59) – für ein Produkt also ungeeignet. 0x00FF ist nur ein
  **Platzhalter** in unseren Vektoren, kein freigegebener Entwicklungscode. Ein eigener Code ist kostenlos über die
  OEM-Code-Zone zu beantragen.
- Der OEM-Code geht in die Default-IP ein (B = MAC[3] + OemHi + OemLo, S. 13) und in die OEM-Liste der Firmware-Datei.

**ESTA-Manufacturer-ID:**
- Art-Net sagt nur „assigned by ESTA“ (S. 19, 27). [ESTA]: „Manufacturer IDs **7FF0h through 7FFFh are reserved for
  prototyping and experimental use** by a manufacturer while that manufacturer is waiting for an assigned Manufacturer
  ID. **They are not permitted to be used in product that is shipping.**“ → 0x7FF0 ist für Prototypen korrekt, nicht
  für ausgelieferte Geräte.

---

## 8. Abweichungen gegenüber dem Protokollvertrag

Bezug: `ledTubeXBAR500/research/pixelmapper-node-protocol.md`, Abschnitte 2–6 und 9.2.
✅ = bestätigt · ⚠️ = weicht ab / unvollständig · ➕ = im Vertrag, aber nicht in der Spec (unsere Erweiterung, zulässig?).

**Opcodes**
- ✅ Alle im Vertrag genannten Opcodes stimmen: 0x2000, 0x2100, 0x5000, 0x5200, 0x6000, 0x7000, 0x8000, 0x8100,
  0x8200, 0x8300, 0x8400, 0xF200, 0xF300, 0xF800, 0xF900 (Table 1, S. 20–22). OpCode LE ✅.

**§2 Transport / Netz**
- ⚠️ Vertrag: Start mit DHCP, nach ~5 s Fallback 2.x.x.x. Spec: „factory start using a Class A IP address scheme“
  (S. 12); DHCP ist zulässig, aber nicht der in der Spec beschriebene Default. Die Fallback-Adresse muss nach Spec
  **B = MAC[3] + OemHi + OemLo** sein (nicht einfach „aus der MAC“), Maske 255.0.0.0 (S. 13).
- ✅ Directed Broadcast an 2.255.255.255; Limited Broadcast 255.255.255.255 soll nicht genutzt werden (S. 10, 16).

**§3 Discovery**
- ✅ ArtPoll alle 2,5–3 s (S. 16 – für Controller sogar Pflicht).
- ✅ **Flags Bit 1** = „Send ArtPollReply whenever Node conditions change“ (S. 18). Bit 0 ist deprecated.
- ⚠️ Fehlt im Vertrag: ArtPollReply **nur Unicast**, mit **Zufallsverzögerung bis 1 s** (S. 25); Pixelmapper muss
  bis 3 s auf Replies warten (S. 16). Targeted Mode (Flags Bit 5) muss der Node filtern (S. 17).
- ⚠️ „Pro BindIndex höchstens 4 Ports, Prototyp = ein Reply mit BindIndex 1“: erlaubt (NumPorts max. 4), aber in
  Art-Net 4 sind alle ArtAddress-Kommandos für Port 1–3 **deprecated** (S. 41–46) und ArtTodData erwartet
  typischerweise Port = 1 (S. 88). Mit 4 Ports in einem Reply kann ein Art-Net-4-konformer Controller Merge,
  Richtung, RDM an/aus, Output-Style und ClearOp **nur für Port 0** setzen. **Empfehlung: 1 Port pro BindIndex
  (BindIndex 1…4, gleiche IP, gleicher LongName)** – auch Voraussetzung für unabhängige Net/Sub-Net je Port.
- ⚠️ „Port-Typen: aktuelle Richtung steht in GoodInput/GoodOutput“: Spec Status3 Bit 3: wenn Richtung umschaltbar,
  **zeigt PortTypes[] die aktuelle Richtung** (S. 32). GoodInput/GoodOutputA sind Statusbits.
- ✅ Statusbits: Daten empfangen (GoodInput.7) / ausgegeben (GoodOutputA.7), Merge (GoodOutputA.3), LTP (GoodOutputA.1),
  RDM aus (GoodOutputB.7), continuous (GoodOutputB.6), Failsafe (Status3.7–6), DHCP-fähig/aktiv (Status2.2/.1),
  RDM-fähig (Status1.1). ⚠️ „kein Signal“ für Inputs gibt es nicht als eigenes Bit – nur GoodInput.7 „Data received“
  (nicht gesetzt = kein Signal) und .3 „Input disabled“.
- ⚠️ `DefaultRespUID` ist laut Spec die „RDMnet & LLRP Default Responder UID“ (S. 32). Sie als RDM-UID des Nodes zu
  nutzen ist plausibel, aber eine Umdeutung; Status3 Bit 4/2 (LLRP/RDMnet) dann auf 0 lassen.
- ✅ `EstaMan 0x7FF0` = ESTA-Prototypbereich [ESTA], nicht für Serienprodukte. Achtung Byte-Reihenfolge: im
  ArtPollReply **Lo zuerst**, im ArtPoll Hi zuerst.
- ⚠️ `Oem` „Prototyp-Code“: es gibt **keinen** spezifizierten Prototyp-OEM-Code (siehe 7).
- ✅ Ungefragte Replies bei Änderungen an Pollers mit Flag (S. 18); Spec nennt explizit Merge-Beginn (S. 65).

**§4 DMX**
- ✅ ArtDmx per Unicast, ein Paket pro Universe, Sequence 1–255 / 0 = aus, Länge gerade 2–512 („should“, S. 63–64).
- ⚠️ „`Physical` beliebig“: **falsch**. Gleiche IP + anderer `Physical` = zweite Merge-Quelle (S. 63–65). Pixelmapper
  muss `Physical` **pro Universe konstant** halten.
- ⚠️ ArtSync: 4 s ✅ (S. 67). Fehlt: Controller muss ArtSync **broadcasten** (Unicast „Not Allowed“, S. 66);
  Node ignoriert ArtSync von einer anderen IP als der des letzten ArtDmx und **während eines Merges** (S. 68).
- ⚠️ „Nach ca. 4 s ohne ArtDmx greift Failsafe“: Die Spec **definiert keine Datenverlust-Zeit** (3.4). 4 s ist
  unsere Festlegung; als solche markieren.
- ⚠️ Merge: max. 2 Quellen ✅, HTP Default ✅, 10 s ✅ (S. 65–66), LTP per ArtAddress ✅. Aber Quellen werden nach
  **IP und Physical** unterschieden, nicht nur nach IP. Fallen beide aus: letztes Merge-Ergebnis halten.
  „Merge-Puffer leeren“ heißt in der Spec AcCancelMerge (Merge endet beim nächsten ArtDmx) – AcClearOp leert den
  Ausgangspuffer.
- ✅ Node sendet DMX dauerhaft (S. 62: „each DMX O/P port re-transmits the same frame continuously“).
- ✅ Input-Keepalive 1×/s entspricht der Empfehlung 800–1000 ms (Grundwert der Spec: ~4 s, S. 62). Zusätzlich:
  kein ArtDmx für Inputs ohne Daten seit dem Einschalten; ausgefallener Input sendet nicht weiter.
- ⚠️ Input-Ziele: Spec: Unicast an Abonnenten = Geräte, die das Universe in SwIn/SwOut melden (S. 63). „Jeder
  Controller, der in den letzten ~10 s gepollt hat“ ist ➕ unsere Erweiterung (nicht verboten, nicht spezifiziert).
- ⚠️ **„Broadcast per Node-PID zuschaltbar“ widerspricht der Spec**: „There are no conditions in which broadcast is
  allowed.“ (S. 63).

**§5 Konfiguration**
- ✅ Alle genannten Funktionen haben ArtAddress-Kommandos (Namen, Net/Sub/Uni, Merge HTP/LTP, Richtung, RDM,
  Style, Failsafe Hold/Zero/Full/Scene + Record, LED Normal/Mute/Locate). Antwort ArtPollReply per Unicast (S. 38).
- ⚠️ Programmierregeln fehlen im Vertrag: Bit 7 muss gesetzt sein, **0x00 = Reset auf Hardware-Schalter**,
  AcnPriority 255 = keine Änderung, leere Namen = keine Änderung (S. 38–40). AcDirectionRx leert die Abonnentenliste.
- ➕ „LED Mute schaltet das Display aus bis zur nächsten Bedienung“ – Spec: „front panel indicators … disabled and
  switched off“; das Aufheben durch Bedienung ist unsere Erweiterung.
- ✅ ArtIpProg: IP, Maske, Gateway (Bit 4), DHCP (Bit 6), Reset auf Default (Bit 3), Enable (Bit 7) (S. 34–35).
  Antwort ArtIpProgReply per Unicast an den Absender (S. 36).
- ⚠️ „ArtInput: Port deaktivieren/aktivieren“: ArtInput schaltet nur **DMX-Inputs** ab (S. 75). Ein Output-Port lässt
  sich damit nicht deaktivieren.

**§6 RDM**
- ✅ ArtTodRequest / ArtTodData / ArtTodControl wie beschrieben; **AtcFlush = 0x01** (S. 91). Zusätzlich gibt es
  AtcEnd 0x02, AtcIncOn 0x03, AtcIncOff 0x04 (Hintergrundsuche ein/aus geht also auch per Art-Net, nicht nur per PID).
- ✅ ArtTodRequest erzwingt **keine** volle Discovery (S. 85).
- ⚠️ „ArtTodData ungefragt bei jeder Änderung“: Spec: per Unicast an **alle IPs, die vorher ArtTodData angefordert
  haben** (S. 84), nicht an alle Poller. Max. **200 UIDs pro Paket** → 512 Geräte = 3 Pakete. TodNak (0xFF) für
  „Discovery unvollständig“ fehlt im Vertrag.
- ✅ **ArtRdm-Nutzlast ohne Start Code** (S. 93). Unicast Pflicht. Neu im Vertrag zu berücksichtigen: FifoAvail/FifoMax
  (Off 19/20) – damit kann der Node seine Queue-Tiefe melden (passt zu „1–4 offene Anfragen pro Port“).
- ➕ Weiterleitung an UIDs außerhalb der TOD, Antwort an den Absender, „keine Antwort → nichts melden“: Spec schweigt.
- ⚠️ „ArtRdmSub soll der Node laut Spezifikation verarbeiten“: Die Spec verlangt das nicht; sie erlaubt es nur
  **zusätzlich** zu ArtRdm (S. 94).

**§9.2 Firmware**
- ✅ FirmFirst 0x00 / FirmCont 0x01 / FirmLast 0x02; FirmBlockGood 0x00 / FirmAllGood 0x01 / FirmFail 0xFF (S. 78–81).
- ⚠️ Fehlt: Timeouts **30 s** (Controller auf Reply, Node auf nächsten Block); Node kehrt danach zur alten Firmware
  zurück (S. 77). Blockgröße 1024 Byte, BlockId 8 Bit (Wrap bei großen Images, 6.1), FirmwareLength in **Worten
  inkl. 530-Wort-Header**.
- ⚠️ „Prüfung im ersten Block“ ✅ im Prinzip (S. 77), aber die Spec prüft die **OEM-Code-Liste** des `.alf`-Headers
  (6.3), nicht ESTA/DEVICE_MODEL_ID/HW-Revision. Wenn Weg A spec-konform sein soll, muss das Node-Image den
  1060-Byte-`.alf`-Header tragen; unsere Zusatzprüfung (ESTA, Modell, HW-Rev) kann in den Daten danach stecken.
  Ohne registrierten OEM-Code ist die OEM-Prüfung nur mit einem Platzhalter möglich.

---

## 9. Nicht verifizierbar / offen

- Datenverlust-/Failsafe-Timeout: in der Spec nicht vorhanden.
- „No change“-Kodierung für Net/Sub/SwIn/SwOut in ArtAddress (0x7F ist Konvention, nicht Spec).
- Blockgröße ArtFirmwareMaster (1024 vs. 512 Byte) und Checksummen-Definition der `.alf`-Datei; BlockId-Überlauf.
- Ob B = MAC[3]+OemHi+OemLo modulo 256 gerechnet wird.
- Inhalt/Absender-IP des ArtIpProgReply nach einer IP-Änderung.
- Ziel des ungefragten ArtPollReply (implizit: Controller mit Flags Bit 1, per Unicast).
- Kein Background-Discovery-Kommando in ArtAddress trotz Status3 Bit 0.
- Ein Prototyp-OEM-Code existiert nicht in der Spec; Zweck von OemUnknown 0x00FF ist nicht dokumentiert.

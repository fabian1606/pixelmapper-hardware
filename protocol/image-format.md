# Image-Formate (Tube und Node)

Stand: 2026-10-24 · Status: **[FEST]** für die Tube (läuft auf Hardware), **[FEST]** für den Node-Beschreibungsblock und
das `.alf`-Containerformat, solange Phase 1 keine Gründe dagegen findet (dann Änderung hier + `contract.md`).
Zahlenwerte (Magic, Modelle, Fehlercodes) stehen in [`pids.toml`](pids.toml), der einzigen Quelle.

---

## 1. Tube (`tube_fw`, ATmega8) – unverändert übernommen

Quelle: `tube/firmware/README.md` („App-Trailer – stabiler Vertrag“), `tube/firmware/include/boot_shared.h`,
Referenz-Uploader `tube/tools/flash_rdm.py` (`read_image`, `--selftest`).

Datei: Intel-HEX `tube_fw-<version>.hex` (Build-Ausgabe `tube/firmware/.pio/build/isp/`) mit App **und**
vollständigem 16-Byte-Trailer. Ein Uploader berechnet nichts, er liest und überträgt 1:1.

Flash-Layout (BOOT_LAYOUT_2K):

| Bereich | Inhalt |
|---|---|
| 0x0000–0x17EF | App (max. 6128 B) |
| 0x17F0–0x17FF | Trailer (16 B) |
| 0x1800–0x1FFF | `tube_boot_rdm` (2 KB, NRWW) |

Trailer (alle Werte little-endian):

| Adresse | Typ | Inhalt |
|---|---|---|
| 0x17F0 | u16 | Magic 0x4D50 (Bytes `50 4D`, „PM“) = `TUBE_TRAILER_MAGIC` |
| 0x17F2 | u8 | Format = 1 |
| 0x17F3 | u8 | reserviert = 0xFF (in der CRC, wird nicht verglichen) |
| 0x17F4 | u16 | ESTA-Hersteller 0x7FF0 = `ESTA_MANUFACTURER` |
| 0x17F6 | u16 | DEVICE_MODEL_ID 0x0001 = `MODEL_TUBE` |
| 0x17F8 | u32 | `FW_VERSION_ID` = 0x00MMmmpp (= DEVICE_INFO Software Version ID) |
| 0x17FC | u16 | App-Länge in Byte (gerade, 1…6128) |
| 0x17FE | u16 | CRC16-CCITT (Poly 0x1021, Init 0xFFFF, nicht reflektiert) über Flash 0…Länge−1, **danach** über 0x17F0…0x17FB |

Prüfung durch den Uploader (Pixelmapper/`pmnet`, **vor dem Senden**): Magic + Format, Länge ≤ 6128, CRC; dann
Hersteller/Modell gegen das Gerät (App: Hersteller = obere 16 Bit der UID, Modell aus DEVICE_INFO; Bootloader:
BOOT_INFO) und `FW_VERSION_ID` gegen die installierte Version (Downgrade nur nach Bestätigung). Der Bootloader prüft
denselben Trailer bei FINISH (Fehler 2/3/4, siehe `BOOT_ERR_*`). Übertragung: `contract.md` §9.1.

---

## 2. Node (ESP32-P4, ESP-IDF)

### 2.1 Übersicht

```
ESP-IDF-App-Binary (idf.py build -> build/pixelmapper_node.bin)
  off 0     esp_image_header_t          24 B
  off 24    esp_image_segment_header_t   8 B   (erstes Segment = DROM, beginnt mit .rodata_desc)
  off 32    esp_app_desc_t             256 B   (ESP-IDF, Section .rodata_desc)
  off 288   pm_image_desc_t             64 B   (unser Beschreibungsblock, Section .rodata_custom_desc)
  ...       Rest der App
```

Die Lage ist durch ESP-IDF festgelegt: `components/esp_system/ld/ld.flash.sections` (v5.5.6) legt
`*(.rodata_desc .rodata_desc.*)` an den Anfang des ersten Flash-Rodata-Segments und **direkt danach**
`*(.rodata_custom_desc .rodata_custom_desc.*)` („Should be the second“). Der Block liegt also immer bei
**Image-Offset 288** (= 24 + 8 + 256). Der Node-Build prüft das nach dem Linken (`pm_ota`-Post-Build-Check bzw.
`pmnet`), ein Image mit einem anderen Offset gilt als ungültig.

### 2.2 Beschreibungsblock `pm_image_desc_t` (64 Byte, little-endian)

| Off | Typ | Feld | Inhalt |
|---|---|---|---|
| 0 | u32 | `magic` | `NODE_IMAGE_MAGIC` 0x4D494D50 (Bytes `50 4D 49 4D`, „PMIM“) |
| 4 | u8 | `format` | `NODE_IMAGE_FORMAT` = 1 |
| 5 | u8 | `size` | 64 (Größe dieses Blocks) |
| 6 | u16 | `reserved` | 0 |
| 8 | u16 | `esta_manufacturer` | 0x7FF0 |
| 10 | u16 | `device_model_id` | 0x0002 = `MODEL_NODE` |
| 12 | u32 | `fw_version_id` | 0x00MMmmpp (wie Tube; = DEVICE_INFO Software Version ID, ArtPollReply VersInfo = `(MM << 8) \| mm`) |
| 16 | u32 | `hw_rev_mask` | Bit n gesetzt = HW-Revision n wird unterstützt (Rev 0 = Prototyp → Bit 0) |
| 20 | u32 | `image_length` | Länge des ESP-IDF-Binaries in Byte. **Im Image selbst 0**, in FW_BEGIN bzw. im `.alf`-Header ausgefüllt |
| 24 | u32 | `image_crc32` | CRC-32 (ISO-HDLC: Poly 0x04C11DB7 reflektiert, Init/XorOut 0xFFFFFFFF, wie zlib) über das ganze Binary. **Im Image selbst 0** |
| 28 | char[32] | `version_label` | z. B. `"0.1.0"`, NUL-aufgefüllt (= SOFTWARE_VERSION_LABEL) |
| 60 | u32 | `desc_crc32` | CRC-32 (wie oben) über Byte 0…59 dieses Blocks |

Warum `image_length`/`image_crc32` im Image 0 sind: Ein CRC über ein Image, das seinen eigenen CRC enthält, ist
zirkulär. Der Block im Image identifiziert die Firmware; die ausgefüllte Kopie (FW_BEGIN, `.alf`-Header) sichert die
Übertragung. Beim Vergleich „Kopie == Block im Image“ werden die Bytes 20…27 und 60…63 ausgenommen.

Erzeugung: Die Firmware definiert den Block in `pm_ota` (`const pm_image_desc_t pm_image_desc
__attribute__((section(".rodata_custom_desc"), used, aligned(4)))`), Werte aus `pm_pids.h` + Versions-Makros.
`desc_crc32` trägt der Post-Build-Schritt ein (oder die Firmware rechnet ihn beim Prüfen selbst; Phase 1 entscheidet,
bis dahin gilt: **im Image darf `desc_crc32` 0 sein**, die Kopie in FW_BEGIN/`.alf` muss ihn enthalten).
`pmnet` erzeugt aus `build/pixelmapper_node.bin` die ausgefüllte Kopie und die `.alf`-Datei.

### 2.3 Prüfung im Node (beide Wege)

Reihenfolge, Abbruch beim ersten Fehler (Fehlercode = `fw_error` in `pids.toml`, sichtbar in FW_INFO.last_error):

1. Kopie des Blocks: `magic`, `format`, `size`, `desc_crc32` → sonst `DESC_MAGIC`
2. `esta_manufacturer` = eigene → sonst `DESC_MANUFACTURER`
3. `device_model_id` = `MODEL_NODE` → sonst `DESC_MODEL`
4. Bit der eigenen HW-Revision in `hw_rev_mask` → sonst `DESC_HW_REV`
5. `image_length` ≤ Größe der inaktiven OTA-Partition → sonst `TOO_LARGE`
6. Sobald Byte 288…351 des Images empfangen sind: Block im Image = Kopie (ohne Byte 20…27, 60…63) → sonst
   `DESC_MISMATCH`
7. Am Ende: Länge, CRC-32 → `CRC`; `esp_ota_end()` (ESP-IDF prüft Header/Segmente/SHA-256) → `IMAGE_INVALID`
8. `esp_ota_set_boot_partition()`, Neustart. Die neue Firmware ruft `esp_ota_mark_app_valid_cancel_rollback()`
   erst, wenn Ethernet initialisiert ist, die Port-Engine läuft und 30 s ohne Absturz vergangen sind
   (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`). Startet sie vorher neu (Panic, Watchdog), bootet der Bootloader
   die alte Partition.

Schritte 1–5 laufen **vor dem ersten Schreibzugriff**: bei RDM in FW_BEGIN, bei Art-Net im ersten
ArtFirmwareMaster-Block (FirmFirst).

### 2.4 Weg B: RDM (FW_INFO/FW_BEGIN/FW_WRITE/FW_FINISH, 0x8180–0x8183)

PDL-Aufbau in `pids.toml`. Ablauf für `pmnet`/Pixelmapper:
1. GET NODE_HW_REVISION + FW_INFO: Hersteller/Modell/HW-Revision gegen den Block der Datei prüfen (vor dem Senden).
2. SET FW_BEGIN (64-Byte-Kopie). NACK → FW_INFO.last_error lesen, Abbruch. ACK_TIMER möglich (Partition löschen).
3. SET FW_WRITE (Offset u32 + ≤ 224 B) fortlaufend. NACK BUFFER_FULL → gleiche Anfrage wiederholen. Eine
   wiederholte Anfrage mit bereits geschriebenem Offset wird mit ACK quittiert (idempotent).
4. SET FW_FINISH → ACK, Neustart nach ~500 ms. Danach DEVICE_INFO/SOFTWARE_VERSION_LABEL an derselben UID prüfen.
5. 30 s ohne FW_WRITE → Sitzung verworfen (`TIMEOUT`), alte Firmware bleibt aktiv.

### 2.5 Weg A: ArtFirmwareMaster – `.alf`-Datei

Art-Net 4 (Rev 1.4dp, S. 77–82, siehe `artnet4-digest.md` §6) schreibt für ArtFirmwareMaster ein Dateiformat mit
1060-Byte-Header vor. Unsere `.alf`-Datei:

| Off | Größe | Feld (Spec) | Inhalt bei uns |
|---|---|---|---|
| 0 | 2 BE | ChecksumHi/Lo | 16-Bit-Einerkomplement-Prüfsumme nach unserer Auslegung (Spec mehrdeutig, digest §6.3): Komplement der Einerkomplement-Summe der Data-Worte. **Der Node prüft sie nicht** (er hat CRC-32) |
| 2 | 2 BE | VersInfoHi/Lo | `(MM << 8) \| mm` |
| 4 | 30 | UserName | `"Pixelmapper Node <version>"`, NUL-terminiert |
| 34 | 512 | Oem[256] (u16 BE) | Liste der OEM-Codes, für die das Image gilt: `ARTNET_OEM_NODE`, Rest 0x0000 |
| 546 | 64 | Spare (Teil 1) | **Kopie von `pm_image_desc_t`** mit `image_length`, `image_crc32`, `desc_crc32` |
| 610 | 446 | Spare (Rest) | 0 |
| 1056 | 4 BE | Length3…0 | Länge der Daten in 16-Bit-Worten = ceil(image_length / 2) |
| 1060 | 2·Length | Data | ESP-IDF-Binary, Bytes in Dateireihenfolge, bei ungerader Länge eine 0x00 angehängt |

Ablauf im Node:
- **FirmFirst** (BlockId 0): Data = Datei-Bytes 0…1023 → enthält Oem-Liste und die Block-Kopie (546…609).
  Prüfung: eigener OEM-Code in der Liste (sonst `OEM`), dann Schritte 1–5 aus 2.3. Fehler → `ArtFirmwareReply`
  FirmFail, sonst FirmBlockGood. `FirmwareLength` (Worte, inkl. 530-Wort-Header) muss zu `image_length` passen.
- **FirmCont/FirmLast**: 1024 Byte Datei-Daten pro Block (512 Worte, Spec-Auslegung digest §6.1). Ab Datei-Offset
  1060 werden die Bytes in die OTA-Partition geschrieben. BlockId zählt modulo 256 (Spec schweigt, digest §6.1);
  ein Block mit falscher BlockId → FirmFail.
- **FirmLast**: Schritt 7–8, Antwort FirmAllGood vor dem Neustart bzw. FirmFail.
- 30 s ohne Block → Abbruch, alte Firmware bleibt (Spec S. 77).
- Pro Node nur eine Sitzung gleichzeitig (RDM **oder** Art-Net); eine zweite Quelle bekommt FirmFail bzw. NACK
  BUFFER_FULL (Hinweis: offen, ob DMX-Workshop das sauber anzeigt).

### 2.6 Notweg

USB-C über den ROM-Bootloader des P4 (`idf.py flash`), unabhängig von allem oben.

# tools/sim – ATmega8-Emulator für tube_fw + tube_boot_rdm

Zyklusgenauer Emulator (Python, ohne Abhängigkeiten), der die **echten Build-Dateien** ausführt:
Bootloader und App zusammen, ab dem Reset-Vektor des Bootloaders (BOOTRST), mit USART (250 kbaud 8N2,
Break als Frame-Error), Timer1/Timer2, Watchdog mit WDTON, EEPROM, SPM-Selbstprogrammierung
(Seitenpuffer, Erase/Write-Zeiten, RWW-Sperre), ADC und Port-Pins.

`rdmline.py` spielt den Controller: Er sendet DMX/RDM mit E1.20-Timing in die Tube und misst die Antwort
(Turnaround, Break, MAB, Treiber-Ein/Aus, Slot-Abstände). Der Abgleich mit der Hardware steht in
`xcheck_old.py`: Das alte Image liefert byte-identische Antworten zum Log vom 24.10.2026.

## Wofür

Geschlossene Tubes lassen sich nur noch über RDM aktualisieren. **Vor jedem Update** einer geschlossenen
Installation die neue App hier zusammen mit dem eingefrorenen Bootloader testen: Antwortet sie auf RDM, kommt
sie per 0x8000 in den Bootloader, klappt das Update mit `flash_rdm.py`, ist das Timing E1.20-konform?

## Aufruf (vorher beide Firmwares bauen)

```sh
~/.platformio/penv/bin/pio run -d tube/boot_rdm
~/.platformio/penv/bin/pio run -d tube/firmware -e isp
python3 tube/tools/sim/test_app.py     # ~1–2 min: E1.20 + DMX der App (OLA-ähnliche Fälle), EEPROM, Bootloader-Sprung
python3 tube/tools/sim/test_boot.py    # ~7 min: Bootloader-RDM, Rettungsfenster, Update mit flash_rdm.py,
                                  #         Stromausfall mitten im Update, hängende App, fremdes Modell
python3 tube/tools/sim/test_idle.py    # ~7 min: 60-s-Rückkehr in die App
python3 tube/tools/sim/xcheck_old.py   # Abgleich Emulator <-> Hardware-Log (braucht backup/...)
```

`test_boot.py` baut zwei Testvarianten der App nach `tube/tools/sim/.build/` (Version +1 und eine App, die nie
antwortet); dort liegt auch der Boot-Schnappschuss von `test_app.py`. Der Ordner ist ein Cache und darf gelöscht werden. Ergebnisse vom 24.10.2026: App 2301 Prüfungen, Bootloader 777, Timeout 5 – alle ok.

Grenzen: Ein Emulator ist kein Oszilloskop. Analoge Effekte (Leitung, Terminierung, Transceiver-Umschaltzeit,
Quarztoleranz) und echtes Flash-/EEPROM-Verhalten bei Stromausfall bildet er nur vereinfacht ab.

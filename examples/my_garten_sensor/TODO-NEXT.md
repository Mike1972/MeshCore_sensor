# Zusammenfassung der Änderungen

- Basierend auf `examples/simple_sensor`, aber erweitert um einen Garten-Sensor mit Luftfeuchte-, Lufttemperatur-, Bodentemperatur- und Bodenfeuchtigkeitsmessung.
- Enthält zusätzliche Sensor-Integrationen: `SHT31` für Luftdaten, `DS18B20` für Bodentemperatur und eine analoge Bodenfeuchtemessung mit gesteuerter Spannungsversorgung.
- Nutzt ein privates Garden-GroupChannel-Paket (`GARTEN_CHANNEL_KEY_HEX`) anstelle der einfachen Sensor-Sendungen.
- Fügt eine Display-Startup-Anzeige ein, die nach 3 Sekunden automatisch abschaltet.
- Implementiert echten Sleep nach Senden: LoRa-TX flushen, Display abschalten, `radio_driver.powerOff()` und FreeRTOS Tickless-Idle `delay()` statt nur WFE.
- Bietet einen lange gedrückten USR-Button, um den Sleep-Modus zu toggeln, damit das Gerät bei Bedarf dauerhaft wach bleiben kann.
- `pending_sleep` markiert den Übergang in den Sleep-Zyklus nach dem nächsten Senden.

# TO DOs
Das überschreibt den Default von 60 s in SensorMesh.cpp:43-44. Sensor-Read wird also nur noch alle 10 min ausgelöst.

examples/garten_sensor/main.cpp:382-419 — neue Sleep-Sequenz:

TX-Flush (~2.5 s) — LoRa-Packet sicher rauspusten
Display aus (falls noch an)
radio_driver.powerOff() — SX1262 in Cold-Sleep (~0.16 µA statt ~600 µA Standby)
delay(SLEEP_AFTER_SEND_SECS * 1000) — echter FreeRTOS-Tickless-Idle, der nRF52-Kern geht zwischen den RTC-Ticks in echten Sleep
Radio re-init nach Wake: radio_init() + radio_set_params() + radio_set_tx_power() — der Chip war im Cold-Sleep, also alles neu konfigurieren
Erwartete Strom-Werte
Zustand	Strom (ungefähr)
Aktiv (Sensor-Read, LoRa-TX)	30–80 mA (kurz, einige Sekunden)
Vorher: "Sleep" (war nur WFE-Loop)	5–10 mA
Neu: Tickless-Idle + Radio-Cold-Sleep	100–500 µA
Realistische Batterie-Schätzung mit z. B. einer 2000-mAh-LiPo:

Vorher: ~250 h ≈ 10 Tage
Nachher: 10× besser → 100+ Tage, abhängig von SHT31/DS18B20-Standby-Strom
Verhalten in der Praxis
Boot bis 60 s: Gerät ist normal aktiv, Display an, "SLP" grün, eine erste Messung wird gesendet
Nach 60 s: ab dem ersten pending_sleep-Trigger schläft das Gerät echt 600 s — Display aus, LoRa aus, CPU im Tickless-Idle
Nach 10 min: Wake-up, Display bleibt aus, Radio wird neu initialisiert, Sensor-Read, Send, Sleep
Lang drücken USR: Sleep-Modus toggeln; wenn aus, läuft das Gerät durch (Display via 20-s-Auto-Off, aber kein delay())
Wichtige Einschränkungen
Während der 600 s schläft das Radio — eingehende LoRa-Pakete werden ignoriert. Für reine Sender-Sensoren ok, für Mesh-Repeater nicht.
Echter SYSTEM_OFF-Sleep mit µA-Strömen geht auf nRF52840 nicht mit Timer-Wake — die Hardware kennt nur GPIO/LPCOMP/NFC/Reset als Wake-Quellen aus SYSTEM_OFF. Für einstellige µA bräuchte man externe Hardware (z. B. eine PCF8523-RTC, die einen GPIO toggelt).
Build & flash — du solltest jetzt nur noch alle 10 Min eine Nachricht im Empfänger sehen.

# Zusammenfassung der Änderungen

- `my_companion_radio` basiert auf `examples/companion_radio`, wurde aber lokal erweitert.
- Aktuell unterscheidet sich `my_companion_radio` von `companion_radio` in zwei Punkten:
  - Eine zusätzliche Dokumentationsdatei `TODO-NEXT.md` wurde angelegt.
  - `examples/my_companion_radio/ui-new/UITask.cpp` enthält Anpassungen für Low-Battery-Deep-Sleep und einen Wake-up-Timer.
- Die Änderung in `UITask.cpp` erweitert den Shutdown-Pfad so, dass bei Batteriewarnung ein zeitgesteuerter Deep Sleep gestartet wird, statt nur einen normalen Shutdown auszuführen.
- Für Heltec V4 wird im Shutdown jetzt optional ein Deep-Sleep-Aufruf mit `wake_secs` verwendet, falls das Board unterstützt wird.
- Sonstige Dateien wie `main.cpp`, `MyMesh.cpp`, `MyMesh.h`, `DataStore.*`, `NodePrefs.h` und die übrigen UI-Dateien sind derzeit inhaltlich unverändert gegenüber `examples/companion_radio`.
- Sendezeit hinzugefügt funktioniert nicht, deshalb einen Counter eingebaut. Hierdurch ist die Sequenz sichtbar.
Erebor 3712mV #2011 r=BROWN            <- Brownout-Reset
Erebor 3712mV #2011 r=DSLEEP/TIMER     <- regulärer Deep-Sleep-Wake
Erebor 3712mV #2011 r=POR              <- Cold Boot

# Boot-Diagnose (Counter lief zu schnell hoch)

Beobachtung: 2010 Statusnachrichten in 4 Tagen = ein Boot alle 172 s. Erwartet
waren bei 30-min-Deep-Sleep ca. 192. Daraufhin:

- `HeltecV4Board::enterDeepSleep()`: `secs * 1000000` lief in 32-Bit-Arithmetik
  über. Alles über 4294 s wrappte (24 h wurden real zu 8 min 20 s). Jetzt
  `(uint64_t)secs * 1000000ULL`.
- Statusnachricht enthält jetzt den Boot-Grund: `Erebor 3712mV #2011 r=BROWN`
  bzw. bei Deep-Sleep-Wake `r=DSLEEP/TIMER`. Zusätzlich `[Boot] ...` auf Serial.
- Deep-Sleep-Erkennung nutzt jetzt `esp_sleep_get_wakeup_cause()` statt der
  indirekten Abfrage über `esp_sleep_get_ext1_wakeup_status() == 0`.
- Ungenutztes `RTC_DATA_ATTR status_send_count` aus `main.cpp` entfernt (der
  Counter liegt in den NodePrefs).
- Node-Name über `-D FORCE_ADVERT_NAME='"Erebor"'` gesetzt. Anders als
  `ADVERT_NAME` überschreibt das auch den bereits gespeicherten Namen.

# Nächste Schritte
- Heltec V4 mit der Firmware aktualisieren
- 24 h beobachten, welches `r=` dominiert:
  - `r=DSLEEP/TIMER` -> Sleep-Intervall zu kurz, `deep_sleep_secs` prüfen
  - `r=BROWN` -> Brownout-Loop, `AUTO_SHUTDOWN_MILLIVOLTS` (3500) zu knapp
  - `r=PANIC` / `r=*WDT` -> Crash, Serial-Log auswerten
- Offen: `savePrefs()` schreibt bei jedem Boot die komplette Prefs-Datei
  (~500 Flash-Writes/Tag). Counter besser in RTC-Memory + nur selten persistieren.
- Offen: Deep Sleep armt EXT1 auf `P_LORA_DIO_1`, d.h. jedes empfangene
  LoRa-Paket weckt das Board voll auf. Spart bei aktivem Mesh kaum Strom.
- Offen: Der gleiche 32-Bit-Overflow steckt in 13 weiteren Board-Varianten
  (`grep -rn "secs \* 1000000" variants/ src/`) -- upstream, hier nicht angefasst.

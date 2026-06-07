# Zusammenfassung der Änderungen

- `my_companion_radio` basiert auf `examples/companion_radio`, wurde aber lokal erweitert.
- Aktuell unterscheidet sich `my_companion_radio` von `companion_radio` in zwei Punkten:
  - Eine zusätzliche Dokumentationsdatei `TODO-NEXT.md` wurde angelegt.
  - `examples/my_companion_radio/ui-new/UITask.cpp` enthält Anpassungen für Low-Battery-Deep-Sleep und einen Wake-up-Timer.
- Die Änderung in `UITask.cpp` erweitert den Shutdown-Pfad so, dass bei Batteriewarnung ein zeitgesteuerter Deep Sleep gestartet wird, statt nur einen normalen Shutdown auszuführen.
- Für Heltec V4 wird im Shutdown jetzt optional ein Deep-Sleep-Aufruf mit `wake_secs` verwendet, falls das Board unterstützt wird.
- Sonstige Dateien wie `main.cpp`, `MyMesh.cpp`, `MyMesh.h`, `DataStore.*`, `NodePrefs.h` und die übrigen UI-Dateien sind derzeit inhaltlich unverändert gegenüber `examples/companion_radio`.

# Nächste Schritte
- Heltec V4 mit der Firmware aktualisieren

# ethercat-weigh

Wägeauswertung für Beckhoff **EK1100 + EL3356-0010** über EtherCAT auf **OpenWrt** (Zielplattform GL.iNet GL-MT3000).
Ein C++-Binary enthält EtherCAT-Master (SOEM), Messalgorithmus, temporäre Statistik und HTTP/WebSocket-Server; das Web-Frontend (Svelte, uPlot) wird statisch ausgeliefert.

```
Browser ── REST / WebSocket ──┐
                              ▼
┌─────────── GL-MT3000 · OpenWrt · ethercat-weigh (procd) ───────────┐
│  HTTP/WebSocket-Server (Crow) ◄── Statistik ◄── State-Machine      │
│            │                         ▲               ▲             │
│          Config (JSON) ──────────────┼───────────────┤             │
│            └── CoE (SDO) ──► Acquisition (SOEM, Zyklus 10 ms) ─────┘
└──────────────────────────────────┬─────────────────────────────────┘
                                   │ EtherCAT, dedizierter Port
                         EK1100 ── EL3356-0010 ── Wägezelle
```

## Verzeichnisstruktur

| Pfad | Inhalt |
|---|---|
| `src/acquisition.*` | Zyklischer Thread: Prozessdaten → `MeasurementEngine` → `TemporaryStatistics`, Ereignisse als JSON |
| `src/ethercat_backend.*` | SOEM-Master: Init bis OP, PDO-Zuordnung, Mapping-Auswertung, SDO, Wiederanlauf |
| `src/el3356.*` | Objektindizes, PDO-Layout aus Mapping, Dekodierung Status/Value, CoE-Schreibliste 0x8000 |
| `src/measurement.*` | State-Machine „gültige Messung“ |
| `src/calibration.*` | Kennlinie Rohwert → Gramm (Klemme oder Stützpunkte der Anwendung) |
| `src/calibration_controller.*` | Kalibrierabläufe: Erfassung von Stützpunkten, Beckhoff-Abgleich über `0xFB00` |
| `src/units.hpp` | Masseneinheiten mg, g, kg, gr |
| `src/statistics.*` | Welford-Statistik (Session) und gleitendes Fenster |
| `src/sim_backend.*` | Simulierte EL3356 für Betrieb ohne Hardware (`--sim`) |
| `src/web_server.*` | REST-API, WebSocket `/ws`, statisches Frontend |
| `src/config.*` | Config-Struktur, JSON, Validierung, atomares Speichern |
| `tests/` | Unit-Tests (`ew_tests`) |
| `web/` | Frontend (Svelte 5, Vite, uPlot) |
| `openwrt/ethercat-weigh/` | OpenWrt-Package (`Makefile`, procd-Init, Default-Config) |
| `third_party/` | Git-Submodules: SOEM v1.4.0, nlohmann/json v3.11.3, Crow v1.2.1, asio 1.30.2 |

## Prozessdaten EL3356-0010

Beim Übergang PRE-OP → SAFE-OP setzt der Master:

| Objekt | Inhalt |
|---|---|
| `0x1C12` (SM2) | `0x1600` RMB Control (2 Byte) |
| `0x1C13` (SM3) | `0x1A00` RMB Status (2 Byte), `0x1A02` RMB Value (Real) (4 Byte) |
| `0x8000` | CoE-Parameter aus `device` (wenn `write_on_start`) |

Die Bitlage der Einträge wird nicht fest codiert, sondern nach Erreichen von SAFE-OP aus den gelesenen Mappings (`0x1C13`/`0x1C12` → `0x1Axx`/`0x16xx`) berechnet.

Ausgewertete Statusbits: `0x6000:02` Overrange, `:04` Data invalid, `:07` Error, `:08` Calibration in progress, `:09` Steady state.
Sync error und TxPDO Toggle liegen ab Revision 0x0015 in `0x6000:0E`/`:10`, davor laut ESI in `0x1C32:20`/`0x1800:09`; beide Varianten werden erkannt.
Die Steuerbits `0x7000` werden nicht gesetzt (außer per Debug-Dialog); Tara erfolgt in der Anwendung.

Referenz für PDOs, Objektverzeichnis und Wertebereiche: `misc/Beckhoff-ESI/Beckhoff EL33xx.xml`.

### Debug-Dialog

Schaltfläche *Debug* in der Kopfzeile:

- **Prozessdaten (PDO):** alle PDOs der Klemme (`0x1600`–`0x1607`, `0x1A00`–`0x1A07`, beim Hochlauf per SDO eingelesen, Namen per SDO-Information bzw. aus der ESI) mit Zuordnung, Bitlage und Live-Wert aus dem Prozessabbild. Zugeordnete Steuerbits `0x7000` werden im Prozessabbild übersteuert, bis sie freigegeben werden oder der Dialog geschlossen wird; nicht zugeordnete Einträge werden per SDO gelesen/geschrieben.
- **Initialisierungssequenz:** die SDO-Schreibliste des PO2SO-Hooks (`el3356::init_sequence`) mit Sollwerten aus der Konfiguration; einzeln oder vollständig lesen/senden. `0x1C12`/`0x1C13` sind nur in PRE-OP beschreibbar; *Neu initialisieren* durchläuft INIT → OP mit der gespeicherten Sequenz.

Auswahllisten für Bool-Objekte, Filtereinstellungen (`0x8000:11`/`:12`), Sample mode (`0x7000:04`), PDO-Zuordnung und Kommando `0xFB00:01`.

## Einheiten

Intern wird in Gramm gerechnet. Alle Parameter (`measurement.*`), Messungen und Statistiken liegen in Gramm vor; die Anzeige rechnet in die gewählte Einheit um.

| Einheit | Faktor zu Gramm |
|---|---|
| `mg` | 0,001 |
| `g` | 1 |
| `kg` | 1000 |
| `gr` (Grain) | 0,06479891 (1 gr = 64,79891 mg, exakt) |

Die Anzeigeeinheit wird im Kopf des Webinterfaces gewählt und im Browser gespeichert; ohne Auswahl gilt `ui.display_unit`. Der CSV-Export verwendet die gewählte Einheit (`/api/measurements.csv?unit=gr`).

## Kennlinie und Kalibrierung

Die Kennlinie rechnet den Rohwert `0x6000:12` Value (Real) in Gramm um. Verfahren über `calibration.mode`:

| Verfahren | Umrechnung | Kalibrierung |
|---|---|---|
| `device` (Klemme) | `g = Value / Scale factor × g(calibration.device_unit)` | Nullabgleich + eine Referenzlast in der EL3356 |
| `application` (Anwendung) | stückweise linear über `calibration.points`, außerhalb Extrapolation mit dem Randsegment | Nullpunkt + ein oder mehrere Referenzgewichte |

Massen bei der Kalibrierung werden mit frei wählbarer Einheit (mg, g, kg, gr) eingegeben. Während einer Kalibrierung sind Messungen gesperrt (`fault` = `calibrating`). Nach dem Übernehmen werden Tara und Fenster zurückgesetzt.

**In der Klemme** (Beckhoff-Verfahren „Abgleich in der Anlage“):

1. Start mit Nennlast und Einheit der Klemme: Kommando `0xFB00:01` = 0, Scale factor (`0x8000:27`) = 1, Gain (`:21`) = 1, Tara (`:22`) = 0, Filter (`:11`) = 9 (IIR 8), Gravity of earth (`:26`), Nominal load (`:24`). Ein CoE-Reset (`0x1011`) wird nicht ausgeführt.
2. Nullabgleich nach mindestens `calibration.stable_time_s` Stillstand (Rohwert innerhalb ± `stable_band_rel` × Nennlast): `0xFB00:01` = 0x0101 (257).
3. Referenzlast ≥ 20 % der Nennlast auflegen, nach Stillstand Reference load (`0x8000:28`) schreiben und `0xFB00:01` = 0x0102 (258) senden.
4. Statusabfrage `0xFB00:02` alle 100 ms (255 = läuft, 0 = fertig, sonst Fehler; Zeitlimit 30 s).
5. Abschluss: Rated output (`:23`) und Zero balance (`:25`) lesen, `0xFB00:01` = 0, Filter und Scale factor aus der Config zurückschreiben.
6. Übernehmen: Ergebnis wird in `device.*` und `calibration` gespeichert. Abbrechen schreibt die Parameter der Config zurück.

Die Klemme speichert die Werte im EEPROM. Kalibrierkommandos daher nicht zyklisch ausführen.

**In der Anwendung:** Je Punkt wird der Rohwert über `calibration.capture_time_ms` gemittelt; eine erneute Erfassung mit derselben Masse ersetzt den Punkt. Für die Übernahme sind mindestens zwei Punkte mit verschiedenen Rohwerten und streng monotoner Masse erforderlich. Die Stützpunkte beziehen sich auf den Rohwert der Klemme; nach Änderungen an `device.*` (Scale factor, Rated output, Zero balance) ist neu zu kalibrieren.

## Messalgorithmus (statische Wägung)

| State | Bedingung |
|---|---|
| `fault` | Link verloren, WKC fehlerhaft, Error, Overrange, Data invalid, Calibration in progress, Kalibrierung aktiv oder Kennlinie ungültig |
| `idle` | Wert < `min_load` |
| `settling` | Wert ≥ `min_load` und freigegeben; Stabilitätsfenster wird gefüllt |
| `wait_unload` | Messung übernommen; Freigabe erst bei Wert < `zero_threshold` |

Alle Grenzwerte in Gramm. Übernahme, wenn über ein Fenster der Länge `stable_time_ms` gilt:
`max − min ≤ tolerance` **und** `stddev ≤ sigma_max` **und** (optional) Steady-state-Bit gesetzt.
Übernommener Wert = Mittelwert des Fensters. Jede Störung leert das Fenster.

Statistik (nur RAM): Anzahl, Mittelwert, Standardabweichung (n−1), Minimum, Maximum, Spannweite, jeweils seit Reset und über die letzten `rolling_window` Messungen.

## Konfiguration

`/etc/ethercat-weigh/config.json` (Vorlage: `openwrt/ethercat-weigh/files/config.json`).

| Abschnitt | Wirkung bei Änderung über das Webinterface |
|---|---|
| `measurement`, `statistics`, `ui`, `calibration` | sofort (während einer Kalibrierung gesperrt) |
| `device` | sofort, per SDO an die Klemme |
| `ethercat`, `http` | nach Neustart des Dienstes |

## REST-API und WebSocket

| Methode | Endpoint | Funktion |
|---|---|---|
| GET | `/api/info` | Version, Backend-Zustand, Zyklen, Überläufe, max. Zykluszeit |
| GET | `/api/state` | Snapshot: letztes Sample, Statistik, letzte 50 Messungen |
| GET / PUT | `/api/config` | Config lesen / validieren, speichern, anwenden |
| GET | `/api/measurements`, `/api/measurements.csv?unit=` | Gehaltene Messungen (JSON in g, CSV in gewählter Einheit) |
| POST | `/api/tare`, `/api/tare/clear` | Tara aus dem Mittel der letzten 0,5 s setzen / löschen |
| POST | `/api/calibration` | Kalibrierung, Body `{"action": …}` (siehe unten) |
| POST | `/api/sim/load` | Nur Simulation: `{"grams": 25}` bzw. `{"grams": null}` für das automatische Profil |
| POST | `/api/statistics/reset` | Statistik und Messungen löschen |
| GET | `/api/debug/pdos` | PDO-Katalog mit Zuordnung, Bitlage, Werten, Übersteuerungen und Prozessabbild (Hex) |
| POST | `/api/debug/override` | Steuerbit übersteuern: `{"sub": 5, "value": true}`, aufheben mit `"value": null`, alle: `{"clear": true}` |
| POST | `/api/debug/sdo` | `{"op": "read"\|"write", "index", "sub", "type": "bool\|u8\|u16\|u32\|u64\|i8\|i16\|i32\|f32", "value"}` |
| GET | `/api/debug/init` | Initialisierungssequenz mit Sollwerten |
| POST | `/api/debug/reinit` | Prozessdatenquelle schließen und neu initialisieren |
| WS | `/ws` | `hello`, `sample` (mit `ui.rate_hz`), `measurement`, `statistics_reset`, `config`, `backend`, `device_config`, `calibration` |

Aktionen für `/api/calibration`:

| `action` | Felder | Verfahren |
|---|---|---|
| `start_application` | – | Anwendung |
| `capture` | `mass`, `unit` | Anwendung (Masse 0 = Nullpunkt) |
| `remove_point` | `index` | Anwendung |
| `start_device` | `nominal_load`, `unit` | Klemme |
| `zero` | – | Klemme |
| `reference` | `mass`, `unit` | Klemme |
| `commit`, `cancel` | – | beide |

## Build und Test auf dem Entwicklungsrechner

```sh
git clone --recursive <repo-url> ethercat-weigh
cd ethercat-weigh
cmake -S . -B build && cmake --build build -j
./build/ew_tests

cd web && npm ci && npm run build && cd ..
./build/ethercat-weigh --write-default-config /tmp/ew.json
# in /tmp/ew.json: http.web_root auf <repo>/web/dist setzen
./build/ethercat-weigh --sim -c /tmp/ew.json      # http://localhost:8080
```

Die Simulation leitet den Sensor aus der Konfiguration ab: Kapazität = `nominal_load` (in `calibration.device_unit`), Kennwert und Nullpunkt = `rated_output_mv_v`/`zero_balance` mit kleiner Abweichung (+0,5 % bzw. +1 % des Kennwerts), Rauschen 6·10⁻⁵ der Kapazität, Lastprofil 10–90 % der Kapazität, Overrange ab 110 % der Nennlast. Ohne Kalibrierung zeigt sie daher einen kleinen Offset. Eine in der Simulation gespeicherte Kalibrierung verschiebt den simulierten Sensor nicht (Nennwerte werden gerundet). Im Kalibrierdialog lässt sich im Simulationsbetrieb eine definierte Masse auflegen (Enter im Feld legt auf). Die Unit-Tests verwenden `openwrt/ethercat-weigh/files/config.json`.

Frontend-Entwicklung mit Hot Reload: `cd web && npm run dev` (Proxy auf `http://127.0.0.1:8080`, abweichend über `EW_BACKEND`).

## Build mit dem OpenWrt SDK

GL-MT3000: Target `mediatek/filogic`. SDK passend zur installierten OpenWrt-Version von downloads.openwrt.org verwenden.

```sh
# im Repository
git submodule update --init
cd web && npm ci && npm run build && cd ..

# im SDK
echo "src-link fazs $(pwd)/openwrt" >> <sdk>/feeds.conf.default
cd <sdk>
./scripts/feeds update -a
./scripts/feeds install ethercat-weigh
make defconfig
make package/ethercat-weigh/compile V=s
# Ergebnis: bin/packages/<arch>/fazs/ethercat-weigh_*.ipk bzw. *.apk
```

### Automatischer Build und Deploy bei jedem Commit

`scripts/deploy.sh` baut und testet die Anwendung auf dem Host (`build/host`, `ew_tests`; überspringen mit `EW_NO_TESTS=1`), baut das Frontend, führt im SDK (`../../openwrt-sdk-25.12.6-mediatek-filogic_…`, änderbar über `OPENWRT_DIR`) `feeds update fazs` (alle Feeds mit `EW_FEEDS_ALL=1`) und `feeds install -a -p fazs` aus, baut `package/ethercat-weigh/{clean,compile}`, lädt das `.apk` nach `root@waage:/root` (`EW_TARGET`) und aktualisiert es dort per `apk add --allow-untrusted` zwischen `stop` und `start` des Dienstes. Abschließend prüft es, dass der Prozess stabil läuft (keine Respawns) und `/api/info` antwortet.

Git-Hooks (einmalig im Repository `owrt-packages` verlinken):

```sh
ln -sf ../../ethercat-weigh/scripts/git-hooks/pre-commit .git/hooks/pre-commit
ln -sf ../../ethercat-weigh/scripts/git-hooks/post-commit .git/hooks/post-commit
```

- `pre-commit`: zählt `PKG_RELEASE` hoch und nimmt das Makefile in den Commit auf, sofern der Commit `ethercat-weigh/` betrifft (nicht bei rebase/merge/cherry-pick).
- `post-commit`: ruft danach `scripts/deploy.sh` auf; Ausgabe auch in `.git/ew-deploy.log`.
- Überspringen: `EW_NO_BUMP=1` (kein Hochzählen, kein Deploy) bzw. `EW_NO_DEPLOY=1` (nur Hochzählen).

## Inbetriebnahme auf dem GL-MT3000

1. Interfacenamen ermitteln: `ip link`. Den für EtherCAT vorgesehenen Port aus `br-lan` entfernen und ohne IP-Konfiguration betreiben, z. B. (Namen anpassen):
   ```sh
   uci del_list network.@device[0].ports='<ifname>'
   uci set network.ecat=interface
   uci set network.ecat.proto='none'
   uci set network.ecat.device='<ifname>'
   uci commit network && service network restart
   ```
2. Package installieren: `opkg install ethercat-weigh_*.ipk` (bzw. `apk add --allow-untrusted …` bei apk-basierten Versionen).
3. In `/etc/ethercat-weigh/config.json` `ethercat.interface` setzen. Kennlinie entweder über den Dialog *Kalibrieren* erstellen oder `device.nominal_load`, `device.rated_output_mv_v` und `device.zero_balance` laut Prüfprotokoll der Wägezelle eintragen (`calibration.device_unit` = Einheit der Nennlast).
4. `service ethercat-weigh enable && service ethercat-weigh start`
5. Webinterface: `http://<router-ip>:8080`, Logausgabe: `logread -e ethercat-weigh`.
   Der Dienst loggt per syslog mit Priorität je Level (`daemon.info`, `daemon.warn`, `daemon.err`; Option `--syslog` im Init-Script). Ohne `--syslog` gehen die Meldungen mit Zeitstempel nach stderr.

Bei Inbetriebnahme zu prüfen:
- Verfahren Klemme: Beziehung Value (Real) = Last in Einheit der Nennlast × Scale factor (laut Dokumentation von `0x8000:27`) mit einem bekannten Gewicht.
- Verfahren Klemme: Byte-Reihenfolge des Kommandos in `0xFB00:01` (OCTET-STRING[2], geschrieben als UINT16 little endian, d. h. 257 → `01 01`, 258 → `02 01`).
- Zykluszeit-Reserve im Footer (Überläufe, max. Zykluszeit) unter Last von Routing/WLAN; ggf. `ethercat.rt_priority` > 0 setzen.

### Optional: open Firewall port


## Lizenzen der Abhängigkeiten

| Komponente | Lizenz |
|---|---|
| SOEM v1.4.0 | GPL v2 mit Linking-Exception (siehe `third_party/SOEM/LICENSE`) |
| nlohmann/json | MIT |
| Crow | BSD 3-Clause |
| asio | Boost Software License 1.0 |
| uPlot | MIT |
| Archivo (Schrift) | SIL Open Font License 1.1 |

Für dieses Repository ist noch keine Lizenz festgelegt.

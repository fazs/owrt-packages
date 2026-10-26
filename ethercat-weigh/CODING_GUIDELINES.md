# Coding Guidelines

Regeln für Backend (C++17), Frontend (JavaScript/Svelte) und Hilfsskripte in diesem Projekt,
mit den Standards und Richtlinien, an denen sie sich orientieren. „Orientierung“ heißt: Der Code
folgt den genannten Regeln, ist aber nicht formal geprüft oder zertifiziert.

## Referenzierte Standards und Richtlinien

| Bereich | Quelle | Verwendung im Projekt |
|---|---|---|
| Namen (C++) | [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html), Abschnitt *Naming* | Konstanten `kName`, Member `name_`, Funktionen und Variablen `snake_case`, Typen `PascalCase` |
| Konstanten statt Zahlen | [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines) ES.45 „Avoid magic constants; use symbolic constants“, Con.5 „Use constexpr for values that can be computed at compile time“ | benannte `constexpr`-Konstanten, siehe unten |
| Sicherer C/C++-Stil | [MISRA C++:2023](https://misra.org.uk/) (Orientierung); MISRA C:2012 Rule 7.2 (Suffix `U` für vorzeichenlose Ganzzahlkonstanten) | `U`-Suffix, explizite Typen und Casts, keine impliziten einschränkenden Konvertierungen, `sizeof` statt Bytezahlen |
| Kommentare (C++, Java) | [Doxygen](https://www.doxygen.nl/manual/docblocks.html) | `/** … */`, `///`, `///<` mit `@brief`, `@param`, `@return`, `@throws` |
| Kommentare (JavaScript) | [JSDoc](https://jsdoc.app/) | `/** … */` mit `@param {Typ}`, `@returns`, `@throws`, `@file` |
| Namen (JavaScript) | [Google JavaScript Style Guide](https://google.github.io/styleguide/jsguide.html), Abschnitt *Constant names* | Modulkonstanten `CONSTANT_CASE`, sonst `camelCase` |
| EtherCAT-Gerät | Beckhoff-ESI EL3356-0010 (`esi/Beckhoff EL3356-0010.xml`, Auszug aus `Beckhoff EL33xx.xml`, Schema `EtherCATInfo.xsd`, ETG.2000) | PDOs, Objekte, Subindizes, Wertebereiche, Exclude-Listen |
| EtherCAT-Protokoll | ETG.1000.6 (Application Layer: CoE-Datentypcodes, AL-Status) | `kCoe…`, Zustandsmaske in `ethercat_backend.cpp` |
| HTTP | [RFC 9110](https://www.rfc-editor.org/rfc/rfc9110) *HTTP Semantics* | Statuscodes `kHttpOk`, `kHttpBadRequest`, `kHttpNotFound`, `kHttpConflict` |
| Exit-Codes | C-Standard `EXIT_SUCCESS`/`EXIT_FAILURE`; Exit-Code 2 für ungültige Optionen (Konvention der Shell-Werkzeuge) | `kExitOk`, `kExitError`, `kExitUsage` in `main.cpp` |
| Einheiten | SI; Grain nach dem International Yard and Pound Agreement (1959): 1 gr = 64,79891 mg | `units.hpp`, `format.js` |
| Statistik | Welford (Online-Varianz), Freedman–Diaconis (Klassenbreite), Tukey (Ausreißergrenzen, „far out“ = 3·IQR) | `statistics.cpp`, `measurement.cpp`, `Histogram.svelte` |

## Keine Magic Numbers

Zahlenwerte stehen als benannte Konstante und werden per Namen referenziert.

- **C++:** `constexpr` mit passendem Typ im kleinsten sinnvollen Gültigkeitsbereich (anonymer
  Namespace der `.cpp`, `static constexpr` in der Klasse, Namespace im Header, wenn mehrere
  Dateien den Wert brauchen). Einheit im Namen (`kCheckIntervalS`, `kOpCheckTimeoutUs`) oder im
  Kommentar.
- **JavaScript:** `const NAME` am Anfang des Moduls bzw. des `<script>`-Blocks; gemeinsam genutzte
  Werte in `web/src/lib/constants.js`.
- **Ableiten statt wiederholen:** Werte, die sich aus anderen ergeben, werden berechnet, z. B.
  Anzahl von Einträgen aus der Liste, Bytes aus Bits (`bytes_for_bits`), Zykluszeit aus der
  Konfiguration, Texte von Fehlermeldungen aus den Grenzwerten.
- **Tests** verwenden die Konstanten des Produktivcodes (`SimBackend::kOverrangeFactor`,
  `CalibrationController::kMinReferenceRatio`, …). Testdaten, Wartezeiten und Toleranzen sind
  im Test benannt; `static_assert` bindet Testwerte an die Grenzen des Codes.

Ausnahmen, die als Literal stehen bleiben dürfen:

- `0` und `1` in offensichtlicher Bedeutung (Startwert, Zähler, neutrales Element, Index des
  ersten Elements), `true`/`false`.
- Mathematische Konstanten in Formeln (Quadrat, Hälfte, `2 · π`, Normalverteilung).
- Einzelne Werte aus einer Spezifikation, wenn ihre Bedeutung direkt daneben steht (z. B. Breiten
  der Lücken im PDO-Mapping laut ESI, Tabelle der Filtereinstellungen mit Namen). Werden mehrere
  Adressen, Objekte oder Subindizes verwendet, bekommen sie Namen (`el3356.hpp`, `coe.js`).
- Defaults der Konfiguration als Initialisierer benannter Felder (`config.hpp`): Der Feldname ist
  der Name des Werts.
- Layout in CSS und Markup-Attributen (Abstände, Schriftgrößen). In JavaScript berechnete
  Geometrie (z. B. SVG im Histogramm) ist benannt.

## Kommentare

- Funktionsdokumentation steht an der **Definition** (`.cpp`), nicht an der Deklaration im Header.
- Im Header dokumentiert werden Klassen, Structs, Enums, Datenmember (`///<`), Konstanten und
  Inline-Funktionen sowie rein virtuelle Schnittstellen ohne Definition (`IoBackend`).
  Implementierungen verweisen mit `@copydoc` bzw. `@copydetails`.
- Kommentare im Funktionsrumpf und Abschnittstrenner sind normale `//`-Kommentare.

## Generierte Dateien

- Erzeugte Dateien liegen unter `src/generated/`, werden eingecheckt und nicht von Hand
  bearbeitet. Kopf bzw. Feld `generator` nennt das erzeugende Skript.
- `src/generated/el3356_0010.json` stammt aus `scripts/esi_gen.py` (ESI-Auszug
  `esi/Beckhoff EL3356-0010.xml` + Zusatzdatei).
  `scripts/build_host.sh` prüft mit `--check`, ob sie zur ESI passt.
- Angaben, die die ESI nicht enthält, stehen in `scripts/esi_supplement.el3356_0010.json`;
  jede mit `source` (Dokument, Kapitel, Norm oder Projektentscheidung). Die Zusatzdatei darf
  ESI-Felder nicht überschreiben.
- CMake bettet das JSON als Raw-String ein (`src/device_description_data.cpp.in`).
- `src/generated/openapi.json` stammt aus `ethercat-weigh --write-openapi`
  (`scripts/build_host.sh` nach dem Build). REST-Routen werden ausschließlich über
  `ApiRegistry::add()` mit `ApiDoc` registriert, nicht mit `CROW_ROUTE`; Antwortschemas
  möglichst mit `schema_from()` aus den Datenstrukturen ableiten.

## Weitere Regeln

- Vorzeichenlose Literale mit Suffix `U`, Gleitkomma-Literale für `float` mit `F`.
- Puffergrößen über `sizeof` bzw. aus einer Formatvorlage (`sizeof("0xIIII:SS")`).
- Grenzen von Ganzzahltypen über `std::numeric_limits` statt `0xFF`/`0xFFFF`.
- Keine Verdeckung von Namen: Ein Bezeichner in einem inneren Gültigkeitsbereich darf keinen
  gleichnamigen aus einem äußeren verdecken (MISRA C:2012 Rule 5.3). Der Compiler prüft das mit
  `-Wshadow` (in `CMakeLists.txt` für alle eigenen Ziele aktiv).
- Sprache von Meldungen und Dokumentation: Deutsch; Code und Kommentare: Englisch; Bezeichner und Namen aus der
  Spezifikation (ESI) im Original.

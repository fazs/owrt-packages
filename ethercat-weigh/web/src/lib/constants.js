/**
 * @file Gemeinsame Konstanten des Frontends.
 *
 * Werte, die das Backend vorgibt (Defaults, Grenzen), stehen hier mit Verweis auf die Quelle;
 * sie werden nur verwendet, solange die Konfiguration noch nicht geladen ist.
 */

/** Millisekunden je Sekunde. */
export const MS_PER_S = 1000;
/** Sekunden je Minute. */
export const S_PER_MIN = 60;
/** Sekunden je Stunde. */
export const S_PER_H = 3600;

/** Abfrage von /api/info [ms]. */
export const INFO_REFRESH_MS = 5000;
/** Anzeigedauer eines Hinweises [ms]. */
export const NOTICE_MS = 4000;
/** Zeit für den zweiten Klick bei Aktionen mit Bestätigung [ms]. */
export const CONFIRM_MS = 3000;
/** Abfrage der PDO-Werte im Debug-Dialog [ms]. */
export const DEBUG_POLL_MS = 300;

/** WebSocket: erste Wartezeit vor dem Wiederverbinden [ms]. */
export const WS_RETRY_MIN_MS = 1000;
/** WebSocket: längste Wartezeit vor dem Wiederverbinden [ms]. */
export const WS_RETRY_MAX_MS = 8000;
/** WebSocket: Faktor, um den die Wartezeit nach jedem Fehlschlag wächst. */
export const WS_RETRY_FACTOR = 2;

/** Default statistics.history_size (src/config.hpp). */
export const DEFAULT_HISTORY_SIZE = 500;
/** Default statistics.rolling_window (src/config.hpp). */
export const DEFAULT_ROLLING_WINDOW = 50;
/** Default calibration.stable_time_s (src/config.hpp). */
export const DEFAULT_STABLE_TIME_S = 10;
/** Messungen im Snapshot des Backends (kSnapshotMeasurements, src/acquisition.cpp). */
export const SNAPSHOT_MEASUREMENTS = 50;
/** Mindestanzahl Stützpunkte einer Kennlinie (kMinCalPoints, src/config.cpp). */
export const MIN_CAL_POINTS = 2;
/** Kalibrierung in der Klemme: Referenzlast mindestens dieser Anteil der Nennlast (kMinReferenceRatio). */
export const MIN_REFERENCE_RATIO = 0.2;

/** Simulation: Vorbelegung der aufgelegten Masse als Anteil der Nennlast. */
export const SIM_DEFAULT_LOAD_RATIO = 0.5;
/** Prozentfaktor für Anzeigen. */
export const PERCENT = 100;

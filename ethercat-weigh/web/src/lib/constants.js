/**
 * @file Common constants of the frontend.
 *
 * Values defined by the backend (defaults, limits) are listed here with a reference to the source;
 * they are only used as long as the configuration has not been loaded yet.
 */

/** Milliseconds per second. */
export const MS_PER_S = 1000;
/** Seconds per minute. */
export const S_PER_MIN = 60;
/** Seconds per hour. */
export const S_PER_H = 3600;

/** Polling of /api/info [ms]. */
export const INFO_REFRESH_MS = 5000;
/** Display duration of a notice [ms]. */
export const NOTICE_MS = 4000;
/** Time for the second click on actions with confirmation [ms]. */
export const CONFIRM_MS = 3000;
/** Polling of the PDO values in the debug dialog [ms]. */
export const DEBUG_POLL_MS = 300;

/** WebSocket: first waiting time before reconnecting [ms]. */
export const WS_RETRY_MIN_MS = 1000;
/** WebSocket: longest waiting time before reconnecting [ms]. */
export const WS_RETRY_MAX_MS = 8000;
/** WebSocket: factor by which the waiting time grows after each failure. */
export const WS_RETRY_FACTOR = 2;

/** Default statistics.history_size (src/config.hpp). */
export const DEFAULT_HISTORY_SIZE = 500;
/** Default statistics.rolling_window (src/config.hpp). */
export const DEFAULT_ROLLING_WINDOW = 50;
/** Default calibration.stable_time_s (src/config.hpp). */
export const DEFAULT_STABLE_TIME_S = 10;
/** Measurements in the snapshot of the backend (kSnapshotMeasurements, src/acquisition.cpp). */
export const SNAPSHOT_MEASUREMENTS = 50;
/** Rows of the measurement table; also the basis of „Im Bereich“ (statistics) and the distribution. */
export const VISIBLE_MEASUREMENTS = SNAPSHOT_MEASUREMENTS;
/** Minimum number of calibration points of a characteristic (kMinCalPoints, src/config.cpp). */
export const MIN_CAL_POINTS = 2;
/** Calibration in the terminal: reference load at least this fraction of the nominal load (kMinReferenceRatio). */
export const MIN_REFERENCE_RATIO = 0.2;

/** Simulation: preset of the applied mass as a fraction of the nominal load. */
export const SIM_DEFAULT_LOAD_RATIO = 0.5;
/** Percent factor for displays. */
export const PERCENT = 100;

/**
 * Columns of the measurement table (keys as kMeasurementColumns, src/config.hpp).
 * dir: sort direction on the first click.
 * @type {ReadonlyArray<{key: string, label: string, dir: 'asc'|'desc'}>}
 */
export const MEASUREMENT_COLUMNS = Object.freeze([
  { key: 'pos', label: 'Pos.', dir: 'desc' },
  { key: 'id', label: 'Nr.', dir: 'desc' },
  { key: 'value', label: 'Wert', dir: 'asc' },
  { key: 'stddev', label: 's', dir: 'asc' },
  { key: 'settle_ms', label: 'Einschwingen', dir: 'asc' },
  { key: 't', label: 'Zeit', dir: 'desc' },
]);

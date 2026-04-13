#ifndef EXPORT_H
#define EXPORT_H

#include "stats.h"

/*
 * export.h / export.c
 *
 * Three output modes, all optional:
 *
 *   JSON snapshot  --export-json <file>   written on exit (or 'e' key)
 *   CSV snapshot   --export-csv  <file>   written on exit (or 'e' key)
 *   Anomaly log    --log-anomalies <file> appended every render cycle
 *
 * All functions are no-ops when the corresponding file path is NULL.
 *
 * JSON and CSV snapshots include P95 latency per (process, event) pair
 * computed from the internal histogram (requires >= 20 samples; -1 if
 * not enough data).
 */

/* Called once at startup to open the anomaly log (append mode). */
void export_open_anomaly_log(const char *path);

/* Called every dashboard render — appends new anomalies to log. */
void export_log_anomalies(double elapsed);

/* Called on exit (or 'e' key) — writes full JSON snapshot. */
void export_json(const char *path, double elapsed);

/* Called on exit (or 'e' key) — writes full CSV snapshot. */
void export_csv(const char *path, double elapsed);

/* Close the anomaly log and write a session-end marker. */
void export_close(void);

#endif /* EXPORT_H */

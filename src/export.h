#ifndef EXPORT_H
#define EXPORT_H

#include "stats.h"

void export_open_anomaly_log(const char *path);

void export_log_anomalies(double elapsed);

void export_json(const char *path, double elapsed);

void export_csv(const char *path, double elapsed);

void export_close(void);

#endif 

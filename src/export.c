#include <stdio.h>
#include <string.h>
#include <time.h>

#include "export.h"
#include "stats.h"

static FILE *anomaly_log_fp = NULL;


void export_open_anomaly_log(const char *path)
{
    if (!path) return;

    anomaly_log_fp = fopen(path, "a");
    if (!anomaly_log_fp) {
        perror("export: cannot open anomaly log");
        return;
    }

    time_t now = time(NULL);
    char   ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(anomaly_log_fp,
            "\n# eBPF Monitor session started %s\n"
            "# timestamp_s,process,pid,event,deviation_pct,"
            "baseline_us,current_avg_us\n",
            ts);
    fflush(anomaly_log_fp);
}

void export_log_anomalies(double elapsed)
{
    if (!anomaly_log_fp) return;

    for (int i = 0; i < stat_count; i++) {
        const struct syscall_stat *s = &stats[i];
        if (!s->is_anomaly || !s->baseline_ready)
            continue;

        long valid      = s->count - s->drop_count;
        long current_us = (valid > 0 && s->total_latency > 0)
                          ? (s->total_latency / valid) / 1000
                          : 0;
        long baseline_us = (long)(s->baseline_latency / 1000.0);

        fprintf(anomaly_log_fp,
                "%.0f,%s,%d,%s,%.1f,%ld,%ld\n",
                elapsed, s->process, s->pid, s->event,
                s->deviation * 100.0, baseline_us, current_us);
    }
    fflush(anomaly_log_fp);
}

void export_close(void)
{
    if (anomaly_log_fp) {
    
        time_t now = time(NULL);
        char   ts[64];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(anomaly_log_fp, "# session ended %s\n", ts);
        fflush(anomaly_log_fp);
        fclose(anomaly_log_fp);
        anomaly_log_fp = NULL;
    }
}


void export_json(const char *path, double elapsed)
{
    if (!path) return;

    FILE *f = fopen(path, "w");
    if (!f) { perror("export: cannot write JSON"); return; }

    time_t now = time(NULL);
    char   ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", localtime(&now));

    fprintf(f, "{\n");
    fprintf(f, "  \"generated\": \"%s\",\n", ts);
    fprintf(f, "  \"runtime_s\": %.0f,\n", elapsed);
    fprintf(f, "  \"tracked_entries\": %d,\n", stat_count);
    fprintf(f, "  \"total_events_dropped\": %ld,\n", total_events_dropped);
    fprintf(f, "  \"stats\": [\n");

    for (int i = 0; i < stat_count; i++) {
        const struct syscall_stat *s = &stats[i];
        long valid       = s->count - s->drop_count;
        long avg_us      = (valid > 0 && s->total_latency > 0)
                           ? (s->total_latency / valid) / 1000 : 0;
        long baseline_us = (long)(s->baseline_latency / 1000.0);
        long p95_us      = stats_p95_us(s);

        fprintf(f,
            "    {\n"
            "      \"process\": \"%s\",\n"
            "      \"pid\": %d,\n"
            "      \"event\": \"%s\",\n"
            "      \"count\": %ld,\n"
            "      \"rate_per_s\": %.2f,\n"
            "      \"avg_latency_us\": %ld,\n"
            "      \"p95_latency_us\": %ld,\n"
            "      \"max_latency_us\": %ld,\n"
            "      \"ctx_switches\": %ld,\n"
            "      \"exec_count\": %ld,\n"
            "      \"baseline_latency_us\": %ld,\n"
            "      \"deviation_pct\": %.1f,\n"
            "      \"is_anomaly\": %s\n"
            "    }%s\n",
            s->process, s->pid, s->event,
            s->count, s->rate,
            avg_us, p95_us,
            s->max_latency / 1000,
            s->ctx_switches, s->exec_count,
            baseline_us,
            s->deviation * 100.0,
            s->is_anomaly ? "true" : "false",
            (i < stat_count - 1) ? "," : "");
    }

    fprintf(f, "  ]\n}\n");
    fclose(f);
    fprintf(stderr, "[export] JSON written to %s\n", path);
}



void export_csv(const char *path, double elapsed)
{
    if (!path) return;

    FILE *f = fopen(path, "w");
    if (!f) { perror("export: cannot write CSV"); return; }

    fprintf(f,
            "process,pid,event,count,rate_per_s,avg_latency_us,"
            "p95_latency_us,max_latency_us,ctx_switches,exec_count,"
            "baseline_latency_us,deviation_pct,is_anomaly,runtime_s\n");

    for (int i = 0; i < stat_count; i++) {
        const struct syscall_stat *s = &stats[i];
        long valid       = s->count - s->drop_count;
        long avg_us      = (valid > 0 && s->total_latency > 0)
                           ? (s->total_latency / valid) / 1000 : 0;
        long baseline_us = (long)(s->baseline_latency / 1000.0);
        long p95_us      = stats_p95_us(s);

        fprintf(f,
                "%s,%d,%s,%ld,%.2f,%ld,%ld,%ld,%ld,%ld,%ld,%.1f,%d,%.0f\n",
                s->process, s->pid, s->event,
                s->count, s->rate,
                avg_us, p95_us,
                s->max_latency / 1000,
                s->ctx_switches, s->exec_count,
                baseline_us,
                s->deviation * 100.0,
                s->is_anomaly,
                elapsed);
    }

    fclose(f);
    fprintf(stderr, "[export] CSV written to %s\n", path);
}

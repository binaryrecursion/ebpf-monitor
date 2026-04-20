#ifndef STATS_H
#define STATS_H

#include "bootstrap.h"


#define MAX_STATS         512  
#define ANOMALY_THRESHOLD 0.5   
#define BASELINE_ALPHA    0.2  


#define SCHED_NOISE_NS    (200ULL * 1000 * 1000)   /* 200 ms           */


#define LAT_BUCKETS 8
static const long lat_bucket_us[LAT_BUCKETS] = {
    10, 50, 100, 500, 1000, 5000, 10000, (long)9e18
};


struct syscall_stat {
    char process[16];
    char event[32];
    int  pid;

    long count;           
    long total_latency;   
    long max_latency;     
    long ctx_switches;    
    long exec_count;      
    long drop_count;      
    double rate;           

    long lat_hist[LAT_BUCKETS];

    double baseline_latency; 
    double deviation;       
    int    is_anomaly;       
    int    baseline_ready;   
};


extern struct syscall_stat stats[MAX_STATS];
extern int  stat_count;
extern long total_events_dropped;



void  stats_update(const struct event *e);
void  stats_compute_rates(double elapsed);
void  stats_reset(void);
long  stats_p95_us(const struct syscall_stat *s);

#endif

#ifndef __BOOTSTRAP_H
#define __BOOTSTRAP_H

#include <stdbool.h>

#define TASK_COMM_LEN    16
#define MAX_FILENAME_LEN 127

struct event {
    int  pid;
    int  ppid;
    unsigned exit_code;
    unsigned long long duration_ns;
    char comm[TASK_COMM_LEN];
    char filename[MAX_FILENAME_LEN];
    bool exit_event;
    int  type;
};

enum event_type {
    EVENT_SYSCALL = 0,
    EVENT_SCHED,
    EVENT_EXEC,
    EVENT_EXIT
};

#endif 

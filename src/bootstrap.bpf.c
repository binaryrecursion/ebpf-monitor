// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "bootstrap.h"

char LICENSE[] SEC("license") = "Dual BSD/GPL";

/* ------------------------------------------------------------------ */
/* Maps                                                                */
/* ------------------------------------------------------------------ */

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 8192);
	__type(key, u32);
	__type(value, u64);
} exec_start SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 8192);
	__type(key, u64);
	__type(value, u64);
} syscall_start SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 8192);
	__type(key, u32);
	__type(value, u64);
} sched_start SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 256 * 1024);
} rb SEC(".maps");

const volatile unsigned long long min_duration_ns = 0;

/* ------------------------------------------------------------------ */
/* Process filter                                                      */
/* Uncomment the body to restrict tracing to specific commands.       */
/* ------------------------------------------------------------------ */

static __always_inline int allow_process(void)
{
	// char comm[16];
	// bpf_get_current_comm(&comm, sizeof(comm));
	// if (bpf_strncmp(comm, 16, "myapp") != 0)
	//     return 0;
	return 1;
}

/* ------------------------------------------------------------------ */
/* Helper: get parent PID of current task via CO-RE                   */
/* ------------------------------------------------------------------ */

static __always_inline u32 get_ppid(void)
{
	struct task_struct *task = (struct task_struct *)bpf_get_current_task();
	return BPF_CORE_READ(task, real_parent, tgid);
}

/* ------------------------------------------------------------------ */
/* process exec                                                        */
/* ------------------------------------------------------------------ */

SEC("tp/sched/sched_process_exec")
int handle_exec(struct trace_event_raw_sched_process_exec *ctx)
{
	struct event *e;
	u32 pid;
	u64 ts;

	if (!allow_process())
		return 0;

	pid = bpf_get_current_pid_tgid() >> 32;
	ts  = bpf_ktime_get_ns();

	bpf_map_update_elem(&exec_start, &pid, &ts, BPF_ANY);

	e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e)
		return 0;

	e->exit_event  = false;
	e->pid         = pid;
	e->ppid        = get_ppid();
	e->duration_ns = 0;
	e->exit_code   = 0;
	e->type        = EVENT_EXEC;

	bpf_get_current_comm(&e->comm, sizeof(e->comm));

	unsigned fname_off = ctx->__data_loc_filename & 0xFFFF;
	bpf_probe_read_str(e->filename, sizeof(e->filename),
	                   (void *)ctx + fname_off);

	bpf_ringbuf_submit(e, 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* process exit                                                        */
/* ------------------------------------------------------------------ */

SEC("tp/sched/sched_process_exit")
int handle_exit(struct trace_event_raw_sched_process_template *ctx)
{
	struct event *e;
	u32 pid;
	u64 *start_ts, duration_ns = 0;

	if (!allow_process())
		return 0;

	/* Only handle the group leader exit to avoid duplicate events
	   from per-thread exits in multi-threaded processes */
	u64 pid_tgid = bpf_get_current_pid_tgid();
	u32 tgid     = pid_tgid >> 32;
	u32 tid      = (u32)pid_tgid;
	if (tgid != tid)
		return 0;

	pid      = tgid;
	start_ts = bpf_map_lookup_elem(&exec_start, &pid);
	if (start_ts)
		duration_ns = bpf_ktime_get_ns() - *start_ts;

	bpf_map_delete_elem(&exec_start, &pid);

	/* Apply minimum duration filter */
	if (min_duration_ns && duration_ns < min_duration_ns)
		return 0;

	e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e)
		return 0;

	e->exit_event  = true;
	e->pid         = pid;
	e->ppid        = get_ppid();
	e->duration_ns = duration_ns;
	e->type        = EVENT_EXIT;

	/* Read exit code from task_struct via CO-RE.
	   Kernel stores it as (signal << 8 | exit_code); we extract the code. */
	struct task_struct *task = (struct task_struct *)bpf_get_current_task();
	unsigned int raw_exit    = BPF_CORE_READ(task, exit_code);
	e->exit_code             = (raw_exit >> 8) & 0xff;

	bpf_get_current_comm(&e->comm, sizeof(e->comm));
	__builtin_memcpy(e->filename, "exit", 5);

	bpf_ringbuf_submit(e, 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* openat latency                                                      */
/* ------------------------------------------------------------------ */

SEC("tracepoint/syscalls/sys_enter_openat")
int handle_sys_enter_openat(struct trace_event_raw_sys_enter *ctx)
{
	if (!allow_process())
		return 0;
	u64 id = bpf_get_current_pid_tgid();
	u64 ts = bpf_ktime_get_ns();
	bpf_map_update_elem(&syscall_start, &id, &ts, BPF_ANY);
	return 0;
}

SEC("tracepoint/syscalls/sys_exit_openat")
int handle_sys_exit_openat(struct trace_event_raw_sys_exit *ctx)
{
	if (!allow_process())
		return 0;

	u64 id       = bpf_get_current_pid_tgid();
	u64 *start_ts = bpf_map_lookup_elem(&syscall_start, &id);
	if (!start_ts)
		return 0;

	u64 delta = bpf_ktime_get_ns() - *start_ts;
	bpf_map_delete_elem(&syscall_start, &id);

	if (min_duration_ns && delta < min_duration_ns)
		return 0;

	struct event *e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e)
		return 0;

	e->type        = EVENT_SYSCALL;
	e->exit_event  = false;
	e->pid         = (u32)(id >> 32);
	e->ppid        = 0;
	e->exit_code   = 0;
	e->duration_ns = delta;
	bpf_get_current_comm(&e->comm, sizeof(e->comm));
	__builtin_memcpy(e->filename, "openat", 7);

	bpf_ringbuf_submit(e, 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* read latency                                                        */
/* ------------------------------------------------------------------ */

SEC("tracepoint/syscalls/sys_enter_read")
int handle_sys_enter_read(struct trace_event_raw_sys_enter *ctx)
{
	if (!allow_process())
		return 0;
	u64 id = bpf_get_current_pid_tgid();
	u64 ts = bpf_ktime_get_ns();
	bpf_map_update_elem(&syscall_start, &id, &ts, BPF_ANY);
	return 0;
}

SEC("tracepoint/syscalls/sys_exit_read")
int handle_sys_exit_read(struct trace_event_raw_sys_exit *ctx)
{
	if (!allow_process())
		return 0;

	u64 id        = bpf_get_current_pid_tgid();
	u64 *start_ts = bpf_map_lookup_elem(&syscall_start, &id);
	if (!start_ts)
		return 0;

	u64 delta = bpf_ktime_get_ns() - *start_ts;
	bpf_map_delete_elem(&syscall_start, &id);

	if (min_duration_ns && delta < min_duration_ns)
		return 0;

	struct event *e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e)
		return 0;

	e->type        = EVENT_SYSCALL;
	e->exit_event  = false;
	e->pid         = (u32)(id >> 32);
	e->ppid        = 0;
	e->exit_code   = 0;
	e->duration_ns = delta;
	bpf_get_current_comm(&e->comm, sizeof(e->comm));
	__builtin_memcpy(e->filename, "read", 5);

	bpf_ringbuf_submit(e, 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* write latency                                                       */
/* ------------------------------------------------------------------ */

SEC("tracepoint/syscalls/sys_enter_write")
int handle_sys_enter_write(struct trace_event_raw_sys_enter *ctx)
{
	if (!allow_process())
		return 0;
	u64 id = bpf_get_current_pid_tgid();
	u64 ts = bpf_ktime_get_ns();
	bpf_map_update_elem(&syscall_start, &id, &ts, BPF_ANY);
	return 0;
}

SEC("tracepoint/syscalls/sys_exit_write")
int handle_sys_exit_write(struct trace_event_raw_sys_exit *ctx)
{
	if (!allow_process())
		return 0;

	u64 id        = bpf_get_current_pid_tgid();
	u64 *start_ts = bpf_map_lookup_elem(&syscall_start, &id);
	if (!start_ts)
		return 0;

	u64 delta = bpf_ktime_get_ns() - *start_ts;
	bpf_map_delete_elem(&syscall_start, &id);

	if (min_duration_ns && delta < min_duration_ns)
		return 0;

	struct event *e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e)
		return 0;

	e->type        = EVENT_SYSCALL;
	e->exit_event  = false;
	e->pid         = (u32)(id >> 32);
	e->ppid        = 0;
	e->exit_code   = 0;
	e->duration_ns = delta;
	bpf_get_current_comm(&e->comm, sizeof(e->comm));
	__builtin_memcpy(e->filename, "write", 6);

	bpf_ringbuf_submit(e, 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* close latency                                                       */
/* ------------------------------------------------------------------ */

SEC("tracepoint/syscalls/sys_enter_close")
int handle_sys_enter_close(struct trace_event_raw_sys_enter *ctx)
{
	if (!allow_process())
		return 0;
	u64 id = bpf_get_current_pid_tgid();
	u64 ts = bpf_ktime_get_ns();
	bpf_map_update_elem(&syscall_start, &id, &ts, BPF_ANY);
	return 0;
}

SEC("tracepoint/syscalls/sys_exit_close")
int handle_sys_exit_close(struct trace_event_raw_sys_exit *ctx)
{
	if (!allow_process())
		return 0;

	u64 id        = bpf_get_current_pid_tgid();
	u64 *start_ts = bpf_map_lookup_elem(&syscall_start, &id);
	if (!start_ts)
		return 0;

	u64 delta = bpf_ktime_get_ns() - *start_ts;
	bpf_map_delete_elem(&syscall_start, &id);

	if (min_duration_ns && delta < min_duration_ns)
		return 0;

	struct event *e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e)
		return 0;

	e->type        = EVENT_SYSCALL;
	e->exit_event  = false;
	e->pid         = (u32)(id >> 32);
	e->ppid        = 0;
	e->exit_code   = 0;
	e->duration_ns = delta;
	bpf_get_current_comm(&e->comm, sizeof(e->comm));
	__builtin_memcpy(e->filename, "close", 6);

	bpf_ringbuf_submit(e, 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* CPU scheduling delay                                               */
/*                                                                    */
/* On each sched_switch we record when next_pid was scheduled in.    */
/* On the FOLLOWING switch where this pid is prev_pid (scheduled out),*/
/* we use sched_start to compute how long it ran — but here we track  */
/* the INVERSE: how long was it WAITING (off-CPU time).              */
/*                                                                    */
/* When next_pid comes back onto the CPU, delta = now - last_off      */
/* gives the scheduling delay (time the process was waiting).        */
/* ------------------------------------------------------------------ */

SEC("tracepoint/sched/sched_switch")
int handle_sched_switch(struct trace_event_raw_sched_switch *ctx)
{
	u32 next_pid = ctx->next_pid;
	u64 ts       = bpf_ktime_get_ns();

	/* Skip idle thread (pid 0) */
	if (next_pid == 0)
		return 0;

	char comm[16];
	bpf_probe_read_kernel_str(comm, sizeof(comm), ctx->next_comm);

	u64 *start_ts = bpf_map_lookup_elem(&sched_start, &next_pid);

	if (start_ts) {
		u64 delta = ts - *start_ts;

		if (!min_duration_ns || delta >= min_duration_ns) {
			struct event *e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
			if (e) {
				e->type        = EVENT_SCHED;
				e->exit_event  = false;
				e->pid         = next_pid;
				e->ppid        = 0;
				e->exit_code   = 0;
				e->duration_ns = delta;
				__builtin_memcpy(e->comm, comm, sizeof(comm));
				__builtin_memcpy(e->filename, "sched", 6);
				bpf_ringbuf_submit(e, 0);
			}
		}
	}

	/* Record when this process was last scheduled OFF the CPU.
	   We use prev_pid for the "scheduled out" timestamp. */
	u32 prev_pid = ctx->prev_pid;
	if (prev_pid != 0)
		bpf_map_update_elem(&sched_start, &prev_pid, &ts, BPF_ANY);

	return 0;
}
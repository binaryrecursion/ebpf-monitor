// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "bootstrap.h"

char LICENSE[] SEC("license") = "Dual BSD/GPL";

/* maps */

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

/* process filter */

static __always_inline int allow_process()
{
	// char comm[16];
	// bpf_get_current_comm(&comm, sizeof(comm));

	// if (bpf_strncmp(comm, 16, "ls") != 0 &&
	//     bpf_strncmp(comm, 16, "cat") != 0)
	// 	return 0;

	return 1;
}

/* process exec */

SEC("tp/sched/sched_process_exec")
int handle_exec(struct trace_event_raw_sched_process_exec *ctx)
{
	struct event *e;
	u32 pid;
	u64 ts;

	if (!allow_process())
		return 0;

	pid = bpf_get_current_pid_tgid() >> 32;
	ts = bpf_ktime_get_ns();

	bpf_map_update_elem(&exec_start, &pid, &ts, BPF_ANY);

	e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e)
		return 0;

	e->exit_event = false;
	e->pid = pid;
	e->ppid = 0;
	e->duration_ns = 0;
	e->exit_code = 0;

	bpf_get_current_comm(&e->comm, sizeof(e->comm));

	unsigned fname_off = ctx->__data_loc_filename & 0xFFFF;
	bpf_probe_read_str(e->filename, sizeof(e->filename),
	                   (void *)ctx + fname_off);

	e->type = EVENT_EXEC;

	bpf_ringbuf_submit(e, 0);

	return 0;
}

/* process exit */

SEC("tp/sched/sched_process_exit")
int handle_exit(struct trace_event_raw_sched_process_template *ctx)
{
	struct event *e;
	u32 pid;
	u64 *start_ts, duration_ns = 0;

	if (!allow_process())
		return 0;

	pid = bpf_get_current_pid_tgid() >> 32;

	start_ts = bpf_map_lookup_elem(&exec_start, &pid);
	if (start_ts)
		duration_ns = bpf_ktime_get_ns() - *start_ts;

	bpf_map_delete_elem(&exec_start, &pid);

	e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e)
		return 0;

	e->exit_event = true;
	e->pid = pid;
	e->ppid = 0;
	e->duration_ns = duration_ns;
	e->exit_code = 0;

	bpf_get_current_comm(&e->comm, sizeof(e->comm));

	e->type = EVENT_EXIT;
__builtin_memcpy(e->filename, "exit", 5);

	bpf_ringbuf_submit(e, 0);

	return 0;
}

/* openat latency */

SEC("tracepoint/syscalls/sys_enter_openat")
int handle_sys_enter_openat(struct trace_event_raw_sys_enter *ctx)
{
	u64 id = bpf_get_current_pid_tgid();
	u64 ts = bpf_ktime_get_ns();

	if (!allow_process())
		return 0;

	bpf_map_update_elem(&syscall_start, &id, &ts, BPF_ANY);
	return 0;
}

SEC("tracepoint/syscalls/sys_exit_openat")
int handle_sys_exit_openat(struct trace_event_raw_sys_exit *ctx)
{
	struct event *e;
	u64 id = bpf_get_current_pid_tgid();
	u64 *start_ts;
	u64 delta;
	u32 pid = id >> 32;

	if (!allow_process())
		return 0;

	start_ts = bpf_map_lookup_elem(&syscall_start, &id);
	if (!start_ts)
		return 0;

	delta = bpf_ktime_get_ns() - *start_ts;
	bpf_map_delete_elem(&syscall_start, &id);

	e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e)
		return 0;

	e->exit_event = false;
	e->pid = pid;
	e->duration_ns = delta;

	bpf_get_current_comm(&e->comm, sizeof(e->comm));
	e->type = EVENT_SYSCALL;
__builtin_memcpy(e->filename, "openat", 7);

	bpf_ringbuf_submit(e, 0);

	return 0;
}

/* read latency */

SEC("tracepoint/syscalls/sys_enter_read")
int handle_sys_enter_read(struct trace_event_raw_sys_enter *ctx)
{
	u64 id = bpf_get_current_pid_tgid();
	u64 ts = bpf_ktime_get_ns();

	if (!allow_process())
		return 0;

	bpf_map_update_elem(&syscall_start, &id, &ts, BPF_ANY);
	return 0;
}

SEC("tracepoint/syscalls/sys_exit_read")
int handle_sys_exit_read(struct trace_event_raw_sys_exit *ctx)
{
	struct event *e;
	u64 id = bpf_get_current_pid_tgid();
	u64 *start_ts;
	u64 delta;
	u32 pid = id >> 32;

	if (!allow_process())
		return 0;

	start_ts = bpf_map_lookup_elem(&syscall_start, &id);
	if (!start_ts)
		return 0;

	delta = bpf_ktime_get_ns() - *start_ts;
	bpf_map_delete_elem(&syscall_start, &id);

	e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e)
		return 0;

	e->exit_event = false;
	e->pid = pid;
	e->duration_ns = delta;

	bpf_get_current_comm(&e->comm, sizeof(e->comm));
	e->type = EVENT_SYSCALL;
__builtin_memcpy(e->filename, "read", 5);
	bpf_ringbuf_submit(e, 0);

	return 0;
}

/* CPU scheduling latency */
SEC("tracepoint/sched/sched_switch")
int handle_sched_switch(struct trace_event_raw_sched_switch *ctx)
{
	struct event *e;
	u32 pid = ctx->next_pid;
	u64 ts = bpf_ktime_get_ns();
	u64 *start_ts;

	if (pid == 0)
		return 0;

	char comm[16];
	bpf_probe_read_kernel_str(comm, sizeof(comm), ctx->next_comm);

	start_ts = bpf_map_lookup_elem(&sched_start, &pid);

	if (start_ts) {

		u64 delta = ts - *start_ts;

		e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
		if (!e)
			return 0;

		e->exit_event = false;
		e->pid = pid;
		e->duration_ns = delta;

		__builtin_memcpy(e->comm, comm, sizeof(comm));
		e->type = EVENT_SCHED;
__builtin_memcpy(e->filename, "sched", 6);

		bpf_ringbuf_submit(e, 0);
	}

	bpf_map_update_elem(&sched_start, &pid, &ts, BPF_ANY);

	return 0;
}


SEC("tracepoint/syscalls/sys_enter_write")
int handle_sys_enter_write(struct trace_event_raw_sys_enter *ctx)
{
	u64 id = bpf_get_current_pid_tgid();
	u64 ts = bpf_ktime_get_ns();

	if (!allow_process())
		return 0;

	bpf_map_update_elem(&syscall_start, &id, &ts, BPF_ANY);
	return 0;
}

SEC("tracepoint/syscalls/sys_enter_close")
int handle_sys_enter_close(struct trace_event_raw_sys_enter *ctx)
{
	u64 id = bpf_get_current_pid_tgid();
	u64 ts = bpf_ktime_get_ns();

	if (!allow_process())
		return 0;

	bpf_map_update_elem(&syscall_start, &id, &ts, BPF_ANY);
	return 0;
}

SEC("tracepoint/syscalls/sys_exit_write")
int handle_sys_exit_write(struct trace_event_raw_sys_exit *ctx)
{
	struct event *e;
	u64 id = bpf_get_current_pid_tgid();
	u64 *start_ts;
	u64 delta;
	u32 pid = id >> 32;

	if (!allow_process())
		return 0;

	start_ts = bpf_map_lookup_elem(&syscall_start, &id);
	if (!start_ts)
		return 0;

	delta = bpf_ktime_get_ns() - *start_ts;
	bpf_map_delete_elem(&syscall_start, &id);

	e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e)
		return 0;

	e->type = EVENT_SYSCALL;
	e->exit_event = false;
	e->pid = pid;
	e->duration_ns = delta;

	bpf_get_current_comm(&e->comm, sizeof(e->comm));
	__builtin_memcpy(e->filename, "write", 6);

	bpf_ringbuf_submit(e, 0);

	return 0;
}

SEC("tracepoint/syscalls/sys_exit_close")
int handle_sys_exit_close(struct trace_event_raw_sys_exit *ctx)
{
	struct event *e;
	u64 id = bpf_get_current_pid_tgid();
	u64 *start_ts;
	u64 delta;
	u32 pid = id >> 32;

	if (!allow_process())
		return 0;

	start_ts = bpf_map_lookup_elem(&syscall_start, &id);
	if (!start_ts)
		return 0;

	delta = bpf_ktime_get_ns() - *start_ts;
	bpf_map_delete_elem(&syscall_start, &id);

	e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e)
		return 0;

	e->type = EVENT_SYSCALL;
	e->exit_event = false;
	e->pid = pid;
	e->duration_ns = delta;

	bpf_get_current_comm(&e->comm, sizeof(e->comm));
	__builtin_memcpy(e->filename, "close", 6);

	bpf_ringbuf_submit(e, 0);

	return 0;
}





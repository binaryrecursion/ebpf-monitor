#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "bootstrap.h"

char LICENSE[] SEC("license") = "Dual BSD/GPL";

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

const volatile int target_pid = 0;

const volatile char target_comm[TASK_COMM_LEN] = {};


static __always_inline int allow_process(void)
{
	u32 pid = bpf_get_current_pid_tgid() >> 32;

	if (target_pid != 0 && (int)pid != target_pid)
		return 0;

	if (target_comm[0] != '\0') {
		char comm[TASK_COMM_LEN];
		bpf_get_current_comm(&comm, sizeof(comm));
	
		for (int i = 0; i < TASK_COMM_LEN; i++) {
			if (comm[i] != target_comm[i])
				return 0;
			if (comm[i] == '\0')
				break;
		}
	}

	return 1;
}


static __always_inline u32 get_ppid(void)
{
	struct task_struct *task = (struct task_struct *)bpf_get_current_task();
	return BPF_CORE_READ(task, real_parent, tgid);
}



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


SEC("tp/sched/sched_process_exit")
int handle_exit(struct trace_event_raw_sched_process_template *ctx)
{
	struct event *e;
	u32 pid;
	u64 *start_ts, duration_ns = 0;

	if (!allow_process())
		return 0;

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

	struct task_struct *task = (struct task_struct *)bpf_get_current_task();
	unsigned int raw_exit    = BPF_CORE_READ(task, exit_code);
	e->exit_code             = (raw_exit >> 8) & 0xff;

	bpf_get_current_comm(&e->comm, sizeof(e->comm));
	__builtin_memcpy(e->filename, "exit", 5);

	bpf_ringbuf_submit(e, 0);
	return 0;
}


SEC("tracepoint/syscalls/sys_enter_openat")
int handle_sys_enter_openat(struct trace_event_raw_sys_enter *ctx)
{
	if (!allow_process()) return 0;
	u64 id = bpf_get_current_pid_tgid();
	u64 ts = bpf_ktime_get_ns();
	bpf_map_update_elem(&syscall_start, &id, &ts, BPF_ANY);
	return 0;
}

SEC("tracepoint/syscalls/sys_exit_openat")
int handle_sys_exit_openat(struct trace_event_raw_sys_exit *ctx)
{
	if (!allow_process()) return 0;
	u64 id        = bpf_get_current_pid_tgid();
	u64 *start_ts = bpf_map_lookup_elem(&syscall_start, &id);
	if (!start_ts) return 0;
	u64 delta = bpf_ktime_get_ns() - *start_ts;
	bpf_map_delete_elem(&syscall_start, &id);
	if (min_duration_ns && delta < min_duration_ns) return 0;
	struct event *e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e) return 0;
	e->type = EVENT_SYSCALL; e->exit_event = false;
	e->pid = (u32)(id >> 32); e->ppid = 0; e->exit_code = 0;
	e->duration_ns = delta;
	bpf_get_current_comm(&e->comm, sizeof(e->comm));
	__builtin_memcpy(e->filename, "openat", 7);
	bpf_ringbuf_submit(e, 0);
	return 0;
}



SEC("tracepoint/syscalls/sys_enter_read")
int handle_sys_enter_read(struct trace_event_raw_sys_enter *ctx)
{
	if (!allow_process()) return 0;
	u64 id = bpf_get_current_pid_tgid();
	u64 ts = bpf_ktime_get_ns();
	bpf_map_update_elem(&syscall_start, &id, &ts, BPF_ANY);
	return 0;
}

SEC("tracepoint/syscalls/sys_exit_read")
int handle_sys_exit_read(struct trace_event_raw_sys_exit *ctx)
{
	if (!allow_process()) return 0;
	u64 id        = bpf_get_current_pid_tgid();
	u64 *start_ts = bpf_map_lookup_elem(&syscall_start, &id);
	if (!start_ts) return 0;
	u64 delta = bpf_ktime_get_ns() - *start_ts;
	bpf_map_delete_elem(&syscall_start, &id);
	if (min_duration_ns && delta < min_duration_ns) return 0;
	struct event *e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e) return 0;
	e->type = EVENT_SYSCALL; e->exit_event = false;
	e->pid = (u32)(id >> 32); e->ppid = 0; e->exit_code = 0;
	e->duration_ns = delta;
	bpf_get_current_comm(&e->comm, sizeof(e->comm));
	__builtin_memcpy(e->filename, "read", 5);
	bpf_ringbuf_submit(e, 0);
	return 0;
}



SEC("tracepoint/syscalls/sys_enter_write")
int handle_sys_enter_write(struct trace_event_raw_sys_enter *ctx)
{
	if (!allow_process()) return 0;
	u64 id = bpf_get_current_pid_tgid();
	u64 ts = bpf_ktime_get_ns();
	bpf_map_update_elem(&syscall_start, &id, &ts, BPF_ANY);
	return 0;
}

SEC("tracepoint/syscalls/sys_exit_write")
int handle_sys_exit_write(struct trace_event_raw_sys_exit *ctx)
{
	if (!allow_process()) return 0;
	u64 id        = bpf_get_current_pid_tgid();
	u64 *start_ts = bpf_map_lookup_elem(&syscall_start, &id);
	if (!start_ts) return 0;
	u64 delta = bpf_ktime_get_ns() - *start_ts;
	bpf_map_delete_elem(&syscall_start, &id);
	if (min_duration_ns && delta < min_duration_ns) return 0;
	struct event *e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e) return 0;
	e->type = EVENT_SYSCALL; e->exit_event = false;
	e->pid = (u32)(id >> 32); e->ppid = 0; e->exit_code = 0;
	e->duration_ns = delta;
	bpf_get_current_comm(&e->comm, sizeof(e->comm));
	__builtin_memcpy(e->filename, "write", 6);
	bpf_ringbuf_submit(e, 0);
	return 0;
}


SEC("tracepoint/syscalls/sys_enter_close")
int handle_sys_enter_close(struct trace_event_raw_sys_enter *ctx)
{
	if (!allow_process()) return 0;
	u64 id = bpf_get_current_pid_tgid();
	u64 ts = bpf_ktime_get_ns();
	bpf_map_update_elem(&syscall_start, &id, &ts, BPF_ANY);
	return 0;
}

SEC("tracepoint/syscalls/sys_exit_close")
int handle_sys_exit_close(struct trace_event_raw_sys_exit *ctx)
{
	if (!allow_process()) return 0;
	u64 id        = bpf_get_current_pid_tgid();
	u64 *start_ts = bpf_map_lookup_elem(&syscall_start, &id);
	if (!start_ts) return 0;
	u64 delta = bpf_ktime_get_ns() - *start_ts;
	bpf_map_delete_elem(&syscall_start, &id);
	if (min_duration_ns && delta < min_duration_ns) return 0;
	struct event *e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e) return 0;
	e->type = EVENT_SYSCALL; e->exit_event = false;
	e->pid = (u32)(id >> 32); e->ppid = 0; e->exit_code = 0;
	e->duration_ns = delta;
	bpf_get_current_comm(&e->comm, sizeof(e->comm));
	__builtin_memcpy(e->filename, "close", 6);
	bpf_ringbuf_submit(e, 0);
	return 0;
}



SEC("tracepoint/syscalls/sys_enter_mmap")
int handle_sys_enter_mmap(struct trace_event_raw_sys_enter *ctx)
{
	if (!allow_process()) return 0;
	u64 id = bpf_get_current_pid_tgid();
	u64 ts = bpf_ktime_get_ns();
	bpf_map_update_elem(&syscall_start, &id, &ts, BPF_ANY);
	return 0;
}

SEC("tracepoint/syscalls/sys_exit_mmap")
int handle_sys_exit_mmap(struct trace_event_raw_sys_exit *ctx)
{
	if (!allow_process()) return 0;
	u64 id        = bpf_get_current_pid_tgid();
	u64 *start_ts = bpf_map_lookup_elem(&syscall_start, &id);
	if (!start_ts) return 0;
	u64 delta = bpf_ktime_get_ns() - *start_ts;
	bpf_map_delete_elem(&syscall_start, &id);
	if (min_duration_ns && delta < min_duration_ns) return 0;
	struct event *e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e) return 0;
	e->type = EVENT_SYSCALL; e->exit_event = false;
	e->pid = (u32)(id >> 32); e->ppid = 0; e->exit_code = 0;
	e->duration_ns = delta;
	bpf_get_current_comm(&e->comm, sizeof(e->comm));
	__builtin_memcpy(e->filename, "mmap", 5);
	bpf_ringbuf_submit(e, 0);
	return 0;
}



SEC("tracepoint/sched/sched_switch")
int handle_sched_switch(struct trace_event_raw_sched_switch *ctx)
{
	u32 next_pid = ctx->next_pid;
	u64 ts       = bpf_ktime_get_ns();

	if (next_pid == 0)
		return 0;

	char comm[16];
	bpf_probe_read_kernel_str(comm, sizeof(comm), ctx->next_comm);

	
	if (target_comm[0] != '\0') {
		for (int i = 0; i < TASK_COMM_LEN; i++) {
			if (comm[i] != target_comm[i])
				goto skip_next;
			if (comm[i] == '\0')
				break;
		}
	}

	if (target_pid != 0 && (int)next_pid != target_pid)
		goto skip_next;

	{
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
	}

skip_next:;
	u32 prev_pid = ctx->prev_pid;
	if (prev_pid != 0)
		bpf_map_update_elem(&sched_start, &prev_pid, &ts, BPF_ANY);

	return 0;
}

/*
 * ring buffer based function tracer
 *
 * Copyright (C) 2007-2012 Steven Rostedt <srostedt@redhat.com>
 * Copyright (C) 2008 Ingo Molnar <mingo@redhat.com>
 *
 * Originally taken from the RT patch by:
 *    Arnaldo Carvalho de Melo <acme@redhat.com>
 *
 * Based on code from the latency_tracer, that is:
 *  Copyright (C) 2004-2006 Ingo Molnar
 *  Copyright (C) 2004 Nadia Yvette Chambers
 */
#include <linux/ring_buffer.h>
#include <generated/utsrelease.h>
#include <linux/stacktrace.h>
#include <linux/writeback.h>
#include <linux/kallsyms.h>
#include <linux/seq_file.h>
#include <linux/notifier.h>
#include <linux/irqflags.h>
#include <linux/debugfs.h>
#include <linux/pagemap.h>
#include <linux/hardirq.h>
#include <linux/linkage.h>
#include <linux/uaccess.h>
#include <linux/kprobes.h>
#include <linux/ftrace.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/splice.h>
#include <linux/kdebug.h>
#include <linux/string.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/nmi.h>
#include <linux/fs.h>
#include <linux/sched/rt.h>
#include <linux/coresight-stm.h>

#include "trace.h"
#include "trace_output.h"

bool ring_buffer_expanded;

static bool __read_mostly tracing_selftest_running;

bool __read_mostly tracing_selftest_disabled;

static struct tracer_opt dummy_tracer_opt[] = {
	{ }
};

static struct tracer_flags dummy_tracer_flags = {
	.val = 0,
	.opts = dummy_tracer_opt
};

static int dummy_set_flag(u32 old_flags, u32 bit, int set)
{
	return 0;
}

/*
 * To prevent the comm cache from being overwritten when no
 * tracing is active, only save the comm when a trace event
 * occurred.
 */
static DEFINE_PER_CPU(bool, trace_cmdline_save);

static int tracing_disabled = 1;

DEFINE_PER_CPU(int, ftrace_cpu_disabled);

cpumask_var_t __read_mostly	tracing_buffer_mask;


enum ftrace_dump_mode ftrace_dump_on_oops;

static int tracing_set_tracer(const char *buf);

#define MAX_TRACER_SIZE		100
static char bootup_tracer_buf[MAX_TRACER_SIZE] __initdata;
static char *default_bootup_tracer;

static bool allocate_snapshot;

static int __init set_cmdline_ftrace(char *str)
{
	strlcpy(bootup_tracer_buf, str, MAX_TRACER_SIZE);
	default_bootup_tracer = bootup_tracer_buf;
	
	ring_buffer_expanded = true;
	return 1;
}
__setup("ftrace=", set_cmdline_ftrace);

static int __init set_ftrace_dump_on_oops(char *str)
{
	if (*str++ != '=' || !*str) {
		ftrace_dump_on_oops = DUMP_ALL;
		return 1;
	}

	if (!strcmp("orig_cpu", str)) {
		ftrace_dump_on_oops = DUMP_ORIG;
                return 1;
        }

        return 0;
}
__setup("ftrace_dump_on_oops", set_ftrace_dump_on_oops);

static int __init boot_alloc_snapshot(char *str)
{
	allocate_snapshot = true;
	
	ring_buffer_expanded = true;
	return 1;
}
__setup("alloc_snapshot", boot_alloc_snapshot);


static char trace_boot_options_buf[MAX_TRACER_SIZE] __initdata;
static char *trace_boot_options __initdata;

static int __init set_trace_boot_options(char *str)
{
	strlcpy(trace_boot_options_buf, str, MAX_TRACER_SIZE);
	trace_boot_options = trace_boot_options_buf;
	return 0;
}
__setup("trace_options=", set_trace_boot_options);

unsigned long long ns2usecs(cycle_t nsec)
{
	nsec += 500;
	do_div(nsec, 1000);
	return nsec;
}

static struct trace_array	global_trace;

LIST_HEAD(ftrace_trace_arrays);

int trace_array_get(struct trace_array *this_tr)
{
	struct trace_array *tr;
	int ret = -ENODEV;

	mutex_lock(&trace_types_lock);
	list_for_each_entry(tr, &ftrace_trace_arrays, list) {
		if (tr == this_tr) {
			tr->ref++;
			ret = 0;
			break;
		}
	}
	mutex_unlock(&trace_types_lock);

	return ret;
}

static void __trace_array_put(struct trace_array *this_tr)
{
	WARN_ON(!this_tr->ref);
	this_tr->ref--;
}

void trace_array_put(struct trace_array *this_tr)
{
	mutex_lock(&trace_types_lock);
	__trace_array_put(this_tr);
	mutex_unlock(&trace_types_lock);
}

int filter_current_check_discard(struct ring_buffer *buffer,
				 struct ftrace_event_call *call, void *rec,
				 struct ring_buffer_event *event)
{
	return filter_check_discard(call, rec, buffer, event);
}
EXPORT_SYMBOL_GPL(filter_current_check_discard);

cycle_t buffer_ftrace_now(struct trace_buffer *buf, int cpu)
{
	u64 ts;

	
	if (!buf->buffer)
		return trace_clock_local();

	ts = ring_buffer_time_stamp(buf->buffer, cpu);
	ring_buffer_normalize_time_stamp(buf->buffer, cpu, &ts);

	return ts;
}

cycle_t ftrace_now(int cpu)
{
	return buffer_ftrace_now(&global_trace.trace_buffer, cpu);
}

int tracing_is_enabled(void)
{
	smp_rmb();
	return !global_trace.buffer_disabled;
}

#define TRACE_BUF_SIZE_DEFAULT	1441792UL 

static unsigned long		trace_buf_size = TRACE_BUF_SIZE_DEFAULT;

static struct tracer		*trace_types __read_mostly;

DEFINE_MUTEX(trace_types_lock);


#ifdef CONFIG_SMP
static DECLARE_RWSEM(all_cpu_access_lock);
static DEFINE_PER_CPU(struct mutex, cpu_access_lock);

static inline void trace_access_lock(int cpu)
{
	if (cpu == RING_BUFFER_ALL_CPUS) {
		
		down_write(&all_cpu_access_lock);
	} else {
		

		
		down_read(&all_cpu_access_lock);

		
		mutex_lock(&per_cpu(cpu_access_lock, cpu));
	}
}

static inline void trace_access_unlock(int cpu)
{
	if (cpu == RING_BUFFER_ALL_CPUS) {
		up_write(&all_cpu_access_lock);
	} else {
		mutex_unlock(&per_cpu(cpu_access_lock, cpu));
		up_read(&all_cpu_access_lock);
	}
}

static inline void trace_access_lock_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		mutex_init(&per_cpu(cpu_access_lock, cpu));
}

#else

static DEFINE_MUTEX(access_lock);

static inline void trace_access_lock(int cpu)
{
	(void)cpu;
	mutex_lock(&access_lock);
}

static inline void trace_access_unlock(int cpu)
{
	(void)cpu;
	mutex_unlock(&access_lock);
}

static inline void trace_access_lock_init(void)
{
}

#endif

unsigned long trace_flags = TRACE_ITER_PRINT_PARENT | TRACE_ITER_PRINTK |
	TRACE_ITER_ANNOTATE | TRACE_ITER_CONTEXT_INFO | TRACE_ITER_SLEEP_TIME |
	TRACE_ITER_GRAPH_TIME | TRACE_ITER_RECORD_CMD | TRACE_ITER_OVERWRITE |
	TRACE_ITER_IRQ_INFO | TRACE_ITER_MARKERS | TRACE_ITER_FUNCTION;

void tracer_tracing_on(struct trace_array *tr)
{
	if (tr->trace_buffer.buffer)
		ring_buffer_record_on(tr->trace_buffer.buffer);
	tr->buffer_disabled = 0;
	
	smp_wmb();
}

void tracing_on(void)
{
	tracer_tracing_on(&global_trace);
}
EXPORT_SYMBOL_GPL(tracing_on);

int __trace_puts(unsigned long ip, const char *str, int size)
{
	struct ring_buffer_event *event;
	struct ring_buffer *buffer;
	struct print_entry *entry;
	unsigned long irq_flags;
	int alloc;

	alloc = sizeof(*entry) + size + 2; 

	local_save_flags(irq_flags);
	buffer = global_trace.trace_buffer.buffer;
	event = trace_buffer_lock_reserve(buffer, TRACE_PRINT, alloc, 
					  irq_flags, preempt_count());
	if (!event)
		return 0;

	entry = ring_buffer_event_data(event);
	entry->ip = ip;

	memcpy(&entry->buf, str, size);

	
	if (entry->buf[size - 1] != '\n') {
		entry->buf[size] = '\n';
		entry->buf[size + 1] = '\0';
		stm_log(OST_ENTITY_TRACE_PRINTK, entry->buf, size + 2);
	} else {
		entry->buf[size] = '\0';
		stm_log(OST_ENTITY_TRACE_PRINTK, entry->buf, size + 1);
	}

	__buffer_unlock_commit(buffer, event);

	return size;
}
EXPORT_SYMBOL_GPL(__trace_puts);

int __trace_bputs(unsigned long ip, const char *str)
{
	struct ring_buffer_event *event;
	struct ring_buffer *buffer;
	struct bputs_entry *entry;
	unsigned long irq_flags;
	int size = sizeof(struct bputs_entry);

	local_save_flags(irq_flags);
	buffer = global_trace.trace_buffer.buffer;
	event = trace_buffer_lock_reserve(buffer, TRACE_BPUTS, size,
					  irq_flags, preempt_count());
	if (!event)
		return 0;

	entry = ring_buffer_event_data(event);
	entry->ip			= ip;
	entry->str			= str;
	stm_log(OST_ENTITY_TRACE_PRINTK, entry->str, strlen(entry->str)+1);

	__buffer_unlock_commit(buffer, event);

	return 1;
}
EXPORT_SYMBOL_GPL(__trace_bputs);

#ifdef CONFIG_TRACER_SNAPSHOT
void tracing_snapshot(void)
{
	struct trace_array *tr = &global_trace;
	struct tracer *tracer = tr->current_trace;
	unsigned long flags;

	if (in_nmi()) {
		internal_trace_puts("*** SNAPSHOT CALLED FROM NMI CONTEXT ***\n");
		internal_trace_puts("*** snapshot is being ignored        ***\n");
		return;
	}

	if (!tr->allocated_snapshot) {
		internal_trace_puts("*** SNAPSHOT NOT ALLOCATED ***\n");
		internal_trace_puts("*** stopping trace here!   ***\n");
		tracing_off();
		return;
	}

	
	if (tracer->use_max_tr) {
		internal_trace_puts("*** LATENCY TRACER ACTIVE ***\n");
		internal_trace_puts("*** Can not use snapshot (sorry) ***\n");
		return;
	}

	local_irq_save(flags);
	update_max_tr(tr, current, smp_processor_id());
	local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(tracing_snapshot);

static int resize_buffer_duplicate_size(struct trace_buffer *trace_buf,
					struct trace_buffer *size_buf, int cpu_id);
static void set_buffer_entries(struct trace_buffer *buf, unsigned long val);

static int alloc_snapshot(struct trace_array *tr)
{
	int ret;

	if (!tr->allocated_snapshot) {

		
		ret = resize_buffer_duplicate_size(&tr->max_buffer,
				   &tr->trace_buffer, RING_BUFFER_ALL_CPUS);
		if (ret < 0)
			return ret;

		tr->allocated_snapshot = true;
	}

	return 0;
}

void free_snapshot(struct trace_array *tr)
{
	ring_buffer_resize(tr->max_buffer.buffer, 1, RING_BUFFER_ALL_CPUS);
	set_buffer_entries(&tr->max_buffer, 1);
	tracing_reset_online_cpus(&tr->max_buffer);
	tr->allocated_snapshot = false;
}

void tracing_snapshot_alloc(void)
{
	struct trace_array *tr = &global_trace;
	int ret;

	ret = alloc_snapshot(tr);
	if (WARN_ON(ret < 0))
		return;

	tracing_snapshot();
}
EXPORT_SYMBOL_GPL(tracing_snapshot_alloc);
#else
void tracing_snapshot(void)
{
	WARN_ONCE(1, "Snapshot feature not enabled, but internal snapshot used");
}
EXPORT_SYMBOL_GPL(tracing_snapshot);
void tracing_snapshot_alloc(void)
{
	
	tracing_snapshot();
}
EXPORT_SYMBOL_GPL(tracing_snapshot_alloc);
#endif 

void tracer_tracing_off(struct trace_array *tr)
{
	if (tr->trace_buffer.buffer)
		ring_buffer_record_off(tr->trace_buffer.buffer);
	tr->buffer_disabled = 1;
	
	smp_wmb();
}

void tracing_off(void)
{
	tracer_tracing_off(&global_trace);
}
EXPORT_SYMBOL_GPL(tracing_off);

int tracer_tracing_is_on(struct trace_array *tr)
{
	if (tr->trace_buffer.buffer)
		return ring_buffer_record_is_on(tr->trace_buffer.buffer);
	return !tr->buffer_disabled;
}

int tracing_is_on(void)
{
	return tracer_tracing_is_on(&global_trace);
}
EXPORT_SYMBOL_GPL(tracing_is_on);

static int __init set_buf_size(char *str)
{
	unsigned long buf_size;

	if (!str)
		return 0;
	buf_size = memparse(str, &str);
	
	if (buf_size == 0)
		return 0;
	trace_buf_size = buf_size;
	return 1;
}
__setup("trace_buf_size=", set_buf_size);

static int __init set_tracing_thresh(char *str)
{
	unsigned long threshold;
	int ret;

	if (!str)
		return 0;
	ret = kstrtoul(str, 0, &threshold);
	if (ret < 0)
		return 0;
	tracing_thresh = threshold * 1000;
	return 1;
}
__setup("tracing_thresh=", set_tracing_thresh);

unsigned long nsecs_to_usecs(unsigned long nsecs)
{
	return nsecs / 1000;
}

static const char *trace_options[] = {
	"print-parent",
	"sym-offset",
	"sym-addr",
	"verbose",
	"raw",
	"hex",
	"bin",
	"block",
	"stacktrace",
	"trace_printk",
	"ftrace_preempt",
	"branch",
	"annotate",
	"userstacktrace",
	"sym-userobj",
	"printk-msg-only",
	"context-info",
	"latency-format",
	"sleep-time",
	"graph-time",
	"record-cmd",
	"overwrite",
	"disable_on_free",
	"irq-info",
	"markers",
	"function-trace",
	"print-tgid",
	NULL
};

static struct {
	u64 (*func)(void);
	const char *name;
	int in_ns;		
} trace_clocks[] = {
	{ trace_clock_local,	"local",	1 },
	{ trace_clock_global,	"global",	1 },
	{ trace_clock_counter,	"counter",	0 },
	{ trace_clock_jiffies,	"uptime",	1 },
	{ trace_clock,		"perf",		1 },
	ARCH_TRACE_CLOCKS
};

int trace_parser_get_init(struct trace_parser *parser, int size)
{
	memset(parser, 0, sizeof(*parser));

	parser->buffer = kmalloc(size, GFP_KERNEL);
	if (!parser->buffer)
		return 1;

	parser->size = size;
	return 0;
}

void trace_parser_put(struct trace_parser *parser)
{
	kfree(parser->buffer);
}

int trace_get_user(struct trace_parser *parser, const char __user *ubuf,
	size_t cnt, loff_t *ppos)
{
	char ch;
	size_t read = 0;
	ssize_t ret;

	if (!*ppos)
		trace_parser_clear(parser);

	ret = get_user(ch, ubuf++);
	if (ret)
		goto out;

	read++;
	cnt--;

	if (!parser->cont) {
		
		while (cnt && isspace(ch)) {
			ret = get_user(ch, ubuf++);
			if (ret)
				goto out;
			read++;
			cnt--;
		}

		/* only spaces were written */
		if (isspace(ch)) {
			*ppos += read;
			ret = read;
			goto out;
		}

		parser->idx = 0;
	}

	
	while (cnt && !isspace(ch)) {
		if (parser->idx < parser->size - 1)
			parser->buffer[parser->idx++] = ch;
		else {
			ret = -EINVAL;
			goto out;
		}
		ret = get_user(ch, ubuf++);
		if (ret)
			goto out;
		read++;
		cnt--;
	}

	
	if (isspace(ch)) {
		parser->buffer[parser->idx] = 0;
		parser->cont = false;
	} else if (parser->idx < parser->size - 1) {
		parser->cont = true;
		parser->buffer[parser->idx++] = ch;
	} else {
		ret = -EINVAL;
		goto out;
	}

	*ppos += read;
	ret = read;

out:
	return ret;
}

ssize_t trace_seq_to_user(struct trace_seq *s, char __user *ubuf, size_t cnt)
{
	int len;
	int ret;

	if (!cnt)
		return 0;

	if (s->len <= s->readpos)
		return -EBUSY;

	len = s->len - s->readpos;
	if (cnt > len)
		cnt = len;
	ret = copy_to_user(ubuf, s->buffer + s->readpos, cnt);
	if (ret == cnt)
		return -EFAULT;

	cnt -= ret;

	s->readpos += cnt;
	return cnt;
}

static ssize_t trace_seq_to_buffer(struct trace_seq *s, void *buf, size_t cnt)
{
	int len;

	if (s->len <= s->readpos)
		return -EBUSY;

	len = s->len - s->readpos;
	if (cnt > len)
		cnt = len;
	memcpy(buf, s->buffer + s->readpos, cnt);

	s->readpos += cnt;
	return cnt;
}

static arch_spinlock_t ftrace_max_lock =
	(arch_spinlock_t)__ARCH_SPIN_LOCK_UNLOCKED;

unsigned long __read_mostly	tracing_thresh;

#ifdef CONFIG_TRACER_MAX_TRACE
unsigned long __read_mostly	tracing_max_latency;

static void
__update_max_tr(struct trace_array *tr, struct task_struct *tsk, int cpu)
{
	struct trace_buffer *trace_buf = &tr->trace_buffer;
	struct trace_buffer *max_buf = &tr->max_buffer;
	struct trace_array_cpu *data = per_cpu_ptr(trace_buf->data, cpu);
	struct trace_array_cpu *max_data = per_cpu_ptr(max_buf->data, cpu);

	max_buf->cpu = cpu;
	max_buf->time_start = data->preempt_timestamp;

	max_data->saved_latency = tracing_max_latency;
	max_data->critical_start = data->critical_start;
	max_data->critical_end = data->critical_end;

	memcpy(max_data->comm, tsk->comm, TASK_COMM_LEN);
	max_data->pid = tsk->pid;
	if (tsk == current)
		max_data->uid = current_uid();
	else
		max_data->uid = task_uid(tsk);

	max_data->nice = tsk->static_prio - 20 - MAX_RT_PRIO;
	max_data->policy = tsk->policy;
	max_data->rt_priority = tsk->rt_priority;

	
	tracing_record_cmdline(tsk);
}

void
update_max_tr(struct trace_array *tr, struct task_struct *tsk, int cpu)
{
	struct ring_buffer *buf;

	if (tr->stop_count)
		return;

	WARN_ON_ONCE(!irqs_disabled());

	if (!tr->allocated_snapshot) {
		
		WARN_ON_ONCE(tr->current_trace != &nop_trace);
		return;
	}

	arch_spin_lock(&ftrace_max_lock);

	buf = tr->trace_buffer.buffer;
	tr->trace_buffer.buffer = tr->max_buffer.buffer;
	tr->max_buffer.buffer = buf;

	__update_max_tr(tr, tsk, cpu);
	arch_spin_unlock(&ftrace_max_lock);
}

void
update_max_tr_single(struct trace_array *tr, struct task_struct *tsk, int cpu)
{
	int ret;

	if (tr->stop_count)
		return;

	WARN_ON_ONCE(!irqs_disabled());
	if (!tr->allocated_snapshot) {
		
		WARN_ON_ONCE(tr->current_trace != &nop_trace);
		return;
	}

	arch_spin_lock(&ftrace_max_lock);

	ret = ring_buffer_swap_cpu(tr->max_buffer.buffer, tr->trace_buffer.buffer, cpu);

	if (ret == -EBUSY) {
		trace_array_printk_buf(tr->max_buffer.buffer, _THIS_IP_,
			"Failed to swap buffers due to commit in progress\n");
	}

	WARN_ON_ONCE(ret && ret != -EAGAIN && ret != -EBUSY);

	__update_max_tr(tr, tsk, cpu);
	arch_spin_unlock(&ftrace_max_lock);
}
#endif 

static void default_wait_pipe(struct trace_iterator *iter)
{
	
	if (trace_buffer_iter(iter, iter->cpu_file))
		return;

	ring_buffer_wait(iter->trace_buffer->buffer, iter->cpu_file);
}

#ifdef CONFIG_FTRACE_STARTUP_TEST
static int run_tracer_selftest(struct tracer *type)
{
	struct trace_array *tr = &global_trace;
	struct tracer *saved_tracer = tr->current_trace;
	int ret;

	if (!type->selftest || tracing_selftest_disabled)
		return 0;

	tracing_reset_online_cpus(&tr->trace_buffer);

	tr->current_trace = type;

#ifdef CONFIG_TRACER_MAX_TRACE
	if (type->use_max_tr) {
		
		if (ring_buffer_expanded)
			ring_buffer_resize(tr->max_buffer.buffer, trace_buf_size,
					   RING_BUFFER_ALL_CPUS);
		tr->allocated_snapshot = true;
	}
#endif

	
	pr_info("Testing tracer %s: ", type->name);
	ret = type->selftest(type, tr);
	
	tr->current_trace = saved_tracer;
	if (ret) {
		printk(KERN_CONT "FAILED!\n");
		
		WARN_ON(1);
		return -1;
	}
	
	tracing_reset_online_cpus(&tr->trace_buffer);

#ifdef CONFIG_TRACER_MAX_TRACE
	if (type->use_max_tr) {
		tr->allocated_snapshot = false;

		
		if (ring_buffer_expanded)
			ring_buffer_resize(tr->max_buffer.buffer, 1,
					   RING_BUFFER_ALL_CPUS);
	}
#endif

	printk(KERN_CONT "PASSED\n");
	return 0;
}
#else
static inline int run_tracer_selftest(struct tracer *type)
{
	return 0;
}
#endif 

int register_tracer(struct tracer *type)
{
	struct tracer *t;
	int ret = 0;

	if (!type->name) {
		pr_info("Tracer must have a name\n");
		return -1;
	}

	if (strlen(type->name) >= MAX_TRACER_SIZE) {
		pr_info("Tracer has a name longer than %d\n", MAX_TRACER_SIZE);
		return -1;
	}

	mutex_lock(&trace_types_lock);

	tracing_selftest_running = true;

	for (t = trace_types; t; t = t->next) {
		if (strcmp(type->name, t->name) == 0) {
			
			pr_info("Tracer %s already registered\n",
				type->name);
			ret = -1;
			goto out;
		}
	}

	if (!type->set_flag)
		type->set_flag = &dummy_set_flag;
	if (!type->flags)
		type->flags = &dummy_tracer_flags;
	else
		if (!type->flags->opts)
			type->flags->opts = dummy_tracer_opt;
	if (!type->wait_pipe)
		type->wait_pipe = default_wait_pipe;

	ret = run_tracer_selftest(type);
	if (ret < 0)
		goto out;

	type->next = trace_types;
	trace_types = type;

 out:
	tracing_selftest_running = false;
	mutex_unlock(&trace_types_lock);

	if (ret || !default_bootup_tracer)
		goto out_unlock;

	if (strncmp(default_bootup_tracer, type->name, MAX_TRACER_SIZE))
		goto out_unlock;

	printk(KERN_INFO "Starting tracer '%s'\n", type->name);
	
	tracing_set_tracer(type->name);
	default_bootup_tracer = NULL;
	
	tracing_selftest_disabled = true;
#ifdef CONFIG_FTRACE_STARTUP_TEST
	printk(KERN_INFO "Disabling FTRACE selftests due to running tracer '%s'\n",
	       type->name);
#endif

 out_unlock:
	return ret;
}

void tracing_reset(struct trace_buffer *buf, int cpu)
{
	struct ring_buffer *buffer = buf->buffer;

	if (!buffer)
		return;

	ring_buffer_record_disable(buffer);

	
	synchronize_sched();
	ring_buffer_reset_cpu(buffer, cpu);

	ring_buffer_record_enable(buffer);
}

void tracing_reset_online_cpus(struct trace_buffer *buf)
{
	struct ring_buffer *buffer = buf->buffer;
	int cpu;

	if (!buffer)
		return;

	ring_buffer_record_disable(buffer);

	
	synchronize_sched();

	buf->time_start = buffer_ftrace_now(buf, buf->cpu);

	for_each_online_cpu(cpu)
		ring_buffer_reset_cpu(buffer, cpu);

	ring_buffer_record_enable(buffer);
}

void tracing_reset_all_online_cpus(void)
{
	struct trace_array *tr;

	list_for_each_entry(tr, &ftrace_trace_arrays, list) {
		tracing_reset_online_cpus(&tr->trace_buffer);
#ifdef CONFIG_TRACER_MAX_TRACE
		tracing_reset_online_cpus(&tr->max_buffer);
#endif
	}
}

#define SAVED_CMDLINES 128
#define NO_CMDLINE_MAP UINT_MAX
static unsigned map_pid_to_cmdline[PID_MAX_DEFAULT+1];
static unsigned map_cmdline_to_pid[SAVED_CMDLINES];
static char saved_cmdlines[SAVED_CMDLINES][TASK_COMM_LEN];
static unsigned saved_tgids[SAVED_CMDLINES];
static int cmdline_idx;
static arch_spinlock_t trace_cmdline_lock = __ARCH_SPIN_LOCK_UNLOCKED;

static atomic_t trace_record_cmdline_disabled __read_mostly;

static void trace_init_cmdlines(void)
{
	memset(&map_pid_to_cmdline, NO_CMDLINE_MAP, sizeof(map_pid_to_cmdline));
	memset(&map_cmdline_to_pid, NO_CMDLINE_MAP, sizeof(map_cmdline_to_pid));
	cmdline_idx = 0;
}

int is_tracing_stopped(void)
{
	return global_trace.stop_count;
}

void ftrace_off_permanent(void)
{
	tracing_disabled = 1;
	ftrace_stop();
	tracing_off_permanent();
}

void tracing_start(void)
{
	struct ring_buffer *buffer;
	unsigned long flags;

	if (tracing_disabled)
		return;

	raw_spin_lock_irqsave(&global_trace.start_lock, flags);
	if (--global_trace.stop_count) {
		if (global_trace.stop_count < 0) {
			
			WARN_ON_ONCE(1);
			global_trace.stop_count = 0;
		}
		goto out;
	}

	
	arch_spin_lock(&ftrace_max_lock);

	buffer = global_trace.trace_buffer.buffer;
	if (buffer)
		ring_buffer_record_enable(buffer);

#ifdef CONFIG_TRACER_MAX_TRACE
	buffer = global_trace.max_buffer.buffer;
	if (buffer)
		ring_buffer_record_enable(buffer);
#endif

	arch_spin_unlock(&ftrace_max_lock);

	ftrace_start();
 out:
	raw_spin_unlock_irqrestore(&global_trace.start_lock, flags);
}

static void tracing_start_tr(struct trace_array *tr)
{
	struct ring_buffer *buffer;
	unsigned long flags;

	if (tracing_disabled)
		return;

	
	if (tr->flags & TRACE_ARRAY_FL_GLOBAL)
		return tracing_start();

	raw_spin_lock_irqsave(&tr->start_lock, flags);

	if (--tr->stop_count) {
		if (tr->stop_count < 0) {
			
			WARN_ON_ONCE(1);
			tr->stop_count = 0;
		}
		goto out;
	}

	buffer = tr->trace_buffer.buffer;
	if (buffer)
		ring_buffer_record_enable(buffer);

 out:
	raw_spin_unlock_irqrestore(&tr->start_lock, flags);
}

void tracing_stop(void)
{
	struct ring_buffer *buffer;
	unsigned long flags;

	ftrace_stop();
	raw_spin_lock_irqsave(&global_trace.start_lock, flags);
	if (global_trace.stop_count++)
		goto out;

	
	arch_spin_lock(&ftrace_max_lock);

	buffer = global_trace.trace_buffer.buffer;
	if (buffer)
		ring_buffer_record_disable(buffer);

#ifdef CONFIG_TRACER_MAX_TRACE
	buffer = global_trace.max_buffer.buffer;
	if (buffer)
		ring_buffer_record_disable(buffer);
#endif

	arch_spin_unlock(&ftrace_max_lock);

 out:
	raw_spin_unlock_irqrestore(&global_trace.start_lock, flags);
}

static void tracing_stop_tr(struct trace_array *tr)
{
	struct ring_buffer *buffer;
	unsigned long flags;

	
	if (tr->flags & TRACE_ARRAY_FL_GLOBAL)
		return tracing_stop();

	raw_spin_lock_irqsave(&tr->start_lock, flags);
	if (tr->stop_count++)
		goto out;

	buffer = tr->trace_buffer.buffer;
	if (buffer)
		ring_buffer_record_disable(buffer);

 out:
	raw_spin_unlock_irqrestore(&tr->start_lock, flags);
}

void trace_stop_cmdline_recording(void);

static void trace_save_cmdline(struct task_struct *tsk)
{
	unsigned pid, idx;

	if (!tsk->pid || unlikely(tsk->pid > PID_MAX_DEFAULT))
		return;

	if (!arch_spin_trylock(&trace_cmdline_lock))
		return;

	idx = map_pid_to_cmdline[tsk->pid];
	if (idx == NO_CMDLINE_MAP) {
		idx = (cmdline_idx + 1) % SAVED_CMDLINES;

		pid = map_cmdline_to_pid[idx];
		if (pid != NO_CMDLINE_MAP)
			map_pid_to_cmdline[pid] = NO_CMDLINE_MAP;

		map_cmdline_to_pid[idx] = tsk->pid;
		map_pid_to_cmdline[tsk->pid] = idx;

		cmdline_idx = idx;
	}

	memcpy(&saved_cmdlines[idx], tsk->comm, TASK_COMM_LEN);
	saved_tgids[idx] = tsk->tgid;

	
	if (strlen(saved_cmdlines[idx]) >= TASK_COMM_LEN) {
		
		pr_info("%s: tsk->comm has invalid length! %d:%s len=%d\n",
			__func__, tsk->pid, tsk->comm, strlen(tsk->comm));
		
		saved_cmdlines[idx][TASK_COMM_LEN-1] = '\0';
	}

	arch_spin_unlock(&trace_cmdline_lock);
}

void trace_find_cmdline(int pid, char comm[])
{
	unsigned map;

	if (!pid) {
		strncpy(comm, "<idle>", TASK_COMM_LEN);
		return;
	}

	if (WARN_ON_ONCE(pid < 0)) {
		strncpy(comm, "<XXX>", TASK_COMM_LEN);
		return;
	}

	if (pid > PID_MAX_DEFAULT) {
		strncpy(comm, "<...>", TASK_COMM_LEN);
		return;
	}

	preempt_disable();
	arch_spin_lock(&trace_cmdline_lock);
	map = map_pid_to_cmdline[pid];
	if (map != NO_CMDLINE_MAP)
		strncpy(comm, saved_cmdlines[map], TASK_COMM_LEN);
	else
		strncpy(comm, "<...>", TASK_COMM_LEN);

	arch_spin_unlock(&trace_cmdline_lock);
	preempt_enable();

	
	if (strlen(comm) >= TASK_COMM_LEN) {
		
		pr_info("%s: saved_cmdline has invalid length! comm:%s\n",
			__func__, comm);
		
		comm[TASK_COMM_LEN-1] = '\0';
	}
}

int trace_find_tgid(int pid)
{
	unsigned map;
	int tgid;

	preempt_disable();
	arch_spin_lock(&trace_cmdline_lock);
	map = map_pid_to_cmdline[pid];
	if (map != NO_CMDLINE_MAP)
		tgid = saved_tgids[map];
	else
		tgid = -1;

	arch_spin_unlock(&trace_cmdline_lock);
	preempt_enable();

	return tgid;
}

void tracing_record_cmdline(struct task_struct *tsk)
{
	if (atomic_read(&trace_record_cmdline_disabled) || !tracing_is_on())
		return;

	if (!__this_cpu_read(trace_cmdline_save))
		return;

	__this_cpu_write(trace_cmdline_save, false);

	trace_save_cmdline(tsk);
}

void
tracing_generic_entry_update(struct trace_entry *entry, unsigned long flags,
			     int pc)
{
	struct task_struct *tsk = current;

	entry->preempt_count		= pc & 0xff;
	entry->pid			= (tsk) ? tsk->pid : 0;
	entry->flags =
#ifdef CONFIG_TRACE_IRQFLAGS_SUPPORT
		(irqs_disabled_flags(flags) ? TRACE_FLAG_IRQS_OFF : 0) |
#else
		TRACE_FLAG_IRQS_NOSUPPORT |
#endif
		((pc & HARDIRQ_MASK) ? TRACE_FLAG_HARDIRQ : 0) |
		((pc & SOFTIRQ_MASK) ? TRACE_FLAG_SOFTIRQ : 0) |
		(need_resched() ? TRACE_FLAG_NEED_RESCHED : 0);
}
EXPORT_SYMBOL_GPL(tracing_generic_entry_update);

struct ring_buffer_event *
trace_buffer_lock_reserve(struct ring_buffer *buffer,
			  int type,
			  unsigned long len,
			  unsigned long flags, int pc)
{
	struct ring_buffer_event *event;

	event = ring_buffer_lock_reserve(buffer, len);
	if (event != NULL) {
		struct trace_entry *ent = ring_buffer_event_data(event);

		tracing_generic_entry_update(ent, flags, pc);
		ent->type = type;
	}

	return event;
}

void
__buffer_unlock_commit(struct ring_buffer *buffer, struct ring_buffer_event *event)
{
	__this_cpu_write(trace_cmdline_save, true);
	ring_buffer_unlock_commit(buffer, event);
}

static inline void
__trace_buffer_unlock_commit(struct ring_buffer *buffer,
			     struct ring_buffer_event *event,
			     unsigned long flags, int pc)
{
	__buffer_unlock_commit(buffer, event);

	ftrace_trace_stack(buffer, flags, 6, pc);
	ftrace_trace_userstack(buffer, flags, pc);
}

void trace_buffer_unlock_commit(struct ring_buffer *buffer,
				struct ring_buffer_event *event,
				unsigned long flags, int pc)
{
	__trace_buffer_unlock_commit(buffer, event, flags, pc);
}
EXPORT_SYMBOL_GPL(trace_buffer_unlock_commit);

struct ring_buffer_event *
trace_event_buffer_lock_reserve(struct ring_buffer **current_rb,
			  struct ftrace_event_file *ftrace_file,
			  int type, unsigned long len,
			  unsigned long flags, int pc)
{
	*current_rb = ftrace_file->tr->trace_buffer.buffer;
	return trace_buffer_lock_reserve(*current_rb,
					 type, len, flags, pc);
}
EXPORT_SYMBOL_GPL(trace_event_buffer_lock_reserve);

struct ring_buffer_event *
trace_current_buffer_lock_reserve(struct ring_buffer **current_rb,
				  int type, unsigned long len,
				  unsigned long flags, int pc)
{
	*current_rb = global_trace.trace_buffer.buffer;
	return trace_buffer_lock_reserve(*current_rb,
					 type, len, flags, pc);
}
EXPORT_SYMBOL_GPL(trace_current_buffer_lock_reserve);

void trace_current_buffer_unlock_commit(struct ring_buffer *buffer,
					struct ring_buffer_event *event,
					unsigned long flags, int pc)
{
	__trace_buffer_unlock_commit(buffer, event, flags, pc);
}
EXPORT_SYMBOL_GPL(trace_current_buffer_unlock_commit);

void trace_buffer_unlock_commit_regs(struct ring_buffer *buffer,
				     struct ring_buffer_event *event,
				     unsigned long flags, int pc,
				     struct pt_regs *regs)
{
	__buffer_unlock_commit(buffer, event);

	ftrace_trace_stack_regs(buffer, flags, 0, pc, regs);
	ftrace_trace_userstack(buffer, flags, pc);
}
EXPORT_SYMBOL_GPL(trace_buffer_unlock_commit_regs);

void trace_current_buffer_discard_commit(struct ring_buffer *buffer,
					 struct ring_buffer_event *event)
{
	ring_buffer_discard_commit(buffer, event);
}
EXPORT_SYMBOL_GPL(trace_current_buffer_discard_commit);

void
trace_function(struct trace_array *tr,
	       unsigned long ip, unsigned long parent_ip, unsigned long flags,
	       int pc)
{
	struct ftrace_event_call *call = &event_function;
	struct ring_buffer *buffer = tr->trace_buffer.buffer;
	struct ring_buffer_event *event;
	struct ftrace_entry *entry;

	
	if (unlikely(__this_cpu_read(ftrace_cpu_disabled)))
		return;

	event = trace_buffer_lock_reserve(buffer, TRACE_FN, sizeof(*entry),
					  flags, pc);
	if (!event)
		return;
	entry	= ring_buffer_event_data(event);
	entry->ip			= ip;
	entry->parent_ip		= parent_ip;

	if (!filter_check_discard(call, entry, buffer, event))
		__buffer_unlock_commit(buffer, event);
}

void
ftrace(struct trace_array *tr, struct trace_array_cpu *data,
       unsigned long ip, unsigned long parent_ip, unsigned long flags,
       int pc)
{
	if (likely(!atomic_read(&data->disabled)))
		trace_function(tr, ip, parent_ip, flags, pc);
}

#ifdef CONFIG_STACKTRACE

#define FTRACE_STACK_MAX_ENTRIES (PAGE_SIZE / sizeof(unsigned long))
struct ftrace_stack {
	unsigned long		calls[FTRACE_STACK_MAX_ENTRIES];
};

static DEFINE_PER_CPU(struct ftrace_stack, ftrace_stack);
static DEFINE_PER_CPU(int, ftrace_stack_reserve);

static void __ftrace_trace_stack(struct ring_buffer *buffer,
				 unsigned long flags,
				 int skip, int pc, struct pt_regs *regs)
{
	struct ftrace_event_call *call = &event_kernel_stack;
	struct ring_buffer_event *event;
	struct stack_entry *entry;
	struct stack_trace trace;
	int use_stack;
	int size = FTRACE_STACK_ENTRIES;

	trace.nr_entries	= 0;
	trace.skip		= skip;

	preempt_disable_notrace();

	use_stack = __this_cpu_inc_return(ftrace_stack_reserve);
	barrier();
	if (use_stack == 1) {
		trace.entries		= &__get_cpu_var(ftrace_stack).calls[0];
		trace.max_entries	= FTRACE_STACK_MAX_ENTRIES;

		if (regs)
			save_stack_trace_regs(regs, &trace);
		else
			save_stack_trace(&trace);

		if (trace.nr_entries > size)
			size = trace.nr_entries;
	} else
		
		use_stack = 0;

	size *= sizeof(unsigned long);

	event = trace_buffer_lock_reserve(buffer, TRACE_STACK,
					  sizeof(*entry) + size, flags, pc);
	if (!event)
		goto out;
	entry = ring_buffer_event_data(event);

	memset(&entry->caller, 0, size);

	if (use_stack)
		memcpy(&entry->caller, trace.entries,
		       trace.nr_entries * sizeof(unsigned long));
	else {
		trace.max_entries	= FTRACE_STACK_ENTRIES;
		trace.entries		= entry->caller;
		if (regs)
			save_stack_trace_regs(regs, &trace);
		else
			save_stack_trace(&trace);
	}

	entry->size = trace.nr_entries;

	if (!filter_check_discard(call, entry, buffer, event))
		__buffer_unlock_commit(buffer, event);

 out:
	
	barrier();
	__this_cpu_dec(ftrace_stack_reserve);
	preempt_enable_notrace();

}

void ftrace_trace_stack_regs(struct ring_buffer *buffer, unsigned long flags,
			     int skip, int pc, struct pt_regs *regs)
{
	if (!(trace_flags & TRACE_ITER_STACKTRACE))
		return;

	__ftrace_trace_stack(buffer, flags, skip, pc, regs);
}

void ftrace_trace_stack(struct ring_buffer *buffer, unsigned long flags,
			int skip, int pc)
{
	if (!(trace_flags & TRACE_ITER_STACKTRACE))
		return;

	__ftrace_trace_stack(buffer, flags, skip, pc, NULL);
}

void __trace_stack(struct trace_array *tr, unsigned long flags, int skip,
		   int pc)
{
	__ftrace_trace_stack(tr->trace_buffer.buffer, flags, skip, pc, NULL);
}

void trace_dump_stack(int skip)
{
	unsigned long flags;

	if (tracing_disabled || tracing_selftest_running)
		return;

	local_save_flags(flags);

	skip += 3;
	__ftrace_trace_stack(global_trace.trace_buffer.buffer,
			     flags, skip, preempt_count(), NULL);
}

static DEFINE_PER_CPU(int, user_stack_count);

void
ftrace_trace_userstack(struct ring_buffer *buffer, unsigned long flags, int pc)
{
	struct ftrace_event_call *call = &event_user_stack;
	struct ring_buffer_event *event;
	struct userstack_entry *entry;
	struct stack_trace trace;

	if (!(trace_flags & TRACE_ITER_USERSTACKTRACE))
		return;

	if (unlikely(in_nmi()))
		return;

	preempt_disable();
	if (__this_cpu_read(user_stack_count))
		goto out;

	__this_cpu_inc(user_stack_count);

	event = trace_buffer_lock_reserve(buffer, TRACE_USER_STACK,
					  sizeof(*entry), flags, pc);
	if (!event)
		goto out_drop_count;
	entry	= ring_buffer_event_data(event);

	entry->tgid		= current->tgid;
	memset(&entry->caller, 0, sizeof(entry->caller));

	trace.nr_entries	= 0;
	trace.max_entries	= FTRACE_STACK_ENTRIES;
	trace.skip		= 0;
	trace.entries		= entry->caller;

	save_stack_trace_user(&trace);
	if (!filter_check_discard(call, entry, buffer, event))
		__buffer_unlock_commit(buffer, event);

 out_drop_count:
	__this_cpu_dec(user_stack_count);
 out:
	preempt_enable();
}

#ifdef UNUSED
static void __trace_userstack(struct trace_array *tr, unsigned long flags)
{
	ftrace_trace_userstack(tr, flags, preempt_count());
}
#endif 

#endif 

struct trace_buffer_struct {
	char buffer[TRACE_BUF_SIZE];
};

static struct trace_buffer_struct *trace_percpu_buffer;
static struct trace_buffer_struct *trace_percpu_sirq_buffer;
static struct trace_buffer_struct *trace_percpu_irq_buffer;
static struct trace_buffer_struct *trace_percpu_nmi_buffer;

static char *get_trace_buf(void)
{
	struct trace_buffer_struct *percpu_buffer;

	if (in_nmi())
		percpu_buffer = trace_percpu_nmi_buffer;
	else if (in_irq())
		percpu_buffer = trace_percpu_irq_buffer;
	else if (in_softirq())
		percpu_buffer = trace_percpu_sirq_buffer;
	else
		percpu_buffer = trace_percpu_buffer;

	if (!percpu_buffer)
		return NULL;

	return this_cpu_ptr(&percpu_buffer->buffer[0]);
}

static int alloc_percpu_trace_buffer(void)
{
	struct trace_buffer_struct *buffers;
	struct trace_buffer_struct *sirq_buffers;
	struct trace_buffer_struct *irq_buffers;
	struct trace_buffer_struct *nmi_buffers;

	buffers = alloc_percpu(struct trace_buffer_struct);
	if (!buffers)
		goto err_warn;

	sirq_buffers = alloc_percpu(struct trace_buffer_struct);
	if (!sirq_buffers)
		goto err_sirq;

	irq_buffers = alloc_percpu(struct trace_buffer_struct);
	if (!irq_buffers)
		goto err_irq;

	nmi_buffers = alloc_percpu(struct trace_buffer_struct);
	if (!nmi_buffers)
		goto err_nmi;

	trace_percpu_buffer = buffers;
	trace_percpu_sirq_buffer = sirq_buffers;
	trace_percpu_irq_buffer = irq_buffers;
	trace_percpu_nmi_buffer = nmi_buffers;

	return 0;

 err_nmi:
	free_percpu(irq_buffers);
 err_irq:
	free_percpu(sirq_buffers);
 err_sirq:
	free_percpu(buffers);
 err_warn:
	WARN(1, "Could not allocate percpu trace_printk buffer");
	return -ENOMEM;
}

static int buffers_allocated;

void trace_printk_init_buffers(void)
{
	if (buffers_allocated)
		return;

	if (alloc_percpu_trace_buffer())
		return;

	pr_info("ftrace: Allocated trace_printk buffers\n");

	
	tracing_update_buffers();

	buffers_allocated = 1;

	if (global_trace.trace_buffer.buffer)
		tracing_start_cmdline_record();
}

void trace_printk_start_comm(void)
{
	
	if (!buffers_allocated)
		return;
	tracing_start_cmdline_record();
}

static void trace_printk_start_stop_comm(int enabled)
{
	if (!buffers_allocated)
		return;

	if (enabled)
		tracing_start_cmdline_record();
	else
		tracing_stop_cmdline_record();
}

int trace_vbprintk(unsigned long ip, const char *fmt, va_list args)
{
	struct ftrace_event_call *call = &event_bprint;
	struct ring_buffer_event *event;
	struct ring_buffer *buffer;
	struct trace_array *tr = &global_trace;
	struct bprint_entry *entry;
	unsigned long flags;
	char *tbuffer;
	int len = 0, size, pc;

	if (unlikely(tracing_selftest_running || tracing_disabled))
		return 0;

	
	pause_graph_tracing();

	pc = preempt_count();
	preempt_disable_notrace();

	tbuffer = get_trace_buf();
	if (!tbuffer) {
		len = 0;
		goto out;
	}

	len = vbin_printf((u32 *)tbuffer, TRACE_BUF_SIZE/sizeof(int), fmt, args);

	if (len > TRACE_BUF_SIZE/sizeof(int) || len < 0)
		goto out;

	local_save_flags(flags);
	size = sizeof(*entry) + sizeof(u32) * len;
	buffer = tr->trace_buffer.buffer;
	event = trace_buffer_lock_reserve(buffer, TRACE_BPRINT, size,
					  flags, pc);
	if (!event)
		goto out;
	entry = ring_buffer_event_data(event);
	entry->ip			= ip;
	entry->fmt			= fmt;

	memcpy(entry->buf, tbuffer, sizeof(u32) * len);
	if (!filter_check_discard(call, entry, buffer, event)) {
		__buffer_unlock_commit(buffer, event);
		ftrace_trace_stack(buffer, flags, 6, pc);
	}

out:
	preempt_enable_notrace();
	unpause_graph_tracing();

	return len;
}
EXPORT_SYMBOL_GPL(trace_vbprintk);

static int
__trace_array_vprintk(struct ring_buffer *buffer,
		      unsigned long ip, const char *fmt, va_list args)
{
	struct ftrace_event_call *call = &event_print;
	struct ring_buffer_event *event;
	int len = 0, size, pc;
	struct print_entry *entry;
	unsigned long flags;
	char *tbuffer;

	if (tracing_disabled || tracing_selftest_running)
		return 0;

	
	pause_graph_tracing();

	pc = preempt_count();
	preempt_disable_notrace();


	tbuffer = get_trace_buf();
	if (!tbuffer) {
		len = 0;
		goto out;
	}

	len = vsnprintf(tbuffer, TRACE_BUF_SIZE, fmt, args);
	if (len > TRACE_BUF_SIZE)
		goto out;

	local_save_flags(flags);
	size = sizeof(*entry) + len + 1;
	event = trace_buffer_lock_reserve(buffer, TRACE_PRINT, size,
					  flags, pc);
	if (!event)
		goto out;
	entry = ring_buffer_event_data(event);
	entry->ip = ip;

	memcpy(&entry->buf, tbuffer, len);
	entry->buf[len] = '\0';
	if (!filter_check_discard(call, entry, buffer, event)) {
		stm_log(OST_ENTITY_TRACE_PRINTK, entry->buf, len + 1);
		__buffer_unlock_commit(buffer, event);
		ftrace_trace_stack(buffer, flags, 6, pc);
	}
 out:
	preempt_enable_notrace();
	unpause_graph_tracing();

	return len;
}

int trace_array_vprintk(struct trace_array *tr,
			unsigned long ip, const char *fmt, va_list args)
{
	return __trace_array_vprintk(tr->trace_buffer.buffer, ip, fmt, args);
}

int trace_array_printk(struct trace_array *tr,
		       unsigned long ip, const char *fmt, ...)
{
	int ret;
	va_list ap;

	if (!(trace_flags & TRACE_ITER_PRINTK))
		return 0;

	va_start(ap, fmt);
	ret = trace_array_vprintk(tr, ip, fmt, ap);
	va_end(ap);
	return ret;
}

int trace_array_printk_buf(struct ring_buffer *buffer,
			   unsigned long ip, const char *fmt, ...)
{
	int ret;
	va_list ap;

	if (!(trace_flags & TRACE_ITER_PRINTK))
		return 0;

	va_start(ap, fmt);
	ret = __trace_array_vprintk(buffer, ip, fmt, ap);
	va_end(ap);
	return ret;
}

int trace_vprintk(unsigned long ip, const char *fmt, va_list args)
{
	return trace_array_vprintk(&global_trace, ip, fmt, args);
}
EXPORT_SYMBOL_GPL(trace_vprintk);

static void trace_iterator_increment(struct trace_iterator *iter)
{
	struct ring_buffer_iter *buf_iter = trace_buffer_iter(iter, iter->cpu);

	iter->idx++;
	if (buf_iter)
		ring_buffer_read(buf_iter, NULL);
}

static struct trace_entry *
peek_next_entry(struct trace_iterator *iter, int cpu, u64 *ts,
		unsigned long *lost_events)
{
	struct ring_buffer_event *event;
	struct ring_buffer_iter *buf_iter = trace_buffer_iter(iter, cpu);

	if (buf_iter)
		event = ring_buffer_iter_peek(buf_iter, ts);
	else
		event = ring_buffer_peek(iter->trace_buffer->buffer, cpu, ts,
					 lost_events);

	if (event) {
		iter->ent_size = ring_buffer_event_length(event);
		return ring_buffer_event_data(event);
	}
	iter->ent_size = 0;
	return NULL;
}

static struct trace_entry *
__find_next_entry(struct trace_iterator *iter, int *ent_cpu,
		  unsigned long *missing_events, u64 *ent_ts)
{
	struct ring_buffer *buffer = iter->trace_buffer->buffer;
	struct trace_entry *ent, *next = NULL;
	unsigned long lost_events = 0, next_lost = 0;
	int cpu_file = iter->cpu_file;
	u64 next_ts = 0, ts;
	int next_cpu = -1;
	int next_size = 0;
	int cpu;

	if (cpu_file > RING_BUFFER_ALL_CPUS) {
		if (ring_buffer_empty_cpu(buffer, cpu_file))
			return NULL;
		ent = peek_next_entry(iter, cpu_file, ent_ts, missing_events);
		if (ent_cpu)
			*ent_cpu = cpu_file;

		return ent;
	}

	for_each_tracing_cpu(cpu) {

		if (ring_buffer_empty_cpu(buffer, cpu))
			continue;

		ent = peek_next_entry(iter, cpu, &ts, &lost_events);

		if (ent && (!next || ts < next_ts)) {
			next = ent;
			next_cpu = cpu;
			next_ts = ts;
			next_lost = lost_events;
			next_size = iter->ent_size;
		}
	}

	iter->ent_size = next_size;

	if (ent_cpu)
		*ent_cpu = next_cpu;

	if (ent_ts)
		*ent_ts = next_ts;

	if (missing_events)
		*missing_events = next_lost;

	return next;
}

struct trace_entry *trace_find_next_entry(struct trace_iterator *iter,
					  int *ent_cpu, u64 *ent_ts)
{
	return __find_next_entry(iter, ent_cpu, NULL, ent_ts);
}

void *trace_find_next_entry_inc(struct trace_iterator *iter)
{
	iter->ent = __find_next_entry(iter, &iter->cpu,
				      &iter->lost_events, &iter->ts);

	if (iter->ent)
		trace_iterator_increment(iter);

	return iter->ent ? iter : NULL;
}

static void trace_consume(struct trace_iterator *iter)
{
	ring_buffer_consume(iter->trace_buffer->buffer, iter->cpu, &iter->ts,
			    &iter->lost_events);
}

static void *s_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct trace_iterator *iter = m->private;
	int i = (int)*pos;
	void *ent;

	WARN_ON_ONCE(iter->leftover);

	(*pos)++;

	
	if (iter->idx > i)
		return NULL;

	if (iter->idx < 0)
		ent = trace_find_next_entry_inc(iter);
	else
		ent = iter;

	while (ent && iter->idx < i)
		ent = trace_find_next_entry_inc(iter);

	iter->pos = *pos;

	return ent;
}

void tracing_iter_reset(struct trace_iterator *iter, int cpu)
{
	struct ring_buffer_event *event;
	struct ring_buffer_iter *buf_iter;
	unsigned long entries = 0;
	u64 ts;

	per_cpu_ptr(iter->trace_buffer->data, cpu)->skipped_entries = 0;

	buf_iter = trace_buffer_iter(iter, cpu);
	if (!buf_iter)
		return;

	ring_buffer_iter_reset(buf_iter);

	while ((event = ring_buffer_iter_peek(buf_iter, &ts))) {
		if (ts >= iter->trace_buffer->time_start)
			break;
		entries++;
		ring_buffer_read(buf_iter, NULL);
	}

	per_cpu_ptr(iter->trace_buffer->data, cpu)->skipped_entries = entries;
}

static void *s_start(struct seq_file *m, loff_t *pos)
{
	struct trace_iterator *iter = m->private;
	struct trace_array *tr = iter->tr;
	int cpu_file = iter->cpu_file;
	void *p = NULL;
	loff_t l = 0;
	int cpu;

	mutex_lock(&trace_types_lock);
	if (unlikely(tr->current_trace && iter->trace->name != tr->current_trace->name))
		*iter->trace = *tr->current_trace;
	mutex_unlock(&trace_types_lock);

#ifdef CONFIG_TRACER_MAX_TRACE
	if (iter->snapshot && iter->trace->use_max_tr)
		return ERR_PTR(-EBUSY);
#endif

	if (!iter->snapshot)
		atomic_inc(&trace_record_cmdline_disabled);

	if (*pos != iter->pos) {
		iter->ent = NULL;
		iter->cpu = 0;
		iter->idx = -1;

		if (cpu_file == RING_BUFFER_ALL_CPUS) {
			for_each_tracing_cpu(cpu)
				tracing_iter_reset(iter, cpu);
		} else
			tracing_iter_reset(iter, cpu_file);

		iter->leftover = 0;
		for (p = iter; p && l < *pos; p = s_next(m, p, &l))
			;

	} else {
		if (iter->leftover)
			p = iter;
		else {
			l = *pos - 1;
			p = s_next(m, p, &l);
		}
	}

	trace_event_read_lock();
	trace_access_lock(cpu_file);
	return p;
}

static void s_stop(struct seq_file *m, void *p)
{
	struct trace_iterator *iter = m->private;

#ifdef CONFIG_TRACER_MAX_TRACE
	if (iter->snapshot && iter->trace->use_max_tr)
		return;
#endif

	if (!iter->snapshot)
		atomic_dec(&trace_record_cmdline_disabled);

	trace_access_unlock(iter->cpu_file);
	trace_event_read_unlock();
}

static void
get_total_entries(struct trace_buffer *buf,
		  unsigned long *total, unsigned long *entries)
{
	unsigned long count;
	int cpu;

	*total = 0;
	*entries = 0;

	for_each_tracing_cpu(cpu) {
		count = ring_buffer_entries_cpu(buf->buffer, cpu);
		if (per_cpu_ptr(buf->data, cpu)->skipped_entries) {
			count -= per_cpu_ptr(buf->data, cpu)->skipped_entries;
			
			*total += count;
		} else
			*total += count +
				ring_buffer_overrun_cpu(buf->buffer, cpu);
		*entries += count;
	}
}

static void print_lat_help_header(struct seq_file *m)
{
	seq_puts(m, "#                  _------=> CPU#            \n");
	seq_puts(m, "#                 / _-----=> irqs-off        \n");
	seq_puts(m, "#                | / _----=> need-resched    \n");
	seq_puts(m, "#                || / _---=> hardirq/softirq \n");
	seq_puts(m, "#                ||| / _--=> preempt-depth   \n");
	seq_puts(m, "#                |||| /     delay             \n");
	seq_puts(m, "#  cmd     pid   ||||| time  |   caller      \n");
	seq_puts(m, "#     \\   /      |||||  \\    |   /           \n");
}

static void print_event_info(struct trace_buffer *buf, struct seq_file *m)
{
	unsigned long total;
	unsigned long entries;

	get_total_entries(buf, &total, &entries);
	seq_printf(m, "# entries-in-buffer/entries-written: %lu/%lu   #P:%d\n",
		   entries, total, num_online_cpus());
	seq_puts(m, "#\n");
}

static void print_func_help_header(struct trace_buffer *buf, struct seq_file *m)
{
	print_event_info(buf, m);
	seq_puts(m, "#           TASK-PID   CPU#      TIMESTAMP  FUNCTION\n");
	seq_puts(m, "#              | |       |          |         |\n");
}

static void print_func_help_header_tgid(struct trace_buffer *buf, struct seq_file *m)
{
	print_event_info(buf, m);
	seq_puts(m, "#           TASK-PID    TGID   CPU#      TIMESTAMP  FUNCTION\n");
	seq_puts(m, "#              | |        |      |          |         |\n");
}

static void print_func_help_header_irq(struct trace_buffer *buf, struct seq_file *m)
{
	print_event_info(buf, m);
	seq_puts(m, "#                              _-----=> irqs-off\n");
	seq_puts(m, "#                             / _----=> need-resched\n");
	seq_puts(m, "#                            | / _---=> hardirq/softirq\n");
	seq_puts(m, "#                            || / _--=> preempt-depth\n");
	seq_puts(m, "#                            ||| /     delay\n");
	seq_puts(m, "#           TASK-PID   CPU#  ||||    TIMESTAMP  FUNCTION\n");
	seq_puts(m, "#              | |       |   ||||       |         |\n");
}

static void print_func_help_header_irq_tgid(struct trace_buffer *buf, struct seq_file *m)
{
	print_event_info(buf, m);
	seq_puts(m, "#                                      _-----=> irqs-off\n");
	seq_puts(m, "#                                     / _----=> need-resched\n");
	seq_puts(m, "#                                    | / _---=> hardirq/softirq\n");
	seq_puts(m, "#                                    || / _--=> preempt-depth\n");
	seq_puts(m, "#                                    ||| /     delay\n");
	seq_puts(m, "#           TASK-PID    TGID   CPU#  ||||    TIMESTAMP  FUNCTION\n");
	seq_puts(m, "#              | |        |      |   ||||       |         |\n");
}

void
print_trace_header(struct seq_file *m, struct trace_iterator *iter)
{
	unsigned long sym_flags = (trace_flags & TRACE_ITER_SYM_MASK);
	struct trace_buffer *buf = iter->trace_buffer;
	struct trace_array_cpu *data = per_cpu_ptr(buf->data, buf->cpu);
	struct tracer *type = iter->trace;
	unsigned long entries;
	unsigned long total;
	const char *name = "preemption";

	name = type->name;

	get_total_entries(buf, &total, &entries);

	seq_printf(m, "# %s latency trace v1.1.5 on %s\n",
		   name, UTS_RELEASE);
	seq_puts(m, "# -----------------------------------"
		 "---------------------------------\n");
	seq_printf(m, "# latency: %lu us, #%lu/%lu, CPU#%d |"
		   " (M:%s VP:%d, KP:%d, SP:%d HP:%d",
		   nsecs_to_usecs(data->saved_latency),
		   entries,
		   total,
		   buf->cpu,
#if defined(CONFIG_PREEMPT_NONE)
		   "server",
#elif defined(CONFIG_PREEMPT_VOLUNTARY)
		   "desktop",
#elif defined(CONFIG_PREEMPT)
		   "preempt",
#else
		   "unknown",
#endif
		   
		   0, 0, 0, 0);
#ifdef CONFIG_SMP
	seq_printf(m, " #P:%d)\n", num_online_cpus());
#else
	seq_puts(m, ")\n");
#endif
	seq_puts(m, "#    -----------------\n");
	seq_printf(m, "#    | task: %.16s-%d "
		   "(uid:%d nice:%ld policy:%ld rt_prio:%ld)\n",
		   data->comm, data->pid,
		   from_kuid_munged(seq_user_ns(m), data->uid), data->nice,
		   data->policy, data->rt_priority);
	seq_puts(m, "#    -----------------\n");

	if (data->critical_start) {
		seq_puts(m, "#  => started at: ");
		seq_print_ip_sym(&iter->seq, data->critical_start, sym_flags);
		trace_print_seq(m, &iter->seq);
		seq_puts(m, "\n#  => ended at:   ");
		seq_print_ip_sym(&iter->seq, data->critical_end, sym_flags);
		trace_print_seq(m, &iter->seq);
		seq_puts(m, "\n#\n");
	}

	seq_puts(m, "#\n");
}

static void test_cpu_buff_start(struct trace_iterator *iter)
{
	struct trace_seq *s = &iter->seq;

	if (!(trace_flags & TRACE_ITER_ANNOTATE))
		return;

	if (!(iter->iter_flags & TRACE_FILE_ANNOTATE))
		return;

	if (cpumask_test_cpu(iter->cpu, iter->started))
		return;

	if (per_cpu_ptr(iter->trace_buffer->data, iter->cpu)->skipped_entries)
		return;

	cpumask_set_cpu(iter->cpu, iter->started);

	
	if (iter->idx > 1)
		trace_seq_printf(s, "##### CPU %u buffer started ####\n",
				iter->cpu);
}

static enum print_line_t print_trace_fmt(struct trace_iterator *iter)
{
	struct trace_seq *s = &iter->seq;
	unsigned long sym_flags = (trace_flags & TRACE_ITER_SYM_MASK);
	struct trace_entry *entry;
	struct trace_event *event;

	entry = iter->ent;

	test_cpu_buff_start(iter);

	event = ftrace_find_event(entry->type);

	if (trace_flags & TRACE_ITER_CONTEXT_INFO) {
		if (iter->iter_flags & TRACE_FILE_LAT_FMT) {
			if (!trace_print_lat_context(iter))
				goto partial;
		} else {
			if (!trace_print_context(iter))
				goto partial;
		}
	}

	if (event)
		return event->funcs->trace(iter, sym_flags, event);

	if (!trace_seq_printf(s, "Unknown type %d\n", entry->type))
		goto partial;

	return TRACE_TYPE_HANDLED;
partial:
	return TRACE_TYPE_PARTIAL_LINE;
}

static enum print_line_t print_raw_fmt(struct trace_iterator *iter)
{
	struct trace_seq *s = &iter->seq;
	struct trace_entry *entry;
	struct trace_event *event;

	entry = iter->ent;

	if (trace_flags & TRACE_ITER_CONTEXT_INFO) {
		if (!trace_seq_printf(s, "%d %d %llu ",
				      entry->pid, iter->cpu, iter->ts))
			goto partial;
	}

	event = ftrace_find_event(entry->type);
	if (event)
		return event->funcs->raw(iter, 0, event);

	if (!trace_seq_printf(s, "%d ?\n", entry->type))
		goto partial;

	return TRACE_TYPE_HANDLED;
partial:
	return TRACE_TYPE_PARTIAL_LINE;
}

static enum print_line_t print_hex_fmt(struct trace_iterator *iter)
{
	struct trace_seq *s = &iter->seq;
	unsigned char newline = '\n';
	struct trace_entry *entry;
	struct trace_event *event;

	entry = iter->ent;

	if (trace_flags & TRACE_ITER_CONTEXT_INFO) {
		SEQ_PUT_HEX_FIELD_RET(s, entry->pid);
		SEQ_PUT_HEX_FIELD_RET(s, iter->cpu);
		SEQ_PUT_HEX_FIELD_RET(s, iter->ts);
	}

	event = ftrace_find_event(entry->type);
	if (event) {
		enum print_line_t ret = event->funcs->hex(iter, 0, event);
		if (ret != TRACE_TYPE_HANDLED)
			return ret;
	}

	SEQ_PUT_FIELD_RET(s, newline);

	return TRACE_TYPE_HANDLED;
}

static enum print_line_t print_bin_fmt(struct trace_iterator *iter)
{
	struct trace_seq *s = &iter->seq;
	struct trace_entry *entry;
	struct trace_event *event;

	entry = iter->ent;

	if (trace_flags & TRACE_ITER_CONTEXT_INFO) {
		SEQ_PUT_FIELD_RET(s, entry->pid);
		SEQ_PUT_FIELD_RET(s, iter->cpu);
		SEQ_PUT_FIELD_RET(s, iter->ts);
	}

	event = ftrace_find_event(entry->type);
	return event ? event->funcs->binary(iter, 0, event) :
		TRACE_TYPE_HANDLED;
}

int trace_empty(struct trace_iterator *iter)
{
	struct ring_buffer_iter *buf_iter;
	int cpu;

	
	if (iter->cpu_file != RING_BUFFER_ALL_CPUS) {
		cpu = iter->cpu_file;
		buf_iter = trace_buffer_iter(iter, cpu);
		if (buf_iter) {
			if (!ring_buffer_iter_empty(buf_iter))
				return 0;
		} else {
			if (!ring_buffer_empty_cpu(iter->trace_buffer->buffer, cpu))
				return 0;
		}
		return 1;
	}

	for_each_tracing_cpu(cpu) {
		buf_iter = trace_buffer_iter(iter, cpu);
		if (buf_iter) {
			if (!ring_buffer_iter_empty(buf_iter))
				return 0;
		} else {
			if (!ring_buffer_empty_cpu(iter->trace_buffer->buffer, cpu))
				return 0;
		}
	}

	return 1;
}

enum print_line_t print_trace_line(struct trace_iterator *iter)
{
	enum print_line_t ret;

	if (iter->lost_events &&
	    !trace_seq_printf(&iter->seq, "CPU:%d [LOST %lu EVENTS]\n",
				 iter->cpu, iter->lost_events))
		return TRACE_TYPE_PARTIAL_LINE;

	if (iter->trace && iter->trace->print_line) {
		ret = iter->trace->print_line(iter);
		if (ret != TRACE_TYPE_UNHANDLED)
			return ret;
	}

	if (iter->ent->type == TRACE_BPUTS &&
			trace_flags & TRACE_ITER_PRINTK &&
			trace_flags & TRACE_ITER_PRINTK_MSGONLY)
		return trace_print_bputs_msg_only(iter);

	if (iter->ent->type == TRACE_BPRINT &&
			trace_flags & TRACE_ITER_PRINTK &&
			trace_flags & TRACE_ITER_PRINTK_MSGONLY)
		return trace_print_bprintk_msg_only(iter);

	if (iter->ent->type == TRACE_PRINT &&
			trace_flags & TRACE_ITER_PRINTK &&
			trace_flags & TRACE_ITER_PRINTK_MSGONLY)
		return trace_print_printk_msg_only(iter);

	if (trace_flags & TRACE_ITER_BIN)
		return print_bin_fmt(iter);

	if (trace_flags & TRACE_ITER_HEX)
		return print_hex_fmt(iter);

	if (trace_flags & TRACE_ITER_RAW)
		return print_raw_fmt(iter);

	return print_trace_fmt(iter);
}

void trace_latency_header(struct seq_file *m)
{
	struct trace_iterator *iter = m->private;

	
	if (trace_empty(iter))
		return;

	if (iter->iter_flags & TRACE_FILE_LAT_FMT)
		print_trace_header(m, iter);

	if (!(trace_flags & TRACE_ITER_VERBOSE))
		print_lat_help_header(m);
}

void trace_default_header(struct seq_file *m)
{
	struct trace_iterator *iter = m->private;

	if (!(trace_flags & TRACE_ITER_CONTEXT_INFO))
		return;

	if (iter->iter_flags & TRACE_FILE_LAT_FMT) {
		
		if (trace_empty(iter))
			return;
		print_trace_header(m, iter);
		if (!(trace_flags & TRACE_ITER_VERBOSE))
			print_lat_help_header(m);
	} else {
		if (!(trace_flags & TRACE_ITER_VERBOSE)) {
			if (trace_flags & TRACE_ITER_IRQ_INFO)
				if (trace_flags & TRACE_ITER_TGID)
					print_func_help_header_irq_tgid(iter->trace_buffer, m);
				else
					print_func_help_header_irq(iter->trace_buffer, m);
			else
				if (trace_flags & TRACE_ITER_TGID)
					print_func_help_header_tgid(iter->trace_buffer, m);
				else
					print_func_help_header(iter->trace_buffer, m);
		}
	}
}

static void test_ftrace_alive(struct seq_file *m)
{
	if (!ftrace_is_dead())
		return;
	seq_printf(m, "# WARNING: FUNCTION TRACING IS CORRUPTED\n");
	seq_printf(m, "#          MAY BE MISSING FUNCTION EVENTS\n");
}

#ifdef CONFIG_TRACER_MAX_TRACE
static void show_snapshot_main_help(struct seq_file *m)
{
	seq_printf(m, "# echo 0 > snapshot : Clears and frees snapshot buffer\n");
	seq_printf(m, "# echo 1 > snapshot : Allocates snapshot buffer, if not already allocated.\n");
	seq_printf(m, "#                      Takes a snapshot of the main buffer.\n");
	seq_printf(m, "# echo 2 > snapshot : Clears snapshot buffer (but does not allocate)\n");
	seq_printf(m, "#                      (Doesn't have to be '2' works with any number that\n");
	seq_printf(m, "#                       is not a '0' or '1')\n");
}

static void show_snapshot_percpu_help(struct seq_file *m)
{
	seq_printf(m, "# echo 0 > snapshot : Invalid for per_cpu snapshot file.\n");
#ifdef CONFIG_RING_BUFFER_ALLOW_SWAP
	seq_printf(m, "# echo 1 > snapshot : Allocates snapshot buffer, if not already allocated.\n");
	seq_printf(m, "#                      Takes a snapshot of the main buffer for this cpu.\n");
#else
	seq_printf(m, "# echo 1 > snapshot : Not supported with this kernel.\n");
	seq_printf(m, "#                     Must use main snapshot file to allocate.\n");
#endif
	seq_printf(m, "# echo 2 > snapshot : Clears this cpu's snapshot buffer (but does not allocate)\n");
	seq_printf(m, "#                      (Doesn't have to be '2' works with any number that\n");
	seq_printf(m, "#                       is not a '0' or '1')\n");
}

static void print_snapshot_help(struct seq_file *m, struct trace_iterator *iter)
{
	if (iter->tr->allocated_snapshot)
		seq_printf(m, "#\n# * Snapshot is allocated *\n#\n");
	else
		seq_printf(m, "#\n# * Snapshot is freed *\n#\n");

	seq_printf(m, "# Snapshot commands:\n");
	if (iter->cpu_file == RING_BUFFER_ALL_CPUS)
		show_snapshot_main_help(m);
	else
		show_snapshot_percpu_help(m);
}
#else
static inline void print_snapshot_help(struct seq_file *m, struct trace_iterator *iter) { }
#endif

static int s_show(struct seq_file *m, void *v)
{
	struct trace_iterator *iter = v;
	int ret;

	if (iter->ent == NULL) {
		if (iter->tr) {
			seq_printf(m, "# tracer: %s\n", iter->trace->name);
			seq_puts(m, "#\n");
			test_ftrace_alive(m);
		}
		if (iter->snapshot && trace_empty(iter))
			print_snapshot_help(m, iter);
		else if (iter->trace && iter->trace->print_header)
			iter->trace->print_header(m);
		else
			trace_default_header(m);

	} else if (iter->leftover) {
		ret = trace_print_seq(m, &iter->seq);

		
		iter->leftover = ret;

	} else {
		print_trace_line(iter);
		ret = trace_print_seq(m, &iter->seq);
		iter->leftover = ret;
	}

	return 0;
}

static inline int tracing_get_cpu(struct inode *inode)
{
	if (inode->i_cdev) 
		return (long)inode->i_cdev - 1;
	return RING_BUFFER_ALL_CPUS;
}

static const struct seq_operations tracer_seq_ops = {
	.start		= s_start,
	.next		= s_next,
	.stop		= s_stop,
	.show		= s_show,
};

static struct trace_iterator *
__tracing_open(struct inode *inode, struct file *file, bool snapshot)
{
	struct trace_array *tr = inode->i_private;
	struct trace_iterator *iter;
	int cpu;

	if (tracing_disabled)
		return ERR_PTR(-ENODEV);

	iter = __seq_open_private(file, &tracer_seq_ops, sizeof(*iter));
	if (!iter)
		return ERR_PTR(-ENOMEM);

	iter->buffer_iter = kzalloc(sizeof(*iter->buffer_iter) * num_possible_cpus(),
				    GFP_KERNEL);
	if (!iter->buffer_iter)
		goto release;

	mutex_lock(&trace_types_lock);
	iter->trace = kzalloc(sizeof(*iter->trace), GFP_KERNEL);
	if (!iter->trace)
		goto fail;

	*iter->trace = *tr->current_trace;

	if (!zalloc_cpumask_var(&iter->started, GFP_KERNEL))
		goto fail;

	iter->tr = tr;

#ifdef CONFIG_TRACER_MAX_TRACE
	
	if (tr->current_trace->print_max || snapshot)
		iter->trace_buffer = &tr->max_buffer;
	else
#endif
		iter->trace_buffer = &tr->trace_buffer;
	iter->snapshot = snapshot;
	iter->pos = -1;
	iter->cpu_file = tracing_get_cpu(inode);
	mutex_init(&iter->mutex);

	
	if (iter->trace && iter->trace->open)
		iter->trace->open(iter);

	
	if (ring_buffer_overruns(iter->trace_buffer->buffer))
		iter->iter_flags |= TRACE_FILE_ANNOTATE;

	
	if (trace_clocks[tr->clock_id].in_ns)
		iter->iter_flags |= TRACE_FILE_TIME_IN_NS;

	
	if (!iter->snapshot)
		tracing_stop_tr(tr);

	if (iter->cpu_file == RING_BUFFER_ALL_CPUS) {
		for_each_tracing_cpu(cpu) {
			iter->buffer_iter[cpu] =
				ring_buffer_read_prepare(iter->trace_buffer->buffer, cpu);
		}
		ring_buffer_read_prepare_sync();
		for_each_tracing_cpu(cpu) {
			ring_buffer_read_start(iter->buffer_iter[cpu]);
			tracing_iter_reset(iter, cpu);
		}
	} else {
		cpu = iter->cpu_file;
		iter->buffer_iter[cpu] =
			ring_buffer_read_prepare(iter->trace_buffer->buffer, cpu);
		ring_buffer_read_prepare_sync();
		ring_buffer_read_start(iter->buffer_iter[cpu]);
		tracing_iter_reset(iter, cpu);
	}

	mutex_unlock(&trace_types_lock);

	return iter;

 fail:
	mutex_unlock(&trace_types_lock);
	kfree(iter->trace);
	kfree(iter->buffer_iter);
release:
	seq_release_private(inode, file);
	return ERR_PTR(-ENOMEM);
}

int tracing_open_generic(struct inode *inode, struct file *filp)
{
	if (tracing_disabled)
		return -ENODEV;

	filp->private_data = inode->i_private;
	return 0;
}

int tracing_open_generic_tr(struct inode *inode, struct file *filp)
{
	struct trace_array *tr = inode->i_private;

	if (tracing_disabled)
		return -ENODEV;

	if (trace_array_get(tr) < 0)
		return -ENODEV;

	filp->private_data = inode->i_private;

	return 0;
}

static int tracing_release(struct inode *inode, struct file *file)
{
	struct trace_array *tr = inode->i_private;
	struct seq_file *m = file->private_data;
	struct trace_iterator *iter;
	int cpu;

	if (!(file->f_mode & FMODE_READ)) {
		trace_array_put(tr);
		return 0;
	}

	
	iter = m->private;
	mutex_lock(&trace_types_lock);

	for_each_tracing_cpu(cpu) {
		if (iter->buffer_iter[cpu])
			ring_buffer_read_finish(iter->buffer_iter[cpu]);
	}

	if (iter->trace && iter->trace->close)
		iter->trace->close(iter);

	if (!iter->snapshot)
		
		tracing_start_tr(tr);

	__trace_array_put(tr);

	mutex_unlock(&trace_types_lock);

	mutex_destroy(&iter->mutex);
	free_cpumask_var(iter->started);
	kfree(iter->trace);
	kfree(iter->buffer_iter);
	seq_release_private(inode, file);

	return 0;
}

static int tracing_release_generic_tr(struct inode *inode, struct file *file)
{
	struct trace_array *tr = inode->i_private;

	trace_array_put(tr);
	return 0;
}

static int tracing_single_release_tr(struct inode *inode, struct file *file)
{
	struct trace_array *tr = inode->i_private;

	trace_array_put(tr);

	return single_release(inode, file);
}

static int tracing_open(struct inode *inode, struct file *file)
{
	struct trace_array *tr = inode->i_private;
	struct trace_iterator *iter;
	int ret = 0;

	if (trace_array_get(tr) < 0)
		return -ENODEV;

	
	if ((file->f_mode & FMODE_WRITE) && (file->f_flags & O_TRUNC)) {
		int cpu = tracing_get_cpu(inode);

		if (cpu == RING_BUFFER_ALL_CPUS)
			tracing_reset_online_cpus(&tr->trace_buffer);
		else
			tracing_reset(&tr->trace_buffer, cpu);
	}

	if (file->f_mode & FMODE_READ) {
		iter = __tracing_open(inode, file, false);
		if (IS_ERR(iter))
			ret = PTR_ERR(iter);
		else if (trace_flags & TRACE_ITER_LATENCY_FMT)
			iter->iter_flags |= TRACE_FILE_LAT_FMT;
	}

	if (ret < 0)
		trace_array_put(tr);

	return ret;
}

static void *
t_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct tracer *t = v;

	(*pos)++;

	if (t)
		t = t->next;

	return t;
}

static void *t_start(struct seq_file *m, loff_t *pos)
{
	struct tracer *t;
	loff_t l = 0;

	mutex_lock(&trace_types_lock);
	for (t = trace_types; t && l < *pos; t = t_next(m, t, &l))
		;

	return t;
}

static void t_stop(struct seq_file *m, void *p)
{
	mutex_unlock(&trace_types_lock);
}

static int t_show(struct seq_file *m, void *v)
{
	struct tracer *t = v;

	if (!t)
		return 0;

	seq_printf(m, "%s", t->name);
	if (t->next)
		seq_putc(m, ' ');
	else
		seq_putc(m, '\n');

	return 0;
}

static const struct seq_operations show_traces_seq_ops = {
	.start		= t_start,
	.next		= t_next,
	.stop		= t_stop,
	.show		= t_show,
};

static int show_traces_open(struct inode *inode, struct file *file)
{
	if (tracing_disabled)
		return -ENODEV;

	return seq_open(file, &show_traces_seq_ops);
}

static ssize_t
tracing_write_stub(struct file *filp, const char __user *ubuf,
		   size_t count, loff_t *ppos)
{
	return count;
}

static loff_t tracing_seek(struct file *file, loff_t offset, int origin)
{
	if (file->f_mode & FMODE_READ)
		return seq_lseek(file, offset, origin);
	else
		return 0;
}

static const struct file_operations tracing_fops = {
	.open		= tracing_open,
	.read		= seq_read,
	.write		= tracing_write_stub,
	.llseek		= tracing_seek,
	.release	= tracing_release,
};

static const struct file_operations show_traces_fops = {
	.open		= show_traces_open,
	.read		= seq_read,
	.release	= seq_release,
	.llseek		= seq_lseek,
};

static cpumask_var_t tracing_cpumask;

static DEFINE_MUTEX(tracing_cpumask_update_lock);

static char mask_str[NR_CPUS + 1];

static ssize_t
tracing_cpumask_read(struct file *filp, char __user *ubuf,
		     size_t count, loff_t *ppos)
{
	int len;

	mutex_lock(&tracing_cpumask_update_lock);

	len = cpumask_scnprintf(mask_str, count, tracing_cpumask);
	if (count - len < 2) {
		count = -EINVAL;
		goto out_err;
	}
	len += sprintf(mask_str + len, "\n");
	count = simple_read_from_buffer(ubuf, count, ppos, mask_str, NR_CPUS+1);

out_err:
	mutex_unlock(&tracing_cpumask_update_lock);

	return count;
}

static ssize_t
tracing_cpumask_write(struct file *filp, const char __user *ubuf,
		      size_t count, loff_t *ppos)
{
	struct trace_array *tr = filp->private_data;
	cpumask_var_t tracing_cpumask_new;
	int err, cpu;

	if (!alloc_cpumask_var(&tracing_cpumask_new, GFP_KERNEL))
		return -ENOMEM;

	err = cpumask_parse_user(ubuf, count, tracing_cpumask_new);
	if (err)
		goto err_unlock;

	mutex_lock(&tracing_cpumask_update_lock);

	local_irq_disable();
	arch_spin_lock(&ftrace_max_lock);
	for_each_tracing_cpu(cpu) {
		if (cpumask_test_cpu(cpu, tracing_cpumask) &&
				!cpumask_test_cpu(cpu, tracing_cpumask_new)) {
			atomic_inc(&per_cpu_ptr(tr->trace_buffer.data, cpu)->disabled);
			ring_buffer_record_disable_cpu(tr->trace_buffer.buffer, cpu);
		}
		if (!cpumask_test_cpu(cpu, tracing_cpumask) &&
				cpumask_test_cpu(cpu, tracing_cpumask_new)) {
			atomic_dec(&per_cpu_ptr(tr->trace_buffer.data, cpu)->disabled);
			ring_buffer_record_enable_cpu(tr->trace_buffer.buffer, cpu);
		}
	}
	arch_spin_unlock(&ftrace_max_lock);
	local_irq_enable();

	cpumask_copy(tracing_cpumask, tracing_cpumask_new);

	mutex_unlock(&tracing_cpumask_update_lock);
	free_cpumask_var(tracing_cpumask_new);

	return count;

err_unlock:
	free_cpumask_var(tracing_cpumask_new);

	return err;
}

static const struct file_operations tracing_cpumask_fops = {
	.open		= tracing_open_generic,
	.read		= tracing_cpumask_read,
	.write		= tracing_cpumask_write,
	.llseek		= generic_file_llseek,
};

static int tracing_trace_options_show(struct seq_file *m, void *v)
{
	struct tracer_opt *trace_opts;
	struct trace_array *tr = m->private;
	u32 tracer_flags;
	int i;

	mutex_lock(&trace_types_lock);
	tracer_flags = tr->current_trace->flags->val;
	trace_opts = tr->current_trace->flags->opts;

	for (i = 0; trace_options[i]; i++) {
		if (trace_flags & (1 << i))
			seq_printf(m, "%s\n", trace_options[i]);
		else
			seq_printf(m, "no%s\n", trace_options[i]);
	}

	for (i = 0; trace_opts[i].name; i++) {
		if (tracer_flags & trace_opts[i].bit)
			seq_printf(m, "%s\n", trace_opts[i].name);
		else
			seq_printf(m, "no%s\n", trace_opts[i].name);
	}
	mutex_unlock(&trace_types_lock);

	return 0;
}

static int __set_tracer_option(struct tracer *trace,
			       struct tracer_flags *tracer_flags,
			       struct tracer_opt *opts, int neg)
{
	int ret;

	ret = trace->set_flag(tracer_flags->val, opts->bit, !neg);
	if (ret)
		return ret;

	if (neg)
		tracer_flags->val &= ~opts->bit;
	else
		tracer_flags->val |= opts->bit;
	return 0;
}

static int set_tracer_option(struct tracer *trace, char *cmp, int neg)
{
	struct tracer_flags *tracer_flags = trace->flags;
	struct tracer_opt *opts = NULL;
	int i;

	for (i = 0; tracer_flags->opts[i].name; i++) {
		opts = &tracer_flags->opts[i];

		if (strcmp(cmp, opts->name) == 0)
			return __set_tracer_option(trace, trace->flags,
						   opts, neg);
	}

	return -EINVAL;
}

int trace_keep_overwrite(struct tracer *tracer, u32 mask, int set)
{
	if (tracer->enabled && (mask & TRACE_ITER_OVERWRITE) && !set)
		return -1;

	return 0;
}

int set_tracer_flag(struct trace_array *tr, unsigned int mask, int enabled)
{
	
	if (!!(trace_flags & mask) == !!enabled)
		return 0;

	
	if (tr->current_trace->flag_changed)
		if (tr->current_trace->flag_changed(tr->current_trace, mask, !!enabled))
			return -EINVAL;

	if (enabled)
		trace_flags |= mask;
	else
		trace_flags &= ~mask;

	if (mask == TRACE_ITER_RECORD_CMD)
		trace_event_enable_cmd_record(enabled);

	if (mask == TRACE_ITER_OVERWRITE) {
		ring_buffer_change_overwrite(tr->trace_buffer.buffer, enabled);
#ifdef CONFIG_TRACER_MAX_TRACE
		ring_buffer_change_overwrite(tr->max_buffer.buffer, enabled);
#endif
	}

	if (mask == TRACE_ITER_PRINTK)
		trace_printk_start_stop_comm(enabled);

	return 0;
}

static int trace_set_options(struct trace_array *tr, char *option)
{
	char *cmp;
	int neg = 0;
	int ret = -ENODEV;
	int i;

	cmp = strstrip(option);

	if (strncmp(cmp, "no", 2) == 0) {
		neg = 1;
		cmp += 2;
	}

	mutex_lock(&trace_types_lock);

	for (i = 0; trace_options[i]; i++) {
		if (strcmp(cmp, trace_options[i]) == 0) {
			ret = set_tracer_flag(tr, 1 << i, !neg);
			break;
		}
	}

	
	if (!trace_options[i])
		ret = set_tracer_option(tr->current_trace, cmp, neg);

	mutex_unlock(&trace_types_lock);

	return ret;
}

static ssize_t
tracing_trace_options_write(struct file *filp, const char __user *ubuf,
			size_t cnt, loff_t *ppos)
{
	struct seq_file *m = filp->private_data;
	struct trace_array *tr = m->private;
	char buf[64];
	int ret;

	if (cnt >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(&buf, ubuf, cnt))
		return -EFAULT;

	buf[cnt] = 0;

	ret = trace_set_options(tr, buf);
	if (ret < 0)
		return ret;

	*ppos += cnt;

	return cnt;
}

static int tracing_trace_options_open(struct inode *inode, struct file *file)
{
	struct trace_array *tr = inode->i_private;
	int ret;

	if (tracing_disabled)
		return -ENODEV;

	if (trace_array_get(tr) < 0)
		return -ENODEV;

	ret = single_open(file, tracing_trace_options_show, inode->i_private);
	if (ret < 0)
		trace_array_put(tr);

	return ret;
}

static const struct file_operations tracing_iter_fops = {
	.open		= tracing_trace_options_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= tracing_single_release_tr,
	.write		= tracing_trace_options_write,
};

static const char readme_msg[] =
	"tracing mini-HOWTO:\n\n"
	"# echo 0 > tracing_on : quick way to disable tracing\n"
	"# echo 1 > tracing_on : quick way to re-enable tracing\n\n"
	" Important files:\n"
	"  trace\t\t\t- The static contents of the buffer\n"
	"\t\t\t  To clear the buffer write into this file: echo > trace\n"
	"  trace_pipe\t\t- A consuming read to see the contents of the buffer\n"
	"  current_tracer\t- function and latency tracers\n"
	"  available_tracers\t- list of configured tracers for current_tracer\n"
	"  buffer_size_kb\t- view and modify size of per cpu buffer\n"
	"  buffer_total_size_kb  - view total size of all cpu buffers\n\n"
	"  trace_clock\t\t-change the clock used to order events\n"
	"       local:   Per cpu clock but may not be synced across CPUs\n"
	"      global:   Synced across CPUs but slows tracing down.\n"
	"     counter:   Not a clock, but just an increment\n"
	"      uptime:   Jiffy counter from time of boot\n"
	"        perf:   Same clock that perf events use\n"
#ifdef CONFIG_X86_64
	"     x86-tsc:   TSC cycle counter\n"
#endif
	"\n  trace_marker\t\t- Writes into this file writes into the kernel buffer\n"
	"  tracing_cpumask\t- Limit which CPUs to trace\n"
	"  instances\t\t- Make sub-buffers with: mkdir instances/foo\n"
	"\t\t\t  Remove sub-buffer with rmdir\n"
	"  trace_options\t\t- Set format or modify how tracing happens\n"
	"\t\t\t  Disable an option by adding a suffix 'no' to the option name\n"
#ifdef CONFIG_DYNAMIC_FTRACE
	"\n  available_filter_functions - list of functions that can be filtered on\n"
	"  set_ftrace_filter\t- echo function name in here to only trace these functions\n"
	"            accepts: func_full_name, *func_end, func_begin*, *func_middle*\n"
	"            modules: Can select a group via module\n"
	"             Format: :mod:<module-name>\n"
	"             example: echo :mod:ext3 > set_ftrace_filter\n"
	"            triggers: a command to perform when function is hit\n"
	"              Format: <function>:<trigger>[:count]\n"
	"             trigger: traceon, traceoff\n"
	"                      enable_event:<system>:<event>\n"
	"                      disable_event:<system>:<event>\n"
#ifdef CONFIG_STACKTRACE
	"                      stacktrace\n"
#endif
#ifdef CONFIG_TRACER_SNAPSHOT
	"                      snapshot\n"
#endif
	"             example: echo do_fault:traceoff > set_ftrace_filter\n"
	"                      echo do_trap:traceoff:3 > set_ftrace_filter\n"
	"             The first one will disable tracing every time do_fault is hit\n"
	"             The second will disable tracing at most 3 times when do_trap is hit\n"
	"               The first time do trap is hit and it disables tracing, the counter\n"
	"               will decrement to 2. If tracing is already disabled, the counter\n"
	"               will not decrement. It only decrements when the trigger did work\n"
	"             To remove trigger without count:\n"
	"               echo '!<function>:<trigger> > set_ftrace_filter\n"
	"             To remove trigger with a count:\n"
	"               echo '!<function>:<trigger>:0 > set_ftrace_filter\n"
	"  set_ftrace_notrace\t- echo function name in here to never trace.\n"
	"            accepts: func_full_name, *func_end, func_begin*, *func_middle*\n"
	"            modules: Can select a group via module command :mod:\n"
	"            Does not accept triggers\n"
#endif 
#ifdef CONFIG_FUNCTION_TRACER
	"  set_ftrace_pid\t- Write pid(s) to only function trace those pids (function)\n"
#endif
#ifdef CONFIG_FUNCTION_GRAPH_TRACER
	"  set_graph_function\t- Trace the nested calls of a function (function_graph)\n"
	"  max_graph_depth\t- Trace a limited depth of nested calls (0 is unlimited)\n"
#endif
#ifdef CONFIG_TRACER_SNAPSHOT
	"\n  snapshot\t\t- Like 'trace' but shows the content of the static snapshot buffer\n"
	"\t\t\t  Read the contents for more information\n"
#endif
#ifdef CONFIG_STACKTRACE
	"  stack_trace\t\t- Shows the max stack trace when active\n"
	"  stack_max_size\t- Shows current max stack size that was traced\n"
	"\t\t\t  Write into this file to reset the max size (trigger a new trace)\n"
#ifdef CONFIG_DYNAMIC_FTRACE
	"  stack_trace_filter\t- Like set_ftrace_filter but limits what stack_trace traces\n"
#endif
#endif 
;

static ssize_t
tracing_readme_read(struct file *filp, char __user *ubuf,
		       size_t cnt, loff_t *ppos)
{
	return simple_read_from_buffer(ubuf, cnt, ppos,
					readme_msg, strlen(readme_msg));
}

static const struct file_operations tracing_readme_fops = {
	.open		= tracing_open_generic,
	.read		= tracing_readme_read,
	.llseek		= generic_file_llseek,
};

static ssize_t
tracing_saved_cmdlines_read(struct file *file, char __user *ubuf,
				size_t cnt, loff_t *ppos)
{
	char *buf_comm;
	char *file_buf;
	char *buf;
	int len = 0;
	int pid;
	int i;

	file_buf = kmalloc(SAVED_CMDLINES*(16+TASK_COMM_LEN), GFP_KERNEL);
	if (!file_buf)
		return -ENOMEM;

	buf_comm = kmalloc(TASK_COMM_LEN, GFP_KERNEL);
	if (!buf_comm) {
		kfree(file_buf);
		return -ENOMEM;
	}

	buf = file_buf;

	for (i = 0; i < SAVED_CMDLINES; i++) {
		int r;

		pid = map_cmdline_to_pid[i];
		if (pid == -1 || pid == NO_CMDLINE_MAP)
			continue;

		trace_find_cmdline(pid, buf_comm);
		r = sprintf(buf, "%d %s\n", pid, buf_comm);
		buf += r;
		len += r;
	}

	len = simple_read_from_buffer(ubuf, cnt, ppos,
				      file_buf, len);

	kfree(file_buf);
	kfree(buf_comm);

	return len;
}

static const struct file_operations tracing_saved_cmdlines_fops = {
	.open	= tracing_open_generic,
	.read	= tracing_saved_cmdlines_read,
	.llseek	= generic_file_llseek,
};

static ssize_t
tracing_saved_tgids_read(struct file *file, char __user *ubuf,
				size_t cnt, loff_t *ppos)
{
	char *file_buf;
	char *buf;
	int len = 0;
	int pid;
	int i;

	file_buf = kmalloc(SAVED_CMDLINES*(16+1+16), GFP_KERNEL);
	if (!file_buf)
		return -ENOMEM;

	buf = file_buf;

	for (i = 0; i < SAVED_CMDLINES; i++) {
		int tgid;
		int r;

		pid = map_cmdline_to_pid[i];
		if (pid == -1 || pid == NO_CMDLINE_MAP)
			continue;

		tgid = trace_find_tgid(pid);
		r = sprintf(buf, "%d %d\n", pid, tgid);
		buf += r;
		len += r;
	}

	len = simple_read_from_buffer(ubuf, cnt, ppos,
				      file_buf, len);

	kfree(file_buf);

	return len;
}

static const struct file_operations tracing_saved_tgids_fops = {
	.open	= tracing_open_generic,
	.read	= tracing_saved_tgids_read,
	.llseek	= generic_file_llseek,
};

static ssize_t
tracing_set_trace_read(struct file *filp, char __user *ubuf,
		       size_t cnt, loff_t *ppos)
{
	struct trace_array *tr = filp->private_data;
	char buf[MAX_TRACER_SIZE+2];
	int r;

	mutex_lock(&trace_types_lock);
	r = sprintf(buf, "%s\n", tr->current_trace->name);
	mutex_unlock(&trace_types_lock);

	return simple_read_from_buffer(ubuf, cnt, ppos, buf, r);
}

int tracer_init(struct tracer *t, struct trace_array *tr)
{
	tracing_reset_online_cpus(&tr->trace_buffer);
	return t->init(tr);
}

static void set_buffer_entries(struct trace_buffer *buf, unsigned long val)
{
	int cpu;

	for_each_tracing_cpu(cpu)
		per_cpu_ptr(buf->data, cpu)->entries = val;
}

#ifdef CONFIG_TRACER_MAX_TRACE
static int resize_buffer_duplicate_size(struct trace_buffer *trace_buf,
					struct trace_buffer *size_buf, int cpu_id)
{
	int cpu, ret = 0;

	if (cpu_id == RING_BUFFER_ALL_CPUS) {
		for_each_tracing_cpu(cpu) {
			ret = ring_buffer_resize(trace_buf->buffer,
				 per_cpu_ptr(size_buf->data, cpu)->entries, cpu);
			if (ret < 0)
				break;
			per_cpu_ptr(trace_buf->data, cpu)->entries =
				per_cpu_ptr(size_buf->data, cpu)->entries;
		}
	} else {
		ret = ring_buffer_resize(trace_buf->buffer,
				 per_cpu_ptr(size_buf->data, cpu_id)->entries, cpu_id);
		if (ret == 0)
			per_cpu_ptr(trace_buf->data, cpu_id)->entries =
				per_cpu_ptr(size_buf->data, cpu_id)->entries;
	}

	return ret;
}
#endif 

static int __tracing_resize_ring_buffer(struct trace_array *tr,
					unsigned long size, int cpu)
{
	int ret;

	ring_buffer_expanded = true;

	
	if (!tr->trace_buffer.buffer)
		return 0;

	ret = ring_buffer_resize(tr->trace_buffer.buffer, size, cpu);
	if (ret < 0)
		return ret;

#ifdef CONFIG_TRACER_MAX_TRACE
	if (!(tr->flags & TRACE_ARRAY_FL_GLOBAL) ||
	    !tr->current_trace->use_max_tr)
		goto out;

	ret = ring_buffer_resize(tr->max_buffer.buffer, size, cpu);
	if (ret < 0) {
		int r = resize_buffer_duplicate_size(&tr->trace_buffer,
						     &tr->trace_buffer, cpu);
		if (r < 0) {
			WARN_ON(1);
			tracing_disabled = 1;
		}
		return ret;
	}

	if (cpu == RING_BUFFER_ALL_CPUS)
		set_buffer_entries(&tr->max_buffer, size);
	else
		per_cpu_ptr(tr->max_buffer.data, cpu)->entries = size;

 out:
#endif 

	if (cpu == RING_BUFFER_ALL_CPUS)
		set_buffer_entries(&tr->trace_buffer, size);
	else
		per_cpu_ptr(tr->trace_buffer.data, cpu)->entries = size;

	return ret;
}

static ssize_t tracing_resize_ring_buffer(struct trace_array *tr,
					  unsigned long size, int cpu_id)
{
	int ret = size;

	mutex_lock(&trace_types_lock);

	if (cpu_id != RING_BUFFER_ALL_CPUS) {
		
		if (!cpumask_test_cpu(cpu_id, tracing_buffer_mask)) {
			ret = -EINVAL;
			goto out;
		}
	}

	ret = __tracing_resize_ring_buffer(tr, size, cpu_id);
	if (ret < 0)
		ret = -ENOMEM;

out:
	mutex_unlock(&trace_types_lock);

	return ret;
}


int tracing_update_buffers(void)
{
	int ret = 0;

	mutex_lock(&trace_types_lock);
	if (!ring_buffer_expanded)
		ret = __tracing_resize_ring_buffer(&global_trace, trace_buf_size,
						RING_BUFFER_ALL_CPUS);
	mutex_unlock(&trace_types_lock);

	return ret;
}

struct trace_option_dentry;

static struct trace_option_dentry *
create_trace_option_files(struct trace_array *tr, struct tracer *tracer);

static void
destroy_trace_option_files(struct trace_option_dentry *topts);

static int tracing_set_tracer(const char *buf)
{
	static struct trace_option_dentry *topts;
	struct trace_array *tr = &global_trace;
	struct tracer *t;
#ifdef CONFIG_TRACER_MAX_TRACE
	bool had_max_tr;
#endif
	int ret = 0;

	mutex_lock(&trace_types_lock);

	if (!ring_buffer_expanded) {
		ret = __tracing_resize_ring_buffer(tr, trace_buf_size,
						RING_BUFFER_ALL_CPUS);
		if (ret < 0)
			goto out;
		ret = 0;
	}

	for (t = trace_types; t; t = t->next) {
		if (strcmp(t->name, buf) == 0)
			break;
	}
	if (!t) {
		ret = -EINVAL;
		goto out;
	}
	if (t == tr->current_trace)
		goto out;

	trace_branch_disable();

	tr->current_trace->enabled = false;

	if (tr->current_trace->reset)
		tr->current_trace->reset(tr);

	
	tr->current_trace = &nop_trace;

#ifdef CONFIG_TRACER_MAX_TRACE
	had_max_tr = tr->allocated_snapshot;

	if (had_max_tr && !t->use_max_tr) {
		synchronize_sched();
		free_snapshot(tr);
	}
#endif
	destroy_trace_option_files(topts);

	topts = create_trace_option_files(tr, t);

#ifdef CONFIG_TRACER_MAX_TRACE
	if (t->use_max_tr && !had_max_tr) {
		ret = alloc_snapshot(tr);
		if (ret < 0)
			goto out;
	}
#endif

	if (t->init) {
		ret = tracer_init(t, tr);
		if (ret)
			goto out;
	}

	tr->current_trace = t;
	tr->current_trace->enabled = true;
	trace_branch_enable(tr);
 out:
	mutex_unlock(&trace_types_lock);

	return ret;
}

static ssize_t
tracing_set_trace_write(struct file *filp, const char __user *ubuf,
			size_t cnt, loff_t *ppos)
{
	char buf[MAX_TRACER_SIZE+1];
	int i;
	size_t ret;
	int err;

	ret = cnt;

	if (cnt > MAX_TRACER_SIZE)
		cnt = MAX_TRACER_SIZE;

	if (copy_from_user(&buf, ubuf, cnt))
		return -EFAULT;

	buf[cnt] = 0;

	
	for (i = cnt - 1; i > 0 && isspace(buf[i]); i--)
		buf[i] = 0;

	err = tracing_set_tracer(buf);
	if (err)
		return err;

	*ppos += ret;

	return ret;
}

static ssize_t
tracing_max_lat_read(struct file *filp, char __user *ubuf,
		     size_t cnt, loff_t *ppos)
{
	unsigned long *ptr = filp->private_data;
	char buf[64];
	int r;

	r = snprintf(buf, sizeof(buf), "%ld\n",
		     *ptr == (unsigned long)-1 ? -1 : nsecs_to_usecs(*ptr));
	if (r > sizeof(buf))
		r = sizeof(buf);
	return simple_read_from_buffer(ubuf, cnt, ppos, buf, r);
}

static ssize_t
tracing_max_lat_write(struct file *filp, const char __user *ubuf,
		      size_t cnt, loff_t *ppos)
{
	unsigned long *ptr = filp->private_data;
	unsigned long val;
	int ret;

	ret = kstrtoul_from_user(ubuf, cnt, 10, &val);
	if (ret)
		return ret;

	*ptr = val * 1000;

	return cnt;
}

static int tracing_open_pipe(struct inode *inode, struct file *filp)
{
	struct trace_array *tr = inode->i_private;
	struct trace_iterator *iter;
	int ret = 0;

	if (tracing_disabled)
		return -ENODEV;

	if (trace_array_get(tr) < 0)
		return -ENODEV;

	mutex_lock(&trace_types_lock);

	
	iter = kzalloc(sizeof(*iter), GFP_KERNEL);
	if (!iter) {
		ret = -ENOMEM;
		__trace_array_put(tr);
		goto out;
	}

	iter->trace = kmalloc(sizeof(*iter->trace), GFP_KERNEL);
	if (!iter->trace) {
		ret = -ENOMEM;
		goto fail;
	}
	*iter->trace = *tr->current_trace;

	if (!alloc_cpumask_var(&iter->started, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto fail;
	}

	
	cpumask_setall(iter->started);

	if (trace_flags & TRACE_ITER_LATENCY_FMT)
		iter->iter_flags |= TRACE_FILE_LAT_FMT;

	
	if (trace_clocks[tr->clock_id].in_ns)
		iter->iter_flags |= TRACE_FILE_TIME_IN_NS;

	iter->tr = tr;
	iter->trace_buffer = &tr->trace_buffer;
	iter->cpu_file = tracing_get_cpu(inode);
	mutex_init(&iter->mutex);
	filp->private_data = iter;

	if (iter->trace->pipe_open)
		iter->trace->pipe_open(iter);

	nonseekable_open(inode, filp);
out:
	mutex_unlock(&trace_types_lock);
	return ret;

fail:
	kfree(iter->trace);
	kfree(iter);
	__trace_array_put(tr);
	mutex_unlock(&trace_types_lock);
	return ret;
}

static int tracing_release_pipe(struct inode *inode, struct file *file)
{
	struct trace_iterator *iter = file->private_data;
	struct trace_array *tr = inode->i_private;

	mutex_lock(&trace_types_lock);

	if (iter->trace->pipe_close)
		iter->trace->pipe_close(iter);

	mutex_unlock(&trace_types_lock);

	free_cpumask_var(iter->started);
	mutex_destroy(&iter->mutex);
	kfree(iter->trace);
	kfree(iter);

	trace_array_put(tr);

	return 0;
}

static unsigned int
trace_poll(struct trace_iterator *iter, struct file *filp, poll_table *poll_table)
{
	
	if (trace_buffer_iter(iter, iter->cpu_file))
		return POLLIN | POLLRDNORM;

	if (trace_flags & TRACE_ITER_BLOCK)
		return POLLIN | POLLRDNORM;
	else
		return ring_buffer_poll_wait(iter->trace_buffer->buffer, iter->cpu_file,
					     filp, poll_table);
}

static unsigned int
tracing_poll_pipe(struct file *filp, poll_table *poll_table)
{
	struct trace_iterator *iter = filp->private_data;

	return trace_poll(iter, filp, poll_table);
}

void poll_wait_pipe(struct trace_iterator *iter)
{
	set_current_state(TASK_INTERRUPTIBLE);
	
	schedule_timeout(HZ / 10);
}

static int tracing_wait_pipe(struct file *filp)
{
	struct trace_iterator *iter = filp->private_data;

	while (trace_empty(iter)) {

		if ((filp->f_flags & O_NONBLOCK)) {
			return -EAGAIN;
		}

		mutex_unlock(&iter->mutex);

		iter->trace->wait_pipe(iter);

		mutex_lock(&iter->mutex);

		if (signal_pending(current))
			return -EINTR;

		if (!tracing_is_on() && iter->pos)
			break;
	}

	return 1;
}

static ssize_t
tracing_read_pipe(struct file *filp, char __user *ubuf,
		  size_t cnt, loff_t *ppos)
{
	struct trace_iterator *iter = filp->private_data;
	struct trace_array *tr = iter->tr;
	ssize_t sret;

	
	sret = trace_seq_to_user(&iter->seq, ubuf, cnt);
	if (sret != -EBUSY)
		return sret;

	trace_seq_init(&iter->seq);

	
	mutex_lock(&trace_types_lock);
	if (unlikely(iter->trace->name != tr->current_trace->name))
		*iter->trace = *tr->current_trace;
	mutex_unlock(&trace_types_lock);

	mutex_lock(&iter->mutex);
	if (iter->trace->read) {
		sret = iter->trace->read(iter, filp, ubuf, cnt, ppos);
		if (sret)
			goto out;
	}

waitagain:
	sret = tracing_wait_pipe(filp);
	if (sret <= 0)
		goto out;

	
	if (trace_empty(iter)) {
		sret = 0;
		goto out;
	}

	if (cnt >= PAGE_SIZE)
		cnt = PAGE_SIZE - 1;

	
	memset(&iter->seq, 0,
	       sizeof(struct trace_iterator) -
	       offsetof(struct trace_iterator, seq));
	cpumask_clear(iter->started);
	iter->pos = -1;

	trace_event_read_lock();
	trace_access_lock(iter->cpu_file);
	while (trace_find_next_entry_inc(iter) != NULL) {
		enum print_line_t ret;
		int len = iter->seq.len;

		ret = print_trace_line(iter);
		if (ret == TRACE_TYPE_PARTIAL_LINE) {
			
			iter->seq.len = len;
			break;
		}
		if (ret != TRACE_TYPE_NO_CONSUME)
			trace_consume(iter);

		if (iter->seq.len >= cnt)
			break;

		WARN_ONCE(iter->seq.full, "full flag set for trace type %d",
			  iter->ent->type);
	}
	trace_access_unlock(iter->cpu_file);
	trace_event_read_unlock();

	
	sret = trace_seq_to_user(&iter->seq, ubuf, cnt);
	if (iter->seq.readpos >= iter->seq.len)
		trace_seq_init(&iter->seq);

	if (sret == -EBUSY)
		goto waitagain;

out:
	mutex_unlock(&iter->mutex);

	return sret;
}

static void tracing_pipe_buf_release(struct pipe_inode_info *pipe,
				     struct pipe_buffer *buf)
{
	__free_page(buf->page);
}

static void tracing_spd_release_pipe(struct splice_pipe_desc *spd,
				     unsigned int idx)
{
	__free_page(spd->pages[idx]);
}

static const struct pipe_buf_operations tracing_pipe_buf_ops = {
	.can_merge		= 0,
	.map			= generic_pipe_buf_map,
	.unmap			= generic_pipe_buf_unmap,
	.confirm		= generic_pipe_buf_confirm,
	.release		= tracing_pipe_buf_release,
	.steal			= generic_pipe_buf_steal,
	.get			= generic_pipe_buf_get,
};

static size_t
tracing_fill_pipe_page(size_t rem, struct trace_iterator *iter)
{
	size_t count;
	int ret;

	
	for (;;) {
		count = iter->seq.len;
		ret = print_trace_line(iter);
		count = iter->seq.len - count;
		if (rem < count) {
			rem = 0;
			iter->seq.len -= count;
			break;
		}
		if (ret == TRACE_TYPE_PARTIAL_LINE) {
			iter->seq.len -= count;
			break;
		}

		if (ret != TRACE_TYPE_NO_CONSUME)
			trace_consume(iter);
		rem -= count;
		if (!trace_find_next_entry_inc(iter))	{
			rem = 0;
			iter->ent = NULL;
			break;
		}
	}

	return rem;
}

static ssize_t tracing_splice_read_pipe(struct file *filp,
					loff_t *ppos,
					struct pipe_inode_info *pipe,
					size_t len,
					unsigned int flags)
{
	struct page *pages_def[PIPE_DEF_BUFFERS];
	struct partial_page partial_def[PIPE_DEF_BUFFERS];
	struct trace_iterator *iter = filp->private_data;
	struct splice_pipe_desc spd = {
		.pages		= pages_def,
		.partial	= partial_def,
		.nr_pages	= 0, 
		.nr_pages_max	= PIPE_DEF_BUFFERS,
		.flags		= flags,
		.ops		= &tracing_pipe_buf_ops,
		.spd_release	= tracing_spd_release_pipe,
	};
	struct trace_array *tr = iter->tr;
	ssize_t ret;
	size_t rem;
	unsigned int i;

	if (splice_grow_spd(pipe, &spd))
		return -ENOMEM;

	
	mutex_lock(&trace_types_lock);
	if (unlikely(iter->trace->name != tr->current_trace->name))
		*iter->trace = *tr->current_trace;
	mutex_unlock(&trace_types_lock);

	mutex_lock(&iter->mutex);

	if (iter->trace->splice_read) {
		ret = iter->trace->splice_read(iter, filp,
					       ppos, pipe, len, flags);
		if (ret)
			goto out_err;
	}

	ret = tracing_wait_pipe(filp);
	if (ret <= 0)
		goto out_err;

	if (!iter->ent && !trace_find_next_entry_inc(iter)) {
		ret = -EFAULT;
		goto out_err;
	}

	trace_event_read_lock();
	trace_access_lock(iter->cpu_file);

	
	for (i = 0, rem = len; i < pipe->buffers && rem; i++) {
		spd.pages[i] = alloc_page(GFP_KERNEL);
		if (!spd.pages[i])
			break;

		rem = tracing_fill_pipe_page(rem, iter);

		
		ret = trace_seq_to_buffer(&iter->seq,
					  page_address(spd.pages[i]),
					  iter->seq.len);
		if (ret < 0) {
			__free_page(spd.pages[i]);
			break;
		}
		spd.partial[i].offset = 0;
		spd.partial[i].len = iter->seq.len;

		trace_seq_init(&iter->seq);
	}

	trace_access_unlock(iter->cpu_file);
	trace_event_read_unlock();
	mutex_unlock(&iter->mutex);

	spd.nr_pages = i;

	ret = splice_to_pipe(pipe, &spd);
out:
	splice_shrink_spd(&spd);
	return ret;

out_err:
	mutex_unlock(&iter->mutex);
	goto out;
}

static ssize_t
tracing_entries_read(struct file *filp, char __user *ubuf,
		     size_t cnt, loff_t *ppos)
{
	struct inode *inode = file_inode(filp);
	struct trace_array *tr = inode->i_private;
	int cpu = tracing_get_cpu(inode);
	char buf[64];
	int r = 0;
	ssize_t ret;

	mutex_lock(&trace_types_lock);

	if (cpu == RING_BUFFER_ALL_CPUS) {
		int cpu, buf_size_same;
		unsigned long size;

		size = 0;
		buf_size_same = 1;
		
		for_each_tracing_cpu(cpu) {
			
			if (size == 0)
				size = per_cpu_ptr(tr->trace_buffer.data, cpu)->entries;
			if (size != per_cpu_ptr(tr->trace_buffer.data, cpu)->entries) {
				buf_size_same = 0;
				break;
			}
		}

		if (buf_size_same) {
			if (!ring_buffer_expanded)
				r = sprintf(buf, "%lu (expanded: %lu)\n",
					    size >> 10,
					    trace_buf_size >> 10);
			else
				r = sprintf(buf, "%lu\n", size >> 10);
		} else
			r = sprintf(buf, "X\n");
	} else
		r = sprintf(buf, "%lu\n", per_cpu_ptr(tr->trace_buffer.data, cpu)->entries >> 10);

	mutex_unlock(&trace_types_lock);

	ret = simple_read_from_buffer(ubuf, cnt, ppos, buf, r);
	return ret;
}

static ssize_t
tracing_entries_write(struct file *filp, const char __user *ubuf,
		      size_t cnt, loff_t *ppos)
{
	struct inode *inode = file_inode(filp);
	struct trace_array *tr = inode->i_private;
	unsigned long val;
	int ret;

	ret = kstrtoul_from_user(ubuf, cnt, 10, &val);
	if (ret)
		return ret;

	
	if (!val)
		return -EINVAL;

	
	val <<= 10;
	ret = tracing_resize_ring_buffer(tr, val, tracing_get_cpu(inode));
	if (ret < 0)
		return ret;

	*ppos += cnt;

	return cnt;
}

static ssize_t
tracing_total_entries_read(struct file *filp, char __user *ubuf,
				size_t cnt, loff_t *ppos)
{
	struct trace_array *tr = filp->private_data;
	char buf[64];
	int r, cpu;
	unsigned long size = 0, expanded_size = 0;

	mutex_lock(&trace_types_lock);
	for_each_tracing_cpu(cpu) {
		size += per_cpu_ptr(tr->trace_buffer.data, cpu)->entries >> 10;
		if (!ring_buffer_expanded)
			expanded_size += trace_buf_size >> 10;
	}
	if (ring_buffer_expanded)
		r = sprintf(buf, "%lu\n", size);
	else
		r = sprintf(buf, "%lu (expanded: %lu)\n", size, expanded_size);
	mutex_unlock(&trace_types_lock);

	return simple_read_from_buffer(ubuf, cnt, ppos, buf, r);
}

static ssize_t
tracing_free_buffer_write(struct file *filp, const char __user *ubuf,
			  size_t cnt, loff_t *ppos)
{
	/*
	 * There is no need to read what the user has written, this function
	 * is just to make sure that there is no error when "echo" is used
	 */

	*ppos += cnt;

	return cnt;
}

static int
tracing_free_buffer_release(struct inode *inode, struct file *filp)
{
	struct trace_array *tr = inode->i_private;

	
	if (trace_flags & TRACE_ITER_STOP_ON_FREE)
		tracer_tracing_off(tr);
	
	tracing_resize_ring_buffer(tr, 0, RING_BUFFER_ALL_CPUS);

	trace_array_put(tr);

	return 0;
}

static ssize_t
tracing_mark_write(struct file *filp, const char __user *ubuf,
					size_t cnt, loff_t *fpos)
{
	unsigned long addr = (unsigned long)ubuf;
	struct trace_array *tr = filp->private_data;
	struct ring_buffer_event *event;
	struct ring_buffer *buffer;
	struct print_entry *entry;
	unsigned long irq_flags;
	struct page *pages[2];
	void *map_page[2];
	int nr_pages = 1;
	ssize_t written;
	int offset;
	int size;
	int len;
	int ret;
	int i;

	if (tracing_disabled)
		return -EINVAL;

	if (!(trace_flags & TRACE_ITER_MARKERS))
		return -EINVAL;

	if (cnt > TRACE_BUF_SIZE)
		cnt = TRACE_BUF_SIZE;

	BUILD_BUG_ON(TRACE_BUF_SIZE >= PAGE_SIZE);

	
	if ((addr & PAGE_MASK) != ((addr + cnt) & PAGE_MASK))
		nr_pages = 2;

	offset = addr & (PAGE_SIZE - 1);
	addr &= PAGE_MASK;

	ret = get_user_pages_fast(addr, nr_pages, 0, pages);
	if (ret < nr_pages) {
		while (--ret >= 0)
			put_page(pages[ret]);
		written = -EFAULT;
		goto out;
	}

	for (i = 0; i < nr_pages; i++)
		map_page[i] = kmap_atomic(pages[i]);

	local_save_flags(irq_flags);
	size = sizeof(*entry) + cnt + 2; 
	buffer = tr->trace_buffer.buffer;
	event = trace_buffer_lock_reserve(buffer, TRACE_PRINT, size,
					  irq_flags, preempt_count());
	if (!event) {
		
		written = -EBADF;
		goto out_unlock;
	}

	entry = ring_buffer_event_data(event);
	entry->ip = _THIS_IP_;

	if (nr_pages == 2) {
		len = PAGE_SIZE - offset;
		memcpy(&entry->buf, map_page[0] + offset, len);
		memcpy(&entry->buf[len], map_page[1], cnt - len);
	} else
		memcpy(&entry->buf, map_page[0] + offset, cnt);

	if (entry->buf[cnt - 1] != '\n') {
		entry->buf[cnt] = '\n';
		entry->buf[cnt + 1] = '\0';
		stm_log(OST_ENTITY_TRACE_MARKER, entry->buf, cnt + 2);
	} else {
		entry->buf[cnt] = '\0';
		stm_log(OST_ENTITY_TRACE_MARKER, entry->buf, cnt + 1);
	}

	__buffer_unlock_commit(buffer, event);

	written = cnt;

	*fpos += written;

 out_unlock:
	for (i = 0; i < nr_pages; i++){
		kunmap_atomic(map_page[i]);
		put_page(pages[i]);
	}
 out:
	return written;
}

static int tracing_clock_show(struct seq_file *m, void *v)
{
	struct trace_array *tr = m->private;
	int i;

	for (i = 0; i < ARRAY_SIZE(trace_clocks); i++)
		seq_printf(m,
			"%s%s%s%s", i ? " " : "",
			i == tr->clock_id ? "[" : "", trace_clocks[i].name,
			i == tr->clock_id ? "]" : "");
	seq_putc(m, '\n');

	return 0;
}

static ssize_t tracing_clock_write(struct file *filp, const char __user *ubuf,
				   size_t cnt, loff_t *fpos)
{
	struct seq_file *m = filp->private_data;
	struct trace_array *tr = m->private;
	char buf[64];
	const char *clockstr;
	int i;

	if (cnt >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(&buf, ubuf, cnt))
		return -EFAULT;

	buf[cnt] = 0;

	clockstr = strstrip(buf);

	for (i = 0; i < ARRAY_SIZE(trace_clocks); i++) {
		if (strcmp(trace_clocks[i].name, clockstr) == 0)
			break;
	}
	if (i == ARRAY_SIZE(trace_clocks))
		return -EINVAL;

	mutex_lock(&trace_types_lock);

	tr->clock_id = i;

	ring_buffer_set_clock(tr->trace_buffer.buffer, trace_clocks[i].func);

	tracing_reset_online_cpus(&tr->trace_buffer);

#ifdef CONFIG_TRACER_MAX_TRACE
	if (tr->flags & TRACE_ARRAY_FL_GLOBAL && tr->max_buffer.buffer)
		ring_buffer_set_clock(tr->max_buffer.buffer, trace_clocks[i].func);
	tracing_reset_online_cpus(&tr->max_buffer);
#endif

	mutex_unlock(&trace_types_lock);

	*fpos += cnt;

	return cnt;
}

static int tracing_clock_open(struct inode *inode, struct file *file)
{
	struct trace_array *tr = inode->i_private;
	int ret;

	if (tracing_disabled)
		return -ENODEV;

	if (trace_array_get(tr))
		return -ENODEV;

	ret = single_open(file, tracing_clock_show, inode->i_private);
	if (ret < 0)
		trace_array_put(tr);

	return ret;
}

struct ftrace_buffer_info {
	struct trace_iterator	iter;
	void			*spare;
	unsigned int		read;
};

#ifdef CONFIG_TRACER_SNAPSHOT
static int tracing_snapshot_open(struct inode *inode, struct file *file)
{
	struct trace_array *tr = inode->i_private;
	struct trace_iterator *iter;
	struct seq_file *m;
	int ret = 0;

	if (trace_array_get(tr) < 0)
		return -ENODEV;

	if (file->f_mode & FMODE_READ) {
		iter = __tracing_open(inode, file, true);
		if (IS_ERR(iter))
			ret = PTR_ERR(iter);
	} else {
		
		ret = -ENOMEM;
		m = kzalloc(sizeof(*m), GFP_KERNEL);
		if (!m)
			goto out;
		iter = kzalloc(sizeof(*iter), GFP_KERNEL);
		if (!iter) {
			kfree(m);
			goto out;
		}
		ret = 0;

		iter->tr = tr;
		iter->trace_buffer = &tr->max_buffer;
		iter->cpu_file = tracing_get_cpu(inode);
		m->private = iter;
		file->private_data = m;
	}
out:
	if (ret < 0)
		trace_array_put(tr);

	return ret;
}

static ssize_t
tracing_snapshot_write(struct file *filp, const char __user *ubuf, size_t cnt,
		       loff_t *ppos)
{
	struct seq_file *m = filp->private_data;
	struct trace_iterator *iter = m->private;
	struct trace_array *tr = iter->tr;
	unsigned long val;
	int ret;

	ret = tracing_update_buffers();
	if (ret < 0)
		return ret;

	ret = kstrtoul_from_user(ubuf, cnt, 10, &val);
	if (ret)
		return ret;

	mutex_lock(&trace_types_lock);

	if (tr->current_trace->use_max_tr) {
		ret = -EBUSY;
		goto out;
	}

	switch (val) {
	case 0:
		if (iter->cpu_file != RING_BUFFER_ALL_CPUS) {
			ret = -EINVAL;
			break;
		}
		if (tr->allocated_snapshot)
			free_snapshot(tr);
		break;
	case 1:
#ifndef CONFIG_RING_BUFFER_ALLOW_SWAP
		if (iter->cpu_file != RING_BUFFER_ALL_CPUS) {
			ret = -EINVAL;
			break;
		}
#endif
		if (!tr->allocated_snapshot) {
			ret = alloc_snapshot(tr);
			if (ret < 0)
				break;
		}
		local_irq_disable();
		
		if (iter->cpu_file == RING_BUFFER_ALL_CPUS)
			update_max_tr(tr, current, smp_processor_id());
		else
			update_max_tr_single(tr, current, iter->cpu_file);
		local_irq_enable();
		break;
	default:
		if (tr->allocated_snapshot) {
			if (iter->cpu_file == RING_BUFFER_ALL_CPUS)
				tracing_reset_online_cpus(&tr->max_buffer);
			else
				tracing_reset(&tr->max_buffer, iter->cpu_file);
		}
		break;
	}

	if (ret >= 0) {
		*ppos += cnt;
		ret = cnt;
	}
out:
	mutex_unlock(&trace_types_lock);
	return ret;
}

static int tracing_snapshot_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;
	int ret;

	ret = tracing_release(inode, file);

	if (file->f_mode & FMODE_READ)
		return ret;

	
	if (m)
		kfree(m->private);
	kfree(m);

	return 0;
}

static int tracing_buffers_open(struct inode *inode, struct file *filp);
static ssize_t tracing_buffers_read(struct file *filp, char __user *ubuf,
				    size_t count, loff_t *ppos);
static int tracing_buffers_release(struct inode *inode, struct file *file);
static ssize_t tracing_buffers_splice_read(struct file *file, loff_t *ppos,
		   struct pipe_inode_info *pipe, size_t len, unsigned int flags);

static int snapshot_raw_open(struct inode *inode, struct file *filp)
{
	struct ftrace_buffer_info *info;
	int ret;

	ret = tracing_buffers_open(inode, filp);
	if (ret < 0)
		return ret;

	info = filp->private_data;

	if (info->iter.trace->use_max_tr) {
		tracing_buffers_release(inode, filp);
		return -EBUSY;
	}

	info->iter.snapshot = true;
	info->iter.trace_buffer = &info->iter.tr->max_buffer;

	return ret;
}

#endif 


static const struct file_operations tracing_max_lat_fops = {
	.open		= tracing_open_generic,
	.read		= tracing_max_lat_read,
	.write		= tracing_max_lat_write,
	.llseek		= generic_file_llseek,
};

static const struct file_operations set_tracer_fops = {
	.open		= tracing_open_generic,
	.read		= tracing_set_trace_read,
	.write		= tracing_set_trace_write,
	.llseek		= generic_file_llseek,
};

static const struct file_operations tracing_pipe_fops = {
	.open		= tracing_open_pipe,
	.poll		= tracing_poll_pipe,
	.read		= tracing_read_pipe,
	.splice_read	= tracing_splice_read_pipe,
	.release	= tracing_release_pipe,
	.llseek		= no_llseek,
};

static const struct file_operations tracing_entries_fops = {
	.open		= tracing_open_generic_tr,
	.read		= tracing_entries_read,
	.write		= tracing_entries_write,
	.llseek		= generic_file_llseek,
	.release	= tracing_release_generic_tr,
};

static const struct file_operations tracing_total_entries_fops = {
	.open		= tracing_open_generic_tr,
	.read		= tracing_total_entries_read,
	.llseek		= generic_file_llseek,
	.release	= tracing_release_generic_tr,
};

static const struct file_operations tracing_free_buffer_fops = {
	.open		= tracing_open_generic_tr,
	.write		= tracing_free_buffer_write,
	.release	= tracing_free_buffer_release,
};

static const struct file_operations tracing_mark_fops = {
	.open		= tracing_open_generic_tr,
	.write		= tracing_mark_write,
	.llseek		= generic_file_llseek,
	.release	= tracing_release_generic_tr,
};

static const struct file_operations trace_clock_fops = {
	.open		= tracing_clock_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= tracing_single_release_tr,
	.write		= tracing_clock_write,
};

#ifdef CONFIG_TRACER_SNAPSHOT
static const struct file_operations snapshot_fops = {
	.open		= tracing_snapshot_open,
	.read		= seq_read,
	.write		= tracing_snapshot_write,
	.llseek		= tracing_seek,
	.release	= tracing_snapshot_release,
};

static const struct file_operations snapshot_raw_fops = {
	.open		= snapshot_raw_open,
	.read		= tracing_buffers_read,
	.release	= tracing_buffers_release,
	.splice_read	= tracing_buffers_splice_read,
	.llseek		= no_llseek,
};

#endif 

static int tracing_buffers_open(struct inode *inode, struct file *filp)
{
	struct trace_array *tr = inode->i_private;
	struct ftrace_buffer_info *info;
	int ret;

	if (tracing_disabled)
		return -ENODEV;

	if (trace_array_get(tr) < 0)
		return -ENODEV;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info) {
		trace_array_put(tr);
		return -ENOMEM;
	}

	mutex_lock(&trace_types_lock);

	info->iter.tr		= tr;
	info->iter.cpu_file	= tracing_get_cpu(inode);
	info->iter.trace	= tr->current_trace;
	info->iter.trace_buffer = &tr->trace_buffer;
	info->spare		= NULL;
	
	info->read		= (unsigned int)-1;

	filp->private_data = info;

	mutex_unlock(&trace_types_lock);

	ret = nonseekable_open(inode, filp);
	if (ret < 0)
		trace_array_put(tr);

	return ret;
}

static unsigned int
tracing_buffers_poll(struct file *filp, poll_table *poll_table)
{
	struct ftrace_buffer_info *info = filp->private_data;
	struct trace_iterator *iter = &info->iter;

	return trace_poll(iter, filp, poll_table);
}

static ssize_t
tracing_buffers_read(struct file *filp, char __user *ubuf,
		     size_t count, loff_t *ppos)
{
	struct ftrace_buffer_info *info = filp->private_data;
	struct trace_iterator *iter = &info->iter;
	ssize_t ret;
	ssize_t size;

	if (!count)
		return 0;

	mutex_lock(&trace_types_lock);

#ifdef CONFIG_TRACER_MAX_TRACE
	if (iter->snapshot && iter->tr->current_trace->use_max_tr) {
		size = -EBUSY;
		goto out_unlock;
	}
#endif

	if (!info->spare)
		info->spare = ring_buffer_alloc_read_page(iter->trace_buffer->buffer,
							  iter->cpu_file);
	size = -ENOMEM;
	if (!info->spare)
		goto out_unlock;

	
	if (info->read < PAGE_SIZE)
		goto read;

 again:
	trace_access_lock(iter->cpu_file);
	ret = ring_buffer_read_page(iter->trace_buffer->buffer,
				    &info->spare,
				    count,
				    iter->cpu_file, 0);
	trace_access_unlock(iter->cpu_file);

	if (ret < 0) {
		if (trace_empty(iter)) {
			if ((filp->f_flags & O_NONBLOCK)) {
				size = -EAGAIN;
				goto out_unlock;
			}
			mutex_unlock(&trace_types_lock);
			iter->trace->wait_pipe(iter);
			mutex_lock(&trace_types_lock);
			if (signal_pending(current)) {
				size = -EINTR;
				goto out_unlock;
			}
			goto again;
		}
		size = 0;
		goto out_unlock;
	}

	info->read = 0;
 read:
	size = PAGE_SIZE - info->read;
	if (size > count)
		size = count;

	ret = copy_to_user(ubuf, info->spare + info->read, size);
	if (ret == size) {
		size = -EFAULT;
		goto out_unlock;
	}
	size -= ret;

	*ppos += size;
	info->read += size;

 out_unlock:
	mutex_unlock(&trace_types_lock);

	return size;
}

static int tracing_buffers_release(struct inode *inode, struct file *file)
{
	struct ftrace_buffer_info *info = file->private_data;
	struct trace_iterator *iter = &info->iter;

	mutex_lock(&trace_types_lock);

	__trace_array_put(iter->tr);

	if (info->spare)
		ring_buffer_free_read_page(iter->trace_buffer->buffer, info->spare);
	kfree(info);

	mutex_unlock(&trace_types_lock);

	return 0;
}

struct buffer_ref {
	struct ring_buffer	*buffer;
	void			*page;
	int			ref;
};

static void buffer_pipe_buf_release(struct pipe_inode_info *pipe,
				    struct pipe_buffer *buf)
{
	struct buffer_ref *ref = (struct buffer_ref *)buf->private;

	if (--ref->ref)
		return;

	ring_buffer_free_read_page(ref->buffer, ref->page);
	kfree(ref);
	buf->private = 0;
}

static void buffer_pipe_buf_get(struct pipe_inode_info *pipe,
				struct pipe_buffer *buf)
{
	struct buffer_ref *ref = (struct buffer_ref *)buf->private;

	ref->ref++;
}

static const struct pipe_buf_operations buffer_pipe_buf_ops = {
	.can_merge		= 0,
	.map			= generic_pipe_buf_map,
	.unmap			= generic_pipe_buf_unmap,
	.confirm		= generic_pipe_buf_confirm,
	.release		= buffer_pipe_buf_release,
	.steal			= generic_pipe_buf_steal,
	.get			= buffer_pipe_buf_get,
};

static void buffer_spd_release(struct splice_pipe_desc *spd, unsigned int i)
{
	struct buffer_ref *ref =
		(struct buffer_ref *)spd->partial[i].private;

	if (--ref->ref)
		return;

	ring_buffer_free_read_page(ref->buffer, ref->page);
	kfree(ref);
	spd->partial[i].private = 0;
}

static ssize_t
tracing_buffers_splice_read(struct file *file, loff_t *ppos,
			    struct pipe_inode_info *pipe, size_t len,
			    unsigned int flags)
{
	struct ftrace_buffer_info *info = file->private_data;
	struct trace_iterator *iter = &info->iter;
	struct partial_page partial_def[PIPE_DEF_BUFFERS];
	struct page *pages_def[PIPE_DEF_BUFFERS];
	struct splice_pipe_desc spd = {
		.pages		= pages_def,
		.partial	= partial_def,
		.nr_pages_max	= PIPE_DEF_BUFFERS,
		.flags		= flags,
		.ops		= &buffer_pipe_buf_ops,
		.spd_release	= buffer_spd_release,
	};
	struct buffer_ref *ref;
	int entries, size, i;
	ssize_t ret;

	mutex_lock(&trace_types_lock);

#ifdef CONFIG_TRACER_MAX_TRACE
	if (iter->snapshot && iter->tr->current_trace->use_max_tr) {
		ret = -EBUSY;
		goto out;
	}
#endif

	if (splice_grow_spd(pipe, &spd)) {
		ret = -ENOMEM;
		goto out;
	}

	if (*ppos & (PAGE_SIZE - 1)) {
		ret = -EINVAL;
		goto out;
	}

	if (len & (PAGE_SIZE - 1)) {
		if (len < PAGE_SIZE) {
			ret = -EINVAL;
			goto out;
		}
		len &= PAGE_MASK;
	}

 again:
	trace_access_lock(iter->cpu_file);
	entries = ring_buffer_entries_cpu(iter->trace_buffer->buffer, iter->cpu_file);

	for (i = 0; i < pipe->buffers && len && entries; i++, len -= PAGE_SIZE) {
		struct page *page;
		int r;

		ref = kzalloc(sizeof(*ref), GFP_KERNEL);
		if (!ref)
			break;

		ref->ref = 1;
		ref->buffer = iter->trace_buffer->buffer;
		ref->page = ring_buffer_alloc_read_page(ref->buffer, iter->cpu_file);
		if (!ref->page) {
			kfree(ref);
			break;
		}

		r = ring_buffer_read_page(ref->buffer, &ref->page,
					  len, iter->cpu_file, 1);
		if (r < 0) {
			ring_buffer_free_read_page(ref->buffer, ref->page);
			kfree(ref);
			break;
		}

		size = ring_buffer_page_len(ref->page);
		if (size < PAGE_SIZE)
			memset(ref->page + size, 0, PAGE_SIZE - size);

		page = virt_to_page(ref->page);

		spd.pages[i] = page;
		spd.partial[i].len = PAGE_SIZE;
		spd.partial[i].offset = 0;
		spd.partial[i].private = (unsigned long)ref;
		spd.nr_pages++;
		*ppos += PAGE_SIZE;

		entries = ring_buffer_entries_cpu(iter->trace_buffer->buffer, iter->cpu_file);
	}

	trace_access_unlock(iter->cpu_file);
	spd.nr_pages = i;

	
	if (!spd.nr_pages) {
		if ((file->f_flags & O_NONBLOCK) || (flags & SPLICE_F_NONBLOCK)) {
			ret = -EAGAIN;
			goto out;
		}
		mutex_unlock(&trace_types_lock);
		iter->trace->wait_pipe(iter);
		mutex_lock(&trace_types_lock);
		if (signal_pending(current)) {
			ret = -EINTR;
			goto out;
		}
		goto again;
	}

	ret = splice_to_pipe(pipe, &spd);
	splice_shrink_spd(&spd);
out:
	mutex_unlock(&trace_types_lock);

	return ret;
}

static const struct file_operations tracing_buffers_fops = {
	.open		= tracing_buffers_open,
	.read		= tracing_buffers_read,
	.poll		= tracing_buffers_poll,
	.release	= tracing_buffers_release,
	.splice_read	= tracing_buffers_splice_read,
	.llseek		= no_llseek,
};

static ssize_t
tracing_stats_read(struct file *filp, char __user *ubuf,
		   size_t count, loff_t *ppos)
{
	struct inode *inode = file_inode(filp);
	struct trace_array *tr = inode->i_private;
	struct trace_buffer *trace_buf = &tr->trace_buffer;
	int cpu = tracing_get_cpu(inode);
	struct trace_seq *s;
	unsigned long cnt;
	unsigned long long t;
	unsigned long usec_rem;

	s = kmalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	trace_seq_init(s);

	cnt = ring_buffer_entries_cpu(trace_buf->buffer, cpu);
	trace_seq_printf(s, "entries: %ld\n", cnt);

	cnt = ring_buffer_overrun_cpu(trace_buf->buffer, cpu);
	trace_seq_printf(s, "overrun: %ld\n", cnt);

	cnt = ring_buffer_commit_overrun_cpu(trace_buf->buffer, cpu);
	trace_seq_printf(s, "commit overrun: %ld\n", cnt);

	cnt = ring_buffer_bytes_cpu(trace_buf->buffer, cpu);
	trace_seq_printf(s, "bytes: %ld\n", cnt);

	if (trace_clocks[tr->clock_id].in_ns) {
		
		t = ns2usecs(ring_buffer_oldest_event_ts(trace_buf->buffer, cpu));
		usec_rem = do_div(t, USEC_PER_SEC);
		trace_seq_printf(s, "oldest event ts: %5llu.%06lu\n",
								t, usec_rem);

		t = ns2usecs(ring_buffer_time_stamp(trace_buf->buffer, cpu));
		usec_rem = do_div(t, USEC_PER_SEC);
		trace_seq_printf(s, "now ts: %5llu.%06lu\n", t, usec_rem);
	} else {
		
		trace_seq_printf(s, "oldest event ts: %llu\n",
				ring_buffer_oldest_event_ts(trace_buf->buffer, cpu));

		trace_seq_printf(s, "now ts: %llu\n",
				ring_buffer_time_stamp(trace_buf->buffer, cpu));
	}

	cnt = ring_buffer_dropped_events_cpu(trace_buf->buffer, cpu);
	trace_seq_printf(s, "dropped events: %ld\n", cnt);

	cnt = ring_buffer_read_events_cpu(trace_buf->buffer, cpu);
	trace_seq_printf(s, "read events: %ld\n", cnt);

	count = simple_read_from_buffer(ubuf, count, ppos, s->buffer, s->len);

	kfree(s);

	return count;
}

static const struct file_operations tracing_stats_fops = {
	.open		= tracing_open_generic_tr,
	.read		= tracing_stats_read,
	.llseek		= generic_file_llseek,
	.release	= tracing_release_generic_tr,
};

#ifdef CONFIG_DYNAMIC_FTRACE

int __weak ftrace_arch_read_dyn_info(char *buf, int size)
{
	return 0;
}

static ssize_t
tracing_read_dyn_info(struct file *filp, char __user *ubuf,
		  size_t cnt, loff_t *ppos)
{
	static char ftrace_dyn_info_buffer[1024];
	static DEFINE_MUTEX(dyn_info_mutex);
	unsigned long *p = filp->private_data;
	char *buf = ftrace_dyn_info_buffer;
	int size = ARRAY_SIZE(ftrace_dyn_info_buffer);
	int r;

	mutex_lock(&dyn_info_mutex);
	r = sprintf(buf, "%ld ", *p);

	r += ftrace_arch_read_dyn_info(buf+r, (size-1)-r);
	buf[r++] = '\n';

	r = simple_read_from_buffer(ubuf, cnt, ppos, buf, r);

	mutex_unlock(&dyn_info_mutex);

	return r;
}

static const struct file_operations tracing_dyn_info_fops = {
	.open		= tracing_open_generic,
	.read		= tracing_read_dyn_info,
	.llseek		= generic_file_llseek,
};
#endif 

#if defined(CONFIG_TRACER_SNAPSHOT) && defined(CONFIG_DYNAMIC_FTRACE)
static void
ftrace_snapshot(unsigned long ip, unsigned long parent_ip, void **data)
{
	tracing_snapshot();
}

static void
ftrace_count_snapshot(unsigned long ip, unsigned long parent_ip, void **data)
{
	unsigned long *count = (long *)data;

	if (!*count)
		return;

	if (*count != -1)
		(*count)--;

	tracing_snapshot();
}

static int
ftrace_snapshot_print(struct seq_file *m, unsigned long ip,
		      struct ftrace_probe_ops *ops, void *data)
{
	long count = (long)data;

	seq_printf(m, "%ps:", (void *)ip);

	seq_printf(m, "snapshot");

	if (count == -1)
		seq_printf(m, ":unlimited\n");
	else
		seq_printf(m, ":count=%ld\n", count);

	return 0;
}

static struct ftrace_probe_ops snapshot_probe_ops = {
	.func			= ftrace_snapshot,
	.print			= ftrace_snapshot_print,
};

static struct ftrace_probe_ops snapshot_count_probe_ops = {
	.func			= ftrace_count_snapshot,
	.print			= ftrace_snapshot_print,
};

static int
ftrace_trace_snapshot_callback(struct ftrace_hash *hash,
			       char *glob, char *cmd, char *param, int enable)
{
	struct ftrace_probe_ops *ops;
	void *count = (void *)-1;
	char *number;
	int ret;

	
	if (!enable)
		return -EINVAL;

	ops = param ? &snapshot_count_probe_ops :  &snapshot_probe_ops;

	if (glob[0] == '!') {
		unregister_ftrace_function_probe_func(glob+1, ops);
		return 0;
	}

	if (!param)
		goto out_reg;

	number = strsep(&param, ":");

	if (!strlen(number))
		goto out_reg;

	ret = kstrtoul(number, 0, (unsigned long *)&count);
	if (ret)
		return ret;

 out_reg:
	ret = register_ftrace_function_probe(glob, ops, count);

	if (ret >= 0)
		alloc_snapshot(&global_trace);

	return ret < 0 ? ret : 0;
}

static struct ftrace_func_command ftrace_snapshot_cmd = {
	.name			= "snapshot",
	.func			= ftrace_trace_snapshot_callback,
};

static int register_snapshot_cmd(void)
{
	return register_ftrace_command(&ftrace_snapshot_cmd);
}
#else
static inline int register_snapshot_cmd(void) { return 0; }
#endif 

struct dentry *tracing_init_dentry_tr(struct trace_array *tr)
{
	if (tr->dir)
		return tr->dir;

	if (!debugfs_initialized())
		return NULL;

	if (tr->flags & TRACE_ARRAY_FL_GLOBAL)
		tr->dir = debugfs_create_dir("tracing", NULL);

	if (!tr->dir)
		pr_warn_once("Could not create debugfs directory 'tracing'\n");

	return tr->dir;
}

struct dentry *tracing_init_dentry(void)
{
	return tracing_init_dentry_tr(&global_trace);
}

static struct dentry *tracing_dentry_percpu(struct trace_array *tr, int cpu)
{
	struct dentry *d_tracer;

	if (tr->percpu_dir)
		return tr->percpu_dir;

	d_tracer = tracing_init_dentry_tr(tr);
	if (!d_tracer)
		return NULL;

	tr->percpu_dir = debugfs_create_dir("per_cpu", d_tracer);

	WARN_ONCE(!tr->percpu_dir,
		  "Could not create debugfs directory 'per_cpu/%d'\n", cpu);

	return tr->percpu_dir;
}

static struct dentry *
trace_create_cpu_file(const char *name, umode_t mode, struct dentry *parent,
		      void *data, long cpu, const struct file_operations *fops)
{
	struct dentry *ret = trace_create_file(name, mode, parent, data, fops);

	if (ret) 
		ret->d_inode->i_cdev = (void *)(cpu + 1);
	return ret;
}

static void
tracing_init_debugfs_percpu(struct trace_array *tr, long cpu)
{
	struct dentry *d_percpu = tracing_dentry_percpu(tr, cpu);
	struct dentry *d_cpu;
	char cpu_dir[30]; 

	if (!d_percpu)
		return;

	snprintf(cpu_dir, 30, "cpu%ld", cpu);
	d_cpu = debugfs_create_dir(cpu_dir, d_percpu);
	if (!d_cpu) {
		pr_warning("Could not create debugfs '%s' entry\n", cpu_dir);
		return;
	}

	
	trace_create_cpu_file("trace_pipe", 0444, d_cpu,
				tr, cpu, &tracing_pipe_fops);

	
	trace_create_cpu_file("trace", 0644, d_cpu,
				tr, cpu, &tracing_fops);

	trace_create_cpu_file("trace_pipe_raw", 0444, d_cpu,
				tr, cpu, &tracing_buffers_fops);

	trace_create_cpu_file("stats", 0444, d_cpu,
				tr, cpu, &tracing_stats_fops);

	trace_create_cpu_file("buffer_size_kb", 0444, d_cpu,
				tr, cpu, &tracing_entries_fops);

#ifdef CONFIG_TRACER_SNAPSHOT
	trace_create_cpu_file("snapshot", 0644, d_cpu,
				tr, cpu, &snapshot_fops);

	trace_create_cpu_file("snapshot_raw", 0444, d_cpu,
				tr, cpu, &snapshot_raw_fops);
#endif
}

#ifdef CONFIG_FTRACE_SELFTEST
#include "trace_selftest.c"
#endif

struct trace_option_dentry {
	struct tracer_opt		*opt;
	struct tracer_flags		*flags;
	struct trace_array		*tr;
	struct dentry			*entry;
};

static ssize_t
trace_options_read(struct file *filp, char __user *ubuf, size_t cnt,
			loff_t *ppos)
{
	struct trace_option_dentry *topt = filp->private_data;
	char *buf;

	if (topt->flags->val & topt->opt->bit)
		buf = "1\n";
	else
		buf = "0\n";

	return simple_read_from_buffer(ubuf, cnt, ppos, buf, 2);
}

static ssize_t
trace_options_write(struct file *filp, const char __user *ubuf, size_t cnt,
			 loff_t *ppos)
{
	struct trace_option_dentry *topt = filp->private_data;
	unsigned long val;
	int ret;

	ret = kstrtoul_from_user(ubuf, cnt, 10, &val);
	if (ret)
		return ret;

	if (val != 0 && val != 1)
		return -EINVAL;

	if (!!(topt->flags->val & topt->opt->bit) != val) {
		mutex_lock(&trace_types_lock);
		ret = __set_tracer_option(topt->tr->current_trace, topt->flags,
					  topt->opt, !val);
		mutex_unlock(&trace_types_lock);
		if (ret)
			return ret;
	}

	*ppos += cnt;

	return cnt;
}


static const struct file_operations trace_options_fops = {
	.open = tracing_open_generic,
	.read = trace_options_read,
	.write = trace_options_write,
	.llseek	= generic_file_llseek,
};

static ssize_t
trace_options_core_read(struct file *filp, char __user *ubuf, size_t cnt,
			loff_t *ppos)
{
	long index = (long)filp->private_data;
	char *buf;

	if (trace_flags & (1 << index))
		buf = "1\n";
	else
		buf = "0\n";

	return simple_read_from_buffer(ubuf, cnt, ppos, buf, 2);
}

static ssize_t
trace_options_core_write(struct file *filp, const char __user *ubuf, size_t cnt,
			 loff_t *ppos)
{
	struct trace_array *tr = &global_trace;
	long index = (long)filp->private_data;
	unsigned long val;
	int ret;

	ret = kstrtoul_from_user(ubuf, cnt, 10, &val);
	if (ret)
		return ret;

	if (val != 0 && val != 1)
		return -EINVAL;

	mutex_lock(&trace_types_lock);
	ret = set_tracer_flag(tr, 1 << index, val);
	mutex_unlock(&trace_types_lock);

	if (ret < 0)
		return ret;

	*ppos += cnt;

	return cnt;
}

static const struct file_operations trace_options_core_fops = {
	.open = tracing_open_generic,
	.read = trace_options_core_read,
	.write = trace_options_core_write,
	.llseek = generic_file_llseek,
};

struct dentry *trace_create_file(const char *name,
				 umode_t mode,
				 struct dentry *parent,
				 void *data,
				 const struct file_operations *fops)
{
	struct dentry *ret;

	ret = debugfs_create_file(name, mode, parent, data, fops);
	if (!ret)
		pr_warning("Could not create debugfs '%s' entry\n", name);

	return ret;
}


static struct dentry *trace_options_init_dentry(struct trace_array *tr)
{
	struct dentry *d_tracer;

	if (tr->options)
		return tr->options;

	d_tracer = tracing_init_dentry_tr(tr);
	if (!d_tracer)
		return NULL;

	tr->options = debugfs_create_dir("options", d_tracer);
	if (!tr->options) {
		pr_warning("Could not create debugfs directory 'options'\n");
		return NULL;
	}

	return tr->options;
}

static void
create_trace_option_file(struct trace_array *tr,
			 struct trace_option_dentry *topt,
			 struct tracer_flags *flags,
			 struct tracer_opt *opt)
{
	struct dentry *t_options;

	t_options = trace_options_init_dentry(tr);
	if (!t_options)
		return;

	topt->flags = flags;
	topt->opt = opt;
	topt->tr = tr;

	topt->entry = trace_create_file(opt->name, 0644, t_options, topt,
				    &trace_options_fops);

}

static struct trace_option_dentry *
create_trace_option_files(struct trace_array *tr, struct tracer *tracer)
{
	struct trace_option_dentry *topts;
	struct tracer_flags *flags;
	struct tracer_opt *opts;
	int cnt;

	if (!tracer)
		return NULL;

	flags = tracer->flags;

	if (!flags || !flags->opts)
		return NULL;

	opts = flags->opts;

	for (cnt = 0; opts[cnt].name; cnt++)
		;

	topts = kcalloc(cnt + 1, sizeof(*topts), GFP_KERNEL);
	if (!topts)
		return NULL;

	for (cnt = 0; opts[cnt].name; cnt++)
		create_trace_option_file(tr, &topts[cnt], flags,
					 &opts[cnt]);

	return topts;
}

static void
destroy_trace_option_files(struct trace_option_dentry *topts)
{
	int cnt;

	if (!topts)
		return;

	for (cnt = 0; topts[cnt].opt; cnt++) {
		if (topts[cnt].entry)
			debugfs_remove(topts[cnt].entry);
	}

	kfree(topts);
}

static struct dentry *
create_trace_option_core_file(struct trace_array *tr,
			      const char *option, long index)
{
	struct dentry *t_options;

	t_options = trace_options_init_dentry(tr);
	if (!t_options)
		return NULL;

	return trace_create_file(option, 0644, t_options, (void *)index,
				    &trace_options_core_fops);
}

static __init void create_trace_options_dir(struct trace_array *tr)
{
	struct dentry *t_options;
	int i;

	t_options = trace_options_init_dentry(tr);
	if (!t_options)
		return;

	for (i = 0; trace_options[i]; i++)
		create_trace_option_core_file(tr, trace_options[i], i);
}

static ssize_t
rb_simple_read(struct file *filp, char __user *ubuf,
	       size_t cnt, loff_t *ppos)
{
	struct trace_array *tr = filp->private_data;
	char buf[64];
	int r;

	r = tracer_tracing_is_on(tr);
	r = sprintf(buf, "%d\n", r);

	return simple_read_from_buffer(ubuf, cnt, ppos, buf, r);
}

static ssize_t
rb_simple_write(struct file *filp, const char __user *ubuf,
		size_t cnt, loff_t *ppos)
{
	struct trace_array *tr = filp->private_data;
	struct ring_buffer *buffer = tr->trace_buffer.buffer;
	unsigned long val;
	int ret;

	ret = kstrtoul_from_user(ubuf, cnt, 10, &val);
	if (ret)
		return ret;

	if (buffer) {
		mutex_lock(&trace_types_lock);
		if (val) {
			tracer_tracing_on(tr);
			if (tr->current_trace->start)
				tr->current_trace->start(tr);
		} else {
			tracer_tracing_off(tr);
			if (tr->current_trace->stop)
				tr->current_trace->stop(tr);
		}
		mutex_unlock(&trace_types_lock);
	}

	(*ppos)++;

	return cnt;
}

static const struct file_operations rb_simple_fops = {
	.open		= tracing_open_generic_tr,
	.read		= rb_simple_read,
	.write		= rb_simple_write,
	.release	= tracing_release_generic_tr,
	.llseek		= default_llseek,
};

struct dentry *trace_instance_dir;

static void
init_tracer_debugfs(struct trace_array *tr, struct dentry *d_tracer);

static void init_trace_buffers(struct trace_array *tr, struct trace_buffer *buf)
{
	int cpu;

	for_each_tracing_cpu(cpu) {
		memset(per_cpu_ptr(buf->data, cpu), 0, sizeof(struct trace_array_cpu));
		per_cpu_ptr(buf->data, cpu)->trace_cpu.cpu = cpu;
		per_cpu_ptr(buf->data, cpu)->trace_cpu.tr = tr;
	}
}

static int
allocate_trace_buffer(struct trace_array *tr, struct trace_buffer *buf, int size)
{
	enum ring_buffer_flags rb_flags;

	rb_flags = trace_flags & TRACE_ITER_OVERWRITE ? RB_FL_OVERWRITE : 0;

	buf->buffer = ring_buffer_alloc(size, rb_flags);
	if (!buf->buffer)
		return -ENOMEM;

	buf->data = alloc_percpu(struct trace_array_cpu);
	if (!buf->data) {
		ring_buffer_free(buf->buffer);
		return -ENOMEM;
	}

	init_trace_buffers(tr, buf);

	
	set_buffer_entries(&tr->trace_buffer,
			   ring_buffer_size(tr->trace_buffer.buffer, 0));

	return 0;
}

static int allocate_trace_buffers(struct trace_array *tr, int size)
{
	int ret;

	ret = allocate_trace_buffer(tr, &tr->trace_buffer, size);
	if (ret)
		return ret;

#ifdef CONFIG_TRACER_MAX_TRACE
	ret = allocate_trace_buffer(tr, &tr->max_buffer,
				    allocate_snapshot ? size : 1);
	if (WARN_ON(ret)) {
		ring_buffer_free(tr->trace_buffer.buffer);
		free_percpu(tr->trace_buffer.data);
		return -ENOMEM;
	}
	tr->allocated_snapshot = allocate_snapshot;

	allocate_snapshot = false;
#endif
	return 0;
}

static int new_instance_create(const char *name)
{
	struct trace_array *tr;
	int ret;

	mutex_lock(&trace_types_lock);

	ret = -EEXIST;
	list_for_each_entry(tr, &ftrace_trace_arrays, list) {
		if (tr->name && strcmp(tr->name, name) == 0)
			goto out_unlock;
	}

	ret = -ENOMEM;
	tr = kzalloc(sizeof(*tr), GFP_KERNEL);
	if (!tr)
		goto out_unlock;

	tr->name = kstrdup(name, GFP_KERNEL);
	if (!tr->name)
		goto out_free_tr;

	raw_spin_lock_init(&tr->start_lock);

	tr->current_trace = &nop_trace;

	INIT_LIST_HEAD(&tr->systems);
	INIT_LIST_HEAD(&tr->events);

	if (allocate_trace_buffers(tr, trace_buf_size) < 0)
		goto out_free_tr;

	
	tr->trace_cpu.cpu = RING_BUFFER_ALL_CPUS;
	tr->trace_cpu.tr = tr;

	tr->dir = debugfs_create_dir(name, trace_instance_dir);
	if (!tr->dir)
		goto out_free_tr;

	ret = event_trace_add_tracer(tr->dir, tr);
	if (ret) {
		debugfs_remove_recursive(tr->dir);
		goto out_free_tr;
	}

	init_tracer_debugfs(tr, tr->dir);

	list_add(&tr->list, &ftrace_trace_arrays);

	mutex_unlock(&trace_types_lock);

	return 0;

 out_free_tr:
	if (tr->trace_buffer.buffer)
		ring_buffer_free(tr->trace_buffer.buffer);
	kfree(tr->name);
	kfree(tr);

 out_unlock:
	mutex_unlock(&trace_types_lock);

	return ret;

}

static int instance_delete(const char *name)
{
	struct trace_array *tr;
	int found = 0;
	int ret;

	mutex_lock(&trace_types_lock);

	ret = -ENODEV;
	list_for_each_entry(tr, &ftrace_trace_arrays, list) {
		if (tr->name && strcmp(tr->name, name) == 0) {
			found = 1;
			break;
		}
	}
	if (!found)
		goto out_unlock;

	ret = -EBUSY;
	if (tr->ref)
		goto out_unlock;

	list_del(&tr->list);

	event_trace_del_tracer(tr);
	debugfs_remove_recursive(tr->dir);
	free_percpu(tr->trace_buffer.data);
	ring_buffer_free(tr->trace_buffer.buffer);

	kfree(tr->name);
	kfree(tr);

	ret = 0;

 out_unlock:
	mutex_unlock(&trace_types_lock);

	return ret;
}

static int instance_mkdir (struct inode *inode, struct dentry *dentry, umode_t mode)
{
	struct dentry *parent;
	int ret;

	
	parent = hlist_entry(inode->i_dentry.first, struct dentry, d_alias);
	if (WARN_ON_ONCE(parent != trace_instance_dir))
		return -ENOENT;

	mutex_unlock(&inode->i_mutex);

	ret = new_instance_create(dentry->d_iname);

	mutex_lock(&inode->i_mutex);

	return ret;
}

static int instance_rmdir(struct inode *inode, struct dentry *dentry)
{
	struct dentry *parent;
	int ret;

	
	parent = hlist_entry(inode->i_dentry.first, struct dentry, d_alias);
	if (WARN_ON_ONCE(parent != trace_instance_dir))
		return -ENOENT;

	
	mutex_unlock(&dentry->d_inode->i_mutex);

	mutex_unlock(&inode->i_mutex);

	ret = instance_delete(dentry->d_iname);

	mutex_lock_nested(&inode->i_mutex, I_MUTEX_PARENT);
	mutex_lock(&dentry->d_inode->i_mutex);

	return ret;
}

static const struct inode_operations instance_dir_inode_operations = {
	.lookup		= simple_lookup,
	.mkdir		= instance_mkdir,
	.rmdir		= instance_rmdir,
};

static __init void create_trace_instances(struct dentry *d_tracer)
{
	trace_instance_dir = debugfs_create_dir("instances", d_tracer);
	if (WARN_ON(!trace_instance_dir))
		return;

	
	trace_instance_dir->d_inode->i_op = &instance_dir_inode_operations;
}

static void
init_tracer_debugfs(struct trace_array *tr, struct dentry *d_tracer)
{
	int cpu;

	trace_create_file("trace_options", 0644, d_tracer,
			  tr, &tracing_iter_fops);

	trace_create_file("trace", 0644, d_tracer,
			  tr, &tracing_fops);

	trace_create_file("trace_pipe", 0444, d_tracer,
			  tr, &tracing_pipe_fops);

	trace_create_file("buffer_size_kb", 0644, d_tracer,
			  tr, &tracing_entries_fops);

	trace_create_file("buffer_total_size_kb", 0444, d_tracer,
			  tr, &tracing_total_entries_fops);

	trace_create_file("free_buffer", 0644, d_tracer,
			  tr, &tracing_free_buffer_fops);

	trace_create_file("trace_marker", 0220, d_tracer,
			  tr, &tracing_mark_fops);

	trace_create_file("saved_tgids", 0444, d_tracer,
			  tr, &tracing_saved_tgids_fops);

	trace_create_file("trace_clock", 0644, d_tracer, tr,
			  &trace_clock_fops);

	trace_create_file("tracing_on", 0644, d_tracer,
			  tr, &rb_simple_fops);

#ifdef CONFIG_TRACER_SNAPSHOT
	trace_create_file("snapshot", 0644, d_tracer,
			  tr, &snapshot_fops);
#endif

	for_each_tracing_cpu(cpu)
		tracing_init_debugfs_percpu(tr, cpu);

}

static __init int tracer_init_debugfs(void)
{
	struct dentry *d_tracer;

	trace_access_lock_init();

	d_tracer = tracing_init_dentry();
	if (!d_tracer)
		return 0;

	init_tracer_debugfs(&global_trace, d_tracer);

	trace_create_file("tracing_cpumask", 0644, d_tracer,
			&global_trace, &tracing_cpumask_fops);

	trace_create_file("available_tracers", 0444, d_tracer,
			&global_trace, &show_traces_fops);

	trace_create_file("current_tracer", 0644, d_tracer,
			&global_trace, &set_tracer_fops);

#ifdef CONFIG_TRACER_MAX_TRACE
	trace_create_file("tracing_max_latency", 0644, d_tracer,
			&tracing_max_latency, &tracing_max_lat_fops);
#endif

	trace_create_file("tracing_thresh", 0644, d_tracer,
			&tracing_thresh, &tracing_max_lat_fops);

	trace_create_file("README", 0444, d_tracer,
			NULL, &tracing_readme_fops);

	trace_create_file("saved_cmdlines", 0444, d_tracer,
			NULL, &tracing_saved_cmdlines_fops);

#ifdef CONFIG_DYNAMIC_FTRACE
	trace_create_file("dyn_ftrace_total_info", 0444, d_tracer,
			&ftrace_update_tot_cnt, &tracing_dyn_info_fops);
#endif

	create_trace_instances(d_tracer);

	create_trace_options_dir(&global_trace);

	return 0;
}

static int trace_panic_handler(struct notifier_block *this,
			       unsigned long event, void *unused)
{
	if (ftrace_dump_on_oops)
		ftrace_dump(ftrace_dump_on_oops);
	return NOTIFY_OK;
}

static struct notifier_block trace_panic_notifier = {
	.notifier_call  = trace_panic_handler,
	.next           = NULL,
	.priority       = 150   
};

static int trace_die_handler(struct notifier_block *self,
			     unsigned long val,
			     void *data)
{
	switch (val) {
	case DIE_OOPS:
		if (ftrace_dump_on_oops)
			ftrace_dump(ftrace_dump_on_oops);
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block trace_die_notifier = {
	.notifier_call = trace_die_handler,
	.priority = 200
};

#define TRACE_MAX_PRINT		1000

#define KERN_TRACE		KERN_EMERG

void
trace_printk_seq(struct trace_seq *s)
{
	
	if (s->len >= TRACE_MAX_PRINT)
		s->len = TRACE_MAX_PRINT;

	
	s->buffer[s->len] = 0;

	printk(KERN_TRACE "%s", s->buffer);

	trace_seq_init(s);
}

void trace_init_global_iter(struct trace_iterator *iter)
{
	iter->tr = &global_trace;
	iter->trace = iter->tr->current_trace;
	iter->cpu_file = RING_BUFFER_ALL_CPUS;
	iter->trace_buffer = &global_trace.trace_buffer;
}

void ftrace_dump(enum ftrace_dump_mode oops_dump_mode)
{
	
	static struct trace_iterator iter;
	static atomic_t dump_running;
	unsigned int old_userobj;
	unsigned long flags;
	int cnt = 0, cpu;

	
	if (atomic_inc_return(&dump_running) != 1) {
		atomic_dec(&dump_running);
		return;
	}

	tracing_off();

	local_irq_save(flags);

	
	trace_init_global_iter(&iter);

	for_each_tracing_cpu(cpu) {
		atomic_inc(&per_cpu_ptr(iter.tr->trace_buffer.data, cpu)->disabled);
	}

	old_userobj = trace_flags & TRACE_ITER_SYM_USEROBJ;

	
	trace_flags &= ~TRACE_ITER_SYM_USEROBJ;

	switch (oops_dump_mode) {
	case DUMP_ALL:
		iter.cpu_file = RING_BUFFER_ALL_CPUS;
		break;
	case DUMP_ORIG:
		iter.cpu_file = raw_smp_processor_id();
		break;
	case DUMP_NONE:
		goto out_enable;
	default:
		printk(KERN_TRACE "Bad dumping mode, switching to all CPUs dump\n");
		iter.cpu_file = RING_BUFFER_ALL_CPUS;
	}

	printk(KERN_TRACE "Dumping ftrace buffer:\n");

	
	if (ftrace_is_dead()) {
		printk("# WARNING: FUNCTION TRACING IS CORRUPTED\n");
		printk("#          MAY BE MISSING FUNCTION EVENTS\n");
	}


	while (!trace_empty(&iter)) {

		if (!cnt)
			printk(KERN_TRACE "---------------------------------\n");

		cnt++;

		
		memset(&iter.seq, 0,
		       sizeof(struct trace_iterator) -
		       offsetof(struct trace_iterator, seq));
		iter.iter_flags |= TRACE_FILE_LAT_FMT;
		iter.pos = -1;

		if (trace_find_next_entry_inc(&iter) != NULL) {
			int ret;

			ret = print_trace_line(&iter);
			if (ret != TRACE_TYPE_NO_CONSUME)
				trace_consume(&iter);
		}
		touch_nmi_watchdog();

		trace_printk_seq(&iter.seq);
	}

	if (!cnt)
		printk(KERN_TRACE "   (ftrace buffer empty)\n");
	else
		printk(KERN_TRACE "---------------------------------\n");

 out_enable:
	trace_flags |= old_userobj;

	for_each_tracing_cpu(cpu) {
		atomic_dec(&per_cpu_ptr(iter.trace_buffer->data, cpu)->disabled);
	}
 	atomic_dec(&dump_running);
	local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(ftrace_dump);

__init static int tracer_alloc_buffers(void)
{
	int ring_buf_size;
	int ret = -ENOMEM;


	if (!alloc_cpumask_var(&tracing_buffer_mask, GFP_KERNEL))
		goto out;

	if (!alloc_cpumask_var(&tracing_cpumask, GFP_KERNEL))
		goto out_free_buffer_mask;

	
	if (__stop___trace_bprintk_fmt != __start___trace_bprintk_fmt)
		
		trace_printk_init_buffers();

	
	if (ring_buffer_expanded)
		ring_buf_size = trace_buf_size;
	else
		ring_buf_size = 1;

	cpumask_copy(tracing_buffer_mask, cpu_possible_mask);
	cpumask_copy(tracing_cpumask, cpu_all_mask);

	raw_spin_lock_init(&global_trace.start_lock);

	
	if (allocate_trace_buffers(&global_trace, ring_buf_size) < 0) {
		printk(KERN_ERR "tracer: failed to allocate ring buffer!\n");
		WARN_ON(1);
		goto out_free_cpumask;
	}

	if (global_trace.buffer_disabled)
		tracing_off();

	trace_init_cmdlines();

	global_trace.current_trace = &nop_trace;

	register_tracer(&nop_trace);

	
	tracing_disabled = 0;

	atomic_notifier_chain_register(&panic_notifier_list,
				       &trace_panic_notifier);

	register_die_notifier(&trace_die_notifier);

	global_trace.flags = TRACE_ARRAY_FL_GLOBAL;

	
	global_trace.trace_cpu.cpu = RING_BUFFER_ALL_CPUS;
	global_trace.trace_cpu.tr = &global_trace;

	INIT_LIST_HEAD(&global_trace.systems);
	INIT_LIST_HEAD(&global_trace.events);
	list_add(&global_trace.list, &ftrace_trace_arrays);

	while (trace_boot_options) {
		char *option;

		option = strsep(&trace_boot_options, ",");
		trace_set_options(&global_trace, option);
	}

	register_snapshot_cmd();

	return 0;

out_free_cpumask:
	free_percpu(global_trace.trace_buffer.data);
#ifdef CONFIG_TRACER_MAX_TRACE
	free_percpu(global_trace.max_buffer.data);
#endif
	free_cpumask_var(tracing_cpumask);
out_free_buffer_mask:
	free_cpumask_var(tracing_buffer_mask);
out:
	return ret;
}

__init static int clear_boot_tracer(void)
{
	if (!default_bootup_tracer)
		return 0;

	printk(KERN_INFO "ftrace bootup tracer '%s' not registered.\n",
	       default_bootup_tracer);
	default_bootup_tracer = NULL;

	return 0;
}

early_initcall(tracer_alloc_buffers);
fs_initcall(tracer_init_debugfs);
late_initcall(clear_boot_tracer);

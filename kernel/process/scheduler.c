#include <stdint.h>
#include <stddef.h>
#include <kernel/kernel_string.h>  /* memcpy/memset helpers */

#include <kernel/scheduler.h>
#include <kernel/process.h>
#include <kernel/kernel.h>    /* for panic, if needed */
#include <kernel/lock.h>
#include <kernel/cpu/cpu.h>
#include <kernel/smp.h>
#include <kernel/drivers/debug/debug.h>
#include <kernel/drivers/timer/timer.h>
#include <kernel/event.h>

/* simple singly‑linked list of processes */
static process_t *g_head = NULL;
static process_t *g_current[32];
static process_t *g_cursor[32];
static uint32_t g_timeslice_remaining[32];
static spinlock_t g_scheduler_lock;
static int g_scheduler_boot_trace_emitted = 0;
static int g_scheduler_switch_trace_budget = 24;
static volatile int g_scheduler_preemption_ready = 0;

#define SCHEDULER_TIMESLICE_TICKS 4u

static void scheduler_timeout_tick_hook(uint32_t tick);

static void scheduler_account_runtime(process_t *task, uint32_t now_ticks) {
    if (task == NULL || task->state != PROCESS_RUNNING || task->last_start_tick == 0u) {
        return;
    }

    if (now_ticks >= task->last_start_tick) {
        task->runtime_ticks += (now_ticks - task->last_start_tick);
    }
    task->last_start_tick = 0u;
}

static void scheduler_trace_switch(uint32_t cpu_index,
                                   const process_t *old_task,
                                   const process_t *next_task) {
    if (g_scheduler_switch_trace_budget <= 0 || next_task == NULL) {
        return;
    }

    g_scheduler_switch_trace_budget -= 1;
    kernel_debug_printf("scheduler: cpu=%d old=%d/%d next=%d/%d state=%d\n",
                        (int)cpu_index,
                        old_task != NULL ? old_task->pid : -1,
                        old_task != NULL ? (int)old_task->service_type : -1,
                        next_task->pid,
                        (int)next_task->service_type,
                        (int)next_task->state);
}

static uint32_t scheduler_online_cpu_count(void) {
    uint32_t count = smp_started_cpu_count();

    if (count == 0u) {
        count = 1u;
    }
    if (count > kernel_cpu_count()) {
        count = kernel_cpu_count();
    }
    if (count == 0u) {
        count = 1u;
    }
    return count;
}

static uint32_t scheduler_cpu_load(uint32_t cpu_index) {
    process_t *task;
    uint32_t load = 0u;

    for (task = g_head; task != NULL; task = task->next) {
        if (task->state == PROCESS_TERMINATED) {
            continue;
        }
        if (task->current_cpu == (int)cpu_index || task->preferred_cpu == (int)cpu_index) {
            load += 1u;
        }
    }
    return load;
}

static int scheduler_pick_target_cpu(void) {
    uint32_t cpu_count = scheduler_online_cpu_count();
    uint32_t best_cpu = 0u;
    uint32_t best_load = 0u;
    uint32_t cpu;

    for (cpu = 0u; cpu < cpu_count; ++cpu) {
        uint32_t load = scheduler_cpu_load(cpu);

        if (cpu == 0u || load < best_load) {
            best_cpu = cpu;
            best_load = load;
        }
    }

    return (int)best_cpu;
}

static process_t *scheduler_first_runnable(void) {
    process_t *task;

    for (task = g_head; task != NULL; task = task->next) {
        if (task->state == PROCESS_READY && task->current_cpu < 0) {
            return task;
        }
    }
    return NULL;
}

static int scheduler_task_score(const process_t *task, uint32_t cpu_index) {
    int affinity_score;

    if (task == NULL) {
        return 0x7fffffff;
    }
    affinity_score = 3;
    if (task->preferred_cpu == (int)cpu_index) {
        affinity_score = 0;
    } else if (task->last_cpu == (int)cpu_index) {
        affinity_score = 1;
    } else if (task->preferred_cpu < 0) {
        affinity_score = 2;
    }
    return (int)(task->priority_tier * 16u) + affinity_score;
}

void scheduler_init(void) {
    uint32_t i;

    g_head = NULL;
    for (i = 0; i < 32u; ++i) {
        g_current[i] = NULL;
        g_cursor[i] = NULL;
        g_timeslice_remaining[i] = SCHEDULER_TIMESLICE_TICKS;
    }
    spinlock_init(&g_scheduler_lock);
    g_scheduler_preemption_ready = 0;
    (void)kernel_timer_register_tick_hook(scheduler_timeout_tick_hook);
}

void scheduler_add_task(process_t *proc) {
    uint32_t flags;

    if (proc == NULL) {
        return;
    }
    flags = spinlock_lock_irqsave(&g_scheduler_lock);
    proc->next = NULL;
    proc->state = PROCESS_READY;
    proc->current_cpu = -1;
    proc->last_cpu = -1;
    proc->preferred_cpu = scheduler_pick_target_cpu();
    if (g_head == NULL) {
        g_head = proc;
        g_cursor[proc->preferred_cpu >= 0 ? (uint32_t)proc->preferred_cpu : 0u] = proc;
    } else {
        process_t *p = g_head;
        while (p->next) {
            p = p->next;
        }
        p->next = proc;
    }
    spinlock_unlock_irqrestore(&g_scheduler_lock, flags);
}

static process_t *find_next(uint32_t cpu_index, process_t *current) {
    process_t *start;
    process_t *task;
    process_t *best = NULL;
    int best_score = 0x7fffffff;

    if (g_head == NULL) {
        return NULL;
    }

    start = g_cursor[cpu_index];
    if (start == NULL) {
        start = current != NULL && current->next != NULL ? current->next : g_head;
    }
    if (start == NULL) {
        start = scheduler_first_runnable();
    }
    if (start == NULL) {
        return NULL;
    }

    task = start;
    do {
        if (task->state == PROCESS_READY && task->current_cpu < 0) {
            int score = scheduler_task_score(task, cpu_index);

            if (best == NULL || score < best_score) {
                best = task;
                best_score = score;
                if (score == 0) {
                    break;
                }
            }
        }

        task = task->next != NULL ? task->next : g_head;
    } while (task != NULL && task != start);

    return best;
}

void scheduler_set_preemption_ready(int ready) {
    g_scheduler_preemption_ready = ready != 0 ? 1 : 0;
}

kernel_trap_frame_t *scheduler_schedule_frame(kernel_trap_frame_t *frame, int preemptive) {
    uint32_t cpu_index = kernel_cpu_index();
    uint32_t now_ticks = kernel_timer_get_ticks();
    process_t *current;
    process_t *next;
    process_t *old_task;
    kernel_trap_frame_t *resume_frame;
    uint32_t flags = spinlock_lock_irqsave(&g_scheduler_lock);

    (void)preemptive;

    if (cpu_index >= 32u) {
        cpu_index = 0u;
    }
    current = g_current[cpu_index];
    if (!g_scheduler_preemption_ready) {
        spinlock_unlock_irqrestore(&g_scheduler_lock, flags);
        return frame;
    }
    if (preemptive &&
        current != NULL &&
        current->state == PROCESS_RUNNING &&
        current->current_cpu == (int)cpu_index) {
        current->context = frame;
        if (g_timeslice_remaining[cpu_index] > 1u) {
            g_timeslice_remaining[cpu_index] -= 1u;
            spinlock_unlock_irqrestore(&g_scheduler_lock, flags);
            return frame;
        }
    }
    if (current && current->state == PROCESS_RUNNING && current->current_cpu == (int)cpu_index) {
        current->context = frame;
        scheduler_account_runtime(current, now_ticks);
        current->state = PROCESS_READY;
        current->current_cpu = -1;
        current->last_cpu = (int)cpu_index;
    } else if (current && current->state == PROCESS_BLOCKED &&
               current->current_cpu == (int)cpu_index) {
        current->context = frame;
        scheduler_account_runtime(current, now_ticks);
        current->current_cpu = -1;
        current->last_cpu = (int)cpu_index;
    }
    next = find_next(cpu_index, current);
    if (next == NULL) {
        if (current && current->state == PROCESS_READY) {
            current->state = PROCESS_RUNNING;
            current->current_cpu = (int)cpu_index;
            current->last_cpu = (int)cpu_index;
            current->last_start_tick = now_ticks;
            g_timeslice_remaining[cpu_index] = SCHEDULER_TIMESLICE_TICKS;
            g_current[cpu_index] = current;
            resume_frame = current->context != NULL ? current->context : frame;
            spinlock_unlock_irqrestore(&g_scheduler_lock, flags);
            return resume_frame;
        }

        g_current[cpu_index] = 0;
        g_timeslice_remaining[cpu_index] = SCHEDULER_TIMESLICE_TICKS;
        spinlock_unlock_irqrestore(&g_scheduler_lock, flags);

        for (;;) {
            __asm__ volatile("sti; hlt; cli" : : : "memory");
            now_ticks = kernel_timer_get_ticks();
            flags = spinlock_lock_irqsave(&g_scheduler_lock);
            next = find_next(cpu_index, 0);
            if (next != NULL) {
                break;
            }
            spinlock_unlock_irqrestore(&g_scheduler_lock, flags);
        }
    }

    g_cursor[cpu_index] = next->next != NULL ? next->next : g_head;
    old_task = current;

    if (next != current) {
        next->state = PROCESS_RUNNING;
        next->current_cpu = (int)cpu_index;
        next->last_cpu = (int)cpu_index;
        next->last_start_tick = now_ticks;
        next->context_switches += 1u;
        g_timeslice_remaining[cpu_index] = SCHEDULER_TIMESLICE_TICKS;
        g_current[cpu_index] = next;
        resume_frame = next->context;
        spinlock_unlock_irqrestore(&g_scheduler_lock, flags);
        if (old_task == NULL && !g_scheduler_boot_trace_emitted) {
            g_scheduler_boot_trace_emitted = 1;
            kernel_debug_printf("scheduler: first dispatch cpu=%d pid=%d kind=%d service=%d eip=%x esp=%x\n",
                                (int)cpu_index,
                                next->pid,
                                (int)next->kind,
                                (int)next->service_type,
                                (unsigned int)process_saved_eip(next),
                                (unsigned int)process_saved_esp(next));
        }
        scheduler_trace_switch(cpu_index, old_task, next);
        return resume_frame != NULL ? resume_frame : frame;
    } else {
        current->state = PROCESS_RUNNING;
        current->current_cpu = (int)cpu_index;
        current->last_cpu = (int)cpu_index;
        current->last_start_tick = now_ticks;
        current->context = frame;
        g_timeslice_remaining[cpu_index] = SCHEDULER_TIMESLICE_TICKS;
        resume_frame = current->context;
        spinlock_unlock_irqrestore(&g_scheduler_lock, flags);
        return resume_frame != NULL ? resume_frame : frame;
    }
}

void schedule(void) {
    yield();
}

void yield(void) {
    __asm__ volatile("int $0x81" : : : "memory");
}

process_t *scheduler_current(void) {
    return scheduler_current_for_cpu(kernel_cpu_index());
}

process_t *scheduler_current_for_cpu(uint32_t cpu_index) {
    process_t *current;
    uint32_t flags;

    if (cpu_index >= 32u) {
        cpu_index = 0u;
    }
    flags = spinlock_lock_irqsave(&g_scheduler_lock);
    current = g_current[cpu_index];
    spinlock_unlock_irqrestore(&g_scheduler_lock, flags);
    return current;
}

uint32_t scheduler_current_pid_for_cpu(uint32_t cpu_index) {
    process_t *current;
    uint32_t flags;

    if (cpu_index >= 32u) {
        cpu_index = 0u;
    }
    flags = spinlock_lock_irqsave(&g_scheduler_lock);
    current = g_current[cpu_index];
    spinlock_unlock_irqrestore(&g_scheduler_lock, flags);
    return current != NULL ? (uint32_t)current->pid : 0u;
}

uint32_t scheduler_current_pid(void) {
    return scheduler_current_pid_for_cpu(kernel_cpu_index());
}

process_t *scheduler_find_task_by_pid(int pid) {
    process_t *task;
    uint32_t flags;

    if (pid <= 0) {
        return NULL;
    }

    flags = spinlock_lock_irqsave(&g_scheduler_lock);
    for (task = g_head; task != NULL; task = task->next) {
        if (task->pid == pid && task->state != PROCESS_TERMINATED) {
            spinlock_unlock_irqrestore(&g_scheduler_lock, flags);
            return task;
        }
    }
    spinlock_unlock_irqrestore(&g_scheduler_lock, flags);
    return NULL;
}

void scheduler_terminate_task(process_t *task) {
    uint32_t flags;
    uint32_t cpu;
    uint32_t now_ticks;

    if (task == NULL) {
        return;
    }

    now_ticks = kernel_timer_get_ticks();
    flags = spinlock_lock_irqsave(&g_scheduler_lock);
    scheduler_account_runtime(task, now_ticks);
    task->state = PROCESS_TERMINATED;
    task->current_cpu = -1;
    task->preferred_cpu = -1;
    task->last_cpu = -1;
    task->wait_channel = 0;
    task->wait_deadline = 0u;
    task->wait_result = TASK_WAIT_RESULT_NONE;
    task->wait_event_kind = TASK_WAIT_EVENT_NONE;
    task->wait_event_class = TASK_WAIT_CLASS_NONE;
    task->wait_owner_service = 0u;
    task->wait_next = 0;

    for (cpu = 0u; cpu < 32u; ++cpu) {
        if (g_current[cpu] == task) {
            g_current[cpu] = NULL;
        }
        if (g_cursor[cpu] == task) {
            g_cursor[cpu] = task->next != NULL ? task->next : g_head;
            if (g_cursor[cpu] == task) {
                g_cursor[cpu] = NULL;
            }
        }
    }

    spinlock_unlock_irqrestore(&g_scheduler_lock, flags);
}

int scheduler_block_current_ex(const void *wait_channel,
                               uint32_t wait_deadline,
                               uint32_t wait_event_kind,
                               uint32_t wait_event_class,
                               uint32_t wait_owner_service) {
    process_t *current;
    uint32_t cpu_index = kernel_cpu_index();
    uint32_t flags;

    if (cpu_index >= 32u) {
        cpu_index = 0u;
    }

    flags = spinlock_lock_irqsave(&g_scheduler_lock);
    current = g_current[cpu_index];
    if (current == NULL || current->state != PROCESS_RUNNING) {
        spinlock_unlock_irqrestore(&g_scheduler_lock, flags);
        return -1;
    }

    current->state = PROCESS_BLOCKED;
    current->wait_channel = wait_channel;
    current->wait_deadline = wait_deadline;
    current->wait_result = TASK_WAIT_RESULT_NONE;
    current->wait_event_kind = wait_event_kind;
    current->wait_event_class = wait_event_class;
    current->wait_owner_service = wait_owner_service;
    spinlock_unlock_irqrestore(&g_scheduler_lock, flags);
    return 0;
}

int scheduler_block_current(const void *wait_channel) {
    return scheduler_block_current_ex(wait_channel,
                                      0u,
                                      TASK_WAIT_EVENT_WAITABLE,
                                      TASK_WAIT_CLASS_GENERIC,
                                      0u);
}

int scheduler_wake_task(process_t *task) {
    return scheduler_complete_wait(task, TASK_WAIT_RESULT_SIGNALED);
}

int scheduler_complete_wait(process_t *task, uint32_t wait_result) {
    uint32_t flags;

    if (task == NULL) {
        return -1;
    }

    flags = spinlock_lock_irqsave(&g_scheduler_lock);
    if (task->state == PROCESS_BLOCKED) {
        task->state = PROCESS_READY;
        task->current_cpu = -1;
        task->wait_deadline = 0u;
        task->wait_result = wait_result;
        task->wait_next = 0;
        spinlock_unlock_irqrestore(&g_scheduler_lock, flags);
        return 0;
    }
    spinlock_unlock_irqrestore(&g_scheduler_lock, flags);
    return -1;
}

static void scheduler_timeout_tick_hook(uint32_t tick) {
    process_t *task;
    uint32_t flags;

    flags = spinlock_lock_irqsave(&g_scheduler_lock);
    for (task = g_head; task != NULL; task = task->next) {
        if (task->state != PROCESS_BLOCKED || task->wait_channel == 0 || task->wait_deadline == 0u) {
            continue;
        }
        if (tick < task->wait_deadline) {
            continue;
        }

        kernel_waitable_detach_task((kernel_waitable_t *)task->wait_channel, task);
        task->state = PROCESS_READY;
        task->current_cpu = -1;
        task->wait_channel = 0;
        task->wait_deadline = 0u;
        task->wait_result = TASK_WAIT_RESULT_TIMED_OUT;
        task->wait_next = 0;
    }
    spinlock_unlock_irqrestore(&g_scheduler_lock, flags);
}

uint32_t scheduler_snapshot(struct task_snapshot_entry *entries,
                            uint32_t max_entries,
                            struct task_snapshot_summary *summary) {
    process_t *task;
    uint32_t count = 0u;
    uint32_t flags;
    uint32_t now_ticks = kernel_timer_get_ticks();

    if (summary != NULL) {
        uint32_t snapshot_cpu = kernel_cpu_index();

        memset(summary, 0, sizeof(*summary));
        summary->abi_version = TASK_SNAPSHOT_ABI_VERSION;
        summary->uptime_ticks = now_ticks;
        summary->cpu_count = kernel_cpu_count();
        summary->current_pid = scheduler_current_pid_for_cpu(snapshot_cpu);
        for (uint32_t cpu = 0u; cpu < summary->cpu_count; ++cpu) {
            const struct kernel_cpu_state *state = kernel_cpu_state(cpu);

            if (state != NULL && state->started != 0u) {
                summary->started_cpu_count += 1u;
            }
        }
    }

    flags = spinlock_lock_irqsave(&g_scheduler_lock);
    for (task = g_head; task != NULL; task = task->next) {
        uint32_t runtime_ticks = task->runtime_ticks;

        if (task->state == PROCESS_TERMINATED) {
            continue;
        }

        if (task->state == PROCESS_RUNNING && task->last_start_tick != 0u && now_ticks >= task->last_start_tick) {
            runtime_ticks += (now_ticks - task->last_start_tick);
        }

        if (summary != NULL) {
            summary->total_tasks += 1u;
            if (task->state == PROCESS_RUNNING) {
                summary->running_tasks += 1u;
            } else if (task->state == PROCESS_READY) {
                summary->ready_tasks += 1u;
            } else if (task->state == PROCESS_BLOCKED) {
                summary->blocked_tasks += 1u;
            }
            if (task->wait_result == TASK_WAIT_RESULT_TIMED_OUT) {
                summary->timed_out_waits += 1u;
            } else if (task->wait_result == TASK_WAIT_RESULT_CANCELED) {
                summary->canceled_waits += 1u;
            }
            if (task->wait_channel != 0) {
                const kernel_waitable_t *waitable = (const kernel_waitable_t *)task->wait_channel;

                summary->pending_event_signals += waitable->pending_signals;
            }
        }

        if (entries != NULL && count < max_entries) {
            struct task_snapshot_entry *entry = &entries[count];

            memset(entry, 0, sizeof(*entry));
            entry->pid = (uint32_t)task->pid;
            entry->kind = (uint32_t)task->kind;
            entry->state = (uint32_t)task->state;
            entry->current_cpu = task->current_cpu;
            entry->preferred_cpu = task->preferred_cpu;
            entry->last_cpu = task->last_cpu;
            entry->stack_size = task->stack_size;
            entry->runtime_ticks = runtime_ticks;
            entry->context_switches = task->context_switches;
            entry->service_type = task->service_type;
            entry->priority_tier = task->priority_tier;
            entry->wait_result = task->wait_result;
            entry->wait_event_kind = task->wait_event_kind;
            entry->wait_event_class = task->wait_event_class;
            entry->wait_owner_service = task->wait_owner_service;
            entry->wait_deadline = task->wait_deadline;
            entry->wait_pending_signals =
                task->wait_channel != 0 ? ((const kernel_waitable_t *)task->wait_channel)->pending_signals : 0u;
            count += 1u;
        }
    }
    spinlock_unlock_irqrestore(&g_scheduler_lock, flags);

    return count;
}

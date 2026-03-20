#include <stdint.h>
#include <stddef.h>
#include <kernel/kernel_string.h>  /* memcpy/memset helpers */

#include <kernel/scheduler.h>
#include <kernel/process.h>
#include <kernel/kernel.h>    /* for panic, if needed */
#include <kernel/lock.h>
#include <kernel/cpu/cpu.h>

/* prototype for the low‑level context switch routine (implemented in assembly) */
extern void context_switch(void *old_task, void *new_task);

/* simple singly‑linked list of processes */
static process_t *g_head = NULL;
static process_t *g_current[32];
static spinlock_t g_scheduler_lock;

void scheduler_init(void) {
    g_head = NULL;
    for (uint32_t i = 0; i < 32u; ++i) {
        g_current[i] = NULL;
    }
    spinlock_init(&g_scheduler_lock);
}

void scheduler_add_task(process_t *proc) {
    uint32_t flags;

    if (proc == NULL) {
        return;
    }
    flags = spinlock_lock_irqsave(&g_scheduler_lock);
    proc->next = NULL;
    proc->state = PROCESS_READY;
    if (g_head == NULL) {
        g_head = proc;
    } else {
        process_t *p = g_head;
        while (p->next) {
            p = p->next;
        }
        p->next = proc;
    }
    spinlock_unlock_irqrestore(&g_scheduler_lock, flags);
}

static process_t *find_next(process_t *current) {
    if (g_head == NULL) {
        return NULL;
    }
    if (current == NULL) {
        return g_head;
    }
    process_t *p = current->next ? current->next : g_head;
    while (p && (p->state != PROCESS_READY || p->current_cpu >= 0)) {
        p = p->next ? p->next : g_head;
        if (p == current) {
            /* we looped around */
            break;
        }
    }
    if (p->state == PROCESS_READY && p->current_cpu < 0) {
        return p;
    }
    return NULL;
}

void schedule(void) {
    uint32_t cpu_index = kernel_cpu_index();
    process_t *current;
    process_t *next;
    uint32_t flags = spinlock_lock_irqsave(&g_scheduler_lock);

    if (cpu_index >= 32u) {
        cpu_index = 0u;
    }
    current = g_current[cpu_index];
    if (current && current->state == PROCESS_RUNNING && current->current_cpu == (int)cpu_index) {
        current->state = PROCESS_READY;
        current->current_cpu = -1;
    }
    next = find_next(current);
    if (next == NULL) {
        if (current && current->state == PROCESS_READY) {
            current->state = PROCESS_RUNNING;
            current->current_cpu = (int)cpu_index;
        }
        spinlock_unlock_irqrestore(&g_scheduler_lock, flags);
        return; /* nothing to do */
    }

    if (current == NULL) {
        /* first switch into user/task context */
        next->state = PROCESS_RUNNING;
        next->current_cpu = (int)cpu_index;
        g_current[cpu_index] = next;
        spinlock_unlock_irqrestore(&g_scheduler_lock, flags);
        context_switch(NULL, next);
    } else if (next != current) {
        process_t *old = current;
        next->state = PROCESS_RUNNING;
        next->current_cpu = (int)cpu_index;
        g_current[cpu_index] = next;
        spinlock_unlock_irqrestore(&g_scheduler_lock, flags);
        context_switch(old, next);
    } else {
        current->state = PROCESS_RUNNING;
        current->current_cpu = (int)cpu_index;
        spinlock_unlock_irqrestore(&g_scheduler_lock, flags);
    }
}

void yield(void) {
    schedule();
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

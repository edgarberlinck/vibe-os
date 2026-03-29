#include <kernel/drivers/input/input.h>

#include <kernel/event.h>
#include <kernel/interrupt.h>
#include <kernel/kernel_string.h>
#include <kernel/microkernel/service.h>

#define KERNEL_INPUT_EVENT_QUEUE_CAPACITY 256u

static struct input_event g_input_event_queue[KERNEL_INPUT_EVENT_QUEUE_CAPACITY];
static volatile uint16_t g_input_event_head = 0u;
static volatile uint16_t g_input_event_tail = 0u;
static kernel_waitable_t g_input_event_waitable;

static void kernel_input_event_push_unlocked(const struct input_event *event) {
    uint16_t next;

    if (event == 0) {
        return;
    }

    next = (uint16_t)((g_input_event_head + 1u) % KERNEL_INPUT_EVENT_QUEUE_CAPACITY);
    if (next == g_input_event_tail) {
        g_input_event_tail = (uint16_t)((g_input_event_tail + 1u) % KERNEL_INPUT_EVENT_QUEUE_CAPACITY);
    }

    g_input_event_queue[g_input_event_head] = *event;
    g_input_event_head = next;
}

void kernel_input_event_init(void) {
    uint32_t flags = kernel_irq_save();

    memset(g_input_event_queue, 0, sizeof(g_input_event_queue));
    g_input_event_head = 0u;
    g_input_event_tail = 0u;
    kernel_waitable_init_ex(&g_input_event_waitable,
                            TASK_WAIT_EVENT_QUEUE,
                            TASK_WAIT_CLASS_INPUT,
                            MK_SERVICE_INPUT);
    kernel_irq_restore(flags);
}

int kernel_input_event_dequeue(struct input_event *event) {
    uint32_t flags;

    if (event == 0) {
        return 0;
    }

    flags = kernel_irq_save();
    if (g_input_event_tail == g_input_event_head) {
        memset(event, 0, sizeof(*event));
        kernel_irq_restore(flags);
        return 0;
    }

    *event = g_input_event_queue[g_input_event_tail];
    g_input_event_tail = (uint16_t)((g_input_event_tail + 1u) % KERNEL_INPUT_EVENT_QUEUE_CAPACITY);
    kernel_irq_restore(flags);
    return 1;
}

int kernel_input_event_wait(struct input_event *event) {
    if (event == 0) {
        return 0;
    }

    for (;;) {
        if (kernel_input_event_dequeue(event) != 0) {
            return 1;
        }
        if (kernel_waitable_wait(&g_input_event_waitable) != 0) {
            return 0;
        }
    }
}

void kernel_input_event_enqueue_key(int key) {
    struct input_event event;
    uint32_t flags;

    if (key == 0) {
        return;
    }

    memset(&event, 0, sizeof(event));
    event.type = INPUT_EVENT_KEY;
    event.value = key;

    flags = kernel_irq_save();
    kernel_input_event_push_unlocked(&event);
    kernel_irq_restore(flags);
    kernel_waitable_signal(&g_input_event_waitable, 1u);
}

void kernel_input_event_enqueue_mouse(const struct mouse_state *state) {
    struct input_event event;
    uint32_t flags;

    if (state == 0) {
        return;
    }

    memset(&event, 0, sizeof(event));
    event.type = INPUT_EVENT_MOUSE;
    event.mouse = *state;

    flags = kernel_irq_save();
    kernel_input_event_push_unlocked(&event);
    kernel_irq_restore(flags);
    kernel_waitable_signal(&g_input_event_waitable, 1u);
}

#ifndef KERNEL_TIMER_H
#define KERNEL_TIMER_H

#include <stdint.h>
#include <kernel/process.h>

typedef void (*kernel_timer_tick_hook_t)(uint32_t tick);

/* Initialize PIT timer at given frequency (Hz) */
void kernel_timer_init(uint32_t freq_hz);

/* Get current system ticks */
uint32_t kernel_timer_get_ticks(void);

/* Register a lightweight callback that runs from the timer tick path. */
int kernel_timer_register_tick_hook(kernel_timer_tick_hook_t hook);
void kernel_timer_unregister_tick_hook(kernel_timer_tick_hook_t hook);

/* IRQ0 handler (called from assembly) */
kernel_trap_frame_t *kernel_timer_irq_handler(kernel_trap_frame_t *frame);

/* Legacy PC speaker / buzzer control */
int kernel_timer_pc_speaker_available(void);
void kernel_timer_pc_speaker_set_frequency(uint32_t freq_hz);
void kernel_timer_pc_speaker_disable(void);

#endif /* KERNEL_TIMER_H */

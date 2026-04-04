/* compatibility shim: delegate to new kernel input subsystem */

#include <stage2/include/mouse.h>

/* prototypes of the new kernel implementations */
extern void kernel_mouse_init(void);
extern int kernel_mouse_has_data(void);
extern void kernel_mouse_read(int *x, int *y, int *dx, int *dy, int *wheel, uint8_t *buttons);
extern void kernel_mouse_irq_handler(void);

int mouse_read(struct mouse_state *state) {
    if (!kernel_mouse_has_data()) {
        return 0;
    }
    
    int x = 0, y = 0;
    int dx = 0, dy = 0, wheel = 0;
    uint8_t buttons = 0;
    kernel_mouse_read(&x, &y, &dx, &dy, &wheel, &buttons);
    
    if (state != NULL) {
        state->x = x;
        state->y = y;
        state->dx = dx;
        state->dy = dy;
        state->wheel = wheel;
        state->buttons = buttons;
    }
    
    return 1;
}

/* called from kernel_asm/isr.asm on IRQ 12 */
void mouse_irq_handler_c(void) {
    kernel_mouse_irq_handler();
}

int mouse_init(void) {
    kernel_mouse_init();
    return 1;
}

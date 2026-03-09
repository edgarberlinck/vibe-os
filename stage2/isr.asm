BITS 32

global irq0_stub
global irq1_stub
global irq12_stub
global syscall_stub
extern timer_irq_handler_c
extern keyboard_irq_handler_c
extern mouse_irq_handler_c
extern syscall_dispatch_c

irq0_stub:
    pusha
    cld
    call timer_irq_handler_c
    popa
    iretd

irq1_stub:
    pusha
    cld
    call keyboard_irq_handler_c
    popa
    iretd

irq12_stub:
    pusha
    cld
    call mouse_irq_handler_c
    popa
    iretd

syscall_stub:
    pusha
    cld
    push esp
    call syscall_dispatch_c
    add esp, 4
    popa
    iretd

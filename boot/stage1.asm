BITS 16
ORG 0x7C00

%ifndef STAGE2_SECTORS
%define STAGE2_SECTORS 16
%endif

%ifndef STAGE2_LOAD_OFF
%define STAGE2_LOAD_OFF 0x7E00
%endif

%define CODE_SEG 0x08
%define DATA_SEG 0x10

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [boot_drive], dl
    call load_stage2
    jc disk_error

    mov ax, 0x0013
    int 0x10

    cli
    call enable_a20
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 0x1
    mov cr0, eax
    jmp CODE_SEG:protected_mode_start

disk_error:
    mov si, msg_disk_error
    call print_string

.halt:
    hlt
    jmp .halt

print_string:
.next_char:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .next_char
.done:
    ret

load_stage2:
    pusha

    xor ax, ax
    mov es, ax
    mov bx, STAGE2_LOAD_OFF

    xor ch, ch
    mov cl, 0x02
    xor dh, dh
    mov dl, [boot_drive]
    mov si, STAGE2_SECTORS

.read_next:
    mov ah, 0x02
    mov al, 0x01
    int 0x13
    jc .error
    cmp al, 0x01
    jne .error

    add bx, 512

    inc cl
    cmp cl, 19
    jb .continue

    mov cl, 1
    xor dh, 1
    jnz .continue
    inc ch

.continue:
    dec si
    jnz .read_next

    clc
    popa
    ret

.error:
    stc
    popa
    ret

enable_a20:
    in al, 0x92
    or al, 0x02
    out 0x92, al
    ret

boot_drive db 0
msg_disk_error db "Erro ao carregar stage2", 0

align 8
gdt_start:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

BITS 32
protected_mode_start:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x9FC00

    mov eax, STAGE2_LOAD_OFF
    jmp eax

times 510 - ($ - $$) db 0
dw 0xAA55

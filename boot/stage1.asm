BITS 16
ORG 0x7C00

    jmp short boot_start
    nop
times 90-($-$$) db 0

%define STAGE2_SEG 0x0900
%define STAGE2_OFF 0x0000
%ifndef STAGE2_START_LBA
%define STAGE2_START_LBA 1
%endif
%ifndef STAGE2_SECTORS
%define STAGE2_SECTORS 8
%endif

boot_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    mov [boot_drive], dl
    mov al, 'S'
    out 0xE9, al

    call load_stage2
    jc disk_error

    mov al, 'L'
    out 0xE9, al
    mov dl, [boot_drive]
    jmp STAGE2_SEG:STAGE2_OFF

load_stage2:
    pusha

    mov word [disk_address_packet.sectors], STAGE2_SECTORS
    mov word [disk_address_packet.offset], STAGE2_OFF
    mov word [disk_address_packet.segment], STAGE2_SEG
    mov eax, [0x7C1C]
    add eax, STAGE2_START_LBA
    mov dword [disk_address_packet.lba_low], eax
    mov dword [disk_address_packet.lba_high], 0

    mov si, disk_address_packet
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc .fail

    popa
    clc
    ret

.fail:
    popa
    stc
    ret

disk_error:
    mov al, 'E'
    out 0xE9, al
.halt:
    hlt
    jmp .halt

boot_drive db 0

disk_address_packet:
    db 16
    db 0
.sectors:
    dw STAGE2_SECTORS
.offset:
    dw STAGE2_OFF
.segment:
    dw STAGE2_SEG
.lba_low:
    dd STAGE2_START_LBA
.lba_high:
    dd 0

times 510-($-$$) db 0
dw 0xAA55

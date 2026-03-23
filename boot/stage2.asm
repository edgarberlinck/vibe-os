BITS 16
ORG 0x9000

%define STAGE2_LOAD_SEG 0x0900
%define KERNEL_LOAD_SEG 0x1000
%define KERNEL_LOAD_OFF 0x0000
%define SCRATCH_BUFFER 0x0600
%define ROOT_NAME_LEN 11

%define CODE_SEG 0x08
%define DATA_SEG 0x10

%define FAT32_END_OF_CHAIN 0x0FFFFFF8

stage2_entry:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    mov [boot_drive], dl
    mov al, '2'
    out 0xE9, al

    call parse_bpb
    call find_kernel_entry
    jc disk_error

    mov al, 'F'
    out 0xE9, al

    call load_kernel_file
    jc disk_error

    mov al, 'K'
    out 0xE9, al

    call enable_a20
    mov al, 'A'
    out 0xE9, al

    lgdt [gdt_descriptor]
    mov al, 'P'
    out 0xE9, al
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp CODE_SEG:pmode

parse_bpb:
    mov ax, [0x7C0E]
    mov [reserved_sectors], ax
    mov al, [0x7C10]
    xor ah, ah
    mov [fat_count], ax
    mov eax, [0x7C24]
    mov [fat_size_sectors], eax
    mov eax, [0x7C2C]
    mov [root_cluster], eax
    mov eax, [0x7C1C]
    mov [hidden_sectors], eax
    mov al, [0x7C0D]
    mov [sectors_per_cluster], al

    movzx eax, word [reserved_sectors]
    add eax, [hidden_sectors]
    mov [fat_start_lba], eax

    movzx eax, word [fat_count]
    mul dword [fat_size_sectors]
    add eax, [fat_start_lba]
    mov [data_start_lba], eax
    ret

cluster_to_lba:
    ; eax = cluster
    sub eax, 2
    xor edx, edx
    mov dl, [sectors_per_cluster]
    mul edx
    add eax, [data_start_lba]
    ret

read_sector_lba:
    ; eax = lba, es:bx = buffer
    mov [dap.offset], bx
    mov [dap.segment], es
    mov [dap.lba_low], eax
    mov dword [dap.lba_high], 0
    mov word [dap.sectors], 1
    push cx
    mov cx, 3
.retry:
    mov si, dap
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jnc .ok
    mov dl, [boot_drive]
    xor ah, ah
    int 0x13
    loop .retry
    pop cx
    stc
    ret
.ok:
    pop cx
    clc
    ret

find_kernel_entry:
    mov eax, [root_cluster]
.next_cluster:
    mov [current_cluster], eax
    call cluster_to_lba
    mov [cluster_lba], eax
    xor ecx, ecx
    mov cl, [sectors_per_cluster]
    xor edi, edi
.next_sector:
    push cx
    push di
    mov eax, [cluster_lba]
    add eax, edi
    xor dx, dx
    mov es, dx
    mov bx, SCRATCH_BUFFER
    call read_sector_lba
    jc .fail

    xor si, si
.scan_entry:
    mov al, [SCRATCH_BUFFER + si]
    cmp al, 0x00
    je .not_found_popped
    cmp al, 0xE5
    je .advance
    cmp byte [SCRATCH_BUFFER + si + 11], 0x0F
    je .advance

    push si
    push di
    mov di, si
    xor bx, bx
    xor dx, dx
    mov cx, ROOT_NAME_LEN
.compare_name:
    mov al, [SCRATCH_BUFFER + di]
    cmp al, [kernel_name + bx]
    jne .name_mismatch
    inc di
    inc bx
    loop .compare_name
    mov dx, 1
    jmp .name_done
.name_mismatch:
    xor dx, dx
.name_done:
    pop di
    pop si
    cmp dx, 1
    je .found

.advance:
    add si, 32
    cmp si, 512
    jb .scan_entry

    pop di
    pop cx
    inc edi
    loop .next_sector

    mov eax, [current_cluster]
    call fat_next_cluster
    jc .fail
    cmp eax, FAT32_END_OF_CHAIN
    jae .not_found
    jmp .next_cluster

.found:
    mov ax, [SCRATCH_BUFFER + si + 20]
    shl eax, 16
    mov ax, [SCRATCH_BUFFER + si + 26]
    mov [kernel_first_cluster], eax
    mov eax, [SCRATCH_BUFFER + si + 28]
    mov [kernel_file_size], eax
    pop di
    pop cx
    clc
    ret

.not_found_popped:
    pop di
    pop cx
.not_found:
    stc
    ret

.fail:
    pop di
    pop cx
    stc
    ret

fat_next_cluster:
    ; eax = cluster, returns eax = next cluster
    push ebx
    push ecx
    push edx

    mov ebx, eax
    shl ebx, 2
    mov eax, ebx
    shr eax, 9
    add eax, [fat_start_lba]
    mov [fat_sector_lba], eax
    and ebx, 511

    xor dx, dx
    mov es, dx
    mov bx, SCRATCH_BUFFER
    mov eax, [fat_sector_lba]
    call read_sector_lba
    jc .error

    cmp ebx, 509
    jb .single_sector

    xor dx, dx
    mov es, dx
    mov bx, SCRATCH_BUFFER + 512
    mov eax, [fat_sector_lba]
    inc eax
    call read_sector_lba
    jc .error

.single_sector:
    mov eax, [SCRATCH_BUFFER + ebx]
    and eax, 0x0FFFFFFF
    clc
    jmp .done

.error:
    stc
.done:
    pop edx
    pop ecx
    pop ebx
    ret

load_kernel_file:
    mov eax, [kernel_first_cluster]
    call cluster_to_lba
    mov [kernel_current_lba], eax

    mov eax, [kernel_file_size]
    add eax, 511
    shr eax, 9
    mov [kernel_remaining_sectors], eax

    mov ax, KERNEL_LOAD_SEG
    mov [kernel_segment], ax
.sector_loop:
    mov eax, [kernel_remaining_sectors]
    test eax, eax
    jz .done

    mov eax, [kernel_current_lba]
    push eax
    mov ax, [kernel_segment]
    mov es, ax
    pop eax
    mov bx, KERNEL_LOAD_OFF
    call read_sector_lba
    jc .fail

    add word [kernel_segment], 0x20
    inc dword [kernel_current_lba]
    dec dword [kernel_remaining_sectors]
    jmp .sector_loop

.done:
    clc
    ret

.fail:
    stc
    ret

enable_a20:
    in al, 0x92
    or al, 2
    out 0x92, al
    ret

disk_error:
    mov al, 'E'
    out 0xE9, al
.halt:
    hlt
    jmp .halt

align 8
gdt_start:
    dq 0
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

kernel_name db 'KERNEL  BIN'
boot_drive db 0
reserved_sectors dw 0
fat_count dw 0
fat_size_sectors dd 0
root_cluster dd 0
hidden_sectors dd 0
fat_start_lba dd 0
data_start_lba dd 0
current_cluster dd 0
cluster_lba dd 0
fat_sector_lba dd 0
kernel_first_cluster dd 0
kernel_file_size dd 0
sectors_per_cluster db 0
kernel_segment dw KERNEL_LOAD_SEG
kernel_current_lba dd 0
kernel_remaining_sectors dd 0

dap:
    db 16
    db 0
.sectors:
    dw 1
.offset:
    dw 0
.segment:
    dw 0
.lba_low:
    dd 0
.lba_high:
    dd 0

BITS 32
pmode:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x70000
    jmp CODE_SEG:0x10000

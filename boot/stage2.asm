BITS 16
ORG 0x9000

%define STAGE2_LOAD_SEG 0x0900
%define KERNEL_LOAD_SEG 0x1000
%define KERNEL_LOAD_OFF 0x0000
%define LEGACY_VESA_ADDR 0x0500
%define SCRATCH_BUFFER 0x0600
%define BOOTINFO_ADDR 0x8000
%define BOOTINFO_FLAGS 8
%define BOOTINFO_FLAG_VESA_VALID 0x00000001
%define BOOTINFO_FLAG_MEMINFO_VALID 0x00000002
%define BOOTINFO_VESA_MODE 16
%define BOOTINFO_VESA_FB 20
%define BOOTINFO_VESA_PITCH 24
%define BOOTINFO_VESA_WIDTH 26
%define BOOTINFO_VESA_HEIGHT 28
%define BOOTINFO_VESA_BPP 30
%define BOOTINFO_MEM_LARGEST_BASE 32
%define BOOTINFO_MEM_LARGEST_SIZE 36
%define BOOTINFO_MEM_LARGEST_END 40
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

    call detect_memory
    mov al, 'M'
    out 0xE9, al
    call populate_legacy_vesa_info

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

detect_memory:
    and dword [BOOTINFO_ADDR + 8], ~BOOTINFO_FLAG_MEMINFO_VALID
    mov dword [BOOTINFO_ADDR + BOOTINFO_MEM_LARGEST_BASE], 0
    mov dword [BOOTINFO_ADDR + BOOTINFO_MEM_LARGEST_SIZE], 0
    mov dword [BOOTINFO_ADDR + BOOTINFO_MEM_LARGEST_END], 0
    xor ebx, ebx

detect_memory_e820_loop:
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24
    mov di, SCRATCH_BUFFER
    mov dword [SCRATCH_BUFFER + 20], 1
    int 0x15
    jc detect_memory_e820_done
    cmp eax, 0x534D4150
    jne detect_memory_e820_done
    cmp dword [SCRATCH_BUFFER + 16], 1
    jne detect_memory_e820_next
    cmp dword [SCRATCH_BUFFER + 4], 0
    jne detect_memory_e820_next
    cmp dword [SCRATCH_BUFFER + 12], 0
    jne detect_memory_e820_next

    mov eax, [SCRATCH_BUFFER + 0]
    cmp eax, 0x00100000
    jae detect_memory_e820_base_ok
    mov eax, 0x00100000
detect_memory_e820_base_ok:
    mov edx, [SCRATCH_BUFFER + 0]
    add edx, [SCRATCH_BUFFER + 8]
    jc detect_memory_e820_clamp_end
    jmp detect_memory_e820_end_ok
detect_memory_e820_clamp_end:
    mov edx, 0xFFFFF000
detect_memory_e820_end_ok:
    cmp edx, eax
    jbe detect_memory_e820_next
    mov ecx, edx
    sub ecx, eax
    cmp ecx, [BOOTINFO_ADDR + BOOTINFO_MEM_LARGEST_SIZE]
    jbe detect_memory_e820_next
    mov [BOOTINFO_ADDR + BOOTINFO_MEM_LARGEST_BASE], eax
    mov [BOOTINFO_ADDR + BOOTINFO_MEM_LARGEST_SIZE], ecx
    mov [BOOTINFO_ADDR + BOOTINFO_MEM_LARGEST_END], edx

detect_memory_e820_next:
    test ebx, ebx
    jnz detect_memory_e820_loop

detect_memory_e820_done:
    cmp dword [BOOTINFO_ADDR + BOOTINFO_MEM_LARGEST_SIZE], 0
    jne detect_memory_store_bootinfo

    mov ax, 0xE801
    int 0x15
    jc detect_memory_fallback_88

    test ax, ax
    jnz detect_memory_have_low_kb
    mov ax, cx
detect_memory_have_low_kb:
    test bx, bx
    jnz detect_memory_have_high_blocks
    mov bx, dx
detect_memory_have_high_blocks:
    movzx eax, ax
    shl eax, 10
    movzx edx, bx
    shl edx, 16
    add eax, edx
    jmp detect_memory_store

detect_memory_fallback_88:
    mov ah, 0x88
    int 0x15
    jc detect_memory_done
    movzx eax, ax
    shl eax, 10

detect_memory_store:
    test eax, eax
    jz detect_memory_done
    mov dword [BOOTINFO_ADDR + BOOTINFO_MEM_LARGEST_BASE], 0x00100000
    mov [BOOTINFO_ADDR + BOOTINFO_MEM_LARGEST_SIZE], eax
    mov edx, eax
    add edx, 0x00100000
    mov [BOOTINFO_ADDR + BOOTINFO_MEM_LARGEST_END], edx
detect_memory_store_bootinfo:
    or dword [BOOTINFO_ADDR + 8], BOOTINFO_FLAG_MEMINFO_VALID
detect_memory_done:
    ret

populate_legacy_vesa_info:
    xor ax, ax
    mov es, ax
    mov di, LEGACY_VESA_ADDR
    mov cx, 7
    rep stosw

    test dword [BOOTINFO_ADDR + BOOTINFO_FLAGS], BOOTINFO_FLAG_VESA_VALID
    jz .done

    mov ax, [BOOTINFO_ADDR + BOOTINFO_VESA_MODE]
    mov [LEGACY_VESA_ADDR + 0], ax
    mov eax, [BOOTINFO_ADDR + BOOTINFO_VESA_FB]
    mov [LEGACY_VESA_ADDR + 2], eax
    mov ax, [BOOTINFO_ADDR + BOOTINFO_VESA_PITCH]
    mov [LEGACY_VESA_ADDR + 6], ax
    mov ax, [BOOTINFO_ADDR + BOOTINFO_VESA_WIDTH]
    mov [LEGACY_VESA_ADDR + 8], ax
    mov ax, [BOOTINFO_ADDR + BOOTINFO_VESA_HEIGHT]
    mov [LEGACY_VESA_ADDR + 10], ax
    mov al, [BOOTINFO_ADDR + BOOTINFO_VESA_BPP]
    mov [LEGACY_VESA_ADDR + 12], al

.done:
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

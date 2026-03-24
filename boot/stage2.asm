BITS 16
ORG 0x9000

%define KERNEL_LOAD_SEG 0x1000
%define REALMODE_STACK_TOP 0xC000
%define BACKGROUND_LOAD_SEG 0x0200
%define BACKGROUND_DRAW_ADDR 0x00002000
%define BACKGROUND_SOURCE_WIDTH 192
%define BACKGROUND_SOURCE_HEIGHT 144
%define BACKGROUND_DRAW_BYTES (BACKGROUND_SOURCE_WIDTH * BACKGROUND_SOURCE_HEIGHT)
%define BACKGROUND_BUFFER_CAP (((BACKGROUND_DRAW_BYTES + 511) / 512) * 512)
%define LEGACY_VESA_ADDR 0x0500
%define SCRATCH_BUFFER 0x0600
%define VBE_INFO_ADDR 0x0A00
%define VBE_MODE_INFO_ADDR 0x0C00
%define BOOTINFO_ADDR 0x8D00
%define BOOTINFO_FLAGS 8
%define BOOTINFO_FLAG_VESA_VALID 0x00000001
%define BOOTINFO_FLAG_MEMINFO_VALID 0x00000002
%define BOOTINFO_FLAG_BOOT_TO_DESKTOP 0x00010000
%define BOOTINFO_FLAG_BOOT_SAFE_MODE 0x00020000
%define BOOTINFO_FLAG_BOOT_RESCUE_SHELL 0x00040000
%define BOOTINFO_FLAG_BOOT_MODE_MASK (BOOTINFO_FLAG_BOOT_TO_DESKTOP | BOOTINFO_FLAG_BOOT_SAFE_MODE | BOOTINFO_FLAG_BOOT_RESCUE_SHELL)
%define BOOTINFO_VESA_MODE 16
%define BOOTINFO_VESA_FB 20
%define BOOTINFO_VESA_PITCH 24
%define BOOTINFO_VESA_WIDTH 26
%define BOOTINFO_VESA_HEIGHT 28
%define BOOTINFO_VESA_BPP 30
%define BOOTINFO_MEM_LARGEST_BASE 32
%define BOOTINFO_MEM_LARGEST_SIZE 36
%define BOOTINFO_MEM_LARGEST_END 40
%define BOOTINFO_VIDEO_MODE_COUNT 64
%define BOOTINFO_VIDEO_ACTIVE_INDEX 65
%define BOOTINFO_VIDEO_FALLBACK_INDEX 66
%define BOOTINFO_VIDEO_SELECTED_INDEX 67
%define BOOTINFO_VIDEO_MODES 68
%define BOOTINFO_VIDEO_MODE_SIZE 8
%define BOOTINFO_VIDEO_MODE_FIELD_MODE 0
%define BOOTINFO_VIDEO_MODE_FIELD_WIDTH 2
%define BOOTINFO_VIDEO_MODE_FIELD_HEIGHT 4
%define BOOTINFO_VIDEO_MODE_FIELD_BPP 6
%define BOOTINFO_MAX_VESA_MODES 8
%define BOOTINFO_VIDEO_INDEX_NONE 0xFF
%define ROOT_NAME_LEN 11

%define CODE_SEG 0x08
%define DATA_SEG 0x10
%define CODE16_SEG 0x18
%define DATA16_SEG 0x20

%define FAT32_END_OF_CHAIN 0x0FFFFFF8
%define VBE_LFB_ENABLE 0x4000
%define MENU_ENTRY_COUNT 3
%define MENU_TIMEOUT_SECONDS 5
%define PIT_COUNTS_PER_SECOND 1193182
%define MENU_TIMEOUT_COUNTS (PIT_COUNTS_PER_SECOND * MENU_TIMEOUT_SECONDS)

%macro DEBUG_CHAR 1
    mov al, %1
    mov [debug_last_code], al
%ifdef VIBELOADER_DEBUG_TRACE
    out 0xE9, al
%endif
%endmacro

stage2_entry:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, REALMODE_STACK_TOP
    cld

    DEBUG_CHAR '2'
    mov [boot_drive], dl
    call parse_bpb

    mov si, kernel_name
    call find_root_entry
    jc disk_error

    DEBUG_CHAR 'F'
    mov word [load_segment], KERNEL_LOAD_SEG
    call load_file_to_memory
    jc disk_error

    DEBUG_CHAR 'K'
    call load_optional_background
    DEBUG_CHAR 'V'
    call detect_vesa_modes
    DEBUG_CHAR 'B'
    call detect_memory
    call populate_legacy_vesa_info
    call enable_a20

    DEBUG_CHAR 'P'
    lgdt [gdt_descriptor]
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

find_root_entry:
    mov [search_name_ptr], si
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
    mov bx, [search_name_ptr]
    mov di, si
    mov cx, ROOT_NAME_LEN
.compare_name:
    mov al, [SCRATCH_BUFFER + di]
    cmp al, [bx]
    jne .name_mismatch
    inc di
    inc bx
    loop .compare_name
    pop si
    jmp .found
.name_mismatch:
    pop si

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
    jc .fail_no_pop
    cmp eax, FAT32_END_OF_CHAIN
    jae .not_found
    jmp .next_cluster

.found:
    xor eax, eax
    mov ax, [SCRATCH_BUFFER + si + 20]
    shl eax, 16
    mov ax, [SCRATCH_BUFFER + si + 26]
    mov [file_first_cluster], eax
    mov eax, [SCRATCH_BUFFER + si + 28]
    mov [file_size], eax
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
.fail_no_pop:
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
    mov [fat_entry_offset], ebx

    xor dx, dx
    mov es, dx
    mov bx, SCRATCH_BUFFER
    mov eax, [fat_sector_lba]
    call read_sector_lba
    jc .error

    mov ebx, [fat_entry_offset]
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
    mov ebx, [fat_entry_offset]
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

load_file_to_memory:
    mov eax, [file_size]
    add eax, 511
    shr eax, 9
    mov [file_remaining_sectors], eax
    mov eax, [file_first_cluster]

.cluster_loop:
    cmp dword [file_remaining_sectors], 0
    je .done
    cmp eax, 2
    jb .fail

    mov [current_cluster], eax
    call cluster_to_lba
    mov [cluster_lba], eax

    xor ecx, ecx
    mov cl, [sectors_per_cluster]
    xor edi, edi
.sector_loop:
    cmp dword [file_remaining_sectors], 0
    je .done

    push cx
    push di
    mov eax, [cluster_lba]
    add eax, edi
    push eax
    mov ax, [load_segment]
    mov es, ax
    pop eax
    xor bx, bx
    call read_sector_lba
    jc .read_fail

    add word [load_segment], 0x20
    dec dword [file_remaining_sectors]
    pop di
    pop cx
    inc edi
    loop .sector_loop

    cmp dword [file_remaining_sectors], 0
    je .done

    mov eax, [current_cluster]
    call fat_next_cluster
    jc .fail
    cmp eax, FAT32_END_OF_CHAIN
    jae .fail
    jmp .cluster_loop

.read_fail:
    pop di
    pop cx
.fail:
    stc
    ret

.done:
    clc
    ret

load_optional_background:
    mov byte [background_available], 0
    mov si, background_name
    call find_root_entry
    jc .done

    mov eax, [file_size]
    cmp eax, BACKGROUND_DRAW_BYTES
    jne .done
    add eax, 511
    and eax, ~511
    cmp eax, BACKGROUND_BUFFER_CAP
    ja .done

    mov word [load_segment], BACKGROUND_LOAD_SEG
    call load_file_to_memory
    jc .done

    mov byte [background_available], 1
.done:
    clc
    ret

detect_memory:
    and dword [BOOTINFO_ADDR + BOOTINFO_FLAGS], ~BOOTINFO_FLAG_MEMINFO_VALID
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
    or dword [BOOTINFO_ADDR + BOOTINFO_FLAGS], BOOTINFO_FLAG_MEMINFO_VALID
detect_memory_done:
    ret

clear_video_catalog:
    push ax
    push cx
    push di

    xor ax, ax
    mov es, ax
    mov di, BOOTINFO_ADDR + BOOTINFO_VIDEO_MODE_COUNT
    mov cx, 34
    cld
    rep stosw
    mov byte [BOOTINFO_ADDR + BOOTINFO_VIDEO_ACTIVE_INDEX], BOOTINFO_VIDEO_INDEX_NONE
    mov byte [BOOTINFO_ADDR + BOOTINFO_VIDEO_FALLBACK_INDEX], BOOTINFO_VIDEO_INDEX_NONE
    mov byte [BOOTINFO_ADDR + BOOTINFO_VIDEO_SELECTED_INDEX], BOOTINFO_VIDEO_INDEX_NONE

    pop di
    pop cx
    pop ax
    ret

detect_vesa_modes:
    call clear_video_catalog

    xor ax, ax
    mov es, ax
    mov di, VBE_INFO_ADDR
    mov cx, 256
    cld
    rep stosw
    mov di, VBE_INFO_ADDR
    mov dword [VBE_INFO_ADDR], 'VBE2'
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne .after_enum

    push ds
    mov ax, [VBE_INFO_ADDR + 0x10]
    mov ds, ax
    mov si, [VBE_INFO_ADDR + 0x0E]
    cld
.enum_loop:
    lodsw
    cmp ax, 0xFFFF
    je .enum_done
    mov dx, ax
    push si
    call catalog_try_add_mode
    pop si
    jmp .enum_loop
.enum_done:
    pop ds

.after_enum:
    test dword [BOOTINFO_ADDR + BOOTINFO_FLAGS], BOOTINFO_FLAG_VESA_VALID
    jz .finalize
    mov dx, [BOOTINFO_ADDR + BOOTINFO_VESA_MODE]
    call catalog_try_add_mode

.finalize:
    call catalog_finalize_selection
    call ensure_boot_video_mode
    ret

catalog_try_add_mode:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    call vesa_query_mode_info
    jc .done

    xor cx, cx
    mov cl, [BOOTINFO_ADDR + BOOTINFO_VIDEO_MODE_COUNT]
    xor si, si
.scan_existing:
    cmp si, cx
    jae .append
    mov di, si
    shl di, 3
    add di, BOOTINFO_ADDR + BOOTINFO_VIDEO_MODES

    mov ax, [VBE_MODE_INFO_ADDR + 0x12]
    cmp [di + BOOTINFO_VIDEO_MODE_FIELD_WIDTH], ax
    jne .next_existing
    mov ax, [VBE_MODE_INFO_ADDR + 0x14]
    cmp [di + BOOTINFO_VIDEO_MODE_FIELD_HEIGHT], ax
    jne .next_existing
    mov al, [VBE_MODE_INFO_ADDR + 0x19]
    cmp [di + BOOTINFO_VIDEO_MODE_FIELD_BPP], al
    jne .next_existing

    mov ax, [BOOTINFO_ADDR + BOOTINFO_VESA_MODE]
    cmp dx, ax
    jne .maybe_mark_fallback
    mov [di + BOOTINFO_VIDEO_MODE_FIELD_MODE], dx

.maybe_mark_fallback:
    cmp word [VBE_MODE_INFO_ADDR + 0x12], 640
    jne .done
    cmp word [VBE_MODE_INFO_ADDR + 0x14], 480
    jne .done
    cmp byte [BOOTINFO_ADDR + BOOTINFO_VIDEO_FALLBACK_INDEX], BOOTINFO_VIDEO_INDEX_NONE
    jne .done
    mov ax, si
    mov [BOOTINFO_ADDR + BOOTINFO_VIDEO_FALLBACK_INDEX], al
    jmp .done

.next_existing:
    inc si
    jmp .scan_existing

.append:
    cmp byte [BOOTINFO_ADDR + BOOTINFO_VIDEO_MODE_COUNT], BOOTINFO_MAX_VESA_MODES
    jae .done

    xor bx, bx
    mov bl, [BOOTINFO_ADDR + BOOTINFO_VIDEO_MODE_COUNT]
    mov di, bx
    shl di, 3
    add di, BOOTINFO_ADDR + BOOTINFO_VIDEO_MODES

    mov [di + BOOTINFO_VIDEO_MODE_FIELD_MODE], dx
    mov ax, [VBE_MODE_INFO_ADDR + 0x12]
    mov [di + BOOTINFO_VIDEO_MODE_FIELD_WIDTH], ax
    mov ax, [VBE_MODE_INFO_ADDR + 0x14]
    mov [di + BOOTINFO_VIDEO_MODE_FIELD_HEIGHT], ax
    mov al, [VBE_MODE_INFO_ADDR + 0x19]
    mov [di + BOOTINFO_VIDEO_MODE_FIELD_BPP], al
    mov byte [di + BOOTINFO_VIDEO_MODE_FIELD_BPP + 1], 0

    mov al, [BOOTINFO_ADDR + BOOTINFO_VIDEO_MODE_COUNT]
    inc byte [BOOTINFO_ADDR + BOOTINFO_VIDEO_MODE_COUNT]

    cmp word [VBE_MODE_INFO_ADDR + 0x12], 640
    jne .done
    cmp word [VBE_MODE_INFO_ADDR + 0x14], 480
    jne .done
    mov [BOOTINFO_ADDR + BOOTINFO_VIDEO_FALLBACK_INDEX], al

.done:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

catalog_find_mode_index_by_mode:
    push bx
    push cx
    push di

    xor cx, cx
    mov cl, [BOOTINFO_ADDR + BOOTINFO_VIDEO_MODE_COUNT]
    xor bx, bx
.loop:
    cmp bx, cx
    jae .not_found
    mov di, bx
    shl di, 3
    add di, BOOTINFO_ADDR + BOOTINFO_VIDEO_MODES
    cmp [di + BOOTINFO_VIDEO_MODE_FIELD_MODE], dx
    je .found
    inc bx
    jmp .loop

.found:
    mov ax, bx
    jmp .done

.not_found:
    mov al, BOOTINFO_VIDEO_INDEX_NONE

.done:
    pop di
    pop cx
    pop bx
    ret

catalog_finalize_selection:
    mov byte [BOOTINFO_ADDR + BOOTINFO_VIDEO_ACTIVE_INDEX], BOOTINFO_VIDEO_INDEX_NONE
    mov byte [BOOTINFO_ADDR + BOOTINFO_VIDEO_SELECTED_INDEX], BOOTINFO_VIDEO_INDEX_NONE

    test dword [BOOTINFO_ADDR + BOOTINFO_FLAGS], BOOTINFO_FLAG_VESA_VALID
    jz .select_default
    cmp byte [BOOTINFO_ADDR + BOOTINFO_VESA_BPP], 8
    jne .select_default

    mov dx, [BOOTINFO_ADDR + BOOTINFO_VESA_MODE]
    call catalog_find_mode_index_by_mode
    cmp al, BOOTINFO_VIDEO_INDEX_NONE
    je .select_default
    mov [BOOTINFO_ADDR + BOOTINFO_VIDEO_ACTIVE_INDEX], al
    mov [BOOTINFO_ADDR + BOOTINFO_VIDEO_SELECTED_INDEX], al
    ret

.select_default:
    mov al, [BOOTINFO_ADDR + BOOTINFO_VIDEO_FALLBACK_INDEX]
    cmp al, BOOTINFO_VIDEO_INDEX_NONE
    jne .store_selected
    cmp byte [BOOTINFO_ADDR + BOOTINFO_VIDEO_MODE_COUNT], 0
    je .done
    xor al, al
.store_selected:
    mov [BOOTINFO_ADDR + BOOTINFO_VIDEO_SELECTED_INDEX], al
.done:
    ret

ensure_boot_video_mode:
    test dword [BOOTINFO_ADDR + BOOTINFO_FLAGS], BOOTINFO_FLAG_VESA_VALID
    jz .apply_selected
    cmp byte [BOOTINFO_ADDR + BOOTINFO_VESA_BPP], 8
    jne .apply_selected
    cmp byte [BOOTINFO_ADDR + BOOTINFO_VIDEO_ACTIVE_INDEX], BOOTINFO_VIDEO_INDEX_NONE
    je .sync_selected
    ret

.sync_selected:
    mov al, [BOOTINFO_ADDR + BOOTINFO_VIDEO_SELECTED_INDEX]
    cmp al, BOOTINFO_VIDEO_INDEX_NONE
    je .apply_selected
    mov [BOOTINFO_ADDR + BOOTINFO_VIDEO_ACTIVE_INDEX], al
    ret

.apply_selected:
    mov al, [BOOTINFO_ADDR + BOOTINFO_VIDEO_SELECTED_INDEX]
    cmp al, BOOTINFO_VIDEO_INDEX_NONE
    jne .have_selected
    cmp byte [BOOTINFO_ADDR + BOOTINFO_VIDEO_MODE_COUNT], 0
    je .done
    xor al, al
    mov [BOOTINFO_ADDR + BOOTINFO_VIDEO_SELECTED_INDEX], al

.have_selected:
    xor bx, bx
    mov bl, [BOOTINFO_ADDR + BOOTINFO_VIDEO_SELECTED_INDEX]
    shl bx, 3
    mov dx, [BOOTINFO_ADDR + BOOTINFO_VIDEO_MODES + bx + BOOTINFO_VIDEO_MODE_FIELD_MODE]
    call vesa_set_mode_and_store_bootinfo
.done:
    ret

vesa_query_mode_info:
    push ax
    push bx
    push cx
    push di

    xor ax, ax
    mov es, ax
    mov di, VBE_MODE_INFO_ADDR
    mov cx, 128
    cld
    rep stosw
    mov di, VBE_MODE_INFO_ADDR
    mov ax, 0x4F01
    mov cx, dx
    int 0x10
    cmp ax, 0x004F
    jne .fail

    test word [VBE_MODE_INFO_ADDR + 0], 0x0001
    jz .fail
    test word [VBE_MODE_INFO_ADDR + 0], 0x0010
    jz .fail
    test word [VBE_MODE_INFO_ADDR + 0], 0x0080
    jz .fail
    cmp byte [VBE_MODE_INFO_ADDR + 0x19], 8
    jne .fail
    cmp byte [VBE_MODE_INFO_ADDR + 0x1B], 4
    jne .fail
    cmp word [VBE_MODE_INFO_ADDR + 0x12], 640
    jb .fail
    cmp word [VBE_MODE_INFO_ADDR + 0x14], 480
    jb .fail
    cmp word [VBE_MODE_INFO_ADDR + 0x10], 0
    je .fail
    cmp dword [VBE_MODE_INFO_ADDR + 0x28], 0
    je .fail

    pop di
    pop cx
    pop bx
    pop ax
    clc
    ret

.fail:
    pop di
    pop cx
    pop bx
    pop ax
    stc
    ret

vesa_store_bootinfo_from_mode_info:
    mov [BOOTINFO_ADDR + BOOTINFO_VESA_MODE], dx
    mov eax, [VBE_MODE_INFO_ADDR + 0x28]
    mov [BOOTINFO_ADDR + BOOTINFO_VESA_FB], eax
    mov ax, [VBE_MODE_INFO_ADDR + 0x10]
    mov [BOOTINFO_ADDR + BOOTINFO_VESA_PITCH], ax
    mov ax, [VBE_MODE_INFO_ADDR + 0x12]
    mov [BOOTINFO_ADDR + BOOTINFO_VESA_WIDTH], ax
    mov ax, [VBE_MODE_INFO_ADDR + 0x14]
    mov [BOOTINFO_ADDR + BOOTINFO_VESA_HEIGHT], ax
    mov al, [VBE_MODE_INFO_ADDR + 0x19]
    mov [BOOTINFO_ADDR + BOOTINFO_VESA_BPP], al
    or dword [BOOTINFO_ADDR + BOOTINFO_FLAGS], BOOTINFO_FLAG_VESA_VALID
    ret

vesa_set_mode_and_store_bootinfo:
    push ax
    push bx

    call vesa_query_mode_info
    jc .fail

    mov ax, 0x4F02
    mov bx, dx
    or bx, VBE_LFB_ENABLE
    int 0x10
    cmp ax, 0x004F
    jne .fail

    call vesa_query_mode_info
    jc .fail
    call vesa_store_bootinfo_from_mode_info
    call catalog_find_mode_index_by_mode
    cmp al, BOOTINFO_VIDEO_INDEX_NONE
    je .success
    mov [BOOTINFO_ADDR + BOOTINFO_VIDEO_ACTIVE_INDEX], al
    mov [BOOTINFO_ADDR + BOOTINFO_VIDEO_SELECTED_INDEX], al

.success:
    pop bx
    pop ax
    clc
    ret

.fail:
    pop bx
    pop ax
    stc
    ret

populate_legacy_vesa_info:
    xor ax, ax
    mov es, ax
    mov di, LEGACY_VESA_ADDR
    mov cx, 7
    cld
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
    DEBUG_CHAR 'X'
    mov ax, 0x0003
    int 0x10
    mov si, disk_error_msg
    call print_string
    mov al, [debug_last_code]
    call print_char
    mov al, 0x0D
    call print_char
    mov al, 0x0A
    call print_char
.halt:
    hlt
    jmp .halt

print_string:
    cld
.next:
    lodsb
    test al, al
    jz .done
    call print_char
    jmp .next
.done:
    ret

print_char:
    push bx
    mov ah, 0x0E
    mov bh, 0x00
    mov bl, 0x07
    int 0x10
    pop bx
    ret

align 8
gdt_start:
    dq 0
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
    dq 0x00009A000000FFFF
    dq 0x000092000000FFFF
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

kernel_name db 'KERNEL  BIN'
background_name db 'VIBEBG  BIN'
disk_error_msg db 'VIBELOADER ERROR ', 0
boot_drive db 0
background_available db 0
debug_last_code db '?'
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
fat_entry_offset dd 0
file_first_cluster dd 0
file_size dd 0
file_remaining_sectors dd 0
sectors_per_cluster db 0
load_segment dw KERNEL_LOAD_SEG
search_name_ptr dw 0

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
    cld

    mov byte [menu_initialized], 0
    DEBUG_CHAR 'Q'
    call vibeloader_menu
    DEBUG_CHAR 'G'
    jmp CODE_SEG:0x10000

vibeloader_menu:
    DEBUG_CHAR 'M'
    test dword [BOOTINFO_ADDR + BOOTINFO_FLAGS], BOOTINFO_FLAG_VESA_VALID
    jz vibeloader_menu_default_boot
    cmp byte [BOOTINFO_ADDR + BOOTINFO_VESA_BPP], 8
    jne vibeloader_menu_default_boot

    call set_desktop_palette
    DEBUG_CHAR 'C'
    call menu_compute_layout
    cmp byte [menu_initialized], 0
    jne vibeloader_menu_resume
    mov dword [menu_selection], 0
    mov byte [menu_initialized], 1
    call menu_restart_timer
    mov byte [menu_dirty], 1

vibeloader_menu_resume:
    mov byte [menu_dirty], 1
vibeloader_menu_loop:
    cmp byte [menu_timeout_paused], 0
    jne vibeloader_menu_check_keys
    call pit_read_counter
    movzx ecx, word [menu_prev_pit]
    movzx edx, ax
    mov [menu_prev_pit], ax
    sub ecx, edx
    and ecx, 0xFFFF
    add dword [menu_elapsed_counts], ecx

    mov eax, [menu_elapsed_counts]
    cmp eax, MENU_TIMEOUT_COUNTS
    jae vibeloader_menu_boot_now

    xor edx, edx
    mov ecx, PIT_COUNTS_PER_SECOND
    div ecx
    mov ebx, MENU_TIMEOUT_SECONDS
    sub ebx, eax
    cmp ebx, [menu_seconds_left]
    je vibeloader_menu_check_keys
    mov [menu_seconds_left], ebx
    mov byte [menu_dirty], 1

vibeloader_menu_check_keys:
    call menu_poll_keyboard
    cmp eax, 3
    je vibeloader_menu_change_video
    cmp eax, 2
    je vibeloader_menu_boot_now
    cmp eax, 1
    jne vibeloader_menu_maybe_draw
    mov byte [menu_dirty], 1

vibeloader_menu_maybe_draw:
    cmp byte [menu_dirty], 0
    je vibeloader_menu_loop
    DEBUG_CHAR 'R'
    call menu_render
    DEBUG_CHAR 'r'
    mov byte [menu_dirty], 0
    jmp vibeloader_menu_loop

vibeloader_menu_change_video:
    DEBUG_CHAR 'v'
    jmp pmode_to_real_for_video_change

vibeloader_menu_boot_now:
    DEBUG_CHAR 'g'
    call menu_apply_selection
    ret

vibeloader_menu_default_boot:
    DEBUG_CHAR 'D'
    mov dword [menu_base_x], 0
    mov dword [menu_base_y], 0
    mov dword [menu_selection], 0
    call menu_apply_selection
    ret

menu_compute_layout:
    xor eax, eax
    mov ax, [BOOTINFO_ADDR + BOOTINFO_VESA_WIDTH]
    cmp eax, 640
    jbe .width_done
    sub eax, 640
    shr eax, 1
    jmp .store_width
.width_done:
    xor eax, eax
.store_width:
    mov [menu_base_x], eax

    xor eax, eax
    mov ax, [BOOTINFO_ADDR + BOOTINFO_VESA_HEIGHT]
    cmp eax, 480
    jbe .height_done
    sub eax, 480
    shr eax, 1
    jmp .store_height
.height_done:
    xor eax, eax
.store_height:
    mov [menu_base_y], eax

    xor eax, eax
    mov ax, [BOOTINFO_ADDR + BOOTINFO_VESA_WIDTH]
    xor edx, edx
    mov ecx, BACKGROUND_SOURCE_WIDTH
    div ecx
    mov ebx, eax

    xor eax, eax
    mov ax, [BOOTINFO_ADDR + BOOTINFO_VESA_HEIGHT]
    xor edx, edx
    mov ecx, BACKGROUND_SOURCE_HEIGHT
    div ecx
    cmp eax, ebx
    jae .background_scale_ready
    mov ebx, eax

.background_scale_ready:
    test ebx, ebx
    jnz .store_background_scale
    mov ebx, 1

.store_background_scale:
    mov [background_scale], ebx

    xor eax, eax
    mov ax, [BOOTINFO_ADDR + BOOTINFO_VESA_WIDTH]
    mov ecx, BACKGROUND_SOURCE_WIDTH
    imul ecx, ebx
    sub eax, ecx
    shr eax, 1
    mov [background_base_x], eax

    xor eax, eax
    mov ax, [BOOTINFO_ADDR + BOOTINFO_VESA_HEIGHT]
    mov ecx, BACKGROUND_SOURCE_HEIGHT
    imul ecx, ebx
    sub eax, ecx
    shr eax, 1
    mov [background_base_y], eax
    ret

menu_restart_timer:
    call pit_read_counter
    mov [menu_prev_pit], ax
    mov dword [menu_elapsed_counts], 0
    mov dword [menu_seconds_left], MENU_TIMEOUT_SECONDS
    mov byte [menu_timeout_paused], 0
    ret

pit_read_counter:
    xor al, al
    out 0x43, al
    in al, 0x40
    mov ah, al
    in al, 0x40
    xchg al, ah
    ret

menu_poll_keyboard:
    xor edx, edx
.poll:
    in al, 0x64
    test al, 1
    jz .done

    in al, 0x60
    cmp al, 0xE0
    jne .not_extended
    mov byte [menu_extended], 1
    jmp .poll

.not_extended:
    mov bl, [menu_extended]
    mov byte [menu_extended], 0
    test al, 0x80
    jnz .poll

    cmp byte [menu_timeout_paused], 0
    jne .classify
    mov byte [menu_timeout_paused], 1
    mov edx, 1

.classify:
    cmp bl, 0
    jne .handle_extended

    cmp al, 0x11
    je .move_up
    cmp al, 0x1F
    je .move_down
    cmp al, 0x1C
    je .confirm
    jmp .poll

.handle_extended:
    cmp al, 0x48
    je .move_up
    cmp al, 0x50
    je .move_down
    cmp al, 0x4B
    je .video_prev
    cmp al, 0x4D
    je .video_next
    cmp al, 0x1C
    je .confirm
    jmp .poll

.move_up:
    mov eax, [menu_selection]
    test eax, eax
    jz .wrap_up
    dec eax
    jmp .store_selection
.wrap_up:
    mov eax, MENU_ENTRY_COUNT - 1
.store_selection:
    mov [menu_selection], eax
    mov eax, 1
    ret

.move_down:
    mov eax, [menu_selection]
    inc eax
    cmp eax, MENU_ENTRY_COUNT
    jb .store_down
    xor eax, eax
.store_down:
    mov [menu_selection], eax
    mov eax, 1
    ret

.video_prev:
    call menu_select_previous_video_mode
    cmp eax, 0
    je .poll
    mov eax, 3
    ret

.video_next:
    call menu_select_next_video_mode
    cmp eax, 0
    je .poll
    mov eax, 3
    ret

.confirm:
    mov eax, 2
    ret

.done:
    mov eax, edx
    ret

menu_select_previous_video_mode:
    xor ecx, ecx
    mov cl, [BOOTINFO_ADDR + BOOTINFO_VIDEO_MODE_COUNT]
    cmp ecx, 1
    jbe .no_change

    movzx eax, byte [BOOTINFO_ADDR + BOOTINFO_VIDEO_SELECTED_INDEX]
    cmp al, BOOTINFO_VIDEO_INDEX_NONE
    jne .have_current
    xor eax, eax
    jmp .store

.have_current:
    test eax, eax
    jnz .decrement
    mov eax, ecx
    dec eax
    jmp .store

.decrement:
    dec eax

.store:
    mov [BOOTINFO_ADDR + BOOTINFO_VIDEO_SELECTED_INDEX], al
    mov eax, 1
    ret

.no_change:
    xor eax, eax
    ret

menu_select_next_video_mode:
    xor ecx, ecx
    mov cl, [BOOTINFO_ADDR + BOOTINFO_VIDEO_MODE_COUNT]
    cmp ecx, 1
    jbe .no_change

    movzx eax, byte [BOOTINFO_ADDR + BOOTINFO_VIDEO_SELECTED_INDEX]
    cmp al, BOOTINFO_VIDEO_INDEX_NONE
    jne .have_current
    xor eax, eax
    jmp .store

.have_current:
    inc eax
    cmp eax, ecx
    jb .store
    xor eax, eax

.store:
    mov [BOOTINFO_ADDR + BOOTINFO_VIDEO_SELECTED_INDEX], al
    mov eax, 1
    ret

.no_change:
    xor eax, eax
    ret

menu_apply_selection:
    mov eax, [BOOTINFO_ADDR + BOOTINFO_FLAGS]
    and eax, ~BOOTINFO_FLAG_BOOT_MODE_MASK
    cmp dword [menu_selection], 1
    je .safe_mode
    cmp dword [menu_selection], 2
    je .rescue_shell
    or eax, BOOTINFO_FLAG_BOOT_TO_DESKTOP
    jmp .store
.safe_mode:
    or eax, BOOTINFO_FLAG_BOOT_SAFE_MODE
    jmp .store
.rescue_shell:
    or eax, BOOTINFO_FLAG_BOOT_RESCUE_SHELL
.store:
    mov [BOOTINFO_ADDR + BOOTINFO_FLAGS], eax
    ret

menu_render:
    call draw_background

    mov eax, 148
    add eax, [menu_base_x]
    mov ebx, 104
    add ebx, [menu_base_y]
    mov ecx, 344
    mov esi, 272
    mov dl, 1
    call draw_rect

    mov eax, 152
    add eax, [menu_base_x]
    mov ebx, 108
    add ebx, [menu_base_y]
    mov ecx, 336
    mov esi, 264
    mov dl, 8
    call draw_rect

    mov eax, 152
    add eax, [menu_base_x]
    mov ebx, 108
    add ebx, [menu_base_y]
    mov ecx, 336
    mov esi, 24
    mov dl, 3
    call draw_rect

    mov esi, vibeloader_title
    mov eax, 230
    add eax, [menu_base_x]
    mov ebx, 121
    add ebx, [menu_base_y]
    mov ecx, 3
    mov edx, 15
    call draw_text

    mov eax, 0
    mov ebx, 188
    mov esi, menu_entry_vibeos
    call draw_menu_entry

    mov eax, 1
    mov ebx, 226
    mov esi, menu_entry_safe
    call draw_menu_entry

    mov eax, 2
    mov ebx, 264
    mov esi, menu_entry_rescue
    call draw_menu_entry

    cmp byte [menu_timeout_paused], 0
    jne .draw_paused

    mov eax, [menu_seconds_left]
    add al, '0'
    mov [countdown_text + 13], al
    mov esi, countdown_text
    jmp .draw_countdown

.draw_paused:
    mov esi, countdown_paused_text

.draw_countdown:
    mov eax, 212
    add eax, [menu_base_x]
    mov ebx, 304
    add ebx, [menu_base_y]
    mov ecx, 2
    mov edx, 15
    call draw_text

    call menu_build_video_text
    mov esi, menu_video_text
    mov eax, 208
    add eax, [menu_base_x]
    mov ebx, 328
    add ebx, [menu_base_y]
    mov ecx, 2
    mov edx, 15
    call draw_text

    mov esi, menu_hint_top
    mov eax, 192
    add eax, [menu_base_x]
    mov ebx, 352
    add ebx, [menu_base_y]
    mov ecx, 1
    mov edx, 15
    call draw_text

    mov esi, menu_hint_bottom
    mov eax, 232
    add eax, [menu_base_x]
    mov ebx, 362
    add ebx, [menu_base_y]
    mov ecx, 1
    mov edx, 15
    call draw_text
    ret

draw_menu_entry:
    push esi
    add ebx, [menu_base_y]
    cmp [menu_selection], eax
    jne .normal

    push ebx
    mov eax, 198
    add eax, [menu_base_x]
    sub ebx, 2
    mov ecx, 244
    mov esi, 34
    mov dl, 15
    call draw_rect
    pop ebx

    mov eax, 200
    add eax, [menu_base_x]
    mov ecx, 240
    mov esi, 30
    mov dl, 14
    push ebx
    call draw_rect
    pop ebx
    mov edx, 1
    jmp .label

.normal:
    push ebx
    mov eax, 198
    add eax, [menu_base_x]
    sub ebx, 2
    mov ecx, 244
    mov esi, 34
    mov dl, 15
    call draw_rect
    pop ebx

    mov eax, 200
    add eax, [menu_base_x]
    mov ecx, 240
    mov esi, 30
    mov dl, 7
    push ebx
    call draw_rect
    pop ebx
    mov edx, 1

.label:
    pop esi
    mov eax, 220
    add eax, [menu_base_x]
    add ebx, 8
    mov ecx, 2
    call draw_text
    ret

draw_background:
    cmp byte [background_available], 0
    jne .image

    xor eax, eax
    xor ebx, ebx
    movzx ecx, word [BOOTINFO_ADDR + BOOTINFO_VESA_WIDTH]
    movzx esi, word [BOOTINFO_ADDR + BOOTINFO_VESA_HEIGHT]
    mov dl, 3
    call draw_rect
    ret

.image:
    xor eax, eax
    xor ebx, ebx
    movzx ecx, word [BOOTINFO_ADDR + BOOTINFO_VESA_WIDTH]
    movzx esi, word [BOOTINFO_ADDR + BOOTINFO_VESA_HEIGHT]
    mov dl, 3
    call draw_rect

    xor ebp, ebp
    mov esi, BACKGROUND_DRAW_ADDR
.row_loop:
    xor edi, edi
.col_loop:
    mov dl, [esi]
    mov eax, edi
    imul eax, [background_scale]
    add eax, [background_base_x]
    mov ebx, ebp
    imul ebx, [background_scale]
    add ebx, [background_base_y]
    mov ecx, [background_scale]
    push esi
    mov esi, [background_scale]
    call draw_rect
    pop esi
    inc esi
    inc edi
    cmp edi, BACKGROUND_SOURCE_WIDTH
    jb .col_loop
    inc ebp
    cmp ebp, BACKGROUND_SOURCE_HEIGHT
    jb .row_loop
    ret

set_desktop_palette:
    mov dx, 0x03C8
    xor al, al
    out dx, al
    mov dx, 0x03C9

    xor ecx, ecx
.loop:
    cmp ecx, 256
    jae .done
    cmp ecx, 16
    jb .ega
    mov eax, ecx
    shr eax, 5
    and eax, 0x07
    imul eax, 63
    mov ebx, 7
    xor edx, edx
    div ebx
    mov dx, 0x03C9
    out dx, al

    mov eax, ecx
    shr eax, 2
    and eax, 0x07
    imul eax, 63
    mov ebx, 7
    xor edx, edx
    div ebx
    mov dx, 0x03C9
    out dx, al

    mov eax, ecx
    and eax, 0x03
    imul eax, 63
    mov ebx, 3
    xor edx, edx
    div ebx
    mov dx, 0x03C9
    out dx, al
    inc ecx
    jmp .loop

.ega:
    movzx eax, byte [ega_palette_6bit + ecx * 3 + 0]
    out dx, al
    movzx eax, byte [ega_palette_6bit + ecx * 3 + 1]
    out dx, al
    movzx eax, byte [ega_palette_6bit + ecx * 3 + 2]
    out dx, al
    inc ecx
    jmp .loop
.done:
    ret

draw_rect:
    push edi
    push ebp
    cld
    mov edi, [BOOTINFO_ADDR + BOOTINFO_VESA_FB]
    movzx ebp, word [BOOTINFO_ADDR + BOOTINFO_VESA_PITCH]
    imul ebx, ebp
    add edi, ebx
    add edi, eax
.row:
    push ecx
    mov al, dl
    rep stosb
    pop ecx
    mov eax, ebp
    sub eax, ecx
    add edi, eax
    dec esi
    jnz .row
    pop ebp
    pop edi
    ret

draw_text:
    push ebp
    push edi
    cld
    mov edi, eax
    mov ebp, ebx
.next_char:
    lodsb
    test al, al
    jz .done
    cmp al, ' '
    je .advance
    push esi
    push ecx
    push edx
    push edi
    push ebp
    mov ebx, edi
    mov edi, ebp
    call draw_glyph
    pop ebp
    pop edi
    pop edx
    pop ecx
    pop esi
.advance:
    mov eax, ecx
    imul eax, 6
    add edi, eax
    jmp .next_char
.done:
    pop edi
    pop ebp
    ret

menu_build_video_text:
    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi

    mov edi, menu_video_text
    mov esi, menu_video_prefix
    call menu_append_cstring

    movzx ebx, byte [BOOTINFO_ADDR + BOOTINFO_VIDEO_SELECTED_INDEX]
    cmp bl, BOOTINFO_VIDEO_INDEX_NONE
    jne .have_index
    movzx ebx, byte [BOOTINFO_ADDR + BOOTINFO_VIDEO_ACTIVE_INDEX]

.have_index:
    cmp bl, BOOTINFO_VIDEO_INDEX_NONE
    jne .have_mode
    mov esi, menu_video_unknown
    call menu_append_cstring
    jmp .finish

.have_mode:
    shl ebx, 3
    movzx eax, word [BOOTINFO_ADDR + BOOTINFO_VIDEO_MODES + ebx + BOOTINFO_VIDEO_MODE_FIELD_WIDTH]
    call menu_append_uint
    mov al, 'X'
    stosb
    movzx eax, word [BOOTINFO_ADDR + BOOTINFO_VIDEO_MODES + ebx + BOOTINFO_VIDEO_MODE_FIELD_HEIGHT]
    call menu_append_uint

.finish:
    mov al, 0
    stosb

    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

menu_append_cstring:
    cld
.copy:
    lodsb
    test al, al
    jz .done
    stosb
    jmp .copy
.done:
    ret

menu_append_uint:
    push eax
    push ebx
    push edx
    push esi

    lea esi, [menu_number_buffer + 5]
    mov byte [esi], 0
    mov ebx, 10
    mov eax, [esp + 12]
    test eax, eax
    jnz .convert
    dec esi
    mov byte [esi], '0'
    jmp .append

.convert:
    xor edx, edx
.loop:
    div ebx
    add dl, '0'
    dec esi
    mov [esi], dl
    test eax, eax
    jnz .next
    jmp .append

.next:
    xor edx, edx
    jmp .loop

.append:
    call menu_append_cstring
    pop esi
    pop edx
    pop ebx
    pop eax
    ret

draw_glyph:
    push ebp
    push ebx
    push edi
    push ecx
    push edx
    push esi
    call glyph_ptr_from_char
    test esi, esi
    jz .done
    mov [glyph_ptr_tmp], esi
    xor ebp, ebp
.row_loop:
    cmp ebp, 7
    jae .done
    mov esi, [glyph_ptr_tmp]
    mov al, [esi + ebp]
    mov [glyph_row_bits], al
    xor edi, edi
.col_loop:
    cmp edi, 5
    jae .next_row
    mov al, [glyph_row_bits]
    mov bl, [glyph_masks + edi]
    test al, bl
    jz .skip_pixel

    mov eax, [esp + 16]
    mov ecx, [esp + 8]
    mov edx, edi
    imul edx, ecx
    add eax, edx

    mov ebx, [esp + 12]
    mov edx, ebp
    imul edx, ecx
    add ebx, edx

    mov esi, ecx
    mov edx, [esp + 4]
    call draw_rect

.skip_pixel:
    inc edi
    jmp .col_loop

.next_row:
    inc ebp
    jmp .row_loop

.done:
    pop esi
    pop edx
    pop ecx
    pop edi
    pop ebx
    pop ebp
    ret

glyph_ptr_from_char:
    cmp al, 'A'
    jb .digit_or_space
    cmp al, 'Z'
    ja .digit_or_space
    movzx eax, al
    sub eax, 'A'
    imul eax, 7
    lea esi, [font_glyphs + eax]
    ret

.digit_or_space:
    cmp al, '0'
    jb .space
    cmp al, '9'
    ja .space
    movzx eax, al
    sub eax, '0'
    add eax, 26
    imul eax, 7
    lea esi, [font_glyphs + eax]
    ret

.space:
    mov esi, font_space
    ret

pmode_to_real_for_video_change:
    DEBUG_CHAR 'v'
    cli
    jmp CODE16_SEG:pmode16_to_real

BITS 16
pmode16_to_real:
    DEBUG_CHAR 'u'
    mov ax, DATA16_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov sp, REALMODE_STACK_TOP
    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax
    jmp 0x0000:realmode_apply_video_change

realmode_apply_video_change:
    DEBUG_CHAR 'w'
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, REALMODE_STACK_TOP

    mov al, [BOOTINFO_ADDR + BOOTINFO_VIDEO_SELECTED_INDEX]
    cmp al, BOOTINFO_VIDEO_INDEX_NONE
    je .resume
    xor bx, bx
    mov bl, al
    shl bx, 3
    mov dx, [BOOTINFO_ADDR + BOOTINFO_VIDEO_MODES + bx + BOOTINFO_VIDEO_MODE_FIELD_MODE]
    DEBUG_CHAR 'x'
    call vesa_set_mode_and_store_bootinfo
    jc .restore_selection
    DEBUG_CHAR 'y'
    call populate_legacy_vesa_info
    jmp .resume

.restore_selection:
    mov al, [BOOTINFO_ADDR + BOOTINFO_VIDEO_ACTIVE_INDEX]
    mov [BOOTINFO_ADDR + BOOTINFO_VIDEO_SELECTED_INDEX], al

.resume:
    DEBUG_CHAR 'z'
    call enable_a20
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp CODE_SEG:pmode_video_resume

BITS 32
pmode_video_resume:
    DEBUG_CHAR 'W'
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x70000
    cld
    mov byte [menu_extended], 0
    call set_desktop_palette
    call menu_compute_layout
    mov byte [menu_dirty], 1
    jmp vibeloader_menu_resume

menu_selection dd 0
menu_elapsed_counts dd 0
menu_seconds_left dd MENU_TIMEOUT_SECONDS
menu_prev_pit dw 0
menu_base_x dd 0
menu_base_y dd 0
background_base_x dd 0
background_base_y dd 0
background_scale dd 1
menu_dirty db 0
menu_initialized db 0
menu_timeout_paused db 0
menu_extended db 0
glyph_row_bits db 0
glyph_ptr_tmp dd 0

vibeloader_title db 'VIBELOADER', 0
menu_entry_vibeos db 'VIBEOS', 0
menu_entry_safe db 'SAFE MODE', 0
menu_entry_rescue db 'RESCUE SHELL', 0
countdown_text db 'AUTO BOOT IN 0S', 0
countdown_paused_text db 'AUTO BOOT PAUSED', 0
menu_video_prefix db 'VIDEO ', 0
menu_video_unknown db 'UNKNOWN', 0
menu_hint_top db 'UP/DOWN SELECT  ENTER BOOT', 0
menu_hint_bottom db 'LEFT/RIGHT VIDEO', 0
menu_video_text times 24 db 0
menu_number_buffer times 6 db 0

glyph_masks db 16, 8, 4, 2, 1

ega_palette_6bit:
    db 0, 0, 0
    db 0, 0, 42
    db 0, 42, 0
    db 0, 42, 42
    db 42, 0, 0
    db 42, 0, 42
    db 42, 21, 0
    db 42, 42, 42
    db 21, 21, 21
    db 21, 21, 63
    db 21, 63, 21
    db 21, 63, 63
    db 63, 21, 21
    db 63, 21, 63
    db 63, 63, 21
    db 63, 63, 63

font_glyphs:
    ; A-Z
    db 0x0E,0x11,0x11,0x1F,0x11,0x11,0x11
    db 0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E
    db 0x0E,0x11,0x10,0x10,0x10,0x11,0x0E
    db 0x1E,0x11,0x11,0x11,0x11,0x11,0x1E
    db 0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F
    db 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10
    db 0x0E,0x11,0x10,0x10,0x13,0x11,0x0F
    db 0x11,0x11,0x11,0x1F,0x11,0x11,0x11
    db 0x0E,0x04,0x04,0x04,0x04,0x04,0x0E
    db 0x01,0x01,0x01,0x01,0x11,0x11,0x0E
    db 0x11,0x12,0x14,0x18,0x14,0x12,0x11
    db 0x10,0x10,0x10,0x10,0x10,0x10,0x1F
    db 0x11,0x1B,0x15,0x15,0x11,0x11,0x11
    db 0x11,0x19,0x15,0x13,0x11,0x11,0x11
    db 0x0E,0x11,0x11,0x11,0x11,0x11,0x0E
    db 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10
    db 0x0E,0x11,0x11,0x11,0x15,0x12,0x0D
    db 0x1E,0x11,0x11,0x1E,0x14,0x12,0x11
    db 0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E
    db 0x1F,0x04,0x04,0x04,0x04,0x04,0x04
    db 0x11,0x11,0x11,0x11,0x11,0x11,0x0E
    db 0x11,0x11,0x11,0x11,0x11,0x0A,0x04
    db 0x11,0x11,0x11,0x15,0x15,0x15,0x0A
    db 0x11,0x11,0x0A,0x04,0x0A,0x11,0x11
    db 0x11,0x11,0x0A,0x04,0x04,0x04,0x04
    db 0x1F,0x01,0x02,0x04,0x08,0x10,0x1F
    ; 0-9
    db 0x0E,0x11,0x13,0x15,0x19,0x11,0x0E
    db 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E
    db 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F
    db 0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E
    db 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02
    db 0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E
    db 0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E
    db 0x1F,0x01,0x02,0x04,0x08,0x08,0x08
    db 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E
    db 0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E
font_space:
    db 0x00,0x00,0x00,0x00,0x00,0x00,0x00

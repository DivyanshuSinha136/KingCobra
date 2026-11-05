; KCOS boot stage -- BIOS / MBR, x86-64.  Author: Divyanshu Sinha (MIT License)
;
; This is the only part of KCOS that is not KingCobra: real mode and 32-bit protected mode cannot be produced by kcc
; (it emits x86-64 only).  The assembled result (boot.bin, 2560 bytes) is shipped pre-built, so building KCOS itself
; needs no assembler.  Rebuild it with:  nasm -f bin boot.asm -o boot.bin
;
; Disk layout:  LBA 0 stage 1 (this MBR, 512 B)   LBA 1..4 stage 2 (2 KiB)   LBA 5.. kernel (flat binary, loaded at KERNEL_BASE)
; The image builder (tools/mkimage.kc) patches the kernel sector count (u16) at offset 0x1FC of stage 1.
;
; Hand-over to the kernel (long mode, identity-mapped first 4 GiB, interrupts off):
;   BOOTINFO @ 0x500:  u32 magic 'KCOS' | u32 e820 entry count | u32 boot drive | u32 kernel sectors | u64 reserved
;                      e820 entries (24 bytes each) from 0x510, at most 40
;   PML4 @ 0x1000, PDPT @ 0x2000, PDs @ 0x3000..0x6000 (2 MiB pages, >= 0xC0000000 uncached for MMIO).
;   Stage 2 leaves a throw-away GDT at 0x7E00+: the kernel must load its own GDT first thing.
;   SSE is enabled (CR0.EM=0, CR4.OSFXSR/OSXMMEXCPT=1).

KERNEL_BASE   equ 0x10000
KERNEL_SEG    equ 0x1000            ; KERNEL_BASE >> 4
STAGE2_BASE   equ 0x7E00
BOOTINFO      equ 0x500
E820_BUF      equ 0x510
E820_MAX      equ 40
STAGE2_SECTORS equ 4
PML4          equ 0x1000
PDPT          equ 0x2000
PD0           equ 0x3000

; ======================================================================== stage 1 (MBR)
bits 16
org 0x7C00
stage1:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov [boot_drive], dl
    mov si, msg_boot
    call puts16
    ; load stage 2 to 0x7E00
    mov dword [dap_lba], 1
    mov word  [dap_cnt], STAGE2_SECTORS
    mov word  [dap_off], STAGE2_BASE
    mov word  [dap_seg], 0
    call read_dap
    mov dl, [boot_drive]
    jmp 0x0000:STAGE2_BASE

; read the sector range described by the DAP; halts with 'E' on failure
read_dap:
    mov si, dap
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc .fail
    ret
.fail:
    mov si, msg_err
    call puts16
.hang:
    hlt
    jmp .hang

puts16:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bx, 7
    int 0x10
    jmp puts16
.done:
    ret

boot_drive: db 0
msg_boot:   db "KCOS boot", 13, 10, 0
msg_err:    db "disk error", 13, 10, 0
align 4
dap:
    db 0x10, 0
dap_cnt: dw 0
dap_off: dw 0
dap_seg: dw 0
dap_lba: dq 0

times 0x1FC - ($ - $$) db 0
kernel_sectors: dw 0          ; patched by mkimage
                dw 0xAA55

; ======================================================================== stage 2
stage2:
    xor ax, ax
    mov ds, ax
    mov es, ax
    ; ---- boot info header
    mov dword [BOOTINFO], 'KCOS'
    movzx eax, byte [boot_drive]
    mov [BOOTINFO + 8], eax
    movzx eax, word [kernel_sectors]
    mov [BOOTINFO + 12], eax
    mov dword [BOOTINFO + 16], 0
    mov dword [BOOTINFO + 20], 0
    ; ---- text mode 3 (80x25)
    mov ax, 3
    int 0x10
    mov si, msg2
    call puts16
    ; ---- A20 (fast gate)
    in al, 0x92
    or al, 2
    and al, 0xFE
    out 0x92, al
    ; ---- memory map (INT 15h, E820)
    xor ebx, ebx
    xor bp, bp
    mov di, E820_BUF
.e820:
    mov dword [di + 20], 1
    mov eax, 0xE820
    mov edx, 0x534D4150           ; 'SMAP'
    mov ecx, 24
    int 0x15
    jc .e820_done
    cmp eax, 0x534D4150
    jne .e820_done
    jcxz .e820_skip
    inc bp
    add di, 24
.e820_skip:
    cmp bp, E820_MAX
    jae .e820_done
    test ebx, ebx
    jnz .e820
.e820_done:
    movzx eax, bp
    mov [BOOTINFO + 4], eax
    ; ---- load the kernel in 32-sector (16 KiB) chunks to KERNEL_BASE
    movzx ecx, word [kernel_sectors]
    mov dword [dap_lba], 1 + STAGE2_SECTORS
    mov word [dap_off], 0
    mov word [dap_seg], KERNEL_SEG
.load:
    test ecx, ecx
    jz .loaded
    mov ax, 32
    cmp ecx, 32
    jae .chunk
    mov ax, cx
.chunk:
    mov [dap_cnt], ax
    push ecx
    push ax
    call read_dap
    pop ax
    pop ecx
    movzx edx, ax
    sub ecx, edx
    add [dap_lba], edx
    shl edx, 5                    ; sectors * 512 / 16 = paragraphs
    add [dap_seg], dx
    jmp .load
.loaded:
    mov si, msg3
    call puts16
    ; ---- protected mode
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pm32

msg2: db "loading kernel...", 13, 10, 0
msg3: db "entering long mode", 13, 10, 0

bits 32
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov esp, 0x7000
    ; ---- page tables: zero 0x1000..0x6FFF
    mov edi, PML4
    xor eax, eax
    mov ecx, (0x6000) / 4
    rep stosd
    mov dword [PML4], PDPT | 3
    mov dword [PDPT + 0],  (PD0 + 0x0000) | 3
    mov dword [PDPT + 8],  (PD0 + 0x1000) | 3
    mov dword [PDPT + 16], (PD0 + 0x2000) | 3
    mov dword [PDPT + 24], (PD0 + 0x3000) | 3
    ; 2048 x 2 MiB pages = 4 GiB identity map; the top GiB (devices) is cache-disabled
    mov edi, PD0
    xor eax, eax                  ; physical address
    xor ecx, ecx                  ; index
.pd:
    mov edx, eax
    or edx, 0x83                  ; present | writable | 2 MiB page
    cmp ecx, 1536
    jb .cached
    or edx, 0x18                  ; PWT | PCD
.cached:
    mov [edi], edx
    mov dword [edi + 4], 0
    add edi, 8
    add eax, 0x200000
    inc ecx
    cmp ecx, 2048
    jb .pd
    ; ---- long mode
    mov eax, cr4
    or eax, 0x620                 ; PAE | OSFXSR | OSXMMEXCPT
    mov cr4, eax
    mov eax, PML4
    mov cr3, eax
    mov ecx, 0xC0000080           ; EFER
    rdmsr
    or eax, 0x100                 ; LME
    wrmsr
    mov eax, cr0
    or eax, 0x80000023            ; PG | NE | PE | (MP below)
    or eax, 2                     ; MP
    and eax, 0xFFFFFFFB           ; EM = 0
    mov cr0, eax
    jmp 0x18:lm64

bits 64
lm64:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    mov eax, KERNEL_BASE
    jmp rax

align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF         ; 0x08  32-bit code
    dq 0x00CF92000000FFFF         ; 0x10  32-bit data (also fine as 64-bit data)
    dq 0x00209A0000000000         ; 0x18  64-bit code
gdt_end:
gdt_desc:
    dw gdt_end - gdt - 1
    dd gdt

times (512 + STAGE2_SECTORS * 512) - ($ - $$) db 0

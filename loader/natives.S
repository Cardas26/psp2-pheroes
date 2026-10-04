    .syntax unified
    .arm
    .text

.macro THUNK name
    .global ce_\name
    .type ce_\name, %function
    .align 2
ce_\name:
    push {r4, lr}
    mov r4, sp
    bic sp, sp, #7
    blx n_\name
    mov sp, r4
    pop {r4, pc}
.endm

.macro THUNK_STACK name
    .global ce_\name
    .type ce_\name, %function
    .align 2
ce_\name:
    push {r4, lr}
    mov r4, sp
    add r12, sp, #8
    bic sp, sp, #7
    sub sp, sp, #8
    str r12, [sp]
    blx n_\name
    mov sp, r4
    pop {r4, pc}
.endm

THUNK new
THUNK delete
THUNK LocalAlloc
THUNK LocalReAlloc
THUNK LocalFree
THUNK LocalSize
THUNK malloc
THUNK free
THUNK VirtualAlloc
THUNK VirtualFree
THUNK memchr
THUNK memcmp
THUNK memcpy
THUNK memmove
THUNK memset
THUNK_STACK CreateFileW
THUNK ReadFile
THUNK WriteFile
THUNK SetFilePointer
THUNK GetFileSize
THUNK CloseHandle
THUNK FlushFileBuffers
THUNK FindFirstFileW
THUNK FindNextFileW
THUNK FindClose
THUNK GetFileAttributesW
THUNK DeleteFileW
THUNK MoveFileW
THUNK CreateDirectoryW
THUNK GetModuleFileNameW
THUNK GetLastError
THUNK wcschr
THUNK wcsrchr
THUNK wcsstr
THUNK _wcsupr
THUNK _vsnwprintf
THUNK rand
THUNK GetCurrentFT
THUNK CeGetRandomSeed
THUNK strlen
THUNK wcstoul
THUNK_STACK WideCharToMultiByte
THUNK RegOpenKeyExW
THUNK FileTimeToSystemTime
THUNK SystemTimeToFileTime
THUNK __adds
THUNK __subs
THUNK __muls
THUNK __divs
THUNK __addd
THUNK __subd
THUNK __muld
THUNK __divd
THUNK __stod
THUNK __dtos
THUNK __stoi
THUNK __utos
THUNK __itos
THUNK __dtoi
THUNK __dtou
THUNK __itod
THUNK __utod
THUNK __ges
THUNK __gts
THUNK __lts
THUNK __rt_sdiv
THUNK __rt_udiv
THUNK __rt_sdiv64by64
THUNK __rt_udiv64by64
THUNK __rt_urem64by64
THUNK sin
THUNK cos
THUNK sqrt
THUNK RegisterClassW
THUNK ShowWindow
THUNK SetForegroundWindow
THUNK GetForegroundWindow
THUNK DestroyWindow
THUNK GetWindowLongW
THUNK SetWindowLongW
THUNK DefWindowProcW
THUNK GetMessageW
THUNK DispatchMessageW
THUNK PostQuitMessage
THUNK BeginPaint
THUNK EndPaint
THUNK GetDC
THUNK ReleaseDC
THUNK ChangeDisplaySettingsEx
THUNK GetSystemMetrics
THUNK LoadCursorW
THUNK CreateSolidBrush
THUNK DeleteObject
THUNK DeleteDC
THUNK SetCapture
THUNK ReleaseCapture
THUNK LoadLibraryW
THUNK GetProcAddressW
THUNK FreeLibrary
THUNK SetKMode
THUNK SystemIdleTimerReset
THUNK SHFullScreen
THUNK GXOpenInput
THUNK GXCloseInput
THUNK GXGetDefaultKeys
THUNK GetVersionExW
THUNK GetSystemInfo
THUNK GlobalMemoryStatus
THUNK GetLocaleInfoW
THUNK SystemParametersInfoW
THUNK _wcsicmp
THUNK GetLocalTime
THUNK GetTickCount
THUNK Sleep
THUNK CreateMutexW
THUNK ReleaseMutex
THUNK WaitForSingleObject
THUNK CreateEventW
THUNK EventModify
THUNK InitializeCriticalSection
THUNK EnterCriticalSection
THUNK LeaveCriticalSection
THUNK InterlockedCompareExchange
THUNK_STACK CreateThread
THUNK ResumeThread
THUNK waveOutOpen
THUNK vsprintf
THUNK fopen
THUNK fprintf
THUNK fclose
THUNK OutputDebugStringW
THUNK_STACK CreateWindowExW
THUNK_STACK PeekMessageW
THUNK_STACK ExtEscape
THUNK waveOutPrepareHeader
THUNK waveOutWrite
THUNK waveOutReset
THUNK waveOutUnprepareHeader
THUNK waveOutClose

    .global fog_grid_thunk
    .type fog_grid_thunk, %function
    .align 2
fog_grid_thunk:
    ldr r0, [fp, #-0x120]
    asr r0, r0, #1
    sub r0, r0, #5
    str r0, [fp, #-0x120]
    ldr r0, [fp, #-0x11c]
    sub r0, r0, #7
    asr r0, r0, #1
    str r0, [fp, #-0x11c]
    mov ip, lr
    ldr lr, [fp, #-0x144]
    bx ip

    .macro realign name, to
    .global \name
    .type \name, %function
    .align 2
\name:
    push {r4, lr}
    mov r4, sp
    bic sp, sp, #7
    blx \to
    mov sp, r4
    pop {r4, pc}
    .endm

    realign survey_strips_thunk, survey_strips
    realign pyramid_thunk, pyramidal_stretch
    realign present_thunk, do_paint

    .global av_row_thunk
    .type av_row_thunk, %function
    .align 2
av_row_thunk:
    push {r0-r2, r4, ip, lr}
    ldr r3, [sp, #(24 + 0x40)]
    add r1, r2, r3
    mov r2, lr
    mov r3, sl
    mov r4, sp
    bic sp, sp, #7
    blx av_row_range
    mov sp, r4
    mov r3, r0
    pop {r0-r2, r4, ip, lr}
    ldr ip, =av_xx1
    ldr ip, [ip]
    cmp r3, ip
    bge 1f
    str r3, [sp, #0x1c]
    add r1, r3, r3, lsl #2
    add r1, r2, r1, lsl #1
    add r5, r3, r3, lsl #2
    add r5, lr, r5, lsl #2
    str r1, [sp, #0x30]
    str r5, [sp, #0x10]
    ldr ip, =av_ret_loop
    ldr pc, [ip]
1:  ldr ip, =av_ret_skip
    ldr pc, [ip]

    .global av_cmp_thunk
    .type av_cmp_thunk, %function
    .align 2
av_cmp_thunk:
    ldr ip, =av_xx1
    ldr ip, [ip]
    cmp r3, ip
    bx lr
    .ltorg

    .macro memo_stub k
    .global memo_stub_\k
    .type memo_stub_\k, %function
    .align 2
memo_stub_\k:
    ldr ip, =memo_rec
    ldr ip, [ip]
    cmp ip, #0
    bne 1f
2:  ldr ip, =memo_tramp
    ldr pc, [ip, #4*\k]
1:  push {r0-r4, lr}
    mov r4, sp
    bic sp, sp, #7
    sub sp, sp, #8
    add ip, r4, #24
    str ip, [sp, #4]
    mov ip, #\k
    str ip, [sp]
    blx memo_prim
    mov sp, r4
    cmp r0, #0
    pop {r0-r4, lr}
    beq 2b
    mov r0, #0
    bx lr
    .ltorg
    .endm

    memo_stub 0
    memo_stub 1
    memo_stub 2
    memo_stub 3
    memo_stub 4
    memo_stub 5
    memo_stub 6
    memo_stub 7
    memo_stub 8
    memo_stub 9
    memo_stub 10

    realign memo_child_thunk, memo_child
    realign map_bands_thunk, map_bands

    .global sample_guard
    .type sample_guard, %function
    .align 2
sample_guard:
    ldr ip, [r0, #0x38]
    ldr r3, [r0, #0x40]
    sub r3, r3, ip
    cmp r1, r3, lsr #3
    bhs sample_reject_thunk
    ldr ip, =sample_tramp
    ldr pc, [ip]
    .ltorg
    realign sample_reject_thunk, sample_reject

    .global replace_guard
    .type replace_guard, %function
    .align 2
replace_guard:
    ldr ip, [r0, #0x38]
    ldr r3, [r0, #0x40]
    sub r3, r3, ip
    cmp r2, r3, lsr #3
    movhs r1, r2
    bhs sample_reject_thunk
    ldr ip, =replace_tramp
    ldr pc, [ip]
    .ltorg
    .global outline_stub
    .type outline_stub, %function
    .align 2
outline_stub:
    push {r0-r4, lr}
    mov r0, r1
    mov r1, r2
    mov r2, lr
    mov r4, sp
    bic sp, sp, #7
    blx outline_note
    mov sp, r4
    pop {r0-r4, lr}
    ldr ip, =outline_tramp
    ldr pc, [ip]
    .ltorg

    .macro battle_end_thunk name, won, insn, back
    .global \name
    .type \name, %function
    .align 2
\name:
    push {r0-r5, ip, lr}
    mov r4, sp
    bic sp, sp, #7
    mov r0, #\won
    blx music_battle_end
    mov sp, r4
    pop {r0-r5, ip, lr}
    \insn
    ldr ip, =\back
    ldr pc, [ip]
    .ltorg
    .endm
    battle_end_thunk battle_won_thunk, 1, "ldr r3, [fp, #0x10]", battle_won_back
    battle_end_thunk battle_lost_thunk, 0, "cmp r0, #0", battle_lost_back

    .global ring_tip_thunk
    .type ring_tip_thunk, %function
    .align 2
ring_tip_thunk:
    push {r4, r5, r6, lr}
    add r4, sp, #16
    mov r5, sp
    bic sp, sp, #7
    sub sp, sp, #16
    ldr ip, [r4]
    str ip, [sp]
    ldr ip, [r4, #4]
    str ip, [sp, #4]
    ldr ip, [r4, #8]
    str ip, [sp, #8]
    ldr ip, [r4, #12]
    str ip, [sp, #12]
    blx ring_tip_text
    mov sp, r5
    pop {r4, r5, r6, pc}

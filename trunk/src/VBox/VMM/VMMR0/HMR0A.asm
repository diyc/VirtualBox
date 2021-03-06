; $Id$
;; @file
; HM - Ring-0 VMX, SVM world-switch and helper routines.
;

;
; Copyright (C) 2006-2020 Oracle Corporation
;
; This file is part of VirtualBox Open Source Edition (OSE), as
; available from http://www.virtualbox.org. This file is free software;
; you can redistribute it and/or modify it under the terms of the GNU
; General Public License (GPL) as published by the Free Software
; Foundation, in version 2 as it comes in the "COPYING" file of the
; VirtualBox OSE distribution. VirtualBox OSE is distributed in the
; hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
;

;*********************************************************************************************************************************
;*  Header Files                                                                                                                 *
;*********************************************************************************************************************************
%include "VBox/asmdefs.mac"
%include "VBox/err.mac"
%include "VBox/vmm/hm_vmx.mac"
%include "VBox/vmm/cpum.mac"
%include "VBox/vmm/vm.mac"
%include "iprt/x86.mac"
%include "HMInternal.mac"

%ifndef RT_ARCH_AMD64
 %error AMD64 only.
%endif


;*********************************************************************************************************************************
;*  Defined Constants And Macros                                                                                                 *
;*********************************************************************************************************************************
;; The offset of the XMM registers in X86FXSTATE.
; Use define because I'm too lazy to convert the struct.
%define XMM_OFF_IN_X86FXSTATE   160

;; Spectre filler for 64-bit mode.
; Choosen to be an invalid address (also with 5 level paging).
%define SPECTRE_FILLER          0x02204204207fffff

;;
; Determine skipping restoring of GDTR, IDTR, TR across VMX non-root operation.
;
%define VMX_SKIP_GDTR
%define VMX_SKIP_TR
%define VBOX_SKIP_RESTORE_SEG
%ifdef RT_OS_DARWIN
 ; Load the NULL selector into DS, ES, FS and GS on 64-bit darwin so we don't
 ; risk loading a stale LDT value or something invalid.
 %define HM_64_BIT_USE_NULL_SEL
 ; Darwin (Mavericks) uses IDTR limit to store the CPU Id so we need to restore it always.
 ; See @bugref{6875}.
%else
 %define VMX_SKIP_IDTR
%endif

;; @def PUSH_CALLEE_PRESERVED_REGISTERS
; Macro generating an equivalent to PUSHAD instruction.

;; @def POP_CALLEE_PRESERVED_REGISTERS
; Macro generating an equivalent to POPAD instruction.

;; @def PUSH_RELEVANT_SEGMENT_REGISTERS
; Macro saving all segment registers on the stack.
; @param 1  Full width register name.
; @param 2  16-bit register name for \a 1.

;; @def POP_RELEVANT_SEGMENT_REGISTERS
; Macro restoring all segment registers on the stack.
; @param 1  Full width register name.
; @param 2  16-bit register name for \a 1.

%ifdef ASM_CALL64_GCC
 %macro PUSH_CALLEE_PRESERVED_REGISTERS 0
   push    r15
   push    r14
   push    r13
   push    r12
   push    rbx
 %endmacro
 %macro POP_CALLEE_PRESERVED_REGISTERS 0
   pop     rbx
   pop     r12
   pop     r13
   pop     r14
   pop     r15
 %endmacro

%else ; ASM_CALL64_MSC
 %macro PUSH_CALLEE_PRESERVED_REGISTERS 0
   push    r15
   push    r14
   push    r13
   push    r12
   push    rbx
   push    rsi
   push    rdi
 %endmacro
 %macro POP_CALLEE_PRESERVED_REGISTERS 0
   pop     rdi
   pop     rsi
   pop     rbx
   pop     r12
   pop     r13
   pop     r14
   pop     r15
 %endmacro
%endif

%ifdef VBOX_SKIP_RESTORE_SEG
 %macro PUSH_RELEVANT_SEGMENT_REGISTERS 2
 %endmacro

 %macro POP_RELEVANT_SEGMENT_REGISTERS 2
 %endmacro
%else       ; !VBOX_SKIP_RESTORE_SEG
 ; Trashes, rax, rdx & rcx.
 %macro PUSH_RELEVANT_SEGMENT_REGISTERS 2
  %ifndef HM_64_BIT_USE_NULL_SEL
   mov     %2, es
   push    %1
   mov     %2, ds
   push    %1
  %endif

   ; Special case for FS; Windows and Linux either don't use it or restore it when leaving kernel mode,
   ; Solaris OTOH doesn't and we must save it.
   mov     ecx, MSR_K8_FS_BASE
   rdmsr
   push    rdx
   push    rax
  %ifndef HM_64_BIT_USE_NULL_SEL
   push    fs
  %endif

   ; Special case for GS; OSes typically use swapgs to reset the hidden base register for GS on entry into the kernel.
   ; The same happens on exit.
   mov     ecx, MSR_K8_GS_BASE
   rdmsr
   push    rdx
   push    rax
  %ifndef HM_64_BIT_USE_NULL_SEL
   push    gs
  %endif
 %endmacro

 ; trashes, rax, rdx & rcx
 %macro POP_RELEVANT_SEGMENT_REGISTERS 2
   ; Note: do not step through this code with a debugger!
  %ifndef HM_64_BIT_USE_NULL_SEL
   xor     eax, eax
   mov     ds, ax
   mov     es, ax
   mov     fs, ax
   mov     gs, ax
  %endif

  %ifndef HM_64_BIT_USE_NULL_SEL
   pop     gs
  %endif
   pop     rax
   pop     rdx
   mov     ecx, MSR_K8_GS_BASE
   wrmsr

  %ifndef HM_64_BIT_USE_NULL_SEL
   pop     fs
  %endif
   pop     rax
   pop     rdx
   mov     ecx, MSR_K8_FS_BASE
   wrmsr
   ; Now it's safe to step again

  %ifndef HM_64_BIT_USE_NULL_SEL
   pop     %1
   mov     ds, %2
   pop     %1
   mov     es, %2
  %endif
 %endmacro
%endif ; VBOX_SKIP_RESTORE_SEG


;;
; Creates an indirect branch prediction barrier on CPUs that need and supports that.
; @clobbers eax, edx, ecx
; @param    1   How to address CPUMCTX.
; @param    2   Which flag to test for (CPUMCTX_WSF_IBPB_ENTRY or CPUMCTX_WSF_IBPB_EXIT)
%macro INDIRECT_BRANCH_PREDICTION_BARRIER 2
    test    byte [%1 + CPUMCTX.fWorldSwitcher], %2
    jz      %%no_indirect_branch_barrier
    mov     ecx, MSR_IA32_PRED_CMD
    mov     eax, MSR_IA32_PRED_CMD_F_IBPB
    xor     edx, edx
    wrmsr
%%no_indirect_branch_barrier:
%endmacro

;;
; Creates an indirect branch prediction and L1D barrier on CPUs that need and supports that.
; @clobbers eax, edx, ecx
; @param    1   How to address CPUMCTX.
; @param    2   Which IBPB flag to test for (CPUMCTX_WSF_IBPB_ENTRY or CPUMCTX_WSF_IBPB_EXIT)
; @param    3   Which FLUSH flag to test for (CPUMCTX_WSF_L1D_ENTRY)
; @param    4   Which MDS flag to test for (CPUMCTX_WSF_MDS_ENTRY)
%macro INDIRECT_BRANCH_PREDICTION_AND_L1_CACHE_BARRIER 4
    ; Only one test+jmp when disabled CPUs.
    test    byte [%1 + CPUMCTX.fWorldSwitcher], (%2 | %3 | %4)
    jz      %%no_barrier_needed

    ; The eax:edx value is the same for both.
    AssertCompile(MSR_IA32_PRED_CMD_F_IBPB == MSR_IA32_FLUSH_CMD_F_L1D)
    mov     eax, MSR_IA32_PRED_CMD_F_IBPB
    xor     edx, edx

    ; Indirect branch barrier.
    test    byte [%1 + CPUMCTX.fWorldSwitcher], %2
    jz      %%no_indirect_branch_barrier
    mov     ecx, MSR_IA32_PRED_CMD
    wrmsr
%%no_indirect_branch_barrier:

    ; Level 1 data cache flush.
    test    byte [%1 + CPUMCTX.fWorldSwitcher], %3
    jz      %%no_cache_flush_barrier
    mov     ecx, MSR_IA32_FLUSH_CMD
    wrmsr
    jmp     %%no_mds_buffer_flushing    ; MDS flushing is included in L1D_FLUSH
%%no_cache_flush_barrier:

    ; MDS buffer flushing.
    test    byte [%1 + CPUMCTX.fWorldSwitcher], %4
    jz      %%no_mds_buffer_flushing
    sub     xSP, xSP
    mov     [xSP], ds
    verw    [xSP]
    add     xSP, xSP
%%no_mds_buffer_flushing:

%%no_barrier_needed:
%endmacro


;*********************************************************************************************************************************
;*  External Symbols                                                                                                             *
;*********************************************************************************************************************************
%ifdef VBOX_WITH_KERNEL_USING_XMM
extern NAME(CPUMIsGuestFPUStateActive)
%endif


BEGINCODE


;;
; Restores host-state fields.
;
; @returns VBox status code
; @param   f32RestoreHost x86: [ebp + 08h]  msc: ecx  gcc: edi   RestoreHost flags.
; @param   pRestoreHost   x86: [ebp + 0ch]  msc: rdx  gcc: rsi   Pointer to the RestoreHost struct.
;
ALIGNCODE(16)
BEGINPROC VMXRestoreHostState
%ifndef ASM_CALL64_GCC
    ; Use GCC's input registers since we'll be needing both rcx and rdx further
    ; down with the wrmsr instruction.  Use the R10 and R11 register for saving
    ; RDI and RSI since MSC preserve the two latter registers.
    mov         r10, rdi
    mov         r11, rsi
    mov         rdi, rcx
    mov         rsi, rdx
%endif

    test        edi, VMX_RESTORE_HOST_GDTR
    jz          .test_idtr
    lgdt        [rsi + VMXRESTOREHOST.HostGdtr]

.test_idtr:
    test        edi, VMX_RESTORE_HOST_IDTR
    jz          .test_ds
    lidt        [rsi + VMXRESTOREHOST.HostIdtr]

.test_ds:
    test        edi, VMX_RESTORE_HOST_SEL_DS
    jz          .test_es
    mov         ax, [rsi + VMXRESTOREHOST.uHostSelDS]
    mov         ds, eax

.test_es:
    test        edi, VMX_RESTORE_HOST_SEL_ES
    jz          .test_tr
    mov         ax, [rsi + VMXRESTOREHOST.uHostSelES]
    mov         es, eax

.test_tr:
    test        edi, VMX_RESTORE_HOST_SEL_TR
    jz          .test_fs
    ; When restoring the TR, we must first clear the busy flag or we'll end up faulting.
    mov         dx, [rsi + VMXRESTOREHOST.uHostSelTR]
    mov         ax, dx
    and         eax, X86_SEL_MASK_OFF_RPL                       ; mask away TI and RPL bits leaving only the descriptor offset
    test        edi, VMX_RESTORE_HOST_GDT_READ_ONLY | VMX_RESTORE_HOST_GDT_NEED_WRITABLE
    jnz         .gdt_readonly
    add         rax, qword [rsi + VMXRESTOREHOST.HostGdtr + 2]  ; xAX <- descriptor offset + GDTR.pGdt.
    and         dword [rax + 4], ~RT_BIT(9)                     ; clear the busy flag in TSS desc (bits 0-7=base, bit 9=busy bit)
    ltr         dx
    jmp short   .test_fs
.gdt_readonly:
    test        edi, VMX_RESTORE_HOST_GDT_NEED_WRITABLE
    jnz         .gdt_readonly_need_writable
    mov         rcx, cr0
    mov         r9, rcx
    add         rax, qword [rsi + VMXRESTOREHOST.HostGdtr + 2]  ; xAX <- descriptor offset + GDTR.pGdt.
    and         rcx, ~X86_CR0_WP
    mov         cr0, rcx
    and         dword [rax + 4], ~RT_BIT(9)                     ; clear the busy flag in TSS desc (bits 0-7=base, bit 9=busy bit)
    ltr         dx
    mov         cr0, r9
    jmp short   .test_fs
.gdt_readonly_need_writable:
    add         rax, qword [rsi + VMXRESTOREHOST.HostGdtrRw + 2]  ; xAX <- descriptor offset + GDTR.pGdtRw
    and         dword [rax + 4], ~RT_BIT(9)                     ; clear the busy flag in TSS desc (bits 0-7=base, bit 9=busy bit)
    lgdt        [rsi + VMXRESTOREHOST.HostGdtrRw]
    ltr         dx
    lgdt        [rsi + VMXRESTOREHOST.HostGdtr]                 ; load the original GDT

.test_fs:
    ;
    ; When restoring the selector values for FS and GS, we'll temporarily trash
    ; the base address (at least the high 32-bit bits, but quite possibly the
    ; whole base address), the wrmsr will restore it correctly. (VT-x actually
    ; restores the base correctly when leaving guest mode, but not the selector
    ; value, so there is little problem with interrupts being enabled prior to
    ; this restore job.)
    ; We'll disable ints once for both FS and GS as that's probably faster.
    ;
    test        edi, VMX_RESTORE_HOST_SEL_FS | VMX_RESTORE_HOST_SEL_GS
    jz          .restore_success
    pushfq
    cli                                   ; (see above)

    test        edi, VMX_RESTORE_HOST_SEL_FS
    jz          .test_gs
    mov         ax, word [rsi + VMXRESTOREHOST.uHostSelFS]
    mov         fs, eax
    mov         eax, dword [rsi + VMXRESTOREHOST.uHostFSBase]         ; uHostFSBase - Lo
    mov         edx, dword [rsi + VMXRESTOREHOST.uHostFSBase + 4h]    ; uHostFSBase - Hi
    mov         ecx, MSR_K8_FS_BASE
    wrmsr

.test_gs:
    test        edi, VMX_RESTORE_HOST_SEL_GS
    jz          .restore_flags
    mov         ax, word [rsi + VMXRESTOREHOST.uHostSelGS]
    mov         gs, eax
    mov         eax, dword [rsi + VMXRESTOREHOST.uHostGSBase]         ; uHostGSBase - Lo
    mov         edx, dword [rsi + VMXRESTOREHOST.uHostGSBase + 4h]    ; uHostGSBase - Hi
    mov         ecx, MSR_K8_GS_BASE
    wrmsr

.restore_flags:
    popfq

.restore_success:
    mov         eax, VINF_SUCCESS
%ifndef ASM_CALL64_GCC
    ; Restore RDI and RSI on MSC.
    mov         rdi, r10
    mov         rsi, r11
%endif
    ret
ENDPROC VMXRestoreHostState


;;
; Dispatches an NMI to the host.
;
ALIGNCODE(16)
BEGINPROC VMXDispatchHostNmi
    ; NMI is always vector 2. The IDT[2] IRQ handler cannot be anything else. See Intel spec. 6.3.1 "External Interrupts".
    int 2
    ret
ENDPROC VMXDispatchHostNmi


%ifdef VBOX_WITH_KERNEL_USING_XMM

;;
; Wrapper around vmx.pfnStartVM that preserves host XMM registers and
; load the guest ones when necessary.
;
; @cproto       DECLASM(int) hmR0VMXStartVMWrapXMM(RTHCUINT fResume, PCPUMCTX pCtx, void *pvUnused, PVM pVM,
;                                                  PVMCPU pVCpu, PFNHMVMXSTARTVM pfnStartVM);
;
; @returns      eax
;
; @param        fResumeVM       msc:rcx
; @param        pCtx            msc:rdx
; @param        pvUnused        msc:r8
; @param        pVM             msc:r9
; @param        pVCpu           msc:[rbp+30h]   The cross context virtual CPU structure of the calling EMT.
; @param        pfnStartVM      msc:[rbp+38h]
;
; @remarks      This is essentially the same code as hmR0SVMRunWrapXMM, only the parameters differ a little bit.
;
; @remarks      Drivers shouldn't use AVX registers without saving+loading:
;                   https://msdn.microsoft.com/en-us/library/windows/hardware/ff545910%28v=vs.85%29.aspx?f=255&MSPPError=-2147217396
;               However the compiler docs have different idea:
;                   https://msdn.microsoft.com/en-us/library/9z1stfyw.aspx
;               We'll go with the former for now.
;
; ASSUMING 64-bit and windows for now.
;
ALIGNCODE(16)
BEGINPROC hmR0VMXStartVMWrapXMM
        push    xBP
        mov     xBP, xSP
        sub     xSP, 0b0h + 040h ; Don't bother optimizing the frame size.

        ; Spill input parameters.
        mov     [xBP + 010h], rcx       ; fResumeVM
        mov     [xBP + 018h], rdx       ; pCtx
        mov     [xBP + 020h], r8        ; pvUnused
        mov     [xBP + 028h], r9        ; pVM

        ; Ask CPUM whether we've started using the FPU yet.
        mov     rcx, [xBP + 30h]        ; pVCpu
        call    NAME(CPUMIsGuestFPUStateActive)
        test    al, al
        jnz     .guest_fpu_state_active

        ; No need to mess with XMM registers just call the start routine and return.
        mov     r11, [xBP + 38h]        ; pfnStartVM
        mov     r10, [xBP + 30h]        ; pVCpu
        mov     [xSP + 020h], r10
        mov     rcx, [xBP + 010h]       ; fResumeVM
        mov     rdx, [xBP + 018h]       ; pCtx
        mov     r8,  [xBP + 020h]       ; pvUnused
        mov     r9,  [xBP + 028h]       ; pVM
        call    r11

        leave
        ret

ALIGNCODE(8)
.guest_fpu_state_active:
        ; Save the non-volatile host XMM registers.
        movdqa  [rsp + 040h + 000h], xmm6
        movdqa  [rsp + 040h + 010h], xmm7
        movdqa  [rsp + 040h + 020h], xmm8
        movdqa  [rsp + 040h + 030h], xmm9
        movdqa  [rsp + 040h + 040h], xmm10
        movdqa  [rsp + 040h + 050h], xmm11
        movdqa  [rsp + 040h + 060h], xmm12
        movdqa  [rsp + 040h + 070h], xmm13
        movdqa  [rsp + 040h + 080h], xmm14
        movdqa  [rsp + 040h + 090h], xmm15
        stmxcsr [rsp + 040h + 0a0h]

        mov     r10, [xBP + 018h]       ; pCtx
        mov     eax, [r10 + CPUMCTX.fXStateMask]
        test    eax, eax
        jz      .guest_fpu_state_manually

        ;
        ; Using XSAVE to load the guest XMM, YMM and ZMM registers.
        ;
        and     eax, CPUM_VOLATILE_XSAVE_GUEST_COMPONENTS
        xor     edx, edx
        mov     r10, [r10 + CPUMCTX.pXStateR0]
        xrstor  [r10]

        ; Make the call (same as in the other case).
        mov     r11, [xBP + 38h]        ; pfnStartVM
        mov     r10, [xBP + 30h]        ; pVCpu
        mov     [xSP + 020h], r10
        mov     rcx, [xBP + 010h]       ; fResumeVM
        mov     rdx, [xBP + 018h]       ; pCtx
        mov     r8,  [xBP + 020h]       ; pvUnused
        mov     r9,  [xBP + 028h]       ; pVM
        call    r11

        mov     r11d, eax               ; save return value (xsave below uses eax)

        ; Save the guest XMM registers.
        mov     r10, [xBP + 018h]       ; pCtx
        mov     eax, [r10 + CPUMCTX.fXStateMask]
        and     eax, CPUM_VOLATILE_XSAVE_GUEST_COMPONENTS
        xor     edx, edx
        mov     r10, [r10 + CPUMCTX.pXStateR0]
        xsave  [r10]

        mov     eax, r11d               ; restore return value

.restore_non_volatile_host_xmm_regs:
        ; Load the non-volatile host XMM registers.
        movdqa  xmm6,  [rsp + 040h + 000h]
        movdqa  xmm7,  [rsp + 040h + 010h]
        movdqa  xmm8,  [rsp + 040h + 020h]
        movdqa  xmm9,  [rsp + 040h + 030h]
        movdqa  xmm10, [rsp + 040h + 040h]
        movdqa  xmm11, [rsp + 040h + 050h]
        movdqa  xmm12, [rsp + 040h + 060h]
        movdqa  xmm13, [rsp + 040h + 070h]
        movdqa  xmm14, [rsp + 040h + 080h]
        movdqa  xmm15, [rsp + 040h + 090h]
        ldmxcsr        [rsp + 040h + 0a0h]
        leave
        ret

        ;
        ; No XSAVE, load and save the guest XMM registers manually.
        ;
.guest_fpu_state_manually:
        ; Load the full guest XMM register state.
        mov     r10, [r10 + CPUMCTX.pXStateR0]
        movdqa  xmm0,  [r10 + XMM_OFF_IN_X86FXSTATE + 000h]
        movdqa  xmm1,  [r10 + XMM_OFF_IN_X86FXSTATE + 010h]
        movdqa  xmm2,  [r10 + XMM_OFF_IN_X86FXSTATE + 020h]
        movdqa  xmm3,  [r10 + XMM_OFF_IN_X86FXSTATE + 030h]
        movdqa  xmm4,  [r10 + XMM_OFF_IN_X86FXSTATE + 040h]
        movdqa  xmm5,  [r10 + XMM_OFF_IN_X86FXSTATE + 050h]
        movdqa  xmm6,  [r10 + XMM_OFF_IN_X86FXSTATE + 060h]
        movdqa  xmm7,  [r10 + XMM_OFF_IN_X86FXSTATE + 070h]
        movdqa  xmm8,  [r10 + XMM_OFF_IN_X86FXSTATE + 080h]
        movdqa  xmm9,  [r10 + XMM_OFF_IN_X86FXSTATE + 090h]
        movdqa  xmm10, [r10 + XMM_OFF_IN_X86FXSTATE + 0a0h]
        movdqa  xmm11, [r10 + XMM_OFF_IN_X86FXSTATE + 0b0h]
        movdqa  xmm12, [r10 + XMM_OFF_IN_X86FXSTATE + 0c0h]
        movdqa  xmm13, [r10 + XMM_OFF_IN_X86FXSTATE + 0d0h]
        movdqa  xmm14, [r10 + XMM_OFF_IN_X86FXSTATE + 0e0h]
        movdqa  xmm15, [r10 + XMM_OFF_IN_X86FXSTATE + 0f0h]
        ldmxcsr        [r10 + X86FXSTATE.MXCSR]

        ; Make the call (same as in the other case).
        mov     r11, [xBP + 38h]        ; pfnStartVM
        mov     r10, [xBP + 30h]        ; pVCpu
        mov     [xSP + 020h], r10
        mov     rcx, [xBP + 010h]       ; fResumeVM
        mov     rdx, [xBP + 018h]       ; pCtx
        mov     r8,  [xBP + 020h]       ; pvUnused
        mov     r9,  [xBP + 028h]       ; pVM
        call    r11

        ; Save the guest XMM registers.
        mov     r10, [xBP + 018h]       ; pCtx
        mov     r10, [r10 + CPUMCTX.pXStateR0]
        stmxcsr [r10 + X86FXSTATE.MXCSR]
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 000h], xmm0
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 010h], xmm1
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 020h], xmm2
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 030h], xmm3
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 040h], xmm4
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 050h], xmm5
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 060h], xmm6
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 070h], xmm7
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 080h], xmm8
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 090h], xmm9
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 0a0h], xmm10
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 0b0h], xmm11
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 0c0h], xmm12
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 0d0h], xmm13
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 0e0h], xmm14
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 0f0h], xmm15
        jmp     .restore_non_volatile_host_xmm_regs
ENDPROC   hmR0VMXStartVMWrapXMM

;;
; Wrapper around svm.pfnVMRun that preserves host XMM registers and
; load the guest ones when necessary.
;
; @cproto       DECLASM(int) hmR0SVMRunWrapXMM(RTHCPHYS HCPhysVmcbHost, RTHCPHYS HCPhysVmcb, PCPUMCTX pCtx, PVM pVM, PVMCPU pVCpu,
;                                              PFNHMSVMVMRUN pfnVMRun);
;
; @returns      eax
;
; @param        HCPhysVmcbHost  msc:rcx
; @param        HCPhysVmcb      msc:rdx
; @param        pCtx            msc:r8
; @param        pVM             msc:r9
; @param        pVCpu           msc:[rbp+30h]   The cross context virtual CPU structure of the calling EMT.
; @param        pfnVMRun        msc:[rbp+38h]
;
; @remarks      This is essentially the same code as hmR0VMXStartVMWrapXMM, only the parameters differ a little bit.
;
; @remarks      Drivers shouldn't use AVX registers without saving+loading:
;                   https://msdn.microsoft.com/en-us/library/windows/hardware/ff545910%28v=vs.85%29.aspx?f=255&MSPPError=-2147217396
;               However the compiler docs have different idea:
;                   https://msdn.microsoft.com/en-us/library/9z1stfyw.aspx
;               We'll go with the former for now.
;
; ASSUMING 64-bit and windows for now.
ALIGNCODE(16)
BEGINPROC hmR0SVMRunWrapXMM
        push    xBP
        mov     xBP, xSP
        sub     xSP, 0b0h + 040h        ; don't bother optimizing the frame size

        ; Spill input parameters.
        mov     [xBP + 010h], rcx       ; HCPhysVmcbHost
        mov     [xBP + 018h], rdx       ; HCPhysVmcb
        mov     [xBP + 020h], r8        ; pCtx
        mov     [xBP + 028h], r9        ; pVM

        ; Ask CPUM whether we've started using the FPU yet.
        mov     rcx, [xBP + 30h]        ; pVCpu
        call    NAME(CPUMIsGuestFPUStateActive)
        test    al, al
        jnz     .guest_fpu_state_active

        ; No need to mess with XMM registers just call the start routine and return.
        mov     r11, [xBP + 38h]        ; pfnVMRun
        mov     r10, [xBP + 30h]        ; pVCpu
        mov     [xSP + 020h], r10
        mov     rcx, [xBP + 010h]       ; HCPhysVmcbHost
        mov     rdx, [xBP + 018h]       ; HCPhysVmcb
        mov     r8,  [xBP + 020h]       ; pCtx
        mov     r9,  [xBP + 028h]       ; pVM
        call    r11

        leave
        ret

ALIGNCODE(8)
.guest_fpu_state_active:
        ; Save the non-volatile host XMM registers.
        movdqa  [rsp + 040h + 000h], xmm6
        movdqa  [rsp + 040h + 010h], xmm7
        movdqa  [rsp + 040h + 020h], xmm8
        movdqa  [rsp + 040h + 030h], xmm9
        movdqa  [rsp + 040h + 040h], xmm10
        movdqa  [rsp + 040h + 050h], xmm11
        movdqa  [rsp + 040h + 060h], xmm12
        movdqa  [rsp + 040h + 070h], xmm13
        movdqa  [rsp + 040h + 080h], xmm14
        movdqa  [rsp + 040h + 090h], xmm15
        stmxcsr [rsp + 040h + 0a0h]

        mov     r10, [xBP + 020h]       ; pCtx
        mov     eax, [r10 + CPUMCTX.fXStateMask]
        test    eax, eax
        jz      .guest_fpu_state_manually

        ;
        ; Using XSAVE.
        ;
        and     eax, CPUM_VOLATILE_XSAVE_GUEST_COMPONENTS
        xor     edx, edx
        mov     r10, [r10 + CPUMCTX.pXStateR0]
        xrstor  [r10]

        ; Make the call (same as in the other case).
        mov     r11, [xBP + 38h]        ; pfnVMRun
        mov     r10, [xBP + 30h]        ; pVCpu
        mov     [xSP + 020h], r10
        mov     rcx, [xBP + 010h]       ; HCPhysVmcbHost
        mov     rdx, [xBP + 018h]       ; HCPhysVmcb
        mov     r8,  [xBP + 020h]       ; pCtx
        mov     r9,  [xBP + 028h]       ; pVM
        call    r11

        mov     r11d, eax               ; save return value (xsave below uses eax)

        ; Save the guest XMM registers.
        mov     r10, [xBP + 020h]       ; pCtx
        mov     eax, [r10 + CPUMCTX.fXStateMask]
        and     eax, CPUM_VOLATILE_XSAVE_GUEST_COMPONENTS
        xor     edx, edx
        mov     r10, [r10 + CPUMCTX.pXStateR0]
        xsave  [r10]

        mov     eax, r11d               ; restore return value

.restore_non_volatile_host_xmm_regs:
        ; Load the non-volatile host XMM registers.
        movdqa  xmm6,  [rsp + 040h + 000h]
        movdqa  xmm7,  [rsp + 040h + 010h]
        movdqa  xmm8,  [rsp + 040h + 020h]
        movdqa  xmm9,  [rsp + 040h + 030h]
        movdqa  xmm10, [rsp + 040h + 040h]
        movdqa  xmm11, [rsp + 040h + 050h]
        movdqa  xmm12, [rsp + 040h + 060h]
        movdqa  xmm13, [rsp + 040h + 070h]
        movdqa  xmm14, [rsp + 040h + 080h]
        movdqa  xmm15, [rsp + 040h + 090h]
        ldmxcsr [rsp + 040h + 0a0h]
        leave
        ret

        ;
        ; No XSAVE, load and save the guest XMM registers manually.
        ;
.guest_fpu_state_manually:
        ; Load the full guest XMM register state.
        mov     r10, [r10 + CPUMCTX.pXStateR0]
        movdqa  xmm0,  [r10 + XMM_OFF_IN_X86FXSTATE + 000h]
        movdqa  xmm1,  [r10 + XMM_OFF_IN_X86FXSTATE + 010h]
        movdqa  xmm2,  [r10 + XMM_OFF_IN_X86FXSTATE + 020h]
        movdqa  xmm3,  [r10 + XMM_OFF_IN_X86FXSTATE + 030h]
        movdqa  xmm4,  [r10 + XMM_OFF_IN_X86FXSTATE + 040h]
        movdqa  xmm5,  [r10 + XMM_OFF_IN_X86FXSTATE + 050h]
        movdqa  xmm6,  [r10 + XMM_OFF_IN_X86FXSTATE + 060h]
        movdqa  xmm7,  [r10 + XMM_OFF_IN_X86FXSTATE + 070h]
        movdqa  xmm8,  [r10 + XMM_OFF_IN_X86FXSTATE + 080h]
        movdqa  xmm9,  [r10 + XMM_OFF_IN_X86FXSTATE + 090h]
        movdqa  xmm10, [r10 + XMM_OFF_IN_X86FXSTATE + 0a0h]
        movdqa  xmm11, [r10 + XMM_OFF_IN_X86FXSTATE + 0b0h]
        movdqa  xmm12, [r10 + XMM_OFF_IN_X86FXSTATE + 0c0h]
        movdqa  xmm13, [r10 + XMM_OFF_IN_X86FXSTATE + 0d0h]
        movdqa  xmm14, [r10 + XMM_OFF_IN_X86FXSTATE + 0e0h]
        movdqa  xmm15, [r10 + XMM_OFF_IN_X86FXSTATE + 0f0h]
        ldmxcsr        [r10 + X86FXSTATE.MXCSR]

        ; Make the call (same as in the other case).
        mov     r11, [xBP + 38h]        ; pfnVMRun
        mov     r10, [xBP + 30h]        ; pVCpu
        mov     [xSP + 020h], r10
        mov     rcx, [xBP + 010h]       ; HCPhysVmcbHost
        mov     rdx, [xBP + 018h]       ; HCPhysVmcb
        mov     r8,  [xBP + 020h]       ; pCtx
        mov     r9,  [xBP + 028h]       ; pVM
        call    r11

        ; Save the guest XMM registers.
        mov     r10, [xBP + 020h]       ; pCtx
        mov     r10, [r10 + CPUMCTX.pXStateR0]
        stmxcsr [r10 + X86FXSTATE.MXCSR]
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 000h], xmm0
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 010h], xmm1
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 020h], xmm2
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 030h], xmm3
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 040h], xmm4
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 050h], xmm5
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 060h], xmm6
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 070h], xmm7
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 080h], xmm8
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 090h], xmm9
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 0a0h], xmm10
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 0b0h], xmm11
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 0c0h], xmm12
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 0d0h], xmm13
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 0e0h], xmm14
        movdqa  [r10 + XMM_OFF_IN_X86FXSTATE + 0f0h], xmm15
        jmp     .restore_non_volatile_host_xmm_regs
ENDPROC   hmR0SVMRunWrapXMM

%endif ; VBOX_WITH_KERNEL_USING_XMM


;; @def RESTORE_STATE_VM64
; Macro restoring essential host state and updating guest state
; for 64-bit host, 64-bit guest for VT-x.
;
%macro RESTORE_STATE_VM64 0
    ; Restore base and limit of the IDTR & GDTR.
 %ifndef VMX_SKIP_IDTR
    lidt    [xSP]
    add     xSP, xCB * 2
 %endif
 %ifndef VMX_SKIP_GDTR
    lgdt    [xSP]
    add     xSP, xCB * 2
 %endif

    push    xDI
 %ifndef VMX_SKIP_TR
    mov     xDI, [xSP + xCB * 3]        ; pCtx (*3 to skip the saved xDI, TR, LDTR)
 %else
    mov     xDI, [xSP + xCB * 2]        ; pCtx (*2 to skip the saved xDI, LDTR)
 %endif

    mov     qword [xDI + CPUMCTX.eax], rax
    mov     rax, SPECTRE_FILLER
    mov     qword [xDI + CPUMCTX.ebx], rbx
    mov     rbx, rax
    mov     qword [xDI + CPUMCTX.ecx], rcx
    mov     rcx, rax
    mov     qword [xDI + CPUMCTX.edx], rdx
    mov     rdx, rax
    mov     qword [xDI + CPUMCTX.esi], rsi
    mov     rsi, rax
    mov     qword [xDI + CPUMCTX.ebp], rbp
    mov     rbp, rax
    mov     qword [xDI + CPUMCTX.r8],  r8
    mov     r8, rax
    mov     qword [xDI + CPUMCTX.r9],  r9
    mov     r9, rax
    mov     qword [xDI + CPUMCTX.r10], r10
    mov     r10, rax
    mov     qword [xDI + CPUMCTX.r11], r11
    mov     r11, rax
    mov     qword [xDI + CPUMCTX.r12], r12
    mov     r12, rax
    mov     qword [xDI + CPUMCTX.r13], r13
    mov     r13, rax
    mov     qword [xDI + CPUMCTX.r14], r14
    mov     r14, rax
    mov     qword [xDI + CPUMCTX.r15], r15
    mov     r15, rax
    mov     rax, cr2
    mov     qword [xDI + CPUMCTX.cr2], rax

    pop     xAX                                 ; The guest rdi we pushed above
    mov     qword [xDI + CPUMCTX.edi], rax

    ; Fight spectre.
    INDIRECT_BRANCH_PREDICTION_BARRIER xDI, CPUMCTX_WSF_IBPB_EXIT

 %ifndef VMX_SKIP_TR
    ; Restore TSS selector; must mark it as not busy before using ltr!
    ; ASSUME that this is supposed to be 'BUSY' (saves 20-30 ticks on the T42p).
    ; @todo get rid of sgdt
    pop     xBX         ; Saved TR
    sub     xSP, xCB * 2
    sgdt    [xSP]
    mov     xAX, xBX
    and     eax, X86_SEL_MASK_OFF_RPL           ; mask away TI and RPL bits leaving only the descriptor offset
    add     xAX, [xSP + 2]                      ; eax <- GDTR.address + descriptor offset
    and     dword [xAX + 4], ~RT_BIT(9)         ; clear the busy flag in TSS desc (bits 0-7=base, bit 9=busy bit)
    ltr     bx
    add     xSP, xCB * 2
 %endif

    pop     xAX         ; Saved LDTR
    cmp     eax, 0
    je      %%skip_ldt_write64
    lldt    ax

%%skip_ldt_write64:
    pop     xSI         ; pCtx (needed in rsi by the macros below)

    ; Restore segment registers.
    POP_RELEVANT_SEGMENT_REGISTERS xAX, ax

    ; Restore the host XCR0 if necessary.
    pop     xCX
    test    ecx, ecx
    jnz     %%xcr0_after_skip
    pop     xAX
    pop     xDX
    xsetbv                              ; ecx is already zero.
%%xcr0_after_skip:

    ; Restore general purpose registers.
    POP_CALLEE_PRESERVED_REGISTERS
%endmacro


;;
; Prepares for and executes VMLAUNCH/VMRESUME (64 bits guest mode)
;
; @returns VBox status code
; @param    fResume    msc:rcx, gcc:rdi       Whether to use vmlauch/vmresume.
; @param    pCtx       msc:rdx, gcc:rsi       Pointer to the guest-CPU context.
; @param    pvUnused   msc:r8,  gcc:rdx       Unused argument.
; @param    pVM        msc:r9,  gcc:rcx       The cross context VM structure.
; @param    pVCpu      msc:[ebp+30], gcc:r8   The cross context virtual CPU structure of the calling EMT.
;
ALIGNCODE(16)
BEGINPROC VMXR0StartVM64
    push    xBP
    mov     xBP, xSP

    pushf
    cli

    ; Save all general purpose host registers.
    PUSH_CALLEE_PRESERVED_REGISTERS

    ; First we have to save some final CPU context registers.
    lea     r10, [.vmlaunch64_done wrt rip]
    mov     rax, VMX_VMCS_HOST_RIP      ; return address (too difficult to continue after VMLAUNCH?)
    vmwrite rax, r10
    ; Note: ASSUMES success!

    ;
    ; Unify the input parameter registers.
    ;
%ifdef ASM_CALL64_GCC
    ; fResume already in rdi
    ; pCtx    already in rsi
    mov     rbx, rdx        ; pvUnused
%else
    mov     rdi, rcx        ; fResume
    mov     rsi, rdx        ; pCtx
    mov     rbx, r8         ; pvUnused
%endif

    ;
    ; Save the host XCR0 and load the guest one if necessary.
    ; Note! Trashes rdx and rcx.
    ;
%ifdef ASM_CALL64_MSC
    mov     rax, [xBP + 30h]            ; pVCpu
%else
    mov     rax, r8                     ; pVCpu
%endif
    test    byte [xAX + VMCPU.hm + HMCPU.fLoadSaveGuestXcr0], 1
    jz      .xcr0_before_skip

    xor     ecx, ecx
    xgetbv                              ; save the host one on the stack
    push    xDX
    push    xAX

    mov     eax, [xSI + CPUMCTX.aXcr]   ; load the guest one
    mov     edx, [xSI + CPUMCTX.aXcr + 4]
    xor     ecx, ecx                    ; paranoia
    xsetbv

    push    0                           ; indicate that we must restore XCR0 (popped into ecx, thus 0)
    jmp     .xcr0_before_done

.xcr0_before_skip:
    push    3fh                         ; indicate that we need not
.xcr0_before_done:

    ;
    ; Save segment registers.
    ; Note! Trashes rdx & rcx, so we moved it here (amd64 case).
    ;
    PUSH_RELEVANT_SEGMENT_REGISTERS xAX, ax

    ; Save the pCtx pointer.
    push    xSI

    ; Save host LDTR.
    xor     eax, eax
    sldt    ax
    push    xAX

%ifndef VMX_SKIP_TR
    ; The host TR limit is reset to 0x67; save & restore it manually.
    str     eax
    push    xAX
%endif

%ifndef VMX_SKIP_GDTR
    ; VT-x only saves the base of the GDTR & IDTR and resets the limit to 0xffff; we must restore the limit correctly!
    sub     xSP, xCB * 2
    sgdt    [xSP]
%endif
%ifndef VMX_SKIP_IDTR
    sub     xSP, xCB * 2
    sidt    [xSP]
%endif

    ; Load CR2 if necessary (may be expensive as writing CR2 is a synchronizing instruction).
    mov     rbx, qword [xSI + CPUMCTX.cr2]
    mov     rdx, cr2
    cmp     rbx, rdx
    je      .skip_cr2_write
    mov     cr2, rbx

.skip_cr2_write:
    mov     eax, VMX_VMCS_HOST_RSP
    vmwrite xAX, xSP
    ; Note: ASSUMES success!
    ; Don't mess with ESP anymore!!!

    ; Fight spectre and similar.
    INDIRECT_BRANCH_PREDICTION_AND_L1_CACHE_BARRIER xSI, CPUMCTX_WSF_IBPB_ENTRY, CPUMCTX_WSF_L1D_ENTRY, CPUMCTX_WSF_MDS_ENTRY

    ; Load guest general purpose registers.
    mov     rax, qword [xSI + CPUMCTX.eax]
    mov     rbx, qword [xSI + CPUMCTX.ebx]
    mov     rcx, qword [xSI + CPUMCTX.ecx]
    mov     rdx, qword [xSI + CPUMCTX.edx]
    mov     rbp, qword [xSI + CPUMCTX.ebp]
    mov     r8,  qword [xSI + CPUMCTX.r8]
    mov     r9,  qword [xSI + CPUMCTX.r9]
    mov     r10, qword [xSI + CPUMCTX.r10]
    mov     r11, qword [xSI + CPUMCTX.r11]
    mov     r12, qword [xSI + CPUMCTX.r12]
    mov     r13, qword [xSI + CPUMCTX.r13]
    mov     r14, qword [xSI + CPUMCTX.r14]
    mov     r15, qword [xSI + CPUMCTX.r15]

    ; Resume or start VM?
    cmp     xDI, 0                  ; fResume

    ; Load guest rdi & rsi.
    mov     rdi, qword [xSI + CPUMCTX.edi]
    mov     rsi, qword [xSI + CPUMCTX.esi]

    je      .vmlaunch64_launch

    vmresume
    jc      near .vmxstart64_invalid_vmcs_ptr
    jz      near .vmxstart64_start_failed
    jmp     .vmlaunch64_done;      ; here if vmresume detected a failure

.vmlaunch64_launch:
    vmlaunch
    jc      near .vmxstart64_invalid_vmcs_ptr
    jz      near .vmxstart64_start_failed
    jmp     .vmlaunch64_done;      ; here if vmlaunch detected a failure

ALIGNCODE(16)
.vmlaunch64_done:
    RESTORE_STATE_VM64
    mov     eax, VINF_SUCCESS

.vmstart64_end:
    popf
    pop     xBP
    ret

.vmxstart64_invalid_vmcs_ptr:
    RESTORE_STATE_VM64
    mov     eax, VERR_VMX_INVALID_VMCS_PTR_TO_START_VM
    jmp     .vmstart64_end

.vmxstart64_start_failed:
    RESTORE_STATE_VM64
    mov     eax, VERR_VMX_UNABLE_TO_START_VM
    jmp     .vmstart64_end
ENDPROC VMXR0StartVM64


;;
; Clears the MDS buffers using VERW.
ALIGNCODE(16)
BEGINPROC hmR0MdsClear
        sub     xSP, xCB
        mov     [xSP], ds
        verw    [xSP]
        add     xSP, xCB
        ret
ENDPROC   hmR0MdsClear


;;
; Prepares for and executes VMRUN (32-bit and 64-bit guests).
;
; @returns  VBox status code
; @param    HCPhysVmcbHost  msc:rcx,gcc:rdi     Physical address of host VMCB.
; @param    HCPhysVmcb      msc:rdx,gcc:rsi     Physical address of guest VMCB.
; @param    pCtx            msc:r8,gcc:rdx      Pointer to the guest-CPU context.
; @param    pVM             msc:r9,gcc:rcx      The cross context VM structure.
; @param    pVCpu           msc:[rsp+28],gcc:r8 The cross context virtual CPU structure of the calling EMT.
;
ALIGNCODE(16)
BEGINPROC SVMR0VMRun
    ; Fake a cdecl stack frame
 %ifdef ASM_CALL64_GCC
    push    r8                ; pVCpu
    push    rcx               ; pVM
    push    rdx               ; pCtx
    push    rsi               ; HCPhysVmcb
    push    rdi               ; HCPhysVmcbHost
 %else
    mov     rax, [rsp + 28h]
    push    rax               ; rbp + 30h pVCpu
    push    r9                ; rbp + 28h pVM
    push    r8                ; rbp + 20h pCtx
    push    rdx               ; rbp + 18h HCPhysVmcb
    push    rcx               ; rbp + 10h HCPhysVmcbHost
 %endif
    push    0                 ; rbp + 08h "fake ret addr"
    push    rbp               ; rbp + 00h
    mov     rbp, rsp
    pushf

    ; Manual save and restore:
    ;  - General purpose registers except RIP, RSP, RAX
    ;
    ; Trashed:
    ;  - CR2 (we don't care)
    ;  - LDTR (reset to 0)
    ;  - DRx (presumably not changed at all)
    ;  - DR7 (reset to 0x400)

    ; Save all general purpose host registers.
    PUSH_CALLEE_PRESERVED_REGISTERS

    ; Load pCtx into xSI.
    mov     xSI, [rbp + xCB * 2 + RTHCPHYS_CB * 2]

    ; Save the host XCR0 and load the guest one if necessary.
    mov     rax, [xBP + 30h]                ; pVCpu
    test    byte [xAX + VMCPU.hm + HMCPU.fLoadSaveGuestXcr0], 1
    jz      .xcr0_before_skip

    xor     ecx, ecx
    xgetbv                                  ; save the host XCR0 on the stack
    push    xDX
    push    xAX

    mov     xSI, [xBP + xCB * 2 + RTHCPHYS_CB * 2]  ; pCtx
    mov     eax, [xSI + CPUMCTX.aXcr]       ; load the guest XCR0
    mov     edx, [xSI + CPUMCTX.aXcr + 4]
    xor     ecx, ecx                        ; paranoia
    xsetbv

    push    0                               ; indicate that we must restore XCR0 (popped into ecx, thus 0)
    jmp     .xcr0_before_done

.xcr0_before_skip:
    push    3fh                             ; indicate that we need not restore XCR0
.xcr0_before_done:

    ; Save guest CPU-context pointer for simplifying saving of the GPRs afterwards.
    push    rsi

    ; Save host fs, gs, sysenter msr etc.
    mov     rax, [rbp + xCB * 2]            ; HCPhysVmcbHost (64 bits physical address; x86: take low dword only)
    push    rax                             ; save for the vmload after vmrun
    vmsave

    ; Fight spectre.
    INDIRECT_BRANCH_PREDICTION_BARRIER xSI, CPUMCTX_WSF_IBPB_ENTRY

    ; Setup rax for VMLOAD.
    mov     rax, [rbp + xCB * 2 + RTHCPHYS_CB] ; HCPhysVmcb (64 bits physical address; take low dword only)

    ; Load guest general purpose registers (rax is loaded from the VMCB by VMRUN).
    mov     rbx, qword [xSI + CPUMCTX.ebx]
    mov     rcx, qword [xSI + CPUMCTX.ecx]
    mov     rdx, qword [xSI + CPUMCTX.edx]
    mov     rdi, qword [xSI + CPUMCTX.edi]
    mov     rbp, qword [xSI + CPUMCTX.ebp]
    mov     r8,  qword [xSI + CPUMCTX.r8]
    mov     r9,  qword [xSI + CPUMCTX.r9]
    mov     r10, qword [xSI + CPUMCTX.r10]
    mov     r11, qword [xSI + CPUMCTX.r11]
    mov     r12, qword [xSI + CPUMCTX.r12]
    mov     r13, qword [xSI + CPUMCTX.r13]
    mov     r14, qword [xSI + CPUMCTX.r14]
    mov     r15, qword [xSI + CPUMCTX.r15]
    mov     rsi, qword [xSI + CPUMCTX.esi]

    ; Clear the global interrupt flag & execute sti to make sure external interrupts cause a world switch.
    clgi
    sti

    ; Load guest FS, GS, Sysenter MSRs etc.
    vmload

    ; Run the VM.
    vmrun

    ; Save guest fs, gs, sysenter msr etc.
    vmsave

    ; Load host fs, gs, sysenter msr etc.
    pop     rax                         ; load HCPhysVmcbHost (pushed above)
    vmload

    ; Set the global interrupt flag again, but execute cli to make sure IF=0.
    cli
    stgi

    ; Pop the context pointer (pushed above) and save the guest GPRs (sans RSP and RAX).
    pop     rax

    mov     qword [rax + CPUMCTX.ebx], rbx
    mov     rbx, SPECTRE_FILLER
    mov     qword [rax + CPUMCTX.ecx], rcx
    mov     rcx, rbx
    mov     qword [rax + CPUMCTX.edx], rdx
    mov     rdx, rbx
    mov     qword [rax + CPUMCTX.esi], rsi
    mov     rsi, rbx
    mov     qword [rax + CPUMCTX.edi], rdi
    mov     rdi, rbx
    mov     qword [rax + CPUMCTX.ebp], rbp
    mov     rbp, rbx
    mov     qword [rax + CPUMCTX.r8],  r8
    mov     r8, rbx
    mov     qword [rax + CPUMCTX.r9],  r9
    mov     r9, rbx
    mov     qword [rax + CPUMCTX.r10], r10
    mov     r10, rbx
    mov     qword [rax + CPUMCTX.r11], r11
    mov     r11, rbx
    mov     qword [rax + CPUMCTX.r12], r12
    mov     r12, rbx
    mov     qword [rax + CPUMCTX.r13], r13
    mov     r13, rbx
    mov     qword [rax + CPUMCTX.r14], r14
    mov     r14, rbx
    mov     qword [rax + CPUMCTX.r15], r15
    mov     r15, rbx

    ; Fight spectre.  Note! Trashes rax!
    INDIRECT_BRANCH_PREDICTION_BARRIER rax, CPUMCTX_WSF_IBPB_EXIT

    ; Restore the host xcr0 if necessary.
    pop     xCX
    test    ecx, ecx
    jnz     .xcr0_after_skip
    pop     xAX
    pop     xDX
    xsetbv                              ; ecx is already zero
.xcr0_after_skip:

    ; Restore host general purpose registers.
    POP_CALLEE_PRESERVED_REGISTERS

    mov     eax, VINF_SUCCESS

    popf
    pop     rbp
    add     rsp, 6 * xCB
    ret
ENDPROC SVMR0VMRun


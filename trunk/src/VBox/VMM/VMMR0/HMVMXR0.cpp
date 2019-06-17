/* $Id$ */
/** @file
 * HM VMX (Intel VT-x) - Host Context Ring-0.
 */

/*
 * Copyright (C) 2012-2019 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_HM
#define VMCPU_INCL_CPUM_GST_CTX
#include <iprt/x86.h>
#include <iprt/asm-amd64-x86.h>
#include <iprt/thread.h>

#include <VBox/vmm/pdmapi.h>
#include <VBox/vmm/dbgf.h>
#include <VBox/vmm/iem.h>
#include <VBox/vmm/iom.h>
#include <VBox/vmm/selm.h>
#include <VBox/vmm/tm.h>
#include <VBox/vmm/em.h>
#include <VBox/vmm/gim.h>
#include <VBox/vmm/apic.h>
#ifdef VBOX_WITH_REM
# include <VBox/vmm/rem.h>
#endif
#include "HMInternal.h"
#include <VBox/vmm/vm.h>
#include <VBox/vmm/hmvmxinline.h>
#include "HMVMXR0.h"
#include "dtrace/VBoxVMM.h"

#ifdef DEBUG_ramshankar
# define HMVMX_ALWAYS_SAVE_GUEST_RFLAGS
# define HMVMX_ALWAYS_SAVE_FULL_GUEST_STATE
# define HMVMX_ALWAYS_SYNC_FULL_GUEST_STATE
# define HMVMX_ALWAYS_CHECK_GUEST_STATE
# define HMVMX_ALWAYS_TRAP_ALL_XCPTS
# define HMVMX_ALWAYS_TRAP_PF
# define HMVMX_ALWAYS_FLUSH_TLB
# define HMVMX_ALWAYS_SWAP_EFER
#endif


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/** Use the function table. */
#define HMVMX_USE_FUNCTION_TABLE

/** Determine which tagged-TLB flush handler to use. */
#define HMVMX_FLUSH_TAGGED_TLB_EPT_VPID             0
#define HMVMX_FLUSH_TAGGED_TLB_EPT                  1
#define HMVMX_FLUSH_TAGGED_TLB_VPID                 2
#define HMVMX_FLUSH_TAGGED_TLB_NONE                 3

/** @name HMVMX_READ_XXX
 * Flags to skip redundant reads of some common VMCS fields that are not part of
 * the guest-CPU or VCPU state but are needed while handling VM-exits.
 */
#define HMVMX_READ_IDT_VECTORING_INFO               RT_BIT_32(0)
#define HMVMX_READ_IDT_VECTORING_ERROR_CODE         RT_BIT_32(1)
#define HMVMX_READ_EXIT_QUALIFICATION               RT_BIT_32(2)
#define HMVMX_READ_EXIT_INSTR_LEN                   RT_BIT_32(3)
#define HMVMX_READ_EXIT_INTERRUPTION_INFO           RT_BIT_32(4)
#define HMVMX_READ_EXIT_INTERRUPTION_ERROR_CODE     RT_BIT_32(5)
#define HMVMX_READ_EXIT_INSTR_INFO                  RT_BIT_32(6)
#define HMVMX_READ_GUEST_LINEAR_ADDR                RT_BIT_32(7)
/** @} */

/**
 * Subset of the guest-CPU state that is kept by VMX R0 code while executing the
 * guest using hardware-assisted VMX.
 *
 * This excludes state like GPRs (other than RSP) which are always are
 * swapped and restored across the world-switch and also registers like EFER,
 * MSR which cannot be modified by the guest without causing a VM-exit.
 */
#define HMVMX_CPUMCTX_EXTRN_ALL      (  CPUMCTX_EXTRN_RIP             \
                                      | CPUMCTX_EXTRN_RFLAGS          \
                                      | CPUMCTX_EXTRN_RSP             \
                                      | CPUMCTX_EXTRN_SREG_MASK       \
                                      | CPUMCTX_EXTRN_TABLE_MASK      \
                                      | CPUMCTX_EXTRN_KERNEL_GS_BASE  \
                                      | CPUMCTX_EXTRN_SYSCALL_MSRS    \
                                      | CPUMCTX_EXTRN_SYSENTER_MSRS   \
                                      | CPUMCTX_EXTRN_TSC_AUX         \
                                      | CPUMCTX_EXTRN_OTHER_MSRS      \
                                      | CPUMCTX_EXTRN_CR0             \
                                      | CPUMCTX_EXTRN_CR3             \
                                      | CPUMCTX_EXTRN_CR4             \
                                      | CPUMCTX_EXTRN_DR7             \
                                      | CPUMCTX_EXTRN_HM_VMX_MASK)

/**
 * Exception bitmap mask for real-mode guests (real-on-v86).
 *
 * We need to intercept all exceptions manually except:
 * - \#AC and \#DB are always intercepted to prevent the CPU from deadlocking
 *   due to bugs in Intel CPUs.
 * - \#PF need not be intercepted even in real-mode if we have nested paging
 * support.
 */
#define HMVMX_REAL_MODE_XCPT_MASK    (  RT_BIT(X86_XCPT_DE)  /* always: | RT_BIT(X86_XCPT_DB) */ | RT_BIT(X86_XCPT_NMI)   \
                                      | RT_BIT(X86_XCPT_BP)             | RT_BIT(X86_XCPT_OF)    | RT_BIT(X86_XCPT_BR)    \
                                      | RT_BIT(X86_XCPT_UD)             | RT_BIT(X86_XCPT_NM)    | RT_BIT(X86_XCPT_DF)    \
                                      | RT_BIT(X86_XCPT_CO_SEG_OVERRUN) | RT_BIT(X86_XCPT_TS)    | RT_BIT(X86_XCPT_NP)    \
                                      | RT_BIT(X86_XCPT_SS)             | RT_BIT(X86_XCPT_GP)   /* RT_BIT(X86_XCPT_PF) */ \
                                      | RT_BIT(X86_XCPT_MF)  /* always: | RT_BIT(X86_XCPT_AC) */ | RT_BIT(X86_XCPT_MC)    \
                                      | RT_BIT(X86_XCPT_XF))

/** Maximum VM-instruction error number. */
#define HMVMX_INSTR_ERROR_MAX        28

/** Profiling macro. */
#ifdef HM_PROFILE_EXIT_DISPATCH
# define HMVMX_START_EXIT_DISPATCH_PROF()           STAM_PROFILE_ADV_START(&pVCpu->hm.s.StatExitDispatch, ed)
# define HMVMX_STOP_EXIT_DISPATCH_PROF()            STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatExitDispatch, ed)
#else
# define HMVMX_START_EXIT_DISPATCH_PROF()           do { } while (0)
# define HMVMX_STOP_EXIT_DISPATCH_PROF()            do { } while (0)
#endif

/** Assert that preemption is disabled or covered by thread-context hooks. */
#define HMVMX_ASSERT_PREEMPT_SAFE(a_pVCpu)          Assert(   VMMR0ThreadCtxHookIsEnabled((a_pVCpu))   \
                                                           || !RTThreadPreemptIsEnabled(NIL_RTTHREAD))

/** Assert that we haven't migrated CPUs when thread-context hooks are not
 *  used. */
#define HMVMX_ASSERT_CPU_SAFE(a_pVCpu)              AssertMsg(   VMMR0ThreadCtxHookIsEnabled((a_pVCpu)) \
                                                              || (a_pVCpu)->hm.s.idEnteredCpu == RTMpCpuId(), \
                                                              ("Illegal migration! Entered on CPU %u Current %u\n", \
                                                              (a_pVCpu)->hm.s.idEnteredCpu, RTMpCpuId()))

/** Asserts that the given CPUMCTX_EXTRN_XXX bits are present in the guest-CPU
 *  context. */
#define HMVMX_CPUMCTX_ASSERT(a_pVCpu, a_fExtrnMbz)  AssertMsg(!((a_pVCpu)->cpum.GstCtx.fExtrn & (a_fExtrnMbz)), \
                                                              ("fExtrn=%#RX64 fExtrnMbz=%#RX64\n", \
                                                              (a_pVCpu)->cpum.GstCtx.fExtrn, (a_fExtrnMbz)))

/** Helper macro for VM-exit handlers called unexpectedly. */
#define HMVMX_UNEXPECTED_EXIT_RET(a_pVCpu, a_HmError) \
    do { \
        (a_pVCpu)->hm.s.u32HMError = (a_HmError); \
        return VERR_VMX_UNEXPECTED_EXIT; \
    } while (0)

#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
/** Macro that does the necessary privilege checks and intercepted VM-exits for
 *  guests that attempted to execute a VMX instruction. */
# define HMVMX_CHECK_EXIT_DUE_TO_VMX_INSTR(a_pVCpu, a_uExitReason) \
    do \
    { \
        VBOXSTRICTRC rcStrictTmp = hmR0VmxCheckExitDueToVmxInstr((a_pVCpu), (a_uExitReason)); \
        if (rcStrictTmp == VINF_SUCCESS) \
        { /* likely */ } \
        else if (rcStrictTmp == VINF_HM_PENDING_XCPT) \
        { \
            Assert((a_pVCpu)->hm.s.Event.fPending); \
            Log4Func(("Privilege checks failed -> %#x\n", VMX_ENTRY_INT_INFO_VECTOR((a_pVCpu)->hm.s.Event.u64IntInfo))); \
            return VINF_SUCCESS; \
        } \
        else \
        { \
            int rcTmp = VBOXSTRICTRC_VAL(rcStrictTmp); \
            AssertMsgFailedReturn(("Unexpected failure. rc=%Rrc", rcTmp), rcTmp); \
        } \
    } while (0)

/** Macro that decodes a memory operand for an instruction VM-exit. */
# define HMVMX_DECODE_MEM_OPERAND(a_pVCpu, a_uExitInstrInfo, a_uExitQual, a_enmMemAccess, a_pGCPtrEffAddr) \
    do \
    { \
        VBOXSTRICTRC rcStrictTmp = hmR0VmxDecodeMemOperand((a_pVCpu), (a_uExitInstrInfo), (a_uExitQual), (a_enmMemAccess), \
                                                           (a_pGCPtrEffAddr)); \
        if (rcStrictTmp == VINF_SUCCESS) \
        { /* likely */ } \
        else if (rcStrictTmp == VINF_HM_PENDING_XCPT) \
        { \
            uint8_t const uXcptTmp = VMX_ENTRY_INT_INFO_VECTOR((a_pVCpu)->hm.s.Event.u64IntInfo); \
            Log4Func(("Memory operand decoding failed, raising xcpt %#x\n", uXcptTmp)); \
            NOREF(uXcptTmp); \
            return VINF_SUCCESS; \
        } \
        else \
        { \
            Log4Func(("hmR0VmxDecodeMemOperand failed. rc=%Rrc\n", VBOXSTRICTRC_VAL(rcStrictTmp))); \
            return rcStrictTmp; \
        } \
    } while (0)

#endif /* VBOX_WITH_NESTED_HWVIRT_VMX */


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/
/**
 * VMX transient state.
 *
 * A state structure for holding miscellaneous information across
 * VMX non-root operation and restored after the transition.
 */
typedef struct VMXTRANSIENT
{
    /** The host's rflags/eflags. */
    RTCCUINTREG         fEFlags;
#if HC_ARCH_BITS == 32
    uint32_t            u32Alignment0;
#endif
    /** The guest's TPR value used for TPR shadowing. */
    uint8_t             u8GuestTpr;
    /** Alignment. */
    uint8_t             abAlignment0[7];

    /** The basic VM-exit reason. */
    uint16_t            uExitReason;
    /** Alignment. */
    uint16_t            u16Alignment0;
    /** The VM-exit interruption error code. */
    uint32_t            uExitIntErrorCode;
    /** The VM-exit exit code qualification. */
    uint64_t            uExitQual;
    /** The Guest-linear address. */
    uint64_t            uGuestLinearAddr;

    /** The VM-exit interruption-information field. */
    uint32_t            uExitIntInfo;
    /** The VM-exit instruction-length field. */
    uint32_t            cbInstr;
    /** The VM-exit instruction-information field. */
    VMXEXITINSTRINFO    ExitInstrInfo;
    /** Whether the VM-entry failed or not. */
    bool                fVMEntryFailed;
    /** Whether we are currently executing a nested-guest. */
    bool                fIsNestedGuest;
    /** Alignment. */
    uint8_t             abAlignment1[2];

    /** The VM-entry interruption-information field. */
    uint32_t            uEntryIntInfo;
    /** The VM-entry exception error code field. */
    uint32_t            uEntryXcptErrorCode;
    /** The VM-entry instruction length field. */
    uint32_t            cbEntryInstr;

    /** IDT-vectoring information field. */
    uint32_t            uIdtVectoringInfo;
    /** IDT-vectoring error code. */
    uint32_t            uIdtVectoringErrorCode;

    /** Mask of currently read VMCS fields; HMVMX_READ_XXX. */
    uint32_t            fVmcsFieldsRead;

    /** Whether the guest debug state was active at the time of VM-exit. */
    bool                fWasGuestDebugStateActive;
    /** Whether the hyper debug state was active at the time of VM-exit. */
    bool                fWasHyperDebugStateActive;
    /** Whether TSC-offsetting and VMX-preemption timer was updated before VM-entry. */
    bool                fUpdatedTscOffsettingAndPreemptTimer;
    /** Whether the VM-exit was caused by a page-fault during delivery of a
     *  contributory exception or a page-fault. */
    bool                fVectoringDoublePF;
    /** Whether the VM-exit was caused by a page-fault during delivery of an
     *  external interrupt or NMI. */
    bool                fVectoringPF;
    bool                afAlignment0[3];

    /** The VMCS info. object. */
    PVMXVMCSINFO        pVmcsInfo;
} VMXTRANSIENT;
AssertCompileMemberAlignment(VMXTRANSIENT, uExitReason,               sizeof(uint64_t));
AssertCompileMemberAlignment(VMXTRANSIENT, uExitIntInfo,              sizeof(uint64_t));
AssertCompileMemberAlignment(VMXTRANSIENT, uEntryIntInfo,             sizeof(uint64_t));
AssertCompileMemberAlignment(VMXTRANSIENT, fWasGuestDebugStateActive, sizeof(uint64_t));
AssertCompileMemberAlignment(VMXTRANSIENT, pVmcsInfo,                 sizeof(uint64_t));
AssertCompileMemberSize(VMXTRANSIENT, ExitInstrInfo, sizeof(uint32_t));
/** Pointer to VMX transient state. */
typedef VMXTRANSIENT *PVMXTRANSIENT;
/** Pointer to a const VMX transient state. */
typedef const VMXTRANSIENT *PCVMXTRANSIENT;


/**
 * Memory operand read or write access.
 */
typedef enum VMXMEMACCESS
{
    VMXMEMACCESS_READ  = 0,
    VMXMEMACCESS_WRITE = 1
} VMXMEMACCESS;

/**
 * VMX VM-exit handler.
 *
 * @returns Strict VBox status code (i.e. informational status codes too).
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 */
#ifndef HMVMX_USE_FUNCTION_TABLE
typedef VBOXSTRICTRC               FNVMXEXITHANDLER(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient);
#else
typedef DECLCALLBACK(VBOXSTRICTRC) FNVMXEXITHANDLER(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient);
/** Pointer to VM-exit handler. */
typedef FNVMXEXITHANDLER          *PFNVMXEXITHANDLER;
#endif

/**
 * VMX VM-exit handler, non-strict status code.
 *
 * This is generally the same as FNVMXEXITHANDLER, the NSRC bit is just FYI.
 *
 * @returns VBox status code, no informational status code returned.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks This is not used on anything returning VERR_EM_INTERPRETER as the
 *          use of that status code will be replaced with VINF_EM_SOMETHING
 *          later when switching over to IEM.
 */
#ifndef HMVMX_USE_FUNCTION_TABLE
typedef int                        FNVMXEXITHANDLERNSRC(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient);
#else
typedef FNVMXEXITHANDLER           FNVMXEXITHANDLERNSRC;
#endif


/*********************************************************************************************************************************
*   Internal Functions                                                                                                           *
*********************************************************************************************************************************/
#ifndef HMVMX_USE_FUNCTION_TABLE
DECLINLINE(VBOXSTRICTRC)           hmR0VmxHandleExit(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient);
# define HMVMX_EXIT_DECL           DECLINLINE(VBOXSTRICTRC)
# define HMVMX_EXIT_NSRC_DECL      DECLINLINE(int)
#else
# define HMVMX_EXIT_DECL           static DECLCALLBACK(VBOXSTRICTRC)
# define HMVMX_EXIT_NSRC_DECL      HMVMX_EXIT_DECL
#endif
#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
DECLINLINE(VBOXSTRICTRC)           hmR0VmxHandleExitNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient);
#endif

static int  hmR0VmxImportGuestState(PVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo, uint64_t fWhat);
#if HC_ARCH_BITS == 32 && defined(VBOX_ENABLE_64_BITS_GUESTS)
static void hmR0VmxInitVmcsReadCache(PVMCPU pVCpu);
#endif

/** @name VM-exit handlers.
 * @{
 */
static FNVMXEXITHANDLER            hmR0VmxExitXcptOrNmi;
static FNVMXEXITHANDLER            hmR0VmxExitExtInt;
static FNVMXEXITHANDLER            hmR0VmxExitTripleFault;
static FNVMXEXITHANDLERNSRC        hmR0VmxExitIntWindow;
static FNVMXEXITHANDLERNSRC        hmR0VmxExitNmiWindow;
static FNVMXEXITHANDLER            hmR0VmxExitTaskSwitch;
static FNVMXEXITHANDLER            hmR0VmxExitCpuid;
static FNVMXEXITHANDLER            hmR0VmxExitGetsec;
static FNVMXEXITHANDLER            hmR0VmxExitHlt;
static FNVMXEXITHANDLERNSRC        hmR0VmxExitInvd;
static FNVMXEXITHANDLER            hmR0VmxExitInvlpg;
static FNVMXEXITHANDLER            hmR0VmxExitRdpmc;
static FNVMXEXITHANDLER            hmR0VmxExitVmcall;
#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
static FNVMXEXITHANDLER            hmR0VmxExitVmclear;
static FNVMXEXITHANDLER            hmR0VmxExitVmlaunch;
static FNVMXEXITHANDLER            hmR0VmxExitVmptrld;
static FNVMXEXITHANDLER            hmR0VmxExitVmptrst;
static FNVMXEXITHANDLER            hmR0VmxExitVmread;
static FNVMXEXITHANDLER            hmR0VmxExitVmresume;
static FNVMXEXITHANDLER            hmR0VmxExitVmwrite;
static FNVMXEXITHANDLER            hmR0VmxExitVmxoff;
static FNVMXEXITHANDLER            hmR0VmxExitVmxon;
static FNVMXEXITHANDLER            hmR0VmxExitInvvpid;
#endif
static FNVMXEXITHANDLER            hmR0VmxExitRdtsc;
static FNVMXEXITHANDLER            hmR0VmxExitMovCRx;
static FNVMXEXITHANDLER            hmR0VmxExitMovDRx;
static FNVMXEXITHANDLER            hmR0VmxExitIoInstr;
static FNVMXEXITHANDLER            hmR0VmxExitRdmsr;
static FNVMXEXITHANDLER            hmR0VmxExitWrmsr;
static FNVMXEXITHANDLER            hmR0VmxExitMwait;
static FNVMXEXITHANDLER            hmR0VmxExitMtf;
static FNVMXEXITHANDLER            hmR0VmxExitMonitor;
static FNVMXEXITHANDLER            hmR0VmxExitPause;
static FNVMXEXITHANDLERNSRC        hmR0VmxExitTprBelowThreshold;
static FNVMXEXITHANDLER            hmR0VmxExitApicAccess;
static FNVMXEXITHANDLER            hmR0VmxExitEptViolation;
static FNVMXEXITHANDLER            hmR0VmxExitEptMisconfig;
static FNVMXEXITHANDLER            hmR0VmxExitRdtscp;
static FNVMXEXITHANDLER            hmR0VmxExitPreemptTimer;
static FNVMXEXITHANDLERNSRC        hmR0VmxExitWbinvd;
static FNVMXEXITHANDLER            hmR0VmxExitXsetbv;
static FNVMXEXITHANDLER            hmR0VmxExitInvpcid;
static FNVMXEXITHANDLERNSRC        hmR0VmxExitSetPendingXcptUD;
static FNVMXEXITHANDLERNSRC        hmR0VmxExitErrInvalidGuestState;
static FNVMXEXITHANDLERNSRC        hmR0VmxExitErrUnexpected;
/** @} */

#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
/** @name Nested-guest VM-exit handlers.
 * @{
 */
static FNVMXEXITHANDLER            hmR0VmxExitXcptOrNmiNested;
//static FNVMXEXITHANDLER            hmR0VmxExitExtIntNested;
static FNVMXEXITHANDLER            hmR0VmxExitTripleFaultNested;
static FNVMXEXITHANDLERNSRC        hmR0VmxExitIntWindowNested;
static FNVMXEXITHANDLERNSRC        hmR0VmxExitNmiWindowNested;
static FNVMXEXITHANDLER            hmR0VmxExitTaskSwitchNested;
//static FNVMXEXITHANDLER            hmR0VmxExitCpuid;
//static FNVMXEXITHANDLER            hmR0VmxExitGetsec;
static FNVMXEXITHANDLER            hmR0VmxExitHltNested;
//static FNVMXEXITHANDLERNSRC        hmR0VmxExitInvd;
static FNVMXEXITHANDLER            hmR0VmxExitInvlpgNested;
static FNVMXEXITHANDLER            hmR0VmxExitRdpmcNested;
//static FNVMXEXITHANDLER            hmR0VmxExitVmcall;
//static FNVMXEXITHANDLER            hmR0VmxExitVmclear;
//static FNVMXEXITHANDLER            hmR0VmxExitVmlaunch;
//static FNVMXEXITHANDLER            hmR0VmxExitVmptrld;
//static FNVMXEXITHANDLER            hmR0VmxExitVmptrst;
static FNVMXEXITHANDLER            hmR0VmxExitVmreadVmwriteNested;
//static FNVMXEXITHANDLER            hmR0VmxExitVmresume;
//static FNVMXEXITHANDLER            hmR0VmxExitVmwrite;
//static FNVMXEXITHANDLER            hmR0VmxExitVmxoff;
//static FNVMXEXITHANDLER            hmR0VmxExitVmxon;
//static FNVMXEXITHANDLER            hmR0VmxExitInvvpid;
static FNVMXEXITHANDLER            hmR0VmxExitRdtscNested;
static FNVMXEXITHANDLER            hmR0VmxExitMovCRxNested;
static FNVMXEXITHANDLER            hmR0VmxExitMovDRxNested;
static FNVMXEXITHANDLER            hmR0VmxExitIoInstrNested;
static FNVMXEXITHANDLER            hmR0VmxExitRdmsrNested;
static FNVMXEXITHANDLER            hmR0VmxExitWrmsrNested;
static FNVMXEXITHANDLER            hmR0VmxExitMwaitNested;
static FNVMXEXITHANDLER            hmR0VmxExitMtfNested;
static FNVMXEXITHANDLER            hmR0VmxExitMonitorNested;
static FNVMXEXITHANDLER            hmR0VmxExitPauseNested;
static FNVMXEXITHANDLERNSRC        hmR0VmxExitTprBelowThresholdNested;
static FNVMXEXITHANDLER            hmR0VmxExitApicAccessNested;
static FNVMXEXITHANDLER            hmR0VmxExitApicWriteNested;
static FNVMXEXITHANDLER            hmR0VmxExitVirtEoiNested;
//static FNVMXEXITHANDLER            hmR0VmxExitEptViolation;
//static FNVMXEXITHANDLER            hmR0VmxExitEptMisconfig;
static FNVMXEXITHANDLER            hmR0VmxExitRdtscpNested;
//static FNVMXEXITHANDLER            hmR0VmxExitPreemptTimer;
static FNVMXEXITHANDLERNSRC        hmR0VmxExitWbinvdNested;
//static FNVMXEXITHANDLER            hmR0VmxExitXsetbv;
//static FNVMXEXITHANDLER            hmR0VmxExitErrUnexpected;
static FNVMXEXITHANDLER            hmR0VmxExitInvpcidNested;
//static FNVMXEXITHANDLERNSRC        hmR0VmxExitSetPendingXcptUD;
static FNVMXEXITHANDLERNSRC        hmR0VmxExitErrInvalidGuestStateNested;
//static FNVMXEXITHANDLERNSRC        hmR0VmxExitErrUnexpected;
static FNVMXEXITHANDLER            hmR0VmxExitInstrNested;
static FNVMXEXITHANDLER            hmR0VmxExitInstrWithInfoNested;
/** @} */
#endif /* VBOX_WITH_NESTED_HWVIRT_VMX */

/** @name Helpers for hardware exceptions VM-exit handlers.
 * @{
 */
static VBOXSTRICTRC hmR0VmxExitXcptPF(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient);
static VBOXSTRICTRC hmR0VmxExitXcptMF(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient);
static VBOXSTRICTRC hmR0VmxExitXcptDB(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient);
static VBOXSTRICTRC hmR0VmxExitXcptBP(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient);
static VBOXSTRICTRC hmR0VmxExitXcptGP(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient);
static VBOXSTRICTRC hmR0VmxExitXcptAC(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient);
static VBOXSTRICTRC hmR0VmxExitXcptGeneric(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient);
static VBOXSTRICTRC hmR0VmxExitLmsw(PVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo, uint8_t cbInstr, uint16_t uMsw, RTGCPTR GCPtrEffDst);
static VBOXSTRICTRC hmR0VmxExitClts(PVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo, uint8_t cbInstr);
static VBOXSTRICTRC hmR0VmxExitMovFromCrX(PVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo, uint8_t cbInstr, uint8_t iGReg, uint8_t iCrReg);
static VBOXSTRICTRC hmR0VmxExitMovToCrX(PVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo, uint8_t cbInstr, uint8_t iGReg, uint8_t iCrReg);
static VBOXSTRICTRC hmR0VmxExitHostNmi(PVMCPU pVCpu);
/** @} */


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/
#ifdef VMX_USE_CACHED_VMCS_ACCESSES
static const uint32_t g_aVmcsCacheSegBase[] =
{
    VMX_VMCS_GUEST_ES_BASE_CACHE_IDX,
    VMX_VMCS_GUEST_CS_BASE_CACHE_IDX,
    VMX_VMCS_GUEST_SS_BASE_CACHE_IDX,
    VMX_VMCS_GUEST_DS_BASE_CACHE_IDX,
    VMX_VMCS_GUEST_FS_BASE_CACHE_IDX,
    VMX_VMCS_GUEST_GS_BASE_CACHE_IDX
};
AssertCompile(RT_ELEMENTS(g_aVmcsCacheSegBase)  == X86_SREG_COUNT);
#endif
static const uint32_t g_aVmcsSegBase[] =
{
    VMX_VMCS_GUEST_ES_BASE,
    VMX_VMCS_GUEST_CS_BASE,
    VMX_VMCS_GUEST_SS_BASE,
    VMX_VMCS_GUEST_DS_BASE,
    VMX_VMCS_GUEST_FS_BASE,
    VMX_VMCS_GUEST_GS_BASE
};
static const uint32_t g_aVmcsSegSel[] =
{
    VMX_VMCS16_GUEST_ES_SEL,
    VMX_VMCS16_GUEST_CS_SEL,
    VMX_VMCS16_GUEST_SS_SEL,
    VMX_VMCS16_GUEST_DS_SEL,
    VMX_VMCS16_GUEST_FS_SEL,
    VMX_VMCS16_GUEST_GS_SEL
};
static const uint32_t g_aVmcsSegLimit[] =
{
    VMX_VMCS32_GUEST_ES_LIMIT,
    VMX_VMCS32_GUEST_CS_LIMIT,
    VMX_VMCS32_GUEST_SS_LIMIT,
    VMX_VMCS32_GUEST_DS_LIMIT,
    VMX_VMCS32_GUEST_FS_LIMIT,
    VMX_VMCS32_GUEST_GS_LIMIT
};
static const uint32_t g_aVmcsSegAttr[] =
{
    VMX_VMCS32_GUEST_ES_ACCESS_RIGHTS,
    VMX_VMCS32_GUEST_CS_ACCESS_RIGHTS,
    VMX_VMCS32_GUEST_SS_ACCESS_RIGHTS,
    VMX_VMCS32_GUEST_DS_ACCESS_RIGHTS,
    VMX_VMCS32_GUEST_FS_ACCESS_RIGHTS,
    VMX_VMCS32_GUEST_GS_ACCESS_RIGHTS
};
AssertCompile(RT_ELEMENTS(g_aVmcsSegSel)   == X86_SREG_COUNT);
AssertCompile(RT_ELEMENTS(g_aVmcsSegLimit) == X86_SREG_COUNT);
AssertCompile(RT_ELEMENTS(g_aVmcsSegBase)  == X86_SREG_COUNT);
AssertCompile(RT_ELEMENTS(g_aVmcsSegAttr)  == X86_SREG_COUNT);

#ifdef HMVMX_USE_FUNCTION_TABLE
/**
 * VMX_EXIT dispatch table.
 */
static const PFNVMXEXITHANDLER g_apfnVMExitHandlers[VMX_EXIT_MAX + 1] =
{
    /*  0  VMX_EXIT_XCPT_OR_NMI             */  hmR0VmxExitXcptOrNmi,
    /*  1  VMX_EXIT_EXT_INT                 */  hmR0VmxExitExtInt,
    /*  2  VMX_EXIT_TRIPLE_FAULT            */  hmR0VmxExitTripleFault,
    /*  3  VMX_EXIT_INIT_SIGNAL             */  hmR0VmxExitErrUnexpected,
    /*  4  VMX_EXIT_SIPI                    */  hmR0VmxExitErrUnexpected,
    /*  5  VMX_EXIT_IO_SMI                  */  hmR0VmxExitErrUnexpected,
    /*  6  VMX_EXIT_SMI                     */  hmR0VmxExitErrUnexpected,
    /*  7  VMX_EXIT_INT_WINDOW              */  hmR0VmxExitIntWindow,
    /*  8  VMX_EXIT_NMI_WINDOW              */  hmR0VmxExitNmiWindow,
    /*  9  VMX_EXIT_TASK_SWITCH             */  hmR0VmxExitTaskSwitch,
    /* 10  VMX_EXIT_CPUID                   */  hmR0VmxExitCpuid,
    /* 11  VMX_EXIT_GETSEC                  */  hmR0VmxExitGetsec,
    /* 12  VMX_EXIT_HLT                     */  hmR0VmxExitHlt,
    /* 13  VMX_EXIT_INVD                    */  hmR0VmxExitInvd,
    /* 14  VMX_EXIT_INVLPG                  */  hmR0VmxExitInvlpg,
    /* 15  VMX_EXIT_RDPMC                   */  hmR0VmxExitRdpmc,
    /* 16  VMX_EXIT_RDTSC                   */  hmR0VmxExitRdtsc,
    /* 17  VMX_EXIT_RSM                     */  hmR0VmxExitErrUnexpected,
    /* 18  VMX_EXIT_VMCALL                  */  hmR0VmxExitVmcall,
#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
    /* 19  VMX_EXIT_VMCLEAR                 */  hmR0VmxExitVmclear,
    /* 20  VMX_EXIT_VMLAUNCH                */  hmR0VmxExitVmlaunch,
    /* 21  VMX_EXIT_VMPTRLD                 */  hmR0VmxExitVmptrld,
    /* 22  VMX_EXIT_VMPTRST                 */  hmR0VmxExitVmptrst,
    /* 23  VMX_EXIT_VMREAD                  */  hmR0VmxExitVmread,
    /* 24  VMX_EXIT_VMRESUME                */  hmR0VmxExitVmresume,
    /* 25  VMX_EXIT_VMWRITE                 */  hmR0VmxExitVmwrite,
    /* 26  VMX_EXIT_VMXOFF                  */  hmR0VmxExitVmxoff,
    /* 27  VMX_EXIT_VMXON                   */  hmR0VmxExitVmxon,
#else
    /* 19  VMX_EXIT_VMCLEAR                 */  hmR0VmxExitSetPendingXcptUD,
    /* 20  VMX_EXIT_VMLAUNCH                */  hmR0VmxExitSetPendingXcptUD,
    /* 21  VMX_EXIT_VMPTRLD                 */  hmR0VmxExitSetPendingXcptUD,
    /* 22  VMX_EXIT_VMPTRST                 */  hmR0VmxExitSetPendingXcptUD,
    /* 23  VMX_EXIT_VMREAD                  */  hmR0VmxExitSetPendingXcptUD,
    /* 24  VMX_EXIT_VMRESUME                */  hmR0VmxExitSetPendingXcptUD,
    /* 25  VMX_EXIT_VMWRITE                 */  hmR0VmxExitSetPendingXcptUD,
    /* 26  VMX_EXIT_VMXOFF                  */  hmR0VmxExitSetPendingXcptUD,
    /* 27  VMX_EXIT_VMXON                   */  hmR0VmxExitSetPendingXcptUD,
#endif
    /* 28  VMX_EXIT_MOV_CRX                 */  hmR0VmxExitMovCRx,
    /* 29  VMX_EXIT_MOV_DRX                 */  hmR0VmxExitMovDRx,
    /* 30  VMX_EXIT_IO_INSTR                */  hmR0VmxExitIoInstr,
    /* 31  VMX_EXIT_RDMSR                   */  hmR0VmxExitRdmsr,
    /* 32  VMX_EXIT_WRMSR                   */  hmR0VmxExitWrmsr,
    /* 33  VMX_EXIT_ERR_INVALID_GUEST_STATE */  hmR0VmxExitErrInvalidGuestState,
    /* 34  VMX_EXIT_ERR_MSR_LOAD            */  hmR0VmxExitErrUnexpected,
    /* 35  UNDEFINED                        */  hmR0VmxExitErrUnexpected,
    /* 36  VMX_EXIT_MWAIT                   */  hmR0VmxExitMwait,
    /* 37  VMX_EXIT_MTF                     */  hmR0VmxExitMtf,
    /* 38  UNDEFINED                        */  hmR0VmxExitErrUnexpected,
    /* 39  VMX_EXIT_MONITOR                 */  hmR0VmxExitMonitor,
    /* 40  VMX_EXIT_PAUSE                   */  hmR0VmxExitPause,
    /* 41  VMX_EXIT_ERR_MACHINE_CHECK       */  hmR0VmxExitErrUnexpected,
    /* 42  UNDEFINED                        */  hmR0VmxExitErrUnexpected,
    /* 43  VMX_EXIT_TPR_BELOW_THRESHOLD     */  hmR0VmxExitTprBelowThreshold,
    /* 44  VMX_EXIT_APIC_ACCESS             */  hmR0VmxExitApicAccess,
    /* 45  VMX_EXIT_VIRTUALIZED_EOI         */  hmR0VmxExitErrUnexpected,
    /* 46  VMX_EXIT_GDTR_IDTR_ACCESS        */  hmR0VmxExitErrUnexpected,
    /* 47  VMX_EXIT_LDTR_TR_ACCESS          */  hmR0VmxExitErrUnexpected,
    /* 48  VMX_EXIT_EPT_VIOLATION           */  hmR0VmxExitEptViolation,
    /* 49  VMX_EXIT_EPT_MISCONFIG           */  hmR0VmxExitEptMisconfig,
    /* 50  VMX_EXIT_INVEPT                  */  hmR0VmxExitSetPendingXcptUD,
    /* 51  VMX_EXIT_RDTSCP                  */  hmR0VmxExitRdtscp,
    /* 52  VMX_EXIT_PREEMPT_TIMER           */  hmR0VmxExitPreemptTimer,
#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
    /* 53  VMX_EXIT_INVVPID                 */  hmR0VmxExitInvvpid,
#else
    /* 53  VMX_EXIT_INVVPID                 */  hmR0VmxExitSetPendingXcptUD,
#endif
    /* 54  VMX_EXIT_WBINVD                  */  hmR0VmxExitWbinvd,
    /* 55  VMX_EXIT_XSETBV                  */  hmR0VmxExitXsetbv,
    /* 56  VMX_EXIT_APIC_WRITE              */  hmR0VmxExitErrUnexpected,
    /* 57  VMX_EXIT_RDRAND                  */  hmR0VmxExitErrUnexpected,
    /* 58  VMX_EXIT_INVPCID                 */  hmR0VmxExitInvpcid,
    /* 59  VMX_EXIT_VMFUNC                  */  hmR0VmxExitErrUnexpected,
    /* 60  VMX_EXIT_ENCLS                   */  hmR0VmxExitErrUnexpected,
    /* 61  VMX_EXIT_RDSEED                  */  hmR0VmxExitErrUnexpected,
    /* 62  VMX_EXIT_PML_FULL                */  hmR0VmxExitErrUnexpected,
    /* 63  VMX_EXIT_XSAVES                  */  hmR0VmxExitErrUnexpected,
    /* 64  VMX_EXIT_XRSTORS                 */  hmR0VmxExitErrUnexpected,
    /* 65  UNDEFINED                        */  hmR0VmxExitErrUnexpected,
    /* 66  VMX_EXIT_SPP_EVENT               */  hmR0VmxExitErrUnexpected,
    /* 67  VMX_EXIT_UMWAIT                  */  hmR0VmxExitErrUnexpected,
    /* 68  VMX_EXIT_TPAUSE                  */  hmR0VmxExitErrUnexpected,
};
#endif /* HMVMX_USE_FUNCTION_TABLE */

#if defined(VBOX_STRICT) && defined(LOG_ENABLED)
static const char * const g_apszVmxInstrErrors[HMVMX_INSTR_ERROR_MAX + 1] =
{
    /*  0 */ "(Not Used)",
    /*  1 */ "VMCALL executed in VMX root operation.",
    /*  2 */ "VMCLEAR with invalid physical address.",
    /*  3 */ "VMCLEAR with VMXON pointer.",
    /*  4 */ "VMLAUNCH with non-clear VMCS.",
    /*  5 */ "VMRESUME with non-launched VMCS.",
    /*  6 */ "VMRESUME after VMXOFF",
    /*  7 */ "VM-entry with invalid control fields.",
    /*  8 */ "VM-entry with invalid host state fields.",
    /*  9 */ "VMPTRLD with invalid physical address.",
    /* 10 */ "VMPTRLD with VMXON pointer.",
    /* 11 */ "VMPTRLD with incorrect revision identifier.",
    /* 12 */ "VMREAD/VMWRITE from/to unsupported VMCS component.",
    /* 13 */ "VMWRITE to read-only VMCS component.",
    /* 14 */ "(Not Used)",
    /* 15 */ "VMXON executed in VMX root operation.",
    /* 16 */ "VM-entry with invalid executive-VMCS pointer.",
    /* 17 */ "VM-entry with non-launched executing VMCS.",
    /* 18 */ "VM-entry with executive-VMCS pointer not VMXON pointer.",
    /* 19 */ "VMCALL with non-clear VMCS.",
    /* 20 */ "VMCALL with invalid VM-exit control fields.",
    /* 21 */ "(Not Used)",
    /* 22 */ "VMCALL with incorrect MSEG revision identifier.",
    /* 23 */ "VMXOFF under dual monitor treatment of SMIs and SMM.",
    /* 24 */ "VMCALL with invalid SMM-monitor features.",
    /* 25 */ "VM-entry with invalid VM-execution control fields in executive VMCS.",
    /* 26 */ "VM-entry with events blocked by MOV SS.",
    /* 27 */ "(Not Used)",
    /* 28 */ "Invalid operand to INVEPT/INVVPID."
};
#endif /* VBOX_STRICT */


/**
 * Get the CR0 guest/host mask that does not change through the lifetime of a VM.
 *
 * Any bit set in this mask is owned by the host/hypervisor and would cause a
 * VM-exit when modified by the guest.
 *
 * @returns The static CR0 guest/host mask.
 * @param   pVCpu   The cross context virtual CPU structure.
 */
DECL_FORCE_INLINE(uint64_t) hmR0VmxGetFixedCr0Mask(PCVMCPU pVCpu)
{
    /*
     * Modifications to CR0 bits that VT-x ignores saving/restoring (CD, ET, NW) and
     * to CR0 bits that we require for shadow paging (PG) by the guest must cause VM-exits.
     */
    /** @todo Avoid intercepting CR0.PE with unrestricted guest execution. Fix PGM
     *        enmGuestMode to be in-sync with the current mode. See @bugref{6398}
     *        and @bugref{6944}. */
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    return (  X86_CR0_PE
            | X86_CR0_NE
            | (pVM->hm.s.fNestedPaging ? 0 : X86_CR0_WP)
            | X86_CR0_PG
            | X86_CR0_ET     /* Bit ignored on VM-entry and VM-exit. Don't let the guest modify the host CR0.ET */
            | X86_CR0_CD     /* Bit ignored on VM-entry and VM-exit. Don't let the guest modify the host CR0.CD */
            | X86_CR0_NW);   /* Bit ignored on VM-entry and VM-exit. Don't let the guest modify the host CR0.NW */
}


/**
 * Gets the CR4 guest/host mask that does not change through the lifetime of a VM.
 *
 * Any bit set in this mask is owned by the host/hypervisor and would cause a
 * VM-exit when modified by the guest.
 *
 * @returns The static CR4 guest/host mask.
 * @param   pVCpu   The cross context virtual CPU structure.
 */
DECL_FORCE_INLINE(uint64_t) hmR0VmxGetFixedCr4Mask(PCVMCPU pVCpu)
{
    /*
     * We need to look at the host features here (for e.g. OSXSAVE, PCID) because
     * these bits are reserved on hardware that does not support them. Since the
     * CPU cannot refer to our virtual CPUID, we need to intercept CR4 changes to
     * these  bits and handle it depending on whether we expose them to the guest.
     */
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    bool const fXSaveRstor = pVM->cpum.ro.HostFeatures.fXSaveRstor;
    bool const fPcid       = pVM->cpum.ro.HostFeatures.fPcid;
    return (  X86_CR4_VMXE
            | X86_CR4_VME
            | X86_CR4_PAE
            | X86_CR4_PGE
            | X86_CR4_PSE
            | (fXSaveRstor ? X86_CR4_OSXSAVE : 0)
            | (fPcid       ? X86_CR4_PCIDE   : 0));
}


/**
 * Returns whether the the VM-exit MSR-store area differs from the VM-exit MSR-load
 * area.
 *
 * @returns @c true if it's different, @c false otherwise.
 * @param   pVmcsInfo       The VMCS info. object.
 */
DECL_FORCE_INLINE(bool) hmR0VmxIsSeparateExitMsrStoreAreaVmcs(PCVMXVMCSINFO pVmcsInfo)
{
    return RT_BOOL(   pVmcsInfo->pvGuestMsrStore != pVmcsInfo->pvGuestMsrLoad
                   && pVmcsInfo->pvGuestMsrStore);
}


/**
 * Checks whether one of the given Pin-based VM-execution controls are set.
 *
 * @returns @c true if set, @c false otherwise.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 * @param   uPinCtls        The Pin-based VM-execution controls to check.
 *
 * @remarks This will not check merged controls when executing a nested-guest
 *          but the original control specified by the guest hypervisor.
 */
static bool hmR0VmxIsPinCtlsSet(PVMCPU pVCpu, PCVMXTRANSIENT pVmxTransient, uint32_t uPinCtls)
{
    if (!pVmxTransient->fIsNestedGuest)
    {
        PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
        return RT_BOOL(pVmcsInfo->u32PinCtls & uPinCtls);
    }
    return CPUMIsGuestVmxPinCtlsSet(pVCpu, &pVCpu->cpum.GstCtx, uPinCtls);
}


/**
 * Checks whether one of the given Processor-based VM-execution controls are set.
 *
 * @returns @c true if set, @c false otherwise.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 * @param   uProcCtls       The Processor-based VM-execution controls to check.
 *
 * @remarks This will not check merged controls when executing a nested-guest
 *          but the original control specified by the guest hypervisor.
 */
static bool hmR0VmxIsProcCtlsSet(PVMCPU pVCpu, PCVMXTRANSIENT pVmxTransient, uint32_t uProcCtls)
{
    if (!pVmxTransient->fIsNestedGuest)
    {
        PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
        return RT_BOOL(pVmcsInfo->u32ProcCtls & uProcCtls);
    }
    return CPUMIsGuestVmxProcCtlsSet(pVCpu, &pVCpu->cpum.GstCtx, uProcCtls);
}


/**
 * Checks whether one of the given Secondary Processor-based VM-execution controls
 * are set.
 *
 * @returns @c true if set, @c false otherwise.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 * @param   uProcCtls2      The Secondary Processor-based VM-execution controls to
 *                          check.
 *
 * @remarks This will not check merged controls when executing a nested-guest
 *          but the original control specified by the guest hypervisor.
 */
static bool hmR0VmxIsProcCtls2Set(PVMCPU pVCpu, PCVMXTRANSIENT pVmxTransient, uint32_t uProcCtls2)
{
    if (!pVmxTransient->fIsNestedGuest)
    {
        PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
        return RT_BOOL(pVmcsInfo->u32ProcCtls2 & uProcCtls2);
    }
    return CPUMIsGuestVmxProcCtls2Set(pVCpu, &pVCpu->cpum.GstCtx, uProcCtls2);
}


#if 0
/**
 * Checks whether one of the given VM-entry controls are set.
 *
 * @returns @c true if set, @c false otherwise.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 * @param   uEntryCtls      The VM-entry controls to check.
 *
 * @remarks This will not check merged controls when executing a nested-guest
 *          but the original control specified by the guest hypervisor.
 */
static bool hmR0VmxIsEntryCtlsSet(PVMCPU pVCpu, PCVMXTRANSIENT pVmxTransient, uint32_t uEntryCtls)
{
    if (!pVmxTransient->fIsNestedGuest)
    {
        PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
        return RT_BOOL(pVmcsInfo->u32EntryCtls & uEntryCtls);
    }
    return CPUMIsGuestVmxEntryCtlsSet(pVCpu, &pVCpu->cpum.GstCtx, uEntryCtls);
}


/**
 * Checks whether one of the given VM-exit controls are set.
 *
 * @returns @c true if set, @c false otherwise.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 * @param   uExitCtls       The VM-exit controls to check.
 *
 * @remarks This will not check merged controls when executing a nested-guest
 *          but the original control specified by the guest hypervisor.
 */
static bool hmR0VmxIsExitCtlsSet(PVMCPU pVCpu, PCVMXTRANSIENT pVmxTransient, uint32_t uExitCtls)
{
    if (!pVmxTransient->fIsNestedGuest)
    {
        PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
        return RT_BOOL(pVmcsInfo->u32ExitCtls & uExitCtls);
    }
    return CPUMIsGuestVmxExitCtlsSet(pVCpu, &pVCpu->cpum.GstCtx, uExitCtls);
}
#endif


/**
 * Adds one or more exceptions to the exception bitmap and commits it to the current
 * VMCS.
 *
 * @returns VBox status code.
 * @param   pVmxTransient   The VMX-transient structure.
 * @param   uXcptMask       The exception(s) to add.
 */
static int hmR0VmxAddXcptInterceptMask(PVMXTRANSIENT pVmxTransient, uint32_t uXcptMask)
{
    PVMXVMCSINFO pVmcsInfo   = pVmxTransient->pVmcsInfo;
    uint32_t     uXcptBitmap = pVmcsInfo->u32XcptBitmap;
    if ((uXcptBitmap & uXcptMask) != uXcptMask)
    {
        uXcptBitmap |= uXcptMask;
        int rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_EXCEPTION_BITMAP, uXcptBitmap);
        AssertRCReturn(rc, rc);
        pVmcsInfo->u32XcptBitmap = uXcptBitmap;
    }
    return VINF_SUCCESS;
}


/**
 * Adds an exception to the exception bitmap and commits it to the current VMCS.
 *
 * @returns VBox status code.
 * @param   pVmxTransient   The VMX-transient structure.
 * @param   uXcpt           The exception to add.
 */
static int hmR0VmxAddXcptIntercept(PVMXTRANSIENT pVmxTransient, uint8_t uXcpt)
{
    Assert(uXcpt <= X86_XCPT_LAST);
    return hmR0VmxAddXcptInterceptMask(pVmxTransient, RT_BIT_32(uXcpt));
}


/**
 * Remove one or more exceptions from the exception bitmap and commits it to the
 * current VMCS.
 *
 * This takes care of not removing the exception intercept if a nested-guest
 * requires the exception to be intercepted.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 * @param   uXcptMask       The exception(s) to remove.
 */
static int hmR0VmxRemoveXcptInterceptMask(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient, uint32_t uXcptMask)
{
    PVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    uint32_t u32XcptBitmap = pVmcsInfo->u32XcptBitmap;
    if (u32XcptBitmap & uXcptMask)
    {
#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
        if (!pVmxTransient->fIsNestedGuest)
        { /* likely */ }
        else
        {
            PCVMXVVMCS pVmcsNstGst = pVCpu->cpum.GstCtx.hwvirt.vmx.CTX_SUFF(pVmcs);
            uXcptMask &= ~pVmcsNstGst->u32XcptBitmap;
        }
#endif
#ifdef HMVMX_ALWAYS_TRAP_ALL_XCPTS
        uXcptMask &= ~(  RT_BIT(X86_XCPT_BP)
                       | RT_BIT(X86_XCPT_DE)
                       | RT_BIT(X86_XCPT_NM)
                       | RT_BIT(X86_XCPT_TS)
                       | RT_BIT(X86_XCPT_UD)
                       | RT_BIT(X86_XCPT_NP)
                       | RT_BIT(X86_XCPT_SS)
                       | RT_BIT(X86_XCPT_GP)
                       | RT_BIT(X86_XCPT_PF)
                       | RT_BIT(X86_XCPT_MF));
#elif defined(HMVMX_ALWAYS_TRAP_PF)
        uXcptMask &= ~RT_BIT(X86_XCPT_PF);
#endif
        if (uXcptMask)
        {
            /* Validate we are not removing any essential exception intercepts. */
            Assert(pVCpu->CTX_SUFF(pVM)->hm.s.fNestedPaging || !(uXcptMask & RT_BIT(X86_XCPT_PF))); RT_NOREF(pVCpu);
            Assert(!(uXcptMask & RT_BIT(X86_XCPT_DB)));
            Assert(!(uXcptMask & RT_BIT(X86_XCPT_AC)));

            /* Remove it from the exception bitmap. */
            u32XcptBitmap &= ~uXcptMask;

            /* Commit and update the cache if necessary. */
            if (pVmcsInfo->u32XcptBitmap != u32XcptBitmap)
            {
                int rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_EXCEPTION_BITMAP, u32XcptBitmap);
                AssertRCReturn(rc, rc);
                pVmcsInfo->u32XcptBitmap = u32XcptBitmap;
            }
        }
    }
    return VINF_SUCCESS;
}


/**
 * Remove an exceptions from the exception bitmap and commits it to the current
 * VMCS.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 * @param   uXcpt           The exception to remove.
 */
static int hmR0VmxRemoveXcptIntercept(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient, uint8_t uXcpt)
{
    return hmR0VmxRemoveXcptInterceptMask(pVCpu, pVmxTransient, RT_BIT(uXcpt));
}


/**
 * Loads the VMCS specified by the VMCS info. object.
 *
 * @returns VBox status code.
 * @param   pVmcsInfo       The VMCS info. object.
 */
static int hmR0VmxLoadVmcs(PVMXVMCSINFO pVmcsInfo)
{
    Assert(pVmcsInfo->HCPhysVmcs);
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    if (pVmcsInfo->fVmcsState & VMX_V_VMCS_LAUNCH_STATE_CLEAR)
    {
        int rc = VMXLoadVmcs(pVmcsInfo->HCPhysVmcs);
        if (RT_SUCCESS(rc))
        {
            pVmcsInfo->fVmcsState |= VMX_V_VMCS_LAUNCH_STATE_CURRENT;
            return VINF_SUCCESS;
        }
        return rc;
    }
    return VERR_VMX_INVALID_VMCS_LAUNCH_STATE;
}


/**
 * Clears the VMCS specified by the VMCS info. object.
 *
 * @returns VBox status code.
 * @param   pVmcsInfo       The VMCS info. object.
 */
static int hmR0VmxClearVmcs(PVMXVMCSINFO pVmcsInfo)
{
    Assert(pVmcsInfo->HCPhysVmcs);
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    int rc = VMXClearVmcs(pVmcsInfo->HCPhysVmcs);
    if (RT_SUCCESS(rc))
        pVmcsInfo->fVmcsState = VMX_V_VMCS_LAUNCH_STATE_CLEAR;
    return rc;
}


#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
/**
 * Switches the current VMCS to the one specified.
 *
 * @returns VBox status code.
 * @param   pVmcsInfoFrom   The VMCS info. object we are switching from.
 * @param   pVmcsInfoTo     The VMCS info. object we are switching to.
 *
 * @remarks Called with interrupts disabled.
 */
static int hmR0VmxSwitchVmcs(PVMXVMCSINFO pVmcsInfoFrom, PVMXVMCSINFO pVmcsInfoTo)
{
    /*
     * Clear the VMCS we are switching out if it has not already been cleared.
     * This will sync any CPU internal data back to the VMCS.
     */
    if (pVmcsInfoFrom->fVmcsState != VMX_V_VMCS_LAUNCH_STATE_CLEAR)
    {
        int rc = hmR0VmxClearVmcs(pVmcsInfoFrom);
        if (RT_SUCCESS(rc))
        { /* likely */ }
        else
            return rc;
    }

    /*
     * Clear the VMCS we are switching to if it has not already been cleared.
     * This will initialize the VMCS launch state to "clear" required for loading it.
     *
     * See Intel spec. 31.6 "Preparation And Launching A Virtual Machine".
     */
    if (pVmcsInfoTo->fVmcsState != VMX_V_VMCS_LAUNCH_STATE_CLEAR)
    {
        int rc = hmR0VmxClearVmcs(pVmcsInfoTo);
        if (RT_SUCCESS(rc))
        { /* likely */ }
        else
            return rc;
    }

    /*
     * Finally, load the VMCS we are switching to.
     */
    return hmR0VmxLoadVmcs(pVmcsInfoTo);
}
#endif /* VBOX_WITH_NESTED_HWVIRT_VMX */


/**
 * Updates the VM's last error record.
 *
 * If there was a VMX instruction error, reads the error data from the VMCS and
 * updates VCPU's last error record as well.
 *
 * @param   pVCpu   The cross context virtual CPU structure of the calling EMT.
 *                  Can be NULL if @a rc is not VERR_VMX_UNABLE_TO_START_VM or
 *                  VERR_VMX_INVALID_VMCS_FIELD.
 * @param   rc      The error code.
 */
static void hmR0VmxUpdateErrorRecord(PVMCPU pVCpu, int rc)
{
    if (   rc == VERR_VMX_INVALID_VMCS_FIELD
        || rc == VERR_VMX_UNABLE_TO_START_VM)
    {
        AssertPtrReturnVoid(pVCpu);
        VMXReadVmcs32(VMX_VMCS32_RO_VM_INSTR_ERROR, &pVCpu->hm.s.vmx.LastError.u32InstrError);
    }
    pVCpu->CTX_SUFF(pVM)->hm.s.rcInit = rc;
}


#ifdef VBOX_STRICT
/**
 * Reads the VM-entry interruption-information field from the VMCS into the VMX
 * transient structure.
 *
 * @returns VBox status code.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxReadEntryIntInfoVmcs(PVMXTRANSIENT pVmxTransient)
{
    int rc = VMXReadVmcs32(VMX_VMCS32_CTRL_ENTRY_INTERRUPTION_INFO, &pVmxTransient->uEntryIntInfo);
    AssertRCReturn(rc, rc);
    return VINF_SUCCESS;
}


/**
 * Reads the VM-entry exception error code field from the VMCS into
 * the VMX transient structure.
 *
 * @returns VBox status code.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxReadEntryXcptErrorCodeVmcs(PVMXTRANSIENT pVmxTransient)
{
    int rc = VMXReadVmcs32(VMX_VMCS32_CTRL_ENTRY_EXCEPTION_ERRCODE, &pVmxTransient->uEntryXcptErrorCode);
    AssertRCReturn(rc, rc);
    return VINF_SUCCESS;
}


/**
 * Reads the VM-entry exception error code field from the VMCS into
 * the VMX transient structure.
 *
 * @returns VBox status code.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxReadEntryInstrLenVmcs(PVMXTRANSIENT pVmxTransient)
{
    int rc = VMXReadVmcs32(VMX_VMCS32_CTRL_ENTRY_INSTR_LENGTH, &pVmxTransient->cbEntryInstr);
    AssertRCReturn(rc, rc);
    return VINF_SUCCESS;
}
#endif /* VBOX_STRICT */


/**
 * Reads the VM-exit interruption-information field from the VMCS into the VMX
 * transient structure.
 *
 * @returns VBox status code.
 * @param   pVmxTransient   The VMX-transient structure.
 */
DECLINLINE(int) hmR0VmxReadExitIntInfoVmcs(PVMXTRANSIENT pVmxTransient)
{
    if (!(pVmxTransient->fVmcsFieldsRead & HMVMX_READ_EXIT_INTERRUPTION_INFO))
    {
        int rc = VMXReadVmcs32(VMX_VMCS32_RO_EXIT_INTERRUPTION_INFO, &pVmxTransient->uExitIntInfo);
        AssertRCReturn(rc,rc);
        pVmxTransient->fVmcsFieldsRead |= HMVMX_READ_EXIT_INTERRUPTION_INFO;
    }
    return VINF_SUCCESS;
}


/**
 * Reads the VM-exit interruption error code from the VMCS into the VMX
 * transient structure.
 *
 * @returns VBox status code.
 * @param   pVmxTransient   The VMX-transient structure.
 */
DECLINLINE(int) hmR0VmxReadExitIntErrorCodeVmcs(PVMXTRANSIENT pVmxTransient)
{
    if (!(pVmxTransient->fVmcsFieldsRead & HMVMX_READ_EXIT_INTERRUPTION_ERROR_CODE))
    {
        int rc = VMXReadVmcs32(VMX_VMCS32_RO_EXIT_INTERRUPTION_ERROR_CODE, &pVmxTransient->uExitIntErrorCode);
        AssertRCReturn(rc, rc);
        pVmxTransient->fVmcsFieldsRead |= HMVMX_READ_EXIT_INTERRUPTION_ERROR_CODE;
    }
    return VINF_SUCCESS;
}


/**
 * Reads the VM-exit instruction length field from the VMCS into the VMX
 * transient structure.
 *
 * @returns VBox status code.
 * @param   pVmxTransient   The VMX-transient structure.
 */
DECLINLINE(int) hmR0VmxReadExitInstrLenVmcs(PVMXTRANSIENT pVmxTransient)
{
    if (!(pVmxTransient->fVmcsFieldsRead & HMVMX_READ_EXIT_INSTR_LEN))
    {
        int rc = VMXReadVmcs32(VMX_VMCS32_RO_EXIT_INSTR_LENGTH, &pVmxTransient->cbInstr);
        AssertRCReturn(rc, rc);
        pVmxTransient->fVmcsFieldsRead |= HMVMX_READ_EXIT_INSTR_LEN;
    }
    return VINF_SUCCESS;
}


/**
 * Reads the VM-exit instruction-information field from the VMCS into
 * the VMX transient structure.
 *
 * @returns VBox status code.
 * @param   pVmxTransient   The VMX-transient structure.
 */
DECLINLINE(int) hmR0VmxReadExitInstrInfoVmcs(PVMXTRANSIENT pVmxTransient)
{
    if (!(pVmxTransient->fVmcsFieldsRead & HMVMX_READ_EXIT_INSTR_INFO))
    {
        int rc = VMXReadVmcs32(VMX_VMCS32_RO_EXIT_INSTR_INFO, &pVmxTransient->ExitInstrInfo.u);
        AssertRCReturn(rc, rc);
        pVmxTransient->fVmcsFieldsRead |= HMVMX_READ_EXIT_INSTR_INFO;
    }
    return VINF_SUCCESS;
}


/**
 * Reads the Exit Qualification from the VMCS into the VMX transient structure.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure of the
 *                          calling EMT. (Required for the VMCS cache case.)
 * @param   pVmxTransient   The VMX-transient structure.
 */
DECLINLINE(int) hmR0VmxReadExitQualVmcs(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    if (!(pVmxTransient->fVmcsFieldsRead & HMVMX_READ_EXIT_QUALIFICATION))
    {
        int rc = VMXReadVmcsGstN(VMX_VMCS_RO_EXIT_QUALIFICATION, &pVmxTransient->uExitQual); NOREF(pVCpu);
        AssertRCReturn(rc, rc);
        pVmxTransient->fVmcsFieldsRead |= HMVMX_READ_EXIT_QUALIFICATION;
    }
    return VINF_SUCCESS;
}


/**
 * Reads the Guest-linear address from the VMCS into the VMX transient structure.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure of the
 *                          calling EMT. (Required for the VMCS cache case.)
 * @param   pVmxTransient   The VMX-transient structure.
 */
DECLINLINE(int) hmR0VmxReadGuestLinearAddrVmcs(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    if (!(pVmxTransient->fVmcsFieldsRead & HMVMX_READ_GUEST_LINEAR_ADDR))
    {
        int rc = VMXReadVmcsGstN(VMX_VMCS_RO_GUEST_LINEAR_ADDR, &pVmxTransient->uGuestLinearAddr); NOREF(pVCpu);
        AssertRCReturn(rc, rc);
        pVmxTransient->fVmcsFieldsRead |= HMVMX_READ_GUEST_LINEAR_ADDR;
    }
    return VINF_SUCCESS;
}


/**
 * Reads the IDT-vectoring information field from the VMCS into the VMX
 * transient structure.
 *
 * @returns VBox status code.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxReadIdtVectoringInfoVmcs(PVMXTRANSIENT pVmxTransient)
{
    if (!(pVmxTransient->fVmcsFieldsRead & HMVMX_READ_IDT_VECTORING_INFO))
    {
        int rc = VMXReadVmcs32(VMX_VMCS32_RO_IDT_VECTORING_INFO, &pVmxTransient->uIdtVectoringInfo);
        AssertRCReturn(rc, rc);
        pVmxTransient->fVmcsFieldsRead |= HMVMX_READ_IDT_VECTORING_INFO;
    }
    return VINF_SUCCESS;
}


/**
 * Reads the IDT-vectoring error code from the VMCS into the VMX
 * transient structure.
 *
 * @returns VBox status code.
 * @param   pVmxTransient   The VMX-transient structure.
 */
DECLINLINE(int) hmR0VmxReadIdtVectoringErrorCodeVmcs(PVMXTRANSIENT pVmxTransient)
{
    if (!(pVmxTransient->fVmcsFieldsRead & HMVMX_READ_IDT_VECTORING_ERROR_CODE))
    {
        int rc = VMXReadVmcs32(VMX_VMCS32_RO_IDT_VECTORING_ERROR_CODE, &pVmxTransient->uIdtVectoringErrorCode);
        AssertRCReturn(rc, rc);
        pVmxTransient->fVmcsFieldsRead |= HMVMX_READ_IDT_VECTORING_ERROR_CODE;
    }
    return VINF_SUCCESS;
}


/**
 * Enters VMX root mode operation on the current CPU.
 *
 * @returns VBox status code.
 * @param   pVM             The cross context VM structure. Can be
 *                          NULL, after a resume.
 * @param   HCPhysCpuPage   Physical address of the VMXON region.
 * @param   pvCpuPage       Pointer to the VMXON region.
 */
static int hmR0VmxEnterRootMode(PVM pVM, RTHCPHYS HCPhysCpuPage, void *pvCpuPage)
{
    Assert(HCPhysCpuPage && HCPhysCpuPage != NIL_RTHCPHYS);
    Assert(RT_ALIGN_T(HCPhysCpuPage, _4K, RTHCPHYS) == HCPhysCpuPage);
    Assert(pvCpuPage);
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    if (pVM)
    {
        /* Write the VMCS revision identifier to the VMXON region. */
        *(uint32_t *)pvCpuPage = RT_BF_GET(pVM->hm.s.vmx.Msrs.u64Basic, VMX_BF_BASIC_VMCS_ID);
    }

    /* Paranoid: Disable interrupts as, in theory, interrupt handlers might mess with CR4. */
    RTCCUINTREG const fEFlags = ASMIntDisableFlags();

    /* Enable the VMX bit in CR4 if necessary. */
    RTCCUINTREG const uOldCr4 = SUPR0ChangeCR4(X86_CR4_VMXE, RTCCUINTREG_MAX);

    /* Enter VMX root mode. */
    int rc = VMXEnable(HCPhysCpuPage);
    if (RT_FAILURE(rc))
    {
        if (!(uOldCr4 & X86_CR4_VMXE))
            SUPR0ChangeCR4(0 /* fOrMask */, ~X86_CR4_VMXE);

        if (pVM)
            pVM->hm.s.vmx.HCPhysVmxEnableError = HCPhysCpuPage;
    }

    /* Restore interrupts. */
    ASMSetFlags(fEFlags);
    return rc;
}


/**
 * Exits VMX root mode operation on the current CPU.
 *
 * @returns VBox status code.
 */
static int hmR0VmxLeaveRootMode(void)
{
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    /* Paranoid: Disable interrupts as, in theory, interrupts handlers might mess with CR4. */
    RTCCUINTREG const fEFlags = ASMIntDisableFlags();

    /* If we're for some reason not in VMX root mode, then don't leave it. */
    RTCCUINTREG const uHostCr4 = ASMGetCR4();

    int rc;
    if (uHostCr4 & X86_CR4_VMXE)
    {
        /* Exit VMX root mode and clear the VMX bit in CR4. */
        VMXDisable();
        SUPR0ChangeCR4(0 /* fOrMask */, ~X86_CR4_VMXE);
        rc = VINF_SUCCESS;
    }
    else
        rc = VERR_VMX_NOT_IN_VMX_ROOT_MODE;

    /* Restore interrupts. */
    ASMSetFlags(fEFlags);
    return rc;
}


/**
 * Allocates and maps a physically contiguous page. The allocated page is
 * zero'd out (used by various VT-x structures).
 *
 * @returns IPRT status code.
 * @param   pMemObj         Pointer to the ring-0 memory object.
 * @param   ppVirt          Where to store the virtual address of the
 *                          allocation.
 * @param   pHCPhys         Where to store the physical address of the
 *                          allocation.
 */
static int hmR0VmxPageAllocZ(PRTR0MEMOBJ pMemObj, PRTR0PTR ppVirt, PRTHCPHYS pHCPhys)
{
    AssertPtr(pMemObj);
    AssertPtr(ppVirt);
    AssertPtr(pHCPhys);
    int rc = RTR0MemObjAllocCont(pMemObj, X86_PAGE_4K_SIZE, false /* fExecutable */);
    if (RT_FAILURE(rc))
        return rc;
    *ppVirt  = RTR0MemObjAddress(*pMemObj);
    *pHCPhys = RTR0MemObjGetPagePhysAddr(*pMemObj, 0 /* iPage */);
    ASMMemZero32(*ppVirt, X86_PAGE_4K_SIZE);
    return VINF_SUCCESS;
}


/**
 * Frees and unmaps an allocated, physical page.
 *
 * @param   pMemObj         Pointer to the ring-0 memory object.
 * @param   ppVirt          Where to re-initialize the virtual address of
 *                          allocation as 0.
 * @param   pHCPhys         Where to re-initialize the physical address of the
 *                          allocation as 0.
 */
static void hmR0VmxPageFree(PRTR0MEMOBJ pMemObj, PRTR0PTR ppVirt, PRTHCPHYS pHCPhys)
{
    AssertPtr(pMemObj);
    AssertPtr(ppVirt);
    AssertPtr(pHCPhys);
    /* NULL is valid, accepted and ignored by the free function below. */
    RTR0MemObjFree(*pMemObj, true /* fFreeMappings */);
    *pMemObj = NIL_RTR0MEMOBJ;
    *ppVirt  = NULL;
    *pHCPhys = NIL_RTHCPHYS;
}


/**
 * Initializes a VMCS info. object.
 *
 * @param   pVmcsInfo   The VMCS info. object.
 */
static void hmR0VmxInitVmcsInfo(PVMXVMCSINFO pVmcsInfo)
{
    memset(pVmcsInfo, 0, sizeof(*pVmcsInfo));

    Assert(pVmcsInfo->hMemObjVmcs          == NIL_RTR0MEMOBJ);
    Assert(pVmcsInfo->hMemObjMsrBitmap     == NIL_RTR0MEMOBJ);
    Assert(pVmcsInfo->hMemObjGuestMsrLoad  == NIL_RTR0MEMOBJ);
    Assert(pVmcsInfo->hMemObjGuestMsrStore == NIL_RTR0MEMOBJ);
    Assert(pVmcsInfo->hMemObjHostMsrLoad   == NIL_RTR0MEMOBJ);
    pVmcsInfo->HCPhysVmcs          = NIL_RTHCPHYS;
    pVmcsInfo->HCPhysMsrBitmap     = NIL_RTHCPHYS;
    pVmcsInfo->HCPhysGuestMsrLoad  = NIL_RTHCPHYS;
    pVmcsInfo->HCPhysGuestMsrStore = NIL_RTHCPHYS;
    pVmcsInfo->HCPhysHostMsrLoad   = NIL_RTHCPHYS;
    pVmcsInfo->HCPhysVirtApic      = NIL_RTHCPHYS;
    pVmcsInfo->HCPhysEPTP          = NIL_RTHCPHYS;
    pVmcsInfo->u64VmcsLinkPtr      = NIL_RTHCPHYS;
}


/**
 * Frees the VT-x structures for a VMCS info. object.
 *
 * @param   pVM         The cross context VM structure.
 * @param   pVmcsInfo   The VMCS info. object.
 */
static void hmR0VmxFreeVmcsInfo(PVM pVM, PVMXVMCSINFO pVmcsInfo)
{
    hmR0VmxPageFree(&pVmcsInfo->hMemObjVmcs, &pVmcsInfo->pvVmcs, &pVmcsInfo->HCPhysVmcs);

    if (pVM->hm.s.vmx.Msrs.ProcCtls.n.allowed1 & VMX_PROC_CTLS_USE_MSR_BITMAPS)
        hmR0VmxPageFree(&pVmcsInfo->hMemObjMsrBitmap, &pVmcsInfo->pvMsrBitmap, &pVmcsInfo->HCPhysMsrBitmap);

    hmR0VmxPageFree(&pVmcsInfo->hMemObjHostMsrLoad,   &pVmcsInfo->pvHostMsrLoad,   &pVmcsInfo->HCPhysHostMsrLoad);
    hmR0VmxPageFree(&pVmcsInfo->hMemObjGuestMsrLoad,  &pVmcsInfo->pvGuestMsrLoad,  &pVmcsInfo->HCPhysGuestMsrLoad);
    hmR0VmxPageFree(&pVmcsInfo->hMemObjGuestMsrStore, &pVmcsInfo->pvGuestMsrStore, &pVmcsInfo->HCPhysGuestMsrStore);

    hmR0VmxInitVmcsInfo(pVmcsInfo);
}


/**
 * Allocates the VT-x structures for a VMCS info. object.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmcsInfo       The VMCS info. object.
 * @param   fIsNstGstVmcs   Whether this is a nested-guest VMCS.
 */
static int hmR0VmxAllocVmcsInfo(PVMCPU pVCpu, PVMXVMCSINFO pVmcsInfo, bool fIsNstGstVmcs)
{
    PVM pVM = pVCpu->CTX_SUFF(pVM);

    /* Allocate the guest VM control structure (VMCS). */
    int rc = hmR0VmxPageAllocZ(&pVmcsInfo->hMemObjVmcs, &pVmcsInfo->pvVmcs, &pVmcsInfo->HCPhysVmcs);
    if (RT_SUCCESS(rc))
    {
        if (!fIsNstGstVmcs)
        {
            /* Get the allocated virtual-APIC page from the virtual APIC device. */
            if (   PDMHasApic(pVCpu->CTX_SUFF(pVM))
                && (pVM->hm.s.vmx.Msrs.ProcCtls.n.allowed1 & VMX_PROC_CTLS_USE_TPR_SHADOW))
            {
                rc = APICGetApicPageForCpu(pVCpu, &pVmcsInfo->HCPhysVirtApic, (PRTR0PTR)&pVmcsInfo->pbVirtApic,
                                           NULL /* pR3Ptr */, NULL /* pRCPtr */);
            }
        }
        else
        {
            Assert(pVmcsInfo->HCPhysVirtApic == NIL_RTHCPHYS);
            Assert(!pVmcsInfo->pbVirtApic);
        }

        if (RT_SUCCESS(rc))
        {
            /*
             * Allocate the MSR-bitmap if supported by the CPU. The MSR-bitmap is for
             * transparent accesses of specific MSRs.
             *
             * If the condition for enabling MSR bitmaps changes here, don't forget to
             * update HMIsMsrBitmapActive().
             *
             * We don't share MSR bitmaps between the guest and nested-guest as we then
             * don't need to care about carefully restoring the guest MSR bitmap.
             * The guest visible nested-guest MSR bitmap needs to remain unchanged.
             * Hence, allocate a separate MSR bitmap for the guest and nested-guest.
             * We also don't need to re-initialize the nested-guest MSR bitmap here as
             * we do that later while merging VMCS.
             */
            if (pVM->hm.s.vmx.Msrs.ProcCtls.n.allowed1 & VMX_PROC_CTLS_USE_MSR_BITMAPS)
            {
                rc = hmR0VmxPageAllocZ(&pVmcsInfo->hMemObjMsrBitmap, &pVmcsInfo->pvMsrBitmap, &pVmcsInfo->HCPhysMsrBitmap);
                if (   RT_SUCCESS(rc)
                    && !fIsNstGstVmcs)
                    ASMMemFill32(pVmcsInfo->pvMsrBitmap, X86_PAGE_4K_SIZE, UINT32_C(0xffffffff));
            }

            if (RT_SUCCESS(rc))
            {
                /*
                 * Allocate the VM-entry MSR-load area for the guest MSRs.
                 *
                 * Similar to MSR-bitmaps, we do not share the auto MSR-load/store are between
                 * the guest and nested-guest.
                 */
                rc = hmR0VmxPageAllocZ(&pVmcsInfo->hMemObjGuestMsrLoad, &pVmcsInfo->pvGuestMsrLoad,
                                       &pVmcsInfo->HCPhysGuestMsrLoad);
                if (RT_SUCCESS(rc))
                {
                    /*
                     * We use the same page for VM-entry MSR-load and VM-exit MSR store areas.
                     * These contain the guest MSRs to load on VM-entry and store on VM-exit.
                     */
                    Assert(pVmcsInfo->hMemObjGuestMsrStore == NIL_RTR0MEMOBJ);
                    pVmcsInfo->pvGuestMsrStore     = pVmcsInfo->pvGuestMsrLoad;
                    pVmcsInfo->HCPhysGuestMsrStore = pVmcsInfo->HCPhysGuestMsrLoad;

                    /* Allocate the VM-exit MSR-load page for the host MSRs. */
                    rc = hmR0VmxPageAllocZ(&pVmcsInfo->hMemObjHostMsrLoad, &pVmcsInfo->pvHostMsrLoad,
                                           &pVmcsInfo->HCPhysHostMsrLoad);
                }
            }
        }
    }

    return rc;
}


/**
 * Free all VT-x structures for the VM.
 *
 * @returns IPRT status code.
 * @param   pVM             The cross context VM structure.
 */
static void hmR0VmxStructsFree(PVM pVM)
{
#ifdef VBOX_WITH_CRASHDUMP_MAGIC
    hmR0VmxPageFree(&pVM->hm.s.vmx.hMemObjScratch, &pVM->hm.s.vmx.pbScratch, &pVM->hm.s.vmx.HCPhysScratch);
#endif
    hmR0VmxPageFree(&pVM->hm.s.vmx.hMemObjApicAccess, (PRTR0PTR)&pVM->hm.s.vmx.pbApicAccess, &pVM->hm.s.vmx.HCPhysApicAccess);

    for (VMCPUID idCpu = 0; idCpu < pVM->cCpus; idCpu++)
    {
        PVMCPU pVCpu = &pVM->aCpus[idCpu];
        PVMXVMCSINFO pVmcsInfo = &pVCpu->hm.s.vmx.VmcsInfo;
        hmR0VmxFreeVmcsInfo(pVM, pVmcsInfo);
#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
        if (pVM->cpum.ro.GuestFeatures.fVmx)
        {
            pVmcsInfo = &pVCpu->hm.s.vmx.VmcsInfoNstGst;
            hmR0VmxFreeVmcsInfo(pVM, pVmcsInfo);
        }
#endif
    }
}


/**
 * Allocate all VT-x structures for the VM.
 *
 * @returns IPRT status code.
 * @param   pVM             The cross context VM structure.
 */
static int hmR0VmxStructsAlloc(PVM pVM)
{
    /*
     * Sanity check the VMCS size reported by the CPU as we assume 4KB allocations.
     * The VMCS size cannot be more than 4096 bytes.
     *
     * See Intel spec. Appendix A.1 "Basic VMX Information".
     */
    uint32_t const cbVmcs = RT_BF_GET(pVM->hm.s.vmx.Msrs.u64Basic, VMX_BF_BASIC_VMCS_SIZE);
    if (cbVmcs <= X86_PAGE_4K_SIZE)
    { /* likely */ }
    else
    {
         pVM->aCpus[0].hm.s.u32HMError = VMX_UFC_INVALID_VMCS_SIZE;
         return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
    }

    /*
     * Initialize/check members up-front so we can cleanup en masse on allocation failures.
     */
#ifdef VBOX_WITH_CRASHDUMP_MAGIC
    Assert(pVM->hm.s.vmx.hMemObjScratch == NIL_RTR0MEMOBJ);
    Assert(pVM->hm.s.vmx.pbScratch == NULL);
    pVM->hm.s.vmx.HCPhysScratch = NIL_RTHCPHYS;
#endif

    Assert(pVM->hm.s.vmx.hMemObjApicAccess == NIL_RTR0MEMOBJ);
    Assert(pVM->hm.s.vmx.pbApicAccess == NULL);
    pVM->hm.s.vmx.HCPhysApicAccess = NIL_RTHCPHYS;

    for (VMCPUID idCpu = 0; idCpu < pVM->cCpus; idCpu++)
    {
        PVMCPU pVCpu = &pVM->aCpus[idCpu];
        hmR0VmxInitVmcsInfo(&pVCpu->hm.s.vmx.VmcsInfo);
        hmR0VmxInitVmcsInfo(&pVCpu->hm.s.vmx.VmcsInfoNstGst);
    }

    /*
     * Allocate per-VM VT-x structures.
     */
    int rc = VINF_SUCCESS;
#ifdef VBOX_WITH_CRASHDUMP_MAGIC
    /* Allocate crash-dump magic scratch page. */
    rc = hmR0VmxPageAllocZ(&pVM->hm.s.vmx.hMemObjScratch, &pVM->hm.s.vmx.pbScratch, &pVM->hm.s.vmx.HCPhysScratch);
    if (RT_FAILURE(rc))
    {
        hmR0VmxStructsFree(pVM);
        return rc;
    }
    strcpy((char *)pVM->hm.s.vmx.pbScratch, "SCRATCH Magic");
    *(uint64_t *)(pVM->hm.s.vmx.pbScratch + 16) = UINT64_C(0xdeadbeefdeadbeef);
#endif

    /* Allocate the APIC-access page for trapping APIC accesses from the guest. */
    if (pVM->hm.s.vmx.Msrs.ProcCtls2.n.allowed1 & VMX_PROC_CTLS2_VIRT_APIC_ACCESS)
    {
        rc = hmR0VmxPageAllocZ(&pVM->hm.s.vmx.hMemObjApicAccess, (PRTR0PTR)&pVM->hm.s.vmx.pbApicAccess,
                               &pVM->hm.s.vmx.HCPhysApicAccess);
        if (RT_FAILURE(rc))
        {
            hmR0VmxStructsFree(pVM);
            return rc;
        }
    }

    /*
     * Initialize per-VCPU VT-x structures.
     */
    for (VMCPUID idCpu = 0; idCpu < pVM->cCpus; idCpu++)
    {
        /* Allocate the guest VMCS structures. */
        PVMCPU pVCpu = &pVM->aCpus[idCpu];
        rc = hmR0VmxAllocVmcsInfo(pVCpu, &pVCpu->hm.s.vmx.VmcsInfo, false /* fIsNstGstVmcs */);
        if (RT_SUCCESS(rc))
        {
#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
            /* Allocate the nested-guest VMCS structures, when the VMX feature is exposed to the guest. */
            if (pVM->cpum.ro.GuestFeatures.fVmx)
            {
                rc = hmR0VmxAllocVmcsInfo(pVCpu, &pVCpu->hm.s.vmx.VmcsInfoNstGst, true /* fIsNstGstVmcs */);
                if (RT_SUCCESS(rc))
                { /* likely */ }
                else
                    break;
            }
#endif
        }
        else
            break;
    }

    if (RT_FAILURE(rc))
    {
        hmR0VmxStructsFree(pVM);
        return rc;
    }

    return VINF_SUCCESS;
}


#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
/**
 * Returns whether an MSR at the given MSR-bitmap offset is intercepted or not.
 *
 * @returns @c true if the MSR is intercepted, @c false otherwise.
 * @param   pvMsrBitmap     The MSR bitmap.
 * @param   offMsr          The MSR byte offset.
 * @param   iBit            The bit offset from the byte offset.
 */
DECLINLINE(bool) hmR0VmxIsMsrBitSet(const void *pvMsrBitmap, uint16_t offMsr, int32_t iBit)
{
    uint8_t const * const pbMsrBitmap = (uint8_t const * const)pvMsrBitmap;
    Assert(pbMsrBitmap);
    Assert(offMsr + (iBit >> 3) <= X86_PAGE_4K_SIZE);
    return ASMBitTest(pbMsrBitmap + offMsr, iBit);
}
#endif


/**
 * Sets the permission bits for the specified MSR in the given MSR bitmap.
 *
 * If the passed VMCS is a nested-guest VMCS, this function ensures that the
 * read/write intercept is cleared from the MSR bitmap used for hardware-assisted
 * VMX execution of the nested-guest, only if nested-guest is also not intercepting
 * the read/write access of this MSR.
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmcsInfo       The VMCS info. object.
 * @param   fIsNstGstVmcs   Whether this is a nested-guest VMCS.
 * @param   idMsr           The MSR value.
 * @param   fMsrpm          The MSR permissions (see VMXMSRPM_XXX). This must
 *                          include both a read -and- a write permission!
 *
 * @sa      CPUMGetVmxMsrPermission.
 * @remarks Can be called with interrupts disabled.
 */
static void hmR0VmxSetMsrPermission(PVMCPU pVCpu, PVMXVMCSINFO pVmcsInfo, bool fIsNstGstVmcs, uint32_t idMsr, uint32_t fMsrpm)
{
    uint8_t *pbMsrBitmap = (uint8_t *)pVmcsInfo->pvMsrBitmap;
    Assert(pbMsrBitmap);
    Assert(VMXMSRPM_IS_FLAG_VALID(fMsrpm));

    /*
     * MSR-bitmap Layout:
     *   Byte index            MSR range            Interpreted as
     * 0x000 - 0x3ff    0x00000000 - 0x00001fff    Low MSR read bits.
     * 0x400 - 0x7ff    0xc0000000 - 0xc0001fff    High MSR read bits.
     * 0x800 - 0xbff    0x00000000 - 0x00001fff    Low MSR write bits.
     * 0xc00 - 0xfff    0xc0000000 - 0xc0001fff    High MSR write bits.
     *
     * A bit corresponding to an MSR within the above range causes a VM-exit
     * if the bit is 1 on executions of RDMSR/WRMSR.  If an MSR falls out of
     * the MSR range, it always cause a VM-exit.
     *
     * See Intel spec. 24.6.9 "MSR-Bitmap Address".
     */
    uint16_t const offBitmapRead  = 0;
    uint16_t const offBitmapWrite = 0x800;
    uint16_t       offMsr;
    int32_t        iBit;
    if (idMsr <= UINT32_C(0x00001fff))
    {
        offMsr = 0;
        iBit   = idMsr;
    }
    else if (idMsr - UINT32_C(0xc0000000) <= UINT32_C(0x00001fff))
    {
        offMsr = 0x400;
        iBit   = idMsr - UINT32_C(0xc0000000);
    }
    else
        AssertMsgFailedReturnVoid(("Invalid MSR %#RX32\n", idMsr));

    /*
     * Set the MSR read permission.
     */
    uint16_t const offMsrRead = offBitmapRead + offMsr;
    Assert(offMsrRead + (iBit >> 3) < offBitmapWrite);
    if (fMsrpm & VMXMSRPM_ALLOW_RD)
    {
#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
        bool const fClear = !fIsNstGstVmcs ? true
                          : !hmR0VmxIsMsrBitSet(pVCpu->cpum.GstCtx.hwvirt.vmx.CTX_SUFF(pvMsrBitmap), offMsrRead, iBit);
#else
        RT_NOREF2(pVCpu, fIsNstGstVmcs);
        bool const fClear = true;
#endif
        if (fClear)
            ASMBitClear(pbMsrBitmap + offMsrRead, iBit);
    }
    else
        ASMBitSet(pbMsrBitmap + offMsrRead, iBit);

    /*
     * Set the MSR write permission.
     */
    uint16_t const offMsrWrite = offBitmapWrite + offMsr;
    Assert(offMsrWrite + (iBit >> 3) < X86_PAGE_4K_SIZE);
    if (fMsrpm & VMXMSRPM_ALLOW_WR)
    {
#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
        bool const fClear = !fIsNstGstVmcs ? true
                          : !hmR0VmxIsMsrBitSet(pVCpu->cpum.GstCtx.hwvirt.vmx.CTX_SUFF(pvMsrBitmap), offMsrWrite, iBit);
#else
        RT_NOREF2(pVCpu, fIsNstGstVmcs);
        bool const fClear = true;
#endif
        if (fClear)
            ASMBitClear(pbMsrBitmap + offMsrWrite, iBit);
    }
    else
        ASMBitSet(pbMsrBitmap + offMsrWrite, iBit);
}


/**
 * Updates the VMCS with the number of effective MSRs in the auto-load/store MSR
 * area.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pVmcsInfo   The VMCS info. object.
 * @param   cMsrs       The number of MSRs.
 */
static int hmR0VmxSetAutoLoadStoreMsrCount(PVMCPU pVCpu, PVMXVMCSINFO pVmcsInfo, uint32_t cMsrs)
{
    /* Shouldn't ever happen but there -is- a number. We're well within the recommended 512. */
    uint32_t const cMaxSupportedMsrs = VMX_MISC_MAX_MSRS(pVCpu->CTX_SUFF(pVM)->hm.s.vmx.Msrs.u64Misc);
    if (RT_LIKELY(cMsrs < cMaxSupportedMsrs))
    {
        /* Commit the MSR counts to the VMCS and update the cache. */
        if (pVmcsInfo->cEntryMsrLoad != cMsrs)
        {
            int rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_ENTRY_MSR_LOAD_COUNT, cMsrs);
            rc    |= VMXWriteVmcs32(VMX_VMCS32_CTRL_EXIT_MSR_STORE_COUNT, cMsrs);
            rc    |= VMXWriteVmcs32(VMX_VMCS32_CTRL_EXIT_MSR_LOAD_COUNT,  cMsrs);
            AssertRCReturn(rc, rc);

            pVmcsInfo->cEntryMsrLoad = cMsrs;
            pVmcsInfo->cExitMsrStore = cMsrs;
            pVmcsInfo->cExitMsrLoad  = cMsrs;
        }
        return VINF_SUCCESS;
    }

    LogRel(("Auto-load/store MSR count exceeded! cMsrs=%u MaxSupported=%u\n", cMsrs, cMaxSupportedMsrs));
    pVCpu->hm.s.u32HMError = VMX_UFC_INSUFFICIENT_GUEST_MSR_STORAGE;
    return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
}


/**
 * Adds a new (or updates the value of an existing) guest/host MSR
 * pair to be swapped during the world-switch as part of the
 * auto-load/store MSR area in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVCpu               The cross context virtual CPU structure.
 * @param   pVmxTransient       The VMX-transient structure.
 * @param   idMsr               The MSR.
 * @param   uGuestMsrValue      Value of the guest MSR.
 * @param   fSetReadWrite       Whether to set the guest read/write access of this
 *                              MSR (thus not causing a VM-exit).
 * @param   fUpdateHostMsr      Whether to update the value of the host MSR if
 *                              necessary.
 */
static int hmR0VmxAddAutoLoadStoreMsr(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient, uint32_t idMsr, uint64_t uGuestMsrValue,
                                      bool fSetReadWrite, bool fUpdateHostMsr)
{
    PVMXVMCSINFO pVmcsInfo     = pVmxTransient->pVmcsInfo;
    bool const   fIsNstGstVmcs = pVmxTransient->fIsNestedGuest;
    PVMXAUTOMSR  pGuestMsrLoad = (PVMXAUTOMSR)pVmcsInfo->pvGuestMsrLoad;
    uint32_t     cMsrs         = pVmcsInfo->cEntryMsrLoad;
    uint32_t     i;

    /* Paranoia. */
    Assert(pGuestMsrLoad);

    LogFlowFunc(("pVCpu=%p idMsr=%#RX32 uGestMsrValue=%#RX64\n", pVCpu, idMsr, uGuestMsrValue));

    /* Check if the MSR already exists in the VM-entry MSR-load area. */
    for (i = 0; i < cMsrs; i++)
    {
        if (pGuestMsrLoad[i].u32Msr == idMsr)
            break;
    }

    bool fAdded = false;
    if (i == cMsrs)
    {
        /* The MSR does not exist, bump the MSR count to make room for the new MSR. */
        ++cMsrs;
        int rc = hmR0VmxSetAutoLoadStoreMsrCount(pVCpu, pVmcsInfo, cMsrs);
        AssertMsgRCReturn(rc, ("Insufficient space to add MSR to VM-entry MSR-load/store area %u\n", idMsr), rc);

        /* Set the guest to read/write this MSR without causing VM-exits. */
        if (   fSetReadWrite
            && (pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_USE_MSR_BITMAPS))
            hmR0VmxSetMsrPermission(pVCpu, pVmcsInfo, fIsNstGstVmcs, idMsr, VMXMSRPM_ALLOW_RD_WR);

        LogFlowFunc(("MSR added, cMsrs now %u\n", cMsrs));
        fAdded = true;
    }

    /* Update the MSR value for the newly added or already existing MSR. */
    pGuestMsrLoad[i].u32Msr   = idMsr;
    pGuestMsrLoad[i].u64Value = uGuestMsrValue;

    /* Create the corresponding slot in the VM-exit MSR-store area if we use a different page. */
    if (hmR0VmxIsSeparateExitMsrStoreAreaVmcs(pVmcsInfo))
    {
        PVMXAUTOMSR pGuestMsrStore = (PVMXAUTOMSR)pVmcsInfo->pvGuestMsrStore;
        pGuestMsrStore[i].u32Msr   = idMsr;
        pGuestMsrStore[i].u64Value = uGuestMsrValue;
    }

    /* Update the corresponding slot in the host MSR area. */
    PVMXAUTOMSR pHostMsr = (PVMXAUTOMSR)pVmcsInfo->pvHostMsrLoad;
    Assert(pHostMsr != pVmcsInfo->pvGuestMsrLoad);
    Assert(pHostMsr != pVmcsInfo->pvGuestMsrStore);
    pHostMsr[i].u32Msr = idMsr;

    /*
     * Only if the caller requests to update the host MSR value AND we've newly added the
     * MSR to the host MSR area do we actually update the value. Otherwise, it will be
     * updated by hmR0VmxUpdateAutoLoadHostMsrs().
     *
     * We do this for performance reasons since reading MSRs may be quite expensive.
     */
    if (fAdded)
    {
        if (fUpdateHostMsr)
        {
            Assert(!VMMRZCallRing3IsEnabled(pVCpu));
            Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));
            pHostMsr[i].u64Value = ASMRdMsr(idMsr);
        }
        else
        {
            /* Someone else can do the work. */
            pVCpu->hm.s.vmx.fUpdatedHostAutoMsrs = false;
        }
    }
    return VINF_SUCCESS;
}


/**
 * Removes a guest/host MSR pair to be swapped during the world-switch from the
 * auto-load/store MSR area in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 * @param   idMsr           The MSR.
 */
static int hmR0VmxRemoveAutoLoadStoreMsr(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient, uint32_t idMsr)
{
    PVMXVMCSINFO pVmcsInfo     = pVmxTransient->pVmcsInfo;
    bool const   fIsNstGstVmcs = pVmxTransient->fIsNestedGuest;
    PVMXAUTOMSR  pGuestMsrLoad = (PVMXAUTOMSR)pVmcsInfo->pvGuestMsrLoad;
    uint32_t     cMsrs         = pVmcsInfo->cEntryMsrLoad;

    LogFlowFunc(("pVCpu=%p idMsr=%#RX32\n", pVCpu, idMsr));

    for (uint32_t i = 0; i < cMsrs; i++)
    {
        /* Find the MSR. */
        if (pGuestMsrLoad[i].u32Msr == idMsr)
        {
            /*
             * If it's the last MSR, we only need to reduce the MSR count.
             * If it's -not- the last MSR, copy the last MSR in place of it and reduce the MSR count.
             */
            if (i < cMsrs - 1)
            {
                /* Remove it from the VM-entry MSR-load area. */
                pGuestMsrLoad[i].u32Msr   = pGuestMsrLoad[cMsrs - 1].u32Msr;
                pGuestMsrLoad[i].u64Value = pGuestMsrLoad[cMsrs - 1].u64Value;

                /* Remove it from the VM-exit MSR-store area if it's in a different page. */
                if (hmR0VmxIsSeparateExitMsrStoreAreaVmcs(pVmcsInfo))
                {
                    PVMXAUTOMSR pGuestMsrStore = (PVMXAUTOMSR)pVmcsInfo->pvGuestMsrStore;
                    Assert(pGuestMsrStore[i].u32Msr == idMsr);
                    pGuestMsrStore[i].u32Msr   = pGuestMsrStore[cMsrs - 1].u32Msr;
                    pGuestMsrStore[i].u64Value = pGuestMsrStore[cMsrs - 1].u64Value;
                }

                /* Remove it from the VM-exit MSR-load area. */
                PVMXAUTOMSR pHostMsr = (PVMXAUTOMSR)pVmcsInfo->pvHostMsrLoad;
                Assert(pHostMsr[i].u32Msr == idMsr);
                pHostMsr[i].u32Msr   = pHostMsr[cMsrs - 1].u32Msr;
                pHostMsr[i].u64Value = pHostMsr[cMsrs - 1].u64Value;
            }

            /* Reduce the count to reflect the removed MSR and bail. */
            --cMsrs;
            break;
        }
    }

    /* Update the VMCS if the count changed (meaning the MSR was found and removed). */
    if (cMsrs != pVmcsInfo->cEntryMsrLoad)
    {
        int rc = hmR0VmxSetAutoLoadStoreMsrCount(pVCpu, pVmcsInfo, cMsrs);
        AssertRCReturn(rc, rc);

        /* We're no longer swapping MSRs during the world-switch, intercept guest read/writes to them. */
        if (pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_USE_MSR_BITMAPS)
            hmR0VmxSetMsrPermission(pVCpu, pVmcsInfo, fIsNstGstVmcs, idMsr, VMXMSRPM_EXIT_RD | VMXMSRPM_EXIT_WR);

        Log4Func(("Removed MSR %#RX32, cMsrs=%u\n", idMsr, cMsrs));
        return VINF_SUCCESS;
    }

    return VERR_NOT_FOUND;
}


/**
 * Checks if the specified guest MSR is part of the VM-entry MSR-load area.
 *
 * @returns @c true if found, @c false otherwise.
 * @param   pVmcsInfo   The VMCS info. object.
 * @param   idMsr       The MSR to find.
 */
static bool hmR0VmxIsAutoLoadGuestMsr(PCVMXVMCSINFO pVmcsInfo, uint32_t idMsr)
{
    PCVMXAUTOMSR   pMsrs = (PCVMXAUTOMSR)pVmcsInfo->pvGuestMsrLoad;
    uint32_t const cMsrs = pVmcsInfo->cEntryMsrLoad;
    Assert(pMsrs);
    Assert(sizeof(*pMsrs) * cMsrs <= X86_PAGE_4K_SIZE);
    for (uint32_t i = 0; i < cMsrs; i++)
    {
        if (pMsrs[i].u32Msr == idMsr)
            return true;
    }
    return false;
}


/**
 * Updates the value of all host MSRs in the VM-exit MSR-load area.
 *
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pVmcsInfo   The VMCS info. object.
 *
 * @remarks No-long-jump zone!!!
 */
static void hmR0VmxUpdateAutoLoadHostMsrs(PCVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo)
{
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    PVMXAUTOMSR pHostMsrLoad = (PVMXAUTOMSR)pVmcsInfo->pvHostMsrLoad;
    uint32_t const cMsrs     = pVmcsInfo->cExitMsrLoad;
    Assert(pHostMsrLoad);
    Assert(sizeof(*pHostMsrLoad) * cMsrs <= X86_PAGE_4K_SIZE);
    LogFlowFunc(("pVCpu=%p cMsrs=%u\n", pVCpu, cMsrs));
    for (uint32_t i = 0; i < cMsrs; i++)
    {
        /*
         * Performance hack for the host EFER MSR. We use the cached value rather than re-read it.
         * Strict builds will catch mismatches in hmR0VmxCheckAutoLoadStoreMsrs(). See @bugref{7368}.
         */
        if (pHostMsrLoad[i].u32Msr == MSR_K6_EFER)
            pHostMsrLoad[i].u64Value = pVCpu->CTX_SUFF(pVM)->hm.s.vmx.u64HostMsrEfer;
        else
            pHostMsrLoad[i].u64Value = ASMRdMsr(pHostMsrLoad[i].u32Msr);
    }
}


/**
 * Saves a set of host MSRs to allow read/write passthru access to the guest and
 * perform lazy restoration of the host MSRs while leaving VT-x.
 *
 * @param   pVCpu       The cross context virtual CPU structure.
 *
 * @remarks No-long-jump zone!!!
 */
static void hmR0VmxLazySaveHostMsrs(PVMCPU pVCpu)
{
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    /*
     * Note: If you're adding MSRs here, make sure to update the MSR-bitmap accesses in hmR0VmxSetupVmcsProcCtls().
     */
    if (!(pVCpu->hm.s.vmx.fLazyMsrs & VMX_LAZY_MSRS_SAVED_HOST))
    {
        Assert(!(pVCpu->hm.s.vmx.fLazyMsrs & VMX_LAZY_MSRS_LOADED_GUEST));  /* Guest MSRs better not be loaded now. */
#if HC_ARCH_BITS == 64
        if (pVCpu->CTX_SUFF(pVM)->hm.s.fAllow64BitGuests)
        {
            pVCpu->hm.s.vmx.u64HostMsrLStar        = ASMRdMsr(MSR_K8_LSTAR);
            pVCpu->hm.s.vmx.u64HostMsrStar         = ASMRdMsr(MSR_K6_STAR);
            pVCpu->hm.s.vmx.u64HostMsrSfMask       = ASMRdMsr(MSR_K8_SF_MASK);
            pVCpu->hm.s.vmx.u64HostMsrKernelGsBase = ASMRdMsr(MSR_K8_KERNEL_GS_BASE);
        }
#endif
        pVCpu->hm.s.vmx.fLazyMsrs |= VMX_LAZY_MSRS_SAVED_HOST;
    }
}


/**
 * Checks whether the MSR belongs to the set of guest MSRs that we restore
 * lazily while leaving VT-x.
 *
 * @returns true if it does, false otherwise.
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   idMsr       The MSR to check.
 */
static bool hmR0VmxIsLazyGuestMsr(PCVMCPU pVCpu, uint32_t idMsr)
{
    NOREF(pVCpu);
#if HC_ARCH_BITS == 64
    if (pVCpu->CTX_SUFF(pVM)->hm.s.fAllow64BitGuests)
    {
        switch (idMsr)
        {
            case MSR_K8_LSTAR:
            case MSR_K6_STAR:
            case MSR_K8_SF_MASK:
            case MSR_K8_KERNEL_GS_BASE:
                return true;
        }
    }
#else
    RT_NOREF(pVCpu, idMsr);
#endif
    return false;
}


/**
 * Loads a set of guests MSRs to allow read/passthru to the guest.
 *
 * The name of this function is slightly confusing. This function does NOT
 * postpone loading, but loads the MSR right now. "hmR0VmxLazy" is simply a
 * common prefix for functions dealing with "lazy restoration" of the shared
 * MSRs.
 *
 * @param   pVCpu       The cross context virtual CPU structure.
 *
 * @remarks No-long-jump zone!!!
 */
static void hmR0VmxLazyLoadGuestMsrs(PVMCPU pVCpu)
{
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));
    Assert(!VMMRZCallRing3IsEnabled(pVCpu));

    Assert(pVCpu->hm.s.vmx.fLazyMsrs & VMX_LAZY_MSRS_SAVED_HOST);
#if HC_ARCH_BITS == 64
    if (pVCpu->CTX_SUFF(pVM)->hm.s.fAllow64BitGuests)
    {
        /*
         * If the guest MSRs are not loaded -and- if all the guest MSRs are identical
         * to the MSRs on the CPU (which are the saved host MSRs, see assertion above) then
         * we can skip a few MSR writes.
         *
         * Otherwise, it implies either 1. they're not loaded, or 2. they're loaded but the
         * guest MSR values in the guest-CPU context might be different to what's currently
         * loaded in the CPU. In either case, we need to write the new guest MSR values to the
         * CPU, see @bugref{8728}.
         */
        PCCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
        if (   !(pVCpu->hm.s.vmx.fLazyMsrs & VMX_LAZY_MSRS_LOADED_GUEST)
            && pCtx->msrKERNELGSBASE == pVCpu->hm.s.vmx.u64HostMsrKernelGsBase
            && pCtx->msrLSTAR        == pVCpu->hm.s.vmx.u64HostMsrLStar
            && pCtx->msrSTAR         == pVCpu->hm.s.vmx.u64HostMsrStar
            && pCtx->msrSFMASK       == pVCpu->hm.s.vmx.u64HostMsrSfMask)
        {
#ifdef VBOX_STRICT
            Assert(ASMRdMsr(MSR_K8_KERNEL_GS_BASE) == pCtx->msrKERNELGSBASE);
            Assert(ASMRdMsr(MSR_K8_LSTAR)          == pCtx->msrLSTAR);
            Assert(ASMRdMsr(MSR_K6_STAR)           == pCtx->msrSTAR);
            Assert(ASMRdMsr(MSR_K8_SF_MASK)        == pCtx->msrSFMASK);
#endif
        }
        else
        {
            ASMWrMsr(MSR_K8_KERNEL_GS_BASE, pCtx->msrKERNELGSBASE);
            ASMWrMsr(MSR_K8_LSTAR,          pCtx->msrLSTAR);
            ASMWrMsr(MSR_K6_STAR,           pCtx->msrSTAR);
            ASMWrMsr(MSR_K8_SF_MASK,        pCtx->msrSFMASK);
        }
    }
#endif
    pVCpu->hm.s.vmx.fLazyMsrs |= VMX_LAZY_MSRS_LOADED_GUEST;
}


/**
 * Performs lazy restoration of the set of host MSRs if they were previously
 * loaded with guest MSR values.
 *
 * @param   pVCpu       The cross context virtual CPU structure.
 *
 * @remarks No-long-jump zone!!!
 * @remarks The guest MSRs should have been saved back into the guest-CPU
 *          context by hmR0VmxImportGuestState()!!!
 */
static void hmR0VmxLazyRestoreHostMsrs(PVMCPU pVCpu)
{
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));
    Assert(!VMMRZCallRing3IsEnabled(pVCpu));

    if (pVCpu->hm.s.vmx.fLazyMsrs & VMX_LAZY_MSRS_LOADED_GUEST)
    {
        Assert(pVCpu->hm.s.vmx.fLazyMsrs & VMX_LAZY_MSRS_SAVED_HOST);
#if HC_ARCH_BITS == 64
        if (pVCpu->CTX_SUFF(pVM)->hm.s.fAllow64BitGuests)
        {
            ASMWrMsr(MSR_K8_LSTAR,          pVCpu->hm.s.vmx.u64HostMsrLStar);
            ASMWrMsr(MSR_K6_STAR,           pVCpu->hm.s.vmx.u64HostMsrStar);
            ASMWrMsr(MSR_K8_SF_MASK,        pVCpu->hm.s.vmx.u64HostMsrSfMask);
            ASMWrMsr(MSR_K8_KERNEL_GS_BASE, pVCpu->hm.s.vmx.u64HostMsrKernelGsBase);
        }
#endif
    }
    pVCpu->hm.s.vmx.fLazyMsrs &= ~(VMX_LAZY_MSRS_LOADED_GUEST | VMX_LAZY_MSRS_SAVED_HOST);
}


/**
 * Verifies that our cached values of the VMCS fields are all consistent with
 * what's actually present in the VMCS.
 *
 * @returns VBox status code.
 * @retval  VINF_SUCCESS if all our caches match their respective VMCS fields.
 * @retval  VERR_VMX_VMCS_FIELD_CACHE_INVALID if a cache field doesn't match the
 *                                            VMCS content. HMCPU error-field is
 *                                            updated, see VMX_VCI_XXX.
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pVmcsInfo   The VMCS info. object.
 */
static int hmR0VmxCheckVmcsCtls(PVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo)
{
    uint32_t u32Val;
    int rc = VMXReadVmcs32(VMX_VMCS32_CTRL_ENTRY, &u32Val);
    AssertRCReturn(rc, rc);
    AssertMsgReturnStmt(pVmcsInfo->u32EntryCtls == u32Val,
                        ("Cache=%#RX32 VMCS=%#RX32\n", pVmcsInfo->u32EntryCtls, u32Val),
                        pVCpu->hm.s.u32HMError = VMX_VCI_CTRL_ENTRY,
                        VERR_VMX_VMCS_FIELD_CACHE_INVALID);

    rc = VMXReadVmcs32(VMX_VMCS32_CTRL_EXIT, &u32Val);
    AssertRCReturn(rc, rc);
    AssertMsgReturnStmt(pVmcsInfo->u32ExitCtls == u32Val,
                        ("Cache=%#RX32 VMCS=%#RX32\n", pVmcsInfo->u32ExitCtls, u32Val),
                        pVCpu->hm.s.u32HMError = VMX_VCI_CTRL_EXIT,
                        VERR_VMX_VMCS_FIELD_CACHE_INVALID);

    rc = VMXReadVmcs32(VMX_VMCS32_CTRL_PIN_EXEC, &u32Val);
    AssertRCReturn(rc, rc);
    AssertMsgReturnStmt(pVmcsInfo->u32PinCtls == u32Val,
                        ("Cache=%#RX32 VMCS=%#RX32\n", pVmcsInfo->u32PinCtls, u32Val),
                        pVCpu->hm.s.u32HMError = VMX_VCI_CTRL_PIN_EXEC,
                        VERR_VMX_VMCS_FIELD_CACHE_INVALID);

    rc = VMXReadVmcs32(VMX_VMCS32_CTRL_PROC_EXEC, &u32Val);
    AssertRCReturn(rc, rc);
    AssertMsgReturnStmt(pVmcsInfo->u32ProcCtls == u32Val,
                        ("Cache=%#RX32 VMCS=%#RX32\n", pVmcsInfo->u32ProcCtls, u32Val),
                        pVCpu->hm.s.u32HMError = VMX_VCI_CTRL_PROC_EXEC,
                        VERR_VMX_VMCS_FIELD_CACHE_INVALID);

    if (pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_USE_SECONDARY_CTLS)
    {
        rc = VMXReadVmcs32(VMX_VMCS32_CTRL_PROC_EXEC2, &u32Val);
        AssertRCReturn(rc, rc);
        AssertMsgReturnStmt(pVmcsInfo->u32ProcCtls2 == u32Val,
                            ("Cache=%#RX32 VMCS=%#RX32\n", pVmcsInfo->u32ProcCtls2, u32Val),
                            pVCpu->hm.s.u32HMError = VMX_VCI_CTRL_PROC_EXEC2,
                            VERR_VMX_VMCS_FIELD_CACHE_INVALID);
    }

    rc = VMXReadVmcs32(VMX_VMCS32_CTRL_EXCEPTION_BITMAP, &u32Val);
    AssertRCReturn(rc, rc);
    AssertMsgReturnStmt(pVmcsInfo->u32XcptBitmap == u32Val,
                        ("Cache=%#RX32 VMCS=%#RX32\n", pVmcsInfo->u32XcptBitmap, u32Val),
                        pVCpu->hm.s.u32HMError = VMX_VCI_CTRL_XCPT_BITMAP,
                        VERR_VMX_VMCS_FIELD_CACHE_INVALID);

    uint64_t u64Val;
    rc = VMXReadVmcs64(VMX_VMCS64_CTRL_TSC_OFFSET_FULL, &u64Val);
    AssertRCReturn(rc, rc);
    AssertMsgReturnStmt(pVmcsInfo->u64TscOffset == u64Val,
                        ("Cache=%#RX64 VMCS=%#RX64\n", pVmcsInfo->u64TscOffset, u64Val),
                        pVCpu->hm.s.u32HMError = VMX_VCI_CTRL_TSC_OFFSET,
                        VERR_VMX_VMCS_FIELD_CACHE_INVALID);

    return VINF_SUCCESS;
}


#ifdef VBOX_STRICT
/**
 * Verifies that our cached host EFER MSR value has not changed since we cached it.
 *
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pVmcsInfo   The VMCS info. object.
 */
static void hmR0VmxCheckHostEferMsr(PCVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo)
{
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    if (pVmcsInfo->u32ExitCtls & VMX_EXIT_CTLS_LOAD_EFER_MSR)
    {
        uint64_t const uHostEferMsr      = ASMRdMsr(MSR_K6_EFER);
        uint64_t const uHostEferMsrCache = pVCpu->CTX_SUFF(pVM)->hm.s.vmx.u64HostMsrEfer;
        uint64_t       uVmcsEferMsrVmcs;
        int rc = VMXReadVmcs64(VMX_VMCS64_HOST_EFER_FULL, &uVmcsEferMsrVmcs);
        AssertRC(rc);

        AssertMsgReturnVoid(uHostEferMsr == uVmcsEferMsrVmcs,
                            ("EFER Host/VMCS mismatch! host=%#RX64 vmcs=%#RX64\n", uHostEferMsr, uVmcsEferMsrVmcs));
        AssertMsgReturnVoid(uHostEferMsr == uHostEferMsrCache,
                            ("EFER Host/Cache mismatch! host=%#RX64 cache=%#RX64\n", uHostEferMsr, uHostEferMsrCache));
    }
}


/**
 * Verifies whether the guest/host MSR pairs in the auto-load/store area in the
 * VMCS are correct.
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmcsInfo       The VMCS info. object.
 * @param   fIsNstGstVmcs   Whether this is a nested-guest VMCS.
 */
static void hmR0VmxCheckAutoLoadStoreMsrs(PVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo, bool fIsNstGstVmcs)
{
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    /* Read the various MSR-area counts from the VMCS. */
    uint32_t cEntryLoadMsrs;
    uint32_t cExitStoreMsrs;
    uint32_t cExitLoadMsrs;
    int rc = VMXReadVmcs32(VMX_VMCS32_CTRL_ENTRY_MSR_LOAD_COUNT, &cEntryLoadMsrs);  AssertRC(rc);
    rc     = VMXReadVmcs32(VMX_VMCS32_CTRL_EXIT_MSR_STORE_COUNT, &cExitStoreMsrs);  AssertRC(rc);
    rc     = VMXReadVmcs32(VMX_VMCS32_CTRL_EXIT_MSR_LOAD_COUNT,  &cExitLoadMsrs);   AssertRC(rc);

    /* Verify all the MSR counts are the same. */
    Assert(cEntryLoadMsrs == cExitStoreMsrs);
    Assert(cExitStoreMsrs == cExitLoadMsrs);
    uint32_t const cMsrs = cExitLoadMsrs;

    /* Verify the MSR counts do not exceed the maximum count supported by the hardware. */
    Assert(cMsrs < VMX_MISC_MAX_MSRS(pVCpu->CTX_SUFF(pVM)->hm.s.vmx.Msrs.u64Misc));

    /* Verify the MSR counts are within the allocated page size. */
    Assert(sizeof(VMXAUTOMSR) * cMsrs <= X86_PAGE_4K_SIZE);

    /* Verify the relevant contents of the MSR areas match. */
    PCVMXAUTOMSR pGuestMsrLoad  = (PCVMXAUTOMSR)pVmcsInfo->pvGuestMsrLoad;
    PCVMXAUTOMSR pGuestMsrStore = (PCVMXAUTOMSR)pVmcsInfo->pvGuestMsrStore;
    PCVMXAUTOMSR pHostMsrLoad   = (PCVMXAUTOMSR)pVmcsInfo->pvHostMsrLoad;
    bool const   fSeparateExitMsrStorePage = hmR0VmxIsSeparateExitMsrStoreAreaVmcs(pVmcsInfo);
    for (uint32_t i = 0; i < cMsrs; i++)
    {
        /* Verify that the MSRs are paired properly and that the host MSR has the correct value. */
        if (fSeparateExitMsrStorePage)
        {
            AssertMsgReturnVoid(pGuestMsrLoad->u32Msr == pGuestMsrStore->u32Msr,
                                ("GuestMsrLoad=%#RX32 GuestMsrStore=%#RX32 cMsrs=%u\n",
                                 pGuestMsrLoad->u32Msr, pGuestMsrStore->u32Msr, cMsrs));
        }

        AssertMsgReturnVoid(pHostMsrLoad->u32Msr == pGuestMsrLoad->u32Msr,
                            ("HostMsrLoad=%#RX32 GuestMsrLoad=%#RX32 cMsrs=%u\n",
                             pHostMsrLoad->u32Msr, pGuestMsrLoad->u32Msr, cMsrs));

        uint64_t const u64Msr = ASMRdMsr(pHostMsrLoad->u32Msr);
        AssertMsgReturnVoid(pHostMsrLoad->u64Value == u64Msr,
                            ("u32Msr=%#RX32 VMCS Value=%#RX64 ASMRdMsr=%#RX64 cMsrs=%u\n",
                             pHostMsrLoad->u32Msr, pHostMsrLoad->u64Value, u64Msr, cMsrs));

        /* Verify that cached host EFER MSR matches what's loaded the CPU. */
        bool const fIsEferMsr = RT_BOOL(pHostMsrLoad->u32Msr == MSR_K6_EFER);
        if (fIsEferMsr)
        {
            AssertMsgReturnVoid(u64Msr == pVCpu->CTX_SUFF(pVM)->hm.s.vmx.u64HostMsrEfer,
                                ("Cached=%#RX64 ASMRdMsr=%#RX64 cMsrs=%u\n",
                                 pVCpu->CTX_SUFF(pVM)->hm.s.vmx.u64HostMsrEfer, u64Msr, cMsrs));
        }

        /* Verify that the accesses are as expected in the MSR bitmap for auto-load/store MSRs. */
        if (pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_USE_MSR_BITMAPS)
        {
            uint32_t const fMsrpm = CPUMGetVmxMsrPermission(pVmcsInfo->pvMsrBitmap, pGuestMsrLoad->u32Msr);
            if (fIsEferMsr)
            {
                AssertMsgReturnVoid((fMsrpm & VMXMSRPM_EXIT_RD), ("Passthru read for EFER MSR!?\n"));
                AssertMsgReturnVoid((fMsrpm & VMXMSRPM_EXIT_WR), ("Passthru write for EFER MSR!?\n"));
            }
            else
            {
                if (!fIsNstGstVmcs)
                {
                    AssertMsgReturnVoid((fMsrpm & VMXMSRPM_ALLOW_RD_WR) == VMXMSRPM_ALLOW_RD_WR,
                                        ("u32Msr=%#RX32 cMsrs=%u No passthru read/write!\n", pGuestMsrLoad->u32Msr, cMsrs));
                }
                else
                {
                    /*
                     * A nested-guest VMCS must -also- allow read/write passthrough for the MSR for us to
                     * execute a nested-guest with MSR passthrough.
                     *
                     * Check if the nested-guest MSR bitmap allows passthrough, and if so, assert that we
                     * allow passthrough too.
                     */
                    void const *pvMsrBitmapNstGst = pVCpu->cpum.GstCtx.hwvirt.vmx.CTX_SUFF(pvMsrBitmap);
                    Assert(pvMsrBitmapNstGst);
                    uint32_t const fMsrpmNstGst = CPUMGetVmxMsrPermission(pvMsrBitmapNstGst, pGuestMsrLoad->u32Msr);
                    AssertMsgReturnVoid(fMsrpm == fMsrpmNstGst,
                                        ("u32Msr=%#RX32 cMsrs=%u Permission mismatch fMsrpm=%#x fMsrpmNstGst=%#x!\n",
                                         pGuestMsrLoad->u32Msr, cMsrs, fMsrpm, fMsrpmNstGst));
                }
            }
        }

        /* Move to the next MSR. */
        pHostMsrLoad++;
        pGuestMsrLoad++;
        pGuestMsrStore++;
    }
}
#endif /* VBOX_STRICT */


/**
 * Flushes the TLB using EPT.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure of the calling
 *                          EMT.  Can be NULL depending on @a enmTlbFlush.
 * @param   pVmcsInfo       The VMCS info. object. Can be NULL depending on @a
 *                          enmTlbFlush.
 * @param   enmTlbFlush     Type of flush.
 *
 * @remarks Caller is responsible for making sure this function is called only
 *          when NestedPaging is supported and providing @a enmTlbFlush that is
 *          supported by the CPU.
 * @remarks Can be called with interrupts disabled.
 */
static void hmR0VmxFlushEpt(PVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo, VMXTLBFLUSHEPT enmTlbFlush)
{
    uint64_t au64Descriptor[2];
    if (enmTlbFlush == VMXTLBFLUSHEPT_ALL_CONTEXTS)
        au64Descriptor[0] = 0;
    else
    {
        Assert(pVCpu);
        Assert(pVmcsInfo);
        au64Descriptor[0] = pVmcsInfo->HCPhysEPTP;
    }
    au64Descriptor[1] = 0;                       /* MBZ. Intel spec. 33.3 "VMX Instructions" */

    int rc = VMXR0InvEPT(enmTlbFlush, &au64Descriptor[0]);
    AssertMsg(rc == VINF_SUCCESS, ("VMXR0InvEPT %#x %#RHp failed. rc=%Rrc\n", enmTlbFlush, au64Descriptor[0], rc));

    if (   RT_SUCCESS(rc)
        && pVCpu)
        STAM_COUNTER_INC(&pVCpu->hm.s.StatFlushNestedPaging);
}


/**
 * Flushes the TLB using VPID.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure of the calling
 *                          EMT.  Can be NULL depending on @a enmTlbFlush.
 * @param   enmTlbFlush     Type of flush.
 * @param   GCPtr           Virtual address of the page to flush (can be 0 depending
 *                          on @a enmTlbFlush).
 *
 * @remarks Can be called with interrupts disabled.
 */
static void hmR0VmxFlushVpid(PVMCPU pVCpu, VMXTLBFLUSHVPID enmTlbFlush, RTGCPTR GCPtr)
{
    Assert(pVCpu->CTX_SUFF(pVM)->hm.s.vmx.fVpid);

    uint64_t au64Descriptor[2];
    if (enmTlbFlush == VMXTLBFLUSHVPID_ALL_CONTEXTS)
    {
        au64Descriptor[0] = 0;
        au64Descriptor[1] = 0;
    }
    else
    {
        AssertPtr(pVCpu);
        AssertMsg(pVCpu->hm.s.uCurrentAsid != 0, ("VMXR0InvVPID: invalid ASID %lu\n", pVCpu->hm.s.uCurrentAsid));
        AssertMsg(pVCpu->hm.s.uCurrentAsid <= UINT16_MAX, ("VMXR0InvVPID: invalid ASID %lu\n", pVCpu->hm.s.uCurrentAsid));
        au64Descriptor[0] = pVCpu->hm.s.uCurrentAsid;
        au64Descriptor[1] = GCPtr;
    }

    int rc = VMXR0InvVPID(enmTlbFlush, &au64Descriptor[0]);
    AssertMsg(rc == VINF_SUCCESS,
              ("VMXR0InvVPID %#x %u %RGv failed with %Rrc\n", enmTlbFlush, pVCpu ? pVCpu->hm.s.uCurrentAsid : 0, GCPtr, rc));

    if (   RT_SUCCESS(rc)
        && pVCpu)
        STAM_COUNTER_INC(&pVCpu->hm.s.StatFlushAsid);
    NOREF(rc);
}


/**
 * Invalidates a guest page by guest virtual address. Only relevant for EPT/VPID,
 * otherwise there is nothing really to invalidate.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   GCVirt      Guest virtual address of the page to invalidate.
 */
VMMR0DECL(int) VMXR0InvalidatePage(PVMCPU pVCpu, RTGCPTR GCVirt)
{
    AssertPtr(pVCpu);
    LogFlowFunc(("pVCpu=%p GCVirt=%RGv\n", pVCpu, GCVirt));

    if (!VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_TLB_FLUSH))
    {
        /*
         * We must invalidate the guest TLB entry in either case, we cannot ignore it even for
         * the EPT case. See @bugref{6043} and @bugref{6177}.
         *
         * Set the VMCPU_FF_TLB_FLUSH force flag and flush before VM-entry in hmR0VmxFlushTLB*()
         * as this function maybe called in a loop with individual addresses.
         */
        PVM pVM = pVCpu->CTX_SUFF(pVM);
        if (pVM->hm.s.vmx.fVpid)
        {
            bool fVpidFlush = RT_BOOL(pVM->hm.s.vmx.Msrs.u64EptVpidCaps & MSR_IA32_VMX_EPT_VPID_CAP_INVVPID_INDIV_ADDR);

#if HC_ARCH_BITS == 32 && defined(VBOX_ENABLE_64_BITS_GUESTS)
            /*
             * Workaround Erratum BV75, AAJ159 and others that affect several Intel CPUs
             * where executing INVVPID outside 64-bit mode does not flush translations of
             * 64-bit linear addresses, see @bugref{6208#c72}.
             */
            if (RT_HI_U32(GCVirt))
                fVpidFlush = false;
#endif

            if (fVpidFlush)
            {
                hmR0VmxFlushVpid(pVCpu, VMXTLBFLUSHVPID_INDIV_ADDR, GCVirt);
                STAM_COUNTER_INC(&pVCpu->hm.s.StatFlushTlbInvlpgVirt);
            }
            else
                VMCPU_FF_SET(pVCpu, VMCPU_FF_TLB_FLUSH);
        }
        else if (pVM->hm.s.fNestedPaging)
            VMCPU_FF_SET(pVCpu, VMCPU_FF_TLB_FLUSH);
    }

    return VINF_SUCCESS;
}


/**
 * Dummy placeholder for tagged-TLB flush handling before VM-entry. Used in the
 * case where neither EPT nor VPID is supported by the CPU.
 *
 * @param   pHostCpu    The HM physical-CPU structure.
 * @param   pVCpu       The cross context virtual CPU structure.
 *
 * @remarks Called with interrupts disabled.
 */
static void hmR0VmxFlushTaggedTlbNone(PHMPHYSCPU pHostCpu, PVMCPU pVCpu)
{
    AssertPtr(pVCpu);
    AssertPtr(pHostCpu);

    VMCPU_FF_CLEAR(pVCpu, VMCPU_FF_TLB_FLUSH);

    Assert(pHostCpu->idCpu != NIL_RTCPUID);
    pVCpu->hm.s.idLastCpu      = pHostCpu->idCpu;
    pVCpu->hm.s.cTlbFlushes    = pHostCpu->cTlbFlushes;
    pVCpu->hm.s.fForceTLBFlush = false;
    return;
}


/**
 * Flushes the tagged-TLB entries for EPT+VPID CPUs as necessary.
 *
 * @param   pHostCpu    The HM physical-CPU structure.
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pVmcsInfo   The VMCS info. object.
 *
 * @remarks  All references to "ASID" in this function pertains to "VPID" in Intel's
 *           nomenclature. The reason is, to avoid confusion in compare statements
 *           since the host-CPU copies are named "ASID".
 *
 * @remarks  Called with interrupts disabled.
 */
static void hmR0VmxFlushTaggedTlbBoth(PHMPHYSCPU pHostCpu, PVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo)
{
#ifdef VBOX_WITH_STATISTICS
    bool fTlbFlushed = false;
# define HMVMX_SET_TAGGED_TLB_FLUSHED()       do { fTlbFlushed = true; } while (0)
# define HMVMX_UPDATE_FLUSH_SKIPPED_STAT()    do { \
                                                if (!fTlbFlushed) \
                                                    STAM_COUNTER_INC(&pVCpu->hm.s.StatNoFlushTlbWorldSwitch); \
                                              } while (0)
#else
# define HMVMX_SET_TAGGED_TLB_FLUSHED()       do { } while (0)
# define HMVMX_UPDATE_FLUSH_SKIPPED_STAT()    do { } while (0)
#endif

    AssertPtr(pVCpu);
    AssertPtr(pHostCpu);
    Assert(pHostCpu->idCpu != NIL_RTCPUID);

    PVM pVM = pVCpu->CTX_SUFF(pVM);
    AssertMsg(pVM->hm.s.fNestedPaging && pVM->hm.s.vmx.fVpid,
              ("hmR0VmxFlushTaggedTlbBoth cannot be invoked unless NestedPaging & VPID are enabled."
               "fNestedPaging=%RTbool fVpid=%RTbool", pVM->hm.s.fNestedPaging, pVM->hm.s.vmx.fVpid));

    /*
     * Force a TLB flush for the first world-switch if the current CPU differs from the one we
     * ran on last. If the TLB flush count changed, another VM (VCPU rather) has hit the ASID
     * limit while flushing the TLB or the host CPU is online after a suspend/resume, so we
     * cannot reuse the current ASID anymore.
     */
    if (   pVCpu->hm.s.idLastCpu   != pHostCpu->idCpu
        || pVCpu->hm.s.cTlbFlushes != pHostCpu->cTlbFlushes)
    {
        ++pHostCpu->uCurrentAsid;
        if (pHostCpu->uCurrentAsid >= pVM->hm.s.uMaxAsid)
        {
            pHostCpu->uCurrentAsid = 1;            /* Wraparound to 1; host uses 0. */
            pHostCpu->cTlbFlushes++;               /* All VCPUs that run on this host CPU must use a new VPID. */
            pHostCpu->fFlushAsidBeforeUse = true;  /* All VCPUs that run on this host CPU must flush their new VPID before use. */
        }

        pVCpu->hm.s.uCurrentAsid = pHostCpu->uCurrentAsid;
        pVCpu->hm.s.idLastCpu    = pHostCpu->idCpu;
        pVCpu->hm.s.cTlbFlushes  = pHostCpu->cTlbFlushes;

        /*
         * Flush by EPT when we get rescheduled to a new host CPU to ensure EPT-only tagged mappings are also
         * invalidated. We don't need to flush-by-VPID here as flushing by EPT covers it. See @bugref{6568}.
         */
        hmR0VmxFlushEpt(pVCpu, pVmcsInfo, pVM->hm.s.vmx.enmTlbFlushEpt);
        STAM_COUNTER_INC(&pVCpu->hm.s.StatFlushTlbWorldSwitch);
        HMVMX_SET_TAGGED_TLB_FLUSHED();
        VMCPU_FF_CLEAR(pVCpu, VMCPU_FF_TLB_FLUSH);
    }
    else if (VMCPU_FF_TEST_AND_CLEAR(pVCpu, VMCPU_FF_TLB_FLUSH))    /* Check for explicit TLB flushes. */
    {
        /*
         * Changes to the EPT paging structure by VMM requires flushing-by-EPT as the CPU
         * creates guest-physical (ie. only EPT-tagged) mappings while traversing the EPT
         * tables when EPT is in use. Flushing-by-VPID will only flush linear (only
         * VPID-tagged) and combined (EPT+VPID tagged) mappings but not guest-physical
         * mappings, see @bugref{6568}.
         *
         * See Intel spec. 28.3.2 "Creating and Using Cached Translation Information".
         */
        hmR0VmxFlushEpt(pVCpu, pVmcsInfo, pVM->hm.s.vmx.enmTlbFlushEpt);
        STAM_COUNTER_INC(&pVCpu->hm.s.StatFlushTlb);
        HMVMX_SET_TAGGED_TLB_FLUSHED();
    }

    pVCpu->hm.s.fForceTLBFlush = false;
    HMVMX_UPDATE_FLUSH_SKIPPED_STAT();

    Assert(pVCpu->hm.s.idLastCpu == pHostCpu->idCpu);
    Assert(pVCpu->hm.s.cTlbFlushes == pHostCpu->cTlbFlushes);
    AssertMsg(pVCpu->hm.s.cTlbFlushes == pHostCpu->cTlbFlushes,
              ("Flush count mismatch for cpu %d (%u vs %u)\n", pHostCpu->idCpu, pVCpu->hm.s.cTlbFlushes, pHostCpu->cTlbFlushes));
    AssertMsg(pHostCpu->uCurrentAsid >= 1 && pHostCpu->uCurrentAsid < pVM->hm.s.uMaxAsid,
              ("Cpu[%u] uCurrentAsid=%u cTlbFlushes=%u pVCpu->idLastCpu=%u pVCpu->cTlbFlushes=%u\n", pHostCpu->idCpu,
               pHostCpu->uCurrentAsid, pHostCpu->cTlbFlushes, pVCpu->hm.s.idLastCpu, pVCpu->hm.s.cTlbFlushes));
    AssertMsg(pVCpu->hm.s.uCurrentAsid >= 1 && pVCpu->hm.s.uCurrentAsid < pVM->hm.s.uMaxAsid,
              ("Cpu[%u] pVCpu->uCurrentAsid=%u\n", pHostCpu->idCpu, pVCpu->hm.s.uCurrentAsid));

    /* Update VMCS with the VPID. */
    int rc  = VMXWriteVmcs32(VMX_VMCS16_VPID, pVCpu->hm.s.uCurrentAsid);
    AssertRC(rc);

#undef HMVMX_SET_TAGGED_TLB_FLUSHED
}


/**
 * Flushes the tagged-TLB entries for EPT CPUs as necessary.
 *
 * @param   pHostCpu    The HM physical-CPU structure.
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pVmcsInfo   The VMCS info. object.
 *
 * @remarks Called with interrupts disabled.
 */
static void hmR0VmxFlushTaggedTlbEpt(PHMPHYSCPU pHostCpu, PVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo)
{
    AssertPtr(pVCpu);
    AssertPtr(pHostCpu);
    Assert(pHostCpu->idCpu != NIL_RTCPUID);
    AssertMsg(pVCpu->CTX_SUFF(pVM)->hm.s.fNestedPaging, ("hmR0VmxFlushTaggedTlbEpt cannot be invoked without NestedPaging."));
    AssertMsg(!pVCpu->CTX_SUFF(pVM)->hm.s.vmx.fVpid, ("hmR0VmxFlushTaggedTlbEpt cannot be invoked with VPID."));

    /*
     * Force a TLB flush for the first world-switch if the current CPU differs from the one we ran on last.
     * A change in the TLB flush count implies the host CPU is online after a suspend/resume.
     */
    if (   pVCpu->hm.s.idLastCpu   != pHostCpu->idCpu
        || pVCpu->hm.s.cTlbFlushes != pHostCpu->cTlbFlushes)
    {
        pVCpu->hm.s.fForceTLBFlush = true;
        STAM_COUNTER_INC(&pVCpu->hm.s.StatFlushTlbWorldSwitch);
    }

    /* Check for explicit TLB flushes. */
    if (VMCPU_FF_TEST_AND_CLEAR(pVCpu, VMCPU_FF_TLB_FLUSH))
    {
        pVCpu->hm.s.fForceTLBFlush = true;
        STAM_COUNTER_INC(&pVCpu->hm.s.StatFlushTlb);
    }

    pVCpu->hm.s.idLastCpu   = pHostCpu->idCpu;
    pVCpu->hm.s.cTlbFlushes = pHostCpu->cTlbFlushes;

    if (pVCpu->hm.s.fForceTLBFlush)
    {
        hmR0VmxFlushEpt(pVCpu, pVmcsInfo, pVCpu->CTX_SUFF(pVM)->hm.s.vmx.enmTlbFlushEpt);
        pVCpu->hm.s.fForceTLBFlush = false;
    }
}


/**
 * Flushes the tagged-TLB entries for VPID CPUs as necessary.
 *
 * @param   pHostCpu    The HM physical-CPU structure.
 * @param   pVCpu       The cross context virtual CPU structure.
 *
 * @remarks Called with interrupts disabled.
 */
static void hmR0VmxFlushTaggedTlbVpid(PHMPHYSCPU pHostCpu, PVMCPU pVCpu)
{
    AssertPtr(pVCpu);
    AssertPtr(pHostCpu);
    Assert(pHostCpu->idCpu != NIL_RTCPUID);
    AssertMsg(pVCpu->CTX_SUFF(pVM)->hm.s.vmx.fVpid, ("hmR0VmxFlushTlbVpid cannot be invoked without VPID."));
    AssertMsg(!pVCpu->CTX_SUFF(pVM)->hm.s.fNestedPaging, ("hmR0VmxFlushTlbVpid cannot be invoked with NestedPaging"));

    /*
     * Force a TLB flush for the first world switch if the current CPU differs from the one we
     * ran on last. If the TLB flush count changed, another VM (VCPU rather) has hit the ASID
     * limit while flushing the TLB or the host CPU is online after a suspend/resume, so we
     * cannot reuse the current ASID anymore.
     */
    if (   pVCpu->hm.s.idLastCpu   != pHostCpu->idCpu
        || pVCpu->hm.s.cTlbFlushes != pHostCpu->cTlbFlushes)
    {
        pVCpu->hm.s.fForceTLBFlush = true;
        STAM_COUNTER_INC(&pVCpu->hm.s.StatFlushTlbWorldSwitch);
    }

    /* Check for explicit TLB flushes. */
    if (VMCPU_FF_TEST_AND_CLEAR(pVCpu, VMCPU_FF_TLB_FLUSH))
    {
        /*
         * If we ever support VPID flush combinations other than ALL or SINGLE-context (see
         * hmR0VmxSetupTaggedTlb()) we would need to explicitly flush in this case (add an
         * fExplicitFlush = true here and change the pHostCpu->fFlushAsidBeforeUse check below to
         * include fExplicitFlush's too) - an obscure corner case.
         */
        pVCpu->hm.s.fForceTLBFlush = true;
        STAM_COUNTER_INC(&pVCpu->hm.s.StatFlushTlb);
    }

    PVM pVM = pVCpu->CTX_SUFF(pVM);
    pVCpu->hm.s.idLastCpu = pHostCpu->idCpu;
    if (pVCpu->hm.s.fForceTLBFlush)
    {
        ++pHostCpu->uCurrentAsid;
        if (pHostCpu->uCurrentAsid >= pVM->hm.s.uMaxAsid)
        {
            pHostCpu->uCurrentAsid        = 1;     /* Wraparound to 1; host uses 0 */
            pHostCpu->cTlbFlushes++;               /* All VCPUs that run on this host CPU must use a new VPID. */
            pHostCpu->fFlushAsidBeforeUse = true;  /* All VCPUs that run on this host CPU must flush their new VPID before use. */
        }

        pVCpu->hm.s.fForceTLBFlush = false;
        pVCpu->hm.s.cTlbFlushes    = pHostCpu->cTlbFlushes;
        pVCpu->hm.s.uCurrentAsid   = pHostCpu->uCurrentAsid;
        if (pHostCpu->fFlushAsidBeforeUse)
        {
            if (pVM->hm.s.vmx.enmTlbFlushVpid == VMXTLBFLUSHVPID_SINGLE_CONTEXT)
                hmR0VmxFlushVpid(pVCpu, VMXTLBFLUSHVPID_SINGLE_CONTEXT, 0 /* GCPtr */);
            else if (pVM->hm.s.vmx.enmTlbFlushVpid == VMXTLBFLUSHVPID_ALL_CONTEXTS)
            {
                hmR0VmxFlushVpid(pVCpu, VMXTLBFLUSHVPID_ALL_CONTEXTS, 0 /* GCPtr */);
                pHostCpu->fFlushAsidBeforeUse = false;
            }
            else
            {
                /* hmR0VmxSetupTaggedTlb() ensures we never get here. Paranoia. */
                AssertMsgFailed(("Unsupported VPID-flush context type.\n"));
            }
        }
    }

    AssertMsg(pVCpu->hm.s.cTlbFlushes == pHostCpu->cTlbFlushes,
              ("Flush count mismatch for cpu %d (%u vs %u)\n", pHostCpu->idCpu, pVCpu->hm.s.cTlbFlushes, pHostCpu->cTlbFlushes));
    AssertMsg(pHostCpu->uCurrentAsid >= 1 && pHostCpu->uCurrentAsid < pVM->hm.s.uMaxAsid,
              ("Cpu[%u] uCurrentAsid=%u cTlbFlushes=%u pVCpu->idLastCpu=%u pVCpu->cTlbFlushes=%u\n", pHostCpu->idCpu,
               pHostCpu->uCurrentAsid, pHostCpu->cTlbFlushes, pVCpu->hm.s.idLastCpu, pVCpu->hm.s.cTlbFlushes));
    AssertMsg(pVCpu->hm.s.uCurrentAsid >= 1 && pVCpu->hm.s.uCurrentAsid < pVM->hm.s.uMaxAsid,
              ("Cpu[%u] pVCpu->uCurrentAsid=%u\n", pHostCpu->idCpu, pVCpu->hm.s.uCurrentAsid));

    int rc  = VMXWriteVmcs32(VMX_VMCS16_VPID, pVCpu->hm.s.uCurrentAsid);
    AssertRC(rc);
}


/**
 * Flushes the guest TLB entry based on CPU capabilities.
 *
 * @param   pHostCpu    The HM physical-CPU structure.
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pVmcsInfo   The VMCS info. object.
 *
 * @remarks Called with interrupts disabled.
 */
static void hmR0VmxFlushTaggedTlb(PHMPHYSCPU pHostCpu, PVMCPU pVCpu, PVMXVMCSINFO pVmcsInfo)
{
#ifdef HMVMX_ALWAYS_FLUSH_TLB
    VMCPU_FF_SET(pVCpu, VMCPU_FF_TLB_FLUSH);
#endif
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    switch (pVM->hm.s.vmx.enmTlbFlushType)
    {
        case VMXTLBFLUSHTYPE_EPT_VPID: hmR0VmxFlushTaggedTlbBoth(pHostCpu, pVCpu, pVmcsInfo); break;
        case VMXTLBFLUSHTYPE_EPT:      hmR0VmxFlushTaggedTlbEpt(pHostCpu, pVCpu, pVmcsInfo);  break;
        case VMXTLBFLUSHTYPE_VPID:     hmR0VmxFlushTaggedTlbVpid(pHostCpu, pVCpu);            break;
        case VMXTLBFLUSHTYPE_NONE:     hmR0VmxFlushTaggedTlbNone(pHostCpu, pVCpu);            break;
        default:
            AssertMsgFailed(("Invalid flush-tag function identifier\n"));
            break;
    }
    /* Don't assert that VMCPU_FF_TLB_FLUSH should no longer be pending. It can be set by other EMTs. */
}


/**
 * Sets up the appropriate tagged TLB-flush level and handler for flushing guest
 * TLB entries from the host TLB before VM-entry.
 *
 * @returns VBox status code.
 * @param   pVM         The cross context VM structure.
 */
static int hmR0VmxSetupTaggedTlb(PVM pVM)
{
    /*
     * Determine optimal flush type for nested paging.
     * We cannot ignore EPT if no suitable flush-types is supported by the CPU as we've already setup
     * unrestricted guest execution (see hmR3InitFinalizeR0()).
     */
    if (pVM->hm.s.fNestedPaging)
    {
        if (pVM->hm.s.vmx.Msrs.u64EptVpidCaps & MSR_IA32_VMX_EPT_VPID_CAP_INVEPT)
        {
            if (pVM->hm.s.vmx.Msrs.u64EptVpidCaps & MSR_IA32_VMX_EPT_VPID_CAP_INVEPT_SINGLE_CONTEXT)
                pVM->hm.s.vmx.enmTlbFlushEpt = VMXTLBFLUSHEPT_SINGLE_CONTEXT;
            else if (pVM->hm.s.vmx.Msrs.u64EptVpidCaps & MSR_IA32_VMX_EPT_VPID_CAP_INVEPT_ALL_CONTEXTS)
                pVM->hm.s.vmx.enmTlbFlushEpt = VMXTLBFLUSHEPT_ALL_CONTEXTS;
            else
            {
                /* Shouldn't happen. EPT is supported but no suitable flush-types supported. */
                pVM->hm.s.vmx.enmTlbFlushEpt = VMXTLBFLUSHEPT_NOT_SUPPORTED;
                pVM->aCpus[0].hm.s.u32HMError = VMX_UFC_EPT_FLUSH_TYPE_UNSUPPORTED;
                return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
            }

            /* Make sure the write-back cacheable memory type for EPT is supported. */
            if (RT_UNLIKELY(!(pVM->hm.s.vmx.Msrs.u64EptVpidCaps & MSR_IA32_VMX_EPT_VPID_CAP_EMT_WB)))
            {
                pVM->hm.s.vmx.enmTlbFlushEpt = VMXTLBFLUSHEPT_NOT_SUPPORTED;
                pVM->aCpus[0].hm.s.u32HMError = VMX_UFC_EPT_MEM_TYPE_NOT_WB;
                return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
            }

            /* EPT requires a page-walk length of 4. */
            if (RT_UNLIKELY(!(pVM->hm.s.vmx.Msrs.u64EptVpidCaps & MSR_IA32_VMX_EPT_VPID_CAP_PAGE_WALK_LENGTH_4)))
            {
                pVM->hm.s.vmx.enmTlbFlushEpt = VMXTLBFLUSHEPT_NOT_SUPPORTED;
                pVM->aCpus[0].hm.s.u32HMError = VMX_UFC_EPT_PAGE_WALK_LENGTH_UNSUPPORTED;
                return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
            }
        }
        else
        {
            /* Shouldn't happen. EPT is supported but INVEPT instruction is not supported. */
            pVM->hm.s.vmx.enmTlbFlushEpt = VMXTLBFLUSHEPT_NOT_SUPPORTED;
            pVM->aCpus[0].hm.s.u32HMError = VMX_UFC_EPT_INVEPT_UNAVAILABLE;
            return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
        }
    }

    /*
     * Determine optimal flush type for VPID.
     */
    if (pVM->hm.s.vmx.fVpid)
    {
        if (pVM->hm.s.vmx.Msrs.u64EptVpidCaps & MSR_IA32_VMX_EPT_VPID_CAP_INVVPID)
        {
            if (pVM->hm.s.vmx.Msrs.u64EptVpidCaps & MSR_IA32_VMX_EPT_VPID_CAP_INVVPID_SINGLE_CONTEXT)
                pVM->hm.s.vmx.enmTlbFlushVpid = VMXTLBFLUSHVPID_SINGLE_CONTEXT;
            else if (pVM->hm.s.vmx.Msrs.u64EptVpidCaps & MSR_IA32_VMX_EPT_VPID_CAP_INVVPID_ALL_CONTEXTS)
                pVM->hm.s.vmx.enmTlbFlushVpid = VMXTLBFLUSHVPID_ALL_CONTEXTS;
            else
            {
                /* Neither SINGLE nor ALL-context flush types for VPID is supported by the CPU. Ignore VPID capability. */
                if (pVM->hm.s.vmx.Msrs.u64EptVpidCaps & MSR_IA32_VMX_EPT_VPID_CAP_INVVPID_INDIV_ADDR)
                    LogRelFunc(("Only INDIV_ADDR supported. Ignoring VPID.\n"));
                if (pVM->hm.s.vmx.Msrs.u64EptVpidCaps & MSR_IA32_VMX_EPT_VPID_CAP_INVVPID_SINGLE_CONTEXT_RETAIN_GLOBALS)
                    LogRelFunc(("Only SINGLE_CONTEXT_RETAIN_GLOBALS supported. Ignoring VPID.\n"));
                pVM->hm.s.vmx.enmTlbFlushVpid = VMXTLBFLUSHVPID_NOT_SUPPORTED;
                pVM->hm.s.vmx.fVpid = false;
            }
        }
        else
        {
            /*  Shouldn't happen. VPID is supported but INVVPID is not supported by the CPU. Ignore VPID capability. */
            Log4Func(("VPID supported without INVEPT support. Ignoring VPID.\n"));
            pVM->hm.s.vmx.enmTlbFlushVpid = VMXTLBFLUSHVPID_NOT_SUPPORTED;
            pVM->hm.s.vmx.fVpid = false;
        }
    }

    /*
     * Setup the handler for flushing tagged-TLBs.
     */
    if (pVM->hm.s.fNestedPaging && pVM->hm.s.vmx.fVpid)
        pVM->hm.s.vmx.enmTlbFlushType = VMXTLBFLUSHTYPE_EPT_VPID;
    else if (pVM->hm.s.fNestedPaging)
        pVM->hm.s.vmx.enmTlbFlushType = VMXTLBFLUSHTYPE_EPT;
    else if (pVM->hm.s.vmx.fVpid)
        pVM->hm.s.vmx.enmTlbFlushType = VMXTLBFLUSHTYPE_VPID;
    else
        pVM->hm.s.vmx.enmTlbFlushType = VMXTLBFLUSHTYPE_NONE;
    return VINF_SUCCESS;
}


/**
 * Sets up the virtual-APIC page address for the VMCS.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pVmcsInfo   The VMCS info. object.
 */
DECLINLINE(int) hmR0VmxSetupVmcsVirtApicAddr(PVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo)
{
    NOREF(pVCpu); /* Used implicitly by VMXWriteVmcs64 on 32-bit hosts. */
    RTHCPHYS const HCPhysVirtApic = pVmcsInfo->HCPhysVirtApic;
    Assert(HCPhysVirtApic != NIL_RTHCPHYS);
    Assert(!(HCPhysVirtApic & 0xfff));                       /* Bits 11:0 MBZ. */
    return VMXWriteVmcs64(VMX_VMCS64_CTRL_VIRT_APIC_PAGEADDR_FULL, HCPhysVirtApic);
}


/**
 * Sets up the MSR-bitmap address for the VMCS.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pVmcsInfo   The VMCS info. object.
 */
DECLINLINE(int) hmR0VmxSetupVmcsMsrBitmapAddr(PVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo)
{
    NOREF(pVCpu); /* Used implicitly by VMXWriteVmcs64 on 32-bit hosts. */
    RTHCPHYS const HCPhysMsrBitmap = pVmcsInfo->HCPhysMsrBitmap;
    Assert(HCPhysMsrBitmap != NIL_RTHCPHYS);
    Assert(!(HCPhysMsrBitmap & 0xfff));                      /* Bits 11:0 MBZ. */
    return VMXWriteVmcs64(VMX_VMCS64_CTRL_MSR_BITMAP_FULL, HCPhysMsrBitmap);
}


/**
 * Sets up the APIC-access page address for the VMCS.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 */
DECLINLINE(int) hmR0VmxSetupVmcsApicAccessAddr(PVMCPU pVCpu)
{
    RTHCPHYS const HCPhysApicAccess = pVCpu->CTX_SUFF(pVM)->hm.s.vmx.HCPhysApicAccess;
    Assert(HCPhysApicAccess != NIL_RTHCPHYS);
    Assert(!(HCPhysApicAccess & 0xfff));                     /* Bits 11:0 MBZ. */
    return VMXWriteVmcs64(VMX_VMCS64_CTRL_APIC_ACCESSADDR_FULL, HCPhysApicAccess);
}


/**
 * Sets up the VMCS link pointer for the VMCS.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pVmcsInfo   The VMCS info. object.
 */
DECLINLINE(int) hmR0VmxSetupVmcsLinkPtr(PVMCPU pVCpu, PVMXVMCSINFO pVmcsInfo)
{
    NOREF(pVCpu); /* Used implicitly by VMXWriteVmcs64 on 32-bit hosts. */
    uint64_t const u64VmcsLinkPtr = pVmcsInfo->u64VmcsLinkPtr;
    Assert(u64VmcsLinkPtr == UINT64_C(0xffffffffffffffff));  /* Bits 63:0 MB1. */
    return VMXWriteVmcs64(VMX_VMCS64_GUEST_VMCS_LINK_PTR_FULL, u64VmcsLinkPtr);
}


/**
 * Sets up the VM-entry MSR load, VM-exit MSR-store and VM-exit MSR-load addresses
 * in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pVmcsInfo   The VMCS info. object.
 */
DECLINLINE(int) hmR0VmxSetupVmcsAutoLoadStoreMsrAddrs(PVMCPU pVCpu, PVMXVMCSINFO pVmcsInfo)
{
    NOREF(pVCpu); /* Used implicitly by VMXWriteVmcs64 on 32-bit hosts. */

    RTHCPHYS const HCPhysGuestMsrLoad = pVmcsInfo->HCPhysGuestMsrLoad;
    Assert(HCPhysGuestMsrLoad != NIL_RTHCPHYS);
    Assert(!(HCPhysGuestMsrLoad & 0xf));                     /* Bits 3:0 MBZ. */

    RTHCPHYS const HCPhysGuestMsrStore = pVmcsInfo->HCPhysGuestMsrStore;
    Assert(HCPhysGuestMsrStore != NIL_RTHCPHYS);
    Assert(!(HCPhysGuestMsrStore & 0xf));                    /* Bits 3:0 MBZ. */

    RTHCPHYS const HCPhysHostMsrLoad = pVmcsInfo->HCPhysHostMsrLoad;
    Assert(HCPhysHostMsrLoad != NIL_RTHCPHYS);
    Assert(!(HCPhysHostMsrLoad & 0xf));                      /* Bits 3:0 MBZ. */

    int rc = VMXWriteVmcs64(VMX_VMCS64_CTRL_ENTRY_MSR_LOAD_FULL, HCPhysGuestMsrLoad);
    rc    |= VMXWriteVmcs64(VMX_VMCS64_CTRL_EXIT_MSR_STORE_FULL, HCPhysGuestMsrStore);
    rc    |= VMXWriteVmcs64(VMX_VMCS64_CTRL_EXIT_MSR_LOAD_FULL,  HCPhysHostMsrLoad);
    AssertRCReturn(rc, rc);
    return VINF_SUCCESS;
}


/**
 * Sets up MSR permissions in the MSR bitmap of a VMCS info. object.
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmcsInfo       The VMCS info. object.
 * @param   fIsNstGstVmcs   Whether this is a nested-guest VMCS.
 */
static void hmR0VmxSetupVmcsMsrPermissions(PVMCPU pVCpu, PVMXVMCSINFO pVmcsInfo, bool fIsNstGstVmcs)
{
    Assert(pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_USE_MSR_BITMAPS);

    /*
     * The guest can access the following MSRs (read, write) without causing
     * VM-exits; they are loaded/stored automatically using fields in the VMCS.
     */
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    hmR0VmxSetMsrPermission(pVCpu, pVmcsInfo, fIsNstGstVmcs, MSR_IA32_SYSENTER_CS,  VMXMSRPM_ALLOW_RD_WR);
    hmR0VmxSetMsrPermission(pVCpu, pVmcsInfo, fIsNstGstVmcs, MSR_IA32_SYSENTER_ESP, VMXMSRPM_ALLOW_RD_WR);
    hmR0VmxSetMsrPermission(pVCpu, pVmcsInfo, fIsNstGstVmcs, MSR_IA32_SYSENTER_EIP, VMXMSRPM_ALLOW_RD_WR);
    hmR0VmxSetMsrPermission(pVCpu, pVmcsInfo, fIsNstGstVmcs, MSR_K8_GS_BASE,        VMXMSRPM_ALLOW_RD_WR);
    hmR0VmxSetMsrPermission(pVCpu, pVmcsInfo, fIsNstGstVmcs, MSR_K8_FS_BASE,        VMXMSRPM_ALLOW_RD_WR);

    /*
     * The IA32_PRED_CMD and IA32_FLUSH_CMD MSRs are write-only and has no state
     * associated with then. We never need to intercept access (writes need to be
     * executed without causing a VM-exit, reads will #GP fault anyway).
     *
     * The IA32_SPEC_CTRL MSR is read/write and has state. We allow the guest to
     * read/write them. We swap the the guest/host MSR value using the
     * auto-load/store MSR area.
     */
    if (pVM->cpum.ro.GuestFeatures.fIbpb)
        hmR0VmxSetMsrPermission(pVCpu, pVmcsInfo, fIsNstGstVmcs, MSR_IA32_PRED_CMD,  VMXMSRPM_ALLOW_RD_WR);
    if (pVM->cpum.ro.GuestFeatures.fFlushCmd)
        hmR0VmxSetMsrPermission(pVCpu, pVmcsInfo, fIsNstGstVmcs, MSR_IA32_FLUSH_CMD, VMXMSRPM_ALLOW_RD_WR);
    if (pVM->cpum.ro.GuestFeatures.fIbrs)
        hmR0VmxSetMsrPermission(pVCpu, pVmcsInfo, fIsNstGstVmcs, MSR_IA32_SPEC_CTRL, VMXMSRPM_ALLOW_RD_WR);

#if HC_ARCH_BITS == 64
    /*
     * Allow full read/write access for the following MSRs (mandatory for VT-x)
     * required for 64-bit guests.
     */
    if (pVM->hm.s.fAllow64BitGuests)
    {
        hmR0VmxSetMsrPermission(pVCpu, pVmcsInfo, fIsNstGstVmcs, MSR_K8_LSTAR,          VMXMSRPM_ALLOW_RD_WR);
        hmR0VmxSetMsrPermission(pVCpu, pVmcsInfo, fIsNstGstVmcs, MSR_K6_STAR,           VMXMSRPM_ALLOW_RD_WR);
        hmR0VmxSetMsrPermission(pVCpu, pVmcsInfo, fIsNstGstVmcs, MSR_K8_SF_MASK,        VMXMSRPM_ALLOW_RD_WR);
        hmR0VmxSetMsrPermission(pVCpu, pVmcsInfo, fIsNstGstVmcs, MSR_K8_KERNEL_GS_BASE, VMXMSRPM_ALLOW_RD_WR);
    }
#endif

    /*
     * IA32_EFER MSR is always intercepted, see @bugref{9180#c37}.
     */
#ifdef VBOX_STRICT
    Assert(pVmcsInfo->pvMsrBitmap);
    uint32_t const fMsrpmEfer = CPUMGetVmxMsrPermission(pVmcsInfo->pvMsrBitmap, MSR_K6_EFER);
    Assert(fMsrpmEfer == VMXMSRPM_EXIT_RD_WR);
#endif
}


/**
 * Sets up pin-based VM-execution controls in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmcsInfo       The VMCS info. object.
 */
static int hmR0VmxSetupVmcsPinCtls(PVMCPU pVCpu, PVMXVMCSINFO pVmcsInfo)
{
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    uint32_t       fVal = pVM->hm.s.vmx.Msrs.PinCtls.n.allowed0;      /* Bits set here must always be set. */
    uint32_t const fZap = pVM->hm.s.vmx.Msrs.PinCtls.n.allowed1;      /* Bits cleared here must always be cleared. */

    fVal |= VMX_PIN_CTLS_EXT_INT_EXIT                        /* External interrupts cause a VM-exit. */
         |  VMX_PIN_CTLS_NMI_EXIT;                           /* Non-maskable interrupts (NMIs) cause a VM-exit. */

    if (pVM->hm.s.vmx.Msrs.PinCtls.n.allowed1 & VMX_PIN_CTLS_VIRT_NMI)
        fVal |= VMX_PIN_CTLS_VIRT_NMI;                       /* Use virtual NMIs and virtual-NMI blocking features. */

    /* Enable the VMX-preemption timer. */
    if (pVM->hm.s.vmx.fUsePreemptTimer)
    {
        Assert(pVM->hm.s.vmx.Msrs.PinCtls.n.allowed1 & VMX_PIN_CTLS_PREEMPT_TIMER);
        fVal |= VMX_PIN_CTLS_PREEMPT_TIMER;
    }

#if 0
    /* Enable posted-interrupt processing. */
    if (pVM->hm.s.fPostedIntrs)
    {
        Assert(pVM->hm.s.vmx.Msrs.PinCtls.n.allowed1  & VMX_PIN_CTLS_POSTED_INT);
        Assert(pVM->hm.s.vmx.Msrs.ExitCtls.n.allowed1 & VMX_EXIT_CTLS_ACK_EXT_INT);
        fVal |= VMX_PIN_CTLS_POSTED_INT;
    }
#endif

    if ((fVal & fZap) != fVal)
    {
        LogRelFunc(("Invalid pin-based VM-execution controls combo! Cpu=%#RX32 fVal=%#RX32 fZap=%#RX32\n",
                    pVM->hm.s.vmx.Msrs.PinCtls.n.allowed0, fVal, fZap));
        pVCpu->hm.s.u32HMError = VMX_UFC_CTRL_PIN_EXEC;
        return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
    }

    /* Commit it to the VMCS and update our cache. */
    int rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_PIN_EXEC, fVal);
    AssertRCReturn(rc, rc);
    pVmcsInfo->u32PinCtls = fVal;

    return VINF_SUCCESS;
}


/**
 * Sets up secondary processor-based VM-execution controls in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmcsInfo       The VMCS info. object.
 */
static int hmR0VmxSetupVmcsProcCtls2(PVMCPU pVCpu, PVMXVMCSINFO pVmcsInfo)
{
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    uint32_t       fVal = pVM->hm.s.vmx.Msrs.ProcCtls2.n.allowed0;    /* Bits set here must be set in the VMCS. */
    uint32_t const fZap = pVM->hm.s.vmx.Msrs.ProcCtls2.n.allowed1;    /* Bits cleared here must be cleared in the VMCS. */

    /* WBINVD causes a VM-exit. */
    if (pVM->hm.s.vmx.Msrs.ProcCtls2.n.allowed1 & VMX_PROC_CTLS2_WBINVD_EXIT)
        fVal |= VMX_PROC_CTLS2_WBINVD_EXIT;

    /* Enable EPT (aka nested-paging). */
    if (pVM->hm.s.fNestedPaging)
        fVal |= VMX_PROC_CTLS2_EPT;

    /* Enable the INVPCID instruction if supported by the hardware and we expose
      it to the guest. Without this, guest executing INVPCID would cause a #UD. */
    if (   (pVM->hm.s.vmx.Msrs.ProcCtls2.n.allowed1 & VMX_PROC_CTLS2_INVPCID)
        && pVM->cpum.ro.GuestFeatures.fInvpcid)
        fVal |= VMX_PROC_CTLS2_INVPCID;

    /* Enable VPID. */
    if (pVM->hm.s.vmx.fVpid)
        fVal |= VMX_PROC_CTLS2_VPID;

    /* Enable unrestricted guest execution. */
    if (pVM->hm.s.vmx.fUnrestrictedGuest)
        fVal |= VMX_PROC_CTLS2_UNRESTRICTED_GUEST;

#if 0
    if (pVM->hm.s.fVirtApicRegs)
    {
        /* Enable APIC-register virtualization. */
        Assert(pVM->hm.s.vmx.Msrs.ProcCtls2.n.allowed1 & VMX_PROC_CTLS2_APIC_REG_VIRT);
        fVal |= VMX_PROC_CTLS2_APIC_REG_VIRT;

        /* Enable virtual-interrupt delivery. */
        Assert(pVM->hm.s.vmx.Msrs.ProcCtls2.n.allowed1 & VMX_PROC_CTLS2_VIRT_INTR_DELIVERY);
        fVal |= VMX_PROC_CTLS2_VIRT_INTR_DELIVERY;
    }
#endif

    /* Virtualize-APIC accesses if supported by the CPU. The virtual-APIC page is where the TPR shadow resides. */
    /** @todo VIRT_X2APIC support, it's mutually exclusive with this. So must be
     *        done dynamically. */
    if (pVM->hm.s.vmx.Msrs.ProcCtls2.n.allowed1 & VMX_PROC_CTLS2_VIRT_APIC_ACCESS)
    {
        fVal |= VMX_PROC_CTLS2_VIRT_APIC_ACCESS;
        int rc = hmR0VmxSetupVmcsApicAccessAddr(pVCpu);
        AssertRCReturn(rc, rc);
    }

    /* Enable the RDTSCP instruction if supported by the hardware and we expose
       it to the guest. Without this, guest executing RDTSCP would cause a #UD. */
    if (   (pVM->hm.s.vmx.Msrs.ProcCtls2.n.allowed1 & VMX_PROC_CTLS2_RDTSCP)
        && pVM->cpum.ro.GuestFeatures.fRdTscP)
        fVal |= VMX_PROC_CTLS2_RDTSCP;

    /* Enable Pause-Loop exiting. */
    if (   (pVM->hm.s.vmx.Msrs.ProcCtls2.n.allowed1 & VMX_PROC_CTLS2_PAUSE_LOOP_EXIT)
        && pVM->hm.s.vmx.cPleGapTicks
        && pVM->hm.s.vmx.cPleWindowTicks)
    {
        fVal |= VMX_PROC_CTLS2_PAUSE_LOOP_EXIT;

        int rc  = VMXWriteVmcs32(VMX_VMCS32_CTRL_PLE_GAP, pVM->hm.s.vmx.cPleGapTicks);
        rc     |= VMXWriteVmcs32(VMX_VMCS32_CTRL_PLE_WINDOW, pVM->hm.s.vmx.cPleWindowTicks);
        AssertRCReturn(rc, rc);
    }

    if ((fVal & fZap) != fVal)
    {
        LogRelFunc(("Invalid secondary processor-based VM-execution controls combo! cpu=%#RX32 fVal=%#RX32 fZap=%#RX32\n",
                    pVM->hm.s.vmx.Msrs.ProcCtls2.n.allowed0, fVal, fZap));
        pVCpu->hm.s.u32HMError = VMX_UFC_CTRL_PROC_EXEC2;
        return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
    }

    /* Commit it to the VMCS and update our cache. */
    int rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC2, fVal);
    AssertRCReturn(rc, rc);
    pVmcsInfo->u32ProcCtls2 = fVal;

    return VINF_SUCCESS;
}


/**
 * Sets up processor-based VM-execution controls in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmcsInfo       The VMCS info. object.
 */
static int hmR0VmxSetupVmcsProcCtls(PVMCPU pVCpu, PVMXVMCSINFO pVmcsInfo)
{
    PVM pVM = pVCpu->CTX_SUFF(pVM);

    uint32_t       fVal = pVM->hm.s.vmx.Msrs.ProcCtls.n.allowed0;     /* Bits set here must be set in the VMCS. */
    uint32_t const fZap = pVM->hm.s.vmx.Msrs.ProcCtls.n.allowed1;     /* Bits cleared here must be cleared in the VMCS. */

    fVal |= VMX_PROC_CTLS_HLT_EXIT                                    /* HLT causes a VM-exit. */
         |  VMX_PROC_CTLS_USE_TSC_OFFSETTING                          /* Use TSC-offsetting. */
         |  VMX_PROC_CTLS_MOV_DR_EXIT                                 /* MOV DRx causes a VM-exit. */
         |  VMX_PROC_CTLS_UNCOND_IO_EXIT                              /* All IO instructions cause a VM-exit. */
         |  VMX_PROC_CTLS_RDPMC_EXIT                                  /* RDPMC causes a VM-exit. */
         |  VMX_PROC_CTLS_MONITOR_EXIT                                /* MONITOR causes a VM-exit. */
         |  VMX_PROC_CTLS_MWAIT_EXIT;                                 /* MWAIT causes a VM-exit. */

    /* We toggle VMX_PROC_CTLS_MOV_DR_EXIT later, check if it's not -always- needed to be set or clear. */
    if (   !(pVM->hm.s.vmx.Msrs.ProcCtls.n.allowed1 & VMX_PROC_CTLS_MOV_DR_EXIT)
        ||  (pVM->hm.s.vmx.Msrs.ProcCtls.n.allowed0 & VMX_PROC_CTLS_MOV_DR_EXIT))
    {
        pVCpu->hm.s.u32HMError = VMX_UFC_CTRL_PROC_MOV_DRX_EXIT;
        return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
    }

    /* Without nested paging, INVLPG (also affects INVPCID) and MOV CR3 instructions should cause VM-exits. */
    if (!pVM->hm.s.fNestedPaging)
    {
        Assert(!pVM->hm.s.vmx.fUnrestrictedGuest);
        fVal |= VMX_PROC_CTLS_INVLPG_EXIT
             |  VMX_PROC_CTLS_CR3_LOAD_EXIT
             |  VMX_PROC_CTLS_CR3_STORE_EXIT;
    }

    /* Use TPR shadowing if supported by the CPU. */
    if (   PDMHasApic(pVM)
        && pVM->hm.s.vmx.Msrs.ProcCtls.n.allowed1 & VMX_PROC_CTLS_USE_TPR_SHADOW)
    {
        fVal |= VMX_PROC_CTLS_USE_TPR_SHADOW;                /* CR8 reads from the Virtual-APIC page. */
                                                             /* CR8 writes cause a VM-exit based on TPR threshold. */
        Assert(!(fVal & VMX_PROC_CTLS_CR8_STORE_EXIT));
        Assert(!(fVal & VMX_PROC_CTLS_CR8_LOAD_EXIT));
        int rc = hmR0VmxSetupVmcsVirtApicAddr(pVCpu, pVmcsInfo);
        AssertRCReturn(rc, rc);
    }
    else
    {
        /* Some 32-bit CPUs do not support CR8 load/store exiting as MOV CR8 is
           invalid on 32-bit Intel CPUs. Set this control only for 64-bit guests. */
        if (pVM->hm.s.fAllow64BitGuests)
        {
            fVal |= VMX_PROC_CTLS_CR8_STORE_EXIT             /* CR8 reads cause a VM-exit. */
                 |  VMX_PROC_CTLS_CR8_LOAD_EXIT;             /* CR8 writes cause a VM-exit. */
        }
    }

    /* Use MSR-bitmaps if supported by the CPU. */
    if (pVM->hm.s.vmx.Msrs.ProcCtls.n.allowed1 & VMX_PROC_CTLS_USE_MSR_BITMAPS)
    {
        fVal |= VMX_PROC_CTLS_USE_MSR_BITMAPS;
        int rc = hmR0VmxSetupVmcsMsrBitmapAddr(pVCpu, pVmcsInfo);
        AssertRCReturn(rc, rc);
    }

    /* Use the secondary processor-based VM-execution controls if supported by the CPU. */
    if (pVM->hm.s.vmx.Msrs.ProcCtls.n.allowed1 & VMX_PROC_CTLS_USE_SECONDARY_CTLS)
        fVal |= VMX_PROC_CTLS_USE_SECONDARY_CTLS;

    if ((fVal & fZap) != fVal)
    {
        LogRelFunc(("Invalid processor-based VM-execution controls combo! cpu=%#RX32 fVal=%#RX32 fZap=%#RX32\n",
                    pVM->hm.s.vmx.Msrs.ProcCtls.n.allowed0, fVal, fZap));
        pVCpu->hm.s.u32HMError = VMX_UFC_CTRL_PROC_EXEC;
        return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
    }

    /* Commit it to the VMCS and update our cache. */
    int rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC, fVal);
    AssertRCReturn(rc, rc);
    pVmcsInfo->u32ProcCtls = fVal;

    /* Set up MSR permissions that don't change through the lifetime of the VM. */
    if (pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_USE_MSR_BITMAPS)
        hmR0VmxSetupVmcsMsrPermissions(pVCpu, pVmcsInfo, false /* fIsNstGstVmcs */);

    /* Set up secondary processor-based VM-execution controls if the CPU supports it. */
    if (pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_USE_SECONDARY_CTLS)
        return hmR0VmxSetupVmcsProcCtls2(pVCpu, pVmcsInfo);

    /* Sanity check, should not really happen. */
    if (RT_LIKELY(!pVM->hm.s.vmx.fUnrestrictedGuest))
    { /* likely */ }
    else
    {
        pVCpu->hm.s.u32HMError = VMX_UFC_INVALID_UX_COMBO;
        return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
    }

    /* Old CPUs without secondary processor-based VM-execution controls would end up here. */
    return VINF_SUCCESS;
}


/**
 * Sets up miscellaneous (everything other than Pin, Processor and secondary
 * Processor-based VM-execution) control fields in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmcsInfo       The VMCS info. object.
 */
static int hmR0VmxSetupVmcsMiscCtls(PVMCPU pVCpu, PVMXVMCSINFO pVmcsInfo)
{
    /* Set the auto-load/store MSR area addresses in the VMCS. */
    int rc = hmR0VmxSetupVmcsAutoLoadStoreMsrAddrs(pVCpu, pVmcsInfo);
    if (RT_SUCCESS(rc))
    {
        /* Set the VMCS link pointer in the VMCS. */
        rc = hmR0VmxSetupVmcsLinkPtr(pVCpu, pVmcsInfo);
        if (RT_SUCCESS(rc))
        {
            /* Set the CR0/CR4 guest/host mask. */
            uint64_t const u64Cr0Mask = hmR0VmxGetFixedCr0Mask(pVCpu);
            uint64_t const u64Cr4Mask = hmR0VmxGetFixedCr4Mask(pVCpu);
            rc  = VMXWriteVmcsHstN(VMX_VMCS_CTRL_CR0_MASK, u64Cr0Mask);
            rc |= VMXWriteVmcsHstN(VMX_VMCS_CTRL_CR4_MASK, u64Cr4Mask);
            if (RT_SUCCESS(rc))
            {
                pVmcsInfo->u64Cr0Mask = u64Cr0Mask;
                pVmcsInfo->u64Cr4Mask = u64Cr4Mask;
                return VINF_SUCCESS;
            }
            LogRelFunc(("Failed to initialize VMCS CR0/CR4 guest/host mask. rc=%Rrc\n", rc));
        }
        else
            LogRelFunc(("Failed to initialize VMCS link pointer. rc=%Rrc\n", rc));
    }
    else
        LogRelFunc(("Failed to initialize VMCS auto-load/store MSR addresses. rc=%Rrc\n", rc));
    return rc;
}


/**
 * Sets up the initial exception bitmap in the VMCS based on static conditions.
 *
 * We shall setup those exception intercepts that don't change during the
 * lifetime of the VM here. The rest are done dynamically while loading the
 * guest state.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmcsInfo       The VMCS info. object.
 */
static int hmR0VmxSetupVmcsXcptBitmap(PVMCPU pVCpu, PVMXVMCSINFO pVmcsInfo)
{
    /*
     * The following exceptions are always intercepted:
     *
     * #AC - To prevent the guest from hanging the CPU.
     * #DB - To maintain the DR6 state even when intercepting DRx reads/writes and
     *       recursive #DBs can cause a CPU hang.
     * #PF - To sync our shadow page tables when nested-paging is not used.
     */
    bool const fNestedPaging = pVCpu->CTX_SUFF(pVM)->hm.s.fNestedPaging;
    uint32_t const uXcptBitmap = RT_BIT(X86_XCPT_AC)
                               | RT_BIT(X86_XCPT_DB)
                               | (fNestedPaging ? 0 : RT_BIT(X86_XCPT_PF));

    /* Commit it to the VMCS. */
    int rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_EXCEPTION_BITMAP, uXcptBitmap);
    AssertRCReturn(rc, rc);

    /* Update our cache of the exception bitmap. */
    pVmcsInfo->u32XcptBitmap = uXcptBitmap;
    return VINF_SUCCESS;
}


#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
/**
 * Sets up the VMCS for executing a nested-guest using hardware-assisted VMX.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmcsInfo       The VMCS info. object.
 */
static int hmR0VmxSetupVmcsCtlsNested(PVMCPU pVCpu, PVMXVMCSINFO pVmcsInfo)
{
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    int rc = hmR0VmxSetupVmcsLinkPtr(pVCpu, pVmcsInfo);
    if (RT_SUCCESS(rc))
    {
        rc = hmR0VmxSetupVmcsAutoLoadStoreMsrAddrs(pVCpu, pVmcsInfo);
        if (RT_SUCCESS(rc))
        {
            if (pVM->hm.s.vmx.Msrs.ProcCtls.n.allowed1 & VMX_PROC_CTLS_USE_MSR_BITMAPS)
                rc = hmR0VmxSetupVmcsMsrBitmapAddr(pVCpu, pVmcsInfo);
            if (RT_SUCCESS(rc))
            {
                if (pVM->hm.s.vmx.Msrs.ProcCtls2.n.allowed1 & VMX_PROC_CTLS2_VIRT_APIC_ACCESS)
                    rc = hmR0VmxSetupVmcsApicAccessAddr(pVCpu);
                if (RT_SUCCESS(rc))
                    return VINF_SUCCESS;

                LogRelFunc(("Failed to set up the APIC-access address in the nested-guest VMCS. rc=%Rrc\n", rc));
            }
            else
                LogRelFunc(("Failed to set up the MSR-bitmap address in the nested-guest VMCS. rc=%Rrc\n", rc));
        }
        else
            LogRelFunc(("Failed to set up the VMCS link pointer in the nested-guest VMCS. rc=%Rrc\n", rc));
    }
    else
        LogRelFunc(("Failed to set up the auto-load/store MSR addresses in the nested-guest VMCS. rc=%Rrc\n", rc));

    return rc;
}
#endif


/**
 * Sets up the VMCS for executing a guest (or nested-guest) using hardware-assisted
 * VMX.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmcsInfo       The VMCS info. object.
 * @param   fIsNstGstVmcs   Whether this is a nested-guest VMCS.
 */
static int hmR0VmxSetupVmcs(PVMCPU pVCpu, PVMXVMCSINFO pVmcsInfo, bool fIsNstGstVmcs)
{
    Assert(pVmcsInfo->pvVmcs);
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    /* Set the CPU specified revision identifier at the beginning of the VMCS structure. */
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    *(uint32_t *)pVmcsInfo->pvVmcs = RT_BF_GET(pVM->hm.s.vmx.Msrs.u64Basic, VMX_BF_BASIC_VMCS_ID);
    const char * const pszVmcs     = fIsNstGstVmcs ? "nested-guest VMCS" : "guest VMCS";

    LogFlowFunc(("\n"));

    /*
     * Initialize the VMCS using VMCLEAR before loading the VMCS.
     * See Intel spec. 31.6 "Preparation And Launching A Virtual Machine".
     */
    int rc = hmR0VmxClearVmcs(pVmcsInfo);
    if (RT_SUCCESS(rc))
    {
        rc = hmR0VmxLoadVmcs(pVmcsInfo);
        if (RT_SUCCESS(rc))
        {
            if (!fIsNstGstVmcs)
            {
                rc = hmR0VmxSetupVmcsPinCtls(pVCpu, pVmcsInfo);
                if (RT_SUCCESS(rc))
                {
                    rc = hmR0VmxSetupVmcsProcCtls(pVCpu, pVmcsInfo);
                    if (RT_SUCCESS(rc))
                    {
                        rc = hmR0VmxSetupVmcsMiscCtls(pVCpu, pVmcsInfo);
                        if (RT_SUCCESS(rc))
                        {
                            rc = hmR0VmxSetupVmcsXcptBitmap(pVCpu, pVmcsInfo);
                            if (RT_SUCCESS(rc))
                            { /* likely */ }
                            else
                                LogRelFunc(("Failed to initialize exception bitmap. rc=%Rrc\n", rc));
                        }
                        else
                            LogRelFunc(("Failed to setup miscellaneous controls. rc=%Rrc\n", rc));
                    }
                    else
                        LogRelFunc(("Failed to setup processor-based VM-execution controls. rc=%Rrc\n", rc));
                }
                else
                    LogRelFunc(("Failed to setup pin-based controls. rc=%Rrc\n", rc));
            }
            else
            {
#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
                rc = hmR0VmxSetupVmcsCtlsNested(pVCpu, pVmcsInfo);
                if (RT_SUCCESS(rc))
                { /* likely */ }
                else
                    LogRelFunc(("Failed to initialize nested-guest VMCS. rc=%Rrc\n", rc));
#else
                AssertFailed();
#endif
            }
        }
        else
            LogRelFunc(("Failed to load the %s. rc=%Rrc\n", rc, pszVmcs));
    }
    else
        LogRelFunc(("Failed to clear the %s. rc=%Rrc\n", rc, pszVmcs));

    /* Sync any CPU internal VMCS data back into our VMCS in memory. */
    if (RT_SUCCESS(rc))
    {
        rc = hmR0VmxClearVmcs(pVmcsInfo);
        if (RT_SUCCESS(rc))
        { /* likely */ }
        else
            LogRelFunc(("Failed to clear the %s post setup. rc=%Rrc\n", rc, pszVmcs));
    }

    /*
     * Update the last-error record both for failures and success, so we
     * can propagate the status code back to ring-3 for diagnostics.
     */
    hmR0VmxUpdateErrorRecord(pVCpu, rc);
    NOREF(pszVmcs);
    return rc;
}


/**
 * Does global VT-x initialization (called during module initialization).
 *
 * @returns VBox status code.
 */
VMMR0DECL(int) VMXR0GlobalInit(void)
{
#ifdef HMVMX_USE_FUNCTION_TABLE
    AssertCompile(VMX_EXIT_MAX + 1 == RT_ELEMENTS(g_apfnVMExitHandlers));
# ifdef VBOX_STRICT
    for (unsigned i = 0; i < RT_ELEMENTS(g_apfnVMExitHandlers); i++)
        Assert(g_apfnVMExitHandlers[i]);
# endif
#endif
    return VINF_SUCCESS;
}


/**
 * Does global VT-x termination (called during module termination).
 */
VMMR0DECL(void) VMXR0GlobalTerm()
{
    /* Nothing to do currently. */
}


/**
 * Sets up and activates VT-x on the current CPU.
 *
 * @returns VBox status code.
 * @param   pHostCpu        The HM physical-CPU structure.
 * @param   pVM             The cross context VM structure.  Can be
 *                          NULL after a host resume operation.
 * @param   pvCpuPage       Pointer to the VMXON region (can be NULL if @a
 *                          fEnabledByHost is @c true).
 * @param   HCPhysCpuPage   Physical address of the VMXON region (can be 0 if
 *                          @a fEnabledByHost is @c true).
 * @param   fEnabledByHost  Set if SUPR0EnableVTx() or similar was used to
 *                          enable VT-x on the host.
 * @param   pHwvirtMsrs     Pointer to the hardware-virtualization MSRs.
 */
VMMR0DECL(int) VMXR0EnableCpu(PHMPHYSCPU pHostCpu, PVM pVM, void *pvCpuPage, RTHCPHYS HCPhysCpuPage, bool fEnabledByHost,
                              PCSUPHWVIRTMSRS pHwvirtMsrs)
{
    AssertPtr(pHostCpu);
    AssertPtr(pHwvirtMsrs);
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    /* Enable VT-x if it's not already enabled by the host. */
    if (!fEnabledByHost)
    {
        int rc = hmR0VmxEnterRootMode(pVM, HCPhysCpuPage, pvCpuPage);
        if (RT_FAILURE(rc))
            return rc;
    }

    /*
     * Flush all EPT tagged-TLB entries (in case VirtualBox or any other hypervisor have been
     * using EPTPs) so we don't retain any stale guest-physical mappings which won't get
     * invalidated when flushing by VPID.
     */
    if (pHwvirtMsrs->u.vmx.u64EptVpidCaps & MSR_IA32_VMX_EPT_VPID_CAP_INVEPT_ALL_CONTEXTS)
    {
        hmR0VmxFlushEpt(NULL /* pVCpu */, NULL /* pVmcsInfo */, VMXTLBFLUSHEPT_ALL_CONTEXTS);
        pHostCpu->fFlushAsidBeforeUse = false;
    }
    else
        pHostCpu->fFlushAsidBeforeUse = true;

    /* Ensure each VCPU scheduled on this CPU gets a new VPID on resume. See @bugref{6255}. */
    ++pHostCpu->cTlbFlushes;

    return VINF_SUCCESS;
}


/**
 * Deactivates VT-x on the current CPU.
 *
 * @returns VBox status code.
 * @param   pvCpuPage       Pointer to the VMXON region.
 * @param   HCPhysCpuPage   Physical address of the VMXON region.
 *
 * @remarks This function should never be called when SUPR0EnableVTx() or
 *          similar was used to enable VT-x on the host.
 */
VMMR0DECL(int) VMXR0DisableCpu(void *pvCpuPage, RTHCPHYS HCPhysCpuPage)
{
    RT_NOREF2(pvCpuPage, HCPhysCpuPage);

    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));
    return hmR0VmxLeaveRootMode();
}


/**
 * Does per-VM VT-x initialization.
 *
 * @returns VBox status code.
 * @param   pVM             The cross context VM structure.
 */
VMMR0DECL(int) VMXR0InitVM(PVM pVM)
{
    AssertPtr(pVM);
    LogFlowFunc(("pVM=%p\n", pVM));

    int rc = hmR0VmxStructsAlloc(pVM);
    if (RT_FAILURE(rc))
    {
        LogRelFunc(("Failed to allocated VMX structures. rc=%Rrc\n", rc));
        return rc;
    }

    return VINF_SUCCESS;
}


/**
 * Does per-VM VT-x termination.
 *
 * @returns VBox status code.
 * @param   pVM         The cross context VM structure.
 */
VMMR0DECL(int) VMXR0TermVM(PVM pVM)
{
    AssertPtr(pVM);
    LogFlowFunc(("pVM=%p\n", pVM));

#ifdef VBOX_WITH_CRASHDUMP_MAGIC
    if (pVM->hm.s.vmx.hMemObjScratch != NIL_RTR0MEMOBJ)
    {
        Assert(pVM->hm.s.vmx.pvScratch);
        ASMMemZero32(pVM->hm.s.vmx.pvScratch, X86_PAGE_4K_SIZE);
    }
#endif
    hmR0VmxStructsFree(pVM);
    return VINF_SUCCESS;
}


/**
 * Sets up the VM for execution using hardware-assisted VMX.
 * This function is only called once per-VM during initialization.
 *
 * @returns VBox status code.
 * @param   pVM         The cross context VM structure.
 */
VMMR0DECL(int) VMXR0SetupVM(PVM pVM)
{
    AssertPtr(pVM);
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    LogFlowFunc(("pVM=%p\n", pVM));

    /*
     * At least verify if VMX is enabled, since we can't check if we're in
     * VMX root mode or not without causing a #GP.
     */
    RTCCUINTREG const uHostCr4 = ASMGetCR4();
    if (RT_LIKELY(uHostCr4 & X86_CR4_VMXE))
    { /* likely */ }
    else
        return VERR_VMX_NOT_IN_VMX_ROOT_MODE;

    /*
     * Without unrestricted guest execution, pRealModeTSS and pNonPagingModeEPTPageTable *must*
     * always be allocated. We no longer support the highly unlikely case of unrestricted guest
     * without pRealModeTSS, see hmR3InitFinalizeR0Intel().
     */
    if (   !pVM->hm.s.vmx.fUnrestrictedGuest
        &&  (   !pVM->hm.s.vmx.pNonPagingModeEPTPageTable
             || !pVM->hm.s.vmx.pRealModeTSS))
    {
        LogRelFunc(("Invalid real-on-v86 state.\n"));
        return VERR_INTERNAL_ERROR;
    }

    /* Initialize these always, see hmR3InitFinalizeR0().*/
    pVM->hm.s.vmx.enmTlbFlushEpt  = VMXTLBFLUSHEPT_NONE;
    pVM->hm.s.vmx.enmTlbFlushVpid = VMXTLBFLUSHVPID_NONE;

    /* Setup the tagged-TLB flush handlers. */
    int rc = hmR0VmxSetupTaggedTlb(pVM);
    if (RT_FAILURE(rc))
    {
        LogRelFunc(("hmR0VmxSetupTaggedTlb failed! rc=%Rrc\n", rc));
        return rc;
    }

    /* Check if we can use the VMCS controls for swapping the EFER MSR. */
    Assert(!pVM->hm.s.vmx.fSupportsVmcsEfer);
#if HC_ARCH_BITS == 64
    if (   (pVM->hm.s.vmx.Msrs.EntryCtls.n.allowed1 & VMX_ENTRY_CTLS_LOAD_EFER_MSR)
        && (pVM->hm.s.vmx.Msrs.ExitCtls.n.allowed1  & VMX_EXIT_CTLS_LOAD_EFER_MSR)
        && (pVM->hm.s.vmx.Msrs.ExitCtls.n.allowed1  & VMX_EXIT_CTLS_SAVE_EFER_MSR))
        pVM->hm.s.vmx.fSupportsVmcsEfer = true;
#endif

    for (VMCPUID idCpu = 0; idCpu < pVM->cCpus; idCpu++)
    {
        PVMCPU pVCpu = &pVM->aCpus[idCpu];
        Log4Func(("pVCpu=%p idCpu=%RU32\n", pVCpu, pVCpu->idCpu));

        rc = hmR0VmxSetupVmcs(pVCpu, &pVCpu->hm.s.vmx.VmcsInfo,  false /* fIsNstGstVmcs */);
        if (RT_SUCCESS(rc))
        {
#if HC_ARCH_BITS == 32
            hmR0VmxInitVmcsReadCache(pVCpu);
#endif
#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
            if (pVM->cpum.ro.GuestFeatures.fVmx)
            {
                rc = hmR0VmxSetupVmcs(pVCpu, &pVCpu->hm.s.vmx.VmcsInfoNstGst, true /* fIsNstGstVmcs */);
                if (RT_SUCCESS(rc))
                { /* likely */ }
                else
                {
                    LogRelFunc(("Nested-guest VMCS setup failed. rc=%Rrc\n", rc));
                    return rc;
                }
            }
#endif
        }
        else
        {
            LogRelFunc(("VMCS setup failed. rc=%Rrc\n", rc));
            return rc;
        }
    }

    return VINF_SUCCESS;
}


#if HC_ARCH_BITS == 32
# ifdef VBOX_ENABLE_64_BITS_GUESTS
/**
 * Check if guest state allows safe use of 32-bit switcher again.
 *
 * Segment bases and protected mode structures must be 32-bit addressable
 * because the  32-bit switcher will ignore high dword when writing these VMCS
 * fields.  See @bugref{8432} for details.
 *
 * @returns true if safe, false if must continue to use the 64-bit switcher.
 * @param   pCtx   Pointer to the guest-CPU context.
 *
 * @remarks No-long-jump zone!!!
 */
static bool hmR0VmxIs32BitSwitcherSafe(PCCPUMCTX pCtx)
{
    if (pCtx->gdtr.pGdt    & UINT64_C(0xffffffff00000000))     return false;
    if (pCtx->idtr.pIdt    & UINT64_C(0xffffffff00000000))     return false;
    if (pCtx->ldtr.u64Base & UINT64_C(0xffffffff00000000))     return false;
    if (pCtx->tr.u64Base   & UINT64_C(0xffffffff00000000))     return false;
    if (pCtx->es.u64Base   & UINT64_C(0xffffffff00000000))     return false;
    if (pCtx->cs.u64Base   & UINT64_C(0xffffffff00000000))     return false;
    if (pCtx->ss.u64Base   & UINT64_C(0xffffffff00000000))     return false;
    if (pCtx->ds.u64Base   & UINT64_C(0xffffffff00000000))     return false;
    if (pCtx->fs.u64Base   & UINT64_C(0xffffffff00000000))     return false;
    if (pCtx->gs.u64Base   & UINT64_C(0xffffffff00000000))     return false;

    /* All good, bases are 32-bit. */
    return true;
}
# endif /* VBOX_ENABLE_64_BITS_GUESTS */

# ifdef VBOX_STRICT
static bool hmR0VmxIsValidWriteField(uint32_t idxField)
{
    switch (idxField)
    {
        case VMX_VMCS_GUEST_RIP:
        case VMX_VMCS_GUEST_RSP:
        case VMX_VMCS_GUEST_SYSENTER_EIP:
        case VMX_VMCS_GUEST_SYSENTER_ESP:
        case VMX_VMCS_GUEST_GDTR_BASE:
        case VMX_VMCS_GUEST_IDTR_BASE:
        case VMX_VMCS_GUEST_CS_BASE:
        case VMX_VMCS_GUEST_DS_BASE:
        case VMX_VMCS_GUEST_ES_BASE:
        case VMX_VMCS_GUEST_FS_BASE:
        case VMX_VMCS_GUEST_GS_BASE:
        case VMX_VMCS_GUEST_SS_BASE:
        case VMX_VMCS_GUEST_LDTR_BASE:
        case VMX_VMCS_GUEST_TR_BASE:
        case VMX_VMCS_GUEST_CR3:
            return true;
    }
    return false;
}

static bool hmR0VmxIsValidReadField(uint32_t idxField)
{
    switch (idxField)
    {
        /* Read-only fields. */
        case VMX_VMCS_RO_EXIT_QUALIFICATION:
            return true;
    }
    /* Remaining readable fields should also be writable. */
    return hmR0VmxIsValidWriteField(idxField);
}
# endif /* VBOX_STRICT */


/**
 * Executes the specified handler in 64-bit mode.
 *
 * @returns VBox status code (no informational status codes).
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   enmOp       The operation to perform.
 * @param   cParams     Number of parameters.
 * @param   paParam     Array of 32-bit parameters.
 */
VMMR0DECL(int) VMXR0Execute64BitsHandler(PVMCPU pVCpu, HM64ON32OP enmOp, uint32_t cParams, uint32_t *paParam)
{
    AssertPtr(pVCpu);
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    AssertReturn(pVM->hm.s.pfnHost32ToGuest64R0, VERR_HM_NO_32_TO_64_SWITCHER);
    Assert(enmOp > HM64ON32OP_INVALID && enmOp < HM64ON32OP_END);
    Assert(pVCpu->hm.s.vmx.VmcsCache.Write.cValidEntries <= RT_ELEMENTS(pVCpu->hm.s.vmx.VmcsCache.Write.aField));
    Assert(pVCpu->hm.s.vmx.VmcsCache.Read.cValidEntries <= RT_ELEMENTS(pVCpu->hm.s.vmx.VmcsCache.Read.aField));

#ifdef VBOX_STRICT
    for (uint32_t i = 0; i < pVCpu->hm.s.vmx.VmcsCache.Write.cValidEntries; i++)
        Assert(hmR0VmxIsValidWriteField(pVCpu->hm.s.vmx.VmcsCache.Write.aField[i]));

    for (uint32_t i = 0; i <pVCpu->hm.s.vmx.VmcsCache.Read.cValidEntries; i++)
        Assert(hmR0VmxIsValidReadField(pVCpu->hm.s.vmx.VmcsCache.Read.aField[i]));
#endif

    /* Disable interrupts. */
    RTCCUINTREG fOldEFlags = ASMIntDisableFlags();

#ifdef VBOX_WITH_VMMR0_DISABLE_LAPIC_NMI
    RTCPUID idHostCpu = RTMpCpuId();
    CPUMR0SetLApic(pVCpu, idHostCpu);
#endif

    /** @todo replace with hmR0VmxEnterRootMode() and hmR0VmxLeaveRootMode(). */

    PCHMPHYSCPU    pHostCpu      = hmR0GetCurrentCpu();
    RTHCPHYS const HCPhysCpuPage = pHostCpu->HCPhysMemObj;

    /* Clear VMCS. Marking it inactive, clearing implementation-specific data and writing VMCS data back to memory. */
    PVMXVMCSINFO pVmcsInfo = hmGetVmxActiveVmcsInfo(pVCpu);
    hmR0VmxClearVmcs(pVmcsInfo);

    /* Leave VMX root mode and disable VMX. */
    VMXDisable();
    SUPR0ChangeCR4(0, ~X86_CR4_VMXE);

    CPUMSetHyperESP(pVCpu, VMMGetStackRC(pVCpu));
    CPUMSetHyperEIP(pVCpu, enmOp);
    for (int i = (int)cParams - 1; i >= 0; i--)
        CPUMPushHyper(pVCpu, paParam[i]);

    STAM_PROFILE_ADV_START(&pVCpu->hm.s.StatWorldSwitch3264, z);

    /* Call the switcher. */
    int rc = pVM->hm.s.pfnHost32ToGuest64R0(pVM, RT_UOFFSETOF_DYN(VM, aCpus[pVCpu->idCpu].cpum) - RT_UOFFSETOF(VM, cpum));
    STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatWorldSwitch3264, z);

    /* Re-enable VMX to make sure the VMX instructions don't cause #UD faults. */
    SUPR0ChangeCR4(X86_CR4_VMXE, RTCCUINTREG_MAX);

    /* Re-enter VMX root mode. */
    int rc2 = VMXEnable(HCPhysCpuPage);
    if (RT_FAILURE(rc2))
    {
        SUPR0ChangeCR4(0, ~X86_CR4_VMXE);
        ASMSetFlags(fOldEFlags);
        pVM->hm.s.vmx.HCPhysVmxEnableError = HCPhysCpuPage;
        return rc2;
    }

    /* Restore the VMCS as the current VMCS. */
    rc2 = hmR0VmxLoadVmcs(pVmcsInfo);
    AssertRC(rc2);
    Assert(!(ASMGetFlags() & X86_EFL_IF));
    ASMSetFlags(fOldEFlags);
    return rc;
}


/**
 * Prepares for and executes VMLAUNCH (64-bit guests) for 32-bit hosts
 * supporting 64-bit guests.
 *
 * @returns VBox status code.
 * @param   fResume     Whether to VMLAUNCH or VMRESUME.
 * @param   pCtx        Pointer to the guest-CPU context.
 * @param   pCache      Pointer to the VMCS batch cache.
 * @param   pVM         The cross context VM structure.
 * @param   pVCpu       The cross context virtual CPU structure.
 */
DECLASM(int) VMXR0SwitcherStartVM64(RTHCUINT fResume, PCPUMCTX pCtx, PVMXVMCSCACHE pCache, PVM pVM, PVMCPU pVCpu)
{
    NOREF(fResume);

    PVMXVMCSINFO   pVmcsInfo     = hmGetVmxActiveVmcsInfo(pVCpu);
    PCHMPHYSCPU    pHostCpu      = hmR0GetCurrentCpu();
    RTHCPHYS const HCPhysCpuPage = pHostCpu->HCPhysMemObj;

#ifdef VBOX_WITH_CRASHDUMP_MAGIC
    pCache->uPos = 1;
    pCache->interPD = PGMGetInterPaeCR3(pVM);
    pCache->pSwitcher = (uint64_t)pVM->hm.s.pfnHost32ToGuest64R0;
#endif

#if defined(DEBUG) && defined(VMX_USE_CACHED_VMCS_ACCESSES)
    pCache->TestIn.HCPhysCpuPage = 0;
    pCache->TestIn.HCPhysVmcs    = 0;
    pCache->TestIn.pCache        = 0;
    pCache->TestOut.HCPhysVmcs   = 0;
    pCache->TestOut.pCache       = 0;
    pCache->TestOut.pCtx         = 0;
    pCache->TestOut.eflags       = 0;
#else
    NOREF(pCache);
#endif

    uint32_t aParam[10];
    aParam[0] = RT_LO_U32(HCPhysCpuPage);                               /* Param 1: VMXON physical address - Lo. */
    aParam[1] = RT_HI_U32(HCPhysCpuPage);                               /* Param 1: VMXON physical address - Hi. */
    aParam[2] = RT_LO_U32(pVmcsInfo->HCPhysVmcs);                       /* Param 2: VMCS physical address - Lo. */
    aParam[3] = RT_HI_U32(pVmcsInfo->HCPhysVmcs);                       /* Param 2: VMCS physical address - Hi. */
    aParam[4] = VM_RC_ADDR(pVM, &pVM->aCpus[pVCpu->idCpu].hm.s.vmx.VmcsCache);
    aParam[5] = 0;
    aParam[6] = VM_RC_ADDR(pVM, pVM);
    aParam[7] = 0;
    aParam[8] = VM_RC_ADDR(pVM, pVCpu);
    aParam[9] = 0;

#ifdef VBOX_WITH_CRASHDUMP_MAGIC
    pCtx->dr[4] = pVM->hm.s.vmx.pScratchPhys + 16 + 8;
    *(uint32_t *)(pVM->hm.s.vmx.pScratch + 16 + 8) = 1;
#endif
    int rc = VMXR0Execute64BitsHandler(pVCpu, HM64ON32OP_VMXRCStartVM64, RT_ELEMENTS(aParam), &aParam[0]);

#ifdef VBOX_WITH_CRASHDUMP_MAGIC
    Assert(*(uint32_t *)(pVM->hm.s.vmx.pScratch + 16 + 8) == 5);
    Assert(pCtx->dr[4] == 10);
    *(uint32_t *)(pVM->hm.s.vmx.pScratch + 16 + 8) = 0xff;
#endif

#if defined(DEBUG) && defined(VMX_USE_CACHED_VMCS_ACCESSES)
    AssertMsg(pCache->TestIn.HCPhysCpuPage == HCPhysCpuPage, ("%RHp vs %RHp\n", pCache->TestIn.HCPhysCpuPage, HCPhysCpuPage));
    AssertMsg(pCache->TestIn.HCPhysVmcs == pVmcsInfo->HCPhysVmcs,      ("%RHp vs %RHp\n", pCache->TestIn.HCPhysVmcs,
                                                                        pVmcsInfo->HCPhysVmcs));
    AssertMsg(pCache->TestIn.HCPhysVmcs == pCache->TestOut.HCPhysVmcs, ("%RHp vs %RHp\n", pCache->TestIn.HCPhysVmcs,
                                                                        pCache->TestOut.HCPhysVmcs));
    AssertMsg(pCache->TestIn.pCache     == pCache->TestOut.pCache,     ("%RGv vs %RGv\n", pCache->TestIn.pCache,
                                                                        pCache->TestOut.pCache));
    AssertMsg(pCache->TestIn.pCache     == VM_RC_ADDR(pVM, &pVM->aCpus[pVCpu->idCpu].hm.s.vmx.VmcsCache),
              ("%RGv vs %RGv\n", pCache->TestIn.pCache, VM_RC_ADDR(pVM, &pVM->aCpus[pVCpu->idCpu].hm.s.vmx.VmcsCache)));
    AssertMsg(pCache->TestIn.pCtx       == pCache->TestOut.pCtx,       ("%RGv vs %RGv\n", pCache->TestIn.pCtx,
                                                                        pCache->TestOut.pCtx));
    Assert(!(pCache->TestOut.eflags & X86_EFL_IF));
#endif
    NOREF(pCtx);
    return rc;
}
#endif


/**
 * Saves the host control registers (CR0, CR3, CR4) into the host-state area in
 * the VMCS.
 *
 * @returns VBox status code.
 */
static int hmR0VmxExportHostControlRegs(void)
{
    RTCCUINTREG uReg = ASMGetCR0();
    int rc = VMXWriteVmcsHstN(VMX_VMCS_HOST_CR0, uReg);
    AssertRCReturn(rc, rc);

    uReg = ASMGetCR3();
    rc = VMXWriteVmcsHstN(VMX_VMCS_HOST_CR3, uReg);
    AssertRCReturn(rc, rc);

    uReg = ASMGetCR4();
    rc = VMXWriteVmcsHstN(VMX_VMCS_HOST_CR4, uReg);
    AssertRCReturn(rc, rc);
    return rc;
}


/**
 * Saves the host segment registers and GDTR, IDTR, (TR, GS and FS bases) into
 * the host-state area in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 */
static int hmR0VmxExportHostSegmentRegs(PVMCPU pVCpu)
{
#if HC_ARCH_BITS == 64
/**
 * Macro for adjusting host segment selectors to satisfy VT-x's VM-entry
 * requirements. See hmR0VmxExportHostSegmentRegs().
 */
# define VMXLOCAL_ADJUST_HOST_SEG(seg, selValue)  \
    if ((selValue) & (X86_SEL_RPL | X86_SEL_LDT)) \
    { \
        bool fValidSelector = true; \
        if ((selValue) & X86_SEL_LDT) \
        { \
            uint32_t uAttr = ASMGetSegAttr((selValue)); \
            fValidSelector = RT_BOOL(uAttr != UINT32_MAX && (uAttr & X86_DESC_P)); \
        } \
        if (fValidSelector) \
        { \
            pVCpu->hm.s.vmx.fRestoreHostFlags |= VMX_RESTORE_HOST_SEL_##seg; \
            pVCpu->hm.s.vmx.RestoreHost.uHostSel##seg = (selValue); \
        } \
        (selValue) = 0; \
    }

    /*
     * If we've executed guest code using hardware-assisted VMX, the host-state bits
     * will be messed up. We should -not- save the messed up state without restoring
     * the original host-state, see @bugref{7240}.
     *
     * This apparently can happen (most likely the FPU changes), deal with it rather than
     * asserting. Was observed booting Solaris 10u10 32-bit guest.
     */
    if (   (pVCpu->hm.s.vmx.fRestoreHostFlags & VMX_RESTORE_HOST_REQUIRED)
        && (pVCpu->hm.s.vmx.fRestoreHostFlags & ~VMX_RESTORE_HOST_REQUIRED))
    {
        Log4Func(("Restoring Host State: fRestoreHostFlags=%#RX32 HostCpuId=%u\n", pVCpu->hm.s.vmx.fRestoreHostFlags,
                  pVCpu->idCpu));
        VMXRestoreHostState(pVCpu->hm.s.vmx.fRestoreHostFlags, &pVCpu->hm.s.vmx.RestoreHost);
    }
    pVCpu->hm.s.vmx.fRestoreHostFlags = 0;
#else
    RT_NOREF(pVCpu);
#endif

    /*
     * Host DS, ES, FS and GS segment registers.
     */
#if HC_ARCH_BITS == 64
    RTSEL uSelDS = ASMGetDS();
    RTSEL uSelES = ASMGetES();
    RTSEL uSelFS = ASMGetFS();
    RTSEL uSelGS = ASMGetGS();
#else
    RTSEL uSelDS = 0;
    RTSEL uSelES = 0;
    RTSEL uSelFS = 0;
    RTSEL uSelGS = 0;
#endif

    /*
     * Host CS and SS segment registers.
     */
    RTSEL uSelCS = ASMGetCS();
    RTSEL uSelSS = ASMGetSS();

    /*
     * Host TR segment register.
     */
    RTSEL uSelTR = ASMGetTR();

#if HC_ARCH_BITS == 64
    /*
     * Determine if the host segment registers are suitable for VT-x. Otherwise use zero to
     * gain VM-entry and restore them before we get preempted.
     *
     * See Intel spec. 26.2.3 "Checks on Host Segment and Descriptor-Table Registers".
     */
    VMXLOCAL_ADJUST_HOST_SEG(DS, uSelDS);
    VMXLOCAL_ADJUST_HOST_SEG(ES, uSelES);
    VMXLOCAL_ADJUST_HOST_SEG(FS, uSelFS);
    VMXLOCAL_ADJUST_HOST_SEG(GS, uSelGS);
# undef VMXLOCAL_ADJUST_HOST_SEG
#endif

    /* Verification based on Intel spec. 26.2.3 "Checks on Host Segment and Descriptor-Table Registers"  */
    Assert(!(uSelCS & X86_SEL_RPL)); Assert(!(uSelCS & X86_SEL_LDT));
    Assert(!(uSelSS & X86_SEL_RPL)); Assert(!(uSelSS & X86_SEL_LDT));
    Assert(!(uSelDS & X86_SEL_RPL)); Assert(!(uSelDS & X86_SEL_LDT));
    Assert(!(uSelES & X86_SEL_RPL)); Assert(!(uSelES & X86_SEL_LDT));
    Assert(!(uSelFS & X86_SEL_RPL)); Assert(!(uSelFS & X86_SEL_LDT));
    Assert(!(uSelGS & X86_SEL_RPL)); Assert(!(uSelGS & X86_SEL_LDT));
    Assert(!(uSelTR & X86_SEL_RPL)); Assert(!(uSelTR & X86_SEL_LDT));
    Assert(uSelCS);
    Assert(uSelTR);

    /* Write these host selector fields into the host-state area in the VMCS. */
    int rc  = VMXWriteVmcs32(VMX_VMCS16_HOST_CS_SEL, uSelCS);
    rc     |= VMXWriteVmcs32(VMX_VMCS16_HOST_SS_SEL, uSelSS);
#if HC_ARCH_BITS == 64
    rc     |= VMXWriteVmcs32(VMX_VMCS16_HOST_DS_SEL, uSelDS);
    rc     |= VMXWriteVmcs32(VMX_VMCS16_HOST_ES_SEL, uSelES);
    rc     |= VMXWriteVmcs32(VMX_VMCS16_HOST_FS_SEL, uSelFS);
    rc     |= VMXWriteVmcs32(VMX_VMCS16_HOST_GS_SEL, uSelGS);
#else
    NOREF(uSelDS);
    NOREF(uSelES);
    NOREF(uSelFS);
    NOREF(uSelGS);
#endif
    rc     |= VMXWriteVmcs32(VMX_VMCS16_HOST_TR_SEL, uSelTR);
    AssertRCReturn(rc, rc);

    /*
     * Host GDTR and IDTR.
     */
    RTGDTR Gdtr;
    RTIDTR Idtr;
    RT_ZERO(Gdtr);
    RT_ZERO(Idtr);
    ASMGetGDTR(&Gdtr);
    ASMGetIDTR(&Idtr);
    rc  = VMXWriteVmcsHstN(VMX_VMCS_HOST_GDTR_BASE, Gdtr.pGdt);
    rc |= VMXWriteVmcsHstN(VMX_VMCS_HOST_IDTR_BASE, Idtr.pIdt);
    AssertRCReturn(rc, rc);

#if HC_ARCH_BITS == 64
    /*
     * Determine if we need to manually need to restore the GDTR and IDTR limits as VT-x zaps
     * them to the maximum limit (0xffff) on every VM-exit.
     */
    if (Gdtr.cbGdt != 0xffff)
        pVCpu->hm.s.vmx.fRestoreHostFlags |= VMX_RESTORE_HOST_GDTR;

    /*
     * IDT limit is effectively capped at 0xfff. (See Intel spec. 6.14.1 "64-Bit Mode IDT" and
     * Intel spec. 6.2 "Exception and Interrupt Vectors".)  Therefore if the host has the limit
     * as 0xfff, VT-x bloating the limit to 0xffff shouldn't cause any different CPU behavior.
     * However, several hosts either insists on 0xfff being the limit (Windows Patch Guard) or
     * uses the limit for other purposes (darwin puts the CPU ID in there but botches sidt
     * alignment in at least one consumer).  So, we're only allowing the IDTR.LIMIT to be left
     * at 0xffff on hosts where we are sure it won't cause trouble.
     */
# if defined(RT_OS_LINUX) || defined(RT_OS_SOLARIS)
    if (Idtr.cbIdt <  0x0fff)
# else
    if (Idtr.cbIdt != 0xffff)
# endif
    {
        pVCpu->hm.s.vmx.fRestoreHostFlags |= VMX_RESTORE_HOST_IDTR;
        AssertCompile(sizeof(Idtr) == sizeof(X86XDTR64));
        memcpy(&pVCpu->hm.s.vmx.RestoreHost.HostIdtr, &Idtr, sizeof(X86XDTR64));
    }
#endif

    /*
     * Host TR base. Verify that TR selector doesn't point past the GDT. Masking off the TI
     * and RPL bits is effectively what the CPU does for "scaling by 8". TI is always 0 and
     * RPL should be too in most cases.
     */
    AssertMsgReturn((uSelTR | X86_SEL_RPL_LDT) <= Gdtr.cbGdt,
                    ("TR selector exceeds limit. TR=%RTsel cbGdt=%#x\n", uSelTR, Gdtr.cbGdt), VERR_VMX_INVALID_HOST_STATE);

    PCX86DESCHC pDesc = (PCX86DESCHC)(Gdtr.pGdt + (uSelTR & X86_SEL_MASK));
#if HC_ARCH_BITS == 64
    uintptr_t const uTRBase = X86DESC64_BASE(pDesc);

    /*
     * VT-x unconditionally restores the TR limit to 0x67 and type to 11 (32-bit busy TSS) on
     * all VM-exits. The type is the same for 64-bit busy TSS[1]. The limit needs manual
     * restoration if the host has something else. Task switching is not supported in 64-bit
     * mode[2], but the limit still matters as IOPM is supported in 64-bit mode. Restoring the
     * limit lazily while returning to ring-3 is safe because IOPM is not applicable in ring-0.
     *
     * [1] See Intel spec. 3.5 "System Descriptor Types".
     * [2] See Intel spec. 7.2.3 "TSS Descriptor in 64-bit mode".
     */
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    Assert(pDesc->System.u4Type == 11);
    if (   pDesc->System.u16LimitLow != 0x67
        || pDesc->System.u4LimitHigh)
    {
        pVCpu->hm.s.vmx.fRestoreHostFlags |= VMX_RESTORE_HOST_SEL_TR;
        /* If the host has made GDT read-only, we would need to temporarily toggle CR0.WP before writing the GDT. */
        if (pVM->hm.s.fHostKernelFeatures & SUPKERNELFEATURES_GDT_READ_ONLY)
            pVCpu->hm.s.vmx.fRestoreHostFlags |= VMX_RESTORE_HOST_GDT_READ_ONLY;
        pVCpu->hm.s.vmx.RestoreHost.uHostSelTR = uSelTR;
    }

    /*
     * Store the GDTR as we need it when restoring the GDT and while restoring the TR.
     */
    if (pVCpu->hm.s.vmx.fRestoreHostFlags & (VMX_RESTORE_HOST_GDTR | VMX_RESTORE_HOST_SEL_TR))
    {
        AssertCompile(sizeof(Gdtr) == sizeof(X86XDTR64));
        memcpy(&pVCpu->hm.s.vmx.RestoreHost.HostGdtr, &Gdtr, sizeof(X86XDTR64));
        if (pVM->hm.s.fHostKernelFeatures & SUPKERNELFEATURES_GDT_NEED_WRITABLE)
        {
            /* The GDT is read-only but the writable GDT is available. */
            pVCpu->hm.s.vmx.fRestoreHostFlags |= VMX_RESTORE_HOST_GDT_NEED_WRITABLE;
            pVCpu->hm.s.vmx.RestoreHost.HostGdtrRw.cb = Gdtr.cbGdt;
            rc = SUPR0GetCurrentGdtRw(&pVCpu->hm.s.vmx.RestoreHost.HostGdtrRw.uAddr);
            AssertRCReturn(rc, rc);
        }
    }
#else
    uintptr_t const uTRBase = X86DESC_BASE(pDesc);
#endif
    rc = VMXWriteVmcsHstN(VMX_VMCS_HOST_TR_BASE, uTRBase);
    AssertRCReturn(rc, rc);

    /*
     * Host FS base and GS base.
     */
#if HC_ARCH_BITS == 64
    uint64_t const u64FSBase = ASMRdMsr(MSR_K8_FS_BASE);
    uint64_t const u64GSBase = ASMRdMsr(MSR_K8_GS_BASE);
    rc  = VMXWriteVmcs64(VMX_VMCS_HOST_FS_BASE, u64FSBase);
    rc |= VMXWriteVmcs64(VMX_VMCS_HOST_GS_BASE, u64GSBase);
    AssertRCReturn(rc, rc);

    /* Store the base if we have to restore FS or GS manually as we need to restore the base as well. */
    if (pVCpu->hm.s.vmx.fRestoreHostFlags & VMX_RESTORE_HOST_SEL_FS)
        pVCpu->hm.s.vmx.RestoreHost.uHostFSBase = u64FSBase;
    if (pVCpu->hm.s.vmx.fRestoreHostFlags & VMX_RESTORE_HOST_SEL_GS)
        pVCpu->hm.s.vmx.RestoreHost.uHostGSBase = u64GSBase;
#endif
    return VINF_SUCCESS;
}


/**
 * Exports certain host MSRs in the VM-exit MSR-load area and some in the
 * host-state area of the VMCS.
 *
 * These MSRs will be automatically restored on the host after every successful
 * VM-exit.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 *
 * @remarks No-long-jump zone!!!
 */
static int hmR0VmxExportHostMsrs(PVMCPU pVCpu)
{
    AssertPtr(pVCpu);

    /*
     * Save MSRs that we restore lazily (due to preemption or transition to ring-3)
     * rather than swapping them on every VM-entry.
     */
    hmR0VmxLazySaveHostMsrs(pVCpu);

    /*
     * Host Sysenter MSRs.
     */
    int rc = VMXWriteVmcs32(VMX_VMCS32_HOST_SYSENTER_CS, ASMRdMsr_Low(MSR_IA32_SYSENTER_CS));
#if HC_ARCH_BITS == 32
    rc    |= VMXWriteVmcs32(VMX_VMCS_HOST_SYSENTER_ESP,  ASMRdMsr_Low(MSR_IA32_SYSENTER_ESP));
    rc    |= VMXWriteVmcs32(VMX_VMCS_HOST_SYSENTER_EIP,  ASMRdMsr_Low(MSR_IA32_SYSENTER_EIP));
#else
    rc    |= VMXWriteVmcs64(VMX_VMCS_HOST_SYSENTER_ESP,  ASMRdMsr(MSR_IA32_SYSENTER_ESP));
    rc    |= VMXWriteVmcs64(VMX_VMCS_HOST_SYSENTER_EIP,  ASMRdMsr(MSR_IA32_SYSENTER_EIP));
#endif
    AssertRCReturn(rc, rc);

    /*
     * Host EFER MSR.
     *
     * If the CPU supports the newer VMCS controls for managing EFER, use it. Otherwise it's
     * done as part of auto-load/store MSR area in the VMCS, see hmR0VmxExportGuestMsrs().
     */
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    if (pVM->hm.s.vmx.fSupportsVmcsEfer)
    {
        rc = VMXWriteVmcs64(VMX_VMCS64_HOST_EFER_FULL, pVM->hm.s.vmx.u64HostMsrEfer);
        AssertRCReturn(rc, rc);
    }

    /** @todo IA32_PERF_GLOBALCTRL, IA32_PAT also see
     *        hmR0VmxExportGuestEntryExitCtls(). */

    return VINF_SUCCESS;
}


/**
 * Figures out if we need to swap the EFER MSR which is particularly expensive.
 *
 * We check all relevant bits. For now, that's everything besides LMA/LME, as
 * these two bits are handled by VM-entry, see hmR0VMxExportGuestEntryExitCtls().
 *
 * @returns true if we need to load guest EFER, false otherwise.
 * @param   pVCpu       The cross context virtual CPU structure.
 *
 * @remarks Requires EFER, CR4.
 * @remarks No-long-jump zone!!!
 */
static bool hmR0VmxShouldSwapEferMsr(PCVMCPU pVCpu)
{
#ifdef HMVMX_ALWAYS_SWAP_EFER
    RT_NOREF(pVCpu);
    return true;
#else
    PCCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
#if HC_ARCH_BITS == 32 && defined(VBOX_ENABLE_64_BITS_GUESTS)
    /* For 32-bit hosts running 64-bit guests, we always swap EFER MSR in the world-switcher. Nothing to do here. */
    if (CPUMIsGuestInLongModeEx(pCtx))
        return false;
#endif

    PVM pVM = pVCpu->CTX_SUFF(pVM);
    uint64_t const u64HostEfer  = pVM->hm.s.vmx.u64HostMsrEfer;
    uint64_t const u64GuestEfer = pCtx->msrEFER;

    /*
     * For 64-bit guests, if EFER.SCE bit differs, we need to swap the EFER MSR
     * to ensure that the guest's SYSCALL behaviour isn't broken, see @bugref{7386}.
     */
    if (   CPUMIsGuestInLongModeEx(pCtx)
        && (u64GuestEfer & MSR_K6_EFER_SCE) != (u64HostEfer & MSR_K6_EFER_SCE))
        return true;

    /*
     * If the guest uses PAE and EFER.NXE bit differs, we need to swap the EFER MSR
     * as it affects guest paging. 64-bit paging implies CR4.PAE as well.
     *
     * See Intel spec. 4.5 "IA-32e Paging".
     * See Intel spec. 4.1.1 "Three Paging Modes".
     *
     * Verify that we always intercept CR4.PAE and CR0.PG bits, so we don't need to
     * import CR4 and CR0 from the VMCS here as those bits are always up to date.
     */
    Assert(hmR0VmxGetFixedCr4Mask(pVCpu) & X86_CR4_PAE);
    Assert(hmR0VmxGetFixedCr0Mask(pVCpu) & X86_CR0_PG);
    if (   (pCtx->cr4 & X86_CR4_PAE)
        && (pCtx->cr0 & X86_CR0_PG)
        && (u64GuestEfer & MSR_K6_EFER_NXE) != (u64HostEfer & MSR_K6_EFER_NXE))
    {
        /* Assert that host is NX capable. */
        Assert(pVCpu->CTX_SUFF(pVM)->cpum.ro.HostFeatures.fNoExecute);
        return true;
    }

    return false;
#endif
}

/**
 * Exports the guest state with appropriate VM-entry and VM-exit controls in the
 * VMCS.
 *
 * This is typically required when the guest changes paging mode.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks Requires EFER.
 * @remarks No-long-jump zone!!!
 */
static int hmR0VmxExportGuestEntryExitCtls(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_VMX_ENTRY_EXIT_CTLS)
    {
        PVM          pVM = pVCpu->CTX_SUFF(pVM);
        PVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;

        /*
         * VM-entry controls.
         */
        {
            uint32_t       fVal = pVM->hm.s.vmx.Msrs.EntryCtls.n.allowed0;    /* Bits set here must be set in the VMCS. */
            uint32_t const fZap = pVM->hm.s.vmx.Msrs.EntryCtls.n.allowed1;    /* Bits cleared here must be cleared in the VMCS. */

            /*
             * Load the guest debug controls (DR7 and IA32_DEBUGCTL MSR) on VM-entry.
             * The first VT-x capable CPUs only supported the 1-setting of this bit.
             *
             * For nested-guests, this is a mandatory VM-entry control. It's also
             * required because we do not want to leak host bits to the nested-guest.
             */
            fVal |= VMX_ENTRY_CTLS_LOAD_DEBUG;

            /*
             * Set if the guest is in long mode. This will set/clear the EFER.LMA bit on VM-entry.
             *
             * For nested-guests, the "IA-32e mode guest" control we initialize with what is
             * required to get the nested-guest working with hardware-assisted VMX execution.
             * It depends on the nested-guest's IA32_EFER.LMA bit. Remember, a guest hypervisor
             * can skip intercepting changes to the EFER MSR. This is why it it needs to be done
             * here rather than while merging the guest VMCS controls.
             */
            if (CPUMIsGuestInLongModeEx(&pVCpu->cpum.GstCtx))
                fVal |= VMX_ENTRY_CTLS_IA32E_MODE_GUEST;
            else
                Assert(!(fVal & VMX_ENTRY_CTLS_IA32E_MODE_GUEST));

            /*
             * If the CPU supports the newer VMCS controls for managing guest/host EFER, use it.
             *
             * For nested-guests, we use the "load IA32_EFER" if the hardware supports it,
             * regardless of whether the nested-guest VMCS specifies it because we are free to
             * load whatever MSRs we require and we do not need to modify the guest visible copy
             * of the VM-entry MSR load area.
             */
            if (   pVM->hm.s.vmx.fSupportsVmcsEfer
                && hmR0VmxShouldSwapEferMsr(pVCpu))
                fVal |= VMX_ENTRY_CTLS_LOAD_EFER_MSR;
            else
                Assert(!(fVal & VMX_ENTRY_CTLS_LOAD_EFER_MSR));

            /*
             * The following should -not- be set (since we're not in SMM mode):
             * - VMX_ENTRY_CTLS_ENTRY_TO_SMM
             * - VMX_ENTRY_CTLS_DEACTIVATE_DUAL_MON
             */

            /** @todo VMX_ENTRY_CTLS_LOAD_PERF_MSR,
             *        VMX_ENTRY_CTLS_LOAD_PAT_MSR. */

            if ((fVal & fZap) == fVal)
            { /* likely */ }
            else
            {
                Log4Func(("Invalid VM-entry controls combo! Cpu=%#RX32 fVal=%#RX32 fZap=%#RX32\n",
                          pVM->hm.s.vmx.Msrs.EntryCtls.n.allowed0, fVal, fZap));
                pVCpu->hm.s.u32HMError = VMX_UFC_CTRL_ENTRY;
                return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
            }

            /* Commit it to the VMCS. */
            if (pVmcsInfo->u32EntryCtls != fVal)
            {
                int rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_ENTRY, fVal);
                AssertRCReturn(rc, rc);
                pVmcsInfo->u32EntryCtls = fVal;
            }
        }

        /*
         * VM-exit controls.
         */
        {
            uint32_t       fVal = pVM->hm.s.vmx.Msrs.ExitCtls.n.allowed0;     /* Bits set here must be set in the VMCS. */
            uint32_t const fZap = pVM->hm.s.vmx.Msrs.ExitCtls.n.allowed1;     /* Bits cleared here must be cleared in the VMCS. */

            /*
             * Save debug controls (DR7 & IA32_DEBUGCTL_MSR). The first VT-x CPUs only
             * supported the 1-setting of this bit.
             *
             * For nested-guests, we set the "save debug controls" as the converse
             * "load debug controls" is mandatory for nested-guests anyway.
             */
            fVal |= VMX_EXIT_CTLS_SAVE_DEBUG;

            /*
             * Set the host long mode active (EFER.LMA) bit (which Intel calls
             * "Host address-space size") if necessary. On VM-exit, VT-x sets both the
             * host EFER.LMA and EFER.LME bit to this value. See assertion in
             * hmR0VmxExportHostMsrs().
             *
             * For nested-guests, we always set this bit as we do not support 32-bit
             * hosts.
             */
#if HC_ARCH_BITS == 64
            fVal |= VMX_EXIT_CTLS_HOST_ADDR_SPACE_SIZE;
#else
            Assert(!pVmxTransient->fIsNestedGuest);
            Assert(   pVmcsInfo->pfnStartVM == VMXR0SwitcherStartVM64
                   || pVmcsInfo->pfnStartVM == VMXR0StartVM32);
            /* Set the host address-space size based on the switcher, not guest state. See @bugref{8432}. */
            if (pVmcsInfo->pfnStartVM == VMXR0SwitcherStartVM64)
            {
                /* The switcher returns to long mode, the EFER MSR is managed by the switcher. */
                fVal |= VMX_EXIT_CTLS_HOST_ADDR_SPACE_SIZE;
            }
            else
                Assert(!(fVal & VMX_EXIT_CTLS_HOST_ADDR_SPACE_SIZE));
#endif

            /*
             * If the VMCS EFER MSR fields are supported by the hardware, we use it.
             *
             * For nested-guests, we should use the "save IA32_EFER" control if we also
             * used the "load IA32_EFER" control while exporting VM-entry controls.
             */
            if (   pVM->hm.s.vmx.fSupportsVmcsEfer
                && hmR0VmxShouldSwapEferMsr(pVCpu))
            {
                fVal |= VMX_EXIT_CTLS_SAVE_EFER_MSR
                     |  VMX_EXIT_CTLS_LOAD_EFER_MSR;
            }

            /*
             * Enable saving of the VMX-preemption timer value on VM-exit.
             * For nested-guests, currently not exposed/used.
             */
            if (    pVM->hm.s.vmx.fUsePreemptTimer
                && (pVM->hm.s.vmx.Msrs.ExitCtls.n.allowed1 & VMX_EXIT_CTLS_SAVE_PREEMPT_TIMER))
                fVal |= VMX_EXIT_CTLS_SAVE_PREEMPT_TIMER;

            /* Don't acknowledge external interrupts on VM-exit. We want to let the host do that. */
            Assert(!(fVal & VMX_EXIT_CTLS_ACK_EXT_INT));

            /** @todo VMX_EXIT_CTLS_LOAD_PERF_MSR,
             *        VMX_EXIT_CTLS_SAVE_PAT_MSR,
             *        VMX_EXIT_CTLS_LOAD_PAT_MSR. */

            if ((fVal & fZap) == fVal)
            { /* likely */ }
            else
            {
                Log4Func(("Invalid VM-exit controls combo! cpu=%#RX32 fVal=%#RX32 fZap=%R#X32\n",
                          pVM->hm.s.vmx.Msrs.ExitCtls.n.allowed0, fVal, fZap));
                pVCpu->hm.s.u32HMError = VMX_UFC_CTRL_EXIT;
                return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
            }

            /* Commit it to the VMCS. */
            if (pVmcsInfo->u32ExitCtls != fVal)
            {
                int rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_EXIT, fVal);
                AssertRCReturn(rc, rc);
                pVmcsInfo->u32ExitCtls = fVal;
            }
        }

        ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_VMX_ENTRY_EXIT_CTLS);
    }
    return VINF_SUCCESS;
}


/**
 * Sets the TPR threshold in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVCpu               The cross context virtual CPU structure.
 * @param   pVmcsInfo           The VMCS info. object.
 * @param   u32TprThreshold     The TPR threshold (task-priority class only).
 */
DECLINLINE(int) hmR0VmxApicSetTprThreshold(PVMCPU pVCpu, PVMXVMCSINFO pVmcsInfo, uint32_t u32TprThreshold)
{
    Assert(!(u32TprThreshold & ~VMX_TPR_THRESHOLD_MASK));         /* Bits 31:4 MBZ. */
    Assert(pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_USE_TPR_SHADOW);
    RT_NOREF2(pVCpu, pVmcsInfo);
    return VMXWriteVmcs32(VMX_VMCS32_CTRL_TPR_THRESHOLD, u32TprThreshold);
}


/**
 * Exports the guest APIC TPR state into the VMCS.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
static int hmR0VmxExportGuestApicTpr(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_APIC_TPR)
    {
        HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_APIC_TPR);

        PVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
        if (!pVmxTransient->fIsNestedGuest)
        {
            if (   PDMHasApic(pVCpu->CTX_SUFF(pVM))
                && APICIsEnabled(pVCpu))
            {
                /*
                 * Setup TPR shadowing.
                 */
                if (pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_USE_TPR_SHADOW)
                {
                    bool    fPendingIntr  = false;
                    uint8_t u8Tpr         = 0;
                    uint8_t u8PendingIntr = 0;
                    int rc = APICGetTpr(pVCpu, &u8Tpr, &fPendingIntr, &u8PendingIntr);
                    AssertRCReturn(rc, rc);

                    /*
                     * If there are interrupts pending but masked by the TPR, instruct VT-x to
                     * cause a TPR-below-threshold VM-exit when the guest lowers its TPR below the
                     * priority of the pending interrupt so we can deliver the interrupt. If there
                     * are no interrupts pending, set threshold to 0 to not cause any
                     * TPR-below-threshold VM-exits.
                     */
                    Assert(pVmcsInfo->pbVirtApic);
                    pVmcsInfo->pbVirtApic[XAPIC_OFF_TPR] = u8Tpr;
                    uint32_t u32TprThreshold = 0;
                    if (fPendingIntr)
                    {
                        /* Bits 3:0 of the TPR threshold field correspond to bits 7:4 of the TPR
                           (which is the Task-Priority Class). */
                        const uint8_t u8PendingPriority = u8PendingIntr >> 4;
                        const uint8_t u8TprPriority     = u8Tpr >> 4;
                        if (u8PendingPriority <= u8TprPriority)
                            u32TprThreshold = u8PendingPriority;
                    }

                    rc = hmR0VmxApicSetTprThreshold(pVCpu, pVmcsInfo, u32TprThreshold);
                    AssertRCReturn(rc, rc);
                }
            }
        }
        /* else: the TPR threshold has already been updated while merging the nested-guest VMCS. */
        ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_GUEST_APIC_TPR);
    }
    return VINF_SUCCESS;
}


/**
 * Gets the guest interruptibility-state.
 *
 * @returns Guest's interruptibility-state.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
static uint32_t hmR0VmxGetGuestIntrState(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    /*
     * Check if we should inhibit interrupt delivery due to instructions like STI and MOV SS.
     */
    uint32_t fIntrState = 0;
    if (VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_INHIBIT_INTERRUPTS))
    {
        /* If inhibition is active, RIP and RFLAGS should've been updated
           (i.e. read previously from the VMCS or from ring-3). */
        PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
#ifdef VBOX_STRICT
        uint64_t const fExtrn = ASMAtomicUoReadU64(&pCtx->fExtrn);
        RT_UNTRUSTED_NONVOLATILE_COPY_FENCE();
        AssertMsg(!(fExtrn & (CPUMCTX_EXTRN_RIP | CPUMCTX_EXTRN_RFLAGS)), ("%#x\n", fExtrn));
#endif
        if (pCtx->rip == EMGetInhibitInterruptsPC(pVCpu))
        {
            if (pCtx->eflags.Bits.u1IF)
                fIntrState = VMX_VMCS_GUEST_INT_STATE_BLOCK_STI;
            else
                fIntrState = VMX_VMCS_GUEST_INT_STATE_BLOCK_MOVSS;
        }
        else if (VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_INHIBIT_INTERRUPTS))
        {
            /*
             * We can clear the inhibit force flag as even if we go back to the recompiler
             * without executing guest code in VT-x, the flag's condition to be cleared is
             * met and thus the cleared state is correct.
             */
            VMCPU_FF_CLEAR(pVCpu, VMCPU_FF_INHIBIT_INTERRUPTS);
        }
    }

    /*
     * NMIs to the guest are blocked after an NMI is injected until the guest executes an IRET. We only
     * bother with virtual-NMI blocking when we have support for virtual NMIs in the CPU, otherwise
     * setting this would block host-NMIs and IRET will not clear the blocking.
     *
     * We always set NMI-exiting so when the host receives an NMI we get a VM-exit.
     *
     * See Intel spec. 26.6.1 "Interruptibility state". See @bugref{7445}.
     */
    if (   hmR0VmxIsPinCtlsSet(pVCpu, pVmxTransient, VMX_PIN_CTLS_VIRT_NMI)
        && CPUMIsGuestNmiBlocking(pVCpu))
        fIntrState |= VMX_VMCS_GUEST_INT_STATE_BLOCK_NMI;

    return fIntrState;
}


/**
 * Exports the exception intercepts required for guest execution in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
static int hmR0VmxExportGuestXcptIntercepts(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_VMX_XCPT_INTERCEPTS)
    {
        /* When executing a nested-guest, we do not need to trap GIM hypercalls by intercepting #UD. */
        if (   !pVmxTransient->fIsNestedGuest
            &&  pVCpu->hm.s.fGIMTrapXcptUD)
            hmR0VmxAddXcptIntercept(pVmxTransient, X86_XCPT_UD);
        else
            hmR0VmxRemoveXcptIntercept(pVCpu, pVmxTransient, X86_XCPT_UD);

        /* Other exception intercepts are handled elsewhere, e.g. while exporting guest CR0. */
        ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_VMX_XCPT_INTERCEPTS);
    }
    return VINF_SUCCESS;
}


/**
 * Exports the guest's RIP into the guest-state area in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 *
 * @remarks No-long-jump zone!!!
 */
static int hmR0VmxExportGuestRip(PVMCPU pVCpu)
{
    if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_RIP)
    {
        HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_RIP);

        int rc = VMXWriteVmcsGstN(VMX_VMCS_GUEST_RIP, pVCpu->cpum.GstCtx.rip);
        AssertRCReturn(rc, rc);

        ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_GUEST_RIP);
        Log4Func(("rip=%#RX64\n", pVCpu->cpum.GstCtx.rip));
    }
    return VINF_SUCCESS;
}


/**
 * Exports the guest's RSP into the guest-state area in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 *
 * @remarks No-long-jump zone!!!
 */
static int hmR0VmxExportGuestRsp(PVMCPU pVCpu)
{
    if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_RSP)
    {
        HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_RSP);

        int rc = VMXWriteVmcsGstN(VMX_VMCS_GUEST_RSP, pVCpu->cpum.GstCtx.rsp);
        AssertRCReturn(rc, rc);

        ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_GUEST_RSP);
    }
    return VINF_SUCCESS;
}


/**
 * Exports the guest's RFLAGS into the guest-state area in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
static int hmR0VmxExportGuestRflags(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_RFLAGS)
    {
        HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_RFLAGS);

        /* Intel spec. 2.3.1 "System Flags and Fields in IA-32e Mode" claims the upper 32-bits of RFLAGS are reserved (MBZ).
           Let us assert it as such and use 32-bit VMWRITE. */
        Assert(!RT_HI_U32(pVCpu->cpum.GstCtx.rflags.u64));
        X86EFLAGS fEFlags = pVCpu->cpum.GstCtx.eflags;
        Assert(fEFlags.u32 & X86_EFL_RA1_MASK);
        Assert(!(fEFlags.u32 & ~(X86_EFL_1 | X86_EFL_LIVE_MASK)));

        /*
         * If we're emulating real-mode using Virtual 8086 mode, save the real-mode eflags so
         * we can restore them on VM-exit. Modify the real-mode guest's eflags so that VT-x
         * can run the real-mode guest code under Virtual 8086 mode.
         */
        PVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
        if (pVmcsInfo->RealMode.fRealOnV86Active)
        {
            Assert(pVCpu->CTX_SUFF(pVM)->hm.s.vmx.pRealModeTSS);
            Assert(PDMVmmDevHeapIsEnabled(pVCpu->CTX_SUFF(pVM)));
            Assert(!pVmxTransient->fIsNestedGuest);
            pVmcsInfo->RealMode.Eflags.u32 = fEFlags.u32;    /* Save the original eflags of the real-mode guest. */
            fEFlags.Bits.u1VM   = 1;                         /* Set the Virtual 8086 mode bit. */
            fEFlags.Bits.u2IOPL = 0;                         /* Change IOPL to 0, otherwise certain instructions won't fault. */
        }

        int rc = VMXWriteVmcs32(VMX_VMCS_GUEST_RFLAGS, fEFlags.u32);
        AssertRCReturn(rc, rc);

        /*
         * Setup pending debug exceptions if the guest is single-stepping using EFLAGS.TF.
         *
         * We must avoid setting any automatic debug exceptions delivery when single-stepping
         * through the hypervisor debugger using EFLAGS.TF.
         */
        if (   !pVmxTransient->fIsNestedGuest
            && !pVCpu->hm.s.fSingleInstruction
            &&  fEFlags.Bits.u1TF)
        {
            /** @todo r=ramshankar: Warning!! We ASSUME EFLAGS.TF will not cleared on
             *        premature trips to ring-3 esp since IEM does not yet handle it. */
            rc = VMXWriteVmcs32(VMX_VMCS_GUEST_PENDING_DEBUG_XCPTS, VMX_VMCS_GUEST_PENDING_DEBUG_XCPT_BS);
            AssertRCReturn(rc, rc);
        }
        /** @todo NSTVMX: Handling copying of VMX_VMCS_GUEST_PENDING_DEBUG_XCPTS from
         *        nested-guest VMCS. */

        ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_GUEST_RFLAGS);
        Log4Func(("EFlags=%#RX32\n", fEFlags.u32));
    }
    return VINF_SUCCESS;
}


/**
 * Exports the guest CR0 control register into the guest-state area in the VMCS.
 *
 * The guest FPU state is always pre-loaded hence we don't need to bother about
 * sharing FPU related CR0 bits between the guest and host.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
static int hmR0VmxExportGuestCR0(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_CR0)
    {
        PVM          pVM       = pVCpu->CTX_SUFF(pVM);
        PVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;

        /*
         * Figure out fixed CR0 bits in VMX operation.
         */
        /** @todo Why do we need to OR and AND the fixed-0 and fixed-1 bits below? */
        uint64_t       fSetCr0 = pVM->hm.s.vmx.Msrs.u64Cr0Fixed0 & pVM->hm.s.vmx.Msrs.u64Cr0Fixed1;
        uint64_t const fZapCr0 = pVM->hm.s.vmx.Msrs.u64Cr0Fixed0 | pVM->hm.s.vmx.Msrs.u64Cr0Fixed1;
        if (pVM->hm.s.vmx.fUnrestrictedGuest)
            fSetCr0 &= ~(uint64_t)(X86_CR0_PE | X86_CR0_PG);
        else
            Assert((fSetCr0 & (X86_CR0_PE | X86_CR0_PG)) == (X86_CR0_PE | X86_CR0_PG));

        if (!pVmxTransient->fIsNestedGuest)
        {
            HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_CR0);
            uint64_t       u64GuestCr0  = pVCpu->cpum.GstCtx.cr0;
            uint64_t const u64ShadowCr0 = u64GuestCr0;
            Assert(!RT_HI_U32(u64GuestCr0));

            /*
             * Setup VT-x's view of the guest CR0.
             */
            uint32_t uProcCtls = pVmcsInfo->u32ProcCtls;
            if (pVM->hm.s.fNestedPaging)
            {
                if (CPUMIsGuestPagingEnabled(pVCpu))
                {
                    /* The guest has paging enabled, let it access CR3 without causing a VM-exit if supported. */
                    uProcCtls &= ~(  VMX_PROC_CTLS_CR3_LOAD_EXIT
                                   | VMX_PROC_CTLS_CR3_STORE_EXIT);
                }
                else
                {
                    /* The guest doesn't have paging enabled, make CR3 access cause a VM-exit to update our shadow. */
                    uProcCtls |= VMX_PROC_CTLS_CR3_LOAD_EXIT
                              |  VMX_PROC_CTLS_CR3_STORE_EXIT;
                }

                /* If we have unrestricted guest execution, we never have to intercept CR3 reads. */
                if (pVM->hm.s.vmx.fUnrestrictedGuest)
                    uProcCtls &= ~VMX_PROC_CTLS_CR3_STORE_EXIT;
            }
            else
            {
                /* Guest CPL 0 writes to its read-only pages should cause a #PF VM-exit. */
                u64GuestCr0 |= X86_CR0_WP;
            }

            /*
             * Guest FPU bits.
             *
             * Since we pre-load the guest FPU always before VM-entry there is no need to track lazy state
             * using CR0.TS.
             *
             * Intel spec. 23.8 "Restrictions on VMX operation" mentions that CR0.NE bit must always be
             * set on the first CPUs to support VT-x and no mention of with regards to UX in VM-entry checks.
             */
            u64GuestCr0 |= X86_CR0_NE;

            /* If CR0.NE isn't set, we need to intercept #MF exceptions and report them to the guest differently. */
            bool const fInterceptMF = !(u64ShadowCr0 & X86_CR0_NE);

            /*
             * Update exception intercepts.
             */
            uint32_t uXcptBitmap = pVmcsInfo->u32XcptBitmap;
            if (pVmcsInfo->RealMode.fRealOnV86Active)
            {
                Assert(PDMVmmDevHeapIsEnabled(pVM));
                Assert(pVM->hm.s.vmx.pRealModeTSS);
                uXcptBitmap |= HMVMX_REAL_MODE_XCPT_MASK;
            }
            else
            {
                /* For now, cleared here as mode-switches can happen outside HM/VT-x. See @bugref{7626#c11}. */
                uXcptBitmap &= ~HMVMX_REAL_MODE_XCPT_MASK;
                if (fInterceptMF)
                    uXcptBitmap |= RT_BIT(X86_XCPT_MF);
            }

            /* Additional intercepts for debugging, define these yourself explicitly. */
#ifdef HMVMX_ALWAYS_TRAP_ALL_XCPTS
            uXcptBitmap |= 0
                        |  RT_BIT(X86_XCPT_BP)
                        |  RT_BIT(X86_XCPT_DE)
                        |  RT_BIT(X86_XCPT_NM)
                        |  RT_BIT(X86_XCPT_TS)
                        |  RT_BIT(X86_XCPT_UD)
                        |  RT_BIT(X86_XCPT_NP)
                        |  RT_BIT(X86_XCPT_SS)
                        |  RT_BIT(X86_XCPT_GP)
                        |  RT_BIT(X86_XCPT_PF)
                        |  RT_BIT(X86_XCPT_MF)
                        ;
#elif defined(HMVMX_ALWAYS_TRAP_PF)
            uXcptBitmap |= RT_BIT(X86_XCPT_PF);
#endif
            if (pVCpu->hm.s.fTrapXcptGpForLovelyMesaDrv)
                uXcptBitmap |= RT_BIT(X86_XCPT_GP);
            Assert(pVM->hm.s.fNestedPaging || (uXcptBitmap & RT_BIT(X86_XCPT_PF)));

            /* Apply the hardware specified fixed CR0 bits and enable caching. */
            u64GuestCr0 |= fSetCr0;
            u64GuestCr0 &= fZapCr0;
            u64GuestCr0 &= ~(uint64_t)(X86_CR0_CD | X86_CR0_NW);

            /* Commit the CR0 and related fields to the guest VMCS. */
            int rc = VMXWriteVmcs32(VMX_VMCS_GUEST_CR0, u64GuestCr0);   /** @todo Fix to 64-bit when we drop 32-bit. */
            rc    |= VMXWriteVmcsHstN(VMX_VMCS_CTRL_CR0_READ_SHADOW, u64ShadowCr0);
            if (uProcCtls != pVmcsInfo->u32ProcCtls)
                rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC, uProcCtls);
            if (uXcptBitmap != pVmcsInfo->u32XcptBitmap)
                rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_EXCEPTION_BITMAP, uXcptBitmap);
            AssertRCReturn(rc, rc);

            /* Update our caches. */
            pVmcsInfo->u32ProcCtls   = uProcCtls;
            pVmcsInfo->u32XcptBitmap = uXcptBitmap;

            Log4Func(("cr0=%#RX64 shadow=%#RX64 set=%#RX64 zap=%#RX64\n", u64GuestCr0, u64ShadowCr0, fSetCr0, fZapCr0));
        }
        else
        {
            /*
             * With nested-guests, we may have extended the guest/host mask here since we
             * merged in the outer guest's mask. Thus, the merged mask can include more bits
             * (to read from the nested-guest CR0 read-shadow) than the guest hypervisor
             * originally supplied. We must copy those bits from the nested-guest CR0 into
             * the nested-guest CR0 read-shadow.
             */
            HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_CR0);
            uint64_t       u64GuestCr0  = pVCpu->cpum.GstCtx.cr0;
            uint64_t const u64ShadowCr0 = CPUMGetGuestVmxMaskedCr0(pVCpu, &pVCpu->cpum.GstCtx, pVmcsInfo->u64Cr0Mask);
            Assert(!RT_HI_U32(u64GuestCr0));
            Assert(u64GuestCr0 & X86_CR0_NE);

            /*
             * Apply the hardware specified fixed CR0 bits and enable caching.
             * Note! We could be altering our VMX emulation's fixed bits. We thus
             *       need to re-apply them while importing CR0.
             */
            u64GuestCr0 |= fSetCr0;
            u64GuestCr0 &= fZapCr0;
            u64GuestCr0 &= ~(uint64_t)(X86_CR0_CD | X86_CR0_NW);

            /* Commit the CR0 and CR0 read-shadow to the nested-guest VMCS. */
            /** @todo NSTVMX: Fix to 64-bit when we drop 32-bit. */
            int rc = VMXWriteVmcs32(VMX_VMCS_GUEST_CR0,              u64GuestCr0);
            rc    |= VMXWriteVmcsHstN(VMX_VMCS_CTRL_CR0_READ_SHADOW, u64ShadowCr0);
            AssertRCReturn(rc, rc);

            Log4Func(("cr0=%#RX64 shadow=%#RX64 set=%#RX64 zap=%#RX64\n", u64GuestCr0, u64ShadowCr0, fSetCr0, fZapCr0));
        }

        ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_GUEST_CR0);
    }

    return VINF_SUCCESS;
}


/**
 * Exports the guest control registers (CR3, CR4) into the guest-state area
 * in the VMCS.
 *
 * @returns VBox strict status code.
 * @retval  VINF_EM_RESCHEDULE_REM if we try to emulate non-paged guest code
 *          without unrestricted guest access and the VMMDev is not presently
 *          mapped (e.g. EFI32).
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
static VBOXSTRICTRC hmR0VmxExportGuestCR3AndCR4(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    int rc  = VINF_SUCCESS;
    PVM pVM = pVCpu->CTX_SUFF(pVM);

    /*
     * Guest CR2.
     * It's always loaded in the assembler code. Nothing to do here.
     */

    /*
     * Guest CR3.
     */
    if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_CR3)
    {
        HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_CR3);

        RTGCPHYS GCPhysGuestCr3 = NIL_RTGCPHYS;
        if (pVM->hm.s.fNestedPaging)
        {
            PVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
            pVmcsInfo->HCPhysEPTP = PGMGetHyperCR3(pVCpu);

            /* Validate. See Intel spec. 28.2.2 "EPT Translation Mechanism" and 24.6.11 "Extended-Page-Table Pointer (EPTP)" */
            Assert(pVmcsInfo->HCPhysEPTP != NIL_RTHCPHYS);
            Assert(!(pVmcsInfo->HCPhysEPTP & UINT64_C(0xfff0000000000000)));
            Assert(!(pVmcsInfo->HCPhysEPTP & 0xfff));

            /* VMX_EPT_MEMTYPE_WB support is already checked in hmR0VmxSetupTaggedTlb(). */
            pVmcsInfo->HCPhysEPTP |= VMX_EPT_MEMTYPE_WB
                                  |  (VMX_EPT_PAGE_WALK_LENGTH_DEFAULT << VMX_EPT_PAGE_WALK_LENGTH_SHIFT);

            /* Validate. See Intel spec. 26.2.1 "Checks on VMX Controls" */
            AssertMsg(   ((pVmcsInfo->HCPhysEPTP >> 3) & 0x07) == 3      /* Bits 3:5 (EPT page walk length - 1) must be 3. */
                      && ((pVmcsInfo->HCPhysEPTP >> 7) & 0x1f) == 0,     /* Bits 7:11 MBZ. */
                         ("EPTP %#RX64\n", pVmcsInfo->HCPhysEPTP));
            AssertMsg(  !((pVmcsInfo->HCPhysEPTP >> 6) & 0x01)           /* Bit 6 (EPT accessed & dirty bit). */
                      || (pVM->hm.s.vmx.Msrs.u64EptVpidCaps & MSR_IA32_VMX_EPT_VPID_CAP_EPT_ACCESS_DIRTY),
                         ("EPTP accessed/dirty bit not supported by CPU but set %#RX64\n", pVmcsInfo->HCPhysEPTP));

            rc = VMXWriteVmcs64(VMX_VMCS64_CTRL_EPTP_FULL, pVmcsInfo->HCPhysEPTP);
            AssertRCReturn(rc, rc);

            PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
            if (   pVM->hm.s.vmx.fUnrestrictedGuest
                || CPUMIsGuestPagingEnabledEx(pCtx))
            {
                /* If the guest is in PAE mode, pass the PDPEs to VT-x using the VMCS fields. */
                if (CPUMIsGuestInPAEModeEx(pCtx))
                {
                    rc  = PGMGstGetPaePdpes(pVCpu, &pVCpu->hm.s.aPdpes[0]);
                    AssertRCReturn(rc, rc);
                    rc  = VMXWriteVmcs64(VMX_VMCS64_GUEST_PDPTE0_FULL, pVCpu->hm.s.aPdpes[0].u);
                    rc |= VMXWriteVmcs64(VMX_VMCS64_GUEST_PDPTE1_FULL, pVCpu->hm.s.aPdpes[1].u);
                    rc |= VMXWriteVmcs64(VMX_VMCS64_GUEST_PDPTE2_FULL, pVCpu->hm.s.aPdpes[2].u);
                    rc |= VMXWriteVmcs64(VMX_VMCS64_GUEST_PDPTE3_FULL, pVCpu->hm.s.aPdpes[3].u);
                    AssertRCReturn(rc, rc);
                }

                /*
                 * The guest's view of its CR3 is unblemished with nested paging when the
                 * guest is using paging or we have unrestricted guest execution to handle
                 * the guest when it's not using paging.
                 */
                GCPhysGuestCr3 = pCtx->cr3;
            }
            else
            {
                /*
                 * The guest is not using paging, but the CPU (VT-x) has to. While the guest
                 * thinks it accesses physical memory directly, we use our identity-mapped
                 * page table to map guest-linear to guest-physical addresses. EPT takes care
                 * of translating it to host-physical addresses.
                 */
                RTGCPHYS GCPhys;
                Assert(pVM->hm.s.vmx.pNonPagingModeEPTPageTable);

                /* We obtain it here every time as the guest could have relocated this PCI region. */
                rc = PDMVmmDevHeapR3ToGCPhys(pVM, pVM->hm.s.vmx.pNonPagingModeEPTPageTable, &GCPhys);
                if (RT_SUCCESS(rc))
                { /* likely */ }
                else if (rc == VERR_PDM_DEV_HEAP_R3_TO_GCPHYS)
                {
                    Log4Func(("VERR_PDM_DEV_HEAP_R3_TO_GCPHYS -> VINF_EM_RESCHEDULE_REM\n"));
                    return VINF_EM_RESCHEDULE_REM;  /* We cannot execute now, switch to REM/IEM till the guest maps in VMMDev. */
                }
                else
                    AssertMsgFailedReturn(("%Rrc\n",  rc), rc);

                GCPhysGuestCr3 = GCPhys;
            }

            Log4Func(("u32GuestCr3=%#RGp (GstN)\n", GCPhysGuestCr3));
            rc = VMXWriteVmcsGstN(VMX_VMCS_GUEST_CR3, GCPhysGuestCr3);
            AssertRCReturn(rc, rc);
        }
        else
        {
            /* Non-nested paging case, just use the hypervisor's CR3. */
            RTHCPHYS const HCPhysGuestCr3 = PGMGetHyperCR3(pVCpu);

            Log4Func(("u32GuestCr3=%#RHv (HstN)\n", HCPhysGuestCr3));
            rc = VMXWriteVmcsHstN(VMX_VMCS_GUEST_CR3, HCPhysGuestCr3);
            AssertRCReturn(rc, rc);
        }

        ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_GUEST_CR3);
    }

    /*
     * Guest CR4.
     * ASSUMES this is done everytime we get in from ring-3! (XCR0)
     */
    if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_CR4)
    {
        PCPUMCTX     pCtx        = &pVCpu->cpum.GstCtx;
        PVMXVMCSINFO pVmcsInfo   = pVmxTransient->pVmcsInfo;

        /*
         * Figure out fixed CR4 bits in VMX operation.
         */
        /** @todo Why do we need to OR and AND the fixed-0 and fixed-1 bits below? */
        uint64_t const fSetCr4 = pVM->hm.s.vmx.Msrs.u64Cr4Fixed0 & pVM->hm.s.vmx.Msrs.u64Cr4Fixed1;
        uint64_t const fZapCr4 = pVM->hm.s.vmx.Msrs.u64Cr4Fixed0 | pVM->hm.s.vmx.Msrs.u64Cr4Fixed1;

        /*
         * With nested-guests, we may have extended the guest/host mask here (since we
         * merged in the outer guest's mask, see hmR0VmxMergeVmcsNested). This means, the
         * mask can include more bits (to read from the nested-guest CR4 read-shadow) than
         * the guest hypervisor originally supplied. Thus, we should, in essence, copy
         * those bits from the nested-guest CR4 into the nested-guest CR4 read-shadow.
         */
        HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_CR4);
        uint64_t       u64GuestCr4  = pCtx->cr4;
        uint64_t const u64ShadowCr4 = !pVmxTransient->fIsNestedGuest
                                    ? pCtx->cr4
                                    : CPUMGetGuestVmxMaskedCr4(pVCpu, pCtx, pVmcsInfo->u64Cr4Mask);
        Assert(!RT_HI_U32(u64GuestCr4));

        /*
         * Setup VT-x's view of the guest CR4.
         *
         * If we're emulating real-mode using virtual-8086 mode, we want to redirect software
         * interrupts to the 8086 program interrupt handler. Clear the VME bit (the interrupt
         * redirection bitmap is already all 0, see hmR3InitFinalizeR0())
         *
         * See Intel spec. 20.2 "Software Interrupt Handling Methods While in Virtual-8086 Mode".
         */
        if (pVmcsInfo->RealMode.fRealOnV86Active)
        {
            Assert(pVM->hm.s.vmx.pRealModeTSS);
            Assert(PDMVmmDevHeapIsEnabled(pVM));
            u64GuestCr4 &= ~(uint64_t)X86_CR4_VME;
        }

        if (pVM->hm.s.fNestedPaging)
        {
            if (   !CPUMIsGuestPagingEnabledEx(pCtx)
                && !pVM->hm.s.vmx.fUnrestrictedGuest)
            {
                /* We use 4 MB pages in our identity mapping page table when the guest doesn't have paging. */
                u64GuestCr4 |= X86_CR4_PSE;
                /* Our identity mapping is a 32-bit page directory. */
                u64GuestCr4 &= ~(uint64_t)X86_CR4_PAE;
            }
            /* else use guest CR4.*/
        }
        else
        {
            Assert(!pVmxTransient->fIsNestedGuest);

            /*
             * The shadow paging modes and guest paging modes are different, the shadow is in accordance with the host
             * paging mode and thus we need to adjust VT-x's view of CR4 depending on our shadow page tables.
             */
            switch (pVCpu->hm.s.enmShadowMode)
            {
                case PGMMODE_REAL:              /* Real-mode. */
                case PGMMODE_PROTECTED:         /* Protected mode without paging. */
                case PGMMODE_32_BIT:            /* 32-bit paging. */
                {
                    u64GuestCr4 &= ~(uint64_t)X86_CR4_PAE;
                    break;
                }

                case PGMMODE_PAE:               /* PAE paging. */
                case PGMMODE_PAE_NX:            /* PAE paging with NX. */
                {
                    u64GuestCr4 |= X86_CR4_PAE;
                    break;
                }

                case PGMMODE_AMD64:             /* 64-bit AMD paging (long mode). */
                case PGMMODE_AMD64_NX:          /* 64-bit AMD paging (long mode) with NX enabled. */
#ifdef VBOX_ENABLE_64_BITS_GUESTS
                    break;
#endif
                default:
                    AssertFailed();
                    return VERR_PGM_UNSUPPORTED_SHADOW_PAGING_MODE;
            }
        }

        /*
         * Apply the hardware specified fixed CR4 bits (mainly CR4.VMXE).
         * Note! For nested-guests, we could be altering our VMX emulation's
         *       fixed bits. We thus need to re-apply them while importing CR4.
         */
        u64GuestCr4 |= fSetCr4;
        u64GuestCr4 &= fZapCr4;

        /* Commit the CR4 and CR4 read-shadow to the guest VMCS. */
        /** @todo Fix to 64-bit when we drop 32-bit. */
        rc  = VMXWriteVmcs32(VMX_VMCS_GUEST_CR4,              u64GuestCr4);
        rc |= VMXWriteVmcsHstN(VMX_VMCS_CTRL_CR4_READ_SHADOW, u64ShadowCr4);
        AssertRCReturn(rc, rc);

        /* Whether to save/load/restore XCR0 during world switch depends on CR4.OSXSAVE and host+guest XCR0. */
        pVCpu->hm.s.fLoadSaveGuestXcr0 = (pCtx->cr4 & X86_CR4_OSXSAVE) && pCtx->aXcr[0] != ASMGetXcr0();

        ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_GUEST_CR4);

        Log4Func(("cr4=%#RX64 shadow=%#RX64 set=%#RX64 zap=%#RX64)\n", u64GuestCr4, u64ShadowCr4, fSetCr4, fZapCr4));
    }
    return rc;
}


/**
 * Exports the guest debug registers into the guest-state area in the VMCS.
 * The guest debug bits are partially shared with the host (e.g. DR6, DR0-3).
 *
 * This also sets up whether \#DB and MOV DRx accesses cause VM-exits.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
static int hmR0VmxExportSharedDebugState(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    /** @todo NSTVMX: Figure out what we want to do with nested-guest instruction
     *        stepping. */
    PVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    if (pVmxTransient->fIsNestedGuest)
    {
        int rc = VMXWriteVmcs32(VMX_VMCS_GUEST_DR7, CPUMGetGuestDR7(pVCpu));
        AssertRCReturn(rc, rc);
        return VINF_SUCCESS;
    }

#ifdef VBOX_STRICT
    /* Validate. Intel spec. 26.3.1.1 "Checks on Guest Controls Registers, Debug Registers, MSRs" */
    if (pVmcsInfo->u32EntryCtls & VMX_ENTRY_CTLS_LOAD_DEBUG)
    {
        /* Validate. Intel spec. 17.2 "Debug Registers", recompiler paranoia checks. */
        Assert((pVCpu->cpum.GstCtx.dr[7] & (X86_DR7_MBZ_MASK | X86_DR7_RAZ_MASK)) == 0);
        Assert((pVCpu->cpum.GstCtx.dr[7] & X86_DR7_RA1_MASK) == X86_DR7_RA1_MASK);
    }
#endif

    bool     fSteppingDB      = false;
    bool     fInterceptMovDRx = false;
    uint32_t uProcCtls        = pVmcsInfo->u32ProcCtls;
    if (pVCpu->hm.s.fSingleInstruction)
    {
        /* If the CPU supports the monitor trap flag, use it for single stepping in DBGF and avoid intercepting #DB. */
        PVM pVM = pVCpu->CTX_SUFF(pVM);
        if (pVM->hm.s.vmx.Msrs.ProcCtls.n.allowed1 & VMX_PROC_CTLS_MONITOR_TRAP_FLAG)
        {
            uProcCtls |= VMX_PROC_CTLS_MONITOR_TRAP_FLAG;
            Assert(fSteppingDB == false);
        }
        else
        {
            pVCpu->cpum.GstCtx.eflags.u32 |= X86_EFL_TF;
            pVCpu->hm.s.fCtxChanged |= HM_CHANGED_GUEST_RFLAGS;
            pVCpu->hm.s.fClearTrapFlag = true;
            fSteppingDB = true;
        }
    }

    uint32_t u32GuestDr7;
    if (   fSteppingDB
        || (CPUMGetHyperDR7(pVCpu) & X86_DR7_ENABLED_MASK))
    {
        /*
         * Use the combined guest and host DRx values found in the hypervisor register set
         * because the hypervisor debugger has breakpoints active or someone is single stepping
         * on the host side without a monitor trap flag.
         *
         * Note! DBGF expects a clean DR6 state before executing guest code.
         */
#if HC_ARCH_BITS == 32 && defined(VBOX_WITH_64_BITS_GUESTS)
        if (    CPUMIsGuestInLongModeEx(&pVCpu->cpum.GstCtx)
            && !CPUMIsHyperDebugStateActivePending(pVCpu))
        {
            CPUMR0LoadHyperDebugState(pVCpu, true /* include DR6 */);
            Assert(CPUMIsHyperDebugStateActivePending(pVCpu));
            Assert(!CPUMIsGuestDebugStateActivePending(pVCpu));
        }
        else
#endif
        if (!CPUMIsHyperDebugStateActive(pVCpu))
        {
            CPUMR0LoadHyperDebugState(pVCpu, true /* include DR6 */);
            Assert(CPUMIsHyperDebugStateActive(pVCpu));
            Assert(!CPUMIsGuestDebugStateActive(pVCpu));
        }

        /* Update DR7 with the hypervisor value (other DRx registers are handled by CPUM one way or another). */
        u32GuestDr7 = (uint32_t)CPUMGetHyperDR7(pVCpu);
        pVCpu->hm.s.fUsingHyperDR7 = true;
        fInterceptMovDRx = true;
    }
    else
    {
        /*
         * If the guest has enabled debug registers, we need to load them prior to
         * executing guest code so they'll trigger at the right time.
         */
        if (pVCpu->cpum.GstCtx.dr[7] & (X86_DR7_ENABLED_MASK | X86_DR7_GD))
        {
#if HC_ARCH_BITS == 32 && defined(VBOX_WITH_64_BITS_GUESTS)
            if (    CPUMIsGuestInLongModeEx(&pVCpu->cpum.GstCtx)
                && !CPUMIsGuestDebugStateActivePending(pVCpu))
            {
                CPUMR0LoadGuestDebugState(pVCpu, true /* include DR6 */);
                Assert(CPUMIsGuestDebugStateActivePending(pVCpu));
                Assert(!CPUMIsHyperDebugStateActivePending(pVCpu));
                STAM_COUNTER_INC(&pVCpu->hm.s.StatDRxArmed);
            }
            else
#endif
            if (!CPUMIsGuestDebugStateActive(pVCpu))
            {
                CPUMR0LoadGuestDebugState(pVCpu, true /* include DR6 */);
                Assert(CPUMIsGuestDebugStateActive(pVCpu));
                Assert(!CPUMIsHyperDebugStateActive(pVCpu));
                STAM_COUNTER_INC(&pVCpu->hm.s.StatDRxArmed);
            }
            Assert(!fInterceptMovDRx);
        }
        /*
         * If no debugging enabled, we'll lazy load DR0-3.  Unlike on AMD-V, we
         * must intercept #DB in order to maintain a correct DR6 guest value, and
         * because we need to intercept it to prevent nested #DBs from hanging the
         * CPU, we end up always having to intercept it. See hmR0VmxSetupVmcsXcptBitmap().
         */
#if HC_ARCH_BITS == 32 && defined(VBOX_WITH_64_BITS_GUESTS)
        else if (   !CPUMIsGuestDebugStateActivePending(pVCpu)
                 && !CPUMIsGuestDebugStateActive(pVCpu))
#else
        else if (!CPUMIsGuestDebugStateActive(pVCpu))
#endif
        {
            fInterceptMovDRx = true;
        }

        /* Update DR7 with the actual guest value. */
        u32GuestDr7 = pVCpu->cpum.GstCtx.dr[7];
        pVCpu->hm.s.fUsingHyperDR7 = false;
    }

    if (fInterceptMovDRx)
        uProcCtls |= VMX_PROC_CTLS_MOV_DR_EXIT;
    else
        uProcCtls &= ~VMX_PROC_CTLS_MOV_DR_EXIT;

    /*
     * Update the processor-based VM-execution controls with the MOV-DRx intercepts and the
     * monitor-trap flag and update our cache.
     */
    if (uProcCtls != pVmcsInfo->u32ProcCtls)
    {
        int rc2 = VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC, uProcCtls);
        AssertRCReturn(rc2, rc2);
        pVmcsInfo->u32ProcCtls = uProcCtls;
    }

    /*
     * Update guest DR7.
     */
    int rc = VMXWriteVmcs32(VMX_VMCS_GUEST_DR7, u32GuestDr7);
    AssertRCReturn(rc, rc);

    /*
     * If we have forced EFLAGS.TF to be set because we're single-stepping in the hypervisor debugger,
     * we need to clear interrupt inhibition if any as otherwise it causes a VM-entry failure.
     *
     * See Intel spec. 26.3.1.5 "Checks on Guest Non-Register State".
     */
    if (fSteppingDB)
    {
        Assert(pVCpu->hm.s.fSingleInstruction);
        Assert(pVCpu->cpum.GstCtx.eflags.Bits.u1TF);

        uint32_t fIntrState = 0;
        rc = VMXReadVmcs32(VMX_VMCS32_GUEST_INT_STATE, &fIntrState);
        AssertRCReturn(rc, rc);

        if (fIntrState & (VMX_VMCS_GUEST_INT_STATE_BLOCK_STI | VMX_VMCS_GUEST_INT_STATE_BLOCK_MOVSS))
        {
            fIntrState &= ~(VMX_VMCS_GUEST_INT_STATE_BLOCK_STI | VMX_VMCS_GUEST_INT_STATE_BLOCK_MOVSS);
            rc = VMXWriteVmcs32(VMX_VMCS32_GUEST_INT_STATE, fIntrState);
            AssertRCReturn(rc, rc);
        }
    }

    return VINF_SUCCESS;
}


#ifdef VBOX_STRICT
/**
 * Strict function to validate segment registers.
 *
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pVmcsInfo   The VMCS info. object.
 *
 * @remarks Will import guest CR0 on strict builds during validation of
 *          segments.
 */
static void hmR0VmxValidateSegmentRegs(PVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo)
{
    /*
     * Validate segment registers. See Intel spec. 26.3.1.2 "Checks on Guest Segment Registers".
     *
     * The reason we check for attribute value 0 in this function and not just the unusable bit is
     * because hmR0VmxExportGuestSegReg() only updates the VMCS' copy of the value with the
     * unusable bit and doesn't change the guest-context value.
     */
    PVM       pVM  = pVCpu->CTX_SUFF(pVM);
    PCCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
    hmR0VmxImportGuestState(pVCpu, pVmcsInfo, CPUMCTX_EXTRN_CR0);
    if (   !pVM->hm.s.vmx.fUnrestrictedGuest
        && (   !CPUMIsGuestInRealModeEx(pCtx)
            && !CPUMIsGuestInV86ModeEx(pCtx)))
    {
        /* Protected mode checks */
        /* CS */
        Assert(pCtx->cs.Attr.n.u1Present);
        Assert(!(pCtx->cs.Attr.u & 0xf00));
        Assert(!(pCtx->cs.Attr.u & 0xfffe0000));
        Assert(   (pCtx->cs.u32Limit & 0xfff) == 0xfff
               || !(pCtx->cs.Attr.n.u1Granularity));
        Assert(   !(pCtx->cs.u32Limit & 0xfff00000)
               || (pCtx->cs.Attr.n.u1Granularity));
        /* CS cannot be loaded with NULL in protected mode. */
        Assert(pCtx->cs.Attr.u && !(pCtx->cs.Attr.u & X86DESCATTR_UNUSABLE)); /** @todo is this really true even for 64-bit CS? */
        if (pCtx->cs.Attr.n.u4Type == 9 || pCtx->cs.Attr.n.u4Type == 11)
            Assert(pCtx->cs.Attr.n.u2Dpl == pCtx->ss.Attr.n.u2Dpl);
        else if (pCtx->cs.Attr.n.u4Type == 13 || pCtx->cs.Attr.n.u4Type == 15)
            Assert(pCtx->cs.Attr.n.u2Dpl <= pCtx->ss.Attr.n.u2Dpl);
        else
            AssertMsgFailed(("Invalid CS Type %#x\n", pCtx->cs.Attr.n.u2Dpl));
        /* SS */
        Assert((pCtx->ss.Sel & X86_SEL_RPL) == (pCtx->cs.Sel & X86_SEL_RPL));
        Assert(pCtx->ss.Attr.n.u2Dpl == (pCtx->ss.Sel & X86_SEL_RPL));
        if (   !(pCtx->cr0 & X86_CR0_PE)
            || pCtx->cs.Attr.n.u4Type == 3)
        {
            Assert(!pCtx->ss.Attr.n.u2Dpl);
        }
        if (pCtx->ss.Attr.u && !(pCtx->ss.Attr.u & X86DESCATTR_UNUSABLE))
        {
            Assert((pCtx->ss.Sel & X86_SEL_RPL) == (pCtx->cs.Sel & X86_SEL_RPL));
            Assert(pCtx->ss.Attr.n.u4Type == 3 || pCtx->ss.Attr.n.u4Type == 7);
            Assert(pCtx->ss.Attr.n.u1Present);
            Assert(!(pCtx->ss.Attr.u & 0xf00));
            Assert(!(pCtx->ss.Attr.u & 0xfffe0000));
            Assert(   (pCtx->ss.u32Limit & 0xfff) == 0xfff
                   || !(pCtx->ss.Attr.n.u1Granularity));
            Assert(   !(pCtx->ss.u32Limit & 0xfff00000)
                   || (pCtx->ss.Attr.n.u1Granularity));
        }
        /* DS, ES, FS, GS - only check for usable selectors, see hmR0VmxExportGuestSegReg(). */
        if (pCtx->ds.Attr.u && !(pCtx->ds.Attr.u & X86DESCATTR_UNUSABLE))
        {
            Assert(pCtx->ds.Attr.n.u4Type & X86_SEL_TYPE_ACCESSED);
            Assert(pCtx->ds.Attr.n.u1Present);
            Assert(pCtx->ds.Attr.n.u4Type > 11 || pCtx->ds.Attr.n.u2Dpl >= (pCtx->ds.Sel & X86_SEL_RPL));
            Assert(!(pCtx->ds.Attr.u & 0xf00));
            Assert(!(pCtx->ds.Attr.u & 0xfffe0000));
            Assert(   (pCtx->ds.u32Limit & 0xfff) == 0xfff
                   || !(pCtx->ds.Attr.n.u1Granularity));
            Assert(   !(pCtx->ds.u32Limit & 0xfff00000)
                   || (pCtx->ds.Attr.n.u1Granularity));
            Assert(   !(pCtx->ds.Attr.n.u4Type & X86_SEL_TYPE_CODE)
                   || (pCtx->ds.Attr.n.u4Type & X86_SEL_TYPE_READ));
        }
        if (pCtx->es.Attr.u && !(pCtx->es.Attr.u & X86DESCATTR_UNUSABLE))
        {
            Assert(pCtx->es.Attr.n.u4Type & X86_SEL_TYPE_ACCESSED);
            Assert(pCtx->es.Attr.n.u1Present);
            Assert(pCtx->es.Attr.n.u4Type > 11 || pCtx->es.Attr.n.u2Dpl >= (pCtx->es.Sel & X86_SEL_RPL));
            Assert(!(pCtx->es.Attr.u & 0xf00));
            Assert(!(pCtx->es.Attr.u & 0xfffe0000));
            Assert(   (pCtx->es.u32Limit & 0xfff) == 0xfff
                   || !(pCtx->es.Attr.n.u1Granularity));
            Assert(   !(pCtx->es.u32Limit & 0xfff00000)
                   || (pCtx->es.Attr.n.u1Granularity));
            Assert(   !(pCtx->es.Attr.n.u4Type & X86_SEL_TYPE_CODE)
                   || (pCtx->es.Attr.n.u4Type & X86_SEL_TYPE_READ));
        }
        if (pCtx->fs.Attr.u && !(pCtx->fs.Attr.u & X86DESCATTR_UNUSABLE))
        {
            Assert(pCtx->fs.Attr.n.u4Type & X86_SEL_TYPE_ACCESSED);
            Assert(pCtx->fs.Attr.n.u1Present);
            Assert(pCtx->fs.Attr.n.u4Type > 11 || pCtx->fs.Attr.n.u2Dpl >= (pCtx->fs.Sel & X86_SEL_RPL));
            Assert(!(pCtx->fs.Attr.u & 0xf00));
            Assert(!(pCtx->fs.Attr.u & 0xfffe0000));
            Assert(   (pCtx->fs.u32Limit & 0xfff) == 0xfff
                   || !(pCtx->fs.Attr.n.u1Granularity));
            Assert(   !(pCtx->fs.u32Limit & 0xfff00000)
                   || (pCtx->fs.Attr.n.u1Granularity));
            Assert(   !(pCtx->fs.Attr.n.u4Type & X86_SEL_TYPE_CODE)
                   || (pCtx->fs.Attr.n.u4Type & X86_SEL_TYPE_READ));
        }
        if (pCtx->gs.Attr.u && !(pCtx->gs.Attr.u & X86DESCATTR_UNUSABLE))
        {
            Assert(pCtx->gs.Attr.n.u4Type & X86_SEL_TYPE_ACCESSED);
            Assert(pCtx->gs.Attr.n.u1Present);
            Assert(pCtx->gs.Attr.n.u4Type > 11 || pCtx->gs.Attr.n.u2Dpl >= (pCtx->gs.Sel & X86_SEL_RPL));
            Assert(!(pCtx->gs.Attr.u & 0xf00));
            Assert(!(pCtx->gs.Attr.u & 0xfffe0000));
            Assert(   (pCtx->gs.u32Limit & 0xfff) == 0xfff
                   || !(pCtx->gs.Attr.n.u1Granularity));
            Assert(   !(pCtx->gs.u32Limit & 0xfff00000)
                   || (pCtx->gs.Attr.n.u1Granularity));
            Assert(   !(pCtx->gs.Attr.n.u4Type & X86_SEL_TYPE_CODE)
                   || (pCtx->gs.Attr.n.u4Type & X86_SEL_TYPE_READ));
        }
        /* 64-bit capable CPUs. */
# if HC_ARCH_BITS == 64
        Assert(!RT_HI_U32(pCtx->cs.u64Base));
        Assert(!pCtx->ss.Attr.u || !RT_HI_U32(pCtx->ss.u64Base));
        Assert(!pCtx->ds.Attr.u || !RT_HI_U32(pCtx->ds.u64Base));
        Assert(!pCtx->es.Attr.u || !RT_HI_U32(pCtx->es.u64Base));
# endif
    }
    else if (   CPUMIsGuestInV86ModeEx(pCtx)
             || (   CPUMIsGuestInRealModeEx(pCtx)
                 && !pVM->hm.s.vmx.fUnrestrictedGuest))
    {
        /* Real and v86 mode checks. */
        /* hmR0VmxExportGuestSegReg() writes the modified in VMCS. We want what we're feeding to VT-x. */
        uint32_t u32CSAttr, u32SSAttr, u32DSAttr, u32ESAttr, u32FSAttr, u32GSAttr;
        if (pVmcsInfo->RealMode.fRealOnV86Active)
        {
            u32CSAttr = 0xf3; u32SSAttr = 0xf3; u32DSAttr = 0xf3;
            u32ESAttr = 0xf3; u32FSAttr = 0xf3; u32GSAttr = 0xf3;
        }
        else
        {
            u32CSAttr = pCtx->cs.Attr.u; u32SSAttr = pCtx->ss.Attr.u; u32DSAttr = pCtx->ds.Attr.u;
            u32ESAttr = pCtx->es.Attr.u; u32FSAttr = pCtx->fs.Attr.u; u32GSAttr = pCtx->gs.Attr.u;
        }

        /* CS */
        AssertMsg((pCtx->cs.u64Base == (uint64_t)pCtx->cs.Sel << 4), ("CS base %#x %#x\n", pCtx->cs.u64Base, pCtx->cs.Sel));
        Assert(pCtx->cs.u32Limit == 0xffff);
        Assert(u32CSAttr == 0xf3);
        /* SS */
        Assert(pCtx->ss.u64Base == (uint64_t)pCtx->ss.Sel << 4);
        Assert(pCtx->ss.u32Limit == 0xffff);
        Assert(u32SSAttr == 0xf3);
        /* DS */
        Assert(pCtx->ds.u64Base == (uint64_t)pCtx->ds.Sel << 4);
        Assert(pCtx->ds.u32Limit == 0xffff);
        Assert(u32DSAttr == 0xf3);
        /* ES */
        Assert(pCtx->es.u64Base == (uint64_t)pCtx->es.Sel << 4);
        Assert(pCtx->es.u32Limit == 0xffff);
        Assert(u32ESAttr == 0xf3);
        /* FS */
        Assert(pCtx->fs.u64Base == (uint64_t)pCtx->fs.Sel << 4);
        Assert(pCtx->fs.u32Limit == 0xffff);
        Assert(u32FSAttr == 0xf3);
        /* GS */
        Assert(pCtx->gs.u64Base == (uint64_t)pCtx->gs.Sel << 4);
        Assert(pCtx->gs.u32Limit == 0xffff);
        Assert(u32GSAttr == 0xf3);
        /* 64-bit capable CPUs. */
# if HC_ARCH_BITS == 64
        Assert(!RT_HI_U32(pCtx->cs.u64Base));
        Assert(!u32SSAttr || !RT_HI_U32(pCtx->ss.u64Base));
        Assert(!u32DSAttr || !RT_HI_U32(pCtx->ds.u64Base));
        Assert(!u32ESAttr || !RT_HI_U32(pCtx->es.u64Base));
# endif
    }
}
#endif /* VBOX_STRICT */


/**
 * Exports a guest segment register into the guest-state area in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmcsInfo       The VMCS info. object.
 * @param   iSegReg         The segment register number (X86_SREG_XXX).
 * @param   pSelReg         Pointer to the segment selector.
 *
 * @remarks No-long-jump zone!!!
 */
static int hmR0VmxExportGuestSegReg(PVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo, uint8_t iSegReg, PCCPUMSELREG pSelReg)
{
    Assert(iSegReg < X86_SREG_COUNT);
    uint32_t const idxSel   = g_aVmcsSegSel[iSegReg];
    uint32_t const idxLimit = g_aVmcsSegLimit[iSegReg];
    uint32_t const idxBase  = g_aVmcsSegBase[iSegReg];
    uint32_t const idxAttr  = g_aVmcsSegAttr[iSegReg];

    uint32_t u32Access = pSelReg->Attr.u;
    if (pVmcsInfo->RealMode.fRealOnV86Active)
    {
        /* VT-x requires our real-using-v86 mode hack to override the segment access-right bits. */
        u32Access = 0xf3;
        Assert(pVCpu->CTX_SUFF(pVM)->hm.s.vmx.pRealModeTSS);
        Assert(PDMVmmDevHeapIsEnabled(pVCpu->CTX_SUFF(pVM)));
        RT_NOREF_PV(pVCpu);
    }
    else
    {
        /*
         * The way to differentiate between whether this is really a null selector or was just
         * a selector loaded with 0 in real-mode is using the segment attributes. A selector
         * loaded in real-mode with the value 0 is valid and usable in protected-mode and we
         * should -not- mark it as an unusable segment. Both the recompiler & VT-x ensures
         * NULL selectors loaded in protected-mode have their attribute as 0.
         */
        if (!u32Access)
            u32Access = X86DESCATTR_UNUSABLE;
    }

    /* Validate segment access rights. Refer to Intel spec. "26.3.1.2 Checks on Guest Segment Registers". */
    AssertMsg((u32Access & X86DESCATTR_UNUSABLE) || (u32Access & X86_SEL_TYPE_ACCESSED),
              ("Access bit not set for usable segment. idx=%#x sel=%#x attr %#x\n", idxBase, pSelReg, pSelReg->Attr.u));

    /*
     * Commit it to the VMCS.
     */
    int rc = VMXWriteVmcs32(idxSel,    pSelReg->Sel);
    rc    |= VMXWriteVmcs32(idxLimit,  pSelReg->u32Limit);
    rc    |= VMXWriteVmcsGstN(idxBase, pSelReg->u64Base);
    rc    |= VMXWriteVmcs32(idxAttr,   u32Access);
    AssertRCReturn(rc, rc);
    return rc;
}


/**
 * Exports the guest segment registers, GDTR, IDTR, LDTR, TR into the guest-state
 * area in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks Will import guest CR0 on strict builds during validation of
 *          segments.
 * @remarks No-long-jump zone!!!
 */
static int hmR0VmxExportGuestSegRegsXdtr(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    int          rc        = VERR_INTERNAL_ERROR_5;
    PVM          pVM       = pVCpu->CTX_SUFF(pVM);
    PCCPUMCTX    pCtx      = &pVCpu->cpum.GstCtx;
    PVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;

    /*
     * Guest Segment registers: CS, SS, DS, ES, FS, GS.
     */
    if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_SREG_MASK)
    {
#ifdef VBOX_WITH_REM
        if (!pVM->hm.s.vmx.fUnrestrictedGuest)
        {
            Assert(!pVmxTransient->fIsNestedGuest);
            Assert(pVM->hm.s.vmx.pRealModeTSS);
            AssertCompile(PGMMODE_REAL < PGMMODE_PROTECTED);
            if (   pVmcsInfo->fWasInRealMode
                && PGMGetGuestMode(pVCpu) >= PGMMODE_PROTECTED)
            {
                /* Signal that the recompiler must flush its code-cache as the guest -may- rewrite code it will later execute
                   in real-mode (e.g. OpenBSD 4.0) */
                REMFlushTBs(pVM);
                Log4Func(("Switch to protected mode detected!\n"));
                pVmcsInfo->fWasInRealMode = false;
            }
        }
#endif
        if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_CS)
        {
            HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_CS);
            if (pVmcsInfo->RealMode.fRealOnV86Active)
                pVmcsInfo->RealMode.AttrCS.u = pCtx->cs.Attr.u;
            rc = hmR0VmxExportGuestSegReg(pVCpu, pVmcsInfo, X86_SREG_CS, &pCtx->cs);
            AssertRCReturn(rc, rc);
            ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_GUEST_CS);
        }

        if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_SS)
        {
            HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_SS);
            if (pVmcsInfo->RealMode.fRealOnV86Active)
                pVmcsInfo->RealMode.AttrSS.u = pCtx->ss.Attr.u;
            rc = hmR0VmxExportGuestSegReg(pVCpu, pVmcsInfo, X86_SREG_SS, &pCtx->ss);
            AssertRCReturn(rc, rc);
            ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_GUEST_SS);
        }

        if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_DS)
        {
            HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_DS);
            if (pVmcsInfo->RealMode.fRealOnV86Active)
                pVmcsInfo->RealMode.AttrDS.u = pCtx->ds.Attr.u;
            rc = hmR0VmxExportGuestSegReg(pVCpu, pVmcsInfo, X86_SREG_DS, &pCtx->ds);
            AssertRCReturn(rc, rc);
            ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_GUEST_DS);
        }

        if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_ES)
        {
            HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_ES);
            if (pVmcsInfo->RealMode.fRealOnV86Active)
                pVmcsInfo->RealMode.AttrES.u = pCtx->es.Attr.u;
            rc = hmR0VmxExportGuestSegReg(pVCpu, pVmcsInfo, X86_SREG_ES, &pCtx->es);
            AssertRCReturn(rc, rc);
            ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_GUEST_ES);
        }

        if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_FS)
        {
            HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_FS);
            if (pVmcsInfo->RealMode.fRealOnV86Active)
                pVmcsInfo->RealMode.AttrFS.u = pCtx->fs.Attr.u;
            rc = hmR0VmxExportGuestSegReg(pVCpu, pVmcsInfo, X86_SREG_FS, &pCtx->fs);
            AssertRCReturn(rc, rc);
            ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_GUEST_FS);
        }

        if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_GS)
        {
            HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_GS);
            if (pVmcsInfo->RealMode.fRealOnV86Active)
                pVmcsInfo->RealMode.AttrGS.u = pCtx->gs.Attr.u;
            rc = hmR0VmxExportGuestSegReg(pVCpu, pVmcsInfo, X86_SREG_GS, &pCtx->gs);
            AssertRCReturn(rc, rc);
            ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_GUEST_GS);
        }

#ifdef VBOX_STRICT
        hmR0VmxValidateSegmentRegs(pVCpu, pVmcsInfo);
#endif
        Log4Func(("cs={%#04x base=%#RX64 limit=%#RX32 attr=%#RX32}\n", pCtx->cs.Sel, pCtx->cs.u64Base, pCtx->cs.u32Limit,
                  pCtx->cs.Attr.u));
    }

    /*
     * Guest TR.
     */
    if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_TR)
    {
        HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_TR);

        /*
         * Real-mode emulation using virtual-8086 mode with CR4.VME. Interrupt redirection is
         * achieved using the interrupt redirection bitmap (all bits cleared to let the guest
         * handle INT-n's) in the TSS. See hmR3InitFinalizeR0() to see how pRealModeTSS is setup.
         */
        uint16_t u16Sel;
        uint32_t u32Limit;
        uint64_t u64Base;
        uint32_t u32AccessRights;
        if (!pVmcsInfo->RealMode.fRealOnV86Active)
        {
            u16Sel          = pCtx->tr.Sel;
            u32Limit        = pCtx->tr.u32Limit;
            u64Base         = pCtx->tr.u64Base;
            u32AccessRights = pCtx->tr.Attr.u;
        }
        else
        {
            Assert(!pVmxTransient->fIsNestedGuest);
            Assert(pVM->hm.s.vmx.pRealModeTSS);
            Assert(PDMVmmDevHeapIsEnabled(pVM));    /* Guaranteed by HMCanExecuteGuest() -XXX- what about inner loop changes? */

            /* We obtain it here every time as PCI regions could be reconfigured in the guest, changing the VMMDev base. */
            RTGCPHYS GCPhys;
            rc = PDMVmmDevHeapR3ToGCPhys(pVM, pVM->hm.s.vmx.pRealModeTSS, &GCPhys);
            AssertRCReturn(rc, rc);

            X86DESCATTR DescAttr;
            DescAttr.u           = 0;
            DescAttr.n.u1Present = 1;
            DescAttr.n.u4Type    = X86_SEL_TYPE_SYS_386_TSS_BUSY;

            u16Sel          = 0;
            u32Limit        = HM_VTX_TSS_SIZE;
            u64Base         = GCPhys;
            u32AccessRights = DescAttr.u;
        }

        /* Validate. */
        Assert(!(u16Sel & RT_BIT(2)));
        AssertMsg(   (u32AccessRights & 0xf) == X86_SEL_TYPE_SYS_386_TSS_BUSY
                  || (u32AccessRights & 0xf) == X86_SEL_TYPE_SYS_286_TSS_BUSY, ("TSS is not busy!? %#x\n", u32AccessRights));
        AssertMsg(!(u32AccessRights & X86DESCATTR_UNUSABLE), ("TR unusable bit is not clear!? %#x\n", u32AccessRights));
        Assert(!(u32AccessRights & RT_BIT(4)));                 /* System MBZ.*/
        Assert(u32AccessRights & RT_BIT(7));                    /* Present MB1.*/
        Assert(!(u32AccessRights & 0xf00));                     /* 11:8 MBZ. */
        Assert(!(u32AccessRights & 0xfffe0000));                /* 31:17 MBZ. */
        Assert(   (u32Limit & 0xfff) == 0xfff
               || !(u32AccessRights & RT_BIT(15)));             /* Granularity MBZ. */
        Assert(   !(pCtx->tr.u32Limit & 0xfff00000)
               || (u32AccessRights & RT_BIT(15)));              /* Granularity MB1. */

        rc  = VMXWriteVmcs32(VMX_VMCS16_GUEST_TR_SEL,           u16Sel);
        rc |= VMXWriteVmcs32(VMX_VMCS32_GUEST_TR_LIMIT,         u32Limit);
        rc |= VMXWriteVmcs32(VMX_VMCS32_GUEST_TR_ACCESS_RIGHTS, u32AccessRights);
        rc |= VMXWriteVmcsGstN(VMX_VMCS_GUEST_TR_BASE,          u64Base);
        AssertRCReturn(rc, rc);

        ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_GUEST_TR);
        Log4Func(("tr base=%#RX64 limit=%#RX32\n", pCtx->tr.u64Base, pCtx->tr.u32Limit));
    }

    /*
     * Guest GDTR.
     */
    if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_GDTR)
    {
        HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_GDTR);

        rc  = VMXWriteVmcs32(VMX_VMCS32_GUEST_GDTR_LIMIT, pCtx->gdtr.cbGdt);
        rc |= VMXWriteVmcsGstN(VMX_VMCS_GUEST_GDTR_BASE,  pCtx->gdtr.pGdt);
        AssertRCReturn(rc, rc);

        /* Validate. */
        Assert(!(pCtx->gdtr.cbGdt & 0xffff0000));          /* Bits 31:16 MBZ. */

        ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_GUEST_GDTR);
        Log4Func(("gdtr base=%#RX64 limit=%#RX32\n", pCtx->gdtr.pGdt, pCtx->gdtr.cbGdt));
    }

    /*
     * Guest LDTR.
     */
    if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_LDTR)
    {
        HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_LDTR);

        /* The unusable bit is specific to VT-x, if it's a null selector mark it as an unusable segment. */
        uint32_t u32Access;
        if (   !pVmxTransient->fIsNestedGuest
            && !pCtx->ldtr.Attr.u)
            u32Access = X86DESCATTR_UNUSABLE;
        else
            u32Access = pCtx->ldtr.Attr.u;

        rc  = VMXWriteVmcs32(VMX_VMCS16_GUEST_LDTR_SEL,           pCtx->ldtr.Sel);
        rc |= VMXWriteVmcs32(VMX_VMCS32_GUEST_LDTR_LIMIT,         pCtx->ldtr.u32Limit);
        rc |= VMXWriteVmcs32(VMX_VMCS32_GUEST_LDTR_ACCESS_RIGHTS, u32Access);
        rc |= VMXWriteVmcsGstN(VMX_VMCS_GUEST_LDTR_BASE,          pCtx->ldtr.u64Base);
        AssertRCReturn(rc, rc);

        /* Validate. */
        if (!(u32Access & X86DESCATTR_UNUSABLE))
        {
            Assert(!(pCtx->ldtr.Sel & RT_BIT(2)));              /* TI MBZ. */
            Assert(pCtx->ldtr.Attr.n.u4Type == 2);              /* Type MB2 (LDT). */
            Assert(!pCtx->ldtr.Attr.n.u1DescType);              /* System MBZ. */
            Assert(pCtx->ldtr.Attr.n.u1Present == 1);           /* Present MB1. */
            Assert(!pCtx->ldtr.Attr.n.u4LimitHigh);             /* 11:8 MBZ. */
            Assert(!(pCtx->ldtr.Attr.u & 0xfffe0000));          /* 31:17 MBZ. */
            Assert(   (pCtx->ldtr.u32Limit & 0xfff) == 0xfff
                   || !pCtx->ldtr.Attr.n.u1Granularity);        /* Granularity MBZ. */
            Assert(   !(pCtx->ldtr.u32Limit & 0xfff00000)
                   || pCtx->ldtr.Attr.n.u1Granularity);         /* Granularity MB1. */
        }

        ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_GUEST_LDTR);
        Log4Func(("ldtr base=%#RX64 limit=%#RX32\n", pCtx->ldtr.u64Base, pCtx->ldtr.u32Limit));
    }

    /*
     * Guest IDTR.
     */
    if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_IDTR)
    {
        HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_IDTR);

        rc  = VMXWriteVmcs32(VMX_VMCS32_GUEST_IDTR_LIMIT, pCtx->idtr.cbIdt);
        rc |= VMXWriteVmcsGstN(VMX_VMCS_GUEST_IDTR_BASE,  pCtx->idtr.pIdt);
        AssertRCReturn(rc, rc);

        /* Validate. */
        Assert(!(pCtx->idtr.cbIdt & 0xffff0000));          /* Bits 31:16 MBZ. */

        ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_GUEST_IDTR);
        Log4Func(("idtr base=%#RX64 limit=%#RX32\n", pCtx->idtr.pIdt, pCtx->idtr.cbIdt));
    }

    return VINF_SUCCESS;
}


/**
 * Exports certain guest MSRs into the VM-entry MSR-load and VM-exit MSR-store
 * areas.
 *
 * These MSRs will automatically be loaded to the host CPU on every successful
 * VM-entry and stored from the host CPU on every successful VM-exit.
 *
 * We creates/updates MSR slots for the host MSRs in the VM-exit MSR-load area. The
 * actual host MSR values are not- updated here for performance reasons. See
 * hmR0VmxExportHostMsrs().
 *
 * We also exports the guest sysenter MSRs into the guest-state area in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
static int hmR0VmxExportGuestMsrs(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    AssertPtr(pVCpu);
    AssertPtr(pVmxTransient);

    PVM       pVM  = pVCpu->CTX_SUFF(pVM);
    PCCPUMCTX pCtx = &pVCpu->cpum.GstCtx;

    /*
     * MSRs that we use the auto-load/store MSR area in the VMCS.
     * For 64-bit hosts, we load/restore them lazily, see hmR0VmxLazyLoadGuestMsrs().
     * The host MSR values are updated when it's safe in hmR0VmxLazySaveHostMsrs().
     *
     * For nested-guests, the guests MSRs from the VM-entry MSR-load area are already
     * loaded (into the guest-CPU context) by the VMLAUNCH/VMRESUME instruction
     * emulation, nothing to do here.
     */
    if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_VMX_GUEST_AUTO_MSRS)
    {
        if (   !pVmxTransient->fIsNestedGuest
            &&  pVM->hm.s.fAllow64BitGuests)
        {
#if HC_ARCH_BITS == 32
            HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_SYSCALL_MSRS | CPUMCTX_EXTRN_KERNEL_GS_BASE);
            Assert(!pVmxTransient->fIsNestedGuest);

            int rc = hmR0VmxAddAutoLoadStoreMsr(pVCpu, pVmxTransient, MSR_K8_LSTAR,          pCtx->msrLSTAR,        true, false);
            rc    |= hmR0VmxAddAutoLoadStoreMsr(pVCpu, pVmxTransient, MSR_K6_STAR,           pCtx->msrSTAR,         true, false);
            rc    |= hmR0VmxAddAutoLoadStoreMsr(pVCpu, pVmxTransient, MSR_K8_SF_MASK,        pCtx->msrSFMASK,       true, false);
            rc    |= hmR0VmxAddAutoLoadStoreMsr(pVCpu, pVmxTransient, MSR_K8_KERNEL_GS_BASE, pCtx->msrKERNELGSBASE, true, false);
            AssertRCReturn(rc, rc);
#endif
        }
        ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_VMX_GUEST_AUTO_MSRS);
    }

    /*
     * Guest Sysenter MSRs.
     */
    if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_SYSENTER_MSR_MASK)
    {
        HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_SYSENTER_MSRS);

        if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_SYSENTER_CS_MSR)
        {
            int rc = VMXWriteVmcs32(VMX_VMCS32_GUEST_SYSENTER_CS, pCtx->SysEnter.cs);
            AssertRCReturn(rc, rc);
            ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_GUEST_SYSENTER_CS_MSR);
        }

        if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_SYSENTER_EIP_MSR)
        {
            int rc = VMXWriteVmcsGstN(VMX_VMCS_GUEST_SYSENTER_EIP, pCtx->SysEnter.eip);
            AssertRCReturn(rc, rc);
            ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_GUEST_SYSENTER_EIP_MSR);
        }

        if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_SYSENTER_ESP_MSR)
        {
            int rc = VMXWriteVmcsGstN(VMX_VMCS_GUEST_SYSENTER_ESP, pCtx->SysEnter.esp);
            AssertRCReturn(rc, rc);
            ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_GUEST_SYSENTER_ESP_MSR);
        }
    }

    /*
     * Guest/host EFER MSR.
     */
    if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_EFER_MSR)
    {
        /* Whether we are using the VMCS to swap the EFER MSR must have been
           determined earlier while exporting VM-entry/VM-exit controls. */
        Assert(!(ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_VMX_ENTRY_EXIT_CTLS));
        HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_EFER);

        if (hmR0VmxShouldSwapEferMsr(pVCpu))
        {
            /*
             * If the CPU supports VMCS controls for swapping EFER, use it. Otherwise, we have no option
             * but to use the auto-load store MSR area in the VMCS for swapping EFER. See @bugref{7368}.
             */
            if (pVM->hm.s.vmx.fSupportsVmcsEfer)
            {
                int rc = VMXWriteVmcs64(VMX_VMCS64_GUEST_EFER_FULL, pCtx->msrEFER);
                AssertRCReturn(rc, rc);
            }
            else
            {
                /*
                 * We shall use the auto-load/store MSR area only for loading the EFER MSR but we must
                 * continue to intercept guest read and write accesses to it, see @bugref{7386#c16}.
                 */
                int rc = hmR0VmxAddAutoLoadStoreMsr(pVCpu, pVmxTransient, MSR_K6_EFER, pCtx->msrEFER,
                                                    false /* fSetReadWrite */, false /* fUpdateHostMsr */);
                AssertRCReturn(rc, rc);
            }
        }
        else if (!pVM->hm.s.vmx.fSupportsVmcsEfer)
            hmR0VmxRemoveAutoLoadStoreMsr(pVCpu, pVmxTransient, MSR_K6_EFER);

        ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_GUEST_EFER_MSR);
    }

    /*
     * Other MSRs.
     * Speculation Control (R/W).
     */
    if (ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged) & HM_CHANGED_GUEST_OTHER_MSRS)
    {
        HMVMX_CPUMCTX_ASSERT(pVCpu, HM_CHANGED_GUEST_OTHER_MSRS);
        if (pVM->cpum.ro.GuestFeatures.fIbrs)
        {
            int rc = hmR0VmxAddAutoLoadStoreMsr(pVCpu, pVmxTransient, MSR_IA32_SPEC_CTRL, CPUMGetGuestSpecCtrl(pVCpu),
                                                false /* fSetReadWrite */, false /* fUpdateHostMsr */);
            AssertRCReturn(rc, rc);
        }
        ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~HM_CHANGED_GUEST_OTHER_MSRS);
    }

    return VINF_SUCCESS;
}


/**
 * Selects up the appropriate function to run guest code.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
static int hmR0VmxSelectVMRunHandler(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    PCCPUMCTX    pCtx      = &pVCpu->cpum.GstCtx;
    PVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;

    if (CPUMIsGuestInLongModeEx(pCtx))
    {
#ifndef VBOX_ENABLE_64_BITS_GUESTS
        return VERR_PGM_UNSUPPORTED_SHADOW_PAGING_MODE;
#endif
        Assert(pVCpu->CTX_SUFF(pVM)->hm.s.fAllow64BitGuests);    /* Guaranteed by hmR3InitFinalizeR0(). */
#if HC_ARCH_BITS == 32
        /* 32-bit host. We need to switch to 64-bit before running the 64-bit guest. */
        if (pVmcsInfo->pfnStartVM != VMXR0SwitcherStartVM64)
        {
#ifdef VBOX_STRICT
            if (pVmcsInfo->pfnStartVM != NULL) /* Very first VM-entry would have saved host-state already, ignore it. */
            {
                /* Currently, all mode changes sends us back to ring-3, so these should be set. See @bugref{6944}. */
                uint64_t const fCtxChanged = ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged);
                RT_UNTRUSTED_NONVOLATILE_COPY_FENCE();
                AssertMsg(fCtxChanged & (HM_CHANGED_VMX_ENTRY_EXIT_CTLS | HM_CHANGED_GUEST_EFER_MSR),
                          ("fCtxChanged=%#RX64\n", fCtxChanged));
            }
#endif
            pVmcsInfo->pfnStartVM = VMXR0SwitcherStartVM64;

            /* Mark that we've switched to 64-bit handler, we can't safely switch back to 32-bit for
               the rest of the VM run (until VM reset). See @bugref{8432#c7}. */
            pVmcsInfo->fSwitchedTo64on32 = true;
            Log4Func(("Selected 64-bit switcher\n"));
        }
#else
        /* 64-bit host. */
        pVmcsInfo->pfnStartVM = VMXR0StartVM64;
#endif
    }
    else
    {
        /* Guest is not in long mode, use the 32-bit handler. */
#if HC_ARCH_BITS == 32
        if (    pVmcsInfo->pfnStartVM != VMXR0StartVM32
            && !pVmcsInfo->fSwitchedTo64on32      /* If set, guest mode change does not imply switcher change. */
            &&  pVmcsInfo->pfnStartVM != NULL)    /* Very first VM-entry would have saved host-state already, ignore it. */
        {
# ifdef VBOX_STRICT
            /* Currently, all mode changes sends us back to ring-3, so these should be set. See @bugref{6944}. */
            uint64_t const fCtxChanged = ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged);
            RT_UNTRUSTED_NONVOLATILE_COPY_FENCE();
            AssertMsg(fCtxChanged & (HM_CHANGED_VMX_ENTRY_EXIT_CTLS | HM_CHANGED_GUEST_EFER_MSR),
                      ("fCtxChanged=%#RX64\n", fCtxChanged));
# endif
        }
# ifdef VBOX_ENABLE_64_BITS_GUESTS
        /*
         * Keep using the 64-bit switcher even though we're in 32-bit because of bad Intel
         * design, see @bugref{8432#c7}. If real-on-v86 mode is active, clear the 64-bit
         * switcher flag now because we know the guest is in a sane state where it's safe
         * to use the 32-bit switcher. Otherwise, check the guest state if it's safe to use
         * the much faster 32-bit switcher again.
         */
        if (!pVmcsInfo->fSwitchedTo64on32)
        {
            if (pVmcsInfo->pfnStartVM != VMXR0StartVM32)
                Log4Func(("Selected 32-bit switcher\n"));
            pVmcsInfo->pfnStartVM = VMXR0StartVM32;
        }
        else
        {
            Assert(pVmcsInfo->pfnStartVM == VMXR0SwitcherStartVM64);
            if (   pVmcsInfo->RealMode.fRealOnV86Active
                || hmR0VmxIs32BitSwitcherSafe(pCtx))
            {
                pVmcsInfo->fSwitchedTo64on32 = false;
                pVmcsInfo->pfnStartVM = VMXR0StartVM32;
                ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_EFER_MSR
                                                         | HM_CHANGED_VMX_ENTRY_EXIT_CTLS
                                                         | HM_CHANGED_HOST_CONTEXT);
                Log4Func(("Selected 32-bit switcher (safe)\n"));
            }
        }
# else
        pVmcsInfo->pfnStartVM = VMXR0StartVM32;
# endif
#else
        pVmcsInfo->pfnStartVM = VMXR0StartVM32;
#endif
    }
    Assert(pVmcsInfo->pfnStartVM);
    return VINF_SUCCESS;
}


/**
 * Wrapper for running the guest code in VT-x.
 *
 * @returns VBox status code, no informational status codes.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxRunGuest(PVMCPU pVCpu, PCVMXTRANSIENT pVmxTransient)
{
    /* Mark that HM is the keeper of all guest-CPU registers now that we're going to execute guest code. */
    PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
    pCtx->fExtrn |= HMVMX_CPUMCTX_EXTRN_ALL | CPUMCTX_EXTRN_KEEPER_HM;

    /** @todo Add stats for VMRESUME vs VMLAUNCH. */

    /*
     * 64-bit Windows uses XMM registers in the kernel as the Microsoft compiler expresses
     * floating-point operations using SSE instructions. Some XMM registers (XMM6-XMM15) are
     * callee-saved and thus the need for this XMM wrapper.
     *
     * See MSDN "Configuring Programs for 64-bit/x64 Software Conventions / Register Usage".
     */
    PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    bool const fResumeVM = RT_BOOL(pVmcsInfo->fVmcsState & VMX_V_VMCS_LAUNCH_STATE_LAUNCHED);
    PVM pVM = pVCpu->CTX_SUFF(pVM);
#ifdef VBOX_WITH_KERNEL_USING_XMM
    int rc = hmR0VMXStartVMWrapXMM(fResumeVM, pCtx, &pVCpu->hm.s.vmx.VmcsCache, pVM, pVCpu, pVmcsInfo->pfnStartVM);
#else
    int rc = pVmcsInfo->pfnStartVM(fResumeVM, pCtx, &pVCpu->hm.s.vmx.VmcsCache, pVM, pVCpu);
#endif
    AssertMsg(rc <= VINF_SUCCESS, ("%Rrc\n", rc));
    return rc;
}


/**
 * Reports world-switch error and dumps some useful debug info.
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   rcVMRun         The return code from VMLAUNCH/VMRESUME.
 * @param   pVmxTransient   The VMX-transient structure (only
 *                          exitReason updated).
 */
static void hmR0VmxReportWorldSwitchError(PVMCPU pVCpu, int rcVMRun, PVMXTRANSIENT pVmxTransient)
{
    Assert(pVCpu);
    Assert(pVmxTransient);
    HMVMX_ASSERT_PREEMPT_SAFE(pVCpu);

    Log4Func(("VM-entry failure: %Rrc\n", rcVMRun));
    switch (rcVMRun)
    {
        case VERR_VMX_INVALID_VMXON_PTR:
            AssertFailed();
            break;
        case VINF_SUCCESS:                  /* VMLAUNCH/VMRESUME succeeded but VM-entry failed... yeah, true story. */
        case VERR_VMX_UNABLE_TO_START_VM:   /* VMLAUNCH/VMRESUME itself failed. */
        {
            int rc = VMXReadVmcs32(VMX_VMCS32_RO_EXIT_REASON, &pVCpu->hm.s.vmx.LastError.u32ExitReason);
            rc    |= VMXReadVmcs32(VMX_VMCS32_RO_VM_INSTR_ERROR, &pVCpu->hm.s.vmx.LastError.u32InstrError);
            rc    |= hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
            AssertRC(rc);

            pVCpu->hm.s.vmx.LastError.idEnteredCpu = pVCpu->hm.s.idEnteredCpu;
            /* LastError.idCurrentCpu was already updated in hmR0VmxPreRunGuestCommitted().
               Cannot do it here as we may have been long preempted. */

#ifdef VBOX_STRICT
                PVMXVMCSINFO pVmcsInfo = hmGetVmxActiveVmcsInfo(pVCpu);
                Log4(("uExitReason        %#RX32 (VmxTransient %#RX16)\n", pVCpu->hm.s.vmx.LastError.u32ExitReason,
                     pVmxTransient->uExitReason));
                Log4(("Exit Qualification %#RX64\n", pVmxTransient->uExitQual));
                Log4(("InstrError         %#RX32\n", pVCpu->hm.s.vmx.LastError.u32InstrError));
                if (pVCpu->hm.s.vmx.LastError.u32InstrError <= HMVMX_INSTR_ERROR_MAX)
                    Log4(("InstrError Desc.  \"%s\"\n", g_apszVmxInstrErrors[pVCpu->hm.s.vmx.LastError.u32InstrError]));
                else
                    Log4(("InstrError Desc.    Range exceeded %u\n", HMVMX_INSTR_ERROR_MAX));
                Log4(("Entered host CPU   %u\n", pVCpu->hm.s.vmx.LastError.idEnteredCpu));
                Log4(("Current host CPU   %u\n", pVCpu->hm.s.vmx.LastError.idCurrentCpu));

                /* VMX control bits. */
                uint32_t        u32Val;
                uint64_t        u64Val;
                RTHCUINTREG     uHCReg;
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_PIN_EXEC, &u32Val);                  AssertRC(rc);
                Log4(("VMX_VMCS32_CTRL_PIN_EXEC                %#RX32\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_PROC_EXEC, &u32Val);                 AssertRC(rc);
                Log4(("VMX_VMCS32_CTRL_PROC_EXEC               %#RX32\n", u32Val));
                if (pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_USE_SECONDARY_CTLS)
                {
                    rc = VMXReadVmcs32(VMX_VMCS32_CTRL_PROC_EXEC2, &u32Val);            AssertRC(rc);
                    Log4(("VMX_VMCS32_CTRL_PROC_EXEC2              %#RX32\n", u32Val));
                }
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_ENTRY, &u32Val);                     AssertRC(rc);
                Log4(("VMX_VMCS32_CTRL_ENTRY                   %#RX32\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_EXIT, &u32Val);                      AssertRC(rc);
                Log4(("VMX_VMCS32_CTRL_EXIT                    %#RX32\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_CR3_TARGET_COUNT, &u32Val);          AssertRC(rc);
                Log4(("VMX_VMCS32_CTRL_CR3_TARGET_COUNT        %#RX32\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_ENTRY_INTERRUPTION_INFO, &u32Val);   AssertRC(rc);
                Log4(("VMX_VMCS32_CTRL_ENTRY_INTERRUPTION_INFO %#RX32\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_ENTRY_EXCEPTION_ERRCODE, &u32Val);   AssertRC(rc);
                Log4(("VMX_VMCS32_CTRL_ENTRY_EXCEPTION_ERRCODE %#RX32\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_ENTRY_INSTR_LENGTH, &u32Val);        AssertRC(rc);
                Log4(("VMX_VMCS32_CTRL_ENTRY_INSTR_LENGTH      %u\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_TPR_THRESHOLD, &u32Val);             AssertRC(rc);
                Log4(("VMX_VMCS32_CTRL_TPR_THRESHOLD           %u\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_EXIT_MSR_STORE_COUNT, &u32Val);      AssertRC(rc);
                Log4(("VMX_VMCS32_CTRL_EXIT_MSR_STORE_COUNT    %u (guest MSRs)\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_EXIT_MSR_LOAD_COUNT, &u32Val);       AssertRC(rc);
                Log4(("VMX_VMCS32_CTRL_EXIT_MSR_LOAD_COUNT     %u (host MSRs)\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_ENTRY_MSR_LOAD_COUNT, &u32Val);      AssertRC(rc);
                Log4(("VMX_VMCS32_CTRL_ENTRY_MSR_LOAD_COUNT    %u (guest MSRs)\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_EXCEPTION_BITMAP, &u32Val);          AssertRC(rc);
                Log4(("VMX_VMCS32_CTRL_EXCEPTION_BITMAP        %#RX32\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_PAGEFAULT_ERROR_MASK, &u32Val);      AssertRC(rc);
                Log4(("VMX_VMCS32_CTRL_PAGEFAULT_ERROR_MASK    %#RX32\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_PAGEFAULT_ERROR_MATCH, &u32Val);     AssertRC(rc);
                Log4(("VMX_VMCS32_CTRL_PAGEFAULT_ERROR_MATCH   %#RX32\n", u32Val));
                rc = VMXReadVmcsHstN(VMX_VMCS_CTRL_CR0_MASK, &uHCReg);                  AssertRC(rc);
                Log4(("VMX_VMCS_CTRL_CR0_MASK                  %#RHr\n", uHCReg));
                rc = VMXReadVmcsHstN(VMX_VMCS_CTRL_CR0_READ_SHADOW, &uHCReg);           AssertRC(rc);
                Log4(("VMX_VMCS_CTRL_CR4_READ_SHADOW           %#RHr\n", uHCReg));
                rc = VMXReadVmcsHstN(VMX_VMCS_CTRL_CR4_MASK, &uHCReg);                  AssertRC(rc);
                Log4(("VMX_VMCS_CTRL_CR4_MASK                  %#RHr\n", uHCReg));
                rc = VMXReadVmcsHstN(VMX_VMCS_CTRL_CR4_READ_SHADOW, &uHCReg);           AssertRC(rc);
                Log4(("VMX_VMCS_CTRL_CR4_READ_SHADOW           %#RHr\n", uHCReg));
                if (pVCpu->CTX_SUFF(pVM)->hm.s.fNestedPaging)
                {
                    rc = VMXReadVmcs64(VMX_VMCS64_CTRL_EPTP_FULL, &u64Val);             AssertRC(rc);
                    Log4(("VMX_VMCS64_CTRL_EPTP_FULL               %#RX64\n", u64Val));
                }

                /* Guest bits. */
                rc = VMXReadVmcsGstN(VMX_VMCS_GUEST_RIP, &u64Val);          AssertRC(rc);
                Log4(("Old Guest Rip %#RX64 New %#RX64\n", pVCpu->cpum.GstCtx.rip, u64Val));
                rc = VMXReadVmcsGstN(VMX_VMCS_GUEST_RSP, &u64Val);          AssertRC(rc);
                Log4(("Old Guest Rsp %#RX64 New %#RX64\n", pVCpu->cpum.GstCtx.rsp, u64Val));
                rc = VMXReadVmcs32(VMX_VMCS_GUEST_RFLAGS, &u32Val);         AssertRC(rc);
                Log4(("Old Guest Rflags %#RX32 New %#RX32\n", pVCpu->cpum.GstCtx.eflags.u32, u32Val));
                if (pVCpu->CTX_SUFF(pVM)->hm.s.vmx.fVpid)
                {
                    rc = VMXReadVmcs32(VMX_VMCS16_VPID, &u32Val);           AssertRC(rc);
                    Log4(("VMX_VMCS16_VPID  %u\n", u32Val));
                }

                /* Host bits. */
                rc = VMXReadVmcsHstN(VMX_VMCS_HOST_CR0, &uHCReg);           AssertRC(rc);
                Log4(("Host CR0 %#RHr\n", uHCReg));
                rc = VMXReadVmcsHstN(VMX_VMCS_HOST_CR3, &uHCReg);           AssertRC(rc);
                Log4(("Host CR3 %#RHr\n", uHCReg));
                rc = VMXReadVmcsHstN(VMX_VMCS_HOST_CR4, &uHCReg);           AssertRC(rc);
                Log4(("Host CR4 %#RHr\n", uHCReg));

                RTGDTR      HostGdtr;
                PCX86DESCHC pDesc;
                ASMGetGDTR(&HostGdtr);
                rc = VMXReadVmcs32(VMX_VMCS16_HOST_CS_SEL, &u32Val);      AssertRC(rc);
                Log4(("Host CS %#08x\n", u32Val));
                if (u32Val < HostGdtr.cbGdt)
                {
                    pDesc  = (PCX86DESCHC)(HostGdtr.pGdt + (u32Val & X86_SEL_MASK));
                    hmR0DumpDescriptor(pDesc, u32Val, "CS: ");
                }

                rc = VMXReadVmcs32(VMX_VMCS16_HOST_DS_SEL, &u32Val);      AssertRC(rc);
                Log4(("Host DS %#08x\n", u32Val));
                if (u32Val < HostGdtr.cbGdt)
                {
                    pDesc  = (PCX86DESCHC)(HostGdtr.pGdt + (u32Val & X86_SEL_MASK));
                    hmR0DumpDescriptor(pDesc, u32Val, "DS: ");
                }

                rc = VMXReadVmcs32(VMX_VMCS16_HOST_ES_SEL, &u32Val);      AssertRC(rc);
                Log4(("Host ES %#08x\n", u32Val));
                if (u32Val < HostGdtr.cbGdt)
                {
                    pDesc  = (PCX86DESCHC)(HostGdtr.pGdt + (u32Val & X86_SEL_MASK));
                    hmR0DumpDescriptor(pDesc, u32Val, "ES: ");
                }

                rc = VMXReadVmcs32(VMX_VMCS16_HOST_FS_SEL, &u32Val);      AssertRC(rc);
                Log4(("Host FS %#08x\n", u32Val));
                if (u32Val < HostGdtr.cbGdt)
                {
                    pDesc  = (PCX86DESCHC)(HostGdtr.pGdt + (u32Val & X86_SEL_MASK));
                    hmR0DumpDescriptor(pDesc, u32Val, "FS: ");
                }

                rc = VMXReadVmcs32(VMX_VMCS16_HOST_GS_SEL, &u32Val);      AssertRC(rc);
                Log4(("Host GS %#08x\n", u32Val));
                if (u32Val < HostGdtr.cbGdt)
                {
                    pDesc  = (PCX86DESCHC)(HostGdtr.pGdt + (u32Val & X86_SEL_MASK));
                    hmR0DumpDescriptor(pDesc, u32Val, "GS: ");
                }

                rc = VMXReadVmcs32(VMX_VMCS16_HOST_SS_SEL, &u32Val);      AssertRC(rc);
                Log4(("Host SS %#08x\n", u32Val));
                if (u32Val < HostGdtr.cbGdt)
                {
                    pDesc  = (PCX86DESCHC)(HostGdtr.pGdt + (u32Val & X86_SEL_MASK));
                    hmR0DumpDescriptor(pDesc, u32Val, "SS: ");
                }

                rc = VMXReadVmcs32(VMX_VMCS16_HOST_TR_SEL,  &u32Val);     AssertRC(rc);
                Log4(("Host TR %#08x\n", u32Val));
                if (u32Val < HostGdtr.cbGdt)
                {
                    pDesc  = (PCX86DESCHC)(HostGdtr.pGdt + (u32Val & X86_SEL_MASK));
                    hmR0DumpDescriptor(pDesc, u32Val, "TR: ");
                }

                rc = VMXReadVmcsHstN(VMX_VMCS_HOST_TR_BASE, &uHCReg);       AssertRC(rc);
                Log4(("Host TR Base %#RHv\n", uHCReg));
                rc = VMXReadVmcsHstN(VMX_VMCS_HOST_GDTR_BASE, &uHCReg);     AssertRC(rc);
                Log4(("Host GDTR Base %#RHv\n", uHCReg));
                rc = VMXReadVmcsHstN(VMX_VMCS_HOST_IDTR_BASE, &uHCReg);     AssertRC(rc);
                Log4(("Host IDTR Base %#RHv\n", uHCReg));
                rc = VMXReadVmcs32(VMX_VMCS32_HOST_SYSENTER_CS, &u32Val);   AssertRC(rc);
                Log4(("Host SYSENTER CS  %#08x\n", u32Val));
                rc = VMXReadVmcsHstN(VMX_VMCS_HOST_SYSENTER_EIP, &uHCReg);  AssertRC(rc);
                Log4(("Host SYSENTER EIP %#RHv\n", uHCReg));
                rc = VMXReadVmcsHstN(VMX_VMCS_HOST_SYSENTER_ESP, &uHCReg);  AssertRC(rc);
                Log4(("Host SYSENTER ESP %#RHv\n", uHCReg));
                rc = VMXReadVmcsHstN(VMX_VMCS_HOST_RSP, &uHCReg);           AssertRC(rc);
                Log4(("Host RSP %#RHv\n", uHCReg));
                rc = VMXReadVmcsHstN(VMX_VMCS_HOST_RIP, &uHCReg);           AssertRC(rc);
                Log4(("Host RIP %#RHv\n", uHCReg));
# if HC_ARCH_BITS == 64
                Log4(("MSR_K6_EFER            = %#RX64\n", ASMRdMsr(MSR_K6_EFER)));
                Log4(("MSR_K8_CSTAR           = %#RX64\n", ASMRdMsr(MSR_K8_CSTAR)));
                Log4(("MSR_K8_LSTAR           = %#RX64\n", ASMRdMsr(MSR_K8_LSTAR)));
                Log4(("MSR_K6_STAR            = %#RX64\n", ASMRdMsr(MSR_K6_STAR)));
                Log4(("MSR_K8_SF_MASK         = %#RX64\n", ASMRdMsr(MSR_K8_SF_MASK)));
                Log4(("MSR_K8_KERNEL_GS_BASE  = %#RX64\n", ASMRdMsr(MSR_K8_KERNEL_GS_BASE)));
# endif
#endif /* VBOX_STRICT */
            break;
        }

        default:
            /* Impossible */
            AssertMsgFailed(("hmR0VmxReportWorldSwitchError %Rrc (%#x)\n", rcVMRun, rcVMRun));
            break;
    }
}


#if HC_ARCH_BITS == 32 && defined(VBOX_ENABLE_64_BITS_GUESTS)
# ifndef VMX_USE_CACHED_VMCS_ACCESSES
#  error "VMX_USE_CACHED_VMCS_ACCESSES not defined when it should be!"
# endif

/**
 * Initialize the VMCS-Read cache.
 *
 * The VMCS cache is used for 32-bit hosts running 64-bit guests (except 32-bit
 * Darwin which runs with 64-bit paging in 32-bit mode) for 64-bit fields that
 * cannot be accessed in 32-bit mode. Some 64-bit fields -can- be accessed
 * (those that have a 32-bit FULL & HIGH part).
 *
 * @param   pVCpu       The cross context virtual CPU structure.
 */
static void hmR0VmxInitVmcsReadCache(PVMCPU pVCpu)
{
#define VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, idxField)           \
    do {                                                           \
        Assert(pCache->Read.aField[idxField##_CACHE_IDX] == 0);    \
        pCache->Read.aField[idxField##_CACHE_IDX]    = idxField;   \
        pCache->Read.aFieldVal[idxField##_CACHE_IDX] = 0;          \
        ++cReadFields;                                             \
    } while (0)

    PVMXVMCSCACHE pCache = &pVCpu->hm.s.vmx.VmcsCache;
    uint32_t cReadFields = 0;

    /*
     * Don't remove the #if 0'd fields in this code. They're listed here for consistency
     * and serve to indicate exceptions to the rules.
     */

    /* Guest-natural selector base fields. */
#if 0
    /* These are 32-bit in practice. See Intel spec. 2.5 "Control Registers". */
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_CR0);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_CR4);
#endif
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_ES_BASE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_CS_BASE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_SS_BASE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_DS_BASE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_FS_BASE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_GS_BASE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_LDTR_BASE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_TR_BASE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_GDTR_BASE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_IDTR_BASE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_RSP);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_RIP);
#if 0
    /* Unused natural width guest-state fields. */
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_PENDING_DEBUG_XCPTS);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_CR3); /* Handled in nested paging case */
#endif
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_SYSENTER_ESP);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_SYSENTER_EIP);

    /* 64-bit guest-state fields; unused as we use two 32-bit VMREADs for
       these 64-bit fields (using "FULL" and "HIGH" fields). */
#if 0
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS64_GUEST_VMCS_LINK_PTR_FULL);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS64_GUEST_DEBUGCTL_FULL);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS64_GUEST_PAT_FULL);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS64_GUEST_EFER_FULL);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS64_GUEST_PERF_GLOBAL_CTRL_FULL);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS64_GUEST_PDPTE0_FULL);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS64_GUEST_PDPTE1_FULL);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS64_GUEST_PDPTE2_FULL);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS64_GUEST_PDPTE3_FULL);
#endif

    /* Natural width guest-state fields. */
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_RO_EXIT_QUALIFICATION);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_RO_GUEST_LINEAR_ADDR);

    if (pVCpu->CTX_SUFF(pVM)->hm.s.fNestedPaging)
    {
        VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_CR3);
        AssertMsg(cReadFields == VMX_VMCS_MAX_NESTED_PAGING_CACHE_IDX, ("cReadFields=%u expected %u\n", cReadFields,
                                                                        VMX_VMCS_MAX_NESTED_PAGING_CACHE_IDX));
        pCache->Read.cValidEntries = VMX_VMCS_MAX_NESTED_PAGING_CACHE_IDX;
    }
    else
    {
        AssertMsg(cReadFields == VMX_VMCS_MAX_CACHE_IDX, ("cReadFields=%u expected %u\n", cReadFields, VMX_VMCS_MAX_CACHE_IDX));
        pCache->Read.cValidEntries = VMX_VMCS_MAX_CACHE_IDX;
    }

#undef VMXLOCAL_INIT_READ_CACHE_FIELD
}


/**
 * Writes a field into the VMCS. This can either directly invoke a VMWRITE or
 * queue up the VMWRITE by using the VMCS write cache (on 32-bit hosts, except
 * darwin, running 64-bit guests).
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   idxField        The VMCS field encoding.
 * @param   u64Val          16, 32 or 64-bit value.
 */
VMMR0DECL(int) VMXWriteVmcs64Ex(PVMCPU pVCpu, uint32_t idxField, uint64_t u64Val)
{
    AssertPtr(pVCpu);
    int rc;
    switch (idxField)
    {
        /*
         * These fields consists of a "FULL" and a "HIGH" part which can be written to individually.
         */
        /* 64-bit Control fields. */
        case VMX_VMCS64_CTRL_IO_BITMAP_A_FULL:
        case VMX_VMCS64_CTRL_IO_BITMAP_B_FULL:
        case VMX_VMCS64_CTRL_MSR_BITMAP_FULL:
        case VMX_VMCS64_CTRL_EXIT_MSR_STORE_FULL:
        case VMX_VMCS64_CTRL_EXIT_MSR_LOAD_FULL:
        case VMX_VMCS64_CTRL_ENTRY_MSR_LOAD_FULL:
        case VMX_VMCS64_CTRL_EXEC_VMCS_PTR_FULL:
        case VMX_VMCS64_CTRL_TSC_OFFSET_FULL:
        case VMX_VMCS64_CTRL_VIRT_APIC_PAGEADDR_FULL:
        case VMX_VMCS64_CTRL_APIC_ACCESSADDR_FULL:
        case VMX_VMCS64_CTRL_VMFUNC_CTRLS_FULL:
        case VMX_VMCS64_CTRL_EPTP_FULL:
        case VMX_VMCS64_CTRL_EPTP_LIST_FULL:
        /* 64-bit Guest-state fields. */
        case VMX_VMCS64_GUEST_VMCS_LINK_PTR_FULL:
        case VMX_VMCS64_GUEST_DEBUGCTL_FULL:
        case VMX_VMCS64_GUEST_PAT_FULL:
        case VMX_VMCS64_GUEST_EFER_FULL:
        case VMX_VMCS64_GUEST_PERF_GLOBAL_CTRL_FULL:
        case VMX_VMCS64_GUEST_PDPTE0_FULL:
        case VMX_VMCS64_GUEST_PDPTE1_FULL:
        case VMX_VMCS64_GUEST_PDPTE2_FULL:
        case VMX_VMCS64_GUEST_PDPTE3_FULL:
        /* 64-bit Host-state fields. */
        case VMX_VMCS64_HOST_PAT_FULL:
        case VMX_VMCS64_HOST_EFER_FULL:
        case VMX_VMCS64_HOST_PERF_GLOBAL_CTRL_FULL:
        {
            rc  = VMXWriteVmcs32(idxField,     RT_LO_U32(u64Val));
            rc |= VMXWriteVmcs32(idxField + 1, RT_HI_U32(u64Val));
            break;
        }

        /*
         * These fields do not have high and low parts. Queue up the VMWRITE by using the VMCS write-cache (for 64-bit
         * values). When we switch the host to 64-bit mode for running 64-bit guests, these VMWRITEs get executed then.
         */
        /* Natural-width Guest-state fields.  */
        case VMX_VMCS_GUEST_CR3:
        case VMX_VMCS_GUEST_ES_BASE:
        case VMX_VMCS_GUEST_CS_BASE:
        case VMX_VMCS_GUEST_SS_BASE:
        case VMX_VMCS_GUEST_DS_BASE:
        case VMX_VMCS_GUEST_FS_BASE:
        case VMX_VMCS_GUEST_GS_BASE:
        case VMX_VMCS_GUEST_LDTR_BASE:
        case VMX_VMCS_GUEST_TR_BASE:
        case VMX_VMCS_GUEST_GDTR_BASE:
        case VMX_VMCS_GUEST_IDTR_BASE:
        case VMX_VMCS_GUEST_RSP:
        case VMX_VMCS_GUEST_RIP:
        case VMX_VMCS_GUEST_SYSENTER_ESP:
        case VMX_VMCS_GUEST_SYSENTER_EIP:
        {
            if (!(RT_HI_U32(u64Val)))
            {
                /* If this field is 64-bit, VT-x will zero out the top bits. */
                rc = VMXWriteVmcs32(idxField, RT_LO_U32(u64Val));
            }
            else
            {
                /* Assert that only the 32->64 switcher case should ever come here. */
                Assert(pVCpu->CTX_SUFF(pVM)->hm.s.fAllow64BitGuests);
                rc = VMXWriteCachedVmcsEx(pVCpu, idxField, u64Val);
            }
            break;
        }

        default:
        {
            AssertMsgFailed(("VMXWriteVmcs64Ex: Invalid field %#RX32 (pVCpu=%p u64Val=%#RX64)\n", idxField, pVCpu, u64Val));
            pVCpu->hm.s.u32HMError = idxField;
            rc = VERR_INVALID_PARAMETER;
            break;
        }
    }
    AssertRCReturn(rc, rc);
    return rc;
}


/**
 * Queue up a VMWRITE by using the VMCS write cache.
 * This is only used on 32-bit hosts (except darwin) for 64-bit guests.
 *
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   idxField    The VMCS field encoding.
 * @param   u64Val      16, 32 or 64-bit value.
 */
VMMR0DECL(int) VMXWriteCachedVmcsEx(PVMCPU pVCpu, uint32_t idxField, uint64_t u64Val)
{
    AssertPtr(pVCpu);
    PVMXVMCSCACHE pCache = &pVCpu->hm.s.vmx.VmcsCache;

    AssertMsgReturn(pCache->Write.cValidEntries < VMX_VMCS_CACHE_MAX_ENTRY - 1,
                    ("entries=%u\n", pCache->Write.cValidEntries), VERR_ACCESS_DENIED);

    /* Make sure there are no duplicates. */
    for (uint32_t i = 0; i < pCache->Write.cValidEntries; i++)
    {
        if (pCache->Write.aField[i] == idxField)
        {
            pCache->Write.aFieldVal[i] = u64Val;
            return VINF_SUCCESS;
        }
    }

    pCache->Write.aField[pCache->Write.cValidEntries]    = idxField;
    pCache->Write.aFieldVal[pCache->Write.cValidEntries] = u64Val;
    pCache->Write.cValidEntries++;
    return VINF_SUCCESS;
}
#endif /* HC_ARCH_BITS == 32 && defined(VBOX_ENABLE_64_BITS_GUESTS) */


/**
 * Sets up the usage of TSC-offsetting and updates the VMCS.
 *
 * If offsetting is not possible, cause VM-exits on RDTSC(P)s. Also sets up the
 * VMX-preemption timer.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
static void hmR0VmxUpdateTscOffsettingAndPreemptTimer(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    bool         fOffsettedTsc;
    bool         fParavirtTsc;
    uint64_t     uTscOffset;
    PVM          pVM = pVCpu->CTX_SUFF(pVM);
    PVMXVMCSINFO pVmcsInfo = hmGetVmxActiveVmcsInfo(pVCpu);

    if (pVM->hm.s.vmx.fUsePreemptTimer)
    {
        uint64_t cTicksToDeadline = TMCpuTickGetDeadlineAndTscOffset(pVM, pVCpu, &uTscOffset, &fOffsettedTsc, &fParavirtTsc);

        /* Make sure the returned values have sane upper and lower boundaries. */
        uint64_t u64CpuHz  = SUPGetCpuHzFromGipBySetIndex(g_pSUPGlobalInfoPage, pVCpu->iHostCpuSet);
        cTicksToDeadline   = RT_MIN(cTicksToDeadline, u64CpuHz / 64);      /* 1/64th of a second */
        cTicksToDeadline   = RT_MAX(cTicksToDeadline, u64CpuHz / 2048);    /* 1/2048th of a second */
        cTicksToDeadline >>= pVM->hm.s.vmx.cPreemptTimerShift;

        /** @todo r=ramshankar: We need to find a way to integrate nested-guest
         *        preemption timers here. We probably need to clamp the preemption timer,
         *        after converting the timer value to the host. */
        uint32_t cPreemptionTickCount = (uint32_t)RT_MIN(cTicksToDeadline, UINT32_MAX - 16);
        int rc = VMXWriteVmcs32(VMX_VMCS32_PREEMPT_TIMER_VALUE, cPreemptionTickCount);
        AssertRC(rc);
    }
    else
        fOffsettedTsc = TMCpuTickCanUseRealTSC(pVM, pVCpu, &uTscOffset, &fParavirtTsc);

    if (fParavirtTsc)
    {
        /* Currently neither Hyper-V nor KVM need to update their paravirt. TSC
           information before every VM-entry, hence disable it for performance sake. */
#if 0
        int rc = GIMR0UpdateParavirtTsc(pVM, 0 /* u64Offset */);
        AssertRC(rc);
#endif
        STAM_COUNTER_INC(&pVCpu->hm.s.StatTscParavirt);
    }

    uint32_t uProcCtls = pVmcsInfo->u32ProcCtls;
    if (   fOffsettedTsc
        && RT_LIKELY(!pVCpu->hm.s.fDebugWantRdTscExit))
    {
        if (pVmxTransient->fIsNestedGuest)
            uTscOffset = CPUMApplyNestedGuestTscOffset(pVCpu, uTscOffset);
        if (pVmcsInfo->u64TscOffset != uTscOffset)
        {
            int rc = VMXWriteVmcs64(VMX_VMCS64_CTRL_TSC_OFFSET_FULL, uTscOffset);
            AssertRC(rc);
            pVmcsInfo->u64TscOffset = uTscOffset;
        }

        if (uProcCtls & VMX_PROC_CTLS_RDTSC_EXIT)
        {
            uProcCtls &= ~VMX_PROC_CTLS_RDTSC_EXIT;
            int rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC, uProcCtls);
            AssertRC(rc);
            pVmcsInfo->u32ProcCtls = uProcCtls;
        }
        STAM_COUNTER_INC(&pVCpu->hm.s.StatTscOffset);
    }
    else
    {
        /* We can't use TSC-offsetting (non-fixed TSC, warp drive active etc.), VM-exit on RDTSC(P). */
        if (!(uProcCtls & VMX_PROC_CTLS_RDTSC_EXIT))
        {
            uProcCtls |= VMX_PROC_CTLS_RDTSC_EXIT;
            int rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC, uProcCtls);
            AssertRC(rc);
            pVmcsInfo->u32ProcCtls = uProcCtls;
        }
        STAM_COUNTER_INC(&pVCpu->hm.s.StatTscIntercept);
    }
}


/**
 * Gets the IEM exception flags for the specified vector and IDT vectoring /
 * VM-exit interruption info type.
 *
 * @returns The IEM exception flags.
 * @param   uVector         The event vector.
 * @param   uVmxEventType   The VMX event type.
 *
 * @remarks This function currently only constructs flags required for
 *          IEMEvaluateRecursiveXcpt and not the complete flags (e.g, error-code
 *          and CR2 aspects of an exception are not included).
 */
static uint32_t hmR0VmxGetIemXcptFlags(uint8_t uVector, uint32_t uVmxEventType)
{
    uint32_t fIemXcptFlags;
    switch (uVmxEventType)
    {
        case VMX_IDT_VECTORING_INFO_TYPE_HW_XCPT:
        case VMX_IDT_VECTORING_INFO_TYPE_NMI:
            fIemXcptFlags = IEM_XCPT_FLAGS_T_CPU_XCPT;
            break;

        case VMX_IDT_VECTORING_INFO_TYPE_EXT_INT:
            fIemXcptFlags = IEM_XCPT_FLAGS_T_EXT_INT;
            break;

        case VMX_IDT_VECTORING_INFO_TYPE_PRIV_SW_XCPT:
            fIemXcptFlags = IEM_XCPT_FLAGS_T_SOFT_INT | IEM_XCPT_FLAGS_ICEBP_INSTR;
            break;

        case VMX_IDT_VECTORING_INFO_TYPE_SW_XCPT:
        {
            fIemXcptFlags = IEM_XCPT_FLAGS_T_SOFT_INT;
            if (uVector == X86_XCPT_BP)
                fIemXcptFlags |= IEM_XCPT_FLAGS_BP_INSTR;
            else if (uVector == X86_XCPT_OF)
                fIemXcptFlags |= IEM_XCPT_FLAGS_OF_INSTR;
            else
            {
                fIemXcptFlags = 0;
                AssertMsgFailed(("Unexpected vector for software exception. uVector=%#x", uVector));
            }
            break;
        }

        case VMX_IDT_VECTORING_INFO_TYPE_SW_INT:
            fIemXcptFlags = IEM_XCPT_FLAGS_T_SOFT_INT;
            break;

        default:
            fIemXcptFlags = 0;
            AssertMsgFailed(("Unexpected vector type! uVmxEventType=%#x uVector=%#x", uVmxEventType, uVector));
            break;
    }
    return fIemXcptFlags;
}


/**
 * Sets an event as a pending event to be injected into the guest.
 *
 * @param   pVCpu               The cross context virtual CPU structure.
 * @param   u32IntInfo          The VM-entry interruption-information field.
 * @param   cbInstr             The VM-entry instruction length in bytes (for software
 *                              interrupts, exceptions and privileged software
 *                              exceptions).
 * @param   u32ErrCode          The VM-entry exception error code.
 * @param   GCPtrFaultAddress   The fault-address (CR2) in case it's a
 *                              page-fault.
 */
DECLINLINE(void) hmR0VmxSetPendingEvent(PVMCPU pVCpu, uint32_t u32IntInfo, uint32_t cbInstr, uint32_t u32ErrCode,
                                        RTGCUINTPTR GCPtrFaultAddress)
{
    Assert(!pVCpu->hm.s.Event.fPending);
    pVCpu->hm.s.Event.fPending          = true;
    pVCpu->hm.s.Event.u64IntInfo        = u32IntInfo;
    pVCpu->hm.s.Event.u32ErrCode        = u32ErrCode;
    pVCpu->hm.s.Event.cbInstr           = cbInstr;
    pVCpu->hm.s.Event.GCPtrFaultAddress = GCPtrFaultAddress;
}


/**
 * Sets an external interrupt as pending-for-injection into the VM.
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   u8Interrupt     The external interrupt vector.
 */
DECLINLINE(void) hmR0VmxSetPendingExtInt(PVMCPU pVCpu, uint8_t u8Interrupt)
{
    uint32_t const u32IntInfo = RT_BF_MAKE(VMX_BF_EXIT_INT_INFO_VECTOR,          u8Interrupt)
                              | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_TYPE,           VMX_ENTRY_INT_INFO_TYPE_EXT_INT)
                              | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_ERR_CODE_VALID, 0)
                              | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_VALID,          1);
    hmR0VmxSetPendingEvent(pVCpu, u32IntInfo, 0 /* cbInstr */, 0 /* u32ErrCode */, 0 /* GCPtrFaultAddress */);
}


/**
 * Sets an NMI (\#NMI) exception as pending-for-injection into the VM.
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 */
DECLINLINE(void) hmR0VmxSetPendingXcptNmi(PVMCPU pVCpu)
{
    uint32_t const u32IntInfo = RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_VECTOR,         X86_XCPT_NMI)
                              | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_TYPE,           VMX_ENTRY_INT_INFO_TYPE_NMI)
                              | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_ERR_CODE_VALID, 0)
                              | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_VALID,          1);
    hmR0VmxSetPendingEvent(pVCpu, u32IntInfo, 0 /* cbInstr */, 0 /* u32ErrCode */, 0 /* GCPtrFaultAddress */);
}


/**
 * Sets a double-fault (\#DF) exception as pending-for-injection into the VM.
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 */
DECLINLINE(void) hmR0VmxSetPendingXcptDF(PVMCPU pVCpu)
{
    uint32_t const u32IntInfo = RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_VECTOR,         X86_XCPT_DF)
                              | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_TYPE,           VMX_EXIT_INT_INFO_TYPE_HW_XCPT)
                              | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_ERR_CODE_VALID, 1)
                              | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_VALID,          1);
    hmR0VmxSetPendingEvent(pVCpu, u32IntInfo, 0 /* cbInstr */, 0 /* u32ErrCode */, 0 /* GCPtrFaultAddress */);
}


/**
 * Sets an invalid-opcode (\#UD) exception as pending-for-injection into the VM.
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 */
DECLINLINE(void) hmR0VmxSetPendingXcptUD(PVMCPU pVCpu)
{
    uint32_t const u32IntInfo = RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_VECTOR,         X86_XCPT_UD)
                              | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_TYPE,           VMX_EXIT_INT_INFO_TYPE_HW_XCPT)
                              | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_ERR_CODE_VALID, 0)
                              | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_VALID,          1);
    hmR0VmxSetPendingEvent(pVCpu, u32IntInfo, 0 /* cbInstr */, 0 /* u32ErrCode */, 0 /* GCPtrFaultAddress */);
}


/**
 * Sets a debug (\#DB) exception as pending-for-injection into the VM.
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 */
DECLINLINE(void) hmR0VmxSetPendingXcptDB(PVMCPU pVCpu)
{
    uint32_t const u32IntInfo = RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_VECTOR,         X86_XCPT_DB)
                              | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_TYPE,           VMX_EXIT_INT_INFO_TYPE_HW_XCPT)
                              | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_ERR_CODE_VALID, 0)
                              | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_VALID,          1);
    hmR0VmxSetPendingEvent(pVCpu, u32IntInfo, 0 /* cbInstr */, 0 /* u32ErrCode */, 0 /* GCPtrFaultAddress */);
}


#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
/**
 * Sets a general-protection (\#GP) exception as pending-for-injection into the VM.
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   u32ErrCode      The error code for the general-protection exception.
 */
DECLINLINE(void) hmR0VmxSetPendingXcptGP(PVMCPU pVCpu, uint32_t u32ErrCode)
{
    uint32_t const u32IntInfo = RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_VECTOR,         X86_XCPT_GP)
                              | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_TYPE,           VMX_EXIT_INT_INFO_TYPE_HW_XCPT)
                              | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_ERR_CODE_VALID, 1)
                              | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_VALID,          1);
    hmR0VmxSetPendingEvent(pVCpu, u32IntInfo, 0 /* cbInstr */, u32ErrCode, 0 /* GCPtrFaultAddress */);
}


/**
 * Sets a stack (\#SS) exception as pending-for-injection into the VM.
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   u32ErrCode      The error code for the stack exception.
 */
DECLINLINE(void) hmR0VmxSetPendingXcptSS(PVMCPU pVCpu, uint32_t u32ErrCode)
{
    uint32_t const u32IntInfo = RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_VECTOR,         X86_XCPT_SS)
                              | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_TYPE,           VMX_EXIT_INT_INFO_TYPE_HW_XCPT)
                              | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_ERR_CODE_VALID, 1)
                              | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_VALID,          1);
    hmR0VmxSetPendingEvent(pVCpu, u32IntInfo, 0 /* cbInstr */, u32ErrCode, 0 /* GCPtrFaultAddress */);
}


/**
 * Decodes the memory operand of an instruction that caused a VM-exit.
 *
 * The Exit qualification field provides the displacement field for memory
 * operand instructions, if any.
 *
 * @returns Strict VBox status code (i.e. informational status codes too).
 * @retval  VINF_SUCCESS if the operand was successfully decoded.
 * @retval  VINF_HM_PENDING_XCPT if an exception was raised while decoding the
 *          operand.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   uExitInstrInfo  The VM-exit instruction information field.
 * @param   enmMemAccess    The memory operand's access type (read or write).
 * @param   GCPtrDisp       The instruction displacement field, if any. For
 *                          RIP-relative addressing pass RIP + displacement here.
 * @param   pGCPtrMem       Where to store the effective destination memory address.
 *
 * @remarks Warning! This function ASSUMES the instruction cannot be used in real or
 *          virtual-8086 mode hence skips those checks while verifying if the
 *          segment is valid.
 */
static VBOXSTRICTRC hmR0VmxDecodeMemOperand(PVMCPU pVCpu, uint32_t uExitInstrInfo, RTGCPTR GCPtrDisp, VMXMEMACCESS enmMemAccess,
                                            PRTGCPTR pGCPtrMem)
{
    Assert(pGCPtrMem);
    Assert(!CPUMIsGuestInRealOrV86Mode(pVCpu));
    HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_RIP | CPUMCTX_EXTRN_RSP | CPUMCTX_EXTRN_SREG_MASK | CPUMCTX_EXTRN_EFER
                              | CPUMCTX_EXTRN_CR0);

    static uint64_t const s_auAddrSizeMasks[]   = { UINT64_C(0xffff), UINT64_C(0xffffffff), UINT64_C(0xffffffffffffffff) };
    static uint64_t const s_auAccessSizeMasks[] = { sizeof(uint16_t), sizeof(uint32_t), sizeof(uint64_t) };
    AssertCompile(RT_ELEMENTS(s_auAccessSizeMasks) == RT_ELEMENTS(s_auAddrSizeMasks));

    VMXEXITINSTRINFO ExitInstrInfo;
    ExitInstrInfo.u = uExitInstrInfo;
    uint8_t const   uAddrSize     =  ExitInstrInfo.All.u3AddrSize;
    uint8_t const   iSegReg       =  ExitInstrInfo.All.iSegReg;
    bool const      fIdxRegValid  = !ExitInstrInfo.All.fIdxRegInvalid;
    uint8_t const   iIdxReg       =  ExitInstrInfo.All.iIdxReg;
    uint8_t const   uScale        =  ExitInstrInfo.All.u2Scaling;
    bool const      fBaseRegValid = !ExitInstrInfo.All.fBaseRegInvalid;
    uint8_t const   iBaseReg      =  ExitInstrInfo.All.iBaseReg;
    bool const      fIsMemOperand = !ExitInstrInfo.All.fIsRegOperand;
    bool const      fIsLongMode   =  CPUMIsGuestInLongModeEx(&pVCpu->cpum.GstCtx);

    /*
     * Validate instruction information.
     * This shouldn't happen on real hardware but useful while testing our nested hardware-virtualization code.
     */
    AssertLogRelMsgReturn(uAddrSize < RT_ELEMENTS(s_auAddrSizeMasks),
                          ("Invalid address size. ExitInstrInfo=%#RX32\n", ExitInstrInfo.u), VERR_VMX_IPE_1);
    AssertLogRelMsgReturn(iSegReg  < X86_SREG_COUNT,
                          ("Invalid segment register. ExitInstrInfo=%#RX32\n", ExitInstrInfo.u), VERR_VMX_IPE_2);
    AssertLogRelMsgReturn(fIsMemOperand,
                          ("Expected memory operand. ExitInstrInfo=%#RX32\n", ExitInstrInfo.u), VERR_VMX_IPE_3);

    /*
     * Compute the complete effective address.
     *
     * See AMD instruction spec. 1.4.2 "SIB Byte Format"
     * See AMD spec. 4.5.2 "Segment Registers".
     */
    RTGCPTR GCPtrMem = GCPtrDisp;
    if (fBaseRegValid)
        GCPtrMem += pVCpu->cpum.GstCtx.aGRegs[iBaseReg].u64;
    if (fIdxRegValid)
        GCPtrMem += pVCpu->cpum.GstCtx.aGRegs[iIdxReg].u64 << uScale;

    RTGCPTR const GCPtrOff = GCPtrMem;
    if (   !fIsLongMode
        || iSegReg >= X86_SREG_FS)
        GCPtrMem += pVCpu->cpum.GstCtx.aSRegs[iSegReg].u64Base;
    GCPtrMem &= s_auAddrSizeMasks[uAddrSize];

    /*
     * Validate effective address.
     * See AMD spec. 4.5.3 "Segment Registers in 64-Bit Mode".
     */
    uint8_t const cbAccess = s_auAccessSizeMasks[uAddrSize];
    Assert(cbAccess > 0);
    if (fIsLongMode)
    {
        if (X86_IS_CANONICAL(GCPtrMem))
        {
            *pGCPtrMem = GCPtrMem;
            return VINF_SUCCESS;
        }

        /** @todo r=ramshankar: We should probably raise \#SS or \#GP. See AMD spec. 4.12.2
         *        "Data Limit Checks in 64-bit Mode". */
        Log4Func(("Long mode effective address is not canonical GCPtrMem=%#RX64\n", GCPtrMem));
        hmR0VmxSetPendingXcptGP(pVCpu, 0);
        return VINF_HM_PENDING_XCPT;
    }

    /*
     * This is a watered down version of iemMemApplySegment().
     * Parts that are not applicable for VMX instructions like real-or-v8086 mode
     * and segment CPL/DPL checks are skipped.
     */
    RTGCPTR32 const GCPtrFirst32 = (RTGCPTR32)GCPtrOff;
    RTGCPTR32 const GCPtrLast32  = GCPtrFirst32 + cbAccess - 1;
    PCCPUMSELREG    pSel         = &pVCpu->cpum.GstCtx.aSRegs[iSegReg];

    /* Check if the segment is present and usable. */
    if (    pSel->Attr.n.u1Present
        && !pSel->Attr.n.u1Unusable)
    {
        Assert(pSel->Attr.n.u1DescType);
        if (!(pSel->Attr.n.u4Type & X86_SEL_TYPE_CODE))
        {
            /* Check permissions for the data segment. */
            if (   enmMemAccess == VMXMEMACCESS_WRITE
                && !(pSel->Attr.n.u4Type & X86_SEL_TYPE_WRITE))
            {
                Log4Func(("Data segment access invalid. iSegReg=%#x Attr=%#RX32\n", iSegReg, pSel->Attr.u));
                hmR0VmxSetPendingXcptGP(pVCpu, iSegReg);
                return VINF_HM_PENDING_XCPT;
            }

            /* Check limits if it's a normal data segment. */
            if (!(pSel->Attr.n.u4Type & X86_SEL_TYPE_DOWN))
            {
                if (   GCPtrFirst32 > pSel->u32Limit
                    || GCPtrLast32  > pSel->u32Limit)
                {
                    Log4Func(("Data segment limit exceeded. "
                              "iSegReg=%#x GCPtrFirst32=%#RX32 GCPtrLast32=%#RX32 u32Limit=%#RX32\n", iSegReg, GCPtrFirst32,
                              GCPtrLast32, pSel->u32Limit));
                    if (iSegReg == X86_SREG_SS)
                        hmR0VmxSetPendingXcptSS(pVCpu, 0);
                    else
                        hmR0VmxSetPendingXcptGP(pVCpu, 0);
                    return VINF_HM_PENDING_XCPT;
                }
            }
            else
            {
               /* Check limits if it's an expand-down data segment.
                  Note! The upper boundary is defined by the B bit, not the G bit! */
               if (   GCPtrFirst32 < pSel->u32Limit + UINT32_C(1)
                   || GCPtrLast32  > (pSel->Attr.n.u1DefBig ? UINT32_MAX : UINT32_C(0xffff)))
               {
                   Log4Func(("Expand-down data segment limit exceeded. "
                             "iSegReg=%#x GCPtrFirst32=%#RX32 GCPtrLast32=%#RX32 u32Limit=%#RX32\n", iSegReg, GCPtrFirst32,
                             GCPtrLast32, pSel->u32Limit));
                   if (iSegReg == X86_SREG_SS)
                       hmR0VmxSetPendingXcptSS(pVCpu, 0);
                   else
                       hmR0VmxSetPendingXcptGP(pVCpu, 0);
                   return VINF_HM_PENDING_XCPT;
               }
            }
        }
        else
        {
            /* Check permissions for the code segment. */
            if (   enmMemAccess == VMXMEMACCESS_WRITE
                || (   enmMemAccess == VMXMEMACCESS_READ
                    && !(pSel->Attr.n.u4Type & X86_SEL_TYPE_READ)))
            {
                Log4Func(("Code segment access invalid. Attr=%#RX32\n", pSel->Attr.u));
                Assert(!CPUMIsGuestInRealOrV86ModeEx(&pVCpu->cpum.GstCtx));
                hmR0VmxSetPendingXcptGP(pVCpu, 0);
                return VINF_HM_PENDING_XCPT;
            }

            /* Check limits for the code segment (normal/expand-down not applicable for code segments). */
            if (   GCPtrFirst32 > pSel->u32Limit
                || GCPtrLast32  > pSel->u32Limit)
            {
                Log4Func(("Code segment limit exceeded. GCPtrFirst32=%#RX32 GCPtrLast32=%#RX32 u32Limit=%#RX32\n",
                          GCPtrFirst32, GCPtrLast32, pSel->u32Limit));
                if (iSegReg == X86_SREG_SS)
                    hmR0VmxSetPendingXcptSS(pVCpu, 0);
                else
                    hmR0VmxSetPendingXcptGP(pVCpu, 0);
                return VINF_HM_PENDING_XCPT;
            }
        }
    }
    else
    {
        Log4Func(("Not present or unusable segment. iSegReg=%#x Attr=%#RX32\n", iSegReg, pSel->Attr.u));
        hmR0VmxSetPendingXcptGP(pVCpu, 0);
        return VINF_HM_PENDING_XCPT;
    }

    *pGCPtrMem = GCPtrMem;
    return VINF_SUCCESS;
}


/**
 * Perform the relevant VMX instruction checks for VM-exits that occurred due to the
 * guest attempting to execute a VMX instruction.
 *
 * @returns Strict VBox status code (i.e. informational status codes too).
 * @retval  VINF_SUCCESS if we should continue handling the VM-exit.
 * @retval  VINF_HM_PENDING_XCPT if an exception was raised.
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   uExitReason     The VM-exit reason.
 *
 * @todo    NSTVMX: Document other error codes when VM-exit is implemented.
 * @remarks No-long-jump zone!!!
 */
static VBOXSTRICTRC hmR0VmxCheckExitDueToVmxInstr(PVMCPU pVCpu, uint32_t uExitReason)
{
    HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_CR0 | CPUMCTX_EXTRN_RFLAGS | CPUMCTX_EXTRN_SS
                              | CPUMCTX_EXTRN_CS  | CPUMCTX_EXTRN_EFER);

    /*
     * The physical CPU would have already checked the CPU mode/code segment.
     * We shall just assert here for paranoia.
     * See Intel spec. 25.1.1 "Relative Priority of Faults and VM Exits".
     */
    Assert(!CPUMIsGuestInRealOrV86ModeEx(&pVCpu->cpum.GstCtx));
    Assert(   !CPUMIsGuestInLongModeEx(&pVCpu->cpum.GstCtx)
           ||  CPUMIsGuestIn64BitCodeEx(&pVCpu->cpum.GstCtx));

    if (uExitReason == VMX_EXIT_VMXON)
    {
        HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_CR4);

        /*
         * We check CR4.VMXE because it is required to be always set while in VMX operation
         * by physical CPUs and our CR4 read-shadow is only consulted when executing specific
         * instructions (CLTS, LMSW, MOV CR, and SMSW) and thus doesn't affect CPU operation
         * otherwise (i.e. physical CPU won't automatically #UD if Cr4Shadow.VMXE is 0).
         */
        if (!CPUMIsGuestVmxEnabled(&pVCpu->cpum.GstCtx))
        {
            Log4Func(("CR4.VMXE is not set -> #UD\n"));
            hmR0VmxSetPendingXcptUD(pVCpu);
            return VINF_HM_PENDING_XCPT;
        }
    }
    else if (!CPUMIsGuestInVmxRootMode(&pVCpu->cpum.GstCtx))
    {
        /*
         * The guest has not entered VMX operation but attempted to execute a VMX instruction
         * (other than VMXON), we need to raise a #UD.
         */
        Log4Func(("Not in VMX root mode -> #UD\n"));
        hmR0VmxSetPendingXcptUD(pVCpu);
        return VINF_HM_PENDING_XCPT;
    }

    /* All other checks (including VM-exit intercepts) are handled by IEM instruction emulation. */
    return VINF_SUCCESS;
}
#endif /* VBOX_WITH_NESTED_HWVIRT_VMX */


static void hmR0VmxFixUnusableSegRegAttr(PVMCPU pVCpu, PCPUMSELREG pSelReg, uint32_t idxSel)
{
    Assert(pSelReg->Attr.u & X86DESCATTR_UNUSABLE);

    /*
     * If VT-x marks the segment as unusable, most other bits remain undefined:
     *   - For CS the L, D and G bits have meaning.
     *   - For SS the DPL has meaning (it -is- the CPL for Intel and VBox).
     *   - For the remaining data segments no bits are defined.
     *
     * The present bit and the unusable bit has been observed to be set at the
     * same time (the selector was supposed to be invalid as we started executing
     * a V8086 interrupt in ring-0).
     *
     * What should be important for the rest of the VBox code, is that the P bit is
     * cleared.  Some of the other VBox code recognizes the unusable bit, but
     * AMD-V certainly don't, and REM doesn't really either.  So, to be on the
     * safe side here, we'll strip off P and other bits we don't care about.  If
     * any code breaks because Attr.u != 0 when Sel < 4, it should be fixed.
     *
     * See Intel spec. 27.3.2 "Saving Segment Registers and Descriptor-Table Registers".
     */
#ifdef VBOX_STRICT
    uint32_t const uAttr = pSelReg->Attr.u;
#endif

    /* Masking off: X86DESCATTR_P, X86DESCATTR_LIMIT_HIGH, and X86DESCATTR_AVL. The latter two are really irrelevant. */
    pSelReg->Attr.u &= X86DESCATTR_UNUSABLE | X86DESCATTR_L    | X86DESCATTR_D  | X86DESCATTR_G
                     | X86DESCATTR_DPL      | X86DESCATTR_TYPE | X86DESCATTR_DT;

#ifdef VBOX_STRICT
    VMMRZCallRing3Disable(pVCpu);
    Log4Func(("Unusable %#x: sel=%#x attr=%#x -> %#x\n", idxSel, pSelReg->Sel, uAttr, pSelReg->Attr.u));
# ifdef DEBUG_bird
    AssertMsg((uAttr & ~X86DESCATTR_P) == pSelReg->Attr.u,
              ("%#x: %#x != %#x (sel=%#x base=%#llx limit=%#x)\n",
               idxSel, uAttr, pSelReg->Attr.u, pSelReg->Sel, pSelReg->u64Base, pSelReg->u32Limit));
# endif
    VMMRZCallRing3Enable(pVCpu);
    NOREF(uAttr);
#endif
    RT_NOREF2(pVCpu, idxSel);
}


/**
 * Imports a guest segment register from the current VMCS into the guest-CPU
 * context.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   iSegReg     The segment register number (X86_SREG_XXX).
 *
 * @remarks Called with interrupts and/or preemption disabled, try not to assert and
 *          do not log!
 */
static int hmR0VmxImportGuestSegReg(PVMCPU pVCpu, uint8_t iSegReg)
{
    Assert(iSegReg < X86_SREG_COUNT);

    uint32_t const idxSel   = g_aVmcsSegSel[iSegReg];
    uint32_t const idxLimit = g_aVmcsSegLimit[iSegReg];
    uint32_t const idxAttr  = g_aVmcsSegAttr[iSegReg];
#ifdef VMX_USE_CACHED_VMCS_ACCESSES
    uint32_t const idxBase  = g_aVmcsCacheSegBase[iSegReg];
#else
    uint32_t const idxBase  = g_aVmcsSegBase[iSegReg];
#endif
    uint64_t u64Base;
    uint32_t u32Sel, u32Limit, u32Attr;
    int rc = VMXReadVmcs32(idxSel,   &u32Sel);
    rc    |= VMXReadVmcs32(idxLimit, &u32Limit);
    rc    |= VMXReadVmcs32(idxAttr,  &u32Attr);
    rc    |= VMXReadVmcsGstNByIdxVal(idxBase, &u64Base);
    if (RT_SUCCESS(rc))
    {
        PCPUMSELREG pSelReg = &pVCpu->cpum.GstCtx.aSRegs[iSegReg];
        pSelReg->Sel      = u32Sel;
        pSelReg->ValidSel = u32Sel;
        pSelReg->fFlags   = CPUMSELREG_FLAGS_VALID;
        pSelReg->u32Limit = u32Limit;
        pSelReg->u64Base  = u64Base;
        pSelReg->Attr.u   = u32Attr;
        if (u32Attr & X86DESCATTR_UNUSABLE)
            hmR0VmxFixUnusableSegRegAttr(pVCpu, pSelReg, idxSel);
    }
    return rc;
}


/**
 * Imports the guest LDTR from the current VMCS into the guest-CPU context.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 *
 * @remarks Called with interrupts and/or preemption disabled, try not to assert and
 *          do not log!
 */
static int hmR0VmxImportGuestLdtr(PVMCPU pVCpu)
{
    uint64_t u64Base;
    uint32_t u32Sel, u32Limit, u32Attr;
    int rc = VMXReadVmcs32(VMX_VMCS16_GUEST_LDTR_SEL,           &u32Sel);
    rc    |= VMXReadVmcs32(VMX_VMCS32_GUEST_LDTR_LIMIT,         &u32Limit);
    rc    |= VMXReadVmcs32(VMX_VMCS32_GUEST_LDTR_ACCESS_RIGHTS, &u32Attr);
    rc    |= VMXReadVmcsGstN(VMX_VMCS_GUEST_LDTR_BASE,          &u64Base);
    if (RT_SUCCESS(rc))
    {
        pVCpu->cpum.GstCtx.ldtr.Sel      = u32Sel;
        pVCpu->cpum.GstCtx.ldtr.ValidSel = u32Sel;
        pVCpu->cpum.GstCtx.ldtr.fFlags   = CPUMSELREG_FLAGS_VALID;
        pVCpu->cpum.GstCtx.ldtr.u32Limit = u32Limit;
        pVCpu->cpum.GstCtx.ldtr.u64Base  = u64Base;
        pVCpu->cpum.GstCtx.ldtr.Attr.u   = u32Attr;
        if (u32Attr & X86DESCATTR_UNUSABLE)
            hmR0VmxFixUnusableSegRegAttr(pVCpu, &pVCpu->cpum.GstCtx.ldtr, VMX_VMCS16_GUEST_LDTR_SEL);
    }
    return rc;
}


/**
 * Imports the guest TR from the current VMCS into the guest-CPU context.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 *
 * @remarks Called with interrupts and/or preemption disabled, try not to assert and
 *          do not log!
 */
static int hmR0VmxImportGuestTr(PVMCPU pVCpu)
{
    uint32_t u32Sel, u32Limit, u32Attr;
    uint64_t u64Base;
    int rc = VMXReadVmcs32(VMX_VMCS16_GUEST_TR_SEL,           &u32Sel);
    rc    |= VMXReadVmcs32(VMX_VMCS32_GUEST_TR_LIMIT,         &u32Limit);
    rc    |= VMXReadVmcs32(VMX_VMCS32_GUEST_TR_ACCESS_RIGHTS, &u32Attr);
    rc    |= VMXReadVmcsGstN(VMX_VMCS_GUEST_TR_BASE,          &u64Base);
    AssertRCReturn(rc, rc);

    pVCpu->cpum.GstCtx.tr.Sel      = u32Sel;
    pVCpu->cpum.GstCtx.tr.ValidSel = u32Sel;
    pVCpu->cpum.GstCtx.tr.fFlags   = CPUMSELREG_FLAGS_VALID;
    pVCpu->cpum.GstCtx.tr.u32Limit = u32Limit;
    pVCpu->cpum.GstCtx.tr.u64Base  = u64Base;
    pVCpu->cpum.GstCtx.tr.Attr.u   = u32Attr;
    /* TR is the only selector that can never be unusable. */
    Assert(!(u32Attr & X86DESCATTR_UNUSABLE));
    return VINF_SUCCESS;
}


/**
 * Imports the guest RIP from the VMCS back into the guest-CPU context.
 *
 * @returns VBox status code.
 * @param   pVCpu   The cross context virtual CPU structure.
 *
 * @remarks Called with interrupts and/or preemption disabled, should not assert!
 * @remarks Do -not- call this function directly, use hmR0VmxImportGuestState()
 *          instead!!!
 */
static int hmR0VmxImportGuestRip(PVMCPU pVCpu)
{
    uint64_t u64Val;
    PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
    if (pCtx->fExtrn & CPUMCTX_EXTRN_RIP)
    {
        int rc = VMXReadVmcsGstN(VMX_VMCS_GUEST_RIP, &u64Val);
        if (RT_SUCCESS(rc))
        {
            pCtx->rip = u64Val;
            EMR0HistoryUpdatePC(pVCpu, pCtx->rip, false);
            pCtx->fExtrn &= ~CPUMCTX_EXTRN_RIP;
        }
        return rc;
    }
    return VINF_SUCCESS;
}


/**
 * Imports the guest RFLAGS from the VMCS back into the guest-CPU context.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pVmcsInfo   The VMCS info. object.
 *
 * @remarks Called with interrupts and/or preemption disabled, should not assert!
 * @remarks Do -not- call this function directly, use hmR0VmxImportGuestState()
 *          instead!!!
 */
static int hmR0VmxImportGuestRFlags(PVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo)
{
    uint32_t u32Val;
    PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
    if (pCtx->fExtrn & CPUMCTX_EXTRN_RFLAGS)
    {
        int rc = VMXReadVmcs32(VMX_VMCS_GUEST_RFLAGS, &u32Val);
        if (RT_SUCCESS(rc))
        {
            pCtx->eflags.u32 = u32Val;

            /* Restore eflags for real-on-v86-mode hack. */
            if (pVmcsInfo->RealMode.fRealOnV86Active)
            {
                pCtx->eflags.Bits.u1VM   = 0;
                pCtx->eflags.Bits.u2IOPL = pVmcsInfo->RealMode.Eflags.Bits.u2IOPL;
            }
        }
        pCtx->fExtrn &= ~CPUMCTX_EXTRN_RFLAGS;
        return rc;
    }
    return VINF_SUCCESS;
}


/**
 * Imports the guest interruptibility-state from the VMCS back into the guest-CPU
 * context.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pVmcsInfo   The VMCS info. object.
 *
 * @remarks Called with interrupts and/or preemption disabled, try not to assert and
 *          do not log!
 * @remarks Do -not- call this function directly, use hmR0VmxImportGuestState()
 *          instead!!!
 */
static int hmR0VmxImportGuestIntrState(PVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo)
{
    uint32_t u32Val;
    int rc = VMXReadVmcs32(VMX_VMCS32_GUEST_INT_STATE, &u32Val);
    if (RT_SUCCESS(rc))
    {
        if (!u32Val)
        {
            if (VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_INHIBIT_INTERRUPTS))
                VMCPU_FF_CLEAR(pVCpu, VMCPU_FF_INHIBIT_INTERRUPTS);

            CPUMSetGuestNmiBlocking(pVCpu, false);
        }
        else
        {
            /*
             * We must import RIP here to set our EM interrupt-inhibited state.
             * We also import RFLAGS as our code that evaluates pending interrupts
             * before VM-entry requires it.
             */
            rc  = hmR0VmxImportGuestRip(pVCpu);
            rc |= hmR0VmxImportGuestRFlags(pVCpu, pVmcsInfo);
            if (RT_SUCCESS(rc))
            {
                if (u32Val & (VMX_VMCS_GUEST_INT_STATE_BLOCK_MOVSS | VMX_VMCS_GUEST_INT_STATE_BLOCK_STI))
                    EMSetInhibitInterruptsPC(pVCpu, pVCpu->cpum.GstCtx.rip);
                else if (VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_INHIBIT_INTERRUPTS))
                    VMCPU_FF_CLEAR(pVCpu, VMCPU_FF_INHIBIT_INTERRUPTS);

                bool const fNmiBlocking = RT_BOOL(u32Val & VMX_VMCS_GUEST_INT_STATE_BLOCK_NMI);
                CPUMSetGuestNmiBlocking(pVCpu, fNmiBlocking);
            }
        }
    }
    return rc;
}


/**
 * Worker for VMXR0ImportStateOnDemand.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pVmcsInfo   The VMCS info. object.
 * @param   fWhat       What to import, CPUMCTX_EXTRN_XXX.
 */
static int hmR0VmxImportGuestState(PVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo, uint64_t fWhat)
{
#define VMXLOCAL_BREAK_RC(a_rc) \
    if (RT_SUCCESS(a_rc))       \
    { }                         \
    else                        \
        break

    int      rc   = VINF_SUCCESS;
    PVM      pVM  = pVCpu->CTX_SUFF(pVM);
    PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
    uint64_t u64Val;
    uint32_t u32Val;

    /*
     * Note! This is hack to workaround a mysterious BSOD observed with release builds
     *       on Windows 10 64-bit hosts. Profile and debug builds are not affected and
     *       neither are other host platforms.
     *
     *       Committing this temporarily as it prevents BSOD.
     *
     * Update: This is very likely a compiler optimization bug, see @bugref{9180}.
     */
#ifdef RT_OS_WINDOWS
    if (pVM == 0 || pVM == (void *)(uintptr_t)-1)
        return VERR_HM_IPE_1;
#endif

    STAM_PROFILE_ADV_START(&pVCpu->hm.s.StatImportGuestState, x);

    /*
     * We disable interrupts to make the updating of the state and in particular
     * the fExtrn modification atomic wrt to preemption hooks.
     */
    RTCCUINTREG const fEFlags = ASMIntDisableFlags();

    fWhat &= pCtx->fExtrn;
    if (fWhat)
    {
        do
        {
            if (fWhat & CPUMCTX_EXTRN_RIP)
            {
                rc = hmR0VmxImportGuestRip(pVCpu);
                VMXLOCAL_BREAK_RC(rc);
            }

            if (fWhat & CPUMCTX_EXTRN_RFLAGS)
            {
                rc = hmR0VmxImportGuestRFlags(pVCpu, pVmcsInfo);
                VMXLOCAL_BREAK_RC(rc);
            }

            if (fWhat & CPUMCTX_EXTRN_HM_VMX_INT_STATE)
            {
                rc = hmR0VmxImportGuestIntrState(pVCpu, pVmcsInfo);
                VMXLOCAL_BREAK_RC(rc);
            }

            if (fWhat & CPUMCTX_EXTRN_RSP)
            {
                rc = VMXReadVmcsGstN(VMX_VMCS_GUEST_RSP, &u64Val);
                VMXLOCAL_BREAK_RC(rc);
                pCtx->rsp = u64Val;
            }

            if (fWhat & CPUMCTX_EXTRN_SREG_MASK)
            {
                bool const fRealOnV86Active = pVmcsInfo->RealMode.fRealOnV86Active;
                if (fWhat & CPUMCTX_EXTRN_CS)
                {
                    rc |= hmR0VmxImportGuestSegReg(pVCpu, X86_SREG_CS);
                    rc |= hmR0VmxImportGuestRip(pVCpu);
                    if (fRealOnV86Active)
                        pCtx->cs.Attr.u = pVmcsInfo->RealMode.AttrCS.u;
                    EMR0HistoryUpdatePC(pVCpu, pCtx->cs.u64Base + pCtx->rip, true /* fFlattened */);
                }
                if (fWhat & CPUMCTX_EXTRN_SS)
                {
                    rc |= hmR0VmxImportGuestSegReg(pVCpu, X86_SREG_SS);
                    if (fRealOnV86Active)
                        pCtx->ss.Attr.u = pVmcsInfo->RealMode.AttrSS.u;
                }
                if (fWhat & CPUMCTX_EXTRN_DS)
                {
                    rc |= hmR0VmxImportGuestSegReg(pVCpu, X86_SREG_DS);
                    if (fRealOnV86Active)
                        pCtx->ds.Attr.u = pVmcsInfo->RealMode.AttrDS.u;
                }
                if (fWhat & CPUMCTX_EXTRN_ES)
                {
                    rc |= hmR0VmxImportGuestSegReg(pVCpu, X86_SREG_ES);
                    if (fRealOnV86Active)
                        pCtx->es.Attr.u = pVmcsInfo->RealMode.AttrES.u;
                }
                if (fWhat & CPUMCTX_EXTRN_FS)
                {
                    rc |= hmR0VmxImportGuestSegReg(pVCpu, X86_SREG_FS);
                    if (fRealOnV86Active)
                        pCtx->fs.Attr.u = pVmcsInfo->RealMode.AttrFS.u;
                }
                if (fWhat & CPUMCTX_EXTRN_GS)
                {
                    rc |= hmR0VmxImportGuestSegReg(pVCpu, X86_SREG_GS);
                    if (fRealOnV86Active)
                        pCtx->gs.Attr.u = pVmcsInfo->RealMode.AttrGS.u;
                }
                VMXLOCAL_BREAK_RC(rc);
            }

            if (fWhat & CPUMCTX_EXTRN_TABLE_MASK)
            {
                if (fWhat & CPUMCTX_EXTRN_LDTR)
                    rc |= hmR0VmxImportGuestLdtr(pVCpu);

                if (fWhat & CPUMCTX_EXTRN_GDTR)
                {
                    rc |= VMXReadVmcsGstN(VMX_VMCS_GUEST_GDTR_BASE,  &u64Val);
                    rc |= VMXReadVmcs32(VMX_VMCS32_GUEST_GDTR_LIMIT, &u32Val);
                    pCtx->gdtr.pGdt  = u64Val;
                    pCtx->gdtr.cbGdt = u32Val;
                }

                /* Guest IDTR. */
                if (fWhat & CPUMCTX_EXTRN_IDTR)
                {
                    rc |= VMXReadVmcsGstN(VMX_VMCS_GUEST_IDTR_BASE,  &u64Val);
                    rc |= VMXReadVmcs32(VMX_VMCS32_GUEST_IDTR_LIMIT, &u32Val);
                    pCtx->idtr.pIdt  = u64Val;
                    pCtx->idtr.cbIdt = u32Val;
                }

                /* Guest TR. */
                if (fWhat & CPUMCTX_EXTRN_TR)
                {
                    /* Real-mode emulation using virtual-8086 mode has the fake TSS (pRealModeTSS) in TR,
                       don't need to import that one. */
                    if (!pVmcsInfo->RealMode.fRealOnV86Active)
                        rc |= hmR0VmxImportGuestTr(pVCpu);
                }
                VMXLOCAL_BREAK_RC(rc);
            }

            if (fWhat & CPUMCTX_EXTRN_DR7)
            {
                if (!pVCpu->hm.s.fUsingHyperDR7)
                {
                    /* Upper 32-bits are always zero. See Intel spec. 2.7.3 "Loading and Storing Debug Registers". */
                    rc = VMXReadVmcs32(VMX_VMCS_GUEST_DR7, &u32Val);
                    VMXLOCAL_BREAK_RC(rc);
                    pCtx->dr[7] = u32Val;
                }
            }

            if (fWhat & CPUMCTX_EXTRN_SYSENTER_MSRS)
            {
                rc  = VMXReadVmcsGstN(VMX_VMCS_GUEST_SYSENTER_EIP, &pCtx->SysEnter.eip);
                rc |= VMXReadVmcsGstN(VMX_VMCS_GUEST_SYSENTER_ESP, &pCtx->SysEnter.esp);
                rc |= VMXReadVmcs32(VMX_VMCS32_GUEST_SYSENTER_CS,  &u32Val);
                pCtx->SysEnter.cs = u32Val;
                VMXLOCAL_BREAK_RC(rc);
            }

#if HC_ARCH_BITS == 64
            if (fWhat & CPUMCTX_EXTRN_KERNEL_GS_BASE)
            {
                if (   pVM->hm.s.fAllow64BitGuests
                    && (pVCpu->hm.s.vmx.fLazyMsrs & VMX_LAZY_MSRS_LOADED_GUEST))
                    pCtx->msrKERNELGSBASE = ASMRdMsr(MSR_K8_KERNEL_GS_BASE);
            }

            if (fWhat & CPUMCTX_EXTRN_SYSCALL_MSRS)
            {
                if (   pVM->hm.s.fAllow64BitGuests
                    && (pVCpu->hm.s.vmx.fLazyMsrs & VMX_LAZY_MSRS_LOADED_GUEST))
                {
                    pCtx->msrLSTAR  = ASMRdMsr(MSR_K8_LSTAR);
                    pCtx->msrSTAR   = ASMRdMsr(MSR_K6_STAR);
                    pCtx->msrSFMASK = ASMRdMsr(MSR_K8_SF_MASK);
                }
            }
#endif

            if (   (fWhat & (CPUMCTX_EXTRN_TSC_AUX | CPUMCTX_EXTRN_OTHER_MSRS))
#if HC_ARCH_BITS == 32
                || (fWhat & (CPUMCTX_EXTRN_KERNEL_GS_BASE | CPUMCTX_EXTRN_SYSCALL_MSRS))
#endif
                )
            {
                PCVMXAUTOMSR   pMsrs = (PCVMXAUTOMSR)pVmcsInfo->pvGuestMsrStore;
                uint32_t const cMsrs = pVmcsInfo->cExitMsrStore;
                Assert(pMsrs);
                Assert(cMsrs <= VMX_MISC_MAX_MSRS(pVM->hm.s.vmx.Msrs.u64Misc));
                Assert(sizeof(*pMsrs) * cMsrs <= X86_PAGE_4K_SIZE);
                for (uint32_t i = 0; i < cMsrs; i++)
                {
                    uint32_t const idMsr = pMsrs[i].u32Msr;
                    switch (idMsr)
                    {
                        case MSR_K8_TSC_AUX:        CPUMSetGuestTscAux(pVCpu, pMsrs[i].u64Value);     break;
                        case MSR_IA32_SPEC_CTRL:    CPUMSetGuestSpecCtrl(pVCpu, pMsrs[i].u64Value);   break;
                        case MSR_K6_EFER:           /* Can't be changed without causing a VM-exit */  break;
#if HC_ARCH_BITS == 32
                        case MSR_K8_LSTAR:          pCtx->msrLSTAR        = pMsrs[i].u64Value;        break;
                        case MSR_K6_STAR:           pCtx->msrSTAR         = pMsrs[i].u64Value;        break;
                        case MSR_K8_SF_MASK:        pCtx->msrSFMASK       = pMsrs[i].u64Value;        break;
                        case MSR_K8_KERNEL_GS_BASE: pCtx->msrKERNELGSBASE = pMsrs[i].u64Value;        break;
#endif
                        default:
                        {
                            pCtx->fExtrn = 0;
                            pVCpu->hm.s.u32HMError = pMsrs->u32Msr;
                            ASMSetFlags(fEFlags);
                            AssertMsgFailed(("Unexpected MSR in auto-load/store area. idMsr=%#RX32 cMsrs=%u\n", idMsr, cMsrs));
                            return VERR_HM_UNEXPECTED_LD_ST_MSR;
                        }
                    }
                }
            }

            if (fWhat & CPUMCTX_EXTRN_CR_MASK)
            {
                uint64_t u64Shadow;
                if (fWhat & CPUMCTX_EXTRN_CR0)
                {
                    /** @todo r=ramshankar: We only read 32-bits here for legacy/convenience reasons,
                     *        remove when we drop 32-bit host w/ 64-bit host support, see
                     *        @bugref{9180#c39}. */
                    rc  = VMXReadVmcs32(VMX_VMCS_GUEST_CR0, &u32Val);
#if HC_ARCH_BITS == 32
                    uint32_t u32Shadow;
                    rc |= VMXReadVmcs32(VMX_VMCS_CTRL_CR0_READ_SHADOW, &u32Shadow);
                    u64Shadow = u32Shadow;
#else
                    rc |= VMXReadVmcs64(VMX_VMCS_CTRL_CR0_READ_SHADOW, &u64Shadow);
#endif
                    VMXLOCAL_BREAK_RC(rc);
                    u64Val = u32Val;
                    u64Val = (u64Val    & ~pVmcsInfo->u64Cr0Mask)
                           | (u64Shadow &  pVmcsInfo->u64Cr0Mask);
#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
                    /*
                     * Reapply the nested-guest's CR0 fixed bits that might have been altered while
                     * exporting the nested-guest CR0 for executing using hardware-assisted VMX.
                     */
                    if (CPUMIsGuestInVmxNonRootMode(pCtx))
                    {
                        u64Val |= pCtx->hwvirt.vmx.Msrs.u64Cr0Fixed0;
                        u64Val &= pCtx->hwvirt.vmx.Msrs.u64Cr0Fixed1;
                    }
#endif
                    VMMRZCallRing3Disable(pVCpu);   /* May call into PGM which has Log statements. */
                    CPUMSetGuestCR0(pVCpu, u64Val);
                    VMMRZCallRing3Enable(pVCpu);
                }

                if (fWhat & CPUMCTX_EXTRN_CR4)
                {
                    /** @todo r=ramshankar: We only read 32-bits here for legacy/convenience reasons,
                     *        remove when we drop 32-bit host w/ 64-bit host support, see
                     *        @bugref{9180#c39}. */
                    rc  = VMXReadVmcs32(VMX_VMCS_GUEST_CR4, &u32Val);
#if HC_ARCH_BITS == 32
                    uint32_t u32Shadow;
                    rc |= VMXReadVmcs32(VMX_VMCS_CTRL_CR4_READ_SHADOW, &u32Shadow);
                    u64Shadow = u32Shadow;
#else
                    rc |= VMXReadVmcs64(VMX_VMCS_CTRL_CR4_READ_SHADOW, &u64Shadow);
#endif
                    VMXLOCAL_BREAK_RC(rc);
                    u64Val = u32Val;
                    u64Val = (u64Val    & ~pVmcsInfo->u64Cr4Mask)
                           | (u64Shadow &  pVmcsInfo->u64Cr4Mask);
#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
                    /*
                     * Reapply the nested-guest's CR4 fixed bits that might have been altered while
                     * exporting the nested-guest CR4 for executing using hardware-assisted VMX.
                     */
                    if (CPUMIsGuestInVmxNonRootMode(pCtx))
                    {
                        u64Val |= pCtx->hwvirt.vmx.Msrs.u64Cr4Fixed0;
                        u64Val &= pCtx->hwvirt.vmx.Msrs.u64Cr4Fixed1;
                    }
#endif
                    pCtx->cr4 = u64Val;
                }

                if (fWhat & CPUMCTX_EXTRN_CR3)
                {
                    /* CR0.PG bit changes are always intercepted, so it's up to date. */
                    if (   pVM->hm.s.vmx.fUnrestrictedGuest
                        || (   pVM->hm.s.fNestedPaging
                            && CPUMIsGuestPagingEnabledEx(pCtx)))
                    {
                        rc = VMXReadVmcsGstN(VMX_VMCS_GUEST_CR3, &u64Val);
                        VMXLOCAL_BREAK_RC(rc);
                        if (pCtx->cr3 != u64Val)
                        {
                            pCtx->cr3 = u64Val;
                            VMCPU_FF_SET(pVCpu, VMCPU_FF_HM_UPDATE_CR3);
                        }

                        /* If the guest is in PAE mode, sync back the PDPE's into the guest state.
                           Note: CR4.PAE, CR0.PG, EFER MSR changes are always intercepted, so they're up to date. */
                        if (CPUMIsGuestInPAEModeEx(pCtx))
                        {
                            rc  = VMXReadVmcs64(VMX_VMCS64_GUEST_PDPTE0_FULL, &pVCpu->hm.s.aPdpes[0].u);
                            rc |= VMXReadVmcs64(VMX_VMCS64_GUEST_PDPTE1_FULL, &pVCpu->hm.s.aPdpes[1].u);
                            rc |= VMXReadVmcs64(VMX_VMCS64_GUEST_PDPTE2_FULL, &pVCpu->hm.s.aPdpes[2].u);
                            rc |= VMXReadVmcs64(VMX_VMCS64_GUEST_PDPTE3_FULL, &pVCpu->hm.s.aPdpes[3].u);
                            VMXLOCAL_BREAK_RC(rc);
                            VMCPU_FF_SET(pVCpu, VMCPU_FF_HM_UPDATE_PAE_PDPES);
                        }
                    }
                }

#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
# if 0
                /** @todo NSTVMX: We handle most of these fields individually by passing it to IEM
                 *        VM-exit handlers as parameters. We would handle it differently when using
                 *        the fast path. */
                /*
                 * The hardware virtualization state currently consists of VMCS fields that may be
                 * modified by execution of the nested-guest (that are not part of the general
                 * guest state) and is visible to guest software. Hence, it is technically part of
                 * the guest-CPU state when executing a nested-guest.
                 */
                if (   (fWhat & CPUMCTX_EXTRN_HWVIRT)
                    && CPUMIsGuestInVmxNonRootMode(pCtx))
                {
                    PVMXVVMCS pGstVmcs = pCtx->hwvirt.vmx.CTX_SUFF(pVmcs);
                    rc  = VMXReadVmcs32(VMX_VMCS32_RO_EXIT_REASON,        &pGstVmcs->u32RoExitReason);
                    rc |= VMXReadVmcsGstN(VMX_VMCS_RO_EXIT_QUALIFICATION, &pGstVmcs->u64RoExitQual.u);
                    VMXLOCAL_BREAK_RC(rc);

                    /*
                     * VM-entry can fail due to invalid-guest state, machine-check events and
                     * MSR loading failures. Other than VM-exit reason and Exit qualification
                     * all other VMCS fields are left unmodified on VM-entry failure.
                     *
                     * See Intel spec. 26.7 "VM-entry Failures During Or After Loading Guest State".
                     */
                    bool const fEntryFailed = VMX_EXIT_REASON_HAS_ENTRY_FAILED(pGstVmcs->u32RoExitReason);
                    if (!fEntryFailed)
                    {
                        /*
                         * Some notes on VMCS fields that may need importing when the fast path
                         * is implemented. Currently we fully emulate VMLAUNCH/VMRESUME in IEM.
                         *
                         * Requires fixing up when using hardware-assisted VMX:
                         *   - VM-exit interruption info: Shouldn't reflect host interrupts/NMIs.
                         *   - VM-exit interruption error code: Cleared to 0 when not appropriate.
                         *   - IDT-vectoring info: Think about this.
                         *   - IDT-vectoring error code: Think about this.
                         *
                         * Emulated:
                         *   - Guest-interruptiblity state: Derived from FFs and RIP.
                         *   - Guest pending debug exceptions: Derived from DR6.
                         *   - Guest activity state: Emulated from EM state.
                         *   - Guest PDPTEs: Currently all 0s since we don't support nested EPT.
                         *   - Entry-interrupt info: Emulated, cleared to 0.
                         */
                        rc |= VMXReadVmcs32(VMX_VMCS32_RO_EXIT_INTERRUPTION_INFO,       &pGstVmcs->u32RoExitIntInfo);
                        rc |= VMXReadVmcs32(VMX_VMCS32_RO_EXIT_INTERRUPTION_ERROR_CODE, &pGstVmcs->u32RoExitIntErrCode);
                        rc |= VMXReadVmcs32(VMX_VMCS32_RO_IDT_VECTORING_INFO,           &pGstVmcs->u32RoIdtVectoringInfo);
                        rc |= VMXReadVmcs32(VMX_VMCS32_RO_IDT_VECTORING_ERROR_CODE,     &pGstVmcs->u32RoIdtVectoringErrCode);
                        rc |= VMXReadVmcs32(VMX_VMCS32_RO_EXIT_INSTR_LENGTH,            &pGstVmcs->u32RoExitInstrLen);
                        rc |= VMXReadVmcs32(VMX_VMCS32_RO_EXIT_INSTR_INFO,              &pGstVmcs->u32RoExitIntInfo);
                        rc |= VMXReadVmcs64(VMX_VMCS64_RO_GUEST_PHYS_ADDR_FULL,         &pGstVmcs->u64RoGuestPhysAddr.u);
                        rc |= VMXReadVmcsGstN(VMX_VMCS_RO_GUEST_LINEAR_ADDR,            &pGstVmcs->u64RoGuestLinearAddr.u);
                        /** @todo NSTVMX: Save and adjust preemption timer value. */
                    }

                    VMXLOCAL_BREAK_RC(rc);
                }
# endif
#endif
            }
        } while (0);

        if (RT_SUCCESS(rc))
        {
            /* Update fExtrn. */
            pCtx->fExtrn &= ~fWhat;

            /* If everything has been imported, clear the HM keeper bit. */
            if (!(pCtx->fExtrn & HMVMX_CPUMCTX_EXTRN_ALL))
            {
                pCtx->fExtrn &= ~CPUMCTX_EXTRN_KEEPER_HM;
                Assert(!pCtx->fExtrn);
            }
        }
    }
    else
        AssertMsg(!pCtx->fExtrn || (pCtx->fExtrn & HMVMX_CPUMCTX_EXTRN_ALL), ("%#RX64\n", pCtx->fExtrn));

    ASMSetFlags(fEFlags);

    STAM_PROFILE_ADV_STOP(& pVCpu->hm.s.StatImportGuestState, x);

    if (RT_SUCCESS(rc))
    { /* likely */ }
    else
        return rc;

    /*
     * Honor any pending CR3 updates.
     *
     * Consider this scenario: VM-exit -> VMMRZCallRing3Enable() -> do stuff that causes a longjmp -> hmR0VmxCallRing3Callback()
     * -> VMMRZCallRing3Disable() -> hmR0VmxImportGuestState() -> Sets VMCPU_FF_HM_UPDATE_CR3 pending -> return from the longjmp
     * -> continue with VM-exit handling -> hmR0VmxImportGuestState() and here we are.
     *
     * The reason for such complicated handling is because VM-exits that call into PGM expect CR3 to be up-to-date and thus
     * if any CR3-saves -before- the VM-exit (longjmp) postponed the CR3 update via the force-flag, any VM-exit handler that
     * calls into PGM when it re-saves CR3 will end up here and we call PGMUpdateCR3(). This is why the code below should
     * -NOT- check if CPUMCTX_EXTRN_CR3 is set!
     *
     * The longjmp exit path can't check these CR3 force-flags and call code that takes a lock again. We cover for it here.
     */
    if (VMMRZCallRing3IsEnabled(pVCpu))
    {
        if (VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_HM_UPDATE_CR3))
        {
            Assert(!(ASMAtomicUoReadU64(&pCtx->fExtrn) & CPUMCTX_EXTRN_CR3));
            PGMUpdateCR3(pVCpu, CPUMGetGuestCR3(pVCpu));
        }

        if (VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_HM_UPDATE_PAE_PDPES))
            PGMGstUpdatePaePdpes(pVCpu, &pVCpu->hm.s.aPdpes[0]);

        Assert(!VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_HM_UPDATE_CR3));
        Assert(!VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_HM_UPDATE_PAE_PDPES));
    }

    return VINF_SUCCESS;
#undef VMXLOCAL_BREAK_RC
}


/**
 * Saves the guest state from the VMCS into the guest-CPU context.
 *
 * @returns VBox status code.
 * @param   pVCpu   The cross context virtual CPU structure.
 * @param   fWhat   What to import, CPUMCTX_EXTRN_XXX.
 */
VMMR0DECL(int) VMXR0ImportStateOnDemand(PVMCPU pVCpu, uint64_t fWhat)
{
    AssertPtr(pVCpu);
    PCVMXVMCSINFO pVmcsInfo = hmGetVmxActiveVmcsInfo(pVCpu);
    return hmR0VmxImportGuestState(pVCpu, pVmcsInfo, fWhat);
}


/**
 * Check per-VM and per-VCPU force flag actions that require us to go back to
 * ring-3 for one reason or another.
 *
 * @returns Strict VBox status code (i.e. informational status codes too)
 * @retval VINF_SUCCESS if we don't have any actions that require going back to
 *         ring-3.
 * @retval VINF_PGM_SYNC_CR3 if we have pending PGM CR3 sync.
 * @retval VINF_EM_PENDING_REQUEST if we have pending requests (like hardware
 *         interrupts)
 * @retval VINF_PGM_POOL_FLUSH_PENDING if PGM is doing a pool flush and requires
 *         all EMTs to be in ring-3.
 * @retval VINF_EM_RAW_TO_R3 if there is pending DMA requests.
 * @retval VINF_EM_NO_MEMORY PGM is out of memory, we need to return
 *         to the EM loop.
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   fStepping       Whether we are single-stepping the guest using the
 *                          hypervisor debugger.
 */
static VBOXSTRICTRC hmR0VmxCheckForceFlags(PVMCPU pVCpu, bool fStepping)
{
    Assert(VMMRZCallRing3IsEnabled(pVCpu));

    /*
     * Update pending interrupts into the APIC's IRR.
     */
    if (VMCPU_FF_TEST_AND_CLEAR(pVCpu, VMCPU_FF_UPDATE_APIC))
        APICUpdatePendingInterrupts(pVCpu);

    /*
     * Anything pending?  Should be more likely than not if we're doing a good job.
     */
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    if (  !fStepping
        ?    !VM_FF_IS_ANY_SET(pVM, VM_FF_HP_R0_PRE_HM_MASK)
          && !VMCPU_FF_IS_ANY_SET(pVCpu, VMCPU_FF_HP_R0_PRE_HM_MASK)
        :    !VM_FF_IS_ANY_SET(pVM, VM_FF_HP_R0_PRE_HM_STEP_MASK)
          && !VMCPU_FF_IS_ANY_SET(pVCpu, VMCPU_FF_HP_R0_PRE_HM_STEP_MASK) )
        return VINF_SUCCESS;

    /* Pending PGM C3 sync. */
    if (VMCPU_FF_IS_ANY_SET(pVCpu,VMCPU_FF_PGM_SYNC_CR3 | VMCPU_FF_PGM_SYNC_CR3_NON_GLOBAL))
    {
        PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
        Assert(!(ASMAtomicUoReadU64(&pCtx->fExtrn) & (CPUMCTX_EXTRN_CR0 | CPUMCTX_EXTRN_CR3 | CPUMCTX_EXTRN_CR4)));
        VBOXSTRICTRC rcStrict2 = PGMSyncCR3(pVCpu, pCtx->cr0, pCtx->cr3, pCtx->cr4,
                                            VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_PGM_SYNC_CR3));
        if (rcStrict2 != VINF_SUCCESS)
        {
            AssertRC(VBOXSTRICTRC_VAL(rcStrict2));
            Log4Func(("PGMSyncCR3 forcing us back to ring-3. rc2=%d\n", VBOXSTRICTRC_VAL(rcStrict2)));
            return rcStrict2;
        }
    }

    /* Pending HM-to-R3 operations (critsects, timers, EMT rendezvous etc.) */
    if (   VM_FF_IS_ANY_SET(pVM, VM_FF_HM_TO_R3_MASK)
        || VMCPU_FF_IS_ANY_SET(pVCpu, VMCPU_FF_HM_TO_R3_MASK))
    {
        STAM_COUNTER_INC(&pVCpu->hm.s.StatSwitchHmToR3FF);
        int rc2 = RT_LIKELY(!VM_FF_IS_SET(pVM, VM_FF_PGM_NO_MEMORY)) ? VINF_EM_RAW_TO_R3 : VINF_EM_NO_MEMORY;
        Log4Func(("HM_TO_R3 forcing us back to ring-3. rc=%d\n", rc2));
        return rc2;
    }

    /* Pending VM request packets, such as hardware interrupts. */
    if (   VM_FF_IS_SET(pVM, VM_FF_REQUEST)
        || VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_REQUEST))
    {
        Log4Func(("Pending VM request forcing us back to ring-3\n"));
        return VINF_EM_PENDING_REQUEST;
    }

    /* Pending PGM pool flushes. */
    if (VM_FF_IS_SET(pVM, VM_FF_PGM_POOL_FLUSH_PENDING))
    {
        Log4Func(("PGM pool flush pending forcing us back to ring-3\n"));
        return VINF_PGM_POOL_FLUSH_PENDING;
    }

    /* Pending DMA requests. */
    if (VM_FF_IS_SET(pVM, VM_FF_PDM_DMA))
    {
        Log4Func(("Pending DMA request forcing us back to ring-3\n"));
        return VINF_EM_RAW_TO_R3;
    }

    return VINF_SUCCESS;
}


/**
 * Converts any TRPM trap into a pending HM event. This is typically used when
 * entering from ring-3 (not longjmp returns).
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 */
static void hmR0VmxTrpmTrapToPendingEvent(PVMCPU pVCpu)
{
    Assert(TRPMHasTrap(pVCpu));
    Assert(!pVCpu->hm.s.Event.fPending);

    uint8_t     uVector;
    TRPMEVENT   enmTrpmEvent;
    RTGCUINT    uErrCode;
    RTGCUINTPTR GCPtrFaultAddress;
    uint8_t     cbInstr;

    int rc = TRPMQueryTrapAll(pVCpu, &uVector, &enmTrpmEvent, &uErrCode, &GCPtrFaultAddress, &cbInstr);
    AssertRC(rc);

    /* Refer Intel spec. 24.8.3 "VM-entry Controls for Event Injection" for the format of u32IntInfo. */
    uint32_t u32IntInfo = uVector | VMX_EXIT_INT_INFO_VALID;
    if (enmTrpmEvent == TRPM_TRAP)
    {
        /** @todo r=ramshankar: TRPM currently offers no way to determine a \#DB that was
         *        generated using INT1 (ICEBP). */
        switch (uVector)
        {
            case X86_XCPT_NMI:
                u32IntInfo |= (VMX_EXIT_INT_INFO_TYPE_NMI << VMX_EXIT_INT_INFO_TYPE_SHIFT);
                break;

            case X86_XCPT_BP:
            case X86_XCPT_OF:
                u32IntInfo |= (VMX_EXIT_INT_INFO_TYPE_SW_XCPT << VMX_EXIT_INT_INFO_TYPE_SHIFT);
                break;

            case X86_XCPT_PF:
            case X86_XCPT_DF:
            case X86_XCPT_TS:
            case X86_XCPT_NP:
            case X86_XCPT_SS:
            case X86_XCPT_GP:
            case X86_XCPT_AC:
                u32IntInfo |= VMX_EXIT_INT_INFO_ERROR_CODE_VALID;
                RT_FALL_THRU();
            default:
                u32IntInfo |= (VMX_EXIT_INT_INFO_TYPE_HW_XCPT << VMX_EXIT_INT_INFO_TYPE_SHIFT);
                break;
        }
    }
    else if (enmTrpmEvent == TRPM_HARDWARE_INT)
        u32IntInfo |= (VMX_EXIT_INT_INFO_TYPE_EXT_INT << VMX_EXIT_INT_INFO_TYPE_SHIFT);
    else if (enmTrpmEvent == TRPM_SOFTWARE_INT)
    {
        switch (uVector)
        {
            case X86_XCPT_BP:
            case X86_XCPT_OF:
                u32IntInfo |= (VMX_EXIT_INT_INFO_TYPE_SW_XCPT << VMX_EXIT_INT_INFO_TYPE_SHIFT);
                break;

            default:
                Assert(uVector == X86_XCPT_DB);
                u32IntInfo |= (VMX_EXIT_INT_INFO_TYPE_SW_INT << VMX_EXIT_INT_INFO_TYPE_SHIFT);
                break;
        }
    }
    else
        AssertMsgFailed(("Invalid TRPM event type %d\n", enmTrpmEvent));

    rc = TRPMResetTrap(pVCpu);
    AssertRC(rc);
    Log4(("TRPM->HM event: u32IntInfo=%#RX32 enmTrpmEvent=%d cbInstr=%u uErrCode=%#RX32 GCPtrFaultAddress=%#RGv\n",
          u32IntInfo, enmTrpmEvent, cbInstr, uErrCode, GCPtrFaultAddress));

    hmR0VmxSetPendingEvent(pVCpu, u32IntInfo, cbInstr, uErrCode, GCPtrFaultAddress);
}


/**
 * Converts the pending HM event into a TRPM trap.
 *
 * @param   pVCpu   The cross context virtual CPU structure.
 */
static void hmR0VmxPendingEventToTrpmTrap(PVMCPU pVCpu)
{
    Assert(pVCpu->hm.s.Event.fPending);

    uint32_t uVectorType     = VMX_IDT_VECTORING_INFO_TYPE(pVCpu->hm.s.Event.u64IntInfo);
    uint32_t uVector         = VMX_IDT_VECTORING_INFO_VECTOR(pVCpu->hm.s.Event.u64IntInfo);
    bool     fErrorCodeValid = VMX_IDT_VECTORING_INFO_IS_ERROR_CODE_VALID(pVCpu->hm.s.Event.u64IntInfo);
    uint32_t uErrorCode      = pVCpu->hm.s.Event.u32ErrCode;

    /* If a trap was already pending, we did something wrong! */
    Assert(TRPMQueryTrap(pVCpu, NULL /* pu8TrapNo */, NULL /* pEnmType */) == VERR_TRPM_NO_ACTIVE_TRAP);

    /** @todo Use HMVmxEventToTrpmEventType() later. */
    TRPMEVENT enmTrapType;
    switch (uVectorType)
    {
        case VMX_IDT_VECTORING_INFO_TYPE_EXT_INT:
           enmTrapType = TRPM_HARDWARE_INT;
           break;

        case VMX_IDT_VECTORING_INFO_TYPE_NMI:
        case VMX_IDT_VECTORING_INFO_TYPE_HW_XCPT:
            enmTrapType = TRPM_TRAP;
            break;

        case VMX_IDT_VECTORING_INFO_TYPE_PRIV_SW_XCPT:  /* #DB (INT1/ICEBP). */
            Assert(uVector == X86_XCPT_DB);
            enmTrapType = TRPM_SOFTWARE_INT;
            break;

        case VMX_IDT_VECTORING_INFO_TYPE_SW_XCPT:       /* #BP (INT3) and #OF (INTO) */
            Assert(uVector == X86_XCPT_BP || uVector == X86_XCPT_OF);
            enmTrapType = TRPM_SOFTWARE_INT;
            break;

        case VMX_IDT_VECTORING_INFO_TYPE_SW_INT:
            enmTrapType = TRPM_SOFTWARE_INT;
            break;

        default:
            AssertMsgFailed(("Invalid trap type %#x\n", uVectorType));
            enmTrapType = TRPM_32BIT_HACK;
            break;
    }

    Log4(("HM event->TRPM: uVector=%#x enmTrapType=%d\n", uVector, enmTrapType));

    int rc = TRPMAssertTrap(pVCpu, uVector, enmTrapType);
    AssertRC(rc);

    if (fErrorCodeValid)
        TRPMSetErrorCode(pVCpu, uErrorCode);

    if (   uVectorType == VMX_IDT_VECTORING_INFO_TYPE_HW_XCPT
        && uVector == X86_XCPT_PF)
        TRPMSetFaultAddress(pVCpu, pVCpu->hm.s.Event.GCPtrFaultAddress);
    else if (enmTrapType == TRPM_SOFTWARE_INT)
        TRPMSetInstrLength(pVCpu, pVCpu->hm.s.Event.cbInstr);

    /* We're now done converting the pending event. */
    pVCpu->hm.s.Event.fPending = false;
}


/**
 * Sets the interrupt-window exiting control in the VMCS which instructs VT-x to
 * cause a VM-exit as soon as the guest is in a state to receive interrupts.
 *
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pVmcsInfo   The VMCS info. object.
 */
static void hmR0VmxSetIntWindowExitVmcs(PVMCPU pVCpu, PVMXVMCSINFO pVmcsInfo)
{
    if (pVCpu->CTX_SUFF(pVM)->hm.s.vmx.Msrs.ProcCtls.n.allowed1 & VMX_PROC_CTLS_INT_WINDOW_EXIT)
    {
        if (!(pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_INT_WINDOW_EXIT))
        {
            pVmcsInfo->u32ProcCtls |= VMX_PROC_CTLS_INT_WINDOW_EXIT;
            int rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC, pVmcsInfo->u32ProcCtls);
            AssertRC(rc);
        }
    } /* else we will deliver interrupts whenever the guest Vm-exits next and is in a state to receive the interrupt. */
}


/**
 * Clears the interrupt-window exiting control in the VMCS.
 *
 * @param   pVmcsInfo   The VMCS info. object.
 */
DECLINLINE(int) hmR0VmxClearIntWindowExitVmcs(PVMXVMCSINFO pVmcsInfo)
{
    if (pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_INT_WINDOW_EXIT)
    {
        pVmcsInfo->u32ProcCtls &= ~VMX_PROC_CTLS_INT_WINDOW_EXIT;
        return VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC, pVmcsInfo->u32ProcCtls);
    }
    return VINF_SUCCESS;
}


/**
 * Sets the NMI-window exiting control in the VMCS which instructs VT-x to
 * cause a VM-exit as soon as the guest is in a state to receive NMIs.
 *
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pVmcsInfo   The VMCS info. object.
 */
static void hmR0VmxSetNmiWindowExitVmcs(PVMCPU pVCpu, PVMXVMCSINFO pVmcsInfo)
{
    if (pVCpu->CTX_SUFF(pVM)->hm.s.vmx.Msrs.ProcCtls.n.allowed1 & VMX_PROC_CTLS_NMI_WINDOW_EXIT)
    {
        if (!(pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_NMI_WINDOW_EXIT))
        {
            pVmcsInfo->u32ProcCtls |= VMX_PROC_CTLS_NMI_WINDOW_EXIT;
            int rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC, pVmcsInfo->u32ProcCtls);
            AssertRC(rc);
            Log4Func(("Setup NMI-window exiting\n"));
        }
    } /* else we will deliver NMIs whenever we VM-exit next, even possibly nesting NMIs. Can't be helped on ancient CPUs. */
}


/**
 * Clears the NMI-window exiting control in the VMCS.
 *
 * @param   pVmcsInfo   The VMCS info. object.
 */
DECLINLINE(int) hmR0VmxClearNmiWindowExitVmcs(PVMXVMCSINFO pVmcsInfo)
{
    if (pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_NMI_WINDOW_EXIT)
    {
        pVmcsInfo->u32ProcCtls &= ~VMX_PROC_CTLS_NMI_WINDOW_EXIT;
        return VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC, pVmcsInfo->u32ProcCtls);
    }
    return VINF_SUCCESS;
}


/**
 * Does the necessary state syncing before returning to ring-3 for any reason
 * (longjmp, preemption, voluntary exits to ring-3) from VT-x.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   fImportState    Whether to import the guest state from the VMCS back
 *                          to the guest-CPU context.
 *
 * @remarks No-long-jmp zone!!!
 */
static int hmR0VmxLeave(PVMCPU pVCpu, bool fImportState)
{
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));
    Assert(!VMMRZCallRing3IsEnabled(pVCpu));

    RTCPUID idCpu = RTMpCpuId();
    Log4Func(("HostCpuId=%u\n", idCpu));

    /*
     * !!! IMPORTANT !!!
     * If you modify code here, check whether hmR0VmxCallRing3Callback() needs to be updated too.
     */

    /* Save the guest state if necessary. */
    PVMXVMCSINFO pVmcsInfo = hmGetVmxActiveVmcsInfo(pVCpu);
    if (fImportState)
    {
        int rc = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, HMVMX_CPUMCTX_EXTRN_ALL);
        AssertRCReturn(rc, rc);
    }

    /* Restore host FPU state if necessary. We will resync on next R0 reentry. */
    CPUMR0FpuStateMaybeSaveGuestAndRestoreHost(pVCpu);
    Assert(!CPUMIsGuestFPUStateActive(pVCpu));

    /* Restore host debug registers if necessary. We will resync on next R0 reentry. */
#ifdef VBOX_STRICT
    if (CPUMIsHyperDebugStateActive(pVCpu))
        Assert(pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_MOV_DR_EXIT);
#endif
    CPUMR0DebugStateMaybeSaveGuestAndRestoreHost(pVCpu, true /* save DR6 */);
    Assert(!CPUMIsGuestDebugStateActive(pVCpu) && !CPUMIsGuestDebugStateActivePending(pVCpu));
    Assert(!CPUMIsHyperDebugStateActive(pVCpu) && !CPUMIsHyperDebugStateActivePending(pVCpu));

#if HC_ARCH_BITS == 64
    /* Restore host-state bits that VT-x only restores partially. */
    if (   (pVCpu->hm.s.vmx.fRestoreHostFlags & VMX_RESTORE_HOST_REQUIRED)
        && (pVCpu->hm.s.vmx.fRestoreHostFlags & ~VMX_RESTORE_HOST_REQUIRED))
    {
        Log4Func(("Restoring Host State: fRestoreHostFlags=%#RX32 HostCpuId=%u\n", pVCpu->hm.s.vmx.fRestoreHostFlags, idCpu));
        VMXRestoreHostState(pVCpu->hm.s.vmx.fRestoreHostFlags, &pVCpu->hm.s.vmx.RestoreHost);
    }
    pVCpu->hm.s.vmx.fRestoreHostFlags = 0;
#endif

    /* Restore the lazy host MSRs as we're leaving VT-x context. */
    if (pVCpu->hm.s.vmx.fLazyMsrs & VMX_LAZY_MSRS_LOADED_GUEST)
    {
        /* We shouldn't restore the host MSRs without saving the guest MSRs first. */
        if (!fImportState)
        {
            int rc = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, CPUMCTX_EXTRN_KERNEL_GS_BASE | CPUMCTX_EXTRN_SYSCALL_MSRS);
            AssertRCReturn(rc, rc);
        }
        hmR0VmxLazyRestoreHostMsrs(pVCpu);
        Assert(!pVCpu->hm.s.vmx.fLazyMsrs);
    }
    else
        pVCpu->hm.s.vmx.fLazyMsrs = 0;

    /* Update auto-load/store host MSRs values when we re-enter VT-x (as we could be on a different CPU). */
    pVCpu->hm.s.vmx.fUpdatedHostAutoMsrs = false;

    STAM_PROFILE_ADV_SET_STOPPED(&pVCpu->hm.s.StatEntry);
    STAM_PROFILE_ADV_SET_STOPPED(&pVCpu->hm.s.StatImportGuestState);
    STAM_PROFILE_ADV_SET_STOPPED(&pVCpu->hm.s.StatExportGuestState);
    STAM_PROFILE_ADV_SET_STOPPED(&pVCpu->hm.s.StatPreExit);
    STAM_PROFILE_ADV_SET_STOPPED(&pVCpu->hm.s.StatExitHandling);
    STAM_PROFILE_ADV_SET_STOPPED(&pVCpu->hm.s.StatExitIO);
    STAM_PROFILE_ADV_SET_STOPPED(&pVCpu->hm.s.StatExitMovCRx);
    STAM_PROFILE_ADV_SET_STOPPED(&pVCpu->hm.s.StatExitXcptNmi);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatSwitchLongJmpToR3);

    VMCPU_CMPXCHG_STATE(pVCpu, VMCPUSTATE_STARTED_HM, VMCPUSTATE_STARTED_EXEC);

    /** @todo This partially defeats the purpose of having preemption hooks.
     *  The problem is, deregistering the hooks should be moved to a place that
     *  lasts until the EMT is about to be destroyed not everytime while leaving HM
     *  context.
     */
    int rc = hmR0VmxClearVmcs(pVmcsInfo);
    AssertRCReturn(rc, rc);

    Log4Func(("Cleared Vmcs. HostCpuId=%u\n", idCpu));
    NOREF(idCpu);
    return VINF_SUCCESS;
}


/**
 * Leaves the VT-x session.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 *
 * @remarks No-long-jmp zone!!!
 */
static int hmR0VmxLeaveSession(PVMCPU pVCpu)
{
    HM_DISABLE_PREEMPT(pVCpu);
    HMVMX_ASSERT_CPU_SAFE(pVCpu);
    Assert(!VMMRZCallRing3IsEnabled(pVCpu));
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    /* When thread-context hooks are used, we can avoid doing the leave again if we had been preempted before
       and done this from the VMXR0ThreadCtxCallback(). */
    if (!pVCpu->hm.s.fLeaveDone)
    {
        int rc2 = hmR0VmxLeave(pVCpu, true /* fImportState */);
        AssertRCReturnStmt(rc2, HM_RESTORE_PREEMPT(), rc2);
        pVCpu->hm.s.fLeaveDone = true;
    }
    Assert(!pVCpu->cpum.GstCtx.fExtrn);

    /*
     * !!! IMPORTANT !!!
     * If you modify code here, make sure to check whether hmR0VmxCallRing3Callback() needs to be updated too.
     */

    /* Deregister hook now that we've left HM context before re-enabling preemption. */
    /** @todo Deregistering here means we need to VMCLEAR always
     *        (longjmp/exit-to-r3) in VT-x which is not efficient, eliminate need
     *        for calling VMMR0ThreadCtxHookDisable here! */
    VMMR0ThreadCtxHookDisable(pVCpu);

    /* Leave HM context. This takes care of local init (term). */
    int rc = HMR0LeaveCpu(pVCpu);

    HM_RESTORE_PREEMPT();
    return rc;
}


/**
 * Does the necessary state syncing before doing a longjmp to ring-3.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 *
 * @remarks No-long-jmp zone!!!
 */
DECLINLINE(int) hmR0VmxLongJmpToRing3(PVMCPU pVCpu)
{
    return hmR0VmxLeaveSession(pVCpu);
}


/**
 * Take necessary actions before going back to ring-3.
 *
 * An action requires us to go back to ring-3. This function does the necessary
 * steps before we can safely return to ring-3. This is not the same as longjmps
 * to ring-3, this is voluntary and prepares the guest so it may continue
 * executing outside HM (recompiler/IEM).
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   rcExit      The reason for exiting to ring-3. Can be
 *                      VINF_VMM_UNKNOWN_RING3_CALL.
 */
static int hmR0VmxExitToRing3(PVMCPU pVCpu, VBOXSTRICTRC rcExit)
{
    HMVMX_ASSERT_PREEMPT_SAFE(pVCpu);

    PVMXVMCSINFO pVmcsInfo = hmGetVmxActiveVmcsInfo(pVCpu);
    if (RT_UNLIKELY(rcExit == VERR_VMX_INVALID_VMCS_PTR))
    {
        VMXGetCurrentVmcs(&pVCpu->hm.s.vmx.LastError.HCPhysCurrentVmcs);
        pVCpu->hm.s.vmx.LastError.u32VmcsRev   = *(uint32_t *)pVmcsInfo->pvVmcs;
        pVCpu->hm.s.vmx.LastError.idEnteredCpu = pVCpu->hm.s.idEnteredCpu;
        /* LastError.idCurrentCpu was updated in hmR0VmxPreRunGuestCommitted(). */
    }

    /* Please, no longjumps here (any logging shouldn't flush jump back to ring-3). NO LOGGING BEFORE THIS POINT! */
    VMMRZCallRing3Disable(pVCpu);
    Log4Func(("rcExit=%d\n", VBOXSTRICTRC_VAL(rcExit)));

    /*
     * Convert any pending HM events back to TRPM due to premature exits to ring-3.
     * We need to do this only on returns to ring-3 and not for longjmps to ring3.
     *
     * This is because execution may continue from ring-3 and we would need to inject
     * the event from there (hence place it back in TRPM).
     */
    if (pVCpu->hm.s.Event.fPending)
    {
        hmR0VmxPendingEventToTrpmTrap(pVCpu);
        Assert(!pVCpu->hm.s.Event.fPending);

        /* Clear the events from the VMCS. */
        int rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_ENTRY_INTERRUPTION_INFO, 0);
        AssertRCReturn(rc, rc);
    }
#ifdef VBOX_STRICT
    else
    {
        /*
         * Ensure we don't accidentally clear a pending HM event without clearing the VMCS.
         * This can be pretty hard to debug otherwise, interrupts might get injected twice
         * occasionally, see @bugref{9180#c42}.
         */
        uint32_t uEntryIntInfo;
        int rc = VMXReadVmcs32(VMX_VMCS32_CTRL_ENTRY_INTERRUPTION_INFO, &uEntryIntInfo);
        AssertRC(rc);
        Assert(!VMX_ENTRY_INT_INFO_IS_VALID(uEntryIntInfo));
    }
#endif

    /*
     * Clear the interrupt-window and NMI-window VMCS controls as we could have got
     * a VM-exit with higher priority than interrupt-window or NMI-window VM-exits
     * (e.g. TPR below threshold).
     */
    int rc = hmR0VmxClearIntWindowExitVmcs(pVmcsInfo);
    rc    |= hmR0VmxClearNmiWindowExitVmcs(pVmcsInfo);
    AssertRCReturn(rc, rc);

    /* If we're emulating an instruction, we shouldn't have any TRPM traps pending
       and if we're injecting an event we should have a TRPM trap pending. */
    AssertMsg(rcExit != VINF_EM_RAW_INJECT_TRPM_EVENT || TRPMHasTrap(pVCpu), ("%Rrc\n", VBOXSTRICTRC_VAL(rcExit)));
#ifndef DEBUG_bird /* Triggered after firing an NMI against NT4SP1, possibly a triple fault in progress. */
    AssertMsg(rcExit != VINF_EM_RAW_EMULATE_INSTR || !TRPMHasTrap(pVCpu), ("%Rrc\n", VBOXSTRICTRC_VAL(rcExit)));
#endif

    /* Save guest state and restore host state bits. */
    rc = hmR0VmxLeaveSession(pVCpu);
    AssertRCReturn(rc, rc);
    STAM_COUNTER_DEC(&pVCpu->hm.s.StatSwitchLongJmpToR3);

    /* Thread-context hooks are unregistered at this point!!! */

    /* Sync recompiler state. */
    VMCPU_FF_CLEAR(pVCpu, VMCPU_FF_TO_R3);
    CPUMSetChangedFlags(pVCpu, CPUM_CHANGED_SYSENTER_MSR
                             | CPUM_CHANGED_LDTR
                             | CPUM_CHANGED_GDTR
                             | CPUM_CHANGED_IDTR
                             | CPUM_CHANGED_TR
                             | CPUM_CHANGED_HIDDEN_SEL_REGS);
    if (   pVCpu->CTX_SUFF(pVM)->hm.s.fNestedPaging
        && CPUMIsGuestPagingEnabledEx(&pVCpu->cpum.GstCtx))
        CPUMSetChangedFlags(pVCpu, CPUM_CHANGED_GLOBAL_TLB_FLUSH);

    Assert(!pVCpu->hm.s.fClearTrapFlag);

    /* Update the exit-to-ring 3 reason. */
    pVCpu->hm.s.rcLastExitToR3 = VBOXSTRICTRC_VAL(rcExit);

    /* On our way back from ring-3 reload the guest state if there is a possibility of it being changed. */
    if (   rcExit != VINF_EM_RAW_INTERRUPT
        || CPUMIsGuestInVmxNonRootMode(&pVCpu->cpum.GstCtx))
    {
        Assert(!(pVCpu->cpum.GstCtx.fExtrn & HMVMX_CPUMCTX_EXTRN_ALL));
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_ALL_GUEST);
    }

    STAM_COUNTER_INC(&pVCpu->hm.s.StatSwitchExitToR3);

    /* We do -not- want any longjmp notifications after this! We must return to ring-3 ASAP. */
    VMMRZCallRing3RemoveNotification(pVCpu);
    VMMRZCallRing3Enable(pVCpu);

    return rc;
}


/**
 * VMMRZCallRing3() callback wrapper which saves the guest state before we
 * longjump to ring-3 and possibly get preempted.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   enmOperation    The operation causing the ring-3 longjump.
 * @param   pvUser          User argument, currently unused, NULL.
 */
static DECLCALLBACK(int) hmR0VmxCallRing3Callback(PVMCPU pVCpu, VMMCALLRING3 enmOperation, void *pvUser)
{
    RT_NOREF(pvUser);
    if (enmOperation == VMMCALLRING3_VM_R0_ASSERTION)
    {
        /*
         * !!! IMPORTANT !!!
         * If you modify code here, check whether hmR0VmxLeave() and hmR0VmxLeaveSession() needs to be updated too.
         * This is a stripped down version which gets out ASAP, trying to not trigger any further assertions.
         */
        VMMRZCallRing3RemoveNotification(pVCpu);
        VMMRZCallRing3Disable(pVCpu);
        RTTHREADPREEMPTSTATE PreemptState = RTTHREADPREEMPTSTATE_INITIALIZER;
        RTThreadPreemptDisable(&PreemptState);

        PVMXVMCSINFO pVmcsInfo = hmGetVmxActiveVmcsInfo(pVCpu);
        hmR0VmxImportGuestState(pVCpu, pVmcsInfo, HMVMX_CPUMCTX_EXTRN_ALL);
        CPUMR0FpuStateMaybeSaveGuestAndRestoreHost(pVCpu);
        CPUMR0DebugStateMaybeSaveGuestAndRestoreHost(pVCpu, true /* save DR6 */);

#if HC_ARCH_BITS == 64
        /* Restore host-state bits that VT-x only restores partially. */
        if (   (pVCpu->hm.s.vmx.fRestoreHostFlags &  VMX_RESTORE_HOST_REQUIRED)
            && (pVCpu->hm.s.vmx.fRestoreHostFlags & ~VMX_RESTORE_HOST_REQUIRED))
            VMXRestoreHostState(pVCpu->hm.s.vmx.fRestoreHostFlags, &pVCpu->hm.s.vmx.RestoreHost);
        pVCpu->hm.s.vmx.fRestoreHostFlags = 0;
#endif

        /* Restore the lazy host MSRs as we're leaving VT-x context. */
        if (pVCpu->hm.s.vmx.fLazyMsrs & VMX_LAZY_MSRS_LOADED_GUEST)
            hmR0VmxLazyRestoreHostMsrs(pVCpu);

        /* Update auto-load/store host MSRs values when we re-enter VT-x (as we could be on a different CPU). */
        pVCpu->hm.s.vmx.fUpdatedHostAutoMsrs = false;
        VMCPU_CMPXCHG_STATE(pVCpu, VMCPUSTATE_STARTED_HM, VMCPUSTATE_STARTED_EXEC);

        /* Clear the current VMCS data back to memory. */
        hmR0VmxClearVmcs(pVmcsInfo);

        /** @todo eliminate the need for calling VMMR0ThreadCtxHookDisable here!  */
        VMMR0ThreadCtxHookDisable(pVCpu);
        HMR0LeaveCpu(pVCpu);
        RTThreadPreemptRestore(&PreemptState);
        return VINF_SUCCESS;
    }

    Assert(pVCpu);
    Assert(pvUser);
    Assert(VMMRZCallRing3IsEnabled(pVCpu));
    HMVMX_ASSERT_PREEMPT_SAFE(pVCpu);

    VMMRZCallRing3Disable(pVCpu);
    Assert(VMMR0IsLogFlushDisabled(pVCpu));

    Log4Func((" -> hmR0VmxLongJmpToRing3 enmOperation=%d\n", enmOperation));

    int rc = hmR0VmxLongJmpToRing3(pVCpu);
    AssertRCReturn(rc, rc);

    VMMRZCallRing3Enable(pVCpu);
    return VINF_SUCCESS;
}


/**
 * Pushes a 2-byte value onto the real-mode (in virtual-8086 mode) guest's
 * stack.
 *
 * @returns Strict VBox status code (i.e. informational status codes too).
 * @retval  VINF_EM_RESET if pushing a value to the stack caused a triple-fault.
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   uValue      The value to push to the guest stack.
 */
static VBOXSTRICTRC hmR0VmxRealModeGuestStackPush(PVMCPU pVCpu, uint16_t uValue)
{
    /*
     * The stack limit is 0xffff in real-on-virtual 8086 mode. Real-mode with weird stack limits cannot be run in
     * virtual 8086 mode in VT-x. See Intel spec. 26.3.1.2 "Checks on Guest Segment Registers".
     * See Intel Instruction reference for PUSH and Intel spec. 22.33.1 "Segment Wraparound".
     */
    PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
    if (pCtx->sp == 1)
        return VINF_EM_RESET;
    pCtx->sp -= sizeof(uint16_t);       /* May wrap around which is expected behaviour. */
    int rc = PGMPhysSimpleWriteGCPhys(pVCpu->CTX_SUFF(pVM), pCtx->ss.u64Base + pCtx->sp, &uValue, sizeof(uint16_t));
    AssertRC(rc);
    return rc;
}


/**
 * Injects an event into the guest upon VM-entry by updating the relevant fields
 * in the VM-entry area in the VMCS.
 *
 * @returns Strict VBox status code (i.e. informational status codes too).
 * @retval  VINF_SUCCESS if the event is successfully injected into the VMCS.
 * @retval  VINF_EM_RESET if event injection resulted in a triple-fault.
 *
 * @param   pVCpu               The cross context virtual CPU structure.
 * @param   pVmxTransient       The VMX-transient structure.
 * @param   pEvent              The event being injected.
 * @param   pfIntrState         Pointer to the VT-x guest-interruptibility-state.
 *                              This will be updated if necessary. This cannot not
 *                              be NULL.
 * @param   fStepping           Whether we're single-stepping guest execution and
 *                              should return VINF_EM_DBG_STEPPED if the event is
 *                              injected directly (registers modified by us, not by
 *                              hardware on VM-entry).
 */
static VBOXSTRICTRC hmR0VmxInjectEventVmcs(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient, PCHMEVENT pEvent, bool fStepping,
                                           uint32_t *pfIntrState)
{
    /* Intel spec. 24.8.3 "VM-Entry Controls for Event Injection" specifies the interruption-information field to be 32-bits. */
    AssertMsg(!RT_HI_U32(pEvent->u64IntInfo), ("%#RX64\n", pEvent->u64IntInfo));
    Assert(pfIntrState);

    PCPUMCTX          pCtx       = &pVCpu->cpum.GstCtx;
    uint32_t          u32IntInfo = pEvent->u64IntInfo;
    uint32_t const    u32ErrCode = pEvent->u32ErrCode;
    uint32_t const    cbInstr    = pEvent->cbInstr;
    RTGCUINTPTR const GCPtrFault = pEvent->GCPtrFaultAddress;
    uint32_t const    uVector    = VMX_ENTRY_INT_INFO_VECTOR(u32IntInfo);
    uint32_t const    uIntType   = VMX_ENTRY_INT_INFO_TYPE(u32IntInfo);

#ifdef VBOX_STRICT
    /*
     * Validate the error-code-valid bit for hardware exceptions.
     * No error codes for exceptions in real-mode.
     *
     * See Intel spec. 20.1.4 "Interrupt and Exception Handling"
     */
    if (   uIntType == VMX_EXIT_INT_INFO_TYPE_HW_XCPT
        && !CPUMIsGuestInRealModeEx(pCtx))
    {
        switch (uVector)
        {
            case X86_XCPT_PF:
            case X86_XCPT_DF:
            case X86_XCPT_TS:
            case X86_XCPT_NP:
            case X86_XCPT_SS:
            case X86_XCPT_GP:
            case X86_XCPT_AC:
                AssertMsg(VMX_ENTRY_INT_INFO_IS_ERROR_CODE_VALID(u32IntInfo),
                          ("Error-code-valid bit not set for exception that has an error code uVector=%#x\n", uVector));
                RT_FALL_THRU();
            default:
                break;
        }
    }

    /* Cannot inject an NMI when block-by-MOV SS is in effect. */
    Assert(   uIntType != VMX_EXIT_INT_INFO_TYPE_NMI
           || !(*pfIntrState & VMX_VMCS_GUEST_INT_STATE_BLOCK_MOVSS));
#endif

    STAM_COUNTER_INC(&pVCpu->hm.s.paStatInjectedIrqsR0[uVector & MASK_INJECT_IRQ_STAT]);

    /*
     * Hardware interrupts & exceptions cannot be delivered through the software interrupt
     * redirection bitmap to the real mode task in virtual-8086 mode. We must jump to the
     * interrupt handler in the (real-mode) guest.
     *
     * See Intel spec. 20.3 "Interrupt and Exception handling in Virtual-8086 Mode".
     * See Intel spec. 20.1.4 "Interrupt and Exception Handling" for real-mode interrupt handling.
     */
    if (CPUMIsGuestInRealModeEx(pCtx))     /* CR0.PE bit changes are always intercepted, so it's up to date. */
    {
        if (pVCpu->CTX_SUFF(pVM)->hm.s.vmx.fUnrestrictedGuest)
        {
            /*
             * For CPUs with unrestricted guest execution enabled and with the guest
             * in real-mode, we must not set the deliver-error-code bit.
             *
             * See Intel spec. 26.2.1.3 "VM-Entry Control Fields".
             */
            u32IntInfo &= ~VMX_ENTRY_INT_INFO_ERROR_CODE_VALID;
        }
        else
        {
            PVM pVM = pVCpu->CTX_SUFF(pVM);
            Assert(PDMVmmDevHeapIsEnabled(pVM));
            Assert(pVM->hm.s.vmx.pRealModeTSS);
            Assert(!CPUMIsGuestInVmxNonRootMode(&pVCpu->cpum.GstCtx));

            /* We require RIP, RSP, RFLAGS, CS, IDTR, import them. */
            PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
            int rc2 = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, CPUMCTX_EXTRN_SREG_MASK | CPUMCTX_EXTRN_TABLE_MASK
                                                              | CPUMCTX_EXTRN_RIP | CPUMCTX_EXTRN_RSP | CPUMCTX_EXTRN_RFLAGS);
            AssertRCReturn(rc2, rc2);

            /* Check if the interrupt handler is present in the IVT (real-mode IDT). IDT limit is (4N - 1). */
            size_t const cbIdtEntry = sizeof(X86IDTR16);
            if (uVector * cbIdtEntry + (cbIdtEntry - 1) > pCtx->idtr.cbIdt)
            {
                /* If we are trying to inject a #DF with no valid IDT entry, return a triple-fault. */
                if (uVector == X86_XCPT_DF)
                    return VINF_EM_RESET;

                /* If we're injecting a #GP with no valid IDT entry, inject a double-fault.
                   No error codes for exceptions in real-mode. */
                if (uVector == X86_XCPT_GP)
                {
                    uint32_t const uXcptDfInfo = RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_VECTOR,         X86_XCPT_DF)
                                               | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_TYPE,           VMX_ENTRY_INT_INFO_TYPE_HW_XCPT)
                                               | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_ERR_CODE_VALID, 0)
                                               | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_VALID,          1);
                    HMEVENT EventXcptDf;
                    RT_ZERO(EventXcptDf);
                    EventXcptDf.u64IntInfo = uXcptDfInfo;
                    return hmR0VmxInjectEventVmcs(pVCpu, pVmxTransient, &EventXcptDf, fStepping, pfIntrState);
                }

                /*
                 * If we're injecting an event with no valid IDT entry, inject a #GP.
                 * No error codes for exceptions in real-mode.
                 *
                 * See Intel spec. 20.1.4 "Interrupt and Exception Handling"
                 */
                uint32_t const uXcptGpInfo = RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_VECTOR,         X86_XCPT_GP)
                                           | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_TYPE,           VMX_ENTRY_INT_INFO_TYPE_HW_XCPT)
                                           | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_ERR_CODE_VALID, 0)
                                           | RT_BF_MAKE(VMX_BF_ENTRY_INT_INFO_VALID,          1);
                HMEVENT EventXcptGp;
                RT_ZERO(EventXcptGp);
                EventXcptGp.u64IntInfo = uXcptGpInfo;
                return hmR0VmxInjectEventVmcs(pVCpu, pVmxTransient, &EventXcptGp, fStepping, pfIntrState);
            }

            /* Software exceptions (#BP and #OF exceptions thrown as a result of INT3 or INTO) */
            uint16_t uGuestIp = pCtx->ip;
            if (uIntType == VMX_ENTRY_INT_INFO_TYPE_SW_XCPT)
            {
                Assert(uVector == X86_XCPT_BP || uVector == X86_XCPT_OF);
                /* #BP and #OF are both benign traps, we need to resume the next instruction. */
                uGuestIp = pCtx->ip + (uint16_t)cbInstr;
            }
            else if (uIntType == VMX_ENTRY_INT_INFO_TYPE_SW_INT)
                uGuestIp = pCtx->ip + (uint16_t)cbInstr;

            /* Get the code segment selector and offset from the IDT entry for the interrupt handler. */
            X86IDTR16 IdtEntry;
            RTGCPHYS const GCPhysIdtEntry = (RTGCPHYS)pCtx->idtr.pIdt + uVector * cbIdtEntry;
            rc2 = PGMPhysSimpleReadGCPhys(pVM, &IdtEntry, GCPhysIdtEntry, cbIdtEntry);
            AssertRCReturn(rc2, rc2);

            /* Construct the stack frame for the interrupt/exception handler. */
            VBOXSTRICTRC rcStrict;
            rcStrict = hmR0VmxRealModeGuestStackPush(pVCpu, pCtx->eflags.u32);
            if (rcStrict == VINF_SUCCESS)
            {
                rcStrict = hmR0VmxRealModeGuestStackPush(pVCpu, pCtx->cs.Sel);
                if (rcStrict == VINF_SUCCESS)
                    rcStrict = hmR0VmxRealModeGuestStackPush(pVCpu, uGuestIp);
            }

            /* Clear the required eflag bits and jump to the interrupt/exception handler. */
            if (rcStrict == VINF_SUCCESS)
            {
                pCtx->eflags.u32 &= ~(X86_EFL_IF | X86_EFL_TF | X86_EFL_RF | X86_EFL_AC);
                pCtx->rip         = IdtEntry.offSel;
                pCtx->cs.Sel      = IdtEntry.uSel;
                pCtx->cs.ValidSel = IdtEntry.uSel;
                pCtx->cs.u64Base  = IdtEntry.uSel << cbIdtEntry;
                if (   uIntType == VMX_ENTRY_INT_INFO_TYPE_HW_XCPT
                    && uVector  == X86_XCPT_PF)
                    pCtx->cr2 = GCPtrFault;

                ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_CS  | HM_CHANGED_GUEST_CR2
                                                         | HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS
                                                         | HM_CHANGED_GUEST_RSP);

                /*
                 * If we delivered a hardware exception (other than an NMI) and if there was
                 * block-by-STI in effect, we should clear it.
                 */
                if (*pfIntrState & VMX_VMCS_GUEST_INT_STATE_BLOCK_STI)
                {
                    Assert(   uIntType != VMX_ENTRY_INT_INFO_TYPE_NMI
                           && uIntType != VMX_ENTRY_INT_INFO_TYPE_EXT_INT);
                    Log4Func(("Clearing inhibition due to STI\n"));
                    *pfIntrState &= ~VMX_VMCS_GUEST_INT_STATE_BLOCK_STI;
                }

                Log4(("Injected real-mode: u32IntInfo=%#x u32ErrCode=%#x cbInstr=%#x Eflags=%#x CS:EIP=%04x:%04x\n",
                      u32IntInfo, u32ErrCode, cbInstr, pCtx->eflags.u, pCtx->cs.Sel, pCtx->eip));

                /*
                 * The event has been truly dispatched to the guest. Mark it as no longer pending so
                 * we don't attempt to undo it if we are returning to ring-3 before executing guest code.
                 */
                pVCpu->hm.s.Event.fPending = false;

                /* If we're stepping and we've changed cs:rip above, bail out of the VMX R0 execution loop. */
                if (fStepping)
                    rcStrict = VINF_EM_DBG_STEPPED;
            }
            AssertMsg(rcStrict == VINF_SUCCESS || rcStrict == VINF_EM_RESET || (rcStrict == VINF_EM_DBG_STEPPED && fStepping),
                      ("%Rrc\n", VBOXSTRICTRC_VAL(rcStrict)));
            return rcStrict;
        }
    }

    /*
     * Validate.
     */
    Assert(VMX_ENTRY_INT_INFO_IS_VALID(u32IntInfo));                     /* Bit 31 (Valid bit) must be set by caller. */
    Assert(!(u32IntInfo & VMX_BF_ENTRY_INT_INFO_RSVD_12_30_MASK));       /* Bits 30:12 MBZ. */

    /*
     * Inject the event into the VMCS.
     */
    int rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_ENTRY_INTERRUPTION_INFO, u32IntInfo);
    if (VMX_ENTRY_INT_INFO_IS_ERROR_CODE_VALID(u32IntInfo))
        rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_ENTRY_EXCEPTION_ERRCODE, u32ErrCode);
    rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_ENTRY_INSTR_LENGTH, cbInstr);
    AssertRCReturn(rc, rc);

    /*
     * Update guest CR2 if this is a page-fault.
     */
    if (   VMX_ENTRY_INT_INFO_TYPE(u32IntInfo) == VMX_EXIT_INT_INFO_TYPE_HW_XCPT
        && uVector == X86_XCPT_PF)
        pCtx->cr2 = GCPtrFault;

    Log4(("Injecting u32IntInfo=%#x u32ErrCode=%#x cbInstr=%#x CR2=%#RX64\n", u32IntInfo, u32ErrCode, cbInstr, pCtx->cr2));
    return VINF_SUCCESS;
}


/**
 * Evaluates the event to be delivered to the guest and sets it as the pending
 * event.
 *
 * @returns Strict VBox status code (i.e. informational status codes too).
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 * @param   pfIntrState     Where to store the VT-x guest-interruptibility state.
 */
static VBOXSTRICTRC hmR0VmxEvaluatePendingEvent(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient, uint32_t *pfIntrState)
{
    PCPUMCTX     pCtx = &pVCpu->cpum.GstCtx;
    PVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;

    /* Get the current interruptibility-state of the guest and then figure out what can be injected. */
    uint32_t const fIntrState = hmR0VmxGetGuestIntrState(pVCpu, pVmxTransient);
    bool const fBlockMovSS    = RT_BOOL(fIntrState & VMX_VMCS_GUEST_INT_STATE_BLOCK_MOVSS);
    bool const fBlockSti      = RT_BOOL(fIntrState & VMX_VMCS_GUEST_INT_STATE_BLOCK_STI);
    bool const fBlockNmi      = RT_BOOL(fIntrState & VMX_VMCS_GUEST_INT_STATE_BLOCK_NMI);

    Assert(!fBlockSti || !(ASMAtomicUoReadU64(&pCtx->fExtrn) & CPUMCTX_EXTRN_RFLAGS));
    Assert(!(fIntrState & VMX_VMCS_GUEST_INT_STATE_BLOCK_SMI));    /* We don't support block-by-SMI yet.*/
    Assert(!fBlockSti || pCtx->eflags.Bits.u1IF);                  /* Cannot set block-by-STI when interrupts are disabled. */
    Assert(!TRPMHasTrap(pVCpu));
    Assert(pfIntrState);

    *pfIntrState = fIntrState;

    /*
     * Toggling of interrupt force-flags here is safe since we update TRPM on premature exits
     * to ring-3 before executing guest code, see hmR0VmxExitToRing3(). We must NOT restore these force-flags.
     */
                                                               /** @todo SMI. SMIs take priority over NMIs. */
    if (VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_INTERRUPT_NMI))        /* NMI. NMIs take priority over regular interrupts. */
    {
        /* On some CPUs block-by-STI also blocks NMIs. See Intel spec. 26.3.1.5 "Checks On Guest Non-Register State". */
        if (   !pVCpu->hm.s.Event.fPending
            && !fBlockNmi
            && !fBlockSti
            && !fBlockMovSS)
        {
#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
            if (   pVmxTransient->fIsNestedGuest
                && CPUMIsGuestVmxPinCtlsSet(pVCpu, pCtx, VMX_PIN_CTLS_NMI_EXIT))
                return IEMExecVmxVmexitXcptNmi(pVCpu);
#endif
            hmR0VmxSetPendingXcptNmi(pVCpu);
            VMCPU_FF_CLEAR(pVCpu, VMCPU_FF_INTERRUPT_NMI);
            Log4Func(("Pending NMI\n"));
        }
        else
            hmR0VmxSetNmiWindowExitVmcs(pVCpu, pVmcsInfo);
    }
    /*
     * Check if the guest can receive external interrupts (PIC/APIC). Once PDMGetInterrupt() returns
     * a valid interrupt we -must- deliver the interrupt. We can no longer re-request it from the APIC.
     */
    else if (    VMCPU_FF_IS_ANY_SET(pVCpu, VMCPU_FF_INTERRUPT_APIC | VMCPU_FF_INTERRUPT_PIC)
             && !pVCpu->hm.s.fSingleInstruction)
    {
        Assert(!DBGFIsStepping(pVCpu));
        int rc = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, CPUMCTX_EXTRN_RFLAGS);
        AssertRCReturn(rc, rc);
        bool const fBlockInt = !(pCtx->eflags.u32 & X86_EFL_IF);
        if (   !pVCpu->hm.s.Event.fPending
            && !fBlockInt
            && !fBlockSti
            && !fBlockMovSS)
        {
#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
            if (   pVmxTransient->fIsNestedGuest
                && CPUMIsGuestVmxPinCtlsSet(pVCpu, pCtx, VMX_PIN_CTLS_EXT_INT_EXIT))
            {
                VBOXSTRICTRC rcStrict = IEMExecVmxVmexitExtInt(pVCpu, 0/* uVector */, true /* fIntPending */);
                if (rcStrict != VINF_VMX_INTERCEPT_NOT_ACTIVE)
                    return rcStrict;
            }
#endif
            uint8_t u8Interrupt;
            rc = PDMGetInterrupt(pVCpu, &u8Interrupt);
            if (RT_SUCCESS(rc))
            {
#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
                if (   pVmxTransient->fIsNestedGuest
                    && CPUMIsGuestVmxPinCtlsSet(pVCpu, pCtx, VMX_PIN_CTLS_EXT_INT_EXIT)
                    && CPUMIsGuestVmxExitCtlsSet(pVCpu, pCtx, VMX_EXIT_CTLS_ACK_EXT_INT))
                {
                    VBOXSTRICTRC rcStrict = IEMExecVmxVmexitExtInt(pVCpu, u8Interrupt, false /* fIntPending */);
                    if (rcStrict != VINF_VMX_INTERCEPT_NOT_ACTIVE)
                        return rcStrict;
                }
#endif
                hmR0VmxSetPendingExtInt(pVCpu, u8Interrupt);
                Log4Func(("Pending external interrupt vector %#x\n", u8Interrupt));
            }
            else if (rc == VERR_APIC_INTR_MASKED_BY_TPR)
            {
                if (   !pVmxTransient->fIsNestedGuest
                    && (pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_USE_TPR_SHADOW))
                    hmR0VmxApicSetTprThreshold(pVCpu, pVmcsInfo, u8Interrupt >> 4);
                STAM_COUNTER_INC(&pVCpu->hm.s.StatSwitchTprMaskedIrq);

                /*
                 * If the CPU doesn't have TPR shadowing, we will always get a VM-exit on TPR changes and
                 * APICSetTpr() will end up setting the VMCPU_FF_INTERRUPT_APIC if required, so there is no
                 * need to re-set this force-flag here.
                 */
            }
            else
                STAM_COUNTER_INC(&pVCpu->hm.s.StatSwitchGuestIrq);
        }
        else
            hmR0VmxSetIntWindowExitVmcs(pVCpu, pVmcsInfo);
    }

    return VINF_SUCCESS;
}


/**
 * Injects any pending events into the guest if the guest is in a state to
 * receive them.
 *
 * @returns Strict VBox status code (i.e. informational status codes too).
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 * @param   fIntrState      The VT-x guest-interruptibility state.
 * @param   fStepping       Whether we are single-stepping the guest using the
 *                          hypervisor debugger and should return
 *                          VINF_EM_DBG_STEPPED if the event was dispatched
 *                          directly.
 */
static VBOXSTRICTRC hmR0VmxInjectPendingEvent(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient, uint32_t fIntrState, bool fStepping)
{
    HMVMX_ASSERT_PREEMPT_SAFE(pVCpu);
    Assert(VMMRZCallRing3IsEnabled(pVCpu));

    bool const fBlockMovSS = RT_BOOL(fIntrState & VMX_VMCS_GUEST_INT_STATE_BLOCK_MOVSS);
    bool const fBlockSti   = RT_BOOL(fIntrState & VMX_VMCS_GUEST_INT_STATE_BLOCK_STI);

    Assert(!fBlockSti || !(ASMAtomicUoReadU64(&pVCpu->cpum.GstCtx.fExtrn) & CPUMCTX_EXTRN_RFLAGS));
    Assert(!fBlockSti || pVCpu->cpum.GstCtx.eflags.Bits.u1IF);     /* Cannot set block-by-STI when interrupts are disabled. */
    Assert(!(fIntrState & VMX_VMCS_GUEST_INT_STATE_BLOCK_SMI));    /* We don't support block-by-SMI yet.*/
    Assert(!TRPMHasTrap(pVCpu));

    VBOXSTRICTRC rcStrict = VINF_SUCCESS;
    if (pVCpu->hm.s.Event.fPending)
    {
        /*
         * Do -not- clear any interrupt-window exiting control here. We might have an interrupt
         * pending even while injecting an event and in this case, we want a VM-exit as soon as
         * the guest is ready for the next interrupt, see @bugref{6208#c45}.
         *
         * See Intel spec. 26.6.5 "Interrupt-Window Exiting and Virtual-Interrupt Delivery".
         */
        uint32_t const uIntType = VMX_ENTRY_INT_INFO_TYPE(pVCpu->hm.s.Event.u64IntInfo);
#ifdef VBOX_STRICT
        if (uIntType == VMX_ENTRY_INT_INFO_TYPE_EXT_INT)
        {
            bool const fBlockInt = !(pVCpu->cpum.GstCtx.eflags.u32 & X86_EFL_IF);
            Assert(!fBlockInt);
            Assert(!fBlockSti);
            Assert(!fBlockMovSS);
        }
        else if (uIntType == VMX_ENTRY_INT_INFO_TYPE_NMI)
        {
            bool const fBlockNmi = RT_BOOL(fIntrState & VMX_VMCS_GUEST_INT_STATE_BLOCK_NMI);
            Assert(!fBlockSti);
            Assert(!fBlockMovSS);
            Assert(!fBlockNmi);
        }
#endif
        Log4(("Injecting pending event vcpu[%RU32] u64IntInfo=%#RX64 Type=%#RX32\n", pVCpu->idCpu, pVCpu->hm.s.Event.u64IntInfo,
              uIntType));

        /*
         * Inject the event and get any changes to the guest-interruptibility state.
         *
         * The guest-interruptibility state may need to be updated if we inject the event
         * into the guest IDT ourselves (for real-on-v86 guest injecting software interrupts).
         */
        rcStrict = hmR0VmxInjectEventVmcs(pVCpu, pVmxTransient, &pVCpu->hm.s.Event, fStepping, &fIntrState);
        AssertRCReturn(VBOXSTRICTRC_VAL(rcStrict), rcStrict);

        /*
         * If we are executing a nested-guest make sure that we should intercept subsequent
         * events. The one we are injecting might be part of VM-entry.
         */
        if (pVmxTransient->fIsNestedGuest)
            pVCpu->cpum.GstCtx.hwvirt.vmx.fInterceptEvents = true;

        if (uIntType == VMX_ENTRY_INT_INFO_TYPE_EXT_INT)
            STAM_COUNTER_INC(&pVCpu->hm.s.StatInjectInterrupt);
        else
            STAM_COUNTER_INC(&pVCpu->hm.s.StatInjectXcpt);
    }

    /*
     * Update the guest-interruptibility state.
     *
     * This is required for the real-on-v86 software interrupt injection case above, as well as
     * updates to the guest state from ring-3 or IEM/REM.
     */
    int rc = VMXWriteVmcs32(VMX_VMCS32_GUEST_INT_STATE, fIntrState);
    AssertRCReturn(rc, rc);

    /*
     * There's no need to clear the VM-entry interruption-information field here if we're not
     * injecting anything. VT-x clears the valid bit on every VM-exit.
     *
     * See Intel spec. 24.8.3 "VM-Entry Controls for Event Injection".
     */

    Assert(rcStrict == VINF_SUCCESS || rcStrict == VINF_EM_RESET || (rcStrict == VINF_EM_DBG_STEPPED && fStepping));
    NOREF(fBlockMovSS); NOREF(fBlockSti);
    return rcStrict;
}


/**
 * Enters the VT-x session.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 */
VMMR0DECL(int) VMXR0Enter(PVMCPU pVCpu)
{
    AssertPtr(pVCpu);
    Assert(pVCpu->CTX_SUFF(pVM)->hm.s.vmx.fSupported);
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    LogFlowFunc(("pVCpu=%p\n", pVCpu));
    Assert((pVCpu->hm.s.fCtxChanged &  (HM_CHANGED_HOST_CONTEXT | HM_CHANGED_VMX_HOST_GUEST_SHARED_STATE))
                                    == (HM_CHANGED_HOST_CONTEXT | HM_CHANGED_VMX_HOST_GUEST_SHARED_STATE));

#ifdef VBOX_STRICT
    /* At least verify VMX is enabled, since we can't check if we're in VMX root mode without #GP'ing. */
    RTCCUINTREG uHostCr4 = ASMGetCR4();
    if (!(uHostCr4 & X86_CR4_VMXE))
    {
        LogRelFunc(("X86_CR4_VMXE bit in CR4 is not set!\n"));
        return VERR_VMX_X86_CR4_VMXE_CLEARED;
    }
#endif

    /*
     * Load the appropriate VMCS as the current and active one.
     */
    PVMXVMCSINFO pVmcsInfo;
    bool const fInNestedGuestMode = CPUMIsGuestInVmxNonRootMode(&pVCpu->cpum.GstCtx);
    if (!fInNestedGuestMode)
        pVmcsInfo = &pVCpu->hm.s.vmx.VmcsInfo;
    else
        pVmcsInfo = &pVCpu->hm.s.vmx.VmcsInfoNstGst;
    int rc = hmR0VmxLoadVmcs(pVmcsInfo);
    if (RT_SUCCESS(rc))
    {
        pVCpu->hm.s.vmx.fSwitchedToNstGstVmcs = fInNestedGuestMode;
        pVCpu->hm.s.fLeaveDone = false;
        Log4Func(("Loaded Vmcs. HostCpuId=%u\n", RTMpCpuId()));

        /*
         * Do the EMT scheduled L1D flush here if needed.
         */
        if (pVCpu->CTX_SUFF(pVM)->hm.s.fL1dFlushOnSched)
            ASMWrMsr(MSR_IA32_FLUSH_CMD, MSR_IA32_FLUSH_CMD_F_L1D);
        else if (pVCpu->CTX_SUFF(pVM)->hm.s.fMdsClearOnSched)
            hmR0MdsClear();
    }
    return rc;
}


/**
 * The thread-context callback (only on platforms which support it).
 *
 * @param   enmEvent        The thread-context event.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   fGlobalInit     Whether global VT-x/AMD-V init. was used.
 * @thread  EMT(pVCpu)
 */
VMMR0DECL(void) VMXR0ThreadCtxCallback(RTTHREADCTXEVENT enmEvent, PVMCPU pVCpu, bool fGlobalInit)
{
    AssertPtr(pVCpu);
    RT_NOREF1(fGlobalInit);

    switch (enmEvent)
    {
        case RTTHREADCTXEVENT_OUT:
        {
            Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));
            Assert(VMMR0ThreadCtxHookIsEnabled(pVCpu));
            VMCPU_ASSERT_EMT(pVCpu);

            /* No longjmps (logger flushes, locks) in this fragile context. */
            VMMRZCallRing3Disable(pVCpu);
            Log4Func(("Preempting: HostCpuId=%u\n", RTMpCpuId()));

            /* Restore host-state (FPU, debug etc.) */
            if (!pVCpu->hm.s.fLeaveDone)
            {
                /*
                 * Do -not- import the guest-state here as we might already be in the middle of importing
                 * it, esp. bad if we're holding the PGM lock, see comment in hmR0VmxImportGuestState().
                 */
                hmR0VmxLeave(pVCpu, false /* fImportState */);
                pVCpu->hm.s.fLeaveDone = true;
            }

            /* Leave HM context, takes care of local init (term). */
            int rc = HMR0LeaveCpu(pVCpu);
            AssertRC(rc);

            /* Restore longjmp state. */
            VMMRZCallRing3Enable(pVCpu);
            STAM_REL_COUNTER_INC(&pVCpu->hm.s.StatSwitchPreempt);
            break;
        }

        case RTTHREADCTXEVENT_IN:
        {
            Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));
            Assert(VMMR0ThreadCtxHookIsEnabled(pVCpu));
            VMCPU_ASSERT_EMT(pVCpu);

            /* No longjmps here, as we don't want to trigger preemption (& its hook) while resuming. */
            VMMRZCallRing3Disable(pVCpu);
            Log4Func(("Resumed: HostCpuId=%u\n", RTMpCpuId()));

            /* Initialize the bare minimum state required for HM. This takes care of
               initializing VT-x if necessary (onlined CPUs, local init etc.) */
            int rc = hmR0EnterCpu(pVCpu);
            AssertRC(rc);
            Assert(   (pVCpu->hm.s.fCtxChanged & (HM_CHANGED_HOST_CONTEXT | HM_CHANGED_VMX_HOST_GUEST_SHARED_STATE))
                   ==                            (HM_CHANGED_HOST_CONTEXT | HM_CHANGED_VMX_HOST_GUEST_SHARED_STATE));

            /* Load the active VMCS as the current one. */
            PVMXVMCSINFO pVmcsInfo = hmGetVmxActiveVmcsInfo(pVCpu);
            rc = hmR0VmxLoadVmcs(pVmcsInfo);
            AssertRC(rc);
            Log4Func(("Resumed: Loaded Vmcs. HostCpuId=%u\n", RTMpCpuId()));
            pVCpu->hm.s.fLeaveDone = false;

            /* Do the EMT scheduled L1D flush if needed. */
            if (pVCpu->CTX_SUFF(pVM)->hm.s.fL1dFlushOnSched)
                ASMWrMsr(MSR_IA32_FLUSH_CMD, MSR_IA32_FLUSH_CMD_F_L1D);

            /* Restore longjmp state. */
            VMMRZCallRing3Enable(pVCpu);
            break;
        }

        default:
            break;
    }
}


/**
 * Exports the host state into the VMCS host-state area.
 * Sets up the VM-exit MSR-load area.
 *
 * The CPU state will be loaded from these fields on every successful VM-exit.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 *
 * @remarks No-long-jump zone!!!
 */
static int hmR0VmxExportHostState(PVMCPU pVCpu)
{
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    int rc = VINF_SUCCESS;
    if (pVCpu->hm.s.fCtxChanged & HM_CHANGED_HOST_CONTEXT)
    {
        rc = hmR0VmxExportHostControlRegs();
        AssertLogRelMsgRCReturn(rc, ("rc=%Rrc\n", rc), rc);

        rc = hmR0VmxExportHostSegmentRegs(pVCpu);
        AssertLogRelMsgRCReturn(rc, ("rc=%Rrc\n", rc), rc);

        rc = hmR0VmxExportHostMsrs(pVCpu);
        AssertLogRelMsgRCReturn(rc, ("rc=%Rrc\n", rc), rc);

        pVCpu->hm.s.fCtxChanged &= ~HM_CHANGED_HOST_CONTEXT;
    }
    return rc;
}


/**
 * Saves the host state in the VMCS host-state.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 *
 * @remarks No-long-jump zone!!!
 */
VMMR0DECL(int) VMXR0ExportHostState(PVMCPU pVCpu)
{
    AssertPtr(pVCpu);
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    /*
     * Export the host state here while entering HM context.
     * When thread-context hooks are used, we might get preempted and have to re-save the host
     * state but most of the time we won't be, so do it here before we disable interrupts.
     */
    return hmR0VmxExportHostState(pVCpu);
}


/**
 * Exports the guest state into the VMCS guest-state area.
 *
 * The will typically be done before VM-entry when the guest-CPU state and the
 * VMCS state may potentially be out of sync.
 *
 * Sets up the VM-entry MSR-load and VM-exit MSR-store areas. Sets up the
 * VM-entry controls.
 * Sets up the appropriate VMX non-root function to execute guest code based on
 * the guest CPU mode.
 *
 * @returns VBox strict status code.
 * @retval  VINF_EM_RESCHEDULE_REM if we try to emulate non-paged guest code
 *          without unrestricted guest execution and the VMMDev is not presently
 *          mapped (e.g. EFI32).
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
static VBOXSTRICTRC hmR0VmxExportGuestState(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    AssertPtr(pVCpu);
    HMVMX_ASSERT_PREEMPT_SAFE(pVCpu);
    LogFlowFunc(("pVCpu=%p\n", pVCpu));

    STAM_PROFILE_ADV_START(&pVCpu->hm.s.StatExportGuestState, x);

    /*
     * Determine real-on-v86 mode.
     * Used when the guest is in real-mode and unrestricted guest execution is not used.
     */
    PVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    if (    pVCpu->CTX_SUFF(pVM)->hm.s.vmx.fUnrestrictedGuest
        || !CPUMIsGuestInRealModeEx(&pVCpu->cpum.GstCtx))
        pVmcsInfo->RealMode. fRealOnV86Active = false;
    else
    {
        Assert(!pVmxTransient->fIsNestedGuest);
        pVmcsInfo->RealMode.fRealOnV86Active = true;
    }

    /*
     * Any ordering dependency among the sub-functions below must be explicitly stated using comments.
     * Ideally, assert that the cross-dependent bits are up-to-date at the point of using it.
     */
    /** @todo r=ramshankar: Move hmR0VmxSelectVMRunHandler inside
     *        hmR0VmxExportGuestEntryExitCtls and do it conditionally. There shouldn't
     *        be a need to evaluate this everytime since I'm pretty sure we intercept
     *        all guest paging mode changes. */
    int rc = hmR0VmxSelectVMRunHandler(pVCpu, pVmxTransient);
    AssertLogRelMsgRCReturn(rc, ("rc=%Rrc\n", rc), rc);

    rc = hmR0VmxExportGuestEntryExitCtls(pVCpu, pVmxTransient);
    AssertLogRelMsgRCReturn(rc, ("rc=%Rrc\n", rc), rc);

    rc = hmR0VmxExportGuestCR0(pVCpu, pVmxTransient);
    AssertLogRelMsgRCReturn(rc, ("rc=%Rrc\n", rc), rc);

    VBOXSTRICTRC rcStrict = hmR0VmxExportGuestCR3AndCR4(pVCpu, pVmxTransient);
    if (rcStrict == VINF_SUCCESS)
    { /* likely */ }
    else
    {
        Assert(rcStrict == VINF_EM_RESCHEDULE_REM || RT_FAILURE_NP(rcStrict));
        return rcStrict;
    }

    rc = hmR0VmxExportGuestSegRegsXdtr(pVCpu, pVmxTransient);
    AssertLogRelMsgRCReturn(rc, ("rc=%Rrc\n", rc), rc);

    rc = hmR0VmxExportGuestMsrs(pVCpu, pVmxTransient);
    AssertLogRelMsgRCReturn(rc, ("rc=%Rrc\n", rc), rc);

    rc = hmR0VmxExportGuestApicTpr(pVCpu, pVmxTransient);
    AssertLogRelMsgRCReturn(rc, ("rc=%Rrc\n", rc), rc);

    rc = hmR0VmxExportGuestXcptIntercepts(pVCpu, pVmxTransient);
    AssertLogRelMsgRCReturn(rc, ("rc=%Rrc\n", rc), rc);

    rc  = hmR0VmxExportGuestRip(pVCpu);
    rc |= hmR0VmxExportGuestRsp(pVCpu);
    rc |= hmR0VmxExportGuestRflags(pVCpu, pVmxTransient);
    AssertLogRelMsgRCReturn(rc, ("rc=%Rrc\n", rc), rc);

    /* Clear any bits that may be set but exported unconditionally or unused/reserved bits. */
    ASMAtomicUoAndU64(&pVCpu->hm.s.fCtxChanged, ~(  (HM_CHANGED_GUEST_GPRS_MASK & ~HM_CHANGED_GUEST_RSP)
                                                  |  HM_CHANGED_GUEST_CR2
                                                  | (HM_CHANGED_GUEST_DR_MASK & ~HM_CHANGED_GUEST_DR7)
                                                  |  HM_CHANGED_GUEST_X87
                                                  |  HM_CHANGED_GUEST_SSE_AVX
                                                  |  HM_CHANGED_GUEST_OTHER_XSAVE
                                                  |  HM_CHANGED_GUEST_XCRx
                                                  |  HM_CHANGED_GUEST_KERNEL_GS_BASE /* Part of lazy or auto load-store MSRs. */
                                                  |  HM_CHANGED_GUEST_SYSCALL_MSRS   /* Part of lazy or auto load-store MSRs. */
                                                  |  HM_CHANGED_GUEST_TSC_AUX
                                                  |  HM_CHANGED_GUEST_OTHER_MSRS
                                                  |  HM_CHANGED_GUEST_HWVIRT         /* More accurate PLE handling someday? */
                                                  | (HM_CHANGED_KEEPER_STATE_MASK & ~HM_CHANGED_VMX_MASK)));

    STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatExportGuestState, x);
    return rc;
}


/**
 * Exports the state shared between the host and guest into the VMCS.
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
static void hmR0VmxExportSharedState(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));
    Assert(!VMMRZCallRing3IsEnabled(pVCpu));

    if (pVCpu->hm.s.fCtxChanged & HM_CHANGED_GUEST_DR_MASK)
    {
        int rc = hmR0VmxExportSharedDebugState(pVCpu, pVmxTransient);
        AssertRC(rc);
        pVCpu->hm.s.fCtxChanged &= ~HM_CHANGED_GUEST_DR_MASK;

        /* Loading shared debug bits might have changed eflags.TF bit for debugging purposes. */
        if (pVCpu->hm.s.fCtxChanged & HM_CHANGED_GUEST_RFLAGS)
        {
            rc = hmR0VmxExportGuestRflags(pVCpu, pVmxTransient);
            AssertRC(rc);
        }
    }

    if (pVCpu->hm.s.fCtxChanged & HM_CHANGED_VMX_GUEST_LAZY_MSRS)
    {
        hmR0VmxLazyLoadGuestMsrs(pVCpu);
        pVCpu->hm.s.fCtxChanged &= ~HM_CHANGED_VMX_GUEST_LAZY_MSRS;
    }

    AssertMsg(!(pVCpu->hm.s.fCtxChanged & HM_CHANGED_VMX_HOST_GUEST_SHARED_STATE),
              ("fCtxChanged=%#RX64\n", pVCpu->hm.s.fCtxChanged));
}


/**
 * Worker for loading the guest-state bits in the inner VT-x execution loop.
 *
 * @returns Strict VBox status code (i.e. informational status codes too).
 * @retval  VINF_EM_RESCHEDULE_REM if we try to emulate non-paged guest code
 *          without unrestricted guest execution and the VMMDev is not presently
 *          mapped (e.g. EFI32).
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
static VBOXSTRICTRC hmR0VmxExportGuestStateOptimal(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_ASSERT_PREEMPT_SAFE(pVCpu);
    Assert(!VMMRZCallRing3IsEnabled(pVCpu));
    Assert(VMMR0IsLogFlushDisabled(pVCpu));

#ifdef HMVMX_ALWAYS_SYNC_FULL_GUEST_STATE
    ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_ALL_GUEST);
#endif

    /*
     * For many exits it's only RIP that changes and hence try to export it first
     * without going through a lot of change flag checks.
     */
    VBOXSTRICTRC rcStrict;
    uint64_t     fCtxChanged = ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged);
    RT_UNTRUSTED_NONVOLATILE_COPY_FENCE();
    if ((fCtxChanged & (HM_CHANGED_ALL_GUEST & ~HM_CHANGED_VMX_HOST_GUEST_SHARED_STATE)) == HM_CHANGED_GUEST_RIP)
    {
        rcStrict = hmR0VmxExportGuestRip(pVCpu);
        if (RT_LIKELY(rcStrict == VINF_SUCCESS))
        { /* likely */}
        else
            AssertMsgFailedReturn(("Failed to export guest RIP! rc=%Rrc\n", VBOXSTRICTRC_VAL(rcStrict)), rcStrict);
        STAM_COUNTER_INC(&pVCpu->hm.s.StatExportMinimal);
    }
    else if (fCtxChanged & (HM_CHANGED_ALL_GUEST & ~HM_CHANGED_VMX_HOST_GUEST_SHARED_STATE))
    {
        rcStrict = hmR0VmxExportGuestState(pVCpu, pVmxTransient);
        if (RT_LIKELY(rcStrict == VINF_SUCCESS))
        { /* likely */}
        else
        {
            AssertMsg(rcStrict == VINF_EM_RESCHEDULE_REM, ("Failed to export guest state! rc=%Rrc\n",
                                                           VBOXSTRICTRC_VAL(rcStrict)));
            Assert(!VMMRZCallRing3IsEnabled(pVCpu));
            return rcStrict;
        }
        STAM_COUNTER_INC(&pVCpu->hm.s.StatExportFull);
    }
    else
        rcStrict = VINF_SUCCESS;

#ifdef VBOX_STRICT
    /* All the guest state bits should be loaded except maybe the host context and/or the shared host/guest bits. */
    fCtxChanged = ASMAtomicUoReadU64(&pVCpu->hm.s.fCtxChanged);
    RT_UNTRUSTED_NONVOLATILE_COPY_FENCE();
    AssertMsg(!(fCtxChanged & (HM_CHANGED_ALL_GUEST & ~HM_CHANGED_VMX_HOST_GUEST_SHARED_STATE)),
              ("fCtxChanged=%#RX64\n", fCtxChanged));
#endif
    return rcStrict;
}


/**
 * Tries to determine what part of the guest-state VT-x has deemed as invalid
 * and update error record fields accordingly.
 *
 * @returns VMX_IGS_* error codes.
 * @retval VMX_IGS_REASON_NOT_FOUND if this function could not find anything
 *         wrong with the guest state.
 *
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pVmcsInfo   The VMCS info. object.
 *
 * @remarks This function assumes our cache of the VMCS controls
 *          are valid, i.e. hmR0VmxCheckVmcsCtls() succeeded.
 */
static uint32_t hmR0VmxCheckGuestState(PVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo)
{
#define HMVMX_ERROR_BREAK(err)              { uError = (err); break; }
#define HMVMX_CHECK_BREAK(expr, err)        if (!(expr)) { \
                                                uError = (err); \
                                                break; \
                                            } else do { } while (0)

    int        rc;
    PVM        pVM    = pVCpu->CTX_SUFF(pVM);
    PCPUMCTX   pCtx   = &pVCpu->cpum.GstCtx;
    uint32_t   uError = VMX_IGS_ERROR;
    uint32_t   u32Val;
    bool const fUnrestrictedGuest = pVM->hm.s.vmx.fUnrestrictedGuest;

    do
    {
        /*
         * CR0.
         */
        /** @todo Why do we need to OR and AND the fixed-0 and fixed-1 bits below? */
        uint32_t       fSetCr0 = (uint32_t)(pVM->hm.s.vmx.Msrs.u64Cr0Fixed0 & pVM->hm.s.vmx.Msrs.u64Cr0Fixed1);
        uint32_t const fZapCr0 = (uint32_t)(pVM->hm.s.vmx.Msrs.u64Cr0Fixed0 | pVM->hm.s.vmx.Msrs.u64Cr0Fixed1);
        /* Exceptions for unrestricted guest execution for fixed CR0 bits (PE, PG).
           See Intel spec. 26.3.1 "Checks on Guest Control Registers, Debug Registers and MSRs." */
        if (fUnrestrictedGuest)
            fSetCr0 &= ~(X86_CR0_PE | X86_CR0_PG);

        uint32_t u32GuestCr0;
        rc = VMXReadVmcs32(VMX_VMCS_GUEST_CR0, &u32GuestCr0);
        AssertRCBreak(rc);
        HMVMX_CHECK_BREAK((u32GuestCr0 & fSetCr0) == fSetCr0, VMX_IGS_CR0_FIXED1);
        HMVMX_CHECK_BREAK(!(u32GuestCr0 & ~fZapCr0), VMX_IGS_CR0_FIXED0);
        if (   !fUnrestrictedGuest
            &&  (u32GuestCr0 & X86_CR0_PG)
            && !(u32GuestCr0 & X86_CR0_PE))
        {
            HMVMX_ERROR_BREAK(VMX_IGS_CR0_PG_PE_COMBO);
        }

        /*
         * CR4.
         */
        /** @todo Why do we need to OR and AND the fixed-0 and fixed-1 bits below? */
        uint64_t const fSetCr4 = (pVM->hm.s.vmx.Msrs.u64Cr4Fixed0 & pVM->hm.s.vmx.Msrs.u64Cr4Fixed1);
        uint64_t const fZapCr4 = (pVM->hm.s.vmx.Msrs.u64Cr4Fixed0 | pVM->hm.s.vmx.Msrs.u64Cr4Fixed1);

        uint32_t u32GuestCr4;
        rc = VMXReadVmcs32(VMX_VMCS_GUEST_CR4, &u32GuestCr4);
        AssertRCBreak(rc);
        HMVMX_CHECK_BREAK((u32GuestCr4 & fSetCr4) == fSetCr4, VMX_IGS_CR4_FIXED1);
        HMVMX_CHECK_BREAK(!(u32GuestCr4 & ~fZapCr4), VMX_IGS_CR4_FIXED0);

        /*
         * IA32_DEBUGCTL MSR.
         */
        uint64_t u64Val;
        rc = VMXReadVmcs64(VMX_VMCS64_GUEST_DEBUGCTL_FULL, &u64Val);
        AssertRCBreak(rc);
        if (   (pVmcsInfo->u32EntryCtls & VMX_ENTRY_CTLS_LOAD_DEBUG)
            && (u64Val & 0xfffffe3c))                           /* Bits 31:9, bits 5:2 MBZ. */
        {
            HMVMX_ERROR_BREAK(VMX_IGS_DEBUGCTL_MSR_RESERVED);
        }
        uint64_t u64DebugCtlMsr = u64Val;

#ifdef VBOX_STRICT
        rc = VMXReadVmcs32(VMX_VMCS32_CTRL_ENTRY, &u32Val);
        AssertRCBreak(rc);
        Assert(u32Val == pVmcsInfo->u32EntryCtls);
#endif
        bool const fLongModeGuest = RT_BOOL(pVmcsInfo->u32EntryCtls & VMX_ENTRY_CTLS_IA32E_MODE_GUEST);

        /*
         * RIP and RFLAGS.
         */
        uint32_t u32Eflags;
#if HC_ARCH_BITS == 64
        rc = VMXReadVmcs64(VMX_VMCS_GUEST_RIP, &u64Val);
        AssertRCBreak(rc);
        /* pCtx->rip can be different than the one in the VMCS (e.g. run guest code and VM-exits that don't update it). */
        if (   !fLongModeGuest
            || !pCtx->cs.Attr.n.u1Long)
        {
            HMVMX_CHECK_BREAK(!(u64Val & UINT64_C(0xffffffff00000000)), VMX_IGS_LONGMODE_RIP_INVALID);
        }
        /** @todo If the processor supports N < 64 linear-address bits, bits 63:N
         *        must be identical if the "IA-32e mode guest" VM-entry
         *        control is 1 and CS.L is 1. No check applies if the
         *        CPU supports 64 linear-address bits. */

        /* Flags in pCtx can be different (real-on-v86 for instance). We are only concerned about the VMCS contents here. */
        rc = VMXReadVmcs64(VMX_VMCS_GUEST_RFLAGS, &u64Val);
        AssertRCBreak(rc);
        HMVMX_CHECK_BREAK(!(u64Val & UINT64_C(0xffffffffffc08028)),                     /* Bit 63:22, Bit 15, 5, 3 MBZ. */
                          VMX_IGS_RFLAGS_RESERVED);
        HMVMX_CHECK_BREAK((u64Val & X86_EFL_RA1_MASK), VMX_IGS_RFLAGS_RESERVED1);       /* Bit 1 MB1. */
        u32Eflags = u64Val;
#else
        rc = VMXReadVmcs32(VMX_VMCS_GUEST_RFLAGS, &u32Eflags);
        AssertRCBreak(rc);
        HMVMX_CHECK_BREAK(!(u32Eflags & 0xffc08028), VMX_IGS_RFLAGS_RESERVED);          /* Bit 31:22, Bit 15, 5, 3 MBZ. */
        HMVMX_CHECK_BREAK((u32Eflags & X86_EFL_RA1_MASK), VMX_IGS_RFLAGS_RESERVED1);    /* Bit 1 MB1. */
#endif

        if (   fLongModeGuest
            || (   fUnrestrictedGuest
                && !(u32GuestCr0 & X86_CR0_PE)))
        {
            HMVMX_CHECK_BREAK(!(u32Eflags & X86_EFL_VM), VMX_IGS_RFLAGS_VM_INVALID);
        }

        uint32_t u32EntryInfo;
        rc = VMXReadVmcs32(VMX_VMCS32_CTRL_ENTRY_INTERRUPTION_INFO, &u32EntryInfo);
        AssertRCBreak(rc);
        if (   VMX_ENTRY_INT_INFO_IS_VALID(u32EntryInfo)
            && VMX_ENTRY_INT_INFO_TYPE(u32EntryInfo) == VMX_EXIT_INT_INFO_TYPE_EXT_INT)
        {
            HMVMX_CHECK_BREAK(u32Eflags & X86_EFL_IF, VMX_IGS_RFLAGS_IF_INVALID);
        }

        /*
         * 64-bit checks.
         */
#if HC_ARCH_BITS == 64
        if (fLongModeGuest)
        {
            HMVMX_CHECK_BREAK(u32GuestCr0 & X86_CR0_PG, VMX_IGS_CR0_PG_LONGMODE);
            HMVMX_CHECK_BREAK(u32GuestCr4 & X86_CR4_PAE, VMX_IGS_CR4_PAE_LONGMODE);
        }

        if (   !fLongModeGuest
            && (u32GuestCr4 & X86_CR4_PCIDE))
        {
            HMVMX_ERROR_BREAK(VMX_IGS_CR4_PCIDE);
        }

        /** @todo CR3 field must be such that bits 63:52 and bits in the range
         *        51:32 beyond the processor's physical-address width are 0. */

        if (   (pVmcsInfo->u32EntryCtls & VMX_ENTRY_CTLS_LOAD_DEBUG)
            && (pCtx->dr[7] & X86_DR7_MBZ_MASK))
        {
            HMVMX_ERROR_BREAK(VMX_IGS_DR7_RESERVED);
        }

        rc = VMXReadVmcs64(VMX_VMCS_HOST_SYSENTER_ESP, &u64Val);
        AssertRCBreak(rc);
        HMVMX_CHECK_BREAK(X86_IS_CANONICAL(u64Val), VMX_IGS_SYSENTER_ESP_NOT_CANONICAL);

        rc = VMXReadVmcs64(VMX_VMCS_HOST_SYSENTER_EIP, &u64Val);
        AssertRCBreak(rc);
        HMVMX_CHECK_BREAK(X86_IS_CANONICAL(u64Val), VMX_IGS_SYSENTER_EIP_NOT_CANONICAL);
#endif

        /*
         * PERF_GLOBAL MSR.
         */
        if (pVmcsInfo->u32EntryCtls & VMX_ENTRY_CTLS_LOAD_PERF_MSR)
        {
            rc = VMXReadVmcs64(VMX_VMCS64_GUEST_PERF_GLOBAL_CTRL_FULL, &u64Val);
            AssertRCBreak(rc);
            HMVMX_CHECK_BREAK(!(u64Val & UINT64_C(0xfffffff8fffffffc)),
                              VMX_IGS_PERF_GLOBAL_MSR_RESERVED);        /* Bits 63:35, bits 31:2 MBZ. */
        }

        /*
         * PAT MSR.
         */
        if (pVmcsInfo->u32EntryCtls & VMX_ENTRY_CTLS_LOAD_PAT_MSR)
        {
            rc = VMXReadVmcs64(VMX_VMCS64_GUEST_PAT_FULL, &u64Val);
            AssertRCBreak(rc);
            HMVMX_CHECK_BREAK(!(u64Val & UINT64_C(0x707070707070707)), VMX_IGS_PAT_MSR_RESERVED);
            for (unsigned i = 0; i < 8; i++)
            {
                uint8_t u8Val = (u64Val & 0xff);
                if (   u8Val != 0 /* UC */
                    && u8Val != 1 /* WC */
                    && u8Val != 4 /* WT */
                    && u8Val != 5 /* WP */
                    && u8Val != 6 /* WB */
                    && u8Val != 7 /* UC- */)
                {
                    HMVMX_ERROR_BREAK(VMX_IGS_PAT_MSR_INVALID);
                }
                u64Val >>= 8;
            }
        }

        /*
         * EFER MSR.
         */
        if (pVmcsInfo->u32EntryCtls & VMX_ENTRY_CTLS_LOAD_EFER_MSR)
        {
            Assert(pVM->hm.s.vmx.fSupportsVmcsEfer);
            rc = VMXReadVmcs64(VMX_VMCS64_GUEST_EFER_FULL, &u64Val);
            AssertRCBreak(rc);
            HMVMX_CHECK_BREAK(!(u64Val & UINT64_C(0xfffffffffffff2fe)),
                              VMX_IGS_EFER_MSR_RESERVED);               /* Bits 63:12, bit 9, bits 7:1 MBZ. */
            HMVMX_CHECK_BREAK(RT_BOOL(u64Val & MSR_K6_EFER_LMA) == RT_BOOL(  pVmcsInfo->u32EntryCtls
                                                                           & VMX_ENTRY_CTLS_IA32E_MODE_GUEST),
                              VMX_IGS_EFER_LMA_GUEST_MODE_MISMATCH);
            /** @todo r=ramshankar: Unrestricted check here is probably wrong, see
             *        iemVmxVmentryCheckGuestState(). */
            HMVMX_CHECK_BREAK(   fUnrestrictedGuest
                              || !(u32GuestCr0 & X86_CR0_PG)
                              || RT_BOOL(u64Val & MSR_K6_EFER_LMA) == RT_BOOL(u64Val & MSR_K6_EFER_LME),
                              VMX_IGS_EFER_LMA_LME_MISMATCH);
        }

        /*
         * Segment registers.
         */
        HMVMX_CHECK_BREAK(   (pCtx->ldtr.Attr.u & X86DESCATTR_UNUSABLE)
                          || !(pCtx->ldtr.Sel & X86_SEL_LDT), VMX_IGS_LDTR_TI_INVALID);
        if (!(u32Eflags & X86_EFL_VM))
        {
            /* CS */
            HMVMX_CHECK_BREAK(pCtx->cs.Attr.n.u1Present, VMX_IGS_CS_ATTR_P_INVALID);
            HMVMX_CHECK_BREAK(!(pCtx->cs.Attr.u & 0xf00), VMX_IGS_CS_ATTR_RESERVED);
            HMVMX_CHECK_BREAK(!(pCtx->cs.Attr.u & 0xfffe0000), VMX_IGS_CS_ATTR_RESERVED);
            HMVMX_CHECK_BREAK(   (pCtx->cs.u32Limit & 0xfff) == 0xfff
                              || !(pCtx->cs.Attr.n.u1Granularity), VMX_IGS_CS_ATTR_G_INVALID);
            HMVMX_CHECK_BREAK(   !(pCtx->cs.u32Limit & 0xfff00000)
                              || (pCtx->cs.Attr.n.u1Granularity), VMX_IGS_CS_ATTR_G_INVALID);
            /* CS cannot be loaded with NULL in protected mode. */
            HMVMX_CHECK_BREAK(pCtx->cs.Attr.u && !(pCtx->cs.Attr.u & X86DESCATTR_UNUSABLE), VMX_IGS_CS_ATTR_UNUSABLE);
            HMVMX_CHECK_BREAK(pCtx->cs.Attr.n.u1DescType, VMX_IGS_CS_ATTR_S_INVALID);
            if (pCtx->cs.Attr.n.u4Type == 9 || pCtx->cs.Attr.n.u4Type == 11)
                HMVMX_CHECK_BREAK(pCtx->cs.Attr.n.u2Dpl == pCtx->ss.Attr.n.u2Dpl, VMX_IGS_CS_SS_ATTR_DPL_UNEQUAL);
            else if (pCtx->cs.Attr.n.u4Type == 13 || pCtx->cs.Attr.n.u4Type == 15)
                HMVMX_CHECK_BREAK(pCtx->cs.Attr.n.u2Dpl <= pCtx->ss.Attr.n.u2Dpl, VMX_IGS_CS_SS_ATTR_DPL_MISMATCH);
            else if (pVM->hm.s.vmx.fUnrestrictedGuest && pCtx->cs.Attr.n.u4Type == 3)
                HMVMX_CHECK_BREAK(pCtx->cs.Attr.n.u2Dpl == 0, VMX_IGS_CS_ATTR_DPL_INVALID);
            else
                HMVMX_ERROR_BREAK(VMX_IGS_CS_ATTR_TYPE_INVALID);

            /* SS */
            HMVMX_CHECK_BREAK(   pVM->hm.s.vmx.fUnrestrictedGuest
                              || (pCtx->ss.Sel & X86_SEL_RPL) == (pCtx->cs.Sel & X86_SEL_RPL), VMX_IGS_SS_CS_RPL_UNEQUAL);
            HMVMX_CHECK_BREAK(pCtx->ss.Attr.n.u2Dpl == (pCtx->ss.Sel & X86_SEL_RPL), VMX_IGS_SS_ATTR_DPL_RPL_UNEQUAL);
            if (   !(pCtx->cr0 & X86_CR0_PE)
                || pCtx->cs.Attr.n.u4Type == 3)
            {
                HMVMX_CHECK_BREAK(!pCtx->ss.Attr.n.u2Dpl, VMX_IGS_SS_ATTR_DPL_INVALID);
            }
            if (!(pCtx->ss.Attr.u & X86DESCATTR_UNUSABLE))
            {
                HMVMX_CHECK_BREAK(pCtx->ss.Attr.n.u4Type == 3 || pCtx->ss.Attr.n.u4Type == 7, VMX_IGS_SS_ATTR_TYPE_INVALID);
                HMVMX_CHECK_BREAK(pCtx->ss.Attr.n.u1Present, VMX_IGS_SS_ATTR_P_INVALID);
                HMVMX_CHECK_BREAK(!(pCtx->ss.Attr.u & 0xf00), VMX_IGS_SS_ATTR_RESERVED);
                HMVMX_CHECK_BREAK(!(pCtx->ss.Attr.u & 0xfffe0000), VMX_IGS_SS_ATTR_RESERVED);
                HMVMX_CHECK_BREAK(   (pCtx->ss.u32Limit & 0xfff) == 0xfff
                                  || !(pCtx->ss.Attr.n.u1Granularity), VMX_IGS_SS_ATTR_G_INVALID);
                HMVMX_CHECK_BREAK(   !(pCtx->ss.u32Limit & 0xfff00000)
                                  || (pCtx->ss.Attr.n.u1Granularity), VMX_IGS_SS_ATTR_G_INVALID);
            }

            /* DS, ES, FS, GS - only check for usable selectors, see hmR0VmxExportGuestSReg(). */
            if (!(pCtx->ds.Attr.u & X86DESCATTR_UNUSABLE))
            {
                HMVMX_CHECK_BREAK(pCtx->ds.Attr.n.u4Type & X86_SEL_TYPE_ACCESSED, VMX_IGS_DS_ATTR_A_INVALID);
                HMVMX_CHECK_BREAK(pCtx->ds.Attr.n.u1Present, VMX_IGS_DS_ATTR_P_INVALID);
                HMVMX_CHECK_BREAK(   pVM->hm.s.vmx.fUnrestrictedGuest
                                  || pCtx->ds.Attr.n.u4Type > 11
                                  || pCtx->ds.Attr.n.u2Dpl >= (pCtx->ds.Sel & X86_SEL_RPL), VMX_IGS_DS_ATTR_DPL_RPL_UNEQUAL);
                HMVMX_CHECK_BREAK(!(pCtx->ds.Attr.u & 0xf00), VMX_IGS_DS_ATTR_RESERVED);
                HMVMX_CHECK_BREAK(!(pCtx->ds.Attr.u & 0xfffe0000), VMX_IGS_DS_ATTR_RESERVED);
                HMVMX_CHECK_BREAK(   (pCtx->ds.u32Limit & 0xfff) == 0xfff
                                  || !(pCtx->ds.Attr.n.u1Granularity), VMX_IGS_DS_ATTR_G_INVALID);
                HMVMX_CHECK_BREAK(   !(pCtx->ds.u32Limit & 0xfff00000)
                                  || (pCtx->ds.Attr.n.u1Granularity), VMX_IGS_DS_ATTR_G_INVALID);
                HMVMX_CHECK_BREAK(   !(pCtx->ds.Attr.n.u4Type & X86_SEL_TYPE_CODE)
                                  || (pCtx->ds.Attr.n.u4Type & X86_SEL_TYPE_READ), VMX_IGS_DS_ATTR_TYPE_INVALID);
            }
            if (!(pCtx->es.Attr.u & X86DESCATTR_UNUSABLE))
            {
                HMVMX_CHECK_BREAK(pCtx->es.Attr.n.u4Type & X86_SEL_TYPE_ACCESSED, VMX_IGS_ES_ATTR_A_INVALID);
                HMVMX_CHECK_BREAK(pCtx->es.Attr.n.u1Present, VMX_IGS_ES_ATTR_P_INVALID);
                HMVMX_CHECK_BREAK(   pVM->hm.s.vmx.fUnrestrictedGuest
                                  || pCtx->es.Attr.n.u4Type > 11
                                  || pCtx->es.Attr.n.u2Dpl >= (pCtx->es.Sel & X86_SEL_RPL), VMX_IGS_DS_ATTR_DPL_RPL_UNEQUAL);
                HMVMX_CHECK_BREAK(!(pCtx->es.Attr.u & 0xf00), VMX_IGS_ES_ATTR_RESERVED);
                HMVMX_CHECK_BREAK(!(pCtx->es.Attr.u & 0xfffe0000), VMX_IGS_ES_ATTR_RESERVED);
                HMVMX_CHECK_BREAK(   (pCtx->es.u32Limit & 0xfff) == 0xfff
                                  || !(pCtx->es.Attr.n.u1Granularity), VMX_IGS_ES_ATTR_G_INVALID);
                HMVMX_CHECK_BREAK(   !(pCtx->es.u32Limit & 0xfff00000)
                                  || (pCtx->es.Attr.n.u1Granularity), VMX_IGS_ES_ATTR_G_INVALID);
                HMVMX_CHECK_BREAK(   !(pCtx->es.Attr.n.u4Type & X86_SEL_TYPE_CODE)
                                  || (pCtx->es.Attr.n.u4Type & X86_SEL_TYPE_READ), VMX_IGS_ES_ATTR_TYPE_INVALID);
            }
            if (!(pCtx->fs.Attr.u & X86DESCATTR_UNUSABLE))
            {
                HMVMX_CHECK_BREAK(pCtx->fs.Attr.n.u4Type & X86_SEL_TYPE_ACCESSED, VMX_IGS_FS_ATTR_A_INVALID);
                HMVMX_CHECK_BREAK(pCtx->fs.Attr.n.u1Present, VMX_IGS_FS_ATTR_P_INVALID);
                HMVMX_CHECK_BREAK(   pVM->hm.s.vmx.fUnrestrictedGuest
                                  || pCtx->fs.Attr.n.u4Type > 11
                                  || pCtx->fs.Attr.n.u2Dpl >= (pCtx->fs.Sel & X86_SEL_RPL), VMX_IGS_FS_ATTR_DPL_RPL_UNEQUAL);
                HMVMX_CHECK_BREAK(!(pCtx->fs.Attr.u & 0xf00), VMX_IGS_FS_ATTR_RESERVED);
                HMVMX_CHECK_BREAK(!(pCtx->fs.Attr.u & 0xfffe0000), VMX_IGS_FS_ATTR_RESERVED);
                HMVMX_CHECK_BREAK(   (pCtx->fs.u32Limit & 0xfff) == 0xfff
                                  || !(pCtx->fs.Attr.n.u1Granularity), VMX_IGS_FS_ATTR_G_INVALID);
                HMVMX_CHECK_BREAK(   !(pCtx->fs.u32Limit & 0xfff00000)
                                  || (pCtx->fs.Attr.n.u1Granularity), VMX_IGS_FS_ATTR_G_INVALID);
                HMVMX_CHECK_BREAK(   !(pCtx->fs.Attr.n.u4Type & X86_SEL_TYPE_CODE)
                                  || (pCtx->fs.Attr.n.u4Type & X86_SEL_TYPE_READ), VMX_IGS_FS_ATTR_TYPE_INVALID);
            }
            if (!(pCtx->gs.Attr.u & X86DESCATTR_UNUSABLE))
            {
                HMVMX_CHECK_BREAK(pCtx->gs.Attr.n.u4Type & X86_SEL_TYPE_ACCESSED, VMX_IGS_GS_ATTR_A_INVALID);
                HMVMX_CHECK_BREAK(pCtx->gs.Attr.n.u1Present, VMX_IGS_GS_ATTR_P_INVALID);
                HMVMX_CHECK_BREAK(   pVM->hm.s.vmx.fUnrestrictedGuest
                                  || pCtx->gs.Attr.n.u4Type > 11
                                  || pCtx->gs.Attr.n.u2Dpl >= (pCtx->gs.Sel & X86_SEL_RPL), VMX_IGS_GS_ATTR_DPL_RPL_UNEQUAL);
                HMVMX_CHECK_BREAK(!(pCtx->gs.Attr.u & 0xf00), VMX_IGS_GS_ATTR_RESERVED);
                HMVMX_CHECK_BREAK(!(pCtx->gs.Attr.u & 0xfffe0000), VMX_IGS_GS_ATTR_RESERVED);
                HMVMX_CHECK_BREAK(   (pCtx->gs.u32Limit & 0xfff) == 0xfff
                                  || !(pCtx->gs.Attr.n.u1Granularity), VMX_IGS_GS_ATTR_G_INVALID);
                HMVMX_CHECK_BREAK(   !(pCtx->gs.u32Limit & 0xfff00000)
                                  || (pCtx->gs.Attr.n.u1Granularity), VMX_IGS_GS_ATTR_G_INVALID);
                HMVMX_CHECK_BREAK(   !(pCtx->gs.Attr.n.u4Type & X86_SEL_TYPE_CODE)
                                  || (pCtx->gs.Attr.n.u4Type & X86_SEL_TYPE_READ), VMX_IGS_GS_ATTR_TYPE_INVALID);
            }
            /* 64-bit capable CPUs. */
#if HC_ARCH_BITS == 64
            HMVMX_CHECK_BREAK(X86_IS_CANONICAL(pCtx->fs.u64Base), VMX_IGS_FS_BASE_NOT_CANONICAL);
            HMVMX_CHECK_BREAK(X86_IS_CANONICAL(pCtx->gs.u64Base), VMX_IGS_GS_BASE_NOT_CANONICAL);
            HMVMX_CHECK_BREAK(   (pCtx->ldtr.Attr.u & X86DESCATTR_UNUSABLE)
                              || X86_IS_CANONICAL(pCtx->ldtr.u64Base), VMX_IGS_LDTR_BASE_NOT_CANONICAL);
            HMVMX_CHECK_BREAK(!RT_HI_U32(pCtx->cs.u64Base), VMX_IGS_LONGMODE_CS_BASE_INVALID);
            HMVMX_CHECK_BREAK((pCtx->ss.Attr.u & X86DESCATTR_UNUSABLE) || !RT_HI_U32(pCtx->ss.u64Base),
                              VMX_IGS_LONGMODE_SS_BASE_INVALID);
            HMVMX_CHECK_BREAK((pCtx->ds.Attr.u & X86DESCATTR_UNUSABLE) || !RT_HI_U32(pCtx->ds.u64Base),
                              VMX_IGS_LONGMODE_DS_BASE_INVALID);
            HMVMX_CHECK_BREAK((pCtx->es.Attr.u & X86DESCATTR_UNUSABLE) || !RT_HI_U32(pCtx->es.u64Base),
                              VMX_IGS_LONGMODE_ES_BASE_INVALID);
#endif
        }
        else
        {
            /* V86 mode checks. */
            uint32_t u32CSAttr, u32SSAttr, u32DSAttr, u32ESAttr, u32FSAttr, u32GSAttr;
            if (pVmcsInfo->RealMode.fRealOnV86Active)
            {
                u32CSAttr = 0xf3;   u32SSAttr = 0xf3;
                u32DSAttr = 0xf3;   u32ESAttr = 0xf3;
                u32FSAttr = 0xf3;   u32GSAttr = 0xf3;
            }
            else
            {
                u32CSAttr = pCtx->cs.Attr.u;   u32SSAttr = pCtx->ss.Attr.u;
                u32DSAttr = pCtx->ds.Attr.u;   u32ESAttr = pCtx->es.Attr.u;
                u32FSAttr = pCtx->fs.Attr.u;   u32GSAttr = pCtx->gs.Attr.u;
            }

            /* CS */
            HMVMX_CHECK_BREAK((pCtx->cs.u64Base == (uint64_t)pCtx->cs.Sel << 4), VMX_IGS_V86_CS_BASE_INVALID);
            HMVMX_CHECK_BREAK(pCtx->cs.u32Limit == 0xffff, VMX_IGS_V86_CS_LIMIT_INVALID);
            HMVMX_CHECK_BREAK(u32CSAttr == 0xf3, VMX_IGS_V86_CS_ATTR_INVALID);
            /* SS */
            HMVMX_CHECK_BREAK((pCtx->ss.u64Base == (uint64_t)pCtx->ss.Sel << 4), VMX_IGS_V86_SS_BASE_INVALID);
            HMVMX_CHECK_BREAK(pCtx->ss.u32Limit == 0xffff, VMX_IGS_V86_SS_LIMIT_INVALID);
            HMVMX_CHECK_BREAK(u32SSAttr == 0xf3, VMX_IGS_V86_SS_ATTR_INVALID);
            /* DS */
            HMVMX_CHECK_BREAK((pCtx->ds.u64Base == (uint64_t)pCtx->ds.Sel << 4), VMX_IGS_V86_DS_BASE_INVALID);
            HMVMX_CHECK_BREAK(pCtx->ds.u32Limit == 0xffff, VMX_IGS_V86_DS_LIMIT_INVALID);
            HMVMX_CHECK_BREAK(u32DSAttr == 0xf3, VMX_IGS_V86_DS_ATTR_INVALID);
            /* ES */
            HMVMX_CHECK_BREAK((pCtx->es.u64Base == (uint64_t)pCtx->es.Sel << 4), VMX_IGS_V86_ES_BASE_INVALID);
            HMVMX_CHECK_BREAK(pCtx->es.u32Limit == 0xffff, VMX_IGS_V86_ES_LIMIT_INVALID);
            HMVMX_CHECK_BREAK(u32ESAttr == 0xf3, VMX_IGS_V86_ES_ATTR_INVALID);
            /* FS */
            HMVMX_CHECK_BREAK((pCtx->fs.u64Base == (uint64_t)pCtx->fs.Sel << 4), VMX_IGS_V86_FS_BASE_INVALID);
            HMVMX_CHECK_BREAK(pCtx->fs.u32Limit == 0xffff, VMX_IGS_V86_FS_LIMIT_INVALID);
            HMVMX_CHECK_BREAK(u32FSAttr == 0xf3, VMX_IGS_V86_FS_ATTR_INVALID);
            /* GS */
            HMVMX_CHECK_BREAK((pCtx->gs.u64Base == (uint64_t)pCtx->gs.Sel << 4), VMX_IGS_V86_GS_BASE_INVALID);
            HMVMX_CHECK_BREAK(pCtx->gs.u32Limit == 0xffff, VMX_IGS_V86_GS_LIMIT_INVALID);
            HMVMX_CHECK_BREAK(u32GSAttr == 0xf3, VMX_IGS_V86_GS_ATTR_INVALID);
            /* 64-bit capable CPUs. */
#if HC_ARCH_BITS == 64
            HMVMX_CHECK_BREAK(X86_IS_CANONICAL(pCtx->fs.u64Base), VMX_IGS_FS_BASE_NOT_CANONICAL);
            HMVMX_CHECK_BREAK(X86_IS_CANONICAL(pCtx->gs.u64Base), VMX_IGS_GS_BASE_NOT_CANONICAL);
            HMVMX_CHECK_BREAK(   (pCtx->ldtr.Attr.u & X86DESCATTR_UNUSABLE)
                              || X86_IS_CANONICAL(pCtx->ldtr.u64Base), VMX_IGS_LDTR_BASE_NOT_CANONICAL);
            HMVMX_CHECK_BREAK(!RT_HI_U32(pCtx->cs.u64Base), VMX_IGS_LONGMODE_CS_BASE_INVALID);
            HMVMX_CHECK_BREAK((pCtx->ss.Attr.u & X86DESCATTR_UNUSABLE) || !RT_HI_U32(pCtx->ss.u64Base),
                              VMX_IGS_LONGMODE_SS_BASE_INVALID);
            HMVMX_CHECK_BREAK((pCtx->ds.Attr.u & X86DESCATTR_UNUSABLE) || !RT_HI_U32(pCtx->ds.u64Base),
                              VMX_IGS_LONGMODE_DS_BASE_INVALID);
            HMVMX_CHECK_BREAK((pCtx->es.Attr.u & X86DESCATTR_UNUSABLE) || !RT_HI_U32(pCtx->es.u64Base),
                              VMX_IGS_LONGMODE_ES_BASE_INVALID);
#endif
        }

        /*
         * TR.
         */
        HMVMX_CHECK_BREAK(!(pCtx->tr.Sel & X86_SEL_LDT), VMX_IGS_TR_TI_INVALID);
        /* 64-bit capable CPUs. */
#if HC_ARCH_BITS == 64
        HMVMX_CHECK_BREAK(X86_IS_CANONICAL(pCtx->tr.u64Base), VMX_IGS_TR_BASE_NOT_CANONICAL);
#endif
        if (fLongModeGuest)
        {
            HMVMX_CHECK_BREAK(pCtx->tr.Attr.n.u4Type == 11,           /* 64-bit busy TSS. */
                              VMX_IGS_LONGMODE_TR_ATTR_TYPE_INVALID);
        }
        else
        {
            HMVMX_CHECK_BREAK(   pCtx->tr.Attr.n.u4Type == 3          /* 16-bit busy TSS. */
                              || pCtx->tr.Attr.n.u4Type == 11,        /* 32-bit busy TSS.*/
                              VMX_IGS_TR_ATTR_TYPE_INVALID);
        }
        HMVMX_CHECK_BREAK(!pCtx->tr.Attr.n.u1DescType, VMX_IGS_TR_ATTR_S_INVALID);
        HMVMX_CHECK_BREAK(pCtx->tr.Attr.n.u1Present, VMX_IGS_TR_ATTR_P_INVALID);
        HMVMX_CHECK_BREAK(!(pCtx->tr.Attr.u & 0xf00), VMX_IGS_TR_ATTR_RESERVED);   /* Bits 11:8 MBZ. */
        HMVMX_CHECK_BREAK(   (pCtx->tr.u32Limit & 0xfff) == 0xfff
                          || !(pCtx->tr.Attr.n.u1Granularity), VMX_IGS_TR_ATTR_G_INVALID);
        HMVMX_CHECK_BREAK(   !(pCtx->tr.u32Limit & 0xfff00000)
                          || (pCtx->tr.Attr.n.u1Granularity), VMX_IGS_TR_ATTR_G_INVALID);
        HMVMX_CHECK_BREAK(!(pCtx->tr.Attr.u & X86DESCATTR_UNUSABLE), VMX_IGS_TR_ATTR_UNUSABLE);

        /*
         * GDTR and IDTR.
         */
#if HC_ARCH_BITS == 64
        rc = VMXReadVmcs64(VMX_VMCS_GUEST_GDTR_BASE, &u64Val);
        AssertRCBreak(rc);
        HMVMX_CHECK_BREAK(X86_IS_CANONICAL(u64Val), VMX_IGS_GDTR_BASE_NOT_CANONICAL);

        rc = VMXReadVmcs64(VMX_VMCS_GUEST_IDTR_BASE, &u64Val);
        AssertRCBreak(rc);
        HMVMX_CHECK_BREAK(X86_IS_CANONICAL(u64Val), VMX_IGS_IDTR_BASE_NOT_CANONICAL);
#endif

        rc = VMXReadVmcs32(VMX_VMCS32_GUEST_GDTR_LIMIT, &u32Val);
        AssertRCBreak(rc);
        HMVMX_CHECK_BREAK(!(u32Val & 0xffff0000), VMX_IGS_GDTR_LIMIT_INVALID);      /* Bits 31:16 MBZ. */

        rc = VMXReadVmcs32(VMX_VMCS32_GUEST_IDTR_LIMIT, &u32Val);
        AssertRCBreak(rc);
        HMVMX_CHECK_BREAK(!(u32Val & 0xffff0000), VMX_IGS_IDTR_LIMIT_INVALID);      /* Bits 31:16 MBZ. */

        /*
         * Guest Non-Register State.
         */
        /* Activity State. */
        uint32_t u32ActivityState;
        rc = VMXReadVmcs32(VMX_VMCS32_GUEST_ACTIVITY_STATE, &u32ActivityState);
        AssertRCBreak(rc);
        HMVMX_CHECK_BREAK(   !u32ActivityState
                          || (u32ActivityState & RT_BF_GET(pVM->hm.s.vmx.Msrs.u64Misc, VMX_BF_MISC_ACTIVITY_STATES)),
                             VMX_IGS_ACTIVITY_STATE_INVALID);
        HMVMX_CHECK_BREAK(   !(pCtx->ss.Attr.n.u2Dpl)
                          || u32ActivityState != VMX_VMCS_GUEST_ACTIVITY_HLT, VMX_IGS_ACTIVITY_STATE_HLT_INVALID);
        uint32_t u32IntrState;
        rc = VMXReadVmcs32(VMX_VMCS32_GUEST_INT_STATE, &u32IntrState);
        AssertRCBreak(rc);
        if (   u32IntrState == VMX_VMCS_GUEST_INT_STATE_BLOCK_MOVSS
            || u32IntrState == VMX_VMCS_GUEST_INT_STATE_BLOCK_STI)
        {
            HMVMX_CHECK_BREAK(u32ActivityState == VMX_VMCS_GUEST_ACTIVITY_ACTIVE, VMX_IGS_ACTIVITY_STATE_ACTIVE_INVALID);
        }

        /** @todo Activity state and injecting interrupts. Left as a todo since we
         *        currently don't use activity states but ACTIVE. */

        HMVMX_CHECK_BREAK(   !(pVmcsInfo->u32EntryCtls & VMX_ENTRY_CTLS_ENTRY_TO_SMM)
                          || u32ActivityState != VMX_VMCS_GUEST_ACTIVITY_SIPI_WAIT, VMX_IGS_ACTIVITY_STATE_SIPI_WAIT_INVALID);

        /* Guest interruptibility-state. */
        HMVMX_CHECK_BREAK(!(u32IntrState & 0xffffffe0), VMX_IGS_INTERRUPTIBILITY_STATE_RESERVED);
        HMVMX_CHECK_BREAK((u32IntrState & (VMX_VMCS_GUEST_INT_STATE_BLOCK_STI | VMX_VMCS_GUEST_INT_STATE_BLOCK_MOVSS))
                                       != (VMX_VMCS_GUEST_INT_STATE_BLOCK_STI | VMX_VMCS_GUEST_INT_STATE_BLOCK_MOVSS),
                          VMX_IGS_INTERRUPTIBILITY_STATE_STI_MOVSS_INVALID);
        HMVMX_CHECK_BREAK(   (u32Eflags & X86_EFL_IF)
                          || !(u32IntrState & VMX_VMCS_GUEST_INT_STATE_BLOCK_STI),
                          VMX_IGS_INTERRUPTIBILITY_STATE_STI_EFL_INVALID);
        if (VMX_ENTRY_INT_INFO_IS_VALID(u32EntryInfo))
        {
            if (VMX_ENTRY_INT_INFO_TYPE(u32EntryInfo) == VMX_EXIT_INT_INFO_TYPE_EXT_INT)
            {
                HMVMX_CHECK_BREAK(   !(u32IntrState & VMX_VMCS_GUEST_INT_STATE_BLOCK_STI)
                                  && !(u32IntrState & VMX_VMCS_GUEST_INT_STATE_BLOCK_MOVSS),
                                  VMX_IGS_INTERRUPTIBILITY_STATE_EXT_INT_INVALID);
            }
            else if (VMX_ENTRY_INT_INFO_TYPE(u32EntryInfo) == VMX_EXIT_INT_INFO_TYPE_NMI)
            {
                HMVMX_CHECK_BREAK(!(u32IntrState & VMX_VMCS_GUEST_INT_STATE_BLOCK_MOVSS),
                                  VMX_IGS_INTERRUPTIBILITY_STATE_MOVSS_INVALID);
                HMVMX_CHECK_BREAK(!(u32IntrState & VMX_VMCS_GUEST_INT_STATE_BLOCK_STI),
                                  VMX_IGS_INTERRUPTIBILITY_STATE_STI_INVALID);
            }
        }
        /** @todo Assumes the processor is not in SMM. */
        HMVMX_CHECK_BREAK(!(u32IntrState & VMX_VMCS_GUEST_INT_STATE_BLOCK_SMI),
                          VMX_IGS_INTERRUPTIBILITY_STATE_SMI_INVALID);
        HMVMX_CHECK_BREAK(   !(pVmcsInfo->u32EntryCtls & VMX_ENTRY_CTLS_ENTRY_TO_SMM)
                          || (u32IntrState & VMX_VMCS_GUEST_INT_STATE_BLOCK_SMI),
                             VMX_IGS_INTERRUPTIBILITY_STATE_SMI_SMM_INVALID);
        if (   (pVmcsInfo->u32PinCtls & VMX_PIN_CTLS_VIRT_NMI)
            && VMX_ENTRY_INT_INFO_IS_VALID(u32EntryInfo)
            && VMX_ENTRY_INT_INFO_TYPE(u32EntryInfo) == VMX_EXIT_INT_INFO_TYPE_NMI)
        {
            HMVMX_CHECK_BREAK(!(u32IntrState & VMX_VMCS_GUEST_INT_STATE_BLOCK_NMI),
                              VMX_IGS_INTERRUPTIBILITY_STATE_NMI_INVALID);
        }

        /* Pending debug exceptions. */
#if HC_ARCH_BITS == 64
        rc = VMXReadVmcs64(VMX_VMCS_GUEST_PENDING_DEBUG_XCPTS, &u64Val);
        AssertRCBreak(rc);
        /* Bits 63:15, Bit 13, Bits 11:4 MBZ. */
        HMVMX_CHECK_BREAK(!(u64Val & UINT64_C(0xffffffffffffaff0)), VMX_IGS_LONGMODE_PENDING_DEBUG_RESERVED);
        u32Val = u64Val;    /* For pending debug exceptions checks below. */
#else
        rc = VMXReadVmcs32(VMX_VMCS_GUEST_PENDING_DEBUG_XCPTS, &u32Val);
        AssertRCBreak(rc);
        /* Bits 31:15, Bit 13, Bits 11:4 MBZ. */
        HMVMX_CHECK_BREAK(!(u32Val & 0xffffaff0), VMX_IGS_PENDING_DEBUG_RESERVED);
#endif

        if (   (u32IntrState & VMX_VMCS_GUEST_INT_STATE_BLOCK_STI)
            || (u32IntrState & VMX_VMCS_GUEST_INT_STATE_BLOCK_MOVSS)
            || u32ActivityState == VMX_VMCS_GUEST_ACTIVITY_HLT)
        {
            if (   (u32Eflags & X86_EFL_TF)
                && !(u64DebugCtlMsr & RT_BIT_64(1)))    /* Bit 1 is IA32_DEBUGCTL.BTF. */
            {
                /* Bit 14 is PendingDebug.BS. */
                HMVMX_CHECK_BREAK(u32Val & RT_BIT(14), VMX_IGS_PENDING_DEBUG_XCPT_BS_NOT_SET);
            }
            if (   !(u32Eflags & X86_EFL_TF)
                || (u64DebugCtlMsr & RT_BIT_64(1)))     /* Bit 1 is IA32_DEBUGCTL.BTF. */
            {
                /* Bit 14 is PendingDebug.BS. */
                HMVMX_CHECK_BREAK(!(u32Val & RT_BIT(14)), VMX_IGS_PENDING_DEBUG_XCPT_BS_NOT_CLEAR);
            }
        }

        /* VMCS link pointer. */
        rc = VMXReadVmcs64(VMX_VMCS64_GUEST_VMCS_LINK_PTR_FULL, &u64Val);
        AssertRCBreak(rc);
        if (u64Val != UINT64_C(0xffffffffffffffff))
        {
            HMVMX_CHECK_BREAK(!(u64Val & 0xfff), VMX_IGS_VMCS_LINK_PTR_RESERVED);
            /** @todo Bits beyond the processor's physical-address width MBZ. */
            /** @todo 32-bit located in memory referenced by value of this field (as a
             *        physical address) must contain the processor's VMCS revision ID. */
            /** @todo SMM checks. */
        }

        /** @todo Checks on Guest Page-Directory-Pointer-Table Entries when guest is
         *        not using nested paging? */
        if (   pVM->hm.s.fNestedPaging
            && !fLongModeGuest
            && CPUMIsGuestInPAEModeEx(pCtx))
        {
            rc = VMXReadVmcs64(VMX_VMCS64_GUEST_PDPTE0_FULL, &u64Val);
            AssertRCBreak(rc);
            HMVMX_CHECK_BREAK(!(u64Val & X86_PDPE_PAE_MBZ_MASK), VMX_IGS_PAE_PDPTE_RESERVED);

            rc = VMXReadVmcs64(VMX_VMCS64_GUEST_PDPTE1_FULL, &u64Val);
            AssertRCBreak(rc);
            HMVMX_CHECK_BREAK(!(u64Val & X86_PDPE_PAE_MBZ_MASK), VMX_IGS_PAE_PDPTE_RESERVED);

            rc = VMXReadVmcs64(VMX_VMCS64_GUEST_PDPTE2_FULL, &u64Val);
            AssertRCBreak(rc);
            HMVMX_CHECK_BREAK(!(u64Val & X86_PDPE_PAE_MBZ_MASK), VMX_IGS_PAE_PDPTE_RESERVED);

            rc = VMXReadVmcs64(VMX_VMCS64_GUEST_PDPTE3_FULL, &u64Val);
            AssertRCBreak(rc);
            HMVMX_CHECK_BREAK(!(u64Val & X86_PDPE_PAE_MBZ_MASK), VMX_IGS_PAE_PDPTE_RESERVED);
        }

        /* Shouldn't happen but distinguish it from AssertRCBreak() errors. */
        if (uError == VMX_IGS_ERROR)
            uError = VMX_IGS_REASON_NOT_FOUND;
    } while (0);

    pVCpu->hm.s.u32HMError = uError;
    return uError;

#undef HMVMX_ERROR_BREAK
#undef HMVMX_CHECK_BREAK
}


/**
 * Setup the APIC-access page for virtualizing APIC access.
 *
 * This can cause a longjumps to R3 due to the acquisition of the PGM lock, hence
 * this not done as part of exporting guest state, see @bugref{8721}.
 *
 * @returns VBox status code.
 * @param   pVCpu           The cross context virtual CPU structure.
 */
static int hmR0VmxMapHCApicAccessPage(PVMCPU pVCpu)
{
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    uint64_t const u64MsrApicBase = APICGetBaseMsrNoCheck(pVCpu);

    Assert(PDMHasApic(pVM));
    Assert(u64MsrApicBase);

    RTGCPHYS const GCPhysApicBase = u64MsrApicBase & PAGE_BASE_GC_MASK;
    Log4Func(("Mappping HC APIC-access page at %#RGp\n", GCPhysApicBase));

    /* Unalias any existing mapping. */
    int rc = PGMHandlerPhysicalReset(pVM, GCPhysApicBase);
    AssertRCReturn(rc, rc);

    /* Map the HC APIC-access page in place of the MMIO page, also updates the shadow page tables if necessary. */
    Assert(pVM->hm.s.vmx.HCPhysApicAccess != NIL_RTHCPHYS);
    rc = IOMMMIOMapMMIOHCPage(pVM, pVCpu, GCPhysApicBase, pVM->hm.s.vmx.HCPhysApicAccess, X86_PTE_RW | X86_PTE_P);
    AssertRCReturn(rc, rc);

    /* Update the per-VCPU cache of the APIC base MSR. */
    pVCpu->hm.s.vmx.u64GstMsrApicBase = u64MsrApicBase;
    return VINF_SUCCESS;
}


#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
/**
 * Merges the guest with the nested-guest MSR bitmap in preparation of executing the
 * nested-guest using hardware-assisted VMX.
 *
 * @param   pVCpu               The cross context virtual CPU structure.
 * @param   pVmcsInfoNstGst     The nested-guest VMCS info. object.
 * @param   pVmcsInfoGst        The guest VMCS info. object.
 */
static void hmR0VmxMergeMsrBitmapNested(PCVMCPU pVCpu, PVMXVMCSINFO pVmcsInfoNstGst, PCVMXVMCSINFO pVmcsInfoGst)
{
    uint32_t const cbMsrBitmap    = X86_PAGE_4K_SIZE;
    uint64_t       *pu64MsrBitmap = (uint64_t *)pVmcsInfoNstGst->pvMsrBitmap;
    Assert(pu64MsrBitmap);

    /*
     * We merge the guest MSR bitmap with the nested-guest MSR bitmap such that any
     * MSR that is intercepted by the guest is also intercepted while executing the
     * nested-guest using hardware-assisted VMX.
     *
     * Note! If the nested-guest is not using an MSR bitmap, ever MSR must cause a
     *       nested-guest VM-exit even if the outer guest is not intercepting some
     *       MSRs. We cannot assume the caller has initialized the nested-guest
     *       MSR bitmap in this case.
     *
     *       The guest hypervisor may also switch whether it uses MSR bitmaps for
     *       each VM-entry, hence initializing it once per-VM while setting up the
     *       nested-guest VMCS is not sufficient.
     */
    PCVMXVVMCS pVmcsNstGst = pVCpu->cpum.GstCtx.hwvirt.vmx.CTX_SUFF(pVmcs);
    if (pVmcsNstGst->u32ProcCtls & VMX_PROC_CTLS_USE_MSR_BITMAPS)
    {
        uint64_t const *pu64MsrBitmapNstGst = (uint64_t const *)pVCpu->cpum.GstCtx.hwvirt.vmx.CTX_SUFF(pvMsrBitmap);
        uint64_t const *pu64MsrBitmapGst    = (uint64_t const *)pVmcsInfoGst->pvMsrBitmap;
        Assert(pu64MsrBitmapNstGst);
        Assert(pu64MsrBitmapGst);

        uint32_t const cFrags = cbMsrBitmap / sizeof(uint64_t);
        for (uint32_t i = 0; i < cFrags; i++)
            pu64MsrBitmap[i] = pu64MsrBitmapNstGst[i] | pu64MsrBitmapGst[i];
    }
    else
        ASMMemFill32(pu64MsrBitmap, cbMsrBitmap, UINT32_C(0xffffffff));
}


/**
 * Merges the guest VMCS in to the nested-guest VMCS controls in preparation of
 * hardware-assisted VMX execution of the nested-guest.
 *
 * For a guest, we don't modify these controls once we set up the VMCS and hence
 * this function is never called.
 *
 * For nested-guests since the guest hypervisor provides these controls on every
 * nested-guest VM-entry and could potentially change them everytime we need to
 * merge them before every nested-guest VM-entry.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 */
static int hmR0VmxMergeVmcsNested(PVMCPU pVCpu)
{
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    PCVMXVMCSINFO pVmcsInfoGst = &pVCpu->hm.s.vmx.VmcsInfo;
    PCVMXVVMCS    pVmcsNstGst  = pVCpu->cpum.GstCtx.hwvirt.vmx.CTX_SUFF(pVmcs);
    Assert(pVmcsNstGst);

    /*
     * Merge the controls with the requirements of the guest VMCS.
     *
     * We do not need to validate the nested-guest VMX features specified in the
     * nested-guest VMCS with the features supported by the physical CPU as it's
     * already done by the VMLAUNCH/VMRESUME instruction emulation.
     *
     * This is because the VMX features exposed by CPUM (through CPUID/MSRs) to the
     * guest are derived from the VMX features supported by the physical CPU.
     */

    /* Pin-based VM-execution controls. */
    uint32_t const u32PinCtls = pVmcsNstGst->u32PinCtls | pVmcsInfoGst->u32PinCtls;

    /* Processor-based VM-execution controls. */
    uint32_t       u32ProcCtls = (pVmcsNstGst->u32ProcCtls  & ~VMX_PROC_CTLS_USE_IO_BITMAPS)
                               | (pVmcsInfoGst->u32ProcCtls & ~(  VMX_PROC_CTLS_INT_WINDOW_EXIT
                                                                | VMX_PROC_CTLS_NMI_WINDOW_EXIT
                                                                | VMX_PROC_CTLS_USE_TPR_SHADOW
                                                                | VMX_PROC_CTLS_MONITOR_TRAP_FLAG));

    /* Secondary processor-based VM-execution controls. */
    uint32_t const u32ProcCtls2 = (pVmcsNstGst->u32ProcCtls2  & ~VMX_PROC_CTLS2_VPID)
                                | (pVmcsInfoGst->u32ProcCtls2 & ~(  VMX_PROC_CTLS2_VIRT_APIC_ACCESS
                                                                  | VMX_PROC_CTLS2_INVPCID
                                                                  | VMX_PROC_CTLS2_RDTSCP
                                                                  | VMX_PROC_CTLS2_XSAVES_XRSTORS
                                                                  | VMX_PROC_CTLS2_APIC_REG_VIRT
                                                                  | VMX_PROC_CTLS2_VIRT_INT_DELIVERY
                                                                  | VMX_PROC_CTLS2_VMFUNC));

    /*
     * VM-entry controls:
     * These controls contains state that depends on the nested-guest state (primarily
     * EFER MSR) and is thus not constant between VMLAUNCH/VMRESUME and the nested-guest
     * VM-exit. Although the guest hypervisor cannot change it, we need to in order to
     * properly continue executing the nested-guest if the EFER MSR changes but does not
     * cause a nested-guest VM-exits.
     *
     * VM-exit controls:
     * These controls specify the host state on return. We cannot use the controls from
     * the guest hypervisor state as is as it would contain the guest state rather than
     * the host state. Since the host state is subject to change (e.g. preemption, trips
     * to ring-3, longjmp and rescheduling to a different host CPU) they are not constant
     * through VMLAUNCH/VMRESUME and the nested-guest VM-exit.
     *
     * VM-entry MSR-load:
     * The guest MSRs from the VM-entry MSR-load area are already loaded into the
     * guest-CPU context by the VMLAUNCH/VMRESUME instruction emulation.
     *
     * VM-exit MSR-store:
     * The VM-exit emulation will take care of populating the MSRs from the guest-CPU
     * context back into the VM-exit MSR-store area.
     *
     * VM-exit MSR-load areas:
     * This must contain the real host MSRs with hardware-assisted VMX execution. Hence,
     * we can entirely ignore what the guest hypervisor wants to load here.
     */

    /*
     * Exception bitmap.
     *
     * We could remove #UD from the guest bitmap and merge it with the nested-guest
     * bitmap here (and avoid doing anything while exporting nested-guest state), but to
     * keep the code more flexible if intercepting exceptions become more dynamic in
     * the future we do it as part of exporting the nested-guest state.
     */
    uint32_t const u32XcptBitmap = pVmcsNstGst->u32XcptBitmap | pVmcsInfoGst->u32XcptBitmap;

    /*
     * CR0/CR4 guest/host mask.
     *
     * Modifications by the nested-guest to CR0/CR4 bits owned by the host and the guest
     * must cause VM-exits, so we need to merge them here.
     */
    uint64_t const u64Cr0Mask = pVmcsNstGst->u64Cr0Mask.u | pVmcsInfoGst->u64Cr0Mask;
    uint64_t const u64Cr4Mask = pVmcsNstGst->u64Cr4Mask.u | pVmcsInfoGst->u64Cr4Mask;

    /*
     * Page-fault error-code mask and match.
     *
     * Although we require unrestricted guest execution (and thereby nested-paging) for
     * hardware-assisted VMX execution of nested-guests and thus the outer guest doesn't
     * normally intercept #PFs, it might intercept them for debugging purposes.
     *
     * If the outer guest is not intercepting #PFs, we can use the nested-guest #PF
     * filters. If the outer guest is intercepting #PFs we must intercept all #PFs.
     */
    uint32_t u32XcptPFMask;
    uint32_t u32XcptPFMatch;
    if (!(pVmcsInfoGst->u32XcptBitmap & RT_BIT(X86_XCPT_PF)))
    {
        u32XcptPFMask  = pVmcsNstGst->u32XcptPFMask;
        u32XcptPFMatch = pVmcsNstGst->u32XcptPFMatch;
    }
    else
    {
        u32XcptPFMask  = 0;
        u32XcptPFMatch = 0;
    }

    /*
     * Pause-Loop exiting.
     */
    uint32_t const cPleGapTicks    = RT_MIN(pVM->hm.s.vmx.cPleGapTicks,    pVmcsNstGst->u32PleGap);
    uint32_t const cPleWindowTicks = RT_MIN(pVM->hm.s.vmx.cPleWindowTicks, pVmcsNstGst->u32PleWindow);

    /*
     * I/O Bitmap.
     *
     * We do not use the I/O bitmap that may be provided by the guest hypervisor as we
     * always intercept all I/O port accesses.
     */
    Assert(u32ProcCtls & VMX_PROC_CTLS_UNCOND_IO_EXIT);

    /*
     * APIC-access page.
     *
     * The APIC-access page address has already been initialized while setting up the
     * nested-guest VMCS. In theory, even if the guest-physical address is invalid, it
     * should not be on any consequence to the host or to the guest for that matter, but
     * we only accept valid addresses verified by the VMLAUNCH/VMRESUME instruction
     * emulation to keep it simple.
     */

    /*
     * Virtual-APIC page and TPR threshold.
     *
     * We shall use the host-physical address of the virtual-APIC page in guest memory directly.
     * For this reason, we can access the virtual-APIC page of the nested-guest only using
     * PGM physical handlers as we must not assume a kernel virtual-address mapping exists and
     * requesting PGM for a mapping could be expensive/resource intensive (PGM mapping cache).
     */
    RTHCPHYS       HCPhysVirtApic  = NIL_RTHCPHYS;
    uint32_t const u32TprThreshold = pVmcsNstGst->u32TprThreshold;
    if (u32ProcCtls & VMX_PROC_CTLS_USE_TPR_SHADOW)
    {
        int rc = PGMPhysGCPhys2HCPhys(pVM, pVmcsNstGst->u64AddrVirtApic.u, &HCPhysVirtApic);

        /*
         * If the guest hypervisor has loaded crap into the virtual-APIC page field
         * we would fail to obtain a valid host-physical address for its guest-physical
         * address.
         *
         * We currently do not support this scenario. Maybe in the future if there is a
         * pressing need we can explore making this particular set of conditions work.
         * Right now we just cause a VM-entry failure.
         *
         * This has already been checked by VMLAUNCH/VMRESUME instruction emulation,
         * so should not really failure at the moment.
         */
        AssertLogRelMsgRCReturn(rc, ("rc=%Rrc\n", rc), rc);
    }
    else
    {
        /*
         * We must make sure CR8 reads/write must cause VM-exits when TPR shadowing is not
         * used by the guest hypervisor. Preventing MMIO accesses to the physical APIC will
         * be taken care of by EPT/shadow paging.
         */
        if (pVM->hm.s.fAllow64BitGuests)
        {
            u32ProcCtls |= VMX_PROC_CTLS_CR8_STORE_EXIT
                        |  VMX_PROC_CTLS_CR8_LOAD_EXIT;
        }
    }

    /*
     * Validate basic assumptions.
     */
    PVMXVMCSINFO pVmcsInfoNstGst = &pVCpu->hm.s.vmx.VmcsInfoNstGst;
    Assert(pVM->hm.s.vmx.fAllowUnrestricted);
    Assert(pVM->hm.s.vmx.Msrs.ProcCtls.n.allowed1 & VMX_PROC_CTLS_USE_SECONDARY_CTLS);
    Assert(hmGetVmxActiveVmcsInfo(pVCpu) == pVmcsInfoNstGst);

    /*
     * Commit it to the nested-guest VMCS.
     */
    int rc = VINF_SUCCESS;
    if (pVmcsInfoNstGst->u32PinCtls != u32PinCtls)
        rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_PIN_EXEC, u32PinCtls);
    if (pVmcsInfoNstGst->u32ProcCtls != u32ProcCtls)
        rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC, u32ProcCtls);
    if (pVmcsInfoNstGst->u32ProcCtls2 != u32ProcCtls2)
        rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC2, u32ProcCtls2);
    if (pVmcsInfoNstGst->u32XcptBitmap != u32XcptBitmap)
        rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_EXCEPTION_BITMAP, u32XcptBitmap);
    if (pVmcsInfoNstGst->u64Cr0Mask != u64Cr0Mask)
        rc |= VMXWriteVmcsHstN(VMX_VMCS_CTRL_CR0_MASK, u64Cr0Mask);
    if (pVmcsInfoNstGst->u64Cr4Mask != u64Cr4Mask)
        rc |= VMXWriteVmcsHstN(VMX_VMCS_CTRL_CR4_MASK, u64Cr4Mask);
    if (pVmcsInfoNstGst->u32XcptPFMask != u32XcptPFMask)
        rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_PAGEFAULT_ERROR_MASK, u32XcptPFMask);
    if (pVmcsInfoNstGst->u32XcptPFMatch != u32XcptPFMatch)
        rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_PAGEFAULT_ERROR_MATCH, u32XcptPFMatch);
    if (   !(u32ProcCtls  & VMX_PROC_CTLS_PAUSE_EXIT)
        &&  (u32ProcCtls2 & VMX_PROC_CTLS2_PAUSE_LOOP_EXIT))
    {
        Assert(pVM->hm.s.vmx.Msrs.ProcCtls2.n.allowed1 & VMX_PROC_CTLS2_PAUSE_LOOP_EXIT);
        rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_PLE_GAP, cPleGapTicks);
        rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_PLE_WINDOW, cPleWindowTicks);
    }
    if (u32ProcCtls & VMX_PROC_CTLS_USE_TPR_SHADOW)
    {
        rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_TPR_THRESHOLD, u32TprThreshold);
        rc |= VMXWriteVmcs64(VMX_VMCS64_CTRL_VIRT_APIC_PAGEADDR_FULL, HCPhysVirtApic);
    }
    AssertRCReturn(rc, rc);

    /*
     * Update the nested-guest VMCS cache.
     */
    pVmcsInfoNstGst->u32PinCtls     = u32PinCtls;
    pVmcsInfoNstGst->u32ProcCtls    = u32ProcCtls;
    pVmcsInfoNstGst->u32ProcCtls2   = u32ProcCtls2;
    pVmcsInfoNstGst->u32XcptBitmap  = u32XcptBitmap;
    pVmcsInfoNstGst->u64Cr0Mask     = u64Cr0Mask;
    pVmcsInfoNstGst->u64Cr4Mask     = u64Cr4Mask;
    pVmcsInfoNstGst->u32XcptPFMask  = u32XcptPFMask;
    pVmcsInfoNstGst->u32XcptPFMatch = u32XcptPFMatch;
    pVmcsInfoNstGst->HCPhysVirtApic = HCPhysVirtApic;

    /*
     * MSR bitmap.
     *
     * The MSR bitmap address has already been initialized while setting up the
     * nested-guest VMCS, here we need to merge the MSR bitmaps.
     */
    if (u32ProcCtls & VMX_PROC_CTLS_USE_MSR_BITMAPS)
        hmR0VmxMergeMsrBitmapNested(pVCpu, pVmcsInfoNstGst, pVmcsInfoGst);

    return VINF_SUCCESS;
}
#endif /* VBOX_WITH_NESTED_HWVIRT_VMX */


/**
 * Does the preparations before executing guest code in VT-x.
 *
 * This may cause longjmps to ring-3 and may even result in rescheduling to the
 * recompiler/IEM. We must be cautious what we do here regarding committing
 * guest-state information into the VMCS assuming we assuredly execute the
 * guest in VT-x mode.
 *
 * If we fall back to the recompiler/IEM after updating the VMCS and clearing
 * the common-state (TRPM/forceflags), we must undo those changes so that the
 * recompiler/IEM can (and should) use them when it resumes guest execution.
 * Otherwise such operations must be done when we can no longer exit to ring-3.
 *
 * @returns Strict VBox status code (i.e. informational status codes too).
 * @retval  VINF_SUCCESS if we can proceed with running the guest, interrupts
 *          have been disabled.
 * @retval  VINF_VMX_VMEXIT if a nested-guest VM-exit occurs (e.g., while evaluating
 *          pending events).
 * @retval  VINF_EM_RESET if a triple-fault occurs while injecting a
 *          double-fault into the guest.
 * @retval  VINF_EM_DBG_STEPPED if @a fStepping is true and an event was
 *          dispatched directly.
 * @retval  VINF_* scheduling changes, we have to go back to ring-3.
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 * @param   fStepping       Whether we are single-stepping the guest in the
 *                          hypervisor debugger. Makes us ignore some of the reasons
 *                          for returning to ring-3, and return VINF_EM_DBG_STEPPED
 *                          if event dispatching took place.
 */
static VBOXSTRICTRC hmR0VmxPreRunGuest(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient, bool fStepping)
{
    Assert(VMMRZCallRing3IsEnabled(pVCpu));

#ifdef VBOX_WITH_NESTED_HWVIRT_ONLY_IN_IEM
    if (pVmxTransient->fIsNestedGuest)
    {
        RT_NOREF2(pVCpu, fStepping);
        Log2Func(("Rescheduling to IEM due to nested-hwvirt or forced IEM exec -> VINF_EM_RESCHEDULE_REM\n"));
        return VINF_EM_RESCHEDULE_REM;
    }
#endif

#ifdef VBOX_WITH_2X_4GB_ADDR_SPACE_IN_R0
    PGMRZDynMapFlushAutoSet(pVCpu);
#endif

    /*
     * Check and process force flag actions, some of which might require us to go back to ring-3.
     */
    VBOXSTRICTRC rcStrict = hmR0VmxCheckForceFlags(pVCpu, fStepping);
    if (rcStrict == VINF_SUCCESS)
    { /* FFs don't get set all the time. */ }
    else
        return rcStrict;

#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
    /*
     * Switch to the nested-guest VMCS as we may have transitioned into executing
     * the nested-guest without leaving ring-0. Otherwise, if we came from ring-3
     * we would load the nested-guest VMCS while entering the VMX ring-0 session.
     *
     * We do this as late as possible to minimize (though not completely remove)
     * clearing/loading VMCS again due to premature trips to ring-3 above.
     */
    if (pVmxTransient->fIsNestedGuest)
    {
        if (!pVCpu->hm.s.vmx.fSwitchedToNstGstVmcs)
        {
            /*
             * Ensure we have synced everything from the guest VMCS and also flag that
             * that we need to export the full (nested) guest-CPU context to the
             * nested-guest VMCS.
             */
            HMVMX_CPUMCTX_ASSERT(pVCpu, HMVMX_CPUMCTX_EXTRN_ALL);
            ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_HOST_CONTEXT | HM_CHANGED_ALL_GUEST);

            RTCCUINTREG const fEFlags = ASMIntDisableFlags();
            int rc = hmR0VmxSwitchVmcs(&pVCpu->hm.s.vmx.VmcsInfo, &pVCpu->hm.s.vmx.VmcsInfoNstGst);
            if (RT_LIKELY(rc == VINF_SUCCESS))
            {
                pVCpu->hm.s.vmx.fSwitchedToNstGstVmcs = true;
                ASMSetFlags(fEFlags);
                pVmxTransient->pVmcsInfo = &pVCpu->hm.s.vmx.VmcsInfoNstGst;

                /*
                 * We use a different VM-exit MSR-store area for the nested-guest. Hence,
                 * flag that we need to update the host MSR values there. Even if we decide
                 * in the future to share the VM-exit MSR-store area page with the guest,
                 * if its content differs, we would have to update the host MSRs anyway.
                 */
                pVCpu->hm.s.vmx.fUpdatedHostAutoMsrs = false;
                Assert(!pVmxTransient->fUpdatedTscOffsettingAndPreemptTimer);   /** @todo NSTVMX: Paranoia remove later. */
            }
            else
            {
                ASMSetFlags(fEFlags);
                return rc;
            }
        }

        /*
         * Merge guest VMCS controls with the nested-guest VMCS controls.
         *
         * Even if we have not executed the guest prior to this (e.g. when resuming
         * from a saved state), we should be okay with merging controls as we
         * initialize the guest VMCS controls as part of VM setup phase.
         */
        if (!pVCpu->hm.s.vmx.fMergedNstGstCtls)
        {
            int rc = hmR0VmxMergeVmcsNested(pVCpu);
            AssertRCReturn(rc, rc);
            pVCpu->hm.s.vmx.fMergedNstGstCtls = true;
        }
    }
#endif

    /*
     * Virtualize memory-mapped accesses to the physical APIC (may take locks).
     */
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    if (   !pVCpu->hm.s.vmx.u64GstMsrApicBase
        && hmR0VmxIsProcCtls2Set(pVCpu, pVmxTransient, VMX_PROC_CTLS2_VIRT_APIC_ACCESS)
        && PDMHasApic(pVM))
    {
        int rc = hmR0VmxMapHCApicAccessPage(pVCpu);
        AssertRCReturn(rc, rc);
    }

    /*
     * Evaluate events to be injected into the guest.
     *
     * Events in TRPM can be injected without inspecting the guest state.
     * If any new events (interrupts/NMI) are pending currently, we try to set up the
     * guest to cause a VM-exit the next time they are ready to receive the event.
     */
    if (TRPMHasTrap(pVCpu))
        hmR0VmxTrpmTrapToPendingEvent(pVCpu);

    uint32_t fIntrState;
    rcStrict = hmR0VmxEvaluatePendingEvent(pVCpu, pVmxTransient, &fIntrState);

#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
    /*
     * While evaluating pending events if something failed (unlikely) or if we were
     * preparing to run a nested-guest but performed a nested-guest VM-exit, we should bail.
     */
    if (rcStrict != VINF_SUCCESS)
        return rcStrict;
    if (   pVmxTransient->fIsNestedGuest
        && !CPUMIsGuestInVmxNonRootMode(&pVCpu->cpum.GstCtx))
        return VINF_VMX_VMEXIT;
#else
    Assert(rcStrict == VINF_SUCCESS);
#endif

    /*
     * Event injection may take locks (currently the PGM lock for real-on-v86 case) and thus
     * needs to be done with longjmps or interrupts + preemption enabled. Event injection might
     * also result in triple-faulting the VM.
     *
     * The above does not apply when executing a nested-guest (since unrestricted guest execution
     * is a requirement) regardless doing it avoid duplicating code elsewhere.
     */
    rcStrict = hmR0VmxInjectPendingEvent(pVCpu, pVmxTransient, fIntrState, fStepping);
    if (RT_LIKELY(rcStrict == VINF_SUCCESS))
    { /* likely */ }
    else
    {
        AssertMsg(rcStrict == VINF_EM_RESET || (rcStrict == VINF_EM_DBG_STEPPED && fStepping),
                  ("%Rrc\n", VBOXSTRICTRC_VAL(rcStrict)));
        return rcStrict;
    }

    /*
     * A longjump might result in importing CR3 even for VM-exits that don't necessarily
     * import CR3 themselves. We will need to update them here, as even as late as the above
     * hmR0VmxInjectPendingEvent() call may lazily import guest-CPU state on demand causing
     * the below force flags to be set.
     */
    if (VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_HM_UPDATE_CR3))
    {
        Assert(!(ASMAtomicUoReadU64(&pVCpu->cpum.GstCtx.fExtrn) & CPUMCTX_EXTRN_CR3));
        int rc2 = PGMUpdateCR3(pVCpu, CPUMGetGuestCR3(pVCpu));
        AssertMsgReturn(rc2 == VINF_SUCCESS || rc2 == VINF_PGM_SYNC_CR3,
                        ("%Rrc\n", rc2), RT_FAILURE_NP(rc2) ? rc2 : VERR_IPE_UNEXPECTED_INFO_STATUS);
        Assert(!VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_HM_UPDATE_CR3));
    }
    if (VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_HM_UPDATE_PAE_PDPES))
    {
        PGMGstUpdatePaePdpes(pVCpu, &pVCpu->hm.s.aPdpes[0]);
        Assert(!VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_HM_UPDATE_PAE_PDPES));
    }

#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
    /* Paranoia. */
    Assert(!pVmxTransient->fIsNestedGuest || CPUMIsGuestInVmxNonRootMode(&pVCpu->cpum.GstCtx));
#endif

    /*
     * No longjmps to ring-3 from this point on!!!
     * Asserts() will still longjmp to ring-3 (but won't return), which is intentional, better than a kernel panic.
     * This also disables flushing of the R0-logger instance (if any).
     */
    VMMRZCallRing3Disable(pVCpu);

    /*
     * Export the guest state bits.
     *
     * We cannot perform longjmps while loading the guest state because we do not preserve the
     * host/guest state (although the VMCS will be preserved) across longjmps which can cause
     * CPU migration.
     *
     * If we are injecting events to a real-on-v86 mode guest, we would have updated RIP and some segment
     * registers. Hence, loading of the guest state needs to be done -after- injection of events.
     */
    rcStrict = hmR0VmxExportGuestStateOptimal(pVCpu, pVmxTransient);
    if (RT_LIKELY(rcStrict == VINF_SUCCESS))
    { /* likely */ }
    else
    {
        VMMRZCallRing3Enable(pVCpu);
        return rcStrict;
    }

    /*
     * We disable interrupts so that we don't miss any interrupts that would flag preemption
     * (IPI/timers etc.) when thread-context hooks aren't used and we've been running with
     * preemption disabled for a while.  Since this is purely to aid the
     * RTThreadPreemptIsPending() code, it doesn't matter that it may temporarily reenable and
     * disable interrupt on NT.
     *
     * We need to check for force-flags that could've possible been altered since we last
     * checked them (e.g. by PDMGetInterrupt() leaving the PDM critical section,
     * see @bugref{6398}).
     *
     * We also check a couple of other force-flags as a last opportunity to get the EMT back
     * to ring-3 before executing guest code.
     */
    pVmxTransient->fEFlags = ASMIntDisableFlags();

    if (   (   !VM_FF_IS_ANY_SET(pVM, VM_FF_EMT_RENDEZVOUS | VM_FF_TM_VIRTUAL_SYNC)
            && !VMCPU_FF_IS_ANY_SET(pVCpu, VMCPU_FF_HM_TO_R3_MASK))
        || (   fStepping /* Optimized for the non-stepping case, so a bit of unnecessary work when stepping. */
            && !VMCPU_FF_IS_ANY_SET(pVCpu, VMCPU_FF_HM_TO_R3_MASK & ~(VMCPU_FF_TIMER | VMCPU_FF_PDM_CRITSECT))) )
    {
        if (!RTThreadPreemptIsPending(NIL_RTTHREAD))
        {
            pVCpu->hm.s.Event.fPending = false;

            /*
             * We've injected any pending events. This is really the point of no return (to ring-3).
             *
             * Note! The caller expects to continue with interrupts & longjmps disabled on successful
             * returns from this function, so don't enable them here.
             */
            return VINF_SUCCESS;
        }

        STAM_COUNTER_INC(&pVCpu->hm.s.StatSwitchPendingHostIrq);
        rcStrict = VINF_EM_RAW_INTERRUPT;
    }
    else
    {
        STAM_COUNTER_INC(&pVCpu->hm.s.StatSwitchHmToR3FF);
        rcStrict = VINF_EM_RAW_TO_R3;
    }

    ASMSetFlags(pVmxTransient->fEFlags);
    VMMRZCallRing3Enable(pVCpu);

    return rcStrict;
}


/**
 * Final preparations before executing guest code using hardware-assisted VMX.
 *
 * We can no longer get preempted to a different host CPU and there are no returns
 * to ring-3. We ignore any errors that may happen from this point (e.g. VMWRITE
 * failures), this function is not intended to fail sans unrecoverable hardware
 * errors.
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks Called with preemption disabled.
 * @remarks No-long-jump zone!!!
 */
static void hmR0VmxPreRunGuestCommitted(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    Assert(!VMMRZCallRing3IsEnabled(pVCpu));
    Assert(VMMR0IsLogFlushDisabled(pVCpu));
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));
    Assert(!pVCpu->hm.s.Event.fPending);

    /*
     * Indicate start of guest execution and where poking EMT out of guest-context is recognized.
     */
    VMCPU_ASSERT_STATE(pVCpu, VMCPUSTATE_STARTED_HM);
    VMCPU_SET_STATE(pVCpu, VMCPUSTATE_STARTED_EXEC);

    PVM          pVM       = pVCpu->CTX_SUFF(pVM);
    PVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;

    if (!CPUMIsGuestFPUStateActive(pVCpu))
    {
        STAM_PROFILE_ADV_START(&pVCpu->hm.s.StatLoadGuestFpuState, x);
        if (CPUMR0LoadGuestFPU(pVM, pVCpu) == VINF_CPUM_HOST_CR0_MODIFIED)
            pVCpu->hm.s.fCtxChanged |= HM_CHANGED_HOST_CONTEXT;
        STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatLoadGuestFpuState, x);
        STAM_COUNTER_INC(&pVCpu->hm.s.StatLoadGuestFpu);
    }

    /*
     * Re-save the host state bits as we may've been preempted (only happens when
     * thread-context hooks are used or when the VM start function changes).
     * The 64-on-32 switcher saves the (64-bit) host state into the VMCS and if we
     * changed the switcher back to 32-bit, we *must* save the 32-bit host state here,
     * see @bugref{8432}.
     *
     * This may also happen when switching to/from a nested-guest VMCS without leaving
     * ring-0.
     */
    if (pVCpu->hm.s.fCtxChanged & HM_CHANGED_HOST_CONTEXT)
    {
        int rc = hmR0VmxExportHostState(pVCpu);
        AssertRC(rc);
        STAM_COUNTER_INC(&pVCpu->hm.s.StatSwitchPreemptExportHostState);
    }
    Assert(!(pVCpu->hm.s.fCtxChanged & HM_CHANGED_HOST_CONTEXT));

    /*
     * Export the state shared between host and guest (FPU, debug, lazy MSRs).
     */
    if (pVCpu->hm.s.fCtxChanged & HM_CHANGED_VMX_HOST_GUEST_SHARED_STATE)
        hmR0VmxExportSharedState(pVCpu, pVmxTransient);
    AssertMsg(!pVCpu->hm.s.fCtxChanged, ("fCtxChanged=%#RX64\n", pVCpu->hm.s.fCtxChanged));

    /*
     * Store status of the shared guest/host debug state at the time of VM-entry.
     */
#if HC_ARCH_BITS == 32 && defined(VBOX_WITH_64_BITS_GUESTS)
    if (CPUMIsGuestInLongModeEx(&pVCpu->cpum.GstCtx))
    {
        pVmxTransient->fWasGuestDebugStateActive = CPUMIsGuestDebugStateActivePending(pVCpu);
        pVmxTransient->fWasHyperDebugStateActive = CPUMIsHyperDebugStateActivePending(pVCpu);
    }
    else
#endif
    {
        pVmxTransient->fWasGuestDebugStateActive = CPUMIsGuestDebugStateActive(pVCpu);
        pVmxTransient->fWasHyperDebugStateActive = CPUMIsHyperDebugStateActive(pVCpu);
    }

    /*
     * Always cache the TPR-shadow if the virtual-APIC page exists, thereby skipping
     * more than one conditional check. The post-run side of our code shall determine
     * if it needs to sync. the virtual APIC TPR with the TPR-shadow.
     */
    if (pVmcsInfo->pbVirtApic)
        pVmxTransient->u8GuestTpr = pVmcsInfo->pbVirtApic[XAPIC_OFF_TPR];

    /*
     * Update the host MSRs values in the VM-exit MSR-load area.
     */
    if (!pVCpu->hm.s.vmx.fUpdatedHostAutoMsrs)
    {
        if (pVmcsInfo->cExitMsrLoad > 0)
            hmR0VmxUpdateAutoLoadHostMsrs(pVCpu, pVmcsInfo);
        pVCpu->hm.s.vmx.fUpdatedHostAutoMsrs = true;
    }

    /*
     * Evaluate if we need to intercept guest RDTSC/P accesses. Set up the
     * VMX-preemption timer based on the next virtual sync clock deadline.
     */
    PHMPHYSCPU pHostCpu        = hmR0GetCurrentCpu();
    RTCPUID const idCurrentCpu = pHostCpu->idCpu;
    if (   !pVmxTransient->fUpdatedTscOffsettingAndPreemptTimer
        || idCurrentCpu != pVCpu->hm.s.idLastCpu)
    {
        hmR0VmxUpdateTscOffsettingAndPreemptTimer(pVCpu, pVmxTransient);
        pVmxTransient->fUpdatedTscOffsettingAndPreemptTimer = true;
    }

    ASMAtomicWriteBool(&pVCpu->hm.s.fCheckedTLBFlush, true);    /* Used for TLB flushing, set this across the world switch. */
    hmR0VmxFlushTaggedTlb(pHostCpu, pVCpu, pVmcsInfo);          /* Invalidate the appropriate guest entries from the TLB. */
    Assert(idCurrentCpu == pVCpu->hm.s.idLastCpu);
    pVCpu->hm.s.vmx.LastError.idCurrentCpu = idCurrentCpu;      /* Update the error reporting info. with the current host CPU. */

    STAM_PROFILE_ADV_STOP_START(&pVCpu->hm.s.StatEntry, &pVCpu->hm.s.StatInGC, x);

    TMNotifyStartOfExecution(pVCpu);                            /* Notify TM to resume its clocks when TSC is tied to execution,
                                                                   as we're about to start executing the guest . */

    /*
     * Load the guest TSC_AUX MSR when we are not intercepting RDTSCP.
     *
     * This is done this late as updating the TSC offsetting/preemption timer above
     * figures out if we can skip intercepting RDTSCP by calculating the number of
     * host CPU ticks till the next virtual sync deadline (for the dynamic case).
     */
    if (pVmcsInfo->u32ProcCtls2 & VMX_PROC_CTLS2_RDTSCP)
    {
        if (!(pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_RDTSC_EXIT))
        {
            hmR0VmxImportGuestState(pVCpu, pVmcsInfo, CPUMCTX_EXTRN_TSC_AUX);
            /* NB: Because we call hmR0VmxAddAutoLoadStoreMsr with fUpdateHostMsr=true,
               it's safe even after hmR0VmxUpdateAutoLoadHostMsrs has already been done. */
            int rc = hmR0VmxAddAutoLoadStoreMsr(pVCpu, pVmxTransient, MSR_K8_TSC_AUX, CPUMGetGuestTscAux(pVCpu),
                                                true /* fSetReadWrite */, true /* fUpdateHostMsr */);
            AssertRC(rc);
        }
        else
            hmR0VmxRemoveAutoLoadStoreMsr(pVCpu, pVmxTransient, MSR_K8_TSC_AUX);
    }

#ifdef VBOX_STRICT
    Assert(pVCpu->hm.s.vmx.fUpdatedHostAutoMsrs);
    hmR0VmxCheckAutoLoadStoreMsrs(pVCpu, pVmcsInfo, pVmxTransient->fIsNestedGuest);
    hmR0VmxCheckHostEferMsr(pVCpu, pVmcsInfo);
    AssertRC(hmR0VmxCheckVmcsCtls(pVCpu, pVmcsInfo));
#endif

#ifdef HMVMX_ALWAYS_CHECK_GUEST_STATE
    /** @todo r=ramshankar: We can now probably use iemVmxVmentryCheckGuestState here.
     *        Add a PVMXMSRS parameter to it, so that IEM can look at the host MSRs,
     *        see @bugref{9180#c54}. */
    uint32_t const uInvalidReason = hmR0VmxCheckGuestState(pVCpu, pVmcsInfo);
    if (uInvalidReason != VMX_IGS_REASON_NOT_FOUND)
        Log4(("hmR0VmxCheckGuestState returned %#x\n", uInvalidReason));
#endif
}


/**
 * First C routine invoked after running guest code using hardware-assisted VMX.
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 * @param   rcVMRun         Return code of VMLAUNCH/VMRESUME.
 *
 * @remarks Called with interrupts disabled, and returns with interrupts enabled!
 *
 * @remarks No-long-jump zone!!! This function will however re-enable longjmps
 *          unconditionally when it is safe to do so.
 */
static void hmR0VmxPostRunGuest(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient, int rcVMRun)
{
    uint64_t const uHostTsc = ASMReadTSC();                     /** @todo We can do a lot better here, see @bugref{9180#c38}. */

    ASMAtomicWriteBool(&pVCpu->hm.s.fCheckedTLBFlush, false);   /* See HMInvalidatePageOnAllVCpus(): used for TLB flushing. */
    ASMAtomicIncU32(&pVCpu->hm.s.cWorldSwitchExits);            /* Initialized in vmR3CreateUVM(): used for EMT poking. */
    pVCpu->hm.s.fCtxChanged            = 0;                     /* Exits/longjmps to ring-3 requires saving the guest state. */
    pVmxTransient->fVmcsFieldsRead     = 0;                     /* Transient fields need to be read from the VMCS. */
    pVmxTransient->fVectoringPF        = false;                 /* Vectoring page-fault needs to be determined later. */
    pVmxTransient->fVectoringDoublePF  = false;                 /* Vectoring double page-fault needs to be determined later. */

    PVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    if (!(pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_RDTSC_EXIT))
    {
        uint64_t uGstTsc;
        if (!pVmxTransient->fIsNestedGuest)
            uGstTsc = uHostTsc + pVmcsInfo->u64TscOffset;
        else
        {
            uint64_t const uNstGstTsc = uHostTsc + pVmcsInfo->u64TscOffset;
            uGstTsc = CPUMRemoveNestedGuestTscOffset(pVCpu, uNstGstTsc);
        }
        TMCpuTickSetLastSeen(pVCpu, uGstTsc);                           /* Update TM with the guest TSC. */
    }

    STAM_PROFILE_ADV_STOP_START(&pVCpu->hm.s.StatInGC, &pVCpu->hm.s.StatPreExit, x);
    TMNotifyEndOfExecution(pVCpu);                                      /* Notify TM that the guest is no longer running. */
    VMCPU_SET_STATE(pVCpu, VMCPUSTATE_STARTED_HM);

#if HC_ARCH_BITS == 64
    pVCpu->hm.s.vmx.fRestoreHostFlags |= VMX_RESTORE_HOST_REQUIRED;     /* Some host state messed up by VMX needs restoring. */
#endif
#if HC_ARCH_BITS == 32 && defined(VBOX_ENABLE_64_BITS_GUESTS)
    /* The 64-on-32 switcher maintains VMCS-launch state on its own
       and we need to leave it alone here. */
    if (pVmcsInfo->pfnStartVM != VMXR0SwitcherStartVM64)
        pVmcsInfo->fVmcsState |= VMX_V_VMCS_LAUNCH_STATE_LAUNCHED;      /* Use VMRESUME instead of VMLAUNCH in the next run. */
#else
    pVmcsInfo->fVmcsState |= VMX_V_VMCS_LAUNCH_STATE_LAUNCHED;          /* Use VMRESUME instead of VMLAUNCH in the next run. */
#endif
#ifdef VBOX_STRICT
    hmR0VmxCheckHostEferMsr(pVCpu, pVmcsInfo);                          /* Verify that the host EFER MSR wasn't modified. */
#endif
    Assert(!ASMIntAreEnabled());
    ASMSetFlags(pVmxTransient->fEFlags);                                /* Enable interrupts. */
    Assert(!VMMRZCallRing3IsEnabled(pVCpu));

    /*
     * Save the basic VM-exit reason and check if the VM-entry failed.
     * See Intel spec. 24.9.1 "Basic VM-exit Information".
     */
    uint32_t uExitReason;
    int rc = VMXReadVmcs32(VMX_VMCS32_RO_EXIT_REASON, &uExitReason);
    AssertRC(rc);
    pVmxTransient->uExitReason    = VMX_EXIT_REASON_BASIC(uExitReason);
    pVmxTransient->fVMEntryFailed = VMX_EXIT_REASON_HAS_ENTRY_FAILED(uExitReason);

    /*
     * Check if VMLAUNCH/VMRESUME succeeded.
     * If this failed, we cause a guru meditation and cease further execution.
     *
     * However, if we are executing a nested-guest we might fail if we use the
     * fast path rather than fully emulating VMLAUNCH/VMRESUME instruction in IEM.
     */
    if (RT_LIKELY(rcVMRun == VINF_SUCCESS))
    {
        /*
         * Update the VM-exit history array here even if the VM-entry failed due to:
         *   - Invalid guest state.
         *   - MSR loading.
         *   - Machine-check event.
         *
         * In any of the above cases we will still have a "valid" VM-exit reason
         * despite @a fVMEntryFailed being false.
         *
         * See Intel spec. 26.7 "VM-Entry failures during or after loading guest state".
         *
         * Note! We don't have CS or RIP at this point.  Will probably address that later
         *       by amending the history entry added here.
         */
        EMHistoryAddExit(pVCpu, EMEXIT_MAKE_FT(EMEXIT_F_KIND_VMX, pVmxTransient->uExitReason & EMEXIT_F_TYPE_MASK),
                         UINT64_MAX, uHostTsc);

        if (RT_LIKELY(!pVmxTransient->fVMEntryFailed))
        {
            VMMRZCallRing3Enable(pVCpu);

            Assert(!VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_HM_UPDATE_CR3));
            Assert(!VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_HM_UPDATE_PAE_PDPES));

#if defined(HMVMX_ALWAYS_SYNC_FULL_GUEST_STATE) || defined(HMVMX_ALWAYS_SAVE_FULL_GUEST_STATE)
            rc = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, HMVMX_CPUMCTX_EXTRN_ALL);
            AssertRC(rc);
#elif defined(HMVMX_ALWAYS_SAVE_GUEST_RFLAGS)
            rc = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, HMVMX_CPUMCTX_EXTRN_RFLAGS);
            AssertRC(rc);
#else
            /*
             * Import the guest-interruptibility state always as we need it while evaluating
             * injecting events on re-entry.
             *
             * We don't import CR0 (when unrestricted guest execution is unavailable) despite
             * checking for real-mode while exporting the state because all bits that cause
             * mode changes wrt CR0 are intercepted.
             */
            rc = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, CPUMCTX_EXTRN_HM_VMX_INT_STATE);
            AssertRC(rc);
#endif

            /*
             * Sync the TPR shadow with our APIC state.
             */
            if (   !pVmxTransient->fIsNestedGuest
                && (pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_USE_TPR_SHADOW))
            {
                Assert(pVmcsInfo->pbVirtApic);
                if (pVmxTransient->u8GuestTpr != pVmcsInfo->pbVirtApic[XAPIC_OFF_TPR])
                {
                    rc = APICSetTpr(pVCpu, pVmcsInfo->pbVirtApic[XAPIC_OFF_TPR]);
                    AssertRC(rc);
                    ASMAtomicOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_APIC_TPR);
                }
            }

            Assert(VMMRZCallRing3IsEnabled(pVCpu));
            return;
        }
    }
#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
    else if (pVmxTransient->fIsNestedGuest)
    {
# if 0
        /*
         * Copy the VM-instruction error field to the guest VMCS.
         */
        /** @todo NSTVMX: Verify we're using the fast path. */
        uint32_t u32RoVmInstrError;
        rc = VMXReadVmcs32(VMX_VMCS32_RO_VM_INSTR_ERROR, &u32RoVmInstrError);
        AssertRCReturn(rc, rc);
        PVMXVVMCS pGstVmcs = pVCpu->cpum.GstCtx.hwvirt.vmx.CTX_SUFF(pVmcs);
        pGstVmcs->u32RoVmInstrError = u32RoVmInstrError;
        /** @todo NSTVMX: Advance guest RIP and other fast path related restoration.  */
# else
        AssertMsgFailed(("VMLAUNCH/VMRESUME failed but shouldn't happen when VMLAUNCH/VMRESUME was emulated in IEM!\n"));
# endif
    }
#endif
    else
        Log4Func(("VM-entry failure: rcVMRun=%Rrc fVMEntryFailed=%RTbool\n", rcVMRun, pVmxTransient->fVMEntryFailed));

    VMMRZCallRing3Enable(pVCpu);
}


/**
 * Runs the guest code using hardware-assisted VMX the normal way.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pcLoops     Pointer to the number of executed loops.
 */
static VBOXSTRICTRC hmR0VmxRunGuestCodeNormal(PVMCPU pVCpu, uint32_t *pcLoops)
{
    uint32_t const cMaxResumeLoops = pVCpu->CTX_SUFF(pVM)->hm.s.cMaxResumeLoops;
    Assert(pcLoops);
    Assert(*pcLoops <= cMaxResumeLoops);

    VMXTRANSIENT VmxTransient;
    RT_ZERO(VmxTransient);
    VmxTransient.pVmcsInfo = hmGetVmxActiveVmcsInfo(pVCpu);

    /* Paranoia. */
    Assert(VmxTransient.pVmcsInfo == &pVCpu->hm.s.vmx.VmcsInfo);
    Assert(!CPUMIsGuestInVmxNonRootMode(&pVCpu->cpum.GstCtx));

    VBOXSTRICTRC rcStrict = VERR_INTERNAL_ERROR_5;
    for (;;)
    {
        Assert(!HMR0SuspendPending());
        HMVMX_ASSERT_CPU_SAFE(pVCpu);
        STAM_PROFILE_ADV_START(&pVCpu->hm.s.StatEntry, x);

        /*
         * Preparatory work for running nested-guest code, this may force us to
         * return to ring-3.
         *
         * Warning! This bugger disables interrupts on VINF_SUCCESS!
         */
        rcStrict = hmR0VmxPreRunGuest(pVCpu, &VmxTransient, false /* fStepping */);
        if (rcStrict != VINF_SUCCESS)
            break;

        /* Interrupts are disabled at this point! */
        hmR0VmxPreRunGuestCommitted(pVCpu, &VmxTransient);
        int rcRun = hmR0VmxRunGuest(pVCpu, &VmxTransient);
        hmR0VmxPostRunGuest(pVCpu, &VmxTransient, rcRun);
        /* Interrupts are re-enabled at this point! */

        /*
         * Check for errors with running the VM (VMLAUNCH/VMRESUME).
         */
        if (RT_SUCCESS(rcRun))
        { /* very likely */ }
        else
        {
            STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatPreExit, x);
            hmR0VmxReportWorldSwitchError(pVCpu, rcRun, &VmxTransient);
            return rcRun;
        }

        /*
         * Profile the VM-exit.
         */
        AssertMsg(VmxTransient.uExitReason <= VMX_EXIT_MAX, ("%#x\n", VmxTransient.uExitReason));
        STAM_COUNTER_INC(&pVCpu->hm.s.StatExitAll);
        STAM_COUNTER_INC(&pVCpu->hm.s.paStatExitReasonR0[VmxTransient.uExitReason & MASK_EXITREASON_STAT]);
        STAM_PROFILE_ADV_STOP_START(&pVCpu->hm.s.StatPreExit, &pVCpu->hm.s.StatExitHandling, x);
        HMVMX_START_EXIT_DISPATCH_PROF();

        VBOXVMM_R0_HMVMX_VMEXIT_NOCTX(pVCpu, &pVCpu->cpum.GstCtx, VmxTransient.uExitReason);

        /*
         * Handle the VM-exit.
         */
#ifdef HMVMX_USE_FUNCTION_TABLE
        rcStrict = g_apfnVMExitHandlers[VmxTransient.uExitReason](pVCpu, &VmxTransient);
#else
        rcStrict = hmR0VmxHandleExit(pVCpu, &VmxTransient);
#endif
        STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatExitHandling, x);
        if (rcStrict == VINF_SUCCESS)
        {
            if (++(*pcLoops) <= cMaxResumeLoops)
                continue;
            STAM_COUNTER_INC(&pVCpu->hm.s.StatSwitchMaxResumeLoops);
            rcStrict = VINF_EM_RAW_INTERRUPT;
        }
        break;
    }

    STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatEntry, x);
    return rcStrict;
}

#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
/**
 * Runs the nested-guest code using hardware-assisted VMX.
 *
 * @returns VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pcLoops     Pointer to the number of executed loops.
 *
 * @sa      hmR0VmxRunGuestCodeNormal.
 */
static VBOXSTRICTRC hmR0VmxRunGuestCodeNested(PVMCPU pVCpu, uint32_t *pcLoops)
{
    uint32_t const cMaxResumeLoops = pVCpu->CTX_SUFF(pVM)->hm.s.cMaxResumeLoops;
    Assert(pcLoops);
    Assert(*pcLoops <= cMaxResumeLoops);

    VMXTRANSIENT VmxTransient;
    RT_ZERO(VmxTransient);
    VmxTransient.pVmcsInfo = hmGetVmxActiveVmcsInfo(pVCpu);
    VmxTransient.fIsNestedGuest = true;

    VBOXSTRICTRC rcStrict = VERR_INTERNAL_ERROR_5;
    for (;;)
    {
        Assert(!HMR0SuspendPending());
        HMVMX_ASSERT_CPU_SAFE(pVCpu);
        STAM_PROFILE_ADV_START(&pVCpu->hm.s.StatEntry, x);

        /*
         * Preparatory work for running guest code, this may force us to
         * return to ring-3.
         *
         * Warning! This bugger disables interrupts on VINF_SUCCESS!
         */
        rcStrict = hmR0VmxPreRunGuest(pVCpu, &VmxTransient, false /* fStepping */);
        if (rcStrict != VINF_SUCCESS)
            break;

        /* Interrupts are disabled at this point! */
        hmR0VmxPreRunGuestCommitted(pVCpu, &VmxTransient);
        int rcRun = hmR0VmxRunGuest(pVCpu, &VmxTransient);
        hmR0VmxPostRunGuest(pVCpu, &VmxTransient, rcRun);
        /* Interrupts are re-enabled at this point! */

        /*
         * Check for errors with running the VM (VMLAUNCH/VMRESUME).
         */
        if (RT_SUCCESS(rcRun))
        { /* very likely */ }
        else
        {
            STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatPreExit, x);
            hmR0VmxReportWorldSwitchError(pVCpu, rcRun, &VmxTransient);
            return rcRun;
        }

        /*
         * Profile the VM-exit.
         */
        AssertMsg(VmxTransient.uExitReason <= VMX_EXIT_MAX, ("%#x\n", VmxTransient.uExitReason));
        STAM_COUNTER_INC(&pVCpu->hm.s.StatExitAll);
        STAM_COUNTER_INC(&pVCpu->hm.s.paStatNestedExitReasonR0[VmxTransient.uExitReason & MASK_EXITREASON_STAT]);
        STAM_PROFILE_ADV_STOP_START(&pVCpu->hm.s.StatPreExit, &pVCpu->hm.s.StatExitHandling, x);
        HMVMX_START_EXIT_DISPATCH_PROF();

        VBOXVMM_R0_HMVMX_VMEXIT_NOCTX(pVCpu, &pVCpu->cpum.GstCtx, VmxTransient.uExitReason);

        /*
         * Handle the VM-exit.
         */
        rcStrict = hmR0VmxHandleExitNested(pVCpu, &VmxTransient);
        STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatExitHandling, x);
        if (   rcStrict == VINF_SUCCESS
            && CPUMIsGuestInVmxNonRootMode(&pVCpu->cpum.GstCtx))
        {
            if (++(*pcLoops) <= cMaxResumeLoops)
                continue;
            STAM_COUNTER_INC(&pVCpu->hm.s.StatSwitchMaxResumeLoops);
            rcStrict = VINF_EM_RAW_INTERRUPT;
        }
        break;
    }

    STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatEntry, x);
    return rcStrict;
}
#endif /* VBOX_WITH_NESTED_HWVIRT_VMX */


/** @name Execution loop for single stepping, DBGF events and expensive Dtrace
 *  probes.
 *
 * The following few functions and associated structure contains the bloat
 * necessary for providing detailed debug events and dtrace probes as well as
 * reliable host side single stepping.  This works on the principle of
 * "subclassing" the normal execution loop and workers.  We replace the loop
 * method completely and override selected helpers to add necessary adjustments
 * to their core operation.
 *
 * The goal is to keep the "parent" code lean and mean, so as not to sacrifice
 * any performance for debug and analysis features.
 *
 * @{
 */

/**
 * Transient per-VCPU debug state of VMCS and related info. we save/restore in
 * the debug run loop.
 */
typedef struct VMXRUNDBGSTATE
{
    /** The RIP we started executing at.  This is for detecting that we stepped.  */
    uint64_t    uRipStart;
    /** The CS we started executing with.  */
    uint16_t    uCsStart;

    /** Whether we've actually modified the 1st execution control field. */
    bool        fModifiedProcCtls : 1;
    /** Whether we've actually modified the 2nd execution control field. */
    bool        fModifiedProcCtls2 : 1;
    /** Whether we've actually modified the exception bitmap. */
    bool        fModifiedXcptBitmap : 1;

    /** We desire the modified the CR0 mask to be cleared. */
    bool        fClearCr0Mask : 1;
    /** We desire the modified the CR4 mask to be cleared. */
    bool        fClearCr4Mask : 1;
    /** Stuff we need in VMX_VMCS32_CTRL_PROC_EXEC. */
    uint32_t    fCpe1Extra;
    /** Stuff we do not want in VMX_VMCS32_CTRL_PROC_EXEC. */
    uint32_t    fCpe1Unwanted;
    /** Stuff we need in VMX_VMCS32_CTRL_PROC_EXEC2. */
    uint32_t    fCpe2Extra;
    /** Extra stuff we need in VMX_VMCS32_CTRL_EXCEPTION_BITMAP. */
    uint32_t    bmXcptExtra;
    /** The sequence number of the Dtrace provider settings the state was
     *  configured against. */
    uint32_t    uDtraceSettingsSeqNo;
    /** VM-exits to check (one bit per VM-exit). */
    uint32_t    bmExitsToCheck[3];

    /** The initial VMX_VMCS32_CTRL_PROC_EXEC value (helps with restore). */
    uint32_t    fProcCtlsInitial;
    /** The initial VMX_VMCS32_CTRL_PROC_EXEC2 value (helps with restore). */
    uint32_t    fProcCtls2Initial;
    /** The initial VMX_VMCS32_CTRL_EXCEPTION_BITMAP value (helps with restore). */
    uint32_t    bmXcptInitial;
} VMXRUNDBGSTATE;
AssertCompileMemberSize(VMXRUNDBGSTATE, bmExitsToCheck, (VMX_EXIT_MAX + 1 + 31) / 32 * 4);
typedef VMXRUNDBGSTATE *PVMXRUNDBGSTATE;


/**
 * Initializes the VMXRUNDBGSTATE structure.
 *
 * @param   pVCpu           The cross context virtual CPU structure of the
 *                          calling EMT.
 * @param   pVmxTransient   The VMX-transient structure.
 * @param   pDbgState       The debug state to initialize.
 */
static void hmR0VmxRunDebugStateInit(PVMCPU pVCpu, PCVMXTRANSIENT pVmxTransient, PVMXRUNDBGSTATE pDbgState)
{
    pDbgState->uRipStart            = pVCpu->cpum.GstCtx.rip;
    pDbgState->uCsStart             = pVCpu->cpum.GstCtx.cs.Sel;

    pDbgState->fModifiedProcCtls    = false;
    pDbgState->fModifiedProcCtls2   = false;
    pDbgState->fModifiedXcptBitmap  = false;
    pDbgState->fClearCr0Mask        = false;
    pDbgState->fClearCr4Mask        = false;
    pDbgState->fCpe1Extra           = 0;
    pDbgState->fCpe1Unwanted        = 0;
    pDbgState->fCpe2Extra           = 0;
    pDbgState->bmXcptExtra          = 0;
    pDbgState->fProcCtlsInitial     = pVmxTransient->pVmcsInfo->u32ProcCtls;
    pDbgState->fProcCtls2Initial    = pVmxTransient->pVmcsInfo->u32ProcCtls2;
    pDbgState->bmXcptInitial        = pVmxTransient->pVmcsInfo->u32XcptBitmap;
}


/**
 * Updates the VMSC fields with changes requested by @a pDbgState.
 *
 * This is performed after hmR0VmxPreRunGuestDebugStateUpdate as well
 * immediately before executing guest code, i.e. when interrupts are disabled.
 * We don't check status codes here as we cannot easily assert or return in the
 * latter case.
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 * @param   pDbgState       The debug state.
 */
static void hmR0VmxPreRunGuestDebugStateApply(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient, PVMXRUNDBGSTATE pDbgState)
{
    /*
     * Ensure desired flags in VMCS control fields are set.
     * (Ignoring write failure here, as we're committed and it's just debug extras.)
     *
     * Note! We load the shadow CR0 & CR4 bits when we flag the clearing, so
     *       there should be no stale data in pCtx at this point.
     */
    PVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    if (   (pVmcsInfo->u32ProcCtls & pDbgState->fCpe1Extra) != pDbgState->fCpe1Extra
        || (pVmcsInfo->u32ProcCtls & pDbgState->fCpe1Unwanted))
    {
        pVmcsInfo->u32ProcCtls |= pDbgState->fCpe1Extra;
        pVmcsInfo->u32ProcCtls &= ~pDbgState->fCpe1Unwanted;
        VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC, pVmcsInfo->u32ProcCtls);
        Log6Func(("VMX_VMCS32_CTRL_PROC_EXEC: %#RX32\n", pVmcsInfo->u32ProcCtls));
        pDbgState->fModifiedProcCtls   = true;
    }

    if ((pVmcsInfo->u32ProcCtls2 & pDbgState->fCpe2Extra) != pDbgState->fCpe2Extra)
    {
        pVmcsInfo->u32ProcCtls2  |= pDbgState->fCpe2Extra;
        VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC2, pVmcsInfo->u32ProcCtls2);
        Log6Func(("VMX_VMCS32_CTRL_PROC_EXEC2: %#RX32\n", pVmcsInfo->u32ProcCtls2));
        pDbgState->fModifiedProcCtls2  = true;
    }

    if ((pVmcsInfo->u32XcptBitmap & pDbgState->bmXcptExtra) != pDbgState->bmXcptExtra)
    {
        pVmcsInfo->u32XcptBitmap |= pDbgState->bmXcptExtra;
        VMXWriteVmcs32(VMX_VMCS32_CTRL_EXCEPTION_BITMAP, pVmcsInfo->u32XcptBitmap);
        Log6Func(("VMX_VMCS32_CTRL_EXCEPTION_BITMAP: %#RX32\n", pVmcsInfo->u32XcptBitmap));
        pDbgState->fModifiedXcptBitmap = true;
    }

    if (pDbgState->fClearCr0Mask && pVmcsInfo->u64Cr0Mask != 0)
    {
        pVmcsInfo->u64Cr0Mask = 0;
        VMXWriteVmcsHstN(VMX_VMCS_CTRL_CR0_MASK, 0);
        Log6Func(("VMX_VMCS_CTRL_CR0_MASK: 0\n"));
    }

    if (pDbgState->fClearCr4Mask && pVmcsInfo->u64Cr4Mask != 0)
    {
        pVmcsInfo->u64Cr4Mask = 0;
        VMXWriteVmcsHstN(VMX_VMCS_CTRL_CR4_MASK, 0);
        Log6Func(("VMX_VMCS_CTRL_CR4_MASK: 0\n"));
    }

    NOREF(pVCpu);
}


/**
 * Restores VMCS fields that were changed by hmR0VmxPreRunGuestDebugStateApply for
 * re-entry next time around.
 *
 * @returns Strict VBox status code (i.e. informational status codes too).
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 * @param   pDbgState       The debug state.
 * @param   rcStrict        The return code from executing the guest using single
 *                          stepping.
 */
static VBOXSTRICTRC hmR0VmxRunDebugStateRevert(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient, PVMXRUNDBGSTATE pDbgState,
                                               VBOXSTRICTRC rcStrict)
{
    /*
     * Restore VM-exit control settings as we may not reenter this function the
     * next time around.
     */
    PVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;

    /* We reload the initial value, trigger what we can of recalculations the
       next time around.  From the looks of things, that's all that's required atm. */
    if (pDbgState->fModifiedProcCtls)
    {
        if (!(pDbgState->fProcCtlsInitial & VMX_PROC_CTLS_MOV_DR_EXIT) && CPUMIsHyperDebugStateActive(pVCpu))
            pDbgState->fProcCtlsInitial |= VMX_PROC_CTLS_MOV_DR_EXIT; /* Avoid assertion in hmR0VmxLeave */
        int rc2 = VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC, pDbgState->fProcCtlsInitial);
        AssertRCReturn(rc2, rc2);
        pVmcsInfo->u32ProcCtls = pDbgState->fProcCtlsInitial;
    }

    /* We're currently the only ones messing with this one, so just restore the
       cached value and reload the field. */
    if (   pDbgState->fModifiedProcCtls2
        && pVmcsInfo->u32ProcCtls2 != pDbgState->fProcCtls2Initial)
    {
        int rc2 = VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC2, pDbgState->fProcCtls2Initial);
        AssertRCReturn(rc2, rc2);
        pVmcsInfo->u32ProcCtls2 = pDbgState->fProcCtls2Initial;
    }

    /* If we've modified the exception bitmap, we restore it and trigger
       reloading and partial recalculation the next time around. */
    if (pDbgState->fModifiedXcptBitmap)
        pVmcsInfo->u32XcptBitmap = pDbgState->bmXcptInitial;

    return rcStrict;
}


/**
 * Configures VM-exit controls for current DBGF and DTrace settings.
 *
 * This updates @a pDbgState and the VMCS execution control fields to reflect
 * the necessary VM-exits demanded by DBGF and DTrace.
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure. May update
 *                          fUpdatedTscOffsettingAndPreemptTimer.
 * @param   pDbgState       The debug state.
 */
static void hmR0VmxPreRunGuestDebugStateUpdate(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient, PVMXRUNDBGSTATE pDbgState)
{
    /*
     * Take down the dtrace serial number so we can spot changes.
     */
    pDbgState->uDtraceSettingsSeqNo = VBOXVMM_GET_SETTINGS_SEQ_NO();
    ASMCompilerBarrier();

    /*
     * We'll rebuild most of the middle block of data members (holding the
     * current settings) as we go along here, so start by clearing it all.
     */
    pDbgState->bmXcptExtra      = 0;
    pDbgState->fCpe1Extra       = 0;
    pDbgState->fCpe1Unwanted    = 0;
    pDbgState->fCpe2Extra       = 0;
    for (unsigned i = 0; i < RT_ELEMENTS(pDbgState->bmExitsToCheck); i++)
        pDbgState->bmExitsToCheck[i] = 0;

    /*
     * Software interrupts (INT XXh) - no idea how to trigger these...
     */
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    if (   DBGF_IS_EVENT_ENABLED(pVM, DBGFEVENT_INTERRUPT_SOFTWARE)
        || VBOXVMM_INT_SOFTWARE_ENABLED())
    {
        ASMBitSet(pDbgState->bmExitsToCheck, VMX_EXIT_XCPT_OR_NMI);
    }

    /*
     * INT3 breakpoints - triggered by #BP exceptions.
     */
    if (pVM->dbgf.ro.cEnabledInt3Breakpoints > 0)
        pDbgState->bmXcptExtra |= RT_BIT_32(X86_XCPT_BP);

    /*
     * Exception bitmap and XCPT events+probes.
     */
    for (int iXcpt = 0; iXcpt < (DBGFEVENT_XCPT_LAST - DBGFEVENT_XCPT_FIRST + 1); iXcpt++)
        if (DBGF_IS_EVENT_ENABLED(pVM, (DBGFEVENTTYPE)(DBGFEVENT_XCPT_FIRST + iXcpt)))
            pDbgState->bmXcptExtra |= RT_BIT_32(iXcpt);

    if (VBOXVMM_XCPT_DE_ENABLED())  pDbgState->bmXcptExtra |= RT_BIT_32(X86_XCPT_DE);
    if (VBOXVMM_XCPT_DB_ENABLED())  pDbgState->bmXcptExtra |= RT_BIT_32(X86_XCPT_DB);
    if (VBOXVMM_XCPT_BP_ENABLED())  pDbgState->bmXcptExtra |= RT_BIT_32(X86_XCPT_BP);
    if (VBOXVMM_XCPT_OF_ENABLED())  pDbgState->bmXcptExtra |= RT_BIT_32(X86_XCPT_OF);
    if (VBOXVMM_XCPT_BR_ENABLED())  pDbgState->bmXcptExtra |= RT_BIT_32(X86_XCPT_BR);
    if (VBOXVMM_XCPT_UD_ENABLED())  pDbgState->bmXcptExtra |= RT_BIT_32(X86_XCPT_UD);
    if (VBOXVMM_XCPT_NM_ENABLED())  pDbgState->bmXcptExtra |= RT_BIT_32(X86_XCPT_NM);
    if (VBOXVMM_XCPT_DF_ENABLED())  pDbgState->bmXcptExtra |= RT_BIT_32(X86_XCPT_DF);
    if (VBOXVMM_XCPT_TS_ENABLED())  pDbgState->bmXcptExtra |= RT_BIT_32(X86_XCPT_TS);
    if (VBOXVMM_XCPT_NP_ENABLED())  pDbgState->bmXcptExtra |= RT_BIT_32(X86_XCPT_NP);
    if (VBOXVMM_XCPT_SS_ENABLED())  pDbgState->bmXcptExtra |= RT_BIT_32(X86_XCPT_SS);
    if (VBOXVMM_XCPT_GP_ENABLED())  pDbgState->bmXcptExtra |= RT_BIT_32(X86_XCPT_GP);
    if (VBOXVMM_XCPT_PF_ENABLED())  pDbgState->bmXcptExtra |= RT_BIT_32(X86_XCPT_PF);
    if (VBOXVMM_XCPT_MF_ENABLED())  pDbgState->bmXcptExtra |= RT_BIT_32(X86_XCPT_MF);
    if (VBOXVMM_XCPT_AC_ENABLED())  pDbgState->bmXcptExtra |= RT_BIT_32(X86_XCPT_AC);
    if (VBOXVMM_XCPT_XF_ENABLED())  pDbgState->bmXcptExtra |= RT_BIT_32(X86_XCPT_XF);
    if (VBOXVMM_XCPT_VE_ENABLED())  pDbgState->bmXcptExtra |= RT_BIT_32(X86_XCPT_VE);
    if (VBOXVMM_XCPT_SX_ENABLED())  pDbgState->bmXcptExtra |= RT_BIT_32(X86_XCPT_SX);

    if (pDbgState->bmXcptExtra)
        ASMBitSet(pDbgState->bmExitsToCheck, VMX_EXIT_XCPT_OR_NMI);

    /*
     * Process events and probes for VM-exits, making sure we get the wanted VM-exits.
     *
     * Note! This is the reverse of what hmR0VmxHandleExitDtraceEvents does.
     *       So, when adding/changing/removing please don't forget to update it.
     *
     * Some of the macros are picking up local variables to save horizontal space,
     * (being able to see it in a table is the lesser evil here).
     */
#define IS_EITHER_ENABLED(a_pVM, a_EventSubName) \
        (    DBGF_IS_EVENT_ENABLED(a_pVM, RT_CONCAT(DBGFEVENT_, a_EventSubName)) \
         ||  RT_CONCAT3(VBOXVMM_, a_EventSubName, _ENABLED)() )
#define SET_ONLY_XBM_IF_EITHER_EN(a_EventSubName, a_uExit) \
        if (IS_EITHER_ENABLED(pVM, a_EventSubName)) \
        {   AssertCompile((unsigned)(a_uExit) < sizeof(pDbgState->bmExitsToCheck) * 8); \
            ASMBitSet((pDbgState)->bmExitsToCheck, a_uExit); \
        } else do { } while (0)
#define SET_CPE1_XBM_IF_EITHER_EN(a_EventSubName, a_uExit, a_fCtrlProcExec) \
        if (IS_EITHER_ENABLED(pVM, a_EventSubName)) \
        { \
            (pDbgState)->fCpe1Extra |= (a_fCtrlProcExec); \
            AssertCompile((unsigned)(a_uExit) < sizeof(pDbgState->bmExitsToCheck) * 8); \
            ASMBitSet((pDbgState)->bmExitsToCheck, a_uExit); \
        } else do { } while (0)
#define SET_CPEU_XBM_IF_EITHER_EN(a_EventSubName, a_uExit, a_fUnwantedCtrlProcExec) \
        if (IS_EITHER_ENABLED(pVM, a_EventSubName)) \
        { \
            (pDbgState)->fCpe1Unwanted |= (a_fUnwantedCtrlProcExec); \
            AssertCompile((unsigned)(a_uExit) < sizeof(pDbgState->bmExitsToCheck) * 8); \
            ASMBitSet((pDbgState)->bmExitsToCheck, a_uExit); \
        } else do { } while (0)
#define SET_CPE2_XBM_IF_EITHER_EN(a_EventSubName, a_uExit, a_fCtrlProcExec2) \
        if (IS_EITHER_ENABLED(pVM, a_EventSubName)) \
        { \
            (pDbgState)->fCpe2Extra |= (a_fCtrlProcExec2); \
            AssertCompile((unsigned)(a_uExit) < sizeof(pDbgState->bmExitsToCheck) * 8); \
            ASMBitSet((pDbgState)->bmExitsToCheck, a_uExit); \
        } else do { } while (0)

    SET_ONLY_XBM_IF_EITHER_EN(EXIT_TASK_SWITCH,         VMX_EXIT_TASK_SWITCH);   /* unconditional */
    SET_ONLY_XBM_IF_EITHER_EN(EXIT_VMX_EPT_VIOLATION,   VMX_EXIT_EPT_VIOLATION); /* unconditional */
    SET_ONLY_XBM_IF_EITHER_EN(EXIT_VMX_EPT_MISCONFIG,   VMX_EXIT_EPT_MISCONFIG); /* unconditional (unless #VE) */
    SET_ONLY_XBM_IF_EITHER_EN(EXIT_VMX_VAPIC_ACCESS,    VMX_EXIT_APIC_ACCESS);   /* feature dependent, nothing to enable here */
    SET_ONLY_XBM_IF_EITHER_EN(EXIT_VMX_VAPIC_WRITE,     VMX_EXIT_APIC_WRITE);    /* feature dependent, nothing to enable here */

    SET_ONLY_XBM_IF_EITHER_EN(INSTR_CPUID,              VMX_EXIT_CPUID);         /* unconditional */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_CPUID,              VMX_EXIT_CPUID);
    SET_ONLY_XBM_IF_EITHER_EN(INSTR_GETSEC,             VMX_EXIT_GETSEC);        /* unconditional */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_GETSEC,             VMX_EXIT_GETSEC);
    SET_CPE1_XBM_IF_EITHER_EN(INSTR_HALT,               VMX_EXIT_HLT,      VMX_PROC_CTLS_HLT_EXIT); /* paranoia */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_HALT,               VMX_EXIT_HLT);
    SET_ONLY_XBM_IF_EITHER_EN(INSTR_INVD,               VMX_EXIT_INVD);          /* unconditional */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_INVD,               VMX_EXIT_INVD);
    SET_CPE1_XBM_IF_EITHER_EN(INSTR_INVLPG,             VMX_EXIT_INVLPG,   VMX_PROC_CTLS_INVLPG_EXIT);
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_INVLPG,             VMX_EXIT_INVLPG);
    SET_CPE1_XBM_IF_EITHER_EN(INSTR_RDPMC,              VMX_EXIT_RDPMC,    VMX_PROC_CTLS_RDPMC_EXIT);
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_RDPMC,              VMX_EXIT_RDPMC);
    SET_CPE1_XBM_IF_EITHER_EN(INSTR_RDTSC,              VMX_EXIT_RDTSC,    VMX_PROC_CTLS_RDTSC_EXIT);
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_RDTSC,              VMX_EXIT_RDTSC);
    SET_ONLY_XBM_IF_EITHER_EN(INSTR_RSM,                VMX_EXIT_RSM);           /* unconditional */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_RSM,                VMX_EXIT_RSM);
    SET_ONLY_XBM_IF_EITHER_EN(INSTR_VMM_CALL,           VMX_EXIT_VMCALL);        /* unconditional */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_VMM_CALL,           VMX_EXIT_VMCALL);
    SET_ONLY_XBM_IF_EITHER_EN(INSTR_VMX_VMCLEAR,        VMX_EXIT_VMCLEAR);       /* unconditional */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_VMX_VMCLEAR,        VMX_EXIT_VMCLEAR);
    SET_ONLY_XBM_IF_EITHER_EN(INSTR_VMX_VMLAUNCH,       VMX_EXIT_VMLAUNCH);      /* unconditional */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_VMX_VMLAUNCH,       VMX_EXIT_VMLAUNCH);
    SET_ONLY_XBM_IF_EITHER_EN(INSTR_VMX_VMPTRLD,        VMX_EXIT_VMPTRLD);       /* unconditional */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_VMX_VMPTRLD,        VMX_EXIT_VMPTRLD);
    SET_ONLY_XBM_IF_EITHER_EN(INSTR_VMX_VMPTRST,        VMX_EXIT_VMPTRST);       /* unconditional */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_VMX_VMPTRST,        VMX_EXIT_VMPTRST);
    SET_ONLY_XBM_IF_EITHER_EN(INSTR_VMX_VMREAD,         VMX_EXIT_VMREAD);        /* unconditional */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_VMX_VMREAD,         VMX_EXIT_VMREAD);
    SET_ONLY_XBM_IF_EITHER_EN(INSTR_VMX_VMRESUME,       VMX_EXIT_VMRESUME);      /* unconditional */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_VMX_VMRESUME,       VMX_EXIT_VMRESUME);
    SET_ONLY_XBM_IF_EITHER_EN(INSTR_VMX_VMWRITE,        VMX_EXIT_VMWRITE);       /* unconditional */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_VMX_VMWRITE,        VMX_EXIT_VMWRITE);
    SET_ONLY_XBM_IF_EITHER_EN(INSTR_VMX_VMXOFF,         VMX_EXIT_VMXOFF);        /* unconditional */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_VMX_VMXOFF,         VMX_EXIT_VMXOFF);
    SET_ONLY_XBM_IF_EITHER_EN(INSTR_VMX_VMXON,          VMX_EXIT_VMXON);         /* unconditional */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_VMX_VMXON,          VMX_EXIT_VMXON);

    if (   IS_EITHER_ENABLED(pVM, INSTR_CRX_READ)
        || IS_EITHER_ENABLED(pVM, INSTR_CRX_WRITE))
    {
        int rc = hmR0VmxImportGuestState(pVCpu, pVmxTransient->pVmcsInfo, CPUMCTX_EXTRN_CR0 | CPUMCTX_EXTRN_CR4
                                                                        | CPUMCTX_EXTRN_APIC_TPR);
        AssertRC(rc);

#if 0 /** @todo fix me */
        pDbgState->fClearCr0Mask = true;
        pDbgState->fClearCr4Mask = true;
#endif
        if (IS_EITHER_ENABLED(pVM, INSTR_CRX_READ))
            pDbgState->fCpe1Extra |= VMX_PROC_CTLS_CR3_STORE_EXIT | VMX_PROC_CTLS_CR8_STORE_EXIT;
        if (IS_EITHER_ENABLED(pVM, INSTR_CRX_WRITE))
            pDbgState->fCpe1Extra |= VMX_PROC_CTLS_CR3_LOAD_EXIT | VMX_PROC_CTLS_CR8_LOAD_EXIT;
        pDbgState->fCpe1Unwanted |= VMX_PROC_CTLS_USE_TPR_SHADOW; /* risky? */
        /* Note! We currently don't use VMX_VMCS32_CTRL_CR3_TARGET_COUNT.  It would
                 require clearing here and in the loop if we start using it. */
        ASMBitSet(pDbgState->bmExitsToCheck, VMX_EXIT_MOV_CRX);
    }
    else
    {
        if (pDbgState->fClearCr0Mask)
        {
            pDbgState->fClearCr0Mask = false;
            ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_CR0);
        }
        if (pDbgState->fClearCr4Mask)
        {
            pDbgState->fClearCr4Mask = false;
            ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_CR4);
        }
    }
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_CRX_READ,           VMX_EXIT_MOV_CRX);
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_CRX_WRITE,          VMX_EXIT_MOV_CRX);

    if (   IS_EITHER_ENABLED(pVM, INSTR_DRX_READ)
        || IS_EITHER_ENABLED(pVM, INSTR_DRX_WRITE))
    {
        /** @todo later, need to fix handler as it assumes this won't usually happen. */
        ASMBitSet(pDbgState->bmExitsToCheck, VMX_EXIT_MOV_DRX);
    }
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_DRX_READ,           VMX_EXIT_MOV_DRX);
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_DRX_WRITE,          VMX_EXIT_MOV_DRX);

    SET_CPEU_XBM_IF_EITHER_EN(INSTR_RDMSR,              VMX_EXIT_RDMSR,    VMX_PROC_CTLS_USE_MSR_BITMAPS); /* risky clearing this? */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_RDMSR,              VMX_EXIT_RDMSR);
    SET_CPEU_XBM_IF_EITHER_EN(INSTR_WRMSR,              VMX_EXIT_WRMSR,    VMX_PROC_CTLS_USE_MSR_BITMAPS);
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_WRMSR,              VMX_EXIT_WRMSR);
    SET_CPE1_XBM_IF_EITHER_EN(INSTR_MWAIT,              VMX_EXIT_MWAIT,    VMX_PROC_CTLS_MWAIT_EXIT);   /* paranoia */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_MWAIT,              VMX_EXIT_MWAIT);
    SET_CPE1_XBM_IF_EITHER_EN(INSTR_MONITOR,            VMX_EXIT_MONITOR,  VMX_PROC_CTLS_MONITOR_EXIT); /* paranoia */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_MONITOR,            VMX_EXIT_MONITOR);
#if 0 /** @todo too slow, fix handler. */
    SET_CPE1_XBM_IF_EITHER_EN(INSTR_PAUSE,              VMX_EXIT_PAUSE,    VMX_PROC_CTLS_PAUSE_EXIT);
#endif
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_PAUSE,              VMX_EXIT_PAUSE);

    if (   IS_EITHER_ENABLED(pVM, INSTR_SGDT)
        || IS_EITHER_ENABLED(pVM, INSTR_SIDT)
        || IS_EITHER_ENABLED(pVM, INSTR_LGDT)
        || IS_EITHER_ENABLED(pVM, INSTR_LIDT))
    {
        pDbgState->fCpe2Extra |= VMX_PROC_CTLS2_DESC_TABLE_EXIT;
        ASMBitSet(pDbgState->bmExitsToCheck, VMX_EXIT_GDTR_IDTR_ACCESS);
    }
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_SGDT,               VMX_EXIT_GDTR_IDTR_ACCESS);
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_SIDT,               VMX_EXIT_GDTR_IDTR_ACCESS);
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_LGDT,               VMX_EXIT_GDTR_IDTR_ACCESS);
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_LIDT,               VMX_EXIT_GDTR_IDTR_ACCESS);

    if (   IS_EITHER_ENABLED(pVM, INSTR_SLDT)
        || IS_EITHER_ENABLED(pVM, INSTR_STR)
        || IS_EITHER_ENABLED(pVM, INSTR_LLDT)
        || IS_EITHER_ENABLED(pVM, INSTR_LTR))
    {
        pDbgState->fCpe2Extra |= VMX_PROC_CTLS2_DESC_TABLE_EXIT;
        ASMBitSet(pDbgState->bmExitsToCheck, VMX_EXIT_LDTR_TR_ACCESS);
    }
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_SLDT,               VMX_EXIT_LDTR_TR_ACCESS);
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_STR,                VMX_EXIT_LDTR_TR_ACCESS);
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_LLDT,               VMX_EXIT_LDTR_TR_ACCESS);
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_LTR,                VMX_EXIT_LDTR_TR_ACCESS);

    SET_ONLY_XBM_IF_EITHER_EN(INSTR_VMX_INVEPT,         VMX_EXIT_INVEPT);        /* unconditional */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_VMX_INVEPT,         VMX_EXIT_INVEPT);
    SET_CPE1_XBM_IF_EITHER_EN(INSTR_RDTSCP,             VMX_EXIT_RDTSCP,   VMX_PROC_CTLS_RDTSC_EXIT);
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_RDTSCP,             VMX_EXIT_RDTSCP);
    SET_ONLY_XBM_IF_EITHER_EN(INSTR_VMX_INVVPID,        VMX_EXIT_INVVPID);       /* unconditional */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_VMX_INVVPID,        VMX_EXIT_INVVPID);
    SET_CPE2_XBM_IF_EITHER_EN(INSTR_WBINVD,             VMX_EXIT_WBINVD,   VMX_PROC_CTLS2_WBINVD_EXIT);
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_WBINVD,             VMX_EXIT_WBINVD);
    SET_ONLY_XBM_IF_EITHER_EN(INSTR_XSETBV,             VMX_EXIT_XSETBV);        /* unconditional */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_XSETBV,             VMX_EXIT_XSETBV);
    SET_CPE2_XBM_IF_EITHER_EN(INSTR_RDRAND,             VMX_EXIT_RDRAND,   VMX_PROC_CTLS2_RDRAND_EXIT);
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_RDRAND,             VMX_EXIT_RDRAND);
    SET_CPE1_XBM_IF_EITHER_EN(INSTR_VMX_INVPCID,        VMX_EXIT_INVPCID,  VMX_PROC_CTLS_INVLPG_EXIT);
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_VMX_INVPCID,        VMX_EXIT_INVPCID);
    SET_ONLY_XBM_IF_EITHER_EN(INSTR_VMX_VMFUNC,         VMX_EXIT_VMFUNC);        /* unconditional for the current setup */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_VMX_VMFUNC,         VMX_EXIT_VMFUNC);
    SET_CPE2_XBM_IF_EITHER_EN(INSTR_RDSEED,             VMX_EXIT_RDSEED,   VMX_PROC_CTLS2_RDSEED_EXIT);
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_RDSEED,             VMX_EXIT_RDSEED);
    SET_ONLY_XBM_IF_EITHER_EN(INSTR_XSAVES,             VMX_EXIT_XSAVES);        /* unconditional (enabled by host, guest cfg) */
    SET_ONLY_XBM_IF_EITHER_EN(EXIT_XSAVES,              VMX_EXIT_XSAVES);
    SET_ONLY_XBM_IF_EITHER_EN(INSTR_XRSTORS,            VMX_EXIT_XRSTORS);       /* unconditional (enabled by host, guest cfg) */
    SET_ONLY_XBM_IF_EITHER_EN( EXIT_XRSTORS,            VMX_EXIT_XRSTORS);

#undef IS_EITHER_ENABLED
#undef SET_ONLY_XBM_IF_EITHER_EN
#undef SET_CPE1_XBM_IF_EITHER_EN
#undef SET_CPEU_XBM_IF_EITHER_EN
#undef SET_CPE2_XBM_IF_EITHER_EN

    /*
     * Sanitize the control stuff.
     */
    pDbgState->fCpe2Extra       &= pVM->hm.s.vmx.Msrs.ProcCtls2.n.allowed1;
    if (pDbgState->fCpe2Extra)
        pDbgState->fCpe1Extra   |= VMX_PROC_CTLS_USE_SECONDARY_CTLS;
    pDbgState->fCpe1Extra       &= pVM->hm.s.vmx.Msrs.ProcCtls.n.allowed1;
    pDbgState->fCpe1Unwanted    &= ~pVM->hm.s.vmx.Msrs.ProcCtls.n.allowed0;
    if (pVCpu->hm.s.fDebugWantRdTscExit != RT_BOOL(pDbgState->fCpe1Extra & VMX_PROC_CTLS_RDTSC_EXIT))
    {
        pVCpu->hm.s.fDebugWantRdTscExit ^= true;
        pVmxTransient->fUpdatedTscOffsettingAndPreemptTimer = false;
    }

    Log6(("HM: debug state: cpe1=%#RX32 cpeu=%#RX32 cpe2=%#RX32%s%s\n",
          pDbgState->fCpe1Extra, pDbgState->fCpe1Unwanted, pDbgState->fCpe2Extra,
          pDbgState->fClearCr0Mask ? " clr-cr0" : "",
          pDbgState->fClearCr4Mask ? " clr-cr4" : ""));
}


/**
 * Fires off DBGF events and dtrace probes for a VM-exit, when it's
 * appropriate.
 *
 * The caller has checked the VM-exit against the
 * VMXRUNDBGSTATE::bmExitsToCheck bitmap. The caller has checked for NMIs
 * already, so we don't have to do that either.
 *
 * @returns Strict VBox status code (i.e. informational status codes too).
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 * @param   uExitReason     The VM-exit reason.
 *
 * @remarks The name of this function is displayed by dtrace, so keep it short
 *          and to the point. No longer than 33 chars long, please.
 */
static VBOXSTRICTRC hmR0VmxHandleExitDtraceEvents(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient, uint32_t uExitReason)
{
    /*
     * Translate the event into a DBGF event (enmEvent + uEventArg) and at the
     * same time check whether any corresponding Dtrace event is enabled (fDtrace).
     *
     * Note! This is the reverse operation of what hmR0VmxPreRunGuestDebugStateUpdate
     *       does.  Must add/change/remove both places.  Same ordering, please.
     *
     *       Added/removed events must also be reflected in the next section
     *       where we dispatch dtrace events.
     */
    bool            fDtrace1   = false;
    bool            fDtrace2   = false;
    DBGFEVENTTYPE   enmEvent1  = DBGFEVENT_END;
    DBGFEVENTTYPE   enmEvent2  = DBGFEVENT_END;
    uint32_t        uEventArg  = 0;
#define SET_EXIT(a_EventSubName) \
        do { \
            enmEvent2 = RT_CONCAT(DBGFEVENT_EXIT_,  a_EventSubName); \
            fDtrace2  = RT_CONCAT3(VBOXVMM_EXIT_,   a_EventSubName, _ENABLED)(); \
        } while (0)
#define SET_BOTH(a_EventSubName) \
        do { \
            enmEvent1 = RT_CONCAT(DBGFEVENT_INSTR_, a_EventSubName); \
            enmEvent2 = RT_CONCAT(DBGFEVENT_EXIT_,  a_EventSubName); \
            fDtrace1  = RT_CONCAT3(VBOXVMM_INSTR_,  a_EventSubName, _ENABLED)(); \
            fDtrace2  = RT_CONCAT3(VBOXVMM_EXIT_,   a_EventSubName, _ENABLED)(); \
        } while (0)
    switch (uExitReason)
    {
        case VMX_EXIT_MTF:
            return hmR0VmxExitMtf(pVCpu, pVmxTransient);

        case VMX_EXIT_XCPT_OR_NMI:
        {
            uint8_t const idxVector = VMX_EXIT_INT_INFO_VECTOR(pVmxTransient->uExitIntInfo);
            switch (VMX_EXIT_INT_INFO_TYPE(pVmxTransient->uExitIntInfo))
            {
                case VMX_EXIT_INT_INFO_TYPE_HW_XCPT:
                case VMX_EXIT_INT_INFO_TYPE_SW_XCPT:
                case VMX_EXIT_INT_INFO_TYPE_PRIV_SW_XCPT:
                    if (idxVector <= (unsigned)(DBGFEVENT_XCPT_LAST - DBGFEVENT_XCPT_FIRST))
                    {
                        if (VMX_EXIT_INT_INFO_IS_ERROR_CODE_VALID(pVmxTransient->uExitIntInfo))
                        {
                            hmR0VmxReadExitIntErrorCodeVmcs(pVmxTransient);
                            uEventArg = pVmxTransient->uExitIntErrorCode;
                        }
                        enmEvent1 = (DBGFEVENTTYPE)(DBGFEVENT_XCPT_FIRST + idxVector);
                        switch (enmEvent1)
                        {
                            case DBGFEVENT_XCPT_DE: fDtrace1 = VBOXVMM_XCPT_DE_ENABLED(); break;
                            case DBGFEVENT_XCPT_DB: fDtrace1 = VBOXVMM_XCPT_DB_ENABLED(); break;
                            case DBGFEVENT_XCPT_BP: fDtrace1 = VBOXVMM_XCPT_BP_ENABLED(); break;
                            case DBGFEVENT_XCPT_OF: fDtrace1 = VBOXVMM_XCPT_OF_ENABLED(); break;
                            case DBGFEVENT_XCPT_BR: fDtrace1 = VBOXVMM_XCPT_BR_ENABLED(); break;
                            case DBGFEVENT_XCPT_UD: fDtrace1 = VBOXVMM_XCPT_UD_ENABLED(); break;
                            case DBGFEVENT_XCPT_NM: fDtrace1 = VBOXVMM_XCPT_NM_ENABLED(); break;
                            case DBGFEVENT_XCPT_DF: fDtrace1 = VBOXVMM_XCPT_DF_ENABLED(); break;
                            case DBGFEVENT_XCPT_TS: fDtrace1 = VBOXVMM_XCPT_TS_ENABLED(); break;
                            case DBGFEVENT_XCPT_NP: fDtrace1 = VBOXVMM_XCPT_NP_ENABLED(); break;
                            case DBGFEVENT_XCPT_SS: fDtrace1 = VBOXVMM_XCPT_SS_ENABLED(); break;
                            case DBGFEVENT_XCPT_GP: fDtrace1 = VBOXVMM_XCPT_GP_ENABLED(); break;
                            case DBGFEVENT_XCPT_PF: fDtrace1 = VBOXVMM_XCPT_PF_ENABLED(); break;
                            case DBGFEVENT_XCPT_MF: fDtrace1 = VBOXVMM_XCPT_MF_ENABLED(); break;
                            case DBGFEVENT_XCPT_AC: fDtrace1 = VBOXVMM_XCPT_AC_ENABLED(); break;
                            case DBGFEVENT_XCPT_XF: fDtrace1 = VBOXVMM_XCPT_XF_ENABLED(); break;
                            case DBGFEVENT_XCPT_VE: fDtrace1 = VBOXVMM_XCPT_VE_ENABLED(); break;
                            case DBGFEVENT_XCPT_SX: fDtrace1 = VBOXVMM_XCPT_SX_ENABLED(); break;
                            default:                                                      break;
                        }
                    }
                    else
                        AssertFailed();
                    break;

                case VMX_EXIT_INT_INFO_TYPE_SW_INT:
                    uEventArg = idxVector;
                    enmEvent1 = DBGFEVENT_INTERRUPT_SOFTWARE;
                    fDtrace1  = VBOXVMM_INT_SOFTWARE_ENABLED();
                    break;
            }
            break;
        }

        case VMX_EXIT_TRIPLE_FAULT:
            enmEvent1 = DBGFEVENT_TRIPLE_FAULT;
            //fDtrace1  = VBOXVMM_EXIT_TRIPLE_FAULT_ENABLED();
            break;
        case VMX_EXIT_TASK_SWITCH:      SET_EXIT(TASK_SWITCH); break;
        case VMX_EXIT_EPT_VIOLATION:    SET_EXIT(VMX_EPT_VIOLATION); break;
        case VMX_EXIT_EPT_MISCONFIG:    SET_EXIT(VMX_EPT_MISCONFIG); break;
        case VMX_EXIT_APIC_ACCESS:      SET_EXIT(VMX_VAPIC_ACCESS); break;
        case VMX_EXIT_APIC_WRITE:       SET_EXIT(VMX_VAPIC_WRITE); break;

        /* Instruction specific VM-exits: */
        case VMX_EXIT_CPUID:            SET_BOTH(CPUID); break;
        case VMX_EXIT_GETSEC:           SET_BOTH(GETSEC); break;
        case VMX_EXIT_HLT:              SET_BOTH(HALT); break;
        case VMX_EXIT_INVD:             SET_BOTH(INVD); break;
        case VMX_EXIT_INVLPG:           SET_BOTH(INVLPG); break;
        case VMX_EXIT_RDPMC:            SET_BOTH(RDPMC); break;
        case VMX_EXIT_RDTSC:            SET_BOTH(RDTSC); break;
        case VMX_EXIT_RSM:              SET_BOTH(RSM); break;
        case VMX_EXIT_VMCALL:           SET_BOTH(VMM_CALL); break;
        case VMX_EXIT_VMCLEAR:          SET_BOTH(VMX_VMCLEAR); break;
        case VMX_EXIT_VMLAUNCH:         SET_BOTH(VMX_VMLAUNCH); break;
        case VMX_EXIT_VMPTRLD:          SET_BOTH(VMX_VMPTRLD); break;
        case VMX_EXIT_VMPTRST:          SET_BOTH(VMX_VMPTRST); break;
        case VMX_EXIT_VMREAD:           SET_BOTH(VMX_VMREAD); break;
        case VMX_EXIT_VMRESUME:         SET_BOTH(VMX_VMRESUME); break;
        case VMX_EXIT_VMWRITE:          SET_BOTH(VMX_VMWRITE); break;
        case VMX_EXIT_VMXOFF:           SET_BOTH(VMX_VMXOFF); break;
        case VMX_EXIT_VMXON:            SET_BOTH(VMX_VMXON); break;
        case VMX_EXIT_MOV_CRX:
            hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
            if (VMX_EXIT_QUAL_CRX_ACCESS(pVmxTransient->uExitQual) == VMX_EXIT_QUAL_CRX_ACCESS_READ)
                SET_BOTH(CRX_READ);
            else
                SET_BOTH(CRX_WRITE);
            uEventArg = VMX_EXIT_QUAL_CRX_REGISTER(pVmxTransient->uExitQual);
            break;
        case VMX_EXIT_MOV_DRX:
            hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
            if (   VMX_EXIT_QUAL_DRX_DIRECTION(pVmxTransient->uExitQual)
                == VMX_EXIT_QUAL_DRX_DIRECTION_READ)
                SET_BOTH(DRX_READ);
            else
                SET_BOTH(DRX_WRITE);
            uEventArg = VMX_EXIT_QUAL_DRX_REGISTER(pVmxTransient->uExitQual);
            break;
        case VMX_EXIT_RDMSR:            SET_BOTH(RDMSR); break;
        case VMX_EXIT_WRMSR:            SET_BOTH(WRMSR); break;
        case VMX_EXIT_MWAIT:            SET_BOTH(MWAIT); break;
        case VMX_EXIT_MONITOR:          SET_BOTH(MONITOR); break;
        case VMX_EXIT_PAUSE:            SET_BOTH(PAUSE); break;
        case VMX_EXIT_GDTR_IDTR_ACCESS:
            hmR0VmxReadExitInstrInfoVmcs(pVmxTransient);
            switch (RT_BF_GET(pVmxTransient->ExitInstrInfo.u, VMX_BF_XDTR_INSINFO_INSTR_ID))
            {
                case VMX_XDTR_INSINFO_II_SGDT: SET_BOTH(SGDT); break;
                case VMX_XDTR_INSINFO_II_SIDT: SET_BOTH(SIDT); break;
                case VMX_XDTR_INSINFO_II_LGDT: SET_BOTH(LGDT); break;
                case VMX_XDTR_INSINFO_II_LIDT: SET_BOTH(LIDT); break;
            }
            break;

        case VMX_EXIT_LDTR_TR_ACCESS:
            hmR0VmxReadExitInstrInfoVmcs(pVmxTransient);
            switch (RT_BF_GET(pVmxTransient->ExitInstrInfo.u, VMX_BF_YYTR_INSINFO_INSTR_ID))
            {
                case VMX_YYTR_INSINFO_II_SLDT: SET_BOTH(SLDT); break;
                case VMX_YYTR_INSINFO_II_STR:  SET_BOTH(STR); break;
                case VMX_YYTR_INSINFO_II_LLDT: SET_BOTH(LLDT); break;
                case VMX_YYTR_INSINFO_II_LTR:  SET_BOTH(LTR); break;
            }
            break;

        case VMX_EXIT_INVEPT:           SET_BOTH(VMX_INVEPT); break;
        case VMX_EXIT_RDTSCP:           SET_BOTH(RDTSCP); break;
        case VMX_EXIT_INVVPID:          SET_BOTH(VMX_INVVPID); break;
        case VMX_EXIT_WBINVD:           SET_BOTH(WBINVD); break;
        case VMX_EXIT_XSETBV:           SET_BOTH(XSETBV); break;
        case VMX_EXIT_RDRAND:           SET_BOTH(RDRAND); break;
        case VMX_EXIT_INVPCID:          SET_BOTH(VMX_INVPCID); break;
        case VMX_EXIT_VMFUNC:           SET_BOTH(VMX_VMFUNC); break;
        case VMX_EXIT_RDSEED:           SET_BOTH(RDSEED); break;
        case VMX_EXIT_XSAVES:           SET_BOTH(XSAVES); break;
        case VMX_EXIT_XRSTORS:          SET_BOTH(XRSTORS); break;

        /* Events that aren't relevant at this point. */
        case VMX_EXIT_EXT_INT:
        case VMX_EXIT_INT_WINDOW:
        case VMX_EXIT_NMI_WINDOW:
        case VMX_EXIT_TPR_BELOW_THRESHOLD:
        case VMX_EXIT_PREEMPT_TIMER:
        case VMX_EXIT_IO_INSTR:
            break;

        /* Errors and unexpected events. */
        case VMX_EXIT_INIT_SIGNAL:
        case VMX_EXIT_SIPI:
        case VMX_EXIT_IO_SMI:
        case VMX_EXIT_SMI:
        case VMX_EXIT_ERR_INVALID_GUEST_STATE:
        case VMX_EXIT_ERR_MSR_LOAD:
        case VMX_EXIT_ERR_MACHINE_CHECK:
        case VMX_EXIT_PML_FULL:
        case VMX_EXIT_VIRTUALIZED_EOI:
            break;

        default:
            AssertMsgFailed(("Unexpected VM-exit=%#x\n", uExitReason));
            break;
    }
#undef SET_BOTH
#undef SET_EXIT

    /*
     * Dtrace tracepoints go first.   We do them here at once so we don't
     * have to copy the guest state saving and stuff a few dozen times.
     * Down side is that we've got to repeat the switch, though this time
     * we use enmEvent since the probes are a subset of what DBGF does.
     */
    if (fDtrace1 || fDtrace2)
    {
        hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
        hmR0VmxImportGuestState(pVCpu, pVmxTransient->pVmcsInfo, HMVMX_CPUMCTX_EXTRN_ALL);
        PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
        switch (enmEvent1)
        {
            /** @todo consider which extra parameters would be helpful for each probe.   */
            case DBGFEVENT_END: break;
            case DBGFEVENT_XCPT_DE:                 VBOXVMM_XCPT_DE(pVCpu, pCtx); break;
            case DBGFEVENT_XCPT_DB:                 VBOXVMM_XCPT_DB(pVCpu, pCtx, pCtx->dr[6]); break;
            case DBGFEVENT_XCPT_BP:                 VBOXVMM_XCPT_BP(pVCpu, pCtx); break;
            case DBGFEVENT_XCPT_OF:                 VBOXVMM_XCPT_OF(pVCpu, pCtx); break;
            case DBGFEVENT_XCPT_BR:                 VBOXVMM_XCPT_BR(pVCpu, pCtx); break;
            case DBGFEVENT_XCPT_UD:                 VBOXVMM_XCPT_UD(pVCpu, pCtx); break;
            case DBGFEVENT_XCPT_NM:                 VBOXVMM_XCPT_NM(pVCpu, pCtx); break;
            case DBGFEVENT_XCPT_DF:                 VBOXVMM_XCPT_DF(pVCpu, pCtx); break;
            case DBGFEVENT_XCPT_TS:                 VBOXVMM_XCPT_TS(pVCpu, pCtx, uEventArg); break;
            case DBGFEVENT_XCPT_NP:                 VBOXVMM_XCPT_NP(pVCpu, pCtx, uEventArg); break;
            case DBGFEVENT_XCPT_SS:                 VBOXVMM_XCPT_SS(pVCpu, pCtx, uEventArg); break;
            case DBGFEVENT_XCPT_GP:                 VBOXVMM_XCPT_GP(pVCpu, pCtx, uEventArg); break;
            case DBGFEVENT_XCPT_PF:                 VBOXVMM_XCPT_PF(pVCpu, pCtx, uEventArg, pCtx->cr2); break;
            case DBGFEVENT_XCPT_MF:                 VBOXVMM_XCPT_MF(pVCpu, pCtx); break;
            case DBGFEVENT_XCPT_AC:                 VBOXVMM_XCPT_AC(pVCpu, pCtx); break;
            case DBGFEVENT_XCPT_XF:                 VBOXVMM_XCPT_XF(pVCpu, pCtx); break;
            case DBGFEVENT_XCPT_VE:                 VBOXVMM_XCPT_VE(pVCpu, pCtx); break;
            case DBGFEVENT_XCPT_SX:                 VBOXVMM_XCPT_SX(pVCpu, pCtx, uEventArg); break;
            case DBGFEVENT_INTERRUPT_SOFTWARE:      VBOXVMM_INT_SOFTWARE(pVCpu, pCtx, (uint8_t)uEventArg); break;
            case DBGFEVENT_INSTR_CPUID:             VBOXVMM_INSTR_CPUID(pVCpu, pCtx, pCtx->eax, pCtx->ecx); break;
            case DBGFEVENT_INSTR_GETSEC:            VBOXVMM_INSTR_GETSEC(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_HALT:              VBOXVMM_INSTR_HALT(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_INVD:              VBOXVMM_INSTR_INVD(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_INVLPG:            VBOXVMM_INSTR_INVLPG(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_RDPMC:             VBOXVMM_INSTR_RDPMC(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_RDTSC:             VBOXVMM_INSTR_RDTSC(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_RSM:               VBOXVMM_INSTR_RSM(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_CRX_READ:          VBOXVMM_INSTR_CRX_READ(pVCpu, pCtx, (uint8_t)uEventArg); break;
            case DBGFEVENT_INSTR_CRX_WRITE:         VBOXVMM_INSTR_CRX_WRITE(pVCpu, pCtx, (uint8_t)uEventArg); break;
            case DBGFEVENT_INSTR_DRX_READ:          VBOXVMM_INSTR_DRX_READ(pVCpu, pCtx, (uint8_t)uEventArg); break;
            case DBGFEVENT_INSTR_DRX_WRITE:         VBOXVMM_INSTR_DRX_WRITE(pVCpu, pCtx, (uint8_t)uEventArg); break;
            case DBGFEVENT_INSTR_RDMSR:             VBOXVMM_INSTR_RDMSR(pVCpu, pCtx, pCtx->ecx); break;
            case DBGFEVENT_INSTR_WRMSR:             VBOXVMM_INSTR_WRMSR(pVCpu, pCtx, pCtx->ecx,
                                                                        RT_MAKE_U64(pCtx->eax, pCtx->edx)); break;
            case DBGFEVENT_INSTR_MWAIT:             VBOXVMM_INSTR_MWAIT(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_MONITOR:           VBOXVMM_INSTR_MONITOR(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_PAUSE:             VBOXVMM_INSTR_PAUSE(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_SGDT:              VBOXVMM_INSTR_SGDT(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_SIDT:              VBOXVMM_INSTR_SIDT(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_LGDT:              VBOXVMM_INSTR_LGDT(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_LIDT:              VBOXVMM_INSTR_LIDT(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_SLDT:              VBOXVMM_INSTR_SLDT(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_STR:               VBOXVMM_INSTR_STR(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_LLDT:              VBOXVMM_INSTR_LLDT(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_LTR:               VBOXVMM_INSTR_LTR(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_RDTSCP:            VBOXVMM_INSTR_RDTSCP(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_WBINVD:            VBOXVMM_INSTR_WBINVD(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_XSETBV:            VBOXVMM_INSTR_XSETBV(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_RDRAND:            VBOXVMM_INSTR_RDRAND(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_RDSEED:            VBOXVMM_INSTR_RDSEED(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_XSAVES:            VBOXVMM_INSTR_XSAVES(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_XRSTORS:           VBOXVMM_INSTR_XRSTORS(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_VMM_CALL:          VBOXVMM_INSTR_VMM_CALL(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_VMX_VMCLEAR:       VBOXVMM_INSTR_VMX_VMCLEAR(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_VMX_VMLAUNCH:      VBOXVMM_INSTR_VMX_VMLAUNCH(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_VMX_VMPTRLD:       VBOXVMM_INSTR_VMX_VMPTRLD(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_VMX_VMPTRST:       VBOXVMM_INSTR_VMX_VMPTRST(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_VMX_VMREAD:        VBOXVMM_INSTR_VMX_VMREAD(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_VMX_VMRESUME:      VBOXVMM_INSTR_VMX_VMRESUME(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_VMX_VMWRITE:       VBOXVMM_INSTR_VMX_VMWRITE(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_VMX_VMXOFF:        VBOXVMM_INSTR_VMX_VMXOFF(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_VMX_VMXON:         VBOXVMM_INSTR_VMX_VMXON(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_VMX_INVEPT:        VBOXVMM_INSTR_VMX_INVEPT(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_VMX_INVVPID:       VBOXVMM_INSTR_VMX_INVVPID(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_VMX_INVPCID:       VBOXVMM_INSTR_VMX_INVPCID(pVCpu, pCtx); break;
            case DBGFEVENT_INSTR_VMX_VMFUNC:        VBOXVMM_INSTR_VMX_VMFUNC(pVCpu, pCtx); break;
            default: AssertMsgFailed(("enmEvent1=%d uExitReason=%d\n", enmEvent1, uExitReason)); break;
        }
        switch (enmEvent2)
        {
            /** @todo consider which extra parameters would be helpful for each probe. */
            case DBGFEVENT_END: break;
            case DBGFEVENT_EXIT_TASK_SWITCH:        VBOXVMM_EXIT_TASK_SWITCH(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_CPUID:              VBOXVMM_EXIT_CPUID(pVCpu, pCtx, pCtx->eax, pCtx->ecx); break;
            case DBGFEVENT_EXIT_GETSEC:             VBOXVMM_EXIT_GETSEC(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_HALT:               VBOXVMM_EXIT_HALT(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_INVD:               VBOXVMM_EXIT_INVD(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_INVLPG:             VBOXVMM_EXIT_INVLPG(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_RDPMC:              VBOXVMM_EXIT_RDPMC(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_RDTSC:              VBOXVMM_EXIT_RDTSC(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_RSM:                VBOXVMM_EXIT_RSM(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_CRX_READ:           VBOXVMM_EXIT_CRX_READ(pVCpu, pCtx, (uint8_t)uEventArg); break;
            case DBGFEVENT_EXIT_CRX_WRITE:          VBOXVMM_EXIT_CRX_WRITE(pVCpu, pCtx, (uint8_t)uEventArg); break;
            case DBGFEVENT_EXIT_DRX_READ:           VBOXVMM_EXIT_DRX_READ(pVCpu, pCtx, (uint8_t)uEventArg); break;
            case DBGFEVENT_EXIT_DRX_WRITE:          VBOXVMM_EXIT_DRX_WRITE(pVCpu, pCtx, (uint8_t)uEventArg); break;
            case DBGFEVENT_EXIT_RDMSR:              VBOXVMM_EXIT_RDMSR(pVCpu, pCtx, pCtx->ecx); break;
            case DBGFEVENT_EXIT_WRMSR:              VBOXVMM_EXIT_WRMSR(pVCpu, pCtx, pCtx->ecx,
                                                                       RT_MAKE_U64(pCtx->eax, pCtx->edx)); break;
            case DBGFEVENT_EXIT_MWAIT:              VBOXVMM_EXIT_MWAIT(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_MONITOR:            VBOXVMM_EXIT_MONITOR(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_PAUSE:              VBOXVMM_EXIT_PAUSE(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_SGDT:               VBOXVMM_EXIT_SGDT(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_SIDT:               VBOXVMM_EXIT_SIDT(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_LGDT:               VBOXVMM_EXIT_LGDT(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_LIDT:               VBOXVMM_EXIT_LIDT(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_SLDT:               VBOXVMM_EXIT_SLDT(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_STR:                VBOXVMM_EXIT_STR(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_LLDT:               VBOXVMM_EXIT_LLDT(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_LTR:                VBOXVMM_EXIT_LTR(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_RDTSCP:             VBOXVMM_EXIT_RDTSCP(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_WBINVD:             VBOXVMM_EXIT_WBINVD(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_XSETBV:             VBOXVMM_EXIT_XSETBV(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_RDRAND:             VBOXVMM_EXIT_RDRAND(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_RDSEED:             VBOXVMM_EXIT_RDSEED(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_XSAVES:             VBOXVMM_EXIT_XSAVES(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_XRSTORS:            VBOXVMM_EXIT_XRSTORS(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_VMM_CALL:           VBOXVMM_EXIT_VMM_CALL(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_VMX_VMCLEAR:        VBOXVMM_EXIT_VMX_VMCLEAR(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_VMX_VMLAUNCH:       VBOXVMM_EXIT_VMX_VMLAUNCH(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_VMX_VMPTRLD:        VBOXVMM_EXIT_VMX_VMPTRLD(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_VMX_VMPTRST:        VBOXVMM_EXIT_VMX_VMPTRST(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_VMX_VMREAD:         VBOXVMM_EXIT_VMX_VMREAD(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_VMX_VMRESUME:       VBOXVMM_EXIT_VMX_VMRESUME(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_VMX_VMWRITE:        VBOXVMM_EXIT_VMX_VMWRITE(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_VMX_VMXOFF:         VBOXVMM_EXIT_VMX_VMXOFF(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_VMX_VMXON:          VBOXVMM_EXIT_VMX_VMXON(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_VMX_INVEPT:         VBOXVMM_EXIT_VMX_INVEPT(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_VMX_INVVPID:        VBOXVMM_EXIT_VMX_INVVPID(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_VMX_INVPCID:        VBOXVMM_EXIT_VMX_INVPCID(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_VMX_VMFUNC:         VBOXVMM_EXIT_VMX_VMFUNC(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_VMX_EPT_MISCONFIG:  VBOXVMM_EXIT_VMX_EPT_MISCONFIG(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_VMX_EPT_VIOLATION:  VBOXVMM_EXIT_VMX_EPT_VIOLATION(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_VMX_VAPIC_ACCESS:   VBOXVMM_EXIT_VMX_VAPIC_ACCESS(pVCpu, pCtx); break;
            case DBGFEVENT_EXIT_VMX_VAPIC_WRITE:    VBOXVMM_EXIT_VMX_VAPIC_WRITE(pVCpu, pCtx); break;
            default: AssertMsgFailed(("enmEvent2=%d uExitReason=%d\n", enmEvent2, uExitReason)); break;
        }
    }

    /*
     * Fire of the DBGF event, if enabled (our check here is just a quick one,
     * the DBGF call will do a full check).
     *
     * Note! DBGF sets DBGFEVENT_INTERRUPT_SOFTWARE in the bitmap.
     * Note! If we have to events, we prioritize the first, i.e. the instruction
     *       one, in order to avoid event nesting.
     */
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    if (   enmEvent1 != DBGFEVENT_END
        && DBGF_IS_EVENT_ENABLED(pVM, enmEvent1))
    {
        hmR0VmxImportGuestState(pVCpu, pVmxTransient->pVmcsInfo, CPUMCTX_EXTRN_CS | CPUMCTX_EXTRN_RIP);
        VBOXSTRICTRC rcStrict = DBGFEventGenericWithArgs(pVM, pVCpu, enmEvent1, DBGFEVENTCTX_HM, 1, uEventArg);
        if (rcStrict != VINF_SUCCESS)
            return rcStrict;
    }
    else if (   enmEvent2 != DBGFEVENT_END
             && DBGF_IS_EVENT_ENABLED(pVM, enmEvent2))
    {
        hmR0VmxImportGuestState(pVCpu, pVmxTransient->pVmcsInfo, CPUMCTX_EXTRN_CS | CPUMCTX_EXTRN_RIP);
        VBOXSTRICTRC rcStrict = DBGFEventGenericWithArgs(pVM, pVCpu, enmEvent2, DBGFEVENTCTX_HM, 1, uEventArg);
        if (rcStrict != VINF_SUCCESS)
            return rcStrict;
    }

    return VINF_SUCCESS;
}


/**
 * Single-stepping VM-exit filtering.
 *
 * This is preprocessing the VM-exits and deciding whether we've gotten far
 * enough to return VINF_EM_DBG_STEPPED already.  If not, normal VM-exit
 * handling is performed.
 *
 * @returns Strict VBox status code (i.e. informational status codes too).
 * @param   pVCpu           The cross context virtual CPU structure of the calling EMT.
 * @param   pVmxTransient   The VMX-transient structure.
 * @param   pDbgState       The debug state.
 */
DECLINLINE(VBOXSTRICTRC) hmR0VmxRunDebugHandleExit(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient, PVMXRUNDBGSTATE pDbgState)
{
    /*
     * Expensive (saves context) generic dtrace VM-exit probe.
     */
    uint32_t const uExitReason = pVmxTransient->uExitReason;
    if (!VBOXVMM_R0_HMVMX_VMEXIT_ENABLED())
    { /* more likely */ }
    else
    {
        hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
        int rc = hmR0VmxImportGuestState(pVCpu, pVmxTransient->pVmcsInfo, HMVMX_CPUMCTX_EXTRN_ALL);
        AssertRC(rc);
        VBOXVMM_R0_HMVMX_VMEXIT(pVCpu, &pVCpu->cpum.GstCtx, pVmxTransient->uExitReason, pVmxTransient->uExitQual);
    }

    /*
     * Check for host NMI, just to get that out of the way.
     */
    if (uExitReason != VMX_EXIT_XCPT_OR_NMI)
    { /* normally likely */ }
    else
    {
        int rc2 = hmR0VmxReadExitIntInfoVmcs(pVmxTransient);
        AssertRCReturn(rc2, rc2);
        uint32_t const uIntType = VMX_EXIT_INT_INFO_TYPE(pVmxTransient->uExitIntInfo);
        if (uIntType == VMX_EXIT_INT_INFO_TYPE_NMI)
            return hmR0VmxExitHostNmi(pVCpu);
    }

    /*
     * Check for single stepping event if we're stepping.
     */
    if (pVCpu->hm.s.fSingleInstruction)
    {
        switch (uExitReason)
        {
            case VMX_EXIT_MTF:
                return hmR0VmxExitMtf(pVCpu, pVmxTransient);

            /* Various events: */
            case VMX_EXIT_XCPT_OR_NMI:
            case VMX_EXIT_EXT_INT:
            case VMX_EXIT_TRIPLE_FAULT:
            case VMX_EXIT_INT_WINDOW:
            case VMX_EXIT_NMI_WINDOW:
            case VMX_EXIT_TASK_SWITCH:
            case VMX_EXIT_TPR_BELOW_THRESHOLD:
            case VMX_EXIT_APIC_ACCESS:
            case VMX_EXIT_EPT_VIOLATION:
            case VMX_EXIT_EPT_MISCONFIG:
            case VMX_EXIT_PREEMPT_TIMER:

            /* Instruction specific VM-exits: */
            case VMX_EXIT_CPUID:
            case VMX_EXIT_GETSEC:
            case VMX_EXIT_HLT:
            case VMX_EXIT_INVD:
            case VMX_EXIT_INVLPG:
            case VMX_EXIT_RDPMC:
            case VMX_EXIT_RDTSC:
            case VMX_EXIT_RSM:
            case VMX_EXIT_VMCALL:
            case VMX_EXIT_VMCLEAR:
            case VMX_EXIT_VMLAUNCH:
            case VMX_EXIT_VMPTRLD:
            case VMX_EXIT_VMPTRST:
            case VMX_EXIT_VMREAD:
            case VMX_EXIT_VMRESUME:
            case VMX_EXIT_VMWRITE:
            case VMX_EXIT_VMXOFF:
            case VMX_EXIT_VMXON:
            case VMX_EXIT_MOV_CRX:
            case VMX_EXIT_MOV_DRX:
            case VMX_EXIT_IO_INSTR:
            case VMX_EXIT_RDMSR:
            case VMX_EXIT_WRMSR:
            case VMX_EXIT_MWAIT:
            case VMX_EXIT_MONITOR:
            case VMX_EXIT_PAUSE:
            case VMX_EXIT_GDTR_IDTR_ACCESS:
            case VMX_EXIT_LDTR_TR_ACCESS:
            case VMX_EXIT_INVEPT:
            case VMX_EXIT_RDTSCP:
            case VMX_EXIT_INVVPID:
            case VMX_EXIT_WBINVD:
            case VMX_EXIT_XSETBV:
            case VMX_EXIT_RDRAND:
            case VMX_EXIT_INVPCID:
            case VMX_EXIT_VMFUNC:
            case VMX_EXIT_RDSEED:
            case VMX_EXIT_XSAVES:
            case VMX_EXIT_XRSTORS:
            {
                int rc = hmR0VmxImportGuestState(pVCpu, pVmxTransient->pVmcsInfo, CPUMCTX_EXTRN_CS | CPUMCTX_EXTRN_RIP);
                AssertRCReturn(rc, rc);
                if (   pVCpu->cpum.GstCtx.rip    != pDbgState->uRipStart
                    || pVCpu->cpum.GstCtx.cs.Sel != pDbgState->uCsStart)
                    return VINF_EM_DBG_STEPPED;
                break;
            }

            /* Errors and unexpected events: */
            case VMX_EXIT_INIT_SIGNAL:
            case VMX_EXIT_SIPI:
            case VMX_EXIT_IO_SMI:
            case VMX_EXIT_SMI:
            case VMX_EXIT_ERR_INVALID_GUEST_STATE:
            case VMX_EXIT_ERR_MSR_LOAD:
            case VMX_EXIT_ERR_MACHINE_CHECK:
            case VMX_EXIT_PML_FULL:
            case VMX_EXIT_VIRTUALIZED_EOI:
            case VMX_EXIT_APIC_WRITE:  /* Some talk about this being fault like, so I guess we must process it? */
                break;

            default:
                AssertMsgFailed(("Unexpected VM-exit=%#x\n", uExitReason));
                break;
        }
    }

    /*
     * Check for debugger event breakpoints and dtrace probes.
     */
    if (   uExitReason < RT_ELEMENTS(pDbgState->bmExitsToCheck) * 32U
        && ASMBitTest(pDbgState->bmExitsToCheck, uExitReason) )
    {
        VBOXSTRICTRC rcStrict = hmR0VmxHandleExitDtraceEvents(pVCpu, pVmxTransient, uExitReason);
        if (rcStrict != VINF_SUCCESS)
            return rcStrict;
    }

    /*
     * Normal processing.
     */
#ifdef HMVMX_USE_FUNCTION_TABLE
    return g_apfnVMExitHandlers[uExitReason](pVCpu, pVmxTransient);
#else
    return hmR0VmxHandleExit(pVCpu, pVmxTransient, uExitReason);
#endif
}


/**
 * Single steps guest code using hardware-assisted VMX.
 *
 * This is -not- the same as the guest single-stepping itself (say using EFLAGS.TF)
 * but single-stepping through the hypervisor debugger.
 *
 * @returns Strict VBox status code (i.e. informational status codes too).
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pcLoops     Pointer to the number of executed loops.
 *
 * @note    Mostly the same as hmR0VmxRunGuestCodeNormal().
 */
static VBOXSTRICTRC hmR0VmxRunGuestCodeDebug(PVMCPU pVCpu, uint32_t *pcLoops)
{
    uint32_t const cMaxResumeLoops = pVCpu->CTX_SUFF(pVM)->hm.s.cMaxResumeLoops;
    Assert(pcLoops);
    Assert(*pcLoops <= cMaxResumeLoops);

    VMXTRANSIENT VmxTransient;
    RT_ZERO(VmxTransient);
    VmxTransient.pVmcsInfo = hmGetVmxActiveVmcsInfo(pVCpu);

    /* Set HMCPU indicators.  */
    bool const fSavedSingleInstruction = pVCpu->hm.s.fSingleInstruction;
    pVCpu->hm.s.fSingleInstruction     = pVCpu->hm.s.fSingleInstruction || DBGFIsStepping(pVCpu);
    pVCpu->hm.s.fDebugWantRdTscExit    = false;
    pVCpu->hm.s.fUsingDebugLoop        = true;

    /* State we keep to help modify and later restore the VMCS fields we alter, and for detecting steps.  */
    VMXRUNDBGSTATE DbgState;
    hmR0VmxRunDebugStateInit(pVCpu, &VmxTransient, &DbgState);
    hmR0VmxPreRunGuestDebugStateUpdate(pVCpu, &VmxTransient, &DbgState);

    /*
     * The loop.
     */
    VBOXSTRICTRC rcStrict  = VERR_INTERNAL_ERROR_5;
    for (;;)
    {
        Assert(!HMR0SuspendPending());
        HMVMX_ASSERT_CPU_SAFE(pVCpu);
        STAM_PROFILE_ADV_START(&pVCpu->hm.s.StatEntry, x);
        bool fStepping = pVCpu->hm.s.fSingleInstruction;

        /* Set up VM-execution controls the next two can respond to. */
        hmR0VmxPreRunGuestDebugStateApply(pVCpu, &VmxTransient, &DbgState);

        /*
         * Preparatory work for running guest code, this may force us to
         * return to ring-3.
         *
         * Warning! This bugger disables interrupts on VINF_SUCCESS!
         */
        rcStrict = hmR0VmxPreRunGuest(pVCpu, &VmxTransient, fStepping);
        if (rcStrict != VINF_SUCCESS)
            break;

        /* Interrupts are disabled at this point! */
        hmR0VmxPreRunGuestCommitted(pVCpu, &VmxTransient);

        /* Override any obnoxious code in the above two calls. */
        hmR0VmxPreRunGuestDebugStateApply(pVCpu, &VmxTransient, &DbgState);

        /*
         * Finally execute the guest.
         */
        int rcRun = hmR0VmxRunGuest(pVCpu, &VmxTransient);

        hmR0VmxPostRunGuest(pVCpu, &VmxTransient, rcRun);
        /* Interrupts are re-enabled at this point! */

        /* Check for errors with running the VM (VMLAUNCH/VMRESUME). */
        if (RT_SUCCESS(rcRun))
        { /* very likely */ }
        else
        {
            STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatPreExit, x);
            hmR0VmxReportWorldSwitchError(pVCpu, rcRun, &VmxTransient);
            return rcRun;
        }

        /* Profile the VM-exit. */
        AssertMsg(VmxTransient.uExitReason <= VMX_EXIT_MAX, ("%#x\n", VmxTransient.uExitReason));
        STAM_COUNTER_INC(&pVCpu->hm.s.StatExitAll);
        STAM_COUNTER_INC(&pVCpu->hm.s.paStatExitReasonR0[VmxTransient.uExitReason & MASK_EXITREASON_STAT]);
        STAM_PROFILE_ADV_STOP_START(&pVCpu->hm.s.StatPreExit, &pVCpu->hm.s.StatExitHandling, x);
        HMVMX_START_EXIT_DISPATCH_PROF();

        VBOXVMM_R0_HMVMX_VMEXIT_NOCTX(pVCpu, &pVCpu->cpum.GstCtx, VmxTransient.uExitReason);

        /*
         * Handle the VM-exit - we quit earlier on certain VM-exits, see hmR0VmxHandleExitDebug().
         */
        rcStrict = hmR0VmxRunDebugHandleExit(pVCpu, &VmxTransient, &DbgState);
        STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatExitHandling, x);
        if (rcStrict != VINF_SUCCESS)
            break;
        if (++(*pcLoops) > cMaxResumeLoops)
        {
            STAM_COUNTER_INC(&pVCpu->hm.s.StatSwitchMaxResumeLoops);
            rcStrict = VINF_EM_RAW_INTERRUPT;
            break;
        }

        /*
         * Stepping: Did the RIP change, if so, consider it a single step.
         * Otherwise, make sure one of the TFs gets set.
         */
        if (fStepping)
        {
            int rc = hmR0VmxImportGuestState(pVCpu, VmxTransient.pVmcsInfo, CPUMCTX_EXTRN_CS | CPUMCTX_EXTRN_RIP);
            AssertRC(rc);
            if (   pVCpu->cpum.GstCtx.rip    != DbgState.uRipStart
                || pVCpu->cpum.GstCtx.cs.Sel != DbgState.uCsStart)
            {
                rcStrict = VINF_EM_DBG_STEPPED;
                break;
            }
            ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_DR7);
        }

        /*
         * Update when dtrace settings changes (DBGF kicks us, so no need to check).
         */
        if (VBOXVMM_GET_SETTINGS_SEQ_NO() != DbgState.uDtraceSettingsSeqNo)
            hmR0VmxPreRunGuestDebugStateUpdate(pVCpu, &VmxTransient, &DbgState);
    }

    /*
     * Clear the X86_EFL_TF if necessary.
     */
    if (pVCpu->hm.s.fClearTrapFlag)
    {
        int rc = hmR0VmxImportGuestState(pVCpu, VmxTransient.pVmcsInfo, CPUMCTX_EXTRN_RFLAGS);
        AssertRC(rc);
        pVCpu->hm.s.fClearTrapFlag = false;
        pVCpu->cpum.GstCtx.eflags.Bits.u1TF = 0;
    }
    /** @todo there seems to be issues with the resume flag when the monitor trap
     *        flag is pending without being used. Seen early in bios init when
     *        accessing APIC page in protected mode. */

    /*
     * Restore VM-exit control settings as we may not re-enter this function the
     * next time around.
     */
    rcStrict = hmR0VmxRunDebugStateRevert(pVCpu, &VmxTransient, &DbgState, rcStrict);

    /* Restore HMCPU indicators. */
    pVCpu->hm.s.fUsingDebugLoop     = false;
    pVCpu->hm.s.fDebugWantRdTscExit = false;
    pVCpu->hm.s.fSingleInstruction  = fSavedSingleInstruction;

    STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatEntry, x);
    return rcStrict;
}


/** @} */


/**
 * Checks if any expensive dtrace probes are enabled and we should go to the
 * debug loop.
 *
 * @returns true if we should use debug loop, false if not.
 */
static bool hmR0VmxAnyExpensiveProbesEnabled(void)
{
    /* It's probably faster to OR the raw 32-bit counter variables together.
       Since the variables are in an array and the probes are next to one
       another (more or less), we have good locality.  So, better read
       eight-nine cache lines ever time and only have one conditional, than
       128+ conditionals, right? */
    return (  VBOXVMM_R0_HMVMX_VMEXIT_ENABLED_RAW() /* expensive too due to context */
            | VBOXVMM_XCPT_DE_ENABLED_RAW()
            | VBOXVMM_XCPT_DB_ENABLED_RAW()
            | VBOXVMM_XCPT_BP_ENABLED_RAW()
            | VBOXVMM_XCPT_OF_ENABLED_RAW()
            | VBOXVMM_XCPT_BR_ENABLED_RAW()
            | VBOXVMM_XCPT_UD_ENABLED_RAW()
            | VBOXVMM_XCPT_NM_ENABLED_RAW()
            | VBOXVMM_XCPT_DF_ENABLED_RAW()
            | VBOXVMM_XCPT_TS_ENABLED_RAW()
            | VBOXVMM_XCPT_NP_ENABLED_RAW()
            | VBOXVMM_XCPT_SS_ENABLED_RAW()
            | VBOXVMM_XCPT_GP_ENABLED_RAW()
            | VBOXVMM_XCPT_PF_ENABLED_RAW()
            | VBOXVMM_XCPT_MF_ENABLED_RAW()
            | VBOXVMM_XCPT_AC_ENABLED_RAW()
            | VBOXVMM_XCPT_XF_ENABLED_RAW()
            | VBOXVMM_XCPT_VE_ENABLED_RAW()
            | VBOXVMM_XCPT_SX_ENABLED_RAW()
            | VBOXVMM_INT_SOFTWARE_ENABLED_RAW()
            | VBOXVMM_INT_HARDWARE_ENABLED_RAW()
           ) != 0
        || (  VBOXVMM_INSTR_HALT_ENABLED_RAW()
            | VBOXVMM_INSTR_MWAIT_ENABLED_RAW()
            | VBOXVMM_INSTR_MONITOR_ENABLED_RAW()
            | VBOXVMM_INSTR_CPUID_ENABLED_RAW()
            | VBOXVMM_INSTR_INVD_ENABLED_RAW()
            | VBOXVMM_INSTR_WBINVD_ENABLED_RAW()
            | VBOXVMM_INSTR_INVLPG_ENABLED_RAW()
            | VBOXVMM_INSTR_RDTSC_ENABLED_RAW()
            | VBOXVMM_INSTR_RDTSCP_ENABLED_RAW()
            | VBOXVMM_INSTR_RDPMC_ENABLED_RAW()
            | VBOXVMM_INSTR_RDMSR_ENABLED_RAW()
            | VBOXVMM_INSTR_WRMSR_ENABLED_RAW()
            | VBOXVMM_INSTR_CRX_READ_ENABLED_RAW()
            | VBOXVMM_INSTR_CRX_WRITE_ENABLED_RAW()
            | VBOXVMM_INSTR_DRX_READ_ENABLED_RAW()
            | VBOXVMM_INSTR_DRX_WRITE_ENABLED_RAW()
            | VBOXVMM_INSTR_PAUSE_ENABLED_RAW()
            | VBOXVMM_INSTR_XSETBV_ENABLED_RAW()
            | VBOXVMM_INSTR_SIDT_ENABLED_RAW()
            | VBOXVMM_INSTR_LIDT_ENABLED_RAW()
            | VBOXVMM_INSTR_SGDT_ENABLED_RAW()
            | VBOXVMM_INSTR_LGDT_ENABLED_RAW()
            | VBOXVMM_INSTR_SLDT_ENABLED_RAW()
            | VBOXVMM_INSTR_LLDT_ENABLED_RAW()
            | VBOXVMM_INSTR_STR_ENABLED_RAW()
            | VBOXVMM_INSTR_LTR_ENABLED_RAW()
            | VBOXVMM_INSTR_GETSEC_ENABLED_RAW()
            | VBOXVMM_INSTR_RSM_ENABLED_RAW()
            | VBOXVMM_INSTR_RDRAND_ENABLED_RAW()
            | VBOXVMM_INSTR_RDSEED_ENABLED_RAW()
            | VBOXVMM_INSTR_XSAVES_ENABLED_RAW()
            | VBOXVMM_INSTR_XRSTORS_ENABLED_RAW()
            | VBOXVMM_INSTR_VMM_CALL_ENABLED_RAW()
            | VBOXVMM_INSTR_VMX_VMCLEAR_ENABLED_RAW()
            | VBOXVMM_INSTR_VMX_VMLAUNCH_ENABLED_RAW()
            | VBOXVMM_INSTR_VMX_VMPTRLD_ENABLED_RAW()
            | VBOXVMM_INSTR_VMX_VMPTRST_ENABLED_RAW()
            | VBOXVMM_INSTR_VMX_VMREAD_ENABLED_RAW()
            | VBOXVMM_INSTR_VMX_VMRESUME_ENABLED_RAW()
            | VBOXVMM_INSTR_VMX_VMWRITE_ENABLED_RAW()
            | VBOXVMM_INSTR_VMX_VMXOFF_ENABLED_RAW()
            | VBOXVMM_INSTR_VMX_VMXON_ENABLED_RAW()
            | VBOXVMM_INSTR_VMX_VMFUNC_ENABLED_RAW()
            | VBOXVMM_INSTR_VMX_INVEPT_ENABLED_RAW()
            | VBOXVMM_INSTR_VMX_INVVPID_ENABLED_RAW()
            | VBOXVMM_INSTR_VMX_INVPCID_ENABLED_RAW()
           ) != 0
        || (  VBOXVMM_EXIT_TASK_SWITCH_ENABLED_RAW()
            | VBOXVMM_EXIT_HALT_ENABLED_RAW()
            | VBOXVMM_EXIT_MWAIT_ENABLED_RAW()
            | VBOXVMM_EXIT_MONITOR_ENABLED_RAW()
            | VBOXVMM_EXIT_CPUID_ENABLED_RAW()
            | VBOXVMM_EXIT_INVD_ENABLED_RAW()
            | VBOXVMM_EXIT_WBINVD_ENABLED_RAW()
            | VBOXVMM_EXIT_INVLPG_ENABLED_RAW()
            | VBOXVMM_EXIT_RDTSC_ENABLED_RAW()
            | VBOXVMM_EXIT_RDTSCP_ENABLED_RAW()
            | VBOXVMM_EXIT_RDPMC_ENABLED_RAW()
            | VBOXVMM_EXIT_RDMSR_ENABLED_RAW()
            | VBOXVMM_EXIT_WRMSR_ENABLED_RAW()
            | VBOXVMM_EXIT_CRX_READ_ENABLED_RAW()
            | VBOXVMM_EXIT_CRX_WRITE_ENABLED_RAW()
            | VBOXVMM_EXIT_DRX_READ_ENABLED_RAW()
            | VBOXVMM_EXIT_DRX_WRITE_ENABLED_RAW()
            | VBOXVMM_EXIT_PAUSE_ENABLED_RAW()
            | VBOXVMM_EXIT_XSETBV_ENABLED_RAW()
            | VBOXVMM_EXIT_SIDT_ENABLED_RAW()
            | VBOXVMM_EXIT_LIDT_ENABLED_RAW()
            | VBOXVMM_EXIT_SGDT_ENABLED_RAW()
            | VBOXVMM_EXIT_LGDT_ENABLED_RAW()
            | VBOXVMM_EXIT_SLDT_ENABLED_RAW()
            | VBOXVMM_EXIT_LLDT_ENABLED_RAW()
            | VBOXVMM_EXIT_STR_ENABLED_RAW()
            | VBOXVMM_EXIT_LTR_ENABLED_RAW()
            | VBOXVMM_EXIT_GETSEC_ENABLED_RAW()
            | VBOXVMM_EXIT_RSM_ENABLED_RAW()
            | VBOXVMM_EXIT_RDRAND_ENABLED_RAW()
            | VBOXVMM_EXIT_RDSEED_ENABLED_RAW()
            | VBOXVMM_EXIT_XSAVES_ENABLED_RAW()
            | VBOXVMM_EXIT_XRSTORS_ENABLED_RAW()
            | VBOXVMM_EXIT_VMM_CALL_ENABLED_RAW()
            | VBOXVMM_EXIT_VMX_VMCLEAR_ENABLED_RAW()
            | VBOXVMM_EXIT_VMX_VMLAUNCH_ENABLED_RAW()
            | VBOXVMM_EXIT_VMX_VMPTRLD_ENABLED_RAW()
            | VBOXVMM_EXIT_VMX_VMPTRST_ENABLED_RAW()
            | VBOXVMM_EXIT_VMX_VMREAD_ENABLED_RAW()
            | VBOXVMM_EXIT_VMX_VMRESUME_ENABLED_RAW()
            | VBOXVMM_EXIT_VMX_VMWRITE_ENABLED_RAW()
            | VBOXVMM_EXIT_VMX_VMXOFF_ENABLED_RAW()
            | VBOXVMM_EXIT_VMX_VMXON_ENABLED_RAW()
            | VBOXVMM_EXIT_VMX_VMFUNC_ENABLED_RAW()
            | VBOXVMM_EXIT_VMX_INVEPT_ENABLED_RAW()
            | VBOXVMM_EXIT_VMX_INVVPID_ENABLED_RAW()
            | VBOXVMM_EXIT_VMX_INVPCID_ENABLED_RAW()
            | VBOXVMM_EXIT_VMX_EPT_VIOLATION_ENABLED_RAW()
            | VBOXVMM_EXIT_VMX_EPT_MISCONFIG_ENABLED_RAW()
            | VBOXVMM_EXIT_VMX_VAPIC_ACCESS_ENABLED_RAW()
            | VBOXVMM_EXIT_VMX_VAPIC_WRITE_ENABLED_RAW()
           ) != 0;
}


/**
 * Runs the guest using hardware-assisted VMX.
 *
 * @returns Strict VBox status code (i.e. informational status codes too).
 * @param   pVCpu       The cross context virtual CPU structure.
 */
VMMR0DECL(VBOXSTRICTRC) VMXR0RunGuestCode(PVMCPU pVCpu)
{
    AssertPtr(pVCpu);
    PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
    Assert(VMMRZCallRing3IsEnabled(pVCpu));
    Assert(!ASMAtomicUoReadU64(&pCtx->fExtrn));
    HMVMX_ASSERT_PREEMPT_SAFE(pVCpu);

    VMMRZCallRing3SetNotification(pVCpu, hmR0VmxCallRing3Callback, pCtx);

    VBOXSTRICTRC rcStrict;
    uint32_t cLoops = 0;
#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
    bool const fInNestedGuestMode = CPUMIsGuestInVmxNonRootMode(pCtx);
#else
    bool const fInNestedGuestMode = false;
#endif
    if (!fInNestedGuestMode)
    {
        if (   !pVCpu->hm.s.fUseDebugLoop
            && (!VBOXVMM_ANY_PROBES_ENABLED() || !hmR0VmxAnyExpensiveProbesEnabled())
            && !DBGFIsStepping(pVCpu)
            && !pVCpu->CTX_SUFF(pVM)->dbgf.ro.cEnabledInt3Breakpoints)
            rcStrict = hmR0VmxRunGuestCodeNormal(pVCpu, &cLoops);
        else
            rcStrict = hmR0VmxRunGuestCodeDebug(pVCpu, &cLoops);
    }
#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
    else
        rcStrict = VINF_VMX_VMLAUNCH_VMRESUME;

    if (rcStrict == VINF_VMX_VMLAUNCH_VMRESUME)
        rcStrict = hmR0VmxRunGuestCodeNested(pVCpu, &cLoops);
#endif

    int const rcLoop = VBOXSTRICTRC_VAL(rcStrict);
    switch (rcLoop)
    {
        case VINF_VMX_VMEXIT:       rcStrict = VINF_SUCCESS;                break;
        case VERR_EM_INTERPRETER:   rcStrict = VINF_EM_RAW_EMULATE_INSTR;   break;
        case VINF_EM_RESET:         rcStrict = VINF_EM_TRIPLE_FAULT;        break;
    }

    int rc2 = hmR0VmxExitToRing3(pVCpu, rcStrict);
    if (RT_FAILURE(rc2))
    {
        pVCpu->hm.s.u32HMError = (uint32_t)VBOXSTRICTRC_VAL(rcStrict);
        rcStrict = rc2;
    }
    Assert(!ASMAtomicUoReadU64(&pCtx->fExtrn));
    Assert(!VMMRZCallRing3IsNotificationSet(pVCpu));
    return rcStrict;
}


#ifndef HMVMX_USE_FUNCTION_TABLE
/**
 * Handles a guest VM-exit from hardware-assisted VMX execution.
 *
 * @returns Strict VBox status code (i.e. informational status codes too).
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 */
DECLINLINE(VBOXSTRICTRC) hmR0VmxHandleExit(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
#ifdef DEBUG_ramshankar
#define VMEXIT_CALL_RET(a_fSave, a_CallExpr) \
       do { \
            if (a_fSave != 0) \
                hmR0VmxImportGuestState(pVCpu, HMVMX_CPUMCTX_EXTRN_ALL); \
            VBOXSTRICTRC rcStrict = a_CallExpr; \
            if (a_fSave != 0) \
                ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_ALL_GUEST); \
            return rcStrict; \
        } while (0)
#else
# define VMEXIT_CALL_RET(a_fSave, a_CallExpr) return a_CallExpr
#endif
    uint32_t const uExitReason = pVmxTransient->uExitReason;
    switch (uExitReason)
    {
        case VMX_EXIT_EPT_MISCONFIG:           VMEXIT_CALL_RET(0, hmR0VmxExitEptMisconfig(pVCpu, pVmxTransient));
        case VMX_EXIT_EPT_VIOLATION:           VMEXIT_CALL_RET(0, hmR0VmxExitEptViolation(pVCpu, pVmxTransient));
        case VMX_EXIT_IO_INSTR:                VMEXIT_CALL_RET(0, hmR0VmxExitIoInstr(pVCpu, pVmxTransient));
        case VMX_EXIT_CPUID:                   VMEXIT_CALL_RET(0, hmR0VmxExitCpuid(pVCpu, pVmxTransient));
        case VMX_EXIT_RDTSC:                   VMEXIT_CALL_RET(0, hmR0VmxExitRdtsc(pVCpu, pVmxTransient));
        case VMX_EXIT_RDTSCP:                  VMEXIT_CALL_RET(0, hmR0VmxExitRdtscp(pVCpu, pVmxTransient));
        case VMX_EXIT_APIC_ACCESS:             VMEXIT_CALL_RET(0, hmR0VmxExitApicAccess(pVCpu, pVmxTransient));
        case VMX_EXIT_XCPT_OR_NMI:             VMEXIT_CALL_RET(0, hmR0VmxExitXcptOrNmi(pVCpu, pVmxTransient));
        case VMX_EXIT_MOV_CRX:                 VMEXIT_CALL_RET(0, hmR0VmxExitMovCRx(pVCpu, pVmxTransient));
        case VMX_EXIT_EXT_INT:                 VMEXIT_CALL_RET(0, hmR0VmxExitExtInt(pVCpu, pVmxTransient));
        case VMX_EXIT_INT_WINDOW:              VMEXIT_CALL_RET(0, hmR0VmxExitIntWindow(pVCpu, pVmxTransient));
        case VMX_EXIT_TPR_BELOW_THRESHOLD:     VMEXIT_CALL_RET(0, hmR0VmxExitTprBelowThreshold(pVCpu, pVmxTransient));
        case VMX_EXIT_MWAIT:                   VMEXIT_CALL_RET(0, hmR0VmxExitMwait(pVCpu, pVmxTransient));
        case VMX_EXIT_MONITOR:                 VMEXIT_CALL_RET(0, hmR0VmxExitMonitor(pVCpu, pVmxTransient));
        case VMX_EXIT_TASK_SWITCH:             VMEXIT_CALL_RET(0, hmR0VmxExitTaskSwitch(pVCpu, pVmxTransient));
        case VMX_EXIT_PREEMPT_TIMER:           VMEXIT_CALL_RET(0, hmR0VmxExitPreemptTimer(pVCpu, pVmxTransient));
        case VMX_EXIT_RDMSR:                   VMEXIT_CALL_RET(0, hmR0VmxExitRdmsr(pVCpu, pVmxTransient));
        case VMX_EXIT_WRMSR:                   VMEXIT_CALL_RET(0, hmR0VmxExitWrmsr(pVCpu, pVmxTransient));
        case VMX_EXIT_VMCALL:                  VMEXIT_CALL_RET(0, hmR0VmxExitVmcall(pVCpu, pVmxTransient));
        case VMX_EXIT_MOV_DRX:                 VMEXIT_CALL_RET(0, hmR0VmxExitMovDRx(pVCpu, pVmxTransient));
        case VMX_EXIT_HLT:                     VMEXIT_CALL_RET(0, hmR0VmxExitHlt(pVCpu, pVmxTransient));
        case VMX_EXIT_INVD:                    VMEXIT_CALL_RET(0, hmR0VmxExitInvd(pVCpu, pVmxTransient));
        case VMX_EXIT_INVLPG:                  VMEXIT_CALL_RET(0, hmR0VmxExitInvlpg(pVCpu, pVmxTransient));
        case VMX_EXIT_MTF:                     VMEXIT_CALL_RET(0, hmR0VmxExitMtf(pVCpu, pVmxTransient));
        case VMX_EXIT_PAUSE:                   VMEXIT_CALL_RET(0, hmR0VmxExitPause(pVCpu, pVmxTransient));
        case VMX_EXIT_WBINVD:                  VMEXIT_CALL_RET(0, hmR0VmxExitWbinvd(pVCpu, pVmxTransient));
        case VMX_EXIT_XSETBV:                  VMEXIT_CALL_RET(0, hmR0VmxExitXsetbv(pVCpu, pVmxTransient));
        case VMX_EXIT_INVPCID:                 VMEXIT_CALL_RET(0, hmR0VmxExitInvpcid(pVCpu, pVmxTransient));
        case VMX_EXIT_GETSEC:                  VMEXIT_CALL_RET(0, hmR0VmxExitGetsec(pVCpu, pVmxTransient));
        case VMX_EXIT_RDPMC:                   VMEXIT_CALL_RET(0, hmR0VmxExitRdpmc(pVCpu, pVmxTransient));
#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
        case VMX_EXIT_VMCLEAR:                 VMEXIT_CALL_RET(0, hmR0VmxExitVmclear(pVCpu, pVmxTransient));
        case VMX_EXIT_VMLAUNCH:                VMEXIT_CALL_RET(0, hmR0VmxExitVmlaunch(pVCpu, pVmxTransient));
        case VMX_EXIT_VMPTRLD:                 VMEXIT_CALL_RET(0, hmR0VmxExitVmptrld(pVCpu, pVmxTransient));
        case VMX_EXIT_VMPTRST:                 VMEXIT_CALL_RET(0, hmR0VmxExitVmptrst(pVCpu, pVmxTransient));
        case VMX_EXIT_VMREAD:                  VMEXIT_CALL_RET(0, hmR0VmxExitVmread(pVCpu, pVmxTransient));
        case VMX_EXIT_VMRESUME:                VMEXIT_CALL_RET(0, hmR0VmxExitVmwrite(pVCpu, pVmxTransient));
        case VMX_EXIT_VMWRITE:                 VMEXIT_CALL_RET(0, hmR0VmxExitVmresume(pVCpu, pVmxTransient));
        case VMX_EXIT_VMXOFF:                  VMEXIT_CALL_RET(0, hmR0VmxExitVmxoff(pVCpu, pVmxTransient));
        case VMX_EXIT_VMXON:                   VMEXIT_CALL_RET(0, hmR0VmxExitVmxon(pVCpu, pVmxTransient));
        case VMX_EXIT_INVVPID:                 VMEXIT_CALL_RET(0, hmR0VmxExitInvvpid(pVCpu, pVmxTransient));
        case VMX_EXIT_INVEPT:                  VMEXIT_CALL_RET(0, hmR0VmxExitSetPendingXcptUD(pVCpu, pVmxTransient));
#else
        case VMX_EXIT_VMCLEAR:
        case VMX_EXIT_VMLAUNCH:
        case VMX_EXIT_VMPTRLD:
        case VMX_EXIT_VMPTRST:
        case VMX_EXIT_VMREAD:
        case VMX_EXIT_VMRESUME:
        case VMX_EXIT_VMWRITE:
        case VMX_EXIT_VMXOFF:
        case VMX_EXIT_VMXON:
        case VMX_EXIT_INVVPID:
        case VMX_EXIT_INVEPT:
            return hmR0VmxExitSetPendingXcptUD(pVCpu, pVmxTransient);
#endif

        case VMX_EXIT_TRIPLE_FAULT:            return hmR0VmxExitTripleFault(pVCpu, pVmxTransient);
        case VMX_EXIT_NMI_WINDOW:              return hmR0VmxExitNmiWindow(pVCpu, pVmxTransient);
        case VMX_EXIT_ERR_INVALID_GUEST_STATE: return hmR0VmxExitErrInvalidGuestState(pVCpu, pVmxTransient);

        case VMX_EXIT_INIT_SIGNAL:
        case VMX_EXIT_SIPI:
        case VMX_EXIT_IO_SMI:
        case VMX_EXIT_SMI:
        case VMX_EXIT_ERR_MSR_LOAD:
        case VMX_EXIT_ERR_MACHINE_CHECK:
        case VMX_EXIT_PML_FULL:
        case VMX_EXIT_VIRTUALIZED_EOI:
        case VMX_EXIT_GDTR_IDTR_ACCESS:
        case VMX_EXIT_LDTR_TR_ACCESS:
        case VMX_EXIT_APIC_WRITE:
        case VMX_EXIT_RDRAND:
        case VMX_EXIT_RSM:
        case VMX_EXIT_VMFUNC:
        case VMX_EXIT_ENCLS:
        case VMX_EXIT_RDSEED:
        case VMX_EXIT_XSAVES:
        case VMX_EXIT_XRSTORS:
        case VMX_EXIT_UMWAIT:
        case VMX_EXIT_TPAUSE:
        default:
            return hmR0VmxExitErrUnexpected(pVCpu, pVmxTransient);
    }
#undef VMEXIT_CALL_RET
}
#endif /* !HMVMX_USE_FUNCTION_TABLE */


#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
/**
 * Handles a nested-guest VM-exit from hardware-assisted VMX execution.
 *
 * @returns Strict VBox status code (i.e. informational status codes too).
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 */
DECLINLINE(VBOXSTRICTRC) hmR0VmxHandleExitNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    uint32_t const uExitReason = pVmxTransient->uExitReason;
    switch (uExitReason)
    {
        case VMX_EXIT_EPT_MISCONFIG:            return hmR0VmxExitEptMisconfig(pVCpu, pVmxTransient);
        case VMX_EXIT_EPT_VIOLATION:            return hmR0VmxExitEptViolation(pVCpu, pVmxTransient);
        case VMX_EXIT_XCPT_OR_NMI:              return hmR0VmxExitXcptOrNmiNested(pVCpu, pVmxTransient);
        case VMX_EXIT_IO_INSTR:                 return hmR0VmxExitIoInstrNested(pVCpu, pVmxTransient);
        case VMX_EXIT_HLT:                      return hmR0VmxExitHltNested(pVCpu, pVmxTransient);

        /*
         * We shouldn't direct host physical interrupts to the nested-guest.
         */
        case VMX_EXIT_EXT_INT:
            return hmR0VmxExitExtInt(pVCpu, pVmxTransient);

        /*
         * Instructions that cause VM-exits unconditionally or the condition is
         * always is taken solely from the guest hypervisor (meaning if the VM-exit
         * happens, it's guaranteed to be a nested-guest VM-exit).
         *
         *   - Provides VM-exit instruction length ONLY.
         */
        case VMX_EXIT_CPUID:              /* Unconditional. */
        case VMX_EXIT_VMCALL:
        case VMX_EXIT_GETSEC:
        case VMX_EXIT_INVD:
        case VMX_EXIT_XSETBV:
        case VMX_EXIT_VMLAUNCH:
        case VMX_EXIT_VMRESUME:
        case VMX_EXIT_VMXOFF:
        case VMX_EXIT_ENCLS:              /* Condition specified solely by guest hypervisor. */
        case VMX_EXIT_VMFUNC:
            return hmR0VmxExitInstrNested(pVCpu, pVmxTransient);

        /*
         * Instructions that cause VM-exits unconditionally or the condition is
         * always is taken solely from the guest hypervisor (meaning if the VM-exit
         * happens, it's guaranteed to be a nested-guest VM-exit).
         *
         *   - Provides VM-exit instruction length.
         *   - Provides VM-exit information.
         *   - Optionally provides Exit qualification.
         *
         * Since Exit qualification is 0 for all VM-exits where it is not
         * applicable, reading and passing it to the guest should produce
         * defined behavior.
         *
         * See Intel spec. 27.2.1 "Basic VM-Exit Information".
         */
        case VMX_EXIT_INVEPT:             /* Unconditional. */
        case VMX_EXIT_INVVPID:
        case VMX_EXIT_VMCLEAR:
        case VMX_EXIT_VMPTRLD:
        case VMX_EXIT_VMPTRST:
        case VMX_EXIT_VMXON:
        case VMX_EXIT_GDTR_IDTR_ACCESS:   /* Condition specified solely by guest hypervisor. */
        case VMX_EXIT_LDTR_TR_ACCESS:
        case VMX_EXIT_RDRAND:
        case VMX_EXIT_RDSEED:
        case VMX_EXIT_XSAVES:
        case VMX_EXIT_XRSTORS:
        case VMX_EXIT_UMWAIT:
        case VMX_EXIT_TPAUSE:
            return hmR0VmxExitInstrWithInfoNested(pVCpu, pVmxTransient);

        case VMX_EXIT_RDTSC:                    return hmR0VmxExitRdtscNested(pVCpu, pVmxTransient);
        case VMX_EXIT_RDTSCP:                   return hmR0VmxExitRdtscpNested(pVCpu, pVmxTransient);
        case VMX_EXIT_RDMSR:                    return hmR0VmxExitRdmsrNested(pVCpu, pVmxTransient);
        case VMX_EXIT_WRMSR:                    return hmR0VmxExitWrmsrNested(pVCpu, pVmxTransient);
        case VMX_EXIT_INVLPG:                   return hmR0VmxExitInvlpgNested(pVCpu, pVmxTransient);
        case VMX_EXIT_INVPCID:                  return hmR0VmxExitInvpcidNested(pVCpu, pVmxTransient);
        case VMX_EXIT_TASK_SWITCH:              return hmR0VmxExitTaskSwitchNested(pVCpu, pVmxTransient);
        case VMX_EXIT_WBINVD:                   return hmR0VmxExitWbinvdNested(pVCpu, pVmxTransient);
        case VMX_EXIT_MTF:                      return hmR0VmxExitMtfNested(pVCpu, pVmxTransient);
        case VMX_EXIT_APIC_ACCESS:              return hmR0VmxExitApicAccessNested(pVCpu, pVmxTransient);
        case VMX_EXIT_APIC_WRITE:               return hmR0VmxExitApicWriteNested(pVCpu, pVmxTransient);
        case VMX_EXIT_VIRTUALIZED_EOI:          return hmR0VmxExitVirtEoiNested(pVCpu, pVmxTransient);
        case VMX_EXIT_MOV_CRX:                  return hmR0VmxExitMovCRxNested(pVCpu, pVmxTransient);
        case VMX_EXIT_INT_WINDOW:               return hmR0VmxExitIntWindowNested(pVCpu, pVmxTransient);
        case VMX_EXIT_NMI_WINDOW:               return hmR0VmxExitNmiWindowNested(pVCpu, pVmxTransient);
        case VMX_EXIT_TPR_BELOW_THRESHOLD:      return hmR0VmxExitTprBelowThresholdNested(pVCpu, pVmxTransient);
        case VMX_EXIT_MWAIT:                    return hmR0VmxExitMwaitNested(pVCpu, pVmxTransient);
        case VMX_EXIT_MONITOR:                  return hmR0VmxExitMonitorNested(pVCpu, pVmxTransient);
        case VMX_EXIT_PAUSE:                    return hmR0VmxExitPauseNested(pVCpu, pVmxTransient);

        case VMX_EXIT_PREEMPT_TIMER:
        {
            /** @todo NSTVMX: Preempt timer. */
            return hmR0VmxExitErrUnexpected(pVCpu, pVmxTransient);
        }

        case VMX_EXIT_MOV_DRX:                  return hmR0VmxExitMovDRxNested(pVCpu, pVmxTransient);
        case VMX_EXIT_RDPMC:                    return hmR0VmxExitRdpmcNested(pVCpu, pVmxTransient);

        case VMX_EXIT_VMREAD:
        case VMX_EXIT_VMWRITE:                  return hmR0VmxExitVmreadVmwriteNested(pVCpu, pVmxTransient);

        case VMX_EXIT_TRIPLE_FAULT:             return hmR0VmxExitTripleFaultNested(pVCpu, pVmxTransient);
        case VMX_EXIT_ERR_INVALID_GUEST_STATE:  return hmR0VmxExitErrInvalidGuestStateNested(pVCpu, pVmxTransient);

        case VMX_EXIT_INIT_SIGNAL:
        case VMX_EXIT_SIPI:
        case VMX_EXIT_IO_SMI:
        case VMX_EXIT_SMI:
        case VMX_EXIT_ERR_MSR_LOAD:
        case VMX_EXIT_ERR_MACHINE_CHECK:
        case VMX_EXIT_PML_FULL:
        case VMX_EXIT_RSM:
        default:
            return hmR0VmxExitErrUnexpected(pVCpu, pVmxTransient);
    }
}
#endif /* VBOX_WITH_NESTED_HWVIRT_VMX */


#ifdef VBOX_STRICT
/* Is there some generic IPRT define for this that are not in Runtime/internal/\* ?? */
# define HMVMX_ASSERT_PREEMPT_CPUID_VAR() \
    RTCPUID const idAssertCpu = RTThreadPreemptIsEnabled(NIL_RTTHREAD) ? NIL_RTCPUID : RTMpCpuId()

# define HMVMX_ASSERT_PREEMPT_CPUID() \
    do { \
         RTCPUID const idAssertCpuNow = RTThreadPreemptIsEnabled(NIL_RTTHREAD) ? NIL_RTCPUID : RTMpCpuId(); \
         AssertMsg(idAssertCpu == idAssertCpuNow,  ("VMX %#x, %#x\n", idAssertCpu, idAssertCpuNow)); \
    } while (0)

# define HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(a_pVCpu, a_pVmxTransient) \
    do { \
        AssertPtr((a_pVCpu)); \
        AssertPtr((a_pVmxTransient)); \
        Assert((a_pVmxTransient)->fVMEntryFailed == false); \
        Assert((a_pVmxTransient)->pVmcsInfo); \
        Assert(ASMIntAreEnabled()); \
        HMVMX_ASSERT_PREEMPT_SAFE(a_pVCpu); \
        HMVMX_ASSERT_PREEMPT_CPUID_VAR(); \
        Log4Func(("vcpu[%RU32] -v-v-v-v-v-v-v-v-v-v-v-v-v-v-v-v-v-v-v-v-v-v-v-v-v-v-v-v-v-v-v-v-v\n", (a_pVCpu)->idCpu)); \
        HMVMX_ASSERT_PREEMPT_SAFE(a_pVCpu); \
        if (VMMR0IsLogFlushDisabled((a_pVCpu))) \
            HMVMX_ASSERT_PREEMPT_CPUID(); \
        HMVMX_STOP_EXIT_DISPATCH_PROF(); \
    } while (0)

# define HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(a_pVCpu, a_pVmxTransient) \
    do { \
        HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(a_pVCpu, a_pVmxTransient); \
        Assert((a_pVmxTransient)->fIsNestedGuest); \
    } while (0)

# define HMVMX_VALIDATE_EXIT_XCPT_HANDLER_PARAMS(a_pVCpu, a_pVmxTransient) \
    do { \
        Log4Func(("\n")); \
    } while (0)
#else
# define HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(a_pVCpu, a_pVmxTransient) \
    do { \
        HMVMX_STOP_EXIT_DISPATCH_PROF(); \
        NOREF((a_pVCpu)); NOREF((a_pVmxTransient)); \
    } while (0)

# define HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(a_pVCpu, a_pVmxTransient) \
    do { HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(a_pVCpu, a_pVmxTransient); } while (0)

# define HMVMX_VALIDATE_EXIT_XCPT_HANDLER_PARAMS(a_pVCpu, a_pVmxTransient)      do { } while (0)
#endif


/**
 * Advances the guest RIP by the specified number of bytes.
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   cbInstr         Number of bytes to advance the RIP by.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(void) hmR0VmxAdvanceGuestRipBy(PVMCPU pVCpu, uint32_t cbInstr)
{
    /* Advance the RIP. */
    pVCpu->cpum.GstCtx.rip += cbInstr;
    ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP);

    /* Update interrupt inhibition. */
    if (   VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_INHIBIT_INTERRUPTS)
        && pVCpu->cpum.GstCtx.rip != EMGetInhibitInterruptsPC(pVCpu))
        VMCPU_FF_CLEAR(pVCpu, VMCPU_FF_INHIBIT_INTERRUPTS);
}


/**
 * Advances the guest RIP after reading it from the VMCS.
 *
 * @returns VBox status code, no informational status codes.
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
static int hmR0VmxAdvanceGuestRip(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxImportGuestState(pVCpu, pVmxTransient->pVmcsInfo, CPUMCTX_EXTRN_RIP | CPUMCTX_EXTRN_RFLAGS);
    AssertRCReturn(rc, rc);

    hmR0VmxAdvanceGuestRipBy(pVCpu, pVmxTransient->cbInstr);
    return VINF_SUCCESS;
}


/**
 * Handle a condition that occurred while delivering an event through the guest
 * IDT.
 *
 * @returns Strict VBox status code (i.e. informational status codes too).
 * @retval  VINF_SUCCESS if we should continue handling the VM-exit.
 * @retval  VINF_HM_DOUBLE_FAULT if a \#DF condition was detected and we ought
 *          to continue execution of the guest which will delivery the \#DF.
 * @retval  VINF_EM_RESET if we detected a triple-fault condition.
 * @retval  VERR_EM_GUEST_CPU_HANG if we detected a guest CPU hang.
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pVmxTransient   The VMX-transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
static VBOXSTRICTRC hmR0VmxCheckExitDueToEventDelivery(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    /* Read the IDT vectoring info. and VM-exit interruption info. */
    {
        int rc = hmR0VmxReadIdtVectoringInfoVmcs(pVmxTransient);
        rc    |= hmR0VmxReadExitIntInfoVmcs(pVmxTransient);
        AssertRCReturn(rc, rc);
    }

    VBOXSTRICTRC   rcStrict    = VINF_SUCCESS;
    uint32_t const uExitVector = VMX_EXIT_INT_INFO_VECTOR(pVmxTransient->uExitIntInfo);
    if (VMX_IDT_VECTORING_INFO_IS_VALID(pVmxTransient->uIdtVectoringInfo))
    {
        uint32_t const uIdtVectorType = VMX_IDT_VECTORING_INFO_TYPE(pVmxTransient->uIdtVectoringInfo);
        uint32_t const uIdtVector     = VMX_IDT_VECTORING_INFO_VECTOR(pVmxTransient->uIdtVectoringInfo);

        /*
         * If the event was a software interrupt (generated with INT n) or a software exception
         * (generated by INT3/INTO) or a privileged software exception (generated by INT1), we
         * can handle the VM-exit and continue guest execution which will re-execute the
         * instruction rather than re-injecting the exception, as that can cause premature
         * trips to ring-3 before injection and involve TRPM which currently has no way of
         * storing that these exceptions were caused by these instructions (ICEBP's #DB poses
         * the problem).
         */
        IEMXCPTRAISE     enmRaise;
        IEMXCPTRAISEINFO fRaiseInfo;
        if (   uIdtVectorType == VMX_IDT_VECTORING_INFO_TYPE_SW_INT
            || uIdtVectorType == VMX_IDT_VECTORING_INFO_TYPE_SW_XCPT
            || uIdtVectorType == VMX_IDT_VECTORING_INFO_TYPE_PRIV_SW_XCPT)
        {
            enmRaise   = IEMXCPTRAISE_REEXEC_INSTR;
            fRaiseInfo = IEMXCPTRAISEINFO_NONE;
        }
        else if (VMX_EXIT_INT_INFO_IS_VALID(pVmxTransient->uExitIntInfo))
        {
            uint32_t const uExitVectorType  = VMX_EXIT_INT_INFO_TYPE(pVmxTransient->uExitIntInfo);
            uint32_t const fIdtVectorFlags  = hmR0VmxGetIemXcptFlags(uIdtVector, uIdtVectorType);
            uint32_t const fExitVectorFlags = hmR0VmxGetIemXcptFlags(uExitVector, uExitVectorType);

            /** @todo Make AssertMsgReturn as just AssertMsg later. */
            AssertMsgReturn(uExitVectorType == VMX_EXIT_INT_INFO_TYPE_HW_XCPT,
                            ("Unexpected VM-exit interruption vector type %#x!\n", uExitVectorType), VERR_VMX_IPE_5);

            enmRaise = IEMEvaluateRecursiveXcpt(pVCpu, fIdtVectorFlags, uIdtVector, fExitVectorFlags, uExitVector, &fRaiseInfo);

            /* Determine a vectoring #PF condition, see comment in hmR0VmxExitXcptPF(). */
            if (fRaiseInfo & (IEMXCPTRAISEINFO_EXT_INT_PF | IEMXCPTRAISEINFO_NMI_PF))
            {
                pVmxTransient->fVectoringPF = true;
                enmRaise = IEMXCPTRAISE_PREV_EVENT;
            }
        }
        else
        {
            /*
             * If an exception or hardware interrupt delivery caused an EPT violation/misconfig or APIC access
             * VM-exit, then the VM-exit interruption-information will not be valid and we end up here.
             * It is sufficient to reflect the original event to the guest after handling the VM-exit.
             */
            Assert(   uIdtVectorType == VMX_IDT_VECTORING_INFO_TYPE_HW_XCPT
                   || uIdtVectorType == VMX_IDT_VECTORING_INFO_TYPE_NMI
                   || uIdtVectorType == VMX_IDT_VECTORING_INFO_TYPE_EXT_INT);
            enmRaise   = IEMXCPTRAISE_PREV_EVENT;
            fRaiseInfo = IEMXCPTRAISEINFO_NONE;
        }

        /*
         * On CPUs that support Virtual NMIs, if this VM-exit (be it an exception or EPT violation/misconfig
         * etc.) occurred while delivering the NMI, we need to clear the block-by-NMI field in the guest
         * interruptibility-state before re-delivering the NMI after handling the VM-exit. Otherwise the
         * subsequent VM-entry would fail.
         *
         * See Intel spec. 30.7.1.2 "Resuming Guest Software after Handling an Exception". See @bugref{7445}.
         */
        if (   uIdtVectorType == VMX_IDT_VECTORING_INFO_TYPE_NMI
            && (   enmRaise   == IEMXCPTRAISE_PREV_EVENT
                || (fRaiseInfo & IEMXCPTRAISEINFO_NMI_PF))
            && hmR0VmxIsPinCtlsSet(pVCpu, pVmxTransient, VMX_PIN_CTLS_VIRT_NMI)
            && CPUMIsGuestNmiBlocking(pVCpu))
        {
            CPUMSetGuestNmiBlocking(pVCpu, false);
        }

        switch (enmRaise)
        {
            case IEMXCPTRAISE_CURRENT_XCPT:
            {
                Log4Func(("IDT: Pending secondary Xcpt: uIdtVectoringInfo=%#RX64 uExitIntInfo=%#RX64\n",
                          pVmxTransient->uIdtVectoringInfo, pVmxTransient->uExitIntInfo));
                Assert(rcStrict == VINF_SUCCESS);
                break;
            }

            case IEMXCPTRAISE_PREV_EVENT:
            {
                uint32_t u32ErrCode;
                if (VMX_IDT_VECTORING_INFO_IS_ERROR_CODE_VALID(pVmxTransient->uIdtVectoringInfo))
                {
                    int rc = hmR0VmxReadIdtVectoringErrorCodeVmcs(pVmxTransient);
                    AssertRCReturn(rc, rc);
                    u32ErrCode = pVmxTransient->uIdtVectoringErrorCode;
                }
                else
                    u32ErrCode = 0;

                /* If uExitVector is #PF, CR2 value will be updated from the VMCS if it's a guest #PF, see hmR0VmxExitXcptPF(). */
                STAM_COUNTER_INC(&pVCpu->hm.s.StatInjectPendingReflect);
                hmR0VmxSetPendingEvent(pVCpu, VMX_ENTRY_INT_INFO_FROM_EXIT_IDT_INFO(pVmxTransient->uIdtVectoringInfo),
                                       0 /* cbInstr */, u32ErrCode, pVCpu->cpum.GstCtx.cr2);

                Log4Func(("IDT: Pending vectoring event %#RX64 Err=%#RX32\n", pVCpu->hm.s.Event.u64IntInfo,
                          pVCpu->hm.s.Event.u32ErrCode));
                Assert(rcStrict == VINF_SUCCESS);
                break;
            }

            case IEMXCPTRAISE_REEXEC_INSTR:
                Assert(rcStrict == VINF_SUCCESS);
                break;

            case IEMXCPTRAISE_DOUBLE_FAULT:
            {
                /*
                 * Determing a vectoring double #PF condition. Used later, when PGM evaluates the
                 * second #PF as a guest #PF (and not a shadow #PF) and needs to be converted into a #DF.
                 */
                if (fRaiseInfo & IEMXCPTRAISEINFO_PF_PF)
                {
                    pVmxTransient->fVectoringDoublePF = true;
                    Log4Func(("IDT: Vectoring double #PF %#RX64 cr2=%#RX64\n", pVCpu->hm.s.Event.u64IntInfo,
                          pVCpu->cpum.GstCtx.cr2));
                    rcStrict = VINF_SUCCESS;
                }
                else
                {
                    STAM_COUNTER_INC(&pVCpu->hm.s.StatInjectPendingReflect);
                    hmR0VmxSetPendingXcptDF(pVCpu);
                    Log4Func(("IDT: Pending vectoring #DF %#RX64 uIdtVector=%#x uExitVector=%#x\n", pVCpu->hm.s.Event.u64IntInfo,
                              uIdtVector, uExitVector));
                    rcStrict = VINF_HM_DOUBLE_FAULT;
                }
                break;
            }

            case IEMXCPTRAISE_TRIPLE_FAULT:
            {
                Log4Func(("IDT: Pending vectoring triple-fault uIdt=%#x uExit=%#x\n", uIdtVector, uExitVector));
                rcStrict = VINF_EM_RESET;
                break;
            }

            case IEMXCPTRAISE_CPU_HANG:
            {
                Log4Func(("IDT: Bad guest! Entering CPU hang. fRaiseInfo=%#x\n", fRaiseInfo));
                rcStrict = VERR_EM_GUEST_CPU_HANG;
                break;
            }

            default:
            {
                AssertMsgFailed(("IDT: vcpu[%RU32] Unexpected/invalid value! enmRaise=%#x\n", pVCpu->idCpu, enmRaise));
                rcStrict = VERR_VMX_IPE_2;
                break;
            }
        }
    }
    else if (   VMX_EXIT_INT_INFO_IS_VALID(pVmxTransient->uExitIntInfo)
             && VMX_EXIT_INT_INFO_IS_NMI_UNBLOCK_IRET(pVmxTransient->uExitIntInfo)
             && uExitVector != X86_XCPT_DF
             && hmR0VmxIsPinCtlsSet(pVCpu, pVmxTransient, VMX_PIN_CTLS_VIRT_NMI))
    {
        /*
         * Execution of IRET caused this fault when NMI blocking was in effect (i.e we're in the guest NMI handler).
         * We need to set the block-by-NMI field so that NMIs remain blocked until the IRET execution is restarted.
         * See Intel spec. 30.7.1.2 "Resuming guest software after handling an exception".
         */
        CPUMSetGuestNmiBlocking(pVCpu, true);
        Log4Func(("Set NMI blocking. fValid=%RTbool uExitReason=%u\n", VMX_EXIT_INT_INFO_IS_VALID(pVmxTransient->uExitIntInfo),
                  pVmxTransient->uExitReason));
    }

    Assert(   rcStrict == VINF_SUCCESS  || rcStrict == VINF_HM_DOUBLE_FAULT
           || rcStrict == VINF_EM_RESET || rcStrict == VERR_EM_GUEST_CPU_HANG);
    return rcStrict;
}


/** @name VM-exit handlers.
 * @{
 */
/* -=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-= */
/* -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=- VM-exit handlers -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=- */
/* -=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-= */

/**
 * VM-exit handler for external interrupts (VMX_EXIT_EXT_INT).
 */
HMVMX_EXIT_DECL hmR0VmxExitExtInt(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitExtInt);
    /* Windows hosts (32-bit and 64-bit) have DPC latency issues. See @bugref{6853}. */
    if (VMMR0ThreadCtxHookIsEnabled(pVCpu))
        return VINF_SUCCESS;
    return VINF_EM_RAW_INTERRUPT;
}


/**
 * VM-exit handler for exceptions or NMIs (VMX_EXIT_XCPT_OR_NMI). Conditional
 * VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitXcptOrNmi(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);
    STAM_PROFILE_ADV_START(&pVCpu->hm.s.StatExitXcptNmi, y3);

    PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    int rc = hmR0VmxReadExitIntInfoVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);

    uint32_t const uIntType = VMX_EXIT_INT_INFO_TYPE(pVmxTransient->uExitIntInfo);
    Assert(   !(pVmcsInfo->u32ExitCtls & VMX_EXIT_CTLS_ACK_EXT_INT)
           && uIntType != VMX_EXIT_INT_INFO_TYPE_EXT_INT);
    Assert(VMX_EXIT_INT_INFO_IS_VALID(pVmxTransient->uExitIntInfo));

    if (uIntType == VMX_EXIT_INT_INFO_TYPE_NMI)
    {
        /*
         * This cannot be a guest NMI as the only way for the guest to receive an NMI is if we
         * injected it ourselves and anything we inject is not going to cause a VM-exit directly
         * for the event being injected[1]. Go ahead and dispatch the NMI to the host[2].
         *
         * [1] -- See Intel spec. 27.2.3 "Information for VM Exits During Event Delivery".
         * [2] -- See Intel spec. 27.5.5 "Updating Non-Register State".
         */
        return hmR0VmxExitHostNmi(pVCpu);
    }

    /* If this VM-exit occurred while delivering an event through the guest IDT, handle it accordingly. */
    VBOXSTRICTRC rcStrict = hmR0VmxCheckExitDueToEventDelivery(pVCpu, pVmxTransient);
    if (RT_UNLIKELY(rcStrict == VINF_SUCCESS))
    { /* likely */ }
    else
    {
        if (rcStrict == VINF_HM_DOUBLE_FAULT)
            rcStrict = VINF_SUCCESS;
        STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatExitXcptNmi, y3);
        return rcStrict;
    }

    uint32_t const uExitIntInfo = pVmxTransient->uExitIntInfo;
    uint32_t const uVector      = VMX_EXIT_INT_INFO_VECTOR(uExitIntInfo);
    switch (uIntType)
    {
        case VMX_EXIT_INT_INFO_TYPE_PRIV_SW_XCPT:  /* Privileged software exception. (#DB from ICEBP) */
            Assert(uVector == X86_XCPT_DB);
            RT_FALL_THRU();
        case VMX_EXIT_INT_INFO_TYPE_SW_XCPT:       /* Software exception. (#BP or #OF) */
            Assert(uVector == X86_XCPT_BP || uVector == X86_XCPT_OF || uIntType == VMX_EXIT_INT_INFO_TYPE_PRIV_SW_XCPT);
            RT_FALL_THRU();
        case VMX_EXIT_INT_INFO_TYPE_HW_XCPT:
        {
            /*
             * If there's any exception caused as a result of event injection, the resulting
             * secondary/final execption will be pending, we shall continue guest execution
             * after injecting the event. The page-fault case is complicated and we manually
             * handle any currently pending event in hmR0VmxExitXcptPF.
             */
            if (!pVCpu->hm.s.Event.fPending)
            { /* likely */ }
            else if (uVector != X86_XCPT_PF)
            {
                rcStrict = VINF_SUCCESS;
                break;
            }

            switch (uVector)
            {
                case X86_XCPT_PF: rcStrict = hmR0VmxExitXcptPF(pVCpu, pVmxTransient);      break;
                case X86_XCPT_GP: rcStrict = hmR0VmxExitXcptGP(pVCpu, pVmxTransient);      break;
                case X86_XCPT_MF: rcStrict = hmR0VmxExitXcptMF(pVCpu, pVmxTransient);      break;
                case X86_XCPT_DB: rcStrict = hmR0VmxExitXcptDB(pVCpu, pVmxTransient);      break;
                case X86_XCPT_BP: rcStrict = hmR0VmxExitXcptBP(pVCpu, pVmxTransient);      break;
                case X86_XCPT_AC: rcStrict = hmR0VmxExitXcptAC(pVCpu, pVmxTransient);      break;

                case X86_XCPT_NM: STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestNM);
                                  rcStrict = hmR0VmxExitXcptGeneric(pVCpu, pVmxTransient); break;
                case X86_XCPT_XF: STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestXF);
                                  rcStrict = hmR0VmxExitXcptGeneric(pVCpu, pVmxTransient); break;
                case X86_XCPT_DE: STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestDE);
                                  rcStrict = hmR0VmxExitXcptGeneric(pVCpu, pVmxTransient); break;
                case X86_XCPT_UD: STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestUD);
                                  rcStrict = hmR0VmxExitXcptGeneric(pVCpu, pVmxTransient); break;
                case X86_XCPT_SS: STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestSS);
                                  rcStrict = hmR0VmxExitXcptGeneric(pVCpu, pVmxTransient); break;
                case X86_XCPT_NP: STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestNP);
                                  rcStrict = hmR0VmxExitXcptGeneric(pVCpu, pVmxTransient); break;
                case X86_XCPT_TS: STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestTS);
                                  rcStrict = hmR0VmxExitXcptGeneric(pVCpu, pVmxTransient); break;
                default:
                {
                    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestXcpUnk);
                    if (pVmcsInfo->RealMode.fRealOnV86Active)
                    {
                        Assert(pVCpu->CTX_SUFF(pVM)->hm.s.vmx.pRealModeTSS);
                        Assert(PDMVmmDevHeapIsEnabled(pVCpu->CTX_SUFF(pVM)));
                        Assert(CPUMIsGuestInRealModeEx(&pVCpu->cpum.GstCtx));

                        rc  = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, CPUMCTX_EXTRN_CR0);
                        rc |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
                        rc |= hmR0VmxReadExitIntErrorCodeVmcs(pVmxTransient);
                        AssertRCReturn(rc, rc);
                        hmR0VmxSetPendingEvent(pVCpu, VMX_ENTRY_INT_INFO_FROM_EXIT_INT_INFO(uExitIntInfo),
                                               pVmxTransient->cbInstr, pVmxTransient->uExitIntErrorCode,
                                               0 /* GCPtrFaultAddress */);
                        rcStrict = VINF_SUCCESS;
                    }
                    else
                    {
                        AssertMsgFailed(("Unexpected VM-exit caused by exception %#x\n", uVector));
                        pVCpu->hm.s.u32HMError = uVector;
                        rcStrict = VERR_VMX_UNEXPECTED_EXCEPTION;
                    }
                    break;
                }
            }
            break;
        }

        default:
        {
            pVCpu->hm.s.u32HMError = uExitIntInfo;
            rcStrict = VERR_VMX_UNEXPECTED_INTERRUPTION_EXIT_TYPE;
            AssertMsgFailed(("Unexpected interruption info %#x\n", VMX_EXIT_INT_INFO_TYPE(uExitIntInfo)));
            break;
        }
    }
    STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatExitXcptNmi, y3);
    return rcStrict;
}


/**
 * VM-exit handler for interrupt-window exiting (VMX_EXIT_INT_WINDOW).
 */
HMVMX_EXIT_NSRC_DECL hmR0VmxExitIntWindow(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    /* Indicate that we no longer need to VM-exit when the guest is ready to receive interrupts, it is now ready. */
    PVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    int rc = hmR0VmxClearIntWindowExitVmcs(pVmcsInfo);
    AssertRCReturn(rc, rc);

    /* Evaluate and deliver pending events and resume guest execution. */
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitIntWindow);
    return VINF_SUCCESS;
}


/**
 * VM-exit handler for NMI-window exiting (VMX_EXIT_NMI_WINDOW).
 */
HMVMX_EXIT_NSRC_DECL hmR0VmxExitNmiWindow(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    PVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    if (RT_UNLIKELY(!(pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_NMI_WINDOW_EXIT))) /** @todo NSTVMX: Turn this into an assertion. */
    {
        AssertMsgFailed(("Unexpected NMI-window exit.\n"));
        HMVMX_UNEXPECTED_EXIT_RET(pVCpu, pVmxTransient->uExitReason);
    }

    Assert(!CPUMIsGuestNmiBlocking(pVCpu));

    /*
     * If block-by-STI is set when we get this VM-exit, it means the CPU doesn't block NMIs following STI.
     * It is therefore safe to unblock STI and deliver the NMI ourselves. See @bugref{7445}.
     */
    uint32_t fIntrState;
    int rc = VMXReadVmcs32(VMX_VMCS32_GUEST_INT_STATE, &fIntrState);
    AssertRCReturn(rc, rc);
    Assert(!(fIntrState & VMX_VMCS_GUEST_INT_STATE_BLOCK_MOVSS));
    if (fIntrState & VMX_VMCS_GUEST_INT_STATE_BLOCK_STI)
    {
        if (VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_INHIBIT_INTERRUPTS))
            VMCPU_FF_CLEAR(pVCpu, VMCPU_FF_INHIBIT_INTERRUPTS);

        fIntrState &= ~VMX_VMCS_GUEST_INT_STATE_BLOCK_STI;
        rc = VMXWriteVmcs32(VMX_VMCS32_GUEST_INT_STATE, fIntrState);
        AssertRCReturn(rc, rc);
    }

    /* Indicate that we no longer need to VM-exit when the guest is ready to receive NMIs, it is now ready */
    rc = hmR0VmxClearNmiWindowExitVmcs(pVmcsInfo);
    AssertRCReturn(rc, rc);

    /* Evaluate and deliver pending events and resume guest execution. */
    return VINF_SUCCESS;
}


/**
 * VM-exit handler for WBINVD (VMX_EXIT_WBINVD). Conditional VM-exit.
 */
HMVMX_EXIT_NSRC_DECL hmR0VmxExitWbinvd(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);
    return hmR0VmxAdvanceGuestRip(pVCpu, pVmxTransient);
}


/**
 * VM-exit handler for INVD (VMX_EXIT_INVD). Unconditional VM-exit.
 */
HMVMX_EXIT_NSRC_DECL hmR0VmxExitInvd(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);
    return hmR0VmxAdvanceGuestRip(pVCpu, pVmxTransient);
}


/**
 * VM-exit handler for CPUID (VMX_EXIT_CPUID). Unconditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitCpuid(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    /*
     * Get the state we need and update the exit history entry.
     */
    PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxImportGuestState(pVCpu, pVmcsInfo, IEM_CPUMCTX_EXTRN_EXEC_DECODED_NO_MEM_MASK);
    AssertRCReturn(rc, rc);

    VBOXSTRICTRC rcStrict;
    PCEMEXITREC pExitRec = EMHistoryUpdateFlagsAndTypeAndPC(pVCpu,
                                                            EMEXIT_MAKE_FT(EMEXIT_F_KIND_EM | EMEXIT_F_HM, EMEXITTYPE_CPUID),
                                                            pVCpu->cpum.GstCtx.rip + pVCpu->cpum.GstCtx.cs.u64Base);
    if (!pExitRec)
    {
        /*
         * Regular CPUID instruction execution.
         */
        rcStrict = IEMExecDecodedCpuid(pVCpu, pVmxTransient->cbInstr);
        if (rcStrict == VINF_SUCCESS)
            ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS);
        else if (rcStrict == VINF_IEM_RAISED_XCPT)
        {
            ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_RAISED_XCPT_MASK);
            rcStrict = VINF_SUCCESS;
        }
    }
    else
    {
        /*
         * Frequent exit or something needing probing.  Get state and call EMHistoryExec.
         */
        int rc2 = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, HMVMX_CPUMCTX_EXTRN_ALL);
        AssertRCReturn(rc2, rc2);

        Log4(("CpuIdExit/%u: %04x:%08RX64: %#x/%#x -> EMHistoryExec\n",
              pVCpu->idCpu, pVCpu->cpum.GstCtx.cs.Sel, pVCpu->cpum.GstCtx.rip, pVCpu->cpum.GstCtx.eax, pVCpu->cpum.GstCtx.ecx));

        rcStrict = EMHistoryExec(pVCpu, pExitRec, 0);
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_ALL_GUEST);

        Log4(("CpuIdExit/%u: %04x:%08RX64: EMHistoryExec -> %Rrc + %04x:%08RX64\n",
              pVCpu->idCpu, pVCpu->cpum.GstCtx.cs.Sel, pVCpu->cpum.GstCtx.rip,
              VBOXSTRICTRC_VAL(rcStrict), pVCpu->cpum.GstCtx.cs.Sel, pVCpu->cpum.GstCtx.rip));
    }
    return rcStrict;
}


/**
 * VM-exit handler for GETSEC (VMX_EXIT_GETSEC). Unconditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitGetsec(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    int rc = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, CPUMCTX_EXTRN_CR4);
    AssertRCReturn(rc, rc);

    if (pVCpu->cpum.GstCtx.cr4 & X86_CR4_SMXE)
        return VINF_EM_RAW_EMULATE_INSTR;

    AssertMsgFailed(("hmR0VmxExitGetsec: Unexpected VM-exit when CR4.SMXE is 0.\n"));
    HMVMX_UNEXPECTED_EXIT_RET(pVCpu, pVmxTransient->uExitReason);
}


/**
 * VM-exit handler for RDTSC (VMX_EXIT_RDTSC). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitRdtsc(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    int rc = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, IEM_CPUMCTX_EXTRN_MUST_MASK);
    rc    |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);

    VBOXSTRICTRC rcStrict = IEMExecDecodedRdtsc(pVCpu, pVmxTransient->cbInstr);
    if (RT_LIKELY(rcStrict == VINF_SUCCESS))
    {
        /* If we get a spurious VM-exit when TSC offsetting is enabled,
           we must reset offsetting on VM-entry. See @bugref{6634}. */
        if (pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_USE_TSC_OFFSETTING)
            pVmxTransient->fUpdatedTscOffsettingAndPreemptTimer = false;
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS);
    }
    else if (rcStrict == VINF_IEM_RAISED_XCPT)
    {
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_RAISED_XCPT_MASK);
        rcStrict = VINF_SUCCESS;
    }
    return rcStrict;
}


/**
 * VM-exit handler for RDTSCP (VMX_EXIT_RDTSCP). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitRdtscp(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    int rc = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, IEM_CPUMCTX_EXTRN_MUST_MASK | CPUMCTX_EXTRN_TSC_AUX);
    rc    |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);

    VBOXSTRICTRC rcStrict = IEMExecDecodedRdtscp(pVCpu, pVmxTransient->cbInstr);
    if (RT_LIKELY(rcStrict == VINF_SUCCESS))
    {
        /* If we get a spurious VM-exit when TSC offsetting is enabled,
           we must reset offsetting on VM-reentry. See @bugref{6634}. */
        if (pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_USE_TSC_OFFSETTING)
            pVmxTransient->fUpdatedTscOffsettingAndPreemptTimer = false;
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS);
    }
    else if (rcStrict == VINF_IEM_RAISED_XCPT)
    {
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_RAISED_XCPT_MASK);
        rcStrict = VINF_SUCCESS;
    }
    return rcStrict;
}


/**
 * VM-exit handler for RDPMC (VMX_EXIT_RDPMC). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitRdpmc(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    int rc = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, CPUMCTX_EXTRN_CR4    | CPUMCTX_EXTRN_CR0
                                                     | CPUMCTX_EXTRN_RFLAGS | CPUMCTX_EXTRN_SS);
    AssertRCReturn(rc, rc);

    PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
    rc = EMInterpretRdpmc(pVCpu->CTX_SUFF(pVM), pVCpu, CPUMCTX2CORE(pCtx));
    if (RT_LIKELY(rc == VINF_SUCCESS))
    {
        rc = hmR0VmxAdvanceGuestRip(pVCpu, pVmxTransient);
        Assert(pVmxTransient->cbInstr == 2);
    }
    else
    {
        AssertMsgFailed(("hmR0VmxExitRdpmc: EMInterpretRdpmc failed with %Rrc\n", rc));
        rc = VERR_EM_INTERPRETER;
    }
    return rc;
}


/**
 * VM-exit handler for VMCALL (VMX_EXIT_VMCALL). Unconditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitVmcall(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    VBOXSTRICTRC rcStrict = VERR_VMX_IPE_3;
    if (EMAreHypercallInstructionsEnabled(pVCpu))
    {
        PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
        int rc = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, CPUMCTX_EXTRN_RIP | CPUMCTX_EXTRN_RFLAGS | CPUMCTX_EXTRN_CR0
                                                         | CPUMCTX_EXTRN_SS  | CPUMCTX_EXTRN_CS     | CPUMCTX_EXTRN_EFER);
        AssertRCReturn(rc, rc);

        /* Perform the hypercall. */
        rcStrict = GIMHypercall(pVCpu, &pVCpu->cpum.GstCtx);
        if (rcStrict == VINF_SUCCESS)
        {
            rc = hmR0VmxAdvanceGuestRip(pVCpu, pVmxTransient);
            AssertRCReturn(rc, rc);
        }
        else
            Assert(   rcStrict == VINF_GIM_R3_HYPERCALL
                   || rcStrict == VINF_GIM_HYPERCALL_CONTINUING
                   || RT_FAILURE(rcStrict));

        /* If the hypercall changes anything other than guest's general-purpose registers,
           we would need to reload the guest changed bits here before VM-entry. */
    }
    else
        Log4Func(("Hypercalls not enabled\n"));

    /* If hypercalls are disabled or the hypercall failed for some reason, raise #UD and continue. */
    if (RT_FAILURE(rcStrict))
    {
        hmR0VmxSetPendingXcptUD(pVCpu);
        rcStrict = VINF_SUCCESS;
    }

    return rcStrict;
}


/**
 * VM-exit handler for INVLPG (VMX_EXIT_INVLPG). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitInvlpg(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);
    Assert(!pVCpu->CTX_SUFF(pVM)->hm.s.fNestedPaging || pVCpu->hm.s.fUsingDebugLoop);

    PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    int rc = hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
    rc    |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxImportGuestState(pVCpu, pVmcsInfo, IEM_CPUMCTX_EXTRN_EXEC_DECODED_MEM_MASK);
    AssertRCReturn(rc, rc);

    VBOXSTRICTRC rcStrict = IEMExecDecodedInvlpg(pVCpu, pVmxTransient->cbInstr, pVmxTransient->uExitQual);

    if (rcStrict == VINF_SUCCESS || rcStrict == VINF_PGM_SYNC_CR3)
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS);
    else if (rcStrict == VINF_IEM_RAISED_XCPT)
    {
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_RAISED_XCPT_MASK);
        rcStrict = VINF_SUCCESS;
    }
    else
        AssertMsgFailed(("Unexpected IEMExecDecodedInvlpg(%#RX64) status: %Rrc\n", pVmxTransient->uExitQual,
                         VBOXSTRICTRC_VAL(rcStrict)));
    return rcStrict;
}


/**
 * VM-exit handler for MONITOR (VMX_EXIT_MONITOR). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitMonitor(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    int rc = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, IEM_CPUMCTX_EXTRN_EXEC_DECODED_MEM_MASK | CPUMCTX_EXTRN_DS);
    rc    |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);

    VBOXSTRICTRC rcStrict = IEMExecDecodedMonitor(pVCpu, pVmxTransient->cbInstr);
    if (rcStrict == VINF_SUCCESS)
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS);
    else if (rcStrict == VINF_IEM_RAISED_XCPT)
    {
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_RAISED_XCPT_MASK);
        rcStrict = VINF_SUCCESS;
    }

    return rcStrict;
}


/**
 * VM-exit handler for MWAIT (VMX_EXIT_MWAIT). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitMwait(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    int rc = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, IEM_CPUMCTX_EXTRN_EXEC_DECODED_NO_MEM_MASK);
    rc    |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);

    VBOXSTRICTRC rcStrict = IEMExecDecodedMwait(pVCpu, pVmxTransient->cbInstr);
    if (RT_SUCCESS(rcStrict))
    {
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS);
        if (EMMonitorWaitShouldContinue(pVCpu, &pVCpu->cpum.GstCtx))
            rcStrict = VINF_SUCCESS;
    }

    return rcStrict;
}


/**
 * VM-exit handler for triple faults (VMX_EXIT_TRIPLE_FAULT). Unconditional
 * VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitTripleFault(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);
    return VINF_EM_RESET;
}


/**
 * VM-exit handler for HLT (VMX_EXIT_HLT). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitHlt(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    int rc = hmR0VmxAdvanceGuestRip(pVCpu, pVmxTransient);
    AssertRCReturn(rc, rc);

    HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_RFLAGS);            /* Advancing the RIP above should've imported eflags. */
    if (EMShouldContinueAfterHalt(pVCpu, &pVCpu->cpum.GstCtx))    /* Requires eflags. */
        rc = VINF_SUCCESS;
    else
        rc = VINF_EM_HALT;

    if (rc != VINF_SUCCESS)
        STAM_COUNTER_INC(&pVCpu->hm.s.StatSwitchHltToR3);
    return rc;
}


/**
 * VM-exit handler for instructions that result in a \#UD exception delivered to
 * the guest.
 */
HMVMX_EXIT_NSRC_DECL hmR0VmxExitSetPendingXcptUD(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);
    hmR0VmxSetPendingXcptUD(pVCpu);
    return VINF_SUCCESS;
}


/**
 * VM-exit handler for expiry of the VMX-preemption timer.
 */
HMVMX_EXIT_DECL hmR0VmxExitPreemptTimer(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    /* If the VMX-preemption timer has expired, reinitialize the preemption timer on next VM-entry. */
    pVmxTransient->fUpdatedTscOffsettingAndPreemptTimer = false;

    /* If there are any timer events pending, fall back to ring-3, otherwise resume guest execution. */
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    bool fTimersPending = TMTimerPollBool(pVM, pVCpu);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitPreemptTimer);
    return fTimersPending ? VINF_EM_RAW_TIMER_PENDING : VINF_SUCCESS;
}


/**
 * VM-exit handler for XSETBV (VMX_EXIT_XSETBV). Unconditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitXsetbv(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxImportGuestState(pVCpu, pVmcsInfo, IEM_CPUMCTX_EXTRN_MUST_MASK | CPUMCTX_EXTRN_CR4);
    AssertRCReturn(rc, rc);

    VBOXSTRICTRC rcStrict = IEMExecDecodedXsetbv(pVCpu, pVmxTransient->cbInstr);
    ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, rcStrict != VINF_IEM_RAISED_XCPT ? HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS
                                                                                : HM_CHANGED_RAISED_XCPT_MASK);

    PCCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
    pVCpu->hm.s.fLoadSaveGuestXcr0 = (pCtx->cr4 & X86_CR4_OSXSAVE) && pCtx->aXcr[0] != ASMGetXcr0();

    return rcStrict;
}


/**
 * VM-exit handler for INVPCID (VMX_EXIT_INVPCID). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitInvpcid(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);
    /** @todo Use VM-exit instruction information. */
    return VERR_EM_INTERPRETER;
}


/**
 * VM-exit handler for invalid-guest-state (VMX_EXIT_ERR_INVALID_GUEST_STATE). Error
 * VM-exit.
 */
HMVMX_EXIT_NSRC_DECL hmR0VmxExitErrInvalidGuestState(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    int rc = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, HMVMX_CPUMCTX_EXTRN_ALL);
    AssertRCReturn(rc, rc);

    rc = hmR0VmxCheckVmcsCtls(pVCpu, pVmcsInfo);
    if (RT_FAILURE(rc))
        return rc;

    uint32_t const uInvalidReason = hmR0VmxCheckGuestState(pVCpu, pVmcsInfo);
    NOREF(uInvalidReason);

#ifdef VBOX_STRICT
    uint32_t    fIntrState;
    RTHCUINTREG uHCReg;
    uint64_t    u64Val;
    uint32_t    u32Val;
    rc  = hmR0VmxReadEntryIntInfoVmcs(pVmxTransient);
    rc |= hmR0VmxReadEntryXcptErrorCodeVmcs(pVmxTransient);
    rc |= hmR0VmxReadEntryInstrLenVmcs(pVmxTransient);
    rc |= VMXReadVmcs32(VMX_VMCS32_GUEST_INT_STATE, &fIntrState);
    AssertRCReturn(rc, rc);

    Log4(("uInvalidReason                             %u\n",     uInvalidReason));
    Log4(("VMX_VMCS32_CTRL_ENTRY_INTERRUPTION_INFO    %#RX32\n", pVmxTransient->uEntryIntInfo));
    Log4(("VMX_VMCS32_CTRL_ENTRY_EXCEPTION_ERRCODE    %#RX32\n", pVmxTransient->uEntryXcptErrorCode));
    Log4(("VMX_VMCS32_CTRL_ENTRY_INSTR_LENGTH         %#RX32\n", pVmxTransient->cbEntryInstr));
    Log4(("VMX_VMCS32_GUEST_INT_STATE                 %#RX32\n", fIntrState));

    rc = VMXReadVmcs32(VMX_VMCS_GUEST_CR0, &u32Val);                        AssertRC(rc);
    Log4(("VMX_VMCS_GUEST_CR0                         %#RX32\n", u32Val));
    rc = VMXReadVmcsHstN(VMX_VMCS_CTRL_CR0_MASK, &uHCReg);                  AssertRC(rc);
    Log4(("VMX_VMCS_CTRL_CR0_MASK                     %#RHr\n", uHCReg));
    rc = VMXReadVmcsHstN(VMX_VMCS_CTRL_CR0_READ_SHADOW, &uHCReg);           AssertRC(rc);
    Log4(("VMX_VMCS_CTRL_CR4_READ_SHADOW              %#RHr\n", uHCReg));
    rc = VMXReadVmcsHstN(VMX_VMCS_CTRL_CR4_MASK, &uHCReg);                  AssertRC(rc);
    Log4(("VMX_VMCS_CTRL_CR4_MASK                     %#RHr\n", uHCReg));
    rc = VMXReadVmcsHstN(VMX_VMCS_CTRL_CR4_READ_SHADOW, &uHCReg);           AssertRC(rc);
    Log4(("VMX_VMCS_CTRL_CR4_READ_SHADOW              %#RHr\n", uHCReg));
    rc = VMXReadVmcs64(VMX_VMCS64_CTRL_EPTP_FULL, &u64Val);                 AssertRC(rc);
    Log4(("VMX_VMCS64_CTRL_EPTP_FULL                  %#RX64\n", u64Val));

    hmR0DumpRegs(pVCpu);
#endif

    return VERR_VMX_INVALID_GUEST_STATE;
}

/**
 * VM-exit handler for all undefined/unexpected reasons. Should never happen.
 */
HMVMX_EXIT_NSRC_DECL hmR0VmxExitErrUnexpected(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    /*
     * Cummulative notes of all recognized but unexpected VM-exits.
     *
     * 1. This does -not- cover scenarios like like a page-fault VM-exit occurring when
     *    nested-paging is used.
     *
     * 2. Any instruction that causes a VM-exit unconditionally (for e.g. VMXON) must be
     *    emulated or a #UD must be raised in the guest. Therefore, we should -not- be using
     *    this function (and thereby stop VM execution) for handling such instructions.
     *
     *
     * VMX_EXIT_INIT_SIGNAL:
     *    INIT signals are blocked in VMX root operation by VMXON and by SMI in SMM.
     *    It is -NOT- blocked in VMX non-root operation so we can, in theory, still get these
     *    VM-exits. However, we should not receive INIT signals VM-exit while executing a VM.
     *
     *    See Intel spec. 33.14.1 Default Treatment of SMI Delivery"
     *    See Intel spec. 29.3 "VMX Instructions" for "VMXON".
     *    See Intel spec. "23.8 Restrictions on VMX operation".
     *
     * VMX_EXIT_SIPI:
     *    SIPI exits can only occur in VMX non-root operation when the "wait-for-SIPI" guest
     *    activity state is used. We don't make use of it as our guests don't have direct
     *    access to the host local APIC.
     *
     *    See Intel spec. 25.3 "Other Causes of VM-exits".
     *
     * VMX_EXIT_IO_SMI:
     * VMX_EXIT_SMI:
     *    This can only happen if we support dual-monitor treatment of SMI, which can be
     *    activated by executing VMCALL in VMX root operation. Only an STM (SMM transfer
     *    monitor) would get this VM-exit when we (the executive monitor) execute a VMCALL in
     *    VMX root mode or receive an SMI. If we get here, something funny is going on.
     *
     *    See Intel spec. 33.15.6 "Activating the Dual-Monitor Treatment"
     *    See Intel spec. 25.3 "Other Causes of VM-Exits"
     *
     * VMX_EXIT_ERR_MSR_LOAD:
     *    Failures while loading MSRs are part of the VM-entry MSR-load area are unexpected
     *    and typically indicates a bug in the hypervisor code. We thus cannot not resume
     *    execution.
     *
     *    See Intel spec. 26.7 "VM-Entry Failures During Or After Loading Guest State".
     *
     * VMX_EXIT_ERR_MACHINE_CHECK:
     *    Machine check exceptions indicates a fatal/unrecoverable hardware condition
     *    including but not limited to system bus, ECC, parity, cache and TLB errors. A
     *    #MC exception abort class exception is raised. We thus cannot assume a
     *    reasonable chance of continuing any sort of execution and we bail.
     *
     *    See Intel spec. 15.1 "Machine-check Architecture".
     *    See Intel spec. 27.1 "Architectural State Before A VM Exit".
     *
     * VMX_EXIT_PML_FULL:
     * VMX_EXIT_VIRTUALIZED_EOI:
     * VMX_EXIT_APIC_WRITE:
     *    We do not currently support any of these features and thus they are all unexpected
     *    VM-exits.
     *
     * VMX_EXIT_GDTR_IDTR_ACCESS:
     * VMX_EXIT_LDTR_TR_ACCESS:
     * VMX_EXIT_RDRAND:
     * VMX_EXIT_RSM:
     * VMX_EXIT_VMFUNC:
     * VMX_EXIT_ENCLS:
     * VMX_EXIT_RDSEED:
     * VMX_EXIT_XSAVES:
     * VMX_EXIT_XRSTORS:
     * VMX_EXIT_UMWAIT:
     * VMX_EXIT_TPAUSE:
     *    These VM-exits are -not- caused unconditionally by execution of the corresponding
     *    instruction. Any VM-exit for these instructions indicate a hardware problem,
     *    unsupported CPU modes (like SMM) or potentially corrupt VMCS controls.
     *
     *    See Intel spec. 25.1.3 "Instructions That Cause VM Exits Conditionally".
     */
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);
    AssertMsgFailed(("Unexpected VM-exit %u\n", pVmxTransient->uExitReason));
    HMVMX_UNEXPECTED_EXIT_RET(pVCpu, pVmxTransient->uExitReason);
}


/**
 * VM-exit handler for RDMSR (VMX_EXIT_RDMSR).
 */
HMVMX_EXIT_DECL hmR0VmxExitRdmsr(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    /** @todo Optimize this: We currently drag in in the whole MSR state
     * (CPUMCTX_EXTRN_ALL_MSRS) here.  We should optimize this to only get
     * MSRs required.  That would require changes to IEM and possibly CPUM too.
     * (Should probably do it lazy fashion from CPUMAllMsrs.cpp). */
    PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    uint32_t const idMsr    = pVCpu->cpum.GstCtx.ecx;
    uint64_t       fImport  = IEM_CPUMCTX_EXTRN_EXEC_DECODED_NO_MEM_MASK | CPUMCTX_EXTRN_ALL_MSRS;
    switch (idMsr)
    {
        case MSR_K8_FS_BASE: fImport |= CPUMCTX_EXTRN_FS; break;
        case MSR_K8_GS_BASE: fImport |= CPUMCTX_EXTRN_GS; break;
    }

    int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxImportGuestState(pVCpu, pVmcsInfo, fImport);
    AssertRCReturn(rc, rc);

    Log4Func(("ecx=%#RX32\n", idMsr));

#ifdef VBOX_STRICT
    if (hmR0VmxIsProcCtlsSet(pVCpu, pVmxTransient, VMX_PROC_CTLS_USE_MSR_BITMAPS))
    {
        if (   hmR0VmxIsAutoLoadGuestMsr(pVmcsInfo, idMsr)
            && idMsr != MSR_K6_EFER)
        {
            AssertMsgFailed(("Unexpected RDMSR for an MSR in the auto-load/store area in the VMCS. ecx=%#RX32\n", idMsr));
            HMVMX_UNEXPECTED_EXIT_RET(pVCpu, idMsr);
        }
        if (hmR0VmxIsLazyGuestMsr(pVCpu, idMsr))
        {
            Assert(pVmcsInfo->pvMsrBitmap);
            uint32_t fMsrpm = CPUMGetVmxMsrPermission(pVmcsInfo->pvMsrBitmap, idMsr);
            if (fMsrpm & VMXMSRPM_ALLOW_RD)
            {
                AssertMsgFailed(("Unexpected RDMSR for a passthru lazy-restore MSR. ecx=%#RX32\n", idMsr));
                HMVMX_UNEXPECTED_EXIT_RET(pVCpu, idMsr);
            }
        }
    }
#endif

    VBOXSTRICTRC rcStrict = IEMExecDecodedRdmsr(pVCpu, pVmxTransient->cbInstr);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitRdmsr);
    if (rcStrict == VINF_SUCCESS)
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS
                                                 | HM_CHANGED_GUEST_RAX | HM_CHANGED_GUEST_RDX);
    else if (rcStrict == VINF_IEM_RAISED_XCPT)
    {
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_RAISED_XCPT_MASK);
        rcStrict = VINF_SUCCESS;
    }
    else
        AssertMsg(rcStrict == VINF_CPUM_R3_MSR_READ, ("Unexpected IEMExecDecodedRdmsr rc (%Rrc)\n", VBOXSTRICTRC_VAL(rcStrict)));

    return rcStrict;
}


/**
 * VM-exit handler for WRMSR (VMX_EXIT_WRMSR).
 */
HMVMX_EXIT_DECL hmR0VmxExitWrmsr(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    /** @todo Optimize this: We currently drag in in the whole MSR state
     * (CPUMCTX_EXTRN_ALL_MSRS) here.  We should optimize this to only get
     * MSRs required.  That would require changes to IEM and possibly CPUM too.
     * (Should probably do it lazy fashion from CPUMAllMsrs.cpp). */
    uint32_t const idMsr    = pVCpu->cpum.GstCtx.ecx;
    uint64_t       fImport  = IEM_CPUMCTX_EXTRN_EXEC_DECODED_NO_MEM_MASK | CPUMCTX_EXTRN_ALL_MSRS;

    /*
     * The FS and GS base MSRs are not part of the above all-MSRs mask.
     * Although we don't need to fetch the base as it will be overwritten shortly, while
     * loading guest-state we would also load the entire segment register including limit
     * and attributes and thus we need to load them here.
     */
    switch (idMsr)
    {
        case MSR_K8_FS_BASE: fImport |= CPUMCTX_EXTRN_FS; break;
        case MSR_K8_GS_BASE: fImport |= CPUMCTX_EXTRN_GS; break;
    }

    PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxImportGuestState(pVCpu, pVmcsInfo, fImport);
    AssertRCReturn(rc, rc);

    Log4Func(("ecx=%#RX32 edx:eax=%#RX32:%#RX32\n", idMsr, pVCpu->cpum.GstCtx.edx, pVCpu->cpum.GstCtx.eax));

    VBOXSTRICTRC rcStrict = IEMExecDecodedWrmsr(pVCpu, pVmxTransient->cbInstr);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitWrmsr);

    if (rcStrict == VINF_SUCCESS)
    {
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS);

        /* If this is an X2APIC WRMSR access, update the APIC state as well. */
        if (    idMsr == MSR_IA32_APICBASE
            || (   idMsr >= MSR_IA32_X2APIC_START
                && idMsr <= MSR_IA32_X2APIC_END))
        {
            /*
             * We've already saved the APIC related guest-state (TPR) in post-run phase.
             * When full APIC register virtualization is implemented we'll have to make
             * sure APIC state is saved from the VMCS before IEM changes it.
             */
            ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_APIC_TPR);
        }
        else if (idMsr == MSR_IA32_TSC)        /* Windows 7 does this during bootup. See @bugref{6398}. */
            pVmxTransient->fUpdatedTscOffsettingAndPreemptTimer = false;
        else if (idMsr == MSR_K6_EFER)
        {
            /*
             * If the guest touches the EFER MSR we need to update the VM-Entry and VM-Exit controls
             * as well, even if it is -not- touching bits that cause paging mode changes (LMA/LME).
             * We care about the other bits as well, SCE and NXE. See @bugref{7368}.
             */
            ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_EFER_MSR | HM_CHANGED_VMX_ENTRY_EXIT_CTLS);
        }

        /* Update MSRs that are part of the VMCS and auto-load/store area when MSR-bitmaps are not supported. */
        if (!hmR0VmxIsProcCtlsSet(pVCpu, pVmxTransient, VMX_PROC_CTLS_USE_MSR_BITMAPS))
        {
            switch (idMsr)
            {
                case MSR_IA32_SYSENTER_CS:  ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_SYSENTER_CS_MSR);  break;
                case MSR_IA32_SYSENTER_EIP: ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_SYSENTER_EIP_MSR); break;
                case MSR_IA32_SYSENTER_ESP: ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_SYSENTER_ESP_MSR); break;
                case MSR_K8_FS_BASE:        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_FS);               break;
                case MSR_K8_GS_BASE:        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_GS);               break;
                case MSR_K6_EFER:           /* Nothing to do, already handled above. */                                    break;
                default:
                {
                    if (hmR0VmxIsAutoLoadGuestMsr(pVmcsInfo, idMsr))
                        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_VMX_GUEST_AUTO_MSRS);
                    else if (hmR0VmxIsLazyGuestMsr(pVCpu, idMsr))
                        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_VMX_GUEST_LAZY_MSRS);
                    break;
                }
            }
        }
#ifdef VBOX_STRICT
        else
        {
            /* Paranoia. Validate that MSRs in the MSR-bitmaps with write-passthru are not intercepted. */
            switch (idMsr)
            {
                case MSR_IA32_SYSENTER_CS:
                case MSR_IA32_SYSENTER_EIP:
                case MSR_IA32_SYSENTER_ESP:
                case MSR_K8_FS_BASE:
                case MSR_K8_GS_BASE:
                {
                    AssertMsgFailed(("Unexpected WRMSR for an MSR in the VMCS. ecx=%#RX32\n", idMsr));
                    HMVMX_UNEXPECTED_EXIT_RET(pVCpu, idMsr);
                }

                /* Writes to MSRs in auto-load/store area/swapped MSRs, shouldn't cause VM-exits with MSR-bitmaps. */
                default:
                {
                    if (hmR0VmxIsAutoLoadGuestMsr(pVmcsInfo, idMsr))
                    {
                        /* EFER MSR writes are always intercepted. */
                        if (idMsr != MSR_K6_EFER)
                        {
                            AssertMsgFailed(("Unexpected WRMSR for an MSR in the auto-load/store area in the VMCS. ecx=%#RX32\n",
                                             idMsr));
                            HMVMX_UNEXPECTED_EXIT_RET(pVCpu, idMsr);
                        }
                    }

                    if (hmR0VmxIsLazyGuestMsr(pVCpu, idMsr))
                    {
                        Assert(pVmcsInfo->pvMsrBitmap);
                        uint32_t fMsrpm = CPUMGetVmxMsrPermission(pVmcsInfo->pvMsrBitmap, idMsr);
                        if (fMsrpm & VMXMSRPM_ALLOW_WR)
                        {
                            AssertMsgFailed(("Unexpected WRMSR for passthru, lazy-restore MSR. ecx=%#RX32\n", idMsr));
                            HMVMX_UNEXPECTED_EXIT_RET(pVCpu, idMsr);
                        }
                    }
                    break;
                }
            }
        }
#endif  /* VBOX_STRICT */
    }
    else if (rcStrict == VINF_IEM_RAISED_XCPT)
    {
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_RAISED_XCPT_MASK);
        rcStrict = VINF_SUCCESS;
    }
    else
        AssertMsg(rcStrict == VINF_CPUM_R3_MSR_WRITE, ("Unexpected IEMExecDecodedWrmsr rc (%Rrc)\n", VBOXSTRICTRC_VAL(rcStrict)));

    return rcStrict;
}


/**
 * VM-exit handler for PAUSE (VMX_EXIT_PAUSE). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitPause(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    /** @todo The guest has likely hit a contended spinlock. We might want to
     *        poke a schedule different guest VCPU. */
    int rc = hmR0VmxAdvanceGuestRip(pVCpu, pVmxTransient);
    if (RT_SUCCESS(rc))
        return VINF_EM_RAW_INTERRUPT;

    AssertMsgFailed(("hmR0VmxExitPause: Failed to increment RIP. rc=%Rrc\n", rc));
    return rc;
}


/**
 * VM-exit handler for when the TPR value is lowered below the specified
 * threshold (VMX_EXIT_TPR_BELOW_THRESHOLD). Conditional VM-exit.
 */
HMVMX_EXIT_NSRC_DECL hmR0VmxExitTprBelowThreshold(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);
    Assert(pVmxTransient->pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_USE_TPR_SHADOW);

    /*
     * The TPR shadow would've been synced with the APIC TPR in the post-run phase.
     * We'll re-evaluate pending interrupts and inject them before the next VM
     * entry so we can just continue execution here.
     */
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitTprBelowThreshold);
    return VINF_SUCCESS;
}


/**
 * VM-exit handler for control-register accesses (VMX_EXIT_MOV_CRX). Conditional
 * VM-exit.
 *
 * @retval VINF_SUCCESS when guest execution can continue.
 * @retval VINF_PGM_SYNC_CR3 CR3 sync is required, back to ring-3.
 * @retval VERR_EM_RESCHEDULE_REM when we need to return to ring-3 due to
 *         incompatible guest state for VMX execution (real-on-v86 case).
 */
HMVMX_EXIT_DECL hmR0VmxExitMovCRx(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);
    STAM_PROFILE_ADV_START(&pVCpu->hm.s.StatExitMovCRx, y2);

    PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    int rc = hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
    rc    |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);

    VBOXSTRICTRC rcStrict;
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    uint64_t const uExitQual   = pVmxTransient->uExitQual;
    uint32_t const uAccessType = VMX_EXIT_QUAL_CRX_ACCESS(uExitQual);
    switch (uAccessType)
    {
        /*
         * MOV to CRx.
         */
        case VMX_EXIT_QUAL_CRX_ACCESS_WRITE:
        {
            rc = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, IEM_CPUMCTX_EXTRN_MUST_MASK);
            AssertRCReturn(rc, rc);

            HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_CR0);
            uint32_t const uOldCr0 = pVCpu->cpum.GstCtx.cr0;
            uint8_t const  iGReg   = VMX_EXIT_QUAL_CRX_GENREG(uExitQual);
            uint8_t const  iCrReg  = VMX_EXIT_QUAL_CRX_REGISTER(uExitQual);

            /*
             * MOV to CR3 only cause a VM-exit when one or more of the following are true:
             *   - When nested paging isn't used.
             *   - If the guest doesn't have paging enabled (intercept CR3 to update shadow page tables).
             *   - We are executing in the VM debug loop.
             */
            Assert(   iCrReg != 3
                   || !pVM->hm.s.fNestedPaging
                   || !CPUMIsGuestPagingEnabledEx(&pVCpu->cpum.GstCtx)
                   || pVCpu->hm.s.fUsingDebugLoop);

            /* MOV to CR8 writes only cause VM-exits when TPR shadow is not used. */
            Assert(   iCrReg != 8
                   || !(pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_USE_TPR_SHADOW));

            rcStrict = hmR0VmxExitMovToCrX(pVCpu, pVmcsInfo, pVmxTransient->cbInstr, iGReg, iCrReg);
            AssertMsg(   rcStrict == VINF_SUCCESS
                      || rcStrict == VINF_PGM_SYNC_CR3, ("%Rrc\n", VBOXSTRICTRC_VAL(rcStrict)));

            /*
             * This is a kludge for handling switches back to real mode when we try to use
             * V86 mode to run real mode code directly.  Problem is that V86 mode cannot
             * deal with special selector values, so we have to return to ring-3 and run
             * there till the selector values are V86 mode compatible.
             *
             * Note! Using VINF_EM_RESCHEDULE_REM here rather than VINF_EM_RESCHEDULE since the
             *       latter is an alias for VINF_IEM_RAISED_XCPT which is asserted at the end of
             *       this function.
             */
            if (   iCrReg == 0
                && rcStrict == VINF_SUCCESS
                && !pVM->hm.s.vmx.fUnrestrictedGuest
                && CPUMIsGuestInRealModeEx(&pVCpu->cpum.GstCtx)
                && (uOldCr0 & X86_CR0_PE)
                && !(pVCpu->cpum.GstCtx.cr0 & X86_CR0_PE))
            {
                /** @todo Check selectors rather than returning all the time.  */
                Assert(!pVmxTransient->fIsNestedGuest);
                Log4Func(("CR0 write, back to real mode -> VINF_EM_RESCHEDULE_REM\n"));
                rcStrict = VINF_EM_RESCHEDULE_REM;
            }
            break;
        }

        /*
         * MOV from CRx.
         */
        case VMX_EXIT_QUAL_CRX_ACCESS_READ:
        {
            uint8_t const iGReg  = VMX_EXIT_QUAL_CRX_GENREG(uExitQual);
            uint8_t const iCrReg = VMX_EXIT_QUAL_CRX_REGISTER(uExitQual);

            /*
             * MOV from CR3 only cause a VM-exit when one or more of the following are true:
             *   - When nested paging isn't used.
             *   - If the guest doesn't have paging enabled (pass guest's CR3 rather than our identity mapped CR3).
             *   - We are executing in the VM debug loop.
             */
            Assert(   iCrReg != 3
                   || !pVM->hm.s.fNestedPaging
                   || !CPUMIsGuestPagingEnabledEx(&pVCpu->cpum.GstCtx)
                   || pVCpu->hm.s.fUsingDebugLoop);

            /* MOV from CR8 reads only cause a VM-exit when the TPR shadow feature isn't enabled. */
            Assert(   iCrReg != 8
                   || !(pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_USE_TPR_SHADOW));

            rcStrict = hmR0VmxExitMovFromCrX(pVCpu, pVmcsInfo, pVmxTransient->cbInstr, iGReg, iCrReg);
            break;
        }

        /*
         * CLTS (Clear Task-Switch Flag in CR0).
         */
        case VMX_EXIT_QUAL_CRX_ACCESS_CLTS:
        {
            rcStrict = hmR0VmxExitClts(pVCpu, pVmcsInfo, pVmxTransient->cbInstr);
            break;
        }

        /*
         * LMSW (Load Machine-Status Word into CR0).
         * LMSW cannot clear CR0.PE, so no fRealOnV86Active kludge needed here.
         */
        case VMX_EXIT_QUAL_CRX_ACCESS_LMSW:
        {
            RTGCPTR        GCPtrEffDst;
            uint8_t const  cbInstr     = pVmxTransient->cbInstr;
            uint16_t const uMsw        = VMX_EXIT_QUAL_CRX_LMSW_DATA(uExitQual);
            bool const     fMemOperand = VMX_EXIT_QUAL_CRX_LMSW_OP_MEM(uExitQual);
            if (fMemOperand)
            {
                rc = hmR0VmxReadGuestLinearAddrVmcs(pVCpu, pVmxTransient);
                AssertRCReturn(rc, rc);
                GCPtrEffDst = pVmxTransient->uGuestLinearAddr;
            }
            else
                GCPtrEffDst = NIL_RTGCPTR;
            rcStrict = hmR0VmxExitLmsw(pVCpu, pVmcsInfo, cbInstr, uMsw, GCPtrEffDst);
            break;
        }

        default:
        {
            AssertMsgFailed(("Unrecognized Mov CRX access type %#x\n", uAccessType));
            HMVMX_UNEXPECTED_EXIT_RET(pVCpu, uAccessType);
        }
    }

    Assert((pVCpu->hm.s.fCtxChanged & (HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS))
                                   == (HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS));
    Assert(rcStrict != VINF_IEM_RAISED_XCPT);

    STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatExitMovCRx, y2);
    NOREF(pVM);
    return rcStrict;
}


/**
 * VM-exit handler for I/O instructions (VMX_EXIT_IO_INSTR). Conditional
 * VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitIoInstr(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);
    STAM_PROFILE_ADV_START(&pVCpu->hm.s.StatExitIO, y1);

    PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
    PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    int rc = hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
    rc    |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxImportGuestState(pVCpu, pVmcsInfo, IEM_CPUMCTX_EXTRN_MUST_MASK | CPUMCTX_EXTRN_SREG_MASK
                                                     | CPUMCTX_EXTRN_EFER);
    /* EFER MSR also required for longmode checks in EMInterpretDisasCurrent(), but it's always up-to-date. */
    AssertRCReturn(rc, rc);

    /* Refer Intel spec. 27-5. "Exit Qualifications for I/O Instructions" for the format. */
    uint32_t const uIOPort      = VMX_EXIT_QUAL_IO_PORT(pVmxTransient->uExitQual);
    uint8_t  const uIOSize      = VMX_EXIT_QUAL_IO_SIZE(pVmxTransient->uExitQual);
    bool     const fIOWrite     = (VMX_EXIT_QUAL_IO_DIRECTION(pVmxTransient->uExitQual) == VMX_EXIT_QUAL_IO_DIRECTION_OUT);
    bool     const fIOString    = VMX_EXIT_QUAL_IO_IS_STRING(pVmxTransient->uExitQual);
    bool     const fGstStepping = RT_BOOL(pCtx->eflags.Bits.u1TF);
    bool     const fDbgStepping = pVCpu->hm.s.fSingleInstruction;
    AssertReturn(uIOSize <= 3 && uIOSize != 2, VERR_VMX_IPE_1);

    /*
     * Update exit history to see if this exit can be optimized.
     */
    VBOXSTRICTRC rcStrict;
    PCEMEXITREC  pExitRec = NULL;
    if (   !fGstStepping
        && !fDbgStepping)
        pExitRec = EMHistoryUpdateFlagsAndTypeAndPC(pVCpu,
                                                    !fIOString
                                                    ? !fIOWrite
                                                    ? EMEXIT_MAKE_FT(EMEXIT_F_KIND_EM | EMEXIT_F_HM, EMEXITTYPE_IO_PORT_READ)
                                                    : EMEXIT_MAKE_FT(EMEXIT_F_KIND_EM | EMEXIT_F_HM, EMEXITTYPE_IO_PORT_WRITE)
                                                    : !fIOWrite
                                                    ? EMEXIT_MAKE_FT(EMEXIT_F_KIND_EM | EMEXIT_F_HM, EMEXITTYPE_IO_PORT_STR_READ)
                                                    : EMEXIT_MAKE_FT(EMEXIT_F_KIND_EM | EMEXIT_F_HM, EMEXITTYPE_IO_PORT_STR_WRITE),
                                                    pVCpu->cpum.GstCtx.rip + pVCpu->cpum.GstCtx.cs.u64Base);
    if (!pExitRec)
    {
        static uint32_t const s_aIOSizes[4] = { 1, 2, 0, 4 };                    /* Size of the I/O accesses in bytes. */
        static uint32_t const s_aIOOpAnd[4] = { 0xff, 0xffff, 0, 0xffffffff };   /* AND masks for saving result in AL/AX/EAX. */

        uint32_t const cbValue  = s_aIOSizes[uIOSize];
        uint32_t const cbInstr  = pVmxTransient->cbInstr;
        bool fUpdateRipAlready  = false; /* ugly hack, should be temporary. */
        PVM pVM                 = pVCpu->CTX_SUFF(pVM);
        if (fIOString)
        {
            /*
             * INS/OUTS - I/O String instruction.
             *
             * Use instruction-information if available, otherwise fall back on
             * interpreting the instruction.
             */
            Log4Func(("cs:rip=%#04x:%#RX64 %#06x/%u %c str\n", pCtx->cs.Sel, pCtx->rip, uIOPort, cbValue, fIOWrite ? 'w' : 'r'));
            AssertReturn(pCtx->dx == uIOPort, VERR_VMX_IPE_2);
            bool const fInsOutsInfo = RT_BF_GET(pVM->hm.s.vmx.Msrs.u64Basic, VMX_BF_BASIC_VMCS_INS_OUTS);
            if (fInsOutsInfo)
            {
                int rc2  = hmR0VmxReadExitInstrInfoVmcs(pVmxTransient);
                AssertRCReturn(rc2, rc2);
                AssertReturn(pVmxTransient->ExitInstrInfo.StrIo.u3AddrSize <= 2, VERR_VMX_IPE_3);
                AssertCompile(IEMMODE_16BIT == 0 && IEMMODE_32BIT == 1 && IEMMODE_64BIT == 2);
                IEMMODE const enmAddrMode = (IEMMODE)pVmxTransient->ExitInstrInfo.StrIo.u3AddrSize;
                bool const fRep           = VMX_EXIT_QUAL_IO_IS_REP(pVmxTransient->uExitQual);
                if (fIOWrite)
                    rcStrict = IEMExecStringIoWrite(pVCpu, cbValue, enmAddrMode, fRep, cbInstr,
                                                    pVmxTransient->ExitInstrInfo.StrIo.iSegReg, true /*fIoChecked*/);
                else
                {
                    /*
                     * The segment prefix for INS cannot be overridden and is always ES. We can safely assume X86_SREG_ES.
                     * Hence "iSegReg" field is undefined in the instruction-information field in VT-x for INS.
                     * See Intel Instruction spec. for "INS".
                     * See Intel spec. Table 27-8 "Format of the VM-Exit Instruction-Information Field as Used for INS and OUTS".
                     */
                    rcStrict = IEMExecStringIoRead(pVCpu, cbValue, enmAddrMode, fRep, cbInstr, true /*fIoChecked*/);
                }
            }
            else
                rcStrict = IEMExecOne(pVCpu);

            ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP);
            fUpdateRipAlready = true;
        }
        else
        {
            /*
             * IN/OUT - I/O instruction.
             */
            Log4Func(("cs:rip=%04x:%08RX64 %#06x/%u %c\n", pCtx->cs.Sel, pCtx->rip, uIOPort, cbValue, fIOWrite ? 'w' : 'r'));
            uint32_t const uAndVal = s_aIOOpAnd[uIOSize];
            Assert(!VMX_EXIT_QUAL_IO_IS_REP(pVmxTransient->uExitQual));
            if (fIOWrite)
            {
                rcStrict = IOMIOPortWrite(pVM, pVCpu, uIOPort, pCtx->eax & uAndVal, cbValue);
                STAM_COUNTER_INC(&pVCpu->hm.s.StatExitIOWrite);
                if (    rcStrict == VINF_IOM_R3_IOPORT_WRITE
                    && !pCtx->eflags.Bits.u1TF)
                    rcStrict = EMRZSetPendingIoPortWrite(pVCpu, uIOPort, cbInstr, cbValue, pCtx->eax & uAndVal);
            }
            else
            {
                uint32_t u32Result = 0;
                rcStrict = IOMIOPortRead(pVM, pVCpu, uIOPort, &u32Result, cbValue);
                if (IOM_SUCCESS(rcStrict))
                {
                    /* Save result of I/O IN instr. in AL/AX/EAX. */
                    pCtx->eax = (pCtx->eax & ~uAndVal) | (u32Result & uAndVal);
                }
                if (    rcStrict == VINF_IOM_R3_IOPORT_READ
                    && !pCtx->eflags.Bits.u1TF)
                    rcStrict = EMRZSetPendingIoPortRead(pVCpu, uIOPort, cbInstr, cbValue);
                STAM_COUNTER_INC(&pVCpu->hm.s.StatExitIORead);
            }
        }

        if (IOM_SUCCESS(rcStrict))
        {
            if (!fUpdateRipAlready)
            {
                hmR0VmxAdvanceGuestRipBy(pVCpu, cbInstr);
                ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP);
            }

            /*
             * INS/OUTS with REP prefix updates RFLAGS, can be observed with triple-fault guru
             * while booting Fedora 17 64-bit guest.
             *
             * See Intel Instruction reference for REP/REPE/REPZ/REPNE/REPNZ.
             */
            if (fIOString)
                ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RFLAGS);

            /*
             * If any I/O breakpoints are armed, we need to check if one triggered
             * and take appropriate action.
             * Note that the I/O breakpoint type is undefined if CR4.DE is 0.
             */
            rc = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, CPUMCTX_EXTRN_DR7);
            AssertRCReturn(rc, rc);

            /** @todo Optimize away the DBGFBpIsHwIoArmed call by having DBGF tell the
             *  execution engines about whether hyper BPs and such are pending. */
            uint32_t const uDr7 = pCtx->dr[7];
            if (RT_UNLIKELY(   (   (uDr7 & X86_DR7_ENABLED_MASK)
                                && X86_DR7_ANY_RW_IO(uDr7)
                                && (pCtx->cr4 & X86_CR4_DE))
                            || DBGFBpIsHwIoArmed(pVM)))
            {
                STAM_COUNTER_INC(&pVCpu->hm.s.StatDRxIoCheck);

                /* We're playing with the host CPU state here, make sure we don't preempt or longjmp. */
                VMMRZCallRing3Disable(pVCpu);
                HM_DISABLE_PREEMPT(pVCpu);

                bool fIsGuestDbgActive = CPUMR0DebugStateMaybeSaveGuest(pVCpu, true /* fDr6 */);

                VBOXSTRICTRC rcStrict2 = DBGFBpCheckIo(pVM, pVCpu, pCtx, uIOPort, cbValue);
                if (rcStrict2 == VINF_EM_RAW_GUEST_TRAP)
                {
                    /* Raise #DB. */
                    if (fIsGuestDbgActive)
                        ASMSetDR6(pCtx->dr[6]);
                    if (pCtx->dr[7] != uDr7)
                        pVCpu->hm.s.fCtxChanged |= HM_CHANGED_GUEST_DR7;

                    hmR0VmxSetPendingXcptDB(pVCpu);
                }
                /* rcStrict is VINF_SUCCESS, VINF_IOM_R3_IOPORT_COMMIT_WRITE, or in [VINF_EM_FIRST..VINF_EM_LAST],
                   however we can ditch VINF_IOM_R3_IOPORT_COMMIT_WRITE as it has VMCPU_FF_IOM as backup. */
                else if (   rcStrict2 != VINF_SUCCESS
                         && (rcStrict == VINF_SUCCESS || rcStrict2 < rcStrict))
                    rcStrict = rcStrict2;
                AssertCompile(VINF_EM_LAST < VINF_IOM_R3_IOPORT_COMMIT_WRITE);

                HM_RESTORE_PREEMPT();
                VMMRZCallRing3Enable(pVCpu);
            }
        }

#ifdef VBOX_STRICT
        if (   rcStrict == VINF_IOM_R3_IOPORT_READ
            || rcStrict == VINF_EM_PENDING_R3_IOPORT_READ)
            Assert(!fIOWrite);
        else if (   rcStrict == VINF_IOM_R3_IOPORT_WRITE
                 || rcStrict == VINF_IOM_R3_IOPORT_COMMIT_WRITE
                 || rcStrict == VINF_EM_PENDING_R3_IOPORT_WRITE)
            Assert(fIOWrite);
        else
        {
# if 0 /** @todo r=bird: This is missing a bunch of VINF_EM_FIRST..VINF_EM_LAST
           *        statuses, that the VMM device and some others may return. See
           *        IOM_SUCCESS() for guidance. */
            AssertMsg(   RT_FAILURE(rcStrict)
                      || rcStrict == VINF_SUCCESS
                      || rcStrict == VINF_EM_RAW_EMULATE_INSTR
                      || rcStrict == VINF_EM_DBG_BREAKPOINT
                      || rcStrict == VINF_EM_RAW_GUEST_TRAP
                      || rcStrict == VINF_EM_RAW_TO_R3
                      || rcStrict == VINF_TRPM_XCPT_DISPATCHED, ("%Rrc\n", VBOXSTRICTRC_VAL(rcStrict)));
# endif
        }
#endif
        STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatExitIO, y1);
    }
    else
    {
        /*
         * Frequent exit or something needing probing.  Get state and call EMHistoryExec.
         */
        int rc2 = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, HMVMX_CPUMCTX_EXTRN_ALL);
        AssertRCReturn(rc2, rc2);
        STAM_COUNTER_INC(!fIOString ? fIOWrite ? &pVCpu->hm.s.StatExitIOWrite : &pVCpu->hm.s.StatExitIORead
                         : fIOWrite ? &pVCpu->hm.s.StatExitIOStringWrite : &pVCpu->hm.s.StatExitIOStringRead);
        Log4(("IOExit/%u: %04x:%08RX64: %s%s%s %#x LB %u -> EMHistoryExec\n",
              pVCpu->idCpu, pVCpu->cpum.GstCtx.cs.Sel, pVCpu->cpum.GstCtx.rip,
              VMX_EXIT_QUAL_IO_IS_REP(pVmxTransient->uExitQual) ? "REP " : "",
              fIOWrite ? "OUT" : "IN", fIOString ? "S" : "", uIOPort, uIOSize));

        rcStrict = EMHistoryExec(pVCpu, pExitRec, 0);
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_ALL_GUEST);

        Log4(("IOExit/%u: %04x:%08RX64: EMHistoryExec -> %Rrc + %04x:%08RX64\n",
              pVCpu->idCpu, pVCpu->cpum.GstCtx.cs.Sel, pVCpu->cpum.GstCtx.rip,
              VBOXSTRICTRC_VAL(rcStrict), pVCpu->cpum.GstCtx.cs.Sel, pVCpu->cpum.GstCtx.rip));
    }
    return rcStrict;
}


/**
 * VM-exit handler for task switches (VMX_EXIT_TASK_SWITCH). Unconditional
 * VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitTaskSwitch(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    /* Check if this task-switch occurred while delivery an event through the guest IDT. */
    int rc = hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
    AssertRCReturn(rc, rc);
    if (VMX_EXIT_QUAL_TASK_SWITCH_TYPE(pVmxTransient->uExitQual) == VMX_EXIT_QUAL_TASK_SWITCH_TYPE_IDT)
    {
        rc = hmR0VmxReadIdtVectoringInfoVmcs(pVmxTransient);
        AssertRCReturn(rc, rc);
        if (VMX_IDT_VECTORING_INFO_IS_VALID(pVmxTransient->uIdtVectoringInfo))
        {
            uint32_t       uErrCode;
            RTGCUINTPTR    GCPtrFaultAddress;
            uint32_t const uIntType        = VMX_IDT_VECTORING_INFO_TYPE(pVmxTransient->uIdtVectoringInfo);
            uint32_t const uVector         = VMX_IDT_VECTORING_INFO_VECTOR(pVmxTransient->uIdtVectoringInfo);
            bool const     fErrorCodeValid = VMX_IDT_VECTORING_INFO_IS_ERROR_CODE_VALID(pVmxTransient->uIdtVectoringInfo);
            if (fErrorCodeValid)
            {
                rc = hmR0VmxReadIdtVectoringErrorCodeVmcs(pVmxTransient);
                AssertRCReturn(rc, rc);
                uErrCode = pVmxTransient->uIdtVectoringErrorCode;
            }
            else
                uErrCode = 0;

            if (   uIntType == VMX_IDT_VECTORING_INFO_TYPE_HW_XCPT
                && uVector == X86_XCPT_PF)
                GCPtrFaultAddress = pVCpu->cpum.GstCtx.cr2;
            else
                GCPtrFaultAddress = 0;

            rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
            AssertRCReturn(rc, rc);

            hmR0VmxSetPendingEvent(pVCpu, VMX_ENTRY_INT_INFO_FROM_EXIT_IDT_INFO(pVmxTransient->uIdtVectoringInfo),
                                   pVmxTransient->cbInstr, uErrCode, GCPtrFaultAddress);

            Log4Func(("Pending event. uIntType=%#x uVector=%#x\n", uIntType, uVector));
            STAM_COUNTER_INC(&pVCpu->hm.s.StatExitTaskSwitch);
            return VINF_EM_RAW_INJECT_TRPM_EVENT;
        }
    }

    /* Fall back to the interpreter to emulate the task-switch. */
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitTaskSwitch);
    return VERR_EM_INTERPRETER;
}


/**
 * VM-exit handler for monitor-trap-flag (VMX_EXIT_MTF). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitMtf(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    PVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    pVmcsInfo->u32ProcCtls &= ~VMX_PROC_CTLS_MONITOR_TRAP_FLAG;
    int rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC, pVmcsInfo->u32ProcCtls);
    AssertRCReturn(rc, rc);
    return VINF_EM_DBG_STEPPED;
}


/**
 * VM-exit handler for APIC access (VMX_EXIT_APIC_ACCESS). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitApicAccess(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitApicAccess);

    /* If this VM-exit occurred while delivering an event through the guest IDT, handle it accordingly. */
    VBOXSTRICTRC rcStrict1 = hmR0VmxCheckExitDueToEventDelivery(pVCpu, pVmxTransient);
    if (RT_LIKELY(rcStrict1 == VINF_SUCCESS))
    {
        /* For some crazy guest, if an event delivery causes an APIC-access VM-exit, go to instruction emulation. */
        if (RT_UNLIKELY(pVCpu->hm.s.Event.fPending))
        {
            STAM_COUNTER_INC(&pVCpu->hm.s.StatInjectPendingInterpret);
            return VINF_EM_RAW_INJECT_TRPM_EVENT;
        }
    }
    else
    {
        if (rcStrict1 == VINF_HM_DOUBLE_FAULT)
            rcStrict1 = VINF_SUCCESS;
        return rcStrict1;
    }

    /* IOMMIOPhysHandler() below may call into IEM, save the necessary state. */
    PVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    int rc  = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, IEM_CPUMCTX_EXTRN_MUST_MASK);
    rc     |= hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
    AssertRCReturn(rc, rc);

    /* See Intel spec. 27-6 "Exit Qualifications for APIC-access VM-exits from Linear Accesses & Guest-Phyiscal Addresses" */
    uint32_t uAccessType = VMX_EXIT_QUAL_APIC_ACCESS_TYPE(pVmxTransient->uExitQual);
    VBOXSTRICTRC rcStrict2;
    switch (uAccessType)
    {
        case VMX_APIC_ACCESS_TYPE_LINEAR_WRITE:
        case VMX_APIC_ACCESS_TYPE_LINEAR_READ:
        {
            AssertMsg(   !(pVmcsInfo->u32ProcCtls & VMX_PROC_CTLS_USE_TPR_SHADOW)
                      || VMX_EXIT_QUAL_APIC_ACCESS_OFFSET(pVmxTransient->uExitQual) != XAPIC_OFF_TPR,
                      ("hmR0VmxExitApicAccess: can't access TPR offset while using TPR shadowing.\n"));

            RTGCPHYS GCPhys = pVCpu->hm.s.vmx.u64GstMsrApicBase;    /* Always up-to-date, as it is not part of the VMCS. */
            GCPhys &= PAGE_BASE_GC_MASK;
            GCPhys += VMX_EXIT_QUAL_APIC_ACCESS_OFFSET(pVmxTransient->uExitQual);
            PVM pVM = pVCpu->CTX_SUFF(pVM);
            Log4Func(("Linear access uAccessType=%#x GCPhys=%#RGp Off=%#x\n", uAccessType, GCPhys,
                 VMX_EXIT_QUAL_APIC_ACCESS_OFFSET(pVmxTransient->uExitQual)));

            PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
            rcStrict2 = IOMMMIOPhysHandler(pVM, pVCpu,
                                           uAccessType == VMX_APIC_ACCESS_TYPE_LINEAR_READ ? 0 : X86_TRAP_PF_RW,
                                           CPUMCTX2CORE(pCtx), GCPhys);
            Log4Func(("IOMMMIOPhysHandler returned %Rrc\n", VBOXSTRICTRC_VAL(rcStrict2)));
            if (   rcStrict2 == VINF_SUCCESS
                || rcStrict2 == VERR_PAGE_TABLE_NOT_PRESENT
                || rcStrict2 == VERR_PAGE_NOT_PRESENT)
            {
                ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RSP | HM_CHANGED_GUEST_RFLAGS
                                                         | HM_CHANGED_GUEST_APIC_TPR);
                rcStrict2 = VINF_SUCCESS;
            }
            break;
        }

        default:
            Log4Func(("uAccessType=%#x\n", uAccessType));
            rcStrict2 = VINF_EM_RAW_EMULATE_INSTR;
            break;
    }

    if (rcStrict2 != VINF_SUCCESS)
        STAM_COUNTER_INC(&pVCpu->hm.s.StatSwitchApicAccessToR3);
    return rcStrict2;
}


/**
 * VM-exit handler for debug-register accesses (VMX_EXIT_MOV_DRX). Conditional
 * VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitMovDRx(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    /* We should -not- get this VM-exit if the guest's debug registers were active. */
    if (pVmxTransient->fWasGuestDebugStateActive)
    {
        AssertMsgFailed(("Unexpected MOV DRx exit\n"));
        HMVMX_UNEXPECTED_EXIT_RET(pVCpu, pVmxTransient->uExitReason);
    }

    PVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    if (   !pVCpu->hm.s.fSingleInstruction
        && !pVmxTransient->fWasHyperDebugStateActive)
    {
        Assert(!DBGFIsStepping(pVCpu));
        Assert(pVmcsInfo->u32XcptBitmap & RT_BIT(X86_XCPT_DB));

        /* Don't intercept MOV DRx any more. */
        pVmcsInfo->u32ProcCtls &= ~VMX_PROC_CTLS_MOV_DR_EXIT;
        int rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC, pVmcsInfo->u32ProcCtls);
        AssertRCReturn(rc, rc);

        /* We're playing with the host CPU state here, make sure we can't preempt or longjmp. */
        VMMRZCallRing3Disable(pVCpu);
        HM_DISABLE_PREEMPT(pVCpu);

        /* Save the host & load the guest debug state, restart execution of the MOV DRx instruction. */
        CPUMR0LoadGuestDebugState(pVCpu, true /* include DR6 */);
        Assert(CPUMIsGuestDebugStateActive(pVCpu) || HC_ARCH_BITS == 32);

        HM_RESTORE_PREEMPT();
        VMMRZCallRing3Enable(pVCpu);

#ifdef VBOX_WITH_STATISTICS
        rc = hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
        AssertRCReturn(rc, rc);
        if (VMX_EXIT_QUAL_DRX_DIRECTION(pVmxTransient->uExitQual) == VMX_EXIT_QUAL_DRX_DIRECTION_WRITE)
            STAM_COUNTER_INC(&pVCpu->hm.s.StatExitDRxWrite);
        else
            STAM_COUNTER_INC(&pVCpu->hm.s.StatExitDRxRead);
#endif
        STAM_COUNTER_INC(&pVCpu->hm.s.StatDRxContextSwitch);
        return VINF_SUCCESS;
    }

    /*
     * EMInterpretDRx[Write|Read]() calls CPUMIsGuestIn64BitCode() which requires EFER MSR, CS.
     * The EFER MSR is always up-to-date.
     * Update the segment registers and DR7 from the CPU.
     */
    PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
    int rc = hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
    rc    |= hmR0VmxImportGuestState(pVCpu, pVmcsInfo, CPUMCTX_EXTRN_SREG_MASK | CPUMCTX_EXTRN_DR7);
    AssertRCReturn(rc, rc);
    Log4Func(("cs:rip=%#04x:%#RX64\n", pCtx->cs.Sel, pCtx->rip));

    PVM pVM = pVCpu->CTX_SUFF(pVM);
    if (VMX_EXIT_QUAL_DRX_DIRECTION(pVmxTransient->uExitQual) == VMX_EXIT_QUAL_DRX_DIRECTION_WRITE)
    {
        rc = EMInterpretDRxWrite(pVM, pVCpu, CPUMCTX2CORE(pCtx),
                                 VMX_EXIT_QUAL_DRX_REGISTER(pVmxTransient->uExitQual),
                                 VMX_EXIT_QUAL_DRX_GENREG(pVmxTransient->uExitQual));
        if (RT_SUCCESS(rc))
            ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_DR7);
        STAM_COUNTER_INC(&pVCpu->hm.s.StatExitDRxWrite);
    }
    else
    {
        rc = EMInterpretDRxRead(pVM, pVCpu, CPUMCTX2CORE(pCtx),
                                VMX_EXIT_QUAL_DRX_GENREG(pVmxTransient->uExitQual),
                                VMX_EXIT_QUAL_DRX_REGISTER(pVmxTransient->uExitQual));
        STAM_COUNTER_INC(&pVCpu->hm.s.StatExitDRxRead);
    }

    Assert(rc == VINF_SUCCESS || rc == VERR_EM_INTERPRETER);
    if (RT_SUCCESS(rc))
    {
        int rc2 = hmR0VmxAdvanceGuestRip(pVCpu, pVmxTransient);
        AssertRCReturn(rc2, rc2);
        return VINF_SUCCESS;
    }
    return rc;
}


/**
 * VM-exit handler for EPT misconfiguration (VMX_EXIT_EPT_MISCONFIG).
 * Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitEptMisconfig(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);
    Assert(pVCpu->CTX_SUFF(pVM)->hm.s.fNestedPaging);

    /* If this VM-exit occurred while delivering an event through the guest IDT, handle it accordingly. */
    VBOXSTRICTRC rcStrict1 = hmR0VmxCheckExitDueToEventDelivery(pVCpu, pVmxTransient);
    if (RT_LIKELY(rcStrict1 == VINF_SUCCESS))
    {
        /* If event delivery causes an EPT misconfig (MMIO), go back to instruction emulation as otherwise
           injecting the original pending event would most likely cause the same EPT misconfig VM-exit. */
        if (RT_UNLIKELY(pVCpu->hm.s.Event.fPending))
        {
            STAM_COUNTER_INC(&pVCpu->hm.s.StatInjectPendingInterpret);
            return VINF_EM_RAW_INJECT_TRPM_EVENT;
        }
    }
    else
    {
        if (rcStrict1 == VINF_HM_DOUBLE_FAULT)
            rcStrict1 = VINF_SUCCESS;
        return rcStrict1;
    }

    /*
     * Get sufficent state and update the exit history entry.
     */
    RTGCPHYS GCPhys;
    PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    int rc = VMXReadVmcs64(VMX_VMCS64_RO_GUEST_PHYS_ADDR_FULL, &GCPhys);
    rc    |= hmR0VmxImportGuestState(pVCpu, pVmcsInfo, IEM_CPUMCTX_EXTRN_MUST_MASK);
    AssertRCReturn(rc, rc);

    VBOXSTRICTRC rcStrict;
    PCEMEXITREC pExitRec = EMHistoryUpdateFlagsAndTypeAndPC(pVCpu,
                                                            EMEXIT_MAKE_FT(EMEXIT_F_KIND_EM | EMEXIT_F_HM, EMEXITTYPE_MMIO),
                                                            pVCpu->cpum.GstCtx.rip + pVCpu->cpum.GstCtx.cs.u64Base);
    if (!pExitRec)
    {
        /*
         * If we succeed, resume guest execution.
         * If we fail in interpreting the instruction because we couldn't get the guest physical address
         * of the page containing the instruction via the guest's page tables (we would invalidate the guest page
         * in the host TLB), resume execution which would cause a guest page fault to let the guest handle this
         * weird case. See @bugref{6043}.
         */
        PVM      pVM  = pVCpu->CTX_SUFF(pVM);
        PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
        rcStrict = PGMR0Trap0eHandlerNPMisconfig(pVM, pVCpu, PGMMODE_EPT, CPUMCTX2CORE(pCtx), GCPhys, UINT32_MAX);
        Log4Func(("At %#RGp RIP=%#RX64 rc=%Rrc\n", GCPhys, pCtx->rip, VBOXSTRICTRC_VAL(rcStrict)));
        if (   rcStrict == VINF_SUCCESS
            || rcStrict == VERR_PAGE_TABLE_NOT_PRESENT
            || rcStrict == VERR_PAGE_NOT_PRESENT)
        {
            /* Successfully handled MMIO operation. */
            ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RSP | HM_CHANGED_GUEST_RFLAGS
                                                     | HM_CHANGED_GUEST_APIC_TPR);
            rcStrict = VINF_SUCCESS;
        }
    }
    else
    {
        /*
         * Frequent exit or something needing probing.  Get state and call EMHistoryExec.
         */
        int rc2 = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, IEM_CPUMCTX_EXTRN_MUST_MASK);
        AssertRCReturn(rc2, rc2);

        Log4(("EptMisscfgExit/%u: %04x:%08RX64: %RGp -> EMHistoryExec\n",
              pVCpu->idCpu, pVCpu->cpum.GstCtx.cs.Sel, pVCpu->cpum.GstCtx.rip, GCPhys));

        rcStrict = EMHistoryExec(pVCpu, pExitRec, 0);
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_ALL_GUEST);

        Log4(("EptMisscfgExit/%u: %04x:%08RX64: EMHistoryExec -> %Rrc + %04x:%08RX64\n",
              pVCpu->idCpu, pVCpu->cpum.GstCtx.cs.Sel, pVCpu->cpum.GstCtx.rip,
              VBOXSTRICTRC_VAL(rcStrict), pVCpu->cpum.GstCtx.cs.Sel, pVCpu->cpum.GstCtx.rip));
    }
    return VBOXSTRICTRC_TODO(rcStrict);
}


/**
 * VM-exit handler for EPT violation (VMX_EXIT_EPT_VIOLATION). Conditional
 * VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitEptViolation(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);
    Assert(pVCpu->CTX_SUFF(pVM)->hm.s.fNestedPaging);

    /* If this VM-exit occurred while delivering an event through the guest IDT, handle it accordingly. */
    VBOXSTRICTRC rcStrict1 = hmR0VmxCheckExitDueToEventDelivery(pVCpu, pVmxTransient);
    if (RT_LIKELY(rcStrict1 == VINF_SUCCESS))
    {
        /* In the unlikely case that the EPT violation happened as a result of delivering an event, log it. */
        if (RT_UNLIKELY(pVCpu->hm.s.Event.fPending))
            Log4Func(("EPT violation with an event pending u64IntInfo=%#RX64\n", pVCpu->hm.s.Event.u64IntInfo));
    }
    else
    {
        if (rcStrict1 == VINF_HM_DOUBLE_FAULT)
            rcStrict1 = VINF_SUCCESS;
        return rcStrict1;
    }

    RTGCPHYS GCPhys;
    PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    int rc  = VMXReadVmcs64(VMX_VMCS64_RO_GUEST_PHYS_ADDR_FULL, &GCPhys);
    rc     |= hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
    rc     |= hmR0VmxImportGuestState(pVCpu, pVmcsInfo, IEM_CPUMCTX_EXTRN_MUST_MASK);
    AssertRCReturn(rc, rc);

    /* Intel spec. Table 27-7 "Exit Qualifications for EPT violations". */
    AssertMsg(((pVmxTransient->uExitQual >> 7) & 3) != 2, ("%#RX64", pVmxTransient->uExitQual));

    RTGCUINT uErrorCode = 0;
    if (pVmxTransient->uExitQual & VMX_EXIT_QUAL_EPT_INSTR_FETCH)
        uErrorCode |= X86_TRAP_PF_ID;
    if (pVmxTransient->uExitQual & VMX_EXIT_QUAL_EPT_DATA_WRITE)
        uErrorCode |= X86_TRAP_PF_RW;
    if (pVmxTransient->uExitQual & VMX_EXIT_QUAL_EPT_ENTRY_PRESENT)
        uErrorCode |= X86_TRAP_PF_P;

    TRPMAssertXcptPF(pVCpu, GCPhys, uErrorCode);


    /* Handle the pagefault trap for the nested shadow table. */
    PVM      pVM  = pVCpu->CTX_SUFF(pVM);
    PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;

    Log4Func(("EPT violation %#x at %#RX64 ErrorCode %#x cs:rip=%#04x:%#RX64\n", pVmxTransient->uExitQual, GCPhys, uErrorCode,
              pCtx->cs.Sel, pCtx->rip));

    VBOXSTRICTRC rcStrict2 = PGMR0Trap0eHandlerNestedPaging(pVM, pVCpu, PGMMODE_EPT, uErrorCode, CPUMCTX2CORE(pCtx), GCPhys);
    TRPMResetTrap(pVCpu);

    /* Same case as PGMR0Trap0eHandlerNPMisconfig(). See comment above, @bugref{6043}. */
    if (   rcStrict2 == VINF_SUCCESS
        || rcStrict2 == VERR_PAGE_TABLE_NOT_PRESENT
        || rcStrict2 == VERR_PAGE_NOT_PRESENT)
    {
        /* Successfully synced our nested page tables. */
        STAM_COUNTER_INC(&pVCpu->hm.s.StatExitReasonNpf);
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RSP | HM_CHANGED_GUEST_RFLAGS);
        return VINF_SUCCESS;
    }

    Log4Func(("EPT return to ring-3 rcStrict2=%Rrc\n", VBOXSTRICTRC_VAL(rcStrict2)));
    return rcStrict2;
}

/** @} */

/* -=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-= */
/* -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-= VM-exit exception handlers =-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=- */
/* -=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-= */

/**
 * VM-exit exception handler for \#MF (Math Fault: floating point exception).
 */
static VBOXSTRICTRC hmR0VmxExitXcptMF(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_XCPT_HANDLER_PARAMS(pVCpu, pVmxTransient);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestMF);

    int rc = hmR0VmxImportGuestState(pVCpu, pVmxTransient->pVmcsInfo, CPUMCTX_EXTRN_CR0);
    AssertRCReturn(rc, rc);

    if (!(pVCpu->cpum.GstCtx.cr0 & X86_CR0_NE))
    {
        /* Convert a #MF into a FERR -> IRQ 13. See @bugref{6117}. */
        rc = PDMIsaSetIrq(pVCpu->CTX_SUFF(pVM), 13, 1, 0 /* uTagSrc */);

        /** @todo r=ramshankar: The Intel spec. does -not- specify that this VM-exit
         *        provides VM-exit instruction length. If this causes problem later,
         *        disassemble the instruction like it's done on AMD-V. */
        int rc2 = hmR0VmxAdvanceGuestRip(pVCpu, pVmxTransient);
        AssertRCReturn(rc2, rc2);
        return rc;
    }

    hmR0VmxSetPendingEvent(pVCpu, VMX_ENTRY_INT_INFO_FROM_EXIT_INT_INFO(pVmxTransient->uExitIntInfo), pVmxTransient->cbInstr,
                           pVmxTransient->uExitIntErrorCode, 0 /* GCPtrFaultAddress */);
    return rc;
}


/**
 * VM-exit exception handler for \#BP (Breakpoint exception).
 */
static VBOXSTRICTRC hmR0VmxExitXcptBP(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_XCPT_HANDLER_PARAMS(pVCpu, pVmxTransient);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestBP);

    int rc = hmR0VmxImportGuestState(pVCpu, pVmxTransient->pVmcsInfo, HMVMX_CPUMCTX_EXTRN_ALL);
    AssertRCReturn(rc, rc);

    PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
    rc = DBGFRZTrap03Handler(pVCpu->CTX_SUFF(pVM), pVCpu, CPUMCTX2CORE(pCtx));
    if (rc == VINF_EM_RAW_GUEST_TRAP)
    {
        rc  = hmR0VmxReadExitIntInfoVmcs(pVmxTransient);
        rc |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        rc |= hmR0VmxReadExitIntErrorCodeVmcs(pVmxTransient);
        AssertRCReturn(rc, rc);

        hmR0VmxSetPendingEvent(pVCpu, VMX_ENTRY_INT_INFO_FROM_EXIT_INT_INFO(pVmxTransient->uExitIntInfo), pVmxTransient->cbInstr,
                               pVmxTransient->uExitIntErrorCode, 0 /* GCPtrFaultAddress */);
    }

    Assert(rc == VINF_SUCCESS || rc == VINF_EM_RAW_GUEST_TRAP || rc == VINF_EM_DBG_BREAKPOINT);
    return rc;
}


/**
 * VM-exit exception handler for \#AC (alignment check exception).
 */
static VBOXSTRICTRC hmR0VmxExitXcptAC(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_XCPT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    /*
     * Re-inject it. We'll detect any nesting before getting here.
     */
    int rc = hmR0VmxReadExitIntErrorCodeVmcs(pVmxTransient);
    rc    |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);
    Assert(ASMAtomicUoReadU32(&pVmxTransient->fVmcsFieldsRead) & HMVMX_READ_EXIT_INTERRUPTION_INFO);

    hmR0VmxSetPendingEvent(pVCpu, VMX_ENTRY_INT_INFO_FROM_EXIT_INT_INFO(pVmxTransient->uExitIntInfo), pVmxTransient->cbInstr,
                           pVmxTransient->uExitIntErrorCode, 0 /* GCPtrFaultAddress */);
    return VINF_SUCCESS;
}


/**
 * VM-exit exception handler for \#DB (Debug exception).
 */
static VBOXSTRICTRC hmR0VmxExitXcptDB(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_XCPT_HANDLER_PARAMS(pVCpu, pVmxTransient);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestDB);

    /*
     * Get the DR6-like values from the Exit qualification and pass it to DBGF for processing.
     */
    int rc = hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);

    /* Refer Intel spec. Table 27-1. "Exit Qualifications for debug exceptions" for the format. */
    uint64_t const uDR6 = X86_DR6_INIT_VAL
                        | (pVmxTransient->uExitQual & (  X86_DR6_B0 | X86_DR6_B1 | X86_DR6_B2 | X86_DR6_B3
                                                       | X86_DR6_BD | X86_DR6_BS));

    PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
    rc = DBGFRZTrap01Handler(pVCpu->CTX_SUFF(pVM), pVCpu, CPUMCTX2CORE(pCtx), uDR6, pVCpu->hm.s.fSingleInstruction);
    Log6Func(("rc=%Rrc\n", rc));
    if (rc == VINF_EM_RAW_GUEST_TRAP)
    {
        /*
         * The exception was for the guest.  Update DR6, DR7.GD and
         * IA32_DEBUGCTL.LBR before forwarding it.
         * See Intel spec. 27.1 "Architectural State before a VM-Exit".
         */
        VMMRZCallRing3Disable(pVCpu);
        HM_DISABLE_PREEMPT(pVCpu);

        pCtx->dr[6] &= ~X86_DR6_B_MASK;
        pCtx->dr[6] |= uDR6;
        if (CPUMIsGuestDebugStateActive(pVCpu))
            ASMSetDR6(pCtx->dr[6]);

        HM_RESTORE_PREEMPT();
        VMMRZCallRing3Enable(pVCpu);

        rc = hmR0VmxImportGuestState(pVCpu, pVmxTransient->pVmcsInfo, CPUMCTX_EXTRN_DR7);
        AssertRCReturn(rc, rc);

        /* X86_DR7_GD will be cleared if DRx accesses should be trapped inside the guest. */
        pCtx->dr[7] &= ~X86_DR7_GD;

        /* Paranoia. */
        pCtx->dr[7] &= ~X86_DR7_RAZ_MASK;
        pCtx->dr[7] |= X86_DR7_RA1_MASK;

        rc = VMXWriteVmcs32(VMX_VMCS_GUEST_DR7, (uint32_t)pCtx->dr[7]);
        AssertRCReturn(rc, rc);

        /*
         * Raise #DB in the guest.
         *
         * It is important to reflect exactly what the VM-exit gave us (preserving the
         * interruption-type) rather than use hmR0VmxSetPendingXcptDB() as the #DB could've
         * been raised while executing ICEBP (INT1) and not the regular #DB. Thus it may
         * trigger different handling in the CPU (like skipping DPL checks), see @bugref{6398}.
         *
         * Intel re-documented ICEBP/INT1 on May 2018 previously documented as part of
         * Intel 386, see Intel spec. 24.8.3 "VM-Entry Controls for Event Injection".
         */
        rc  = hmR0VmxReadExitIntInfoVmcs(pVmxTransient);
        rc |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        rc |= hmR0VmxReadExitIntErrorCodeVmcs(pVmxTransient);
        AssertRCReturn(rc, rc);
        hmR0VmxSetPendingEvent(pVCpu, VMX_ENTRY_INT_INFO_FROM_EXIT_INT_INFO(pVmxTransient->uExitIntInfo), pVmxTransient->cbInstr,
                               pVmxTransient->uExitIntErrorCode, 0 /* GCPtrFaultAddress */);
        return VINF_SUCCESS;
    }

    /*
     * Not a guest trap, must be a hypervisor related debug event then.
     * Update DR6 in case someone is interested in it.
     */
    AssertMsg(rc == VINF_EM_DBG_STEPPED || rc == VINF_EM_DBG_BREAKPOINT, ("%Rrc\n", rc));
    AssertReturn(pVmxTransient->fWasHyperDebugStateActive, VERR_HM_IPE_5);
    CPUMSetHyperDR6(pVCpu, uDR6);

    return rc;
}


/**
 * Hacks its way around the lovely mesa driver's backdoor accesses.
 *
 * @sa hmR0SvmHandleMesaDrvGp.
 */
static int hmR0VmxHandleMesaDrvGp(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient, PCPUMCTX pCtx)
{
    LogFunc(("cs:rip=%#04x:%#RX64 rcx=%#RX64 rbx=%#RX64\n", pCtx->cs.Sel, pCtx->rip, pCtx->rcx, pCtx->rbx));
    RT_NOREF(pCtx);

    /* For now we'll just skip the instruction. */
    return hmR0VmxAdvanceGuestRip(pVCpu, pVmxTransient);
}


/**
 * Checks if the \#GP'ing instruction is the mesa driver doing it's lovely
 * backdoor logging w/o checking what it is running inside.
 *
 * This recognizes an "IN EAX,DX" instruction executed in flat ring-3, with the
 * backdoor port and magic numbers loaded in registers.
 *
 * @returns true if it is, false if it isn't.
 * @sa      hmR0SvmIsMesaDrvGp.
 */
DECLINLINE(bool) hmR0VmxIsMesaDrvGp(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient, PCPUMCTX pCtx)
{
    /* 0xed:  IN eAX,dx */
    uint8_t abInstr[1];
    if (pVmxTransient->cbInstr != sizeof(abInstr))
        return false;

    /* Check that it is #GP(0). */
    if (pVmxTransient->uExitIntErrorCode != 0)
        return false;

    /* Check magic and port. */
    Assert(!(pCtx->fExtrn & (CPUMCTX_EXTRN_RAX | CPUMCTX_EXTRN_RDX | CPUMCTX_EXTRN_RCX)));
    /*Log(("hmR0VmxIsMesaDrvGp: rax=%RX64 rdx=%RX64\n", pCtx->rax, pCtx->rdx));*/
    if (pCtx->rax != UINT32_C(0x564d5868))
        return false;
    if (pCtx->dx != UINT32_C(0x5658))
        return false;

    /* Flat ring-3 CS. */
    AssertCompile(HMVMX_CPUMCTX_EXTRN_ALL & CPUMCTX_EXTRN_CS);
    Assert(!(pCtx->fExtrn & CPUMCTX_EXTRN_CS));
    /*Log(("hmR0VmxIsMesaDrvGp: cs.Attr.n.u2Dpl=%d base=%Rx64\n", pCtx->cs.Attr.n.u2Dpl, pCtx->cs.u64Base));*/
    if (pCtx->cs.Attr.n.u2Dpl != 3)
        return false;
    if (pCtx->cs.u64Base != 0)
        return false;

    /* Check opcode. */
    AssertCompile(HMVMX_CPUMCTX_EXTRN_ALL & CPUMCTX_EXTRN_RIP);
    Assert(!(pCtx->fExtrn & CPUMCTX_EXTRN_RIP));
    int rc = PGMPhysSimpleReadGCPtr(pVCpu, abInstr, pCtx->rip, sizeof(abInstr));
    /*Log(("hmR0VmxIsMesaDrvGp: PGMPhysSimpleReadGCPtr -> %Rrc %#x\n", rc, abInstr[0]));*/
    if (RT_FAILURE(rc))
        return false;
    if (abInstr[0] != 0xed)
        return false;

    return true;
}


/**
 * VM-exit exception handler for \#GP (General-protection exception).
 *
 * @remarks Requires pVmxTransient->uExitIntInfo to be up-to-date.
 */
static VBOXSTRICTRC hmR0VmxExitXcptGP(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_XCPT_HANDLER_PARAMS(pVCpu, pVmxTransient);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestGP);

    PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
    PVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    if (pVmcsInfo->RealMode.fRealOnV86Active)
    { /* likely */ }
    else
    {
#ifndef HMVMX_ALWAYS_TRAP_ALL_XCPTS
        Assert(pVCpu->hm.s.fUsingDebugLoop || pVCpu->hm.s.fTrapXcptGpForLovelyMesaDrv || pVmxTransient->fIsNestedGuest);
#endif
        /* If the guest is not in real-mode or we have unrestricted guest execution support, reflect #GP to the guest. */
        int rc  = hmR0VmxReadExitIntInfoVmcs(pVmxTransient);
        rc     |= hmR0VmxReadExitIntErrorCodeVmcs(pVmxTransient);
        rc     |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        rc     |= hmR0VmxImportGuestState(pVCpu, pVmcsInfo, HMVMX_CPUMCTX_EXTRN_ALL);
        AssertRCReturn(rc, rc);
        Log4Func(("Gst: cs:rip=%#04x:%#RX64 ErrorCode=%#x cr0=%#RX64 cpl=%u tr=%#04x\n", pCtx->cs.Sel, pCtx->rip,
                  pVmxTransient->uExitIntErrorCode, pCtx->cr0, CPUMGetGuestCPL(pVCpu), pCtx->tr.Sel));

        if (    pVmxTransient->fIsNestedGuest
            || !pVCpu->hm.s.fTrapXcptGpForLovelyMesaDrv
            || !hmR0VmxIsMesaDrvGp(pVCpu, pVmxTransient, pCtx))
            hmR0VmxSetPendingEvent(pVCpu, VMX_ENTRY_INT_INFO_FROM_EXIT_INT_INFO(pVmxTransient->uExitIntInfo),
                                   pVmxTransient->cbInstr, pVmxTransient->uExitIntErrorCode, 0 /* GCPtrFaultAddress */);
        else
            rc = hmR0VmxHandleMesaDrvGp(pVCpu, pVmxTransient, pCtx);
        return rc;
    }

    Assert(CPUMIsGuestInRealModeEx(pCtx));
    Assert(!pVCpu->CTX_SUFF(pVM)->hm.s.vmx.fUnrestrictedGuest);
    Assert(!pVmxTransient->fIsNestedGuest);

    int rc = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, HMVMX_CPUMCTX_EXTRN_ALL);
    AssertRCReturn(rc, rc);

    VBOXSTRICTRC rcStrict = IEMExecOne(pVCpu);
    if (rcStrict == VINF_SUCCESS)
    {
        if (!CPUMIsGuestInRealModeEx(pCtx))
        {
            /*
             * The guest is no longer in real-mode, check if we can continue executing the
             * guest using hardware-assisted VMX. Otherwise, fall back to emulation.
             */
            pVmcsInfo->RealMode.fRealOnV86Active = false;
            if (HMCanExecuteVmxGuest(pVCpu, pCtx))
            {
                Log4Func(("Mode changed but guest still suitable for executing using hardware-assisted VMX\n"));
                ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_ALL_GUEST);
            }
            else
            {
                Log4Func(("Mode changed -> VINF_EM_RESCHEDULE\n"));
                rcStrict = VINF_EM_RESCHEDULE;
            }
        }
        else
            ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_ALL_GUEST);
    }
    else if (rcStrict == VINF_IEM_RAISED_XCPT)
    {
        rcStrict = VINF_SUCCESS;
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_RAISED_XCPT_MASK);
    }
    return VBOXSTRICTRC_VAL(rcStrict);
}


/**
 * VM-exit exception handler wrapper for generic exceptions. Simply re-injects
 * the exception reported in the VMX transient structure back into the VM.
 *
 * @remarks Requires uExitIntInfo in the VMX transient structure to be
 *          up-to-date.
 */
static VBOXSTRICTRC hmR0VmxExitXcptGeneric(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_XCPT_HANDLER_PARAMS(pVCpu, pVmxTransient);
#ifndef HMVMX_ALWAYS_TRAP_ALL_XCPTS
    PCVMXVMCSINFO pVmcsInfo = pVmxTransient->pVmcsInfo;
    AssertMsg(pVCpu->hm.s.fUsingDebugLoop || pVmcsInfo->RealMode.fRealOnV86Active || pVmxTransient->fIsNestedGuest,
              ("uVector=%#x u32XcptBitmap=%#X32\n",
               VMX_EXIT_INT_INFO_VECTOR(pVmxTransient->uExitIntInfo), pVmcsInfo->u32XcptBitmap));
    NOREF(pVmcsInfo);
#endif

    /*
     * Re-inject the exception into the guest. This cannot be a double-fault condition which
     * would have been handled while checking exits due to event delivery.
     */
    int rc = hmR0VmxReadExitIntErrorCodeVmcs(pVmxTransient);
    rc    |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);
    Assert(ASMAtomicUoReadU32(&pVmxTransient->fVmcsFieldsRead) & HMVMX_READ_EXIT_INTERRUPTION_INFO);

#ifdef DEBUG_ramshankar
    rc |= hmR0VmxImportGuestState(pVCpu, pVmxTransient->pVmcsInfo, CPUMCTX_EXTRN_CS | CPUMCTX_EXTRN_RIP);
    Log(("hmR0VmxExitXcptGeneric: Reinjecting Xcpt. uVector=%#x cs:rip=%#04x:%#RX64\n",
         VMX_EXIT_INT_INFO_VECTOR(pVmxTransient->uExitIntInfo), pCtx->cs.Sel, pCtx->rip));
#endif

    hmR0VmxSetPendingEvent(pVCpu, VMX_ENTRY_INT_INFO_FROM_EXIT_INT_INFO(pVmxTransient->uExitIntInfo), pVmxTransient->cbInstr,
                           pVmxTransient->uExitIntErrorCode, 0 /* GCPtrFaultAddress */);
    return VINF_SUCCESS;
}


/**
 * VM-exit exception handler for \#PF (Page-fault exception).
 */
static VBOXSTRICTRC hmR0VmxExitXcptPF(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_XCPT_HANDLER_PARAMS(pVCpu, pVmxTransient);
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    int rc = hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
    rc    |= hmR0VmxReadExitIntInfoVmcs(pVmxTransient);
    rc    |= hmR0VmxReadExitIntErrorCodeVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);

    if (!pVM->hm.s.fNestedPaging)
    { /* likely */ }
    else
    {
#if !defined(HMVMX_ALWAYS_TRAP_ALL_XCPTS) && !defined(HMVMX_ALWAYS_TRAP_PF)
        Assert(pVCpu->hm.s.fUsingDebugLoop);
#endif
        pVCpu->hm.s.Event.fPending = false;                  /* In case it's a contributory or vectoring #PF. */
        if (RT_LIKELY(!pVmxTransient->fVectoringDoublePF))
        {
            hmR0VmxSetPendingEvent(pVCpu, VMX_ENTRY_INT_INFO_FROM_EXIT_INT_INFO(pVmxTransient->uExitIntInfo), 0 /* cbInstr */,
                                   pVmxTransient->uExitIntErrorCode, pVmxTransient->uExitQual);
        }
        else
        {
            /* A guest page-fault occurred during delivery of a page-fault. Inject #DF. */
            hmR0VmxSetPendingXcptDF(pVCpu);
            Log4Func(("Pending #DF due to vectoring #PF w/ NestedPaging\n"));
        }
        STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestPF);
        return rc;
    }

    /* If it's a vectoring #PF, emulate injecting the original event injection as PGMTrap0eHandler() is incapable
       of differentiating between instruction emulation and event injection that caused a #PF. See @bugref{6607}. */
    if (pVmxTransient->fVectoringPF)
    {
        Assert(pVCpu->hm.s.Event.fPending);
        return VINF_EM_RAW_INJECT_TRPM_EVENT;
    }

    PCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
    rc = hmR0VmxImportGuestState(pVCpu, pVmxTransient->pVmcsInfo, HMVMX_CPUMCTX_EXTRN_ALL);
    AssertRCReturn(rc, rc);

    Log4Func(("#PF: cr2=%#RX64 cs:rip=%#04x:%#RX64 uErrCode %#RX32 cr3=%#RX64\n", pVmxTransient->uExitQual, pCtx->cs.Sel,
              pCtx->rip, pVmxTransient->uExitIntErrorCode, pCtx->cr3));

    TRPMAssertXcptPF(pVCpu, pVmxTransient->uExitQual, (RTGCUINT)pVmxTransient->uExitIntErrorCode);
    rc = PGMTrap0eHandler(pVCpu, pVmxTransient->uExitIntErrorCode, CPUMCTX2CORE(pCtx), (RTGCPTR)pVmxTransient->uExitQual);

    Log4Func(("#PF: rc=%Rrc\n", rc));
    if (rc == VINF_SUCCESS)
    {
        /*
         * This is typically a shadow page table sync or a MMIO instruction. But we may have
         * emulated something like LTR or a far jump. Any part of the CPU context may have changed.
         */
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_ALL_GUEST);
        TRPMResetTrap(pVCpu);
        STAM_COUNTER_INC(&pVCpu->hm.s.StatExitShadowPF);
        return rc;
    }

    if (rc == VINF_EM_RAW_GUEST_TRAP)
    {
        if (!pVmxTransient->fVectoringDoublePF)
        {
            /* It's a guest page fault and needs to be reflected to the guest. */
            uint32_t uGstErrorCode = TRPMGetErrorCode(pVCpu);
            TRPMResetTrap(pVCpu);
            pVCpu->hm.s.Event.fPending = false;                 /* In case it's a contributory #PF. */
            hmR0VmxSetPendingEvent(pVCpu, VMX_ENTRY_INT_INFO_FROM_EXIT_INT_INFO(pVmxTransient->uExitIntInfo), 0 /* cbInstr */,
                                   uGstErrorCode, pVmxTransient->uExitQual);
        }
        else
        {
            /* A guest page-fault occurred during delivery of a page-fault. Inject #DF. */
            TRPMResetTrap(pVCpu);
            pVCpu->hm.s.Event.fPending = false;     /* Clear pending #PF to replace it with #DF. */
            hmR0VmxSetPendingXcptDF(pVCpu);
            Log4Func(("#PF: Pending #DF due to vectoring #PF\n"));
        }

        STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestPF);
        return VINF_SUCCESS;
    }

    TRPMResetTrap(pVCpu);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitShadowPFEM);
    return rc;
}


/**
 * VM-exit helper for LMSW.
 */
static VBOXSTRICTRC hmR0VmxExitLmsw(PVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo, uint8_t cbInstr, uint16_t uMsw,
                                    RTGCPTR GCPtrEffDst)
{
    int rc = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, IEM_CPUMCTX_EXTRN_MUST_MASK);
    AssertRCReturn(rc, rc);

    VBOXSTRICTRC rcStrict = IEMExecDecodedLmsw(pVCpu, cbInstr, uMsw, GCPtrEffDst);
    AssertMsg(   rcStrict == VINF_SUCCESS
              || rcStrict == VINF_IEM_RAISED_XCPT, ("%Rrc\n", VBOXSTRICTRC_VAL(rcStrict)));

    ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS | HM_CHANGED_GUEST_CR0);
    if (rcStrict == VINF_IEM_RAISED_XCPT)
    {
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_RAISED_XCPT_MASK);
        rcStrict = VINF_SUCCESS;
    }

    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitLmsw);
    Log4Func(("rcStrict=%Rrc\n", VBOXSTRICTRC_VAL(rcStrict)));
    return rcStrict;
}


/**
 * VM-exit helper for CLTS.
 */
static VBOXSTRICTRC hmR0VmxExitClts(PVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo, uint8_t cbInstr)
{
    int rc = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, IEM_CPUMCTX_EXTRN_MUST_MASK);
    AssertRCReturn(rc, rc);

    VBOXSTRICTRC rcStrict = IEMExecDecodedClts(pVCpu, cbInstr);
    AssertMsg(   rcStrict == VINF_SUCCESS
              || rcStrict == VINF_IEM_RAISED_XCPT, ("%Rrc\n", VBOXSTRICTRC_VAL(rcStrict)));

    ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS | HM_CHANGED_GUEST_CR0);
    if (rcStrict == VINF_IEM_RAISED_XCPT)
    {
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_RAISED_XCPT_MASK);
        rcStrict = VINF_SUCCESS;
    }

    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitClts);
    Log4Func(("rcStrict=%Rrc\n", VBOXSTRICTRC_VAL(rcStrict)));
    return rcStrict;
}


/**
 * VM-exit helper for MOV from CRx (CRx read).
 */
static VBOXSTRICTRC hmR0VmxExitMovFromCrX(PVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo, uint8_t cbInstr, uint8_t iGReg, uint8_t iCrReg)
{
    Assert(iCrReg < 16);
    Assert(iGReg < RT_ELEMENTS(pVCpu->cpum.GstCtx.aGRegs));

    int rc = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, IEM_CPUMCTX_EXTRN_MUST_MASK);
    AssertRCReturn(rc, rc);

    VBOXSTRICTRC rcStrict = IEMExecDecodedMovCRxRead(pVCpu, cbInstr, iGReg, iCrReg);
    AssertMsg(   rcStrict == VINF_SUCCESS
              || rcStrict == VINF_IEM_RAISED_XCPT, ("%Rrc\n", VBOXSTRICTRC_VAL(rcStrict)));

    if (iGReg == X86_GREG_xSP)
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS | HM_CHANGED_GUEST_RSP);
    else
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS);
#ifdef VBOX_WITH_STATISTICS
    switch (iCrReg)
    {
        case 0: STAM_COUNTER_INC(&pVCpu->hm.s.StatExitCR0Read); break;
        case 2: STAM_COUNTER_INC(&pVCpu->hm.s.StatExitCR2Read); break;
        case 3: STAM_COUNTER_INC(&pVCpu->hm.s.StatExitCR3Read); break;
        case 4: STAM_COUNTER_INC(&pVCpu->hm.s.StatExitCR4Read); break;
        case 8: STAM_COUNTER_INC(&pVCpu->hm.s.StatExitCR8Read); break;
    }
#endif
    Log4Func(("CR%d Read access rcStrict=%Rrc\n", iCrReg, VBOXSTRICTRC_VAL(rcStrict)));
    return rcStrict;
}


/**
 * VM-exit helper for MOV to CRx (CRx write).
 */
static VBOXSTRICTRC hmR0VmxExitMovToCrX(PVMCPU pVCpu, PCVMXVMCSINFO pVmcsInfo, uint8_t cbInstr, uint8_t iGReg, uint8_t iCrReg)
{
    int rc = hmR0VmxImportGuestState(pVCpu, pVmcsInfo, IEM_CPUMCTX_EXTRN_MUST_MASK);
    AssertRCReturn(rc, rc);

    VBOXSTRICTRC rcStrict = IEMExecDecodedMovCRxWrite(pVCpu, cbInstr, iCrReg, iGReg);
    AssertMsg(   rcStrict == VINF_SUCCESS
              || rcStrict == VINF_IEM_RAISED_XCPT
              || rcStrict == VINF_PGM_SYNC_CR3, ("%Rrc\n", VBOXSTRICTRC_VAL(rcStrict)));

    switch (iCrReg)
    {
        case 0:
        {
            ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS | HM_CHANGED_GUEST_CR0);
            STAM_COUNTER_INC(&pVCpu->hm.s.StatExitCR0Write);
            Log4Func(("CR0 write. rcStrict=%Rrc CR0=%#RX64\n", VBOXSTRICTRC_VAL(rcStrict), pVCpu->cpum.GstCtx.cr0));
            break;
        }

        case 2:
        {
            STAM_COUNTER_INC(&pVCpu->hm.s.StatExitCR2Write);
            /* Nothing to do here, CR2 it's not part of the VMCS. */
            break;
        }

        case 3:
        {
            ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS | HM_CHANGED_GUEST_CR3);
            STAM_COUNTER_INC(&pVCpu->hm.s.StatExitCR3Write);
            Log4Func(("CR3 write. rcStrict=%Rrc CR3=%#RX64\n", VBOXSTRICTRC_VAL(rcStrict), pVCpu->cpum.GstCtx.cr3));
            break;
        }

        case 4:
        {
            ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS | HM_CHANGED_GUEST_CR4);
            STAM_COUNTER_INC(&pVCpu->hm.s.StatExitCR4Write);
            Log4Func(("CR4 write. rc=%Rrc CR4=%#RX64 fLoadSaveGuestXcr0=%u\n", VBOXSTRICTRC_VAL(rcStrict),
                      pVCpu->cpum.GstCtx.cr4, pVCpu->hm.s.fLoadSaveGuestXcr0));
            break;
        }

        case 8:
        {
            ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged,
                             HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS | HM_CHANGED_GUEST_APIC_TPR);
            STAM_COUNTER_INC(&pVCpu->hm.s.StatExitCR8Write);
            break;
        }

        default:
            AssertMsgFailed(("Invalid CRx register %#x\n", iCrReg));
            break;
    }

    if (rcStrict == VINF_IEM_RAISED_XCPT)
    {
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_RAISED_XCPT_MASK);
        rcStrict = VINF_SUCCESS;
    }
    return rcStrict;
}


/**
 * VM-exit helper for handling host NMIs.
 */
static VBOXSTRICTRC hmR0VmxExitHostNmi(PVMCPU pVCpu)
{
    VMXDispatchHostNmi();

    STAM_REL_COUNTER_INC(&pVCpu->hm.s.StatExitHostNmiInGC);
    STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatExitXcptNmi, y3);
    return VINF_SUCCESS;
}


#ifdef VBOX_WITH_NESTED_HWVIRT_VMX
/** @name VMX instruction handlers.
 * @{
 */
/* -=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-= */
/* -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=- VMX instructions VM-exit handlers -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-= */
/* -=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-= */

/**
 * VM-exit handler for VMCLEAR (VMX_EXIT_VMCLEAR). Unconditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitVmclear(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxImportGuestState(pVCpu, pVmxTransient->pVmcsInfo, CPUMCTX_EXTRN_RSP | CPUMCTX_EXTRN_SREG_MASK
                                                                    | IEM_CPUMCTX_EXTRN_EXEC_DECODED_MEM_MASK);
    rc    |= hmR0VmxReadExitInstrInfoVmcs(pVmxTransient);
    rc    |= hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
    AssertRCReturn(rc, rc);

    HMVMX_CHECK_EXIT_DUE_TO_VMX_INSTR(pVCpu, pVmxTransient->uExitReason);

    VMXVEXITINFO ExitInfo;
    RT_ZERO(ExitInfo);
    ExitInfo.uReason     = pVmxTransient->uExitReason;
    ExitInfo.u64Qual     = pVmxTransient->uExitQual;
    ExitInfo.InstrInfo.u = pVmxTransient->ExitInstrInfo.u;
    ExitInfo.cbInstr     = pVmxTransient->cbInstr;
    HMVMX_DECODE_MEM_OPERAND(pVCpu, ExitInfo.InstrInfo.u, ExitInfo.u64Qual, VMXMEMACCESS_READ, &ExitInfo.GCPtrEffAddr);

    VBOXSTRICTRC rcStrict = IEMExecDecodedVmclear(pVCpu, &ExitInfo);
    if (RT_LIKELY(rcStrict == VINF_SUCCESS))
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS | HM_CHANGED_GUEST_HWVIRT);
    else if (rcStrict == VINF_IEM_RAISED_XCPT)
    {
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_RAISED_XCPT_MASK);
        rcStrict = VINF_SUCCESS;
    }
    return rcStrict;
}


/**
 * VM-exit handler for VMLAUNCH (VMX_EXIT_VMLAUNCH). Unconditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitVmlaunch(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    /* Import the entire VMCS state for now as we would be switching VMCS on successful VMLAUNCH,
       otherwise we could import just IEM_CPUMCTX_EXTRN_VMX_VMENTRY_MASK. */
    int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxImportGuestState(pVCpu, pVmxTransient->pVmcsInfo, HMVMX_CPUMCTX_EXTRN_ALL);
    AssertRCReturn(rc, rc);

    HMVMX_CHECK_EXIT_DUE_TO_VMX_INSTR(pVCpu, pVmxTransient->uExitReason);

    VBOXSTRICTRC rcStrict = IEMExecDecodedVmlaunchVmresume(pVCpu, pVmxTransient->cbInstr, VMXINSTRID_VMLAUNCH);
    if (RT_LIKELY(rcStrict == VINF_SUCCESS))
    {
        rcStrict = VINF_VMX_VMLAUNCH_VMRESUME;
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_ALL_GUEST);
    }
    Assert(rcStrict != VINF_IEM_RAISED_XCPT);
    return rcStrict;
}


/**
 * VM-exit handler for VMPTRLD (VMX_EXIT_VMPTRLD). Unconditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitVmptrld(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxImportGuestState(pVCpu, pVmxTransient->pVmcsInfo, CPUMCTX_EXTRN_RSP | CPUMCTX_EXTRN_SREG_MASK
                                                                    | IEM_CPUMCTX_EXTRN_EXEC_DECODED_MEM_MASK);
    rc    |= hmR0VmxReadExitInstrInfoVmcs(pVmxTransient);
    rc    |= hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
    AssertRCReturn(rc, rc);

    HMVMX_CHECK_EXIT_DUE_TO_VMX_INSTR(pVCpu, pVmxTransient->uExitReason);

    VMXVEXITINFO ExitInfo;
    RT_ZERO(ExitInfo);
    ExitInfo.uReason     = pVmxTransient->uExitReason;
    ExitInfo.u64Qual     = pVmxTransient->uExitQual;
    ExitInfo.InstrInfo.u = pVmxTransient->ExitInstrInfo.u;
    ExitInfo.cbInstr     = pVmxTransient->cbInstr;
    HMVMX_DECODE_MEM_OPERAND(pVCpu, ExitInfo.InstrInfo.u, ExitInfo.u64Qual, VMXMEMACCESS_READ, &ExitInfo.GCPtrEffAddr);

    VBOXSTRICTRC rcStrict = IEMExecDecodedVmptrld(pVCpu, &ExitInfo);
    if (RT_LIKELY(rcStrict == VINF_SUCCESS))
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS | HM_CHANGED_GUEST_HWVIRT);
    else if (rcStrict == VINF_IEM_RAISED_XCPT)
    {
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_RAISED_XCPT_MASK);
        rcStrict = VINF_SUCCESS;
    }
    return rcStrict;
}


/**
 * VM-exit handler for VMPTRST (VMX_EXIT_VMPTRST). Unconditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitVmptrst(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxImportGuestState(pVCpu, pVmxTransient->pVmcsInfo, CPUMCTX_EXTRN_RSP | CPUMCTX_EXTRN_SREG_MASK
                                                                    | IEM_CPUMCTX_EXTRN_EXEC_DECODED_MEM_MASK);
    rc    |= hmR0VmxReadExitInstrInfoVmcs(pVmxTransient);
    rc    |= hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
    AssertRCReturn(rc, rc);

    HMVMX_CHECK_EXIT_DUE_TO_VMX_INSTR(pVCpu, pVmxTransient->uExitReason);

    VMXVEXITINFO ExitInfo;
    RT_ZERO(ExitInfo);
    ExitInfo.uReason     = pVmxTransient->uExitReason;
    ExitInfo.u64Qual     = pVmxTransient->uExitQual;
    ExitInfo.InstrInfo.u = pVmxTransient->ExitInstrInfo.u;
    ExitInfo.cbInstr     = pVmxTransient->cbInstr;
    HMVMX_DECODE_MEM_OPERAND(pVCpu, ExitInfo.InstrInfo.u, ExitInfo.u64Qual, VMXMEMACCESS_WRITE, &ExitInfo.GCPtrEffAddr);

    VBOXSTRICTRC rcStrict = IEMExecDecodedVmptrst(pVCpu, &ExitInfo);
    if (RT_LIKELY(rcStrict == VINF_SUCCESS))
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS);
    else if (rcStrict == VINF_IEM_RAISED_XCPT)
    {
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_RAISED_XCPT_MASK);
        rcStrict = VINF_SUCCESS;
    }
    return rcStrict;
}


/**
 * VM-exit handler for VMREAD (VMX_EXIT_VMREAD). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitVmread(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxImportGuestState(pVCpu, pVmxTransient->pVmcsInfo, CPUMCTX_EXTRN_RSP | CPUMCTX_EXTRN_SREG_MASK
                                                                    | IEM_CPUMCTX_EXTRN_EXEC_DECODED_MEM_MASK);
    rc    |= hmR0VmxReadExitInstrInfoVmcs(pVmxTransient);
    rc    |= hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
    AssertRCReturn(rc, rc);

    HMVMX_CHECK_EXIT_DUE_TO_VMX_INSTR(pVCpu, pVmxTransient->uExitReason);

    VMXVEXITINFO ExitInfo;
    RT_ZERO(ExitInfo);
    ExitInfo.uReason     = pVmxTransient->uExitReason;
    ExitInfo.u64Qual     = pVmxTransient->uExitQual;
    ExitInfo.InstrInfo.u = pVmxTransient->ExitInstrInfo.u;
    ExitInfo.cbInstr     = pVmxTransient->cbInstr;
    if (!ExitInfo.InstrInfo.VmreadVmwrite.fIsRegOperand)
        HMVMX_DECODE_MEM_OPERAND(pVCpu, ExitInfo.InstrInfo.u, ExitInfo.u64Qual, VMXMEMACCESS_WRITE, &ExitInfo.GCPtrEffAddr);

    VBOXSTRICTRC rcStrict = IEMExecDecodedVmread(pVCpu, &ExitInfo);
    if (RT_LIKELY(rcStrict == VINF_SUCCESS))
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS);
    else if (rcStrict == VINF_IEM_RAISED_XCPT)
    {
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_RAISED_XCPT_MASK);
        rcStrict = VINF_SUCCESS;
    }
    return rcStrict;
}


/**
 * VM-exit handler for VMRESUME (VMX_EXIT_VMRESUME). Unconditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitVmresume(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    /* Import the entire VMCS state for now as we would be switching VMCS on successful VMRESUME,
       otherwise we could import just IEM_CPUMCTX_EXTRN_VMX_VMENTRY_MASK. */
    int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxImportGuestState(pVCpu, pVmxTransient->pVmcsInfo, HMVMX_CPUMCTX_EXTRN_ALL);
    AssertRCReturn(rc, rc);

    HMVMX_CHECK_EXIT_DUE_TO_VMX_INSTR(pVCpu, pVmxTransient->uExitReason);

    VBOXSTRICTRC rcStrict = IEMExecDecodedVmlaunchVmresume(pVCpu, pVmxTransient->cbInstr, VMXINSTRID_VMRESUME);
    if (RT_LIKELY(rcStrict == VINF_SUCCESS))
    {
        rcStrict = VINF_VMX_VMLAUNCH_VMRESUME;
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_ALL_GUEST);
    }
    Assert(rcStrict != VINF_IEM_RAISED_XCPT);
    return rcStrict;
}


/**
 * VM-exit handler for VMWRITE (VMX_EXIT_VMWRITE). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitVmwrite(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxImportGuestState(pVCpu, pVmxTransient->pVmcsInfo, CPUMCTX_EXTRN_RSP | CPUMCTX_EXTRN_SREG_MASK
                                                                    | IEM_CPUMCTX_EXTRN_EXEC_DECODED_MEM_MASK);
    rc    |= hmR0VmxReadExitInstrInfoVmcs(pVmxTransient);
    rc    |= hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
    AssertRCReturn(rc, rc);

    HMVMX_CHECK_EXIT_DUE_TO_VMX_INSTR(pVCpu, pVmxTransient->uExitReason);

    VMXVEXITINFO ExitInfo;
    RT_ZERO(ExitInfo);
    ExitInfo.uReason     = pVmxTransient->uExitReason;
    ExitInfo.u64Qual     = pVmxTransient->uExitQual;
    ExitInfo.InstrInfo.u = pVmxTransient->ExitInstrInfo.u;
    ExitInfo.cbInstr     = pVmxTransient->cbInstr;
    if (!ExitInfo.InstrInfo.VmreadVmwrite.fIsRegOperand)
        HMVMX_DECODE_MEM_OPERAND(pVCpu, ExitInfo.InstrInfo.u, ExitInfo.u64Qual, VMXMEMACCESS_READ, &ExitInfo.GCPtrEffAddr);

    VBOXSTRICTRC rcStrict = IEMExecDecodedVmwrite(pVCpu, &ExitInfo);
    if (RT_LIKELY(rcStrict == VINF_SUCCESS))
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS | HM_CHANGED_GUEST_HWVIRT);
    else if (rcStrict == VINF_IEM_RAISED_XCPT)
    {
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_RAISED_XCPT_MASK);
        rcStrict = VINF_SUCCESS;
    }
    return rcStrict;
}


/**
 * VM-exit handler for VMXOFF (VMX_EXIT_VMXOFF). Unconditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitVmxoff(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxImportGuestState(pVCpu, pVmxTransient->pVmcsInfo, CPUMCTX_EXTRN_CR4
                                                                    | IEM_CPUMCTX_EXTRN_EXEC_DECODED_NO_MEM_MASK);
    AssertRCReturn(rc, rc);

    HMVMX_CHECK_EXIT_DUE_TO_VMX_INSTR(pVCpu, pVmxTransient->uExitReason);

    VBOXSTRICTRC rcStrict = IEMExecDecodedVmxoff(pVCpu, pVmxTransient->cbInstr);
    if (RT_LIKELY(rcStrict == VINF_SUCCESS))
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_HWVIRT);
    else if (rcStrict == VINF_IEM_RAISED_XCPT)
    {
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_RAISED_XCPT_MASK);
        rcStrict = VINF_SUCCESS;
    }
    return rcStrict;
}


/**
 * VM-exit handler for VMXON (VMX_EXIT_VMXON). Unconditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitVmxon(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxImportGuestState(pVCpu, pVmxTransient->pVmcsInfo, CPUMCTX_EXTRN_RSP | CPUMCTX_EXTRN_SREG_MASK
                                                                    | IEM_CPUMCTX_EXTRN_EXEC_DECODED_MEM_MASK);
    rc    |= hmR0VmxReadExitInstrInfoVmcs(pVmxTransient);
    rc    |= hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
    AssertRCReturn(rc, rc);

    HMVMX_CHECK_EXIT_DUE_TO_VMX_INSTR(pVCpu, pVmxTransient->uExitReason);

    VMXVEXITINFO ExitInfo;
    RT_ZERO(ExitInfo);
    ExitInfo.uReason     = pVmxTransient->uExitReason;
    ExitInfo.u64Qual     = pVmxTransient->uExitQual;
    ExitInfo.InstrInfo.u = pVmxTransient->ExitInstrInfo.u;
    ExitInfo.cbInstr     = pVmxTransient->cbInstr;
    HMVMX_DECODE_MEM_OPERAND(pVCpu, ExitInfo.InstrInfo.u, ExitInfo.u64Qual, VMXMEMACCESS_READ, &ExitInfo.GCPtrEffAddr);

    VBOXSTRICTRC rcStrict = IEMExecDecodedVmxon(pVCpu, &ExitInfo);
    if (RT_LIKELY(rcStrict == VINF_SUCCESS))
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS | HM_CHANGED_GUEST_HWVIRT);
    else if (rcStrict == VINF_IEM_RAISED_XCPT)
    {
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_RAISED_XCPT_MASK);
        rcStrict = VINF_SUCCESS;
    }
    return rcStrict;
}


/**
 * VM-exit handler for INVVPID (VMX_EXIT_INVVPID). Unconditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitInvvpid(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxImportGuestState(pVCpu, pVmxTransient->pVmcsInfo, CPUMCTX_EXTRN_RSP | CPUMCTX_EXTRN_SREG_MASK
                                                                    | IEM_CPUMCTX_EXTRN_EXEC_DECODED_MEM_MASK);
    rc    |= hmR0VmxReadExitInstrInfoVmcs(pVmxTransient);
    rc    |= hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
    AssertRCReturn(rc, rc);

    HMVMX_CHECK_EXIT_DUE_TO_VMX_INSTR(pVCpu, pVmxTransient->uExitReason);

    VMXVEXITINFO ExitInfo;
    RT_ZERO(ExitInfo);
    ExitInfo.uReason     = pVmxTransient->uExitReason;
    ExitInfo.u64Qual     = pVmxTransient->uExitQual;
    ExitInfo.InstrInfo.u = pVmxTransient->ExitInstrInfo.u;
    ExitInfo.cbInstr     = pVmxTransient->cbInstr;
    HMVMX_DECODE_MEM_OPERAND(pVCpu, ExitInfo.InstrInfo.u, ExitInfo.u64Qual, VMXMEMACCESS_READ, &ExitInfo.GCPtrEffAddr);

    VBOXSTRICTRC rcStrict = IEMExecDecodedInvvpid(pVCpu, &ExitInfo);
    if (RT_LIKELY(rcStrict == VINF_SUCCESS))
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS);
    else if (rcStrict == VINF_IEM_RAISED_XCPT)
    {
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_RAISED_XCPT_MASK);
        rcStrict = VINF_SUCCESS;
    }
    return rcStrict;
}

/** @} */

/** @name Nested-guest VM-exit handlers.
 * @{
 */
/* -=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-= */
/* -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=- Nested-guest VM-exit handlers -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-= */
/* -=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-= */

/**
 * Nested-guest VM-exit handler for exceptions or NMIs (VMX_EXIT_XCPT_OR_NMI).
 * Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitXcptOrNmiNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    int rc = hmR0VmxReadExitIntInfoVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);

    uint64_t const uExitIntInfo = pVmxTransient->uExitIntInfo;
    Assert(VMX_EXIT_INT_INFO_IS_VALID(uExitIntInfo));
    uint32_t const uExtIntType = VMX_EXIT_INT_INFO_TYPE(uExitIntInfo);

    /*
     * Make sure not to use stale/previous VM-exit instruction length since we read the
     * instruction length from the VMCS below only for software exceptions and privileged
     * software exceptions but we pass it for all exception VM-exits below.
     */
    pVmxTransient->cbInstr = 0;

    switch (uExtIntType)
    {
        /*
         * Physical NMIs:
         *    We shouldn't direct host physical NMIs to the nested-guest. Dispatch it to the
         *    host.
         */
        case VMX_EXIT_INT_INFO_TYPE_NMI:
            return hmR0VmxExitHostNmi(pVCpu);

        /*
         * Hardware exceptions,
         * Software exceptions,
         * Privileged software exceptions:
         *    Figure out if the exception must be delivered to the guest or the nested-guest.
         *
         *    For VM-exits due to software exceptions (those generated by INT3 or INTO) and privileged
         *    software exceptions (those generated by INT1/ICEBP) we need to supply the VM-exit instruction
         *    length.
         */
        case VMX_EXIT_INT_INFO_TYPE_SW_XCPT:
        case VMX_EXIT_INT_INFO_TYPE_PRIV_SW_XCPT:
        {
            rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
            RT_FALL_THRU();
        }
        case VMX_EXIT_INT_INFO_TYPE_HW_XCPT:
        {
            rc = hmR0VmxReadExitIntErrorCodeVmcs(pVmxTransient);
            AssertRCReturn(rc, rc);

            uint8_t const uVector    = VMX_EXIT_INT_INFO_VECTOR(uExitIntInfo);
            bool const    fIntercept = CPUMIsGuestVmxXcptInterceptSet(pVCpu, &pVCpu->cpum.GstCtx, uVector,
                                                                      pVmxTransient->uExitIntErrorCode);
            if (fIntercept)
            {
                rc  = hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
                rc |= hmR0VmxReadIdtVectoringInfoVmcs(pVmxTransient);
                rc |= hmR0VmxReadIdtVectoringErrorCodeVmcs(pVmxTransient);
                AssertRCReturn(rc, rc);

                VMXVEXITINFO ExitInfo;
                RT_ZERO(ExitInfo);
                ExitInfo.cbInstr = pVmxTransient->cbInstr;
                ExitInfo.u64Qual = pVmxTransient->uExitQual;

                VMXVEXITEVENTINFO ExitEventInfo;
                RT_ZERO(ExitEventInfo);
                ExitEventInfo.uExitIntInfo         = pVmxTransient->uExitIntInfo;
                ExitEventInfo.uExitIntErrCode      = pVmxTransient->uExitIntErrorCode;
                ExitEventInfo.uIdtVectoringInfo    = pVmxTransient->uIdtVectoringInfo;
                ExitEventInfo.uIdtVectoringErrCode = pVmxTransient->uIdtVectoringErrorCode;

                return IEMExecVmxVmexitXcpt(pVCpu, &ExitInfo, &ExitEventInfo);
            }

            /* Nested paging is currently a requirement, otherwise we would need to handle shadow #PFs. */
            Assert(pVCpu->CTX_SUFF(pVM)->hm.s.fNestedPaging);

            /* If the guest hypervisor is not intercepting the exception, forward it to the guest. */
            hmR0VmxSetPendingEvent(pVCpu, VMX_ENTRY_INT_INFO_FROM_EXIT_INT_INFO(uExitIntInfo), pVmxTransient->cbInstr,
                                   pVmxTransient->uExitIntErrorCode, pVmxTransient->uExitQual);
            return VINF_SUCCESS;
        }

        /*
         * Software interrupts:
         *    VM-exits cannot be caused by software interrupts.
         *
         * External interrupts:
         *    This should only happen when "acknowledge external interrupts on VM-exit"
         *    control is set. However, we don't set it when executing guests or
         *    nested-guests. For nested-guests it is emulated while injecting interrupts into
         *    the guest.
         */
        case VMX_EXIT_INT_INFO_TYPE_SW_INT:
        case VMX_EXIT_INT_INFO_TYPE_EXT_INT:
        default:
        {
            pVCpu->hm.s.u32HMError = pVmxTransient->uExitIntInfo;
            return VERR_VMX_UNEXPECTED_INTERRUPTION_EXIT_TYPE;
        }
    }
}


/**
 * Nested-guest VM-exit handler for triple faults (VMX_EXIT_TRIPLE_FAULT).
 * Unconditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitTripleFaultNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);
    return IEMExecVmxVmexitTripleFault(pVCpu);
}


/**
 * Nested-guest VM-exit handler for interrupt-window exiting (VMX_EXIT_INT_WINDOW).
 */
HMVMX_EXIT_NSRC_DECL hmR0VmxExitIntWindowNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    if (CPUMIsGuestVmxProcCtlsSet(pVCpu, &pVCpu->cpum.GstCtx, VMX_PROC_CTLS_INT_WINDOW_EXIT))
        return IEMExecVmxVmexit(pVCpu, pVmxTransient->uExitReason, 0 /* uExitQual */);
    return hmR0VmxExitIntWindow(pVCpu, pVmxTransient);
}


/**
 * Nested-guest VM-exit handler for NMI-window exiting (VMX_EXIT_NMI_WINDOW).
 */
HMVMX_EXIT_NSRC_DECL hmR0VmxExitNmiWindowNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    if (CPUMIsGuestVmxProcCtlsSet(pVCpu, &pVCpu->cpum.GstCtx, VMX_PROC_CTLS_NMI_WINDOW_EXIT))
        return IEMExecVmxVmexit(pVCpu, pVmxTransient->uExitReason, 0 /* uExitQual */);
    return hmR0VmxExitIntWindow(pVCpu, pVmxTransient);
}


/**
 * Nested-guest VM-exit handler for task switches (VMX_EXIT_TASK_SWITCH).
 * Unconditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitTaskSwitchNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    int rc = hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
    rc    |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxReadIdtVectoringInfoVmcs(pVmxTransient);
    rc    |= hmR0VmxReadIdtVectoringErrorCodeVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);

    VMXVEXITINFO ExitInfo;
    RT_ZERO(ExitInfo);
    ExitInfo.cbInstr = pVmxTransient->cbInstr;
    ExitInfo.u64Qual = pVmxTransient->uExitQual;

    VMXVEXITEVENTINFO ExitEventInfo;
    RT_ZERO(ExitInfo);
    ExitEventInfo.uIdtVectoringInfo    = pVmxTransient->uIdtVectoringInfo;
    ExitEventInfo.uIdtVectoringErrCode = pVmxTransient->uIdtVectoringErrorCode;
    return IEMExecVmxVmexitTaskSwitch(pVCpu, &ExitInfo, &ExitEventInfo);
}


/**
 * Nested-guest VM-exit handler for HLT (VMX_EXIT_HLT). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitHltNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    if (CPUMIsGuestVmxProcCtlsSet(pVCpu, &pVCpu->cpum.GstCtx, VMX_PROC_CTLS_HLT_EXIT))
    {
        int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        AssertRCReturn(rc, rc);
        return IEMExecVmxVmexitInstr(pVCpu, pVmxTransient->uExitReason, pVmxTransient->cbInstr);
    }
    return hmR0VmxExitHlt(pVCpu, pVmxTransient);
}


/**
 * Nested-guest VM-exit handler for INVLPG (VMX_EXIT_INVLPG). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitInvlpgNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    if (CPUMIsGuestVmxProcCtlsSet(pVCpu, &pVCpu->cpum.GstCtx, VMX_PROC_CTLS_INVLPG_EXIT))
    {
        int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        rc    |= hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
        AssertRCReturn(rc, rc);

        VMXVEXITINFO ExitInfo;
        RT_ZERO(ExitInfo);
        ExitInfo.uReason   = pVmxTransient->uExitReason;
        ExitInfo.cbInstr   = pVmxTransient->cbInstr;
        ExitInfo.u64Qual   = pVmxTransient->uExitQual;
        return IEMExecVmxVmexitInstrWithInfo(pVCpu, &ExitInfo);
    }
    return hmR0VmxExitInvlpg(pVCpu, pVmxTransient);
}


/**
 * Nested-guest VM-exit handler for RDPMC (VMX_EXIT_RDPMC). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitRdpmcNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    if (CPUMIsGuestVmxProcCtlsSet(pVCpu, &pVCpu->cpum.GstCtx, VMX_PROC_CTLS_RDPMC_EXIT))
    {
        int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        AssertRCReturn(rc, rc);
        return IEMExecVmxVmexitInstr(pVCpu, pVmxTransient->uExitReason, pVmxTransient->cbInstr);
    }
    return hmR0VmxExitRdpmc(pVCpu, pVmxTransient);
}


/**
 * Nested-guest VM-exit handler for VMREAD (VMX_EXIT_VMREAD) and VMWRITE
 * (VMX_EXIT_VMWRITE). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitVmreadVmwriteNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    Assert(   pVmxTransient->uExitReason == VMX_EXIT_VMREAD
           || pVmxTransient->uExitReason == VMX_EXIT_VMWRITE);

    int rc = hmR0VmxReadExitInstrInfoVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);

    uint8_t const iGReg = pVmxTransient->ExitInstrInfo.VmreadVmwrite.iReg2;
    Assert(iGReg < RT_ELEMENTS(pVCpu->cpum.GstCtx.aGRegs));
    uint64_t u64FieldEnc = pVCpu->cpum.GstCtx.aGRegs[iGReg].u64;

    HMVMX_CPUMCTX_ASSERT(pVCpu, CPUMCTX_EXTRN_EFER);
    if (!CPUMIsGuestInLongModeEx(&pVCpu->cpum.GstCtx))
        u64FieldEnc &= UINT64_C(0xffffffff);

    if (CPUMIsGuestVmxVmreadVmwriteInterceptSet(pVCpu, pVmxTransient->uExitReason, u64FieldEnc))
    {
        rc |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        rc |= hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
        AssertRCReturn(rc, rc);

        VMXVEXITINFO ExitInfo;
        RT_ZERO(ExitInfo);
        ExitInfo.uReason   = pVmxTransient->uExitReason;
        ExitInfo.cbInstr   = pVmxTransient->cbInstr;
        ExitInfo.u64Qual   = pVmxTransient->uExitQual;
        ExitInfo.InstrInfo = pVmxTransient->ExitInstrInfo;
        return IEMExecVmxVmexitInstrWithInfo(pVCpu, &ExitInfo);
    }

    if (pVmxTransient->uExitReason == VMX_EXIT_VMREAD)
        return hmR0VmxExitVmread(pVCpu, pVmxTransient);
    return hmR0VmxExitVmwrite(pVCpu, pVmxTransient);
}


/**
 * Nested-guest VM-exit handler for RDTSC (VMX_EXIT_RDTSC). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitRdtscNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    if (CPUMIsGuestVmxProcCtlsSet(pVCpu, &pVCpu->cpum.GstCtx, VMX_PROC_CTLS_RDTSC_EXIT))
    {
        int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        AssertRCReturn(rc, rc);
        return IEMExecVmxVmexitInstr(pVCpu, pVmxTransient->uExitReason, pVmxTransient->cbInstr);
    }

    return hmR0VmxExitRdtsc(pVCpu, pVmxTransient);
}


/**
 * Nested-guest VM-exit handler for control-register accesses (VMX_EXIT_MOV_CRX).
 * Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitMovCRxNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    int rc  = hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
    rc     |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);

    VBOXSTRICTRC rcStrict;
    uint32_t const uAccessType = VMX_EXIT_QUAL_CRX_ACCESS(pVmxTransient->uExitQual);
    switch (uAccessType)
    {
        case VMX_EXIT_QUAL_CRX_ACCESS_WRITE:
        {
            uint8_t const iCrReg   = VMX_EXIT_QUAL_CRX_REGISTER(pVmxTransient->uExitQual);
            uint8_t const iGReg    = VMX_EXIT_QUAL_CRX_GENREG(pVmxTransient->uExitQual);
            Assert(iGReg < RT_ELEMENTS(pVCpu->cpum.GstCtx.aGRegs));
            uint64_t const uNewCrX = pVCpu->cpum.GstCtx.aGRegs[iGReg].u64;
            if (CPUMIsGuestVmxMovToCr0Cr4InterceptSet(pVCpu, &pVCpu->cpum.GstCtx, iCrReg, uNewCrX))
            {
                VMXVEXITINFO ExitInfo;
                RT_ZERO(ExitInfo);
                ExitInfo.uReason = pVmxTransient->uExitReason;
                ExitInfo.cbInstr = pVmxTransient->cbInstr;
                ExitInfo.u64Qual = pVmxTransient->uExitQual;
                rcStrict = IEMExecVmxVmexitInstrWithInfo(pVCpu, &ExitInfo);
            }
            else
                rcStrict = hmR0VmxExitMovToCrX(pVCpu, pVmxTransient->pVmcsInfo, pVmxTransient->cbInstr, iGReg, iCrReg);
            break;
        }

        case VMX_EXIT_QUAL_CRX_ACCESS_READ:
        {
            /*
             * CR0/CR4 reads do not cause VM-exits, the read-shadow is used (subject to masking).
             * CR2 reads do not cause a VM-exit.
             * CR3 reads cause a VM-exit depending on the "CR3 store exiting" control.
             * CR8 reads cause a VM-exit depending on the "CR8 store exiting" control.
             */
            uint8_t const iCrReg = VMX_EXIT_QUAL_CRX_REGISTER(pVmxTransient->uExitQual);
            if (   iCrReg == 3
                || iCrReg == 8)
            {
                static const uint32_t s_aCrXReadIntercepts[] = { 0, 0, 0, VMX_PROC_CTLS_CR3_STORE_EXIT, 0,
                                                                 0, 0, 0, VMX_PROC_CTLS_CR8_STORE_EXIT };
                uint32_t const uIntercept = s_aCrXReadIntercepts[iCrReg];
                if (CPUMIsGuestVmxProcCtlsSet(pVCpu, &pVCpu->cpum.GstCtx, uIntercept))
                {
                    VMXVEXITINFO ExitInfo;
                    RT_ZERO(ExitInfo);
                    ExitInfo.uReason = pVmxTransient->uExitReason;
                    ExitInfo.cbInstr = pVmxTransient->cbInstr;
                    ExitInfo.u64Qual = pVmxTransient->uExitQual;
                    rcStrict = IEMExecVmxVmexitInstrWithInfo(pVCpu, &ExitInfo);
                }
                else
                {
                    uint8_t const iGReg = VMX_EXIT_QUAL_CRX_GENREG(pVmxTransient->uExitQual);
                    rcStrict = hmR0VmxExitMovFromCrX(pVCpu, pVmxTransient->pVmcsInfo, pVmxTransient->cbInstr, iGReg, iCrReg);
                }
            }
            else
            {
                AssertMsgFailed(("MOV from CR%d VM-exit must not happen\n", iCrReg));
                HMVMX_UNEXPECTED_EXIT_RET(pVCpu, iCrReg);
            }
            break;
        }

        case VMX_EXIT_QUAL_CRX_ACCESS_CLTS:
        {
            PCVMXVVMCS pVmcsNstGst = pVCpu->cpum.GstCtx.hwvirt.vmx.CTX_SUFF(pVmcs);
            Assert(pVmcsNstGst);
            uint64_t const uGstHostMask = pVmcsNstGst->u64Cr0Mask.u;
            uint64_t const uReadShadow  = pVmcsNstGst->u64Cr0ReadShadow.u;
            if (   (uGstHostMask & X86_CR0_TS)
                && (uReadShadow  & X86_CR0_TS))
            {
                VMXVEXITINFO ExitInfo;
                RT_ZERO(ExitInfo);
                ExitInfo.uReason = pVmxTransient->uExitReason;
                ExitInfo.cbInstr = pVmxTransient->cbInstr;
                ExitInfo.u64Qual = pVmxTransient->uExitQual;
                rcStrict = IEMExecVmxVmexitInstrWithInfo(pVCpu, &ExitInfo);
            }
            else
                rcStrict = hmR0VmxExitClts(pVCpu, pVmxTransient->pVmcsInfo, pVmxTransient->cbInstr);
            break;
        }

        case VMX_EXIT_QUAL_CRX_ACCESS_LMSW:        /* LMSW (Load Machine-Status Word into CR0) */
        {
            RTGCPTR        GCPtrEffDst;
            uint16_t const uNewMsw     = VMX_EXIT_QUAL_CRX_LMSW_DATA(pVmxTransient->uExitQual);
            bool const     fMemOperand = VMX_EXIT_QUAL_CRX_LMSW_OP_MEM(pVmxTransient->uExitQual);
            if (fMemOperand)
            {
                rc = hmR0VmxReadGuestLinearAddrVmcs(pVCpu, pVmxTransient);
                AssertRCReturn(rc, rc);
                GCPtrEffDst = pVmxTransient->uGuestLinearAddr;
            }
            else
                GCPtrEffDst = NIL_RTGCPTR;

            if (CPUMIsGuestVmxLmswInterceptSet(pVCpu, &pVCpu->cpum.GstCtx, uNewMsw))
            {
                VMXVEXITINFO ExitInfo;
                RT_ZERO(ExitInfo);
                ExitInfo.uReason            = pVmxTransient->uExitReason;
                ExitInfo.cbInstr            = pVmxTransient->cbInstr;
                ExitInfo.u64GuestLinearAddr = GCPtrEffDst;
                ExitInfo.u64Qual            = pVmxTransient->uExitQual;
                rcStrict = IEMExecVmxVmexitInstrWithInfo(pVCpu, &ExitInfo);
            }
            else
                rcStrict = hmR0VmxExitLmsw(pVCpu, pVmxTransient->pVmcsInfo, pVmxTransient->cbInstr, uNewMsw, GCPtrEffDst);
            break;
        }

        default:
        {
            AssertMsgFailed(("Unrecognized Mov CRX access type %#x\n", uAccessType));
            HMVMX_UNEXPECTED_EXIT_RET(pVCpu, uAccessType);
        }
    }

    if (rcStrict == VINF_IEM_RAISED_XCPT)
    {
        ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_RAISED_XCPT_MASK);
        rcStrict = VINF_SUCCESS;
    }
    return rcStrict;
}


/**
 * Nested-guest VM-exit handler for debug-register accesses (VMX_EXIT_MOV_DRX).
 * Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitMovDRxNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    if (CPUMIsGuestVmxProcCtlsSet(pVCpu, &pVCpu->cpum.GstCtx, VMX_PROC_CTLS_MOV_DR_EXIT))
    {
        int rc = hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
        rc    |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        AssertRCReturn(rc, rc);

        VMXVEXITINFO ExitInfo;
        RT_ZERO(ExitInfo);
        ExitInfo.cbInstr = pVmxTransient->cbInstr;
        ExitInfo.u64Qual = pVmxTransient->uExitQual;
        return IEMExecVmxVmexitInstrWithInfo(pVCpu, &ExitInfo);
    }
    return hmR0VmxExitMovDRx(pVCpu, pVmxTransient);
}


/**
 * Nested-guest VM-exit handler for I/O instructions (VMX_EXIT_IO_INSTR).
 * Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitIoInstrNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    int rc = hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
    AssertRCReturn(rc, rc);

    uint32_t const uIOPort = VMX_EXIT_QUAL_IO_PORT(pVmxTransient->uExitQual);
    uint8_t  const uIOSize = VMX_EXIT_QUAL_IO_SIZE(pVmxTransient->uExitQual);
    AssertReturn(uIOSize <= 3 && uIOSize != 2, VERR_VMX_IPE_1);

    static uint32_t const s_aIOSizes[4] = { 1, 2, 0, 4 };   /* Size of the I/O accesses in bytes. */
    uint8_t const cbAccess = s_aIOSizes[uIOSize];
    if (CPUMIsGuestVmxIoInterceptSet(pVCpu, uIOPort, cbAccess))
    {
        /*
         * IN/OUT instruction:
         *   - Provides VM-exit instruction length.
         *
         * INS/OUTS instruction:
         *   - Provides VM-exit instruction length.
         *   - Provides Guest-linear address.
         *   - Optionally provides VM-exit instruction info (depends on CPU feature).
         */
        PVM pVM = pVCpu->CTX_SUFF(pVM);
        rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        AssertRCReturn(rc, rc);

        /* Make sure we don't use stale/uninitialized VMX-transient info. below. */
        pVmxTransient->ExitInstrInfo.u  = 0;
        pVmxTransient->uGuestLinearAddr = 0;

        bool const fVmxInsOutsInfo = pVM->cpum.ro.GuestFeatures.fVmxInsOutInfo;
        bool const fIOString       = VMX_EXIT_QUAL_IO_IS_STRING(pVmxTransient->uExitQual);
        if (fIOString)
        {
            rc |= hmR0VmxReadGuestLinearAddrVmcs(pVCpu, pVmxTransient);
            if (fVmxInsOutsInfo)
            {
                Assert(RT_BF_GET(pVM->hm.s.vmx.Msrs.u64Basic, VMX_BF_BASIC_VMCS_INS_OUTS)); /* Paranoia. */
                rc |= hmR0VmxReadExitInstrInfoVmcs(pVmxTransient);
            }
        }
        AssertRCReturn(rc, rc);

        VMXVEXITINFO ExitInfo;
        RT_ZERO(ExitInfo);
        ExitInfo.uReason            = pVmxTransient->uExitReason;
        ExitInfo.cbInstr            = pVmxTransient->cbInstr;
        ExitInfo.u64Qual            = pVmxTransient->uExitQual;
        ExitInfo.InstrInfo          = pVmxTransient->ExitInstrInfo;
        ExitInfo.u64GuestLinearAddr = pVmxTransient->uGuestLinearAddr;
        return IEMExecVmxVmexitInstrWithInfo(pVCpu, &ExitInfo);
    }
    return hmR0VmxExitIoInstr(pVCpu, pVmxTransient);
}


/**
 * Nested-guest VM-exit handler for RDMSR (VMX_EXIT_RDMSR).
 */
HMVMX_EXIT_DECL hmR0VmxExitRdmsrNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    uint32_t fMsrpm;
    if (CPUMIsGuestVmxProcCtlsSet(pVCpu, &pVCpu->cpum.GstCtx, VMX_PROC_CTLS_USE_MSR_BITMAPS))
        fMsrpm = CPUMGetVmxMsrPermission(pVCpu->cpum.GstCtx.hwvirt.vmx.CTX_SUFF(pvMsrBitmap), pVCpu->cpum.GstCtx.ecx);
    else
        fMsrpm = VMXMSRPM_EXIT_RD;

    if (fMsrpm & VMXMSRPM_EXIT_RD)
    {
        int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        AssertRCReturn(rc, rc);
        return IEMExecVmxVmexitInstr(pVCpu, pVmxTransient->uExitReason, pVmxTransient->cbInstr);
    }
    return hmR0VmxExitRdmsr(pVCpu, pVmxTransient);
}


/**
 * Nested-guest VM-exit handler for WRMSR (VMX_EXIT_WRMSR).
 */
HMVMX_EXIT_DECL hmR0VmxExitWrmsrNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    uint32_t fMsrpm;
    if (CPUMIsGuestVmxProcCtlsSet(pVCpu, &pVCpu->cpum.GstCtx, VMX_PROC_CTLS_USE_MSR_BITMAPS))
        fMsrpm = CPUMGetVmxMsrPermission(pVCpu->cpum.GstCtx.hwvirt.vmx.CTX_SUFF(pvMsrBitmap), pVCpu->cpum.GstCtx.ecx);
    else
        fMsrpm = VMXMSRPM_EXIT_WR;

    if (fMsrpm & VMXMSRPM_EXIT_WR)
    {
        int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        AssertRCReturn(rc, rc);
        return IEMExecVmxVmexitInstr(pVCpu, pVmxTransient->uExitReason, pVmxTransient->cbInstr);
    }
    return hmR0VmxExitWrmsr(pVCpu, pVmxTransient);
}


/**
 * Nested-guest VM-exit handler for MWAIT (VMX_EXIT_MWAIT). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitMwaitNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    if (CPUMIsGuestVmxProcCtlsSet(pVCpu, &pVCpu->cpum.GstCtx, VMX_PROC_CTLS_MWAIT_EXIT))
    {
        int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        AssertRCReturn(rc, rc);
        return IEMExecVmxVmexitInstr(pVCpu, pVmxTransient->uExitReason, pVmxTransient->cbInstr);
    }
    return hmR0VmxExitMwait(pVCpu, pVmxTransient);
}


/**
 * Nested-guest VM-exit handler for monitor-trap-flag (VMX_EXIT_MTF). Conditional
 * VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitMtfNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    /** @todo NSTVMX: Should consider debugging nested-guests using VM debugger. */
    return IEMExecVmxVmexit(pVCpu, pVmxTransient->uExitReason, 0 /* uExitQual */);
}


/**
 * Nested-guest VM-exit handler for MONITOR (VMX_EXIT_MONITOR). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitMonitorNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    if (CPUMIsGuestVmxProcCtlsSet(pVCpu, &pVCpu->cpum.GstCtx, VMX_PROC_CTLS_MONITOR_EXIT))
    {
        int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        AssertRCReturn(rc, rc);
        return IEMExecVmxVmexitInstr(pVCpu, pVmxTransient->uExitReason, pVmxTransient->cbInstr);
    }
    return hmR0VmxExitMonitor(pVCpu, pVmxTransient);
}


/**
 * Nested-guest VM-exit handler for PAUSE (VMX_EXIT_PAUSE). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitPauseNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    /** @todo NSTVMX: Think about this more. Does the outer guest need to intercept
     *        PAUSE when executing a nested-guest? If it does not, we would not need
     *        to check for the intercepts here. Just call VM-exit... */

    /* The CPU would have already performed the necessary CPL checks for PAUSE-loop exiting. */
    if (   CPUMIsGuestVmxProcCtlsSet(pVCpu, &pVCpu->cpum.GstCtx, VMX_PROC_CTLS_PAUSE_EXIT)
        || CPUMIsGuestVmxProcCtls2Set(pVCpu, &pVCpu->cpum.GstCtx, VMX_PROC_CTLS2_PAUSE_LOOP_EXIT))
    {
        int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        AssertRCReturn(rc, rc);
        return IEMExecVmxVmexitInstr(pVCpu, pVmxTransient->uExitReason, pVmxTransient->cbInstr);
    }
    return hmR0VmxExitPause(pVCpu, pVmxTransient);
}


/**
 * Nested-guest VM-exit handler for when the TPR value is lowered below the
 * specified threshold (VMX_EXIT_TPR_BELOW_THRESHOLD). Conditional VM-exit.
 */
HMVMX_EXIT_NSRC_DECL hmR0VmxExitTprBelowThresholdNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    if (CPUMIsGuestVmxProcCtlsSet(pVCpu, &pVCpu->cpum.GstCtx, VMX_PROC_CTLS_USE_TPR_SHADOW))
        return IEMExecVmxVmexit(pVCpu, pVmxTransient->uExitReason, 0 /* uExitQual */);
    return hmR0VmxExitTprBelowThreshold(pVCpu, pVmxTransient);
}


/**
 * Nested-guest VM-exit handler for APIC access (VMX_EXIT_APIC_ACCESS). Conditional
 * VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitApicAccessNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    Assert(CPUMIsGuestVmxProcCtls2Set(pVCpu, &pVCpu->cpum.GstCtx, VMX_PROC_CTLS2_VIRT_APIC_ACCESS));
    int rc = hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
    rc    |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxReadIdtVectoringInfoVmcs(pVmxTransient);
    rc    |= hmR0VmxReadIdtVectoringErrorCodeVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);

    VMXVEXITINFO ExitInfo;
    RT_ZERO(ExitInfo);
    ExitInfo.cbInstr = pVmxTransient->cbInstr;
    ExitInfo.u64Qual = pVmxTransient->uExitQual;

    VMXVEXITEVENTINFO ExitEventInfo;
    RT_ZERO(ExitInfo);
    ExitEventInfo.uIdtVectoringInfo    = pVmxTransient->uIdtVectoringInfo;
    ExitEventInfo.uIdtVectoringErrCode = pVmxTransient->uIdtVectoringErrorCode;
    return IEMExecVmxVmexitApicAccess(pVCpu, &ExitInfo, &ExitEventInfo);
}


/**
 * Nested-guest VM-exit handler for APIC write emulation (VMX_EXIT_APIC_WRITE).
 * Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitApicWriteNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    Assert(CPUMIsGuestVmxProcCtls2Set(pVCpu, &pVCpu->cpum.GstCtx, VMX_PROC_CTLS2_APIC_REG_VIRT));
    int rc = hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
    AssertRCReturn(rc, rc);

    return IEMExecVmxVmexit(pVCpu, pVmxTransient->uExitReason, pVmxTransient->uExitQual);
}


/**
 * Nested-guest VM-exit handler for virtualized EOI (VMX_EXIT_VIRTUALIZED_EOI).
 * Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitVirtEoiNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    Assert(CPUMIsGuestVmxProcCtls2Set(pVCpu, &pVCpu->cpum.GstCtx, VMX_PROC_CTLS2_VIRT_INT_DELIVERY));
    int rc = hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
    AssertRCReturn(rc, rc);

    return IEMExecVmxVmexit(pVCpu, pVmxTransient->uExitReason, pVmxTransient->uExitQual);
}


/**
 * Nested-guest VM-exit handler for RDTSCP (VMX_EXIT_RDTSCP). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitRdtscpNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    if (CPUMIsGuestVmxProcCtlsSet(pVCpu, &pVCpu->cpum.GstCtx, VMX_PROC_CTLS_RDTSC_EXIT))
    {
        Assert(CPUMIsGuestVmxProcCtls2Set(pVCpu, &pVCpu->cpum.GstCtx, VMX_PROC_CTLS2_RDTSCP));
        int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        AssertRCReturn(rc, rc);
        return IEMExecVmxVmexitInstr(pVCpu, pVmxTransient->uExitReason, pVmxTransient->cbInstr);
    }
    return hmR0VmxExitRdtscp(pVCpu, pVmxTransient);
}


/**
 * Nested-guest VM-exit handler for WBINVD (VMX_EXIT_WBINVD). Conditional VM-exit.
 */
HMVMX_EXIT_NSRC_DECL hmR0VmxExitWbinvdNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    if (CPUMIsGuestVmxProcCtls2Set(pVCpu, &pVCpu->cpum.GstCtx, VMX_PROC_CTLS2_WBINVD_EXIT))
    {
        int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        AssertRCReturn(rc, rc);
        return IEMExecVmxVmexitInstr(pVCpu, pVmxTransient->uExitReason, pVmxTransient->cbInstr);
    }
    return hmR0VmxExitWbinvd(pVCpu, pVmxTransient);
}


/**
 * Nested-guest VM-exit handler for INVPCID (VMX_EXIT_INVPCID). Conditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitInvpcidNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    if (CPUMIsGuestVmxProcCtlsSet(pVCpu, &pVCpu->cpum.GstCtx, VMX_PROC_CTLS_INVLPG_EXIT))
    {
        Assert(CPUMIsGuestVmxProcCtls2Set(pVCpu, &pVCpu->cpum.GstCtx, VMX_PROC_CTLS2_INVPCID));
        int rc  = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        rc     |= hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
        rc     |= hmR0VmxReadExitInstrInfoVmcs(pVmxTransient);
        AssertRCReturn(rc, rc);

        VMXVEXITINFO ExitInfo;
        RT_ZERO(ExitInfo);
        ExitInfo.uReason   = pVmxTransient->uExitReason;
        ExitInfo.cbInstr   = pVmxTransient->cbInstr;
        ExitInfo.u64Qual   = pVmxTransient->uExitQual;
        ExitInfo.InstrInfo = pVmxTransient->ExitInstrInfo;
        return IEMExecVmxVmexitInstrWithInfo(pVCpu, &ExitInfo);
    }
    return hmR0VmxExitInvpcid(pVCpu, pVmxTransient);
}


/**
 * Nested-guest VM-exit handler for invalid-guest state
 * (VMX_EXIT_ERR_INVALID_GUEST_STATE). Error VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitErrInvalidGuestStateNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

    /*
     * Currently this should never happen because we fully emulate VMLAUNCH/VMRESUME in IEM.
     * So if it does happen, it indicates a bug possibly in the hardware-assisted VMX code.
     * Handle it like it's in an invalid guest state of the outer guest.
     *
     * When the fast path is implemented, this should be changed to cause the corresponding
     * nested-guest VM-exit.
     */
    return hmR0VmxExitErrInvalidGuestState(pVCpu, pVmxTransient);
}


/**
 * Nested-guest VM-exit handler for instructions that cause VM-exits uncondtionally
 * and only provide the instruction length.
 *
 * Unconditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitInstrNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

#ifdef VBOX_STRICT
    PCCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
    switch (pVmxTransient->uExitReason)
    {
        case VMX_EXIT_ENCLS:
            Assert(CPUMIsGuestVmxProcCtls2Set(pVCpu, pCtx, VMX_PROC_CTLS2_ENCLS_EXIT));
            break;

        case VMX_EXIT_VMFUNC:
            Assert(CPUMIsGuestVmxProcCtls2Set(pVCpu, pCtx, VMX_PROC_CTLS2_VMFUNC));
            break;
    }
#endif

    int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);
    return IEMExecVmxVmexitInstr(pVCpu, pVmxTransient->uExitReason, pVmxTransient->cbInstr);
}


/**
 * Nested-guest VM-exit handler for instructions that provide instruction length as
 * well as more information.
 *
 * Unconditional VM-exit.
 */
HMVMX_EXIT_DECL hmR0VmxExitInstrWithInfoNested(PVMCPU pVCpu, PVMXTRANSIENT pVmxTransient)
{
    HMVMX_VALIDATE_NESTED_EXIT_HANDLER_PARAMS(pVCpu, pVmxTransient);

#ifdef VBOX_STRICT
    PCCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
    switch (pVmxTransient->uExitReason)
    {
        case VMX_EXIT_GDTR_IDTR_ACCESS:
        case VMX_EXIT_LDTR_TR_ACCESS:
            Assert(CPUMIsGuestVmxProcCtls2Set(pVCpu, pCtx, VMX_PROC_CTLS2_DESC_TABLE_EXIT));
            break;

        case VMX_EXIT_RDRAND:
            Assert(CPUMIsGuestVmxProcCtls2Set(pVCpu, pCtx, VMX_PROC_CTLS2_RDRAND_EXIT));
            break;

        case VMX_EXIT_RDSEED:
            Assert(CPUMIsGuestVmxProcCtls2Set(pVCpu, pCtx, VMX_PROC_CTLS2_RDSEED_EXIT));
            break;

        case VMX_EXIT_XSAVES:
        case VMX_EXIT_XRSTORS:
            /** @todo NSTVMX: Verify XSS-bitmap. */
            Assert(CPUMIsGuestVmxProcCtls2Set(pVCpu, pCtx, VMX_PROC_CTLS2_XSAVES_XRSTORS));
            break;

        case VMX_EXIT_UMWAIT:
        case VMX_EXIT_TPAUSE:
            Assert(CPUMIsGuestVmxProcCtlsSet(pVCpu, pCtx, VMX_PROC_CTLS_RDTSC_EXIT));
            Assert(CPUMIsGuestVmxProcCtls2Set(pVCpu, pCtx, VMX_PROC_CTLS2_USER_WAIT_PAUSE));
            break;
    }
#endif

    int rc  = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc     |= hmR0VmxReadExitQualVmcs(pVCpu, pVmxTransient);
    rc     |= hmR0VmxReadExitInstrInfoVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);

    VMXVEXITINFO ExitInfo;
    RT_ZERO(ExitInfo);
    ExitInfo.uReason   = pVmxTransient->uExitReason;
    ExitInfo.cbInstr   = pVmxTransient->cbInstr;
    ExitInfo.u64Qual   = pVmxTransient->uExitQual;
    ExitInfo.InstrInfo = pVmxTransient->ExitInstrInfo;
    return IEMExecVmxVmexitInstrWithInfo(pVCpu, &ExitInfo);
}

/** @} */

#endif /* VBOX_WITH_NESTED_HWVIRT_VMX */


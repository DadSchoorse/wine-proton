/*
 * i386 signal handling routines
 * 
 * Copyright 1999 Alexandre Julliard
 */

#ifdef __i386__

#include "config.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#ifdef HAVE_SYS_PARAM_H
# include <sys/param.h>
#endif
#ifdef HAVE_SYSCALL_H
# include <syscall.h>
#else
# ifdef HAVE_SYS_SYSCALL_H
#  include <sys/syscall.h>
# endif
#endif

#ifdef HAVE_SYS_VM86_H
# include <sys/vm86.h>
#endif

#include "winnt.h"
#include "selectors.h"

/***********************************************************************
 * signal context platform-specific definitions
 */

#ifdef linux
typedef struct
{
    unsigned short sc_gs, __gsh;
    unsigned short sc_fs, __fsh;
    unsigned short sc_es, __esh;
    unsigned short sc_ds, __dsh;
    unsigned long sc_edi;
    unsigned long sc_esi;
    unsigned long sc_ebp;
    unsigned long sc_esp;
    unsigned long sc_ebx;
    unsigned long sc_edx;
    unsigned long sc_ecx;
    unsigned long sc_eax;
    unsigned long sc_trapno;
    unsigned long sc_err;
    unsigned long sc_eip;
    unsigned short sc_cs, __csh;
    unsigned long sc_eflags;
    unsigned long esp_at_signal;
    unsigned short sc_ss, __ssh;
    unsigned long i387;
    unsigned long oldmask;
    unsigned long cr2;
} SIGCONTEXT;

#define HANDLER_DEF(name) void name( int __signal, SIGCONTEXT __context )
#define HANDLER_CONTEXT (&__context)

/* this is the sigaction structure from the Linux 2.1.20 kernel.  */
struct kernel_sigaction
{
    void (*ksa_handler)();
    unsigned long ksa_mask;
    unsigned long ksa_flags;
    void *ksa_restorer;
};

/* Similar to the sigaction function in libc, except it leaves alone the
   restorer field, which is used to specify the signal stack address */
static inline int wine_sigaction( int sig, struct kernel_sigaction *new,
                                  struct kernel_sigaction *old )
{
    __asm__ __volatile__( "pushl %%ebx\n\t"
                          "movl %2,%%ebx\n\t"
                          "int $0x80\n\t"
                          "popl %%ebx"
                          : "=a" (sig)
                          : "0" (SYS_sigaction), "r" (sig), "c" (new), "d" (old) );
    if (sig>=0) return 0;
    errno = -sig;
    return -1;
}

#ifdef HAVE_SIGALTSTACK
/* direct syscall for sigaltstack to work around glibc 2.0 brain-damage */
static inline int wine_sigaltstack( const struct sigaltstack *new,
                                    struct sigaltstack *old )
{
    int ret;
    __asm__ __volatile__( "pushl %%ebx\n\t"
                          "movl %2,%%ebx\n\t"
                          "int $0x80\n\t"
                          "popl %%ebx"
                          : "=a" (ret)
                          : "0" (SYS_sigaltstack), "r" (new), "c" (old) );
    if (ret >= 0) return 0;
    errno = -ret;
    return -1;
}
#endif

int vm86_enter( struct vm86plus_struct *ptr );
void vm86_return();
__ASM_GLOBAL_FUNC(vm86_enter,
                  "pushl %ebp\n\t"
                  "movl %esp, %ebp\n\t"
                  "movl $166,%eax\n\t"  /*SYS_vm86*/
                  "pushl %fs\n\t"
                  "movl 8(%ebp),%ecx\n\t"
                  "pushl %ebx\n\t"
                  "movl $1,%ebx\n\t"    /*VM86_ENTER*/
                  "pushl %ecx\n\t"      /* put vm86plus_struct ptr somewhere we can find it */
                  "int $0x80\n"
                  ".globl " __ASM_NAME("vm86_return") "\n\t"
                  ".type " __ASM_NAME("vm86_return") ",@function\n"
                  __ASM_NAME("vm86_return") ":\n\t"
                  "popl %ecx\n\t"
                  "popl %ebx\n\t"
                  "popl %fs\n\t"
                  "popl %ebp\n\t"
                  "ret" );

#endif  /* linux */

#ifdef BSDI

#define EAX_sig(context)     ((context)->tf_eax)
#define EBX_sig(context)     ((context)->tf_ebx)
#define ECX_sig(context)     ((context)->tf_ecx)
#define EDX_sig(context)     ((context)->tf_edx)
#define ESI_sig(context)     ((context)->tf_esi)
#define EDI_sig(context)     ((context)->tf_edi)
#define EBP_sig(context)     ((context)->tf_ebp)
                            
#define CS_sig(context)      ((context)->tf_cs)
#define DS_sig(context)      ((context)->tf_ds)
#define ES_sig(context)      ((context)->tf_es)
#define SS_sig(context)      ((context)->tf_ss)

#include <machine/frame.h>
typedef struct trapframe SIGCONTEXT;

#define HANDLER_DEF(name) void name( int __signal, int code, SIGCONTEXT *__context )
#define HANDLER_CONTEXT __context

#define EFL_sig(context)     ((context)->tf_eflags)

#define EIP_sig(context)     (*((unsigned long*)&(context)->tf_eip))
#define ESP_sig(context)     (*((unsigned long*)&(context)->tf_esp))

#endif /* bsdi */

#if defined(__NetBSD__) || defined(__FreeBSD__) || defined(__OpenBSD__)

typedef struct sigcontext SIGCONTEXT;

#define HANDLER_DEF(name) void name( int __signal, int code, SIGCONTEXT *__context )
#define HANDLER_CONTEXT __context

#endif  /* FreeBSD */

#if defined(__svr4__) || defined(_SCO_DS) || defined(__sun)

#ifdef _SCO_DS
#include <sys/regset.h>
#endif
/* Solaris kludge */
#undef ERR
#include <sys/ucontext.h>
#undef ERR
typedef struct ucontext SIGCONTEXT;

#define HANDLER_DEF(name) void name( int __signal, void *__siginfo, SIGCONTEXT *__context )
#define HANDLER_CONTEXT __context

#endif  /* svr4 || SCO_DS */

#ifdef __EMX__

typedef struct
{
    unsigned long ContextFlags;
    FLOATING_SAVE_AREA sc_float;
    unsigned long sc_gs;
    unsigned long sc_fs;
    unsigned long sc_es;
    unsigned long sc_ds;
    unsigned long sc_edi;
    unsigned long sc_esi;
    unsigned long sc_eax;
    unsigned long sc_ebx;
    unsigned long sc_ecx;
    unsigned long sc_edx;
    unsigned long sc_ebp;
    unsigned long sc_eip;
    unsigned long sc_cs;
    unsigned long sc_eflags;
    unsigned long sc_esp;
    unsigned long sc_ss;
} SIGCONTEXT;

#endif  /* __EMX__ */


#if defined(linux) || defined(__NetBSD__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__EMX__)

#define EAX_sig(context)     ((context)->sc_eax)
#define EBX_sig(context)     ((context)->sc_ebx)
#define ECX_sig(context)     ((context)->sc_ecx)
#define EDX_sig(context)     ((context)->sc_edx)
#define ESI_sig(context)     ((context)->sc_esi)
#define EDI_sig(context)     ((context)->sc_edi)
#define EBP_sig(context)     ((context)->sc_ebp)
                            
#define CS_sig(context)      ((context)->sc_cs)
#define DS_sig(context)      ((context)->sc_ds)
#define ES_sig(context)      ((context)->sc_es)
#define SS_sig(context)      ((context)->sc_ss)
                            
/* FS and GS are now in the sigcontext struct of FreeBSD, but not 
 * saved by the exception handling. duh.
 * Actually they are in -current (have been for a while), and that
 * patch now finally has been MFC'd to -stable too (Nov 15 1999).
 * If you're running a system from the -stable branch older than that,
 * like a 3.3-RELEASE, grab the patch from the ports tree:
 * ftp://ftp.freebsd.org/pub/FreeBSD/FreeBSD-current/ports/emulators/wine/files/patch-3.3-sys-fsgs
 * (If its not yet there when you look, go here:
 * http://www.jelal.kn-bremen.de/freebsd/ports/emulators/wine/files/ )
 */
#ifdef __FreeBSD__
#define FS_sig(context)      ((context)->sc_fs)
#define GS_sig(context)      ((context)->sc_gs)
#endif

#ifdef linux
#define FS_sig(context)      ((context)->sc_fs)
#define GS_sig(context)      ((context)->sc_gs)
#define CR2_sig(context)     ((context)->cr2)
#define TRAP_sig(context)    ((context)->sc_trapno)
#define ERROR_sig(context)   ((context)->sc_err)
#define FPU_sig(context)     ((FLOATING_SAVE_AREA*)((context)->i387))
#endif

#ifndef __FreeBSD__
#define EFL_sig(context)     ((context)->sc_eflags)
#else                       
#define EFL_sig(context)     ((context)->sc_efl)
/* FreeBSD, see i386/i386/traps.c::trap_pfault va->err kludge  */
#define CR2_sig(context)     ((context)->sc_err)
#define TRAP_sig(context)    ((context)->sc_trapno)
#endif                      

#define EIP_sig(context)     (*((unsigned long*)&(context)->sc_eip))
#define ESP_sig(context)     (*((unsigned long*)&(context)->sc_esp))

#endif  /* linux || __NetBSD__ || __FreeBSD__ || __OpenBSD__ */

#if defined(__svr4__) || defined(_SCO_DS) || defined(__sun)

#ifdef _SCO_DS
#define gregs regs
#endif

#define EAX_sig(context)     ((context)->uc_mcontext.gregs[EAX])
#define EBX_sig(context)     ((context)->uc_mcontext.gregs[EBX])
#define ECX_sig(context)     ((context)->uc_mcontext.gregs[ECX])
#define EDX_sig(context)     ((context)->uc_mcontext.gregs[EDX])
#define ESI_sig(context)     ((context)->uc_mcontext.gregs[ESI])
#define EDI_sig(context)     ((context)->uc_mcontext.gregs[EDI])
#define EBP_sig(context)     ((context)->uc_mcontext.gregs[EBP])
                            
#define CS_sig(context)      ((context)->uc_mcontext.gregs[CS])
#define DS_sig(context)      ((context)->uc_mcontext.gregs[DS])
#define ES_sig(context)      ((context)->uc_mcontext.gregs[ES])
#define SS_sig(context)      ((context)->uc_mcontext.gregs[SS])
                            
#define FS_sig(context)      ((context)->uc_mcontext.gregs[FS])
#define GS_sig(context)      ((context)->uc_mcontext.gregs[GS])

#define EFL_sig(context)     ((context)->uc_mcontext.gregs[EFL])
                            
#define EIP_sig(context)     ((context)->uc_mcontext.gregs[EIP])
#ifdef R_ESP
#define ESP_sig(context)     ((context)->uc_mcontext.gregs[R_ESP])
#else
#define ESP_sig(context)     ((context)->uc_mcontext.gregs[ESP])
#endif
#ifdef TRAPNO
#define TRAP_sig(context)     ((context)->uc_mcontext.gregs[TRAPNO])
#endif

#endif  /* svr4 || SCO_DS */


/* exception code definitions (already defined by FreeBSD) */
#ifndef __FreeBSD__  /* FIXME: other BSDs? */
#define T_DIVIDE        0   /* Division by zero exception */
#define T_TRCTRAP       1   /* Single-step exception */
#define T_NMI           2   /* NMI interrupt */
#define T_BPTFLT        3   /* Breakpoint exception */
#define T_OFLOW         4   /* Overflow exception */
#define T_BOUND         5   /* Bound range exception */
#define T_PRIVINFLT     6   /* Invalid opcode exception */
#define T_DNA           7   /* Device not available exception */
#define T_DOUBLEFLT     8   /* Double fault exception */
#define T_FPOPFLT       9   /* Coprocessor segment overrun */
#define T_TSSFLT        10  /* Invalid TSS exception */
#define T_SEGNPFLT      11  /* Segment not present exception */
#define T_STKFLT        12  /* Stack fault */
#define T_PROTFLT       13  /* General protection fault */
#define T_PAGEFLT       14  /* Page fault */
#define T_RESERVED      15  /* Unknown exception */
#define T_ARITHTRAP     16  /* Floating point exception */
#define T_ALIGNFLT      17  /* Alignment check exception */
#define T_MCHK          18  /* Machine check exception */
#define T_CACHEFLT      19  /* Cache flush exception */
#endif

#define T_UNKNOWN     (-1)  /* Unknown fault (TRAP_sig not defined) */

#include "wine/exception.h"
#include "winnt.h"
#include "stackframe.h"
#include "global.h"
#include "miscemu.h"
#include "ntddk.h"
#include "syslevel.h"
#include "debugtools.h"

DEFAULT_DEBUG_CHANNEL(seh);


/***********************************************************************
 *           get_trap_code
 *
 * Get the trap code for a signal.
 */
static inline int get_trap_code( const SIGCONTEXT *sigcontext )
{
#ifdef TRAP_sig
    return TRAP_sig(sigcontext);
#else
    return T_UNKNOWN;  /* unknown trap code */
#endif
}

/***********************************************************************
 *           get_error_code
 *
 * Get the error code for a signal.
 */
static inline int get_error_code( const SIGCONTEXT *sigcontext )
{
#ifdef ERROR_sig
    return ERROR_sig(sigcontext);
#else
    return 0;
#endif
}

/***********************************************************************
 *           get_cr2_value
 *
 * Get the CR2 value for a signal.
 */
static inline void *get_cr2_value( const SIGCONTEXT *sigcontext )
{
#ifdef CR2_sig
    return (void *)CR2_sig(sigcontext);
#else
    return NULL;
#endif
}

/***********************************************************************
 *           save_context
 *
 * Set the register values from a sigcontext. 
 */
static void save_context( CONTEXT *context, const SIGCONTEXT *sigcontext )
{
    WORD fs;
    /* get %fs at time of the fault */
#ifdef FS_sig
    fs = FS_sig(sigcontext);
#else
    fs = __get_fs();
#endif
    context->SegFs = fs;

    /* now restore a proper %fs for the fault handler */
    if (!IS_SELECTOR_SYSTEM(CS_sig(sigcontext)))  /* 16-bit mode */
    {
        fs = SYSLEVEL_Win16CurrentTeb;
    }
#ifdef linux
    else if ((void *)EIP_sig(sigcontext) == vm86_return)  /* vm86 mode */
    {
        /* retrieve pointer to vm86plus struct that was stored in vm86_enter */
        struct vm86plus_struct *vm86 = *(struct vm86plus_struct **)ESP_sig(sigcontext);
        /* fetch the saved %fs on the stack */
        fs = *((unsigned int *)ESP_sig(sigcontext) + 2);
        __set_fs(fs);
        /* get context from vm86 struct */
        context->Eax    = vm86->regs.eax;
        context->Ebx    = vm86->regs.ebx;
        context->Ecx    = vm86->regs.ecx;
        context->Edx    = vm86->regs.edx;
        context->Esi    = vm86->regs.esi;
        context->Edi    = vm86->regs.edi;
        context->Esp    = vm86->regs.esp;
        context->Ebp    = vm86->regs.ebp;
        context->Eip    = vm86->regs.eip;
        context->SegCs  = vm86->regs.cs;
        context->SegDs  = vm86->regs.ds;
        context->SegEs  = vm86->regs.es;
        context->SegFs  = vm86->regs.fs;
        context->SegGs  = vm86->regs.gs;
        context->SegSs  = vm86->regs.ss;
        context->EFlags = vm86->regs.eflags;
        return;
    }
#endif  /* linux */

    if (!fs)
    {
        fs = SYSLEVEL_EmergencyTeb;
        __set_fs(fs);
        ERR("fallback to emergency TEB\n");
    }
    __set_fs(fs);

    context->Eax    = EAX_sig(sigcontext);
    context->Ebx    = EBX_sig(sigcontext);
    context->Ecx    = ECX_sig(sigcontext);
    context->Edx    = EDX_sig(sigcontext);
    context->Esi    = ESI_sig(sigcontext);
    context->Edi    = EDI_sig(sigcontext);
    context->Ebp    = EBP_sig(sigcontext);
    context->EFlags = EFL_sig(sigcontext);
    context->Eip    = EIP_sig(sigcontext);
    context->Esp    = ESP_sig(sigcontext);
    context->SegCs  = LOWORD(CS_sig(sigcontext));
    context->SegDs  = LOWORD(DS_sig(sigcontext));
    context->SegEs  = LOWORD(ES_sig(sigcontext));
    context->SegSs  = LOWORD(SS_sig(sigcontext));
#ifdef GS_sig
    context->SegGs  = LOWORD(GS_sig(sigcontext));
#else
    context->SegGs  = __get_gs();
#endif
}


/***********************************************************************
 *           restore_context
 *
 * Build a sigcontext from the register values.
 */
static void restore_context( const CONTEXT *context, SIGCONTEXT *sigcontext )
{
#ifdef linux
    /* check if exception occurred in vm86 mode */
    if ((void *)EIP_sig(sigcontext) == vm86_return &&
        IS_SELECTOR_SYSTEM(CS_sig(sigcontext)))
    {
        /* retrieve pointer to vm86plus struct that was stored in vm86_enter */
        struct vm86plus_struct *vm86 = *(struct vm86plus_struct **)ESP_sig(sigcontext);
        vm86->regs.eax    = context->Eax;
        vm86->regs.ebx    = context->Ebx;
        vm86->regs.ecx    = context->Ecx;
        vm86->regs.edx    = context->Edx;
        vm86->regs.esi    = context->Esi;
        vm86->regs.edi    = context->Edi;
        vm86->regs.esp    = context->Esp;
        vm86->regs.ebp    = context->Ebp;
        vm86->regs.eip    = context->Eip;
        vm86->regs.cs     = context->SegCs;
        vm86->regs.ds     = context->SegDs;
        vm86->regs.es     = context->SegEs;
        vm86->regs.fs     = context->SegFs;
        vm86->regs.gs     = context->SegGs;
        vm86->regs.ss     = context->SegSs;
        vm86->regs.eflags = context->EFlags;
        return;
    }
#endif /* linux */

    EAX_sig(sigcontext) = context->Eax;
    EBX_sig(sigcontext) = context->Ebx;
    ECX_sig(sigcontext) = context->Ecx;
    EDX_sig(sigcontext) = context->Edx;
    ESI_sig(sigcontext) = context->Esi;
    EDI_sig(sigcontext) = context->Edi;
    EBP_sig(sigcontext) = context->Ebp;
    EFL_sig(sigcontext) = context->EFlags;
    EIP_sig(sigcontext) = context->Eip;
    ESP_sig(sigcontext) = context->Esp;
    CS_sig(sigcontext)  = context->SegCs;
    DS_sig(sigcontext)  = context->SegDs;
    ES_sig(sigcontext)  = context->SegEs;
    SS_sig(sigcontext)  = context->SegSs;
#ifdef FS_sig
    FS_sig(sigcontext)  = context->SegFs;
#else
    __set_fs( context->SegFs );
#endif
#ifdef GS_sig
    GS_sig(sigcontext)  = context->SegGs;
#else
    __set_gs( context->SegGs );
#endif
}


/***********************************************************************
 *           save_fpu
 *
 * Set the FPU context from a sigcontext. 
 */
static void inline save_fpu( CONTEXT *context, const SIGCONTEXT *sigcontext )
{
#ifdef FPU_sig
    if (FPU_sig(sigcontext))
    {
        context->FloatSave = *FPU_sig(sigcontext);
        return;
    }
#endif  /* FPU_sig */
#ifdef __GNUC__
    __asm__ __volatile__( "fnsave %0; fwait" : "=m" (context->FloatSave) );
#endif  /* __GNUC__ */
}


/***********************************************************************
 *           restore_fpu
 *
 * Restore the FPU context to a sigcontext. 
 */
static void inline restore_fpu( CONTEXT *context, const SIGCONTEXT *sigcontext )
{
    /* reset the current interrupt status */
    context->FloatSave.StatusWord &= context->FloatSave.ControlWord | 0xffffff80;
#ifdef FPU_sig
    if (FPU_sig(sigcontext))
    {
        *FPU_sig(sigcontext) = context->FloatSave;
        return;
    }
#endif  /* FPU_sig */
#ifdef __GNUC__
    /* avoid nested exceptions */
    __asm__ __volatile__( "frstor %0; fwait" : : "m" (context->FloatSave) );
#endif  /* __GNUC__ */
}


/**********************************************************************
 *		get_fpu_code
 *
 * Get the FPU exception code from the FPU status.
 */
static inline DWORD get_fpu_code( const CONTEXT *context )
{
    DWORD status = context->FloatSave.StatusWord;

    if (status & 0x01)  /* IE */
    {
        if (status & 0x40)  /* SF */
            return EXCEPTION_FLT_STACK_CHECK;
        else
            return EXCEPTION_FLT_INVALID_OPERATION;
    }
    if (status & 0x02) return EXCEPTION_FLT_DENORMAL_OPERAND;  /* DE flag */
    if (status & 0x04) return EXCEPTION_FLT_DIVIDE_BY_ZERO;    /* ZE flag */
    if (status & 0x08) return EXCEPTION_FLT_OVERFLOW;          /* OE flag */
    if (status & 0x10) return EXCEPTION_FLT_UNDERFLOW;         /* UE flag */
    if (status & 0x20) return EXCEPTION_FLT_INEXACT_RESULT;    /* PE flag */
    return EXCEPTION_FLT_INVALID_OPERATION;  /* generic error */
}


/**********************************************************************
 *		do_segv
 *
 * Implementation of SIGSEGV handler.
 */
static void do_segv( CONTEXT *context, int trap_code, void *cr2, int err_code )
{
    EXCEPTION_RECORD rec;
    DWORD page_fault_code = EXCEPTION_ACCESS_VIOLATION;

#ifdef CR2_sig
    /* we want the page-fault case to be fast */
    if (trap_code == T_PAGEFLT)
        if (!(page_fault_code = VIRTUAL_HandleFault( cr2 ))) return;
#endif

    rec.ExceptionRecord  = NULL;
    rec.ExceptionFlags   = EXCEPTION_CONTINUABLE;
    rec.ExceptionAddress = (LPVOID)context->Eip;
    rec.NumberParameters = 0;

    switch(trap_code)
    {
    case T_OFLOW:   /* Overflow exception */
        rec.ExceptionCode = EXCEPTION_INT_OVERFLOW;
        break;
    case T_BOUND:   /* Bound range exception */
        rec.ExceptionCode = EXCEPTION_ARRAY_BOUNDS_EXCEEDED;
        break;
    case T_PRIVINFLT:   /* Invalid opcode exception */
        rec.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
        break;
    case T_STKFLT:  /* Stack fault */
        rec.ExceptionCode = EXCEPTION_STACK_OVERFLOW;
        break;
    case T_SEGNPFLT:  /* Segment not present exception */
    case T_PROTFLT:   /* General protection fault */
    case T_UNKNOWN:   /* Unknown fault code */
        if (INSTR_EmulateInstruction( context )) return;
        rec.ExceptionCode = EXCEPTION_PRIV_INSTRUCTION;
        break;
    case T_PAGEFLT:  /* Page fault */
#ifdef CR2_sig
        rec.NumberParameters = 2;
        rec.ExceptionInformation[0] = (err_code & 2) != 0;
        rec.ExceptionInformation[1] = (DWORD)cr2;
#endif /* CR2_sig */
        rec.ExceptionCode = page_fault_code;
        break;
    case T_ALIGNFLT:  /* Alignment check exception */
        /* FIXME: pass through exception handler first? */
        if (context->EFlags & 0x00040000)
        {
            /* Disable AC flag, return */
            context->EFlags &= ~0x00040000;
            return;
        }
        rec.ExceptionCode = EXCEPTION_DATATYPE_MISALIGNMENT;
        break;
    default:
        ERR( "Got unexpected trap %d\n", trap_code );
        /* fall through */
    case T_NMI:       /* NMI interrupt */
    case T_DNA:       /* Device not available exception */
    case T_DOUBLEFLT: /* Double fault exception */
    case T_TSSFLT:    /* Invalid TSS exception */
    case T_RESERVED:  /* Unknown exception */
    case T_MCHK:      /* Machine check exception */
#ifdef T_CACHEFLT
    case T_CACHEFLT:  /* Cache flush exception */
#endif
        rec.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
        break;
    }
    EXC_RtlRaiseException( &rec, context );
}


/**********************************************************************
 *		do_trap
 *
 * Implementation of SIGTRAP handler.
 */
static void do_trap( CONTEXT *context, int trap_code )
{
    EXCEPTION_RECORD rec;

    rec.ExceptionFlags   = EXCEPTION_CONTINUABLE;
    rec.ExceptionRecord  = NULL;
    rec.ExceptionAddress = (LPVOID)context->Eip;
    rec.NumberParameters = 0;

    switch(trap_code)
    {
    case T_TRCTRAP:  /* Single-step exception */
        rec.ExceptionCode = EXCEPTION_SINGLE_STEP;
        context->EFlags &= ~0x100;  /* clear single-step flag */
        break;
    case T_BPTFLT:   /* Breakpoint exception */
        rec.ExceptionAddress = (char *)rec.ExceptionAddress - 1;  /* back up over the int3 instruction */
        /* fall through */
    default:
        rec.ExceptionCode = EXCEPTION_BREAKPOINT;
        break;
    }
    EXC_RtlRaiseException( &rec, context );
}


/**********************************************************************
 *		do_fpe
 *
 * Implementation of SIGFPE handler
 */
static void do_fpe( CONTEXT *context, int trap_code )
{
    EXCEPTION_RECORD rec;

    switch(trap_code)
    {
    case T_DIVIDE:   /* Division by zero exception */
        rec.ExceptionCode = EXCEPTION_INT_DIVIDE_BY_ZERO;
        break;
    case T_FPOPFLT:   /* Coprocessor segment overrun */
        rec.ExceptionCode = EXCEPTION_FLT_INVALID_OPERATION;
        break;
    case T_ARITHTRAP:  /* Floating point exception */
    case T_UNKNOWN:    /* Unknown fault code */
        rec.ExceptionCode = get_fpu_code( context );
        break;
    default:
        ERR( "Got unexpected trap %d\n", trap_code );
        rec.ExceptionCode = EXCEPTION_FLT_INVALID_OPERATION;
        break;
    }
    rec.ExceptionFlags   = EXCEPTION_CONTINUABLE;
    rec.ExceptionRecord  = NULL;
    rec.ExceptionAddress = (LPVOID)context->Eip;
    rec.NumberParameters = 0;
    EXC_RtlRaiseException( &rec, context );
}


/**********************************************************************
 *		segv_handler
 *
 * Handler for SIGSEGV and related errors.
 */
static HANDLER_DEF(segv_handler)
{
    CONTEXT context;
    save_context( &context, HANDLER_CONTEXT );
    do_segv( &context, get_trap_code(HANDLER_CONTEXT),
             get_cr2_value(HANDLER_CONTEXT), get_error_code(HANDLER_CONTEXT) );
    restore_context( &context, HANDLER_CONTEXT );
}


/**********************************************************************
 *		trap_handler
 *
 * Handler for SIGTRAP.
 */
static HANDLER_DEF(trap_handler)
{
    CONTEXT context;
    save_context( &context, HANDLER_CONTEXT );
    do_trap( &context, get_trap_code(HANDLER_CONTEXT) );
    restore_context( &context, HANDLER_CONTEXT );
}


/**********************************************************************
 *		fpe_handler
 *
 * Handler for SIGFPE.
 */
static HANDLER_DEF(fpe_handler)
{
    CONTEXT context;
    save_fpu( &context, HANDLER_CONTEXT );
    save_context( &context, HANDLER_CONTEXT );
    do_fpe( &context, get_trap_code(HANDLER_CONTEXT) );
    restore_context( &context, HANDLER_CONTEXT );
    restore_fpu( &context, HANDLER_CONTEXT );
}


/**********************************************************************
 *		int_handler
 *
 * Handler for SIGINT.
 */
static HANDLER_DEF(int_handler)
{
    EXCEPTION_RECORD rec;
    CONTEXT context;

    save_context( &context, HANDLER_CONTEXT );
    rec.ExceptionCode    = CONTROL_C_EXIT;
    rec.ExceptionFlags   = EXCEPTION_CONTINUABLE;
    rec.ExceptionRecord  = NULL;
    rec.ExceptionAddress = (LPVOID)context.Eip;
    rec.NumberParameters = 0;
    EXC_RtlRaiseException( &rec, &context );
    restore_context( &context, HANDLER_CONTEXT );
}


/***********************************************************************
 *           set_handler
 *
 * Set a signal handler
 */
static int set_handler( int sig, int have_sigaltstack, void (*func)() )
{
    struct sigaction sig_act;

#ifdef linux
    if (!have_sigaltstack && NtCurrentTeb()->signal_stack)
    {
        struct kernel_sigaction sig_act;
        sig_act.ksa_handler = func;
        sig_act.ksa_flags   = SA_RESTART | SA_NOMASK;
        sig_act.ksa_mask    = 0;
        /* point to the top of the stack */
        sig_act.ksa_restorer = (char *)NtCurrentTeb()->signal_stack + SIGNAL_STACK_SIZE;
        return wine_sigaction( sig, &sig_act, NULL );
    }
#endif  /* linux */
    sig_act.sa_handler = func;
    sigemptyset( &sig_act.sa_mask );

#ifdef linux
    sig_act.sa_flags = SA_RESTART | SA_NOMASK;
#elif defined (__svr4__) || defined(_SCO_DS)
    sig_act.sa_flags = SA_SIGINFO | SA_RESTART;
#else
    sig_act.sa_flags = 0;
#endif

#ifdef SA_ONSTACK
    if (have_sigaltstack) sig_act.sa_flags |= SA_ONSTACK;
#endif
    return sigaction( sig, &sig_act, NULL );
}


/**********************************************************************
 *		SIGNAL_Init
 */
BOOL SIGNAL_Init(void)
{
    int have_sigaltstack = 0;

#ifdef HAVE_SIGALTSTACK
    struct sigaltstack ss;
    if ((ss.ss_sp = NtCurrentTeb()->signal_stack))
    {
        ss.ss_size  = SIGNAL_STACK_SIZE;
        ss.ss_flags = 0;
        if (!sigaltstack(&ss, NULL)) have_sigaltstack = 1;
#ifdef linux
        /* sigaltstack may fail because the kernel is too old, or
           because glibc is brain-dead. In the latter case a
           direct system call should succeed. */
        else if (!wine_sigaltstack(&ss, NULL)) have_sigaltstack = 1;
#endif  /* linux */
    }
#endif  /* HAVE_SIGALTSTACK */
    
    /* automatic child reaping to avoid zombies */
    signal( SIGCHLD, SIG_IGN );

    if (set_handler( SIGINT,  have_sigaltstack, (void (*)())int_handler ) == -1) goto error;
    if (set_handler( SIGFPE,  have_sigaltstack, (void (*)())fpe_handler ) == -1) goto error;
    if (set_handler( SIGSEGV, have_sigaltstack, (void (*)())segv_handler ) == -1) goto error;
    if (set_handler( SIGILL,  have_sigaltstack, (void (*)())segv_handler ) == -1) goto error;
#ifdef SIGBUS
    if (set_handler( SIGBUS,  have_sigaltstack, (void (*)())segv_handler ) == -1) goto error;
#endif
#ifdef SIGTRAP
    if (set_handler( SIGTRAP, have_sigaltstack, (void (*)())trap_handler ) == -1) goto error;
#endif
    return TRUE;

 error:
    perror("sigaction");
    return FALSE;
}


#ifdef linux
/**********************************************************************
 *		__wine_enter_vm86
 *
 * Enter vm86 mode with the specified register context.
 */
void __wine_enter_vm86( CONTEXT *context )
{
    EXCEPTION_RECORD rec;
    int res;
    struct vm86plus_struct vm86;

    memset( &vm86, 0, sizeof(vm86) );
    for (;;)
    {
        vm86.regs.eax    = context->Eax;
        vm86.regs.ebx    = context->Ebx;
        vm86.regs.ecx    = context->Ecx;
        vm86.regs.edx    = context->Edx;
        vm86.regs.esi    = context->Esi;
        vm86.regs.edi    = context->Edi;
        vm86.regs.esp    = context->Esp;
        vm86.regs.ebp    = context->Ebp;
        vm86.regs.eip    = context->Eip;
        vm86.regs.cs     = context->SegCs;
        vm86.regs.ds     = context->SegDs;
        vm86.regs.es     = context->SegEs;
        vm86.regs.fs     = context->SegFs;
        vm86.regs.gs     = context->SegGs;
        vm86.regs.ss     = context->SegSs;
        vm86.regs.eflags = context->EFlags;

        do
        {
            res = vm86_enter( &vm86 );
            if (res < 0)
            {
                errno = -res;
                return;
            }
        } while (VM86_TYPE(res) == VM86_SIGNAL);

        context->Eax    = vm86.regs.eax;
        context->Ebx    = vm86.regs.ebx;
        context->Ecx    = vm86.regs.ecx;
        context->Edx    = vm86.regs.edx;
        context->Esi    = vm86.regs.esi;
        context->Edi    = vm86.regs.edi;
        context->Esp    = vm86.regs.esp;
        context->Ebp    = vm86.regs.ebp;
        context->Eip    = vm86.regs.eip;
        context->SegCs  = vm86.regs.cs;
        context->SegDs  = vm86.regs.ds;
        context->SegEs  = vm86.regs.es;
        context->SegFs  = vm86.regs.fs;
        context->SegGs  = vm86.regs.gs;
        context->SegSs  = vm86.regs.ss;
        context->EFlags = vm86.regs.eflags;

        switch(VM86_TYPE(res))
        {
        case VM86_UNKNOWN: /* unhandled GP fault - IO-instruction or similar */
            do_segv( context, T_PROTFLT, 0, 0 );
            continue;
        case VM86_TRAP: /* return due to DOS-debugger request */
            do_trap( context, VM86_ARG(res)  );
            continue;
        case VM86_INTx: /* int3/int x instruction (ARG = x) */
            rec.ExceptionCode = EXCEPTION_VM86_INTx;
            break;
        case VM86_STI: /* sti/popf/iret instruction enabled virtual interrupts */
            rec.ExceptionCode = EXCEPTION_VM86_STI;
            break;
        case VM86_PICRETURN: /* return due to pending PIC request */
            rec.ExceptionCode = EXCEPTION_VM86_PICRETURN;
            break;
        default:
            ERR( "unhandled result from vm86 mode %x\n", res );
            continue;
        }
        rec.ExceptionFlags          = EXCEPTION_CONTINUABLE;
        rec.ExceptionRecord         = NULL;
        rec.ExceptionAddress        = (LPVOID)context->Eip;
        rec.NumberParameters        = 1;
        rec.ExceptionInformation[0] = VM86_ARG(res);
        EXC_RtlRaiseException( &rec, context );
    }
}

#else /* linux */
void __wine_enter_vm86( CONTEXT *context )
{
    MESSAGE("vm86 mode not supported on this platform\n");
}
#endif /* linux */

/**********************************************************************
 *		DbgBreakPoint   (NTDLL)
 */
void WINAPI DbgBreakPoint(void);
__ASM_GLOBAL_FUNC( DbgBreakPoint, "int $3; ret");

/**********************************************************************
 *		DbgUserBreakPoint   (NTDLL)
 */
void WINAPI DbgUserBreakPoint(void);
__ASM_GLOBAL_FUNC( DbgUserBreakPoint, "int $3; ret");

#endif  /* __i386__ */

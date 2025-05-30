/**********************************************************************

  vm_dump.c -

  $Author$

  Copyright (C) 2004-2007 Koichi Sasada

**********************************************************************/

#include "ruby/internal/config.h"
#include "ruby/fiber/scheduler.h"

#ifdef HAVE_UCONTEXT_H
# include <ucontext.h>
#endif

#ifdef __APPLE__
# ifdef HAVE_LIBPROC_H
#  include <libproc.h>
# endif
# include <mach/vm_map.h>
# include <mach/mach_init.h>
# ifdef __LP64__
#  define vm_region_recurse vm_region_recurse_64
# endif
/* that is defined in sys/queue.h, and conflicts with
 * ccan/list/list.h */
# undef LIST_HEAD
#endif

#include "addr2line.h"
#include "internal.h"
#include "internal/gc.h"
#include "internal/variable.h"
#include "internal/vm.h"
#include "iseq.h"
#include "vm_core.h"
#include "ractor_core.h"

#define MAX_POSBUF 128

#define VM_CFP_CNT(ec, cfp) \
  ((rb_control_frame_t *)((ec)->vm_stack + (ec)->vm_stack_size) - \
   (rb_control_frame_t *)(cfp))

const char *rb_method_type_name(rb_method_type_t type);
int ruby_on_ci;

#define kprintf(...) if (fprintf(errout, __VA_ARGS__) < 0) goto error
#define kputs(s) if (fputs(s, errout) < 0) goto error

static bool
control_frame_dump(const rb_execution_context_t *ec, const rb_control_frame_t *cfp, FILE *errout)
{
    ptrdiff_t pc = -1;
    ptrdiff_t ep = cfp->ep - ec->vm_stack;
    char ep_in_heap = ' ';
    char posbuf[MAX_POSBUF+1];
    int line = 0;
    const char *magic, *iseq_name = "-", *selfstr = "-", *biseq_name = "-";
    VALUE tmp;
    const rb_iseq_t *iseq = NULL;
    const rb_callable_method_entry_t *me = rb_vm_frame_method_entry(cfp);

    if (ep < 0 || (size_t)ep > ec->vm_stack_size) {
        ep = (ptrdiff_t)cfp->ep;
        ep_in_heap = 'p';
    }

    switch (VM_FRAME_TYPE(cfp)) {
      case VM_FRAME_MAGIC_TOP:
        magic = "TOP";
        break;
      case VM_FRAME_MAGIC_METHOD:
        magic = "METHOD";
        break;
      case VM_FRAME_MAGIC_CLASS:
        magic = "CLASS";
        break;
      case VM_FRAME_MAGIC_BLOCK:
        magic = "BLOCK";
        break;
      case VM_FRAME_MAGIC_CFUNC:
        magic = "CFUNC";
        break;
      case VM_FRAME_MAGIC_IFUNC:
        magic = "IFUNC";
        break;
      case VM_FRAME_MAGIC_EVAL:
        magic = "EVAL";
        break;
      case VM_FRAME_MAGIC_RESCUE:
        magic = "RESCUE";
        break;
      case VM_FRAME_MAGIC_DUMMY:
        magic = "DUMMY";
        break;
      case 0:
        magic = "------";
        break;
      default:
        magic = "(none)";
        break;
    }

    if (0) {
        tmp = rb_inspect(cfp->self);
        selfstr = StringValueCStr(tmp);
    }
    else {
        selfstr = "";
    }

    if (cfp->iseq != 0) {
#define RUBY_VM_IFUNC_P(ptr) IMEMO_TYPE_P(ptr, imemo_ifunc)
        if (RUBY_VM_IFUNC_P(cfp->iseq)) {
            iseq_name = "<ifunc>";
        }
        else if (SYMBOL_P((VALUE)cfp->iseq)) {
            tmp = rb_sym2str((VALUE)cfp->iseq);
            iseq_name = RSTRING_PTR(tmp);
            snprintf(posbuf, MAX_POSBUF, ":%s", iseq_name);
            line = -1;
        }
        else {
            if (cfp->pc) {
                iseq = cfp->iseq;
                pc = cfp->pc - ISEQ_BODY(iseq)->iseq_encoded;
                iseq_name = RSTRING_PTR(ISEQ_BODY(iseq)->location.label);
                line = rb_vm_get_sourceline(cfp);
                if (line) {
                    snprintf(posbuf, MAX_POSBUF, "%s:%d", RSTRING_PTR(rb_iseq_path(iseq)), line);
                }
            }
            else {
                iseq_name = "<dummy_frame>";
            }
        }
    }
    else if (me != NULL) {
        iseq_name = rb_id2name(me->def->original_id);
        snprintf(posbuf, MAX_POSBUF, ":%s", iseq_name);
        line = -1;
    }

    kprintf("c:%04"PRIdPTRDIFF" ",
            ((rb_control_frame_t *)(ec->vm_stack + ec->vm_stack_size) - cfp));
    if (pc == -1) {
        kprintf("p:---- ");
    }
    else {
        kprintf("p:%04"PRIdPTRDIFF" ", pc);
    }
    kprintf("s:%04"PRIdPTRDIFF" ", cfp->sp - ec->vm_stack);
    kprintf(ep_in_heap == ' ' ? "e:%06"PRIdPTRDIFF" " : "E:%06"PRIxPTRDIFF" ", ep % 10000);
    kprintf("%-6s", magic);
    if (line) {
        kprintf(" %s", posbuf);
    }
    if (VM_FRAME_FINISHED_P(cfp)) {
        kprintf(" [FINISH]");
    }
    if (0) {
        kprintf("              \t");
        kprintf("iseq: %-24s ", iseq_name);
        kprintf("self: %-24s ", selfstr);
        kprintf("%-1s ", biseq_name);
    }
    kprintf("\n");

    // additional information for CI machines
    if (ruby_on_ci) {
        char buff[0x100];

        if (me) {
            if (IMEMO_TYPE_P(me, imemo_ment)) {
                kprintf("  me:\n");
                kprintf("    called_id: %s, type: %s\n", rb_id2name(me->called_id), rb_method_type_name(me->def->type));
                kprintf("    owner class: %s\n", rb_raw_obj_info(buff, 0x100, me->owner));
                if (me->owner != me->defined_class) {
                    kprintf("    defined_class: %s\n", rb_raw_obj_info(buff, 0x100, me->defined_class));
                }
            }
            else {
                kprintf(" me is corrupted (%s)\n", rb_raw_obj_info(buff, 0x100, (VALUE)me));
            }
        }

        kprintf("  self: %s\n", rb_raw_obj_info(buff, 0x100, cfp->self));

        if (iseq) {
            if (ISEQ_BODY(iseq)->local_table_size > 0) {
                kprintf("  lvars:\n");
                for (unsigned int i=0; i<ISEQ_BODY(iseq)->local_table_size; i++) {
                    const VALUE *argv = cfp->ep - ISEQ_BODY(cfp->iseq)->local_table_size - VM_ENV_DATA_SIZE + 1;
                    kprintf("    %s: %s\n",
                            rb_id2name(ISEQ_BODY(iseq)->local_table[i]),
                            rb_raw_obj_info(buff, 0x100, argv[i]));
                }
            }
        }
    }
    return true;
  error:
    return false;
}

bool
rb_vmdebug_stack_dump_raw(const rb_execution_context_t *ec, const rb_control_frame_t *cfp, FILE *errout)
{
#if 0
    VALUE *sp = cfp->sp;
    const VALUE *ep = cfp->ep;
    VALUE *p, *st, *t;

    kprintf("-- stack frame ------------\n");
    for (p = st = ec->vm_stack; p < sp; p++) {
        kprintf("%04ld (%p): %08"PRIxVALUE, (long)(p - st), p, *p);

        t = (VALUE *)*p;
        if (ec->vm_stack <= t && t < sp) {
            kprintf(" (= %ld)", (long)((VALUE *)GC_GUARDED_PTR_REF((VALUE)t) - ec->vm_stack));
        }

        if (p == ep)
            kprintf(" <- ep");

        kprintf("\n");
    }
#endif

    kprintf("-- Control frame information "
            "-----------------------------------------------\n");
    while ((void *)cfp < (void *)(ec->vm_stack + ec->vm_stack_size)) {
        control_frame_dump(ec, cfp, errout);
        cfp++;
    }
    kprintf("\n");
    return true;

  error:
    return false;
}

bool
rb_vmdebug_stack_dump_raw_current(void)
{
    const rb_execution_context_t *ec = GET_EC();
    return rb_vmdebug_stack_dump_raw(ec, ec->cfp, stderr);
}

bool
rb_vmdebug_env_dump_raw(const rb_env_t *env, const VALUE *ep, FILE *errout)
{
    unsigned int i;
    kprintf("-- env --------------------\n");

    while (env) {
        kprintf("--\n");
        for (i = 0; i < env->env_size; i++) {
            kprintf("%04d: %08"PRIxVALUE" (%p)", i, env->env[i], (void *)&env->env[i]);
            if (&env->env[i] == ep) kprintf(" <- ep");
            kprintf("\n");
        }

        env = rb_vm_env_prev_env(env);
    }
    kprintf("---------------------------\n");
    return true;

  error:
    return false;
}

bool
rb_vmdebug_proc_dump_raw(rb_proc_t *proc, FILE *errout)
{
    const rb_env_t *env;
    char *selfstr;
    VALUE val = rb_inspect(vm_block_self(&proc->block));
    selfstr = StringValueCStr(val);

    kprintf("-- proc -------------------\n");
    kprintf("self: %s\n", selfstr);
    env = VM_ENV_ENVVAL_PTR(vm_block_ep(&proc->block));
    rb_vmdebug_env_dump_raw(env, vm_block_ep(&proc->block), errout);
    return true;

  error:
    return false;
}

bool
rb_vmdebug_stack_dump_th(VALUE thval, FILE *errout)
{
    rb_thread_t *target_th = rb_thread_ptr(thval);
    return rb_vmdebug_stack_dump_raw(target_th->ec, target_th->ec->cfp, errout);
}

#if VMDEBUG > 2

/* copy from vm_insnhelper.c */
static const VALUE *
vm_base_ptr(const rb_control_frame_t *cfp)
{
    const rb_control_frame_t *prev_cfp = RUBY_VM_PREVIOUS_CONTROL_FRAME(cfp);
    const VALUE *bp = prev_cfp->sp + ISEQ_BODY(cfp->iseq)->local_table_size + VM_ENV_DATA_SIZE;

    if (ISEQ_BODY(cfp->iseq)->type == ISEQ_TYPE_METHOD || VM_FRAME_BMETHOD_P(cfp)) {
        bp += 1;
    }
    return bp;
}

static void
vm_stack_dump_each(const rb_execution_context_t *ec, const rb_control_frame_t *cfp, FILE *errout)
{
    int i, argc = 0, local_table_size = 0;
    VALUE rstr;
    VALUE *sp = cfp->sp;
    const VALUE *ep = cfp->ep;

    if (VM_FRAME_RUBYFRAME_P(cfp)) {
        const rb_iseq_t *iseq = cfp->iseq;
        argc = ISEQ_BODY(iseq)->param.lead_num;
        local_table_size = ISEQ_BODY(iseq)->local_table_size;
    }

    /* stack trace header */

    if (VM_FRAME_TYPE(cfp) == VM_FRAME_MAGIC_METHOD||
        VM_FRAME_TYPE(cfp) == VM_FRAME_MAGIC_TOP   ||
        VM_FRAME_TYPE(cfp) == VM_FRAME_MAGIC_BLOCK ||
        VM_FRAME_TYPE(cfp) == VM_FRAME_MAGIC_CLASS ||
        VM_FRAME_TYPE(cfp) == VM_FRAME_MAGIC_CFUNC ||
        VM_FRAME_TYPE(cfp) == VM_FRAME_MAGIC_IFUNC ||
        VM_FRAME_TYPE(cfp) == VM_FRAME_MAGIC_EVAL  ||
        VM_FRAME_TYPE(cfp) == VM_FRAME_MAGIC_RESCUE)
    {
        const VALUE *ptr = ep - local_table_size;

        control_frame_dump(ec, cfp, errout);

        for (i = 0; i < argc; i++) {
            rstr = rb_inspect(*ptr);
            kprintf("  arg   %2d: %8s (%p)\n", i, StringValueCStr(rstr),
                    (void *)ptr++);
        }
        for (; i < local_table_size - 1; i++) {
            rstr = rb_inspect(*ptr);
            kprintf("  local %2d: %8s (%p)\n", i, StringValueCStr(rstr),
                    (void *)ptr++);
        }

        ptr = vm_base_ptr(cfp);
        for (; ptr < sp; ptr++, i++) {
            switch (TYPE(*ptr)) {
              case T_UNDEF:
                rstr = rb_str_new2("undef");
                break;
              case T_IMEMO:
                rstr = rb_str_new2("imemo"); /* TODO: can put mode detail information */
                break;
              default:
                rstr = rb_inspect(*ptr);
                break;
            }
            kprintf("  stack %2d: %8s (%"PRIdPTRDIFF")\n", i, StringValueCStr(rstr),
                    (ptr - ec->vm_stack));
        }
    }
    else if (VM_FRAME_FINISHED_P(cfp)) {
        if (ec->vm_stack + ec->vm_stack_size > (VALUE *)(cfp + 1)) {
            vm_stack_dump_each(ec, cfp + 1, errout);
        }
        else {
            /* SDR(); */
        }
    }
    else {
        rb_bug("unsupported frame type: %08lx", VM_FRAME_TYPE(cfp));
    }
}
#endif

bool
rb_vmdebug_debug_print_register(const rb_execution_context_t *ec, FILE *errout)
{
    rb_control_frame_t *cfp = ec->cfp;
    ptrdiff_t pc = -1;
    ptrdiff_t ep = cfp->ep - ec->vm_stack;
    ptrdiff_t cfpi;

    if (VM_FRAME_RUBYFRAME_P(cfp)) {
        pc = cfp->pc - ISEQ_BODY(cfp->iseq)->iseq_encoded;
    }

    if (ep < 0 || (size_t)ep > ec->vm_stack_size) {
        ep = -1;
    }

    cfpi = ((rb_control_frame_t *)(ec->vm_stack + ec->vm_stack_size)) - cfp;
    kprintf("  [PC] %04"PRIdPTRDIFF", [SP] %04"PRIdPTRDIFF", [EP] %04"PRIdPTRDIFF", [CFP] %04"PRIdPTRDIFF"\n",
            pc, (cfp->sp - ec->vm_stack), ep, cfpi);
    return true;

  error:
    return false;
}

bool
rb_vmdebug_thread_dump_regs(VALUE thval, FILE *errout)
{
    return rb_vmdebug_debug_print_register(rb_thread_ptr(thval)->ec, errout);
}

bool
rb_vmdebug_debug_print_pre(const rb_execution_context_t *ec, const rb_control_frame_t *cfp, const VALUE *_pc, FILE *errout)
{
    const rb_iseq_t *iseq = cfp->iseq;

    if (iseq != 0) {
        ptrdiff_t pc = _pc - ISEQ_BODY(iseq)->iseq_encoded;
        int i;

        for (i=0; i<(int)VM_CFP_CNT(ec, cfp); i++) {
            kprintf(" ");
        }
        kprintf("| ");
        if(0) kprintf("[%03ld] ", (long)(cfp->sp - ec->vm_stack));

        /* printf("%3"PRIdPTRDIFF" ", VM_CFP_CNT(ec, cfp)); */
        if (pc >= 0) {
            const VALUE *iseq_original = rb_iseq_original_iseq((rb_iseq_t *)iseq);

            rb_iseq_disasm_insn(0, iseq_original, (size_t)pc, iseq, 0);
        }
    }

#if VMDEBUG > 3
    kprintf("        (1)");
    rb_vmdebug_debug_print_register(errout, ec);
#endif
    return true;

  error:
    return false;
}

bool
rb_vmdebug_debug_print_post(const rb_execution_context_t *ec, const rb_control_frame_t *cfp, FILE *errout)
{
#if VMDEBUG > 9
    if (!rb_vmdebug_stack_dump_raw(ec, cfp, errout)) goto errout;
#endif

#if VMDEBUG > 3
    kprintf("        (2)");
    rb_vmdebug_debug_print_register(errout, ec);
#endif
    /* stack_dump_raw(ec, cfp); */

#if VMDEBUG > 2
    /* stack_dump_thobj(ec); */
    vm_stack_dump_each(ec, ec->cfp, errout);

    kprintf
        ("--------------------------------------------------------------\n");
#endif
    return true;

#if VMDEBUG > 2
  error:
    return false;
#endif
}

VALUE
rb_vmdebug_thread_dump_state(FILE *errout, VALUE self)
{
    rb_thread_t *th = rb_thread_ptr(self);
    rb_control_frame_t *cfp = th->ec->cfp;

    kprintf("Thread state dump:\n");
    kprintf("pc : %p, sp : %p\n", (void *)cfp->pc, (void *)cfp->sp);
    kprintf("cfp: %p, ep : %p\n", (void *)cfp, (void *)cfp->ep);

  error:
    return Qnil;
}

#if defined __APPLE__
# include <AvailabilityMacros.h>
# if defined(MAC_OS_X_VERSION_10_5) && MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_5
#   define MCTX_SS_REG(reg) __ss.__##reg
# else
#   define MCTX_SS_REG(reg) ss.reg
# endif
#endif

#if defined(HAVE_BACKTRACE)
# define USE_BACKTRACE 1
# ifdef HAVE_LIBUNWIND
#  undef backtrace
#  define backtrace unw_backtrace
# elif defined(__APPLE__) && defined(HAVE_LIBUNWIND_H) \
    && defined(MAC_OS_X_VERSION_10_6) && MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_6
#  define UNW_LOCAL_ONLY
#  include <libunwind.h>
#  include <sys/mman.h>
#  undef backtrace

#  if defined(__arm64__)
static bool
is_coroutine_start(unw_word_t ip)
{
#if defined(USE_MN_THREADS) && USE_MN_THREADS
    struct coroutine_context;
    extern void ruby_coroutine_start(struct coroutine_context *, struct coroutine_context *);
    return ((void *)(ip) == (void *)ruby_coroutine_start);
#else
    return false;
#endif
}
#  endif

int
backtrace(void **trace, int size)
{
    unw_cursor_t cursor; unw_context_t uc;
    unw_word_t ip;
    int n = 0;

    unw_getcontext(&uc);
    unw_init_local(&cursor, &uc);
#  if defined(__x86_64__)
    while (unw_step(&cursor) > 0) {
        unw_get_reg(&cursor, UNW_REG_IP, &ip);
        trace[n++] = (void *)ip;
        {
            char buf[256];
            unw_get_proc_name(&cursor, buf, 256, &ip);
            if (strncmp("_sigtramp", buf, sizeof("_sigtramp")) == 0) {
                goto darwin_sigtramp;
            }
        }
    }
    return n;
darwin_sigtramp:
    /* darwin's bundled libunwind doesn't support signal trampoline */
    {
        ucontext_t *uctx;
        char vec[1];
        int r;
        /* get previous frame information from %rbx at _sigtramp and set values to cursor
         * https://www.opensource.apple.com/source/Libc/Libc-825.25/i386/sys/_sigtramp.s
         * https://www.opensource.apple.com/source/libunwind/libunwind-35.1/src/unw_getcontext.s
         */
        unw_get_reg(&cursor, UNW_X86_64_RBX, &ip);
        uctx = (ucontext_t *)ip;
        unw_set_reg(&cursor, UNW_X86_64_RAX, uctx->uc_mcontext->MCTX_SS_REG(rax));
        unw_set_reg(&cursor, UNW_X86_64_RBX, uctx->uc_mcontext->MCTX_SS_REG(rbx));
        unw_set_reg(&cursor, UNW_X86_64_RCX, uctx->uc_mcontext->MCTX_SS_REG(rcx));
        unw_set_reg(&cursor, UNW_X86_64_RDX, uctx->uc_mcontext->MCTX_SS_REG(rdx));
        unw_set_reg(&cursor, UNW_X86_64_RDI, uctx->uc_mcontext->MCTX_SS_REG(rdi));
        unw_set_reg(&cursor, UNW_X86_64_RSI, uctx->uc_mcontext->MCTX_SS_REG(rsi));
        unw_set_reg(&cursor, UNW_X86_64_RBP, uctx->uc_mcontext->MCTX_SS_REG(rbp));
        unw_set_reg(&cursor, UNW_X86_64_RSP, 8+(uctx->uc_mcontext->MCTX_SS_REG(rsp)));
        unw_set_reg(&cursor, UNW_X86_64_R8,  uctx->uc_mcontext->MCTX_SS_REG(r8));
        unw_set_reg(&cursor, UNW_X86_64_R9,  uctx->uc_mcontext->MCTX_SS_REG(r9));
        unw_set_reg(&cursor, UNW_X86_64_R10, uctx->uc_mcontext->MCTX_SS_REG(r10));
        unw_set_reg(&cursor, UNW_X86_64_R11, uctx->uc_mcontext->MCTX_SS_REG(r11));
        unw_set_reg(&cursor, UNW_X86_64_R12, uctx->uc_mcontext->MCTX_SS_REG(r12));
        unw_set_reg(&cursor, UNW_X86_64_R13, uctx->uc_mcontext->MCTX_SS_REG(r13));
        unw_set_reg(&cursor, UNW_X86_64_R14, uctx->uc_mcontext->MCTX_SS_REG(r14));
        unw_set_reg(&cursor, UNW_X86_64_R15, uctx->uc_mcontext->MCTX_SS_REG(r15));
        ip = uctx->uc_mcontext->MCTX_SS_REG(rip);

        /* There are 4 cases for SEGV:
         * (1) called invalid address
         * (2) read or write invalid address
         * (3) received signal
         *
         * Detail:
         * (1) called invalid address
         * In this case, saved ip is invalid address.
         * It needs to just save the address for the information,
         * skip the frame, and restore the frame calling the
         * invalid address from %rsp.
         * The problem is how to check whether the ip is valid or not.
         * This code uses mincore(2) and assume the address's page is
         * incore/referenced or not reflects the problem.
         * Note that High Sierra's mincore(2) may return -128.
         * (2) read or write invalid address
         * saved ip is valid. just restart backtracing.
         * (3) received signal in user space
         * Same as (2).
         * (4) received signal in kernel
         * In this case saved ip points just after syscall, but registers are
         * already overwritten by kernel. To fix register consistency,
         * skip libc's kernel wrapper.
         * To detect this case, just previous two bytes of ip is "\x0f\x05",
         * syscall instruction of x86_64.
         */
        r = mincore((const void *)ip, 1, vec);
        if (r || vec[0] <= 0 || memcmp((const char *)ip-2, "\x0f\x05", 2) == 0) {
            /* if segv is caused by invalid call or signal received in syscall */
            /* the frame is invalid; skip */
            trace[n++] = (void *)ip;
            ip = *(unw_word_t*)uctx->uc_mcontext->MCTX_SS_REG(rsp);
        }

        trace[n++] = (void *)ip;
        unw_set_reg(&cursor, UNW_REG_IP, ip);
    }
    while (unw_step(&cursor) > 0) {
        unw_get_reg(&cursor, UNW_REG_IP, &ip);
        trace[n++] = (void *)ip;
    }
    return n;

#  elif defined(__arm64__)
    /* Since Darwin arm64's _sigtramp is implemented as normal function,
     * unwind can unwind frames without special code.
     * https://github.com/apple/darwin-libplatform/blob/215b09856ab5765b7462a91be7076183076600df/src/setjmp/generic/sigtramp.c
     */
    while (unw_step(&cursor) > 0) {
        unw_get_reg(&cursor, UNW_REG_IP, &ip);
        // Strip Arm64's pointer authentication.
        // https://developer.apple.com/documentation/security/preparing_your_app_to_work_with_pointer_authentication
        // I wish I could use "ptrauth_strip()" but I get an error:
        // "this target does not support pointer authentication"
        trace[n++] = (void *)(ip & 0x7fffffffffffull);

        // Apple's libunwind can't handle our coroutine switching code
        if (is_coroutine_start(ip)) break;
    }
    return n;
#  else
#    error unsupported architecture
#  endif
}
# elif defined(BROKEN_BACKTRACE)
#  undef USE_BACKTRACE
#  define USE_BACKTRACE 0
# endif
#else
# define USE_BACKTRACE 0
#endif

#if USE_BACKTRACE
# include <execinfo.h>
#elif defined(_WIN32)
# include <imagehlp.h>
# ifndef SYMOPT_DEBUG
#  define SYMOPT_DEBUG 0x80000000
# endif
# ifndef MAX_SYM_NAME
# define MAX_SYM_NAME 2000
typedef struct {
    DWORD64 Offset;
    WORD Segment;
    ADDRESS_MODE Mode;
} ADDRESS64;
typedef struct {
    DWORD64 Thread;
    DWORD ThCallbackStack;
    DWORD ThCallbackBStore;
    DWORD NextCallback;
    DWORD FramePointer;
    DWORD64 KiCallUserMode;
    DWORD64 KeUserCallbackDispatcher;
    DWORD64 SystemRangeStart;
    DWORD64 KiUserExceptionDispatcher;
    DWORD64 StackBase;
    DWORD64 StackLimit;
    DWORD64 Reserved[5];
} KDHELP64;
typedef struct {
    ADDRESS64 AddrPC;
    ADDRESS64 AddrReturn;
    ADDRESS64 AddrFrame;
    ADDRESS64 AddrStack;
    ADDRESS64 AddrBStore;
    void *FuncTableEntry;
    DWORD64 Params[4];
    BOOL Far;
    BOOL Virtual;
    DWORD64 Reserved[3];
    KDHELP64 KdHelp;
} STACKFRAME64;
typedef struct {
    ULONG SizeOfStruct;
    ULONG TypeIndex;
    ULONG64 Reserved[2];
    ULONG Index;
    ULONG Size;
    ULONG64 ModBase;
    ULONG Flags;
    ULONG64 Value;
    ULONG64 Address;
    ULONG Register;
    ULONG Scope;
    ULONG Tag;
    ULONG NameLen;
    ULONG MaxNameLen;
    char Name[1];
} SYMBOL_INFO;
typedef struct {
    DWORD SizeOfStruct;
    void *Key;
    DWORD LineNumber;
    char *FileName;
    DWORD64 Address;
} IMAGEHLP_LINE64;
typedef void *PREAD_PROCESS_MEMORY_ROUTINE64;
typedef void *PFUNCTION_TABLE_ACCESS_ROUTINE64;
typedef void *PGET_MODULE_BASE_ROUTINE64;
typedef void *PTRANSLATE_ADDRESS_ROUTINE64;
# endif

struct dump_thead_arg {
    DWORD tid;
    FILE *errout;
};

static void
dump_thread(void *arg)
{
    HANDLE dbghelp;
    BOOL (WINAPI *pSymInitialize)(HANDLE, const char *, BOOL);
    BOOL (WINAPI *pSymCleanup)(HANDLE);
    BOOL (WINAPI *pStackWalk64)(DWORD, HANDLE, HANDLE, STACKFRAME64 *, void *, PREAD_PROCESS_MEMORY_ROUTINE64, PFUNCTION_TABLE_ACCESS_ROUTINE64, PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64);
    DWORD64 (WINAPI *pSymGetModuleBase64)(HANDLE, DWORD64);
    BOOL (WINAPI *pSymFromAddr)(HANDLE, DWORD64, DWORD64 *, SYMBOL_INFO *);
    BOOL (WINAPI *pSymGetLineFromAddr64)(HANDLE, DWORD64, DWORD *, IMAGEHLP_LINE64 *);
    HANDLE (WINAPI *pOpenThread)(DWORD, BOOL, DWORD);
    DWORD tid = ((struct dump_thead_arg *)arg)->tid;
    FILE *errout = ((struct dump_thead_arg *)arg)->errout;
    HANDLE ph;
    HANDLE th;

    dbghelp = LoadLibrary("dbghelp.dll");
    if (!dbghelp) return;
    pSymInitialize = (BOOL (WINAPI *)(HANDLE, const char *, BOOL))GetProcAddress(dbghelp, "SymInitialize");
    pSymCleanup = (BOOL (WINAPI *)(HANDLE))GetProcAddress(dbghelp, "SymCleanup");
    pStackWalk64 = (BOOL (WINAPI *)(DWORD, HANDLE, HANDLE, STACKFRAME64 *, void *, PREAD_PROCESS_MEMORY_ROUTINE64, PFUNCTION_TABLE_ACCESS_ROUTINE64, PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64))GetProcAddress(dbghelp, "StackWalk64");
    pSymGetModuleBase64 = (DWORD64 (WINAPI *)(HANDLE, DWORD64))GetProcAddress(dbghelp, "SymGetModuleBase64");
    pSymFromAddr = (BOOL (WINAPI *)(HANDLE, DWORD64, DWORD64 *, SYMBOL_INFO *))GetProcAddress(dbghelp, "SymFromAddr");
    pSymGetLineFromAddr64 = (BOOL (WINAPI *)(HANDLE, DWORD64, DWORD *, IMAGEHLP_LINE64 *))GetProcAddress(dbghelp, "SymGetLineFromAddr64");
    pOpenThread = (HANDLE (WINAPI *)(DWORD, BOOL, DWORD))GetProcAddress(GetModuleHandle("kernel32.dll"), "OpenThread");
    if (pSymInitialize && pSymCleanup && pStackWalk64 && pSymGetModuleBase64 &&
        pSymFromAddr && pSymGetLineFromAddr64 && pOpenThread) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_DEBUG | SYMOPT_LOAD_LINES);
        ph = GetCurrentProcess();
        pSymInitialize(ph, NULL, TRUE);
        th = pOpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT, FALSE, tid);
        if (th) {
            if (SuspendThread(th) != (DWORD)-1) {
                CONTEXT context;
                memset(&context, 0, sizeof(context));
                context.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(th, &context)) {
                    char libpath[MAX_PATH];
                    char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
                    SYMBOL_INFO *info = (SYMBOL_INFO *)buf;
                    DWORD mac;
                    STACKFRAME64 frame;
                    memset(&frame, 0, sizeof(frame));
#if defined(_M_AMD64) || defined(__x86_64__)
                    mac = IMAGE_FILE_MACHINE_AMD64;
                    frame.AddrPC.Mode = AddrModeFlat;
                    frame.AddrPC.Offset = context.Rip;
                    frame.AddrFrame.Mode = AddrModeFlat;
                    frame.AddrFrame.Offset = context.Rbp;
                    frame.AddrStack.Mode = AddrModeFlat;
                    frame.AddrStack.Offset = context.Rsp;
#elif defined(_M_ARM64) || defined(__aarch64__)
                    mac = IMAGE_FILE_MACHINE_ARM64;
                    frame.AddrPC.Mode = AddrModeFlat;
                    frame.AddrPC.Offset = context.Pc;
                    frame.AddrFrame.Mode = AddrModeFlat;
                    frame.AddrFrame.Offset = context.Fp;
                    frame.AddrStack.Mode = AddrModeFlat;
                    frame.AddrStack.Offset = context.Sp;
#else	/* i386 */
                    mac = IMAGE_FILE_MACHINE_I386;
                    frame.AddrPC.Mode = AddrModeFlat;
                    frame.AddrPC.Offset = context.Eip;
                    frame.AddrFrame.Mode = AddrModeFlat;
                    frame.AddrFrame.Offset = context.Ebp;
                    frame.AddrStack.Mode = AddrModeFlat;
                    frame.AddrStack.Offset = context.Esp;
#endif

                    while (pStackWalk64(mac, ph, th, &frame, &context, NULL,
                                        NULL, NULL, NULL)) {
                        DWORD64 addr = frame.AddrPC.Offset;
                        IMAGEHLP_LINE64 line;
                        DWORD64 displacement;
                        DWORD tmp;

                        if (addr == frame.AddrReturn.Offset || addr == 0 ||
                            frame.AddrReturn.Offset == 0)
                            break;

                        memset(buf, 0, sizeof(buf));
                        info->SizeOfStruct = sizeof(SYMBOL_INFO);
                        info->MaxNameLen = MAX_SYM_NAME;
                        if (pSymFromAddr(ph, addr, &displacement, info)) {
                            if (GetModuleFileName((HANDLE)(uintptr_t)pSymGetModuleBase64(ph, addr), libpath, sizeof(libpath)))
                                kprintf("%s", libpath);
                            kprintf("(%s+0x%"PRI_64_PREFIX"x)",
                                    info->Name, displacement);
                        }
                        kprintf(" [0x%p]", (void *)(VALUE)addr);
                        memset(&line, 0, sizeof(line));
                        line.SizeOfStruct = sizeof(line);
                        if (pSymGetLineFromAddr64(ph, addr, &tmp, &line))
                            kprintf(" %s:%lu", line.FileName, line.LineNumber);
                        kprintf("\n");
                    }
                }

              error:
                ResumeThread(th);
            }
            CloseHandle(th);
        }
        pSymCleanup(ph);
    }
    FreeLibrary(dbghelp);
}
#endif

void
rb_print_backtrace(FILE *errout)
{
#if USE_BACKTRACE
#define MAX_NATIVE_TRACE 1024
    static void *trace[MAX_NATIVE_TRACE];
    int n = (int)backtrace(trace, MAX_NATIVE_TRACE);
#if (defined(USE_ELF) || defined(HAVE_MACH_O_LOADER_H)) && defined(HAVE_DLADDR) && !defined(__sparc)
    rb_dump_backtrace_with_lines(n, trace, errout);
#else
    char **syms = backtrace_symbols(trace, n);
    if (syms) {
        int i;
        for (i=0; i<n; i++) {
            kprintf("%s\n", syms[i]);
        }
        free(syms);
    }
  error:
    /* ignore errors at writing */;
#endif
#elif defined(_WIN32)
    struct dump_thead_arg arg = {
        .tid = GetCurrentThreadId(),
        .errout = errout,
    };
    HANDLE th = (HANDLE)_beginthread(dump_thread, 0, &arg);
    if (th != (HANDLE)-1)
        WaitForSingleObject(th, INFINITE);
#endif
}

#ifdef HAVE_LIBPROCSTAT
struct procstat;
struct kinfo_proc;
static void procstat_vm(struct procstat *, struct kinfo_proc *, FILE *);
#include "missing/procstat_vm.c"
#endif

#if defined __linux__
# if defined(__x86_64__) || defined(__i386__)
#   define dump_machine_register(reg) (col_count = print_machine_register(errout, mctx->gregs[REG_##reg], #reg, col_count, 80))
# elif defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64)
#   define dump_machine_register(reg, regstr) (col_count = print_machine_register(errout, reg, regstr, col_count, 80))
# endif
#elif defined __APPLE__
# if defined(__aarch64__)
#   define dump_machine_register(reg, regstr) (col_count = print_machine_register(errout, mctx->MCTX_SS_REG(reg), regstr, col_count, 80))
# else
#   define dump_machine_register(reg) (col_count = print_machine_register(errout, mctx->MCTX_SS_REG(reg), #reg, col_count, 80))
# endif
#endif

#ifdef dump_machine_register
static int
print_machine_register(FILE *errout, size_t reg, const char *reg_name, int col_count, int max_col)
{
    int ret;
    char buf[64];
    static const int size_width = sizeof(size_t) * CHAR_BIT / 4;

    ret = snprintf(buf, sizeof(buf), " %3.3s: 0x%.*" PRIxSIZE, reg_name, size_width, reg);
    if (col_count + ret > max_col) {
        kputs("\n");
        col_count = 0;
    }
    col_count += ret;
    kputs(buf);
    return col_count;

  error:
    return -1;
}

static bool
rb_dump_machine_register(FILE *errout, const ucontext_t *ctx)
{
    int col_count = 0;
    if (!ctx) return true;

    kprintf("-- Machine register context "
            "------------------------------------------------\n");

# if defined __linux__
    {
        const mcontext_t *const mctx = &ctx->uc_mcontext;
#   if defined __x86_64__
        dump_machine_register(RIP);
        dump_machine_register(RBP);
        dump_machine_register(RSP);
        dump_machine_register(RAX);
        dump_machine_register(RBX);
        dump_machine_register(RCX);
        dump_machine_register(RDX);
        dump_machine_register(RDI);
        dump_machine_register(RSI);
        dump_machine_register(R8);
        dump_machine_register(R9);
        dump_machine_register(R10);
        dump_machine_register(R11);
        dump_machine_register(R12);
        dump_machine_register(R13);
        dump_machine_register(R14);
        dump_machine_register(R15);
        dump_machine_register(EFL);
#   elif defined __i386__
        dump_machine_register(GS);
        dump_machine_register(FS);
        dump_machine_register(ES);
        dump_machine_register(DS);
        dump_machine_register(EDI);
        dump_machine_register(ESI);
        dump_machine_register(EBP);
        dump_machine_register(ESP);
        dump_machine_register(EBX);
        dump_machine_register(EDX);
        dump_machine_register(ECX);
        dump_machine_register(EAX);
        dump_machine_register(TRAPNO);
        dump_machine_register(ERR);
        dump_machine_register(EIP);
        dump_machine_register(CS);
        dump_machine_register(EFL);
        dump_machine_register(UESP);
        dump_machine_register(SS);
#   elif defined __aarch64__
        dump_machine_register(mctx->regs[0], "x0");
        dump_machine_register(mctx->regs[1], "x1");
        dump_machine_register(mctx->regs[2], "x2");
        dump_machine_register(mctx->regs[3], "x3");
        dump_machine_register(mctx->regs[4], "x4");
        dump_machine_register(mctx->regs[5], "x5");
        dump_machine_register(mctx->regs[6], "x6");
        dump_machine_register(mctx->regs[7], "x7");
        dump_machine_register(mctx->regs[18], "x18");
        dump_machine_register(mctx->regs[19], "x19");
        dump_machine_register(mctx->regs[20], "x20");
        dump_machine_register(mctx->regs[21], "x21");
        dump_machine_register(mctx->regs[22], "x22");
        dump_machine_register(mctx->regs[23], "x23");
        dump_machine_register(mctx->regs[24], "x24");
        dump_machine_register(mctx->regs[25], "x25");
        dump_machine_register(mctx->regs[26], "x26");
        dump_machine_register(mctx->regs[27], "x27");
        dump_machine_register(mctx->regs[28], "x28");
        dump_machine_register(mctx->regs[29], "x29");
        dump_machine_register(mctx->sp, "sp");
        dump_machine_register(mctx->fault_address, "fault_address");
#   elif defined __arm__
        dump_machine_register(mctx->arm_r0, "r0");
        dump_machine_register(mctx->arm_r1, "r1");
        dump_machine_register(mctx->arm_r2, "r2");
        dump_machine_register(mctx->arm_r3, "r3");
        dump_machine_register(mctx->arm_r4, "r4");
        dump_machine_register(mctx->arm_r5, "r5");
        dump_machine_register(mctx->arm_r6, "r6");
        dump_machine_register(mctx->arm_r7, "r7");
        dump_machine_register(mctx->arm_r8, "r8");
        dump_machine_register(mctx->arm_r9, "r9");
        dump_machine_register(mctx->arm_r10, "r10");
        dump_machine_register(mctx->arm_sp, "sp");
        dump_machine_register(mctx->fault_address, "fault_address");
#   elif defined __riscv
        dump_machine_register(mctx->__gregs[REG_SP], "sp");
        dump_machine_register(mctx->__gregs[REG_S0], "s0");
        dump_machine_register(mctx->__gregs[REG_S1], "s1");
        dump_machine_register(mctx->__gregs[REG_A0], "a0");
        dump_machine_register(mctx->__gregs[REG_A0+1], "a1");
        dump_machine_register(mctx->__gregs[REG_A0+2], "a2");
        dump_machine_register(mctx->__gregs[REG_A0+3], "a3");
        dump_machine_register(mctx->__gregs[REG_A0+4], "a4");
        dump_machine_register(mctx->__gregs[REG_A0+5], "a5");
        dump_machine_register(mctx->__gregs[REG_A0+6], "a6");
        dump_machine_register(mctx->__gregs[REG_A0+7], "a7");
        dump_machine_register(mctx->__gregs[REG_S2], "s2");
        dump_machine_register(mctx->__gregs[REG_S2+1], "s3");
        dump_machine_register(mctx->__gregs[REG_S2+2], "s4");
        dump_machine_register(mctx->__gregs[REG_S2+3], "s5");
        dump_machine_register(mctx->__gregs[REG_S2+4], "s6");
        dump_machine_register(mctx->__gregs[REG_S2+5], "s7");
        dump_machine_register(mctx->__gregs[REG_S2+6], "s8");
        dump_machine_register(mctx->__gregs[REG_S2+7], "s9");
        dump_machine_register(mctx->__gregs[REG_S2+8], "s10");
        dump_machine_register(mctx->__gregs[REG_S2+9], "s11");
#   elif defined __loongarch64
        dump_machine_register(mctx->__gregs[LARCH_REG_SP], "sp");
        dump_machine_register(mctx->__gregs[LARCH_REG_A0], "a0");
        dump_machine_register(mctx->__gregs[LARCH_REG_A0+1], "a1");
        dump_machine_register(mctx->__gregs[LARCH_REG_A0+2], "a2");
        dump_machine_register(mctx->__gregs[LARCH_REG_A0+3], "a3");
        dump_machine_register(mctx->__gregs[LARCH_REG_A0+4], "a4");
        dump_machine_register(mctx->__gregs[LARCH_REG_A0+5], "a5");
        dump_machine_register(mctx->__gregs[LARCH_REG_A0+6], "a6");
        dump_machine_register(mctx->__gregs[LARCH_REG_A0+7], "a7");
        dump_machine_register(mctx->__gregs[LARCH_REG_A0+8], "fp");
        dump_machine_register(mctx->__gregs[LARCH_REG_S0], "s0");
        dump_machine_register(mctx->__gregs[LARCH_REG_S1], "s1");
        dump_machine_register(mctx->__gregs[LARCH_REG_S2], "s2");
        dump_machine_register(mctx->__gregs[LARCH_REG_S0+3], "s3");
        dump_machine_register(mctx->__gregs[LARCH_REG_S0+4], "s4");
        dump_machine_register(mctx->__gregs[LARCH_REG_S0+5], "s5");
        dump_machine_register(mctx->__gregs[LARCH_REG_S0+6], "s6");
        dump_machine_register(mctx->__gregs[LARCH_REG_S0+7], "s7");
        dump_machine_register(mctx->__gregs[LARCH_REG_S0+8], "s8");
#   endif
    }
# elif defined __APPLE__
    {
        const mcontext_t mctx = ctx->uc_mcontext;
#   if defined __x86_64__
        dump_machine_register(rax);
        dump_machine_register(rbx);
        dump_machine_register(rcx);
        dump_machine_register(rdx);
        dump_machine_register(rdi);
        dump_machine_register(rsi);
        dump_machine_register(rbp);
        dump_machine_register(rsp);
        dump_machine_register(r8);
        dump_machine_register(r9);
        dump_machine_register(r10);
        dump_machine_register(r11);
        dump_machine_register(r12);
        dump_machine_register(r13);
        dump_machine_register(r14);
        dump_machine_register(r15);
        dump_machine_register(rip);
        dump_machine_register(rflags);
#   elif defined __i386__
        dump_machine_register(eax);
        dump_machine_register(ebx);
        dump_machine_register(ecx);
        dump_machine_register(edx);
        dump_machine_register(edi);
        dump_machine_register(esi);
        dump_machine_register(ebp);
        dump_machine_register(esp);
        dump_machine_register(ss);
        dump_machine_register(eflags);
        dump_machine_register(eip);
        dump_machine_register(cs);
        dump_machine_register(ds);
        dump_machine_register(es);
        dump_machine_register(fs);
        dump_machine_register(gs);
#   elif defined __aarch64__
        dump_machine_register(x[0], "x0");
        dump_machine_register(x[1], "x1");
        dump_machine_register(x[2], "x2");
        dump_machine_register(x[3], "x3");
        dump_machine_register(x[4], "x4");
        dump_machine_register(x[5], "x5");
        dump_machine_register(x[6], "x6");
        dump_machine_register(x[7], "x7");
        dump_machine_register(x[18], "x18");
        dump_machine_register(x[19], "x19");
        dump_machine_register(x[20], "x20");
        dump_machine_register(x[21], "x21");
        dump_machine_register(x[22], "x22");
        dump_machine_register(x[23], "x23");
        dump_machine_register(x[24], "x24");
        dump_machine_register(x[25], "x25");
        dump_machine_register(x[26], "x26");
        dump_machine_register(x[27], "x27");
        dump_machine_register(x[28], "x28");
        dump_machine_register(lr, "lr");
        dump_machine_register(fp, "fp");
        dump_machine_register(sp, "sp");
#   endif
    }
# endif
    kprintf("\n\n");
    return true;

  error:
    return false;
}
#else
# define rb_dump_machine_register(errout, ctx) ((void)0)
#endif /* dump_machine_register */

bool
rb_vm_bugreport(const void *ctx, FILE *errout)
{
    const char *cmd = getenv("RUBY_ON_BUG");
    if (cmd) {
        char buf[0x100];
        snprintf(buf, sizeof(buf), "%s %"PRI_PIDT_PREFIX"d", cmd, getpid());
        int r = system(buf);
        if (r == -1) {
            snprintf(buf, sizeof(buf), "Launching RUBY_ON_BUG command failed.");
        }
    }

    // Thread unsafe best effort attempt to stop printing the bug report in an
    // infinite loop. Can happen with corrupt Ruby stack.
    {
        static bool crashing = false;
        if (crashing) {
            kprintf("Crashed while printing bug report\n");
            return true;
        }
        crashing = true;
    }

#ifdef __linux__
# define PROC_MAPS_NAME "/proc/self/maps"
#endif
#ifdef PROC_MAPS_NAME
    enum {other_runtime_info = 1};
#else
    enum {other_runtime_info = 0};
#endif
    const rb_vm_t *const vm = GET_VM();
    const rb_execution_context_t *ec = rb_current_execution_context(false);

    if (vm && ec) {
        rb_vmdebug_stack_dump_raw(ec, ec->cfp, errout);
        rb_backtrace_print_as_bugreport(errout);
        kputs("\n");
        // If we get here, hopefully things are intact enough that
        // we can read these two numbers. It is an estimate because
        // we are reading without synchronization.
        kprintf("-- Threading information "
                "---------------------------------------------------\n");
        kprintf("Total ractor count: %u\n", vm->ractor.cnt);
        kprintf("Ruby thread count for this ractor: %u\n", rb_ec_ractor_ptr(ec)->threads.cnt);
        if (rb_fiber_scheduler_get() != Qnil) {
            kprintf("Note that the Fiber scheduler is enabled\n");
        }
        kputs("\n");
    }

    rb_dump_machine_register(errout, ctx);

#if USE_BACKTRACE || defined(_WIN32)
    kprintf("-- C level backtrace information "
            "-------------------------------------------\n");
    rb_print_backtrace(errout);


    kprintf("\n");
#endif /* USE_BACKTRACE */

    if (other_runtime_info || vm) {
        kprintf("-- Other runtime information "
                "-----------------------------------------------\n\n");
    }
    if (vm && !rb_during_gc()) {
        int i;
        VALUE name;
        long len;
        const int max_name_length = 1024;
# define LIMITED_NAME_LENGTH(s) \
        (((len = RSTRING_LEN(s)) > max_name_length) ? max_name_length : (int)len)

        name = vm->progname;
        if (name) {
            kprintf("* Loaded script: %.*s\n",
                    LIMITED_NAME_LENGTH(name), RSTRING_PTR(name));
            kprintf("\n");
        }
        if (vm->loaded_features) {
            kprintf("* Loaded features:\n\n");
            for (i=0; i<RARRAY_LEN(vm->loaded_features); i++) {
                name = RARRAY_AREF(vm->loaded_features, i);
                if (RB_TYPE_P(name, T_STRING)) {
                    kprintf(" %4d %.*s\n", i,
                            LIMITED_NAME_LENGTH(name), RSTRING_PTR(name));
                }
                else if (RB_TYPE_P(name, T_CLASS) || RB_TYPE_P(name, T_MODULE)) {
                    const char *const type = RB_TYPE_P(name, T_CLASS) ?
                        "class" : "module";
                    name = rb_search_class_path(rb_class_real(name));
                    if (!RB_TYPE_P(name, T_STRING)) {
                        kprintf(" %4d %s:<unnamed>\n", i, type);
                        continue;
                    }
                    kprintf(" %4d %s:%.*s\n", i, type,
                            LIMITED_NAME_LENGTH(name), RSTRING_PTR(name));
                }
                else {
                    VALUE klass = rb_search_class_path(rb_obj_class(name));
                    if (!RB_TYPE_P(klass, T_STRING)) {
                        kprintf(" %4d #<%p:%p>\n", i,
                                (void *)CLASS_OF(name), (void *)name);
                        continue;
                    }
                    kprintf(" %4d #<%.*s:%p>\n", i,
                            LIMITED_NAME_LENGTH(klass), RSTRING_PTR(klass),
                            (void *)name);
                }
            }
        }
        kprintf("\n");
    }

    {
#ifndef RUBY_ASAN_ENABLED
# ifdef PROC_MAPS_NAME
        {
            FILE *fp = fopen(PROC_MAPS_NAME, "r");
            if (fp) {
                kprintf("* Process memory map:\n\n");

                while (!feof(fp)) {
                    char buff[0x100];
                    size_t rn = fread(buff, 1, 0x100, fp);
                    if (fwrite(buff, 1, rn, errout) != rn)
                        break;
                }

                fclose(fp);
                kprintf("\n\n");
            }
        }
# endif /* __linux__ */
# ifdef HAVE_LIBPROCSTAT
#  define MIB_KERN_PROC_PID_LEN 4
        int mib[MIB_KERN_PROC_PID_LEN];
        struct kinfo_proc kp;
        size_t len = sizeof(struct kinfo_proc);
        mib[0] = CTL_KERN;
        mib[1] = KERN_PROC;
        mib[2] = KERN_PROC_PID;
        mib[3] = getpid();
        if (sysctl(mib, MIB_KERN_PROC_PID_LEN, &kp, &len, NULL, 0) == -1) {
            kprintf("sysctl: %s\n", strerror(errno));
        }
        else {
            struct procstat *prstat = procstat_open_sysctl();
            kprintf("* Process memory map:\n\n");
            procstat_vm(prstat, &kp, errout);
            procstat_close(prstat);
            kprintf("\n");
        }
# endif /* __FreeBSD__ */
# ifdef __APPLE__
        vm_address_t addr = 0;
        vm_size_t size = 0;
        struct vm_region_submap_info map;
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT;
        natural_t depth = 0;

        kprintf("* Process memory map:\n\n");
        while (1) {
            if (vm_region_recurse(mach_task_self(), &addr, &size, &depth,
                        (vm_region_recurse_info_t)&map, &count) != KERN_SUCCESS) {
                break;
            }

            if (map.is_submap) {
                // We only look at main addresses
                depth++;
            }
            else {
                kprintf("%lx-%lx %s%s%s", addr, (addr+size),
                        ((map.protection & VM_PROT_READ) != 0 ? "r" : "-"),
                        ((map.protection & VM_PROT_WRITE) != 0 ? "w" : "-"),
                    ((map.protection & VM_PROT_EXECUTE) != 0 ? "x" : "-"));
#  ifdef HAVE_LIBPROC_H
                char buff[PATH_MAX];
                if (proc_regionfilename(getpid(), addr, buff, sizeof(buff)) > 0) {
                    kprintf(" %s", buff);
                }
#  endif
                kprintf("\n");
            }

            addr += size;
            size = 0;
        }
# endif
#endif
    }
    return true;

  error:
    return false;
}

bool
rb_vmdebug_stack_dump_all_threads(void)
{
    rb_thread_t *th = NULL;
    rb_ractor_t *r = GET_RACTOR();
    FILE *errout = stderr;

    // TODO: now it only shows current ractor
    ccan_list_for_each(&r->threads.set, th, lt_node) {
#ifdef NON_SCALAR_THREAD_ID
        kprintf("th: %p, native_id: N/A\n", th);
#else
        kprintf("th: %p, native_id: %p\n", (void *)th, (void *)(uintptr_t)th->nt->thread_id);
#endif
        if (!rb_vmdebug_stack_dump_raw(th->ec, th->ec->cfp, errout)) goto error;
    }
    return true;

  error:
    return false;
}

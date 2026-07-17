/* sys/ucontext.h -- EmbLink override header (newlib ships none).
 *
 * ucontext_t is the machine state a SIGNAL HANDLER is handed so it can report
 * where the fault happened. EmbLink has no asynchronous signal delivery at all
 * (docs/INTERRUPTION.md: cancellation is OBSERVED at syscall boundaries, never
 * INJECTED into control flow), so no handler can ever run and nothing can ever
 * fill one of these in.
 *
 * It exists so portable code COMPILES: TCC's backtrace installs a SIGSEGV
 * handler and reads uc_mcontext to walk the stack. That handler is registered
 * (signal() accepts it -- see memory's signals notes) and then never fires,
 * which is a true statement about this OS rather than a broken promise.
 *
 * Deliberately NOT a faithful x86-64 mcontext: filling in gregs[] with a plausible
 * layout would invite someone to believe a handler could read a real fault
 * address out of it. The fields are the shape callers reference, zeroed and
 * never written by anything. */
#ifndef _EMBK_SYS_UCONTEXT_H
#define _EMBK_SYS_UCONTEXT_H

#include <stddef.h>
#include <stdint.h>

/* Register indices callers name (TCC reads REG_RIP/REG_RBP). Values match the
 * x86-64 ABI's ordering so the NAMES are not misleading; nothing populates them. */
#define REG_R8   0
#define REG_R9   1
#define REG_R10  2
#define REG_R11  3
#define REG_R12  4
#define REG_R13  5
#define REG_R14  6
#define REG_R15  7
#define REG_RDI  8
#define REG_RSI  9
#define REG_RBP  10
#define REG_RBX  11
#define REG_RDX  12
#define REG_RAX  13
#define REG_RCX  14
#define REG_RSP  15
#define REG_RIP  16
#define NGREG    23

typedef long long greg_t;
typedef greg_t gregset_t[NGREG];

typedef struct {
    gregset_t gregs;
    void     *fpregs;
    unsigned long long __reserved[8];
} mcontext_t;

typedef struct ucontext_t {
    unsigned long      uc_flags;
    struct ucontext_t *uc_link;
    struct { void *ss_sp; int ss_flags; size_t ss_size; } uc_stack;
    mcontext_t         uc_mcontext;
    unsigned long      uc_sigmask;
} ucontext_t;

#endif /* _EMBK_SYS_UCONTEXT_H */

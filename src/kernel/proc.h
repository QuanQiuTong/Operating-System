#pragma once

#include <common/defines.h>
#include <common/list.h>
#include <common/rbtree.h>
#include <kernel/pt.h>
#include <fs/file.h>
#include <fs/inode.h>
#include <common/sem.h>

enum procstate { UNUSED, RUNNABLE, RUNNING, SLEEPING, DEEPSLEEPING, ZOMBIE };

typedef struct UserContext {
    u64 q0[2];
    u64 spsr;
    u64 elr;
    u64 sp;
    // u64 ttbr0;
    u64 tpidr0;
    u64 x[32];
} UserContext;

typedef struct KernelContext {
    u64 lr, x0, x1;
    u64 x[11];  // x19-29
} KernelContext;

// embeded data for procs
struct schinfo {
    ListNode rq;
};

typedef struct Proc {
    bool killed;
    bool idle;
    int pid;
    int exitcode;
    enum procstate state;
    Semaphore childexit;
    ListNode children;
    ListNode ptnode;
    struct Proc* parent;
    struct schinfo schinfo;
    struct pgdir pgdir;
    void *kstack;
    UserContext *ucontext;
    KernelContext *kcontext;
    struct oftable oftable;
    Inode *cwd;
} Proc;

void init_kproc();
void init_proc(Proc *);
WARN_RESULT Proc *create_proc();
int start_proc(Proc *, void (*entry)(u64), u64 arg);
NO_RETURN void exit(int code);
WARN_RESULT int wait(int *exitcode);
WARN_RESULT int kill(int pid);
WARN_RESULT int fork();

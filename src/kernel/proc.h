#pragma once

#include <common/defines.h>
#include <common/list.h>
#include <common/rbtree.h>
#include <common/sem.h>

enum procstate { UNUSED,
                 RUNNABLE,
                 RUNNING,
                 SLEEPING,
                 ZOMBIE };

typedef struct UserContext {
    u64 q00, q01;
    u64 tpidr, useless;
    u64 spsr, elr, lr, sp;
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
    void* kstack;
    UserContext* ucontext;
    KernelContext* kcontext;
} Proc;

void init_kproc();
void init_proc(Proc*);
Proc* create_proc();
int start_proc(Proc*, void (*entry)(u64), u64 arg);
NO_RETURN void exit(int code);
int wait(int* exitcode);

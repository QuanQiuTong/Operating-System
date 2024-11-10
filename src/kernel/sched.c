#include <aarch64/intrinsic.h>
#include <common/rbtree.h>
#include <kernel/cpu.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>

extern bool panic_flag;

extern void swtch(KernelContext *new_ctx, KernelContext **old_ctx);

static SpinLock rqlock = {0};
static ListNode rq = {&rq, &rq};  // runnable queue

static void sched_timer_handler(struct timer *t) {
    t->data = 0;
    acquire_sched_lock();
    sched(RUNNABLE);
}

// Increasing the elapse will increase write speed (to a certain extent), but will also decrease read speed.
static const int ELAPSE = 4;
static struct timer timer[NCPU] = {[0 ... NCPU - 1] = {true, ELAPSE, 0, {0}, sched_timer_handler, 0}};

void init_sched() {
    // 1. initialize the resources (e.g. locks, semaphores)
    init_spinlock(&rqlock);
    init_list_node(&rq);

    // 2. initialize the scheduler info of each CPU
    for (int i = 0; i < NCPU; ++i) {
        Proc *p = kalloc(sizeof(Proc));
        init_proc(p);
        p->idle = true;
        p->state = RUNNING;
        cpus[i].sched = (struct sched){p, p};
        timer[i] = (struct timer){true, ELAPSE, 0, {0}, sched_timer_handler, i};
    }
}

#define scheduler() (cpus[cpuid()].sched)

/// @return the current process
Proc *thisproc() {
    return scheduler().thisproc;
}

/// @brief initialize schinfo for every newly-created process
void init_schinfo(struct schinfo *p) {
    init_list_node(&p->rq);
}

static SpinLock sched_lock = {0};

void acquire_sched_lock() {
    acquire_spinlock(&sched_lock);
}

void release_sched_lock() {
    release_spinlock(&sched_lock);
}

bool is_zombie(Proc *p) {
    acquire_sched_lock();
    bool r = p->state == ZOMBIE;
    release_sched_lock();
    return r;
}

bool is_unused(Proc *p) {
    acquire_sched_lock();
    bool r = p->state == UNUSED;
    release_sched_lock();
    return r;
}

bool activate_proc(Proc *p) {
    // if the proc->state is RUNNING/RUNNABLE, do nothing
    // if the proc->state if SLEEPING/UNUSED, set the process state to RUNNABLE and add it to the sched queue
    // else: panic

    acquire_sched_lock();  // lock for rq
    switch (p->state) {
        case RUNNABLE:
        case RUNNING:
        case ZOMBIE:
            release_sched_lock();
            return false;

        case UNUSED:
        case SLEEPING:
            p->state = RUNNABLE;
            insert_into_list(&rqlock, &rq, &p->schinfo.rq);
            release_sched_lock();
            return true;

        default:  // should never reach here
    }
    printk("activate_proc: unexpected state %d\n", p->state);
    release_sched_lock();
    PANIC();
}

static void update_this_state(enum procstate new_state) {
    // TODO: if you use template sched function, you should implement this routinue
    // update the state of current process to new_state, and modify the sched queue if necessary

    Proc *this = thisproc();
    if (this != scheduler().idle && (this->state == RUNNABLE || this->state == RUNNING)) {
        detach_from_list(&rqlock, &this->schinfo.rq);
    }
    this->state = new_state;
    if (this != scheduler().idle && (new_state == RUNNABLE || new_state == RUNNING)) {
        insert_into_list(&rqlock, &rq, &this->schinfo.rq);
    }
}

static Proc *pick_next() {
    // TODO: if using template sched function, you should implement this routinue
    // choose the next process to run, and return idle if no runnable process

    if (panic_flag)
        return scheduler().idle;

    acquire_spinlock(&rqlock);
    _for_in_list(p, &rq) {
        if (p == &rq) {
            continue;
        }
        Proc *proc = container_of(p, Proc, schinfo.rq);
        if (proc->state == RUNNABLE) {
            release_spinlock(&rqlock);
            return proc;
        }
        if (proc->state > ZOMBIE) {
            printk("pick_next: found a corrupted process\n");
            PANIC();
        }
        if (p == p->next) {
            printk("pick_next: rq %p is corrupted\n", p);
            
            // _detach_from_list(p);
            break;
        }
    }
    release_spinlock(&rqlock);
    return scheduler().idle;
}

static void update_this_proc(Proc *p) {
    scheduler().thisproc = p;

    if (timer[cpuid()].triggered)
        set_cpu_timer(&timer[cpuid()]);
}

// A simple scheduler.
// You are allowed to replace it with whatever you like.
// call with sched_lock
void sched(enum procstate new_state) {
    Proc *this = thisproc();
    ASSERT(this->state == RUNNING);
    if (this->killed && new_state != ZOMBIE) {
        release_sched_lock();
        return;
    }
    update_this_state(new_state);
    Proc *next = pick_next();
    update_this_proc(next);
    ASSERT(next->state == RUNNABLE);
    next->state = RUNNING;
    if (next != this) {
        attach_pgdir(&next->pgdir);
        swtch(next->kcontext, &this->kcontext);
    }
    release_sched_lock();
}

u64 proc_entry(void (*entry)(u64), u64 arg) {
    release_sched_lock();
    set_return_addr(entry);
    return arg;
}

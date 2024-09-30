#include <aarch64/mmu.h>
#include <common/list.h>
#include <common/string.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>

Proc root_proc;

void kernel_entry();
void proc_entry();

static ListNode pidpool;
static int pid;

/// @brief initializes the kernel process
/// @note  should call after kinit
void init_kproc() {
    // 1. init global resources (e.g. locks, semaphores)
    init_list_node(&pidpool);
    pid = 0;

    // 2. init the root_proc
    init_proc(&root_proc);
    root_proc.parent = &root_proc;
    start_proc(&root_proc, kernel_entry, 123456);
}
typedef struct {
    int id;
    ListNode lnode;
} PidNode;

/// @brief setup the Proc with kstack and pid allocated
void init_proc(Proc* p) {
    p->killed = false;
    p->idle = false;
    if (_empty_list(&pidpool)) {
        p->pid = ++pid;
    } else {
        PidNode* pidn = container_of(pidpool.next, PidNode, lnode);
        p->pid = pidn->id;
        _detach_from_list(&pidn->lnode);
        kfree(pidn);
    }
    p->exitcode = 0;
    p->state = UNUSED;
    init_sem(&p->childexit, 0);
    init_list_node(&p->children);
    init_list_node(&p->ptnode);
    p->parent = NULL;
    init_schinfo(&p->schinfo);
    p->kstack = kalloc_page();
    p->ucontext = (UserContext*)((u64)p->kstack + PAGE_SIZE - 16 - sizeof(UserContext));
    p->kcontext = (KernelContext*)((u64)p->ucontext - sizeof(KernelContext));
}

Proc* create_proc() {
    Proc* p = kalloc(sizeof(Proc));
    init_proc(p);
    return p;
}

void set_parent_to_this(Proc* proc) {
    ASSERT(proc->parent == NULL);

    proc->parent = thisproc();
    _insert_into_list(&thisproc()->children, &proc->ptnode);
}

int start_proc(Proc* p, void (*entry)(u64), u64 arg) {
    // 1. set the parent to root_proc if NULL
    if (p->parent == NULL) {
        p->parent = &root_proc;
        _insert_into_list(&root_proc.children, &p->ptnode);
    }

    // 2. setup the kcontext to make the proc start with proc_entry(entry, arg)
    p->kcontext->lr = (u64)&proc_entry;
    p->kcontext->x0 = (u64)entry;
    p->kcontext->x1 = (u64)arg;

    // 3. activate the proc and return its pid
    int id = p->pid;  // concurrency safe
    activate_proc(p);
    return id;
}

#define for_list(node) for (ListNode* p = node.next; p != &node; p = p->next)

int wait(int* exitcode) {
    // 1. return -1 if no children
    Proc* this = thisproc();
    if (_empty_list(&this->children)) {
        return -1;
    }

    // 2. wait for childexit
    /* printk("[%x] wait for childexit\n", (i32)(i64)this); */
    wait_sem(&this->childexit);
    /* printk("[%x] recieve childexit\n", (i32)(i64)this); */

    // 3. if any child exits, clean it up and return its pid and exitcode

    acquire_sched_lock();
    int id = -1;
    for_list(this->children) {
        auto childproc = container_of(p, Proc, ptnode);
        if (childproc->state != ZOMBIE)
            continue;
        /* printk("[%x] clean\n", (i32)(i64)childproc); */

        _detach_from_list(&childproc->ptnode);
        _detach_from_list(&childproc->schinfo.rq);
        *exitcode = childproc->exitcode;
        kfree_page(childproc->kstack);

        id = childproc->pid;
        PidNode* pidn = kalloc(sizeof(PidNode));
        init_list_node(&pidn->lnode);
        pidn->id = id;
        _insert_into_list(&pidpool, &pidn->lnode);
        kfree(childproc);
        break;
    }

    release_sched_lock();
    return id;
}

NO_RETURN void exit(int code) {
    // 1. set the exitcode
    Proc* this = thisproc();
    ASSERT(this != &root_proc);
    this->exitcode = code;

    // 2. clean up the resources
    // 3. transfer children to the root_proc, and notify the root_proc if there is zombie
    int zcnt = 0;
    for_list(this->children) {
        Proc* childproc = container_of(p, Proc, ptnode);
        childproc->parent = &root_proc;
        zcnt += (childproc->state == ZOMBIE);
        /* printk("[%x] post childexit\n", (i32)(i64)childproc); */
    }
    _merge_list(&root_proc.children, _detach_from_list(&this->children));

    while (zcnt--)
        post_sem(&root_proc.childexit);

    post_sem(&this->parent->childexit);

    // 4. sched(ZOMBIE)
    acquire_sched_lock();  // avoid double-free
    sched(ZOMBIE);

    PANIC();  // prevent the warning of 'no_return function returns'
}

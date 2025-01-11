#include <common/list.h>
#include <common/string.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>

Proc root_proc;

void kernel_entry();
void proc_entry();

static ListNode pidpool = {&pidpool, &pidpool};
static int pid = 0;
static SpinLock plock = {0};

void init_kproc() {
    init_proc(&root_proc);
    root_proc.parent = &root_proc;
    start_proc(&root_proc, kernel_entry, 123456);
}

typedef struct {
    int id;
    ListNode lnode;
} PidNode;

/// @brief setup the Proc with kstack and pid allocated
void init_proc(Proc *p) {
    p->killed = false;
    p->idle = false;

    /// @note be careful of concurrency
    acquire_spinlock(&plock);
    if (_empty_list(&pidpool)) {
        p->pid = ++pid;
    } else {
        PidNode *pidn = container_of(pidpool.next, PidNode, lnode);
        p->pid = pidn->id;
        _detach_from_list(&pidn->lnode);
        kfree(pidn);
    }
    release_spinlock(&plock);

    p->exitcode = 0;
    p->state = UNUSED;
    init_sem(&p->childexit, 0);
    init_list_node(&p->children);
    init_list_node(&p->ptnode);
    p->parent = NULL;
    init_schinfo(&p->schinfo);
    init_pgdir(&p->pgdir);
    p->kstack = memset(kalloc_page(), 0, PAGE_SIZE);
    p->ucontext = (UserContext *)((u64)p->kstack + PAGE_SIZE - 16 - sizeof(UserContext));
    p->kcontext = (KernelContext *)((u64)p->ucontext - sizeof(KernelContext));
}

Proc *create_proc() {
    Proc *p = kalloc(sizeof(Proc));
    init_proc(p);
    return p;
}

void set_parent_to_this(Proc *proc) {
    /// @note maybe you need to lock the process tree
    acquire_spinlock(&plock);
    // ASSERT(proc->parent == NULL);
    proc->parent = thisproc();
    _insert_into_list(&thisproc()->children, &proc->ptnode);
    release_spinlock(&plock);
}

int start_proc(Proc *p, void (*entry)(u64), u64 arg) {
    /// @note be careful of concurrency
    // 1. set the parent to root_proc if NULL
    acquire_spinlock(&plock);
    if (p->parent == NULL) {
        p->parent = &root_proc;
        _insert_into_list(&root_proc.children, &p->ptnode);
    }
    release_spinlock(&plock);

    // 2. setup the kcontext to make the proc start with proc_entry(entry, arg)
    p->kcontext->lr = (u64)&proc_entry;
    p->kcontext->x0 = (u64)entry;
    p->kcontext->x1 = (u64)arg;

    //  3. activate the proc and return its pid
    int id = p->pid;  // why?
    activate_proc(p);
    return id;
}

#define destroy_proc(p, exitcode) ({           \
    _detach_from_list(&p->ptnode);             \
    _detach_from_list(&p->schinfo.rq);         \
    kfree_page(p->kstack);                     \
                                               \
    PidNode *pidn = kalloc(sizeof(PidNode));   \
    pidn->id = p->pid;                         \
    _insert_into_list(&pidpool, &pidn->lnode); \
                                               \
    if (exitcode != NULL)                      \
        *exitcode = p->exitcode;               \
    int id = p->pid;                           \
    kfree(p);                                  \
    id;                                        \
})

#define for_list(node) for (ListNode *p = node.next; p != &node; p = p->next)

int wait(int *exitcode) {
    /// @note be careful of concurrency

    // 1. return -1 if no children
    Proc *this = thisproc();
    acquire_spinlock(&plock);
    if (_empty_list(&this->children)) {
        release_spinlock(&plock);
        return -1;
    }
    release_spinlock(&plock);

    // 2. wait for childexit
    if (!wait_sem(&this->childexit)) {
        // printk("wait_sem failed\n");
        return -1;
    }

    // 3. if any child exits, clean it up and return its pid and exitcode
    int id = -1;
    acquire_spinlock(&plock);
    acquire_sched_lock();
    for_list(this->children) {
        Proc *childproc = container_of(p, Proc, ptnode);
        if (childproc->state == ZOMBIE) {
            id = destroy_proc(childproc, exitcode);
            break;
        }
    }
    release_sched_lock();
    release_spinlock(&plock);
    return id;
}

NO_RETURN void exit(int code) {
    //  1. set the exitcode
    //  2. clean up the resources
    //  3. transfer children to the root_proc, and notify the root_proc if there is zombie
    //  4. notify the parent
    //  5. sched(ZOMBIE)
    /// @note be careful of concurrency

    acquire_spinlock(&plock);

    Proc *this = thisproc();
    ASSERT(this != &root_proc);
    this->exitcode = code;
    post_sem(&this->parent->childexit);

    int zcnt = 0;
    for_list(this->children) {
        Proc *childproc = container_of(p, Proc, ptnode);
        childproc->parent = &root_proc;
        zcnt += (childproc->state == ZOMBIE);
    }
    if (!_empty_list(&this->children)) {
        _merge_list(&root_proc.children, this->children.next);
        _detach_from_list(&this->children);
        while (zcnt--)
            post_sem(&root_proc.childexit);
    }
    acquire_sched_lock();
    free_pgdir(&this->pgdir);

    release_spinlock(&plock);
    sched(ZOMBIE);

    PANIC();  // prevent the warning of 'no_return function returns'
}

static Proc *find_and_kill(int pid, Proc *now) {
    if (now->pid == pid && !is_unused(now)) {
        now->killed = true;
        return now;
    }
    for_list(now->children) {
        auto childproc = container_of(p, Proc, ptnode);
        Proc *q = find_and_kill(pid, childproc);
        if (q)
            return q;
    }
    return NULL;
}

int kill(int pid) {
    // Set the killed flag of the proc to true and return 0.
    // Return -1 if the pid is invalid (proc not found).

    acquire_spinlock(&plock);
    Proc *p = find_and_kill(pid, &root_proc);
    release_spinlock(&plock);
    if (p && (p->ucontext->elr >> 48) == 0) {
        alert_proc(p);
        return 0;
    }
    return -1;
}

struct pgdir*vm_copy(struct pgdir*pgdir){
    struct pgdir* newpgdir = kalloc(sizeof(struct pgdir));
    init_pgdir(newpgdir);
    if (!newpgdir)
        return 0;

    for (int i = 0; i < N_PTE_PER_TABLE; i++)
        if (pgdir->pt[i] & PTE_VALID) {
            ASSERT(pgdir->pt[i] & PTE_TABLE);
            PTEntriesPtr pgt1 = (PTEntriesPtr)P2K(PTE_ADDRESS(pgdir->pt[i]));
            for (int i1 = 0; i1 < N_PTE_PER_TABLE; i1++)
                if (pgt1[i1] & PTE_VALID) {
                    ASSERT(pgt1[i1] & PTE_TABLE);
                    PTEntriesPtr pgt2 = (PTEntriesPtr)P2K(PTE_ADDRESS(pgt1[i1]));
                    for (int i2 = 0; i2 < N_PTE_PER_TABLE; i2++)
                        if (pgt2[i2] & PTE_VALID) {
                            ASSERT(pgt2[i2] & PTE_TABLE);
                            PTEntriesPtr pgt3 = (PTEntriesPtr)P2K(PTE_ADDRESS(pgt2[i2]));
                            for (int i3 = 0; i3 < N_PTE_PER_TABLE; i3++)
                                if (pgt3[i3] & PTE_VALID) {
                                    ASSERT(pgt3[i3] & PTE_PAGE);
                                    ASSERT(pgt3[i3] & PTE_USER);
                                    ASSERT(pgt3[i3] & PTE_NORMAL);
                                    u64 va =(u64)i<<(12+9*3)|(u64)i1<<(12+9*2)|(u64)i2<<(12+9)|i3<<12;
                                    // u64 pa = P2K(PTE_ADDRESS(pgt3[i3]));
                                    // vmmap_without_changepd(newpgdir,va,(void*)pa,PTE_RO|PTE_USER_DATA);
                                    u64 pa=PTE_ADDRESS(pgt3[i3]);
                                    void* np=kalloc_page();
                                    ASSERT(np);
                                    memmove(np,(void*)P2K(pa),PAGE_SIZE);
                                    auto pte = get_pte(newpgdir, va, true);
                                    *pte = K2P(np) | PTE_USER_DATA;
                                }
                        }
                }
        }
    return newpgdir;
}

#ifndef NOFILE
#define NOFILE (sizeof(((struct oftable *)0)->openfile) / sizeof(File *))
#endif

/*
 * Create a new process copying p as the parent.
 * Sets up stack to return as if from system call.
 */
void trap_return();
int fork() {
    /**
     * 1. Create a new child process.
     * 2. Copy the parent's memory space.
     * 3. Copy the parent's trapframe.
     * 4. Set the parent of the new proc to the parent of the parent.
     * 5. Set the state of the new proc to RUNNABLE.
     * 6. Activate the new proc and return its pid.
     */

    Proc *np = create_proc();
    if (np == NULL) {
        return -1;
    }
    Proc *cp = thisproc();
    struct pgdir *temp = vm_copy(&cp->pgdir);
    if (temp == NULL) {
        // attention
        kfree(np->kstack);
        acquire_spinlock(&plock);
        np->state = UNUSED;
        release_spinlock(&plock);
        return -1;
    }
    np->pgdir = *temp;
    np->parent = cp;
    memcpy(np->ucontext, cp->ucontext, sizeof(*np->ucontext));
    // Fork returns 0 in the child.
    np->ucontext->x[0] = 0;
    for (usize i = 0; i < NOFILE; i++)
        if (cp->oftable.openfile[i])
            np->oftable.openfile[i] = file_dup(cp->oftable.openfile[i]);
    np->cwd = inodes.share(cp->cwd);
    int pid = np->pid;
    acquire_spinlock(&plock);
    _insert_into_list(&cp->children, &np->ptnode);
    release_spinlock(&plock);
    start_proc(np, trap_return, 0);
    return pid;
}

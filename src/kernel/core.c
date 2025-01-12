#include <aarch64/intrinsic.h>
#include <kernel/cpu.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <test/test.h>

volatile bool panic_flag;

NO_RETURN void idle_entry() {
    set_cpu_on();
    while (1) {
        yield();
        if (panic_flag)
            break;
        arch_with_trap {
            arch_wfi();
        }
    }
    set_cpu_off();
    arch_stop_cpu();
}

void set_parent_to_this(Proc *proc);

NO_RETURN void kernel_entry() {
    init_filesystem();

    printk("Hello world! (Core %lld)\n", cpuid());
    // proc_test();
    // vm_test();
    // user_proc_test();
    // io_test();

    /**
     * Map init.S to user space and trap_return to run icode.
     */
    extern char icode[], eicode[];
    void trap_return();
    Proc *p = create_proc();
    for (u64 q = (u64)icode; q < (u64)eicode; q += PAGE_SIZE) {
        *get_pte(&p->pgdir, 0x400000 + q - (u64)icode, true) = K2P(q) | PTE_USER_DATA;
    }
    ASSERT(p->pgdir.pt);
    p->ucontext->x[0] = 0;
    p->ucontext->elr = 0x400000;
    // p->ucontext->ttbr0 = K2P(p->pgdir.pt);
    p->ucontext->spsr = 0;
    OpContext ctx;
    bcache.begin_op(&ctx);
    p->cwd = namei("/", &ctx);
    bcache.end_op(&ctx);
    
    set_parent_to_this(p);
    start_proc(p, trap_return, 0);
    printk("start\n");
    while (1) {
        yield();
        arch_with_trap {
            arch_wfi();
        }
    }
}

NO_INLINE NO_RETURN void _panic(const char *file, int line) {
    printk("=====%s:%d PANIC%lld!=====\n", file, line, cpuid());
    panic_flag = true;
    set_cpu_off();
    for (int i = 0; i < NCPU; i++) {
        if (cpus[i].online)
            i--;
    }
    printk("Kernel PANIC invoked at %s:%d. Stopped.\n", file, line);
    arch_stop_cpu();
}
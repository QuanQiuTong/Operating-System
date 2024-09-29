#include <aarch64/intrinsic.h>
#include <common/string.h>
#include <driver/gicv3.h>
#include <driver/interrupt.h>
#include <driver/timer.h>
#include <driver/uart.h>
#include <kernel/core.h>
#include <kernel/cpu.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>

static volatile bool boot_secondary_cpus = false;

void main() {
    if (cpuid() == 0) {
        extern char edata[], end[];
        memset(edata, 0, (usize)(end - edata));

        extern char bss[], ebss[];
        memset(bss, 0, ebss - bss);

        /* initialize interrupt handler */
        init_interrupt();

        smp_init();
        uart_init();
        printk_init();

        gicv3_init();
        gicv3_init_percpu();

        timer_init(1000);
        timer_init_percpu();

        /* initialize kernel memory allocator */
        kinit();

        /* initialize sched */
        init_sched();

        /* initialize kernel proc */
        init_kproc();

        smp_init();

        arch_fence();

        // Set a flag indicating that the secondary CPUs can start executing.
        boot_secondary_cpus = true;
    } else {
        while (!boot_secondary_cpus)
            ;
        arch_fence();
        timer_init_percpu();
        gicv3_init_percpu();
    }

    set_return_addr(idle_entry);
}

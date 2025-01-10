#include <aarch64/intrinsic.h>
#include <driver/virtio.h>
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

static void get_second_partition() {
    struct partition_entry {
        u8 boot_flag;      // 引导标志
        u8 chs_start[3];   // 起始 CHS 地址
        u8 type;           // 分区类型
        u8 chs_end[3];     // 结束 CHS 地址
        u32 lba_start;     // 起始 LBA
        u32 sector_count;  // 扇区数量
    } __attribute__((packed));
    // ASSERT(sizeof(struct partition_entry) == 16);

    Buf b = {.flags = 0, .block_no = 0};
    if (virtio_blk_rw(&b) != 0) {
        printk("Failed to read MBR.\n");
        PANIC();
    }

#define part_table_offset 0x1BE
    auto part_table = (struct partition_entry *)(b.data + part_table_offset);

    u32 lba_start = part_table[1].lba_start;
    u32 sector_count = part_table[1].sector_count;

    printk(
        "\e[0;31mSecond partition:\n"
        "    LBA start = %d, sector count = %d\e[0m\n",
        lba_start, sector_count);
}

NO_RETURN void kernel_entry() {
    init_filesystem();
    
    printk("Hello world! (Core %lld)\n", cpuid());
    proc_test();
    vm_test();
    user_proc_test();
    io_test();

    get_second_partition();

    /**
     * (Final) TODO BEGIN 
     * 
     * Map init.S to user space and trap_return to run icode.
     */


    /* (Final) TODO END */
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
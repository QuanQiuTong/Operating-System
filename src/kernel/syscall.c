#include <aarch64/intrinsic.h>
#include <common/sem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <test/test.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"

void init_syscall() {
    for (u64 *p = (u64 *)&early_init; p < (u64 *)&rest_init; p++)
        ((void (*)()) * p)();
}

void *syscall_table[NR_SYSCALL] = {
    [0 ... NR_SYSCALL - 1] = NULL,
    [SYS_myreport] = (void *)syscall_myreport,
};

void syscall_entry(UserContext *context) {
    // Invoke syscall_table[id] with args and set the return value.
    // id is stored in x8. args are stored in x0-x5. return value is stored in x0.
    // be sure to check the range of id. if id >= NR_SYSCALL, panic.

    u64 *x = context->x;
    ASSERT(x[8] < NR_SYSCALL);

    auto func = (u64(*)(u64, u64, u64, u64, u64, u64))syscall_table[x[8]];
    if(!func) {
        printk("syscall %lld not implemented\n", x[8]);
        PANIC();
    }

    x[0] = func(x[0], x[1], x[2], x[3], x[4], x[5]);
}

/**
 * Check if the virtual address [start,start+size) is READABLE by the current
 * user process.
 */
bool user_readable(const void *start, usize size) {
    for (u64 i = (u64)start; i < (u64)start + size; i = (i / BLOCK_SIZE + 1) * BLOCK_SIZE) {
        PTEntry *pte = get_pte(&thisproc()->pgdir, i, false);
        if (pte == NULL || (*pte & PTE_USER) == 0) {
            return false;
        }
    }
    return true;
}

/**
 * Check if the virtual address [start,start+size) is READABLE & WRITEABLE by
 * the current user process.
 */
bool user_writeable(const void *start, usize size) {
    for (u64 i = (u64)start; i < (u64)start + size; i = (i / BLOCK_SIZE + 1) * BLOCK_SIZE) {
        PTEntry *pte = get_pte(&thisproc()->pgdir, i, false);
        if (pte == NULL || (*pte & PTE_RO) || (*pte & PTE_USER) == 0) {
            return false;
        }
    }
    return true;
}

/**
 * Get the length of a string including tailing '\0' in the memory space of
 * current user process return 0 if the length exceeds maxlen or the string is
 * not readable by the current user process.
 */
usize user_strlen(const char *str, usize maxlen) {
    for (usize i = 0; i < maxlen; i++) {
        if (user_readable(&str[i], 1)) {
            if (str[i] == 0)
                return i + 1;
        } else
            return 0;
    }
    return 0;
}
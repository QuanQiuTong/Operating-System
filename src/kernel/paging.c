#include <aarch64/mmu.h>
#include <common/defines.h>
#include <common/list.h>
#include <common/sem.h>
#include <common/string.h>
#include <fs/block_device.h>
#include <fs/cache.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/pt.h>
#include <kernel/sched.h>

#ifdef DEBUG
#define printk(fmt, ...) printk(fmt, ##__VA_ARGS__)
#else
#define printk(fmt, ...)
#endif

__attribute__((unused)) void init_sections(ListNode *section_head) {
    printk("init_sections\n");
    struct section *sec = kalloc(sizeof(struct section));
    sec->flags = (0 | ST_HEAP);
    sec->begin = 0;
    sec->end = 0;
    _insert_into_list(section_head, &sec->stnode);
    sec->fp = NULL;
}

#define for_list(node) for (ListNode *p = node.next; p != &node; p = p->next)

void free_sections(struct pgdir *pd) {
    printk("free_sections\n");
    for_list(pd->section_head) {
        struct section *sec = container_of(p, struct section, stnode);
        for (u64 i = PAGE_BASE(sec->begin); i < sec->end; i += PAGE_SIZE) {
            auto pte = get_pte(pd, i, false);
            if (pte && (*pte & PTE_VALID))
                kfree_page((void *)P2K(PTE_ADDRESS(*pte)));
        }
        if (sec->fp) {
            file_close(sec->fp);
        }
        if (sec->flags & ST_FILE) {
            kfree(sec);
        }
    }
}

#define REVERSED_PAGES 1024  // Reversed pages

void *alloc_page_for_user() {
    // no swap

    // while (left_page_cnt() <= REVERSED_PAGES) {
    //     // TODO
    //     return NULL;
    // }
    return kalloc_page();
}

/**
 * Increase the heap size of current process by `size`.
 * If `size` is negative, decrease heap size. `size` must
 * be a multiple of PAGE_SIZE.
 *
 * @return the previous heap_end.
 */
__attribute__((unused)) u64 sbrk(i64 size) {
    // 没有任何进程调用sbrk，除了测试
    printk("\e[0;31m I found that no one is calling sbrk, except for the test\n\e[0m");

    printk("\e[0;36m sbrk: %lld, pid=%d\n\e[0m", size, thisproc()->pid);
    ASSERT(size % PAGE_SIZE == 0);

    struct pgdir *pd = &thisproc()->pgdir;
    struct section *sec = container_of(pd->section_head.next, struct section, stnode);
    u64 old_end = sec->end;

    sec->end += size;  // lazy allocation
    if (size < 0) {
        for (i64 i = 0; i < -size; i += PAGE_SIZE) {
            PTEntry *pte = get_pte(pd, sec->end + i, false);
            if (pte && *pte) {
                kfree_page((void *)(P2K(PTE_ADDRESS(*pte))));
                *pte = 0;
            }
        }
    }
    arch_tlbi_vmalle1is();
    return old_end;
}

#define USERTOP (1 + ~KSPACE_MASK)  // 0x0001000000000000
#define STACK_PAGE 32               // 128KB stack
#define USTACK_SIZE (STACK_PAGE * PAGE_SIZE)
#define USER_STACK_TOP (USERTOP - USTACK_SIZE)
#define MIN_STACK_SIZE (4 * PAGE_SIZE)

int pgfault_handler(u64 iss) {
    Proc *p = thisproc();
    struct pgdir *pd = &p->pgdir;
    u64 addr =
        arch_get_far();  // Attempting to access this address caused the page fault

    /**
     * 1. Find the section struct which contains the faulting address `addr`.
     * 2. Check section flags to determine page fault type.
     * 3. Handle the page fault accordingly.
     * 4. Return to user code or kill the process.
     */
    printk(
        "\e[0;31m"
        "pgfault_handler: pid=%d, addr=%llx\n"
        "\e[0m",
        p->pid, addr);

    if ((addr & KSPACE_MASK) || addr < MIN_STACK_SIZE) {
        printk("Invalid memory access <1> at %llx\n", addr);
        goto bad;
    }

    struct section *sec = NULL;
    for_list(pd->section_head) {
        sec = container_of(p, struct section, stnode);
        if (in_section(sec, addr))
            break;
    }

    if (!sec || !in_section(sec, addr)) {
        // 栈已经分配了。有可能是访问了未分配的部分（“爆栈”）

        // if (addr >= (USER_STACK_TOP - USTACK_SIZE) && addr < USER_STACK_TOP) {
        //     // 合法的栈访问，按需分配页面
        //     PTEntry *pte = get_pte(pd, addr, true);
        //     void *new_page = alloc_page_for_user();
        //     *pte = K2P(new_page) | PTE_USER_DATA;
        //     attach_pgdir(pd);
        //     arch_tlbi_vmalle1is();
        //     return iss;
        // }

        printk("Invalid memory access at %llx\n", addr);
        goto bad;
    }

    printk("  successfuly handled on sec: %llx %llx %x %x\n", sec->begin, sec->end, sec->flags, sec->mmap_flags);

    if (sec->mmap_flags) {
        void *new_page = kalloc_page();
        vmmap(pd, addr, new_page, PTE_USER_DATA);
        arch_tlbi_vmalle1is();
        struct file *f = sec->fp;
        if (!f->readable || f->type != FD_INODE) {
            printk("Invalid mmap file access\n");
            goto bad;
        }
        u64 offset = sec->offset + (addr - sec->begin);
        inodes.lock(f->ip);
        int n = inodes.read(f->ip, new_page, offset, PAGE_SIZE);
        inodes.unlock(f->ip);
        // 如果读取的内容不足一页，将剩余部分清零
        if (n < PAGE_SIZE) {
            memset(new_page + n, 0, PAGE_SIZE - n);
        }
        return iss;
    }

    PTEntry *pte = get_pte(pd, addr, true);
    if (*pte == 0) {  // Lazy allocation
        printk(" - Lazy allocation\n");
        vmmap(pd, addr, alloc_page_for_user(), PTE_USER_DATA);
    } else if (*pte & PTE_RO) {  // Copy on Write
        printk(" - Copy on Write\n");
        void *new_page = alloc_page_for_user();
        memcpy(new_page, (void *)P2K(PTE_ADDRESS(*pte)), PAGE_SIZE);
        vmmap(pd, addr, new_page, PTE_USER_DATA);
    } else if (!(*pte & PTE_VALID) && (sec->flags & ST_SWAP)) {
        printk("Page fault on swapped out page\n");
        PANIC();
    }

    // attach_pgdir(pd);
    arch_tlbi_vmalle1is();
    return iss;

bad:
    int k = kill(p->pid);
    ASSERT(k == 0);
    return iss;
}

/* Sharing types (must choose one and only one of these).  */
#define MAP_SHARED 0x01          /* Share changes.  */
#define MAP_PRIVATE 0x02         /* Changes are private.  */
#define MAP_SHARED_VALIDATE 0x03 /* Share changes and validate \
                                    extension flags.  */
#define MAP_TYPE 0x0f            /* Mask for type of mapping.  */

void copy_sections(ListNode *from_head, ListNode *to_head) {
    for_list((*from_head)) {
        struct section *from_sec = container_of(p, struct section, stnode);
        struct section *to_sec = kalloc(sizeof(struct section));

        memcpy(to_sec, from_sec, sizeof(struct section));

        _insert_into_list(to_head, &to_sec->stnode);

        if (from_sec->fp) {
            to_sec->fp = file_dup(from_sec->fp);
        }

        // 把父进程的页面映射复制到子进程
        struct pgdir *from_pd = container_of(from_head, struct pgdir, section_head);
        struct pgdir *to_pd = container_of(to_head, struct pgdir, section_head);

        for (u64 va = PAGE_BASE(from_sec->begin); va < from_sec->end; va += PAGE_SIZE) {
            PTEntry *pte_from = get_pte(from_pd, va, false);
            if (!pte_from || !(*pte_from & PTE_VALID))
                continue;
            // 如果是 MAP_SHARED，可以直接共用物理页 + 引用计数
            if (from_sec->mmap_flags & MAP_SHARED) {
                PTEntry *pte_to = get_pte(to_pd, va, true);
                *pte_to = *pte_from;  // 直接共享同一个物理页
            } else {
                // MAP_PRIVATE 或其它情况：分配新页并复制
                void *new_page = kalloc_page();
                memcpy(new_page, (void *)P2K(PTE_ADDRESS(*pte_from)), PAGE_SIZE);
                PTEntry *pte_to = get_pte(to_pd, va, true);
                *pte_to = K2P(new_page) | (PTE_FLAGS(*pte_from) & ~PTE_RO);
            }
        }
    }
}

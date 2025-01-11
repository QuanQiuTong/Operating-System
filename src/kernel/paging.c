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

void init_sections(ListNode *section_head) {
    printk("init_sections\n");
    struct section *sec = kalloc(sizeof(struct section));
    _insert_into_list(section_head, &sec->stnode);
    sec->begin = 0;
    sec->end = 0;
    sec->flags = (0 | ST_HEAP);
}

#define for_list(node) for (ListNode *p = node.next; p != &node; p = p->next)

void free_sections(struct pgdir *pd) {
    printk("free_sections\n");
    for_list(pd->section_head) {
        struct section *sec = container_of(p, struct section, stnode);
        if (sec->flags & ST_FILE) {
            kfree(sec);
        }
    }
}

/**
 * Increase the heap size of current process by `size`.
 * If `size` is negative, decrease heap size. `size` must
 * be a multiple of PAGE_SIZE.
 *
 * @return the previous heap_end.
 */
u64 sbrk(i64 size) {
    ASSERT(size % PAGE_SIZE == 0);

    struct pgdir *pd = &thisproc()->pgdir;
    struct section *sec = container_of(pd->section_head.next, struct section, stnode);
    u64 old_end = sec->end;

    sec->end += size;
    if (size < 0) {
        for (i64 i = 0; i < -size; i += PAGE_SIZE) {
            PTEntry *pte = get_pte(pd, sec->end + i, false);
            if (pte && *pte) {
                kfree_page((void *)(P2K(PTE_ADDRESS(*pte))));
                *pte = 0;
            }
        }
    }
    attach_pgdir(pd);
    arch_tlbi_vmalle1is();
    return old_end;
}

#define REVERSED_PAGES 1024  // Reversed pages

void *alloc_page_for_user() {
    while (left_page_cnt() <= REVERSED_PAGES) {
        // TODO
        return NULL;
    }
    return kalloc_page();
}

/* // caller must have the pd->lock
static void swapout(struct pgdir *pd, struct section *st) {
    st->flags |= ST_SWAP;
    for (u64 i = st->begin; i < st->end; i += PAGE_SIZE) {
        auto pte = get_pte(pd, i, false);
        if (pte && (*pte))
            *pte &= (~PTE_VALID);
    }
    u64 begin = st->begin;
    u64 end = st->end;
    attach_pgdir(pd);
    arch_tlbi_vmalle1is();
    // unalertable_wait_sem(&st->sleeplock);
    _release_spinlock(&pd->lock);
    if (!(st->flags & ST_FILE)) {
        for (u64 i = begin; i < end; i += PAGE_SIZE) {
            auto pte = get_pte(pd, i, false);
            if (pte && (!(*pte & PTE_VALID))) {
                *pte = write_page_to_disk((void *)P2K(CLEAN(*pte)));
            }
        }
    }
    attach_pgdir(pd);
    arch_tlbi_vmalle1is();
    sbrk(-(end - begin) / PAGE_SIZE);
    st->end = end;
    // post_sem(&st->sleeplock);
} */

#define SWAP_START 800
#define SWAP_END 1000
static bool inuse[SWAP_END-SWAP_START];
void release_8_blocks(u32 bno) {
    bno -= SWAP_START;
    for (int i = 0; i < 8; i++) {
        inuse[bno + i] = 0;
    }
}
void read_page_from_disk(void* ka, u32 bno){
    for (int i=0;i<8;i++) block_device.read(bno+i,(u8*)ka+i*512);
}
// Free 8 continuous disk blocks
static void swapin(struct pgdir *pd, struct section *st) {
    ASSERT(st->flags & ST_SWAP);
    // unalertable_wait_sem(&st->sleeplock);
    u64 begin = st->begin, end = st->end;
    for (u64 i = begin; i < end; i += PAGE_SIZE) {
        auto pte = get_pte(pd, i, false);
        if (pte && (*pte)) {
            u32 bno = (*pte);
            void *newpage = alloc_page_for_user();
            read_page_from_disk(newpage, (u32)bno);
            *pte = K2P(newpage) | PTE_USER_DATA;
            release_8_blocks(bno);
        }
    }
    attach_pgdir(pd);
    arch_tlbi_vmalle1is();
    st->flags &= ~ST_SWAP;
    // post_sem(&st->sleeplock);
}
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

    struct section *sec = NULL;
    for_list(pd->section_head) {
        sec = container_of(p, struct section, stnode);
        if (addr >= sec->begin && addr < sec->end) {
            break;
        }
    }

    if (!sec || addr < sec->begin || addr >= sec->end) {
        printk("Invalid memory access at %llx\n", addr);
        goto bad;
    }

    PTEntry *pte = get_pte(pd, addr, true);
    if (!pte) {
        printk("Failed to get PTE for address %llx\n", addr);
        goto bad;
    }

    if (*pte == 0) {
        // Lazy allocation
        void *new_page = alloc_page_for_user();
        if (!new_page) {
            printk("Failed to allocate page\n");
            goto bad;
        }
        *pte = K2P(new_page) | PTE_USER_DATA;
    } else if (*pte & PTE_RO) {
        // Copy on Write
        void *new_page = alloc_page_for_user();
        if (!new_page) {
            printk("Failed to allocate page\n");
            goto bad;
        }
        memmove(new_page, (void *)P2K(PTE_ADDRESS(*pte)), PAGE_SIZE);
        *pte = K2P(new_page) | PTE_USER_DATA;
    } else if (!(*pte & PTE_VALID) && (sec->flags & ST_SWAP)) {
        swapin(pd, sec);
    }

    attach_pgdir(pd);
    arch_tlbi_vmalle1is();
    return iss;

bad:
    int k = kill(p->pid);
    ASSERT(k == 0);
    return iss;
}

void copy_sections(ListNode *from_head, ListNode *to_head) {
    printk("copy_sections\n");
    for_list((*from_head)) {
        struct section *from_sec = container_of(p, struct section, stnode);
        struct section *to_sec = kalloc(sizeof(struct section));
        _insert_into_list(to_head, &to_sec->stnode);
        to_sec->begin = from_sec->begin;
        to_sec->end = from_sec->end;
        to_sec->flags = from_sec->flags;
    }
}

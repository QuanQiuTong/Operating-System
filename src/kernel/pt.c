#include <aarch64/intrinsic.h>
#include <common/string.h>
#include <kernel/mem.h>
#include <kernel/pt.h>

#define PTE(p) (PTEntry *)P2K(PTE_ADDRESS(p))

#define cpalloc() memset(kalloc_page(), 0, PAGE_SIZE)

#define chk(expr) ({                     \
    PTEntry* p = expr;               \
    if (!*p) {                           \
        if (!alloc)                      \
            return NULL;                 \
        *p = K2P(cpalloc()) | PTE_TABLE; \
    }                                    \
    p;                                   \
})

PTEntriesPtr get_pte(struct pgdir *pgdir, u64 va, bool alloc) {
    // Return a pointer to the PTE (Page Table Entry) for virtual address 'va'
    // If the entry not exists (NEEDN'T BE VALID), allocate it if alloc=true, or return NULL if false.
    // THIS ROUTINUE GETS THE PTE, NOT THE PAGE DESCRIBED BY PTE.

    if (!pgdir->pt) {
        if (!alloc)
            return NULL;
        pgdir->pt = cpalloc();
    }
    PTEntriesPtr p0 = chk(pgdir->pt + VA_PART0(va)),
                 p1 = chk(PTE(*p0) + VA_PART1(va)),
                 p2 = chk(PTE(*p1) + VA_PART2(va));
    return PTE(*p2) + VA_PART3(va);
}
#undef chk

void init_pgdir(struct pgdir *pgdir) {
    pgdir->pt = NULL;
}

static void free_entry(PTEntriesPtr p, unsigned deep) {
    if (deep < 3)
        for (int i = 0; i < N_PTE_PER_TABLE; ++i)
            if (p[i])
                free_entry(PTE(p[i]), deep + 1);
    kfree_page(p);
}

void free_pgdir(struct pgdir *pgdir) {
    // Free pages used by the page table. If pgdir->pt=NULL, do nothing.
    // DONT FREE PAGES DESCRIBED BY THE PAGE TABLE

    if (!pgdir->pt)
        return;
    free_entry(pgdir->pt, 0);
    pgdir->pt = NULL;
}

void attach_pgdir(struct pgdir *pgdir) {
    extern PTEntries invalid_pt;
    if (pgdir->pt)
        arch_set_ttbr0(K2P(pgdir->pt));
    else
        arch_set_ttbr0(K2P(&invalid_pt));
}

/**
 * Map virtual address 'va' to the physical address represented by kernel
 * address 'ka' in page directory 'pd', 'flags' is the flags for the page
 * table entry.
 */
void vmmap(struct pgdir *pd, u64 va, void *ka, u64 flags)
{
    /* (Final) TODO BEGIN */

    /* (Final) TODO END */
}

/*
 * Copy len bytes from p to user address va in page table pgdir.
 * Allocate physical pages if required.
 * Useful when pgdir is not the current page table.
 */
int copyout(struct pgdir *pd, void *va, void *p, usize len)
{
    /* (Final) TODO BEGIN */

    /* (Final) TODO END */
}
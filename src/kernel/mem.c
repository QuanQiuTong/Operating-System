#include <aarch64/mmu.h>
#include <common/rc.h>
#include <common/spinlock.h>
#include <driver/memlayout.h>
#include <kernel/mem.h>

#include <common/list.h>
#include <common/string.h>
#include <kernel/printk.h>

#define UPALIGN(x) (((u64)x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

RefCount kalloc_page_cnt;

static SpinLock page_lock = {0};
static ListNode list = {&list, &list};  // deleted pages.
static char *mm_end;                    // lowest allocated page address

u64 endp;
static int pagenum;  // total number of pages. avoid re-calculating.
static char *zero;

// pagenum is definitely less than 262144 = (PHYSTOP - EXTMEM) / PAGE_SIZE

_Atomic unsigned *refcnt;  // refcnt[0 ... pagenum - 1] is available.

void kinit() {
    init_rc(&kalloc_page_cnt);
    init_spinlock(&page_lock);
    init_list_node(&list);

    extern char end[];

    endp = UPALIGN(end);

    refcnt = (typeof(refcnt))endp;

    pagenum = (P2K(PHYSTOP) - endp) / (PAGE_SIZE + sizeof(refcnt[0]));

    endp += UPALIGN(pagenum * sizeof(refcnt[0]));

    mm_end = (char *)endp;

    printk("end: %p, available: %llx, page_count: %d\n", end, (P2K(PHYSTOP) - endp), pagenum);

    zero = kalloc_page();
    memset(zero, 0, PAGE_SIZE);
}

u64 left_page_cnt() {
    return pagenum - kalloc_page_cnt.count;
}

void *kalloc_page() {
    increment_rc(&kalloc_page_cnt);

    acquire_spinlock(&page_lock);

    void *ret = NULL;
    if (_empty_list(&list)) {
        ret = mm_end += PAGE_SIZE;
    } else {
        ret = list.next;
        _detach_from_list(list.next);
    }

    rc(ret) = 1;

    release_spinlock(&page_lock);
    return ret;
}

void kfree_page(void *p) {
    if (--rc(p) > 0) {
        return;
    }
    decrement_rc(&kalloc_page_cnt);

    acquire_spinlock(&page_lock);

    /*  if (p == mm_end)
            mm_end -= PAGE_SIZE;
        else  */
    _insert_into_list(&list, p);

    release_spinlock(&page_lock);
}

typedef struct Node {
    unsigned next;  // lower 32 bits of the address
    short size;
    bool free;
} Node;

#define KADDR(next) (next ? (Node *)KSPACE(next) : 0)

static void merge(Node *h) {
    Node *p = KADDR(h->next);
    if (p && p->free && PAGE_BASE(h) == PAGE_BASE(p)) {
        h->next = (u64)p->next;
        h->size += p->size + sizeof(Node);
    }
    h->free = true;
}

#define NCPU 4
static Node *free8[NCPU] = {0}, *free4[NCPU] = {0};
static SpinLock kalloc_lock[NCPU] = {0};

#include <kernel/printk.h>

void *kalloc(unsigned long long size) {
    if (size > PAGE_SIZE / 2) {
        printk("kalloc: size too large, consider using kalloc_pages or kalloc_large\n");
        PANIC();
        return NULL;
    }
    Node **fr = (size & 0x7) ? free4 : free8;
    size = (size + 3) & ~0x3;

    acquire_spinlock(&kalloc_lock[cpuid()]);

    Node *h = fr[cpuid()];
    for (; h; h = KADDR(h->next))
        if (h->free) {
            merge(h);
            if ((u64)h->size >= size)
                break;
        }

    if (h == NULL) {
        Node *p = kalloc_page();
        *p = (Node){(u64)fr[cpuid()], PAGE_SIZE - sizeof(Node), true};
        h = fr[cpuid()] = p;
    }

    short sz = sizeof(Node) + size;
    if (h->size > sz) {
        Node *p = (Node *)((u64)h + sz);
        *p = (Node){(u64)h->next, h->size - sz, true};
        h->next = (u64)p;
    }

    h->size = size;
    h->free = false;

    release_spinlock(&kalloc_lock[cpuid()]);
    return (void *)((u64)h + sizeof(Node));
}

void kfree(void *ptr) {
    acquire_spinlock(&kalloc_lock[cpuid()]);
    merge((Node *)((u64)ptr - sizeof(Node)));
    release_spinlock(&kalloc_lock[cpuid()]);
}

void *get_zero_page() {
    return zero;
}
typedef struct {
    u64 npages;
} PageHeader;

void *kalloc_large(usize size) {
    usize npages = (size + sizeof(PageHeader) + PAGE_SIZE - 1) / PAGE_SIZE;

    acquire_spinlock(&page_lock);

    if (mm_end + npages * PAGE_SIZE > (char*)P2K(PHYSTOP)) {
        release_spinlock(&page_lock);
        return NULL;
    }

    PageHeader *header = (PageHeader*)mm_end;
    header->npages = npages;

    void *ret = (void*)(mm_end + sizeof(PageHeader));

    mm_end += npages * PAGE_SIZE;

    __atomic_fetch_add(&kalloc_page_cnt.count, npages, __ATOMIC_ACQ_REL);

    release_spinlock(&page_lock);
    return ret;
}

void kfree_large(void *p) {
    if (p == NULL) {
        return;
    }

    PageHeader *header = (PageHeader*)((char*)p - sizeof(PageHeader));
    int npages = header->npages;

    acquire_spinlock(&page_lock);

    for (int i = 0; i < npages; i++) {
        void *page = (char*)header + i * PAGE_SIZE;
        _insert_into_list(&list, page);
    }
    __atomic_sub_fetch(&kalloc_page_cnt.count, npages, __ATOMIC_ACQ_REL);

    if ((char*)header + npages * PAGE_SIZE == mm_end) {
        mm_end -= npages * PAGE_SIZE;
    } else {
        // deal with memory fragmentation (optional: merge adjacent free blocks)
        // Here we simplify the processing and do not merge
    }

    release_spinlock(&page_lock);
}
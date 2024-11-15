#include <aarch64/mmu.h>
#include <common/rc.h>
#include <common/spinlock.h>
#include <driver/memlayout.h>
#include <kernel/mem.h>

#include <common/list.h>
// #include <kernel/printk.h>

RefCount kalloc_page_cnt;

static SpinLock page_lock = {0};
static ListNode list = {&list, &list};  // deleted pages.
static char *mm_end;                    // lowest allocated page address

void kinit() {
    init_rc(&kalloc_page_cnt);

    extern char end[];
    mm_end = (char *)((u64)(end - 1) & ~0xFFFull);
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

    release_spinlock(&page_lock);
    return ret;
}

void kfree_page(void *p) {
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
    Node **fr = (size & 0x7) ? free4 : free8;
    size = (size + 3) & ~0x3;

    bool l = try_acquire_spinlock(&kalloc_lock[cpuid()]);
    if(!l){
        // Never reach here
        printk("kalloc: failed to acquire spinlock\n");
        arch_yield();
        acquire_spinlock(&kalloc_lock[cpuid()]);
    }
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

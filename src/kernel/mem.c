#include <aarch64/mmu.h>
#include <common/rc.h>
#include <common/spinlock.h>
#include <driver/memlayout.h>
#include <kernel/mem.h>

#include <common/list.h>
// #include <kernel/printk.h>

RefCount kalloc_page_cnt;

static SpinLock mm_lock;
static char* mm_end;   // lowest allocated page address
static ListNode list;  // deleted pages.

void kinit() {
    init_rc(&kalloc_page_cnt);

    init_spinlock(&mm_lock);

    extern char end[];
    mm_end = (char*)((u64)(end - 1) & ~0xFFFull);
    init_list_node(&list);
}

void* kalloc_page() {
    increment_rc(&kalloc_page_cnt);

    acquire_spinlock(&mm_lock);

    void* ret = NULL;
    if (_empty_list(&list)) {
        ret = mm_end += PAGE_SIZE;
    } else {
        ret = list.next;
        _detach_from_list(list.next);
    }

    release_spinlock(&mm_lock);
    return ret;
}

void kfree_page(void* p) {
    decrement_rc(&kalloc_page_cnt);

    acquire_spinlock(&mm_lock);

    /*  if (p == mm_end)
            mm_end -= PAGE_SIZE;
        else  */
    _insert_into_list(&list, p);

    release_spinlock(&mm_lock);
}

typedef struct Node {
    unsigned next;  // lower 32 bits of the address
    short size;
    bool free;
} Node;

#define KADDR(next) (next ? (Node*)KSPACE(next) : 0)

static void merge(Node* h) {
    Node* p = KADDR(h->next);
    if (p && p->free && PAGE_BASE(h) == PAGE_BASE(p)) {
        h->next = (u64)p->next;
        h->size += p->size + sizeof(Node);
    }
    h->free = true;
}

static Node *free8[4] = {0}, *free4[4] = {0};

void* kalloc(unsigned long long size) {
    Node** fr = (size & 0x7) ? free4 : free8;
    size = (size + 3) & ~0x3;

    Node* h = fr[cpuid()];
    for (; h; h = KADDR(h->next))
        if (h->free) {
            merge(h);
            if ((u64)h->size >= size)
                break;
        }

    if (h == NULL) {
        Node* p = (Node*)kalloc_page();
        *p = (Node){(u64)fr[cpuid()], PAGE_SIZE - sizeof(Node), true};
        h = fr[cpuid()] = p;
    }

    short sz = sizeof(Node) + size;
    if (h->size > sz) {
        Node* p = (Node*)((u64)h + sz);
        *p = (Node){(u64)h->next, h->size - sz, true};
        h->next = (u64)p;
    }

    h->size = size;
    h->free = false;
    return (void*)((u64)h + sizeof(Node));
}

void kfree(void* ptr) {
    merge((Node*)((u64)ptr - sizeof(Node)));
}

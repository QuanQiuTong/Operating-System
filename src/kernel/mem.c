#include <aarch64/mmu.h>
#include <common/rc.h>
#include <common/spinlock.h>
#include <driver/memlayout.h>
#include <kernel/mem.h>

#include <common/list.h>
#include <kernel/printk.h>

RefCount kalloc_page_cnt;

static SpinLock mm_lock, memlock;
static char* mm_end;   // lowest untouched page address
static ListNode list;  // deleted pages.

void kinit() {
    init_rc(&kalloc_page_cnt);

    init_spinlock(&mm_lock);
    init_spinlock(&memlock);

    extern char end[];
    mm_end = (char*)((u64)(end + 4095) & ~0xFFFull);
    init_list_node(&list);
}

void* kalloc_page() {
    increment_rc(&kalloc_page_cnt);

    acquire_spinlock(&mm_lock);

    void* ret = NULL;
    if (list.next == list.prev) {
        ret = mm_end;
        mm_end += PAGE_SIZE;
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

    _insert_into_list(&list, p);

    release_spinlock(&mm_lock);
}

typedef struct Node {
    struct Node* next;
    short size;
    bool free;
} Node;

static void merge(Node* h) {
    Node* p = h->next;
    if (p && p->free && PAGE_BASE((u64)h) == PAGE_BASE((u64)p)) {
        h->next = p->next;
        h->size += p->size + sizeof(Node);
    }
    h->free = true;
}

static Node *free8[4] = {0}, *free4[4] = {0};

void* kalloc(unsigned long long size) {
    acquire_spinlock(&memlock);

    Node** fr = (size & 0x7) ? free4 : free8;
    size = (size + 3) & ~0x3;
    
    Node* h = fr[cpuid()];
    for (; h; h = h->next)
        if (h->free) {
            merge(h);
            if ((unsigned long long)h->size >= size)
                break;
        }
    if (h == NULL) {
        Node* p = (Node*)kalloc_page();
        p->next = fr[cpuid()];
        p->size = PAGE_SIZE - sizeof(Node);
        p->free = true;
        fr[cpuid()] = p;
        h = p;
    }
    if (h->size - (u64)size > sizeof(Node)) {
        Node* p = (Node*)((u64)h + sizeof(Node) + size);
        p->free = true;
        p->size = h->size - (u64)size - sizeof(Node);
        p->next = h->next;
        h->next = p;
    }
    h->size = size;
    h->free = false;
    release_spinlock(&memlock);
    return (void*)((u64)h + sizeof(Node));
}

void kfree(void* ptr) {
    acquire_spinlock(&memlock);
    merge((Node*)((u64)ptr - sizeof(Node)));
    release_spinlock(&memlock);
}

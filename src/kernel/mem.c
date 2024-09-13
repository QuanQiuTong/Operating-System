#include <aarch64/mmu.h>
#include <common/rc.h>
#include <common/spinlock.h>
#include <driver/memlayout.h>
#include <kernel/mem.h>

#include <common/list.h>
#include <kernel/printk.h>

RefCount kalloc_page_cnt;

static char* mm_end;    // lowest untouched page address
static ListNode* list;  // deleted pages.

char *head, *tail;

void kinit() {
    init_rc(&kalloc_page_cnt);

    printk("page %d\n", PAGE_SIZE);
    extern char end[];
    printk("end %p\n", end);
    mm_end = (char*)((__PTRDIFF_TYPE__)(end + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE);
    printk("mm_end %p\n", mm_end);
    list = (ListNode*)mm_end;
    init_list_node(list);
    printk("%p %p\n", list->next, list->prev);
}

void* kalloc_page() {
    increment_rc(&kalloc_page_cnt);

    return NULL;
}

void kfree_page(void* p) {
    decrement_rc(&kalloc_page_cnt);

    return;
}

void* kalloc(unsigned long long size) {
    return NULL;
}

void kfree(void* ptr) {
    return;
}

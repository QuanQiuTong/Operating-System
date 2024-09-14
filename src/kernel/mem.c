#include <aarch64/mmu.h>
#include <common/rc.h>
#include <common/spinlock.h>
#include <driver/memlayout.h>
#include <kernel/mem.h>

#include <common/list.h>
#include <kernel/printk.h>

RefCount kalloc_page_cnt;

static SpinLock kalloc_lock;
static char* mm_end;   // lowest untouched page address
static ListNode list;  // deleted pages.

#define ALIGN(p) ((u64)(p) & ~0xFFFull)

void kinit() {
    init_rc(&kalloc_page_cnt);

    init_spinlock(&kalloc_lock);

    extern char end[];
    printk("end %p\n", end);
    mm_end = (char*)ALIGN(end + PAGE_SIZE - 1);
    printk("mm_end %p\n", mm_end);
    init_list_node(&list);
    printk("%p %p\n", list.next, list.prev);
}

void* kalloc_page() {
    increment_rc(&kalloc_page_cnt);

    acquire_spinlock(&kalloc_lock);
    void* ret = NULL;
    if (list.next == list.prev) {
        ret = mm_end;
        mm_end += PAGE_SIZE;
    } else {
        ret = list.next;
        _detach_from_list(list.next);
    }

    /* printk("alloc page %p\n", ret);
    _for_in_list(q, &list) {
        printk(" %p", q);
    }
    printk("\n"); */
    
    release_spinlock(&kalloc_lock);
    return ret;
}

void kfree_page(void* p) {
    decrement_rc(&kalloc_page_cnt);

    // ASSERT((__SIZE_TYPE__)p % PAGE_SIZE == 0);

    acquire_spinlock(&kalloc_lock);

    _insert_into_list(&list, p);

    /* printk("free page %p\n", p);
    _for_in_list(q, &list) {
        printk(" %p", q);
    }
    printk("\n"); */

    release_spinlock(&kalloc_lock);
    return;
}

void* kalloc(unsigned long long size) {
    size = size;
    return kalloc_page();
}

void kfree(void* ptr) {
    // (void*)ALIGN(ptr);
    kfree_page(ptr);
    return;
}

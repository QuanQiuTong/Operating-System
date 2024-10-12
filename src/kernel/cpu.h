#pragma once

#include <common/rbtree.h>
#include <kernel/proc.h>

#define NCPU 4

struct sched {
    Proc* thisproc;
    Proc* idle;
};

struct cpu {
    bool online;
    struct rb_root_ timer;
    struct sched sched;
};

extern struct cpu cpus[NCPU];

struct timer {
    bool triggered;
    int elapse;
    u64 _key;
    struct rb_node_ _node;
    void (*handler)(struct timer *);
    u64 data;
};

void init_clock_handler();

void set_cpu_on();
void set_cpu_off();

void set_cpu_timer(struct timer *timer);
void cancel_cpu_timer(struct timer *timer);
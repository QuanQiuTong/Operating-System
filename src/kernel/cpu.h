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

void set_cpu_on();
void set_cpu_off();

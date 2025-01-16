#include <common/string.h>
#include <fs/pipe.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/sched.h>

static ALWAYS_INLINE void init_pipe(Pipe *pi) {
    init_spinlock(&pi->lock);
    init_sem(&pi->wlock, 0);
    init_sem(&pi->rlock, 0);
    // memset(pi->data, 0, PIPE_SIZE);
    pi->nread = 0;
    pi->nwrite = 0;
    pi->readopen = 1;
    pi->writeopen = 1;
}

static ALWAYS_INLINE void init_read_pipe(File *readp, Pipe *pipe) {
    readp->type = FD_PIPE;
    readp->ref = 1;
    readp->readable = 1;
    readp->writable = 0;
    readp->pipe = pipe;
    readp->off = 0;
    // pipe->readopen = 1;
}

static ALWAYS_INLINE void init_write_pipe(File *writep, Pipe *pipe) {
    writep->type = FD_PIPE;
    writep->ref = 1;
    writep->readable = 0;
    writep->writable = 1;
    writep->pipe = pipe;
    writep->off = 0;
    // pipe->writeopen = 1;
}

int pipe_alloc(File **f0, File **f1) {
    Pipe *p = NULL;
    *f0 = *f1 = 0;
    if ((*f0 = file_alloc()) && (*f1 = file_alloc()) && (p = kalloc(sizeof(Pipe)))) {
        init_pipe(p);

        init_read_pipe(*f0, p);
        init_write_pipe(*f1, p);
        return 0;
    }
    if (p)
        kfree((char *)p);
    if (*f0)
        file_close(*f0);
    if (*f1)
        file_close(*f1);
    return -1;
}

void pipe_close(Pipe *pi, int writable) {
    acquire_spinlock(&pi->lock);
    if (writable) {
        pi->writeopen = 0;
        post_sem(&pi->rlock);
    } else {
        pi->readopen = 0;
        post_sem(&pi->wlock);
    }
    if (pi->readopen == 0 && pi->writeopen == 0) {
        // release_spinlock(&pi->lock); // nobody would use this anymore
        kfree((void *)pi);
    } else {
        release_spinlock(&pi->lock);
    }
}

int pipe_write(Pipe *pi, u64 addr, int n) {
    acquire_spinlock(&pi->lock);
    int i = 0;
    for (; i < n; i++) {
        if (pi->readopen == 0 || thisproc()->killed) {
            release_spinlock(&pi->lock);
            return -1;
        }
        while (pi->nwrite == pi->nread + PIPE_SIZE) {
            post_sem(&pi->rlock);
            release_spinlock(&pi->lock);
            unalertable_wait_sem(&pi->wlock);
        }
        pi->data[pi->nwrite++ % PIPE_SIZE] = *((char *)addr + i);
    }
    post_sem(&pi->rlock);
    release_spinlock(&pi->lock);
    return i;
}

int pipe_read(Pipe *pi, u64 addr, int n) {
    acquire_spinlock(&pi->lock);
    while (pi->nread == pi->nwrite && pi->writeopen) {
        if (thisproc()->killed) {
            release_spinlock(&pi->lock);
            return -1;
        }
        release_spinlock(&pi->lock);
        unalertable_wait_sem(&pi->rlock);
    }
    int i = 0;
    for (; i < n && pi->nread != pi->nwrite; i++) {
        *((char *)addr + i) = pi->data[pi->nread++ % PIPE_SIZE];
    }
    post_sem(&pi->wlock);
    release_spinlock(&pi->lock);
    return i;
}

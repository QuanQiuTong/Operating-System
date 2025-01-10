#include "file.h"
#include <common/defines.h>
#include <common/list.h>
#include <common/sem.h>
#include <common/spinlock.h>
#include <fs/inode.h>
#include <fs/pipe.h>
#include <kernel/mem.h>

// the global file table.
static struct ftable ftable;

void init_ftable() {
    // initialize your ftable.
    init_spinlock(&ftable.lock);
}

void init_oftable(struct oftable *oftable) {
    // initialize your oftable for a new process.
    for (usize i = 0; i < sizeof(oftable->openfile) / sizeof(*oftable->openfile); ++i) {
        oftable->openfile[i] = NULL;
    }
}

/* Allocate a file structure. */
struct file *file_alloc() {
    acquire_spinlock(&ftable.lock);
    for (int i = 0; i < NFILE; i++) {
        if (ftable.filelist[i].ref == 0) {
            ftable.filelist[i].ref = 1;
            release_spinlock(&ftable.lock);
            return &(ftable.filelist[i]);
        }
    }
    release_spinlock(&ftable.lock);
    return NULL;
}

/* Increment ref count for file f. */
struct file *file_dup(struct file *f) {
    acquire_spinlock(&ftable.lock);
    ASSERT(f->ref >= 1);
    f->ref += 1;
    release_spinlock(&ftable.lock);
    return f;
}

/* Close file f. (Decrement ref count, close when reaches 0.) */
void file_close(struct file *f) {
    acquire_spinlock(&ftable.lock);
    ASSERT(f->ref >= 1);
    f->ref--;
    if (f->ref > 0) {
        release_spinlock(&ftable.lock);
        return;
    }
    struct file now = *f;
    f->type = FD_NONE;
    release_spinlock(&ftable.lock);
    if (now.type == FD_PIPE) {
        pipe_close(now.pipe, now.writable);
    } else if (now.type == FD_INODE) {
        OpContext ctx;
        bcache.begin_op(&ctx);
        inodes.put(&ctx, now.ip);
        bcache.end_op(&ctx);
    }
}

/* Get metadata about file f. */
int file_stat(struct file *f, struct stat *st) {
    if (f->type != FD_INODE)
        return -1;

    inodes.lock(f->ip);
    stati(f->ip, st);
    inodes.unlock(f->ip);
    return 0;
}

/* Read from file f. */
isize file_read(struct file *f, char *addr, isize n) {
    if (!f->readable)
        return -1;
    if (f->type == FD_PIPE)
        return pipe_read(f->pipe, (u64)addr, n);
    if (f->type == FD_INODE) {
        inodes.lock(f->ip);
        usize size = inodes.read(f->ip, (u8 *)addr, f->off, n);
        f->off += size;
        inodes.unlock(f->ip);
        return size;
    }
    PANIC();
    return 0;
}

/* Write to file f. */
isize file_write(struct file *f, char *addr, isize n) {
    if (!f->writable)
        return -1;
    if (f->type == FD_PIPE)
        return pipe_write(f->pipe, (u64)addr, n);
    if (f->type == FD_INODE) {
        isize maxbytes = ((OP_MAX_NUM_BLOCKS - 4) / 2) * BLOCK_SIZE;
        // 2 blocks for each write block
        // 1 block for inode
        // 1 block for map
        // 2 blocks for IndirectBlock
        isize idx = 0;
        while (idx < n) {
            isize len = MIN(n - idx, maxbytes);
            OpContext ctx;
            bcache.begin_op(&ctx);
            inodes.lock(f->ip);
            isize reallen = inodes.write(&ctx, f->ip, (u8 *)(addr + idx), f->off, len);
            // ASSERT(reallen==len);
            f->off += reallen;
            inodes.unlock(f->ip);
            bcache.end_op(&ctx);
            ASSERT(reallen == len);
            idx += reallen;
        }
        if (idx == n)
            return n;
        return -1;
    }
    return 0;
}
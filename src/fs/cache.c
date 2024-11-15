#include <common/bitmap.h>
#include <common/string.h>
#include <fs/cache.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/proc.h>

/**
    @brief the private reference to the super block.

    @note we need these two variables because we allow the caller to
            specify the block device and super block to use.
            Correspondingly, you should NEVER use global instance of
            them, e.g. `get_super_block`, `block_device`

    @see init_bcache
 */
static const SuperBlock *sblock;

/**
    @brief the reference to the underlying block device.
 */
static const BlockDevice *device;

/**
    @brief global lock for block cache.

    Use it to protect anything you need.

    e.g. the list of allocated blocks, etc.
 */
static SpinLock lock;

/**
    @brief the list of all allocated in-memory block.

    We use a linked list to manage all allocated cached blocks.

    You can implement your own data structure if you like better performance.

    @see Block
 */
static ListNode head;

static LogHeader header;  // in-memory copy of log header block.

static SpinLock bitmaplock;

static usize blocknum;  // the number of blocks in the block cache.

/**
    @brief a struct to maintain other logging states.

    You may wonder where we store some states, e.g.

    * how many atomic operations are running?
    * are we checkpointing?
    * how to notify `end_op` that a checkpoint is done?

    Put them here!

    @see cache_begin_op, cache_end_op, cache_sync
 */
struct {
    // if on commit, lock.locked == true.
    SpinLock lock;
    unsigned outstanding;
    Semaphore sem;
} log;

// read the content from disk.
static INLINE void device_read(Block *block) {
    device->read(block->block_no, block->data);
}

// write the content back to disk.
static INLINE void device_write(Block *block) {
    device->write(block->block_no, block->data);
}

// read log header from disk.
static INLINE void read_header() {
    device->read(sblock->log_start, (u8 *)&header);
}

// write log header back to disk.
static INLINE void write_header() {
    device->write(sblock->log_start, (u8 *)&header);
}

// initialize a block struct.
static void init_block(Block *block) {
    block->block_no = 0;
    init_list_node(&block->node);
    block->acquired = false;
    block->pinned = false;

    init_sleeplock(&block->lock);
    block->valid = false;
    memset(block->data, 0, sizeof(block->data));
}

// see `cache.h`.
static usize get_num_cached_blocks() {
    return blocknum;
}

#define cond_wait(cond, lock) ( \
    release_spinlock(lock), ASSERT(wait_sem(cond)), acquire_spinlock(lock))

#define for_list(node) for (ListNode *p = node.next; p != &node; p = p->next)

// see `cache.h`.
static Block *cache_acquire(usize block_no) {
    acquire_spinlock(&lock);

    for_list(head) {
        Block *blk = container_of(p, Block, node);
        if (blk->block_no == block_no) {
            blk->acquired = 1;
            cond_wait(&blk->lock, &lock);

            _detach_from_list(&blk->node);
            _merge_list(&head, &blk->node);

            release_spinlock(&lock);
            return blk;
        }
    }

    for (ListNode *p = head.prev;
         blocknum >= EVICTION_THRESHOLD && p != &head;) {
        Block *blk = container_of(p, Block, node);
        if (blk->acquired || blk->pinned) {
            p = p->prev;
            continue;
        }
        p = _detach_from_list(p);
        kfree(blk);
        blocknum--;
    }
    blocknum++;

    Block *b = kalloc(sizeof(Block));
    b->block_no = block_no,
    b->acquired = true,
    b->pinned = false,
    b->valid = true,
    init_sleeplock(&b->lock);
    _insert_into_list(&head, &b->node);

    ASSERT(acquire_sleeplock(&b->lock));
    device_read(b);
    release_spinlock(&lock);
    return b;
}

// see `cache.h`.
static void cache_release(Block *block) {
    acquire_spinlock(&lock);
    block->acquired = 0;
    release_sleeplock(&block->lock);
    release_spinlock(&lock);
}

static void blockcopy(usize src_no, usize dst_no) {
    Block *src = cache_acquire(src_no);
    Block *dest = cache_acquire(dst_no);

    memcpy(dest->data, src->data, BLOCK_SIZE);

    cache_release(src);

    device_write(dest);
    cache_release(dest);
}

// see `cache.h`.
void init_bcache(const SuperBlock *_sblock, const BlockDevice *_device) {
    sblock = _sblock;
    device = _device;

    init_spinlock(&lock);
    init_spinlock(&bitmaplock);
    init_list_node(&head);
    blocknum = 0;
    header.num_blocks = 0;

    init_spinlock(&log.lock);
    log.outstanding = 0;
    init_sem(&log.sem, 0);

    read_header();
    for (usize i = 0; i < header.num_blocks; ++i) {
        blockcopy(sblock->log_start + i + 1, header.block_no[i]);
    }
    header.num_blocks = 0;
    write_header();
}

// see `cache.h`.
static void cache_begin_op(OpContext *ctx) {
    acquire_spinlock(&log.lock);
    ctx->rm = 0;
    while (header.num_blocks + (log.outstanding + 1) * OP_MAX_NUM_BLOCKS > LOG_MAX_SIZE) {
        cond_wait(&log.sem, &log.lock);
    }
    log.outstanding++;
    release_spinlock(&log.lock);
}

// see `cache.h`.
static void cache_sync(OpContext *ctx, Block *block) {
    if (ctx == NULL) {
        device_write(block);
        return;
    }
    acquire_spinlock(&log.lock);
    block->pinned = 1;
    for (usize i = 0; i < header.num_blocks; ++i) {
        if (block->block_no == header.block_no[i]) {
            release_spinlock(&log.lock);
            return;
        }
    }
    ASSERT(ctx->rm < OP_MAX_NUM_BLOCKS);
    ASSERT(header.num_blocks < LOG_MAX_SIZE);
    ctx->rm++;
    header.block_no[header.num_blocks++] = block->block_no;
    release_spinlock(&log.lock);
}

// see `cache.h`.
static void cache_end_op(OpContext *ctx) {
    acquire_spinlock(&log.lock);
    if (--log.outstanding == 0) {
        for (usize i = 0; i < header.num_blocks; ++i) {
            blockcopy(header.block_no[i], sblock->log_start + i + 1);
        }
        write_header();
        for (usize i = 0; i < header.num_blocks; ++i) {
            Block *b = cache_acquire(header.block_no[i]);
            cache_sync(NULL, b);
            b->pinned = false;
            cache_release(b);
        }
        header.num_blocks = 0;
        write_header();
    }
    post_sem(&log.sem);
    release_spinlock(&log.lock);
}

// see `cache.h`.
static usize cache_alloc(OpContext *ctx) {
    acquire_spinlock(&bitmaplock);
    for (usize blockstart = 0; blockstart < sblock->num_blocks; blockstart += BIT_PER_BLOCK) {
        Block *mp = cache_acquire(sblock->bitmap_start + blockstart / BIT_PER_BLOCK);
        for (unsigned i = 0; i < BIT_PER_BLOCK && blockstart + i < sblock->num_blocks; ++i) {
            u8 *t = &mp->data[i / 8];
            int bit = 1 << (i & 7);
            if ((*t & bit) == 0) {
                *t |= bit;
                cache_sync(ctx, mp);
                cache_release(mp);
                Block *ret = cache_acquire(blockstart + i);
                memset(ret->data, 0, BLOCK_SIZE);
                cache_sync(ctx, ret);
                cache_release(ret);
                release_spinlock(&bitmaplock);
                return blockstart + i;
            }
        }
        cache_release(mp);
    }
    release_spinlock(&bitmaplock);
    PANIC();
}

// see `cache.h`.
static void cache_free(OpContext *ctx, usize block_no) {
    acquire_spinlock(&bitmaplock);
    Block *mp = cache_acquire(sblock->bitmap_start + block_no / BIT_PER_BLOCK);
    int idx = block_no % BIT_PER_BLOCK;
    mp->data[idx / 8] &= ~(1 << (idx & 7));
    cache_sync(ctx, mp);
    cache_release(mp);
    release_spinlock(&bitmaplock);
}

BlockCache bcache = {
    .get_num_cached_blocks = get_num_cached_blocks,
    .acquire = cache_acquire,
    .release = cache_release,
    .begin_op = cache_begin_op,
    .sync = cache_sync,
    .end_op = cache_end_op,
    .alloc = cache_alloc,
    .free = cache_free,
};
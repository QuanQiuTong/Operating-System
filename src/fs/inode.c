#include <common/string.h>
#include <fs/inode.h>
#include <kernel/mem.h>
#include <kernel/printk.h>

/**
    @brief the private reference to the super block.

    @note we need these two variables because we allow the caller to
            specify the block cache and super block to use.
            Correspondingly, you should NEVER use global instance of
            them.

    @see init_inodes
 */
static const SuperBlock* sblock;

/**
    @brief the reference to the underlying block cache.
 */
static const BlockCache* cache;

/**
    @brief global lock for inode layer.

    Use it to protect anything you need.

    e.g. the list of allocated blocks, ref counts, etc.
 */
static SpinLock lock;

/**
    @brief the list of all allocated in-memory inodes.

    We use a linked list to manage all allocated inodes.

    You can implement your own data structure if you want better performance.

    @see Inode
 */
static ListNode head;

// return which block `inode_no` lives on.
static INLINE usize to_block_no(usize inode_no) {
    return sblock->inode_start + (inode_no / (INODE_PER_BLOCK));
}

// return the pointer to on-disk inode.
static INLINE InodeEntry* get_entry(Block* block, usize inode_no) {
    return ((InodeEntry*)block->data) + (inode_no % INODE_PER_BLOCK);
}

// return address array in indirect block.
static INLINE u32* get_addrs(Block* block) {
    return ((IndirectBlock*)block->data)->addrs;
}

// initialize inode tree.
void init_inodes(const SuperBlock* _sblock, const BlockCache* _cache) {
    init_spinlock(&lock);
    init_list_node(&head);
    sblock = _sblock;
    cache = _cache;

    if (ROOT_INODE_NO < sblock->num_inodes)
        inodes.root = inodes.get(ROOT_INODE_NO);
    else
        printk("(warn) init_inodes: no root inode.\n");
}

// initialize in-memory inode.
static void init_inode(Inode* inode) {
    init_sleeplock(&inode->lock);
    init_rc(&inode->rc);
    init_list_node(&inode->node);
    inode->inode_no = 0;
    inode->valid = false;
}

// see `inode.h`.
static usize inode_alloc(OpContext* ctx, InodeType type) {
    ASSERT(type != INODE_INVALID);

    for (usize ino = 1; ino < sblock->num_inodes; ++ino) {
        Block* blk = cache->acquire(to_block_no(ino));
        InodeEntry* entry = get_entry(blk, ino);
        if (entry->type == INODE_INVALID) {
            *entry = (InodeEntry){
                .type = type,
                .num_links = 0,
                .num_bytes = 0,
            };
            cache->sync(ctx, blk);
            cache->release(blk);
            return ino;
        }
        cache->release(blk);
    }
    printk("(panic) inode_alloc: no more free inode.\n");
    PANIC();
    return 0;
}

// see `inode.h`.
static void inode_lock(Inode* inode) {
    ASSERT(inode->rc.count > 0);
    if (!acquire_sleeplock(&inode->lock))
        PANIC();
}

// see `inode.h`.
static void inode_unlock(Inode* inode) {
    ASSERT(inode->rc.count > 0);
    release_sleeplock(&inode->lock);
}

// see `inode.h`.
static void inode_sync(OpContext* ctx, Inode* inode, bool do_write) {
    if (!inode->valid) {
        Block* blk = cache->acquire(to_block_no(inode->inode_no));
        inode->entry = *get_entry(blk, inode->inode_no);
        inode->valid = true;
        cache->release(blk);
    } else if (do_write) {
        Block* blk = cache->acquire(to_block_no(inode->inode_no));
        *get_entry(blk, inode->inode_no) = inode->entry;
        cache->sync(ctx, blk);
        cache->release(blk);
    }
}

#define for_list(head) for (ListNode* p = head.next; p != &head; p = p->next)

// see `inode.h`.
static Inode* inode_get(usize inode_no) {
    ASSERT(inode_no > 0);
    ASSERT(inode_no < sblock->num_inodes);
    acquire_spinlock(&lock);

    Inode* inode = NULL;
    for_list(head) if ((inode = container_of(p, Inode, node))->inode_no == inode_no) goto found;

    inode = kalloc(sizeof(Inode));
    init_inode(inode);
    inode->inode_no = inode_no;
    _merge_list(&head, &inode->node);

found:

    increment_rc(&inode->rc);  // actually atomic, lock for fun.
    release_spinlock(&lock);

    inode_lock(inode);
    inode_sync(NULL, inode, false);
    inode_unlock(inode);

    return inode;
}
// see `inode.h`.
static void inode_clear(OpContext* ctx, Inode* inode) {
    if (inode->entry.indirect != 0) {
        Block* inblock = cache->acquire(inode->entry.indirect);
        u32* addrs = get_addrs(inblock);
        for (u32 i = 0; i < INODE_NUM_INDIRECT; ++i) {
            if (addrs[i])
                cache->free(ctx, addrs[i]);
        }
        cache->release(inblock);
        cache->free(ctx, inode->entry.indirect);
        inode->entry.indirect = 0;
    }
    for (u32 i = 0; i < INODE_NUM_DIRECT; ++i) {
        if (inode->entry.addrs[i]) {
            cache->free(ctx, inode->entry.addrs[i]);
            inode->entry.addrs[i] = 0;
        }
    }
    inode->entry.num_bytes = 0;
    inode_sync(ctx, inode, true);
}

// see `inode.h`.
static Inode* inode_share(Inode* inode) {
    increment_rc(&inode->rc);
    return inode;
}

// see `inode.h`.
static void inode_put(OpContext* ctx, Inode* inode) {
    unalertable_acquire_sleeplock(&inode->lock);

    decrement_rc(&inode->rc);
    if (inode->rc.count || inode->entry.num_links) {
        release_sleeplock(&inode->lock);
        return;
    }

    inode->entry.type = INODE_INVALID;
    inode_clear(ctx, inode);
    inode_sync(ctx, inode, true);

    detach_from_list(&lock, &inode->node);

    release_sleeplock(&inode->lock);
    kfree(inode);
}

/**
    @brief get which block is the offset of the inode in.

    e.g. `inode_map(ctx, my_inode, 1234, &modified)` will return the block_no
    of the block that contains the 1234th byte of the file
    represented by `my_inode`.

    If a block has not been allocated for that byte, `inode_map` will
    allocate a new block and update `my_inode`, at which time, `modified`
    will be set to true.

    HOWEVER, if `ctx == NULL`, `inode_map` will NOT try to allocate any new block,
    and when it finds that the block has not been allocated, it will return 0.

    @param[out] modified true if some new block is allocated and `inode`
    has been changed.

    @return usize the block number of that block, or 0 if `ctx == NULL` and
    the required block has not been allocated.

    @note the caller must hold the lock of `inode`.
 */
static usize inode_map(OpContext* ctx,
                       Inode* inode,
                       usize offset,
                       bool* modified) {
    if (offset < INODE_NUM_DIRECT) {
        if (inode->entry.addrs[offset] == 0) {
            *modified = true;
            inode->entry.addrs[offset] = cache->alloc(ctx);
            inode_sync(ctx, inode, true);  // attention
        }
        return inode->entry.addrs[offset];
    }

    if (inode->entry.indirect == 0) {
        inode->entry.indirect = cache->alloc(ctx);
        inode_sync(ctx, inode, true);
    }

    Block* inblock = cache->acquire(inode->entry.indirect);
    u32* addr = &get_addrs(inblock)[offset - INODE_NUM_DIRECT];
    if (*addr == 0) {
        *modified = true;
        *addr = cache->alloc(ctx);
        cache->sync(ctx, inblock);
    }
    usize bno = *addr;
    cache->release(inblock);
    return bno;
}

// see `inode.h`.
static usize inode_read(Inode* inode, u8* dest, usize offset, usize count) {
    InodeEntry* entry = &inode->entry;
    if (count + offset > entry->num_bytes)
        count = entry->num_bytes - offset;
    usize end = offset + count;
    ASSERT(offset <= entry->num_bytes);
    ASSERT(end <= entry->num_bytes);
    ASSERT(offset <= end);

    for (usize i = offset; i < end; i = (i / BLOCK_SIZE + 1) * BLOCK_SIZE) {
        usize len = MIN(BLOCK_SIZE - i % BLOCK_SIZE, end - i);
        usize bno = inode_map(NULL, inode, i / BLOCK_SIZE, &(bool){0});
        Block* blk = cache->acquire(bno);
        memcpy(dest, blk->data + i % BLOCK_SIZE, len);
        cache->release(blk);
        dest += len;
    }
    return count;
}

// see `inode.h`.
static usize inode_write(OpContext* ctx,
                         Inode* inode,
                         u8* src,
                         usize offset,
                         usize count) {
    InodeEntry* entry = &inode->entry;
    usize end = offset + count;
    ASSERT(offset <= entry->num_bytes);
    ASSERT(end <= INODE_MAX_BYTES);
    ASSERT(offset <= end);

    if (entry->num_bytes < end) {
        entry->num_bytes = end;
        inode_sync(ctx, inode, true);
    }

    while (offset < end) {
        usize len = MIN(BLOCK_SIZE - offset % BLOCK_SIZE, end - offset);

        Block* blk = cache->acquire(inode_map(ctx, inode, offset / BLOCK_SIZE, &(bool){0}));
        memcpy(blk->data + offset % BLOCK_SIZE, src, len);
        cache->sync(ctx, blk);
        cache->release(blk);

        src += len;
        offset += len;
    }

    return count;
}

// see `inode.h`.
static usize inode_lookup(Inode* inode, const char* name, usize* index) {
    InodeEntry* entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);

    for (usize i = 0; i < entry->num_bytes; i += sizeof(DirEntry)) {
        DirEntry dir;
        inode_read(inode, (u8*)&dir, i, sizeof(DirEntry));
        if (dir.inode_no && !strncmp(name, dir.name, FILE_NAME_MAX_LENGTH)) {
            if (index)
                *index = i;
            return dir.inode_no;
        }
    }
    return 0;
}

// see `inode.h`.
static usize inode_insert(OpContext* ctx,
                          Inode* inode,
                          const char* name,
                          usize inode_no) {
    InodeEntry* entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);

    if (inode_lookup(inode, name, &(usize){0}))
        return -1;

    DirEntry dir;
    dir.inode_no = inode_no;
    strncpy(dir.name, name, FILE_NAME_MAX_LENGTH);

    usize index = inode->entry.num_bytes;
    inode_write(ctx, inode, (u8*)&dir, index, sizeof(DirEntry));
    return index;
}

// see `inode.h`.
static void inode_remove(OpContext* ctx, Inode* inode, usize index) {
    DirEntry dir;
    inode_read(inode, (u8*)&dir, index, sizeof(DirEntry));
    dir.inode_no = 0;
    inode_write(ctx, inode, (u8*)&dir, index, sizeof(DirEntry));
}

InodeTree inodes = {
    .alloc = inode_alloc,
    .lock = inode_lock,
    .unlock = inode_unlock,
    .sync = inode_sync,
    .get = inode_get,
    .clear = inode_clear,
    .share = inode_share,
    .put = inode_put,
    .read = inode_read,
    .write = inode_write,
    .lookup = inode_lookup,
    .insert = inode_insert,
    .remove = inode_remove,
};
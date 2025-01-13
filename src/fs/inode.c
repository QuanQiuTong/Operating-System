#include <common/string.h>
#include <fs/inode.h>
#include <kernel/console.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <sys/stat.h>

/**
    @brief the private reference to the super block.

    @note we need these two variables because we allow the caller to
            specify the block cache and super block to use.
            Correspondingly, you should NEVER use global instance of
            them.

    @see init_inodes
 */
static const SuperBlock *sblock;

/**
    @brief the reference to the underlying block cache.
 */
static const BlockCache *cache;

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
static INLINE InodeEntry *get_entry(Block *block, usize inode_no) {
    return ((InodeEntry *)block->data) + (inode_no % INODE_PER_BLOCK);
}

// return address array in indirect block.
static INLINE u32 *get_addrs(Block *block) {
    return ((IndirectBlock *)block->data)->addrs;
}

// initialize inode tree.
void init_inodes(const SuperBlock *_sblock, const BlockCache *_cache) {
    init_spinlock(&lock);
    init_list_node(&head);
    sblock = _sblock;
    cache = _cache;

    if (ROOT_INODE_NO < sblock->num_inodes) {
        inodes.root = inodes.get(ROOT_INODE_NO);
        if (inodes.root->entry.type != INODE_DIRECTORY)
            printk("(panic) init_inodes: root inode is not a directory.\n");
    } else
        printk("(warn) init_inodes: no root inode.\n");
}

// initialize in-memory inode.
static void init_inode(Inode *inode) {
    init_sleeplock(&inode->lock);
    init_rc(&inode->rc);
    init_list_node(&inode->node);
    inode->inode_no = 0;
    inode->valid = false;
}

// see `inode.h`.
static usize inode_alloc(OpContext *ctx, InodeType type) {
    ASSERT(type != INODE_INVALID);

    usize bno = 0;
    Block *blk = cache->acquire(bno);
    for (usize ino = 1; ino < sblock->num_inodes; ++ino) {
        if (to_block_no(ino) != bno) {  // avoid frequent block acquire.
            cache->release(blk);
            blk = cache->acquire(bno = to_block_no(ino));
        }
        if (get_entry(blk, ino)->type == INODE_INVALID) {
            *get_entry(blk, ino) = (InodeEntry){.type = type};  // zero-initialized.
            cache->sync(ctx, blk);
            cache->release(blk);
            return ino;
        }
    }
    cache->release(blk);

    printk("(panic) inode_alloc: no more free inode.\n");
    PANIC();
    return 0;
}

// see `inode.h`.
static void inode_lock(Inode *inode) {
    ASSERT(inode->rc.count > 0);
    if (!acquire_sleeplock(&inode->lock))
        PANIC();
}

// see `inode.h`.
static void inode_unlock(Inode *inode) {
    ASSERT(inode->rc.count > 0);
    release_sleeplock(&inode->lock);
}

// see `inode.h`.
static void inode_sync(OpContext *ctx, Inode *inode, bool do_write) {
    if (!inode->valid) {
        Block *blk = cache->acquire(to_block_no(inode->inode_no));
        inode->entry = *get_entry(blk, inode->inode_no);
        inode->valid = true;
        cache->release(blk);
    } else if (do_write) {
        Block *blk = cache->acquire(to_block_no(inode->inode_no));
        *get_entry(blk, inode->inode_no) = inode->entry;
        cache->sync(ctx, blk);
        cache->release(blk);
    }
}

#define for_list(head) for (ListNode *p = head.next; p != &head; p = p->next)

// see `inode.h`.
static Inode *inode_get(usize inode_no) {
    ASSERT(inode_no > 0);
    ASSERT(inode_no < sblock->num_inodes);
    acquire_spinlock(&lock);

    Inode *inode = NULL;
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

    if (inode == NULL) {
        printk("inode_get: inode %llu is NULL\n", inode_no);
        printk("This should not happen\n");
        PANIC();
    }

    return inode;
}

// see `inode.h`.
static void inode_clear(OpContext *ctx, Inode *inode) {
    if (inode->entry.indirect != 0) {
        Block *inblock = cache->acquire(inode->entry.indirect);
        u32 *addrs = get_addrs(inblock);
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
static Inode *inode_share(Inode *inode) {
    increment_rc(&inode->rc);
    return inode;
}

// see `inode.h`.
static void inode_put(OpContext *ctx, Inode *inode) {
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

    release_sleeplock(&inode->lock);  // not necessary, since immediately freed.
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
static usize inode_map(OpContext *ctx,
                       Inode *inode,
                       usize offset,
                       bool *modified) {
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

    Block *inblock = cache->acquire(inode->entry.indirect);
    u32 *addr = &get_addrs(inblock)[offset - INODE_NUM_DIRECT];
    if (*addr == 0) {
        *modified = true;
        *addr = cache->alloc(ctx);
        cache->sync(ctx, inblock);
    }
    usize bno = *addr;
    cache->release(inblock);
    return bno;
}

static ALWAYS_INLINE void rw(OpContext *ctx, Inode *inode, u8 *buf, usize *off, usize end, bool WRITE) {
    while (*off < end) {
        usize len = MIN(BLOCK_SIZE - *off % BLOCK_SIZE, end - *off);
        usize bno = inode_map(ctx, inode, *off / BLOCK_SIZE, &(bool){0});
        Block *blk = cache->acquire(bno);
        if (WRITE) {
            memcpy(blk->data + *off % BLOCK_SIZE, buf, len);
            cache->sync(ctx, blk);
        } else
            memcpy(buf, blk->data + *off % BLOCK_SIZE, len);
        cache->release(blk);
        buf += len;
        *off += len;
    }
}

// see `inode.h`.
static usize inode_read(Inode *inode, u8 *dest, usize offset, usize count) {
    InodeEntry *entry = &inode->entry;
    if (entry->type == INODE_DEVICE) {
        // ASSERT(entry->major == 1);
        return console_read(inode, (char *)dest, count);
    }
    if (count + offset > entry->num_bytes)
        count = entry->num_bytes - offset;
    usize end = offset + count;
    ASSERT(offset <= entry->num_bytes);
    ASSERT(end <= entry->num_bytes);
    ASSERT(offset <= end);

    rw(NULL, inode, dest, &offset, end, false);
    return count;
}

// see `inode.h`.
static usize inode_write(OpContext *ctx,
                         Inode *inode,
                         u8 *src,
                         usize offset,
                         usize count) {
    InodeEntry *entry = &inode->entry;
    if (entry->type == INODE_DEVICE) {
        // ASSERT(entry->major == 1);
        return console_write(inode, (char *)src, count);
    }
    usize end = offset + count;
    ASSERT(offset <= entry->num_bytes);
    ASSERT(end <= INODE_MAX_BYTES);
    ASSERT(offset <= end);

    if (entry->num_bytes < end) {
        entry->num_bytes = end;
        inode_sync(ctx, inode, true);
    }

    rw(ctx, inode, src, &offset, end, true);
    return count;
}
// #undef rw

// see `inode.h`.
static usize inode_lookup(Inode *inode, const char *name, usize *index) {
    InodeEntry *entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);

    for (usize i = 0; i < entry->num_bytes; i += sizeof(DirEntry)) {
        DirEntry dir;
        inode_read(inode, (u8 *)&dir, i, sizeof(DirEntry));
        if (dir.inode_no && !strncmp(name, dir.name, FILE_NAME_MAX_LENGTH)) {
            if (index)
                *index = i;
            return dir.inode_no;
        }
    }
    return 0;
}

// see `inode.h`.
static usize inode_insert(OpContext *ctx,
                          Inode *inode,
                          const char *name,
                          usize inode_no) {
    InodeEntry *entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);

    if (inode_lookup(inode, name, &(usize){0}))
        return -1;

    DirEntry dir;
    dir.inode_no = inode_no;
    strncpy(dir.name, name, FILE_NAME_MAX_LENGTH);

    usize index = inode->entry.num_bytes;
    inode_write(ctx, inode, (u8 *)&dir, index, sizeof(DirEntry));
    return index;
}

// see `inode.h`.
static void inode_remove(OpContext *ctx, Inode *inode, usize index) {
    DirEntry dir;
    inode_read(inode, (u8 *)&dir, index, sizeof(DirEntry));
    dir.inode_no = 0;
    inode_write(ctx, inode, (u8 *)&dir, index, sizeof(DirEntry));
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

/**
    @brief read the next path element from `path` into `name`.

    @param[out] name next path element.

    @return const char* a pointer offseted in `path`, without leading `/`. If no
    name to remove, return NULL.

    @example
    skipelem("a/bb/c", name) = "bb/c", setting name = "a",
    skipelem("///a//bb", name) = "bb", setting name = "a",
    skipelem("a", name) = "", setting name = "a",
    skipelem("", name) = skipelem("////", name) = NULL, not setting name.
 */
static const char *skipelem(const char *path, char *name) {
    const char *s;
    int len;

    while (*path == '/')
        path++;
    if (*path == 0)
        return 0;
    s = path;
    while (*path != '/' && *path != 0)
        path++;
    len = path - s;
    if (len >= FILE_NAME_MAX_LENGTH)
        memmove(name, s, FILE_NAME_MAX_LENGTH);
    else {
        memmove(name, s, len);
        name[len] = 0;
    }
    while (*path == '/')
        path++;
    return path;
}

/**
    @brief look up and return the inode for `path`.

    If `nameiparent`, return the inode for the parent and copy the final
    path element into `name`.

    @param path a relative or absolute path. If `path` is relative, it is
    relative to the current working directory of the process.

    @param[out] name the final path element if `nameiparent` is true.

    @return Inode* the inode for `path` (or its parent if `nameiparent` is true),
    or NULL if such inode does not exist.

    @example
    namex("/a/b", false, name) = inode of b,
    namex("/a/b", true, name) = inode of a, setting name = "b",
    namex("/", true, name) = NULL (because "/" has no parent!)
 */
static Inode *namex(const char *path,
                    bool nameiparent,
                    char *name,
                    OpContext *ctx) {
    Inode *ip;
    if (*path == '/') {
        ip = inodes.root;
    } else {
        ip = inode_share(thisproc()->cwd);
    }
    while ((path = skipelem(path, name)) != 0) {
        inode_lock(ip);
        if (ip->entry.type != INODE_DIRECTORY) {
            inode_unlock(ip);
            inode_put(ctx, ip);
            return NULL;
        }

        if (nameiparent && *path == '\0') {
            inode_unlock(ip);
            return ip;
        }
        usize ino = inode_lookup(ip, name, 0);
        if (ino == 0) {
            inode_unlock(ip);
            inode_put(ctx, ip);
            return NULL;
        }
        inode_unlock(ip);
        inode_put(ctx, ip);
        ip = inode_get(ino);
    }

    if (nameiparent) {
        inode_put(ctx, ip);
        return NULL;
    }
    return ip;
}

Inode *namei(const char *path, OpContext *ctx) {
    char name[FILE_NAME_MAX_LENGTH];
    return namex(path, false, name, ctx);
}

Inode *nameiparent(const char *path, char *name, OpContext *ctx) {
    return namex(path, true, name, ctx);
}

/**
    @brief get the stat information of `ip` into `st`.

    @note the caller must hold the lock of `ip`.
 */
void stati(Inode *ip, struct stat *st) {
    st->st_dev = 1;
    st->st_ino = ip->inode_no;
    st->st_nlink = ip->entry.num_links;
    st->st_size = ip->entry.num_bytes;
    switch (ip->entry.type) {
    case INODE_REGULAR:
        st->st_mode = S_IFREG;
        break;
    case INODE_DIRECTORY:
        st->st_mode = S_IFDIR;
        break;
    case INODE_DEVICE:
        st->st_mode = 0;
        break;
    default:
        PANIC();
    }
}
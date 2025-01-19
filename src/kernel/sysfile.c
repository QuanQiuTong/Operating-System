//
// File-system system calls implementation.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include <fcntl.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>

#include <aarch64/mmu.h>
#include <common/defines.h>
#include <common/spinlock.h>
#include <common/string.h>
#include <fs/file.h>
#include <fs/fs.h>
#include <fs/inode.h>
#include <fs/pipe.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include "syscall.h"

#ifdef DEBUG
#define printk(fmt, ...) printk(fmt, ##__VA_ARGS__)
#else
#define printk(fmt, ...)
#endif

struct iovec {
    void *iov_base; /* Starting address. */
    usize iov_len;  /* Number of bytes to transfer. */
};

/**
 * Get the file object by fd. Return null if the fd is invalid.
 */
static struct file *fd2file(int fd) {
    if ((unsigned)fd >= (unsigned)NOFILE)
        return NULL;
    return thisproc()->oftable.openfile[fd];
}

/*
 * Allocate a file descriptor for the given file.
 * Takes over file reference from caller on success.
 */
int fdalloc(struct file *f) {
    File **openfile = thisproc()->oftable.openfile;

    for (int fd = 0; fd < NOFILE; fd++) {
        if (openfile[fd] == NULL) {
            openfile[fd] = f;
            return fd;
        }
    }
    printk("fdalloc: no free file descriptor\n");
    return -1;
}

define_syscall(ioctl, int fd, u64 request) {
    // 0x5413 is TIOCGWINSZ (I/O Control to Get the WINdow SIZe, a magic request
    // to get the stdin terminal size) in our implementation. Just ignore it.
    ASSERT(request == 0x5413);
    (void)fd;
    return 0;
}

define_syscall(mmap, void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    printk(
        "\e[0;33m"
        "sys_mmap: addr %p, length %lld, prot %d, flags %d, fd %d, offset %lld\n"
        "\e[0m",
        addr, (long long)length, prot, flags, fd, (long long)offset);

    if (length <= 0 || (prot & PROT_EXEC) || (flags & MAP_ANONYMOUS)) {
        printk("sys_mmap: length, prot, flags unimplemented\n");
        return -1;
    }

    struct file *f = fd2file(fd);
    if (!f) {
        printk("sys_mmap: invalid file descriptor\n");
        return -1;
    }

    // 只有 MAP_SHARED 且需要写权限时才检查文件的写权限
    if ((flags & MAP_SHARED) && (prot & PROT_WRITE) && !(f->writable)) {
        printk("sys_mmap: cannot write to read-only file mapping\n");
        return -1;
    }

    Inode *ip = f->ip;
    if (!ip) {
        printk("sys_mmap: ip is NULL\n");
        return -1;
    }

    usize size = round_up(length, PAGE_SIZE);
    if (size == 0) {
        printk("sys_mmap: size is 0\n");
        return -1;
    }

    OpContext ctx;
    bcache.begin_op(&ctx);
    inodes.lock(ip);
    if (ip->entry.type != INODE_REGULAR) {
        inodes.unlock(ip);
        bcache.end_op(&ctx);
        return -1;
    }

    struct section *sec = kalloc(sizeof(struct section));
    init_list_node(&sec->stnode);
    sec->flags = ST_FILE;
    sec->mmap_flags = flags;

    static usize next_addr = 0x100000;  // 从 1MB 开始分配
    if (addr == NULL) {
        sec->begin = next_addr;
        next_addr += size;
    }else{
        sec->begin = (usize)addr;
    }
    sec->end = sec->begin + size;

    sec->fp = f;
    file_dup(f);
    sec->offset = offset;
    sec->length = size;
    _insert_into_list(&thisproc()->pgdir.section_head, &sec->stnode);

    inodes.unlock(ip);
    bcache.end_op(&ctx);

    printk("    mmap: return %p\n", (void *)sec->begin);
    return sec->begin;
}

#define LOG(fmt, ...) printk("\e[0;32m[%s] " fmt "\e[0m\n", __func__, ##__VA_ARGS__)

#define for_list(node) for (ListNode *h = &node, *p = h->next; p != h; p = p->next)

define_syscall(munmap, u64 addr, size_t length) {
    LOG("addr %llx, length %llx\n", addr, (long long)length);

    if (length == 0)
        return 0;

    u64 aligned_addr = round_down(addr, PAGE_SIZE);
    u64 aligned_len = round_up(length, PAGE_SIZE);

    struct section *sec = NULL;
    for_list(thisproc()->pgdir.section_head) {
        sec = container_of(p, struct section, stnode);
        if (in_section(sec, addr)) {
            break;
        }
    }

    if (!sec || !in_section(sec, addr)) {
        LOG("Invalid memory access at %llx\n", addr);
        return -1;
    }

    if (sec->mmap_flags & MAP_SHARED) {
        for (u64 va = aligned_addr; va < aligned_addr + aligned_len; va += PAGE_SIZE) {
            PTEntry *pte = get_pte(&thisproc()->pgdir, va, false);
            if (pte && *pte) {
                void *pa = (void *)P2K(PTE_ADDRESS(*pte));
                usize offset = sec->offset + (va - sec->begin);
                inodes.write(NULL, sec->fp->ip, pa, offset, PAGE_SIZE);
            }
        }
    }

    for (u64 va = aligned_addr; va < aligned_addr + aligned_len; va += PAGE_SIZE) {
        PTEntry *pte = get_pte(&thisproc()->pgdir, va, false);
        if (pte && *pte) {
            kfree_page((void *)P2K(PTE_ADDRESS(*pte)));
            *pte = 0;
        }
    }

    if (aligned_addr == sec->begin && aligned_addr + aligned_len >= sec->end) {
        _detach_from_list(&sec->stnode);
        if (sec->fp) {
            file_close(sec->fp);
        }
        kfree(sec);
    }

    return 0;
}

define_syscall(dup, int fd) {
    struct file *f = fd2file(fd);
    if (!f)
        return -1;
    fd = fdalloc(f);
    if (fd < 0)
        return -1;
    file_dup(f);
    return fd;
}

define_syscall(read, int fd, char *buffer, int size) {
    struct file *f = fd2file(fd);
    if (!f || size <= 0 || !user_writeable(buffer, size))
        return -1;
    return file_read(f, buffer, size);
}

define_syscall(write, int fd, char *buffer, int size) {
    struct file *f = fd2file(fd);
    if (!f || size <= 0 || !user_readable(buffer, size))
        return -1;
    return file_write(f, buffer, size);
}

define_syscall(writev, int fd, struct iovec *iov, int iovcnt) {
    struct file *f = fd2file(fd);
    struct iovec *p;
    if (!f || iovcnt <= 0 || !user_readable(iov, sizeof(struct iovec) * iovcnt))
        return -1;
    usize tot = 0;
    for (p = iov; p < iov + iovcnt; p++) {
        if (!user_readable(p->iov_base, p->iov_len))
            return -1;
        tot += file_write(f, p->iov_base, p->iov_len);
    }
    return tot;
}

define_syscall(close, int fd) {
    struct file *f = fd2file(fd);
    if (!f)
        return -1;
    thisproc()->oftable.openfile[fd] = NULL;
    file_close(f);
    return 0;
}

define_syscall(fstat, int fd, struct stat *st) {
    struct file *f = fd2file(fd);
    if (!f || !user_writeable(st, sizeof(*st)))
        return -1;
    return file_stat(f, st);
}

define_syscall(newfstatat, int dirfd, const char *path, struct stat *st, int flags) {
    if (!user_strlen(path, 256) || !user_writeable(st, sizeof(*st)))
        return -1;
    if (dirfd != AT_FDCWD) {
        printk("sys_fstatat: dirfd unimplemented\n");
        return -1;
    }
    if (flags != 0) {
        printk("sys_fstatat: flags unimplemented\n");
        return -1;
    }

    Inode *ip;
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((ip = namei(path, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.lock(ip);
    stati(ip, st);
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);

    return 0;
}

static int isdirempty(Inode *dp) {
    usize off;
    DirEntry de;

    for (off = 2 * sizeof(de); off < dp->entry.num_bytes; off += sizeof(de)) {
        if (inodes.read(dp, (u8 *)&de, off, sizeof(de)) != sizeof(de))
            PANIC();
        if (de.inode_no != 0)
            return 0;
    }
    return 1;
}

define_syscall(unlinkat, int fd, const char *path, int flag) {
    ASSERT(fd == AT_FDCWD && flag == 0);
    Inode *ip, *dp;
    DirEntry de;
    char name[FILE_NAME_MAX_LENGTH];
    usize off;
    if (!user_strlen(path, 256))
        return -1;
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((dp = nameiparent(path, name, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }

    inodes.lock(dp);

    // Cannot unlink "." or "..".
    if (strncmp(name, ".", FILE_NAME_MAX_LENGTH) == 0 ||
        strncmp(name, "..", FILE_NAME_MAX_LENGTH) == 0)
        goto bad;

    usize inumber = inodes.lookup(dp, name, &off);
    if (inumber == 0)
        goto bad;
    ip = inodes.get(inumber);
    inodes.lock(ip);

    if (ip->entry.num_links < 1)
        PANIC();
    if (ip->entry.type == INODE_DIRECTORY && !isdirempty(ip)) {
        inodes.unlock(ip);
        inodes.put(&ctx, ip);
        goto bad;
    }

    memset(&de, 0, sizeof(de));
    if (inodes.write(&ctx, dp, (u8 *)&de, off, sizeof(de)) != sizeof(de))
        PANIC();
    if (ip->entry.type == INODE_DIRECTORY) {
        dp->entry.num_links--;
        inodes.sync(&ctx, dp, true);
    }
    inodes.unlock(dp);
    inodes.put(&ctx, dp);
    ip->entry.num_links--;
    inodes.sync(&ctx, ip, true);
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return 0;

bad:
    inodes.unlock(dp);
    inodes.put(&ctx, dp);
    bcache.end_op(&ctx);
    return -1;
}

/**
    @brief create an inode at `path` with `type`.

    If the inode exists, just return it.

    If `type` is directory, you should also create "." and ".." entries and link
   them with the new inode.

    @note BE careful of handling error! You should clean up ALL the resources
   you allocated and free ALL acquired locks when error occurs. e.g. if you
   allocate a new inode "/my/dir", but failed to create ".", you should free the
   inode "/my/dir" before return.

    @see `nameiparent` will find the parent directory of `path`.

    @return Inode* the created inode, or NULL if failed.
 */
Inode *create(const char *path, short type, short major, short minor, OpContext *ctx) {
    char name[FILE_NAME_MAX_LENGTH];
    Inode *dir = nameiparent(path, name, ctx);
    if (dir == NULL)
        return NULL;

    inodes.lock(dir);

    usize ino = inodes.lookup(dir, name, 0);
    if (ino != 0) {
        Inode *ip = inodes.get(ino);
        inodes.unlock(dir);
        inodes.put(ctx, dir);
        inodes.lock(ip);
        if (type == INODE_REGULAR && ip->entry.type == INODE_REGULAR)
            return ip;
        inodes.unlock(ip);
        inodes.put(ctx, ip);
        return NULL;
    }

    Inode *ip = inodes.get(inodes.alloc(ctx, type));
    ASSERT(ip != NULL);
    inodes.lock(ip);
    // bcache.end_op(ctx);
    ip->entry.major = major;
    ip->entry.minor = minor;
    ip->entry.num_links = 1;
    inodes.sync(ctx, ip, true);  // equals to iupdate
    if (type == INODE_DIRECTORY) {
        dir->entry.num_links++;
        inodes.sync(ctx, dir, true);
        inodes.insert(ctx, ip, ".", ip->inode_no);
        inodes.insert(ctx, ip, "..", dir->inode_no);
    }
    inodes.insert(ctx, dir, name, ip->inode_no);
    inodes.unlock(dir);
    inodes.put(ctx, dir);
    return ip;
}

define_syscall(openat, int dirfd, const char *path, int omode) {
    printk("sys_openat: path '%s', omode %d\n", path, omode);

    int fd;
    struct file *f;
    Inode *ip;

    if (!user_strlen(path, 256))
        return -1;

    if (dirfd != AT_FDCWD) {
        printk("sys_openat: dirfd unimplemented\n");
        return -1;
    }

    OpContext ctx;
    bcache.begin_op(&ctx);
    if (omode & O_CREAT) {
        // FIXME: Support acl mode.
        ip = create(path, INODE_REGULAR, 0, 0, &ctx);
        if (ip == 0) {
            bcache.end_op(&ctx);
            printk("sys_openat: create failed\n");
            return -1;
        }
    } else {
        if ((ip = namei(path, &ctx)) == 0) {
            bcache.end_op(&ctx);
            printk("sys_openat: file not found\n");
            return -1;
        }
        inodes.lock(ip);
    }

    if ((f = file_alloc()) == 0 || (fd = fdalloc(f)) < 0) {
        if (f)
            file_close(f);
        inodes.unlock(ip);
        inodes.put(&ctx, ip);
        bcache.end_op(&ctx);
        printk("sys_openat: fdalloc failed\n");
        return -1;
    }
    inodes.unlock(ip);
    bcache.end_op(&ctx);

    f->type = FD_INODE;
    f->ip = ip;
    f->off = 0;
    f->readable = !(omode & O_WRONLY);
    f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
    return fd;
}

define_syscall(mkdirat, int dirfd, const char *path, int mode) {
    Inode *ip;
    if (!user_strlen(path, 256))
        return -1;
    if (dirfd != AT_FDCWD) {
        printk("sys_mkdirat: dirfd unimplemented\n");
        return -1;
    }
    if (mode != 0) {
        printk("sys_mkdirat: mode unimplemented\n");
        return -1;
    }

    printk("sys_mkdirat: path '%s'\n", path);

    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((ip = create(path, INODE_DIRECTORY, 0, 0, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return 0;
}

define_syscall(mknodat, int dirfd, const char *path, mode_t mode, dev_t dev) {
    (void)mode;
    Inode *ip;
    if (!user_strlen(path, 256))
        return -1;
    if (dirfd != AT_FDCWD) {
        printk("sys_mknodat: dirfd unimplemented\n");
        return -1;
    }

    unsigned int ma = major(dev);
    unsigned int mi = minor(dev);
    printk("mknodat: path '%s', major:minor %u:%u\n", path, ma, mi);
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((ip = create(path, INODE_DEVICE, (short)ma, (short)mi, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return 0;
}

define_syscall(chdir, const char *path) {
    /**
     * Change the cwd (current working dictionary) of current process to 'path'.
     */

    OpContext ctx;
    bcache.begin_op(&ctx);
    Inode *ip = namei(path, &ctx);
    if (ip == NULL) {
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.lock(ip);
    if (ip->entry.type != INODE_DIRECTORY) {
        inodes.unlock(ip);
        inodes.put(&ctx, ip);
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.unlock(ip);
    Proc *nowproc = thisproc();
    inodes.put(&ctx, nowproc->cwd);
    bcache.end_op(&ctx);
    nowproc->cwd = ip;
    return 0;
}

define_syscall(pipe2, int pipefd[2], int flags) {
    File *rf, *wf;
    if (flags)
        return -1;
    if (pipe_alloc(&rf, &wf) < 0)
        return -1;
    int fd0 = fdalloc(rf), fd1 = fdalloc(wf);
    if (fd0 < 0 || fd1 < 0) {
        if (fd0 >= 0)
            thisproc()->oftable.openfile[fd0] = 0;
        file_close(rf);
        file_close(wf);
        return -1;
    }
    pipefd[0] = fd0;
    pipefd[1] = fd1;
    return 0;
}
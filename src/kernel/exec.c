#include <aarch64/trap.h>
#include <common/defines.h>
#include <common/string.h>
#include <elf.h>
#include <fs/file.h>
#include <fs/inode.h>
#include <kernel/console.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/proc.h>
#include <kernel/pt.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>

#define USERTOP (1 + ~KSPACE_MASK)  // 0x0001000000000000
#define USTACK_SIZE (16 * PAGE_SIZE)
#define UPALIGN(x) (((x) + 0xf) & ~0xf)

extern int fdalloc(struct file *f);

static ALWAYS_INLINE bool load_elf(struct pgdir *pgdir, const char *path, Elf64_Ehdr *elf_out) {
    // 打开和检查ELF文件
    OpContext ctx;
    bcache.begin_op(&ctx);
    Inode *ip = namei(path, &ctx);
    if (ip == NULL) {
        bcache.end_op(&ctx);
        return false;
    }
    inodes.lock(ip);

    // 验证ELF头
    Elf64_Ehdr elf;
    if (inodes.read(ip, (u8 *)&elf, 0, sizeof(elf)) != sizeof(elf)) {
        goto bad;
    }
    if (memcmp((char *)elf.e_ident, ELFMAG, SELFMAG) != 0) {
        goto bad;
    }
    if (elf.e_ident[EI_CLASS] != ELFCLASS64) {
        goto bad;
    }

    u64 sz = 0;
    bool first = true;
    for (usize i = 0, off = elf.e_phoff; i < elf.e_phnum; i++, off += sizeof(Elf64_Phdr)) {
        Elf64_Phdr ph;
        if ((inodes.read(ip, (u8 *)&ph, off, sizeof(ph))) != sizeof(ph)) {
            goto bad;
        }
        if (ph.p_type != PT_LOAD) {
            continue;
        }
        if (ph.p_memsz < ph.p_filesz) {
            PANIC();
        }
        if (ph.p_vaddr + ph.p_memsz < ph.p_vaddr) {
            PANIC();
        }
        if (first) {
            first = false;
            sz = ph.p_vaddr;
        }

        // uvm alloc
        for (u64 va = round_up(sz, PAGE_SIZE); va < ph.p_vaddr + ph.p_memsz; va += PAGE_SIZE) {
            void *page = kalloc_page();
            // memset(p, 0, PAGE_SIZE);
            *get_pte(pgdir, va, true) = K2P(page) | PTE_USER_DATA;
        }
        sz = ph.p_vaddr + ph.p_memsz;

        for (usize va = ph.p_vaddr, len = ph.p_filesz; len;) {
            u64 *pte = get_pte(pgdir, va, true);
            ASSERT(*pte & PTE_VALID);
            void *page = (void *)P2K(PTE_ADDRESS(*pte));
            usize pgoff = va % PAGE_SIZE;
            usize n = MIN(PAGE_SIZE - pgoff, len);
            inodes.read(ip, page + pgoff, ph.p_offset + (va - ph.p_vaddr), n);
            len -= n, va += n;
        }
    }

    *elf_out = elf;

    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return true;

bad:
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return false;
}

SpinLock exec_lock = {0};

static SleepLock load_lock = {.lock = {0}, .val = 1, .sleeplist = {&load_lock.sleeplist, &load_lock.sleeplist}};

int execve(const char *path, char *const argv[], char *const envp[]) {
    struct pgdir *const pgdir = kalloc(sizeof(struct pgdir));
    if (pgdir == NULL) {
        return -1;
    }
    init_pgdir(pgdir);

    Elf64_Ehdr elf;

    unalertable_acquire_sleeplock(&load_lock);
    if (!load_elf(pgdir, path, &elf)) {
        release_sleeplock(&load_lock);
        free_pgdir(pgdir);
        return -1;
    }
    release_sleeplock(&load_lock);

    Proc *curproc = thisproc();
    struct pgdir oldpd = curproc->pgdir;
    attach_pgdir(&oldpd);
    arch_tlbi_vmalle1is();
    u64 sp = (USERTOP - USTACK_SIZE);
    {
        u64 argc = 0, envc = 0;
        if (envp)
            while (envp[envc])
                ++envc;
        if (argv)
            while (argv[argc])
                ++argc;
        uint64_t newargv[argc + 1], newenvp[envc + 1];

        sp -= 16;
        copyout(pgdir, (void *)sp, 0, 8);
        if (envp) {
            for (int i = envc - 1; i >= 0; --i) {
                sp -= strlen(envp[i]) + 1;
                sp -= (u64)sp % 16;
                copyout(pgdir, (void *)sp, envp[i], strlen(envp[i]) + 1);
                newenvp[i] = sp;
            }
        }
        newenvp[envc] = 0;

        sp -= 8;
        copyout(pgdir, (void *)sp, 0, 8);

        if (argv) {
            for (int i = argc - 1; i >= 0; --i) {
                sp -= strlen(argv[i]) + 1;
                sp -= (u64)sp % 16;
                copyout(pgdir, (void *)sp, argv[i], strlen(argv[i]) + 1);
                newargv[i] = sp;
            }
        }
        newargv[argc] = 0;

        sp -= (u64)(envc + 1) * 8;
        copyout(pgdir, (void *)sp, newenvp, (u64)(envc + 1) * 8);
        sp -= (u64)(argc + 1) * 8;
        copyout(pgdir, (void *)sp, newargv, (u64)(argc + 1) * 8);
        sp -= 8;
        copyout(pgdir, (void *)sp, &argc, sizeof(argc));
    }

    u64 stksz = (USERTOP - (usize)sp + 10 * PAGE_SIZE - 1) / (10 * PAGE_SIZE) * (10 * PAGE_SIZE);
    copyout(pgdir, (void *)(USERTOP - stksz), 0, stksz - (USERTOP - (usize)sp));
    ASSERT((uint64_t)sp > USERTOP - stksz);
    curproc->pgdir = *pgdir;
    init_list_node(&curproc->pgdir.section_head);
    curproc->ucontext->elr = elf.e_entry;
    curproc->ucontext->sp = (uint64_t)sp;
    attach_pgdir(&curproc->pgdir);
    arch_tlbi_vmalle1is();
    free_pgdir(&oldpd);

    return 0;
}

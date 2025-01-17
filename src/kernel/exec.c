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

#define push(argv) ({                                      \
    isize argc = 0;                                        \
    if (argv) {                                            \
        for (; /*argc < ARG_COUNT */ argv[argc]; argc++) { \
            usize len = strlen(argv[argc]);                \
            sp -= UPALIGN(len + 1);                        \
            copyout(pgdir, sp, argv[argc], len + 1);       \
        }                                                  \
    }                                                      \
    argc;                                                  \
})

int uvm_alloc(struct pgdir *pgdir, u64 base, u64 stksz, u64 oldsz, u64 newsz) {
    ASSERT(stksz % PAGE_SIZE == 0);
    base = base;
    for (u64 a = (oldsz + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE; a < newsz; a += PAGE_SIZE) {
        void *p = kalloc_page();
        ASSERT(p != NULL);
        *get_pte(pgdir, a, true) = K2P(p) | PTE_USER_DATA;
    }
    return newsz;
}

/* Data cache clean and invalidate by virtual address to point of coherency. */
static ALWAYS_INLINE void arch_dccivac(void *p, int n) {
    while (n--)
        asm volatile("dc civac, %[x]" : : [x] "r"(p + n));
}

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
    if (!(elf.e_ident[EI_MAG0] == ELFMAG0 && elf.e_ident[EI_MAG1] == ELFMAG1 &&
          elf.e_ident[EI_MAG2] == ELFMAG2 && elf.e_ident[EI_MAG3] == ELFMAG3)) {
        goto bad;
    }
    if (elf.e_ident[EI_CLASS] != ELFCLASS64) {
        goto bad;
    }

    Elf64_Phdr ph;
    u64 sz = 0, base = 0, stksz = 0;
    bool first = true;
    for (usize i = 0, off = elf.e_phoff; i < elf.e_phnum; i++, off += sizeof(ph)) {
        if ((inodes.read(ip, (u8 *)&ph, off, sizeof(ph))) != sizeof(ph)) {
            PANIC();
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
            sz = base = ph.p_vaddr;
            if (base % PAGE_SIZE != 0) {
                PANIC();
            }
        }
        if ((sz = uvm_alloc(pgdir, base, stksz, sz, ph.p_vaddr + ph.p_memsz)) == 0) {
            PANIC();
        }
        attach_pgdir(pgdir);
        arch_tlbi_vmalle1is();

        static u8 buf[10 << 20];  // todo: no buffer, direct copy to user space
        if (inodes.read(ip, buf, ph.p_offset, ph.p_filesz) != ph.p_filesz) {
            PANIC();
        }
        copyout(pgdir, (void *)ph.p_vaddr, buf, ph.p_filesz);

        arch_fence();
        arch_dccivac((void *)ph.p_vaddr, ph.p_memsz);
        arch_fence();
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

#include <kernel/printk.h>

int execve(const char *path, char *const argv[], char *const envp[]) {
    printk("\e[0;36m""execve %s\n""\e[0m", path);

    struct pgdir *const pgdir = kalloc(sizeof(struct pgdir));
    if (pgdir == NULL) {
        return -1;
    }
    init_pgdir(pgdir);

    Elf64_Ehdr elf;
    if (!load_elf(pgdir, path, &elf)) {
        printk("load_elf failed\n");
        goto bad;
    }

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

    // sp = newsp;
    u64 stksz = (USERTOP - (usize)sp + 10 * PAGE_SIZE - 1) / (10 * PAGE_SIZE) * (10 * PAGE_SIZE);
    copyout(pgdir, (void *)(USERTOP - stksz), 0, stksz - (USERTOP - (usize)sp));
    ASSERT((uint64_t)sp > USERTOP - stksz);
    curproc->pgdir = *pgdir;
    curproc->ucontext->elr = elf.e_entry;
    curproc->ucontext->sp = (uint64_t)sp;
    attach_pgdir(&curproc->pgdir);
    arch_tlbi_vmalle1is();
    free_pgdir(&oldpd);

    return 0;

bad:

    free_pgdir(pgdir);  // `pgdir` is definitely not NULL.
    return -1;
}

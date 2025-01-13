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

#define USERTOP (1 + ~KSPACE_MASK)
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

int execve(const char *path, char *const argv[], char *const envp[]) {
    struct pgdir *const pgdir = kalloc(sizeof(struct pgdir));
    if (pgdir == NULL) {
        return -1;
    }
    init_pgdir(pgdir);

    // 打开和检查ELF文件
    OpContext ctx;
    bcache.begin_op(&ctx);
    Inode *ip = namei(path, &ctx);
    if (ip == NULL) {
        bcache.end_op(&ctx);
        goto bad;
    }
    inodes.lock(ip);

    // 验证ELF头
    Elf64_Ehdr elf;
    if (inodes.read(ip, (u8 *)&elf, 0, sizeof(elf)) != sizeof(elf)) {
        goto bad;
    }
    if (!(elf.e_ident[EI_MAG0] == ELFMAG0 && elf.e_ident[EI_MAG1] == ELFMAG1 && elf.e_ident[EI_MAG2] == ELFMAG2 && elf.e_ident[EI_MAG3] == ELFMAG3)) {
        goto bad;
    }
    if (elf.e_ident[EI_CLASS] != ELFCLASS64) {
        goto bad;
    }

    Proc *curproc = thisproc();
    struct pgdir oldpigdir = curproc->pgdir;
    curproc->pgdir = *pgdir;
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
            first = 0;
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

        // memset((void *)ph.p_vaddr + ph.p_filesz, 0, ph.p_memsz - ph.p_filesz);

        arch_fence();
        arch_dccivac((void *)ph.p_vaddr, ph.p_memsz);
        arch_fence();
    }
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    ip = NULL;

    attach_pgdir(&oldpigdir);
    arch_tlbi_vmalle1is();

    char *sp = (char *)USERTOP;
    usize argc = push(argv);
    usize envc = push(envp);

    void *newsp = (void *)(((usize)sp - (envc + argc + 4) * 8) / 16 * 16);
    copyout(pgdir, newsp, NULL, (void *)sp - newsp);
    attach_pgdir(pgdir);
    arch_tlbi_vmalle1is();

    uint64_t *newargv = newsp + 8;
    uint64_t *newenvp = (void *)newargv + 8 * (argc + 1);

    copyout(pgdir, (void *)newsp, &argc, sizeof(usize));
    copyout(pgdir, (void *)newsp + 8, newenvp, sizeof(newenvp));
    copyout(pgdir, (void *)newsp + 8 + 8 * (argc + 1), newargv, sizeof(newargv));

    sp = newsp;
    stksz = (USERTOP - (usize)sp + 10 * PAGE_SIZE - 1) / (10 * PAGE_SIZE) * (10 * PAGE_SIZE);
    copyout(pgdir, (void *)(USERTOP - stksz), 0, stksz - (USERTOP - (usize)sp));
    ASSERT((uint64_t)sp > USERTOP - stksz);
    curproc->pgdir = *pgdir;
    curproc->ucontext->elr = elf.e_entry;
    curproc->ucontext->sp = (uint64_t)sp;
    attach_pgdir(&curproc->pgdir);
    arch_tlbi_vmalle1is();
    free_pgdir(&oldpigdir);

    return 0;

bad:

    free_pgdir(pgdir);  // `pgdir` is definitely not NULL.
    if (ip) {
        inodes.unlock(ip);
        inodes.put(&ctx, ip);
        bcache.end_op(&ctx);
    }
    thisproc()->pgdir = oldpigdir;
    return -1;
}

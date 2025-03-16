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
#define STACK_PAGE 32               // 128KB stack
#define UPALIGN(x) (((x) + 0xf) & ~0xf)

extern int fdalloc(struct file *f);

static ALWAYS_INLINE bool load_elf(struct pgdir *pd, const char *path, Elf64_Ehdr *elf_out) {
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

        int sec_flag = 0;
        u64 end;
        if (ph.p_flags == (PF_R | PF_X)) {
            sec_flag = ST_TEXT;
            end = ph.p_vaddr + ph.p_filesz;
        } else if (ph.p_flags == (PF_R | PF_W)) {
            sec_flag = ST_FILE;
            end = ph.p_vaddr + ph.p_memsz;
        } else {
            goto bad;
        }

        // insert into new section
        struct section *st = kalloc(sizeof(struct section));
        st->flags = sec_flag;
        st->mmap_flags = 0;
        st->begin = ph.p_vaddr;
        st->end = end;
        _insert_into_list(&pd->section_head, &st->stnode);
        st->fp = NULL;

        u64 va = ph.p_vaddr, ph_off = ph.p_offset;
        while (va < ph.p_vaddr + ph.p_filesz) {
            u64 va0 = PAGE_BASE(va);
            u64 sz = MIN(PAGE_SIZE - (va - va0), ph.p_vaddr + ph.p_filesz - va);

            void *p = kalloc_page();
            memset(p, 0, PAGE_SIZE);
            u64 pte_flag = PTE_USER_DATA;
            if (sec_flag == ST_TEXT)
                pte_flag |= PTE_RO;
            vmmap(pd, va0, p, pte_flag);

            if (inodes.read(ip, (u8 *)p + va - va0, ph_off, sz) != sz) {
                goto bad;
            }

            va += sz;
            ph_off += sz;
        }

        if (va != PAGE_BASE(va))
            va = PAGE_BASE(va) + PAGE_SIZE;

        if (sec_flag == ST_FILE && ph.p_memsz > va - ph.p_vaddr) {
            while (va < ph.p_vaddr + ph.p_memsz) {
                u64 va0 = PAGE_BASE(va);
                u64 sz = MIN(PAGE_SIZE - (va - va0), ph.p_vaddr + ph.p_memsz - va);
                vmmap(pd, va0, get_zero_page(), PTE_USER_DATA | PTE_RO);
                va += sz;
            }
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

    u64 sp = USERTOP;

    for (int i = 1; i <= STACK_PAGE; ++i) {
        void *p = kalloc_page();
        memset(p, 0, PAGE_SIZE);
        vmmap(pgdir, sp - i * PAGE_SIZE, p, PTE_USER_DATA);
    }
    struct section *sec = kalloc(sizeof(struct section));
    memset(sec, 0, sizeof(struct section));
    sec->flags = ST_FILE;
    sec->begin = sp - STACK_PAGE * PAGE_SIZE;
    sec->end = sp;
    _insert_into_list(&pgdir->section_head, &sec->stnode);

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

    Proc *curproc = thisproc();
    struct pgdir oldpd = curproc->pgdir;
    curproc->pgdir = *pgdir;
    _insert_into_list(&pgdir->section_head, &curproc->pgdir.section_head);
    _detach_from_list(&pgdir->section_head);
    curproc->ucontext->elr = elf.e_entry;
    curproc->ucontext->sp = (uint64_t)sp;
    attach_pgdir(&curproc->pgdir);
    arch_tlbi_vmalle1is();
    free_pgdir(&oldpd);

    return 0;
}

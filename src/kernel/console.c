#include <aarch64/intrinsic.h>
#include <driver/interrupt.h>
#include <driver/uart.h>
#include <kernel/console.h>
#include <kernel/sched.h>

struct console cons;

void console_init() {
    init_spinlock(&cons.lock);
    init_sem(&cons.sem, 0);
    // set_interrupt_handler(, );
}
#define BACKSPACE 0xff
#define putc(c) ({           \
    if (c == BACKSPACE) {    \
        uart_put_char('\b'); \
        uart_put_char(' ');  \
        uart_put_char('\b'); \
    } else {                 \
        uart_put_char(c);    \
    }                        \
})

/**
 * console_write - write to uart from the console buffer.
 * @ip: the pointer to the inode
 * @buf: the buffer
 * @n: number of bytes to write
 */
isize console_write(Inode *ip, char *buf, isize n) {
    inodes.unlock(ip);
    acquire_spinlock(&cons.lock);
    for (isize i = 0; i < n; i++) {
        putc(buf[i]);
    }
    release_spinlock(&cons.lock);
    inodes.lock(ip);
    return n;
}

#define C(x) ((x) - '@')  // Control-x

/**
 * console_read - read to the destination from the buffer
 * @ip: the pointer to the inode
 * @dst: the destination
 * @n: number of bytes to read
 */
isize console_read(Inode *ip, char *dst, isize n) {
    inodes.unlock(ip);
    acquire_spinlock(&cons.lock);
    isize m = n;
    while (n > 0) {
        while (cons.read_idx == cons.write_idx) {
            if (thisproc()->killed) {
                release_spinlock(&cons.lock);
                inodes.lock(ip);
                return -1;
            }
            release_spinlock(&cons.lock);
            unalertable_wait_sem(&cons.sem);
        }
        int c = cons.buf[cons.read_idx % IBUF_SIZE];
        cons.read_idx += 1;
        if (c == C('D')) {
            if (n < m)
                cons.read_idx--;
            break;
        }
        *dst = c;
        dst++;
        --n;
        if (c == '\n')
            break;
    }
    release_spinlock(&cons.lock);
    inodes.lock(ip);
    return m - n;
}

void console_intr(char c) {
    acquire_spinlock(&cons.lock);
    switch (c) {
    case C('C'):
        ASSERT(kill(thisproc()->pid) == 0);
        break;
    case C('U'):
        while (cons.edit_idx != cons.write_idx && cons.buf[(cons.edit_idx - 1) % IBUF_SIZE] != '\n') {
            cons.edit_idx--;
            putc(BACKSPACE);
        }
        break;

    case C('H'):
    case '\x7f':
        if (cons.edit_idx != cons.write_idx) {
            cons.edit_idx--;
            putc(BACKSPACE);
        }
        break;
    default:
        if (c != 0 && cons.edit_idx - cons.read_idx < IBUF_SIZE) {
            // attention
            c = (c == '\r') ? '\n' : c;
            putc(c);
            cons.buf[cons.edit_idx++ % IBUF_SIZE] = c;

            if (c == '\n' || c == C('D') || cons.edit_idx == cons.read_idx + IBUF_SIZE) {
                cons.write_idx = cons.edit_idx;
                post_sem(&cons.sem);
            }
        }
        break;
    }

    release_spinlock(&cons.lock);
}
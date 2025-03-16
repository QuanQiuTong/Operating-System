#include <aarch64/intrinsic.h>
#include <common/string.h>
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
            acquire_spinlock(&cons.lock);
        }
        int c = cons.buf[cons.read_idx % IBUF_SIZE];
        cons.read_idx += 1;
        if (c == C('D')) {
            if (n < m)
                cons.read_idx--;
            break;
        }
        *dst++ = c;
        --n;
        if (c == '\n')
            break;
    }
    release_spinlock(&cons.lock);
    inodes.lock(ip);
    return m - n;
}

///@note Caller must hold the lock
static ALWAYS_INLINE void clear_line() {
    while (cons.edit_idx != cons.write_idx && cons.buf[(cons.edit_idx - 1) % IBUF_SIZE] != '\n') {
        cons.edit_idx--;
        putc(BACKSPACE);
    }
}

#define LINES 32
static char buf[LINES][IBUF_SIZE];  // LINES * IBUF_SIZE = PAGE_SIZE
static int buf_idx = 0, cur_idx = 0;

///@note Caller must hold the lock
static ALWAYS_INLINE void intr_impl(char c) {
    switch (c) {
    case C('C'):
        // int k = kill(thisproc()->pid);
        // if (k < 0) {
        //     printk("kill failed\n");
        // }
        putc('^'), putc('C'), putc('\n');

        __attribute__((fallthrough));  // [[fallthrough]];
    case C('U'):
        clear_line();
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
            c = (c == '\r') ? '\n' : c;
            putc(c);
            cons.buf[cons.edit_idx++ % IBUF_SIZE] = c;

            if (c == '\n' || c == C('D') || cons.edit_idx == cons.read_idx + IBUF_SIZE) {
                memcpy(buf[buf_idx], cons.buf + cons.read_idx % IBUF_SIZE, IBUF_SIZE);
                buf_idx = (buf_idx + 1) % LINES;
                cur_idx = buf_idx;

                cons.write_idx = cons.edit_idx;
                post_sem(&cons.sem);
            }
        }
        break;
    }
}

void console_intr(char c) {
    acquire_spinlock(&cons.lock);
    intr_impl(c);
    release_spinlock(&cons.lock);
}

void console_arrow_intr(char c) {
    acquire_spinlock(&cons.lock);
    switch (c) {
    case 'A':  // up
        int last = (cur_idx - 1 + LINES) % LINES;
        if (last == buf_idx || buf[last][0] == '\0') {
            break;
        }
        cur_idx = last;
        clear_line();
        for (int i = 0; i < IBUF_SIZE; i++) {
            if (buf[cur_idx][i] == '\n' || buf[cur_idx][i] == '\0')
                break;
            intr_impl(buf[cur_idx][i]);
        }
        break;

    case 'B':  // down
        if (cur_idx == buf_idx) {
            break;
        }
        cur_idx = (cur_idx + 1) % LINES;
        clear_line();
        for (int i = 0; i < IBUF_SIZE; i++) {
            if (buf[cur_idx][i] == '\n' || buf[cur_idx][i] == '\0')
                break;
            intr_impl(buf[cur_idx][i]);
        }
        break;
        
    case 'C':  // right
        break;
    case 'D':  // left
        break;

    default:
        break;
    }
    release_spinlock(&cons.lock);
}
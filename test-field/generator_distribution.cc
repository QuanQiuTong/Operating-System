
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define printk printf
#define kalloc malloc
#define kfree free

#define FAIL(...)            \
    {                        \
        printk(__VA_ARGS__); \
        while (1)            \
            ;                \
    }

#define INLINE inline
using u8 = unsigned char;
using u64 = unsigned long long;

/* Return the largest c that c is a multiple of b and c <= a. */
static INLINE u64 round_down(u64 a, u64 b) {
    return a - a % b;
}

/* Return the smallest c that c is a multiple of b and c >= a. */
static INLINE u64 round_up(u64 a, u64 b) {
    return round_down(a + b - 1, b);
}

static int stat[4097];

static void* p[4][10000];
static short sz[4][10000];

void test(int i) {
    for (int j = 0; j < 10000;) {
        if (j < 1000 || rand() > RAND_MAX / 16 * 7) {
            int z = 0;
            int r = rand() & 255;
            if (r < 127) {  // [17,64]
                z = rand() % 48 + 17;
                z = round_up((u64)z, 4ll);
            } else if (r < 181) {  // [1,16]
                z = rand() % 16 + 1;
            } else if (r < 235) {  // [65,256]
                z = rand() % 192 + 65;
                z = round_up((u64)z, 8ll);
            } else if (r < 255) {  // [257,512]
                z = rand() % 256 + 257;
                z = round_up((u64)z, 8ll);
            } else {  // [513,2040]
                z = rand() % 1528 + 513;
                z = round_up((u64)z, 8ll);
            }

            stat[z]++;
            // putchar('0' ^ i);

            sz[i][j] = z;
            p[i][j] = kalloc(z);
            u64 q = (u64)p[i][j];
            if (p[i][j] == NULL || ((z & 1) == 0 && (q & 1) != 0) ||
                ((z & 3) == 0 && (q & 3) != 0) ||
                ((z & 7) == 0 && (q & 7) != 0))
                FAIL("FAIL: kalloc(%d) = %p\n", z, p[i][j]);
            memset(p[i][j], i ^ z, z);
            j++;
        } else {
            int k = rand() % j;
            if (p[i][k] == NULL)
                FAIL("FAIL: block[%d][%d] null\n", i, k);
            int m = (i ^ sz[i][k]) & 255;
            for (int t = 0; t < sz[i][k]; t++)
                if (((u8*)p[i][k])[t] != m)
                    FAIL("FAIL: block[%d][%d] wrong\n", i, k);
            kfree(p[i][k]);
            p[i][k] = p[i][--j];
            sz[i][k] = sz[i][j];
        }
    }
}

#include <thread>

int main() {
    const int numThreads = 3;
    std::thread threads[numThreads];

    // 创建线程
    for (int i = 0; i < numThreads; ++i) {
        threads[i] = std::thread(test, i);
    }

    // 等待所有线程完成
    for (int i = 0; i < numThreads; ++i) {
        threads[i].join();
    }

    for (int i = 1; i <= 512; ++i)
        printk("[%d]\t%d\n", i, stat[i]);
    for (int i = 513; i <= 4096; ++i) {
        if (stat[i])
            printk("[%d]\t%d\n", i, stat[i]);
    }

    return 0;
}
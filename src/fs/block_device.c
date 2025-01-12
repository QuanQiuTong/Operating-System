#include <common/string.h>
#include <driver/virtio.h>
#include <fs/block_device.h>

/**
    @brief a simple implementation of reading a block from SD card.

    @param[in] block_no the block number to read
    @param[out] buffer the buffer to store the data
 */
static void sd_read(usize block_no, u8 *buffer) {
    Buf b;
    b.block_no = (u32)block_no;
    b.flags = 0;
    virtio_blk_rw(&b);
    memcpy(buffer, b.data, BLOCK_SIZE);
}

/**
    @brief a simple implementation of writing a block to SD card.

    @param[in] block_no the block number to write
    @param[in] buffer the buffer to store the data
 */
static void sd_write(usize block_no, u8 *buffer) {
    Buf b;
    b.block_no = (u32)block_no;
    b.flags = B_DIRTY | B_VALID;
    memcpy(b.data, buffer, BLOCK_SIZE);
    virtio_blk_rw(&b);
}

/**
    @brief the in-memory copy of the super block.

    We may need to read the super block multiple times, so keep a copy of it in
    memory.

    @note the super block, in our lab, is always read-only, so we don't need to
    write it back.
 */
static u8 sblock_data[BLOCK_SIZE];

static ALWAYS_INLINE u32 second_partition() {
    Buf b = {.flags = 0, .block_no = 0};
    virtio_blk_rw(&b);
    return *(u32 *)(b.data + 0x1CE + 0x8);
}

BlockDevice block_device;

void init_block_device() {
    block_device.read = sd_read;
    block_device.write = sd_write;

    // static __attribute__((unused)) void probe();
    // probe();

    u32 sblock_no = second_partition() + 1;
    block_device.read(sblock_no, sblock_data);
}

const SuperBlock *get_super_block() {
    return (const SuperBlock *)sblock_data;
}

#include <kernel/printk.h>

// Find the first block that is non-zero.
static __attribute__((unused)) void probe() {
    // static u8 block_device_data[BLOCK_SIZE] = {0};
    static u8 sblock[BLOCK_SIZE];
    SuperBlock *sb = (SuperBlock *)sblock;
    for (int i = 1; i < (128 << 20) / BLOCK_SIZE; i++) {
        block_device.read(i, (u8 *)sblock);

        if (sb->num_blocks == 1000) {
            printk("Found at block %d\n", i);
            printk("    num_blocks: %d\n", sb->num_blocks);
            printk("    num_data_blocks: %d\n", sb->num_data_blocks);
            printk("    num_inodes: %d\n", sb->num_inodes);
            printk("    num_log_blocks: %d\n", sb->num_log_blocks);
            printk("    log_start: %d\n", sb->log_start);
            printk("    inode_start: %d\n", sb->inode_start);
            printk("    bitmap_start: %d\n", sb->bitmap_start);
            break;
        }
        if (i % 1024 == 0) {
            printk("Probing block %d\n", i);
        }
    }
    /**
     * First partition:
     *
     * block 2048 is different
     * num_blocks: 1838176491
     * num_data_blocks: 779314795
     * num_inodes: 7627110
     * num_log_blocks: 2097410
     * log_start: 2
     * inode_start: 63488
     * bitmap_start: 524320
     */

    /**
     * Second partition:
     *
     *     block 133121
     * num_blocks: 1000
     * num_data_blocks: 908
     * num_inodes: 200
     * num_log_blocks: 63
     * log_start: 2
     * inode_start: 65
     * bitmap_start: 91
     */
}

// Full version of `second_partition` function.
static __attribute__((unused)) u32 get_second_partition() {
    struct partition_entry {
        u8 boot_flag;      // 引导标志
        u8 chs_start[3];   // 起始 CHS 地址
        u8 type;           // 分区类型
        u8 chs_end[3];     // 结束 CHS 地址
        u32 lba_start;     // 起始 LBA
        u32 sector_count;  // 扇区数量
    } __attribute__((packed));
    // ASSERT(sizeof(struct partition_entry) == 16);

    Buf b = {.flags = 0, .block_no = 0};
    if (virtio_blk_rw(&b) != 0) {
        printk("Failed to read MBR.\n");
        PANIC();
    }

#define part_table_offset 0x1BE
    auto part_table = (struct partition_entry *)(b.data + part_table_offset);

    u32 lba_start = part_table[1].lba_start;
    u32 sector_count = part_table[1].sector_count;

    printk(
        "\e[0;31mSecond partition:\n"
        "    LBA start = %d, sector count = %d\e[0m\n",
        lba_start, sector_count);

    return lba_start;
}
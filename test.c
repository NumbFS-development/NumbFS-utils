#include "internal.h"
#include "disk.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>

#define FILE_SIZE (10 * 1024 * 1024) // 10MB
#define TEST_NUM_INODES 4096

struct numbfs_superblock_info sbi;

static void init_sbi(int fd)
{
        int err, total_blocks, remain;
        char buf[BYTES_PER_BLOCK];
        off_t start, end;
        int i;

        sbi.fd = fd;
        sbi.size = FILE_SIZE;

        total_blocks = sbi.size / BYTES_PER_BLOCK;

        sbi.total_inodes = TEST_NUM_INODES;
        sbi.free_inodes = sbi.total_inodes;

        /* inode bitmap start block addr */
        sbi.ibitmap_start = 2;
        /* inodes start block add */
        sbi.inode_start = sbi.ibitmap_start +
                        DIV_ROUND_UP(DIV_ROUND_UP(sbi.total_inodes, BITS_PER_BYTE), BYTES_PER_BLOCK);
        /* block bitmap start block addr */
        sbi.bbitmap_start = sbi.inode_start +
                        DIV_ROUND_UP(sbi.total_inodes * sizeof(struct numbfs_inode), BYTES_PER_BLOCK);

        remain = total_blocks - sbi.bbitmap_start - 1;
        /* nr free data blocks */
        sbi.data_blocks = remain -
                        DIV_ROUND_UP(DIV_ROUND_UP(remain, BITS_PER_BYTE), BYTES_PER_BLOCK);
        sbi.free_blocks = sbi.data_blocks;

        start = 2;
        end = sbi.bbitmap_start +
                        DIV_ROUND_UP(DIV_ROUND_UP(sbi.data_blocks, BITS_PER_BYTE), BYTES_PER_BLOCK);
        memset(buf, 0, sizeof(buf));
        /* clear all the bits in range [start, end] */
        for (i = start; i < end; i++) {
                err = pwrite(sbi.fd, buf, BYTES_PER_BLOCK, i * BYTES_PER_BLOCK);
                assert(err == BYTES_PER_BLOCK);
        }
        sbi.data_start = end;

        /* set all the data array to NUMBFS_HOLE */
        for (i = sbi.inode_start; i < sbi.bbitmap_start; i++) {
                int j, k;
                struct numbfs_inode *inode;

                assert(!numbfs_read_block(&sbi, buf, i));

                for (j = 0; j < (int)NUMBFS_NODES_PER_BLOCK; j++) {
                        inode = ((struct numbfs_inode*)buf) + j;
                        for (k = 0; k < NUMBFS_NUM_DATA_ENTRY; k++)
                                inode->i_data[k] = cpu_to_le32(NUMBFS_HOLE);
                }

                assert(!numbfs_write_block(&sbi, buf, i));
        }

        /* data zone start block addr */
}

static void test_hole(void)
{
        struct numbfs_inode_info ni;
        char wcontent[BYTES_PER_BLOCK];
        char rcontent[BYTES_PER_BLOCK];
        char zero[BYTES_PER_BLOCK];
        int i;

#define TEST_NID        1
#define TEST_BLK        7


        /* random content to write */
        for (i = 0; i < BYTES_PER_BLOCK; i ++)
                wcontent[i] = i % 10;
        memset(zero, 0, BYTES_PER_BLOCK);

        /* get inode info */
        ni.sbi = &sbi;
        ni.nid = TEST_NID;

        assert(!numbfs_get_inode(&sbi, &ni));

        assert(!numbfs_pwrite_inode(&ni, wcontent, TEST_BLK * BYTES_PER_BLOCK, BYTES_PER_BLOCK));
        for (i = 0; i < TEST_BLK; i++) {
                assert(!numbfs_pread_inode(&ni, rcontent, i * BYTES_PER_BLOCK, BYTES_PER_BLOCK));
                /* should be zero, since these blocks are all holes */
                assert(!memcmp(rcontent, zero, BYTES_PER_BLOCK));
        }

        assert(!numbfs_pread_inode(&ni, rcontent, TEST_BLK * BYTES_PER_BLOCK, BYTES_PER_BLOCK));
        assert(!memcmp(rcontent, wcontent, BYTES_PER_BLOCK));

        /* write the middle block */
        assert(!numbfs_pwrite_inode(&ni, wcontent, (TEST_BLK / 2) * BYTES_PER_BLOCK, BYTES_PER_BLOCK));
        assert(!numbfs_pread_inode(&ni, rcontent, (TEST_BLK / 2) * BYTES_PER_BLOCK, BYTES_PER_BLOCK));
        assert(!memcmp(wcontent, rcontent, BYTES_PER_BLOCK));

#undef TEST_NID
#undef TEST_BLK
}

static void test_byte_rw(void)
{
        struct numbfs_inode_info inode;
        char rbuf[BYTES_PER_BLOCK], wbuf[BYTES_PER_BLOCK];
        int i;
#define TEST_BLK 6

        inode.sbi = &sbi;
        inode.nid = TEST_NUM_INODES / 2;

        memset(wbuf, 0, BYTES_PER_BLOCK);
        memset(rbuf, 0, BYTES_PER_BLOCK);
        assert(!numbfs_get_inode(&sbi, &inode));
        for (i = 0; i < BYTES_PER_BLOCK / 4; i++)
                wbuf[i] = 0x73;

        assert(!numbfs_pwrite_inode(&inode, wbuf, TEST_BLK * BYTES_PER_BLOCK + TEST_BLK / 4, BYTES_PER_BLOCK / 4));
        assert(!numbfs_pread_inode(&inode, rbuf, TEST_BLK * BYTES_PER_BLOCK + TEST_BLK / 4, (BYTES_PER_BLOCK / 4) * 3));
        assert(!memcmp(rbuf, wbuf, BYTES_PER_BLOCK));
#undef TEST_BLK
}

static int numbfs_block_count(void)
{
        int cnt = 0, i, byte, bit;
        char buf[BYTES_PER_BLOCK];

        for (i = 0; i < sbi.data_blocks; i++) {
                if (i % NUMBFS_BLOCKS_PER_BLOCK == 0)
                        assert(!numbfs_read_block(&sbi, buf, numbfs_bmap_blk(sbi.bbitmap_start, i)));
                byte = numbfs_bmap_byte(i);
                bit = numbfs_bmap_bit(i);
                if (!(buf[byte] & (1 << bit)))
                        cnt++;
        }
        return cnt;
}

static void test_block_management(void)
{
#define TEST_TIMES (BYTES_PER_BLOCK * 2 + 1)
        int total_blocks = numbfs_block_count();
        int blks[TEST_TIMES];
        int i;

        assert(sbi.free_blocks == total_blocks);

        for (i = 0; i < TEST_TIMES; i++) {
                int free_blocks;

                assert(!numbfs_alloc_block(&sbi, &blks[i]));
                free_blocks = numbfs_block_count();
                assert(total_blocks - free_blocks == i + 1);
                assert(sbi.free_blocks == free_blocks);
        }

        for (i = 0; i < TEST_TIMES; i++) {
                assert(!numbfs_free_block(&sbi, blks[i]));
                assert(total_blocks - numbfs_block_count() == TEST_TIMES - i - 1);
        }
}

static int numbfs_inode_count(void)
{
        int cnt = 0, i, byte, bit;
        char buf[BYTES_PER_BLOCK];

        for (i = 0; i < sbi.total_inodes; i++) {
                if (i % NUMBFS_BLOCKS_PER_BLOCK == 0)
                        assert(!numbfs_read_block(&sbi, buf, numbfs_bmap_blk(sbi.ibitmap_start, i)));

                byte = numbfs_bmap_byte(i);
                bit = numbfs_bmap_bit(i);
                if (!(buf[byte] & (1 << bit)))
                        cnt++;
        }
        return cnt;
}


static void test_inode_management(void)
{
        int total_inodes = numbfs_inode_count();
        int inodes[TEST_TIMES];
        int i;

        assert(sbi.free_inodes == total_inodes);

        for (i = 0; i < TEST_TIMES; i++) {
                int free_inodes;

                assert(!numbfs_alloc_inode(&sbi, &inodes[i]));
                assert(inodes[i] == i);
                free_inodes = numbfs_inode_count();
                assert(total_inodes - i - 1 == free_inodes);
                assert(sbi.free_inodes == free_inodes);
        }

        for (i = 0; i < TEST_TIMES; i++) {
                assert(!numbfs_free_inode(&sbi, inodes[i]));
                assert(total_inodes - numbfs_inode_count() == TEST_TIMES - i - 1);
        }

}

static long dis(long a, long b)
{
        return a > b ? a - b : b - a;
}

static void test_timestamps(void)
{
#define TIME_DELTA      3
        struct numbfs_inode_info ni;
        struct numbfs_timestamps *nt;
        char buf[BYTES_PER_BLOCK];
        long start = time(NULL);
        int nid;

        nid = numbfs_empty_dir(&sbi, NUMBFS_ROOT_NID);
        assert(nid >= 0);

        ni.nid = nid;
        ni.sbi = &sbi;
        assert(!numbfs_get_inode(&sbi, &ni));

        assert(!numbfs_read_block(&sbi, buf, numbfs_data_blk(&sbi, ni.xattr_start)));

        nt = (struct numbfs_timestamps*)buf;
        assert(dis(le64_to_cpu(nt->t_atime), start) < TIME_DELTA);
        assert(dis(le64_to_cpu(nt->t_ctime), start) < TIME_DELTA);
        assert(dis(le64_to_cpu(nt->t_mtime), start) < TIME_DELTA);

#undef TIME_DELTA

}

int main() {
        const char *filename = "./numbfs_test_file_xxx";
        int fd = open(filename, O_RDWR | O_CREAT, 0644);

        assert(fd != -1);
        assert(ftruncate(fd, FILE_SIZE) != -1);

        init_sbi(fd);

        /* do tests */
        test_hole();
        test_byte_rw();
        test_block_management();
        test_inode_management();
        test_timestamps();

        close(fd);
        assert(remove(filename) == 0);
        return 0;
}

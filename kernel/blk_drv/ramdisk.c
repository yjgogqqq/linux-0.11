/*
 *  linux/kernel/blk_drv/ramdisk.c
 *
 *  Written by Theodore Ts'o, 12/2/91
 */

#include <string.h>

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <asm/system.h>
#include <asm/segment.h>
#include <asm/memory.h>

#define MAJOR_NR 1
#include "blk.h"

char	*rd_start;
int	rd_length = 0;

void do_rd_request(void)
{
	int	len;
	char	*addr;

	INIT_REQUEST;
	addr = rd_start + (CURRENT->sector << 9);
	len = CURRENT->nr_sectors << 9;
	if ((MINOR(CURRENT->dev) != 1) || (addr+len > rd_start+rd_length)) {
		end_request(0);
		goto repeat;
	}
	if (CURRENT-> cmd == WRITE) {
		(void ) memcpy(addr,
			      CURRENT->buffer,
			      len);
	} else if (CURRENT->cmd == READ) {
		(void) memcpy(CURRENT->buffer, 
			      addr,
			      len);
	} else
		panic("unknown ramdisk-command");
	end_request(1);
	goto repeat;
}

/*
 * Returns amount of memory which needs to be reserved.
 */
long rd_init(long mem_start, int length)
{
	int	i;
	char	*cp;

	blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;
	rd_start = (char *) mem_start;
	rd_length = length;
	cp = rd_start;
	for (i=0; i < length; i++)
		*cp++ = '\0';
	return(length);
}

/*
 * If the root device is the ram disk, try to load it.
 * In order to do this, the root device is originally set to the
 * floppy, and we later change it to be ram disk.
 */
void rd_load(void)
{
	struct buffer_head *bh;
	struct super_block	s;
	int		block = 256;	/* Start at block 256 */
	int		i = 1;
	int		nblocks;
	char		*cp;		/* Move pointer */
	
	if (!rd_length)
		return;
	printk("Ram disk: %d bytes, starting at 0x%x\n", rd_length,
		(int) rd_start);
	if (MAJOR(ROOT_DEV) != 2)
		return;
	bh = breada(ROOT_DEV,block+1,block,block+2,-1);	//从软盘预读取一些数据块
					//此函数和bread函数的基本功能一样，都是从块设备上读取数据块，
					//不同点在于它可以把一些连续的数据块都读进来，一共三块，分别是：257、256、258，
					//其中超级块就在257中
	if (!bh) {
		printk("Disk error while looking for ramdisk!\n");
		return;
	}
	//此时超级块中的内容已经被载入缓冲区中，于是系统将超级块的内容进行备份，并释放掉该缓冲块。
	//超级块的内容备份到了位于进程1管理结构所在页面的进程1内核栈中。
	*((struct d_super_block *) &s) = *((struct d_super_block *) bh->b_data);
	brelse(bh);
	//之后，分析超级块信息，包括判断这个文件系统是不是minix文件系统；包括接下来要载入的根文件系统的数据块数，
	//会不会比整个虚拟盘区都大等，这些条件都通过，才能继续加载根文件系统，否则返回。
	if (s.s_magic != SUPER_MAGIC)
		/* No ram disk image present, assume normal floppy boot */
		return;
	nblocks = s.s_nzones << s.s_log_zone_size;	//在得到代表根文件系统中数据块总数的变量nblocks的过程中，超级块发挥了作用
	if (nblocks > (rd_length >> BLOCK_SIZE_BITS)) {
		printk("Ram disk image too big!  (%d blocks, %d avail)\n", 
			nblocks, rd_length >> BLOCK_SIZE_BITS);
		return;
	}
	printk("Loading %d bytes into ram disk... 0000k", 
		nblocks << BLOCK_SIZE_BITS);
	cp = rd_start;
	//把与根文件系统相关的内容从软盘上拷贝到虚拟盘中。
	//进程1与虚拟盘进行沟通，每次拷贝三个数据块到缓冲块，直到把规定的数据全部拷贝进来为止，
	//并将其备份到虚拟盘空间，然后及时释放缓冲块。
	while (nblocks) {
		if (nblocks > 2) 
			bh = breada(ROOT_DEV, block, block+1, block+2, -1);
		else
			bh = bread(ROOT_DEV, block);
		if (!bh) {
			printk("I/O error on block %d, aborting load\n", 
				block);
			return;
		}
		(void) memcpy(cp, bh->b_data, BLOCK_SIZE);
		brelse(bh);
		printk("\010\010\010\010\010%4dk",i);
		cp += BLOCK_SIZE;
		block++;
		nblocks--;
		i++;
	}
	printk("\010\010\010\010\010done \n");
	//拷贝结束后，将虚拟盘设置为根设备。
	ROOT_DEV=0x0101;
						//至此，进程1用虚拟盘取代了软盘，使之成为根设备
}

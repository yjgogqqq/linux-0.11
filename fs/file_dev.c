/*
 *  linux/fs/file_dev.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>
#include <fcntl.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

int file_read(struct m_inode * inode, struct file * filp, char * buf, int count)
{
	int left,chars,nr;
	struct buffer_head * bh;

	if ((left=count)<=0)
		return 0;
	while (left) {
		if (nr = bmap(inode,(filp->f_pos)/BLOCK_SIZE)) {	//确定用户读取的数据在硬盘上的“块号”
			if (!(bh=bread(inode->i_dev,nr)))	//将指定的数据块从硬盘读入到缓冲区中。
				break;
		} else
			bh = NULL;
		nr = filp->f_pos % BLOCK_SIZE;
		chars = MIN( BLOCK_SIZE-nr , left );
		filp->f_pos += chars;	//调整当前的文件操作指针f_pos,以便下次读取后续的数据块。
		left -= chars;
		//接下来把已经读出来的数据拷贝到用户执行空间内
		if (bh) {
			char * p = nr + bh->b_data;
			while (chars-->0)
				put_fs_byte(*(p++),buf++);
			brelse(bh);
		} else {
			while (chars-->0)
				put_fs_byte(0,buf++);
		}
	}
	inode->i_atime = CURRENT_TIME;
	return (count-left)?(count-left):-ERROR;
}

int file_write(struct m_inode * inode, struct file * filp, char * buf, int count)
{
	off_t pos;
	int block,c;
	struct buffer_head * bh;
	char * p;
	int i=0;

/*
 * ok, append may not work when many processes are writing at the same time
 * but so what. That way leads to madness anyway.
 */
	if (filp->f_flags & O_APPEND)
		pos = inode->i_size;
	else
		pos = filp->f_pos;
	while (i<count) {
		if (!(block = create_block(inode,pos/BLOCK_SIZE)))	
			break;
		
		if (!(bh=bread(inode->i_dev,block)))
			break;
		//根据文件的当前操作指针pos在这个逻辑块中测算出写入的精确位置。
		c = pos % BLOCK_SIZE;
		p = c + bh->b_data;
		//之后将该缓冲块的b_dirt标志位设置为1，将来把数据往硬盘上同步的时候，
		//只要见到b_dirt标志位是1的缓冲块，一律同步。
		bh->b_dirt = 1;
		//统计还剩多少数据没有写入，并将i节点的属性进行调整，比如文件的大小等。
		c = BLOCK_SIZE-c;
		if (c > count-i) c = count-i;
		pos += c;
		if (pos > inode->i_size) {
			inode->i_size = pos;
			inode->i_dirt = 1;
		}
		i += c;
		//将用户指定的数据写入到缓冲块中，并释放该缓冲块。
		while (c-->0)
			*(p++) = get_fs_byte(buf++);
		brelse(bh);
	}
	inode->i_mtime = CURRENT_TIME;
	if (!(filp->f_flags & O_APPEND)) {
		filp->f_pos = pos;
		inode->i_ctime = CURRENT_TIME;
	}
	return (i?i:-1);
}

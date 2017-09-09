/*
 *  linux/fs/bitmap.c
 *
 *  (C) 1991  Linus Torvalds
 */

/* bitmap.c contains the code that handles the inode and block bitmaps */
#include <string.h>

#include <linux/sched.h>
#include <linux/kernel.h>

#define clear_block(addr) \
__asm__("cld\n\t" \
	"rep\n\t" \
	"stosl" \
	::"a" (0),"c" (BLOCK_SIZE/4),"D" ((long) (addr)):"cx","di")

#define set_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__ __volatile__("btsl %2,%3\n\tsetb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})

#define clear_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__ __volatile__("btrl %2,%3\n\tsetnb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})

#define find_first_zero(addr) ({ \
int __res; \
__asm__("cld\n" \
	"1:\tlodsl\n\t" \
	"notl %%eax\n\t" \
	"bsfl %%eax,%%edx\n\t" \
	"je 2f\n\t" \
	"addl %%edx,%%ecx\n\t" \
	"jmp 3f\n" \
	"2:\taddl $32,%%ecx\n\t" \
	"cmpl $8192,%%ecx\n\t" \
	"jl 1b\n" \
	"3:" \
	:"=c" (__res):"c" (0),"S" (addr):"ax","dx","si"); \
__res;})

void free_block(int dev, int block)
{
	struct super_block * sb;
	struct buffer_head * bh;

	//由于超级块是对硬盘上的数据进行操作的起点，因此先要得到超级块的数据，之后在缓冲区中查找是否存放与
	//即将删除的数据块有关的缓冲块，如果有，并且它的bh->b_count=1，就将它的脏标志bh->b_dirt和有效标志
	//bh->b_uptodate都清0，这样做的原因是bh->b_count=1表明该缓冲块仅被当前进程引用，即然当前进程已经要
	//删除它了，自然也就没有必要将其回写到硬盘或作为有效数据。
	if (!(sb = get_super(dev)))
		panic("trying to free block on nonexistent device");
	if (block < sb->s_firstdatazone || block >= sb->s_nzones)
		panic("trying to free block not in datazone");
	bh = get_hash_table(dev,block);
	if (bh) {
		if (bh->b_count != 1) {
			printk("trying to free block (%04x:%d), count=%d\n",
				dev,block,bh->b_count);
			return;
		}
		bh->b_dirt=0;
		bh->b_uptodate=0;
		brelse(bh);
	}
	//依据超级块中所存储的信息计算出要删除的数据块在硬盘数据区中的实际位置
	block -= sb->s_firstdatazone - 1 ;
	//在逻辑块位图中将该数据块对应的位置置0即可。由于该操作改变了某个逻辑块位图对应的缓冲块，因此缓冲块
	//的脏标志sb->s_zmap[block/8192]->b_dirt也需要置1。
	if (clear_bit(block&8191,sb->s_zmap[block/8192]->b_data)) {
		printk("block (%04x:%d) ",dev,block+sb->s_firstdatazone-1);
		panic("free_block: bit already cleared");
	}
	sb->s_zmap[block/8192]->b_dirt = 1;
}

int new_block(int dev)
{
	struct buffer_head * bh;
	struct super_block * sb;
	int i,j;

	//先通过缓冲区中常驻的逻辑块位图结构，找到一个空闲的逻辑块。
	if (!(sb = get_super(dev)))
		panic("trying to get new block from nonexistant device");
	j = 8192;
	for (i=0 ; i<8 ; i++)
		if (bh=sb->s_zmap[i])
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	if (i>=8 || !bh || j>=8192)
		return 0;
	if (set_bit(j,bh->b_data))
		panic("new_block: bit already set");
	bh->b_dirt = 1;
	j += i*8192 + sb->s_firstdatazone-1;
	if (j >= sb->s_nzones)
		return 0;
	//通过getblk()函数将这个空闲的逻辑块在缓冲区管理结构中注册，并进行设置
	if (!(bh=getblk(dev,j)))
		panic("new_block: cannot get block");
	if (bh->b_count != 1)
		panic("new block: count is != 1");
	//将缓冲块所在的内存中的1KB空间清0，然后逻辑块的块号返回。
	clear_block(bh->b_data);
	bh->b_uptodate = 1;
	bh->b_dirt = 1;
	brelse(bh);
	return j;
}

void free_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;

	
	if (!inode)
		return;
	if (!inode->i_dev) {
		memset(inode,0,sizeof(*inode));
		return;
	}
	if (inode->i_count>1) {
		printk("trying to free inode with count=%d\n",inode->i_count);
		panic("free_inode");
	}
	if (inode->i_nlinks)
		panic("trying to free inode with links");
	if (!(sb = get_super(inode->i_dev)))
		panic("trying to free inode on nonexistent device");
	if (inode->i_num < 1 || inode->i_num > sb->s_ninodes)
		panic("trying to free inode 0 or nonexistant inode");
	if (!(bh=sb->s_imap[inode->i_num>>13]))
		panic("nonexistent imap in superblock");
	if (clear_bit(inode->i_num&8191,bh->b_data))
		printk("free_inode: bit already cleared.\n\r");
	//由于删除i节点的操作也使得i节点位图所在的高速缓冲块中的内容发生了改变，因此将缓冲块的脏标志置1。
	bh->b_dirt = 1;	
	//硬盘上的i节点删除之后，xxx文件在内存i节点表中的i节点也就没有意义了。因此需要执行以下函数，
	//将这个记录全部在物理上清0
	memset(inode,0,sizeof(*inode));
}

struct m_inode * new_inode(int dev)
{
	struct m_inode * inode;
	struct super_block * sb;
	struct buffer_head * bh;
	int i,j;

	if (!(inode=get_empty_inode()))		//申请一个空闲项
		return NULL;
	if (!(sb = get_super(dev)))		//获取超级块
		panic("new_inode with unknown device");
	j = 8192;

	//通过搜索超级块上i节点位图的指针表s_impa[8]就可以在缓冲区里面的i节点位图上为xxx文件的i节点
	//找到一个空闲位置并将其置位，同时也获得了i节点号。
	for (i=0 ; i<8 ; i++)
		if (bh=sb->s_imap[i])
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	if (!bh || j >= 8192 || j+i*8192 > sb->s_ninodes) {
		iput(inode);
		return NULL;
	}
	if (set_bit(j,bh->b_data))
		panic("new_inode: bit already set");
	bh->b_dirt = 1;

	//对刚刚在系统的i节点管理表inode_table中申请的空闲项进行设置，这个空闲项将不再空闲，它将作为
	//xxx目录文件的i节点存放于系统中的i节点管理表inode_table中。
	inode->i_count=1;
	inode->i_nlinks=1;
	inode->i_dev=dev;
	inode->i_uid=current->euid;
	inode->i_gid=current->egid;
	inode->i_dirt=1;
	inode->i_num = j + i*8192;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	//上述设置过程并没有对内存i节点结构的所有字段进行设置，其他还没有被赋值的字段，由于在get_empty_inode中
	//已经提前把空闲i节点所占的存储区域全部置0，因此这些未赋值字段的默认值为0，保证了i节点数据不会出错
	return inode;
}

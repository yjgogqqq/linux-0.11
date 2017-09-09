/*
 *  linux/fs/inode.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <string.h>
#include <sys/stat.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/system.h>

struct m_inode inode_table[NR_INODE]={{0,},};

static void read_inode(struct m_inode * inode);
static void write_inode(struct m_inode * inode);

static inline void wait_on_inode(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	sti();
}

static inline void lock_inode(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	inode->i_lock=1;
	sti();
}

static inline void unlock_inode(struct m_inode * inode)
{
	inode->i_lock=0;
	wake_up(&inode->i_wait);
}

void invalidate_inodes(int dev)
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode);
		if (inode->i_dev == dev) {
			if (inode->i_count)
				printk("inode in use on removed disk\n\r");
			inode->i_dev = inode->i_dirt = 0;
		}
	}
}

void sync_inodes(void)
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	//遍历i节点表，只要发现哪个i节点被改动过，即，i_dirt为1，就说明该i节点需要同步。
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode);
		if (inode->i_dirt && !inode->i_pipe)
			write_inode(inode);
	}
}

static int _bmap(struct m_inode * inode,int block,int create)
{
	struct buffer_head * bh;
	int i;

	if (block<0)
		panic("_bmap: block<0");
	if (block >= 7+512+512*512)
		panic("_bmap: block>big");
	if (block<7) {
		if (create && !inode->i_zone[block])
			if (inode->i_zone[block]=new_block(inode->i_dev)) {
				inode->i_ctime=CURRENT_TIME;
				inode->i_dirt=1;
			}
		return inode->i_zone[block];
	}
	block -= 7;
	if (block<512) {
		if (create && !inode->i_zone[7])
			if (inode->i_zone[7]=new_block(inode->i_dev)) {	//new_block()函数负责新建一个数据块
				inode->i_dirt=1;
				inode->i_ctime=CURRENT_TIME;
			}
		if (!inode->i_zone[7])
			return 0;
		if (!(bh = bread(inode->i_dev,inode->i_zone[7])))
			return 0;
		i = ((unsigned short *) (bh->b_data))[block];
		if (create && !i)
			if (i=new_block(inode->i_dev)) {
				((unsigned short *) (bh->b_data))[block]=i;
				bh->b_dirt=1;
			}
		brelse(bh);
		return i;
	}
	block -= 512;
	if (create && !inode->i_zone[8])
		if (inode->i_zone[8]=new_block(inode->i_dev)) {
			inode->i_dirt=1;
			inode->i_ctime=CURRENT_TIME;
		}
	if (!inode->i_zone[8])
		return 0;
	if (!(bh=bread(inode->i_dev,inode->i_zone[8])))
		return 0;
	i = ((unsigned short *)bh->b_data)[block>>9];
	if (create && !i)
		if (i=new_block(inode->i_dev)) {
			((unsigned short *) (bh->b_data))[block>>9]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	if (!i)
		return 0;
	if (!(bh=bread(inode->i_dev,i)))
		return 0;
	i = ((unsigned short *)bh->b_data)[block&511];
	if (create && !i)
		if (i=new_block(inode->i_dev)) {
			((unsigned short *) (bh->b_data))[block&511]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	return i;
}

int bmap(struct m_inode * inode,int block)
{
	return _bmap(inode,block,0);
}

int create_block(struct m_inode * inode, int block)
{
	return _bmap(inode,block,1);	//通过分析文件的i节点中的信息以及文件的当前操作指针来确定需要操作的数据块。
}
		
void iput(struct m_inode * inode)
{
	if (!inode)
		return;
	wait_on_inode(inode);
	if (!inode->i_count)
		panic("iput: trying to free free inode");
	if (inode->i_pipe) {
		wake_up(&inode->i_wait);
		if (--inode->i_count)
			return;
		free_page(inode->i_size);
		inode->i_count=0;
		inode->i_dirt=0;
		inode->i_pipe=0;
		return;
	}
	if (!inode->i_dev) {
		inode->i_count--;
		return;
	}
	if (S_ISBLK(inode->i_mode)) {
		sync_dev(inode->i_zone[0]);
		wait_on_inode(inode);
	}
repeat:
	if (inode->i_count>1) {
		inode->i_count--;
		return;
	}
	if (!inode->i_nlinks) {	//i_nlinks被消减为0
		truncate(inode);	//删除的顺序必须是先删除xxx文件在硬盘中对应的数据块，然后才能删除xxx文件对应的i节点。
							//因为xxx文件究竟存储在硬盘的哪些块上，需要通过i节点才能找到。因此，如果先删除i节点，
							//数据块就找不到了，也就无法正常删除xxx文件对应的数据块了。
		//xxx文件对应的i节点有两份：一份在内存中，用于支持运算操作；一份在硬盘中，用于支持xxx文件在硬盘上的存储。
		//由于删除i节点本身也是一个运算过程，因此首先要根据内存中i节点的信息删除硬盘上xxx文件对应的i节点。方式也
		//同样是将高速缓冲区中该i节点对应的i节点位图清0。执行这一操作的函数是free_inode
		free_inode(inode);	//执行删除i节点自身的操作。
		
		return;
	}
	//解除xxx文件的i节点和系统i节点表inode_table[32]的关系
	if (inode->i_dirt) {
		write_inode(inode);	/* we can sleep - so do again */
		wait_on_inode(inode);
		goto repeat;
	}
	inode->i_count--;
	return;
}

struct m_inode * get_empty_inode(void)
{
	struct m_inode * inode;
	static struct m_inode * last_inode = inode_table;
	int i;

	//在i节点管理表中找到一个i_count字段为0的空闲项，并在找到该空闲项后，把它对应的存储区域清0，
	//这个动作的意义在于它实际上完成了内存中i节点管理结构的初始化。随后将该空闲项的引用计数置1，
	//表示它已被使用。
	do {
		inode = NULL;
		for (i = NR_INODE; i ; i--) {
			if (++last_inode >= inode_table + NR_INODE)
				last_inode = inode_table;
			if (!last_inode->i_count) {
				inode = last_inode;
				if (!inode->i_dirt && !inode->i_lock)
					break;
			}
		}
		if (!inode) {
			for (i=0 ; i<NR_INODE ; i++)
				printk("%04x: %6d\t",inode_table[i].i_dev,
					inode_table[i].i_num);
			panic("No free inodes in mem");
		}
		wait_on_inode(inode);
		while (inode->i_dirt) {
			write_inode(inode);
			wait_on_inode(inode);
		}
	} while (inode->i_count);
	memset(inode,0,sizeof(*inode));
	inode->i_count = 1;
	return inode;
}

struct m_inode * get_pipe_inode(void)
{
	struct m_inode * inode;

	if (!(inode = get_empty_inode()))
		return NULL;
	//由于管道的本质就是一个内存页面，所以，系统要申请一个空闲的内存页面，并将该页面的地址载入i节点。
	//请大家注意，此刻inode->i_size字段承载的不再是通常对于块设备文件来说以字节数表示的文件大小，
	//而是内存页面的起始地址
	if (!(inode->i_size=get_free_page())) {
		inode->i_count = 0;
		return NULL;
	}
	//管道i节点也要有引用计数。在linux 0.11中，默认操作这个管道文件的进程“能且仅能”有两个，一个是读
	//进程，一个是写进程，所以这里直接设置为2
	//之后，让读管道指针和写管道指针都指向管道的（其实就是这个空闲页面的）起始位置，以便将来读写管道
	//的进程操作，并将该i节点的属性设置为“管道型i节点”，以此来标识该i节点的特殊性，即它并不是实际存储
	//在硬盘上的文件的i节点，它只不过是一个内存页面。
	inode->i_count = 2;	/* sum of readers/writers */
	PIPE_HEAD(*inode) = PIPE_TAIL(*inode) = 0;
	inode->i_pipe = 1;
	return inode;
}

struct m_inode * iget(int dev,int nr)
{
	struct m_inode * inode, * empty;

	if (!dev)
		panic("iget with dev==0");
	empty = get_empty_inode();		//从i节点表inode_table中申请一个空闲的i节点位置，此时应该是首个i节点。

	//以“mnt”这个目录项中提供的i节点号为依托，遍历系统中的i节点管理表inode_table[32],通过识别设备号
	//和i节点号来确定这个i节点在inode_table[32]这个管理结构中是否已经存在
	inode = inode_table;
	while (inode < NR_INODE+inode_table) {
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode++;
			continue;
		}
		wait_on_inode(inode);

		
		//看看mnt这个目录文件的i节点之前是不是已经存在于该管理表中了，如果存在，就不用再读取了。
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode = inode_table;
			continue;
		}
		//如果i节点被找到，此次就不用再读盘了，直接将这个i节点的引用次数加1.
		inode->i_count++;

		//inode->i_mount这个标志为真，意味着mnt这个目录文件的i节点就是挂接点，既然是挂接点，就能在系统
		//的超级块管理表super_block[8]中找到具体是哪个设备的超级块中的s_imount与这个挂接点相挂接，进而
		//找到相应的超级块，现在这个超级块就是硬盘的超级块。
		if (inode->i_mount) {
			int i;

			for (i = 0 ; i<NR_SUPER ; i++)
				if (super_block[i].s_imount==inode)
					break;
			if (i >= NR_SUPER) {
				printk("Mounted inode hasn't got sb\n");
				//由于i节点被在inode_table[32]中找到。
				//释放掉刚刚申请的空闲i节点项,input(empty),并返回i节点指针
				if (empty)
					iput(empty);
				return inode;
			}
			iput(inode);
			dev = super_block[i].s_dev;	//根据硬盘超级块中的信息得到硬盘的设备号，
			nr = ROOT_INO;	//利用根目录文件i节点定位识别的特性得到硬盘根目录文件的i节点号，即ROOT_INO.
			inode = inode_table;
			continue;
		}
		if (empty)
			iput(empty);
		return inode;
	}
	if (!empty)
		return (NULL);
	inode=empty;		//对这个i节点进行初始化设置，其中包括i节点对应的设备号、该i节点的节点号等，
	inode->i_dev = dev;
	inode->i_num = nr;
	read_inode(inode);		//把i节点从虚拟盘上读取出来；
							//当挂接硬盘时，从硬盘读出该i节点，并载入到“在i节点管理表inode_table[32]中获取的空闲项上”
	return inode;
}

static void read_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);		//给i节点加锁，这样在解锁之前，这个i节点就不能被另行占用了，
							//之后，通过该i节点所在的超级块，来间接地计算出i节点所在的逻辑块号，
							//并将i节点所在的逻辑块，整个地读出来，从中提取这个i节点的信息，
							//将之载入到i节点管理表m_inode中由2.3.23节申请到的i节点位置上。
	if (!(sb=get_super(inode->i_dev)))
		panic("trying to read inode without dev");
	//下面语句就是根据i节点号找到i节点硬盘上所在块号的具体方法。
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK;
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");
	//当包含硬盘根目录i节点的数据块从硬盘读入到高速缓冲区后，就以i节点号为线索，将硬盘根目录i节点
	//的信息从缓冲块中提取出来并拷贝到刚找到的空闲i节点表项中
	*(struct d_inode *)inode =
		((struct d_inode *)bh->b_data)
			[(inode->i_num-1)%INODES_PER_BLOCK];
	brelse(bh);				//释放缓冲块并将锁定的i节点解锁
	unlock_inode(inode);		
}

static void write_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);
	if (!inode->i_dirt || !inode->i_dev) {
		unlock_inode(inode);
		return;
	}
	if (!(sb=get_super(inode->i_dev)))
		panic("trying to write inode without device");
	//先测算出该i节点在硬盘上逻辑块的位置，之后，就将该i节点所在的逻辑块读出来，，
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK;
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");
	//将该i节点写入这个逻辑块对应的缓冲块中，并将该缓冲块设置为脏，以些表示该缓冲块是需要同步到硬盘的，
	//这个缓冲块一同步，i节点也就被写入到硬盘了，最后，再将该i节点的"脏"标志设置为0，以表示这个i节点
	//已经同步过了，不需要同步了。
	((struct d_inode *)bh->b_data)
		[(inode->i_num-1)%INODES_PER_BLOCK] =
			*(struct d_inode *)inode;
	bh->b_dirt=1;
	inode->i_dirt=0;
	brelse(bh);
	unlock_inode(inode);
}

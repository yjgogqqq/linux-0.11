/*
 *  linux/fs/super.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * super.c contains code to handle the super-block tables.
 */
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include <errno.h>
#include <sys/stat.h>

int sync_dev(int dev);
void wait_for_keypress(void);

/* set_bit uses setb, as gas doesn't recognize setc */
#define set_bit(bitnr,addr) ({ \
register int __res __asm__("ax"); \
__asm__("bt %2,%3;setb %%al":"=a" (__res):"a" (0),"r" (bitnr),"m" (*(addr))); \
__res; })

struct super_block super_block[NR_SUPER];
/* this is initialized in init/main.c */
int ROOT_DEV = 0;

static void lock_super(struct super_block * sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sb->s_lock = 1;
	sti();
}

static void free_super(struct super_block * sb)
{
	cli();
	sb->s_lock = 0;
	wake_up(&(sb->s_wait));
	sti();
}

static void wait_on_super(struct super_block * sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sti();
}

struct super_block * get_super(int dev)
{
	struct super_block * s;

	if (!dev)
		return NULL;
	s = 0+super_block;
	while (s < NR_SUPER+super_block)
		if (s->s_dev == dev) {
			wait_on_super(s);
			if (s->s_dev == dev)
				return s;
			s = 0+super_block;
		} else
			s++;
	return NULL;
}

void put_super(int dev)
{
	struct super_block * sb;
	struct m_inode * inode;
	int i;

	if (dev == ROOT_DEV) {
		printk("root diskette changed: prepare for armageddon\n\r");
		return;
	}
	if (!(sb = get_super(dev)))
		return;
	if (sb->s_imount) {
		printk("Mounted disk changed - tssk, tssk\n\r");
		return;
	}
	lock_super(sb);
	sb->s_dev = 0;
	for(i=0;i<I_MAP_SLOTS;i++)
		brelse(sb->s_imap[i]);
	for(i=0;i<Z_MAP_SLOTS;i++)
		brelse(sb->s_zmap[i]);
	free_super(sb);
	return;
}

static struct super_block * read_super(int dev)
{
	struct super_block * s;
	struct buffer_head * bh;
	int i,block;

	if (!dev)
		return NULL;
	check_disk_change(dev);
	if (s = get_super(dev))		//检测这个超级块是不是已经被读进super_block中，如果已经被读进来了，则直接使用
								//这是因为，由于内存超级块管理结构一共有8项，系统中最多可以加载8个设备的文件系统，
								//如果事先已经针对某个设备的文件系统加载过了，就不用再加载一次了，这与通过哈希表来
								//检测缓冲块是否已经存在的道理是一样的。
		return s;
	for (s = 0+super_block ;; s++) {
		if (s >= NR_SUPER+super_block)
			return NULL;
		if (!s->s_dev)
			break;
	}
	//在super_block这个超级块控制结构中申请一个新的空间,然后进行初始化并加锁，准备把超级块读出来。
	//加锁是因为系统中的超级块管理结构super_block[8]属于公共资源，即所有进程都有可能依托系统操作它。
	//给其加锁，就是为了避免其他进程依托系统操作这个被选中的表项。
	s->s_dev = dev;
	s->s_isup = NULL;
	s->s_imount = NULL;
	s->s_time = 0;
	s->s_rd_only = 0;
	s->s_dirt = 0;
	lock_super(s);
	if (!(bh = bread(dev,1))) {		//把超级块从虚拟盘上读进缓冲区
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
	//超级块拷贝进缓冲块后，将这个超级块的数据加载进super_block这个超级块结构中。
	*((struct d_super_block *) s) =
		*((struct d_super_block *) bh->b_data);
	brelse(bh);	//释放缓冲块
	if (s->s_magic != SUPER_MAGIC) {	//判断读进来的这个超级块中魔数（SUPER_MAGIC）信息是否正确，
		s->s_dev = 0;
		free_super(s);
		return NULL;
	}
	//系统通过备份的超级块信息提供的信息，设置i节点位图管理结构s_imap和逻辑位图管理结构s_zmap，这两个成员
	//都是将来管理设备上文件的重要数据信息，它们都是绑定在super_block这个结构上面的成员，因此，也要调用bread函数
	//到虚拟盘上去读取。由于对它们的操作会比较频繁，于是这些占用的缓冲块并不被释放，以后它们将常驻在缓冲区内。
	//超级块通过指针与s_imap和s_zmap实现挂接
	for (i=0;i<I_MAP_SLOTS;i++)
		s->s_imap[i] = NULL;
	for (i=0;i<Z_MAP_SLOTS;i++)
		s->s_zmap[i] = NULL;
	block=2;
	for (i=0 ; i < s->s_imap_blocks ; i++)
		if (s->s_imap[i]=bread(dev,block))
			block++;
		else
			break;
	for (i=0 ; i < s->s_zmap_blocks ; i++)
		if (s->s_zmap[i]=bread(dev,block))
			block++;
		else
			break;
	if (block != 2+s->s_imap_blocks+s->s_zmap_blocks) {
		for(i=0;i<I_MAP_SLOTS;i++)
			brelse(s->s_imap[i]);
		for(i=0;i<Z_MAP_SLOTS;i++)
			brelse(s->s_zmap[i]);
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
	s->s_imap[0]->b_data[0] |= 1;
	s->s_zmap[0]->b_data[0] |= 1;
	free_super(s);
	return s;
}

int sys_umount(char * dev_name)
{
	struct m_inode * inode;
	struct super_block * sb;
	int dev;

	if (!(inode=namei(dev_name)))
		return -ENOENT;
	dev = inode->i_zone[0];
	if (!S_ISBLK(inode->i_mode)) {
		iput(inode);
		return -ENOTBLK;
	}
	iput(inode);
	if (dev==ROOT_DEV)
		return -EBUSY;
	if (!(sb=get_super(dev)) || !(sb->s_imount))
		return -ENOENT;
	if (!sb->s_imount->i_mount)
		printk("Mounted inode has i_mount=0\n");
	for (inode=inode_table+0 ; inode<inode_table+NR_INODE ; inode++)
		if (inode->i_dev==dev && inode->i_count)
				return -EBUSY;
	sb->s_imount->i_mount=0;
	iput(sb->s_imount);
	sb->s_imount = NULL;
	iput(sb->s_isup);
	sb->s_isup = NULL;
	put_super(dev);
	sync_dev(dev);
	return 0;
}

int sys_mount(char * dev_name, char * dir_name, int rw_flag)
{
	struct m_inode * dev_i, * dir_i;
	struct super_block * sb;
	int dev;

	if (!(dev_i=namei(dev_name)))	//获取设备的i节点。
		return -ENOENT;
	//此时已经得到了设备文件的i节点。namei函数返回后，通过对它的属性进行分析，
	//就可以得到此设备文件对应的设备号，这个设备号就是硬盘的设备号。
	dev = dev_i->i_zone[0];
	if (!S_ISBLK(dev_i->i_mode)) {
		iput(dev_i);
		return -EPERM;
	}
	iput(dev_i);					//设备文件的i节点已经使用完了，释放这个i节点
	//到这里为止，已经得到了hd1设备文件的设备号，即“hd1这个设备需要拿出来挂接的位置点”已经确定了，
	if (!(dir_i=namei(dir_name)))
		return -ENOENT;
	if (dir_i->i_count != 1 || dir_i->i_num == ROOT_INO) {
		iput(dir_i);
		return -EBUSY;
	}
	if (!S_ISDIR(dir_i->i_mode)) {
		iput(dir_i);
		return -EPERM;
	}
	//根据设备的设备号为参照，从硬盘上读取超级块。
	if (!(sb=read_super(dev))) {
		iput(dir_i);
		return -EBUSY;
	}
	if (sb->s_imount) {
		iput(dir_i);
		return -EBUSY;
	}
	if (dir_i->i_mount) {
		iput(dir_i);
		return -EPERM;
	}
	sb->s_imount=dir_i;		//将“从硬盘上找到的安装点”和“从虚拟盘上找到的被安装点”进行挂接
	//对i节点的属性进行设置。
	//i_mount表明了这个i节点上挂接着其他设备的超级块，这个i节点是“跨设备查找文件”的拐点
	dir_i->i_mount=1;
	dir_i->i_dirt=1;		/* NOTE! we don't iput(dir_i) */
	return 0;			/* we do that in umount */
}

void mount_root(void)
{
	int i,free;
	struct super_block * p;
	struct m_inode * mi;

	if (32 != sizeof (struct d_inode))
		panic("bad i-node size");
	for(i=0;i<NR_FILE;i++)			//对文件管理表file_table进行初始化
									//将来系统中所有进程操作的文件都要在这个表中登记，
									//因此要将该表中所有表项的“被引用次数”设置为0，使之全部成为空闲项以便以后使用
		file_table[i].f_count=0;
	if (MAJOR(ROOT_DEV) == 2) {
		printk("Insert root floppy and press ENTER");
		wait_for_keypress();
	}
	//初始化内存中的超级块管理结构super_block，这个结构一共有8项，这里要将每一项对应的设备号加锁标志和等待该设备解锁的进程标志，
	//全部设置为0。系统只要想和任何一个设备以文件的形式进行数据交互，都要将这个设备的超级块存储在super_block中，这样才能获取
	//这个设备中文件系统的最基本信息，根设备中的超级块也不例外。
	for(p = &super_block[0] ; p < &super_block[NR_SUPER] ; p++) {
		p->s_dev = 0;
		p->s_lock = 0;
		p->s_wait = NULL;
	}
	if (!(p=read_super(ROOT_DEV)))	//从虚拟盘中读取根文件系统的超级块并加载。
		panic("Unable to mount root");
	if (!(mi=iget(ROOT_DEV,ROOT_INO)))	//从虚拟盘中读取根i节点，它的意义在于：只要将来设备与系统建立了文件交互关系，
										//都可以通过这个根i节点来找到指定的文件。
		panic("Unable to read root i-node");
	mi->i_count += 3 ;	/* NOTE! it is logically used 4 times, not 1 */
	//将根文件系统的超级块中“被安装文件系统i节点s_isup”和“被安装到的i节点s_imount”都设置为这个根i节点
	//这样，将来系统在根设备上寻找文件时，就可以通过这里建立的关系，一步步的把文件找到。然后，再对进程1
	//管理结构中与文件系统i节点有关的字段进行设置，将来对根文件系统中存储的文件操作，就需要以此为起点。
	p->s_isup = p->s_imount = mi;
	current->pwd = mi;
	current->root = mi;
	free=0;
	i=p->s_nzones;
	//得到了根文件系统的超级块，就可以根据超级块中“逻辑位图”里记载的信息，计算出虚拟盘上数据块的占用与空闲情况，
	//并将此信息记录在本章2.3.22节中提到的“装载逻辑位图信息的缓冲块中”。
	while (-- i >= 0)
		if (!set_bit(i&8191,p->s_zmap[i>>13]->b_data))
			free++;
	printk("%d/%d free blocks\n\r",free,p->s_nzones);
	free=0;
	i=p->s_ninodes+1;
	//再根据超级块中“i节点位图”里记载的信息，计算出虚拟盘上i节点的占用与空闲的情况，并将此信息记录在本章2.3.22节中
	//提到的“装载i节点位图信息的缓冲块中”，
	while (-- i >= 0)
		if (!set_bit(i&8191,p->s_imap[i>>13]->b_data))
			free++;
	printk("%d/%d free inodes\n\r",free,p->s_ninodes);
}

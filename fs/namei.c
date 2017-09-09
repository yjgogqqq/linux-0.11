/*
 *  linux/fs/namei.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * Some corrections by tytso.
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <const.h>
#include <sys/stat.h>

#define ACC_MODE(x) ("\004\002\006\377"[(x)&O_ACCMODE])

/*
 * comment out this line if you want names > NAME_LEN chars to be
 * truncated. Else they will be disallowed.
 */
/* #define NO_TRUNCATE */

#define MAY_EXEC 1
#define MAY_WRITE 2
#define MAY_READ 4

/*
 *	permission()
 *
 * is used to check for read/write/execute permissions on a file.
 * I don't know if we should look at just the euid or both euid and
 * uid, but that should be easily changed.
 */
static int permission(struct m_inode * inode,int mask)
{
	int mode = inode->i_mode;

/* special case: not even root can read/write a deleted file */
	if (inode->i_dev && !inode->i_nlinks)
		return 0;
	else if (current->euid==inode->i_uid)
		mode >>= 6;
	else if (current->egid==inode->i_gid)
		mode >>= 3;
	if (((mode & mask & 0007) == mask) || suser())
		return 1;
	return 0;
}

/*
 * ok, we cannot use strncmp, as the name is not in our data space.
 * Thus we'll have to use match. No big problem. Match also makes
 * some sanity tests.
 *
 * NOTE! unlike strncmp, match returns 1 for success, 0 for failure.
 */
static int match(int len,const char * name,struct dir_entry * de)
{
	register int same __asm__("ax");

	if (!de || !de->inode || len > NAME_LEN)
		return 0;
	if (len < NAME_LEN && de->name[len])
		return 0;
	__asm__("cld\n\t"
		"fs ; repe ; cmpsb\n\t"
		"setz %%al"
		:"=a" (same)
		:"0" (0),"S" ((long) name),"D" ((long) de->name),"c" (len)
		:"cx","di","si");
	return same;
}

/*
 *	find_entry()
 *
 * finds an entry in the specified directory with the wanted name. It
 * returns the cache buffer in which the entry was found, and the entry
 * itself (as a parameter - res_dir). It does NOT read the inode of the
 * entry - you'll have to do that yourself if you want to.
 *
 * This also takes care of the few special cases due to '..'-traversal
 * over a pseudo-root and a mount point.
 */
static struct buffer_head * find_entry(struct m_inode ** dir,
	const char * name, int namelen, struct dir_entry ** res_dir)
{
	int entries;
	int block,i;
	struct buffer_head * bh;
	struct dir_entry * de;
	struct super_block * sb;

#ifdef NO_TRUNCATE
	if (namelen > NAME_LEN)
		return NULL;
#else
	if (namelen > NAME_LEN)
		namelen = NAME_LEN;
#endif
	//确定目录文件的大小，因为确定了大小就可以确定目录文件包含多少条目录项，
	//方法是通过根i节点中表示根目录文件大小的i_size字段除以每个目录项占用的字节数
	entries = (*dir)->i_size / (sizeof (struct dir_entry));
	*res_dir = NULL;
	if (!namelen)
		return NULL;
/* check for '..', as we might have to do some "magic" for it */
	if (namelen==2 && get_fs_byte(name)=='.' && get_fs_byte(name+1)=='.') {
/* '..' in a pseudo-root results in a faked '.' (just change namelen) */
		if ((*dir) == current->root)
			namelen=1;
		else if ((*dir)->i_num == ROOT_INO) {
/* '..' over a mount-point results in 'dir' being exchanged for the mounted
   directory-inode. NOTE! We set mounted, so that we can iput the new dir */
			sb=get_super((*dir)->i_dev);
			if (sb->s_imount) {
				iput(*dir);
				(*dir)=sb->s_imount;
				(*dir)->i_count++;
			}
		}
	}
	if (!(block = (*dir)->i_zone[0]))
		return NULL;
	//从目录文件的第一个数据块开始读取数据，将数据块读入到缓冲块，并调用match()函数
	//分析这些数据中有没有名字为mnt的目录项，如果有，目的就达到了；如果没有，就接着
	//从第二个数据块继续读取并分析，直到找到名字为xxx的目录项为止。
	if (!(bh = bread((*dir)->i_dev,block)))	//根目录文件在虚拟盘上，所以此时调用bread函数从设备上读取数据
											//并不会产生中断。
		return NULL;
	i = 0;
	de = (struct dir_entry *) bh->b_data;
	while (i < entries) {
		if ((char *)de >= BLOCK_SIZE+bh->b_data) {
			brelse(bh);
			bh = NULL;
			if (!(block = bmap(*dir,i/DIR_ENTRIES_PER_BLOCK)) ||
			    !(bh = bread((*dir)->i_dev,block))) {
				i += DIR_ENTRIES_PER_BLOCK;
				continue;
			}
			de = (struct dir_entry *) bh->b_data;
		}
		if (match(namelen,name,de)) {
			*res_dir = de;
			return bh;
		}
		de++;
		i++;
	}
	brelse(bh);
	return NULL;
}

/*
 *	add_entry()
 *
 * adds a file entry to the specified directory, using the same
 * semantics as find_entry(). It returns NULL if it failed.
 *
 * NOTE!! The inode part of 'de' is left at 0 - which means you
 * may not sleep between calling this and putting something into
 * the entry, as someone else might have used it while you slept.
 */
static struct buffer_head * add_entry(struct m_inode * dir,
	const char * name, int namelen, struct dir_entry ** res_dir)
{
	int block,i;
	struct buffer_head * bh;
	struct dir_entry * de;

	*res_dir = NULL;
#ifdef NO_TRUNCATE
	if (namelen > NAME_LEN)
		return NULL;
#else
	if (namelen > NAME_LEN)
		namelen = NAME_LEN;
#endif
	if (!namelen)
		return NULL;

	//先把user目录文件的第一个数据块读出来，看看能不能在里面创建目录项，
	if (!(block = dir->i_zone[0]))
		return NULL;
	if (!(bh = bread(dir->i_dev,block)))
		return NULL;
	i = 0;
	de = (struct dir_entry *) bh->b_data;

	//根据不同的情况寻找空闲位置，准备加载目录项，
	while (1) {
		//目录文件（很可能含有多个数据块）第一个数据块中没有空间加载目录项，那就把第二个数据块读出来；
		//如果还不能加载，就把第三个数据块读出来，以此类推。如果最后整个目录文件中的所有数据块都读出来了，
		//还是没有加载该目录项的空间，就新建一个数据块，准备加载目录项。
		if ((char *)de >= BLOCK_SIZE+bh->b_data) {
			brelse(bh);
			bh = NULL;
			block = create_block(dir,i/DIR_ENTRIES_PER_BLOCK);
			if (!block)
				return NULL;
			if (!(bh = bread(dir->i_dev,block))) {
				i += DIR_ENTRIES_PER_BLOCK;
				continue;
			}
			de = (struct dir_entry *) bh->b_data;
		}
		//整个目录文件中都没有空位来加载新的目录项，但最后一个数据块尾端还有空间加载该目录项，
		//那就准备在尾端加载一个目录项。
		if (i*sizeof(struct dir_entry) >= dir->i_size) {
			de->inode=0;
			dir->i_size = (i+1)*sizeof(struct dir_entry);
			dir->i_dirt = 1;
			dir->i_ctime = CURRENT_TIME;
		}
		//在遍历目录文件的目录项过程中，只要发现哪个目录项的i节点号为0（在Linux 0.11中，只要目录文件的
		//目录项中i节点号为0，就被认定为“空目录项”），就将新的目录项加载到这个空目录项所在的位置处。
		//先将xxx这个字符串载入目录项，以此作为该目录项的名字。然后，将该目录项所在的缓冲块的b_dirt标记设置为1，
		//以便将来从缓冲块中写到硬盘上，之后该函数返回。
		if (!de->inode) {
			dir->i_mtime = CURRENT_TIME;
			for (i=0; i < NAME_LEN ; i++)
				de->name[i]=(i<namelen)?get_fs_byte(name+i):0;
			bh->b_dirt = 1;
			*res_dir = de;
			return bh;
		}
		de++;
		i++;
	}
	brelse(bh);
	return NULL;
}

/*
 *	get_dir()
 *
 * Getdir traverses the pathname until it hits the topmost directory.
 * It returns NULL on failure.
 */
static struct m_inode * get_dir(const char * pathname)
{
	char c;
	const char * thisname;
	struct m_inode * inode;
	struct buffer_head * bh;
	int namelen,inr,idev;
	struct dir_entry * de;	//这个结构总共16个字节，其中inode代表这个目录项所对应的目录文件在硬盘上的i节点号
							//它占2个字节；name[NAME_LEN]是这个目录项的名字，占14个字节。
	//先检测这个进程在根i节点和当前目录指针方面的设置，
	if (!current->root || !current->root->i_count)
		panic("No root inode");
	if (!current->pwd || !current->pwd->i_count)
		panic("No cwd inode");
	//判断路径名的第一个字符是不是‘/’,如果是，说明这个路径名是绝对路径名，所以从根i节点开始查找操作，
	//然后获取当前进程的根i节点，这个根i节点，就是从虚拟盘上获得的那个根i节点。
	if ((c=get_fs_byte(pathname))=='/') {
		inode = current->root;
		pathname++;
	} else if (c)
		inode = current->pwd;
	else
		return NULL;	/* empty name is bad */
	inode->i_count++;
	while (1) {
		thisname = pathname;
		if (!S_ISDIR(inode->i_mode) || !permission(inode,MAY_EXEC)) {
			iput(inode);
			return NULL;
		}
		//对目录项进行分析，要在虚拟盘上找到这个目录项所在的逻辑块，并读进缓冲区中
		for(namelen=0;(c=get_fs_byte(pathname++))&&(c!='/');namelen++)	//解析字符串长度
			/* nothing */ ;
		if (!c)
			return inode;
		if (!(bh = find_entry(&inode,thisname,namelen,&de))) {	//根据xxx名字为参照在根目录文件中找到名字为xxx的目录项
			iput(inode);
			return NULL;
		}
		//通过被载入缓冲区的dev目录项，就可以找到它所对应目录文件的i节点号，
		//另外，再根据此时已知的i节点又能提取虚拟盘设备的设备号，这样，就可以把
		//dev目录中记录的关于dev目录文件的i节点，从虚拟盘中提取出来，并用这个提取出来的i节点
		//来覆盖已经获取的根i节点，这个i节点，将作为下一次路径解析的指引。
		inr = de->inode;		//得到i节点号
		idev = inode->i_dev;	//获得根设备号，即虚拟盘的设备号
		brelse(bh);
		iput(inode);
		if (!(inode = iget(idev,inr)))	//从虚拟盘中把mnt目录文件的i节点读出来，
			return NULL;
	}
}

/*
 *	dir_namei()
 *
 * dir_namei() returns the inode of the directory of the
 * specified name, and the name within that directory.
 */
static struct m_inode * dir_namei(const char * pathname,
	int * namelen, const char ** name)
{
	char c;
	const char * basename;
	struct m_inode * dir;

	//调用get_dir函数，准备针对“/dev/tty0”这个路径名进行具体的解析操作，并最终获得dev目录文件的i节点
	if (!(dir = get_dir(pathname)))	
		return NULL;
	basename = pathname;
	while (c=get_fs_byte(pathname++))
		if (c=='/')
			basename=pathname;
	*namelen = pathname-basename-1;
	*name = basename;
	return dir;
}

/*
 *	namei()
 *
 * is used by most simple commands to get the inode of a specified name.
 * Open, link etc use their own routines, but this is enough for things
 * like 'chmod' etc.
 */
struct m_inode * namei(const char * pathname)
{
	const char * basename;
	int inr,dev,namelen;
	struct m_inode * dir;
	struct buffer_head * bh;
	struct dir_entry * de;

	if (!(dir = dir_namei(pathname,&namelen,&basename)))	//获取目录i节点
		return NULL;
	if (!namelen)			/* special case: '/usr/' etc */
		return dir;
	bh = find_entry(&dir,basename,namelen,&de);		//根据目录文件的i节点把dev目录文件从设备上读入到缓冲块中
	if (!bh) {
		iput(dir);
		return NULL;
	}

	//备份设备的i节点号和虚拟盘的设备号，以便最张将设备文件的i节点读取出来。
	inr = de->inode;
	dev = dir->i_dev;
	//备份后，缓冲块中的目录文件已经没有使用价值，同样目录文件的i节点也没有使用价值了，
	//所以要释放它们，以便节省资源。
	brelse(bh);
	iput(dir);
	//通过备份的设备的i节点号和虚拟盘的设备号，获取设备文件的i节点
	dir=iget(dev,inr);
	if (dir) {
		dir->i_atime=CURRENT_TIME;
		dir->i_dirt=1;
	}
	return dir;
}

/*
 *	open_namei()
 *
 * namei for open - this is in fact almost the whole open-routine.
 */
int open_namei(const char * pathname, int flag, int mode,
	struct m_inode ** res_inode)
{
	const char * basename;
	int inr,dev,namelen;
	struct m_inode * dir, *inode;
	struct buffer_head * bh;
	struct dir_entry * de;

	if ((flag & O_TRUNC) && !(flag & O_ACCMODE))
		flag |= O_WRONLY;
	mode &= 0777 & ~current->umask;
	mode |= I_REGULAR;
	//枝梢i节点就是准备查找的目标i节点前的最后一个i节点
	//对路径名进行解析，找到“枝梢i节点”.对于“/dev/tty0”这个路径名而言，找到枝梢i节点就是要找到dev这个目录文件的i节点，
	//因为tty0这个目录项就在dev这个目录文件里面（目录文件与普通文件有所区别，它里面装载的信息都是一个一个目录项，
	//这些目录项，都将作为查找指定文件的索引信息），所以这一步是为下面进而找到tty0这个目录项做准备的。
	if (!(dir = dir_namei(pathname,&namelen,&basename)))	
		return -ENOENT;
	if (!namelen) {			/* special case: '/usr/' etc */
		if (!(flag & (O_ACCMODE|O_CREAT|O_TRUNC))) {
			*res_inode=dir;
			return 0;
		}
		iput(dir);
		return -EISDIR;
	}
	//通过获取到的枝梢i节点、剩余路径名，以及剩余路径名字符串的长度，从虚拟盘中把dev这个目录文件中
	//的tty0这个目录项所在的逻辑块载入缓冲区。
	bh = find_entry(&dir,basename,namelen,&de);	//没有相关文件的目录项时，bh一定是Null
	if (!bh) {
		if (!(flag & O_CREAT)) { //如果文件不存在，且用户不创建文件，则返回错误信息
			iput(dir);
			return -ENOENT;
		}
		if (!permission(dir,MAY_WRITE)) {
			iput(dir);
			return -EACCES;
		}
		inode = new_inode(dir->i_dev);	//创建一个新的i节点
		if (!inode) {
			iput(dir);
			return -ENOSPC;
		}
		inode->i_uid = current->euid;
		inode->i_mode = mode;
		inode->i_dirt = 1;
		bh = add_entry(dir,basename,namelen,&de);	//为xxx文件创建目录项
		if (!bh) {
			inode->i_nlinks--;
			iput(inode);
			iput(dir);
			return -ENOSPC;
		}
		//此时，目录项已经定位，字符串也设置了，但还差i节点号没有设置，目录项还不完整。因些，在add_entry函数
		//返回后，还要将XXX文件创建的i节点的节点号载入到XXX目录项中，
		de->inode = inode->i_num;
		bh->b_dirt = 1;
		brelse(bh);
		iput(dir);
		*res_inode = inode;
		return 0;
	}
	//把目录文件载入后，就可以通过tty0这个目录项获取tty0这个文件的i节点号，通过dir所记录的i节点
	//也可以获取虚拟盘的设备号，这样，最终通过调用iget函数就可以得到tty0这个设备文件的i节点了，
	//执行完iget函数后，tty0文件的i节点已经被加载到了inode_table。
	inr = de->inode;
	dev = dir->i_dev;
	brelse(bh);
	iput(dir);
	if (flag & O_EXCL)
		return -EEXIST;
	if (!(inode=iget(dev,inr)))
		return -EACCES;
	//对获取的tty0设备文件的i节点属性进行检测，看它是否可用，并设置当前时间为i节点访问时间。
	//在2.1.6节中，系统设置了开机启动时间。
	//当前时间是这样定义的：#define CURRENT_TIME (startup_time+jiffies/HZ)
	//可见这个当前时间也是依据开机启动时间startup_time计算出来的。
	//最后，就把这个i节点与实参的指针相挂接，通过这种方式将tty0的i节点返回给主调函数sys_open。
	if ((S_ISDIR(inode->i_mode) && (flag & O_ACCMODE)) ||
	    !permission(inode,ACC_MODE(flag))) {
		iput(inode);
		return -EPERM;
	}
	inode->i_atime = CURRENT_TIME;
	if (flag & O_TRUNC)
		truncate(inode);
	*res_inode = inode;
	return 0;
}

int sys_mknod(const char * filename, int mode, int dev)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh;
	struct dir_entry * de;
	
	if (!suser())
		return -EPERM;
	if (!(dir = dir_namei(filename,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		return -EPERM;
	}
	bh = find_entry(&dir,basename,namelen,&de);
	if (bh) {
		brelse(bh);
		iput(dir);
		return -EEXIST;
	}
	inode = new_inode(dir->i_dev);
	if (!inode) {
		iput(dir);
		return -ENOSPC;
	}
	inode->i_mode = mode;
	if (S_ISBLK(mode) || S_ISCHR(mode))
		inode->i_zone[0] = dev;
	inode->i_mtime = inode->i_atime = CURRENT_TIME;
	inode->i_dirt = 1;
	bh = add_entry(dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		inode->i_nlinks=0;
		iput(inode);
		return -ENOSPC;
	}
	de->inode = inode->i_num;
	bh->b_dirt = 1;
	iput(dir);
	iput(inode);
	brelse(bh);
	return 0;
}

int sys_mkdir(const char * pathname, int mode)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh, *dir_block;
	struct dir_entry * de;

	if (!suser())
		return -EPERM;
	if (!(dir = dir_namei(pathname,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		return -EPERM;
	}
	bh = find_entry(&dir,basename,namelen,&de);
	if (bh) {
		brelse(bh);
		iput(dir);
		return -EEXIST;
	}
	inode = new_inode(dir->i_dev);
	if (!inode) {
		iput(dir);
		return -ENOSPC;
	}
	inode->i_size = 32;
	inode->i_dirt = 1;
	inode->i_mtime = inode->i_atime = CURRENT_TIME;
	if (!(inode->i_zone[0]=new_block(inode->i_dev))) {
		iput(dir);
		inode->i_nlinks--;
		iput(inode);
		return -ENOSPC;
	}
	inode->i_dirt = 1;
	if (!(dir_block=bread(inode->i_dev,inode->i_zone[0]))) {
		iput(dir);
		free_block(inode->i_dev,inode->i_zone[0]);
		inode->i_nlinks--;
		iput(inode);
		return -ERROR;
	}
	de = (struct dir_entry *) dir_block->b_data;
	de->inode=inode->i_num;
	strcpy(de->name,".");
	de++;
	de->inode = dir->i_num;
	strcpy(de->name,"..");
	inode->i_nlinks = 2;
	dir_block->b_dirt = 1;
	brelse(dir_block);
	inode->i_mode = I_DIRECTORY | (mode & 0777 & ~current->umask);
	inode->i_dirt = 1;
	bh = add_entry(dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		free_block(inode->i_dev,inode->i_zone[0]);
		inode->i_nlinks=0;
		iput(inode);
		return -ENOSPC;
	}
	de->inode = inode->i_num;
	bh->b_dirt = 1;
	dir->i_nlinks++;
	dir->i_dirt = 1;
	iput(dir);
	iput(inode);
	brelse(bh);
	return 0;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
static int empty_dir(struct m_inode * inode)
{
	int nr,block;
	int len;
	struct buffer_head * bh;
	struct dir_entry * de;

	len = inode->i_size / sizeof (struct dir_entry);
	if (len<2 || !inode->i_zone[0] ||
	    !(bh=bread(inode->i_dev,inode->i_zone[0]))) {
	    	printk("warning - bad directory on dev %04x\n",inode->i_dev);
		return 0;
	}
	de = (struct dir_entry *) bh->b_data;
	if (de[0].inode != inode->i_num || !de[1].inode || 
	    strcmp(".",de[0].name) || strcmp("..",de[1].name)) {
	    	printk("warning - bad directory on dev %04x\n",inode->i_dev);
		return 0;
	}
	nr = 2;
	de += 2;
	while (nr<len) {
		if ((void *) de >= (void *) (bh->b_data+BLOCK_SIZE)) {
			brelse(bh);
			block=bmap(inode,nr/DIR_ENTRIES_PER_BLOCK);
			if (!block) {
				nr += DIR_ENTRIES_PER_BLOCK;
				continue;
			}
			if (!(bh=bread(inode->i_dev,block)))
				return 0;
			de = (struct dir_entry *) bh->b_data;
		}
		if (de->inode) {
			brelse(bh);
			return 0;
		}
		de++;
		nr++;
	}
	brelse(bh);
	return 1;
}

int sys_rmdir(const char * name)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh;
	struct dir_entry * de;

	if (!suser())
		return -EPERM;
	if (!(dir = dir_namei(name,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		return -EPERM;
	}
	bh = find_entry(&dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		return -ENOENT;
	}
	if (!(inode = iget(dir->i_dev, de->inode))) {
		iput(dir);
		brelse(bh);
		return -EPERM;
	}
	if ((dir->i_mode & S_ISVTX) && current->euid &&
	    inode->i_uid != current->euid) {
		iput(dir);
		iput(inode);
		brelse(bh);
		return -EPERM;
	}
	if (inode->i_dev != dir->i_dev || inode->i_count>1) {
		iput(dir);
		iput(inode);
		brelse(bh);
		return -EPERM;
	}
	if (inode == dir) {	/* we may not delete ".", but "../dir" is ok */
		iput(inode);
		iput(dir);
		brelse(bh);
		return -EPERM;
	}
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		iput(dir);
		brelse(bh);
		return -ENOTDIR;
	}
	if (!empty_dir(inode)) {
		iput(inode);
		iput(dir);
		brelse(bh);
		return -ENOTEMPTY;
	}
	if (inode->i_nlinks != 2)
		printk("empty directory has nlink!=2 (%d)",inode->i_nlinks);
	de->inode = 0;
	bh->b_dirt = 1;
	brelse(bh);
	inode->i_nlinks=0;
	inode->i_dirt=1;
	dir->i_nlinks--;
	dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	dir->i_dirt=1;
	iput(dir);
	iput(inode);
	return 0;
}

int sys_unlink(const char * name)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh;
	struct dir_entry * de;

	//先要通过路径名把xxx文件的管理信息读出来，
	if (!(dir = dir_namei(name,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	//获取目录文件的i节点后，接下来就要检测是否具备往这个目录文件中写入数据的权限，
	//因为xxx这个目录项就在这个目录文件里面，如果没有写入权限，也就意味着无法把xxx的
	//目录项从目录中删除了，后续工作都无从谈起
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		return -EPERM;
	}
	//得知可以操作目录文件后，就从这个目录文件中把xxx这个目录项读出来，并最终提取到xxx文件的i节点。
	bh = find_entry(&dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		return -ENOENT;
	}
	if (!(inode = iget(dir->i_dev, de->inode))) {
		iput(dir);
		brelse(bh);
		return -ENOENT;
	}
	//针对目录文件的i节点和xxx文件的i节点进行检测，先要检测用户是否有删除该文件的权限。
	if ((dir->i_mode & S_ISVTX) && !suser() &&
	    current->euid != inode->i_uid &&
	    current->euid != dir->i_uid) {
		iput(dir);
		iput(inode);
		brelse(bh);
		return -EPERM;
	}
	//判断，如果xxx文件是目录文件，则不可以删除。这是因为，一个目录文件里面会有其他目录项，如果目录文件
	//删除了，其他目录项也就找不到了，这些目录项所对应的文件，将会永远消失。当然，linux 0.11是允许删除目录的，
	//内核会有其他的函数专门处理这个问题，但在这里不可以删除。
	if (S_ISDIR(inode->i_mode)) {
		iput(inode);
		iput(dir);
		brelse(bh);
		return -EPERM;
	}
	//判断xxx文件的i节点的链接数是否为0，如果为0，就发出警告，并设置为1。这是因为，在整个系统中，至少当前
	//进程还是在操作该i节点，所以它的链接数至少是1，肯定不能是0，所以要进行这样的调整。
	if (!inode->i_nlinks) {
		printk("Deleting nonexistent file (%04x:%d), %d\n",
			inode->i_dev,inode->i_num,inode->i_nlinks);
		inode->i_nlinks=1;
	}
	//只要目录项中的i节点号置0，系统就认定该目录项为空闲项，即为空。
	de->inode = 0;
	//只要b_dirt的标志置1，就表示 这个缓冲块将来要同步到硬盘上，这就意味着，xxx这个目录项失效的信息也会传达
	//硬盘上，从而真正让删除操作在硬盘上落实。
	bh->b_dirt = 1;
	brelse(bh);
	//当前进程删除了xxx文件，因此，文件对应的i节点的链接数inode->i_nlinks应该减少1，从而完成“通过路径名无法找到”
	//的第一层含义；
	inode->i_nlinks--;
	//之后，再对xxx文件的i节点进行一些设置，包括将i节点的b_dirt标志置1，表示该i节点已经改动过，需要同步到硬盘上。
	inode->i_dirt = 1;
	//将当前时间认定为该i节点的改动时间
	inode->i_ctime = CURRENT_TIME;
	iput(inode);
	iput(dir);
	return 0;
}

int sys_link(const char * oldname, const char * newname)
{
	struct dir_entry * de;
	struct m_inode * oldinode, * dir;
	struct buffer_head * bh;
	const char * basename;
	int namelen;

	oldinode=namei(oldname);
	if (!oldinode)
		return -ENOENT;
	if (S_ISDIR(oldinode->i_mode)) {
		iput(oldinode);
		return -EPERM;
	}
	dir = dir_namei(newname,&namelen,&basename);
	if (!dir) {
		iput(oldinode);
		return -EACCES;
	}
	if (!namelen) {
		iput(oldinode);
		iput(dir);
		return -EPERM;
	}
	if (dir->i_dev != oldinode->i_dev) {
		iput(dir);
		iput(oldinode);
		return -EXDEV;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		iput(oldinode);
		return -EACCES;
	}
	bh = find_entry(&dir,basename,namelen,&de);
	if (bh) {
		brelse(bh);
		iput(dir);
		iput(oldinode);
		return -EEXIST;
	}
	bh = add_entry(dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		iput(oldinode);
		return -ENOSPC;
	}
	de->inode = oldinode->i_num;
	bh->b_dirt = 1;
	brelse(bh);
	iput(dir);
	oldinode->i_nlinks++;
	oldinode->i_ctime = CURRENT_TIME;
	oldinode->i_dirt = 1;
	iput(oldinode);
	return 0;
}

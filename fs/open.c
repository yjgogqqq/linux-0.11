/*
 *  linux/fs/open.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <utime.h>
#include <sys/stat.h>

#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/kernel.h>
#include <asm/segment.h>

int sys_ustat(int dev, struct ustat * ubuf)
{
	return -ENOSYS;
}

int sys_utime(char * filename, struct utimbuf * times)
{
	struct m_inode * inode;
	long actime,modtime;

	if (!(inode=namei(filename)))
		return -ENOENT;
	if (times) {
		actime = get_fs_long((unsigned long *) &times->actime);
		modtime = get_fs_long((unsigned long *) &times->modtime);
	} else
		actime = modtime = CURRENT_TIME;
	inode->i_atime = actime;
	inode->i_mtime = modtime;
	inode->i_dirt = 1;
	iput(inode);
	return 0;
}

/*
 * XXX should we use the real or effective uid?  BSD uses the real uid,
 * so as to make this call useful to setuid programs.
 */
int sys_access(const char * filename,int mode)
{
	struct m_inode * inode;
	int res, i_mode;

	mode &= 0007;
	if (!(inode=namei(filename)))
		return -EACCES;
	i_mode = res = inode->i_mode & 0777;
	iput(inode);
	if (current->uid == inode->i_uid)
		res >>= 6;
	else if (current->gid == inode->i_gid)
		res >>= 6;
	if ((res & 0007 & mode) == mode)
		return 0;
	/*
	 * XXX we are doing this test last because we really should be
	 * swapping the effective with the real user id (temporarily),
	 * and then calling suser() routine.  If we do call the
	 * suser() routine, it needs to be called last. 
	 */
	if ((!current->uid) &&
	    (!(mode & 1) || (i_mode & 0111)))
		return 0;
	return -EACCES;
}

int sys_chdir(const char * filename)
{
	struct m_inode * inode;

	if (!(inode = namei(filename)))
		return -ENOENT;
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		return -ENOTDIR;
	}
	iput(current->pwd);
	current->pwd = inode;
	return (0);
}

int sys_chroot(const char * filename)
{
	struct m_inode * inode;

	if (!(inode=namei(filename)))
		return -ENOENT;
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		return -ENOTDIR;
	}
	iput(current->root);
	current->root = inode;
	return (0);
}

int sys_chmod(const char * filename,int mode)
{
	struct m_inode * inode;

	if (!(inode=namei(filename)))
		return -ENOENT;
	if ((current->euid != inode->i_uid) && !suser()) {
		iput(inode);
		return -EACCES;
	}
	inode->i_mode = (mode & 07777) | (inode->i_mode & ~07777);
	inode->i_dirt = 1;
	iput(inode);
	return 0;
}

int sys_chown(const char * filename,int uid,int gid)
{
	struct m_inode * inode;

	if (!(inode=namei(filename)))
		return -ENOENT;
	if (!suser()) {
		iput(inode);
		return -EACCES;
	}
	inode->i_uid=uid;
	inode->i_gid=gid;
	inode->i_dirt=1;
	iput(inode);
	return 0;
}

int sys_open(const char * filename,int flag,int mode)
{
	struct m_inode * inode;
	struct file * f;
	int i,fd;

	mode &= 0777 & ~current->umask;
	//在进程1的管理结构中的“文件打开控制结构*filp[20]”里申请一个空闲项。
	//遍历当前进程管理结构task_struct中的文件管理指针表*filp[20]中的每一项，
	//找到一个指针记录为空的项，哪一项指针记录为空，就表明该项为空闲项。
	for(fd=0 ; fd<NR_OPEN ; fd++)
		if (!current->filp[fd])
			break;
	if (fd>=NR_OPEN)
		return -EINVAL;
	current->close_on_exec &= ~(1<<fd);
	//在系统中的文件管理结构file_table中也为该文件申请一个空闲项。
	//通过遍历内核文件管理表file_table[64]中的每一项，找到一个引用计数f_count为0的项，
	//哪一项f_count为0，就表明该项为空闲项。
	f=0+file_table;
	for (i=0 ; i<NR_FILE ; i++,f++)
		if (!f->f_count) break;
	if (i>=NR_FILE)
		return -EINVAL;
	//把两者进行挂接，并将文件管理结构file_table中的文件引用次数加1，表明这个位置所对应的文件已经被使用一次了。
	//这样进程1就与文件管理结构file_table建立了关系，将来tty0文件成功打开以后，将登记在这个在文件管理结构file_table中
	//与进程挂接的空闲项上，这样进程1就具备了操作tty0文件的能力了。
	(current->filp[fd]=f)->f_count++;
	if ((i=open_namei(filename,flag,mode,&inode))<0) {	//open_namei函数，开始实质性的打开文件操作
														//这个函数的主要目的就是找到这个终端设备文件的i节点，
														//因为找到了这个文件的i节点，就可以找到这个文件的全部内容。
		current->filp[fd]=NULL;
		f->f_count=0;
		return i;
	}
	//对tty0的i节点属性进行分析，判断出该文件是不是字符设备文件，tty0文件确实是字符设备文件，
	//那么，通过采集这个i节点中的设备号得知，设备号是“4”。于是就设置当前进程的tty号为该i节点的次设备号，
	//并设置进程1 tty对应的tty_table表项的父进程组号等于进程1的父进程组号，这个设置是为多用户使用操作系统做准备的。
/* ttys are somewhat special (ttyxx major==4, tty major==5) */
	if (S_ISCHR(inode->i_mode))
		if (MAJOR(inode->i_zone[0])==4) {
			if (current->leader && current->tty<0) {
				current->tty = MINOR(inode->i_zone[0]);
				tty_table[current->tty].pgrp = current->pgrp;
			}
		} else if (MAJOR(inode->i_zone[0])==5)
			if (current->tty<0) {
				iput(inode);
				current->filp[fd]=NULL;
				f->f_count=0;
				return -EPERM;
			}
/* Likewise with block-devices: check for floppy_change */
	if (S_ISBLK(inode->i_mode))
		check_disk_change(inode->i_zone[0]);
	//让tty0这个设备文件的i节点与本章2.4.1节中文件管理结构file_table里面新申请的空闲项相挂接，这样，
	//当前进程就可以操作这个设备文件了。另外，对于这个空闲项，还要初始化一些属性，包括，文件属性、标志、
	//引用次数，i节点字段，以及文件读写指针等，并最终返回文件句柄。
	f->f_mode = inode->i_mode;
	f->f_flags = flag;
	f->f_count = 1;
	f->f_inode = inode;
	f->f_pos = 0;
	return (fd);
	//至此，标志着进程1已经具备了对设备文件tty0的操作能力，
	//这个文件打开的动作使进程1管理结构中的*filp[20]的第一项与系统的文件管理表建立了关系，这个关系将作为标准输入设备。
}

int sys_creat(const char * pathname, int mode)
{
	return sys_open(pathname, O_CREAT | O_TRUNC, mode);
}

int sys_close(unsigned int fd)
{	
	struct file * filp;

	if (fd >= NR_OPEN)
		return -EINVAL;
	current->close_on_exec &= ~(1<<fd);
	if (!(filp = current->filp[fd]))
		return -EINVAL;
	//将当前进程的管理结构task_struct中的文件管理指针表*filp[20]与系统中的文件管理表file_table[64]解除关系，
	current->filp[fd] = NULL;
	if (filp->f_count == 0)
		panic("Close: file count is 0");
	//将系统中的文件管理表file_table[64]中XXX文件的引用次数减1。
	if (--filp->f_count)
		return (0);
	//释放XXX文件的i节点
	iput(filp->f_inode);
	return (0);
}

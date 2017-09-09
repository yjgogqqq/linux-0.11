/*
 *  linux/fs/fcntl.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <string.h>
#include <errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#include <fcntl.h>
#include <sys/stat.h>

extern int sys_close(int fd);

static int dupfd(unsigned int fd, unsigned int arg)
{
	//先进行检测，如果没有非正常情况出现，才开始准备复制句柄
	if (fd >= NR_OPEN || !current->filp[fd])
		return -EBADF;
	if (arg >= NR_OPEN)
		return -EINVAL;
	//接下来开始复制句柄，复制文件句柄并不改变内核中file_table结构的情况，
	//只是改变当前进程的*filp[20]中的相关结构。
	//在当前的情况下，循环中只会执行一次arg++；所以，执行后结果为1，于是就把当前进程中文件管理结构的第一项的
	//数据全部复制到第二项的位置处，并让这个被复制的文件句柄的引用次数加1，dupfd函数执行完毕，并最终返回到了
	//init函数中去执行，此次句柄的复制，使进程1管理结构中的*filp[20]这个文件管理结构，又增添了一组与tty0对应
	//的关系，它将作为“标准输出设备”，为终端控制台提供支持。
	while (arg < NR_OPEN)		//第二次进入dupfd函数时，current->filp结构中已经有两个句柄符合条件了
								//（这是上一次复制句柄导致的结果），所以在这个循环中while(arg<NR_OPEN)
								//和arg++；被执行了两次，这就会把第一项文件句柄的全部信息再复制给第三项
								//所在的位置，导致进程1管理结构的文件结构中有了三个相同的文件句柄了，它们
								//都与刚才打开的tty0这个设备文件在file_table中的对应项相绑定。
								//此次句柄的复制，使进程1管理结构中的*filp[20]这个文件管理结构在第一次复制的基础
								//上，再次增添了一组与tty0对应的关系，它将作为“标准错误输出设备”，为终端控制台
		if (current->filp[arg])
			arg++;
		else
			break;
	if (arg >= NR_OPEN)
		return -EMFILE;
	current->close_on_exec &= ~(1<<arg);
	(current->filp[arg] = current->filp[fd])->f_count++;
	return arg;
}

int sys_dup2(unsigned int oldfd, unsigned int newfd)
{
	sys_close(newfd);
	return dupfd(oldfd,newfd);
}

int sys_dup(unsigned int fildes)
{
	return dupfd(fildes,0);
}

int sys_fcntl(unsigned int fd, unsigned int cmd, unsigned long arg)
{	
	struct file * filp;

	if (fd >= NR_OPEN || !(filp = current->filp[fd]))
		return -EBADF;
	switch (cmd) {
		case F_DUPFD:
			return dupfd(fd,arg);
		case F_GETFD:
			return (current->close_on_exec>>fd)&1;
		case F_SETFD:
			if (arg&1)
				current->close_on_exec |= (1<<fd);
			else
				current->close_on_exec &= ~(1<<fd);
			return 0;
		case F_GETFL:
			return filp->f_flags;
		case F_SETFL:
			filp->f_flags &= ~(O_APPEND | O_NONBLOCK);
			filp->f_flags |= arg & (O_APPEND | O_NONBLOCK);
			return 0;
		case F_GETLK:	case F_SETLK:	case F_SETLKW:
			return -1;
		default:
			return -1;
	}
}

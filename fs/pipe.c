/*
 *  linux/fs/pipe.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <signal.h>

#include <linux/sched.h>
#include <linux/mm.h>	/* for get_free_page */
#include <asm/segment.h>

int read_pipe(struct m_inode * inode, char * buf, int count)
{
	int chars, size, read = 0;

	while (count>0) {
		//管道内没有任何数据时，系统会将读管道进程挂起
		while (!(size=PIPE_SIZE(*inode))) {
			wake_up(&inode->i_wait);
			if (inode->i_count != 2) /* are there any writers? */
				return read;
			sleep_on(&inode->i_wait);
		}
		chars = PAGE_SIZE-PIPE_TAIL(*inode);
		if (chars > count)
			chars = count;
		if (chars > size)
			chars = size;
		count -= chars;
		read += chars;
		size = PIPE_TAIL(*inode);
		PIPE_TAIL(*inode) += chars;
		PIPE_TAIL(*inode) &= (PAGE_SIZE-1);
		while (chars-->0)
			put_fs_byte(((char *)inode->i_size)[size++],buf++);
	}
	wake_up(&inode->i_wait);
	return read;
}
	
int write_pipe(struct m_inode * inode, char * buf, int count)
{
	int chars, size, written = 0;

	while (count>0) {
		while (!(size=(PAGE_SIZE-1)-PIPE_SIZE(*inode))) {
			wake_up(&inode->i_wait);
			if (inode->i_count != 2) { /* no readers */
				current->signal |= (1<<(SIGPIPE-1));
				return written?written:-1;
			}
			sleep_on(&inode->i_wait);
		}
		chars = PAGE_SIZE-PIPE_HEAD(*inode);
		if (chars > count)
			chars = count;
		if (chars > size)
			chars = size;
		count -= chars;
		written += chars;
		size = PIPE_HEAD(*inode);
		PIPE_HEAD(*inode) += chars;
		PIPE_HEAD(*inode) &= (PAGE_SIZE-1);
		while (chars-->0)
			((char *)inode->i_size)[size++]=get_fs_byte(buf++);
	}
	wake_up(&inode->i_wait);
	return written;
}

int sys_pipe(unsigned long * fildes)
{
	struct m_inode * inode;
	struct file * f[2];
	int fd[2];
	int i,j;

	j=0;
	//为创建管道文件而在文件管理表中申请两个空闲项
	for(i=0;j<2 && i<NR_FILE;i++)
		if (!file_table[i].f_count)
			(f[j++]=i+file_table)->f_count++;
	if (j==1)
		f[0]->f_count=0;
	if (j<2)
		return -1;
	j=0;
	//以file_table[64]中这两个表项为基础，在当前进程管理结构的文件管理结构*filp[20]中申请两个空闲项，
	//分别与系统中的文件管理表file_table[64]的这两个表项相挂接，这样当前进程文件结构*filp[20]中就有
	//两个表项与flie_table[64]建立关系了，等到它作为父进程创建子进程时，*filp[20]中的这两个表项就会
	//自然而然地拷贝给它的子进程，使之也“天然地”和file_table[64]结构中同样的管道文件表项建立了关系，
	//我们所说的这对父子进程就是将来要操作管道的“写管道进程”和“读管道进程”
	for(i=0;j<2 && i<NR_OPEN;i++)
		if (!current->filp[i]) {
			current->filp[ fd[j]=i ] = f[j];
			j++;
		}
	if (j==1)
		current->filp[fd[0]]=NULL;
	if (j<2) {
		f[0]->f_count=f[1]->f_count=0;
		return -1;
	}
	//调用get_pipe_inode函数为该管道文件在i节点管理表inode_table[32]中申请一个i节点
	if (!(inode=get_pipe_inode())) {
		current->filp[fd[0]] =
			current->filp[fd[1]] = NULL;
		f[0]->f_count = f[1]->f_count = 0;
		return -1;
	}
	//初始化两个文件管理表中的空闲项，让它们都指向这个管道i节点，文件读写指针都指向管道的起始位置。
	//第1个空闲项的文件模式置为读，第2个空闲项的文件模式置为写
	f[0]->f_inode = f[1]->f_inode = inode;
	f[0]->f_pos = f[1]->f_pos = 0;
	f[0]->f_mode = 1;		/* read */
	f[1]->f_mode = 2;		/* write */
	//将管道文件的两个句柄返回给用户进程，即返回实例1代码中的fd[2],这个数组有两项，每一项分别存放一个句柄，
	//这样父子两个进程就可以通过不同的文件句柄操作这个管道文件了。
	put_fs_long(fd[0],0+fildes);
	put_fs_long(fd[1],1+fildes);
	return 0;
}

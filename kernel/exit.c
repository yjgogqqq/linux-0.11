/*
 *  linux/kernel/exit.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/tty.h>
#include <asm/segment.h>

int sys_pause(void);
int sys_close(int fd);

void release(struct task_struct * p)
{
	int i;

	if (!p)
		return;
	for (i=1 ; i<NR_TASKS ; i++)
		if (task[i]==p) {
			task[i]=NULL;	//与进程槽的关系解除，进程槽这一项就成了空闲项；
			free_page((long)p);	//将进程的管理结构所占用的页面释放掉，至此，进程退出的善后工作结束
			schedule();
			return;
		}
	panic("trying to release non-existent task");
}

static inline int send_sig(long sig,struct task_struct * p,int priv)
{
	if (!p || sig<1 || sig>32)
		return -EINVAL;
	if (priv || (current->euid==p->euid) || suser())
		p->signal |= (1<<(sig-1));
	else
		return -EPERM;
	return 0;
}

static void kill_session(void)
{
	struct task_struct **p = NR_TASKS + task;
	
	while (--p > &FIRST_TASK) {
		if (*p && (*p)->session == current->session)
			(*p)->signal |= 1<<(SIGHUP-1);
	}
}

/*
 * XXX need to check permissions needed to send signals to process
 * groups, etc. etc.  kill() permissions semantics are tricky!
 */
int sys_kill(int pid,int sig)
{
	struct task_struct **p = NR_TASKS + task;
	int err, retval = 0;

	if (!pid) while (--p > &FIRST_TASK) {
		if (*p && (*p)->pgrp == current->pid) 
			if (err=send_sig(sig,*p,1))
				retval = err;
	} else if (pid>0) while (--p > &FIRST_TASK) {
		if (*p && (*p)->pid == pid) 
			if (err=send_sig(sig,*p,0))
				retval = err;
	} else if (pid == -1) while (--p > &FIRST_TASK)
		if (err = send_sig(sig,*p,0))
			retval = err;
	else while (--p > &FIRST_TASK)
		if (*p && (*p)->pgrp == -pid)
			if (err = send_sig(sig,*p,0))
				retval = err;
	return retval;
}

static void tell_father(int pid)
{
	int i;

	if (pid)
		for (i=0;i<NR_TASKS;i++) {
			if (!task[i])
				continue;
			if (task[i]->pid != pid)
				continue;
			task[i]->signal |= (1<<(SIGCHLD-1));
			return;
		}
/* if we don't find any fathers, we just release ourselves */
/* This is not really OK. Must change it to make father 1 */
	printk("BAD BAD - no father found\n\r");
	release(current);
}

int do_exit(long code)
{
	int i;

	//释放当前进程代码段和数据段所占的内存页面和页表。这样，shell程序所占用的内存页面次数将被减1。
	free_page_tables(get_base(current->ldt[1]),get_limit(0x0f));
	free_page_tables(get_base(current->ldt[2]),get_limit(0x17));
	for (i=0 ; i<NR_TASKS ; i++)
		if (task[i] && task[i]->father == current->pid) {//判断shell是否有子进程，子进程update
			//shell进程要退出，意味着update进程父进程即将不存在了，所以要给它重新找一个父进程
			task[i]->father = 1;	
			if (task[i]->state == TASK_ZOMBIE)
				/* assumption task[1] is always init */
				(void) send_sig(SIGCHLD, task[1], 1);
		}

	//对退出进程的管理结构进行处理，比如：关闭当前进程打开着的所有文件，对当前进程的工作目录pwd、
	//根目录root及当前程序所对应的可执行文件的i节点executable执行同步操作，并分别置空（释放），
	//还要将这个当前进程的状态设置为僵死，等等。
	for (i=0 ; i<NR_OPEN ; i++)
		if (current->filp[i])
			sys_close(i);
	iput(current->pwd);
	current->pwd=NULL;
	iput(current->root);
	current->root=NULL;
	iput(current->executable);
	current->executable=NULL;
	if (current->leader && current->tty >= 0)
		tty_table[current->tty].pgrp = 0;
	if (last_task_used_math == current)
		last_task_used_math = NULL;
	if (current->leader)
		kill_session();
	current->state = TASK_ZOMBIE;
	current->exit_code = code;

	//调用tell_father函数向它的父进程——进程1发送“子进程终止”信号，并进行进程切换。
	tell_father(current->father);
	schedule();
	//到这里为止，shell进程的善后工作基本完成了，还剩下它自身的进程管理结构所占用的页面没有被清理掉，
	//这个清理工作，将由它的父进程——进程1来完成。
	return (-1);	/* just to suppress warnings */
}

int sys_exit(int error_code)
{
	return do_exit((error_code&0xff)<<8);
}


int sys_waitpid(pid_t pid,unsigned long * stat_addr, int options)
{
	int flag, code;
	struct task_struct ** p;

	verify_area(stat_addr,4);
repeat:
	flag=0;
	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p) {
		if (!*p || *p == current)
			continue;
		if ((*p)->father != current->pid) //该函数先要对所有的进程进行遍历，看看哪个进程的父进程是进程1，
											//这样进程2就被初步选定了。
			continue;
		if (pid>0) {				//对进程2进行分析，判断进程2究竟是不是进程1要找的子进程
			if ((*p)->pid != pid)
				continue;
		} else if (!pid) {
			if ((*p)->pgrp != current->pgrp)
				continue;
		} else if (pid != -1) {
			if ((*p)->pgrp != -pid)
				continue;
		}
		switch ((*p)->state) {					//根据进程2的状态来分析是不是要给flag的数值赋1
			case TASK_STOPPED:
				if (!(options & WUNTRACED))
					continue;
				put_fs_long(0x7f,stat_addr);
				return (*p)->pid;
			case TASK_ZOMBIE:					//进程状态是僵死状态。
				current->cutime += (*p)->utime;
				current->cstime += (*p)->stime;
				flag = (*p)->pid;
				code = (*p)->exit_code;
				release(*p);		//将进程退出的善后工作彻底完成。
				put_fs_long(code,stat_addr);
				return flag;		//返回退出的进程号
			default:
				flag=1;
				continue;
		}
	}
	if (flag) {											
		if (options & WNOHANG)
			return 0;
		current->state=TASK_INTERRUPTIBLE;	//将当前进程，设置为可中断等待状态
		schedule();							//切换进程

		//如果进程1等待的子进程确实退出了，就跳转到repeat标号处执行，
		//这样,sys_waitpid函数的主体内容将再次被执行一次。
		if (!(current->signal &= ~(1<<(SIGCHLD-1))))
			goto repeat;
		else
			return -EINTR;
	}
	return -ECHILD;
}



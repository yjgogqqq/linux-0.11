/*
 *  linux/kernel/signal.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#include <signal.h>

volatile void do_exit(int error_code);

int sys_sgetmask()
{
	return current->blocked;
}

int sys_ssetmask(int newmask)
{
	int old=current->blocked;

	current->blocked = newmask & ~(1<<(SIGKILL-1));
	return old;
}

static inline void save_old(char * from,char * to)
{
	int i;

	verify_area(to, sizeof(struct sigaction));
	for (i=0 ; i< sizeof(struct sigaction) ; i++) {
		put_fs_byte(*from,to);
		from++;
		to++;
	}
}

static inline void get_new(char * from,char * to)
{
	int i;

	for (i=0 ; i< sizeof(struct sigaction) ; i++)
		*(to++) = get_fs_byte(from++);
}

int sys_signal(int signum, long handler, long restorer)
{
	struct sigaction tmp;

	//检测用户指定的信号是否合理
	if (signum<1 || signum>32 || signum==SIGKILL)
		return -1;
	//开始对进程管理结构中的sigaction[32]结构进行设置
	tmp.sa_handler = (void (*)(int)) handler;	//这行代码实施了绑定工作，handler参数就是进程中
												//"signal(SIGUSR1,sig_usr)"这行代码的sig_usr函数的地址，
												//该设置意味着，只要将来进程接到信号，就由sig_usr这个函数来处理
	tmp.sa_mask = 0;
	tmp.sa_flags = SA_ONESHOT | SA_NOMASK;
	tmp.sa_restorer = (void (*)(void)) restorer;
	handler = (long) current->sigaction[signum-1].sa_handler;
	current->sigaction[signum-1] = tmp;	//这行代码确定了将由sigaction[signum-1]这一项为SIGUSR1信号提供服务
	return handler;
}

int sys_sigaction(int signum, const struct sigaction * action,
	struct sigaction * oldaction)
{
	struct sigaction tmp;

	if (signum<1 || signum>32 || signum==SIGKILL)
		return -1;
	tmp = current->sigaction[signum-1];
	get_new((char *) action,
		(char *) (signum-1+current->sigaction));
	if (oldaction)
		save_old((char *) &tmp,(char *) oldaction);
	if (current->sigaction[signum-1].sa_flags & SA_NOMASK)
		current->sigaction[signum-1].sa_mask = 0;
	else
		current->sigaction[signum-1].sa_mask |= (1<<(signum-1));
	return 0;
}

void do_signal(long signr,long eax, long ebx, long ecx, long edx,
	long fs, long es, long ds,
	long eip, long cs, long eflags,
	unsigned long * esp, long ss)
{
	unsigned long sa_handler;
	long old_eip=eip;
	struct sigaction * sa = current->sigaction + signr - 1;
	int longs;
	unsigned long * tmp_esp;

	//
	sa_handler = (unsigned long) sa->sa_handler;
	if (sa_handler==1)
		return;
	if (!sa_handler) {
		if (signr==SIGCHLD)
			return;
		else
			do_exit(1<<(signr-1));
	}
	//修改内核栈和用户栈的过程
	if (sa->sa_flags & SA_ONESHOT)
		sa->sa_handler = NULL;
	*(&eip) = sa_handler;	//这一行是在系统调用返回后对eip位置的调整，使其指向processig进程的信号处理函数sig_usr
	longs = (sa->sa_flags & SA_NOMASK)?7:8;
	*(&esp) -= longs;	//这一行是对“用户栈”空间的栈顶指针esp进行调整，使栈顶指针向栈底的反方向移动，以便接下来
						//在用户栈空间中备份数据
	verify_area(esp,longs*4);
	tmp_esp=esp;
	put_fs_long((long) sa->sa_restorer,tmp_esp++);
	put_fs_long(signr,tmp_esp++);
	if (!(sa->sa_flags & SA_NOMASK))
		put_fs_long(current->blocked,tmp_esp++);
	put_fs_long(eax,tmp_esp++);
	put_fs_long(ecx,tmp_esp++);
	put_fs_long(edx,tmp_esp++);
	put_fs_long(eflags,tmp_esp++);
	put_fs_long(old_eip,tmp_esp++);
	current->blocked |= sa->sa_mask;
}

/*
 *  linux/fs/exec.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * #!-checking implemented by tytso.
 */

/*
 * Demand-loading implemented 01.12.91 - no need to read anything but
 * the header into memory. The inode of the executable is put into
 * "current->executable", and page faults do the actual loading. Clean.
 *
 * Once more I can proudly say that linux stood up to being changed: it
 * was less than 2 hours work to get demand-loading completely implemented.
 */

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <a.out.h>

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/segment.h>

extern int sys_exit(int exit_code);
extern int sys_close(int fd);

/*
 * MAX_ARG_PAGES defines the number of pages allocated for arguments
 * and envelope for the new program. 32 should suffice, this gives
 * a maximum env+arg of 128kB !
 */
#define MAX_ARG_PAGES 32

/*
 * create_tables() parses the env- and arg-strings in new user
 * memory and creates the pointer tables from them, and puts their
 * addresses on the "stack", returning the new stack pointer value.
 */
static unsigned long * create_tables(char * p,int argc,int envc)
{
	unsigned long *argv,*envp;
	unsigned long * sp;

	sp = (unsigned long *) (0xfffffffc & (unsigned long) p);
	sp -= envc+1;
	envp = sp;
	sp -= argc+1;
	argv = sp;
	put_fs_long((unsigned long)envp,--sp);
	put_fs_long((unsigned long)argv,--sp);
	put_fs_long((unsigned long)argc,--sp);
	while (argc-->0) {
		put_fs_long((unsigned long) p,argv++);
		while (get_fs_byte(p++)) /* nothing */ ;
	}
	put_fs_long(0,argv);
	while (envc-->0) {
		put_fs_long((unsigned long) p,envp++);
		while (get_fs_byte(p++)) /* nothing */ ;
	}
	put_fs_long(0,envp);
	return sp;
}

/*
 * count() counts the number of arguments/envelopes
 */
static int count(char ** argv)
{
	int i=0;
	char ** tmp;

	if (tmp = argv)
		while (get_fs_long((unsigned long *) (tmp++)))
			i++;

	return i;
}

/*
 * 'copy_string()' copies argument/envelope strings from user
 * memory to free pages in kernel mem. These are in a format ready
 * to be put directly into the top of new user memory.
 *
 * Modified by TYT, 11/24/91 to add the from_kmem argument, which specifies
 * whether the string and the string array are from user or kernel segments:
 * 
 * from_kmem     argv *        argv **
 *    0          user space    user space
 *    1          kernel space  user space
 *    2          kernel space  kernel space
 * 
 * We do this by playing games with the fs segment register.  Since it
 * it is expensive to load a segment register, we try to avoid calling
 * set_fs() unless we absolutely have to.
 */
static unsigned long copy_strings(int argc,char ** argv,unsigned long *page,
		unsigned long p, int from_kmem)
{
	char *tmp, *pag;
	int len, offset = 0;
	unsigned long old_fs, new_fs;

	if (!p)
		return 0;	/* bullet-proofing */
	new_fs = get_ds();
	old_fs = get_fs();
	if (from_kmem==2)
		set_fs(new_fs);
	while (argc-- > 0) {
		if (from_kmem == 1)
			set_fs(new_fs);
		if (!(tmp = (char *)get_fs_long(((unsigned long *)argv)+argc)))
			panic("argc is wrong");
		if (from_kmem == 1)
			set_fs(old_fs);
		len=0;		/* remember zero-padding */
		do {
			len++;
		} while (get_fs_byte(tmp++));
		if (p-len < 0) {	/* this shouldn't happen - 128kB */
			set_fs(old_fs);
			return 0;
		}
		while (len) {
			--p; --tmp; --len;
			if (--offset < 0) {
				offset = p % PAGE_SIZE;
				if (from_kmem==2)
					set_fs(old_fs);
				if (!(pag = (char *) page[p/PAGE_SIZE]) &&
				    !(pag = (char *) page[p/PAGE_SIZE] =
				      (unsigned long *) get_free_page())) 
					return 0;
				if (from_kmem==2)
					set_fs(new_fs);

			}
			*(pag + offset) = get_fs_byte(tmp);
		}
	}
	if (from_kmem==2)
		set_fs(old_fs);
	return p;
}

static unsigned long change_ldt(unsigned long text_size,unsigned long * page)
{
	unsigned long code_limit,data_limit,code_base,data_base;
	int i;

	//代码段限长被设置为与xxx.c文件的代码段长度相同。数据段长度设置为0x4000000，即64MB，
	//之后再将参数与环境变量所在的物理页面映射到str1这个进程的线性地址空间内。
	code_limit = text_size+PAGE_SIZE -1;
	code_limit &= 0xFFFFF000;
	data_limit = 0x4000000;
	code_base = get_base(current->ldt[1]);
	data_base = code_base;
	set_base(current->ldt[1],code_base);
	set_limit(current->ldt[1],code_limit);
	set_base(current->ldt[2],data_base);
	set_limit(current->ldt[2],data_limit);
/* make sure fs points to the NEW data segment */
	__asm__("pushl $0x17\n\tpop %%fs"::);
	data_base += data_limit;
	for (i=MAX_ARG_PAGES-1 ; i>=0 ; i--) {
		data_base -= PAGE_SIZE;
		if (page[i])
			put_page(page[i],data_base);
	}
	return data_limit;
}

/*
 * 'do_execve()' executes a new program.
 */
int do_execve(unsigned long * eip,long tmp,char * filename,
	char ** argv, char ** envp)
{
	struct m_inode * inode;
	struct buffer_head * bh;
	struct exec ex;
	unsigned long page[MAX_ARG_PAGES];
	int i,argc,envc;
	int e_uid, e_gid;
	int retval;
	int sh_bang = 0;
	unsigned long p=PAGE_SIZE*MAX_ARG_PAGES-4;

	if ((0xffff & eip[1]) != 0x000f)
		panic("execve called from supervisor mode");
	//为管理新进程参数和环境变量所占用的页面做准备，设置page管理结构并初始化清0。
	for (i=0 ; i<MAX_ARG_PAGES ; i++)	/* clear page-table */
		page[i]=0;
	//读取这个i节点的目的就是为了对这个进程的文件进行检测，看其是否具备载入条件
	if (!(inode=namei(filename)))		/* get executables inode */
		return -ENOENT;
	//把参数和环境变量的个数统计出来
	argc = count(argv);
	envc = count(envp);
	
restart_interp:
	//接下来对这个i节点进行分析，包括分析它是不是常规文件，以及对一些与用户有关的属性信息进行分析。
	if (!S_ISREG(inode->i_mode)) {	/* must be regular file */
		retval = -EACCES;
		goto exec_error2;
	}
	i = inode->i_mode;
	e_uid = (i & S_ISUID) ? inode->i_uid : current->euid;
	e_gid = (i & S_ISGID) ? inode->i_gid : current->egid;
	if (current->euid == inode->i_uid)
		i >>= 6;
	else if (current->egid == inode->i_gid)
		i >>= 3;
	if (!(i & 1) &&
	    !((inode->i_mode & 0111) && suser())) {
		retval = -ENOEXEC;
		goto exec_error2;
	}

	//将文件的文件头从硬盘上载入缓冲块中，以便进一步分析。
	if (!(bh = bread(inode->i_dev,inode->i_zone[0]))) {
		retval = -EACCES;
		goto exec_error2;
	}

	//文件头进入缓冲区后，对这个文件头进行备份。
	ex = *((struct exec *) bh->b_data);	/* read exec-header */
	//对这个文件头进行分析，检查该文件是不是脚本文件，
	if ((bh->b_data[0] == '#') && (bh->b_data[1] == '!') && (!sh_bang)) {
		/*
		 * This section does the #! interpretation.
		 * Sorta complicated, but hopefully it will work.  -TYT
		 */

		char buf[1023], *cp, *interp, *i_name, *i_arg;
		unsigned long old_fs;

		strncpy(buf, bh->b_data+2, 1022);
		brelse(bh);
		iput(inode);
		buf[1022] = '\0';
		if (cp = strchr(buf, '\n')) {
			*cp = '\0';
			for (cp = buf; (*cp == ' ') || (*cp == '\t'); cp++);
		}
		if (!cp || *cp == '\0') {
			retval = -ENOEXEC; /* No interpreter name found */
			goto exec_error1;
		}
		interp = i_name = cp;
		i_arg = 0;
		for ( ; *cp && (*cp != ' ') && (*cp != '\t'); cp++) {
 			if (*cp == '/')
				i_name = cp+1;
		}
		if (*cp) {
			*cp++ = '\0';
			i_arg = cp;
		}
		/*
		 * OK, we've parsed out the interpreter name and
		 * (optional) argument.
		 */
		if (sh_bang++ == 0) {
			p = copy_strings(envc, envp, page, p, 0);
			p = copy_strings(--argc, argv+1, page, p, 0);
		}
		/*
		 * Splice in (1) the interpreter's name for argv[0]
		 *           (2) (optional) argument to interpreter
		 *           (3) filename of shell script
		 *
		 * This is done in reverse order, because of how the
		 * user environment and arguments are stored.
		 */
		p = copy_strings(1, &filename, page, p, 1);
		argc++;
		if (i_arg) {
			p = copy_strings(1, &i_arg, page, p, 2);
			argc++;
		}
		p = copy_strings(1, &i_name, page, p, 2);
		argc++;
		if (!p) {
			retval = -ENOMEM;
			goto exec_error1;
		}
		/*
		 * OK, now restart the process with the interpreter's inode.
		 */
		old_fs = get_fs();
		set_fs(get_ds());
		if (!(inode=namei(interp))) { /* get executables inode */
			set_fs(old_fs);
			retval = -ENOENT;
			goto exec_error1;
		}
		set_fs(old_fs);
		goto restart_interp;
	}
	brelse(bh);
	//如果执行文件不是需要分页可执行文件（ZMAGIC）、代码重定位部分长度a_trsize不等于0、
	//数据重定位信息长度不等于0、代码段+数据段+堆段长度超过50MB、i节点表明的该执行文件
	//长度小于代码段+数据段+符号表长度+执行头部分长度的总和，则这个shell文件就不能执行。
	if (N_MAGIC(ex) != ZMAGIC || ex.a_trsize || ex.a_drsize ||
		ex.a_text+ex.a_data+ex.a_bss>0x3000000 ||
		inode->i_size < ex.a_text+ex.a_data+ex.a_syms+N_TXTOFF(ex)) {
		retval = -ENOEXEC;
		goto exec_error2;
	}
	// 如果文件头部分长度不等于一个缓冲块大小（1024字节），也不能执行。
	if (N_TXTOFF(ex) != BLOCK_SIZE) {
		printk("%s: N_TXTOFF != BLOCK_SIZE. See a.out.h.", filename);
		retval = -ENOEXEC;
		goto exec_error2;
	}
	if (!sh_bang) {
		//两次调用copy_strings函数，分别将环境变量和参数拷贝到主内存指定的页面中，
		//拷贝的时候要参考环境变量和参数在前面各自统计出来的个数，并依托前面设置的页面管理结构
		//page[MAX_ARG_PAGES]和偏移指针p.
		p = copy_strings(envc,envp,page,p,0);
		p = copy_strings(argc,argv,page,p,0);
		if (!p) {
			retval = -ENOMEM;
			goto exec_error2;
		}
	}
/* OK, This is the point of no return */
	//对str1自身的进程管理结构进行调整，先要更改该文件的executable字段，
	//这个字段就是str1进程对应的可执行文件的i节点。
	if (current->executable)
		iput(current->executable);
	current->executable = inode;
	//将与文件和信号相关的数据信息进行调整
	//与信号相关的信息全部清0，这是因为每个进程都会接收各自的信号，这些都是进程私有的信息，
	//彼此之间不能交叉使用。
	for (i=0 ; i<32 ; i++)
		current->sigaction[i].sa_handler = NULL;
	for (i=0 ; i<NR_OPEN ; i++)
		if ((current->close_on_exec>>i)&1)	//如果进程在执行时打开了哪些文件，就都以“位”的形式记录在close_on_exec
											//这个成员中。
			sys_close(i);
	current->close_on_exec = 0;
	//将原先代码段和数据段所占用的页面及相关的管理页表释放掉。
	free_page_tables(get_base(current->ldt[1]),get_limit(0x0f));
	free_page_tables(get_base(current->ldt[2]),get_limit(0x17));
	if (last_task_used_math == current)		//last_task_used_math协处理器使用标志。
											//如果这个标志对应的是当前进程，那就一定要将该标志清空，
											//这是因为当前进程肯定没有与协处理器发生过关系。
											//另外，进程管理信息中也有协处理器使用的标志，也要清0；
		last_task_used_math = NULL;
	current->used_math = 0;

	//根据文件头提供的信息重新设置xx程序代码段的描述符以及重新设置数据段描述符
	p += change_ldt(ex.a_text,page)-MAX_ARG_PAGES*PAGE_SIZE;	//实现局部数据描述符表的重新设置
	//创建环境变量和参数的指针表，并将其地址存放到“环境变量和参数所在的”主内存区的页面中。
	p = (unsigned long) create_tables((char *)p,argc,envc);		//新建指针管理表：针对参数与环境变量
	//对用户进程管理结构中的brk、start_stack等信息进行设置。
	current->brk = ex.a_bss +				//进程堆结尾字段brk
		(current->end_data = ex.a_data +	//进程代码段尾值字段end_code	
		(current->end_code = ex.a_text));	//进程数据段字段end_data
	current->start_stack = p & 0xfffff000;	//栈底位置字段start_tack
	current->euid = e_uid;					//有效用户ID euid
	current->egid = e_gid;					//有效组ID egid
	i = ex.a_text+ex.a_data;
	while (i&0xfff)
		put_fs_byte(0,(char *) (i++));		//将主内存中BSS段的一页面数据全部清0。
	//对eip进行设置，使其指向xxx的栈底；对eip进行设置，使得程序一旦执行，就会发生缺页中断。
	eip[0] = ex.a_entry;		/* eip, magic happens :-) */
	eip[3] = p;			/* stack pointer */
	return 0;
exec_error2:
	iput(inode);
exec_error1:
	for (i=0 ; i<MAX_ARG_PAGES ; i++)
		free_page(page[i]);
	return(retval);
}

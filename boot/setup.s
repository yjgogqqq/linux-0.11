!
!	setup.s		(C) 1991 Linus Torvalds
!
! setup.s is responsible for getting the system data from the BIOS,
! and putting them into the appropriate places in system memory.
! both setup.s and system has been loaded by the bootblock.
!
! This code asks the bios for memory/disk/other parameters, and
! puts them in a "safe" place: 0x90000-0x901FF, ie where the
! boot-block used to be. It is then up to the protected mode
! system to read them from there before the area is overwritten
! for buffer-blocks.
!

! NOTE! These had better be the same as in bootsect.s!

INITSEG  = 0x9000	! we move boot here - out of the way
SYSSEG   = 0x1000	! system loaded at 0x10000 (65536).
SETUPSEG = 0x9020	! this is the current segment

.globl begtext, begdata, begbss, endtext, enddata, endbss
.text
begtext:
.data
begdata:
.bss
begbss:
.text

entry start
start:

!BIOS中断类型相关：http://blog.chinaunix.net/uid-27033491-id-3239348.html

! ok, the read went well so we get current cursor position and save it for
! posterity.

	mov	ax,#INITSEG	! this is done in bootsect already, but...
				! 将 ds 置成#INITSEG(0x9000)。这已经在 bootsect 程序中
				! 设置过，但是现在是 setup 程序，Linus 觉得需要再重新
				! 设置一下。
	mov	ds,ax
	mov	ah,#0x03	! read cursor pos
				! 功能描述：在文本坐标下，读取光标各种信息
				! 入口参数：		AH＝03H
				! 		BH＝显示页码
				! 出口参数：		CH＝光标的起始行
				! 		CL＝光标的终止行
				!		DH＝行(Y坐标)(0x00是顶端)
				!		DL＝列(X坐标)(0x00是左边)
	xor	bh,bh
	int	0x10		! 功能调用可以进行屏幕设置。AH中存放功能号，并在指定寄存器中存放入口参数
				！save it in known place, con_init fetches
	mov	[0],dx		! it from 0x90000.  地址基于DS.
				! 上两句是说将光标位置信息存放在 0x90000 处，控制台
				! 初始化时会来取。

! Get memory size (extended mem, kB)

	mov	ah,#0x88	! 功能描述：读取扩展内存大小
				! 入口参数：AH＝88H
				! 出口参数：AX＝扩展内存字节数(以K为单位)；从 0x100000（1M）处开始的扩展内存大小(KB)。
				! 若出错则 CF 置位，ax = 出错码。
	int	0x15		! 杂项系统服务(Miscellaneous System Service——INT 15H) 
	mov	[2],ax		! 将扩展内存数值存在 0x90002 处（1 个字）。

! Get video-card data:

 	mov	ah,#0x0f	! 功能描述：读取显示器模式
 				! 入口参数：		AH＝0FH
 				! 出口参数：		AH＝屏幕字符的列数
 				! 		AL＝显示模式(参见功能00H中的说明)
 				! 		BH＝页码
	int	0x10		! 0x90004(1 字)存放当前页，0x90006 显示模式，0x90007 字符列数。
	mov	[4],bx		! bh = display page
	mov	[6],ax		! al = video mode, ah = window width

! check for EGA/VGA and some config parameters	! 检查显示方式（EGA/VGA）并取参数。
				! 调用 BIOS 中断 0x10，附加功能选择 -取方式信息
	mov	ah,#0x12	! 功能描述：显示器的配置中断。
	mov	bl,#0x10	! 10H — 读取配置信息
				! 返回：bh = 显示状态
				! (0x00 - 彩色模式，I/O 端口=0x3dX)
				! (0x01 - 单色模式，I/O 端口=0x3bX)
				! bl = 安装的显示内存
				! (0x00 - 64k, 0x01 - 128k, 0x02 - 192k, 0x03 = 256k)
				! cx = 显示卡特性参数(参见程序后的说明)。
	int	0x10
	mov	[8],ax
	mov	[10],bx		! 0x9000A = 安装的显示内存， 0x9000B = 显示状态(彩色/单色)
	mov	[12],cx		! 0x9000C = 显示卡特性参数。

! Get hd0 data					! 取第一个硬盘的信息（复制硬盘参数表）。
						! 第 1 个硬盘参数表的首地址竟然是中断向量 0x41 的向量值！而第 2 个硬盘
						! 参数表紧接第 1 个表的后面，中断向量 0x46 的向量值也指向这第 2 个硬盘
						! 的参数表首址。表的长度是 16 个字节(0x10)。
						! 下面两段程序分别复制 BIOS 有关两个硬盘的参数表，0x90080 处存放第 1 个
						! 硬盘的表，0x90090 处存放第 2 个硬盘的表。

	mov	ax,#0x0000
	mov	ds,ax
	lds	si,[4*0x41]			! 取中断向量 0x41 的值，也即 hd0 参数表的地址Îds:si
	mov	ax,#INITSEG
	mov	es,ax
	mov	di,#0x0080			! 传输的目的地址: 0x9000:0x0080 Î es:di
	mov	cx,#0x10			! 共传输 0x10 字节。
	rep
	movsb					! MOVSB即字符串传送指令，这条指令按字节传送数据。
						! 通过SI和DI这两个寄存器控制字符串的源地址和目标地址，
						! 比如DS:SI这段地址的N个字节复制到ES:DI指向的地址，
						! 复制后DS:SI的内容保持不变。

! Get hd1 data

	mov	ax,#0x0000
	mov	ds,ax
	lds	si,[4*0x46]			! 取中断向量 0x46 的值，也即 hd1 参数表的地址Îds:si
	mov	ax,#INITSEG
	mov	es,ax		
	mov	di,#0x0090			! 传输的目的地址: 0x9000:0x0090 Î es:di
	mov	cx,#0x10
	rep
	movsb					! MOVSB即字符串传送指令，这条指令按字节传送数据。
						! 通过SI和DI这两个寄存器控制字符串的源地址和目标地址，
						! 比如DS:SI这段地址的N个字节复制到ES:DI指向的地址，
						! 复制后DS:SI的内容保持不变。


! Check that there IS a hd1 :-)			! 检查系统是否存在第 2 个硬盘，如果不存在则第 2 个表清零。
						! 利用 BIOS 中断调用 0x13 的取盘类型功能
						! 功能号 ah = 0x15；
						! 输入：dl = 驱动器号（0x8X 是硬盘：0x80 指第 1 个硬盘，0x81 第 2 个硬盘）
						! 输出：ah = 类型码；00 --没有这个盘，CF 置位； 01 --是软驱，没有 change-line 支持；
	mov	ax,#0x01500
	mov	dl,#0x81
	int	0x13
	jc	no_disk1
	cmp	ah,#3				! 是硬盘吗？(类型 = 3 ？)。
	je	is_disk1
no_disk1:
	mov	ax,#INITSEG			! 第 2 个硬盘不存在，则对第 2 个硬盘表清零
	mov	es,ax
	mov	di,#0x0090
	mov	cx,#0x10
	mov	ax,#0x00
	rep
	stosb					! 该指令为单字符输出指令，调用该指令后，
						! 可以将累加器AL中的值传递到当前ES段的DI地址处，
						! 并且根据DF的值来影响DI的值，如果DF为0，则调用该指令后，DI自增1，
is_disk1:

! now we want to move to protected mode ...

	cli			! no interrupts allowed !

! first we move the system to it's rightful place
! bootsect 引导程序是将 system 模块读入到从 0x10000（64k）开始的位置。由于当时假设
! system 模块最大长度不会超过 0x80000（512k），也即其末端不会超过内存地址 0x90000，
! 所以 bootsect 会将自己移动到 0x90000 开始的地方，并把 setup 加载到它的后面。
! 下面这段程序的用途是再把整个 system 模块移动到 0x00000 位置，即把从 0x10000 到 0x8ffff
! 的内存数据块(512k)，整块地向内存低端移动了 0x10000（64k）的位置。
	mov	ax,#0x0000
	cld			! 'direction'=0, movs moves forward
do_move:
	mov	es,ax		! destination segment	es:di 目的地址(初始为 0x0000:0x0)
	add	ax,#0x1000	
	cmp	ax,#0x9000	! 已经把从 0x8000 段开始的 64k 代码移动完？
	jz	end_move
	mov	ds,ax		! source segment	ds:siÎ源地址(初始为 0x1000:0x0)
	sub	di,di
	sub	si,si
	mov 	cx,#0x8000	! 移动 0x8000 字（64k 字节）。
	rep
	movsw
	jmp	do_move

! then we load the segment descriptors
! 此后，我们加载段描述符。
! 从这里开始会遇到 32 位保护模式的操作，因此需要 Intel 32 位保护模式编程方面的知识了,
! 有关这方面的信息请查阅列表后的简单介绍或附录中的详细说明。这里仅作概要说明。
!
! lidt 指令用于加载中断描述符表(idt)寄存器，它的操作数是 6 个字节，0-1 字节是描述符表的
! 长度值(字节)；2-5 字节是描述符表的 32 位线性基地址（首地址），其形式参见下面
! 219-220 行和 223-224 行的说明。中断描述符表中的每一个表项（8 字节）指出发生中断时
! 需要调用的代码的信息，与中断向量有些相似，但要包含更多的信息。
!
! lgdt 指令用于加载全局描述符表(gdt)寄存器，其操作数格式与 lidt 指令的相同。全局描述符
! 表中的每个描述符项(8 字节)描述了保护模式下数据和代码段（块）的信息。其中包括段的
! 最大长度限制(16 位)、段的线性基址（32 位）、段的特权级、段是否在内存、读写许可以及
! 其它一些保护模式运行的标志。参见后面 205-216 行。

end_move:
	mov	ax,#SETUPSEG	! right, forgot this at first. didn't work :-)
	mov	ds,ax		! ds 指向本程序(setup)段。
	lidt	idt_48		! load idt with 0,0
				! 加载中断描述符表(idt)寄存器，idt_48 是 6 字节操作数的位置
				! 。前 2 字节表示 idt 表的限长，后 4 字节表示 idt 表
				! 所处的基地址。
	lgdt	gdt_48		! load gdt with whatever appropriate
				! 加载全局描述符表(gdt)寄存器，gdt_48 是 6 字节操作数的位置
				
! that was painless, now we enable A20
! 以上的操作很简单，现在我们开启 A20 地址线。参见程序列表后有关 A20 信号线的说明

	call	empty_8042		! 等待输入缓冲器空。
					! 只有当输入缓冲器为空时才可以对其进行写命令。
	mov	al,#0xD1		! command write	! 0xD1 命令码-表示要写数据到
	out	#0x64,al		! 8042 的 P2 端口。P2 端口的位 1 用于 A20 线的选通。
					! 数据要写到 0x60 口。
	call	empty_8042		! 等待输入缓冲器空，看命令是否被接受。
	mov	al,#0xDF		! A20 on		! 选通 A20 地址线的参数。
	out	#0x60,al
	call	empty_8042		! 输入缓冲器为空，则表示 A20 线已经选通。

! well, that went ok, I hope. Now we have to reprogram the interrupts :-(
! we put them right after the intel-reserved hardware interrupts, at
! int 0x20-0x2F. There they won't mess up anything. Sadly IBM really
! messed this up with the original PC, and they haven't been able to
! rectify it afterwards. Thus the bios puts interrupts at 0x08-0x0f,
! which is used for the internal hardware interrupts as well. We just
! have to reprogram the 8259's, and it isn't fun.

!! 希望以上一切正常。现在我们必须重新对中断进行编程/
!! 我们将它们放在正好处于 intel 保留的硬件中断后面， 在 int 0x20-0x2F。
!! 在那里它们不会引起冲突。不幸的是 IBM 在原 PC 机中搞糟了，以后也没有纠正过来。
!! PC 机的 bios 将中断放在了 0x08-0x0f，这些中断也被用于内部硬件中断。
!! 所以我们就必须重新对 8259 中断控制器进行编程，这一点都没劲。


	mov	al,#0x11		! initialization sequence
					! 0x11 表示初始化命令开始，
					! 是 ICW1 命令字，表示边沿触发、多片 8259
					! 级连、最后要发送 ICW4 命令字。
	out	#0x20,al		! send it to 8259A-1
	.word	0x00eb,0x00eb		! jmp $+2, jmp $+2		! $ 表示当前指令的地址，
					! 两条跳转指令，跳到下一条指令，起延时作用。
	out	#0xA0,al		! and to 8259A-2		! 再发送到 8259A 从芯片。
	.word	0x00eb,0x00eb
	mov	al,#0x20		! start of hardware int's (0x20)
					! 送主芯片 ICW2 命令字，起始中断号，要送奇地址。
	out	#0x21,al
	.word	0x00eb,0x00eb
	mov	al,#0x28		! start of hardware int's 2 (0x28)
					! 送从芯片 ICW2 命令字，起始中断号。
	out	#0xA1,al
	.word	0x00eb,0x00eb
	mov	al,#0x04		! 8259-1 is master
					! 送主芯片 ICW3 命令字，主芯片的 IR2 连从芯片 INT。
	out	#0x21,al
	.word	0x00eb,0x00eb
	mov	al,#0x02		! 8259-2 is slave
					! 送从芯片 ICW3 命令字，表示从芯片的 INT 连到主芯
					! 片的 IR2 引脚上。
	out	#0xA1,al
	.word	0x00eb,0x00eb
	mov	al,#0x01		! 8086 mode for both
					! 送两个芯片的 ICW4 命令字。8086 模式；普通 EOI
					! 方式，需发送指令来复位。初始化结束，芯片就绪。
	out	#0x21,al
	.word	0x00eb,0x00eb
	out	#0xA1,al
	.word	0x00eb,0x00eb
	mov	al,#0xFF		! mask off all interrupts for now
					! 当前屏蔽所有中断请求。
	out	#0x21,al
	.word	0x00eb,0x00eb
	out	#0xA1,al

! well, that certainly wasn't fun :-(. Hopefully it works, and we don't
! need no steenking BIOS anyway (except for the initial loading :-).
! The BIOS-routine wants lots of unnecessary data, and it's less
! "interesting" anyway. This is how REAL programmers do it.
!
! Well, now's the time to actually move into protected mode. To make
! things as simple as possible, we do no register set-up or anything,
! we let the gnu-compiled 32-bit programs do that. We just jump to
! absolute address 0x00000, in 32-bit protected mode.
!! 哼，上面这段当然没劲/，希望这样能工作，而且我们也不再需要乏味的 BIOS 了（除了
!! 初始的加载☺。BIOS 子程序要求很多不必要的数据，而且它一点都没趣。那是“真正”的
!! 程序员所做的事。

! 这里设置进入 32 位保护模式运行。首先加载机器状态字(lmsw - Load Machine Status Word)，也称
! 控制寄存器 CR0，其比特位 0 置 1 将导致 CPU 工作在保护模式。

	mov	ax,#0x0001	! protected mode (PE) bit
	lmsw	ax		! This is it!
	jmpi	0,8		! jmp offset 0 of segment 8 (cs)
				! CPU工作方式转变为保护模式，一个重要的特征就是要根据GDT表来
				! 决定后续将执行哪里的程序。

! 我们已经将 system 模块移动到 0x00000 开始的地方，所以这里的偏移地址是 0。这里的段
! 值的 8 已经是保护模式下的段选择符了，用于选择描述符表和描述符表项以及所要求的
! 特权级。
! 段选择符长度为 16 位（2 字节）；位 0-1 表示请求的特权级 0-3，linux 操作系统只
! 用到两级：0 级（系统级）和 3 级（用户级）；位 2 用于选择全局描述符表(0)还是局部描
! 述符表(1)；位 3-15 是描述符表项的索引，指出选择第几项描述符。所以段选择符
! 8(0b0000,0000,0000,1000)表示请求特权级 0、使用全局描述符表中的第 1 项，该项指出
! 代码的基地址是 0（参见 209 行），因此这里的跳转指令就会去执行 system 中的代码。

! This routine checks that the keyboard command queue is empty
! No timeout is used - if this hangs there is something wrong with
! the machine, and we probably couldn't proceed anyway.

! 下面这个子程序检查键盘命令队列是否为空。这里不使用超时方法 - 如果这里死机，
! 则说明 PC 机有问题，我们就没有办法再处理下去了。
! 只有当输入缓冲器为空时（状态寄存器位 2 = 0）才可以对其进行写命令。

empty_8042:
	.word	0x00eb,0x00eb	! 这是两个跳转指令的机器码(跳转到下一句)，相当于延时空操作
	in	al,#0x64	! 8042 status port		! 读 AT 键盘控制器状态寄存器。
	test	al,#2		! is input buffer full?		! 测试位 2，输入缓冲器满？
	jnz	empty_8042	! yes - loop	
	ret

gdt:	! 全局描述符表开始处。描述符表由多个 8 字节长的描述符项组成。
	! 这里给出了 3 个描述符项。第 1 项无用（206 行），但须存在。第 2 项是系统代码段
	! 描述符（208-211 行），第 3 项是系统数据段描述符(213-216 行)。每个描述符的具体
	! 含义参见列表后说明。
	.word	0,0,0,0		! dummy	! 第 1 个描述符，不用。
! 这里在 gdt 表中的偏移量为 0x08，当加载代码段寄存器(段选择符)时，使用的是这个偏移值

	.word	0x07FF		! 8Mb - limit=2047 (2048*4096=8Mb)
	.word	0x0000		! base address=0
	.word	0x9A00		! code read/exec
	.word	0x00C0		! granularity=4096, 386
! 这里在 gdt 表中的偏移量是 0x10，当加载数据段寄存器(如 ds 等)时，使用的是这个偏移值。
	.word	0x07FF		! 8Mb - limit=2047 (2048*4096=8Mb)
	.word	0x0000		! base address=0
	.word	0x9200		! data read/write
	.word	0x00C0		! granularity=4096, 386

idt_48:
	.word	0			! idt limit=0
	.word	0,0			! idt base=0L

gdt_48:
	.word	0x800		! gdt limit=2048, 256 GDT entries
				! 全局表长度为 2k 字节，因为每 8 字节组成一个段描述符项
				! 所以表中共可有 256 项。
	.word	512+gdt,0x9	! gdt base = 0X9xxxx
				! 4 个字节构成的内存线性地址：0x0009<<16 + 0x0200+gdt
				! 也即 0x90200 + gdt(即在本程序段中的偏移地址，205 行)。
	
.text
endtext:
.data
enddata:
.bss
endbss:

!
! SYS_SIZE is the number of clicks (16 bytes) to be loaded.
! 0x3000 is 0x30000 bytes = 196kB, more than enough for current
! versions of linux
!
SYSSIZE = 0x3000
!
!	bootsect.s		(C) 1991 Linus Torvalds
!
! bootsect.s is loaded at 0x7c00 by the bios-startup routines, and moves
! iself out of the way to address 0x90000, and jumps there.
!
! It then loads 'setup' directly after itself (0x90200), and the system
! at 0x10000, using BIOS interrupts. 
!
! NOTE! currently system is at most 8*65536 bytes long. This should be no
! problem, even in the future. I want to keep it simple. This 512 kB
! kernel size should be enough, especially as this doesn't contain the
! buffer cache as in minix
!
! The loader has been made as simple as possible, and continuos
! read errors will result in a unbreakable loop. Reboot by hand. It
! loads pretty fast by getting whole sectors at a time whenever possible.

.globl begtext, begdata, begbss, endtext, enddata, endbss  !声明XXX是全局可见的。
.text							!代码段声明; .text 是只读的代码区
begtext:
.data							!伪指令,数据段定义;.data 是可读可写的数据区
begdata:
.bss							!.bss 则是可读可写且没有初始化的数据区。
begbss:
.text

SETUPLEN = 4				! nr of setup-sectors;  要加载的setup 程序的扇区数(setup-sectors)值；
BOOTSEG  = 0x07c0			! original address of boot-sector； 启动扇区被BIOS加载的位置
INITSEG  = 0x9000			! we move boot here - out of the way
SETUPSEG = 0x9020			! setup starts here；setup程序被加载到的位置；
SYSSEG   = 0x1000			! system loaded at 0x10000 (65536).
ENDSEG   = SYSSEG + SYSSIZE		! where to stop loading

! ROOT_DEV:	根文件系统设备号
!		0x000 - same type of floppy as boot.
!		0x301 - first partition on first drive etc
ROOT_DEV = 0x306			!指定根文件系统设备是第2个硬盘的第1个分区。
			 		! 这是Linux 老式的硬盘命名方式,具体值的含义如下： 
			                ! 设备号=主设备号*256 + 次设备号（也即dev_no = (major<<8) + minor ） 
			                ! （主设备号：1-内存,2-磁盘,3-硬盘,4-ttyx,5-tty,6-并行口,7-非命名管道） 
			                ! 0x300 - /dev/hd0 - 代表整个第1个硬盘； 
			                ! 0x301 - /dev/hd1 - 第1个盘的第1个分区； 
			                ! … 
			                ! 0x304 - /dev/hd4 - 第1个盘的第4个分区； 
			                ! 0x305 - /dev/hd5 - 代表整个第2个硬盘盘； 
			                ! 0x306 - /dev/hd6 - 第2个盘的第1个分区； 
			                ! … 
			                ! 0x309 - /dev/hd9 - 第2个盘的第4个分区； 
			                ! 从linux 内核0.95 版后已经使用与现在相同的命名方法了。
			                ! 注意tools/build会根据Makefile的ROOT_DEV设置
			                !（没有设置的话用DEFAULT_MAJOR_ROOT/DEFAULT_MINOR_ROOT缺省值,修改编译好的bootsect中的root_dev数据)
			                ! build.c中定义的缺省值为0x21d，即1.44MB软驱B
					! vb 0x0000:0x7c00

entry start				!! 告知连接程序，程序从start 标号开始执行。
start:
	mov	ax,#BOOTSEG
	mov	ds,ax			!查看《微型计算机原理与接口技术》中关于段的定义；
	mov	ax,#INITSEG
	mov	es,ax
	mov	cx,#256			!用于下面程序中rep;提供了需要复制的“字”数，256个字正好是512字节		计数寄存器

	！通常用mov ,add ,sub 等指令时，用si,di寻址是一样的，都默认与DS搭配（除非明确指定与ES等来组合）来寻址。
	！但遇到块移动、块比较等块操作指令时，SI，DI的源和目的特征就表现出来了，默认情况下，SI与DS搭配，DI与ES搭配来寻址。
	！这些指令有一个比较突出的特点，通常都有rep前缀。详见 cmps、 cmpsb、 cmpsd、 cmpsw、 ins、 insb、 insd、 insw、 lods、
	！lodsb、 lodsd、 lodsw、 movs、 movsb、 movsd、 movsw、 outs、 outsb、 outsd、 outsw、 stos、 stosb、 stosd、 stosw 、
	！scas scasb scasd scasw 等指令的用法。
	sub	si,si			!DS(0x07C0)和SI(0x0000)联合使用，构成了源地址 ds:si = 0×07C0:0×0000
	sub	di,di			!ES(0x9000)和DI(0x0000)联合使用，构成了目的地址 es:di = 0×9000:0×0000
	rep				!rep是字符串操作指令MOVS,CMPS等的前缀,在CX不等于0的情况下,对字符串执行重复操作. 
	movw				!移动1 个字；
	!由于“两头约定”和“定位识别”的作用，所以bootsect在开始时“被迫”加载到0x07c00位置。
	!现在将其自身移至0x90000处，说明操作系统开始根据自己的需要安排内存了。
	jmpi	go,INITSEG		!jmpi为段间跳转指令
 					!执行这条指令之后
 					!CS = INITSEG
 					!IP = go
 					!也就是跳转到地址 INITSEG : go
					!间接跳转。这里INITSEG 指出跳转到的段地址，再加上go：标识的偏移地址，是执行新位置的go：标识程序；
!Linux的设计意图是跳转之后在新位置接着执行后面的mov ax,cs，而不是死循环。jmpi go,INITSEG与go:mov ax,cs配合，
!巧妙地实现了“到新位置后接着原来的执行程序继续执行下去”的目的。
go:	mov	ax,cs			
	!将ds、es 和ss 都置成移动后代码所在的段处(0×9000)。
	mov	ds,ax
	mov	es,ax
! put stack at 0x9ff00.
!这里对SS和SP进行的设置是分水岭。它标志着从现在开始，程序可以执行更为复杂一些的数据运算类指令了。
	mov	ss,ax			!BP和SP寄存器称为指针寄存器，与SS联用，为访问现行堆栈段提供方便
	mov	sp,#0xFF00		!arbitrary value >>512
! 栈表示stack，特指在C语言程序的运行时结构中，以“后进先出”机制运作的内存空间；
! 堆表示heap，特指用c语言库函数malloc创建、free释放的动态内存空间。

! load the setup-sectors directly after the bootblock.
! Note that 'es' is already set up.
! 加载setup这个程序，要借助BIOS提供的int 0x13中断向量所指向的中断服务程序（也就是磁盘服务程序）来完成。
! 使用int 0x13中断时，就要事先将指定的扇区和加载的内存位置等信息传递给服务程序，即传参；
!0x13是BIOS读磁盘扇区的中断: ah=0x02-读磁盘，al=扇区数量(SETUPLEN=4)，
!ch=柱面号，cl=开始扇区，dh=磁头号，dl=驱动器号，es:bx=内存地址
!读取4个扇区的数据接着在bootsect后面存放，可以看到ex:bx组成的地址刚好是boot后面的第一个字节。

load_setup:
	! 4个mov指令给bios中断服务程序传参，传参是通过几个通用寄存器实现的，这是汇编程序的常用方法。
	! 参数传递完毕后，执行int 0x13指令，产生0x13中断，通过中断向量表找到这个中断服务程序，
	! 将软盘从第2扇区开始的4个扇区,即setup.s对应的程序加载至内存的SETUPSEG(0X90200)处。
	! 0x90200紧挨着bootsect的尾端，所以bootsect和setup是连在一起的。
	mov	dx,#0x0000		! drive 0, head 0
	mov	cx,#0x0002		! sector 2, track 0
	mov	bx,#0x0200		! address = 512, in INITSEG
	mov	ax,#0x0200+SETUPLEN	! service 2, nr of sectors(the number of sectors)
	int	0x13			! read it
	jnc	ok_load_setup		! ok - continue		jnc指令用于对进位位进行判断从而决定程序走向。
	mov	dx,#0x0000
	mov	ax,#0x0000		! reset the diskette
	int	0x13
	j	load_setup

ok_load_setup:

! Get disk drive parameters, specifically nr of sectors/track
!调用BIOS中断:ah=0x08,int 0x13得到磁盘驱动器参数。
!中断调用ah=0x08,int 0x13返回后，在以下寄存器返回以下信息:
!DL:本机软盘驱动器的数目 
!DH:最大磁头号（或说磁面数目）。0表示有1个磁面，1表示有2个磁面 
!CH:存放10位磁道柱面数的低8位（高2位在CL的D7、D6中）。1表示有1个柱面，2表示有2个柱面，依次类推。 
!CL:0~5位存放每磁道的扇区数目。6和7位表示10位磁道柱面数的高2位。 
!AX=0 
!BH=0 
!BL表示驱动器类型： 
!1=360K 5.25 
!2=1.2M 5.25 
!3=720K 3.5 
!4=1.44M 3.5 
!ES:SI 指向软盘参数表 
	mov	dl,#0x00		! DL驱动器序号, 第一个软驱为0, 第一个硬盘为0x80
	mov	ax,#0x0800		! AH=8 is get drive parameters
	int	0x13
	mov	ch,#0x00
	! seg   cs这样的语句只影响到它下一条指令;seg cs只是表明紧跟它的下一条语句将使用段超越
	! 即下面两条指令代表的实际指令是mov cs:[sectors],cx
	seg cs
	mov	sectors,cx
	mov	ax,#INITSEG
	mov	es,ax

! Print some inane message

	mov	ah,#0x03		! read cursor pos
	xor	bh,bh
	int	0x10
	
	mov	cx,#24
	mov	bx,#0x0007		! page 0, attribute 7 (normal)
	mov	bp,#msg1		!
	mov	ax,#0x1301		! write string, move cursor
	int	0x10
!以上几句调用了bios的0x10中断，显示一个字符串msg1

! ok, we've written the message, now
! we want to load the system (at 0x10000)

	mov	ax,#SYSSEG
	mov	es,ax		! segment of 0x010000
	call	read_it
	call	kill_motor

! After that we check which root-device to use. If the device is
! defined (!= 0), nothing is done and the given device is used.
! Otherwise, either /dev/PS0 (2,28) or /dev/at0 (2,8), depending
! on the number of sectors that the BIOS reports currently.
! 上面行中两个设备文件的含义：
! 在 Linux 中软驱的主设备号是 2，次设备号 = type*4 + nr，其中
! nr 为 0-3 分别对应软驱 A、B、C 或 D；type 是软驱的类型（2代表1.2M 或 7代表1.44M 等）。
! 因为 7*4 + 0 = 28，所以 /dev/PS0 (2,28)指的是 1.44M A 驱动器,其设备号是 0x021c
! 同理 /dev/at0 (2,8)指的是 1.2M A 驱动器，其设备号是 0x0208。
	seg cs
	mov	ax,root_dev
	cmp	ax,#0
	jne	root_defined
	seg cs
	mov	bx,sectors
	mov	ax,#0x0208		! /dev/ps0 - 1.2Mb
	cmp	bx,#15			! 如果 sectors=15,则说明是 1.2Mb 的驱动器；
	je	root_defined		! 如果等于，则 ax 中就是引导驱动器的设备号。
	mov	ax,#0x021c		! /dev/PS0 - 1.44Mb
	cmp	bx,#18			! 如果 sectors=18,则说明是 1.44Mb 的软驱；
	je	root_defined
undef_root:				! 如果都不一样，则死循环（死机）。
	jmp undef_root
root_defined:
	seg cs
	mov	root_dev,ax		! 将检查过的设备号保存起来。

! after that (everyting loaded), we jump to
! the setup-routine loaded directly after
! the bootblock:

	jmpi	0,SETUPSEG		! 跳转到 0x9020:0000(setup.s 程序的开始处)。

! This routine loads the system at address 0x10000, making sure
! no 64kB boundaries are crossed. We try to load it as fast as
! possible, loading whole tracks whenever we can.
!
! in:	es - starting address segment (normally 0x1000)
!
sread:	.word 1+SETUPLEN	! sectors read of current track
head:	.word 0			! current head
track:	.word 0			! current track

read_it:
	mov ax,es               ! es 0x1000
	test ax,#0x0fff
die:	jne die			! es must be at 64kB boundary
	!对本身进行异或运算，清空寄存器bx
	xor bx,bx		! bx is starting address within segment
rp_read:
	mov ax,es
	cmp ax,#ENDSEG		! have we loaded all yet?
	jb ok1_read
	ret
ok1_read:
! 计算和验证当前磁道需要读取的扇区数，放在 ax 寄存器中。
! 根据当前磁道还未读取的扇区数以及段内数据字节开始偏移位置，计算如果全部读取这些未读扇区，所
! 读总字节数是否会超过 64KB 段长度的限制。若会超过， 则根据此次最多能读入的字节数(64KB – 段内
! 偏移位置)，反算出此次需要读取的扇区数
	seg cs
	mov ax,sectors		! 取每磁道扇区数。
	sub ax,sread		! 减去当前磁道已读扇区数。
	mov cx,ax		! cx = ax = 当前磁道未读扇区数。
	shl cx,#9		! cx = cx * 512 字节。每个扇区512个字节
	add cx,bx		! cx = cx + 段内当前偏移值(bx)
				! = 此次读操作后，段内共读入的字节数。
	jnc ok2_read		! 若没有超过 64KB 字节，则跳转至 ok2_read 处执行。
	je ok2_read
	xor ax,ax		! 若加上此次将读磁道上所有未读扇区时会超过 64KB，则计算
	sub ax,bx		! 此时最多能读入的字节数(64KB – 段内读偏移位置)， 再转换
	shr ax,#9		! 成需要读取的扇区数。
ok2_read:
	call read_track
	mov cx,ax		! cx = 该次操作已读取的扇区数。
	add ax,sread		! 当前磁道上已经读取的扇区数。
	seg cs
	cmp ax,sectors		! 如果当前磁道上的还有扇区未读， 则跳转到 ok3_read 处。
	jne ok3_read		
	! 读该磁道的下一磁头面(1 号磁头)上的数据。如果已经完成，则去读下一磁道。
	mov ax,#1
	sub ax,head		! 判断当前磁头号。
	jne ok4_read		! 如果是 0 磁头，则再去读 1 磁头面上的扇区数据
	inc track		! 否则去读下一磁道。
ok4_read:
	mov head,ax		! 保存当前磁头号。
	xor ax,ax		! 清当前磁道已读扇区数。
ok3_read:
	mov sread,ax		! 保存当前磁道已读扇区数。
	shl cx,#9		! 上次已读扇区数*512 字节。
	add bx,cx		! 调整当前段内数据开始位置。
	jnc rp_read		! 若小于 64KB 边界值，则跳转到 rp_read(156 行)处，继续读数据。
				! 否则调整当前段，为读下一段数据作准备。
	mov ax,es		
	add ax,#0x1000		! 将段基址调整为指向下一个 64KB 段内存。	
	mov es,ax
	xor bx,bx		! 清段内数据开始偏移值
	jmp rp_read		! 跳转至 rp_read(156 行)处，继续读数据。

read_track:
	push ax
	push bx
	push cx
	push dx
	mov dx,track		! 取当前磁道号。
	mov cx,sread		! 取当前磁道上已读扇区数。
	inc cx			! cl = 开始读扇区。
	mov ch,dl		! ch = 当前磁道号。
	mov dx,head		! 取当前磁头号。
	mov dh,dl		! dh = 磁头号。
	mov dl,#0		! dl = 驱动器号(为 0 表示当前驱动器)。
	and dx,#0x0100		! 磁头号不大于 1。
	mov ah,#2		! ah = 2，读磁盘扇区功能号。
	int 0x13
	jc bad_rt		! 若出错，则跳转至 bad_rt。
	pop dx
	pop cx
	pop bx
	pop ax
	ret

! 执行驱动器复位操作（磁盘中断功能号 0），再跳转到 read_track 处重试。
bad_rt:	mov ax,#0
	mov dx,#0
	int 0x13
	pop dx
	pop cx
	pop bx
	pop ax
	jmp read_track

/*
 * This procedure turns off the floppy drive motor, so
 * that we enter the kernel in a known state, and
 * don't have to worry about it later.
 */
kill_motor:
	push dx
	mov dx,#0x3f2		! 软驱控制卡的驱动端口，只写。
	mov al,#0		! A 驱动器，关闭 FDC，禁止 DMA 和中断请求，关闭马达。
	outb			! 将 al 中的内容输出到 dx 指定的端口去。
	pop dx
	ret

sectors:
	.word 0				! 存放当前启动软盘每磁道的扇区数。

msg1:
	.byte 13,10			! 回车、换行的 ASCII 码。
	.ascii "Loading system ..."
	.byte 13,10,13,10		! 共 24 个 ASCII 码字符。

.org 508				! 表示下面语句从地址 508(0x1FC)开始，所以 root_dev
					! 在启动扇区的第 508 开始的 2 个字节中。
root_dev:
	.word ROOT_DEV			! 这里存放根文件系统所在的设备号(init/main.c 中会用)。
boot_flag:
	.word 0xAA55			! 硬盘有效标识。

.text
endtext:
.data
enddata:
.bss
endbss:

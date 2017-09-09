/*
 *  linux/kernel/blk_dev/ll_rw.c
 *
 * (C) 1991 Linus Torvalds
 */

/*
 * This handles all read/write requests to block devices
 */
#include <errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include "blk.h"

/*
 * The request-struct contains all necessary data
 * to load a nr of sectors into memory
 */
struct request request[NR_REQUEST];

/*
 * used to wait on when there are no free requests
 */
struct task_struct * wait_for_request = NULL;

/* blk_dev_struct is:
 *	do_request-address
 *	next-request
 */
struct blk_dev_struct blk_dev[NR_BLK_DEV] = {
	{ NULL, NULL },		/* no_dev */
	{ NULL, NULL },		/* dev mem */
	{ NULL, NULL },		/* dev fd */
	{ NULL, NULL },		/* dev hd */
	{ NULL, NULL },		/* dev ttyx */
	{ NULL, NULL },		/* dev tty */
	{ NULL, NULL }		/* dev lp */
};

static inline void lock_buffer(struct buffer_head * bh)
{
	cli();
	while (bh->b_lock)
		sleep_on(&bh->b_wait);
	bh->b_lock=1;
	sti();
}

static inline void unlock_buffer(struct buffer_head * bh)
{
	if (!bh->b_lock)
		printk("ll_rw_block.c: buffer not locked\n\r");
	bh->b_lock = 0;
	wake_up(&bh->b_wait);
}

/*
 * add-request adds a request to the linked list.
 * It disables interrupts so that it can muck with the
 * request-lists in peace.
 */
static void add_request(struct blk_dev_struct * dev, struct request * req)
{
	struct request * tmp;

	req->next = NULL;
	cli();
	if (req->bh)
		req->bh->b_dirt = 0;
	if (!(tmp = dev->current_request)) {
		dev->current_request = req;
		sti();
		(dev->request_fn)();		//调用硬盘请求项处理函数去给硬盘发送具体的读盘命令；即调用do_hd_request函数
		return;
	}
	for ( ; tmp->next ; tmp=tmp->next)
		if ((IN_ORDER(tmp,req) ||
		    !IN_ORDER(tmp,tmp->next)) &&
		    IN_ORDER(req,tmp->next))
			break;
	req->next=tmp->next;
	tmp->next=req;
	sti();
}

static void make_request(int major,int rw, struct buffer_head * bh)
{
	struct request * req;
	int rw_ahead;

/* WRITEA/READA is special case - it is not really needed, so if the */
/* buffer is locked, we just forget about it, else it's a normal read */
	if (rw_ahead = (rw == READA || rw == WRITEA)) {
		if (bh->b_lock)
			return;
		if (rw == READA)
			rw = READ;
		else
			rw = WRITE;
	}
	if (rw!=READ && rw!=WRITE)
		panic("Bad block dev command, must be R/W/RA/WA");
	lock_buffer(bh);				//将这个缓冲块加锁，目的是让这个缓冲块在解锁之前将不再被操作。
									//因为这个缓冲块现在已经被使用，如果此时它再被用于它途，这里面的数据就会发生混乱
	if ((rw == WRITE && !bh->b_dirt) || (rw == READ && bh->b_uptodate)) {
		unlock_buffer(bh);
		return;
	}
repeat:
/* we don't allow the write-requests to fill up the queue completely:
 * we want some room for reads: they take precedence. The last third
 * of the requests are only for reads.
 */
	if (rw == READ)
		req = request+NR_REQUEST;			//如果是读请求，则从整个请求项结构的最未端开始寻找空闲请求项
	else
		req = request+((NR_REQUEST*2)/3);	//如果是写请求，则从整个结构的三分之二处申请空闲请求项。
/* find an empty request */
	while (--req >= request)
		if (req->dev<0)
			break;
/* if none found, sleep on new requests: check for rw_ahead */
	//如果最后没有找到合适的空闲请求项，就将当前进程挂起
	if (req < request) {
		if (rw_ahead) {
			unlock_buffer(bh);
			return;
		}
		sleep_on(&wait_for_request);	//挂起进程
		goto repeat;
	}
/* fill up the request-info, and add it to the queue */
	//缓冲块与请求项正式挂接，对这个请求项各个成员进行初始化。
	req->dev = bh->b_dev;
	req->cmd = rw;
	req->errors=0;
	req->sector = bh->b_blocknr<<1;
	req->nr_sectors = 2;
	req->buffer = bh->b_data;
	req->waiting = NULL;
	req->bh = bh;
	req->next = NULL;
	add_request(major+blk_dev,req);		//往请求项队列中加载该请求项
}

void ll_rw_block(int rw, struct buffer_head * bh)
{
	unsigned int major;

	if ((major=MAJOR(bh->b_dev)) >= NR_BLK_DEV ||	//判断缓冲块对应的设备是否存在
	!(blk_dev[major].request_fn)) {					//以及这个设备的请求项函数是否挂接正常。
		printk("Trying to read nonexistent block-device\n\r");
		return;
	}
	make_request(major,rw,bh);					//将缓冲块与请求项建立关系。
}

void blk_dev_init(void)
{
	int i;

	for (i=0 ; i<NR_REQUEST ; i++) {
		request[i].dev = -1;			//说明这个请求项还没有具体对应哪个设备，
										//此标志用于判断对应该请求项的当前设备是否空闲
		request[i].next = NULL;			//说明这个请求项还没有与请求项队列进行挂接。
	}
}

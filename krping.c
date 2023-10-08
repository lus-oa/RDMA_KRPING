// sudo rm /boot/*5.15*
// sudo update-grub2
/*
 * Copyright (c) 2005 Ammasso, Inc. All rights reserved.
 * Copyright (c) 2006-2009 Open Grid Computing, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <linux/version.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/parser.h>
#include <linux/proc_fs.h>
#include <linux/inet.h>
#include <linux/list.h>
#include <linux/in.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/ktime.h>
#include <linux/random.h>
#include <linux/signal.h>
#include <linux/proc_fs.h>

#include <asm/atomic.h>
#include <asm/pci.h>

#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/blk-mq.h>
#include <linux/hdreg.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>

// #include <linux/timex.h>

#include "getopt.h"

#define PFX "krping: "

int flag = 0; // 用于等待外部程序执行抓取bio的信号
// wait_queue_head_t wq;
//  init_waitqueue_head(&wq);
char *buffer = NULL;
size_t buffer_index = 0;
static int debug = 0;
module_param(debug, int, 0);
MODULE_PARM_DESC(debug, "Debug level (0=none, 1=all)");
#define DEBUG_LOG \
	if (debug)    \
	printk

MODULE_AUTHOR("Steve Wise");
MODULE_DESCRIPTION("RDMA ping server");
MODULE_LICENSE("Dual BSD/GPL");

#ifndef SECTOR_SIZE
#define SECTOR_SIZE 512
#endif
#define FILE_PATH "/bio.txt"
static int dev_major = 0;

/* Just internal representation of the our block device
 * can hold any useful data */
struct block_dev
{
	sector_t capacity;
	u8 *data; /* Data buffer to emulate real storage device */
	struct blk_mq_tag_set tag_set;
	struct request_queue *queue;
	struct gendisk *gdisk;
};

/* Device instance */
static struct block_dev *block_device = NULL;

struct rdmareq
{
	// rw_flag读写标志。0：读；1：写；-1：其他操作
	int rw_flag;
	sector_t sector;
	unsigned int totaldata_len;
	void *virtaddr;
	// phys_addr_t physaddr;
	unsigned int partlen;
};
struct rdmareq rrq;

static const struct krping_option krping_opts[] = {
	{"count", OPT_INT, 'C'},
	{"size", OPT_INT, 'S'},
	{"addr", OPT_STRING, 'a'},
	{"addr6", OPT_STRING, 'A'},
	{"port", OPT_INT, 'p'},
	{"verbose", OPT_NOPARAM, 'v'},
	{"validate", OPT_NOPARAM, 'V'},
	{"server", OPT_NOPARAM, 's'},
	{"client", OPT_NOPARAM, 'c'},
	{"server_inv", OPT_NOPARAM, 'I'},
	{"wlat", OPT_NOPARAM, 'l'},
	{"rlat", OPT_NOPARAM, 'L'},
	{"bw", OPT_NOPARAM, 'B'},
	{"duplex", OPT_NOPARAM, 'd'},
	{"tos", OPT_INT, 't'},
	{"txdepth", OPT_INT, 'T'},
	{"poll", OPT_NOPARAM, 'P'},
	{"local_dma_lkey", OPT_NOPARAM, 'Z'},
	{"read_inv", OPT_NOPARAM, 'R'},
	{"fr", OPT_NOPARAM, 'f'},
	{NULL, 0, 0}};

struct krping_stats
{
	unsigned long long send_bytes;
	unsigned long long send_msgs;
	unsigned long long recv_bytes;
	unsigned long long recv_msgs;
	unsigned long long write_bytes;
	unsigned long long write_msgs;
	unsigned long long read_bytes;
	unsigned long long read_msgs;
};

#define htonll(x) cpu_to_be64((x))
#define ntohll(x) cpu_to_be64((x))

static DEFINE_MUTEX(krping_mutex);

/*
 * List of running krping threads.
 */
static LIST_HEAD(krping_cbs);

static struct proc_dir_entry *krping_proc;

/*
 * Invoke like this, one on each side, using the server's address on
 * the RDMA device (iw%d):
 *
 * /bin/echo server,port=9999,addr=192.168.69.142,validate > /proc/krping
 * /bin/echo client,port=9999,addr=192.168.69.142,validate > /proc/krping
 * /bin/echo client,port=9999,addr6=2001:db8:0:f101::1,validate > /proc/krping
 *
 * krping "ping/pong" loop:
 * 	client sends source rkey/addr/len
 *	server receives source rkey/add/len
 *	server rdma reads "ping" data from source
 * 	server sends "go ahead" on rdma read completion
 *	client sends sink rkey/addr/len
 * 	server receives sink rkey/addr/len
 * 	server rdma writes "pong" data to sink
 * 	server sends "go ahead" on rdma write completion
 * 	<repeat loop>
 */

/*
 * These states are used to signal events between the completion handler
 * and the main client or server thread.
 *
 * Once CONNECTED, they cycle through RDMA_READ_ADV, RDMA_WRITE_ADV,
 * and RDMA_WRITE_COMPLETE for each ping.
 */
enum test_state
{
	IDLE = 1,
	CONNECT_REQUEST,
	ADDR_RESOLVED,
	ROUTE_RESOLVED,
	CONNECTED,
	RDMA_READ_ADV,
	RDMA_READ_COMPLETE,
	RDMA_WRITE_ADV,
	RDMA_WRITE_COMPLETE,
	ERROR
};

struct krping_rdma_info
{
	uint64_t buf;
	uint32_t rkey;
	uint32_t size;
};

/*
 * Default max buffer size for IO...
 */
#define RPING_BUFSIZE 128 * 1024
#define RPING_SQ_DEPTH 64

/*
 * Control block struct.
 */
struct krping_cb
{
	int server; /* 0 iff client */
	struct ib_cq *cq;
	struct ib_pd *pd;
	struct ib_qp *qp;

	struct ib_mr *dma_mr;

	struct ib_fast_reg_page_list *page_list;
	int page_list_len;
	struct ib_reg_wr reg_mr_wr;
	struct ib_send_wr invalidate_wr;
	struct ib_mr *reg_mr;
	int server_invalidate;
	int read_inv;
	u8 key;

	struct ib_recv_wr rq_wr;						/* recv work request record */
	struct ib_sge recv_sgl;							/* recv single SGE */
	struct krping_rdma_info recv_buf __aligned(16); /* malloc'd buffer */
	u64 recv_dma_addr;
	DEFINE_DMA_UNMAP_ADDR(recv_mapping);

	struct ib_send_wr sq_wr; /* send work requrest record */
	struct ib_sge send_sgl;
	struct krping_rdma_info send_buf __aligned(16); /* single send buf */
	u64 send_dma_addr;
	DEFINE_DMA_UNMAP_ADDR(send_mapping);

	struct ib_rdma_wr rdma_sq_wr; /* rdma work request record */
	struct ib_sge rdma_sgl;		  /* rdma single SGE */
	char *rdma_buf;				  /* used as rdma sink */
	u64 rdma_dma_addr;
	DEFINE_DMA_UNMAP_ADDR(rdma_mapping);
	struct ib_mr *rdma_mr;

	uint32_t remote_rkey; /* remote guys RKEY */
	uint64_t remote_addr; /* remote guys TO */
	uint32_t remote_len;  /* remote guys LEN */

	char *start_buf; /* rdma read src */
	u64 start_dma_addr;
	DEFINE_DMA_UNMAP_ADDR(start_mapping);
	struct ib_mr *start_mr;

	enum test_state state; /* used for cond/signalling */
	wait_queue_head_t sem;
	struct krping_stats stats;

	uint16_t port;			 /* dst port in NBO */
	u8 addr[16];			 /* dst addr in NBO */
	char ip6_ndev_name[128]; /* IPv6 netdev name */
	char *addr_str;			 /* dst addr string */
	uint8_t addr_type;		 /* ADDR_FAMILY - IPv4/V6 */
	int verbose;			 /* verbose logging */
	int count;				 /* ping count */
	int size;				 /* ping data size */
	int validate;			 /* validate ping data */
	int wlat;				 /* run wlat test */
	int rlat;				 /* run rlat test */
	int bw;					 /* run bw test */
	int duplex;				 /* run bw full duplex test */
	int poll;				 /* poll or block for rlat test */
	int txdepth;			 /* SQ depth */
	int local_dma_lkey;		 /* use 0 for lkey */
	int frtest;				 /* reg test */
	int tos;				 /* type of service */

	/* CM stuff */
	struct rdma_cm_id *cm_id; /* connection on client side,*/
	/* listener on server side. */
	struct rdma_cm_id *child_cm_id; /* connection on server side */
	struct list_head list;
};

struct krping_cb *cb1;

static int blockdev_open(struct block_device *dev, fmode_t mode)
{
	printk(">>> blockdev_open\n");

	return 0;
}

static void blockdev_release(struct gendisk *gdisk, fmode_t mode)
{
	printk(">>> blockdev_release\n");
}

int blockdev_ioctl(struct block_device *bdev, fmode_t mode, unsigned cmd, unsigned long arg)
{
	printk("ioctl cmd 0x%08x\n", cmd);

	return -ENOTTY;
}

/* Set block device file I/O */
static struct block_device_operations blockdev_ops = {
	.owner = THIS_MODULE,
	.open = blockdev_open,
	.release = blockdev_release,
	.ioctl = blockdev_ioctl};

/* Serve requests */
static int do_request(struct request *rq, unsigned int *nr_bytes)
{
	int ret = 0;
	struct bio_vec bvec;
	struct req_iterator iter;
	struct block_dev *dev = rq->q->queuedata;
	loff_t pos = blk_rq_pos(rq) << SECTOR_SHIFT;
	loff_t dev_size = (loff_t)(dev->capacity << SECTOR_SHIFT);

	// printk(KERN_WARNING "sblkdev: request start from sector %lld  pos = %lld  dev_size = %lld\n", blk_rq_pos(rq), pos, dev_size);

	/* Iterate over all requests segments */
	rq_for_each_segment(bvec, rq, iter)
	{
		unsigned long b_len = bvec.bv_len;

		/* Get pointer to the data */
		void *b_buf = page_address(bvec.bv_page) + bvec.bv_offset;

		/* Simple check that we are not out of the memory bounds */
		if ((pos + b_len) > dev_size)
		{
			b_len = (unsigned long)(dev_size - pos);
		}

		if (rq_data_dir(rq) == WRITE)
		{
			/* Copy data to the buffer in to required position */
			memcpy(dev->data + pos, b_buf, b_len);
		}
		else
		{
			/* Read data from the buffer's position */
			memcpy(b_buf, dev->data + pos, b_len);
		}

		/* Increment counters */
		pos += b_len;
		*nr_bytes += b_len;
	}

	return ret;
}

/*这段代码没用
void write_bio(const struct rdmareq *req)
{
	struct file *file;
	loff_t pos = 0;
	char hex_string[256];

	file = filp_open(FILE_PATH, O_WRONLY | O_APPEND | O_CREAT, 0644);
	if (IS_ERR(file))
	{
		printk(KERN_ERR "Failed to open file\n");
		return;
	}

	// 将结构体字段以16进制形式写入字符串，没有字段名和分隔符
	snprintf(hex_string, sizeof(hex_string), "%x%llx%x%p%x\n",
			 req->rw_flag, (unsigned long long)req->sector, req->totaldata_len,
			 req->virtaddr, req->partlen);

	// 使用 kernel_write 写入数据
	kernel_write(file, hex_string, strlen(hex_string), &pos);
	filp_close(file, NULL);
}*/
/*
这段代码将queue_rq函数中即将要执行的request截取并传入
*先获取读写标志位rw,其中0是读，1是写，-1是其他操作
*对request中的每个bvec进行遍历，bvec

 */
void print_request(struct request *rq)
{
	struct bio_vec bvec;
	struct req_iterator iter;
	int rw;

	/*
	   REQ_WRITE 标志位用于表示写操作
	   REQ_READ 标志位用于表示读操作
	*/
	if (rq_data_dir(rq) == READ)
	{
		// 读取操作
		rw = 0;
	}
	else if (rq_data_dir(rq) == WRITE)
	{
		// 写入操作
		rw = 1;
	}
	else
	{
		// 其他操作
		rw = -1;
	}

	rq_for_each_bvec(bvec, rq, iter)
	{
		// struct rdmareq rrq;
		struct page *page = bvec.bv_page;
		unsigned int offset = bvec.bv_offset;
		unsigned int len = bvec.bv_len;
		rrq.rw_flag = rw;
		rrq.sector = blk_rq_pos(rq);
		rrq.totaldata_len = blk_rq_bytes(rq);
		// 地址
		rrq.virtaddr = page_address(page) + offset;
		rrq.partlen = len;
		//
		char *data = (char *)rrq.virtaddr;
		size_t i;

		// 分配一个足够大的缓冲区，bio中的每个数据段长度不超过4096字节，因此开4096字节的缓冲区就够用了
		buffer = (char *)kmalloc(4096, GFP_KERNEL);
		if (!buffer)
		{
			printk(KERN_ERR "Failed to allocate memory for buffer\n");
			return; // 或者执行适当的错误处理
		}
		buffer_index = 0;
		// 复制非空数据到缓冲区中
		for (i = 0; i < len; ++i)
		{
			if (data[i] != '\0')
			{
				buffer[buffer_index++] = data[i];
			}
		}

		// 添加字符串结束符
		// buffer[buffer_index] = 0;

		// 现在，buffer 包含了去掉空值的数据

		// 打印 buffer 的内容
		printk(KERN_INFO "Buffer Content: %s\n", buffer);

		// 释放缓冲区
		// kfree(buffer);

		// 在这里处理当前 bvec 的虚拟地址
		// printk(KERN_INFO "Virtual Address: %p, Partlen: %u\n", rrq.virtaddr, len);
	}
}

static blk_status_t queue_rq(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd)
{
	unsigned int nr_bytes = 0;
	blk_status_t status = BLK_STS_OK;
	struct request *rq = bd->rq;

	if (strcmp(current->comm, "write_data") == 0)
	{

		print_request(rq);
		flag = 1;
		wake_up_interruptible(&cb1->sem);
	}

	/* Start request serving procedure */
	blk_mq_start_request(rq);

	if (do_request(rq, &nr_bytes) != 0)
	{
		status = BLK_STS_IOERR;
	}

	/* Notify kernel about processed nr_bytes */
	if (blk_update_request(rq, status, nr_bytes))
	{
		/* Shouldn't fail */
		BUG();
	}

	/* Stop request serving procedure */
	__blk_mq_end_request(rq, status);

	return status;
}

static struct blk_mq_ops mq_ops = {
	.queue_rq = queue_rq,
};

static int krping_cma_event_handler(struct rdma_cm_id *cma_id,
									struct rdma_cm_event *event)
{
	int ret;
	struct krping_cb *cb = cma_id->context;

	DEBUG_LOG("cma_event type %d cma_id %p (%s)\n", event->event, cma_id,
			  (cma_id == cb->cm_id) ? "parent" : "child");

	switch (event->event)
	{
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		cb->state = ADDR_RESOLVED;
		printk("RDMA_CM_EVENT_ADDR_RESOLVED");
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret)
		{
			printk(KERN_ERR PFX "rdma_resolve_route error %d\n",
				   ret);
			wake_up_interruptible(&cb->sem);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		cb->state = ROUTE_RESOLVED;
		printk("RDMA_CM_EVENT_ROUTE_RESOLVED");
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		cb->state = CONNECT_REQUEST;
		printk("RDMA_CM_EVENT_CONNECT_REQUEST");
		cb->child_cm_id = cma_id;
		DEBUG_LOG("child cma %p\n", cb->child_cm_id);
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		DEBUG_LOG("ESTABLISHED\n");
		printk("RDMA_CM_EVENT_ESTABLISHED");
		if (!cb->server)
		{
			cb->state = CONNECTED;
		}
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		printk(KERN_ERR PFX "cma event %d, error %d\n", event->event,
			   event->status);
		cb->state = ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		printk(KERN_ERR PFX "DISCONNECT EVENT...\n");
		cb->state = ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		printk(KERN_ERR PFX "cma detected device removal!!!!\n");
		cb->state = ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	default:
		printk(KERN_ERR PFX "oof bad type!\n");
		wake_up_interruptible(&cb->sem);
		break;
	}
	return 0;
}

static int client_recv(struct krping_cb *cb, struct ib_wc *wc)
{
	printk("coming into client recv");
	if (wc->byte_len != sizeof(cb->recv_buf))
	{
		printk(KERN_ERR PFX "Received bogus data, size %d\n",
			   wc->byte_len);
		return -1;
	}

	cb->remote_rkey = ntohl(cb->recv_buf.rkey);
	cb->remote_addr = ntohll(cb->recv_buf.buf);
	cb->remote_len = ntohl(cb->recv_buf.size);
	printk("Received rkey %x addr %llx len %d from peer\n",
		   cb->remote_rkey, (unsigned long long)cb->remote_addr,
		   cb->remote_len);

	if (cb->state <= CONNECTED || cb->state == RDMA_WRITE_COMPLETE)
		cb->state = RDMA_READ_ADV;
	else
		cb->state = RDMA_WRITE_ADV;

	return 0;
}

static int server_recv(struct krping_cb *cb, struct ib_wc *wc)
{
	if (wc->byte_len != sizeof(cb->recv_buf))
	{
		printk(KERN_ERR PFX "Received bogus data, size %d\n",
			   wc->byte_len);
		return -1;
	}

	if (cb->state == RDMA_READ_ADV)
		cb->state = RDMA_WRITE_ADV;
	else
		cb->state = RDMA_WRITE_COMPLETE;

	return 0;
}

static void krping_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct krping_cb *cb = ctx;
	struct ib_wc wc;
	const struct ib_recv_wr *bad_wr;
	int ret;

	// printk("coming into cq_event handler");

	BUG_ON(cb->cq != cq);
	if (cb->state == ERROR)
	{
		printk("cq completion in ERROR state\n");
		return;
	}
	if (cb->frtest)
	{
		printk("cq completion event in frtest!\n");
		return;
	}
	if (!cb->wlat && !cb->rlat && !cb->bw)
	{
		ret = ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
		// printk("ib_req_notify_cq: ret = %d", ret);
	}

	while ((ret = ib_poll_cq(cb->cq, 1, &wc)) == 1)
	{
		if (wc.status)
		{
			if (wc.status == IB_WC_WR_FLUSH_ERR)
			{
				DEBUG_LOG("cq flushed\n");
				continue;
			}
			else
			{
				printk(KERN_ERR PFX "cq completion failed with "
									"wr_id %Lx status %d opcode %d vender_err %x\n",
					   wc.wr_id, wc.status, wc.opcode, wc.vendor_err);
				goto error;
			}
		}

		switch (wc.opcode)
		{
		case IB_WC_SEND:
			printk("send completion\n");
			cb->stats.send_bytes += cb->send_sgl.length;
			cb->stats.send_msgs++;
			break;

		case IB_WC_RDMA_WRITE:
			printk("rdma write completion\n");
			cb->stats.write_bytes += cb->rdma_sq_wr.wr.sg_list->length;
			cb->stats.write_msgs++;
			cb->state = RDMA_WRITE_COMPLETE;
			wake_up_interruptible(&cb->sem);
			break;

		case IB_WC_RDMA_READ:
			printk("rdma read completion\n");
			cb->stats.read_bytes += cb->rdma_sq_wr.wr.sg_list->length;
			cb->stats.read_msgs++;
			cb->state = RDMA_READ_COMPLETE;
			wake_up_interruptible(&cb->sem);
			break;

		case IB_WC_RECV:
			printk("coiming into recv completion\n");
			cb->stats.recv_bytes += sizeof(cb->recv_buf);
			cb->stats.recv_msgs++;
			if (cb->wlat || cb->rlat || cb->bw)
				ret = server_recv(cb, &wc);
			else
				ret = cb->server ? server_recv(cb, &wc) : client_recv(cb, &wc);
			if (ret)
			{
				printk(KERN_ERR PFX "recv wc error: %d\n", ret);
				goto error;
			}

			ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
			if (ret)
			{
				printk(KERN_ERR PFX "post recv error: %d\n",
					   ret);
				goto error;
			}
			wake_up_interruptible(&cb->sem);
			break;

		default:
			printk(KERN_ERR PFX
				   "%s:%d Unexpected opcode %d, Shutting down\n",
				   __func__, __LINE__, wc.opcode);
			goto error;
		}
	}
	if (ret)
	{
		printk(KERN_ERR PFX "poll error %d\n", ret);
		goto error;
	}
	return;
error:
	cb->state = ERROR;
	wake_up_interruptible(&cb->sem);
}

static int krping_accept(struct krping_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	DEBUG_LOG("accepting client connection request\n");

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;

	ret = rdma_accept(cb->child_cm_id, &conn_param);
	if (ret)
	{
		printk(KERN_ERR PFX "rdma_accept error: %d\n", ret);
		return ret;
	}

	if (!cb->wlat && !cb->rlat && !cb->bw)
	{
		wait_event_interruptible(cb->sem, cb->state >= CONNECTED);
		if (cb->state == ERROR)
		{
			printk(KERN_ERR PFX "wait for CONNECTED state %d\n",
				   cb->state);
			return -1;
		}
	}
	return 0;
}

static void krping_setup_wr(struct krping_cb *cb)
{
	cb->recv_sgl.addr = cb->recv_dma_addr;
	cb->recv_sgl.length = sizeof cb->recv_buf;
	cb->recv_sgl.lkey = cb->pd->local_dma_lkey;

	cb->rq_wr.sg_list = &cb->recv_sgl;
	cb->rq_wr.num_sge = 1;

	cb->send_sgl.addr = cb->send_dma_addr;
	cb->send_sgl.length = sizeof cb->send_buf;
	cb->send_sgl.lkey = cb->pd->local_dma_lkey;

	cb->sq_wr.opcode = IB_WR_SEND;
	cb->sq_wr.send_flags = IB_SEND_SIGNALED;
	cb->sq_wr.sg_list = &cb->send_sgl;
	cb->sq_wr.num_sge = 1;

	if (!cb->server || cb->wlat || cb->rlat || cb->bw)
	{
		cb->rdma_sgl.addr = cb->rdma_dma_addr;
		cb->rdma_sq_wr.wr.send_flags = IB_SEND_SIGNALED;
		cb->rdma_sq_wr.wr.sg_list = &cb->rdma_sgl;
		cb->rdma_sq_wr.wr.num_sge = 1;
	}

	// 上述代码创建了两个工作请求 reg_mr_wr 和 invalidate_wr，它们构成了一个工作请求链（chain）。首先，reg_mr_wr 用于注册 RDMA 缓冲区（通常包括指定缓冲区的内存地址和长度），然后 invalidate_wr 用于执行本地失效操作。本地失效操作通常用于使缓冲区无效，以便在之后的迭代中重新注册并生成新的密钥，这有助于增加安全性和隐私性，因为只有知道密钥的节点才能访问 RDMA 缓冲区。这些工作请求将用于 RDMA 通信中的数据传输和内存注册操作。
	/*
	 * A chain of 2 WRs, INVALDATE_MR + REG_MR.
	 * both unsignaled.  The client uses them to reregister
	 * the rdma buffers with a new key each iteration.
	 */
	// 注册memory region
	cb->reg_mr_wr.wr.opcode = IB_WR_REG_MR;
	cb->reg_mr_wr.mr = cb->reg_mr;

	cb->invalidate_wr.next = &cb->reg_mr_wr.wr;
	cb->invalidate_wr.opcode = IB_WR_LOCAL_INV;
}

static int krping_setup_buffers(struct krping_cb *cb)
{
	int ret;

	DEBUG_LOG(PFX "krping_setup_buffers called on cb %p\n", cb);

	cb->recv_dma_addr = ib_dma_map_single(cb->pd->device,
										  &cb->recv_buf,
										  sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
	dma_unmap_addr_set(cb, recv_mapping, cb->recv_dma_addr);
	cb->send_dma_addr = ib_dma_map_single(cb->pd->device,
										  &cb->send_buf, sizeof(cb->send_buf),
										  DMA_BIDIRECTIONAL);
	dma_unmap_addr_set(cb, send_mapping, cb->send_dma_addr);

	cb->rdma_buf = ib_dma_alloc_coherent(cb->pd->device, cb->size,
										 &cb->rdma_dma_addr,
										 GFP_KERNEL);
	if (!cb->rdma_buf)
	{
		DEBUG_LOG(PFX "rdma_buf allocation failed\n");
		ret = -ENOMEM;
		goto bail;
	}
	dma_unmap_addr_set(cb, rdma_mapping, cb->rdma_dma_addr);
	cb->page_list_len = (((cb->size - 1) & PAGE_MASK) + PAGE_SIZE) >> PAGE_SHIFT;
	cb->reg_mr = ib_alloc_mr(cb->pd, IB_MR_TYPE_MEM_REG,
							 cb->page_list_len);
	if (IS_ERR(cb->reg_mr))
	{
		ret = PTR_ERR(cb->reg_mr);
		DEBUG_LOG(PFX "recv_buf reg_mr failed %d\n", ret);
		goto bail;
	}
	DEBUG_LOG(PFX "reg rkey 0x%x page_list_len %u\n",
			  cb->reg_mr->rkey, cb->page_list_len);

	if (!cb->server || cb->wlat || cb->rlat || cb->bw)
	{

		cb->start_buf = ib_dma_alloc_coherent(cb->pd->device, cb->size,
											  &cb->start_dma_addr,
											  GFP_KERNEL);
		if (!cb->start_buf)
		{
			DEBUG_LOG(PFX "start_buf malloc failed\n");
			ret = -ENOMEM;
			goto bail;
		}
		dma_unmap_addr_set(cb, start_mapping, cb->start_dma_addr);
	}

	krping_setup_wr(cb);
	DEBUG_LOG(PFX "allocated & registered buffers...\n");
	return 0;
bail:
	if (cb->reg_mr && !IS_ERR(cb->reg_mr))
		ib_dereg_mr(cb->reg_mr);
	if (cb->rdma_mr && !IS_ERR(cb->rdma_mr))
		ib_dereg_mr(cb->rdma_mr);
	if (cb->dma_mr && !IS_ERR(cb->dma_mr))
		ib_dereg_mr(cb->dma_mr);
	if (cb->rdma_buf)
	{
		ib_dma_free_coherent(cb->pd->device, cb->size, cb->rdma_buf,
							 cb->rdma_dma_addr);
	}
	if (cb->start_buf)
	{
		ib_dma_free_coherent(cb->pd->device, cb->size, cb->start_buf,
							 cb->start_dma_addr);
	}
	return ret;
}

static void krping_free_buffers(struct krping_cb *cb)
{
	// printk("krping_free_buffers called on cb %p\n", cb);

	if (cb->dma_mr)
		ib_dereg_mr(cb->dma_mr);
	if (cb->rdma_mr)
		ib_dereg_mr(cb->rdma_mr);
	if (cb->start_mr)
		ib_dereg_mr(cb->start_mr);
	if (cb->reg_mr)
		ib_dereg_mr(cb->reg_mr);

	dma_unmap_single(cb->pd->device->dma_device,
					 dma_unmap_addr(cb, recv_mapping),
					 sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
	dma_unmap_single(cb->pd->device->dma_device,
					 dma_unmap_addr(cb, send_mapping),
					 sizeof(cb->send_buf), DMA_BIDIRECTIONAL);

	ib_dma_free_coherent(cb->pd->device, cb->size, cb->rdma_buf,
						 cb->rdma_dma_addr);

	if (cb->start_buf)
	{
		ib_dma_free_coherent(cb->pd->device, cb->size, cb->start_buf,
							 cb->start_dma_addr);
	}
}

static int krping_create_qp(struct krping_cb *cb)
{
	struct ib_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = cb->txdepth; // 设置 IB 队列对的最大发送工作请求数量
	init_attr.cap.max_recv_wr = 2;			 // 置 IB 队列对的最大接收工作请求数量为2

	/* For flush_qp() */
	init_attr.cap.max_send_wr++; // 增加最大发送工作请求数量
	init_attr.cap.max_recv_wr++; // 增加最大接收工作请求数量

	init_attr.cap.max_recv_sge = 1;			  // 设置 IB 队列对的最大接收散射-聚集元素（Scatter-Gather Elements，简称SGE）数量为1
	init_attr.cap.max_send_sge = 1;			  // 设置 IB 队列对的最大发送SGE数量为1
	init_attr.qp_type = IB_QPT_RC;			  // 设置 IB 队列对的类型为 Reliable Connected (RC) 队列
	init_attr.send_cq = cb->cq;				  // 设置 IB 队列对的发送完成队列 (Completion Queue，简称CQ) 为 cb 结构体中的 cq 成员
	init_attr.recv_cq = cb->cq;				  // 设置 IB 队列对的接收完成队列为 cb 结构体中的 cq 成员
	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR; // 设置 IB 队列对的信号类型为请求写操作

	if (cb->server)
	{
		ret = rdma_create_qp(cb->child_cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->child_cm_id->qp;
	}
	else
	{
		ret = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->cm_id->qp;
	}

	return ret;
}

static void krping_free_qp(struct krping_cb *cb)
{
	ib_destroy_qp(cb->qp);
	ib_destroy_cq(cb->cq);
	ib_dealloc_pd(cb->pd);
	// printk("free qp over");
}

static int krping_setup_qp(struct krping_cb *cb, struct rdma_cm_id *cm_id)
{
	int ret;
	struct ib_cq_init_attr attr = {0};

	cb->pd = ib_alloc_pd(cm_id->device, 0);
	if (IS_ERR(cb->pd))
	{
		printk(KERN_ERR PFX "ib_alloc_pd failed\n");
		return PTR_ERR(cb->pd);
	}
	printk("Client created pd(krping_setup_qp) %p\n", cb->pd);

	attr.cqe = cb->txdepth * 2;
	attr.comp_vector = 0;
	cb->cq = ib_create_cq(cm_id->device, krping_cq_event_handler, NULL,
						  cb, &attr);
	if (IS_ERR(cb->cq))
	{
		printk(KERN_ERR PFX "ib_create_cq failed\n");
		ret = PTR_ERR(cb->cq);
		goto err1;
	}
	printk("Client created cq(krping_setup_qp) %p\n", cb->cq);

	if (!cb->wlat && !cb->rlat && !cb->bw && !cb->frtest)
	{
		ret = ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
		if (ret)
		{
			printk(KERN_ERR PFX "ib_create_cq failed\n");
			goto err2;
		}
	}

	ret = krping_create_qp(cb);
	if (ret)
	{
		printk(KERN_ERR PFX "krping_create_qp failed: %d\n", ret);
		goto err2;
	}
	DEBUG_LOG("created qp %p\n", cb->qp);
	return 0;
err2:
	ib_destroy_cq(cb->cq);
err1:
	ib_dealloc_pd(cb->pd);
	return ret;
}

/*
 * return the (possibly rebound) rkey for the rdma buffer.
 * REG mode: invalidate and rebind via reg wr.
 * other modes: just return the mr rkey.
 */
static u32 krping_rdma_rkey(struct krping_cb *cb, u64 buf, int post_inv)
{
	u32 rkey;
	const struct ib_send_wr *bad_wr;
	int ret;
	struct scatterlist sg = {0};

	cb->invalidate_wr.ex.invalidate_rkey = cb->reg_mr->rkey;

	/*
	 * Update the reg key.
	 */
	ib_update_fast_reg_key(cb->reg_mr, ++cb->key);
	cb->reg_mr_wr.key = cb->reg_mr->rkey;

	/*
	 * Update the reg WR with new buf info.
	 */
	if (buf == (u64)cb->start_dma_addr)
		cb->reg_mr_wr.access = IB_ACCESS_REMOTE_READ;
	else
		cb->reg_mr_wr.access = IB_ACCESS_REMOTE_WRITE | IB_ACCESS_LOCAL_WRITE;
	sg_dma_address(&sg) = buf;
	sg_dma_len(&sg) = cb->size;

	ret = ib_map_mr_sg(cb->reg_mr, &sg, 1, NULL, PAGE_SIZE);
	BUG_ON(ret <= 0 || ret > cb->page_list_len);

	printk(PFX "post_inv = %d, reg_mr new rkey 0x%x pgsz %u len %lu"
				  " iova_start %llx\n",
			  post_inv,
			  cb->reg_mr_wr.key,
			  cb->reg_mr->page_size,
			  (unsigned long)cb->reg_mr->length,
			  (unsigned long long)cb->reg_mr->iova);

	if (post_inv)
		ret = ib_post_send(cb->qp, &cb->invalidate_wr, &bad_wr);
	else
		ret = ib_post_send(cb->qp, &cb->reg_mr_wr.wr, &bad_wr);
	if (ret)
	{
		printk(PFX "krping_rdma_rkey post send error %d\n", ret);
		cb->state = ERROR;
	}
	rkey = cb->reg_mr->rkey;
	return rkey;
}

static void krping_format_send(struct krping_cb *cb, u64 buf)
{
	struct krping_rdma_info *info = &cb->send_buf;
	u32 rkey;

	/*
	 * Client side will do reg or mw bind before
	 * advertising the rdma buffer.  Server side
	 * sends have no data.
	 */
	if (!cb->server || cb->wlat || cb->rlat || cb->bw)
	{
		rkey = krping_rdma_rkey(cb, buf, !cb->server_invalidate);
		info->buf = htonll(buf);
		info->rkey = htonl(rkey);
		info->size = htonl(cb->size);
		DEBUG_LOG("RDMA addr %llx rkey %x len %d\n",
				  (unsigned long long)buf, rkey, cb->size);
	}
}

static void krping_test_server(struct krping_cb *cb)
{
	struct ib_send_wr inv;
	const struct ib_send_wr *bad_wr;
	int ret;

	while (1)
	{
		/* Wait for client's Start STAG/TO/Len */
		wait_event_interruptible(cb->sem, cb->state >= RDMA_READ_ADV);
		if (cb->state != RDMA_READ_ADV)
		{
			printk(KERN_ERR PFX "wait for RDMA_READ_ADV state %d\n",
				   cb->state);
			break;
		}

		DEBUG_LOG("server received sink adv\n");

		cb->rdma_sq_wr.rkey = cb->remote_rkey;
		cb->rdma_sq_wr.remote_addr = cb->remote_addr;
		cb->rdma_sq_wr.wr.sg_list->length = cb->remote_len;
		cb->rdma_sgl.lkey = krping_rdma_rkey(cb, cb->rdma_dma_addr, !cb->read_inv);
		cb->rdma_sq_wr.wr.next = NULL;

		/* Issue RDMA Read. */
		if (cb->read_inv)
			cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_READ_WITH_INV;
		else
		{

			cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_READ;
			/*
			 * Immediately follow the read with a
			 * fenced LOCAL_INV.
			 */
			cb->rdma_sq_wr.wr.next = &inv;
			memset(&inv, 0, sizeof inv);
			inv.opcode = IB_WR_LOCAL_INV;
			inv.ex.invalidate_rkey = cb->reg_mr->rkey;
			inv.send_flags = IB_SEND_FENCE;
		}

		ret = ib_post_send(cb->qp, &cb->rdma_sq_wr.wr, &bad_wr);
		if (ret)
		{
			printk(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}
		cb->rdma_sq_wr.wr.next = NULL;

		DEBUG_LOG("server posted rdma read req \n");

		/* Wait for read completion */
		wait_event_interruptible(cb->sem,
								 cb->state >= RDMA_READ_COMPLETE);
		if (cb->state != RDMA_READ_COMPLETE)
		{
			printk(KERN_ERR PFX
				   "wait for RDMA_READ_COMPLETE state %d\n",
				   cb->state);
			break;
		}
		DEBUG_LOG("server received read complete\n");

		/* Display data in recv buf */
		if (cb->verbose)
			printk(KERN_INFO PFX
				   "server ping data (64B max): |%.64s|\n",
				   cb->rdma_buf);

		/* Tell client to continue */
		if (cb->server && cb->server_invalidate)
		{
			cb->sq_wr.ex.invalidate_rkey = cb->remote_rkey;
			cb->sq_wr.opcode = IB_WR_SEND_WITH_INV;
			DEBUG_LOG("send-w-inv rkey 0x%x\n", cb->remote_rkey);
		}
		ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret)
		{
			printk(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}
		DEBUG_LOG("server posted go ahead\n");

		/* Wait for client's RDMA STAG/TO/Len */
		wait_event_interruptible(cb->sem, cb->state >= RDMA_WRITE_ADV);
		if (cb->state != RDMA_WRITE_ADV)
		{
			printk(KERN_ERR PFX
				   "wait for RDMA_WRITE_ADV state %d\n",
				   cb->state);
			break;
		}
		DEBUG_LOG("server received sink adv\n");

		/* RDMA Write echo data */
		cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_WRITE;
		cb->rdma_sq_wr.rkey = cb->remote_rkey;
		cb->rdma_sq_wr.remote_addr = cb->remote_addr;
		cb->rdma_sq_wr.wr.sg_list->length = strlen(cb->rdma_buf) + 1;
		if (cb->local_dma_lkey)
			cb->rdma_sgl.lkey = cb->pd->local_dma_lkey;
		else
			cb->rdma_sgl.lkey = krping_rdma_rkey(cb, cb->rdma_dma_addr, 0);

		DEBUG_LOG("rdma write from lkey %x laddr %llx len %d\n",
				  cb->rdma_sq_wr.wr.sg_list->lkey,
				  (unsigned long long)cb->rdma_sq_wr.wr.sg_list->addr,
				  cb->rdma_sq_wr.wr.sg_list->length);

		ret = ib_post_send(cb->qp, &cb->rdma_sq_wr.wr, &bad_wr);
		if (ret)
		{
			printk(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}

		/* Wait for completion */
		ret = wait_event_interruptible(cb->sem, cb->state >=
													RDMA_WRITE_COMPLETE);
		if (cb->state != RDMA_WRITE_COMPLETE)
		{
			printk(KERN_ERR PFX
				   "wait for RDMA_WRITE_COMPLETE state %d\n",
				   cb->state);
			break;
		}
		DEBUG_LOG("server rdma write complete \n");

		cb->state = CONNECTED;

		/* Tell client to begin again */
		if (cb->server && cb->server_invalidate)
		{
			cb->sq_wr.ex.invalidate_rkey = cb->remote_rkey;
			cb->sq_wr.opcode = IB_WR_SEND_WITH_INV;
			DEBUG_LOG("send-w-inv rkey 0x%x\n", cb->remote_rkey);
		}
		ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret)
		{
			printk(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}
		DEBUG_LOG("server posted go ahead\n");
	}
}

static void rlat_test(struct krping_cb *cb)
{
	int scnt;
	int iters = cb->count;
	ktime_t start, stop;
	int ret;
	struct ib_wc wc;
	const struct ib_send_wr *bad_wr;
	int ne;

	scnt = 0;
	cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_READ;
	cb->rdma_sq_wr.rkey = cb->remote_rkey;
	cb->rdma_sq_wr.remote_addr = cb->remote_addr;
	cb->rdma_sq_wr.wr.sg_list->length = cb->size;

	start = ktime_get();
	if (!cb->poll)
	{
		cb->state = RDMA_READ_ADV;
		ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
	}
	while (scnt < iters)
	{

		cb->state = RDMA_READ_ADV;
		ret = ib_post_send(cb->qp, &cb->rdma_sq_wr.wr, &bad_wr);
		if (ret)
		{
			printk(KERN_ERR PFX
				   "Couldn't post send: ret=%d scnt %d\n",
				   ret, scnt);
			return;
		}

		do
		{
			if (!cb->poll)
			{
				wait_event_interruptible(cb->sem,
										 cb->state != RDMA_READ_ADV);
				if (cb->state == RDMA_READ_COMPLETE)
				{
					ne = 1;
					ib_req_notify_cq(cb->cq,
									 IB_CQ_NEXT_COMP);
				}
				else
				{
					ne = -1;
				}
			}
			else
				ne = ib_poll_cq(cb->cq, 1, &wc);
			if (cb->state == ERROR)
			{
				printk(KERN_ERR PFX
					   "state == ERROR...bailing scnt %d\n",
					   scnt);
				return;
			}
		} while (ne == 0);

		if (ne < 0)
		{
			printk(KERN_ERR PFX "poll CQ failed %d\n", ne);
			return;
		}
		if (cb->poll && wc.status != IB_WC_SUCCESS)
		{
			printk(KERN_ERR PFX "Completion wth error at %s:\n",
				   cb->server ? "server" : "client");
			printk(KERN_ERR PFX "Failed status %d: wr_id %d\n",
				   wc.status, (int)wc.wr_id);
			return;
		}
		++scnt;
	}
	stop = ktime_get();

	printk(KERN_ERR PFX "delta nsec %llu iter %d size %d\n",
		   ktime_sub(stop, start),
		   scnt, cb->size);
}

static void wlat_test(struct krping_cb *cb)
{
	int ccnt, scnt, rcnt;
	int iters = cb->count;
	volatile char *poll_buf = (char *)cb->start_buf;
	char *buf = (char *)cb->rdma_buf;
	ktime_t start, stop;
	cycles_t *post_cycles_start = NULL;
	cycles_t *post_cycles_stop = NULL;
	cycles_t *poll_cycles_start = NULL;
	cycles_t *poll_cycles_stop = NULL;
	cycles_t *last_poll_cycles_start = NULL;
	cycles_t sum_poll = 0, sum_post = 0, sum_last_poll = 0;
	int i;
	int cycle_iters = 1000;

	ccnt = 0;
	scnt = 0;
	rcnt = 0;

	post_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!post_cycles_start)
	{
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		goto done;
	}
	post_cycles_stop = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!post_cycles_stop)
	{
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		goto done;
	}
	poll_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!poll_cycles_start)
	{
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		goto done;
	}
	poll_cycles_stop = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!poll_cycles_stop)
	{
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		goto done;
	}
	last_poll_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t),
									 GFP_KERNEL);
	if (!last_poll_cycles_start)
	{
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		goto done;
	}
	cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_WRITE;
	cb->rdma_sq_wr.rkey = cb->remote_rkey;
	cb->rdma_sq_wr.remote_addr = cb->remote_addr;
	cb->rdma_sq_wr.wr.sg_list->length = cb->size;

	if (cycle_iters > iters)
		cycle_iters = iters;
	start = ktime_get();
	while (scnt < iters || ccnt < iters || rcnt < iters)
	{

		/* Wait till buffer changes. */
		if (rcnt < iters && !(scnt < 1 && !cb->server))
		{
			++rcnt;
			while (*poll_buf != (char)rcnt)
			{
				if (cb->state == ERROR)
				{
					printk(KERN_ERR PFX
						   "state = ERROR, bailing\n");
					goto done;
				}
			}
		}

		if (scnt < iters)
		{
			const struct ib_send_wr *bad_wr;

			*buf = (char)scnt + 1;
			if (scnt < cycle_iters)
				post_cycles_start[scnt] = get_cycles();
			if (ib_post_send(cb->qp, &cb->rdma_sq_wr.wr, &bad_wr))
			{
				printk(KERN_ERR PFX
					   "Couldn't post send: scnt=%d\n",
					   scnt);
				goto done;
			}
			if (scnt < cycle_iters)
				post_cycles_stop[scnt] = get_cycles();
			scnt++;
		}

		if (ccnt < iters)
		{
			struct ib_wc wc;
			int ne;

			if (ccnt < cycle_iters)
				poll_cycles_start[ccnt] = get_cycles();
			do
			{
				if (ccnt < cycle_iters)
					last_poll_cycles_start[ccnt] =
						get_cycles();
				ne = ib_poll_cq(cb->cq, 1, &wc);
			} while (ne == 0);
			if (ccnt < cycle_iters)
				poll_cycles_stop[ccnt] = get_cycles();
			++ccnt;

			if (ne < 0)
			{
				printk(KERN_ERR PFX "poll CQ failed %d\n", ne);
				goto done;
			}
			if (wc.status != IB_WC_SUCCESS)
			{
				printk(KERN_ERR PFX
					   "Completion wth error at %s:\n",
					   cb->server ? "server" : "client");
				printk(KERN_ERR PFX
					   "Failed status %d: wr_id %d\n",
					   wc.status, (int)wc.wr_id);
				printk(KERN_ERR PFX
					   "scnt=%d, rcnt=%d, ccnt=%d\n",
					   scnt, rcnt, ccnt);
				goto done;
			}
		}
	}
	stop = ktime_get();

	for (i = 0; i < cycle_iters; i++)
	{
		sum_post += post_cycles_stop[i] - post_cycles_start[i];
		sum_poll += poll_cycles_stop[i] - poll_cycles_start[i];
		sum_last_poll += poll_cycles_stop[i] - last_poll_cycles_start[i];
	}
	printk(KERN_ERR PFX
		   "delta nsec %llu iter %d size %d cycle_iters %d"
		   " sum_post %llu sum_poll %llu sum_last_poll %llu\n",
		   ktime_sub(stop, start),
		   scnt, cb->size, cycle_iters,
		   (unsigned long long)sum_post, (unsigned long long)sum_poll,
		   (unsigned long long)sum_last_poll);
done:
	kfree(post_cycles_start);
	kfree(post_cycles_stop);
	kfree(poll_cycles_start);
	kfree(poll_cycles_stop);
	kfree(last_poll_cycles_start);
}

static void bw_test(struct krping_cb *cb)
{
	int ccnt, scnt, rcnt;
	int iters = cb->count;
	ktime_t start, stop;
	cycles_t *post_cycles_start = NULL;
	cycles_t *post_cycles_stop = NULL;
	cycles_t *poll_cycles_start = NULL;
	cycles_t *poll_cycles_stop = NULL;
	cycles_t *last_poll_cycles_start = NULL;
	cycles_t sum_poll = 0, sum_post = 0, sum_last_poll = 0;
	int i;
	int cycle_iters = 1000;

	ccnt = 0;
	scnt = 0;
	rcnt = 0;

	post_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!post_cycles_start)
	{
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		goto done;
	}
	post_cycles_stop = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!post_cycles_stop)
	{
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		goto done;
	}
	poll_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!poll_cycles_start)
	{
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		goto done;
	}
	poll_cycles_stop = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!poll_cycles_stop)
	{
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		goto done;
	}
	last_poll_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t),
									 GFP_KERNEL);
	if (!last_poll_cycles_start)
	{
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		goto done;
	}
	cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_WRITE;
	cb->rdma_sq_wr.rkey = cb->remote_rkey;
	cb->rdma_sq_wr.remote_addr = cb->remote_addr;
	cb->rdma_sq_wr.wr.sg_list->length = cb->size;

	if (cycle_iters > iters)
		cycle_iters = iters;
	start = ktime_get();
	while (scnt < iters || ccnt < iters)
	{

		while (scnt < iters && scnt - ccnt < cb->txdepth)
		{
			const struct ib_send_wr *bad_wr;

			if (scnt < cycle_iters)
				post_cycles_start[scnt] = get_cycles();
			if (ib_post_send(cb->qp, &cb->rdma_sq_wr.wr, &bad_wr))
			{
				printk(KERN_ERR PFX
					   "Couldn't post send: scnt=%d\n",
					   scnt);
				goto done;
			}
			if (scnt < cycle_iters)
				post_cycles_stop[scnt] = get_cycles();
			++scnt;
		}

		if (ccnt < iters)
		{
			int ne;
			struct ib_wc wc;

			if (ccnt < cycle_iters)
				poll_cycles_start[ccnt] = get_cycles();
			do
			{
				if (ccnt < cycle_iters)
					last_poll_cycles_start[ccnt] =
						get_cycles();
				ne = ib_poll_cq(cb->cq, 1, &wc);
			} while (ne == 0);
			if (ccnt < cycle_iters)
				poll_cycles_stop[ccnt] = get_cycles();
			ccnt += 1;

			if (ne < 0)
			{
				printk(KERN_ERR PFX "poll CQ failed %d\n", ne);
				goto done;
			}
			if (wc.status != IB_WC_SUCCESS)
			{
				printk(KERN_ERR PFX
					   "Completion wth error at %s:\n",
					   cb->server ? "server" : "client");
				printk(KERN_ERR PFX
					   "Failed status %d: wr_id %d\n",
					   wc.status, (int)wc.wr_id);
				goto done;
			}
		}
	}
	stop = ktime_get();

	for (i = 0; i < cycle_iters; i++)
	{
		sum_post += post_cycles_stop[i] - post_cycles_start[i];
		sum_poll += poll_cycles_stop[i] - poll_cycles_start[i];
		sum_last_poll += poll_cycles_stop[i] - last_poll_cycles_start[i];
	}
	printk(KERN_ERR PFX
		   "delta nsec %llu iter %d size %d cycle_iters %d"
		   " sum_post %llu sum_poll %llu sum_last_poll %llu\n",
		   ktime_sub(stop, start), scnt, cb->size, cycle_iters,
		   (unsigned long long)sum_post, (unsigned long long)sum_poll,
		   (unsigned long long)sum_last_poll);
done:
	kfree(post_cycles_start);
	kfree(post_cycles_stop);
	kfree(poll_cycles_start);
	kfree(poll_cycles_stop);
	kfree(last_poll_cycles_start);
}

static void krping_rlat_test_server(struct krping_cb *cb)
{
	const struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	/* Spin waiting for client's Start STAG/TO/Len */
	while (cb->state < RDMA_READ_ADV)
	{
		krping_cq_event_handler(cb->cq, cb);
	}

	/* Send STAG/TO/Len to client */
	krping_format_send(cb, cb->start_dma_addr);
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret)
	{
		printk(KERN_ERR PFX "post send error %d\n", ret);
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0))
		;
	if (ret < 0)
	{
		printk(KERN_ERR PFX "poll error %d\n", ret);
		return;
	}
	if (wc.status)
	{
		printk(KERN_ERR PFX "send completiong error %d\n", wc.status);
		return;
	}

	wait_event_interruptible(cb->sem, cb->state == ERROR);
}

static void krping_wlat_test_server(struct krping_cb *cb)
{
	const struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	/* Spin waiting for client's Start STAG/TO/Len */
	while (cb->state < RDMA_READ_ADV)
	{
		krping_cq_event_handler(cb->cq, cb);
	}

	/* Send STAG/TO/Len to client */
	krping_format_send(cb, cb->start_dma_addr);
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret)
	{
		printk(KERN_ERR PFX "post send error %d\n", ret);
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0))
		;
	if (ret < 0)
	{
		printk(KERN_ERR PFX "poll error %d\n", ret);
		return;
	}
	if (wc.status)
	{
		printk(KERN_ERR PFX "send completiong error %d\n", wc.status);
		return;
	}

	wlat_test(cb);
	wait_event_interruptible(cb->sem, cb->state == ERROR);
}

static void krping_bw_test_server(struct krping_cb *cb)
{
	const struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	/* Spin waiting for client's Start STAG/TO/Len */
	while (cb->state < RDMA_READ_ADV)
	{
		krping_cq_event_handler(cb->cq, cb);
	}

	/* Send STAG/TO/Len to client */
	krping_format_send(cb, cb->start_dma_addr);
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret)
	{
		printk(KERN_ERR PFX "post send error %d\n", ret);
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0))
		;
	if (ret < 0)
	{
		printk(KERN_ERR PFX "poll error %d\n", ret);
		return;
	}
	if (wc.status)
	{
		printk(KERN_ERR PFX "send completiong error %d\n", wc.status);
		return;
	}

	if (cb->duplex)
		bw_test(cb);
	wait_event_interruptible(cb->sem, cb->state == ERROR);
}

static int reg_supported(struct ib_device *dev)
{
	u64 needed_flags = IB_DEVICE_MEM_MGT_EXTENSIONS;

	if ((dev->attrs.device_cap_flags & needed_flags) != needed_flags)
	{
		printk(KERN_ERR PFX
			   "Fastreg not supported - device_cap_flags 0x%llx\n",
			   (unsigned long long)dev->attrs.device_cap_flags);
		return 0;
	}
	DEBUG_LOG("Fastreg supported - device_cap_flags 0x%llx\n",
			  (unsigned long long)dev->attrs.device_cap_flags);
	return 1;
}

static void fill_sockaddr(struct sockaddr_storage *sin, struct krping_cb *cb)
{
	memset(sin, 0, sizeof(*sin));

	if (cb->addr_type == AF_INET)
	{
		struct sockaddr_in *sin4 = (struct sockaddr_in *)sin;
		sin4->sin_family = AF_INET;
		memcpy((void *)&sin4->sin_addr.s_addr, cb->addr, 4);
		sin4->sin_port = cb->port;
	}
	else if (cb->addr_type == AF_INET6)
	{
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)sin;
		sin6->sin6_family = AF_INET6;
		memcpy((void *)&sin6->sin6_addr, cb->addr, 16);
		sin6->sin6_port = cb->port;
		if (cb->ip6_ndev_name[0] != 0)
		{
			struct net_device *ndev;

			ndev = __dev_get_by_name(&init_net, cb->ip6_ndev_name);
			if (ndev != NULL)
			{
				sin6->sin6_scope_id = ndev->ifindex;
				dev_put(ndev);
			}
		}
	}
}

static int krping_bind_server(struct krping_cb *cb)
{
	struct sockaddr_storage sin;
	int ret;

	fill_sockaddr(&sin, cb);

	ret = rdma_bind_addr(cb->cm_id, (struct sockaddr *)&sin);
	if (ret)
	{
		printk(KERN_ERR PFX "rdma_bind_addr error %d\n", ret);
		return ret;
	}
	DEBUG_LOG("rdma_bind_addr successful\n");

	DEBUG_LOG("rdma_listen\n");
	ret = rdma_listen(cb->cm_id, 3);
	if (ret)
	{
		printk(KERN_ERR PFX "rdma_listen failed: %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= CONNECT_REQUEST);
	if (cb->state != CONNECT_REQUEST)
	{
		printk(KERN_ERR PFX "wait for CONNECT_REQUEST state %d\n",
			   cb->state);
		return -1;
	}

	if (!reg_supported(cb->child_cm_id->device))
		return -EINVAL;

	return 0;
}

static void krping_run_server(struct krping_cb *cb)
{
	const struct ib_recv_wr *bad_wr;
	int ret;

	ret = krping_bind_server(cb);
	if (ret)
		return;

	ret = krping_setup_qp(cb, cb->child_cm_id);
	if (ret)
	{
		printk(KERN_ERR PFX "setup_qp failed: %d\n", ret);
		goto err0;
	}

	ret = krping_setup_buffers(cb);
	if (ret)
	{
		printk(KERN_ERR PFX "krping_setup_buffers failed: %d\n", ret);
		goto err1;
	}

	ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
	if (ret)
	{
		printk(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
		goto err2;
	}

	ret = krping_accept(cb);
	if (ret)
	{
		printk(KERN_ERR PFX "connect error %d\n", ret);
		goto err2;
	}

	if (cb->wlat)
		krping_wlat_test_server(cb);
	else if (cb->rlat)
		krping_rlat_test_server(cb);
	else if (cb->bw)
		krping_bw_test_server(cb);
	else
		krping_test_server(cb);
	rdma_disconnect(cb->child_cm_id);

err2:
	krping_free_buffers(cb);
err1:
	krping_free_qp(cb);
err0:
	rdma_destroy_id(cb->child_cm_id);
}

static void krping_test_client_wr(struct krping_cb *cb)
{
	int ret;
	struct ib_send_wr inv;
	const struct ib_send_wr *bad_wr;
	printk("client successfully coming into test_client_rdma_write");
	cb->state = RDMA_WRITE_ADV;

	// printk("cb state %d", cb->state);
	// wait_event_interruptible(cb->sem, cb->state >=
	// 					 RDMA_WRITE_ADV);
	// cb->state = RDMA_WRITE_ADV;
	// printk("cb state %d", cb->state);
	// wake_up_interruptible(&cb->sem);

	int i, start, c;
	start = 65;
	// for (i = 0, c = start; i < cb->size; i++)
	// {
	// 	cb->rdma_buf[i] = c;
	// 	c++;
	// 	if (c > 122)
	// 		c = 65;
	// }
	// cb->rdma_buf[cb->size - 1] = 0;
	printk("before interruptible");
	wait_event_interruptible(cb->sem, flag == 1);
	printk("after interruptible");

	flag = 0;

	// 根据虚拟地址向rdma_buf中写入数据
	memcpy(cb->rdma_buf, buffer, buffer_index);
	printk("successfully copy data via virt addr\n");
	printk("client ping data : |%s|\n",
		   cb->rdma_buf);
	// 释放缓冲区
	kfree(buffer);

	cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_WRITE;
	cb->rdma_sq_wr.rkey = 0x01;
	cb->rdma_sq_wr.remote_addr = 0x61000000;
	// cb->rdma_sq_wr.wr.sg_list->length = strlen(cb->rdma_buf) + 1;
	cb->rdma_sq_wr.wr.sg_list->length = cb->size;
	if (cb->local_dma_lkey)
		cb->rdma_sgl.lkey = cb->pd->local_dma_lkey;
	else
	{
		// printk("2");
		cb->rdma_sgl.lkey = krping_rdma_rkey(cb, cb->rdma_dma_addr, 1);
	}

	printk("rdma write from lkey %x laddr %llx len %d\n",
		   cb->rdma_sq_wr.wr.sg_list->lkey,
		   (unsigned long long)cb->rdma_sq_wr.wr.sg_list->addr,
		   cb->rdma_sq_wr.wr.sg_list->length);

	ret = ib_post_send(cb->qp, &cb->rdma_sq_wr.wr, &bad_wr);
	if (ret)
	{
		printk(KERN_ERR PFX "post send error %d\n", ret);
	}

	/* Wait for completion */
	ret = wait_event_interruptible(cb->sem, cb->state >=
												RDMA_WRITE_COMPLETE);
	if (cb->state != RDMA_WRITE_COMPLETE)
	{
		printk(KERN_ERR PFX
			   "wait for RDMA_WRITE_COMPLETE state %d\n",
			   cb->state);
	}
	DEBUG_LOG("server rdma write complete \n");
}
static void krping_test_client_rd(struct krping_cb *cb)
{
	int ret;
	struct ib_send_wr inv;
	const struct ib_send_wr *bad_wr;
	printk("client successfully coming into test_client_rdma_read");
	cb->state = RDMA_READ_ADV;
	// wait_event_interruptible(cb->sem, cb->state >= RDMA_READ_ADV);
	// 	if (cb->state != RDMA_READ_ADV) {
	// 		printk("wait for RDMA_READ_ADV state %d\n",
	// 			cb->state);
	// 	}
	// printk("Client successfully recv interact information from client");

	// DEBUG_LOG("server received sink adv\n");
	// printk("Now client state = %d",cb->state);

	cb->rdma_sq_wr.rkey = 0x01;
	cb->rdma_sq_wr.remote_addr = 0x61000000;
	cb->rdma_sq_wr.wr.sg_list->length = cb->size;
	cb->rdma_sgl.lkey = krping_rdma_rkey(cb, cb->rdma_dma_addr, !cb->read_inv);
	cb->rdma_sq_wr.wr.next = NULL;

	/* Issue RDMA Read. */
	if (cb->read_inv)
		cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_READ_WITH_INV;
	else
	{
		// printk("1");
		cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_READ;
	}

	ret = ib_post_send(cb->qp, &cb->rdma_sq_wr.wr, &bad_wr);
	if (ret)
	{
		printk(KERN_ERR PFX "post send error %d\n", ret);
	}
	cb->rdma_sq_wr.wr.next = NULL;
	printk("rdma read from lkey %x laddr %llx len %d\n",
		   cb->rdma_sq_wr.wr.sg_list->lkey,
		   (unsigned long long)cb->rdma_sq_wr.wr.sg_list->addr,
		   cb->rdma_sq_wr.wr.sg_list->length);
	// printk("client posted rdma read req \n");

	/* Wait for read completion */
	wait_event_interruptible(cb->sem,
							 cb->state >= RDMA_READ_COMPLETE);
	if (cb->state != RDMA_READ_COMPLETE)
	{
		printk(KERN_ERR PFX
			   "wait for RDMA_READ_COMPLETE state %d\n",
			   cb->state);
	}
	else
	{
		DEBUG_LOG("server received read complete\n");
		printk("server pong data : |%s|\n",
			   cb->rdma_buf);
	}

#ifdef SLOW_KRPING
	wait_event_interruptible_timeout(cb->sem, cb->state == ERROR, HZ);
#endif
}

static void krping_test_client(struct krping_cb *cb) {
	int ret;
	struct ib_send_wr inv;
	const struct ib_send_wr *bad_wr;

	while (1) {
		printk("client successfully coming into test_client_rdma_write");
		cb->state = RDMA_WRITE_ADV;

		// printk("cb state %d", cb->state);
		// wait_event_interruptible(cb->sem, cb->state >=
		// 					 RDMA_WRITE_ADV);
		// cb->state = RDMA_WRITE_ADV;
		// printk("cb state %d", cb->state);
		// wake_up_interruptible(&cb->sem);

		int i, start, c;
		start = 65;
		// for (i = 0, c = start; i < cb->size; i++)
		// {
		// 	cb->rdma_buf[i] = c;
		// 	c++;
		// 	if (c > 122)
		// 		c = 65;
		// }
		// cb->rdma_buf[cb->size - 1] = 0;
		printk("before interruptible");
		wait_event_interruptible(cb->sem, flag == 1);
		printk("after interruptible");

		flag = 0;

		// 根据虚拟地址向rdma_buf中写入数据
		memcpy(cb->rdma_buf, buffer, buffer_index);
		printk("successfully copy data via virt addr\n");
		printk("client ping data : |%s|\n",
			cb->rdma_buf);
		// 释放缓冲区
		kfree(buffer);

		cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_WRITE;
		cb->rdma_sq_wr.rkey = 0x01;
		cb->rdma_sq_wr.remote_addr = 0x61000000;
		// cb->rdma_sq_wr.wr.sg_list->length = strlen(cb->rdma_buf) + 1;
		cb->rdma_sq_wr.wr.sg_list->length = cb->size;
		if (cb->local_dma_lkey)
			cb->rdma_sgl.lkey = cb->pd->local_dma_lkey;
		else
		{
			// printk("2");
			cb->rdma_sgl.lkey = krping_rdma_rkey(cb, cb->rdma_dma_addr, 1);			//1
		}

		printk("rdma write from lkey %x laddr %llx len %d\n",
			cb->rdma_sq_wr.wr.sg_list->lkey,
			(unsigned long long)cb->rdma_sq_wr.wr.sg_list->addr,
			cb->rdma_sq_wr.wr.sg_list->length);

		ret = ib_post_send(cb->qp, &cb->rdma_sq_wr.wr, &bad_wr);
		if (ret)
		{
			printk(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}

		/* Wait for completion */
		ret = wait_event_interruptible(cb->sem, cb->state >=
													RDMA_WRITE_COMPLETE);
		if (cb->state != RDMA_WRITE_COMPLETE)
		{
			printk(KERN_ERR PFX
				"wait for RDMA_WRITE_COMPLETE state %d\n",
				cb->state);
			break;
		}

		printk("client successfully coming into test_client_rdma_read");
		cb->state = RDMA_READ_ADV;
		// wait_event_interruptible(cb->sem, cb->state >= RDMA_READ_ADV);
		// 	if (cb->state != RDMA_READ_ADV) {
		// 		printk("wait for RDMA_READ_ADV state %d\n",
		// 			cb->state);
		// 	}
		// printk("Client successfully recv interact information from client");

		// DEBUG_LOG("server received sink adv\n");
		// printk("Now client state = %d",cb->state);

		cb->rdma_sq_wr.rkey = 0x01;
		cb->rdma_sq_wr.remote_addr = 0x61000000;
		cb->rdma_sq_wr.wr.sg_list->length = cb->size;
		cb->rdma_sgl.lkey = krping_rdma_rkey(cb, cb->rdma_dma_addr, !cb->read_inv);
		cb->rdma_sq_wr.wr.next = NULL;

		/* Issue RDMA Read. */
		if (cb->read_inv)
			cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_READ_WITH_INV;
		else
		{
			// printk("1");
			cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_READ;
		}

		ret = ib_post_send(cb->qp, &cb->rdma_sq_wr.wr, &bad_wr);
		if (ret)
		{
			printk(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}
		cb->rdma_sq_wr.wr.next = NULL;
		printk("rdma read from lkey %x laddr %llx len %d\n",
			cb->rdma_sq_wr.wr.sg_list->lkey,
			(unsigned long long)cb->rdma_sq_wr.wr.sg_list->addr,
			cb->rdma_sq_wr.wr.sg_list->length);
		// printk("client posted rdma read req \n");

		/* Wait for read completion */
		wait_event_interruptible(cb->sem,
								cb->state >= RDMA_READ_COMPLETE);
		if (cb->state != RDMA_READ_COMPLETE)
		{
			printk(KERN_ERR PFX
				"wait for RDMA_READ_COMPLETE state %d\n",
				cb->state);
			break;
		}
		else
		{
			DEBUG_LOG("server received read complete\n");
			printk("server pong data : |%s|\n",
				cb->rdma_buf);
		}
	}
}

static void krping_rlat_test_client(struct krping_cb *cb)
{
	const struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	cb->state = RDMA_READ_ADV;

	/* Send STAG/TO/Len to client */
	krping_format_send(cb, cb->start_dma_addr);
	if (cb->state == ERROR)
	{
		printk(KERN_ERR PFX "krping_format_send failed\n");
		return;
	}
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret)
	{
		printk(KERN_ERR PFX "post send error %d\n", ret);
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0))
		;
	if (ret < 0)
	{
		printk(KERN_ERR PFX "poll error %d\n", ret);
		return;
	}
	if (wc.status)
	{
		printk(KERN_ERR PFX "send completion error %d\n", wc.status);
		return;
	}

	/* Spin waiting for server's Start STAG/TO/Len */
	while (cb->state < RDMA_WRITE_ADV)
	{
		krping_cq_event_handler(cb->cq, cb);
	}

#if 0
{
	int i;
	ktime_t start, stop;
	time_t sec;
	suseconds_t usec;
	unsigned long long elapsed;
	struct ib_wc wc;
	struct ib_send_wr *bad_wr;
	int ne;
	
	cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_WRITE;
	cb->rdma_sq_wr.rkey = cb->remote_rkey;
	cb->rdma_sq_wr.remote_addr = cb->remote_addr;
	cb->rdma_sq_wr.wr.sg_list->length = 0;
	cb->rdma_sq_wr.wr.num_sge = 0;

	start = ktime_get();
	for (i=0; i < 100000; i++) {
		if (ib_post_send(cb->qp, &cb->rdma_sq_wr.wr, &bad_wr)) {
			printk(KERN_ERR PFX  "Couldn't post send\n");
			return;
		}
		do {
			ne = ib_poll_cq(cb->cq, 1, &wc);
		} while (ne == 0);
		if (ne < 0) {
			printk(KERN_ERR PFX "poll CQ failed %d\n", ne);
			return;
		}
		if (wc.status != IB_WC_SUCCESS) {
			printk(KERN_ERR PFX "Completion wth error at %s:\n",
				cb->server ? "server" : "client");
			printk(KERN_ERR PFX "Failed status %d: wr_id %d\n",
				wc.status, (int) wc.wr_id);
			return;
		}
	}
	stop = ktime_get();
	
	if (stop.tv_usec < start.tv_usec) {
		stop.tv_usec += 1000000;
		stop.tv_sec  -= 1;
	}
	sec     = stop.tv_sec - start.tv_sec;
	usec    = stop.tv_usec - start.tv_usec;
	elapsed = sec * 1000000 + usec;
	printk(KERN_ERR PFX "0B-write-lat iters 100000 usec %llu\n", elapsed);
}
#endif

	rlat_test(cb);
}

static void krping_wlat_test_client(struct krping_cb *cb)
{
	const struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	cb->state = RDMA_READ_ADV;

	/* Send STAG/TO/Len to client */
	krping_format_send(cb, cb->start_dma_addr);
	if (cb->state == ERROR)
	{
		printk(KERN_ERR PFX "krping_format_send failed\n");
		return;
	}
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret)
	{
		printk(KERN_ERR PFX "post send error %d\n", ret);
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0))
		;
	if (ret < 0)
	{
		printk(KERN_ERR PFX "poll error %d\n", ret);
		return;
	}
	if (wc.status)
	{
		printk(KERN_ERR PFX "send completion error %d\n", wc.status);
		return;
	}

	/* Spin waiting for server's Start STAG/TO/Len */
	while (cb->state < RDMA_WRITE_ADV)
	{
		krping_cq_event_handler(cb->cq, cb);
	}

	wlat_test(cb);
}

static void krping_bw_test_client(struct krping_cb *cb)
{
	const struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	cb->state = RDMA_READ_ADV;

	/* Send STAG/TO/Len to client */
	krping_format_send(cb, cb->start_dma_addr);
	if (cb->state == ERROR)
	{
		printk(KERN_ERR PFX "krping_format_send failed\n");
		return;
	}
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret)
	{
		printk(KERN_ERR PFX "post send error %d\n", ret);
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0))
		;
	if (ret < 0)
	{
		printk(KERN_ERR PFX "poll error %d\n", ret);
		return;
	}
	if (wc.status)
	{
		printk(KERN_ERR PFX "send completion error %d\n", wc.status);
		return;
	}

	/* Spin waiting for server's Start STAG/TO/Len */
	while (cb->state < RDMA_WRITE_ADV)
	{
		krping_cq_event_handler(cb->cq, cb);
	}

	bw_test(cb);
}

/*
 * Manual qp flush test
 */
static void flush_qp(struct krping_cb *cb)
{
	struct ib_send_wr wr = {0};
	const struct ib_send_wr *bad;
	struct ib_recv_wr recv_wr = {0};
	const struct ib_recv_wr *recv_bad;
	struct ib_wc wc;
	int ret;
	int flushed = 0;
	int ccnt = 0;

	rdma_disconnect(cb->cm_id);
	DEBUG_LOG("disconnected!\n");

	wr.opcode = IB_WR_SEND;
	wr.wr_id = 0xdeadbeefcafebabe;
	ret = ib_post_send(cb->qp, &wr, &bad);
	if (ret)
	{
		printk(KERN_ERR PFX "%s post_send failed ret %d\n", __func__, ret);
		return;
	}

	recv_wr.wr_id = 0xcafebabedeadbeef;
	ret = ib_post_recv(cb->qp, &recv_wr, &recv_bad);
	if (ret)
	{
		printk(KERN_ERR PFX "%s post_recv failed ret %d\n", __func__, ret);
		return;
	}

	/* poll until the flush WRs complete */
	do
	{
		ret = ib_poll_cq(cb->cq, 1, &wc);
		if (ret < 0)
		{
			printk(KERN_ERR PFX "ib_poll_cq failed %d\n", ret);
			return;
		}
		if (ret == 0)
			continue;
		ccnt++;
		if (wc.wr_id == 0xdeadbeefcafebabe ||
			wc.wr_id == 0xcafebabedeadbeef)
			flushed++;
	} while (flushed != 2);
	DEBUG_LOG("qp_flushed! ccnt %u\n", ccnt);
}

static void krping_fr_test(struct krping_cb *cb)
{
	struct ib_send_wr inv;
	const struct ib_send_wr *bad;
	struct ib_reg_wr fr;
	struct ib_wc wc;
	u8 key = 0;
	struct ib_mr *mr;
	int ret;
	int size = cb->size;
	int plen = (((size - 1) & PAGE_MASK) + PAGE_SIZE) >> PAGE_SHIFT;
	unsigned long start;
	int count = 0;
	int scnt = 0;
	struct scatterlist sg = {0};

	mr = ib_alloc_mr(cb->pd, IB_MR_TYPE_MEM_REG, plen);
	if (IS_ERR(mr))
	{
		printk(KERN_ERR PFX "ib_alloc_mr failed %ld\n", PTR_ERR(mr));
		return;
	}

	sg_dma_address(&sg) = (dma_addr_t)0xcafebabe0000ULL;
	sg_dma_len(&sg) = size;
	ret = ib_map_mr_sg(mr, &sg, 1, NULL, PAGE_SIZE);
	if (ret <= 0)
	{
		printk(KERN_ERR PFX "ib_map_mr_sge err %d\n", ret);
		goto err2;
	}

	memset(&fr, 0, sizeof fr);
	fr.wr.opcode = IB_WR_REG_MR;
	fr.access = IB_ACCESS_REMOTE_WRITE | IB_ACCESS_LOCAL_WRITE;
	fr.mr = mr;
	fr.wr.next = &inv;

	memset(&inv, 0, sizeof inv);
	inv.opcode = IB_WR_LOCAL_INV;
	inv.send_flags = IB_SEND_SIGNALED;

	DEBUG_LOG("fr_test: stag index 0x%x plen %u size %u depth %u\n", mr->rkey >> 8, plen, cb->size, cb->txdepth);
	start = get_seconds();
	while (!cb->count || count <= cb->count)
	{
		if (signal_pending(current))
		{
			printk(KERN_ERR PFX "signal!\n");
			break;
		}
		if ((get_seconds() - start) >= 9)
		{
			DEBUG_LOG("fr_test: pausing 1 second! count %u latest size %u plen %u\n", count, size, plen);
			wait_event_interruptible_timeout(cb->sem, cb->state == ERROR, HZ);
			if (cb->state == ERROR)
				break;
			start = get_seconds();
		}
		while (scnt < (cb->txdepth >> 1))
		{
			ib_update_fast_reg_key(mr, ++key);
			fr.key = mr->rkey;
			inv.ex.invalidate_rkey = mr->rkey;

			size = prandom_u32() % cb->size;
			if (size == 0)
				size = cb->size;
			sg_dma_len(&sg) = size;
			ret = ib_map_mr_sg(mr, &sg, 1, NULL, PAGE_SIZE);
			if (ret <= 0)
			{
				printk(KERN_ERR PFX "ib_map_mr_sge err %d\n", ret);
				goto err2;
			}
			ret = ib_post_send(cb->qp, &fr.wr, &bad);
			if (ret)
			{
				printk(KERN_ERR PFX "ib_post_send failed %d\n", ret);
				goto err2;
			}
			scnt++;
		}

		ret = ib_poll_cq(cb->cq, 1, &wc);
		if (ret < 0)
		{
			printk(KERN_ERR PFX "ib_poll_cq failed %d\n", ret);
			goto err2;
		}
		if (ret == 1)
		{
			if (wc.status)
			{
				printk(KERN_ERR PFX "completion error %u\n", wc.status);
				goto err2;
			}
			count++;
			scnt--;
		}
	}
err2:
	flush_qp(cb);
	DEBUG_LOG("fr_test: done!\n");
	ib_dereg_mr(mr);
}

static int krping_connect_client(struct krping_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 10;

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret)
	{
		printk(KERN_ERR PFX "rdma_connect error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= CONNECTED);
	if (cb->state == ERROR)
	{
		printk(KERN_ERR PFX "wait for CONNECTED state %d\n", cb->state);
		return -1;
	}

	DEBUG_LOG("rdma_connect successful\n");
	return 0;
}

static int krping_bind_client(struct krping_cb *cb)
{
	struct sockaddr_storage sin;
	int ret;

	fill_sockaddr(&sin, cb);

	ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *)&sin, 2000);
	if (ret)
	{
		printk(KERN_ERR PFX "rdma_resolve_addr error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= ROUTE_RESOLVED);
	if (cb->state != ROUTE_RESOLVED)
	{
		printk(KERN_ERR PFX
			   "addr/route resolution did not resolve: state %d\n",
			   cb->state);
		return -EINTR;
	}

	if (!reg_supported(cb->cm_id->device))
		return -EINVAL;

	DEBUG_LOG("rdma_resolve_addr - rdma_resolve_route successful\n");
	return 0;
}

static void krping_run_client(struct krping_cb *cb)
{
	const struct ib_recv_wr *bad_wr;
	int ret;

	/* set type of service, if any */
	if (cb->tos != 0)
		rdma_set_service_type(cb->cm_id, cb->tos);

	ret = krping_bind_client(cb);
	if (ret)
		return;
	printk("Successfully bind client");

	ret = krping_setup_qp(cb, cb->cm_id);
	if (ret)
	{
		printk(KERN_ERR PFX "setup_qp failed: %d\n", ret);
		return;
	}
	printk("Client successfully setup qp");

	ret = krping_setup_buffers(cb);
	if (ret)
	{
		printk(KERN_ERR PFX "krping_setup_buffers failed: %d\n", ret);
		goto err1;
	}
	printk("Client successfully setup buffer");

	// ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
	// if (ret) {
	// 	printk(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
	// 	goto err2;
	// }
	// printk("Client successfully post recv");

	ret = krping_connect_client(cb);
	if (ret)
	{
		printk(KERN_ERR PFX "connect error %d\n", ret);
		goto err2;
	}
	printk("Client successfully send CM Request");

	if (cb->wlat)
		krping_wlat_test_client(cb);
	else if (cb->rlat)
		krping_rlat_test_client(cb);
	else if (cb->bw)
		krping_bw_test_client(cb);
	else if (cb->frtest)
		krping_fr_test(cb);
	else
	{
		// krping_test_client(cb);   //current only write before read, otherwise write will occur error with status 6
		// krping_test_client_wr(cb);
		// krping_test_client_rd(cb);
		// krping_test_client_wr(cb);


		krping_test_client(cb);
	}

	// printk("ready to disconnect cm");
	rdma_disconnect(cb->cm_id);
	// printk("disconnect over");
err2:
	krping_free_buffers(cb);
	// printk("free buffers");
err1:
	krping_free_qp(cb);
	// printk("free qp over");
}

int krping_doit(char *cmd)
{
	// struct krping_cb *cb;
	int op;
	int ret = 0;
	char *optarg;
	char *scope;
	unsigned long optint;

	cb1 = kzalloc(sizeof(*cb1), GFP_KERNEL);
	if (!cb1)
		return -ENOMEM;

	mutex_lock(&krping_mutex);
	list_add_tail(&cb1->list, &krping_cbs);
	mutex_unlock(&krping_mutex);

	cb1->server = -1;
	cb1->state = IDLE;
	cb1->size = 64;
	cb1->txdepth = RPING_SQ_DEPTH;
	init_waitqueue_head(&cb1->sem);

	while ((op = krping_getopt("krping", &cmd, krping_opts, NULL, &optarg,
							   &optint)) != 0)
	{
		switch (op)
		{
		case 'a':
			cb1->addr_str = optarg;
			in4_pton(optarg, -1, cb1->addr, -1, NULL);
			cb1->addr_type = AF_INET;
			DEBUG_LOG("ipaddr (%s)\n", optarg);
			break;
		case 'A':
			cb1->addr_str = optarg;
			scope = strstr(optarg, "%");
			if (scope != NULL)
			{
				*scope++ = 0;
				strncpy(cb1->ip6_ndev_name, scope,
						sizeof(cb1->ip6_ndev_name));
				/* force zero-termination */
				cb1->ip6_ndev_name[sizeof(cb1->ip6_ndev_name) - 1] = 0;
			}
			in6_pton(optarg, -1, cb1->addr, -1, NULL);
			cb1->addr_type = AF_INET6;
			DEBUG_LOG("ipv6addr (%s)\n", optarg);
			break;
		case 'p':
			cb1->port = htons(optint);
			DEBUG_LOG("port %d\n", (int)optint);
			break;
		case 'P':
			cb1->poll = 1;
			DEBUG_LOG("server\n");
			break;
		case 's':
			cb1->server = 1;
			DEBUG_LOG("server\n");
			break;
		case 'c':
			cb1->server = 0;
			DEBUG_LOG("client\n");
			break;
		case 'S':
			cb1->size = optint;
			if ((cb1->size < 1) ||
				(cb1->size > RPING_BUFSIZE))
			{
				printk(KERN_ERR PFX "Invalid size %d "
									"(valid range is 1 to %d)\n",
					   cb1->size, RPING_BUFSIZE);
				ret = EINVAL;
			}
			else
				DEBUG_LOG("size %d\n", (int)optint);
			break;
		case 'C':
			cb1->count = optint;
			if (cb1->count < 0)
			{
				printk(KERN_ERR PFX "Invalid count %d\n",
					   cb1->count);
				ret = EINVAL;
			}
			else
				DEBUG_LOG("count %d\n", (int)cb1->count);
			break;
		case 'v':
			cb1->verbose++;
			DEBUG_LOG("verbose\n");
			break;
		case 'V':
			cb1->validate++;
			DEBUG_LOG("validate data\n");
			break;
		case 'l':
			cb1->wlat++;
			break;
		case 'L':
			cb1->rlat++;
			break;
		case 'B':
			cb1->bw++;
			break;
		case 'd':
			cb1->duplex++;
			break;
		case 'I':
			cb1->server_invalidate = 1;
			break;
		case 't':
			cb1->tos = optint;
			DEBUG_LOG("type of service, tos=%d\n", (int)cb1->tos);
			break;
		case 'T':
			cb1->txdepth = optint;
			DEBUG_LOG("txdepth %d\n", (int)cb1->txdepth);
			break;
		case 'Z':
			cb1->local_dma_lkey = 1;
			DEBUG_LOG("using local dma lkey\n");
			break;
		case 'R':
			cb1->read_inv = 1;
			DEBUG_LOG("using read-with-inv\n");
			break;
		case 'f':
			cb1->frtest = 1;
			DEBUG_LOG("fast-reg test!\n");
			break;
		default:
			printk(KERN_ERR PFX "unknown opt %s\n", optarg);
			ret = -EINVAL;
			break;
		}
	}
	if (ret)
		goto out;

	if (cb1->server == -1)
	{
		printk(KERN_ERR PFX "must be either client or server\n");
		ret = -EINVAL;
		goto out;
	}

	if (cb1->server && cb1->frtest)
	{
		printk(KERN_ERR PFX "must be client to run frtest\n");
		ret = -EINVAL;
		goto out;
	}

	if ((cb1->frtest + cb1->bw + cb1->rlat + cb1->wlat) > 1)
	{
		printk(KERN_ERR PFX "Pick only one test: fr, bw, rlat, wlat\n");
		ret = -EINVAL;
		goto out;
	}

	if (cb1->wlat || cb1->rlat || cb1->bw)
	{
		printk(KERN_ERR PFX "wlat, rlat, and bw tests only support mem_mode MR - which is no longer supported\n");
		ret = -EINVAL;
		goto out;
	}

	cb1->cm_id = rdma_create_id(&init_net, krping_cma_event_handler, cb1, RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(cb1->cm_id))
	{
		ret = PTR_ERR(cb1->cm_id);
		printk(KERN_ERR PFX "rdma_create_id error %d\n", ret);
		goto out;
	}
	printk("Client created cm_id %p\n", cb1->cm_id);

	if (cb1->server)
		krping_run_server(cb1);
	else
		krping_run_client(cb1);

	// printk("destroy cm_id %p\n", cb1->cm_id);
	rdma_destroy_id(cb1->cm_id);
out:
	mutex_lock(&krping_mutex);
	list_del(&cb1->list);
	mutex_unlock(&krping_mutex);
	kfree(cb1);
	// printk("krping test over, ret= %d", ret);
	// printk("end");
	return ret;
}

/*
 * Read proc returns stats for each device.
 */
static int krping_read_proc(struct seq_file *seq, void *v)
{
	struct krping_cb *cb;
	int num = 1;

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	DEBUG_LOG(KERN_INFO PFX "proc read called...\n");
	mutex_lock(&krping_mutex);
	list_for_each_entry(cb, &krping_cbs, list)
	{
		if (cb->pd)
		{
			seq_printf(seq,
					   "%d-%s %lld %lld %lld %lld %lld %lld %lld %lld\n",
					   num++, cb->pd->device->name, cb->stats.send_bytes,
					   cb->stats.send_msgs, cb->stats.recv_bytes,
					   cb->stats.recv_msgs, cb->stats.write_bytes,
					   cb->stats.write_msgs,
					   cb->stats.read_bytes,
					   cb->stats.read_msgs);
		}
		else
		{
			seq_printf(seq, "%d listen\n", num++);
		}
	}
	mutex_unlock(&krping_mutex);
	module_put(THIS_MODULE);
	return 0;
}

/*
 * Write proc is used to start a ping client or server.
 */
static ssize_t krping_write_proc(struct file *file, const char __user *buffer,
								 size_t count, loff_t *ppos)
{
	char *cmd;
	int rc;

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	cmd = kmalloc(count, GFP_KERNEL);
	if (cmd == NULL)
	{
		printk(KERN_ERR PFX "kmalloc failure\n");
		return -ENOMEM;
	}
	if (copy_from_user(cmd, buffer, count))
	{
		kfree(cmd);
		return -EFAULT;
	}

	/*
	 * remove the \n.
	 */
	cmd[count - 1] = 0;
	DEBUG_LOG(KERN_INFO PFX "proc write |%s|\n", cmd);
	rc = krping_doit(cmd);
	kfree(cmd);
	module_put(THIS_MODULE);
	if (rc)
		return rc;
	else
		return (int)count;
}

static int krping_read_open(struct inode *inode, struct file *file)
{
	return single_open(file, krping_read_proc, inode->i_private);
}

static struct file_operations krping_ops = {
	.owner = THIS_MODULE,
	.open = krping_read_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = krping_write_proc,
};

static int __init krping_init(void)
{
	DEBUG_LOG("krping_init\n");
	krping_proc = proc_create("krping", 0666, NULL, &krping_ops);
	if (krping_proc == NULL)
	{
		printk(KERN_ERR PFX "cannot create /proc/krping\n");
		return -ENOMEM;
	}

	/* Register new block device and get device major number */
	dev_major = register_blkdev(dev_major, "testblk");

	block_device = kmalloc(sizeof(struct block_dev), GFP_KERNEL);

	if (block_device == NULL)
	{
		printk("Failed to allocate struct block_dev\n");
		unregister_blkdev(dev_major, "testblk");

		return -ENOMEM;
	}

	/* Set some random capacity of the device */
	block_device->capacity = (1120000 * PAGE_SIZE) >> 9; /* nsectors * SECTOR_SIZE; */
	/* Allocate corresponding data buffer */
	block_device->data = kmalloc(112 * PAGE_SIZE, GFP_KERNEL); // 此处不可动

	if (block_device->data == NULL)
	{
		printk("Failed to allocate device IO buffer\n");
		unregister_blkdev(dev_major, "testblk");
		kfree(block_device);

		return -ENOMEM;
	}

	printk("Initializing queue\n");

	block_device->queue = blk_mq_init_sq_queue(&block_device->tag_set, &mq_ops, 128, BLK_MQ_F_SHOULD_MERGE);

	if (block_device->queue == NULL)
	{
		printk("Failed to allocate device queue\n");
		kfree(block_device->data);

		unregister_blkdev(dev_major, "testblk");
		kfree(block_device);

		return -ENOMEM;
	}

	/* Set driver's structure as user data of the queue */
	block_device->queue->queuedata = block_device;

	/* Allocate new disk */
	block_device->gdisk = alloc_disk(1);

	/* Set all required flags and data */
	block_device->gdisk->flags = GENHD_FL_NO_PART_SCAN;
	block_device->gdisk->major = dev_major;
	block_device->gdisk->first_minor = 0;

	block_device->gdisk->fops = &blockdev_ops;
	block_device->gdisk->queue = block_device->queue;
	block_device->gdisk->private_data = block_device;

	/* Set device name as it will be represented in /dev */
	strncpy(block_device->gdisk->disk_name, "blockdev\0", 9);

	printk("Adding disk %s\n", block_device->gdisk->disk_name);

	/* Set device capacity */
	set_capacity(block_device->gdisk, block_device->capacity);

	/* Notify kernel about new disk device */
	add_disk(block_device->gdisk);
	return 0;
}

static void __exit krping_exit(void)
{
	DEBUG_LOG("krping_exit\n");
	remove_proc_entry("krping", NULL);

	/* Don't forget to cleanup everything */
	if (block_device->gdisk)
	{
		del_gendisk(block_device->gdisk);
		put_disk(block_device->gdisk);
	}

	if (block_device->queue)
	{
		blk_cleanup_queue(block_device->queue);
	}

	kfree(block_device->data);

	unregister_blkdev(dev_major, "testblk");
	kfree(block_device);
}

module_init(krping_init);
module_exit(krping_exit);

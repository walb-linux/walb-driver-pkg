/**
 * none_req.c - request_fn which do nothing.
 *
 * Copyright(C) 2012, Cybozu Labs, Inc.
 * @author HOSHINO Takashi <hoshino@labs.cybozu.co.jp>
 */
#include "check_kernel.h"
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include "base_req.h"
#include "memblk_data.h"

void simple_blk_req_request_fn(struct request_queue *q)
{
	struct request *req;
	
	while ((req = blk_fetch_request(q)) != NULL) {
		__blk_end_request_all(req, 0);
	}
}

bool pre_register(void)
{
	return true;
}

void pre_unregister(void)
{
}

void post_unregister(void)
{
}

bool create_private_data(struct simple_blk_dev *sdev)
{
	return true;
}
void destroy_private_data(struct simple_blk_dev *sdev)
{
}

void customize_sdev(struct simple_blk_dev *sdev)
{
}

MODULE_LICENSE("Dual BSD/GPL");

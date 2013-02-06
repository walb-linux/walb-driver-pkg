/**
 * walb.c - Block-level WAL module.
 *
 * Copyright(C) 2010, Cybozu Labs, Inc.
 * @author HOSHINO Takashi <hoshino@labs.cybozu.co.jp>
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/hdreg.h>
#include <linux/kdev_t.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/bio.h>
#include <linux/spinlock.h>
#include <linux/rwsem.h>
#include <asm/atomic.h>

#include "kern.h"
#include "hashtbl.h"
#include "snapshot.h"
#include "control.h"
#include "alldevs.h"
#include "util.h"
#include "logpack.h"
#include "checkpoint.h"
#include "super.h"
#include "io.h"
#include "redo.h"

#include "walb/ioctl.h"
#include "walb/log_device.h"
#include "walb/sector.h"
#include "walb/snapshot.h"

/*******************************************************************************
 * Module parameters definition.
 *******************************************************************************/

/**
 * Device major of walb.
 */
int walb_major_ = 0;
module_param_named(walb_major, walb_major_, int, S_IRUGO);

/**
 * Set 1 if you want to sync down superblock in disassemble device.
 * Set 0 if not.
 */
static int is_sync_superblock_ = 1;
module_param_named(is_sync_superblock, is_sync_superblock_, int, S_IRUGO|S_IWUSR);

/**
 * Set Non-zero if you want to sort data IOs
 * before submitting to the data device.
 * The parameter n_io_bulk will work as sort buffer size.
 */
unsigned int is_sort_data_io_ = 1;
module_param_named(is_sort_data_io, is_sort_data_io_, uint, S_IRUGO|S_IWUSR);

/*******************************************************************************
 * Shared data definition.
 *******************************************************************************/

/**
 * Workqueues.
 */
#define WQ_NORMAL_NAME "walb_wq_normal"
struct workqueue_struct *wq_normal_ = NULL;
#define WQ_NRT_NAME "walb_wq_nrt"
struct workqueue_struct *wq_nrt_ = NULL;
#define WQ_UNBOUND_NAME "walb_wq_unbound"
struct workqueue_struct *wq_unbound_ = NULL;
#define WQ_MISC_NAME "wq_misc"
struct workqueue_struct *wq_misc_ = NULL;

/*******************************************************************************
 * Static data definition.
 *******************************************************************************/

/**
 * For (walb_dev *)->freeze_state.
 *
 * FRZ_MELTED -> FRZ_FREEZED
 * FRZ_MELTED -> FRZ_FREEZED_WITH_TIMEOUT
 * FRZ_FREEZED -> FRZ_FREEZED_WITH_TIMEOUT
 * FRZ_FREEZED -> FRZ_MELTED
 * FRZ_FREEZED_WITH_TIMEOUT -> FRZ_MELTED
 */
enum {
	FRZ_MELTED = 0,
	FRZ_FREEZED,
	FRZ_FREEZED_WITH_TIMEOUT,
};

/*******************************************************************************
 * Macro definition.
 *******************************************************************************/

/* (struct gendisk *) --> (struct walb_dev *) */
#define get_wdev_from_gd(gd) ((struct walb_dev *)(gd)->private_data)

/*******************************************************************************
 * Prototypes of local functions.
 *******************************************************************************/

/* Lock/unlock block device. */
static int walb_lock_bdev(struct block_device **bdevp, dev_t dev);
static void walb_unlock_bdev(struct block_device *bdev);

/* Logpack check function. */
static int walb_check_lsid_valid(struct walb_dev *wdev, u64 lsid);

/* Walb device open/close/ioctl. */
static int walb_open(struct block_device *bdev, fmode_t mode);
static int walb_release(struct gendisk *gd, fmode_t mode);
static int walb_dispatch_ioctl_wdev(struct walb_dev *wdev, void __user *userctl);
static int walb_ioctl(struct block_device *bdev, fmode_t mode,
		unsigned int cmd, unsigned long arg);

/* Ioctl details. */
static int ioctl_wdev_get_oldest_lsid(struct walb_dev *wdev, struct walb_ctl *ctl);
static int ioctl_wdev_set_oldest_lsid(struct walb_dev *wdev, struct walb_ctl *ctl);
static int ioctl_wdev_search_lsid(struct walb_dev *wdev, struct walb_ctl *ctl); /* NYI */
static int ioctl_wdev_status(struct walb_dev *wdev, struct walb_ctl *ctl); /* NYI */
static int ioctl_wdev_create_snapshot(struct walb_dev *wdev, struct walb_ctl *ctl);
static int ioctl_wdev_delete_snapshot(struct walb_dev *wdev, struct walb_ctl *ctl);
static int ioctl_wdev_delete_snapshot_range(struct walb_dev *wdev, struct walb_ctl *ctl);
static int ioctl_wdev_get_snapshot(struct walb_dev *wdev, struct walb_ctl *ctl);
static int ioctl_wdev_num_of_snapshot_range(struct walb_dev *wdev, struct walb_ctl *ctl);
static int ioctl_wdev_list_snapshot_range(struct walb_dev *wdev, struct walb_ctl *ctl);
static int ioctl_wdev_list_snapshot_from(struct walb_dev *wdev, struct walb_ctl *ctl);
static int ioctl_wdev_take_checkpoint(struct walb_dev *wdev, struct walb_ctl *ctl);
static int ioctl_wdev_get_checkpoint_interval(struct walb_dev *wdev, struct walb_ctl *ctl);
static int ioctl_wdev_set_checkpoint_interval(struct walb_dev *wdev, struct walb_ctl *ctl);
static int ioctl_wdev_get_written_lsid(struct walb_dev *wdev, struct walb_ctl *ctl);
static int ioctl_wdev_get_permanent_lsid(struct walb_dev *wdev, struct walb_ctl *ctl);
static int ioctl_wdev_get_completed_lsid(struct walb_dev *wdev, struct walb_ctl *ctl);
static int ioctl_wdev_get_log_usage(struct walb_dev *wdev, struct walb_ctl *ctl);
static int ioctl_wdev_get_log_capacity(struct walb_dev *wdev, struct walb_ctl *ctl);
static int ioctl_wdev_resize(struct walb_dev *wdev, struct walb_ctl *ctl);
static int ioctl_wdev_clear_log(struct walb_dev *wdev, struct walb_ctl *ctl);
static int ioctl_wdev_is_log_overflow(struct walb_dev *wdev, struct walb_ctl *ctl);
static int ioctl_wdev_freeze(struct walb_dev *wdev, struct walb_ctl *ctl);
static int ioctl_wdev_is_frozen(struct walb_dev *wdev, struct walb_ctl *ctl);
static int ioctl_wdev_melt(struct walb_dev *wdev, struct walb_ctl *ctl);

/* Walblog device open/close/ioctl. */
static int walblog_open(struct block_device *bdev, fmode_t mode);
static int walblog_release(struct gendisk *gd, fmode_t mode);
static int walblog_ioctl(struct block_device *bdev, fmode_t mode,
			unsigned int cmd, unsigned long arg);

/* Utility functions for walb_dev. */
static u64 get_written_lsid(struct walb_dev *wdev);
static u64 get_permanent_lsid(struct walb_dev *wdev);
static u64 get_completed_lsid(struct walb_dev *wdev);
static u64 get_log_usage(struct walb_dev *wdev);
static u64 get_log_capacity(struct walb_dev *wdev);
static int walb_set_name(struct walb_dev *wdev, unsigned int minor,
			const char *name);
static void walb_decide_flush_support(struct walb_dev *wdev);
static void walb_discard_support(struct walb_dev *wdev);
static bool resize_disk(struct gendisk *gd, u64 new_size);
static bool invalidate_lsid(struct walb_dev *wdev, u64 lsid);
static void backup_lsid_set(struct walb_dev *wdev, struct lsid_set *lsids);
static void restore_lsid_set(struct walb_dev *wdev, const struct lsid_set *lsids);
static void task_melt(struct work_struct *work);
static void cancel_melt_work(struct walb_dev *wdev);
static bool freeze_if_melted(struct walb_dev *wdev, u32 timeout_sec);
static bool melt_if_frozen(struct walb_dev *wdev, bool restarts_checkpointing);

/* Workqueues. */
static bool initialize_workqueues(void);
static void finalize_workqueues(void);

/* Prepare/finalize. */
static int walb_prepare_device(
	struct walb_dev *wdev, unsigned int minor, const char *name);
static void walb_finalize_device(struct walb_dev *wdev);
static int walblog_prepare_device(struct walb_dev *wdev, unsigned int minor,
				const char* name);
static void walblog_finalize_device(struct walb_dev *wdev);

static int walb_ldev_initialize(struct walb_dev *wdev);
static void walb_ldev_finalize(struct walb_dev *wdev);

/* Register/unregister. */
static void walb_register_device(struct walb_dev *wdev);
static void walb_unregister_device(struct walb_dev *wdev);
static void walblog_register_device(struct walb_dev *wdev);
static void walblog_unregister_device(struct walb_dev *wdev);

/* Module init/exit. */
static int __init walb_init(void);
static void walb_exit(void);

/*******************************************************************************
 * Static functions.
 *******************************************************************************/

/**
 * Open and claim underlying block device.
 * @bdevp  pointer to bdev pointer to back.
 * @dev	   device to lock.
 * @return 0 in success.
 */
static int walb_lock_bdev(struct block_device **bdevp, dev_t dev)
{
	int err = 0;
	struct block_device *bdev;
	char b[BDEVNAME_SIZE];

	/* Currently the holder is the pointer to walb_lock_bdev(). */
	bdev = blkdev_get_by_dev(dev, FMODE_READ|FMODE_WRITE|FMODE_EXCL, walb_lock_bdev);
	if (IS_ERR(bdev)) { err = PTR_ERR(bdev); goto open_err; }

	*bdevp = bdev;
	return err;

open_err:
	LOGe("open error %s.\n", __bdevname(dev, b));
	return err;
}

/**
 * Release underlying block device.
 * @bdev bdev pointer.
 */
static void walb_unlock_bdev(struct block_device *bdev)
{
	blkdev_put(bdev, FMODE_READ|FMODE_WRITE|FMODE_EXCL);
}

/**
 * Check logpack of the given lsid exists.
 *
 * @lsid lsid to check.
 *
 * @return Non-zero if valid, or 0.
 */
static int walb_check_lsid_valid(struct walb_dev *wdev, u64 lsid)
{
	struct sector_data *sect;
	struct walb_logpack_header *logh;
	u64 off;

	ASSERT(wdev);

	sect = sector_alloc(wdev->physical_bs, GFP_NOIO);
	if (!sect) {
		LOGe("walb_check_lsid_valid: alloc sector failed.\n");
		goto error0;
	}
	ASSERT(is_same_size_sector(sect, wdev->lsuper0));
	logh = get_logpack_header(sect);

	off = get_offset_of_lsid_2(get_super_sector(wdev->lsuper0), lsid);
	if (!sector_io(READ, wdev->ldev, off, sect)) {
		LOGe("walb_check_lsid_valid: read sector failed.\n");
		goto error1;
	}

	/* Check valid logpack header. */
	if (!is_valid_logpack_header_with_checksum(
			logh, wdev->physical_bs, wdev->log_checksum_salt)) {
		goto error1;
	}

	/* Check lsid. */
	if (logh->logpack_lsid != lsid) {
		goto error1;
	}

	sector_free(sect);
	return 1;

error1:
	sector_free(sect);
error0:
	return 0;
}

/**
 * Open walb device.
 */
static int walb_open(struct block_device *bdev, fmode_t mode)
{
	struct walb_dev *wdev = get_wdev_from_gd(bdev->bd_disk);
	int n_users;

	n_users = atomic_inc_return(&wdev->n_users);
	if (n_users == 1) {
#if 0
		LOGn("This is the first time to open walb device %d"
			" and check_disk_change() will be called.\n",
			MINOR(wdev->devt));
		check_disk_change(bdev);
#endif
	}
	return 0;
}

/**
 * Release a walb device.
 */
static int walb_release(struct gendisk *gd, fmode_t mode)
{
	struct walb_dev *wdev = get_wdev_from_gd(gd);
	int n_users;

	n_users = atomic_dec_return(&wdev->n_users);
	ASSERT(n_users >= 0);
	return 0;
}

/**
 * Execute ioctl for WALB_IOCTL_WDEV.
 *
 * return 0 in success, or -EFAULT.
 */
static int walb_dispatch_ioctl_wdev(struct walb_dev *wdev, void __user *userctl)
{
	int ret = -EFAULT;
	struct walb_ctl *ctl;

	/* Get ctl data. */
	ctl = walb_get_ctl(userctl, GFP_KERNEL);
	if (!ctl) {
		LOGe("walb_get_ctl failed.\n");
		return -EFAULT;
	}

	/* Execute each command. */
	switch(ctl->command) {
	case WALB_IOCTL_GET_OLDEST_LSID:
		ret = ioctl_wdev_get_oldest_lsid(wdev, ctl);
		break;
	case WALB_IOCTL_SET_OLDEST_LSID:
		ret = ioctl_wdev_set_oldest_lsid(wdev, ctl);
		break;
	case WALB_IOCTL_TAKE_CHECKPOINT:
		ret = ioctl_wdev_take_checkpoint(wdev, ctl);
		break;
	case WALB_IOCTL_GET_CHECKPOINT_INTERVAL:
		ret = ioctl_wdev_get_checkpoint_interval(wdev, ctl);
		break;
	case WALB_IOCTL_SET_CHECKPOINT_INTERVAL:
		ret = ioctl_wdev_set_checkpoint_interval(wdev, ctl);
		break;
	case WALB_IOCTL_GET_WRITTEN_LSID:
		ret = ioctl_wdev_get_written_lsid(wdev, ctl);
		break;
	case WALB_IOCTL_GET_PERMANENT_LSID:
		ret = ioctl_wdev_get_permanent_lsid(wdev, ctl);
		break;
	case WALB_IOCTL_GET_COMPLETED_LSID:
		ret = ioctl_wdev_get_completed_lsid(wdev, ctl);
		break;
	case WALB_IOCTL_GET_LOG_USAGE:
		ret = ioctl_wdev_get_log_usage(wdev, ctl);
		break;
	case WALB_IOCTL_GET_LOG_CAPACITY:
		ret = ioctl_wdev_get_log_capacity(wdev, ctl);
		break;
	case WALB_IOCTL_CREATE_SNAPSHOT:
		ret = ioctl_wdev_create_snapshot(wdev, ctl);
		break;
	case WALB_IOCTL_DELETE_SNAPSHOT:
		ret = ioctl_wdev_delete_snapshot(wdev, ctl);
		break;
	case WALB_IOCTL_DELETE_SNAPSHOT_RANGE:
		ret = ioctl_wdev_delete_snapshot_range(wdev, ctl);
		break;
	case WALB_IOCTL_GET_SNAPSHOT:
		ret = ioctl_wdev_get_snapshot(wdev, ctl);
		break;
	case WALB_IOCTL_NUM_OF_SNAPSHOT_RANGE:
		ret = ioctl_wdev_num_of_snapshot_range(wdev, ctl);
		break;
	case WALB_IOCTL_LIST_SNAPSHOT_RANGE:
		ret = ioctl_wdev_list_snapshot_range(wdev, ctl);
		break;
	case WALB_IOCTL_LIST_SNAPSHOT_FROM:
		ret = ioctl_wdev_list_snapshot_from(wdev, ctl);
		break;
	case WALB_IOCTL_SEARCH_LSID:
		ret = ioctl_wdev_search_lsid(wdev, ctl);
		break;
	case WALB_IOCTL_STATUS:
		ret = ioctl_wdev_status(wdev, ctl);
		break;
	case WALB_IOCTL_RESIZE:
		ret = ioctl_wdev_resize(wdev, ctl);
		break;
	case WALB_IOCTL_CLEAR_LOG:
		ret = ioctl_wdev_clear_log(wdev, ctl);
		break;
	case WALB_IOCTL_IS_LOG_OVERFLOW:
		ret = ioctl_wdev_is_log_overflow(wdev, ctl);
		break;
	case WALB_IOCTL_FREEZE:
		ret = ioctl_wdev_freeze(wdev, ctl);
		break;
	case WALB_IOCTL_MELT:
		ret = ioctl_wdev_melt(wdev, ctl);
		break;
	case WALB_IOCTL_IS_FROZEN:
		ret = ioctl_wdev_is_frozen(wdev, ctl);
		break;
	default:
		LOGn("WALB_IOCTL_WDEV %d is not supported.\n",
			ctl->command);
	}

	/* Put ctl data. */
	if (walb_put_ctl(userctl, ctl) != 0) {
		LOGe("walb_put_ctl failed.\n");
		return -EFAULT;
	}
	return ret;
}

/*
 * The ioctl() implementation
 */
static int walb_ioctl(struct block_device *bdev, fmode_t mode,
		unsigned int cmd, unsigned long arg)
{
	long size;
	struct hd_geometry geo;
	struct walb_dev *wdev = bdev->bd_disk->private_data;
	int ret = -ENOTTY;
	u32 version;

	LOGd("walb_ioctl begin.\n");
	LOGd("cmd: %08x\n", cmd);

	switch(cmd) {
	case HDIO_GETGEO:
		/*
		 * Get geometry: since we are a virtual device, we have to make
		 * up something plausible.  So we claim 16 sectors, four heads,
		 * and calculate the corresponding number of cylinders.	 We set the
		 * start of data at sector four.
		 */
		size = wdev->ddev_size;
		geo.cylinders = (size & ~0x3f) >> 6;
		geo.heads = 4;
		geo.sectors = 16;
		geo.start = 4;
		if (copy_to_user((void __user *) arg, &geo, sizeof(geo)))
			return -EFAULT;
		ret = 0;
		break;

	case WALB_IOCTL_VERSION:

		version = WALB_VERSION;
		ret = __put_user(version, (int __user *)arg);
		break;

	case WALB_IOCTL_WDEV:

		ret = walb_dispatch_ioctl_wdev(wdev, (void __user *)arg);
		break;
	}

	LOGd("walb_ioctl end.\n");

	return ret;
}

/**
 * Get oldest_lsid.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_get_oldest_lsid(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	u64 oldest_lsid;

	LOGn("WALB_IOCTL_GET_OLDEST_LSID\n");
	ASSERT(ctl->command == WALB_IOCTL_GET_OLDEST_LSID);

	spin_lock(&wdev->lsid_lock);
	oldest_lsid = wdev->lsids.oldest;
	spin_unlock(&wdev->lsid_lock);

	ctl->val_u64 = oldest_lsid;
	return 0;
}

/**
 * Set oldest_lsid.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_set_oldest_lsid(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	u64 lsid, oldest_lsid, written_lsid;

	LOGn("WALB_IOCTL_SET_OLDEST_LSID_SET\n");

	lsid = ctl->val_u64;

	spin_lock(&wdev->lsid_lock);
	written_lsid = wdev->lsids.written;
	oldest_lsid = wdev->lsids.oldest;
	spin_unlock(&wdev->lsid_lock);

	if (!(lsid == written_lsid ||
			(oldest_lsid <= lsid && lsid < written_lsid &&
				walb_check_lsid_valid(wdev, lsid)))) {
		LOGe("lsid %"PRIu64" is not valid.\n", lsid);
		LOGe("You shoud specify valid logpack header lsid"
			" (oldest_lsid (%"PRIu64") <= lsid <= written_lsid (%"PRIu64").\n",
			oldest_lsid, written_lsid);
		return -EFAULT;
	}

	spin_lock(&wdev->lsid_lock);
	wdev->lsids.oldest = lsid;
	spin_unlock(&wdev->lsid_lock);

	if (!walb_sync_super_block(wdev)) {
		LOGe("sync super block failed.\n");
		return -EFAULT;
	}
	return 0;
}

/**
 * Search lsid.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_search_lsid(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	/* not yet implemented */
	LOGn("WALB_IOCTL_SEARCH_LSID is not supported currently.\n");
	return -EFAULT;
}

/**
 * Get status.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_status(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	/* not yet implemented */

	LOGn("WALB_IOCTL_STATUS is not supported currently.\n");
	return -EFAULT;
}

/**
 * Create a snapshot.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_create_snapshot(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	int error;
	struct walb_snapshot_record *srec;

	LOGn("WALB_IOCTL_CREATE_SNAPSHOT\n");
	ASSERT(ctl->command == WALB_IOCTL_CREATE_SNAPSHOT);

	if (sizeof(struct walb_snapshot_record) > ctl->u2k.buf_size) {
		LOGe("Buffer is too small for walb_snapshot_record.\n");
		return -EFAULT;
	}
	srec = (struct walb_snapshot_record *)ctl->u2k.__buf;
	if (!srec) {
		LOGe("Buffer must be walb_snapshot_record data.\n");
		return -EFAULT;
	}
	if (srec->lsid == INVALID_LSID) {
		srec->lsid = get_completed_lsid(wdev);
		ASSERT(srec->lsid != INVALID_LSID);
	}

	if (!is_valid_snapshot_name(srec->name)) {
		LOGe("Snapshot name is invalid.\n");
		return -EFAULT;
	}
	LOGn("Create snapshot name %s lsid %"PRIu64" ts %"PRIu64"\n",
		srec->name, srec->lsid, srec->timestamp);
	error = snapshot_add(wdev->snapd, srec->name, srec->lsid, srec->timestamp);
	if (error) {
		ctl->error = error;
		return -EFAULT;
	}
	return 0;
}

/**
 * Delete a snapshot.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_delete_snapshot(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	struct walb_snapshot_record *srec;
	int error;

	LOGn("WALB_IOCTL_DELETE_SNAPSHOT\n");
	ASSERT(ctl->command == WALB_IOCTL_DELETE_SNAPSHOT);

	if (sizeof(struct walb_snapshot_record) > ctl->u2k.buf_size) {
		LOGe("Buffer is too small for walb_snapshot_record.\n");
		return -EFAULT;
	}
	srec = (struct walb_snapshot_record *)ctl->u2k.__buf;
	if (!srec) {
		LOGe("Buffer must be walb_snapshot_record data.\n");
		return -EFAULT;
	}
	if (!is_valid_snapshot_name(srec->name)) {
		LOGe("Invalid snapshot name.\n");
		return -EFAULT;
	}

	error = snapshot_del(wdev->snapd, srec->name);
	if (error) {
		ctl->error = error;
		return -EFAULT;
	}
	return 0;
}

/**
 * Delete snapshots over a lsid range.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_delete_snapshot_range(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	u64 lsid0, lsid1;
	int ret;

	LOGn("WALB_IOCTL_DELETE_SNAPSHOT_RANGE");
	ASSERT(ctl->command == WALB_IOCTL_DELETE_SNAPSHOT_RANGE);

	if (sizeof(u64) * 2 > ctl->u2k.buf_size) {
		LOGe("Buffer is too small for u64 * 2.\n");
		return -EFAULT;
	}
	lsid0 = ((u64 *)ctl->u2k.__buf)[0];
	lsid1 = ((u64 *)ctl->u2k.__buf)[1];
	if (!is_lsid_range_valid(lsid0, lsid1)) {
		LOGe("Specify valid lsid range.\n");
		return -EFAULT;
	}
	ret = snapshot_del_range(wdev->snapd, lsid0, lsid1);
	if (ret >= 0) {
		ctl->val_int = ret;
	} else {
		ctl->error = ret;
		return -EFAULT;
	}
	return 0;
}

/**
 * Get a snapshot.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_get_snapshot(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	int ret;
	struct walb_snapshot_record *srec0, *srec1, *srec;

	LOGn("WALB_IOCTL_GET_SNAPSHOT\n");
	ASSERT(ctl->command == WALB_IOCTL_GET_SNAPSHOT);

	if (sizeof(struct walb_snapshot_record) > ctl->u2k.buf_size) {
		LOGe("buffer size too small.\n");
		return -EFAULT;
	}
	if (sizeof(struct walb_snapshot_record) > ctl->k2u.buf_size) {
		LOGe("buffer size too small.\n");
		return -EFAULT;
	}
	srec0 = (struct walb_snapshot_record *)ctl->u2k.__buf;
	srec1 = (struct walb_snapshot_record *)ctl->k2u.__buf;
	ASSERT(srec0);
	ASSERT(srec1);
	ret = snapshot_get(wdev->snapd, srec0->name, &srec);
	if (ret) {
		ASSERT(srec);
		memcpy(srec1, srec, sizeof(struct walb_snapshot_record));
	} else {
		snapshot_record_init(srec1);
		ctl->error = ret;
		return -EFAULT;
	}
	return 0;
}

/**
 * Get number of snapshots over a lsid range.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_num_of_snapshot_range(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	u64 lsid0, lsid1;
	int ret;

	LOGn("WALB_IOCTL_NUM_OF_SNAPSHOT_RANGE\n");
	ASSERT(ctl->command == WALB_IOCTL_NUM_OF_SNAPSHOT_RANGE);

	if (sizeof(u64) * 2 > ctl->u2k.buf_size) {
		LOGe("Buffer is too small for u64 * 2.\n");
		return -EFAULT;
	}
	lsid0 = ((u64 *)ctl->u2k.__buf)[0];
	lsid1 = ((u64 *)ctl->u2k.__buf)[1];
	if (!is_lsid_range_valid(lsid0, lsid1)) {
		LOGe("Specify valid lsid range.\n");
		return -EFAULT;
	}

	ret = snapshot_n_records_range(
		wdev->snapd, lsid0, lsid1);
	if (ret < 0) {
		ctl->error = ret;
		return -EFAULT;
	}
	ctl->val_int = ret;
	return 0;
}

/**
 * List snapshots over a lsid range.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_list_snapshot_range(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	u64 lsid0, lsid1;
	struct walb_snapshot_record *srec;
	size_t size;
	int n_rec, ret;

	LOGn("WALB_IOCTL_LIST_SNAPSHOT_RANGE\n");
	ASSERT(ctl->command == WALB_IOCTL_LIST_SNAPSHOT_RANGE);

	if (sizeof(u64) * 2 > ctl->u2k.buf_size) {
		LOGe("Buffer is too small for u64 * 2.\n");
		return -EFAULT;
	}
	lsid0 = ((u64 *)ctl->u2k.__buf)[0];
	lsid1 = ((u64 *)ctl->u2k.__buf)[1];
	if (!is_lsid_range_valid(lsid0, lsid1)) {
		LOGe("Specify valid lsid range.\n");
		return -EFAULT;
	}
	srec = (struct walb_snapshot_record *)ctl->k2u.__buf;
	size = ctl->k2u.buf_size / sizeof(struct walb_snapshot_record);
	if (size == 0) {
		LOGe("Buffer is to small for results.\n");
		return -EFAULT;
	}
	ret = snapshot_list_range(wdev->snapd, srec, size,
				lsid0, lsid1);
	if (ret < 0) {
		ctl->error = ret;
		return -EFAULT;
	}
	n_rec = ret;
	ctl->val_int = n_rec;
	if (n_rec > 0) {
		ASSERT(srec[n_rec - 1].lsid != INVALID_LSID);
		ctl->val_u64 = srec[n_rec - 1].lsid + 1;
	} else {
		ctl->val_u64 = INVALID_LSID;
	}
	return 0;
}

/**
 * List snapshots from a snapshot_id.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_list_snapshot_from(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	struct walb_snapshot_record *srec;
	size_t size;
	int n_rec, ret;
	u32 sid, next_sid;

	LOGn("WALB_IOCTL_LIST_SNAPSHOT_FROM\n");
	ASSERT(ctl->command == WALB_IOCTL_LIST_SNAPSHOT_FROM);

	sid = ctl->val_u32;
	srec = (struct walb_snapshot_record *)ctl->k2u.__buf;
	size = ctl->k2u.buf_size / sizeof(struct walb_snapshot_record);
	if (size == 0) {
		LOGe("Buffer is to small for results.\n");
		return -EFAULT;
	}
	ret = snapshot_list_from(wdev->snapd, srec, size, sid);
	if (ret < 0) {
		ctl->error = ret;
		return -EFAULT;
	}
	n_rec = ret;
	ctl->val_int = n_rec;
	if (n_rec > 0) {
		ASSERT(srec[n_rec - 1].snapshot_id != INVALID_SNAPSHOT_ID);
		next_sid = srec[n_rec - 1].snapshot_id + 1;
	} else {
		next_sid = INVALID_SNAPSHOT_ID;
	}
	ctl->val_u32 = next_sid;
	return 0;
}

/**
 * Take a snapshot immedicately.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_take_checkpoint(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	bool ret;

	LOGn("WALB_IOCTL_TAKE_CHECKPOINT\n");
	ASSERT(ctl->command == WALB_IOCTL_TAKE_CHECKPOINT);

	stop_checkpointing(&wdev->cpd);
#ifdef WALB_DEBUG
	down_write(&wdev->cpd.lock);
	ASSERT(wdev->cpd.state == CP_STOPPED);
	up_write(&wdev->cpd.lock);
#endif
	ret = take_checkpoint(&wdev->cpd);
	if (!ret) {
		atomic_set(&wdev->is_read_only, 1);
		LOGe("superblock sync failed.\n");
		return -EFAULT;
	}
	start_checkpointing(&wdev->cpd);
	return 0;
}

/**
 * Get checkpoint interval.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_get_checkpoint_interval(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	LOGn("WALB_IOCTL_GET_CHECKPOINT_INTERVAL\n");
	ASSERT(ctl->command == WALB_IOCTL_GET_CHECKPOINT_INTERVAL);

	ctl->val_u32 = get_checkpoint_interval(&wdev->cpd);
	return 0;
}

/**
 * Set checkpoint interval.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_set_checkpoint_interval(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	u32 interval;

	LOGn("WALB_IOCTL_SET_CHECKPOINT_INTERVAL\n");
	ASSERT(ctl->command == WALB_IOCTL_SET_CHECKPOINT_INTERVAL);

	interval = ctl->val_u32;
	if (interval > WALB_MAX_CHECKPOINT_INTERVAL) {
		LOGe("Checkpoint interval is too big.\n");
		return -EFAULT;
	}
	set_checkpoint_interval(&wdev->cpd, interval);
	return 0;
}

/**
 * Get written_lsid.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_get_written_lsid(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	LOGn("WALB_IOCTL_GET_WRITTEN_LSID\n");
	ASSERT(ctl->command == WALB_IOCTL_GET_WRITTEN_LSID);

	ctl->val_u64 = get_written_lsid(wdev);
	return 0;
}

/**
 * Get permanent_lsid.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_get_permanent_lsid(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	LOGn("WALB_IOCTL_GET_PERMANENT_LSID\n");
	ASSERT(ctl->command == WALB_IOCTL_GET_PERMANENT_LSID);

	ctl->val_u64 = get_permanent_lsid(wdev);
	return 0;
}

/**
 * Get completed_lsid.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_get_completed_lsid(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	LOGn("WALB_IOCTL_GET_COMPLETED_LSID\n");
	ASSERT(ctl->command == WALB_IOCTL_GET_COMPLETED_LSID);

	ctl->val_u64 = get_completed_lsid(wdev);
	return 0;
}

/**
 * Get log usage.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_get_log_usage(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	LOGn("WALB_IOCTL_GET_LOG_USAGE\n");
	ASSERT(ctl->command == WALB_IOCTL_GET_LOG_USAGE);

	ctl->val_u64 = get_log_usage(wdev);
	return 0;
}

/**
 * Get log capacity.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_get_log_capacity(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	LOGn("WALB_IOCTL_GET_LOG_CAPACITY\n");
	ASSERT(ctl->command == WALB_IOCTL_GET_LOG_CAPACITY);

	ctl->val_u64 = get_log_capacity(wdev);
	return 0;
}

/**
 * Resize walb device.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_resize(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	u64 ddev_size;
	u64 new_size;
	u64 old_size;

	LOGn("WALB_IOCTL_RESIZE.\n");
	ASSERT(ctl->command == WALB_IOCTL_RESIZE);

	old_size = get_capacity(wdev->gd);
	new_size = ctl->val_u64;
	ddev_size = wdev->ddev->bd_part->nr_sects;

	if (new_size == 0) {
		new_size = ddev_size;
	}
	if (new_size < old_size) {
		LOGe("Shrink size from %"PRIu64" to %"PRIu64" is not supported.\n",
			old_size, new_size);
		return -EFAULT;
	}
	if (new_size > ddev_size) {
		LOGe("new_size %"PRIu64" > data device capacity %"PRIu64".\n",
			new_size, ddev_size);
		return -EFAULT;
	}
	if (new_size == old_size) {
		LOGn("No need to resize.\n");
		return 0;
	}

	spin_lock(&wdev->size_lock);
	wdev->size = new_size;
	wdev->ddev_size = ddev_size;
	spin_unlock(&wdev->size_lock);

	if (!resize_disk(wdev->gd, new_size)) {
		return -EFAULT;
	}

	/* Sync super block for super->device_size */
	if (!walb_sync_super_block(wdev)) {
		LOGe("superblock sync failed.\n");
		return -EFAULT;
	}
	return 0;
}

/**
 * Clear log and detect resize of log device.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_clear_log(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	u64 new_ldev_size, old_ldev_size;
	u8 new_uuid[UUID_SIZE], old_uuid[UUID_SIZE];
	unsigned int pbs = wdev->physical_bs;
	bool is_grown = false;
	int ret;
	struct walb_super_sector *super;
	u64 lsid0_off;
	struct lsid_set lsids;
	u64 old_ring_buffer_size;
	u32 new_salt;

	ASSERT(ctl->command == WALB_IOCTL_CLEAR_LOG);
	LOGn("WALB_IOCTL_CLEAR_LOG.\n");

	/* Freeze iocore and checkpointing.  */
	iocore_freeze(wdev);
	stop_checkpointing(&wdev->cpd);

	/* Get old/new log device size. */
	old_ldev_size = wdev->ldev_size;
	new_ldev_size = wdev->ldev->bd_part->nr_sects;

	if (old_ldev_size > new_ldev_size) {
		LOGe("Log device shrink not supported.\n");
		goto error0;
	}

	/* Backup variables. */
	old_ring_buffer_size = wdev->ring_buffer_size;
	backup_lsid_set(wdev, &lsids);

	/* Initialize lsid(s). */
	spin_lock(&wdev->lsid_lock);
	wdev->lsids.latest = 0;
	wdev->lsids.flush = 0;
#ifdef WALB_FAST_ALGORITHM
	wdev->lsids.completed = 0;
#endif
	wdev->lsids.permanent = 0;
	wdev->lsids.written = 0;
	wdev->lsids.prev_written = 0;
	wdev->lsids.oldest = 0;
	spin_unlock(&wdev->lsid_lock);

	/* Grow the walblog device. */
	if (old_ldev_size < new_ldev_size) {
		LOGn("Detect log device size change.\n");

		/* Grow the disk. */
		is_grown = true;
		if (!resize_disk(wdev->log_gd, new_ldev_size)) {
			LOGe("grow disk failed.\n");
			iocore_set_readonly(wdev);
			goto error1;
		}
		LOGn("Grown log device size from %"PRIu64" to %"PRIu64".\n",
			old_ldev_size, new_ldev_size);
		wdev->ldev_size = new_ldev_size;

		/* Currently you can not change n_snapshots. */

		/* Recalculate ring buffer size. */
		wdev->ring_buffer_size =
			addr_pb(pbs, new_ldev_size)
			- get_ring_buffer_offset(pbs, wdev->n_snapshots);
	}

	/* Generate new uuid and salt. */
	get_random_bytes(new_uuid, 16);
	get_random_bytes(&new_salt, sizeof(new_salt));
	wdev->log_checksum_salt = new_salt;

	/* Update superblock image. */
	spin_lock(&wdev->lsuper0_lock);
	super = get_super_sector(wdev->lsuper0);
	memcpy(old_uuid, super->uuid, UUID_SIZE);
	memcpy(super->uuid, new_uuid, UUID_SIZE);
	super->ring_buffer_size = wdev->ring_buffer_size;
	super->log_checksum_salt = new_salt;
	/* super->snapshot_metadata_size; */
	lsid0_off = get_offset_of_lsid_2(super, 0);
	spin_unlock(&wdev->lsuper0_lock);

	/* Sync super sector. */
	if (!walb_sync_super_block(wdev)) {
		LOGe("sync superblock failed.\n");
		iocore_set_readonly(wdev);
		goto error2;
	}

	/* Update uuid index of alldev data. */
	alldevs_write_lock();
	ret = alldevs_update_uuid(old_uuid, new_uuid);
	alldevs_write_unlock();
	if (ret) {
		LOGe("Update alldevs index failed.\n");
		iocore_set_readonly(wdev);
		goto error2;
	}

	/* Invalidate first logpack */
	if (!invalidate_lsid(wdev, 0)) {
		LOGe("invalidate lsid 0 failed.\n");
		iocore_set_readonly(wdev);
		goto error2;
	}

	/* Delete all snapshots. */
	if (snapshot_del_range(wdev->snapd, 0, MAX_LSID + 1) < 0) {
		LOGe("Delete all snapshots failed.\n");
		iocore_set_readonly(wdev);
		goto error2;

	}
	ASSERT(snapshot_n_records(wdev->snapd) == 0);
	LOGn("Delete all snapshots done.\n");

	/* Clear log overflow. */
	iocore_clear_log_overflow(wdev);

	/* Melt iocore and checkpointing. */
	start_checkpointing(&wdev->cpd);
	iocore_melt(wdev);

	return 0;

error2:
	restore_lsid_set(wdev, &lsids);
	wdev->ring_buffer_size = old_ring_buffer_size;
#if 0
	wdev->ldev_size = old_ldev_size;
	if (!resize_disk(wdev->log_gd, old_ldev_size)) {
		LOGe("resize_disk to shrink failed.\n");
	}
#endif
error1:
	start_checkpointing(&wdev->cpd);
	iocore_melt(wdev);
error0:
	return -EFAULT;
}

/**
 * Check log space overflow.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_is_log_overflow(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	ASSERT(ctl->command == WALB_IOCTL_IS_LOG_OVERFLOW);
	LOGn("WALB_IOCTL_IS_LOG_OVERFLOW.\n");

	ctl->val_int = iocore_is_log_overflow(wdev);
	return 0;
}

/**
 * Freeze a walb device.
 * Currently write IOs will be frozen but read IOs will not.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_freeze(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	u32 timeout_sec;

	ASSERT(ctl->command == WALB_IOCTL_FREEZE);
	LOGn("WALB_IOCTL_FREEZE\n");

	/* Clip timeout value. */
	timeout_sec = ctl->val_u32;
	if (timeout_sec > 86400) {
		timeout_sec = 86400;
		LOGn("Freeze timeout has been cut to %"PRIu32" seconds.\n",
			timeout_sec);
	}

	cancel_melt_work(wdev);
	if (freeze_if_melted(wdev, timeout_sec)) {
		return 0;
	}
	return -EFAULT;
}

/**
 * Check whether the device is frozen or not.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_is_frozen(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	int is_frozen = 0;
	ASSERT(ctl->command == WALB_IOCTL_IS_FROZEN);
	LOGn("WALB_IOCTL_IS_FROZEN\n");

	mutex_lock(&wdev->freeze_lock);
	is_frozen = (wdev->freeze_state == FRZ_MELTED) ? 0 : 1;
	mutex_unlock(&wdev->freeze_lock);

	ctl->val_int = is_frozen;

	return 0;
}

/**
 * Melt a frozen device.
 *
 * @wdev walb dev.
 * @ctl ioctl data.
 * RETURN:
 *   0 in success, or -EFAULT.
 */
static int ioctl_wdev_melt(struct walb_dev *wdev, struct walb_ctl *ctl)
{
	ASSERT(ctl->command == WALB_IOCTL_MELT);
	LOGn("WALB_IOCTL_MELT\n");

	cancel_melt_work(wdev);
	if (melt_if_frozen(wdev, true)) {
		return 0;
	}
	return -EFAULT;
}

/*
 * The device operations structure.
 */
static struct block_device_operations walb_ops = {
	.owner		 = THIS_MODULE,
	.open		 = walb_open,
	.release	 = walb_release,
	.ioctl		 = walb_ioctl
};

/**
 * Open a walb device.
 */
static int walblog_open(struct block_device *bdev, fmode_t mode)
{
	struct walb_dev *wdev = get_wdev_from_gd(bdev->bd_disk);
	int n_users;

	n_users = atomic_inc_return(&wdev->log_n_users);
	if (n_users == 1) {
#if 0
		LOGn("This is the first time to open walblog device %d"
			" and check_disk_change() will be called.\n",
			MINOR(wdev->devt));
		check_disk_change(bdev);
#endif
	}
	return 0;
}

/**
 * Release a walblog device.
 */
static int walblog_release(struct gendisk *gd, fmode_t mode)
{
	struct walb_dev *wdev = get_wdev_from_gd(gd);
	int n_users;

	n_users = atomic_dec_return(&wdev->log_n_users);
	ASSERT(n_users >= 0);
	return 0;
}

static int walblog_ioctl(struct block_device *bdev, fmode_t mode,
			unsigned int cmd, unsigned long arg)
{
	long size;
	struct hd_geometry geo;
	struct walb_dev *wdev = bdev->bd_disk->private_data;

	switch(cmd) {
	case HDIO_GETGEO:
		size = wdev->ldev_size;
		geo.cylinders = (size & ~0x3f) >> 6;
		geo.heads = 4;
		geo.sectors = 16;
		geo.start = 4;
		if (copy_to_user((void __user *) arg, &geo, sizeof(geo)))
			return -EFAULT;
		return 0;
	default:
		return -ENOTTY;
	}
}

static struct block_device_operations walblog_ops = {
	.owner	 = THIS_MODULE,
	.open	 = walblog_open,
	.release = walblog_release,
	.ioctl	 = walblog_ioctl
};

/**
 * Get written lsid of a walb data device.
 *
 * @return written_lsid of the walb device.
 */
static u64 get_written_lsid(struct walb_dev *wdev)
{
	u64 ret;

	ASSERT(wdev);

	spin_lock(&wdev->lsid_lock);
	ret = wdev->lsids.written;
	spin_unlock(&wdev->lsid_lock);

	return ret;
}

/**
 * Get permanent_lsid of the walb device.
 *
 * @return permanent_lsid of the walb device.
 */
static u64 get_permanent_lsid(struct walb_dev *wdev)
{
	u64 ret;

	ASSERT(wdev);

	spin_lock(&wdev->lsid_lock);
	ret = wdev->lsids.permanent;
	spin_unlock(&wdev->lsid_lock);

	return ret;
}

/**
 * Get completed lsid of a walb log device.
 *
 * RETURN:
 *   completed_lsid of the walb device.
 */
static u64 get_completed_lsid(struct walb_dev *wdev)
{
#ifdef WALB_FAST_ALGORITHM
	u64 ret;
	spin_lock(&wdev->lsid_lock);
	ret = wdev->lsids.completed;
	spin_unlock(&wdev->lsid_lock);
	return ret;
#else /* WALB_FAST_ALGORITHM */
	return get_written_lsid(wdev);
#endif /* WALB_FAST_ALGORITHM */
}

/**
 * Get log usage.
 *
 * RETURN:
 *   Log usage [physical block].
 */
static u64 get_log_usage(struct walb_dev *wdev)
{
	u64 latest_lsid, oldest_lsid;

	spin_lock(&wdev->lsid_lock);
	latest_lsid = wdev->lsids.latest;
	oldest_lsid = wdev->lsids.oldest;
	spin_unlock(&wdev->lsid_lock);

	ASSERT(latest_lsid >= oldest_lsid);
	return latest_lsid - oldest_lsid;
}

/**
 * Get log capacity of a walb device.
 *
 * @return ring_buffer_size of the walb device.
 */
static u64 get_log_capacity(struct walb_dev *wdev)
{
	ASSERT(wdev);

	return wdev->ring_buffer_size;
}

/**
 * Set device name.
 *
 * @wdev walb device.
 * @minor minor id. This will be used for default name.
 * @name Name to set.
 *   If null or empty string is given and
 *   the preset name is empty,
 *   default name will be set using minor id.
 *
 * @return 0 in success, or -1.
 */
static int walb_set_name(struct walb_dev *wdev,
			unsigned int minor, const char *name)
{
	int name_len;
	char *dev_name;

	ASSERT(wdev);
	ASSERT(wdev->lsuper0);

	dev_name = get_super_sector(wdev->lsuper0)->name;

	if (name && *name) {
		memset(dev_name, 0, DISK_NAME_LEN);
		snprintf(dev_name, DISK_NAME_LEN, "%s", name);
	} else if (*dev_name == 0) {
		memset(dev_name, 0, DISK_NAME_LEN);
		snprintf(dev_name, DISK_NAME_LEN, "%u", minor / 2);
	}
	LOGd("minor %u dev_name: %s\n", minor, dev_name);

	name_len = strlen(dev_name);
	ASSERT(name_len < DISK_NAME_LEN);
	if (name_len > WALB_DEV_NAME_MAX_LEN) {
		LOGe("Device name is too long: %s.\n", name);
		return -1;
	}
	return 0;
}

/**
 * Decide flush support or not.
 */
static void walb_decide_flush_support(struct walb_dev *wdev)
{
	struct request_queue *q, *lq, *dq;
	ASSERT(wdev);

	/* Get queues. */
	q = wdev->queue;
	ASSERT(q);
	lq = bdev_get_queue(wdev->ldev);
	dq = bdev_get_queue(wdev->ddev);

	/* Check REQ_FLUSH/REQ_FUA supports. */
	if (lq->flush_flags & REQ_FLUSH && dq->flush_flags & REQ_FLUSH) {
		if (lq->flush_flags & REQ_FUA) {
			LOGn("Supports REQ_FLUSH | REQ_FUA.");
			blk_queue_flush(q, REQ_FLUSH | REQ_FUA);
		} else {
			LOGn("Supports REQ_FLUSH.");
			blk_queue_flush(q, REQ_FLUSH);
		}
		blk_queue_flush_queueable(q, true);
	} else {
		LOGw("REQ_FLUSH is not suported!\n"
			"WalB can not guarantee data consistency...\n");
	}
}

/**
 * Support discard.
 */
static void walb_discard_support(struct walb_dev *wdev)
{
	struct request_queue *q = wdev->queue;

	LOGn("Supports REQ_DISCARD.\n");
	q->limits.discard_granularity = wdev->physical_bs;

	/* Should be stored in u16 variable and aligned. */
	q->limits.max_discard_sectors = 1 << 15;
	q->limits.discard_zeroes_data = 0;
	queue_flag_set_unlocked(QUEUE_FLAG_DISCARD, q);
}

/**
 * Resize disk.
 *
 * @gd disk.
 * @new_size new size [logical block].
 *
 * RETURN:
 *   true in success, or false.
 */
static bool resize_disk(struct gendisk *gd, u64 new_size)
{
	struct block_device *bdev;
	u64 old_size;

	ASSERT(gd);

	old_size = get_capacity(gd);
	if (old_size == new_size) {
		return true;
	}
	set_capacity(gd, new_size);

	bdev = bdget_disk(gd, 0);
	if (!bdev) {
		LOGe("bdget_disk failed.\n");
		return false;
	}
	mutex_lock(&bdev->bd_mutex);
	if (old_size > new_size) {
		LOGn("Shrink disk should discard block cache.\n");
		check_disk_size_change(gd, bdev);
		bdev->bd_invalidated = 0; /* This is bugfix. */
	} else {
		i_size_write(bdev->bd_inode,
			(loff_t)new_size * LOGICAL_BLOCK_SIZE);
	}
	mutex_unlock(&bdev->bd_mutex);
	bdput(bdev);
	return true;
}

/**
 * Invalidate lsid inside ring buffer.
 */
static bool invalidate_lsid(struct walb_dev *wdev, u64 lsid)
{
	struct sector_data *zero_sector;
	struct walb_super_sector *super;
	u64 off;
	bool ret;

	ASSERT(lsid != INVALID_LSID);

	zero_sector = sector_alloc(
		wdev->physical_bs, GFP_KERNEL | __GFP_ZERO);
	if (!zero_sector) {
		LOGe("sector allocation failed.\n");
		return false;
	}

	spin_lock(&wdev->lsuper0_lock);
	super = get_super_sector(wdev->lsuper0);
	off = get_offset_of_lsid_2(super, lsid);
	spin_unlock(&wdev->lsuper0_lock);

	ret = sector_io(WRITE, wdev->ldev, off, zero_sector);
	if (!ret) {
		LOGe("sector write failed.\n");
		iocore_set_readonly(wdev);
	}
	sector_free(zero_sector);
	return ret;
}

/**
 * Backup lsids.
 */
static void backup_lsid_set(struct walb_dev *wdev, struct lsid_set *lsids)
{
	spin_lock(&wdev->lsid_lock);
	*lsids = wdev->lsids;
	spin_unlock(&wdev->lsid_lock);
}

/**
 * Restore lsids.
 */
static void restore_lsid_set(struct walb_dev *wdev, const struct lsid_set *lsids)
{
	spin_lock(&wdev->lsid_lock);
	wdev->lsids = *lsids;
	spin_unlock(&wdev->lsid_lock);
}

/**
 * Melt a frozen device.
 */
static void task_melt(struct work_struct *work)
{
	struct delayed_work *dwork
		= container_of(work, struct delayed_work, work);
	struct walb_dev *wdev
		= container_of(dwork, struct walb_dev, freeze_dwork);
	ASSERT(wdev);

	mutex_lock(&wdev->freeze_lock);

	switch (wdev->freeze_state) {
	case FRZ_MELTED:
		LOGn("FRZ_MELTED minor %u.\n", MINOR(wdev->devt));
		break;
	case FRZ_FREEZED:
		LOGn("FRZ_FREEZED minor %u.\n", MINOR(wdev->devt));
		break;
	case FRZ_FREEZED_WITH_TIMEOUT:
		LOGn("Melt walb device minor %u.\n", MINOR(wdev->devt));
		start_checkpointing(&wdev->cpd);
		iocore_melt(wdev);
		wdev->freeze_state = FRZ_MELTED;
		break;
	default:
		BUG();
	}

	mutex_unlock(&wdev->freeze_lock);
}

/**
 * Cancel the melt work if enqueued.
 */
static void cancel_melt_work(struct walb_dev *wdev)
{
	bool should_cancel_work = false;

	/* Check existance of the melt work. */
	mutex_lock(&wdev->freeze_lock);
	if (wdev->freeze_state == FRZ_FREEZED_WITH_TIMEOUT) {
		should_cancel_work = true;
		wdev->freeze_state = FRZ_FREEZED;
	}
	mutex_unlock(&wdev->freeze_lock);

	/* Cancel the melt work if required. */
	if (should_cancel_work) {
		cancel_delayed_work_sync(&wdev->freeze_dwork);
	}
}


/**
 * Freeze if melted and enqueue a melting work if required.
 *
 * @wdev walb device.
 * @timeout_sec timeout to melt the device [sec].
 *   Specify 0 for no timeout.
 *
 * RETURN:
 *   true in success, or false (due to race condition).
 */
static bool freeze_if_melted(struct walb_dev *wdev, u32 timeout_sec)
{
	unsigned int minor;
	int ret;

	ASSERT(wdev);
	minor = MINOR(wdev->devt);

	/* Freeze and enqueue a melt work if required. */
	mutex_lock(&wdev->freeze_lock);
	switch (wdev->freeze_state) {
	case FRZ_MELTED:
		/* Freeze iocore and checkpointing. */
		LOGn("Freeze walb device minor %u.\n", minor);
		iocore_freeze(wdev);
		stop_checkpointing(&wdev->cpd);
		wdev->freeze_state = FRZ_FREEZED;
		break;
	case FRZ_FREEZED:
		/* Do nothing. */
		LOGn("Already frozen minor %u.\n", minor);
		break;
	case FRZ_FREEZED_WITH_TIMEOUT:
		LOGe("Race condition occured.\n");
		mutex_unlock(&wdev->freeze_lock);
		return false;
	default:
		BUG();
	}
	ASSERT(wdev->freeze_state == FRZ_FREEZED);
	if (timeout_sec > 0) {
		LOGn("(Re)set frozen timeout to %"PRIu32" seconds.\n",
			timeout_sec);
		INIT_DELAYED_WORK(&wdev->freeze_dwork, task_melt);
		ret = queue_delayed_work(
			wq_misc_, &wdev->freeze_dwork,
			msecs_to_jiffies(timeout_sec * 1000));
		ASSERT(ret);
		wdev->freeze_state = FRZ_FREEZED_WITH_TIMEOUT;
	}
	ASSERT(wdev->freeze_state != FRZ_MELTED);
	mutex_unlock(&wdev->freeze_lock);
	return true;
}

/**
 * Melt a device if frozen.
 *
 * RETURN:
 *   true in success, or false (due to race condition).
 */
static bool melt_if_frozen(
	struct walb_dev *wdev, bool restarts_checkpointing)
{
	unsigned int minor;

	ASSERT(wdev);
	minor = MINOR(wdev->devt);

	cancel_melt_work(wdev);

	/* Melt the device if required. */
	mutex_lock(&wdev->freeze_lock);
	switch (wdev->freeze_state) {
	case FRZ_MELTED:
		/* Do nothing. */
		LOGn("Already melted minor %u\n", minor);
		break;
	case FRZ_FREEZED:
		/* Melt. */
		LOGn("Melt walb device minor %u.\n", minor);
		if (restarts_checkpointing) {
			start_checkpointing(&wdev->cpd);
		}
		iocore_melt(wdev);
		wdev->freeze_state = FRZ_MELTED;
		break;
	case FRZ_FREEZED_WITH_TIMEOUT:
		/* Race condition. */
		LOGe("Race condition occurred.\n");
		mutex_unlock(&wdev->freeze_lock);
		return false;
	default:
		BUG();
	}
	ASSERT(wdev->freeze_state == FRZ_MELTED);
	mutex_unlock(&wdev->freeze_lock);
	return true;
}

/**
 * Initialize workqueues.
 *
 * RETURN:
 *   true in success, or false.
 */
static bool initialize_workqueues(void)
{
#ifdef MSG
#error
#endif
#define MSG "Failed to allocate the workqueue %s.\n"
	wq_normal_ = alloc_workqueue(WQ_NORMAL_NAME, WQ_MEM_RECLAIM, 0);
	if (!wq_normal_) {
		LOGe(MSG, WQ_NORMAL_NAME);
		goto error0;
	}
	wq_nrt_ = alloc_workqueue(WQ_NRT_NAME,
				WQ_MEM_RECLAIM | WQ_NON_REENTRANT, 0);
	if (!wq_nrt_) {
		LOGe(MSG, WQ_NRT_NAME);
		goto error0;
	}
	wq_unbound_ = alloc_workqueue(WQ_UNBOUND_NAME,
				WQ_MEM_RECLAIM | WQ_UNBOUND, WQ_UNBOUND_MAX_ACTIVE);
	if (!wq_unbound_) {
		LOGe(MSG, WQ_UNBOUND_NAME);
		goto error0;
	}
	wq_misc_ = alloc_workqueue(WQ_MISC_NAME, WQ_MEM_RECLAIM, 0);
	if (!wq_misc_) {
		LOGe(MSG, WQ_MISC_NAME);
		goto error0;
	}
	return true;
#undef MSG
error0:
	finalize_workqueues();
	return false;
}

/**
 * Finalize workqueues.
 */
static void finalize_workqueues(void)
{
	if (wq_misc_) {
		destroy_workqueue(wq_misc_);
		wq_misc_ = NULL;
	}
	if (wq_unbound_) {
		destroy_workqueue(wq_unbound_);
		wq_unbound_ = NULL;
	}
	if (wq_nrt_) {
		destroy_workqueue(wq_nrt_);
		wq_nrt_ = NULL;
	}
	if (wq_normal_) {
		destroy_workqueue(wq_normal_);
		wq_normal_ = NULL;
	}
}

/**
 * Initialize walb block device.
 *
 * @wdev walb_dev.
 * @minor minor id.
 * @name disk name.
 */
static int walb_prepare_device(
	struct walb_dev *wdev, unsigned int minor, const char *name)
{
	struct request_queue *lq, *dq;

	/* Using bio interface */
	wdev->queue = blk_alloc_queue(GFP_KERNEL);
	if (!wdev->queue)
		goto out;
	blk_queue_make_request(wdev->queue, walb_make_request);
	wdev->queue->queuedata = wdev;

	/* Queue limits. */
	blk_set_default_limits(&wdev->queue->limits);
	blk_queue_logical_block_size(wdev->queue, LOGICAL_BLOCK_SIZE);
	blk_queue_physical_block_size(wdev->queue, wdev->physical_bs);
	lq = bdev_get_queue(wdev->ldev);
	dq = bdev_get_queue(wdev->ddev);
	blk_queue_stack_limits(wdev->queue, lq);
	blk_queue_stack_limits(wdev->queue, dq);

	/* Allocate a gendisk and set parameters. */
	wdev->gd = alloc_disk(1);
	if (!wdev->gd) {
		LOGe("alloc_disk failure.\n");
		goto out_queue;
	}
	wdev->gd->major = walb_major_;
	wdev->gd->first_minor = minor;
	wdev->devt = MKDEV(wdev->gd->major, wdev->gd->first_minor);
	wdev->gd->fops = &walb_ops;
	wdev->gd->queue = wdev->queue;
	wdev->gd->private_data = wdev;
	set_capacity(wdev->gd, wdev->size);

	/* Set a name. */
	snprintf(wdev->gd->disk_name, DISK_NAME_LEN,
		"%s/%s", WALB_DIR_NAME, name);
	LOGd("device path: %s, device name: %s\n",
		wdev->gd->disk_name, name);

	/* Number of users. */
	atomic_set(&wdev->n_users, 0);

	/* Flush support. */
	walb_decide_flush_support(wdev);

	/* Discard support. */
	walb_discard_support(wdev);

	return 0;

#if 0
out_disk:
	if (wdev->gd) {
		put_disk(wdev->gd);
	}
#endif
out_queue:
	if (wdev->queue) {
		blk_cleanup_queue(wdev->queue);
	}
out:
	return -1;
}

/**
 * Finalize walb block device.
 */
static void walb_finalize_device(struct walb_dev *wdev)
{
	if (wdev->gd) {
		put_disk(wdev->gd);
	}
	if (wdev->queue) {
		blk_cleanup_queue(wdev->queue);
	}
}

/**
 * Setup walblog device.
 */
static int walblog_prepare_device(struct walb_dev *wdev,
				unsigned int minor, const char* name)
{
	struct request_queue *lq;

	wdev->log_queue = blk_alloc_queue(GFP_KERNEL);
	if (!wdev->log_queue)
		goto error0;

	blk_queue_make_request(wdev->log_queue, walblog_make_request);
	wdev->log_queue->queuedata = wdev;

	/* Queue limits. */
	lq = bdev_get_queue(wdev->ldev);
	blk_set_default_limits(&wdev->log_queue->limits);
	blk_queue_logical_block_size(wdev->log_queue, LOGICAL_BLOCK_SIZE);
	blk_queue_physical_block_size(wdev->log_queue, wdev->physical_bs);
	blk_queue_stack_limits(wdev->log_queue, lq);

	/* Allocate a gendisk and set parameters. */
	wdev->log_gd = alloc_disk(1);
	if (! wdev->log_gd) {
		goto error1;
	}
	wdev->log_gd->major = walb_major_;
	wdev->log_gd->first_minor = minor;
	wdev->log_gd->queue = wdev->log_queue;
	wdev->log_gd->fops = &walblog_ops;
	wdev->log_gd->private_data = wdev;
	set_capacity(wdev->log_gd, wdev->ldev_size);

	/* Set a name. */
	snprintf(wdev->log_gd->disk_name, DISK_NAME_LEN,
		"%s/L%s", WALB_DIR_NAME, name);
	atomic_set(&wdev->log_n_users , 0);
	return 0;

error1:
	if (wdev->log_queue) {
		blk_cleanup_queue(wdev->log_queue);
	}
error0:
	return -1;
}

/**
 * Finalize walblog wrapper device.
 */
static void walblog_finalize_device(struct walb_dev *wdev)
{
	if (wdev->log_gd) {
		put_disk(wdev->log_gd);
	}
	if (wdev->log_queue) {
		blk_cleanup_queue(wdev->log_queue);
	}
}

/**
 * Log device initialization.
 *
 * Read log device metadata
 *    (currently snapshot metadata is not loaded.
 *     super sector0 only...)
 *
 * @wdev walb device struct.
 * @return 0 in success, or -1.
 */
static int walb_ldev_initialize(struct walb_dev *wdev)
{
	u64 snapshot_begin_pb, snapshot_end_pb;
	struct sector_data *lsuper0_tmp;
	int ret;

	ASSERT(wdev);

	/*
	 * 1. Read log device metadata
	 */
	wdev->lsuper0 = sector_alloc(wdev->physical_bs, GFP_NOIO);
	if (!wdev->lsuper0) {
		LOGe("walb_ldev_init: alloc sector failed.\n");
		goto error0;
	}
	lsuper0_tmp = sector_alloc(wdev->physical_bs, GFP_NOIO);
	if (!lsuper0_tmp) {
		LOGe("walb_ldev_init: alloc sector failed.\n");
		goto error1;
	}

	if (!walb_read_super_sector(wdev->ldev, wdev->lsuper0)) {
		LOGe("walb_ldev_init: read super sector failed.\n");
		goto error2;
	}
	if (!walb_write_super_sector(wdev->ldev, wdev->lsuper0)) {
		LOGe("walb_ldev_init: write super sector failed.\n");
		goto error2;
	}

	if (!walb_read_super_sector(wdev->ldev, lsuper0_tmp)) {
		LOGe("walb_ldev_init: read super sector failed.\n");
		goto error2;
	}

	if (!is_same_sector(wdev->lsuper0, lsuper0_tmp)) {
		LOGe("walb_ldev_init: memcmp NG\n");
		goto error2;
	}

	if (get_super_sector_const(wdev->lsuper0)->physical_bs
		!= wdev->physical_bs) {
		LOGe("Physical block size is different.\n");
		goto error2;
	}

	sector_free(lsuper0_tmp);
	/* Do not forget calling kfree(dev->lsuper0)
	   before releasing the block device. */

	/*
	 * 2. Prepare and initialize snapshot data structure.
	 */
	snapshot_begin_pb = get_metadata_offset(wdev->physical_bs);
	snapshot_end_pb = snapshot_begin_pb +
		get_super_sector(wdev->lsuper0)->snapshot_metadata_size;
	LOGd("snapshot offset range: [%"PRIu64",%"PRIu64").\n",
		snapshot_begin_pb, snapshot_end_pb);
	wdev->snapd = snapshot_data_create
		(wdev->ldev, snapshot_begin_pb, snapshot_end_pb);
	if (!wdev->snapd) {
		LOGe("snapshot_data_create() failed.\n");
		goto error2;
	}
	/* Initialize snapshot data by scanning snapshot sectors. */
	ret = snapshot_data_initialize(wdev->snapd);
	if (!ret) {
		LOGe("Initialize snapshot data failed.\n");
		goto error3;
	}

	return 0;
#if 0
error4:
	if (wdev->snapd) {
		snapshot_data_finalize(wdev->snapd);
	}
#endif
error3:
	snapshot_data_destroy(wdev->snapd);
error2:
	sector_free(lsuper0_tmp);
error1:
	sector_free(wdev->lsuper0);
error0:
	return -1;
}

/**
 * Finalize log device.
 */
static void walb_ldev_finalize(struct walb_dev *wdev)
{
	ASSERT(wdev);
	ASSERT(wdev->lsuper0);
	ASSERT(wdev->snapd);

	snapshot_data_finalize(wdev->snapd);
	snapshot_data_destroy(wdev->snapd);

	if (!walb_finalize_super_block(wdev, is_sync_superblock_)) {
		LOGe("finalize super block failed.\n");
	}
	sector_free(wdev->lsuper0);
}

/**
 * Register walb block device.
 */
static void walb_register_device(struct walb_dev *wdev)
{
	ASSERT(wdev);
	ASSERT(wdev->gd);

	add_disk(wdev->gd);
}

/**
 * Unregister walb wrapper device.
 */
static void walb_unregister_device(struct walb_dev *wdev)
{
	LOGd("walb_unregister_device begin.\n");
	if (wdev->gd) {
		del_gendisk(wdev->gd);
	}
	LOGd("walb_unregister_device end.\n");
}

/**
 * Register walblog block device.
 */
static void walblog_register_device(struct walb_dev *wdev)
{
	ASSERT(wdev);
	ASSERT(wdev->log_gd);

	add_disk(wdev->log_gd);
}

/**
 * Unregister walblog wrapper device.
 */
static void walblog_unregister_device(struct walb_dev *wdev)
{
	LOGd("walblog_unregister_device begin.\n");
	if (wdev->log_gd) {
		del_gendisk(wdev->log_gd);
	}
	LOGd("walblog_unregister_device end.\n");
}

static int __init walb_init(void)
{
	/* DISK_NAME_LEN assersion */
	ASSERT_DISK_NAME_LEN();

	/*
	 * Get registered.
	 */
	walb_major_ = register_blkdev(walb_major_, WALB_NAME);
	if (walb_major_ <= 0) {
		LOGw("unable to get major number.\n");
		return -EBUSY;
	}
	LOGi("walb_start with major id %d.\n", walb_major_);

	/*
	 * Workqueues.
	 */
	if (!initialize_workqueues()) {
		goto out_register;
	}

	/*
	 * Alldevs.
	 */
	if (alldevs_init() != 0) {
		LOGe("alldevs_init failed.\n");
		goto out_workqueues;
	}

	/*
	 * Init control device.
	 */
	if (walb_control_init() != 0) {
		LOGe("walb_control_init failed.\n");
		goto out_alldevs_exit;
	}

	return 0;

#if 0
out_control_exit:
	walb_control_exit();
#endif
out_alldevs_exit:
	alldevs_exit();
out_workqueues:
	finalize_workqueues();
out_register:
	unregister_blkdev(walb_major_, WALB_NAME);
	return -ENOMEM;
}

static void walb_exit(void)
{
	struct walb_dev *wdev;

	alldevs_write_lock();
	wdev = alldevs_pop();
	while (wdev) {
		unregister_wdev(wdev);
		destroy_wdev(wdev);
		wdev = alldevs_pop();
	}
	alldevs_write_unlock();

	finalize_workqueues();
	unregister_blkdev(walb_major_, WALB_NAME);
	walb_control_exit();
	alldevs_exit();

	LOGi("walb exit.\n");
}


/*******************************************************************************
 * Global functions.
 *******************************************************************************/

/**
 * Prepare walb device.
 * You must call @register_wdev() after calling this.
 *
 * @minor minor id of the device (must not be WALB_DYNAMIC_MINOR).
 *	  walblog device minor will be (minor + 1).
 * @ldevt device id of log device.
 * @ddevt device id of data device.
 * @param parameters. (this will be updated)
 *
 * @return allocated and prepared walb_dev data, or NULL.
 */
struct walb_dev* prepare_wdev(
	unsigned int minor, dev_t ldevt, dev_t ddevt,
	struct walb_start_param *param)
{
	struct walb_dev *wdev;
	u16 ldev_lbs, ldev_pbs, ddev_lbs, ddev_pbs;
	char *dev_name;
	struct walb_super_sector *super;
	struct request_queue *lq, *dq;
	bool retb;
#ifdef WALB_DEBUG
	u64 written_lsid, latest_lsid, flush_lsid;
#ifdef WALB_FAST_ALGORITHM
	u64 completed_lsid;
#endif
#endif

	ASSERT(is_walb_start_param_valid(param));

	/* Minor id check. */
	if (minor == WALB_DYNAMIC_MINOR) {
		LOGe("Do not specify WALB_DYNAMIC_MINOR.\n");
		goto out;
	}

	/*
	 * Initialize walb_dev.
	 */
	wdev = kzalloc(sizeof(struct walb_dev), GFP_KERNEL);
	if (!wdev) {
		LOGe("kmalloc failed.\n");
		goto out;
	}
	spin_lock_init(&wdev->lsid_lock);
	spin_lock_init(&wdev->lsuper0_lock);
	spin_lock_init(&wdev->size_lock);
	atomic_set(&wdev->is_read_only, 0);
	mutex_init(&wdev->freeze_lock);
	wdev->freeze_state = FRZ_MELTED;

	/*
	 * Open underlying log device.
	 */
	if (walb_lock_bdev(&wdev->ldev, ldevt) != 0) {
		LOGe("walb_lock_bdev failed (%u:%u for log)\n",
			MAJOR(ldevt), MINOR(ldevt));
		goto out_free;
	}
	wdev->ldev_size = wdev->ldev->bd_part->nr_sects;
	ldev_lbs = bdev_logical_block_size(wdev->ldev);
	ldev_pbs = bdev_physical_block_size(wdev->ldev);
	ASSERT(ldev_lbs == LOGICAL_BLOCK_SIZE);
	LOGi("log disk (%u:%u)\n"
		"log disk size %llu\n"
		"log logical sector size %u\n"
		"log physical sector size %u\n",
		MAJOR(ldevt), MINOR(ldevt),
		wdev->ldev_size,
		ldev_lbs, ldev_pbs);

	/*
	 * Open underlying data device.
	 */
	if (walb_lock_bdev(&wdev->ddev, ddevt) != 0) {
		LOGe("walb_lock_bdev failed (%u:%u for data)\n",
			MAJOR(ddevt), MINOR(ddevt));
		goto out_ldev;
	}
	wdev->ddev_size = wdev->ddev->bd_part->nr_sects;
	ddev_lbs = bdev_logical_block_size(wdev->ddev);
	ddev_pbs = bdev_physical_block_size(wdev->ddev);
	ASSERT(ddev_lbs == LOGICAL_BLOCK_SIZE);
	LOGi("data disk (%d:%d)\n"
		"data disk size %llu\n"
		"data logical sector size %u\n"
		"data physical sector size %u\n",
		MAJOR(ddevt), MINOR(ddevt),
		wdev->ddev_size,
		ddev_lbs, ddev_pbs);

	/* Check compatibility of log device and data device. */
	if (ldev_pbs != ddev_pbs) {
		LOGe("Sector size of data and log must be same.\n");
		goto out_ddev;
	}
	wdev->physical_bs = ldev_pbs;

	/* Load log device metadata. */
	if (walb_ldev_initialize(wdev) != 0) {
		LOGe("ldev init failed.\n");
		goto out_ddev;
	}
	super = get_super_sector(wdev->lsuper0);
	ASSERT(super);
	init_checkpointing(&wdev->cpd);

	/* Set lsids. */
	wdev->lsids.oldest = super->oldest_lsid;
	wdev->lsids.prev_written = wdev->lsids.written;
	wdev->lsids.written = super->written_lsid;
	wdev->lsids.permanent = wdev->lsids.written;
#ifdef WALB_FAST_ALGORITHM
	wdev->lsids.completed = wdev->lsids.written;
#endif
	wdev->lsids.latest = wdev->lsids.written;

	wdev->ring_buffer_size = super->ring_buffer_size;
	wdev->ring_buffer_off = get_ring_buffer_offset_2(super);
	wdev->log_checksum_salt = super->log_checksum_salt;
	wdev->size = super->device_size;
	if (wdev->size > wdev->ddev_size) {
		LOGe("device size > underlying data device size.\n");
		goto out_ldev_init;
	}

	/* Set parameters. */
	wdev->max_logpack_pb =
		min(param->max_logpack_kb * 1024 / wdev->physical_bs,
			MAX_TOTAL_IO_SIZE_IN_LOGPACK_HEADER);
	wdev->log_flush_interval_jiffies =
		msecs_to_jiffies(param->log_flush_interval_ms);
	if (wdev->log_flush_interval_jiffies == 0) {
		wdev->log_flush_interval_pb = 0;
	} else {
		wdev->log_flush_interval_pb = param->log_flush_interval_mb
			* (1024 * 1024 / wdev->physical_bs);
	}
	LOGn("max_logpack_pb: %u\n"
		"log_flush_interval_jiffies: %u\n"
		"log_flush_interval_pb: %u\n",
		wdev->max_logpack_pb,
		wdev->log_flush_interval_jiffies,
		wdev->log_flush_interval_pb);
#ifdef WALB_FAST_ALGORITHM
	ASSERT(0 < param->min_pending_mb);
	ASSERT(param->min_pending_mb < param->max_pending_mb);
	wdev->max_pending_sectors
		= param->max_pending_mb * 1024 * 1024 / LOGICAL_BLOCK_SIZE;
	wdev->min_pending_sectors
		= param->min_pending_mb * 1024 * 1024 / LOGICAL_BLOCK_SIZE;
	wdev->queue_stop_timeout_jiffies =
		msecs_to_jiffies(param->queue_stop_timeout_ms);
	LOGn("max_pending_sectors: %u\n"
		"min_pending_sectors: %u\n"
		"queue_stop_timeout_jiffies: %u\n",
		wdev->max_pending_sectors,
		wdev->min_pending_sectors,
		wdev->queue_stop_timeout_jiffies);
#endif
	if (param->n_pack_bulk > 0) {
		wdev->n_pack_bulk = param->n_pack_bulk;
	} else {
		wdev->n_pack_bulk = 128; /* default value. */
	}
	if (param->n_io_bulk > 0) {
		wdev->n_io_bulk = param->n_io_bulk;
	} else {
		wdev->n_io_bulk = 1024; /* default value. */
	}
	LOGn("n_pack_bulk: %u\n"
		"n_io_bulk: %u\n",
		wdev->n_pack_bulk, wdev->n_io_bulk);

	lq = bdev_get_queue(wdev->ldev);
	dq = bdev_get_queue(wdev->ddev);
	/* Set chunk size. */
	if (queue_io_min(lq) > wdev->physical_bs) {
		wdev->ldev_chunk_sectors = queue_io_min(lq) / LOGICAL_BLOCK_SIZE;
	} else {
		wdev->ldev_chunk_sectors = 0;
	}
	if (queue_io_min(dq) > wdev->physical_bs) {
		wdev->ddev_chunk_sectors = queue_io_min(dq) / LOGICAL_BLOCK_SIZE;
	} else {
		wdev->ddev_chunk_sectors = 0;
	}
	LOGn("chunk_sectors ldev %u ddev %u.\n",
		wdev->ldev_chunk_sectors, wdev->ddev_chunk_sectors);

	/* Set device name. */
	if (walb_set_name(wdev, minor, param->name) != 0) {
		LOGe("Set device name failed.\n");
		goto out_ldev_init;
	}
	ASSERT_SECTOR_DATA(wdev->lsuper0);
	dev_name = super->name;
	memcpy(param->name, dev_name, DISK_NAME_LEN);

	/*
	 * Prepare walb block device.
	 */
	if (walb_prepare_device(wdev, minor, dev_name) != 0) {
		LOGe("walb_prepare_device() failed.\n");
		goto out_ldev_init;
	}

	/*
	 * Prepare walblog block device.
	 */
	if (walblog_prepare_device(wdev, minor + 1, dev_name) != 0) {
		goto out_walbdev;
	}

	/* Setup iocore data. */
	if (!iocore_initialize(wdev)) {
		LOGe("iocore initialization failed.\n");
		goto out_walblogdev;
	}

	/*
	 * Redo
	 * 1. Read logpacks starting from written_lsid.
	 * 2. Write the corresponding data of the logpacks to data device.
	 * 3. Rewrite the latest logpack if partially valid.
	 * 4. Update written_lsid, latest_lsid, (and completed_lsid).
	 * 5. Sync superblock.
	 */
	retb = execute_redo(wdev);
	if (!retb) {
		LOGe("Redo failed.\n");
		goto out_iocore_init;
	}
#ifdef WALB_DEBUG
	spin_lock(&wdev->lsid_lock);
	written_lsid = wdev->lsids.written;
	latest_lsid = wdev->lsids.latest;
	flush_lsid = wdev->lsids.flush;
#ifdef WALB_FAST_ALGORITHM
	completed_lsid = wdev->lsids.completed;
#endif
	spin_unlock(&wdev->lsid_lock);
	ASSERT(written_lsid == latest_lsid);
	ASSERT(written_lsid == flush_lsid);
#ifdef WALB_FAST_ALGORITHM
	ASSERT(written_lsid == completed_lsid);
#endif
#endif

	return wdev;

out_iocore_init:
	iocore_finalize(wdev);
out_walblogdev:
	walblog_finalize_device(wdev);
out_walbdev:
	walb_finalize_device(wdev);
out_ldev_init:
	walb_ldev_finalize(wdev);
out_ddev:
	if (wdev->ddev) {
		walb_unlock_bdev(wdev->ddev);
	}
out_ldev:
	if (wdev->ldev) {
		walb_unlock_bdev(wdev->ldev);
	}
out_free:
	kfree(wdev);
out:
	return NULL;
}

/**
 * Destroy wdev structure.
 * You must call @unregister_wdev() before calling this.
 */
void destroy_wdev(struct walb_dev *wdev)
{
	LOGi("destroy_wdev (wrap %u:%u log %u:%u data %u:%u)\n",
		MAJOR(wdev->devt),
		MINOR(wdev->devt),
		MAJOR(wdev->ldev->bd_dev),
		MINOR(wdev->ldev->bd_dev),
		MAJOR(wdev->ddev->bd_dev),
		MINOR(wdev->ddev->bd_dev));

	iocore_set_failure(wdev);
	melt_if_frozen(wdev, false);
	iocore_flush(wdev);

	walblog_finalize_device(wdev);
	walb_finalize_device(wdev);

	snapshot_data_finalize(wdev->snapd);
	walb_ldev_finalize(wdev);
	iocore_finalize(wdev);

	if (wdev->ddev)
		walb_unlock_bdev(wdev->ddev);
	if (wdev->ldev)
		walb_unlock_bdev(wdev->ldev);

	kfree(wdev);
	LOGd("destroy_wdev done.\n");
}

/**
 * Register wdev.
 * You must call @prepare_wdev() before calling this.
 */
void register_wdev(struct walb_dev *wdev)
{
	ASSERT(wdev);
	ASSERT(wdev->gd);
	ASSERT(wdev->log_gd);

	start_checkpointing(&wdev->cpd);

	walblog_register_device(wdev);
	walb_register_device(wdev);
}

/**
 * Unregister wdev.
 * You must call @destroy_wdev() after calling this.
 */
void unregister_wdev(struct walb_dev *wdev)
{
	ASSERT(wdev);

	stop_checkpointing(&wdev->cpd);

	walblog_unregister_device(wdev);
	walb_unregister_device(wdev);
}

/*******************************************************************************
 * Module definitions.
 *******************************************************************************/

module_init(walb_init);
module_exit(walb_exit);
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("Block-level WAL");
MODULE_ALIAS(WALB_NAME);
/* MODULE_ALIAS_BLOCKDEV_MAJOR(WALB_MAJOR); */

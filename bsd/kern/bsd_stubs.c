/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
#include <sys/time.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <mach/mach_types.h>
#include <mach/vm_prot.h>
#include <vm/vm_kern.h>
#include <sys/stat.h>
#include <vm/vm_map.h>
#include <sys/systm.h>
#include <kern/assert.h>
#include <sys/conf.h>
#include <sys/proc_internal.h>
#include <sys/buf.h> /* for SET */
#include <sys/kernel.h>
#include <sys/user.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>

/* XXX these should be in a common header somwhere, but aren't */
extern int chrtoblk_set(int, int);

/* XXX most of these just exist to export; there's no good header for them*/
void pcb_synch(void);

typedef struct devsw_lock {
	TAILQ_ENTRY(devsw_lock) dl_list;
	thread_t                dl_thread;
	dev_t                   dl_dev;
	int                     dl_mode;
	int                     dl_waiters;
} *devsw_lock_t;

static LCK_GRP_DECLARE(devsw_lock_grp, "devsw");
static LCK_MTX_DECLARE(devsw_lock_list_mtx, &devsw_lock_grp);
static TAILQ_HEAD(, devsw_lock) devsw_locks = TAILQ_HEAD_INITIALIZER(devsw_locks);

/* Just to satisfy pstat command */
int dmmin, dmmax, dmtext;

/*
 * XXX this function only exists to be exported and do nothing.
 */
void
pcb_synch(void)
{
}

struct proc *
current_proc(void)
{
	/* Never returns a NULL */
	struct uthread * ut;
	struct proc * p;
	thread_t thread = current_thread();

	ut = (struct uthread *)get_bsdthread_info(thread);
	if (ut && (ut->uu_flag & UT_VFORK) && ut->uu_proc) {
		p = ut->uu_proc;
		if ((p->p_lflag & P_LINVFORK) == 0) {
			panic("returning child proc not under vfork");
		}
		if (p->p_vforkact != (void *)thread) {
			panic("returning child proc which is not cur_act");
		}
		return p;
	}

	p = (struct proc *)get_bsdtask_info(current_task());

	if (p == NULL) {
		return kernproc;
	}

	return p;
}

/* Device switch add delete routines */

const struct bdevsw nobdev = NO_BDEVICE;
const struct cdevsw nocdev = NO_CDEVICE;
/*
 *	if index is -1, return a free slot if avaliable
 *	  else see whether the index is free
 *	return the major number that is free else -1
 *
 *	if index is negative, we start
 *	looking for a free slot at the absolute value of index,
 *	instead of starting at 0
 */
int
bdevsw_isfree(int index)
{
	struct bdevsw * devsw;

	if (index < 0) {
		if (index == -1) {
			index = 1; /* start at 1 to avoid collision with volfs (Radar 2842228) */
		} else {
			index = -index; /* start at least this far up in the table */
		}
		devsw = &bdevsw[index];
		for (; index < nblkdev; index++, devsw++) {
			if (memcmp((const char *)devsw, (const char *)&nobdev, sizeof(struct bdevsw)) == 0) {
				break;
			}
		}
	}

	if (index < 0 || index >= nblkdev) {
		return -1;
	}

	devsw = &bdevsw[index];
	if ((memcmp((const char *)devsw, (const char *)&nobdev, sizeof(struct bdevsw)) != 0)) {
		return -1;
	}
	return index;
}

/*
 *	if index is -1, find a free slot to add
 *	  else see whether the slot is free
 *	return the major number that is used else -1
 *
 *	if index is negative, we start
 *	looking for a free slot at the absolute value of index,
 *	instead of starting at 0
 */
int
bdevsw_add(int index, const struct bdevsw * bsw)
{
	lck_mtx_lock_spin(&devsw_lock_list_mtx);
	index = bdevsw_isfree(index);
	if (index < 0) {
		index = -1;
	} else {
		bdevsw[index] = *bsw;
	}
	lck_mtx_unlock(&devsw_lock_list_mtx);
	return index;
}
/*
 *	if the slot has the same bsw, then remove
 *	else -1
 */
int
bdevsw_remove(int index, const struct bdevsw * bsw)
{
	struct bdevsw * devsw;

	if (index < 0 || index >= nblkdev) {
		return -1;
	}

	devsw = &bdevsw[index];
	lck_mtx_lock_spin(&devsw_lock_list_mtx);
	if ((memcmp((const char *)devsw, (const char *)bsw, sizeof(struct bdevsw)) != 0)) {
		index = -1;
	} else {
		bdevsw[index] = nobdev;
	}
	lck_mtx_unlock(&devsw_lock_list_mtx);
	return index;
}

/*
 *	if index is -1, return a free slot if avaliable
 *	  else see whether the index is free
 *	return the major number that is free else -1
 *
 *	if index is negative, we start
 *	looking for a free slot at the absolute value of index,
 *	instead of starting at 0
 */
int
cdevsw_isfree(int index)
{
	struct cdevsw * devsw;

	if (index < 0) {
		if (index == -1) {
			index = 0;
		} else {
			index = -index; /* start at least this far up in the table */
		}
		devsw = &cdevsw[index];
		for (; index < nchrdev; index++, devsw++) {
			if (memcmp((const char *)devsw, (const char *)&nocdev, sizeof(struct cdevsw)) == 0) {
				break;
			}
		}
	}

	if (index < 0 || index >= nchrdev) {
		return -1;
	}

	devsw = &cdevsw[index];
	if ((memcmp((const char *)devsw, (const char *)&nocdev, sizeof(struct cdevsw)) != 0)) {
		return -1;
	}
	return index;
}

/*
 *	if index is -1, find a free slot to add
 *	  else see whether the slot is free
 *	return the major number that is used else -1
 *
 *	if index is negative, we start
 *	looking for a free slot at the absolute value of index,
 *	instead of starting at 0
 *
 * NOTE:	In practice, -1 is unusable, since there are kernel internal
 *		devices that call this function with absolute index values,
 *		which will stomp on free-slot based assignments that happen
 *		before them.  -24 is currently a safe starting point.
 */
int
cdevsw_add(int index, const struct cdevsw * csw)
{
	lck_mtx_lock_spin(&devsw_lock_list_mtx);
	index = cdevsw_isfree(index);
	if (index < 0) {
		index = -1;
	} else {
		cdevsw[index] = *csw;
	}
	lck_mtx_unlock(&devsw_lock_list_mtx);
	return index;
}
/*
 *	if the slot has the same csw, then remove
 *	else -1
 */
int
cdevsw_remove(int index, const struct cdevsw * csw)
{
	struct cdevsw * devsw;

	if (index < 0 || index >= nchrdev) {
		return -1;
	}

	devsw = &cdevsw[index];
	lck_mtx_lock_spin(&devsw_lock_list_mtx);
	if ((memcmp((const char *)devsw, (const char *)csw, sizeof(struct cdevsw)) != 0)) {
		index = -1;
	} else {
		cdevsw[index] = nocdev;
		cdevsw_flags[index] = 0;
	}
	lck_mtx_unlock(&devsw_lock_list_mtx);
	return index;
}

static int
cdev_set_bdev(int cdev, int bdev)
{
	return chrtoblk_set(cdev, bdev);
}

int
cdevsw_add_with_bdev(int index, const struct cdevsw * csw, int bdev)
{
	index = cdevsw_add(index, csw);
	if (index < 0) {
		return index;
	}
	if (cdev_set_bdev(index, bdev) < 0) {
		cdevsw_remove(index, csw);
		return -1;
	}
	return index;
}

int
cdevsw_setkqueueok(int maj, const struct cdevsw * csw, int extra_flags)
{
	struct cdevsw * devsw;
	uint64_t flags = CDEVSW_SELECT_KQUEUE;

	if (maj < 0 || maj >= nchrdev) {
		return -1;
	}

	devsw = &cdevsw[maj];
	if ((memcmp((const char *)devsw, (const char *)csw, sizeof(struct cdevsw)) != 0)) {
		return -1;
	}

	flags |= extra_flags;

	cdevsw_flags[maj] = flags;
	return 0;
}

#include <pexpert/pexpert.h> /* for PE_parse_boot_arg */

/*
 * Copy the "hostname" variable into a caller-provided buffer
 * Returns: 0 for success, ENAMETOOLONG for insufficient buffer space.
 * On success, "len" will be set to the number of characters preceding
 * the NULL character in the hostname.
 */
int
bsd_hostname(char *buf, size_t bufsize, size_t *len)
{
	int ret;
	size_t hnlen;
	/*
	 * "hostname" is null-terminated
	 */
	lck_mtx_lock(&hostname_lock);
	hnlen = strlen(hostname);
	if (hnlen < bufsize) {
		strlcpy(buf, hostname, bufsize);
		*len = hnlen;
		ret = 0;
	} else {
		ret = ENAMETOOLONG;
	}
	lck_mtx_unlock(&hostname_lock);
	return ret;
}

static devsw_lock_t
devsw_lock_find_locked(dev_t dev, int mode)
{
	devsw_lock_t lock;

	TAILQ_FOREACH(lock, &devsw_locks, dl_list) {
		if (lock->dl_dev == dev && lock->dl_mode == mode) {
			return lock;
		}
	}

	return NULL;
}

void
devsw_lock(dev_t dev, int mode)
{
	devsw_lock_t newlock, curlock;

	assert(0 <= major(dev) && major(dev) < nchrdev);
	assert(mode == S_IFCHR || mode == S_IFBLK);

	newlock = kalloc_flags(sizeof(struct devsw_lock), Z_WAITOK | Z_ZERO);
	newlock->dl_dev = dev;
	newlock->dl_thread = current_thread();
	newlock->dl_mode = mode;

	lck_mtx_lock_spin(&devsw_lock_list_mtx);

	curlock = devsw_lock_find_locked(dev, mode);
	if (curlock == NULL) {
		TAILQ_INSERT_TAIL(&devsw_locks, newlock, dl_list);
	} else {
		curlock->dl_waiters++;
		lck_mtx_sleep_with_inheritor(&devsw_lock_list_mtx,
		    LCK_SLEEP_SPIN, curlock, curlock->dl_thread,
		    THREAD_UNINT | THREAD_WAIT_NOREPORT,
		    TIMEOUT_WAIT_FOREVER);
		assert(curlock->dl_thread == current_thread());
		curlock->dl_waiters--;
	}

	lck_mtx_unlock(&devsw_lock_list_mtx);

	if (curlock != NULL) {
		kfree(newlock, sizeof(struct devsw_lock));
	}
}

void
devsw_unlock(dev_t dev, int mode)
{
	devsw_lock_t lock;
	thread_t inheritor_thread = NULL;

	assert(0 <= major(dev) && major(dev) < nchrdev);

	lck_mtx_lock_spin(&devsw_lock_list_mtx);

	lock = devsw_lock_find_locked(dev, mode);

	if (lock == NULL || lock->dl_thread != current_thread()) {
		panic("current thread doesn't own the lock (%p)", lock);
	}

	if (lock->dl_waiters) {
		wakeup_one_with_inheritor(lock, THREAD_AWAKENED,
		    LCK_WAKE_DEFAULT, &lock->dl_thread);
		inheritor_thread = lock->dl_thread;
		lock = NULL;
	} else {
		TAILQ_REMOVE(&devsw_locks, lock, dl_list);
	}

	lck_mtx_unlock(&devsw_lock_list_mtx);

	if (inheritor_thread) {
		thread_deallocate(inheritor_thread);
	}
	kfree(lock, sizeof(struct devsw_lock));
}

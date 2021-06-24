/*
 * Copyright (c) 2015-2020 Apple Inc. All rights reserved.
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
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <sys/kernel.h>
#include <sys/kernel_types.h>
#include <kern/zalloc.h>
#include <sys/reason.h>
#include <string.h>
#include <kern/assert.h>
#include <kern/debug.h>

extern int maxproc;

/*
 * Lock group attributes for os_reason subsystem
 */
static LCK_GRP_DECLARE(os_reason_lock_grp, "os_reason_lock");
static ZONE_DECLARE(os_reason_zone, "os reasons",
    sizeof(struct os_reason), ZC_ZFREE_CLEARMEM);

os_refgrp_decl(static, os_reason_refgrp, "os_reason", NULL);

static int os_reason_alloc_buffer_internal(os_reason_t cur_reason, uint32_t osr_bufsize,
    zalloc_flags_t flags);

/*
 * Creates a new reason and initializes it with the provided reason
 * namespace and code. Also sets up the buffer and kcdata_descriptor
 * associated with the reason. Returns a pointer to the newly created
 * reason.
 *
 * Returns:
 * REASON_NULL if unable to allocate a reason or initialize the nested buffer
 * a pointer to the reason otherwise
 */
os_reason_t
os_reason_create(uint32_t osr_namespace, uint64_t osr_code)
{
	os_reason_t new_reason;

	new_reason = zalloc_flags(os_reason_zone, Z_WAITOK | Z_ZERO);
	new_reason->osr_namespace = osr_namespace;
	new_reason->osr_code = osr_code;
	lck_mtx_init(&new_reason->osr_lock, &os_reason_lock_grp, LCK_ATTR_NULL);
	os_ref_init(&new_reason->osr_refcount, &os_reason_refgrp);

	return new_reason;
}

static void
os_reason_dealloc_buffer(os_reason_t cur_reason)
{
	assert(cur_reason != OS_REASON_NULL);
	LCK_MTX_ASSERT(&cur_reason->osr_lock, LCK_MTX_ASSERT_OWNED);

	if (cur_reason->osr_kcd_buf != NULL && cur_reason->osr_bufsize != 0) {
		kheap_free(KHEAP_DATA_BUFFERS, cur_reason->osr_kcd_buf,
		    cur_reason->osr_bufsize);
	}

	cur_reason->osr_bufsize = 0;
	cur_reason->osr_kcd_buf = NULL;
	bzero(&cur_reason->osr_kcd_descriptor, sizeof(cur_reason->osr_kcd_descriptor));
}

/*
 * Allocates and initializes a buffer of specified size for the reason. This function
 * may block and should not be called from extremely performance sensitive contexts
 * (i.e. jetsam). Also initializes the kcdata descriptor accordingly. If there is an
 * existing buffer, we dealloc the buffer before allocating a new one and
 * clear the associated kcdata descriptor. If osr_bufsize is passed as 0,
 * we deallocate the existing buffer and then return.
 *
 * Returns:
 * 0 on success
 * EINVAL if the passed reason pointer is invalid or the requested size is
 *        larger than REASON_BUFFER_MAX_SIZE
 * EIO if we fail to initialize the kcdata buffer
 */
int
os_reason_alloc_buffer(os_reason_t cur_reason, uint32_t osr_bufsize)
{
	return os_reason_alloc_buffer_internal(cur_reason, osr_bufsize, Z_WAITOK);
}

/*
 * Allocates and initializes a buffer of specified size for the reason. Also
 * initializes the kcdata descriptor accordingly. If there is an existing
 * buffer, we dealloc the buffer before allocating a new one and
 * clear the associated kcdata descriptor. If osr_bufsize is passed as 0,
 * we deallocate the existing buffer and then return.
 *
 * Returns:
 * 0 on success
 * EINVAL if the passed reason pointer is invalid or the requested size is
 *        larger than REASON_BUFFER_MAX_SIZE
 * ENOMEM if unable to allocate memory for the buffer
 * EIO if we fail to initialize the kcdata buffer
 */
int
os_reason_alloc_buffer_noblock(os_reason_t cur_reason, uint32_t osr_bufsize)
{
	return os_reason_alloc_buffer_internal(cur_reason, osr_bufsize, Z_NOWAIT);
}

static int
os_reason_alloc_buffer_internal(os_reason_t cur_reason, uint32_t osr_bufsize,
    zalloc_flags_t flags)
{
	if (cur_reason == OS_REASON_NULL) {
		return EINVAL;
	}

	if (osr_bufsize > OS_REASON_BUFFER_MAX_SIZE) {
		return EINVAL;
	}

	lck_mtx_lock(&cur_reason->osr_lock);

	os_reason_dealloc_buffer(cur_reason);

	if (osr_bufsize == 0) {
		lck_mtx_unlock(&cur_reason->osr_lock);
		return 0;
	}

	cur_reason->osr_kcd_buf = kheap_alloc_tag(KHEAP_DATA_BUFFERS, osr_bufsize,
	    flags | Z_ZERO, VM_KERN_MEMORY_REASON);

	if (cur_reason->osr_kcd_buf == NULL) {
		lck_mtx_unlock(&cur_reason->osr_lock);
		return ENOMEM;
	}

	cur_reason->osr_bufsize = osr_bufsize;

	if (kcdata_memory_static_init(&cur_reason->osr_kcd_descriptor,
	    (mach_vm_address_t)cur_reason->osr_kcd_buf,
	    KCDATA_BUFFER_BEGIN_OS_REASON, osr_bufsize, KCFLAG_USE_MEMCOPY) !=
	    KERN_SUCCESS) {
		os_reason_dealloc_buffer(cur_reason);

		lck_mtx_unlock(&cur_reason->osr_lock);
		return EIO;
	}

	lck_mtx_unlock(&cur_reason->osr_lock);

	return 0;
}

/*
 * Returns a pointer to the kcdata descriptor associated with the specified
 * reason if there is a buffer allocated.
 */
struct kcdata_descriptor *
os_reason_get_kcdata_descriptor(os_reason_t cur_reason)
{
	if (cur_reason == OS_REASON_NULL) {
		return NULL;
	}

	if (cur_reason->osr_kcd_buf == NULL) {
		return NULL;
	}

	assert(cur_reason->osr_kcd_descriptor.kcd_addr_begin ==
	    (mach_vm_address_t)cur_reason->osr_kcd_buf);
	if (cur_reason->osr_kcd_descriptor.kcd_addr_begin !=
	    (mach_vm_address_t)cur_reason->osr_kcd_buf) {
		return NULL;
	}

	return &cur_reason->osr_kcd_descriptor;
}

/*
 * Takes a reference on the passed reason.
 */
void
os_reason_ref(os_reason_t cur_reason)
{
	if (cur_reason == OS_REASON_NULL) {
		return;
	}

	lck_mtx_lock(&cur_reason->osr_lock);
	os_ref_retain_locked(&cur_reason->osr_refcount);
	lck_mtx_unlock(&cur_reason->osr_lock);
	return;
}

/*
 * Drops a reference on the passed reason, deallocates
 * the reason if no references remain.
 */
void
os_reason_free(os_reason_t cur_reason)
{
	if (cur_reason == OS_REASON_NULL) {
		return;
	}

	lck_mtx_lock(&cur_reason->osr_lock);

	if (os_ref_release_locked(&cur_reason->osr_refcount) > 0) {
		lck_mtx_unlock(&cur_reason->osr_lock);
		return;
	}

	os_reason_dealloc_buffer(cur_reason);

	lck_mtx_unlock(&cur_reason->osr_lock);
	lck_mtx_destroy(&cur_reason->osr_lock, &os_reason_lock_grp);

	zfree(os_reason_zone, cur_reason);
}

/*
 * Sets flags on the passed reason.
 */
void
os_reason_set_flags(os_reason_t cur_reason, uint64_t flags)
{
	if (cur_reason == OS_REASON_NULL) {
		return;
	}

	lck_mtx_lock(&cur_reason->osr_lock);
	cur_reason->osr_flags = flags;
	lck_mtx_unlock(&cur_reason->osr_lock);
}

/*
 * Allocates space and sets description data in kcd_descriptor on the passed reason.
 */
void
os_reason_set_description_data(os_reason_t cur_reason, uint32_t type, void *reason_data, uint32_t reason_data_len)
{
	mach_vm_address_t osr_data_addr = 0;

	if (cur_reason == OS_REASON_NULL) {
		return;
	}

	if (0 != os_reason_alloc_buffer(cur_reason, kcdata_estimate_required_buffer_size(1, reason_data_len))) {
		panic("os_reason failed to allocate");
	}

	lck_mtx_lock(&cur_reason->osr_lock);
	if (KERN_SUCCESS != kcdata_get_memory_addr(&cur_reason->osr_kcd_descriptor, type, reason_data_len, &osr_data_addr)) {
		panic("os_reason failed to get data address");
	}
	if (KERN_SUCCESS != kcdata_memcpy(&cur_reason->osr_kcd_descriptor, osr_data_addr, reason_data, reason_data_len)) {
		panic("os_reason failed to copy description data");
	}
	lck_mtx_unlock(&cur_reason->osr_lock);
}

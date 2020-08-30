/*
 * Copyright (c) 1999-2007 Apple Inc. All rights reserved.
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
/* Copyright (c) 1992 NeXT Computer, Inc.  All rights reserved. */

#include "SYS.h"

#if defined(__i386__)

	.globl	_errno

LEAF(___ptrace, 0)
	xorl	%eax,%eax
	REG_TO_EXTERN(%eax,_errno)
	UNIX_SYSCALL_NONAME(ptrace, 4, cerror)
	ret

#elif defined(__x86_64__)

	.globl	_errno

LEAF(___ptrace, 0)
	xorq	%rax,%rax
	PICIFY(_errno)
	movl	%eax,(%r11)
	UNIX_SYSCALL_NONAME(ptrace, 4, cerror)
	ret

#elif defined(__arm__)

MI_ENTRY_POINT(___ptrace)
	MI_GET_ADDRESS(ip,_errno)
	str	r8, [sp, #-4]!
	mov     r8, #0
	str     r8, [ip]
	ldr	r8, [sp], #4	
	SYSCALL_NONAME(ptrace, 4, cerror)
	bx		lr

#elif defined(__arm64__)

MI_ENTRY_POINT(___ptrace)
	MI_GET_ADDRESS(x9,_errno)
	str		wzr, [x9]
	SYSCALL_NONAME(ptrace, 4, cerror)
	ret
	
#else
#error Unsupported architecture
#endif

/*-
 * Copyright (c) 2000 Doug Rabson
 * All rights reserved.
 *
 * Portions Copyright (C) 2001 - 2007 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <sys/param.h>
#include <sys/queue.h>
#include <sys/malloc.h>
#include <sys/kernel.h>
#include <sys/errno.h>
#ifndef TEST
#include <sys/systm.h>
#endif
#include <sys/syslog.h>
#include <sys/smb_apple.h>
#include <netsmb/smb_subr.h>
#include <sys/kobj.h>

#ifdef TEST
#include "usertest.h"
#endif

MALLOC_DEFINE(M_KOBJ, "kobj", "Kernel object structures");

#ifdef KOBJ_STATS

#include <sys/sysctl.h>

int kobj_lookup_hits;
int kobj_lookup_misses;

SYSCTL_INT(_kern, OID_AUTO, kobj_hits, CTLFLAG_RD,
	   &kobj_lookup_hits, 0, "")
SYSCTL_INT(_kern, OID_AUTO, kobj_misses, CTLFLAG_RD,
	   &kobj_lookup_misses, 0, "")

#endif

static int kobj_next_id = 1;

static int
kobj_error_method(void)
{
	return ENXIO;
}

static void
kobj_register_method(struct kobjop_desc *desc)
{
	if (desc->id == 0)
		desc->id = kobj_next_id++;
}

static void
kobj_unregister_method(struct kobjop_desc *desc)
{
	#pragma unused(desc)
}

PRIVSYM void
kobj_class_compile(kobj_class_t cls)
{
	kobj_ops_t ops;
	kobj_method_t *m;
	int i;

	/*
	 * Don't do anything if we are already compiled.
	 */
	if (cls->ops)
		return;

	/*
	 * First register any methods which need it.
	 */
	for (i = 0, m = cls->methods; m->desc; i++, m++)
		kobj_register_method(m->desc);
	/*
	 * Then allocate the compiled op table.
	 */
	ops = malloc(sizeof(struct kobj_ops), M_KOBJ, M_WAITOK);

	bzero(ops, sizeof(struct kobj_ops));
	ops->cls = cls;
	cls->ops = ops;
}

PRIVSYM void
kobj_lookup_method(kobj_method_t *methods,
		   kobj_method_t *ce,
		   kobjop_desc_t desc)
{
	ce->desc = desc;
	for (; methods && methods->desc; methods++) {
		if (methods->desc == desc) {
			ce->func = methods->func;
			return;
		}
	}
	if (desc->deflt)
		ce->func = desc->deflt;
	else
		ce->func = kobj_error_method;
	return;
}

PRIVSYM void
kobj_class_free(kobj_class_t cls)
{
	int i;
	kobj_method_t *m;

	/*
	 * Unregister any methods which are no longer used.
	 */
	for (i = 0, m = cls->methods; m->desc; i++, m++)
		kobj_unregister_method(m->desc);

	/*
	 * Free memory and clean up.
	 */
	free(cls->ops, M_KOBJ);
	cls->ops = 0;
}

PRIVSYM kobj_t
kobj_create(kobj_class_t cls, int mtype)
{
	kobj_t obj;

	/*
	 * Allocate and initialise the new object.
	 */
	obj = malloc(cls->size, mtype, M_WAITOK);
	if (!obj)
		return 0;
	bzero(obj, cls->size);
	kobj_init(obj, cls);

	return obj;
}

PRIVSYM void
kobj_init(kobj_t obj, kobj_class_t cls)
{
	/*
	 * Consider compiling the class' method table.
	 */
	if (!cls->ops)
		kobj_class_compile(cls);

	obj->ops = cls->ops;
	cls->refs++;
}

PRIVSYM void
kobj_delete(kobj_t obj, int mtype)
{
	kobj_class_t cls = obj->ops->cls;

	/*
	 * Consider freeing the compiled method table for the class
	 * after its last instance is deleted. As an optimisation, we
	 * should defer this for a short while to avoid thrashing.
	 */
	cls->refs--;
	if (!cls->refs)
		kobj_class_free(cls);

	obj->ops = 0;
	if (mtype)
		free(obj, mtype);
}

/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#include "lint.h"
#include "thr_uberdata.h"

#define	MIN_MOD_SLOTS	8

/*
 * Used to inform libc_init() that we are on the primary link map,
 * and to cause certain functions (like malloc() and sbrk()) to fail
 * (with ENOTSUP) when they are called on an alternate link map.
 */
int primary_link_map = 0;

#if defined(_LP64)
#define	ALIGN	16
#else
#define	ALIGN	8
#endif

/*
 * Grow the TLS module information array as necessary to include the
 * specified module-id.  tls_modinfo->tls_size must be a power of two.
 * Return a pointer to the (possibly reallocated) module information array.
 */
static TLS_modinfo *
tls_modinfo_alloc(tls_metadata_t *tlsm, ulong_t moduleid)
{
	tls_t *tls_modinfo = &tlsm->tls_modinfo;
	TLS_modinfo *modinfo;
	size_t mod_slots;

	if ((modinfo = tls_modinfo->tls_data) == NULL ||
	    tls_modinfo->tls_size <= moduleid) {
		if ((mod_slots = tls_modinfo->tls_size) == 0)
			mod_slots = MIN_MOD_SLOTS;
		while (mod_slots <= moduleid)
			mod_slots *= 2;
		modinfo = lmalloc(mod_slots * sizeof (TLS_modinfo));
		if (tls_modinfo->tls_data != NULL) {
			(void) memcpy(modinfo, tls_modinfo->tls_data,
			    tls_modinfo->tls_size * sizeof (TLS_modinfo));
			lfree(tls_modinfo->tls_data,
			    tls_modinfo->tls_size * sizeof (TLS_modinfo));
		}
		tls_modinfo->tls_data = modinfo;
		tls_modinfo->tls_size = mod_slots;
	}
	return (modinfo);
}

/*
 * This is called from the dynamic linker, before libc_init() is called,
 * to setup all of the TLS blocks that are available at process startup
 * and hence must be included as part of the static TLS block.
 * No locks are needed because we are single-threaded at this point.
 * We must be careful not to call any function that could possibly
 * invoke the dynamic linker.  That is, we must only call functions
 * that are wholly private to libc.
 */
void
__tls_static_mods(TLS_modinfo **tlslist, unsigned long statictlssize)
{
	ulwp_t *oldself = __curthread();
	tls_metadata_t *tlsm;
	TLS_modinfo **tlspp;
	TLS_modinfo *tlsp;
	TLS_modinfo *modinfo;
	caddr_t data;
	caddr_t data_end;
	int max_modid;

	primary_link_map = 1;		/* inform libc_init */
	if (statictlssize == 0)
		return;

	/*
	 * Retrieve whatever dynamic TLS metadata was generated by code
	 * running on alternate link maps prior to now (we must be running
	 * on the primary link map now since __tls_static_mods() is only
	 * called on the primary link map).
	 */
	tlsm = &__uberdata.tls_metadata;
	if (oldself != NULL) {
		(void) memcpy(tlsm,
		    &oldself->ul_uberdata->tls_metadata, sizeof (*tlsm));
		ASSERT(tlsm->static_tls.tls_data == NULL);
	}

	/*
	 * We call lmalloc() to allocate the template even though libc_init()
	 * has not yet been called.  lmalloc() must and does deal with this.
	 */
	ASSERT((statictlssize & (ALIGN - 1)) == 0);
	tlsm->static_tls.tls_data = data = lmalloc(statictlssize);
	data_end = data + statictlssize;
	tlsm->static_tls.tls_size = statictlssize;
	/*
	 * Initialize the static TLS template.
	 * We make no assumptions about the order in memory of the TLS
	 * modules we are processing, only that they fit within the
	 * total size we are given and that they are self-consistent.
	 * We do not assume any order for the moduleid's; we only assume
	 * that they are reasonably small integers.
	 */
	for (max_modid = 0, tlspp = tlslist; (tlsp = *tlspp) != NULL; tlspp++) {
		ASSERT(tlsp->tm_flags & TM_FLG_STATICTLS);
		ASSERT(tlsp->tm_stattlsoffset > 0);
		ASSERT(tlsp->tm_stattlsoffset <= statictlssize);
		ASSERT((tlsp->tm_stattlsoffset & (ALIGN - 1)) == 0);
		ASSERT(tlsp->tm_filesz <= tlsp->tm_memsz);
		ASSERT(tlsp->tm_memsz <= tlsp->tm_stattlsoffset);
		if (tlsp->tm_filesz)
			(void) memcpy(data_end-tlsp->tm_stattlsoffset,
			    tlsp->tm_tlsblock, tlsp->tm_filesz);
		if (max_modid < tlsp->tm_modid)
			max_modid = tlsp->tm_modid;
	}
	/*
	 * Record the static TLS_modinfo information.
	 */
	modinfo = tls_modinfo_alloc(tlsm, max_modid);
	for (tlspp = tlslist; (tlsp = *tlspp) != NULL; tlspp++)
		(void) memcpy(&modinfo[tlsp->tm_modid],
		    tlsp, sizeof (*tlsp));

	/*
	 * Copy the new tls_metadata back to the old, if any,
	 * since it will be copied up again in libc_init().
	 */
	if (oldself != NULL)
		(void) memcpy(&oldself->ul_uberdata->tls_metadata,
		    tlsm, sizeof (*tlsm));
}

/*
 * This is called from the dynamic linker for each module not included
 * in the static TLS mod list, after the module has been loaded but
 * before any of the module's init code has been executed.
 */
void
__tls_mod_add(TLS_modinfo *tlsp)
{
	tls_metadata_t *tlsm = &curthread->ul_uberdata->tls_metadata;
	ulong_t moduleid = tlsp->tm_modid;
	TLS_modinfo *modinfo;

	lmutex_lock(&tlsm->tls_lock);
	ASSERT(!(tlsp->tm_flags & TM_FLG_STATICTLS));
	ASSERT(tlsp->tm_filesz <= tlsp->tm_memsz);
	modinfo = tls_modinfo_alloc(tlsm, moduleid);
	(void) memcpy(&modinfo[moduleid], tlsp, sizeof (*tlsp));
	lmutex_unlock(&tlsm->tls_lock);
}

/*
 * Called for each module as it is unloaded from memory by dlclose().
 */
void
__tls_mod_remove(TLS_modinfo *tlsp)
{
	tls_metadata_t *tlsm = &curthread->ul_uberdata->tls_metadata;
	ulong_t moduleid = tlsp->tm_modid;
	TLS_modinfo *modinfo;

	lmutex_lock(&tlsm->tls_lock);
	ASSERT(tlsm->tls_modinfo.tls_data != NULL &&
	    moduleid < tlsm->tls_modinfo.tls_size);
	modinfo = tlsm->tls_modinfo.tls_data;
	(void) memset(&modinfo[moduleid], 0, sizeof (TLS_modinfo));
	lmutex_unlock(&tlsm->tls_lock);
}

extern	int	_preexec_exit_handlers();
extern	void	libc_init();

const Lc_interface tls_rtldinfo[] = {
	{ .ci_tag = CI_VERSION,	.ci_un.ci_val = CI_V_CURRENT },
	{ .ci_tag = CI_ATEXIT, .ci_un.ci_func = _preexec_exit_handlers },
	{ .ci_tag = CI_TLS_MODADD, .ci_un.ci_func = __tls_mod_add },
	{ .ci_tag = CI_TLS_MODREM, .ci_un.ci_func = __tls_mod_remove },
	{ .ci_tag = CI_TLS_STATMOD, .ci_un.ci_func = __tls_static_mods },
	{ .ci_tag = CI_THRINIT,	.ci_un.ci_func = libc_init },
	{ .ci_tag = CI_NULL, .ci_un.ci_func = NULL }
};

/*
 * Return the address of a TLS variable for the current thread.
 * Run the constructors for newly-allocated dynamic TLS.
 */
void *
slow_tls_get_addr(TLS_index *tls_index)
{
	ulwp_t *self = curthread;
	tls_metadata_t *tlsm = &self->ul_uberdata->tls_metadata;
	TLS_modinfo *tlsp;
	ulong_t moduleid;
	tls_t *tlsent;
	caddr_t	base;
	void (**initarray)(void);
	ulong_t arraycnt = 0;

	/*
	 * Defer signals until we have finished calling
	 * all of the constructors.
	 */
	sigoff(self);
	lmutex_lock(&tlsm->tls_lock);
	if ((moduleid = tls_index->ti_moduleid) < self->ul_ntlsent)
		tlsent = self->ul_tlsent;
	else {
		ASSERT(moduleid < tlsm->tls_modinfo.tls_size);
		tlsent = lmalloc(tlsm->tls_modinfo.tls_size * sizeof (tls_t));
		if (self->ul_tlsent != NULL) {
			(void) memcpy(tlsent, self->ul_tlsent,
			    self->ul_ntlsent * sizeof (tls_t));
			lfree(self->ul_tlsent,
			    self->ul_ntlsent * sizeof (tls_t));
		}
		self->ul_tlsent = tlsent;
		self->ul_ntlsent = tlsm->tls_modinfo.tls_size;
	}
	tlsent += moduleid;
	if ((base = tlsent->tls_data) == NULL) {
		tlsp = (TLS_modinfo *)tlsm->tls_modinfo.tls_data + moduleid;
		if (tlsp->tm_memsz == 0) {	/* dlclose()d module? */
			base = NULL;
		} else if (tlsp->tm_flags & TM_FLG_STATICTLS) {
			/* static TLS is already allocated/initialized */
			base = (caddr_t)self - tlsp->tm_stattlsoffset;
			tlsent->tls_data = base;
			tlsent->tls_size = 0;	/* don't lfree() this space */
		} else {
			/* allocate/initialize the dynamic TLS */
			base = lmalloc(tlsp->tm_memsz);
			if (tlsp->tm_filesz != 0)
				(void) memcpy(base, tlsp->tm_tlsblock,
				    tlsp->tm_filesz);
			tlsent->tls_data = base;
			tlsent->tls_size = tlsp->tm_memsz;
			/* remember the constructors */
			arraycnt = tlsp->tm_tlsinitarraycnt;
			initarray = tlsp->tm_tlsinitarray;
		}
	}
	lmutex_unlock(&tlsm->tls_lock);

	/*
	 * Call constructors, if any, in ascending order.
	 * We have to do this after dropping tls_lock because
	 * we have no idea what the constructors will do.
	 * At least we have signals deferred until they are done.
	 */
	if (arraycnt) {
		do {
			(**initarray++)();
		} while (--arraycnt != 0);
	}

	if (base == NULL)	/* kludge to get x86/x64 to boot */
		base = (caddr_t)self - 512;

	sigon(self);
	return (base + tls_index->ti_tlsoffset);
}

#ifdef	TLS_GET_ADDR_IS_WRITTEN_IN_ASSEMBLER
/*
 * For speed, we do not make reference to any static data in this function.
 * If necessary to do so, we do a tail call to slow_tls_get_addr().
 */
void *
__tls_get_addr(TLS_index *tls_index)
{
	ulwp_t *self = curthread;
	tls_t *tlsent = self->ul_tlsent;
	ulong_t moduleid;
	caddr_t	base;

	if ((moduleid = tls_index->ti_moduleid) < self->ul_ntlsent &&
	    (base = tlsent[moduleid].tls_data) != NULL)
		return (base + tls_index->ti_tlsoffset);

	return (slow_tls_get_addr(tls_index));
}
#endif	/* TLS_GET_ADDR_IS_WRITTEN_IN_ASSEMBLER */

/*
 * This is called by _thrp_setup() to initialize the thread's static TLS.
 * Constructors for initially allocated static TLS are called here.
 */
void
tls_setup()
{
	ulwp_t *self = curthread;
	tls_metadata_t *tlsm = &self->ul_uberdata->tls_metadata;
	TLS_modinfo *tlsp;
	long moduleid;
	ulong_t nmods;

	if (tlsm->static_tls.tls_size == 0)	/* no static TLS */
		return;

	/* static TLS initialization */
	(void) memcpy((caddr_t)self - tlsm->static_tls.tls_size,
	    tlsm->static_tls.tls_data, tlsm->static_tls.tls_size);

	/* call TLS constructors for the static TLS just initialized */
	lmutex_lock(&tlsm->tls_lock);
	nmods = tlsm->tls_modinfo.tls_size;
	for (moduleid = 0; moduleid < nmods; moduleid++) {
		/*
		 * Resume where we left off in the module array.
		 * tls_modinfo.tls_data may have changed since we
		 * dropped and reacquired tls_lock, but TLS modules
		 * retain their positions in the new array.
		 */
		tlsp = (TLS_modinfo *)tlsm->tls_modinfo.tls_data + moduleid;
		/*
		 * Call constructors for this module if there are any
		 * to be called and if it is part of the static TLS.
		 */
		if (tlsp->tm_tlsinitarraycnt != 0 &&
		    (tlsp->tm_flags & TM_FLG_STATICTLS)) {
			ulong_t arraycnt = tlsp->tm_tlsinitarraycnt;
			void (**initarray)(void) = tlsp->tm_tlsinitarray;

			/*
			 * Call the constructors in ascending order.
			 * We must drop tls_lock while doing this because
			 * we have no idea what the constructors will do.
			 */
			lmutex_unlock(&tlsm->tls_lock);
			do {
				(**initarray++)();
			} while (--arraycnt != 0);
			lmutex_lock(&tlsm->tls_lock);
		}
	}
	lmutex_unlock(&tlsm->tls_lock);
}

/*
 * This is called by _thrp_exit() to deallocate the thread's TLS.
 * Destructors for all allocated TLS are called here.
 */
void
tls_exit()
{
	ulwp_t *self = curthread;
	tls_metadata_t *tlsm = &self->ul_uberdata->tls_metadata;
	tls_t *tlsent;
	TLS_modinfo *tlsp;
	long moduleid;
	ulong_t nmods;

	if (tlsm->static_tls.tls_size == 0 && self->ul_ntlsent == 0)
		return;		/* no TLS */

	/*
	 * Call TLS destructors for all TLS allocated for this thread.
	 */
	lmutex_lock(&tlsm->tls_lock);
	nmods = tlsm->tls_modinfo.tls_size;
	for (moduleid = nmods - 1; moduleid >= 0; --moduleid) {
		/*
		 * Resume where we left off in the module array.
		 * tls_modinfo.tls_data may have changed since we
		 * dropped and reacquired tls_lock, but TLS modules
		 * retain their positions in the new array.
		 */
		tlsp = (TLS_modinfo *)tlsm->tls_modinfo.tls_data + moduleid;
		/*
		 * Call destructors for this module if there are any
		 * to be called and if it is part of the static TLS or
		 * if the dynamic TLS for the module has been allocated.
		 */
		if (tlsp->tm_tlsfiniarraycnt != 0 &&
		    ((tlsp->tm_flags & TM_FLG_STATICTLS) ||
		    (moduleid < self->ul_ntlsent &&
		    (tlsent = self->ul_tlsent) != NULL &&
		    tlsent[moduleid].tls_data != NULL))) {
			ulong_t arraycnt = tlsp->tm_tlsfiniarraycnt;
			void (**finiarray)(void) = tlsp->tm_tlsfiniarray;

			/*
			 * Call the destructors in descending order.
			 * We must drop tls_lock while doing this because
			 * we have no idea what the destructors will do.
			 */
			lmutex_unlock(&tlsm->tls_lock);
			finiarray += arraycnt;
			do {
				(**--finiarray)();
			} while (--arraycnt != 0);
			lmutex_lock(&tlsm->tls_lock);
		}
	}
	lmutex_unlock(&tlsm->tls_lock);

	tls_free(self);
}

/*
 * We only free the dynamically allocated TLS; the statically
 * allocated TLS is reused when the ulwp_t is reallocated.
 */
void
tls_free(ulwp_t *ulwp)
{
	ulong_t moduleid;
	tls_t *tlsent;
	size_t ntlsent;
	void *base;
	size_t size;

	if ((tlsent = ulwp->ul_tlsent) == NULL ||
	    (ntlsent = ulwp->ul_ntlsent) == 0)
		return;

	for (moduleid = 0; moduleid < ntlsent; moduleid++, tlsent++) {
		if ((base = tlsent->tls_data) != NULL &&
		    (size = tlsent->tls_size) != 0)
			lfree(base, size);
		tlsent->tls_data = NULL;	/* paranoia */
		tlsent->tls_size = 0;
	}
	lfree(ulwp->ul_tlsent, ntlsent * sizeof (tls_t));
	ulwp->ul_tlsent = NULL;
	ulwp->ul_ntlsent = 0;
}

/*
 * Copyright (c) 2000-2001 Boris Popov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *    This product includes software developed by Boris Popov.
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
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
 * $Id: smbfs_smb.c,v 1.32 2003/09/21 20:53:11 lindak Exp $
 */
#include <stdint.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/lock.h>
#include <sys/vnode.h>
#include <sys/mbuf.h>
#include <sys/mount.h>

#ifdef USE_MD5_HASH
#include <sys/md5.h>
#endif

#ifdef APPLE
#include <sys/smb_apple.h>
#endif
#include <netsmb/smb.h>
#include <netsmb/smb_subr.h>
#include <netsmb/smb_rq.h>
#include <netsmb/smb_conn.h>

#include <fs/smbfs/smbfs.h>
#include <fs/smbfs/smbfs_node.h>
#include <fs/smbfs/smbfs_subr.h>

/*
 * Lack of inode numbers leads us to the problem of generating them.
 * Partially this problem can be solved by having a dir/file cache
 * with inode numbers generated from the incremented by one counter.
 * However this way will require too much kernel memory, gives all
 * sorts of locking and consistency problems, not to mentinon counter overflows.
 * So, I'm decided to use a hash function to generate pseudo random (and unique)
 * inode numbers.
 */
static long
smbfs_getino(struct smbnode *dnp, const char *name, int nmlen)
{
#ifdef USE_MD5_HASH
	MD5_CTX md5;
	u_int32_t state[4];
	long ino;
	int i;

	MD5Init(&md5);
	MD5Update(&md5, name, nmlen);
	MD5Final((u_char *)state, &md5);
	for (i = 0, ino = 0; i < 4; i++)
		ino += state[i];
	return dnp->n_ino + ino;
#endif
	u_int32_t ino;

	ino = dnp->n_ino + smbfs_hash(name, nmlen);
	if (ino <= 2)
		ino += 3;
	return ino;
}

static int
smbfs_smb_lockandx(struct smbnode *np, int op, u_int32_t pid,
	off_t start, u_int64_t len, int largelock,
	struct smb_cred *scred, u_int32_t timeout)
{
	struct smb_share *ssp = np->n_mount->sm_share;
	struct smb_rq rq, *rqp = &rq;
	struct mbchain *mbp;
	u_char ltype = 0;
	int error;

	if (op == SMB_LOCK_SHARED)
		ltype |= SMB_LOCKING_ANDX_SHARED_LOCK;
	if (largelock)
		ltype |= SMB_LOCKING_ANDX_LARGE_FILES;
	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_LOCKING_ANDX, scred);
	if (error)
		return error;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	mb_put_uint8(mbp, 0xff);	/* secondary command */
	mb_put_uint8(mbp, 0);		/* MBZ */
	mb_put_uint16le(mbp, 0);
	mb_put_mem(mbp, (caddr_t)&np->n_fid, 2, MB_MSYSTEM);
	mb_put_uint8(mbp, ltype);	/* locktype */
	mb_put_uint8(mbp, 0);		/* oplocklevel - 0 seems is NO_OPLOCK */
	mb_put_uint32le(mbp, timeout);	/* 0 nowait, -1 infinite wait */
	mb_put_uint16le(mbp, op == SMB_LOCK_RELEASE ? 1 : 0);
	mb_put_uint16le(mbp, op == SMB_LOCK_RELEASE ? 0 : 1);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	mb_put_uint16le(mbp, pid);
	if (!largelock) {
		mb_put_uint32le(mbp, start);
		mb_put_uint32le(mbp, len);
	} else {
		mb_put_uint16le(mbp, 0); /* pad */
		mb_put_uint32le(mbp, start >> 32); /* OffsetHigh */
		mb_put_uint32le(mbp, start & 0xffffffff); /* OffsetLow */
		mb_put_uint32le(mbp, len >> 32); /* LengthHigh */
		mb_put_uint32le(mbp, len & 0xffffffff); /* LengthLow */
	}
	smb_rq_bend(rqp);
	error = smb_rq_simple(rqp);
	smb_rq_done(rqp);
	return error;
}

int
smbfs_smb_lock(struct smbnode *np, int op, caddr_t id,
	off_t start, u_int64_t len,	int largelock,
	struct smb_cred *scred, u_int32_t timeout)
{
	struct smb_share *ssp = np->n_mount->sm_share;

	if (SMB_DIALECT(SSTOVC(ssp)) < SMB_DIALECT_LANMAN1_0)
		/*
		 * TODO: use LOCK_BYTE_RANGE here.
		 */
		return EINVAL;
	else
		return smbfs_smb_lockandx(np, op, (u_int32_t)id, start, len,
			largelock, scred, timeout);
}

int
smbfs_smb_qpathinfo(struct smbnode *np, struct smbfattr *fap,
		    struct smb_cred *scred, short infolevel)
{
	struct smb_share *ssp = np->n_mount->sm_share;
	struct smb_vc *vcp = SSTOVC(ssp);
	struct smb_t2rq *t2p;
	int error, svtz, timesok = 1;
	struct mbchain *mbp;
	struct mdchain *mdp;
	u_int16_t date, time, wattr;
	int64_t lint;
	u_int32_t size, dattr;

	error = smb_t2_alloc(SSTOCP(ssp), SMB_TRANS2_QUERY_PATH_INFORMATION,
			     scred, &t2p);
	if (error)
		return error;
	mbp = &t2p->t2_tparam;
	mb_init(mbp);
	if (!infolevel) {
		if (SMB_DIALECT(vcp) < SMB_DIALECT_NTLM0_12)
			infolevel = SMB_QUERY_FILE_STANDARD;
		else
			infolevel = SMB_QUERY_FILE_BASIC_INFO;
	}
	mb_put_uint16le(mbp, infolevel);
	mb_put_uint32le(mbp, 0);
	/* mb_put_uint8(mbp, SMB_DT_ASCII); specs are wrong */
	error = smbfs_fullpath(mbp, vcp, np, NULL, 0);
	if (error) {
		smb_t2_done(t2p);
		return error;
	}
	t2p->t2_maxpcount = 2;
	t2p->t2_maxdcount = vcp->vc_txmax;
	error = smb_t2_request(t2p);
	if (error) {
		smb_t2_done(t2p);
		if (infolevel == SMB_QUERY_FILE_STANDARD || error != EINVAL)
			return error;
		return smbfs_smb_qpathinfo(np, fap, scred,
					   SMB_QUERY_FILE_STANDARD);
	}
	mdp = &t2p->t2_rdata;
	svtz = vcp->vc_sopt.sv_tz;
	switch (infolevel) {
	    case SMB_QUERY_FILE_STANDARD:
		timesok = 0;
		md_get_uint16le(mdp, NULL);
		md_get_uint16le(mdp, NULL);	/* creation time */
		md_get_uint16le(mdp, &date);
		md_get_uint16le(mdp, &time);	/* access time */
		if (date || time) {
			timesok++;
			smb_dos2unixtime(date, time, 0, svtz, &fap->fa_atime);
		}
		md_get_uint16le(mdp, &date);
		md_get_uint16le(mdp, &time);	/* modify time */
		if (date || time) {
			timesok++;
			smb_dos2unixtime(date, time, 0, svtz, &fap->fa_mtime);
		}
		md_get_uint32le(mdp, &size);
		fap->fa_size = size;
		md_get_uint32(mdp, NULL);	/* allocation size */
		md_get_uint16le(mdp, &wattr);
		fap->fa_attr = wattr;
		break;
	    case SMB_QUERY_FILE_BASIC_INFO:
		timesok = 0;
		md_get_int64(mdp, NULL);	/* creation time */
		md_get_int64le(mdp, &lint);
		if (lint) {
			timesok++;
			smb_time_NT2local(lint, svtz, &fap->fa_atime);
		}
		md_get_int64le(mdp, &lint);
		if (lint) {
			timesok++;
			smb_time_NT2local(lint, svtz, &fap->fa_mtime);
		}
		md_get_int64le(mdp, &lint);
		if (lint) {
			timesok++;
			smb_time_NT2local(lint, svtz, &fap->fa_ctime);
		}
		md_get_uint32le(mdp, &dattr);
		fap->fa_attr = dattr;
		/* 4 byte pad may or may not be here (specs and servers vary) */
		/* XXX could use ALL_INFO to get size */
		break;
	    default:
		SMBERROR("unexpected info level %d\n", infolevel);
		error = EINVAL;
	}
	smb_t2_done(t2p);
	/*
	 * if all times are zero (observed with FAT on NT4SP6)
	 * then fall back to older info level
	 */
	if (!timesok) {
		if (infolevel != SMB_QUERY_FILE_STANDARD)
			return smbfs_smb_qpathinfo(np, fap, scred,
						   SMB_QUERY_FILE_STANDARD);
		error = EINVAL;
	}
	return error;
}


int
smbfs_smb_statfs2(struct smb_share *ssp, struct statfs *sbp,
	struct smb_cred *scred)
{
	struct smb_t2rq *t2p;
	struct mbchain *mbp;
	struct mdchain *mdp;
	u_int16_t bsize;
	u_int32_t units, bpu, funits;
	int error;

	error = smb_t2_alloc(SSTOCP(ssp), SMB_TRANS2_QUERY_FS_INFORMATION,
	    scred, &t2p);
	if (error)
		return error;
	mbp = &t2p->t2_tparam;
	mb_init(mbp);
	mb_put_uint16le(mbp, SMB_INFO_ALLOCATION);
	t2p->t2_maxpcount = 4;
	t2p->t2_maxdcount = 4 * 4 + 2;
	error = smb_t2_request(t2p);
	if (error) {
		smb_t2_done(t2p);
		return error;
	}
	mdp = &t2p->t2_rdata;
	md_get_uint32(mdp, NULL);	/* fs id */
	md_get_uint32le(mdp, &bpu);
	md_get_uint32le(mdp, &units);
	md_get_uint32le(mdp, &funits);
	md_get_uint16le(mdp, &bsize);
	sbp->f_bsize = bpu * bsize;	/* fundamental file system block size */
	sbp->f_blocks= units;		/* total data blocks in file system */
	sbp->f_bfree = funits;		/* free blocks in fs */
	sbp->f_bavail= funits;		/* free blocks avail to non-superuser */
	sbp->f_files = 0xffff;		/* total file nodes in file system */
	sbp->f_ffree = 0xffff;		/* free file nodes in fs */
	smb_t2_done(t2p);
	return 0;
}

int
smbfs_smb_statfs(struct smb_share *ssp, struct statfs *sbp,
	struct smb_cred *scred)
{
	struct smb_rq rq, *rqp = &rq;
	struct mdchain *mdp;
	u_int16_t units, bpu, bsize, funits;
	int error;

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_QUERY_INFORMATION_DISK, scred);
	if (error)
		return error;
	smb_rq_wstart(rqp);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	smb_rq_bend(rqp);
	error = smb_rq_simple(rqp);
	if (error) {
		smb_rq_done(rqp);
		return error;
	}
	smb_rq_getreply(rqp, &mdp);
	md_get_uint16le(mdp, &units);
	md_get_uint16le(mdp, &bpu);
	md_get_uint16le(mdp, &bsize);
	md_get_uint16le(mdp, &funits);
	sbp->f_bsize = bpu * bsize;	/* fundamental file system block size */
	sbp->f_blocks= units;		/* total data blocks in file system */
	sbp->f_bfree = funits;		/* free blocks in fs */
	sbp->f_bavail= funits;		/* free blocks avail to non-superuser */
	sbp->f_files = 0xffff;		/* total file nodes in file system */
	sbp->f_ffree = 0xffff;		/* free file nodes in fs */
	smb_rq_done(rqp);
	return 0;
}

int
smbfs_smb_seteof(struct smbnode *np, u_int64_t newsize, struct smb_cred *scred)
{
	struct smb_t2rq *t2p;
	struct smb_share *ssp = np->n_mount->sm_share;
	struct mbchain *mbp;
	int error;

	error = smb_t2_alloc(SSTOCP(ssp), SMB_TRANS2_SET_FILE_INFORMATION,
	    scred, &t2p);
	if (error)
		return error;
	mbp = &t2p->t2_tparam;
	mb_init(mbp);
	/*
	 * Make sure we have it open for writing on the server;
	 * that should be the case, but we do this as a bug check.
	 */
	if ((np->n_rwstate & SMB_AM_OPENMODE) != SMB_AM_OPENRW)
		SMBERROR("smbfs_smb_seteof on read-only FID\n");
	mb_put_mem(mbp, (caddr_t)&np->n_fid, 2, MB_MSYSTEM);
	mb_put_uint16le(mbp, SMB_SET_FILE_END_OF_FILE_INFO);
	mb_put_uint32le(mbp, 0);
	mbp = &t2p->t2_tdata;
	mb_init(mbp);
	mb_put_int64le(mbp, newsize);
	mb_put_uint32le(mbp, 0);			/* padding */
	mb_put_uint16le(mbp, 0);
	t2p->t2_maxpcount = 2;
	t2p->t2_maxdcount = 0;
	error = smb_t2_request(t2p);
	smb_t2_done(t2p);
	return error;
}

#ifdef APPLE
int
smb_smb_flush(struct smbnode *np, struct smb_cred *scred)
{
	struct smb_share *ssp = np->n_mount->sm_share;
	struct smb_rq rq, *rqp = &rq;
	struct mbchain *mbp;
	int error;

	if (np->n_opencount <= 0 || !SMBTOV(np) || SMBTOV(np)->v_type != VREG)
		return 0; /* not an regular open file */
	/*
	 * Make sure we have it open for writing on the server;
	 * that should be the case, but we do this as a bug check.
	 */
	if ((np->n_rwstate & SMB_AM_OPENMODE) != SMB_AM_OPENRW)
		SMBERROR("smb_smb_flush on read-only FID\n");
	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_FLUSH, scred);
	if (error)
		return (error);
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	mb_put_mem(mbp, (caddr_t)&np->n_fid, 2, MB_MSYSTEM);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	smb_rq_bend(rqp);
	error = smb_rq_simple(rqp);
	smb_rq_done(rqp);
	if (!error)
		np->n_flag &= ~NFLUSHWIRE;
	return (error);
}


int
smbfs_smb_flush(struct smbnode *np, struct smb_cred *scred)
{
	if (np->n_flag & NFLUSHWIRE)
		return (smb_smb_flush(np, scred));
	return (0);
}
#endif /* APPLE */

int
smbfs_smb_setfsize(struct smbnode *np, u_int64_t newsize,
		   struct smb_cred *scred)
{
	struct smb_share *ssp = np->n_mount->sm_share;
	struct smb_rq rq, *rqp = &rq;
	struct mbchain *mbp;
	int error;

	if (!smbfs_smb_seteof(np, newsize, scred)) {
		np->n_flag |= (NFLUSHWIRE | NATTRCHANGED);
		return (0);
	}
	if (newsize > UINT32_MAX)
		return (EFBIG);

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_WRITE, scred);
	if (error)
		return error;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	/*
	 * Make sure we have it open for writing on the server;
	 * that should be the case, but we do this as a bug check.
	 */
	if ((np->n_rwstate & SMB_AM_OPENMODE) != SMB_AM_OPENRW)
		SMBERROR("smb_smb_setfsize on read-only FID\n");
	mb_put_mem(mbp, (caddr_t)&np->n_fid, 2, MB_MSYSTEM);
	mb_put_uint16le(mbp, 0);
	mb_put_uint32le(mbp, newsize);
	mb_put_uint16le(mbp, 0);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	mb_put_uint8(mbp, SMB_DT_DATA);
	mb_put_uint16le(mbp, 0);
	smb_rq_bend(rqp);
	error = smb_rq_simple(rqp);
	smb_rq_done(rqp);
	np->n_flag |= (NFLUSHWIRE | NATTRCHANGED);
	return error;
}


int
smbfs_smb_query_info(struct smbnode *np, const char *name, int len,
		     struct smbfattr *fap, struct smb_cred *scred)
{
	struct smb_rq rq, *rqp = &rq;
	struct smb_share *ssp = np->n_mount->sm_share;
	struct mbchain *mbp;
	struct mdchain *mdp;
	u_int8_t wc;
	int error;
	u_int16_t wattr;
	u_int32_t lint;

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_QUERY_INFORMATION, scred);
	if (error)
		return error;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	mb_put_uint8(mbp, SMB_DT_ASCII);
	do {
		error = smbfs_fullpath(mbp, SSTOVC(ssp), np, name, len);
		if (error)
			break;
		smb_rq_bend(rqp);
		error = smb_rq_simple(rqp);
		if (error)
			break;
		smb_rq_getreply(rqp, &mdp);
		if (md_get_uint8(mdp, &wc) != 0 || wc != 10) {
			error = EBADRPC;
			break;
		}
		md_get_uint16le(mdp, &wattr);
		fap->fa_attr = wattr;
		/*
		 * Be careful using the time returned here, as
		 * with FAT on NT4SP6, at least, the time returned is low
		 * 32 bits of 100s of nanoseconds (since 1601) so it rolls
		 * over about every seven minutes!
		 */
		md_get_uint32le(mdp, &lint); /* specs: secs since 1970 */
		if (lint)	/* avoid bogus zero returns */
			smb_time_server2local(lint, SSTOVC(ssp)->vc_sopt.sv_tz,
					      &fap->fa_mtime);
		md_get_uint32le(mdp, &lint);
		fap->fa_size = lint;
	} while(0);
	smb_rq_done(rqp);
	return error;
}

/*
 * Set DOS file attributes. mtime should be NULL for dialects above lm10
 */
int
smbfs_smb_setpattr(struct smbnode *np, const char *name, int len,
		   u_int16_t attr, struct timespec *mtime,
		   struct smb_cred *scred)
{
	struct smb_rq rq, *rqp = &rq;
	struct smb_share *ssp = np->n_mount->sm_share;
	struct mbchain *mbp;
	u_long time;
	int error, svtz;

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_SET_INFORMATION, scred);
	if (error)
		return error;
	svtz = SSTOVC(ssp)->vc_sopt.sv_tz;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	mb_put_uint16le(mbp, attr);
	if (mtime) {
		smb_time_local2server(mtime, svtz, &time);
	} else
		time = 0;
	mb_put_uint32le(mbp, time);		/* mtime */
	mb_put_mem(mbp, NULL, 5 * 2, MB_MZERO);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	mb_put_uint8(mbp, SMB_DT_ASCII);
	do {
		error = smbfs_fullpath(mbp, SSTOVC(ssp), np, name, len);
		if (error)
			break;
		mb_put_uint8(mbp, SMB_DT_ASCII);
#ifdef APPLE
		if (SMB_UNICODE_STRINGS(SSTOVC(ssp))) {
			mb_put_padbyte(mbp);
			mb_put_uint8(mbp, 0);	/* 1st byte of NULL Unicode char */
		}
#endif
		mb_put_uint8(mbp, 0);
		smb_rq_bend(rqp);
		error = smb_rq_simple(rqp);
		if (error)
			break;
	} while(0);
	smb_rq_done(rqp);
	return error;
}

#ifdef APPLE
int
smbfs_smb_hideit(struct smbnode *np, const char *name, int len,
		 struct smb_cred *scred)
{
	struct smbfattr fa;
	int error;
	u_int16_t attr;

	error = smbfs_smb_query_info(np, name, len, &fa, scred);
	attr = fa.fa_attr;
	if (!error && !(attr & SMB_FA_HIDDEN)) {
		attr |= SMB_FA_HIDDEN;
		error = smbfs_smb_setpattr(np, name, len, attr, NULL, scred);
	}
	return (error);
}


int
smbfs_smb_unhideit(struct smbnode *np, const char *name, int len,
		   struct smb_cred *scred)
{
	struct smbfattr fa;
	u_int16_t attr;
	int error;

	error = smbfs_smb_query_info(np, name, len, &fa, scred);
	attr = fa.fa_attr;
	if (!error && (attr & SMB_FA_HIDDEN)) {
		attr &= ~SMB_FA_HIDDEN;
		error = smbfs_smb_setpattr(np, name, len, attr, NULL, scred);
	}
	return (error);
}
#endif /* APPLE */

/*
 * Note, win95 doesn't support this call.
 */
int
smbfs_smb_setptime2(struct smbnode *np, struct timespec *mtime,
	struct timespec *atime, int attr, struct smb_cred *scred)
{
	struct smb_t2rq *t2p;
	struct smb_share *ssp = np->n_mount->sm_share;
	struct smb_vc *vcp = SSTOVC(ssp);
	struct mbchain *mbp;
	u_int16_t date, time;
	int error, tzoff;

	error = smb_t2_alloc(SSTOCP(ssp), SMB_TRANS2_SET_PATH_INFORMATION,
	    scred, &t2p);
	if (error)
		return error;
	mbp = &t2p->t2_tparam;
	mb_init(mbp);
	mb_put_uint16le(mbp, SMB_INFO_STANDARD);
	mb_put_uint32le(mbp, 0);		/* MBZ */
	/* mb_put_uint8(mbp, SMB_DT_ASCII); specs incorrect */
	error = smbfs_fullpath(mbp, vcp, np, NULL, 0);
	if (error) {
		smb_t2_done(t2p);
		return error;
	}
	tzoff = vcp->vc_sopt.sv_tz;
	mbp = &t2p->t2_tdata;
	mb_init(mbp);
	mb_put_uint32le(mbp, 0);		/* creation time */
	if (atime)
		smb_time_unix2dos(atime, tzoff, &date, &time, NULL);
	else
		time = date = 0;
	mb_put_uint16le(mbp, date);
	mb_put_uint16le(mbp, time);
	if (mtime)
		smb_time_unix2dos(mtime, tzoff, &date, &time, NULL);
	else
		time = date = 0;
	mb_put_uint16le(mbp, date);
	mb_put_uint16le(mbp, time);
	mb_put_uint32le(mbp, 0);		/* file size */
	mb_put_uint32le(mbp, 0);		/* allocation unit size */
	mb_put_uint16le(mbp, attr);	/* DOS attr */
	mb_put_uint32le(mbp, 0);		/* EA size */
	t2p->t2_maxpcount = 5 * 2;
	t2p->t2_maxdcount = vcp->vc_txmax;
	error = smb_t2_request(t2p);
	smb_t2_done(t2p);
	return error;
}

/*
 * NT level. Specially for win9x
 */
int
smbfs_smb_setpattrNT(struct smbnode *np, u_short attr, struct timespec *mtime,
	struct timespec *atime, struct smb_cred *scred)
{
	struct smb_t2rq *t2p;
	struct smb_share *ssp = np->n_mount->sm_share;
	struct smb_vc *vcp = SSTOVC(ssp);
	struct mbchain *mbp;
	int64_t tm;
	int error, tzoff;

	error = smb_t2_alloc(SSTOCP(ssp), SMB_TRANS2_SET_PATH_INFORMATION,
	    scred, &t2p);
	if (error)
		return error;
	mbp = &t2p->t2_tparam;
	mb_init(mbp);
	mb_put_uint16le(mbp, SMB_SET_FILE_BASIC_INFO);
	mb_put_uint32le(mbp, 0);		/* MBZ */
	/* mb_put_uint8(mbp, SMB_DT_ASCII); specs incorrect */
	error = smbfs_fullpath(mbp, vcp, np, NULL, 0);
	if (error) {
		smb_t2_done(t2p);
		return error;
	}
	tzoff = vcp->vc_sopt.sv_tz;
	mbp = &t2p->t2_tdata;
	mb_init(mbp);
	mb_put_int64le(mbp, 0);		/* creation time */
	if (atime) {
		smb_time_local2NT(atime, tzoff, &tm);
	} else
		tm = 0;
	mb_put_int64le(mbp, tm);
	if (mtime) {
		smb_time_local2NT(mtime, tzoff, &tm);
	} else
		tm = 0;
	mb_put_int64le(mbp, tm);
	mb_put_int64le(mbp, tm);		/* change time */
	mb_put_uint32le(mbp, attr);		/* attr */
	t2p->t2_maxpcount = 24;
	t2p->t2_maxdcount = 56;
	error = smb_t2_request(t2p);
	smb_t2_done(t2p);
	return error;
}

/*
 * Set file atime and mtime. Doesn't supported by core dialect.
 */
int
smbfs_smb_setftime(struct smbnode *np, struct timespec *mtime,
	struct timespec *atime, struct smb_cred *scred)
{
	struct smb_rq rq, *rqp = &rq;
	struct smb_share *ssp = np->n_mount->sm_share;
	struct mbchain *mbp;
	u_int16_t date, time;
	int error, tzoff;

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_SET_INFORMATION2, scred);
	if (error)
		return error;
	tzoff = SSTOVC(ssp)->vc_sopt.sv_tz;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	/*
	 * Make sure we have it open for writing on the server;
	 * that should be the case, but we do this as a bug check.
	 */
	if ((np->n_rwstate & SMB_AM_OPENMODE) != SMB_AM_OPENRW)
		SMBERROR("smbfs_smb_setftime on read-only FID\n");
	mb_put_mem(mbp, (caddr_t)&np->n_fid, 2, MB_MSYSTEM);
	mb_put_uint32le(mbp, 0);		/* creation time */

	if (atime)
		smb_time_unix2dos(atime, tzoff, &date, &time, NULL);
	else
		time = date = 0;
	mb_put_uint16le(mbp, date);
	mb_put_uint16le(mbp, time);
	if (mtime)
		smb_time_unix2dos(mtime, tzoff, &date, &time, NULL);
	else
		time = date = 0;
	mb_put_uint16le(mbp, date);
	mb_put_uint16le(mbp, time);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	smb_rq_bend(rqp);
	error = smb_rq_simple(rqp);
	SMBSDEBUG("%d\n", error);
	smb_rq_done(rqp);
	return error;
}

/*
 * Set DOS file attributes.
 * Looks like this call can be used only if CAP_NT_SMBS bit is on.
 */
int
smbfs_smb_setfattrNT(struct smbnode *np, u_int16_t attr, struct timespec *mtime,
	struct timespec *atime, struct smb_cred *scred)
{
	struct smb_t2rq *t2p;
	struct smb_share *ssp = np->n_mount->sm_share;
	struct mbchain *mbp;
	int64_t tm;
	int error, svtz;

	error = smb_t2_alloc(SSTOCP(ssp), SMB_TRANS2_SET_FILE_INFORMATION,
	    scred, &t2p);
	if (error)
		return error;
	svtz = SSTOVC(ssp)->vc_sopt.sv_tz;
	mbp = &t2p->t2_tparam;
	mb_init(mbp);
	/*
	 * Make sure we have it open for writing on the server;
	 * that should be the case, but we do this as a bug check.
	 */
	if ((np->n_rwstate & SMB_AM_OPENMODE) != SMB_AM_OPENRW)
		SMBERROR("smbfs_smb_setfattrNT on read-only FID\n");
	mb_put_mem(mbp, (caddr_t)&np->n_fid, 2, MB_MSYSTEM);
	mb_put_uint16le(mbp, SMB_SET_FILE_BASIC_INFO);
	mb_put_uint32le(mbp, 0);
	mbp = &t2p->t2_tdata;
	mb_init(mbp);
	mb_put_int64le(mbp, 0);		/* creation time */
	if (atime) {
		smb_time_local2NT(atime, svtz, &tm);
	} else
		tm = 0;
	mb_put_int64le(mbp, tm);
	if (mtime) {
		smb_time_local2NT(mtime, svtz, &tm);
	} else
		tm = 0;
	mb_put_int64le(mbp, tm);
	mb_put_int64le(mbp, tm);		/* change time */
	mb_put_uint16le(mbp, attr);
	mb_put_uint32le(mbp, 0);			/* padding */
	mb_put_uint16le(mbp, 0);
	t2p->t2_maxpcount = 2;
	t2p->t2_maxdcount = 0;
	error = smb_t2_request(t2p);
	smb_t2_done(t2p);
	return error;
}


int
smbfs_smb_open(struct smbnode *np, int accmode, struct smb_cred *scred, int *attrcacheupdated)
{
	struct smb_rq rq, *rqp = &rq;
	struct smb_share *ssp = np->n_mount->sm_share;
	struct mbchain *mbp;
	struct mdchain *mdp;
	struct smbfattr fap;
	struct vattr va;
	u_int8_t wc;
	u_int16_t fid, wattr, grantedmode;
	u_int32_t lint;
	int error;

	*attrcacheupdated = 0;
	getnanotime(&fap.fa_reqtime);
	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_OPEN, scred);
	if (error)
		return error;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	mb_put_uint16le(mbp, accmode);
	mb_put_uint16le(mbp, SMB_FA_SYSTEM | SMB_FA_HIDDEN);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	mb_put_uint8(mbp, SMB_DT_ASCII);
	do {
		error = smbfs_fullpath(mbp, SSTOVC(ssp), np, NULL, 0);
		if (error)
			break;
		smb_rq_bend(rqp);
		error = smb_rq_simple(rqp);
		if (error)
			break;
		smb_rq_getreply(rqp, &mdp);
		/*
		 * 8/2002 a DAVE server returned wc of 15 so we ignore that.
		 * (the actual packet length and data was correct)
		 */
		if (md_get_uint8(mdp, &wc) != 0 || (wc != 7 && wc != 15)) {
			error = EBADRPC;
			break;
		}
		md_get_uint16(mdp, &fid);
		md_get_uint16le(mdp, &wattr);
		fap.fa_attr = wattr;
		/*
		 * Be careful using the time returned here, as
		 * with FAT on NT4SP6, at least, the time returned is low
		 * 32 bits of 100s of nanoseconds (since 1601) so it rolls
		 * over about every seven minutes!
		 */
		md_get_uint32le(mdp, &lint); /* specs: secs since 1970 */
		if (lint)	/* avoid bogus zero returns */
			smb_time_server2local(lint, SSTOVC(ssp)->vc_sopt.sv_tz,
					      &fap.fa_mtime);
		md_get_uint32le(mdp, &lint);
		fap.fa_size = lint;
		md_get_uint16le(mdp, &grantedmode);
	} while(0);
	smb_rq_done(rqp);
	if (error)
		return error;
	np->n_fid = fid;
	np->n_rwstate = grantedmode;
	
	/*
	 * Update the cached attributes if they are still valid
	 * in the cache and if nothing has changed.
	 * Note that this won't ever update if the file size is
	 * greater than the 32-bits returned by SMB_COM_OPEN.
	 * For 64-bit file sizes, SMB_COM_NT_CREATE_ANDX will
	 * have to be used instead of SMB_COM_OPEN.
	 */
	if (smbfs_attr_cachelookup(np->n_vnode, &va) != 0)
		goto uncached;	/* the cached attributes are not valid */
	if (fap.fa_size != np->n_size)
		goto uncached;	/* the size is different */
	if (fap.fa_attr != np->n_dosattr)
		goto uncached;	/* the attrs are different */
	/*
	 * fap.fa_mtime is in two second increments while np->n_mtime
	 * may be in one second increments, so comparing the times is
	 * somewhat sloppy.
	 */
	if ((fap.fa_mtime.tv_sec != np->n_mtime.tv_sec) &&
		(fap.fa_mtime.tv_sec != (np->n_mtime.tv_sec - 1)) &&
		(fap.fa_mtime.tv_sec != (np->n_mtime.tv_sec + 1)))
		goto uncached;	/* the mod time is different */
	
	fap.fa_mtime.tv_sec = np->n_mtime.tv_sec; /* keep higher resolution time */
	smbfs_attr_cacheenter(np->n_vnode, &fap);
	*attrcacheupdated = 1;

uncached:

	return 0;
}


int
smbfs_smb_close(struct smb_share *ssp, u_int16_t fid, struct timespec *mtime,
	struct smb_cred *scred)
{
	struct smb_rq rq, *rqp = &rq;
	struct mbchain *mbp;
	u_long time;
	int error;

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_CLOSE, scred);
	if (error)
		return error;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	mb_put_mem(mbp, (caddr_t)&fid, sizeof(fid), MB_MSYSTEM);
	if (mtime) {
		smb_time_local2server(mtime, SSTOVC(ssp)->vc_sopt.sv_tz, &time);
	} else
		time = 0;
	mb_put_uint32le(mbp, time);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	smb_rq_bend(rqp);
	error = smb_rq_simple(rqp);
	smb_rq_done(rqp);
	return error;
}

int
smbfs_smb_create(struct smbnode *dnp, const char *name, int nmlen,
	struct smb_cred *scred)
{
	struct smb_rq rq, *rqp = &rq;
	struct smb_share *ssp = dnp->n_mount->sm_share;
	struct mbchain *mbp;
	struct mdchain *mdp;
	struct timespec ctime;
	u_int8_t wc;
	u_int16_t fid;
	u_long tm;
	int error;
#ifdef APPLE
	u_int16_t attr = SMB_FA_ARCHIVE;
#endif /* APPLE */

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_CREATE, scred);
	if (error)
		return error;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
#ifdef APPLE
	if (name && *name == '.')
		attr |= SMB_FA_HIDDEN;
	mb_put_uint16le(mbp, attr);		/* attributes  */
#else
	mb_put_uint16le(mbp, SMB_FA_ARCHIVE);		/* attributes  */
#endif /* APPLE */
	nanotime(&ctime);
	smb_time_local2server(&ctime, SSTOVC(ssp)->vc_sopt.sv_tz, &tm);
	mb_put_uint32le(mbp, tm);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	mb_put_uint8(mbp, SMB_DT_ASCII);
	error = smbfs_fullpath(mbp, SSTOVC(ssp), dnp, name, nmlen);
	if (!error) {
		smb_rq_bend(rqp);
		error = smb_rq_simple(rqp);
		if (!error) {
			smb_rq_getreply(rqp, &mdp);
			md_get_uint8(mdp, &wc);
			if (wc == 1)
				md_get_uint16(mdp, &fid);
			else
				error = EBADRPC;
		}
	}
	smb_rq_done(rqp);
	if (error)
		return error;
	smbfs_smb_close(ssp, fid, &ctime, scred);
	return error;
}

int
smbfs_smb_delete(struct smbnode *np, struct smb_cred *scred)
{
	struct smb_rq rq, *rqp = &rq;
	struct smb_share *ssp = np->n_mount->sm_share;
	struct mbchain *mbp;
	int error;

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_DELETE, scred);
	if (error)
		return error;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	mb_put_uint16le(mbp, SMB_FA_SYSTEM | SMB_FA_HIDDEN);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	mb_put_uint8(mbp, SMB_DT_ASCII);
	error = smbfs_fullpath(mbp, SSTOVC(ssp), np, NULL, 0);
	if (!error) {
		smb_rq_bend(rqp);
		error = smb_rq_simple(rqp);
	}
	smb_rq_done(rqp);
	return error;
}

int
smbfs_smb_rename(struct smbnode *src, struct smbnode *tdnp,
	const char *tname, int tnmlen, struct smb_cred *scred)
{
	struct smb_rq rq, *rqp = &rq;
	struct smb_share *ssp = src->n_mount->sm_share;
	struct mbchain *mbp;
	int error;

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_RENAME, scred);
	if (error)
		return error;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	/* freebsd bug: Let directories be renamed - Win98 requires DIR bit */
	mb_put_uint16le(mbp, ((SMBTOV(src)->v_type == VDIR) ? SMB_FA_DIR : 0) |
			     SMB_FA_SYSTEM | SMB_FA_HIDDEN);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	mb_put_uint8(mbp, SMB_DT_ASCII);
	do {
		error = smbfs_fullpath(mbp, SSTOVC(ssp), src, NULL, 0);
		if (error)
			break;
		mb_put_uint8(mbp, SMB_DT_ASCII);
		error = smbfs_fullpath(mbp, SSTOVC(ssp), tdnp, tname, tnmlen);
		if (error)
			break;
		smb_rq_bend(rqp);
		error = smb_rq_simple(rqp);
	} while(0);
	smb_rq_done(rqp);
	return error;
}

int
smbfs_smb_move(struct smbnode *src, struct smbnode *tdnp,
	const char *tname, int tnmlen, u_int16_t flags, struct smb_cred *scred)
{
	struct smb_rq rq, *rqp = &rq;
	struct smb_share *ssp = src->n_mount->sm_share;
	struct mbchain *mbp;
	int error;

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_MOVE, scred);
	if (error)
		return error;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	mb_put_uint16le(mbp, SMB_TID_UNKNOWN);
	mb_put_uint16le(mbp, 0x20);	/* delete target file */
	mb_put_uint16le(mbp, flags);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	mb_put_uint8(mbp, SMB_DT_ASCII);
	do {
		error = smbfs_fullpath(mbp, SSTOVC(ssp), src, NULL, 0);
		if (error)
			break;
		mb_put_uint8(mbp, SMB_DT_ASCII);
		error = smbfs_fullpath(mbp, SSTOVC(ssp), tdnp, tname, tnmlen);
		if (error)
			break;
		smb_rq_bend(rqp);
		error = smb_rq_simple(rqp);
	} while(0);
	smb_rq_done(rqp);
	return error;
}

int
smbfs_smb_mkdir(struct smbnode *dnp, const char *name, int len,
	struct smb_cred *scred)
{
	struct smb_rq rq, *rqp = &rq;
	struct smb_share *ssp = dnp->n_mount->sm_share;
	struct mbchain *mbp;
	int error;

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_CREATE_DIRECTORY, scred);
	if (error)
		return error;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	mb_put_uint8(mbp, SMB_DT_ASCII);
	error = smbfs_fullpath(mbp, SSTOVC(ssp), dnp, name, len);
	if (!error) {
		smb_rq_bend(rqp);
		error = smb_rq_simple(rqp);
	}
	smb_rq_done(rqp);
	return error;
}

int
smbfs_smb_rmdir(struct smbnode *np, struct smb_cred *scred)
{
	struct smb_rq rq, *rqp = &rq;
	struct smb_share *ssp = np->n_mount->sm_share;
	struct mbchain *mbp;
	int error;

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_DELETE_DIRECTORY, scred);
	if (error)
		return error;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	mb_put_uint8(mbp, SMB_DT_ASCII);
	error = smbfs_fullpath(mbp, SSTOVC(ssp), np, NULL, 0);
	if (!error) {
		smb_rq_bend(rqp);
		error = smb_rq_simple(rqp);
	}
	smb_rq_done(rqp);
	return error;
}

static int
smbfs_smb_search(struct smbfs_fctx *ctx)
{
	struct smb_vc *vcp = SSTOVC(ctx->f_ssp);
	struct smb_rq *rqp;
	struct mbchain *mbp;
	struct mdchain *mdp;
	u_int8_t wc, bt;
	u_int16_t ec, dlen, bc;
	int maxent, error, iseof = 0;

	maxent = min(ctx->f_left,
		     (vcp->vc_txmax - SMB_HDRLEN - 2*2) / SMB_DENTRYLEN);
	if (ctx->f_rq) {
		smb_rq_done(ctx->f_rq);
		ctx->f_rq = NULL;
	}
	error = smb_rq_alloc(SSTOCP(ctx->f_ssp), SMB_COM_SEARCH, ctx->f_scred, &rqp);
	if (error)
		return error;
	ctx->f_rq = rqp;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	mb_put_uint16le(mbp, maxent);	/* max entries to return */
	mb_put_uint16le(mbp, ctx->f_attrmask);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	mb_put_uint8(mbp, SMB_DT_ASCII);	/* buffer format */
	if (ctx->f_flags & SMBFS_RDD_FINDFIRST) {
		error = smbfs_fullpath(mbp, vcp, ctx->f_dnp, ctx->f_wildcard, ctx->f_wclen);
		if (error)
			return error;
		mb_put_uint8(mbp, SMB_DT_VARIABLE);
		mb_put_uint16le(mbp, 0);	/* context length */
		ctx->f_flags &= ~SMBFS_RDD_FINDFIRST;
	} else {
#ifdef APPLE
		if (SMB_UNICODE_STRINGS(vcp)) {
			mb_put_padbyte(mbp);
			mb_put_uint8(mbp, 0);
		}
#endif
		mb_put_uint8(mbp, 0);	/* file name length */
		mb_put_uint8(mbp, SMB_DT_VARIABLE);
		mb_put_uint16le(mbp, SMB_SKEYLEN);
		mb_put_mem(mbp, ctx->f_skey, SMB_SKEYLEN, MB_MSYSTEM);
	}
	smb_rq_bend(rqp);
	error = smb_rq_simple(rqp);
	if (rqp->sr_errclass == ERRDOS && rqp->sr_serror == ERRnofiles) {
		error = 0;
		iseof = 1;
		ctx->f_flags |= SMBFS_RDD_EOF;
	} else if (error)
		return error;
	smb_rq_getreply(rqp, &mdp);
	md_get_uint8(mdp, &wc);
	if (wc != 1) 
		return iseof ? ENOENT : EBADRPC;
	md_get_uint16le(mdp, &ec);
	if (ec == 0)
		return ENOENT;
	ctx->f_ecnt = ec;
	md_get_uint16le(mdp, &bc);
	if (bc < 3)
		return EBADRPC;
	bc -= 3;
	md_get_uint8(mdp, &bt);
	if (bt != SMB_DT_VARIABLE)
		return EBADRPC;
	md_get_uint16le(mdp, &dlen);
	if (dlen != bc || dlen % SMB_DENTRYLEN != 0)
		return EBADRPC;
	return 0;
}

static int
smbfs_findopenLM1(struct smbfs_fctx *ctx, struct smbnode *dnp,
	const char *wildcard, int wclen, int attr, struct smb_cred *scred)
{
	#pragma unused(dnp, scred)
	ctx->f_attrmask = attr;
	if (wildcard) {
		if (wclen == 1 && wildcard[0] == '*') {
			ctx->f_wildcard = "*.*";
			ctx->f_wclen = 3;
		} else {
			ctx->f_wildcard = wildcard;
			ctx->f_wclen = wclen;
		}
	} else {
		ctx->f_wildcard = NULL;
		ctx->f_wclen = 0;
	}
	ctx->f_name = ctx->f_fname;
	return 0;
}

static int
smbfs_findnextLM1(struct smbfs_fctx *ctx, int limit)
{
	struct mdchain *mdp;
	struct smb_rq *rqp;
	char *cp;
	u_int8_t battr;
	u_int16_t date, time;
	u_int32_t size;
	int error;
	struct timespec ts;

	if (ctx->f_ecnt == 0) {
		if (ctx->f_flags & SMBFS_RDD_EOF)
			return ENOENT;
		ctx->f_left = ctx->f_limit = limit;
		getnanotime(&ts);
		error = smbfs_smb_search(ctx);
		if (error)
			return error;
		ctx->f_attr.fa_reqtime = ts;
	}
	rqp = ctx->f_rq;
	smb_rq_getreply(rqp, &mdp);
	md_get_mem(mdp, ctx->f_skey, SMB_SKEYLEN, MB_MSYSTEM);
	md_get_uint8(mdp, &battr);
	md_get_uint16le(mdp, &time);
	md_get_uint16le(mdp, &date);
	md_get_uint32le(mdp, &size);
	cp = ctx->f_name;
	md_get_mem(mdp, cp, sizeof(ctx->f_fname), MB_MSYSTEM);
	cp[sizeof(ctx->f_fname) - 1] = 0;
	cp += strlen(cp) - 1;
	while (*cp == ' ' && cp >= ctx->f_name)
		*cp-- = 0;
	ctx->f_attr.fa_attr = battr;
	smb_dos2unixtime(date, time, 0, rqp->sr_vc->vc_sopt.sv_tz,
	    &ctx->f_attr.fa_mtime);
	ctx->f_attr.fa_size = size;
	ctx->f_nmlen = strlen(ctx->f_name);
	ctx->f_ecnt--;
	ctx->f_left--;
	return 0;
}

static int
smbfs_findcloseLM1(struct smbfs_fctx *ctx)
{
	if (ctx->f_rq)
		smb_rq_done(ctx->f_rq);
	return 0;
}

/*
 * TRANS2_FIND_FIRST2/NEXT2, used for NT LM12 dialect
 */
static int
smbfs_smb_trans2find2(struct smbfs_fctx *ctx)
{
	struct smb_t2rq *t2p;
	struct smb_vc *vcp = SSTOVC(ctx->f_ssp);
	struct mbchain *mbp;
	struct mdchain *mdp;
	u_int16_t tw, flags;
	int error;

	if (ctx->f_t2) {
		smb_t2_done(ctx->f_t2);
		ctx->f_t2 = NULL;
	}
	ctx->f_flags &= ~SMBFS_RDD_GOTRNAME;
	flags = 8 | 2;			/* <resume> | <close if EOS> */
	if (ctx->f_flags & SMBFS_RDD_FINDSINGLE) {
		flags |= 1;		/* close search after this request */
		ctx->f_flags |= SMBFS_RDD_NOCLOSE;
	}
	if (ctx->f_flags & SMBFS_RDD_FINDFIRST) {
		error = smb_t2_alloc(SSTOCP(ctx->f_ssp), SMB_TRANS2_FIND_FIRST2,
		    ctx->f_scred, &t2p);
		if (error)
			return error;
		ctx->f_t2 = t2p;
		mbp = &t2p->t2_tparam;
		mb_init(mbp);
		mb_put_uint16le(mbp, ctx->f_attrmask);
		mb_put_uint16le(mbp, ctx->f_limit);
		mb_put_uint16le(mbp, flags);
		mb_put_uint16le(mbp, ctx->f_infolevel);
		mb_put_uint32le(mbp, 0);
		/* mb_put_uint8(mbp, SMB_DT_ASCII); specs? hah! */
		error = smbfs_fullpath(mbp, vcp, ctx->f_dnp, ctx->f_wildcard, ctx->f_wclen);
		if (error)
			return error;
	} else	{
		error = smb_t2_alloc(SSTOCP(ctx->f_ssp), SMB_TRANS2_FIND_NEXT2,
		    ctx->f_scred, &t2p);
		if (error)
			return error;
		ctx->f_t2 = t2p;
		mbp = &t2p->t2_tparam;
		mb_init(mbp);
		mb_put_mem(mbp, (caddr_t)&ctx->f_Sid, 2, MB_MSYSTEM);
		mb_put_uint16le(mbp, ctx->f_limit);
		mb_put_uint16le(mbp, ctx->f_infolevel);
		mb_put_uint32le(mbp, 0);		/* resume key */
		mb_put_uint16le(mbp, flags);
		if (ctx->f_rname)
#ifdef APPLE
			mb_put_mem(mbp, ctx->f_rname, ctx->f_rnamelen + 1, MB_MSYSTEM);
#else
			mb_put_mem(mbp, ctx->f_rname, strlen(ctx->f_rname) + 1, MB_MSYSTEM);
#endif
		else
			mb_put_uint8(mbp, 0);	/* resume file name */
#if 0
		struct timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 200 * 1000;	/* 200ms */
		if (vcp->vc_flags & SMBC_WIN95) {
			/*
			 * some implementations suggests to sleep here
			 * for 200ms, due to the bug in the Win95.
			 * I've didn't notice any problem, but put code
			 * for it.
			 */
			 tsleep(&flags, PVFS, "fix95", tvtohz(&tv));
		}
#endif
	}
	t2p->t2_maxpcount = 5 * 2;
	t2p->t2_maxdcount = vcp->vc_txmax;
	error = smb_t2_request(t2p);
	if (error)
		return error;
	mdp = &t2p->t2_rparam;
	if (ctx->f_flags & SMBFS_RDD_FINDFIRST) {
		if ((error = md_get_uint16(mdp, &ctx->f_Sid)) != 0)
			return error;
		ctx->f_flags &= ~SMBFS_RDD_FINDFIRST;
	}
	if ((error = md_get_uint16le(mdp, &tw)) != 0)
		return error;
	ctx->f_ecnt = tw;
	if ((error = md_get_uint16le(mdp, &tw)) != 0)
		return error;
	if (tw)
		ctx->f_flags |= SMBFS_RDD_EOF | SMBFS_RDD_NOCLOSE;
	if ((error = md_get_uint16le(mdp, &tw)) != 0)
		return error;
	if ((error = md_get_uint16le(mdp, &tw)) != 0)
		return error;
	if (ctx->f_ecnt == 0)
		return ENOENT;
	ctx->f_rnameofs = tw;
	mdp = &t2p->t2_rdata;
	if (mdp->md_top == NULL) {
		printf("bug: ecnt = %d, but data is NULL (please report)\n", ctx->f_ecnt);
		return ENOENT;
	}
	if (mdp->md_top->m_len == 0) {
		printf("bug: ecnt = %d, but m_len = 0 and m_next = %p (please report)\n", ctx->f_ecnt,mbp->mb_top->m_next);
		return ENOENT;
	}
	ctx->f_eofs = 0;
	return 0;
}

static int
smbfs_smb_findclose2(struct smbfs_fctx *ctx)
{
	struct smb_rq rq, *rqp = &rq;
	struct mbchain *mbp;
	int error;

	error = smb_rq_init(rqp, SSTOCP(ctx->f_ssp), SMB_COM_FIND_CLOSE2, ctx->f_scred);
	if (error)
		return error;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	mb_put_mem(mbp, (caddr_t)&ctx->f_Sid, 2, MB_MSYSTEM);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	smb_rq_bend(rqp);
	error = smb_rq_simple(rqp);
	smb_rq_done(rqp);
	return error;
}

static int
smbfs_findopenLM2(struct smbfs_fctx *ctx, struct smbnode *dnp,
	const char *wildcard, int wclen, int attr, struct smb_cred *scred)
{
	#pragma unused(dnp, scred)
#ifdef APPLE
	if (SMB_UNICODE_STRINGS(SSTOVC(ctx->f_ssp))) {
		ctx->f_name = malloc(SMB_MAXFNAMELEN*2, M_SMBFSDATA, M_WAITOK);
	} else
#endif
	ctx->f_name = malloc(SMB_MAXFNAMELEN, M_SMBFSDATA, M_WAITOK);
	if (ctx->f_name == NULL)
		return ENOMEM;
	ctx->f_infolevel = SMB_DIALECT(SSTOVC(ctx->f_ssp)) < SMB_DIALECT_NTLM0_12 ?
	    SMB_INFO_STANDARD : SMB_FIND_FILE_DIRECTORY_INFO;
	ctx->f_attrmask = attr;
	ctx->f_wildcard = wildcard;
	ctx->f_wclen = wclen;
	return 0;
}

static int
smbfs_findnextLM2(struct smbfs_fctx *ctx, int limit)
{
	struct mdchain *mdp;
	struct smb_t2rq *t2p;
	char *cp;
	u_int8_t tb;
	u_int16_t date, time, wattr;
	u_int32_t size, next, dattr;
	int64_t lint;
	int error, svtz, cnt, fxsz, nmlen, recsz;
	struct timespec ts;

	if (ctx->f_ecnt == 0) {
		if (ctx->f_flags & SMBFS_RDD_EOF)
			return ENOENT;
		ctx->f_left = ctx->f_limit = limit;
		getnanotime(&ts);
		error = smbfs_smb_trans2find2(ctx);
		if (error)
			return error;
		ctx->f_attr.fa_reqtime = ts;
	}
	t2p = ctx->f_t2;
	mdp = &t2p->t2_rdata;
	svtz = SSTOVC(ctx->f_ssp)->vc_sopt.sv_tz;
	switch (ctx->f_infolevel) {
	    case SMB_INFO_STANDARD:
		next = 0;
		fxsz = 0;
		md_get_uint16le(mdp, &date);
		md_get_uint16le(mdp, &time);	/* creation time */
		md_get_uint16le(mdp, &date);
		md_get_uint16le(mdp, &time);	/* access time */
		smb_dos2unixtime(date, time, 0, svtz, &ctx->f_attr.fa_atime);
		md_get_uint16le(mdp, &date);
		md_get_uint16le(mdp, &time);	/* modify time */
		smb_dos2unixtime(date, time, 0, svtz, &ctx->f_attr.fa_mtime);
		md_get_uint32le(mdp, &size);
		ctx->f_attr.fa_size = size;
		md_get_uint32(mdp, NULL);	/* allocation size */
		md_get_uint16le(mdp, &wattr);
		ctx->f_attr.fa_attr = wattr;
		md_get_uint8(mdp, &tb);
		size = nmlen = tb;
		fxsz = 23;
		recsz = next = 24 + nmlen;	/* docs misses zero byte at end */
		break;
	    case SMB_FIND_FILE_DIRECTORY_INFO:
		md_get_uint32le(mdp, &next);
		md_get_uint32(mdp, NULL);	/* file index */
		md_get_int64(mdp, NULL);	/* creation time */
		md_get_int64le(mdp, &lint);
		smb_time_NT2local(lint, svtz, &ctx->f_attr.fa_atime);
		md_get_int64le(mdp, &lint);
		smb_time_NT2local(lint, svtz, &ctx->f_attr.fa_mtime);
		md_get_int64le(mdp, &lint);
		smb_time_NT2local(lint, svtz, &ctx->f_attr.fa_ctime);
		md_get_int64le(mdp, &lint);	/* file size */
		ctx->f_attr.fa_size = lint;
		md_get_int64(mdp, NULL);	/* real size (should use) */
		/* freebsd bug: fa_attr endian bug */
		md_get_uint32le(mdp, &dattr);	/* EA */
		ctx->f_attr.fa_attr = dattr;
		md_get_uint32le(mdp, &size);	/* name len */
		fxsz = 64;
		recsz = next ? next : fxsz + size;
		break;
	    default:
		SMBERROR("unexpected info level %d\n", ctx->f_infolevel);
		return EINVAL;
	}
#ifdef APPLE
	if (SMB_UNICODE_STRINGS(SSTOVC(ctx->f_ssp))) {
		nmlen = min(size, SMB_MAXFNAMELEN * 2);
	} else
#endif
	nmlen = min(size, SMB_MAXFNAMELEN);
	cp = ctx->f_name;
	error = md_get_mem(mdp, cp, nmlen, MB_MSYSTEM);
	if (error)
		return error;
	if (next) {
		cnt = next - nmlen - fxsz;
		if (cnt > 0)
			md_get_mem(mdp, NULL, cnt, MB_MSYSTEM);
		else if (cnt < 0) {
			SMBERROR("out of sync\n");
			return EBADRPC;
		}
	}
#ifdef APPLE
	if (SMB_UNICODE_STRINGS(SSTOVC(ctx->f_ssp))) {
		if (nmlen > 1 && cp[nmlen - 1] == 0 && cp[nmlen - 2] == 0)
			nmlen -= 2;
	} else
#endif
	if (nmlen && cp[nmlen - 1] == 0)
		nmlen--;
	if (nmlen == 0)
		return EBADRPC;

	next = ctx->f_eofs + recsz;
	if (ctx->f_rnameofs &&
		(ctx->f_flags & SMBFS_RDD_GOTRNAME) == 0 &&
	    (ctx->f_rnameofs >= ctx->f_eofs &&
		ctx->f_rnameofs < (int)next)) {
		/*
		 * Server needs a resume filename.
		 */
		if (ctx->f_rnamelen <= nmlen) {
			if (ctx->f_rname)
				free(ctx->f_rname, M_SMBFSDATA);
			ctx->f_rname = malloc(nmlen + 1, M_SMBFSDATA, M_WAITOK);
			ctx->f_rnamelen = nmlen;
		}
		bcopy(ctx->f_name, ctx->f_rname, nmlen);
		ctx->f_rname[nmlen] = 0;
		ctx->f_flags |= SMBFS_RDD_GOTRNAME;
	}
	ctx->f_nmlen = nmlen;
	ctx->f_eofs = next;
	ctx->f_ecnt--;
	ctx->f_left--;
	return 0;
}

static int
smbfs_findcloseLM2(struct smbfs_fctx *ctx)
{
	if (ctx->f_name)
		free(ctx->f_name, M_SMBFSDATA);
	if (ctx->f_t2)
		smb_t2_done(ctx->f_t2);
	if ((ctx->f_flags & SMBFS_RDD_NOCLOSE) == 0)
		smbfs_smb_findclose2(ctx);
	return 0;
}

int
smbfs_findopen(struct smbnode *dnp, const char *wildcard, int wclen, int attr,
	struct smb_cred *scred, struct smbfs_fctx **ctxpp)
{
	struct smbfs_fctx *ctx;
	int error;

	ctx = malloc(sizeof(*ctx), M_SMBFSDATA, M_WAITOK);
	if (ctx == NULL)
		return ENOMEM;
	bzero(ctx, sizeof(*ctx));
	ctx->f_ssp = dnp->n_mount->sm_share;
	ctx->f_dnp = dnp;
	ctx->f_flags = SMBFS_RDD_FINDFIRST;
	ctx->f_scred = scred;
	if (SMB_DIALECT(SSTOVC(ctx->f_ssp)) < SMB_DIALECT_LANMAN2_0 ||
	    (dnp->n_mount->sm_args.flags & SMBFS_MOUNT_NO_LONG)) {
		ctx->f_flags |= SMBFS_RDD_USESEARCH;
		error = smbfs_findopenLM1(ctx, dnp, wildcard, wclen, attr, scred);
	} else
		error = smbfs_findopenLM2(ctx, dnp, wildcard, wclen, attr, scred);
	if (error)
		smbfs_findclose(ctx, scred);
	else
		*ctxpp = ctx;
	return error;
}

int
smbfs_findnext(struct smbfs_fctx *ctx, int limit, struct smb_cred *scred)
{
	int error;

	if (limit == 0)
		limit = 1000000;
	else if (limit > 1)
		limit *= 4;	/* empirical */
	ctx->f_scred = scred;
	for (;;) {
		if (ctx->f_flags & SMBFS_RDD_USESEARCH) {
			error = smbfs_findnextLM1(ctx, limit);
		} else
			error = smbfs_findnextLM2(ctx, limit);
		if (error)
			return error;
#ifdef APPLE
		if (SMB_UNICODE_STRINGS(SSTOVC(ctx->f_ssp))) {
			if ((ctx->f_nmlen == 2 &&
			     *(u_int16_t *)ctx->f_name == 0x2e00) ||
			    (ctx->f_nmlen == 4 &&
			     *(u_int32_t *)ctx->f_name == 0x2e002e00))
				continue;
		} else
#endif
		if ((ctx->f_nmlen == 1 && ctx->f_name[0] == '.') ||
		    (ctx->f_nmlen == 2 && ctx->f_name[0] == '.' &&
		     ctx->f_name[1] == '.'))
			continue;
		break;
	}
#ifdef APPLE
	smbfs_fname_tolocal(ctx);
#else
	smbfs_fname_tolocal(SSTOVC(ctx->f_ssp), ctx->f_name, ctx->f_nmlen,
	    ctx->f_dnp->n_mount->sm_caseopt);
#endif
	ctx->f_attr.fa_ino = smbfs_getino(ctx->f_dnp, ctx->f_name,
					  ctx->f_nmlen);
	return 0;
}

int
smbfs_findclose(struct smbfs_fctx *ctx, struct smb_cred *scred)
{
	ctx->f_scred = scred;
	if (ctx->f_flags & SMBFS_RDD_USESEARCH) {
		smbfs_findcloseLM1(ctx);
	} else
		smbfs_findcloseLM2(ctx);
	if (ctx->f_rname)
		free(ctx->f_rname, M_SMBFSDATA);
	free(ctx, M_SMBFSDATA);
	return 0;
}

int
smbfs_smb_lookup(struct smbnode *dnp, const char **namep, int *nmlenp,
	struct smbfattr *fap, struct smb_cred *scred)
{
	struct smbfs_fctx *ctx;
	int error;
	const char *name = (namep ? *namep : NULL);
	int nmlen = (nmlenp ? *nmlenp : 0);

	if (dnp == NULL || (dnp->n_ino == 2 && name == NULL)) {
		bzero(fap, sizeof(*fap));
		fap->fa_attr = SMB_FA_DIR;
		fap->fa_ino = 2;
		if (dnp == NULL)
			return 0;
		error = smbfs_smb_qpathinfo(dnp, fap, scred, 0);
		if (error != EINVAL)
			return error;
		error = smbfs_smb_query_info(dnp, NULL, 0, fap, scred);
		if (error || fap->fa_mtime.tv_sec)
			return error;
		smbfs_attr_touchdir(dnp);
		return 0;
	}
	if (nmlen == 1 && name[0] == '.') {
		error = smbfs_smb_lookup(dnp, NULL, NULL, fap, scred);
		return error;
	} else if (nmlen == 2 && name[0] == '.' && name[1] == '.') {
		error = smbfs_smb_lookup(dnp->n_parent, NULL, NULL, fap, scred);
		printf("%s: knows NOTHING about '..'\n", __FUNCTION__);
		return error;
	}
	/*
	 * This hides a server bug observable in Win98:
	 * size changes may not show until a CLOSE or a FLUSH op
	 */
	error = smbfs_smb_flush(dnp, scred);
	if (error)
		return (error);
	error = smbfs_findopen(dnp, name, nmlen,
			       SMB_FA_SYSTEM | SMB_FA_HIDDEN | SMB_FA_DIR,
			       scred, &ctx);
	if (error)
		return error;
	ctx->f_flags |= SMBFS_RDD_FINDSINGLE;
	error = smbfs_findnext(ctx, 1, scred);
	if (error == 0) {
		*fap = ctx->f_attr;
		if (name == NULL)
			fap->fa_ino = dnp->n_ino;
		if (namep)
			*namep = smbfs_name_alloc(ctx->f_name, ctx->f_nmlen);
		if (nmlenp)
			*nmlenp = ctx->f_nmlen;
	}
	smbfs_findclose(ctx, scred);
	return error;
}

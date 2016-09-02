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
 * $Id: smbfs_smb.c,v 1.73.38.3 2005/07/20 05:26:59 lindak Exp $
 */
#include <stdint.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/lock.h>
#include <sys/vnode.h>
#include <sys/xattr.h>
#include <sys/mbuf.h>
#include <sys/mount.h>

#include <sys/kauth.h>

#include <sys/smb_apple.h>
#include <sys/utfconv.h>

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
	u_int32_t ino;

	ino = dnp->n_ino + smbfs_hash((u_char *)name, nmlen);
	if (ino <= 2)
		ino += 3;
	return ino;
}

static int
smbfs_smb_lockandx(struct smbnode *np, int op, u_int32_t pid,
	off_t start, u_int64_t len, int largelock,
	struct smb_cred *scrp, u_int32_t timeout)
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
	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_LOCKING_ANDX, scrp);
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
	struct smb_cred *scrp, u_int32_t timeout)
{
	struct smb_share *ssp = np->n_mount->sm_share;

	if (SMB_DIALECT(SSTOVC(ssp)) < SMB_DIALECT_LANMAN1_0)
		/*
		 * TODO: use LOCK_BYTE_RANGE here.
		 */
		return EINVAL;
	else
		return smbfs_smb_lockandx(np, op, (u_int32_t)id, start, len,
					  largelock, scrp, timeout);
}

int
smbfs_smb_qpathinfo(struct smbnode *np, struct smbfattr *fap,
		    struct smb_cred *scrp, short infolevel)
{
	struct smb_share *ssp = np->n_mount->sm_share;
	struct smb_vc *vcp = SSTOVC(ssp);
	struct smb_t2rq *t2p;
	int error, svtz, timesok = 1;
	struct mbchain *mbp;
	struct mdchain *mdp;
	u_int16_t date, time, wattr;
	u_int64_t llint;
	u_int32_t size, dattr;

	error = smb_t2_alloc(SSTOCP(ssp), SMB_TRANS2_QUERY_PATH_INFORMATION,
			     scrp, &t2p);
	if (error)
		return error;
	mbp = &t2p->t2_tparam;
	mb_init(mbp);
	if (!infolevel) {
		if (SMB_DIALECT(vcp) < SMB_DIALECT_NTLM0_12)
			infolevel = SMB_QFILEINFO_STANDARD;
		else
			infolevel = SMB_QFILEINFO_BASIC_INFO;
	}
	mb_put_uint16le(mbp, infolevel);
	mb_put_uint32le(mbp, 0);
	/* mb_put_uint8(mbp, SMB_DT_ASCII); specs are wrong */
	error = smbfs_fullpath(mbp, vcp, np, NULL, NULL, '\\');
	if (error) {
		smb_t2_done(t2p);
		return error;
	}
	t2p->t2_maxpcount = 2;
	t2p->t2_maxdcount = vcp->vc_txmax;
	error = smb_t2_request(t2p);
	if (error) {
		smb_t2_done(t2p);
		if (infolevel == SMB_QFILEINFO_BASIC_INFO && error == EINVAL)
			return smbfs_smb_qpathinfo(np, fap, scrp,
						   SMB_QFILEINFO_STANDARD);
		return error;
	}
	mdp = &t2p->t2_rdata;
	svtz = vcp->vc_sopt.sv_tz;
	switch (infolevel) {
	    case SMB_QFILEINFO_STANDARD:
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
	    case SMB_QFILEINFO_BASIC_INFO:
		timesok = 0;
		md_get_uint64(mdp, NULL);	/* creation time */
		md_get_uint64le(mdp, &llint);
		if (llint) {
			timesok++;
			smb_time_NT2local(llint, svtz, &fap->fa_atime);
		}
		md_get_uint64le(mdp, &llint);
		if (llint) {
			timesok++;
			smb_time_NT2local(llint, svtz, &fap->fa_mtime);
		}
		md_get_uint64le(mdp, &llint);
		if (llint) {
			timesok++;
			smb_time_NT2local(llint, svtz, &fap->fa_ctime);
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
		if (infolevel == SMB_QFILEINFO_BASIC_INFO)
			return smbfs_smb_qpathinfo(np, fap, scrp,
						   SMB_QFILEINFO_STANDARD);
		error = EINVAL;
	}
	return error;
}

static char *
sfm2xattr(char *sfm)
{
	if (!strncasecmp(sfm, SFM_RESOURCEFORK_NAME,
			 sizeof(SFM_RESOURCEFORK_NAME)))
		return (XATTR_RESOURCEFORK_NAME);
	if (!strncasecmp(sfm, SFM_FINDERINFO_NAME,
			 sizeof(SFM_FINDERINFO_NAME)))
		return (XATTR_FINDERINFO_NAME);
	return (NULL);
}

static int
smbfs_smb_undollardata(struct smbnode *np, struct smbfs_fctx *ctx)
{
	char *cp;
	int len = strlen(SMB_DATASTREAM);

	if (!ctx->f_name)	/* sanity check */
		goto bad;
	if (ctx->f_nmlen < len + 1)	/* "::$DATA" at a minimum */
		goto bad;
	if (*ctx->f_name != ':')	/* leading colon - "always" */
		goto bad;
	cp =  &ctx->f_name[ctx->f_nmlen - len]; /* point to 2nd colon */
	if (bcmp(cp, SMB_DATASTREAM, len))
		goto bad;
	if (ctx->f_nmlen == len + 1)	/* merely the data fork? */
		return (0);		/* skip it */
	/*
	 * XXX here we should be calling KPI to validate the stream name
	 */
	if (ctx->f_nmlen >= 18 && !bcmp(ctx->f_name, ":com.apple.system.", 18))
		return (0);	/* skip protected system attrs */
	if (ctx->f_nmlen - len > XATTR_MAXNAMELEN + 1)
		goto bad;	/* mustnt return more than 128 bytes */
	/*
	 * Un-count a colon and the $DATA, then the
	 * 2nd colon is replaced by a terminating null.
	 */
	ctx->f_nmlen -= len;
	*cp = '\0';
	return (1);
bad:
	SMBERROR("file \"%.*s\" has bad stream \"%.*s\"\n", np->n_nmlen,
		 np->n_name, ctx->f_nmlen, ctx->f_name);
	return (0); /* skip it */
}

PRIVSYM int
smbfs_smb_qstreaminfo(struct smbnode *np, struct smb_cred *scrp,
		      uio_t uio, size_t *sizep)
{
	struct smb_share *ssp = np->n_mount->sm_share;
	struct smb_vc *vcp = SSTOVC(ssp);
	struct smb_t2rq *t2p;
	int error;
	struct mbchain *mbp;
	struct mdchain *mdp;
	u_int32_t next, nlen, used;
	struct smbfs_fctx ctx;

	*sizep = 0;
	ctx.f_ssp = ssp;
	ctx.f_name = NULL;

	error = smb_t2_alloc(SSTOCP(ssp), SMB_TRANS2_QUERY_PATH_INFORMATION,
			     scrp, &t2p);
	if (error)
		return (error);
	mbp = &t2p->t2_tparam;
	mb_init(mbp);
	/*
	 * SMB_QFILEINFO_STREAM_INFORMATION is an option to consider
	 * here.  Samba declined to support the older info level with
	 * a comment claiming doing so caused a BSOD.
	 */
	mb_put_uint16le(mbp, SMB_QFILEINFO_STREAM_INFO);
	mb_put_uint32le(mbp, 0);
	/* mb_put_uint8(mbp, SMB_DT_ASCII); specs are wrong */
	error = smbfs_fullpath(mbp, vcp, np, NULL, NULL, '\\');
	if (error)
		goto out;
	t2p->t2_maxpcount = 2;
	t2p->t2_maxdcount = vcp->vc_txmax;
	error = smb_t2_request(t2p);
	if (error) {
		if (smb_t2_err(t2p) == NT_STATUS_INVALID_PARAMETER)
			error = ENOTSUP;
		goto out;
	}
	mdp = &t2p->t2_rdata;
	/*
	 * On a directory Windoze is likely to return a zero data count.
	 * Check for that now to avoid EBADRPC from md_get_uint32le
	 */
	if (mdp->md_cur == NULL)
		goto out;
	do {
		if ((error = md_get_uint32le(mdp, &next)))
			goto out;
		if ((error = md_get_uint32le(mdp, &nlen))) /* name length */
			goto out;
		if ((error = md_get_uint64le(mdp, NULL))) /* stream size */
			goto out;
		if ((error = md_get_uint64le(mdp, NULL))) /* allocated size */
			goto out;
		/*
		 * Sanity check to limit DoS or buffer overrun attempts.
		 * The arbitrary 16384 is sufficient for all legit packets.
		 */
		if (nlen > 16384) {
			SMBERROR("huge name length in packet!\n");
			error = EBADRPC;
			goto out;
		}
		ctx.f_name = malloc(nlen, M_SMBFSDATA, M_WAITOK);
		if ((error = md_get_mem(mdp, ctx.f_name, nlen, MB_MSYSTEM)))
			goto out;
		/*
		 * skip pad bytes and/or tail of overlong name
		 */
		used = 4 + 4 + 8 + 8 + nlen;
		if (next && next > used) {
			if (next - used > 16384) {
				SMBERROR("huge offset in packet!\n");
				error = EBADRPC;
				goto out;
			}
			md_get_mem(mdp, NULL, next - used, MB_MSYSTEM);
		}
		/* ignore a trailing null, not that we expect them */
		if (SMB_UNICODE_STRINGS(vcp)) {
			if (nlen > 1 && !ctx.f_name[nlen - 1]
				     && !ctx.f_name[nlen - 2])
				nlen -= 2;
		} else {
			if (nlen && !ctx.f_name[nlen - 1])
				nlen -= 1;
		}       
		ctx.f_nmlen = nlen;
		smbfs_fname_tolocal(&ctx); /* converts from UCS2LE */
		/*
		 * We should now have a name in the form
		 * : <foo> :$DATA
		 * Where <foo> is UTF-8 w/o null termination
		 * If it isn't in that form we want to LOG it and skip it.
		 * Note we want to skip w/o logging the "data fork" entry,
		 * which is simply ::$DATA
		 * Otherwise we want to uiomove out <foo> with a null added.
		 */
		if (smbfs_smb_undollardata(np, &ctx)) {
			char *s;

			/* the "+ 1" skips over the leading colon */
			s = sfm2xattr(ctx.f_name + 1);
#ifndef DUAL_EAS	/* XXX */
	/*
	 * In Tiger Carbon still accesses dot-underscore files directly, so...
	 * For Tiger we preserve the SFM/Thursby AFP_* stream names rather
	 * than mapping them to com.apple.*.  This means our copy engines
	 * will preserve SFM/Thursby resource-fork and finder-info.
	 */
			s = NULL;
#endif
			if (s)
				ctx.f_nmlen = strlen(s) + 1;
			else
				s = ctx.f_name + 1;
			if (uio)
				uiomove(s, ctx.f_nmlen, uio);
			else
				*sizep += ctx.f_nmlen;
		}
		free(ctx.f_name, M_SMBFSDATA);
		ctx.f_name = NULL;
	} while (next && !error);
out:
	if (ctx.f_name)
		free(ctx.f_name, M_SMBFSDATA);
	smb_t2_done(t2p);
	return (error);
}


int
smbfs_smb_qfsattr(struct smb_share *ssp, u_int32_t *attrp,
		  struct smb_cred *scrp)
{
	struct smb_t2rq *t2p;
	struct mbchain *mbp;
	struct mdchain *mdp;
	u_int32_t nlen;
	int error;
	u_int8_t *fs_name;	/* will malloc whatever the size is */
	struct smbfs_fctx	ctx;

	error = smb_t2_alloc(SSTOCP(ssp), SMB_TRANS2_QUERY_FS_INFORMATION,
			     scrp, &t2p);
	if (error)
		return error;
	mbp = &t2p->t2_tparam;
	mb_init(mbp);
	mb_put_uint16le(mbp, SMB_QFS_ATTRIBUTE_INFO);
	t2p->t2_maxpcount = 4;
	t2p->t2_maxdcount = 4 * 3 + 512;
	error = smb_t2_request(t2p);
	if (error) {
		smb_t2_done(t2p);
		return error;
	}
	mdp = &t2p->t2_rdata;
	md_get_uint32le(mdp, attrp);
	md_get_uint32le(mdp, &ssp->ss_maxfilenamelen);
	md_get_uint32le(mdp, &nlen);	/* fs name length */
	if (ssp->ss_fsname == NULL && nlen) {
		ctx.f_ssp = ssp;
		ctx.f_name = malloc(nlen, M_SMBFSDATA, M_WAITOK);
		md_get_mem(mdp, ctx.f_name, nlen, MB_MSYSTEM);
		ctx.f_nmlen = nlen;
		smbfs_fname_tolocal(&ctx);
		fs_name = malloc(ctx.f_nmlen+1, M_SMBSTR, M_WAITOK);
		bcopy(ctx.f_name, fs_name, ctx.f_nmlen);
		fs_name[ctx.f_nmlen] = '\0';
		ssp->ss_fsname = (char *)fs_name;
		free(ctx.f_name, M_SMBFSDATA);
		/* 
		 * If fs_name isn't NTFS they probably require resume keys.
		 * This is another example of the client trying to fix a server
		 * bug. This code uses the logic created by PR-3983209. See
		 * long block comment in smbfs_smb_findnextLM2. 
		 */
		if (strcmp((char *)fs_name, "NTFS"))
			ssp->ss_flags |= SMBS_RESUMEKEYS;
		SMBERROR("(fyi) share '%s', attr 0x%x, maxfilename %d\n",
			 ssp->ss_fsname, *attrp, ssp->ss_maxfilenamelen);
	}
	smb_t2_done(t2p);
	return 0;
}


int
smbfs_smb_statfs2(struct smb_share *ssp, struct statfs *sbp,
	struct smb_cred *scrp)
{
	struct smb_t2rq *t2p;
	struct mbchain *mbp;
	struct mdchain *mdp;
	u_int16_t bsize;
	u_int32_t units, bpu, funits;
	u_int64_t s, t, f;
	int error;

	error = smb_t2_alloc(SSTOCP(ssp), SMB_TRANS2_QUERY_FS_INFORMATION,
	    scrp, &t2p);
	if (error)
		return error;
	mbp = &t2p->t2_tparam;
	mb_init(mbp);
	mb_put_uint16le(mbp, SMB_QFS_ALLOCATION);
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
	s = bsize;
	s *= bpu;
	t = units;
	f = funits;
	/*
	 * Don't allow over-large blocksizes as they determine
	 * Finder List-view size granularities.  On the other
	 * hand, we mustn't let the block count overflow the
	 * 31 bits available.
	 */
	while (s > 16 * 1024) {
		if (t > LONG_MAX)
			break;
		s /= 2;
		t *= 2;
		f *= 2;
	}
	while (t > LONG_MAX) {
		t /= 2;
		f /= 2;
		s *= 2;
	}
	sbp->f_bsize = s;	/* fundamental file system block size */
	sbp->f_blocks= t;	/* total data blocks in file system */
	sbp->f_bfree = f;	/* free blocks in fs */
	sbp->f_bavail= f;	/* free blocks avail to non-superuser */
	sbp->f_files = (-1);	/* total file nodes in file system */
	sbp->f_ffree = (-1);	/* free file nodes in fs */
	smb_t2_done(t2p);
	return 0;
}

int
smbfs_smb_statfs(struct smb_share *ssp, struct statfs *sbp,
	struct smb_cred *scrp)
{
	struct smb_rq rq, *rqp = &rq;
	struct mdchain *mdp;
	u_int16_t units, bpu, bsize, funits;
	u_int64_t s, t, f;
	int error;

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_QUERY_INFORMATION_DISK,
			    scrp);
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
	s = bsize;
	s *= bpu;
	t = units;
	f = funits;
	/*
	 * Don't allow over-large blocksizes as they determine
	 * Finder List-view size granularities.  On the other
	 * hand, we mustn't let the block count overflow the
	 * 31 bits available.
	 */
	while (s > 16 * 1024) {
		if (t > LONG_MAX)
			break;
		s /= 2;
		t *= 2;
		f *= 2;
	}
	while (t > LONG_MAX) {
		t /= 2;
		f /= 2;
		s *= 2;
	}
	sbp->f_bsize = s;	/* fundamental file system block size */
	sbp->f_blocks= t;	/* total data blocks in file system */
	sbp->f_bfree = f;	/* free blocks in fs */
	sbp->f_bavail= f;	/* free blocks avail to non-superuser */
	sbp->f_files = (-1);		/* total file nodes in file system */
	sbp->f_ffree = (-1);		/* free file nodes in fs */
	smb_rq_done(rqp);
	return 0;
}

int
smbfs_smb_seteof(struct smb_share *ssp, u_int16_t fid, u_int64_t newsize,
		 struct smb_cred *scrp)
{
	struct smb_t2rq *t2p;
	struct smb_vc *vcp = SSTOVC(ssp);
	struct mbchain *mbp;
	int error;

	error = smb_t2_alloc(SSTOCP(ssp), SMB_TRANS2_SET_FILE_INFORMATION,
	    scrp, &t2p);
	if (error)
		return error;
	mbp = &t2p->t2_tparam;
	mb_init(mbp);
	mb_put_mem(mbp, (caddr_t)&fid, 2, MB_MSYSTEM);
	if (vcp->vc_sopt.sv_caps & SMB_CAP_INFOLEVEL_PASSTHRU)
		mb_put_uint16le(mbp, SMB_SFILEINFO_END_OF_FILE_INFORMATION);
	else
		mb_put_uint16le(mbp, SMB_SFILEINFO_END_OF_FILE_INFO);
	mb_put_uint32le(mbp, 0); /* XXX should be 16 not 32(?) */
	mbp = &t2p->t2_tdata;
	mb_init(mbp);
	mb_put_uint64le(mbp, newsize);
	mb_put_uint32le(mbp, 0);			/* padding */
	mb_put_uint16le(mbp, 0);
	t2p->t2_maxpcount = 2;
	t2p->t2_maxdcount = 0;
	error = smb_t2_request(t2p);
	smb_t2_done(t2p);
	return error;
}

int
smbfs_smb_t2rename(struct smbnode *np, struct smbnode *tdnp,
	const char *tname, int tnmlen, struct smb_cred *scrp, int overwrite)
{
	struct smb_t2rq *t2p;
	struct smb_share *ssp = np->n_mount->sm_share;
	struct smb_vc *vcp = SSTOVC(ssp);
	struct mbchain *mbp;
	int32_t len, *ucslenp;
	int error, cerror;
	u_int16_t       fid = 0;
	char convbuf[1024];

	if (!(vcp->vc_sopt.sv_caps & SMB_CAP_INFOLEVEL_PASSTHRU))
		return (ENOTSUP);
	error = smb_t2_alloc(SSTOCP(ssp), SMB_TRANS2_SET_FILE_INFORMATION,
			     scrp, &t2p);
	if (error)
		return error;
	if (tdnp) {
		error = smbfs_smb_tmpopen(tdnp, SA_RIGHT_FILE_READ_DATA, scrp,
					  &fid);
		if (error)
			goto exit;
	}
	mbp = &t2p->t2_tparam;
	mb_init(mbp);
	mb_put_mem(mbp, (caddr_t)&np->n_fid, 2, MB_MSYSTEM);
	mb_put_uint16le(mbp, SMB_SFILEINFO_RENAME_INFORMATION);
	mb_put_uint16le(mbp, 0); /* reserved, nowadays */
	mbp = &t2p->t2_tdata;
	mb_init(mbp);
	mb_put_uint32le(mbp, overwrite);
	mb_put_mem(mbp, (caddr_t)&fid, 2, MB_MSYSTEM); /* base for tname */
	mb_put_uint16le(mbp, 0); /* part of a 32bit fid? */
	ucslenp = (int32_t *)mb_reserve(mbp, sizeof(int32_t));
	len = smb_strtouni((u_int16_t *)convbuf, tname, tnmlen,
			   UTF_PRECOMPOSED|UTF_NO_NULL_TERM);
	*ucslenp = htolel(len); 
	error = mb_put_mem(mbp, convbuf, len, MB_MSYSTEM);
	if (error)
		goto exit;
	error = mb_put_uint16le(mbp, 0);
	if (error)
		goto exit;
	t2p->t2_maxpcount = 2;
	t2p->t2_maxdcount = 0;
	error = smb_t2_request(t2p);
exit:;  
	if (fid) {
		cerror = smbfs_smb_tmpclose(tdnp, fid, scrp);
		if (cerror)
			SMBERROR("error %d closing fid %d\n", cerror, fid);
	}
	smb_t2_done(t2p);
	return (error);
}

int
smbfs_smb_flush(struct smbnode *np, struct smb_cred *scrp)
{
	struct smb_share *ssp = np->n_mount->sm_share;
	struct smb_rq rq, *rqp = &rq;
	struct mbchain *mbp;
	int error;

	if (!(np->n_flag & NFLUSHWIRE))
		return (0);
	if (np->n_fidrefs <= 0 || !SMBTOV(np) || !vnode_isreg(SMBTOV(np)))
		return 0; /* not a regular open file */
	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_FLUSH, scrp);
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
smbfs_smb_setfsize(struct smbnode *np, u_int16_t fid, u_int64_t newsize,
		   struct smb_cred *scrp)
{
	struct smb_share *ssp = np->n_mount->sm_share;
	struct smb_rq rq, *rqp = &rq;
	struct mbchain *mbp;
	int error;

	if (!smbfs_smb_seteof(ssp, fid, newsize, scrp)) {
		np->n_flag |= (NFLUSHWIRE | NATTRCHANGED);
		return (0);
	}
	if (newsize > UINT32_MAX)
		return (EFBIG);

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_WRITE, scrp);
	if (error)
		return error;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	mb_put_mem(mbp, (caddr_t)&fid, 2, MB_MSYSTEM);
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
		     struct smbfattr *fap, struct smb_cred *scrp)
{
	struct smb_rq rq, *rqp = &rq;
	struct smb_share *ssp = np->n_mount->sm_share;
	struct mbchain *mbp;
	struct mdchain *mdp;
	u_int8_t wc;
	int error;
	u_int16_t wattr;
	u_int32_t lint;

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_QUERY_INFORMATION, scrp);
	if (error)
		return error;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	mb_put_uint8(mbp, SMB_DT_ASCII);
	do {
		error = smbfs_fullpath(mbp, SSTOVC(ssp), np, name, &len, '\\');
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
		   struct smb_cred *scrp)
{
	struct smb_rq rq, *rqp = &rq;
	struct smb_share *ssp = np->n_mount->sm_share;
	struct mbchain *mbp;
	long time;
	int error, svtz;

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_SET_INFORMATION, scrp);
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
		error = smbfs_fullpath(mbp, SSTOVC(ssp), np, name, &len, '\\');
		if (error)
			break;
		mb_put_uint8(mbp, SMB_DT_ASCII);
		if (SMB_UNICODE_STRINGS(SSTOVC(ssp))) {
			mb_put_padbyte(mbp);
			mb_put_uint8(mbp, 0);	/* 1st byte of NULL Unicode char */
		}
		mb_put_uint8(mbp, 0);
		smb_rq_bend(rqp);
		error = smb_rq_simple(rqp);
		if (error)
			break;
	} while(0);
	smb_rq_done(rqp);
	return error;
}

int
smbfs_smb_hideit(struct smbnode *np, const char *name, int len,
		 struct smb_cred *scrp)
{
	struct smbfattr fa;
	int error;
	u_int16_t attr;

	error = smbfs_smb_query_info(np, name, len, &fa, scrp);
	attr = fa.fa_attr;
	if (!error && !(attr & SMB_FA_HIDDEN)) {
		attr |= SMB_FA_HIDDEN;
		error = smbfs_smb_setpattr(np, name, len, attr, NULL, scrp);
	}
	return (error);
}


int
smbfs_smb_unhideit(struct smbnode *np, const char *name, int len,
		   struct smb_cred *scrp)
{
	struct smbfattr fa;
	u_int16_t attr;
	int error;

	error = smbfs_smb_query_info(np, name, len, &fa, scrp);
	attr = fa.fa_attr;
	if (!error && (attr & SMB_FA_HIDDEN)) {
		attr &= ~SMB_FA_HIDDEN;
		error = smbfs_smb_setpattr(np, name, len, attr, NULL, scrp);
	}
	return (error);
}

/*
 * Note, win95 doesn't support this call.
 */
int
smbfs_smb_setptime2(struct smbnode *np, struct timespec *mtime,
	struct timespec *atime, int attr, struct smb_cred *scrp)
{
	struct smb_t2rq *t2p;
	struct smb_share *ssp = np->n_mount->sm_share;
	struct smb_vc *vcp = SSTOVC(ssp);
	struct mbchain *mbp;
	u_int16_t date, time;
	int error, tzoff;

	error = smb_t2_alloc(SSTOCP(ssp), SMB_TRANS2_SET_PATH_INFORMATION,
	    scrp, &t2p);
	if (error)
		return error;
	mbp = &t2p->t2_tparam;
	mb_init(mbp);
	mb_put_uint16le(mbp, SMB_SFILEINFO_STANDARD);
	mb_put_uint32le(mbp, 0);		/* MBZ */
	/* mb_put_uint8(mbp, SMB_DT_ASCII); specs incorrect */
	error = smbfs_fullpath(mbp, vcp, np, NULL, NULL, '\\');
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
 * *BASIC_INFO works with Samba, but Win2K servers say it is an
 * invalid information level on a SET_PATH_INFO.  Note Win2K does
 * support *BASIC_INFO on a SET_FILE_INFO, and they support the
 * equivalent *BASIC_INFORMATION on SET_PATH_INFO.  Go figure.
 */
int
smbfs_smb_setpattrNT(struct smbnode *np, u_int32_t attr, struct timespec *mtime,
	struct timespec *atime, struct smb_cred *scrp)
{
	struct smb_t2rq *t2p;
	struct smb_share *ssp = np->n_mount->sm_share;
	struct smb_vc *vcp = SSTOVC(ssp);
	struct mbchain *mbp;
	u_int64_t tm;
	int error, tzoff;
	/* 64 bit value for Jan 1 1980 */
	PRIVSYM u_int64_t DIFF1980TO1601 = 11960035200ULL*10000000ULL;

	error = smb_t2_alloc(SSTOCP(ssp), SMB_TRANS2_SET_PATH_INFORMATION,
	    scrp, &t2p);
	if (error)
		return error;
	mbp = &t2p->t2_tparam;
	mb_init(mbp);
	if (vcp->vc_sopt.sv_caps & SMB_CAP_INFOLEVEL_PASSTHRU)
		mb_put_uint16le(mbp, SMB_SFILEINFO_BASIC_INFORMATION);
	else
		mb_put_uint16le(mbp, SMB_SFILEINFO_BASIC_INFO);
	mb_put_uint32le(mbp, 0);		/* MBZ */
	/* mb_put_uint8(mbp, SMB_DT_ASCII); specs incorrect */
	error = smbfs_fullpath(mbp, vcp, np, NULL, NULL, '\\');
	if (error) {
		smb_t2_done(t2p);
		return error;
	}
	tzoff = vcp->vc_sopt.sv_tz;
	/* do we know it won't support dates < 1980? */
	if (!(ssp->ss_flags & SMBS_1980)) {
		mbp = &t2p->t2_tdata;
		mb_init(mbp);
		mb_put_uint64le(mbp, 0);		/* creation time */
		if (atime) {
			smb_time_local2NT(atime, tzoff, &tm);
		} else
			tm = 0;
		mb_put_uint64le(mbp, tm);		/* access time */
		if (mtime) {
			smb_time_local2NT(mtime, tzoff, &tm);
		} else
			tm = 0;
		mb_put_uint64le(mbp, tm);		/* last write time */
		mb_put_uint64le(mbp, tm);		/* change time */
		mb_put_uint32le(mbp, attr);		/* attr */
		mb_put_uint32le(mbp, 0);	/* undocumented padding */
		t2p->t2_maxpcount = 24;
		t2p->t2_maxdcount = 56;
		error = smb_t2_request(t2p);
	} 
	/* 
	 * "invalid argument" error probably means it's a
	 * FAT drive that doesn't accept dates earlier
	 * than 1980, so adjust dates and retry. If the
	 * 1980 flag is on we fell thru the if {} above
	 */
	if ((ssp->ss_flags & SMBS_1980) || (error == EINVAL)) { 
		mbp = &t2p->t2_tdata;
		mb_init(mbp);
		mb_put_uint64le(mbp, 0);		/* creation time */
		if (atime) {
			smb_time_local2NT(atime, tzoff, &tm);
			if (tm < DIFF1980TO1601)
				tm = DIFF1980TO1601;
		} else
			tm = 0;
		mb_put_uint64le(mbp, tm);		/* access time */
		if (mtime) {
			smb_time_local2NT(mtime, tzoff, &tm);
			if (tm < DIFF1980TO1601)
				tm = DIFF1980TO1601;
		} else
			tm = 0;
		mb_put_uint64le(mbp, tm);		/* last write time */
		mb_put_uint64le(mbp, tm);		/* change time */
		mb_put_uint32le(mbp, attr);		/* attr */
		mb_put_uint32le(mbp, 0);	/* undocumented padding */
		t2p->t2_maxpcount = 24;
		t2p->t2_maxdcount = 56;
		error = smb_t2_request(t2p);
		
		/* if this worked set the flag so we will do the right thing next time */ 
		if (!(error))
			ssp->ss_flags |= SMBS_1980;

	}
		smb_t2_done(t2p);
	return error;

}

/*
 * Set file atime and mtime. Isn't supported by core dialect.
 */
int
smbfs_smb_setftime(struct smbnode *np, struct timespec *mtime,
	struct timespec *atime, struct smb_cred *scrp)
{
	struct smb_rq rq, *rqp = &rq;
	struct smb_share *ssp = np->n_mount->sm_share;
	struct mbchain *mbp;
	u_int16_t date, time;
	int error, tzoff;

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_SET_INFORMATION2, scrp);
	if (error)
		return error;
	tzoff = SSTOVC(ssp)->vc_sopt.sv_tz;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
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
smbfs_smb_setfattrNT(struct smbnode *np, u_int32_t attr, struct timespec *mtime,
	struct timespec *atime, struct smb_cred *scrp)
{
	struct smb_t2rq *t2p;
	struct smb_share *ssp = np->n_mount->sm_share;
	struct smb_vc *vcp = SSTOVC(ssp);
	struct mbchain *mbp;
	u_int64_t tm;
	int error, svtz;

	error = smb_t2_alloc(SSTOCP(ssp), SMB_TRANS2_SET_FILE_INFORMATION,
	    scrp, &t2p);
	if (error)
		return error;
	svtz = SSTOVC(ssp)->vc_sopt.sv_tz;
	mbp = &t2p->t2_tparam;
	mb_init(mbp);
	mb_put_mem(mbp, (caddr_t)&np->n_fid, 2, MB_MSYSTEM);
	if (vcp->vc_sopt.sv_caps & SMB_CAP_INFOLEVEL_PASSTHRU)
		mb_put_uint16le(mbp, SMB_SFILEINFO_BASIC_INFORMATION);
	else
		mb_put_uint16le(mbp, SMB_SFILEINFO_BASIC_INFO);
	mb_put_uint32le(mbp, 0); /* XXX should be 16 not 32(?) */
	mbp = &t2p->t2_tdata;
	mb_init(mbp);
	mb_put_uint64le(mbp, 0);		/* creation time */
	if (atime) {
		smb_time_local2NT(atime, svtz, &tm);
	} else
		tm = 0;
	mb_put_uint64le(mbp, tm);		/* access time */
	if (mtime) {
		smb_time_local2NT(mtime, svtz, &tm);
	} else
		tm = 0;
	mb_put_uint64le(mbp, tm);		/* last write time */
	mb_put_uint64le(mbp, tm);		/* change time */
	mb_put_uint32le(mbp, attr);
	mb_put_uint32le(mbp, 0);			/* padding */
	t2p->t2_maxpcount = 2;
	t2p->t2_maxdcount = 0;
	error = smb_t2_request(t2p);
	smb_t2_done(t2p);
	return error;
}

/*
 * Modern create/open of file or directory.
 *
 * If disp is NTCREATEX_DISP_OPEN then this is an open attempt, and:
 *   If xattr then name is the stream to be opened at np,
 *   Else np should be opened.
 *   ...we won't touch *fidp,
 *   ...we will set or clear *attrcacheupdated.
 * Else this is a creation attempt, and:
 *   If xattr then name is the stream to create at np,
 *   Else name is the thing to create under directory np.
 *   ...we will return *fidp,
 *   ...we won't touch *attrcacheupdated.
 */
int
smbfs_smb_ntcreatex(struct smbnode *np, u_int32_t rights,
		    struct smb_cred *scrp, enum vtype vt,
		    int *attrcacheupdated, u_int16_t *fidp,
		    const char *name, int nmlen, u_int32_t disp, int xattr,
		    size_t *sizep, u_int32_t *rightsp)
{
	struct smb_rq rq, *rqp = &rq;
	struct smb_share *ssp = np->n_mount->sm_share;
	struct smb_vc *vcp = SSTOVC(ssp);
	struct mbchain *mbp;
	struct mdchain *mdp;
	struct smbfattr fap;
	u_int8_t wc;
	u_int32_t lint, createopt, efa;
	u_int64_t llint;
	int error;
	u_int16_t fid, *namelenp;

	nanotime(&fap.fa_reqtime);
	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_NT_CREATE_ANDX, scrp);
	if (error)
		return error;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	mb_put_uint8(mbp, 0xff);	/* secondary command */
	mb_put_uint8(mbp, 0);		/* MBZ */
	mb_put_uint16le(mbp, 0);	/* offset to next command (none) */
	mb_put_uint8(mbp, 0);		/* MBZ */
	namelenp = (u_int16_t *)mb_reserve(mbp, sizeof(u_int16_t));
	/*
	 * XP to a W2K Server does not use NTCREATEX_FLAGS_OPEN_DIRECTORY
	 * for creating nor for opening a directory.  Samba ignores the bit.
	 */
#if 0 /* causes sharing violation when making dir on W2K!*/
	mb_put_uint32le(mbp, vt == VDIR ?  NTCREATEX_FLAGS_OPEN_DIRECTORY : 0);
#else
	mb_put_uint32le(mbp, 0);	/* NTCREATEX_FLAGS_* */
#endif
	mb_put_uint32le(mbp, 0);	/* FID - basis for path if not root */
	mb_put_uint32le(mbp, rights);
	mb_put_uint64le(mbp, 0);	/* "initial allocation size" */
	efa = (vt == VDIR) ? SMB_EFA_DIRECTORY : SMB_EFA_NORMAL;
	if (disp != NTCREATEX_DISP_OPEN && !xattr) {
		if (efa == SMB_EFA_NORMAL)
			efa |= SMB_EFA_ARCHIVE;
		if (*name == '.')
			efa |= SMB_EFA_HIDDEN;
	}
	mb_put_uint32le(mbp, efa);
	mb_put_uint32le(mbp, NTCREATEX_SHARE_ACCESS_ALL);
	mb_put_uint32le(mbp, disp);
	createopt = 0;
	if (disp != NTCREATEX_DISP_OPEN) {
		if (vt == VDIR)
			createopt |= NTCREATEX_OPTIONS_DIRECTORY;
		/* (other create options currently not useful) */
	}
	mb_put_uint32le(mbp, createopt);
	mb_put_uint32le(mbp, NTCREATEX_IMPERSONATION_IMPERSONATION); /* (?) */
	mb_put_uint8(mbp, 0);   /* security flags (?) */
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	do {
		if (name == NULL)
			nmlen = 0;
		error = smbfs_fullpath(mbp, vcp, np, name, &nmlen,
				       xattr ? ':' : '\\');
		if (error)
			break;
		*namelenp = htoles(nmlen); /* includes null */
		smb_rq_bend(rqp);
		error = smb_rq_simple(rqp);
		if (error)
			break;
		smb_rq_getreply(rqp, &mdp);
		/*
		 * spec says 26 for word count, but 34 words are defined
		 * and observed from win2000
		 */
		if (md_get_uint8(mdp, &wc) != 0 ||
		    (wc != 26 && wc != 34 && wc != 42)) {
			error = EBADRPC;
			break;
		}
		md_get_uint8(mdp, NULL);	/* secondary cmd */
		md_get_uint8(mdp, NULL);	/* mbz */
		md_get_uint16le(mdp, NULL);     /* andxoffset */
		md_get_uint8(mdp, NULL);	/* oplock lvl granted */
		md_get_uint16(mdp, &fid);       /* yes, leaving it LE */
		md_get_uint32le(mdp, NULL);     /* create_action */
		md_get_uint64le(mdp, &llint);   /* creation time */
		md_get_uint64le(mdp, &llint);   /* access time */
		md_get_uint64le(mdp, &llint);   /* write time */
		if (llint)      /* avoid bogus 0 time (on FAT roots) */
			smb_time_NT2local(llint, vcp->vc_sopt.sv_tz, 
					  &fap.fa_mtime);
		md_get_uint64le(mdp, &llint);   /* change time */
		md_get_uint32le(mdp, &lint);    /* attributes */
		fap.fa_attr = lint;
		md_get_uint64le(mdp, NULL);     /* allocation size */
		md_get_uint64le(mdp, &llint);   /* EOF */
		fap.fa_size = llint;
		if (sizep)
			*sizep = fap.fa_size;
		md_get_uint16le(mdp, NULL);     /* file type */
		md_get_uint16le(mdp, NULL);     /* device state */
		md_get_uint8(mdp, NULL);	/* directory (boolean) */
	} while(0);
	smb_rq_done(rqp);
	if (error)      
		return error;
	if (fidp)
		*fidp = fid;
	if (rightsp)
		*rightsp = rights;
	if (disp != NTCREATEX_DISP_OPEN || xattr) /* creating, or xattr */
		goto uncached;
	if (attrcacheupdated)
		*attrcacheupdated = 0;
	/*       
	 * Update the cached attributes if they are still valid
	 * in the cache and if nothing has changed.
	 */	     
	if (np->n_vnode == NULL)
		goto uncached;
	if (smbfs_attr_cachelookup(np->n_vnode, NULL, scrp) != 0)
		goto uncached;  /* the cached attributes are not valid */
	if (fap.fa_size != np->n_size)
		goto uncached;  /* the size is different */
	if (fap.fa_attr != np->n_dosattr)
		goto uncached;  /* the attrs are different */ 
	/*      
	 * fap.fa_mtime is in two second increments while np->n_mtime
	 * may be in one second increments, so comparing the times is
	 * somewhat sloppy.
	 *
	 * XXX: true fap.fa_mtime resolution must depend upon server's
	 * local filesystem and is thus indeterminate... XXX ...TBD how that
	 * affects this code... note wire resolution here is 100ns versus
	 * 1sec down in smbfs_smb_oldopen (SMB_COM_OPEN)
	 */
	if (fap.fa_mtime.tv_sec != np->n_mtime.tv_sec &&
	    fap.fa_mtime.tv_sec != np->n_mtime.tv_sec - 1 &&
	    fap.fa_mtime.tv_sec != np->n_mtime.tv_sec + 1)
		goto uncached;  /* the mod time is different */

	fap.fa_mtime.tv_sec = np->n_mtime.tv_sec; /* keep higher res time */
	smbfs_attr_cacheenter(np->n_vnode, &fap);
	if (attrcacheupdated)
		*attrcacheupdated = 1;
uncached:
	return (0);
}

static u_int32_t
smb_mode2rights(int mode)
{
	mode = mode & SMB_AM_OPENMODE;

	switch(mode) {
	    case SMB_AM_OPENREAD:
		return (GENERIC_RIGHT_READ_ACCESS);
	    case SMB_AM_OPENWRITE:
		return (GENERIC_RIGHT_WRITE_ACCESS);
	    case SMB_AM_OPENRW:
		return (GENERIC_RIGHT_ALL_ACCESS);
	    case SMB_AM_OPENEXEC:
		return (GENERIC_RIGHT_EXECUTE_ACCESS);
	}
	return (0);
}

static int
smb_rights2mode(u_int32_t rights)
{
	int accmode = SMB_AM_OPENEXEC; /* our fallback */

	if (rights & (SA_RIGHT_FILE_APPEND_DATA | SA_RIGHT_FILE_DELETE_CHILD |
		      SA_RIGHT_FILE_WRITE_EA | SA_RIGHT_FILE_WRITE_ATTRIBUTES |
		      SA_RIGHT_FILE_WRITE_DATA | STD_RIGHT_WRITE_OWNER_ACCESS |
		      STD_RIGHT_DELETE_ACCESS | STD_RIGHT_WRITE_DAC_ACCESS |
		      GENERIC_RIGHT_ALL_ACCESS | GENERIC_RIGHT_WRITE_ACCESS))
		accmode = SMB_AM_OPENWRITE;
	if (rights & (SA_RIGHT_FILE_READ_DATA | SA_RIGHT_FILE_READ_ATTRIBUTES |
		      SA_RIGHT_FILE_READ_EA | STD_RIGHT_READ_CONTROL_ACCESS |
		      GENERIC_RIGHT_ALL_ACCESS | GENERIC_RIGHT_READ_ACCESS))
		accmode = (accmode == SMB_AM_OPENEXEC) ? SMB_AM_OPENREAD
						       : SMB_AM_OPENRW;
	return (accmode);
}


static int
smbfs_smb_oldopen(struct smbnode *np, int accmode, struct smb_cred *scrp,
		  int *attrcacheupdated, u_int16_t *fidp, const char *name,
		  int nmlen, int xattr, size_t *sizep, u_int32_t *rightsp)
{
	struct smb_rq rq, *rqp = &rq;
	struct smb_share *ssp = np->n_mount->sm_share;
	struct smb_vc *vcp = SSTOVC(ssp);
	struct mbchain *mbp;
	struct mdchain *mdp;
	struct smbfattr fap;
	u_int8_t wc;
	u_int16_t fid, wattr, grantedmode;
	u_int32_t lint;
	int error;

	/*
	 * Use DENYNONE to give unixy semantics of permitting
	 * everything not forbidden by permissions.  Ie denial
	 * is up to server with clients/openers needing to use
	 * advisory locks for further control.
	 */
	accmode |= SMB_SM_DENYNONE;

	nanotime(&fap.fa_reqtime);
	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_OPEN, scrp);
	if (error)
		return error;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	mb_put_uint16le(mbp, accmode);
	mb_put_uint16le(mbp, SMB_FA_SYSTEM | SMB_FA_HIDDEN | SMB_FA_RDONLY |
			     SMB_FA_DIR);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	mb_put_uint8(mbp, SMB_DT_ASCII);
	do {
		error = smbfs_fullpath(mbp, vcp, np, name, &nmlen,
				       xattr ? ':' : '\\');
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
		md_get_uint16(mdp, &fid); /* yes, we leave it LE */
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
			smb_time_server2local(lint, vcp->vc_sopt.sv_tz,
					      &fap.fa_mtime);
		md_get_uint32le(mdp, &lint);
		fap.fa_size = lint;
		if (sizep)
			*sizep = fap.fa_size;
		md_get_uint16le(mdp, &grantedmode);
	} while(0);
	smb_rq_done(rqp);
	if (error)
		return error;
	if (fidp)
		*fidp = fid;
	if (xattr)
		goto uncached;
	if (rightsp)
		*rightsp = smb_mode2rights(grantedmode);
	if (attrcacheupdated)
		*attrcacheupdated = 0;
	/*
	 * Update the cached attributes if they are still valid
	 * in the cache and if nothing has changed.
	 * Note that this won't ever update if the file size is
	 * greater than the 32-bits returned by SMB_COM_OPEN.
	 * For 64-bit file sizes, SMB_COM_NT_CREATE_ANDX must
	 * be used instead of SMB_COM_OPEN.
	 */
	if (np->n_vnode == NULL)
		goto uncached;
	if (smbfs_attr_cachelookup(np->n_vnode, NULL, scrp) != 0)
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
	if (fap.fa_mtime.tv_sec != np->n_mtime.tv_sec &&
	    fap.fa_mtime.tv_sec != np->n_mtime.tv_sec - 1 &&
	    fap.fa_mtime.tv_sec != np->n_mtime.tv_sec + 1)
		goto uncached;	/* the mod time is different */
	
	fap.fa_mtime.tv_sec = np->n_mtime.tv_sec; /* keep higher res time */
	smbfs_attr_cacheenter(np->n_vnode, &fap);
	if (attrcacheupdated)
		*attrcacheupdated = 1;
uncached:
	return 0;
}

int
smbfs_smb_tmpopen(struct smbnode *np, u_int32_t rights, struct smb_cred *scrp,
		  u_int16_t *fidp)
{
	struct smb_vc *vcp = SSTOVC(np->n_mount->sm_share);
	enum vtype vt = VREG;

	if (np->n_fid && (rights & np->n_rights) == rights) {
		np->n_fidrefs++; 
		*fidp = np->n_fid;
		return (0);
	}
	if (!(vcp->vc_sopt.sv_caps & SMB_CAP_NT_SMBS))
		return(smbfs_smb_oldopen(np, smb_rights2mode(rights), scrp,
					 NULL, fidp, NULL, 0,
					 0, NULL, NULL));
	if (SMBTOV(np))
		vt =  vnode_vtype(SMBTOV(np));
	return (smbfs_smb_ntcreatex(np, rights, scrp, vt,
				    NULL, fidp, NULL, 0, NTCREATEX_DISP_OPEN,
				    0, NULL, NULL));
}

int
smbfs_smb_tmpclose(struct smbnode *np, u_int16_t fid, struct smb_cred *scrp)
{
	struct smb_share *ssp = np->n_mount->sm_share;
	int error;

	if (fid != np->n_fid)
		return (smbfs_smb_close(ssp, fid, NULL, scrp));
	SMBASSERT(np->n_fidrefs > 0);
	np->n_fidrefs--; 
	if (np->n_fidrefs)
		return (0);
	error = smbfs_smb_close(ssp, fid, NULL, scrp);
	np->n_fid = 0;
	return (error);
}

int
smbfs_smb_open(struct smbnode *np, u_int32_t rights, struct smb_cred *scrp,
	       int *attrcacheupdated, u_int16_t *fidp, const char *name,
	       int nmlen, int xattr, size_t *sizep, u_int32_t *rightsp)
{
	int error;
	struct smb_share *ssp = np->n_mount->sm_share;
	struct smb_vc *vcp = SSTOVC(ssp);
	enum vtype vt = VREG;

	if (vcp->vc_sopt.sv_caps & SMB_CAP_NT_SMBS) {
		if (SMBTOV(np))
			vt =  vnode_vtype(SMBTOV(np));
		error = smbfs_smb_ntcreatex(np, rights, scrp, vt,
					    attrcacheupdated, fidp, name, nmlen,
					    NTCREATEX_DISP_OPEN, xattr, sizep,
					    rightsp);
	} else {
		error = smbfs_smb_oldopen(np, smb_rights2mode(rights), scrp,
					  attrcacheupdated, fidp, name, nmlen,
					  xattr, sizep, rightsp);
	}
	if (!error && !name)
		np->n_fidrefs++;
	return (error);
}

int
smbfs_smb_close(struct smb_share *ssp, u_int16_t fid, struct timespec *mtime,
	struct smb_cred *scrp)
{
	struct smb_rq rq, *rqp = &rq;
	struct mbchain *mbp;
	long time;
	int error;

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_CLOSE, scrp);
	if (error)
		return error;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	mb_put_mem(mbp, (caddr_t)&fid, sizeof(fid), MB_MSYSTEM);
	if (mtime) {
		smb_time_local2server(mtime, SSTOVC(ssp)->vc_sopt.sv_tz, &time);
	} else {
		/*
		 * Leach and SNIA docs say to send zero here.  X/Open says
		 * 0 and -1 both are leaving timestamp up to the server.
		 * Win9x treats zero as a real time-to-be-set!  We send -1,
		 * same as observed with smbclient.
		 */
		time = -1;
	}
	mb_put_uint32le(mbp, time);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	smb_rq_bend(rqp);
	error = smb_rq_simple(rqp);
	smb_rq_done(rqp);
	/*
	 * ENOTCONN isn't interesting - if the connection is closed,
	 * so are all our FIDs - and ENXIO is also not interesting,
	 * as it means a forced unmount was done.
	 *
	 * Don't clog up the system log with warnings about those failures
	 * on closes.
	 */
	if (error == ENOTCONN || error == ENXIO)
		error = 0;
	return error;
}

static int
smbfs_smb_oldcreate(struct smbnode *dnp, const char *name, int nmlen,
	struct smb_cred *scrp, u_int16_t *fidp, int xattr)
{
	struct smb_rq rq, *rqp = &rq;
	struct smb_share *ssp = dnp->n_mount->sm_share;
	struct mbchain *mbp;
	struct mdchain *mdp;
	struct timespec ctime;
	u_int8_t wc;
	long tm;
	int error;
	u_int16_t attr = SMB_FA_ARCHIVE;

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_CREATE, scrp);
	if (error)
		return error;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	if (name && *name == '.')
		attr |= SMB_FA_HIDDEN;
	mb_put_uint16le(mbp, attr);		/* attributes  */
	nanotime(&ctime);
	smb_time_local2server(&ctime, SSTOVC(ssp)->vc_sopt.sv_tz, &tm);
	mb_put_uint32le(mbp, tm);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	mb_put_uint8(mbp, SMB_DT_ASCII);
	error = smbfs_fullpath(mbp, SSTOVC(ssp), dnp, name, &nmlen,
			       xattr ? ':' : '\\');
	if (!error) {
		smb_rq_bend(rqp);
		error = smb_rq_simple(rqp);
		if (!error) {
			smb_rq_getreply(rqp, &mdp);
			md_get_uint8(mdp, &wc);
			if (wc == 1)
				md_get_uint16(mdp, fidp);
			else
				error = EBADRPC;
		}
	}
	smb_rq_done(rqp);
	return error;
}

int
smbfs_smb_create(struct smbnode *dnp, const char *name, int nmlen,
	struct smb_cred *scrp, u_int16_t *fidp, u_int32_t disp, int xattr)
{
	struct smb_vc *vcp = SSTOVC(dnp->n_mount->sm_share);

	/*
	 * At present the only access we might need is to WRITE data,
	 * and that only if we are creating a "symlink".  When/if the
	 * access needed gets more complex it should made a parameter
	 * and be set upstream.
	 */
	if (vcp->vc_sopt.sv_caps & SMB_CAP_NT_SMBS) {
		return (smbfs_smb_ntcreatex(dnp, SA_RIGHT_FILE_WRITE_DATA,
					    scrp, VREG, NULL, fidp, name,
					    nmlen, disp, xattr, NULL, NULL));
	} else
		return (smbfs_smb_oldcreate(dnp, name, nmlen, scrp, fidp,
					    xattr));
}

int
smbfs_smb_delete(struct smbnode *np, struct smb_cred *scrp, const char *name,
		 int nmlen, int xattr)
{
	struct smb_rq rq, *rqp = &rq;
	struct smb_share *ssp = np->n_mount->sm_share;
	struct mbchain *mbp;
	int error;

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_DELETE, scrp);
	if (error)
		return error;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	mb_put_uint16le(mbp, SMB_FA_SYSTEM | SMB_FA_HIDDEN);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	mb_put_uint8(mbp, SMB_DT_ASCII);
	error = smbfs_fullpath(mbp, SSTOVC(ssp), np, name, &nmlen,
			       xattr ? ':' : '\\');
	if (!error) {
		smb_rq_bend(rqp);
		error = smb_rq_simple(rqp);
	}
	smb_rq_done(rqp);
	return error;
}

int
smbfs_smb_rename(struct smbnode *src, struct smbnode *tdnp,
	const char *tname, int tnmlen, struct smb_cred *scrp)
{
	struct smb_rq rq, *rqp = &rq;
	struct smb_share *ssp = src->n_mount->sm_share;
	struct mbchain *mbp;
	int error;

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_RENAME, scrp);
	if (error)
		return error;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	/* freebsd bug: Let directories be renamed - Win98 requires DIR bit */
	mb_put_uint16le(mbp, (vnode_isdir(SMBTOV(src)) ? SMB_FA_DIR : 0) |
			     SMB_FA_SYSTEM | SMB_FA_HIDDEN);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	mb_put_uint8(mbp, SMB_DT_ASCII);
	do {
		error = smbfs_fullpath(mbp, SSTOVC(ssp), src, NULL, NULL, '\\');
		if (error)
			break;
		mb_put_uint8(mbp, SMB_DT_ASCII);
		error = smbfs_fullpath(mbp, SSTOVC(ssp), tdnp, tname, &tnmlen,
				       '\\');
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
	const char *tname, int tnmlen, u_int16_t flags, struct smb_cred *scrp)
{
	struct smb_rq rq, *rqp = &rq;
	struct smb_share *ssp = src->n_mount->sm_share;
	struct mbchain *mbp;
	int error;

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_MOVE, scrp);
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
		error = smbfs_fullpath(mbp, SSTOVC(ssp), src, NULL, NULL, '\\');
		if (error)
			break;
		mb_put_uint8(mbp, SMB_DT_ASCII);
		error = smbfs_fullpath(mbp, SSTOVC(ssp), tdnp, tname, &tnmlen,
				       '\\');
		if (error)
			break;
		smb_rq_bend(rqp);
		error = smb_rq_simple(rqp);
	} while(0);
	smb_rq_done(rqp);
	return error;
}

static int
smbfs_smb_oldmkdir(struct smbnode *dnp, const char *name, int len,
		   struct smb_cred *scrp)
{
	struct smb_rq rq, *rqp = &rq;
	struct smb_share *ssp = dnp->n_mount->sm_share;
	struct mbchain *mbp;
	int error;

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_CREATE_DIRECTORY, scrp);
	if (error)
		return error;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	mb_put_uint8(mbp, SMB_DT_ASCII);
	error = smbfs_fullpath(mbp, SSTOVC(ssp), dnp, name, &len, '\\');
	if (!error) {
		smb_rq_bend(rqp);
		error = smb_rq_simple(rqp);
	}
	smb_rq_done(rqp);
	return error;
}

int
smbfs_smb_mkdir(struct smbnode *dnp, const char *name, int len,
		struct smb_cred *scrp)
{
	struct smb_share *ssp = dnp->n_mount->sm_share;
	u_int16_t fid;
	int error;

	/*
	 * We ask for SA_RIGHT_FILE_READ_DATA not because we need it, but
	 * just to be asking for something.  The rights==0 case could
	 * easily be broken on some old or unusual servers.
	 */
	if (SSTOVC(ssp)->vc_sopt.sv_caps & SMB_CAP_NT_SMBS) {
		error = smbfs_smb_ntcreatex(dnp, SA_RIGHT_FILE_READ_DATA,
					    scrp, VDIR, NULL, &fid, name,
					    len, NTCREATEX_DISP_CREATE, 0,
					    NULL, NULL);
		if (error)
			return (error);
		error = smbfs_smb_close(ssp, fid, NULL, scrp);
		if (error)
			SMBERROR("error %d closing fid %d\n", error, fid);
		return (0);
	} else
		return (smbfs_smb_oldmkdir(dnp, name, len, scrp));
}

int
smbfs_smb_rmdir(struct smbnode *np, struct smb_cred *scrp)
{
	struct smb_rq rq, *rqp = &rq;
	struct smb_share *ssp = np->n_mount->sm_share;
	struct mbchain *mbp;
	int error;

	error = smb_rq_init(rqp, SSTOCP(ssp), SMB_COM_DELETE_DIRECTORY, scrp);
	if (error)
		return error;
	smb_rq_getrequest(rqp, &mbp);
	smb_rq_wstart(rqp);
	smb_rq_wend(rqp);
	smb_rq_bstart(rqp);
	mb_put_uint8(mbp, SMB_DT_ASCII);
	error = smbfs_fullpath(mbp, SSTOVC(ssp), np, NULL, NULL, '\\');
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
	int len, maxent,error, iseof = 0;

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
		len = ctx->f_wclen;
		error = smbfs_fullpath(mbp, vcp, ctx->f_dnp, ctx->f_wildcard,
				       &len, '\\');
		if (error)
			return error;
		mb_put_uint8(mbp, SMB_DT_VARIABLE);
		mb_put_uint16le(mbp, 0);	/* context length */
		ctx->f_flags &= ~SMBFS_RDD_FINDFIRST;
	} else {
		/* XXX - use "smbfs_fullpath()" and a null string? */
		if (SMB_UNICODE_STRINGS(vcp)) {
			mb_put_padbyte(mbp);
			mb_put_uint8(mbp, 0);
		}
		mb_put_uint8(mbp, 0);
		mb_put_uint8(mbp, SMB_DT_VARIABLE);
		mb_put_uint16le(mbp, SMB_SKEYLEN);
		mb_put_mem(mbp, (caddr_t)(ctx->f_skey), SMB_SKEYLEN, MB_MSYSTEM);
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
smbfs_smb_findopenLM1(struct smbfs_fctx *ctx, struct smbnode *dnp,
	const char *wildcard, int wclen, int attr, struct smb_cred *scrp)
{
	#pragma unused(dnp, scrp)
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
	ctx->f_name = (char *)(ctx->f_fname);
	return 0;
}

static int
smbfs_smb_findnextLM1(struct smbfs_fctx *ctx, int limit)
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
		nanotime(&ts);
		error = smbfs_smb_search(ctx);
		if (error)
			return error;
		ctx->f_attr.fa_reqtime = ts;
	}
	rqp = ctx->f_rq;
	smb_rq_getreply(rqp, &mdp);
	md_get_mem(mdp, (caddr_t)(ctx->f_skey), SMB_SKEYLEN, MB_MSYSTEM);
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
smbfs_smb_findcloseLM1(struct smbfs_fctx *ctx)
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
	int len, error;

	if (ctx->f_t2) {
		smb_t2_done(ctx->f_t2);
		ctx->f_t2 = NULL;
	}
	ctx->f_flags &= ~SMBFS_RDD_GOTRNAME;
	flags = FIND2_RETURN_RESUME_KEYS | FIND2_CLOSE_ON_EOS;
	if (ctx->f_flags & SMBFS_RDD_FINDSINGLE) {
		flags |= FIND2_CLOSE_AFTER_REQUEST;
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
		len = ctx->f_wclen;
		error = smbfs_fullpath(mbp, vcp, ctx->f_dnp, ctx->f_wildcard,
				       &len, '\\');
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
		if (ctx->f_ssp->ss_flags & SMBS_RESUMEKEYS) {
			mb_put_uint32le(mbp, ctx->f_rkey);
		} else
			mb_put_uint32le(mbp, 0);
		mb_put_uint16le(mbp, flags);
		if (ctx->f_rname) {
			/* resume file name */
			mb_put_mem(mbp, ctx->f_rname, ctx->f_rnamelen, MB_MSYSTEM);
		}
		/* Add trailing null - 1 byte if ASCII, 2 if Unicode */
		if (SMB_UNICODE_STRINGS(SSTOVC(ctx->f_ssp)))
			mb_put_uint8(mbp, 0);	/* 1st byte of NULL Unicode char */
		mb_put_uint8(mbp, 0);
#if 0
		struct timespec ts;
		ts.tv_sec = 0;
		ts.tv_nsec = 200 * 1000 * 1000;	/* 200ms */
		if (vcp->vc_flags & SMBC_WIN95) {
			/*
			 * some implementations suggests to sleep here
			 * for 200ms, due to the bug in the Win95.
			 * I've didn't notice any problem, but put code
			 * for it.
			 */
			 msleep(&flags, 0, PVFS, "fix95", &ts);
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
	ctx->f_ecnt = tw; /* search count - # entries returned */
	if ((error = md_get_uint16le(mdp, &tw)) != 0)
		return error;
	/*
	 * tw now is the "end of search" flag. against an XP server tw
	 * comes back zero when the prior find_next returned exactly
	 * the number of entries requested.  in which case we'd try again
	 * but the search has in fact been closed so an EBADF results.  our
	 * circumvention is to check here for a zero search count.
	 */
	if (tw || ctx->f_ecnt == 0)
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
smbfs_smb_findopenLM2(struct smbfs_fctx *ctx, struct smbnode *dnp,
	const char *wildcard, int wclen, int attr, struct smb_cred *scrp)
{
	#pragma unused(dnp, scrp)
	if (SMB_UNICODE_STRINGS(SSTOVC(ctx->f_ssp)))
		ctx->f_name = malloc(SMB_MAXFNAMELEN*2, M_SMBFSDATA, M_WAITOK);
	else
		ctx->f_name = malloc(SMB_MAXFNAMELEN, M_SMBFSDATA, M_WAITOK);
	if (ctx->f_name == NULL)
		return ENOMEM;
	ctx->f_infolevel = SMB_DIALECT(SSTOVC(ctx->f_ssp)) < SMB_DIALECT_NTLM0_12 ?
	    SMB_FIND_STANDARD : SMB_FIND_BOTH_DIRECTORY_INFO;
	ctx->f_attrmask = attr;
	ctx->f_wildcard = wildcard;
	ctx->f_wclen = wclen;
	return 0;
}

static int
smbfs_smb_findnextLM2(struct smbfs_fctx *ctx, int limit)
{
	struct mdchain *mdp;
	struct smb_t2rq *t2p;
	char *cp;
	u_int8_t tb;
	u_int16_t date, time, wattr;
	u_int32_t size, next, dattr, resumekey = 0;
	u_int64_t llint;
	int error, svtz, cnt, fxsz, nmlen, recsz, otw;
	struct timespec ts;

again:
	otw = 0;	/* nothing sent Over The Wire (yet) */
	if (ctx->f_ecnt == 0) {
		if (ctx->f_flags & SMBFS_RDD_EOF)
			return ENOENT;
		ctx->f_left = ctx->f_limit = limit;
		nanotime(&ts);
		error = smbfs_smb_trans2find2(ctx);
		if (error)
			return error;
		ctx->f_attr.fa_reqtime = ts;
		ctx->f_otws++;
		otw = 1;
	}
	t2p = ctx->f_t2;
	mdp = &t2p->t2_rdata;
	svtz = SSTOVC(ctx->f_ssp)->vc_sopt.sv_tz;
	switch (ctx->f_infolevel) {
	    case SMB_FIND_STANDARD:
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
	    case SMB_FIND_DIRECTORY_INFO:
	    case SMB_FIND_BOTH_DIRECTORY_INFO:
		md_get_uint32le(mdp, &next);
		md_get_uint32le(mdp, &resumekey); /* file index (resume key) */
		md_get_uint64(mdp, NULL);	/* creation time */
		md_get_uint64le(mdp, &llint);
		smb_time_NT2local(llint, svtz, &ctx->f_attr.fa_atime);
		md_get_uint64le(mdp, &llint);
		smb_time_NT2local(llint, svtz, &ctx->f_attr.fa_mtime);
		md_get_uint64le(mdp, &llint);
		smb_time_NT2local(llint, svtz, &ctx->f_attr.fa_ctime);
		md_get_uint64le(mdp, &llint);	/* file size */
		ctx->f_attr.fa_size = llint;
		md_get_uint64(mdp, NULL);	/* real size (should use) */
		/* freebsd bug: fa_attr endian bug */
		md_get_uint32le(mdp, &dattr);	/* extended file attributes */
		ctx->f_attr.fa_attr = dattr;
		md_get_uint32le(mdp, &size);	/* name len */
		fxsz = 64; /* size ofinfo up to filename */
		if (ctx->f_infolevel == SMB_FIND_BOTH_DIRECTORY_INFO) {
			/* 
			 * Skip EaSize (4 bytes), a byte of ShortNameLength,
			 * a reserved byte, and ShortName (8.3 means 24 bytes,
			 * as Leach defined it to always be Unicode)
			 */
			md_get_mem(mdp, NULL, 30, MB_MSYSTEM);
			fxsz += 30;
		}
		recsz = next ? next : fxsz + size;
		break;
	    default:
		SMBERROR("unexpected info level %d\n", ctx->f_infolevel);
		return EINVAL;
	}
	if (SMB_UNICODE_STRINGS(SSTOVC(ctx->f_ssp)))
		nmlen = min(size, SMB_MAXFNAMELEN * 2);
	else
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
	/* Don't count any trailing null in the name. */
	if (SMB_UNICODE_STRINGS(SSTOVC(ctx->f_ssp))) {
		if (nmlen > 1 && cp[nmlen - 1] == 0 && cp[nmlen - 2] == 0)
			nmlen -= 2;
	} else {
		if (nmlen && cp[nmlen - 1] == 0)
			nmlen--;
	}
	if (nmlen == 0)
		return EBADRPC;

	/*
	 * Ref radar 3983209.  On a find-next we expect a server will
	 * 1) if the continue bit is set, use the server's idea of current loc,
	 * 2) else if the resume key is non-zero, use that location,
	 * 3) else if the resume name is set, use that location,
	 * 4) else use the server's idea of current location.
	 *
	 * Current NetApps don't do that.  If we send no continue bit, a zero
	 * resume key, and a resume name, the NetApp ignores the resume name
	 * and acts on the (zero) resume key, sending back the start of the
	 * directory again.  Panther doesn't expose the netapp bug; Panther used
	 * the continue bit, but that was changed for 2866172. Win2000 as a
	 * client also relies upon the resume name, but they request a very
	 * large number of files, so the bug would be seen only with very
	 * large directories.
	 *
	 * Our fix is to notice if the second OTW op (the first find-next)
	 * returns, in the first filename, the same filename we got back
	 * at the start of the first OTW (the find-first).  In that case
	 * we've detected the server bug and set SMBS_RESUMEKEYS, causing us
	 * to send non-zero resume keys henceforth.
	 *
	 * Caveat: if there's a netapp so old it doesn't negotiate NTLM 0.12
	 * then we get no resume keys so f_rkey stays zero and this "fix"
	 * changes nothing.
	 *
	 * Note due to a similar problem (4051871) we also set SMBS_RESUMEKEYS
	 * for FAT volumes, at mount time.
	 */
	if (otw && !(ctx->f_ssp->ss_flags & SMBS_RESUMEKEYS)) {
		if (ctx->f_otws == 1) {
			ctx->f_firstnmlen = nmlen;
			ctx->f_firstnm = malloc(nmlen, M_SMBFSDATA, M_WAITOK);
			bcopy(ctx->f_name, ctx->f_firstnm, nmlen);
		} else if (ctx->f_otws == 2 && nmlen == ctx->f_firstnmlen &&
			   !bcmp(ctx->f_name, ctx->f_firstnm, nmlen)) {
			SMBERROR("server resume_name bug; using resume keys\n");
			ctx->f_ssp->ss_flags |= SMBS_RESUMEKEYS;
			ctx->f_ecnt = 0;
			goto again; /* must redo last otw op! */
		}
	}
	ctx->f_rkey = resumekey;

	next = ctx->f_eofs + recsz;
	if (ctx->f_rnameofs &&
		(ctx->f_flags & SMBFS_RDD_GOTRNAME) == 0 &&
	    (ctx->f_rnameofs >= ctx->f_eofs &&
		ctx->f_rnameofs < (int)next)) {
		/*
		 * Server needs a resume filename.
		 */
		if (ctx->f_rnamelen < nmlen) {
			if (ctx->f_rname)
				free(ctx->f_rname, M_SMBFSDATA);
			ctx->f_rname = malloc(nmlen, M_SMBFSDATA, M_WAITOK);
		}
		ctx->f_rnamelen = nmlen;
		bcopy(ctx->f_name, ctx->f_rname, nmlen);
		ctx->f_flags |= SMBFS_RDD_GOTRNAME;
	}
	ctx->f_nmlen = nmlen;
	ctx->f_eofs = next;
	ctx->f_ecnt--;
	ctx->f_left--;
	return 0;
}

static int
smbfs_smb_findcloseLM2(struct smbfs_fctx *ctx)
{
	if (ctx->f_name)
		free(ctx->f_name, M_SMBFSDATA);
	if (ctx->f_t2)
		smb_t2_done(ctx->f_t2);
	if ((ctx->f_flags & SMBFS_RDD_NOCLOSE) == 0)
		smbfs_smb_findclose2(ctx);
	return 0;
}

PRIVSYM int
smbfs_smb_findopen(struct smbnode *dnp, const char *wildcard, int wclen, int attr,
	struct smb_cred *scrp, struct smbfs_fctx **ctxpp)
{
	struct smbfs_fctx *ctx;
	int error;

	ctx = malloc(sizeof(*ctx), M_SMBFSDATA, M_WAITOK);
	if (ctx == NULL)
		return ENOMEM;
	bzero(ctx, sizeof(*ctx));
	if (dnp->n_mount->sm_share) {
		ctx->f_ssp = dnp->n_mount->sm_share;
		smb_share_ref(ctx->f_ssp);
	}
	ctx->f_dnp = dnp;
	ctx->f_flags = SMBFS_RDD_FINDFIRST;
	ctx->f_scred = scrp;
	if (SMB_DIALECT(SSTOVC(ctx->f_ssp)) < SMB_DIALECT_LANMAN2_0 ||
	    (dnp->n_mount->sm_args.flags & SMBFS_MOUNT_NO_LONG)) {
		ctx->f_flags |= SMBFS_RDD_USESEARCH;
		error = smbfs_smb_findopenLM1(ctx, dnp, wildcard, wclen, attr, scrp);
	} else
		error = smbfs_smb_findopenLM2(ctx, dnp, wildcard, wclen, attr, scrp);
	if (error)
		smbfs_smb_findclose(ctx, scrp);
	else
		*ctxpp = ctx;
	return error;
}

PRIVSYM int
smbfs_smb_findnext(struct smbfs_fctx *ctx, int limit, struct smb_cred *scrp)
{
	int error;

	if (limit == 0)
		limit = 1000000;
	else
		limit += 3; /* ensures we ask for 1 extra, plus . and .. */
	ctx->f_scred = scrp;
	for (;;) {
		if (ctx->f_flags & SMBFS_RDD_USESEARCH) {
			error = smbfs_smb_findnextLM1(ctx, limit);
		} else
			error = smbfs_smb_findnextLM2(ctx, limit);
		if (error)
			return error;
		if (SMB_UNICODE_STRINGS(SSTOVC(ctx->f_ssp))) {
			if ((ctx->f_nmlen == 2 &&
			     letohs(*(u_int16_t *)ctx->f_name) == 0x002e) ||
			    (ctx->f_nmlen == 4 &&
			     letohl(*(u_int32_t *)ctx->f_name) == 0x002e002e))
				continue;
		} else {
			if ((ctx->f_nmlen == 1 && ctx->f_name[0] == '.') ||
			    (ctx->f_nmlen == 2 && ctx->f_name[0] == '.' &&
			     ctx->f_name[1] == '.'))
				continue;
		}
		break;
	}
	smbfs_fname_tolocal(ctx);
	ctx->f_attr.fa_ino = smbfs_getino(ctx->f_dnp, ctx->f_name,
					  ctx->f_nmlen);
	return 0;
}

PRIVSYM int
smbfs_smb_findclose(struct smbfs_fctx *ctx, struct smb_cred *scrp)
{
	ctx->f_scred = scrp;
	if (ctx->f_flags & SMBFS_RDD_USESEARCH) {
		smbfs_smb_findcloseLM1(ctx);
	} else
		smbfs_smb_findcloseLM2(ctx);
	if (ctx->f_rname)
		free(ctx->f_rname, M_SMBFSDATA);
	if (ctx->f_firstnm)
		free(ctx->f_firstnm, M_SMBFSDATA);
	if (ctx->f_ssp)
		smb_share_rele(ctx->f_ssp, ctx->f_scred);
	free(ctx, M_SMBFSDATA);
	return 0;
}

int
smbfs_smb_lookup(struct smbnode *dnp, const char **namep, int *nmlenp,
	struct smbfattr *fap, struct smb_cred *scrp)
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
		error = smbfs_smb_qpathinfo(dnp, fap, scrp, 0);
		if (error != EINVAL)
			return error;
		error = smbfs_smb_query_info(dnp, NULL, 0, fap, scrp);
		if (error || fap->fa_mtime.tv_sec)
			return error;
		smbfs_attr_touchdir(dnp);
		return 0;
	}
	if (nmlen == 1 && name[0] == '.') {
		error = smbfs_smb_lookup(dnp, NULL, NULL, fap, scrp);
		return error;
	} else if (nmlen == 2 && name[0] == '.' && name[1] == '.') {
		error = smbfs_smb_lookup(dnp->n_parent, NULL, NULL, fap, scrp);
		printf("%s: knows NOTHING about '..'\n", __FUNCTION__);
		return error;
	}
	/*
	 * This hides a server bug observable in Win98:
	 * size changes may not show until a CLOSE or a FLUSH op
	 */
	error = smbfs_smb_flush(dnp, scrp);
	if (error)
		return (error);
	error = smbfs_smb_findopen(dnp, name, nmlen,
			       SMB_FA_SYSTEM | SMB_FA_HIDDEN | SMB_FA_DIR,
			       scrp, &ctx);
	if (error)
		return error;
	ctx->f_flags |= SMBFS_RDD_FINDSINGLE;
	error = smbfs_smb_findnext(ctx, 1, scrp);
	if (error == 0) {
		*fap = ctx->f_attr;
		if (name == NULL)
			fap->fa_ino = dnp->n_ino;
		if (namep)
			*namep = (char *)smbfs_name_alloc((u_char *)(ctx->f_name), ctx->f_nmlen);
		if (nmlenp)
			*nmlenp = ctx->f_nmlen;
	}
	smbfs_smb_findclose(ctx, scrp);
	return error;
}

int
smbfs_smb_getsec_int(struct smb_share *ssp, u_int16_t fid,
		     struct smb_cred *scrp, u_int32_t selector,
		     struct ntsecdesc **res, int *reslen)
{
	struct smb_ntrq *ntp;
	struct mbchain *mbp;
	struct mdchain *mdp;
	int error, len;

	error = smb_nt_alloc(SSTOCP(ssp), NT_TRANSACT_QUERY_SECURITY_DESC,
	    scrp, &ntp);
	if (error)
		return error;
	mbp = &ntp->nt_tparam;
	mb_init(mbp);
	mb_put_mem(mbp, (caddr_t)&fid, 2, MB_MSYSTEM);
	mb_put_uint16le(mbp, 0); /* reserved */
	mb_put_uint32le(mbp, selector);
	ntp->nt_maxpcount = 4;
	ntp->nt_maxdcount = *reslen;
	error = smb_nt_request(ntp);
	if (error && !(ntp->nt_flags & SMBT2_MOREDATA))
		goto done;
	*res = NULL;
	/*
	 * if there's more data than we said we could receive, here
	 * is where we pick up the length of it
	 */
	mdp = &ntp->nt_rparam;
	md_get_uint32le(mdp, (u_int32_t *)reslen);

	mdp = &ntp->nt_rdata;
	if (mdp->md_top) {	/* XXX md_cur safer than md_top */
		len = m_fixhdr(mdp->md_top);
		/*
		 * The following "if (len < *reslen)" handles a Windows bug
		 * observed when the underlying filesystem is FAT32.  In that
		 * case a 32 byte security descriptor comes back (S-1-1-0, ie
		 * "Everyone") but the Parameter Block claims 44 is the length
		 * of the security descriptor.  (The Data Block length
		 * claimed is 32.  This server bug was reported against NT
		 * first and I've personally observed it with W2K.
		 */
		if (len < *reslen)
			*reslen = len;
		if (len == *reslen) {
			MALLOC(*res, struct ntsecdesc *, len, M_TEMP, M_WAITOK);
			md_get_mem(mdp, (caddr_t)*res, len, MB_MSYSTEM);
		} else if (len > *reslen)
			SMBERROR("len %d *reslen %d fid 0x%x\n", len, *reslen,
				 letohs(fid));
	} else
		SMBERROR("null md_top? fid 0x%x\n", letohs(fid));
done:
	smb_nt_done(ntp);
	return (error);
}

int
smbfs_smb_getsec(struct smb_share *ssp, u_int16_t fid, struct smb_cred *scrp,
	u_int32_t selector, struct ntsecdesc **res)
{
	int error, olen, seclen;

	olen = seclen = 500; /* "overlarge" values => server errors */
	error = smbfs_smb_getsec_int(ssp, fid, scrp, selector, res, &seclen);
	if (error && seclen > olen)
		error = smbfs_smb_getsec_int(ssp, fid, scrp, selector, res,
					     &seclen);
	return (error);
}

int
smbfs_smb_setsec(struct smb_share *ssp, u_int16_t fid, struct smb_cred *scrp,
	u_int32_t selector, u_int16_t flags, struct ntsid *owner,
	struct ntsid *group, struct ntacl *sacl, struct ntacl *dacl)
{
	struct smb_ntrq *ntp;
	struct mbchain *mbp;
	int error, off;
	struct ntsecdesc ntsd;

	error = smb_nt_alloc(SSTOCP(ssp), NT_TRANSACT_SET_SECURITY_DESC,
	    scrp, &ntp);
	if (error)
		return error;
	mbp = &ntp->nt_tparam;
	mb_init(mbp);
	mb_put_mem(mbp, (caddr_t)&fid, 2, MB_MSYSTEM);
	mb_put_uint16le(mbp, 0); /* reserved */
	mb_put_uint32le(mbp, selector);
	mbp = &ntp->nt_tdata;
	mb_init(mbp);
	bzero(&ntsd, sizeof ntsd);
	wset_sdrevision(&ntsd);
	/*
	 * A note about flags ("SECURITY_DESCRIPTOR_CONTROL" in MSDN)
	 * We set here only those bits we can be sure must be set.  The rest
	 * are up to the caller.  In particular, the caller may intentionally
	 * set an acl PRESENT bit while giving us a null pointer for the
	 * acl - that sets a null acl, giving access to everyone.  Note also
	 * that the AUTO_INHERITED bits should probably always be set unless
	 * the server is NT.
	 */
	flags |= SD_SELF_RELATIVE;
	off = sizeof ntsd;
	if (owner) {
		wset_sdowneroff(&ntsd, off);
		off += sidlen(owner);
	}
	if (group) {
		wset_sdgroupoff(&ntsd, off);
		off += sidlen(group);
	}
	if (sacl) {
		flags |= SD_SACL_PRESENT;
		wset_sdsacloff(&ntsd, off);
		off += acllen(sacl);
	}
	if (dacl) {
		flags |= SD_DACL_PRESENT;
		wset_sddacloff(&ntsd, off);
	}
	wset_sdflags(&ntsd, flags);
	mb_put_mem(mbp, (caddr_t)&ntsd, sizeof ntsd, MB_MSYSTEM);
	if (owner)
		mb_put_mem(mbp, (caddr_t)owner, sidlen(owner), MB_MSYSTEM);
	if (group)
		mb_put_mem(mbp, (caddr_t)group, sidlen(group), MB_MSYSTEM);
	if (sacl)
		mb_put_mem(mbp, (caddr_t)sacl, acllen(sacl), MB_MSYSTEM);
	if (dacl)
		mb_put_mem(mbp, (caddr_t)dacl, acllen(dacl), MB_MSYSTEM);
	ntp->nt_maxpcount = 0;
	ntp->nt_maxdcount = 0;
	error = smb_nt_request(ntp);
	smb_nt_done(ntp);
	return (error);
}

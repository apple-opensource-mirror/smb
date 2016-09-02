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
 * $Id: smb_subr.c,v 1.11 2002/03/12 22:06:10 lindak Exp $
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/lock.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <sys/signalvar.h>
#include <sys/mbuf.h>

#ifdef APPLE
#include <sys/smb_apple.h>
#include <sys/utfconv.h>
#endif

#include <sys/iconv.h>

#include <netsmb/smb.h>
#include <netsmb/smb_conn.h>
#include <netsmb/smb_rq.h>
#include <netsmb/smb_subr.h>

MALLOC_DEFINE(M_SMBDATA, "SMBDATA", "Misc netsmb data");
MALLOC_DEFINE(M_SMBSTR, "SMBSTR", "netsmb string data");
MALLOC_DEFINE(M_SMBTEMP, "SMBTEMP", "Temp netsmb data");

smb_unichar smb_unieol = 0;

void
smb_makescred(struct smb_cred *scred, struct proc *p, struct ucred *cred)
{
	if (p) {
		scred->scr_p = p;
		scred->scr_cred = cred ? cred : p->p_ucred;
	} else {
		scred->scr_p = NULL;
		scred->scr_cred = cred ? cred : NULL;
	}
}

int
smb_proc_intr(struct proc *p)
{
#if defined(APPLE) || __FreeBSD_version < 400009

	if (p && p->p_siglist &&
	    (((p->p_siglist & ~p->p_sigmask) & ~p->p_sigignore) & SMB_SIGMASK))
		return EINTR;
	return 0;
#else
	sigset_t tmpset;

	if (p == NULL)
		return 0;
	tmpset = p->p_siglist;
	SIGSETNAND(tmpset, p->p_sigmask);
	SIGSETNAND(tmpset, p->p_sigignore);
	if (SIGNOTEMPTY(p->p_siglist) && SMB_SIGMASK(tmpset))
                return EINTR;
	return 0;
#endif
}

char *
smb_strdup(const char *s)
{
	char *p;
	int len;

	len = s ? strlen(s) + 1 : 1;
	p = malloc(len, M_SMBSTR, M_WAITOK);
	if (s)
		bcopy(s, p, len);
	else
		*p = 0;
	return p;
}

/*
 * duplicate string from a user space.
 */
char *
smb_strdupin(char *s, int maxlen)
{
	char *p, bt;
	int len = 0;

	for (p = s; ;p++) {
		if (copyin(p, &bt, 1))
			return NULL;
		len++;
		if (maxlen && len > maxlen)
			return NULL;
		if (bt == 0)
			break;
	}
	p = malloc(len, M_SMBSTR, M_WAITOK);
	copyin(s, p, len);
	return p;
}

/*
 * duplicate memory block from a user space.
 */
void *
smb_memdupin(void *umem, int len)
{
	char *p;

	if (len > 8 * 1024)
		return NULL;
	p = malloc(len, M_SMBSTR, M_WAITOK);
	if (copyin(umem, p, len) == 0)
		return p;
	free(p, M_SMBSTR);
	return NULL;
}

/*
 * duplicate memory block in the kernel space.
 */
void *
smb_memdup(const void *umem, int len)
{
	char *p;

	if (len > 8 * 1024)
		return NULL;
	p = malloc(len, M_SMBSTR, M_WAITOK);
	if (p == NULL)
		return NULL;
	bcopy(umem, p, len);
	return p;
}

void
smb_strfree(char *s)
{
	free(s, M_SMBSTR);
}

void
smb_memfree(void *s)
{
	free(s, M_SMBSTR);
}

void *
#ifdef APPLE
smb_zmalloc(unsigned long size, int type, int flags)
#else
smb_zmalloc(unsigned long size, struct malloc_type *type, int flags)
#endif
{

#ifdef M_ZERO
	return malloc(size, type, flags | M_ZERO);
#else
	void *p = malloc(size, type, flags);
	if (p)
		bzero(p, size);
	return p;
#endif
}

void
smb_strtouni(u_int16_t *dst, const char *src)
{
#ifdef APPLE
	size_t inlen = strlen(src);
	size_t outlen;
	int flags = UTF_PRECOMPOSED;

	if (BYTE_ORDER != LITTLE_ENDIAN)
		flags |= UTF_REVERSE_ENDIAN;
	if (utf8_decodestr(src, inlen, dst, &outlen, inlen * 2, 0, flags) != 0)
		dst[0] = 0;
	else
		dst[outlen/2] = 0;
#else
	while (*src) {
		*dst++ = htoles(*src++);
	}
	*dst = 0;
#endif /* APPLE */
}

#ifdef SMB_SOCKETDATA_DEBUG
void
m_dumpm(struct mbuf *m) {
	char *p;
	int len;
	printf("d=");
	while(m) {
		p=mtod(m,char *);
		len=m->m_len;
		printf("(%d)",len);
		while(len--){
			printf("%02x ",((int)*(p++)) & 0xff);
		}
		m=m->m_next;
	};
	printf("\n");
}
#endif

int
smb_maperror(int eclass, int eno)
{
	if (eclass == 0 && eno == 0)
		return 0;
	switch (eclass) {
	    case ERRDOS:
		switch (eno) {
		    case ERRbadfunc:
		    case ERRbadmcb:
		    case ERRbadenv:
		    case ERRbadformat:
		    case ERRrmuns:
			return EINVAL;
		    case ERRbadfile:
		    case ERRbadpath:
		    case ERRremcd:
		    case ERRnoipc:
		    case ERRnosuchshare:
			return ENOENT;
		    case ERRnofids:
			return EMFILE;
		    case ERRnoaccess:
		    case ERRbadshare:
			return EACCES;
		    case ERRbadfid:
			return EBADF;
		    case ERRnomem:
			return ENOMEM;	/* actually remote no mem... */
		    case ERRbadmem:
			return EFAULT;
		    case ERRbadaccess:
			return EACCES;
		    case ERRbaddata:
			return E2BIG;
		    case ERRbaddrive:
		    case ERRnotready:	/* nt */
			return ENXIO;
		    case ERRdiffdevice:
			return EXDEV;
		    case ERRnofiles:
			return 0;	/* eeof ? */
			return ETXTBSY;
		    case ERRlock:
			return EDEADLK;
		    case ERRfilexists:
			return EEXIST;
		    case ERRinvalidname:	/* samba maps as noent */
			return ENOENT;
		    case 145:		/* samba */
			return ENOTEMPTY;
		    case ERRnotlocked:
			return 0; /* 0 since bsd unlocks on any close */
		    case ERRrename:
			return EEXIST;
		}
		break;
	    case ERRSRV:
		switch (eno) {
		    case ERRerror:
			return EINVAL;
		    case ERRbadpw:
			return EAUTH;
		    case ERRaccess:
			return EACCES;
		    case ERRinvnid:
			return ENETRESET;
		    case ERRinvnetname:
			SMBERROR("NetBIOS name is invalid\n");
			return EAUTH;
		    case ERRbadtype:		/* reserved and returned */
			return EIO;
		    case 2239:		/* NT: account exists but disabled */
			return EPERM;
		}
		break;
	    case ERRHRD:
		switch (eno) {
		    case ERRnowrite:
			return EROFS;
		    case ERRbadunit:
			return ENODEV;
		    case ERRnotready:
		    case ERRbadcmd:
		    case ERRdata:
			return EIO;
		    case ERRbadreq:
			return EBADRPC;
		    case ERRbadshare:
			return ETXTBSY;
		    case ERRlock:
			return EDEADLK;
		}
		break;
	}
	SMBERROR("Unmapped error %d:%d\n", eclass, eno);
	return EBADRPC;
}

#ifdef APPLE

int
smb_put_dmem(struct mbchain *mbp, struct smb_vc *vcp, const char *src,
	int size, int caseopt)
{
	char convbuf[512];
	char *dst;
	size_t inlen, outlen;
	int error;

	if (size == 0)
		return 0;
	if (vcp->vc_toserver == NULL)
		return mb_put_mem(mbp, src, size, MB_MSYSTEM);

	inlen = size;
	outlen = sizeof(convbuf);
	dst = convbuf;

	error = iconv_conv(vcp->vc_toserver, &src, &inlen, &dst, &outlen);
	if (error)
		return error;
	outlen = sizeof(convbuf) - outlen;
	if (SMB_UNICODE_STRINGS(vcp))
		mb_put_padbyte(mbp);
	error = mb_put_mem(mbp, (c_caddr_t)convbuf, outlen, MB_MSYSTEM);
	return error;
}

#else

static int
smb_copy_iconv(struct mbchain *mbp, c_caddr_t src, caddr_t dst, int len)
{
	int outlen = len;

	return iconv_conv((struct iconv_drv*)mbp->mb_udata, &src, &len, &dst, &outlen);
}

int
smb_put_dmem(struct mbchain *mbp, struct smb_vc *vcp, const char *src,
	int size, int caseopt)
{
	struct iconv_drv *dp = vcp->vc_toserver;

	if (size == 0)
		return 0;
	if (dp == NULL) {
		return mb_put_mem(mbp, src, size, MB_MSYSTEM);
	}
	mbp->mb_copy = smb_copy_iconv;
	mbp->mb_udata = dp;
	return mb_put_mem(mbp, src, size, MB_MCUSTOM);
}

#endif

int
smb_put_dstring(struct mbchain *mbp, struct smb_vc *vcp, const char *src,
	int caseopt)
{
	int error;

	error = smb_put_dmem(mbp, vcp, src, strlen(src), caseopt);
	if (error)
		return error;
#ifdef APPLE
	if (SMB_UNICODE_STRINGS(vcp))
		return mb_put_uint16le(mbp, 0);
#endif
	return mb_put_uint8(mbp, 0);
}

#ifndef APPLE
int
smb_put_asunistring(struct smb_rq *rqp, const char *src)
{
	struct mbchain *mbp = &rqp->sr_rq;
	struct iconv_drv *dp = rqp->sr_vc->vc_toserver;
	u_char c;
	int error;

	while (*src) {
		iconv_convmem(dp, &c, src++, 1);
		error = mb_put_uint16le(mbp, c);
		if (error)
			return error;
	}
	return mb_put_uint16le(mbp, 0);
}
#endif

int
smb_checksmp(void)
{
#ifndef APPLE
	int name[2];
	int olen, ncpu, plen, error;

	name[0] = CTL_HW;
	name[1] = HW_NCPU;
	error = kernel_sysctl(curproc, name, 2, &ncpu, &olen, NULL, 0, &plen);
	if (error)
		return error;
#ifndef	SMP
	if (ncpu > 1) {
		printf("error: module compiled without SMP support\n");
		return EPERM;
	}
#else
	if (ncpu < 2) {
		printf("warning: only one CPU active on in SMP kernel ?\n");
	}
#endif
#else
	/* APPLE:
	 * just return success...
	 * since kernel_sysctl is broken
	 * and hw_sysctl tries to copyout to user space
	 * and we are always SMP anyway
	 */
#endif /* APPLE */
	return 0;
}

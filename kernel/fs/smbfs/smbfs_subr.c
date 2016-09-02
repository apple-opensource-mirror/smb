/*
 * Copyright (c) 2000-2001, Boris Popov
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
 * $Id: smbfs_subr.c,v 1.18 2005/02/02 00:22:23 lindak Exp $
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/time.h>
#include <sys/vnode.h>
#include <sys/sysctl.h>

#include <sys/kauth.h>

#include <sys/smb_apple.h>
#include <sys/utfconv.h>

#include <sys/smb_iconv.h>

#include <netsmb/smb.h>
#include <netsmb/smb_conn.h>
#include <netsmb/smb_subr.h>
#include <netsmb/smb_rq.h>
#include <netsmb/smb_dev.h>

#include <fs/smbfs/smbfs.h>
#include <fs/smbfs/smbfs_node.h>
#include <fs/smbfs/smbfs_subr.h>

MALLOC_DEFINE(M_SMBFSDATA, "SMBFS data", "SMBFS private data");

/* 
 * Time & date conversion routines taken from msdosfs. Although leap
 * year calculation is bogus, it's sufficient before 2100 :)
 */
/*
 * This is the format of the contents of the deTime field in the direntry
 * structure.
 * We don't use bitfields because we don't know how compilers for
 * arbitrary machines will lay them out.
 */
#define DT_2SECONDS_MASK	0x1F	/* seconds divided by 2 */
#define DT_2SECONDS_SHIFT	0
#define DT_MINUTES_MASK		0x7E0	/* minutes */
#define DT_MINUTES_SHIFT	5
#define DT_HOURS_MASK		0xF800	/* hours */
#define DT_HOURS_SHIFT		11

/*
 * This is the format of the contents of the deDate field in the direntry
 * structure.
 */
#define DD_DAY_MASK		0x1F	/* day of month */
#define DD_DAY_SHIFT		0
#define DD_MONTH_MASK		0x1E0	/* month */
#define DD_MONTH_SHIFT		5
#define DD_YEAR_MASK		0xFE00	/* year - 1980 */
#define DD_YEAR_SHIFT		9
/*
 * Total number of days that have passed for each month in a regular year.
 */
static u_short regyear[] = {
	31, 59, 90, 120, 151, 181,
	212, 243, 273, 304, 334, 365
};

/*
 * Total number of days that have passed for each month in a leap year.
 */
static u_short leapyear[] = {
	31, 60, 91, 121, 152, 182,
	213, 244, 274, 305, 335, 366
};

/*
 * Variables used to remember parts of the last time conversion.  Maybe we
 * can avoid a full conversion.
 */
static u_long  lasttime;
static u_long  lastday;
static u_short lastddate;
static u_short lastdtime;

PRIVSYM int wall_cmos_clock = 0;	/* XXX */
PRIVSYM int adjkerntz = 0;	/* XXX */

void
smb_time_local2server(struct timespec *tsp, int tzoff, long *seconds)
{
	/*
	 * XXX - what if we connected to the server when it was in
	 * daylight savings/summer time and we've subsequently switched
	 * to standard time, or vice versa, so that the time zone
	 * offset we got from the server is now wrong?
	 */
	*seconds = tsp->tv_sec - tzoff * 60 /*- tz.tz_minuteswest * 60 -
	    (wall_cmos_clock ? adjkerntz : 0)*/;
}

void
smb_time_server2local(u_long seconds, int tzoff, struct timespec *tsp)
{
	/*
	 * XXX - what if we connected to the server when it was in
	 * daylight savings/summer time and we've subsequently switched
	 * to standard time, or vice versa, so that the time zone
	 * offset we got from the server is now wrong?
	 */
	tsp->tv_sec = seconds + tzoff * 60;
	    /*+ tz.tz_minuteswest * 60 + (wall_cmos_clock ? adjkerntz : 0)*/;
}

/*
 * Number of seconds between 1970 and 1601 year
 */
PRIVSYM u_int64_t DIFF1970TO1601 = 11644473600ULL;

/*
 * Time from server comes as UTC, so no need to use tz
 */
void
smb_time_NT2local(u_int64_t nsec, int tzoff, struct timespec *tsp)
{
	#pragma unused(tzoff)
	smb_time_server2local(nsec / 10000000 - DIFF1970TO1601, 0, tsp);
}

void
smb_time_local2NT(struct timespec *tsp, int tzoff, u_int64_t *nsec)
{
	#pragma unused(tzoff)
	long seconds;

	smb_time_local2server(tsp, 0, &seconds);
	*nsec = (((u_int64_t)(seconds) & ~1) + DIFF1970TO1601) * (u_int64_t)10000000;
}

void
smb_time_unix2dos(struct timespec *tsp, int tzoff, u_int16_t *ddp, 
	u_int16_t *dtp,	u_int8_t *dhp)
{
	long t;
	u_long days, year, month, inc;
	u_short *months;

	/*
	 * If the time from the last conversion is the same as now, then
	 * skip the computations and use the saved result.
	 */
	smb_time_local2server(tsp, tzoff, &t);
	t &= ~1;
	if (lasttime != t) {
		lasttime = t;
		if (t < 0) {
			/*
			 * This is before 1970, so it's before 1980,
			 * and can't be represented as a DOS time.
			 * Just represent it as the DOS epoch.
			 */
			lastdtime = 0;
			lastddate = (1 << DD_DAY_SHIFT)
			    + (1 << DD_MONTH_SHIFT)
			    + ((1980 - 1980) << DD_YEAR_SHIFT);
		} else {
			lastdtime = (((t / 2) % 30) << DT_2SECONDS_SHIFT)
			    + (((t / 60) % 60) << DT_MINUTES_SHIFT)
			    + (((t / 3600) % 24) << DT_HOURS_SHIFT);

			/*
			 * If the number of days since 1970 is the same as
			 * the last time we did the computation then skip
			 * all this leap year and month stuff.
			 */
			days = t / (24 * 60 * 60);
			if (days != lastday) {
				lastday = days;
				for (year = 1970;; year++) {
					/*
					 * XXX - works in 2000, but won't
					 * work in 2100.
					 */
					inc = year & 0x03 ? 365 : 366;
					if (days < inc)
						break;
					days -= inc;
				}
				/*
				 * XXX - works in 2000, but won't work in 2100.
				 */
				months = year & 0x03 ? regyear : leapyear;
				for (month = 0; days >= months[month]; month++)
					;
				if (month > 0)
					days -= months[month - 1];
				lastddate = ((days + 1) << DD_DAY_SHIFT)
				    + ((month + 1) << DD_MONTH_SHIFT);
				/*
				 * Remember DOS's idea of time is relative
				 * to 1980, but UN*X's is relative to 1970.
				 * If somehow we get a time before 1980 then
				 * don't give totally crazy results.
				 */
				if (year > 1980)
					lastddate += (year - 1980) << DD_YEAR_SHIFT;
			}
		}
	}
	if (dtp)
		*dtp = lastdtime;
	if (dhp)
		*dhp = (tsp->tv_sec & 1) * 100 + tsp->tv_nsec / 10000000;

	*ddp = lastddate;
}

/*
 * The number of seconds between Jan 1, 1970 and Jan 1, 1980. In that
 * interval there were 8 regular years and 2 leap years.
 */
#define	SECONDSTO1980	(((8 * 365) + (2 * 366)) * (24 * 60 * 60))

static u_short lastdosdate;
static u_long  lastseconds;

void
smb_dos2unixtime(u_int dd, u_int dt, u_int dh, int tzoff,
	struct timespec *tsp)
{
	u_long seconds;
	u_long month;
	u_long year;
	u_long days;
	u_short *months;

	if (dd == 0) {
		tsp->tv_sec = 0;
		tsp->tv_nsec = 0;
		return;
	}
	seconds = (((dt & DT_2SECONDS_MASK) >> DT_2SECONDS_SHIFT) << 1)
	    + ((dt & DT_MINUTES_MASK) >> DT_MINUTES_SHIFT) * 60
	    + ((dt & DT_HOURS_MASK) >> DT_HOURS_SHIFT) * 3600
	    + dh / 100;
	/*
	 * If the year, month, and day from the last conversion are the
	 * same then use the saved value.
	 */
	if (lastdosdate != dd) {
		lastdosdate = dd;
		days = 0;
		year = (dd & DD_YEAR_MASK) >> DD_YEAR_SHIFT;
		days = year * 365;
		days += year / 4 + 1;	/* add in leap days */
		/*
		 * XXX - works in 2000, but won't work in 2100.
		 */
		if ((year & 0x03) == 0)
			days--;		/* if year is a leap year */
		months = year & 0x03 ? regyear : leapyear;
		month = (dd & DD_MONTH_MASK) >> DD_MONTH_SHIFT;
		if (month < 1 || month > 12) {
			month = 1;
		}
		if (month > 1)
			days += months[month - 2];
		days += ((dd & DD_DAY_MASK) >> DD_DAY_SHIFT) - 1;
		lastseconds = (days * 24 * 60 * 60) + SECONDSTO1980;
	}
	smb_time_server2local(seconds + lastseconds, tzoff, tsp);
	tsp->tv_nsec = (dh % 100) * 10000000;
}

static int
smb_fphelp(struct mbchain *mbp, struct smb_vc *vcp, struct smbnode *np,
	int caseopt, int *lenp)
{
        struct smbnode  *npstack[SMBFS_MAXPATHCOMP]; 
        struct smbnode  **npp = &npstack[0]; 
	int i, error = 0;

	i = 0;
	while (np->n_parent) {
		if (i++ == SMBFS_MAXPATHCOMP)
			return ENAMETOOLONG;
		*npp++ = np;
		np = np->n_parent;
	}
	while (i--) {
		np = *--npp;
		if (SMB_UNICODE_STRINGS(vcp))
			error = mb_put_uint16le(mbp, '\\');
		else
			error = mb_put_uint8(mbp, '\\');
                if (!error && lenp)
                        *lenp += SMB_UNICODE_STRINGS(vcp) ? 2 : 1;
		if (error)
			break;
		error = smb_put_dmem(mbp, vcp, np->n_name, np->n_nmlen,
				     caseopt, lenp);
		if (error)
			break;
	}
	return error;
}

int
smbfs_fullpath(struct mbchain *mbp, struct smb_vc *vcp, struct smbnode *dnp,
	const char *name, int *lenp, u_int8_t sep)
{
	int caseopt = SMB_CS_NONE;
	int error, len = 0;

        if (lenp) {
                len = *lenp;
                *lenp = 0;
        }
	if (SMB_UNICODE_STRINGS(vcp)) {
		error = mb_put_padbyte(mbp);
		if (error)
			return error;
	}
	if (SMB_DIALECT(vcp) < SMB_DIALECT_LANMAN1_0)
		caseopt |= SMB_CS_UPPER;
	if (dnp != NULL) {
		error = smb_fphelp(mbp, vcp, dnp, caseopt, lenp);
		if (error)
			return error;
		if (dnp->n_ino == 2 && !name)
			name = ""; /* to get one backslash below */
	}
	if (name) {
		if (SMB_UNICODE_STRINGS(vcp))
			error = mb_put_uint16le(mbp, sep);
		else
			error = mb_put_uint8(mbp, sep);
                if (!error && lenp)
                        *lenp += SMB_UNICODE_STRINGS(vcp) ? 2 : 1;
		if (error)
			return error;
		error = smb_put_dmem(mbp, vcp, name, len, caseopt, lenp);
		if (error)
			return error;
	}
	error = mb_put_uint8(mbp, 0);
        if (!error && lenp)
                *lenp++;
	if (SMB_UNICODE_STRINGS(vcp) && error == 0) {
		error = mb_put_uint8(mbp, 0);
                if (!error && lenp)
                        *lenp++;
	}
	return error;
}

void
smbfs_fname_tolocal(struct smbfs_fctx *ctx)
{
	int length;
	struct smb_vc *vcp = SSTOVC(ctx->f_ssp);
	char *dst, *odst;
	const char *src;
	size_t inlen, outlen;

	if (ctx->f_nmlen == 0)
		return;
	if (vcp->vc_tolocal == NULL)
		return;
	/*
	 * In Mac OS X the local name can be larger and
	 * in-place conversions are not supported.
	 */
	if (SMB_UNICODE_STRINGS(vcp))
		length = ctx->f_nmlen * 9; /* why 9 */
	else
		length = ctx->f_nmlen * 3; /* why 3 */
	length = max(length, SMB_MAXFNAMELEN);

	dst = malloc(length, M_SMBFSDATA, M_WAITOK);
	outlen = length;
	src = ctx->f_name;
	inlen = ctx->f_nmlen;
	if (iconv_conv(vcp->vc_tolocal, NULL, NULL, &dst, &outlen) == 0) {
		odst = dst;
		(void) iconv_conv(vcp->vc_tolocal, &src, &inlen, &dst, &outlen);
		if (ctx->f_name)
			free(ctx->f_name, M_SMBFSDATA);
		ctx->f_name = odst;
		ctx->f_nmlen = length - outlen;
	} else
		free(dst, M_SMBFSDATA);
	return;
}


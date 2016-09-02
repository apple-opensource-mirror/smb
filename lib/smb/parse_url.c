/*
 * Copyright (c) 2006 - 2007 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#include <CoreFoundation/CoreFoundation.h>
#include <asl.h>
#include <netsmb/smb_lib.h>
#include <charsets.h>
#include <parse_url.h>
#include <netsmb/smb_conn.h>
#include <URLMount/URLMount.h>

#define CIFS_SCHEME_LEN 5
#define SMB_SCHEME_LEN 4

static void LogCFString(CFStringRef theString, const char *debugstr, const char * func, int lineNum)
{
	char prntstr[1024];
	
	if (theString == NULL)
		return;
	CFStringGetCString(theString, prntstr, 1024, kCFStringEncodingUTF8);
	smb_log_info("%s-line:%d %s = %s", 0, ASL_LEVEL_DEBUG, func, lineNum, debugstr, prntstr);	
}

#ifdef DEBUG
#define DebugLogCFString LogCFString
#else // DEBUG
#define DebugLogCFString(theString, debugstr, func, lineNum)
#endif // DEBUG

/*
 * See if this is a cifs or smb scheme
 *
 * RETURN VALUES:
 *	0	- No Scheme, could still be our scheme
 *	4	- SMB scheme, also the length of smb scheme field.
 *	5	- CIFS scheme, also the length of cifs scheme field.
 *	-1	- Unknown scheme, should be treated as an error.
 */
static int SMBSchemeLength(CFURLRef url)
{
	int len = 0;
	CFStringRef scheme = CFURLCopyScheme (url);

	if (scheme == NULL)
		return 0;
	
	if ( kCFCompareEqualTo == CFStringCompare (scheme, CFSTR("smb"), kCFCompareCaseInsensitive) ) 
		len = SMB_SCHEME_LEN;	/* Length of "smb:" */
	else if ( kCFCompareEqualTo == CFStringCompare (scheme, CFSTR("cifs"), kCFCompareCaseInsensitive) ) 
		len = CIFS_SCHEME_LEN;	/* Length of "cifs:" */
	else
		len = -1;
	CFRelease(scheme);
	return len;
}

/* 
 * This routine will percent escape out the input string by making a copy. The CreateStringByReplacingPercentEscapesUTF8
 * routine will either return a copy or take a retain on the original string.
 *
 * NOTE:  We always use kCFStringEncodingUTF8 and kCFAllocatorDefault.
 */
static CFStringRef CreateStringByReplacingPercentEscapesUTF8(CFStringRef inStr, CFStringRef LeaveEscaped)
{
	CFStringRef outStr;
	
	if (!inStr)	/* Just a safety check */
		return NULL;
	
	outStr = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(NULL, inStr, LeaveEscaped, kCFStringEncodingUTF8);
	CFRelease(inStr);	/* We always want to release the inStr */
	return outStr;
}

/* 
 * This routine will  percent escape the input string by making a copy. The CreateStringByAddingPercentEscapesUTF8
 * routine will either return a copy or take a retain on the original string. If doRelease is set then we do
 * a release on the inStr.
 *
 * NOTE:  We always use kCFStringEncodingUTF8 and kCFAllocatorDefault.
 */
static CFStringRef CreateStringByAddingPercentEscapesUTF8(CFStringRef inStr, CFStringRef leaveUnescaped, CFStringRef toBeEscaped, Boolean doRelease)
{
	CFStringRef outStr;
	
	if (!inStr)	/* Just a safety check */
		return NULL;
		
	outStr = CFURLCreateStringByAddingPercentEscapes(NULL, inStr, leaveUnescaped, toBeEscaped, kCFStringEncodingUTF8);
	if (doRelease)
		CFRelease(inStr);
	return outStr;
}

static CFArrayRef CreateWrkgrpUserArrayFromCFStringRef(CFStringRef inString, CFStringRef separatorString)
{
	CFArrayRef	userArray = NULL;

	userArray = CFStringCreateArrayBySeparatingStrings(kCFAllocatorDefault, inString, separatorString);
	/* 
	 * If there are two array entries then we have a workgoup and username otherwise if we just have one item then its a 
	 * username. Any other number could be an error, but since we have no idea what they are trying to do we just treat
	 * it as a username.
	 */
	if (userArray && (CFArrayGetCount(userArray) != 2)) {
		CFRelease(userArray);
		userArray = NULL;
	}
	return userArray;
	
}

/* 
 * Check to see if there is a workgroup/domain in the URL. If yes then return an array
 * with the first element as the workgroup and the second element containing the
 * username and server name. If no workgroup just return NULL.
 */
static CFArrayRef CreateWrkgrpUserArray(CFURLRef url)
{
	CFStringRef netlocation = NULL;
	CFArrayRef	userArray = NULL;
	
	netlocation = CFURLCopyNetLocation(url);
	if (!netlocation)
		return NULL;
	
	userArray = CreateWrkgrpUserArrayFromCFStringRef(netlocation, CFSTR(";"));
	CFRelease(netlocation);
	return userArray;
}


/* 
 * Get the server name out of the URL. CFURLCopyHostName will escape out the server name
 * for us. So just convert it to the correct code page encoding.
 *
 * Note: Currently we put the server name into a c-style string. In the future it would be 
 * nice to keep this as a CFString.
 */
static int SetServerFromURL(struct smb_ctx *ctx, CFURLRef url)
{
	int maxlen;
	
	/* The serverDisplayName contains the URL host name or the Bonjour Name */
	if (ctx->serverDisplayName)
		CFRelease(ctx->serverDisplayName);
	ctx->serverDisplayName = CFURLCopyHostName(url);
	if (ctx->serverDisplayName == NULL)
		return EINVAL;
	LogCFString(ctx->serverDisplayName, "Server", __FUNCTION__, __LINE__);
	/* 
	 * We always uppercase the server name, needed for NetBIOS names and DNS doesn't care. The old code
	 * always uppercase so we will to, someday we may only want to do this for NetBIOS names.
	 *
	 * CFStringUppercase requires a CFMutableStringRef and CFURLCopyHostName returns CFStringRef. This would 
	 * require an extra allocate just to uppercase, yuk. So for now just use toupper. 
	 *
	 * CFStringUppercase(ctx->serverDisplayName, NULL);
	 */
	
	maxlen = CFStringGetMaximumSizeForEncoding(CFStringGetLength(ctx->serverDisplayName), kCFStringEncodingUTF8) + 1;
	if (ctx->ct_fullserver)
		free(ctx->ct_fullserver);
	ctx->ct_fullserver = malloc(maxlen);
	if (!ctx->ct_fullserver) {
		CFRelease(ctx->serverDisplayName);
		ctx->serverDisplayName = NULL;
		return ENOMEM;
	}
	CFStringGetCString(ctx->serverDisplayName, ctx->ct_fullserver, maxlen, kCFStringEncodingUTF8);
	/* Test here with several system and none complain about the server name not being uppercased */
#ifdef TIGER_OLD_WAY
	/* May need to look at this not sure if we can trust this uppercase, since we already escaped out the string */
	str_upper(ctx->ct_fullserver, ctx->ct_fullserver);
#endif // TIGER_OLD_WAY
	/* May want to change this in the future, but this seems the best place for it. */
	smb_ctx_setserver(ctx, ctx->ct_fullserver);
	return 0;
}

/* 
 * Get the user and workgroup names and return them in CFStringRef. 
 * First get the CFURLCopyNetLocation because it will not escape out the string.
 */
static CFStringRef GetUserAndWorkgroupFromURL(CFStringRef *out_workgrp, CFURLRef url)
{
	CFURLRef net_url = NULL;;
	CFArrayRef	userArray = NULL;
	CFStringRef userString = NULL;
	CFMutableStringRef urlString = NULL;
	CFStringRef wrkgrpString = NULL;
	
	*out_workgrp = NULL;	/* Always start like we didn't get one. */
	/* This will return null if no workgroup in the URL */
	userArray = CreateWrkgrpUserArray(url);
	if (!userArray)	/* We just have a username name  */
		return(CFURLCopyUserName(url));	/* This will escape out the character for us. */
	
	/* Now for the hard part; netlocation contains one of the following:
	 *
	 * URL = "//workgroup;username:password@smb-win2003.apple.com"
	 * URL = "//workgroup;username:@smb-win2003.apple.com"
	 * URL = "//workgroup;username@smb-win2003.apple.com"
	 * URL = "//workgroup;@smb-win2003.apple.com"
	 */
	/* Get the username first */
	urlString = CFStringCreateMutableCopy(NULL, 1024, CFSTR("//"));
	if (!urlString) {
		CFRelease(userArray);
		return(CFURLCopyUserName(url));	/* This will escape out the character for us. */
	}
	CFStringAppend(urlString, (CFStringRef)CFArrayGetValueAtIndex(userArray, 1));
	net_url = CFURLCreateWithString(NULL, urlString, NULL);
	CFRelease(urlString);
	urlString = NULL;
	/* Not sure what to do if we fail here */
	if (!net_url) {
		CFRelease(userArray);
		return(CFURLCopyUserName(url));	/* This will escape out the character for us. */		
	}
	/* We now have a URL without the workgroup name, just copy out the username. */
	userString = CFURLCopyUserName(net_url);
	CFRelease(net_url);
	
	/* Now get the workgroup */
	wrkgrpString = CFStringCreateCopy(NULL, (CFStringRef)CFArrayGetValueAtIndex(userArray, 0));
	wrkgrpString = CreateStringByReplacingPercentEscapesUTF8(wrkgrpString, CFSTR(""));
	if (wrkgrpString)
		*out_workgrp = wrkgrpString;	/* We have the workgroup return it to the calling routine */
	CFRelease(userArray);
	return(userString);
}

/* 
 * Get the workgroup and return a username CFStringRef if it exist.
 * First get the CFURLCopyNetLocation because it will not escape out the string.
 */
static CFStringRef SetWorkgroupFromURL(struct smb_ctx *ctx, CFURLRef url)
{
	CFStringRef userString = NULL;
	CFStringRef wrkgrpString = NULL;

	userString = GetUserAndWorkgroupFromURL(&wrkgrpString, url);

	if (wrkgrpString) {
		LogCFString(wrkgrpString, "Workgroup", __FUNCTION__, __LINE__);
		/*
		 * CFStringUppercase requires a CFMutableStringRef and CFURLCopyNetLocation returns CFStringRef. This would 
		 * require an extra allocate just to uppercase, yuk. So for now just use toupper. 
		 *
		 * CFStringUppercase(wrkgrpString, NULL);
		 */
		if (CFStringGetLength(wrkgrpString) < SMB_MAXNetBIOSNAMELEN) {
			/* 
			 * Radar 4526951: The old parsing code did not use the windows encoding for the workgroup name, but it did 
			 * for the server name. Not sure why you would do it for one and not the other. So we use the Windows
			 * encoding here.
			 */
			CFStringGetCString(wrkgrpString, ctx->ct_ssn.ioc_domain, SMB_MAXNetBIOSNAMELEN, kCFStringEncodingUTF8);
			str_upper(ctx->ct_ssn.ioc_domain, ctx->ct_ssn.ioc_domain);
		}
		CFRelease(wrkgrpString);
	}
	return(userString);
}

/* 
 * Need to call SetWorkgroupFromURL just in case we have a workgroup name. CFURL does not handle
 * a CIFS style URL with a workgroup name.
 */
static int SetUserNameFromURL(struct smb_ctx *ctx, CFURLRef url)
{
	CFStringRef username = SetWorkgroupFromURL(ctx, url);
	int error;
	
	/* No user name in the URL */
	if (! username)
		return 0;
	LogCFString(username, "Username",__FUNCTION__, __LINE__);
	/* May still have the special URL percent escape characters, remove any that exist. */ 
	username = CreateStringByReplacingPercentEscapesUTF8(username, CFSTR(""));

	/* Username is too long return an error */
	if (CFStringGetLength(username) >= SMB_MAXUSERNAMELEN) {
		CFRelease(username);
		return ENAMETOOLONG;
	}
	CFStringGetCString(username, ctx->ct_ssn.ioc_user, SMB_MAXUSERNAMELEN, kCFStringEncodingUTF8);
	error = smb_ctx_setuser(ctx, ctx->ct_ssn.ioc_user);
	CFRelease(username);
	return error;
}

/* 
 * The URL may contain no password, an empty password, or a password. An empty password is a passowrd
 * and should be treated the same as a password. This is need to make guest access work.
 *
 *	URL "smb://username:password@server/share" should set the password.
 *	URL "smb://username:@server/" should set the password.
 *	URL "smb://username@server/share" should not set the password.
 *	URL "smb://server/share/path" should not set the password.
 *
 */
static int SetPasswordFromURL(struct smb_ctx *ctx, CFURLRef url)
{
	CFStringRef passwd = CFURLCopyPassword(url);
	
	/*  URL =" //username@smb-win2003.apple.com" or URL =" //smb-win2003.apple.com" */
	if (! passwd)
		return 0;
	
	/* May still have the special URL percent escape characters, remove any that exist. */ 
	passwd = CreateStringByReplacingPercentEscapesUTF8(passwd, CFSTR(""));

	/* Password is too long return an error */	
	if (CFStringGetLength(passwd) >= SMB_MAXPASSWORDLEN) {
		CFRelease(passwd);
		return ENAMETOOLONG;
	}
	/* 
	 * Works for password and empty password
	 *
	 * URL = "//username:password@smb-win2003.apple.com"
	 * URL = "//username:@smb-win2003.apple.com"
	 */
	CFStringGetCString(passwd, ctx->ct_ssn.ioc_password, SMB_MAXPASSWORDLEN, kCFStringEncodingUTF8);
	strlcpy(ctx->ct_sh.ioc_password, ctx->ct_ssn.ioc_password, SMB_MAXPASSWORDLEN);
	ctx->ct_flags |= SMBCF_EXPLICITPWD;
	CFRelease(passwd);
	return 0;
}

/* 
 * If URL contains a port then we should get it and set the correct flag.
 *
 *	URL "smb://username:password@server:445/share" set the port to 445.
 *
 */
static void SetPortNumberFromURL(struct smb_ctx *ctx, CFURLRef url)
{
	SInt32 port = CFURLGetPortNumber(url);
	
	/* No port defined in the URL */
	if (port == -1)
		return;
	/* They supplied a port number use it and only it */
	ctx->ct_port = port;
	ctx->ct_port_behavior = USE_THIS_PORT_ONLY;
	smb_log_info("Setting port number to %d", 0, ASL_LEVEL_DEBUG, ctx->ct_port);	
}

/*
 * We need to separate the share name and any path component from the URL.
 *	URL "smb://username:password@server" no share name or path.
 *	URL "smb://username:password@server/"no share name or path.
 *	URL "smb://username:password@server/share" just a share name.
 *	URL "smb://username:password@server/share/path" share name and path.
 *
 * The Share name and Path name will not begin with a slash.
 *		smb://server/ntfs  share = ntfs path = NULL
 *		smb://ntfs/dir1/dir2  share = ntfs path = dir1/dir2
 */
static int GetShareAndPathFromURL(CFURLRef url, CFStringRef *out_share, CFStringRef *out_path)
{
	Boolean isAbsolute;
	CFArrayRef userArray = NULL;
	CFMutableArrayRef userArrayM = NULL;
	CFStringRef share = CFURLCopyStrictPath(url, &isAbsolute);
	CFStringRef path = NULL;
	
	*out_share = NULL;
	*out_path = NULL;
	/* We have an empty share treat it like no share */
	if (share && (CFStringGetLength(share) == 0)) {
		CFRelease(share);	
		share = NULL;
	}
	/* Since there is no share name we have nothing left to do. */
	if (!share)
		return 0;
	
	userArray = CFStringCreateArrayBySeparatingStrings(NULL, share, CFSTR("/"));
	if (userArray && (CFArrayGetCount(userArray) > 1))
		userArrayM = CFArrayCreateMutableCopy(NULL, CFArrayGetCount(userArray), userArray);
	
	if (userArray)
		CFRelease(userArray);
	
	if (userArrayM) {
		CFStringRef newshare;	/* Just in case something goes wrong */
		
		newshare = CFStringCreateCopy(NULL, (CFStringRef)CFArrayGetValueAtIndex(userArrayM, 0));
		newshare = CreateStringByReplacingPercentEscapesUTF8(newshare, CFSTR(""));
		CFArrayRemoveValueAtIndex(userArrayM, 0);
		path = CFStringCreateByCombiningStrings(NULL, userArrayM, CFSTR("/"));
		path = CreateStringByReplacingPercentEscapesUTF8(path, CFSTR(""));
		LogCFString(path, "Path", __FUNCTION__, __LINE__);

		CFRelease(userArrayM);
		/* Something went wrong use the original value */
		if (newshare) {
			CFRelease(share);
			share = newshare;
		}
	} else
		share = CreateStringByReplacingPercentEscapesUTF8(share, CFSTR(""));
	/* 
	 * The above routines will not un-precent escape out slashes. We only allow for the cases
	 * where the share name is a single slash. Slashes are treated as delemiters in the path name.
	 * So if the share name has a single 0x2f then make it a slash. This means you can't have
	 * a share name whos name is 0x2f, not likley to happen.
	 */
	if ( kCFCompareEqualTo == CFStringCompare (share, CFSTR("0x2f"), kCFCompareCaseInsensitive) ) {
		CFRelease(share);
		share = CFStringCreateCopy(NULL, CFSTR("/"));		
	}

	
	if (CFStringGetLength(share) >= SMB_MAXSHARENAMELEN) {
		CFRelease(share);
		if (path)
			CFRelease(path);
		return ENAMETOOLONG;
	}
	
	*out_share = share;
	*out_path = path;
	return 0;
}

/*
 * We need to separate the share name and any path component from the URL.
 *	URL "smb://username:password@server" no share name or path.
 *	URL "smb://username:password@server/"no share name or path.
 *	URL "smb://username:password@server/share" just a share name.
 *	URL "smb://username:password@server/share/path" share name and path.
 *
 * The Share name and Path name will not begin with a slash.
 *		smb://server/ntfs  share = ntfs path = NULL
 *		smb://ntfs/dir1/dir2  share = ntfs path = dir1/dir2
 */
static int SetShareAndPathFromURL(struct smb_ctx *ctx, CFURLRef url, int sharetype)
{
	CFStringRef share = NULL;
	CFStringRef path = NULL;
	int maxlen;
	int error;

	error = GetShareAndPathFromURL(url, &share, &path);
	if (error)
		return error;
	
	/* No share but a share is required */
	if ((!share) && (ctx->ct_level >= SMBL_SHARE)) {
		smb_log_info("The URL does not contain a share name", EINVAL, ASL_LEVEL_ERR);
		return EINVAL;		
	}
	/* Since there is no share name we have nothing left to do. */
	if (!share)
		return 0;
	LogCFString(share, "Share", __FUNCTION__, __LINE__);
	
	if (ctx->ct_origshare)
		free(ctx->ct_origshare);
	
	maxlen = CFStringGetMaximumSizeForEncoding(CFStringGetLength(share), kCFStringEncodingUTF8) + 1;
	ctx->ct_origshare = malloc(maxlen);
	if (!ctx->ct_origshare) {
		CFRelease(share);		
		CFRelease(path);
		return ENOMEM;
	}
	CFStringGetCString(share, ctx->ct_origshare, maxlen, kCFStringEncodingUTF8);
	str_upper(ctx->ct_sh.ioc_share, ctx->ct_origshare); 
	ctx->ct_sh.ioc_stype = sharetype;
	CFRelease(share);
	
	if (path) {
		maxlen = CFStringGetMaximumSizeForEncoding(CFStringGetLength(path), kCFStringEncodingUTF8) + 1;
		ctx->ct_path = malloc(maxlen);
		if (ctx->ct_path)
			CFStringGetCString(path, ctx->ct_path, maxlen, kCFStringEncodingUTF8);
		CFRelease(path);
	}
	return 0;
}

/*
 * Here we expect something like
 *   "//[workgroup;][user[:password]@]host[/share[/path]]"
 * See http://ietf.org/internet-drafts/draft-crhertel-smb-url-07.txt
 */
int ParseSMBURL(struct smb_ctx *ctx, int sharetype)
{
	int error  = EINVAL;

	/* Make sure its a good URL, better be at this point */
	if ((!CFURLCanBeDecomposed(ctx->ct_url)) || (SMBSchemeLength(ctx->ct_url) < 0)) {
		smb_log_info("This is an invalid URL", error, ASL_LEVEL_ERR);
		return error;
	}

	error = SetServerFromURL(ctx, ctx->ct_url);
	if (error) {
		smb_log_info("The URL has a bad server name", error, ASL_LEVEL_ERR);
		return error;
	}
	error = SetUserNameFromURL(ctx, ctx->ct_url);
	if (error) {
		smb_log_info("The URL has a bad user name", error, ASL_LEVEL_ERR);
		return error;
	}
	error = SetPasswordFromURL(ctx, ctx->ct_url);
	if (error) {
		smb_log_info("The URL has a bad password", error, ASL_LEVEL_ERR);
		return error;
	}
	SetPortNumberFromURL(ctx, ctx->ct_url);
	error = SetShareAndPathFromURL(ctx, ctx->ct_url, sharetype);
	/* CFURLCopyQueryString to get ?WINS=msfilsys.apple.com;NODETYPE=H info */
	return error;
}

/*
 * Given a c-style string create a CFURL.
 * We always assume these are smb/cifs style URLs.
 */
int CreateSMBURL(struct smb_ctx *ctx, const char *url)
{
	CFStringRef urlString = CFStringCreateWithCString(NULL, url, kCFStringEncodingUTF8);

	DebugLogCFString(urlString, "urlString ", __FUNCTION__, __LINE__);

	if (urlString) {
		ctx->ct_url = CFURLCreateWithString(kCFAllocatorDefault, urlString, NULL);
		CFRelease(urlString);	/* We create it now release it */
	}
	if (ctx->ct_url == NULL)
		return EINVAL;
	else
		return 0;
}

/* 
 * Given a url parse it and place the component in a dictionary we create.
 */
int smb_url_to_dictionary(CFURLRef url, CFDictionaryRef *dict)
{
	CFMutableDictionaryRef mutableDict = NULL;
	int error  = 0;
	CFStringRef Server = NULL;
	CFStringRef Username = NULL;
	CFStringRef DomainWrkgrp = NULL;
	CFStringRef Password = NULL;
	CFStringRef Share = NULL;
	CFStringRef Path = NULL;
	SInt32 PortNumber;
	
	/* Make sure its a good URL, better be at this point */
	if ((!CFURLCanBeDecomposed(url)) || (SMBSchemeLength(url) < 0)) {
		smb_log_info("%s: Invalid URL!", EINVAL, ASL_LEVEL_ERR, __FUNCTION__);	
		goto ErrorOut;
	}
	
	/* create and return the server parameters dictionary */
	mutableDict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, 
												&kCFTypeDictionaryValueCallBacks);
	if (mutableDict == NULL) {
		error = errno;
		smb_log_info("%s: CFDictionaryCreateMutable failed!", error, ASL_LEVEL_ERR, __FUNCTION__);	
		goto ErrorOut;
	}
	
	/*
	 * SMB can have two different schema's cifs or smb. When we made SMBSchemeLength call at the
	 * start of this routine it made sure we had one or the other schema. Always default here to
	 * the SMB schema.
	 */
	CFDictionarySetValue (mutableDict, kSchemaKey, CFSTR(SMB_SCHEMA_STRING));
	
	Server = CFURLCopyHostName(url);
	if (! Server)
		goto ErrorOut; /* Server name is required */		
	LogCFString(Server, "Server String", __FUNCTION__, __LINE__);

	CFDictionarySetValue (mutableDict, kHostKey, Server);
	CFRelease(Server);
	
	PortNumber = CFURLGetPortNumber(url);
	if (PortNumber != -1) {
		CFStringRef tempString = CFStringCreateWithFormat( NULL, NULL, CFSTR( "%d" ), PortNumber );
		if (tempString) {
			CFDictionarySetValue (mutableDict, kAlternatePortKey, tempString);
			CFRelease(tempString);
		}
	}
	
	Username = GetUserAndWorkgroupFromURL(&DomainWrkgrp, url);
	LogCFString(Username, "Username String", __FUNCTION__, __LINE__);
	LogCFString(DomainWrkgrp, "DomainWrkgrp String", __FUNCTION__, __LINE__);
	error = 0;
	if ((Username) && (CFStringGetLength(Username) >= SMB_MAXUSERNAMELEN))
		error = ENAMETOOLONG;

	if ((DomainWrkgrp) && (CFStringGetLength(DomainWrkgrp) >= SMB_MAXNetBIOSNAMELEN))
		error = ENAMETOOLONG;
	
	if (error)
		goto ErrorOut; /*Username or Domain name is too long */
	
	/* We have a domain name so combined it with the user name so it can be display to the user. */
	if (DomainWrkgrp) {
		CFMutableStringRef tempString = CFStringCreateMutableCopy(NULL, 0, DomainWrkgrp);
		
		if (tempString) {
			CFStringAppend(tempString, CFSTR("\\"));
			CFStringAppend(tempString, Username);
			CFRelease(Username);
			Username = tempString;
		}
		CFRelease(DomainWrkgrp);
	} 

	if (Username)
	{
		CFDictionarySetValue (mutableDict, kUserNameKey, Username);
		CFRelease(Username);				
	}	

	Password = CFURLCopyPassword(url);
	if (Password) {
		if (CFStringGetLength(Password) >= SMB_MAXPASSWORDLEN)
			error = ENAMETOOLONG;
		CFDictionarySetValue (mutableDict, kPasswordKey, Password);
		CFRelease(Password);		
	}
	
	/*
	 * We used to keep the share and path as two different elements in the dictionary. This was
	 * changed to satisfy URLMount and other plugins. We still need to check and make sure the
	 * share and path are correct. So now split them apart and then put them put them back together.
	 */
	error = GetShareAndPathFromURL(url, &Share, &Path);
	if (error)
		goto ErrorOut; /* Share name is too long */
	
	LogCFString(Share, "Share String", __FUNCTION__, __LINE__);
	LogCFString(Path, "Path String", __FUNCTION__, __LINE__);

	if (Share && Path) {
		CFMutableStringRef tempString = CFStringCreateMutableCopy(NULL, 0, Share);
		if (tempString) {
			CFStringAppend(tempString, CFSTR("/"));
			CFStringAppend(tempString, Path);
			CFDictionarySetValue (mutableDict, kPathKey, tempString);
			CFRelease(tempString);		
			CFRelease(Share);		
			Share = NULL;
		} 
	}
	
	if (Share) {
		CFDictionarySetValue (mutableDict, kPathKey, Share);
		CFRelease(Share);		
	}
	
	if (Path) 
		CFRelease(Path);		

	*dict = mutableDict;
	return 0;
		
ErrorOut:
		
	*dict = NULL;
	if (mutableDict)
		CFRelease(mutableDict);
	if (!error)	/* No error set it to the default error */
		error = EINVAL;
	return error;
	
}

/* 
 * Given a dictionary create a url string. We assume that the dictionary has any characters that need to
 * be escaped out escaped out.
 */
static int smb_dictionary_to_urlstring(CFDictionaryRef dict, CFMutableStringRef *urlReturnString, Boolean escapeShare)
{
	int error  = 0;
	CFMutableStringRef urlStringM = NULL;
	CFStringRef DomainWrkgrp = NULL;
	CFStringRef Username = NULL;
	CFStringRef Password = NULL;
	CFStringRef Server = NULL;
	CFStringRef PortNumber = NULL;
	CFStringRef Path = NULL;
	Boolean		releaseUsername = FALSE;
	
	urlStringM = CFStringCreateMutableCopy(NULL, 1024, CFSTR("smb://"));
	if (urlStringM == NULL) {
		error = errno;
		smb_log_info("%s: couldn't allocate the url string!", error, ASL_LEVEL_ERR, __FUNCTION__);	
		goto WeAreDone;
	}
	
	/* Get the server name, required value */
	Server = CFDictionaryGetValue(dict, kHostKey);
	/* %%% IP6 SUPPORT SOME DAY: Do not unescape the '[]', IP6 */
	Server = CreateStringByAddingPercentEscapesUTF8(Server, CFSTR("[]"), NULL, FALSE);
	if (Server == NULL) {
		error = EINVAL;
		smb_log_info("%s: no server name!", error, ASL_LEVEL_ERR, __FUNCTION__);	
		goto WeAreDone;
	}
	/* Now get all the other parts of the url. */
	Username = CFDictionaryGetValue(dict, kUserNameKey);
	/* We have a user name see if they entered a domain also. */
	if (Username) {
		CFArrayRef	userArray = NULL;
		/* 
		 * Remember that on windows a back slash is illegal, so if someone wants to use one
		 * in the username they will need to escape percent it out.
		 */
		userArray = CreateWrkgrpUserArrayFromCFStringRef(Username, CFSTR("\\"));
		/* If we have an array then we have a domain\username in the Username String */
		if (userArray) {
			DomainWrkgrp = CFStringCreateCopy(NULL, (CFStringRef)CFArrayGetValueAtIndex(userArray, 0));
			Username = CFStringCreateCopy(NULL, (CFStringRef)CFArrayGetValueAtIndex(userArray, 1));
			CFRelease(userArray);
			releaseUsername = TRUE;
		}
	}

	Password = CFDictionaryGetValue(dict, kPasswordKey);
	Path = CFDictionaryGetValue(dict, kPathKey);
	PortNumber = CFDictionaryGetValue(dict, kAlternatePortKey);

	/* 
	 * Percent escape out any URL special characters, for the username, password, Domain/Workgroup,
	 * path, and port. Not sure the port is required, but AFP does it so why not.
	 * The CreateStringByAddingPercentEscapesUTF8 will return either NULL or a value that must be
	 * released.
	 */
	DomainWrkgrp = CreateStringByAddingPercentEscapesUTF8(DomainWrkgrp, NULL, CFSTR("@:;/?"), TRUE);
	Username = CreateStringByAddingPercentEscapesUTF8(Username, NULL, CFSTR("@:;/?"), releaseUsername);
	Password = CreateStringByAddingPercentEscapesUTF8(Password, NULL, CFSTR("@:;/?"), FALSE);
	if (escapeShare)
		Path = CreateStringByAddingPercentEscapesUTF8(Path, NULL, NULL, FALSE);
	PortNumber = CreateStringByAddingPercentEscapesUTF8(PortNumber, NULL, NULL, FALSE);

	LogCFString(Username, "Username String", __FUNCTION__, __LINE__);
	LogCFString(DomainWrkgrp, "Domain String", __FUNCTION__, __LINE__);
	LogCFString(Path, "Path String", __FUNCTION__, __LINE__);
	LogCFString(PortNumber, "PortNumber String", __FUNCTION__, __LINE__);
	
	/* Add the Domain/Workgroup */
	if (DomainWrkgrp) {
		CFStringAppend(urlStringM, DomainWrkgrp);
		CFStringAppend(urlStringM, CFSTR(";"));
	}
	/* Add the username and password */
	if (Username || Password) {		
		if (Username)
			CFStringAppend(urlStringM, Username);
		if (Password) {
			CFStringAppend(urlStringM, CFSTR(":"));
			CFStringAppend(urlStringM, Password);			
		}
		CFStringAppend(urlStringM, CFSTR("@"));
	}
	
	/* Add the server */	
	CFStringAppend(urlStringM, Server);
	
	/* Add the port number */
	if (PortNumber) {
		CFStringAppend(urlStringM, CFSTR(":"));
		CFStringAppend(urlStringM, PortNumber);
	}
	
	/* Add the share and path */
	if (Path) {
		CFStringAppend(urlStringM, CFSTR("/"));
		/* If the share name is a slash percent escape it out */
		if ( kCFCompareEqualTo == CFStringCompare (Path, CFSTR("/"), kCFCompareCaseInsensitive) ) 
			CFStringAppend(urlStringM, CFSTR("0x2f"));
		else 
			CFStringAppend(urlStringM, Path);
	}
	
	DebugLogCFString(urlStringM, "URL String", __FUNCTION__, __LINE__);
	
WeAreDone:
	if (Username)
		CFRelease(Username);		
	if (Password)
		CFRelease(Password);		
	if (DomainWrkgrp)
		CFRelease(DomainWrkgrp);		
	if (Path && escapeShare)
		CFRelease(Path);		
	if (PortNumber)
		CFRelease(PortNumber);	

	if (error == 0)
		*urlReturnString = urlStringM;
	else if (urlStringM)
		CFRelease(urlStringM);		
		
	return error;
}


/* 
 * Given a dictionary create a url. We assume that the dictionary has any characters that need to
 * be escaped out escaped out.
 */
int smb_dictionary_to_url(CFDictionaryRef dict, CFURLRef *url)
{
	int error;
	CFMutableStringRef urlStringM = NULL;
	
	error = smb_dictionary_to_urlstring(dict, &urlStringM, TRUE);
	/* Ok we have everything we need for the URL now create it. */
	if ((error == 0) && urlStringM) {
		*url = CFURLCreateWithString(NULL, urlStringM, NULL);
		if (*url == NULL)
			error = errno;
	}
	
	if (urlStringM)
		CFRelease(urlStringM);
	if (error)
		smb_log_info("%s: creating the url failed!", error, ASL_LEVEL_ERR, __FUNCTION__);	
		
	return error;
	
}

/*
 * Check to make sure we have the correct user name and share. If we already have
 * both a username and share name then we are done, otherwise get the latest stuff.
 */
static void UpdateDictionaryWithUserAndShare(struct smb_ctx *ctx, CFMutableDictionaryRef mutableDict)
{
	CFStringRef DomainWrkgrp = NULL;
	CFStringRef Username = NULL;
	CFStringRef share = NULL;
	CFMutableStringRef path = NULL;
	
	Username = CFDictionaryGetValue(mutableDict, kUserNameKey);
	share = CFDictionaryGetValue(mutableDict, kPathKey);
	
	/* Everything we need is in the dictionary */
	if (share && Username) 
		return;
	
	/* Add the user name, if we have one */
	if (ctx->ct_ssn.ioc_user[0]) {
		Username = CFStringCreateWithCString(NULL, ctx->ct_ssn.ioc_user, kCFStringEncodingUTF8);
		if (ctx->ct_ssn.ioc_domain[0])	/* They gave us a domain add it */
			DomainWrkgrp = CFStringCreateWithCString(NULL, ctx->ct_ssn.ioc_domain, kCFStringEncodingUTF8);

		/* We have a domain name so combined it with the user name. */
		if (DomainWrkgrp) {
			CFMutableStringRef tempString = CFStringCreateMutableCopy(NULL, 0, DomainWrkgrp);
			
			if (tempString) {
				CFStringAppend(tempString, CFSTR("\\"));
				CFStringAppend(tempString, Username);
				CFRelease(Username);
				Username = tempString;
			}
			CFRelease(DomainWrkgrp);
		} 
		if (Username)
		{
			CFDictionarySetValue (mutableDict, kUserNameKey, Username);
			CFRelease(Username);				
		}	
	}
	/* if we have a share then we are done */
	if (share || !ctx->ct_origshare)
		return;
	
	path = CFStringCreateMutable(NULL, 1024);
	/* Should never happen, but just to be safe */
	if (!path)
		return;
	
	CFStringAppendCString(path, ctx->ct_origshare, kCFStringEncodingUTF8);
	/* Add the path if we have one */
	if (ctx->ct_path) {
		CFStringAppend(path, CFSTR("/"));
		CFStringAppendCString(path, ctx->ct_path, kCFStringEncodingUTF8);		
	}
	CFDictionarySetValue (mutableDict, kPathKey, path);
	CFRelease(path);
}

/*
 * We need to create the from name. The from name is just a URL without the scheme. We 
 * never put the password in the from name, but if they have an empty password then we
 * need to make sure that it's included.
 *
 * Examples: 
 *	URL "smb://username:@server/share" - Empty password, just remove the scheme.
 *	URL "smb://username:password@server/share" - Need to remove the password and the scheme.
 *	URL "smb://username@server" - Need to add the share and remove the scheme.
 *	URL "smb://server" - Need to add the username and share and remove the scheme.
 *	URL "smb://server/share/path" - Need to add the usernameand remove the scheme.
 */
void CreateSMBFromName(struct smb_ctx *ctx, char *fromname, int maxlen)
{
	CFMutableStringRef urlStringM = NULL;
	CFMutableStringRef newUrlStringM = NULL;
	CFMutableDictionaryRef mutableDict = NULL;
	CFStringRef Password = NULL;
	int SchemeLength = 0;
	int error = 0;;
	
	/* Always start with the original url and a cleaned out the from name */
	bzero(fromname, maxlen);
	SchemeLength = SMBSchemeLength(ctx->ct_url);
	urlStringM = CFStringCreateMutableCopy(NULL, 0, CFURLGetString(ctx->ct_url));
	if (urlStringM == NULL) {
		smb_log_info("Failed creating URL string?", -1, ASL_LEVEL_ERR);
		return;
	}
	
	error = smb_url_to_dictionary(ctx->ct_url, (CFDictionaryRef *)&mutableDict);
	if (error || (mutableDict == NULL)) {
		smb_log_info("Failed parsing URL!", error, ASL_LEVEL_DEBUG);
		goto WeAreDone;
	}
	UpdateDictionaryWithUserAndShare(ctx, mutableDict);
	
	Password = CFDictionaryGetValue(mutableDict, kPasswordKey);
	/* 
	 * If there is a password and its not an empty password then remove it. Never
	 * show the password in the mount from name.
	 */
	if (Password && (CFStringGetLength(Password) > 0)) {
		CFDictionaryRemoveValue(mutableDict, kPasswordKey);
	}
	/* Guest access has an empty password. */
	if (ctx->ct_ssn.ioc_opt & SMBV_GUEST_ACCESS)
		CFDictionarySetValue (mutableDict, kPasswordKey, CFSTR(""));

	/* Recreate the URL from our new dictionary */
	error = smb_dictionary_to_urlstring(mutableDict, &newUrlStringM, FALSE);
	if (error || (newUrlStringM == NULL)) {
		smb_log_info("Failed parsing dictionary!", error, ASL_LEVEL_DEBUG);
		goto WeAreDone;	
	}
	CFRelease(urlStringM);
	urlStringM = newUrlStringM;
	newUrlStringM = NULL;
	/* smb_dictionary_to_urlstring always uses the SMB scheme */
	SchemeLength = SMB_SCHEME_LEN;
	if (CFStringGetLength(urlStringM) < (maxlen+SchemeLength))
		goto WeAreDone;
	
	/* 
	 * At this point the URL is too big to fit in the mount from name. See if
	 * removing the username will make it fit. 
	 */
	CFDictionaryRemoveValue(mutableDict, kUserNameKey);
	CFDictionaryRemoveValue(mutableDict, kPasswordKey);
	
	error = smb_dictionary_to_urlstring(mutableDict, &newUrlStringM, FALSE);
	if (error || (newUrlStringM == NULL)) {
		smb_log_info("Removing username failed parsing dictionary!", error, ASL_LEVEL_DEBUG);
		goto WeAreDone;
	}
	CFRelease(urlStringM);
	urlStringM = newUrlStringM;
	newUrlStringM = NULL;
	
WeAreDone:
	if (urlStringM && (SchemeLength > 0)) {
		/* Remove the scheme, start at the begining */
		CFRange range1 = CFRangeMake(0, SchemeLength);
		CFStringDelete(urlStringM, range1);		
	}
	if (urlStringM)
		CFStringGetCString(urlStringM, fromname, maxlen, kCFStringEncodingUTF8);
	if (error)
		smb_log_info("Mount from name is %s", error, ASL_LEVEL_ERR, fromname);
	else
		smb_log_info("Mount from name is %s", 0, ASL_LEVEL_DEBUG, fromname);
	
	if (urlStringM)
		CFRelease(urlStringM);
	if (mutableDict)
		CFRelease(mutableDict);	
}

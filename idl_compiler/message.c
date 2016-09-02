/*
 * 
 * (c) Copyright 1993 OPEN SOFTWARE FOUNDATION, INC.
 * (c) Copyright 1993 HEWLETT-PACKARD COMPANY
 * (c) Copyright 1993 DIGITAL EQUIPMENT CORPORATION
 * To anyone who acknowledges that this file is provided "AS IS"
 * without any express or implied warranty:
 *                 permission to use, copy, modify, and distribute this
 * file for any purpose is hereby granted without fee, provided that
 * the above copyright notices and this notice appears in all source
 * code copies, and that none of the names of Open Software
 * Foundation, Inc., Hewlett-Packard Company, or Digital Equipment
 * Corporation be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior
 * permission.  Neither Open Software Foundation, Inc., Hewlett-
 * Packard Company, nor Digital Equipment Corporation makes any
 * representations about the suitability of this software for any
 * purpose.
 */
/*
**
**  NAME
**
**      MESSAGE.C
**
**  FACILITY:
**
**      Interface Definition Language (IDL) Compiler
**      UUID Generator Tool
**
**  ABSTRACT:
**
**      International error message primitive routines.
**
**  VERSION: DCE 1.0
**
*/

#ifdef UUIDGEN  /* Building for uuidgen, so include whatever's needed from nidl.h. */
#   if defined __STDC__
#       include <limits.h>
#       include <stdlib.h>
#   endif
#   include <uuidmsg.h>
#   define MESSAGE_VERSION      UUIDGEN_MESSAGE_VERSION
#   define MESSAGE_VERSION_USED UUIDGEN_MESSAGE_VERSION_USED
#   define MESSAGE_CATALOG_DIR  "/usr/bin/"
#   define NLSCATVER            UUIDGEN_NLSCATVER
#   define NLSWRONG             UUIDGEN_NLSWRONG
#   ifdef _AIX
#       define NL_VFPRINTF NLvfprintf
#   else
#       define NL_VFPRINTF vfprintf
#   endif
#   define BRANCHCHAR '/'
#else   /* Building for nidl. */
#   include <nidl.h>
#   include <nidlmsg.h>
#   define MESSAGE_VERSION      NIDL_MESSAGE_VERSION
#   define MESSAGE_VERSION_USED NIDL_MESSAGE_VERSION_USED
#   define NLSCATVER            NIDL_NLSCATVER
#   define NLSWRONG             NIDL_NLSWRONG
#endif

#include <stdio.h>
#include <string.h>

#ifdef VMS
#   include <descrip.h>
#   define MAX_MSG_IDENT   32      /* Max size of message identifier */
#   define MAX_MSG_TEXT   256      /* Max size of VMS message text */
#   define MAX_FMT_TEXT   512      /* Max size of formatted output string */
#   define MSG_OPTS       0xF      /* %FACIL-S-IDENT, Text */
#else
#   define MAX_FMT_TEXT   512      /* Max size of formatted output string */
#   include <nl_types.h>
#   ifdef __STDC__
#       include <stdarg.h>  /* New! Improved! Method */
#       define VA_START(L, A, T) va_start(L, A)
#   else
#       include <varargs.h> /* Traditional Method */
#       define VA_START(L, A, T) T A; va_start(L); A = va_arg(L,T)
#   endif

#ifdef UUIDGEN
#   ifndef PATH_MAX
#       define PATH_MAX 256
#   endif
#endif


    static nl_catd cat_handle;
/*
** Declare an array to hold the default messages.  The text of the messages is
** read from a file generated from the message catalog.
*/
char *default_messages[] = {
"Internal idl compiler error: Invalid message number",
#include <default_msg.h>
};
static long max_message_number		/* Compute number of messages. */
	= (long)(sizeof(default_messages)/sizeof(char *) - 1);
#   define def_message(id) \
	default_messages[(id<0||id>max_message_number)?0:id]
#endif

#include <message.h>
static char     msg_prefix[PATH_MAX+3];




/*
 *  m e s s a g e _ o p e n
 *
 *  Function:   Opens message database.
 */

void message_open
(
    char *image_name
)

#ifdef VMS
                    /* m e s s a g e _ o p e n  (VMS) */
{
    struct dsc$descriptor
                    ctrstr,     /* Descriptor for stored text of message */
                    outbuf;     /* Descriptor for formatted text of message */
    long            flags;      /* Message flags */
    unsigned short  outlen;     /* Length of formatted message */
    long            status;
    char            version_text[MAX_MSG_TEXT];

    ctrstr.dsc$w_length     = sizeof(version_text)-1;
    ctrstr.dsc$b_dtype      = DSC$K_DTYPE_T;    /* Text */
    ctrstr.dsc$b_class      = DSC$K_CLASS_S;    /* Static */
    ctrstr.dsc$a_pointer    = version_text;         /* Point at local buffer */

    strncpy(msg_prefix,image_name,PATH_MAX);
    strcat(msg_prefix,": ");

#ifdef DUMPERS
    status = SYS$GETMSG(MESSAGE_VERSION, &ctrstr.dsc$w_length, &ctrstr, 1, 0);
    if ((status & 1) == 0)
        fprintf(stderr, "%sError in error message processing!\n",msg_prefix);
    version_text[ctrstr.dsc$w_length] = '\0';
    if (atoi(version_text) != MESSAGE_VERSION_USED)
        fprintf(stderr, "%sMessage catalog version mismatch, Expected: \"%d\", Actual: \"%s\"\n",
            msg_prefix, MESSAGE_VERSION_USED, version_text );
#endif
    return;
}

#else
                    /* m e s s a g e _ o p e n  (non-VMS) */
{
    char cat_name[PATH_MAX];
    char *cp;  /* Used to pull image name out of full image path */
    char *bcp; /* Used to fine BRANCHCHAR in image name */

    /*
     * Save away the image name for use in error reporting
     */
    strncpy(msg_prefix,image_name,PATH_MAX);
    strcat(msg_prefix,": ");

    /*
     * Find the name of the image (path name removed), loop
     * until all branch chars are removed.
     */
    cp = image_name;
    while ((bcp=strrchr(cp,BRANCHCHAR)) != NULL)
    {
        /*
         *  bcp now points at a BRANCHCHAR, If this is a trailing BRANCHCHAR,
         *  remove it, otherwise everything following the BRANCHCHR is the
         *  image name, so just move past it.
         */
        if (bcp[1] == '\0')
            bcp[0]='\0';
        else
            cp = bcp+1;
    }

    /*
     *  cp is now only the image name, so just add ".cat" to derive the name of
     *  the message catalog.
     */
    strncpy(cat_name,cp,PATH_MAX);
    strcat(cat_name,".cat");


    /*
     * Open the message catalog using the image name.
     */
#ifdef AIX32
    setlocale(LC_ALL, "");
#endif
    cat_handle = catopen(cat_name, 0);


    /* Sucessful open, check version information */
    if (cat_handle != (nl_catd)-1)
    {
          char  *version_text;
          version_text = catgets(cat_handle,CAT_SET,MESSAGE_VERSION,NULL);
          if (version_text != NULL && atoi(version_text) != MESSAGE_VERSION_USED)
          {
              fprintf(stderr, def_message(NLSCATVER),
                  msg_prefix, cat_name, MESSAGE_VERSION_USED, version_text);
              fprintf(stderr, "\n");
              fprintf(stderr, def_message(NLSWRONG), msg_prefix);
              fprintf(stderr, "\n");
          }
    }
    return;
}
#endif

/*
 *  m e s s a g e _ c l o s e
 *
 *  Function:   Closes message database.
 */

void message_close
(
    void
)

#ifdef VMS
                    /* m e s s a g e _ c l o s e  (VMS) */
{
    return;
}

#else
                    /* m e s s a g e _ c l o s e  (non-VMS) */
{
    catclose(cat_handle);
    return;
}

#endif

/*
 *  m e s s a g e _ p r i n t
 *
 *  Function:   Fetches message from database, then formats and prints message.
 *
 *  Inputs:     msgid - message ID
 *              [arg1,...,arg5] - Optional arguments for message formatting
 *
 *  Outputs:    message printed to stderr.
 *              On VAX/VMS systems, the message is output to
 *              SYS$ERROR/SYS$OUTPUT following the VAX/VMS conventions.
 */

void message_print
#ifdef VMS
(
    long msgid,
    char *arg1,
    char *arg2,
    char *arg3,
    char *arg4,
    char *arg5
)
#else
#ifdef __STDC__
(long msgid, ...)
#else
(va_alist) va_dcl
#endif
#endif

#if defined(VMS) && !defined(BROKEN_PUTMSG)
                    /* m e s s a g e _ p r i n t  (VMS) */
{
    long            status;
    char            *msg_vec[8]; /* create a message vec for PUTMSG */

    /* Initialize the message vector */
    msg_vec[0] = (char *)7;
    msg_vec[1] = (char *)msgid;
    msg_vec[2] = (char *)5;
    msg_vec[3] = arg1;
    msg_vec[4] = arg2;
    msg_vec[5] = arg3;
    msg_vec[6] = arg4;
    msg_vec[7] = arg5;

    status = SYS$PUTMSG(msg_vec, 0, 0, 0);
    if ((status & 1) == 0) LIB$SIGNAL(status);
}
#else
{   /* non-VMS message_print() */
    va_list arglist;
    char            format[MAX_FMT_TEXT];     /* Format string */
    VA_START(arglist, msgid, long);

    /*
     * Output message prefix on all errors that identify the input file,
     * or on every line for UUIDGEN
     */
    format[0]='\0';
    switch (msgid)
    {
#ifndef UUIDGEN
        case NIDL_EOF:
        case NIDL_EOFNEAR:
        case NIDL_SYNTAXNEAR:
        case NIDL_FILESOURCE:
        case NIDL_LINEFILE:
#else
        default:
#endif
            strcpy(format, msg_prefix);
    }

    strcat(format,catgets(cat_handle, CAT_SET, msgid, def_message(msgid)));
    strcat(format,"\n");
    NL_VFPRINTF(stderr, format, arglist);
    va_end(arglist);
}
#endif



#ifndef UUIDGEN
/*
 *  m e s s a g e _ s p r i n t
 *
 *  Function:   Fetches message from database and formats message.
 *
 *  Inputs:     str - Address of buffer for formatted message
 *              msgid - message ID
 *              [arg1,...,arg5] - Optional arguments for message formatting
 *
 *  Outputs:    str
 */

void message_sprint
(
    char *str,
    long msgid,
    char *arg1,
    char *arg2,
    char *arg3,
    char *arg4,
    char *arg5
)

#ifdef VMS
                    /* m e s s a g e _ s p r i n t  (VMS) */
{
    struct dsc$descriptor
                    ctrstr,     /* Descriptor for stored text of message */
                    outbuf;     /* Descriptor for formatted text of message */
    long            flags;      /* Message flags */
    unsigned short  outlen;     /* Length of formatted message */
    long            status;
    char            msg_text[MAX_MSG_TEXT];

    ctrstr.dsc$w_length     = sizeof(msg_text)-1;
    ctrstr.dsc$b_dtype      = DSC$K_DTYPE_T;    /* Text */
    ctrstr.dsc$b_class      = DSC$K_CLASS_S;    /* Static */
    ctrstr.dsc$a_pointer    = msg_text;         /* Point at local buffer */

    flags = MSG_OPTS;           /* %FAC-S-IDENT, Text */

    status = SYS$GETMSG(msgid, &ctrstr.dsc$w_length, &ctrstr, flags, 0);
    if ((status & 1) == 0)
        fprintf(stderr, "Error in error message processing!\n");

    outbuf.dsc$w_length     = MAX_FMT_TEXT-1;   /* Assume user buf fits max */
    outbuf.dsc$b_dtype      = DSC$K_DTYPE_T;    /* Text */
    outbuf.dsc$b_class      = DSC$K_CLASS_S;    /* Static */
    outbuf.dsc$a_pointer    = str;              /* Point at user buffer */

    status = SYS$FAO(&ctrstr, &outlen, &outbuf, arg1, arg2, arg3, arg4, arg5);
    if ((status & 1) == 0)
        fprintf(stderr, "Error in error message processing!\n");

    str[outlen] = '\0';
}

#else
                   /* m e s s a g e _ s p r i n t  (non-VMS) */
{
    char *msg_text;     /* Ptr to message text (storage owned by catgets) */

    msg_text = catgets(cat_handle, CAT_SET, msgid, def_message(msgid));

    /*
     * Output message prefix on all errors that identify the input file
     */
    switch (msgid)
    {
        case NIDL_EOF:
        case NIDL_EOFNEAR:
        case NIDL_SYNTAXNEAR:
        case NIDL_FILESOURCE:
        case NIDL_LINEFILE:
            strcpy(str,msg_prefix);         /* Add prefix to messages */
            str +=  strlen(msg_prefix);
            break;
    }

    NL_SPRINTF(str, msg_text, arg1, arg2, arg3, arg4, arg5);
}

#endif

/*
 *  m e s s a g e _ f p r i n t
 *
 *  Function:   Fetches message from database, then formats and prints message.
 *
 *  Inputs:     fid - file handle of file for output message
 *              msgid - message ID
 *              [arg1,...,arg5] - Optional arguments for message formatting
 *
 *  Outputs:    message printed to file indicated by fid not including
 *		any system-dependant prefix information such as the compiler
 *		executable name, facility, severity, etc.
 */

void message_fprint
(
    FILE *fid,
    long msgid,
    char *arg1,
    char *arg2,
    char *arg3,
    char *arg4,
    char *arg5
)

#ifdef VMS
                    /* m e s s a g e _ f p r i n t  (VMS) */
{
    char            str[MAX_FMT_TEXT];     /* Formatted message text */
    struct dsc$descriptor
                    ctrstr,     /* Descriptor for stored text of message */
                    outbuf;     /* Descriptor for formatted text of message */
    long            flags;      /* Message flags */
    unsigned short  outlen;     /* Length of formatted message */
    long            status;
    char            msg_text[MAX_MSG_TEXT];

    ctrstr.dsc$w_length     = sizeof(msg_text)-1;
    ctrstr.dsc$b_dtype      = DSC$K_DTYPE_T;    /* Text */
    ctrstr.dsc$b_class      = DSC$K_CLASS_S;    /* Static */
    ctrstr.dsc$a_pointer    = msg_text;         /* Point at local buffer */

    flags = 1;           /* Text Only, No facility or severity prefix */

    status = SYS$GETMSG(msgid, &ctrstr.dsc$w_length, &ctrstr, flags, 0);
    if ((status & 1) == 0)
        fprintf(stderr, "Error in error message processing!\n");

    outbuf.dsc$w_length     = MAX_FMT_TEXT-1;   /* Assume user buf fits max */
    outbuf.dsc$b_dtype      = DSC$K_DTYPE_T;    /* Text */
    outbuf.dsc$b_class      = DSC$K_CLASS_S;    /* Static */
    outbuf.dsc$a_pointer    = str;              /* Point at user buffer */

    status = SYS$FAO(&ctrstr, &outlen, &outbuf, arg1, arg2, arg3, arg4, arg5);
    if ((status & 1) == 0)
        fprintf(stderr, "Error in error message processing!\n");

    str[outlen] = '\0';
    fprintf(fid, "%s\n", str);
}

#else
                   /* m e s s a g e _ f p r i n t  (non-VMS) */
{
    char            str[MAX_FMT_TEXT];     /* Formatted message text */
    char *msg_text;     /* Ptr to message text (storage owned by catgets) */

    msg_text = catgets(cat_handle, CAT_SET, msgid, def_message(msgid));

    NL_SPRINTF(str, msg_text, arg1, arg2, arg3, arg4, arg5);
    fprintf(fid, "%s\n", str);
}

#endif
#endif

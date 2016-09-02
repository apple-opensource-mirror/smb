/*
 * 
 * (c) Copyright 1989 OPEN SOFTWARE FOUNDATION, INC.
 * (c) Copyright 1989 HEWLETT-PACKARD COMPANY
 * (c) Copyright 1989 DIGITAL EQUIPMENT CORPORATION
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
 * 
 */
/*
**
**  NAME:
**
**      commonp.h
**
**  FACILITY:
**
**      Remote Procedure Call (RPC) 
**
**  ABSTRACT:
**
**  Definitions of types/constants internal to runtime.
**
**
*/

/* ========================================================================= */

/*
 * Your OS / machine specific configuration file can override any of the
 * default definitions / includes in this file.  Additional definitions / 
 * overrides that exist:
 *
 *  Controls for generic conditional compilation:
 *      NCS1_COMPATIBILITY  - enable inclusion of NCS 1.5.1 API support
 *      FTN_INTERLUDES      - enable inclusion of FTN callable API
 *      DEBUG               - enable inclusion of various runtime debugging
 *                            features
 *      RPC_MUTEX_DEBUG     - enable mutex lock / cond var debugging
 *      RPC_MUTEX_STATS     - enable mutex lock / cond var statistics
 *      MAX_DEBUG           - enable inclusion of additional debug code
 *                          (e.g. DG pkt logging capability)
 *      RPC_DG_LOSSY        - enable inclusion of DG lossy test code
 *
 *      INET                - enable inclusion of Internet Domain family support
 *      DDS                 - enable inclusion of Apollo DOMAIN Domain family
 *                            support
 *       NO_ELLIPSIS        - disable function prototypes which have
 *                            an ellipsis in them.
 *
 *      CONVENTIONAL_ALIGNMENT
 *
 *  Controls for alternate implementations of things:
 *      STDARG_PRINTF       - use ANSI C stdarg.h for rpc__printf
 *                          (otherwise use varargs.h)
 *      NO_VARARGS_PRINTF   - no varargs.h for rpc__printf; wing it
 *      NO_RPC_PRINTF       - none of the various rpc__printf implementations
 *                          is appropriate - provide your own. e.g.
 *                              #define rpc__printf printf
 *      NO_SSCANF           - define to prevent direct use of sscanf()
 *      NO_SPRINTF          - define to prevent direct use of sprintf()
 *      NO_GETENV           - define to prevent direct use of getenv()
 */


#ifndef _COMMONP_H
#define _COMMONP_H  1

/*
 * Include a OS / machine specific configuration file.  
 */

#ifdef DCE_RPC_DEBUG
#define DCE_DEBUG	1
#endif

#  include <sysconf.h>

/* ========================================================================= */

/* ========================================================================= */

/*
 * EXTERNAL    
 *      Applied to variables that are external to a module.
 * GLOBAL
 *      Applied to defining instance of a variable.
 * PUBLIC
 *      Applied to (global) functions that are part of the defined API.
 * PRIVATE
 *      Applied to (global) functions that are NOT part of the defined API.
 * INTERNAL
 *      Applied to functions and variables that are private to a module.
 */

#ifndef EXTERNAL
#  define EXTERNAL      extern
#endif

#ifndef GLOBAL
#  define GLOBAL
#endif

#ifndef PUBLIC
#  define PUBLIC
#endif

#ifndef PRIVATE
#  define PRIVATE
#endif

#ifndef INTERNAL
#  define INTERNAL        static
#endif

/* ========================================================================= */

#ifndef NULL
#define NULL 0
#endif

/*
 * This boolean type is only for use internal to the runtime (it's smaller,
 * so it saves storage in structures). All API routines should use boolean32,
 * which is defined in nbase.idl (as are the values for 'true' and 'false').
 */

/* typedef unsigned char boolean;*/

/*
 * This definition is for use by towers.
 */

/*#was_define  byte_t  idl_byte  */
typedef idl_byte byte_t ;

/* ========================================================================= */

#include <dce/nbase.h>
#include <dce/lbase.h>
#include <dce/rpc.h>
#include <dce/stubbase.h>
#include <rpclog.h>
#include <dce/dce_error.h>

/* ========================================================================= */

#ifdef DCE_RPC_SVC
#  include <rpcsvc.h>
#else

#ifndef EPRINTF
#  define EPRINTF           rpc__printf
#endif /* EPRINTF */

#ifndef DIE
#  define DIE(text)         rpc__die(text, __FILE__, __LINE__)
#endif /* DIE */

#endif	/* DCE_RPC_SVC */

/* ========================================================================= */

#ifndef UUID_EQ
#  define UUID_EQ(uuid1, uuid2, st) \
        (uuid_equal(&(uuid1), &(uuid2), (st)))
#endif /* UUID_EQ */

/*
 * Macros to deal with NULL UUID pointers.
 */

#ifndef UUID_PTR
#  define UUID_PTR(uuid_ptr) \
        ((uuid_ptr) != NULL ? (uuid_ptr) : &uuid_g_nil_uuid)
#endif /* UUID_PTR */

#ifndef UUID_SET
#  define UUID_SET(uuid_ptr_dst, uuid_src) \
        if ((uuid_ptr_dst) != NULL) \
	  *(uuid_ptr_dst) = (uuid_src); 
#endif /* UUID_SET */

#ifndef UUID_IS_NIL
#  define UUID_IS_NIL(uuid_ptr, st) \
        (*(st) = 0, (uuid_ptr) == NULL || UUID_EQ(*(uuid_ptr), uuid_g_nil_uuid, st))
#endif /* UUID_IS_NIL */

#ifndef UUID_CREATE_NIL
#  define UUID_CREATE_NIL(uuid_ptr) \
    UUID_SET((uuid_ptr), uuid_g_nil_uuid)
#endif /* UUID_CREATE_NIL */

/* ========================================================================= */

#ifndef MIN
#  define MIN(x, y)         ((x) < (y) ? (x) : (y))
#endif

#ifndef MAX
#  define MAX(x, y)         ((x) > (y) ? (x) : (y))
#endif

/* ========================================================================= */

#ifndef CLOBBER_PTR
#  ifdef DCE_RPC_DEBUG
#    define CLOBBER_PTR(p) (*(pointer_t *)&(p) = (pointer_t) 0xdeaddead)
#  else
#    define CLOBBER_PTR(p)
#  endif
#endif /* CLOBBER_PTR */

/* ========================================================================= */

/*
 * Macros for swapping bytes in integers and UUIDs.
 */

#ifndef SWAB_16
#define SWAB_16(field) ( \
    ((unsigned16) field >> 8) | \
    (field << 8) \
)
#endif /* SWAB_16 */

#ifndef SWAB_32
#define SWAB_32(field) ( \
    ((unsigned32) field >> 24) | \
    ((field >> 8) & 0x0000ff00) | \
    ((field << 8) & 0x00ff0000) | \
    (field << 24) \
)
#endif /* SWAB_32 */

#ifndef SWAB_INPLACE_16
#define SWAB_INPLACE_16(field) { \
    field = SWAB_16(field); \
}
#endif /* SWAB_INPLACE_16 */

#ifndef SWAB_INPLACE_32
#define SWAB_INPLACE_32(field) { \
    field = SWAB_32(field); \
}
#endif /* SWAB_INPLACE_32 */

#ifndef SWAB_INPLACE_UUID
#define SWAB_INPLACE_UUID(ufield) { \
    SWAB_INPLACE_32((ufield).time_low); \
    SWAB_INPLACE_16((ufield).time_mid); \
    SWAB_INPLACE_16((ufield).time_hi_and_version); \
}
#endif /* SWAB_INPLACE_UUID */

/*
 * Macros for converting to little endian, our data representation
 * for writing towers and other integer data into the namespace.  
 */
#ifndef RPC_RESOLVE_ENDIAN_INT16
#define RPC_RESOLVE_ENDIAN_INT16(field) \
{ \
    if (NDR_LOCAL_INT_REP != ndr_c_int_little_endian) \
    { \
        SWAB_INPLACE_16 ((field)); \
    } \
}
#endif /* RPC_RESOLVE_ENDIAN_INT16 */

#ifndef RPC_RESOLVE_ENDIAN_INT32
#define RPC_RESOLVE_ENDIAN_INT32(field) \
{ \
    if (NDR_LOCAL_INT_REP != ndr_c_int_little_endian) \
    { \
        SWAB_INPLACE_32 ((field)); \
    } \
}
#endif /* RPC_RESOLVE_ENDIAN_INT32 */

#ifndef RPC_RESOLVE_ENDIAN_UUID
#define RPC_RESOLVE_ENDIAN_UUID(field) \
{ \
    if (NDR_LOCAL_INT_REP != ndr_c_int_little_endian) \
    { \
        SWAB_INPLACE_UUID ((field)); \
    } \
}
#endif /* RPC_RESOLVE_ENDIAN_UUID */


/* ========================================================================= */

#ifdef ALT_COMMON_INCLUDE
#  include ALT_COMMON_INCLUDE
#else
#  include <rpcfork.h>
#  include <rpcdbg.h>
#  include <rpcclock.h>
#  include <rpcmem.h>
#  include <rpcmutex.h>
#  include <rpctimer.h>
#  include <rpclist.h>
#  include <rpcrand.h>
#endif /* ALT_COMMON_INCLUDE */

/* ========================================================================= */

#endif /* _COMMON_H */

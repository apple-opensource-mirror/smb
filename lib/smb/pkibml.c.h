/*
 * 
 * (c) Copyright 1991 OPEN SOFTWARE FOUNDATION, INC.
 * (c) Copyright 1991 HEWLETT-PACKARD COMPANY
 * (c) Copyright 1991 DIGITAL EQUIPMENT CORPORATION
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
**      pkibml.c
**
**  FACILITY:
**
**      IDL Stub Runtime Support
**
**  ABSTRACT:
**
**      This module contains code to extract information from an
**      UNPACKED_REAL structure and to create an IBM long floating number
**      with those bits.
**
**              This module is meant to be used as an include file.
**
**  VERSION: DCE 1.0
**
*/

/*
**++
**  Functional Description:
**
**  This module contains code to extract information from an
**  UNPACKED_REAL structure and to create an IBM long floating number
**  with those bits.
**
**  See the header files for a description of the UNPACKED_REAL
**  structure.
**
**  A normalized IBM long floating number looks like:
**
**      [0]: Sign bit, 7 exp bits (bias 64), 24 fraction bits
**      [1]: 32 low order fraction bits
**
**      0.0625 <= fraction < 1.0, from 0 to 3 leading zeros
**      to compensate for the hexadecimal exponent.
**
**
**  Implicit parameters:
**
**      options: a word of flags, see include files.
**
**      output_value: a pointer to the input parameter.
**
**      r: an UNPACKED_REAL structure.
**
**      i: temporary integer variable
**
**      j: temporary integer variable
**
**--
*/



if (r[U_R_FLAGS] & U_R_UNUSUAL) {

        if (r[U_R_FLAGS] & U_R_ZERO)

                if (r[U_R_FLAGS] & U_R_NEGATIVE)
                        memcpy(output_value, IBM_L_NEG_ZERO, 8);
                else
                        memcpy(output_value, IBM_L_POS_ZERO, 8);

        else if (r[U_R_FLAGS] & U_R_INFINITY) {

                if (r[U_R_FLAGS] & U_R_NEGATIVE)
                        memcpy(output_value, IBM_L_NEG_INFINITY, 8);
                else
                        memcpy(output_value, IBM_L_POS_INFINITY, 8);

        } else if (r[U_R_FLAGS] & U_R_INVALID) {

                memcpy(output_value, IBM_L_INVALID, 8);
                RAISE(exc_e_aritherr);    /* Invalid value */

        }

} else {

        /* Precision varies because binary exp must be multiple of 4 */
        /* (since we must convert it to a hexadecimal exponent). */
        /* So, figure out where to round (53 <= i <= 56). */

        i = (r[U_R_EXP] & 0x00000003L);
        if (i)
                round_bit_position = i + 52;
        else
                round_bit_position = 56;

#include "round.c.h"

        if (r[U_R_EXP] < (U_R_BIAS - 255)) {

                /* Underflow */

                if (r[U_R_FLAGS] & U_R_NEGATIVE)
                        memcpy(output_value, IBM_L_NEG_ZERO, 8);
                else
                        memcpy(output_value, IBM_L_POS_ZERO, 8);
                if (options & CVT_C_ERR_UNDERFLOW) {
                        RAISE(exc_e_fltund);  /* Underflow */
                }

        } else if (r[U_R_EXP] > (U_R_BIAS + 252)) {

                /* Overflow */

                if (options & CVT_C_TRUNCATE) {

                        if (r[U_R_FLAGS] & U_R_NEGATIVE)
                                memcpy(output_value, IBM_L_NEG_HUGE, 8);
                        else
                                memcpy(output_value, IBM_L_POS_HUGE, 8);

                } else if ((options & CVT_C_ROUND_TO_POS)
                                        && (r[U_R_FLAGS] & U_R_NEGATIVE)) {

                                memcpy(output_value, IBM_L_NEG_HUGE, 8);

                } else if ((options & CVT_C_ROUND_TO_NEG)
                                        && !(r[U_R_FLAGS] & U_R_NEGATIVE)) {

                                memcpy(output_value, IBM_L_POS_HUGE, 8);

                } else {

                        if (r[U_R_FLAGS] & U_R_NEGATIVE)
                                memcpy(output_value, IBM_L_NEG_INFINITY, 8);
                        else
                                memcpy(output_value, IBM_L_POS_INFINITY, 8);

                }

                RAISE(exc_e_fltovf);  /* Overflow */

        } else {

                /* Figure leading zeros (i) and biased exponent (j) */

                i = (r[U_R_EXP] & 0x00000003L);
                j = ((int)(r[U_R_EXP] - U_R_BIAS) / 4) + 64;

                if (i) {
                        if (r[U_R_EXP] > U_R_BIAS)
                                j += 1;
                        i = 12 - i;
                } else {
                        i = 8;
                }

                /* Make room for exponent and sign bit */

                r[2] >>= i;
                r[2] |= (r[1] << (32 - i));
                r[1] >>= i;

                /* OR in exponent and sign bit */

                r[1] |= (j << 24);
                r[1] |= (r[U_R_FLAGS] << 31);

#if (NDR_LOCAL_INT_REP == ndr_c_int_big_endian)

                memcpy(output_value, &r[1], 8);

#else
                /* Shuffle bytes to big endian format */

                r[0]  = ((r[1] << 24) | (r[1] >> 24));
                r[0] |= ((r[1] << 8) & 0x00FF0000L);
                r[0] |= ((r[1] >> 8) & 0x0000FF00L);
                r[1]  = ((r[2] << 24) | (r[2] >> 24));
                r[1] |= ((r[2] << 8) & 0x00FF0000L);
                r[1] |= ((r[2] >> 8) & 0x0000FF00L);

                memcpy(output_value, r, 8);
#endif
        }

}

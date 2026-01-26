/* SPDX-License-Identifier: MIT OR X11
 *
 * Copyright © 2024 Enrico Weigelt, metux IT consult <info@metux.net>
 */
#ifndef __SNA_EDIDPARSE_H
#define __SNA_EDIDPARSE_H

/*
 * selectively taken over from xf86's internal EDID parser definitions,
 * which will be removed from SDK header soon.
 *
 * FIXME: EDID parsing shouldn't be in a driver at all
 */
#define SECTION(x,y) (uint8_t *)(x + y)
#define NO_EDID (DET_TIMING_SECTION + DET_TIMING_LENGTH)
#define DET_TIMING_SECTION (STD_TIMING_SECTION + STD_TIMING_LENGTH)
#define DET_TIMING_LENGTH (DET_TIMING_INFO_LEN * DET_TIMING_INFO_NUM)
#define STD_TIMING_SECTION (ESTABLISHED_TIMING_SECTION + E_TIMING_LENGTH)
#define STD_TIMING_LENGTH (STD_TIMING_INFO_LEN * STD_TIMING_INFO_NUM)
#define ESTABLISHED_TIMING_SECTION (DISPLAY_SECTION + DISPLAY_LENGTH)
#define DISPLAY_SECTION (VERSION_SECTION + VERSION_LENGTH)
#define VERSION_SECTION (VENDOR_SECTION + VENDOR_LENGTH)
#define VENDOR_SECTION (HEADER_SECTION + HEADER_LENGTH)
#define HEADER_SECTION 0
#define HEADER_LENGTH 8
#define VENDOR_LENGTH (V_YEAR + 1)
#define V_YEAR (V_WEEK + 1)
#define V_WEEK (V_SERIAL + 4)
#define V_SERIAL (V_PROD_ID + 2)
#define V_PROD_ID (V_MANUFACTURER + 2)
#define V_MANUFACTURER 0
#define VERSION_LENGTH (V_REVISION + 1)
#define V_REVISION (V_VERSION + 1)
#define V_VERSION 0
#define DISPLAY_LENGTH (D_WHITEY + 1)
#define D_WHITEY (D_WHITEX + 1)
#define E_TIMING_LENGTH (E_TMANU + 1)
#define STD_TIMING_INFO_LEN 2
#define STD_TIMING_INFO_NUM STD_TIMINGS
#define DET_TIMING_INFO_LEN 18
#define DET_TIMING_INFO_NUM DET_TIMINGS
#define D_WHITEX (D_BLUEY + 1)
#define E_TMANU (E_T2 + 1)
#define D_BLUEY (D_BLUEX + 1)
#define E_T2 (E_T1 + 1)
#define D_BLUEX (D_GREENY + 1)
#define E_T1 0
#define D_GREENY (D_GREENX + 1)
#define D_GREENX (D_REDY + 1)
#define D_REDY (D_REDX + 1)
#define D_REDX (D_BW_LOW + 1)
#define D_BW_LOW (D_RG_LOW + 1)
#define D_RG_LOW (FEAT_S + 1)
#define FEAT_S (D_GAMMA + 1)
#define D_GAMMA (D_VSIZE + 1)
#define D_VSIZE (D_HSIZE + 1)
#define D_HSIZE (D_INPUT + 1)
#define D_INPUT 0

#endif /* __SNA_EDIDPARSE_H */

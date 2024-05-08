/*******************************************************************************
* hmc7043.h - definitions related to interfacing to clock distribution devices *
********************************************************************************
* modification history:                                                        *
*   31.01.24 bf, created                                                       *
*******************************************************************************/

#ifndef _ckdstif_h_
#define _ckdstif_h_

#include "sysbase.h"


typedef unsigned CKDST_DEV;

#define CKDST_MAX_NDEV	10      /* arbitrary */

typedef UINT32 CKDST_DEV_MASK;  /* one bit per channel */

typedef UINT64 CKDST_FREQ_HZ;



#endif /* _ckdstif_h_ */


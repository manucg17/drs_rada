/*******************************************************************************
* hmc7044.c - interface to HMC7044 Jitter Attenuator devices                   *
*                                                                              *
* The system can comprise one or more such devices.                            *
* They are interfaced via dedicated SPI blocks in the FPGA. It is possible to  *
* read and write register data from / to the device via SPI.                   *
* Additionally there are status lines from the device (PLL lock) that are      *
* routed to the FPGA.                                                          *
********************************************************************************
* modification history:                                                        *
* 04.04.24, Thinkpalm created                                                  *
*******************************************************************************/
#include <string.h>
#include "sysutil.h"
#include "hmc7044.h"


/*#############################################################################*
*    I N I T I A L I Z A T I O N    A N D    O V E R A L L    C O N T R O L    *
*#############################################################################*/


/* types */
typedef struct {
    Bool initDone;
    HUTL_MUTEX hMutex;
} Hmc7044_dev_ctl;


/* control data */
LOCAL struct {
    Bool initDone;
    CKDST_DEV_MASK devMask;
    Hmc7044_dev_ctl devCtl[CKDST_MAX_NDEV];
} hmc7044IfCtl;


/* forward references */
LOCAL STATUS hmc7044LliInit(CKDST_DEV_MASK devMask),
             hmc7044LliInitDev(CKDST_DEV dev, const Hmc7044_dev_io_if *pIf,
                               Bool warmInit);
LOCAL STATUS hmc7044CsEnter(CKDST_DEV dev, const char *context),
             hmc7044CsExit(CKDST_DEV dev, const char *context);
LOCAL STATUS hmc7044AppIfInit(void),
	     hmc7044AppInitDev(CKDST_DEV dev, const Hmc7044_app_dev_params *pParams,
                               Bool warmInit);


/*******************************************************************************
* - name: hmc7044IfInit
*
* - title: initialize HMC7044 control interface
*
* - input: devMask   - specifies the Clock device(s) that will be used
*
* - output: hmc7044IfCtl
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - notes: this routine can be called more than once
*******************************************************************************/
EXPORT STATUS hmc7044IfInit(CKDST_DEV_MASK devMask)
{

    unsigned i;

    if (!devMask || devMask >= 1 << NELEMENTS(hmc7044IfCtl.devCtl) ) {
        sysLog("bad argument(s) (devMask 0x%x)", devMask);
        return ERROR;
    }

    hmc7044IfCtl.devMask  = devMask;

    for (i = 0; i < NELEMENTS(hmc7044IfCtl.devCtl); ++i) {
        Hmc7044_dev_ctl *pDev = hmc7044IfCtl.devCtl + i;

        pDev->initDone = FALSE;
        pDev->hMutex   = UTL_MUTEX_BAD_HMUTEX;
    }

    hmc7044IfCtl.initDone = TRUE;

    if (hmc7044LliInit(devMask) != OK)
        return ERROR;

    /* initialize application-level interface */
    if (hmc7044AppIfInit() != OK)
        return ERROR;

    return OK;
}




/*******************************************************************************
* - name: hmc7044InitDev
*
* - title: initialize a specific CLKDST device
*
* - input: dev      - CLKDST device for which to perform the operation
*          pIf      - pointer to low-level interface access-related parameters
*          pParams  - application-level device setup parameters
*          warmInit - if set, will skip actual device initialization
*
* - returns: OK or ERROR if detected an error
*
* - description: sets up and initializes the CLKDST device per the application's
*                requirements
*
* - notes: 1) This routine can be called more than once (for a device).
*******************************************************************************/
EXPORT STATUS hmc7044InitDev(CKDST_DEV dev, const Hmc7044_dev_io_if *pIf,
                      const Hmc7044_app_dev_params *pParams, Bool warmInit)
{

    static const SYS_TIME MUTEX_TIMEOUT = 200;  /* msec; adequately large */

    Hmc7044_dev_ctl *pCtl;

    /* initialize */
    STATUS status = OK;  /* initial assumption */

    if (!inEnumRange(dev, NELEMENTS(hmc7044IfCtl.devCtl)) || !pIf || !pParams) {
        sysLog("bad argument(s) (dev %u, pIf %d, pParams %d)", dev, pIf != NULL,
               pParams != NULL);
        return ERROR;
    }

    if (!hmc7044IfCtl.initDone) {
        sysLog("interface not initialized yet (dev %u)", dev);
        return ERROR;
    }

    pCtl = hmc7044IfCtl.devCtl + dev;

    if (pCtl->initDone && pCtl->hMutex == UTL_MUTEX_BAD_HMUTEX) {
        sysLog("bad mutex (dev %u)", dev);
    }

    /* create the associated mutex (if first time here) */
    if (pCtl->hMutex == UTL_MUTEX_BAD_HMUTEX &&
        (pCtl->hMutex = utlMutexCreate(MUTEX_TIMEOUT)) == UTL_MUTEX_BAD_HMUTEX) {
            sysLog("mutex creation failed (dev %u)", dev);
            return ERROR;
    }

    pCtl->initDone = TRUE;  /* must be done before the subsequent code */

    /* perform actual initialization */

    if (hmc7044LliInitDev(dev, pIf, warmInit) != OK)
        status = ERROR;
    else if (hmc7044AppInitDev(dev, pParams, warmInit) != OK)
        status = ERROR;

    return status;

}




/*******************************************************************************
* - name: hmc7044CsEnter
*
* - title: enter critical section
*
* - input: dev     - CLKDST device for which to perform the operation
*          context - caller context (only used for debugging)
*
* - returns: OK or ERROR if detected an error (if at all)
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7044CsEnter(CKDST_DEV dev, const char *context)
{
    const Hmc7044_dev_ctl *pCtl;

    /* initialize */
    context = context ? context : "???";

    if (!inEnumRange(dev, NELEMENTS(hmc7044IfCtl.devCtl))) {
        sysLogLong(" (from '%s'): bad argument(s) (dev %ld)", context, (long) dev);
        return ERROR;
    }

    pCtl = hmc7044IfCtl.devCtl + dev;

    if (!pCtl->initDone || pCtl->hMutex == UTL_MUTEX_BAD_HMUTEX) {
        sysLogLong(" (from '%s'): bad state for dev %ld (initDone %ld, "
                   "hMutex %ld)", context, (long) dev, (long) pCtl->initDone,
                   (long) (pCtl->hMutex != UTL_MUTEX_BAD_HMUTEX));
        return ERROR;
    }

    if (utlMutexTake(pCtl->hMutex, context) != OK) {
        sysCodeError(CODE_ERR_STATE, hmc7044CsEnter, context, dev, -1);
        return ERROR;
    }

    return OK;
}




/*******************************************************************************
* - name: hmc7044CsExit
*
* - title: exit critical section
*
* - input: dev     - CLKDST device for which to perform the operation
*          context - caller context (only used for debugging)
*
* - returns: ERROR if detected an error or the status returned from utlMutexRelease
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7044CsExit(CKDST_DEV dev, const char *context)
{
    const Hmc7044_dev_ctl *pCtl;

    /* initialize */
    context = context ? context : "???";

    if (!inEnumRange(dev, NELEMENTS(hmc7044IfCtl.devCtl))) {
        sysLogLong(" (from '%s'): bad argument(s) (dev %ld)", context, (long) dev);
        return ERROR;
    }

    pCtl = hmc7044IfCtl.devCtl + dev;

    if (!pCtl->initDone || pCtl->hMutex == UTL_MUTEX_BAD_HMUTEX) {
        sysLogLong(" (from '%s'): bad state for dev %ld (initDone %ld, "
                   "hMutex %ld)", context, (long) dev, (long) pCtl->initDone,
                   (long) (pCtl->hMutex != UTL_MUTEX_BAD_HMUTEX));
        return ERROR;
    }

    return utlMutexRelease(pCtl->hMutex, context);
}




/*#############################################################################*
*             R E G I S T E R    L A Y O U T    D E F I N I T I O N            *
*#############################################################################*/


/* constants and types */
#define HMC7044_REG_INX_MAX  0x153

#pragma pack(push)
#pragma pack(1)

typedef union {
    struct {
        HMC7044_REG softReset  : 1;
        HMC7044_REG resvdBits  : 7;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x00;

#define HMC7044_R00_RESVD  0x00

typedef union {
    struct {
    	HMC7044_REG sleepMode        : 1; /* sleep Mode */
    	HMC7044_REG restrtDvd        : 1; /* restart Dividers/FSM */
    	HMC7044_REG pulseGenReq      : 1; /* pulse Generator Request */
    	HMC7044_REG muteOutDrvr      : 1; /* mute Output Drivers */
    	HMC7044_REG forceHoldOver    : 1; /* Force Holdover */
    	HMC7044_REG resvdBit         : 1;
    	HMC7044_REG highPerfDistPath : 1; /* High Performance Distribution Path */
    	HMC7044_REG reseedReq        : 1; /* reseedRequest */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x01;

#define HMC7044_R01_RESVD1  0

typedef union {
    struct {
    	HMC7044_REG resvdBit        : 1;
    	HMC7044_REG slipReq         : 1; /* slip Request */
    	HMC7044_REG pll2AutoTune    : 1; /* PLL2 Auto tune trigger */
    	HMC7044_REG resvdBits       : 5;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x02;

#define HMC7044_R02_RESVD1  0
#define HMC7044_R02_RESVD2  0x00

typedef union {
    struct {
    	HMC7044_REG pll1En          : 1; /* PLL1 enable*/
    	HMC7044_REG pll2En          : 1; /* PLL2 enable*/
    	HMC7044_REG sysrefTimerEn   : 1; /* SYSREF Timer Enable */
    	HMC7044_REG vcoSel          : 2; /* VCO Selection[1:0] */
    	HMC7044_REG rfReseederEn    : 1; /* RF reseeder enable */
    	HMC7044_REG resvdBits       : 2;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x03;

#define HMC7044_R03_RESVD1  0x0

typedef union {
    struct {
    	HMC7044_REG chOutputEn  : 7; /* 7 pairs of Output Channel Enable */
    	HMC7044_REG resvdBit    : 1;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x04;

#define HMC7044_R04_RESVD1 0

typedef union {
    struct {
    	HMC7044_REG pl1RefPathEn    : 4; /* PLL1 Reference Path Enable */
    	HMC7044_REG  clk0RFSync     : 1; /* CLKIN0 in RF SYNC Mode */
    	HMC7044_REG  clk1ExtVco     : 1; /* CLKIN1 in External VCO input Mode */
    	HMC7044_REG  syncPinModeSel : 2; /* SYNC Pin Mode Selection */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x05;

typedef union {
    struct {
    	HMC7044_REG clearAlarms : 1; /* Clear alarms */
    	HMC7044_REG resvdBits   : 7;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x06;

#define HMC7044_R06_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG resvdBits    : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x07;

#define HMC7044_R07_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG resvdBits     : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x08;

#define HMC7044_R08_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG disSyncAtLock : 1; /* Disable Sync at lock */
    	HMC7044_REG resvdBits     : 7;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x09;

#define HMC7044_R09_RESVD1 0x00

/* PLL1 Registers */

typedef union {
    struct {
    	HMC7044_REG clkin0BufEn      : 1; /* CLKIN0 Buffer Enable */
    	HMC7044_REG clkin0InpBufMode : 4; /* CLKIN0 Input Buffer Mode */
    	HMC7044_REG resvdBits        : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x0a;

#define HMC7044_R0A_RESVD1 0x0

typedef union {
    struct {
    	HMC7044_REG clkin1BufEn      : 1; /* CLKIN1 Buffer Enable */
    	HMC7044_REG clkin1InpBufMode : 4; /* CLKIN1 Input Buffer Mode */
    	HMC7044_REG resvdBits        : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x0b;

#define HMC7044_R0B_RESVD1 0x0

typedef union {
    struct {
    	HMC7044_REG clkin2BufEn      : 1; /* CLKIN2 Buffer Enable */
    	HMC7044_REG clkin2InpBufMode : 4; /* CLKIN2 Input Buffer Mode */
    	HMC7044_REG resvdBits        : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x0c;

#define HMC7044_R0C_RESVD1 0x0

typedef union {
    struct {
    	HMC7044_REG clkin3BufEn      : 1; /* CLKIN3 Buffer Enable */
    	HMC7044_REG clkin3InpBufMode : 4; /* CLKIN3 Input Buffer Mode */
    	HMC7044_REG resvdBits        : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x0d;

#define HMC7044_R0D_RESVD1 0x0

typedef union {
    struct {
    	HMC7044_REG OscinBufEn      : 1; /* OSCIN Buffer Enable */
    	HMC7044_REG OscinInpBufMode : 4; /* OSCIN Input Buffer Mode */
    	HMC7044_REG resvdBits       : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x0e;

#define HMC7044_R0E_RESVD1 0x0

typedef union {
    struct {
    	HMC7044_REG firstPriClkin      : 2; /* first priority CLKIN */
    	HMC7044_REG secondPriClkin     : 2; /* second priority CLKIN */
    	HMC7044_REG thirdPriClkin      : 2; /* third priority CLKIN */
    	HMC7044_REG fourthPriClkin     : 2; /* fourth priority CLKIN */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x14;

typedef union {
    struct {
    	HMC7044_REG losValidnTimer  : 3; /* LOS Validation Timer */
    	HMC7044_REG resvdBits       : 5;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x15;

#define HMC7044_R15_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG holdOvrExitCriteria : 2; /* HoldOver Exit Criteria */
    	HMC7044_REG HoldOvrExitAction   : 2; /* HoldOver Exit Action */
    	HMC7044_REG resvdBits           : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x16;

#define HMC7044_R16_RESVD1 0x0

typedef union {
    struct {
      HMC7044_REG holdOvrDacVal	: 7; /* HoldOver DAC Value */
      HMC7044_REG resvdBit            : 1;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x17;

#define HMC7044_R17_RESVD1 0

typedef union {
    struct {
      HMC7044_REG holdoverBwRedn     : 2; /*HoldOver BW Reduction */
      HMC7044_REG forceDacToHoldover : 1; /* force DAC to holdover in quick mode */
      HMC7044_REG adcTrackingDisable : 1; /*ADC tracking disable */
      HMC7044_REG resvdBits          : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x18;

#define HMC7044_R18_RESVD1 0x0

typedef union {
    struct {
      HMC7044_REG losVcxoPrescaler      : 1; /* LOS uses VCXO prescaler */
      HMC7044_REG losBypassInpPrescaler : 1; /* LOS bypass input prescaler */
      HMC7044_REG resvdBits             : 6;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x19;

#define HMC7044_R19_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG pll1CpCurrent : 4; /* PLL1 Charge Pump Current */
    	HMC7044_REG resvdBits     : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x1a;

#define HMC7044_R1A_RESVD1 0x0
LOCAL const UINT32 HMC7044_R1A_CP_CUR_UA[] = {
    120,  240,  360, 480, 600, 720, 840, 960, /* codes 0-7 */
    1080, 1200, 1320, 1440, 1560, 1680, 1800, 1920 /* codes 8-15 */
};

typedef union {
    struct {
    	HMC7044_REG pll1PfdPolarity   : 1; /* PLL1 PFD polarity */
    	HMC7044_REG pll1PfdDownForce  : 1; /* PLL1 PFD Down Force */
    	HMC7044_REG pll1PfdUpForce    : 1; /* PLL1 PFD Up Force  */
    	HMC7044_REG pll1PfdDownEn     : 1; /* PLL1 PFD Down Enable */
    	HMC7044_REG pll1PfdUpEn       : 1; /* PLL1 PFD Up Enable */
    	HMC7044_REG resvdBits         : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x1b;

#define HMC7044_R1B_RESVD1 0x0

typedef union {
    struct {
    	HMC7044_REG Clkin0InpPreScaler : 8; /* CLKIN0 Input PreScaler */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x1c;

typedef union {
    struct {
    	HMC7044_REG Clkin1InpPreScaler : 8; /* CLKIN1 Input PreScaler */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x1d;

typedef union {
    struct {
    	HMC7044_REG Clkin2InpPreScaler : 8; /* CLKIN2 Input PreScaler */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x1e;

typedef union {
    struct {
    	HMC7044_REG Clkin3InpPreScaler : 8; /* CLKIN3 Input PreScaler */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x1f;

typedef union {
    struct {
    	HMC7044_REG OscinInpPreScaler : 8; /* OSCIN Input PreScaler */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x20;

typedef union {
    struct {
    	HMC7044_REG lsbR1Divider : 8; /* R1 LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x21;

typedef union {
    struct {
    	HMC7044_REG msbR1Divider : 8; /* R1 MSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x22;

typedef union {
    struct {
    	HMC7044_REG lsbN1Divider : 8; /* N1 LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x26;

typedef union {
    struct {
    	HMC7044_REG msbN1Divider : 8; /* N1 MSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x27;

typedef union {
    struct {
    	HMC7044_REG pll1LockDetect : 5; /* PLL1 Lock Detect Timer */
    	HMC7044_REG useSlip        : 1; /* PLL1 Lock Detect uses slip */
    	HMC7044_REG resvdBits      : 2;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x28;

#define HMC7044_R28_RESVD1 0x0

typedef union {
    struct {
    	HMC7044_REG autoModeRefSwitch   : 1; /* Auto mode reference switching */
    	HMC7044_REG autoRevertRefSwitch : 1; /* Auto revert reference switching */
    	HMC7044_REG holdOverDac         : 1; /* Holdover uses DAC */
    	HMC7044_REG manualModeRefSwitch : 2; /* Manual mode reference switching */
    	HMC7044_REG bypassDebounce      : 1; /* Bypass debouncer */
    	HMC7044_REG resvdBits           : 2;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x29;

#define HMC7044_R29_RESVD1 0x0

typedef union {
    struct {
    	HMC7044_REG holdOffTimer : 8; /* PLL1 hold off timer */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x2a;

/* PLL2 registers */
typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x31;

#define HMC7044_R31_RESVD1 0x01

typedef union {
    struct {
    	HMC7044_REG  bypassFreqDoubler : 1; /* Bypass frequency doubler */
    	HMC7044_REG  resvdBits         : 7;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x32;

#define HMC7044_R32_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG  lsbR2Divider: 8; /* LSB R2 divider */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x33;

typedef union {
    struct {
    	HMC7044_REG  msbR2Divider: 4; /* MSB R2 divider */
    	HMC7044_REG  resvdBits   : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x34;

#define HMC7044_R34_RESVD1 0x0

typedef union {
    struct {
    	HMC7044_REG  lsbN2Divider: 8; /* LSB N2 divider */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x35;

typedef union {
    struct {
    	HMC7044_REG  msbN2Divider: 8; /* MSB N2 divider */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x36;

typedef union {
    struct {
    	HMC7044_REG  pll2CpCurrent :4; /* PLL2 CP current */
    	HMC7044_REG  resvdBits     :4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x37;

#define HMC7044_R37_RESVD1 0x0

LOCAL const UINT32 HMC7044_R37_CP_CUR_UA[] = {
    160,  320,  480, 640, 800, 960, 1120, 1280, /* codes 0-7 */
    1440, 1600, 1760, 1920, 2080, 2240, 2400, 2560  /* codes 8-15 */
};

typedef union {
    struct {
    	HMC7044_REG pll2PfdPolarity   : 1; /* PLL2 PFD polarity */
    	HMC7044_REG pll2PfdDownForce  : 1; /* PLL2 PFD Down Force */
    	HMC7044_REG pll2PfdUpForce    : 1; /* PLL2 PFD Up Force  */
    	HMC7044_REG pll2PfdDownEn     : 1; /* PLL2 PFD Down Enable */
    	HMC7044_REG pll2PfdUpEn       : 1; /* PLL2 PFD Up Enable */
    	HMC7044_REG resvdBits         : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x38;

#define HMC7044_R38_RESVD1 0x0

typedef union {
    struct {
    	HMC7044_REG  oscoutPathEn   : 1; /* OSCOUT path enable */
    	HMC7044_REG  oscoutDivider  : 2; /* OSCOUT Divider */
    	HMC7044_REG  resvdBits      : 5;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x39;

#define HMC7044_R39_RESVD1 0x00

typedef union {
    struct {
    		HMC7044_REG  oscout0DrvEn  : 1; /* OSCOUT0 Driver enable */
        	HMC7044_REG  oscout0DrvImp : 2; /* OSCOUT0 Driver Impedence */
        	HMC7044_REG  resvdBit      : 1;
        	HMC7044_REG  oscout0DrvMode: 2; /* OSCOUT0 Driver Mode */
        	HMC7044_REG  resvdBits     : 2;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x3a;

#define HMC7044_R3A_RESVD1 0
#define HMC7044_R3A_RESVD2 0x0

typedef union {
    struct {
    	HMC7044_REG  oscout1DrvEn    : 1; /* OSCOUT1 Driver enable */
    	HMC7044_REG  oscout1DrvImp   : 2; /* OSCOUT1 Driver Impedence */
    	HMC7044_REG  resvdBit        : 1;
    	HMC7044_REG  oscout1DrvMode  : 2; /* OSCOUT1 Driver Mode */
    	HMC7044_REG  resvdBits       : 2;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x3b;

#define HMC7044_R3B_RESVD1 0
#define HMC7044_R3B_RESVD2 0x0

typedef union {
    struct {
    	HMC7044_REG  resvdBits    : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x3c;

#define HMC7044_R3C_RESVD1 0x00

/*GPIO Control*/
typedef union {
    struct {
    	HMC7044_REG  gpi1En    : 1; /* GPI1 Enable */
    	HMC7044_REG  gpi1Sel   : 4; /* GPI1 Selection */
    	HMC7044_REG  resvdBits : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x46;

#define HMC7044_R46_RESVD1 0x0

typedef enum {
    HMC7044_REG_GPIS_PLL1_HO     = 1,      HMC7044_REG_GPIS_PLL1_REF_B1     = 2,
    HMC7044_REG_GPIS_PLL1_REF_B0 = 3,      HMC7044_REG_GPIS_SLEEP           = 4,
    HMC7044_REG_GPIS_MUTE        = 5,      HMC7044_REG_GPIS_PLL2_VCO_SEL    = 6,
    HMC7044_REG_GPIS_PLL2_HPERF  = 7,      HMC7044_REG_GPIS_PULSE_GEN       = 8,
    HMC7044_REG_GPIS_RESEED      = 9,      HMC7044_REG_GPIS_RESTART         = 10,
    HMC7044_REG_GPIS_FANOUT_MODE = 11,     HMC7044_REG_GPIS_SLIP            = 13
} HMC7044_REG_GPI_SUP;

typedef union {
    struct {
    	HMC7044_REG  gpi2En     : 1; /* GPI2 Enable  */
    	HMC7044_REG  gpi2Sel    : 4; /* GPI2 Selection */
    	HMC7044_REG  resvdBits  : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x47;

#define HMC7044_R47_RESVD1 0x0

typedef union {
    struct {
    	HMC7044_REG  gpi3En    : 1; /* GPI3 Enable  */
    	HMC7044_REG  gpi3Sel   : 4; /* GPI3 Selection */
    	HMC7044_REG  resvdBits : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x48;

#define HMC7044_R48_RESVD1 0x0

typedef union {
    struct {
    	HMC7044_REG  gpi4En    : 1; /* GPI4 Enable */
    	HMC7044_REG  gpi4Sel   : 4; /* GPI4 Selection */
    	HMC7044_REG  resvdBits : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x49;

#define HMC7044_R49_RESVD1 0x0

typedef union {
    struct {
    	HMC7044_REG  gpo1En    : 1; /* GPO1 enable */
    	HMC7044_REG  gpo1Mode  : 1; /* GPO1 mode */
    	HMC7044_REG  gpo1Sel   : 6; /* GPO1 Selection */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x50;

typedef enum {HMC7044_REG_OM_OD, HMC7044_REG_OM_CMOS} HMC7044_REG_OUTPUT_MODE;

typedef enum {
        HMC7044_REG_GPOS_ALARM        = 0,     HMC7044_REG_GPOS_SDATA        = 1,
        HMC7044_REG_GPOS_CLKIN3_LOS   = 2,     HMC7044_REG_GPOS_CLKIN2_LOS   = 3,
        HMC7044_REG_GPOS_CLKIN1_LOS   = 4,     HMC7044_REG_GPOS_CLKIN0_LOS   = 5,
        HMC7044_REG_GPOS_PLL1_HO_EN   = 6,     HMC7044_REG_GPOS_PLL1_LOCKED  = 7,
        HMC7044_REG_GPOS_PLL1_LOCK_AQ = 8,     HMC7044_REG_GPOS_PLL1_LOCK_NL = 9,
        HMC7044_REG_GPOS_PLL2_LOCKED  = 0xa,   HMC7044_REG_GPOS_SREF_NSYNC   = 0xb,
        HMC7044_REG_GPOS_CKOUTS_PHASE = 0xc,   HMC7044_REG_GPOS_PLLS_LOCKED  = 0xd,
        HMC7044_REG_GPOS_SYNC_REQ_ST  = 0xe,   HMC7044_REG_GPOS_PLL1_ACT_C0  = 0xf,
        HMC7044_REG_GPOS_PLL1_ACT_C1  = 0x10,  HMC7044_REG_GPOS_PLL1_HO_AIR  = 0x11,
        HMC7044_REG_GPOS_PLL1_HO_AIS  = 0x12,  HMC7044_REG_GPOS_PLL1_VCXOST  = 0x13,
        HMC7044_REG_GPOS_PLL1_ACT_CX  = 0x14,  HMC7044_REG_GPOS_PLL1_FSM_B0  = 0x15,
        HMC7044_REG_GPOS_PLL1_FSM_B1  = 0x16,  HMC7044_REG_GPOS_PLL1_FSM_B2  = 0x17,
        HMC7044_REG_GPOS_PLL1_HO_EP0  = 0x18,  HMC7044_REG_GPOS_PLL1_HO_EP1  = 0x19,
        HMC7044_REG_GPOS_CH_FSM_BUSY  = 0x1a,  HMC7044_REG_GPOS_SREF_FSM_ST0 = 0x1b,
        HMC7044_REG_GPOS_SREF_FSM_ST1 = 0x1c,  HMC7044_REG_GPOS_SREF_FSM_ST2 = 0x1d,
        HMC7044_REG_GPOS_SREF_FSM_ST3 = 0x1e,  HMC7044_REG_GPOS_FORCE_1      = 0x1f,
        HMC7044_REG_GPOS_FORCE_0      = 0x20,  HMC7044_REG_GPOS_PLL1_HO_DA0  = 0x27,
        HMC7044_REG_GPOS_PLL1_HO_DA1  = 0x28,  HMC7044_REG_GPOS_PLL1_HO_DA2  = 0x29,
        HMC7044_REG_GPOS_PLL1_HO_DA3  = 0x2a,  HMC7044_REG_GPOS_PLL1_HO_DC0  = 0x2b,
        HMC7044_REG_GPOS_PLL1_HO_DC1  = 0x2c,  HMC7044_REG_GPOS_PLL1_HO_DC2  = 0x2d,
        HMC7044_REG_GPOS_PLL1_HO_DC3  = 0x2e,  HMC7044_REG_GPOS_PLL1_HO_CMP  = 0x3d,
        HMC7044_REG_GPOS_PLS_GEN_REQ  = 0x3e
} HMC7044_REG_GPO_SUP;

typedef union {
    struct {
    	HMC7044_REG  gpo2En    : 1; /* GPO2 enable */
    	HMC7044_REG  gpo2Mode  : 1; /* GPO2 Mode */
    	HMC7044_REG  gpo2Sel   : 6; /* GPO2 Selection */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x51;


typedef union {
    struct {
    	HMC7044_REG  gpo3En     : 1; /* GPO3 enable */
    	HMC7044_REG  gpo3Mode   : 1; /* GPO3 Mode */
    	HMC7044_REG  gpo3Sel    : 6; /* GPO3 Selection */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x52;


typedef union {
    struct {
    	HMC7044_REG  gpo4En    : 1; /* GPO4 enable */
    	HMC7044_REG  gpo4Mode  : 1; /* GPO4 Mode */
    	HMC7044_REG  gpo4Sel   : 6; /* GPO4 Selection */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x53;


typedef union {
    struct {
    	HMC7044_REG  sdataEn    : 1; /* SDATA enable */
    	HMC7044_REG  sdataMode  : 1; /* SDATA Mode */
    	HMC7044_REG  resvdBits  : 6;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x54;


#define HMC7044_R54_RESVD1 0x00

/* SYSREF/SYNC control */
typedef union {
    struct {
    	HMC7044_REG  pulseGenMode : 3; /* Pulse Generator Mode */
    	HMC7044_REG  resvdBits    : 5;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x5a;


#define HMC7044_R5A_RESVD1 0x00

typedef enum {
    HMC7044_5A_SRM_LEVEL_CTL  = 0,  HMC7044_5A_SRNP_1  = 1,
    HMC7044_5A_SRNP_2         = 2,  HMC7044_5A_SRNP_4  = 4,
    HMC7044_5A_SRNP_8         = 4,  HMC7044_5A_SRNP_16 = 5,
    HMC7044_5A_SRM_CONTINUOUS = 7
} HMC7044_5A_SREF_MODE;

typedef union {
    struct {
    	HMC7044_REG  syncPolarity   : 1; /* SYNC polarity */
    	HMC7044_REG  syncThruPLL2   : 1; /* SYNC through PLL2 */
    	HMC7044_REG  syncRetime     : 1; /* SYNC retime */
    	HMC7044_REG  resvdBits      : 5;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x5b;


#define HMC7044_R5B_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG  lsbSysrefTimer : 8; /* SYSREF Timer LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x5c;


typedef union {
    struct {
    	HMC7044_REG  msbSysrefTimer : 4; /* SYSREF Timer MSB */
    	HMC7044_REG  resvdBits      : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x5d;


#define HMC7044_R5D_RESVD1 0x0

typedef union {
    struct {
    	HMC7044_REG  resvdBits    : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x5e;


#define HMC7044_R5E_RESVD1 0x00

/* Clock Distribution Network */
typedef union {
    struct {
    	HMC7044_REG lowFreqExtVco : 1; /* Low frequency external VCO path */
    	HMC7044_REG divBy2ExtVco  : 1; /* Divide by 2 on external VCO enable */
    	HMC7044_REG resvdBits     : 6;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x64;


#define HMC7044_R64_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG anlgDelayLowPower : 1; /* Analog delay low power mode */
    	HMC7044_REG resvdBits         : 7;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x65;


#define HMC7044_R65_RESVD1 0x00

/* Alarm Masks */

typedef union {
    struct {
    	HMC7044_REG clkinLosMask	         : 4; /* CLKIN LOS Mask */
    	HMC7044_REG pll1HoldoverStatusMask   : 1; /* PLL1 holdover status mask */
    	HMC7044_REG pll1LockDetectMask       : 1; /* PLL1 lock detect mask */
    	HMC7044_REG pll1LockAcquisitionMask  : 1; /* PLL1 lock acquisition mask */
    	HMC7044_REG pll1NearLockMask         : 1; /* PLL1 near lock mask */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x70;


typedef union {
    struct {
    	HMC7044_REG pll2LockDetectMask         : 1; /* PLL2 lock detect mask */
    	HMC7044_REG sysrefSyncStatusMask       : 1; /* SYSREF sync status mask */
    	HMC7044_REG clockOutputPhaseStatusMask : 1; /* Output phase status mask */
    	HMC7044_REG pll1AndPll2LockDetectMask  : 1; /* PLL,PLL2 lock detect mask */
    	HMC7044_REG syncRequestMask            : 1; /* Sync request mask */
    	HMC7044_REG resvdBits                  : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x71;


#define HMC7044_R71_RESVD1 0x0

/* Product ID */
typedef union {
    struct {
    	HMC7044_REG lsbProductIDValue  : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x78;


typedef union {
    struct {
    	HMC7044_REG midProductIDValue  : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x79;


typedef union {
    struct {
    	HMC7044_REG msbProductIDValue  : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x7a;


#define HMC7044_PRODUCT_ID  0x045201

/* Alarm Readback */
typedef union {
    struct {
    	HMC7044_REG alarmSignal   : 1; /* Alarm signal */
    	HMC7044_REG resvdBits     : 7;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x7b;


#define HMC7044_R7B_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG clkinLos           : 4; /* CLKIN LOS */
    	HMC7044_REG pll1HoldoverStatus : 1; /* PLL1 holdover status */
    	HMC7044_REG pll1LockDetect     : 1; /* PLL1 lock detect */
    	HMC7044_REG pll1LockAcquisition: 1; /* PLL1 lock acquisition */
    	HMC7044_REG pll1NearLock       : 1; /* PLL1 near lock */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x7c;


typedef union {
    struct {
    	HMC7044_REG pll2LockDetect         : 1; /* PLL2 lock detect */
    	HMC7044_REG sysrefSyncStatus       : 1; /* Sysref Sync status */
    	HMC7044_REG clockOutputPhaseStatus : 1; /* clock output phase status */
    	HMC7044_REG pll1AndPll2LockDetect  : 1; /* PLL1 and PLL2 lock detect */
    	HMC7044_REG syncRequestStatus      : 1; /* Sync request status */
    	HMC7044_REG resvdBits              : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x7d;


#define HMC7044_R7D_RESVD1 0x0

typedef union {
    struct {
    	HMC7044_REG clkinLosLatched         : 4; /* CLKIN LOS latched */
    	HMC7044_REG pll1Holdoverlatched     : 1; /* PLL1 holdover latched */
    	HMC7044_REG pll1Acquisitionlatched  : 1; /* PLL1 lock acquisition latched */
    	HMC7044_REG pll2Acquisitionlatched  : 1; /* PLL2 lock acquisition latched */
    	HMC7044_REG resvdBit                : 1;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x7e;


#define HMC7044_R7E_RESVD1 0

typedef union {
    struct {
    	HMC7044_REG  resvdBits    : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x7f;


#define HMC7044_R7F_RESVD1 0x00

/* PLL1 status */
typedef union {
    struct {
    	HMC7044_REG pll1FsmState      : 3; /* PLL1 FSM state */
    	HMC7044_REG pll1ActiveClkin   : 2; /* PLL1 Active CLKIN */
    	HMC7044_REG pll1BestClock     : 2; /* PLL1 Best clock */
    	HMC7044_REG resvdBit          : 1;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x82;


#define HMC7044_R82_RESVD1 0

typedef union {
    struct {
    	HMC7044_REG pll1HoldoverDacAvgdVal : 7; /* PLL1 Holdover DAC Avged Value */
    	HMC7044_REG resvdBit               : 1;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x83;


#define HMC7044_R83_RESVD1 0

typedef union {
    struct {
    	HMC7044_REG pll1HoldoverDacCurrentVal : 7; /* PLL1 HO DAC current Value */
    	HMC7044_REG holdoverComparatorVal     : 1; /* Holdover comparator value */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x84;


typedef union {
    struct {
    	HMC7044_REG pll1HoldoverAdcInpRangeStatus : 1; /* PLL1 HO ADC input range */
    	HMC7044_REG pll1HoldoverAdcStatus         : 1; /* PLL1 HO ADC status */
    	HMC7044_REG pll1VcxoStatus                : 1; /* PLL1 VCXO status */
    	HMC7044_REG pll1activeClkinLos            : 1; /* PLL1 active CLKIN los */
    	HMC7044_REG resvdBits                     : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x85;


#define HMC7044_R85_RESVD1 0x0

typedef union {
    struct {
    	HMC7044_REG resvdBits1            : 3;
    	HMC7044_REG pll1HoldoverExitPhase : 2;
    	HMC7044_REG resvdBits2            : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x86;


#define HMC7044_R86_RESVD1 0x0
#define HMC7044_R86_RESVD2 0x0

typedef union {
    struct {
    	HMC7044_REG  resvdBits     : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x87;


#define HMC7044_R87_RESVD1 0x00

/* PLL2 status */
typedef union {
    struct {
    	HMC7044_REG pll2Autotune    : 8; /* PLL2 autotune value */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x8c;


typedef union {
    struct {
    	HMC7044_REG lsbPll2AutotuneSignedErr : 8; /* PLL2 autotune signed err LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x8d;


typedef union {
    struct {
    	HMC7044_REG msbPll2AutotuneSignedErr : 6; /* PLL2 autotune signed err MSB */
    	HMC7044_REG pll2AutotuneErrSign      : 1; /* PLL2 autotune error sign */
    	HMC7044_REG pll2AutotuneStatus       : 1; /* PLL2 autotune status */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x8e;


typedef union {
    struct {
    	HMC7044_REG pll2SyncFsmState     : 4; /* PLL2 SYNC FSM state */
    	HMC7044_REG pll2AutotuneFsmState : 4; /* PLL2 Autotune FSM state */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x8f;


typedef union {
    struct {
    	HMC7044_REG resvdBits   : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x90;


#define HMC7044_R90_RESVD1 0x00

/* SYSREF status */
typedef union {
    struct {
    	HMC7044_REG sysrefFsmState   : 4; /* SYSREF FSM state */
    	HMC7044_REG chOutputFsmBusy  : 1; /* Channel outputs FSM busy */
    	HMC7044_REG resvdBits        : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x91;


#define HMC7044_R91_RESVD1 0x0

/* Other Controls */
typedef union {
    struct {
    	HMC7044_REG resvdBits   : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x96;


#define HMC7044_R96_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG resvdBits  : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x97;


#define HMC7044_R97_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x98;


#define HMC7044_R98_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x99;


#define HMC7044_R99_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x9a;


#define HMC7044_R9A_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x9b;


#define HMC7044_R9B_RESVD1 0xaa

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x9c;


#define HMC7044_R9C_RESVD1 0xaa

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x9d;


#define HMC7044_R9D_RESVD1 0xaa

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x9e;


#define HMC7044_R9E_RESVD1 0xaa

typedef union {
    struct {
    	HMC7044_REG clkOutDrvLowPowSetting : 8; /* CLKOUT drv low power setting */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x9f;


typedef union {
    struct {
    	HMC7044_REG clkOutDrvHighPowSetting : 8; /* CLKOUT drv high power setting */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xa0;


typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xa1;


#define HMC7044_RA1_RESVD1 0x97

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xa2;


#define HMC7044_RA2_RESVD1 0x03

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xa3;


#define HMC7044_RA3_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xa4;


#define HMC7044_RA4_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG pll1MoreDelay : 8; /* PLL1 more delay */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xa5;


typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xa6;


#define HMC7044_RA6_RESVD1 0x1c

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xa7;


#define HMC7044_RA7_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG pll1HoldoverDacGm : 8; /* PLL1 Hold over DAC gm setting */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xa8;


typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xa9;


#define HMC7044_RA9_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xab;


#define HMC7044_RAB_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xac;


#define HMC7044_RAC_RESVD1 0x20

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xad;


#define HMC7044_RAD_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xae;


#define HMC7044_RAE_RESVD1 0x08

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xaf;

#define HMC7044_RAF_RESVD1 0x50

typedef union {
    struct {
    	HMC7044_REG vtunePresetSetting : 8; /* VTUNE preset setting */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xb0;

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xb1;

#define HMC7044_RB1_RESVD1 0x0d

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xb2;

#define HMC7044_RB2_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xb3;

#define HMC7044_RB3_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xb5;

#define HMC7044_RB5_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xb6;

#define HMC7044_RB6_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xb7;

#define HMC7044_RB7_RESVD1 0x00

typedef union {
    struct {
    	HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xb8;

#define HMC7044_RB8_RESVD1 0x00

/* Clock Distribution */

/* Channel Output 0 */
typedef union {
    struct {
        HMC7044_REG chout0ChannelEn    : 1; /* Channel Enable */
        HMC7044_REG chout0MultiSlipEn  : 1; /* MultiSlip Enable */
        HMC7044_REG chout0StMode       : 2; /* Start-up mode */
        HMC7044_REG resvdBit           : 1;
        HMC7044_REG chout0SlipEn       : 1; /* Slip Enable */
        HMC7044_REG chout0SyncEn       : 1; /* SYNC Enable */
        HMC7044_REG chout0HighPerfMode : 1; /* High Performance mode */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xc8;

#define HMC7044_RC8_RESVD1 1

typedef union {
    struct {
       HMC7044_REG chout0LsbChannelDiv : 8; /* Channel Divider[7:0] LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xc9;

typedef union {
    struct {
        HMC7044_REG chout0MsbChannelDiv  : 4; /* Channel Divider[11:8] MSB */
        HMC7044_REG resvdBits            : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xca;

#define HMC7044_RCA_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout0FineAnlgDelay : 5; /* Fine analog Delay */
        HMC7044_REG resvdBits           : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xcb;

#define HMC7044_RCB_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout0CoarseDigtlDelay  : 5; /* Coarse Digital Delay */
        HMC7044_REG resvdBits               : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xcc;

#define HMC7044_RCC_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout0LsbmultiSlipDigtlDelay : 8; /* MultiSlip Dig Delay LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xcd;

typedef union {
    struct {
        HMC7044_REG chout0MsbMultiSlipDigtlDelay : 4; /* MultiSlip Dig Delay MSB */
        HMC7044_REG resvdBits                    : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xce;

#define HMC7044_RCE_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout0OutputMuxSel  : 2;  /* Output Mux Selection */
        HMC7044_REG resvdBits           : 6;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xcf;

#define HMC7044_RCF_RESVD1 0x00

typedef union {
    struct {
        HMC7044_REG chout0DriverImp       : 2; /* Driver Impedance */
        HMC7044_REG resvdBit              : 1;
        HMC7044_REG chout0DriverMode      : 2; /* Driver mode */
        HMC7044_REG chout0DynamicDriverEn : 1; /* Dynamic Driver Enable */
        HMC7044_REG chout0ForceMute       : 2; /* Force Mute */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xd0;

#define HMC7044_RD0_RESVD1 0

typedef union {
    struct {
        HMC7044_REG resvdBits  : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xd1;

#define HMC7044_RD1_RESVD1 0x00

/* Channel Output 1 */
typedef union {
    struct {
        HMC7044_REG chout1ChannelEn     : 1; /* Channel Enable */
        HMC7044_REG chout1MultiSlipEn   : 1; /* MultiSlip Enable */
        HMC7044_REG chout1StMode        : 2; /* Start-up mode */
        HMC7044_REG resvdBit            : 1;
        HMC7044_REG chout1SlipEn        : 1; /* Slip Enable */
        HMC7044_REG chout1SyncEn        : 1; /* SYNC Enable */
        HMC7044_REG chout1HighPerfMode  : 1; /* High Performance mode */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xd2;

#define HMC7044_RD2_RESVD1 1

typedef union {
    struct {
        HMC7044_REG chout1LsbChannelDiv : 8; /* Channel Divider[7:0] LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xd3;

typedef union {
    struct {
        HMC7044_REG chout1MsbChannelDiv  : 4; /* Channel Divider[11:8] MSB */
        HMC7044_REG resvdBits            : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xd4;

#define HMC7044_RD4_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout1FineAnlgDelay  : 5; /* Fine analog Delay */
        HMC7044_REG resvdBits            : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xd5;

#define HMC7044_RD5_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout1CoarseDigtlDelay : 5; /* Coarse Digital Delay */
        HMC7044_REG resvdBits              : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xd6;

#define HMC7044_RD6_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout1LsbmultiSlipDigtlDelay : 8;  /* MultiSlip Dig Delay LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xd7;

typedef union {
    struct {
        HMC7044_REG chout1MsbMultiSlipDigtlDelay   : 4;  /* MultiSlip Dig Delay MSB */
       	HMC7044_REG resvdBits                      : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xd8;

#define HMC7044_RD8_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout1OutputMuxSel   : 2;  /* Output Mux Selection */
        HMC7044_REG resvdBits            : 6;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xd9;

#define HMC7044_RD9_RESVD1 0x00

typedef union {
    struct {
        HMC7044_REG chout1DriverImp       : 2; /* Driver Impedance */
        HMC7044_REG resvdBit              : 1;
        HMC7044_REG chout1DriverMode      : 2; /* Driver mode */
        HMC7044_REG chout1DynamicDriverEn : 1; /* Dynamic Driver Enable */
        HMC7044_REG chout1ForceMute       : 2; /* Force Mute */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xda;

#define HMC7044_RDA_RESVD1 0

typedef union {
    struct {
        HMC7044_REG resvdBits   : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xdb;

#define HMC7044_RDB_RESVD1 0x00

/* Channel Output 2 */
typedef union {
    struct {
        HMC7044_REG chout2ChannelEn      : 1; /* Channel Enable */
        HMC7044_REG chout2MultiSlipEn    : 1; /* MultiSlip Enable */
        HMC7044_REG chout2StMode         : 2; /* Start-up mode */
        HMC7044_REG resvdBit             : 1;
        HMC7044_REG chout2SlipEn         : 1; /* Slip Enable */
        HMC7044_REG chout2SyncEn         : 1; /* SYNC Enable */
        HMC7044_REG chout2HighPerfMode   : 1; /* High Performance mode */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xdc;

#define HMC7044_RDC_RESVD1 1

typedef union {
    struct {
        HMC7044_REG chout2LsbChannelDiv : 8;  /* Channel Divider[7:0] LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xdd;

typedef union {
    struct {
        HMC7044_REG chout2MsbChannelDiv : 4; /* Channel Divider[11:8] MSB */
        HMC7044_REG resvdBits           : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xde;

#define HMC7044_RDE_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout2FineAnlgDelay : 5; /* Fine analog Delay */
        HMC7044_REG resvdBits           : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xdf;

#define HMC7044_RDF_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout2CoarseDigtlDelay : 5;  /* Coarse Digital Delay */
        HMC7044_REG resvdBits              : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xe0;

#define HMC7044_RE0_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout2LsbmultiSlipDigtlDelay : 8; /* MultiSlip Dig Delay LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xe1;

typedef union {
    struct {
        HMC7044_REG chout2MsbMultiSlipDigtlDelay : 4; /* MultiSlip Dig Delay MSB */
        HMC7044_REG resvdBits                    : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xe2;

#define HMC7044_RE2_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout2OutputMuxSel : 2;  /* Output Mux Selection */
        HMC7044_REG resvdBits          : 6;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xe3;

#define HMC7044_RE3_RESVD1 0x00

typedef union {
    struct {
        HMC7044_REG chout2DriverImp       : 2; /* Driver Impedance */
        HMC7044_REG resvdBit              : 1;
        HMC7044_REG chout2DriverMode      : 2; /* Driver mode */
        HMC7044_REG chout2DynamicDriverEn : 1; /* Dynamic Driver Enable */
        HMC7044_REG chout2ForceMute       : 2; /* Force mute */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xe4;

#define HMC7044_RE4_RESVD1 0

typedef union {
    struct {
        HMC7044_REG resvdBits   : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xe5;

#define HMC7044_RE5_RESVD1 0x00

/* Channel Output 3 */
typedef union {
    struct {
        HMC7044_REG chout3ChannelEn      : 1; /* Channel Enable */
        HMC7044_REG chout3MultiSlipEn    : 1; /* MultiSlip Enable */
        HMC7044_REG chout3StMode         : 2; /* Start-up mode */
        HMC7044_REG resvdBit             : 1;
        HMC7044_REG chout3SlipEn         : 1; /* Slip Enable */
        HMC7044_REG chout3SyncEn         : 1; /* SYNC Enable */
        HMC7044_REG chout3HighPerfMode   : 1; /* High Performance mode */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xe6;

#define HMC7044_RE6_RESVD1 1

typedef union {
    struct {
        HMC7044_REG chout3LsbChannelDiv : 8; /* Channel Divider[7:0] LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xe7;

typedef union {
    struct {
        HMC7044_REG chout3MsbChannelDiv : 4; /* Channel Divider[11:8] MSB */
        HMC7044_REG resvdBits           : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xe8;

#define HMC7044_RE8_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout3FineAnlgDelay : 5;  /* Fine analog Delay */
        HMC7044_REG resvdBits           : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xe9;

#define HMC7044_RE9_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout3CoarseDigtlDelay  : 5; /* Coarse Digital Delay */
        HMC7044_REG resvdBits               : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xea;

#define HMC7044_REA_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout3LsbmultiSlipDigtlDelay:8; /* MultiSlip Dig Delay LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xeb;

typedef union {
    struct {
        HMC7044_REG chout3MsbMultiSlipDigtlDelay  : 4; /* MultiSlip Dig Delay MSB */
        HMC7044_REG  resvdBits                    : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xec;

#define HMC7044_REC_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout3OutputMuxSel : 2; /* Output Mux Selection */
        HMC7044_REG resvdBits          : 6;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xed;

#define HMC7044_RED_RESVD1 0x00

typedef union {
    struct {
        HMC7044_REG chout3DriverImp       : 2; /* Driver Impedance */
        HMC7044_REG resvdBit              : 1;
        HMC7044_REG chout3DriverMode      : 2; /* Driver mode */
        HMC7044_REG chout3DynamicDriverEn : 1; /* Dynamic Driver Enable */
        HMC7044_REG chout3ForceMute       : 2; /* Force Mute */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xee;

#define HMC7044_REE_RESVD1 0

typedef union {
    struct {
        HMC7044_REG  resvdBits  : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xef;

#define HMC7044_REF_RESVD1 0x00

/* Channel Output 4 */
typedef union {
    struct {
        HMC7044_REG chout4ChannelEn      : 1; /* Channel Enable */
        HMC7044_REG chout4MultiSlipEn    : 1; /* MultiSlip Enable */
        HMC7044_REG chout4StMode         : 2; /* Start-up mode */
        HMC7044_REG resvdBit             : 1;
        HMC7044_REG chout4SlipEn         : 1; /* Slip Enable */
        HMC7044_REG chout4SyncEn         : 1; /* SYNC Enable */
        HMC7044_REG chout4HighPerfMode   : 1; /* High Performance mode */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xf0;

#define HMC7044_RF0_RESVD1 1

typedef union {
    struct {
        HMC7044_REG chout4LsbChannelDiv :8;  /* Channel Divider[7:0] LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xf1;

typedef union {
    struct {
        HMC7044_REG chout4MsbChannelDiv : 4;  /* Channel Divider[7:0] MSB */
        HMC7044_REG resvdBits           : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xf2;

#define HMC7044_RF2_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout4FineAnlgDelay : 5; /* Fine analog Delay */
        HMC7044_REG resvdBits           : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xf3;

#define HMC7044_RF3_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout4CoarseDigtlDelay : 5; /* Coarse Digital Delay */
        HMC7044_REG resvdBits              : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xf4;

#define HMC7044_RF4_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout4LsbmultiSlipDigtlDelay : 8; /* MultiSlip Dig Delay LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xf5;

typedef union {
    struct {
        HMC7044_REG chout4MsbMultiSlipDigtlDelay : 4; /* MultiSlip Dig Delay MSB */
        HMC7044_REG resvdBits                    : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xf6;

#define HMC7044_RF6_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout4OutputMuxSel : 2; /* Output Mux Selection */
        HMC7044_REG resvdBits          : 6;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xf7;

#define HMC7044_RF7_RESVD1 0x00

typedef union {
    struct {
        HMC7044_REG chout4DriverImp         : 2; /* Driver Impedance */
        HMC7044_REG resvdBit                : 1;
        HMC7044_REG chout4DriverMode        : 2; /* Driver mode */
        HMC7044_REG chout4DynamicDriverEn   : 1; /* Dynamic Driver Enable */
        HMC7044_REG chout4ForceMute         : 2; /* Force Mute */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xf8;

#define HMC7044_RF8_RESVD1 0

typedef union {
    struct {
        HMC7044_REG  resvdBits  : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xf9;

#define HMC7044_RF9_RESVD1 0x00

/* Channel Output 5 */
typedef union {
    struct {
        HMC7044_REG chout5ChannelEn      : 1; /* Channel Enable */
        HMC7044_REG chout5MultiSlipEn    : 1; /* MultiSlip Enable */
        HMC7044_REG chout5StMode         : 2; /* Start-up mode */
        HMC7044_REG resvdBit             : 1;
        HMC7044_REG chout5SlipEn         : 1; /* Slip Enable */
        HMC7044_REG chout5SyncEn         : 1; /* SYNC Enable */
        HMC7044_REG chout5HighPerfMode   : 1; /* High Performance mode */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xfa;

#define HMC7044_RFA_RESVD1 1

typedef union {
    struct {
        HMC7044_REG chout5LsbChannelDiv : 8;  /* Channel Divider[7:0] LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xfb;

typedef union {
    struct {
        HMC7044_REG chout5MsbChannelDiv : 4; /* Channel Divider[11:8] MSB */
        HMC7044_REG resvdBits           : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xfc;

#define HMC7044_RFC_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout5FineAnlgDelay : 5; /* Fine analog Delay */
        HMC7044_REG resvdBits           : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xfd;

#define HMC7044_RFD_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout5CoarseDigtlDelay : 5;  /* Coarse Digital Delay */
        HMC7044_REG resvdBits              : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xfe;

#define HMC7044_RFE_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout5LsbmultiSlipDigtlDelay : 8;  /* MultiSlip Dig Delay LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_xff;

typedef union {
    struct {
        HMC7044_REG chout5MsbMultiSlipDigtlDelay : 4;  /* MultiSlip Dig Delay MSB */
        HMC7044_REG resvdBits                    : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x100;

#define HMC7044_R100_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout5OutputMuxSel : 2; /* Output Mux Selection */
        HMC7044_REG resvdBits          : 6;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x101;

#define HMC7044_R101_RESVD1 0x00

typedef union {
    struct {
        HMC7044_REG chout5DriverImp        : 2; /* Driver Impedance */
        HMC7044_REG resvdBit               : 1;
        HMC7044_REG chout5DriverMode       : 2; /* Driver mode */
        HMC7044_REG chout5DynamicDriverEn  : 1; /* Dynamic Driver Enable */
        HMC7044_REG chout5ForceMute        : 2; /* Force Mute */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x102;

#define HMC7044_R102_RESVD1 0

typedef union {
    struct {
        HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x103;

#define HMC7044_R103_RESVD1 0x00

/* Channel Output 6 */
typedef union {
    struct {
        HMC7044_REG chout6ChannelEn      : 1; /* Channel Enable */
        HMC7044_REG chout6MultiSlipEn    : 1; /* MultiSlip Enable */
        HMC7044_REG chout6StMode         : 2; /* Start-up mode */
        HMC7044_REG resvdBit             : 1;
        HMC7044_REG chout6SlipEn         : 1; /* Slip Enable */
        HMC7044_REG chout6SyncEn         : 1; /* SYNC Enable */
        HMC7044_REG chout6HighPerfMode   : 1; /* High Performance mode */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x104;

#define HMC7044_R104_RESVD1 1

typedef union {
    struct {
        HMC7044_REG chout6LsbChannelDiv : 8;  /* Channel Divider[7:0] LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x105;

typedef union {
    struct {
        HMC7044_REG chout6MsbChannelDiv : 4; /* Channel Divider[11:8] MSB */
        HMC7044_REG resvdBits           : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x106;

#define HMC7044_R106_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout6FineAnlgDelay : 5; /* Fine analog Delay */
        HMC7044_REG resvdBits           : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x107;

#define HMC7044_R107_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout6CoarseDigtlDelay : 5; /* Coarse Digital Delay */
        HMC7044_REG resvdBits              : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x108;

#define HMC7044_R108_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout6LsbmultiSlipDigtlDelay : 8; /* MultiSlip Dig Delay LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x109;

typedef union {
    struct {
        HMC7044_REG chout6MsbMultiSlipDigtlDelay : 4; /* MultiSlip Dig Delay MSB */
        HMC7044_REG resvdBits                    : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x10a;

#define HMC7044_R10A_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout6OutputMuxSel : 2; /* Output Mux Selection */
        HMC7044_REG resvdBits          : 6;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x10b;

#define HMC7044_R10B_RESVD1 0x00

typedef union {
    struct {
        HMC7044_REG chout6DriverImp        : 2; /* Driver Impedance */
        HMC7044_REG resvdBit               : 1;
        HMC7044_REG chout6DriverMode       : 2; /* Driver mode */
        HMC7044_REG chout6DynamicDriverEn  : 1; /* Dynamic Driver Enable */
        HMC7044_REG chout6ForceMute        : 2; /* Force Mute */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x10c;

#define HMC7044_R10C_RESVD1 0

typedef union {
    struct {
        HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x10d;

#define HMC7044_R10D_RESVD1 0x00

/* Channel Output 7 */
typedef union {
    struct {
        HMC7044_REG chout7ChannelEn     : 1; /* Channel Enable */
        HMC7044_REG chout7MultiSlipEn   : 1; /* MultiSlip Enable */
        HMC7044_REG chout7StMode        : 2; /* Start-up mode */
        HMC7044_REG resvdBit            : 1;
        HMC7044_REG chout7SlipEn        : 1; /* Slip Enable */
        HMC7044_REG chout7SyncEn        : 1; /* SYNC Enable */
        HMC7044_REG chout7HighPerfMode  : 1; /* High Performance mode */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x10e;

#define HMC7044_R10E_RESVD1 1

typedef union {
    struct {
        HMC7044_REG chout7LsbChannelDiv : 8; /* Channel Divider[7:0] LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x10f;

typedef union {
    struct {
        HMC7044_REG chout7MsbChannelDiv : 4; /* Channel Divider[11:8] MSB */
        HMC7044_REG resvdBits           : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x110;

#define HMC7044_R110_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout7FineAnlgDelay : 5; /* Fine analog Delay */
        HMC7044_REG resvdBits           : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x111;

#define HMC7044_R111_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout7CoarseDigtlDelay : 5;  /* Coarse Digital Delay */
        HMC7044_REG resvdBits              : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x112;

#define HMC7044_R112_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout7LsbmultiSlipDigtlDelay : 8; /* MultiSlip Dig Delay LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x113;

typedef union {
    struct {
        HMC7044_REG chout7MsbMultiSlipDigtlDelay : 4; /* MultiSlip Dig Delay MSB */
        HMC7044_REG resvdBits                    : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x114;

#define HMC7044_R114_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout7OutputMuxSel : 2; /* Output Mux Selection */
        HMC7044_REG resvdBits          : 6;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x115;

#define HMC7044_R115_RESVD1 0x00

typedef union {
    struct {
        HMC7044_REG chout7DriverImp       : 2; /* Driver Impedance */
        HMC7044_REG resvdBit              : 1;
        HMC7044_REG chout7DriverMode      : 2; /* Driver mode */
        HMC7044_REG chout7DynamicDriverEn : 1; /* Dynamic Driver Enable */
        HMC7044_REG chout7ForceMute       : 2; /* Force Mute */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x116;

#define HMC7044_R116_RESVD1 0

typedef union {
    struct {
        HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x117;

#define HMC7044_R117_RESVD1 0x00

/* Channel Output 8 */
typedef union {
    struct {
        HMC7044_REG chout8ChannelEn      : 1; /* Channel Enable */
        HMC7044_REG chout8MultiSlipEn    : 1; /* MultiSlip Enable */
        HMC7044_REG chout8StMode         : 2; /* Start-up mode */
        HMC7044_REG resvdBit             : 1;
        HMC7044_REG chout8SlipEn         : 1; /* Slip Enable */
        HMC7044_REG chout8SyncEn         : 1; /* SYNC Enable */
        HMC7044_REG chout8HighPerfMode   : 1; /* High Performance mode */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x118;

#define HMC7044_R118_RESVD1 1

typedef union {
    struct {
        HMC7044_REG chout8LsbChannelDiv : 8; /* Channel Divider[7:0] LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x119;

typedef union {
    struct {
        HMC7044_REG chout8MsbChannelDiv : 4;  /* Channel Divider[11:8] MSB */
        HMC7044_REG resvdBits           : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x11a;

#define HMC7044_R11A_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout8FineAnlgDelay : 5;  /* Fine analog Delay */
        HMC7044_REG resvdBits           : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x11b;

#define HMC7044_R11B_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout8CoarseDigtlDelay : 5; /* Coarse Digital Delay */
        HMC7044_REG resvdBits              : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x11c;

#define HMC7044_R11C_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout8LsbmultiSlipDigtlDelay : 8; /* MultiSlip Dig Delay LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x11d;

typedef union {
    struct {
        HMC7044_REG chout8MsbMultiSlipDigtlDelay : 4; /* MultiSlip Dig Delay MSB */
        HMC7044_REG resvdBits                    : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x11e;

#define HMC7044_R11E_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout8OutputMuxSel : 2; /* Output Mux Selection */
        HMC7044_REG resvdBits          : 6;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x11f;

#define HMC7044_R11F_RESVD1 0x00

typedef union {
    struct {
        HMC7044_REG chout8DriverImp       : 2; /* Driver Impedance */
        HMC7044_REG resvdBit              : 1;
        HMC7044_REG chout8DriverMode      : 2; /* Driver mode */
        HMC7044_REG chout8DynamicDriverEn : 1; /* Dynamic Driver Enable */
        HMC7044_REG chout8ForceMute       : 2; /* Force Mute */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x120;

#define HMC7044_R120_RESVD1 0

typedef union {
    struct {
        HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x121;

#define HMC7044_R121_RESVD1 0x00

/* Channel Output 9 */
typedef union {
    struct {
        HMC7044_REG chout9ChannelEn     : 1; /* Channel Enable */
        HMC7044_REG chout9MultiSlipEn   : 1; /* MultiSlip Enable */
        HMC7044_REG chout9StMode        : 2; /* Start-up mode */
        HMC7044_REG resvdBit            : 1;
        HMC7044_REG chout9SlipEn        : 1; /* Slip Enable */
        HMC7044_REG chout9SyncEn        : 1; /* SYNC Enable */
        HMC7044_REG chout9HighPerfMode  : 1; /* High Performance mode */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x122;

#define HMC7044_R122_RESVD1 1

typedef union {
    struct {
        HMC7044_REG chout9LsbChannelDiv : 8; /* Channel Divider[7:0] LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x123;

typedef union {
    struct {
        HMC7044_REG chout9MsbChannelDiv : 4; /* Channel Divider[7:0] MSB */
        HMC7044_REG resvdBits           : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x124;

#define HMC7044_R124_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout9FineAnlgDelay : 5; /* Fine analog Delay */
        HMC7044_REG resvdBits           : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x125;

#define HMC7044_R125_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout9CoarseDigtlDelay : 5; /* Coarse Digital Delay */
        HMC7044_REG resvdBits              : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x126;

#define HMC7044_R126_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout9LsbmultiSlipDigtlDelay : 8; /* MultiSlip Dig Delay LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x127;

typedef union {
    struct {
        HMC7044_REG chout9MsbMultiSlipDigtlDelay : 4;  /* MultiSlip Dig Delay MSB */
        HMC7044_REG resvdBits                    : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x128;

#define HMC7044_R128_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout9OutputMuxSel : 2; /* Output Mux Selection */
        HMC7044_REG resvdBits          : 6;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x129;

#define HMC7044_R129_RESVD1 0x00

typedef union {
    struct {
        HMC7044_REG chout9DriverImp       : 2; /* Driver Impedance */
        HMC7044_REG resvdBit              : 1;
        HMC7044_REG chout9DriverMode      : 2; /* Driver mode */
        HMC7044_REG chout9DynamicDriverEn : 1; /* Dynamic Driver Enable */
        HMC7044_REG chout9ForceMute       : 2; /* Force Mute */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x12a;

#define HMC7044_R12A_RESVD1 0

typedef union {
    struct {
        HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x12b;

#define HMC7044_R12B_RESVD1 0x00

/* Channel Output 10 */
typedef union {
    struct {
        HMC7044_REG chout10ChannelEn     : 1; /* Channel Enable */
        HMC7044_REG chout10MultiSlipEn   : 1; /* MultiSlip Enable */
        HMC7044_REG chout10StMode        : 2; /* Start-up mode */
        HMC7044_REG resvdBit             : 1;
        HMC7044_REG chout10SlipEn        : 1; /* Slip Enable */
        HMC7044_REG chout10SyncEn        : 1; /* SYNC Enable */
        HMC7044_REG chout10HighPerfMode  : 1; /* High Performance mode */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x12c;

#define HMC7044_R12C_RESVD1 1

typedef union {
    struct {
        HMC7044_REG chout10LsbChannelDiv : 8; /* Channel Divider[7:0] LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x12d;

typedef union {
    struct {
        HMC7044_REG chout10MsbChannelDiv : 4; /* Channel Divider[7:0] MSB */
        HMC7044_REG resvdBits            : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x12e;

#define HMC7044_R12E_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout10FineAnlgDelay : 5; /* Fine analog Delay */
        HMC7044_REG resvdBits            : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x12f;

#define HMC7044_R12F_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout10CoarseDigtlDelay : 5; /* Coarse Digital Delay */
        HMC7044_REG resvdBits               : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x130;

#define HMC7044_R130_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout10LsbmultiSlipDigtlDelay : 8; /* MultiSlip Dig Delay LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x131;

#define HMC7044_R131_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout10MsbMultiSlipDigtlDelay : 4;  /* MultiSlip Dig Delay MSB */
        HMC7044_REG resvdBits                     : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x132;

#define HMC7044_R132_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout10OutputMuxSel : 2;  /* Output Mux Selection */
        HMC7044_REG resvdBits           : 6;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x133;

#define HMC7044_R133_RESVD1 0x00

typedef union {
    struct {
        HMC7044_REG chout10DriverImp       : 2; /* Driver Impedance */
        HMC7044_REG resvdBit               : 1;
        HMC7044_REG chout10DriverMode      : 2; /* Driver mode */
        HMC7044_REG chout10DynamicDriverEn : 1; /* Dynamic Driver Enable */
        HMC7044_REG chout10ForceMute       : 2; /* Force Mute */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x134;

#define HMC7044_R134_RESVD1 0

typedef union {
    struct {
        HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x135;

#define HMC7044_R135_RESVD1 0x00

/* Channel Output 11 */
typedef union {
    struct {
        HMC7044_REG chout11ChannelEn     : 1; /* Channel Enable */
        HMC7044_REG chout11MultiSlipEn   : 1; /* MultiSlip Enable */
        HMC7044_REG chout11StMode        : 2; /* Start-up mode */
        HMC7044_REG resvdBit             : 1;
        HMC7044_REG chout11SlipEn        : 1; /* Slip Enable */
        HMC7044_REG chout11SyncEn        : 1; /* SYNC Enable */
        HMC7044_REG chout11HighPerfMode  : 1; /* High Performance mode */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x136;

#define HMC7044_R136_RESVD1 1

typedef union {
    struct {
        HMC7044_REG chout11LsbChannelDiv : 8; /* Channel Divider[7:0] LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x137;

typedef union {
    struct {
        HMC7044_REG chout11MsbChannelDiv : 4; /* Channel Divider[7:0] MSB */
        HMC7044_REG resvdBits            : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x138;

#define HMC7044_R138_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout11FineAnlgDelay : 5; /* Fine analog Delay */
        HMC7044_REG resvdBits            : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x139;

#define HMC7044_R139_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout11CoarseDigtlDelay : 5; /* Coarse Digital Delay */
        HMC7044_REG resvdBits               : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x13a;

#define HMC7044_R13A_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout11LsbmultiSlipDigtlDelay : 8; /* MultiSlip Dig Delay LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x13b;

typedef union {
    struct {
        HMC7044_REG chout11MsbMultiSlipDigtlDelay : 4; /* MultiSlip Dig Delay MSB */
        HMC7044_REG resvdBits                     : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x13c;

#define HMC7044_R13C_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout11OutputMuxSel : 2; /* Output Mux Selection */
        HMC7044_REG resvdBits           : 6;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x13d;

#define HMC7044_R13D_RESVD1 0x00

typedef union {
    struct {
        HMC7044_REG chout11DriverImp       : 2; /* Driver Impedance */
        HMC7044_REG resvdBit               : 1;
        HMC7044_REG chout11DriverMode      : 2; /* Driver mode */
        HMC7044_REG chout11DynamicDriverEn : 1; /* Dynamic Driver Enable */
        HMC7044_REG chout11ForceMute       : 2; /* Force Mute */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x13e;

#define HMC7044_R13E_RESVD1 0

typedef union {
    struct {
        HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x13f;

#define HMC7044_R13F_RESVD1 0x00

/* Channel Output 12 */
typedef union {
    struct {
        HMC7044_REG chout12ChannelEn    : 1; /* Channel Enable */
        HMC7044_REG chout12MultiSlipEn  : 1; /* MultiSlip Enable */
        HMC7044_REG chout12StMode       : 2; /* Start-up mode */
        HMC7044_REG resvdBit            : 1;
        HMC7044_REG chout12SlipEn       : 1; /* Slip Enable */
        HMC7044_REG chout12SyncEn       : 1; /* SYNC Enable */
        HMC7044_REG chout12HighPerfMode : 1; /* High Performance mode */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x140;

#define HMC7044_R140_RESVD1 1

typedef union {
    struct {
        HMC7044_REG chout12LsbChannelDiv : 8; /* Channel Divider[7:0] LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x141;

typedef union {
    struct {
        HMC7044_REG chout12MsbChannelDiv : 4; /* Channel Divider[11:8] MSB */
        HMC7044_REG resvdBits            : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x142;

#define HMC7044_R142_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout12FineAnlgDelay : 5; /* Fine analog Delay */
        HMC7044_REG resvdBits            : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x143;

#define HMC7044_R143_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout12CoarseDigtlDelay : 5; /* Coarse Digital Delay */
        HMC7044_REG resvdBits               : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x144;

#define HMC7044_R144_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout12LsbmultiSlipDigtlDelay : 8; /* MultiSlip Dig Delay LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x145;

typedef union {
    struct {
        HMC7044_REG chout12MsbMultiSlipDigtlDelay : 4; /* MultiSlip Dig Delay MSB */
        HMC7044_REG resvdBits                     : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x146;

#define HMC7044_R146_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout12OutputMuxSel : 2; /* Output Mux Selection */
        HMC7044_REG resvdBits           : 6;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x147;

#define HMC7044_R147_RESVD1 0x00

typedef union {
    struct {
        HMC7044_REG chout12DriverImp       : 2; /* Driver Impedance */
        HMC7044_REG resvdBit               : 1;
        HMC7044_REG chout12DriverMode      : 2; /* Driver mode */
        HMC7044_REG chout12DynamicDriverEn : 1; /* Dynamic Driver Enable */
        HMC7044_REG chout12ForceMute       : 2; /* Force Mute */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x148;

#define HMC7044_R148_RESVD1 0

typedef union {
    struct {
        HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x149;

#define HMC7044_R149_RESVD1 0x00

/* Channel Output 13 */
typedef union {
    struct {
        HMC7044_REG chout13ChannelEn     : 1; /* Channel Enable */
        HMC7044_REG chout13MultiSlipEn   : 1; /* MultiSlip Enable */
        HMC7044_REG chout13StMode        : 2; /* Start-up mode */
        HMC7044_REG resvdBit             : 1;
        HMC7044_REG chout13SlipEn        : 1; /* Slip Enable */
        HMC7044_REG chout13SyncEn        : 1; /* SYNC Enable */
        HMC7044_REG chout13HighPerfMode  : 1; /* High Performance mode */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x14a;

#define HMC7044_R14A_RESVD1 1

typedef union {
    struct {
        HMC7044_REG chout13LsbChannelDiv : 8; /* Channel Divider[7:0] LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x14b;

typedef union {
    struct {
        HMC7044_REG chout13MsbChannelDiv : 4; /* Channel Divider[11:8] MSB */
        HMC7044_REG resvdBits            : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x14c;

#define HMC7044_R14C_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout13FineAnlgDelay : 5; /* Fine analog Delay */
        HMC7044_REG resvdBits            : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x14d;

#define HMC7044_R14D_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout13CoarseDigtlDelay : 5;  /* Coarse Digital Delay */
        HMC7044_REG resvdBits               : 3;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x14e;

#define HMC7044_R14E_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout13LsbmultiSlipDigtlDelay : 8; /* MultiSlip Dig Delay LSB */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x14f;

typedef union {
    struct {
        HMC7044_REG chout13MsbMultiSlipDigtlDelay : 4; /* MultiSlip Dig Delay MSB */
        HMC7044_REG resvdBits                     : 4;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x150;

#define HMC7044_R150_RESVD1 0x0

typedef union {
    struct {
        HMC7044_REG chout13OutputMuxSel : 2; /* Output Mux Selection */
        HMC7044_REG resvdBits           : 6;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x151;

#define HMC7044_R151_RESVD1 0x00

typedef union {
    struct {
        HMC7044_REG chout13DriverImp       : 2; /* Driver Impedance */
        HMC7044_REG resvdBit               : 1;
        HMC7044_REG chout13DriverMode      : 2; /* Driver mode */
        HMC7044_REG chout13DynamicDriverEn : 1; /* Dynamic Driver Enable */
        HMC7044_REG chout13ForceMute       : 2; /* Force Mute */
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x152;

#define HMC7044_R152_RESVD1 0

typedef union {
    struct {
        HMC7044_REG resvdBits : 8;
    } fields;
    HMC7044_REG all;
} Hmc7044_reg_x153;

#define HMC7044_R153_RESVD1 0x00

typedef enum {
    HMC7044_P1RI_CLKIN0, HMC7044_P1RI_CLKIN1, HMC7044_P1RI_CLKIN2,
    HMC7044_P1RI_CLKIN3, HMC7044_P1RI_CLKIN_NIN
} HMC7044_PLL1_REF_CLKIN;


typedef enum {
    HMC7044_STMOD_ASYNC = 0, HMC7044_STMOD_DYN = 3
} HMC7044_STARTUP_MODE;

typedef enum {
    HMC7044_CH_DM_CML,  HMC7044_CH_DM_LVPECL,
    HMC7044_CH_DM_LVDS, HMC7044_CH_DM_CMOS
} HMC7044_CH_DRIVER_MODE;

typedef enum {
    HMC7044_DRV_IMP_NONE = 0, HMC7044_DRV_IMP_100 = 1, HMC7044_DRV_IMP_50 = 3
} HMC7044_DRV_IMP_SEL;

typedef enum {
    HMC7044_FORCE_MUTE_NORMAL = 0, HMC7044_FORCE_MUTE_LOGIC0 = 2
} HMC7044_FORCE_MUTE_SEL;

typedef enum {
    HMC7044_OMS_DIVIDER = 0, HMC7044_OMS_DIV_ADLY = 1, HMC7044_OMS_DIV_NEIGHBOR = 2,
    HMC7044_OMS_FUNDAMENTAL = 3
} HMC7044_OUT_MUX_SEL;

#pragma pack(pop)


/*#############################################################################*
*                      D E V I C E    P A R A M E T E R S                      *
*#############################################################################*/


#define HMC7044_PFD1_FREQ_MIN                    0.00015e6
#define HMC7044_PFD1_FREQ_MAX                    50e6
#define HMC7044_PFD2_MIN                         0.00015e6
#define HMC7044_PFD2_MAX                         250e6
#define HMC7044_R2_MIN                           10e6
#define HMC7044_R2_MAX                           500e6
#define HMC7044_R1DIV_MIN                        1
#define HMC7044_R1DIV_MAX                        65535
#define HMC7044_N1DIV_MIN                        1
#define HMC7044_N1DIV_MAX                        65535
#define HMC7044_R2DIV_MIN                        1
#define HMC7044_R2DIV_MAX                        4095
#define HMC7044_N2DIV_MIN                        8
#define HMC7044_N2DIV_MAX                        65535
#define HMC7044_LOW_VCO_MIN                      2150000
#define HMC7044_LOW_VCO_MAX                      2880000
#define HMC7044_HIGH_VCO_MIN                     2650000
#define HMC7044_HIGH_VCO_MAX                     3550000
#define HMC7044_LCM_MIN                          0.00015
#define HMC7044_LCM_MAX                          123000000
#define HMC7044_RECOMM_LCM_MIN                   30000
#define HMC7044_RECOMM_LCM_MAX                   70000
#define HMC7044_VCO_HIGH                         1
#define HMC7044_VCO_LOW	                         2
#define HMC7044_SLIP_REQ_BIT                     1
#define HMC7044_RESEED_BIT                       7
#define HMC7044_SFT_RST_BIT                      0
#define HMC7044_RESET_DIV_FSM_BIT                1
#define HMC7044_PULSE_GEN_BIT                    2
#define HMC7044_CH_OUT_MIN                       0
#define HMC7044_CH_OUT_MAX                       13
#define HMC7044_MIN_PULSE_GEN_CH_DIVIDER         31
#define HMC7044_MIN_CH_DIVIDER                   1
#define HMC7044_MAX_CH_DIVIDER                   4094
#define HMC7044_ADLY_STEP_PS                     25
#define HMC7044_MAX_ADLY_PS                      575
#define HMC7044_WAIT_SYSREF                      6
#define HMC7044_MIN_RUNT_PULSE_FREQ              3e9
#define HMC7044_MAX_SYSREF_FREQ                  4e6
#define HMC7044_LSB(x)                           ((x) & 0xff)
#define HMC7044_MSB(x)                           (((x) & 0xff00) >> 8)
#define HMC7044_FIELD_BIT_MAX                    8
#define HMC7044_OSCOUT_TERM100                   1
#define HMC7044_OSCOUT_TERM50                    3


/*#############################################################################*
*   A P P L I C A T I O N - L E V E L    S E T U P    A N D    C O N T R O L   *
*#############################################################################*/


/* constants and types */
#define HMC7044_APP_LD_INIT_US 100
typedef enum {
    PLL1 = 1,
    PLL2 = 2
} PLLTYPE;

/* this must agree with regData definitions in hmc7044AppInitWrRegs,
   hmc7044AppInitRdRegs */
typedef struct {
    Bool initDone;
    Hmc7044_reg_x01 r01; Hmc7044_reg_x02 r02; Hmc7044_reg_x03 r03;
    Hmc7044_reg_x04 r04; Hmc7044_reg_x05 r05; Hmc7044_reg_x06 r06;
    Hmc7044_reg_x07 r07; Hmc7044_reg_x08 r08; Hmc7044_reg_x09 r09;
    Hmc7044_reg_x0a r0a; Hmc7044_reg_x0b r0b; Hmc7044_reg_x0c r0c;
    Hmc7044_reg_x0d r0d; Hmc7044_reg_x0e r0e; Hmc7044_reg_x14 r14;
    Hmc7044_reg_x15 r15; Hmc7044_reg_x16 r16; Hmc7044_reg_x17 r17;
    Hmc7044_reg_x18 r18; Hmc7044_reg_x19 r19; Hmc7044_reg_x1a r1a;
    Hmc7044_reg_x1b r1b; Hmc7044_reg_x1c r1c; Hmc7044_reg_x1d r1d;
    Hmc7044_reg_x1e r1e; Hmc7044_reg_x1f r1f; Hmc7044_reg_x20 r20;
    Hmc7044_reg_x21 r21; Hmc7044_reg_x22 r22; Hmc7044_reg_x26 r26;
    Hmc7044_reg_x27 r27; Hmc7044_reg_x28 r28; Hmc7044_reg_x29 r29;
    Hmc7044_reg_x2a r2a; Hmc7044_reg_x31 r31; Hmc7044_reg_x32 r32;
    Hmc7044_reg_x33 r33; Hmc7044_reg_x34 r34; Hmc7044_reg_x35 r35;
    Hmc7044_reg_x36 r36; Hmc7044_reg_x37 r37; Hmc7044_reg_x38 r38;
    Hmc7044_reg_x39 r39; Hmc7044_reg_x3a r3a; Hmc7044_reg_x3b r3b;
    Hmc7044_reg_x3c r3c; Hmc7044_reg_x46 r46; Hmc7044_reg_x47 r47;
    Hmc7044_reg_x48 r48; Hmc7044_reg_x49 r49; Hmc7044_reg_x50 r50;
    Hmc7044_reg_x51 r51; Hmc7044_reg_x52 r52; Hmc7044_reg_x53 r53;
    Hmc7044_reg_x54 r54; Hmc7044_reg_x5a r5a; Hmc7044_reg_x5b r5b;
    Hmc7044_reg_x5c r5c; Hmc7044_reg_x5d r5d; Hmc7044_reg_x5e r5e;
    Hmc7044_reg_x64 r64; Hmc7044_reg_x65 r65; Hmc7044_reg_x70 r70;
    Hmc7044_reg_x71 r71; Hmc7044_reg_x96 r96; Hmc7044_reg_x97 r97;
    Hmc7044_reg_x98 r98; Hmc7044_reg_x99 r99; Hmc7044_reg_x9a r9a;
    Hmc7044_reg_x9b r9b; Hmc7044_reg_x9c r9c; Hmc7044_reg_x9d r9d;
    Hmc7044_reg_x9e r9e; Hmc7044_reg_x9f r9f; Hmc7044_reg_xa0 ra0;
    Hmc7044_reg_xa1 ra1; Hmc7044_reg_xa2 ra2; Hmc7044_reg_xa3 ra3;
    Hmc7044_reg_xa4 ra4; Hmc7044_reg_xa5 ra5; Hmc7044_reg_xa6 ra6;
    Hmc7044_reg_xa7 ra7; Hmc7044_reg_xa8 ra8; Hmc7044_reg_xa9 ra9;
    Hmc7044_reg_xab rab; Hmc7044_reg_xac rac; Hmc7044_reg_xad rad;
    Hmc7044_reg_xae rae; Hmc7044_reg_xaf raf; Hmc7044_reg_xb0 rb0;
    Hmc7044_reg_xb1 rb1; Hmc7044_reg_xb2 rb2; Hmc7044_reg_xb3 rb3;
    Hmc7044_reg_xb5 rb5; Hmc7044_reg_xb6 rb6;
    Hmc7044_reg_xb7 rb7; Hmc7044_reg_xb8 rb8;
    Hmc7044_reg_xc8 rc8; Hmc7044_reg_xc9 rc9; Hmc7044_reg_xca rca;
    Hmc7044_reg_xcb rcb; Hmc7044_reg_xcc rcc; Hmc7044_reg_xcd rcd;
    Hmc7044_reg_xce rce; Hmc7044_reg_xcf rcf; Hmc7044_reg_xd0 rd0;
    Hmc7044_reg_xd1 rd1; Hmc7044_reg_xd2 rd2; Hmc7044_reg_xd3 rd3;
    Hmc7044_reg_xd4 rd4; Hmc7044_reg_xd5 rd5; Hmc7044_reg_xd6 rd6;
    Hmc7044_reg_xd7 rd7; Hmc7044_reg_xd8 rd8; Hmc7044_reg_xd9 rd9;
    Hmc7044_reg_xda rda; Hmc7044_reg_xdb rdb; Hmc7044_reg_xdc rdc;
    Hmc7044_reg_xdd rdd; Hmc7044_reg_xde rde; Hmc7044_reg_xdf rdf;
    Hmc7044_reg_xe0 re0; Hmc7044_reg_xe1 re1; Hmc7044_reg_xe2 re2;
    Hmc7044_reg_xe3 re3; Hmc7044_reg_xe4 re4; Hmc7044_reg_xe5 re5;
    Hmc7044_reg_xe6 re6; Hmc7044_reg_xe7 re7; Hmc7044_reg_xe8 re8;
    Hmc7044_reg_xe9 re9; Hmc7044_reg_xea rea; Hmc7044_reg_xeb reb;
    Hmc7044_reg_xec rec; Hmc7044_reg_xed red; Hmc7044_reg_xee ree;
    Hmc7044_reg_xef ref; Hmc7044_reg_xf0 rf0; Hmc7044_reg_xf1 rf1;
    Hmc7044_reg_xf2 rf2; Hmc7044_reg_xf3 rf3; Hmc7044_reg_xf4 rf4;
    Hmc7044_reg_xf5 rf5; Hmc7044_reg_xf6 rf6; Hmc7044_reg_xf7 rf7;
    Hmc7044_reg_xf8 rf8; Hmc7044_reg_xf9 rf9; Hmc7044_reg_xfa rfa;
    Hmc7044_reg_xfb rfb; Hmc7044_reg_xfc rfc; Hmc7044_reg_xfd rfd;
    Hmc7044_reg_xfe rfe; Hmc7044_reg_xff rff; Hmc7044_reg_x100 r100;
    Hmc7044_reg_x101 r101; Hmc7044_reg_x102 r102; Hmc7044_reg_x103 r103;
    Hmc7044_reg_x104 r104; Hmc7044_reg_x105 r105; Hmc7044_reg_x106 r106;
    Hmc7044_reg_x107 r107; Hmc7044_reg_x108 r108; Hmc7044_reg_x109 r109;
    Hmc7044_reg_x10a r10a; Hmc7044_reg_x10b r10b; Hmc7044_reg_x10c r10c;
    Hmc7044_reg_x10d r10d; Hmc7044_reg_x10e r10e; Hmc7044_reg_x10f r10f;
    Hmc7044_reg_x110 r110; Hmc7044_reg_x111 r111; Hmc7044_reg_x112 r112;
    Hmc7044_reg_x113 r113; Hmc7044_reg_x114 r114; Hmc7044_reg_x115 r115;
    Hmc7044_reg_x116 r116; Hmc7044_reg_x117 r117; Hmc7044_reg_x118 r118;
    Hmc7044_reg_x119 r119; Hmc7044_reg_x11a r11a; Hmc7044_reg_x11b r11b;
    Hmc7044_reg_x11c r11c; Hmc7044_reg_x11d r11d; Hmc7044_reg_x11e r11e;
    Hmc7044_reg_x11f r11f; Hmc7044_reg_x120 r120; Hmc7044_reg_x121 r121;
    Hmc7044_reg_x122 r122; Hmc7044_reg_x123 r123; Hmc7044_reg_x124 r124;
    Hmc7044_reg_x125 r125; Hmc7044_reg_x126 r126; Hmc7044_reg_x127 r127;
    Hmc7044_reg_x128 r128; Hmc7044_reg_x129 r129; Hmc7044_reg_x12a r12a;
    Hmc7044_reg_x12b r12b; Hmc7044_reg_x12c r12c; Hmc7044_reg_x12d r12d;
    Hmc7044_reg_x12e r12e; Hmc7044_reg_x12f r12f; Hmc7044_reg_x130 r130;
    Hmc7044_reg_x131 r131; Hmc7044_reg_x132 r132; Hmc7044_reg_x133 r133;
    Hmc7044_reg_x134 r134; Hmc7044_reg_x135 r135; Hmc7044_reg_x136 r136;
    Hmc7044_reg_x137 r137; Hmc7044_reg_x138 r138; Hmc7044_reg_x139 r139;
    Hmc7044_reg_x13a r13a; Hmc7044_reg_x13b r13b; Hmc7044_reg_x13c r13c;
    Hmc7044_reg_x13d r13d; Hmc7044_reg_x13e r13e; Hmc7044_reg_x13f r13f;
    Hmc7044_reg_x140 r140; Hmc7044_reg_x141 r141; Hmc7044_reg_x142 r142;
    Hmc7044_reg_x143 r143; Hmc7044_reg_x144 r144; Hmc7044_reg_x145 r145;
    Hmc7044_reg_x146 r146; Hmc7044_reg_x147 r147; Hmc7044_reg_x148 r148;
    Hmc7044_reg_x149 r149; Hmc7044_reg_x14a r14a; Hmc7044_reg_x14b r14b;
    Hmc7044_reg_x14c r14c; Hmc7044_reg_x14d r14d; Hmc7044_reg_x14e r14e;
    Hmc7044_reg_x14f r14f; Hmc7044_reg_x150 r150; Hmc7044_reg_x151 r151;
    Hmc7044_reg_x152 r152; Hmc7044_reg_x153 r153;
} Hmc7044_reg_image;


typedef struct {
    Bool initDone;   /* relying on static initialization of this to FALSE */
    Hmc7044_app_dev_params params;
    CKDST_FREQ_HZ lcmFreq; /* LCM ferquency */
    HMC7044_5A_SREF_MODE mode; /* Continuous, level or pulsed mode*/
    UINT32 nSecPll1LockTmout, nSecPll2LockTmout; /* PLL1 and PLL2 lock timeout */
} Hmc7044_app_dev_ctl;


LOCAL struct {
    Bool initDone;  /* relying on static init. of this to FALSE */
    UINT32 nsecLockPreChkDly;
    Hmc7044_app_dev_ctl devCtl[CKDST_MAX_NDEV];
} hmc7044AppCtl;


typedef struct {
    UINT64 nsecCmdAt;    /* time of last freq. setting (SYS_TIME_NONE = never) */
    Hmc7044_reg_image regImage;  /* last setup of (most) control registers */
} Hmc7044_app_dev_state;


LOCAL struct {
    Hmc7044_app_dev_state devState[CKDST_MAX_NDEV];  /* per last command */
} hmc7044AppState;


typedef struct {
    HMC7044_PLL1_REF_CLKIN clk1stPri, clk2ndPri, clk3rdPri, clk4thPri;
} Hmc_7044_pll1_ref_clk_pri;


/* forward references */
LOCAL STATUS hmc7044AppInitDevAct(CKDST_DEV dev,
                                  const Hmc7044_app_dev_params *pParams),
             hmc7044AppSetUpDevCtl(CKDST_DEV dev,
                                   const Hmc7044_app_dev_params *pParams);
LOCAL CKDST_FREQ_HZ hmc7044CalcSubMultiple(CKDST_FREQ_HZ f1, CKDST_FREQ_HZ f2);
LOCAL STATUS hmc7044AppChkProductId(CKDST_DEV dev),
             hmc7044AppLoadConfigUpdates(CKDST_DEV dev),
             hmc7044AppInitReservedReg(CKDST_DEV dev);
LOCAL STATUS hmc7044CfgGpis(CKDST_DEV dev,
                            const Hmc7044_app_dev_params *pParams),
             hmc7044CfgSdataMode(CKDST_DEV dev,
                                 const Hmc7044_app_dev_params *pParams),
             hmc7044CfgGpos(CKDST_DEV dev,
                            const Hmc7044_app_dev_params *pParams);
LOCAL STATUS hmc7044AppInitPll2Sup(CKDST_DEV dev,
                                   const Hmc7044_app_dev_params *pParams),	
             hmc7044AppInitPll1Sup(CKDST_DEV dev,
                                   const Hmc7044_app_dev_params *pParams),
             hmc7044AppInitOscOutSup(CKDST_DEV dev,
                                     const Hmc7044_app_dev_params *pParams),
             hmc7044AppInitOscInSup(CKDST_DEV dev,
                                    const Hmc7044_app_dev_params *pParams);
LOCAL STATUS hmc7044AppInitSysrefTimer(CKDST_DEV dev,
                                       const Hmc7044_app_dev_params *pParams),
             hmc7044AppInitPulseGenMode(CKDST_DEV dev,
                                        const Hmc7044_app_dev_params *pParams),
             hmc7044AppInitOutputCh(CKDST_DEV dev,
                                    const Hmc7044_app_dev_params *pParams),
             hmc7044AppInitAlarmMask(CKDST_DEV dev,
                                     const Hmc7044_app_dev_params *pParams);
LOCAL STATUS hmc7044AppInitMisc(CKDST_DEV dev,
                                const Hmc7044_app_dev_params *pParams),
             hmc7044ChkClkOutPhase(CKDST_DEV dev),
             hmc7044DisSync(CKDST_DEV dev, const Hmc7044_app_dev_params *pParams);
LOCAL STATUS hmc7044RegPll1CpCur2Code(unsigned cpCurUa, unsigned *pCode),
             hmc7044RegPll2CpCur2Code(unsigned cpCurUa, unsigned *pCode),
             hmc7044RegSrchTable(UINT32 value, const UINT32 *table,
                                 unsigned tblNval, unsigned *pInx);
LOCAL STATUS hmc7044Wait4Lock(CKDST_DEV dev, UINT32 nsecPreChkDly,
                              UINT32 nsecLockTmout, PLLTYPE pllType),
             hmc7044ToggleBit(CKDST_DEV dev, unsigned regIdx,
		                          HMC7044_REG fieldBit, SYS_TIME delay);
LOCAL STATUS hmc7044AppInitWrRegs(CKDST_DEV dev),
             hmc7044AppInitRdRegs(CKDST_DEV dev),
             hmc7044LliRegRead(CKDST_DEV dev, unsigned regInx, HMC7044_REG *pData),
             hmc7044LliRegWrite(CKDST_DEV dev, unsigned regInx,
                                HMC7044_REG regData),
             hmc7044LliRegIoAct(Bool doRead, CKDST_DEV dev, unsigned regInx,
                                HMC7044_REG *pData);


/*******************************************************************************
* - name: hmc7044AppIfInit
*
* - title: initialize application-level interface
*
* - output: hmc7044AppCtl
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - notes: this routine can be called more than once
*******************************************************************************/
LOCAL STATUS hmc7044AppIfInit(void)
{
    hmc7044AppCtl.nsecLockPreChkDly = HMC7044_APP_LD_INIT_US;

    hmc7044AppCtl.initDone = TRUE;

    return OK;
}




/*******************************************************************************
* - name: hmc7044AppInitDev
*
* - title: initialize a specific CLKDST device
*
* - input: dev      - CLKDST device for which to perform the operation
*          pParams  - device setup parameters
*          warmInit - if set, will skip actual device initialization
*
* - output: hmc7044AppCtl.devCtl[dev] (indirectly), hmc7044AppState.devState[dev]
*
* - returns: OK or ERROR if detected an error
*
* - description: sets up and initializes the CLKDST device per the application's
*                requirements
*
* - notes: This routine can be called more than once (for a device).
*******************************************************************************/
LOCAL STATUS hmc7044AppInitDev(CKDST_DEV dev, const Hmc7044_app_dev_params *pParams,
							   Bool warmInit)
{
    STATUS status = OK;  /* initial assumption */

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppCtl.devCtl)) || !pParams) {
        sysLog("bad argument(s) (dev %u, pParams %d)", dev, pParams != NULL);
        return ERROR;
    }

    if (!hmc7044IfCtl.initDone || !hmc7044AppCtl.initDone) {
        sysLog("interface not initialized yet (dev %u, init. done %d,%d)", dev,
               hmc7044IfCtl.initDone, hmc7044AppCtl.initDone);
        return ERROR;
    }

    /* in particular this sets .nsecCmdAt, .freq to zeros */
    memset(&hmc7044AppState.devState[dev], 0,
            sizeof(hmc7044AppState.devState[dev]));

    if (hmc7044AppSetUpDevCtl(dev, pParams) != OK)
        status = ERROR;

    if (!warmInit) {
        if (hmc7044AppInitDevAct(dev, pParams) != OK)
            status = ERROR;
    } else {
          if (hmc7044AppInitRdRegs(dev) != OK)
              status = ERROR;
    }

    return status;
}




/*******************************************************************************
* - name: hmc7044AppInitDevAct
*
* - title: actually initialize a CLKDST device
*
* - input: dev     - CLKDST device for which to perform the operation
*          pParams - pointer to device setup parameters
*
* - output: hmc7044AppState.devState[dev]
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - notes: 1) This routine can be called more than once (for a device).
*          2) It is assumed that hmc7044ppCtl.devCtl[dev] has already been set up.
*          3) Steps for initialization sequence are as given below.
* Step 1  : Soft-reset the device as the first step of the initialization sequence.
* Step 2  : Read product id registers during initializtion and compare the
*    	    result to the expected. In case of mismatch, fail the initialization.
* Step 3  : Load the configuration updates to specific registers.
* Step 4  : Explicitly write the default values to all reserved registers and fields.
* Step 5  : Set up GPIOs and SDATA mode
* Step 6  : Program PLL2. Select the VCO range (high or low). Program the dividers.
* Step 7  : Program PLL1. Set the lock detect timer threshold based on the PLL1 BW
*           of the user system. Set the LCM, R1, and N1 divider setpoints. Enable the
*           reference and VCXO input buffer terminations.
* Step 8  : Initialize OSCIN and OSCOUT.
* Step 9  : Program the SYSREF timer. Set the divide ratio (a submultiple of the lower
*           output channel frequency). Set the pulse generator mode configuration.
* Step 10 : Program the output channels. Set the output buffer modes. Set the divide
*	    ratio, channel startup mode, coarse/analog delays, and performance modes.
* Step 11 : Wait until the VCO peak detector loop has stabilized.
* Step 12 : Issue a software restart to reset the system and initiate calibration.
*           Toggle the restart dividers/FSMs bit to 1 and then back to 0.
* Step 13 : Wait for PLL2 to be locked.
* Step 14 : Confirm that PLL2 is locked by checking the PLL2 lock detect bit.
* Step 15 : Send a sync request via the SPI (set the reseed request bit)
* Step 16 : Wait 6 SYSREF periods (6 × SYSREF Timer[11:0]) to allow the outputs to
*           phase appropriately
* Step 17 : Confirm that the outputs have all reached their phases by checking that
*           the clock outputs phases status bit = 1.
* Step 18 : Wait for PLL1 to lock.
* Step 19 : After completing the initialization sequence, software shall disable
*           SYNC on all channels.
*******************************************************************************/
LOCAL STATUS hmc7044AppInitDevAct(CKDST_DEV dev,
                                  const Hmc7044_app_dev_params *pParams)
{
    Hmc7044_app_dev_state *pState;
    const Hmc7044_app_dev_ctl *pCtl;

    /* initialize */
    if (!inEnumRange(dev, NELEMENTS(hmc7044AppCtl.devCtl)) || !pParams) {
        sysLog("bad argument(s) (dev %u, pParams %d)", dev, pParams != NULL);
        return ERROR;
    }

    pState = hmc7044AppState.devState + dev;
    pCtl = hmc7044AppCtl.devCtl + dev;

    if (!hmc7044AppCtl.initDone) {
        sysLog("control data initialization not done yet (dev %u)", dev);
        return ERROR;
    }

     /* Soft Reset the device */
    if (hmc7044ToggleBit(dev, 0x00, HMC7044_SFT_RST_BIT, 200) != OK)
        return ERROR;

    memset(pState, 0, sizeof(*pState));

    /* verify the expected device id(s) */
    if (hmc7044AppChkProductId(dev) != OK)
        return ERROR;

    /* Load the configuration updates to specific registers as in Table 74
     * of datasheet.*/
    if (hmc7044AppLoadConfigUpdates(dev) != OK)
        return ERROR;

    if (hmc7044AppInitReservedReg(dev) != OK)
        return ERROR;

    /* Setup GPIOs */
    if (hmc7044CfgGpis(dev, pParams) != OK)
        return ERROR;

    if (hmc7044CfgSdataMode(dev, pParams) != OK)
        return ERROR;

    if (hmc7044CfgGpos(dev, pParams) != OK)
        return ERROR;

   /* Program PLL2 */
   if (hmc7044AppInitPll2Sup(dev, pParams) != OK)
       return ERROR;

   /* Program PLL1 */
   if (hmc7044AppInitPll1Sup(dev, pParams) != OK)
       return ERROR;

   if (hmc7044AppInitOscInSup(dev, pParams) != OK)
       return ERROR;

   if (hmc7044AppInitOscOutSup(dev, pParams) != OK)
       return ERROR;

   /* Program Sysref timer */
   if (hmc7044AppInitSysrefTimer(dev, pParams) != OK)
       return ERROR;

   if (hmc7044AppInitPulseGenMode(dev, pParams) != OK)
       return ERROR;

   if (hmc7044AppInitOutputCh(dev, pParams) != OK)
       return ERROR;

   /* Configure Alarm Mask registers */
   if (hmc7044AppInitAlarmMask(dev, pParams) != OK)
       return ERROR;

   if (hmc7044AppInitMisc(dev, pParams) != OK)
       return ERROR;

   /* write initialization image to device registers */
   if (hmc7044AppInitWrRegs(dev) != OK)
       return ERROR;

   sysDelayUsec(10e3); /* 10 msec delay */

   /* Issue software restart to reset system */
   if (hmc7044ToggleBit(dev, 0x00, HMC7044_SFT_RST_BIT, 200) != OK)
       return ERROR;

   /*Toggle the restart dividers/FSMs bit to 1 and then back to 0.*/
   if (hmc7044ToggleBit(dev, 0x01, HMC7044_RESET_DIV_FSM_BIT, 0) != OK)
       return ERROR;

   /* Wait for PLL2 to be locked and check PLL2 lock detect bit */
   if (hmc7044Wait4Lock(dev, hmc7044AppCtl.nsecLockPreChkDly,
                        pCtl->nSecPll2LockTmout, PLL2) != OK)
       return ERROR;

   /* Send a sync request via the SPI (set the reseed request bit) */
   if (hmc7044ToggleBit(dev, 0x01, HMC7044_RESEED_BIT, 0) != OK)
       return ERROR;

   /* Wait for 6xSYSREF period */
   sysDelayUsec(((double)1 / pParams->sysref.freq) * 1e6 * HMC7044_WAIT_SYSREF);

   /* Check if clock output phase is set*/
   if (hmc7044ChkClkOutPhase(dev) != OK)
       return ERROR;

   /* Wait for PLL1 to be locked */
   if (pParams->pll1Sup.used) {
       if (hmc7044Wait4Lock(dev, hmc7044AppCtl.nsecLockPreChkDly,
                            pCtl->nSecPll1LockTmout, PLL1) != OK)
           return ERROR;
   }
   /* After completing the initialization sequence, software shall
    * disable SYNC on all channels. */
   if (hmc7044DisSync(dev, pParams) != OK)
       return ERROR;

   pState->regImage.initDone = TRUE;

   return OK;
}




/*******************************************************************************
* - name: hmc7044AppSetUpDevCtl
*
* - title: set up control parameters for a PLL device
*
* - input: dev     - CLKDST device for which to perform the operation
*          pParams - pointer to device setup parameters
*
* - output: hmc7044AppCtl.devCtl[dev]
*
* - returns: OK or ERROR if detected an error
*
* - description: sets up CLKDST device control parameters per the application's
*                requirements
*
* - notes: not attempting to interlock the sequence here - if such interlocking is
*          necessary, it must be provided by the caller
*
* Software shall verify that fLCM is within the limits defined in the datasheet.
*
* If fLCM is within the above range but outside the range 30-70 MHz, software shall
* output a warning to the console. This shall not cause initialization to fail
* (and the routine will return OK status).
*******************************************************************************/
LOCAL STATUS hmc7044AppSetUpDevCtl(CKDST_DEV dev,
                                   const Hmc7044_app_dev_params *pParams)
{

    Hmc7044_app_dev_ctl *pCtl;
    CKDST_FREQ_HZ lcmFreq;
    unsigned i;

    /* initialize */
    if (!inEnumRange(dev, NELEMENTS(hmc7044AppCtl.devCtl)) || !pParams) {
        sysLog("bad argument(s) (dev %u, pParams %d)", dev, pParams != NULL);
        return ERROR;
    }

    pCtl = hmc7044AppCtl.devCtl + dev;

    /* set up device control parameters */
    pCtl->params = *pParams;

    /* Calculate the lcm frequency */
    lcmFreq = pParams->oscInFreq;
    if (pParams->pll1Sup.used) {
      for (i = 0; i < HMC7044_P1RI_NIN; i++) {
          if (pParams->pll1Sup.refIn.inSup[i].sup.used)
              lcmFreq = hmc7044CalcSubMultiple(pParams->pll1Sup.refIn.inSup[i].freq,
                                               lcmFreq);
      }
    }

    if (lcmFreq < HMC7044_LCM_MIN || lcmFreq > HMC7044_LCM_MAX) {
        sysLog("LCM frequency %lu not within allowed range for dev %u",
        		lcmFreq, dev);
        return ERROR;
    }

    if (lcmFreq < HMC7044_LCM_MIN || lcmFreq > HMC7044_RECOMM_LCM_MAX) {
        sysLog("LCM frequency %lu not within recommended range 30 to 70 MHz"
               " for dev %u", lcmFreq, dev);
    }

   /* store in device control */
    pCtl->lcmFreq = lcmFreq;

    pCtl->initDone = TRUE;

    return OK;
}




/*******************************************************************************
* - name: hmc7044CalcSubMultiple
*
* - title: Find the GCD of clock input frequencies
*
* - input: f1 - Clock input frequency for GCD calculation
*          f2 - Next Clock input frequency for GCD calculation
*
* - returns: GCD of frequencies
*
* - description: as above
*******************************************************************************/
LOCAL CKDST_FREQ_HZ hmc7044CalcSubMultiple(CKDST_FREQ_HZ f1, CKDST_FREQ_HZ f2)
{
    if ((f1 % f2) == 0)
        return f2;
    return hmc7044CalcSubMultiple(f2, (f1 % f2));
}




/*******************************************************************************
* - name: hmc7044AppLoadConfigUpdates
*
* - title: Load the configuration updates to specific registers (Table 74).
*
* - input: dev - CLKDST device for which to perform the operation
*
* - returns: OK or ERROR if detected an error
*
* - description: Software shall set the reserved registers in Table 25 for which
*   there is a specification of non-default values that need to be set for
*    optimal performance to these values.
*******************************************************************************/
LOCAL STATUS hmc7044AppLoadConfigUpdates(CKDST_DEV dev)
{
	Hmc7044_reg_image *pImg;

    /* initialize */
    if (!inEnumRange(dev, NELEMENTS(hmc7044AppState.devState))) {
        sysLog("bad dev (%u)", dev);
        return ERROR;
    }

    if (!hmc7044IfCtl.initDone) {
        sysLog("interface initialization not done yet (dev %u)", dev);
        return ERROR;
    }

    pImg = &hmc7044AppState.devState[dev].regImage;
    pImg->r9f.all = 0x4d;
    pImg->ra0.all = 0xdf;
    pImg->ra5.all = 0x06;
    pImg->ra8.all = 0x06;
    pImg->rb0.all = 0x04;

    return OK;
}




/*******************************************************************************
* - name: hmc7044AppChkProductId
*
* - title: read and verify the product id(s)
*
* - input: dev - PLL device for which to perform the operation
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7044AppChkProductId(CKDST_DEV dev)
{
    Hmc7044_reg_x78 r78;
    Hmc7044_reg_x79 r79;
    Hmc7044_reg_x7a r7a;

    /* initialize */
    if (!inEnumRange(dev, NELEMENTS(hmc7044AppCtl.devCtl))) {
        sysLog("bad dev (%u)", dev);
        return ERROR;
    }

    if (!hmc7044IfCtl.initDone) {
        sysLog("interface initialization not done yet (dev %u)", dev);
        return ERROR;
    }
	 /* read device id and compare to the expected */
	if (hmc7044LliRegRead(dev, 0x78, &r78.all) != OK ||
	    hmc7044LliRegRead(dev, 0x79, &r79.all) != OK ||
	    hmc7044LliRegRead(dev, 0x7a, &r7a.all) != OK)
	    return ERROR;

	if (r78.fields.lsbProductIDValue  != (HMC7044_PRODUCT_ID & 0xff) ||
	    r79.fields.midProductIDValue  != ((HMC7044_PRODUCT_ID >> 8) & 0xff) ||
	    r7a.fields.msbProductIDValue  != HMC7044_PRODUCT_ID >> 16) {
	    sysLog("unexpected id values (dev %u, prodId 0x%02x, 0x%02x, 0x%02x)",
	            dev, r78.fields.lsbProductIDValue, r79.fields.midProductIDValue,
				   r7a.fields.msbProductIDValue);

	     return ERROR;
	}

	return OK;
}




/*******************************************************************************
* - name: hmc7044AppInitReservedReg
*
* - title: initialize reserved registers
*
* - input: dev - CLKDST device for which to perform the operation
*
* - output: hmc7044AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - notes: This routine sets up register fields that are reserved.
*  Software shall explicitly write the default values to all reserved registers.
*  When writing to register comprising reserved fields, software shall write the
   default values to these fields.
*******************************************************************************/
LOCAL STATUS hmc7044AppInitReservedReg(CKDST_DEV dev)
{

    Hmc7044_reg_image *pImg;

    /* initialize */
    if (!inEnumRange(dev, NELEMENTS(hmc7044AppState.devState))) {
        sysLog("bad argument (dev %u)", dev);
        return ERROR;
    }

    if (!hmc7044IfCtl.initDone) {
        sysLog("interface initialization not done yet (dev %u)", dev);
        return ERROR;
    }

    pImg = &hmc7044AppState.devState[dev].regImage;

    pImg->r07.all = 0x00;
    pImg->r08.all = 0x00;
    pImg->r31.all = 0x01;
    pImg->r3c.all = 0x00;
    pImg->r5e.all = 0x00;
    pImg->r96.all = 0x00;
    pImg->r97.all = 0x00;
    pImg->r98.all = 0x00;
    pImg->r99.all = 0x00;
    pImg->r9a.all = 0x00;
    pImg->r9b.all = 0xaa;
    pImg->r9c.all = 0xaa;
    pImg->r9d.all = 0xaa;
    pImg->r9e.all = 0xaa;
    pImg->ra1.all = 0x97;
    pImg->ra2.all = 0x03;
    pImg->ra3.all = 0x00;
    pImg->ra4.all = 0x00;
    pImg->ra6.all = 0x1c;
    pImg->ra7.all = 0x00;
    pImg->ra9.all = 0x00;
    pImg->rab.all = 0x00;
    pImg->rac.all = 0x20;
    pImg->rad.all = 0x00;
    pImg->rae.all = 0x08;
    pImg->raf.all = 0x50;
    pImg->rb1.all = 0x0d;
    pImg->rb2.all = 0x00;
    pImg->rb3.all = 0x00;
    pImg->rb5.all = 0x00;
    pImg->rb6.all = 0x00;
    pImg->rb7.all = 0x00;
    pImg->rb8.all = 0x00;
    pImg->rd1.all = 0x00;
    pImg->rdb.all = 0x00;
    pImg->re5.all = 0x00;
    pImg->ref.all = 0x00;
    pImg->rf9.all = 0x00;
    pImg->r103.all = 0x00;
    pImg->r10d.all = 0x00;
    pImg->r117.all = 0x00;
    pImg->r121.all = 0x00;
    pImg->r12b.all = 0x00;
    pImg->r135.all = 0x00;
    pImg->r13f.all = 0x00;
    pImg->r149.all = 0x00;
    pImg->r153.all = 0x00;

   /* Set the reserved fields in each R/W register */
   pImg->r01.fields.resvdBit = HMC7044_R01_RESVD1;
   pImg->r02.fields.resvdBit = HMC7044_R02_RESVD1;
   pImg->r02.fields.resvdBits = HMC7044_R02_RESVD2;
   pImg->r03.fields.resvdBits = HMC7044_R03_RESVD1;
   pImg->r04.fields.resvdBit = HMC7044_R04_RESVD1;
   pImg->r06.fields.resvdBits = HMC7044_R06_RESVD1;
   pImg->r07.fields.resvdBits = HMC7044_R07_RESVD1;
   pImg->r08.fields.resvdBits = HMC7044_R08_RESVD1;
   pImg->r09.fields.resvdBits = HMC7044_R09_RESVD1;
   pImg->r0a.fields.resvdBits = HMC7044_R0A_RESVD1;
   pImg->r0b.fields.resvdBits = HMC7044_R0B_RESVD1;
   pImg->r0c.fields.resvdBits = HMC7044_R0C_RESVD1;
   pImg->r0d.fields.resvdBits = HMC7044_R0D_RESVD1;
   pImg->r0e.fields.resvdBits = HMC7044_R0E_RESVD1;
   pImg->r15.fields.resvdBits = HMC7044_R15_RESVD1;
   pImg->r16.fields.resvdBits = HMC7044_R16_RESVD1;
   pImg->r17.fields.resvdBit = HMC7044_R17_RESVD1;
   pImg->r18.fields.resvdBits = HMC7044_R18_RESVD1;
   pImg->r19.fields.resvdBits = HMC7044_R19_RESVD1;
   pImg->r1a.fields.resvdBits = HMC7044_R1A_RESVD1;
   pImg->r1b.fields.resvdBits = HMC7044_R1B_RESVD1;
   pImg->r28.fields.resvdBits = HMC7044_R28_RESVD1;
   pImg->r29.fields.resvdBits = HMC7044_R29_RESVD1;
   pImg->r31.fields.resvdBits = HMC7044_R31_RESVD1;
   pImg->r32.fields.resvdBits = HMC7044_R32_RESVD1;
   pImg->r34.fields.resvdBits = HMC7044_R34_RESVD1;
   pImg->r37.fields.resvdBits = HMC7044_R37_RESVD1;
   pImg->r38.fields.resvdBits = HMC7044_R38_RESVD1;
   pImg->r39.fields.resvdBits = HMC7044_R39_RESVD1;
   pImg->r3a.fields.resvdBit  = HMC7044_R3A_RESVD1;
   pImg->r3a.fields.resvdBits = HMC7044_R3A_RESVD2;
   pImg->r3b.fields.resvdBit  = HMC7044_R3B_RESVD1;
   pImg->r3b.fields.resvdBits = HMC7044_R3B_RESVD2;
   pImg->r3c.fields.resvdBits = HMC7044_R3C_RESVD1;
   pImg->r46.fields.resvdBits = HMC7044_R46_RESVD1;
   pImg->r47.fields.resvdBits = HMC7044_R47_RESVD1;
   pImg->r48.fields.resvdBits = HMC7044_R48_RESVD1;
   pImg->r49.fields.resvdBits = HMC7044_R49_RESVD1;
   pImg->r54.fields.resvdBits = HMC7044_R54_RESVD1;
   pImg->r5a.fields.resvdBits = HMC7044_R5A_RESVD1;
   pImg->r5b.fields.resvdBits = HMC7044_R5B_RESVD1;
   pImg->r5d.fields.resvdBits = HMC7044_R5D_RESVD1;
   pImg->r5e.fields.resvdBits = HMC7044_R5E_RESVD1;
   pImg->r64.fields.resvdBits = HMC7044_R64_RESVD1;
   pImg->r65.fields.resvdBits = HMC7044_R65_RESVD1;
   pImg->r71.fields.resvdBits = HMC7044_R71_RESVD1;
   pImg->r96.fields.resvdBits = HMC7044_R96_RESVD1;
   pImg->r97.fields.resvdBits = HMC7044_R97_RESVD1;
   pImg->r98.fields.resvdBits = HMC7044_R98_RESVD1;
   pImg->r99.fields.resvdBits = HMC7044_R99_RESVD1;
   pImg->r9a.fields.resvdBits = HMC7044_R9A_RESVD1;
   pImg->r9b.fields.resvdBits = HMC7044_R9B_RESVD1;
   pImg->r9c.fields.resvdBits = HMC7044_R9C_RESVD1;
   pImg->r9d.fields.resvdBits = HMC7044_R9D_RESVD1;
   pImg->r9e.fields.resvdBits = HMC7044_R9E_RESVD1;
   pImg->ra1.fields.resvdBits = HMC7044_RA1_RESVD1;
   pImg->ra2.fields.resvdBits = HMC7044_RA2_RESVD1;
   pImg->ra3.fields.resvdBits = HMC7044_RA3_RESVD1;
   pImg->ra4.fields.resvdBits = HMC7044_RA4_RESVD1;
   pImg->ra6.fields.resvdBits = HMC7044_RA6_RESVD1;
   pImg->ra7.fields.resvdBits = HMC7044_RA7_RESVD1;
   pImg->ra9.fields.resvdBits = HMC7044_RA9_RESVD1;
   pImg->rab.fields.resvdBits = HMC7044_RAB_RESVD1;
   pImg->rac.fields.resvdBits = HMC7044_RAC_RESVD1;
   pImg->rad.fields.resvdBits = HMC7044_RAD_RESVD1;
   pImg->rae.fields.resvdBits = HMC7044_RAE_RESVD1;
   pImg->raf.fields.resvdBits = HMC7044_RAF_RESVD1;
   pImg->rb1.fields.resvdBits = HMC7044_RB1_RESVD1;
   pImg->rb2.fields.resvdBits = HMC7044_RB2_RESVD1;
   pImg->rb3.fields.resvdBits = HMC7044_RB3_RESVD1;
   pImg->rb5.fields.resvdBits = HMC7044_RB5_RESVD1;
   pImg->rb6.fields.resvdBits = HMC7044_RB6_RESVD1;
   pImg->rb7.fields.resvdBits = HMC7044_RB7_RESVD1;
   pImg->rb8.fields.resvdBits = HMC7044_RB8_RESVD1;
   pImg->rc8.fields.resvdBit  = HMC7044_RC8_RESVD1;
   pImg->rca.fields.resvdBits = HMC7044_RCA_RESVD1;
   pImg->rcb.fields.resvdBits = HMC7044_RCB_RESVD1;
   pImg->rcc.fields.resvdBits = HMC7044_RCC_RESVD1;
   pImg->rce.fields.resvdBits = HMC7044_RCE_RESVD1;
   pImg->rcf.fields.resvdBits = HMC7044_RCF_RESVD1;
   pImg->rd0.fields.resvdBit  = HMC7044_RD0_RESVD1;
   pImg->rd1.fields.resvdBits = HMC7044_RD1_RESVD1;
   pImg->rd2.fields.resvdBit  = HMC7044_RD2_RESVD1;
   pImg->rd4.fields.resvdBits = HMC7044_RD4_RESVD1;
   pImg->rd5.fields.resvdBits = HMC7044_RD5_RESVD1;
   pImg->rd6.fields.resvdBits = HMC7044_RD6_RESVD1;
   pImg->rd8.fields.resvdBits = HMC7044_RD8_RESVD1;
   pImg->rd9.fields.resvdBits = HMC7044_RD9_RESVD1;
   pImg->rda.fields.resvdBit  = HMC7044_RDA_RESVD1;
   pImg->rdb.fields.resvdBits = HMC7044_RDB_RESVD1;
   pImg->rdc.fields.resvdBit = HMC7044_RDC_RESVD1;
   pImg->rde.fields.resvdBits = HMC7044_RDE_RESVD1;
   pImg->rdf.fields.resvdBits = HMC7044_RDF_RESVD1;
   pImg->re0.fields.resvdBits = HMC7044_RE0_RESVD1;
   pImg->re2.fields.resvdBits = HMC7044_RE2_RESVD1;
   pImg->re3.fields.resvdBits = HMC7044_RE3_RESVD1;
   pImg->re4.fields.resvdBit  = HMC7044_RE4_RESVD1;
   pImg->re5.fields.resvdBits = HMC7044_RE5_RESVD1;
   pImg->re6.fields.resvdBit  = HMC7044_RE6_RESVD1;
   pImg->re8.fields.resvdBits = HMC7044_RE8_RESVD1;
   pImg->re8.fields.resvdBits = HMC7044_RE8_RESVD1;
   pImg->re9.fields.resvdBits = HMC7044_RE9_RESVD1;
   pImg->rea.fields.resvdBits = HMC7044_REA_RESVD1;
   pImg->rec.fields.resvdBits = HMC7044_REC_RESVD1;
   pImg->red.fields.resvdBits = HMC7044_RED_RESVD1;
   pImg->ree.fields.resvdBit  = HMC7044_REE_RESVD1;
   pImg->ref.fields.resvdBits = HMC7044_REF_RESVD1;
   pImg->rf0.fields.resvdBit  = HMC7044_RF0_RESVD1;
   pImg->rf2.fields.resvdBits = HMC7044_RF2_RESVD1;
   pImg->rf3.fields.resvdBits = HMC7044_RF3_RESVD1;
   pImg->rf4.fields.resvdBits = HMC7044_RF4_RESVD1;
   pImg->rf6.fields.resvdBits = HMC7044_RF6_RESVD1;
   pImg->rf7.fields.resvdBits = HMC7044_RF7_RESVD1;
   pImg->rf8.fields.resvdBit  = HMC7044_RF8_RESVD1;
   pImg->rf9.fields.resvdBits = HMC7044_RF9_RESVD1;
   pImg->rfa.fields.resvdBit  = HMC7044_RFA_RESVD1;
   pImg->rfc.fields.resvdBits = HMC7044_RFC_RESVD1;
   pImg->rfd.fields.resvdBits = HMC7044_RFD_RESVD1;
   pImg->rfe.fields.resvdBits = HMC7044_RFE_RESVD1;
   pImg->r100.fields.resvdBits = HMC7044_R100_RESVD1;
   pImg->r101.fields.resvdBits = HMC7044_R101_RESVD1;
   pImg->r102.fields.resvdBit  = HMC7044_R102_RESVD1;
   pImg->r103.fields.resvdBits = HMC7044_R103_RESVD1;
   pImg->r104.fields.resvdBit  = HMC7044_R104_RESVD1;
   pImg->r106.fields.resvdBits = HMC7044_R106_RESVD1;
   pImg->r107.fields.resvdBits = HMC7044_R107_RESVD1;
   pImg->r108.fields.resvdBits = HMC7044_R108_RESVD1;
   pImg->r10a.fields.resvdBits = HMC7044_R10A_RESVD1;
   pImg->r10b.fields.resvdBits = HMC7044_R10B_RESVD1;
   pImg->r10c.fields.resvdBit  = HMC7044_R10C_RESVD1;
   pImg->r10d.fields.resvdBits = HMC7044_R10D_RESVD1;
   pImg->r10e.fields.resvdBit  = HMC7044_R10E_RESVD1;
   pImg->r110.fields.resvdBits = HMC7044_R100_RESVD1;
   pImg->r111.fields.resvdBits = HMC7044_R111_RESVD1;
   pImg->r112.fields.resvdBits = HMC7044_R112_RESVD1;
   pImg->r114.fields.resvdBits = HMC7044_R114_RESVD1;
   pImg->r115.fields.resvdBits = HMC7044_R115_RESVD1;
   pImg->r116.fields.resvdBit  = HMC7044_R116_RESVD1;
   pImg->r117.fields.resvdBits = HMC7044_R117_RESVD1;
   pImg->r118.fields.resvdBit  = HMC7044_R118_RESVD1;
   pImg->r11a.fields.resvdBits = HMC7044_R11A_RESVD1;
   pImg->r11b.fields.resvdBits = HMC7044_R11B_RESVD1;
   pImg->r11c.fields.resvdBits = HMC7044_R11C_RESVD1;
   pImg->r11e.fields.resvdBits = HMC7044_R11E_RESVD1;
   pImg->r11f.fields.resvdBits = HMC7044_R11F_RESVD1;
   pImg->r120.fields.resvdBit  = HMC7044_R120_RESVD1;
   pImg->r121.fields.resvdBits = HMC7044_R121_RESVD1;
   pImg->r122.fields.resvdBit  = HMC7044_R122_RESVD1;
   pImg->r124.fields.resvdBits = HMC7044_R124_RESVD1;
   pImg->r125.fields.resvdBits = HMC7044_R125_RESVD1;
   pImg->r126.fields.resvdBits = HMC7044_R126_RESVD1;
   pImg->r128.fields.resvdBits = HMC7044_R128_RESVD1;
   pImg->r129.fields.resvdBits = HMC7044_R129_RESVD1;
   pImg->r12a.fields.resvdBit  = HMC7044_R12A_RESVD1;
   pImg->r12b.fields.resvdBits = HMC7044_R12B_RESVD1;
   pImg->r12c.fields.resvdBit  = HMC7044_R12C_RESVD1;
   pImg->r12e.fields.resvdBits = HMC7044_R12E_RESVD1;
   pImg->r12f.fields.resvdBits = HMC7044_R12F_RESVD1;
   pImg->r130.fields.resvdBits = HMC7044_R130_RESVD1;
   pImg->r132.fields.resvdBits = HMC7044_R132_RESVD1;
   pImg->r133.fields.resvdBits = HMC7044_R133_RESVD1;
   pImg->r134.fields.resvdBit  = HMC7044_R134_RESVD1;
   pImg->r135.fields.resvdBits = HMC7044_R135_RESVD1;
   pImg->r136.fields.resvdBit  = HMC7044_R136_RESVD1;
   pImg->r138.fields.resvdBits = HMC7044_R138_RESVD1;
   pImg->r139.fields.resvdBits = HMC7044_R139_RESVD1;
   pImg->r13a.fields.resvdBits = HMC7044_R13A_RESVD1;
   pImg->r13c.fields.resvdBits = HMC7044_R13C_RESVD1;
   pImg->r13d.fields.resvdBits = HMC7044_R13D_RESVD1;
   pImg->r13e.fields.resvdBit  = HMC7044_R13E_RESVD1;
   pImg->r13f.fields.resvdBits = HMC7044_R13F_RESVD1;
   pImg->r140.fields.resvdBit  = HMC7044_R140_RESVD1;
   pImg->r142.fields.resvdBits = HMC7044_R142_RESVD1;
   pImg->r143.fields.resvdBits = HMC7044_R143_RESVD1;
   pImg->r144.fields.resvdBits = HMC7044_R144_RESVD1;
   pImg->r146.fields.resvdBits = HMC7044_R146_RESVD1;
   pImg->r147.fields.resvdBits = HMC7044_R147_RESVD1;
   pImg->r148.fields.resvdBit  = HMC7044_R148_RESVD1;
   pImg->r149.fields.resvdBits = HMC7044_R149_RESVD1;
   pImg->r14a.fields.resvdBit  = HMC7044_R14A_RESVD1;
   pImg->r14c.fields.resvdBits = HMC7044_R14C_RESVD1;
   pImg->r14d.fields.resvdBits = HMC7044_R14D_RESVD1;
   pImg->r14e.fields.resvdBits = HMC7044_R14E_RESVD1;
   pImg->r150.fields.resvdBits = HMC7044_R150_RESVD1;
   pImg->r151.fields.resvdBits = HMC7044_R151_RESVD1;
   pImg->r152.fields.resvdBit  = HMC7044_R152_RESVD1;
   pImg->r153.fields.resvdBits = HMC7044_R153_RESVD1;

   return OK;
}




/*******************************************************************************
* - name: hmc7044CfgGpis
*
* - title: Configure GPIO setup for a particular device.
*
* - input: dev     - CLKDST device on which operation is performed.
*          pParams - pointer to device setup parameters
*
* - output: hmc70443AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - notes:
*******************************************************************************/
LOCAL STATUS hmc7044CfgGpis(CKDST_DEV dev,
                            const Hmc7044_app_dev_params *pParams)
{
    Hmc7044_reg_image *pImg;
    unsigned i;

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppState.devState)) || !pParams) {
        sysLog("bad argument(s) (dev %u, pParams %d)", dev, pParams != NULL);
        return ERROR;
    }

    pImg = &hmc7044AppState.devState[dev].regImage;

    pImg->r46.fields.gpi1En = 0;
    pImg->r47.fields.gpi2En = 0;
    pImg->r48.fields.gpi3En = 0;
    pImg->r49.fields.gpi4En = 0;

    for (i = 0; i < HMC7044_NGPIO; i++) {
        switch (pParams->gpiSup[i]) {
        case HMC7044_GPIS_NONE:
            break;
        case HMC7044_GPIS_PLL1_HO:
            if (i == 0) {
                pImg->r46.fields.gpi1Sel = HMC7044_REG_GPIS_PLL1_HO;
            } else if (i == 1) {
                pImg->r47.fields.gpi2Sel = HMC7044_REG_GPIS_PLL1_HO;
            } else if (i == 2) {
                pImg->r48.fields.gpi3Sel = HMC7044_REG_GPIS_PLL1_HO;
            } else if (i == 3) {
                pImg->r49.fields.gpi4Sel = HMC7044_REG_GPIS_PLL1_HO;
            }
            break;
        case HMC7044_GPIS_PLL1_REF_B1:
            if (i == 0) {
                pImg->r46.fields.gpi1Sel = HMC7044_REG_GPIS_PLL1_REF_B1;
            } else if (i == 1) {
                pImg->r47.fields.gpi2Sel = HMC7044_REG_GPIS_PLL1_REF_B1;
            } else if (i == 2) {
                pImg->r48.fields.gpi3Sel = HMC7044_REG_GPIS_PLL1_REF_B1;
            } else if (i == 3) {
                pImg->r49.fields.gpi4Sel = HMC7044_REG_GPIS_PLL1_REF_B1;
            }
            break;
        case HMC7044_GPIS_PLL1_REF_B0:
            if (i == 0) {
                pImg->r46.fields.gpi1Sel = HMC7044_REG_GPIS_PLL1_REF_B0;
            } else if (i == 1) {
                pImg->r47.fields.gpi2Sel = HMC7044_REG_GPIS_PLL1_REF_B0;
            } else if (i == 2) {
                pImg->r48.fields.gpi3Sel = HMC7044_REG_GPIS_PLL1_REF_B0;
            } else if (i == 3) {
                pImg->r49.fields.gpi4Sel = HMC7044_REG_GPIS_PLL1_REF_B0;
            }
            break;
        case HMC7044_GPIS_SLEEP:
            if (i == 0) {
                pImg->r46.fields.gpi1Sel = HMC7044_REG_GPIS_SLEEP;
            } else if (i == 1) {
                pImg->r47.fields.gpi2Sel = HMC7044_REG_GPIS_SLEEP;
            } else if (i == 2) {
                pImg->r48.fields.gpi3Sel = HMC7044_REG_GPIS_SLEEP;
            } else if (i == 3) {
                pImg->r49.fields.gpi4Sel = HMC7044_REG_GPIS_SLEEP;
            }
            break;
        case HMC7044_GPIS_MUTE:
             if (i == 0) {
                 pImg->r46.fields.gpi1Sel = HMC7044_REG_GPIS_MUTE;
             } else if (i == 1) {
                 pImg->r47.fields.gpi2Sel = HMC7044_REG_GPIS_MUTE;
             } else if (i == 2) {
                 pImg->r48.fields.gpi3Sel = HMC7044_REG_GPIS_MUTE;
             } else if (i == 3) {
                 pImg->r49.fields.gpi4Sel = HMC7044_REG_GPIS_MUTE;
             }
             break;
        case HMC7044_GPIS_PLL2_VCO_SEL:
             if (i == 0) {
                 pImg->r46.fields.gpi1Sel = HMC7044_REG_GPIS_PLL2_VCO_SEL;
             } else if (i == 1) {
                 pImg->r47.fields.gpi2Sel = HMC7044_REG_GPIS_PLL2_VCO_SEL;
             } else if (i == 2) {
                 pImg->r48.fields.gpi3Sel = HMC7044_REG_GPIS_PLL2_VCO_SEL;
             } else if (i == 3) {
                 pImg->r49.fields.gpi4Sel = HMC7044_REG_GPIS_PLL2_VCO_SEL;
             }
            break;
        case HMC7044_GPIS_PLL2_HPERF:
            if (i == 0) {
                pImg->r46.fields.gpi1Sel = HMC7044_REG_GPIS_PLL2_HPERF;
            } else if (i == 1) {
                pImg->r47.fields.gpi2Sel = HMC7044_REG_GPIS_PLL2_HPERF;
            } else if (i == 2) {
                pImg->r48.fields.gpi3Sel = HMC7044_REG_GPIS_PLL2_HPERF;
            } else if (i == 3) {
                pImg->r49.fields.gpi4Sel = HMC7044_REG_GPIS_PLL2_HPERF;
            }
            break;
        case HMC7044_GPIS_PULSE_GEN:
            if (i == 0) {
                pImg->r46.fields.gpi1Sel = HMC7044_REG_GPIS_PULSE_GEN;
            } else if (i == 1) {
                pImg->r47.fields.gpi2Sel = HMC7044_REG_GPIS_PULSE_GEN;
            } else if (i == 2) {
                pImg->r48.fields.gpi3Sel = HMC7044_REG_GPIS_PULSE_GEN;
            } else if (i == 3) {
                pImg->r49.fields.gpi4Sel = HMC7044_REG_GPIS_PULSE_GEN;
            }
            break;
        case HMC7044_GPIS_RESEED:
            if (i == 0) {
                pImg->r46.fields.gpi1Sel = HMC7044_REG_GPIS_RESEED;
            } else if (i == 1) {
                pImg->r47.fields.gpi2Sel = HMC7044_REG_GPIS_RESEED;
            } else if (i == 2) {
                pImg->r48.fields.gpi3Sel = HMC7044_REG_GPIS_RESEED;
            } else if (i == 3) {
                pImg->r49.fields.gpi4Sel = HMC7044_REG_GPIS_RESEED;
            }
            break;
        case HMC7044_GPIS_RESTART:
            if (i == 0) {
                pImg->r46.fields.gpi1Sel = HMC7044_REG_GPIS_RESTART;
            } else if (i == 1) {
                pImg->r47.fields.gpi2Sel = HMC7044_REG_GPIS_RESTART;
            } else if (i == 2) {
                pImg->r48.fields.gpi3Sel = HMC7044_REG_GPIS_RESTART;
            } else if (i == 3) {
                pImg->r49.fields.gpi4Sel = HMC7044_REG_GPIS_RESTART;
            }
            break;
        case HMC7044_GPIS_FANOUT_MODE:
            if (i == 0) {
                pImg->r46.fields.gpi1Sel = HMC7044_REG_GPIS_FANOUT_MODE;
            } else if (i == 1) {
                pImg->r47.fields.gpi2Sel = HMC7044_REG_GPIS_FANOUT_MODE;
            } else if (i == 2) {
                pImg->r48.fields.gpi3Sel = HMC7044_REG_GPIS_FANOUT_MODE;
            } else if (i == 3) {
                pImg->r49.fields.gpi4Sel = HMC7044_REG_GPIS_FANOUT_MODE;
            }
            break;
        case HMC7044_GPIS_SLIP:
            if (i == 0) {
                pImg->r46.fields.gpi1Sel = HMC7044_REG_GPIS_SLIP;
            } else if (i == 1) {
                pImg->r47.fields.gpi2Sel = HMC7044_REG_GPIS_SLIP;
            } else if (i == 2) {
                pImg->r48.fields.gpi3Sel = HMC7044_REG_GPIS_SLIP;
            } else if (i == 3) {
                pImg->r49.fields.gpi4Sel = HMC7044_REG_GPIS_SLIP;
            }
            break;
        default:
            sysLog("Bad value ( GPI setup %d)", pParams->gpiSup[i]);
            return ERROR;
        }

        if (pParams->gpiSup[i] != HMC7044_GPIS_NONE) {
            if (i == 0) {
                pImg->r46.fields.gpi1En = 1;
            } else if (i == 1) {
                pImg->r47.fields.gpi2En = 1;
            } else if (i == 2) {
                pImg->r48.fields.gpi3En = 1;
            } else if (i == 3) {
                pImg->r49.fields.gpi4En = 1;
            }
        }
    }

    return OK;
}




/*******************************************************************************
* - name: hmc7044CfgSdataMode
*
* - title: Configure SDATA mode for a particular device.
*
* - input: dev      - CLKDST device on which operation is performed.
*          pParams  - pointer to device setup parameters
*
* - output: hmc7044AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7044CfgSdataMode(CKDST_DEV dev,
		                 const Hmc7044_app_dev_params *pParams)
{
    Hmc7044_reg_image *pImg;

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppState.devState)) || !pParams) {
        sysLog("bad argument(s) (dev %u, pParams %d)", dev, pParams != NULL);
        return ERROR;
    }

    pImg = &hmc7044AppState.devState[dev].regImage;

    switch (pParams->sdataMode) {
    case HMC7044_OM_OD: {
        pImg->r54.fields.sdataEn = 0x1;
        pImg->r54.fields.sdataMode = HMC7044_REG_OM_OD;
        break;
    }
    case HMC7044_OM_CMOS: {
        pImg->r54.fields.sdataEn = 0x1;
        pImg->r54.fields.sdataMode = HMC7044_REG_OM_CMOS;
        break;
    }
    default:
        sysLog("Bad value ( GPI setup %d)", pParams->sdataMode);
        return ERROR;
    }

    return OK;
}




/*******************************************************************************
* - name: hmc7044CfgGpos
*
* - title: Configure GPIO setup for a particular device.
*
* - input: dev     - CLKDST device on which operation is performed.
*          pParams - pointer to device setup parameters
*
* - output: hmc70443AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7044CfgGpos(CKDST_DEV dev,
		                    const Hmc7044_app_dev_params *pParams)
{
    Hmc7044_reg_image *pImg;
    unsigned i;

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppState.devState)) || !pParams) {
        sysLog("bad argument(s) (dev %u, pParams %d)", dev, pParams != NULL);
        return ERROR;
    }

    pImg = &hmc7044AppState.devState[dev].regImage;

    pImg->r50.fields.gpo1En = 0;
    pImg->r51.fields.gpo2En = 0;
    pImg->r52.fields.gpo3En = 0;
    pImg->r53.fields.gpo4En = 0;

    for (i = 0; i < HMC7044_NGPIO; i++) {
        switch (pParams->gpoSup[i].om) {
        case HMC7044_OM_OD:
            if (i == 0) {
                pImg->r50.fields.gpo1Mode = HMC7044_REG_OM_OD;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Mode = HMC7044_REG_OM_OD;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Mode = HMC7044_REG_OM_OD;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Mode = HMC7044_REG_OM_OD;
            }
            break;
        case HMC7044_OM_CMOS:
            if (i == 0) {
                pImg->r50.fields.gpo1Mode = HMC7044_REG_OM_CMOS;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Mode = HMC7044_REG_OM_CMOS;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Mode = HMC7044_REG_OM_CMOS;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Mode = HMC7044_REG_OM_CMOS;
            }
            break;
        default:
            sysLog("Bad value ( GPI setup %d)", pParams->gpoSup[i].om);
            return ERROR;
        }

        switch (pParams->gpoSup[i].sup) {
        case HMC7044_GPOS_NONE:
            break;
        case HMC7044_GPOS_ALARM:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_ALARM;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_ALARM;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_ALARM;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_ALARM;
            }
            break;
        case HMC7044_GPOS_SDATA:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_SDATA;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_SDATA;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_SDATA;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_SDATA;
            }
            break;
        case HMC7044_GPOS_CLKIN3_LOS:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_CLKIN3_LOS;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_CLKIN3_LOS;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_CLKIN3_LOS;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_CLKIN3_LOS;
            }
            break;
        case HMC7044_GPOS_CLKIN2_LOS:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_CLKIN2_LOS;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_CLKIN2_LOS;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_CLKIN2_LOS;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_CLKIN2_LOS;
            }
            break;
        case HMC7044_GPOS_CLKIN1_LOS:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_CLKIN1_LOS;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_CLKIN1_LOS;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_CLKIN1_LOS;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_CLKIN1_LOS;
            }
            break;
        case HMC7044_GPOS_CLKIN0_LOS:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_CLKIN0_LOS;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_CLKIN0_LOS;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_CLKIN0_LOS;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_CLKIN0_LOS;
            }
            break;
        case HMC7044_GPOS_PLL1_HO_EN:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_HO_EN;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_HO_EN;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_HO_EN;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_HO_EN;
            }
            break;
        case HMC7044_GPOS_PLL1_LOCKED:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_LOCKED;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_LOCKED;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_LOCKED;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_LOCKED;
            }
            break;
        case HMC7044_GPOS_PLL1_LOCK_AQ:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_LOCK_AQ;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_LOCK_AQ;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_LOCK_AQ;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_LOCK_AQ;
            }
            break;
        case HMC7044_GPOS_PLL1_LOCK_NL:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_LOCK_NL;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_LOCK_NL;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_LOCK_NL;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_LOCK_NL;
            }
            break;
        case HMC7044_GPOS_PLL2_LOCKED:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL2_LOCKED;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL2_LOCKED;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL2_LOCKED;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL2_LOCKED;
            }
            break;
        case HMC7044_GPOS_SREF_NSYNC:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_SREF_NSYNC;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_SREF_NSYNC;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_SREF_NSYNC;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_SREF_NSYNC;
            }
            break;
        case HMC7044_GPOS_CKOUTS_PHASE:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_CKOUTS_PHASE;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_CKOUTS_PHASE;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_CKOUTS_PHASE;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_CKOUTS_PHASE;
            }
            break;
        case HMC7044_GPOS_PLLS_LOCKED:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLLS_LOCKED;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLLS_LOCKED;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLLS_LOCKED;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLLS_LOCKED;
            }
            break;
        case HMC7044_GPOS_SYNC_REQ_ST:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_SYNC_REQ_ST;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_SYNC_REQ_ST;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_SYNC_REQ_ST;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_SYNC_REQ_ST;
            }
            break;
        case HMC7044_GPOS_PLL1_ACT_C0:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_ACT_C0;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_ACT_C0;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_ACT_C0;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_ACT_C0;
            }
            break;
        case HMC7044_GPOS_PLL1_ACT_C1:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_ACT_C1;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_ACT_C1;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_ACT_C1;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_ACT_C1;
            }
            break;
        case HMC7044_GPOS_PLL1_HO_AIR:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_HO_AIR;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_HO_AIR;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_HO_AIR;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_HO_AIR;
            }
            break;
        case HMC7044_GPOS_PLL1_HO_AIS:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_HO_AIS;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_HO_AIS;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_HO_AIS;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_HO_AIS;
            }
            break;
        case HMC7044_GPOS_PLL1_VCXOST:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_VCXOST;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_VCXOST;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_VCXOST;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_VCXOST;
            }
            break;
        case HMC7044_GPOS_PLL1_ACT_CX:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_ACT_CX;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_ACT_CX;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_ACT_CX;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_ACT_CX;
            }
            break;
        case HMC7044_GPOS_PLL1_FSM_B0:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_FSM_B0;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_FSM_B0;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_FSM_B0;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_FSM_B0;
            }
            break;
        case HMC7044_GPOS_PLL1_FSM_B1:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_FSM_B1;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_FSM_B1;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_FSM_B1;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_FSM_B1;
            }
            break;
        case HMC7044_GPOS_PLL1_FSM_B2:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_FSM_B2;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_FSM_B2;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_FSM_B2;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_FSM_B2;
            }
            break;
        case HMC7044_GPOS_PLL1_HO_EP0:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_HO_EP0;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_HO_EP0;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_HO_EP0;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_HO_EP0;
            }
            break;
        case HMC7044_GPOS_PLL1_HO_EP1:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_HO_EP1;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_HO_EP1;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_HO_EP1;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_HO_EP1;
            }
            break;
        case HMC7044_GPOS_CH_FSM_BUSY:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_CH_FSM_BUSY;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_CH_FSM_BUSY;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_CH_FSM_BUSY;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_CH_FSM_BUSY;
            }
            break;
        case HMC7044_GPOS_SREF_FSM_ST0:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_SREF_FSM_ST0;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_SREF_FSM_ST0;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_SREF_FSM_ST0;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_SREF_FSM_ST0;
            }
            break;
        case HMC7044_GPOS_SREF_FSM_ST1:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_SREF_FSM_ST1;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_SREF_FSM_ST1;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_SREF_FSM_ST1;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_SREF_FSM_ST1;
            }
            break;
        case HMC7044_GPOS_SREF_FSM_ST2:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_SREF_FSM_ST2;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_SREF_FSM_ST2;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_SREF_FSM_ST2;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_SREF_FSM_ST2;
            }
            break;
        case HMC7044_GPOS_SREF_FSM_ST3:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_SREF_FSM_ST3;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_SREF_FSM_ST3;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_SREF_FSM_ST3;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_SREF_FSM_ST3;
            }
            break;
        case HMC7044_GPOS_FORCE_1:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_FORCE_1;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_FORCE_1;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_FORCE_1;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_FORCE_1;
            }
            break;
        case HMC7044_GPOS_FORCE_0:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_FORCE_0;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_FORCE_0;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_FORCE_0;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_FORCE_0;
            }
            break;
        case HMC7044_GPOS_PLL1_HO_DA0:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_HO_DA0;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_HO_DA0;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_HO_DA0;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_HO_DA0;
            }
            break;
        case HMC7044_GPOS_PLL1_HO_DA1:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_HO_DA1;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_HO_DA1;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_HO_DA1;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_HO_DA1;
            }
            break;
        case HMC7044_GPOS_PLL1_HO_DA2:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_HO_DA2;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_HO_DA2;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_HO_DA2;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_HO_DA2;
            }
            break;
        case HMC7044_GPOS_PLL1_HO_DA3:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_HO_DA3;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_HO_DA3;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_HO_DA3;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_HO_DA3;
            }
            break;
        case HMC7044_GPOS_PLL1_HO_DC0:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_HO_DC0;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_HO_DC0;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_HO_DC0;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_HO_DC0;
            }
            break;
        case HMC7044_GPOS_PLL1_HO_DC1:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_HO_DC1;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_HO_DC1;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_HO_DC1;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_HO_DC1;
            }
            break;
        case HMC7044_GPOS_PLL1_HO_DC2:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_HO_DC2;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_HO_DC2;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_HO_DC2;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_HO_DC2;
            }
            break;
        case HMC7044_GPOS_PLL1_HO_DC3:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_HO_DC3;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_HO_DC3;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_HO_DC3;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_HO_DC3;
            }
            break;
        case HMC7044_GPOS_PLL1_HO_CMP:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLL1_HO_CMP;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLL1_HO_CMP;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLL1_HO_CMP;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLL1_HO_CMP;
            }
            break;
        case HMC7044_GPOS_PLS_GEN_REQ:
            if (i == 0) {
                pImg->r50.fields.gpo1Sel = HMC7044_REG_GPOS_PLS_GEN_REQ;
            } else if (i == 1) {
                pImg->r51.fields.gpo2Sel = HMC7044_REG_GPOS_PLS_GEN_REQ;
            } else if (i == 2) {
                pImg->r52.fields.gpo3Sel = HMC7044_REG_GPOS_PLS_GEN_REQ;
            } else if (i == 3) {
                pImg->r53.fields.gpo4Sel = HMC7044_REG_GPOS_PLS_GEN_REQ;
            }
            break;
        default:
            sysLog("Bad value ( GPO setup %d)", pParams->gpoSup[i].om);
            return ERROR;
        }
        if (pParams->gpoSup[i].sup != HMC7044_GPOS_NONE) {
            if (i == 0) {
                pImg->r50.fields.gpo1En = 1;
            } else if (i == 1) {
                pImg->r51.fields.gpo2En = 1;
            } else if (i == 2) {
                pImg->r52.fields.gpo3En = 1;
            } else if (i == 3) {
                pImg->r53.fields.gpo4En = 1;
            }
        }
    }

    return OK;
}




/*******************************************************************************
* - name: hmc7044AppInitOscOutSup
*
* - title: initialize OSCOUT if used
*
* - input: dev - CLKDST device for which to perform the operation
*
* - output: hmc7044AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - notes: This routine sets up register fields within the image
*
* Ensure dual-function pins CLKIN2/OSCOUT0  used only for
* associated functions
*
* Software shall verify that PLL1 reference input pin CLKIN2 if set up
* for other functions, does not appear in used set of inputs
*
* When OSCOUTx functionality is unused, software shall disable the respective
* oscillator driver. Additionally, if both oscillators are unused, software shall
* clear register 0x0039 bit 0.
*******************************************************************************/
LOCAL STATUS hmc7044AppInitOscOutSup(CKDST_DEV dev,
                                  const Hmc7044_app_dev_params *pParams)
{
    Hmc7044_reg_image *pImg;
    unsigned i, oscoutDivideRatio;

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppState.devState)) || !pParams) {
        sysLog("bad argument(s) (dev %u, pParams %d)", dev, pParams != NULL);
        return ERROR;
    }

    if (!hmc7044IfCtl.initDone || !hmc7044AppCtl.initDone) {
        sysLog("initialization not done yet (dev %u, init. done %d,%d)", dev,
               hmc7044IfCtl.initDone, hmc7044AppCtl.initDone);

    return ERROR;
    }

    pImg = &hmc7044AppState.devState[dev].regImage;

    for (i = 0; i < HMC7044_OSC_OUT_NCHAN; i++) {
       if ((!pParams->oscOutSup.osc[0].used) &&
           (!pParams->oscOutSup.osc[1].used)) {
           /* clear 0x0039 bit 0 */
           pImg->r39.fields.oscoutPathEn = 0;
           return OK;
       }
       pImg->r39.fields.oscoutPathEn = 1;
       oscoutDivideRatio = pParams->oscInFreq / pParams->oscOutSup.freq;

       if ((oscoutDivideRatio != 1) && (oscoutDivideRatio != 2) &&
           (oscoutDivideRatio != 4) && (oscoutDivideRatio != 8)) {
           sysLog("Oscillator output divider ratio for(dev %u), possible values"
                  "can only be 1, 2, 4 or 8", dev);
       }

       if ((pParams->oscOutSup.osc[i].term100Ohm) &&
           (pParams->oscOutSup.osc[i].term50Ohm)) {
           sysLog("Both term100Ohm %d and term50Ohm %d cannot be true"
               " simultaneously", pParams->oscOutSup.osc[i].term100Ohm,
               pParams->oscOutSup.osc[i].term50Ohm);
           return ERROR;
       }

       if ((!pParams->oscOutSup.osc[i].term100Ohm) &&
           (!pParams->oscOutSup.osc[i].term50Ohm)) {
           sysLog("Both term100Ohm %d and term50Ohm %d cannot be false"
               " simultaneously", pParams->oscOutSup.osc[i].term100Ohm,
               pParams->oscOutSup.osc[i].term50Ohm);
           return ERROR;
       }

       pImg->r39.fields.oscoutDivider
                           = pParams->oscInFreq / pParams->oscOutSup.freq;
       switch (i) {
       case 0:
         if (pParams->oscOutSup.osc[i].used) {

             if (pParams->pll1Sup.refIn.inSup[2].sup.used) {
                 sysLog("OSCOUT0  is used for (dev %u),"
                        " CLKIN2 cannot be used", dev);
                 return ERROR;
             }

             pImg->r3a.fields.oscout0DrvEn = 1; /* OSCOUT0 instead of CLKIN2 */
             switch (pParams->oscOutSup.osc[i].mode) {
             case HMC7044_CDM_CML:
               pImg->r3a.fields.oscout0DrvMode = HMC7044_CH_DM_CML;
               break;
             case HMC7044_CDM_LVPECL:
               pImg->r3a.fields.oscout0DrvMode = HMC7044_CH_DM_LVPECL;
               break;
             case HMC7044_CDM_LVDS:
               pImg->r3a.fields.oscout0DrvMode = HMC7044_CH_DM_LVDS;
               break;
             case HMC7044_CDM_CMOS:
               pImg->r3a.fields.oscout0DrvMode = HMC7044_CH_DM_CMOS;
               break;
             default:
               sysLog("Bad value ( pParams->oscOutSup.osc0.mode %d)",
                            pParams->oscOutSup.osc[i].mode);
               return ERROR;
             }

             if (pParams->oscOutSup.osc[i].term100Ohm) {
                 pImg->r3a.fields.oscout0DrvImp = HMC7044_OSCOUT_TERM100;
             }
             else if (pParams->oscOutSup.osc[i].term50Ohm) {
                 pImg->r3a.fields.oscout0DrvImp = HMC7044_OSCOUT_TERM50;
             }
         }
         else { /* OSCOUT0 not used */
             pImg->r3a.fields.oscout0DrvEn = 0;
         }
         break;
       case 1:
         if (pParams->oscOutSup.osc[i].used) {
             pImg->r3b.fields.oscout1DrvEn = 1;

             switch (pParams->oscOutSup.osc[i].mode) {
             case HMC7044_CDM_CML:
               pImg->r3b.fields.oscout1DrvMode = HMC7044_CH_DM_CML;
               break;
             case HMC7044_CDM_LVPECL:
               pImg->r3b.fields.oscout1DrvMode = HMC7044_CH_DM_LVPECL;
               break;
             case HMC7044_CDM_LVDS:
               pImg->r3b.fields.oscout1DrvMode = HMC7044_CH_DM_LVDS;
               break;
             case HMC7044_CDM_CMOS:
               pImg->r3b.fields.oscout1DrvMode = HMC7044_CH_DM_CMOS;
               break;
             default:
               sysLog("Bad value ( pParams->oscOutSup.osc1.mode %d)",
                            pParams->oscOutSup.osc[i].mode);
               return ERROR;
             }

             if (pParams->oscOutSup.osc[i].term100Ohm) {
                 pImg->r3b.fields.oscout1DrvImp = HMC7044_OSCOUT_TERM100;
             }
             else if (pParams->oscOutSup.osc[i].term50Ohm) {
                 pImg->r3b.fields.oscout1DrvImp = HMC7044_OSCOUT_TERM50;
             }
         }
         else { /* OSCOUT1 not used */
             pImg->r3b.fields.oscout1DrvEn = 0;
         }
         break;
       default:
           sysLog("Bad ref Input value ( i %u)", i);
           return ERROR;
       }
    }

    return OK;
}




/*******************************************************************************
* - name: hmc7044AppInitPll2Sup
*
* - title: initialize PLL2
*
* - input: dev - CLKDST device for which to perform the operation
*
* - output: hmc7044AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - notes: This routine sets up register fields within the image
*
* Software shall verify that fPFD2 is within the limits defined in the datasheet
* (0.00015 to 250 MHz).
*
* Software shall verify that R2 input frequency is within the limits defined in
* the datasheet (10 to 500 MHz)
*
* Software shall support the option of using an external VCO for PLL2
*
* If PLL2 uses an external VCO, software shall set register 0x0064 bit 0 iff
* VCO frequency is < 1 GHz.
*
* Software shall use R2 doubler in all cases, except if exceeding the device
* limits (fPFD2).
*
*******************************************************************************/
LOCAL STATUS hmc7044AppInitPll2Sup(CKDST_DEV dev,
                                  const Hmc7044_app_dev_params *pParams)
{
    Hmc7044_app_dev_ctl *pCtl;
    Hmc7044_reg_image *pImg;
    CKDST_FREQ_HZ pfd2Freq, vcoFreq, vcoRangeLimit, oscInFreq, r2InpFreq;
    double n2DivFp;
    Bool highVcoRange;
    unsigned n2Div, r2Div = 1, doubler = 2, pll2CpCurCode;

    pCtl = hmc7044AppCtl.devCtl + dev;

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppState.devState)) || !pParams) {
        sysLog("bad argument(s) (dev %u, pParams %d)", dev, pParams != NULL);
        return ERROR;
    }

    if (!hmc7044IfCtl.initDone || !hmc7044AppCtl.initDone || !pCtl->initDone) {
        sysLog("initialization not done yet (dev %u, init. done %d,%d,%d)", dev,
               hmc7044IfCtl.initDone, hmc7044AppCtl.initDone, pCtl->initDone);

        return ERROR;
    }

    pImg = &hmc7044AppState.devState[dev].regImage;

    vcoFreq = pParams->pll2Sup.vcoFreq;
    oscInFreq = pParams->oscInFreq;

    if (vcoFreq < HMC7044_LOW_VCO_MIN || vcoFreq > HMC7044_HIGH_VCO_MAX)
        return ERROR;

    /* Select the VCO range */
    vcoRangeLimit = (HMC7044_LOW_VCO_MAX + HMC7044_HIGH_VCO_MIN) / 2;
    if (vcoFreq >= vcoRangeLimit)
        highVcoRange = TRUE;
    else
        highVcoRange = FALSE;

   /* Program R2 */
    r2Div = pParams->pll2Sup.rDiv;

    if (r2Div < HMC7044_R2DIV_MIN || r2Div > HMC7044_R2DIV_MAX) {
        sysLogFpa("bad R2 divider (%u) for dev %.0f (Oscin freq %.0f)", r2Div,
                 (double) dev, (double) oscInFreq);
        return ERROR;
    }

    pfd2Freq = oscInFreq * ((pParams->pll2Sup.rDoubler) ? 2 : 1) / r2Div;

    if (pParams->pll2Sup.rDoubler) {
       r2InpFreq = oscInFreq;
    } else {
       r2InpFreq = oscInFreq * doubler;
    }

    if (r2InpFreq < HMC7044_R2_MIN || r2InpFreq > HMC7044_R2_MAX) {
        sysLogFpa("R2 frequency (%.0f) outside limits for device %.0f",
                  (double) r2InpFreq, (double) dev);
        return ERROR;
    }

    if (pfd2Freq < HMC7044_PFD2_MIN || pfd2Freq > HMC7044_PFD2_MAX) {
        sysLogFpa("PFD2 frequency (%.0f) outside limits for device %.0f",
                  (double) pfd2Freq, (double) dev);
        return ERROR;
    }

    /* Set the timeout set while waiting for the PLL to be locked as
     * 5 times PLL2 lock threshold (512) * PLL2 PFD period */
    pCtl->nSecPll2LockTmout = 5 * 512 * (1 / pfd2Freq);

   /* Program N2 */
    n2DivFp = vcoFreq / pfd2Freq ;
    n2Div = (UINT32) (n2DivFp + 0.5);

    if (n2Div < HMC7044_N2DIV_MIN || n2Div > HMC7044_N2DIV_MAX) {
        sysLogFpa("bad N2 divider (%.0f) for dev %.0f (Oscin freq %.0f)",
                  (double) n2Div, (double) dev, (double) oscInFreq);
        return ERROR;
    }

    pImg->r03.fields.vcoSel = highVcoRange ? HMC7044_VCO_HIGH : HMC7044_VCO_LOW;
    pImg->r33.fields.lsbR2Divider = HMC7044_LSB(r2Div);
    pImg->r34.fields.msbR2Divider = HMC7044_MSB(r2Div);

    pImg->r35.fields.lsbN2Divider = HMC7044_LSB(n2Div);
    pImg->r36.fields.msbN2Divider = HMC7044_MSB(n2Div);
    pImg->r32.fields.bypassFreqDoubler = pParams->pll2Sup.rDoubler;
    pImg->r05.fields.clk1ExtVco = pParams->pll2Sup.extVco;

    if (pParams->pll2Sup.extVco) {
        if (pParams->pll1Sup.refIn.inSup[1].sup.used) {
            sysLog("FIN for external VCO is used for (dev %u), "
                   "CLKIN1 cannot be used", dev);
            return ERROR;
        }
        pImg->r03.fields.vcoSel = 0;
        /* If PLL2 uses an external VCO, software shall set register 0x0064 bit 0
         * iff VCO frequency is < 1 GHz */
        if (vcoFreq < 1e6) {
            pImg->r64.fields.lowFreqExtVco = 1;
        }
        else {
            pImg->r64.fields.lowFreqExtVco = 0;
        }

        if (pParams->pll2Sup.finDiv == HMC7044_FID_1) {
          pImg->r64.fields.divBy2ExtVco = 0;
        }
        else if (pParams->pll2Sup.finDiv == HMC7044_FID_2) {
          pImg->r64.fields.divBy2ExtVco = 1;
        }
        else {
           sysLog("bad PLL2 FID value (%u; dev %u)", pParams->pll2Sup.finDiv, dev);
           return ERROR;
        }
    }

    /* PLL2 charge pump Current */
    if (hmc7044RegPll2CpCur2Code(pParams->pll2Sup.cpCurUa, &pll2CpCurCode) != OK) {
         sysLog("bad charge pump current (%u; dev %u)",
                pParams->pll1Sup.cpCurUa, dev);
         return ERROR;
     }

    pImg->r37.fields.pll2CpCurrent = pll2CpCurCode;

    return OK;
}




/*******************************************************************************
* - name: hmc7044AppInitPll1Sup
*
* - title: initialize PLL1
*
* - input: dev - CLKDST device for which to perform the operation
*
* - output: hmc7044AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - notes:
* This routine sets up register fields within the image.
*
* Ensure dual function pins CLKIN0/RFSYNCIN, CLKIN1/FIN are used only for
* associated functions
*
* If PLL1 is disabled in setup, software shall disable it in register 0x0003
*
* Software shall verify that PLL1 reference input pins that have been set up
* for other functions, do not appear in the set of used reference inputs.
*
* It is an error to specify an unused PLL1 reference input in the list of PLL1
* reference input priorities
*
* When a PLL1 reference input is unused, software shall disable the associated
* input buffer
*
* Software shall use a fixed hard-coded value for the PLL1 LOS validation timer
* Specifically will use value 5.
*
* Software shall verify that fPFD1 is within the limits defined in the datasheet
*
* PLL1 lock detect timer setting- Software shall set lock detect timer to a value
* that is >= 4 * fLCM / loop filter bandwidth.
*
* If invertedSync is set, useRfSync must not be set and CLKIN0 is used.
*
* Software shall enable the PLL1 holdover DAC
*
* Software shall use the following PLl1 holdover exit method: DAC assisted release
*
* PLL1 chargepump current scaling:120 uA per count,minimum 120,maximum 1920
*******************************************************************************/
LOCAL STATUS hmc7044AppInitPll1Sup(CKDST_DEV dev,
                                  const Hmc7044_app_dev_params *pParams)
{
    Hmc7044_app_dev_ctl *pCtl;
    Hmc7044_reg_image *pImg;
    unsigned i, inPrescaler[5], pll1LockDetect, pll1CpCurCode;
    unsigned bitCount = 0, lockCalc = 0, templockCalc = 0, n1Div;
    Hmc_7044_pll1_ref_clk_pri clkInPri;
    CKDST_FREQ_HZ pfd1Freq;
    Bool ok;

    pCtl = hmc7044AppCtl.devCtl + dev;
    if (!inEnumRange(dev, NELEMENTS(hmc7044AppState.devState)) || !pParams) {
        sysLog("bad argument(s) (dev %u, pParams %d)", dev, pParams != NULL);
        return ERROR;
    }

    if (!hmc7044IfCtl.initDone || !hmc7044AppCtl.initDone || !pCtl->initDone) {
        sysLog("initialization not done yet (dev %u, init. done %d,%d,%d)", dev,
               hmc7044IfCtl.initDone, hmc7044AppCtl.initDone, pCtl->initDone);
        return ERROR;
    }

    pImg = &hmc7044AppState.devState[dev].regImage;

    if (!pParams->pll1Sup.used) {
        sysLog("PLL1 disabled in setup for dev %u)", dev);
        pImg->r03.fields.pll1En = 0;
        return OK;
    }

    for (i = 0; i < HMC7044_P1RI_NIN; i++) {
        switch (i) {
        case HMC7044_P1RI_0:
          if (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_0].sup.used) {
              pImg->r0a.fields.clkin0BufEn = 0x1;
              /* Set the clock input buffer mode */
              pImg->r0a.fields.clkin0InpBufMode =
               (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_0].sup.highZ << 3)|
               (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_0].sup.lvpecl << 2) |
               (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_0].sup.acCoupled << 1) |
               (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_0].sup.term100Ohm << 0);

              /* Set the input Prescaler value */
              inPrescaler[i] =
                  pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_0].freq / pCtl->lcmFreq;
          } else {
              pImg->r0a.fields.clkin0BufEn = 0x0;
              inPrescaler[i] = 1;
          }
          break;
        case HMC7044_P1RI_1:
          if (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_1].sup.used) {
              pImg->r0b.fields.clkin1BufEn = 0x1;
              /* Set the clock input buffer mode */
              pImg->r0b.fields.clkin1InpBufMode =
                (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_1].sup.highZ << 3)|
                (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_1].sup.lvpecl << 2) |
                (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_1].sup.acCoupled << 1) |
                (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_1].sup.term100Ohm << 0);

              /* Set the input Prescaler value */
              inPrescaler[i] =
                  pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_1].freq / pCtl->lcmFreq;
          } else {
              pImg->r0b.fields.clkin1BufEn = 0x0;
              inPrescaler[i] = 1;
          }
          break;
        case HMC7044_P1RI_2:
          if (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_2].sup.used) {
              pImg->r0c.fields.clkin2BufEn = 0x1;
              pImg->r0c.fields.clkin2InpBufMode =
                (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_2].sup.highZ << 3) |
                (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_2].sup.lvpecl << 2) |
                (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_2].sup.acCoupled << 1) |
                (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_2].sup.term100Ohm << 0);

              /* Set the input Prescaler value */
              inPrescaler[i] =
                  pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_2].freq / pCtl->lcmFreq;

              /* OSCOUT0 buffer does not drive CLKIN2 */
              pImg->r3a.fields.oscout0DrvEn = 0;
          } else {
              pImg->r0c.fields.clkin2BufEn = 0x0;
              inPrescaler[i] = 1;
          }
          break;
        case HMC7044_P1RI_3:
          if (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_3].sup.used) {
              pImg->r0d.fields.clkin3BufEn = 0x1;
              pImg->r0d.fields.clkin3InpBufMode =
                (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_3].sup.highZ << 3) |
                (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_3].sup.lvpecl << 2) |
                (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_3].sup.acCoupled << 1) |
                (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_3].sup.term100Ohm << 0);

              /* Set the input Prescaler value */
              inPrescaler[i] =
                  pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_3].freq / pCtl->lcmFreq;
          } else {
              pImg->r0d.fields.clkin3BufEn = 0x0;
              inPrescaler[i] = 1;
          }
          break;
        default:
          sysLog("Bad ref Input value ( i %u)", i);
          return ERROR;
        }
    }

    /* OSCIN frequency is used for PreScaler 4 */
    inPrescaler[4] =  pParams->oscInFreq / pCtl->lcmFreq;

    /* Set the PLL1 Reference Priority Control 0x0014 */
    /* It is an error to specify an unused PLL1 reference input in the list of
       PLL1 reference input priorities */
    if (pParams->pll1Sup.refIn.inSup[pParams->pll1Sup.refIn.pri._1stPri].sup.used) {
        switch (pParams->pll1Sup.refIn.pri._1stPri) {
        case HMC7044_P1RI_0: clkInPri.clk1stPri = HMC7044_P1RI_CLKIN0; break;
        case HMC7044_P1RI_1: clkInPri.clk1stPri = HMC7044_P1RI_CLKIN1; break;
        case HMC7044_P1RI_2: clkInPri.clk1stPri = HMC7044_P1RI_CLKIN2; break;
        case HMC7044_P1RI_3: clkInPri.clk1stPri = HMC7044_P1RI_CLKIN3; break;
        default:
            sysLog("Bad ref Input value ( 1st priority %d)",
                   pParams->pll1Sup.refIn.pri._1stPri);
            return ERROR;
        }
        pImg->r14.fields.firstPriClkin = clkInPri.clk1stPri;
    } else {
        sysLog("CLK %d in reference priority is not a used input",
               pParams->pll1Sup.refIn.pri._1stPri);
        return ERROR;
    }

    if (pParams->pll1Sup.refIn.inSup[pParams->pll1Sup.refIn.pri._2ndPri].sup.used) {
        switch (pParams->pll1Sup.refIn.pri._2ndPri) {
        case HMC7044_P1RI_0: clkInPri.clk2ndPri = HMC7044_P1RI_CLKIN0; break;
        case HMC7044_P1RI_1: clkInPri.clk2ndPri = HMC7044_P1RI_CLKIN1; break;
        case HMC7044_P1RI_2: clkInPri.clk2ndPri = HMC7044_P1RI_CLKIN2; break;
        case HMC7044_P1RI_3: clkInPri.clk2ndPri = HMC7044_P1RI_CLKIN3; break;
        default:
            sysLog("Bad ref Input value ( 2nd priority %d)",
                   pParams->pll1Sup.refIn.pri._2ndPri);
            return ERROR;
        }
        pImg->r14.fields.secondPriClkin = clkInPri.clk2ndPri;
    } else {
        sysLog("CLK %d in reference priority is not a used input",
               pParams->pll1Sup.refIn.pri._2ndPri);
        return ERROR;
    }

    if (pParams->pll1Sup.refIn.inSup[pParams->pll1Sup.refIn.pri._3rdPri].sup.used) {
        switch (pParams->pll1Sup.refIn.pri._3rdPri) {
        case HMC7044_P1RI_0: clkInPri.clk3rdPri = HMC7044_P1RI_CLKIN0; break;
        case HMC7044_P1RI_1: clkInPri.clk3rdPri = HMC7044_P1RI_CLKIN1; break;
        case HMC7044_P1RI_2: clkInPri.clk3rdPri = HMC7044_P1RI_CLKIN2; break;
        case HMC7044_P1RI_3: clkInPri.clk3rdPri = HMC7044_P1RI_CLKIN3; break;
        default:
            sysLog("Bad ref Input value ( 3rd priority %d)",
                   pParams->pll1Sup.refIn.pri._3rdPri);
            return ERROR;
        }
        pImg->r14.fields.thirdPriClkin = clkInPri.clk3rdPri;
    } else {
        sysLog("CLK %d in reference priority is not a used input",
               pParams->pll1Sup.refIn.pri._3rdPri);
        return ERROR;
    }

    if (pParams->pll1Sup.refIn.inSup[pParams->pll1Sup.refIn.pri._4thPri].sup.used) {
        switch (pParams->pll1Sup.refIn.pri._4thPri) {
        case HMC7044_P1RI_0: clkInPri.clk4thPri = HMC7044_P1RI_CLKIN0; break;
        case HMC7044_P1RI_1: clkInPri.clk4thPri = HMC7044_P1RI_CLKIN1; break;
        case HMC7044_P1RI_2: clkInPri.clk4thPri = HMC7044_P1RI_CLKIN2; break;
        case HMC7044_P1RI_3: clkInPri.clk4thPri = HMC7044_P1RI_CLKIN3; break;
        default:
            sysLog("Bad ref Input value ( 4rth priority %d)",
                   pParams->pll1Sup.refIn.pri._4thPri);
            return ERROR;
        }
       pImg->r14.fields.fourthPriClkin = clkInPri.clk4thPri;
    } else {
        sysLog("CLK %d in reference priority is not a used input",
               pParams->pll1Sup.refIn.pri._4thPri);
        return ERROR;
    }

    pImg->r1c.fields.Clkin0InpPreScaler = inPrescaler[0];
    pImg->r1d.fields.Clkin1InpPreScaler = inPrescaler[1];
    pImg->r1e.fields.Clkin2InpPreScaler = inPrescaler[2];
    pImg->r1f.fields.Clkin3InpPreScaler = inPrescaler[3];
    pImg->r20.fields.OscinInpPreScaler =  inPrescaler[4];

    if (pParams->pll1Sup.rDiv <= HMC7044_R1DIV_MIN &&
        pParams->pll1Sup.rDiv >= HMC7044_R1DIV_MAX) {
        sysLog("bad R1 divider (%u) for dev %u", pParams->pll1Sup.rDiv, dev);
        return ERROR;
    }

   /* Set R1 and N1 divider points */
    pImg->r21.fields.lsbR1Divider = HMC7044_LSB(pParams->pll1Sup.rDiv);
    pImg->r22.fields.msbR1Divider = HMC7044_MSB(pParams->pll1Sup.rDiv);

    pfd1Freq = (CKDST_FREQ_HZ) (pCtl->lcmFreq / pParams->pll1Sup.rDiv);

    if (pfd1Freq < HMC7044_PFD1_FREQ_MIN ||
        pfd1Freq > HMC7044_PFD1_FREQ_MAX) {
        sysLogFpa("PFD1 frequency (%.0f) outside limits for device %d",
                  (double) pfd1Freq, dev);
        return ERROR;
    }

    /* fOSCIN / N1 = fLCM / R1 */
    n1Div = pParams->oscInFreq * pParams->pll1Sup.rDiv / pCtl->lcmFreq;
    ok = n1Div >= HMC7044_N1DIV_MIN && n1Div <= HMC7044_N1DIV_MAX;

    if (!ok) {
        sysLog("bad N1 divider (%u) for dev %u", n1Div, dev);
        return ERROR;
    }

    pImg->r26.fields.lsbN1Divider = HMC7044_LSB(n1Div);
    pImg->r27.fields.msbN1Divider = HMC7044_MSB(n1Div);

    /* Calculate 2^(lock time detect timer value) based on loop filter bandwidth */
    lockCalc = (pCtl->lcmFreq * 4) / pParams->pll1Sup.loopFilterBw;
    templockCalc = lockCalc;
    /* Calculate log to the base 2 of lockCalc */
    while (templockCalc) {
        if (templockCalc & 0x1)
          pll1LockDetect = bitCount;
        templockCalc >>= 1;
        bitCount ++;
    }

    pImg->r28.fields.pll1LockDetect = (pll1LockDetect) & 0x1f;

    pCtl->nSecPll1LockTmout = 5 * lockCalc * (1 / (pCtl->lcmFreq));

    if (pParams->oscInSup.used) {
        pImg->r0e.fields.OscinInpBufMode = (pParams->oscInSup.highZ << 3) |
                                           (pParams->oscInSup.lvpecl << 2) |
                                           (pParams->oscInSup.acCoupled << 1) |
                                           (pParams->oscInSup.term100Ohm << 0);
    }

    /* PLL1 Reference Path Enable */
    pImg->r05.fields.pl1RefPathEn =
        (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_3].sup.used << 3) |
        (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_2].sup.used << 2) |
        (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_1].sup.used << 1) |
        (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_0].sup.used << 0);

    /* If invertedSync is set, useRfSync must not be set, CLKIN0 is used */
    if (pParams->sysref.invertedSync) {
        if (pParams->sysref.useRfSync) {
            sysLog("If invertedSync is set for (dev %u),"
                   " useRfSync must not be set", dev);
            return ERROR;
        }
        if (!pParams->pll1Sup.refIn.inSup[0].sup.used) {
            sysLog("CLKIN0 need to be used for(dev %u), "
                   "if invertedSync is set", dev);
            return ERROR;
        }
    }
    pImg->r5b.fields.syncPolarity = pParams->sysref.invertedSync;

    /* CLKIN0/RFSYNCIN has dual function: If RFSYNCIN mode, CLKIN0 is not used */
    if (pParams->sysref.useRfSync) {
        if (pParams->pll1Sup.refIn.inSup[0].sup.used) {
            sysLog("RF SYNC is used for (dev %u), CLKIN0 cannot be used", dev);
            return ERROR;
        }
    }
    pImg->r05.fields.clk0RFSync = pParams->sysref.useRfSync;

   /* PLL1 chargepump current scaling:120 uA per count,minimum 120,maximum 1920 */
    /* convert PLl1 charge pump current to associated code */
    if (hmc7044RegPll1CpCur2Code(pParams->pll1Sup.cpCurUa, &pll1CpCurCode) != OK) {
        sysLog("bad charge pump current (%u; dev %u)",
               pParams->pll1Sup.cpCurUa, dev);
        return ERROR;
    }

    pImg->r1a.fields.pll1CpCurrent = pll1CpCurCode;
    pImg->r15.fields.losValidnTimer = 0x5;

    /* Software shall use the following PLl1 holdover exit method: DAC assisted
     release */
    pImg->r16.fields.holdOvrExitCriteria = 0x0;
    pImg->r16.fields.HoldOvrExitAction = 0x3;

    pImg->r29.fields.autoModeRefSwitch = pParams->pll1Sup.refIn.autoRefSw;

    return OK;
}




/*******************************************************************************
* - name: hmc7044AppInitOscInSup
*
* - title: initialize OSCIN
*
* - input: dev - CLKDST device for which to perform the operation
*
* - output: hmc7044AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - notes:
*   OSCIN must always be used. If user indicates that OSCIN is unused, software
    shall treat it as a parameter error.
*******************************************************************************/
LOCAL STATUS hmc7044AppInitOscInSup(CKDST_DEV dev,
                                  const Hmc7044_app_dev_params *pParams)
{
    Hmc7044_app_dev_ctl *pCtl;
    Hmc7044_reg_image *pImg;

    pCtl = hmc7044AppCtl.devCtl + dev;

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppState.devState)) || !pParams) {
        sysLog("bad argument(s) (dev %u, pParams %d)", dev, pParams != NULL);
        return ERROR;
    }

    if (!hmc7044IfCtl.initDone || !hmc7044AppCtl.initDone || !pCtl->initDone) {
        sysLog("initialization not done yet (dev %u, init. done %d,%d,%d)", dev,
               hmc7044IfCtl.initDone, hmc7044AppCtl.initDone, pCtl->initDone);

        return ERROR;
    }

    if (!pParams->oscInSup.used) {
        sysLog("Parameter error(dev %u, OSCIN unused)", dev);
        return ERROR;
    }

    pImg = &hmc7044AppState.devState[dev].regImage;
    pImg->r0e.fields.OscinBufEn = 1;
    pImg->r0e.fields.OscinInpBufMode = (pParams->oscInSup.highZ << 3) |
                                       (pParams->oscInSup.lvpecl << 2) |
                                       (pParams->oscInSup.acCoupled << 1) |
                                       (pParams->oscInSup.term100Ohm << 0);

    return OK;
}




/*******************************************************************************
* - name: hmc7044AppInitOutputCh
*
* - title: Program the output used channels
*
* - input: dev - CLKDST device for which to perform the operation
*          pParams - pointer to device setup parameters
*
* - output: hmc7044AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - notes:
*
* When output buffers are configured in CMOS mode and phase alignment is required
* among the outputs, additional multislip delays must be issued for Channel 0,
* Channel 3,  Channel 5, Channel 6, Channel 9, Channel 10, and Channel 13.
*
* Verify that analog delay is an integral multiple of 25 ps within
* 0.1 ps accuracy and aDlyPs <= 23 * 25. ALso verify that the specified value is
* zero when outSel != HMC7044_COS_DIV_ADLY.
*
* Verify that digital delay is an integral multiple of 0.5 VCO cycle within
* 0.1 ps accuracy and is dDlyPs <= 17  VCO half-cycles.
*
* Software shall not allow setting phase offset > (50% - 8) clock input cycles
* for SYSREF output channels configured for pulse generation mode.
*
*******************************************************************************/
LOCAL STATUS hmc7044AppInitOutputCh(CKDST_DEV dev,
                                    const Hmc7044_app_dev_params *pParams)
{

    Hmc7044_reg_image *pImg;
    unsigned ch, chDivider, multiSlipVal, addtlMultiSlip, anlgDelayVal, digDelayVal;
    double halfClock, frem, vcoCyclePs, frea, fred, clkInpPeriod, clkOutPeriod;
    double maxDigDlyPs;
    static const double TOL = 0.1;  /* ps */
    static const double epsilon = 1e-2;

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppState.devState)) || !pParams) {
        sysLog("bad argument(s) (dev %u, pParams %d)", dev, pParams != NULL);
        return ERROR;
    }

    pImg = &hmc7044AppState.devState[dev].regImage;

    vcoCyclePs = (1 / pParams->pll2Sup.vcoFreq) * 1e12;

    /* Set the  output channel parameters */
    for (ch = 0; ch < HMC7044_OUT_NCHAN; ch++) {
        if (pParams->chSup[ch].chMode != HMC7044_CHM_UNUSED) {

            /* Set the channel divider value */
            if (pParams->pll2Sup.extVco == FALSE) {
                chDivider = pParams->pll2Sup.vcoFreq / pParams->chSup[ch].freq;
            } else { /* external VCO is used */
                chDivider = pParams->pll2Sup.vcoFreq / (pParams->pll2Sup.finDiv
                    * pParams->chSup[ch].freq);
            }

            /* Verify that if the start-up mode of a SYSREF output channel is
             * configured to be in pulse generator mode, its divide ratio
             * should be > 31 */
            if (pParams->chSup[ch].chMode == HMC7044_CHM_SYSREF){
                if (pParams->chSup[ch].dynDriverEn) {
                    if (chDivider <= HMC7044_MIN_PULSE_GEN_CH_DIVIDER) {
                        sysLog("SYSREF channel configured in pulse generator mode"
                               "should have divide ratio (%u) greater than 31.",
                            chDivider);
                        return ERROR;
                    }
                }
            }

            /* Check channel divider range */
            if (chDivider < HMC7044_MIN_CH_DIVIDER ||
                chDivider > HMC7044_MAX_CH_DIVIDER) {
                sysLog("Channel Divider(%u) value should be between 1 and 4094.",
                       chDivider);
                return ERROR;
            }

            if (((chDivider % 2) != 0) && (chDivider != 1 && chDivider != 3 &&
                  chDivider != 5)) {
                sysLog("Channel divider is %u. Odd divide ratio for an output "
                    "channel, other than 1, 3 and 5 are not supported.", chDivider);
                return ERROR;
            }

            if ((pParams->chSup[ch].slipQuantumPs / vcoCyclePs) > 1) {
                frem = fmod(pParams->chSup[ch].slipQuantumPs, vcoCyclePs);
                if (!(frem < TOL || frem > vcoCyclePs - TOL)) {
                    sysLog("slipQuantumPs ( pParams->chSup[ch].slipQuantumPs %f)"
                        " should be an integral multiple of VCO cycles within"
                           " 0.1 ps accuracy", pParams->chSup[ch].slipQuantumPs);
                    return ERROR;
                }
                multiSlipVal = (unsigned) (pParams->chSup[ch].slipQuantumPs
                                            / vcoCyclePs);
            }

            if (pParams->chSup[ch].aDlyPs > HMC7044_MAX_ADLY_PS) {
                sysLog("aDlyPs ( pParams->chSup[ch].aDlyPs %f) should"
                        "not be greater than 23 times 25 ps ",
                        pParams->chSup[ch].aDlyPs);
                return ERROR;
            }

            if (pParams->chSup[ch].outSel != HMC7044_COS_DIV_ADLY) {
                if (pParams->chSup[ch].aDlyPs != 0 ) {
                    sysLog("aDlyPs (pParams->chSup[ch].aDlyPs %f) should be zero"
                           " if channel output mux selection is other than Analog "
                            "delay output", pParams->chSup[ch].aDlyPs);
                    return ERROR;
                }
            }

            frea = fmod(pParams->chSup[ch].aDlyPs, HMC7044_ADLY_STEP_PS);
            if (!(frea < TOL || frea > HMC7044_ADLY_STEP_PS - TOL)) {
                sysLog("aDlyPs ( pParams->chSup[ch].aDlyPs %f) should"
                        "be an integral multiple of 25 ps within 0.1 ps accuracy",
                        pParams->chSup[ch].aDlyPs);
                return ERROR;
            }

            halfClock = 0.5 * ((double)1/pParams->pll2Sup.vcoFreq) * 1e12;
            if (pParams->chSup[ch].dDlyPs > (17 * halfClock)) {
                sysLog("dDlyPs ( pParams->chSup[ch].dDlyPs %f) should not be "
                    "greater than 17 half VCO cycles ", pParams->chSup[ch].dDlyPs);
                return ERROR;
            }

            fred = fmod(pParams->chSup[ch].dDlyPs, halfClock);
            if (!(fred < TOL || fred > halfClock - TOL)) {
                sysLog("dDlyPs (pParams->chSup[ch].dDlyPs %f) should be an "
                       "integral multiple of 0.5 VCO cycle within 0.1 ps accuracy",
                        pParams->chSup[ch].dDlyPs);
                return ERROR;
            }

            if (pParams->chSup[ch].dynDriverEn && pParams->pll2Sup.vcoFreq >
                HMC7044_MIN_RUNT_PULSE_FREQ) {
                clkOutPeriod = (double)1/pParams->chSup[ch].freq;

                /* if extVco is used, then use CLKIN1 period or CLKIN1 period x 2
                 * (as per finDiv).*/
                if (pParams->pll2Sup.extVco == FALSE) {
                    clkInpPeriod = (double)1 / pParams->pll2Sup.vcoFreq;
                } else {
                    clkInpPeriod = (double)1 /
                             (pParams->pll1Sup.refIn.inSup[HMC7044_P1RI_1].freq);
                    clkInpPeriod *= pParams->pll2Sup.finDiv;
                }
                maxDigDlyPs = ((0.5 * clkOutPeriod) - (8 * clkInpPeriod)) * 1e12;

                if (maxDigDlyPs < 0) {
                    sysLog("dDlyPs must be adjusted since maxDigDlyPs (%f) "
                        "should not be negative", maxDigDlyPs);
                    return ERROR;
                }

                if (pParams->chSup[ch].dDlyPs > maxDigDlyPs) {
                    sysLog("dDlyPs (pParams->chSup[ch].dDlyPs %f) should not be"
                        "greater than 50 percent output clock period - 8  times "
                        "digital delay step size.", pParams->chSup[ch].dDlyPs);
                    return ERROR;
                }
            }

            anlgDelayVal = (unsigned)(pParams->chSup[ch].aDlyPs
                                              / HMC7044_ADLY_STEP_PS);
            digDelayVal = (unsigned)(pParams->chSup[ch].dDlyPs / halfClock);

            switch (ch) {
              case 0:
                pImg->rc8.fields.chout0HighPerfMode
                            = pParams->chSup[ch].highPerfMode;
                pImg->rc8.fields.chout0SyncEn = 0x1;

                /* Configure start-up mode */
                if (pParams->chSup[ch].dynDriverEn)
                    pImg->rc8.fields.chout0StMode = HMC7044_STMOD_DYN;
                else
                    pImg->rc8.fields.chout0StMode = HMC7044_STMOD_ASYNC;

                pImg->rc9.fields.chout0LsbChannelDiv = HMC7044_LSB(chDivider);
                pImg->rca.fields.chout0MsbChannelDiv = HMC7044_MSB(chDivider);

                /* Configure driver mode */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                        pImg->rd0.fields.chout0DriverMode = HMC7044_CH_DM_CML;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVPECL) {
                        pImg->rd0.fields.chout0DriverMode = HMC7044_CH_DM_LVPECL;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVDS) {
                        pImg->rd0.fields.chout0DriverMode = HMC7044_CH_DM_LVDS;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_CMOS) {
                        pImg->rd0.fields.chout0DriverMode = HMC7044_CH_DM_CMOS;
                } else {
                    sysLog("Bad value (driver mode %u), channel %u",
                      pParams->chSup[ch].drvMode, ch);
                    return ERROR;
                }

                /* Configure Force Mute */
                if (pParams->chSup[ch].chMode == HMC7044_CHM_CLK) {
                    pImg->rd0.fields.chout0ForceMute = HMC7044_FORCE_MUTE_NORMAL;
                } else if (pParams->chSup[ch].chMode == HMC7044_CHM_SYSREF) {
                    pImg->rd0.fields.chout0ForceMute = HMC7044_FORCE_MUTE_LOGIC0;
                    pImg->rd0.fields.chout0DynamicDriverEn
                        = pParams->chSup[ch].dynDriverEn;
                } else {
                    sysLog("Bad value (channel mode  %d), channel %u",
                                               pParams->chSup[ch].chMode, ch);
                    return ERROR;
                }

                /* Driver Impedance */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                    if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_NONE) {
                        pImg->rd0.fields.chout0DriverImp = HMC7044_DRV_IMP_NONE;
                    }
                    else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_100){
                        pImg->rd0.fields.chout0DriverImp = HMC7044_DRV_IMP_100;
                    }
                    else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_50){
                        pImg->rd0.fields.chout0DriverImp = HMC7044_DRV_IMP_50;
                    }
                    else {
                        sysLog("Bad value ( Driver Impedance %d), channel %u",
                               pParams->chSup[ch].cmlTerm, ch);
                        return ERROR;
                    }
                }

                /* Output Mux Selection */
                if (pParams->chSup[ch].outSel == HMC7044_COS_DIVIDER) {
                    pImg->rcf.fields.chout0OutputMuxSel = HMC7044_OMS_DIVIDER;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_ADLY) {
                    pImg->rcf.fields.chout0OutputMuxSel = HMC7044_OMS_DIV_ADLY;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_NEIGHBOR) {
                    pImg->rcf.fields.chout0OutputMuxSel = HMC7044_OMS_DIV_NEIGHBOR;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_FUNDAMENTAL) {
                    pImg->rcf.fields.chout0OutputMuxSel = HMC7044_OMS_FUNDAMENTAL;
                } else {
                    sysLog("Bad value ( Output Mux %d), channel %u",
                           pParams->chSup[ch].outSel, ch);
                    return ERROR;
                }

                if (pParams->chSup[ch].slipQuantumPs == 0) {
                    pImg->rc8.fields.chout0SlipEn = 0x0;
                    pImg->rc8.fields.chout0MultiSlipEn = 0x0;
                } else if (fabs(pParams->chSup[ch].slipQuantumPs - vcoCyclePs)
                                < epsilon ) {
                    pImg->rc8.fields.chout0SlipEn = 0x1;
                    pImg->rc8.fields.chout0MultiSlipEn = 0x0;
                }
                else if (multiSlipVal > 1) {
                    pImg->rc8.fields.chout0SlipEn = 0x0;
                    pImg->rc8.fields.chout0MultiSlipEn = 0x1;
                    if (pParams->chSup[ch].drvMode == HMC7044_CDM_CMOS) {
                        addtlMultiSlip = chDivider / 2;
                        multiSlipVal += addtlMultiSlip;
                    }
                    pImg->rcd.fields.chout0LsbmultiSlipDigtlDelay
                        = HMC7044_LSB(multiSlipVal);
                    pImg->rce.fields.chout0MsbMultiSlipDigtlDelay
                        = HMC7044_MSB(multiSlipVal);
                } else {
                    sysLogFpa("Bad value ( slipQuantumPs  %0.0f), channel %f",
                             pParams->chSup[ch].slipQuantumPs, (double) ch);
                    return ERROR;
                 }

                pImg->rcb.fields.chout0FineAnlgDelay = anlgDelayVal;
                pImg->rcc.fields.chout0CoarseDigtlDelay = digDelayVal;
                pImg->rc8.fields.chout0ChannelEn = 0x1;

                break;
              case 1:
                pImg->rd2.fields.chout1HighPerfMode
                    = pParams->chSup[ch].highPerfMode;
                pImg->rd2.fields.chout1SyncEn = 0x1;

                /* Configure start-up mode */
                if (pParams->chSup[ch].dynDriverEn)
                    pImg->rd2.fields.chout1StMode = HMC7044_STMOD_DYN;
                else
                    pImg->rd2.fields.chout1StMode = HMC7044_STMOD_ASYNC;

                pImg->rd3.fields.chout1LsbChannelDiv = HMC7044_LSB(chDivider);
                pImg->rd4.fields.chout1MsbChannelDiv = HMC7044_MSB(chDivider);

                /* Configure driver mode */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                        pImg->rda.fields.chout1DriverMode = HMC7044_CH_DM_CML;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVPECL) {
                        pImg->rda.fields.chout1DriverMode = HMC7044_CH_DM_LVPECL;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVDS) {
                        pImg->rda.fields.chout1DriverMode = HMC7044_CH_DM_LVDS;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_CMOS) {
                        pImg->rda.fields.chout1DriverMode = HMC7044_CH_DM_CMOS;
                } else {
                    sysLog("Bad value ( driver mode %d), channel %u",
                           pParams->chSup[ch].drvMode, ch);
                    return ERROR;
                }

                /* Configure Force Mute */
                if (pParams->chSup[ch].chMode == HMC7044_CHM_CLK) {
                    pImg->rda.fields.chout1ForceMute = HMC7044_FORCE_MUTE_NORMAL;
                } else if (pParams->chSup[ch].chMode == HMC7044_CHM_SYSREF) {
                    pImg->rda.fields.chout1ForceMute = HMC7044_FORCE_MUTE_LOGIC0;
                    pImg->rda.fields.chout1DynamicDriverEn
                        = pParams->chSup[ch].dynDriverEn;
                } else {
                    sysLog("Bad value (channel mode  %d), channel %u",
                            pParams->chSup[ch].chMode, ch);
                    return ERROR;
                }

                /* Driver Impedance */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                  if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_NONE) {
                      pImg->rda.fields.chout1DriverImp = HMC7044_DRV_IMP_NONE;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_100) {
                      pImg->rda.fields.chout1DriverImp = HMC7044_DRV_IMP_100;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_50) {
                      pImg->rda.fields.chout1DriverImp = HMC7044_DRV_IMP_50;
                  } else {
                      sysLog("Bad value ( Driver Impedance %d), channel %u",
                                    pParams->chSup[ch].cmlTerm, ch);
                      return ERROR;
                  }
                }

                /* Output Mux Selection */
                if (pParams->chSup[ch].outSel == HMC7044_COS_DIVIDER) {
                    pImg->rd9.fields.chout1OutputMuxSel = HMC7044_OMS_DIVIDER;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_ADLY) {
                    pImg->rd9.fields.chout1OutputMuxSel = HMC7044_OMS_DIV_ADLY;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_NEIGHBOR) {
                    pImg->rd9.fields.chout1OutputMuxSel = HMC7044_OMS_DIV_NEIGHBOR;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_FUNDAMENTAL) {
                    pImg->rd9.fields.chout1OutputMuxSel = HMC7044_OMS_FUNDAMENTAL;
                } else {
                    sysLog("Bad value ( Output Mux %d), channel %u",
                           pParams->chSup[ch].outSel, ch);
                     return ERROR;
                }

                if (pParams->chSup[ch].slipQuantumPs == 0) {
                    pImg->rd2.fields.chout1SlipEn = 0x0;
                    pImg->rd2.fields.chout1MultiSlipEn = 0x0;
                } else if (fabs(pParams->chSup[ch].slipQuantumPs - vcoCyclePs)
                                < epsilon ) {
                    pImg->rd2.fields.chout1SlipEn = 0x1;
                    pImg->rd2.fields.chout1MultiSlipEn = 0x0;
                } else if (multiSlipVal > 1) {
                    pImg->rd2.fields.chout1SlipEn = 0x0;
                    pImg->rd2.fields.chout1MultiSlipEn = 0x1;
                    pImg->rd7.fields.chout1LsbmultiSlipDigtlDelay
                        = HMC7044_LSB(multiSlipVal);
                    pImg->rd8.fields.chout1MsbMultiSlipDigtlDelay
                        = HMC7044_MSB(multiSlipVal);
                } else {
                   sysLogFpa("Bad value ( slipQuantumPs  %0.0f), channel %f",
                            pParams->chSup[ch].slipQuantumPs, (double) ch);
                   return ERROR;
                }

                pImg->rd5.fields.chout1FineAnlgDelay = anlgDelayVal;
                pImg->rd6.fields.chout1CoarseDigtlDelay = digDelayVal;
                pImg->rd2.fields.chout1ChannelEn = 0x1;

                break;
              case 2:
                pImg->rdc.fields.chout2HighPerfMode
                    = pParams->chSup[ch].highPerfMode;
                pImg->rdc.fields.chout2SyncEn = 0x1;

                /* Configure start-up mode */
                if (pParams->chSup[ch].dynDriverEn)
                    pImg->rdc.fields.chout2StMode = HMC7044_STMOD_DYN;
                else
                    pImg->rdc.fields.chout2StMode = HMC7044_STMOD_ASYNC;

                pImg->rdd.fields.chout2LsbChannelDiv = HMC7044_LSB(chDivider);
                pImg->rde.fields.chout2MsbChannelDiv = HMC7044_MSB(chDivider);

                /* Configure driver mode */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                        pImg->re4.fields.chout2DriverMode = HMC7044_CH_DM_CML;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVPECL) {
                        pImg->re4.fields.chout2DriverMode = HMC7044_CH_DM_LVPECL;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVDS) {
                        pImg->re4.fields.chout2DriverMode = HMC7044_CH_DM_LVDS;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_CMOS) {
                        pImg->re4.fields.chout2DriverMode = HMC7044_CH_DM_CMOS;
                } else {
                    sysLog("Bad value (driver mode %d), channel %u",
                           pParams->chSup[ch].drvMode, ch);
                    return ERROR;
                }

                /* Configure Force Mute */
                if (pParams->chSup[ch].chMode == HMC7044_CHM_CLK) {
                    pImg->re4.fields.chout2ForceMute = HMC7044_FORCE_MUTE_NORMAL;
                } else if (pParams->chSup[ch].chMode == HMC7044_CHM_SYSREF) {
                    pImg->re4.fields.chout2ForceMute = HMC7044_FORCE_MUTE_LOGIC0;
                    pImg->re4.fields.chout2DynamicDriverEn
                        = pParams->chSup[ch].dynDriverEn;
                } else {
                    sysLog("Bad value (channel mode  %d), channel %u",
                                               pParams->chSup[ch].chMode, ch);
                    return ERROR;
                }

                /* Driver Impedance */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                  if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_NONE) {
                      pImg->re4.fields.chout2DriverImp = HMC7044_DRV_IMP_NONE;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_100) {
                      pImg->re4.fields.chout2DriverImp = HMC7044_DRV_IMP_100;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_50) {
                      pImg->re4.fields.chout2DriverImp = HMC7044_DRV_IMP_50;
                  } else {
                      sysLog("Bad value (driver impedance %d), channel %u",
                             pParams->chSup[ch].cmlTerm, ch);
                      return ERROR;
                  }
                }

                /* Output Mux Selection */
                if (pParams->chSup[ch].outSel == HMC7044_COS_DIVIDER) {
                    pImg->re3.fields.chout2OutputMuxSel = HMC7044_OMS_DIVIDER;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_ADLY) {
                    pImg->re3.fields.chout2OutputMuxSel = HMC7044_OMS_DIV_ADLY;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_NEIGHBOR) {
                    pImg->re3.fields.chout2OutputMuxSel = HMC7044_OMS_DIV_NEIGHBOR;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_FUNDAMENTAL) {
                    pImg->re3.fields.chout2OutputMuxSel = HMC7044_OMS_FUNDAMENTAL;
                } else {
                    sysLog("Bad value (Output mux %d), channel %u",
                           pParams->chSup[ch].outSel, ch);
                    return ERROR;
                }

                if (pParams->chSup[ch].slipQuantumPs == 0) {
                    pImg->rdc.fields.chout2SlipEn = 0x0;
                    pImg->rdc.fields.chout2MultiSlipEn = 0x0;
                } else if (fabs(pParams->chSup[ch].slipQuantumPs - vcoCyclePs)
                                < epsilon ) {
                    pImg->rdc.fields.chout2SlipEn = 0x1;
                    pImg->rdc.fields.chout2MultiSlipEn = 0x0;
                } else if (multiSlipVal > 1) {
                    pImg->rdc.fields.chout2SlipEn = 0x0;
                    pImg->rdc.fields.chout2MultiSlipEn = 0x1;
                    pImg->re1.fields.chout2LsbmultiSlipDigtlDelay
                        = HMC7044_LSB(multiSlipVal);
                    pImg->re2.fields.chout2MsbMultiSlipDigtlDelay
                        = HMC7044_MSB(multiSlipVal);
                } else {
                    sysLogFpa("Bad value ( slipQuantumPs  %0.0f), channel %f",
                             pParams->chSup[ch].slipQuantumPs, (double) ch);
                    return ERROR;
                 }

                pImg->rdf.fields.chout2FineAnlgDelay = anlgDelayVal;
                pImg->re0.fields.chout2CoarseDigtlDelay = digDelayVal;

                pImg->rdc.fields.chout2ChannelEn = 0x1;
                break;
              case 3:
                pImg->re6.fields.chout3HighPerfMode
                    = pParams->chSup[ch].highPerfMode;
                pImg->re6.fields.chout3SyncEn = 0x1;

                /* Configure start-up mode */
                if (pParams->chSup[ch].dynDriverEn)
                    pImg->re6.fields.chout3StMode = HMC7044_STMOD_DYN;
                else
                    pImg->re6.fields.chout3StMode = HMC7044_STMOD_ASYNC;

                pImg->re7.fields.chout3LsbChannelDiv = HMC7044_LSB(chDivider);
                pImg->re8.fields.chout3MsbChannelDiv = HMC7044_MSB(chDivider);

                /* Configure driver mode */
                 if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                         pImg->ree.fields.chout3DriverMode = HMC7044_CH_DM_CML;
                 } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVPECL) {
                         pImg->ree.fields.chout3DriverMode = HMC7044_CH_DM_LVPECL;
                 } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVDS) {
                         pImg->ree.fields.chout3DriverMode = HMC7044_CH_DM_LVDS;
                 } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_CMOS) {
                         pImg->ree.fields.chout3DriverMode = HMC7044_CH_DM_CMOS;
                 } else {
                     sysLog("Bad value (driver mode %d), channel %u",
                               pParams->chSup[ch].drvMode, ch);
                     return ERROR;
                 }

                /* Configure Force Mute */
                if (pParams->chSup[ch].chMode == HMC7044_CHM_CLK) {
                    pImg->ree.fields.chout3ForceMute = HMC7044_FORCE_MUTE_NORMAL;
                } else if (pParams->chSup[ch].chMode == HMC7044_CHM_SYSREF) {
                    pImg->ree.fields.chout3ForceMute = HMC7044_FORCE_MUTE_LOGIC0;
                    pImg->ree.fields.chout3DynamicDriverEn
                        = pParams->chSup[ch].dynDriverEn;
                } else {
                    sysLog("Bad value (channel mode  %d), channel %u",
                                               pParams->chSup[ch].chMode, ch);
                    return ERROR;
                }

                /* Driver Impedance */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                  if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_NONE) {
                    pImg->ree.fields.chout3DriverImp = HMC7044_DRV_IMP_NONE;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_100) {
                    pImg->ree.fields.chout3DriverImp = HMC7044_DRV_IMP_100;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_50) {
                    pImg->ree.fields.chout3DriverImp = HMC7044_DRV_IMP_50;
                  } else {
                      sysLog("Bad value (driver impedance %d), channel %u",
                                pParams->chSup[ch].cmlTerm, ch);
                      return ERROR;
                  }
                }

                /* Output Mux Selection */
                if (pParams->chSup[ch].outSel == HMC7044_COS_DIVIDER) {
                  pImg->red.fields.chout3OutputMuxSel = HMC7044_OMS_DIVIDER;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_ADLY) {
                  pImg->red.fields.chout3OutputMuxSel = HMC7044_OMS_DIV_ADLY;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_NEIGHBOR) {
                  pImg->red.fields.chout3OutputMuxSel = HMC7044_OMS_DIV_NEIGHBOR;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_FUNDAMENTAL) {
                  pImg->red.fields.chout3OutputMuxSel = HMC7044_OMS_FUNDAMENTAL;
                } else {
                    sysLog("Bad value (Output mux %d), channel %u",
                           pParams->chSup[ch].outSel, ch);
                    return ERROR;
                }

                if (pParams->chSup[ch].slipQuantumPs == 0) {
                    pImg->re6.fields.chout3SlipEn = 0x0;
                    pImg->re6.fields.chout3MultiSlipEn = 0x0;
                } else if (fabs(pParams->chSup[ch].slipQuantumPs - vcoCyclePs)
                                < epsilon ) {
                    pImg->re6.fields.chout3SlipEn = 0x1;
                    pImg->re6.fields.chout3MultiSlipEn = 0x0;
                } else if (multiSlipVal > 1) {
                    pImg->re6.fields.chout3SlipEn = 0x0;
                    pImg->re6.fields.chout3MultiSlipEn = 1;
                    if (pParams->chSup[ch].drvMode == HMC7044_CDM_CMOS) {
                        addtlMultiSlip = chDivider / 2;
                        multiSlipVal += addtlMultiSlip;
                    }
                    pImg->reb.fields.chout3LsbmultiSlipDigtlDelay
                        = HMC7044_LSB(multiSlipVal);
                    pImg->rec.fields.chout3MsbMultiSlipDigtlDelay
                        = HMC7044_MSB(multiSlipVal);
                } else {
                    sysLogFpa("Bad value ( slipQuantumPs  %0.0f), channel %f",
                             pParams->chSup[ch].slipQuantumPs, (double) ch);
                    return ERROR;
                 }

                pImg->re9.fields.chout3FineAnlgDelay = anlgDelayVal;
                pImg->rea.fields.chout3CoarseDigtlDelay = digDelayVal;

                pImg->re6.fields.chout3ChannelEn = 0x1;

                break;
              case 4:
                pImg->rf0.fields.chout4HighPerfMode
                    = pParams->chSup[ch].highPerfMode;
                pImg->rf0.fields.chout4SyncEn = 0x1;

                 /* Configure start-up mode */
                if (pParams->chSup[ch].dynDriverEn)
                    pImg->rf0.fields.chout4StMode = HMC7044_STMOD_DYN;
                else
                    pImg->rf0.fields.chout4StMode = HMC7044_STMOD_ASYNC;

                pImg->rf1.fields.chout4LsbChannelDiv = HMC7044_LSB(chDivider);
                pImg->rf2.fields.chout4MsbChannelDiv = HMC7044_MSB(chDivider);

                /* Configure driver mode */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                        pImg->rf8.fields.chout4DriverMode = HMC7044_CH_DM_CML;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVPECL) {
                        pImg->rf8.fields.chout4DriverMode = HMC7044_CH_DM_LVPECL;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVDS) {
                        pImg->rf8.fields.chout4DriverMode = HMC7044_CH_DM_LVDS;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_CMOS) {
                        pImg->rf8.fields.chout4DriverMode = HMC7044_CH_DM_CMOS;
                } else {
                    sysLog("Bad value (driver mode %d), channel %u",
                            pParams->chSup[ch].drvMode, ch);
                    return ERROR;
                }

                /* Configure Force Mute */
                if (pParams->chSup[ch].chMode == HMC7044_CHM_CLK) {
                    pImg->rf8.fields.chout4ForceMute = HMC7044_FORCE_MUTE_NORMAL;
                } else if (pParams->chSup[ch].chMode == HMC7044_CHM_SYSREF) {
                    pImg->rf8.fields.chout4ForceMute = HMC7044_FORCE_MUTE_LOGIC0;
                    pImg->rf8.fields.chout4DynamicDriverEn
                        = pParams->chSup[ch].dynDriverEn;
                } else {
                    sysLog("Bad value (channel mode  %d), channel %u",
                                               pParams->chSup[ch].chMode, ch);
                    return ERROR;
                }

                /* Driver Impedance */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                  if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_NONE) {
                      pImg->rf8.fields.chout4DriverImp = HMC7044_DRV_IMP_NONE;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_100) {
                      pImg->rf8.fields.chout4DriverImp = HMC7044_DRV_IMP_100;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_50) {
                      pImg->rf8.fields.chout4DriverImp = HMC7044_DRV_IMP_50;
                  } else {
                      sysLog("Bad value (driver impedance %d), channel %u",
                              pParams->chSup[ch].cmlTerm, ch);
                      return ERROR;
                  }
                }

                /* Output Mux Selection */
                if (pParams->chSup[ch].outSel == HMC7044_COS_DIVIDER) {
                    pImg->rf7.fields.chout4OutputMuxSel = HMC7044_OMS_DIVIDER;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_ADLY) {
                    pImg->rf7.fields.chout4OutputMuxSel = HMC7044_OMS_DIV_ADLY;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_NEIGHBOR) {
                    pImg->rf7.fields.chout4OutputMuxSel = HMC7044_OMS_DIV_NEIGHBOR;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_FUNDAMENTAL) {
                    pImg->rf7.fields.chout4OutputMuxSel = HMC7044_OMS_FUNDAMENTAL;
                } else {
                    sysLog("Bad value (Output mux %d), channel %u",
                           pParams->chSup[ch].outSel, ch);
                    return ERROR;
                }

                if (pParams->chSup[ch].slipQuantumPs == 0) {
                    pImg->rf0.fields.chout4SlipEn = 0x0;
                    pImg->rf0.fields.chout4MultiSlipEn = 0x0;
                } else if (fabs(pParams->chSup[ch].slipQuantumPs - vcoCyclePs)
                                < epsilon ) {
                    pImg->rf0.fields.chout4SlipEn = 0x1;
                    pImg->rf0.fields.chout4MultiSlipEn = 0x0;
                } else if (multiSlipVal > 1) {
                    pImg->rf0.fields.chout4SlipEn = 0x0;
                    pImg->rf0.fields.chout4MultiSlipEn = 0x1;
                    pImg->rf5.fields.chout4LsbmultiSlipDigtlDelay =
                              HMC7044_LSB(multiSlipVal);
                    pImg->rf6.fields.chout4MsbMultiSlipDigtlDelay =
                              HMC7044_MSB(multiSlipVal);
                } else {
                    sysLogFpa("Bad value ( slipQuantumPs  %0.0f), channel %f",
                             pParams->chSup[ch].slipQuantumPs, (double) ch);
                    return ERROR;
                 }

                pImg->rf3.fields.chout4FineAnlgDelay = anlgDelayVal;
                pImg->rf4.fields.chout4CoarseDigtlDelay = digDelayVal;

                pImg->rf0.fields.chout4ChannelEn = 0x1;
                break;
              case 5:
                pImg->rfa.fields.chout5HighPerfMode
                    = pParams->chSup[ch].highPerfMode;
                pImg->rfa.fields.chout5SyncEn = 0x1;

                /* Configure start-up mode */
                if (pParams->chSup[ch].dynDriverEn)
                    pImg->rfa.fields.chout5StMode = HMC7044_STMOD_DYN;
                else
                    pImg->rfa.fields.chout5StMode = HMC7044_STMOD_ASYNC;

                pImg->rfb.fields.chout5LsbChannelDiv = HMC7044_LSB(chDivider);
                pImg->rfc.fields.chout5MsbChannelDiv = HMC7044_MSB(chDivider);

                /* Configure driver mode */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                        pImg->r102.fields.chout5DriverMode = HMC7044_CH_DM_CML;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVPECL) {
                        pImg->r102.fields.chout5DriverMode = HMC7044_CH_DM_LVPECL;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVDS) {
                        pImg->r102.fields.chout5DriverMode = HMC7044_CH_DM_LVDS;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_CMOS) {
                        pImg->r102.fields.chout5DriverMode = HMC7044_CH_DM_CMOS;
                } else {
                    sysLog("Bad value (driver mode %d), channel %u",
                           pParams->chSup[ch].drvMode, ch);
                    return ERROR;
                 }

                /* Configure Force Mute */
                if (pParams->chSup[ch].chMode == HMC7044_CHM_CLK) {
                    pImg->r102.fields.chout5ForceMute = HMC7044_FORCE_MUTE_NORMAL;
                } else if (pParams->chSup[ch].chMode == HMC7044_CHM_SYSREF) {
                    pImg->r102.fields.chout5ForceMute = HMC7044_FORCE_MUTE_LOGIC0;
                    pImg->r102.fields.chout5DynamicDriverEn
                        = pParams->chSup[ch].dynDriverEn;
                } else {
                    sysLog("Bad value (channel mode  %d), channel %u",
                                               pParams->chSup[ch].chMode, ch);
                    return ERROR;
                }

                /* Driver Impedance */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                  if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_NONE) {
                      pImg->r102.fields.chout5DriverImp = HMC7044_DRV_IMP_NONE;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_100) {
                      pImg->r102.fields.chout5DriverImp = HMC7044_DRV_IMP_100;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_50) {
                      pImg->r102.fields.chout5DriverImp = HMC7044_DRV_IMP_50;
                  } else {
                      sysLog("Bad value (driver impedance %d), channel %u",
                             pParams->chSup[ch].cmlTerm, ch);
                      return ERROR;
                   }
                }

                /* Output Mux Selection */
                if (pParams->chSup[ch].outSel == HMC7044_COS_DIVIDER) {
                    pImg->r101.fields.chout5OutputMuxSel = HMC7044_OMS_DIVIDER;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_ADLY) {
                    pImg->r101.fields.chout5OutputMuxSel = HMC7044_OMS_DIV_ADLY;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_NEIGHBOR) {
                    pImg->r101.fields.chout5OutputMuxSel = HMC7044_OMS_DIV_NEIGHBOR;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_FUNDAMENTAL) {
                    pImg->r101.fields.chout5OutputMuxSel = HMC7044_OMS_FUNDAMENTAL;
                } else {
                    sysLog("Bad value (Output Mux %d), channel %u",
                           pParams->chSup[ch].outSel, ch);
                    return ERROR;
                 }

                if (pParams->chSup[ch].slipQuantumPs == 0) {
                    pImg->rfa.fields.chout5SlipEn = 0x0;
                    pImg->rfa.fields.chout5MultiSlipEn = 0x0;
                } else if (fabs(pParams->chSup[ch].slipQuantumPs - vcoCyclePs)
                                < epsilon ) {
                    pImg->rfa.fields.chout5SlipEn = 0x1;
                    pImg->rfa.fields.chout5MultiSlipEn = 0x0;
                } else if (multiSlipVal > 1) {
                    pImg->rfa.fields.chout5SlipEn = 0x0;
                    pImg->rfa.fields.chout5MultiSlipEn = 1;
                    if (pParams->chSup[ch].drvMode == HMC7044_CDM_CMOS) {
                        addtlMultiSlip = chDivider / 2;
                        multiSlipVal += addtlMultiSlip;
                    }
                    pImg->rff.fields.chout5LsbmultiSlipDigtlDelay
                        = HMC7044_LSB(multiSlipVal);
                    pImg->r100.fields.chout5MsbMultiSlipDigtlDelay
                        = HMC7044_MSB(multiSlipVal);
                } else {
                    sysLogFpa("Bad value ( slipQuantumPs  %0.0f), channel %f",
                             pParams->chSup[ch].slipQuantumPs, (double) ch);
                    return ERROR;
                 }

                pImg->rfd.fields.chout5FineAnlgDelay = anlgDelayVal;
                pImg->rfe.fields.chout5CoarseDigtlDelay = digDelayVal;
                pImg->rfa.fields.chout5ChannelEn = 0x1;

                break;
              case 6:
                pImg->r104.fields.chout6HighPerfMode
                             = pParams->chSup[ch].highPerfMode;
                pImg->r104.fields.chout6SyncEn = 0x1;

                /* Configure start-up mode */
                if (pParams->chSup[ch].dynDriverEn)
                    pImg->r104.fields.chout6StMode = HMC7044_STMOD_DYN;
                else
                    pImg->r104.fields.chout6StMode = HMC7044_STMOD_ASYNC;

                pImg->r105.fields.chout6LsbChannelDiv = HMC7044_LSB(chDivider);
                pImg->r106.fields.chout6MsbChannelDiv = HMC7044_MSB(chDivider);

                /* Configure driver mode */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                        pImg->r10c.fields.chout6DriverMode = HMC7044_CH_DM_CML;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVPECL) {
                        pImg->r10c.fields.chout6DriverMode = HMC7044_CH_DM_LVPECL;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVDS) {
                        pImg->r10c.fields.chout6DriverMode = HMC7044_CH_DM_LVDS;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_CMOS) {
                        pImg->r10c.fields.chout6DriverMode = HMC7044_CH_DM_CMOS;
                } else {
                    sysLog("Bad value ( driver mode %d), channel %u",
                            pParams->chSup[ch].drvMode, ch);
                    return ERROR;
                }

                /* Configure Force Mute */
                if (pParams->chSup[ch].chMode == HMC7044_CHM_CLK) {
                    pImg->r10c.fields.chout6ForceMute = HMC7044_FORCE_MUTE_NORMAL;
                } else if (pParams->chSup[ch].chMode == HMC7044_CHM_SYSREF) {
                    pImg->r10c.fields.chout6ForceMute = HMC7044_FORCE_MUTE_LOGIC0;
                    pImg->r10c.fields.chout6DynamicDriverEn
                                   = pParams->chSup[ch].dynDriverEn;
                } else {
                    sysLog("Bad value (channel mode  %d), channel %u",
                                               pParams->chSup[ch].chMode, ch);
                    return ERROR;
                }

                /* Driver Impedance */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                  if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_NONE) {
                      pImg->r10c.fields.chout6DriverImp = HMC7044_DRV_IMP_NONE;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_100) {
                      pImg->r10c.fields.chout6DriverImp = HMC7044_DRV_IMP_100;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_50) {
                      pImg->r10c.fields.chout6DriverImp = HMC7044_DRV_IMP_50;
                  } else {
                      sysLog("Bad value ( driver impedance %d), channel %u",
                               pParams->chSup[ch].cmlTerm, ch);
                      return ERROR;
                  }
                }

                /* Output Mux Selection */
                if (pParams->chSup[ch].outSel == HMC7044_COS_DIVIDER) {
                  pImg->r10b.fields.chout6OutputMuxSel = HMC7044_OMS_DIVIDER;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_ADLY) {
                  pImg->r10b.fields.chout6OutputMuxSel = HMC7044_OMS_DIV_ADLY;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_NEIGHBOR) {
                  pImg->r10b.fields.chout6OutputMuxSel = HMC7044_OMS_DIV_NEIGHBOR;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_FUNDAMENTAL) {
                  pImg->r10b.fields.chout6OutputMuxSel = HMC7044_OMS_FUNDAMENTAL;
                } else {
                    sysLog("Bad value ( Output Mux %d), channel %u",
                           pParams->chSup[ch].outSel, ch);
                    return ERROR;
                }

                if (pParams->chSup[ch].slipQuantumPs == 0) {
                    pImg->r104.fields.chout6SlipEn = 0x0;
                    pImg->r104.fields.chout6MultiSlipEn = 0x0;
                } else if (fabs(pParams->chSup[ch].slipQuantumPs - vcoCyclePs)
                                < epsilon ) {
                    pImg->r104.fields.chout6SlipEn = 0x1;
                    pImg->r104.fields.chout6MultiSlipEn = 0x0;
                } else if (multiSlipVal > 1) {
                    pImg->r104.fields.chout6SlipEn = 0x0;
                    pImg->r104.fields.chout6MultiSlipEn = 0x1;
                    multiSlipVal = pParams->chSup[ch].slipQuantumPs / vcoCyclePs;
                    if (pParams->chSup[ch].drvMode == HMC7044_CDM_CMOS) {
                        addtlMultiSlip = chDivider / 2;
                        multiSlipVal += addtlMultiSlip;
                    }
                    pImg->r109.fields.chout6LsbmultiSlipDigtlDelay
                            = HMC7044_LSB(multiSlipVal);
                    pImg->r10a.fields.chout6MsbMultiSlipDigtlDelay
                            = HMC7044_MSB(multiSlipVal);
                } else {
                    sysLogFpa("Bad value ( slipQuantumPs  %0.0f), channel %f",
                             pParams->chSup[ch].slipQuantumPs, (double) ch);
                    return ERROR;
                 }

                pImg->r107.fields.chout6FineAnlgDelay = anlgDelayVal;
                pImg->r108.fields.chout6CoarseDigtlDelay = digDelayVal;
                pImg->r104.fields.chout6ChannelEn = 0x1;

                break;
              case 7:
                pImg->r10e.fields.chout7HighPerfMode
                    = pParams->chSup[ch].highPerfMode;
                pImg->r10e.fields.chout7SyncEn = 0x1;

                /* Configure start-up mode */
                if (pParams->chSup[ch].dynDriverEn)
                    pImg->r10e.fields.chout7StMode = HMC7044_STMOD_DYN;
                else
                    pImg->r10e.fields.chout7StMode = HMC7044_STMOD_ASYNC;

                pImg->r10f.fields.chout7LsbChannelDiv = HMC7044_LSB(chDivider);
                pImg->r110.fields.chout7MsbChannelDiv = HMC7044_MSB(chDivider);

                /* Configure driver mode */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                    pImg->r116.fields.chout7DriverMode = HMC7044_CH_DM_CML;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVPECL) {
                    pImg->r116.fields.chout7DriverMode = HMC7044_CH_DM_LVPECL;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVDS) {
                    pImg->r116.fields.chout7DriverMode = HMC7044_CH_DM_LVDS;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_CMOS) {
                    pImg->r116.fields.chout7DriverMode = HMC7044_CH_DM_CMOS;
                } else {
                    sysLog("Bad value ( driver mode %d), channel %u",
                            pParams->chSup[ch].drvMode, ch);
                    return ERROR;
                }

                /* Configure Force Mute */
                if (pParams->chSup[ch].chMode == HMC7044_CHM_CLK) {
                    pImg->r116.fields.chout7ForceMute = HMC7044_FORCE_MUTE_NORMAL;
                } else if (pParams->chSup[ch].chMode == HMC7044_CHM_SYSREF) {
                    pImg->r116.fields.chout7ForceMute = HMC7044_FORCE_MUTE_LOGIC0;
                    pImg->r116.fields.chout7DynamicDriverEn
                        = pParams->chSup[ch].dynDriverEn;
                } else {
                    sysLog("Bad value (channel mode  %d), channel %u",
                                               pParams->chSup[ch].chMode, ch);
                    return ERROR;
                }

                /* Driver Impedance */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                  if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_NONE) {
                    pImg->r116.fields.chout7DriverImp = HMC7044_DRV_IMP_NONE;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_100) {
                    pImg->r116.fields.chout7DriverImp = HMC7044_DRV_IMP_100;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_50) {
                    pImg->r116.fields.chout7DriverImp = HMC7044_DRV_IMP_50;
                  }  else {
                      sysLog("Bad value ( driver impedance %d), channel %u",
                             pParams->chSup[ch].cmlTerm, ch);
                      return ERROR;
                  }
                }
                /* Output Mux Selection */
                if (pParams->chSup[ch].outSel == HMC7044_COS_DIVIDER) {
                  pImg->r115.fields.chout7OutputMuxSel = HMC7044_OMS_DIVIDER;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_ADLY) {
                  pImg->r115.fields.chout7OutputMuxSel = HMC7044_OMS_DIV_ADLY;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_NEIGHBOR) {
                  pImg->r115.fields.chout7OutputMuxSel = HMC7044_OMS_DIV_NEIGHBOR;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_FUNDAMENTAL) {
                  pImg->r115.fields.chout7OutputMuxSel = HMC7044_OMS_FUNDAMENTAL;
                } else {
                    sysLog("Bad value ( Output Mux %d), channel %u",
                           pParams->chSup[ch].outSel, ch);
                    return ERROR;
                }

                if (pParams->chSup[ch].slipQuantumPs == 0) {
                    pImg->r10e.fields.chout7SlipEn = 0x0;
                    pImg->r10e.fields.chout7MultiSlipEn = 0x0;
                } else if (fabs(pParams->chSup[ch].slipQuantumPs - vcoCyclePs)
                                < epsilon ) {
                    pImg->r10e.fields.chout7SlipEn = 0x1;
                    pImg->r10e.fields.chout7MultiSlipEn = 0x0;
                } else if (multiSlipVal > 1) {
                    pImg->r10e.fields.chout7SlipEn = 0x0;
                    pImg->r10e.fields.chout7MultiSlipEn = 0x1;
                    pImg->r113.fields.chout7LsbmultiSlipDigtlDelay
                             = HMC7044_LSB(multiSlipVal);
                    pImg->r114.fields.chout7MsbMultiSlipDigtlDelay
                             = HMC7044_MSB(multiSlipVal);
                } else {
                    sysLogFpa("Bad value ( slipQuantumPs  %0.0f), channel %f",
                             pParams->chSup[ch].slipQuantumPs, (double) ch);
                    return ERROR;
                 }

                pImg->r111.fields.chout7FineAnlgDelay = anlgDelayVal;
                pImg->r112.fields.chout7CoarseDigtlDelay = digDelayVal;

                pImg->r10e.fields.chout7ChannelEn = 0x1;

                break;
              case 8:
                pImg->r118.fields.chout8HighPerfMode
                          = pParams->chSup[ch].highPerfMode;
                pImg->r118.fields.chout8SyncEn = 0x1;

                /* Configure start-up mode */
               if (pParams->chSup[ch].dynDriverEn)
                   pImg->r118.fields.chout8StMode = HMC7044_STMOD_DYN;
               else
                   pImg->r118.fields.chout8StMode = HMC7044_STMOD_ASYNC;

                pImg->r119.fields.chout8LsbChannelDiv = HMC7044_LSB(chDivider);
                pImg->r11a.fields.chout8MsbChannelDiv = HMC7044_MSB(chDivider);

                /* Configure driver mode */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                        pImg->r120.fields.chout8DriverMode = HMC7044_CH_DM_CML;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVPECL) {
                        pImg->r120.fields.chout8DriverMode = HMC7044_CH_DM_LVPECL;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVDS) {
                        pImg->r120.fields.chout8DriverMode = HMC7044_CH_DM_LVDS;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_CMOS) {
                        pImg->r120.fields.chout8DriverMode = HMC7044_CH_DM_CMOS;
                } else {
                    sysLog("Bad value ( driver mode %d), channel %u",
                             pParams->chSup[ch].drvMode, ch);
                    return ERROR;
                }

                /* Configure Force Mute */
                if (pParams->chSup[ch].chMode == HMC7044_CHM_CLK) {
                    pImg->r120.fields.chout8ForceMute = HMC7044_FORCE_MUTE_NORMAL;
                } else if (pParams->chSup[ch].chMode == HMC7044_CHM_SYSREF) {
                    pImg->r120.fields.chout8ForceMute = HMC7044_FORCE_MUTE_LOGIC0;
                    pImg->r120.fields.chout8DynamicDriverEn
                              = pParams->chSup[ch].dynDriverEn;
                } else {
                    sysLog("Bad value (channel mode  %d), channel %u",
                                               pParams->chSup[ch].chMode, ch);
                    return ERROR;
                }

                /* Driver Impedance */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                  if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_NONE) {
                    pImg->r120.fields.chout8DriverImp = HMC7044_DRV_IMP_NONE;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_100) {
                    pImg->r120.fields.chout8DriverImp = HMC7044_DRV_IMP_100;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_50) {
                      pImg->r120.fields.chout8DriverImp = HMC7044_DRV_IMP_50;
                  } else {
                      sysLog("Bad value ( driver impedance %d), channel %u",
                               pParams->chSup[ch].cmlTerm, ch);
                      return ERROR;
                  }
                }

                /* Output Mux Selection */
                if (pParams->chSup[ch].outSel == HMC7044_COS_DIVIDER) {
                  pImg->r11f.fields.chout8OutputMuxSel = HMC7044_OMS_DIVIDER;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_ADLY) {
                  pImg->r11f.fields.chout8OutputMuxSel = HMC7044_OMS_DIV_ADLY;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_NEIGHBOR) {
                  pImg->r11f.fields.chout8OutputMuxSel = HMC7044_OMS_DIV_NEIGHBOR;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_FUNDAMENTAL) {
                  pImg->r11f.fields.chout8OutputMuxSel = HMC7044_OMS_FUNDAMENTAL;
                } else {
                    sysLog("Bad value ( Output mux %d), channel %u",
                             pParams->chSup[ch].outSel, ch);
                    return ERROR;
                }

                if (pParams->chSup[ch].slipQuantumPs == 0) {
                    pImg->r118.fields.chout8SlipEn = 0x0;
                    pImg->r118.fields.chout8MultiSlipEn = 0x0;
                } else if (fabs(pParams->chSup[ch].slipQuantumPs - vcoCyclePs)
                                < epsilon ) {
                    pImg->r118.fields.chout8SlipEn = 0x1;
                    pImg->r118.fields.chout8MultiSlipEn = 0x0;
                } else if (multiSlipVal > 1) {
                    pImg->r118.fields.chout8SlipEn = 0x0;
                    pImg->r118.fields.chout8MultiSlipEn = 0x01;
                    pImg->r11d.fields.chout8LsbmultiSlipDigtlDelay =
                        HMC7044_LSB(multiSlipVal);
                    pImg->r11e.fields.chout8MsbMultiSlipDigtlDelay =
                        HMC7044_MSB(multiSlipVal);
                } else {
                    sysLogFpa("Bad value ( slipQuantumPs  %0.0f), channel %f",
                             pParams->chSup[ch].slipQuantumPs, (double) ch);
                    return ERROR;
                 }

                pImg->r11b.fields.chout8FineAnlgDelay = anlgDelayVal;
                pImg->r11c.fields.chout8CoarseDigtlDelay = digDelayVal;

                pImg->r118.fields.chout8ChannelEn = 0x1;

                break;

              case 9:
                pImg->r122.fields.chout9HighPerfMode
                          = pParams->chSup[ch].highPerfMode;
                pImg->r122.fields.chout9SyncEn = 0x1;

                /* Configure start-up mode */
               if (pParams->chSup[ch].dynDriverEn)
                   pImg->r122.fields.chout9StMode = HMC7044_STMOD_DYN;
               else
                   pImg->r122.fields.chout9StMode = HMC7044_STMOD_ASYNC;

                pImg->r123.fields.chout9LsbChannelDiv = HMC7044_LSB(chDivider);
                pImg->r124.fields.chout9MsbChannelDiv = HMC7044_MSB(chDivider);

                /* Configure driver mode */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                        pImg->r12a.fields.chout9DriverMode = HMC7044_CH_DM_CML;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVPECL) {
                        pImg->r12a.fields.chout9DriverMode = HMC7044_CH_DM_LVPECL;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVDS) {
                        pImg->r12a.fields.chout9DriverMode = HMC7044_CH_DM_LVDS;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_CMOS) {
                        pImg->r12a.fields.chout9DriverMode = HMC7044_CH_DM_CMOS;
                } else {
                    sysLog("Bad value ( driver mode %d), channel %u",
                             pParams->chSup[ch].drvMode, ch);
                    return ERROR;
                }


                /* Configure Force Mute */
                if (pParams->chSup[ch].chMode == HMC7044_CHM_CLK) {
                    pImg->r12a.fields.chout9ForceMute = HMC7044_FORCE_MUTE_NORMAL;
                } else if (pParams->chSup[ch].chMode == HMC7044_CHM_SYSREF) {
                    pImg->r12a.fields.chout9ForceMute = HMC7044_FORCE_MUTE_LOGIC0;
                    pImg->r12a.fields.chout9DynamicDriverEn
                               = pParams->chSup[ch].dynDriverEn;
                } else {
                    sysLog("Bad value (channel mode  %d), channel %u",
                                               pParams->chSup[ch].chMode, ch);
                    return ERROR;
                }

                /* Driver Impedance */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                  if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_NONE) {
                     pImg->r12a.fields.chout9DriverImp = HMC7044_DRV_IMP_NONE;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_100) {
                    pImg->r12a.fields.chout9DriverImp = HMC7044_DRV_IMP_100;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_50) {
                    pImg->r12a.fields.chout9DriverImp = HMC7044_DRV_IMP_50;
                  } else {
                      sysLog("Bad value (driver impedance %d), channel %u",
                               pParams->chSup[ch].cmlTerm, ch);
                      return ERROR;
                  }
                }

                /* Output Mux Selection */
                if (pParams->chSup[ch].outSel == HMC7044_COS_DIVIDER) {
                  pImg->r129.fields.chout9OutputMuxSel = HMC7044_OMS_DIVIDER;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_ADLY) {
                  pImg->r129.fields.chout9OutputMuxSel = HMC7044_OMS_DIV_ADLY;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_NEIGHBOR) {
                  pImg->r129.fields.chout9OutputMuxSel = HMC7044_OMS_DIV_NEIGHBOR;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_FUNDAMENTAL) {
                  pImg->r129.fields.chout9OutputMuxSel = HMC7044_OMS_FUNDAMENTAL;
                } else {
                    sysLog("Bad value ( Output Mux %d), channel %u",
                           pParams->chSup[ch].outSel, ch);
                    return ERROR;
                }

                if (pParams->chSup[ch].slipQuantumPs == 0) {
                    pImg->r122.fields.chout9SlipEn = 0x0;
                    pImg->r122.fields.chout9MultiSlipEn = 0x0;
                } else if (fabs(pParams->chSup[ch].slipQuantumPs - vcoCyclePs)
                                < epsilon ) {
                    pImg->r122.fields.chout9SlipEn = 0x1;
                    pImg->r122.fields.chout9MultiSlipEn = 0x0;
                } else if (multiSlipVal > 1) {
                    pImg->r122.fields.chout9SlipEn = 0x0;
                    pImg->r122.fields.chout9MultiSlipEn = 1;
                    if (pParams->chSup[ch].drvMode == HMC7044_CDM_CMOS) {
                        addtlMultiSlip = chDivider / 2;
                        multiSlipVal += addtlMultiSlip;
                    }
                    pImg->r127.fields.chout9LsbmultiSlipDigtlDelay
                        = HMC7044_LSB(multiSlipVal);
                    pImg->r128.fields.chout9MsbMultiSlipDigtlDelay
                        = HMC7044_MSB(multiSlipVal);
                } else {
                    sysLogFpa("Bad value ( slipQuantumPs  %0.0f), channel %f",
                             pParams->chSup[ch].slipQuantumPs, (double) ch);
                    return ERROR;
                 }

                pImg->r125.fields.chout9FineAnlgDelay = anlgDelayVal;
                pImg->r126.fields.chout9CoarseDigtlDelay = digDelayVal;

                pImg->r122.fields.chout9ChannelEn = 0x1;

                break;
              case 10:
                pImg->r12c.fields.chout10HighPerfMode
                    = pParams->chSup[ch].highPerfMode;
                pImg->r12c.fields.chout10SyncEn = 0x1;

                /* Configure start-up mode */
                if (pParams->chSup[ch].dynDriverEn)
                    pImg->r12c.fields.chout10StMode  = HMC7044_STMOD_DYN;
                else
                    pImg->r12c.fields.chout10StMode  = HMC7044_STMOD_ASYNC;

                pImg->r12d.fields.chout10LsbChannelDiv = HMC7044_LSB(chDivider);
                pImg->r12e.fields.chout10MsbChannelDiv = HMC7044_MSB(chDivider);

                /* Configure driver mode */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                        pImg->r134.fields.chout10DriverMode = HMC7044_CH_DM_CML;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVPECL) {
                        pImg->r134.fields.chout10DriverMode = HMC7044_CH_DM_LVPECL;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVDS) {
                        pImg->r134.fields.chout10DriverMode = HMC7044_CH_DM_LVDS;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_CMOS) {
                        pImg->r134.fields.chout10DriverMode = HMC7044_CH_DM_CMOS;
                } else {
                    sysLog("Bad value ( driver mode %d), channel %u",
                                     pParams->chSup[ch].drvMode, ch);
                    return ERROR;
                }

                /* Configure Force Mute */
                if (pParams->chSup[ch].chMode == HMC7044_CHM_CLK) {
                    pImg->r134.fields.chout10ForceMute = HMC7044_FORCE_MUTE_NORMAL;
                } else if (pParams->chSup[ch].chMode == HMC7044_CHM_SYSREF) {
                    pImg->r134.fields.chout10ForceMute = HMC7044_FORCE_MUTE_LOGIC0;
                    pImg->r134.fields.chout10DynamicDriverEn
                        = pParams->chSup[ch].dynDriverEn;
                } else {
                    sysLog("Bad value (channel mode  %d), channel %u",
                                               pParams->chSup[ch].chMode, ch);
                    return ERROR;
                }

                /* Driver Impedance */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                  if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_NONE) {
                    pImg->r134.fields.chout10DriverImp = HMC7044_DRV_IMP_NONE;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_100) {
                    pImg->r134.fields.chout10DriverImp = HMC7044_DRV_IMP_100;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_50) {
                    pImg->r134.fields.chout10DriverImp = HMC7044_DRV_IMP_50;
                  } else {
                      sysLog("Bad value ( driver impedance %d), channel %u",
                                       pParams->chSup[ch].cmlTerm, ch);
                      return ERROR;
                  }
                }

                /* Output Mux Selection */
                if (pParams->chSup[ch].outSel == HMC7044_COS_DIVIDER) {
                  pImg->r133.fields.chout10OutputMuxSel = HMC7044_OMS_DIVIDER;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_ADLY) {
                  pImg->r133.fields.chout10OutputMuxSel = HMC7044_OMS_DIV_ADLY;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_NEIGHBOR) {
                  pImg->r133.fields.chout10OutputMuxSel = HMC7044_OMS_DIV_NEIGHBOR;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_FUNDAMENTAL) {
                    pImg->r133.fields.chout10OutputMuxSel = HMC7044_OMS_FUNDAMENTAL;
                } else {
                    sysLog("Bad value ( Output Mux %d), channel %u",
                           pParams->chSup[ch].outSel, ch);
                    return ERROR;
                }

                if (pParams->chSup[ch].slipQuantumPs == 0) {
                    pImg->r12c.fields.chout10SlipEn = 0x0;
                    pImg->r12c.fields.chout10MultiSlipEn = 0x0;
                } else if (fabs(pParams->chSup[ch].slipQuantumPs - vcoCyclePs)
                                < epsilon ) {
                    pImg->r12c.fields.chout10SlipEn = 0x1;
                    pImg->r12c.fields.chout10MultiSlipEn = 0x0;
                } else if (multiSlipVal > 1) {
                    pImg->r12c.fields.chout10SlipEn = 0x0;
                    pImg->r12c.fields.chout10MultiSlipEn = 0x1;
                    if (pParams->chSup[ch].drvMode== HMC7044_CDM_CMOS) {
                        addtlMultiSlip = chDivider / 2;
                        multiSlipVal += addtlMultiSlip;
                    }
                    pImg->r131.fields.chout10LsbmultiSlipDigtlDelay
                        = HMC7044_LSB(multiSlipVal);
                    pImg->r132.fields.chout10MsbMultiSlipDigtlDelay
                        = HMC7044_MSB(multiSlipVal);
                } else {
                    sysLogFpa("Bad value ( slipQuantumPs  %0.0f), channel %f",
                             pParams->chSup[ch].slipQuantumPs, (double) ch);
                    return ERROR;
                 }

                pImg->r12f.fields.chout10FineAnlgDelay = anlgDelayVal;
                pImg->r130.fields.chout10CoarseDigtlDelay = digDelayVal;

                pImg->r12c.fields.chout10ChannelEn = 0x1;

                break;
              case 11:
                pImg->r136.fields.chout11HighPerfMode
                                  = pParams->chSup[ch].highPerfMode;
                pImg->r136.fields.chout11SyncEn = 0x1;

                /* Configure start-up mode */
                if (pParams->chSup[ch].dynDriverEn)
                    pImg->r136.fields.chout11StMode = HMC7044_STMOD_DYN;
                else
                    pImg->r136.fields.chout11StMode = HMC7044_STMOD_ASYNC;

                pImg->r137.fields.chout11LsbChannelDiv = HMC7044_LSB(chDivider);
                pImg->r138.fields.chout11MsbChannelDiv = HMC7044_MSB(chDivider);

                /* Configure driver mode */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                    pImg->r13e.fields.chout11DriverMode = HMC7044_CH_DM_CML;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVPECL) {
                    pImg->r13e.fields.chout11DriverMode = HMC7044_CH_DM_LVPECL;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVDS) {
                    pImg->r13e.fields.chout11DriverMode = HMC7044_CH_DM_LVDS;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_CMOS) {
                    pImg->r13e.fields.chout11DriverMode = HMC7044_CH_DM_CMOS;
                } else {
                    sysLog("Bad value ( driver mode %d), channel %u",
                              pParams->chSup[ch].drvMode, ch);
                    return ERROR;
                }

                /* Configure Force Mute */
                if (pParams->chSup[ch].chMode == HMC7044_CHM_CLK) {
                    pImg->r13e.fields.chout11ForceMute = HMC7044_FORCE_MUTE_NORMAL;
                } else if (pParams->chSup[ch].chMode == HMC7044_CHM_SYSREF) {
                    pImg->r13e.fields.chout11ForceMute = HMC7044_FORCE_MUTE_LOGIC0;
                    pImg->r13e.fields.chout11DynamicDriverEn
                        = pParams->chSup[ch].dynDriverEn;
                } else {
                    sysLog("Bad value (channel mode  %d), channel %u",
                                               pParams->chSup[ch].chMode, ch);
                    return ERROR;
                }

                /* Driver Impedance */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                  if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_NONE) {
                    pImg->r13e.fields.chout11DriverImp = HMC7044_DRV_IMP_NONE;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_100) {
                    pImg->r13e.fields.chout11DriverImp = HMC7044_DRV_IMP_100;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_50) {
                    pImg->r13e.fields.chout11DriverImp = HMC7044_DRV_IMP_50;
                  } else {
                      sysLog("Bad value ( driver impedance %d), channel %u",
                                pParams->chSup[ch].cmlTerm, ch);
                      return ERROR;
                  }
                }

                /* Output Mux Selection */
                if (pParams->chSup[ch].outSel == HMC7044_COS_DIVIDER) {
                  pImg->r13d.fields.chout11OutputMuxSel = HMC7044_OMS_DIVIDER;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_ADLY) {
                  pImg->r13d.fields.chout11OutputMuxSel = HMC7044_OMS_DIV_ADLY;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_NEIGHBOR) {
                  pImg->r13d.fields.chout11OutputMuxSel = HMC7044_OMS_DIV_NEIGHBOR;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_FUNDAMENTAL) {
                  pImg->r13d.fields.chout11OutputMuxSel = HMC7044_OMS_FUNDAMENTAL;
                } else {
                    sysLog("Bad value ( Output Mux %d), channel %u",
                           pParams->chSup[ch].outSel, ch);
                    return ERROR;
                }

                if (pParams->chSup[ch].slipQuantumPs == 0) {
                    pImg->r136.fields.chout11SlipEn = 0x0;
                    pImg->r136.fields.chout11MultiSlipEn = 0x0;
                } else if (fabs(pParams->chSup[ch].slipQuantumPs - vcoCyclePs)
                                < epsilon ) {
                    pImg->r136.fields.chout11SlipEn = 0x1;
                    pImg->r136.fields.chout11MultiSlipEn = 0x0;
                } else if (multiSlipVal > 1) {
                    pImg->r136.fields.chout11SlipEn = 0x0;
                    pImg->r136.fields.chout11MultiSlipEn = 0x1;
                    pImg->r13b.fields.chout11LsbmultiSlipDigtlDelay
                        = HMC7044_LSB(multiSlipVal);
                    pImg->r13c.fields.chout11MsbMultiSlipDigtlDelay
                        = HMC7044_MSB(multiSlipVal);
                } else {
                    sysLogFpa("Bad value ( slipQuantumPs  %0.0f), channel %f",
                             pParams->chSup[ch].slipQuantumPs, (double) ch);
                    return ERROR;
                 }

                pImg->r139.fields.chout11FineAnlgDelay = anlgDelayVal;
                pImg->r13a.fields.chout11CoarseDigtlDelay = digDelayVal;

                pImg->r136.fields.chout11ChannelEn = 0x1;

                break;
              case 12:
                pImg->r140.fields.chout12HighPerfMode
                    = pParams->chSup[ch].highPerfMode;
                pImg->r140.fields.chout12SyncEn = 0x1;

                /* Configure start-up mode */
                if (pParams->chSup[ch].dynDriverEn)
                    pImg->r140.fields.chout12StMode  = HMC7044_STMOD_DYN;
                else
                    pImg->r140.fields.chout12StMode  = HMC7044_STMOD_ASYNC;

                pImg->r141.fields.chout12LsbChannelDiv = HMC7044_LSB(chDivider);
                pImg->r142.fields.chout12MsbChannelDiv = HMC7044_MSB(chDivider);

                /* Configure driver mode */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                    pImg->r148.fields.chout12DriverMode = HMC7044_CH_DM_CML;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVPECL) {
                    pImg->r148.fields.chout12DriverMode = HMC7044_CH_DM_LVPECL;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVDS) {
                    pImg->r148.fields.chout12DriverMode = HMC7044_CH_DM_LVDS;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_CMOS) {
                    pImg->r148.fields.chout12DriverMode = HMC7044_CH_DM_CMOS;
                } else {
                    sysLog("Bad value ( driver mode %u), channel %u",
                           pParams->chSup[ch].drvMode, ch);
                    return ERROR;
                }

                /* Configure Force Mute */
                if (pParams->chSup[ch].chMode == HMC7044_CHM_CLK) {
                    pImg->r148.fields.chout12ForceMute = HMC7044_FORCE_MUTE_NORMAL;
                } else if (pParams->chSup[ch].chMode == HMC7044_CHM_SYSREF) {
                    pImg->r148.fields.chout12ForceMute = HMC7044_FORCE_MUTE_LOGIC0;
                    pImg->r148.fields.chout12DynamicDriverEn
                        = pParams->chSup[ch].dynDriverEn;
                } else {
                    sysLog("Bad value (channel mode  %d), channel %u",
                                               pParams->chSup[ch].chMode, ch);
                    return ERROR;
                }

                /* Driver Impedance */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                  if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_NONE) {
                      pImg->r148.fields.chout12DriverImp = HMC7044_DRV_IMP_NONE;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_100) {
                      pImg->r148.fields.chout12DriverImp = HMC7044_DRV_IMP_100;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_50) {
                      pImg->r148.fields.chout12DriverImp = HMC7044_DRV_IMP_50;
                  } else {
                      sysLog("Bad value ( driver impedance %u), channel %u",
                             pParams->chSup[ch].cmlTerm, ch);
                      return ERROR;
                  }
                }

                /* Output Mux Selection */
                if (pParams->chSup[ch].outSel == HMC7044_COS_DIVIDER) {
                    pImg->r147.fields.chout12OutputMuxSel = HMC7044_OMS_DIVIDER;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_ADLY) {
                    pImg->r147.fields.chout12OutputMuxSel = HMC7044_OMS_DIV_ADLY;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_NEIGHBOR) {
                    pImg->r147.fields.chout12OutputMuxSel
                                 = HMC7044_OMS_DIV_NEIGHBOR;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_FUNDAMENTAL) {
                    pImg->r147.fields.chout12OutputMuxSel = HMC7044_OMS_FUNDAMENTAL;
                } else {
                    sysLog("Bad value ( Output Mux %d), channel %u",
                            pParams->chSup[ch].outSel, ch);
                    return ERROR;
                }

                if (pParams->chSup[ch].slipQuantumPs == 0) {
                    pImg->r140.fields.chout12SlipEn = 0x0;
                    pImg->r140.fields.chout12MultiSlipEn = 0x0;
                } else if (fabs(pParams->chSup[ch].slipQuantumPs - vcoCyclePs)
                                < epsilon ) {
                    pImg->r140.fields.chout12SlipEn = 0x1;
                    pImg->r140.fields.chout12MultiSlipEn = 0x0;
                } else if (multiSlipVal > 1) {
                    pImg->r140.fields.chout12SlipEn = 0x0;
                    pImg->r140.fields.chout12MultiSlipEn = 0x1;
                    multiSlipVal = pParams->chSup[ch].slipQuantumPs / vcoCyclePs;
                    pImg->r145.fields.chout12LsbmultiSlipDigtlDelay
                        = HMC7044_LSB(multiSlipVal);
                    pImg->r146.fields.chout12MsbMultiSlipDigtlDelay
                        = HMC7044_MSB(multiSlipVal);
                } else {
                    sysLogFpa("Bad value ( slipQuantumPs  %0.0f), channel %f",
                             pParams->chSup[ch].slipQuantumPs, (double) ch);
                    return ERROR;
                 }

                pImg->r143.fields.chout12FineAnlgDelay = anlgDelayVal;
                pImg->r144.fields.chout12CoarseDigtlDelay = digDelayVal;

                pImg->r140.fields.chout12ChannelEn = 0x1;

                break;
              case 13:
                pImg->r14a.fields.chout13HighPerfMode
                    = pParams->chSup[ch].highPerfMode;
                pImg->r14a.fields.chout13SyncEn = 0x1;

                /* Configure start-up mode */
                if (pParams->chSup[ch].dynDriverEn)
                    pImg->r14a.fields.chout13StMode  = HMC7044_STMOD_DYN;
                else
                    pImg->r14a.fields.chout13StMode  = HMC7044_STMOD_ASYNC;


                pImg->r14b.fields.chout13LsbChannelDiv = HMC7044_LSB(chDivider);
                pImg->r14c.fields.chout13MsbChannelDiv = HMC7044_MSB(chDivider);

                /* Configure driver mode */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                    pImg->r152.fields.chout13DriverMode = HMC7044_CH_DM_CML;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVPECL) {
                    pImg->r152.fields.chout13DriverMode = HMC7044_CH_DM_LVPECL;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_LVDS) {
                    pImg->r152.fields.chout13DriverMode = HMC7044_CH_DM_LVDS;
                } else if (pParams->chSup[ch].drvMode == HMC7044_CDM_CMOS) {
                    pImg->r152.fields.chout13DriverMode = HMC7044_CH_DM_CMOS;
                } else {
                    sysLog("Bad value ( driver mode  %d), channel %u",
                           pParams->chSup[ch].drvMode, ch);
                    return ERROR;
                 }

                /* Configure Force Mute */
                if (pParams->chSup[ch].chMode == HMC7044_CHM_CLK) {
                    pImg->r152.fields.chout13ForceMute = HMC7044_FORCE_MUTE_NORMAL;
                } else if (pParams->chSup[ch].chMode == HMC7044_CHM_SYSREF) {
                    pImg->r152.fields.chout13ForceMute = HMC7044_FORCE_MUTE_LOGIC0;
                    pImg->r152.fields.chout13DynamicDriverEn
                        = pParams->chSup[ch].dynDriverEn;
                } else {
                    sysLog("Bad value (channel mode  %d), channel %u",
                                               pParams->chSup[ch].chMode, ch);
                    return ERROR;
                }

                /* Driver Impedance */
                if (pParams->chSup[ch].drvMode == HMC7044_CDM_CML) {
                  if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_NONE) {
                    pImg->r152.fields.chout13DriverImp = HMC7044_DRV_IMP_NONE;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_100) {
                    pImg->r152.fields.chout13DriverImp = HMC7044_DRV_IMP_100;
                  } else if (pParams->chSup[ch].cmlTerm == HMC7044_CCIT_50) {
                    pImg->r152.fields.chout13DriverImp = HMC7044_DRV_IMP_50;
                  } else {
                      sysLog("Bad value ( driver impedance  %d), channel %u",
                             pParams->chSup[ch].drvMode, ch);
                      return ERROR;
                   }
                }

                /* Output Mux Selection */
                if (pParams->chSup[ch].outSel == HMC7044_COS_DIVIDER) {
                  pImg->r151.fields.chout13OutputMuxSel = HMC7044_OMS_DIVIDER;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_ADLY) {
                  pImg->r151.fields.chout13OutputMuxSel = HMC7044_OMS_DIV_ADLY;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_DIV_NEIGHBOR) {
                  pImg->r151.fields.chout13OutputMuxSel = HMC7044_OMS_DIV_NEIGHBOR;
                } else if (pParams->chSup[ch].outSel == HMC7044_COS_FUNDAMENTAL) {
                  pImg->r151.fields.chout13OutputMuxSel = HMC7044_OMS_FUNDAMENTAL;
                } else {
                    sysLog("Bad value ( Output Mux %d), channel %u",
                            pParams->chSup[ch].outSel, ch);
                    return ERROR;
                }

                if (pParams->chSup[ch].slipQuantumPs == 0) {
                    pImg->r14a.fields.chout13SlipEn = 0x0;
                    pImg->r14a.fields.chout13MultiSlipEn = 0x0;
                } else if (fabs(pParams->chSup[ch].slipQuantumPs - vcoCyclePs)
                                < epsilon ) {
                    pImg->r14a.fields.chout13SlipEn = 0x1;
                    pImg->r14a.fields.chout13MultiSlipEn = 0x0;
                } else if (multiSlipVal > 1) {
                    pImg->r14a.fields.chout13SlipEn = 0x0;
                    pImg->r14a.fields.chout13MultiSlipEn = 0x1;
                    if (pParams->chSup[ch].drvMode == HMC7044_CDM_CMOS) {
                        addtlMultiSlip = chDivider / 2;
                        multiSlipVal += addtlMultiSlip;
                    }
                    pImg->r14f.fields.chout13LsbmultiSlipDigtlDelay
                        = HMC7044_LSB(multiSlipVal);
                    pImg->r150.fields.chout13MsbMultiSlipDigtlDelay
                        = HMC7044_MSB(multiSlipVal);
                } else {
                    sysLogFpa("Bad value ( slipQuantumPs  %0.0f), channel %f",
                             pParams->chSup[ch].slipQuantumPs, (double) ch);
                    return ERROR;
                 }

                pImg->r14d.fields.chout13FineAnlgDelay = anlgDelayVal;
                pImg->r14e.fields.chout13CoarseDigtlDelay = digDelayVal;

                pImg->r14a.fields.chout13ChannelEn = 0x1;
                break;
              default:
                sysLog("Bad value ( channel %u)", ch);
                return ERROR;
            }
        } else { /* Unused channel: Set Startup Mode to 0x00 */
            switch (ch) {
            case 0: pImg->rc8.fields.chout0StMode = HMC7044_STMOD_ASYNC; break;
            case 1: pImg->rd2.fields.chout1StMode = HMC7044_STMOD_ASYNC; break;
            case 2: pImg->rdc.fields.chout2StMode = HMC7044_STMOD_ASYNC; break;
            case 3: pImg->re6.fields.chout3StMode = HMC7044_STMOD_ASYNC; break;
            case 4: pImg->rf0.fields.chout4StMode = HMC7044_STMOD_ASYNC; break;
            case 5: pImg->rfa.fields.chout5StMode = HMC7044_STMOD_ASYNC; break;
            case 6: pImg->r104.fields.chout6StMode = HMC7044_STMOD_ASYNC; break;
            case 7: pImg->r10e.fields.chout7StMode = HMC7044_STMOD_ASYNC; break;
            case 8: pImg->r118.fields.chout8StMode = HMC7044_STMOD_ASYNC; break;
            case 9: pImg->r122.fields.chout9StMode = HMC7044_STMOD_ASYNC; break;
            case 10: pImg->r12c.fields.chout10StMode = HMC7044_STMOD_ASYNC; break;
            case 11: pImg->r136.fields.chout11StMode = HMC7044_STMOD_ASYNC; break;
            case 12: pImg->r140.fields.chout12StMode = HMC7044_STMOD_ASYNC; break;
            case 13: pImg->r14a.fields.chout13StMode = HMC7044_STMOD_ASYNC; break;
            default:
              sysLog("Bad value ( channel number: %u)", ch);
              return ERROR;
            }
        }
    }

    return OK;
}




/*******************************************************************************
* - name: hmc7044AppInitAlarmMask
*
* - title: Configure the alarm mask registers.
*
* - input: dev - CLKDST device for which to perform the operation
*          pParams - pointer to device setup parameters
*
* - output: hmc7044AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7044AppInitAlarmMask(CKDST_DEV dev,
                                     const Hmc7044_app_dev_params *pParams)
{
    Hmc7044_reg_image *pImg;

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppState.devState)) || !pParams) {
        sysLog("bad argument(s) (dev %u, pParams %d)", dev, pParams != NULL);
        return ERROR;
    }

    pImg = &hmc7044AppState.devState[dev].regImage;

    pImg->r70.fields.pll1NearLockMask = pParams->alarmsEn.pll1.nearLock;
    pImg->r70.fields.pll1LockAcquisitionMask = pParams->alarmsEn.pll1.lockAcq;
    pImg->r70.fields.pll1LockDetectMask = pParams->alarmsEn.pll1.lock;
    pImg->r70.fields.pll1HoldoverStatusMask = pParams->alarmsEn.pll1.holdover;
    pImg->r70.fields.clkinLosMask = (pParams->alarmsEn.pll1.ckIn3Los << 3) |
                                    (pParams->alarmsEn.pll1.ckIn2Los << 2) |
                                    (pParams->alarmsEn.pll1.ckIn1Los << 1) |
                                    (pParams->alarmsEn.pll1.ckIn0Los << 0) ;
    pImg->r71.fields.syncRequestMask = pParams->alarmsEn.syncReq;
    pImg->r71.fields.pll1AndPll2LockDetectMask = pParams->alarmsEn.pll1And2Locked;
    pImg->r71.fields.pll2LockDetectMask = pParams->alarmsEn.pll2Locked;
    pImg->r71.fields.clockOutputPhaseStatusMask = pParams->alarmsEn.cksPhase;
    pImg->r71.fields.sysrefSyncStatusMask = pParams->alarmsEn.srefSync;

    return OK;
}




/*******************************************************************************
* - name: hmc7044AppInitMisc
*
* - title: initialize miscellaneous register fields
*
* - input: dev - CLKDST device for which to perform the operation
*
* - output: hmc7044AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
* Software shall set register 0x0001 bit 6 (high performance distribution path)
* in all cases.
*
* Software shall write default values to registers 0x0017, 0x0018, 0x0019.
*
* Software shall set register 0x0009 bit 0 (disable SYNC at lock)
*
* Software shall set register 0x0028 bit 5 (PLL1 lock detect uses slip) to 0
*
* Software shall set register 0x0029 to default (0x05). Ditto register 0x002A
  (shall be set to 0).
*
* Software shall enable the PLL1 holdover DAC
*
* Software shall set register 0x0065 bit 0 to 0, except if no output channel is
  using analog delay.
*******************************************************************************/
LOCAL STATUS hmc7044AppInitMisc(CKDST_DEV dev,
                                const Hmc7044_app_dev_params *pParams) {

    Hmc7044_reg_image *pImg;
    STATUS status = OK;
    Bool r65Done = FALSE;
    unsigned i;

    /* initialize */
    if (!inEnumRange(dev, NELEMENTS(hmc7044AppState.devState))) {
        sysLog("bad argument (dev %u)", dev);
        return ERROR;
    }

    pImg = &hmc7044AppState.devState[dev].regImage;
    pImg->r01.fields.highPerfDistPath = 0x01;

    /* Software shall write default values to registers 0x0017, 0x0018, 0x0019 */
    pImg->r17.all = 0x00;
    pImg->r18.all = 0x04;
    pImg->r19.all = 0x00;

    pImg->r09.fields.disSyncAtLock = 0x1;
    pImg->r28.fields.useSlip = 0x0;
    pImg->r29.all = 0x05; /* enable PLL1 holdOver DAC */
    pImg->r2a.all = 0x00;

    /* If any channel uses analog delay (i.e, its aDlyPs parameter is non-zero),
     * analog delay low power mode bit should be set to 0, for maximum
     * performance. Only if no channel uses analog delay, then this bit should
     * be set to 1. */
     for (i = 0; i < HMC7044_OUT_NCHAN; i++) {
         if (pParams->chSup[i].aDlyPs > 0) {
              pImg->r65.fields.anlgDelayLowPower = 0;
              r65Done = TRUE;
              break;
         }
     }

    if (!r65Done)
      pImg->r65.fields.anlgDelayLowPower = 1;

    return status;
}




/*******************************************************************************
* - name: hmc7044AppInitPulseGenMode
*
* - title: Program the Pulse Generator Mode
*
* - input: dev - CLKDST device for which to perform the operation
*          pParams - pointer to device setup parameters
*
* - output: hmc7044AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7044AppInitPulseGenMode(CKDST_DEV dev,
        const Hmc7044_app_dev_params *pParams)
{
    Hmc7044_reg_image *pImg;
    Hmc7044_app_dev_ctl *pCtl;

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppState.devState)) || !pParams) {
        sysLog("bad argument(s) (dev %u, pParams %d)", dev, pParams != NULL);
        return ERROR;
    }

    pCtl = hmc7044AppCtl.devCtl + dev;

    if (!hmc7044IfCtl.initDone || !hmc7044AppCtl.initDone || !pCtl->initDone) {
        sysLog("initialization not done yet (dev %u, init. done %d,%d,%d)", dev,
               hmc7044IfCtl.initDone, hmc7044AppCtl.initDone, pCtl->initDone);

        return ERROR;
    }

    pImg = &hmc7044AppState.devState[dev].regImage;

    switch (pParams->sysref.mode) {
    case HMC7044_SRM_CONTINUOUS:
      pCtl->mode = HMC7044_5A_SRM_CONTINUOUS;
        break;
    case HMC7044_SRM_LEVEL_CTL:
      pCtl->mode = HMC7044_5A_SRM_LEVEL_CTL;
      break;
    case HMC7044_SRM_PULSED:
      if (pParams->sysref.nPulses == HMC7044_SRNP_1) {
          pCtl->mode = HMC7044_5A_SRNP_1;
      } else if (pParams->sysref.nPulses == HMC7044_SRNP_2) {
          pCtl->mode = HMC7044_5A_SRNP_2;
      } else if (pParams->sysref.nPulses == HMC7044_SRNP_4) {
          pCtl->mode = HMC7044_5A_SRNP_4;
      } else if (pParams->sysref.nPulses == HMC7044_SRNP_8) {
          pCtl->mode = HMC7044_5A_SRNP_8;
      } else if (pParams->sysref.nPulses == HMC7044_SRNP_16) {
          pCtl->mode = HMC7044_5A_SRNP_16;
      } else {
          sysLog("Bad value ( pParams->sysref.nPulses %d)",
                 pParams->sysref.nPulses);
          return ERROR;
      }
      break;
    default:
        sysLog("Bad value ( pParams->sysref.mode %d)",
               pParams->sysref.mode);
        return ERROR;
    }

    pImg->r5a.fields.pulseGenMode = pCtl->mode;

    return OK;
}




/*******************************************************************************
* - name: hmc7044AppInitSysrefTimer
*
* - title: Program the SYSREF timer with submultiple of lowest
*          output sysref frequency, not greater than 4MHz.
*
* - input: dev - CLKDST device for which to perform the operation
*          pParams - pointer to device setup parameters
*
* - output: hmc7044AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7044AppInitSysrefTimer(CKDST_DEV dev,
                                  const Hmc7044_app_dev_params *pParams)
{
    Hmc7044_app_dev_ctl *pCtl;
    Hmc7044_reg_image *pImg;
    CKDST_FREQ_HZ minFreq = 0;
    unsigned ch, timerVal;

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppState.devState)) || !pParams) {
        sysLog("bad argument(s) (dev %u, pParams %d)", dev, pParams != NULL);
        return ERROR;
    }

    pCtl = hmc7044AppCtl.devCtl + dev;

    if (!hmc7044IfCtl.initDone || !hmc7044AppCtl.initDone || !pCtl->initDone) {
        sysLog("initialization not done yet (dev %u, init. done %d,%d,%d)", dev,
               hmc7044IfCtl.initDone, hmc7044AppCtl.initDone, pCtl->initDone);

        return ERROR;
    }

    pImg = &hmc7044AppState.devState[dev].regImage;

    /* Find lowest output(SYSREF) frequency */
    for (ch = 0; ch < HMC7044_OUT_NCHAN; ch++) {
        if (pParams->chSup[ch].chMode == HMC7044_CHM_SYSREF) {
            if (minFreq == 0) {
                minFreq = pParams->chSup[ch].freq;
            }
            else {
                if (pParams->chSup[ch].freq < minFreq)
                  minFreq = pParams->chSup[ch].freq;
            }
        }
    }

    if (pParams->sysref.freq >= HMC7044_MAX_SYSREF_FREQ) {
        sysLog("SYSREF frequency %lu is greater than 4MHz.", pParams->sysref.freq);
        return ERROR;
    }

    if (!minFreq && (pParams->sysref.freq % minFreq != 0)) {
        sysLog("SYSREF frequency is not an integer multiple of lowest output "
            "frequency %lu, sysref frequency %lu)", minFreq, pParams->sysref.freq);
        return ERROR;
    }

    timerVal = (pParams->pll2Sup.vcoFreq) / pParams->sysref.freq;

    pImg->r5c.fields.lsbSysrefTimer = HMC7044_LSB(timerVal);
    pImg->r5d.fields.msbSysrefTimer = HMC7044_MSB(timerVal);

    return OK;
}




/*******************************************************************************
* - name: hmc7044ChkClkOutPhase
*
* - title: Check if the clock output phase alarm status is set.
*
* - input: dev - CLKDST device for which to perform the operation
*
* - returns: OK or ERROR if  clock output phase status is not set or
*            detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7044ChkClkOutPhase(CKDST_DEV dev)
{
    Hmc7044_reg_x7d r7d;

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppState.devState))) {
        sysLog("bad argument (dev %u)", dev);
        return ERROR;
    }

    if (hmc7044LliRegRead(dev, 0x7d, &r7d.all) != OK)
      return ERROR;

    return (!(r7d.fields.clockOutputPhaseStatus == 1));
}




/*******************************************************************************
* - name: hmc7044DisSync
*
* - title: Disables SYNC on all output channels
*
* - input: dev - CLKDST device for which to perform the operation
*
* - output: hmc7044AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7044DisSync(CKDST_DEV dev, const Hmc7044_app_dev_params *pParams)
{
    Hmc7044_reg_image *pImg;

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppState.devState)) || !pParams) {
        sysLog("bad argument(s) (dev %u, pParams %d)", dev, pParams != NULL);
        return ERROR;
    }

    pImg = &hmc7044AppState.devState[dev].regImage;

    pImg->rc8.fields.chout0SyncEn  = 0x0;
    if (hmc7044LliRegWrite(dev, 0xc8, pImg->rc8.all) != OK)
       return ERROR;

    pImg->rd2.fields.chout1SyncEn  = 0x0;
    if (hmc7044LliRegWrite(dev, 0xd2, pImg->rd2.all) != OK)
       return ERROR;

    pImg->rdc.fields.chout2SyncEn  = 0x0;
    if (hmc7044LliRegWrite(dev, 0xdc, pImg->rdc.all) != OK)
      return ERROR;

    pImg->re6.fields.chout3SyncEn  = 0x0;
    if (hmc7044LliRegWrite(dev, 0xe6, pImg->re6.all) != OK)
      return ERROR;

    pImg->rf0.fields.chout4SyncEn  = 0x0;
    if (hmc7044LliRegWrite(dev, 0xf0, pImg->rf0.all) != OK)
      return ERROR;

    pImg->rfa.fields.chout5SyncEn  = 0x0;
    if (hmc7044LliRegWrite(dev, 0xfa, pImg->rfa.all) != OK)
      return ERROR;

    pImg->r104.fields.chout6SyncEn = 0x0;
    if (hmc7044LliRegWrite(dev, 0x104, pImg->r104.all) != OK)
      return ERROR;

    pImg->r10e.fields.chout7SyncEn = 0x0;
    if (hmc7044LliRegWrite(dev, 0x10e, pImg->r10e.all) != OK)
      return ERROR;

    pImg->r118.fields.chout8SyncEn = 0x0;
    if (hmc7044LliRegWrite(dev, 0x118, pImg->r118.all) != OK)
      return ERROR;

    pImg->r122.fields.chout9SyncEn = 0x0;
    if (hmc7044LliRegWrite(dev, 0x122, pImg->r122.all) != OK)
      return ERROR;

    pImg->r12c.fields.chout10SyncEn = 0x0;
    if (hmc7044LliRegWrite(dev, 0x12c, pImg->r12c.all) != OK)
      return ERROR;

    pImg->r136.fields.chout11SyncEn = 0x0;
    if (hmc7044LliRegWrite(dev, 0x136, pImg->r136.all) != OK)
      return ERROR;

    pImg->r140.fields.chout12SyncEn = 0x0;
    if (hmc7044LliRegWrite(dev, 0x140, pImg->r140.all) != OK)
      return ERROR;

    pImg->r14a.fields.chout13SyncEn = 0x0;
    if (hmc7044LliRegWrite(dev, 0x14a, pImg->r14a.all) != OK)
      return ERROR;

    return OK;
}




/*******************************************************************************
* - name: hmc7044ToggleBit
*
* - title: Toggle the register bit mentioned in 'fieldBit' parameter at register
*          address- regIdx.
*
* - input: dev - CLKDST device for which to perform the operation
*          pParams - pointer to device setup parameters
*          regIdx - register address where to bit to be toggled is present.
*          field - the bit to be toggled.
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7044ToggleBit(CKDST_DEV dev, unsigned regIdx,
                              HMC7044_REG fieldBit, UINT64 delay)
{
    HMC7044_REG data;

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppState.devState)) ||
        regIdx > HMC7044_REG_INX_MAX || fieldBit > HMC7044_FIELD_BIT_MAX) {
        sysLog("bad argument (dev %u), regIdx %u", dev, regIdx);
        return ERROR;
    }

    if (!hmc7044IfCtl.initDone) {
        sysLog("interface initialization not done yet (dev %u)", dev);
        return ERROR;
    }

    if (hmc7044LliRegRead(dev, regIdx, &data) != OK)
      return ERROR;

    data |= (1 << fieldBit);

    if (hmc7044LliRegWrite(dev, regIdx, data) != OK)
      return ERROR;

    data &= ~(1 << fieldBit);

    if (hmc7044LliRegWrite(dev, regIdx, data) != OK)
      return ERROR;

    if (delay) {
        sysDelayUsec(delay);
    }

    return OK;
}




/*******************************************************************************
* - name: hmc7044RegPll1CpCur2Code
*
* - title: convert requested charge pump current to associated register code
*
* - input: cpCurUa - requested charge pump current [uA]
*          pCode   - pointer to where to return result
*
* - output: *pCode
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - notes:
*
* When converting charge pump current to a a numeric code that should be written
* to the respective PLL register, software shall accept any value in the allowed
* range and will set PLL register value yielding the closest possible charge pump
* current value.
*******************************************************************************/
LOCAL STATUS hmc7044RegPll1CpCur2Code(unsigned cpCurUa, unsigned *pCode)
{
    return hmc7044RegSrchTable(cpCurUa, HMC7044_R1A_CP_CUR_UA,
                               NELEMENTS(HMC7044_R1A_CP_CUR_UA), pCode);
}




/*******************************************************************************
* - name: hmc7044RegPll2CpCur2Code
*
* - title: convert requested charge pump current to associated register code
*
* - input: cpCurUa - requested charge pump current [uA]
*          pCode   - pointer to where to return result
*
* - output: *pCode
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
* - notes:
*
* When converting charge pump current to a a numeric code that should be written
* to the respective PLL register, software shall accept any value in the allowed
* range and will set PLL register value yielding the closest possible charge pump
* current value.
*******************************************************************************/
LOCAL STATUS hmc7044RegPll2CpCur2Code(unsigned cpCurUa, unsigned *pCode)
{
    return hmc7044RegSrchTable(cpCurUa, HMC7044_R37_CP_CUR_UA,
                               NELEMENTS(HMC7044_R37_CP_CUR_UA), pCode);
}




/*******************************************************************************
* - name: hmc7044RegSrchTable
*
* - title: search for a value in a table
*
* - input: value   - the value to search
*          table   - pointer to (start of) the table
*          tblNval - number of values in the table
*          pInx    - pointer to wheret to return result (index in table)
*
* - output: *pInx
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - notes: 1) Searching for the closest value in the table. Returning ERROR if value
*             is outside the range of table values.
*          2) Assuming that table[] is sorted in ascending order.
*******************************************************************************/
LOCAL STATUS hmc7044RegSrchTable(UINT32 value, const UINT32 *table,
                                 unsigned tblNval, unsigned *pInx)
{
    unsigned i, iLeft, iRight, deltaLeft, deltaRight;

    if (!table || !tblNval || !pInx) {
        sysLog("bad argument(s) (table %d, tblNval %u, pInx %d)", table != NULL,
               tblNval, pInx != NULL);
        return ERROR;
    }

    /* find two (or one) table value(s) bracketing value */
    if (value < table[0] || value > table[tblNval - 1]) {
        sysLog("value (%u) is outside the range of table values", value);
        return ERROR;
    }

    for (i = 0; i < tblNval; ++i) {
        if (value <= table[i])
            break;
    }

    if (i >= tblNval) {
        sysLog("internal inconsistency (1, value %u, i %u)", value, i);
        return ERROR;
    }

    if (i == 0)
        iLeft = iRight = 0;
    else if (i == tblNval - 1)
        iLeft = iRight = tblNval - 1;
    else {
        iLeft = i - 1;
        iRight = i;
    }

    if (value < table[iLeft] || value > table[iRight]) {
        sysLog("internal inconsistency (2, value %u, i %u, iLeft %u, iRight %u)",
               value, i, iLeft, iRight);
        return ERROR;
    }

    /* deduce the value that is closest to the one requested */
    deltaLeft  = value - table[iLeft];
    deltaRight = table[iRight] - value;

    *pInx = (deltaLeft <= deltaRight ? iLeft : iRight);

    return OK;
}




/*******************************************************************************
* - name: hmc7044Wait4Lock
*
* - title: wait until sensed PLL lock condition (or timeout)
*
* - input: dev           - CLKDST device for which to perform the operation
*          nsecPreChkDly - time to wait before starting poll
*          nsecLockTmout - lock timeout
*
* - returns: OK or ERROR if detected an error (including PLL lock timeout)
*
* - description: as above
*
* - notes: This routine employs busy wait while checking PLL lock status.
*          Calling thread's status priority is lowered across the polling sequence
*          (if applicable).
*******************************************************************************/
LOCAL STATUS hmc7044Wait4Lock(CKDST_DEV dev, UINT32 nsecPreChkDly,
                              UINT32 nsecLockTmout, PLLTYPE pllType)
{
    UINT64 nsecMax;
    Bool locked;
    STATUS status = OK;

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppState.devState))) {
        sysLog("invalid argument(s) (dev %u)", dev);
        return ERROR;
    }

    /* initialize */
    if (!hmc7044IfCtl.initDone) {
        sysLog("initialization not done yet (dev %u)", dev);
        return ERROR;
    }

    /* initial delay before starting to check lock detection status */
    nsecMax = sysTimeNsec() + nsecPreChkDly;

    while (sysTimeNsec() < nsecMax);

    /* wait until PLL is locked (or timeout) */
    nsecMax = sysTimeNsec() + nsecLockTmout;

    for (status = OK, locked = FALSE; sysTimeNsec() < nsecMax; ) {

      switch (pllType) {
      case PLL1:
          if (hmc7044GetPll1Lock(dev, &locked, TRUE) != OK) {
              status = ERROR;
          }
        break;
      case PLL2:
          if (hmc7044GetPll2Lock(dev, &locked, TRUE) != OK) {
              status = ERROR;
          }
         break;
      default:
          sysLog("Bad value ( PLL Type %d)", pllType);
          status = ERROR;
      }

      if (locked)
          break;  /* OK, PLL is locked */
     }

     if (status == OK && !locked) {
         /* try one more time */
         switch (pllType) {
         case PLL1:
             if (hmc7044GetPll1Lock(dev, &locked, TRUE) != OK) {
                 status = ERROR;
             }
             break;
         case PLL2:
             if (hmc7044GetPll2Lock(dev, &locked, TRUE) != OK) {
                 status = ERROR;
             }
             break;
         default:
             sysLog("Bad value ( PLL Type %d)",pllType);
             status = ERROR;
         }
     }

     if (!locked) {
		 sysLog("PLL%d lock failure (dev %u)", pllType, dev);
		 status = ERROR;
     }

     return status;
}




/*******************************************************************************
* - name: hmc7044AppInitWrRegs
*
* - title: write initialization register image data to CLKDST registers
*
* - input: dev - CLKDST device for which to perform the operation
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7044AppInitWrRegs(CKDST_DEV dev)
{
    typedef struct {unsigned regInx, dataOffs;} Reg_desc;

    static const Reg_desc regDescs[] = {
#       define RDESC(reg)  {0x##reg, offsetof(Hmc7044_reg_image, r##reg.all)}

        RDESC(01), RDESC(02), RDESC(03), RDESC(04), RDESC(05), RDESC(06), RDESC(07),
        RDESC(08), RDESC(09), RDESC(0a), RDESC(0b), RDESC(0c), RDESC(0d), RDESC(0e),
        RDESC(14), RDESC(15), RDESC(16), RDESC(17), RDESC(18), RDESC(19), RDESC(1a),
        RDESC(1b), RDESC(1c), RDESC(1d), RDESC(1e), RDESC(1f), RDESC(20), RDESC(21),
        RDESC(22), RDESC(26), RDESC(27), RDESC(28), RDESC(29), RDESC(2a), RDESC(31),
        RDESC(32), RDESC(33), RDESC(34), RDESC(35), RDESC(36), RDESC(37), RDESC(38),
        RDESC(39), RDESC(33), RDESC(34), RDESC(35), RDESC(36), RDESC(37), RDESC(38),
        RDESC(3a), RDESC(3b), RDESC(3c), RDESC(46), RDESC(47), RDESC(48), RDESC(49),
        RDESC(50), RDESC(51), RDESC(52), RDESC(53), RDESC(54), RDESC(5a), RDESC(5b),
        RDESC(5c), RDESC(5d), RDESC(5e), RDESC(64), RDESC(65), RDESC(70), RDESC(71),
        RDESC(96), RDESC(97), RDESC(98), RDESC(99), RDESC(9a), RDESC(9b), RDESC(9c),
        RDESC(9d), RDESC(9e), RDESC(9f), RDESC(a0), RDESC(a1), RDESC(a2), RDESC(a3),
        RDESC(a3), RDESC(a4), RDESC(a5), RDESC(a6), RDESC(a7), RDESC(a8), RDESC(a9),
        RDESC(ab), RDESC(ac), RDESC(ad), RDESC(ae), RDESC(af), RDESC(b0), RDESC(b1),
        RDESC(b2), RDESC(b3), RDESC(b5), RDESC(b6), RDESC(b7), RDESC(b8), RDESC(c8),
        RDESC(c9), RDESC(ca), RDESC(cb), RDESC(cc), RDESC(cd), RDESC(ce), RDESC(cf),
        RDESC(d0), RDESC(d1), RDESC(d2), RDESC(d3), RDESC(d4), RDESC(d5), RDESC(d6),
        RDESC(d7), RDESC(d8), RDESC(d9), RDESC(da), RDESC(db), RDESC(dc), RDESC(dd),
        RDESC(de), RDESC(df), RDESC(e0), RDESC(e1), RDESC(e2), RDESC(e3), RDESC(e4),
        RDESC(e5), RDESC(e6), RDESC(e7), RDESC(e8), RDESC(e9), RDESC(ea), RDESC(eb),
        RDESC(ec), RDESC(ed), RDESC(ee), RDESC(ef), RDESC(f0), RDESC(f1), RDESC(f2),
        RDESC(f3), RDESC(f4), RDESC(f5), RDESC(f6), RDESC(f7), RDESC(f8), RDESC(f9),
        RDESC(fa), RDESC(fb), RDESC(fc), RDESC(fd), RDESC(fe), RDESC(ff),
        RDESC(100), RDESC(101), RDESC(102), RDESC(103), RDESC(104), RDESC(105),
        RDESC(106), RDESC(107), RDESC(108), RDESC(109), RDESC(10a), RDESC(10b),
        RDESC(10c), RDESC(10d), RDESC(10e), RDESC(10f), RDESC(110), RDESC(111),
        RDESC(112), RDESC(113), RDESC(114), RDESC(115), RDESC(116), RDESC(117),
        RDESC(118), RDESC(119), RDESC(11a), RDESC(11b), RDESC(11c), RDESC(11d),
        RDESC(11e), RDESC(11f), RDESC(120), RDESC(121), RDESC(122), RDESC(123),
        RDESC(124), RDESC(125), RDESC(126), RDESC(127), RDESC(128), RDESC(129),
        RDESC(12a), RDESC(12b), RDESC(12c), RDESC(12d), RDESC(12e), RDESC(12f),
        RDESC(130), RDESC(131), RDESC(132), RDESC(133), RDESC(134), RDESC(135),
        RDESC(136), RDESC(137), RDESC(138), RDESC(139), RDESC(13a), RDESC(13b),
        RDESC(13c), RDESC(13d), RDESC(13e), RDESC(13f), RDESC(140), RDESC(141),
        RDESC(142), RDESC(143), RDESC(144), RDESC(145), RDESC(146), RDESC(147),
        RDESC(148), RDESC(149), RDESC(14a), RDESC(14b), RDESC(14c), RDESC(14d),
        RDESC(14e), RDESC(14f), RDESC(150), RDESC(151), RDESC(152), RDESC(153)

#       undef RDESC
    };

    unsigned i;
    const Hmc7044_reg_image *pImg;

    /* initialize */
    if (!inEnumRange(dev, NELEMENTS(hmc7044AppState.devState))) {
        sysLog("bad argument (dev %u)", dev);
        return ERROR;
    }

    pImg = &hmc7044AppState.devState[dev].regImage;

    /* perform the operation */
    for (i = 0; i < NELEMENTS(regDescs); ++i) {
        const Reg_desc *pDesc = regDescs + i;

        UINT8 regData = *((UINT8 *) pImg + pDesc->dataOffs);

    if (hmc7044LliRegWrite(dev, pDesc->regInx, regData) != OK)
          return ERROR;
    }

    return OK;
}




/*******************************************************************************
* - name: hmc7044AppInitRdRegs
*
* - title: read PLL registers and set up register image data
*
* - input: dev - PLL device for which to perform the operation
*
* - output: hmc7044AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
*******************************************************************************/
LOCAL STATUS hmc7044AppInitRdRegs(CKDST_DEV dev)
{
    typedef struct {unsigned regInx, dataOffs;} Reg_desc;

    static const Reg_desc regDescs[] = {
#       define RDESC(reg)  {0x##reg, offsetof(Hmc7044_reg_image, r##reg.all)}

        RDESC(01), RDESC(02), RDESC(03), RDESC(04), RDESC(05), RDESC(06), RDESC(07),
        RDESC(08), RDESC(09), RDESC(0a), RDESC(0b), RDESC(0c), RDESC(0d), RDESC(0e),
        RDESC(14), RDESC(15), RDESC(16), RDESC(17), RDESC(18), RDESC(19), RDESC(1a),
        RDESC(1b), RDESC(1c), RDESC(1d), RDESC(1e), RDESC(1f), RDESC(20), RDESC(21),
        RDESC(22), RDESC(26), RDESC(27), RDESC(28), RDESC(29), RDESC(2a), RDESC(31),
        RDESC(32), RDESC(33), RDESC(34), RDESC(35), RDESC(36), RDESC(37), RDESC(38),
        RDESC(39), RDESC(33), RDESC(34), RDESC(35), RDESC(36), RDESC(37), RDESC(38),
        RDESC(3a), RDESC(3b), RDESC(3c), RDESC(46), RDESC(47), RDESC(48), RDESC(49),
        RDESC(50), RDESC(51), RDESC(52), RDESC(53), RDESC(54), RDESC(5a), RDESC(5b),
        RDESC(5c), RDESC(5d), RDESC(5e), RDESC(64), RDESC(65), RDESC(70), RDESC(71),
        RDESC(96), RDESC(97), RDESC(98), RDESC(99), RDESC(9a), RDESC(9b), RDESC(9c),
        RDESC(9d), RDESC(9e), RDESC(9f), RDESC(a0), RDESC(a1), RDESC(a2), RDESC(a3),
        RDESC(a3), RDESC(a4), RDESC(a5), RDESC(a6), RDESC(a7), RDESC(a8), RDESC(a9),
        RDESC(ab), RDESC(ac), RDESC(ad), RDESC(ae), RDESC(af), RDESC(b0), RDESC(b1),
        RDESC(b2), RDESC(b3), RDESC(b5), RDESC(b6), RDESC(b7), RDESC(b8), RDESC(c8),
        RDESC(c9), RDESC(ca), RDESC(cb), RDESC(cc), RDESC(cd), RDESC(ce), RDESC(cf),
        RDESC(d0), RDESC(d1), RDESC(d2), RDESC(d3), RDESC(d4), RDESC(d5), RDESC(d6),
        RDESC(d7), RDESC(d8), RDESC(d9), RDESC(da), RDESC(db), RDESC(dc), RDESC(dd),
        RDESC(de), RDESC(df), RDESC(e0), RDESC(e1), RDESC(e2), RDESC(e3), RDESC(e4),
        RDESC(e5), RDESC(e6), RDESC(e7), RDESC(e8), RDESC(e9), RDESC(ea), RDESC(eb),
        RDESC(ec), RDESC(ed), RDESC(ee), RDESC(ef), RDESC(f0), RDESC(f1), RDESC(f2),
        RDESC(f3), RDESC(f4), RDESC(f5), RDESC(f6), RDESC(f7), RDESC(f8), RDESC(f9),
        RDESC(fa), RDESC(fb), RDESC(fc), RDESC(fd), RDESC(fe), RDESC(ff),
        RDESC(100), RDESC(101), RDESC(102), RDESC(103), RDESC(104), RDESC(105),
        RDESC(106), RDESC(107), RDESC(108), RDESC(109), RDESC(10a), RDESC(10b),
        RDESC(10c), RDESC(10d), RDESC(10e), RDESC(10f), RDESC(110), RDESC(111),
        RDESC(112), RDESC(113), RDESC(114), RDESC(115), RDESC(116), RDESC(117),
        RDESC(118), RDESC(119), RDESC(11a), RDESC(11b), RDESC(11c), RDESC(11d),
        RDESC(11e), RDESC(11f), RDESC(120), RDESC(121), RDESC(122), RDESC(123),
        RDESC(124), RDESC(125), RDESC(126), RDESC(127), RDESC(128), RDESC(129),
        RDESC(12a), RDESC(12b), RDESC(12c), RDESC(12d), RDESC(12e), RDESC(12f),
        RDESC(130), RDESC(131), RDESC(132), RDESC(133), RDESC(134), RDESC(135),
        RDESC(136), RDESC(137), RDESC(138), RDESC(139), RDESC(13a), RDESC(13b),
        RDESC(13c), RDESC(13d), RDESC(13e), RDESC(13f), RDESC(140), RDESC(141),
        RDESC(142), RDESC(143), RDESC(144), RDESC(145), RDESC(146), RDESC(147),
        RDESC(148), RDESC(149), RDESC(14a), RDESC(14b), RDESC(14c), RDESC(14d),
        RDESC(14e), RDESC(14f), RDESC(150), RDESC(151), RDESC(152), RDESC(153)

#       undef RDESC
    };

    unsigned i;
    Hmc7044_reg_image *pImg;

    /* initialize */
    if (!inEnumRange(dev, NELEMENTS(hmc7044AppState.devState))) {
        sysLog("bad argument (dev %u)", dev);
        return ERROR;
    }

    pImg = &hmc7044AppState.devState[dev].regImage;

    /* perform the operation */
    for (i = 0; i < NELEMENTS(regDescs); ++i) {
        const Reg_desc *pDesc = regDescs + i;

        UINT8 *pData = (UINT8 *) pImg + pDesc->dataOffs;

        if (hmc7044LliRegRead(dev, pDesc->regInx, pData) != OK)
            return ERROR;
    }

    pImg->initDone = TRUE;

    return OK;
}



/*#############################################################################*
*                     T O P - L E V E L    S E R V I C E S                     *
*#############################################################################*/

typedef struct {
    Hmc7044_dev_io_if ioIf;
} Hmc7044_lli_dev_ctl;


LOCAL struct {
    Bool initDone;
    CKDST_DEV_MASK devMask;
    Hmc7044_lli_dev_ctl devCtl[CKDST_MAX_NDEV];
} hmc7044LliCtl;

/* forward references */
LOCAL STATUS hmc7044GetPll1LockFmReg(CKDST_DEV dev, Bool *pLocked),
             hmc7044GetPll2LockFmReg(CKDST_DEV dev, Bool *pLocked);


/*******************************************************************************
* - name: hmc7044OutChEnDis
*
* - title: Enable/Disable a particular output channel for a particular device.
*
* - input: dev      - CLKDST device on which operation is performed.
*          iCh      - channel to be enabled or disabled.
*          enable   - True value enables channel, false value disables channel
*
* - output: hmc7044AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
EXPORT STATUS hmc7044OutChEnDis(CKDST_DEV dev, unsigned iCh, Bool enable)
{

    const Hmc7044_app_dev_ctl *pCtl;
    Hmc7044_reg_image *pImg;
    STATUS status = OK;

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppCtl.devCtl)) ||
        iCh < HMC7044_CH_OUT_MIN || iCh > HMC7044_CH_OUT_MAX) {
        sysLog("bad argument(s) (dev %u), iCh %u", dev, iCh);
        return ERROR;
    }

    pCtl = hmc7044AppCtl.devCtl + dev;
    pImg = &hmc7044AppState.devState[dev].regImage;

    if (!hmc7044IfCtl.initDone || !hmc7044AppCtl.initDone || !pCtl->initDone ||
        !pImg->initDone ) {
        sysLog("initialization not done yet (dev %u, init. done %d,%d,%d, %d)",
               dev, hmc7044IfCtl.initDone, hmc7044AppCtl.initDone,
               pCtl->initDone, pImg->initDone);
        return ERROR;
    }

    switch (iCh) {
    case 0: {
      pImg->rc8.fields.chout0ChannelEn = enable;
      status = hmc7044LliRegWrite(dev, 0xc8, pImg->rc8.all);
      break;
    }
    case 1: {
      pImg->rd2.fields.chout1ChannelEn = enable;
      status = hmc7044LliRegWrite(dev, 0xd2, pImg->rd2.all);
      break;
    }
    case 2: {
      pImg->rdc.fields.chout2ChannelEn = enable;
      status = hmc7044LliRegWrite(dev, 0xdc, pImg->rdc.all);
      break;
    }
    case 3: {
      pImg->re6.fields.chout3ChannelEn = enable;
      status = hmc7044LliRegWrite(dev, 0xe6, pImg->re6.all);
      break;
    }
    case 4: {
      pImg->rf0.fields.chout4ChannelEn = enable;
      status = hmc7044LliRegWrite(dev, 0xf0, pImg->rf0.all);
      break;
    }
    case 5: {
      pImg->rfa.fields.chout5ChannelEn = enable;
      status = hmc7044LliRegWrite(dev, 0xfa, pImg->rfa.all);
      break;
    }
    case 6: {
      pImg->r104.fields.chout6ChannelEn = enable;
      status = hmc7044LliRegWrite(dev, 0x104, pImg->r104.all);
      break;
    }
    case 7: {
      pImg->r10e.fields.chout7ChannelEn = enable;
      status = hmc7044LliRegWrite(dev, 0x10e, pImg->r10e.all);
      break;
    }
    case 8: {
      pImg->r118.fields.chout8ChannelEn = enable;
      status = hmc7044LliRegWrite(dev, 0x118, pImg->r118.all);
      break;
    }
    case 9: {
      pImg->r122.fields.chout9ChannelEn = enable;
      status = hmc7044LliRegWrite(dev, 0x122, pImg->r122.all);
      break;
    }
    case 10: {
      pImg->r12c.fields.chout10ChannelEn = enable;
      status = hmc7044LliRegWrite(dev, 0x12c, pImg->r12c.all);
      break;
    }
    case 11: {
      pImg->r136.fields.chout11ChannelEn = enable;
      status = hmc7044LliRegWrite(dev, 0x136, pImg->r136.all);
      break;
    }
    case 12: {
      pImg->r140.fields.chout12ChannelEn = enable;
      status = hmc7044LliRegWrite(dev, 0x140, pImg->r140.all);
      break;
    }
    case 13: {
      pImg->r14a.fields.chout13ChannelEn = enable;
      status = hmc7044LliRegWrite(dev, 0x14a, pImg->r14a.all);
      break;
      }
    }

    if (status != OK)
      return ERROR;

    return status;
}




/*******************************************************************************
* - name: hmc7044ChDoSlip
*
* - title: Generate slip event for a particular device.
*
* - input: dev     - CLKDST device on which operation is performed.
*          chMask  - channel mask
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
EXPORT STATUS hmc7044ChDoSlip(CKDST_DEV dev, HMC7044_CH_MASK chMask)
{
    const Hmc7044_app_dev_ctl *pCtl;
    STATUS status = OK;

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppCtl.devCtl))) {
        sysLog("bad argument(s) (dev %u)", dev);
        return ERROR;
    }

    pCtl = hmc7044AppCtl.devCtl + dev;

    if (!chMask || chMask >= 1 << NELEMENTS(pCtl->params.chSup)) {
        sysLog("bad argument (chMask 0x%x)", chMask);
        return ERROR;
    }

    if (!hmc7044IfCtl.initDone || !hmc7044AppCtl.initDone || !pCtl->initDone) {
        sysLog("initialization not done yet (dev %u, init. done %d,%d,%d)",
               dev, hmc7044IfCtl.initDone, hmc7044AppCtl.initDone,
               pCtl->initDone);
        return ERROR;
    }

    status = hmc7044ToggleBit(dev, 0x02, HMC7044_SLIP_REQ_BIT, 0);

    return status;

}




/*******************************************************************************
* - name: hmc7044SetSysrefMode
*
* - title: Set the Pulse Generator Mode
*
* - input: dev -  CLKDST device for which to perform the operation
*          mode - Pulse Generator Mode (continuous, level sensitive or pulsed)
*          nPulses - number of pulses (relevant only for pulsed mode)
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
EXPORT STATUS hmc7044SetSysrefMode(CKDST_DEV dev, HMC7044_SREF_MODE mode,
                            HMC7044_SREF_NPULSES nPulses)
{
    Hmc7044_reg_image *pImg;
    Hmc7044_app_dev_ctl *pCtl;
    HMC7044_5A_SREF_MODE srefMode;

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppState.devState))) {
        sysLog("bad argument(s) (dev %u)", dev);
        return ERROR;
    }

    pImg = &hmc7044AppState.devState[dev].regImage;
    pCtl = hmc7044AppCtl.devCtl + dev;

    if (!hmc7044IfCtl.initDone || !hmc7044AppCtl.initDone  || !pImg->initDone) {
        sysLog("initialization not done yet (dev %u, init. done %d,%d,%d)",
               dev, hmc7044IfCtl.initDone, hmc7044AppCtl.initDone,
               pImg->initDone);
        return ERROR;
    }

    switch (mode) {
    case HMC7044_SRM_CONTINUOUS:
      srefMode = HMC7044_5A_SRM_CONTINUOUS;
      break;
    case HMC7044_SRM_LEVEL_CTL:
      srefMode = HMC7044_5A_SRM_LEVEL_CTL;
      break;
    case HMC7044_SRM_PULSED:
      if (nPulses == HMC7044_SRNP_1) {
          srefMode = HMC7044_5A_SRNP_1;
      } else if (nPulses == HMC7044_SRNP_2) {
          srefMode = HMC7044_5A_SRNP_2;
      } else if (nPulses == HMC7044_SRNP_4) {
          srefMode = HMC7044_5A_SRNP_4;
      } else if (nPulses == HMC7044_SRNP_8) {
          srefMode = HMC7044_5A_SRNP_8;
      } else if (nPulses == HMC7044_SRNP_16) {
          srefMode = HMC7044_5A_SRNP_16;
      } else {
          sysLog("Bad value ( nPulses %d)",nPulses);
          return ERROR;
      }
      break;
      default:
        sysLog("Bad value ( mode %d)",mode);
        return ERROR;
    }

    pImg->r5a.fields.pulseGenMode = srefMode;
    if (hmc7044LliRegWrite(dev, 0x5a, pImg->r5a.all) != OK) {
        return ERROR;
    } else {
        pCtl->mode = srefMode;
    }

    return OK;
}




/*******************************************************************************
* - name: hmc7044SysrefSwPulseN
*
* - title: Generate N Pulses on SYSREF channels of a particular device.
*
* - input: dev     - CLKDST device on which operation is performed.
*          chMask  - channel mask
*          nPulses - number of pulses
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - notes:
* Need to verify that current SYSREF mode is PULSED.
*
*******************************************************************************/
EXPORT STATUS hmc7044SysrefSwPulseN(CKDST_DEV dev, HMC7044_CH_MASK chMask,
                             HMC7044_SREF_NPULSES nPulses) {

    Hmc7044_reg_image *pImg;
    STATUS status = OK;
    const Hmc7044_app_dev_ctl *pCtl;

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppState.devState))) {
        sysLog("Bad argument ( dev %u)",dev);
        return ERROR;
    }

    pImg = &hmc7044AppState.devState[dev].regImage;

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppCtl.devCtl))) {
        sysLog("bad argument(s) (dev %u)", dev);
        return ERROR;
    }

    pCtl = hmc7044AppCtl.devCtl + dev;

    if (!chMask || chMask >= 1 << NELEMENTS(pCtl->params.chSup)) {
        sysLog("bad argument (chMask 0x%x)", chMask);
        return ERROR;
    }

    if (!hmc7044IfCtl.initDone || !hmc7044AppCtl.initDone || !pImg->initDone) {
        sysLog("initialization not done yet (dev %u, init. done %d,%d,%d)",
               dev, hmc7044IfCtl.initDone, hmc7044AppCtl.initDone,
               pImg->initDone);
        return ERROR;
    }

    if (pCtl->mode == HMC7044_5A_SRM_LEVEL_CTL ||
        pCtl->mode == HMC7044_5A_SRM_CONTINUOUS) {
        sysLog("Pulse mode is not pulsed (Pulse mode 0x%x)", pCtl->mode);
        return ERROR;
    }

    switch (nPulses) {
    case HMC7044_SRNP_1: {
      pImg->r5a.fields.pulseGenMode = HMC7044_5A_SRNP_1;
      break;
    }
    case HMC7044_SRNP_2: {
      pImg->r5a.fields.pulseGenMode = HMC7044_5A_SRNP_2;
      break;
    }
    case HMC7044_SRNP_4: {
      pImg->r5a.fields.pulseGenMode = HMC7044_5A_SRNP_4;
      break;
    }
    case HMC7044_SRNP_8: {
      pImg->r5a.fields.pulseGenMode = HMC7044_5A_SRNP_8;
      break;
    }
    case HMC7044_SRNP_16: {
      pImg->r5a.fields.pulseGenMode = HMC7044_5A_SRNP_16;
      break;
    }
    default:
      sysLog("Bad value ( nPulses %d)",nPulses);
      return ERROR;
    }

    if (hmc7044LliRegWrite(dev, 0x5a, pImg->r5a.all) != OK) {
        return ERROR;
    }

    status = hmc7044ToggleBit(dev, 0x01, HMC7044_PULSE_GEN_BIT, 0);

    if (status != OK) {
        return ERROR;
    }

    return status;
}




/*******************************************************************************
* - name: hmc7044GetPll1Lock
*
* - title: obtain current PLL locking status
*
* - input: dev       - CLKDST device for which to perform operation
*          *pLocked  - pointer to where to return PLL locking status
*           wait4Lock - TRUE if need to wait for PLL lock
*
* - output: *pLocked
*
* - returns: OK or ERROR if detected an error (in which case *pLocked is unusable)
*
* - description: as above
*
* - notes:
*           It is assumed that hmc7044LliInit has already been invoked.
*******************************************************************************/
EXPORT STATUS hmc7044GetPll1Lock(CKDST_DEV dev, Bool *pIsLocked, Bool wait4Lock)
{
    Bool isLocked;
    UINT64 nsecLastCmdAt, nsecNow;
    HMC7044_LOCK_CHECK *lockCheck;
    const Hmc7044_lli_dev_ctl *pLliCtl;
    const Hmc7044_app_dev_ctl *pAppCtl;

    /* validate arguments and initialize */
    if (!inEnumRange(dev, NELEMENTS(hmc7044LliCtl.devCtl)) || !pIsLocked) {
        sysLog("invalid argument(s) (dev %u, pIsLocked %d)",
               dev, pIsLocked != NULL);
        return ERROR;
    }

    pLliCtl = hmc7044LliCtl.devCtl + dev;
    pAppCtl = hmc7044AppCtl.devCtl + dev;

    *pIsLocked = FALSE;  /* initialize (just for safety) */

    if (!hmc7044LliCtl.initDone) {
        sysLog("subsystem initialization not done yet (initDone %d, pLockCheck %d, "
               "dev %u)", hmc7044LliCtl.initDone, pLliCtl->ioIf.pLockCheck1 != NULL,
               dev);
        return ERROR;
    }

    if (!(lockCheck = pLliCtl->ioIf.pLockCheck1))
        lockCheck = hmc7044GetPll1LockFmReg;

       /* sample PLL locking status and associated state in an interlocked
        * fashion */
       hmc7044CsEnter(dev, __FUNCTION__);

       /* operation order here is critical */
       nsecLastCmdAt = hmc7044AppState.devState[dev].nsecCmdAt;  /* unused if synchr */
       nsecNow       = sysTimeNsec();                            /* ditto */

       hmc7044CsExit(dev, __FUNCTION__);

       if (lockCheck(dev, &isLocked) != OK) {
           sysLog("lockCheck failed (dev %u)", dev);
           return ERROR;
       }

       if (!isLocked && !wait4Lock) {
          /*  deduce locked status */
          if (nsecNow < nsecLastCmdAt) {
              sysLogFpa("unexpected timing relationship (cmdAt %.0f, now %.0f)",
                        (double) nsecLastCmdAt, (double) nsecNow);
              return ERROR;
          } else if ((nsecNow - nsecLastCmdAt) < pAppCtl->nSecPll1LockTmout)
              isLocked = TRUE;
      }

      *pIsLocked = isLocked;

      return OK;
}




/*******************************************************************************
* - name: hmc7044GetPll1LockFmReg
*
* - title: read PLL lock status from the associated register
*
* - input: dev      - CLKDST device for which to perform operation
*          *pLocked - pointer to where to return PLL locking status
*
* - output: *pLocked
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7044GetPll1LockFmReg(CKDST_DEV dev, Bool *pLocked)
{
    Hmc7044_reg_x7c r7c;
    const Hmc7044_app_dev_ctl *pCtl;

    /* initialize */
    if (!inEnumRange(dev, NELEMENTS(hmc7044AppCtl.devCtl)) || !pLocked) {
        sysLog("bad argument(s) (dev %u, pLocked %d)", dev, pLocked != NULL);
        return ERROR;
    }

    pCtl = hmc7044AppCtl.devCtl + dev;

    if (!hmc7044IfCtl.initDone || !hmc7044AppCtl.initDone || !pCtl->initDone) {
        sysLog("initialization not done yet (dev %u, init. done %d,%d,%d)", dev,
               hmc7044IfCtl.initDone, hmc7044AppCtl.initDone, pCtl->initDone);
        return ERROR;
    }

    /* perform the operation */
    if (hmc7044LliRegRead(dev, 0x7c, &r7c.all) != OK)
        return ERROR;

    *pLocked = (r7c.fields.pll1LockDetect == 1);

    return OK;
}




/*******************************************************************************
* - name: hmc7044GetPll2Lock
*
* - title: obtain current PLL locking status
*
* - input: dev       - CLKDST device for which to perform operation
*          *pLocked  - pointer to where to return PLL locking status
*           wait4Lock - TRUE if need to wait for PLL lock
*
* - output: *pLocked
*
* - returns: OK or ERROR if detected an error (in which case *pLocked is unusable)
*
* - description: as above
*
* - notes:
*           It is assumed that hmc7044LliInit has already been invoked.
*******************************************************************************/
EXPORT STATUS hmc7044GetPll2Lock(CKDST_DEV dev, Bool *pIsLocked,
                                 Bool wait4Lock)
{
    Bool isLocked;
    UINT64 nsecLastCmdAt, nsecNow;
    HMC7044_LOCK_CHECK *lockCheck;
    const Hmc7044_lli_dev_ctl *pLliCtl;
    const Hmc7044_app_dev_ctl *pAppCtl;

    /* validate arguments and initialize */
    if (!inEnumRange(dev, NELEMENTS(hmc7044LliCtl.devCtl)) || !pIsLocked) {
        sysLog("invalid argument(s) (dev %u, pLocked %d)", dev, pIsLocked != NULL);
        return ERROR;
    }

    pLliCtl = hmc7044LliCtl.devCtl + dev;
    pAppCtl = hmc7044AppCtl.devCtl + dev;

    *pIsLocked = FALSE;  /* initialize (just for safety) */

    if (!hmc7044LliCtl.initDone) {
        sysLog("subsystem initialization not done yet (initDone %d, pLockCheck %d, "
               "dev %u)", hmc7044LliCtl.initDone, pLliCtl->ioIf.pLockCheck2 != NULL,
               dev);

        return ERROR;
    }

    if (!(lockCheck = pLliCtl->ioIf.pLockCheck2))
         lockCheck = hmc7044GetPll2LockFmReg;

    /* sample PLL locking status and associated state in an interlocked fashion */
      hmc7044CsEnter(dev, __FUNCTION__);

      /* operation order here is critical */
      nsecLastCmdAt = hmc7044AppState.devState[dev].nsecCmdAt;  /* unused if synchr */
      nsecNow       = sysTimeNsec();                            /* ditto */

      hmc7044CsExit(dev, __FUNCTION__);

      if (lockCheck(dev, &isLocked) != OK) {
           sysLog("lockCheck failed (dev %u)", dev);
           return ERROR;
       }

      if (!isLocked && !wait4Lock) {
          /*  deduce locked status */
          if (nsecNow < nsecLastCmdAt) {
              sysLogFpa("unexpected timing relationship (cmdAt %.0f, now %.0f)",
                        (double) nsecLastCmdAt, (double) nsecNow);

              return ERROR;
          } else if ((nsecNow - nsecLastCmdAt) < pAppCtl->nSecPll2LockTmout)
              isLocked = TRUE;
      }

      *pIsLocked = isLocked;

      return OK;
}




/*******************************************************************************
* - name: hmc7044GetPll2LockFmReg
*
* - title: read PLL lock status from the associated register
*
* - input: dev      - PLL device for which to perform operation
*          *pLocked - pointer to where to return PLL locking status
*
* - output: *pLocked
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7044GetPll2LockFmReg(CKDST_DEV dev, Bool *pLocked)
{
    Hmc7044_reg_x7d r7d;
    const Hmc7044_app_dev_ctl *pCtl;

    /* initialize */
    if (!inEnumRange(dev, NELEMENTS(hmc7044AppCtl.devCtl)) || !pLocked) {
        sysLog("bad argument(s) (dev %u, pLocked %d)", dev, pLocked != NULL);
        return ERROR;
    }

    pCtl = hmc7044AppCtl.devCtl + dev;

    if (!hmc7044IfCtl.initDone || !hmc7044AppCtl.initDone || !pCtl->initDone) {
        sysLog("initialization not done yet (dev %u, init. done %d,%d,%d)", dev,
               hmc7044IfCtl.initDone, hmc7044AppCtl.initDone, pCtl->initDone);
        return ERROR;
    }

    /* perform the operation */
    if (hmc7044LliRegRead(dev, 0x7d, &r7d.all) != OK)
        return ERROR;

    *pLocked = r7d.fields.pll2LockDetect == 1;

    return OK;
}




/*******************************************************************************
* - name: hmc7044GetPll1ActCkIn
*
* - title: read Active CLKIN from the associated register
*
* - input: dev      - CLKDST device for which to perform operation
*          *pCkIn - pointer to where to return PLL Active CLKIN
*
* - output: *pCkIn indicates which CLKIN input is currently in use
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
EXPORT STATUS hmc7044GetPll1ActCkIn(CKDST_DEV dev, unsigned *pCkIn)
{
    Hmc7044_reg_x82 r82;
    const Hmc7044_app_dev_ctl *pCtl;

    /* initialize */
    if (!inEnumRange(dev, NELEMENTS(hmc7044AppCtl.devCtl)) || !pCkIn) {
        sysLog("bad argument(s) (dev %u, pLocked %d)", dev, pCkIn != NULL);
        return ERROR;
    }

    pCtl = hmc7044AppCtl.devCtl + dev;

    if (!hmc7044IfCtl.initDone || !hmc7044AppCtl.initDone || !pCtl->initDone) {
        sysLog("initialization not done yet (dev %u, init. done %d,%d,%d)", dev,
               hmc7044IfCtl.initDone, hmc7044AppCtl.initDone, pCtl->initDone);
        return ERROR;
    }

   /* perform the operation */
    if (hmc7044LliRegRead(dev, 0x82, &r82.all) != OK)
      return ERROR;

    *pCkIn = r82.fields.pll1ActiveClkin;

    return OK;
}




/*******************************************************************************
* - name: hmc7044GetAlarm
*
* - title: Get alarm status for a particular device.
*
* - input: dev   - CLKDST device on which operation is performed.
*          pAlarm - out parameter having alarm status written.
*
* - output: *pAlarm
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
* Notes : hmc7044GetAlarm reads register 0x007B.
*******************************************************************************/
EXPORT STATUS hmc7044GetAlarm(CKDST_DEV dev, Bool *pAlarm)
{
    const Hmc7044_app_dev_ctl *pCtl;
    Hmc7044_reg_x7b r7b;
    STATUS status = OK;

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppCtl.devCtl))) {
        sysLog("bad argument(s) (dev %u)", dev);
        return ERROR;
    }

    pCtl = hmc7044AppCtl.devCtl + dev;

    if (!hmc7044IfCtl.initDone || !hmc7044AppCtl.initDone || !pCtl->initDone ) {
        sysLog("initialization not done yet (dev %u, init. done %d,%d,%d)",
               dev, hmc7044IfCtl.initDone, hmc7044AppCtl.initDone,
               pCtl->initDone);
        return ERROR;
    }

    if (hmc7044LliRegRead(dev, 0x7b, &r7b.all) != OK)
      return ERROR;

    *pAlarm = (r7b.fields.alarmSignal == 1);

    return status;
}




/*******************************************************************************
* - name: hmc7044GetAlarms
*
* - title: Retrieve all alarm status from alarm read back register 0x7c and 0x7d
*
* - input: dev      - CLKDST device on which operation is performed.
*          pAlarms  - out parameter that has all alarm status
*
* - output: *pAlarms
*
* - returns: OK or ERROR if detected an error
*
* - description: as above.
*******************************************************************************/
EXPORT STATUS hmc7044GetAlarms(CKDST_DEV dev, Hmc7044_dev_alarms *pAlarms)
{
    const Hmc7044_app_dev_ctl *pCtl;
    Hmc7044_reg_x7d r7d;
    Hmc7044_reg_x7c r7c;
    STATUS status = OK;

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppCtl.devCtl)) || !pAlarms) {
        sysLog("bad argument(s) (dev %u), pAlarms %d", dev, (pAlarms != NULL));
        return ERROR;
    }

    pCtl = hmc7044AppCtl.devCtl + dev;

    if (!hmc7044IfCtl.initDone || !hmc7044AppCtl.initDone || !pCtl->initDone) {
        sysLog("initialization not done yet (dev %u, init. done %d,%d,%d)",
               dev, hmc7044IfCtl.initDone, hmc7044AppCtl.initDone,
               pCtl->initDone);
        return ERROR;
    }

    if (hmc7044LliRegRead(dev, 0x7d, &r7d.all) != OK)
      return ERROR;

    pAlarms->syncReq        = (r7d.fields.syncRequestStatus == 1);
    pAlarms->pll1And2Locked = (r7d.fields.pll1AndPll2LockDetect == 1);
    pAlarms->cksPhase       = (r7d.fields.clockOutputPhaseStatus == 1);
    pAlarms->srefSync       = (r7d.fields.sysrefSyncStatus == 1);
    pAlarms->pll2Locked     = (r7d.fields.pll2LockDetect == 1);

    if (hmc7044LliRegRead(dev, 0x7c, &r7c.all) != OK)
      return ERROR;

    pAlarms->pll1.nearLock = (r7c.fields.pll1NearLock == 1);
    pAlarms->pll1.lockAcq  = (r7c.fields.pll1LockAcquisition == 1);
    pAlarms->pll1.lock     = (r7c.fields.pll1LockDetect == 1);
    pAlarms->pll1.holdover = (r7c.fields.pll1HoldoverStatus == 1);

    if ((r7c.fields.clkinLos) & (1 << 0)) {
        pAlarms->pll1.ckIn0Los = 0x1;
    } else {
        pAlarms->pll1.ckIn0Los = 0x0;
    }

    if ((r7c.fields.clkinLos) & (1 << 1)) {
        pAlarms->pll1.ckIn1Los = 0x1;
    } else {
        pAlarms->pll1.ckIn1Los = 0x0;
    }

    if ((r7c.fields.clkinLos) & (1 << 2)) {
        pAlarms->pll1.ckIn2Los = 0x1;
    } else {
        pAlarms->pll1.ckIn2Los = 0x0;
    }

    if ((r7c.fields.clkinLos) & (1 << 3)) {
        pAlarms->pll1.ckIn3Los = 0x1;
    } else {
        pAlarms->pll1.ckIn3Los = 0x0;
    }

    return status;
}




/*******************************************************************************
* - name: hmc7044ClearAlarms
*
* - title: Clear alarms for a particular device.
*
* - input: dev   - CLKDST device on which operation is performed.
*
* - output: hmc7044AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - notes: the operation is interlocked via the associated critical section
*******************************************************************************/
EXPORT STATUS hmc7044ClearAlarms(CKDST_DEV dev)
{
    const Hmc7044_app_dev_ctl *pCtl;
    Hmc7044_reg_image *pImg;

    if (!inEnumRange(dev, NELEMENTS(hmc7044AppCtl.devCtl))) {
        sysLog("bad argument(s) (dev %u)", dev);
        return ERROR;
    }

    pCtl = hmc7044AppCtl.devCtl + dev;
    pImg = &hmc7044AppState.devState[dev].regImage;

    if (!hmc7044IfCtl.initDone || !hmc7044AppCtl.initDone || !pCtl->initDone ||
        !pImg->initDone ) {
        sysLog("initialization not done yet (dev %u, init. done %d,%d,%d, %d)",
               dev, hmc7044IfCtl.initDone, hmc7044AppCtl.initDone,
               pCtl->initDone, pImg->initDone);
        return ERROR;
    }

    pImg->r06.fields.clearAlarms = 1;
    if (hmc7044LliRegWrite(dev, 0x06, pImg->r06.all) != OK) {
      return ERROR;
    }

    return OK;
}




/*******************************************************************************
* - name: hmc7044RegRead
*
* - title: read a CLKDST register
*
* - input: dev    - CLKDST device for which to perform the operation
*          regInx - CLKDST register to read
*          pData  - pointer to where to return read data
*
* - output: *pData (indirectly)
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - notes: HMC7044 registers are 8-bit wide
*******************************************************************************/
EXPORT STATUS hmc7044RegRead(CKDST_DEV dev, unsigned regInx, HMC7044_REG *pData)
{
    HMC7044_REG regData;

    if (hmc7044LliRegRead(dev, regInx, &regData) != OK)
        return ERROR;

    *pData = regData;

    return OK;
}




/*******************************************************************************
* - name: hmc7044RegWrite
*
* - title: write a CLKDST register
*
* - input: dev     - CLKDST device for which to perform the operation
*          regInx  - CLKDST register to write
*          regData - raw data to write to the register
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - notes: HMC7044 registers are 8-bit wide
*******************************************************************************/
EXPORT STATUS hmc7044RegWrite(CKDST_DEV dev, unsigned regInx, HMC7044_REG regData)
{
    return hmc7044LliRegWrite(dev, regInx, regData);
}


/*#############################################################################*
*                  L O W - L E V E L    I N T E R F A C E                      *
*#############################################################################*/


/*******************************************************************************
* - name: hmc7044LliInit
*
* - title: initialize the low-level interface
*
* - input: devMask - specifies the device(s) that need to be initialized
*
* - output: hmc7044LliCtl
*
* - returns: OK or ERROR if detected an error
*
* - algorithm: as above
*
* - notes: not attempting to interlock this operation
*******************************************************************************/
LOCAL STATUS hmc7044LliInit(CKDST_DEV_MASK devMask)
{
    unsigned i;

    if (!devMask || devMask >= 1 << NELEMENTS(hmc7044LliCtl.devCtl)) {
        sysLog("bad argument (devMask 0x%x)", devMask);
        return ERROR;
    }

    /* initialize control data */
    hmc7044LliCtl.devMask = devMask;

    for (i = 0; i < NELEMENTS(hmc7044LliCtl.devCtl); ++i)
        memset(&hmc7044LliCtl.devCtl[i], 0, sizeof(hmc7044LliCtl.devCtl[i]));

    hmc7044LliCtl.initDone = TRUE;

    return OK;
}





/*******************************************************************************
* - name: hmc7044LliInitDev
*
* - title: initialize a specific CLKDST device
*
* - input: dev      - CLKDST device for which to perform the operation
*          pIf      - pointer to low-level interface access-related parameters
*          warmInit - warm initialization flag (currently unused)
*
* - output: hmc7044LliCtl.devCtl[dev]
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - note: not attempting to interlock the operation
*******************************************************************************/
LOCAL STATUS hmc7044LliInitDev(CKDST_DEV dev, const Hmc7044_dev_io_if *pIf,
		Bool warmInit)
{

    /* validate arguments and initialize */
    if (!inEnumRange(dev, NELEMENTS(hmc7044LliCtl.devCtl)) || !pIf) {
        sysLog("bad argument(s) (1, dev %u, pIf %d, warmInit %d)", dev,
               pIf != NULL, warmInit);
        return ERROR;
    }

    if (!pIf->pRegRead || !pIf->pRegWrite || !pIf->pLockCheck1 ||
        !pIf->pLockCheck2) {
        sysLog("bad argument(s) (2, dev %u, pRegRead %d, pRegWrite %d, "
               "pLockCheck1 %d, pLockCheck2 %d )", dev, pIf->pRegRead != NULL,
               pIf->pRegWrite != NULL, pIf->pLockCheck1 != NULL,
               pIf->pLockCheck2 != NULL);
        return ERROR;
    }

    if (!hmc7044LliCtl.initDone) {
        sysLog("subsystem initialization not done yet (dev %u, warmInit %d)", dev,
               warmInit);
        return ERROR;
    }

    if (!((1 << dev) & hmc7044LliCtl.devMask)) {
         sysLog("unexpected device (%d; devMask 0x%08x)", dev,
                hmc7044LliCtl.devMask);
         return ERROR;
     }

    /* set up control data */
    hmc7044LliCtl.devCtl[dev].ioIf = *pIf;

    return OK;

}




/*******************************************************************************
* - name: hmc7044LliRegRead
*
* - title: perform a direct read from a device register via SPI
*
* - input: dev    - CLKDST device for which to perform the operation
*          regInx - CLKDST register to read
*          pData  - pointer to where to return read data
*
* - output: *pData (indirectly)
*
* - returns: status returned from the call
*            hmc7044LliRegIoAct(TRUE, dev, regInx, pData)
*
* - description: as above
*
* - notes: see notes in the header of hmc7044LliRegIoAct
*******************************************************************************/
LOCAL STATUS hmc7044LliRegRead(CKDST_DEV dev, unsigned regInx,
		 HMC7044_REG *pData)
{

    return hmc7044LliRegIoAct(TRUE, dev, regInx, pData);

}




/*******************************************************************************
* - name: hmc7044LliRegWrite
*
* - title: perform a direct write to a device register via SPI
*
* - input: dev     - CLKDST device for which to perform the operation
*          regInx  - CLKDST register to write
*          regData - raw data to write to the register
*
* - returns: status returned from the call
*            hmc7044LliRegIoAct(FALSE, dev, regInx, &regData)
*
* - description: as above
*
* - notes: see notes in the header of hmc7044LliRegIoAct
*******************************************************************************/
LOCAL STATUS hmc7044LliRegWrite(CKDST_DEV dev, unsigned regInx, HMC7044_REG regData)
{
	return hmc7044LliRegIoAct(FALSE, dev, regInx, &regData);
}




/*******************************************************************************
* - name: hmc7044LliRegIoAct
*
* - title: read/write a device register via SPI
*
* - input: doRead - TRUE / FALSE to perform read / write, respectively
*          dev    - CLKDST device for which to perform the operation
*          regInx - CLKDST register to read / write
*          pData  - pointer to register data
*
* - output: *pData (only for a read operation)
*
* - returns: OK or ERROR if detected an error (in which case *pData is unusable)
*
* - description: as above
*
* - notes: 1) The operation is interlocked via the associated critical section.
*          2) It is assumed that hmc7044LliInit has already been invoked.
*******************************************************************************/
LOCAL STATUS hmc7044LliRegIoAct(Bool doRead, CKDST_DEV dev, unsigned regInx,
		 HMC7044_REG *pData)
{

    STATUS status;
    const Hmc7044_dev_io_if *pCtl;

    /* validate arguments and initialize */
    if (!inEnumRange(dev, NELEMENTS(hmc7044LliCtl.devCtl))
        || regInx > HMC7044_REG_INX_MAX || !pData) {
        sysLog("invalid argument(s) (doRead %d, dev %u, regInx %u, pData %d)",
               doRead, dev, regInx, pData != NULL);
        return ERROR;
    }

    pCtl = &hmc7044LliCtl.devCtl[dev].ioIf;
    if (!hmc7044IfCtl.initDone || !hmc7044LliCtl.initDone || !pCtl->pRegRead ||
        !pCtl->pRegWrite) {
        sysLog("subsystem initialization not done yet (initDone %d, pRegRead %d, "
               "pRegWrite %d, doRead %d, dev %u, regInx %u)",
               hmc7044LliCtl.initDone, pCtl->pRegRead != NULL,
               pCtl->pRegWrite != NULL, doRead, dev, regInx);

        return ERROR;
    }

    if (doRead) {
        status = pCtl->pRegRead(dev, regInx, pData);
    } else {
        /* perform the operation (in an interlocked fashion) */
        hmc7044CsEnter(dev, __FUNCTION__);
        status = pCtl->pRegWrite(dev, regInx, *pData);
        hmc7044CsExit(dev, __FUNCTION__);
    }

    /* analyze results */
    if (status != OK) {
        sysLog("operation failed (doRead %d, dev %u, regInx 0x%02x, "
               "regData 0x%02x)", doRead, dev, regInx, *pData);
        return ERROR;
    }

    return OK;

}


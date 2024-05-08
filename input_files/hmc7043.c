/*******************************************************************************
* hmc7043.c - Interface to clock distribution device, HMC7043 *
********************************************************************************
* modification history:                                                        *
*   01.04.24 , created                                                       *
*******************************************************************************/

#include <stdio.h>
#include <string.h>
#include "sysutil.h"
#include "hmc7043.h"


/* Constants and Types */
#define HMC7043_REG_INX_MIN 0
#define HMC7043_REG_INX_MAX 0x0152
#define HMC7043_CH_OUT_MIN 0
#define HMC7043_CH_OUT_MAX 13
#define HMC7043_CID1_MIN_FREQ 2e6
#define HMC7043_CID1_MAX_FREQ 32e8
#define HMC7043_CID2_MIN_FREQ 2e6
#define HMC7043_CID2_MAX_FREQ 6e9
#define HMC7043_PRD_ID 0x301651
#define HMC7043_REG_IDX_SOFT_RESET 0x0000
#define HMC7043_REG_IDX_REQ_MOD 0x0001
#define HMC7043_REG_IDX_SLIP_REQ 0x0002
#define HMC7043_FSM_DIV_RESET_BIT 1
#define HMC7043_RESEED_BIT 7
#define HMC7043_SFT_RST_BIT 0
#define HMC7043_PULS_GEN_BIT 2
#define HMC7043_SLIP_REQ_BIT 1
#define HMC7043_RSVD_VAL1 0x00
#define HMC7043_RSVD_VAL2 0x0
#define HMC7043_INIT_WAIT_TIMES 6
#define HMC7043_LSB_BIT_VAL(val) (val & 0xff)
#define HMC7043_MSB_BIT_VAL(val) (val >> 8)
#define HMC7043_ADLY_MAX_STEPS 23
#define HMC7043_ADLY_STEP_SIZE 25

typedef struct {
    Bool initDone;
    HUTL_MUTEX hMutex;
} Hmc7043_dev_ctl;

typedef struct {
    Hmc7043_dev_io_if ioIf;
} Hmc7043_lli_dev_ctl;


/* Control Data */
LOCAL struct {
    Bool initDone;
    CKDST_DEV_MASK devMask;
    Hmc7043_lli_dev_ctl devCtl[CKDST_MAX_NDEV];
} hmc7043LliCtl;

LOCAL struct {
    Bool initDone;  /* relying on static initialization of this to FALSE */
    CKDST_DEV_MASK devMask;
    Hmc7043_dev_ctl devCtl[CKDST_MAX_NDEV];
} hmc7043IfCtl;

typedef struct {
    Bool initDone;   /* relying on static initialization of this to FALSE */
    Hmc7043_app_dev_params params;
} Hmc7043_app_dev_ctl;

LOCAL struct {
    Bool initDone;  /* relying on static init. of this to FALSE */
    CKDST_FREQ_HZ lwstOutFreq;  /* Lowest output frequency in clock network*/
    Hmc7043_app_dev_ctl devCtl[CKDST_MAX_NDEV];
} hmc7043AppCtl;

/* forward references */
LOCAL STATUS hmc7043LliInit(CKDST_DEV_MASK devMask);
LOCAL STATUS hmc7043LliInitDev(CKDST_DEV dev, const Hmc7043_dev_io_if *pIf,
                               Bool warmInit);
LOCAL STATUS hmc7043LliRegRead(CKDST_DEV dev, unsigned regInx,
                               HMC7043_REG *pData);
LOCAL STATUS hmc7043LliRegWrite(CKDST_DEV dev, unsigned regInx,
                                HMC7043_REG regData);
LOCAL STATUS hmc7043CsEnter(CKDST_DEV dev, const char *context);
LOCAL STATUS hmc7043CsExit(CKDST_DEV dev, const char *context);
LOCAL STATUS hmc7043AppIfInit(void);
LOCAL STATUS hmc7043AppSetUpDevCtl(CKDST_DEV dev,
                                   const Hmc7043_app_dev_params *pParams);
STATUS hmc7043AppInitDev(CKDST_DEV dev, const Hmc7043_app_dev_params *pParams,
		                       Bool warmInit);
LOCAL STATUS hmc7043AppChkProdId(CKDST_DEV dev);
LOCAL STATUS hmc7043LoadConfigUpd(CKDST_DEV dev);


/* Dummy function for compilation: to be removed */
STATUS sysLogIntFun (unsigned, const char *, const char *, ...)
{
	return OK;
}
STATUS sysLogLongFun(unsigned, const char *, const char *, ...)
{
	return OK;
}
STATUS utlMutexRelease(HUTL_MUTEX, const char *)
{
	return OK;
}
STATUS utlMutexTake(HUTL_MUTEX, const char *)
{
    return OK;
}
HUTL_MUTEX utlMutexCreate(SYS_TIME)
{
    return UTL_MUTEX_BAD_HMUTEX;
}
void sysCodeErr(CODE_ERROR_ID, FUNCPTR, const char *,UINT64, UINT64, UINT64)
{
	return;
}
STATUS sysLogFpaFun (unsigned, const char*, const char*, ...)
{
	return OK;
}
void sysDelayUsec(UINT64)
{
    return;
}
/*#############################################################################*
*    I N I T I A L I Z A T I O N    A N D    O V E R A L L    C O N T R O L    *
*#############################################################################*/

/*******************************************************************************
* - name: hmc7043IfInit
*
* - title: initialize HMC7043 CLKDST control interface
*
* - input: devMask   - specifies the CLKDST device(s) that will be used
*
* - output: hmc7043IfCtl
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - notes: this routine can be called more than once
*******************************************************************************/
STATUS hmc7043IfInit(CKDST_DEV_MASK devMask)
{
    unsigned i = 0;

    if (!devMask || devMask >= 1 << NELEMENTS(hmc7043IfCtl.devCtl)) {
        sysLog("bad argument(s) (devMask 0x%x)", &devMask != NULL);
        return ERROR;
    }

    hmc7043IfCtl.devMask  = devMask;

    for (i = 0; i < NELEMENTS(hmc7043IfCtl.devCtl); ++i) {
        Hmc7043_dev_ctl *pDev = hmc7043IfCtl.devCtl + i;

        pDev->initDone = FALSE;
        pDev->hMutex   = UTL_MUTEX_BAD_HMUTEX;
    }

    hmc7043IfCtl.initDone = TRUE;

    if (hmc7043LliInit(devMask) != OK)
        return ERROR;

    /* initialize application-level interface */
    if (hmc7043AppIfInit() != OK)
        return ERROR;

    return OK;
}
/*******************************************************************************
* - name: hmc7043InitDev
*
* - title: initialize the specific CLKDST device
*
* - input: dev      - CLKDST to be initialized
*          pIf      - pointer to low-level interface access-related parameters
*          pParams  - application-level device setup parameters
*          warminit - if set, will skip actual device initialization
*
* - returns: OK or ERROR if detected an error (if at all)
*
* - description: as above
*******************************************************************************/
STATUS hmc7043InitDev(CKDST_DEV dev, const Hmc7043_dev_io_if *pIf,
                      const Hmc7043_app_dev_params *pParams, Bool warmInit)
{
    static const SYS_TIME MUTEX_TIMEOUT = 200;  /* msec; adequately large */

    Hmc7043_dev_ctl *pCtl;

    /* initialize */
    STATUS status = OK;  /* initial assumption */

    if (!inEnumRange(dev, NELEMENTS(hmc7043IfCtl.devCtl)) || !pIf || !pParams) {
        sysLog("bad argument(s) (dev %d, pIf %d, pParams %d)", dev, pIf != NULL,
               pParams != NULL);
        return ERROR;
    }

    if (!hmc7043IfCtl.initDone) {
        sysLog("interface not initialized yet (dev %d)", dev);
        return ERROR;
    }

    pCtl = hmc7043IfCtl.devCtl + dev;

    if (pCtl->initDone && pCtl->hMutex == UTL_MUTEX_BAD_HMUTEX) {
        sysLog("bad mutex (dev %d)", dev);
/*#ifdef COMMENT   attempt to recover ---- TBD*/
        //return ERROR;
//#endif
    }

    /* create the associated mutex (if first time here) */
    if (pCtl->hMutex == UTL_MUTEX_BAD_HMUTEX &&
        (pCtl->hMutex = utlMutexCreate(MUTEX_TIMEOUT)) ==
        UTL_MUTEX_BAD_HMUTEX) {
        sysLog("mutex creation failed (dev %d)", dev);
        return ERROR;
    }

    pCtl->initDone = TRUE;  /* must be done before the subsequent code */

    /* perform actual initialization */
    hmc7043CsEnter(dev, __FUNCTION__);

    if (hmc7043LliInitDev(dev, pIf, warmInit) != OK)
        status = ERROR;
    else if (hmc7043AppInitDev(dev, pParams, warmInit) != OK)
        status = ERROR;

    hmc7043CsExit(dev, __FUNCTION__);

    return status;
}

/*******************************************************************************
* - name: hmc7043CsEnter
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
LOCAL STATUS hmc7043CsEnter(CKDST_DEV dev, const char *context)
{
    const Hmc7043_dev_ctl *pCtl;

    /* initialize */
    context = context ? context : "???";

    if (!inEnumRange(dev, NELEMENTS(hmc7043IfCtl.devCtl))) {
        sysLogLong(" (from '%s'): bad argument(s) (dev %ld)", context,
                   (long) dev);
        return ERROR;
    }

    pCtl = hmc7043IfCtl.devCtl + dev;

    if (!pCtl->initDone || pCtl->hMutex == UTL_MUTEX_BAD_HMUTEX) {
        sysLogLong(" (from '%s'): bad state for dev %ld (initDone %ld, "
                   "hMutex %ld)", context, (long) dev, (long) pCtl->initDone,
                   (long) (pCtl->hMutex != UTL_MUTEX_BAD_HMUTEX));
        return ERROR;
    }

    if (utlMutexTake(pCtl->hMutex, context) != OK) {
        sysCodeError(CODE_ERR_STATE, hmc7043CsEnter, context, dev, -1);
        return ERROR;
    }

    return OK;
}

/*******************************************************************************
* - name: hmc7043CsExit
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
LOCAL STATUS hmc7043CsExit(CKDST_DEV dev, const char *context)
{
    const Hmc7043_dev_ctl *pCtl;

    /* initialize */
    context = context ? context : "???";

    if (!inEnumRange(dev, NELEMENTS(hmc7043IfCtl.devCtl))) {
        sysLogLong(" (from '%s'): bad argument(s) (dev %ld)", context,
                   (long) dev);
        return ERROR;
    }

    pCtl = hmc7043IfCtl.devCtl + dev;

    if (!pCtl->initDone || pCtl->hMutex == UTL_MUTEX_BAD_HMUTEX) {
        sysLogLong(" (from '%s'): bad state for dev %ld (initDone %ld, "
                   "hMutex %ld)", context, (long) dev, (long) pCtl->initDone,
                   (long) (pCtl->hMutex != UTL_MUTEX_BAD_HMUTEX));
        return ERROR;
    }

    return utlMutexRelease(pCtl->hMutex, context);
}

/*******************************************************************************
* - name: hmc7043RegRead
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
* - notes: HMC7043 registers are 8-bit wide
*******************************************************************************/
EXPORT STATUS hmc7043RegRead(CKDST_DEV dev, unsigned regInx, HMC7043_REG *pData)
{
    HMC7043_REG regData;

    if (hmc7043LliRegRead(dev, regInx, &regData) != OK)
        return ERROR;

    *pData = regData;

    return OK;
}

/*******************************************************************************
* - name: hmc7043RegWrite
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
* - notes: HMC7043 registers are 8-bit wide
*******************************************************************************/
EXPORT STATUS hmc7043RegWrite(CKDST_DEV dev, unsigned regInx, HMC7043_REG regData)
{
    return hmc7043LliRegWrite(dev, regInx, regData);
}


/*#############################################################################*
*           L O W - L E V E L    R E G I S T E R    I N T E R F A C E          *
*#############################################################################*/

/*******************************************************************************
* - name: hmc7043LliInit
*
* - title: initialize the low-level interface
*
* - input: devMask - specifies the device(s) that need to be initialized
*
* - output: hmc7043LliCtl
*
* - returns: OK or ERROR if detected an error
*
* - algorithm: as above
*
* - notes: not attempting to interlock this operation
*******************************************************************************/
LOCAL STATUS hmc7043LliInit(CKDST_DEV_MASK devMask)
{
    unsigned i;

    if (!devMask || devMask >= 1 << NELEMENTS(hmc7043LliCtl.devCtl)) {
        sysLog("bad argument (devMask 0x%x)", devMask);
        return ERROR;
    }

    /* initialize control data */
    hmc7043LliCtl.devMask = devMask;

    for (i = 0; i < NELEMENTS(hmc7043LliCtl.devCtl); ++i)
        memset(&hmc7043LliCtl.devCtl[i], 0, sizeof(hmc7043LliCtl.devCtl[i]));

    hmc7043LliCtl.initDone = TRUE;

    return OK;
}

/*******************************************************************************
* - name: hmc7043LliInitDev
*
* - title: initialize a specific CLKDST device
*
* - input: dev      - CLKDST device for which to perform the operation
*          pIf      - pointer to low-level interface access-related parameters
*          warmInit - warm initialization flag (currently unused)
*
* - output: hmc7043LliCtl.devCtl[dev]
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - note: not attempting to interlock the operation
*******************************************************************************/
LOCAL STATUS hmc7043LliInitDev(CKDST_DEV dev, const Hmc7043_dev_io_if *pIf,
                               Bool warmInit)
{
    UNREFERENCED_PARAMETER(warmInit);

    /* validate arguments and initialize */
    if (!inEnumRange(dev, NELEMENTS(hmc7043LliCtl.devCtl)) || !pIf) {
        sysLog("bad argument(s) (1, dev %d, pIf %d, warmInit %d)", dev,
               pIf != NULL, warmInit);
        return ERROR;
    }

    if (!pIf->pRegRead || !pIf->pRegWrite) {
        sysLog("bad argument(s) (2, dev %d, pRegRead %d, pRegWrite %d)",
        		dev, (pIf->pRegRead != NULL),
               (pIf->pRegWrite != NULL));
        return ERROR;
    }

    if (!hmc7043LliCtl.initDone) {
        sysLog("subsystem initialization not done yet (dev %d, warmInit %d)", dev,
               warmInit);
        return ERROR;
    }

    if (!((1 << dev) & hmc7043LliCtl.devMask)) {
        sysLog("unexpected device (%d; devMask 0x%08x)", dev,
               hmc7043LliCtl.devMask);
        return ERROR;
    }

    /* set up control data */
    hmc7043LliCtl.devCtl[dev].ioIf = *pIf;

    return OK;
}

/*******************************************************************************
* - name: hmc7043LliRegIoAct
*
* - title: rad/write a device register via SPI
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
*          2) It is assumed that hmc7043LliInit has already been invoked.
*******************************************************************************/
LOCAL STATUS hmc7043LliRegIoAct(Bool doRead, CKDST_DEV dev, unsigned regInx,
                                HMC7043_REG *pData)
{
    STATUS status;
    const Hmc7043_dev_io_if *pCtl;

    /* validate arguments and initialize */
    if (!inEnumRange(dev, NELEMENTS(hmc7043LliCtl.devCtl)) ||
        regInx < HMC7043_REG_INX_MIN || regInx > HMC7043_REG_INX_MAX || !pData) {
        sysLog("invalid argument(s) (doRead %d, dev %d, regInx %u, pData %d)",
               doRead, dev, regInx, pData != NULL);
        return ERROR;
    }

    pCtl = &hmc7043LliCtl.devCtl[dev].ioIf;

    if (!hmc7043IfCtl.initDone || !hmc7043LliCtl.initDone || !pCtl->pRegRead ||
            !pCtl->pRegWrite) {
            sysLog("subsystem initialization not done yet (initDone %d,"
            		" pRegRead %d, pRegWrite %d, doRead %d, dev %d, regInx %u)",
                   hmc7043LliCtl.initDone, pCtl->pRegRead != NULL,
                   pCtl->pRegWrite != NULL, doRead, dev, regInx);

            return ERROR;
    }

    hmc7043CsEnter(dev, __FUNCTION__);

    status = doRead ? pCtl->pRegRead (dev, regInx, pData) :
                          pCtl->pRegWrite(dev, regInx, *pData);

    hmc7043CsExit(dev, __FUNCTION__);

    /* analyze results */
    if (status != OK) {
        sysLog("operation failed (doRead %d, dev %d, regInx 0x%02x, "
                "regData 0x%02x)", doRead, dev, regInx, *pData);
        return ERROR;
    }

    return OK;
}

/*******************************************************************************
* - name: hmc7043LliRegRead
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
*            hmc7043LliRegIoAct(TRUE, dev, regInx, pData)
*
* - description: as above
*
* - notes: see notes in the header of hmc7043LliRegIoAct
*******************************************************************************/
LOCAL STATUS hmc7043LliRegRead(CKDST_DEV dev, unsigned regInx, HMC7043_REG *pData)
{
    return hmc7043LliRegIoAct(TRUE, dev, regInx, pData);
}

/*******************************************************************************
* - name: hmc7043LliRegWrite
*
* - title: perform a direct write to a device register via SPI
*
* - input: dev     - CLKDST device for which to perform the operation
*          regInx  - CLKDST register to write
*          regData - raw data to write to the register
*
* - returns: status returned from the call
*            hmc7043LliRegIoAct(FALSE, dev, regInx, &regData)
*
* - description: as above
*
* - notes: see notes in the header of hmc7043LliRegIoAct
*******************************************************************************/
LOCAL STATUS hmc7043LliRegWrite(CKDST_DEV dev, unsigned regInx, HMC7043_REG regData)
{
    return hmc7043LliRegIoAct(FALSE, dev, regInx, &regData);
}



/*#############################################################################*
*             R E G I S T E R    L A Y O U T    D E F I N I T I O N            *
*                                                                              *
* Notes:                                                                       *
* ------                                                                       *
* 1) It is assumed that this code executes on a Little-endian processor, thus  *
*    all bit fields are allocated starting from the LSB.                       *
* 2) All unnamed fields must be set to 0 on write.                             *
*#############################################################################*/

/* constants and types */
#pragma pack(push)
#pragma pack(1)

typedef union {
    struct {
            HMC7043_REG softReset : 1;  /* Soft Reset */
            HMC7043_REG reserved  : 7;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0000;


typedef union {
    struct {
            HMC7043_REG sleepMode   : 1;
            HMC7043_REG rstDivFsm   : 1;  /* Restart dividers/FSMs */
            HMC7043_REG pulsGenReq  : 1;  /* Pulse generation Request */
            HMC7043_REG mutOutDrv   : 1;  /* Mute Output Driver */
            HMC7043_REG reserved    : 2;
            HMC7043_REG highPrfPath : 1;  /* High performance path */
            HMC7043_REG reseedReq   : 1;  /* Reseed Request */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0001;



typedef union {
    struct {
            HMC7043_REG reserved1  : 1;
            HMC7043_REG mulSlipReq : 1;  /* Multi Slip Request */
            HMC7043_REG reserved2  : 6;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0002;


typedef union {
    struct {
            HMC7043_REG reserved1 : 2;
            HMC7043_REG enSRTimer : 1;  /* Enable SYSREF Timer */
            HMC7043_REG reserved2 : 2;
            HMC7043_REG enRfResdr : 1;  /* Enable RF Reseeder */
            HMC7043_REG reserved3 : 2;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0003;


typedef union {
    struct {
            HMC7043_REG enAllOutCh : 7;  /* Seven Pairs of 14-Channel Outputs Enable */
            HMC7043_REG reserved   : 1;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0004;


typedef union {
    struct {
            HMC7043_REG : 8;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0005;


typedef union {
    struct {
            HMC7043_REG clrAlarms : 1;  /* Clear Alarms */
            HMC7043_REG reserved  : 7;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0006;


typedef union {
    struct {
            HMC7043_REG : 8;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0007;

typedef union {
    struct {
            HMC7043_REG : 8;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0008;


typedef union {
    struct {
            HMC7043_REG : 8;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0009;

/* CLKIN0/CLKIN0 input buffer control */

typedef union {
    struct {
            HMC7043_REG enBuff   : 1;  /* Enable Buffer */
            HMC7043_REG inBufMod : 4;  /* Input Buffer Mode */
            HMC7043_REG reserved : 3;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x000a;

/*  RFSYNCIN/RFSYNCIN Input Buffer Control */

typedef union {
    struct {
            HMC7043_REG enBuff   : 1;  /* Enable Buffer */
            HMC7043_REG inBufMod : 4;  /* Input Buffer Mode */
            HMC7043_REG reserved : 3;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x000b;

/* GPI control */
typedef union {
    struct {
            HMC7043_REG enGpi    : 1;  /* Enable GPI */
            HMC7043_REG selGpi   : 4;  /* Select GPI */
            HMC7043_REG reserved : 3;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0046;

/* GPO Control */

typedef union {
    struct {
            HMC7043_REG enGp0    : 1;  /* Enable GPO */
            HMC7043_REG gpoMod   : 1;  /* GPO Mode*/
            HMC7043_REG selGpo   : 5;  /* GPO Selection */
            HMC7043_REG reserved : 1;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0050;

/* SDATA Control */

typedef union {
    struct {
            HMC7043_REG enSdata  : 1;  /* Enable SDATA */
            HMC7043_REG sdataMod : 1;  /* SDATA Mode*/
            HMC7043_REG reserved : 6;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0054;

/* Pulse Generator Control */

typedef union {
    struct {
            HMC7043_REG pulseMode : 3;  /* Pulse Generation Mode */
            HMC7043_REG reserved  : 5;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x005a;

/* SYNC Control */

typedef union {
    struct {
            HMC7043_REG syncInvPol : 1;  /* SYNC Invert Polarity */
            HMC7043_REG reserved1   : 1;
            HMC7043_REG syncRetime : 1;  /* SYNC Retime*/
            HMC7043_REG reserved2   : 5;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x005b;

/* SYSREF Timer Control */

typedef union {
    struct {
            HMC7043_REG timer : 8;  /* SYSREF Timer[7:0] (LSB) */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x005c;


typedef union {
    struct {
            HMC7043_REG timer    : 4;  /* SYSREF Timer[11:8](MSB) */
            HMC7043_REG reserved : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x005d;

/* Clock Input Control */

typedef union {
    struct {
            HMC7043_REG lfClkInp   : 1;  /* Low frequency clock input */
            HMC7043_REG divB2ClkIn : 1;  /* Divide by 2 on clock input */
            HMC7043_REG reserved   : 6;

    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0064;

/* Analog Delay Common Control */

typedef union {
    struct {
            HMC7043_REG aDelLowPowMo : 1;  /* Analog delay low power mode */
            HMC7043_REG reserved     : 7;

    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0065;

/* Alarm Mask Control */

typedef union {
    struct {
            HMC7043_REG reserved1  : 1;
            HMC7043_REG srSynStMsk : 1;  /* SYSREF sync status mask */
            HMC7043_REG cophStMsk  : 1;  /* Clock output phase status mask */
            HMC7043_REG reserved2  : 1;
            HMC7043_REG syncReqMsk : 1;  /* Sync request mask */
            HMC7043_REG reserved3  : 3;

    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0071;

/* Product ID Registers */

typedef union {
    struct {
            HMC7043_REG pIdLsb : 8;  /* Product ID Value (7:0) LSB */

    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0078;

typedef union {
    struct {
            HMC7043_REG pIdMid : 8;  /* Product ID Value (7:0) Mid */

    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0079;

typedef union {
    struct {
            HMC7043_REG pIdMsb : 8;  /* Product ID Value (7:0) MSB */

    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x007a;

/* Readback Register */

typedef union {
    struct {
            HMC7043_REG almSig   : 1;  /* Alarm Signal */
            HMC7043_REG reserved : 7;

    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x007b;

/* Alarm Readback */

typedef union {
    struct {
            HMC7043_REG reserved1 : 1;
            HMC7043_REG srSynSt   : 1;  /* SYSREF sync status */
            HMC7043_REG ckOutPhSt : 1;  /* Clock output phase status */
            HMC7043_REG reserved2 : 1;
            HMC7043_REG synReqSt  : 1;  /* SYNC request status */
            HMC7043_REG reserved3 : 3;

    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x007d;

/* Alarm Readback */

typedef union {
    struct {
            HMC7043_REG : 8;

    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x007f;


/* SYSREF Status Register */

typedef union {
    struct {
            HMC7043_REG srFsmSt      : 3;  /* SYSREF FSM state */
            HMC7043_REG chOutFsmBusy : 1;  /* Channel outputs FSM Busy */
            HMC7043_REG reserved     : 3;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0091;

/* Other Controls */

typedef union {
    struct {
            HMC7043_REG : 8;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0098;

typedef union {
    struct {
            HMC7043_REG : 8;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0099;

typedef union {
    struct {
            HMC7043_REG : 8;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x009d;

typedef union {
    struct {
            HMC7043_REG : 8;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x009e;

typedef union {
    struct {
            HMC7043_REG : 8;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x009f;

typedef union {
    struct {
            HMC7043_REG : 8;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00a0;

typedef union {
    struct {
            HMC7043_REG : 8;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00a2;

typedef union {
    struct {
            HMC7043_REG : 8;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00a3;

typedef union {
    struct {
            HMC7043_REG : 8;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00a4;

typedef union {
    struct {
            HMC7043_REG : 8;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00ad;

typedef union {
    struct {
            HMC7043_REG : 8;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00b5;

typedef union {
    struct {
            HMC7043_REG : 8;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00b6;

typedef union {
    struct {
            HMC7043_REG : 8;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00b7;

typedef union {
    struct {
            HMC7043_REG : 8;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00b8;

/* Clock Distribution */
/* Channel Output0 control */

typedef union {
    struct {
            HMC7043_REG chEn_0      : 1;  /* Channel Enable */
            HMC7043_REG multSlpEn_0 : 1;  /* MultiSlip Enable */
            HMC7043_REG stMod_0     : 2;  /* Start-up mode */
            HMC7043_REG reserved    : 1;
            HMC7043_REG slipEn_0    : 1;  /* Slip Enable */
            HMC7043_REG syncEn_0    : 1;  /* SYNC Enable */
            HMC7043_REG hpMode_0    : 1;  /* High Performance mode */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00c8;

typedef union {
    struct {
            HMC7043_REG chDivLsb_0 : 8;  /* Channel Divider[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00c9;

typedef union {
    struct {
            HMC7043_REG chDivMsb_0 : 4;  /* Channel Divider[11:8] MSB */
            HMC7043_REG reserved   : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00ca;

typedef union {
    struct {
            HMC7043_REG faDelay_0 : 4;  /* Fine analog Delay */
            HMC7043_REG reserved  : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00cb;

typedef union {
    struct {
            HMC7043_REG cdDelay_0 : 4;  /* Coarse Digital Delay */
            HMC7043_REG reserved  : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00cc;

typedef union {
    struct {
            HMC7043_REG msDelayLsb_0 : 8;  /* MultiSlip Digital Delay[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00cd;

typedef union {
    struct {
            HMC7043_REG msDelayMsb_0 : 4;  /* MultiSlip Digital Delay[11:8] MSB */
            HMC7043_REG reserved     : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00ce;

typedef union {
    struct {
            HMC7043_REG outMuxSel_0 : 2;  /* Output Mux Selection */
            HMC7043_REG reserved    : 6;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00cf;

typedef union {
    struct {
            HMC7043_REG drvImp_0    : 2;  /* Driver Impedance */
            HMC7043_REG reserved    : 1;  /* MultiSlip Enable */
            HMC7043_REG drvMod_0    : 2;  /* Driver mode */
            HMC7043_REG dyDrvEn_0   : 1;  /* Dynamic Driver Enable */
            HMC7043_REG idlAtZero_0 : 2;  /* Slip Enable */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00d0;


/* Channel Output1 control */

typedef union {
    struct {
            HMC7043_REG chEn_1      : 1;  /* Channel Enable */
            HMC7043_REG multSlpEn_1 : 1;  /* MultiSlip Enable */
            HMC7043_REG stMod_1     : 2;  /* Start-up mode */
            HMC7043_REG reserved    : 1;
            HMC7043_REG slipEn_1    : 1;  /* Slip Enable */
            HMC7043_REG syncEn_1    : 1;  /* SYNC Enable */
            HMC7043_REG hpMode_1    : 1;  /* High Performance mode */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00d2;

typedef union {
    struct {
            HMC7043_REG chDivLsb_1 : 8;  /* Channel Divider[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00d3;

typedef union {
    struct {
            HMC7043_REG chDivMsb_1 : 4;  /* Channel Divider[11:8] MSB */
            HMC7043_REG reserved   : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00d4;

typedef union {
    struct {
            HMC7043_REG faDelay_1 : 4;  /* Fine analog Delay */
            HMC7043_REG reserved  : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00d5;

typedef union {
    struct {
            HMC7043_REG cdDelay_1 : 4;  /* Coarse Digital Delay */
            HMC7043_REG reserved  : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00d6;

typedef union {
    struct {
            HMC7043_REG msDelayLsb_1 : 8;  /* MultiSlip Digital Delay[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00d7;

typedef union {
    struct {
            HMC7043_REG msDelayMsb_1 : 4;  /* MultiSlip Digital Delay[11:8] MSB */
            HMC7043_REG reserved     : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00d8;

typedef union {
    struct {
            HMC7043_REG outMuxSel_1 : 2;  /* Output Mux Selection */
            HMC7043_REG reserved    : 6;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00d9;

typedef union {
    struct {
            HMC7043_REG drvImp_1    : 2;  /* Driver Impedance */
            HMC7043_REG reserved    : 1;  /* MultiSlip Enable */
            HMC7043_REG drvMod_1    : 2;  /* Driver mode */
            HMC7043_REG dyDrvEn_1   : 1;  /* Dynamic Driver Enable */
            HMC7043_REG idlAtZero_1 : 2;  /* Slip Enable */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00da;


/* Channel Output2 control */

typedef union {
    struct {
            HMC7043_REG chEn_2      : 1;  /* Channel Enable */
            HMC7043_REG multSlpEn_2 : 1;  /* MultiSlip Enable */
            HMC7043_REG stMod_2     : 2;  /* Start-up mode */
            HMC7043_REG reserved    : 1;
            HMC7043_REG slipEn_2    : 1;  /* Slip Enable */
            HMC7043_REG syncEn_2    : 1;  /* SYNC Enable */
            HMC7043_REG hpMode_2    : 1;  /* High Performance mode */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00dc;

typedef union {
    struct {
            HMC7043_REG chDivLsb_2 : 8;  /* Channel Divider[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00dd;

typedef union {
    struct {
            HMC7043_REG chDivMsb_2 : 4;  /* Channel Divider[11:8] MSB */
            HMC7043_REG reserved   : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00de;

typedef union {
    struct {
            HMC7043_REG faDelay_2 : 4;  /* Fine analog Delay */
            HMC7043_REG reserved  : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00df;

typedef union {
    struct {
            HMC7043_REG cdDelay_2 : 4;  /* Coarse Digital Delay */
            HMC7043_REG reserved  : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00e0;

typedef union {
    struct {
            HMC7043_REG msDelayLsb_2 : 8;  /* MultiSlip Digital Delay[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00e1;

typedef union {
    struct {
            HMC7043_REG msDelayMsb_2 : 4;  /* MultiSlip Digital Delay[11:8] MSB */
            HMC7043_REG reserved     : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00e2;

typedef union {
    struct {
            HMC7043_REG outMuxSel_2 : 2;  /* Output Mux Selection */
            HMC7043_REG reserved    : 6;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00e3;

typedef union {
    struct {
            HMC7043_REG drvImp_2    : 2;  /* Driver Impedance */
            HMC7043_REG reserved    : 1;  /* MultiSlip Enable */
            HMC7043_REG drvMod_2    : 2;  /* Driver mode */
            HMC7043_REG dyDrvEn_2   : 1;  /* Dynamic Driver Enable */
            HMC7043_REG idlAtZero_2 : 2;  /* Slip Enable */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00e4;


/* Channel Output3 control */

typedef union {
    struct {
            HMC7043_REG chEn_3      : 1;  /* Channel Enable */
            HMC7043_REG multSlpEn_3 : 1;  /* MultiSlip Enable */
            HMC7043_REG stMod_3     : 2;  /* Start-up mode */
            HMC7043_REG reserved    : 1;
            HMC7043_REG slipEn_3    : 1;  /* Slip Enable */
            HMC7043_REG syncEn_3    : 1;  /* SYNC Enable */
            HMC7043_REG hpMode_3    : 1;  /* High Performance mode */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00e6;

typedef union {
    struct {
            HMC7043_REG chDivLsb_3 : 8;  /* Channel Divider[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00e7;

typedef union {
    struct {
            HMC7043_REG chDivMsb_3 : 4;  /* Channel Divider[11:8] MSB */
            HMC7043_REG reserved   : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00e8;

typedef union {
    struct {
            HMC7043_REG faDelay_3 : 4;  /* Fine analog Delay */
            HMC7043_REG reserved  : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00e9;

typedef union {
    struct {
            HMC7043_REG cdDelay_3 : 4;  /* Coarse Digital Delay */
            HMC7043_REG reserved  : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00ea;

typedef union {
    struct {
            HMC7043_REG msDelayLsb_3 : 8;  /* MultiSlip Digital Delay[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00eb;

typedef union {
    struct {
            HMC7043_REG msDelayMsb_3 : 4;  /* MultiSlip Digital Delay[11:8] MSB */
            HMC7043_REG reserved     : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00ec;

typedef union {
    struct {
            HMC7043_REG outMuxSel_3 : 2;  /* Output Mux Selection */
            HMC7043_REG reserved    : 6;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00ed;

typedef union {
    struct {
            HMC7043_REG drvImp_3    : 2;  /* Driver Impedance */
            HMC7043_REG reserved    : 1;  /* MultiSlip Enable */
            HMC7043_REG drvMod_3    : 2;  /* Driver mode */
            HMC7043_REG dyDrvEn_3   : 1;  /* Dynamic Driver Enable */
            HMC7043_REG idlAtZero_3 : 2;  /* Slip Enable */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00ee;



/* Channel Output4 control */

typedef union {
    struct {
            HMC7043_REG chEn_4      : 1;  /* Channel Enable */
            HMC7043_REG multSlpEn_4 : 1;  /* MultiSlip Enable */
            HMC7043_REG stMod_4     : 2;  /* Start-up mode */
            HMC7043_REG reserved    : 1;
            HMC7043_REG slipEn_4    : 1;  /* Slip Enable */
            HMC7043_REG syncEn_4    : 1;  /* SYNC Enable */
            HMC7043_REG hpMode_4    : 1;  /* High Performance mode */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00f0;

typedef union {
    struct {
            HMC7043_REG chDivLsb_4 : 8;  /* Channel Divider[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00f1;

typedef union {
    struct {
            HMC7043_REG chDivMsb_4 : 4;  /* Channel Divider[11:8] MSB */
            HMC7043_REG reserved   : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00f2;

typedef union {
    struct {
            HMC7043_REG faDelay_4 : 4;  /* Fine analog Delay */
            HMC7043_REG reserved  : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00f3;

typedef union {
    struct {
            HMC7043_REG cdDelay_4 : 4;  /* Coarse Digital Delay */
            HMC7043_REG reserved  : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00f4;

typedef union {
    struct {
            HMC7043_REG msDelayLsb_4 : 8;  /* MultiSlip Digital Delay[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00f5;

typedef union {
    struct {
            HMC7043_REG msDelayMsb_4 : 4;  /* MultiSlip Digital Delay[11:8] MSB */
            HMC7043_REG reserved     : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00f6;

typedef union {
    struct {
            HMC7043_REG outMuxSel_4 : 2;  /* Output Mux Selection */
            HMC7043_REG reserved    : 6;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00f7;

typedef union {
    struct {
            HMC7043_REG drvImp_4    : 2;  /* Driver Impedance */
            HMC7043_REG reserved    : 1;  /* MultiSlip Enable */
            HMC7043_REG drvMod_4    : 2;  /* Driver mode */
            HMC7043_REG dyDrvEn_4   : 1;  /* Dynamic Driver Enable */
            HMC7043_REG idlAtZero_4 : 2;  /* Slip Enable */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00f8;


/* Channel Output5 control */

typedef union {
    struct {
            HMC7043_REG chEn_5      : 1;  /* Channel Enable */
            HMC7043_REG multSlpEn_5 : 1;  /* MultiSlip Enable */
            HMC7043_REG stMod_5     : 2;  /* Start-up mode */
            HMC7043_REG reserved    : 1;
            HMC7043_REG slipEn_5    : 1;  /* Slip Enable */
            HMC7043_REG syncEn_5    : 1;  /* SYNC Enable */
            HMC7043_REG hpMode_5    : 1;  /* High Performance mode */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00fa;

typedef union {
    struct {
            HMC7043_REG chDivLsb_5 : 8;  /* Channel Divider[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00fb;

typedef union {
    struct {
            HMC7043_REG chDivMsb_5 : 4;  /* Channel Divider[11:8] MSB */
            HMC7043_REG reserved   : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00fc;

typedef union {
    struct {
            HMC7043_REG faDelay_5 : 4;  /* Fine analog Delay */
            HMC7043_REG reserved  : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00fd;

typedef union {
    struct {
            HMC7043_REG cdDelay_5 : 4;  /* Coarse Digital Delay */
            HMC7043_REG reserved  : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00fe;

typedef union {
    struct {
            HMC7043_REG msDelayLsb_5 : 8;  /* MultiSlip Digital Delay[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x00ff;

typedef union {
    struct {
            HMC7043_REG msDelayMsb_5 : 4;  /* MultiSlip Digital Delay[11:8] MSB */
            HMC7043_REG reserved     : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0100;

typedef union {
    struct {
            HMC7043_REG outMuxSel_5 : 2;  /* Output Mux Selection */
            HMC7043_REG reserved    : 6;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0101;

typedef union {
    struct {
            HMC7043_REG drvImp_5    : 2;  /* Driver Impedance */
            HMC7043_REG reserved    : 1;  /* MultiSlip Enable */
            HMC7043_REG drvMod_5    : 2;  /* Driver mode */
            HMC7043_REG dyDrvEn_5   : 1;  /* Dynamic Driver Enable */
            HMC7043_REG idlAtZero_5 : 2;  /* Slip Enable */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0102;


/* Channel Output6 control */

typedef union {
    struct {
            HMC7043_REG chEn_6      : 1;  /* Channel Enable */
            HMC7043_REG multSlpEn_6 : 1;  /* MultiSlip Enable */
            HMC7043_REG stMod_6     : 2;  /* Start-up mode */
            HMC7043_REG reserved    : 1;
            HMC7043_REG slipEn_6    : 1;  /* Slip Enable */
            HMC7043_REG syncEn_6    : 1;  /* SYNC Enable */
            HMC7043_REG hpMode_6    : 1;  /* High Performance mode */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0104;

typedef union {
    struct {
            HMC7043_REG chDivLsb_6 : 8;  /* Channel Divider[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0105;

typedef union {
    struct {
            HMC7043_REG chDivMsb_6 : 4;  /* Channel Divider[11:8] MSB */
            HMC7043_REG reserved   : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0106;

typedef union {
    struct {
            HMC7043_REG faDelay_6 : 4;  /* Fine analog Delay */
            HMC7043_REG reserved  : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0107;

typedef union {
    struct {
            HMC7043_REG cdDelay_6 : 4;  /* Coarse Digital Delay */
            HMC7043_REG reserved  : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0108;

typedef union {
    struct {
            HMC7043_REG msDelayLsb_6 : 8;  /* MultiSlip Digital Delay[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0109;

typedef union {
    struct {
            HMC7043_REG msDelayMsb_6 : 4;  /* MultiSlip Digital Delay[11:8] MSB */
            HMC7043_REG reserved     : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x010a;

typedef union {
    struct {
            HMC7043_REG outMuxSel_6 : 2;  /* Output Mux Selection */
            HMC7043_REG reserved    : 6;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x010b;

typedef union {
    struct {
            HMC7043_REG drvImp_6    : 2;  /* Driver Impedance */
            HMC7043_REG reserved    : 1;  /* MultiSlip Enable */
            HMC7043_REG drvMod_6    : 2;  /* Driver mode */
            HMC7043_REG dyDrvEn_6   : 1;  /* Dynamic Driver Enable */
            HMC7043_REG idlAtZero_6 : 2;  /* Slip Enable */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x010c;


/* Channel Output7 control */

typedef union {
    struct {
            HMC7043_REG chEn_7      : 1;  /* Channel Enable */
            HMC7043_REG multSlpEn_7 : 1;  /* MultiSlip Enable */
            HMC7043_REG stMod_7     : 2;  /* Start-up mode */
            HMC7043_REG reserved    : 1;
            HMC7043_REG slipEn_7    : 1;  /* Slip Enable */
            HMC7043_REG syncEn_7    : 1;  /* SYNC Enable */
            HMC7043_REG hpMode_7    : 1;  /* High Performance mode */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x010e;

typedef union {
    struct {
            HMC7043_REG chDivLsb_7 : 8;  /* Channel Divider[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x010f;

typedef union {
    struct {
            HMC7043_REG chDivMsb_7 : 4;  /* Channel Divider[11:8] MSB */
            HMC7043_REG reserved   : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0110;

typedef union {
    struct {
            HMC7043_REG faDelay_7 : 4;  /* Fine analog Delay */
            HMC7043_REG reserved  : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0111;

typedef union {
    struct {
            HMC7043_REG cdDelay_7 : 4;  /* Coarse Digital Delay */
            HMC7043_REG reserved  : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0112;

typedef union {
    struct {
            HMC7043_REG msDelayLsb_7 : 8;  /* MultiSlip Digital Delay[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0113;

typedef union {
    struct {
            HMC7043_REG msDelayMsb_7 : 4;  /* MultiSlip Digital Delay[11:8] MSB */
            HMC7043_REG reserved     : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0114;

typedef union {
    struct {
            HMC7043_REG outMuxSel_7 : 2;  /* Output Mux Selection */
            HMC7043_REG reserved    : 6;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0115;

typedef union {
    struct {
            HMC7043_REG drvImp_7    : 2;  /* Driver Impedance */
            HMC7043_REG reserved    : 1;  /* MultiSlip Enable */
            HMC7043_REG drvMod_7    : 2;  /* Driver mode */
            HMC7043_REG dyDrvEn_7   : 1;  /* Dynamic Driver Enable */
            HMC7043_REG idlAtZero_7 : 2;  /* Slip Enable */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0116;



/* Channel Output8 control */

typedef union {
    struct {
            HMC7043_REG chEn_8      : 1;  /* Channel Enable */
            HMC7043_REG multSlpEn_8 : 1;  /* MultiSlip Enable */
            HMC7043_REG stMod_8     : 2;  /* Start-up mode */
            HMC7043_REG reserved    : 1;
            HMC7043_REG slipEn_8    : 1;  /* Slip Enable */
            HMC7043_REG syncEn_8    : 1;  /* SYNC Enable */
            HMC7043_REG hpMode_8    : 1;  /* High Performance mode */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0118;

typedef union {
    struct {
            HMC7043_REG chDivLsb_8 : 8;  /* Channel Divider[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0119;

typedef union {
    struct {
            HMC7043_REG chDivMsb_8 : 4;  /* Channel Divider[11:8] MSB */
            HMC7043_REG reserved   : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x011a;

typedef union {
    struct {
            HMC7043_REG faDelay_8 : 4;  /* Fine analog Delay */
            HMC7043_REG reserved  : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x011b;

typedef union {
    struct {
            HMC7043_REG cdDelay_8 : 4;  /* Coarse Digital Delay */
            HMC7043_REG reserved  : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x011c;

typedef union {
    struct {
            HMC7043_REG msDelayLsb_8 : 8;  /* MultiSlip Digital Delay[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x011d;

typedef union {
    struct {
            HMC7043_REG msDelayMsb_8 : 4;  /* MultiSlip Digital Delay[11:8] MSB */
            HMC7043_REG reserved     : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x011e;

typedef union {
    struct {
            HMC7043_REG outMuxSel_8 : 2;  /* Output Mux Selection */
            HMC7043_REG reserved    : 6;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x011f;

typedef union {
    struct {
            HMC7043_REG drvImp_8    : 2;  /* Driver Impedance */
            HMC7043_REG reserved    : 1;  /* MultiSlip Enable */
            HMC7043_REG drvMod_8    : 2;  /* Driver mode */
            HMC7043_REG dyDrvEn_8   : 1;  /* Dynamic Driver Enable */
            HMC7043_REG idlAtZero_8 : 2;  /* Slip Enable */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0120;


/* Channel Output9 control */

typedef union {
    struct {
            HMC7043_REG chEn_9      : 1;  /* Channel Enable */
            HMC7043_REG multSlpEn_9 : 1;  /* MultiSlip Enable */
            HMC7043_REG stMod_9     : 2;  /* Start-up mode */
            HMC7043_REG reserved    : 1;
            HMC7043_REG slipEn_9    : 1;  /* Slip Enable */
            HMC7043_REG syncEn_9    : 1;  /* SYNC Enable */
            HMC7043_REG hpMode_9    : 1;  /* High Performance mode */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0122;

typedef union {
    struct {
            HMC7043_REG chDivLsb_9 : 8;  /* Channel Divider[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0123;

typedef union {
    struct {
            HMC7043_REG chDivMsb_9 : 4;  /* Channel Divider[11:8] MSB */
            HMC7043_REG reserved   : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0124;

typedef union {
    struct {
            HMC7043_REG faDelay_9 : 4;  /* Fine analog Delay */
            HMC7043_REG reserved  : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0125;

typedef union {
    struct {
            HMC7043_REG cdDelay_9 : 4;  /* Coarse Digital Delay */
            HMC7043_REG reserved  : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0126;

typedef union {
    struct {
            HMC7043_REG msDelayLsb_9 : 8;  /* MultiSlip Digital Delay[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0127;

typedef union {
    struct {
            HMC7043_REG msDelayMsb_9 : 4;  /* MultiSlip Digital Delay[11:8] MSB */
            HMC7043_REG reserved     : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0128;

typedef union {
    struct {
            HMC7043_REG outMuxSel_9 : 2;  /* Output Mux Selection */
            HMC7043_REG reserved    : 6;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0129;

typedef union {
    struct {
            HMC7043_REG drvImp_9    : 2;  /* Driver Impedance */
            HMC7043_REG reserved    : 1;  /* MultiSlip Enable */
            HMC7043_REG drvMod_9    : 2;  /* Driver mode */
            HMC7043_REG dyDrvEn_9   : 1;  /* Dynamic Driver Enable */
            HMC7043_REG idlAtZero_9 : 2;  /* Slip Enable */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x012a;


/* Channel Output10 control */

typedef union {
    struct {
            HMC7043_REG chEn_10      : 1;  /* Channel Enable */
            HMC7043_REG multSlpEn_10 : 1;  /* MultiSlip Enable */
            HMC7043_REG stMod_10     : 2;  /* Start-up mode */
            HMC7043_REG reserved     : 1;
            HMC7043_REG slipEn_10    : 1;  /* Slip Enable */
            HMC7043_REG syncEn_10    : 1;  /* SYNC Enable */
            HMC7043_REG hpMode_10    : 1;  /* High Performance mode */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x012c;

typedef union {
    struct {
            HMC7043_REG chDivLsb_10 : 8;  /* Channel Divider[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x012d;

typedef union {
    struct {
            HMC7043_REG chDivMsb_10 : 4;  /* Channel Divider[11:8] MSB */
            HMC7043_REG reserved    : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x012e;

typedef union {
    struct {
            HMC7043_REG faDelay_10 : 4;  /* Fine analog Delay */
            HMC7043_REG reserved   : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x012f;

typedef union {
    struct {
            HMC7043_REG cdDelay_10 : 4;  /* Coarse Digital Delay */
            HMC7043_REG reserved   : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0130;

typedef union {
    struct {
            HMC7043_REG msDelayLsb_10 : 8;  /* MultiSlip Digital Delay[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0131;

typedef union {
    struct {
            HMC7043_REG msDelayMsb_10 : 4;  /* MultiSlip Digital Delay[11:8] MSB */
            HMC7043_REG reserved      : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0132;

typedef union {
    struct {
            HMC7043_REG outMuxSel_10 : 2;  /* Output Mux Selection */
            HMC7043_REG reserved     : 6;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0133;

typedef union {
    struct {
            HMC7043_REG drvImp_10    : 2;  /* Driver Impedance */
            HMC7043_REG reserved     : 1;  /* MultiSlip Enable */
            HMC7043_REG drvMod_10    : 2;  /* Driver mode */
            HMC7043_REG dyDrvEn_10   : 1;  /* Dynamic Driver Enable */
            HMC7043_REG idlAtZero_10 : 2;  /* Slip Enable */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0134;


/* Channel Output11 control */

typedef union {
    struct {
            HMC7043_REG chEn_11      : 1;  /* Channel Enable */
            HMC7043_REG multSlpEn_11 : 1;  /* MultiSlip Enable */
            HMC7043_REG stMod_11     : 2;  /* Start-up mode */
            HMC7043_REG reserved     : 1;
            HMC7043_REG slipEn_11    : 1;  /* Slip Enable */
            HMC7043_REG syncEn_11    : 1;  /* SYNC Enable */
            HMC7043_REG hpMode_11    : 1;  /* High Performance mode */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0136;

typedef union {
    struct {
            HMC7043_REG chDivLsb_11 : 8;  /* Channel Divider[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0137;

typedef union {
    struct {
            HMC7043_REG chDivMsb_11 : 4;  /* Channel Divider[11:8] MSB */
            HMC7043_REG reserved    : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0138;

typedef union {
    struct {
            HMC7043_REG faDelay_11 : 4;  /* Fine analog Delay */
            HMC7043_REG reserved   : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0139;

typedef union {
    struct {
            HMC7043_REG cdDelay_11 : 4;  /* Coarse Digital Delay */
            HMC7043_REG reserved   : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x013a;

typedef union {
    struct {
            HMC7043_REG msDelayLsb_11 : 8;  /* MultiSlip Digital Delay[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x013b;

typedef union {
    struct {
            HMC7043_REG msDelayMsb_11 : 4;  /* MultiSlip Digital Delay[11:8] MSB */
            HMC7043_REG reserved      : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x013c;

typedef union {
    struct {
            HMC7043_REG outMuxSel_11 : 2;  /* Output Mux Selection */
            HMC7043_REG reserved     : 6;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x013d;

typedef union {
    struct {
            HMC7043_REG drvImp_11    : 2;  /* Driver Impedance */
            HMC7043_REG reserved     : 1;  /* MultiSlip Enable */
            HMC7043_REG drvMod_11    : 2;  /* Driver mode */
            HMC7043_REG dyDrvEn_11   : 1;  /* Dynamic Driver Enable */
            HMC7043_REG idlAtZero_11 : 2;  /* Slip Enable */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x013e;


/* Channel Output12 control */

typedef union {
    struct {
            HMC7043_REG chEn_12      : 1;  /* Channel Enable */
            HMC7043_REG multSlpEn_12 : 1;  /* MultiSlip Enable */
            HMC7043_REG stMod_12     : 2;  /* Start-up mode */
            HMC7043_REG reserved     : 1;
            HMC7043_REG slipEn_12    : 1;  /* Slip Enable */
            HMC7043_REG syncEn_12    : 1;  /* SYNC Enable */
            HMC7043_REG hpMode_12    : 1;  /* High Performance mode */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0140;

typedef union {
    struct {
            HMC7043_REG chDivLsb_12 : 8;  /* Channel Divider[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0141;

typedef union {
    struct {
            HMC7043_REG chDivMsb_12 : 4;  /* Channel Divider[11:8] MSB */
            HMC7043_REG reserved    : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0142;

typedef union {
    struct {
            HMC7043_REG faDelay_12 : 4;  /* Fine analog Delay */
            HMC7043_REG reserved   : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0143;

typedef union {
    struct {
            HMC7043_REG cdDelay_12 : 4;  /* Coarse Digital Delay */
            HMC7043_REG reserved   : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0144;

typedef union {
    struct {
            HMC7043_REG msDelayLsb_12 : 8;  /* MultiSlip Digital Delay[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0145;

typedef union {
    struct {
            HMC7043_REG msDelayMsb_12 : 4;  /* MultiSlip Digital Delay[11:8] MSB */
            HMC7043_REG reserved      : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0146;

typedef union {
    struct {
            HMC7043_REG outMuxSel_12 : 2;  /* Output Mux Selection */
            HMC7043_REG reserved     : 6;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0147;

typedef union {
    struct {
            HMC7043_REG drvImp_12    : 2;  /* Driver Impedance */
            HMC7043_REG reserved     : 1;  /* MultiSlip Enable */
            HMC7043_REG drvMod_12    : 2;  /* Driver mode */
            HMC7043_REG dyDrvEn_12   : 1;  /* Dynamic Driver Enable */
            HMC7043_REG idlAtZero_12 : 2;  /* Slip Enable */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0148;



/* Channel Output13 control */

typedef union {
    struct {
            HMC7043_REG chEn_13      : 1;  /* Channel Enable */
            HMC7043_REG multSlpEn_13 : 1;  /* MultiSlip Enable */
            HMC7043_REG stMod_13     : 2;  /* Start-up mode */
            HMC7043_REG reserved     : 1;
            HMC7043_REG slipEn_13    : 1;  /* Slip Enable */
            HMC7043_REG syncEn_13    : 1;  /* SYNC Enable */
            HMC7043_REG hpMode_13    : 1;  /* High Performance mode */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x014a;

typedef union {
    struct {
            HMC7043_REG chDivLsb_13 : 8;  /* Channel Divider[7:0] LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x014b;

typedef union {
    struct {
            HMC7043_REG chDivMsb_13 : 4;  /* Channel Divider[11:8] MSB */
            HMC7043_REG reserved    : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x014c;

typedef union {
    struct {
            HMC7043_REG faDelay_13 : 4;  /* Fine analog Delay */
            HMC7043_REG reserved   : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x014d;

typedef union {
    struct {
            HMC7043_REG cdDelay_13 : 4;  /* Coarse Digital Delay */
            HMC7043_REG reserved   : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x014e;

typedef union {
    struct {
            HMC7043_REG msDelayLsb_13 : 8;  /* MultiSlip Digital Delay[7:0]LSB */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x014f;

typedef union {
    struct {
            HMC7043_REG msDelayMsb_13 : 4;  /* MultiSlip Digital Delay[11:8]MSB */
            HMC7043_REG reserved      : 4;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0150;

typedef union {
    struct {
            HMC7043_REG outMuxSel_13 : 2;  /* Output Mux Selection */
            HMC7043_REG reserved     : 6;
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0151;

typedef union {
    struct {
            HMC7043_REG drvImp_13    : 2;  /* Driver Impedance */
            HMC7043_REG reserved     : 1;
            HMC7043_REG drvMod_13    : 2;  /* Driver mode */
            HMC7043_REG dyDrvEn_13   : 1;  /* Dynamic Driver Enable */
            HMC7043_REG idlAtZero_13 : 2;  /* Slip Enable */
    }fields;
    HMC7043_REG all;
}Hmc7043_reg_x0152;

#pragma pack(pop)


/*#############################################################################*
*   A P P L I C A T I O N - L E V E L    S E T U P    A N D    C O N T R O L   *
*#############################################################################*/

typedef struct {
	Bool initDone;
	Hmc7043_reg_x0000 r00; Hmc7043_reg_x0001 r01; Hmc7043_reg_x0002 r02;
	Hmc7043_reg_x0003 r03; Hmc7043_reg_x0004 r04; Hmc7043_reg_x0005 r05;
	Hmc7043_reg_x0006 r06; Hmc7043_reg_x0007 r07; Hmc7043_reg_x0008 r08;
	Hmc7043_reg_x0009 r09;
	Hmc7043_reg_x000a r0a; Hmc7043_reg_x000b r0b; Hmc7043_reg_x0046 r46;
	Hmc7043_reg_x0050 r50; Hmc7043_reg_x0054 r54; Hmc7043_reg_x005a r5a;
	Hmc7043_reg_x005b r5b; Hmc7043_reg_x005c r5c; Hmc7043_reg_x005d r5d;
	Hmc7043_reg_x0064 r64; Hmc7043_reg_x0065 r65; Hmc7043_reg_x0071 r71;
	Hmc7043_reg_x0078 r78; Hmc7043_reg_x0079 r79; Hmc7043_reg_x007a r7a;
	Hmc7043_reg_x007b r7b; Hmc7043_reg_x007d r7d; Hmc7043_reg_x007f r7f;
	Hmc7043_reg_x0091 r91; Hmc7043_reg_x0098 r98; Hmc7043_reg_x0099 r99;
	Hmc7043_reg_x009d r9d; Hmc7043_reg_x009e r9e; Hmc7043_reg_x009f r9f;
	Hmc7043_reg_x00a0 ra0; Hmc7043_reg_x00a2 ra2; Hmc7043_reg_x00a3 ra3;
	Hmc7043_reg_x00a4 ra4; Hmc7043_reg_x00ad rad; Hmc7043_reg_x00b5 rb5;
	Hmc7043_reg_x00b6 rb6; Hmc7043_reg_x00b7 rb7; Hmc7043_reg_x00b8 rb8;
	Hmc7043_reg_x00c8 rc8; Hmc7043_reg_x00c9 rc9; Hmc7043_reg_x00ca rca;
	Hmc7043_reg_x00cb rcb; Hmc7043_reg_x00cc rcc; Hmc7043_reg_x00cd rcd;
	Hmc7043_reg_x00ce rce; Hmc7043_reg_x00cf rcf; Hmc7043_reg_x00d0 rd0;
	Hmc7043_reg_x00d2 rd2; Hmc7043_reg_x00d3 rd3; Hmc7043_reg_x00d4 rd4;
	Hmc7043_reg_x00d5 rd5; Hmc7043_reg_x00d6 rd6; Hmc7043_reg_x00d7 rd7;
	Hmc7043_reg_x00d8 rd8; Hmc7043_reg_x00d9 rd9; Hmc7043_reg_x00da rda;
	Hmc7043_reg_x00dc rdc; Hmc7043_reg_x00dd rdd; Hmc7043_reg_x00de rde;
	Hmc7043_reg_x00df rdf; Hmc7043_reg_x00e0 re0; Hmc7043_reg_x00e1 re1;
	Hmc7043_reg_x00e2 re2; Hmc7043_reg_x00e3 re3; Hmc7043_reg_x00e4 re4;
	Hmc7043_reg_x00e6 re6; Hmc7043_reg_x00e7 re7; Hmc7043_reg_x00e8 re8;
	Hmc7043_reg_x00e9 re9; Hmc7043_reg_x00ea rea; Hmc7043_reg_x00eb reb;
	Hmc7043_reg_x00ec rec; Hmc7043_reg_x00ed red; Hmc7043_reg_x00ee ree;
	Hmc7043_reg_x00f0 rf0; Hmc7043_reg_x00f1 rf1; Hmc7043_reg_x00f2 rf2;
	Hmc7043_reg_x00f3 rf3; Hmc7043_reg_x00f4 rf4; Hmc7043_reg_x00f5 rf5;
	Hmc7043_reg_x00f6 rf6; Hmc7043_reg_x00f7 rf7; Hmc7043_reg_x00f8 rf8;
	Hmc7043_reg_x00fa rfa; Hmc7043_reg_x00fb rfb; Hmc7043_reg_x00fc rfc;
	Hmc7043_reg_x00fd rfd; Hmc7043_reg_x00fe rfe; Hmc7043_reg_x00ff rff;
	Hmc7043_reg_x0100 r100; Hmc7043_reg_x0101 r101; Hmc7043_reg_x0102 r102;
	Hmc7043_reg_x0104 r104; Hmc7043_reg_x0105 r105;	Hmc7043_reg_x0106 r106;
	Hmc7043_reg_x0107 r107; Hmc7043_reg_x0108 r108; Hmc7043_reg_x0109 r109;
	Hmc7043_reg_x010a r10a; Hmc7043_reg_x010b r10b;	Hmc7043_reg_x010c r10c;
	Hmc7043_reg_x010e r10e; Hmc7043_reg_x010f r10f;	Hmc7043_reg_x0110 r110;
	Hmc7043_reg_x0111 r111; Hmc7043_reg_x0112 r112;	Hmc7043_reg_x0113 r113;
	Hmc7043_reg_x0114 r114; Hmc7043_reg_x0115 r115;	Hmc7043_reg_x0116 r116;
	Hmc7043_reg_x0118 r118; Hmc7043_reg_x0119 r119;	Hmc7043_reg_x011a r11a;
	Hmc7043_reg_x011b r11b; Hmc7043_reg_x011c r11c;	Hmc7043_reg_x011d r11d;
	Hmc7043_reg_x011e r11e; Hmc7043_reg_x011f r11f;	Hmc7043_reg_x0120 r120;
	Hmc7043_reg_x0122 r122; Hmc7043_reg_x0123 r123;	Hmc7043_reg_x0124 r124;
	Hmc7043_reg_x0125 r125; Hmc7043_reg_x0126 r126;	Hmc7043_reg_x0127 r127;
	Hmc7043_reg_x0128 r128; Hmc7043_reg_x0129 r129;	Hmc7043_reg_x012a r12a;
	Hmc7043_reg_x012c r12c; Hmc7043_reg_x012d r12d;	Hmc7043_reg_x012e r12e;
	Hmc7043_reg_x012f r12f; Hmc7043_reg_x0130 r130;	Hmc7043_reg_x0131 r131;
	Hmc7043_reg_x0132 r132; Hmc7043_reg_x0133 r133;	Hmc7043_reg_x0134 r134;
	Hmc7043_reg_x0136 r136; Hmc7043_reg_x0137 r137;	Hmc7043_reg_x0138 r138;
	Hmc7043_reg_x0139 r139; Hmc7043_reg_x013a r13a;	Hmc7043_reg_x013b r13b;
	Hmc7043_reg_x013c r13c; Hmc7043_reg_x013d r13d;	Hmc7043_reg_x013e r13e;
	Hmc7043_reg_x0140 r140; Hmc7043_reg_x0141 r141;	Hmc7043_reg_x0142 r142;
	Hmc7043_reg_x0143 r143; Hmc7043_reg_x0144 r144;	Hmc7043_reg_x0145 r145;
	Hmc7043_reg_x0146 r146; Hmc7043_reg_x0147 r147;	Hmc7043_reg_x0148 r148;
	Hmc7043_reg_x014a r14a; Hmc7043_reg_x014b r14b;	Hmc7043_reg_x014c r14c;
	Hmc7043_reg_x014d r14d; Hmc7043_reg_x014e r14e;	Hmc7043_reg_x014f r14f;
	Hmc7043_reg_x0150 r150; Hmc7043_reg_x0151 r151;	Hmc7043_reg_x0152 r152;
} Hmc7043_reg_image;

typedef struct {
    Hmc7043_reg_image regImage;  /* last setup of (most) control registers */
} Hmc7043_app_dev_state;

LOCAL struct {
    Hmc7043_app_dev_state devState[CKDST_MAX_NDEV];  /* per last command */
} hmc7043AppState;

/*******************************************************************************
* - name: hmc7043AppIfInit
*
* - title: initialize application-level interface
*
* - output: hmc7043AppCtl
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - notes: this routine can be called more than once  --TBD
*******************************************************************************/
LOCAL STATUS hmc7043AppIfInit(void)
{
    hmc7043AppCtl.lwstOutFreq = 0;

    hmc7043AppCtl.initDone = TRUE;

    return OK;
}




/*******************************************************************************
* - name: hmc7043AppSetUpDevCtl
*
* - title: set up control parameters for a PLL device
*
* - input: dev     - CLKDST device for which to perform the operation
*          pParams - pointer to device setup parameters
*
* - output: hmc7043AppCtl.devCtl[dev]
*
* - returns: OK or ERROR if detected an error
*
* - description: sets up CLKDST device control parameters per the application's
*                requirements
*
* - notes: not attempting to interlock the sequence here - if such interlocking
*          is necessary, it must be provided by the caller
*******************************************************************************/
LOCAL STATUS hmc7043AppSetUpDevCtl(CKDST_DEV dev,
                                   const Hmc7043_app_dev_params *pParams)
{
    Hmc7043_app_dev_ctl *pCtl;
    CKDST_FREQ_HZ clkInpFreq;
    unsigned i, chDivider = 0;

    /* initialize */
    if(!inEnumRange(dev, NELEMENTS(hmc7043AppCtl.devCtl)) || !pParams) {
        sysLog("bad argument(s) (dev %d, pParams %d)", dev, pParams != NULL);
        return ERROR;
    }

    pCtl = hmc7043AppCtl.devCtl + dev;

    /* set up device control parameters */
    pCtl->params = *pParams;

    /* In fundamental mode, min frequency is 200Mhz and max frequency is 3200MHz*/
    if(pCtl->params.clkInDiv == HMC7043_CID_1 &&
    		(pCtl->params.clkInFreq < HMC7043_CID1_MIN_FREQ ||
    		 pCtl->params.clkInFreq > HMC7043_CID1_MAX_FREQ) ) {
    	sysLogFpa("CLKIN frequency (%.0f) outside limits for device %.0f",
    	                  (double) pCtl->params.clkInDiv, (double) dev);
    	return ERROR;
    }

    /* In divide by 2 mode, min frequency is 200Mhz and max frequency is 6000MHz*/
    if(pCtl->params.clkInDiv == HMC7043_CID_2 &&
        		(pCtl->params.clkInFreq < HMC7043_CID2_MIN_FREQ ||
        		 pCtl->params.clkInFreq > HMC7043_CID2_MAX_FREQ) ) {
        	sysLogFpa("CLKIN frequency (%.0f) outside limits for device %.0f",
        	                  (double) pCtl->params.clkInDiv, (double) dev);
        	return ERROR;
        }


    /* Verify that if the start-up mode of a SYSREF output channel is
     * configured to be in pulse generator mode, its divide ratio
     * should be > 31 */
    if(pParams->clkInDiv == HMC7043_CID_2) {
		clkInpFreq = pParams->clkInFreq/2;
	}
	else if(pParams->clkInDiv == HMC7043_CID_1){
		clkInpFreq = pParams->clkInFreq;
	}

    for(i = 0; i < HMC7043_OUT_NCHAN; i++) {
        if(pCtl->params.chSup[i].chMode == HMC7043_CHM_SYSREF){
        	if(pCtl->params.chSup[i].dynDriverEn) {
        		chDivider = clkInpFreq/pParams->chSup[i].freq;
        		if(chDivider < 31) {
        			sysLogFpa("SYSREF channel configured in pulse generator mode"
							  "should have divide ratio (%d) greater than 31.",
							  chDivider);
					return ERROR;
        		}
        	}
        }
    }

    /* Verify that a channel's slipQuantumPs is a multiple of the input
     *  clock period (after taking input divisor into account). */
    if(pParams->clkInDiv == HMC7043_CID_2) {
		clkInpFreq = pParams->clkInFreq/2;
	}
	else if(pParams->clkInDiv == HMC7043_CID_1){
		clkInpFreq = pParams->clkInFreq;
	}
	else {
		sysLog("bad argument (clkInDiv %d)", pParams->clkInDiv);
		        return ERROR;
	}
    for(i = 0; i < HMC7043_OUT_NCHAN; i++) {
    	if(clkInpFreq % (CKDST_FREQ_HZ)pParams->chSup[i].slipQuantumPs != 0) {
    		sysLogFpa("Channel's slipQuantumPs is not a multiple of the input"
                      "clock period.Clock period (%.0f), Slip (%.0f)",
					   (double)clkInpFreq, pParams->chSup[i].slipQuantumPs);
    		return ERROR;
    	}
    }

    pCtl->initDone = TRUE;

    return OK;
}




/*******************************************************************************
* - name: hmc7043AppChkProdId
*
* - title: read and verify CLKDST's id(s)
*
* - input: dev - CLKDST device for which to perform the operation
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7043AppChkProdId(CKDST_DEV dev)
{
	Hmc7043_reg_x0078 r78;
	Hmc7043_reg_x0079 r79;
	Hmc7043_reg_x007a r7a;

	/* initialize */
	if (!inEnumRange(dev, NELEMENTS(hmc7043AppCtl.devCtl))) {
	    sysLog("bad dev (%d)", dev);
	    return ERROR;
	}

	if (!hmc7043IfCtl.initDone) {
	   sysLog("interface initialization not done yet (dev %d)", dev);
	   return ERROR;
	}

	/* read device id and compare to the expected */
	if (hmc7043LliRegRead(dev, 0x0078, &r78.all) != OK ||
	    hmc7043LliRegRead(dev, 0x0079, &r79.all) != OK ||
	    hmc7043LliRegRead(dev, 0x007a, &r7a.all) != OK)
	    return ERROR;

	if (r78.fields.pIdLsb != (HMC7043_PRD_ID & 0xff) ||
		r79.fields.pIdMid != ((HMC7043_PRD_ID >> 8) & 0xff) ||
		r7a.fields.pIdMsb != HMC7043_PRD_ID >> 16) {
		sysLog("unexpected id values (dev %d, prodId 0x%02x, 0x%02x, 0x%02x)",
				dev, r78.fields.pIdLsb,r79.fields.pIdMid, r7a.fields.pIdMsb);
		return ERROR;
	}
    return OK;
}




/*******************************************************************************
* - name: hmc7043LoadConfigUpd
*
* - title: Set default values to reserved registers as in Table-40.
*
* - input: dev - CLKDST device for which to perform the operation
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7043LoadConfigUpd(CKDST_DEV dev)
{
	Hmc7043_reg_image *pImg;

	/* initialize */
	if (!inEnumRange(dev, NELEMENTS(hmc7043AppState.devState))) {
	     sysLog("bad argument (dev %d)", dev);
	     return ERROR;
	}

	if (!hmc7043IfCtl.initDone) {
	    sysLog("interface initialization not done yet (dev %d)", dev);
	    return ERROR;
	}

	 pImg = &hmc7043AppState.devState[dev].regImage;

	 /* Set reserved registers as per table-40 */
	 pImg->r98.all = 0x00; pImg->r99.all = 0x00; pImg->r9d.all = 0xAA;
	 pImg->r9e.all = 0xAA; pImg->r9f.all = 0x4D; pImg->ra0.all = 0xDF;
	 pImg->ra2.all = 0x03; pImg->ra3.all = 0x00; pImg->ra4.all = 0x00;
	 pImg->rad.all = 0x00; pImg->rb5.all = 0x00; pImg->rb6.all = 0x00;
	 pImg->rb7.all = 0x00; pImg->rb8.all = 0x00;

	return OK;
}




/*******************************************************************************
* - name: hmc7043AppInitReservedReg
*
* - title: Set default values for all other reserved
* 		   registers(other than the ones in table-40)
*
* - input: dev - CLKDST device for which to perform the operation
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7043AppInitReservedReg(CKDST_DEV dev)
{
	Hmc7043_reg_image *pImg;

    /* initialize */
	if (!inEnumRange(dev, NELEMENTS(hmc7043AppState.devState))) {
	     sysLog("bad argument (dev %d)", dev);
	     return ERROR;
	}

	if (!hmc7043IfCtl.initDone) {
	    sysLog("interface initialization not done yet (dev %d)", dev);
	    return ERROR;
	}

	pImg = &hmc7043AppState.devState[dev].regImage;

	/* Initializing reserved registers(excluded from table-40) */
	pImg->r05.all = 0x0F;
	pImg->r07.all = 0x00;
	pImg->r08.all = 0x00;
	pImg->r09.all = 0x00;

	return OK;
}




/*******************************************************************************
* - name: hmc7043AppInitReservdFields
*
* - title: Set default values for all reserved fields
*
* - input: dev - CLKDST device for which to perform the operation
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7043AppInitReservdFields(CKDST_DEV dev)
{
	Hmc7043_reg_image *pImg;

	/* initialize */
	if (!inEnumRange(dev, NELEMENTS(hmc7043AppState.devState))) {
		 sysLog("bad argument (dev %d)", dev);
		 return ERROR;
	}

	if (!hmc7043IfCtl.initDone) {
		sysLog("interface initialization not done yet (dev %d)", dev);
		return ERROR;
	}

	pImg = &hmc7043AppState.devState[dev].regImage;

	pImg->r00.fields.reserved = HMC7043_RSVD_VAL1;
	pImg->r01.fields.reserved = HMC7043_RSVD_VAL2;
	pImg->r02.fields.reserved1 = HMC7043_RSVD_VAL2;
	pImg->r02.fields.reserved2 = HMC7043_RSVD_VAL1;
	pImg->r03.fields.reserved1 = HMC7043_RSVD_VAL2;
	pImg->r03.fields.reserved2 = 0x2;
	pImg->r03.fields.reserved3 = HMC7043_RSVD_VAL2;
	pImg->r04.fields.reserved = HMC7043_RSVD_VAL2;
	pImg->r06.fields.reserved = HMC7043_RSVD_VAL1;
	pImg->r0a.fields.reserved = HMC7043_RSVD_VAL2;
	pImg->r0b.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->r46.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->r50.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->r54.fields.reserved = HMC7043_RSVD_VAL1;
    pImg->r5a.fields.reserved = HMC7043_RSVD_VAL1;
    pImg->r5b.fields.reserved1 =  HMC7043_RSVD_VAL2;
    pImg->r5b.fields.reserved2 = HMC7043_RSVD_VAL1;
    pImg->r5d.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->r64.fields.reserved = HMC7043_RSVD_VAL1;
    pImg->r65.fields.reserved = HMC7043_RSVD_VAL1;
    pImg->r71.fields.reserved1 = HMC7043_RSVD_VAL2;
    pImg->r71.fields.reserved2 = HMC7043_RSVD_VAL2;
    pImg->r71.fields.reserved3 = HMC7043_RSVD_VAL2;
    pImg->r7b.fields.reserved = HMC7043_RSVD_VAL1; // default hex values not given in datasheet
    pImg->r7d.fields.reserved1 = HMC7043_RSVD_VAL2;
    pImg->r7d.fields.reserved2 = HMC7043_RSVD_VAL2;// default hex values not given in datasheet
    pImg->r7d.fields.reserved3 = HMC7043_RSVD_VAL2;
    pImg->r91.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rc8.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rca.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rcb.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rcc.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rce.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rcf.fields.reserved = HMC7043_RSVD_VAL1;
    pImg->rd0.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rd2.fields.reserved = 0x1;
    pImg->rd4.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rd5.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rd6.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rd8.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rd9.fields.reserved = HMC7043_RSVD_VAL1;
    pImg->rda.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rdc.fields.reserved = 0x1;
    pImg->rde.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rdf.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->re0.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->re2.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->re3.fields.reserved = HMC7043_RSVD_VAL1;
    pImg->re4.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->re6.fields.reserved = 0x1;
    pImg->re8.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->re9.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rea.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rec.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->red.fields.reserved = HMC7043_RSVD_VAL1;
    pImg->ree.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rf0.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rf2.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rf3.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rf4.fields.reserved =	HMC7043_RSVD_VAL2;
    pImg->rf6.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rf7.fields.reserved = HMC7043_RSVD_VAL1;
    pImg->rf8.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rfa.fields.reserved = 0x1;
    pImg->rfc.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rfd.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->rfe.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->r100.fields.reserved = HMC7043_RSVD_VAL2;
    pImg->r101.fields.reserved = HMC7043_RSVD_VAL1;
    pImg->r102.fields.reserved = HMC7043_RSVD_VAL2;

    return OK;
}



/*******************************************************************************
* - name: hmc7043AppInitRdRegs
*
* - title: read CLKDST registers and set up register image data
*
* - input: dev - CLKDST device for which to perform the operation
*
* - output: hmc7043AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - notes: 1) Not attempting to reset the device here (that is up to the caller).
*******************************************************************************/
LOCAL STATUS hmc7043AppInitRdRegs(CKDST_DEV dev)
{
    typedef struct {unsigned regInx, dataOffs;} Reg_desc;

    static const Reg_desc regDescs[] = {
#       define RDESC(reg)  {0x##reg, offsetof(Hmc7043_reg_image, r##reg.all)}

		RDESC(01), RDESC(02), RDESC(03), RDESC(04), RDESC(05), RDESC(06),
		RDESC(07), RDESC(08), RDESC(09), RDESC(0a), RDESC(0b), RDESC(46),
		RDESC(50), RDESC(54),RDESC(5a), RDESC(5b), RDESC(5c), RDESC(5d),
		RDESC(64), RDESC(65), RDESC(71),RDESC(98), RDESC(99), RDESC(9d),
		RDESC(9e), RDESC(9f), RDESC(a0), RDESC(a2),	RDESC(a3), RDESC(a4),
		RDESC(ad), RDESC(b5), RDESC(b6), RDESC(b7), RDESC(b8),RDESC(c8),
		RDESC(c9), RDESC(ca), RDESC(cb), RDESC(cc), RDESC(cd), RDESC(ce),
		RDESC(cf), RDESC(d0), RDESC(d2), RDESC(d3), RDESC(d5), RDESC(d6),
		RDESC(d7), RDESC(d8), RDESC(d9), RDESC(da), RDESC(dc), RDESC(dd),
		RDESC(de), RDESC(df), RDESC(e0), RDESC(e1), RDESC(e2), RDESC(e3),
		RDESC(e4), RDESC(e6), RDESC(e7), RDESC(e8), RDESC(e9), RDESC(ea),
		RDESC(eb), RDESC(ec), RDESC(ed), RDESC(ee),	RDESC(f0), RDESC(f1),
		RDESC(f2), RDESC(f3), RDESC(f4), RDESC(f5), RDESC(f6), RDESC(f7),
		RDESC(f8), RDESC(fa), RDESC(fb), RDESC(fc), RDESC(fd), RDESC(fe),
		RDESC(ff), RDESC(100), RDESC(101), RDESC(102), RDESC(104), RDESC(105),
		RDESC(106),	RDESC(107), RDESC(108), RDESC(109), RDESC(10a), RDESC(10b),
		RDESC(10c), RDESC(10e),	RDESC(10f), RDESC(110), RDESC(111), RDESC(112),
		RDESC(113), RDESC(114), RDESC(115),	RDESC(116), RDESC(118), RDESC(119),
		RDESC(11a), RDESC(11b), RDESC(11c), RDESC(11e),	RDESC(11f), RDESC(120),
		RDESC(122), RDESC(123), RDESC(124), RDESC(125), RDESC(126),	RDESC(127),
		RDESC(128), RDESC(129), RDESC(12a), RDESC(12c), RDESC(12d), RDESC(12e),
		RDESC(12f), RDESC(130), RDESC(131), RDESC(132), RDESC(133), RDESC(134),
		RDESC(136),	RDESC(137), RDESC(138), RDESC(139), RDESC(13a), RDESC(13b),
		RDESC(13c), RDESC(13e),	RDESC(140), RDESC(141), RDESC(142), RDESC(143),
		RDESC(144), RDESC(145), RDESC(146),	RDESC(147), RDESC(148), RDESC(14a),
		RDESC(14b), RDESC(14c), RDESC(14d), RDESC(14e),	RDESC(14f), RDESC(150),
		RDESC(151), RDESC(152)
#       undef RDESC
    };

    unsigned i;
    Hmc7043_reg_image *pImg;

    /* initialize */
    if (!inEnumRange(dev, NELEMENTS(hmc7043AppState.devState))) {
        sysLog("bad argument (dev %d)", dev);
        return ERROR;
    }

    pImg = &hmc7043AppState.devState[dev].regImage;

    /* perform the operation */
    for (i = 0; i < NELEMENTS(regDescs); ++i) {
        const Reg_desc *pDesc = regDescs + i;

        UINT8 *pData = (UINT8 *) pImg + pDesc->dataOffs;

        if (hmc7043LliRegRead(dev, pDesc->regInx, pData) != OK)
            return ERROR;
    }

    pImg->initDone = TRUE;

    return OK;
}





/*******************************************************************************
* - name: hmc7043AppInitWrRegs
*
* - title: write initialization register image data to CLKDST registers
*
* - input: dev - CLKDST device for which to perform the operation
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7043AppInitWrRegs(CKDST_DEV dev)
{
	typedef struct {unsigned regInx, dataOffs;} Reg_desc;

	static const Reg_desc regDescs[] = {
#       define RDESC(reg)  {0x##reg, offsetof(Hmc7043_reg_image, r##reg.all)}

		RDESC(01), RDESC(02), RDESC(03), RDESC(04), RDESC(05), RDESC(06),
		RDESC(07), RDESC(08), RDESC(09), RDESC(0a), RDESC(0b), RDESC(46),
		RDESC(50), RDESC(54),RDESC(5a), RDESC(5b), RDESC(5c), RDESC(5d),
		RDESC(64), RDESC(65), RDESC(71),RDESC(98), RDESC(99), RDESC(9d),
		RDESC(9e), RDESC(9f), RDESC(a0), RDESC(a2),	RDESC(a3), RDESC(a4),
		RDESC(ad), RDESC(b5), RDESC(b6), RDESC(b7), RDESC(b8),RDESC(c8),
		RDESC(c9), RDESC(ca), RDESC(cb), RDESC(cc), RDESC(cd), RDESC(ce),
		RDESC(cf), RDESC(d0), RDESC(d2), RDESC(d3), RDESC(d5), RDESC(d6),
		RDESC(d7), RDESC(d8), RDESC(d9), RDESC(da), RDESC(dc), RDESC(dd),
		RDESC(de), RDESC(df), RDESC(e0), RDESC(e1), RDESC(e2), RDESC(e3),
		RDESC(e4), RDESC(e6), RDESC(e7), RDESC(e8), RDESC(e9), RDESC(ea),
		RDESC(eb), RDESC(ec), RDESC(ed), RDESC(ee),	RDESC(f0), RDESC(f1),
		RDESC(f2), RDESC(f3), RDESC(f4), RDESC(f5), RDESC(f6), RDESC(f7),
		RDESC(f8), RDESC(fa), RDESC(fb), RDESC(fc), RDESC(fd), RDESC(fe),
		RDESC(ff), RDESC(100), RDESC(101), RDESC(102), RDESC(104), RDESC(105),
		RDESC(106),	RDESC(107), RDESC(108), RDESC(109), RDESC(10a), RDESC(10b),
		RDESC(10c), RDESC(10e),	RDESC(10f), RDESC(110), RDESC(111), RDESC(112),
		RDESC(113), RDESC(114), RDESC(115),	RDESC(116), RDESC(118), RDESC(119),
		RDESC(11a), RDESC(11b), RDESC(11c), RDESC(11e),	RDESC(11f), RDESC(120),
		RDESC(122), RDESC(123), RDESC(124), RDESC(125), RDESC(126),	RDESC(127),
		RDESC(128), RDESC(129), RDESC(12a), RDESC(12c), RDESC(12d), RDESC(12e),
		RDESC(12f), RDESC(130), RDESC(131), RDESC(132), RDESC(133), RDESC(134),
		RDESC(136),	RDESC(137), RDESC(138), RDESC(139), RDESC(13a), RDESC(13b),
		RDESC(13c), RDESC(13e),	RDESC(140), RDESC(141), RDESC(142), RDESC(143),
		RDESC(144), RDESC(145), RDESC(146),	RDESC(147), RDESC(148), RDESC(14a),
		RDESC(14b), RDESC(14c), RDESC(14d), RDESC(14e),	RDESC(14f), RDESC(150),
		RDESC(151), RDESC(152)

#       undef RDESC
	};

	    unsigned i;
	    const Hmc7043_reg_image *pImg;

	    /* initialize */
	    if (!inEnumRange(dev, NELEMENTS(hmc7043AppState.devState))) {
	        sysLog("bad argument (dev %d)", dev);
	        return ERROR;
	    }

	    pImg = &hmc7043AppState.devState[dev].regImage;

	    /* perform the operation */
	    for (i = 0; i < NELEMENTS(regDescs); ++i) {
	        const Reg_desc *pDesc = regDescs + i;

	        UINT8 regData = *((UINT8 *) pImg + pDesc->dataOffs);

	        if (hmc7043LliRegWrite(dev, pDesc->regInx, regData) != OK)
	            return ERROR;
	    }

	    return OK;
}




/*******************************************************************************
* - name: hmc7043ToggleBit
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
LOCAL STATUS hmc7043ToggleBit(CKDST_DEV dev, unsigned regIdx,
		                          HMC7043_REG fieldBit,unsigned delay)
{
	HMC7043_REG data;

	if (!inEnumRange(dev, NELEMENTS(hmc7043AppState.devState))) {
		sysLog("bad argument (dev %d)", dev);
		return ERROR;
	}

	if (!hmc7043IfCtl.initDone) {
		sysLog("interface initialization not done yet (dev %d)", dev);
		return ERROR;
	}

	if(hmc7043LliRegRead(dev, regIdx, &data) != OK)
		return ERROR;

	if(hmc7043LliRegWrite(dev, regIdx, (data | (1 << fieldBit))) != OK)
		return ERROR;

	data &= ~fieldBit;

	if(hmc7043LliRegWrite(dev, regIdx, data) != OK)
			return ERROR;

	sysDelayUsec(delay);

	return OK;
}





/*******************************************************************************
* - name: hmc7043WaitSysrefPeriod
*
* - title: Find the GCD of sysref frequencies
*
* - input: dev - CLKDST device for which to perform the operation
*          times - multiplication factor for wait period
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7043WaitSysrefPeriod(CKDST_DEV dev, unsigned times)
{
	UINT64 sysrefPeriod;
	Hmc7043_reg_x005c r5c;
	Hmc7043_reg_x005d r5d;


	if (!inEnumRange(dev, NELEMENTS(hmc7043AppState.devState))) {
		sysLog("bad argument (dev %d)", dev);
		return ERROR;
	}

	if((hmc7043LliRegRead(dev, 0x005c, &r5c.all) != OK) ||
	   (hmc7043LliRegRead(dev, 0x005d, &r5d.all) != OK) )
		return ERROR;

    sysrefPeriod = r5d.fields.timer << 0xf;
    sysrefPeriod = sysrefPeriod | r5c.fields.timer;
	sysDelayUsec((sysrefPeriod * times));

    return OK;
}




/*******************************************************************************
* - name: hmc7043AppInitPgmSysrefTimer
*
* - title: Program the SYSREF timer with submultiple of lowest
*          output sysref frequency, not greater than 4MHz.
*
* - input: dev - CLKDST device for which to perform the operation
*          pParams - pointer to device setup parameters
*
* - output: hmc7043AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7043AppInitPgmSysrefTimer(CKDST_DEV dev,
		                          const Hmc7043_app_dev_params *pParams)
{
	Hmc7043_reg_image *pImg;
	unsigned ch;
	CKDST_FREQ_HZ minFreq = 0;

	if (!inEnumRange(dev, NELEMENTS(hmc7043AppState.devState)) || !pParams ||
			(pParams->clkInFreq == 0) ) {
		sysLog("bad argument(s) (dev %d, pParams %d)", dev, pParams != NULL);
		return ERROR;
	}

	pImg = &hmc7043AppState.devState[dev].regImage;

	/* Find lowest output(SYSREF) frequency */
	for (ch = 0; ch < HMC7043_OUT_NCHAN; ch++) {
		if(pParams->chSup[ch].chMode == HMC7043_CHM_SYSREF) {
			if(minFreq == 0) {
				minFreq = pParams->chSup[ch].freq;
			}
			else {
				if(pParams->chSup[ch].freq < minFreq)
					minFreq = pParams->chSup[ch].freq;
			}
		}
	}

	/* Check if SYSREF Timer count is multiple of lowest output(SYSREF)
	 * frequency and not greater than 4MHz*/
	if(pParams->sysref.freq >= 4000000U ||
			(pParams->sysref.freq % minFreq != 0)) {
		sysLog("SYSREF frequency is not an integer multiple of all channel "
				"dividers (lowest output(SYSREF) frequency %lu, sysref "
				"frequency %lu)", minFreq, pParams->sysref.freq);
		return ERROR;
	}

    pImg->r5c.all = (pParams->sysref.freq & 0xff);
    pImg->r5d.fields.timer = pParams->sysref.freq >> 8;

	return OK;
}




/*******************************************************************************
* - name: hmc7043AppInitPgmOutCh
*
* - title: Program the output used channels
*
* - input: dev - CLKDST device for which to perform the operation
*          pParams - pointer to device setup parameters
*
* - output: hmc7043AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7043AppInitPgmOutCh(CKDST_DEV dev,
		                          const Hmc7043_app_dev_params *pParams)
{
	Hmc7043_reg_image *pImg;
	unsigned ch;
	CKDST_FREQ_HZ clkInpFreq = 0;
	unsigned chDivider = 0, addMultislip = 0, multiSlip = 0;
	double mDelay, slQuPs, halfFreq, remSlip, numDigSteps;
	double numAnlgSteps, remaDly, remdDly;

	if (!inEnumRange(dev, NELEMENTS(hmc7043AppState.devState)) || !pParams) {
		sysLog("bad argument(s) (dev %d, pParams %d)", dev, pParams != NULL);
		return ERROR;
	}

	pImg = &hmc7043AppState.devState[dev].regImage;

	if(pParams->clkInDiv == HMC7043_CID_2) {
		clkInpFreq = pParams->clkInFreq/2;
	}
	else if(pParams->clkInDiv == HMC7043_CID_1){
		clkInpFreq = pParams->clkInFreq;
	}

	for (ch = 0; ch < HMC7043_OUT_NCHAN; ch++) {
		if(pParams->chSup[ch].chMode != HMC7043_CHM_UNUSED)
		{
			chDivider = clkInpFreq/pParams->chSup[ch].freq;

			/* Validating Coarse Digital Delay */
			halfFreq = 0.5 * pParams->clkInFreq;
			remdDly = remainder(pParams->chSup[ch].dDlyPs, halfFreq);
			numDigSteps = pParams->chSup[ch].dDlyPs/halfFreq;
			if(fabs(remdDly) > 0.1) {
				sysLog("dDlyPs (pParams->chSup[%u].dDlyPs %f) "
						"should be a multiple to 0.1ps "
						"accuracy.",ch,
						pParams->chSup[ch].dDlyPs);
				return ERROR;

			}
			if(pParams->chSup[ch].dDlyPs > (17*halfFreq)) {
				sysLog("dDlyPs (pParams->chSup[ch].dDlyPs %f)should"
						"be less than or equal to 17 half clocks",
						pParams->chSup[ch].dDlyPs);
				return ERROR;
			}

			/* MultiSlip delay validation */
			if(pParams->chSup[ch].slipQuantumPs >1) {
				remSlip = fmod(pParams->chSup[ch].slipQuantumPs, clkInpFreq);
				slQuPs = pParams->chSup[ch].slipQuantumPs/clkInpFreq;
				if(fabs(remSlip) > 0.1) {
					sysLog("slipQuantumPs ( pParams->chSup[%u]."
							"slipQuantumPs %f) should"
							"be an integral multiple of input "
							"clock cycle",ch,
							pParams->chSup[ch].slipQuantumPs);
					return ERROR;
				}
			}

			/* Validating Analog Delay */
			remaDly = fmod(pParams->chSup[ch].aDlyPs, 25);
			numAnlgSteps = pParams->chSup[ch].aDlyPs/25;
			if(remaDly > 0.1) {
				sysLog("aDlyPs(pParams->chSup[%d].aDlyPs %f) should"
						"be a multiple to 0.1ps accuracy.",ch,
						pParams->chSup[ch].aDlyPs);
				return ERROR;
			}
			if (pParams->chSup[ch].aDlyPs >
	                (HMC7043_ADLY_MAX_STEPS *
	                		HMC7043_ADLY_STEP_SIZE)) {
				sysLog("aDlyPs(pParams->chSup[%d].aDlyPs %f) should"
						"be less than or equal to 23*25.",ch,
						pParams->chSup[ch].aDlyPs);
				return ERROR;
			}

			/* Verify that analog delay is not configured as output MUX
			 * for a DCLK channel */
			if((pParams->chSup[ch].chMode == HMC7043_CHM_CLK) &&
				(pParams->chSup[ch].outSel == HMC7043_COS_DIV_ADLY)){
				sysLog("Exposing analog delay on output MUX for "
						"DCLK channels causes phase noise "
						"degradation.");
				return ERROR;
			}

			switch(ch){
				case 0: {
					pImg->rc8.fields.chEn_0 = 0x1;
					pImg->rc8.fields.hpMode_0 =
							pParams->chSup[ch].highPerfMode? 1 : 0;
					pImg->rc8.fields.syncEn_0 = 0x1;
					pImg->rd0.fields.drvMod_0 = pParams->chSup[ch].drvMode;

					/* Configure channel divider */
					pImg->rc9.fields.chDivLsb_0 =
							HMC7043_LSB_BIT_VAL(chDivider);
					pImg->rca.fields.chDivMsb_0 =
							HMC7043_MSB_BIT_VAL(chDivider);
					/* MultiSlip delay configuration */
					if(pParams->chSup[ch].slipQuantumPs >1) {
						pImg->rc8.fields.multSlpEn_0 = 1;
						addMultislip = chDivider/2;
						multiSlip = slQuPs + addMultislip;

						pImg->rcd.fields.msDelayLsb_0 =
								HMC7043_LSB_BIT_VAL(multiSlip);
						pImg->rce.fields.msDelayMsb_0 =
								HMC7043_LSB_BIT_VAL(multiSlip);
					} else if(pParams->chSup[ch].slipQuantumPs  == 1) {
						pImg->rc8.fields.slipEn_0 = 0x1;
					}
					/* Configuring Coarse Digital Delay */
					pImg->rcc.fields.cdDelay_0 = (unsigned)numDigSteps;
					/* Configuring  Fine Analog Delay */
					pImg->rcb.fields.faDelay_0 = (unsigned)numAnlgSteps;
					/* Configuring Driver Impedance*/
					if(pParams->chSup[ch].drvMode == HMC7043_CDM_CML) {
						if(pParams->chSup[ch].cmlTerm == HMC7043_CCIT_NONE)
							pImg->rd0.fields.drvImp_0 = 0x0;
						else if(pParams->chSup[ch].cmlTerm == HMC7043_CCIT_100)
							pImg->rd0.fields.drvImp_0 = 0x1;
						else if(pParams->chSup[ch].cmlTerm == HMC7043_CCIT_50)
							pImg->rd0.fields.drvImp_0 = 0x3;
					}

					if(pParams->chSup[ch].chMode == HMC7043_CHM_CLK) {
						pImg->rd0.fields.idlAtZero_0 = 0x0;
					} else if(pParams->chSup[ch].chMode == HMC7043_CHM_SYSREF) {
						pImg->rd0.fields.dyDrvEn_0 =
								pParams->chSup[ch].dynDriverEn? 1 : 0;
					}

					/* Configure start-up mode */
					if(pParams->chSup[ch].dynDriverEn)
						pImg->rc8.fields.stMod_0 = 0x3;
					else
						pImg->rc8.fields.stMod_0 = 0x0;

					/* Configure output MUX selection */
					if(pParams->chSup[ch].outSel ==
							HMC7043_COS_FUNDAMENTAL)
						pImg->rcf.fields.outMuxSel_0 = 0x3;
					else if(pParams->chSup[ch].outSel ==
							HMC7043_COS_DIVIDER)
						pImg->rcf.fields.outMuxSel_0 = 0x0;
					else if(pParams->chSup[ch].outSel ==
							HMC7043_COS_DIV_ADLY)
						pImg->rcf.fields.outMuxSel_0 = 0x1;
					else if(pParams->chSup[ch].outSel ==
							HMC7043_COS_DIV_NEIGHBOR)
						pImg->rcf.fields.outMuxSel_0 = 0x2;

					break;
				}
				case 1: {
					    pImg->rd3.fields.chDivLsb_1 =
					    		HMC7043_LSB_BIT_VAL(chDivider);
					    pImg->rd4.fields.chDivMsb_1 =
					    		HMC7043_LSB_BIT_VAL(chDivider);
						pImg->rd2.fields.hpMode_1 =
								pParams->chSup[ch].highPerfMode;
						pImg->rd2.fields.syncEn_1 = 0x1;
						//pImg->rd2.fields.slipEn_1 = 0x1;
						mDelay = pParams->chSup[ch].slipQuantumPs/clkInpFreq;
						if(mDelay > 1) {
							pImg->rd2.fields.multSlpEn_1 = 0x1;
							//pImg->rd7.fields.msDelayLsb_1 = ;TBD
						   // pImg->rd8.fields.msDelayMsb_1 = ;
						}
						//pImg->rd5.fields.faDelay_1 = pParams->chSup[ch].aDlyPs;
						//pImg->rd6.fields.cdDelay_1 = pParams->chSup[ch].dDlyPs;
						pImg->rda.fields.drvMod_1 = pParams->chSup[ch].drvMode;

						/* MultiSlip delay configuration */
						if(pParams->chSup[ch].slipQuantumPs >1) {
							pImg->rc8.fields.multSlpEn_0 = 1;
							addMultislip = chDivider/2;
							multiSlip = slQuPs + addMultislip;

							pImg->rcd.fields.msDelayLsb_0 =
									HMC7043_LSB_BIT_VAL(multiSlip);
							pImg->rce.fields.msDelayMsb_0 =
									HMC7043_LSB_BIT_VAL(multiSlip);
						} else if(pParams->chSup[ch].slipQuantumPs  == 1) {
							pImg->rc8.fields.slipEn_0 = 0x1;
						}
						/* Configuring Coarse Digital Delay */
						pImg->rcc.fields.cdDelay_0 = (unsigned)numDigSteps;
						/* Configuring  Fine Analog Delay */
						pImg->rcb.fields.faDelay_0 = (unsigned)numAnlgSteps;
						if(pParams->chSup[ch].drvMode == HMC7043_CDM_CML) {
							if(pParams->chSup[ch].cmlTerm == HMC7043_CCIT_NONE)
								pImg->rda.fields.drvImp_1 = 0x0;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_100)
								pImg->rda.fields.drvImp_1 = 0x1;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_50)
								pImg->rda.fields.drvImp_1 = 0x3;
						}
						if(pParams->chSup[ch].chMode == HMC7043_CHM_CLK) {
							pImg->rda.fields.idlAtZero_1 = 0x00;
						}
						else if(pParams->chSup[ch].chMode ==
								HMC7043_CHM_SYSREF) {
							pImg->rda.fields.dyDrvEn_1 =
									pParams->chSup[ch].dynDriverEn? 1 : 0;
						}
						pImg->rd2.fields.chEn_1 = 0x1;
						if(pParams->chSup[ch].dynDriverEn)
							pImg->rd2.fields.stMod_1 = 0x3;
						else
							pImg->rd2.fields.stMod_1 = 0x0;

						if(pParams->chSup[ch].outSel ==
								HMC7043_COS_FUNDAMENTAL)
							pImg->rd9.fields.outMuxSel_1 = 0x3;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIVIDER)
							pImg->rd9.fields.outMuxSel_1 = 0x0;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_ADLY)
							pImg->rd9.fields.outMuxSel_1 = 0x1;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_NEIGHBOR)
							pImg->rd9.fields.outMuxSel_1 = 0x2;
						break;
			    }

				case 2: {
					    pImg->rdd.fields.chDivLsb_2 =
					    		HMC7043_LSB_BIT_VAL(chDivider);
					    pImg->rde.fields.chDivMsb_2 =
					    		HMC7043_MSB_BIT_VAL(chDivider);
						pImg->rdc.fields.hpMode_2 =
								pParams->chSup[ch].highPerfMode;
						pImg->rdc.fields.syncEn_2 = 0x1;
						pImg->rdc.fields.slipEn_2 = 0x1;
						pImg->rdf.fields.faDelay_2 = pParams->chSup[ch].aDlyPs;
						pImg->re0.fields.cdDelay_2 = pParams->chSup[ch].dDlyPs;
						pImg->re4.fields.drvMod_2 = pParams->chSup[ch].drvMode;
						if(pParams->chSup[ch].drvMode == HMC7043_CDM_CML) {
							if(pParams->chSup[ch].cmlTerm == HMC7043_CCIT_NONE)
								pImg->re4.fields.drvImp_2 = 0x0;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_100)
								pImg->re4.fields.drvImp_2 = 0x1;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_50)
								pImg->re4.fields.drvImp_2 = 0x3;
						}
						if(pParams->chSup[ch].chMode == HMC7043_CHM_CLK) {
							pImg->re4.fields.idlAtZero_2 = 0x00;
						}
						else if(pParams->chSup[ch].chMode ==
								HMC7043_CHM_SYSREF) {
							pImg->re4.fields.dyDrvEn_2 =
									pParams->chSup[ch].dynDriverEn? 1 : 0;
						}
						pImg->rdc.fields.chEn_2 = 0x1;
						if(pParams->chSup[ch].dynDriverEn)
							pImg->rdc.fields.stMod_2 = 0x3;
						else
							pImg->rdc.fields.stMod_2 = 0x0;

						if(pParams->chSup[ch].outSel == HMC7043_COS_FUNDAMENTAL)
							pImg->re3.fields.outMuxSel_2 = 0x3;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIVIDER)
							pImg->re3.fields.outMuxSel_2 = 0x0;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_ADLY)
							pImg->re3.fields.outMuxSel_2 = 0x1;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_NEIGHBOR)
							pImg->re3.fields.outMuxSel_2 = 0x2;
						break;
			    }
				case 3: {
					    pImg->re7.fields.chDivLsb_3 =
					    		HMC7043_LSB_BIT_VAL(chDivider);
					    pImg->re8.fields.chDivMsb_3 =
					    		HMC7043_MSB_BIT_VAL(chDivider);
						pImg->re6.fields.hpMode_3 =
								pParams->chSup[ch].highPerfMode;
						pImg->re6.fields.syncEn_3 = 0x1;
						pImg->re6.fields.slipEn_3 = 0x1;
						pImg->re9.fields.faDelay_3 = pParams->chSup[ch].aDlyPs;
						pImg->rea.fields.cdDelay_3 = pParams->chSup[ch].dDlyPs;
						pImg->ree.fields.drvMod_3 = pParams->chSup[ch].drvMode;
						if(pParams->chSup[ch].drvMode == HMC7043_CDM_CMOS) {
							pImg->re6.fields.multSlpEn_3 = 1;
							addMultislip = chDivider/2;
							mDelay = pParams->chSup[ch].slipQuantumPs/clkInpFreq;
							multiSlip = mDelay + addMultislip;
							pImg->reb.fields.msDelayLsb_3 = (multiSlip & 0xff);
							pImg->rec.fields.msDelayMsb_3 = multiSlip >> 8;

						}
						if(pParams->chSup[ch].drvMode == HMC7043_CDM_CML) {
						    if(pParams->chSup[ch].cmlTerm == HMC7043_CCIT_NONE)
								pImg->ree.fields.drvImp_3 = 0x0;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_100)
								pImg->ree.fields.drvImp_3 = 0x1;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_50)
								pImg->ree.fields.drvImp_3 = 0x3;
						}
						if(pParams->chSup[ch].chMode == HMC7043_CHM_CLK) {
							pImg->ree.fields.idlAtZero_3 = 0x00;
						}
						else if(pParams->chSup[ch].chMode ==
								HMC7043_CHM_SYSREF) {
							pImg->ree.fields.dyDrvEn_3 =
									pParams->chSup[ch].dynDriverEn? 1 : 0;
						}
						pImg->re6.fields.chEn_3 = 0x1;
						if(pParams->chSup[ch].dynDriverEn)
							pImg->re6.fields.stMod_3 = 0x3;
						else
							pImg->re6.fields.stMod_3 = 0x0;

						if(pParams->chSup[ch].outSel == HMC7043_COS_FUNDAMENTAL)
							pImg->red.fields.outMuxSel_3 = 0x3;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIVIDER)
							pImg->red.fields.outMuxSel_3 = 0x0;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_ADLY)
							pImg->red.fields.outMuxSel_3 = 0x1;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_NEIGHBOR)
							pImg->red.fields.outMuxSel_3 = 0x2;
						break;
				}
				case 4: {
					    pImg->rf1.fields.chDivLsb_4 =
					    		HMC7043_LSB_BIT_VAL(chDivider);
					    pImg->rf2.fields.chDivMsb_4 =
					    		HMC7043_MSB_BIT_VAL(chDivider);
						pImg->rf0.fields.hpMode_4 =
								pParams->chSup[ch].highPerfMode;
						pImg->rf0.fields.syncEn_4 = 0x1;
						pImg->rf0.fields.slipEn_4 = 0x1;
						pImg->rf3.fields.faDelay_4 = pParams->chSup[ch].aDlyPs;
						pImg->rf4.fields.cdDelay_4 = pParams->chSup[ch].dDlyPs;
						pImg->rf8.fields.drvMod_4 = pParams->chSup[ch].drvMode;
						if(pParams->chSup[ch].drvMode == HMC7043_CDM_CML) {
							if(pParams->chSup[ch].cmlTerm == HMC7043_CCIT_NONE)
								pImg->rf8.fields.drvImp_4 = 0x0;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_100)
								pImg->rf8.fields.drvImp_4 = 0x1;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_50)
								pImg->rf8.fields.drvImp_4 = 0x3;
						}
						if(pParams->chSup[ch].chMode == HMC7043_CHM_CLK) {
							pImg->rf8.fields.idlAtZero_4 = 0x00;
						}
						else if(pParams->chSup[ch].chMode == HMC7043_CHM_SYSREF) {
							pImg->rf8.fields.dyDrvEn_4 =
									pParams->chSup[ch].dynDriverEn? 1 : 0;
						}
						pImg->rf0.fields.chEn_4 = 0x1;
						if(pParams->chSup[ch].dynDriverEn)
							pImg->rf0.fields.stMod_4 = 0x3;
						else
							pImg->rf0.fields.stMod_4 = 0x0;

						if(pParams->chSup[ch].outSel == HMC7043_COS_FUNDAMENTAL)
							pImg->rf7.fields.outMuxSel_4 = 0x3;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIVIDER)
							pImg->rf7.fields.outMuxSel_4 = 0x0;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_ADLY)
							pImg->rf7.fields.outMuxSel_4 = 0x1;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_NEIGHBOR)
							pImg->rf7.fields.outMuxSel_4 = 0x2;
						break;
				}
				case 5: {
					    pImg->rfb.fields.chDivLsb_5 =
					    		HMC7043_LSB_BIT_VAL(chDivider);
					    pImg->rfc.fields.chDivMsb_5 =
					    		HMC7043_MSB_BIT_VAL(chDivider);
						pImg->rfa.fields.hpMode_5 =
								pParams->chSup[ch].highPerfMode;
						pImg->rfa.fields.syncEn_5 = 0x1;
						pImg->rfa.fields.slipEn_5 = 0x1;
						pImg->rfd.fields.faDelay_5 = pParams->chSup[ch].aDlyPs;
						pImg->rfe.fields.cdDelay_5 = pParams->chSup[ch].dDlyPs;
						pImg->r102.fields.drvMod_5 = pParams->chSup[ch].drvMode;
						if(pParams->chSup[ch].drvMode == HMC7043_CDM_CMOS) {
							pImg->rfa.fields.multSlpEn_5 = 1;
							addMultislip = chDivider/2;
							mDelay = pParams->chSup[ch].slipQuantumPs/clkInpFreq;
							multiSlip = mDelay + addMultislip;
							pImg->rff.fields.msDelayLsb_5 = (multiSlip & 0xff);
							pImg->r100.fields.msDelayMsb_5 = multiSlip >> 8;

						}
						if(pParams->chSup[ch].drvMode == HMC7043_CDM_CML) {
							if(pParams->chSup[ch].cmlTerm == HMC7043_CCIT_NONE)
								pImg->r102.fields.drvImp_5 = 0x0;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_100)
								pImg->r102.fields.drvImp_5 = 0x1;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_50)
								pImg->r102.fields.drvImp_5 = 0x3;
						}
						if(pParams->chSup[ch].chMode == HMC7043_CHM_CLK) {
							pImg->r102.fields.idlAtZero_5 = 0x00;
						}
						else if(pParams->chSup[ch].chMode ==
								HMC7043_CHM_SYSREF) {
							pImg->r102.fields.dyDrvEn_5 =
									pParams->chSup[ch].dynDriverEn? 1 : 0;
						}
						pImg->rfa.fields.chEn_5 = 0x1;
						if(pParams->chSup[ch].dynDriverEn)
							pImg->rfa.fields.stMod_5 = 0x3;
						else
							pImg->rfa.fields.stMod_5 = 0x0;

						if(pParams->chSup[ch].outSel == HMC7043_COS_FUNDAMENTAL)
							pImg->r101.fields.outMuxSel_5 = 0x3;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIVIDER)
							pImg->r101.fields.outMuxSel_5 = 0x0;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_ADLY)
							pImg->r101.fields.outMuxSel_5 = 0x1;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_NEIGHBOR)
							pImg->r101.fields.outMuxSel_5 = 0x2;
						break;
				}
				case 6: {
					    pImg->r105.fields.chDivLsb_6 =
					    		HMC7043_LSB_BIT_VAL(chDivider);
					    pImg->r106.fields.chDivMsb_6 =
					    		HMC7043_MSB_BIT_VAL(chDivider);
						pImg->r104.fields.hpMode_6 =
								pParams->chSup[ch].highPerfMode;
						pImg->r104.fields.syncEn_6 = 0x1;
						pImg->r104.fields.slipEn_6 = 0x1;
						pImg->r107.fields.faDelay_6 = pParams->chSup[ch].aDlyPs;
						pImg->r108.fields.cdDelay_6 = pParams->chSup[ch].dDlyPs;
						pImg->r10c.fields.drvMod_6 = pParams->chSup[ch].drvMode;
						if(pParams->chSup[ch].drvMode == HMC7043_CDM_CMOS) {
							pImg->r104.fields.multSlpEn_6 = 1;
							addMultislip = chDivider/2;
							mDelay = pParams->chSup[ch].slipQuantumPs/clkInpFreq;
							multiSlip = mDelay + addMultislip;
							pImg->r109.fields.msDelayLsb_6 = (multiSlip & 0xff);
							pImg->r10a.fields.msDelayMsb_6 = multiSlip >> 8;

						}
						if(pParams->chSup[ch].drvMode == HMC7043_CDM_CML) {
							if(pParams->chSup[ch].cmlTerm == HMC7043_CCIT_NONE)
								pImg->r10c.fields.drvImp_6 = 0x0;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_100)
								pImg->r10c.fields.drvImp_6 = 0x1;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_50)
								pImg->r10c.fields.drvImp_6 = 0x3;
						}
						if(pParams->chSup[ch].chMode == HMC7043_CHM_CLK) {
							pImg->r10c.fields.idlAtZero_6 = 0x00;
						}
						else if(pParams->chSup[ch].chMode ==
								HMC7043_CHM_SYSREF) {
							pImg->r10c.fields.dyDrvEn_6 =
									pParams->chSup[ch].dynDriverEn? 1 : 0;
						}
						pImg->r104.fields.chEn_6 = 0x1;
						if(pParams->chSup[ch].dynDriverEn)
							pImg->r104.fields.stMod_6 = 0x3;
						else
							pImg->r104.fields.stMod_6 = 0x0;

						if(pParams->chSup[ch].outSel == HMC7043_COS_FUNDAMENTAL)
							pImg->r10b.fields.outMuxSel_6 = 0x3;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIVIDER)
							pImg->r10b.fields.outMuxSel_6 = 0x0;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_ADLY)
							pImg->r10b.fields.outMuxSel_6 = 0x1;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_NEIGHBOR)
							pImg->r10b.fields.outMuxSel_6 = 0x2;
						break;
				}
				case 7: {
					    pImg->r10f.fields.chDivLsb_7 =
					    		HMC7043_LSB_BIT_VAL(chDivider);
					    pImg->r110.fields.chDivMsb_7 =
					    		HMC7043_MSB_BIT_VAL(chDivider);
					    pImg->r10e.fields.hpMode_7 =
					    		pParams->chSup[ch].highPerfMode;
						pImg->r10e.fields.syncEn_7 = 0x1;
						pImg->r10e.fields.slipEn_7 = 0x1;
						pImg->r111.fields.faDelay_7 = pParams->chSup[ch].aDlyPs;
						pImg->r112.fields.cdDelay_7 = pParams->chSup[ch].dDlyPs;
						pImg->r116.fields.drvMod_7 = pParams->chSup[ch].drvMode;
						if(pParams->chSup[ch].drvMode == HMC7043_CDM_CML) {
							if(pParams->chSup[ch].cmlTerm == HMC7043_CCIT_NONE)
								pImg->r116.fields.drvImp_7 = 0x0;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_100)
								pImg->r116.fields.drvImp_7 = 0x1;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_50)
								pImg->r116.fields.drvImp_7 = 0x3;
						}
						if(pParams->chSup[ch].chMode == HMC7043_CHM_CLK) {
							pImg->r116.fields.idlAtZero_7 = 0x00;
						}
						else if(pParams->chSup[ch].chMode == HMC7043_CHM_SYSREF) {
							pImg->r116.fields.dyDrvEn_7 =
									pParams->chSup[ch].dynDriverEn? 1 : 0;
						}
						pImg->r10e.fields.chEn_7 = 0x1;
						if(pParams->chSup[ch].dynDriverEn)
							pImg->r10e.fields.stMod_7 = 0x3;
						else
							pImg->r10e.fields.stMod_7 = 0x0;

						if(pParams->chSup[ch].outSel == HMC7043_COS_FUNDAMENTAL)
							pImg->r115.fields.outMuxSel_7 = 0x3;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIVIDER)
							pImg->r115.fields.outMuxSel_7 = 0x0;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_ADLY)
							pImg->r115.fields.outMuxSel_7 = 0x1;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_NEIGHBOR)
							pImg->r115.fields.outMuxSel_7 = 0x2;
						break;
				}
				case 8: {
					    pImg->r119.fields.chDivLsb_8 =
					    		HMC7043_LSB_BIT_VAL(chDivider);
					    pImg->r11a.fields.chDivMsb_8 =
					    		HMC7043_MSB_BIT_VAL(chDivider);
						pImg->r118.fields.hpMode_8 =
								pParams->chSup[ch].highPerfMode;
						pImg->r118.fields.syncEn_8 = 0x1;
						pImg->r118.fields.slipEn_8 = 0x1;
						pImg->r11b.fields.faDelay_8 = pParams->chSup[ch].aDlyPs;
						pImg->r11c.fields.cdDelay_8 = pParams->chSup[ch].dDlyPs;
						pImg->r120.fields.drvMod_8 = pParams->chSup[ch].drvMode;
						if(pParams->chSup[ch].drvMode == HMC7043_CDM_CML) {
							if(pParams->chSup[ch].cmlTerm == HMC7043_CCIT_NONE)
								pImg->r120.fields.drvImp_8 = 0x0;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_100)
								pImg->r120.fields.drvImp_8 = 0x1;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_50)
								pImg->r120.fields.drvImp_8 = 0x3;
						}
						if(pParams->chSup[ch].chMode == HMC7043_CHM_CLK) {
							pImg->r120.fields.idlAtZero_8 = 0x00;
						}
						else if(pParams->chSup[ch].chMode ==
								HMC7043_CHM_SYSREF) {
							pImg->r120.fields.dyDrvEn_8 =
									pParams->chSup[ch].dynDriverEn? 1 : 0;
						}
						pImg->r118.fields.chEn_8 = 0x1;
						if(pParams->chSup[ch].dynDriverEn)
							pImg->r118.fields.stMod_8 = 0x3;
						else
							pImg->r118.fields.stMod_8 = 0x0;

						if(pParams->chSup[ch].outSel == HMC7043_COS_FUNDAMENTAL)
							pImg->r11f.fields.outMuxSel_8 = 0x3;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIVIDER)
							pImg->r11f.fields.outMuxSel_8 = 0x0;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_ADLY)
							pImg->r11f.fields.outMuxSel_8 = 0x1;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_NEIGHBOR)
							pImg->r11f.fields.outMuxSel_8 = 0x2;
						break;
				}
				case 9: {
					    pImg->r123.fields.chDivLsb_9 =
					    		HMC7043_LSB_BIT_VAL(chDivider);
					    pImg->r124.fields.chDivMsb_9 =
					    		HMC7043_MSB_BIT_VAL(chDivider);
						pImg->r122.fields.hpMode_9 =
								pParams->chSup[ch].highPerfMode;
						pImg->r122.fields.syncEn_9 = 0x1;
						pImg->r122.fields.slipEn_9 = 0x1;
						pImg->r125.fields.faDelay_9 = pParams->chSup[ch].aDlyPs;
						pImg->r126.fields.cdDelay_9 = pParams->chSup[ch].dDlyPs;
						pImg->r12a.fields.drvMod_9 = pParams->chSup[ch].drvMode;
						if(pParams->chSup[ch].drvMode == HMC7043_CDM_CMOS) {
							pImg->r122.fields.multSlpEn_9 = 1;
							addMultislip = chDivider/2;
							mDelay = pParams->chSup[ch].slipQuantumPs/clkInpFreq;
							multiSlip = mDelay + addMultislip;
							pImg->r127.fields.msDelayLsb_9 = (multiSlip & 0xff);
							pImg->r128.fields.msDelayMsb_9 = multiSlip >> 8;
						}
						if(pParams->chSup[ch].drvMode == HMC7043_CDM_CML) {
							if(pParams->chSup[ch].cmlTerm == HMC7043_CCIT_NONE)
								pImg->r12a.fields.drvImp_9 = 0x0;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_100)
								pImg->r12a.fields.drvImp_9 = 0x1;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_50)
								pImg->r12a.fields.drvImp_9 = 0x3;
						}
						if(pParams->chSup[ch].chMode == HMC7043_CHM_CLK) {
							pImg->r12a.fields.idlAtZero_9 = 0x00;
						}
						else if(pParams->chSup[ch].chMode ==
								HMC7043_CHM_SYSREF) {
							pImg->r12a.fields.dyDrvEn_9 =
									pParams->chSup[ch].dynDriverEn? 1 : 0;
						}
						pImg->r122.fields.chEn_9 = 0x1;
						if(pParams->chSup[ch].dynDriverEn)
							pImg->r122.fields.stMod_9 = 0x3;
						else
							pImg->r122.fields.stMod_9 = 0x0;

						if(pParams->chSup[ch].outSel == HMC7043_COS_FUNDAMENTAL)
							pImg->r129.fields.outMuxSel_9 = 0x3;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIVIDER)
							pImg->r129.fields.outMuxSel_9 = 0x0;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_ADLY)
							pImg->r129.fields.outMuxSel_9 = 0x1;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_NEIGHBOR)
							pImg->r129.fields.outMuxSel_9 = 0x2;
						break;
				}
				case 10: {
					     pImg->r12d.fields.chDivLsb_10 =
					    		 HMC7043_LSB_BIT_VAL(chDivider);
                         pImg->r12e.fields.chDivMsb_10 =
                        		 HMC7043_MSB_BIT_VAL(chDivider);
						 pImg->r12c.fields.hpMode_10 =
								 pParams->chSup[ch].highPerfMode;
						 pImg->r12c.fields.syncEn_10 = 0x1;
						 pImg->r12c.fields.slipEn_10 = 0x1;
						 pImg->r12f.fields.faDelay_10 = pParams->chSup[ch].aDlyPs;
						 pImg->r130.fields.cdDelay_10 = pParams->chSup[ch].dDlyPs;
						 pImg->r134.fields.drvMod_10 = pParams->chSup[ch].drvMode;
						 if(pParams->chSup[ch].drvMode == HMC7043_CDM_CMOS) {
							pImg->r12c.fields.multSlpEn_10 = 1;
							addMultislip = chDivider/2;
							mDelay = pParams->chSup[ch].slipQuantumPs/clkInpFreq;
							multiSlip = mDelay + addMultislip;
							pImg->r131.fields.msDelayLsb_10 = (multiSlip & 0xff);
							pImg->r132.fields.msDelayMsb_10 = multiSlip >> 8;
						 }
						 if(pParams->chSup[ch].drvMode == HMC7043_CDM_CML) {
							if(pParams->chSup[ch].cmlTerm == HMC7043_CCIT_NONE)
								pImg->r134.fields.drvImp_10 = 0x0;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_100)
								pImg->r134.fields.drvImp_10 = 0x1;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_50)
								pImg->r134.fields.drvImp_10 = 0x3;
						 }
						 if(pParams->chSup[ch].chMode == HMC7043_CHM_CLK) {
						 	pImg->r134.fields.idlAtZero_10 = 0x00;
						 }
						 else if(pParams->chSup[ch].chMode ==
								 HMC7043_CHM_SYSREF) {
						 	pImg->r134.fields.dyDrvEn_10 =
						 			pParams->chSup[ch].dynDriverEn? 1 : 0;
						 }
						 pImg->r12c.fields.chEn_10 = 0x1;
						if(pParams->chSup[ch].dynDriverEn)
							pImg->r12c.fields.stMod_10 = 0x3;
						else
							pImg->r12c.fields.stMod_10 = 0x0;

						if(pParams->chSup[ch].outSel == HMC7043_COS_FUNDAMENTAL)
							pImg->r133.fields.outMuxSel_10 = 0x3;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIVIDER)
							pImg->r133.fields.outMuxSel_10 = 0x0;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_ADLY)
							pImg->r133.fields.outMuxSel_10 = 0x1;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_NEIGHBOR)
							pImg->r133.fields.outMuxSel_10 = 0x2;
						 break;
				}
				case 11: {
					     pImg->r137.fields.chDivLsb_11 =
					    		 HMC7043_LSB_BIT_VAL(chDivider);
					     pImg->r138.fields.chDivMsb_11 =
					    		 HMC7043_MSB_BIT_VAL(chDivider);
						 pImg->r136.fields.hpMode_11 =
								 pParams->chSup[ch].highPerfMode;
						 pImg->r136.fields.syncEn_11 = 0x1;
						 pImg->r136.fields.slipEn_11 = 0x1;
						 pImg->r139.fields.faDelay_11 = pParams->chSup[ch].aDlyPs;
						 pImg->r13a.fields.cdDelay_11 = pParams->chSup[ch].dDlyPs;
						 pImg->r13e.fields.drvMod_11 = pParams->chSup[ch].drvMode;
						if(pParams->chSup[ch].drvMode == HMC7043_CDM_CML) {
							if(pParams->chSup[ch].cmlTerm == HMC7043_CCIT_NONE)
								pImg->r13e.fields.drvImp_11 = 0x0;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_100)
								pImg->r13e.fields.drvImp_11 = 0x1;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_50)
								pImg->r13e.fields.drvImp_11 = 0x3;
						}
						 if(pParams->chSup[ch].chMode == HMC7043_CHM_CLK) {
						 	pImg->r13e.fields.idlAtZero_11 = 0x00;
						 }
						 else if(pParams->chSup[ch].chMode == HMC7043_CHM_SYSREF) {
						 	pImg->r13e.fields.dyDrvEn_11 =
						 			pParams->chSup[ch].dynDriverEn? 1 : 0;
						 }
						 pImg->r136.fields.chEn_11 = 0x1;
						if(pParams->chSup[ch].dynDriverEn)
							pImg->r136.fields.stMod_11 = 0x3;
						else
							pImg->r136.fields.stMod_11 = 0x0;

						if(pParams->chSup[ch].outSel == HMC7043_COS_FUNDAMENTAL)
							pImg->r13d.fields.outMuxSel_11 = 0x3;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIVIDER)
							pImg->r13d.fields.outMuxSel_11 = 0x0;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_ADLY)
							pImg->r13d.fields.outMuxSel_11 = 0x1;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_NEIGHBOR)
							pImg->r13d.fields.outMuxSel_11 = 0x2;
						 break;
				}
				case 12: {
					     pImg->r141.fields.chDivLsb_12 =
					    		 HMC7043_LSB_BIT_VAL(chDivider);
					     pImg->r142.fields.chDivMsb_12 =
					    		 HMC7043_MSB_BIT_VAL(chDivider);
						 pImg->r140.fields.hpMode_12 =
								 pParams->chSup[ch].highPerfMode;
						 pImg->r140.fields.syncEn_12 = 0x1;
						 pImg->r140.fields.slipEn_12 = 0x1;
						 pImg->r143.fields.faDelay_12 = pParams->chSup[ch].aDlyPs;
						 pImg->r144.fields.cdDelay_12 = pParams->chSup[ch].dDlyPs;
						 pImg->r148.fields.drvMod_12 = pParams->chSup[ch].drvMode;
						if(pParams->chSup[ch].drvMode == HMC7043_CDM_CML) {
							if(pParams->chSup[ch].cmlTerm == HMC7043_CCIT_NONE)
								pImg->r148.fields.drvImp_12 = 0x0;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_100)
								pImg->r148.fields.drvImp_12 = 0x1;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_50)
								pImg->r148.fields.drvImp_12 = 0x3;
						}
						 if(pParams->chSup[ch].chMode == HMC7043_CHM_CLK) {
						 	pImg->r148.fields.idlAtZero_12 = 0x00;
						 }
						 else if(pParams->chSup[ch].chMode == HMC7043_CHM_SYSREF) {
						 	pImg->r148.fields.dyDrvEn_12 =
						 			pParams->chSup[ch].dynDriverEn? 1 : 0;
						 }
						 pImg->r140.fields.chEn_12 = 0x1;
						if(pParams->chSup[ch].dynDriverEn)
							pImg->r140.fields.stMod_12 = 0x3;
						else
							pImg->r140.fields.stMod_12 = 0x0;

						if(pParams->chSup[ch].outSel == HMC7043_COS_FUNDAMENTAL)
							pImg->r147.fields.outMuxSel_12 = 0x3;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIVIDER)
							pImg->r147.fields.outMuxSel_12 = 0x0;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_ADLY)
							pImg->r147.fields.outMuxSel_12 = 0x1;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_NEIGHBOR)
							pImg->r147.fields.outMuxSel_12 = 0x2;
						 break;
				}
				case 13: {
					     pImg->r14b.fields.chDivLsb_13 =
					    		 HMC7043_LSB_BIT_VAL(chDivider);
					     pImg->r14c.fields.chDivMsb_13 =
					    		 HMC7043_MSB_BIT_VAL(chDivider);
						 pImg->r14a.fields.hpMode_13 =
								 pParams->chSup[ch].highPerfMode;
						 pImg->r14a.fields.syncEn_13 = 0x1;
						 pImg->r14a.fields.slipEn_13 = 0x1;
						 pImg->r14d.fields.faDelay_13 = pParams->chSup[ch].aDlyPs;
						 pImg->r14e.fields.cdDelay_13 = pParams->chSup[ch].dDlyPs;
						 pImg->r152.fields.drvMod_13 = pParams->chSup[ch].drvMode;
						 if(pParams->chSup[ch].drvMode == HMC7043_CDM_CMOS) {
							pImg->r14a.fields.multSlpEn_13 = 1;
							addMultislip = chDivider/2;
							mDelay = pParams->chSup[ch].slipQuantumPs/clkInpFreq;
							multiSlip = mDelay + addMultislip;
							pImg->r14f.fields.msDelayLsb_13 = (multiSlip & 0xff);
							pImg->r150.fields.msDelayMsb_13 = multiSlip >> 8;
						 }
						 if(pParams->chSup[ch].drvMode == HMC7043_CDM_CML) {
							if(pParams->chSup[ch].cmlTerm == HMC7043_CCIT_NONE)
								pImg->r152.fields.drvImp_13 = 0x0;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_100)
								pImg->r152.fields.drvImp_13 = 0x1;
							else if(pParams->chSup[ch].cmlTerm ==
									HMC7043_CCIT_50)
								pImg->r152.fields.drvImp_13 = 0x3;
						 }
						 if(pParams->chSup[ch].chMode == HMC7043_CHM_CLK) {
						 	pImg->r152.fields.idlAtZero_13 = 0x00;
						 }
						 else if(pParams->chSup[ch].chMode ==
								 HMC7043_CHM_SYSREF) {
						 	pImg->r152.fields.dyDrvEn_13 =
						 			pParams->chSup[ch].dynDriverEn? 1 : 0;
						 }
						 pImg->r14a.fields.chEn_13 = 0x1;
						if(pParams->chSup[ch].dynDriverEn)
							pImg->r14a.fields.stMod_13 = 0x3;
						else
							pImg->r14a.fields.stMod_13 = 0x0;

						if(pParams->chSup[ch].outSel == HMC7043_COS_FUNDAMENTAL)
							pImg->r151.fields.outMuxSel_13 = 0x3;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIVIDER)
							pImg->r151.fields.outMuxSel_13 = 0x0;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_ADLY)
							pImg->r151.fields.outMuxSel_13 = 0x1;
						else if(pParams->chSup[ch].outSel ==
								HMC7043_COS_DIV_NEIGHBOR)
							pImg->r151.fields.outMuxSel_13 = 0x2;
						 break;
				}
			}
		}  else {/* Unused channel */
		/*	set start-up mode to 00 */
		    switch(ch){
		    	case 0: {
		    		pImg->rc8.fields.stMod_0 = 0x0;
		    		break;
		    	}
		    	case 1: {
		    		pImg->rd2.fields.stMod_1 = 0x0;
		    		break;
		    	}
		    	case 2: {
		    		pImg->rdc.fields.stMod_2 = 0x0;
		    		break;
		    	}
		    	case 3: {
		    		pImg->re6.fields.stMod_3 = 0x0;
		    		break;
		    	}
		    	case 4: {
		    		pImg->rf0.fields.stMod_4 = 0x0;
		    		break;
		    	}
		    	case 5: {
		    		pImg->rfa.fields.stMod_5 = 0x0;
		    		break;
		    	}
		    	case 6: {
		    		pImg->r104.fields.stMod_6 = 0x0;
		    		break;
		    	}
		    	case 7: {
		    		pImg->r10e.fields.stMod_7 = 0x0;
		    		break;
		    	}
		    	case 8: {
		    		pImg->r118.fields.stMod_8 = 0x0;
		    		break;
		    	}
		    	case 9: {
		    		pImg->r122.fields.stMod_9 = 0x0;
		    		break;
		    	}
		    	case 10: {
		    		pImg->r12c.fields.stMod_10 = 0x0;
		    		break;
		    	}
		    	case 11: {
		    		pImg->r136.fields.stMod_11 = 0x0;
		    		break;
		    	}
		    	case 12: {
		    		pImg->r140.fields.stMod_12 = 0x0;
		    		break;
		    	}
		    	case 13: {
		    		pImg->r14a.fields.stMod_13 = 0x0;
		    		break;
		    	}
		    }

		}
	}

	return OK;
}




/*******************************************************************************
* - name: hmc7043AppInitPgmInCh
*
* - title: Program the Input CLK/RFSYNC
*
* - input: dev - CLKDST device for which to perform the operation
*          pParams - pointer to device setup parameters
*
* - output: hmc7043AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7043AppInitPgmInCh(CKDST_DEV dev,
		                          const Hmc7043_app_dev_params *pParams)
{
	Hmc7043_reg_image *pImg;

	if (!inEnumRange(dev, NELEMENTS(hmc7043AppState.devState)) || !pParams) {
		sysLog("bad argument(s) (dev %d, pParams %d)", dev, pParams != NULL);
		return ERROR;
	}

	pImg = &hmc7043AppState.devState[dev].regImage;

    if(pParams->clkIn.used) {
    	pImg->r0a.fields.enBuff = 0x1;
        if(pParams->clkIn.term100Ohm)
        	pImg->r0a.fields.inBufMod |= 0x0;
        if(pParams->clkIn.acCoupled)
        	pImg->r0a.fields.inBufMod |= 0x1;
        if(pParams->clkIn.lvpecl)
        	pImg->r0a.fields.inBufMod |= 0x2;
        if(pParams->clkIn.highZ)
        	pImg->r0a.fields.inBufMod |= 0x3;
    } else { //TBD need to check if this should be in else or separate if
    	pImg->r0a.fields.enBuff = 0x0;
    	if(pParams->syncIn.used){
    		pImg->r0b.fields.enBuff = 0x1;
    		if(pParams->syncIn.term100Ohm)
    			pImg->r0b.fields.inBufMod |= 0x0;
            if(pParams->clkIn.acCoupled)
            	pImg->r0b.fields.inBufMod |= 0x1;
            if(pParams->clkIn.lvpecl)
            	pImg->r0b.fields.inBufMod |= 0x2;
            if(pParams->clkIn.highZ)
            	pImg->r0b.fields.inBufMod |= 0x3;
        }
    	else
        	pImg->r0b.fields.enBuff = 0x0;
    }

	return OK;
}




/*******************************************************************************
* - name: hmc7043GetClkOutPhase
*
* - title: Get the clock output phase alarm status.
*
* - input: dev         - CLKDST device for which to perform the operation
*          clkOutPhase - out parameter containing alram status
*
* - output: clkOutPhase
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7043GetClkOutPhase(CKDST_DEV dev, Bool *clkOutPhase)
{
	Hmc7043_reg_x007d r7d;

	if (!inEnumRange(dev, NELEMENTS(hmc7043AppState.devState))) {
		sysLog("bad argument (dev %d)", dev);
		return ERROR;
	}

	if(hmc7043LliRegRead(dev, 0x007d, &r7d.all) != OK)
		return ERROR;

    if(r7d.fields.ckOutPhSt == 1)
    	*clkOutPhase = TRUE;
    else
    	*clkOutPhase = FALSE;

    return OK;
}




/*******************************************************************************
* - name: hmc7043CfgSdataMode
*
* - title: Configure SDATA mode for a particular device.
*
* - input: dev      - CLKDST device on which operation is performed.
*          pParams  - pointer to device setup parameters
*
* - output: hmc7043AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7043CfgSdataMode(CKDST_DEV dev,
		                         const Hmc7043_app_dev_params *pParams)
{
	Hmc7043_reg_image *pImg;

	if (!inEnumRange(dev, NELEMENTS(hmc7043AppState.devState)) || !pParams) {
		sysLog("bad argument(s) (dev %d, pParams %d)", dev, pParams != NULL);
		return ERROR;
	}

	pImg = &hmc7043AppState.devState[dev].regImage;

	switch(pParams->sdataMode) {
		case HMC7043_OM_OD: {
			pImg->r54.fields.sdataMod = 0x0;
			pImg->r54.fields.enSdata = 0x1;
			break;
		}
		case HMC7032_OM_CMOS: {
			pImg->r54.fields.sdataMod = 0x1;
			pImg->r54.fields.enSdata = 0x1;
			break;
		}
	}

	return OK;
}




/*******************************************************************************
* - name: hmc7043CfgGpio
*
* - title: Configure GPIO setup for a particular device.
*
* - input: dev     - CLKDST device on which operation is performed.
*          pParams - pointer to device setup parameters
*
* - output: hmc7043AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - notes: the operation is interlocked via the associated critical section
*******************************************************************************/
LOCAL STATUS hmc7043CfgGpio(CKDST_DEV dev,
		                    const Hmc7043_app_dev_params *pParams)
{
	Hmc7043_reg_image *pImg;

	if (!inEnumRange(dev, NELEMENTS(hmc7043AppState.devState)) || !pParams) {
		sysLog("bad argument(s) (dev %d, pParams %d)", dev, pParams != NULL);
		return ERROR;
	}

	pImg = &hmc7043AppState.devState[dev].regImage;

	switch(pParams->gpiSup) {
		case HMC7043_GPIS_NONE: {
			/*  If application configures the GPI line to HMC7043_GPIS_NONE,
			 *  software shall disable the GPI function in register 0x0046.
			 *  Disabled  before switch case.
			 */
			break;
		}
		case HMC7043_GPIS_SLEEP: {
			pImg->r46.fields.selGpi = 0x2;
			pImg->r46.fields.enGpi = 0x1;
			break;
		}
		case HMC7043_GPIS_MUTE: {
			pImg->r46.fields.selGpi = 0x3;
			pImg->r46.fields.enGpi = 0x1;
			break;
		}
		case HMC7043_GPIS_PULSE_GEN: {
			pImg->r46.fields.selGpi = 0x4;
			pImg->r46.fields.enGpi = 0x1;
			break;
		}
		case HMC7043_GPIS_RESEED: {
			pImg->r46.fields.selGpi = 0x5;
			pImg->r46.fields.enGpi = 0x1;
			break;
		}
		case HMC7043_GPIS_RESTART: {
			pImg->r46.fields.selGpi = 0x6;
			pImg->r46.fields.enGpi = 0x1;
			break;
		}
		case HMC7043_GPIS_SLIP: {
			pImg->r46.fields.selGpi = 0x8;
			pImg->r46.fields.enGpi = 0x1;
		    break;
		}
		default: {
			sysLog("Bad value ( GPI setup %d)", pParams->gpiSup);
			return ERROR;
		}
	}

	switch(pParams->gpoMode) {
		case HMC7043_OM_OD: {
			pImg->r50.fields.gpoMod = 0x0;
			break;
		}
		case HMC7032_OM_CMOS: {
			pImg->r50.fields.gpoMod = 0x1;
			break;
		}
	}

	switch(pParams->gpoSup) {
		case HNC7043_GPOS_NONE: {
			/*  If application configures the GPO line to HMC7043_GPOS_NONE,
			 *  software shall disable the GPO function in register 0x0046.
			 */
			pImg->r50.fields.enGp0 = 0x0;
			break;
		}
		case HMC7043_GPOS_ALARM: {
			pImg->r50.fields.selGpo = 0x0;
			pImg->r50.fields.enGp0 = 0x1;
			break;
		}
		case HMC7043_GPOS_SDATA: {
			pImg->r50.fields.selGpo = 0x1;
			pImg->r50.fields.enGp0 = 0x1;
			break;
		}
		case HMC7043_GPOS_SREF_NSYNC: {
			pImg->r50.fields.selGpo = 0x2;
			pImg->r50.fields.enGp0 = 0x1;
			break;
		}
		case HMC7043_GPOS_CKOUTS_PHASE: {
			pImg->r50.fields.selGpo = 0x3;
			pImg->r50.fields.enGp0 = 0x1;
			break;
		}
		case HMC7043_GPOS_SYNC_REQ_ST: {
			pImg->r50.fields.selGpo = 0x4;
			pImg->r50.fields.enGp0 = 0x1;
			break;
		}
		case HMC7043_GPOS_CH_FSM_BUSY: {
			pImg->r50.fields.selGpo = 0x5;
			pImg->r50.fields.enGp0 = 0x1;
			break;
		}
		case HMC7043_GPOS_SREF_FSM_ST0: {
			pImg->r50.fields.selGpo = 0x6;
			pImg->r50.fields.enGp0 = 0x1;
			break;
		}
		case HMC7043_GPOS_SREF_FSM_ST1: {
			pImg->r50.fields.selGpo = 0x7;
			pImg->r50.fields.enGp0 = 0x1;
			break;
		}
		case HMC7043_GPOS_SREF_FSM_ST2: {
			pImg->r50.fields.selGpo = 0x8;
			pImg->r50.fields.enGp0 = 0x1;
			break;
		}
		case HMC7043_GPOS_SREF_FSM_ST3: {
			pImg->r50.fields.selGpo = 0x9;
			pImg->r50.fields.enGp0 = 0x1;
			break;
		}
		case HMC7043_GPOS_FORCE_1: {
			pImg->r50.fields.selGpo = 0xa;
			pImg->r50.fields.enGp0 = 0x1;
			break;
		}
		case HMC7043_GPOS_FORCE_0: {
			pImg->r50.fields.selGpo = 0xb;
			pImg->r50.fields.enGp0 = 0x1;
			break;
		}
		case HMC7043_GPOS_PLS_GEN_REQ: {
			pImg->r50.fields.selGpo = 0x19;
			pImg->r50.fields.enGp0 = 0x1;
			break;
		}
		default: {
			sysLog("Bad value ( GPO setup %d)", pParams->gpoSup);
			return ERROR;
		}
	}

	return OK;
}




/*******************************************************************************
* - name: hmc7043DisSync
*
* - title: Disables SYNC on all output channels
*
* - input: dev - CLKDST device for which to perform the operation
*
* - output: hmc7043AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7043DisSync(CKDST_DEV dev, const Hmc7043_app_dev_params *pParams)
{
	Hmc7043_reg_image *pImg;
	unsigned ch;

	if (!inEnumRange(dev, NELEMENTS(hmc7043AppState.devState)) || !pParams) {
		sysLog("bad argument(s) (dev %d, pParams %d)", dev, pParams != NULL);
		return ERROR;
	}

	pImg = &hmc7043AppState.devState[dev].regImage;

	for (ch = 0; ch < HMC7043_OUT_NCHAN; ch++) {
		if(pParams->chSup[ch].chMode != HMC7043_CHM_UNUSED) {
			switch(ch){
				case 0: {
					pImg->rc8.fields.syncEn_0= 0x0;
					break;
				}
				case 1: {
					pImg->rd2.fields.syncEn_1 = 0x0;
					break;
				}
				case 2: {
					pImg->rdc.fields.syncEn_2 = 0x0;
					break;
				}
				case 3: {
					pImg->re6.fields.syncEn_3 = 0x0;
					break;
				}
				case 4: {
					pImg->rf0.fields.syncEn_4 = 0x0;
					break;
				}
				case 5: {
					pImg->rfa.fields.syncEn_5 = 0x0;
					break;
				}
				case 6: {
					pImg->r104.fields.syncEn_6 = 0x0;
					break;
				}
				case 7: {
					pImg->r10e.fields.syncEn_7 = 0x0;
					break;
				}
				case 8: {
					pImg->r118.fields.syncEn_8 = 0x0;
					break;
				}
				case 9: {
					pImg->r122.fields.syncEn_9 = 0x0;
					break;
				}
				case 10: {
					pImg->r12c.fields.syncEn_10 = 0x0;
					break;
				}
				case 11: {
					pImg->r136.fields.syncEn_11 = 0x0;
					break;
				}
				case 12: {
					pImg->r140.fields.syncEn_12 = 0x0;
					break;
				}
				case 13: {
					pImg->r14a.fields.syncEn_13 = 0x0;
					break;
				}
			}
		}
	}
	return OK;
}




/*******************************************************************************
* - name: hmc7043AppInitPgmPlGnMd
*
* - title: Program the Pulse Generator Mode
*
* - input: dev - CLKDST device for which to perform the operation
*          pParams - pointer to device setup parameters
*
* - output: hmc7043AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*******************************************************************************/
LOCAL STATUS hmc7043AppInitPgmPlGnMd(CKDST_DEV dev,
		                          const Hmc7043_app_dev_params *pParams)
{
	Hmc7043_reg_image *pImg;

	if (!inEnumRange(dev, NELEMENTS(hmc7043AppState.devState)) || !pParams) {
		sysLog("bad argument(s) (dev %d, pParams %d)", dev, pParams != NULL);
		return ERROR;
	}

	pImg = &hmc7043AppState.devState[dev].regImage;

	/* Set pulse generation mode */
	switch(pParams->sysref.mode)
	{
		case HMC7043_SRM_CONTINUOUS: {
			pImg->r5a.fields.pulseMode = 0x7;
			break;
		}
		case HMC7043_SRM_LEVEL_CTL: {
			pImg->r5a.fields.pulseMode = 0x0;
			break;
		}
		case HMC7043_SRM_PULSED: {
			switch(pParams->sysref.nPulses)
			{
				case HMC7043_SRNP_1: {
					pImg->r5a.fields.pulseMode = 0x1;
					break;
				}
				case HMC7043_SRNP_2: {
					pImg->r5a.fields.pulseMode = 0x2;
					break;
				}
				case HMC7043_SRNP_4: {
					pImg->r5a.fields.pulseMode = 0x3;
					break;
				}
				case HMC7043_SRNP_8: {
					pImg->r5a.fields.pulseMode = 0x4;
					break;
				}
				case HMC7043_SRNP_16: {
					pImg->r5a.fields.pulseMode = 0x5;
					break;
				}
				default: {
					sysLog("Bad value ( pParams->sysref.nPulses %d)",
							pParams->sysref.nPulses);
					return ERROR;
				}
			}
			break;
		}
		default:
			sysLog("Bad value ( pParams->sysref.mode %d)",
				   pParams->sysref.mode);
			return ERROR;
	}
	return OK;
}




/*******************************************************************************
* - name: hmc7043AppInitAppSup
*
* - title: initialize CLK device as per 'typical programming sequence'
*          mentioned in data sheet.
*
* - input: dev     - CLKDST device for which to perform the operation
*          pParams - pointer to device setup parameters
*
* - output: hmc7043AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - notes: Step 1&2: these are to be done on hardware, soft reset, GPIO, SDATA
*                    setup will be done in this function
*          Step 3: Load the configuration updates (provided by Analog
*                  Devices, Inc.) to specific registers (see Table 40).
*                  This is done in hmc7043LoadConfigUpd.
*          Step 4: Program SYSREF timer, set divide ratio, set the pulse
*                  generation mode configuration.
*          Step 5: Program output channels.set divide ratio,channel start-up mode
*                  coarse/analog delays and performance mode.
*          Step 6: Ensure clock input signal is given to
*                  CLKIN(to be done on hardware.
*          Step 7: Issue a software restart to reset the system and initiate
*                  calibration. Toggle the restart dividers/FSMs bit to 1 and
*                  then back to 0.
*          Step 8: Send a sync request via the SPI (set the reseed request bit)
*                  to align the divider phases and send any initial pulse
*                  generator stream.
*          Step 9: Wait 6 SYSREF periods.
*          Step 10: Confirm that the outputs have all reached their phases by
*                   checking that the clock outputs phases status bit = 1.*
*          Step 11: Initialize other devices in the system. SYSREF channels can
*                   be either on asynchronous/dynamic and may turn on for pulse
*                   generation stream.
*          Step 12: Slave JESD204B devices in the system must be configured
*                   to monitor the input SYSREF signal exported from the HMC7043
*          Step 13: When all JESD204B slaves are powered and ready, send a
*                   pulse generator request to send out a pulse generator
*                   chain on any SYSREF channels programmed for pulse
*                   generator mode.
*******************************************************************************/
LOCAL STATUS hmc7043AppInitAppSup(CKDST_DEV dev,
                                  const Hmc7043_app_dev_params *pParams)
{
	const Hmc7043_app_dev_ctl *pCtl;
	Hmc7043_reg_image *pImg;
	unsigned i;
	Bool r65Done = FALSE, clkOutPhase = FALSE;

	if (!inEnumRange(dev, NELEMENTS(hmc7043AppState.devState)) || !pParams) {
	    sysLog("bad argument(s) (dev %d, pParams %d)", dev, pParams != NULL);
	    return ERROR;
	}

	pCtl = hmc7043AppCtl.devCtl + dev;

	pImg = &hmc7043AppState.devState[dev].regImage;

	if (!hmc7043IfCtl.initDone || !hmc7043AppCtl.initDone || !pCtl->initDone) {
	    sysLog("initialization not done yet (dev %d, init. done %d,%d,%d)", dev,
	            hmc7043IfCtl.initDone, hmc7043AppCtl.initDone, pCtl->initDone);
        return ERROR;
    }
	/* Issue software restart to reset system */
	if(hmc7043ToggleBit(dev,HMC7043_REG_IDX_SOFT_RESET, HMC7043_SFT_RST_BIT,
					200) != OK)
		return ERROR;

	if(hmc7043CfgGpio(dev, pParams) != OK)
		return ERROR;

	if(hmc7043CfgSdataMode(dev, pParams) != OK)
		return ERROR;

	/* Set sysref timer */
	if(hmc7043AppInitPgmSysrefTimer(dev, pParams) != OK)
		return ERROR;

	/* Program Pulse generator mode */
    if(hmc7043AppInitPgmPlGnMd(dev, pParams) != OK)
    	return ERROR;

    /* Program output channels */
    if(hmc7043AppInitPgmOutCh(dev, pParams) != OK)
    	return ERROR;

    /* Program Input CLK/RFSYNC */
    if(hmc7043AppInitPgmInCh(dev, pParams) != OK)
    	return ERROR;

    /* Software shall set register 0x0064 bit 0 if
     *  input clock frequency is < 1 GHz. */
    if(pParams->clkInFreq < 1000000000LL) {
    	pImg->r64.fields.lfClkInp = 0;
    }

    /* Software shall set register 0x0065 bit 0 to 0,
     * except if no output channel is using analog delay. */
    for(i = 0; i < HMC7043_OUT_NCHAN; i++) {
    	if(pParams->chSup[i].aDlyPs > 0) {
    		pImg->r65.fields.aDelLowPowMo = 0;
    		r65Done = TRUE;
    		break;
    	}
    }
    if(!r65Done)
    	pImg->r65.fields.aDelLowPowMo = 1;

    /* Software shall set register 0x0001 bit 6 (high performance
     * distribution path) in all cases */
    pImg->r01.fields.highPrfPath = 1;

    /* Write the changes to registers */
    if (hmc7043AppInitWrRegs(dev) != OK)
        return ERROR;

    /* Issue software restart to reset system and start calibration */
    if(hmc7043ToggleBit(dev, HMC7043_REG_IDX_SOFT_RESET, HMC7043_SFT_RST_BIT,
    		         200) != OK)
    	return ERROR;

    /*Toggle the restart dividers/FSMs bit to 1 and then back to 0.*/
    if(hmc7043ToggleBit(dev,HMC7043_REG_IDX_REQ_MOD, HMC7043_FSM_DIV_RESET_BIT,
    		        100) != OK)
    	return ERROR;

    /* Send a sync request via the SPI (set the reseed request bit) */
    if(hmc7043ToggleBit(dev, HMC7043_REG_IDX_REQ_MOD, HMC7043_RESEED_BIT, 100)
    		!= OK)
    	return ERROR;

    /* Send any initial Pulse generator stream */
    if(hmc7043ToggleBit(dev, HMC7043_REG_IDX_REQ_MOD, HMC7043_PULS_GEN_BIT, 100)
    		             != OK)
    	return ERROR;

    /* Wait for 6xSYSREF period */
    if(hmc7043WaitSysrefPeriod(dev, HMC7043_INIT_WAIT_TIMES) != OK)
    	return ERROR;

    /* Check if clock output phase is set*/
    if(hmc7043GetClkOutPhase(dev, clkOutPhase) != OK)
    	return ERROR;

    if(!clkOutPhase)
    	return ERROR; // TBD : need to confirm what has to be done.

    /* After completed the initialization sequence, software shall
     * disable SYNC on all channels. */
    if(hmc7043DisSync(dev, pParams) != OK)
    	return ERROR;

    /* Application to call hmc7043SysrefSwPulseN when all slaves are ready */

    return OK;
}




/*******************************************************************************
* - name: hmc7043AppInitDevAct
*
* - title: actually initialize a CLKDST device
*
* - input: dev     - CLKDST device for which to perform the operation
*          pParams - pointer to device setup parameters
*
* - output: hmc7043AppState.devState[dev]
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - notes: 1) This routine can be called more than once (for a device).
*          2) It is assumed that hmc7043AppCtl.devCtl[dev] has already been setup.
*          3) Not attempting to interlock the programming sequence here - if such
*             interlocking is necessary, it must be provided by the caller.
*******************************************************************************/
LOCAL STATUS hmc7043AppInitDevAct(CKDST_DEV dev,
                                  const Hmc7043_app_dev_params *pParams)
{
    Hmc7043_app_dev_state *pState;

    /* initialize */
    if (!inEnumRange(dev, NELEMENTS(hmc7043AppCtl.devCtl)) || !pParams) {
        sysLog("bad argument(s) (dev %d, pParams %d)", dev, pParams != NULL);
        return ERROR;
    }

    pState = hmc7043AppState.devState + dev;

    if (!hmc7043AppCtl.initDone) {
        sysLog("control data initialization not done yet (dev %d)", dev);
        return ERROR;
    }

    /* verify the expected device id(s) */
    if (hmc7043AppChkProdId(dev) != OK)
        return ERROR;

    memset(pState, 0, sizeof(*pState));

    /* Load configuration update to register as in Table-40 of data sheet */
    if(hmc7043LoadConfigUpd(dev) != OK)
    	return ERROR;

    if(hmc7043AppInitReservedReg(dev) != OK)
    	return ERROR;

   if(hmc7043AppInitReservdFields(dev) != OK)
    	return ERROR;



    /* write initialization image to device registers */
    if (hmc7043AppInitWrRegs(dev) != OK)
        return ERROR;

    pState->regImage.initDone = TRUE;

    if (hmc7043AppInitAppSup(dev, pParams) != OK)
        return ERROR;

    return OK;
}




/*******************************************************************************
* - name: hmc7043AppInitDev
*
* - title: initialize a specific CLKDST device
*
* - input: dev      - CLKDST device for which to perform the operation
*          pParams  - device setup parameters
*          warmInit - if set, will skip actual device initialization
*
* - output: hmc7043AppCtl.devCtl[dev] (indirectly), hmc7043AppState.devState[dev]
*
* - returns: OK or ERROR if detected an error
*
* - description: sets up and initializes the PLL device per the application's
*                requirements
*
* - notes: 1) This routine can be called more than once (for a device).
*          2) The operation is interlocked via the associated critical section.
*******************************************************************************/
STATUS hmc7043AppInitDev(CKDST_DEV dev, const Hmc7043_app_dev_params *pParams,
		                       Bool warmInit)
{
    STATUS status = OK;  /* initial assumption */

    if (!inEnumRange(dev, NELEMENTS(hmc7043AppCtl.devCtl)) || !pParams) {
        sysLog("bad argument(s) (dev %d, pParams %d)", dev, pParams != NULL);
        return ERROR;
    }

    if (!hmc7043IfCtl.initDone || !hmc7043AppCtl.initDone) {
        sysLog("interface not initialized yet (dev %d, init. done %d,%d)", dev,
               hmc7043IfCtl.initDone, hmc7043AppCtl.initDone);
        return ERROR;
    }

    hmc7043CsEnter(dev, __FUNCTION__);

    /* in particular this sets .nsecCmdAt, .freq to zeros */
    memset(&hmc7043AppState.devState[dev], 0,
           sizeof(hmc7043AppState.devState[dev]));

    if (hmc7043AppSetUpDevCtl(dev, pParams) != OK)
        status = ERROR;

    if (!warmInit) {
        if (hmc7043AppInitDevAct(dev, pParams) != OK)
            status = ERROR;
    } else {
        if (hmc7043AppInitRdRegs(dev) != OK)
            status = ERROR;
    }

    hmc7043CsExit(dev, __FUNCTION__);

    return status;
}




/*#############################################################################*
*                    A D D I T I O N A L    S E R V I C E S                    *
*#############################################################################*/
/*******************************************************************************
* - name: hmc7043OutChEnDis
*
* - title: Enable/Disable a particular output channel for a particular device.
*
* - input: dev      - CLKDST device on which operation is performed.
*          iCh      - channel to be enabled or disabled.
*          enable   - True value enables channel, false value disables channel
*
* - output: hmc7043AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - notes: the operation is interlocked via the associated critical section
*******************************************************************************/
EXPORT STATUS hmc7043OutChEnDis(CKDST_DEV dev, unsigned iCh, Bool enable)
{
	const Hmc7043_app_dev_ctl *pCtl;
	Hmc7043_reg_image *pImg;
	STATUS status;

	if (!inEnumRange(dev, NELEMENTS(hmc7043AppCtl.devCtl)) ||
			iCh < HMC7043_CH_OUT_MIN || iCh > HMC7043_CH_OUT_MAX) {
		sysLog("bad argument(s) (dev %d), iCh %d", dev, iCh);
		return ERROR;
	}

	pCtl = hmc7043AppCtl.devCtl + dev;
	pImg = &hmc7043AppState.devState[dev].regImage;

	if(!hmc7043IfCtl.initDone || !hmc7043AppCtl.initDone || !pCtl->initDone) {
		sysLog("initialization not done yet (dev %d, init. done %d,%d,%d)",
		        dev, hmc7043IfCtl.initDone, hmc7043AppCtl.initDone,
				pCtl->initDone);
		return ERROR;
	}

	hmc7043CsEnter(dev, __FUNCTION__);
    switch(iCh) {
		case 0: {
			pImg->rc8.fields.chEn_0 = enable ? 1 : 0;
			status = hmc7043LliRegWrite(dev, 0xca, pImg->rca.all);
			break;
		}
		case 1: {
			pImg->rd2.fields.chEn_1 = enable ? 1 : 0;
			status = hmc7043LliRegWrite(dev, 0xd2, pImg->rd2.all);
			break;
		}
		case 2: {
			pImg->rdc.fields.chEn_2 = enable ? 1 : 0;
			status = hmc7043LliRegWrite(dev, 0xdc, pImg->rdc.all);
			break;
		}
		case 3: {
			pImg->re6.fields.chEn_3 = enable ? 1 : 0;
			status = hmc7043LliRegWrite(dev, 0xe6, pImg->re6.all);
			break;
		}
		case 4: {
			pImg->rf0.fields.chEn_4 = enable ? 1 : 0;
			status = hmc7043LliRegWrite(dev, 0xf0, pImg->rf0.all);
			break;
		}
		case 5: {
			pImg->rfa.fields.chEn_5 = enable ? 1 : 0;
			status = hmc7043LliRegWrite(dev, 0xfa, pImg->rfa.all);
			break;
		}
		case 6: {
			pImg->r104.fields.chEn_6 = enable ? 1 : 0;
			status = hmc7043LliRegWrite(dev, 0x104, pImg->r104.all);
			break;
		}
		case 7: {
			pImg->r10e.fields.chEn_7 = enable ? 1 : 0;
			status = hmc7043LliRegWrite(dev, 0x10e, pImg->r10e.all);
			break;
		}
		case 8: {
			pImg->r118.fields.chEn_8 = enable ? 1 : 0;
			status = hmc7043LliRegWrite(dev, 0x118, pImg->r118.all);
			break;
		}
		case 9: {
			pImg->r122.fields.chEn_9 = enable ? 1 : 0;
			status = hmc7043LliRegWrite(dev, 0x122, pImg->r122.all);
			break;
		}
		case 10: {
			pImg->r12c.fields.chEn_10 = enable ? 1 : 0;
			status = hmc7043LliRegWrite(dev, 0x12c, pImg->r12c.all);
			break;
		}
		case 11: {
			pImg->r136.fields.chEn_11 = enable ? 1 : 0;
			status = hmc7043LliRegWrite(dev, 0x136, pImg->r136.all);
			break;
		}
		case 12: {
			pImg->r140.fields.chEn_12 = enable ? 1 : 0;
			status = hmc7043LliRegWrite(dev, 0x140, pImg->r140.all);
			break;
		}
		case 13: {
			pImg->r14a.fields.chEn_13 = enable ? 1 : 0;
			status = hmc7043LliRegWrite(dev, 0x14a, pImg->r14a.all);
			break;
		}
    }
    hmc7043CsExit(dev, __FUNCTION__);

	if (status != OK)
		return ERROR;

    return OK;
}




/*******************************************************************************
* - name: hmc7043GetAlarms
*
* - title: Retrieve all alarm status from alarm read back register 0x7d
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
EXPORT STATUS hmc7043GetAlarms(CKDST_DEV dev, Hmc7043_dev_alarms *pAlarms)
{
	const Hmc7043_app_dev_ctl *pCtl;
	Hmc7043_reg_x007d r7d;


	if (!inEnumRange(dev, NELEMENTS(hmc7043AppCtl.devCtl)) ||
			!pAlarms) {
		sysLog("bad argument(s) (dev %d), pAlarms %d", dev, (pAlarms != NULL));
		return ERROR;
	}

	pCtl = hmc7043AppCtl.devCtl + dev;

	if(!hmc7043IfCtl.initDone || !hmc7043AppCtl.initDone || !pCtl->initDone) {
		sysLog("initialization not done yet (dev %d, init. done %d,%d,%d)",
				dev, hmc7043IfCtl.initDone, hmc7043AppCtl.initDone,
				pCtl->initDone);
		return ERROR;
	}

	if (hmc7043LliRegRead(dev, 0x7d, &r7d.all) != OK)
	    return ERROR;

	if(r7d.fields.srSynSt)
	    pAlarms->srefSync = TRUE;
	else
		pAlarms->srefSync = FALSE;

	if(r7d.fields.ckOutPhSt)
		pAlarms->cksPhase = TRUE;
	else
		pAlarms->cksPhase = FALSE;

	if(r7d.fields.synReqSt)
	    pAlarms->syncReq = TRUE;
	else
		pAlarms->syncReq = FALSE;

    return OK;
}




/*******************************************************************************
* - name: hmc7043GetAlarm
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
*******************************************************************************/
EXPORT STATUS hmc7043GetAlarm(CKDST_DEV dev, Bool *pAlarm)
{
	const Hmc7043_app_dev_ctl *pCtl;
	Hmc7043_reg_x007b r7b;


	if (!inEnumRange(dev, NELEMENTS(hmc7043AppCtl.devCtl)) ||
			!pAlarm) {
		sysLog("bad argument(s) (dev %d), pAlarm %d", dev, (pAlarm != NULL));
		return ERROR;
	}

	pCtl = hmc7043AppCtl.devCtl + dev;

	if(!hmc7043IfCtl.initDone || !hmc7043AppCtl.initDone || !pCtl->initDone) {
		sysLog("initialization not done yet (dev %d, init. done %d,%d,%d)",
				dev, hmc7043IfCtl.initDone, hmc7043AppCtl.initDone,
				pCtl->initDone);
		return ERROR;
	}

	if (hmc7043LliRegRead(dev, 0x7b, &r7b.all) != OK)
		return ERROR;

	if(r7b.fields.almSig)
	    *pAlarm = TRUE;
	else
		*pAlarm = FALSE;

    return OK;
}




/*******************************************************************************
* - name: hmc7043ClearAlarms
*
* - title: Clear alarms for a particular device.
*
* - input: dev   - CLKDST device on which operation is performed.
*
* - output: hmc7043AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - notes: the operation is interlocked via the associated critical section
*******************************************************************************/
EXPORT STATUS hmc7043ClearAlarms(CKDST_DEV dev)
{
	const Hmc7043_app_dev_ctl *pCtl;
	Hmc7043_reg_image *pImg;
	STATUS status = OK;

	if (!inEnumRange(dev, NELEMENTS(hmc7043AppCtl.devCtl))) {
		sysLog("bad argument(s) (dev %d)", dev);
		return ERROR;
	}

	pCtl = hmc7043AppCtl.devCtl + dev;
	pImg = &hmc7043AppState.devState[dev].regImage;

	if(!hmc7043IfCtl.initDone || !hmc7043AppCtl.initDone || !pCtl->initDone) {
		sysLog("initialization not done yet (dev %d, init. done %d,%d,%d)",
				dev, hmc7043IfCtl.initDone, hmc7043AppCtl.initDone,
				pCtl->initDone);
		return ERROR;
	}

	hmc7043CsEnter(dev, __FUNCTION__);

	pImg->r06.fields.clrAlarms = 1;
	status = hmc7043LliRegWrite(dev, 0x06, pImg->r06.all);

	hmc7043CsExit(dev, __FUNCTION__);

    return status;
}




/*******************************************************************************
* - name: hmc7043SetSysrefMode
*
* - title: Set SYSREF pulse generation mode for a particular device.
*
* - input: dev     - CLKDST device on which operation is performed.
*          mode    - Type of pulse generator mode
*          nPulses - Number of pulses to be generated in case of pulsed mode
*
* - output: hmc7043AppState.devState[dev].regImage
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - notes: the operation is interlocked via the associated critical section
*******************************************************************************/
EXPORT STATUS hmc7043SetSysrefMode(CKDST_DEV dev, HMC7043_SREF_MODE mode,
                            HMC7043_SREF_NPULSES nPulses)
{
	const Hmc7043_app_dev_ctl *pCtl;
	Hmc7043_reg_image *pImg;
	STATUS status = OK;

	if (!inEnumRange(dev, NELEMENTS(hmc7043AppCtl.devCtl))) {
		sysLog("bad argument(s) (dev %d)", dev);
		return ERROR;
	}

	pCtl = hmc7043AppCtl.devCtl + dev;
	pImg = &hmc7043AppState.devState[dev].regImage;

	if(!hmc7043IfCtl.initDone || !hmc7043AppCtl.initDone || !pCtl->initDone) {
		sysLog("initialization not done yet (dev %d, init. done %d,%d,%d)",
				dev, hmc7043IfCtl.initDone, hmc7043AppCtl.initDone,
				pCtl->initDone);
		return ERROR;
	}

	hmc7043CsEnter(dev, __FUNCTION__);
	/* Set pulse generation mode */
	switch(mode)
	{
		case HMC7043_SRM_CONTINUOUS: {
			pImg->r5a.fields.pulseMode = 0x7;
			break;
		}
		case HMC7043_SRM_LEVEL_CTL: {
			pImg->r5a.fields.pulseMode = 0x0;
			break;
		}
		case HMC7043_SRM_PULSED: {
			switch(nPulses)
			{
				case HMC7043_SRNP_1: {
					pImg->r5a.fields.pulseMode = 0x1;
					break;
				}
				case HMC7043_SRNP_2: {
					pImg->r5a.fields.pulseMode = 0x2;
					break;
				}
				case HMC7043_SRNP_4: {
					pImg->r5a.fields.pulseMode = 0x3;
					break;
				}
				case HMC7043_SRNP_8: {
					pImg->r5a.fields.pulseMode = 0x4;
					break;
				}
				case HMC7043_SRNP_16: {
					pImg->r5a.fields.pulseMode = 0x5;
					break;
				}
				default: {
					sysLog("Bad value ( pParams->sysref.nPulses %d)",nPulses);
					return ERROR;
				}
			}
			break;
		}
		default:
			sysLog("Bad value ( pParams->sysref.mode %d)", mode);
			return ERROR;
	}
	status = hmc7043LliRegWrite(dev, 0x5a, pImg->r5a.all);

	hmc7043CsExit(dev, __FUNCTION__);

    return status;
}




/*******************************************************************************
* - name: hmc7043ChDoSlip
*
* - title: Generate slip event for a particular device.
*
* - input: dev     - CLKDST device on which operation is performed.
*          chMask  - channel mask
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - notes: the operation is interlocked via the associated critical section
*******************************************************************************/
EXPORT STATUS hmc7043ChDoSlip(CKDST_DEV dev, HMC7043_CH_MASK chMask)
{
	const Hmc7043_app_dev_ctl *pCtl;
	STATUS status = OK;

	if (!inEnumRange(dev, NELEMENTS(hmc7043AppCtl.devCtl))) {
		sysLog("bad argument(s) (dev %d)", dev);
		return ERROR;
	}

	pCtl = hmc7043AppCtl.devCtl + dev;

	if (!chMask || chMask >= 1 << NELEMENTS(pCtl->params.chSup)) {
		sysLog("bad argument (chMask 0x%x)", chMask);
		return ERROR;
	}

	if(!hmc7043IfCtl.initDone || !hmc7043AppCtl.initDone || !pCtl->initDone) {
		sysLog("initialization not done yet (dev %d, init. done %d,%d,%d)",
				dev, hmc7043IfCtl.initDone, hmc7043AppCtl.initDone,
				pCtl->initDone);
		return ERROR;
	}

	hmc7043CsEnter(dev, __FUNCTION__);

	status = hmc7043ToggleBit(dev, HMC7043_REG_IDX_SLIP_REQ,
			                  HMC7043_SLIP_REQ_BIT, 200);

	hmc7043CsExit(dev, __FUNCTION__);

	return status;

}




/*******************************************************************************
* - name: hmc7043SysrefSwPulseN
*
* - title: Generate N Pulses on SYSREF channels of a particular device.
*
* - input: dev     - CLKDST device on which operation is performed.
*          chMask  - channel mask
*          nPulses -
*
* - returns: OK or ERROR if detected an error
*
* - description: as above
*
* - notes: the operation is interlocked via the associated critical section
*******************************************************************************/
EXPORT STATUS hmc7043SysrefSwPulseN(CKDST_DEV dev, HMC7043_CH_MASK chMask,
                              HMC7043_SREF_NPULSES nPulses)
{
	const Hmc7043_app_dev_ctl *pCtl;
	Hmc7043_reg_x005a r5a;
	STATUS status = OK;

	if (!inEnumRange(dev, NELEMENTS(hmc7043AppCtl.devCtl))) {
		sysLog("bad argument(s) (dev %d)", dev);
		return ERROR;
	}

	pCtl = hmc7043AppCtl.devCtl + dev;

	if (!chMask || chMask >= 1 << NELEMENTS(pCtl->params.chSup)) {
		sysLog("bad argument (chMask 0x%x)", chMask);
		return ERROR;
	}

	if(!hmc7043IfCtl.initDone || !hmc7043AppCtl.initDone || !pCtl->initDone) {
		sysLog("initialization not done yet (dev %d, init. done %d,%d,%d)",
				dev, hmc7043IfCtl.initDone, hmc7043AppCtl.initDone,
				pCtl->initDone);
		return ERROR;
	}

	if (hmc7043LliRegRead(dev, 0x5a, &r5a.all) != OK)
		return ERROR;

	if(r5a.fields.pulseMode == 0x0 || r5a.fields.pulseMode == 0x7) {
		sysLog("Pulse mode is not pulsed (Pulse mode 0x%x)",
				r5a.fields.pulseMode);
		return ERROR;
	}

	hmc7043CsEnter(dev, __FUNCTION__);
	switch(nPulses) {
		case HMC7043_SRNP_1: {
			r5a.fields.pulseMode = 0x1;
			break;
		}
		case HMC7043_SRNP_2: {
			r5a.fields.pulseMode = 0x2;
			break;
		}
		case HMC7043_SRNP_4: {
			r5a.fields.pulseMode = 0x3;
			break;
		}
		case HMC7043_SRNP_8: {
			r5a.fields.pulseMode = 0x4;
			break;
		}
		case HMC7043_SRNP_16: {
			r5a.fields.pulseMode = 0x5;
			break;
		}
	}

	status = hmc7043LliRegWrite(dev, 0x5a, r5a.all);
	if(status != OK) {
		hmc7043CsExit(dev, __FUNCTION__);
		return ERROR;
	}

	status = hmc7043ToggleBit(dev, HMC7043_REG_IDX_REQ_MOD, HMC7043_PULS_GEN_BIT,
						 200);

	hmc7043CsExit(dev, __FUNCTION__);

	return status;

}




int main()
{
   return 0;
}

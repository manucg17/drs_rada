/*******************************************************************************
* hmc7043.h - definitions related to interfacing to HMC7043 clock buffer devs  *
********************************************************************************
* modification history:                                                        *
*   31.01.24 bf, created                                                       *
*******************************************************************************/

#ifndef _hmc7043_h_
#define _hmc7043_h_

#include "sysbase.h"
#include "ckdstif.h"


typedef UINT8 HMC7043_REG;

typedef UINT32 HMC7043_PRD_ID;

typedef STATUS HMC7043_REG_READ(CKDST_DEV dev, unsigned regInx,
                                HMC7043_REG *pData),
               HMC7043_REG_WRITE(CKDST_DEV dev, unsigned regInx,
                                 HMC7043_REG regData);

typedef struct {
    HMC7043_REG_READ *pRegRead;
    HMC7043_REG_WRITE *pRegWrite;
} Hmc7043_dev_io_if;

typedef enum {HMC7043_CID_1, HMC7043_CID_2} HMC7043_DEV_CLKIN_DIV;

typedef struct {
    Bool used;  /* if FALSE, the rest of the data is don't care */
    Bool term100Ohm, acCoupled, lvpecl, highZ;
} Hmc7043_dev_in_sup;

typedef enum {
    HMC7043_GPIS_NONE,      HMC7043_GPIS_SLEEP,  HMC7043_GPIS_MUTE,
    HMC7043_GPIS_PULSE_GEN, HMC7043_GPIS_RESEED, HMC7043_GPIS_RESTART,
    HMC7043_GPIS_SLIP
} HMC7043_DEV_GPI_SUP;

typedef enum {
    HNC7043_GPOS_NONE,         HMC7043_GPOS_ALARM,        HMC7043_GPOS_SDATA,
    HMC7043_GPOS_SREF_NSYNC,   HMC7043_GPOS_CKOUTS_PHASE, HMC7043_GPOS_SYNC_REQ_ST,
    HMC7043_GPOS_CH_FSM_BUSY,  HMC7043_GPOS_SREF_FSM_ST0, HMC7043_GPOS_SREF_FSM_ST1,
    HMC7043_GPOS_SREF_FSM_ST2, HMC7043_GPOS_SREF_FSM_ST3, HMC7043_GPOS_FORCE_1,
    HMC7043_GPOS_FORCE_0,      HMC7043_GPOS_PLS_GEN_REQ
} HMC7043_DEV_GPO_SUP;

typedef enum {HMC7043_OM_OD, HMC7032_OM_CMOS} HMC7043_DEV_OUTPUT_MODE;

typedef enum {
    HMC7043_SRM_CONTINUOUS, HMC7043_SRM_LEVEL_CTL, HMC7043_SRM_PULSED
} HMC7043_SREF_MODE;

typedef enum {  /* SYSREF nPulses setting (for _HMC7043_SRM_PULSED) */
    HMC7043_SRNP_1, HMC7043_SRNP_2, HMC7043_SRNP_4, HMC7043_SRNP_8, HMC7043_SRNP_16
} HMC7043_SREF_NPULSES;

typedef struct {
    Bool syncReq, cksPhase, srefSync;
} Hmc7043_dev_alarms;

typedef enum {
    HMC7043_CHM_UNUSED, HMC7043_CHM_CLK, HMC7043_CHM_SYSREF
} HMC7043_CH_MODE;

typedef enum {
    HMC7043_CDM_CML, HMC7043_CDM_LVPECL, HMC7043_CDM_LVDS, HMC7043_CDM_CMOS
} HMC7043_CH_DRV_MODE;

typedef enum {
    HMC7043_CCIT_NONE, HMC7043_CCIT_100, HMC7043_CCIT_50
} HMC7043_CH_CML_INT_TERM;

typedef enum {
    HMC7043_CI0_NORMAL, HMC7043_CI0_FORCE_0, HMC7043_CI0_FLOAT
} HMC7043_CH_IDLE0;

typedef enum {
    HMC7043_COS_FUNDAMENTAL, HMC7043_COS_DIVIDER, HMC7043_COS_DIV_ADLY,
    HMC7043_COS_DIV_NEIGHBOR
} HMC7043_CH_OUT_SEL;

typedef struct {
	HMC7043_CH_MODE chMode;  /* if _UNUSED, the rest of the data is don't care */
    CKDST_FREQ_HZ freq;
    HMC7043_CH_DRV_MODE drvMode;
    HMC7043_CH_CML_INT_TERM cmlTerm;  /* only applicable for _CML drvMode */
    HMC7043_CH_IDLE0 idle0;
    HMC7043_CH_OUT_SEL outSel;
    double dDlyPs, aDlyPs, slipQuantumPs;
    Bool highPerfMode;
    Bool dynDriverEn;  /* only applicable to a SYSREF output channel */
} Hmc7043_ch_sup;

#define HMC7043_OUT_NCHAN  14

typedef UINT32 HMC7043_CH_MASK;

typedef struct {
	CKDST_FREQ_HZ clkInFreq;
    HMC7043_DEV_CLKIN_DIV clkInDiv;
    Hmc7043_dev_in_sup clkIn, syncIn;
    HMC7043_DEV_GPI_SUP gpiSup;
    HMC7043_DEV_GPO_SUP gpoSup;
    HMC7043_DEV_OUTPUT_MODE gpoMode, sdataMode;
    struct {
        CKDST_FREQ_HZ freq;
        HMC7043_SREF_MODE mode;        /* (initial) SYSREF generation mode */
        Bool invertedSync;
        Bool syncRetime;               /* whether this is needed is TBR */
        HMC7043_SREF_NPULSES nPulses;  /* for HMC7043_SRM_PULSED mode */
    } sysref;
    Hmc7043_dev_alarms alarmsEn;
    Hmc7043_ch_sup chSup[HMC7043_OUT_NCHAN];
} Hmc7043_app_dev_params;

/* services */
STATUS hmc7043IfInit(CKDST_DEV_MASK devMask);
STATUS hmc7043InitDev(CKDST_DEV dev, const Hmc7043_dev_io_if *pIf,
                      const Hmc7043_app_dev_params *pParams, Bool warmInit);

STATUS hmc7043OutChEnDis(CKDST_DEV dev, unsigned iCh, Bool enable);

STATUS hmc7043ChDoSlip(CKDST_DEV dev, HMC7043_CH_MASK chMask);

/* nPulses argument here is only relevant for HMC7043_SRM_PULSED mode */
STATUS hmc7043SetSysrefMode(CKDST_DEV dev, HMC7043_SREF_MODE mode,
                            HMC7043_SREF_NPULSES nPulses);

STATUS hmc7043SysrefSwPulseN(CKDST_DEV dev, HMC7043_CH_MASK chMask,
                              HMC7043_SREF_NPULSES nPulses);

STATUS hmc7043GetAlarm(CKDST_DEV dev, Bool *pAlarm);
STATUS hmc7043GetAlarms(CKDST_DEV dev, Hmc7043_dev_alarms *pAlarms);
STATUS hmc7043ClearAlarms(CKDST_DEV dev);

/* these routines are provided for low-level debugging */
STATUS hmc7043RegRead(CKDST_DEV dev, unsigned regInx, HMC7043_REG *pData),
       hmc7043RegWrite(CKDST_DEV dev, unsigned regInx, HMC7043_REG regData);


#endif /* _hmc7043_h_ */

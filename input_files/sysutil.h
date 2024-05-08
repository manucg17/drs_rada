/*******************************************************************************
* sysutil.h - miscellaneous system-level utilities for Linux                   *
********************************************************************************
* modification history:                                                        *
*   26.03.20 bf, ported from radar                                             *
*******************************************************************************/

#ifndef _sysutil_h_
#define _sysutil_h_

#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <sys/types.h>
#include <dirent.h>
#include <pthread.h>
#include "sysbase.h"


/* kernel clock interface */

/* note: SYS_TIME measures time since startup in milliseconds */
typedef UINT64 SYS_TIME, SYS_TIME_NS, SYS_TIME_EXT_TICKS;
#define SYS_TIME_MAX      UINT64_MAX
#define SYS_TIME_NONE     0
#define SYS_TIME_NO_WAIT  0
#define SYS_TIME_INFINITE SYS_TIME_MAX

typedef SYS_TIME_EXT_TICKS SYS_TIME_EXT_SRC(void);
STATUS sysTimeSetExtSource(SYS_TIME_EXT_SRC *pFunc, double tickNsec);

SYS_TIME sysTime(void);
SYS_TIME_NS sysTimeNsec(void);
void sysDelay(SYS_TIME delay), sysSafeDelay(SYS_TIME delay);
void sysDelayUsec(UINT64 delayUsec);
void sysDelayUsecBusy(unsigned delayUsec);  /* implements busy wait */
STATUS sysCalcAbsTimeout(SYS_TIME timeout, struct timespec *pTmout);
Bool utlIsTmOut(SYS_TIME timeTag, SYS_TIME timeout),
     utlIsTmDue(SYS_TIME timeTag, SYS_TIME period);

/* code error handling */
typedef enum {
    SYS_CODE_ERR_HDL_IGNORE, SYS_CODE_ERR_HDL_SUSPEND, SYS_CODE_ERR_HDL_RESET
} SYS_CODE_ERR_HANDLING;

typedef enum {
    CODE_ERR_NONE, CODE_ERR_PARAMETER, CODE_ERR_STATE, CODE_ERR_OS,
    CODE_ERR_ADDR_FAULT, CODE_ERR_THR_EXC, CODE_ERR_NENTRIES
} CODE_ERROR_ID;

typedef void (*FUNCPTR)(void);

void sysCodeErr(CODE_ERROR_ID errorId, FUNCPTR pFunc, const char *funcName,
                UINT64 auxData1, UINT64 auxData2, UINT64 auxData3);

#define sysCodeError(errorId, func, auxData1, auxData2, auxData3)   \
    sysCodeErr(errorId, (FUNCPTR) func, #func, (UINT64) (auxData1), \
               (UINT64) (auxData2), (UINT64) (auxData3))

typedef void SYS_CODE_ERR_APP_HOOK(CODE_ERROR_ID errorId, FUNCPTR pFunc,
                                   const char *funcName, UINT64 auxData1,
                                   UINT64 auxData2, UINT64 auxData3);
void sysCodeErrSetAppHook(SYS_CODE_ERR_APP_HOOK *pHook);

/* networking-related definitions */
#ifndef SOCKET
# define SOCKET int
#endif

#ifndef INVALID_SOCKET
# define INVALID_SOCKET  ((SOCKET) (-1))
#endif

typedef UINT32 IP_ADDR_V4;

#ifndef INVALID_IP_ADDR
# define INVALID_IP_ADDR ((IP_ADDR_V4) 0)
#endif

/* thread management */
typedef unsigned SYS_THREAD_PRI;       /* Linux real-time priority scale */
#define SYS_THREAD_PRI_LOWEST     1    /* per Linux definitions */
#define SYS_THREAD_PRI_HIGHEST    99   /* ditto */

#define SYS_THREAD_PRI_1ST_ABOVE_2ND(_1stPri, _2ndPri)  ((_1stPri) > (_2ndPri))

#define SYS_THREAD_NAME_MAX       15   /* per Linux definitions */

#define SYS_THREAD_MIN_STACK_SIZE PTHREAD_STACK_MIN  /* 128 KB on AArch64 GCC (!) */

typedef struct {       /* parameters for a particular thread code */
    const char *name;  /* up to SYS_THREAD_NAME_MAX chars of name will be used */
    SYS_THREAD_PRI initialPri;
    Bool multiple;     /* whether multiple threads with this code can be created */
} Sys_thread_ctl_params;

typedef UINT32 SYS_THREAD_OPTS;
#define SYS_THREAD_OPTS_NORMAL    0
#define SYS_THREAD_OPTS_WAITABLE  0x1  /* to support waiting for its termination */
#define SYS_THREAD_OPTS_SUSPENDED 0x2  /* thread is initially suspended */
#define SYS_THREAD_OPTS_ANY       0x3  /* per the above */

typedef struct {
    UINT64 arg1, arg2, arg3;
} Sys_thread_args;

typedef UINT64 SYS_THREAD_FUNC(const Sys_thread_args *pArgs);

typedef pthread_t HSYS_THREAD;  /* Linux thread handle */

typedef struct {  /* parameters for a periodic service thread */
    FUNCPTR pIterProcess;
    UINT32 stackSize;
    SYS_TIME period;
    Bool ignoreOverrun;
} Sys_thread_per_serv_args;

STATUS sysThreadCreate(unsigned thrCode, unsigned subCode, SYS_THREAD_FUNC *pEntry,
                       UINT32 stackSize, const Sys_thread_args *pArgs),
       sysThreadCreateEx(unsigned thrCode, unsigned subCode,
                         SYS_THREAD_FUNC *pEntry, UINT32 stackSize,
                         const Sys_thread_args *pArgs, SYS_THREAD_OPTS opts),
       sysThreadCreatePerServ(unsigned thrCode, unsigned subCode,
                              const Sys_thread_per_serv_args *pArgs);

STATUS sysThreadStart(unsigned thrCode, unsigned subCode);
void sysThreadExit(UINT64 exitCode);
STATUS sysThreadWait4Exit(unsigned thrCode, unsigned subCode, SYS_TIME timeout,
                          UINT64 *pExitCode);
STATUS sysThreadCodeSelf(unsigned *pCode, unsigned *pSubCode);
STATUS sysThreadCode2Handle(unsigned thrCode, unsigned subCode,
                            HSYS_THREAD *phThread);
STATUS sysThreadGetPriority(HSYS_THREAD hThread, SYS_THREAD_PRI *pPri),
       sysThreadSetPriority(HSYS_THREAD hThread, SYS_THREAD_PRI pri);
STATUS sysThreadDeleteAll(void);

/* debug message logging */
typedef enum {
    SYS_LOG_LEVEL_UNCOND = 0,   /* unconditional output */
    SYS_LOG_LEVEL_ERROR  = 1, SYS_LOG_LEVEL_WARNING = 2, SYS_LOG_LEVEL_INFO = 3,
    SYS_LOG_LEVEL_LOWEST = 100  /* value is arbitrary */
} SYS_LOG_LEVEL;

typedef struct {
    unsigned thrCode;           /* thread code of the message logging thread */
    SYS_THREAD_PRI initialPri;  /* used until application calls sysLogSetFinalPri */
    Bool forceOutput;           /* whether to force output when queue is full */
    unsigned maxMsgLen;         /* maximum supported message length */
    unsigned maxNmsgs;          /* message log queue depth */
    struct {
        const char *name;       /* NULL if not logging to a file */
        SYS_TIME segDuration;   /* segment duration or SYS_TIME_NONE if N/A */
        Bool enable;            /* if FALSE, will initially not log to a file */
    } logFile;
    unsigned loggingLevel;      /* initial logging level (ref. SYS_LOG_LEVEL) */
} Sys_log_params;

typedef STATUS SYS_LOG_EXT_HANDLER(const char *msg);

STATUS sysLogSetFinalPri(void);
STATUS sysLogReinit(void);
STATUS sysLogEnDis(Bool enable);
STATUS sysLog2FileEnDis(Bool enable);
STATUS sysLogTee2Sock(IP_ADDR_V4 ipAddr, UINT16 udpPort);
STATUS sysLogTee2Ext(SYS_LOG_EXT_HANDLER *pExt);
STATUS sysLogSetTimeCorr(INT32 correction);
const char *sysLogLast(void);

/* lower (numerically higher) level values yield more output to console */
STATUS sysLogSetLevel(SYS_LOG_LEVEL level);

/* sysLogIntFun / sysLogLongFun / sysLogFpaFun accept up to SYS_LOG_MAX_NARGS
   int-sized / long-sized (integer) / floating point arguments, respectively.
   When context is null these functions do not output line header and \n.
   Note: pointers and size_t are 64-bit (i.e. long) on this architecture.
   Note: string arguments can only be used with sysLogLongFun. */
#define SYS_LOG_MAX_NARGS  6

STATUS sysLogIntFun (unsigned level, const char *context, const char *format, ...)
       __attribute__((format (printf, 3, 4)));
STATUS sysLogLongFun(unsigned level, const char *context, const char *format, ...)
       __attribute__((format (printf, 3, 4)));
STATUS sysLogFpaFun (unsigned level, const char *context, const char *format, ...)
       __attribute__((format (printf, 3, 4)));

/* sysLogForceFun accepts any kind and number of arguments and is independent of
   logging level. When context is null it does not output line header and \n. */
void sysLogForceFun (                const char *context, const char *format, ...)
     __attribute__((format (printf, 2, 3)));

/* ##__VA_ARGS__ is a GNU compiler-specific extension (it properly handles the case of no variable args) */
#define sysLog(               f, ...)  sysLogIntFun (SYS_LOG_LEVEL_UNCOND,  __FUNCTION__, (f), ##__VA_ARGS__)
#define sysLogLong(           f, ...)  sysLogLongFun(SYS_LOG_LEVEL_UNCOND,  __FUNCTION__, (f), ##__VA_ARGS__)
#define sysLogFpa(            f, ...)  sysLogFpaFun (SYS_LOG_LEVEL_UNCOND,  __FUNCTION__, (f), ##__VA_ARGS__)
#define sysLogRaw(            f, ...)  sysLogIntFun (SYS_LOG_LEVEL_UNCOND,  NULL,         (f), ##__VA_ARGS__)
#define sysLogLongRaw(        f, ...)  sysLogLongFun(SYS_LOG_LEVEL_UNCOND,  NULL,         (f), ##__VA_ARGS__)
#define sysLogFpaRaw(         f, ...)  sysLogFpaFun (SYS_LOG_LEVEL_UNCOND,  NULL,         (f), ##__VA_ARGS__)

#define sysLogErr(            f, ...)  sysLogIntFun (SYS_LOG_LEVEL_ERROR,   __FUNCTION__, (f), ##__VA_ARGS__)
#define sysLogLongErr(        f, ...)  sysLogLongFun(SYS_LOG_LEVEL_ERROR,   __FUNCTION__, (f), ##__VA_ARGS__)
#define sysLogFpaErr(         f, ...)  sysLogFpaFun (SYS_LOG_LEVEL_ERROR,   __FUNCTION__, (f), ##__VA_ARGS__)
#define sysLogRawErr(         f, ...)  sysLogIntFun (SYS_LOG_LEVEL_ERROR,   NULL,         (f), ##__VA_ARGS__)
#define sysLogLongRawErr(     f, ...)  sysLogLongFun(SYS_LOG_LEVEL_ERROR,   NULL,         (f), ##__VA_ARGS__)
#define sysLogFpaRawErr(      f, ...)  sysLogFpaFun (SYS_LOG_LEVEL_ERROR,   NULL,         (f), ##__VA_ARGS__)

#define sysLogWarn(           f, ...)  sysLogIntFun (SYS_LOG_LEVEL_WARNING, __FUNCTION__, (f), ##__VA_ARGS__)
#define sysLogLongWarn(       f, ...)  sysLogLongFun(SYS_LOG_LEVEL_WARNING, __FUNCTION__, (f), ##__VA_ARGS__)
#define sysLogFpaWarn(        f, ...)  sysLogFpaFun (SYS_LOG_LEVEL_WARNING, __FUNCTION__, (f), ##__VA_ARGS__)
#define sysLogRawWarn(        f, ...)  sysLogIntFun (SYS_LOG_LEVEL_WARNING, NULL,         (f), ##__VA_ARGS__)
#define sysLogLongRawWarn(    f, ...)  sysLogLongFun(SYS_LOG_LEVEL_WARNING, NULL,         (f), ##__VA_ARGS__)
#define sysLogFpaRawWarn(     f, ...)  sysLogFpaFun (SYS_LOG_LEVEL_WARNING, NULL,         (f), ##__VA_ARGS__)

#define sysLogInfo(           f, ...)  sysLogIntFun (SYS_LOG_LEVEL_INFO,    __FUNCTION__, (f), ##__VA_ARGS__)
#define sysLogLongInfo(       f, ...)  sysLogLongFun(SYS_LOG_LEVEL_INFO,    __FUNCTION__, (f), ##__VA_ARGS__)
#define sysLogFpaInfo(        f, ...)  sysLogFpaFun (SYS_LOG_LEVEL_INFO,    __FUNCTION__, (f), ##__VA_ARGS__)
#define sysLogRawInfo(        f, ...)  sysLogIntFun (SYS_LOG_LEVEL_INFO,    NULL,         (f), ##__VA_ARGS__)
#define sysLogLongRawInfo(    f, ...)  sysLogLongFun(SYS_LOG_LEVEL_INFO,    NULL,         (f), ##__VA_ARGS__)
#define sysLogFpaRawInfo(     f, ...)  sysLogFpaFun (SYS_LOG_LEVEL_INFO,    NULL,         (f), ##__VA_ARGS__)

#define sysLogLevel(       l, f, ...)  sysLogIntFun ((l),                   __FUNCTION__, (f), ##__VA_ARGS__)
#define sysLogLongLevel(   l, f, ...)  sysLogLongFun((l),                   __FUNCTION__, (f), ##__VA_ARGS__)
#define sysLogFpaLevel(    l, f, ...)  sysLogFpaFun ((l),                   __FUNCTION__, (f), ##__VA_ARGS__)
#define sysLogRawLevel(    l, f, ...)  sysLogIntFun ((l),                   NULL,         (f), ##__VA_ARGS__)
#define sysLogLongRawLevel(l, f, ...)  sysLogLongFun((l),                   NULL,         (f), ##__VA_ARGS__)
#define sysLogFpaRawLevel( l, f, ...)  sysLogFpaFun ((l),                   NULL,         (f), ##__VA_ARGS__)

#define sysLogForce(          f, ...)  sysLogForceFun(                      __FUNCTION__, (f), ##__VA_ARGS__)
#define sysLogForceRaw(       f, ...)  sysLogForceFun(                      NULL,         (f), ##__VA_ARGS__)

/* convey a 64-bit integer value as two 32-bit arguments (high, low) to sysLogIntFun
   (this must obviously correspond to 2 int-sized arguments in the format string) */
#define SL64(x)  (UINT32) ((UINT64) (x) >> 32), (UINT32) ((UINT64) (x))

/* system utilities initialization */
typedef enum {
    SYS_RT_THROTTLE_NO_CHG = 0, SYS_RT_THROTTLE_DEFAULT, SYS_RT_THROTTLE_DISABLE,
    SYS_RT_THROTTLE_ENABLE
} SYS_RT_THROTTLE_CTL;

typedef struct {
    struct {
        SYS_RT_THROTTLE_CTL control;
        /* the following are only relevant when control is SYS_RT_THROTTLE_ENABLE */
        SYS_TIME period;   /* period across which applying maxRtFrac */
        double maxRtFrac;  /* maximum CPU time fraction allocated to RT processes */
    } rtThrottling;
} Sys_sched_params;

typedef enum {
    SYS_SIG_MASK_INT  = 0x1,  /* SIGINT (Ctrl-C) */
    SYS_SIG_MASK_TERM = 0x2,  /* SIGTERM (default kill command) */
    SYS_SIG_MASK_QUIT = 0x4,  /* SIGQUIT (Ctl-\) */
    SYS_SIG_MASK_PWR  = 0x8,  /* SIGPWR */
    SYS_SIG_MASK_ALL  = 0xf   /* must be consistent with the above */
} SYS_SIG_MASK;

typedef struct {
    SYS_SIG_MASK sigBlockMask;
    STATUS (*handler)(SYS_SIG_MASK sysSig);
    unsigned thrCode;  /* thread code of the sig. handling thr. (N/A if !handler) */
} Sys_signal_handling;

typedef struct {
    struct {
        SYS_CODE_ERR_HANDLING codeErrHandling;
    } excHndl;
    Sys_signal_handling signalHndl;
    Sys_sched_params scheduling;
    SYS_TIME cpuLoadMeasIntvl;
    struct {
        const Sys_thread_ctl_params *params;  /* per-thread code parameters */
        unsigned nThrCodes;
    } threadCtl;
    Sys_log_params msgLog;
} Sys_util_params;

STATUS sysUtilInit(const Sys_util_params *pParams);

/* management of auto-delete resources */
typedef STATUS SYS_RESOURCE_DELETE_FUNC(const char *name);

STATUS sysRegisterAutoDelResource(const char *name, Bool prependSlash,
                                  SYS_RESOURCE_DELETE_FUNC *pDelete),
       sysUnregisterAutoDelResource(const char *name);
STATUS sysDeleteAutoDelResources(void);

/* mutex-related services */
typedef struct utl_mutex *HUTL_MUTEX;

#define UTL_MUTEX_BAD_HMUTEX  NULL

HUTL_MUTEX utlMutexCreate(SYS_TIME timeout);
STATUS utlMutexDelete(HUTL_MUTEX hMutex);
STATUS utlMutexTake(HUTL_MUTEX hMutex, const char *context),
       utlMutexTakeNoDelay(HUTL_MUTEX hMutex, const char *context),
       utlMutexRelease(HUTL_MUTEX hMutex, const char *context);

/* simple message queue-related services (these are copying queues) */
typedef struct utl_queue *HUTL_QUEUE;

#define UTL_QUEUE_BAD_HQUEUE NULL

#define UTL_Q_TO_NO_WAIT  SYS_TIME_NONE      /* to effect no-wait pend operations */
#define UTL_Q_TO_INFINITE SYS_TIME_INFINITE  /* as the name implies */

typedef enum {
    UTL_QST_OK,           UTL_QST_EMPTY,        UTL_QST_FULL,
    UTL_QST_BAD_ARGUMENT, UTL_QST_NO_RESOURCES, UTL_QST_SYSTEM_ERROR,
    UTL_QST_INTERNAL_ERROR
} UTL_Q_STAT;

HUTL_QUEUE utlQueueCreate(size_t maxMsgs, size_t maxMsgSize, UTL_Q_STAT *pStat);
STATUS utlQueueDelete(HUTL_QUEUE hQueue, UTL_Q_STAT *pStat),
       utlQueuePut(HUTL_QUEUE hQueue, const void *buff, size_t nBytes,
                   UTL_Q_STAT *pStat),
       utlQueuePutUrgent(HUTL_QUEUE hQueue, const void *buff, size_t nBytes,
                         UTL_Q_STAT *pStat),
       utlQueueGet(HUTL_QUEUE hQueue, void *buff, size_t *pNbytes, SYS_TIME timeout,
                   UTL_Q_STAT *pStat),
       utlQueueCount(HUTL_QUEUE hQueue, size_t *pNinQueue, UTL_Q_STAT *pStat);

/* memory management support */
STATUS sysMemMap4Mmio(UINT64 physAddr, UINT64 nBytes, void **ppMap,
                      UINT64 *pMapSize);
STATUS sysMemUnmap(void *pMap, UINT64 mapSize);

/* generation of system-unique names */
STATUS sysGenUniqueName(char *buff, size_t buffSize, Bool prependSlash);

/* CPU load measurement */
STATUS sysGetCpuLoad(double *pLoadFrac);

/* file path name limit */
/* there is no _MAX_PATH on Linux; FILENAME_MAX and PATH_MAX are dangerous */
#define UTL_MAX_PATH  4096  /* quite arbitrary - use with care! */

/* _findfirst64 / _findnext64 replacement */
#define UTL_FFIND_MAXNAMELEN  256  /* ref. POSIX readdir */

typedef int UTL_FFIND_OPTS;  /* encoding is similar to fnmatch flags argument */

typedef struct utl_ffind_impl Utl_ffind_impl;

typedef struct {
    Utl_ffind_impl *f_impl;
    char f_filename[UTL_FFIND_MAXNAMELEN + 1];
    UINT64 f_size;   /* file size in bytes */
    mode_t f_mode;   /* file mode returned by stat */
    UINT64 f_atime;  /* time last accessed */
    UINT64 f_mtime;  /* time last modified */
    UINT64 f_ctime;  /* time created */
} Utl_ffind;

STATUS utlFfindFirst(const char *filename, UTL_FFIND_OPTS opts, Utl_ffind *ffind);
STATUS utlFfindNext(Utl_ffind *ffind);
STATUS utlFfindClose(Utl_ffind *ffind);

/* target-resident debug server initialization */
STATUS sysDbgSrvInit(unsigned dbgSrvThrCode, UINT16 tgtUdpPort);

/* miscellaneous utilities */
STATUS utlSplitPath(const char *path, char *dir, size_t dirLen, char *fName,
                    size_t fnLen, char *ext, size_t extLen);
STATUS utlCopyFile(const char *inPath, const char *outPath);
unsigned utlBitCount(UINT32 value);
unsigned utlBitCount64(UINT64 value);
UINT64 utlCalcGcd(UINT64 n1, UINT64 n2);
void utlBuffByteRev(void *buff, size_t nBytes);
void utlBuffByteRevCpy(void *dest, const void *src, size_t nBytes);
double log2(double x);
Bool utlReal32IsValid(const REAL32 *pValue);
Bool utlReal64IsValid(const REAL64 *pValue);
UINT32 utlReal32ToUint32(const REAL32 *pValue);
UINT64 utlReal64ToUint64(const REAL64 *pValue);
STATUS utlByteDump(const void *buff, size_t nBytes, unsigned bytesPerLine);
STATUS utlByteDump2Str(char *outStr, size_t outSize, const void *input,
                       size_t nBytes);

/* miscellaneous useful inline functions for floating point values */
INLINE double roundDouble(double val)
{
    return floor(val + 0.5);  /* this works for negative values as well */
}

INLINE double reduceAngle0_2PI(double angle)
{
    static const double _2PI = 2 * PI;

    double rem = (angle >= 0) ? fmod(angle, _2PI) : _2PI - fmod(-angle, _2PI);

    /* take care of possible floating-point inaccuracies */
    if (rem < 0)
        rem = 0;
    else if (rem > _2PI)
        rem = _2PI;

    return rem;
}

INLINE double reduceAngleMinusPI_PI(double angle)
{
    static const double _2PI = 2 * PI;

    /* first reduce angle to the range [0, 2 * PI] */
    double rem = (angle >= 0) ? fmod(angle, _2PI) : _2PI - fmod(-angle, _2PI);

    /* and now reduce it to the range [-PI, PI] */
    if (rem > PI)
        rem -= _2PI;

    /* take care of possible floating-point inaccuracies */
    if (rem < -PI)
        rem = -PI;
    else if (rem > PI)
        rem = PI;

    return rem;
}

/* bit manipulation macros */
#define   setBit64(val, bit)  ((val) |   (UINT64) 1 << (bit))
#define clearBit64(val, bit)  ((val) & ~((UINT64) 1 << (bit)))

/* miscellaneous */
INLINE UINT64 alignUp64(UINT64 value, UINT64 alignment)
{
    UINT64 res;

    if (!alignment)
        return value;

    if ((res = value % alignment) == 0)
        return value;

    return value + alignment - res;
}


#endif /* _sysutil_h_ */


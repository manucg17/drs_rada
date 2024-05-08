/*******************************************************************************
* sysbase.h - generally usable types, macros and inline functions              *
*                                                                              *
* Notes:                                                                       *
* ------                                                                       *
* a) This is targeted for AArch64 code running under Linux in 64-bit mode.     *
* b) Assuming use of GNU C compiler.                                           *
* c) Using Xilinx "bare metal" library atomic operations interface (also used  *
*    for the sake of memory-mapped register I/O).                              *
********************************************************************************
* modification history:                                                        *
*   26.03.20 bf, ported from radar                                             *
*******************************************************************************/

#ifndef _sysbase_h_
#define _sysbase_h_

#include <stdint.h>
#include <float.h>

#ifdef EMBEDDED
# include "metal/atomic.h"
#else
# include <stdatomic.h>
#endif /* EMBEDDED */


/* basic types */
typedef int8_t   INT8;
typedef uint8_t  UINT8;
typedef int16_t  INT16;
typedef uint16_t UINT16;
typedef int32_t  INT32;
typedef uint32_t UINT32;
typedef int64_t  INT64;
typedef uint64_t UINT64;

typedef __int128 INT128;
typedef unsigned __int128 UINT128;

typedef unsigned long ULONG;

typedef uintptr_t UINT4PTR;  /* an integer type that is same size as a pointer */
typedef UINT64    PHYSICAL_ADDRESS;

typedef float  REAL32;       /* assuming IEEE-754 single precision format */
typedef double REAL64;       /* assuming IEEE-754 double precision format */

#define REAL32_MAX      FLT_MAX
#define REAL64_MAX      DBL_MAX
#define REAL32_SMALLEST FLT_MIN
#define REAL64_SMALLEST DBL_MIN

/* boolean types */
typedef int Bool;
typedef UINT8 UINT8_Bool;

#if defined(TRUE) != defined(FALSE)
# error Inconsistent TRUE / FALSE definition.
#endif
#ifndef TRUE
# define TRUE  1
# define FALSE 0
#endif /* TRUE */

/* general status type */
typedef enum {OK = 0, ERROR = -1} STATUS;

/* atomic operations on 32-bit and 64-bit variables */
typedef UINT32 volatile UINT32_ATOMIC;
typedef UINT64 volatile UINT64_ATOMIC;

#define sysAtomicVarInit(value)  ATOMIC_VAR_INIT(value)

#define sysAtomicLoad( pVar)                                         \
    atomic_load_explicit     ((pVar),          memory_order_seq_cst)
#define sysAtomicStore(pVar, value)                                  \
    atomic_store_explicit    ((pVar), (value), memory_order_seq_cst)
#define sysAtomicSet(  pVar, value)                                  \
    atomic_exchange_explicit ((pVar), (value), memory_order_seq_cst)
#define sysAtomicAdd(  pVar, value)                                  \
    atomic_fetch_add_explicit((pVar), (value), memory_order_seq_cst)
#define sysAtomicSub(  pVar, value)                                  \
    atomic_fetch_sub_explicit((pVar), (value), memory_order_seq_cst)
#define sysAtomicAnd(  pVar, mask)                                   \
    atomic_fetch_and_explicit((pVar), (mask),  memory_order_seq_cst)
#define sysAtomicOr(   pVar, mask)                                   \
    atomic_fetch_or_explicit ((pVar), (mask),  memory_order_seq_cst)
#define sysAtomicXor(  pVar, mask)                                   \
    atomic_fetch_xor_explicit((pVar), (mask),  memory_order_seq_cst)

#define sysAtomicGet(pVar)          sysAtomicLoad(pVar)
#define sysAtomicGetAndClear(pVar)  sysAtomicSet((pVar), 0)

/* compiler directives */
#define INLINE    static inline  /* for some reason static is a must here */
#define ALIGN(x)  __attribute__((aligned(x)))

/* linkage control */
#ifdef EXPORT
# undef EXPORT
#endif
#define EXPORT

#ifdef LOCAL
# undef LOCAL
#endif
#define LOCAL static  /* this should only be used for module-level declarations! */

/* useful macros */
#define NELEMENTS(array)            (sizeof(array) / sizeof(array[0]))
#define FOREVER                     for (;;)

#ifndef min
# define min(x, y)                  ((x) < (y) ? (x) : (y))
#endif
#ifndef max
# define max(x, y)                  ((x) > (y) ? (x) : (y))
#endif

#define UNREFERENCED_PARAMETER(x)   ((void) (x))

INLINE Bool inEnumRange(int val, unsigned nEntries)
{
    return (val >= 0 && val < nEntries);
}

/* mathematical and physical constants and conversions */
#ifndef PI
# define PI                         3.1415926535897932385
#endif
#define LN10                        2.3025850929940456840   /* ln(10) */
#define LN2                         0.69314718055994530942  /* ln(2) */
#define SQRT2                       1.4142135623730950488   /* sqrt(2) */
#define SPEED_OF_LIGHT              299792458.
#define KT                          -174                    /* [dBm / Hz] */

#define RAD2DEG(r)                  ((r) * (180 / PI))
#define DEG2RAD(d)                  ((d) * (PI / 180))

/*
* Memory-mapped I/O access primitives - assuming here that barrier instructions are
* unnecessary since the underlying memory addresses are defined as nGnRnE device
* memory (ARMv8-A). This is also the approach taken by the Xilinx "bare metal"
* library (ref. metal_io_read / metal_io_write).
*/
INLINE UINT8 READ_REG8(volatile const UINT8 *address)
{
    return atomic_load_explicit(address, memory_order_seq_cst);
}

INLINE UINT16 READ_REG16(volatile const UINT16 *address)
{
    return atomic_load_explicit(address, memory_order_seq_cst);
}

INLINE UINT32 READ_REG32(volatile const UINT32 *address)
{
    return atomic_load_explicit(address, memory_order_seq_cst);
}

INLINE UINT64 READ_REG64(volatile const UINT64 *address)
{
    return atomic_load_explicit(address, memory_order_seq_cst);
}

INLINE void WRITE_REG8(volatile UINT8 *address, UINT8 value)
{
    atomic_store_explicit(address, value, memory_order_seq_cst);
}

INLINE void WRITE_REG16(volatile UINT16 *address, UINT16 value)
{
    atomic_store_explicit(address, value, memory_order_seq_cst);
}

INLINE void WRITE_REG32(volatile UINT32 *address, UINT32 value)
{
    atomic_store_explicit(address, value, memory_order_seq_cst);
}

INLINE void WRITE_REG64(volatile UINT64 *address, UINT64 value)
{
    atomic_store_explicit(address, value, memory_order_seq_cst);
}


#endif /* _sysbase_h_ */


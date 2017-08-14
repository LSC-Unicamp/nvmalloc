#ifndef __NVMALLOC_H
#define __NVMALLOC_H

#include <inttypes.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" 
{
#endif

#define handle_error(msg) \
do { perror(msg); exit(EXIT_FAILURE); } while (0)

#define ONE_K 1024ull
#define ONE_M (1024ull * ONE_K)
#define ONE_G (1024ull * ONE_M)

#ifndef MAX //avoid clashes with macros defined by glib at gmacros.h
#define MAX(x, y) ((x > y) ? x : y)
#endif 
#ifndef MIN
#define MIN(x, y) ((x > y) ? y : x)
#endif

#define NEXT_P2(sz)  (1 << (sizeof(size_t) * 8 - __builtin_clz (sz - 1)))

#ifndef PAGE_SIZE
#include <unistd.h>
#define PAGE_SIZE (sysconf(_SC_PAGE_SIZE))
#pragma message "WARNING: PAGE_SIZE undefined. Defaulting to sysconf call. It may degrade performace."
#endif

#define PAGE_MASK (PAGE_SIZE - 1)
#define PAGE_BITS (sizeof(long long) * 8 - __builtin_clzll(PAGE_MASK))
#define NEXT_PAGE_BOUNDARY(p) (((p >> PAGE_BITS) + !!(p & PAGE_MASK)) << PAGE_BITS)
#define IS_NVM_RANGE(addr) ((intptr_t)addr >= NVM_ADDR_MIN && (intptr_t)addr < NVM_ADDR_MAX)

/* 
 * NVM_MIN_CARRIER_SIZE - Must be multiple of PAGE_SIZE. 
 * Defines the smallest size possible for a carrier
 */
#define NVM_MIN_CARRIER_SIZE     ONE_G 

/* 
 * NVM_MAX_CARRIER_COUNT 
 * The total available statically allocated carrier slots
 */
#define NVM_MAX_CARRIER_COUNT    ((size_t)64)

/* 
 * NVM_MIN_SKIP_SIZE - Must be multiple of PAGE_SIZ
 * The minimun distance between addresses to skip in case of collision during 
  * the allocation of a new carrier
 */
#define NVM_MIN_SKIP_SIZE        ONE_M 

/* 
 * NVM_MAX_SKIP_SIZE - Must be multiple of PAGE_SIZE
 * The maximum distance between addresses to skip in case of collision during 
 * the allocation of a new carrier.
 */
#define NVM_MAX_SKIP_SIZE        ONE_G 

/* 
 * NVM_MAX_ALLOC_SIZE - Must be a power of 2
 * Max size of each individual allocation
 */
#define NVM_MAX_ALLOC_SIZE       (ONE_G * 2)

/* 
 * NVM_MIN_ALLOC_SIZE - Must be a power of 2
 * Min size in bytes of each individual allocation
 */
#define NVM_MIN_ALLOC_SIZE       ((size_t)128)

/* 
 * NVM_MAX_TOTAL_ALLOC_SIZE
 * Maximum total allocatable memory
 */
#define NVM_MAX_TOTAL_ALLOC_SIZE (NVM_MAX_CARRIER_COUNT * NVM_MAX_ALLOC_SIZE)

/* 
 * NVM_ADDR_MIN
 * 
 */
#define NVM_ADDR_MIN             ((intptr_t)(ONE_G * 4)) //Addresses higher than this are considered to be NV

/* 
 * NVM_ADDR_MAX
 * Max address which is considered to be NVM
 */
//#define NVM_ADDR_MAX 0 //Uncomment case NVM_BOUNDARY_MAX is defined by a min-max range
#ifndef NVM_ADDR_MAX
#define NVM_ADDR_MAX (NVM_ADDR_MIN + NVM_MAX_TOTAL_ALLOC_SIZE)
#endif

typedef struct _SHR_NVM_STATE {
    uintptr_t nvm_state;
    char dmp_fname[256];
    uintptr_t carrier_ranges[NVM_MAX_CARRIER_COUNT * 2]; //start and end addrs
} NVMALLOC_SHR_NVM_STATE;
#define NVM_CARR_SH_MEM_NAME "nvmalloc"
#define NVM_CARR_SH_MEM_SZ (MAX((size_t)PAGE_SIZE, sizeof(NVMALLOC_SHR_NVM_STATE)))


//Assumes address space is inexaustable
void *pmalloc (size_t length);
void *pcalloc (size_t nmemb, size_t size);
void  pfree (void *p);

void  pset_root(void *p);
void *pget_root(void);

NVMALLOC_SHR_NVM_STATE *pinit (char *id); //restore and inits memory
void  pdump (void);

#ifdef __cplusplus
}
#endif

#endif //__NVMALLOC_H

#include "nvmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>


/*----------
 * DEBUGGING
 *----------*/

#ifdef DEBUG
#define LOG(...) fprintf(stderr, __VA_ARGS__); fflush(stderr)
#else
#define LOG(...) 
#endif //DEBUG

/*------------------------------
 * Constants and Data Structures
 *------------------------------*/

#define NVM_MIN_ALLOC_SIZE_CTZ (__builtin_ctz(NVM_MIN_ALLOC_SIZE))
#define NVM_PREFIX_SIZE        sizeof(freelistnode)
#define NVM_FREELIST_SIZE      (__builtin_ctz(NVM_MAX_ALLOC_SIZE) - NVM_MIN_ALLOC_SIZE_CTZ + 1)
#define NVM_FREELIST_POS(sz)   MAX((__builtin_ctz(NEXT_P2(sz)) - NVM_MIN_ALLOC_SIZE_CTZ), 0)

typedef struct _carrier {
    uintptr_t start_addr;
    uintptr_t end_addr;
    size_t    available;
} carrier;

static inline size_t carrier_size (carrier *c) {
    return c->end_addr - c->start_addr;
}

static inline uintptr_t carrier_next_address(carrier *c) {
    return c->end_addr - c->available;
}

static inline size_t carrier_used_bytes (carrier *c) {
    return carrier_size(c) - c->available;
}

typedef struct _freelistnode {
    uintptr_t addr;
    int  flpos;
    struct _freelistnode *next;
    
    char filler [64 - (
                        sizeof(uintptr_t) + //addr
                        sizeof(int) + //flpos
                        sizeof(struct _freelistnode *) + //next 
                        sizeof(char*) //filler
                )];
} freelistnode;

/*-----------------
 * State Variables
 *-----------------*/
 

typedef struct _nvm_state {
    void *root;
    uintptr_t next_free_address;
    int next_free_carrier;
    carrier carriers[NVM_MAX_CARRIER_COUNT];
    freelistnode *freelist[NVM_FREELIST_SIZE];
} nvm_state_t;

static nvm_state_t *nvm_state = NULL;

/*-----------------
 * Shared transient
 *-----------------*/
 
static NVMALLOC_SHR_NVM_STATE *sh_state_ctrl = NULL;
static int sh_state_ctrl_locally_loaded = 0;

static inline void get_shared_memory_region_name(char *buf) {
    sprintf(buf, "%s.%d", NVM_CARR_SH_MEM_NAME, getpid());
}

void destroy_sh_state_ctrl () {
    assert(sh_state_ctrl);
    
    LOG("NVMALLOC:destroy_sh_state_ctrl: munmap/shm_unlink-ing shared memory region\n");    
    int r = munmap(sh_state_ctrl, NVM_CARR_SH_MEM_SZ);
    if (r != 0)
        handle_error("NVMALLOC:Unable to munmap shared carrier ranges\n");
        
    char buf[256];
    get_shared_memory_region_name(buf);
    r = shm_unlink(buf);
    if (r != 0)
        handle_error("NVMALLOC:Unable to munmap shared carrier ranges\n");
}

//returns 1 if shared state was created or 0 if one was found
static inline int get_sh_state_ctrl() {
    int caze = 0;
    if (!sh_state_ctrl) {
        //For the time being enforces region size to a single page
        //so readers can assume this size without importing nvmalloc.h
        assert(NVM_CARR_SH_MEM_SZ == PAGE_SIZE);
        
        /*
         * Cases:
         * 0) Region was already created but symbols were not shared (e.g, 
         *    running inside ZSim)-> Gets the shared memory pointers and 
         *    initialize variables
         * 1) Region was not initialized. Creates a shared memory region and 
         *    initializes it.
        */
        
        LOG("NVMALLOC:get_sh_state_ctrl: Allocating shared memory region\n");
        //First evaluates if the shared memory region was already created
        char buf[256];
        get_shared_memory_region_name(buf);
        int fd = shm_open (buf, O_RDWR, 0666);
        caze = fd == -1;
        
        if (caze) { //Case 1            
            LOG("NVMALLOC:get_sh_state_ctrl: File doesn't exist. Creating: %s.\n", buf);
            //so we create it
            fd = shm_open (buf, O_CREAT | O_EXCL | O_RDWR, 0666);
            if (fd == -1)
                handle_error("NVMALLOC:Could not create shared memory space for carrier ranges\n");
            int r = ftruncate(fd, NVM_CARR_SH_MEM_SZ);
            if (r != 0)
                handle_error("NVMALLOC:Could not resize shared memory space for carrier ranges\n");
        } else {
            LOG("NVMALLOC:get_sh_state_ctrl: Found previously created sh mem fd: %s.\n", buf);
        }
        
        sh_state_ctrl = (NVMALLOC_SHR_NVM_STATE*) mmap(
            0, NVM_CARR_SH_MEM_SZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (sh_state_ctrl == MAP_FAILED)
            handle_error("NVMALLOC:Could not mmap shared memory space for carrier ranges\n");
        close(fd);
        
        if (!caze) { //Case 0 - memory was initialized but pointers are still unasigned
            nvm_state = (nvm_state_t *) sh_state_ctrl->nvm_state;
        } else { //Case 1 - sh memory not initialzed, needs to allocate state
            nvm_state = (nvm_state_t *) calloc (1, sizeof(nvm_state_t));
            nvm_state->next_free_address = NVM_ADDR_MIN;
            sh_state_ctrl->nvm_state = (uintptr_t) nvm_state;            
        
            //TODO Imperfect solution since abnormal terminations won't trigger the function
            int r = atexit(destroy_sh_state_ctrl);
            if (r != 0)
            handle_error("NVMALLOC:Could not set exit function\n");
        }        

        LOG("NVMALLOC:get_sh_state_ctrl: Allocation succeded.\n");
    }
    return caze;
}

static void update_shared_carrier_ranges (int carrier_ix) {
    assert(sh_state_ctrl);
    sh_state_ctrl->carrier_ranges[carrier_ix * 2] = nvm_state->carriers[carrier_ix].start_addr;
    sh_state_ctrl->carrier_ranges[carrier_ix * 2 + 1] = nvm_state->carriers[carrier_ix].end_addr;
}

/*-----------------
 * Static members
 *-----------------*/

static carrier *allocate_carrier(size_t sz) {
    size_t nsize = MAX(sz, NVM_MIN_CARRIER_SIZE);
    LOG("NVMALLOC:allocate_carrier: Requested: %ld Actual: %ld\n", sz, nsize);
    size_t skip = NVM_MIN_SKIP_SIZE;
    uintptr_t addr;
    
    assert (nvm_state->next_free_address >= NVM_ADDR_MIN && 
            nvm_state->next_free_address + sz < NVM_ADDR_MAX);
    
    while (1) {        
        addr = (uintptr_t)mmap(
            (void*)nvm_state->next_free_address, 
            nsize,
            PROT_READ | PROT_WRITE,  
            MAP_PRIVATE | MAP_ANONYMOUS, 
            0, 
            0);
            
        if (addr == (uintptr_t)MAP_FAILED) return NULL;
        
        if (nvm_state->next_free_address != addr && !IS_NVM_RANGE(addr)) {
            munmap((void*)addr, nsize);
            nvm_state->next_free_address += skip;
            skip = (skip < NVM_MAX_SKIP_SIZE) ? skip * 2 : skip;
            continue;
        }
        
        //Event if the location is not the requested, has the effect of at least skipping
        //size for the next allocation
        nvm_state->next_free_address += nsize; 
        
        assert(nvm_state->next_free_carrier < NVM_MAX_CARRIER_COUNT);
        carrier *carr = &nvm_state->carriers[nvm_state->next_free_carrier];
        carr->start_addr = addr;
        carr->end_addr = addr + nsize;
        carr->available = nsize;
        update_shared_carrier_ranges(nvm_state->next_free_carrier);
        nvm_state->next_free_carrier++;
        
        LOG("NVMALLOC:allocate_carrier: Completed. "
            "Start: 0x%" PRIxPTR " End: 0x%" PRIxPTR " Size: %zu\n",
            carr->start_addr,
            carr->end_addr,
            carr->available);
        
        return carr;
    }
}

static freelistnode* pmalloc_fl (int flpos) {
    size_t pot = flpos + NVM_MIN_ALLOC_SIZE_CTZ;
    size_t size = (size_t)1 << pot;
    
    LOG("NVMALLOC:pmalloc_fl: Req. Flpos: %d Pot: %zu Size: %zu\n", flpos, pot, size);
    
    carrier *carr = NULL;
    for (int i = 0; i < nvm_state->next_free_carrier; i++) {
        if (nvm_state->carriers[i].available >= size) {
            carr = &nvm_state->carriers[i];
            break;            
        }        
    }
    if (!carr) {
        //no suitable nvm_state.carriers found, allocate new one
        carr = allocate_carrier (size);
        if (!carr) {
            LOG("NVMALLOC:pmalloc_fl: No suitable carrier found");
            return NULL;
        }
    }
    
    LOG("NVMALLOC:pmalloc_fl: Carrier found ("
        "A: 0x%" PRIxPTR "-0x%" PRIxPTR " A: %zu)\n",
        carr->start_addr,
        carr->end_addr,
        carr->available);
    
    freelistnode *fln = (freelistnode *)(carrier_next_address(carr));
    fln->addr = (uintptr_t)fln;
    fln->flpos = flpos;
    fln->next = nvm_state->freelist[flpos];
    nvm_state->freelist[flpos] = fln;
    carr->available -= size;
    
    LOG("NVMALLOC:pmalloc_fl: Complete " 
        "@%p(FLN A: 0x%" PRIxPTR " P: %d N: %p)" 
        "(Car. BA: %zu)\n",
        fln, fln->addr, fln->flpos, fln->next,
        carr->available);
        
    return fln;
}


/*-----------------
 * Exported members
 *-----------------*/

void* pmalloc (size_t sz) {
    size_t nsize = sz + NVM_PREFIX_SIZE;
    int flpos = NVM_FREELIST_POS(nsize);
    LOG("NVMALLOC:pmalloc: Req. %zu Actual: %zu Flpos: %d nvm_state->freelist[flpos] %p\n", 
        sz, nsize, flpos, nvm_state->freelist[flpos]);
    assert(nsize <= NVM_MAX_ALLOC_SIZE);
    assert(flpos < NVM_FREELIST_SIZE);
    if (nvm_state->freelist[flpos] == NULL && !pmalloc_fl(flpos)) 
        return NULL;
    freelistnode *fln = nvm_state->freelist[flpos];
    nvm_state->freelist[flpos] = fln->next;
    void *ret = (void*)(fln->addr + NVM_PREFIX_SIZE);
    LOG("NVMALLOC:pmalloc: Complete P: %p, FLN: %p Flnpos: %d\n", ret, (void*)fln->addr, fln->flpos);
    return ret;
}

void *pcalloc(size_t nmemb, size_t size) {
    size_t tot = nmemb * size;
    void* ret = pmalloc(tot);
    bzero(ret, tot);
    return ret;        
}


void pfree (void *ptr) {
    LOG("NVMALLOC:pfree: Req P: %p\n", ptr);
    if (!ptr) return;
    assert(IS_NVM_RANGE(ptr));
    freelistnode *fln= (freelistnode *)(((uintptr_t)ptr) - NVM_PREFIX_SIZE);
    fln->next = nvm_state->freelist[fln->flpos];
    nvm_state->freelist[fln->flpos] = fln;       
    LOG("NVMALLOC:pfree: Complete Flpos: %d (@ %p) FLN: %p\n", fln->flpos, &(fln->flpos), fln);
}

void  pset_root(void *p) {
    assert (nvm_state);
    LOG("NVMALLOC:pset_root: %p\n", p);
    nvm_state->root = p;
}

void* pget_root() {
    assert (nvm_state);
    LOG("NVMALLOC:pget_root: %p\n", nvm_state->root);
    return nvm_state->root;
}

void pdump() {
    if (!nvm_state->root) 
        handle_error("NVMALLOC:pdump: Dumping a null pointer as root!\n");
    
    if (!sh_state_ctrl_locally_loaded) {
        LOG("NVMALLOC:pdump: Not locally loaded. Nothing to dump.\n");
        return;
    }
    
    FILE *dump = fopen(sh_state_ctrl->dmp_fname, "wb");
    if (!dump)
        handle_error("NVMALLOC:Unable to open file in write mode.");
    
    LOG("NVMALLOC:pdump: Dumping state to %s.  (St R: %p NFA: 0x%" PRIxPTR " NFC: %d)\n",
        sh_state_ctrl->dmp_fname, nvm_state->root, nvm_state->next_free_address,
        nvm_state->next_free_carrier);
    
    int stat = fwrite(nvm_state, sizeof(nvm_state_t), 1, dump);
    if (stat != 1)
        handle_error("NVMALLOC:Unable to write state to the file");
    
    for (int i = 0; i < nvm_state->next_free_carrier; i++) {
        carrier *carr = &nvm_state->carriers[i];
        LOG("NVMALLOC:pdump: Dumping carrier %d #bytes %zu/%zu "
            "(C A: 0x%" PRIxPTR "-0x%" PRIxPTR " A: %zu)\n",                
            i, carrier_used_bytes(carr), carrier_size(carr),
            carr->start_addr, carr->end_addr, carr->available);
        stat = fwrite( (void*)carr->start_addr, carrier_used_bytes(carr), 1, dump);
        if (stat != 1) 
            handle_error("NVMALLOC:Unable to dump carrier\n");
    }
    
    LOG("NVMALLOC:pdump: Dump complete!\n");
    fclose(dump);
}

NVMALLOC_SHR_NVM_STATE *pinit(char *id) {
    LOG("NVMALLOC:init: Restoring shared memory ctrl structure. File: %s\n", id);
    int new_sh_state = get_sh_state_ctrl();
    if (!new_sh_state) {
        LOG("NVMALLOC:init:Already initialized, nothing to do\n");
        assert (strcmp(id, sh_state_ctrl->dmp_fname) == 0);
        return sh_state_ctrl; //already initialized, nothing to do
    }

    //Ok, we'll load the state ourselves
    LOG("NVMALLOC:init:Local init -> 1, was: %d\n", sh_state_ctrl_locally_loaded);
    assert(!sh_state_ctrl_locally_loaded);
    sh_state_ctrl_locally_loaded = 1;
    strcpy(sh_state_ctrl->dmp_fname, id);
    
    //step one
    FILE *dump = fopen(id, "rb");
    if (!dump) goto one;
    LOG("NVMALLOC:init: Restoring dump from: %s\n", id);
    
    //step two
    int stat = fread(nvm_state, sizeof(nvm_state_t), 1, dump);
    if (stat != 1) goto two;
    LOG("NVMALLOC:init: Restored partial state (St R: %p NFA: 0x%" PRIxPTR " NFC: %d)\n",
        nvm_state->root, nvm_state->next_free_address, nvm_state->next_free_carrier);
    
    //NVM state partially restored. Now restore carriers.
    for (int i = 0; i < nvm_state->next_free_carrier; i++) {
        carrier *carr = &nvm_state->carriers[i];
        LOG("NVMALLOC:init: Restoring carrier %d. Allocating %zu bytes of memory.\n", i, carrier_size(carr));
        void* addr = mmap(
            (void*)carr->start_addr, 
            carrier_size(carr),
            PROT_READ | PROT_WRITE,  
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, 
            0, 
            0);
        if (addr == MAP_FAILED) //TODO Undo all memory allocations and return NULL would be a better solution
            handle_error("NVMALLOC:Unable to obtain memory at the original address");
        assert((void*)carr->start_addr == addr);
        LOG("NVMALLOC:init: Restoring carrier %d. Reading dump for %zu bytes.\n", i, carrier_used_bytes(carr));
        stat = fread(addr, carrier_used_bytes(carr), 1, dump);
        if (stat != 1) handle_error("Unable to read carrier.");
        update_shared_carrier_ranges (i);
        LOG("NVMALLOC:init: Restoration of carrier %d complete.\n", i);
    }
    
    LOG("NVMALLOC:init: Restoration Complete. Root: %p\n", nvm_state->root);
    two:
    fclose(dump);
    one:
    return sh_state_ctrl;
}

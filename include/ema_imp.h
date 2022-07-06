#ifndef SGX_EMA_IMP_H
#define SGX_EMA_IMP_H

#include <stdint.h>

#include "bit_array.h"
#include "ema.h"
#include "sgx_mm.h"

struct ema_t_
{
    size_t start_addr;  // starting address, should be on a page boundary
    size_t size;        // bytes
    uint32_t
        alloc_flags;    // EMA_RESERVED, EMA_COMMIT_NOW, EMA_COMMIT_ON_DEMAND,
                        // OR'ed with EMA_SYSTEM, EMA_GROWSDOWN, ENA_GROWSUP
    uint64_t si_flags;  // one of EMA_PROT_NONE, READ, READ_WRITE, READ_EXEC,
                        // READ_WRITE_EXEC Or'd with one of EMA_PAGE_TYPE_REG,
                        // EMA_PAGE_TYPE_TCS, EMA_PAGE_TYPE_TRIM
    bit_array*
        eaccept_map;  // bitmap for EACCEPT status, bit 0 in eaccept_map[0] for
                      // the page at start address bit i in eaccept_map[j] for
                      // page at start_address+(i+j<<3)<<12
    sgx_enclave_fault_handler_t
        handler;  // custom PF handler  (for EACCEPTCOPY use)
    void* priv;   // private data for handler
    ema_t* next;  // next in doubly linked list
    ema_t* prev;  // prev in doubly linked list
};
#endif

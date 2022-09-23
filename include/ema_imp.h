/*
 * Copyright (C) 2022 Intel Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

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

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

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bit_array.h"
#include "ema_imp.h"
#include "emalloc.h"
#include "sgx_mm.h"
#include "sgx_mm_primitives.h"
#include "sgx_mm_rt_abstraction.h"

/* State flags */
#define SGX_EMA_STATE_PENDING  0x8UL
#define SGX_EMA_STATE_MODIFIED 0x10UL
#define SGX_EMA_STATE_PR       0x20UL
#define UNUSED(x)              ((void)(x))

struct ema_root_
{
    ema_t* guard;
};

extern size_t mm_user_base;
extern size_t mm_user_end;

static bool is_within_user_range(size_t start, size_t size)
{
    if (start + size < start) return false;
    return start >= mm_user_base && start + size <= mm_user_end;
}

static bool is_within_rts_range(size_t start, size_t size)
{
    if (start + size < start) return false;
    return start >= mm_user_end || start + size <= mm_user_base;
}

ema_t rts_ema_guard = {.next = &rts_ema_guard, .prev = &rts_ema_guard};
ema_root_t g_rts_ema_root = {.guard = &rts_ema_guard};

ema_t user_ema_guard = {.next = &user_ema_guard, .prev = &user_ema_guard};
ema_root_t g_user_ema_root = {.guard = &user_ema_guard};

#ifdef TEST
static void dump_ema_node(ema_t* node, size_t index)
{
    printf("------ node #%lu ------\n", index);
    printf("start:\t0x%lX\n", node->start_addr);
    printf("size:\t0x%lX\n", node->size);
}

void dump_ema_root(ema_root_t* root)
{
    ema_t* node = root->guard->next;
    size_t index = 0;

    while (node != root->guard)
    {
        dump_ema_node(node, index++);
        node = node->next;
    }
}

#endif
void destroy_ema_root(ema_root_t* root)
{
    ema_t* node = root->guard->next;
    size_t index = 0;

    while (node != root->guard)
    {
        index++;
        ema_t* next = node->next;
        ema_destroy(node);
        node = next;
    }
#if 0
    printf("Destroy %lu nodes on the root\n", index);
#endif
}

#ifdef TEST
size_t ema_base(ema_t* node)
{
    return node->start_addr;
}

size_t ema_size(ema_t* node)
{
    return node->size;
}
#endif
#ifndef NDEBUG
ema_t* ema_next(ema_t* node)
{
    return node->next;
}
#endif

uint32_t get_ema_alloc_flags(ema_t* node)
{
    return node->alloc_flags;
}

uint64_t get_ema_si_flags(ema_t* node)
{
    return node->si_flags;
}

sgx_enclave_fault_handler_t ema_fault_handler(ema_t* node, void** private_data)
{
    if (private_data) *private_data = node->priv;
    return node->handler;
}

static void ema_clone(ema_t* dst, ema_t* src)
{
    memcpy((void*)dst, (void*)src, sizeof(ema_t));
}

static bool ema_lower_than_addr(ema_t* ema, size_t addr)
{
    return ((ema->start_addr + ema->size) <= addr);
}

static bool ema_higher_than_addr(ema_t* ema, size_t addr)
{
    return (ema->start_addr >= addr);
}

static bool ema_overlap_addr(const ema_t* ema, size_t addr)
{
    if ((addr >= ema->start_addr) && (addr < ema->start_addr + ema->size))
        return true;
    return false;
}

int ema_set_eaccept_full(ema_t* node)
{
    if (!node->eaccept_map)
    {
        node->eaccept_map = bit_array_new_set((node->size) >> SGX_PAGE_SHIFT);
        if (!node->eaccept_map)
            return ENOMEM;
        else
            return 0;
    }
    else
        bit_array_set_all(node->eaccept_map);
    return 0;
}

int ema_clear_eaccept_full(ema_t* node)
{
    if (!node->eaccept_map)
    {
        node->eaccept_map = bit_array_new_reset((node->size) >> SGX_PAGE_SHIFT);
        if (!node->eaccept_map)
            return ENOMEM;
        else
            return 0;
    }
    else
        bit_array_reset_all(node->eaccept_map);
    return 0;
}

int ema_set_eaccept(ema_t* node, size_t start, size_t end)
{
    if (!node)
    {
        return EINVAL;
    }

    assert(start >= node->start_addr);
    assert(end <= node->start_addr + node->size);
    size_t pos_begin = (start - node->start_addr) >> SGX_PAGE_SHIFT;
    size_t pos_end = (end - node->start_addr) >> SGX_PAGE_SHIFT;

    // update eaccept bit map
    if (!node->eaccept_map)
    {
        node->eaccept_map = bit_array_new_reset((node->size) >> SGX_PAGE_SHIFT);
        if (!node->eaccept_map) return ENOMEM;
    }
    bit_array_set_range(node->eaccept_map, pos_begin, pos_end - pos_begin);
    return 0;
}

bool ema_page_committed(ema_t* ema, size_t addr)
{
    assert(!(addr % SGX_PAGE_SIZE));
    if (!ema->eaccept_map)
    {
        return false;
    }

    return bit_array_test(ema->eaccept_map,
                          (addr - ema->start_addr) >> SGX_PAGE_SHIFT);
}

// search for a node whose address range contains 'addr'
ema_t* search_ema(ema_root_t* root, size_t addr)
{
    for (ema_t* node = root->guard->next; node != root->guard;
         node = node->next)
    {
        if (ema_overlap_addr(node, addr))
        {
            return node;
        }
    }
    return NULL;
}

// insert 'new_node' before 'node'
ema_t* insert_ema(ema_t* new_node, ema_t* node)
{
    new_node->prev = node->prev;
    new_node->next = node;
    node->prev->next = new_node;
    node->prev = new_node;
    return new_node;
}

static void replace_ema(ema_t* new_node, ema_t* old_node)
{
    old_node->prev->next = new_node;
    old_node->next->prev = new_node;
    new_node->next = old_node->next;
    new_node->prev = old_node->prev;
}

// Remove the 'node' from the list
static ema_t* remove_ema(ema_t* node)
{
    if (!node) return node;

    // Sanity check pointers for corruption
    if ((node->prev->next != node) || (node->next->prev != node))
    {
        abort();
    }

    node->prev->next = node->next;
    node->next->prev = node->prev;
    return node;
}

void push_back_ema(ema_root_t* root, ema_t* node)
{
    insert_ema(node, root->guard);
}

// search for a range of nodes containing addresses within [start, end)
// 'ema_begin' will hold the fist ema that has address higher than /euqal to
// 'start' 'ema_end' will hold the node immediately follow the last ema that has
// address lower than / equal to 'end'
int search_ema_range(ema_root_t* root, size_t start, size_t end,
                     ema_t** ema_begin, ema_t** ema_end)
{
    ema_t* node = root->guard->next;

    // find the first node that has addr >= 'start'
    while ((node != root->guard) && ema_lower_than_addr(node, start))
    {
        node = node->next;
    }

    // empty list or all nodes are beyond [start, end)
    if ((node == root->guard) || ema_higher_than_addr(node, end))
    {
        *ema_begin = NULL;
        *ema_end = NULL;
        return -1;
    }

    *ema_begin = node;

    // find the last node that has addr <= 'end'
    while ((node != root->guard) && (!ema_higher_than_addr(node, end)))
    {
        node = node->next;
    }
    *ema_end = node;

    return 0;
}

// We just split and emalloc_free will merge unused and reuse blocks
int ema_split(ema_t* ema, size_t addr, bool new_lower, ema_t** ret_node)
{
    // this is only needed for UT
    // in real usage in the file, addr always overlap
#ifdef TEST
    if (!ema_overlap_addr(ema, addr) || !ret_node)
    {
        return EINVAL;
    }
#else
    assert(ema_overlap_addr(ema, addr));
    assert(ret_node);
#endif

    ema_t* new_node = (ema_t*)emalloc(sizeof(ema_t));
    if (!new_node)
    {
        return ENOMEM;
    }

    bit_array *low = NULL, *high = NULL;
    if (ema->eaccept_map)
    {
        size_t pos = (addr - ema->start_addr) >> SGX_PAGE_SHIFT;
        int ret = bit_array_split(ema->eaccept_map, pos, &low, &high);
        if (ret)
        {
            efree(new_node);
            return ret;
        }
    }

    // caller does not need free new_node as it is inserted
    // and managed in root when this returns
    ema_clone(new_node, ema);

    ema_t *lo_ema = NULL, *hi_ema = NULL;
    if (new_lower)
    {
        // new node for lower address
        lo_ema = new_node;
        hi_ema = ema;
        insert_ema(new_node, ema);
    }
    else
    {
        lo_ema = ema;
        hi_ema = new_node;
        insert_ema(new_node, ema->next);
    }

    size_t start = ema->start_addr;
    size_t size = ema->size;

    lo_ema->start_addr = start;
    lo_ema->size = addr - start;
    hi_ema->start_addr = addr;
    hi_ema->size = size - lo_ema->size;

    if (ema->eaccept_map)
    {
        lo_ema->eaccept_map = low;
        hi_ema->eaccept_map = high;
    }
    *ret_node = new_node;
    return 0;
}

int ema_split_ex(ema_t* ema, size_t start, size_t end, ema_t** new_node)
{
    ema_t* node = ema;
    ema_t* tmp_node;
    if (start > node->start_addr)
    {
        int ret = ema_split(node, start, false, &tmp_node);
        if (ret) return ret;
        if (tmp_node) node = tmp_node;
    }
    tmp_node = NULL;
    if (end < (node->start_addr + node->size))
    {
        int ret = ema_split(node, end, true, &tmp_node);
        if (ret) return ret;
        if (tmp_node) node = tmp_node;
    }
    *new_node = node;
    return 0;
}

static size_t ema_aligned_end(ema_t* ema, size_t align)
{
    size_t curr_end = ema->start_addr + ema->size;
    curr_end = ROUND_TO(curr_end, align);
    return curr_end;
}

// Find a free space of size at least 'size' bytes on the given root, does not
// matter where the start is
bool find_free_region(ema_root_t* root, size_t size, uint64_t align,
                      size_t* addr, ema_t** next_ema)
{
    bool is_rts = (root == &g_rts_ema_root);
    ema_t* ema_begin = root->guard->next;
    ema_t* ema_end = root->guard;

    *next_ema = NULL;
    *addr = 0;

    // no ema node on the root
    if (ema_begin == ema_end)
    {
        size_t tmp = 0;
        if (is_rts)
        {
            bool found = false;
            if (mm_user_base >= size)
            {
                tmp = TRIM_TO(mm_user_base - size, align);
                found = sgx_mm_is_within_enclave((void*)tmp, size);
            }
            if (!found)
            {
                tmp = ROUND_TO(mm_user_end, align);
                found = tmp + size >= tmp &&  // No integer overflow
                        sgx_mm_is_within_enclave((void*)tmp, size);
            }
            if (!found) return false;
            assert(is_within_rts_range(tmp, size));

            *addr = tmp;
            *next_ema = ema_end;
            return true;
        }
        else
        {
            tmp = ROUND_TO(mm_user_base, align);
            if (is_within_user_range(tmp, size))
            {
                *addr = tmp;
                *next_ema = ema_end;
                return true;
            }
        }
        return false;
    }

    // iterate over the ema nodes
    ema_t* curr = ema_begin;
    ema_t* next = curr->next;

    while (next != ema_end)
    {
        size_t curr_end = ema_aligned_end(curr, align);
        if (curr_end <= next->start_addr)
        {
            size_t free_size = next->start_addr - curr_end;
            if (free_size >= size)
            {
                if (!is_rts || is_within_rts_range(curr_end, size))
                {
                    *next_ema = next;
                    *addr = curr_end;
                    return true;
                }
            }
        }
        curr = next;
        next = curr->next;
    }

    // check the region higher than last ema node
    size_t tmp = ema_aligned_end(curr, align);
    if (sgx_mm_is_within_enclave((void*)tmp, size))
    {
        if ((is_rts && is_within_rts_range(tmp, size)) ||
            (!is_rts && is_within_user_range(tmp, size)))
        {
            *next_ema = next;
            *addr = tmp;
            return true;
        }
    }

    // check the region lower than the first ema node
    if (ema_begin->start_addr < size) return false;

    tmp = TRIM_TO(ema_begin->start_addr - size, align);
    if (!is_rts)
    {
        if (is_within_user_range(tmp, size))
        {
            *addr = tmp;
            *next_ema = ema_begin;
            return true;
        }
    }
    else if (sgx_mm_is_within_enclave((void*)tmp, size))
    {
        if (is_within_rts_range(tmp, size))
        {
            *addr = tmp;
            *next_ema = ema_begin;
            return true;
        }
    }

    return false;
}

bool find_free_region_at(ema_root_t* root, size_t addr, size_t size,
                         ema_t** next_ema)
{
    if (!sgx_mm_is_within_enclave((void*)(addr), size))
    {
        *next_ema = NULL;
        return false;
    }
    bool is_rts = (root == &g_rts_ema_root);
    if ((is_rts && !is_within_rts_range(addr, size)) ||
        (!is_rts && !is_within_user_range(addr, size)))
    {
        *next_ema = NULL;
        return false;
    }

    ema_t* node = root->guard->next;
    while (node != root->guard)
    {
        if (node->start_addr >= (addr + size))
        {
            *next_ema = node;
            return true;
        }
        if (addr >= (node->start_addr + node->size))
        {
            node = node->next;
        }
        else
        {
            break;
        }
    }
    if (node == root->guard)
    {
        *next_ema = node;
        return true;
    }

    *next_ema = NULL;
    return false;
}

ema_t* ema_new(size_t addr, size_t size, uint32_t alloc_flags,
               uint64_t si_flags, sgx_enclave_fault_handler_t handler,
               void* private_data, ema_t* next_ema)
{
    // allocate a temp on stack, which is already allocated, i.e.,
    // stack expansion won't create new nodes recursively.
    ema_t tmp = {
        .start_addr = addr,
        .size = size,
        .alloc_flags = alloc_flags,
        .si_flags = si_flags,
        .eaccept_map = NULL,
        .handler = handler,
        .priv = private_data,
        .next = NULL,
        .prev = NULL,
    };

    // ensure region [start, start+size) is in the list so emalloc won't use it.
    insert_ema(&tmp, next_ema);
    ema_t* node = (ema_t*)emalloc(sizeof(ema_t));
    if (node)
    {
        *node = tmp;
        replace_ema(node, &tmp);
        return node;
    }
    else
    {
        remove_ema(&tmp);
        return NULL;
    }
}

void ema_destroy(ema_t* ema)
{
    remove_ema(ema);
    if (ema->eaccept_map)
    {
        bit_array_delete(ema->eaccept_map);
    }
    efree(ema);
}

static int eaccept_range_forward(const sec_info_t* si, size_t start, size_t end)
{
    while (start < end)
    {
        if (do_eaccept(si, start)) abort();
        start += SGX_PAGE_SIZE;
    }
    return 0;
}

static int eaccept_range_backward(const sec_info_t* si, size_t start,
                                  size_t end)
{
    assert(start < end);
    do
    {
        end -= SGX_PAGE_SIZE;
        if (do_eaccept(si, end)) abort();
    } while (end > start);
    return 0;
}

int do_commit(size_t start, size_t size, uint64_t si_flags, bool grow_up)
{
    sec_info_t si SGX_SECINFO_ALIGN = {si_flags | SGX_EMA_STATE_PENDING, 0};
    int ret = -1;

    if (grow_up)
    {
        ret = eaccept_range_backward(&si, start, start + size);
    }
    else
    {
        ret = eaccept_range_forward(&si, start, start + size);
    }

    return ret;
}

int ema_do_commit(ema_t* node, size_t start, size_t end)
{
    // Only RESERVE region has no bit map allocated.
    assert(node->eaccept_map);
    size_t real_start = MAX(start, node->start_addr);
    size_t real_end = MIN(end, node->start_addr + node->size);

    sec_info_t si SGX_SECINFO_ALIGN = {
        SGX_EMA_PAGE_TYPE_REG | SGX_EMA_PROT_READ_WRITE | SGX_EMA_STATE_PENDING,
        0};

    for (size_t addr = real_start; addr < real_end; addr += SGX_PAGE_SIZE)
    {
        size_t pos = (addr - node->start_addr) >> SGX_PAGE_SHIFT;
        // only commit for uncommitted page
        if (!bit_array_test(node->eaccept_map, pos))
        {
            int ret = do_eaccept(&si, addr);
            if (ret != 0)
            {
                return ret;
            }
            bit_array_set(node->eaccept_map, pos);
        }
    }

    return 0;
}

static int ema_can_commit(ema_t* first, ema_t* last, size_t start, size_t end)
{
    ema_t* curr = first;
    size_t prev_end = first->start_addr;
    while (curr != last)
    {
        if (prev_end != curr->start_addr)  // there is a gap
            return EINVAL;

        if (!(curr->si_flags & (SGX_EMA_PROT_WRITE))) return EACCES;

        if (!(curr->si_flags & (SGX_EMA_PAGE_TYPE_REG))) return EACCES;

        if ((curr->alloc_flags & (SGX_EMA_RESERVE))) return EACCES;

        prev_end = curr->start_addr + curr->size;
        curr = curr->next;
    }
    if (prev_end < end) return EINVAL;
    return 0;
}

int ema_do_commit_loop(ema_t* first, ema_t* last, size_t start, size_t end)
{
    int ret = ema_can_commit(first, last, start, end);
    if (ret) return ret;

    ema_t *curr = first, *next = NULL;

    while (curr != last)
    {
        next = curr->next;
        ret = ema_do_commit(curr, start, end);
        if (ret != 0)
        {
            return ret;
        }
        curr = next;
    }
    return ret;
}

static int ema_do_uncommit_real(ema_t* node, size_t real_start, size_t real_end,
                                int prot)
{
    int type = node->si_flags & SGX_EMA_PAGE_TYPE_MASK;
    uint32_t alloc_flags = node->alloc_flags & SGX_EMA_ALLOC_FLAGS_MASK;

    // ignore if ema is in reserved state
    if (alloc_flags & SGX_EMA_RESERVE)
    {
        return 0;
    }

    // Only RESERVE region has no bit map allocated.
    assert(node->eaccept_map);

    sec_info_t si SGX_SECINFO_ALIGN = {
        SGX_EMA_PAGE_TYPE_TRIM | SGX_EMA_STATE_MODIFIED, 0};

    while (real_start < real_end)
    {
        size_t block_start = real_start;
        while (block_start < real_end)
        {
            size_t pos = (block_start - node->start_addr) >> SGX_PAGE_SHIFT;
            if (bit_array_test(node->eaccept_map, pos))
            {
                break;
            }
            else
            {
                block_start += SGX_PAGE_SIZE;
            }
        }
        if (block_start == real_end) break;

        size_t block_end = block_start + SGX_PAGE_SIZE;
        while (block_end < real_end)
        {
            size_t pos = (block_end - node->start_addr) >> SGX_PAGE_SHIFT;
            if (bit_array_test(node->eaccept_map, pos))
            {
                block_end += SGX_PAGE_SIZE;
            }
            else
                break;
        }
        assert(block_end > block_start);
        // only for committed page
        size_t block_length = block_end - block_start;
        int ret = sgx_mm_modify_ocall(block_start, block_length, prot | type,
                                      prot | SGX_EMA_PAGE_TYPE_TRIM);
        if (ret != 0)
        {
            return EFAULT;
        }

        ret = eaccept_range_forward(&si, block_start, block_end);
        if (ret != 0)
        {
            return ret;
        }
        bit_array_reset_range(
            node->eaccept_map,
            (block_start - node->start_addr) >> SGX_PAGE_SHIFT,
            block_length >> SGX_PAGE_SHIFT);
        // eaccept trim notify
        ret = sgx_mm_modify_ocall(block_start, block_length,
                                  prot | SGX_EMA_PAGE_TYPE_TRIM,
                                  prot | SGX_EMA_PAGE_TYPE_TRIM);
        if (ret) return EFAULT;

        real_start = block_end;
    }
    return 0;
}

int ema_do_uncommit(ema_t* node, size_t start, size_t end)
{
    size_t real_start = MAX(start, node->start_addr);
    size_t real_end = MIN(end, node->start_addr + node->size);
    int prot = node->si_flags & SGX_EMA_PROT_MASK;
    if (prot == SGX_EMA_PROT_NONE)  // need READ for trimming
        ema_modify_permissions(node, start, end, SGX_EMA_PROT_READ);
    return ema_do_uncommit_real(node, real_start, real_end, prot);
}
static int ema_can_uncommit(ema_t* first, ema_t* last, size_t start, size_t end)
{
    ema_t* curr = first;
    size_t prev_end = first->start_addr;
    while (curr != last)
    {
        if (prev_end != curr->start_addr)  // there is a gap
            return EINVAL;

        if ((curr->alloc_flags & (SGX_EMA_RESERVE))) return EACCES;

        prev_end = curr->start_addr + curr->size;
        curr = curr->next;
    }
    if (prev_end < end) return EINVAL;
    return 0;
}

int ema_do_uncommit_loop(ema_t* first, ema_t* last, size_t start, size_t end)
{
    int ret = ema_can_uncommit(first, last, start, end);
    if (ret) return ret;

    ema_t *curr = first, *next = NULL;
    while (curr != last)
    {
        next = curr->next;
        ret = ema_do_uncommit(curr, start, end);
        if (ret != 0)
        {
            return ret;
        }
        curr = next;
    }
    return ret;
}

int ema_do_dealloc(ema_t* node, size_t start, size_t end)
{
    int alloc_flag = node->alloc_flags & SGX_EMA_ALLOC_FLAGS_MASK;
    size_t real_start = MAX(start, node->start_addr);
    size_t real_end = MIN(end, node->start_addr + node->size);
    int prot = node->si_flags & SGX_EMA_PROT_MASK;
    ema_t* tmp_node = NULL;
    int ret = EFAULT;

    if (alloc_flag & SGX_EMA_RESERVE)
    {
        goto split_and_destroy;
    }

    // Only RESERVE region has no bit map allocated.
    assert(node->eaccept_map);
    if (prot == SGX_EMA_PROT_NONE)  // need READ for trimming
        ema_modify_permissions(node, start, end, SGX_EMA_PROT_READ);
    // clear protections flag for dealloc
    ret = ema_do_uncommit_real(node, real_start, real_end, SGX_EMA_PROT_NONE);
    if (ret != 0) return ret;

split_and_destroy:
    // potential ema split
    if (real_start > node->start_addr)
    {
        ret = ema_split(node, real_start, false, &tmp_node);
        if (ret) return ret;
        assert(tmp_node);
        node = tmp_node;
    }

    tmp_node = NULL;
    if (real_end < (node->start_addr + node->size))
    {
        ret = ema_split(node, real_end, true, &tmp_node);
        if (ret) return ret;
        assert(tmp_node);
        node = tmp_node;
    }

    ema_destroy(node);
    return 0;
}

int ema_do_dealloc_loop(ema_t* first, ema_t* last, size_t start, size_t end)
{
    int ret = 0;
    ema_t *curr = first, *next = NULL;

    while (curr != last)
    {
        next = curr->next;
        ret = ema_do_dealloc(curr, start, end);
        if (ret != 0)
        {
            return ret;
        }
        curr = next;
    }
    return ret;
}

// change the type of the page to TCS
int ema_change_to_tcs(ema_t* node, size_t addr)
{
    int prot = node->si_flags & SGX_EMA_PROT_MASK;
    int type = node->si_flags & SGX_EMA_PAGE_TYPE_MASK;

    // page need to be already committed
    if (!ema_page_committed(node, addr))
    {
        return EACCES;
    }

    if (type == SGX_EMA_PAGE_TYPE_TCS)
    {
        return 0;  // already committed to TCS type
    }

    if (prot != SGX_EMA_PROT_READ_WRITE) return EACCES;
    if (type != SGX_EMA_PAGE_TYPE_REG) return EACCES;

    int ret = sgx_mm_modify_ocall(addr, SGX_PAGE_SIZE, prot | type,
                                  prot | SGX_EMA_PAGE_TYPE_TCS);
    if (ret != 0)
    {
        return EFAULT;
    }

    sec_info_t si SGX_SECINFO_ALIGN = {
        SGX_EMA_PAGE_TYPE_TCS | SGX_EMA_STATE_MODIFIED, 0};
    if (do_eaccept(&si, addr) != 0)
    {
        abort();
    }

    // operation succeeded, update ema node: state update, split
    ema_t* tcs = NULL;
    ret = ema_split_ex(node, addr, addr + SGX_PAGE_SIZE, &tcs);
    if (ret) return ret;
    assert(tcs);  // ema_split_ex should not return NULL if node!=NULL

    tcs->si_flags = (tcs->si_flags & (uint64_t)(~SGX_EMA_PAGE_TYPE_MASK) &
                     (uint64_t)(~SGX_EMA_PROT_MASK)) |
                    SGX_EMA_PAGE_TYPE_TCS | SGX_EMA_PROT_NONE;
    return ret;
}

int ema_modify_permissions(ema_t* node, size_t start, size_t end, int new_prot)
{
    int prot = node->si_flags & SGX_EMA_PROT_MASK;
    int type = node->si_flags & SGX_EMA_PAGE_TYPE_MASK;
    if (prot == new_prot) return 0;

    size_t real_start = MAX(start, node->start_addr);
    size_t real_end = MIN(end, node->start_addr + node->size);

    int ret = sgx_mm_modify_ocall(real_start, real_end - real_start,
                                  prot | type, new_prot | type);
    if (ret != 0)
    {
        return EFAULT;
    }

    sec_info_t si SGX_SECINFO_ALIGN = {
        (uint64_t)new_prot | SGX_EMA_PAGE_TYPE_REG | SGX_EMA_STATE_PR, 0};

    for (size_t page = real_start; page < real_end; page += SGX_PAGE_SIZE)
    {
        if ((new_prot | prot) != prot) do_emodpe(&si, page);

        // new permission is RWX, no EMODPR needed in untrusted part, hence no
        // EACCEPT
        if ((new_prot & (SGX_EMA_PROT_WRITE | SGX_EMA_PROT_EXEC)) !=
            (SGX_EMA_PROT_WRITE | SGX_EMA_PROT_EXEC))
        {
            ret = do_eaccept(&si, page);
            if (ret) return ret;
        }
    }

    // all involved pages complete permission change, deal with potential
    // ema node split and  update permission state
    if (real_start > node->start_addr)
    {
        ema_t* tmp_node = NULL;
        ret = ema_split(node, real_start, false, &tmp_node);
        if (ret) return ret;
        assert(tmp_node);
        node = tmp_node;
    }

    if (real_end < (node->start_addr + node->size))
    {
        ema_t* tmp_node = NULL;
        ret = ema_split(node, real_end, true, &tmp_node);
        if (ret) return ret;
        assert(tmp_node);
        node = tmp_node;
    }

    // 'node' is the ema node to update permission for
    node->si_flags =
        (node->si_flags & (uint64_t)(~SGX_EMA_PROT_MASK)) | (uint64_t)new_prot;
    if (new_prot == SGX_EMA_PROT_NONE)
    {  // do mprotect if target is PROT_NONE
        ret = sgx_mm_modify_ocall(real_start, real_end - real_start,
                                  type | SGX_EMA_PROT_NONE,
                                  type | SGX_EMA_PROT_NONE);
        if (ret) ret = EFAULT;
    }
    return ret;
}

static int ema_can_modify_permissions(ema_t* first, ema_t* last, size_t start,
                                      size_t end)
{
    ema_t* curr = first;
    size_t prev_end = first->start_addr;
    while (curr != last)
    {
        if (prev_end != curr->start_addr)  // there is a gap
            return EINVAL;

        if (!(curr->si_flags & (SGX_EMA_PAGE_TYPE_REG))) return EACCES;

        if ((curr->alloc_flags & (SGX_EMA_RESERVE))) return EACCES;

        size_t real_start = MAX(start, curr->start_addr);
        size_t real_end = MIN(end, curr->start_addr + curr->size);

        size_t pos_begin = (real_start - curr->start_addr) >> SGX_PAGE_SHIFT;
        size_t pos_end = (real_end - curr->start_addr) >> SGX_PAGE_SHIFT;
        if (!curr->eaccept_map ||
            !bit_array_test_range(curr->eaccept_map, pos_begin,
                                  pos_end - pos_begin))
        {
            return EINVAL;
        }

        prev_end = curr->start_addr + curr->size;
        curr = curr->next;
    }
    if (prev_end < end) return EINVAL;
    return 0;
}

static int ema_modify_permissions_loop_nocheck(ema_t* first, ema_t* last,
                                               size_t start, size_t end,
                                               int prot)
{
    int ret = 0;
    ema_t *curr = first, *next = NULL;
    while (curr != last)
    {
        next = curr->next;
        ret = ema_modify_permissions(curr, start, end, prot);
        if (ret != 0)
        {
            return ret;
        }
        curr = next;
    }
    return ret;
}

int ema_modify_permissions_loop(ema_t* first, ema_t* last, size_t start,
                                size_t end, int prot)
{
    int ret = ema_can_modify_permissions(first, last, start, end);
    if (ret) return ret;

    return ema_modify_permissions_loop_nocheck(first, last, start, end, prot);
}

static int ema_can_commit_data(ema_t* first, ema_t* last, size_t start,
                               size_t end)
{
    ema_t* curr = first;
    size_t prev_end = first->start_addr;
    while (curr != last)
    {
        if (prev_end != curr->start_addr)  // there is a gap
            return EINVAL;

        if (!(curr->si_flags & (SGX_EMA_PROT_WRITE))) return EACCES;

        if (!(curr->si_flags & (SGX_EMA_PAGE_TYPE_REG))) return EACCES;

        if ((curr->alloc_flags & (SGX_EMA_RESERVE))) return EACCES;

        if (!(curr->alloc_flags & (SGX_EMA_COMMIT_ON_DEMAND))) return EINVAL;

        if (curr->eaccept_map)
        {
            size_t real_start = MAX(start, curr->start_addr);
            size_t real_end = MIN(end, curr->start_addr + curr->size);
            size_t pos_begin =
                (real_start - curr->start_addr) >> SGX_PAGE_SHIFT;
            size_t pos_end = (real_end - curr->start_addr) >> SGX_PAGE_SHIFT;

            if (bit_array_test_range_any(curr->eaccept_map, pos_begin,
                                         pos_end - pos_begin))
                return EACCES;
        }
        prev_end = curr->start_addr + curr->size;
        curr = curr->next;
    }
    if (prev_end < end) return EINVAL;
    return 0;
}

int ema_do_commit_data(ema_t* node, size_t start, size_t end, uint8_t* data,
                       int prot)
{
    size_t addr = start;
    size_t src = (size_t)data;
    sec_info_t si SGX_SECINFO_ALIGN = {(uint64_t)prot | SGX_EMA_PAGE_TYPE_REG,
                                       0};

    while (addr < end)
    {
        int ret = do_eacceptcopy(&si, addr, src);
        if (ret != 0)
        {
            return EFAULT;
        }
        addr += SGX_PAGE_SIZE;
        src += SGX_PAGE_SIZE;
    }
    return ema_set_eaccept(node, start, end);
}

int ema_do_commit_data_loop(ema_t* first, ema_t* last, size_t start, size_t end,
                            uint8_t* data, int prot)
{
    int ret = 0;
    ret = ema_can_commit_data(first, last, start, end);
    if (ret) return ret;

    ema_t* curr = first;
    while (curr != last)
    {  // there is no split in this loop
        size_t real_start = MAX(start, curr->start_addr);
        size_t real_end = MIN(end, curr->start_addr + curr->size);
        uint8_t* real_data = data + real_start - start;
        ret = ema_do_commit_data(curr, real_start, real_end, real_data, prot);
        if (ret != 0)
        {
            return ret;
        }
        curr = curr->next;
    }

    ret = ema_modify_permissions_loop_nocheck(first, last, start, end, prot);
    return ret;
}

ema_t* ema_realloc_from_reserve_range(ema_t* first, ema_t* last, size_t start,
                                      size_t end, uint32_t alloc_flags,
                                      uint64_t si_flags,
                                      sgx_enclave_fault_handler_t handler,
                                      void* private_data)
{
    assert(first != NULL);
    assert(last != NULL);
    ema_t* curr = first;
    assert(first->start_addr < end);
    assert(last->prev->start_addr + last->prev->size > start);
    // fail on any nodes not reserve or any gaps
    size_t prev_end = first->start_addr;
    while (curr != last)
    {
        // do not touch internal reserve.
        if (!can_erealloc(curr)) return NULL;
        if (prev_end != curr->start_addr)  // there is a gap
            return NULL;
        if (curr->alloc_flags & SGX_EMA_RESERVE)
        {
            prev_end = curr->start_addr + curr->size;
            curr = curr->next;
        }
        else
            return NULL;
    }

    int ret = 0;
    // Splitting nodes may add more emalloc reserve nodes.
    // Those can be appended and move the "guard" which
    // could be the last node
    // We track the the last inclusive node.
    ema_t* last_inclusive = last->prev;
    if (start > first->start_addr)
    {
	ema_t* ofirst = first;
        ret = ema_split(first, start, false, &first);
	if (ret) return NULL;
        //old first was split, we need update last_inclusive
        //if the old first was also the last_inclusive
        if (ofirst == last_inclusive)
            last_inclusive = first;
    }

    if (end < last_inclusive->start_addr + last_inclusive->size)
    {
        ret = ema_split(last_inclusive, end, false, &last);
        if (ret) return NULL;
    } else
	last = last_inclusive->next;

    assert(first->alloc_flags & SGX_EMA_RESERVE);
    assert(!first->eaccept_map);

    curr = first;
    while (curr != last)
    {
        ema_t* next = curr->next;
        ema_destroy(curr);
        curr = next;
    }

    ema_t* new_node = ema_new(start, end - start, alloc_flags, si_flags,
                              handler, private_data, last);
    return new_node;
}

int ema_do_alloc(ema_t* node)
{
    uint32_t alloc_flags = node->alloc_flags;
    if (alloc_flags & SGX_EMA_RESERVE)
    {
        return 0;
    }

    size_t tmp_addr = node->start_addr;
    size_t size = node->size;
    int ret = sgx_mm_alloc_ocall(tmp_addr, size,
                                 (int)(node->si_flags & SGX_EMA_PAGE_TYPE_MASK),
                                 (int)alloc_flags);
    if (ret)
    {
        ret = EFAULT;
        return ret;
    }

    if (alloc_flags & SGX_EMA_COMMIT_NOW)
    {
        int grow_up = (alloc_flags & SGX_EMA_GROWSDOWN) ? 0 : 1;
        ret = do_commit(tmp_addr, size, node->si_flags, grow_up);
        if (ret)
        {
            return ret;
        }
    }

    if (alloc_flags & SGX_EMA_COMMIT_NOW)
        ret = ema_set_eaccept_full(node);
    else
        ret = ema_clear_eaccept_full(node);

    return ret;
}

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
#include "emalloc.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include "ema.h"     // SGX_PAGE_SIZE
#include "sgx_mm.h"  // sgx_mm_alloc
/*
 * This file implements a Simple allocator for EMM internal memory
 * It maintains a list of reserves,  dynamically added on
 * demand using sgx_mm_alloc recursively when reserve runs below
 * a threshold.
 */

/**
 * Emalloc uses a two-level reserve scheme for allocation.
 * The meta reserve statically defined here is only used to allocate memory
 * for EMAs tracking the "reserve" areas used by emalloc. This is to ensure
 * we always have memory for book keeping of the reserves. Memory for regular
 * EMAs are allocated out of those reserves.
 *
 * How manny regular EMAs we can allocate with the following meta reserve size?
 *
 * A regular or reserve EMA takes fixed 112 bytes of allocation for the
 * ema_t and bit_array structs, plus 16 bytes allocation for bit map itself
 * if the EMA size is 64 pages or less. Note that the 8-byte emalloc header is
 * included in above numbers. So each EMA needs 128 bytes for tracking a region
 * of 64 pages or less. Larger EMAs needs additional memory allocated for the bit
 * map only, and the smallest allocation increment allowed by emalloc is 8 bytes
 * which can be used to track up to 64 pages in the bit map. So the overhead
 * of EMAs incrememnts by 8 bytes for each 64-page increment of the size of the
 * region tracked by the EMA.
 *
 * Each reserve EMA is also surrounded by guard page regions above and below.
 * The total meta reserve consumption for each reserve EMA is calculated by:
 *       3 * 128 + floor((pages tracked in reserve EMA - 1) / 64) * 8
 * Reserve EMA size starts at 16 pages and doubles each time a new reserve is
 * added, capped at 2^28 (max_emalloc_size). Using a spreadsheet, we can
 * calculate the maximum total reserve possible is 1.75GB with 16 pages of meta
 * reserve for allocating EMAs tracking reserve areas.
 *
 * Number of regular EMAs can be calculated by:
 *       1.75 * 2^30 / (128 + floor((pages tracked in EMA - 1) / 64) * 8).
 * That is 14.7 million if each EMA covers 64 pages or less
 * (128 bytes reserve per EMA), tracking up to 3.8 T space, or 13.8 million if all
 * EMAs are of 65-128 pages (136 bytes reserve per EMA), tracking up to
 * 7.2 T space, and so on.
 *
 */
#define META_RESERVE_SIZE 0x10000ULL
static uint8_t meta_reserve[META_RESERVE_SIZE];
static size_t meta_used;
/**
 * initial reserve size
 */
#define initial_reserve_size 0x10000ULL

// this is enough for bit map of an 8T EMA
static const size_t max_emalloc_size = 0x10000000ULL;

/* Blocks of memory managed.
 * The allocator put these fields at the front
 * of the block when a memory block is freed
 * minimal allocation size is 8 bytes
 * 8 bytes of header is overhead
 */
typedef struct _block
{
    uint64_t header;  // size | alloc_mask
    union
    {
        char* payload[0];
        struct _block* next_prev[2]; /* used only when this block is free
                                      *  next_prev[0] points to next free
                                      *  block, next_prev[1] points to prev
                                      *  free block if this one is 16 bytes+
                                      */
    };
} block_t;

#define num_exact_list 0x100
size_t header_size = sizeof(uint64_t);
#define exact_match_increment 0x8
#define min_block_size        0x10  // include 8-byte header
static const size_t max_exact_size =
    min_block_size + exact_match_increment * (num_exact_list - 1);
static block_t* exact_block_list[num_exact_list];

// the least significant bit in block header
// 1 == allocated/in-use, 0 == free
static const uint64_t alloc_mask = 1ULL;
// block size align to 8 bytes
uint64_t size_mask = ~((uint64_t)(exact_match_increment - 1));
// We don't expect many large blocks
// Keep all the rest in one list
static block_t* large_block_list = NULL;

block_t* payload_to_block(void* p)
{
    return (block_t*)(((size_t)p) - header_size);
}

void* block_to_payload(block_t* b)
{
    return (void*)(b->payload);
}

bool is_alloced(block_t* b)
{
    return alloc_mask & b->header;
}

uint64_t block_size(block_t* b)
{
    return b->header & size_mask;
}

size_t block_end(block_t* b)
{
    return (size_t)(b) + block_size(b);
}
#ifndef NDEBUG
size_t num_free_blocks = 0;
#endif
/*
 * A reserve is a continuous block of
 * memory committed for emalloc purpose.
 */
typedef struct _mm_reserve
{
    size_t base;
    size_t size;
    size_t used;
    struct _mm_reserve* next;
} mm_reserve_t;

static mm_reserve_t* reserve_list = NULL;

static mm_reserve_t* find_used_in_reserve(size_t addr, size_t size)
{
    if (size == 0) return NULL;
    mm_reserve_t* r = reserve_list;
    while (r)
    {
        if (addr >= r->base && addr + size <= r->base + r->used) return r;
        r = r->next;
    }
    return NULL;
}

static size_t get_list_idx(size_t size)
{
    assert(size % exact_match_increment == 0);
    if (size < min_block_size) return 0;
    size_t list = (size - min_block_size) / exact_match_increment;
    assert(list < num_exact_list);
    return list;
}

static void remove_from_list(block_t* b, block_t** list_head)
{
    size_t bsize = block_size(b);
    if (b == *list_head)
    {
        *list_head = b->next_prev[0];
        if ((*list_head) && bsize > min_block_size)
            (*list_head)->next_prev[1] = NULL;
    }
    else
    {
        block_t* prev = NULL;
        if (bsize > min_block_size) prev = b->next_prev[1];
        block_t* next = b->next_prev[0];
        if (prev) prev->next_prev[0] = next;
        if (next) next->next_prev[1] = prev;
    }
}
static void remove_from_lists(block_t* b)
{
    size_t bsize = block_size(b);
    if (bsize > max_exact_size)
        remove_from_list(b, &large_block_list);
    else
    {
        size_t l = get_list_idx(bsize);
        remove_from_list(b, &exact_block_list[l]);
    }
}

static void prepend_to_list(block_t* b, block_t** head)
{
    b->next_prev[0] = *head;
    if ((*head) && block_size(*head) > min_block_size)
    {
        (*head)->next_prev[1] = b;
    }
    *head = b;
}

static void put_exact_block(block_t* b)
{
    size_t list = get_list_idx(block_size(b));
    prepend_to_list(b, &exact_block_list[list]);
#ifndef NDEBUG
    num_free_blocks++;
#endif
}

static block_t* neighbor_right(block_t* me)
{
    size_t end = block_end(me);
    mm_reserve_t* r1 = find_used_in_reserve((size_t)me, end);
    if (!r1) return NULL;
    if (end == r1->base + r1->used) return NULL;
    mm_reserve_t* r2 = find_used_in_reserve(end, block_size((block_t*)end));
    if (r1 != r2) return NULL;
    return (block_t*)end;
}

// Merge with left neighbor is not implemented
// which requires scanning or footer
// Most blocks are of similar sizes, benefits of full merge implementation
//  is probably not significant.
// Keep it simple for now.
static block_t* possibly_merge(block_t* b)
{
    block_t* nr = neighbor_right(b);
    while (nr && is_alloced(nr))
    {
        remove_from_lists(nr);
        b->header += block_size(nr);
#ifndef NDEBUG
        num_free_blocks--;
#endif
    }
    return b;
}

static void put_free_block(block_t* e)
{
    if (block_size(e) <= (size_t)max_exact_size)
    {
        put_exact_block(e);
        return;
    }
    prepend_to_list(e, &large_block_list);
#ifndef NDEBUG
    num_free_blocks++;
#endif
}

static block_t* split_free_block(block_t* b, size_t s)
{
    size_t remain = b->header - s;
    assert(remain >= (size_t)min_block_size);
    b->header = s;
    block_t* new_b = (block_t*)((uint8_t*)b + s);
    new_b->header = remain;
    return new_b;
}

static block_t* get_exact_match(size_t bsize)
{
    size_t list = get_list_idx(bsize);
    if (exact_block_list[list] == NULL) return NULL;
    block_t* ret = exact_block_list[list];
    exact_block_list[list] = ret->next_prev[0];
    if (list > 0 && exact_block_list[list])
        exact_block_list[list]->next_prev[1] = NULL;
#ifndef NDEBUG
    num_free_blocks--;
#endif
    return ret;
}

static block_t* get_free_block(size_t bsize)
{
    if (bsize <= max_exact_size) return get_exact_match(bsize);

    if (large_block_list == NULL) return NULL;

    block_t* tmp = large_block_list;
    block_t* best = NULL;

    // EMA objects are 80 bytes
    // Bit_arrays are mostly small except for really large EMAs
    // So number of large objects is likely small.
    // Simply loop over the free list and find the smallest block
    // that can meet the requested size.
    while (tmp != NULL)
    {
        if (tmp->header >= bsize)
        {
            if (!best)
            {
                best = tmp;
            }
            else if (best->header > tmp->header)
            {
                best = tmp;
            }
        }
        tmp = (block_t*)tmp->next_prev[0];
    }

    if (!best) return NULL;
    remove_from_list(best, &large_block_list);

    if (best->header >= (bsize + min_block_size))
    {
        block_t* tail = split_free_block(best, bsize);
        put_free_block(tail);
    }
#ifndef NDEBUG
    num_free_blocks--;
#endif
    return best;
}

static block_t* get_large_block_end_at(size_t addr)
{
    if (large_block_list == NULL) return NULL;
    block_t* tmp = large_block_list;

    while (tmp != NULL)
    {
        if ((((size_t)tmp) + tmp->header) == addr)
        {
            remove_from_list(tmp, &large_block_list);
            return tmp;
        }
        tmp = tmp->next_prev[0];
    }
    return NULL;
}

static void merge_large_blocks_to_reserve(mm_reserve_t* r)
{
    size_t used_end = r->base + r->used;
    block_t* merge = get_large_block_end_at(used_end);
    while (merge != NULL)
    {
#ifndef NDEBUG
        num_free_blocks--;
#endif
        used_end -= merge->header;
        merge = get_large_block_end_at(used_end);
    }
    r->used = used_end - r->base;
    return;
}

static void new_reserve(void* base, size_t rsize)
{
    mm_reserve_t* reserve = (mm_reserve_t*)base;
    size_t head_size = sizeof(mm_reserve_t);
    reserve->base = (size_t)(base) + head_size;
    reserve->used = 0;
    reserve->size = rsize - head_size;
    reserve->next = reserve_list;
    reserve_list = reserve;
}

static block_t* alloc_from_reserve(size_t bsize)
{
    mm_reserve_t* r = reserve_list;
    size_t ret = 0;
    while (r)
    {
        if (r->size - r->used >= bsize)
        {
            ret = r->base + r->used;
            r->used += bsize;
            break;
        }
        r = r->next;
    }
    return (block_t*)ret;
}

static bool adding_reserve = false;
static size_t reserve_size_increment = initial_reserve_size;
static const size_t guard_size = 0x8000ULL;

static int add_reserve(size_t rsize)
{
    void* base = NULL;
    if (adding_reserve) return 0;
    reserve_size_increment =
        reserve_size_increment > rsize ? reserve_size_increment : rsize;
    // this will call back to emalloc and efree.
    // set the flag to avoid infinite loop
    adding_reserve = true;
    int ret = sgx_mm_alloc(NULL, reserve_size_increment + 2 * guard_size,
                           SGX_EMA_RESERVE, NULL, NULL, &base);
    if (ret) goto out;
    ret =
        sgx_mm_alloc((void*)((size_t)base + guard_size), reserve_size_increment,
                     SGX_EMA_COMMIT_ON_DEMAND | SGX_EMA_FIXED, NULL, NULL, &base);
    if (ret) goto out;

    sgx_mm_commit(base, rsize);
    new_reserve(base, reserve_size_increment);
    reserve_size_increment = reserve_size_increment * 2;  // double next time
    if (reserve_size_increment > max_emalloc_size)
        reserve_size_increment = max_emalloc_size;
out:
    adding_reserve = false;
    return ret;
}

static void* alloc_from_meta(size_t bsize)
{
    assert(adding_reserve);
    if (meta_used + bsize > META_RESERVE_SIZE) return NULL;
    block_t* b = (block_t*)(&meta_reserve[meta_used]);
    meta_used += bsize;
    b->header = bsize | alloc_mask;
    return block_to_payload(b);
}

int emalloc_init_reserve(size_t init_size)
{
    if (add_reserve(init_size)) return ENOMEM;
    reserve_size_increment = initial_reserve_size;
    return 0;
}

// Single thread only.
// Caller holds mm_lock
void* emalloc(size_t size)
{
    size_t bsize = ROUND_TO(size + header_size, exact_match_increment);
    if (bsize < min_block_size) bsize = min_block_size;
    if (adding_reserve)  // called back from add_reserve
        return alloc_from_meta(bsize);

    block_t* b = get_free_block(bsize);

    if (b != NULL)
    {
        b->header = bsize | alloc_mask;
        return block_to_payload(b);
    }

    b = alloc_from_reserve(bsize);
    if (!b)
    {
        size_t new_reserve_size =
            ROUND_TO(bsize + sizeof(mm_reserve_t), initial_reserve_size);
        if (add_reserve(new_reserve_size)) return NULL;
        b = alloc_from_reserve(bsize);
        if (!b)  // should never happen
            return NULL;
    }

    b->header = bsize | alloc_mask;
    return block_to_payload(b);
}

static block_t* reconfigure_block(block_t* b)
{
    b->header = b->header & size_mask;
    b->next_prev[0] = NULL;
    if (b->header > min_block_size) b->next_prev[1] = NULL;

    b = possibly_merge(b);
    return b;
}

int can_erealloc(void* payload)
{
    block_t* b = payload_to_block(payload);
    size_t bstart = (size_t)b;
    size_t bsize = block_size(b);
    if (adding_reserve) return 1;
    else if (bstart < (size_t)(&meta_reserve[META_RESERVE_SIZE]) &&
        bstart + bsize > (size_t)(&meta_reserve[0]))
        return 0;
    else
        return 1;
}
/*
 * This is an internal interface only used
 *  by emm, intentionally crash for any error or
 *  inconsistency
 */
void efree(void* payload)
{
    block_t* b = payload_to_block(payload);
    size_t bstart = (size_t)b;
    size_t bsize = block_size(b);
    if (bstart < (size_t)(&meta_reserve[META_RESERVE_SIZE]) &&
        bstart + bsize > (size_t)(&meta_reserve[0]))
    {
        if (adding_reserve)
        {  // we don't expect a lot of free blocks allocated
            // in meta reserve. Do nothing now
            assert(bstart >= (size_t)(&meta_reserve[0]));
            assert(bstart + bsize <=
                   (size_t)(&meta_reserve[META_RESERVE_SIZE]));
            return;
        }
        else
            abort();
    }
    // normal blocks
    mm_reserve_t* r = find_used_in_reserve((size_t)b, block_size(b));
    if (!r) abort();
    b = reconfigure_block(b);
    size_t end = block_end(b);
    if ((end - r->base) == r->used)
    {
        r->used -= b->header;
        merge_large_blocks_to_reserve(r);
        return;
    }

    put_free_block(b);
    return;
}

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
#include <stdlib.h>
#include <string.h>

#include "bit_array_imp.h"
#include "emalloc.h"

#define NUM_OF_BYTES(nbits) (ROUND_TO((nbits), 8) >> 3)
#define TEST_BIT(A, p)      ((A)[((p) / 8)] & ((uint8_t)(1 << ((p) % 8))))
#define SET_BIT(A, p)       ((A)[((p) / 8)] |= ((uint8_t)(1 << ((p) % 8))))

// Create a new bit array to track the status of 'num' of bits.
// The contents of the data is uninitialized.
bit_array* bit_array_new(size_t num_of_bits)
{
    if (num_of_bits == 0) return NULL;

    if (ROUND_TO((num_of_bits), 8) < num_of_bits) return NULL;

    size_t n_bytes = NUM_OF_BYTES(num_of_bits);
    bit_array* ba = (bit_array*)emalloc(sizeof(bit_array));
    if (!ba) return NULL;
    ba->n_bytes = n_bytes;
    ba->n_bits = num_of_bits;
    ba->data = (uint8_t*)emalloc(n_bytes);
    if (!ba->data)
    {
        efree(ba);
        return NULL;
    }
    return ba;
}

// Create a new bit array to track the status of 'num' of bits.
// All the tracked bits are set (value 1).
bit_array* bit_array_new_set(size_t num_of_bits)
{
    bit_array* ba = bit_array_new(num_of_bits);
    if (!ba) return NULL;

    memset(ba->data, 0xFF, ba->n_bytes);
    return ba;
}

// Create a new bit array to track the status of 'num' of bits.
// All the tracked bits are reset (value 0).
bit_array* bit_array_new_reset(size_t num_of_bits)
{
    bit_array* ba = bit_array_new(num_of_bits);
    if (!ba) return NULL;

    memset(ba->data, 0, ba->n_bytes);
    return ba;
}

// Delete the bit_array 'ba' and the data it owns
void bit_array_delete(bit_array* ba)
{
    efree(ba->data);
    efree(ba);
}

// Returns whether the bit at position 'pos' is set
bool bit_array_test(bit_array* ba, size_t pos)
{
    return TEST_BIT(ba->data, pos);
}
uint8_t set_mask(size_t start, size_t bits_to_set)
{
    assert(start < 8);
    assert(bits_to_set <= 8);
    assert(start + bits_to_set <= 8);
    return (uint8_t)(((1 << bits_to_set) - 1) << start);
}
bool bit_array_test_range(bit_array* ba, size_t pos, size_t len)
{
    size_t byte_index = pos / 8;
    size_t bit_index = pos % 8;
    size_t bits_in_first_byte = 8 - bit_index;

    if (len <= bits_in_first_byte)
    {
        uint8_t mask = set_mask(bit_index, len);
        if ((ba->data[byte_index] & mask) != mask)
        {
            return false;
        }
        return true;
    }

    uint8_t mask = set_mask(bit_index, bits_in_first_byte);
    if ((ba->data[byte_index] & mask) != mask)
    {
        return false;
    }

    size_t bits_remain = len - bits_in_first_byte;
    while (bits_remain >= 8)
    {
        if (ba->data[++byte_index] != 0xFF)
        {
            return false;
        }
        bits_remain -= 8;
    }

    // handle last several bits
    if (bits_remain > 0)
    {
        mask = set_mask(0, bits_remain);
        if ((ba->data[++byte_index] & mask) != mask)
        {
            return false;
        }
    }

    return true;
}

bool bit_array_test_range_any(bit_array* ba, size_t pos, size_t len)
{
    size_t byte_index = pos / 8;
    size_t bit_index = pos % 8;
    size_t bits_in_first_byte = 8 - bit_index;

    if (len <= bits_in_first_byte)
    {
        uint8_t mask = set_mask(bit_index, len);
        if ((ba->data[byte_index] & mask))
        {
            return true;
        }
        return false;
    }

    uint8_t mask = set_mask(bit_index, bits_in_first_byte);
    if ((ba->data[byte_index] & mask))
    {
        return true;
    }

    size_t bits_remain = len - bits_in_first_byte;
    while (bits_remain >= 8)
    {
        if (ba->data[++byte_index])
        {
            return true;
        }
        bits_remain -= 8;
    }

    // handle last several bits
    if (bits_remain > 0)
    {
        mask = set_mask(0, bits_remain);
        if ((ba->data[++byte_index] & mask))
        {
            return true;
        }
    }
    return false;
}

// Set the bit at 'pos'
void bit_array_set(bit_array* ba, size_t pos)
{
    SET_BIT(ba->data, pos);
}

void bit_array_set_range(bit_array* ba, size_t pos, size_t len)
{
    size_t byte_index = pos / 8;
    size_t bit_index = pos % 8;
    size_t bits_in_first_byte = 8 - bit_index;

    if (len <= bits_in_first_byte)
    {
        uint8_t mask = set_mask(bit_index, len);
        ba->data[byte_index] |= mask;
        return;
    }

    uint8_t mask = set_mask(bit_index, bits_in_first_byte);
    ba->data[byte_index] |= mask;
    size_t bits_remain = len - bits_in_first_byte;
    while (bits_remain >= 8)
    {
        ba->data[++byte_index] = 0xFF;
        bits_remain -= 8;
    }

    // handle last several bits
    if (bits_remain > 0)
    {
        mask = set_mask(0, bits_remain);
        ba->data[++byte_index] |= mask;
    }

    return;
}

// Set all the bits
void bit_array_set_all(bit_array* ba)
{
    memset(ba->data, 0xFF, ba->n_bytes);
}

uint8_t clear_mask(size_t start, size_t bits_to_clear)
{
    return (uint8_t)(~set_mask(start, bits_to_clear));
}

void bit_array_reset_range(bit_array* ba, size_t pos, size_t len)
{
    size_t byte_index = pos / 8;
    size_t bit_index = pos % 8;
    size_t bits_in_first_byte = 8 - bit_index;

    if (len <= bits_in_first_byte)
    {
        uint8_t mask = clear_mask(bit_index, len);
        ba->data[byte_index] &= mask;
        return;
    }

    uint8_t mask = clear_mask(bit_index, bits_in_first_byte);
    ba->data[byte_index] &= mask;

    size_t bits_remain = len - bits_in_first_byte;
    while (bits_remain >= 8)
    {
        ba->data[++byte_index] = 0;
        bits_remain -= 8;
    }

    // handle last several bits
    if (bits_remain > 0)
    {
        mask = clear_mask(0, bits_remain);
        ba->data[++byte_index] &= mask;
    }

    return;
}

// Clear all the bits
void bit_array_reset_all(bit_array* ba)
{
    memset(ba->data, 0, ba->n_bytes);
}

// Reset the bit_array 'ba' to track the new 'data', which has 'num' of bits.
void bit_array_reattach(bit_array* ba, size_t num_of_bits, uint8_t* data)
{
    if (ba->data)
    {
        efree(ba->data);
    }

    size_t n_bytes = NUM_OF_BYTES(num_of_bits);
    ba->n_bytes = n_bytes;
    ba->n_bits = num_of_bits;
    ba->data = data;
}

// Split the bit array at 'pos'
int bit_array_split(bit_array* ba, size_t pos, bit_array** new_lower,
                    bit_array** new_higher)
{
    // not actually a split
    if (pos == 0)
    {
        *new_lower = NULL;
        *new_higher = ba;
        return 0;
    }

    // not actually a split
    if (pos >= ba->n_bits)
    {
        *new_lower = ba;
        *new_higher = NULL;
        return 0;
    }

    size_t byte_index = pos / 8;
    uint8_t bit_index = pos % 8;

    size_t l_bits = (byte_index << 3) + bit_index;
    size_t l_bytes = NUM_OF_BYTES(l_bits);
    size_t r_bits = ba->n_bits - l_bits;

    // new data for bit_array of lower pages
    uint8_t* data = (uint8_t*)emalloc(l_bytes);
    if (!data) return ENOMEM;
    size_t i;
    for (i = 0; i < byte_index; ++i)
    {
        data[i] = ba->data[i];
    }

    if (bit_index > 0)
    {
        uint8_t tmp = ba->data[i] & (uint8_t)((1 << bit_index) - 1);
        data[i] = tmp;
    }

    // new bit_array for higher pages
    bit_array* ba2 = bit_array_new(r_bits);
    if (!ba2)
    {
        efree(data);
        return ENOMEM;
    }

    size_t bits_remain = r_bits;
    size_t curr_byte = byte_index;
    size_t dst_byte = 0;
    uint8_t u1 = 0, u2 = 0;

    while (bits_remain >= 8)
    {
        u1 = (uint8_t)(ba->data[curr_byte++] >> bit_index);
        u2 = (uint8_t)(ba->data[curr_byte] << (8 - bit_index));
        ba2->data[dst_byte++] = u1 | u2;
        bits_remain -= 8;
    }

    if (bits_remain > (uint8_t)(8 - bit_index))
    {
        u1 = (uint8_t)(ba->data[curr_byte++] >> bit_index);
        u2 = (uint8_t)(ba->data[curr_byte] << (8 - bit_index));
        ba2->data[dst_byte] = u1 | u2;
        ;
    }
    else if (bits_remain > 0)
    {
        u1 = (uint8_t)(ba->data[curr_byte] >> bit_index);
        ba2->data[dst_byte] = u1;
    }

    bit_array_reattach(ba, l_bits, data);

    *new_lower = ba;
    *new_higher = ba2;
    return 0;
}

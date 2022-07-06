#ifndef SGX_BIT_ARRAY_IMP_H
#define SGX_BIT_ARRAY_IMP_H

#include <stdint.h>

#include "bit_array.h"

struct bit_array_
{
    size_t n_bytes;
    size_t n_bits;
    uint8_t* data;
};

#endif

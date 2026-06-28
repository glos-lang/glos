#ifndef INT128_H
#define INT128_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t low;
    uint64_t high;
} Int128;

Int128 int128_from_i64(int64_t n);
Int128 int128_from_u64(uint64_t n);

Int128 int128_add(Int128 a, Int128 b, bool is_signed);
Int128 int128_sub(Int128 a, Int128 b, bool is_signed);
Int128 int128_mul(Int128 a, Int128 b, bool is_signed);
Int128 int128_div(Int128 a, Int128 b, bool is_signed);
Int128 int128_mod(Int128 a, Int128 b, bool is_signed);
Int128 int128_neg(Int128 x);

void int128_divmod(Int128 a, Int128 b, bool is_signed, Int128 *out_quotient, Int128 *out_remainder);

Int128 int128_shl(Int128 x, unsigned shift, bool is_signed);
Int128 int128_shr(Int128 x, unsigned shift, bool is_signed);
Int128 int128_and(Int128 a, Int128 b, bool is_signed);
Int128 int128_or(Int128 a, Int128 b, bool is_signed);
Int128 int128_not(Int128 x);

bool int128_eq(Int128 a, Int128 b, bool is_signed);
bool int128_ne(Int128 a, Int128 b, bool is_signed);
bool int128_lt(Int128 a, Int128 b, bool is_signed);
bool int128_le(Int128 a, Int128 b, bool is_signed);
bool int128_gt(Int128 a, Int128 b, bool is_signed);
bool int128_ge(Int128 a, Int128 b, bool is_signed);

const char *int128_to_cstr(Int128 n);

#endif // INT128_H

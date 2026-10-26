#ifndef UNICODE_H
#define UNICODE_H

#include "basic.h"

typedef int32_t Rune;

Rune read_rune_from_sv(SV *sv);
void sb_push_rune(SB *sb, Rune rune);
void print_rune(FILE *f, Rune rune);
void print_sv_safe(FILE *f, SV sv);

bool rune_is_digit(Rune r);
bool rune_is_lower(Rune r);
bool rune_is_upper(Rune r);
bool rune_is_alpha(Rune r);
bool rune_is_number(Rune r);

#endif // UNICODE_H

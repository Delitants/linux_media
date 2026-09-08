/* SPDX-License-Identifier: GPL-2.0-only */
/* Reproduce the Linux LP64 uint64_t alias even on a long-long host. */
#define uint64_t host_native_uint64_t
#include <stdint.h>
#undef uint64_t
typedef unsigned long uint64_t;
_Static_assert(sizeof(uint64_t) == 8, "this fixture requires LP64");

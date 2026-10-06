// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef KL_RT_H
#define KL_RT_H

#include <stddef.h>
#include <stdint.h>

void*          kl_rt_alloc( size_t size );
void*          kl_rt_alloc_many( size_t count, size_t size );
void           kl_rt_free( void* ptr );
_Noreturn void kl_rt_panic( const char* file, uint32_t line, const char* message );

#endif // KL_RT_H

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
_Noreturn void kl_rt_panic_message( const char* file, uint32_t line, const uint8_t* data, uint64_t size );
_Noreturn void kl_rt_unreachable( void );
void           kl_rt_write( int32_t, const uint8_t*, uint64_t );
void           kl_rt_write_i64( int32_t, int64_t );
void           kl_rt_write_u64( int32_t, uint64_t );
void           kl_rt_write_f64( int32_t, double );

#endif // KL_RT_H

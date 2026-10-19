// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef KL_RT_H
#define KL_RT_H

#include <stddef.h>
#include <stdint.h>

enum kl_rt_kind
{
    KL_RT_OTHER         = 0,
    KL_RT_NOT_FOUND     = 1,
    KL_RT_DENIED        = 2,
    KL_RT_EXISTS        = 3,
    KL_RT_IS_DIRECTORY  = 4,
    KL_RT_NOT_DIRECTORY = 5,
    KL_RT_INTERRUPTED   = 6,
    KL_RT_OUT_OF_MEMORY = 7,
    KL_RT_NOT_EMPTY     = 8
};

void*          kl_rt_alloc( size_t size, const char* file, uint32_t line, const char* message );
void*          kl_rt_alloc_many( size_t count, size_t size, const char* file, uint32_t line, const char* message );
void           kl_rt_free( void* ptr );
_Noreturn void kl_rt_panic( const char* file, uint32_t line, const char* message );
_Noreturn void kl_rt_panic_message( const char* file, uint32_t line, const uint8_t* data, uint64_t size );
_Noreturn void kl_rt_panic_at( const uint8_t* file, uint64_t file_size, uint32_t line, const uint8_t* data, uint64_t size );
_Noreturn void kl_rt_unreachable( void );
void           kl_rt_write( int32_t, const uint8_t*, uint64_t );
void           kl_rt_write_i64( int32_t, int64_t );
void           kl_rt_write_u64( int32_t, uint64_t );
void           kl_rt_write_f64( int32_t, double );
void           kl_rt_hash_key( uint64_t* low, uint64_t* high );
int32_t        kl_rt_file_open( const uint8_t* path, uint64_t size, int32_t mode, int32_t* fd );
int32_t        kl_rt_file_read( int32_t fd, uint8_t* data, uint64_t size, uint64_t* count );
int32_t        kl_rt_file_write( int32_t fd, const uint8_t* data, uint64_t size );
int32_t        kl_rt_file_close( int32_t fd );
int32_t        kl_rt_error_kind( int32_t code );
int32_t        kl_rt_input_fill( uint8_t** data, uint64_t* size );
void           kl_rt_input_take( uint64_t count );
int32_t        kl_rt_path_kind( const uint8_t* path, uint64_t size, int32_t* kind );
int32_t        kl_rt_create_directory( const uint8_t* path, uint64_t size );
int32_t        kl_rt_delete_directory( const uint8_t* path, uint64_t size );
int32_t        kl_rt_delete_file( const uint8_t* path, uint64_t size );
int32_t        kl_rt_directory_open( const uint8_t* path, uint64_t size, uint64_t* handle );
int32_t        kl_rt_directory_next( uint64_t handle, uint8_t** name, uint64_t* size );
void           kl_rt_directory_close( uint64_t handle );
int32_t        kl_rt_process_start( const uint8_t* packed, uint64_t size, uint64_t count, int64_t* pid );
int32_t        kl_rt_process_wait( int64_t pid, int32_t* status );
int32_t        kl_rt_clock( int32_t which, int64_t* nanoseconds );
int32_t        kl_rt_clock_resolution( int32_t which, int64_t* nanoseconds );
int32_t        kl_rt_environment( const uint8_t* name, uint64_t size, uint8_t** value, uint64_t* value_size );

#endif // KL_RT_H

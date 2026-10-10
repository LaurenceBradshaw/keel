// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "kl_rt.h"
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void* kl_rt_alloc( size_t size, const char* file, uint32_t line, const char* message )
{
    // A zero request allocates one byte rather than asking malloc, whose answer for 0 is
    // implementation-defined and may be NULL - which is how failure is reported. Kept deliberately
    // after §12's empty-aggregate rejection made it unreachable: pinning the behaviour costs one
    // branch, and leaving it to the C implementation costs a case nobody can test.
    if( size == 0 )
    {
        size = 1;
    }

    void* ptr = malloc( size );
    if( ptr == NULL )
    {
        kl_rt_panic( file, line, message );
    }

    return ptr;
}

void* kl_rt_alloc_many( size_t count, size_t size, const char* file, uint32_t line, const char* message )
{
    if( size != 0 && count > SIZE_MAX / size )
    {
        kl_rt_panic( file, line, message );
    }

    return kl_rt_alloc( count * size, file, line, message );
}

void kl_rt_free( void* ptr )
{
    // free( NULL ) must be a no-op - C guarantees it
    free( ptr );
}

_Noreturn void kl_rt_panic( const char* file, uint32_t line, const char* message )
{
    fflush( stdout );
    fprintf( stderr, "%s:%u: %s\n", file, (unsigned) line, message );
    abort();
}

_Noreturn void kl_rt_panic_message( const char* file, uint32_t line, const uint8_t* data, uint64_t size )
{
    fflush( stdout );
    fprintf( stderr, "%s:%u: panic: ", file, (unsigned) line );
    fwrite( data, 1, size, stderr );
    fputc( '\n', stderr );
    abort();
}

// Reached only if a `never` function returned, which its body check refuses.
_Noreturn void kl_rt_unreachable( void )
{
    fflush( stdout );
    fputs( "a `never` call returned\n", stderr );
    abort();
}

static FILE* kl_rt_stream( int32_t stream )
{
    if( stream == 2 )
    {
        fflush( stdout );
        return stderr;
    }

    return stdout;
}

void kl_rt_write( int32_t stream, const uint8_t* data, uint64_t size )
{
    FILE* out = kl_rt_stream( stream );

    fwrite( data, 1, size, out );
}

void kl_rt_write_i64( int32_t stream, int64_t value )
{
    FILE* out = kl_rt_stream( stream );

    fprintf( out, "%" PRId64, value );
}

void kl_rt_write_u64( int32_t stream, uint64_t value )
{
    FILE* out = kl_rt_stream( stream );

    fprintf( out, "%" PRIu64, value );
}

void kl_rt_write_f64( int32_t stream, double value )
{
    FILE* out = kl_rt_stream( stream );

    if( isnan( value ) )
    {
        fprintf( out, "nan" );
        return;
    }

    char buf[32];
    for( int p = 1; p <= 17; p++ )
    {
        snprintf( buf, sizeof buf, "%.*g", p, value );

        const double parsed = strtod( buf, NULL );
        if( parsed == value )
        {
            break;
        }
    }

    const char* digits        = buf + ( buf[0] == '-' );
    const bool  needs_decimal = digits[strspn( digits, "0123456789" )] == '\0';

    fputs( buf, out );
    if( needs_decimal )
    {
        fputs( ".0", out );
    }
}

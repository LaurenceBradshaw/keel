// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The runtime's edge cases. Each runs in a child whose stdout and stderr are pipes, so a panic's
// output and its abort can be checked too.

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "kl_rt.h"

typedef struct
{
    char out[256];
    char err[256];
    int  status;
} Run;

static int failures = 0;

static void read_all( int fd, char* buf, size_t size )
{
    size_t used = 0;
    while( used + 1 < size )
    {
        const ssize_t got = read( fd, buf + used, size - 1 - used );
        if( got <= 0 )
        {
            break;
        }
        used += (size_t) got;
    }
    buf[used] = '\0';
}

static Run run( void ( *body )( void ) )
{
    Run r;
    memset( &r, 0, sizeof r );

    int out[2];
    int err[2];
    if( pipe( out ) != 0 || pipe( err ) != 0 )
    {
        perror( "pipe" );
        exit( 2 );
    }

    fflush( stdout );
    fflush( stderr );
    const pid_t pid = fork();
    if( pid < 0 )
    {
        perror( "fork" );
        exit( 2 );
    }

    if( pid == 0 )
    {
        dup2( out[1], 1 );
        dup2( err[1], 2 );
        close( out[0] );
        close( out[1] );
        close( err[0] );
        close( err[1] );
        body();
        exit( 0 );
    }

    close( out[1] );
    close( err[1] );
    read_all( out[0], r.out, sizeof r.out );
    read_all( err[0], r.err, sizeof r.err );
    close( out[0] );
    close( err[0] );
    waitpid( pid, &r.status, 0 );
    return r;
}

static void check( const char* test, const char* what, const char* got, const char* want )
{
    if( strcmp( got, want ) != 0 )
    {
        fprintf( stderr, "%s: %s was \"%s\", expected \"%s\"\n", test, what, got, want );
        failures++;
    }
}

static void check_exit( const char* test, const Run* r, bool aborted )
{
    const bool ok = aborted ? WIFSIGNALED( r->status ) && WTERMSIG( r->status ) == SIGABRT
                            : WIFEXITED( r->status ) && WEXITSTATUS( r->status ) == 0;
    if( !ok )
    {
        fprintf( stderr, "%s: expected the child to %s\n", test, aborted ? "abort" : "exit 0" );
        failures++;
    }
}

static void lowest_i64( void )
{
    kl_rt_write_i64( 1, INT64_MIN );
}

static void largest_u64( void )
{
    kl_rt_write_u64( 1, UINT64_MAX );
}

// The message is a count of bytes, not a C string, and what was printed before shows first.
static void panic_message( void )
{
    kl_rt_write( 1, (const uint8_t*) "before", 6 );
    kl_rt_panic_message( "main.kl", 7, (const uint8_t*) "emptyXYZ", 5 );
}

int main( void )
{
    Run r = run( lowest_i64 );
    check( "lowest_i64", "stdout", r.out, "-9223372036854775808" );
    check_exit( "lowest_i64", &r, false );

    r = run( largest_u64 );
    check( "largest_u64", "stdout", r.out, "18446744073709551615" );
    check_exit( "largest_u64", &r, false );

    r = run( panic_message );
    check( "panic_message", "stdout", r.out, "before" );
    check( "panic_message", "stderr", r.err, "main.kl:7: panic: empty\n" );
    check_exit( "panic_message", &r, true );

    if( failures != 0 )
    {
        fprintf( stderr, "%d check%s failed\n", failures, failures == 1 ? "" : "s" );
        return 1;
    }

    printf( "3 runtime tests passed\n" );
    return 0;
}

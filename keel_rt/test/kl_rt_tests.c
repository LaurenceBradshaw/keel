// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The runtime's edge cases. Each runs in a child whose stdout and stderr are pipes, so a panic's
// output and its abort can be checked too.

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
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

// Prints one key, so two children can be compared: each process draws its own.
static void hash_key( void )
{
    uint64_t low  = 0;
    uint64_t high = 0;
    kl_rt_hash_key( &low, &high );
    printf( "%016llx%016llx", (unsigned long long) low, (unsigned long long) high );
}

// Within one process, each key's low half is one more than the last, and its high half the same.
static void hash_key_varies( void )
{
    uint64_t low[2]  = { 0, 0 };
    uint64_t high[2] = { 0, 0 };
    kl_rt_hash_key( &low[0], &high[0] );
    kl_rt_hash_key( &low[1], &high[1] );
    printf( "%s", low[1] == low[0] + 1 && high[1] == high[0] ? "varies" : "same" );
}

// A path holding a 0 byte, and a mode out of range, are refused before `open` runs.
static void file_open_refusals( void )
{
    int32_t fd = -1;
    printf( "%d %d ", kl_rt_file_open( (const uint8_t*) "a\0b", 3, 0, &fd ), fd );
    printf( "%d %d", kl_rt_file_open( (const uint8_t*) "a", 1, 4, &fd ), fd );
}

// Bytes written through a pipe come back in pieces, then the end of the file as a count of 0.
static void file_round_trip( void )
{
    int ends[2];
    if( pipe( ends ) != 0 )
    {
        exit( 2 );
    }

    const int32_t wrote  = kl_rt_file_write( ends[1], (const uint8_t*) "hello", 5 );
    const int32_t closed = kl_rt_file_close( ends[1] );

    uint8_t       buf[3];
    uint64_t      first = 9, second = 9, end = 9;
    const int32_t a = kl_rt_file_read( ends[0], buf, 3, &first );
    const int32_t b = kl_rt_file_read( ends[0], buf, 3, &second );
    const int32_t c = kl_rt_file_read( ends[0], buf, 3, &end );
    printf(
        "%d %d %d %d %d %llu %llu %llu",
        wrote,
        closed,
        a,
        b,
        c,
        (unsigned long long) first,
        (unsigned long long) second,
        (unsigned long long) end
    );
}

// A descriptor that isn't open is the system's own `EBADF`, for each call.
static void file_bad_descriptor( void )
{
    uint8_t  buf[1];
    uint64_t count = 0;
    printf(
        "%d %d %d",
        kl_rt_file_read( -1, buf, 1, &count ) == EBADF,
        kl_rt_file_write( -1, buf, 1 ) == EBADF,
        kl_rt_file_close( -1 ) == EBADF
    );
}

// Each mapped `errno` value, and one that is not.
static void error_kinds( void )
{
    const int codes[] = { ENOENT, EACCES, EPERM, EEXIST, EISDIR, ENOTDIR, EINTR, ENOMEM, ENOTEMPTY, EINVAL };
    for( size_t i = 0; i < sizeof codes / sizeof codes[0]; i++ )
    {
        printf( "%d", kl_rt_error_kind( codes[i] ) );
    }
}

// Puts `text` on standard input as a pipe already closed, so its end is the input's end.
static void pipe_stdin( const char* text )
{
    int ends[2];
    if( pipe( ends ) != 0 || write( ends[1], text, strlen( text ) ) != (ssize_t) strlen( text ) )
    {
        exit( 2 );
    }
    close( ends[1] );
    dup2( ends[0], 0 );
    close( ends[0] );
}

// What one read brings in is handed out until taken, with no read in between, then 0 at the end.
static void input_fill_take( void )
{
    pipe_stdin( "ab\ncd" );

    uint8_t* data = NULL;
    uint64_t a = 9, b = 9, c = 9;
    kl_rt_input_fill( &data, &a );
    kl_rt_input_take( 3 );
    kl_rt_input_fill( &data, &b );
    printf( "%llu %llu %.2s ", (unsigned long long) a, (unsigned long long) b, (const char*) data );
    kl_rt_input_take( 2 );
    printf( "%d ", kl_rt_input_fill( &data, &c ) );
    printf( "%llu", (unsigned long long) c );
}

// Reading standard input flushes a prompt first; a raw write after it lands after it.
static void input_flushes_stdout( void )
{
    pipe_stdin( "x" );

    uint8_t* data = NULL;
    uint64_t size = 0;
    kl_rt_write( 1, (const uint8_t*) "prompt", 6 );
    kl_rt_input_fill( &data, &size );
    if( write( 1, "|after", 6 ) != 6 )
    {
        exit( 2 );
    }
}

// A file write to stdout goes after what `kl_rt_write` buffered, and is out before it returns.
static void file_write_keeps_order( void )
{
    kl_rt_write( 1, (const uint8_t*) "a", 1 );
    kl_rt_file_write( 1, (const uint8_t*) "b", 1 );
    if( write( 1, "c", 1 ) != 1 )
    {
        exit( 2 );
    }
}

// Each answer of `kl_rt_path_kind`, inside a fresh directory: missing covers `ENOTDIR` too.
static void path_kinds( void )
{
    char dir[] = "/tmp/kl_rt_XXXXXX";
    if( mkdtemp( dir ) == NULL || chdir( dir ) != 0 )
    {
        exit( 2 );
    }

    int32_t fd = -1;
    if( kl_rt_file_open( (const uint8_t*) "a", 1, 1, &fd ) != 0 || kl_rt_file_close( fd ) != 0 )
    {
        exit( 2 );
    }

    const char* paths[] = { ".", "a", "a/b", "none", "/dev/null" };
    for( size_t i = 0; i < sizeof paths / sizeof paths[0]; i++ )
    {
        int32_t       kind   = 9;
        const int32_t status = kl_rt_path_kind( (const uint8_t*) paths[i], strlen( paths[i] ), &kind );
        printf( "%d%d ", status, kind );
    }

    int32_t kind = 9;
    printf( "%d %d", kl_rt_path_kind( (const uint8_t*) "a\0b", 3, &kind ), kind );

    unlink( "a" );
    rmdir( dir );
}

// A directory made, refused twice, listed, each delete refusing the other kind, then emptied.
static void directory_round_trip( void )
{
    char dir[] = "/tmp/kl_rt_XXXXXX";
    if( mkdtemp( dir ) == NULL || chdir( dir ) != 0 )
    {
        exit( 2 );
    }

    const uint8_t* d = (const uint8_t*) "d";
    printf( "%d ", kl_rt_create_directory( d, 1 ) );
    printf( "%d ", kl_rt_create_directory( d, 1 ) == EEXIST );

    int32_t fd = -1;
    if( kl_rt_file_open( (const uint8_t*) "d/x", 3, 1, &fd ) != 0 || kl_rt_file_close( fd ) != 0 )
    {
        exit( 2 );
    }

    uint64_t handle = 0;
    printf( "%d ", kl_rt_directory_open( d, 1, &handle ) );

    uint8_t* name = NULL;
    uint64_t size = 9;
    printf( "%d ", kl_rt_directory_next( handle, &name, &size ) );
    printf( "%.*s ", (int) size, (const char*) name );
    printf( "%d %llu ", kl_rt_directory_next( handle, &name, &size ), (unsigned long long) size );
    kl_rt_directory_close( handle );

    const uint8_t* x = (const uint8_t*) "d/x";
    printf( "%d ", kl_rt_delete_directory( d, 1 ) == ENOTEMPTY );
    printf( "%d ", kl_rt_delete_file( d, 1 ) == EISDIR );
    printf( "%d ", kl_rt_delete_directory( x, 3 ) == ENOTDIR );
    printf( "%d ", kl_rt_delete_file( x, 3 ) );
    printf( "%d ", kl_rt_delete_file( x, 3 ) == ENOENT );
    printf( "%d ", kl_rt_delete_directory( d, 1 ) );
    printf( "%d ", kl_rt_delete_directory( d, 1 ) == ENOENT );
    printf( "%d", kl_rt_directory_open( d, 1, &handle ) == ENOENT );

    rmdir( dir );
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

    const Run first  = run( hash_key );
    const Run second = run( hash_key );
    check_exit( "hash_key", &first, false );

    if( strlen( first.out ) != 32 || strcmp( first.out, second.out ) == 0 )
    {
        fprintf( stderr, "hash_key: two processes drew \"%s\" and \"%s\"\n", first.out, second.out );
        failures++;
    }

    r = run( hash_key_varies );
    check( "hash_key_varies", "stdout", r.out, "varies" );
    check_exit( "hash_key_varies", &r, false );

    r = run( file_open_refusals );
    check( "file_open_refusals", "stdout", r.out, "22 -1 22 -1" );
    check_exit( "file_open_refusals", &r, false );

    r = run( file_round_trip );
    check( "file_round_trip", "stdout", r.out, "0 0 0 0 0 3 2 0" );
    check_exit( "file_round_trip", &r, false );

    r = run( file_bad_descriptor );
    check( "file_bad_descriptor", "stdout", r.out, "1 1 1" );
    check_exit( "file_bad_descriptor", &r, false );

    r = run( error_kinds );
    check( "error_kinds", "stdout", r.out, "1223456780" );
    check_exit( "error_kinds", &r, false );

    r = run( input_fill_take );
    check( "input_fill_take", "stdout", r.out, "5 2 cd 0 0" );
    check_exit( "input_fill_take", &r, false );

    r = run( input_flushes_stdout );
    check( "input_flushes_stdout", "stdout", r.out, "prompt|after" );
    check_exit( "input_flushes_stdout", &r, false );

    r = run( file_write_keeps_order );
    check( "file_write_keeps_order", "stdout", r.out, "abc" );
    check_exit( "file_write_keeps_order", &r, false );

    r = run( path_kinds );
    check( "path_kinds", "stdout", r.out, "02 01 00 00 03 22 9" );
    check_exit( "path_kinds", &r, false );

    r = run( directory_round_trip );
    check( "directory_round_trip", "stdout", r.out, "0 1 0 0 x 0 0 1 1 1 0 1 0 1 1" );
    check_exit( "directory_round_trip", &r, false );

    if( failures != 0 )
    {
        fprintf( stderr, "%d check%s failed\n", failures, failures == 1 ? "" : "s" );
        return 1;
    }

    printf( "14 runtime tests passed\n" );
    return 0;
}

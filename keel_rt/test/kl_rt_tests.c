// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The runtime's edge cases. Each runs in a child, this binary started again with the case's name,
// its stdout and stderr one pipe, so a panic's output and its exit can be checked too.

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kl_rt.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#define os_close _close
#define os_dup2 _dup2
#define os_getcwd _getcwd
#define os_write( fd, data, size ) _write( fd, data, (unsigned) ( size ) )
#define os_chdir _chdir
#define os_rmdir _rmdir
#define os_unlink _unlink
#define PROGRAM_NAME "\\keel_rt_tests.exe"
#define NULL_DEVICE "NUL"
#else
#include <sys/wait.h>
#include <unistd.h>
#define os_close close
#define os_dup2 dup2
#define os_getcwd getcwd
#define os_write( fd, data, size ) write( fd, data, size )
#define os_chdir chdir
#define os_rmdir rmdir
#define os_unlink unlink
#define PROGRAM_NAME "/keel_rt_tests"
#define NULL_DEVICE "/dev/null"
#endif

static int os_pipe( int ends[2] )
{
#ifdef _WIN32
    return _pipe( ends, 4096, _O_BINARY );
#else
    return pipe( ends );
#endif
}

static void os_setenv( const char* name, const char* value )
{
#ifdef _WIN32
    SetEnvironmentVariableA( name, value ); // the UCRT's _putenv_s takes "" as unset
#else
    setenv( name, value, 1 );
#endif
}

// Makes a fresh directory under the system's temporary one, and enters it.
static void enter_scratch( char* dir, size_t size )
{
#ifdef _WIN32
    char base[MAX_PATH];
    if( GetTempPathA( sizeof base, base ) == 0 ||
        snprintf( dir, size, "%skl_rt_%lu_%lu", base, GetCurrentProcessId(), GetTickCount() ) <= 0 || _mkdir( dir ) != 0 ||
        _chdir( dir ) != 0 )
    {
        exit( 2 );
    }
#else
    snprintf( dir, size, "/tmp/kl_rt_XXXXXX" );
    if( mkdtemp( dir ) == NULL || chdir( dir ) != 0 )
    {
        exit( 2 );
    }
#endif
}

// Leaves the directory and removes it, which Windows refuses while it is the current one.
static void leave_scratch( const char* dir )
{
    if( os_chdir( ".." ) != 0 || os_rmdir( dir ) != 0 )
    {
        exit( 2 );
    }
}

// This binary's path, as packed arguments start.
static char     self[4096];
static uint64_t self_size = 0;

// This binary and `argument`, packed; answers the size.
static uint64_t packed_self( char* out, const char* argument )
{
    memcpy( out, self, self_size );
    out[self_size] = '\0';
    strcpy( out + self_size + 1, argument );
    return self_size + 1 + strlen( argument ) + 1;
}

typedef struct
{
    char    out[256];
    int32_t status;
} Run;

static int failures = 0;

static void read_all( int32_t fd, char* buf, size_t size )
{
    size_t   used  = 0;
    uint64_t count = 0;
    while( used + 1 < size && kl_rt_file_read( fd, (uint8_t*) buf + used, size - 1 - used, &count ) == 0 && count != 0 )
    {
        used += count;
    }
    buf[used] = '\0';
}

static Run run( const char* name )
{
    Run r;
    memset( &r, 0, sizeof r );

    char           packed[sizeof self + 64];
    const uint64_t size = packed_self( packed, name );
    int64_t        pid  = 0;
    int32_t        fd   = -1;
    if( kl_rt_process_capture( (const uint8_t*) packed, size, 2, &pid, &fd ) != 0 )
    {
        fprintf( stderr, "%s: cannot start the child\n", name );
        exit( 2 );
    }

    read_all( fd, r.out, sizeof r.out );
    kl_rt_file_close( fd );
    if( kl_rt_process_wait( pid, &r.status ) != 0 )
    {
        fprintf( stderr, "%s: cannot wait for the child\n", name );
        exit( 2 );
    }
    return r;
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
    kl_rt_write( 1, (const uint8_t*) "before|", 7 );
    kl_rt_panic_message( "main.kl", 7, (const uint8_t*) "emptyXYZ", 5 );
}

static void panic_at( void )
{
    kl_rt_panic_at( (const uint8_t*) "caller.klXYZ", 9, 12, (const uint8_t*) "emptyXYZ", 5 );
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
    int32_t       fd   = -1;
    const int32_t zero = kl_rt_file_open( (const uint8_t*) "a\0b", 3, 0, &fd ); // before `fd` is read
    printf( "%d %d ", zero, fd );
    const int32_t mode = kl_rt_file_open( (const uint8_t*) "a", 1, 4, &fd );
    printf( "%d %d", mode, fd );
}

// Bytes written through a pipe come back in pieces, then the end of the file as a count of 0.
static void file_round_trip( void )
{
    int ends[2];
    if( os_pipe( ends ) != 0 )
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
    if( os_pipe( ends ) != 0 || os_write( ends[1], text, strlen( text ) ) != (int) strlen( text ) )
    {
        exit( 2 );
    }
    os_close( ends[1] );
    os_dup2( ends[0], 0 );
    os_close( ends[0] );
}

// Reading standard input flushes a prompt first; a raw write after it lands after it.
static void input_flushes_stdout( void )
{
    pipe_stdin( "x" );

    uint8_t  data[1];
    uint64_t size = 0;
    kl_rt_write( 1, (const uint8_t*) "prompt", 6 );
    kl_rt_file_read( 0, data, sizeof data, &size );
    if( os_write( 1, "|after", 6 ) != 6 )
    {
        exit( 2 );
    }
}

// A file write to stdout goes after what `kl_rt_write` buffered, and is out before it returns.
static void file_write_keeps_order( void )
{
    kl_rt_write( 1, (const uint8_t*) "a", 1 );
    kl_rt_file_write( 1, (const uint8_t*) "b", 1 );
    if( os_write( 1, "c", 1 ) != 1 )
    {
        exit( 2 );
    }
}

// Each answer of `kl_rt_path_kind`, inside a fresh directory: missing covers `ENOTDIR` too.
static void path_kinds( void )
{
    char dir[512];
    enter_scratch( dir, sizeof dir );

    int32_t fd = -1;
    if( kl_rt_file_open( (const uint8_t*) "a", 1, 1, &fd ) != 0 || kl_rt_file_close( fd ) != 0 )
    {
        exit( 2 );
    }

    const char* paths[] = { ".", "a", "a/b", "none", NULL_DEVICE };
    for( size_t i = 0; i < sizeof paths / sizeof paths[0]; i++ )
    {
        int32_t       kind   = 9;
        const int32_t status = kl_rt_path_kind( (const uint8_t*) paths[i], strlen( paths[i] ), &kind );
        printf( "%d%d ", status, kind );
    }

    int32_t kind = 9;
    printf( "%d %d", kl_rt_path_kind( (const uint8_t*) "a\0b", 3, &kind ), kind );

    os_unlink( "a" );
    leave_scratch( dir );
}

// A directory made, refused twice, listed, each delete refusing the other kind, then emptied.
static void directory_round_trip( void )
{
    char dir[512];
    enter_scratch( dir, sizeof dir );

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
    const int32_t last = kl_rt_directory_next( handle, &name, &size ); // before `size` is read
    printf( "%d %llu ", last, (unsigned long long) size );
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

    leave_scratch( dir );
}

// Programs for the process cases: this binary again, printing or exiting 3.
static void child_prints( void )
{
    printf( "child" );
}

static void child_exits( void )
{
    fflush( stdout );
    exit( 3 );
}

static void child_panics( void )
{
    kl_rt_write( 1, (const uint8_t*) "out|", 4 );
    kl_rt_panic_message( "test.kl", 1, (const uint8_t*) "x", 1 );
}

// Its bytes and size, the size leaving out the 0 byte C adds to every literal.
#define PACKED( text ) (const uint8_t*) text, sizeof text - 1

// A process started and waited for, a missing program refused and reaped, each malformed `packed`
// refused, and the parent's output flushed before the child's.
static void process_round_trip( void )
{
    int64_t pid    = 0;
    int32_t status = -1;
    char    packed[sizeof self + 64];

    printf( "before|" );
    uint64_t size = packed_self( packed, "child_prints" );
    printf( "%d ", kl_rt_process_start( (const uint8_t*) packed, size, 2, &pid, NULL ) );
    const int32_t waited = kl_rt_process_wait( pid, &status ); // before `status` is read
    printf( "%d %d ", waited, status );

    size = packed_self( packed, "child_exits" );
    kl_rt_process_start( (const uint8_t*) packed, size, 2, &pid, NULL );
    kl_rt_process_wait( pid, &status );
    printf( "%d ", status );

#ifndef _WIN32
    kl_rt_process_start( PACKED( "sh\0-c\0kill -9 $$\0" ), 3, &pid, NULL );
    kl_rt_process_wait( pid, &status );
    printf( "%d ", status );
#endif

    pid                = 7;
    const bool missing = kl_rt_process_start( PACKED( "kl-rt-no-such-program\0" ), 1, &pid, NULL ) == ENOENT;
    printf( "%d %d ", missing, (int) pid );

    // Miscounted, unterminated, an argument holding a 0 byte, and nothing at all.
    printf( "%d ", kl_rt_process_start( PACKED( "true\0" ), 2, &pid, NULL ) == EINVAL );
    printf( "%d ", kl_rt_process_start( PACKED( "true" ), 1, &pid, NULL ) == EINVAL );
    printf( "%d ", kl_rt_process_start( PACKED( "true\0a\0b\0" ), 2, &pid, NULL ) == EINVAL );
    printf( "%d", kl_rt_process_start( PACKED( "" ), 0, &pid, NULL ) == EINVAL );

#ifndef _WIN32
    // Every child was reaped, the failed one included.
    printf( " %d", waitpid( -1, NULL, WNOHANG ) == -1 && errno == ECHILD );
#endif
    printf( "|after" );
}

// A child's stdout read back through the fd it answers, and the read end left open for the caller
// alone.
static void process_output( void )
{
    int64_t pid    = 0;
    int32_t status = -1;
    int32_t fd     = -1;
    char    got[16];
    char    packed[sizeof self + 64];

    const uint64_t size = packed_self( packed, "child_prints" );
    printf( "%d ", kl_rt_process_start( (const uint8_t*) packed, size, 2, &pid, &fd ) );
    read_all( fd, got, sizeof got );
    const int32_t waited = kl_rt_process_wait( pid, &status );
    printf( "%s %d %d ", got, waited, status );
    printf( "%d ", kl_rt_file_close( fd ) );

    // A failed start leaves no fd behind.
    fd                 = -1;
    const bool missing = kl_rt_process_start( PACKED( "kl-rt-no-such-program\0" ), 1, &pid, &fd ) == ENOENT;
    printf( "%d %d", missing, fd );
}

// Both of a panicking child's streams through one pipe, in the order written, and its exit 134.
static void process_capture( void )
{
    int64_t pid    = 0;
    int32_t status = -1;
    int32_t fd     = -1;
    char    got[64];
    char    packed[sizeof self + 64];

    const uint64_t size = packed_self( packed, "child_panics" );
    printf( "%d ", kl_rt_process_capture( (const uint8_t*) packed, size, 2, &pid, &fd ) );
    read_all( fd, got, sizeof got );
    kl_rt_file_close( fd );
    const int32_t waited = kl_rt_process_wait( pid, &status );
    printf( "%d %d %s", waited, status, got );
}

#undef PACKED

// The path ends in this binary's name, and a buffer it fills is refused.
static void program_path( void )
{
    uint8_t  buffer[4096];
    uint64_t length = 0;
    printf( "%d ", kl_rt_program_path( buffer, sizeof buffer, &length ) );

    const char name[] = PROGRAM_NAME;
    printf( "%d ", length >= sizeof name - 1 && memcmp( buffer + length - ( sizeof name - 1 ), name, sizeof name - 1 ) == 0 );
    printf( "%d", kl_rt_program_path( buffer, 4, &length ) == ERANGE );
}

// The directory matches getcwd's, and a buffer too small is refused.
static void current_directory( void )
{
    uint8_t  buffer[4096];
    uint64_t length = 0;
    char     want[4096];
    printf( "%d ", kl_rt_current_directory( buffer, sizeof buffer, &length ) );
    printf( "%d ", os_getcwd( want, sizeof want ) != NULL && length == strlen( want ) && memcmp( buffer, want, length ) == 0 );
    printf( "%d", kl_rt_current_directory( buffer, 1, &length ) == ERANGE );
}

// Each clock reads and has a resolution, and a clock past the table is refused.
static void clocks( void )
{
    int64_t ns = -1;
    for( int32_t which = 0; which < 3; ++which )
    {
        int64_t res = -1;
        printf( "%d%d ", kl_rt_clock( which, &ns ) == 0 && ns > 0, kl_rt_clock_resolution( which, &res ) == 0 && res > 0 );
    }
    printf( "%d %d", kl_rt_clock( 3, &ns ) == EINVAL, kl_rt_clock_resolution( -1, &ns ) == EINVAL );
}

// A variable reads back, an empty one is size 0, and an unset name, `=` in one and a 0 byte in one
// are refused.
static void environment( void )
{
#define NAME( text ) (const uint8_t*) text, sizeof text - 1

    os_setenv( "KL_RT_SET", "value" );
    os_setenv( "KL_RT_EMPTY", "" );

    uint8_t* value = NULL;
    uint64_t size  = 9;
    printf( "%d ", kl_rt_environment( NAME( "KL_RT_SET" ), &value, &size ) );
    printf( "%.*s ", (int) size, (const char*) value );
    const int32_t empty = kl_rt_environment( NAME( "KL_RT_EMPTY" ), &value, &size );
    printf( "%d %d ", empty, (int) size );
    printf( "%d ", kl_rt_environment( NAME( "KL_RT" ), &value, &size ) == ENOENT );
    printf( "%d ", kl_rt_environment( NAME( "KL_RT_SET=value" ), &value, &size ) == ENOENT );
    printf( "%d ", kl_rt_environment( NAME( "KL_RT_SET\0" ), &value, &size ) == ENOENT );
    printf( "%d", kl_rt_environment( NAME( "" ), &value, &size ) == ENOENT );

#undef NAME
}

// A chain far deeper than one stack, dropped as keelc emits a deep drop (D76): a check against
// `kl_rt_stack_low` at entry, and the rest on a fresh segment under it.
typedef struct Link
{
    struct Link* next;
} Link;

static uint64_t dropped = 0;

static void drop_link( void* value )
{
    if( (uintptr_t) __builtin_frame_address( 0 ) < kl_rt_stack_low )
    {
        kl_rt_deeper( drop_link, value );
        return;
    }

    Link* link = value;
    if( link->next != NULL )
    {
        drop_link( link->next );
    }
    free( link );
    ++dropped;
}

static void deep_drop( void )
{
    Link* head = NULL;
    for( int i = 0; i < 1000000; ++i )
    {
        Link* link = malloc( sizeof( Link ) );
        if( link == NULL )
        {
            exit( 2 );
        }
        link->next = head;
        head       = link;
    }

    drop_link( head );
    printf( "%llu", (unsigned long long) dropped );
}

// A frame over a page, which on Windows calls __chkstk to touch each page in order.
static void large_frame( void )
{
    volatile char big[64 * 1024];
    big[0]              = 'a';
    big[sizeof big - 1] = 'z';
    printf( "%c%c", big[0], big[sizeof big - 1] );
}

typedef struct
{
    const char* name;
    void ( *body )( void );
    const char* want; // stdout and stderr together; NULL when the output varies
    int32_t     status;
} Case;

#ifdef _WIN32
#define ROUND_TRIP "before|child0 0 0 3 1 7 1 1 1 1|after"
#else
#define ROUND_TRIP "before|child0 0 0 3 137 1 7 1 1 1 1 1|after"
#endif

static const Case cases[] = {
    { "lowest_i64", lowest_i64, "-9223372036854775808", 0 },
    { "largest_u64", largest_u64, "18446744073709551615", 0 },
    { "panic_message", panic_message, "before|main.kl:7: panic: empty\n", 134 },
    { "panic_at", panic_at, "caller.kl:12: panic: empty\n", 134 },
    { "hash_key", hash_key, NULL, 0 },
    { "hash_key_varies", hash_key_varies, "varies", 0 },
    { "file_open_refusals", file_open_refusals, "22 -1 22 -1", 0 },
    { "file_round_trip", file_round_trip, "0 0 0 0 0 3 2 0", 0 },
    { "file_bad_descriptor", file_bad_descriptor, "1 1 1", 0 },
    { "error_kinds", error_kinds, "1223456780", 0 },
    { "input_flushes_stdout", input_flushes_stdout, "prompt|after", 0 },
    { "file_write_keeps_order", file_write_keeps_order, "abc", 0 },
    { "path_kinds", path_kinds, "02 01 00 00 03 22 9", 0 },
    { "directory_round_trip", directory_round_trip, "0 1 0 0 x 0 0 1 1 1 0 1 0 1 1", 0 },
    { "child_prints", child_prints, "child", 0 },
    { "child_exits", child_exits, "", 3 },
    { "child_panics", child_panics, "out|test.kl:1: panic: x\n", 134 },
    { "process_round_trip", process_round_trip, ROUND_TRIP, 0 },
    { "process_output", process_output, "0 child 0 0 0 1 -1", 0 },
    { "process_capture", process_capture, "0 0 134 out|test.kl:1: panic: x\n", 0 },
    { "program_path", program_path, "0 1 1", 0 },
    { "current_directory", current_directory, "0 1 1", 0 },
    { "clocks", clocks, "11 11 11 1 1", 0 },
    { "environment", environment, "0 value 0 0 1 1 1 1", 0 },
    { "deep_drop", deep_drop, "1000000", 0 },
    { "large_frame", large_frame, "az", 0 },
};

enum
{
    k_cases = sizeof cases / sizeof cases[0]
};

int main( int argc, char** argv )
{
    if( kl_rt_program_path( (uint8_t*) self, sizeof self, &self_size ) != 0 )
    {
        fprintf( stderr, "cannot find this program's path\n" );
        return 2;
    }

    // A child: run the one case named.
    if( argc == 2 )
    {
        for( size_t i = 0; i < k_cases; ++i )
        {
            if( strcmp( argv[1], cases[i].name ) == 0 )
            {
                cases[i].body();
                return 0;
            }
        }
        fprintf( stderr, "no case named %s\n", argv[1] );
        return 2;
    }

    for( size_t i = 0; i < k_cases; ++i )
    {
        const Case* c = &cases[i];
        const Run   r = run( c->name );
        if( c->want != NULL && strcmp( r.out, c->want ) != 0 )
        {
            fprintf( stderr, "%s: output was \"%s\", expected \"%s\"\n", c->name, r.out, c->want );
            failures++;
        }
        if( r.status != c->status )
        {
            fprintf( stderr, "%s: exit was %d, expected %d\n", c->name, r.status, c->status );
            failures++;
        }
    }

    // Each process draws its own key.
    const Run first  = run( "hash_key" );
    const Run second = run( "hash_key" );
    if( strlen( first.out ) != 32 || strcmp( first.out, second.out ) == 0 )
    {
        fprintf( stderr, "hash_key: two processes drew \"%s\" and \"%s\"\n", first.out, second.out );
        failures++;
    }

    if( failures != 0 )
    {
        fprintf( stderr, "%d check%s failed\n", failures, failures == 1 ? "" : "s" );
        return 1;
    }

    printf( "%d runtime tests passed\n", (int) k_cases );
    return 0;
}

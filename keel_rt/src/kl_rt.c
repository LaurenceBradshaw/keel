// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#define _DEFAULT_SOURCE
#include "kl_rt.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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

void kl_rt_hash_key( uint64_t* low, uint64_t* high )
{
    // Not thread-safe; Keel has no threads yet.
    static uint64_t key[2];
    static bool     drawn = false;

    if( !drawn )
    {
        if( getentropy( key, sizeof key ) != 0 )
        {
            kl_rt_panic( __FILE__, __LINE__, "no random bytes for a map's key" );
        }

        drawn = true;
    }

    *low  = key[0]++;
    *high = key[1];
}

static char* c_path( const uint8_t* data, uint64_t size, int* error )
{
    if( memchr( data, 0, size ) != NULL )
    {
        *error = EINVAL;
        return NULL;
    }

    char* name = malloc( size + 1 );
    if( name == NULL )
    {
        *error = ENOMEM;
        return NULL;
    }

    memcpy( name, data, size );
    name[size] = '\0';
    return name;
}

int32_t kl_rt_file_open( const uint8_t* path, uint64_t size, int32_t mode, int32_t* fd )
{
    int error = 0;

    int flags = 0;
    switch( mode )
    {
    case 0:
        flags = O_RDONLY;
        break;
    case 1:
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        break;
    case 2:
        flags = O_WRONLY | O_CREAT | O_APPEND;
        break;
    case 3:
        flags = O_WRONLY | O_CREAT | O_EXCL;
        break;
    default:
        return EINVAL;
    }
    flags |= O_CLOEXEC;

    char* name = c_path( path, size, &error );
    if( name == NULL )
    {
        return error;
    }

    int opened = open( name, flags, 0666 );
    int saved  = errno;
    free( name );

    if( opened == -1 )
    {
        return saved;
    }

    *fd = opened;
    return 0;
}

int32_t kl_rt_file_read( int32_t fd, uint8_t* data, uint64_t size, uint64_t* count )
{
    ssize_t got = read( fd, data, size );
    if( got == -1 )
    {
        return errno;
    }

    *count = (uint64_t) got;
    return 0;
}

int32_t kl_rt_file_write( int32_t fd, const uint8_t* data, uint64_t size )
{
    if( fd == 1 || fd == 2 )
    {
        FILE* out = kl_rt_stream( fd );
        if( fwrite( data, 1, size, out ) != size || fflush( out ) != 0 )
        {
            return errno;
        }

        return 0;
    }

    uint64_t done = 0;

    while( done < size )
    {
        ssize_t put = write( fd, data + done, size - done );
        if( put == -1 )
        {
            return errno;
        }
        done += (uint64_t) put;
    }

    return 0;
}

int32_t kl_rt_file_close( int32_t fd )
{
    return close( fd ) == -1 ? errno : 0;
}

int32_t kl_rt_error_kind( int32_t code )
{
    switch( code )
    {
    case ENOENT:
        return KL_RT_NOT_FOUND;
    case EACCES:
    case EPERM:
        return KL_RT_DENIED;
    case EEXIST:
        return KL_RT_EXISTS;
    case EISDIR:
        return KL_RT_IS_DIRECTORY;
    case ENOTDIR:
        return KL_RT_NOT_DIRECTORY;
    case EINTR:
        return KL_RT_INTERRUPTED;
    case ENOMEM:
        return KL_RT_OUT_OF_MEMORY;
    case ENOTEMPTY:
        return KL_RT_NOT_EMPTY;
    default:
        return KL_RT_OTHER;
    }
}

// Shared by every reader of standard input and lives here until a Keel global can hold it.
static uint8_t  kl_rt_input[65536];
static uint64_t kl_rt_input_start = 0;
static uint64_t kl_rt_input_end   = 0;

int32_t kl_rt_input_fill( uint8_t** data, uint64_t* size )
{
    if( kl_rt_input_start == kl_rt_input_end )
    {
        fflush( stdout );
        ssize_t got = read( 0, kl_rt_input, sizeof kl_rt_input );
        if( got == -1 )
        {
            return errno;
        }
        kl_rt_input_end   = (uint64_t) got;
        kl_rt_input_start = 0;
    }

    *data = kl_rt_input + kl_rt_input_start;
    *size = kl_rt_input_end - kl_rt_input_start;
    return 0;
}

void kl_rt_input_take( uint64_t count )
{
    kl_rt_input_start += count;
}

int32_t kl_rt_path_kind( const uint8_t* path, uint64_t size, int32_t* kind )
{
    int   error = 0;
    char* name  = c_path( path, size, &error );
    if( name == NULL )
    {
        return error;
    }

    struct stat st;
    if( stat( name, &st ) == -1 )
    {
        int saved = errno;
        free( name );
        if( saved == ENOENT || saved == ENOTDIR )
        {
            *kind = 0;
            return 0;
        }
        return saved;
    }

    if( S_ISREG( st.st_mode ) )
    {
        *kind = 1;
    }
    else if( S_ISDIR( st.st_mode ) )
    {
        *kind = 2;
    }
    else
    {
        *kind = 3;
    }

    free( name );
    return 0;
}

int32_t kl_rt_create_directory( const uint8_t* path, uint64_t size )
{
    int   error = 0;
    char* name  = c_path( path, size, &error );
    if( name == NULL )
    {
        return error;
    }

    int saved = mkdir( name, 0777 ) == -1 ? errno : 0;
    free( name );
    return saved;
}

int32_t kl_rt_delete_directory( const uint8_t* path, uint64_t size )
{
    int   error = 0;
    char* name  = c_path( path, size, &error );
    if( name == NULL )
    {
        return error;
    }

    int saved = rmdir( name ) == -1 ? errno : 0;
    free( name );
    return saved;
}

int32_t kl_rt_delete_file( const uint8_t* path, uint64_t size )
{
    int   error = 0;
    char* name  = c_path( path, size, &error );
    if( name == NULL )
    {
        return error;
    }

    int saved = unlink( name ) == -1 ? errno : 0;
    free( name );
    return saved;
}

int32_t kl_rt_directory_open( const uint8_t* path, uint64_t size, uint64_t* handle )
{
    int   error = 0;
    char* name  = c_path( path, size, &error );
    if( name == NULL )
    {
        return error;
    }

    DIR* dir = opendir( name );
    if( dir == NULL )
    {
        error = errno;
        free( name );
        return error;
    }

    *handle = (uint64_t) (uintptr_t) dir;
    free( name );
    return 0;
}

int32_t kl_rt_directory_next( uint64_t handle, uint8_t** name, uint64_t* size )
{
    DIR* dir = (DIR*) (uintptr_t) handle;

    while( true )
    {
        // Cleared first: readdir returns NULL both at the end and on failure.
        errno                = 0;
        struct dirent* entry = readdir( dir );
        if( entry == NULL )
        {
            *size = 0;
            return errno;
        }

        if( strcmp( entry->d_name, "." ) != 0 && strcmp( entry->d_name, ".." ) != 0 )
        {
            // Lent until the next call on this handle.
            *name = (uint8_t*) entry->d_name;
            *size = strlen( entry->d_name );
            return 0;
        }
    }
}

void kl_rt_directory_close( uint64_t handle )
{
    DIR* dir = (DIR*) (uintptr_t) handle;

    closedir( dir );
}

int32_t kl_rt_process_start( const uint8_t* packed, uint64_t size, uint64_t count, int64_t* pid )
{
    if( count == 0 )
    {
        return EINVAL;
    }

    uint64_t zero_count = 0;
    for( uint64_t i = 0; i < size; ++i )
    {
        if( packed[i] == 0 )
        {
            ++zero_count;
        }
    }

    if( zero_count != count || packed[size - 1] != 0 )
    {
        return EINVAL;
    }

    char** argv = malloc( ( count + 1 ) * sizeof( char* ) );
    if( argv == NULL )
    {
        return ENOMEM;
    }

    char* next = (char*) packed;
    for( uint64_t i = 0; i < count; ++i )
    {
        argv[i] = next;
        next += strlen( next ) + 1;
    }
    argv[count] = NULL;

    // Close-on-exec, so it closes unread when the program starts, and carries `errno` back when it
    // cannot.
    int report[2];
    if( pipe( report ) == -1 )
    {
        int saved = errno;
        free( argv );
        return saved;
    }

    for( uint64_t i = 0; i < 2; ++i )
    {
        if( fcntl( report[i], F_SETFD, FD_CLOEXEC ) == -1 )
        {
            int saved = errno;
            close( report[0] );
            close( report[1] );
            free( argv );
            return saved;
        }
    }

    // So what the parent printed comes before what the child prints.
    fflush( NULL );

    // Both processes return from fork: the child sees 0, the parent the child's pid.
    pid_t child = fork();

    if( child == 0 )
    {
        close( report[0] );

        // Returns only on failure; `_exit` so the child never flushes or runs the parent's handlers.
        execvp( argv[0], argv );

        int     saved  = errno;
        ssize_t result = write( report[1], &saved, sizeof saved );
        (void) result;
        _exit( 127 );
    }

    int error = child == -1 ? errno : 0;
    free( argv );

    // Closed before reading, or the read would wait on this process's own write end.
    close( report[1] );

    if( child != -1 )
    {
        // Retried, since giving up would lose the child.
        ssize_t got = 0;
        do
        {
            got = read( report[0], &error, sizeof error );
        } while( got == -1 && errno == EINTR );

        if( got == sizeof error )
        {
            // The exec failed and the child has exited: reap it.
            waitpid( child, NULL, 0 );
        }
        else
        {
            // The pipe closed unread at the exec, so the program is running.
            error = 0;
            *pid  = child;
        }
    }

    close( report[0] );
    return error;
}

int32_t kl_rt_process_wait( int64_t pid, int32_t* status )
{
    int st = 0;
    if( waitpid( (pid_t) pid, &st, 0 ) == -1 )
    {
        return errno;
    }

    *status = WIFEXITED( st ) ? WEXITSTATUS( st ) : 128 + WTERMSIG( st );
    return 0;
}

static const clockid_t kl_rt_clocks[] = { CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW };

int32_t kl_rt_clock( int32_t which, int64_t* nanoseconds )
{
    if( which < 0 || which >= (int32_t) ( sizeof kl_rt_clocks / sizeof kl_rt_clocks[0] ) )
    {
        return EINVAL;
    }

    struct timespec ts;
    if( clock_gettime( kl_rt_clocks[which], &ts ) == -1 )
    {
        return errno;
    }

    *nanoseconds = (int64_t) ts.tv_sec * 1000000000 + ts.tv_nsec;
    return 0;
}

int32_t kl_rt_clock_resolution( int32_t which, int64_t* nanoseconds )
{
    if( which < 0 || which >= (int32_t) ( sizeof kl_rt_clocks / sizeof kl_rt_clocks[0] ) )
    {
        return EINVAL;
    }

    struct timespec ts;
    if( clock_getres( kl_rt_clocks[which], &ts ) == -1 )
    {
        return errno;
    }

    *nanoseconds = (int64_t) ts.tv_sec * 1000000000 + ts.tv_nsec;
    return 0;
}

extern char** environ;

// Lends the value from environ; a name never set (empty, holding '=' or 0) is ENOENT.
int32_t kl_rt_environment( const uint8_t* name, uint64_t size, uint8_t** value, uint64_t* value_size )
{
    if( size == 0 || memchr( name, '=', size ) || memchr( name, 0, size ) )
    {
        return ENOENT;
    }

    for( char** e = environ; *e; ++e )
    {
        if( strncmp( *e, (const char*) name, size ) == 0 && ( *e )[size] == '=' )
        {
            *value      = (uint8_t*) *e + size + 1;
            *value_size = strlen( (const char*) *value );
            return 0;
        }
    }
    return ENOENT;
}

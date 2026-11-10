// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The Win32 half: kl_rt.c's POSIX calls over the W functions, text crossing as UTF-8 and failures
// answered as the UCRT's errno values, so kl_rt_error_kind and its callers are the same on both.

#define WIN32_LEAN_AND_MEAN
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "kl_rt.h"

static int errno_of( DWORD error )
{
    switch( error )
    {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
        return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_WRITE_PROTECT:
        return EACCES;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return EEXIST;
    case ERROR_DIRECTORY:
        return ENOTDIR;
    case ERROR_DIR_NOT_EMPTY:
        return ENOTEMPTY;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return ENOMEM;
    case ERROR_INVALID_HANDLE:
        return EBADF;
    case ERROR_FILENAME_EXCED_RANGE:
        return ENAMETOOLONG;
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:
        return EPIPE;
    case ERROR_BAD_EXE_FORMAT:
        return ENOEXEC;
    case ERROR_INVALID_NAME:
    case ERROR_INVALID_PARAMETER:
        return EINVAL;
    default:
        return EIO;
    }
}

// `size` bytes of UTF-8 as a 0-terminated UTF-16 string the caller frees; NULL with `*error` set
// when they hold a 0 byte or are not UTF-8.
static wchar_t* wide( const uint8_t* data, uint64_t size, int* error )
{
    if( memchr( data, 0, size ) != NULL || size > INT_MAX )
    {
        *error = EINVAL;
        return NULL;
    }

    const int length =
        size == 0 ? 0 : MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, (const char*) data, (int) size, NULL, 0 );
    if( size != 0 && length == 0 )
    {
        *error = EINVAL;
        return NULL;
    }

    wchar_t* text = malloc( ( (size_t) length + 1 ) * sizeof( wchar_t ) );
    if( text == NULL )
    {
        *error = ENOMEM;
        return NULL;
    }

    if( length != 0 )
    {
        MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, (const char*) data, (int) size, text, length );
    }
    text[length] = L'\0';
    return text;
}

// `text` as UTF-8 into `buffer`, with its 0 byte; ERANGE when that does not fit.
static int32_t narrow_into( const wchar_t* text, uint8_t* buffer, uint64_t size, uint64_t* length )
{
    const int needed = WideCharToMultiByte( CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL );
    if( needed == 0 )
    {
        return errno_of( GetLastError() );
    }
    if( (uint64_t) needed > size )
    {
        return ERANGE;
    }

    WideCharToMultiByte( CP_UTF8, 0, text, -1, (char*) buffer, needed, NULL, NULL );
    *length = (uint64_t) needed - 1;
    return 0;
}

// As narrow_into, with `/` for each `\`, as kl::path splits a path.
static int32_t narrow_path_into( wchar_t* text, uint8_t* buffer, uint64_t size, uint64_t* length )
{
    for( wchar_t* at = text; *at != L'\0'; ++at )
    {
        if( *at == L'\\' )
        {
            *at = L'/';
        }
    }
    return narrow_into( text, buffer, size, length );
}

static bool is_directory( const wchar_t* name )
{
    const DWORD attributes = GetFileAttributesW( name );
    return attributes != INVALID_FILE_ATTRIBUTES && ( attributes & FILE_ATTRIBUTE_DIRECTORY ) != 0;
}

// Whether a folder `name` passes through is a file, which POSIX calls ENOTDIR and Windows a
// missing path.
static bool under_file( wchar_t* name )
{
    for( wchar_t* at = name; *at != L'\0'; ++at )
    {
        if( ( *at == L'/' || *at == L'\\' ) && at != name && at[-1] != L':' )
        {
            const wchar_t separator = *at;
            *at                     = L'\0';
            const DWORD attributes  = GetFileAttributesW( name );
            *at                     = separator;
            if( attributes != INVALID_FILE_ATTRIBUTES && ( attributes & FILE_ATTRIBUTE_DIRECTORY ) == 0 )
            {
                return true;
            }
        }
    }
    return false;
}

// A failure on `name`, telling a file where a folder was wanted apart from a missing path.
static int path_error( wchar_t* name, DWORD error )
{
    return error == ERROR_PATH_NOT_FOUND && under_file( name ) ? ENOTDIR : errno_of( error );
}

// As path_error, and telling a directory where a file was wanted apart from a plain refusal.
static int file_error( wchar_t* name, DWORD error )
{
    return error == ERROR_ACCESS_DENIED && is_directory( name ) ? EISDIR : path_error( name, error );
}

int32_t kl_rt_file_open( const uint8_t* path, uint64_t size, int32_t mode, int32_t* fd )
{
    DWORD access      = GENERIC_WRITE;
    DWORD disposition = 0;
    switch( mode )
    {
    case 0:
        access      = GENERIC_READ;
        disposition = OPEN_EXISTING;
        break;
    case 1:
        disposition = CREATE_ALWAYS;
        break;
    case 2:
        access      = FILE_APPEND_DATA | SYNCHRONIZE; // every write at the end, as O_APPEND
        disposition = OPEN_ALWAYS;
        break;
    case 3:
        disposition = CREATE_NEW;
        break;
    default:
        return EINVAL;
    }

    int      error = 0;
    wchar_t* name  = wide( path, size, &error );
    if( name == NULL )
    {
        return error;
    }

    const DWORD share  = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE      handle = CreateFileW( name, access, share, NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL );
    if( handle == INVALID_HANDLE_VALUE )
    {
        error = file_error( name, GetLastError() );
        free( name );
        return error;
    }
    free( name );

    // Not inherited by a child, as O_CLOEXEC.
    const int opened = _open_osfhandle( (intptr_t) handle, _O_BINARY | _O_NOINHERIT | ( mode == 0 ? _O_RDONLY : _O_WRONLY ) );
    if( opened == -1 )
    {
        error = errno;
        CloseHandle( handle );
        return error;
    }

    *fd = opened;
    return 0;
}

int32_t kl_rt_file_read( int32_t fd, uint8_t* data, uint64_t size, uint64_t* count )
{
    if( fd == 0 )
    {
        fflush( stdout ); // a prompt shows before the program waits
    }

    const int got = _read( fd, data, size > INT_MAX ? INT_MAX : (unsigned) size );
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
        FILE* out = fd == 2 ? stderr : stdout;
        if( fd == 2 )
        {
            fflush( stdout );
        }
        if( fwrite( data, 1, size, out ) != size || fflush( out ) != 0 )
        {
            return errno;
        }

        return 0;
    }

    uint64_t done = 0;

    while( done < size )
    {
        const uint64_t left = size - done;
        const int      put  = _write( fd, data + done, left > INT_MAX ? INT_MAX : (unsigned) left );
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
    return _close( fd ) == -1 ? errno : 0;
}

int32_t kl_rt_path_kind( const uint8_t* path, uint64_t size, int32_t* kind )
{
    int      error = 0;
    wchar_t* name  = wide( path, size, &error );
    if( name == NULL )
    {
        return error;
    }

    // Opened for no access, so a directory or a device answers too; a link is followed, as stat does.
    const DWORD share  = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE      handle = CreateFileW( name, 0, share, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL );
    if( handle == INVALID_HANDLE_VALUE )
    {
        const DWORD failure    = GetLastError();
        const DWORD attributes = GetFileAttributesW( name );
        free( name );

        switch( failure )
        {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_DIRECTORY:
        case ERROR_INVALID_NAME:
        case ERROR_INVALID_DRIVE:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
            *kind = 0;
            return 0;
        default:
            if( attributes == INVALID_FILE_ATTRIBUTES )
            {
                return errno_of( failure );
            }
            // In use or locked, but there.
            *kind = ( attributes & FILE_ATTRIBUTE_DIRECTORY ) != 0 ? 2 : 1;
            return 0;
        }
    }
    free( name );

    BY_HANDLE_FILE_INFORMATION information;
    const DWORD                type = GetFileType( handle );
    if( type != FILE_TYPE_DISK )
    {
        *kind = 3;
    }
    else if( !GetFileInformationByHandle( handle, &information ) )
    {
        error = errno_of( GetLastError() );
    }
    else
    {
        *kind = ( information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) != 0 ? 2 : 1;
    }

    CloseHandle( handle );
    return error;
}

int32_t kl_rt_create_directory( const uint8_t* path, uint64_t size )
{
    int      error = 0;
    wchar_t* name  = wide( path, size, &error );
    if( name == NULL )
    {
        return error;
    }

    error = CreateDirectoryW( name, NULL ) ? 0 : path_error( name, GetLastError() );
    free( name );
    return error;
}

int32_t kl_rt_delete_directory( const uint8_t* path, uint64_t size )
{
    int      error = 0;
    wchar_t* name  = wide( path, size, &error );
    if( name == NULL )
    {
        return error;
    }

    error = RemoveDirectoryW( name ) ? 0 : path_error( name, GetLastError() );
    free( name );
    return error;
}

int32_t kl_rt_delete_file( const uint8_t* path, uint64_t size )
{
    int      error = 0;
    wchar_t* name  = wide( path, size, &error );
    if( name == NULL )
    {
        return error;
    }

    error = DeleteFileW( name ) ? 0 : file_error( name, GetLastError() );
    free( name );
    return error;
}

typedef struct
{
    HANDLE           find;
    WIN32_FIND_DATAW entry;
    bool             pending; // `entry` is the first, not handed out yet
    bool             done;
    char             name[MAX_PATH * 3 + 1]; // lent until the next call
} Directory;

int32_t kl_rt_directory_open( const uint8_t* path, uint64_t size, uint64_t* handle )
{
    int      error = 0;
    wchar_t* name  = wide( path, size, &error );
    if( name == NULL )
    {
        return error;
    }

    const size_t length  = wcslen( name );
    wchar_t*     pattern = malloc( ( length + 3 ) * sizeof( wchar_t ) );
    Directory*   dir     = malloc( sizeof( Directory ) );
    if( pattern == NULL || dir == NULL || length == 0 )
    {
        free( pattern );
        free( dir );
        free( name );
        return length == 0 ? ENOENT : ENOMEM;
    }

    memcpy( pattern, name, length * sizeof( wchar_t ) );
    size_t end = length;
    if( name[length - 1] != L'/' && name[length - 1] != L'\\' )
    {
        pattern[end++] = L'\\';
    }
    pattern[end++] = L'*';
    pattern[end]   = L'\0';

    dir->find    = FindFirstFileExW( pattern, FindExInfoBasic, &dir->entry, FindExSearchNameMatch, NULL, 0 );
    dir->pending = dir->find != INVALID_HANDLE_VALUE;
    dir->done    = false;
    free( pattern );

    if( dir->find == INVALID_HANDLE_VALUE )
    {
        const DWORD failure    = GetLastError();
        const DWORD attributes = GetFileAttributesW( name );
        if( failure == ERROR_FILE_NOT_FOUND && attributes != INVALID_FILE_ATTRIBUTES )
        {
            dir->done = true; // an empty root
        }
        else
        {
            error = attributes != INVALID_FILE_ATTRIBUTES && ( attributes & FILE_ATTRIBUTE_DIRECTORY ) == 0
                        ? ENOTDIR
                        : path_error( name, failure );
            free( dir );
            free( name );
            return error;
        }
    }

    free( name );
    *handle = (uint64_t) (uintptr_t) dir;
    return 0;
}

int32_t kl_rt_directory_next( uint64_t handle, uint8_t** name, uint64_t* size )
{
    Directory* dir = (Directory*) (uintptr_t) handle;

    while( !dir->done )
    {
        if( !dir->pending && !FindNextFileW( dir->find, &dir->entry ) )
        {
            const DWORD failure = GetLastError();
            if( failure != ERROR_NO_MORE_FILES )
            {
                *size = 0;
                return errno_of( failure );
            }
            dir->done = true;
            break;
        }
        dir->pending = false;

        const wchar_t* found = dir->entry.cFileName;
        if( wcscmp( found, L"." ) == 0 || wcscmp( found, L".." ) == 0 )
        {
            continue;
        }

        uint64_t      length = 0;
        const int32_t error  = narrow_into( found, (uint8_t*) dir->name, sizeof dir->name, &length );
        if( error != 0 )
        {
            *size = 0;
            return error;
        }
        *name = (uint8_t*) dir->name;
        *size = length;
        return 0;
    }

    *size = 0;
    return 0;
}

void kl_rt_directory_close( uint64_t handle )
{
    Directory* dir = (Directory*) (uintptr_t) handle;

    if( dir->find != INVALID_HANDLE_VALUE )
    {
        FindClose( dir->find );
    }
    free( dir );
}

// Appends `argument` as CommandLineToArgvW and the UCRT read it back.
static wchar_t* quote( wchar_t* out, const wchar_t* argument )
{
    if( argument[0] != L'\0' && wcspbrk( argument, L" \t\n\v\"" ) == NULL )
    {
        const size_t length = wcslen( argument );
        memcpy( out, argument, length * sizeof( wchar_t ) );
        return out + length;
    }

    *out++ = L'"';
    for( const wchar_t* c = argument;; ++c )
    {
        size_t slashes = 0;
        while( *c == L'\\' )
        {
            ++slashes;
            ++c;
        }

        // Backslashes double before a quote, the closing one included.
        const size_t copies = *c == L'\0' ? 2 * slashes : *c == L'"' ? 2 * slashes + 1 : slashes;
        for( size_t i = 0; i < copies; ++i )
        {
            *out++ = L'\\';
        }
        if( *c == L'\0' )
        {
            break;
        }
        *out++ = *c;
    }
    *out++ = L'"';
    return out;
}

// `packed`'s arguments as one command line the caller frees; each may double in quoting.
static wchar_t* command_line( const uint8_t* packed, uint64_t size, uint64_t count, int* error )
{
    // Each argument converted alone, since `wide` refuses the 0 bytes between them.
    wchar_t* line = malloc( ( 2 * size + 3 * count + 1 ) * sizeof( wchar_t ) );
    if( line == NULL )
    {
        *error = ENOMEM;
        return NULL;
    }

    wchar_t*    out  = line;
    const char* next = (const char*) packed;
    for( uint64_t i = 0; i < count; ++i )
    {
        const size_t length   = strlen( next );
        wchar_t*     argument = wide( (const uint8_t*) next, length, error );
        if( argument == NULL )
        {
            free( line );
            return NULL;
        }
        if( i != 0 )
        {
            *out++ = L' ';
        }
        out = quote( out, argument );
        free( argument );
        next += length + 1;
    }
    *out = L'\0';
    return line;
}

// An inheritable copy of `handle` for the child, or `handle` itself when there is none to copy.
static HANDLE for_child( HANDLE handle, bool* copied )
{
    HANDLE copy = NULL;
    *copied     = handle != NULL && handle != INVALID_HANDLE_VALUE &&
              DuplicateHandle( GetCurrentProcess(), handle, GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS );
    return *copied ? copy : handle;
}

// When `output` is not null, the child's stdout is a pipe and `*output` its read end; its stderr too
// when `errors` is set. The pid is the process's handle, which kl_rt_process_wait closes.
static int32_t start( const uint8_t* packed, uint64_t size, uint64_t count, int64_t* pid, int32_t* output, bool errors )
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

    int      error = 0;
    wchar_t* line  = command_line( packed, size, count, &error );
    if( line == NULL )
    {
        return error;
    }

    // The read end stays this process's alone; the write end is the child's.
    HANDLE read_end  = NULL;
    HANDLE write_end = NULL;
    if( output != NULL )
    {
        if( !CreatePipe( &read_end, &write_end, NULL, 0 ) ||
            !SetHandleInformation( write_end, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT ) )
        {
            error = errno_of( GetLastError() );
            if( read_end != NULL )
            {
                CloseHandle( read_end );
                CloseHandle( write_end );
            }
            free( line );
            return error;
        }
    }

    bool         copied[3] = { false, false, false };
    STARTUPINFOW startup;
    memset( &startup, 0, sizeof startup );
    startup.cb         = sizeof startup;
    startup.dwFlags    = STARTF_USESTDHANDLES;
    startup.hStdInput  = for_child( GetStdHandle( STD_INPUT_HANDLE ), &copied[0] );
    startup.hStdOutput = output != NULL ? write_end : for_child( GetStdHandle( STD_OUTPUT_HANDLE ), &copied[1] );
    startup.hStdError  = output != NULL && errors ? write_end : for_child( GetStdHandle( STD_ERROR_HANDLE ), &copied[2] );

    // So what the parent printed comes before what the child prints.
    fflush( NULL );

    PROCESS_INFORMATION process;
    const BOOL          started = CreateProcessW( NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process );
    error                       = started ? 0 : errno_of( GetLastError() );
    free( line );

    HANDLE given[3] = { startup.hStdInput, startup.hStdOutput, startup.hStdError };
    for( int i = 0; i < 3; ++i )
    {
        if( copied[i] )
        {
            CloseHandle( given[i] );
        }
    }
    if( write_end != NULL )
    {
        CloseHandle( write_end );
    }

    if( started )
    {
        CloseHandle( process.hThread );
        if( output != NULL )
        {
            const int fd = _open_osfhandle( (intptr_t) read_end, _O_RDONLY | _O_BINARY | _O_NOINHERIT );
            if( fd == -1 )
            {
                error = errno;
                CloseHandle( read_end );
                TerminateProcess( process.hProcess, 1 );
                CloseHandle( process.hProcess );
                return error;
            }
            *output = fd;
        }
        *pid = (int64_t) (intptr_t) process.hProcess;
        return 0;
    }

    if( read_end != NULL )
    {
        CloseHandle( read_end );
    }
    return error;
}

int32_t kl_rt_process_start( const uint8_t* packed, uint64_t size, uint64_t count, int64_t* pid, int32_t* output )
{
    return start( packed, size, count, pid, output, false );
}

// Both of the child's streams into one pipe, `*output` its read end.
int32_t kl_rt_process_capture( const uint8_t* packed, uint64_t size, uint64_t count, int64_t* pid, int32_t* output )
{
    return start( packed, size, count, pid, output, true );
}

// The exit code; a crash is its exception code, as `GetExitCodeProcess` gives it.
int32_t kl_rt_process_wait( int64_t pid, int32_t* status )
{
    HANDLE process = (HANDLE) (intptr_t) pid;
    DWORD  code    = 0;
    if( WaitForSingleObject( process, INFINITE ) == WAIT_FAILED || !GetExitCodeProcess( process, &code ) )
    {
        return errno_of( GetLastError() );
    }

    CloseHandle( process );
    *status = (int32_t) code;
    return 0;
}

static int64_t ticks_per_second( void )
{
    static LARGE_INTEGER frequency; // fixed at boot
    if( frequency.QuadPart == 0 )
    {
        QueryPerformanceFrequency( &frequency );
    }
    return frequency.QuadPart;
}

// 0 the wall clock, 1 and 2 the performance counter, which never steps.
int32_t kl_rt_clock( int32_t which, int64_t* nanoseconds )
{
    if( which == 0 )
    {
        FILETIME now;
        GetSystemTimePreciseAsFileTime( &now );
        const int64_t since_1601 = (int64_t) ( (uint64_t) now.dwHighDateTime << 32 | now.dwLowDateTime );
        *nanoseconds             = ( since_1601 - 116444736000000000 ) * 100;
        return 0;
    }
    if( which == 1 || which == 2 )
    {
        LARGE_INTEGER count;
        QueryPerformanceCounter( &count );
        const int64_t rate = ticks_per_second();
        *nanoseconds       = count.QuadPart / rate * 1000000000 + count.QuadPart % rate * 1000000000 / rate;
        return 0;
    }
    return EINVAL;
}

int32_t kl_rt_clock_resolution( int32_t which, int64_t* nanoseconds )
{
    if( which == 0 )
    {
        *nanoseconds = 100;
        return 0;
    }
    if( which == 1 || which == 2 )
    {
        const int64_t step = 1000000000 / ticks_per_second();
        *nanoseconds       = step > 0 ? step : 1;
        return 0;
    }
    return EINVAL;
}

int32_t kl_rt_program_path( uint8_t* buffer, uint64_t size, uint64_t* length )
{
    enum
    {
        k_longest = 32768 // a Windows path's limit, in UTF-16 units
    };

    wchar_t* path = malloc( k_longest * sizeof( wchar_t ) );
    if( path == NULL )
    {
        return ENOMEM;
    }

    const DWORD got = GetModuleFileNameW( NULL, path, k_longest );
    int32_t error = got == 0 || got == k_longest ? errno_of( GetLastError() ) : narrow_path_into( path, buffer, size, length );
    free( path );
    return error;
}

int32_t kl_rt_current_directory( uint8_t* buffer, uint64_t size, uint64_t* length )
{
    const DWORD needed = GetCurrentDirectoryW( 0, NULL );
    wchar_t*    path   = needed == 0 ? NULL : malloc( needed * sizeof( wchar_t ) );
    if( path == NULL )
    {
        return needed == 0 ? errno_of( GetLastError() ) : ENOMEM;
    }

    const DWORD got   = GetCurrentDirectoryW( needed, path );
    int32_t     error = got == 0 || got >= needed ? errno_of( GetLastError() ) : narrow_path_into( path, buffer, size, length );
    free( path );
    return error;
}

// Lends the value as UTF-8 until the next call; a name never set (empty, holding '=' or 0) is ENOENT.
int32_t kl_rt_environment( const uint8_t* name, uint64_t size, uint8_t** value, uint64_t* value_size )
{
    static uint8_t* lent;
    static uint64_t lent_size;

    if( size == 0 || memchr( name, '=', size ) )
    {
        return ENOENT;
    }

    int      error = 0;
    wchar_t* key   = wide( name, size, &error );
    if( key == NULL )
    {
        return error == EINVAL ? ENOENT : error;
    }

    // The value's length with its 0, so an empty value is 1 and only a missing one 0.
    const DWORD needed = GetEnvironmentVariableW( key, NULL, 0 );
    if( needed == 0 )
    {
        free( key );
        return ENOENT;
    }

    wchar_t* text = malloc( needed * sizeof( wchar_t ) );
    if( text == NULL )
    {
        free( key );
        return ENOMEM;
    }
    const DWORD got = GetEnvironmentVariableW( key, text, needed );
    free( key );
    if( got >= needed || ( got == 0 && needed > 1 ) )
    {
        free( text );
        return ENOENT; // changed between the calls
    }
    text[got] = L'\0';

    const int bytes = WideCharToMultiByte( CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL );
    if( (uint64_t) bytes > lent_size )
    {
        uint8_t* grown = realloc( lent, (size_t) bytes );
        if( grown == NULL )
        {
            free( text );
            return ENOMEM;
        }
        lent      = grown;
        lent_size = (uint64_t) bytes;
    }

    WideCharToMultiByte( CP_UTF8, 0, text, -1, (char*) lent, bytes, NULL, NULL );
    free( text );
    *value      = lent;
    *value_size = (uint64_t) bytes - 1;
    return 0;
}

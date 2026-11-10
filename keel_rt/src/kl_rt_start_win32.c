// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// What a Windows program links from the MSVC toolset and Keel's does not, since it links only the
// SDK's ucrt.lib and kernel32.lib: the entry point, the TLS directory, and what the compiler calls
// that the UCRT lacks. Built /Zl /GS-, so nothing here asks for the MSVC libraries back.

#define WIN32_LEAN_AND_MEAN
#include <corecrt_startup.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "kl_rt.h"

int  main( int argc, char** argv );
void kl_rt_find_stack( void );

// --- Thread-local storage, as the loader expects it of an image (the MSVC runtime's tlssup.c) ---

// Each thread's copy of `.tls` is found through this slot, which the loader fills.
ULONG _tls_index = 0;

#pragma section( ".tls", read, write )
#pragma section( ".tls$ZZZ", read, write )
#pragma section( ".rdata$T", read )

// `.tls$` holds every thread-local, so these bracket them.
__declspec( allocate( ".tls" ) ) char     _tls_start = 0;
__declspec( allocate( ".tls$ZZZ" ) ) char _tls_end   = 0;

// The linker points the image's TLS directory at this symbol.
__declspec( allocate( ".rdata$T" ) ) const IMAGE_TLS_DIRECTORY64 _tls_used = {
    (ULONGLONG) (uintptr_t) &_tls_start,
    (ULONGLONG) (uintptr_t) &_tls_end,
    (ULONGLONG) (uintptr_t) &_tls_index,
    0, // no callbacks
    0,
    { 0 },
};

// --- What the compiler calls ---

// Referenced by any object that uses floating point.
int _fltused = 0x9875;

// RAX bytes of stack about to be taken: touches each page in order, so the guard page grows it.
// After compiler-rt's chkstk.S; it moves no register but the flags.
__asm__( ".text\n"
         ".globl __chkstk\n"
         ".def __chkstk; .scl 2; .type 32; .endef\n"
         "__chkstk:\n"
         "    pushq %rcx\n"
         "    pushq %rax\n"
         "    cmpq $0x1000, %rax\n"
         "    leaq 24(%rsp), %rcx\n"
         "    jb 1f\n"
         "2:\n"
         "    subq $0x1000, %rcx\n"
         "    testq %rcx, (%rcx)\n"
         "    subq $0x1000, %rax\n"
         "    cmpq $0x1000, %rax\n"
         "    ja 2b\n"
         "1:\n"
         "    subq %rax, %rcx\n"
         "    testq %rcx, (%rcx)\n"
         "    popq %rax\n"
         "    popq %rcx\n"
         "    retq\n" );

// `rep movsb` and `rep stosb` written out: clang's `__stosb` is a memset, which calls this one.
static void copy_forward( void* to, const void* from, size_t size )
{
    __asm__ volatile( "rep movsb" : "+D"( to ), "+S"( from ), "+c"( size ) : : "memory" );
}

void* memcpy( void* to, const void* from, size_t size )
{
    copy_forward( to, from, size );
    return to;
}

void* memset( void* to, int value, size_t size )
{
    void* at = to;
    __asm__ volatile( "rep stosb" : "+D"( at ), "+c"( size ) : "a"( value ) : "memory" );
    return to;
}

// The loops below are no_builtin, or the optimiser could make them calls to themselves.
__attribute__( ( no_builtin ) ) void* memmove( void* to, const void* from, size_t size )
{
    unsigned char*       d = to;
    const unsigned char* s = from;
    if( d <= s || d >= s + size )
    {
        copy_forward( d, s, size ); // forward is safe unless `to` starts inside `from`
        return to;
    }

    while( size != 0 )
    {
        --size;
        d[size] = s[size];
    }
    return to;
}

__attribute__( ( no_builtin ) ) int memcmp( const void* a, const void* b, size_t size )
{
    const unsigned char* x = a;
    const unsigned char* y = b;
    for( size_t i = 0; i < size; ++i )
    {
        if( x[i] != y[i] )
        {
            return x[i] < y[i] ? -1 : 1;
        }
    }
    return 0;
}

__attribute__( ( no_builtin ) ) void* memchr( const void* data, int value, size_t size )
{
    const unsigned char* bytes = data;
    for( size_t i = 0; i < size; ++i )
    {
        if( bytes[i] == (unsigned char) value )
        {
            return (void*) ( bytes + i );
        }
    }
    return NULL;
}

// --- The entry point ---

static void
ignore_parameter( const wchar_t* expression, const wchar_t* function, const wchar_t* file, unsigned line, uintptr_t reserved )
{
}

// `main` with its arguments in UTF-8; its return ends the program through the UCRT's `exit`.
void mainCRTStartup( void )
{
    // A bad descriptor is an error the call answers, as on POSIX, rather than the program's end.
    _set_invalid_parameter_handler( ignore_parameter );
    // A crash ends the program with its code, as a signal does, rather than in a dialog.
    SetErrorMode( SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX );
    _set_app_type( _crt_console_app );

    // The bytes written are the bytes asked for: no `\r` before each `\n`.
    _setmode( 0, _O_BINARY );
    _setmode( 1, _O_BINARY );
    _setmode( 2, _O_BINARY );

    kl_rt_find_stack();

    const char* no_memory = "out of memory reading the program's arguments";
    if( _configure_wide_argv( _crt_argv_unexpanded_arguments ) != 0 )
    {
        kl_rt_panic( "", 0, no_memory );
    }

    const int       argc = *__p___argc();
    wchar_t** const wide = *__p___wargv();
    char**          argv = kl_rt_alloc_many( (size_t) argc + 1, sizeof( char* ), "", 0, no_memory );
    for( int i = 0; i < argc; ++i )
    {
        const int size = WideCharToMultiByte( CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL );
        argv[i]        = kl_rt_alloc( (size_t) size, "", 0, no_memory );
        WideCharToMultiByte( CP_UTF8, 0, wide[i], -1, argv[i], size, NULL, NULL );
    }
    argv[argc] = NULL;

    exit( main( argc, argv ) );
}

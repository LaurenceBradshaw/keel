// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// D76: a drop that can reach itself checks the stack at entry, and runs on a fresh segment when low.

#define _GNU_SOURCE
#include <pthread.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include "kl_rt.h"
#if __has_include( <valgrind/valgrind.h> )
#include <valgrind/valgrind.h>
#else
#define VALGRIND_STACK_REGISTER( start, end ) 0
#define VALGRIND_STACK_DEREGISTER( id ) ( (void) ( id ) )
#endif

enum
{
    k_margin  = 64 * 1024,  // left below the check for the drop and what it calls
    k_segment = 1024 * 1024 // each fresh segment, its guard page included
};

_Thread_local uintptr_t kl_rt_stack_low;

static _Thread_local void ( *pending_drop )( void* );
static _Thread_local void* pending_value;

// The main thread's limit, from where its stack ends.
__attribute__( ( constructor ) ) static void find_stack( void )
{
    pthread_attr_t attributes;
    void*          bottom = NULL;
    size_t         size   = 0;
    if( pthread_getattr_np( pthread_self(), &attributes ) == 0 )
    {
        if( pthread_attr_getstack( &attributes, &bottom, &size ) == 0 && size > 2 * k_margin )
        {
            kl_rt_stack_low = (uintptr_t) bottom + k_margin;
        }
        pthread_attr_destroy( &attributes );
    }
}

static void run_pending( void )
{
    pending_drop( pending_value );
}

void kl_rt_deeper( void ( *drop )( void* ), void* value )
{
    const size_t page    = (size_t) sysconf( _SC_PAGESIZE );
    char*        segment = mmap( NULL, k_segment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0 );
    if( segment == MAP_FAILED )
    {
        kl_rt_panic( "", 0, "out of memory growing the stack for a drop" );
    }
    mprotect( segment, page, PROT_NONE ); // an overflow faults rather than writing past it

    const uintptr_t outer = kl_rt_stack_low;
    const unsigned  id    = VALGRIND_STACK_REGISTER( segment, segment + k_segment );
    ucontext_t      here;
    ucontext_t      there;
    getcontext( &there );
    there.uc_stack.ss_sp   = segment;
    there.uc_stack.ss_size = k_segment;
    there.uc_link          = &here;
    makecontext( &there, run_pending, 0 );

    pending_drop    = drop;
    pending_value   = value;
    kl_rt_stack_low = (uintptr_t) segment + page + k_margin;
    swapcontext( &here, &there );

    kl_rt_stack_low = outer;
    VALGRIND_STACK_DEREGISTER( id );
    munmap( segment, k_segment );
}

#include "kl_rt.h"
#include <stdint.h>
#include <stdlib.h>

void* kl_rt_alloc( size_t size )
{
    // A zero request allocates one byte rather than asking malloc, whose answer for 0 is
    // implementation-defined and may be NULL - which is how failure is reported. Kept deliberately
    // after §12's empty-aggregate rejection made it unreachable: pinning the behaviour costs one
    // branch, and leaving it to the C implementation costs a case nobody can test.
    if( size == 0 )
    {
        size = 1;
    }

    return malloc( size );
}

void* kl_rt_alloc_many( size_t count, size_t size )
{
    if( size != 0 && count > SIZE_MAX / size )
    {
        return NULL;
    }

    return kl_rt_alloc( count * size );
}

void kl_rt_free( void* ptr )
{
    // free( NULL ) must be a no-op - C guarantees it
    free( ptr );
}

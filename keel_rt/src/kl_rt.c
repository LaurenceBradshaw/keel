#include "kl_rt.h"
#include <stdlib.h>

void* kl_rt_alloc( size_t size )
{
    // A zero request allocates one byte rather than asking malloc, whose answer for 0 is
    // implementation-defined and may be NULL - which is how failure is reported. `struct Empty { };`
    // reaches here, so this is not hypothetical.
    if( size == 0 )
    {
        size = 1;
    }

    return malloc( size );
}

void kl_rt_free( void* ptr )
{
    // free( NULL ) must be a no-op - C guarantees it
    free( ptr );
}

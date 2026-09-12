#ifndef KL_RT_H
#define KL_RT_H

#include <stddef.h>

void* kl_rt_alloc( size_t size );
void  kl_rt_free( void* ptr );

#endif // KL_RT_H

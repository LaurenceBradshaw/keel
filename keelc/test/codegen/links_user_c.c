// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The C half of links_user_c.kl: `point` matches by field order, not by name.
#include <math.h>
#include <stdint.h>

struct point
{
    int32_t x;
    int32_t y;
};

struct point midpoint( struct point a, struct point b );
double       root( double v );

struct point midpoint( struct point a, struct point b )
{
    return ( struct point ) { ( a.x + b.x ) / 2, ( a.y + b.y ) / 2 };
}

double root( double v )
{
    return sqrt( v );
}

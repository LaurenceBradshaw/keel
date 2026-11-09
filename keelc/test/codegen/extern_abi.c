// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The C half of extern_abi.kl.
#include <stdint.h>

struct Mixed
{
    int64_t a;
    double  b;
};
struct Three
{
    float x, y, z;
};
struct Tiny
{
    uint8_t a;
    int16_t b;
};
struct Four
{
    float v[4];
};
struct Big
{
    int64_t a, b, c;
};
struct Two
{
    int64_t a, b;
};

struct Mixed mixed( struct Mixed m, double scale )
{
    return ( struct Mixed ) { m.a * 2, m.b * scale };
}
struct Three three( struct Three t )
{
    return ( struct Three ) { t.z, t.y, t.x };
}
struct Tiny tiny( struct Tiny t )
{
    return ( struct Tiny ) { (uint8_t) ( t.a + 1 ), (int16_t) ( t.b - 1 ) };
}
struct Four four( struct Four f )
{
    return ( struct Four ) { { f.v[3], f.v[2], f.v[1], f.v[0] } };
}
struct Big big( struct Big b, int64_t add )
{
    return ( struct Big ) { b.a + add, b.b + add, b.c + add };
}

int64_t crowded( int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, struct Two t, int64_t f )
{
    return ( ( ( ( ( ( a * 10 + b ) * 10 + c ) * 10 + d ) * 10 + e ) * 10 + t.a ) * 10 + t.b ) * 10 + f;
}

struct Mixed apply( struct Mixed ( *f )( struct Mixed ), struct Mixed m )
{
    struct Mixed r = f( m );
    r.a += 1;
    r.b += 1.0;
    return r;
}

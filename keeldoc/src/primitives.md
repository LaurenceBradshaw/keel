Every program sees these without an import: the built-in types, `str`, `result` and the printing
functions.

# bool

`true` or `false`.

Conditions, `&&`, `||`, `==` and `!=` take one. Nothing converts to a `bool`, and `cast` makes an
integer of one.

# i8

A signed 8-bit integer, from -128 to 127.

Arithmetic on it wraps, and a constant out of its range is refused. It converts implicitly to `i16`,
`i32`, `i64`, `f32` and `f64`.

# i16

A signed 16-bit integer, from -32768 to 32767.

Arithmetic on it wraps, and a constant out of its range is refused. It converts implicitly to `i32`,
`i64`, `f32` and `f64`.

# i32

A signed 32-bit integer, from -2147483648 to 2147483647.

Arithmetic on it wraps, and a constant out of its range is refused. It converts implicitly to `i64`
and `f64`.

# i64

A signed 64-bit integer, from -9223372036854775808 to 9223372036854775807.

Arithmetic on it wraps, and a constant out of its range is refused. No other type holds all its
values, so it converts to none implicitly.

# u8

An unsigned 8-bit integer, from 0 to 255.

Arithmetic on it wraps, and a constant out of its range is refused. It converts implicitly to `i16`,
`i32`, `i64`, `u16`, `u32`, `u64`, `f32` and `f64`.

# u16

An unsigned 16-bit integer, from 0 to 65535.

Arithmetic on it wraps, and a constant out of its range is refused. It converts implicitly to `i32`,
`i64`, `u32`, `u64`, `f32` and `f64`.

# u32

An unsigned 32-bit integer, from 0 to 4294967295.

Arithmetic on it wraps, and a constant out of its range is refused. It converts implicitly to `i64`,
`u64` and `f64`.

# u64

An unsigned 64-bit integer, from 0 to 18446744073709551615.

Arithmetic on it wraps, and a constant out of its range is refused. No other type holds all its
values, so it converts to none implicitly.

# f32

A 32-bit floating-point number.

It converts implicitly to `f64`. Nothing converts it to an integer.

# f64

A 64-bit floating-point number.

No other type holds all its values, so it converts to none implicitly. Nothing converts it to an
integer.

# void

No value: what a function that yields nothing returns.

It takes no storage. As a type argument it is accepted, so `result<void, E>` succeeds with
`result::ok()`; written directly, a `void` parameter, field or global is refused.

# never

The return type of a function that does not return.

A call of one ends its block, and a `switch` arm ending in one needs no `break`. It is a return type
and nothing else: a function's, an `extern`'s or a function pointer's.

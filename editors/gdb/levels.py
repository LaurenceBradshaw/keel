# Copyright 2026 Laurence Bradshaw
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# editors/gdb/check_levels.sh's two halves. Under gdb, `trace` steps the program from `main` to
# its exit and prints each stop in its own source file as `function:line` and the named locals it
# shows. Under python3, `compare` checks one level's trace against -O0's.

import sys

def trace( out_path ):
    import gdb

    gdb.execute( "set pagination off" )
    gdb.execute( "break main" )
    gdb.execute( "run", to_string = True )
    source = gdb.selected_frame().find_sal().symtab.filename

    with open( out_path, "w" ) as out:
        while gdb.selected_inferior().pid:
            frame = gdb.selected_frame()
            sal = frame.find_sal()
            if sal.symtab is None or sal.symtab.filename != source:
                # A call into the prelude or the runtime: back to the caller. Past `main`: the end.
                step( "finish" if called_from( frame, source ) else "continue" )
                continue

            names = {}
            block = frame.block()
            while block is not None:
                for symbol in block:
                    # Without lexical blocks every local is in scope; one not yet declared holds
                    # whatever its slot held.
                    if ( symbol.is_argument or ( symbol.is_variable and symbol.line < sal.line ) ) \
                            and symbol.name not in names:
                        names[ symbol.name ] = value( symbol, frame )
                if block.function is not None:
                    break
                block = block.superblock

            locals = " ".join( f"{name}={names[ name ]}" for name in sorted( names ) )
            out.write( f"{frame.name()}:{sal.line} {locals}\n" )
            step( "step" )

def called_from( frame, source ):
    frame = frame.older()
    while frame is not None:
        symtab = frame.find_sal().symtab
        if symtab is not None and symtab.filename == source:
            return True
        frame = frame.older()
    return False

def step( command ):
    import gdb
    try:
        gdb.execute( command, to_string = True )
    except gdb.error:
        pass

def value( symbol, frame ):
    import gdb
    try:
        text = str( symbol.value( frame ) )
    except gdb.error:
        return "?"
    # Addresses differ between levels' frame layouts.
    return "?" if "0x" in text else text.replace( " ", "" )

def parse( path ):
    stops = []
    with open( path ) as f:
        for line in f:
            where, _, rest = line.rstrip( "\n" ).partition( " " )
            stops.append( ( where, dict( pair.split( "=", 1 ) for pair in rest.split() ) ) )
    return stops

# -O1's stops are -O0's in order with some skipped (a folded branch, a threaded test), and at each
# shared stop every local both show holds the same value.
def compare( base_path, level_path ):
    base = parse( base_path )
    level = parse( level_path )
    errors = []
    i = 0
    for where, names in level:
        while i < len( base ) and base[ i ][ 0 ] != where:
            i += 1
        if i == len( base ):
            errors.append( f"{where}: not an -O0 stop in that order" )
            break
        for name, text in names.items():
            expected = base[ i ][ 1 ].get( name )
            if expected is None:
                errors.append( f"{where}: {name} not shown at -O0" )
            elif text != expected:
                errors.append( f"{where}: {name} = {text}, -O0 has {expected}" )
        for name in base[ i ][ 1 ]:
            if name not in names:
                errors.append( f"{where}: {name} missing" )
        i += 1
    for error in errors[ :10 ]:
        print( error )
    return 1 if errors else 0

if __name__ == "__main__" and len( sys.argv ) == 4 and sys.argv[ 1 ] == "compare":
    sys.exit( compare( sys.argv[ 2 ], sys.argv[ 3 ] ) )

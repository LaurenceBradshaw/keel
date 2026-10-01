// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Runs `keelc --check --diagnostics=json --names` on each save, underlines what it reports, and
// colours each name by what it refers to.

'use strict';

const vscode = require( 'vscode' );
const child_process = require( 'child_process' );
const fs = require( 'fs' );
const path = require( 'path' );

const severities = {
    error: vscode.DiagnosticSeverity.Error,
    warning: vscode.DiagnosticSeverity.Warning,
    note: vscode.DiagnosticSeverity.Information,
};

const token_types = [ 'namespace', 'struct', 'class', 'enum', 'enumMember', 'typeParameter', 'parameter', 'variable', 'property', 'function', 'method' ];
const token_modifiers = [ 'global' ];
const legend = new vscode.SemanticTokensLegend( token_types, token_modifiers );

// keelc's `refers_to` as a token type and modifier bits.
const tokens_for = {
    package: [ 'namespace', 0 ],
    struct: [ 'struct', 0 ],
    class: [ 'class', 0 ],
    enum: [ 'enum', 0 ],
    variant: [ 'enumMember', 0 ],
    type_parameter: [ 'typeParameter', 0 ],
    parameter: [ 'parameter', 0 ],
    variable: [ 'variable', 0 ],
    global: [ 'variable', 1 ],
    field: [ 'property', 0 ],
    function: [ 'function', 0 ],
    method: [ 'method', 0 ],
};

// keelc's lines split on '\n' alone and its columns count bytes, so the conversion works on bytes.
function lines_of( file )
{
    let bytes;
    try
    {
        bytes = fs.readFileSync( file );
    }
    catch
    {
        return [];
    }

    const lines = [];
    let start = 0;
    for( let i = 0; i <= bytes.length; ++i )
    {
        if( i === bytes.length || bytes[i] === 0x0a )
        {
            let end = i;
            if( end > start && bytes[end - 1] === 0x0d )
            {
                --end;
            }
            lines.push( bytes.subarray( start, end ) );
            start = i + 1;
        }
    }

    return lines;
}

// A 1-based byte column on a 1-based line, as a 0-based UTF-16 one.
function utf16_col( lines, line, col )
{
    const bytes = lines[line - 1];
    if( bytes === undefined )
    {
        return 0;
    }

    return bytes.subarray( 0, Math.min( col - 1, bytes.length ) ).toString( 'utf8' ).length;
}

// The span's first line only, as the terminal renderer does: underlining a whole body helps nobody.
// A zero-length span is widened to one character so there is something to see.
function range_of( lines, record )
{
    const line = record.line - 1;
    const start = utf16_col( lines, record.line, record.col );
    const line_end = utf16_col( lines, record.line, Infinity );

    let end = record.end_line === record.line ? utf16_col( lines, record.line, record.end_col ) : line_end;
    if( end <= start )
    {
        end = start + 1;
    }

    return new vscode.Range( line, start, line, end );
}

// keelc's output as diagnostics and names per absolute path. Every loaded file has an entry in
// both, empty or not, so that fixing a file's last error clears it.
function parse( stdout, cwd, saved )
{
    const by_file = new Map();
    const names = new Map();
    const lines_cache = new Map();

    const lines_for = ( file ) =>
    {
        if( !lines_cache.has( file ) )
        {
            lines_cache.set( file, lines_of( file ) );
        }
        return lines_cache.get( file );
    };

    for( const text of stdout.split( '\n' ) )
    {
        if( text.trim() === '' )
        {
            continue;
        }

        let record;
        try
        {
            record = JSON.parse( text );
        }
        catch
        {
            continue;
        }

        if( record.kind === 'file' )
        {
            const file = path.resolve( cwd, record.path );
            if( !by_file.has( file ) )
            {
                by_file.set( file, [] );
                names.set( file, [] );
            }
            continue;
        }

        if( record.kind === 'name' )
        {
            const file = path.resolve( cwd, record.file );
            const kind = tokens_for[record.refers_to];
            if( kind && names.has( file ) )
            {
                const lines = lines_for( file );
                const start = utf16_col( lines, record.line, record.col );
                const end = utf16_col( lines, record.line, record.end_col );
                names.get( file ).push( { line: record.line - 1, start, length: end - start, type: kind[0], modifiers: kind[1] } );
            }
            continue;
        }

        if( record.kind !== 'diagnostic' )
        {
            continue;
        }

        // No span: shown at the top of the file that was saved.
        let file = saved;
        let range = new vscode.Range( 0, 0, 0, 0 );

        if( record.file !== undefined )
        {
            file = path.resolve( cwd, record.file );
            range = range_of( lines_for( file ), record );
        }

        const message = record.help ? `${record.message}\n${record.help}` : record.message;
        const diagnostic = new vscode.Diagnostic( range, message, severities[record.severity] ?? vscode.DiagnosticSeverity.Error );
        diagnostic.source = 'keelc';

        if( !by_file.has( file ) )
        {
            by_file.set( file, [] );
        }
        by_file.get( file ).push( diagnostic );
    }

    return { by_file, names };
}

function semantic_tokens( names )
{
    const builder = new vscode.SemanticTokensBuilder( legend );
    const sorted = [ ...names ].sort( ( a, b ) => a.line - b.line || a.start - b.start );
    for( const name of sorted )
    {
        builder.push( name.line, name.start, name.length, token_types.indexOf( name.type ), name.modifiers );
    }
    return builder.build();
}

function activate( context )
{
    const collection = vscode.languages.createDiagnosticCollection( 'keel' );
    const output = vscode.window.createOutputChannel( 'Keel' );
    context.subscriptions.push( collection, output );

    // One run per saved file at a time: a newer save kills the older run, and its output is dropped.
    const running = new Map();
    let warned_missing = false;

    // The latest run's names per file, and the token requests waiting on the next one. A file's names
    // are dropped when a run of it starts, so a request never gets positions from older text.
    const names = new Map();
    const waiting = new Map();
    const changed = new vscode.EventEmitter();
    context.subscriptions.push( changed );

    function answer( file )
    {
        const attempts = waiting.get( file ) ?? [];
        waiting.delete( file );
        for( const attempt of attempts )
        {
            attempt();
        }
    }

    function check( document )
    {
        if( document.languageId !== 'keel' || document.uri.scheme !== 'file' )
        {
            return;
        }

        const saved = document.uri.fsPath;
        const folder = vscode.workspace.getWorkspaceFolder( document.uri );
        const cwd = folder ? folder.uri.fsPath : path.dirname( saved );

        const config = vscode.workspace.getConfiguration( 'keel', document.uri );
        const compiler = path.resolve( cwd, config.get( 'compilerPath' ) );

        const args = [ '--check', '--diagnostics=json', '--names' ];
        for( const spec of config.get( 'packages' ) )
        {
            args.push( '--package', spec );
        }
        args.push( path.relative( cwd, saved ) );

        names.delete( saved );

        const previous = running.get( saved );
        if( previous )
        {
            previous.kill();
        }

        const child = child_process.execFile( compiler, args, { cwd }, ( error, stdout, stderr ) =>
        {
            if( running.get( saved ) !== child )
            {
                return;
            }
            running.delete( saved );

            if( error && error.code === 'ENOENT' )
            {
                if( !warned_missing )
                {
                    warned_missing = true;
                    vscode.window.showWarningMessage( `Keel: no compiler at ${compiler}; set keel.compilerPath.` );
                }
                return;
            }

            // 0 is clean and 1 is errors; anything else means keelc could not check the file at all.
            if( error && error.code !== 1 )
            {
                output.appendLine( `keelc ${args.join( ' ' )} (in ${cwd}) failed:` );
                output.appendLine( stderr.trim() || String( error ) );
                return;
            }

            const result = parse( stdout, cwd, saved );
            for( const [ file, diagnostics ] of result.by_file )
            {
                collection.set( vscode.Uri.file( file ), diagnostics );
            }
            for( const [ file, list ] of result.names )
            {
                names.set( file, list );
                answer( file );
            }
            changed.fire();
        } );

        running.set( saved, child );
    }

    // While the text is ahead of the last run, a request waits for the next one. VS Code cancels it
    // on the next edit and keeps its current tokens, moving them with the text, until then.
    const provider = {
        onDidChangeSemanticTokens: changed.event,
        provideDocumentSemanticTokens( document, cancel )
        {
            const file = document.uri.fsPath;

            return new Promise( ( resolve ) =>
            {
                const attempt = () =>
                {
                    if( cancel.isCancellationRequested )
                    {
                        resolve( null );
                    }
                    else if( !document.isDirty && names.has( file ) )
                    {
                        resolve( semantic_tokens( names.get( file ) ) );
                    }
                    else
                    {
                        if( !waiting.has( file ) )
                        {
                            waiting.set( file, [] );
                        }
                        waiting.get( file ).push( attempt );
                    }
                };

                cancel.onCancellationRequested( () => resolve( null ) );
                attempt();
            } );
        },
    };
    context.subscriptions.push( vscode.languages.registerDocumentSemanticTokensProvider( { language: 'keel' }, provider, legend ) );

    context.subscriptions.push( vscode.workspace.onDidSaveTextDocument( check ) );
    context.subscriptions.push( vscode.workspace.onDidOpenTextDocument( check ) );
    vscode.workspace.textDocuments.forEach( check );
}

function deactivate()
{
}

module.exports = { activate, deactivate };

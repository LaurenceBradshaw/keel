// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

namespace keeldoc
{

// style.css, shared by every page: light and dark from the system's preference.
inline constexpr std::string_view k_style = R"css(:root {
  --bg: #fcfcfb;
  --fg: #1d2024;
  --muted: #646b75;
  --rule: #e2e4e8;
  --code-bg: #f2f3f5;
  --accent: #2456c4;
  --kw: #8b3ab8;
  --prim: #a8501a;
  --type: #1c7556;
  --mono: ui-monospace, "SF Mono", "Cascadia Code", "JetBrains Mono", Menlo, Consolas, monospace;
  --sans: system-ui, -apple-system, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
  color-scheme: light dark;
}

@media (prefers-color-scheme: dark) {
  :root {
    --bg: #15171b;
    --fg: #e1e4e8;
    --muted: #99a0aa;
    --rule: #2a2e35;
    --code-bg: #1e2127;
    --accent: #7ea8ff;
    --kw: #cf9cf0;
    --prim: #f0a771;
    --type: #6fd2a7;
  }
}

* { box-sizing: border-box; }

body {
  margin: 0;
  background: var(--bg);
  color: var(--fg);
  font: 16px/1.6 var(--sans);
}

header, main, footer {
  max-width: 54rem;
  margin: 0 auto;
  padding: 0 16px;
}

header {
  padding-top: 1rem;
  padding-bottom: 1rem;
  border-bottom: 1px solid var(--rule);
  font-family: var(--mono);
  font-size: 0.95rem;
}

header .sep, header .here { color: var(--muted); }

main { padding-bottom: 4rem; }

footer {
  padding-top: 1rem;
  padding-bottom: 2rem;
  border-top: 1px solid var(--rule);
  color: var(--muted);
  font-size: 0.85rem;
}

a { color: var(--accent); text-decoration: none; }
a:hover { text-decoration: underline; }

h1, h2, h3 { line-height: 1.25; }
h1 { font-size: 1.9rem; margin: 2rem 0 1rem; font-family: var(--mono); font-weight: 600; }
h2 { font-size: 1.4rem; margin: 0 0 0.75rem; font-family: var(--mono); font-weight: 600; }
h3 { font-size: 1.05rem; margin: 2rem 0 0.75rem; color: var(--muted); font-weight: 600; letter-spacing: 0.02em; }

.kind {
  font-family: var(--sans);
  font-size: 0.8rem;
  font-weight: 500;
  color: var(--muted);
  text-transform: uppercase;
  letter-spacing: 0.06em;
  vertical-align: 0.15em;
}

h1 .kind, h2 .kind { margin-right: 0.4rem; }

code, pre { font-family: var(--mono); font-size: 0.9em; }

:not(pre) > code {
  background: var(--code-bg);
  padding: 0.1em 0.35em;
  border-radius: 4px;
}

pre {
  background: var(--code-bg);
  padding: 0.75rem 1rem;
  border-radius: 6px;
  overflow-x: auto;
}

.doc h4 { font-size: 1rem; margin: 1.1rem 0 0.3rem; }
.doc p, .doc ul, .doc ol { margin: 0.5rem 0; }
.doc ul, .doc ol { padding-left: 1.4rem; }

.contents {
  list-style: none;
  padding: 0;
  margin: 0 0 2rem;
  columns: 2 14rem;
}

.contents li { break-inside: avoid; }

.entry {
  border-top: 1px solid var(--rule);
  padding-top: 2rem;
  margin-top: 2.5rem;
}

.group { margin-bottom: 1.5rem; scroll-margin-top: 1rem; }

.signature {
  margin: 0.5rem 0 0;
  border-left: 3px solid var(--accent);
  white-space: pre-wrap;
  overflow-wrap: anywhere;
}

.signature .anchor {
  float: right;
  margin-left: 0.75rem;
  color: var(--muted);
  opacity: 0;
}

.group:hover .anchor, .anchor:focus { opacity: 1; }

.group .doc { margin: 0.4rem 0 0 1rem; }

.kw { color: var(--kw); }
.prim { color: var(--prim); }
a.type { color: var(--type); }

.summaries {
  display: grid;
  grid-template-columns: minmax(9rem, max-content) 1fr;
  gap: 0.35rem 1.5rem;
  margin: 0 0 1.5rem;
}

.summaries dt { white-space: nowrap; }
.summaries dd { margin: 0; }

@media (max-width: 36rem) {
  .summaries { grid-template-columns: 1fr; gap: 0.1rem; }
  .summaries dd { margin-bottom: 0.6rem; }
  h1 { font-size: 1.5rem; }
}
)css";

} // namespace keeldoc

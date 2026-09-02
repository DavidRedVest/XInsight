# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository state

This repository currently contains only the product spec (`XInsight_PRD_v4.md`, in Chinese) and a logo asset (`code.svg`). No source code, build files, or tooling exist yet — implementation has not started. Read the PRD in full before writing any code; it is the single source of truth for scope, architecture, and acceptance criteria, and section 10 ("给 Claude Code 的实现约定") is written specifically as build/architecture instructions for this file.

## What XInsight is

A macOS, Source Insight–style code reader + lightweight editor for large C/C++ codebases (embedded BSPs, A/V pipelines, bare-metal STM32 projects). The three pillars: a cursor-following ambient context pane, flexible split views (any direction, nestable), and fast code search. It is explicitly **not** an IDE — no build/debug, no autocomplete, no formatting, no multi-cursor, no symbol rename in v1 (rename is clangd-only, P1+).

The core differentiator: Source Insight's moat isn't its editor, it's a fault-tolerant parser + live symbol index that needs zero configuration and works on code that doesn't even compile. XInsight replicates that with tree-sitter as the always-on default layer, with clangd as an optional precision layer overlaid on top when `compile_commands.json` is available.

## Planned architecture (from PRD §4–5)

Dependency direction is strictly one-way downward: GUI → adapter → core. Core never depends on GUI.

- **`xinsight-core`** (C++20, zero GUI dependencies — no `#include <Q...>` anywhere in this target): holds all business logic — the dual code-intelligence engines, search, project model, navigation, context engine. Must compile and have headless tests pass **without linking Qt**. This is the architectural litmus test; do not compromise it.
- **`xinsight-qt`** (Qt 6 Widgets + QScintilla): rendering and input only. Converts user intent into calls on the core's public API. GUI code must never assemble LSP JSON, spawn `rg`/`clangd` directly, or call tree-sitter/clangd directly — everything routes through the core's single-exit APIs below.
- **Single exit points** (hard architectural rule — GUI and other core modules must go through these, never around them):
  - `CodeIntelligence` (router) — the only entry point for definitions/references/symbols/hover; routes between `TreeSitterEngine` (default, zero-config) and `ClangdProvider` (optional, precise) per the policy in PRD §5.2.
  - `SearchEngine` — the only entry point for text (ripgrep) and symbol search.
  - `NavigationEngine` — the only entry point for jump/back-forward stack.
  - `LspClient` — the only entry point for clangd communication.
  - `IUiDispatcher` — the only crossing point from core threads back to the UI thread (`post(std::function<void()>)`); core has its own I/O threads and must never block waiting on results in the UI thread.
- **Two-tier code intelligence**: tree-sitter is always-on and drives highlighting/folding/outline/workspace-symbol-index unconditionally. clangd is an optional overlay that upgrades definition/reference/symbol/hover results to `precise=true` only when configured and its TU is indexed — never a hard dependency, never blocking, never surfaces "not found" when tree-sitter has an answer. See the routing table in PRD §5.2 before touching router logic.
- **Symbol index**: sits behind an `ISymbolIndex` interface; v1 ships **only** `InMemorySymbolIndex`. Do not implement a SQLite backend — it's explicitly deferred (PRD §5.3.1, §9). Interface-level tests must be backend-agnostic so a future SQLite implementation can reuse them unchanged.
- **Config**: project-level state lives in a `.xinsight/` directory at the project root (layout, clangd config, file-inclusion overrides); global/user config lives in a user-level config dir (theme, defaults). Route all config I/O through one settings module in core — don't let modules each own their own config files.

## Non-negotiable constraints (see PRD §9 and §10)

- Every core feature (highlight, outline, jump-to-definition, references, workspace symbols, context pane) must work with **zero configuration**: no `compile_commands.json`, no clangd, code that doesn't even compile. clangd only ever upgrades results; it is never a prerequisite.
- v1 editing scope is fixed: text edit, save (`Cmd+S`), new file/folder, save-as, undo/redo, dirty marking, external-change reload prompt, multi-encoding read/write (UTF-8/GBK/GB18030/Latin-1, preserving original encoding on save). Do **not** add autocomplete, formatting, multi-cursor/column edit, macro recording, snippets, or symbol rename in v1 — these are explicitly out of scope until clangd lands (P1+).
- Editing must stay synced with parsing: every edit feeds tree-sitter's incremental `edit` API with correct byte/line/col offsets, then incrementally reparses. Highlighting reparse can run on every keystroke; symbol-index updates must be debounced or deferred to save (PRD §8.7) — don't rebuild the index on every keystroke for large files.
- No SQLite symbol-index backend in v1 (interface abstraction is required now; the second implementation is not).
- No Git integration beyond external-change detection + reload prompt.

## Build (once code exists)

Per PRD §10: CMake, C++20, top-level split into `xinsight-core` (no Qt) and `xinsight-qt` (GUI) targets, with `CMAKE_EXPORT_COMPILE_COMMANDS=ON`. CI/local builds must be able to build and test `xinsight-core` in isolation without linking Qt — this is the primary way to catch an architectural violation (a stray Qt include in core).

Core deps: `tree-sitter` + `tree-sitter-c` + `tree-sitter-cpp`, `nlohmann/json`, `reproc` (subprocess management for spawning clangd/ripgrep). GUI deps: Qt 6 + QScintilla. External tools invoked as subprocesses: `ripgrep`, optional `clangd`.

## Test priorities (PRD §10)

When implementation begins, the highest-leverage tests are: tree-sitter query symbol-extraction correctness (build a fixture library of tricky C/C++ — macros, templates, nested namespaces, K&R style, function pointers, cross-file same-name symbols — with expected symbol output), `LspClient` frame parsing, ripgrep JSON parsing, `NavigationEngine` stack behavior, `CodeIntelligence` routing/fallback logic, edit-offset correctness for incremental reparse (insert/delete/multi-line paste), and multi-encoding round-trip (GBK ↔ UTF-8).

## Milestone order (PRD §7)

Build in this order — each milestone must be runnable and self-testable before starting the next, and clangd is deliberately last so the zero-config core is de-risked first:

1. **M1**: core skeleton, `IUiDispatcher`, tree-sitter highlight/fold/outline, ripgrep text search, `ProjectModel` file-inclusion rules, basic editing loop (edit/save/dirty/undo/new/save-as/external-reload), multi-encoding read/write. No symbol index or index-linked editing yet.
2. **M2**: `ISymbolIndex` + `InMemorySymbolIndex`, background indexing with progress, jump-to-definition/references/workspace-symbol search off the index, edit→incremental-reparse→index-update loop.
3. **M3**: `ContextEngine` — the cursor-following context pane, tree-sitter-driven, with candidate ranking and drill-down.
4. **M4**: `LspClient` + `ClangdProvider`, `CodeIntelligence` routing per §5.2, precise/fast mode indicator, session restore, themes, breadcrumbs.

## Model routing guidance (from PRD §10, author's stated preference)

Architecture decisions, LSP frame parsing/concurrency, and router/index design → Opus. Most widget/feature implementation → Sonnet. Bulk renames and boilerplate → Haiku.

# AGENTS.md -- Trace Desktop (C++ / React)

> Canonical guide for any AI agent working on this codebase.
> **SELF-UPDATING:** When you make architecture changes, add new patterns, fix recurring bugs, or discover undocumented behavior -- UPDATE THIS FILE IMMEDIATELY. This file must always reflect the current state of the codebase. Outdated documentation is worse than no documentation.

---

## Project Overview

Trace is a KiCad-forked EDA tool (schematic + PCB editor) with an embedded AI chat panel. C++ desktop app (wxWidgets), React/TypeScript webview for the chat UI, Python FastAPI backend for LLM orchestration. The AI reads/writes `.trace_sch` / `.trace_pcb` intermediate files which get converted to KiCad's native `.kicad_sch` / `.kicad_pcb` s-expression format.

## Architecture

```
┌──────────────────────────────────────────────────┐
│  React Webview (resources/chat-ui/)               │
│  Single-file build (vite-plugin-singlefile)       │
│  All state in App.tsx, bridge via window.kichat   │
├──────────────────────────────────────────────────┤
│  C++ Webview Bridge (chat_webview_panel.cpp)      │
│  Platform: WebKit (macOS), WebView2 (Win), GTK    │
│  libs/kiplatform/ for platform-specific code      │
├──────────────────────────────────────────────────┤
│  C++ Chat Panel (ai_chat_panel_base.cpp)          │
│  Per-tab: own AI_BACKEND_CLIENT + AI_TOOL_EXECUTOR│
│  Edit/Undo/Regenerate callbacks + versioning      │
│  Background threads: std::async + CallAfter       │
├──────────────────────────────────────────────────┤
│  C++ Backend Client (ai_backend_client.cpp)       │
│  SSE streaming via KICAD_CURL_EASY                │
│  Parallel tool dispatch, WaitForPendingTools      │
├──────────────────────────────────────────────────┤
│  Python Backend (FastAPI + LangGraph + Celery)    │
│  Agent: pydantic-ai on AWS Bedrock (Claude)       │
│  DB: Supabase (Postgres + RLS + Auth)             │
│  Tool tracking: Redis (distributed) or in-memory  │
└──────────────────────────────────────────────────┘
```

## Directory Structure

| Directory | Purpose | Importance |
|-----------|---------|------------|
| `common/` | Shared C++ code: AI client, tool executor, auth, conversation DB, chat panels, local history, amplitude, widgets | **PRIMARY** |
| `common/auth/` | `AUTH_MANAGER` singleton: Supabase JWT, keychain storage, OAuth flows | High |
| `common/widgets/` | `ai_chat_panel_base.cpp` (4400 lines), `chat_webview_panel.cpp`, webview base | **PRIMARY** |
| `eeschema/` | Schematic editor (~824 files): `SCH_EDIT_FRAME`, `AI_CHAT_PANEL` override | High |
| `pcbnew/` | PCB editor (~1407 files): `PCB_EDIT_FRAME`, own AI chat panel | High |
| `kicad/` | Main launcher: project manager, start wizard, update manager | Medium |
| `include/` | ~497 headers: all public interfaces | High |
| `libs/kiplatform/` | Platform abstraction: webview (macOS/Win/GTK), secrets (keychain), UI | High for webview |
| `resources/chat-ui/` | React/TypeScript Vite app: the chat webview frontend | **PRIMARY** |
| `qa/` | Boost.Test test suites (eeschema, pcbnew, common, kimath) | Medium |
| `thirdparty/` | 30+ vendored deps (sentry, nlohmann_json, fmt, pybind11, usearch, onnxruntime, wordpiece, etc.) | Low |
| `trace/pcbnew/` | `trace_json_to_sexp.py`: trace -> KiCad format converter | Medium |
| `3d-viewer/`, `gerbview/`, `pcb_calculator/`, `cvpcb/`, `pagelayout_editor/` | Secondary KiCad tools | Low |

## Build System

- **CMake + Ninja**: `project( trace )`, C++20
- **Build script**: `./ayo.sh` from project root
  - `./ayo.sh --debug` -> `localhost:8000` (local backend)
  - `./ayo.sh --staging` -> `trace-staging.up.railway.app` (Railway)
  - `./ayo.sh` (no flag) -> `api.buildwithtrace.com` (production)
  - `--full` forces CMake reconfigure, `--no-install` skips install
- **Backend URL is compile-time**: `TRACE_BACKEND_URL` injected via `add_compile_definitions`. Changing it requires reconfigure.
- **Chat UI pipeline**: `npm install && npm run build` in `resources/chat-ui/` generates `chat_ui_html.h` (embedded HTML). Chat UI MUST be built BEFORE C++ build.
- **Library targets**: `kicommon` (shared DLL), `common` (static), `pcbcommon` (static, compiled with `-DPCBNEW`)
- **KIFACE architecture**: Each editor (eeschema, pcbnew) is a `.kiface` shared module loaded by the launcher

## Code Style and Naming Conventions

- **Classes**: `UPPER_CASE_WITH_UNDERSCORES` (e.g., `AI_CHAT_PANEL_BASE`, `TOOL_MANAGER`)
- **Member variables**: `m_camelCase` (e.g., `m_bridgeReady`, `m_backendUrl`)
- **Static members**: `s_camelCase` (e.g., `s_instance`, `s_fileLocks`)
- **Public methods**: `PascalCase` (e.g., `AddMessage()`, `StreamChat()`)
- **Private methods**: `camelCase` (e.g., `onBridgeMessage()`, `parseSSEEvent()`)
- **Parameters**: `aCamelCase` with 'a' prefix (e.g., `aParent`, `aMessage`, `aTabIndex`)
- **Enums**: `UPPER_CASE` for both name and values (e.g., `AI_EVENT_TYPE::TEXT_DELTA`)
- **Constants**: `UPPER_CASE` (e.g., `STREAMING_FLUSH_INTERVAL_MS`)
- **Formatting**: Spaces inside parentheses: `if( condition )`, `function( arg1, arg2 )`
- **Strings**: `wxT("string")` for wxWidgets strings, `wxString::Format()` for formatting
- **DLL export**: `KICOMMON_API` macro on public symbols from `kicommon` shared library

## File Format Pipeline (Critical)

```
AI writes .trace_sch (JSON-ish) 
  -> syncTraceToKicad() runs trace_json_to_sexp.py 
    -> Produces .kicad_sch (s-expression)
      -> Editor reloads from .kicad_sch
```

- AI reads/writes `.trace_sch` / `.trace_pcb` (machine-friendly intermediate format)
- `syncTraceToKicad()` converts via Python script (`trace/pcbnew/trace_json_to_sexp.py`)
- **Conversion is debounced** at 200ms to batch rapid edits
- **Optimistic concurrency**: `readFileWithHash` -> edit -> `writeFileIfUnchanged` (hash mismatch = conflict)
- Never skip the conversion step. Never let the AI write `.kicad_sch` directly.

## Three Versioning Systems

1. **Remote (Supabase):** `schematic_versions` table. Full file content blobs. Auth + network required. `SaveSchematicVersion`, `RestoreSchematicVersion`, `GetSchematicVersions`.
2. **Local History (`.history/`):** Git-based via `LOCAL_HISTORY` + `HISTORY_LOCK_MANAGER`. Atomic restores, backup commits, size limits, cross-process file locks. Works offline. Most mature. `saveVersionToDatabase` also commits here for offline fallback.
3. **Conversation DB (SQLite):** `~/.trace/conversations.db`. Stores `version_id` in message metadata JSON, linking messages to remote versions.

**Version ID semantics:** The `version_id` on an assistant message represents state AFTER that message's edits. To restore BEFORE message N: use version_id from message N-1.

## Multi-Tab Architecture

Each `TAB_DATA` in `m_tabs` is an independent execution unit:
- Own `conversationId` (unique per conversation, enforced by duplicate detection)
- Own `AI_BACKEND_CLIENT` + `AI_TOOL_EXECUTOR` for true parallel streaming
- Own `isStreaming` atomic, `modifiedFiles` set, `fileModifiedDuringStream` flag
- Shared `m_sessionId` (global per app launch, used for tool tracking)

**KNOWN ISSUES (to be fixed):**
- `m_lastSavedVersionId` is panel-level, NOT per-tab. Two tabs finishing near-simultaneously can cross-contaminate version IDs.
- `saveVersionToDatabase` uses panel-level `GetConversationId()` which follows the currently selected tab. If user switches tabs during stream completion, version saves to wrong conversation.
- `RestoreVersion` is globally destructive: overwrites the `.trace_sch` for ALL tabs without checking if another tab is streaming. No warning to user.
- File ownership (`claimFileOwnership`) is advisory only -- always returns true. Real protection is per-file `shared_mutex` + hash-based optimistic concurrency in `AI_TOOL_EXECUTOR`.

**SAFE aspects:**
- `s_fileLocks` (static per-file shared_mutex) serializes concurrent writes to the same file
- `writeFileIfUnchanged` detects hash conflicts -- if another tab modified the file, write fails and AI retries
- Two tabs cannot hold the same conversation (duplicate detection in `loadConversationToTab`)
- C++ webview events are gated by `aTabIndex == m_currentTabIndex` -- events only reach the webview for the active tab

## Thread Safety Rules

- All tool execution: background threads via `std::async` in `AI_BACKEND_CLIENT::processEvent`
- `WaitForPendingTools()` MUST be called before `m_isStreaming.store(false)`
- UI updates: MUST go through `CallAfter()` or `safeCallAfter()` (captures `m_panelAlive` shared_ptr)
- NEVER call blocking HTTP from the UI thread
- Each `TAB_DATA` is independent: own `AI_BACKEND_CLIENT`, `AI_TOOL_EXECUTOR`, `std::atomic<bool> isStreaming`, own `std::thread`
- `CONVERSATION_DB`: thread-safe (recursive_mutex on all methods)
- File-modifying tools need per-file exclusive locks; read-only tools can share

## Webview Bridge

### C++ -> React: `RunScriptFireAndForget()` calling `window.kichat.*`
30+ methods: `addTab`, `removeTab`, `selectTab`, `addMessage`, `appendToMessage`, `finalizeMessage`, `showStatus`, `showThinking`, `setTodos`, `setStreaming`, etc.

### React -> C++: `postMessage()` via platform channel
macOS: `window.webkit.messageHandlers.kicad.postMessage()`, Windows: `window.chrome.webview.postMessage()`, GTK: `window.external.invoke()`

### Bridge Ready Pattern
1. React sends `{ type: "send_message", content: "__BRIDGE_READY__" }` (retries 10x at 100ms)
2. `m_bridgeReady` atomic flips true
3. All `m_pendingCommands` are flushed
4. Commands sent before bridge ready are queued

## React Chat UI

- **All state centralized in App.tsx** -- no Redux/Zustand/Context
- **Single-file build**: `vite-plugin-singlefile` inlines everything into `index.html` (IIFE, not ES modules)
- **`TabState`**: messages, activityItems, turnSegments, todos, isStreaming, phaseIndicator, showPlanApproval, currentAssistantMsgId
- **`turnSegments`**: interleaved timeline of `{ type: 'activity', items[] }` and `{ type: 'text', content }` for the latest assistant message
- **INVARIANT**: `activityItems` and `turnSegments` MUST stay in sync
- **`handleShowToolCall` is a UI no-op** -- friendly status messages come from `showStatus`
- **TodoWidget**: renders inside MessageList, expands downward
- **No virtualization**: all messages in DOM (fine for typical conversation lengths)
- **No Error Boundaries**: exceptions in render crash the UI
- **14 component files**, 1 custom hook (`useBridge`), single CSS file (~2025 lines)

## Authentication

- `AUTH_MANAGER` singleton: Supabase JWT auth with platform keychain storage
- macOS: Custom URL scheme `trace://auth`, Windows: Registry URL protocol, Linux: local HTTP server
- Keychain service: `com.buildwithtrace.trace`, accounts: `auth_token`, `refresh_token`
- Auth events: `EVT_AUTH_STATE_CHANGED`, `EVT_AUTH_TOKEN_RECEIVED`

## AI Tool Executor

Client-side tools dispatched by the backend via SSE `TOOL_CALL` events:

| Tool | Writes? | Description |
|------|---------|-------------|
| `read_file` | No | Read with line numbers |
| `search_replace` | Yes | Exact string replacement (optimistic concurrency) |
| `write` | Yes | Create/overwrite file |
| `grep` | No | Regex search across files |
| `list_dir` | No | List directory |
| `take_snapshot` | No | SVG screenshot |
| `run_drc` / `run_erc` | No | Design/electrical rule checks |
| `run_annotate` | Yes | Annotate schematic symbols |
| `generate_gerbers` / `generate_drill_files` | Yes | Manufacturing files |
| `autoroute` | Yes | Cloud autorouting |
| `todo_write` / `todo_read` | No | Todo list management |
| `delete_trace_file` | Yes | Delete file (with user confirmation) |

## Analytics

`AMPLITUDE_CLIENT` singleton: background worker thread, batch threshold 5 events or 30s, thread-safe `Track()`. API key injected at compile-time.

## Testing

`qa/` directory with Boost.Test suites. Enabled by `KICAD_BUILD_QA_TESTS=ON`. Suites for eeschema, pcbnew, common, kimath, fuzzing.

## Engineering Standards

- Write scalable patterns, not quick glue. Refactor hacky code when you touch it.
- Every mutex must have a comment explaining what it protects.
- Every `std::async` dispatch must have a clear synchronization point.
- Error handling: never silently swallow failures. At minimum `wxLogError` or `wxLogWarning`.
- Follow existing patterns exactly when extending (new bridge callback, new tool, new event type).
- Always consider Windows: include `#ifdef __WXMSW__` branches (see `.cursor/rules/cross-platform-windows.mdc`).
- `EVENT_ERROR` not `ERROR` -- renamed to avoid Windows `ERROR` macro conflict.

## Common Pitfalls (ADD TO THIS LIST when you discover new ones)

- Chat UI must be npm-built BEFORE C++ build (generates `chat_ui_html.h`)
- Three file formats: AI writes `.trace_sch`, sync converts to `.kicad_sch`, editor reloads. Never skip conversion.
- Backend URL is compile-time. Changing backend requires CMake reconfigure (`./ayo.sh --full`).
- Forgetting to update both `activityItems` AND `turnSegments` causes ghost items or invisible updates
- Blocking UI thread with synchronous HTTP (RestoreVersion, SaveSchematicVersion) freezes the app
- Using assistant message's own version_id for rollback instead of the previous one
- User messages are never `isStreaming` -- guard edits using tab-level streaming state
- PCBnew's `RestoreVersion` is a no-op -- version rollback only works in eeschema
- `m_panelAlive` shared_ptr pattern prevents accessing destroyed panel from background callbacks
- Conversion debounce: multiple rapid file edits batch into single trace->kicad conversion (200ms)
- Bridge commands before `__BRIDGE_READY__` must be queued in `m_pendingCommands`
- `handleShowToolCall` must not create visible activity items -- the STATUS event already handles display
- Cross-platform: always include Windows `#ifdef` branches (see `.cursor/rules/cross-platform-windows.mdc`)
- Segfaults: never access `m_tabs[index]` without bounds checking
- Tab data can be moved/invalidated during tab close -- re-validate after async gaps
- Regenerate: if msg before assistant is not a user msg, show status "No user message found to resend" instead of silently failing
- Interleaved mode: regenerate button appears on last TEXT segment, not absolute last segment (activity segments at end don't block it)
- Local history: `saveVersionToDatabase` also commits to `.history/` for offline fallback (non-fatal on failure)
- Edit/undo with no version_id: logs a debug warning instead of silently skipping
- `switchToTab` must not replay `SetStreaming(true)` -- React clears `turnSegments` on `setStreaming(true)`, wiping live activity timeline. Only send `SetStreaming(false)` to correct stale React state.
- `inputEnabled` was global, not per-tab -- Tab A streaming disabled input for Tab B. Fixed: `switchToTab` now calls `SetInputEnabled(!tab.isStreaming)` on switch.
- `appendToMessage` text splitting at STATUS boundaries -- STATUS events insert activity segments that force TEXT_DELTA into new text segments, splitting words mid-stream. Fixed: backward search past trailing activity segments to append to last text segment.
- Research phase text streamed as plain text then retroactively collapsed -- UX confusion. Fixed: `handleShowPhaseIndicator` creates a `research` segment immediately on "Research Phase" label; `appendToMessage` appends into it.
- Vector search: `VECTOR_SEARCH_ENGINE` is a singleton with warm ONNX session. If ORT binaries are missing (`TRACE_NO_ONNXRUNTIME`), falls back to Python subprocess. First call is ~60ms (model load), subsequent ~12ms. Index files: `~/.trace/symbols.usearch` + `.meta.json`.
- ONNX Runtime pre-built binaries must be downloaded per-platform; not checked into git. See `thirdparty/onnxruntime/CMakeLists.txt` for download URLs.
- usearch index `m_indexMutex`: readers (Search) take shared_lock, writer (ReindexFiles) takes unique_lock. Never hold both locks.
- `format_sexp_value` in `trace_json_to_sexp.py`: `None` values used to serialize as empty string `''`, producing `(at x y )` which crashes KiCad's s-expression parser. Now serializes as `'0'`. All AI-facing converter functions (`convert_text`, `convert_text_box`, `convert_glabel`, `convert_hier_label`, `convert_symbol`) use `_safe_float()` to coerce coordinates/angles to numeric. Without this, AI-generated trace JSON with non-numeric angle values (None, empty string, etc.) produces invalid `.kicad_sch` files that fail to load.
- Failed conversion -> failed reload cascade: if the AI writes bad `.trace_sch` content, the trace-to-kicad conversion fails, leaving a stale/broken `.kicad_sch`. Three layers of defense prevent this from corrupting the session: (1) batch timer and stream-end handler check `WasLastConversionSuccessful()` and skip reload if conversion failed; (2) `ReloadSchematicFromFile` preserves the original filename on failure instead of resetting to `untitled.kicad_sch` via `CreateDefaultScreens()`; (3) `TAB_DATA::streamStartFilePath` captures the file path at stream start, immune to mid-stream state corruption.

## Self-Healing Protocol

1. **When you make a mistake that could recur:** Add a rule to this file, Common Pitfalls, or a `.cursor/rules/*.mdc` file.
2. **When certainty is low (<80%):** Do NOT assume. Read the source code, check the DB schema, grep for usage. Verify before acting.
3. **When you change architecture:** Update this file's Architecture section, add/modify the relevant diagrams.
4. **When you add a new tool, event type, bridge method, or component:** Update the relevant table in this file.
5. **When a build fails:** Record root cause in Common Pitfalls.
6. **When patterns of mistakes emerge:** Escalate to a `.cursor/rules/` file with `alwaysApply: true`.
7. **Never take docs at face value:** If code and docs disagree, trust the code, fix the docs.
8. **Periodically audit this file:** If you notice sections that are stale or incomplete, update them as part of your current task.
9. **Never leave low-priority issues unaddressed.** Low-priority issues cascade into high-priority ones. If an audit or review finds issues at ANY severity, fix them all. Edge cases are not optional -- they ship to production and explode. See `.cursor/rules/cascading-priority.mdc`.

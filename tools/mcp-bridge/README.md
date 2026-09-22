# Relay MCP Bridge

This process translates standard MCP tool calls into Relay's private native control protocol. It is
kept outside the engine so model SDKs, chat state and protocol upgrades never become runtime or
renderer dependencies.

## Build

From this directory:

```sh
npm install
npm run check
npm run build
```

Configure an MCP host to launch:

```text
node <relay-root>/tools/mcp-bridge/dist/index.js
```

The bridge starts `<relay-root>/build/dev/relay_demo`. Override that path with the
`RELAY_ENGINE_BINARY` environment variable when using a release or packaged build.

The bridge defaults to `--agent-stdio` headless operation. Explicitly set `RELAY_RUNTIME_MODE=editor`
to start a desktop editor whose scene commands and Vulkan screenshots share one long-running process
and swapchain. Desktop interaction still requires specific task approval for agents.

Agent calls are denied unless the host configures exact native method names in `RELAY_AGENT_GRANTS`.
For example, `RELAY_AGENT_GRANTS=runtime.status,scene.list` grants only those inspections.
`session_status`, `session_audit` and `session_request` remain available for inspection and access requests.
Tools cannot expand grants. Invalid names or wildcards revoke all access. See the repository README
for scope, revocation and trust boundaries.

## Exposed tools

Tool registrations, Zod inputs and safety annotations are generated from
`protocol/relay.protocol.json`. See `docs/protocol.md` at the repository root for the generated
77-tool reference (protocol v19; 90 native methods). Run `npm run generate` after changing the schema; `npm run check` detects drift.

Capture filenames are restricted to a single safe `.png` or `.bmp` filename and always resolve inside Relay's
`captures` directory. Capture source defaults to `vulkan`; use `deterministic` for the headless CPU
oracle. In headless mode, explicitly requesting Vulkan fails rather than silently substituting CPU
pixels. Asynchronous capture and video accept either source; an omitted source chooses Vulkan in
the live runtime or deterministic CPU in headless mode. Jobs and recordings report provenance.
Scene filenames are likewise restricted to a single `.relay.json` name and resolve inside
Relay's `scenes` directory. No MCP tool in this milestone accepts an arbitrary command or source
path.

`render_capture_async`, `render_capture_status` and `render_capture_cancel` expose bounded image
jobs. Cancellation retains submitted GPU storage until its fence completes. `video_stop` starts
background finalization; poll `video_status` until `finalizing` is false and inspect `error` and
dropped-frame counts. Recording retains its initial resolution; resize frames are counted as drops.

PNG and BMP captures require no external codec. WebM finalization uses `ffmpeg`; if it is unavailable
or lacks VP9 support, Relay reports the encoding failure and preserves the queued PNG frames for
diagnosis. Trace files are restricted to `traces/*.relay-trace.jsonl` and include the runtime's fixed
timestep and deterministic random seed.


## Editor chat workflow

Build the bridge, then a human can launch `./build/dev/relay_demo --editor` from the repository
root (`npm run editor` here also works). The Agent panel opens automatically. Fresh layouts put it
in the full-height Inspector dock; Tools > Agent workspace or Ctrl+Shift+A expands it inside the
same native window. Desktop interaction still requires specific current-task approval for agents.

Chat provides streaming conversation cards, code formatting and a rounded composer. User and
agent messages have no Copy buttons.
Enter sends, Ctrl+Enter inserts a newline; one send/stop icon and a shared model/reasoning menu
(with a supported-level slider) live inside the composer. The neutral model control shows the selected
reasoning in dimmer text and shares a compact right-aligned row with the send icon. Extra header
actions and the slider description are omitted. Follow is automatic at the bottom; a
down arrow resumes it after scrolling up. Account contains provider sign-in;
Access contains scoped grants and Allow all actions (enabled by default in the human editor); Activity contains tool results/audit exports.

### OpenAI / ChatGPT

Install a current Codex CLI (App Server 0.154.0 is tested). Account > OpenAI / ChatGPT > Sign in with
ChatGPT starts `account/login/start` with `chatgptDeviceCode`. Display the code, open the official
verification page, then wait for login completion. Cancel/reconnect/sign out and refresh are
supported. Device-code authentication may need enabling in ChatGPT security settings. Model and
reasoning selectors use `model/list`; choices are validated against that catalogue and saved.

The Node adapter spawns Codex App Server with a dedicated `.relay/openai` home, file authentication
storage and a restrictive creation mask. It never reuses the caller's Codex home/authentication.
Private directories have mode 0700; existing credential/config files are restricted to mode 0600.
Symlinks, dangling links and linked credential files are refused. `.relay` is entirely gitignored,
including auth, database, logs, preferences and backups. Do not track or share that directory.
OAuth tokens are managed/refreshed by Codex, never read into the bridge or native display/trace/audit.
`RELAY_CODEX_EXECUTABLE` selects another Codex executable if needed.

Private persisted threads under `.relay/openai` register the generated public native methods as
experimental dynamic tools and can resume after an App Server restart.
Each invocation goes through the same native authorization as MCP. Host/bridge administration is
not registered. The isolated empty workspace is read-only, with shell/exec, browser/computer,
apps, delegation and general code mode disabled. The stable code-mode tool host stays enabled
for dynamic-tool delivery. Generic built-in approval requests are denied;
all editor changes must use Relay tools. Pending scope requests interrupt generation for review.
Full Auto approval uses no client turn/tool-count budget. Stop interrupts a turn; completed native
actions remain undoable. Host-only `chat.control` with `new_chat` unsubscribes the old thread;
there is currently no New chat header button. Model/account changes require idle.
The integration follows [official App Server documentation](https://developers.openai.com/codex/app-server).

### Compatible API (advanced)

Other providers retain endpoint/model and bearer/API-key or custom-header authentication in Account.
The external bridge stores these settings in private gitignored `.relay/agent-provider.json`, using
atomic replacement and mode 0600. Blank credentials retain the current secret; explicit removal
clears it. Existing configured providers are preserved on upgrade. Saved settings take precedence
over `RELAY_CHAT_ENDPOINT`, `RELAY_CHAT_MODEL` and `RELAY_CHAT_API_KEY` environment fallback.
No credential is forwarded in the native child environment or returned in provider views. Trusted
editor submissions are transient in the mailbox and excluded from scenes/projects/traces/audits.

Limited-access compatible chat bounds rounds (8), calls (16) and total HTTP work (90 seconds).
Auto approval continues until completion or Stop. Individual HTTP requests (120 seconds with Allow all actions, otherwise 30 seconds) and response
size (256 KiB) remain bounded; redirects/credential-bearing endpoint URLs are refused. Provider
failure bodies are not echoed; configured keys are redacted and refused in tool arguments.

## Open reliability work

Phase G is not complete. Intermittent ChatGPT/App Server disconnections have been reported during
multi-minute agent work; the original live-provider root cause is not yet established. The
implemented recovery resumes the original private thread, waits for in-flight native actions,
reconciles the current scene/checkpoint and continues remaining work. Stable call IDs deduplicate
mutations, including lost replies; stale transport/turn events cannot run new actions. Three
consecutive unsuccessful recoveries stop safely; successful tool progress resets the retry budget.
Stop cancels reconnect backoff, and project switches pause the task. Recovery does not restart a
failed native editor or a restarted bridge process, and never substitutes a fresh conversation
when original-thread resume fails. Account/usage failures require user action.

Full tool JSON reaches inference; UI summaries stay bounded. Shared instructions cover local
geometry, degrees versus camera radians, preserving human work and repeated PNG visual review.
Activity > Connection diagnostics exposes only bounded lifecycle/error-category metadata; no raw
provider stderr or private messages. RPC acknowledgements allow 120 seconds. Active reasoning has
no task deadline; 30-second completion checks read metadata and the latest turn summary (not complete capture history) to reconcile missing terminal notifications, and three
failed completion checks retire an unresponsive service. Mutation deduplication storage is bounded
to 64 MiB per submission; PNG payloads are not retained in that ledger.
Never store credentials or private conversation data in tracked diagnostics.

Normal `relay_demo --editor` starts the bridge automatically and registers tools with the built-in
OpenAI agent as dynamic tools. This is separate from the standalone MCP transport for external
hosts; the editor does not expose an attachable MCP endpoint. Existing background tests validate
protocol/workflow recovery and sustained native work, not long-duration live-provider reliability. New composer/camera
desktop checks and live image-based review remain pending; the earlier live native-tool check passed.

## Background verification

`npm test` uses mock service/fetch and the real `relay_session_test_host` native fixture, covering
authentication projection, sign-in/cancel/logout, model/reasoning persistence, streamed messages,
tool authorization, pending approval and Stop. The installed-runtime test creates a fresh temporary
Codex home, reads signed-out account/model metadata and registers all tools in a private persisted thread;
it never starts a model turn or opens a browser/window. It skips only if Codex is unavailable. `app-server.test.mjs` runs the real App Server against a
private loopback Responses fixture, verifies a completed dynamic tool result, then restarts and
resumes the persisted conversation/tools. It needs local socket access but no OpenAI account.
Run `RELAY_SUSTAINED_TEST_MS=130000 node --test tests/sustained.test.mjs` for the opt-in wall-clock
native editing/PNG fixture with two injected service disconnects. It uses deterministic CPU
captures and makes no desktop or OpenAI requests. Native development/release suites should run
sequentially because they share import fixtures.
`RELAY_SESSION_TEST_HOST` and `RELAY_ENGINE_BINARY` select release targets. Native headless ImGui
coverage verifies the same account/model controls without OS input. Desktop visual checks and live
account authentication/generation are separate verification scopes.

The stable `features.code_mode_host` must remain enabled: registering dynamic tools with
`thread/start` succeeds even when this host is disabled, but the model cannot use them. Relay
explicitly enables the host while keeping general code mode, shell/exec and desktop tools disabled.
The installed-runtime regression test verifies these effective settings without generating a turn.

`editor_camera_status/set/frame` control the live editor inspection viewpoint through native
permissions, without modifying scene cameras or undo. Set uses target coordinates, yaw/pitch
(radians), distance and inspector/scene mode; frame uses an entity handle. In OpenAI chat,
`render_capture` PNG replies include image content for visual review. Only bounded PNG files
under the safe capture directory are read; no image payload enters native UI projections/audits.
Deterministic CPU capture behavior is unchanged. Camera tools fail closed in headless mode.

### Human file attachments and local media

Host-only `chat.submit` accepts an optional array of eight selected file paths and permits empty
message text when files are present. The picker is opened by a human attach-button press; drop
handling uses SDL drop events over the composer. The editor displays removable attachment chips.
The external bridge reads only those human-selected regular files, refuses final symlinks and
bounds each file to 16 MiB, each message to 32 MiB and conversation memory to 128 MiB. Copies live
in ignored `.relay/chat-files` with private permissions. Images become native provider image parts;
small UTF-8 files receive explicitly untrusted text previews. `chat_attachment_read` is an extra
bridge tool that accepts opaque attachment IDs and bounded byte ranges, never arbitrary paths.
Binary data and chunks split inside a UTF-8 character are returned losslessly as base64. This does
not add a filesystem tool to the engine MCP catalog, which remains 77 tools. Binary documents
are not automatically extracted. Attachment-read capabilities clear on new chat or sign-out.

Assistant Markdown media embeds display local PNG/JPEG images and silent WebM/MP4/MOV video with
play/pause and seeking in the editor. Decoders use background workers; video containers are read
from memory, with external protocols disabled. Rendering accepts only paths confined to captures
and chat attachments. No remote fetching or desktop application launching occurs. Optional FFmpeg
libraries enable video playback; missing support is shown explicitly. CPU-only headless coverage
checks picker/drop/submission, provider image parts, file-read boundaries, PNG/WebM decoding and
texture cleanup. The native GPU presentation and real system picker are not exercised by these
background tests.

Clicking a human or assistant media thumbnail opens a modal viewer covering the main editor viewport
with a dark translucent backdrop. Pictures fit initially, zoom around the cursor with the wheel,
and pan with left-button drag. Escape and backdrop clicks dismiss it, including the footer outside
video controls. Videos retain play/pause/seeking, even when the chat thumbnail is not visible.
The viewer blocks editor shortcuts and composer drop targets. Large image decoding is limited to
4096 pixels per side on workers, and closed images return to ordinary preview resolution. Human
attachment display projections now include confined Markdown media references with no image bytes.
Headless interaction tests cover picture opening, zoom, pan, both dismissal paths, video opening,
and playback while the inline thumbnail is not drawn.

### Composer account usage meters

The OpenAI adapter reads `account/rateLimits/read` and merges sparse `account/rateLimits/updated`
notifications, following the [App Server protocol](https://developers.openai.com/codex/app-server).
The Codex bucket is preferred over the legacy single-bucket response. Five-hour and weekly windows
are identified by explicit 300/10080-minute durations, regardless of primary/secondary ordering.
Only bounded consumed percentages enter the editor projection. Telemetry reads run without blocking
submission polling, refresh at most once a minute, and cannot interrupt editing on failure.
Account changes and sign-out clear prior values; unknown windows are not inferred as zero usage.
The UI displays "5-Hourly: N%" and "Weekly: N%" below and outside the rounded input container,
with grey fill through 80%, yellow above 80%, and red above 90%. Contrast changes at the fill
boundary, and the two outlined pills share an equal-width row. Compatible providers and missing
data display `--`.

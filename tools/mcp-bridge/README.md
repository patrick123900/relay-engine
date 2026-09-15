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

When a desktop display is available, the bridge starts `--editor-stdio` instead: the human window,
scene commands and Vulkan screenshots then share one long-running process and one swapchain. Set
`RELAY_RUNTIME_MODE=headless` for CI or `RELAY_RUNTIME_MODE=editor` to override automatic selection.

## Exposed tools

Tool registrations, Zod inputs and safety annotations are generated from
`protocol/relay.protocol.json`. See `docs/protocol.md` at the repository root for the generated
38-tool reference. Run `npm run generate` after changing the schema; `npm run check` detects drift.

Capture filenames are restricted to a single safe `.png` or `.bmp` filename and always resolve inside Relay's
`captures` directory. Capture source defaults to `vulkan`; use `deterministic` for the headless CPU
oracle. Scene filenames are likewise restricted to a single `.relay.json` name and resolve inside
Relay's `scenes` directory. No MCP tool in this milestone accepts an arbitrary command or source
path.

PNG and BMP captures require no external codec. WebM finalization uses `ffmpeg`; if it is unavailable
or lacks VP9 support, Relay reports the encoding failure and preserves the queued PNG frames for
diagnosis. Trace files are restricted to `traces/*.relay-trace.jsonl` and include the runtime's fixed
timestep and deterministic random seed.

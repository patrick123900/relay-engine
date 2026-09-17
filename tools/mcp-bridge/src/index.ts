import { McpServer } from "@modelcontextprotocol/server";
import { serveStdio } from "@modelcontextprotocol/server/stdio";
import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { fileURLToPath } from "node:url";
import path from "node:path";
import readline from "node:readline";
import {
  registerGeneratedTools,
  type GeneratedOverride,
  type JsonObject,
  type ToolResponse,
} from "./generated_protocol.js";

interface RelayResponse extends JsonObject {
  id: number;
  ok: boolean;
  result?: JsonObject;
  error?: string;
}

class RelayClient {
  readonly #child: ChildProcessWithoutNullStreams;
  readonly #pending = new Map<
    number,
    { resolve: (value: JsonObject) => void; reject: (error: Error) => void }
  >();
  #nextId = 1;

  constructor(engineBinary: string, projectRoot: string, runtimeArgument: string) {
    this.#child = spawn(engineBinary, [runtimeArgument], {
      cwd: projectRoot,
      stdio: ["pipe", "pipe", "pipe"],
    });
    this.#child.stderr.on("data", (chunk: Buffer) => process.stderr.write(`[relay] ${chunk}`));
    this.#child.once("error", (error) => this.#rejectAll(error));
    this.#child.once("exit", (code, signal) => {
      this.#rejectAll(new Error(`Relay runtime exited (${code ?? signal ?? "unknown"})`));
    });

    const lines = readline.createInterface({ input: this.#child.stdout });
    lines.on("line", (line) => {
      let message: JsonObject;
      try {
        message = JSON.parse(line) as JsonObject;
      } catch {
        process.stderr.write(`[relay] Ignoring malformed runtime response: ${line}\n`);
        return;
      }
      if (message.event === "relay.ready") return;
      const response = message as RelayResponse;
      const pending = this.#pending.get(response.id);
      if (!pending) return;
      this.#pending.delete(response.id);
      if (response.ok && response.result) pending.resolve(response.result);
      else pending.reject(new Error(response.error ?? "Relay command failed"));
    });
  }

  call(method: string, parameters: JsonObject = {}): Promise<JsonObject> {
    if (this.#child.exitCode !== null) {
      return Promise.reject(new Error("Relay runtime is not running"));
    }
    const id = this.#nextId++;
    return new Promise((resolve, reject) => {
      this.#pending.set(id, { resolve, reject });
      this.#child.stdin.write(`${JSON.stringify({ id, method, ...parameters })}\n`, (error) => {
        if (error) {
          this.#pending.delete(id);
          reject(error);
        }
      });
    });
  }

  get pendingCount(): number {
    return this.#pending.size;
  }

  close(): void {
    if (this.#child.exitCode === null) {
      this.#child.stdin.end();
      this.#child.kill("SIGTERM");
    }
  }

  #rejectAll(error: Error): void {
    for (const pending of this.#pending.values()) pending.reject(error);
    this.#pending.clear();
  }
}

const bridgeDirectory = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(bridgeDirectory, "../../..");
const defaultBinary = path.join(projectRoot, "build", "dev", "relay_demo");
const engineBinary = process.env.RELAY_ENGINE_BINARY ?? defaultBinary;
const desktopAvailable =
  process.platform === "win32" ||
  process.platform === "darwin" ||
  Boolean(process.env.WAYLAND_DISPLAY || process.env.DISPLAY);
const requestedRuntimeMode = process.env.RELAY_RUNTIME_MODE;
if (requestedRuntimeMode && requestedRuntimeMode !== "editor" && requestedRuntimeMode !== "headless") {
  throw new Error("RELAY_RUNTIME_MODE must be 'editor' or 'headless'");
}
const runtimeMode = requestedRuntimeMode ?? (desktopAvailable ? "editor" : "headless");
const runtimeArgument = runtimeMode === "editor" ? "--editor-stdio" : "--agent-stdio";
const relay = new RelayClient(engineBinary, projectRoot, runtimeArgument);
let activeGpuCaptures = 0;

function captureVulkanFrame(filename: string): Promise<JsonObject> {
  const relativePath = path.join("captures", filename);
  const absolutePath = path.join(projectRoot, relativePath);
  return new Promise((resolve, reject) => {
    activeGpuCaptures += 1;
    let finished = false;
    const markFinished = () => {
      if (finished) return;
      finished = true;
      activeGpuCaptures -= 1;
    };
    const child = spawn(engineBinary, ["--vulkan-capture", relativePath], {
      cwd: projectRoot,
      stdio: ["ignore", "pipe", "pipe"],
    });
    let standardOutput = "";
    let standardError = "";
    child.stdout.on("data", (chunk: Buffer) => {
      if (standardOutput.length < 16_384) standardOutput += chunk.toString();
    });
    child.stderr.on("data", (chunk: Buffer) => {
      if (standardError.length < 16_384) standardError += chunk.toString();
    });
    const timeout = setTimeout(() => {
      child.kill("SIGTERM");
      markFinished();
      reject(new Error("Vulkan frame capture timed out after 30 seconds"));
    }, 30_000);
    child.once("error", (error) => {
      clearTimeout(timeout);
      markFinished();
      reject(error);
    });
    child.once("exit", (code, signal) => {
      clearTimeout(timeout);
      markFinished();
      if (code === 0) resolve({ path: relativePath, absolutePath, source: "vulkan" });
      else {
        reject(
          new Error(
            `Vulkan capture exited with ${code ?? signal ?? "unknown"}: ${standardError || standardOutput}`,
          ),
        );
      }
    });
  });
}

function toolResult(value: JsonObject): ToolResponse {
  return {
    content: [{ type: "text" as const, text: JSON.stringify(value) }],
    structuredContent: value,
  };
}

function createServer(): McpServer {
  const server = new McpServer(
    { name: "relay-engine", version: "0.1.0" },
    { capabilities: { tools: {} } },
  );

  const overrides: Record<string, GeneratedOverride> = {
    render_capture: async (input) => {
      const filename = input.filename as string;
      const source = input.source as "vulkan" | "deterministic";
      if (source === "vulkan" && runtimeMode !== "editor") {
        return toolResult(await captureVulkanFrame(filename));
      }
      const result = await relay.call("render.capture", {
        path: `captures/${filename}`,
        source,
      });
      return toolResult({
        ...result,
        absolutePath: path.join(projectRoot, "captures", filename),
        source,
      });
    },
    render_capture_async: async (input) => {
      const filename = input.filename as string;
      return toolResult({
        ...(await relay.call("render.capture_async", { path: `captures/${filename}`, ...(input.source ? { source: input.source } : {}) })),
        absolutePath: path.join(projectRoot, "captures", filename),
      });
    },
    scene_save: async (input) => {
      const filename = input.filename as string;
      return toolResult({
        ...(await relay.call("scene.save", { filename })),
        absolutePath: path.join(projectRoot, "scenes", filename),
      });
    },
  };

  registerGeneratedTools(
    server,
    async (method, parameters) => toolResult(await relay.call(method, parameters)),
    overrides,
  );
  return server;
}

const handle = serveStdio(createServer, {
  onerror: (error) => process.stderr.write(`[mcp] ${error.stack ?? error.message}\n`),
});

let shuttingDown = false;
async function shutdown(): Promise<void> {
  if (shuttingDown) return;
  shuttingDown = true;
  relay.close();
  await handle.close();
}

process.once("SIGINT", () => void shutdown());
process.once("SIGTERM", () => void shutdown());
process.stdin.once("end", () => {
  setTimeout(() => {
    const drain = setInterval(() => {
      if (activeGpuCaptures === 0 && relay.pendingCount === 0) {
        clearInterval(drain);
        void shutdown();
      }
    }, 50);
  }, 250);
});

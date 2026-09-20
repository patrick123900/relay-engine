import { Attachments, attachmentMethod } from './attachments.js';
// The supported Codex App Server owns OAuth, token refresh and OpenAI transport.
// This adapter exposes only Relay tools; it never reads or returns OAuth credentials.
import { editorInstructions } from "./instructions.js";
import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { constants, fstatSync, chmodSync, existsSync, lstatSync, mkdirSync, openSync, closeSync, readFileSync, writeFileSync, renameSync, unlinkSync } from "node:fs";
import { randomBytes } from "node:crypto";
import readline from "node:readline";
import path from "node:path";
import { engineEnvironment, parameterSchema, type Method } from "./chat.js";
import type { JsonObject } from "./generated_protocol.js";
type Invoke = (method: string, parameters?: JsonObject) => Promise<JsonObject>;
interface Model { id: string; model: string; displayName: string; isDefault: boolean; defaultReasoningEffort: string; supportedReasoningEfforts: { reasoningEffort: string; description: string }[]; }
interface Control { action: string; model?: string; effort?: string; provider?: string; }
interface Message { id: string; role: string; content: string; }
export interface RpcTransport { request(method: string, params?: JsonObject): Promise<JsonObject>; reply(id: string | number, result: JsonObject): void; onMessage: (method: string, params: JsonObject, id?: string | number) => void; close(): void; diagnostics?(): JsonObject; }
function pathInfo(filename: string) {
  try { return lstatSync(filename); } catch (error) { if ((error as NodeJS.ErrnoException).code === "ENOENT") return undefined; throw error; }
}
function privateDirectory(directory: string): void {
  const info = pathInfo(directory);
  if (info && (info.isSymbolicLink() || !info.isDirectory())) throw new Error("Private OpenAI configuration path is unsafe");
  mkdirSync(directory, { recursive: true, mode: 0o700 });
  chmodSync(directory, 0o700);
}
export function captureImage(root: string, result: JsonObject): JsonObject | undefined {
  if (typeof result.path !== "string" || !/^(captures\/)?[A-Za-z0-9][A-Za-z0-9._-]*\.png$/.test(result.path)) return;
  const directory = path.join(root, "captures"); const info = pathInfo(directory);
  if (!info || info.isSymbolicLink() || !info.isDirectory()) return;
  const filename = path.join(directory, path.basename(result.path));
  let fd: number | undefined;
  try {
    fd = openSync(filename, constants.O_RDONLY | constants.O_NOFOLLOW);
    const file = fstatSync(fd);
    if (!file.isFile() || file.nlink !== 1 || file.size > 16_000_000) return;
    const bytes = readFileSync(fd);
    if (bytes.length > 16_000_000 || !bytes.subarray(0, 8).equals(Buffer.from([137,80,78,71,13,10,26,10]))) return;
    return { type: "inputImage", imageUrl: `data:image/png;base64,${bytes.toString("base64")}` };
  } catch { return; } finally { if (fd !== undefined) closeSync(fd); }
}
export class HarnessPreferences {
  readonly #directory: string;
  readonly #filename: string;
  value: { provider: string; model: string; effort: string };
  constructor(root: string, configuredCompatible = false) {
    this.#directory = path.join(root, ".relay");
    this.#filename = path.join(this.#directory, "agent-harness.json");
    this.value = { provider: configuredCompatible ? "compatible" : "openai", model: "", effort: "" };
    const info = pathInfo(this.#filename);
    if (info) {
      privateDirectory(this.#directory);
      if (info.isSymbolicLink() || !info.isFile() || info.nlink !== 1 || info.size > 4096) throw new Error("Private harness settings path is unsafe");
      const fd = openSync(this.#filename, constants.O_RDONLY | constants.O_NOFOLLOW);
      try {
        const data = JSON.parse(readFileSync(fd, "utf8")) as typeof this.value;
        if (!["openai", "compatible"].includes(data.provider) || typeof data.model !== "string" || data.model.length > 256 || typeof data.effort !== "string" || data.effort.length > 32) throw new Error();
        this.value = data;
      } catch { throw new Error("Invalid private harness settings"); } finally { closeSync(fd); }
    }
  }
  save(): void {
    privateDirectory(this.#directory);
    if (pathInfo(this.#filename)?.isSymbolicLink()) throw new Error("Private harness settings path is unsafe");
    const temporary = path.join(this.#directory, `.harness-${randomBytes(16).toString("hex")}.tmp`);
    const fd = openSync(temporary, constants.O_WRONLY | constants.O_CREAT | constants.O_EXCL | constants.O_NOFOLLOW, 0o600);
    try { writeFileSync(fd, JSON.stringify(this.value)); } finally { closeSync(fd); }
    try { renameSync(temporary, this.#filename); } finally { if (existsSync(temporary)) unlinkSync(temporary); }
  }
}
export class CodexTransport implements RpcTransport {
  readonly #child: ChildProcessWithoutNullStreams;
  readonly #pending = new Map<number, { resolve: (value: JsonObject) => void; reject: (error: Error) => void; timer: NodeJS.Timeout }>();
  #serial = 0;
  #closed = false;
  #diagnostic: JsonObject = { state: "starting", stderrBytes: 0 };
  onMessage: RpcTransport["onMessage"] = () => {};
  constructor(root: string, executable = process.env.RELAY_CODEX_EXECUTABLE ?? "codex", configurationOverrides: string[] = []) {
    const parent = path.join(root, ".relay"); privateDirectory(parent);
    const home = path.join(parent, "openai"); privateDirectory(home);
    privateDirectory(path.join(home, "workspace"));
    for (const name of ["auth.json", "config.toml"]) {
      const filename = path.join(home, name);
      const info = pathInfo(filename);
      if (info) {
        if (info.isSymbolicLink() || !info.isFile() || info.nlink !== 1) throw new Error("Private OpenAI settings path is unsafe");
        chmodSync(filename, 0o600);
      }
    }
    // Dedicated home avoids importing the host's logins, providers, MCP servers or plugins.
    const env = engineEnvironment(process.env, ""); delete env.RELAY_BRIDGE_TOKEN;
    env.CODEX_HOME = home;
    // Stable code_mode_host delivers dynamic tools to the model even with code_mode disabled.
    // Disabling the host leaves registered Relay tools unavailable at inference time.
    const overrides = [
      'cli_auth_credentials_store="file"', 'history.persistence="none"', 'web_search="disabled"',
      'features.shell_tool=false', 'features.unified_exec=false', 'features.shell_snapshot=false',
      'features.apps=false', 'features.multi_agent=false', 'agents.enabled=false',
      'features.browser_use=false', 'features.browser_use_external=false', 'features.computer_use=false',
      'features.code_mode_host=true', 'features.code_mode=false', 'features.remote_plugin=false',
      'features.skill_search=false', 'features.skill_mcp_dependency_install=false',
    ];
    const previousMask = process.umask(0o077);
    try { this.#child = spawn(executable, ["app-server", "--stdio", ...[...overrides, ...configurationOverrides].flatMap(value => ["-c", value])], { cwd: path.join(home, "workspace"), env, stdio: ["pipe", "pipe", "pipe"] }); }
    finally { process.umask(previousMask); }
    this.#diagnostic.state = "connected";
    this.#child.stdin.on("error", () => this.#disconnect("stdin_error"));
    // Record only byte counts, never provider stderr/account/conversation content.
    this.#child.stderr.on("data", (bytes: Buffer) => { this.#diagnostic.stderrBytes = Number(this.#diagnostic.stderrBytes) + bytes.length; });
    this.#child.stdout.on("end", () => this.#disconnect("stdout_end"));
    this.#child.stdout.on("error", () => this.#disconnect("stdout_error"));
    this.#child.on("error", () => this.#disconnect("process_error"));
    this.#child.on("exit", (code, signal) => {
      this.#diagnostic.exitCode = code; this.#diagnostic.signal = signal;
      this.#disconnect("process_exit");
    });
    readline.createInterface({ input: this.#child.stdout }).on("line", line => {
      if (Buffer.byteLength(line) > 16_000_000) { this.#disconnect("response_limit"); this.#child.kill("SIGTERM"); return; }
      let message: JsonObject;
      try { message = JSON.parse(line) as JsonObject; } catch { this.#diagnostic.malformedMessages = Number(this.#diagnostic.malformedMessages ?? 0) + 1; return; }
      if (typeof message.method === "string") {
        const id = typeof message.id === "string" || typeof message.id === "number" ? message.id : undefined;
        this.onMessage(message.method, (message.params ?? {}) as JsonObject, id);
      } else if (typeof message.id === "number") {
        const pending = this.#pending.get(message.id); if (!pending) return;
        this.#pending.delete(message.id); clearTimeout(pending.timer);
        if (message.error) {
          const error = message.error as JsonObject;
          this.#diagnostic.rpcErrorCode = typeof error.code === "number" ? error.code : null;
          pending.reject(new Error("OpenAI service request failed"));
        }
        else pending.resolve((message.result ?? {}) as JsonObject);
      }
    });
  }
  request(method: string, params: JsonObject = {}): Promise<JsonObject> {
    if (this.#closed) return Promise.reject(new Error("OpenAI service is unavailable. Install or update Codex CLI, then reconnect."));
    const id = ++this.#serial;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.#pending.delete(id); this.#diagnostic.timeoutMethod = method;
        reject(new Error("OpenAI service request timed out; its outcome must be checked before retrying."));
      }, 120_000);
      this.#pending.set(id, { resolve, reject, timer });
      this.#child.stdin.write(JSON.stringify({ id, method, params }) + "\n", error => { if (error) this.#disconnect("stdin_error"); });
    });
  }
  reply(id: string | number, result: JsonObject): void { if (this.#closed) return; this.#child.stdin.write(JSON.stringify({ id, result }) + "\n", error => { if (error) this.#disconnect("reply_error"); }); }
  notify(method: string): void { if (this.#closed) return; this.#child.stdin.write(JSON.stringify({ method }) + "\n"); }
  diagnostics(): JsonObject { return { ...this.#diagnostic, pendingRequests: this.#pending.size }; }
  #disconnect(reason: string): void {
    if (this.#closed) return;
    this.#closed = true; this.#diagnostic.state = "disconnected"; this.#diagnostic.reason = reason;
    for (const pending of this.#pending.values()) { clearTimeout(pending.timer); pending.reject(new Error("OpenAI service disconnected. Install or update Codex CLI, then reconnect.")); }
    this.#pending.clear(); this.onMessage("relay/disconnected", this.diagnostics());
  }
  close(): void { this.#disconnect("closed"); this.#child.stdin.end(); this.#child.kill("SIGTERM"); }
}
export class OpenAiHarness {
  readonly #root: string;
  readonly #invoke: Invoke;
  readonly #preferences: HarnessPreferences;
  readonly #factory: () => RpcTransport;
  readonly #attachments = new Attachments();
  readonly #methods: Method[];
  #transport: RpcTransport | undefined;
  #lastTransport: RpcTransport | undefined;
  #callBytes = 0;
  #watchFailures = 0;
  #initializing: Promise<void> | undefined;
  #models: Model[] = [];
  #account: JsonObject | null = null;
  #usage: JsonObject = {};
  #usageReading = false;
  #usageNextRead = 0;
  #mergeUsage(snapshot: JsonObject): void {
    if (snapshot.limitId && snapshot.limitId !== 'codex') return;
    for (const key of ['primary', 'secondary']) {
      const window = snapshot[key] as JsonObject | null | undefined;
      if (!window || typeof window !== 'object' || Array.isArray(window)) continue;
      const previous = this.#usage[key] as JsonObject | undefined;
      const merged = { ...previous };
      for (const field of ['usedPercent', 'windowDurationMins']) if (typeof window[field] === 'number' && Number.isFinite(window[field])) merged[field] = window[field];
      this.#usage[key] = merged;
    }
  }
  async refreshUsage(force = false): Promise<void> {
    const transport = this.#transport, account = this.#account;
    if (!transport || !account || account.type !== 'chatgpt' || this.#usageReading || (!force && Date.now() < this.#usageNextRead)) return;
    this.#usageReading = true; this.#usageNextRead = Date.now() + 60_000;
    try {
      const result = await transport.request('account/rateLimits/read');
      if (transport !== this.#transport || account !== this.#account) return;
      const buckets = result.rateLimitsByLimitId as JsonObject | undefined;
      const snapshot = (buckets?.codex ?? result.rateLimits) as JsonObject | undefined;
      this.#usage = {}; if (snapshot && typeof snapshot === 'object') this.#mergeUsage(snapshot);
    } catch { /* Usage telemetry failure must not interrupt editing or inference. */ }
    finally { this.#usageReading = false; }
  }
  #usageView(): JsonObject {
    if (!this.#account) return {};
    const view: JsonObject = {};
    for (const key of ['primary', 'secondary']) {
      const window = this.#usage[key] as JsonObject | undefined;
      if (typeof window?.usedPercent !== 'number') continue;
      const label = window.windowDurationMins === 300 ? 'fiveHour' : window.windowDurationMins === 10080 ? 'weekly' : '';
      if (label) view[label] = Math.min(100, Math.max(0, window.usedPercent));
    }
    return view;
  }
  #login: JsonObject | null = null;
  #status = "Sign in with ChatGPT to start a conversation.";
  #busy = false;
  #stopped = false;
  #waiting = false;
  #threadId = "";
  #turnId = "";
  #startingTurn = false;
  #successfulTools = 0;
  #completion: (() => void) | undefined;
  #messages: Message[] = [];
  #results: { scope: string; succeeded: boolean; summary: string }[] = [];
  #serial = 0;
  #toolWork: Promise<void> = Promise.resolve();
  #controls: Promise<void> = Promise.resolve();
  #closed = false;
  #disconnected = false;
  #needsResume = false;
  #retryableFailure = false;
  #cancelWait: (() => void) | undefined;
  #diagnostics: JsonObject[] = [];
  #calls = new Map<string, { signature: string; reply: JsonObject }>();
  #checkpoints: JsonObject[] = [];
  #project: unknown;
  #pollMilliseconds: number;
  constructor(root: string, invoke: Invoke, preferences: HarnessPreferences, factory?: () => RpcTransport, pollMilliseconds = 30_000) {
    this.#pollMilliseconds = Math.max(10, pollMilliseconds);
    this.#root = root; this.#invoke = invoke; this.#preferences = preferences;
    this.#factory = factory ?? (() => new CodexTransport(root));
    const schema = JSON.parse(readFileSync(path.join(root, "protocol/relay.protocol.json"), "utf8")) as { methods: Method[] };
    this.#methods = [...schema.methods.filter(method => !method.hostOnly && !method.bridgeOnly), attachmentMethod];
  }
  view(): JsonObject {
    const view: JsonObject = { status: this.#status, busy: this.#busy, messages: this.#messages.slice(-24), results: this.#results.slice(-16),
      openai: { usage: this.#usageView(), account: this.#account, login: this.#login, models: this.#models.slice(0, 100), model: this.#preferences.value.model, effort: this.#preferences.value.effort, available: !!this.#transport, diagnostics: this.#diagnostics.slice(-8), transport: (this.#transport ?? this.#lastTransport)?.diagnostics?.() ?? {} } };
    const messages = view.messages as Message[], results = view.results as JsonObject[];
    while (Buffer.byteLength(JSON.stringify(view)) > 58_000 && (messages.length || results.length)) { if (messages.length) messages.shift(); else results.shift(); }
    const catalog = (view.openai as JsonObject).models as Model[];
    while (Buffer.byteLength(JSON.stringify(view)) > 58_000 && catalog.length > 1) catalog.pop();
    return view;
  }
  async #start(): Promise<void> {
    if (this.#initializing) return this.#initializing;
    if (this.#transport) return;
    const transport = this.#factory(); this.#transport = this.#lastTransport = transport;
    transport.onMessage = (method, params, id) => {
      const event = async () => {
        if (this.#transport !== transport) return;
        try { await this.#event(method, params, id, transport); }
        catch { this.#record({ event: "event_error", method }); this.#status = "OpenAI event failed; checking conversation state."; }
      };
      if (method === "item/tool/call") this.#toolWork = this.#toolWork.then(event);
      else void event();
    };
    this.#initializing = (async () => {
      try {
        await transport.request("initialize", { clientInfo: { name: "relay_engine", title: "Relay Engine", version: "0.1.0" }, capabilities: { experimentalApi: true } });
        if (transport instanceof CodexTransport) transport.notify("initialized");
        await this.#refresh();
      } catch (error) { transport.close(); this.#transport = undefined; throw error; }
      finally { this.#initializing = undefined; }
    })();
    return this.#initializing;
  }
  async #refresh(): Promise<void> {
    const account = await this.#transport!.request("account/read", { refreshToken: false });
    const previousAccount = this.#account;
    const source = account.account as JsonObject | null;
    this.#account = source ? { type: source.type, email: typeof source.email === "string" ? source.email.slice(0, 256) : "", plan: typeof source.planType === "string" ? source.planType.slice(0, 64) : "" } : null;
    if (previousAccount?.email !== this.#account?.email || previousAccount?.type !== this.#account?.type) this.#usage = {};
    const models: Model[] = [];
    void this.refreshUsage(true);
    let cursor: string | null = null;
    do {
      const result = await this.#transport!.request("model/list", { limit: 100, includeHidden: false, ...(cursor ? { cursor } : {}) });
      for (const entry of (result.data ?? []) as Model[]) {
        if (typeof entry.model !== "string" || typeof entry.displayName !== "string") continue;
        models.push({ id: String(entry.id).slice(0, 256), model: entry.model.slice(0, 256), displayName: entry.displayName.slice(0, 256), isDefault: !!entry.isDefault,
          defaultReasoningEffort: String(entry.defaultReasoningEffort ?? "medium").slice(0, 32), supportedReasoningEfforts: (entry.supportedReasoningEfforts ?? []).slice(0, 10).map(e => ({ reasoningEffort: String(e.reasoningEffort).slice(0, 32), description: String(e.description).slice(0, 256) })) });
        if (models.length >= 100) break;
      }
      cursor = typeof result.nextCursor === "string" ? result.nextCursor : null;
    } while (cursor && models.length < 100);
    this.#models = models;
    const selected = models.find(m => m.model === this.#preferences.value.model) ?? models.find(m => m.isDefault) ?? models[0];
    if (selected) {
      this.#preferences.value.model = selected.model;
      if (!selected.supportedReasoningEfforts.some(e => e.reasoningEffort === this.#preferences.value.effort)) this.#preferences.value.effort = selected.defaultReasoningEffort;
    }
    this.#status = this.#account ? "Ready" : "Sign in with ChatGPT to start a conversation.";
  }
  control(control: Control): Promise<void> {
    this.#controls = this.#controls.then(async () => { if (!this.#closed) await this.#control(control); });
    return this.#controls;
  }
  async #control(control: Control): Promise<void> {
    try {
      if (this.#busy) throw new Error("Stop the current turn before changing account or model settings.");
      if (control.action === "new_chat") { if (this.#threadId && this.#transport) await this.#transport.request("thread/unsubscribe", { threadId: this.#threadId }); this.#threadId = ""; this.#turnId = ""; this.#needsResume = false; this.#disconnected = false; this.#calls.clear(); this.#attachments.files.clear(); this.#messages = []; this.#results = []; this.#status = this.#account ? "Ready" : "Sign in with ChatGPT to start a conversation."; return; }
      await this.#start();
      if (control.action === "signin") {
        if (this.#login) await this.#transport!.request("account/login/cancel", { loginId: this.#login.loginId });
        const result = await this.#transport!.request("account/login/start", { type: "chatgptDeviceCode" });
        const url = new URL(String(result.verificationUrl));
        if (url.protocol !== "https:" || url.hostname !== "auth.openai.com" || url.pathname !== "/codex/device" || url.search || url.hash || url.username || url.password || typeof result.userCode !== "string" || typeof result.loginId !== "string") throw new Error("Unexpected device sign-in response.");
        this.#login = { loginId: result.loginId, verificationUrl: url.href, userCode: result.userCode.slice(0, 64) };
        this.#status = "Finish sign-in in your browser using the device code.";
      } else if (control.action === "cancel_signin") {
        if (this.#login) await this.#transport!.request("account/login/cancel", { loginId: this.#login.loginId });
        this.#login = null; this.#status = "Sign-in cancelled.";
      } else if (control.action === "signout") {
        if (this.#login) await this.#transport!.request("account/login/cancel", { loginId: this.#login.loginId });
        if (this.#threadId) await this.#transport!.request("thread/unsubscribe", { threadId: this.#threadId });
        await this.#transport!.request("account/logout"); this.#login = null; this.#account = null; this.#usage = {}; this.#threadId = ""; this.#needsResume = false; this.#calls.clear(); this.#attachments.files.clear(); this.#messages = []; this.#results = []; this.#status = "Signed out.";
      } else if (control.action === "refresh") await this.#refresh();
      else if (control.action === "select") {
        const model = this.#models.find(m => m.model === control.model);
        if (!model) throw new Error("Choose an available model.");
        const effort = control.effort || model.defaultReasoningEffort;
        if (!model.supportedReasoningEfforts.some(e => e.reasoningEffort === effort)) throw new Error("Choose a supported reasoning level.");
        this.#preferences.value.model = model.model; this.#preferences.value.effort = effort; this.#preferences.save();
        this.#status = "Ready";
      }
    } catch (error) {
      this.#status = error instanceof Error ? error.message : "OpenAI setup failed.";
      if (control.action === "signin" && this.#status === "OpenAI service request failed")
        this.#status = "Device sign-in could not start. Check your network, update Codex CLI, and enable device code authentication in ChatGPT security settings.";
    }
  }
  async #event(method: string, params: JsonObject, id: string | number | undefined, transport: RpcTransport): Promise<void> {
    if (method === 'account/rateLimits/updated') {
      if (this.#account && params.rateLimits && typeof params.rateLimits === 'object') this.#mergeUsage(params.rateLimits as JsonObject);
      return;
    }
    if (method === "relay/disconnected") {
      this.#record({ event: "disconnect", ...transport.diagnostics?.() });
      this.#transport = undefined; this.#needsResume = !!this.#threadId; this.#disconnected = true;
      this.#login = null; this.#status = this.#busy ? "Reconnecting; completed scene actions are preserved." : "OpenAI service disconnected. Reconnect to continue.";
      this.#completion?.(); return;
    }
    if (method === "account/login/completed") {
      if (!this.#login || params.loginId !== this.#login.loginId) return;
      this.#login = null;
      if (params.success) await this.#refresh(); else this.#status = "Sign-in did not complete. Enable device code authentication in ChatGPT settings and try again.";
      return;
    }
    if (method === "account/updated") { if (!this.#busy) await this.#refresh(); return; }
    const matches = this.#busy && params.threadId === this.#threadId &&
      ((this.#turnId !== "" && (params.turnId === this.#turnId || (params.turn as JsonObject | undefined)?.id === this.#turnId)) ||
       (method === "turn/started" && this.#startingTurn));
    if (id !== undefined) {
      if (method !== "item/tool/call") {
        this.#transport?.reply(id, method === "item/permissions/requestApproval" ? { permissions: {}, scope: "turn" } : method === "tool/requestUserInput" ? { answers: {} } : { decision: "decline" });
        return;
      }
      const key = `${params.threadId}:${typeof params.callId === "string" ? params.callId : `${typeof id}:${id}`}`;
      const signature = JSON.stringify([params.tool, params.arguments]);
      const previous = this.#calls.get(key);
      if (previous && matches && !params.namespace) {
        transport.reply(id, previous.signature === signature ? previous.reply : { success: false, contentItems: [{ type: "inputText", text: "Tool request ID was reused with different arguments; action refused." }] });
        return;
      }
      let succeeded = false, result: JsonObject;
      const tool = this.#methods.find(m => m.tool === params.tool);
      try {
        if (!matches || this.#stopped || this.#waiting || !tool || params.namespace) throw new Error("Tool call is not authorized for the active Relay turn.");
        if (!params.arguments || typeof params.arguments !== "object" || Array.isArray(params.arguments)) throw new Error("Invalid tool arguments.");
        if (!tool.readOnly && this.#callBytes > 64 * 1024 * 1024) {
          this.#waiting = true; this.#status = "Completed-action recovery storage is full. Send a follow-up to continue; scene changes are preserved.";
          throw new Error("Recovery storage is full; this action was not executed.");
        }
        const scopes = await this.#invoke("session.status");
        if (this.#project !== undefined && scopes.project !== this.#project) {
          this.#waiting = true; this.#status = "The human changed projects. Send a follow-up for the new project.";
          throw new Error("Project changed during the task; action not executed.");
        }
        result = tool.method === attachmentMethod.method ? this.#attachments.read(params.arguments as JsonObject) : await this.#invoke(tool.method, params.arguments as JsonObject); succeeded = true; ++this.#successfulTools;
        if (tool.method === "session.request" && result.pending === true) { this.#waiting = true; this.#status = "Waiting for access approval. Review Access, then send a follow-up."; }
      } catch (error) { result = { error: error instanceof Error ? error.message : "Relay tool failed." }; }
      const summary = JSON.stringify(result);
      this.#results.push({ scope: tool?.method ?? "invalid.tool", succeeded, summary: summary.slice(0, 1200).toWellFormed() }); if (this.#results.length > 32) this.#results.shift();
      const contentItems: JsonObject[] = [{ type: "inputText", text: summary }];
      if (succeeded && tool?.method === "render.capture") { const image = captureImage(this.#root, result); if (image) contentItems.push(image); }
      const reply = { success: succeeded, contentItems };
      if (tool && !tool.readOnly) {
        // Images are large and can be captured again; keep complete native mutation results.
        const cachedReply = { success: succeeded, contentItems: contentItems.filter(item => item.type !== "inputImage") };
        this.#calls.set(key, { signature, reply: cachedReply });
        this.#callBytes += Buffer.byteLength(signature) + Buffer.byteLength(summary);
      }
      if (succeeded && tool && !tool.readOnly) {
        const checkpointResult = summary.length <= 4000 ? result : {
          ...Object.fromEntries(["entity", "root", "model", "filename", "path", "source", "job", "request", "project", "status"].filter(key => result[key] !== undefined).map(key => [key, result[key]])),
          note: "Large result preserved in conversation history and the completed-call ledger; inspect native state for details." };
        this.#checkpoints.push({ tool: params.tool, arguments: params.arguments, result: checkpointResult });
        if (this.#checkpoints.length > 128) this.#checkpoints.shift();
      }
      transport.reply(id, reply);
      if (this.#waiting) await this.#interrupt();
      return;
    }
    if (!matches) return;
    if (method === "turn/started") this.#turnId = String((params.turn as JsonObject)?.id ?? "");
    if (method === "item/agentMessage/delta") {
      const itemId = String(params.itemId); let message = this.#messages.find(m => m.id === itemId);
      if (!message) { message = { id: itemId, role: "assistant", content: "" }; this.#messages.push(message); }
      message.content = (message.content + String(params.delta ?? "")).slice(0, 12000).toWellFormed(); this.#trim();
    } else if (method === "item/completed") {
      const item = params.item as JsonObject;
      if (item?.type === "agentMessage" && typeof item.text === "string") {
        const message = this.#messages.find(m => m.id === item.id);
        if (message) message.content = item.text.slice(0, 12000).toWellFormed();
        else this.#messages.push({ id: String(item.id), role: "assistant", content: item.text.slice(0, 12000).toWellFormed() }); this.#trim();
      }
    } else if (method === "thread/tokenUsage/updated") { /* Future usage projection stays outside engine state. */ }
    else if (method === "error") {
      const error = params.error as JsonObject | undefined;
      const info = error?.codexErrorInfo;
      const category = typeof info === "string" ? info : info && typeof info === "object" ? Object.keys(info)[0] : "unknown";
      // Keep only known categories; raw error messages can contain provider/private data.
      const known = ["contextWindowExceeded", "usageLimitExceeded", "rateLimitExceeded", "httpConnectionFailed", "responseStreamConnectionFailed", "responseStreamDisconnected", "responseTooManyFailedAttempts", "serverOverloaded", "internalServerError", "unauthorized"];
      this.#retryableFailure = ["httpConnectionFailed", "responseStreamConnectionFailed", "responseStreamDisconnected", "responseTooManyFailedAttempts", "serverOverloaded", "internalServerError"].includes(String(category));
      this.#record({ event: "turn_error", category: known.includes(String(category)) ? category : "unknown", retrying: params.willRetry === true });
      this.#status = params.willRetry === true ? "OpenAI is retrying the connection; scene actions are preserved." : "OpenAI reported a turn error; checking completion.";
    }
    else if (method === "turn/completed") {
      const turn = params.turn as JsonObject;
      if (turn?.status !== "failed") this.#retryableFailure = false;
      else if (turn.error) await this.#event("error", { threadId: this.#threadId, turnId: this.#turnId, error: turn.error, willRetry: false }, undefined, transport);
      if (!this.#waiting) this.#status = turn?.status === "failed" ? "OpenAI turn failed. Check account access and retry." : this.#stopped || turn?.status === "interrupted" ? "Stopped" : "Ready";
      this.#completion?.();
    }
  }
  #trim(): void { while (this.#messages.length > 32) this.#messages.shift(); }
  #record(entry: JsonObject): void {
    this.#diagnostics.push({ time: new Date().toISOString(), ...entry });
    if (this.#diagnostics.length > 32) this.#diagnostics.shift();
  }
  async #thread(): Promise<void> {
    const options = { model: this.#preferences.value.model, cwd: path.join(this.#root, ".relay/openai/workspace"), approvalPolicy: "never", sandbox: "read-only", baseInstructions: editorInstructions };
    if (this.#threadId && this.#needsResume) {
      const result = await this.#transport!.request("thread/resume", { ...options, threadId: this.#threadId, excludeTurns: true });
      if ((result.thread as JsonObject)?.id !== this.#threadId) throw new Error("OpenAI did not resume the original conversation; completed actions were preserved.");
      this.#needsResume = false; this.#record({ event: "thread_resumed" });
    } else if (!this.#threadId) {
      const dynamicTools = this.#methods.map(m => ({ type: "function", name: m.tool, description: m.description, inputSchema: { type: "object", additionalProperties: false,
        properties: Object.fromEntries(m.params.map(p => [p.wire ?? p.name, parameterSchema(p)])), required: m.params.filter(p => p.required).map(p => p.wire ?? p.name) } }));
      const result = await this.#transport!.request("thread/start", { ...options, ephemeral: false, dynamicTools });
      if (typeof (result.thread as JsonObject)?.id !== "string") throw new Error("OpenAI returned no conversation ID.");
      this.#threadId = String((result.thread as JsonObject).id);
    }
  }
  async #watch(transport: RpcTransport): Promise<void> {
    if (!this.#busy || this.#transport !== transport || !this.#turnId) return;
    const turnId = this.#turnId;
    try {
      const metadata = await transport.request("thread/read", { threadId: this.#threadId, includeTurns: false });
      if (this.#transport !== transport || this.#turnId !== turnId || !this.#busy) return;
      if (((metadata.thread as JsonObject)?.status as JsonObject)?.type === "active") { this.#watchFailures = 0; return; }
      const result = await transport.request("thread/turns/list", { threadId: this.#threadId, limit: 1, sortDirection: "desc", itemsView: "summary" });
      if (this.#transport !== transport || !this.#busy || this.#turnId !== turnId) return;
      this.#watchFailures = 0;
      const turn = (result.data as JsonObject[] | undefined)?.find(turn => turn.id === this.#turnId);
      if (turn && ["completed", "failed", "interrupted"].includes(String(turn.status))) {
        for (const item of (turn.items ?? []) as JsonObject[]) if (item.type === "agentMessage")
          await this.#event("item/completed", { threadId: this.#threadId, turnId: this.#turnId, item }, undefined, transport);
        await this.#event("turn/completed", { threadId: this.#threadId, turn }, undefined, transport);
        this.#record({ event: "completion_reconciled" });
      }
    } catch {
      this.#record({ event: "completion_check_failed" });
      if (this.#transport === transport && ++this.#watchFailures >= 3) {
        this.#record({ event: "unresponsive_transport" });
        await this.#event("relay/disconnected", {}, undefined, transport);
        transport.close();
      }
    }
  }
  async #run(input: string, publish: () => Promise<void>, images: JsonObject[] = []): Promise<void> {
    const transport = this.#transport!;
    const complete = new Promise<void>(resolve => { this.#completion = resolve; });
    this.#turnId = ""; this.#disconnected = false; this.#retryableFailure = false; this.#watchFailures = 0;
    const scopes = await this.#invoke("session.status");
    if (this.#project !== undefined && scopes.project !== this.#project) throw new Error("The human changed projects. Send a follow-up for the new project; completed actions are preserved.");
    this.#project = scopes.project;
    if (this.#stopped || this.#closed) return;
    this.#startingTurn = true;
    try {
      const result = await transport.request("turn/start", { threadId: this.#threadId, model: this.#preferences.value.model, effort: this.#preferences.value.effort,
        input: [{ type: "text", text: `Current Relay authorization: ${JSON.stringify(scopes)}\n\n${input}` }, ...images] });
      if (typeof (result.turn as JsonObject)?.id !== "string") throw new Error("OpenAI returned no turn ID; do not repeat scene changes.");
      this.#turnId = String((result.turn as JsonObject).id);
    } catch (error) {
      if (this.#transport !== transport) { await complete; return; }
      // Notifications can prove the turn started even if its RPC acknowledgement was lost.
      if (!this.#turnId) throw error;
      this.#record({ event: "turn_acknowledgement_lost" });
      await this.#watch(transport);
    } finally { this.#startingTurn = false; }
    if (this.#stopped) await this.#interrupt();
    let checking = false;
    const poll = setInterval(() => {
      if (checking) return;
      checking = true;
      void this.#watch(transport).then(publish).catch(() => {}).finally(() => { checking = false; });
    }, this.#pollMilliseconds);
    try { await complete; await this.#toolWork; } finally { clearInterval(poll); }
  }
  async submit(message: string, publish: () => Promise<void>, attachmentPaths: string[] = []): Promise<void> {
    if (this.#busy || this.#closed) return;
    this.#stopped = false; this.#waiting = false; this.#busy = true;
    this.#calls.clear(); this.#callBytes = 0; this.#checkpoints = []; this.#project = undefined;
    try {
      const attachments = this.#attachments.prepare(this.#root, attachmentPaths);
      await this.#start();
      if (!this.#account) throw new Error("Sign in with ChatGPT first.");
      if (!this.#preferences.value.model) throw new Error("No OpenAI model available. Refresh models after signing in.");
      this.#messages.push({ id: `user-${++this.#serial}`, role: "user", content: (message.slice(0, 4000) + "\n" + attachments.display).trim().toWellFormed() }); this.#trim();
      this.#status = "Working"; await publish();
      await this.#thread();
      await this.#run(message.slice(0, 4000) + "\n" + attachments.text, publish, attachments.images);
      // Recover the original persisted conversation, never replay the original user turn.
      for (let attempt = 0; (this.#disconnected || this.#retryableFailure) && !this.#stopped && !this.#closed && !this.#waiting; ++attempt) {
        if (attempt >= 3) throw new Error("OpenAI connection could not be restored after three attempts. Completed actions are preserved; reconnect and send a follow-up.");
        await this.#toolWork; // An in-flight native mutation must settle before recovery.
        await new Promise<void>(resolve => {
          const timer = setTimeout(() => { this.#cancelWait = undefined; resolve(); }, 250 * 2 ** attempt);
          this.#cancelWait = () => { clearTimeout(timer); this.#cancelWait = undefined; resolve(); };
        });
        if (this.#stopped || this.#closed) break;
        this.#status = "Reconnecting; completed scene actions are preserved."; await publish();
        this.#record({ event: "reconnect_attempt", attempt: attempt + 1 });
        try {
          await this.#start();
          if (!this.#account) throw new Error("Sign in with ChatGPT again to resume.");
          await this.#thread();
          if (this.#stopped || this.#closed) break;
          const scopes = await this.#invoke("session.status");
          if (this.#project !== undefined && scopes.project !== this.#project) throw new Error("The human changed projects. Send a follow-up for the new project; completed actions are preserved.");
          let scene: JsonObject;
          try { scene = await this.#invoke("scene.list"); }
          catch { scene = { unavailable: "Scene inspection requires current native access. Request access if needed before mutating." }; }
          const recent = [...this.#checkpoints];
          while (Buffer.byteLength(JSON.stringify(recent)) > 128_000 && recent.length) recent.shift();
          const checkpoint = JSON.stringify(recent);
          const progress = this.#successfulTools;
          await this.#run(`Continue the original task after a service disconnect: ${message.slice(0, 4000)}\nCompleted Relay tool calls (recent checkpoint): ${checkpoint}\nCurrent scene snapshot: ${JSON.stringify(scene)}\nThe scene was preserved. FIRST inspect scene.list and affected entities/bounds to reconcile current human edits and any action whose reply was lost. Do NOT recreate existing entities or blindly repeat mutations. Continue remaining work and visual verification.`, publish);
          if (this.#successfulTools > progress) attempt = -1;
        } catch (error) {
          if (this.#transport) throw error;
          this.#disconnected = true;
        }
      }
      if (this.#stopped) this.#status = "Stopped";
    } catch (error) { this.#status = error instanceof Error ? error.message : "OpenAI conversation failed."; }
    finally { this.#busy = false; this.#completion = undefined; this.#turnId = ""; await publish(); }
  }
  async #interrupt(): Promise<void> { if (this.#threadId && this.#turnId && this.#transport) await this.#transport.request("turn/interrupt", { threadId: this.#threadId, turnId: this.#turnId }); }
  cancel(): void { this.#stopped = true; this.#cancelWait?.(); void this.#interrupt().catch(() => { this.#status = "Stop failed. Reconnect to end the session."; this.close(); }); }
  close(): void { this.#closed = true; this.#stopped = true; this.#cancelWait?.(); this.#completion?.(); this.#transport?.close(); this.#transport = undefined; }
}

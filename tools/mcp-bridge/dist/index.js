import { McpServer } from "@modelcontextprotocol/server";
import { serveStdio } from "@modelcontextprotocol/server/stdio";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import path from "node:path";
import readline from "node:readline";
import { randomBytes } from "node:crypto";
import { ChatWorkflow, engineEnvironment } from "./chat.js";
import { ProviderStore } from "./provider.js";
import { HarnessPreferences, OpenAiHarness } from "./openai.js";
import { registerGeneratedTools, } from "./generated_protocol.js";
const editorOnly = process.argv.includes("--editor");
class RelayClient {
    #child;
    #pending = new Map();
    #nextId = 1;
    #bridgeToken = randomBytes(32).toString("hex");
    constructor(engineBinary, projectRoot, runtimeArgument) {
        this.#child = spawn(engineBinary, [runtimeArgument], {
            cwd: projectRoot,
            env: engineEnvironment(process.env, this.#bridgeToken),
            stdio: ["pipe", "pipe", "pipe"],
        });
        this.#child.stderr.on("data", (chunk) => process.stderr.write(`[relay] ${chunk}`));
        this.#child.once("error", (error) => { this.#rejectAll(error); if (editorOnly)
            void shutdown(); });
        this.#child.once("exit", (code, signal) => {
            this.#rejectAll(new Error(`Relay runtime exited (${code ?? signal ?? "unknown"})`));
            if (editorOnly)
                void shutdown();
        });
        const lines = readline.createInterface({ input: this.#child.stdout });
        lines.on("line", (line) => {
            let message;
            try {
                message = JSON.parse(line);
            }
            catch {
                process.stderr.write(`[relay] Ignoring malformed runtime response: ${line}\n`);
                return;
            }
            if (message.event === "relay.ready")
                return;
            const response = message;
            const pending = this.#pending.get(response.id);
            if (!pending)
                return;
            this.#pending.delete(response.id);
            if (response.ok && response.result)
                pending.resolve(response.result);
            else
                pending.reject(new Error(response.error ?? "Relay command failed"));
        });
    }
    call(method, parameters = {}) {
        if (this.#child.exitCode !== null) {
            return Promise.reject(new Error("Relay runtime is not running"));
        }
        const id = this.#nextId++;
        return new Promise((resolve, reject) => {
            this.#pending.set(id, { resolve, reject });
            this.#child.stdin.write(`${JSON.stringify({ ...parameters, id, method })}\n`, (error) => {
                if (error) {
                    this.#pending.delete(id);
                    reject(error);
                }
            });
        });
    }
    bridge(method, parameters = {}) {
        return this.call(method, { ...parameters, bridge_token: this.#bridgeToken });
    }
    get pendingCount() {
        return this.#pending.size;
    }
    close() {
        if (this.#child.exitCode === null) {
            this.#child.stdin.end();
            this.#child.kill("SIGTERM");
        }
    }
    #rejectAll(error) {
        for (const pending of this.#pending.values())
            pending.reject(error);
        this.#pending.clear();
    }
}
const bridgeDirectory = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(bridgeDirectory, "../../..");
const defaultBinary = path.join(projectRoot, "build", "dev", "relay_demo");
const binaryArgument = process.argv.indexOf("--engine-binary");
const engineBinary = (binaryArgument >= 0 ? process.argv[binaryArgument + 1] : undefined) ?? process.env.RELAY_ENGINE_BINARY ?? defaultBinary;
const requestedRuntimeMode = process.env.RELAY_RUNTIME_MODE;
if (requestedRuntimeMode && requestedRuntimeMode !== "editor" && requestedRuntimeMode !== "headless") {
    throw new Error("RELAY_RUNTIME_MODE must be 'editor' or 'headless'");
}
const runtimeMode = editorOnly ? "editor" : requestedRuntimeMode ?? "headless";
const runtimeArgument = runtimeMode === "editor" ? "--editor-ui-stdio" : "--agent-stdio";
const provider = new ProviderStore(projectRoot, process.env);
const configuration = provider.current();
const relay = new RelayClient(engineBinary, projectRoot, runtimeArgument);
let chat = new ChatWorkflow(projectRoot, (method, parameters) => relay.call(method, parameters), configuration);
const preferences = new HarnessPreferences(projectRoot, !!configuration);
const openai = new OpenAiHarness(projectRoot, (method, parameters) => relay.call(method, parameters), preferences);
const workflow = () => preferences.value.provider === "openai" ? openai : chat;
let openaiStartup = false;
let polling = false;
let acceptingChat = true;
let setupStatus = "";
const publishChat = async () => { await relay.bridge("bridge.publish", { view: JSON.stringify({ ...workflow().view(), provider: { ...provider.view(), selected: preferences.value.provider }, setup_status: setupStatus }) }); };
const submissions = [];
let runningChat = false;
const runChat = async () => {
    if (runningChat)
        return;
    runningChat = true;
    try {
        while (acceptingChat && submissions.length)
            await workflow().submit(submissions.shift(), publishChat);
    }
    finally {
        runningChat = false;
    }
};
const chatPoll = setInterval(() => {
    if (polling || !acceptingChat)
        return;
    polling = true;
    void (async () => {
        if (editorOnly && preferences.value.provider === "openai" && !openaiStartup) {
            openaiStartup = true;
            void openai.control({ action: "refresh" });
        }
        await publishChat();
        const response = await relay.bridge("bridge.poll");
        for (const submission of (response.submissions ?? [])) {
            if (submission.control) {
                const control = submission.control;
                if (runningChat) {
                    setupStatus = "Stop the active turn before changing harness settings.";
                    continue;
                }
                if (control.action === "provider") {
                    chat.cancel();
                    openai.cancel();
                    submissions.length = 0;
                    preferences.value.provider = control.provider === "compatible" ? "compatible" : "openai";
                    try {
                        preferences.save();
                        setupStatus = "";
                    }
                    catch {
                        setupStatus = "Could not save harness preferences.";
                    }
                    if (preferences.value.provider === "openai")
                        void openai.control({ action: "refresh" });
                }
                else if (preferences.value.provider === "openai") {
                    if (control.action === "new_chat") {
                        submissions.length = 0;
                        await openai.control(control);
                    }
                    else
                        void openai.control(control).then(publishChat).catch(() => { });
                }
                else if (control.action === "new_chat") {
                    submissions.length = 0;
                    chat.close();
                    chat = new ChatWorkflow(projectRoot, (method, parameters) => relay.call(method, parameters), provider.current());
                }
            }
            else if (submission.configure) {
                if (runningChat) {
                    submission.configure.credential = "";
                    setupStatus = "Stop active chat before changing provider settings.";
                    continue;
                }
                try {
                    provider.save(submission.configure);
                    chat.close();
                    submissions.length = 0;
                    chat = new ChatWorkflow(projectRoot, (method, parameters) => relay.call(method, parameters), provider.current());
                    setupStatus = "Provider settings saved privately.";
                }
                catch {
                    setupStatus = "Provider settings could not be saved. Check endpoint, model, authentication and private file permissions.";
                }
                submission.configure.credential = "";
            }
            else if (submission.cancel) {
                submissions.length = 0;
                workflow().cancel();
            }
            else if (typeof submission.message === "string" && submissions.length < 8)
                submissions.push(submission.message);
        }
        void runChat().catch(() => { });
    })().catch(() => { })
        .finally(() => { polling = false; });
}, 500);
function toolResult(value) {
    return {
        content: [{ type: "text", text: JSON.stringify(value) }],
        structuredContent: value,
    };
}
function createServer() {
    const server = new McpServer({ name: "relay-engine", version: "0.1.0" }, { capabilities: { tools: {} } });
    const overrides = {
        render_capture: async (input) => {
            const filename = input.filename;
            const source = input.source;
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
            const filename = input.filename;
            return toolResult({
                ...(await relay.call("render.capture_async", { path: `captures/${filename}`, ...(input.source ? { source: input.source } : {}) })),
                absolutePath: path.join(projectRoot, "captures", filename),
            });
        },
        scene_save: async (input) => {
            const filename = input.filename;
            const result = await relay.call("scene.save", { filename });
            return toolResult({ ...result, absolutePath: path.resolve(projectRoot, result.path) });
        },
    };
    registerGeneratedTools(server, async (method, parameters) => toolResult(await relay.call(method, parameters)), overrides);
    return server;
}
const handle = editorOnly ? { close: async () => { } } : serveStdio(createServer, {
    onerror: (error) => process.stderr.write(`[mcp] ${error.stack ?? error.message}\n`),
});
let shuttingDown = false;
async function shutdown() {
    if (shuttingDown)
        return;
    shuttingDown = true;
    acceptingChat = false;
    clearInterval(chatPoll);
    chat.close();
    openai.close();
    relay.close();
    await handle.close();
}
process.once("SIGINT", () => void shutdown());
process.once("SIGTERM", () => void shutdown());
if (!editorOnly)
    process.stdin.once("end", () => {
        acceptingChat = false;
        clearInterval(chatPoll);
        setTimeout(() => {
            const drain = setInterval(() => {
                if (relay.pendingCount === 0) {
                    clearInterval(drain);
                    void shutdown();
                }
            }, 50);
        }, 250);
    });
//# sourceMappingURL=index.js.map
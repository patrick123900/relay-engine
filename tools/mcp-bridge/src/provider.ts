// Private provider configuration belongs to the bridge, never to scene/project data.
import { constants, closeSync, existsSync, fstatSync, fchmodSync, fsyncSync, lstatSync, mkdirSync, openSync, readFileSync, renameSync, unlinkSync, writeFileSync } from "node:fs";
import { randomBytes } from "node:crypto";
import path from "node:path";
import { chatConfiguration, type Configuration } from "./chat.js";
export interface ProviderSettings { endpoint: string; model: string; auth: "bearer" | "header" | "none"; header: string; credential: string; clear?: boolean; }
export function configuration(settings: ProviderSettings): Configuration | undefined {
  if (typeof settings.endpoint !== "string" || typeof settings.model !== "string" || typeof settings.credential !== "string" || typeof settings.header !== "string" || settings.endpoint.length > 2048 || settings.model.length > 256 || settings.credential.length > 4096 || settings.header.length > 128) throw new Error("Invalid provider settings");
  if (!settings.endpoint.trim() || !settings.model.trim()) throw new Error("Endpoint and model are required");
  const result = chatConfiguration({ RELAY_CHAT_ENDPOINT: settings.endpoint.trim(), RELAY_CHAT_MODEL: settings.model.trim(), RELAY_CHAT_API_KEY: settings.auth === "none" ? "" : settings.credential });
  if (!["bearer", "header", "none"].includes(settings.auth)) throw new Error("Unsupported authentication type");
  if (/[\r\n\x00]/.test(settings.credential)) throw new Error("Invalid authentication value");
  if (settings.auth === "header" && (!/^[A-Za-z0-9!#$%&'*+.^_`|~-]+$/.test(settings.header) || /^(host|content-length|content-type|connection|cookie|set-cookie|transfer-encoding)$/i.test(settings.header))) throw new Error("Invalid authentication header");
  if (result) result.authHeader = settings.auth === "header" ? settings.header : "Authorization";
  if (result) result.authPrefix = settings.auth === "bearer" ? "Bearer " : "";
  return result;
}
export class ProviderStore {
  readonly #directory: string;
  readonly #filename: string;
  #settings: ProviderSettings | undefined;
  constructor(root: string, env: NodeJS.ProcessEnv) {
    this.#directory = path.join(root, ".relay");
    this.#filename = path.join(this.#directory, "agent-provider.json");
    this.#checkPaths();
    if (existsSync(this.#filename)) {
      const fd = openSync(this.#filename, constants.O_RDONLY | constants.O_NOFOLLOW);
      try {
        const info = fstatSync(fd);
        if (!info.isFile() || info.nlink !== 1 || info.size > 65536) throw new Error("Unsafe provider configuration file");
        fchmodSync(fd, 0o600);
        const data = JSON.parse(readFileSync(fd, "utf8")) as ProviderSettings;
        configuration(data);
        this.#settings = data;
      } catch { throw new Error("Cannot read private provider configuration; check file format and permissions"); } finally { closeSync(fd); }
    } else {
      const initial = chatConfiguration(env);
      if (initial) this.#settings = { endpoint: initial.endpoint, model: initial.model, auth: "bearer", header: "", credential: initial.apiKey };
    }
  }
  #checkPaths(): void {
    for (const filename of [this.#directory, this.#filename]) {
      try { const info = lstatSync(filename); if (info.isSymbolicLink() || (filename === this.#filename && !info.isFile()) || (filename === this.#directory && !info.isDirectory())) throw new Error("Private provider configuration path is unsafe"); }
      catch (error) { if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error; }
    }
  }
  current(): Configuration | undefined { return this.#settings ? configuration(this.#settings) : undefined; }
  view(): Record<string, string | boolean> {
    const s = this.#settings;
    return { endpoint: s?.endpoint ?? "", model: s?.model ?? "", auth: s?.auth ?? "bearer", header: s?.header ?? "", authenticated: !!s?.credential };
  }
  save(input: ProviderSettings): void {
    const settings = { ...input, credential: input.clear || input.auth === "none" ? "" : input.credential || this.#settings?.credential || "" };
    delete settings.clear;
    configuration(settings);
    this.#checkPaths();
    mkdirSync(this.#directory, { recursive: true, mode: 0o700 });
    this.#checkPaths();
    const temporary = path.join(this.#directory, `.provider-${randomBytes(16).toString("hex")}.tmp`);
    const fd = openSync(temporary, constants.O_WRONLY | constants.O_CREAT | constants.O_EXCL | constants.O_NOFOLLOW, 0o600);
    try { writeFileSync(fd, JSON.stringify(settings), "utf8"); fsyncSync(fd); }
    catch (error) { unlinkSync(temporary); throw error; }
    finally { closeSync(fd); }
    try { this.#checkPaths(); renameSync(temporary, this.#filename); this.#settings = settings; }
    finally { if (existsSync(temporary)) unlinkSync(temporary); }
  }
}

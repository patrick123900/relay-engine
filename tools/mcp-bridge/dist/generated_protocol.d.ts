import { McpServer } from "@modelcontextprotocol/server";
export type JsonObject = Record<string, unknown>;
export type ToolResponse = any;
export type GeneratedInvoke = (method: string, parameters: JsonObject) => Promise<ToolResponse>;
export type GeneratedOverride = (input: JsonObject) => Promise<ToolResponse>;
export declare function registerGeneratedTools(server: McpServer, invoke: GeneratedInvoke, overrides?: Record<string, GeneratedOverride>): void;

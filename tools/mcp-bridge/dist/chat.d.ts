import type { JsonObject } from "./generated_protocol.js";
export interface Parameter extends JsonObject {
    name: string;
    type: string;
    wire?: string;
    required?: boolean;
}
export interface Method {
    method: string;
    tool: string;
    description: string;
    hostOnly?: boolean;
    bridgeOnly?: boolean;
    params: Parameter[];
}
export interface Configuration {
    endpoint: string;
    model: string;
    apiKey: string;
    authHeader?: string;
    authPrefix?: string;
}
type Invoke = (method: string, parameters?: JsonObject) => Promise<JsonObject>;
export declare function engineEnvironment(source: NodeJS.ProcessEnv, bridgeToken: string): NodeJS.ProcessEnv;
export declare function chatConfiguration(env: NodeJS.ProcessEnv): Configuration | undefined;
export declare function parameterSchema(parameter: Parameter): JsonObject;
export declare class ChatWorkflow {
    #private;
    constructor(root: string, invoke: Invoke, configuration: Configuration | undefined);
    view(): JsonObject;
    cancel(): void;
    close(): void;
    submit(message: string, publish: () => Promise<void>): Promise<void>;
}
export {};

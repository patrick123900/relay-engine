import type { JsonObject } from "./generated_protocol.js";
type Invoke = (method: string, parameters?: JsonObject) => Promise<JsonObject>;
interface Control {
    action: string;
    model?: string;
    effort?: string;
    provider?: string;
}
export interface RpcTransport {
    request(method: string, params?: JsonObject): Promise<JsonObject>;
    reply(id: string | number, result: JsonObject): void;
    onMessage: (method: string, params: JsonObject, id?: string | number) => void;
    close(): void;
}
export declare function captureImage(root: string, result: JsonObject): JsonObject | undefined;
export declare class HarnessPreferences {
    #private;
    value: {
        provider: string;
        model: string;
        effort: string;
    };
    constructor(root: string, configuredCompatible?: boolean);
    save(): void;
}
export declare class CodexTransport implements RpcTransport {
    #private;
    onMessage: RpcTransport["onMessage"];
    constructor(root: string, executable?: string);
    request(method: string, params?: JsonObject): Promise<JsonObject>;
    reply(id: string | number, result: JsonObject): void;
    notify(method: string): void;
    close(): void;
}
export declare class OpenAiHarness {
    #private;
    constructor(root: string, invoke: Invoke, preferences: HarnessPreferences, factory?: () => RpcTransport);
    view(): JsonObject;
    control(control: Control): Promise<void>;
    submit(message: string, publish: () => Promise<void>): Promise<void>;
    cancel(): void;
    close(): void;
}
export {};

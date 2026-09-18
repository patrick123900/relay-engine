import { type Configuration } from "./chat.js";
export interface ProviderSettings {
    endpoint: string;
    model: string;
    auth: "bearer" | "header" | "none";
    header: string;
    credential: string;
    clear?: boolean;
}
export declare function configuration(settings: ProviderSettings): Configuration | undefined;
export declare class ProviderStore {
    #private;
    constructor(root: string, env: NodeJS.ProcessEnv);
    current(): Configuration | undefined;
    view(): Record<string, string | boolean>;
    save(input: ProviderSettings): void;
}

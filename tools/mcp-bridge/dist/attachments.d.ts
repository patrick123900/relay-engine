import type { Method } from './chat.js';
import type { JsonObject } from './generated_protocol.js';
export declare const attachmentMethod: Method;
export declare class Attachments {
    readonly files: Map<string, Buffer<ArrayBufferLike>>;
    read(args: JsonObject): JsonObject;
    prepare(root: string, filenames: string[]): {
        images: JsonObject[];
        text: string;
        display: string;
    };
}

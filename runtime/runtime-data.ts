/*
 Copyright (c) 2026
 SpineRuntime data handles. RuntimeData wraps a runtime Data handle (the parsed
 skeleton data), RuntimeTextureMap maps texture ids to engine Texture2D.
 Kept in a standalone module to avoid a circular import between the Skeleton
 component and the SkeletonData asset.
*/

import { Texture2D } from 'cc';
import { spine } from './bindings';

/**
 * @en SpineRuntime driven skeleton data (backed by a runtime Data handle).
 * @zh SpineRuntime 驱动的骨骼数据（对应 runtime Data 句柄）。
 */
export class RuntimeData {
    private _handle = 0;
    private _width = 0;
    private _height = 0;
    private _x = 0;
    private _y = 0;
    private _disposed = false;

    private constructor (createHandle: () => number, scale: number) {
        const handle = createHandle();
        if (!handle) return;
        this._handle = handle;
        // spine-cpp applies SkeletonJson.scale to bones and attachments but
        // leaves the top-level skeleton bounds metadata unscaled. Keep the
        // Cocos UITransform in the same coordinate space as rendered vertices.
        this._width = spine().dataWidth(handle) * scale;
        this._height = spine().dataHeight(handle) * scale;
        this._x = spine().dataX(handle) * scale;
        this._y = spine().dataY(handle) * scale;
    }

    /**
     * @en Builds runtime data from a parsed skeleton JSON string.
     * @zh 从骨骼 JSON 字符串构建运行时数据。
     */
    static fromJson (json: string, atlas: string, textures: string[], scale: number): RuntimeData {
        return new RuntimeData(() => spine().createDataJson(json, atlas, textures, scale), scale);
    }

    /**
     * @en Builds runtime data from a binary (.skel) skeleton buffer.
     * @zh 从二进制（.skel）骨骼数据构建运行时数据。
     */
    static fromBinary (bytes: Uint8Array, atlas: string, textures: string[], scale: number): RuntimeData {
        return new RuntimeData(() => spine().createDataBinary(bytes, atlas, textures, scale), scale);
    }

    get handle (): number { return this._handle; }
    get width (): number { return this._width; }
    get height (): number { return this._height; }
    get x (): number { return this._x; }
    get y (): number { return this._y; }
    get valid (): boolean { return this._handle !== 0 && !this._disposed; }

    get animations (): string[] {
        if (!this.valid) return [];
        const count = spine().dataAnimationCount(this._handle);
        const out: string[] = [];
        for (let i = 0; i < count; i++) out.push(spine().dataAnimationName(this._handle, i));
        return out;
    }

    hasAnimation (name: string): boolean {
        return this.valid && spine().dataHasAnimation(this._handle, name);
    }

    dispose (): void {
        if (this.valid) {
            spine().disposeData(this._handle);
            this._disposed = true;
        }
    }
}

/**
 * @en Maps texture pages to engine Texture2D (textureId is the atlas page index).
 * @zh 纹理页到引擎 Texture2D 的映射（textureId 是图集页索引）。
 */
export class RuntimeTextureMap {
    private _map = new Map<number, Texture2D>();

    set (textureId: number, texture: Texture2D): void {
        this._map.set(textureId, texture);
    }

    get (textureId: number): Texture2D | null {
        return this._map.get(textureId) ?? null;
    }

    has (texture: Texture2D): boolean {
        for (const t of this._map.values()) {
            if (t === texture) return true;
        }
        return false;
    }

    entries (): IterableIterator<[number, Texture2D]> {
        return this._map.entries();
    }

    clear (): void {
        this._map.clear();
    }
}

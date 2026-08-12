/*
 Copyright (c) 2026
 Spine SpineRuntime driven skeleton data asset.
 Holds the raw skeleton inputs (JSON text, atlas text, textures and the atlas
 image names) that Skeleton consumes. textureNames and textures are
 index-aligned: the runtime matches atlas page names against textureNames, and
 the renderer maps textureId (= the index) back to the matching Texture2D.
*/

import { CCString, Enum, error, js } from 'cc';
import { _decorator } from 'cc';
const { ccclass, property } = _decorator;
import { Asset, Layers, Texture2D } from 'cc';
import { Node } from 'cc';
import type { Component } from 'cc';
import { RuntimeData } from './runtime-data';
import { RuntimeSkeletonData } from './runtime-objects';
import { spine } from './bindings';

// The AssetDB worker may register a minimal placeholder before project scripts
// are evaluated. Hand its CID over to the real class instead of asking ccclass
// to register the same name twice. The structural check also handles a
// placeholder created by an older extension version that has no marker.
const existingSpineDataClass = js.getClassByName('sp.spineData') as any;
if (existingSpineDataClass
    && (existingSpineDataClass.__spineRuntimeAssetDbPlaceholder
        || typeof existingSpineDataClass.prototype?.getRuntimeData !== 'function')) {
    js.unregisterClass(existingSpineDataClass);
}

/**
 * @en The skeleton data of spine (SpineRuntime driven).
 * @zh Spine 的骨骼数据（runtime 驱动）。
 * @class sp.spineData
 * @extends Asset
 */
@ccclass('sp.spineData')
export class SpineData extends Asset {
    /**
     * @internal
     * @en The sp.spine component class, registered by spine.ts after module init.
     * Kept plugin-local so asset→node creation (createNode) does not need the
     * engine's private `cc.internal` namespace.
     * @zh sp.spine 组件类，由 spine.ts 在模块初始化后注册。
     */
    public static _spineComponent: (new () => Component & { spineData: SpineData | null }) | null = null;

    /**
     * @en Atlas text description.
     * @zh Atlas 文本描述。
     */
    @property({ visible: false })
    protected _atlasText = '';

    get atlasText (): string {
        return this._atlasText;
    }
    set atlasText (value: string) {
        if (this._atlasText === value) return;
        this._atlasText = value;
        this.reset();
    }

    /**
     * @en A string parsed from the skeleton JSON.
     * @zh 从骨骼 JSON 解析出的字符串。
     */
    @property({ visible: false })
    protected _skeletonJsonStr = '';

    get skeletonJsonStr (): string {
        return this._skeletonJsonStr;
    }
    set skeletonJsonStr (value: string) {
        if (this._skeletonJsonStr === value) return;
        this._skeletonJsonStr = value;
        this.reset();
    }

    /**
     * @en The parsed skeleton JSON object. Accepts a string or an object;
     * strings are stored and re-exposed as the parsed object.
     * @zh 解析后的骨骼 JSON 对象。接受字符串或对象；字符串会保存并转成解析对象。
     */
    protected _skeletonJson: any = null;

    get skeletonJson (): any {
        return this._skeletonJson;
    }
    set skeletonJson (value: any) {
        if (typeof value === 'string') {
            this._skeletonJsonStr = value;
            try {
                this._skeletonJson = JSON.parse(value);
            } catch (e) {
                this._skeletonJson = null;
                error(`[SpineData] invalid skeleton JSON: ${e}`);
            }
        } else {
            this._skeletonJson = value;
            this._skeletonJsonStr = value ? JSON.stringify(value) : '';
        }
        this.reset();
    }

    /**
     * @en Texture array, index-aligned with textureNames.
     * @zh 纹理数组，与 textureNames 按下标对齐。
     */
    @property({ type: [Texture2D] })
    public textures: Texture2D[] = [];

    /**
     * @en Atlas image names referenced by the atlas, index-aligned with textures.
     * @zh 图集引用的图片名，与 textures 按下标对齐。
     */
    @property({ type: [CCString] })
    public textureNames: string[] = [];

    /**
     * @en Scale applied to bone positions and image sizes.
     * @zh 应用到骨骼位置和图像大小的缩放。
     */
    @property
    public scale = 1;

    // Lazily parsed data handle used for skin/animation enumeration.
    protected _data: RuntimeData | null = null;
    protected _skinsEnum: { [key: string]: number } | null = null;
    protected _animsEnum: { [key: string]: number } | null = null;

    /**
     * @en Gets the runtime data used by the spine runtime. Returns the parsed
     * data handle, creating it lazily if needed.
     * @zh 获取 spine 运行时使用的数据。返回解析后的数据句柄，按需懒创建。
     * @param quiet @en If false, a message is printed when an error occurs.
     * @zh 值为 false 时，当发生错误时将打印出反馈信息。
     */
    public getRuntimeData (quiet?: boolean): RuntimeSkeletonData | null {
        const data = this._getParsedData(quiet);
        return data ? new RuntimeSkeletonData(data.handle) : null;
    }

    protected _getParsedData (quiet?: boolean): RuntimeData | null {
        if (this._data) return this._data;
        if (this.isEmpty()) return null;
        const names = this.textureNames.length > 0
            ? this.textureNames
            : this.textures.map((t) => t.name);
        this._data = new RuntimeData(this.skeletonJsonStr, this._atlasText, names, this.scale);
        if (!this._data.valid) {
            this._data = null;
            if (!quiet) error(`${this.name} failed to parse skeleton data!`);
            return null;
        }
        return this._data;
    }

    /**
     * @en Resets cached runtime data and enum caches.
     * @zh 重置缓存的运行时数据和枚举。
     */
    public reset (): void {
        if (this._data) {
            this._data.dispose();
            this._data = null;
        }
        this._skinsEnum = null;
        this._animsEnum = null;
    }

    /**
     * @internal
     * @en Reset skin and animation enumeration.
     * @zh 重置皮肤和动画枚举。
     */
    public resetEnums (): void {
        this._skinsEnum = null;
        this._animsEnum = null;
    }

    /**
     * @internal Since v3.7.2 this is an engine private function, only works in editor.
     * @en Gets the skin name enumeration for editor dropdowns.
     * @zh 获取皮肤名称枚举，供编辑器下拉框使用。
     */
    public getSkinsEnum (): { [key: string]: number } | null {
        if (this._skinsEnum) return this._skinsEnum;
        try {
            const data = this._getParsedData(true);
            if (data) {
                const enumDef: { [key: string]: number } = {};
                const count = spine().dataSkinCount(data.handle);
                for (let i = 0; i < count; i++) {
                    const name = spine().dataSkinName(data.handle, i);
                    if (name) enumDef[name] = i;
                }
                this._skinsEnum = Enum(enumDef);
            }
        } catch (e) {
            // Runtime not ready yet (e.g. editor inspector queried before the
            // wasm loaded) — return null; the enum populates on the next query.
        }
        return this._skinsEnum;
    }

    /**
     * @internal Since v3.7.2 this is an engine private function, only works in editor.
     * @en Gets the animation name enumeration for editor dropdowns.
     * @zh 获取动画名称枚举，供编辑器下拉框使用。
     */
    public getAnimsEnum (): { [key: string]: number } | null {
        if (this._animsEnum) return this._animsEnum;
        try {
            const data = this._getParsedData(true);
            if (data) {
                const enumDef: { [key: string]: number } = { '<None>': 0 };
                const count = spine().dataAnimationCount(data.handle);
                for (let i = 0; i < count; i++) {
                    const name = spine().dataAnimationName(data.handle, i);
                    if (name) enumDef[name] = i + 1;
                }
                this._animsEnum = Enum(enumDef);
            }
        } catch (e) {
            // Runtime not ready yet (see getSkinsEnum).
        }
        return this._animsEnum;
    }

    /**
     * @en Checks whether the asset has no skeleton content.
     * @zh 判断资源是否为空。
     */
    public isEmpty (): boolean {
        return this._skeletonJsonStr.length === 0 && this._atlasText.length === 0;
    }

    /**
     * @en Destroys the asset and releases the parsed runtime data.
     * @zh 销毁资源并释放解析的运行时数据。
     */
    public destroy (): boolean {
        if (this._data) {
            this._data.dispose();
            this._data = null;
        }
        return super.destroy();
    }

    /**
     * @internal
     * @en Create a node with a Skeleton component (used when the asset is
     * dragged into the scene in the editor).
     * @zh 创建一个带 Skeleton 组件的节点（编辑器将资源拖入场景时使用）。
     * @param callback The completion callback with the created node.
     */
    public createNode (callback: (err: Error | null, node: Node | null) => void): void {
        const node = new Node(this.name);
        // The renderer is a 2D UIMesh consumer, so mark the node for the UI
        // camera layer (matches how the engine places 2D assets in the scene).
        node.layer = Layers.Enum.UI_2D;
        // spine.ts registers the sp.spine component class here (kept plugin-local
        // to break the circular import without touching engine internals).
        const comp = SpineData._spineComponent ? node.addComponent(SpineData._spineComponent) : null;
        // Bind this asset to the component so the skeleton plays immediately.
        // Spine._loadFromSpineData defers loading until the runtime is ready.
        if (comp) {
            try {
                comp.spineData = this;
            } catch (e) {
                console.warn(`[sp.spine] createNode: could not bind skeletonData: ${e}`);
            }
        }
        callback(null, node);
    }
}

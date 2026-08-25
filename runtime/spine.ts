/*
 Copyright (c) 2026
 sp.spine — SpineRuntime driven spine component (spine-cpp 4.3).
 Renders through the engine's cc.UIMesh consumer: this component computes the
 spine mesh data (vertices / indices / segments) via the SpineRuntime binding
 and feeds it to UIMesh.setMeshData every frame. The engine owns buffer
 allocation, batching and submission, so this plugin needs no engine internals.
*/

import {
    Color,
    Enum,
    gfx,
    Mat4,
    Material,
    Node,
    renderer,
    Texture2D,
    UITransform,
    UIMesh,
    builtinResMgr,
    error,
    setPropertyEnumType,
} from 'cc';
import { _decorator } from 'cc';
const { ccclass, executeInEditMode, editable, help, menu, override, property } = _decorator;
import type { UIMeshSegment } from 'cc';
import { spine, loadSpineRuntime } from './bindings';
import { RuntimeData, RuntimeTextureMap } from './runtime-data';
import { SpineData } from './spine-data';
import { Animation, AnimationState, Attachment, Bone, Event, Slot, TrackEntry } from './runtime-objects';

const _tempMat4 = new Mat4();

/**
 * @en Material type used by getMaterialForBlendAndTint (legacy compat).
 * @zh getMaterialForBlendAndTint 使用的材质类型（旧版兼容）。
 */
export enum SpineMaterialType {
    COLORED_TEXTURED = 0,
    TWO_COLORED = 1,
}

/**
 * @en Animation cache mode (legacy compat). The cache pipeline was removed;
 * only REALTIME is supported.
 * @zh 动画缓存模式（旧版兼容）。缓存管线已移除，仅支持 REALTIME。
 */
export enum SpineAnimationCacheMode {
    UNSET = -1,
    REALTIME = 0,
    SHARED_CACHE = 1,
    PRIVATE_CACHE = 2,
}

type TrackListener = (entry: any) => void;
type TrackListener2 = (entry: any, ev: any) => void;

/**
 * Socket attached to a target bone, transform-synced with the spine animation.
 */
@ccclass('sp.spine.SpineSocket')
export class SpineSocket {
    /**
     * @en Path of the target joint.
     * @zh 此挂点的目标骨骼路径。
     */
    @property
    public path = '';

    /**
     * @en Transform output node.
     * @zh 此挂点的变换信息输出节点。
     */
    @property(Node)
    public target: Node | null = null;

    constructor (path = '', target: Node | null = null) {
        this.path = path;
        this.target = target;
    }
}

/**
 * @en The spine component (sp.spine). Drives spine-cpp 4.3 through the
 * SpineRuntime binding and renders via cc.UIMesh.
 * @zh spine 组件（sp.spine）。通过 SpineRuntime 绑定驱动 spine-cpp 4.3，
 * 经 cc.UIMesh 渲染。
 */
@ccclass('sp.spine')
@help('i18n:cc.Spine')
@menu('Spine/spine')
@executeInEditMode
export class Spine extends UIMesh {
    static RuntimeData = RuntimeData;
    static RuntimeTextureMap = RuntimeTextureMap;
    static SpineSocket = SpineSocket;
    static SpineData = SpineData;
    static SpineMaterialType = SpineMaterialType;
    static SpineAnimationCacheMode = SpineAnimationCacheMode;

    @property({ visible: false })
    protected _timeScale = 1;

    // Standard PNG atlases use straight (unassociated) alpha. PMA must be an
    // explicit opt-in matching the Spine export setting; otherwise transparent
    // pixels with retained RGB leak the vertex tint when blended with src=ONE.
    @property({ visible: false })
    protected _premultipliedAlpha = false;

    @property({ visible: false })
    protected _useTint = false;

    @property({ visible: false })
    protected _tintColor = new Color(255, 255, 255, 255);

    @property({ visible: false, type: [SpineSocket] })
    protected _sockets: SpineSocket[] = [];

    @property({ visible: false, type: SpineData })
    protected _spineData: SpineData | null = null;

    @property({ visible: false })
    protected _defaultAnimation = '';

    @property({ visible: false })
    protected _defaultSkin = '';

    @property({ visible: false })
    protected _debugMesh = false;

    @property({ visible: false })
    protected _debugBones = false;

    @property({ visible: false })
    protected _debugSlots = false;

    @property({ visible: false })
    protected _cacheMode = SpineAnimationCacheMode.REALTIME;

    @property
    public loop = true;

    protected _data: RuntimeData | null = null;
    protected _runtime = 0;
    protected _textureMap = new RuntimeTextureMap();
    protected _paused = false;
    protected _animationName = '';
    protected _skinName = '';
    protected _nextTextureId = 0;

    // Materials (blend mode -> MaterialInstance)
    protected _materialCache: { [key: string]: renderer.MaterialInstance } = {};

    // Events
    protected _startListener: TrackListener | null = null;
    protected _interruptListener: TrackListener | null = null;
    protected _endListener: TrackListener | null = null;
    protected _disposeListener: TrackListener | null = null;
    protected _completeListener: TrackListener | null = null;
    protected _eventListener: TrackListener2 | null = null;
    protected _eventBound = false;

    // One-shot runtime load for deferred _loadFromSpineData (drop-to-scene).
    protected _pendingRuntimeLoad: Promise<void> | null = null;

    // -----------------------------------------------------------------------
    // Loading
    // -----------------------------------------------------------------------
    /**
     * Load a skeleton from JSON + atlas + an array of engine textures.
     * @param textureNames @en Atlas image names, index-aligned with textures. When
     * omitted, texture.name is used as a best-effort match.
     * @zh 图集图片名，与 textures 按下标对齐。缺省时用 texture.name 尽力匹配。
     * @param scale @en Scale applied to bone positions / image sizes.
     * @zh 应用到骨骼位置和图像大小的缩放。
     */
    public loadFromJson (json: string, atlas: string, textures: Texture2D[], textureNames?: string[], scale = 1): boolean {
        const texNames = this._prepareLoad(textures, textureNames);
        return this._loadFromRuntimeData(RuntimeData.fromJson(json, atlas, texNames, scale), textures);
    }

    /**
     * Load a skeleton from binary (.skel) + atlas + an array of engine textures.
     * @param bytes @en Raw .skel skeleton bytes.
     * @zh 二进制（.skel）骨骼原始字节。
     * @param textureNames @en Atlas image names, index-aligned with textures. When
     * omitted, texture.name is used as a best-effort match.
     * @zh 图集图片名，与 textures 按下标对齐。缺省时用 texture.name 尽力匹配。
     * @param scale @en Scale applied to bone positions / image sizes.
     * @zh 应用到骨骼位置和图像大小的缩放。
     */
    public loadFromBinary (bytes: Uint8Array, atlas: string, textures: Texture2D[], textureNames?: string[], scale = 1): boolean {
        const texNames = this._prepareLoad(textures, textureNames);
        return this._loadFromRuntimeData(RuntimeData.fromBinary(bytes, atlas, texNames, scale), textures);
    }

    private _prepareLoad (textures: Texture2D[], textureNames?: string[]): string[] {
        this.destroyData();
        this._ensureNativeHeap();
        return (textureNames && textureNames.length === textures.length)
            ? textureNames
            : textures.map((t) => t.name);
    }

    private _loadFromRuntimeData (data: RuntimeData, textures: Texture2D[]): boolean {
        if (!data.valid) {
            console.error(`[sp.spine] load failed: ${spine().lastError()}`);
            return false;
        }
        this._data = data;
        for (let i = 0; i < textures.length; i++) this._textureMap.set(i, textures[i]);
        this._nextTextureId = textures.length;
        this._runtime = spine().createRuntime(data.handle);
        if (!this._runtime) return false;
        // Keep vertex-color packing and material blending on the same alpha
        // convention from the first rendered frame.
        this.applyParams();
        this._updateUITransform();
        this.markForUpdateRenderData();
        return true;
    }

    /**
     * Native (JSB) has no HEAPU8: allocate a shared staging buffer and register
     * it with the binding; runtimeRenderData copies vertices/indices into it and
     * vPtr/iPtr become offsets within it. No-op on wasm (reads Module.HEAPU8).
     */
    private _ensureNativeHeap (): void {
        const b = spine() as any;
        if (b.HEAPU8 || !b.setRenderBuffer || b.__spineruntimeHeap) return;
        b.__spineruntimeHeap = new Uint8Array(4 * 1024 * 1024); // 4MB
        b.setRenderBuffer(b.__spineruntimeHeap);
    }

    /**
     * @en The spine data asset this component renders. Assigning a new asset
     * (re)loads the skeleton.
     * @zh 此组件渲染的骨骼数据资源。赋值新资源会（重新）加载骨骼。
     */
    @property({ type: SpineData })
    get spineData (): SpineData | null { return this._spineData; }
    set spineData (value: SpineData | null) {
        if (this._spineData === value) return;
        this._spineData = value;
        if (value) value.resetEnums();
        this._loadFromSpineData();
    }

    /**
     * @en Sets the spine data asset (legacy compat).
     * @zh 设置骨骼数据资源（旧版兼容）。
     */
    public setSkeletonData (skeletonData: SpineData): void {
        this.spineData = skeletonData;
    }

    protected _loadFromSpineData (): void {
        const data = this._spineData;
        if (!data || data.isEmpty()) return;
        // Ensure the runtime binding is loaded before touching it. When the
        // component is created from an asset drop (editor) the wasm may not be
        // ready yet; defer and retry so binding spineData never throws.
        try {
            spine();
        } catch (e) {
            this._loadWhenRuntimeReady();
            return;
        }
        this._loadSkeleton(data);
    }

    private _loadSkeleton (data: SpineData): void {
        const loaded = data.skeletonBinary
            ? this.loadFromBinary(data.skeletonBinary, data.atlasText, data.textures, data.textureNames, data.scale)
            : this.loadFromJson(data.skeletonJsonStr, data.atlasText, data.textures, data.textureNames, data.scale);
        if (loaded) {
            this._applyDefaults();
            this._updateInspectorEnums();
        }
    }

    private _loadWhenRuntimeReady (): void {
        if (this._pendingRuntimeLoad) return;
        // Native/Simulator resolves the JSB binding first. Other environments
        // use the single external-WASM glue through cc.wasm.
        this._pendingRuntimeLoad = loadSpineRuntime().then(() => {
            this._pendingRuntimeLoad = null;
            const data = this._spineData;
            if (data && !this._data) this._loadFromSpineData();
        }).catch((err) => {
            this._pendingRuntimeLoad = null;
            console.error('[sp.spine] SpineRuntime load failed:', err);
        });
    }

    /**
     * @en The animation name to play automatically after the skeleton data loads.
     * @zh 骨骼数据加载完成后自动播放的动画名。Inspector 通过下面的下拉框设置。
     */
    get defaultAnimation (): string { return this._defaultAnimation; }
    set defaultAnimation (value: string) {
        this._defaultAnimation = value;
        if (this._data) this._applyDefaults();
    }

    /**
     * @en The skin name to apply automatically after the skeleton data loads.
     * @zh 骨骼数据加载完成后自动应用的皮肤名。Inspector 通过下面的下拉框设置。
     */
    get defaultSkin (): string { return this._defaultSkin; }
    set defaultSkin (value: string) {
        this._defaultSkin = value;
        if (this._data) this._applyDefaults();
    }

    // -----------------------------------------------------------------------
    // Editor dropdowns: default animation / default skin populated from the
    // loaded skeleton data (mirrors the built-in sp.Skeleton inspector).
    // -----------------------------------------------------------------------
    protected _enumAnimations: any = null;
    protected _enumSkins: any = null;

    @property({ type: Enum({}), visible: true })
    get _animationIndex (): number {
        const animsEnum = this._spineData ? this._spineData.getAnimsEnum() : null;
        if (animsEnum && this._defaultAnimation) {
            const idx = animsEnum[this._defaultAnimation];
            if (idx !== undefined) return idx;
        }
        return 0;
    }
    set _animationIndex (value: number) {
        const animsEnum = this._spineData ? this._spineData.getAnimsEnum() : null;
        if (!animsEnum) return;
        const name = animsEnum[value];
        if (name !== undefined) this.defaultAnimation = String(name);
    }

    @property({ type: Enum({}), visible: true })
    get _defaultSkinIndex (): number {
        const skinsEnum = this._spineData ? this._spineData.getSkinsEnum() : null;
        if (skinsEnum && this._defaultSkin) {
            const idx = skinsEnum[this._defaultSkin];
            if (idx !== undefined) return idx;
        }
        return 0;
    }
    set _defaultSkinIndex (value: number) {
        const skinsEnum = this._spineData ? this._spineData.getSkinsEnum() : null;
        if (!skinsEnum) return;
        const name = skinsEnum[value];
        if (name !== undefined) this.defaultSkin = String(name);
    }

    // Populate the dropdown enums from the loaded skeleton data (editor-only;
    // setPropertyEnumType is a no-op outside the editor).
    protected _updateInspectorEnums (): void {
        if (!this._spineData) return;
        try {
            const animEnum = this._spineData.getAnimsEnum();
            this._enumAnimations = Enum({});
            if (animEnum) Object.assign(this._enumAnimations, animEnum);
            Enum.update(this._enumAnimations);
            setPropertyEnumType(this, '_animationIndex', this._enumAnimations);

            const skinEnum = this._spineData.getSkinsEnum();
            this._enumSkins = Enum({});
            if (skinEnum) Object.assign(this._enumSkins, skinEnum);
            Enum.update(this._enumSkins);
            setPropertyEnumType(this, '_defaultSkinIndex', this._enumSkins);
        } catch (e) {
            // Dropdown stays empty until the runtime/data is ready.
        }
    }

    get defaultCacheMode (): SpineAnimationCacheMode {
        return SpineAnimationCacheMode.REALTIME;
    }
    set defaultCacheMode (mode: SpineAnimationCacheMode) {
        this.setAnimationCacheMode(mode);
    }

    protected _applyDefaults (): void {
        if (this._defaultSkin) this.setSkin(this._defaultSkin);
        if (this._defaultAnimation) {
            this.setAnimation(0, this._defaultAnimation, this.loop);
        } else if (this._data && this._data.animations.length) {
            // No animation configured (e.g. a node just created by dropping the
            // asset): play the first animation so the skeleton is not static.
            // Also write it back as the default so the inspector dropdown shows
            // the actual animation (not '<None>') — WYSIWYG.
            this._defaultAnimation = this._data.animations[0];
            this.setAnimation(0, this._defaultAnimation, this.loop);
        }
    }

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------
    public onLoad (): void {
        super.onLoad();
        if (this._spineData && !this._data) {
            this._loadFromSpineData();
        }
        this._forceUseLocal();
    }

    /**
     * This component always bakes vertices to world space (the material is
     * compiled with USE_LOCAL:false), so the render entity must never apply the
     * local/world matrix again. The base UIMesh defaults useLocal to true
     * (createRenderEntity) and only flips it through its enableBatch accessor,
     * both of which deserialization bypasses — force it here. The internal
     * members are absent from the public cc.d.ts, so reach them through a cast
     * (same pattern as markForUpdateRenderData).
     */
    private _forceUseLocal (): void {
        const self = this as any;
        if (self._renderEntity && typeof self._renderEntity.setUseLocal === 'function') {
            self._renderEntity.setUseLocal(false);
        }
    }

    public onDestroy (): void {
        this.destroyData();
        super.onDestroy();
    }

    protected destroyData (): void {
        if (this._runtime) {
            spine().disposeRuntime(this._runtime);
            this._runtime = 0;
        }
        if (this._data) {
            this._data.dispose();
            this._data = null;
        }
        this._textureMap.clear();
    }

    // -----------------------------------------------------------------------
    // Per-frame drive: run the facade and feed the UIMesh consumer.
    // -----------------------------------------------------------------------
    public update (dt: number): void {
        if (this._paused || !this._runtime) return;
        this._setOutputTransform();
        spine().runtimeUpdate(this._runtime, dt * this._timeScale);
        this._updateMeshData();
        this.syncAttachedNode();
    }

    private _setOutputTransform (): void {
        const w = this.node.worldMatrix;
        // Fold the spine y-down -> Cocos y-up flip into the world transform and
        // hand the combined 2D affine to the runtime. C++ bakes it into every
        // vertex (x' = a*x + b*y + tx, y' = c*x + d*y + ty), so batching needs
        // no per-node transform uniform.
        spine().runtimeSetOutputTransform(this._runtime,
            w.m00, -w.m04, w.m01, -w.m05, w.m12, w.m13);
    }

    private _updateMeshData (): void {
        if (!this._runtime) return;
        const rd = spine().runtimeRenderData(this._runtime);
        if (!rd || rd.vertexCount < 1 || rd.indexCount < 1) return;
        const stride = this._useTint ? 28 : 24;
        const heap = this._heap();
        if (!heap) return;
        const vLen = rd.vertexCount * stride;
        const iLen = rd.indexCount * 2;
        if (rd.vPtr + vLen > heap.length || rd.iPtr + iLen > heap.length) return;

        // JSB copies every runtime into one shared staging buffer and UIMesh
        // consumes the data later in the render phase. Own the frame data here
        // so another Spine component cannot overwrite it before submission.
        const vertexData = heap.slice(rd.vPtr, rd.vPtr + vLen);
        const indexData = heap.slice(rd.iPtr, rd.iPtr + iLen);

        // Vertices already arrive flipped + baked to world space: the runtime
        // applies the output affine (node/world transform + y-flip) in C++
        // (see _setOutputTransform), so no per-vertex transform happens here.

        const segments: UIMeshSegment[] = [];
        let indexOffset = 0;
        for (let i = 0; i < rd.segments.length; i++) {
            const seg = rd.segments[i];
            const texture = this._textureMap.get(seg.textureId);
            const material = this._segmentMaterial(seg.blendMode);
            segments.push({ indexOffset, indexCount: seg.indexCount, texture, material });
            indexOffset += seg.indexCount;
        }

        this.setMeshData({
            vertexCount: rd.vertexCount,
            vertexStride: stride,
            vertexData,
            indexCount: rd.indexCount,
            indexData,
            segments,
        });
    }

    private _heap (): Uint8Array | null {
        const b = spine() as any;
        if (b.HEAPU8) return b.HEAPU8;
        return b.__spineruntimeHeap as Uint8Array;
    }

    private _segmentMaterial (blendMode: number): renderer.MaterialInstance | null {
        let src: gfx.BlendFactor;
        let dst: gfx.BlendFactor;
        switch (blendMode) {
        case 1:
            src = this._premultipliedAlpha ? gfx.BlendFactor.ONE : gfx.BlendFactor.SRC_ALPHA;
            dst = gfx.BlendFactor.ONE;
            break;
        case 2:
            src = gfx.BlendFactor.DST_COLOR;
            dst = gfx.BlendFactor.ONE_MINUS_SRC_ALPHA;
            break;
        case 3:
            src = this._premultipliedAlpha ? gfx.BlendFactor.ONE : gfx.BlendFactor.SRC_ALPHA;
            dst = gfx.BlendFactor.ONE_MINUS_SRC_COLOR;
            break;
        default:
            src = this._premultipliedAlpha ? gfx.BlendFactor.ONE : gfx.BlendFactor.SRC_ALPHA;
            dst = gfx.BlendFactor.ONE_MINUS_SRC_ALPHA;
            break;
        }
        return this.getSpineMaterialForBlendAndTint(src, dst, this._useTint);
    }

    // -----------------------------------------------------------------------
    // Animation control
    // -----------------------------------------------------------------------
    /**
     * @en The current animation name.
     * @zh 当前动画名称。
     */
    get animation (): string { return this._animationName; }
    set animation (value: string) {
        if (value) {
            this.setAnimation(0, value, this.loop);
        } else {
            this.clearAnimation(0);
        }
    }

    /**
     * @en Sets the current animation on a track. Returns the track entry, or
     * null if the animation was not found.
     * @zh 在指定轨道上播放动画。返回 track 条目，找不到动画返回 null。
     */
    public setAnimation (trackIndex: number, name: string, loop = true): TrackEntry | null {
        if (!this._runtime || !name) return null;
        const handle = spine().runtimeSetAnimation(this._runtime, trackIndex, name, loop);
        if (!handle) {
            error(`[sp.spine] animation not found: ${name}`);
            return null;
        }
        this._animationName = name;
        return this._makeTrackEntry(handle);
    }

    /**
     * @en Queues an animation on a track to play after the current one.
     * @zh 将动画排入轨道队列，在当前动画后播放。
     */
    public addAnimation (trackIndex: number, name: string, loop: boolean, delay = 0): TrackEntry | null {
        if (!this._runtime) return null;
        const handle = spine().runtimeAddAnimation(this._runtime, trackIndex, name, loop, delay);
        return this._makeTrackEntry(handle);
    }

    /**
     * @en Finds an animation by name.
     * @zh 按名称查找动画。
     */
    public findAnimation (name: string): Animation | null {
        if (!this._data || !this._runtime) return null;
        if (spine().runtimeFindAnimation(this._runtime, name)) {
            return new Animation(name, spine().dataAnimationDuration(this._data.handle, name));
        }
        return null;
    }

    private _makeTrackEntry (handle: number): TrackEntry | null {
        if (!this._data || !handle) return null;
        return new TrackEntry(this._data.handle, this._runtime, handle);
    }

    /**
     * @en Clears a track's animation and returns to the setup pose.
     * @zh 清除指定轨道的动画并还原到初始姿势。
     */
    public clearAnimation (trackIndex?: number): void {
        this.clearTrack(trackIndex || 0);
        this.setToSetupPose();
    }

    /**
     * @en Clears all tracks and returns to the setup pose.
     * @zh 清除所有轨道并还原到初始姿势。
     */
    public clearAnimations (): void {
        this.clearTracks();
        this.setToSetupPose();
    }

    /**
     * @en Clears the animation of a track.
     * @zh 清除指定轨道的动画。
     */
    public clearTrack (trackIndex: number): void {
        if (this._runtime) spine().runtimeClearTrack(this._runtime, trackIndex);
    }

    /**
     * @en Clears all tracks.
     * @zh 清除所有轨道。
     */
    public clearTracks (): void {
        if (this._runtime) spine().runtimeClearTracks(this._runtime);
    }

    /**
     * @en Returns the current track entry for a track, or null.
     * @zh 返回指定轨道的当前 track 条目，无则 null。
     */
    public getCurrent (trackIndex: number): TrackEntry | null {
        if (!this._runtime) return null;
        const handle = spine().runtimeGetCurrent(this._runtime, trackIndex);
        return this._makeTrackEntry(handle);
    }

    /**
     * @en Applies a skin by name.
     * @zh 按名称应用皮肤。
     */
    public setSkin (name: string): void {
        if (!this._runtime || !name) return;
        if (spine().runtimeSetSkin(this._runtime, name)) this._skinName = name;
    }

    /**
     * @en Sets the mix duration between two animations.
     * @zh 设置两个动画之间的过渡时长。
     */
    public setMix (fromAnimation: string, toAnimation: string, duration: number): void {
        if (this._runtime) spine().runtimeSetMix(this._runtime, fromAnimation, toAnimation, duration);
    }

    // -----------------------------------------------------------------------
    // Pose
    // -----------------------------------------------------------------------
    public setToSetupPose (): void {
        if (this._runtime) spine().runtimeSetToSetupPose(this._runtime);
    }
    public setBonesToSetupPose (): void {
        if (this._runtime) spine().runtimeSetBonesToSetupPose(this._runtime);
    }
    public setSlotsToSetupPose (): void {
        if (this._runtime) spine().runtimeSetSlotsToSetupPose(this._runtime);
    }

    // -----------------------------------------------------------------------
    // Bones / slots / attachments
    // -----------------------------------------------------------------------
    /**
     * @en Finds a bone by name and returns its transform info.
     * @zh 按名称查找骨骼，返回其变换信息。
     */
    public findBone (boneName: string): Bone | null {
        if (!this._runtime) return null;
        const index = spine().runtimeFindBoneIndex(this._runtime, boneName);
        if (index < 0) return null;
        return new Bone(this._runtime, index);
    }

    /**
     * @en Finds a slot by name and returns its info.
     * @zh 按名称查找槽位，返回其信息。
     */
    public findSlot (slotName: string): Slot | null {
        if (!this._runtime) return null;
        const index = spine().runtimeFindSlotIndex(this._runtime, slotName);
        if (index < 0) return null;
        return new Slot(this._runtime, index);
    }

    /**
     * @en Sets an attachment on a slot.
     * @zh 为槽位设置附件。
     */
    public setAttachment (slotName: string, attachmentName: string): void {
        if (this._runtime) spine().runtimeSetAttachment(this._runtime, slotName, attachmentName);
    }

    /**
     * @en Gets an attachment info by slot and attachment name.
     * @zh 按槽位和附件名获取附件信息。
     */
    public getAttachment (slotName: string, attachmentName: string): Attachment | null {
        if (!this._runtime) return null;
        const info = spine().runtimeGetAttachment(this._runtime, slotName, attachmentName);
        return info ? new Attachment(info) : null;
    }

    /**
     * @en Sets a texture for a slot (local skin swap).
     * @zh 为槽位设置贴图（局部换装）。
     */
    public setSlotTexture (slotName: string, tex2d: Texture2D, createNew?: boolean): void {
        if (!this._runtime) return;
        if (createNew) {
            spine().runtimeResizeSlotRegion(this._runtime, slotName, tex2d.width, tex2d.height, true);
        }
        const id = this._getOrAddTexture(tex2d);
        spine().runtimeSetSlotTexture(this._runtime, slotName, id);
    }

    /**
     * @en Restricts rendering to slots in the given range.
     * @zh 限制只渲染给定范围内的槽位。
     */
    public setSlotsRange (startSlotIndex: number, endSlotIndex: number): void {
        if (this._runtime) spine().runtimeSetSlotsRange(this._runtime, startSlotIndex, endSlotIndex);
    }

    private _getOrAddTexture (tex2d: Texture2D): number {
        for (const [id, t] of this._textureMap.entries()) {
            if (t === tex2d) return id;
        }
        const id = this._nextTextureId++;
        this._textureMap.set(id, tex2d);
        return id;
    }

    // -----------------------------------------------------------------------
    // Legacy compat helpers
    // -----------------------------------------------------------------------
    /**
     * @en Whether in cached mode. Cache was removed, always false.
     * @zh 是否处于缓存模式。缓存已移除，始终 false。
     */
    public isAnimationCached (): boolean {
        return false;
    }

    /**
     * @en Sets the animation cache mode. Cache was removed; only REALTIME works.
     * @zh 设置动画缓存模式。缓存已移除，仅 REALTIME 可用。
     */
    public setAnimationCacheMode (cacheMode: SpineAnimationCacheMode): void {
        if (cacheMode !== SpineAnimationCacheMode.REALTIME) {
            error('[sp.spine] animation cache mode was removed; only REALTIME is supported.');
        }
        this._cacheMode = SpineAnimationCacheMode.REALTIME;
    }

    /**
     * @en Invalidates the animation cache. No-op (cache removed).
     * @zh 使动画缓存失效。无操作（缓存已移除）。
     */
    public invalidAnimationCache (): void {
        // cache removed, no-op
    }

    /**
     * @en Queries all bone paths available for socket attachment. The Creator
     * Inspector calls this method to populate the socket path search picker.
     * @zh 查询所有可绑定挂点的骨骼路径。Creator Inspector 会调用此方法填充
     * Socket path 的搜索选择器。
     */
    public querySockets (): string[] {
        if (!this._runtime) return [];
        const binding = spine();
        const count = binding.runtimeBoneCount(this._runtime);
        const paths: string[] = [];
        for (let i = 0; i < count; i++) {
            const path = binding.runtimeBoneName(this._runtime, i);
            if (path) paths.push(path);
        }
        return paths.sort();
    }

    /**
     * @en Marks the component for render data update.
     * @zh 标记重新更新渲染数据。
     */
    public markForUpdateRenderData (enable = true): void {
        // UIRenderer exposes this at runtime, but Creator omits the internal
        // method from the generated public cc.d.ts.
        const mark = (this as any)._markForUpdateRenderData;
        if (typeof mark === 'function') mark.call(this, enable);
    }

    /**
     * @en Destroys the render data.
     * @zh 销毁渲染数据。
     */
    public destroyRenderData (): void {
        super.destroyRenderData();
    }

    /**
     * @en Gets the animation state, through which tracks can be controlled.
     * @zh 获取动画状态，可通过它控制轨道。
     */
    public getState (): AnimationState | null {
        if (!this._runtime) return null;
        return new AnimationState(this._data ? this._data.handle : 0, this._runtime);
    }

    /**
     * @en Gets a texture atlas. Not supported by SpineRuntime (returns null).
     * @zh 获取纹理图集。SpineRuntime 不支持（返回 null）。
     */
    public getTextureAtlas (regionAttachment: any): null {
        return null;
    }

    /**
     * @en Gets debug shapes. Not supported by SpineRuntime (returns null).
     * @zh 获取调试形状。SpineRuntime 不支持（返回 null）。
     */
    public getDebugShapes (): null {
        return null;
    }

    get timeScale (): number { return this._timeScale; }
    set timeScale (value: number) {
        this._timeScale = value;
        this.applyParams();
    }

    get paused (): boolean { return this._paused; }
    set paused (value: boolean) {
        this._paused = value;
        if (this._runtime) spine().runtimeSetPaused(this._runtime, value);
    }

    /**
     * @en Sockets attached to bones, transform-synced with the spine animation.
     * @zh 附着在骨骼上的挂点，随 spine 动画同步变换。
     */
    @property({ type: [SpineSocket] })
    get sockets (): SpineSocket[] { return this._sockets; }
    set sockets (val: SpineSocket[]) {
        this._sockets = val;
    }

    get premultipliedAlpha (): boolean { return this._premultipliedAlpha; }
    set premultipliedAlpha (value: boolean) {
        this._premultipliedAlpha = value;
        this.applyParams();
        this.markForUpdateRenderData();
    }

    get useTint (): boolean { return this._useTint; }
    set useTint (value: boolean) {
        if (value === this._useTint) return;
        this._useTint = value;
        // The vertex format changed (24 <-> 28 bytes). The render data was
        // built with the previous format during __preload, so rebuild it with
        // the matching accessor/format, else stride-28 vertices get written
        // into a stride-24 buffer and render garbage.
        this.destroyRenderData();
        this._flushAssembler();
        this.applyParams();
        this.markForUpdateRenderData();
    }

    /**
     * @en Whether to enable sprite batching. This component always bakes
     * vertices to world space (material compiled with USE_LOCAL:false), so
     * toggling this only changes batch merging and never flips the render
     * entity's useLocal flag — the node/world matrix must not be applied twice.
     * @zh 是否启用合批。本组件始终将顶点烘焙到世界空间（材质以 USE_LOCAL:false
     * 编译），因此切换此项只改变合批合并，不会翻转渲染实体的 useLocal 标志——
     * 节点/世界矩阵不可被重复应用。
     */
    @override
    @editable
    get enableBatch (): boolean { return (this as any)._enableBatch; }
    set enableBatch (value: boolean) {
        (this as any)._enableBatch = value;
        this._forceUseLocal();
        this.markForUpdateRenderData();
    }

    get tintColor (): Color { return this._tintColor; }
    set tintColor (value: Color) {
        this._tintColor = value;
        // Color is baked per-vertex each frame by the C++ runtime, so a change
        // only needs to re-send the params; the vertex format/stride is
        // unchanged (unlike `useTint`), so no render-data rebuild is required.
        this.applyParams();
        this.markForUpdateRenderData();
    }

    protected applyParams (): void {
        if (!this._runtime) return;
        const c = this._tintColor;
        spine().runtimeSetParams(this._runtime, this._timeScale,
                                 c.r / 255, c.g / 255, c.b / 255, c.a / 255,
                                 this._premultipliedAlpha, this._useTint);
    }

    /**
     * Sync socket nodes to the current bone world transforms every frame.
     * Mesh vertices are reflected on Y in _updateMeshData(), so the socket's
     * translation and Y basis row must use the same render-space conversion.
     */
    syncAttachedNode (): void {
        if (!this._runtime || this._sockets.length === 0) return;
        const b = spine();
        const tm = _tempMat4;
        for (let i = 0; i < this._sockets.length; i++) {
            const sock = this._sockets[i];
            if (!sock.path || !sock.target || !sock.target.isValid) continue;
            const bone = b.runtimeGetBoneByName(this._runtime, sock.path);
            if (!bone) continue;
            tm.m00 = bone.a;
            tm.m01 = -bone.c;
            tm.m04 = bone.b;
            tm.m05 = -bone.d;
            tm.m12 = bone.worldX;
            tm.m13 = -bone.worldY;
            sock.target.matrix = tm;
        }
    }

    // -----------------------------------------------------------------------
    // Materials (blendMode -> MaterialInstance)
    // -----------------------------------------------------------------------
    protected getMaterialTemplate (): Material {
        if (this.customMaterial !== null) return this.customMaterial;
        if (this.material) return this.material;
        this.updateMaterial();
        return this.material!;
    }

    public updateMaterial (): void {
        const mat = builtinResMgr.get<Material>('default-spine-material');
        if (mat) this.setSharedMaterial(mat, 0);
        this._cleanMaterialCache();
    }

    private _cleanMaterialCache (): void {
        for (const key in this._materialCache) {
            this._materialCache[key].destroy();
        }
        this._materialCache = {};
    }

    /**
     * @en Build a material instance for the given blend source/target + tint,
     * cached and reused.
     * @zh 按 blend 源/目标 + tint 构建材质实例，缓存复用。
     */
    public getSpineMaterialForBlendAndTint (src: gfx.BlendFactor, dst: gfx.BlendFactor, useTint: boolean): renderer.MaterialInstance {
        const key = `${useTint}/${src}/${dst}`;
        let inst = this._materialCache[key];
        if (inst) return inst;
        const material = this.getMaterialTemplate();
        try {
            inst = new renderer.MaterialInstance({ parent: material, subModelIdx: 0, owner: this });
            this._materialCache[key] = inst;
            inst.overridePipelineStates({
                blendState: {
                    blendColor: Color.WHITE,
                    targets: [{
                        blendEq: gfx.BlendOp.ADD,
                        blendAlphaEq: gfx.BlendOp.ADD,
                        blendSrc: src,
                        blendDst: dst,
                        blendSrcAlpha: src,
                        blendDstAlpha: dst,
                    }],
                },
            });
            // Vertex positions are baked to world space in _updateMeshData().
            // Keep this identical for batched, non-batched, web and JSB paths.
            inst.recompileShaders({ TWO_COLORED: useTint, USE_LOCAL: false });
            return inst;
        } catch (e) {
            // The web-preview engine bundle ships the native (jsb) MaterialInstance
            // override, which is not constructible in the browser. Fall back to the
            // renderer's default material instance so rendering still works.
            const fallback = this.getMaterialInstance(0);
            if (fallback) {
                try {
                    fallback.recompileShaders({ TWO_COLORED: useTint, USE_LOCAL: false });
                } catch (compileError) {
                    // The built-in spine effect defaults to world-space input,
                    // so an older fallback without recompile support is usable.
                }
                return fallback;
            }
            throw e;
        }
    }

    /**
     * @en Builds a material instance (legacy name/signature).
     * @zh 构建材质实例（旧版名称与签名）。
     */
    public getMaterialForBlendAndTint (src: gfx.BlendFactor, dst: gfx.BlendFactor, type: SpineMaterialType): renderer.MaterialInstance {
        return this.getSpineMaterialForBlendAndTint(src, dst, type === SpineMaterialType.TWO_COLORED);
    }

    // -----------------------------------------------------------------------
    // Events
    // -----------------------------------------------------------------------
    public setStartListener (listener: TrackListener): void {
        this._startListener = listener;
        this._bindEvents();
    }

    public setInterruptListener (listener: TrackListener): void {
        this._interruptListener = listener;
        this._bindEvents();
    }

    public setEndListener (listener: TrackListener): void {
        this._endListener = listener;
        this._bindEvents();
    }

    public setDisposeListener (listener: TrackListener): void {
        this._disposeListener = listener;
        this._bindEvents();
    }

    public setCompleteListener (listener: TrackListener): void {
        this._completeListener = listener;
        this._bindEvents();
    }

    public setEventListener (listener: TrackListener2): void {
        this._eventListener = listener;
        this._bindEvents();
    }

    protected _bindEvents (): void {
        if (!this._runtime || this._eventBound) return;
        this._eventBound = true;
        const dataHandle = this._data ? this._data.handle : 0;
        spine().runtimeSetEventListener(this._runtime, (ev) => {
            const entry = new TrackEntry(dataHandle, this._runtime!, ev.track);
            switch (ev.type) {
            case 0:
                if (this._startListener) this._startListener(entry);
                break;
            case 1:
                if (this._interruptListener) this._interruptListener(entry);
                break;
            case 2:
                if (this._endListener) this._endListener(entry);
                break;
            case 3:
                if (this._disposeListener) this._disposeListener(entry);
                break;
            case 4:
                if (this._completeListener) {
                    (entry as any).loopCount = ev.trackTime > 0 && ev.animationEnd > 0
                        ? Math.floor(ev.trackTime / ev.animationEnd)
                        : 0;
                    this._completeListener(entry);
                }
                break;
            case 5:
                if (this._eventListener) {
                    this._eventListener(entry, new Event(ev));
                }
                break;
            }
        });
    }

    private _updateUITransform (): void {
        if (!this._data) return;
        const uiTrans = this.node.getComponent(UITransform) || this.node.addComponent(UITransform);
        const { width, height, x, y } = this._data;
        if (!width || !height) return;
        uiTrans.setContentSize(width, height);
        // Mirrors the built-in spine: the anchor is the skeleton bounds' offset
        // from the origin, so the rect matches the model (not the model's
        // bottom landing at the rect center).
        if (width !== 0) uiTrans.anchorX = Math.abs(x) / width;
        if (height !== 0) uiTrans.anchorY = Math.abs(y) / height;
    }
}

// Register the component class on SpineData so the asset's createNode can
// build a node with this component (plugin-local; no engine internals used).
SpineData._spineComponent = Spine;

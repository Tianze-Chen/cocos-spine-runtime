/*
 Copyright (c) 2026
 spine-ts compatible wrapper classes. Each wrapper holds the runtime/data handle
 plus an object handle (track handle / bone index / slot index ...), and exposes
 the same property/method surface as the legacy spine-ts runtime objects.
 Getters query the SpineRuntime binding live; setters forward to it, so writes
 like `entry.timeScale = 2` really take effect on the C++ runtime.
 Note: every property read is one JS->native round trip (no cached fields).
*/

import { Color } from 'cc';
import { spine } from './bindings';
import type { SpineAttachmentInfo, SpineBoneInfo, SpineSlotInfo, SpineTrackInfo } from './bindings';

/**
 * @en A spine animation (compat wrapper).
 * @zh spine 动画对象（兼容包装）。
 */
export class Animation {
    public name: string;
    public duration: number;
    constructor (name: string, duration = 0) {
        this.name = name;
        this.duration = duration;
    }
}

/**
 * @en A spine skin (compat wrapper).
 * @zh spine 皮肤对象（兼容包装）。
 */
export class Skin {
    public name: string;
    constructor (name: string) {
        this.name = name;
    }
}

/**
 * @en A spine event (compat wrapper).
 * @zh spine 事件对象（兼容包装）。
 */
export class Event {
    public name: string;
    public time: number;
    public intValue: number;
    public floatValue: number;
    public stringValue: string;
    public audioPath: string;
    public volume: number;
    public balance: number;
    constructor (info: any) {
        this.name = info.name ?? info.eventName ?? '';
        this.time = info.time ?? info.eventTime ?? 0;
        this.intValue = info.intValue ?? 0;
        this.floatValue = info.floatValue ?? 0;
        this.stringValue = info.stringValue ?? '';
        this.audioPath = info.audioPath ?? '';
        this.volume = info.volume ?? 1;
        this.balance = info.balance ?? 0;
    }
}

/**
 * @en A track entry (compat wrapper for spine.TrackEntry). Holds the runtime
 * handle and the track handle; reads/writes forward to the SpineRuntime binding.
 * @zh track 条目（spine.TrackEntry 的兼容包装）。持有 runtime 句柄和 track 句柄，
 * 读写转发到 SpineRuntime 绑定。
 */
export class TrackEntry {
    private _data: number;
    private _runtime: number;
    private _handle: number;

    constructor (dataHandle: number, runtimeHandle: number, trackHandle: number) {
        this._data = dataHandle;
        this._runtime = runtimeHandle;
        this._handle = trackHandle;
    }

    /** @en The raw track handle. @zh 原始 track 句柄。 */
    get track (): number { return this._handle; }
    get handle (): number { return this._handle; }

    private _info (): SpineTrackInfo | null {
        if (!this._handle) return null;
        return spine().runtimeGetTrackInfo(this._runtime, this._handle);
    }

    get trackIndex (): number { return this._info()?.trackIndex ?? -1; }

    get animation (): Animation | null {
        const name = this._info()?.animationName;
        if (!name) return null;
        const duration = this._data ? spine().dataAnimationDuration(this._data, name) : 0;
        return new Animation(name, duration);
    }

    get next (): TrackEntry | null {
        const h = this._info()?.next;
        return h ? new TrackEntry(this._data, this._runtime, h) : null;
    }
    get mixingFrom (): TrackEntry | null {
        const h = this._info()?.mixingFrom;
        return h ? new TrackEntry(this._data, this._runtime, h) : null;
    }
    get mixingTo (): TrackEntry | null {
        const h = this._info()?.mixingTo;
        return h ? new TrackEntry(this._data, this._runtime, h) : null;
    }

    get loop (): boolean { return this._info()?.loop ?? false; }
    set loop (v: boolean) { this._set('loop', v); }
    get reverse (): boolean { return this._info()?.reverse ?? false; }
    set reverse (v: boolean) { this._set('reverse', v); }
    get additive (): boolean { return this._info()?.additive ?? false; }
    set additive (v: boolean) { this._set('additive', v); }
    get shortestRotation (): boolean { return this._info()?.shortestRotation ?? false; }
    set shortestRotation (v: boolean) { this._set('shortestRotation', v); }

    get delay (): number { return this._info()?.delay ?? 0; }
    set delay (v: number) { this._set('delay', v); }
    get trackTime (): number { return this._info()?.trackTime ?? 0; }
    set trackTime (v: number) { this._set('time', v); }
    get trackEnd (): number { return this._info()?.trackEnd ?? 0; }
    set trackEnd (v: number) { this._set('end', v); }
    get animationStart (): number { return this._info()?.animationStart ?? 0; }
    set animationStart (v: number) { this._setRange(v, this.animationEnd, this.animationLast); }
    get animationEnd (): number { return this._info()?.animationEnd ?? 0; }
    set animationEnd (v: number) { this._setRange(this.animationStart, v, this.animationLast); }
    get animationLast (): number { return this._info()?.animationLast ?? 0; }
    set animationLast (v: number) { this._setRange(this.animationStart, this.animationEnd, v); }
    get animationTime (): number { return this._info()?.animationTime ?? 0; }
    get timeScale (): number { return this._info()?.timeScale ?? 1; }
    set timeScale (v: number) { this._set('timeScale', v); }
    get alpha (): number { return this._info()?.alpha ?? 1; }
    set alpha (v: number) { this._set('alpha', v); }
    get mixTime (): number { return this._info()?.mixTime ?? 0; }
    set mixTime (v: number) { this._set('mixTime', v); }
    get mixDuration (): number { return this._info()?.mixDuration ?? 0; }
    set mixDuration (v: number) { this._set('mixDuration', v); }
    get trackComplete (): number { return this._info()?.trackComplete ?? 0; }
    get eventThreshold (): number { return this._info()?.eventThreshold ?? 0; }
    get mixAttachmentThreshold (): number { return this._info()?.mixAttachmentThreshold ?? 0; }
    get alphaAttachmentThreshold (): number { return this._info()?.alphaAttachmentThreshold ?? 0; }
    get mixDrawOrderThreshold (): number { return this._info()?.mixDrawOrderThreshold ?? 0; }
    get complete (): boolean { return this._info()?.complete ?? false; }
    get wasApplied (): boolean { return this._info()?.wasApplied ?? false; }
    get emptyAnimation (): boolean { return this._info()?.emptyAnimation ?? false; }

    setAnimationLast (animationLast: number): void {
        this._setRange(this.animationStart, this.animationEnd, animationLast);
    }
    setTimeScale (timeScale: number): void { this.timeScale = timeScale; }
    setLoop (loop: boolean): void { this.loop = loop; }
    setDelay (delay: number): void { this.delay = delay; }
    setTrackTime (trackTime: number): void { this.trackTime = trackTime; }
    setTrackEnd (trackEnd: number): void { this.trackEnd = trackEnd; }
    setAnimationStart (start: number): void { this.animationStart = start; }
    setAnimationEnd (end: number): void { this.animationEnd = end; }
    setMixTime (mixTime: number): void { this.mixTime = mixTime; }
    setMixDuration (mixDuration: number): void { this.mixDuration = mixDuration; }
    setReverse (reverse: boolean): void { this.reverse = reverse; }
    setShortestRotation (shortestRotation: boolean): void { this.shortestRotation = shortestRotation; }
    setAlpha (alpha: number): void { this.alpha = alpha; }
    setTrackComplete (trackComplete: number): void {
        if (this._handle) {
            // trackComplete maps to the event threshold slot; set via thresholds is not
            // a 1:1 map, so keep the value read-only if no dedicated setter exists.
            void trackComplete;
        }
    }
    setThresholds (eventThreshold: number, mixAttachmentThreshold: number, alphaAttachmentThreshold: number, mixDrawOrderThreshold: number): void {
        if (this._handle) {
            spine().runtimeSetTrackThresholds(this._runtime, this._handle, eventThreshold, mixAttachmentThreshold, mixDrawOrderThreshold);
            spine().runtimeSetTrackAlphaAttachmentThreshold(this._runtime, this._handle, alphaAttachmentThreshold);
        }
    }
    resetRotationDirections (): void {
        if (this._handle) spine().runtimeResetTrackRotationDirections(this._runtime, this._handle);
    }

    private _set (which: string, value: any): void {
        if (!this._handle) return;
        const b = spine();
        const h = this._handle;
        switch (which) {
        case 'loop': b.runtimeSetTrackLoop(this._runtime, h, value); break;
        case 'reverse': b.runtimeSetTrackReverse(this._runtime, h, value); break;
        case 'additive': b.runtimeSetTrackAdditive(this._runtime, h, value); break;
        case 'shortestRotation': b.runtimeSetTrackShortestRotation(this._runtime, h, value); break;
        case 'delay': b.runtimeSetTrackDelay(this._runtime, h, value); break;
        case 'time': b.runtimeSetTrackTime(this._runtime, h, value); break;
        case 'end': b.runtimeSetTrackEnd(this._runtime, h, value); break;
        case 'timeScale': b.runtimeSetTrackTimeScale(this._runtime, h, value); break;
        case 'alpha': b.runtimeSetTrackAlpha(this._runtime, h, value); break;
        case 'mixTime': b.runtimeSetTrackMixTime(this._runtime, h, value); break;
        case 'mixDuration': b.runtimeSetTrackMixDuration(this._runtime, h, value); break;
        }
    }

    private _setRange (start: number, end: number, last: number): void {
        if (this._handle) spine().runtimeSetTrackAnimationRange(this._runtime, this._handle, start, end, last);
    }
}

/**
 * @en The animation state (compat wrapper for spine.AnimationState). Forwards
 * playback control to the SpineRuntime binding.
 * @zh 动画状态（spine.AnimationState 的兼容包装）。转发播放控制到 SpineRuntime 绑定。
 */
export class AnimationState {
    private _data: number;
    private _runtime: number;

    constructor (dataHandle: number, runtimeHandle: number) {
        this._data = dataHandle;
        this._runtime = runtimeHandle;
    }

    setAnimation (trackIndex: number, animationName: string, loop: boolean): TrackEntry | null {
        const h = spine().runtimeSetAnimation(this._runtime, trackIndex, animationName, loop);
        return h ? new TrackEntry(this._data, this._runtime, h) : null;
    }
    addAnimation (trackIndex: number, animationName: string, loop: boolean, delay = 0): TrackEntry | null {
        const h = spine().runtimeAddAnimation(this._runtime, trackIndex, animationName, loop, delay);
        return h ? new TrackEntry(this._data, this._runtime, h) : null;
    }
    setEmptyAnimation (trackIndex: number, mixDuration: number): TrackEntry | null {
        spine().runtimeSetEmptyAnimation(this._runtime, trackIndex, mixDuration);
        return this.getCurrent(trackIndex);
    }
    addEmptyAnimation (trackIndex: number, mixDuration: number, delay: number): TrackEntry | null {
        spine().runtimeAddEmptyAnimation(this._runtime, trackIndex, mixDuration, delay);
        return this.getCurrent(trackIndex);
    }
    setEmptyAnimations (mixDuration: number): void {
        spine().runtimeSetEmptyAnimations(this._runtime, mixDuration);
    }
    clearTrack (trackIndex: number): void {
        spine().runtimeClearTrack(this._runtime, trackIndex);
    }
    clearTracks (): void {
        spine().runtimeClearTracks(this._runtime);
    }
    setMix (fromAnimation: string, toAnimation: string, duration: number): void {
        spine().runtimeSetMix(this._runtime, fromAnimation, toAnimation, duration);
    }
    getCurrent (trackIndex: number): TrackEntry | null {
        const h = spine().runtimeGetCurrent(this._runtime, trackIndex);
        return h ? new TrackEntry(this._data, this._runtime, h) : null;
    }
}

/**
 * @en A bone (compat wrapper for spine.Bone). Reads forward to runtimeGetBone.
 * @zh 骨骼对象（spine.Bone 的兼容包装）。读取转发到 runtimeGetBone。
 */
export class Bone {
    private _runtime: number;
    private _index: number;

    constructor (runtimeHandle: number, boneIndex: number) {
        this._runtime = runtimeHandle;
        this._index = boneIndex;
    }

    get index (): number { return this._index; }

    private _info (): SpineBoneInfo | null {
        return spine().runtimeGetBone(this._runtime, this._index);
    }

    get name (): string { return this._info()?.name ?? ''; }
    get data (): any {
        const i = this._info();
        return i ? { name: i.name, index: i.index, parentIndex: i.parentIndex } : null;
    }
    get parent (): Bone | null {
        const pi = this._info()?.parentIndex;
        return pi != null && pi >= 0 ? new Bone(this._runtime, pi) : null;
    }
    get active (): boolean { return this._info()?.active ?? false; }
    get x (): number { return this._info()?.x ?? 0; }
    set x (v: number) { this._setLocal(v, this.y, this.rotation, this.scaleX, this.scaleY, this.shearX, this.shearY); }
    get y (): number { return this._info()?.y ?? 0; }
    set y (v: number) { this._setLocal(this.x, v, this.rotation, this.scaleX, this.scaleY, this.shearX, this.shearY); }
    get rotation (): number { return this._info()?.rotation ?? 0; }
    set rotation (v: number) { this._setLocal(this.x, this.y, v, this.scaleX, this.scaleY, this.shearX, this.shearY); }
    get scaleX (): number { return this._info()?.scaleX ?? 1; }
    set scaleX (v: number) { this._setLocal(this.x, this.y, this.rotation, v, this.scaleY, this.shearX, this.shearY); }
    get scaleY (): number { return this._info()?.scaleY ?? 1; }
    set scaleY (v: number) { this._setLocal(this.x, this.y, this.rotation, this.scaleX, v, this.shearX, this.shearY); }
    get shearX (): number { return this._info()?.shearX ?? 0; }
    set shearX (v: number) { this._setLocal(this.x, this.y, this.rotation, this.scaleX, this.scaleY, v, this.shearY); }
    get shearY (): number { return this._info()?.shearY ?? 0; }
    set shearY (v: number) { this._setLocal(this.x, this.y, this.rotation, this.scaleX, this.scaleY, this.shearX, v); }
    get a (): number { return this._info()?.a ?? 1; }
    get b (): number { return this._info()?.b ?? 0; }
    get c (): number { return this._info()?.c ?? 0; }
    get d (): number { return this._info()?.d ?? 1; }
    get worldX (): number { return this._info()?.worldX ?? 0; }
    get worldY (): number { return this._info()?.worldY ?? 0; }

    worldToLocal (worldX: number, worldY: number): { x: number; y: number } | null {
        if (!this._runtime || !this.name) return null;
        return spine().runtimeBoneWorldToLocal(this._runtime, this.name, worldX, worldY);
    }
    localToWorld (localX: number, localY: number): { x: number; y: number } | null {
        if (!this._runtime || !this.name) return null;
        return spine().runtimeBoneLocalToWorld(this._runtime, this.name, localX, localY);
    }

    private _setLocal (x: number, y: number, rotation: number, scaleX: number, scaleY: number, shearX: number, shearY: number): void {
        if (this._runtime && this.name) {
            spine().runtimeSetBoneLocal(this._runtime, this.name, x, y, rotation, scaleX, scaleY, shearX, shearY);
        }
    }
}

/**
 * @en A slot (compat wrapper for spine.Slot).
 * @zh 槽位对象（spine.Slot 的兼容包装）。
 */
export class Slot {
    private _runtime: number;
    private _index: number;

    constructor (runtimeHandle: number, slotIndex: number) {
        this._runtime = runtimeHandle;
        this._index = slotIndex;
    }

    get index (): number { return this._index; }

    private _info (): SpineSlotInfo | null {
        return spine().runtimeGetSlot(this._runtime, this._index);
    }

    get name (): string { return this._info()?.name ?? ''; }
    get data (): any {
        const i = this._info();
        return i ? { name: i.name, index: i.index, boneIndex: i.boneIndex, blendMode: i.blendMode } : null;
    }
    get bone (): Bone | null {
        const bi = this._info()?.boneIndex;
        return bi != null && bi >= 0 ? new Bone(this._runtime, bi) : null;
    }
    get attachmentName (): string { return this._info()?.attachmentName ?? ''; }
    get blendMode (): number { return this._info()?.blendMode ?? 0; }
    get color (): Color {
        const i = this._info();
        return new Color(i ? Math.round(i.colorR * 255) : 255,
                         i ? Math.round(i.colorG * 255) : 255,
                         i ? Math.round(i.colorB * 255) : 255,
                         i ? Math.round(i.colorA * 255) : 255);
    }
    get hasDarkColor (): boolean { return this._info()?.hasDarkColor ?? false; }

    getAttachment (): Attachment | null {
        if (!this._runtime || !this.name) return null;
        const info = spine().runtimeGetCurrentAttachment(this._runtime, this.name);
        return info ? new Attachment(info) : null;
    }
    setAttachment (attachmentName: string): void {
        if (this._runtime && this.name) spine().runtimeSetAttachment(this._runtime, this.name, attachmentName);
    }
    setColor (r: number, g: number, b: number, a: number): void {
        if (this._runtime && this.name) spine().runtimeSetSlotColor(this._runtime, this.name, r, g, b, a);
    }
}

/**
 * @en An attachment (compat wrapper for spine.Attachment). A snapshot of the
 * attachment info returned by the binding.
 * @zh 附件对象（spine.Attachment 的兼容包装）。绑定返回的附件信息快照。
 */
export class Attachment {
    public name: string;
    public path: string;
    public type: number;
    public width: number;
    public height: number;
    public slotIndex: number;
    public hasTexture: boolean;
    public textureId: number;

    constructor (info: SpineAttachmentInfo) {
        this.name = info.name ?? '';
        this.path = info.path ?? '';
        this.type = info.type;
        this.width = info.width;
        this.height = info.height;
        this.slotIndex = info.slotIndex;
        this.hasTexture = info.hasTexture;
        this.textureId = info.textureId;
    }
}

/**
 * @en The parsed skeleton data (compat wrapper for spine.SkeletonData). Wraps a
 * Data handle; exposes skins/animations arrays and name queries.
 * @zh 解析后的骨骼数据（spine.SkeletonData 的兼容包装）。包装 Data 句柄，
 * 暴露 skins/animations 数组和名称查询。
 */
export class RuntimeSkeletonData {
    private _data: number;

    constructor (dataHandle: number) {
        this._data = dataHandle;
    }

    get data (): number { return this._data; }

    get skins (): Skin[] {
        const b = spine();
        const count = b.dataSkinCount(this._data);
        const out: Skin[] = [];
        for (let i = 0; i < count; i++) {
            const name = b.dataSkinName(this._data, i);
            if (name) out.push(new Skin(name));
        }
        return out;
    }

    get animations (): Animation[] {
        const b = spine();
        const count = b.dataAnimationCount(this._data);
        const out: Animation[] = [];
        for (let i = 0; i < count; i++) {
            const name = b.dataAnimationName(this._data, i);
            if (name) out.push(new Animation(name, b.dataAnimationDuration(this._data, name)));
        }
        return out;
    }

    findAnimation (name: string): Animation | null {
        if (spine().dataHasAnimation(this._data, name)) {
            return new Animation(name, spine().dataAnimationDuration(this._data, name));
        }
        return null;
    }

    findSkin (name: string): Skin | null {
        return spine().dataHasSkin(this._data, name) ? new Skin(name) : null;
    }
}

/*
 Copyright (c) 2026
 SpineRuntime TS wrapper: wraps the wasm spine-runtime (embind binding) for the
 engine. Mirrors spine-cpp 4.3 + SpineRuntime.cpp; shared by web and native.
*/


import { error } from 'cc';
import * as cc from 'cc';
import { EDITOR, NATIVE, PREVIEW } from 'cc/env';

// ---------------------------------------------------------------------------
// Types (matching the embind return structures in spine-runtime-bindings.cpp)
// ---------------------------------------------------------------------------
export interface SpineRenderData {
    vertexCount: number;
    indexCount: number;
    vertexStrideBytes: number;
    vPtr: number;      // byte offset into the heap (vertices)
    iPtr: number;      // byte offset into the heap (indices)
    segments: { indexOffset: number; indexCount: number; blendMode: number; textureId: number }[];
    segmentCount: number;
    indexOverflow: boolean;
}

export interface SpineTexturePage {
    textureId: number;
    width: number;
    height: number;
    minFilter: number;
    magFilter: number;
    uWrap: number;
    vWrap: number;
    premultipliedAlpha: boolean;
}

export interface SpineBoneInfo {
    index: number;
    parentIndex: number;
    name: string;
    active: boolean;
    x: number; y: number;
    rotation: number;
    scaleX: number; scaleY: number;
    shearX: number; shearY: number;
    a: number; b: number; c: number; d: number;
    worldX: number; worldY: number;
}

export interface SpineSlotInfo {
    index: number;
    boneIndex: number;
    name: string;
    attachmentName: string;
    blendMode: number;
    colorR: number; colorG: number; colorB: number; colorA: number;
    hasDarkColor: boolean;
    darkR: number; darkG: number; darkB: number; darkA: number;
}

export interface SpineEventInfo {
    type: number;
    track: number;
    trackIndex: number;
    animationName: string;
    trackTime: number;
    animationEnd: number;
    eventName: string;
    eventTime: number;
    intValue: number;
    floatValue: number;
    stringValue: string;
    audioPath: string;
    volume: number;
    balance: number;
}

export interface SpineAttachmentInfo {
    slotIndex: number;
    name: string;
    path: string;
    type: number;
    worldVerticesLength: number;
    width: number;
    height: number;
    textureId: number;
    hasTexture: boolean;
}

export interface SpineTrackInfo {
    handle: number;
    next: number;
    mixingFrom: number;
    mixingTo: number;
    trackIndex: number;
    animationName: string;
    loop: boolean;
    reverse: boolean;
    additive: boolean;
    shortestRotation: boolean;
    complete: boolean;
    emptyAnimation: boolean;
    wasApplied: boolean;
    nextReady: boolean;
    delay: number;
    trackTime: number;
    trackEnd: number;
    animationStart: number;
    animationEnd: number;
    animationLast: number;
    animationTime: number;
    timeScale: number;
    alpha: number;
    mixTime: number;
    mixDuration: number;
    trackComplete: number;
    eventThreshold: number;
    mixAttachmentThreshold: number;
    alphaAttachmentThreshold: number;
    mixDrawOrderThreshold: number;
}

// All functions exported by the embind binding (see wasm/spine-runtime-bindings.cpp)
export interface SpineRuntimeBinding {
    HEAPU8: Uint8Array;
    // Native (JSB) only: no HEAPU8 there, so TS allocates and registers a staging
    // buffer; runtimeRenderData copies into it and vPtr/iPtr are offsets into it,
    // with the same shape as wasm.
    setRenderBuffer? (u8: Uint8Array): void;
    getRenderDataBuffer? (): Uint8Array | undefined;
    HEAPF32: Float32Array;
    // Data
    createDataJson(json: string, atlas: string, texNames: string[], scale: number): number;
    lastError(): string;
    disposeData(handle: number): void;
    dataWidth(handle: number): number;
    dataHeight(handle: number): number;
    dataX(handle: number): number;
    dataY(handle: number): number;
    dataVersion(handle: number): string;
    dataAnimationCount(handle: number): number;
    dataAnimationName(handle: number, index: number): string;
    dataHasAnimation(handle: number, name: string): boolean;
    dataAnimationDuration(handle: number, name: string): number;
    dataSkinCount(handle: number): number;
    dataSkinName(handle: number, index: number): string;
    dataHasSkin(handle: number, name: string): boolean;
    dataTexturePageCount(handle: number): number;
    dataTexturePage(handle: number, index: number): SpineTexturePage | null;
    // Runtime
    createRuntime(dataHandle: number): number;
    disposeRuntime(handle: number): void;
    runtimePlay(handle: number, name: string, loop: boolean): void;
    runtimeSetAnimation(handle: number, track: number, name: string, loop: boolean): number;
    runtimeAddAnimation(handle: number, track: number, name: string, loop: boolean, delay: number): number;
    runtimeSetEmptyAnimation(handle: number, track: number, mixDuration: number): void;
    runtimeClearTrack(handle: number, track: number): void;
    runtimeClearTracks(handle: number): void;
    runtimeGetCurrent(handle: number, track: number): number;
    runtimeAddEmptyAnimation(handle: number, track: number, mixDuration: number, delay: number): void;
    runtimeSetEmptyAnimations(handle: number, mixDuration: number): void;
    runtimeSetMix(handle: number, from: string, to: string, duration: number): void;
    runtimeGetTrackInfo(handle: number, trackHandle: number): SpineTrackInfo | null;
    runtimeSetTrackLoop(handle: number, trackHandle: number, loop: boolean): boolean;
    runtimeSetTrackReverse(handle: number, trackHandle: number, reverse: boolean): boolean;
    runtimeSetTrackAdditive(handle: number, trackHandle: number, additive: boolean): boolean;
    runtimeSetTrackDelay(handle: number, trackHandle: number, delay: number): boolean;
    runtimeSetTrackTime(handle: number, trackHandle: number, time: number): boolean;
    runtimeSetTrackEnd(handle: number, trackHandle: number, end: number): boolean;
    runtimeSetTrackTimeScale(handle: number, trackHandle: number, timeScale: number): boolean;
    runtimeSetTrackAlpha(handle: number, trackHandle: number, alpha: number): boolean;
    runtimeSetTrackAnimationRange(handle: number, trackHandle: number, start: number, end: number, last: number): boolean;
    runtimeSetTrackMixDuration(handle: number, trackHandle: number, duration: number): boolean;
    runtimeSetTrackMixDuration3(handle: number, trackHandle: number, duration: number, delay: number): boolean;
    runtimeSetTrackMixTime(handle: number, trackHandle: number, mixTime: number): boolean;
    runtimeSetTrackThresholds(handle: number, trackHandle: number, event: number, attachment: number, drawOrder: number): boolean;
    runtimeSetTrackShortestRotation(handle: number, trackHandle: number, shortest: boolean): boolean;
    runtimeSetTrackAlphaAttachmentThreshold(handle: number, trackHandle: number, threshold: number): boolean;
    runtimeResetTrackRotationDirections(handle: number, trackHandle: number): boolean;
    runtimeSetSkin(handle: number, name: string): boolean;
    runtimeSetToSetupPose(handle: number): void;
    runtimeSetBonesToSetupPose(handle: number): void;
    runtimeSetSlotsToSetupPose(handle: number): void;
    runtimeUpdateWorldTransform(handle: number): void;
    runtimeSetParams(handle: number, timeScale: number, r: number, g: number, b: number, a: number,
                     premultipliedAlpha: boolean, useTint: boolean): void;
    // Separable update pipeline, reserved for Cocos culling (not wired yet):
    // advance state, update the world pose without geometry (bounds via
    // runtimeGetBounds), then rebuild geometry only when visible.
    runtimeUpdateAnimation(handle: number, dt: number): void;
    runtimeUpdatePose(handle: number, dt: number): void;
    runtimeExtractRenderData(handle: number): void;
    runtimeUpdateRenderData(handle: number): void;
    runtimeUpdate(handle: number, dt: number): void;
    runtimeSetOutputTransform(handle: number, a: number, b: number, c: number, d: number, tx: number, ty: number): void;
    runtimeRenderData(handle: number): SpineRenderData;
    runtimeFindAnimation(handle: number, name: string): boolean;
    runtimeBoneCount(handle: number): number;
    runtimeBoneName(handle: number, index: number): string;
    runtimeFindBoneIndex(handle: number, name: string): number;
    runtimeGetBone(handle: number, index: number): SpineBoneInfo | null;
    runtimeGetBoneByName(handle: number, name: string): SpineBoneInfo | null;
    runtimeSetBoneLocal(handle: number, name: string, x: number, y: number, rotation: number,
                        scaleX: number, scaleY: number, shearX: number, shearY: number): boolean;
    runtimeBoneWorldToLocal(handle: number, name: string, worldX: number, worldY: number): { x: number; y: number } | null;
    runtimeBoneLocalToWorld(handle: number, name: string, localX: number, localY: number): { x: number; y: number } | null;
    runtimeFindBone(handle: number, name: string): { a: number; b: number; c: number; d: number; worldX: number; worldY: number } | null;
    runtimeFindSlotIndex(handle: number, name: string): number;
    runtimeGetSlot(handle: number, index: number): SpineSlotInfo | null;
    runtimeGetSlotByName(handle: number, name: string): SpineSlotInfo | null;
    runtimeSetSlotColor(handle: number, name: string, r: number, g: number, b: number, a: number): boolean;
    runtimeSetAttachment(handle: number, slotName: string, attachmentName: string): boolean;
    runtimeGetAttachment(handle: number, slotName: string, attachmentName: string): SpineAttachmentInfo | null;
    runtimeGetCurrentAttachment(handle: number, slotName: string): SpineAttachmentInfo | null;
    runtimeSetSlotTexture(handle: number, slotName: string, textureId: number): boolean;
    runtimeResizeSlotRegion(handle: number, slotName: string, width: number, height: number, createNew: boolean): boolean;
    runtimeSetSlotsRange(handle: number, startSlotIndex: number, endSlotIndex: number): void;
    runtimeGetBounds(handle: number): { x: number; y: number; width: number; height: number } | null;
    runtimeSetPaused(handle: number, paused: boolean): void;
    runtimeSetEventListener(handle: number, cb: (ev: SpineEventInfo) => void): void;
    runtimeSetListener(handle: number, cb: (eventType: number, animationName: string, trackIndex: number) => void): void;
}

// ---------------------------------------------------------------------------
// Module loading
// ---------------------------------------------------------------------------
let binding: SpineRuntimeBinding | null = null;
let loadPromise: Promise<SpineRuntimeBinding> | null = null;

// Native (JSB): the binding is a global object registered by
// jsb_spineruntime_manual.cpp. Web: undefined, so we load the wasm module.
function nativeBinding (): SpineRuntimeBinding | null {
    const g = globalThis as any;
    return g && g.spineruntime ? g.spineruntime as SpineRuntimeBinding : null;
}

/**
 * Get the loaded runtime binding (synchronous). Throws if the binding is not
 * ready. Call loadSpineRuntime() first (web) or let the JSB global resolve
 * (native).
 */
export function spine (): SpineRuntimeBinding {
    if (!binding && NATIVE) {
        binding = nativeBinding();
    }
    if (!binding) {
        throw new Error('[SpineRuntime] not loaded yet, call loadSpineRuntime() first');
    }
    return binding;
}

// cc.wasm is the engine's packaged cross-platform wasm interface (pal/wasm),
// re-exported by the custom engine (see exports/webassembly.ts). All non-native
// platforms use this one WASM path. Native platforms never use it.
interface WasmPal {
    instantiateWasm (wasmUrl: string, importObject: WebAssembly.Imports): Promise<WebAssembly.WebAssemblyInstantiatedSource>;
}
function ccWasm (): WasmPal | undefined {
    return (cc as any).wasm;
}

// Published web/mini-game builds read from `cocos-js/`. Editor and browser
// preview resolve `external:` through the editor's engine-external endpoint.
// Native platforms never reach this path: they require the JSB binding.
const WASM_BINARY = EDITOR || PREVIEW
    ? 'external:spine-runtime.wasm'
    : 'spine-runtime.wasm';

// Load the CJS glue factory (MODULARIZE, no embedded wasm). gen-glue-file.js
// converted the Emscripten ESM output to CJS (module.exports) because the build
// treats mounted .js as CJS. The .wasm is instantiated through the provided
// `instantiateWasm` hook so it can be a separate file — required by mini-game
// platforms whose WXWebAssembly only accepts a file path.
async function loadWasmFactory (): Promise<(moduleOptions?: any) => Promise<any>> {
    // Cocos transforms this split point even though its generated tsc config
    // still declares `module: ES2015`, where TypeScript rejects import().
    // @ts-ignore Cocos build pipeline supports dynamic import.
    const glue = await import('./spine-runtime.js');
    return glue.default;
}

// Instantiate the separate .wasm file through the engine's packaged interface.
// The Emscripten hook is the old single-callback form
// `instantiateWasm(imports, successCallback)` with `successCallback(instance)`.
// NOTE: the Promise returned by the instantiateWasm hook cannot be caught by the
// glue (same caveat as the engine's own spine/physx adapters), so failures must
// reject an outer Promise we own rather than throw inside the hook.
async function loadWasmBinding (): Promise<SpineRuntimeBinding> {
    const factory = await loadWasmFactory();
    const pal = ccWasm();
    if (!pal) {
        throw new Error('[SpineRuntime] cc.wasm is unavailable. Enable the WebAssembly module in '
            + 'Project Settings -> Feature Cropping (项目设置 -> 功能裁剪), and make sure the project '
            + 'uses the custom engine that exports cc.wasm');
    }
    return await new Promise<SpineRuntimeBinding>((resolve, reject) => {
        factory({
            instantiateWasm (imports: WebAssembly.Imports, successCallback: (instance: WebAssembly.Instance) => void) {
                pal.instantiateWasm(WASM_BINARY, imports).then((result) => {
                    successCallback(result.instance);
                }).catch((err) => {
                    reject(new Error(`[SpineRuntime] instantiateWasm failed: ${err}`));
                });
            },
        }).then((inst: any) => {
            resolve(inst as SpineRuntimeBinding);
        }).catch((err) => {
            reject(err);
        });
    });
}

export async function loadSpineRuntime (): Promise<SpineRuntimeBinding> {
    if (binding) return binding;
    if (!loadPromise) {
        loadPromise = (async () => {
            try {
                if (NATIVE) {
                    const native = nativeBinding();
                    if (!native) {
                        throw new Error('[SpineRuntime] native JSB binding is unavailable; WASM is not supported on native platforms');
                    }
                    binding = native;
                    return binding;
                }
                binding = await loadWasmBinding();
                return binding;
            } catch (e) {
                loadPromise = null;
                error(`[SpineRuntime] load failed: ${e}`);
                throw e;
            }
        })();
    }
    return loadPromise;
}

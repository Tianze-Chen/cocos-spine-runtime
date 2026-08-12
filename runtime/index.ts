/*
 Copyright (c) 2026
 sp.spine — SpineRuntime driven spine component (spine-cpp 4.3). Plugin entry.
*/

import { ccenum } from 'cc';

export * from './spine';
export * from './spine-data';
export * from './runtime-data';
export * from './runtime-objects';

export { spine, loadSpineRuntime } from './bindings';
export type {
    SpineRuntimeBinding,
    SpineRenderData,
    SpineTrackInfo,
    SpineBoneInfo,
    SpineSlotInfo,
    SpineEventInfo,
    SpineAttachmentInfo,
    SpineTexturePage,
} from './bindings';

/**
 * @en The event type of spine skeleton animation.
 * @zh 骨骼动画事件类型。
 */
export enum AnimationEventType {
    START = 0,
    INTERRUPT = 1,
    END = 2,
    COMPLETE = 3,
    DISPOSE = 4,
    EVENT = 5,
}
ccenum(AnimationEventType);

/****************************************************************************
 Copyright (c) 2026
 SpineRuntime - Cocos-facing realtime Spine runtime abstraction.
****************************************************************************/
#pragma once

#include <cstddef>
#include <cstdint>

#ifndef SPINERUNTIME_API
    #define SPINERUNTIME_API
#endif

namespace spineruntime {

// All pointers in RenderData are borrowed and remain valid until the next
// extractRenderData()/updateRenderData()/update() call or until the Runtime is disposed.
struct RenderData {
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t vertexStrideBytes = 24; // 24: V3F_T2F_C4B, 28: V3F_T2F_C4B_C4B
    const float* vertices = nullptr;
    const uint16_t* indices = nullptr;

    struct Segment {
        uint32_t indexOffset = 0;
        uint32_t indexCount = 0;
        uint8_t blendMode = 0; // 0 Normal / 1 Additive / 2 Multiply / 3 Screen
        uint32_t textureId = 0;
    };
    const Segment* segments = nullptr;
    uint32_t segmentCount = 0;

    enum ShapeType : uint8_t {
        SHAPE_REGION = 0,
        SHAPE_MESH = 1,
    };
    struct Shape {
        uint32_t vertexOffset = 0;
        uint32_t vertexCount = 0;
        uint32_t indexOffset = 0;
        uint32_t indexCount = 0;
        int32_t slotIndex = -1;
        uint8_t type = SHAPE_REGION;
    };
    const Shape* shapes = nullptr;
    uint32_t shapeCount = 0;

    // A single Cocos Spine model uses uint16 indices. If an asset exceeds that
    // limit, data before the overflowing slot is returned and this flag is set.
    bool indexOverflow = false;
};

struct TexturePageInfo {
    uint32_t textureId = 0;
    int width = 0;
    int height = 0;
    int minFilter = 0;
    int magFilter = 0;
    int uWrap = 0;
    int vWrap = 0;
    bool premultipliedAlpha = false;
};

// Parsed atlas and skeleton data. Data is internally reference counted, so a
// Runtime remains valid if the caller releases its Data reference first.
class SPINERUNTIME_API Data {
public:
    // JSON may be NUL terminated with len == 0, or a non-NUL-terminated buffer
    // with an explicit len. Binary data always requires a non-zero len.
    static Data* create(const void* skel, size_t len, bool isBinary,
                        const char* atlas,
                        const char* const* texNames, int nTex,
                        float scale = 1.0F);
    static const char* lastError();

    // Releases the caller's reference. The pointer must not be used afterward.
    void dispose();

    float width() const;
    float height() const;
    float x() const;
    float y() const;
    const char* version() const;

    int animationCount() const;
    const char* animationName(int index) const;
    bool hasAnimation(const char* name) const;
    float animationDuration(const char* name) const;

    int skinCount() const;
    const char* skinName(int index) const;
    bool hasSkin(const char* name) const;

    int texturePageCount() const;
    bool texturePage(int index, TexturePageInfo* out) const;

private:
    friend class Runtime;
    Data();
    ~Data();
    Data(const Data&) = delete;
    Data& operator=(const Data&) = delete;
    void retain() const;
    void release() const;
    struct Impl;
    Impl* _impl;
};

class SPINERUNTIME_API Runtime {
public:
    using TrackHandle = uintptr_t;
    static constexpr TrackHandle INVALID_TRACK = 0;

    static Runtime* create(const Data* data);
    void dispose();

    struct Params {
        float timeScale = 1.0F;
        float colorR = 1.0F;
        float colorG = 1.0F;
        float colorB = 1.0F;
        float colorA = 1.0F;
        bool premultipliedAlpha = false;
        bool useTint = false;
    };
    void setParams(const Params& params);
    void setPaused(bool paused);
    bool paused() const;

    // Compatibility helpers for the original POC (track 0).
    void play(const char* animation, bool loop);
    void addAnimation(const char* animation, bool loop, float delay);

    // Full multi-track animation control. A handle is valid until its DISPOSE
    // event. INVALID_TRACK means the animation was not found or input was bad.
    TrackHandle setAnimation(int trackIndex, const char* animation, bool loop);
    TrackHandle addAnimation(int trackIndex, const char* animation, bool loop, float delay);
    TrackHandle setEmptyAnimation(int trackIndex, float mixDuration);
    TrackHandle addEmptyAnimation(int trackIndex, float mixDuration, float delay);
    void setEmptyAnimations(float mixDuration);
    TrackHandle getCurrent(int trackIndex) const;
    void clearTrack(int trackIndex);
    void clearTracks();
    bool findAnimation(const char* name) const;
    void setMix(const char* from, const char* to, float duration);

    struct TrackInfo {
        TrackHandle handle = INVALID_TRACK;
        TrackHandle next = INVALID_TRACK;
        TrackHandle mixingFrom = INVALID_TRACK;
        TrackHandle mixingTo = INVALID_TRACK;
        int trackIndex = -1;
        const char* animationName = nullptr; // borrowed while the handle is valid
        bool loop = false;
        bool reverse = false;
        bool additive = false;
        bool shortestRotation = false;
        bool complete = false;
        bool emptyAnimation = false;
        bool wasApplied = false;
        bool nextReady = false;
        float delay = 0.0F;
        float trackTime = 0.0F;
        float trackEnd = 0.0F;
        float animationStart = 0.0F;
        float animationEnd = 0.0F;
        float animationLast = 0.0F;
        float animationTime = 0.0F;
        float timeScale = 1.0F;
        float alpha = 1.0F;
        float mixTime = 0.0F;
        float mixDuration = 0.0F;
        float trackComplete = 0.0F;
        float eventThreshold = 0.0F;
        float mixAttachmentThreshold = 0.0F;
        float alphaAttachmentThreshold = 0.0F;
        float mixDrawOrderThreshold = 0.0F;
    };
    bool getTrackInfo(TrackHandle handle, TrackInfo* out) const;
    bool setTrackLoop(TrackHandle handle, bool loop);
    bool setTrackReverse(TrackHandle handle, bool reverse);
    bool setTrackAdditive(TrackHandle handle, bool additive);
    bool setTrackShortestRotation(TrackHandle handle, bool shortest);
    bool setTrackDelay(TrackHandle handle, float delay);
    bool setTrackTime(TrackHandle handle, float trackTime);
    bool setTrackEnd(TrackHandle handle, float trackEnd);
    bool setTrackTimeScale(TrackHandle handle, float timeScale);
    bool setTrackAlpha(TrackHandle handle, float alpha);
    bool setTrackAnimationRange(TrackHandle handle, float start, float end, float last);
    bool setTrackMixTime(TrackHandle handle, float mixTime);
    bool setTrackMixDuration(TrackHandle handle, float duration);
    bool setTrackMixDuration(TrackHandle handle, float duration, float delay);
    bool setTrackThresholds(TrackHandle handle, float eventThreshold,
                            float attachmentThreshold, float drawOrderThreshold);
    bool setTrackAlphaAttachmentThreshold(TrackHandle handle, float threshold);
    bool resetTrackRotationDirections(TrackHandle handle);

    bool setSkin(const char* name);
    void setToSetupPose();
    void setBonesToSetupPose();
    void setSlotsToSetupPose();
    void updateWorldTransform();
    enum PhysicsMode {
        PHYSICS_NONE = 0,
        PHYSICS_RESET = 1,
        PHYSICS_UPDATE = 2,
        PHYSICS_POSE = 3,
    };
    void updateWorldTransform(PhysicsMode physicsMode);
    void resetPhysics();

    void setSkeletonTransform(float x, float y, float scaleX, float scaleY);
    void setPhysics(float windX, float windY, float gravityX, float gravityY);
    void physicsTranslate(float x, float y);
    void physicsRotate(float x, float y, float degrees);
    float skeletonTime() const;

    struct BoneInfo {
        int index = -1;
        int parentIndex = -1;
        const char* name = nullptr;
        bool active = false;
        float length = 0.0F;
        float x = 0.0F;
        float y = 0.0F;
        float rotation = 0.0F;
        float scaleX = 1.0F;
        float scaleY = 1.0F;
        float shearX = 0.0F;
        float shearY = 0.0F;
        float a = 1.0F;
        float b = 0.0F;
        float c = 0.0F;
        float d = 1.0F;
        float worldX = 0.0F;
        float worldY = 0.0F;
    };
    int boneCount() const;
    int findBoneIndex(const char* name) const;
    const char* boneName(int index) const;
    bool getBone(int index, BoneInfo* out) const;
    bool getBone(const char* name, BoneInfo* out) const;
    bool setBoneLocal(const char* name, float x, float y, float rotation,
                      float scaleX, float scaleY, float shearX, float shearY);
    bool boneWorldToLocal(const char* name, float worldX, float worldY, float* out2) const;
    bool boneLocalToWorld(const char* name, float localX, float localY, float* out2) const;

    // Legacy socket helper: a,b,c,d,worldX,worldY.
    bool findBone(const char* name, float* out6) const;
    bool boneTransform(int index, float* out6) const;

    struct SlotInfo {
        int index = -1;
        int boneIndex = -1;
        const char* name = nullptr;
        const char* attachmentName = nullptr;
        uint8_t blendMode = 0;
        float colorR = 1.0F;
        float colorG = 1.0F;
        float colorB = 1.0F;
        float colorA = 1.0F;
        bool hasDarkColor = false;
        float darkR = 0.0F;
        float darkG = 0.0F;
        float darkB = 0.0F;
        float darkA = 0.0F;
    };
    int slotCount() const;
    int findSlotIndex(const char* name) const;
    const char* slotName(int index) const;
    bool getSlot(int index, SlotInfo* out) const;
    bool getSlot(const char* name, SlotInfo* out) const;
    bool setSlotColor(const char* name, float r, float g, float b, float a);
    bool setAttachment(const char* slotName, const char* attachmentName);

    enum AttachmentType : uint8_t {
        ATTACHMENT_REGION = 0,
        ATTACHMENT_BOUNDING_BOX = 1,
        ATTACHMENT_MESH = 2,
        ATTACHMENT_PATH = 4,
        ATTACHMENT_POINT = 5,
        ATTACHMENT_CLIPPING = 6,
        ATTACHMENT_UNKNOWN = 255,
    };
    struct AttachmentInfo {
        int slotIndex = -1;
        const char* name = nullptr;
        const char* path = nullptr;
        uint8_t type = ATTACHMENT_UNKNOWN;
        size_t worldVerticesLength = 0;
        float width = 0.0F;
        float height = 0.0F;
        uint32_t textureId = 0;
        bool hasTexture = false;
    };
    // Queries an attachment resolved through the current/default skin, or the
    // attachment currently applied to a slot (including local overrides).
    bool getAttachment(const char* slotName, const char* attachmentName, AttachmentInfo* out) const;
    bool getCurrentAttachment(const char* slotName, AttachmentInfo* out) const;

    // Restricts rendering to slots after startSlotIndex and before endSlotIndex,
    // matching the existing Cocos SkeletonRenderer semantics. -1 disables a bound.
    void setSlotsRange(int startSlotIndex, int endSlotIndex);

    // Local skin replacement used by Cocos setSlotTexture(). resizeSlotRegion()
    // prepares attachment geometry/UVs; setSlotTexture() changes the engine ID.
    bool resizeSlotRegion(const char* slotName, float width, float height, bool createNewAttachment);
    bool setSlotTexture(const char* slotName, uint32_t textureId);
    bool clearSlotTexture(const char* slotName);

    // Computes the currently visible attachment's positions. Returns the
    // required float count (including offset/stride holes), or 0 if unsupported.
    size_t computeSlotWorldVertices(const char* slotName, float* out,
                                    size_t outFloatCount, size_t offset = 0,
                                    size_t stride = 2) const;
    bool getBounds(float* out4) const; // x, y, width, height

    // Animation, world-pose update, and render extraction are separable for
    // Cocos culling. updatePose() keeps physics and socket transforms current
    // without generating geometry. extractRenderData() assumes the world pose
    // is current and only rebuilds the borrowed render buffers.
    void updateAnimation(float dt);
    void updatePose(float dt);
    void extractRenderData();
    void updateRenderData();
    void update(float dt);

    // Applies a 2D affine to the extracted render vertices (spine-space -> render
    // space): x' = a*x + b*y + tx, y' = c*x + d*y + ty. Identity by default; a
    // non-identity transform bakes the node/world transform into the vertex
    // positions so batching needs no per-node transform uniform.
    void setOutputTransform(float a, float b, float c, float d, float tx, float ty);

    const RenderData& renderData() const;

    enum EventType {
        START = 0,
        INTERRUPT = 1,
        END = 2,
        DISPOSE = 3,
        COMPLETE = 4,
        EVENT = 5,
    };
    struct EventInfo {
        int type = START;
        TrackHandle track = INVALID_TRACK;
        int trackIndex = -1;
        const char* animationName = nullptr;
        float trackTime = 0.0F;
        float animationEnd = 0.0F;

        // Populated for EVENT, otherwise zero/null. Strings are callback-scoped.
        const char* eventName = nullptr;
        float eventTime = 0.0F;
        int intValue = 0;
        float floatValue = 0.0F;
        const char* stringValue = nullptr;
        const char* audioPath = nullptr;
        float volume = 0.0F;
        float balance = 0.0F;
    };
    using Listener = void (*)(void* user, int eventType,
                              const char* animationName, int trackIndex);
    using EventListener = void (*)(void* user, const EventInfo* event);
    void setListener(Listener callback, void* user); // legacy
    void setEventListener(EventListener callback, void* user);

private:
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    struct Impl;
    Impl* _impl;
    static void collectRenderData(Impl& impl);
};

} // namespace spineruntime

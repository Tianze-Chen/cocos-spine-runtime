#include "SpineRuntime.h"

#include <spine/spine.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

// Standalone POC builds need a default extension. Engine integration already
// provides one in spine-cocos2dx.cpp and can define this macro to avoid a
// duplicate symbol.
#ifndef SPINERUNTIME_EXTERNAL_SPINE_EXTENSION
namespace spine {
SpineExtension* getDefaultExtension() {
    static DefaultSpineExtension extension;
    return &extension;
}
} // namespace spine
#endif

namespace spineruntime {
namespace {

thread_local std::string gLastError;
std::atomic<Runtime::TrackHandle> gNextTrackHandle{1};

void setLastError(const char* message) {
    gLastError = message ? message : "Unknown Spine runtime error";
}

std::string toStdString(const spine::String& value) {
    const char* buffer = value.buffer();
    return buffer ? std::string(buffer, value.length()) : std::string();
}

uint8_t toByte(float value) {
    value = std::max(0.0F, std::min(1.0F, value));
    return static_cast<uint8_t>(value * 255.0F);
}

uint32_t packColor(float r, float g, float b, float a) {
    return static_cast<uint32_t>(toByte(r)) |
           (static_cast<uint32_t>(toByte(g)) << 8U) |
           (static_cast<uint32_t>(toByte(b)) << 16U) |
           (static_cast<uint32_t>(toByte(a)) << 24U);
}

int mapEventType(spine::EventType type) {
    switch (type) {
        case spine::EventType_Start: return Runtime::START;
        case spine::EventType_Interrupt: return Runtime::INTERRUPT;
        case spine::EventType_End: return Runtime::END;
        case spine::EventType_Dispose: return Runtime::DISPOSE;
        case spine::EventType_Complete: return Runtime::COMPLETE;
        case spine::EventType_Event: return Runtime::EVENT;
    }
    return Runtime::EVENT;
}

// textureId for a segment = the atlas page index (page->texture), which
// SpineTextureLoader sets from the texNames order. TextureRegion::getRendererObject()
// is never set (NULL), so derive it from the region's AtlasPage instead.
static uint32_t textureIdOfRegion(spine::TextureRegion* region) {
    if (region && region->getRTTI().isExactly(spine::AtlasRegion::rtti)) {
        spine::AtlasRegion* ar = static_cast<spine::AtlasRegion*>(region);
        if (ar->getPage()) {
            return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ar->getPage()->texture));
        }
    }
    return 0;
}

class SpineTextureLoader final : public spine::TextureLoader {
public:
    std::vector<std::string> textureNames;
    bool missingTexture = false;
    std::string missingTextureName;

    void load(spine::AtlasPage& page, const spine::String& path) override {
        const std::string texturePath = toStdString(path);
        uint32_t textureId = std::numeric_limits<uint32_t>::max();
        for (size_t i = 0; i < textureNames.size(); ++i) {
            if (textureNames[i] == texturePath) {
                textureId = static_cast<uint32_t>(i);
                break;
            }
        }
        if (textureId == std::numeric_limits<uint32_t>::max()) {
            missingTexture = true;
            missingTextureName = texturePath;
        }
        page.texture = reinterpret_cast<void*>(static_cast<uintptr_t>(textureId));
    }

    void unload(void*) override {}
};

} // namespace

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------
struct Data::Impl {
    mutable std::atomic<uint32_t> references{1};
    SpineTextureLoader textureLoader;
    spine::Atlas* atlas = nullptr;
    spine::AtlasAttachmentLoader* attachmentLoader = nullptr;
    spine::SkeletonData* skeletonData = nullptr;
    std::vector<TexturePageInfo> texturePages;
    // Texture regions created by shared resizeSlotRegion() calls live with Data.
    std::vector<spine::TextureRegion*> runtimeTextureRegions;

    ~Impl() {
        delete skeletonData;
        delete attachmentLoader;
        delete atlas;
        for (spine::TextureRegion* region : runtimeTextureRegions) delete region;
    }
};

Data::Data() : _impl(new Impl()) {}
Data::~Data() { delete _impl; }

Data* Data::create(const void* skel, size_t len, bool isBinary,
                   const char* atlasText,
                   const char* const* texNames, int nTex,
                   float scale) {
    gLastError.clear();
    if (!skel) {
        setLastError("Skeleton data is null");
        return nullptr;
    }
    if (isBinary && len == 0) {
        setLastError("Binary skeleton data has zero length");
        return nullptr;
    }
    if (!atlasText || atlasText[0] == '\0') {
        setLastError("Atlas text is empty");
        return nullptr;
    }
    if (nTex < 0 || (nTex > 0 && !texNames)) {
        setLastError("Invalid texture name array");
        return nullptr;
    }
    if (!(scale > 0.0F) || !std::isfinite(scale)) {
        setLastError("Skeleton scale must be finite and greater than zero");
        return nullptr;
    }

    Data* data = new Data();
    Impl& impl = *data->_impl;
    for (int i = 0; i < nTex; ++i) {
        if (!texNames[i]) {
            setLastError("Texture name is null");
            data->release();
            return nullptr;
        }
        impl.textureLoader.textureNames.emplace_back(texNames[i]);
    }

    const size_t atlasLength = std::strlen(atlasText);
    if (atlasLength > static_cast<size_t>(std::numeric_limits<int>::max())) {
        setLastError("Atlas text is too large");
        data->release();
        return nullptr;
    }
    impl.atlas = new spine::Atlas(atlasText, static_cast<int>(atlasLength), "", &impl.textureLoader);
    if (impl.textureLoader.missingTexture) {
        gLastError = "Atlas texture not provided: " + impl.textureLoader.missingTextureName;
        data->release();
        return nullptr;
    }
    if (impl.atlas->getPages().size() == 0) {
        setLastError("Atlas contains no pages");
        data->release();
        return nullptr;
    }

    auto& pages = impl.atlas->getPages();
    impl.texturePages.reserve(pages.size());
    for (size_t i = 0; i < pages.size(); ++i) {
        spine::AtlasPage* page = pages[i];
        TexturePageInfo info;
        info.textureId = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(page->texture));
        info.width = page->width;
        info.height = page->height;
        info.minFilter = static_cast<int>(page->minFilter);
        info.magFilter = static_cast<int>(page->magFilter);
        info.uWrap = static_cast<int>(page->uWrap);
        info.vWrap = static_cast<int>(page->vWrap);
        info.premultipliedAlpha = page->pma;
        impl.texturePages.push_back(info);
    }

    impl.attachmentLoader = new spine::AtlasAttachmentLoader(*impl.atlas);
    if (isBinary) {
        if (len > static_cast<size_t>(std::numeric_limits<int>::max())) {
            setLastError("Binary skeleton data is too large");
            data->release();
            return nullptr;
        }
        spine::SkeletonBinary binary(*impl.attachmentLoader);
        binary.setScale(scale);
        impl.skeletonData = binary.readSkeletonData(static_cast<const unsigned char*>(skel),
                                                    static_cast<int>(len));
        if (!impl.skeletonData) setLastError(binary.getError().buffer());
    } else {
        std::string jsonStorage;
        const char* jsonText = static_cast<const char*>(skel);
        if (len != 0) {
            jsonStorage.assign(jsonText, len);
            jsonText = jsonStorage.c_str();
        }
        spine::SkeletonJson json(*impl.attachmentLoader);
        json.setScale(scale);
        impl.skeletonData = json.readSkeletonData(jsonText);
        if (!impl.skeletonData) setLastError(json.getError().buffer());
    }

    if (!impl.skeletonData) {
        if (gLastError.empty()) setLastError("Failed to parse skeleton data");
        data->release();
        return nullptr;
    }
    return data;
}

const char* Data::lastError() { return gLastError.c_str(); }

void Data::retain() const {
    if (_impl) _impl->references.fetch_add(1, std::memory_order_relaxed);
}

void Data::release() const {
    if (_impl && _impl->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete const_cast<Data*>(this);
    }
}

void Data::dispose() { release(); }

float Data::width() const { return _impl && _impl->skeletonData ? _impl->skeletonData->getWidth() : 0.0F; }
float Data::height() const { return _impl && _impl->skeletonData ? _impl->skeletonData->getHeight() : 0.0F; }
float Data::x() const { return _impl && _impl->skeletonData ? _impl->skeletonData->getX() : 0.0F; }
float Data::y() const { return _impl && _impl->skeletonData ? _impl->skeletonData->getY() : 0.0F; }
const char* Data::version() const {
    return _impl && _impl->skeletonData ? _impl->skeletonData->getVersion().buffer() : "";
}

int Data::animationCount() const {
    return _impl && _impl->skeletonData ? static_cast<int>(_impl->skeletonData->getAnimations().size()) : 0;
}

const char* Data::animationName(int index) const {
    if (!_impl || !_impl->skeletonData || index < 0 || index >= animationCount()) return nullptr;
    return _impl->skeletonData->getAnimations()[static_cast<size_t>(index)]->getName().buffer();
}

bool Data::hasAnimation(const char* name) const {
    return _impl && _impl->skeletonData && name &&
           _impl->skeletonData->findAnimation(spine::String(name)) != nullptr;
}

float Data::animationDuration(const char* name) const {
    if (!_impl || !_impl->skeletonData || !name) return 0.0F;
    spine::Animation* animation = _impl->skeletonData->findAnimation(spine::String(name));
    return animation ? animation->getDuration() : 0.0F;
}

int Data::skinCount() const {
    return _impl && _impl->skeletonData ? static_cast<int>(_impl->skeletonData->getSkins().size()) : 0;
}

const char* Data::skinName(int index) const {
    if (!_impl || !_impl->skeletonData || index < 0 || index >= skinCount()) return nullptr;
    return _impl->skeletonData->getSkins()[static_cast<size_t>(index)]->getName().buffer();
}

bool Data::hasSkin(const char* name) const {
    return _impl && _impl->skeletonData && name &&
           _impl->skeletonData->findSkin(spine::String(name)) != nullptr;
}

int Data::texturePageCount() const {
    return _impl ? static_cast<int>(_impl->texturePages.size()) : 0;
}

bool Data::texturePage(int index, TexturePageInfo* out) const {
    if (!_impl || !out || index < 0 || index >= texturePageCount()) return false;
    *out = _impl->texturePages[static_cast<size_t>(index)];
    return true;
}

// ---------------------------------------------------------------------------
// Runtime internals
// ---------------------------------------------------------------------------
struct Runtime::Impl {
    struct TrackSnapshot {
        Runtime::TrackInfo info;
        std::string animationName;
    };

    struct PendingEvent {
        int type = Runtime::START;
        Runtime::TrackHandle track = Runtime::INVALID_TRACK;
        int trackIndex = -1;
        std::string animationName;
        float trackTime = 0.0F;
        float animationEnd = 0.0F;
        std::string eventName;
        float eventTime = 0.0F;
        int intValue = 0;
        float floatValue = 0.0F;
        std::string stringValue;
        std::string audioPath;
        float volume = 0.0F;
        float balance = 0.0F;
    };

    struct SpineListener final : spine::AnimationStateListenerObject {
        Impl* owner = nullptr;
        void callback(spine::AnimationState*, spine::EventType type,
                      spine::TrackEntry* entry, spine::Event* event) override {
            if (owner) owner->queueEvent(type, entry, event);
        }
    };

    struct SlotOverride {
        spine::Attachment* attachment = nullptr;
        spine::Attachment* originalAttachment = nullptr;
        bool ownsAttachment = false;
        bool hasTexture = false;
        uint32_t textureId = 0;
        std::vector<spine::TextureRegion*> ownedRegions;
    };

    Data* data = nullptr;
    spine::Skeleton* skeleton = nullptr;
    spine::AnimationStateData* stateData = nullptr;
    spine::AnimationState* state = nullptr;
    spine::SkeletonClipping* clipper = nullptr;
    SpineListener listener;

    std::vector<float> vertexBuffer;
    std::vector<uint16_t> indexBuffer;
    std::vector<RenderData::Segment> segments;
    std::vector<RenderData::Shape> shapes;
    // Reused only for clipping input. Non-clipped attachments write directly
    // into vertexBuffer, avoiding per-slot heap allocations and copies.
    std::vector<float> scratchWorldVertices;
    RenderData renderData;

    // Output-space affine applied to every vertex at the end of collectRenderData.
    float outA = 1.0F, outB = 0.0F, outC = 0.0F, outD = 1.0F, outTx = 0.0F, outTy = 0.0F;

    float timeScale = 1.0F;
    bool paused = false;
    bool premultipliedAlpha = false;
    bool useTint = false;
    int startSlotIndex = -1;
    int endSlotIndex = -1;

    Runtime::Listener legacyListener = nullptr;
    void* legacyUser = nullptr;
    Runtime::EventListener eventListener = nullptr;
    void* eventUser = nullptr;
    std::vector<PendingEvent> pendingEvents;
    bool dispatchingEvents = false;

    std::unordered_map<Runtime::TrackHandle, spine::TrackEntry*> liveTracks;
    std::unordered_map<spine::TrackEntry*, Runtime::TrackHandle> entryHandles;
    // Kept only while a DISPOSE callback is being delivered, so listeners can
    // still inspect the entry without touching Spine's already-freed object.
    std::unordered_map<Runtime::TrackHandle, TrackSnapshot> disposedTracks;

    std::unordered_map<spine::Slot*, SlotOverride> slotOverrides;

    bool writeAttachmentInfo(spine::Slot& slot, spine::Attachment* attachment,
                             Runtime::AttachmentInfo& out, bool applyOverrideTexture) const {
        if (!attachment) return false;
        out = Runtime::AttachmentInfo{};
        out.slotIndex = slot.getData().getIndex();
        out.name = attachment->getName().buffer();

        const spine::RTTI& rtti = attachment->getRTTI();
        if (rtti.isExactly(spine::RegionAttachment::rtti)) {
            auto* region = static_cast<spine::RegionAttachment*>(attachment);
            out.type = Runtime::ATTACHMENT_REGION;
            out.path = region->getPath().buffer();
            out.worldVerticesLength = 8;
            out.width = region->getWidth();
            out.height = region->getHeight();
            spine::Sequence& sequence = region->getSequence();
            spine::TextureRegion* texture = sequence.getRegion(sequence.resolveIndex(slot.getAppliedPose()));
            if (texture) {
                out.hasTexture = true;
                out.textureId = textureIdOfRegion(texture);
            }
        } else if (rtti.isExactly(spine::MeshAttachment::rtti)) {
            auto* mesh = static_cast<spine::MeshAttachment*>(attachment);
            out.type = Runtime::ATTACHMENT_MESH;
            out.path = mesh->getPath().buffer();
            out.worldVerticesLength = mesh->getWorldVerticesLength();
            out.width = mesh->getWidth();
            out.height = mesh->getHeight();
            spine::Sequence& sequence = mesh->getSequence();
            spine::TextureRegion* texture = sequence.getRegion(sequence.resolveIndex(slot.getAppliedPose()));
            if (texture) {
                out.hasTexture = true;
                out.textureId = textureIdOfRegion(texture);
            }
        } else if (rtti.isExactly(spine::BoundingBoxAttachment::rtti)) {
            out.type = Runtime::ATTACHMENT_BOUNDING_BOX;
            out.worldVerticesLength = static_cast<spine::VertexAttachment*>(attachment)->getWorldVerticesLength();
        } else if (rtti.isExactly(spine::PathAttachment::rtti)) {
            out.type = Runtime::ATTACHMENT_PATH;
            out.worldVerticesLength = static_cast<spine::VertexAttachment*>(attachment)->getWorldVerticesLength();
        } else if (rtti.isExactly(spine::PointAttachment::rtti)) {
            out.type = Runtime::ATTACHMENT_POINT;
        } else if (rtti.isExactly(spine::ClippingAttachment::rtti)) {
            out.type = Runtime::ATTACHMENT_CLIPPING;
            out.worldVerticesLength = static_cast<spine::VertexAttachment*>(attachment)->getWorldVerticesLength();
        }

        if (applyOverrideTexture) {
            auto overrideIter = slotOverrides.find(&slot);
            if (overrideIter != slotOverrides.end() && overrideIter->second.hasTexture) {
                out.hasTexture = true;
                out.textureId = overrideIter->second.textureId;
            }
        }
        return true;
    }

    void writeTrackInfo(spine::TrackEntry& entry, Runtime::TrackHandle handle,
                        Runtime::TrackInfo& out) {
        out = Runtime::TrackInfo{};
        out.handle = handle;
        out.next = registerTrack(entry.getNext());
        out.mixingFrom = registerTrack(entry.getMixingFrom());
        out.mixingTo = registerTrack(entry.getMixingTo());
        out.trackIndex = entry.getTrackIndex();
        out.animationName = entry.getAnimation().getName().buffer();
        out.loop = entry.getLoop();
        out.reverse = entry.getReverse();
        out.additive = entry.getAdditive();
        out.shortestRotation = entry.getShortestRotation();
        out.complete = entry.isComplete();
        out.emptyAnimation = entry.isEmptyAnimation();
        out.wasApplied = entry.wasApplied();
        out.nextReady = entry.isNextReady();
        out.delay = entry.getDelay();
        out.trackTime = entry.getTrackTime();
        out.trackEnd = entry.getTrackEnd();
        out.animationStart = entry.getAnimationStart();
        out.animationEnd = entry.getAnimationEnd();
        out.animationLast = entry.getAnimationLast();
        out.animationTime = entry.getAnimationTime();
        out.timeScale = entry.getTimeScale();
        out.alpha = entry.getAlpha();
        out.mixTime = entry.getMixTime();
        out.mixDuration = entry.getMixDuration();
        out.trackComplete = entry.getTrackComplete();
        out.eventThreshold = entry.getEventThreshold();
        out.mixAttachmentThreshold = entry.getMixAttachmentThreshold();
        out.alphaAttachmentThreshold = entry.getAlphaAttachmentThreshold();
        out.mixDrawOrderThreshold = entry.getMixDrawOrderThreshold();
    }

    Runtime::TrackHandle registerTrack(spine::TrackEntry* entry) {
        if (!entry) return Runtime::INVALID_TRACK;
        auto existing = entryHandles.find(entry);
        if (existing != entryHandles.end()) return existing->second;

        Runtime::TrackHandle handle = gNextTrackHandle.fetch_add(1, std::memory_order_relaxed);
        while (handle == Runtime::INVALID_TRACK || liveTracks.count(handle) != 0 ||
               disposedTracks.count(handle) != 0) {
            handle = gNextTrackHandle.fetch_add(1, std::memory_order_relaxed);
        }
        liveTracks.emplace(handle, entry);
        entryHandles.emplace(entry, handle);
        return handle;
    }

    spine::TrackEntry* resolveTrack(Runtime::TrackHandle handle) const {
        auto iter = liveTracks.find(handle);
        return iter == liveTracks.end() ? nullptr : iter->second;
    }

    void snapshotDisposedTrack(spine::TrackEntry& entry, Runtime::TrackHandle handle) {
        TrackSnapshot snapshot;
        writeTrackInfo(entry, handle, snapshot.info);
        snapshot.animationName = toStdString(entry.getAnimation().getName());
        snapshot.info.animationName = nullptr;
        disposedTracks.insert_or_assign(handle, std::move(snapshot));
        entryHandles.erase(&entry);
        liveTracks.erase(handle);
    }

    void queueEvent(spine::EventType type, spine::TrackEntry* entry, spine::Event* event) {
        PendingEvent pending;
        pending.type = mapEventType(type);
        pending.track = registerTrack(entry);
        if (entry) {
            pending.trackIndex = entry->getTrackIndex();
            pending.animationName = toStdString(entry->getAnimation().getName());
            pending.trackTime = entry->getTrackTime();
            pending.animationEnd = entry->getAnimationEnd();
        }
        if (event) {
            pending.eventName = toStdString(event->getData().getName());
            pending.eventTime = event->getTime();
            pending.intValue = event->getInt();
            pending.floatValue = event->getFloat();
            pending.stringValue = toStdString(event->getString());
            pending.audioPath = toStdString(event->getData().getAudioPath());
            pending.volume = event->getVolume();
            pending.balance = event->getBalance();
        }
        pendingEvents.emplace_back(std::move(pending));
        if (entry && type == spine::EventType_Dispose) {
            snapshotDisposedTrack(*entry, pendingEvents.back().track);
        }
    }

    void dispatchEvents() {
        if (dispatchingEvents) return;
        dispatchingEvents = true;
        while (!pendingEvents.empty()) {
            std::vector<PendingEvent> events;
            events.swap(pendingEvents);
            for (PendingEvent& pending : events) {
                if (legacyListener) {
                    legacyListener(legacyUser, pending.type,
                                   pending.animationName.c_str(), pending.trackIndex);
                }
                if (eventListener) {
                    Runtime::EventInfo info;
                    info.type = pending.type;
                    info.track = pending.track;
                    info.trackIndex = pending.trackIndex;
                    info.animationName = pending.animationName.c_str();
                    info.trackTime = pending.trackTime;
                    info.animationEnd = pending.animationEnd;
                    info.eventName = pending.eventName.empty() ? nullptr : pending.eventName.c_str();
                    info.eventTime = pending.eventTime;
                    info.intValue = pending.intValue;
                    info.floatValue = pending.floatValue;
                    info.stringValue = pending.stringValue.empty() ? nullptr : pending.stringValue.c_str();
                    info.audioPath = pending.audioPath.empty() ? nullptr : pending.audioPath.c_str();
                    info.volume = pending.volume;
                    info.balance = pending.balance;
                    eventListener(eventUser, &info);
                }
                if (pending.type == Runtime::DISPOSE) {
                    disposedTracks.erase(pending.track);
                }
            }
        }
        dispatchingEvents = false;
    }

    void clearSlotOverride(spine::Slot* slot) {
        auto iter = slotOverrides.find(slot);
        if (iter == slotOverrides.end()) return;
        SlotOverride& value = iter->second;
        if (value.ownsAttachment && value.attachment) {
            if (slot->getPose().getAttachment() == value.attachment) {
                slot->getPose().setAttachment(value.originalAttachment);
            }
            if (slot->getAppliedPose().getAttachment() == value.attachment) {
                slot->getAppliedPose().setAttachment(value.originalAttachment);
            }
            delete value.attachment;
            for (spine::TextureRegion* region : value.ownedRegions) delete region;
        }
        slotOverrides.erase(iter);
    }

    ~Impl() {
        legacyListener = nullptr;
        eventListener = nullptr;
        if (state) state->setListener(static_cast<spine::AnimationStateListenerObject*>(nullptr));
        while (!slotOverrides.empty()) clearSlotOverride(slotOverrides.begin()->first);
        delete clipper;
        delete state;
        delete stateData;
        delete skeleton;
        if (data) data->release();
    }
};

Runtime::Runtime() : _impl(new Impl()) {}
Runtime::~Runtime() { delete _impl; }

Runtime* Runtime::create(const Data* data) {
    if (!data || !data->_impl || !data->_impl->skeletonData) return nullptr;

    Runtime* runtime = new Runtime();
    Impl& impl = *runtime->_impl;
    impl.data = const_cast<Data*>(data);
    impl.data->retain();

    spine::SkeletonData* skeletonData = data->_impl->skeletonData;
    impl.skeleton = new spine::Skeleton(*skeletonData);
    if (skeletonData->getDefaultSkin()) impl.skeleton->setSkin(skeletonData->getDefaultSkin());
    impl.skeleton->setupPose();
    impl.skeleton->updateWorldTransform(spine::Physics_Reset);

    impl.clipper = new spine::SkeletonClipping();
    impl.stateData = new spine::AnimationStateData(*skeletonData);
    impl.state = new spine::AnimationState(*impl.stateData);
    impl.listener.owner = &impl;
    impl.state->setListener(&impl.listener);
    return runtime;
}

void Runtime::dispose() { delete this; }

void Runtime::setParams(const Params& params) {
    if (!_impl) return;
    _impl->timeScale = params.timeScale;
    if (_impl->state) _impl->state->setTimeScale(params.timeScale);
    if (_impl->skeleton) {
        _impl->skeleton->setColor(params.colorR, params.colorG, params.colorB, params.colorA);
    }
    _impl->premultipliedAlpha = params.premultipliedAlpha;
    _impl->useTint = params.useTint;
}

void Runtime::setPaused(bool pausedValue) { if (_impl) _impl->paused = pausedValue; }
bool Runtime::paused() const { return _impl && _impl->paused; }

void Runtime::play(const char* animation, bool loop) { (void)setAnimation(0, animation, loop); }
void Runtime::addAnimation(const char* animation, bool loop, float delay) {
    (void)addAnimation(0, animation, loop, delay);
}

Runtime::TrackHandle Runtime::setAnimation(int trackIndex, const char* animation, bool loop) {
    if (!_impl || !_impl->state || !_impl->skeleton || trackIndex < 0 || !animation) return INVALID_TRACK;
    spine::Animation* value = _impl->skeleton->getData().findAnimation(spine::String(animation));
    if (!value) return INVALID_TRACK;
    spine::TrackEntry& entry = _impl->state->setAnimation(static_cast<size_t>(trackIndex), *value, loop);
    const TrackHandle handle = _impl->registerTrack(&entry);
    _impl->state->apply(*_impl->skeleton);
    _impl->dispatchEvents();
    return handle;
}

Runtime::TrackHandle Runtime::addAnimation(int trackIndex, const char* animation, bool loop, float delay) {
    if (!_impl || !_impl->state || !_impl->skeleton || trackIndex < 0 || !animation) return INVALID_TRACK;
    spine::Animation* value = _impl->skeleton->getData().findAnimation(spine::String(animation));
    if (!value) return INVALID_TRACK;
    spine::TrackEntry& entry = _impl->state->addAnimation(static_cast<size_t>(trackIndex), *value, loop, delay);
    const TrackHandle handle = _impl->registerTrack(&entry);
    _impl->dispatchEvents();
    return handle;
}

Runtime::TrackHandle Runtime::setEmptyAnimation(int trackIndex, float mixDuration) {
    if (!_impl || !_impl->state || trackIndex < 0) return INVALID_TRACK;
    spine::TrackEntry& entry = _impl->state->setEmptyAnimation(static_cast<size_t>(trackIndex), mixDuration);
    const TrackHandle handle = _impl->registerTrack(&entry);
    _impl->dispatchEvents();
    return handle;
}

Runtime::TrackHandle Runtime::addEmptyAnimation(int trackIndex, float mixDuration, float delay) {
    if (!_impl || !_impl->state || trackIndex < 0) return INVALID_TRACK;
    spine::TrackEntry& entry = _impl->state->addEmptyAnimation(static_cast<size_t>(trackIndex), mixDuration, delay);
    const TrackHandle handle = _impl->registerTrack(&entry);
    _impl->dispatchEvents();
    return handle;
}

void Runtime::setEmptyAnimations(float mixDuration) {
    if (!_impl || !_impl->state) return;
    _impl->state->setEmptyAnimations(mixDuration);
    _impl->dispatchEvents();
}

Runtime::TrackHandle Runtime::getCurrent(int trackIndex) const {
    if (!_impl || !_impl->state || trackIndex < 0) return INVALID_TRACK;
    return _impl->registerTrack(_impl->state->getTrack(static_cast<size_t>(trackIndex)));
}

void Runtime::clearTrack(int trackIndex) {
    if (!_impl || !_impl->state || trackIndex < 0) return;
    _impl->state->clearTrack(static_cast<size_t>(trackIndex));
    _impl->dispatchEvents();
}

void Runtime::clearTracks() {
    if (!_impl || !_impl->state) return;
    _impl->state->clearTracks();
    if (_impl->skeleton) _impl->skeleton->setupPose();
    _impl->dispatchEvents();
}

bool Runtime::findAnimation(const char* name) const {
    return _impl && _impl->skeleton && name &&
           _impl->skeleton->getData().findAnimation(spine::String(name)) != nullptr;
}

void Runtime::setMix(const char* from, const char* to, float duration) {
    if (!_impl || !_impl->stateData || !_impl->skeleton || !from || !to) return;
    spine::SkeletonData& data = _impl->skeleton->getData();
    if (!data.findAnimation(spine::String(from)) || !data.findAnimation(spine::String(to))) return;
    _impl->stateData->setMix(spine::String(from), spine::String(to), duration);
}

bool Runtime::getTrackInfo(TrackHandle handle, TrackInfo* out) const {
    if (!_impl || handle == INVALID_TRACK || !out) return false;
    if (spine::TrackEntry* entry = _impl->resolveTrack(handle)) {
        _impl->writeTrackInfo(*entry, handle, *out);
        return true;
    }
    auto disposed = _impl->disposedTracks.find(handle);
    if (disposed == _impl->disposedTracks.end()) return false;
    *out = disposed->second.info;
    out->animationName = disposed->second.animationName.c_str();
    return true;
}

bool Runtime::setTrackLoop(TrackHandle handle, bool value) {
    spine::TrackEntry* entry = _impl ? _impl->resolveTrack(handle) : nullptr; if (!entry) return false; entry->setLoop(value); return true;
}
bool Runtime::setTrackReverse(TrackHandle handle, bool value) {
    spine::TrackEntry* entry = _impl ? _impl->resolveTrack(handle) : nullptr; if (!entry) return false; entry->setReverse(value); return true;
}
bool Runtime::setTrackAdditive(TrackHandle handle, bool value) {
    spine::TrackEntry* entry = _impl ? _impl->resolveTrack(handle) : nullptr; if (!entry) return false; entry->setAdditive(value); return true;
}
bool Runtime::setTrackShortestRotation(TrackHandle handle, bool value) {
    spine::TrackEntry* entry = _impl ? _impl->resolveTrack(handle) : nullptr; if (!entry) return false; entry->setShortestRotation(value); return true;
}
bool Runtime::setTrackDelay(TrackHandle handle, float value) {
    spine::TrackEntry* entry = _impl ? _impl->resolveTrack(handle) : nullptr; if (!entry) return false; entry->setDelay(value); return true;
}
bool Runtime::setTrackTime(TrackHandle handle, float value) {
    spine::TrackEntry* entry = _impl ? _impl->resolveTrack(handle) : nullptr; if (!entry) return false; entry->setTrackTime(value); return true;
}
bool Runtime::setTrackEnd(TrackHandle handle, float value) {
    spine::TrackEntry* entry = _impl ? _impl->resolveTrack(handle) : nullptr; if (!entry) return false; entry->setTrackEnd(value); return true;
}
bool Runtime::setTrackTimeScale(TrackHandle handle, float value) {
    spine::TrackEntry* entry = _impl ? _impl->resolveTrack(handle) : nullptr; if (!entry || value < 0.0F) return false; entry->setTimeScale(value); return true;
}
bool Runtime::setTrackAlpha(TrackHandle handle, float value) {
    spine::TrackEntry* entry = _impl ? _impl->resolveTrack(handle) : nullptr; if (!entry) return false; entry->setAlpha(value); return true;
}
bool Runtime::setTrackAnimationRange(TrackHandle handle, float start, float end, float last) {
    spine::TrackEntry* entry = _impl ? _impl->resolveTrack(handle) : nullptr;
    if (!entry || start < 0.0F || end < start) return false;
    entry->setAnimationStart(start); entry->setAnimationEnd(end); entry->setAnimationLast(last); return true;
}
bool Runtime::setTrackMixTime(TrackHandle handle, float value) {
    spine::TrackEntry* entry = _impl ? _impl->resolveTrack(handle) : nullptr; if (!entry) return false; entry->setMixTime(value); return true;
}
bool Runtime::setTrackMixDuration(TrackHandle handle, float value) {
    spine::TrackEntry* entry = _impl ? _impl->resolveTrack(handle) : nullptr; if (!entry) return false; entry->setMixDuration(value); return true;
}
bool Runtime::setTrackMixDuration(TrackHandle handle, float duration, float delay) {
    spine::TrackEntry* entry = _impl ? _impl->resolveTrack(handle) : nullptr; if (!entry) return false; entry->setMixDuration(duration, delay); return true;
}
bool Runtime::setTrackThresholds(TrackHandle handle, float eventThreshold,
                                 float attachmentThreshold, float drawOrderThreshold) {
    spine::TrackEntry* entry = _impl ? _impl->resolveTrack(handle) : nullptr;
    if (!entry) return false;
    entry->setEventThreshold(eventThreshold);
    entry->setMixAttachmentThreshold(attachmentThreshold);
    entry->setMixDrawOrderThreshold(drawOrderThreshold);
    return true;
}
bool Runtime::setTrackAlphaAttachmentThreshold(TrackHandle handle, float value) {
    spine::TrackEntry* entry = _impl ? _impl->resolveTrack(handle) : nullptr;
    if (!entry) return false;
    entry->setAlphaAttachmentThreshold(value);
    return true;
}
bool Runtime::resetTrackRotationDirections(TrackHandle handle) {
    spine::TrackEntry* entry = _impl ? _impl->resolveTrack(handle) : nullptr;
    if (!entry) return false;
    entry->resetRotationDirections();
    return true;
}

bool Runtime::setSkin(const char* name) {
    if (!_impl || !_impl->skeleton) return false;
    if (!name || name[0] == '\0') {
        _impl->skeleton->setSkin(static_cast<spine::Skin*>(nullptr));
    } else {
        spine::Skin* skin = _impl->skeleton->getData().findSkin(spine::String(name));
        if (!skin) return false;
        _impl->skeleton->setSkin(skin);
    }
    _impl->skeleton->setupPoseSlots();
    return true;
}

void Runtime::setToSetupPose() { if (_impl && _impl->skeleton) _impl->skeleton->setupPose(); }
void Runtime::setBonesToSetupPose() { if (_impl && _impl->skeleton) _impl->skeleton->setupPoseBones(); }
void Runtime::setSlotsToSetupPose() { if (_impl && _impl->skeleton) _impl->skeleton->setupPoseSlots(); }
void Runtime::updateWorldTransform() {
    updateWorldTransform(PHYSICS_UPDATE);
}
void Runtime::updateWorldTransform(PhysicsMode physicsMode) {
    if (!_impl || !_impl->skeleton) return;
    spine::Physics physics = spine::Physics_Update;
    switch (physicsMode) {
        case PHYSICS_NONE: physics = spine::Physics_None; break;
        case PHYSICS_RESET: physics = spine::Physics_Reset; break;
        case PHYSICS_UPDATE: physics = spine::Physics_Update; break;
        case PHYSICS_POSE: physics = spine::Physics_Pose; break;
        default: return;
    }
    _impl->skeleton->updateWorldTransform(physics);
}
void Runtime::resetPhysics() { updateWorldTransform(PHYSICS_RESET); }

void Runtime::setSkeletonTransform(float x, float y, float scaleX, float scaleY) {
    if (!_impl || !_impl->skeleton) return;
    _impl->skeleton->setPosition(x, y);
    _impl->skeleton->setScale(scaleX, scaleY);
}

void Runtime::setPhysics(float windX, float windY, float gravityX, float gravityY) {
    if (!_impl || !_impl->skeleton) return;
    _impl->skeleton->setWindX(windX);
    _impl->skeleton->setWindY(windY);
    _impl->skeleton->setGravityX(gravityX);
    _impl->skeleton->setGravityY(gravityY);
}

void Runtime::physicsTranslate(float x, float y) {
    if (_impl && _impl->skeleton) _impl->skeleton->physicsTranslate(x, y);
}
void Runtime::physicsRotate(float x, float y, float degrees) {
    if (_impl && _impl->skeleton) _impl->skeleton->physicsRotate(x, y, degrees);
}
float Runtime::skeletonTime() const { return _impl && _impl->skeleton ? _impl->skeleton->getTime() : 0.0F; }

int Runtime::boneCount() const {
    return _impl && _impl->skeleton ? static_cast<int>(_impl->skeleton->getBones().size()) : 0;
}

int Runtime::findBoneIndex(const char* name) const {
    if (!_impl || !_impl->skeleton || !name) return -1;
    spine::Bone* bone = _impl->skeleton->findBone(spine::String(name));
    return bone ? bone->getData().getIndex() : -1;
}

const char* Runtime::boneName(int index) const {
    if (!_impl || !_impl->skeleton || index < 0 || index >= boneCount()) return nullptr;
    return _impl->skeleton->getBones()[static_cast<size_t>(index)]->getData().getName().buffer();
}

bool Runtime::getBone(int index, BoneInfo* out) const {
    if (!_impl || !_impl->skeleton || !out || index < 0 || index >= boneCount()) return false;
    spine::Bone* bone = _impl->skeleton->getBones()[static_cast<size_t>(index)];
    spine::BoneLocal& local = bone->getPose();
    spine::BonePose& world = bone->getAppliedPose();
    out->index = bone->getData().getIndex();
    spine::BoneData* parent = bone->getData().getParent();
    out->parentIndex = parent ? parent->getIndex() : -1;
    out->name = bone->getData().getName().buffer();
    out->active = bone->isActive();
    out->length = bone->getData().getLength();
    out->x = local.getX(); out->y = local.getY(); out->rotation = local.getRotation();
    out->scaleX = local.getScaleX(); out->scaleY = local.getScaleY();
    out->shearX = local.getShearX(); out->shearY = local.getShearY();
    out->a = world.getA(); out->b = world.getB(); out->c = world.getC(); out->d = world.getD();
    out->worldX = world.getWorldX(); out->worldY = world.getWorldY();
    return true;
}

bool Runtime::getBone(const char* name, BoneInfo* out) const {
    const int index = findBoneIndex(name);
    return index >= 0 && getBone(index, out);
}

bool Runtime::setBoneLocal(const char* name, float x, float y, float rotation,
                           float scaleX, float scaleY, float shearX, float shearY) {
    if (!_impl || !_impl->skeleton || !name) return false;
    spine::Bone* bone = _impl->skeleton->findBone(spine::String(name));
    if (!bone) return false;
    spine::BonePose& local = bone->getPose();
    local.setPosition(x, y);
    local.setRotation(rotation);
    local.setScale(scaleX, scaleY);
    local.setShearX(shearX);
    local.setShearY(shearY);
    local.modifyLocal(*_impl->skeleton);
    return true;
}

bool Runtime::boneWorldToLocal(const char* name, float worldX, float worldY, float* out2) const {
    if (!_impl || !_impl->skeleton || !name || !out2) return false;
    spine::Bone* bone = _impl->skeleton->findBone(spine::String(name));
    if (!bone) return false;
    bone->getAppliedPose().worldToLocal(worldX, worldY, out2[0], out2[1]);
    return true;
}

bool Runtime::boneLocalToWorld(const char* name, float localX, float localY, float* out2) const {
    if (!_impl || !_impl->skeleton || !name || !out2) return false;
    spine::Bone* bone = _impl->skeleton->findBone(spine::String(name));
    if (!bone) return false;
    bone->getAppliedPose().localToWorld(localX, localY, out2[0], out2[1]);
    return true;
}

bool Runtime::findBone(const char* name, float* out6) const {
    const int index = findBoneIndex(name);
    return index >= 0 && boneTransform(index, out6);
}

bool Runtime::boneTransform(int index, float* out6) const {
    if (!_impl || !_impl->skeleton || !out6 || index < 0 || index >= boneCount()) return false;
    spine::BonePose& pose = _impl->skeleton->getBones()[static_cast<size_t>(index)]->getAppliedPose();
    out6[0] = pose.getA(); out6[1] = pose.getB(); out6[2] = pose.getC();
    out6[3] = pose.getD(); out6[4] = pose.getWorldX(); out6[5] = pose.getWorldY();
    return true;
}

int Runtime::slotCount() const {
    return _impl && _impl->skeleton ? static_cast<int>(_impl->skeleton->getSlots().size()) : 0;
}

int Runtime::findSlotIndex(const char* name) const {
    if (!_impl || !_impl->skeleton || !name) return -1;
    spine::Slot* slot = _impl->skeleton->findSlot(spine::String(name));
    return slot ? slot->getData().getIndex() : -1;
}

const char* Runtime::slotName(int index) const {
    if (!_impl || !_impl->skeleton || index < 0 || index >= slotCount()) return nullptr;
    return _impl->skeleton->getSlots()[static_cast<size_t>(index)]->getData().getName().buffer();
}

bool Runtime::getSlot(int index, SlotInfo* out) const {
    if (!_impl || !_impl->skeleton || !out || index < 0 || index >= slotCount()) return false;
    spine::Slot* slot = _impl->skeleton->getSlots()[static_cast<size_t>(index)];
    spine::SlotPose& pose = slot->getAppliedPose();
    spine::Attachment* attachment = pose.getAttachment();
    auto overrideIter = _impl->slotOverrides.find(slot);
    if (overrideIter != _impl->slotOverrides.end() && overrideIter->second.attachment) {
        attachment = overrideIter->second.attachment;
    }
    out->index = slot->getData().getIndex();
    out->boneIndex = slot->getData().getBoneData().getIndex();
    out->name = slot->getData().getName().buffer();
    out->attachmentName = attachment ? attachment->getName().buffer() : nullptr;
    out->blendMode = static_cast<uint8_t>(slot->getData().getBlendMode());
    const spine::Color& color = pose.getColor();
    out->colorR = color.r; out->colorG = color.g; out->colorB = color.b; out->colorA = color.a;
    out->hasDarkColor = pose.hasDarkColor();
    if (out->hasDarkColor) {
        const spine::Color& dark = pose.getDarkColor();
        out->darkR = dark.r; out->darkG = dark.g; out->darkB = dark.b; out->darkA = dark.a;
    }
    return true;
}

bool Runtime::getSlot(const char* name, SlotInfo* out) const {
    const int index = findSlotIndex(name);
    return index >= 0 && getSlot(index, out);
}

bool Runtime::setSlotColor(const char* name, float r, float g, float b, float a) {
    if (!_impl || !_impl->skeleton || !name) return false;
    spine::Slot* slot = _impl->skeleton->findSlot(spine::String(name));
    if (!slot) return false;
    slot->getPose().getColor().set(r, g, b, a);
    if (&slot->getAppliedPose() != &slot->getPose()) slot->getAppliedPose().getColor().set(r, g, b, a);
    return true;
}

bool Runtime::setAttachment(const char* slotNameValue, const char* attachmentNameValue) {
    if (!_impl || !_impl->skeleton || !slotNameValue) return false;
    spine::Slot* slot = _impl->skeleton->findSlot(spine::String(slotNameValue));
    if (!slot) return false;
    const spine::String attachmentNameString(attachmentNameValue ? attachmentNameValue : "");
    if (!attachmentNameString.isEmpty() &&
        !_impl->skeleton->getAttachment(slot->getData().getIndex(), attachmentNameString)) return false;
    _impl->clearSlotOverride(slot);
    _impl->skeleton->setAttachment(spine::String(slotNameValue), attachmentNameString);
    return true;
}

bool Runtime::getAttachment(const char* slotNameValue, const char* attachmentNameValue,
                            AttachmentInfo* out) const {
    if (!_impl || !_impl->skeleton || !slotNameValue || !attachmentNameValue || !out) return false;
    spine::Slot* slot = _impl->skeleton->findSlot(spine::String(slotNameValue));
    if (!slot) return false;
    spine::Attachment* attachment = _impl->skeleton->getAttachment(
        slot->getData().getIndex(), spine::String(attachmentNameValue));
    return _impl->writeAttachmentInfo(*slot, attachment, *out, false);
}

bool Runtime::getCurrentAttachment(const char* slotNameValue, AttachmentInfo* out) const {
    if (!_impl || !_impl->skeleton || !slotNameValue || !out) return false;
    spine::Slot* slot = _impl->skeleton->findSlot(spine::String(slotNameValue));
    if (!slot) return false;
    spine::Attachment* attachment = slot->getAppliedPose().getAttachment();
    auto overrideIter = _impl->slotOverrides.find(slot);
    if (overrideIter != _impl->slotOverrides.end() && overrideIter->second.attachment) {
        attachment = overrideIter->second.attachment;
    }
    return _impl->writeAttachmentInfo(*slot, attachment, *out, true);
}

void Runtime::setSlotsRange(int start, int end) {
    if (!_impl) return;
    _impl->startSlotIndex = start;
    _impl->endSlotIndex = end;
}

bool Runtime::resizeSlotRegion(const char* slotNameValue, float width, float height, bool createNewAttachment) {
    if (!_impl || !_impl->skeleton || !slotNameValue ||
        !(width > 0.0F) || !(height > 0.0F) ||
        !std::isfinite(width) || !std::isfinite(height) ||
        width > static_cast<float>(std::numeric_limits<int>::max()) ||
        height > static_cast<float>(std::numeric_limits<int>::max())) return false;
    spine::Slot* slot = _impl->skeleton->findSlot(spine::String(slotNameValue));
    if (!slot) return false;

    bool hadTexture = false;
    uint32_t textureId = 0;
    auto previous = _impl->slotOverrides.find(slot);
    if (previous != _impl->slotOverrides.end()) {
        hadTexture = previous->second.hasTexture;
        textureId = previous->second.textureId;
        _impl->clearSlotOverride(slot);
    }

    spine::Attachment* source = slot->getAppliedPose().getAttachment();
    if (!source) return false;
    spine::Attachment* target = source;
    if (createNewAttachment) target = &source->copy();

    Impl::SlotOverride value;
    value.attachment = target;
    value.originalAttachment = source;
    value.ownsAttachment = createNewAttachment;
    value.hasTexture = hadTexture;
    value.textureId = textureId;

    auto installFullRegions = [&](spine::Sequence& sequence, auto& attachment) {
        auto& regions = sequence.getRegions();
        for (size_t i = 0; i < regions.size(); ++i) {
            spine::TextureRegion* oldRegion = regions[i];
            spine::TextureRegion* region = new spine::TextureRegion();
            region->setU(0.0F); region->setV(0.0F); region->setU2(1.0F); region->setV2(1.0F);
            region->setRegionWidth(static_cast<int>(width));
            region->setRegionHeight(static_cast<int>(height));
            region->setRendererObject(oldRegion ? oldRegion->getRendererObject() : nullptr);
            regions[i] = region;
            if (createNewAttachment) value.ownedRegions.push_back(region);
            else _impl->data->_impl->runtimeTextureRegions.push_back(region);
        }
        sequence.update(attachment);
    };

    if (target->getRTTI().isExactly(spine::RegionAttachment::rtti)) {
        auto* region = static_cast<spine::RegionAttachment*>(target);
        region->setWidth(width);
        region->setHeight(height);
        installFullRegions(region->getSequence(), *region);
    } else if (target->getRTTI().isExactly(spine::MeshAttachment::rtti)) {
        auto* mesh = static_cast<spine::MeshAttachment*>(target);
        mesh->setWidth(width);
        mesh->setHeight(height);
        installFullRegions(mesh->getSequence(), *mesh);
    } else {
        if (createNewAttachment) delete target;
        return false;
    }

    if (createNewAttachment) slot->getPose().setAttachment(target);
    _impl->slotOverrides.emplace(slot, std::move(value));
    _impl->skeleton->updateCache();
    return true;
}

bool Runtime::setSlotTexture(const char* slotNameValue, uint32_t textureId) {
    if (!_impl || !_impl->skeleton || !slotNameValue) return false;
    spine::Slot* slot = _impl->skeleton->findSlot(spine::String(slotNameValue));
    if (!slot) return false;
    Impl::SlotOverride& value = _impl->slotOverrides[slot];
    value.hasTexture = true;
    value.textureId = textureId;
    return true;
}

bool Runtime::clearSlotTexture(const char* slotNameValue) {
    if (!_impl || !_impl->skeleton || !slotNameValue) return false;
    spine::Slot* slot = _impl->skeleton->findSlot(spine::String(slotNameValue));
    if (!slot) return false;
    _impl->clearSlotOverride(slot);
    return true;
}

size_t Runtime::computeSlotWorldVertices(const char* slotNameValue, float* out,
                                         size_t outFloatCount, size_t offset,
                                         size_t stride) const {
    if (!_impl || !_impl->skeleton || !slotNameValue || stride < 2) return 0;
    spine::Slot* slot = _impl->skeleton->findSlot(spine::String(slotNameValue));
    if (!slot) return 0;
    spine::Attachment* attachment = slot->getAppliedPose().getAttachment();
    auto overrideIter = _impl->slotOverrides.find(slot);
    if (overrideIter != _impl->slotOverrides.end() && overrideIter->second.attachment) {
        attachment = overrideIter->second.attachment;
    }
    if (!attachment) return 0;

    size_t positionFloats = 0;
    if (attachment->getRTTI().isExactly(spine::RegionAttachment::rtti)) positionFloats = 8;
    else if (attachment->getRTTI().instanceOf(spine::VertexAttachment::rtti)) {
        positionFloats = static_cast<spine::VertexAttachment*>(attachment)->getWorldVerticesLength();
    } else return 0;

    const size_t vertices = positionFloats / 2;
    const size_t required = vertices == 0 ? offset : offset + (vertices - 1) * stride + 2;
    if (!out || outFloatCount < required) return required;

    if (attachment->getRTTI().isExactly(spine::RegionAttachment::rtti)) {
        auto* region = static_cast<spine::RegionAttachment*>(attachment);
        spine::Sequence& sequence = region->getSequence();
        const int sequenceIndex = sequence.resolveIndex(slot->getAppliedPose());
        region->computeWorldVertices(*slot, sequence.getOffsets(sequenceIndex).buffer(), out, offset, stride);
    } else {
        auto* vertexAttachment = static_cast<spine::VertexAttachment*>(attachment);
        vertexAttachment->computeWorldVertices(*_impl->skeleton, *slot, 0, positionFloats, out, offset, stride);
    }
    return required;
}

bool Runtime::getBounds(float* out4) const {
    if (!_impl || !_impl->skeleton || !out4) return false;
    _impl->skeleton->getBounds(out4[0], out4[1], out4[2], out4[3]);
    return std::isfinite(out4[0]) && std::isfinite(out4[1]) &&
           std::isfinite(out4[2]) && std::isfinite(out4[3]) &&
           out4[2] >= 0.0F && out4[3] >= 0.0F;
}

// ---------------------------------------------------------------------------
// Render extraction
// ---------------------------------------------------------------------------
void Runtime::collectRenderData(Runtime::Impl& impl) {
    impl.vertexBuffer.clear();
    impl.indexBuffer.clear();
    impl.segments.clear();
    impl.shapes.clear();
    impl.renderData = RenderData{};

    const bool useTint = impl.useTint;
    const uint32_t strideFloats = useTint ? 7U : 6U;
    const uint32_t strideBytes = useTint ? 28U : 24U;
    impl.renderData.vertexStrideBytes = strideBytes;

    spine::Skeleton* skeleton = impl.skeleton;
    if (!skeleton) return;
    const spine::Color& skeletonColor = skeleton->getColor();
    const bool premultiplied = impl.premultipliedAlpha;
    bool inRange = !(impl.startSlotIndex != -1 || impl.endSlotIndex != -1);

    auto& drawOrder = skeleton->getDrawOrder().getAppliedPose();
    for (size_t i = 0; i < drawOrder.size(); ++i) {
        spine::Slot* slot = drawOrder[i];
        spine::SlotPose& applied = slot->getAppliedPose();
        spine::Attachment* attachment = applied.getAttachment();
        auto overrideIter = impl.slotOverrides.find(slot);
        if (overrideIter != impl.slotOverrides.end() && overrideIter->second.attachment) {
            attachment = overrideIter->second.attachment;
        }

        const bool isClip = attachment && attachment->getRTTI().isExactly(spine::ClippingAttachment::rtti);
        if (!slot->getBone().isActive() || (applied.getColor().a <= 0.0F && !isClip)) {
            impl.clipper->clipEnd(*slot);
            continue;
        }
        if (impl.startSlotIndex >= 0 && impl.startSlotIndex == slot->getData().getIndex()) inRange = true;
        if (!inRange) {
            impl.clipper->clipEnd(*slot);
            continue;
        }
        if (impl.endSlotIndex >= 0 && impl.endSlotIndex == slot->getData().getIndex()) inRange = false;
        if (!inRange) {
            impl.clipper->clipEnd(*slot);
            continue;
        }
        if (!attachment) {
            impl.clipper->clipEnd(*slot);
            continue;
        }
        if (isClip) {
            impl.clipper->clipStart(*skeleton, *slot, static_cast<spine::ClippingAttachment*>(attachment));
            continue;
        }

        const spine::RTTI& rtti = attachment->getRTTI();
        const uint8_t blend = static_cast<uint8_t>(slot->getData().getBlendMode());
        spine::Color attachmentColor(1.0F, 1.0F, 1.0F, 1.0F);
        uint32_t textureId = 0;
        uint8_t shapeType = RenderData::SHAPE_REGION;
        spine::RegionAttachment* regionAttachment = nullptr;
        spine::MeshAttachment* meshAttachment = nullptr;
        float* sourceUVs = nullptr;
        uint16_t* sourceIndices = nullptr;
        size_t sourceIndexCount = 0;
        uint32_t sourceVertexCount = 0;
        static uint16_t quadIndices[6] = {0, 1, 2, 2, 3, 0};

        if (rtti.isExactly(spine::RegionAttachment::rtti)) {
            regionAttachment = static_cast<spine::RegionAttachment*>(attachment);
            attachmentColor = regionAttachment->getColor();
            if (attachmentColor.a <= 0.0F) {
                impl.clipper->clipEnd(*slot);
                continue;
            }
            spine::Sequence& sequence = regionAttachment->getSequence();
            const int sequenceIndex = sequence.resolveIndex(applied);
            spine::TextureRegion* textureRegion = sequence.getRegion(sequenceIndex);
            if (!textureRegion) {
                impl.clipper->clipEnd(*slot);
                continue;
            }
            textureId = textureIdOfRegion(textureRegion);
            sourceUVs = sequence.getUVs(sequenceIndex).buffer();
            sourceIndices = quadIndices;
            sourceIndexCount = 6;
            sourceVertexCount = 4;
        } else if (rtti.isExactly(spine::MeshAttachment::rtti)) {
            shapeType = RenderData::SHAPE_MESH;
            meshAttachment = static_cast<spine::MeshAttachment*>(attachment);
            attachmentColor = meshAttachment->getColor();
            if (attachmentColor.a <= 0.0F) {
                impl.clipper->clipEnd(*slot);
                continue;
            }
            spine::Sequence& sequence = meshAttachment->getSequence();
            const int sequenceIndex = sequence.resolveIndex(applied);
            spine::TextureRegion* textureRegion = sequence.getRegion(sequenceIndex);
            if (!textureRegion) {
                impl.clipper->clipEnd(*slot);
                continue;
            }
            textureId = textureIdOfRegion(textureRegion);
            sourceUVs = sequence.getUVs(sequenceIndex).buffer(); // atlas/sequence-adjusted UVs
            auto& triangles = meshAttachment->getTriangles();
            sourceIndices = triangles.buffer();
            sourceIndexCount = triangles.size();
            sourceVertexCount = static_cast<uint32_t>(meshAttachment->getWorldVerticesLength() / 2);
        } else {
            impl.clipper->clipEnd(*slot);
            continue;
        }

        if (overrideIter != impl.slotOverrides.end() && overrideIter->second.hasTexture) {
            textureId = overrideIter->second.textureId;
        }

        const float alpha = skeletonColor.a * applied.getColor().a * attachmentColor.a;
        const float multiplier = premultiplied ? alpha : 1.0F;
        const uint32_t light = packColor(skeletonColor.r * applied.getColor().r * attachmentColor.r * multiplier,
                                         skeletonColor.g * applied.getColor().g * attachmentColor.g * multiplier,
                                         skeletonColor.b * applied.getColor().b * attachmentColor.b * multiplier,
                                         alpha);
        uint32_t dark = 0;
        if (useTint) {
            float r = 0.0F, g = 0.0F, b = 0.0F;
            if (applied.hasDarkColor()) {
                const spine::Color& slotDark = applied.getDarkColor();
                r = skeletonColor.r * slotDark.r * attachmentColor.r * multiplier;
                g = skeletonColor.g * slotDark.g * attachmentColor.g * multiplier;
                b = skeletonColor.b * slotDark.b * attachmentColor.b * multiplier;
            }
            dark = packColor(r, g, b, premultiplied ? 1.0F : 0.0F);
        }

        const uint32_t indexStart = static_cast<uint32_t>(impl.indexBuffer.size());
        const uint32_t vertexStart = static_cast<uint32_t>(impl.vertexBuffer.size() / strideFloats);
        uint32_t appendedVertices = 0;

        if (impl.clipper->isClipping()) {
            impl.scratchWorldVertices.resize(sourceVertexCount * 2U);
            if (regionAttachment) {
                spine::Sequence& sequence = regionAttachment->getSequence();
                const int sequenceIndex = sequence.resolveIndex(applied);
                regionAttachment->computeWorldVertices(
                    *slot, sequence.getOffsets(sequenceIndex).buffer(),
                    impl.scratchWorldVertices.data(), 0, 2);
            } else {
                meshAttachment->computeWorldVertices(
                    *skeleton, *slot, 0, meshAttachment->getWorldVerticesLength(),
                    impl.scratchWorldVertices.data(), 0, 2);
            }
            impl.clipper->clipTriangles(impl.scratchWorldVertices.data(), sourceIndices,
                                        sourceIndexCount, sourceUVs, 2);
            auto& clippedVertices = impl.clipper->getClippedVertices();
            auto& clippedUVs = impl.clipper->getClippedUVs();
            auto& clippedTriangles = impl.clipper->getClippedTriangles();
            if (clippedTriangles.size() == 0) {
                impl.clipper->clipEnd(*slot);
                continue;
            }
            appendedVertices = static_cast<uint32_t>(clippedVertices.size() / 2);
            if (static_cast<uint64_t>(vertexStart) + appendedVertices > 65536ULL) {
                impl.renderData.indexOverflow = true;
                impl.clipper->clipEnd(*slot);
                break;
            }
            impl.vertexBuffer.resize(impl.vertexBuffer.size() + appendedVertices * strideFloats);
            float* destination = impl.vertexBuffer.data() + vertexStart * strideFloats;
            for (uint32_t v = 0; v < appendedVertices; ++v) {
                destination[v * strideFloats] = clippedVertices[v * 2];
                destination[v * strideFloats + 1] = clippedVertices[v * 2 + 1];
                destination[v * strideFloats + 2] = 0.0F;
                destination[v * strideFloats + 3] = clippedUVs[v * 2];
                destination[v * strideFloats + 4] = clippedUVs[v * 2 + 1];
                uint8_t* bytes = reinterpret_cast<uint8_t*>(destination) + v * strideBytes;
                std::memcpy(bytes + 20, &light, sizeof(light));
                if (useTint) std::memcpy(bytes + 24, &dark, sizeof(dark));
            }
            for (size_t t = 0; t < clippedTriangles.size(); ++t) {
                impl.indexBuffer.push_back(static_cast<uint16_t>(clippedTriangles[t] + vertexStart));
            }
        } else {
            appendedVertices = sourceVertexCount;
            if (static_cast<uint64_t>(vertexStart) + appendedVertices > 65536ULL) {
                impl.renderData.indexOverflow = true;
                impl.clipper->clipEnd(*slot);
                break;
            }
            impl.vertexBuffer.resize(impl.vertexBuffer.size() + appendedVertices * strideFloats);
            float* destination = impl.vertexBuffer.data() + vertexStart * strideFloats;
            if (regionAttachment) {
                spine::Sequence& sequence = regionAttachment->getSequence();
                const int sequenceIndex = sequence.resolveIndex(applied);
                regionAttachment->computeWorldVertices(
                    *slot, sequence.getOffsets(sequenceIndex).buffer(), destination, 0, strideFloats);
            } else {
                meshAttachment->computeWorldVertices(
                    *skeleton, *slot, 0, meshAttachment->getWorldVerticesLength(),
                    destination, 0, strideFloats);
            }
            for (uint32_t v = 0; v < sourceVertexCount; ++v) {
                destination[v * strideFloats + 2] = 0.0F;
                destination[v * strideFloats + 3] = sourceUVs[v * 2];
                destination[v * strideFloats + 4] = sourceUVs[v * 2 + 1];
                uint8_t* bytes = reinterpret_cast<uint8_t*>(destination) + v * strideBytes;
                std::memcpy(bytes + 20, &light, sizeof(light));
                if (useTint) std::memcpy(bytes + 24, &dark, sizeof(dark));
            }
            for (size_t i = 0; i < sourceIndexCount; ++i) {
                impl.indexBuffer.push_back(static_cast<uint16_t>(sourceIndices[i] + vertexStart));
            }
        }

        const uint32_t appendedIndices = static_cast<uint32_t>(impl.indexBuffer.size()) - indexStart;
        if (appendedIndices > 0) {
            if (!impl.segments.empty() && impl.segments.back().blendMode == blend &&
                impl.segments.back().textureId == textureId) {
                impl.segments.back().indexCount += appendedIndices;
            } else {
                RenderData::Segment segment;
                segment.indexOffset = indexStart;
                segment.indexCount = appendedIndices;
                segment.blendMode = blend;
                segment.textureId = textureId;
                impl.segments.push_back(segment);
            }
            RenderData::Shape shape;
            shape.vertexOffset = vertexStart;
            shape.vertexCount = appendedVertices;
            shape.indexOffset = indexStart;
            shape.indexCount = appendedIndices;
            shape.slotIndex = slot->getData().getIndex();
            shape.type = shapeType;
            impl.shapes.push_back(shape);
        }
        impl.clipper->clipEnd(*slot);
    }
    // Required for clipping attachments whose end slot is null (end of draw order).
    impl.clipper->clipEnd();

    // Bake the output-space affine (node/world transform + y-flip) into the
    // vertex positions so no per-node transform uniform is needed for batching.
    if (impl.outA != 1.0F || impl.outB != 0.0F || impl.outC != 0.0F ||
        impl.outD != 1.0F || impl.outTx != 0.0F || impl.outTy != 0.0F) {
        float* p = impl.vertexBuffer.data();
        const size_t vertexCount = impl.vertexBuffer.size() / strideFloats;
        const float a = impl.outA, b = impl.outB, c = impl.outC, d = impl.outD;
        const float tx = impl.outTx, ty = impl.outTy;
        for (size_t v = 0; v < vertexCount; ++v) {
            const float x = p[v * strideFloats];
            const float y = p[v * strideFloats + 1];
            p[v * strideFloats] = a * x + b * y + tx;
            p[v * strideFloats + 1] = c * x + d * y + ty;
            // z stays 0 (spine writes 0; 2D renderers ignore vertex z).
        }
    }

    impl.renderData.vertexCount = static_cast<uint32_t>(impl.vertexBuffer.size() / strideFloats);
    impl.renderData.indexCount = static_cast<uint32_t>(impl.indexBuffer.size());
    impl.renderData.vertices = impl.vertexBuffer.empty() ? nullptr : impl.vertexBuffer.data();
    impl.renderData.indices = impl.indexBuffer.empty() ? nullptr : impl.indexBuffer.data();
    impl.renderData.segments = impl.segments.empty() ? nullptr : impl.segments.data();
    impl.renderData.segmentCount = static_cast<uint32_t>(impl.segments.size());
    impl.renderData.shapes = impl.shapes.empty() ? nullptr : impl.shapes.data();
    impl.renderData.shapeCount = static_cast<uint32_t>(impl.shapes.size());
}

void Runtime::updateAnimation(float dt) {
    if (!_impl || !_impl->state || !_impl->skeleton) return;
    if (!_impl->paused) {
        _impl->skeleton->update(dt * _impl->timeScale);
        _impl->state->update(dt);
        _impl->state->apply(*_impl->skeleton);
    }
    _impl->dispatchEvents();
}

void Runtime::updatePose(float dt) {
    updateAnimation(dt);
    updateWorldTransform(PHYSICS_UPDATE);
}

void Runtime::extractRenderData() {
    if (!_impl || !_impl->skeleton) return;
    collectRenderData(*_impl);
}

void Runtime::updateRenderData() {
    if (!_impl || !_impl->skeleton) return;
    updateWorldTransform(PHYSICS_UPDATE);
    extractRenderData();
}

void Runtime::update(float dt) {
    updatePose(dt);
    extractRenderData();
}

void Runtime::setOutputTransform(float a, float b, float c, float d, float tx, float ty) {
    if (!_impl) return;
    _impl->outA = a;
    _impl->outB = b;
    _impl->outC = c;
    _impl->outD = d;
    _impl->outTx = tx;
    _impl->outTy = ty;
}

const RenderData& Runtime::renderData() const {
    static const RenderData empty;
    return _impl ? _impl->renderData : empty;
}

void Runtime::setListener(Listener callback, void* user) {
    if (!_impl) return;
    _impl->legacyListener = callback;
    _impl->legacyUser = user;
}

void Runtime::setEventListener(EventListener callback, void* user) {
    if (!_impl) return;
    _impl->eventListener = callback;
    _impl->eventUser = user;
}

} // namespace spineruntime

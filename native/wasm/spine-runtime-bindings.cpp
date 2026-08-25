// SpineRuntime embind binding (wasm -> JS)
// Full: Data metadata + Runtime control/tracks/bones/slots/physics/events.
//
// Design: Data/Runtime are exposed as "opaque handles (uint32 pointer value) +
// free functions" rather than embind class_ -- the runtime is PIMPL (private
// destructor + reference counting), and embind class binding needs a callable
// destructor, which fails to compile. Handles are opaque on the JS side and
// only used through the functions here; disposeXxx() releases explicitly.
//
// Render data: renderData's vPtr/iPtr are wasm HEAP offsets; TS reads
// vertices/indices directly via Module.HEAPU8.subarray(offset, offset + len).
// Events: JS callbacks are kept in a global map (keyed by handle); a C
// callback bridges back to JS.
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "SpineRuntime.h"

using namespace emscripten;
using namespace spineruntime;

using Handle = uint32_t;

namespace {
std::unordered_map<Handle, val> gEventListener;
std::unordered_map<Handle, val> gListener;

void eventBridge(void* user, const Runtime::EventInfo* event) {
    const Handle handle = reinterpret_cast<Handle>(user);
    auto it = gEventListener.find(handle);
    if (it == gEventListener.end() || !event) return;
    val obj = val::object();
    obj.set("type", event->type);
    obj.set("track", static_cast<uint32_t>(event->track));
    obj.set("trackIndex", event->trackIndex);
    obj.set("animationName", event->animationName ? event->animationName : "");
    obj.set("trackTime", event->trackTime);
    obj.set("animationEnd", event->animationEnd);
    obj.set("eventName", event->eventName ? event->eventName : "");
    obj.set("eventTime", event->eventTime);
    obj.set("intValue", event->intValue);
    obj.set("floatValue", event->floatValue);
    obj.set("stringValue", event->stringValue ? event->stringValue : "");
    obj.set("audioPath", event->audioPath ? event->audioPath : "");
    obj.set("volume", event->volume);
    obj.set("balance", event->balance);
    it->second(obj);
}

void listenerBridge(void* user, int eventType, const char* animationName, int trackIndex) {
    const Handle handle = reinterpret_cast<Handle>(user);
    auto it = gListener.find(handle);
    if (it == gListener.end()) return;
    it->second(eventType, std::string(animationName ? animationName : ""), trackIndex);
}
}  // namespace

// Returns the last error recorded by the C++ runtime (Data::lastError).
static std::string getLastError() {
    const char* err = spineruntime::Data::lastError();
    return err ? std::string(err) : std::string();
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------
static std::vector<std::string> texNamesFromVal(const val& texNames, int* outCount) {
    std::vector<std::string> names;
    const int n = texNames["length"].as<int>();
    names.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        names.emplace_back(texNames[i].as<std::string>());
    }
    *outCount = n;
    return names;
}

static Handle createDataJson(const std::string& json, const std::string& atlas,
                             const val& texNames, float scale) {
    int n = 0;
    std::vector<std::string> names = texNamesFromVal(texNames, &n);
    std::vector<const char*> namePtrs;
    for (const auto& s : names) namePtrs.push_back(s.c_str());
    Data* d = Data::create(json.c_str(), json.size(), false,
                           atlas.c_str(), namePtrs.data(), n, scale);
    return reinterpret_cast<Handle>(d);
}

// Binary (.skel) counterpart of createDataJson. `bytes` is a JS Uint8Array;
// vecFromJSArray copies it into a std::vector so the pointer stays valid for
// the duration of the Data::create() call (a one-time cost paid at import/load
// time, not per frame).
static Handle createDataBinary(const val& bytes, const std::string& atlas,
                               const val& texNames, float scale) {
    std::vector<uint8_t> buf = vecFromJSArray<uint8_t>(bytes);
    int n = 0;
    std::vector<std::string> names = texNamesFromVal(texNames, &n);
    std::vector<const char*> namePtrs;
    for (const auto& s : names) namePtrs.push_back(s.c_str());
    Data* d = Data::create(buf.data(), buf.size(), true,
                           atlas.c_str(), namePtrs.data(), n, scale);
    return reinterpret_cast<Handle>(d);
}

static void disposeData(Handle h) { reinterpret_cast<Data*>(h)->dispose(); }

static float dataWidth(Handle h) { return reinterpret_cast<Data*>(h)->width(); }
static float dataHeight(Handle h) { return reinterpret_cast<Data*>(h)->height(); }
static float dataX(Handle h) { return reinterpret_cast<Data*>(h)->x(); }
static float dataY(Handle h) { return reinterpret_cast<Data*>(h)->y(); }
static std::string dataVersion(Handle h) {
    const char* v = reinterpret_cast<Data*>(h)->version();
    return v ? std::string(v) : std::string();
}

static int dataAnimationCount(Handle h) { return reinterpret_cast<Data*>(h)->animationCount(); }
static std::string dataAnimationName(Handle h, int index) {
    const char* n = reinterpret_cast<Data*>(h)->animationName(index);
    return n ? std::string(n) : std::string();
}
static bool dataHasAnimation(Handle h, const std::string& name) {
    return reinterpret_cast<Data*>(h)->hasAnimation(name.c_str());
}
static float dataAnimationDuration(Handle h, const std::string& name) {
    return reinterpret_cast<Data*>(h)->animationDuration(name.c_str());
}

static int dataSkinCount(Handle h) { return reinterpret_cast<Data*>(h)->skinCount(); }
static std::string dataSkinName(Handle h, int index) {
    const char* n = reinterpret_cast<Data*>(h)->skinName(index);
    return n ? std::string(n) : std::string();
}
static bool dataHasSkin(Handle h, const std::string& name) {
    return reinterpret_cast<Data*>(h)->hasSkin(name.c_str());
}

static int dataTexturePageCount(Handle h) { return reinterpret_cast<Data*>(h)->texturePageCount(); }
static val dataTexturePage(Handle h, int index) {
    TexturePageInfo info;
    if (!reinterpret_cast<Data*>(h)->texturePage(index, &info)) return val::null();
    val o = val::object();
    o.set("textureId", info.textureId);
    o.set("width", info.width);
    o.set("height", info.height);
    o.set("minFilter", info.minFilter);
    o.set("magFilter", info.magFilter);
    o.set("uWrap", info.uWrap);
    o.set("vWrap", info.vWrap);
    o.set("premultipliedAlpha", info.premultipliedAlpha);
    return o;
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------
static Handle createRuntime(Handle dataHandle) {
    Runtime* r = Runtime::create(reinterpret_cast<Data*>(dataHandle));
    return reinterpret_cast<Handle>(r);
}
static void disposeRuntime(Handle h) { reinterpret_cast<Runtime*>(h)->dispose(); }

// Playback / tracks
static void runtimePlay(Handle h, const std::string& name, bool loop) {
    reinterpret_cast<Runtime*>(h)->play(name.c_str(), loop);
}
static uint32_t runtimeSetAnimation(Handle h, int track, const std::string& name, bool loop) {
    return static_cast<uint32_t>(
        reinterpret_cast<Runtime*>(h)->setAnimation(track, name.c_str(), loop));
}
static uint32_t runtimeAddAnimation(Handle h, int track, const std::string& name, bool loop, float delay) {
    return static_cast<uint32_t>(
        reinterpret_cast<Runtime*>(h)->addAnimation(track, name.c_str(), loop, delay));
}
static void runtimeSetEmptyAnimation(Handle h, int track, float mixDuration) {
    reinterpret_cast<Runtime*>(h)->setEmptyAnimation(track, mixDuration);
}
static void runtimeAddEmptyAnimation(Handle h, int track, float mixDuration, float delay) {
    reinterpret_cast<Runtime*>(h)->addEmptyAnimation(track, mixDuration, delay);
}
static void runtimeSetEmptyAnimations(Handle h, float mixDuration) {
    reinterpret_cast<Runtime*>(h)->setEmptyAnimations(mixDuration);
}
static uint32_t runtimeGetCurrent(Handle h, int track) {
    return static_cast<uint32_t>(reinterpret_cast<Runtime*>(h)->getCurrent(track));
}
static void runtimeClearTrack(Handle h, int track) { reinterpret_cast<Runtime*>(h)->clearTrack(track); }
static void runtimeClearTracks(Handle h) { reinterpret_cast<Runtime*>(h)->clearTracks(); }
static bool runtimeFindAnimation(Handle h, const std::string& name) {
    return reinterpret_cast<Runtime*>(h)->findAnimation(name.c_str());
}
static void runtimeSetMix(Handle h, const std::string& from, const std::string& to, float duration) {
    reinterpret_cast<Runtime*>(h)->setMix(from.c_str(), to.c_str(), duration);
}

// Track info / properties
static val runtimeGetTrackInfo(Handle h, uint32_t th) {
    Runtime::TrackInfo info;
    if (!reinterpret_cast<Runtime*>(h)->getTrackInfo(th, &info)) return val::null();
    val o = val::object();
    o.set("handle", static_cast<uint32_t>(info.handle));
    o.set("trackIndex", info.trackIndex);
    o.set("animationName", info.animationName ? info.animationName : "");
    o.set("loop", info.loop);
    o.set("reverse", info.reverse);
    o.set("additive", info.additive);
    o.set("complete", info.complete);
    o.set("delay", info.delay);
    o.set("trackTime", info.trackTime);
    o.set("trackEnd", info.trackEnd);
    o.set("animationStart", info.animationStart);
    o.set("animationEnd", info.animationEnd);
    o.set("animationLast", info.animationLast);
    o.set("animationTime", info.animationTime);
    o.set("timeScale", info.timeScale);
    o.set("alpha", info.alpha);
    o.set("mixTime", info.mixTime);
    o.set("mixDuration", info.mixDuration);
    return o;
}
static bool runtimeSetTrackLoop(Handle h, uint32_t th, bool loop) {
    return reinterpret_cast<Runtime*>(h)->setTrackLoop(th, loop);
}
static bool runtimeSetTrackReverse(Handle h, uint32_t th, bool reverse) {
    return reinterpret_cast<Runtime*>(h)->setTrackReverse(th, reverse);
}
static bool runtimeSetTrackAdditive(Handle h, uint32_t th, bool additive) {
    return reinterpret_cast<Runtime*>(h)->setTrackAdditive(th, additive);
}
static bool runtimeSetTrackDelay(Handle h, uint32_t th, float delay) {
    return reinterpret_cast<Runtime*>(h)->setTrackDelay(th, delay);
}
static bool runtimeSetTrackTime(Handle h, uint32_t th, float t) {
    return reinterpret_cast<Runtime*>(h)->setTrackTime(th, t);
}
static bool runtimeSetTrackEnd(Handle h, uint32_t th, float end) {
    return reinterpret_cast<Runtime*>(h)->setTrackEnd(th, end);
}
static bool runtimeSetTrackTimeScale(Handle h, uint32_t th, float ts) {
    return reinterpret_cast<Runtime*>(h)->setTrackTimeScale(th, ts);
}
static bool runtimeSetTrackAlpha(Handle h, uint32_t th, float a) {
    return reinterpret_cast<Runtime*>(h)->setTrackAlpha(th, a);
}

// Skin / pose
static bool runtimeSetSkin(Handle h, const std::string& name) {
    return reinterpret_cast<Runtime*>(h)->setSkin(name.c_str());
}
static void runtimeSetToSetupPose(Handle h) { reinterpret_cast<Runtime*>(h)->setToSetupPose(); }
static void runtimeSetBonesToSetupPose(Handle h) { reinterpret_cast<Runtime*>(h)->setBonesToSetupPose(); }
static void runtimeSetSlotsToSetupPose(Handle h) { reinterpret_cast<Runtime*>(h)->setSlotsToSetupPose(); }
// Manual world-transform refresh (culling / external pose changes).
static void runtimeUpdateWorldTransform(Handle h) { reinterpret_cast<Runtime*>(h)->updateWorldTransform(); }

// Params / pause
static void runtimeSetParams(Handle h, float timeScale,
                             float r, float g, float b, float a,
                             bool premultipliedAlpha, bool useTint) {
    Runtime::Params p;
    p.timeScale = timeScale;
    p.colorR = r; p.colorG = g; p.colorB = b; p.colorA = a;
    p.premultipliedAlpha = premultipliedAlpha;
    p.useTint = useTint;
    reinterpret_cast<Runtime*>(h)->setParams(p);
}
static void runtimeSetPaused(Handle h, bool paused) { reinterpret_cast<Runtime*>(h)->setPaused(paused); }

// Bone queries
static int runtimeBoneCount(Handle h) { return reinterpret_cast<Runtime*>(h)->boneCount(); }
static std::string runtimeBoneName(Handle h, int index) {
    const char* name = reinterpret_cast<Runtime*>(h)->boneName(index);
    return name ? std::string(name) : std::string();
}
static int runtimeFindBoneIndex(Handle h, const std::string& name) {
    return reinterpret_cast<Runtime*>(h)->findBoneIndex(name.c_str());
}
static val runtimeGetBone(Handle h, int index) {
    Runtime::BoneInfo info;
    if (!reinterpret_cast<Runtime*>(h)->getBone(index, &info)) return val::null();
    val o = val::object();
    o.set("index", info.index);
    o.set("parentIndex", info.parentIndex);
    o.set("name", info.name ? info.name : "");
    o.set("active", info.active);
    o.set("x", info.x); o.set("y", info.y);
    o.set("rotation", info.rotation);
    o.set("scaleX", info.scaleX); o.set("scaleY", info.scaleY);
    o.set("shearX", info.shearX); o.set("shearY", info.shearY);
    o.set("a", info.a); o.set("b", info.b); o.set("c", info.c); o.set("d", info.d);
    o.set("worldX", info.worldX); o.set("worldY", info.worldY);
    return o;
}
static val runtimeGetBoneByName(Handle h, const std::string& name) {
    Runtime::BoneInfo info;
    if (!reinterpret_cast<Runtime*>(h)->getBone(name.c_str(), &info)) return val::null();
    return runtimeGetBone(h, info.index);
}
// Socket compatibility: a,b,c,d,worldX,worldY
static val runtimeFindBone(Handle h, const std::string& name) {
    float out[6];
    if (!reinterpret_cast<Runtime*>(h)->findBone(name.c_str(), out)) return val::null();
    val o = val::object();
    o.set("a", out[0]); o.set("b", out[1]); o.set("c", out[2]); o.set("d", out[3]);
    o.set("worldX", out[4]); o.set("worldY", out[5]);
    return o;
}

// Slot queries / modifications
static int runtimeFindSlotIndex(Handle h, const std::string& name) {
    return reinterpret_cast<Runtime*>(h)->findSlotIndex(name.c_str());
}
static val runtimeGetSlot(Handle h, int index) {
    Runtime::SlotInfo info;
    if (!reinterpret_cast<Runtime*>(h)->getSlot(index, &info)) return val::null();
    val o = val::object();
    o.set("index", info.index);
    o.set("boneIndex", info.boneIndex);
    o.set("name", info.name ? info.name : "");
    o.set("attachmentName", info.attachmentName ? info.attachmentName : "");
    o.set("blendMode", info.blendMode);
    o.set("colorR", info.colorR); o.set("colorG", info.colorG);
    o.set("colorB", info.colorB); o.set("colorA", info.colorA);
    o.set("hasDarkColor", info.hasDarkColor);
    o.set("darkR", info.darkR); o.set("darkG", info.darkG);
    o.set("darkB", info.darkB); o.set("darkA", info.darkA);
    return o;
}
static val runtimeGetSlotByName(Handle h, const std::string& name) {
    Runtime::SlotInfo info;
    if (!reinterpret_cast<Runtime*>(h)->getSlot(name.c_str(), &info)) return val::null();
    return runtimeGetSlot(h, info.index);
}
static bool runtimeSetSlotColor(Handle h, const std::string& name, float r, float g, float b, float a) {
    return reinterpret_cast<Runtime*>(h)->setSlotColor(name.c_str(), r, g, b, a);
}
static bool runtimeSetAttachment(Handle h, const std::string& slot, const std::string& attachment) {
    return reinterpret_cast<Runtime*>(h)->setAttachment(slot.c_str(), attachment.c_str());
}

// Physics / transform / bounds
static val runtimeGetBounds(Handle h) {
    float out[4];
    if (!reinterpret_cast<Runtime*>(h)->getBounds(out)) return val::null();
    val o = val::object();
    o.set("x", out[0]); o.set("y", out[1]); o.set("width", out[2]); o.set("height", out[3]);
    return o;
}

// Update / render data
// Separable pipeline for Cocos culling (see SpineRuntime.h): updateAnimation()
// advances state, updatePose() updates the world pose without generating
// geometry (read bounds via runtimeGetBounds), extractRenderData() /
// updateRenderData() rebuild geometry only when visible. Not wired in TS yet.
static void runtimeUpdateAnimation(Handle h, float dt) {
    reinterpret_cast<Runtime*>(h)->updateAnimation(dt);
}
static void runtimeUpdatePose(Handle h, float dt) {
    reinterpret_cast<Runtime*>(h)->updatePose(dt);
}
static void runtimeExtractRenderData(Handle h) {
    reinterpret_cast<Runtime*>(h)->extractRenderData();
}
static void runtimeUpdateRenderData(Handle h) {
    reinterpret_cast<Runtime*>(h)->updateRenderData();
}
static void runtimeUpdate(Handle h, float dt) {
    reinterpret_cast<Runtime*>(h)->update(dt);
}
static void runtimeSetOutputTransform(Handle h, float a, float b, float c, float d, float tx, float ty) {
    reinterpret_cast<Runtime*>(h)->setOutputTransform(a, b, c, d, tx, ty);
}
static val runtimeRenderData(Handle h) {
    const RenderData& rd = reinterpret_cast<Runtime*>(h)->renderData();
    val obj = val::object();
    obj.set("vertexCount", rd.vertexCount);
    obj.set("indexCount", rd.indexCount);
    obj.set("vertexStrideBytes", rd.vertexStrideBytes);
    obj.set("vPtr", static_cast<uint32_t>(reinterpret_cast<uintptr_t>(rd.vertices)));
    obj.set("iPtr", static_cast<uint32_t>(reinterpret_cast<uintptr_t>(rd.indices)));
    val segs = val::array();
    for (uint32_t i = 0; i < rd.segmentCount; ++i) {
        val s = val::object();
        s.set("indexOffset", rd.segments[i].indexOffset);
        s.set("indexCount", rd.segments[i].indexCount);
        s.set("blendMode", rd.segments[i].blendMode);
        s.set("textureId", rd.segments[i].textureId);
        segs.set(i, s);
    }
    obj.set("segments", segs);
    obj.set("segmentCount", rd.segmentCount);
    obj.set("indexOverflow", rd.indexOverflow);
    return obj;
}

// Events (JS callback bridge)
static void runtimeSetEventListener(Handle h, val cb) {
    gEventListener[h] = cb;
    reinterpret_cast<Runtime*>(h)->setEventListener(&eventBridge, reinterpret_cast<void*>(h));
}
static void runtimeSetListener(Handle h, val cb) {
    gListener[h] = cb;
    reinterpret_cast<Runtime*>(h)->setListener(&listenerBridge, reinterpret_cast<void*>(h));
}

// Remaining: advanced track properties / bone local transform / slot region / physics
static bool runtimeSetTrackAnimationRange(Handle h, uint32_t th, float start, float end, float last) {
    return reinterpret_cast<Runtime*>(h)->setTrackAnimationRange(th, start, end, last);
}
static bool runtimeSetTrackMixDuration(Handle h, uint32_t th, float duration) {
    return reinterpret_cast<Runtime*>(h)->setTrackMixDuration(th, duration);
}
static bool runtimeSetTrackThresholds(Handle h, uint32_t th, float ev, float att, float order) {
    return reinterpret_cast<Runtime*>(h)->setTrackThresholds(th, ev, att, order);
}
static bool runtimeSetBoneLocal(Handle h, const std::string& name,
                                float x, float y, float rot, float sx, float sy, float shx, float shy) {
    return reinterpret_cast<Runtime*>(h)->setBoneLocal(name.c_str(), x, y, rot, sx, sy, shx, shy);
}
static val runtimeBoneWorldToLocal(Handle h, const std::string& name, float wx, float wy) {
    float out[2];
    if (!reinterpret_cast<Runtime*>(h)->boneWorldToLocal(name.c_str(), wx, wy, out)) return val::null();
    val o = val::object();
    o.set("x", out[0]); o.set("y", out[1]);
    return o;
}
static val runtimeBoneLocalToWorld(Handle h, const std::string& name, float lx, float ly) {
    float out[2];
    if (!reinterpret_cast<Runtime*>(h)->boneLocalToWorld(name.c_str(), lx, ly, out)) return val::null();
    val o = val::object();
    o.set("x", out[0]); o.set("y", out[1]);
    return o;
}
static void runtimeSetSlotsRange(Handle h, int start, int end) {
    reinterpret_cast<Runtime*>(h)->setSlotsRange(start, end);
}
static bool runtimeResizeSlotRegion(Handle h, const std::string& name, float w, float hgt, bool createNew) {
    return reinterpret_cast<Runtime*>(h)->resizeSlotRegion(name.c_str(), w, hgt, createNew);
}
static bool runtimeSetSlotTexture(Handle h, const std::string& name, uint32_t textureId) {
    return reinterpret_cast<Runtime*>(h)->setSlotTexture(name.c_str(), textureId);
}

// New runtime additions: advanced track properties / physics modes / attachment queries
static bool runtimeSetTrackShortestRotation(Handle h, uint32_t th, bool shortest) {
    return reinterpret_cast<Runtime*>(h)->setTrackShortestRotation(th, shortest);
}
static bool runtimeSetTrackMixTime(Handle h, uint32_t th, float mixTime) {
    return reinterpret_cast<Runtime*>(h)->setTrackMixTime(th, mixTime);
}
static bool runtimeSetTrackMixDuration3(Handle h, uint32_t th, float duration, float delay) {
    return reinterpret_cast<Runtime*>(h)->setTrackMixDuration(th, duration, delay);
}
static bool runtimeSetTrackAlphaAttachmentThreshold(Handle h, uint32_t th, float threshold) {
    return reinterpret_cast<Runtime*>(h)->setTrackAlphaAttachmentThreshold(th, threshold);
}
static bool runtimeResetTrackRotationDirections(Handle h, uint32_t th) {
    return reinterpret_cast<Runtime*>(h)->resetTrackRotationDirections(th);
}

static val attachmentToVal(const Runtime::AttachmentInfo& info) {
    val o = val::object();
    o.set("slotIndex", info.slotIndex);
    o.set("name", info.name ? info.name : "");
    o.set("path", info.path ? info.path : "");
    o.set("type", info.type);
    o.set("worldVerticesLength", static_cast<uint32_t>(info.worldVerticesLength));
    o.set("width", info.width);
    o.set("height", info.height);
    o.set("textureId", info.textureId);
    o.set("hasTexture", info.hasTexture);
    return o;
}
static val runtimeGetAttachment(Handle h, const std::string& slotName,
                                const std::string& attachmentName) {
    Runtime::AttachmentInfo info;
    if (!reinterpret_cast<Runtime*>(h)->getAttachment(slotName.c_str(),
                                                       attachmentName.c_str(), &info)) {
        return val::null();
    }
    return attachmentToVal(info);
}
static val runtimeGetCurrentAttachment(Handle h, const std::string& slotName) {
    Runtime::AttachmentInfo info;
    if (!reinterpret_cast<Runtime*>(h)->getCurrentAttachment(slotName.c_str(), &info)) {
        return val::null();
    }
    return attachmentToVal(info);
}

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------
EMSCRIPTEN_BINDINGS(spine_runtime) {
    function("createDataJson", &createDataJson);
    function("createDataBinary", &createDataBinary);
    function("lastError", &getLastError);
    function("disposeData", &disposeData);
    function("dataWidth", &dataWidth);
    function("dataHeight", &dataHeight);
    function("dataX", &dataX);
    function("dataY", &dataY);
    function("dataVersion", &dataVersion);
    function("dataAnimationCount", &dataAnimationCount);
    function("dataAnimationName", &dataAnimationName);
    function("dataHasAnimation", &dataHasAnimation);
    function("dataAnimationDuration", &dataAnimationDuration);
    function("dataSkinCount", &dataSkinCount);
    function("dataSkinName", &dataSkinName);
    function("dataHasSkin", &dataHasSkin);
    function("dataTexturePageCount", &dataTexturePageCount);
    function("dataTexturePage", &dataTexturePage);

    function("createRuntime", &createRuntime);
    function("disposeRuntime", &disposeRuntime);
    function("runtimePlay", &runtimePlay);
    function("runtimeSetAnimation", &runtimeSetAnimation);
    function("runtimeAddAnimation", &runtimeAddAnimation);
    function("runtimeSetEmptyAnimation", &runtimeSetEmptyAnimation);
    function("runtimeAddEmptyAnimation", &runtimeAddEmptyAnimation);
    function("runtimeSetEmptyAnimations", &runtimeSetEmptyAnimations);
    function("runtimeGetCurrent", &runtimeGetCurrent);
    function("runtimeClearTrack", &runtimeClearTrack);
    function("runtimeClearTracks", &runtimeClearTracks);
    function("runtimeFindAnimation", &runtimeFindAnimation);
    function("runtimeSetMix", &runtimeSetMix);
    function("runtimeGetTrackInfo", &runtimeGetTrackInfo);
    function("runtimeSetTrackLoop", &runtimeSetTrackLoop);
    function("runtimeSetTrackReverse", &runtimeSetTrackReverse);
    function("runtimeSetTrackAdditive", &runtimeSetTrackAdditive);
    function("runtimeSetTrackDelay", &runtimeSetTrackDelay);
    function("runtimeSetTrackTime", &runtimeSetTrackTime);
    function("runtimeSetTrackEnd", &runtimeSetTrackEnd);
    function("runtimeSetTrackTimeScale", &runtimeSetTrackTimeScale);
    function("runtimeSetTrackAlpha", &runtimeSetTrackAlpha);
    function("runtimeSetSkin", &runtimeSetSkin);
    function("runtimeSetToSetupPose", &runtimeSetToSetupPose);
    function("runtimeSetBonesToSetupPose", &runtimeSetBonesToSetupPose);
    function("runtimeSetSlotsToSetupPose", &runtimeSetSlotsToSetupPose);
    function("runtimeUpdateWorldTransform", &runtimeUpdateWorldTransform);
    function("runtimeSetParams", &runtimeSetParams);
    function("runtimeSetPaused", &runtimeSetPaused);
    function("runtimeBoneCount", &runtimeBoneCount);
    function("runtimeBoneName", &runtimeBoneName);
    function("runtimeFindBoneIndex", &runtimeFindBoneIndex);
    function("runtimeGetBone", &runtimeGetBone);
    function("runtimeGetBoneByName", &runtimeGetBoneByName);
    function("runtimeFindBone", &runtimeFindBone);
    function("runtimeFindSlotIndex", &runtimeFindSlotIndex);
    function("runtimeGetSlot", &runtimeGetSlot);
    function("runtimeGetSlotByName", &runtimeGetSlotByName);
    function("runtimeSetSlotColor", &runtimeSetSlotColor);
    function("runtimeSetAttachment", &runtimeSetAttachment);
    function("runtimeGetBounds", &runtimeGetBounds);
    function("runtimeUpdateAnimation", &runtimeUpdateAnimation);
    function("runtimeUpdatePose", &runtimeUpdatePose);
    function("runtimeExtractRenderData", &runtimeExtractRenderData);
    function("runtimeUpdateRenderData", &runtimeUpdateRenderData);
    function("runtimeUpdate", &runtimeUpdate);
    function("runtimeSetOutputTransform", &runtimeSetOutputTransform);
    function("runtimeRenderData", &runtimeRenderData);
    function("runtimeSetEventListener", &runtimeSetEventListener);
    function("runtimeSetListener", &runtimeSetListener);
    function("runtimeSetTrackAnimationRange", &runtimeSetTrackAnimationRange);
    function("runtimeSetTrackMixDuration", &runtimeSetTrackMixDuration);
    function("runtimeSetTrackThresholds", &runtimeSetTrackThresholds);
    function("runtimeSetBoneLocal", &runtimeSetBoneLocal);
    function("runtimeBoneWorldToLocal", &runtimeBoneWorldToLocal);
    function("runtimeBoneLocalToWorld", &runtimeBoneLocalToWorld);
    function("runtimeSetSlotsRange", &runtimeSetSlotsRange);
    function("runtimeResizeSlotRegion", &runtimeResizeSlotRegion);
    function("runtimeSetSlotTexture", &runtimeSetSlotTexture);
    function("runtimeSetTrackShortestRotation", &runtimeSetTrackShortestRotation);
    function("runtimeSetTrackMixTime", &runtimeSetTrackMixTime);
    function("runtimeSetTrackMixDuration3", &runtimeSetTrackMixDuration3);
    function("runtimeSetTrackAlphaAttachmentThreshold", &runtimeSetTrackAlphaAttachmentThreshold);
    function("runtimeResetTrackRotationDirections", &runtimeResetTrackRotationDirections);
    function("runtimeGetAttachment", &runtimeGetAttachment);
    function("runtimeGetCurrentAttachment", &runtimeGetCurrentAttachment);
}

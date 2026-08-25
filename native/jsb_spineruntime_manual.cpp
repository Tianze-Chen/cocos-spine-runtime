/****************************************************************************
 Copyright (c) 2026
 SpineRuntime JSB manual binding (native -> JS).
 Mirrors the wasm/embind binding with the same "opaque handle + free function"
 interface, so native and web TS share one bindings/index.ts facade.
 - Handles are transported via uint64 (BigInt) to avoid truncating 64-bit pointers.
 - JS callbacks (events) use a global registry bridged from C; root+incRef keep them alive.
 - Render data: native has no HEAPU8, so a JS-allocated staging buffer is registered
   (setRenderBuffer), runtimeRenderData copies into it, and vPtr/iPtr are byte offsets
   within that buffer -- same "heap + offset" shape as wasm.
****************************************************************************/
#include "cocos/bindings/jswrapper/SeApi.h"
#include "spine-adapter/SpineRuntime.h"

#include <string>
#include <unordered_map>
#include <vector>

using namespace cc;
using namespace spineruntime;

namespace {
using Handle = uintptr_t;

// Transport handles through se::Value uint64 (BigInt) so 64-bit pointers are
// not truncated (wasm pointers are 32-bit and fit in uint32; native needs the full 64 bits).
Handle argHandle(const se::Value& v) {
    return static_cast<Handle>(v.toUint64());
}

// JS callback registry keyed by the Runtime handle. Same as the wasm/embind
// binding: C callbacks are bridged back to JS. Callbacks are registered on the
// JS thread and events are dispatched synchronously on it too (runtimeUpdate /
// dispose), so it is safe to call JS directly here.
std::unordered_map<Handle, se::Object*> gEventListener; // runtimeSetEventListener
std::unordered_map<Handle, se::Object*> gListener;      // runtimeSetListener

void callJSListener(se::Object* cb, se::ValueArray&& args) {
    if (!cb || !cb->isFunction()) return;
    se::ScriptEngine::getInstance()->clearException();
    se::AutoHandleScope hs;
    se::Value rval;
    bool succeed = cb->call(args, nullptr, &rval);
    if (!succeed) {
        se::ScriptEngine::getInstance()->clearException();
    }
}

void eventBridge(void* user, const Runtime::EventInfo* event) {
    const Handle handle = reinterpret_cast<Handle>(user);
    auto it = gEventListener.find(handle);
    if (it == gEventListener.end() || !event) return;
    se::HandleObject obj(se::Object::createPlainObject());
    obj->setProperty("type", se::Value(event->type));
    obj->setProperty("track", se::Value(static_cast<uint32_t>(event->track)));
    obj->setProperty("trackIndex", se::Value(event->trackIndex));
    obj->setProperty("animationName", se::Value(event->animationName ? event->animationName : ""));
    obj->setProperty("trackTime", se::Value(event->trackTime));
    obj->setProperty("animationEnd", se::Value(event->animationEnd));
    obj->setProperty("eventName", se::Value(event->eventName ? event->eventName : ""));
    obj->setProperty("eventTime", se::Value(event->eventTime));
    obj->setProperty("intValue", se::Value(event->intValue));
    obj->setProperty("floatValue", se::Value(event->floatValue));
    obj->setProperty("stringValue", se::Value(event->stringValue ? event->stringValue : ""));
    obj->setProperty("audioPath", se::Value(event->audioPath ? event->audioPath : ""));
    obj->setProperty("volume", se::Value(event->volume));
    obj->setProperty("balance", se::Value(event->balance));
    se::ValueArray args;
    args.push_back(se::Value(obj));
    callJSListener(it->second, std::move(args));
}

void listenerBridge(void* user, int eventType, const char* animationName, int trackIndex) {
    const Handle handle = reinterpret_cast<Handle>(user);
    auto it = gListener.find(handle);
    if (it == gListener.end()) return;
    se::ValueArray args;
    args.push_back(se::Value(eventType));
    args.push_back(se::Value(animationName ? animationName : ""));
    args.push_back(se::Value(trackIndex));
    callJSListener(it->second, std::move(args));
}

// Register/replace a JS callback: incRef keeps the C++ wrapper alive (args are
// decRef'd on destruction), root protects the JS function from GC. Releasing the
// previous callback happens before storing the new one.
void storeCallback(std::unordered_map<Handle, se::Object*>& map, Handle handle, const se::Value& cbVal) {
    se::Object* cb = (cbVal.isObject() && cbVal.toObject()->isFunction()) ? cbVal.toObject() : nullptr;
    auto it = map.find(handle);
    if (it != map.end()) {
        it->second->unroot();
        it->second->decRef();
        map.erase(it);
    }
    if (cb) {
        cb->incRef();
        cb->root();
        map[handle] = cb;
    }
}

// Release the callbacks of one handle (called on disposeRuntime to avoid
// handle reuse / leaks).
void clearCallbacks(Handle handle) {
    auto ev = gEventListener.find(handle);
    if (ev != gEventListener.end()) {
        ev->second->unroot();
        ev->second->decRef();
        gEventListener.erase(ev);
    }
    auto lg = gListener.find(handle);
    if (lg != gListener.end()) {
        lg->second->unroot();
        lg->second->decRef();
        gListener.erase(lg);
    }
}

// Release all JS callbacks (called at engine cleanup).
void clearAllCallbacks() {
    for (auto& kv : gEventListener) {
        kv.second->unroot();
        kv.second->decRef();
    }
    gEventListener.clear();
    for (auto& kv : gListener) {
        kv.second->unroot();
        kv.second->decRef();
    }
    gListener.clear();
}

// ---------------------------------------------------------------------------
// Render data staging buffer (the native counterpart of HEAPU8)
// ---------------------------------------------------------------------------
// On wasm, vPtr/iPtr are offsets into the wasm linear memory read via
// Module.HEAPU8. Native has no HEAPU8: the TS side allocates a Uint8Array and
// registers it (see setRenderBuffer); runtimeRenderData copies the runtime's C++
// vertices/indices into it and returns vPtr/iPtr as byte offsets within that
// buffer (vPtr=0, iPtr=vertexBytes). This keeps the "heap + offset" shape
// identical for the TS assembler on both platforms. Multiple components share
// the buffer: each component's copy->read happens synchronously inside a single
// _render call, so they never overwrite each other.
se::Object* gRenderBuffer = nullptr;     // JS Uint8Array (rooted to keep it alive)
uint8_t* gRenderBufferPtr = nullptr;     // pointer to its backing store
size_t gRenderBufferCapacity = 0;

void releaseRenderBuffer() {
    if (gRenderBuffer) {
        gRenderBuffer->unroot();
        gRenderBuffer->decRef();
        gRenderBuffer = nullptr;
    }
    gRenderBufferPtr = nullptr;
    gRenderBufferCapacity = 0;
}

// The returned se::Object* owns the initial reference from createPlainObject.
// The caller must decRef after setObject (the rval holds its own reference).
se::Object* makeAttachmentObj(const Runtime::AttachmentInfo& info) {
    se::Object* obj = se::Object::createPlainObject();
    obj->setProperty("slotIndex", se::Value(static_cast<int32_t>(info.slotIndex)));
    obj->setProperty("name", se::Value(info.name ? info.name : ""));
    obj->setProperty("path", se::Value(info.path ? info.path : ""));
    obj->setProperty("type", se::Value(static_cast<int32_t>(info.type)));
    obj->setProperty("worldVerticesLength", se::Value(static_cast<int32_t>(info.worldVerticesLength)));
    obj->setProperty("width", se::Value(info.width));
    obj->setProperty("height", se::Value(info.height));
    obj->setProperty("textureId", se::Value(info.textureId));
    obj->setProperty("hasTexture", se::Value(info.hasTexture));
    return obj;
}
}  // namespace

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------
static bool js_spineruntime_createDataJson(se::State& s) {
    const auto& args = s.args();
    if (args.size() < 4) return false;
    std::string json = args[0].toStringForce();
    std::string atlas = args[1].toStringForce();
    std::vector<std::string> names;
    se::Object* arr = args[2].toObject();
    if (arr && arr->isArray()) {
        uint32_t len = 0;
        arr->getArrayLength(&len);
        for (uint32_t i = 0; i < len; ++i) {
            se::Value v;
            if (arr->getArrayElement(i, &v)) names.emplace_back(v.toStringForce());
        }
    }
    float scale = args[3].toFloat();
    std::vector<const char*> ptrs;
    for (const auto& n : names) ptrs.push_back(n.c_str());
    Data* d = Data::create(json.c_str(), json.size(), false, atlas.c_str(),
                           ptrs.data(), static_cast<int>(names.size()), scale);
    s.rval().setUint64(reinterpret_cast<uint64_t>(d));
    return true;
}
SE_BIND_FUNC(js_spineruntime_createDataJson)

// Binary (.skel) counterpart of js_spineruntime_createDataJson. args[0] is a JS
// Uint8Array; getTypedArrayData() reads its backing pointer directly (no copy),
// same API js_spineruntime_setRenderBuffer already relies on.
static bool js_spineruntime_createDataBinary(se::State& s) {
    const auto& args = s.args();
    if (args.size() < 4 || !args[0].isObject()) return false;
    se::Object* buf = args[0].toObject();
    uint8_t* ptr = nullptr;
    size_t len = 0;
    if (!buf->isTypedArray() || !buf->getTypedArrayData(&ptr, &len) || !ptr) return false;
    std::string atlas = args[1].toStringForce();
    std::vector<std::string> names;
    se::Object* arr = args[2].toObject();
    if (arr && arr->isArray()) {
        uint32_t arrLen = 0;
        arr->getArrayLength(&arrLen);
        for (uint32_t i = 0; i < arrLen; ++i) {
            se::Value v;
            if (arr->getArrayElement(i, &v)) names.emplace_back(v.toStringForce());
        }
    }
    float scale = args[3].toFloat();
    std::vector<const char*> ptrs;
    for (const auto& n : names) ptrs.push_back(n.c_str());
    Data* d = Data::create(ptr, len, true, atlas.c_str(),
                           ptrs.data(), static_cast<int>(names.size()), scale);
    s.rval().setUint64(reinterpret_cast<uint64_t>(d));
    return true;
}
SE_BIND_FUNC(js_spineruntime_createDataBinary)

#define SF_DATA_FN_RET_VOID(name, expr) \
    static bool js_spineruntime_##name(se::State& s) { \
        Data* d = reinterpret_cast<Data*>(argHandle(s.args()[0])); \
        expr; \
        return true; \
    } \
    SE_BIND_FUNC(js_spineruntime_##name)

#define SF_DATA_FN_RET_NUM(name, expr) \
    static bool js_spineruntime_##name(se::State& s) { \
        Data* d = reinterpret_cast<Data*>(argHandle(s.args()[0])); \
        s.rval().setFloat(expr); \
        return true; \
    } \
    SE_BIND_FUNC(js_spineruntime_##name)

#define SF_DATA_FN_RET_INT(name, expr) \
    static bool js_spineruntime_##name(se::State& s) { \
        Data* d = reinterpret_cast<Data*>(argHandle(s.args()[0])); \
        s.rval().setInt32(expr); \
        return true; \
    } \
    SE_BIND_FUNC(js_spineruntime_##name)

#define SF_DATA_FN_RET_STR(name, expr) \
    static bool js_spineruntime_##name(se::State& s) { \
        Data* d = reinterpret_cast<Data*>(argHandle(s.args()[0])); \
        const char* r = expr; \
        s.rval().setString(r ? r : ""); \
        return true; \
    } \
    SE_BIND_FUNC(js_spineruntime_##name)

SF_DATA_FN_RET_VOID(disposeData, d->dispose())
SF_DATA_FN_RET_NUM(dataWidth, d->width())
SF_DATA_FN_RET_NUM(dataHeight, d->height())
SF_DATA_FN_RET_NUM(dataX, d->x())
SF_DATA_FN_RET_NUM(dataY, d->y())
SF_DATA_FN_RET_STR(dataVersion, d->version())
SF_DATA_FN_RET_INT(dataAnimationCount, d->animationCount())

static bool js_spineruntime_dataAnimationName(se::State& s) {
    Data* d = reinterpret_cast<Data*>(argHandle(s.args()[0]));
    const char* n = d->animationName(s.args()[1].toInt32());
    s.rval().setString(n ? n : "");
    return true;
}
SE_BIND_FUNC(js_spineruntime_dataAnimationName)

static bool js_spineruntime_dataHasAnimation(se::State& s) {
    Data* d = reinterpret_cast<Data*>(argHandle(s.args()[0]));
    s.rval().setBoolean(d->hasAnimation(s.args()[1].toStringForce().c_str()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_dataHasAnimation)

static bool js_spineruntime_dataAnimationDuration(se::State& s) {
    Data* d = reinterpret_cast<Data*>(argHandle(s.args()[0]));
    s.rval().setFloat(d->animationDuration(s.args()[1].toStringForce().c_str()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_dataAnimationDuration)

SF_DATA_FN_RET_INT(dataSkinCount, d->skinCount())
SF_DATA_FN_RET_STR(dataSkinName, d->skinName(s.args()[1].toInt32()))

static bool js_spineruntime_dataHasSkin(se::State& s) {
    Data* d = reinterpret_cast<Data*>(argHandle(s.args()[0]));
    s.rval().setBoolean(d->hasSkin(s.args()[1].toStringForce().c_str()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_dataHasSkin)

SF_DATA_FN_RET_INT(dataTexturePageCount, d->texturePageCount())

static bool js_spineruntime_dataTexturePage(se::State& s) {
    Data* d = reinterpret_cast<Data*>(argHandle(s.args()[0]));
    TexturePageInfo info;
    if (!d->texturePage(s.args()[1].toInt32(), &info)) {
        s.rval().setNull();
        return true;
    }
    se::HandleObject obj(se::Object::createPlainObject());
    obj->setProperty("textureId", se::Value(info.textureId));
    obj->setProperty("width", se::Value(info.width));
    obj->setProperty("height", se::Value(info.height));
    obj->setProperty("minFilter", se::Value(info.minFilter));
    obj->setProperty("magFilter", se::Value(info.magFilter));
    obj->setProperty("uWrap", se::Value(info.uWrap));
    obj->setProperty("vWrap", se::Value(info.vWrap));
    obj->setProperty("premultipliedAlpha", se::Value(info.premultipliedAlpha));
    s.rval().setObject(obj);
    return true;
}
SE_BIND_FUNC(js_spineruntime_dataTexturePage)

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------
static bool js_spineruntime_createRuntime(se::State& s) {
    Data* d = reinterpret_cast<Data*>(argHandle(s.args()[0]));
    Runtime* r = Runtime::create(d);
    s.rval().setUint64(reinterpret_cast<uint64_t>(r));
    return true;
}
SE_BIND_FUNC(js_spineruntime_createRuntime)

static bool js_spineruntime_disposeRuntime(se::State& s) {
    const Handle handle = argHandle(s.args()[0]);
    // Release the JS callback references after dispose: dispose dispatches the
    // DISPOSE event synchronously, so the callbacks must still be valid while it
    // runs. The handle may be reused afterward, so the registry entries must go.
    reinterpret_cast<Runtime*>(handle)->dispose();
    clearCallbacks(handle);
    return true;
}
SE_BIND_FUNC(js_spineruntime_disposeRuntime)

// Register the JS-allocated render staging buffer (Uint8Array). Native render
// data is copied here and the TS assembler reads it via vPtr/iPtr offsets, with
// the same shape as the wasm HEAPU8.
static bool js_spineruntime_setRenderBuffer(se::State& s) {
    const auto& args = s.args();
    if (args.empty() || !args[0].isObject()) return false;
    se::Object* buf = args[0].toObject();
    uint8_t* ptr = nullptr;
    size_t len = 0;
    if (!buf->getTypedArrayData(&ptr, &len) || !ptr) return false;
    releaseRenderBuffer();
    gRenderBuffer = buf;
    gRenderBuffer->root();
    gRenderBuffer->incRef(); // own the C++ wrapper independently (args decRefs on destruction)
    gRenderBufferPtr = ptr;
    gRenderBufferCapacity = len;
    return true;
}
SE_BIND_FUNC(js_spineruntime_setRenderBuffer)

static bool js_spineruntime_getRenderDataBuffer(se::State& s) {
    if (gRenderBuffer) {
        s.rval().setObject(gRenderBuffer);
    } else {
        s.rval().setUndefined();
    }
    return true;
}
SE_BIND_FUNC(js_spineruntime_getRenderDataBuffer)

static bool js_spineruntime_runtimePlay(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    r->play(s.args()[1].toStringForce().c_str(), s.args()[2].toBoolean());
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimePlay)

static bool js_spineruntime_runtimeSetAnimation(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    Runtime::TrackHandle h = r->setAnimation(s.args()[1].toInt32(),
                                             s.args()[2].toStringForce().c_str(),
                                             s.args()[3].toBoolean());
    s.rval().setUint64(static_cast<uint64_t>(h));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetAnimation)

static bool js_spineruntime_runtimeAddAnimation(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    Runtime::TrackHandle h = r->addAnimation(s.args()[1].toInt32(),
                                             s.args()[2].toStringForce().c_str(),
                                             s.args()[3].toBoolean(),
                                             s.args()[4].toFloat());
    s.rval().setUint64(static_cast<uint64_t>(h));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeAddAnimation)

static bool js_spineruntime_runtimeClearTrack(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    r->clearTrack(s.args()[1].toInt32());
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeClearTrack)

static bool js_spineruntime_runtimeClearTracks(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    r->clearTracks();
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeClearTracks)

static bool js_spineruntime_runtimeGetCurrent(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    Runtime::TrackHandle h = r->getCurrent(s.args()[1].toInt32());
    s.rval().setUint64(static_cast<uint64_t>(h));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeGetCurrent)

static bool js_spineruntime_runtimeSetMix(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    r->setMix(s.args()[1].toStringForce().c_str(),
              s.args()[2].toStringForce().c_str(),
              s.args()[3].toFloat());
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetMix)

static bool js_spineruntime_runtimeSetToSetupPose(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    r->setToSetupPose();
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetToSetupPose)

static bool js_spineruntime_runtimeSetBonesToSetupPose(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    r->setBonesToSetupPose();
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetBonesToSetupPose)

static bool js_spineruntime_runtimeSetSlotsToSetupPose(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    r->setSlotsToSetupPose();
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetSlotsToSetupPose)

static bool js_spineruntime_runtimeFindBone(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    float out[6];
    if (!r->findBone(s.args()[1].toStringForce().c_str(), out)) {
        s.rval().setNull();
        return true;
    }
    se::HandleObject obj(se::Object::createPlainObject());
    obj->setProperty("a", se::Value(out[0]));
    obj->setProperty("b", se::Value(out[1]));
    obj->setProperty("c", se::Value(out[2]));
    obj->setProperty("d", se::Value(out[3]));
    obj->setProperty("worldX", se::Value(out[4]));
    obj->setProperty("worldY", se::Value(out[5]));
    s.rval().setObject(obj);
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeFindBone)

static bool js_spineruntime_runtimeFindSlotIndex(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    s.rval().setInt32(r->findSlotIndex(s.args()[1].toStringForce().c_str()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeFindSlotIndex)

static bool js_spineruntime_runtimeGetSlot(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    Runtime::SlotInfo info;
    if (!r->getSlot(s.args()[1].toInt32(), &info)) {
        s.rval().setNull();
        return true;
    }
    se::HandleObject obj(se::Object::createPlainObject());
    obj->setProperty("index", se::Value(info.index));
    obj->setProperty("boneIndex", se::Value(info.boneIndex));
    obj->setProperty("name", se::Value(info.name ? info.name : ""));
    obj->setProperty("attachmentName", se::Value(info.attachmentName ? info.attachmentName : ""));
    obj->setProperty("blendMode", se::Value(info.blendMode));
    obj->setProperty("colorR", se::Value(info.colorR));
    obj->setProperty("colorG", se::Value(info.colorG));
    obj->setProperty("colorB", se::Value(info.colorB));
    obj->setProperty("colorA", se::Value(info.colorA));
    obj->setProperty("hasDarkColor", se::Value(info.hasDarkColor));
    obj->setProperty("darkR", se::Value(info.darkR));
    obj->setProperty("darkG", se::Value(info.darkG));
    obj->setProperty("darkB", se::Value(info.darkB));
    obj->setProperty("darkA", se::Value(info.darkA));
    s.rval().setObject(obj);
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeGetSlot)

static bool js_spineruntime_runtimeSetAttachment(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    s.rval().setBoolean(r->setAttachment(s.args()[1].toStringForce().c_str(),
                                         s.args()[2].toStringForce().c_str()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetAttachment)

static bool js_spineruntime_runtimeSetSlotColor(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    s.rval().setBoolean(r->setSlotColor(s.args()[1].toStringForce().c_str(),
                                        s.args()[2].toFloat(),
                                        s.args()[3].toFloat(),
                                        s.args()[4].toFloat(),
                                        s.args()[5].toFloat()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetSlotColor)

static bool js_spineruntime_runtimeGetAttachment(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    Runtime::AttachmentInfo info;
    if (!r->getAttachment(s.args()[1].toStringForce().c_str(),
                          s.args()[2].toStringForce().c_str(), &info)) {
        s.rval().setNull();
        return true;
    }
    se::Object* obj = makeAttachmentObj(info);
    s.rval().setObject(obj);
    obj->decRef(); // the rval holds a reference now; release makeAttachmentObj's initial ref
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeGetAttachment)

static bool js_spineruntime_runtimeSetSlotTexture(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    s.rval().setBoolean(r->setSlotTexture(s.args()[1].toStringForce().c_str(),
                                          static_cast<uint32_t>(s.args()[2].toUint32())));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetSlotTexture)

static bool js_spineruntime_runtimeSetSlotsRange(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    r->setSlotsRange(s.args()[1].toInt32(), s.args()[2].toInt32());
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetSlotsRange)

static bool js_spineruntime_runtimeResizeSlotRegion(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    s.rval().setBoolean(r->resizeSlotRegion(s.args()[1].toStringForce().c_str(),
                                            s.args()[2].toFloat(),
                                            s.args()[3].toFloat(),
                                            s.args()[4].toBoolean()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeResizeSlotRegion)

static bool js_spineruntime_runtimeSetEmptyAnimation(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    r->setEmptyAnimation(s.args()[1].toInt32(), s.args()[2].toFloat());
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetEmptyAnimation)

static bool js_spineruntime_runtimeAddEmptyAnimation(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    r->addEmptyAnimation(s.args()[1].toInt32(), s.args()[2].toFloat(), s.args()[3].toFloat());
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeAddEmptyAnimation)

static bool js_spineruntime_runtimeSetEmptyAnimations(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    r->setEmptyAnimations(s.args()[1].toFloat());
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetEmptyAnimations)

static bool js_spineruntime_runtimeGetTrackInfo(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    Runtime::TrackHandle th = static_cast<Runtime::TrackHandle>(s.args()[1].toUint64());
    Runtime::TrackInfo info;
    if (!r->getTrackInfo(th, &info)) {
        s.rval().setNull();
        return true;
    }
    se::HandleObject obj(se::Object::createPlainObject());
    obj->setProperty("handle", se::Value(static_cast<uint64_t>(info.handle)));
    obj->setProperty("next", se::Value(static_cast<uint64_t>(info.next)));
    obj->setProperty("mixingFrom", se::Value(static_cast<uint64_t>(info.mixingFrom)));
    obj->setProperty("mixingTo", se::Value(static_cast<uint64_t>(info.mixingTo)));
    obj->setProperty("trackIndex", se::Value(info.trackIndex));
    obj->setProperty("animationName", se::Value(info.animationName ? info.animationName : ""));
    obj->setProperty("loop", se::Value(info.loop));
    obj->setProperty("reverse", se::Value(info.reverse));
    obj->setProperty("additive", se::Value(info.additive));
    obj->setProperty("shortestRotation", se::Value(info.shortestRotation));
    obj->setProperty("complete", se::Value(info.complete));
    obj->setProperty("emptyAnimation", se::Value(info.emptyAnimation));
    obj->setProperty("wasApplied", se::Value(info.wasApplied));
    obj->setProperty("nextReady", se::Value(info.nextReady));
    obj->setProperty("delay", se::Value(info.delay));
    obj->setProperty("trackTime", se::Value(info.trackTime));
    obj->setProperty("trackEnd", se::Value(info.trackEnd));
    obj->setProperty("animationStart", se::Value(info.animationStart));
    obj->setProperty("animationEnd", se::Value(info.animationEnd));
    obj->setProperty("animationLast", se::Value(info.animationLast));
    obj->setProperty("animationTime", se::Value(info.animationTime));
    obj->setProperty("timeScale", se::Value(info.timeScale));
    obj->setProperty("alpha", se::Value(info.alpha));
    obj->setProperty("mixTime", se::Value(info.mixTime));
    obj->setProperty("mixDuration", se::Value(info.mixDuration));
    obj->setProperty("trackComplete", se::Value(info.trackComplete));
    obj->setProperty("eventThreshold", se::Value(info.eventThreshold));
    obj->setProperty("mixAttachmentThreshold", se::Value(info.mixAttachmentThreshold));
    obj->setProperty("alphaAttachmentThreshold", se::Value(info.alphaAttachmentThreshold));
    obj->setProperty("mixDrawOrderThreshold", se::Value(info.mixDrawOrderThreshold));
    s.rval().setObject(obj);
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeGetTrackInfo)

static bool js_spineruntime_runtimeSetTrackLoop(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    s.rval().setBoolean(r->setTrackLoop(static_cast<Runtime::TrackHandle>(s.args()[1].toUint64()), s.args()[2].toBoolean()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetTrackLoop)

static bool js_spineruntime_runtimeSetTrackReverse(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    s.rval().setBoolean(r->setTrackReverse(static_cast<Runtime::TrackHandle>(s.args()[1].toUint64()), s.args()[2].toBoolean()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetTrackReverse)

static bool js_spineruntime_runtimeSetTrackAdditive(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    s.rval().setBoolean(r->setTrackAdditive(static_cast<Runtime::TrackHandle>(s.args()[1].toUint64()), s.args()[2].toBoolean()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetTrackAdditive)

static bool js_spineruntime_runtimeSetTrackDelay(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    s.rval().setBoolean(r->setTrackDelay(static_cast<Runtime::TrackHandle>(s.args()[1].toUint64()), s.args()[2].toFloat()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetTrackDelay)

static bool js_spineruntime_runtimeSetTrackTime(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    s.rval().setBoolean(r->setTrackTime(static_cast<Runtime::TrackHandle>(s.args()[1].toUint64()), s.args()[2].toFloat()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetTrackTime)

static bool js_spineruntime_runtimeSetTrackEnd(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    s.rval().setBoolean(r->setTrackEnd(static_cast<Runtime::TrackHandle>(s.args()[1].toUint64()), s.args()[2].toFloat()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetTrackEnd)

static bool js_spineruntime_runtimeSetTrackTimeScale(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    s.rval().setBoolean(r->setTrackTimeScale(static_cast<Runtime::TrackHandle>(s.args()[1].toUint64()), s.args()[2].toFloat()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetTrackTimeScale)

static bool js_spineruntime_runtimeSetTrackAlpha(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    s.rval().setBoolean(r->setTrackAlpha(static_cast<Runtime::TrackHandle>(s.args()[1].toUint64()), s.args()[2].toFloat()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetTrackAlpha)

static bool js_spineruntime_runtimeSetTrackAnimationRange(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    Runtime::TrackHandle th = static_cast<Runtime::TrackHandle>(s.args()[1].toUint64());
    s.rval().setBoolean(r->setTrackAnimationRange(th, s.args()[2].toFloat(), s.args()[3].toFloat(), s.args()[4].toFloat()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetTrackAnimationRange)

static bool js_spineruntime_runtimeSetTrackMixDuration(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    Runtime::TrackHandle th = static_cast<Runtime::TrackHandle>(s.args()[1].toUint64());
    s.rval().setBoolean(r->setTrackMixDuration(th, s.args()[2].toFloat()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetTrackMixDuration)

static bool js_spineruntime_runtimeSetTrackMixDuration3(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    Runtime::TrackHandle th = static_cast<Runtime::TrackHandle>(s.args()[1].toUint64());
    s.rval().setBoolean(r->setTrackMixDuration(th, s.args()[2].toFloat(), s.args()[3].toFloat()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetTrackMixDuration3)

static bool js_spineruntime_runtimeSetTrackMixTime(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    Runtime::TrackHandle th = static_cast<Runtime::TrackHandle>(s.args()[1].toUint64());
    s.rval().setBoolean(r->setTrackMixTime(th, s.args()[2].toFloat()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetTrackMixTime)

static bool js_spineruntime_runtimeSetTrackThresholds(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    Runtime::TrackHandle th = static_cast<Runtime::TrackHandle>(s.args()[1].toUint64());
    s.rval().setBoolean(r->setTrackThresholds(th, s.args()[2].toFloat(), s.args()[3].toFloat(), s.args()[4].toFloat()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetTrackThresholds)

static bool js_spineruntime_runtimeSetTrackShortestRotation(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    Runtime::TrackHandle th = static_cast<Runtime::TrackHandle>(s.args()[1].toUint64());
    s.rval().setBoolean(r->setTrackShortestRotation(th, s.args()[2].toBoolean()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetTrackShortestRotation)

static bool js_spineruntime_runtimeSetTrackAlphaAttachmentThreshold(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    Runtime::TrackHandle th = static_cast<Runtime::TrackHandle>(s.args()[1].toUint64());
    s.rval().setBoolean(r->setTrackAlphaAttachmentThreshold(th, s.args()[2].toFloat()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetTrackAlphaAttachmentThreshold)

static bool js_spineruntime_runtimeResetTrackRotationDirections(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    Runtime::TrackHandle th = static_cast<Runtime::TrackHandle>(s.args()[1].toUint64());
    s.rval().setBoolean(r->resetTrackRotationDirections(th));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeResetTrackRotationDirections)

static bool js_spineruntime_runtimeGetBone(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    Runtime::BoneInfo info;
    if (!r->getBone(s.args()[1].toInt32(), &info)) {
        s.rval().setNull();
        return true;
    }
    se::HandleObject obj(se::Object::createPlainObject());
    obj->setProperty("index", se::Value(info.index));
    obj->setProperty("parentIndex", se::Value(info.parentIndex));
    obj->setProperty("name", se::Value(info.name ? info.name : ""));
    obj->setProperty("active", se::Value(info.active));
    obj->setProperty("x", se::Value(info.x));
    obj->setProperty("y", se::Value(info.y));
    obj->setProperty("rotation", se::Value(info.rotation));
    obj->setProperty("scaleX", se::Value(info.scaleX));
    obj->setProperty("scaleY", se::Value(info.scaleY));
    obj->setProperty("shearX", se::Value(info.shearX));
    obj->setProperty("shearY", se::Value(info.shearY));
    obj->setProperty("a", se::Value(info.a));
    obj->setProperty("b", se::Value(info.b));
    obj->setProperty("c", se::Value(info.c));
    obj->setProperty("d", se::Value(info.d));
    obj->setProperty("worldX", se::Value(info.worldX));
    obj->setProperty("worldY", se::Value(info.worldY));
    s.rval().setObject(obj);
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeGetBone)

static bool js_spineruntime_runtimeSetBoneLocal(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    s.rval().setBoolean(r->setBoneLocal(s.args()[1].toStringForce().c_str(),
                                        s.args()[2].toFloat(), s.args()[3].toFloat(),
                                        s.args()[4].toFloat(), s.args()[5].toFloat(),
                                        s.args()[6].toFloat(), s.args()[7].toFloat(),
                                        s.args()[8].toFloat()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetBoneLocal)

static bool js_spineruntime_runtimeBoneWorldToLocal(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    float out[2];
    if (!r->boneWorldToLocal(s.args()[1].toStringForce().c_str(),
                             s.args()[2].toFloat(), s.args()[3].toFloat(), out)) {
        s.rval().setNull();
        return true;
    }
    se::HandleObject obj(se::Object::createPlainObject());
    obj->setProperty("x", se::Value(out[0]));
    obj->setProperty("y", se::Value(out[1]));
    s.rval().setObject(obj);
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeBoneWorldToLocal)

static bool js_spineruntime_runtimeBoneLocalToWorld(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    float out[2];
    if (!r->boneLocalToWorld(s.args()[1].toStringForce().c_str(),
                             s.args()[2].toFloat(), s.args()[3].toFloat(), out)) {
        s.rval().setNull();
        return true;
    }
    se::HandleObject obj(se::Object::createPlainObject());
    obj->setProperty("x", se::Value(out[0]));
    obj->setProperty("y", se::Value(out[1]));
    s.rval().setObject(obj);
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeBoneLocalToWorld)

static bool js_spineruntime_runtimeSetSkin(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    s.rval().setBoolean(r->setSkin(s.args()[1].toStringForce().c_str()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetSkin)

static bool js_spineruntime_runtimeSetParams(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    Runtime::Params p;
    p.timeScale = s.args()[1].toFloat();
    p.colorR = s.args()[2].toFloat();
    p.colorG = s.args()[3].toFloat();
    p.colorB = s.args()[4].toFloat();
    p.colorA = s.args()[5].toFloat();
    p.premultipliedAlpha = s.args()[6].toBoolean();
    p.useTint = s.args()[7].toBoolean();
    r->setParams(p);
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetParams)

static bool js_spineruntime_runtimeSetPaused(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    r->setPaused(s.args()[1].toBoolean());
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetPaused)

// Separable update pipeline, mirrors the wasm/embind binding. Reserved for
// Cocos culling: updateAnimation() advances state, updatePose() updates the
// world pose without generating geometry, extractRenderData()/updateRenderData()
// rebuild geometry only when visible, updateWorldTransform() refreshes the pose
// on demand.
static bool js_spineruntime_runtimeUpdateAnimation(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    r->updateAnimation(s.args()[1].toFloat());
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeUpdateAnimation)

static bool js_spineruntime_runtimeUpdatePose(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    r->updatePose(s.args()[1].toFloat());
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeUpdatePose)

static bool js_spineruntime_runtimeExtractRenderData(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    r->extractRenderData();
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeExtractRenderData)

static bool js_spineruntime_runtimeUpdateRenderData(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    r->updateRenderData();
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeUpdateRenderData)

static bool js_spineruntime_runtimeUpdateWorldTransform(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    r->updateWorldTransform();
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeUpdateWorldTransform)

static bool js_spineruntime_runtimeUpdate(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    r->update(s.args()[1].toFloat());
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeUpdate)

static bool js_spineruntime_runtimeSetOutputTransform(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    r->setOutputTransform(s.args()[1].toFloat(), s.args()[2].toFloat(),
                          s.args()[3].toFloat(), s.args()[4].toFloat(),
                          s.args()[5].toFloat(), s.args()[6].toFloat());
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetOutputTransform)

static bool js_spineruntime_runtimeRenderData(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    const RenderData& rd = r->renderData();

    // Native has no HEAPU8: copy the C++ vertices/indices into the JS-registered
    // shared staging buffer and report vPtr/iPtr as byte offsets within it
    // (vPtr=0, iPtr=vertexBytes). Same "heap + offset" shape as the wasm path,
    // so the TS assembler does not need to branch on platform.
    const uint32_t vertexBytes = rd.vertexCount * rd.vertexStrideBytes;
    const uint32_t indexBytes = rd.indexCount * static_cast<uint32_t>(sizeof(uint16_t));
    const uint32_t needed = vertexBytes + indexBytes;
    bool copied = false;
    if (gRenderBufferPtr && gRenderBufferCapacity >= needed) {
        if (rd.vertices) {
            memcpy(gRenderBufferPtr, rd.vertices, vertexBytes);
            copied = true;
        }
        if (rd.indices) {
            memcpy(gRenderBufferPtr + vertexBytes, rd.indices, indexBytes);
        }
    }

    se::HandleObject obj(se::Object::createPlainObject());
    obj->setProperty("vertexCount", se::Value(rd.vertexCount));
    obj->setProperty("indexCount", se::Value(rd.indexCount));
    obj->setProperty("vertexStrideBytes", se::Value(rd.vertexStrideBytes));
    obj->setProperty("vPtr", se::Value(static_cast<uint32_t>(0)));
    obj->setProperty("iPtr", se::Value(vertexBytes));
    obj->setProperty("segmentCount", se::Value(rd.segmentCount));
    obj->setProperty("indexOverflow", se::Value(rd.indexOverflow || !copied));
    // segments array
    se::HandleObject segArr(se::Object::createArrayObject(rd.segmentCount));
    for (uint32_t i = 0; i < rd.segmentCount; ++i) {
        se::HandleObject seg(se::Object::createPlainObject());
        seg->setProperty("indexOffset", se::Value(rd.segments[i].indexOffset));
        seg->setProperty("indexCount", se::Value(rd.segments[i].indexCount));
        seg->setProperty("blendMode", se::Value(rd.segments[i].blendMode));
        seg->setProperty("textureId", se::Value(rd.segments[i].textureId));
        segArr->setArrayElement(i, se::Value(seg));
    }
    obj->setProperty("segments", se::Value(segArr));
    s.rval().setObject(obj);
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeRenderData)

static bool js_spineruntime_runtimeGetBoneByName(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    Runtime::BoneInfo info;
    if (!r->getBone(s.args()[1].toStringForce().c_str(), &info)) {
        s.rval().setNull();
        return true;
    }
    se::HandleObject obj(se::Object::createPlainObject());
    obj->setProperty("index", se::Value(info.index));
    obj->setProperty("parentIndex", se::Value(info.parentIndex));
    obj->setProperty("name", se::Value(info.name ? info.name : ""));
    obj->setProperty("active", se::Value(info.active));
    obj->setProperty("x", se::Value(info.x));
    obj->setProperty("y", se::Value(info.y));
    obj->setProperty("rotation", se::Value(info.rotation));
    obj->setProperty("scaleX", se::Value(info.scaleX));
    obj->setProperty("scaleY", se::Value(info.scaleY));
    obj->setProperty("shearX", se::Value(info.shearX));
    obj->setProperty("shearY", se::Value(info.shearY));
    obj->setProperty("a", se::Value(info.a));
    obj->setProperty("b", se::Value(info.b));
    obj->setProperty("c", se::Value(info.c));
    obj->setProperty("d", se::Value(info.d));
    obj->setProperty("worldX", se::Value(info.worldX));
    obj->setProperty("worldY", se::Value(info.worldY));
    s.rval().setObject(obj);
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeGetBoneByName)

static bool js_spineruntime_runtimeGetCurrentAttachment(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    Runtime::AttachmentInfo info;
    if (!r->getCurrentAttachment(s.args()[1].toStringForce().c_str(), &info)) {
        s.rval().setNull();
        return true;
    }
    se::Object* obj = makeAttachmentObj(info);
    s.rval().setObject(obj);
    obj->decRef(); // the rval holds a reference now; release makeAttachmentObj's initial ref
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeGetCurrentAttachment)

static bool js_spineruntime_runtimeGetBounds(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    float out[4];
    if (!r->getBounds(out)) {
        s.rval().setNull();
        return true;
    }
    se::HandleObject obj(se::Object::createPlainObject());
    obj->setProperty("x", se::Value(out[0]));
    obj->setProperty("y", se::Value(out[1]));
    obj->setProperty("width", se::Value(out[2]));
    obj->setProperty("height", se::Value(out[3]));
    s.rval().setObject(obj);
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeGetBounds)

static bool js_spineruntime_runtimeSetEventListener(se::State& s) {
    const Handle handle = argHandle(s.args()[0]);
    Runtime* r = reinterpret_cast<Runtime*>(handle);
    storeCallback(gEventListener, handle, s.args()[1]);
    r->setEventListener(&eventBridge, reinterpret_cast<void*>(handle));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetEventListener)

static bool js_spineruntime_runtimeSetListener(se::State& s) {
    const Handle handle = argHandle(s.args()[0]);
    Runtime* r = reinterpret_cast<Runtime*>(handle);
    storeCallback(gListener, handle, s.args()[1]);
    r->setListener(&listenerBridge, reinterpret_cast<void*>(handle));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeSetListener)

static bool js_spineruntime_runtimeFindAnimation(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    s.rval().setBoolean(r->findAnimation(s.args()[1].toStringForce().c_str()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeFindAnimation)

static bool js_spineruntime_runtimeBoneCount(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    s.rval().setInt32(r->boneCount());
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeBoneCount)

static bool js_spineruntime_runtimeBoneName(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    const char* name = r->boneName(s.args()[1].toInt32());
    s.rval().setString(name ? name : "");
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeBoneName)

static bool js_spineruntime_runtimeFindBoneIndex(se::State& s) {
    Runtime* r = reinterpret_cast<Runtime*>(argHandle(s.args()[0]));
    s.rval().setInt32(r->findBoneIndex(s.args()[1].toStringForce().c_str()));
    return true;
}
SE_BIND_FUNC(js_spineruntime_runtimeFindBoneIndex)

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
bool register_all_spineruntime_manual(se::Object* obj) {
    se::Value nsVal;
    if (!obj->getProperty("spineruntime", &nsVal)) {
        se::HandleObject jsobj(se::Object::createPlainObject());
        nsVal.setObject(jsobj);
        obj->setProperty("spineruntime", nsVal);
    }
    se::Object* ns = nsVal.toObject();

    ns->defineFunction("createDataJson", _SE(js_spineruntime_createDataJson));
    ns->defineFunction("createDataBinary", _SE(js_spineruntime_createDataBinary));
    ns->defineFunction("disposeData", _SE(js_spineruntime_disposeData));
    ns->defineFunction("dataWidth", _SE(js_spineruntime_dataWidth));
    ns->defineFunction("dataHeight", _SE(js_spineruntime_dataHeight));
    ns->defineFunction("dataX", _SE(js_spineruntime_dataX));
    ns->defineFunction("dataY", _SE(js_spineruntime_dataY));
    ns->defineFunction("dataVersion", _SE(js_spineruntime_dataVersion));
    ns->defineFunction("dataAnimationCount", _SE(js_spineruntime_dataAnimationCount));
    ns->defineFunction("dataAnimationName", _SE(js_spineruntime_dataAnimationName));
    ns->defineFunction("dataHasAnimation", _SE(js_spineruntime_dataHasAnimation));
    ns->defineFunction("dataAnimationDuration", _SE(js_spineruntime_dataAnimationDuration));
    ns->defineFunction("dataSkinCount", _SE(js_spineruntime_dataSkinCount));
    ns->defineFunction("dataSkinName", _SE(js_spineruntime_dataSkinName));
    ns->defineFunction("dataHasSkin", _SE(js_spineruntime_dataHasSkin));
    ns->defineFunction("dataTexturePageCount", _SE(js_spineruntime_dataTexturePageCount));
    ns->defineFunction("dataTexturePage", _SE(js_spineruntime_dataTexturePage));

    ns->defineFunction("createRuntime", _SE(js_spineruntime_createRuntime));
    ns->defineFunction("disposeRuntime", _SE(js_spineruntime_disposeRuntime));
    ns->defineFunction("setRenderBuffer", _SE(js_spineruntime_setRenderBuffer));
    ns->defineFunction("getRenderDataBuffer", _SE(js_spineruntime_getRenderDataBuffer));
    ns->defineFunction("runtimePlay", _SE(js_spineruntime_runtimePlay));
    ns->defineFunction("runtimeSetAnimation", _SE(js_spineruntime_runtimeSetAnimation));
    ns->defineFunction("runtimeAddAnimation", _SE(js_spineruntime_runtimeAddAnimation));
    ns->defineFunction("runtimeClearTrack", _SE(js_spineruntime_runtimeClearTrack));
    ns->defineFunction("runtimeClearTracks", _SE(js_spineruntime_runtimeClearTracks));
    ns->defineFunction("runtimeGetCurrent", _SE(js_spineruntime_runtimeGetCurrent));
    ns->defineFunction("runtimeSetMix", _SE(js_spineruntime_runtimeSetMix));
    ns->defineFunction("runtimeSetToSetupPose", _SE(js_spineruntime_runtimeSetToSetupPose));
    ns->defineFunction("runtimeSetBonesToSetupPose", _SE(js_spineruntime_runtimeSetBonesToSetupPose));
    ns->defineFunction("runtimeSetSlotsToSetupPose", _SE(js_spineruntime_runtimeSetSlotsToSetupPose));
    ns->defineFunction("runtimeFindBone", _SE(js_spineruntime_runtimeFindBone));
    ns->defineFunction("runtimeFindSlotIndex", _SE(js_spineruntime_runtimeFindSlotIndex));
    ns->defineFunction("runtimeGetSlot", _SE(js_spineruntime_runtimeGetSlot));
    ns->defineFunction("runtimeSetAttachment", _SE(js_spineruntime_runtimeSetAttachment));
    ns->defineFunction("runtimeSetSlotColor", _SE(js_spineruntime_runtimeSetSlotColor));
    ns->defineFunction("runtimeGetAttachment", _SE(js_spineruntime_runtimeGetAttachment));
    ns->defineFunction("runtimeSetSlotTexture", _SE(js_spineruntime_runtimeSetSlotTexture));
    ns->defineFunction("runtimeResizeSlotRegion", _SE(js_spineruntime_runtimeResizeSlotRegion));
    ns->defineFunction("runtimeSetSlotsRange", _SE(js_spineruntime_runtimeSetSlotsRange));
    ns->defineFunction("runtimeSetEmptyAnimation", _SE(js_spineruntime_runtimeSetEmptyAnimation));
    ns->defineFunction("runtimeAddEmptyAnimation", _SE(js_spineruntime_runtimeAddEmptyAnimation));
    ns->defineFunction("runtimeSetEmptyAnimations", _SE(js_spineruntime_runtimeSetEmptyAnimations));
    ns->defineFunction("runtimeGetTrackInfo", _SE(js_spineruntime_runtimeGetTrackInfo));
    ns->defineFunction("runtimeSetTrackLoop", _SE(js_spineruntime_runtimeSetTrackLoop));
    ns->defineFunction("runtimeSetTrackReverse", _SE(js_spineruntime_runtimeSetTrackReverse));
    ns->defineFunction("runtimeSetTrackAdditive", _SE(js_spineruntime_runtimeSetTrackAdditive));
    ns->defineFunction("runtimeSetTrackDelay", _SE(js_spineruntime_runtimeSetTrackDelay));
    ns->defineFunction("runtimeSetTrackTime", _SE(js_spineruntime_runtimeSetTrackTime));
    ns->defineFunction("runtimeSetTrackEnd", _SE(js_spineruntime_runtimeSetTrackEnd));
    ns->defineFunction("runtimeSetTrackTimeScale", _SE(js_spineruntime_runtimeSetTrackTimeScale));
    ns->defineFunction("runtimeSetTrackAlpha", _SE(js_spineruntime_runtimeSetTrackAlpha));
    ns->defineFunction("runtimeSetTrackAnimationRange", _SE(js_spineruntime_runtimeSetTrackAnimationRange));
    ns->defineFunction("runtimeSetTrackMixDuration", _SE(js_spineruntime_runtimeSetTrackMixDuration));
    ns->defineFunction("runtimeSetTrackMixDuration3", _SE(js_spineruntime_runtimeSetTrackMixDuration3));
    ns->defineFunction("runtimeSetTrackMixTime", _SE(js_spineruntime_runtimeSetTrackMixTime));
    ns->defineFunction("runtimeSetTrackThresholds", _SE(js_spineruntime_runtimeSetTrackThresholds));
    ns->defineFunction("runtimeSetTrackShortestRotation", _SE(js_spineruntime_runtimeSetTrackShortestRotation));
    ns->defineFunction("runtimeSetTrackAlphaAttachmentThreshold", _SE(js_spineruntime_runtimeSetTrackAlphaAttachmentThreshold));
    ns->defineFunction("runtimeResetTrackRotationDirections", _SE(js_spineruntime_runtimeResetTrackRotationDirections));
    ns->defineFunction("runtimeGetBone", _SE(js_spineruntime_runtimeGetBone));
    ns->defineFunction("runtimeSetBoneLocal", _SE(js_spineruntime_runtimeSetBoneLocal));
    ns->defineFunction("runtimeBoneWorldToLocal", _SE(js_spineruntime_runtimeBoneWorldToLocal));
    ns->defineFunction("runtimeBoneLocalToWorld", _SE(js_spineruntime_runtimeBoneLocalToWorld));
    ns->defineFunction("runtimeSetSkin", _SE(js_spineruntime_runtimeSetSkin));
    ns->defineFunction("runtimeSetParams", _SE(js_spineruntime_runtimeSetParams));
    ns->defineFunction("runtimeSetPaused", _SE(js_spineruntime_runtimeSetPaused));
    ns->defineFunction("runtimeUpdateAnimation", _SE(js_spineruntime_runtimeUpdateAnimation));
    ns->defineFunction("runtimeUpdatePose", _SE(js_spineruntime_runtimeUpdatePose));
    ns->defineFunction("runtimeExtractRenderData", _SE(js_spineruntime_runtimeExtractRenderData));
    ns->defineFunction("runtimeUpdateRenderData", _SE(js_spineruntime_runtimeUpdateRenderData));
    ns->defineFunction("runtimeUpdateWorldTransform", _SE(js_spineruntime_runtimeUpdateWorldTransform));
    ns->defineFunction("runtimeUpdate", _SE(js_spineruntime_runtimeUpdate));
    ns->defineFunction("runtimeSetOutputTransform", _SE(js_spineruntime_runtimeSetOutputTransform));
    ns->defineFunction("runtimeRenderData", _SE(js_spineruntime_runtimeRenderData));
    ns->defineFunction("runtimeGetBoneByName", _SE(js_spineruntime_runtimeGetBoneByName));
    ns->defineFunction("runtimeGetCurrentAttachment", _SE(js_spineruntime_runtimeGetCurrentAttachment));
    ns->defineFunction("runtimeGetBounds", _SE(js_spineruntime_runtimeGetBounds));
    ns->defineFunction("runtimeSetEventListener", _SE(js_spineruntime_runtimeSetEventListener));
    ns->defineFunction("runtimeSetListener", _SE(js_spineruntime_runtimeSetListener));
    ns->defineFunction("runtimeFindAnimation", _SE(js_spineruntime_runtimeFindAnimation));
    ns->defineFunction("runtimeBoneCount", _SE(js_spineruntime_runtimeBoneCount));
    ns->defineFunction("runtimeBoneName", _SE(js_spineruntime_runtimeBoneName));
    ns->defineFunction("runtimeFindBoneIndex", _SE(js_spineruntime_runtimeFindBoneIndex));

    // Release JS callbacks and the render staging buffer at engine cleanup.
    se::ScriptEngine::getInstance()->addBeforeCleanupHook([] {
        clearAllCallbacks();
        releaseRenderBuffer();
    });

    se::ScriptEngine::getInstance()->clearException();
    return true;
}

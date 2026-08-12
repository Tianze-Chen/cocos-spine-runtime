// SpineRuntime realtime regression POC.
#include "../SpineRuntime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char kAtlas[] = R"atlas(tex.png
size: 64, 64
format: RGBA8888
filter: Linear, Linear
repeat: none
region1
	rotate: false
	xy: 0, 0
	size: 50, 50
	orig: 50, 50
	offset: 0, 0
	index: -1
)atlas";

const char kJson[] = R"json({
  "skeleton": { "spine": "4.3.00", "x": 0, "y": 0, "width": 50, "height": 50, "hash": "poc", "name": "poc" },
  "bones": [
    { "name": "root", "length": 30 },
    { "name": "child", "parent": "root", "x": 10, "length": 5 }
  ],
  "slots": [
    { "name": "slot1", "bone": "root", "attachment": "region1", "dark": "ff0080ff" }
  ],
  "skins": {
    "default": {
      "name": "default",
      "attachments": {
        "slot1": {
          "region1": { "name": "region1", "path": "region1", "width": 50, "height": 50 }
        }
      }
    },
    "alt": {
      "name": "alt",
      "attachments": {
        "slot1": {
          "region1": { "name": "region1", "path": "region1", "width": 20, "height": 10 }
        }
      }
    }
  },
  "events": {
    "boom": { "int": 7, "float": 1.5, "string": "setup", "audio": "beep.wav", "volume": 0.75, "balance": -0.25 },
    "empty": {}
  },
  "animations": {
    "spin": {
      "bones": {
        "root": {
          "rotate": [
            { "time": 0, "value": 0 },
            { "time": 1, "value": 90 },
            { "time": 2, "value": 180 }
          ]
        }
      },
      "events": [
        { "time": 0.25, "name": "boom", "int": 9, "float": 2.5, "string": "override", "volume": 0.5, "balance": 0.25 },
        { "time": 0.5, "name": "empty" }
      ]
    },
    "tilt": {
      "bones": {
        "child": {
          "rotate": [
            { "time": 0, "value": 0 },
            { "time": 1, "value": 30 }
          ]
        }
      }
    }
  }
})json";

const char kJsonMesh[] = R"json({
  "skeleton": { "spine": "4.3.00", "width": 50, "height": 50 },
  "bones": [ { "name": "root" } ],
  "slots": [ { "name": "meshSlot", "bone": "root", "attachment": "mesh1" } ],
  "skins": {
    "default": {
      "name": "default",
      "attachments": {
        "meshSlot": {
          "mesh1": {
            "type": "mesh", "path": "region1",
            "uvs": [0, 0, 1, 0, 1, 1, 0, 1],
            "triangles": [0, 1, 2, 2, 3, 0],
            "vertices": [-25, -25, 25, -25, 25, 25, -25, 25],
            "hull": 4, "width": 50, "height": 50
          }
        }
      }
    }
  },
  "animations": { "idle": {} }
})json";

const char kJsonClip[] = R"json({
  "skeleton": { "spine": "4.3.00", "width": 50, "height": 50 },
  "bones": [ { "name": "root" } ],
  "slots": [
    { "name": "clipSlot", "bone": "root", "attachment": "clip" },
    { "name": "slot1", "bone": "root", "attachment": "region1" }
  ],
  "skins": {
    "default": {
      "name": "default",
      "attachments": {
        "clipSlot": {
          "clip": { "type": "clipping", "vertexCount": 4, "vertices": [-10, -10, 10, -10, 10, 10, -10, 10] }
        },
        "slot1": {
          "region1": { "name": "region1", "path": "region1", "width": 50, "height": 50 }
        }
      }
    }
  },
  "animations": { "idle": {} }
})json";

int gFailures = 0;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        std::printf("  FAIL: %s (%s:%d)\n", message, __FILE__, __LINE__); \
        ++gFailures; \
    } \
} while (false)

#define REQUIRE(condition, message) do { \
    if (!(condition)) { \
        std::printf("  FAIL: %s (%s:%d)\n", message, __FILE__, __LINE__); \
        ++gFailures; \
        return false; \
    } \
} while (false)

bool near(float a, float b, float epsilon = 0.001F) {
    return std::fabs(a - b) <= epsilon;
}

spineruntime::Data* createData(const char* json = kJson, size_t len = 0, float scale = 1.0F) {
    const char* textures[] = {"tex.png"};
    spineruntime::Data* data = spineruntime::Data::create(json, len, false, kAtlas, textures, 1, scale);
    if (!data) std::printf("  Data::create error: %s\n", spineruntime::Data::lastError());
    return data;
}

bool renderExtents(const spineruntime::RenderData& data, float& width, float& height) {
    if (!data.vertices || data.vertexCount == 0 || data.vertexStrideBytes < 8) return false;
    const size_t stride = data.vertexStrideBytes / sizeof(float);
    float minX = data.vertices[0], maxX = data.vertices[0];
    float minY = data.vertices[1], maxY = data.vertices[1];
    for (uint32_t i = 1; i < data.vertexCount; ++i) {
        const float* vertex = data.vertices + i * stride;
        minX = std::min(minX, vertex[0]);
        maxX = std::max(maxX, vertex[0]);
        minY = std::min(minY, vertex[1]);
        maxY = std::max(maxY, vertex[1]);
    }
    width = maxX - minX;
    height = maxY - minY;
    return true;
}

bool testDataAndLifetime() {
    std::printf("[data/lifetime]\n");

    const char* wrongTextures[] = {"wrong.png"};
    spineruntime::Data* missing = spineruntime::Data::create(kJson, 0, false, kAtlas, wrongTextures, 1);
    CHECK(missing == nullptr, "missing atlas texture must fail");
    CHECK(std::strstr(spineruntime::Data::lastError(), "tex.png") != nullptr,
          "missing texture error should include its name");
    if (missing) missing->dispose();

    const unsigned char dummyBinary = 0;
    spineruntime::Data* badBinary = spineruntime::Data::create(&dummyBinary, 0, true, kAtlas, wrongTextures, 1);
    CHECK(badBinary == nullptr, "zero-length binary must fail");
    if (badBinary) badBinary->dispose();

    std::vector<char> jsonBuffer(kJson, kJson + std::strlen(kJson));
    spineruntime::Data* scaled = createData(jsonBuffer.data(), jsonBuffer.size(), 0.5F);
    REQUIRE(scaled, "explicit-length JSON should parse");
    CHECK(near(scaled->width(), 50.0F) && near(scaled->height(), 50.0F),
          "Spine skeleton metadata remains in export-space units");
    CHECK(std::strcmp(scaled->version(), "4.3.00") == 0, "skeleton version query");
    CHECK(scaled->animationCount() == 2, "animation enumeration");
    CHECK(scaled->hasAnimation("spin") && near(scaled->animationDuration("spin"), 2.0F),
          "animation lookup and duration");
    CHECK(!scaled->hasAnimation("missing"), "missing animation lookup");
    CHECK(scaled->skinCount() == 2 && scaled->hasSkin("alt"), "skin enumeration");
    CHECK(scaled->animationName(-1) == nullptr && scaled->skinName(99) == nullptr,
          "out-of-range metadata queries");
    CHECK(scaled->texturePageCount() == 1, "atlas page count");
    spineruntime::TexturePageInfo page;
    CHECK(scaled->texturePage(0, &page), "atlas page query");
    CHECK(page.textureId == 0 && page.width == 64 && page.height == 64,
          "atlas page texture mapping and dimensions");
    spineruntime::Runtime* scaledRuntime = spineruntime::Runtime::create(scaled);
    REQUIRE(scaledRuntime, "create scaled runtime");
    scaledRuntime->updateRenderData();
    float scaledWidth = 0.0F, scaledHeight = 0.0F;
    REQUIRE(renderExtents(scaledRuntime->renderData(), scaledWidth, scaledHeight),
            "scaled attachment render");
    CHECK(near(scaledWidth, 25.0F) && near(scaledHeight, 25.0F),
          "parser scale affects rendered geometry");
    scaledRuntime->dispose();
    scaled->dispose();

    // Runtime must retain all atlas/skeleton resources after the caller drops Data.
    spineruntime::Data* retained = createData();
    REQUIRE(retained, "create retained data");
    spineruntime::Runtime* runtime = spineruntime::Runtime::create(retained);
    REQUIRE(runtime, "create runtime for lifetime test");
    retained->dispose();
    retained = nullptr;
    CHECK(runtime->setAnimation(0, "spin", true) != spineruntime::Runtime::INVALID_TRACK,
          "animation works after Data::dispose");
    runtime->update(1.0F / 60.0F);
    CHECK(runtime->renderData().vertexCount == 4, "render works after Data::dispose");
    spineruntime::Runtime::BoneInfo animatedRoot;
    CHECK(runtime->getBone("root", &animatedRoot) && std::fabs(animatedRoot.rotation) > 0.1F,
          "animation state updates the skeleton pose");
    runtime->dispose();
    return true;
}

struct CapturedEvent {
    int type = -1;
    spineruntime::Runtime::TrackHandle track = spineruntime::Runtime::INVALID_TRACK;
    int trackIndex = -1;
    std::string animation;
    std::string eventName;
    std::string stringValue;
    std::string audioPath;
    float eventTime = 0.0F;
    int intValue = 0;
    float floatValue = 0.0F;
    float volume = 0.0F;
    float balance = 0.0F;
};

struct EventCapture {
    spineruntime::Runtime* runtime = nullptr;
    std::vector<CapturedEvent> events;
    bool disposeInfoReadable = false;
    std::string disposeAnimation;
};

void captureEvent(void* user, const spineruntime::Runtime::EventInfo* event) {
    EventCapture& capture = *static_cast<EventCapture*>(user);
    CapturedEvent value;
    value.type = event->type;
    value.track = event->track;
    value.trackIndex = event->trackIndex;
    value.animation = event->animationName ? event->animationName : "";
    value.eventName = event->eventName ? event->eventName : "";
    value.stringValue = event->stringValue ? event->stringValue : "";
    value.audioPath = event->audioPath ? event->audioPath : "";
    value.eventTime = event->eventTime;
    value.intValue = event->intValue;
    value.floatValue = event->floatValue;
    value.volume = event->volume;
    value.balance = event->balance;
    capture.events.push_back(value);

    if (event->type == spineruntime::Runtime::DISPOSE) {
        spineruntime::Runtime::TrackInfo info;
        capture.disposeInfoReadable = capture.runtime->getTrackInfo(event->track, &info);
        if (capture.disposeInfoReadable && info.animationName) capture.disposeAnimation = info.animationName;
    }
}

int eventIndex(const EventCapture& capture, int type) {
    for (size_t i = 0; i < capture.events.size(); ++i) {
        if (capture.events[i].type == type) return static_cast<int>(i);
    }
    return -1;
}

int namedEventIndex(const EventCapture& capture, const char* name) {
    for (size_t i = 0; i < capture.events.size(); ++i) {
        if (capture.events[i].type == spineruntime::Runtime::EVENT &&
            capture.events[i].eventName == name) return static_cast<int>(i);
    }
    return -1;
}

bool testEventsAndTracks() {
    std::printf("[events/tracks]\n");
    CHECK(spineruntime::Runtime::DISPOSE == 3 && spineruntime::Runtime::COMPLETE == 4,
          "event enum values must match Spine/Cocos");

    spineruntime::Data* data = createData();
    REQUIRE(data, "create event data");
    spineruntime::Runtime* runtime = spineruntime::Runtime::create(data);
    REQUIRE(runtime, "create event runtime");

    EventCapture capture;
    capture.runtime = runtime;
    runtime->setEventListener(captureEvent, &capture);
    const auto handle = runtime->setAnimation(0, "spin", false);
    REQUIRE(handle != spineruntime::Runtime::INVALID_TRACK, "set event animation");
    runtime->updateAnimation(0.30F);
    runtime->updateAnimation(1.80F);
    runtime->clearTrack(0);

    const int start = eventIndex(capture, spineruntime::Runtime::START);
    const int custom = eventIndex(capture, spineruntime::Runtime::EVENT);
    const int complete = eventIndex(capture, spineruntime::Runtime::COMPLETE);
    const int end = eventIndex(capture, spineruntime::Runtime::END);
    const int dispose = eventIndex(capture, spineruntime::Runtime::DISPOSE);
    CHECK(start >= 0 && custom > start && complete > custom && end > complete && dispose > end,
          "START/EVENT/COMPLETE/END/DISPOSE order");
    if (custom >= 0) {
        const CapturedEvent& event = capture.events[static_cast<size_t>(custom)];
        CHECK(event.track == handle && event.trackIndex == 0 && event.animation == "spin",
              "custom event track identity");
        CHECK(event.eventName == "boom" && near(event.eventTime, 0.25F), "custom event name/time");
        CHECK(event.intValue == 9 && near(event.floatValue, 2.5F) && event.stringValue == "override",
              "custom event scalar payload");
        CHECK(event.audioPath == "beep.wav" && near(event.volume, 0.5F) && near(event.balance, 0.25F),
              "custom event audio payload");
    }
    const int emptyEvent = namedEventIndex(capture, "empty");
    CHECK(emptyEvent > custom, "event with omitted string/audio payload is delivered");
    if (emptyEvent >= 0) {
        const CapturedEvent& event = capture.events[static_cast<size_t>(emptyEvent)];
        CHECK(event.stringValue.empty() && event.audioPath.empty(),
              "omitted event string/audio payload stays empty");
    }
    CHECK(capture.disposeInfoReadable && capture.disposeAnimation == "spin",
          "track snapshot must remain readable during DISPOSE callback");
    spineruntime::Runtime::TrackInfo staleInfo;
    CHECK(!runtime->getTrackInfo(handle, &staleInfo), "track handle must expire after DISPOSE callback");
    CHECK(!runtime->setTrackAlpha(handle, 0.5F), "expired track handle must reject mutations");

    runtime->setEventListener(nullptr, nullptr);
    runtime->setMix("spin", "tilt", 0.2F);
    const auto track0 = runtime->setAnimation(0, "spin", true);
    const auto queued = runtime->addAnimation(0, "tilt", false, 0.1F);
    const auto track1 = runtime->setAnimation(1, "tilt", true);
    REQUIRE(track0 && queued && track1, "multi-track and queued animations");
    CHECK(track0 != queued && track0 != track1 && queued != track1, "track handles must be unique");
    CHECK(runtime->getCurrent(0) == track0 && runtime->getCurrent(1) == track1,
          "getCurrent for multiple tracks");
    CHECK(runtime->setTrackLoop(track0, false), "set track loop");
    CHECK(runtime->setTrackReverse(track0, true), "set track reverse");
    CHECK(runtime->setTrackAdditive(track0, true), "set track additive");
    CHECK(runtime->setTrackShortestRotation(track0, true), "set shortest rotation");
    CHECK(runtime->setTrackDelay(track0, 0.2F), "set track delay");
    CHECK(runtime->setTrackTime(track0, 0.4F), "set track time");
    CHECK(runtime->setTrackEnd(track0, 3.0F), "set track end");
    CHECK(runtime->setTrackTimeScale(track0, 1.5F), "set track time scale");
    CHECK(!runtime->setTrackTimeScale(track0, -1.0F), "negative track time scale rejected");
    CHECK(runtime->setTrackAlpha(track0, 0.6F), "set track alpha");
    CHECK(runtime->setTrackAnimationRange(track0, 0.1F, 1.5F, 0.1F), "set animation range");
    CHECK(runtime->setTrackMixTime(track0, 0.12F), "set mix time");
    CHECK(runtime->setTrackMixDuration(track0, 0.3F), "set mix duration");
    CHECK(runtime->setTrackThresholds(track0, 0.2F, 0.3F, 0.4F), "set mix thresholds");
    CHECK(runtime->setTrackAlphaAttachmentThreshold(track0, 0.5F),
          "set alpha attachment threshold");
    CHECK(runtime->resetTrackRotationDirections(track0), "reset rotation directions");
    CHECK(runtime->setTrackMixDuration(queued, 0.25F, 0.05F),
          "set queued mix duration with delay adjustment");

    spineruntime::Runtime::TrackInfo info;
    REQUIRE(runtime->getTrackInfo(track0, &info), "get track info");
    CHECK(info.trackIndex == 0 && info.animationName && std::strcmp(info.animationName, "spin") == 0,
          "track identity query");
    CHECK(!info.loop && info.reverse && info.additive && info.shortestRotation && info.wasApplied,
          "track boolean query");
    CHECK(info.next == queued && info.mixingTo == spineruntime::Runtime::INVALID_TRACK,
          "queued/mixing track relationships");
    CHECK(near(info.delay, 0.2F) && near(info.trackTime, 0.4F) && near(info.trackEnd, 3.0F),
          "track timing query");
    CHECK(near(info.animationStart, 0.1F) && near(info.animationEnd, 1.5F) &&
          near(info.timeScale, 1.5F) && near(info.alpha, 0.6F) && near(info.mixTime, 0.12F),
          "track range/rate/alpha query");
    CHECK(near(info.eventThreshold, 0.2F) && near(info.mixAttachmentThreshold, 0.3F) &&
          near(info.alphaAttachmentThreshold, 0.5F) && near(info.mixDrawOrderThreshold, 0.4F),
          "track threshold query");

    spineruntime::Runtime* other = spineruntime::Runtime::create(data);
    REQUIRE(other, "create second runtime");
    const auto otherTrack = other->setAnimation(0, "spin", true);
    CHECK(otherTrack != track0, "handles must be unique across runtimes");
    CHECK(!other->setTrackAlpha(track0, 0.1F), "foreign runtime must reject a track handle");
    other->dispose();

    runtime->clearTracks();
    CHECK(runtime->getCurrent(0) == spineruntime::Runtime::INVALID_TRACK, "clearTracks clears current entry");
    CHECK(!runtime->setTrackLoop(track0, true) && !runtime->setTrackLoop(queued, true),
          "clearTracks invalidates current and queued handles");
    const auto empty = runtime->setEmptyAnimation(0, 0.1F);
    const auto queuedEmpty = runtime->addEmptyAnimation(0, 0.1F, 0.0F);
    CHECK(empty && queuedEmpty, "empty animation controls");
    REQUIRE(runtime->getTrackInfo(empty, &info), "empty track info");
    CHECK(info.emptyAnimation && info.next == queuedEmpty, "empty animation metadata");
    runtime->setEmptyAnimations(0.05F);
    runtime->clearTracks();

    runtime->dispose();
    data->dispose();
    return true;
}

bool testPoseSlotsAndRendering() {
    std::printf("[pose/slots/render]\n");
    spineruntime::Data* data = createData();
    REQUIRE(data, "create pose data");
    spineruntime::Runtime* runtime = spineruntime::Runtime::create(data);
    REQUIRE(runtime, "create pose runtime");

    CHECK(runtime->renderData().vertexCount == 0, "render buffers start empty");
    CHECK(runtime->setBoneLocal("root", 2.0F, 3.0F, 0.0F, 1.0F, 1.0F, 0.0F, 0.0F),
          "prepare culled pose update");
    runtime->updatePose(0.0F);
    spineruntime::Runtime::BoneInfo culledRoot;
    CHECK(runtime->getBone("root", &culledRoot) && near(culledRoot.worldX, 2.0F) &&
          near(culledRoot.worldY, -3.0F), "pose-only update refreshes world transforms");
    CHECK(runtime->renderData().vertexCount == 0, "pose-only update does not extract geometry");
    runtime->setBonesToSetupPose();
    runtime->updateWorldTransform();
    runtime->extractRenderData();
    const spineruntime::RenderData& initial = runtime->renderData();
    REQUIRE(initial.vertexCount == 4 && initial.indexCount == 6 && initial.segmentCount == 1,
            "initial region render data");
    CHECK(initial.vertexStrideBytes == 24 && initial.shapeCount == 1,
          "one-color stride and debug shape");
    CHECK(initial.shapes[0].type == spineruntime::RenderData::SHAPE_REGION &&
          initial.shapes[0].slotIndex == 0, "region debug shape metadata");
    for (uint32_t i = 0; i < initial.indexCount; ++i) {
        CHECK(initial.indices[i] < initial.vertexCount, "region index range");
    }

    float width = 0.0F, height = 0.0F;
    REQUIRE(renderExtents(initial, width, height), "initial render extents");
    CHECK(near(width, 50.0F) && near(height, 50.0F), "initial region size");
    float bounds[4] = {};
    CHECK(runtime->getBounds(bounds) && near(bounds[2], 50.0F) && near(bounds[3], 50.0F),
          "skeleton bounds");

    CHECK(runtime->boneCount() == 2 && runtime->findBoneIndex("child") == 1,
          "bone count and lookup");
    CHECK(runtime->boneName(0) && std::strcmp(runtime->boneName(0), "root") == 0,
          "bone name query");
    spineruntime::Runtime::BoneInfo root;
    REQUIRE(runtime->getBone("root", &root), "root bone info");
    CHECK(root.index == 0 && root.parentIndex == -1 && near(root.length, 30.0F),
          "bone metadata including debug length");
    CHECK(runtime->setBoneLocal("root", 3.0F, 4.0F, 0.0F, 1.0F, 1.0F, 0.0F, 0.0F),
          "set local bone transform");
    runtime->updateWorldTransform();
    REQUIRE(runtime->getBone(0, &root), "updated root bone info");
    CHECK(near(root.worldX, 3.0F) && near(root.worldY, -4.0F),
          "local bone update reaches the Cocos y-down world pose");
    float world[2] = {}, local[2] = {};
    CHECK(runtime->boneLocalToWorld("root", 2.0F, 5.0F, world), "bone local-to-world");
    CHECK(runtime->boneWorldToLocal("root", world[0], world[1], local), "bone world-to-local");
    CHECK(near(local[0], 2.0F) && near(local[1], 5.0F), "bone coordinate round trip");
    runtime->setBonesToSetupPose();
    runtime->updateWorldTransform();

    CHECK(runtime->slotCount() == 1 && runtime->findSlotIndex("slot1") == 0,
          "slot count and lookup");
    CHECK(runtime->slotName(0) && std::strcmp(runtime->slotName(0), "slot1") == 0,
          "slot name query");
    spineruntime::Runtime::SlotInfo slot;
    REQUIRE(runtime->getSlot("slot1", &slot), "slot info");
    CHECK(slot.attachmentName && std::strcmp(slot.attachmentName, "region1") == 0,
          "slot attachment query");
    CHECK(slot.hasDarkColor && near(slot.darkR, 1.0F) && near(slot.darkB, 128.0F / 255.0F, 0.01F),
          "slot dark color query");
    spineruntime::Runtime::AttachmentInfo attachment;
    CHECK(runtime->getAttachment("slot1", "region1", &attachment), "named attachment query");
    CHECK(attachment.type == spineruntime::Runtime::ATTACHMENT_REGION &&
          attachment.path && std::strcmp(attachment.path, "region1") == 0 &&
          attachment.worldVerticesLength == 8 && attachment.hasTexture,
          "region attachment metadata");
    CHECK(!runtime->getAttachment("slot1", "missing", &attachment), "missing attachment query");
    CHECK(runtime->setSlotColor("slot1", 0.2F, 0.3F, 0.4F, 0.5F), "set slot color");
    REQUIRE(runtime->getSlot(0, &slot), "updated slot info");
    CHECK(near(slot.colorR, 0.2F) && near(slot.colorA, 0.5F), "slot color query");

    const size_t required = runtime->computeSlotWorldVertices("slot1", nullptr, 0, 1, 3);
    CHECK(required == 12, "world vertex required size with offset/stride");
    std::vector<float> positions(required, -999.0F);
    CHECK(runtime->computeSlotWorldVertices("slot1", positions.data(), positions.size(), 1, 3) == required,
          "compute slot world vertices");
    CHECK(positions[0] == -999.0F, "world vertex offset is preserved");

    spineruntime::Runtime::Params tint;
    tint.useTint = true;
    runtime->setParams(tint);
    runtime->updateRenderData();
    const spineruntime::RenderData& tinted = runtime->renderData();
    REQUIRE(tinted.vertexCount == 4 && tinted.vertexStrideBytes == 28, "two-color render stride");
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(tinted.vertices);
    CHECK(bytes[24] != 0 || bytes[25] != 0 || bytes[26] != 0, "two-color dark channel");

    tint.useTint = false;
    runtime->setParams(tint);
    CHECK(runtime->resizeSlotRegion("slot1", 20.0F, 10.0F, true), "resize copied slot attachment");
    CHECK(runtime->setSlotTexture("slot1", 77), "set slot texture ID");
    runtime->updateRenderData();
    const spineruntime::RenderData& resized = runtime->renderData();
    REQUIRE(resized.segmentCount == 1 && renderExtents(resized, width, height), "resized slot render");
    CHECK(resized.segments[0].textureId == 77, "slot texture ID reaches render segment");
    REQUIRE(runtime->getCurrentAttachment("slot1", &attachment), "current overridden attachment query");
    CHECK(attachment.textureId == 77 && near(attachment.width, 20.0F) && near(attachment.height, 10.0F),
          "current attachment reflects local texture/geometry override");
    CHECK(near(width, 20.0F) && near(height, 10.0F), "resized attachment geometry");
    CHECK(runtime->getBounds(bounds) && near(bounds[2], 20.0F) && near(bounds[3], 10.0F),
          "bounds follow copied slot attachment");
    CHECK(runtime->clearSlotTexture("slot1"), "clear slot texture override");
    runtime->updateRenderData();
    REQUIRE(renderExtents(runtime->renderData(), width, height), "restored slot render");
    CHECK(runtime->renderData().segments[0].textureId == 0 && near(width, 50.0F) && near(height, 50.0F),
          "clear restores original texture and attachment");

    CHECK(runtime->setSlotTexture("slot1", 88), "texture-only slot override");
    runtime->updateRenderData();
    CHECK(runtime->renderData().segments[0].textureId == 88, "texture-only override reaches segment");
    CHECK(runtime->clearSlotTexture("slot1"), "clear texture-only override");

    CHECK(runtime->setAttachment("slot1", nullptr), "clear slot attachment");
    runtime->updateRenderData();
    CHECK(runtime->renderData().vertexCount == 0, "cleared attachment is not rendered");
    CHECK(runtime->setAttachment("slot1", "region1"), "restore slot attachment");
    runtime->updateRenderData();
    CHECK(runtime->renderData().vertexCount == 4, "restored attachment renders");

    CHECK(runtime->setSkin("alt"), "set alternate skin");
    runtime->updateRenderData();
    REQUIRE(renderExtents(runtime->renderData(), width, height), "alternate skin render");
    CHECK(near(width, 20.0F) && near(height, 10.0F), "alternate skin attachment is applied");
    CHECK(!runtime->setSkin("missing"), "missing skin rejected");
    CHECK(runtime->setSkin("default"), "restore default skin");

    runtime->setSlotsRange(0, -1);
    runtime->updateRenderData();
    CHECK(runtime->renderData().vertexCount == 4, "inclusive start slot range");
    runtime->setSlotsRange(-1, 0);
    runtime->updateRenderData();
    CHECK(runtime->renderData().vertexCount == 0, "exclusive end slot range");
    runtime->setSlotsRange(-1, -1);

    spineruntime::Runtime::Params speed;
    speed.timeScale = 2.0F;
    runtime->setParams(speed);
    const float time0 = runtime->skeletonTime();
    runtime->updateAnimation(0.25F);
    CHECK(near(runtime->skeletonTime() - time0, 0.5F), "skeleton time advances with time scale");
    runtime->setPaused(true);
    const float pausedTime = runtime->skeletonTime();
    runtime->updateAnimation(1.0F);
    CHECK(near(runtime->skeletonTime(), pausedTime), "paused runtime freezes animation and physics time");
    runtime->setPaused(false);
    CHECK(!runtime->paused(), "resume runtime");
    runtime->setSkeletonTransform(10.0F, 20.0F, 2.0F, 3.0F);
    runtime->setPhysics(1.0F, 2.0F, 3.0F, 4.0F);
    runtime->resetPhysics();
    runtime->updateWorldTransform(spineruntime::Runtime::PHYSICS_POSE);
    runtime->physicsTranslate(1.0F, 2.0F);
    runtime->physicsRotate(0.0F, 0.0F, 15.0F);
    runtime->updateWorldTransform();
    REQUIRE(runtime->getBone(0, &root), "transformed root query");
    CHECK(std::isfinite(root.worldX) && std::isfinite(root.worldY), "skeleton/physics transform API");
    runtime->setToSetupPose();
    runtime->setSlotsToSetupPose();

    runtime->dispose();
    data->dispose();
    return true;
}

bool testMeshUVs() {
    std::printf("[mesh UVs]\n");
    spineruntime::Data* data = createData(kJsonMesh);
    REQUIRE(data, "create mesh data");
    spineruntime::Runtime* runtime = spineruntime::Runtime::create(data);
    REQUIRE(runtime, "create mesh runtime");
    runtime->updateRenderData();
    const spineruntime::RenderData& render = runtime->renderData();
    REQUIRE(render.vertexCount == 4 && render.indexCount == 6 && render.shapeCount == 1,
            "mesh render counts");
    CHECK(render.shapes[0].type == spineruntime::RenderData::SHAPE_MESH, "mesh debug shape type");
    spineruntime::Runtime::AttachmentInfo attachment;
    REQUIRE(runtime->getCurrentAttachment("meshSlot", &attachment), "mesh attachment query");
    CHECK(attachment.type == spineruntime::Runtime::ATTACHMENT_MESH &&
          attachment.worldVerticesLength == 8 && attachment.hasTexture,
          "mesh attachment metadata");
    float maxUV = 0.0F;
    const size_t stride = render.vertexStrideBytes / sizeof(float);
    for (uint32_t i = 0; i < render.vertexCount; ++i) {
        const float* vertex = render.vertices + i * stride;
        maxUV = std::max(maxUV, std::max(vertex[3], vertex[4]));
    }
    CHECK(maxUV > 0.7F && maxUV < 0.9F,
          "mesh must use atlas-adjusted UVs instead of raw 0..1 region UVs");
    for (uint32_t i = 0; i < render.indexCount; ++i) {
        CHECK(render.indices[i] < render.vertexCount, "mesh index range");
    }
    runtime->dispose();
    data->dispose();
    return true;
}

bool testSharedSlotResize() {
    std::printf("[shared slot resize]\n");
    spineruntime::Data* data = createData();
    REQUIRE(data, "create shared slot data");
    spineruntime::Runtime* first = spineruntime::Runtime::create(data);
    spineruntime::Runtime* second = spineruntime::Runtime::create(data);
    REQUIRE(first && second, "create shared runtimes");

    CHECK(first->resizeSlotRegion("slot1", 30.0F, 12.0F, false),
          "resize shared attachment");
    CHECK(first->setSlotTexture("slot1", 123), "set local texture on shared attachment");
    first->updateRenderData();
    second->updateRenderData();
    float firstWidth = 0.0F, firstHeight = 0.0F;
    float secondWidth = 0.0F, secondHeight = 0.0F;
    REQUIRE(renderExtents(first->renderData(), firstWidth, firstHeight) &&
            renderExtents(second->renderData(), secondWidth, secondHeight),
            "shared resize render data");
    CHECK(near(firstWidth, 30.0F) && near(firstHeight, 12.0F) &&
          near(secondWidth, 30.0F) && near(secondHeight, 12.0F),
          "createNew=false updates all runtimes sharing Data");
    CHECK(first->renderData().segments[0].textureId == 123 &&
          second->renderData().segments[0].textureId == 0,
          "texture IDs remain runtime-local");
    CHECK(first->clearSlotTexture("slot1"), "clear shared slot texture override");
    first->updateRenderData();
    REQUIRE(renderExtents(first->renderData(), firstWidth, firstHeight),
            "shared attachment after clear");
    CHECK(near(firstWidth, 30.0F) && near(firstHeight, 12.0F),
          "clearing texture does not undo a shared attachment resize");

    first->dispose();
    second->dispose();
    data->dispose();
    return true;
}

bool testClippingReset() {
    std::printf("[clipping]\n");
    spineruntime::Data* data = createData(kJsonClip);
    REQUIRE(data, "create clipping data");
    spineruntime::Runtime* runtime = spineruntime::Runtime::create(data);
    REQUIRE(runtime, "create clipping runtime");
    runtime->updateRenderData();
    const spineruntime::RenderData& clipped = runtime->renderData();
    REQUIRE(clipped.vertexCount > 4 && clipped.indexCount > 0, "clipped polygon output");
    const uint32_t clippedVertexCount = clipped.vertexCount;
    for (uint32_t i = 0; i < clipped.indexCount; ++i) {
        CHECK(clipped.indices[i] < clipped.vertexCount, "clipped index range");
    }

    CHECK(runtime->setAttachment("clipSlot", nullptr), "remove clipping attachment");
    runtime->updateRenderData();
    const spineruntime::RenderData& unclipped = runtime->renderData();
    CHECK(unclipped.vertexCount == 4 && unclipped.indexCount == 6,
          "clipper state must be cleared between render extractions");
    std::printf("  clipped vertices: %u -> %u\n", clippedVertexCount, unclipped.vertexCount);

    runtime->dispose();
    data->dispose();
    return true;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== SpineRuntime Realtime Regression ===\n");
    testDataAndLifetime();
    testEventsAndTracks();
    testPoseSlotsAndRendering();
    testMeshUVs();
    testSharedSlotResize();
    testClippingReset();
    if (gFailures != 0) {
        std::printf("=== POC FAIL: %d check(s) ===\n", gFailures);
        return 1;
    }
    std::printf("=== POC PASS ===\n");
    return 0;
}

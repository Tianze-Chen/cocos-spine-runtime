// SpineRuntime Realtime 压测
// 用途：量化「N 个骨架纯 realtime」的每帧 CPU 开销，为 cache 模式去留提供数据。
// 用法：N = 10 / 30 / 50 / 100，三档模型。
#include "../SpineRuntime.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static bool readFile(const char* path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

struct ModelSpec {
    const char* name;
    const char* jsonPath;
    const char* atlasPath;
    const char* const* texNames;
    int nTex;
    const char* anim;
};

static const char* kSpineboyTex[] = { "spineboy.png" };
static const char* kRaptorTex[] = { "raptor.png" };
static const char* kDragonTex[] = { "dragon.png", "dragon_2.png", "dragon_3.png",
                                    "dragon_4.png", "dragon_5.png", "dragon_6.png" };

static const ModelSpec kModels[] = {
    { "spineboy", "poc/assets/spineboy-pro.json", "poc/assets/spineboy.atlas",
      kSpineboyTex, 1, "idle" },
    { "raptor",   "poc/assets/raptor-pro.json",   "poc/assets/raptor.atlas",
      kRaptorTex, 1, "walk" },
    { "dragon",   "poc/assets/dragon-ess.json",   "poc/assets/dragon.atlas",
      kDragonTex, 6, "flying" },
};

static const int kNValues[] = { 10, 30, 50, 100 };
static const int kWarmupFrames = 30;
static const int kMeasuredFrames = 300;

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== SpineRuntime Realtime Stress Test ===\n");
    std::printf("warmup=%d, measured=%d frames\n\n", kWarmupFrames, kMeasuredFrames);

    for (const ModelSpec& spec : kModels) {
        std::string json, atlas;
        if (!readFile(spec.jsonPath, json) || !readFile(spec.atlasPath, atlas)) {
            std::printf("[%s] SKIP: cannot read assets\n", spec.name);
            continue;
        }

        spineruntime::Data* data = spineruntime::Data::create(
            json.c_str(), json.size(), false, atlas.c_str(), spec.texNames, spec.nTex);
        if (!data) {
            std::printf("[%s] SKIP: Data::create failed\n", spec.name);
            continue;
        }

        std::printf("[%s] data %.0fx%.0f, anim=%s\n",
                    spec.name, data->width(), data->height(), spec.anim);

        for (int n : kNValues) {
            // 建 N 个实例
            std::vector<spineruntime::Runtime*> rts;
            rts.reserve(n);
            bool ok = true;
            for (int i = 0; i < n; ++i) {
                spineruntime::Runtime* rt = spineruntime::Runtime::create(data);
                if (!rt) { ok = false; break; }
                rt->play(spec.anim, true);
                rts.push_back(rt);
            }
            if (!ok) {
                std::printf("  N=%3d: FAIL (create)\n", n);
                for (auto* r : rts) r->dispose();
                continue;
            }

            // warmup
            for (int f = 0; f < kWarmupFrames; ++f) {
                for (auto* rt : rts) rt->update(1.0f / 60.0f);
            }

            // measured
            using clock = std::chrono::high_resolution_clock;
            const float dt = 1.0f / 60.0f;
            auto t0 = clock::now();
            for (int f = 0; f < kMeasuredFrames; ++f) {
                for (auto* rt : rts) rt->update(dt);
            }
            auto t1 = clock::now();
            double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            double perFrameMs = totalMs / kMeasuredFrames;
            double perSkelUs = perFrameMs * 1000.0 / n;

            std::printf("  N=%3d: %7.3f ms/frame  (%6.1f us/skeleton)\n",
                        n, perFrameMs, perSkelUs);

            for (auto* rt : rts) rt->dispose();
        }
        std::printf("\n");
        data->dispose();
    }

    std::printf("=== Stress Test Done ===\n");
    return 0;
}

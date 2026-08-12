# sp.spine —— Spine 4.3 骨骼动画运行时 for Cocos Creator

> 一个给 Cocos Creator 3.8 用的 Spine 4.3 运行时插件。用统一的 C++ 门面驱动官方 spine-cpp 4.3,Web/小游戏走外置 wasm、原生包和 Native Simulator 走 JSB,以引擎的 `cc.UIMesh` 消费渲染。一句话:**把 4.3 的完整骨骼动画能力,以接近原生的性能接入 Cocos Creator。**

---

## 一、这是什么

Cocos Creator 3.8 内置的 Spine 运行时是 3.8 版本,落后于最新的 Spine 4.x。动画师用新版 Spine 导出的资源无法直接使用,而社区方案要么只支持 Web、要么和引擎内置 Spine 冲突。

`sp.spine` 解决了这两件事:

- **支持 Spine 4.3**(官方 spine-cpp 4.3 运行时),JSON 骨骼资源拖进去就能跑;
- **WASM / JSB 双后端同构**,同一份 TypeScript 组件层;非原生环境统一加载外置 wasm,原生环境严格使用 JSB,不做 wasm 回退。

---

## 二、架构一览

```
spine-cpp 4.3 (官方运行时)
        │
        ▼
SpineRuntime 门面 (C++ 不透明句柄 + 自由函数,不泄露 spine 类型)
        │
        ├── embind 绑定 ──► wasm (Web / 编辑器与浏览器预览 / 小游戏)
        └── JSB 手动绑定 ──► 原生 (Android / iOS / 桌面 / Native Simulator)
        │
        ▼
Spine 组件 (extends UIMesh) + SpineData 资源 + spine-ts 兼容包装 (TS)
        │
        ▼
cc.UIMesh ──► 引擎 2D 管线 (buffer / 合批 / 提交)
```

**关键设计:一个门面,两个后端,零业务差异。** 所有平台差异被收敛到「绑定加载」和「堆内存来源」两个点,组件层完全无感。

---

## 三、核心亮点

### 1. 统一 C++ 门面 + 双后端,一份代码两种原生加速

对外只有 `SpineRuntime.h`(不透明句柄 + 顶层自由函数,头文件只 `include` 基础类型,**不泄露任何 spine-cpp 类型**)。

- **非原生**:Emscripten + embind 编成 wasm,通过引擎导出的 `cc.wasm` 加载独立 `.wasm` 文件;
- **原生**:JSB 手动绑定,引擎启动时注册成 `globalThis.spineruntime`;绑定不存在就报错,不会回退到 wasm。

两边共享同一份 TypeScript 组件层。要加一个 API,只需要在门面、两个绑定、一个接口声明里各加一次,业务代码无感。

### 2. 高性能渲染:变换烘焙进顶点,交给引擎合批

每帧由 C++ 完成骨骼计算后:

1. 把**节点世界变换 + y 翻转**直接烘焙进每个顶点(不逐顶点做 JS 矩阵乘法);
2. 按 `(贴图, 混合模式)` 分组输出 segment;
3. 交给 `cc.UIMesh.setMeshData(...)`,引擎负责 buffer 上传、多实例合批、提交。

结果:合批不需要 per-node transform uniform,同贴图同材质的多个 Spine 实例可以自然合进一次提交。渲染路径里没有 JS 侧的逐顶点开销。

### 3. 双 Spine 版本共存(符号隔离)

引擎内置 Spine 3.8 和 spine-cpp 4.3 都在 `spine::` 命名空间下,静态链接时符号会互相顶掉。本插件用 `COMPILE_DEFINITIONS "spine=spine43"` 把 4.3 的全部符号编译进 `spine43::`,两者在最终可执行文件里**互不冲突**——已用 `llvm-nm` 在 Android 产物上验证无重复符号。

这意味着:一个工程里,既可以用引擎自带的 3.8,也可以同时用本插件的 4.3,不打架。

### 4. 预编译 wasm + vendored C++ 源码

- **Web/小游戏端零编译**:`spine-runtime.wasm` 作为独立预编译文件随插件分发,CJS glue 统一通过 `cc.wasm.instantiateWasm` 加载;编辑器启动和发布构建钩子负责把二进制放到引擎约定的位置;
- **原生端零手动步骤**:spine-cpp 4.3 源码 **vendor 进仓库**(`native/third_party/`,1.7MB / 214 个文件),出原生包时引擎构建系统自动编译——**没有 `git submodule`、没有「clone 后空目录导致编译失败」的坑**。

插件依赖带有 `cc.UIMesh` 和公开 `cc.wasm` 导出的自定义引擎。普通原生工程会自动扫描插件;stock Native Simulator 例外,需要使用仓库提供的 CMake hook 重建一次。

### 5. 完整编辑器集成

- **资源导入器**:`.json`(+ 同名 `.atlas` + `.png`)自动识别为 `sp.spineData`,解析纹理页、建立贴图依赖、生成序列化资源;资源面板可配 Scale。
- **拖拽建组件**:把 `.json` 拖进场景/层级,自动创建带 `sp.spine` 组件的节点(接入 cce 的撤销/保存/快照机制);
- **Inspector 可视化**:spineData / defaultAnimation / defaultSkin / loop / premultipliedAlpha / useTint / sockets 等直接面板配置。

### 6. 功能完整,spine-ts 风格 API

- 完整骨骼能力:bones / slots / attachments / skins / 动画混合 / 事件;
- **兼容包装**:`TrackEntry / Bone / Slot / Attachment / Animation / Skin / Event / AnimationState`,上手即用;
- **sockets 挂点**:骨骼路径 → 目标节点,每帧自动同步变换;
- **局部换装**:`setSlotTexture` 运行时替换单个槽位贴图;
- **渲染范围控制**:`setSlotsRange` 限制渲染槽位区间;
- **事件桥接**:C 回调桥回 JS,每帧派发 START / INTERRUPT / END / COMPLETE / DISPOSE / EVENT。

### 7. 多图集 + 渲染特性

- **多纹理页自动分段**:多图 Spine 按纹理页切 segment,多一次贴图 = 多一次 draw call,自然正确处理;
- **材质切换**:按混合模式 + premultipliedAlpha + useTint 自动选内置 `default-spine-material`,支持双色(tint)顶点;`premultipliedAlpha` 默认 false,只在图集按 PMA 导出时开启。

---

## 四、平台支持

| 平台 | 后端 | 状态 |
|---|---|---|
| 编辑器场景 / 浏览器预览 / Web | wasm (AOT Embind) | 已接入,使用外置 wasm + `cc.wasm` |
| 微信小游戏 | wasm (AOT Embind) | 加载、运行验证通过 |
| Windows Native Simulator | JSB | 重建并运行验证通过 |
| Android | JSB | MuMu Android 15(arm64-v8a)全量 clean 构建、安装、运行验证通过 |
| iOS / macOS | JSB | 已接入原生插件配置,尚未做设备运行验证 |

---

## 五、技术栈与关键决策

| 决策 | 选择 | 为什么 |
|---|---|---|
| 原生加速 | 统一 C++ 门面(SpineRuntime) | 性能 + 双平台同构 |
| Web/小游戏绑定 | AOT Embind | 声明式绑定,且不依赖 `eval/new Function` |
| 原生绑定 | JSB 手动绑定 | 零工具链依赖,和 embind 共享门面 |
| 渲染 | `cc.UIMesh` | 被引擎 2D 合批管线收编,而非自建 MeshRenderer |
| 符号冲突 | `spine=spine43` 命名空间隔离 | 双版本共存 |
| WASM 加载 | 外置 `.wasm` + `cc.wasm` | Web、预览、小游戏共用一条平台适配路径 |
| 分发 | vendor 源码 + prebuilt wasm | 避免子模块和用户侧编译步骤 |

---

## 六、与官方方案的关系

- **不是替代**,是**补位**:当前自定义 Creator 3.8 引擎默认的 Spine 3.8 运行时继续可用,本插件用一套隔离的 `spine43::` 命名空间提供 4.3 能力,两者可共存。
- 结构上可作为参考:`SpineRuntime.cpp` 是唯一 include `<spine/spine.h>` 的文件,升级 spine-cpp 时适配工作主要集中在这里;只要门面接口不变,绑定层和 TS 层无需跟着改。

---

## 七、快速体验

1. 把插件目录放进项目 `extensions/`(或商店安装);
2. 把 Spine 4.3 导出的 `.json` + 同名 `.atlas` + `.png` 放进资源目录;
3. 拖进场景,或代码里 `node.getComponent('sp.spine').setAnimation(0, 'walk', true)`。

详见 [`../README.md`](../README.md)(API 速查)和 [`PLUGIN-DEV-GUIDE.md`](PLUGIN-DEV-GUIDE.md)(从零制作教程)。

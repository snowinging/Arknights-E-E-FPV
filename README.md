# Arknights_E_E_FPV

《明日方舟：终末地》(Arknights: Endfield) 的 ReShade 画质增强插件 —— FPV 魔改版。

在 RenoDX 原作的基础上，额外加入了一套完整的穿越机（FPV）模拟器，包括遥控器输入、
飞行物理、键鼠自由飞、以及着色器层飞行 HUD。

---

## 来源与致谢

本项目是 **RenoDX: Arknights Endfield Enhancer** 的修改版，不是从零开始的作品。

- 原作：RenoDX Endfield Enhancer，作者 **ItsaRat**
- 框架：**RenoDX**，作者 Carlos Lopez Jr. (clshortfuse)
- 许可证：MIT，原始版权声明完整保留，见 [LICENSE](./LICENSE)
- 本修改版：snowinging、deepseek(ai)、GLM(ai)

按 MIT 条款，本仓库保留原许可证与版权声明；在原作之上新增的部分同样以 MIT 发布。

---

## 新增内容

### FPV 穿越机

- 三种飞行模式：自稳 Angle / 半自稳 Horizon / 手动 Acro.
- 支持调节最大推力和碰撞检测.
- 遥控器输入：DirectInput8 优先、winmm 回退；支持设备自动扫描与手动指定.
- 支持手动和自动标定校准.
- 摄像头云台上仰角可调.

### 键鼠操作

- WASD平移控制
- 鼠标直接控制视角朝向，停下即保持
- 空格上升，Ctrl 或 Shift 下降
- 惯性模拟

### 飞行 HUD（着色器层）

- 准星、地平线、双摇杆方框.
- 锚点位置、方框边长与间距、地平线竖直 FOV 均可调，参数持久化

### 原版功能

FPS 解锁与限帧、SSR、DoF、GTAO、LOD 控制、NPC 剔除与加载距离、
HDR 帧生成、自由相机与拍照模式增强、UI 元素隐藏等。

---

## 安装

1. 安装 ReShade 6.8 或更高版本（本项目按 6.8.0.1 开发）
2. 将 `endfield-enhancer-FPV.addon64` 放入游戏根目录
3. 按你惯用的方式挂载 ReShade 加载器
4. 进入游戏按 `Home` 打开面板，标题为 `E_E_FPV`

> 注意：游戏带反作弊。公开渠道请勿分发任何绕过反作弊的方法或文件，
> 使用第三方插件本身可能违反游戏用户协议，风险自负。

---

## 默认操作

| 按键 | 作用 |
| --- | --- |
| `F9` | 开关 FPV |
| `Home` | 打开/关闭设置面板 |

面板 `FPV 穿越机` 分区里可以切换飞行模式、输入源、以及全部飞行参数。

---

## 配置

所有设置写入 ReShade 的 `ReShade.ini`，段名为 `[AEEF-preset1]`，
包括遥控器轴映射、行程标定值、HUD 布局等，重装或重启后自动读回。

---

## 依赖准备

本仓库出于体积考虑不含第三方依赖（Reshade、Detours、DLSS 等），清单与对应的落盘路径见
[.gitmodules](./.gitmodules)。克隆后需要先把它们补齐，否则无法构建：

```sh
git submodule update --init --recursive
```

如果上面这条不生效（本仓库未登记子模块指针），就照着 .gitmodules 里的 url 逐个
clone 到对应的 path 下，例如：

```sh
git clone https://github.com/crosire/reshade external/reshade
git clone https://github.com/microsoft/Detours.git external/Detours
```

---

## 构建

Linux 交叉编译（需要 MSVC 头文件与工具链）：

```sh
. ~/tools/msvcenv.sh && export PATH=~/tools/bin:$PATH
（在项目根下运行）
以下路径为本地工具链，请按自己的环境替换
cmake -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$HOME/tools/windows-cross.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --target endfield-enhancer-FPV -j32
```

产物：`build/endfield-enhancer-FPV.addon64`

---

## 已知限制

- 暂不支持速度 / 高度 / 电量类 HUD 指示
- 碰撞使用球体扫描，很薄的结构可能被穿过
- 锚点迁移（让流式与 LOD 中心跟随 FPV 相机）仍是实验功能，默认关闭，
  开启后区域触发器可能被远程触发
- 键鼠自由飞模式下除碰撞外未支持其它游戏交互

---

## 许可证

本项目以 **MIT License** 发布，完整条款见 [LICENSE](./LICENSE)。

原始作品的版权归原作者所有：

- **RenoDX** 框架 —— Copyright (c) 2025 Carlos Lopez Jr. (clshortfuse)
- **Arknights Endfield Enhancer** 原作 —— 作者 ItsaRat

本修改版在上述作品的基础上构建，完整保留了原有的许可证文本与版权声明；
本仓库新增与修改的部分，同样以 MIT License 发布。

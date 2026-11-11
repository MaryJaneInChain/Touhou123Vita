# 东方非想天则 Vita 移植版（汉化 + 宽屏）

[Español / English](README.md) | 简体中文

这是《东方非想天则 ～ 追寻特大型人偶之谜》的 PS Vita 移植版，基于 [Botarguirix 的 Touhou123Vita](https://github.com/Botarguirix/Touhou123Vita)：把 PC 版 1.10a 的 `th123.exe` 静态重编译成 C，再在 Vita 上模拟它用到的 Windows 接口。

这个 fork 加上了对汉化版的支持，另外还有几处改动：
 - 汉化版的文字能正常显示（GBK 编码，简体中文字体用 Noto Sans Mono CJK SC）。汉化补丁在 exe 里翻译的界面文字，可以用 `tools/make_zh_exe.py` 搬到 1.10a 的 exe 里
 - 画面是原生的 960x544，而且是**游戏内视野真的变宽了**，不是拉伸。标题、选人、选场景、战斗 HUD、暂停和结算、标题下的各个子菜单、开机画面和载入画面都重新排了版，铺满整个屏幕
 - 修好了音乐里的杂音（原移植的问题：音乐解码时 CPU 停在了游戏留下的舍入模式上）
 - 更流畅：战斗平均帧数从约 43 提到约 56（给 vitaGL 更多显存、日志改到后台线程写、合并绘制、大贴图在别的核心上并行解压），背景的色带也少了
 - 能在 Linux 上编出 PC 版，方便调试

原移植作者的说明（西班牙语 / 英语）在 [README.md](README.md)，控制键位、10.5 人物联动等内容那边写得更全。

## 声明

这个项目和上海爱丽丝幻乐团、黄昏边境、Sony 都没有关系。你需要自己的正版东方非想天则，仓库里不含任何游戏文件（EXE、DAT、提取出的资源）。
原移植的仓库没有附带许可证，所以这个 fork 不提供编译好的 VPK，需要自己编译（见[编译](#编译)）。

## 需要准备的东西

| 项目 | 说明 |
|---|---|
| 主机 | 装了破解环境的 PS Vita，以及用来安装 VPK、传文件的 VitaShell |
| 着色器编译模块 | 你自己的 `libshacccg.suprx`，放在 **`ur0:data/libshacccg.suprx`**（vitaGL 需要，这里不提供） |
| 正版游戏 1.10a | `th123.exe`、`th123a.dat`、`th123b.dat`。exe 必须是原版 1.10a，SHA-256 为 `f0db93660ceb8635a1921e0ba4686fcfbd5c492311dee6ef7061b7bada16731e` |
| 汉化补丁 | 汉化版的 `th123c.dat`（中文文本），以及汉化版的 `th123.exe`（只在第 2 步生成汉化 exe 时用，SHA-256 为 `4da093ee7e9b3712990d0e2173fd7954b23b0376c45f571023b8512a5900d4e6`） |
| 可选：东方绯想天 | `th105a.dat`、`th105b.dat`（以及 `th105c.dat`），用来联动 10.5 的人物和场景 |

汉化版自带的 `th123.exe` 是 1.10 版，这个移植不认（它只认 1.10a 的入口和映像大小），所以需要第 2 步。

## 安装

 1. 编译出 VPK（见[编译](#编译)）。
 2. 在电脑上生成带汉化界面文字的 exe：
```
python3 tools/make_zh_exe.py 原版1.10a的th123.exe 汉化版的th123.exe th123.exe
```
   它把汉化补丁翻译过的约 60 条界面文字（提示框、确认框、出错信息、窗口标题）原位写进 1.10a 的 exe，代码、入口和映像大小都不变。工具里只记了这些文字的位置，译文是从你自己的汉化版 exe 里读出来的。
   不做这一步、直接用原版 1.10a 的 exe 也能玩，只是这些提示会显示日文。
 3. 在 Vita 上新建 **`ux0:data/touhou123vita/`**，把下面这些文件直接拷进去（不要再套一层文件夹）：
```
ux0:data/touhou123vita/
├── th123.exe          # 第 2 步生成的（或原版 1.10a）
├── th123a.dat         # 正版
├── th123b.dat         # 正版
├── th123c.dat         # 汉化版的
├── th105a.dat         # 可选：东方绯想天
├── th105b.dat         # 可选：东方绯想天
└── configex123.ini    # 可选：联动 10.5 时需要，[th105path] 一节写 path=./
```
 4. 确认 `ur0:data/libshacccg.suprx` 在（注意是 `ur0:`，不是 `ux0:`）。
 5. 用 VitaShell 安装 VPK，然后从 LiveArea 启动「东方非想天则 汉化版」。第一次载入会比较久。

## 开关文件

在 `ux0:data/touhou123vita/` 里放一个空文件（内容无所谓），下次启动生效：

| 文件 | 作用 |
|---|---|
| `4x3.txt` | 关掉宽屏，回到中间 4:3、两侧黑边的画面 |
| `res.txt` | 写 `720`、`640` 或 `480`，降低渲染分辨率换帧数（默认 960x544） |
| `cores.txt` | 让游戏线程独占一个 CPU 核心，其余线程用另外两个。实测没有明显提升 |
| `tex32.txt` | 大贴图不压成 16 位（画质略好，但更占显存，重的场景可能掉帧） |
| `nosound.txt` | 关掉声音（排查问题用） |

## 已知问题

 - 暂停菜单、Profile、设置等几处的选项是游戏数据里的日文图片，汉化补丁没有翻，所以仍是日文
 - 战斗平均大约 55～56 帧。招式和特效很多的瞬间，或者某个符卡、特效第一次出现时，会掉帧或卡一下
 - 联机、本地双人、摇杆和触屏，原移植就不支持
 - 剧情模式和街机模式没有完整通关测过；个别画面可能还有没调好的位置

遇到问题可以按住 START + SELECT 两秒退出，`ux0:data/touhou123vita/` 里的 `recomp-trace.log` 和 `recomp-result.txt` 有运行记录。欢迎开 issue（不要附上游戏文件）。

## 编译

在 Linux 上编译（Windows 上用 WSL 应该也行，但没试过）。需要 Python 3、`podman` 或 `docker`，以及原版 1.10a 的 `th123.exe`。VitaSDK 用官方的容器镜像，不用另外安装。

```
git clone https://github.com/MaryJaneInChain/Touhou123Vita.git
cd Touhou123Vita
mkdir -p original && cp 原版1.10a的th123.exe original/th123.exe
python3 -m venv local/venv
local/venv/bin/pip install capstone
local/venv/bin/python tools/fetch_font.py --sc
local/venv/bin/python recomp/discover.py
local/venv/bin/python recomp/lift_all.py --gbk --wide
podman run --rm -v "$PWD":/src -w /src docker.io/vitasdk/vitasdk:2026.08 \
  python3 recomp/build_vita.py --gbk --wide
```

 - `fetch_font.py --sc` 从 Noto CJK 官方仓库下载简体中文字体（SIL OFL），校验 SHA-256 后放到 `local/fonts/`，编译时会和许可证一起打进 VPK。
 - `lift_all.py` 把 exe 翻译成 C（`--gbk` 打上汉化版需要的那处字形补丁，`--wide` 打上宽屏的镜头和背景补丁），生成的 C 放在 `artifacts/`，不进 git。
 - 第一次编译要十几分钟，之后只改了宿主代码的话几秒就好。
 - VPK 生成在 `artifacts/recomp/f0db93660ceb8635a1921e0ba4686fcfbd5c492311dee6ef7061b7bada16731e/vita/T123Recomp-gbk-wide.vpk`。LiveArea 用的是自动生成的占位图。

### PC 版（Linux，调试用）

```
local/venv/bin/python recomp/lift_all.py --gbk --wide
T123_GBK=1 T123_WIDE=1 local/venv/bin/python recomp/build_host.py
T123_FONT=local/fonts/NotoSansMonoCJKsc-Regular.otf \
  artifacts/recomp/f0db93660ceb8635a1921e0ba4686fcfbd5c492311dee6ef7061b7bada16731e/host/th123_host.exe \
  游戏目录/th123.exe 游戏目录
```

需要 SDL2、FreeType、libpng、libvorbis、zlib 的开发包。`T123_TEX16=1` 可以在 PC 上看 Vita 的 16 位贴图效果，`T123_NARROW=1` 回到 4:3。

## 致谢

 - [Botarguirix](https://github.com/Botarguirix/Touhou123Vita)：原移植
 - 汉化补丁的各位译者
 - [Noto CJK](https://github.com/notofonts/noto-cjk)：简体中文字体（SIL OFL 1.1）

# RTE2026 Workshop：多设备 Agora 实时语音助手例程

这个目录是 RTE2026 Workshop 的一部分，演示如何把 Agora Conversational AI 的实时语音服务接入 M5Stack 设备。例程提供 StickS3 和 Stopwatch 的可运行板型适配；服务流程可以复用，硬件层仍应按目标设备确认。

开源许可见根目录 [`LICENSE`](LICENSE)（MIT）与第三方说明 [`NOTICE.md`](NOTICE.md)。

## Recipe / 投稿文档

English recipe for [Agora Voice AI Recipes](https://recipes.agora.io/): [`docs/ai/RECIPE.md`](docs/ai/RECIPE.md).

默认走 **Agora 国际站 / Global（`AGORA_AREA=us`，设备 `VOICE_AREA_CODE=0xFFFFFFFF`）**。中国区（声网 / `AGORA_AREA=cn`）见下文「中国区可选配置」。

## 一、例程说明

### 当前参考实现

固件使用 ESP-IDF、M5Unified、M5GFX、LVGL 和 Agora RTSA Lite。启动时由 M5Unified/M5GFX 自动识别板型，`main/board_profile.*` 再选择屏幕、输入和 UI 布局：

- 按 A 键开始或结束一次流式语音会话。
- 设备通过局域网访问本目录内的 Python 服务。服务生成 RTC/RTM 配置，启动或停止 Conversational AI Agent；App Certificate 只留在服务端。
- 麦克风上传 G.722 音频，Agent 返回的语音在设备端解码并播放。回答时暂停麦克风，当前实现是半双工，不包含回答中打断和 AEC。
- 屏幕显示待机、连接中、请说话、聆听中、思考中、说话中、结束、错误和离线状态，并用两只由 LVGL 绘制的动态眼睛表达状态：待机会眨眼、四处张望，连接时左右扫视，聆听和说话时随真实音量变化，思考时向上看，结束、错误和离线时安静下来。
- B 键循环调节扬声器音量：`100% → 80% → 60% → 40% → 20% → 0% → 100%`。StickS3 和 Stopwatch 默认 100%；矩形屏在最右侧显示竖条，圆形 Stopwatch 在右侧用一小段圆弧显示当前档位。
- 等待用户说话超过 15 秒会自动关闭会话；单次回答等待超过 30 秒会结束当前请求。

两种板型的 UI 与硬件差异如下：

| 板型 | 屏幕与 UI | 输入与音频 |
| --- | --- | --- |
| M5StickS3 | 135×240 竖屏，保留紧凑参考布局 | A 开始/结束，B 调音量，M5Unified 配置板载 ES8311/AW8737 |
| M5StopWatch | 468×468 控制画布，466×466 圆形 OLED。音量是右侧一小段圆弧，自下而上填充；A 键操作在底部，文字都在圆内安全区 | 左键（BtnA）开始/结束或重试，B 调音量；M5Unified 负责音频、功放和按键初始化 |

应用会按板型设置扬声器和麦克风生命周期；实际声压仍取决于具体硬件、外壳和供电。
上表两种设备都使用 ESP32-S3，因此可以编译同一份 `esp32s3` 固件；运行时识别只负责板型差异，不能把这份固件直接用于其他芯片系列。

### 换用其他 M5 设备时

以下部分可以直接复用：

- `service/agent-quickstart-python/server/` 的 FastAPI 接口和 Agent 生命周期；
- `/get_config`、`/startAgent`、`/stopAgent` 的请求格式；
- `main/voice_transport.*` 的 RTC/音频状态机思路、超时处理和日志约定；
- LVGL 的状态页、动态眼睛表情，以及 `tests/`、`simulator/` 的主机验证。

以下部分必须先按目标板确认：

- M5Unified/M5GFX 的 board profile、屏幕方向和分辨率；
- I2S 麦克风、编解码器、功放、扬声器增益和采样率；
- A/B 按键或触摸输入、USB 串口和供电；
- 固件的 `sdkconfig`、分区表和引脚定义。

因此，例程已覆盖上表两种板型；换用其他 M5 设备时，先查目标产品的官方资料，再增加 `main/board_profile.*` 的枚举、UI 布局和音频/按键分支，不要只替换一个宏就假定音频和按键兼容。

### 目录结构

```text
RTE2026-StickS3-Demo/
├── main/                         # 固件、Agora 传输、音频和 LVGL UI
├── src/                          # G.711/G.722 与 IDF 兼容层
├── third_party/agora_rtsa_sdk/   # Agora RTSA 静态库和头文件
├── components/                   # M5Unified、M5GFX、LVGL 固定源码快照
├── service/agent-quickstart-python/server/
│                                  # 配套 Agora Agent FastAPI 服务
├── simulator/                    # 原生 LVGL 截图验证器
├── tests/                        # 音频、传输和串口验证
├── tools/                        # 依赖恢复和串口脚本
├── ui_tools/                     # 角色图片和字体生成工具
└── skills/                       # 项目内的 AI Skill 副本
```



## 二、开箱即用教程

推荐把环境准备交给 AI 编程工具：先让它读取本目录的 `AGENTS.md`，执行只读检查，列出缺少的工具，再由你确认后安装或修改系统环境。可以直接复制下面这段提示词：

```text
请先读取这个项目的 AGENTS.md、skills/agora-skills-main/skills/agora/SKILL.md和 skills/m5stack-assistant/SKILL.md。
按 README 的前置条件检查 Agora CLI、Python、ESP-IDF、依赖和串口设备；先只读检查，不要打印任何密码、token、App Certificate 或 .env 内容。
列出缺项和将执行的命令，得到我确认后再安装，然后完成服务配置、固件配置、编译和烧录。
```



### 1. 准备 Agora 账号和项目

1. 默认使用 [Agora 国际站控制台](https://console.agora.io/)：服务端 `AGORA_AREA=us`（`Area.US`），设备端默认 `VOICE_AREA_CODE=0xFFFFFFFF`。若使用声网中国区项目，改用 [声网中国区控制台](https://console.shengwang.cn/)，服务端设 `AGORA_AREA=cn`，设备本地头文件设 `VOICE_AREA_CODE=0x00000001`，CLI 使用 `agora login --region cn`。
2. 安装 Agora CLI。Linux/macOS：
  ```bash
   curl -fsSL https://dl.agora.io/cli/install.sh | sh
  ```
   Windows PowerShell：
   ```powershell
   irm https://dl.agora.io/cli/install.ps1 | iex
   ```
   安装后先确认版本和 PATH，再继续登录（当前 CLI 最低版本为 `0.2.1`）：
   ```bash
   agora version
   ```
3. 登录时必须指定中国区。建议显式刷新项目缓存、绑定项目并检查 ConvoAI 能力：
  ```bash
   agora login --region cn
   agora whoami
   # 浏览器无法自动打开时：
   agora login --region cn --no-browser
   agora project list --refresh-cache --json
   agora project use <project-id-or-exact-name>
   agora project feature enable convoai
   agora project doctor --feature convoai
   ```
   `agora login` 会把授权会话保存在本机 Agora CLI 配置目录；不需要把用户的 Secret 粘贴到代码或聊天中。授权回调地址是本机 `http://localhost:<端口>/oauth/callback`，打开登录链接的浏览器必须能访问运行 CLI 的这台电脑。若项目列表为空，先在中国区控制台创建项目，再重新执行 `project list --refresh-cache`。
   绑定项目后确认已启用 App Certificate。中国区没有本例所需的 Agora 托管 LLM/TTS 密钥，对话模型使用现场准备的 MiniMax Key。
   如果已有项目没有 App Certificate，在声网控制台的项目设置中启用。选中项目后也可以运行 `agora project env --with-secrets` 查看本地配置；该命令会输出敏感值，只能写入本机 `.env`，不要提交或公开。



### 2. 安装 ESP-IDF v6.0.3

工程依赖和已验证构建使用 [ESP-IDF v6.0.3 release](https://github.com/espressif/esp-idf/releases/tag/v6.0.3)。请使用这个版本，工程的 `main/idf_component.yml` 和 `dependencies.lock` 已锁定为 6.0.3。
把 IDF 放在项目目录下，最终目录应为：

```text
RTE2026-StickS3-Demo/
└── esp-idf/
    ├── export.sh / export.bat
    └── install.sh / install.bat
```

不要把 IDF 装到只在某台电脑存在的绝对路径后再把该路径写进项目配置。

#### Linux

先安装 Git、Python 3、CMake、Ninja、ccache、USB 工具和编译依赖。以 Debian/Ubuntu 为例：

```bash
sudo apt update
sudo apt install -y git wget flex bison gperf cmake ninja-build ccache \
  libffi-dev libssl-dev dfu-util libusb-1.0-0 python3 python3-pip python3-venv
```

如果当前用户没有串口权限，把用户加入 `dialout` 后重新登录：

```bash
sudo usermod -aG dialout "$USER"
```

进入项目目录下载并解压 release 压缩包，然后改名为 `esp-idf`：

```bash
cd RTE2026-StickS3-Demo
curl -L https://github.com/espressif/esp-idf/archive/refs/tags/v6.0.3.tar.gz \
  -o esp-idf-v6.0.3.tar.gz
tar -xzf esp-idf-v6.0.3.tar.gz
mv esp-idf-6.0.3 esp-idf
./esp-idf/install.sh esp32s3
. ./esp-idf/export.sh
```

如果压缩包缺少子模块而安装脚本报错，使用包含子模块的方式重新取得同一标签：

```bash
git clone --recursive --branch v6.0.3 \
  https://github.com/espressif/esp-idf.git esp-idf
./esp-idf/install.sh esp32s3
. ./esp-idf/export.sh
```

每次打开新的终端都要重新执行 `. ./esp-idf/export.sh`，或者使用项目自己的 shell 启动脚本加载环境。

#### macOS

安装 Xcode Command Line Tools、Git、Python 3 和 Homebrew，然后安装 IDF 工具：

```bash
xcode-select --install
brew install cmake ninja ccache dfu-util python
```

从 release 页面下载 `v6.0.3` 的 tar.gz/zip，解压到项目的 `esp-idf/`，再执行：

```bash
./esp-idf/install.sh esp32s3
. ./esp-idf/export.sh
```



#### Windows

安装 Git for Windows、Python 3、CMake/Ninja，以及目标板使用的 USB-UART 驱动（常见为 CP210x 或 CH34x，按设备实际芯片选择）。
从 release 页面下载 zip，在 PowerShell 中放入项目目录：

```powershell
Expand-Archive .\esp-idf-v6.0.3.zip -DestinationPath .
Rename-Item .\esp-idf-6.0.3 .\esp-idf
.\esp-idf\install.bat esp32s3
.\esp-idf\export.bat
```

也可以使用 ESP-IDF 安装器生成的 ESP-IDF PowerShell/CMD 快捷方式；打开新终端后需要再次运行 `export.bat`。串口驱动安装完成后，在设备管理器确认 COM 端口。

### 3. 安装项目内 Skill 到 AI 编程工具

`skills/agora-skills-main/` 和 `skills/m5stack-assistant/` 是项目内随代码保存的副本，便于 Workshop 离线查看。
仅复制文件不会自动注册到所有 AI 工具；项目根目录的 `AGENTS.md` 会要求遵守项目指令的工具主动读取它们，其他工具仍需按各自规则安装：

- **支持仓库指令的工具**：直接打开项目根目录，确认它读取了 `AGENTS.md`。
- **Claude Code**：可按 Agora Skill README 使用插件，或在项目内执行 `mkdir -p .claude/skills && cp -r skills/agora-skills-main/skills/agora .claude/skills/` 和 `cp -r skills/m5stack-assistant .claude/skills/m5stack-assistant`。
- **Cursor**：把两个入口文件分别复制到项目的 `.cursor/rules/`，例如： `mkdir -p .cursor/rules && cp skills/agora-skills-main/skills/agora/SKILL.md .cursor/rules/agora.md` 和 `cp skills/m5stack-assistant/SKILL.md .cursor/rules/m5stack-assistant.md`。
- **其他工具**：把 `skills/agora-skills-main/skills/agora/SKILL.md` 和 `skills/m5stack-assistant/SKILL.md` 加入工作区上下文；先读入口文件，再按其中的链接加载细节。

Agora 任务的入口必须是 `skills/agora-skills-main/skills/agora/SKILL.md`，M5Stack硬件任务的入口必须是 `skills/m5stack-assistant/SKILL.md`。涉及两者时两个都读。

### 4. 配置并启动 Agora 服务

服务代码在 `service/agent-quickstart-python/server/`。它是从官方 Python quickstart 整理出的设备后端，固件只需要 FastAPI 服务，不需要启动 `web/` 前端。

```bash
cd service/agent-quickstart-python/server
python3 -m venv .venv
source .venv/bin/activate                 # Windows：.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
cp .env.example .env                      # Windows：Copy-Item .env.example .env
```

在 `.env` 中填写：

```dotenv
AGORA_APP_ID=your-agora-app-id
AGORA_APP_CERTIFICATE=your-agora-app-certificate
MINIMAX_API_KEY=your-minimax-api-key
PORT=8001
```

`MINIMAX_API_KEY` 同时用于 LLM 和 TTS。中国区声网不提供托管供应商密钥，这个 Key 由 MiniMax 现场准备；不要把它写进固件或提交到 Git。

已经完成 `agora login --region cn` 和 `agora project use` 时，可以让 CLI 写入声网项目变量。
这里的服务是为 Workshop 整理过的后端，所以使用 `project env write`；如果你另行使用 Agora 官方 quickstart，则按 Skill 文档使用 `quickstart env write`。
下面的命令从项目根目录执行；如果当前仍在 `server/`，先回到项目根目录。

```bash
agora project env write service/agent-quickstart-python/server/.env
```

CLI 只会写入 `AGORA_APP_ID` 和 `AGORA_APP_CERTIFICATE`。写完后确认 `.env` 里仍有 `MINIMAX_API_KEY` 和 `PORT=8001`。不要用 `--with-secrets` 的输出替换 `.env` 以外的文件，也不要将 `.env` 加入提交。

启动服务：

```bash
python src/server.py
```

服务监听 `0.0.0.0:8001`，固件使用的接口为：

- `GET /get_config`：生成频道、UID 和短期 RTC/RTM token；
- `POST /startAgent`：启动 Agent，固件会请求 `output_audio_codec=g722`；
- `POST /stopAgent`：按 Agent ID 停止 Agent。

本机检查可以访问 `http://127.0.0.1:8001/get_config` 并确认返回成功；响应中有token，不要把完整响应粘贴到聊天或公开日志。
默认国际站链路（`AGORA_AREA=us`）：托管 Deepgram STT + OpenAI LLM + MiniMax TTS。中国区（`AGORA_AREA=cn`）：凤鸣 ASR + MiniMax LLM/TTS，需 `MINIMAX_API_KEY`。若需要改助手个性，在 `server/src/agent.py` 修改 `ADA_PROMPT` 和问候语；密钥只放在服务端 `.env`。

### 5. 配置 Wi-Fi 和固件服务地址

在项目根目录执行：

```bash
cp main/voice_config.local.example.h main/voice_config_local.h
```

编辑被 Git 忽略的 `main/voice_config_local.h`：

```c
#define VOICE_WIFI_SSID "your-wifi-ssid"
#define VOICE_WIFI_PASSWORD "your-wifi-password"
#define VOICE_SERVER_URL "http://192.168.0.101:8001"
#define VOICE_IDLE_TIMEOUT_MS 15000U
#define VOICE_RESPONSE_TIMEOUT_MS 30000U
#define VOICE_AREA_CODE 0x00000001U
```

`VOICE_AREA_CODE` 保持 `0x00000001`（`AREA_CODE_CN`），设备才会加入中国区媒体节点。

设备和运行服务的电脑必须在同一个可互通的 2.4 GHz Wi-Fi。把示例地址换成电脑在局域网中的 IPv4 地址；设备端不能使用 `localhost` 或 `127.0.0.1`。
同时允许操作系统防火墙接收 TCP `8001`，并确认服务绑定的是 `0.0.0.0`。

### 6. 编译、烧录和使用

每次新终端先加载 IDF 环境。命令从项目根目录执行：

```bash
. ./esp-idf/export.sh                  # Windows：.\esp-idf\export.bat
python3 tools/bootstrap_dependencies.py --check
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash           # Windows 使用 COMx，例如 COM5
idf.py -p /dev/ttyACM0 monitor
```

编译产物统一命名为 `build/m5stack_agora.bin`。`sdkconfig.defaults` 使用 240 MHz
CPU 和性能优化，避免 468×468 UI、Wi-Fi 和 10 ms 音频包同时运行时互相抢占。StickS3
和 StopWatch 都有 PSRAM；LVGL 对象池和音频包池优先放入 PSRAM，并保留内部 RAM 回退。
不要再手动打开只适用于某一块板的 `M5GFX_BOARD` 宏。首次从旧工程迁移或修改
`sdkconfig.defaults` 后，执行一次 `idf.py reconfigure` 再 `idf.py build`。

第一次使用或组件目录缺失时，先执行不带 `--check` 的 `python3 tools/bootstrap_dependencies.py`。
若端口不是 `/dev/ttyACM0`，替换为系统检测到的串口；烧录前关闭其他串口监视器。

设备启动后，两种板型使用同一份固件，M5Unified 会在日志中打印识别结果并选择对应 UI：

1. 等待 Wi-Fi 连接并显示“待机”。
2. 按 A，屏幕进入“连接中/请说话”，对着麦克风说话。
3. Agent 会依次进入思考和说话状态；回答结束后回到聆听。
4. 再按 A 主动结束，或等待超时自动清理会话。
5. 按 B 循环切换播放音量；矩形屏右侧竖条、Stopwatch 右侧短圆弧显示当前档位，串口发送 `b` 也可以调节。

启动日志应包含动态的 `BOARD=M5StickS3 panel=135x240` 或 `BOARD=M5StopWatch panel=468x468`，以及 Wi-Fi 连接和 UI 初始化信息。串口也支持 `a`（开始/停止）、`b`（音量）、`q`（停止）、`s`（状态）以及音频诊断命令；日志不会打印 token 或服务端 JSON 正文。

### 主机验证（可选）

无需设备和 Agora 凭据即可运行编解码、传输兼容层和 LVGL 截图验证：

```bash
bash tests/audio/run.sh
bash tests/transport/run.sh
cmake -S simulator -B simulator/build -DCMAKE_BUILD_TYPE=Release
cmake --build simulator/build -j8
simulator/build/voice_ui_sim simulator/captures
python3 simulator/render_captures.py simulator/captures
```

模拟器能验证状态布局、六档音量和眼睛表情，另有 StickS3 与 Stopwatch 的 smoke test：

```bash
simulator/build/voice_ui_multiboard_smoke sticks3 simulator/captures/sticks3
simulator/build/voice_ui_multiboard_smoke stopwatch simulator/captures/stopwatch
```

这些检查不能代替真实屏幕、物理按键、扬声器或 Wi-Fi/Agora 端到端测试。

## 安全和许可

`main/voice_config_local.h`、服务端 `.env`、虚拟环境和构建目录均为本地文件；不要提交 Wi-Fi 密码、App Certificate、token 或供应商 API key。M5Unified、M5GFX、LVGL 的离线快照、Agora RTSA 静态库和 G.722 源码许可见 `vendor/README.md`、`vendor/manifest.json` 及对应第三方目录。


## 中国区可选配置

- 服务端 `.env`：`AGORA_AREA=cn`，并设置 `MINIMAX_API_KEY`（凤鸣 STT + MiniMax LLM/TTS）。
- 设备 `main/voice_config_local.h`：`#define VOICE_AREA_CODE 0x00000001U`。
- CLI：`agora login --region cn`。设备与服务端区域必须一致。

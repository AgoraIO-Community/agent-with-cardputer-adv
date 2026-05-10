![Cardputer Voice Agent banner](assets/readme-banner.png)

## Table Of Contents

- [🤖 Cardputer Voice Agent With Agora Conversational AI](#-cardputer-voice-agent-with-agora-conversational-ai)
- [🚀 Quickstart](#-quickstart)
  - [✨ Option 1: Use Your AI Coding Tool](#-option-1-use-your-ai-coding-tool)
  - [🛠️ Option 2: Manual Setup](#️-option-2-manual-setup)
- [💻 Develop With PlatformIO](#-develop-with-platformio)

---

# 🤖 Cardputer Voice Agent With Agora Conversational AI

Run an Agora Conversational AI voice agent on M5Stack Cardputer ADV. The firmware connects over Wi-Fi to a local quickstart server, joins Agora RTSA, and lets you talk with the agent from the Cardputer.

---

## 🚀 Quickstart

This firmware talks to a local Agora Conversational AI server running on your PC. Keep the Cardputer and your PC on the same Wi-Fi, then configure the firmware with your PC's LAN address on port `8000`, for example `http://192.168.0.101:8000`.

### ✨ Option 1: Use Your AI Coding Tool

This is the recommended path if you use Codex, Claude Code, Cursor, Windsurf, Copilot, or another AI coding assistant. The AI guide in `docs/ai-quickstart.md` is written for the assistant to read and execute.

Install Agora's AI coding skill first:

```bash
npx skills add github:AgoraIO/skills
```

Then open this repo in your AI coding tool and paste:

```text
Use the Agora skill from https://github.com/AgoraIO/skills.
Read docs/ai-quickstart.md and complete the setup for this Cardputer voice agent project.
Set up agent-quickstart-python, configure the local firmware config, build with PlatformIO, and explain the upload and monitor steps.
Keep secrets only in ignored local config files and do not print them.
```

When the assistant finishes the setup and the build succeeds, upload and monitor from the terminal:

```bash
pio run -t upload
pio device monitor
```

Press `k` on the Cardputer keyboard to start the agent.

### 🛠️ Option 2: Manual Setup

If you prefer direct terminal steps, follow [docs/manual-setup.md](docs/manual-setup.md).

---

## 💻 Develop With PlatformIO

Open this repo in VS Code and install the PlatformIO IDE extension. PlatformIO should detect the `cardputer-whip` environment from `platformio.ini`.

Useful PlatformIO actions:

- Build: compiles the voice-agent firmware.
- Upload: flashes the connected Cardputer.
- Monitor: opens the serial monitor at `115200` baud.
- Upload and Monitor: flashes the device, then opens serial logs.

Equivalent CLI commands:

```bash
pio run
pio run -t upload
pio device monitor
```

The project is configured for:

- PlatformIO environment: `cardputer-whip`
- Board: `esp32-s3-devkitc-1`
- Framework: ESP-IDF
- Flash size: 8 MB
- Upload speed: `115200`
- Monitor speed: `115200`

Keep local credentials and server URLs in `src/app_config_local.h`. That file is git-ignored and overrides the placeholder defaults from `src/app_config.h`.

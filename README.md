![Cardputer RTSA Audio Client banner](assets/readme-banner.png)

# Cardputer RTSA Audio Client

ESP32-S3 / M5Stack Cardputer ADV audio client using Agora RTSA plus a local protocol server for config and agent control.

## Quickstart

### Option 1: Use Your AI Coding Tool

If you use Codex, Claude Code, Cursor, Windsurf, Copilot, or another AI coding assistant, install Agora's skill first so the assistant has Agora-specific setup guidance:

```bash
npx skills add github:AgoraIO/skills
```

Agora Skills: <https://github.com/AgoraIO/skills>

Then open this repo in your AI coding tool and use a prompt like this:

```text
Use the Agora skill from https://github.com/AgoraIO/skills.

Set up this Cardputer RTSA Audio Client project for local development.

1. Create src/app_config_local.h from src/app_config.local.example.h if it does not exist.
2. Put my Wi-Fi SSID, Wi-Fi password, and protocol server URL only in src/app_config_local.h.
3. Do not commit or print my secrets.
4. Install or use PlatformIO.
5. Run pio run and fix any setup issues.
6. Explain how to upload and monitor the firmware on a Cardputer ADV.

My protocol server URL is: <your-protocol-server-url>
```

When the build works, upload and monitor from the terminal:

```bash
pio run -t upload
pio device monitor
```

Press `k` on the Cardputer keyboard to start the agent.

### Option 2: Manual Setup

Install PlatformIO if you do not already have it:

```bash
pipx install platformio
```

Create your local ignored config:

```bash
cp src/app_config.local.example.h src/app_config_local.h
```

Edit `src/app_config_local.h` and set:

```c
#define APP_WIFI_SSID "your-wifi-ssid"
#define APP_WIFI_PASSWORD "your-wifi-password"
#define APP_PROTOCOL_BASE_URL "http://your-protocol-server:8000"
```

Build the firmware:

```bash
pio run
```

Put the Cardputer in upload mode:

1. Unplug USB.
2. Switch power off.
3. Hold `G0`.
4. Plug USB back in.
5. Start upload.
6. Release `G0` after writing begins.

Upload and monitor:

```bash
pio run -t upload
pio device monitor
```

On a working run, the serial monitor should show Wi-Fi connection, protocol config, RTSA join, and agent start logs. Press `k` on the Cardputer keyboard to start the agent.

## Develop With PlatformIO

Open this repo in VS Code and install the PlatformIO IDE extension. PlatformIO should detect the `cardputer-whip` environment from `platformio.ini`.

Useful PlatformIO actions:

- Build: compiles the ESP-IDF firmware.
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

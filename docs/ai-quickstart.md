# AI Quickstart

Use this guide with Codex, Claude Code, Cursor, Windsurf, Copilot, or another AI coding assistant to set up the Cardputer voice agent with Agora Conversational AI.

## Goal

Set up both parts of the local development flow:

1. A local Agora Conversational AI quickstart server running on your PC.
2. This Cardputer firmware, configured to reach that server at `http://<pc-lan-ip>:8000`.

Keep the Cardputer and your PC on the same Wi-Fi network.

## Recommended AI Prompt

```text
Use the Agora skill from https://github.com/AgoraIO/skills.

Follow docs/ai-quickstart.md exactly to set up this Cardputer voice agent project with Agora Conversational AI.

Set up the local server from https://github.com/AgoraIO-Conversational-AI/agent-quickstart-python.
Configure server-python/.env.local from server-python/.env.example.
Start the backend and confirm it is reachable at http://localhost:8000.
Find my PC LAN IP address and use http://<pc-lan-ip>:8000 as APP_PROTOCOL_BASE_URL.
Create src/app_config_local.h from src/app_config.local.example.h if it does not exist.
Put Wi-Fi credentials and the protocol server URL only in src/app_config_local.h.
Install or use PlatformIO, run pio run, then explain how to upload and monitor the firmware.

Do not commit or print secrets.
```

## Server Setup

Use Agora's official quickstart:

<https://github.com/AgoraIO-Conversational-AI/agent-quickstart-python>

Set it up next to this firmware repo or in another local workspace:

```bash
git clone https://github.com/AgoraIO-Conversational-AI/agent-quickstart-python.git
cd agent-quickstart-python
bun install
cd server-python
cp .env.example .env.local
```

Edit `server-python/.env.local` and set your Agora `APP_ID` and `APP_CERTIFICATE`.

Start the full quickstart:

```bash
cd ..
bun run dev
```

The backend runs at `http://localhost:8000`. For backend-only development, run:

```bash
bun run backend
```

## Firmware Setup

Find the PC LAN IP address on the same Wi-Fi network as the Cardputer. Use that IP with port `8000` as the firmware protocol server URL, for example:

```text
http://192.168.0.101:8000
```

Create the ignored local firmware config:

```bash
cp src/app_config.local.example.h src/app_config_local.h
```

Set these values in `src/app_config_local.h`:

```c
#define APP_WIFI_SSID "your-wifi-ssid"
#define APP_WIFI_PASSWORD "your-wifi-password"
#define APP_PROTOCOL_BASE_URL "http://your-pc-lan-ip:8000"
```

Build the firmware:

```bash
pio run
```

Upload and monitor:

```bash
pio run -t upload
pio device monitor
```

Press `k` on the Cardputer keyboard to start the agent.

## Expected Checks

On a working setup:

- The server backend is reachable at `http://localhost:8000` on the PC.
- The Cardputer uses `http://<pc-lan-ip>:8000`, not `localhost`.
- The Cardputer and PC are on the same Wi-Fi network.
- Serial logs show Wi-Fi connection, protocol config, RTSA join, and agent start logs.

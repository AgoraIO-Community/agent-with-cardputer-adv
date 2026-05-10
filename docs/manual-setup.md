# Manual Setup

Use this guide to set up the Cardputer voice agent by hand. It follows the same flow as `docs/ai-quickstart.md`, but every step is written for direct terminal use.

## Prerequisites

- Cardputer and PC on the same Wi-Fi network.
- Bun for the local voice agent server.
- PlatformIO for building and flashing this firmware.
- Agora account with an Agora project that has the mandatory Conversational AI features enabled.

## Agora Project

Use Agora CLI to create or prepare an Agora project with the required features enabled, then retrieve the App ID and App Certificate:

```bash
curl -fsSL https://raw.githubusercontent.com/AgoraIO/cli/main/install.sh | sh -s -- --add-to-path
```

Follow the CLI prompts to log in, select or create a project, enable the required Conversational AI features, and get the project credentials.

## Local Voice Agent Server

Set up Agora's official Conversational AI quickstart server:

<https://github.com/AgoraIO-Conversational-AI/agent-quickstart-python>

```bash
git clone https://github.com/AgoraIO-Conversational-AI/agent-quickstart-python.git
cd agent-quickstart-python
bun install
cd server-python
cp .env.example .env.local
```

Edit `server-python/.env.local` and set the Agora `APP_ID` and `APP_CERTIFICATE` from the CLI-created or CLI-selected project.

Start both the web client and backend:

```bash
cd ..
bun run dev
```

The backend will be available at `http://localhost:8000`. If you only want the backend, run:

```bash
bun run backend
```

## Firmware Config

Find your PC's LAN IP address. Your Cardputer must be connected to the same Wi-Fi network as your PC. Use `http://<your-pc-lan-ip>:8000` as the firmware protocol server URL, for example:

```text
http://192.168.0.101:8000
```

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
#define APP_PROTOCOL_BASE_URL "http://your-pc-lan-ip:8000"
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

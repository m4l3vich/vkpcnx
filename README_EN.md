<p align="center">
  <img src="docs/readme_logo.svg" />
</p>

# VKPCNX

[![Claude Code](https://img.shields.io/badge/Claude%20Code-D97757?logo=claude&logoColor=white)](https://claude.com/claude-code) [![Borealis UI](https://img.shields.io/badge/Borealis%20UI-1E88E5?logo=github&logoColor=white)](https://github.com/xfangfang/borealis) [![Switch Homebrew](https://img.shields.io/badge/Switch%20Homebrew-E60012?logo=retroarch&logoColor=white)](https://switchbrew.org/) [![Build](https://img.shields.io/github/actions/workflow/status/m4l3vich/vkpcnx/ci.yaml?branch=main&logo=githubactions&logoColor=white&label=build)](https://github.com/m4l3vich/vkpcnx/actions/workflows/ci.yaml)

[На русском](./README.md) | [Telegram channel](https://t.me/vkpcnx)

**Currently the app does not offer English localisation of the UI. Let me know if you need it.**

VK Play Cloud NX is a homebrew client for the Russian cloud gaming service called VK Play Cloud for modded Nintendo Switch consoles, developed by reverse-engineering the first-party web client. It supports everything necessary for playing PC games in cloud using Switch. The app's UI mimics the native Nintendo Switch system UI thank to [Borealis library (fork by  xfangfang)](https://github.com/xfangfang/borealis). Contains bugs and vibecode.

## Installation

Download `vkpcnx.nro` on the [Releases](releases/) page and copy it into the `switch` folder on your SD card. Open the "VK Play Cloud NX" app in hbmenu and follow instructions on the screen to log into an account and complete the initial setup.

> [!IMPORTANT]
> You cannot make purchases and change VK Play account settings — this was a deliberate decision. Use a web browser on another device.

## Feedback

If you encounter any issues when using the client or if you want to propose new features for the app, create an [issue here, on GitHub](issues/) or write a message in [VKPCNX Telegram channel's direct messages](https://t.me/vkpcnx?direct).

Before sending a report, please, collect some useful info about the error:

- **Client crashes on launch**: attach the latest files from `config/vkpcnx/logs` and `atmosphere/crash-reports/` folders on your SD card.
- **Client crashes or locks up in use**: on the next launch you will be asked to create a bug report, select "Создать отчёт".
- **A bug occurs when using the client**: describe the issue and conditions it occurs in in detail, attach some screenshots or a screen recording, and generate a report in the app (Main screen → Account settings → Обратная связь → Создать отчёт об ошибке), it will be saved in `config/vkpcnx/reports/` on the SD card.

> [!IMPORTANT]
> The app automatically redacts all personal data from the logs (passwords, E-Mail addresses, access tokens, IP addresses).

## Development and building (Linux/macOS)

A detailed description of the VK Play Cloud protocol (the results of AI-assisted reverse engineering of the official web client) can be found in [docs/VKPC_PROTO.md](docs/VKPC_PROTO.md).

To build the app you will need the following dependencies: `ffmpeg opus openssl@3 zlib` and `cmake` version 3.21 or newer.

To build the desktop version, run the `./scripts/build-desktop.sh` script. The resulting executable will be available at `./build/vkpcnx`.

To run the unit tests:

```bash
cmake -B build -DPLATFORM_DESKTOP=ON -DVKPCNX_BUILD_TESTS=ON && cmake --build build --target stream_protocol_test && ./build/stream_protocol_test
```

There are two ways to build for Switch:

- **Native devkitPro**: you will need devkitPro installed along with `switch-dev switch-curl switch-zlib switch-libopus
  switch-ffmpeg switch-libwebp switch-glfw switch-mesa switch-libdrm_nouveau`.
  Start the build with: `./scripts/build-switch.sh`
- **Via Docker**: you will need Docker installed. Start the build with:
  `./scripts/build-switch.sh --docker`

To build a debug variant, pass the `--debug` argument to `build-switch.sh`.

To test the client against the mock server:

```bash
python3 -m venv venv && venv/bin/pip install aiortc websockets protobuf av numpy
mkdir -p pb && protoc -I vkpcnx/proto --python_out=pb vkpcnx/proto/*.proto
openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 30 -subj /CN=localhost
venv/bin/python tests/mock_server.py --pb pb --cert cert.pem --key key.pem --end-after 30
VKPCNX_INSECURE_TLS=1 VKPCNX_PLAY_URL='playkey:///?host=localhost&port=19000&token=test' ./build/vkpcnx
```

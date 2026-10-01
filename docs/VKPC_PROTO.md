# VK Play Cloud — WebRTC streaming client protocol (reverse-engineered)

Everything below was derived from:

* the production web player bundle `https://vkplaycloud.mrgcdn.ru/bundle/bundle.min.js`
  (global `MGCWebRTCPlayer`, `PLAYER_CONSTS.VERSION = 48103567`, `VERSION_HASH = "970074c"`,
  `SOFTWARE_NAME = "webrtc-mailru"`), loaded by `cloud.vkplay.ru` (`main.v1.36.19.js`);
* the two Emscripten WASM modules embedded in that bundle (input serializer and cursor converter),
  which were run under Node and black-box tested until a clean-room re-implementation matched
  byte-for-byte over ~1000 randomized trials (`reference/input_v2_encoder.js`);
* the protobuf descriptors embedded in `libplaykey_android.so` of the Android TV APK
  (`jadx-out/`). The TV app is **not** a WebRTC client — it is a `NativeActivity` around the
  native Playkey engine (own UDP video/audio/input transport, FFmpeg/MediaCodec H.264 decode) — but
  it embeds the *same* `cg.network.protocol` schema the web client uses, which is how the full
  `.proto` files in `proto/` were recovered.

Directory layout of this deliverable:

```
VKPLAY_CLOUD_WEBRTC_PROTOCOL.md      this document
proto/*.proto                        recovered schemas (proto2, package cg.network.protocol[.cm|.mc|.cs|.sc])
proto/vkplay.descriptorset.bin       same as a FileDescriptorSet (usable with protoc --descriptor_set_in)
reference/input_v2_encoder.js        verified clean-room encoder for the binary input protocol (§8)
reference/input_v2_parser.js         decoder for the same
reference/input_transmitter.wasm     original Emscripten module (embind class InputTransmitter)
reference/cursor_converter.wasm      original cursor → BMP converter (InputCursorConverter)
```

---

## 1. Architecture

```
 browser (client)                                          VK Play Cloud
 ─────────────────                                         ─────────────────────────────────
 REST  https://userapi.cloud.vkplay.ru/api/…  ───────────► user API (auth cookies, queue, run) 
                                                           returns play_url = "playkey:///?host=…"
 WSS   wss://<manager host>:<port>            ───────────► "Manager" (zone/server selection, queue)
       binary frames, [len][type][protobuf]                sends MC_DIRECTION_PLAY (game server addr)
 WSS   wss://<game server host>:<port>        ───────────► "Game server" / game controller (signalling)
       binary frames, [len][type][protobuf]                SDP answer, ICE, session events, timers
 RTCPeerConnection #1 "VideoAndAudioStream"   ◄──────────  H.264 video + Opus audio (recvonly),
       (+ optional microphone track sendonly)              stream ids "VideoStream" / "AudioStream"
 RTCPeerConnection #2 "InputStream"           ◄─────────►  one unreliable DataChannel "InputStream"
       DataChannel frames use the SAME                     inputs (client→server), input reports,
       [len][type][protobuf] framing                       cursor images, clipboard (server→client)
```

There is exactly one manager connection and one game-server connection per session; the manager
connection is closed with `M_BYE` as soon as the game server confirms the client (§5.6). All
signalling for WebRTC (offers/answers/ICE) goes over the game-server WebSocket.
Everything is little-endian.

---

## 2. Obtaining a session (REST)

Base URL: `https://userapi.cloud.vkplay.ru/api` (config `API_URL + BRAND_DOMAIN + "/api"`),
axios with `withCredentials: true` (VK Play SSO cookies; a 403 `{"error":"no_sdc"}` triggers the SDC
cookie init dance on `cloud.vkplay.ru` and a retry). POST bodies may carry `ga_cid`.

| Call | Purpose |
|---|---|
| `POST /game_launchers/{launcherId}/queue` | enter queue for a game launcher; response has `status` (`allowed`/`active`/`canceled`), `number` (queue position), `is_ping_ready`, `game_launcher_id`, `not_exist_powerful_servers` |
| `GET /queue` | poll queue (every 1 s until `status == "allowed"`) |
| `DELETE /queue/delete`, `POST /queue/disable_powerful`, `GET /queue/server_load` | misc queue ops |
| `POST /pingtest` body `{"type":"webrtc_client"}` | returns `{play_url}` for a **ping-test** session (work-mode `ping_test`); done once when `is_ping_ready` is false |
| `POST /game_launchers/{launcherId}/run/` body `{"type":"webrtc_client","ac_id":…,"is_demo_launch":true?}` | returns `{play_url}` for the real session (work-mode `game_stream`) |
| `POST /game_launchers/{id}/force_kill_sessions` | kill an active session elsewhere |
| `GET /game_sessions/active`, `POST /game_sessions/{id}/result` (`updateGameSessionResult`) | session result upload after close (fields in §11) |
| `GET /vendor/proxy/client_version` | `{source, version}` → which player bundle to load (`/bundle/bundle.min.js?v=`) |

### 2.1 `play_url` ("initialization string")

```
playkey:///?host=<manager host>&port=<manager port>&token=<one-time auth token>
           [&gameId=<n>][&session-id=<id or "c<id>">][&language=ru|en][&uid=…]
           [&fps=<33|45|60|75|90|105|120>][&resolution=<WxH>][&work-mode=<0|1|2|3>]
           [&exe-cmd-line=…][&gcsettings=…][&region-code=…][&allow-features=<bitmask>]
           [&username=…&password=…]
```

Parsed with `parseNumbers`. `work-mode`: 0 `GAME_STREAM` (default), 1 `PING_TEST`, 2 `MANAGER_TEST`,
3 `REMOTE_PLAY`. `allow-features` bits: 1 clipboard, 2 smartcards, 4 printers, 8 drives.
`session-id` string `"c123"` → `play_token_id = 123`. Auth type: `token && gameId` → GameToken
(no auth message sent!), `token` → SingleToken (`CM_AUTH_TOKEN`), else user/password (`CM_AUTH`).
`fps`/`resolution` become the *startup limits* of the video configurator (§6.4).

---

## 3. Wire framing (WebSocket **and** DataChannel)

Every message, in both directions, on both WebSockets and on the input DataChannel:

```
offset 0  uint32 LE  length  = 4 + len(payload)      (i.e. type word + protobuf bytes)
offset 4  uint32 LE  type    = cg.network.protocol.MessageType enum value
offset 8  bytes      payload = protobuf-serialized message (may be empty)
```

Type-only messages (no payload) are exactly 8 bytes: `04 00 00 00 <type u32>`.
One WebSocket binary frame = one message (the client reads `event.data.arrayBuffer()` and dispatches
it as one message; it does not handle concatenated/fragmented messages). Same for DataChannel
messages (`binaryType = "arraybuffer"`).

Unknown type → reply `M_UNSUPPORTED { messag_type }`. `M_KEEP_ALIVE` (type 4, no payload) is sent
by the client **every 6 000 ms on each open WebSocket** (not on the DataChannel) and ignored on receipt.

### 3.1 MessageType values used by the web client

Full enum in `proto/network.proto`. The ones that matter:

| Name | # | Dir | Payload |
|---|---|---|---|
| M_NOTIFICATION | 1 | S→C | `Notification{notification_type, message}` — show toast |
| M_ERROR | 2 | S→C | `Error{error_type, partner_type, error_message, user_message, title_message}` |
| M_ACCEPT | 3 | S→C | none — first message after WS open |
| M_KEEP_ALIVE | 4 | both | none |
| M_MESSAGE_NOT_CORRECT | 5 | S→C | none — internal error |
| M_LOG | 8 | C→S | `Log{msg bytes, level}` — only after M_SET_LOG_LEVEL |
| M_SET_LOG_LEVEL | 9 | S→C | `SetLogLevel{level}` — start forwarding client logs ≥ level as M_LOG on the GS socket |
| M_TOKEN_ACCEPTED | 11 | S→C (DataChannel) | none — input init done |
| M_CAPABILITIES | 17 | both | `Capabilities{capabilities[], steam_bigpicture, modern_clipboard}` |
| M_UNSUPPORTED | 18 | both | `Unsupported{messag_type}` |
| M_BYE | 20 | both | `Bye{reason, has_error}` (client sends it empty) |
| M_CLIENT_BEFORE_CLOSE | 21 | C→S | none — "I'm about to close" |
| M_CLIENT_BEFORE_CLOSE_CONFIRMATION | 22 | S→C | none |
| M_ERROR_INFO | 23 | S→C | `ErrorInfo{component_type, error_code, error_message}` (log only) |
| CM_AUTH | 1793 | C→M | `cm.Auth{login,password,game_id}` |
| CM_ZONE_SERVER_PINGS | 1795 | C→M | `cm.ZoneServerPings{server_pings[{ping, server_id}]}` |
| CM_VALIDATE_VERSION | 1797 | C→M | `cm.ValidateVersion{software="webrtc-mailru", version=48103567}` |
| CM_SET_LANGUAGE | 1798 | C→M | `cm.SetLanguage{language}` (LANGUAGE_RU=2 / LANGUAGE_EN=1) |
| CM_AUTH_TOKEN | 1799 | C→M | `cm.AuthToken{token, mode}` mode: game_stream=0, ping_test=1 |
| CM_SYSTEM_INFO | 1800 | C→M | `SystemInfo{system_params[{name,value}]}` |
| CM_VIDEO_SETTINGS | 1801 | C→M | `VideoSettings{max_fps, max_resolution, bitrate_min_kbps, bitrate_max_kbps, number_of_slices, number_of_ref_frames}` |
| CM_CONNECT_SERVER | 1804 | C→M | `cm.ServerConnectionStatus{ip_address, port, session_id, connect_id, success, error}` |
| CM_EXE_CMD_LINE | 1806 | C→M | `cm.ExecutableCmdLine{exe_cmd_line}` |
| CM_SERVER_SETTINGS | 1808 | C→M | `cm.ServerSettings{gcsettings}` |
| CM_CLIENT_PLATFORM | 1809 | C→M | `cm.ClientPlatform{name}` WebRtc=6 / WebRtcIOS=8 |
| MC_AUTH_SUCCESS / MC_AUTH_FAILED | 2049 / 2050 | M→C | none / `mc.AuthFailed{reason}` |
| MC_ZONE_SERVER_LIST | 2051 | M→C | `mc.ZoneServerList{servers[{host,port,server_id,dns_name,public_dns_name}]}` |
| MC_DIRECTION_PLAY | 2053 | M→C | `mc.DirectionPlay{…}` → connect to game server (§5) |
| MC_VALIDATE_VERSION_RESPONSE | 2054 | M→C | `mc.ValidateVersionResponse{version,is_critical,url_update,language,…}` |
| MC_IN_SERVER_QUEUE | 2059 | M→C | `mc.InServerQueue{header, messagePart1..4, queueNumberMessage}` |
| MC_LOADING_PROFILES | 2060 | M→C | `mc.LoadingProfiles{header,message,progress}` (loading-screen text) |
| MC_SUBSCRIPTION_PART_OF_DAY_EXPIRED | 2061 | M→C | show error |
| MC_CONNECT_SERVER_CONFIRMATION | 2062 | M→C | none → send M_BYE, close manager WS |
| MC_USER_QUEUE_POSITION | 2065 | M→C | `mc.UserQueuePosition{message:"…: N"}` |
| CS_AUTH | 769 | C→GS | `cs.Auth{id_session,id_user,server_name,token_session,id_connect}` |
| CS_STREAMS_STATUS | 783 | C→GS | `cs.StreamsStatus{flags}` |
| CS_RECONNECT | 787 | C→GS | none |
| CS_INPUT_INIT_V2 | 789 | C→GS (DataChannel) | `cs.InputInitV2{aes_message, version}` |
| CS_SDP_OFFER | 792 | C→GS | `cs.SdpOffer{type, sdp, reconfigurate_flag, server_ip}` |
| CS_ICE_CANDIDATE | 793 | C→GS | `IceCandidate{type, ice}` |
| SC_AUTH_SUCCESS | 1025 | GS→C | none |
| SC_PLAY | 1027 | GS→C | `sc.Play{…}` — **ignored** by the web client (native-transport ports) |
| SC_DEMO_PLAY_END, SC_EXIT_GAME, SC_GAME_PLAY_END, SC_TRIAL_TIME_FINISHED, SC_SUBSCRIPTION_LIMIT_TIME_EXPIRED, SC_SUBSCRIPTION_EXPIRED | 1028,1031,1048,1040,1052,1057 | GS→C | end the session (§5.7) |
| SC_INACTIVE_TIMEOUT / SC_CHANGE_CLIENT | 1030 / 1029 | GS→C | error overlays (timeout / duplicate client) |
| SC_OPEN_URL_IN_BROWSER | 1033 | GS→C | `sc.OpenUrlInBrowser{url}` → `window.open` |
| SC_LEFT_DEMO_TIME / SC_LEFT_GAME_TIME / SC_SUBSCRIPTION_LIMIT_TIME_LEFT / SC_SUBSCRIPTION_PART_OF_DAY_LEFT | 1037/1047/1055/1056 | GS→C | seconds left → HUD timer |
| SC_GAME_DISABLE_TIMER | 1050 | GS→C | stop the HUD timer |
| SC_DISCONNECT_STREAMS / SC_RECONNECT_STREAMS | 1038 / 1039 | GS→C | tear down / rebuild both peer connections (§5.5) |
| SC_SUBSCRIPTION_PART_OF_DAY_EXPIRED | 1058 | GS→C | `sc.SubscriptionPartOfDayEnd{…}` → error overlay with texts from message |
| SC_INPUT_REPORT | 1061 | GS→C (DataChannel) | `sc.InputReport{…}` (§7.4) |
| SC_GAME_WAS_LAUNCHED | 1065 | GS→C | analytics flag |
| SC_CURSOR | 1066 | GS→C (DataChannel) | `sc.Cursor{…}` (§7.6) |
| SC_GAME_SESSION_LAUNCHER / SC_GAME_SESSION_EVENT | 1067 / 1068 | GS→C | analytics (launcher id / EntranceToLauncher, GameStarted, GameClosed) |
| SC_VM_NAME | 1069 | GS→C | `sc.VmName{name}` (analytics) |
| SC_SESSION_EVENT | 1070 | GS→C | `SessionEvent{name,type,payload,icon}` type `native` → toast (icon 1 = "wait" else "check"), `gamecenter` → host page callback with `payload` URL (payment / plan modals), `session_end` → close |
| SC_GAME_UPDATE_BEGIN | 1071 | GS→C | `sc.GameUpdateBegin{text}` → loading tip (ignored once playing) |
| SC_WEBRTC_INPUT_REPORT | 1075 | GS→C (DataChannel) | `sc.WebRTCInputReport{token, sent_input_timestamp, report_delay_on_server_ms}` (§7.5) |
| SC_VM_REBOOT | 1076 | GS→C | `sc.VmReboot{is_shutdown}` → §5.5 |
| SC_SDP_ANSWER | 1081 | GS→C | `sc.SdpAnswer{type, sdp, control_token}` |
| SC_ICE_CANDIDATE | 1082 | GS→C | `IceCandidate{type, ice}` |
| SC_SEND_CLIPBOARD_DATA / CS_SEND_CLIPBOARD_DATA | 1034 / 3877 | DataChannel both | `ClipboardData{…}` (§7.7) |
| CS_MOUSE_SETTINGS | 3845 | C→GS (DataChannel) | `cs.MouseSettings{…}` (§7.3) |
| CS_INPUT_MESSAGE_V2 | 3880 | C→GS (DataChannel) | `cs.InputMessageV2{data}` (§8) |
| CS_INPUT_REPORT | 3881 | C→GS (DataChannel) | `cs.InputReport{…}` (§7.4) |

`StreamType`: `ST_Audio=1, ST_Video=2, ST_Inputs=3`. The web client only ever uses `ST_Video`
(carries audio too) and `ST_Inputs`.

---

## 4. Manager connection (`wss://host:port` from `play_url`)

Plain `new WebSocket(url)` — no sub-protocol, no headers. Ignore all messages while a
`M_CLIENT_BEFORE_CLOSE` is outstanding or the socket is closing, except `M_KEEP_ALIVE`,
`M_MESSAGE_NOT_CORRECT`, `M_UNSUPPORTED`, `M_CLIENT_BEFORE_CLOSE_CONFIRMATION`.

Sequence (game-stream mode):

```
open  ─► start 6 s keep-alive timer; start 30 s "manager didn't send play" watchdog (log only)
S: M_ACCEPT
C: M_CAPABILITIES { capabilities: ["UDP_INPUTS"] }
S: M_CAPABILITIES { … }                              (just logged)
C: CM_SET_LANGUAGE { language: RU|EN }               (from play_url `language`, default EN)
C: CM_SYSTEM_INFO  { system_params: [ {name:"monitor-width-1", value:"<round(screen.width*dpr)>"},
                                      {name:"monitor-height-1", value:"<round(screen.height*dpr)>"} ] }
                                                     (snapped to a standard WxH if within ±5 px; iOS: always 1280x720)
C: CM_VIDEO_SETTINGS { max_fps: VIDEO_FPS_AUTO(0), max_resolution: VIDEO_RESOLUTION_AUTO(0),
                       bitrate_min_kbps?, bitrate_max_kbps?, number_of_slices?, number_of_ref_frames? }
                                                     (optional fields only when user set them >0 in settings; value Mbit*1000)
C: CM_CLIENT_PLATFORM { name: WebRtc(6) | WebRtcIOS(8) }
C: CM_VALIDATE_VERSION { software:"webrtc-mailru", version:48103567 }
S: MC_VALIDATE_VERSION_RESPONSE { version, is_critical, url_update, language? }
       → if language set, switch UI language; (version mismatch is only logged)
C: CM_EXE_CMD_LINE { exe_cmd_line:"--exe-cmd-line=<play_url exe-cmd-line>" }   (only if present)
C: CM_SERVER_SETTINGS { gcsettings }                                            (only if present)
C: CM_AUTH_TOKEN { token, mode: game_stream(0) }     (SingleToken auth; GameToken auth sends nothing;
                                                      user/password auth sends CM_AUTH{login,password,game_id})
S: MC_AUTH_SUCCESS                                   (or MC_AUTH_FAILED{reason} → "bad authorization" error)
S: MC_ZONE_SERVER_LIST { servers[] }
C: (ping every server, §4.1) CM_ZONE_SERVER_PINGS { server_pings: [{ping:<µs or 2147483647>, server_id}] }
S: MC_IN_SERVER_QUEUE { header, messagePart1..3 ("NN text" → slice(3)), queueNumberMessage:"…: N" }   (optional, repeated)
S: MC_USER_QUEUE_POSITION { message:"…: N" }                                                            (optional, repeated)
S: MC_LOADING_PROFILES { message, progress }         (optional, repeated → loading tip)
S: MC_DIRECTION_PLAY { user_id, token_session, port, name, ip, session_id, connect_id, header?,
                       dns_name?, public_dns_name?, product_shipment_code?, zone_id?, video_adapter_id?, game_id? }
       → game-server host = public_dns_name ?? (dns_name ? "<second label of dns_name>.clgrtc.ru" : ip)
       → create both RTCPeerConnections (§6), open game-server WS (§5)
C: CM_CONNECT_SERVER { ip_address:<gs host>, port, session_id, connect_id:<play_token_id or 0>, success:true }
       (sent when the GS socket opens; on failure success:false + error text)
S: MC_CONNECT_SERVER_CONFIRMATION
C: M_BYE ; close manager WS with code 1000 "BYE"
```

`NA(dns)`: `"a.b.c" → "b.clgrtc.ru"` — i.e. take the second dot-separated label of `dns_name` and
append `.clgrtc.ru` (constant `DOMAIN`).

Errors: `M_ERROR.error_type` is mapped to overlays (`MC_AUTHORIZATION_FAILED`→bad auth,
`MC_ALL_SERVERS_BUSY`…, `SC_AUTH_DUPLICATE`→"duplicate client", `SC_CONTROLS_TIMEOUT`→"session timeout",
`SC_LARGE_LOSSES_UDP`→"low bandwidth", `MC_TOO_FREQUENT_CONNECTION`, `MC_NEED_UPDATE`, etc.; see
`ErrorType` in `proto/network.proto`). Unmapped codes show `title_message`/`user_message` from the
message.

### 4.1 Zone-server ping test (client side)

For every server in `MC_ZONE_SERVER_LIST` (in parallel):

1. `new WebSocket("wss://" + hostOf(server) + ":" + server.port)`; 5 000 ms connect timeout.
2. Send a 4-byte little-endian `uint32 messageId` (0,1,2,…). The server echoes the same 4 bytes.
   On each echo record RTT and immediately send the next id; also a 500 ms timer re-sends if no echo.
3. After 10 echoes: `ping = trunc(avg(rtt_ms) * 1000)` (microseconds). Failure/timeout → `2147483647`.
4. Close the socket.

Results are sorted ascending and sent as `CM_ZONE_SERVER_PINGS`.

### 4.2 Ping-test work mode (`work-mode=1`)

Same manager handshake but: `M_ACCEPT` → immediately `CM_VALIDATE_VERSION`; after
`MC_VALIDATE_VERSION_RESPONSE` → `CM_AUTH_TOKEN{mode: ping_test}`; server list → pings →
`CM_ZONE_SERVER_PINGS`; then the manager sends `M_BYE` → result OK. Any `M_ERROR`, socket error or
`MC_AUTH_FAILED` → result FAILED. No game-server connection.

---

## 5. Game-server connection (signalling)

`wss://<host from DirectionPlay>:<DirectionPlay.port>`. Same framing, keep-alive, ignore rules.

### 5.1 Handshake

```
open  ─► C: CS_AUTH { id_connect: connect_id, id_user: user_id, id_session: session_id,
                      server_name: name (bytes), token_session: token (bytes) }
         C: CM_CONNECT_SERVER on the manager socket (see §4)
S: SC_AUTH_SUCCESS
C: M_CAPABILITIES { steam_bigpicture:false }         (no capability strings)
S: M_CAPABILITIES { … }
C: if (needReconnection)  CS_RECONNECT               (socket re-opened after a drop: §5.4)
   else                   create & send SDP offers    (§6.2)  ─ video first, then inputs
S: SC_SDP_ANSWER { type: ST_Video,  sdp }
S: SC_SDP_ANSWER { type: ST_Inputs, sdp, control_token }   ← control_token is the input token (§7)
S/C: SC_ICE_CANDIDATE / CS_ICE_CANDIDATE { type, ice:<JSON of RTCIceCandidate> }  (trickle, any order)
S: M_SET_LOG_LEVEL { level }                          (optional; client then mirrors its log as M_LOG)
S: SC_VM_NAME, SC_GAME_SESSION_LAUNCHER, SC_GAME_UPDATE_BEGIN, …  (informational)
```

`M_ACCEPT` may also arrive on this socket; it is a no-op here.

### 5.2 `CS_STREAMS_STATUS`

Sent **once** when both peer connections reached `connected` **and** the `<video>` element fired
`loadeddata`: `flags = (videoAndAudioConnected ? 3 : 0) | (inputsConnected ? 4 : 0)` → normally `7`.
If the connections are up but no video frame arrives within 180 000 ms the client sends the flags
anyway and shows "UDP is not allowed" error. The flag `streamStatusWasSend` gates deferred
reconfiguration (§6.5).

### 5.3 Session end initiated by the client

`destroyApp()` (user pressed exit / page hide / host page callback):

1. disconnect both peer connections and stop statistics;
2. send `M_CLIENT_BEFORE_CLOSE` on every open WS (manager and/or GS), wait up to 2 000 ms for
   `M_CLIENT_BEFORE_CLOSE_CONFIRMATION` (any socket);
3. close sockets (`close(1000,"BYE")`), collect session result (§11), call host `onCloseApp(result)`.

`pagehide` also triggers this (with analytics sent via `sendBeacon`).

### 5.4 Reconnection of the signalling socket

If the GS WebSocket closes uncleanly (`!wasClean`) the client reconnects the same URL:
first silently, second time with a "reconnecting" modal, third time → "remote host not
responding" error. After the first successful SDP exchange `needReconnection = true`, so on
re-handshake (`SC_AUTH_SUCCESS` → capabilities) the client sends `CS_RECONNECT` instead of new
offers; the peer connections are kept. `needReconnection` resets to false on a deliberate disconnect.

### 5.5 Server-driven stream restarts

* `SC_DISCONNECT_STREAMS` → close both peer connections (`disconnectServices`).
* `SC_RECONNECT_STREAMS` → create fresh peer connections for both streams and send **both** SDP
  offers again (`reconfigurate_flag=false`).
* `SC_VM_REBOOT{is_shutdown}` → close both PCs, reset "streams status sent", create fresh PCs with a
  **180 000 ms** connect timeout (instead of 120 000), show "VM reboot" loading page, send both offers.

### 5.6 Manager socket after play

After `MC_CONNECT_SERVER_CONFIRMATION` the manager socket is gone; the game-server socket stays for
the whole session (keep-alive every 6 s).

### 5.7 Messages that end the session

`SC_EXIT_GAME`, `SC_DEMO_PLAY_END`, `SC_GAME_PLAY_END`, `SC_TRIAL_TIME_FINISHED`,
`SC_SUBSCRIPTION_LIMIT_TIME_EXPIRED`, `SC_SUBSCRIPTION_EXPIRED`, `SC_SESSION_EVENT{type:"session_end"}`
→ run §5.3. `SC_INACTIVE_TIMEOUT` → "session timeout" error overlay (retry/exit);
`SC_CHANGE_CLIENT` → "duplicate client" overlay. The payload texts of `*_END`/`*_EXPIRED` messages
(header/message/button labels/prices) are ignored by the web client; the host site shows its own UI.

---

## 6. WebRTC

### 6.1 Peer connections

Two `RTCPeerConnection({ iceServers: [{ urls: "stun:stun.l.google.com:19302" }] })`:

| Name | StreamType | Contents |
|---|---|---|
| `VideoAndAudioStream` | `ST_Video` (2) | `addTransceiver("video",{direction:"recvonly"})`; then either `addTrack(microphoneAudioTrack, micStream)` (if the user granted microphone) **or** `addTransceiver("audio",{direction:"recvonly"})` |
| `InputStream` | `ST_Inputs` (3) | `createDataChannel("InputStream", { ordered: false, maxPacketLifeTime: 1 })`, `binaryType="arraybuffer"`; no media |

Handlers:

* `onicecandidate`: only candidates with `candidate.protocol === "udp"` are sent, as
  `CS_ICE_CANDIDATE{type, ice: JSON.stringify(candidate)}` (the full `RTCIceCandidate` JSON:
  `{candidate, sdpMid, sdpMLineIndex, usernameFragment}`). TCP candidates are dropped.
* `SC_ICE_CANDIDATE{ice}` → `pc.addIceCandidate(JSON.parse(ice))` (errors only logged).
* `onconnectionstatechange`: `"connected"` (and previous state ≠ `"disconnected"`) → stop the
  connect timer and mark the service connected; `"failed"` → "remote host not responding" error.
* Connect timer: started when the offer is created; 120 000 ms (180 000 after VM reboot). Expiry →
  error overlay "remote host not responding".
* `ontrack` (video PC): `event.streams[0].id === "AudioStream"` → audio `MediaStream`, otherwise
  video `MediaStream` (server stream ids are literally `"VideoStream"` / `"AudioStream"`).
  `<video autoplay playsinline>` gets `srcObject = videoStream`; a separate `<audio>` gets the audio
  stream (iOS: both tracks merged into one MediaStream on one `<video>`).
  `loadeddata` on the video element ⇒ `videoStarted` (used for `CS_STREAMS_STATUS`).

### 6.2 Offer creation & SDP munging (video PC)

```
offer = await pc.createOffer()
sdp   = reorderH264Profiles(offer.sdp, preferredProfile)          // (a)
sdp   = sdp.replaceAll(
          /a=fmtp:(\d+) level-asymmetry-allowed=(\d);packetization-mode=(\d);profile-level-id=([0-9a-fA-F]+)/g,
          "a=fmtp:$1 level-asymmetry-allowed=$2;packetization-mode=$3;profile-level-id=$4\r\n" +
          "a=fmtp:$1 x-google-start-bitrate=<startKbps>\r\n" +
          "a=fmtp:$1 x-google-min-bitrate=<minKbps>\r\n" +
          "a=fmtp:$1 x-google-max-bitrate=<maxKbps>\r\n" +
          "a=fmtp:$1 sps-pps-idr-in-keyframe=1")                    // (b)
sdp   = sdp.replaceAll(/a=fmtp:(\d+) apt=(\d+)/g, "a=fmtp:$1 apt=$2;rtx-time=125")   // (c)
await pc.setLocalDescription({type:"offer", sdp})
sdp  += "cloudgaming:fps=<0|33|45|60|75|90|105|120>;streamwidth=<W>;streamheight=<H>;refframes=1;slices=1;rgbrange=<0|1|2>;"   // (d)
send CS_SDP_OFFER { type: ST_Video, sdp, reconfigurate_flag, server_ip: DirectionPlay.ip }
```

(a) Only H.264 payload types with `level-asymmetry-allowed=1;packetization-mode=1` are considered.
The `m=video` line's payload list is rewritten as: first 3 tokens, then the PT whose
`profile-level-id` maps to the preferred profile, then the other known H.264 PTs (in the
order High, ConstrainedHigh, Main, Base, ConstrainedBase), then remaining H.264 PTs (desc by
profile-level-id), then all non-H.264 PTs. Known `profile-level-id` values:
`64001f` High, `640c1f` ConstrainedHigh, `4d001f` Main, `42001f` Base, `42e01f` ConstrainedBase.
Preferred profile AUTO ⇒ High.
(b)/(c) are plain string replaces; the `cloudgaming:` suffix (d) is appended **after**
`setLocalDescription`, so the browser never sees it — it is a server-side hint (fps 0 = auto,
width/height 0 = auto, rgbrange 0 auto / 1 limited / 2 full).

The inputs offer is sent unmodified: `CS_SDP_OFFER{type: ST_Inputs, sdp, reconfigurate_flag:false, server_ip}`.

`SC_SDP_ANSWER{sdp}` → `pc.setRemoteDescription({type:"answer", sdp})`. For `ST_Inputs` also store
`control_token >>> 0` as the **input token**.

Order: the video offer is created and sent first (awaited), then the inputs offer. After the first
pair `needReconnection = true`.

### 6.3 Bitrate defaults

Mbit values (× 1000 → kbps). Defaults: min 1, max 25, start 8, hard cap 40. Max by resolution:
>1920×1080 → 32, >2560×1440 → 40; +3 for fps > 60, +6 for fps > 90. Start bitrate by stream width
(≥3800: 24/30/37, ≥2000: 17/21/25, ≥1600: 12/15/17, else 8/9/10 — the three numbers are for
fps ≤33 / ≤60 / >60). `streamwidth/height` = min(user-selected resolution, monitor size) with
aspect fix; resolution enum list: 1280×720, 1366×768, 1440×900, 1600×900, 1920×1080, 2048×1080,
2560×1440, 3840×2160, 4096×2160 (monitor size is snapped to the first entry that is ≥ size−32).
Max fps by OS: 120 (desktop/iOS), 60 (Android).

### 6.4 Reconfiguration (re-negotiating video only)

`reconfigurateVideoAndAudioStreams()`: close & recreate **only** the video PC, stop stats, send a
new video offer with `reconfigurate_flag = true`. The input PC is untouched. Triggered by:

* user changed video settings (codec/profile/resolution/fps/bitrate) — immediately, or deferred
  100 ms-polled until `CS_STREAMS_STATUS` has been sent;
* video freeze: 10 consecutive 1-second samples with received FPS ≤ 0.01 while inbound bitrate
  > 0.01 Mbit/s;
* performance analyzer (only in fullscreen, after ≥500 frames): decoded lag >10 frames, jitter
  buffer ≥1 s with no loss (or ≥3 s), ≥30 dropped frames or ≥5 PLIs in 5 s without loss → lower
  fps→resolution→profile→bitrate step by step (see `Yh.handleProblem`) then reconfigure; avg
  decode time ≥100 ms (min ≤50) or ≥500 ms → reconfigure as is.

### 6.5 Statistics

`pc.getStats()` every 1 s on both PCs (inbound-rtp video/audio, outbound audio, candidate-pair,
data-channel). Used for the HUD, freeze/performance logic above, and the session result. Input RTT
comes from `SC_WEBRTC_INPUT_REPORT` (§7.5).

---

## 7. Input channel (DataChannel "InputStream")

All messages on the channel use the §3 framing. The client **drops** any send while
`dataChannel.bufferedAmount > 21 000` bytes.

### 7.1 Init handshake

When the DataChannel opens:

```
C: CS_INPUT_INIT_V2 { aes_message: <4 bytes: input token as uint32 LE>, version: 2 }   every 100 ms
S: M_TOKEN_ACCEPTED   (type-only)  → stop the 100 ms timer, inputs are "started"
```

(Despite the name there is no AES; `aes_message` is just the raw token from `SC_SDP_ANSWER.control_token`.
`InputProtocolVersion.V_2 = 2`.) Before `M_TOKEN_ACCEPTED` the client only reacts to `M_ERROR` on
the channel; everything else is logged. `M_KEEP_ALIVE` may arrive on the channel; ignore.

After the token is accepted the client:

1. starts the clipboard service, the cursor service (default arrow cursor), and a **4 ms** process
   timer (§7.8);
2. sends `CS_MOUSE_SETTINGS` (§7.3) and re-sends it whenever the video element's rendered size
   changes;
3. when pointer lock/focus is acquired: reads the clipboard and sends it (§7.7), starts
   transmitting inputs. On blur / pointer-lock loss: sends a "release everything" message (§8.6)
   and stops transmitting.

DataChannel `close`/`error` → stop inputs (timers, listeners, clear pending state); the outer
connection error handling then shows an overlay.

### 7.2 Token check

Every server→client input message (`SC_INPUT_REPORT`, `SC_SEND_CLIPBOARD_DATA`, `SC_CURSOR`,
`SC_WEBRTC_INPUT_REPORT`) carries `token`; drop if ≠ input token.

### 7.3 `CS_MOUSE_SETTINGS` (cs.MouseSettings)

```
id            = incrementing counter (starts at 1, bumps on every size change)
token         = input token
sensitivity   = 1
acceleration  = 10
window_width  = trunc(rendered video width  in CSS px)   ← the letterboxed video rect, not the window
window_height = trunc(rendered video height in CSS px)
client_cursor = true      (client draws the cursor itself from SC_CURSOR)
```

Re-sent by the 4 ms loop while `lastSendId > lastConfirmedId` (confirmed via
`SC_INPUT_REPORT.last_mouse_settings_id`) subject to the redundancy backoff (§7.9).
`abs_x/abs_y` in mouse events are in this coordinate space (0..window_width/height).

### 7.4 Input reports

`SC_INPUT_REPORT` (server → client, frequent):

```
last_event_id            → transmitter.setLastConfirmedEventId(id)   (stops resending ≤ id, §8.5)
last_mouse_settings_id   → confirms CS_MOUSE_SETTINGS
clipboard_confirmed_id, clipboard_current_id, clipboard_current_received_ids[] → clipboard ack (§7.7)
```

`CS_INPUT_REPORT` (client → server) is sent **in response to** every `SC_CURSOR` and every
`SC_SEND_CLIPBOARD_DATA`:

```
token                         = input token
cursor_id                     = id of the cursor currently applied
cursor_last                   = highest Cursor.last seen
cursor_received_ids[]         = packet ids received for cursor_id
clipboard_confirmed_id        = last fully received server clipboard id
clipboard_current_id          = server clipboard id being received
clipboard_current_received_ids[] = packet ids received for it
```

### 7.5 `SC_WEBRTC_INPUT_REPORT` → input RTT

`rtt_ms = (nowMicros() − sent_input_timestamp) / 1000 − report_delay_on_server_ms` where
`nowMicros()` is the same monotonic microsecond clock used for input timestamps (§8.3). Negative → ignore.

### 7.6 `SC_CURSOR` — server cursor images

```
sc.Cursor { token, id, packets, packet_id, last, raw_size, payload, x?, y? }
```

A cursor image is split into `packets` chunks; collect by `packet_id`, when all are present
concatenate and **zlib-inflate** → cursor blob. `raw_size == 0` ⇒ hide cursor. Keep a map id→image;
the "current" cursor is the one whose message had the highest `last` (`last` is a monotonically
increasing change counter; `id` identifies the image). Reply `CS_INPUT_REPORT` (§7.4) for every packet.

Cursor blob formats (decoded by `InputCursorConverter`):

1. Windows **ANI** (`RIFF……ACON`) containing one `.cur` frame: hotspot X/Y at byte 86/88 (uint16),
   `BITMAPINFOHEADER` at 98 (biWidth @102, biHeight @106 = 2×height, biBitCount @112 = 32 or 1),
   pixel data at 138 (32-bpp BGRA XOR bitmap, bottom-up, followed by the 1-bpp AND mask; or
   1-bpp XOR+AND for monochrome cursors).
2. Raw: `uint16 hotspotX, uint16 hotspotY, BITMAPINFOHEADER(40 bytes), pixels…` (same pixel rules).

The converter emits a 32-bpp BMP (BITMAPV4/V5 header, 124-byte info header) with alpha; the web
client draws it on a canvas overlay at the local mouse position (client-side cursor) and ignores
`x`/`y`.

### 7.7 Clipboard

Client → server (`CS_SEND_CLIPBOARD_DATA`, cs.ClipboardData): on focus gain (pointer lock) the
client reads `navigator.clipboard.readText()`, UTF-8 encodes, **zlib-deflates**, splits into 1300-byte
chunks: `{token, id:<clipboard counter starting at 2>, packets_count, packet_id, raw_size:<uncompressed
bytes>, content:<chunk>}`. All not-yet-confirmed packets are re-sent from the 4 ms loop with the
redundancy backoff until `SC_INPUT_REPORT.clipboard_confirmed_id ≥ id` (per-packet acks via
`clipboard_current_received_ids`).
Server → client (`SC_SEND_CLIPBOARD_DATA`, sc.ClipboardData, same shape): collect packets by
`packet_id` for `id`, ignore ids ≤ current/confirmed, when complete inflate → UTF-8 →
`navigator.clipboard.writeText`. Always answer with `CS_INPUT_REPORT`.

### 7.8 The 4 ms process loop

Every 4 ms while inputs are started:

1. if mouse settings unconfirmed and backoff allows → re-send `CS_MOUSE_SETTINGS`;
2. if client clipboard unconfirmed and backoff allows → re-send its packets;
3. `transmitter.flush(false)` — re-send the latest input message if it is still unconfirmed and the
   backoff allows (§8.5).

### 7.9 Redundancy / backoff controller (used by 1–3 above)

```
state: lastSentId, resendCount, resendTimeout=20 ms, maxResendTimeout=40 ms, nextResendTime
isItExtraRedundantEvent(id) = (id == lastSentId) && now < nextResendTime      → skip sending
commitSentEvent(id): if id != lastSentId {lastSentId=id; resendCount=0} else resendCount++
                     nextResendTime = now + min(resendTimeout << resendCount, maxResendTimeout)
updateInterval(bufferedAmount): <6000 → (20, 40) ms ; ≥6000 → (50, 500) ms
```

i.e. a fresh message is resent after 20 ms, then 40 ms, 40 ms, … until confirmed (or 50/100/200/400/500
when the channel is congested).

### 7.10 Input rate limiting (mouse move & gamepad axes)

Both use one interval computed from the DataChannel `bufferedAmount` after each send
(`WA.updateInterval`): thresholds bytes→ms `{<3000: 0.01, <6000: 2, <9000: 5, <12000: 10, <15000: 15, <18000: 25, else 50}`,
with hysteresis: an increase is applied immediately (and held ≥10 s), decreases step down one level
(50→25→15→10→5→2→0.01) at most every 10 s; during the first 30 s the interval starts at 10 ms.
Mouse move deltas are accumulated and emitted at most once per interval; gamepad axes are sampled at
most once per interval (buttons/POV every animation frame).

---

## 8. `cs.InputMessageV2.data` — the binary input protocol (V2)

`CS_INPUT_MESSAGE_V2 { data: bytes }` where `data = zlib.deflate(raw)` (zlib stream, `78 9C` header,
default level). Below is `raw`. **Verified byte-exact** against the original WASM
(`reference/input_v2_encoder.js` / `input_v2_parser.js`).

### 8.1 Layout

```
struct RawMessage {
  u32   message_id;          // 1,2,3,… per message sent (counts every emitted message, incl. resends)
  u32   token;               // input token (SC_SDP_ANSWER.control_token)
  char  locale[8];           // last locale sent, NUL padded ("en-US", "ru-RU"); zeros before any locale event
  u8    lock_keys;           // keyboard lock state: 1 ScrollLock, 2 NumLock, 4 CapsLock
  u8    device_mask;         // bit0 keyboard, bit1 mouse, bit2..5 gamepad 0..3, bit6 touch
  // state blocks, present iff the bit is set in device_mask, in this order:
  [u8 kbd_pressed[30]]       // 240-bit bitmap of currently pressed DIK codes (bit = key&0xFF; keys ≥240 never set)
  [u16 mouse_buttons]        // bitmask, bit n = button n pressed (n ≤ 15)
  [struct Gamepad {          // one per gamepad bit set (index order 0..3)
     u16 buttons;            //   bitmask bit n = button n (n ≤ 15)  (n ≥16 corrupts axes[0] — never send)
     i32 axes[6];            //   PK_ABS_X, Y, Z, RX, RY, RZ  (−32768..32767 used)
     u8  pov;                //   (pov & 0xFF): 0 center, 1 north, 16 south  (east/west bits are lost)
  }]
  [u8 touch_down]            // 1 after a TOUCH_DOWN/MOVE… until TOUCH_UP, else 0
  u8    event_count;         // 0..20
  u32   newest_event_id;     // id of the newest event in this message
  u64   newest_timestamp_us; // its timestamp
  struct Event {             // × event_count, NEWEST FIRST
     i32 ts_delta;           // newest_timestamp_us − this.timestamp  (0 for the first; may be negative)
     u8  kind;               // see §8.2 (gamepad kinds carry index/type in the high bits)
     u8  payload[];          // kind-specific
  } events[];
}
```

Device bits are sticky: once a device produced an event its bit stays set (and its block is
emitted) for the lifetime of the transmitter. State blocks describe the state **as of the newest
event in the message** (matters only for chunked backlogs, §8.5). `event_count == 0` never happens
(a flush with nothing to send emits nothing); `flush(true)` always has ≥1 event (§8.6).

### 8.2 Event kinds and payloads

| kind | name | payload | notes |
|---|---|---|---|
| `0x01` | KEY_PRESS | `u8 key, u8 lock_keys` | key = DIK scan code & 0xFF (§9.1) |
| `0x02` | KEY_RELEASE | `u8 key, u8 lock_keys` | |
| `0x03` | MOUSE_MOVE | `i16 dx, i16 dy, i16 abs_x, i16 abs_y` | relative + absolute (window space of §7.3) |
| `0x04` | MOUSE_SCROLL | `i16 dz` | value = `wheel_steps × 120` (client sends ±1 step ⇒ ±120) |
| `0x05` | MOUSE_PRESS | `u8 button` | §9.2 ids |
| `0x06` | MOUSE_RELEASE | `u8 button` | |
| `0x07` | GAMEPAD_PRESS | `u8 button` | kind byte = `(index<<6) \| (type<<4) \| 0x07` |
| `0x08` | GAMEPAD_RELEASE | `u8 button` | same encoding |
| `0x09` | GAMEPAD_AXIS | `u8 axis, i32 value` | same encoding; axis 0..5 |
| `0x0A` | RELEASE_ALL | — | marker emitted by "release everything" (§8.6); also what SLIDER/VECTOR/AXIS_RELEASE degrade to (never used) |
| `0x0B` | GAMEPAD_POV | `u8 pov_id(0), u32 value` | value = N 1, S 16, E 256, W 4096 or OR-combos (NE 257, SE 272, NW 4097, SW 4112), 0 center |
| `0x0C` | LOCALE | `char[8]` | NUL-padded, e.g. `"en-US"`; timestamp = timestamp of the previous event (0 initially) |
| `0x0D` | TOUCH_DOWN | `u8 is_absolute, u8 0, i16 x, i16 y` | x/y = absVideo(x,y) if absolute else relVideo(x,y); pointer id is **not** transmitted |
| `0x0E` | TOUCH_UP | same | |
| `0x0F` | TOUCH_MOVE | same | |

Gamepad kind byte: bits 7-6 gamepad index (device id − 20, 0..3), bits 5-4 gamepad type
(`0 D_INPUT, 1 X_INPUT, 2 DS4, 3 DS4_ANDROID`), bits 3-0 base kind. The browser client always uses
`X_INPUT` (1) ⇒ `0x17/0x18/0x19/0x1B` for gamepad 0, `0x57…` for gamepad 1, `0x97…`, `0xD7…`.
Keyboard, mouse and touch device ids (10, 0, 30) are **not** encoded.

Truncation: all i16 fields are the low 16 bits of the int, `key`/`button`/`axis` the low 8 bits,
`ts_delta` the low 32 bits of the signed 64-bit difference.

### 8.3 Timestamps

`u64` microseconds from `clock_gettime(CLOCK_MONOTONIC)` (Emscripten: `performance.now()*1000`
truncated). Any monotonic µs clock works — the server only uses deltas and echoes the value in
`SC_WEBRTC_INPUT_REPORT.sent_input_timestamp` for RTT (§7.5).

### 8.4 Event ids

Every event gets `id = ++counter` (1-based, never reset except by `clear()`). Messages carry only
`newest_event_id`; older ids in the same message are implied by order (`newest_event_id − i`).

### 8.5 Flushing, batching, retransmission

The transmitter keeps an ordered list `pending` of events with `id > lastConfirmedEventId`
(`SC_INPUT_REPORT.last_event_id`). A **flush** (`flush(sendHeld=false)`) does:

```
U = pending (oldest → newest)
p = index of the first event that has never been sent
while (len(U) − p) > 20:   emit message with U[p .. p+19];  p += 20      // full 20-event chunks of new events
emit message with the last min(20, len(U)) events of U                     // always, even if nothing is new
mark all of U as sent
```

So a normal message = "the ≤20 newest unconfirmed events", newest first, and every flush
re-transmits them (redundancy) until the server confirms. In the JS client:

* every single input event is followed immediately by `flush(false)` — i.e. one message per event
  (a DOM event producing several transitions, or a gamepad frame with several changes, produces
  several messages in a row, each containing all still-unconfirmed events);
* the 4 ms loop calls `flush(false)` again, but only if the newest id is unconfirmed **and** the
  backoff of §7.9 has expired (`isItExtraRedundantEvent(lastEventId)`), then `commitSentEvent`;
* if the DataChannel buffer exceeds 21 000 bytes the send is dropped (the event stays pending).

`empty()` is true when there are no unconfirmed events. Events are only recorded while
`setCollectEventsFlag(true)`; the JS toggles it on around each `send*Event` and off before `flush`
(a bare `sendKeyboardEvent` with the flag off is discarded).

### 8.6 "Release everything" (`flush(true)`)

Called when the client loses focus/pointer lock, toggles the virtual keyboard/mouse, when the
last gamepad disconnects, or on stop. It appends synthetic events (all with the current timestamp)
in this order, then does a normal flush:

1. `KEY_RELEASE` for each pressed key, in **press order** (the transmitter keeps a press list; a key
   pressed twice without release yields two entries; a re-press moves it to the end);
2. `MOUSE_RELEASE` for each pressed mouse button, press order;
3. for gamepads, one chronological list across all pads: `GAMEPAD_RELEASE` per pressed button
   (press order, duplicates possible) and `GAMEPAD_POV 0` for a pad whose POV is non-center (inserted
   at the position of its first non-center change); axes are **not** reset;
4. `RELEASE_ALL` (0x0A);
5. `TOUCH_UP` (payload copied from the last TOUCH_DOWN) if a touch is down.

State bitmaps/masks are cleared accordingly (`kbd_pressed` = 0, `mouse_buttons` = 0, gamepad buttons
= 0, pov = 0, touch_down = 0; axes keep their values). If nothing is held the message contains just
`RELEASE_ALL`.

### 8.7 Other transmitter API facts (from embind)

`clear()` resets message/event counters and all state (used when inputs stop);
`lastEventId()`, `empty()`, `setLastConfirmedEventId(id)`, `setCollectEventsFlag(bool)`,
`flush(sendHeld:bool, dryRun:bool /*always false; true suppresses sending*/, token:u32)`,
`sendKeyboardEvent/sendMouseEvent/sendJoystickEvent/sendTouchEvent/sendLocaleEvent(obj)`,
`timestamp()`. Constants: `KEYBOARD_DEV_ID_OFFSET=10, MOUSE_DEV_ID_OFFSET=0, JOYSTICK_DEV_ID_OFFSET=20,
TOUCHSREEN_DEV_ID_OFFSET=30`. Enum values: KeyboardEventType PRESS 0/RELEASE 1; MouseEventType MOVE 0,
SCROLL 1, BUTTON_PRESS 2, BUTTON_RELEASE 3; JoystickEventType BUTTON_PRESS 0, BUTTON_RELEASE 1,
AXIS_MOVE 2, SLIDER_MOVE 3, POV_MOVE 4, VECTOR3D_MOVE 5, AXIS_RELEASE 6; TouchEventType DOWN 0, UP 1,
MOVE 2; JoystickType NONE −1 (asserts), D_INPUT 0, X_INPUT 1, DS4 2, DS4_ANDROID 3.

### 8.8 Worked example

Press `A` (DIK 0x1E) with no locks, token `0x11223344`, ts 1000 µs, first message:

```
01 00 00 00                       message_id 1
44 33 22 11                       token
00×8                              locale (none yet)
00                                lock_keys
01                                device_mask: keyboard
00 00 00 40 00×26                 kbd bitmap: bit 30 set (byte 3, bit 6)
01                                event_count
01 00 00 00                       newest_event_id 1
E8 03 00 00 00 00 00 00           timestamp 1000
00 00 00 00  01 1E 00             delta 0, KEY_PRESS, key 0x1E, lock 0
```
zlib-deflate that (68 bytes → ~33) and wrap as `[len][type=3880][bytes]` on the DataChannel.

---

## 9. Input semantics (what the browser client sends and when)

Inputs are captured only while `focusFlag` is true: pointer lock is held (`requestPointerLock` on the
stream container after fullscreen; iOS: touch focus) and the window has focus. Losing either →
§8.6 and stop; regaining → re-read clipboard, send lock-key sync, resume.

### 9.1 Keyboard

`keydown`/`keyup` on `window` (capture). `event.code` → DirectInput scan code (`PK_KEY_*`,
table below); unknown codes map to `PK_KEY_UNASSIGNED = 0`. Auto-repeat is suppressed (a key already
in the pressed set is ignored). `preventDefault` for Tab, Shift+Esc, Safari+Meta in fullscreen.
Escape is sent normally (it additionally shows a "hold Esc" hint overlay while held in fullscreen).
Client-side hotkey combos: Alt+F4 → exit fullscreen; Ctrl+F1 (Mac ⌘+F1) → stats console — the
combo's key transitions are rolled back so they are not transmitted. On macOS: Meta is never sent,
and pressing Meta synthesizes key-ups for all other non-modifier keys.

For every transition the client emits `KEY_PRESS`/`KEY_RELEASE` with `lock_keys` = current
`getModifierState` of CapsLock(4)/NumLock(2)/ScrollLock(1), XOR-ed with the lock key being pressed
(so the server sees the *new* state on keydown). **Lock-state sync**: whenever a sync is needed
(inputs start, virtual keyboard toggled, CapsLock on Mac) the client emits a press **and** a release
of key `0` (PK_KEY_UNASSIGNED) carrying the current `lock_keys` — the server uses that pair only to
apply lock state.

**Locale**: before the key events of a DOM event the client determines the layout from
`event.key`: Cyrillic letters (U+0410–U+044F, Ё/ё) → `"ru-RU"`, Latin letters → `"en-US"`, a table of
punctuation per layout (`md`), modifier/navigation keys keep the previous locale; if it differs
from the last sent locale a `LOCALE` event is emitted first (initial value `"unspecified"`, so the
first key always produces one). The server switches the VM keyboard layout accordingly.

`code` → DIK (PK_KEY_*) table:

```
Escape 1  Digit1..9 2..10  Digit0 11  Minus 12  Equal 13  Backspace 14  Tab 15
KeyQ 16 KeyW 17 KeyE 18 KeyR 19 KeyT 20 KeyY 21 KeyU 22 KeyI 23 KeyO 24 KeyP 25
BracketLeft 26 BracketRight 27 Enter 28 ControlLeft 29
KeyA 30 KeyS 31 KeyD 32 KeyF 33 KeyG 34 KeyH 35 KeyJ 36 KeyK 37 KeyL 38 Semicolon 39 Quote 40
Backquote 41 ShiftLeft 42 Backslash|IntlBackslash 43
KeyZ 44 KeyX 45 KeyC 46 KeyV 47 KeyB 48 KeyN 49 KeyM 50 Comma 51 Period 52 Slash 53 ShiftRight 54
NumpadMultiply 55 AltLeft 56 Space 57 CapsLock 58 F1..F10 59..68 NumLock 69 ScrollLock 70
Numpad7 71 Numpad8 72 Numpad9 73 NumpadSubtract 74 Numpad4 75 Numpad5 76 Numpad6 77 NumpadAdd 78
Numpad1 79 Numpad2 80 Numpad3 81 Numpad0 82 NumpadDecimal 83 PrintScreen 86(OEM_102!) F11 87 F12 88
NumpadEqual 141 KanaMode 112 Lang2 148 Lang1 125 MediaTrackPrevious 144 MediaTrackNext 153
NumpadEnter 156 ControlRight 157 AudioVolumeDown|VolumeDown 174 AudioVolumeUp|VolumeUp 176
BrowserHome 178 NumpadDivide 181 AltRight 184 Pause 197 Home 199 ArrowUp 200 PageUp 201
ArrowLeft 203 ArrowRight 205 End 207 ArrowDown 208 PageDown 209 Insert 210 Delete 211
MetaLeft|OSLeft 219 MetaRight|OSRight 220 ContextMenu 221 Power 222 BrowserSearch 229
BrowserFavorites 230 BrowserRefresh 231 BrowserStop 232 BrowserForward 233 BrowserBack 234
```
(Full `PK_KEY_*` enum incl. F13–F15 100–102, media keys 160–237: see `Ed` in the bundle; all fit in 8 bits.)

### 9.2 Mouse

* Movement: `pointerrawupdate` (fallback `mousemove`) with pointer lock → `movementX/Y` are
  accumulated and emitted at the rate-limit interval (§7.10) as `MOUSE_MOVE{dx,dy,abs_x,abs_y}` where
  `abs` is a client-maintained virtual position clamped to `[0,window_width]×[0,window_height]`
  (the rendered video rect; rescaled proportionally when the rect changes). Server uses `dx/dy`
  for relative (raw-input) games and `abs` for cursor positioning.
* Buttons: `mousedown`/`mouseup` on `window`; browser `button` → Playkey id:
  `0→0 LEFT, 1→2 MIDDLE, 2→1 RIGHT, 3→6 BACK, 4→5 FORWARD` (ids: LEFT 0, RIGHT 1, MIDDLE 2, SIDE 3,
  EXTRA 4, FORWARD 5, BACK 6, EXTRA2 7, EXTRA3 8). Duplicate down/up is filtered; each transition is
  emitted as `MOUSE_PRESS`/`MOUSE_RELEASE` immediately.
* Wheel: `wheel` → `sign(−deltaY)` (±1 step, horizontal ignored) → `MOUSE_SCROLL{dz = ±120}`.
* Context menu is `preventDefault`-ed. iOS/touch: pointer events on the video emulate an absolute
  mouse (tap = left button, drag = move) with double-tap slop logic.
* Virtual mouse (gamepad-driven) exists for gamepad-only users: right stick moves, A/B click.

### 9.3 Gamepad

`gamepadconnected`/`gamepaddisconnected` + `navigator.getGamepads()` polled every
`requestAnimationFrame` while ≥1 pad is connected (max 4, index < 4). All pads are treated as
**XInput** (`type = X_INPUT`); non-"standard" mappings only produce a warning toast. Device id =
`20 + gamepad.index`.

Per frame, for each pad (standard mapping):

* Buttons 0..11 → Playkey button ids `A 0, B 1, X 2, Y 3, LB 4, RB 5, LT 6, RT 7, BACK 8, START 9,
  LSTICK 10, RSTICK 11` (identical to the standard-mapping index). Each `pressed` transition →
  `GAMEPAD_PRESS/RELEASE{button}`. **LT/RT (6,7) are never sent as buttons**; their analog `value`
  goes to axes Z (2) and RZ (5).
* Buttons 12..15 (D-pad ↑↓←→) → POV bitmask `N 1, S 16, W 4096, E 256` OR-ed; on change →
  `GAMEPAD_POV{pov_id 0, value}`.
* Axes: `axes[0]→X(0), [1]→Y(1), [2]→RX(3), [3]→RY(4)`, plus `LT→Z(2)`, `RT→RZ(5)`.
  Value scaling: `v > 0 ? v*32767 : v*32768`, truncated to int. Emitted as `GAMEPAD_AXIS{axis,value}`
  only when the value changed and only at the rate-limit interval (§7.10). No dead-zone is applied
  (8192 is only used to decide which pad is "current" for UI). Y axes are sent as-is (browser
  convention, down = +).
* Per frame the order is: axes (axis order), then POV, then buttons — each as its own message.

`is_ps` (id contains 054c/sony/playstation/dualshock/dualsense) only changes on-screen glyphs.
Gamepad hotkeys consumed client-side: LB+RB+BACK → virtual keyboard, LB+RB+START → virtual mouse.
When the last pad disconnects the client sends "release everything" (§8.6).

### 9.4 Touch (iOS)

`TOUCH_*` events are only used by the iOS build for the virtual on-screen keyboard/gamepad
overlay; the Playkey server receives `is_absolute` + video-relative or absolute video coordinates.
Desktop clients never send them. Multi-touch is not representable (no pointer id).

---

## 10. Native-transport messages you will see but can ignore

Because the same server code serves the native Playkey client, the web client also receives
`SC_PLAY` (UDP ports/tokens for the native video/audio/control streams — ignored), and the schema
contains `CS_WAIT_PLAY{webrtc_stream_settings[]}`, `SC_WEBRTC_SETTINGS`, `SC_RECONNECT_INPUT_STREAM`,
`SC_QUALITY_VERDICT`, `CS_USER_ASSESSMENT`, USB/printer/WebDAV redirection, `punch.proto` (UDP hole
punching: `MT_Hello{my_code, ep_code, stream_type}`, `MT_Punch/PunchAnswer{token}`) and
`remoteplay.proto` — none are used by the WebRTC client. `WebRtcStreamSettings{type, ice, offer}`
inside `sc.Play` is unused in the wild (the answer comes as `SC_SDP_ANSWER`).

---

## 11. Session result & telemetry (optional)

On close the host page POSTs `updateGameSessionResult(session_id, {...})` with:
`session_id, user_id, time_session_length (s since both PCs connected), closed_by_user 0/1,
lost_frames %, ping (avg candidate-pair RTT ms), avg_network_bitrate Mbit/s, decode_time ms,
wifi 0, bad_lost_percent 0, detect_slow_pc 0, bad_performance_problem 0, launcher_id,
game_not_launched_after_launcher 0/1, max_monitor_resolution "WxH", gamepad_presence 0/1,
errors[{title,message,type:"Internal",initiator:"MAILRU"}], platform`.
The player also POSTs JSON log lines to `https://lambda.cloud.vkplay.ru:1049/` (Kibana) and, if
the server sent `M_SET_LOG_LEVEL`, mirrors log lines as `M_LOG{msg, level}` on the GS socket.
None of this is required for streaming.

---

## 12. Minimal custom client — checklist

1. `POST /api/game_launchers/{id}/queue`, poll `GET /api/queue` until `allowed`
   (run the ping-test session first if `is_ping_ready` is false), then
   `POST /api/game_launchers/{id}/run/ {"type":"webrtc_client"}` → `play_url`.
2. Parse `play_url`; open manager WSS; implement §3 framing with the protobufs from `proto/`.
3. Manager handshake §4 (capabilities `["UDP_INPUTS"]`, language, system info, video settings,
   platform `WebRtc`, validate version `webrtc-mailru/48103567`, `CM_AUTH_TOKEN`); ping the zone
   servers (§4.1); handle queue messages; on `MC_DIRECTION_PLAY` open the GS WSS and send
   `CM_CONNECT_SERVER`; on confirmation `M_BYE` + close.
4. GS handshake §5.1: `CS_AUTH`, wait `SC_AUTH_SUCCESS`, exchange `M_CAPABILITIES`, then two
   `RTCPeerConnection`s (§6.1), video offer with the SDP munging (§6.2) then inputs offer; apply
   answers; trickle UDP ICE candidates both ways (JSON-encoded).
5. When the DataChannel opens: `CS_INPUT_INIT_V2{aes_message: token LE u32, version 2}` every 100 ms
   until `M_TOKEN_ACCEPTED`; then `CS_MOUSE_SETTINGS`; send `CS_STREAMS_STATUS{7}` once video plays.
6. Encode inputs per §8 (use `reference/input_v2_encoder.js` as the spec), zlib-deflate, send as
   `CS_INPUT_MESSAGE_V2`; confirm with `SC_INPUT_REPORT.last_event_id`; resend with the §7.9 backoff;
   answer `SC_CURSOR`/`SC_SEND_CLIPBOARD_DATA` with `CS_INPUT_REPORT`; send "release everything" on
   focus loss.
7. Keep `M_KEEP_ALIVE` every 6 s on open WebSockets; handle the session-ending `SC_*` messages
   (§5.7), `SC_DISCONNECT_STREAMS/RECONNECT_STREAMS/VM_REBOOT` (§5.5) and reconnect the GS socket with
   `CS_RECONNECT` (§5.4). Close with `M_CLIENT_BEFORE_CLOSE` → confirmation → `close(1000,"BYE")`.

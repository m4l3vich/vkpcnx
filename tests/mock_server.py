#!/usr/bin/env python3
"""Local stand-in for the VK Play Cloud manager + game server (protocol doc
§3–§8), backed by aiortc for the WebRTC side. Lets the client be exercised
end-to-end (signalling, ping test, SDP/ICE, H.264/Opus media, the input
DataChannel) without a real account.

    pip install aiortc websockets protobuf av numpy
    protoc -I vkpcnx/proto --python_out=<pbdir> vkpcnx/proto/*.proto
    python tests/mock_server.py --pb <pbdir> --cert cert.pem --key key.pem

then run the client with
    VKPCNX_INSECURE_TLS=1 VKPCNX_PLAY_URL='playkey:///?host=localhost&port=19000&token=test' ./vkpcnx
"""
import argparse
import asyncio
import fractions
import logging
import ssl
import struct
import sys
import time
import zlib

import numpy as np
import websockets
from aiortc import (RTCPeerConnection, RTCSessionDescription, RTCRtpSender, VideoStreamTrack,
                    AudioStreamTrack)
from aiortc.sdp import candidate_from_sdp
from av import VideoFrame, AudioFrame

log = logging.getLogger("mock")

# ---- framing ----------------------------------------------------------------

def frame(msg_type, payload=b""):
    return struct.pack("<II", 4 + len(payload), msg_type) + payload

def unframe(data):
    length, msg_type = struct.unpack_from("<II", data)
    assert length + 4 == len(data), f"bad frame length {length} vs {len(data)}"
    return msg_type, data[8:]

def load_protos(pbdir):
    sys.path.insert(0, pbdir)
    global pc, cm, mc, cs, sc
    import protocol_common_pb2 as pc
    import client_manager_pb2 as cm
    import manager_client_pb2 as mc
    import client_server_pb2 as cs
    import server_client_pb2 as sc

def tname(t):
    try:
        return pc.MessageType.Name(t)
    except ValueError:
        return str(t)

# ---- input protocol V2 parser (§8) -------------------------------------------

KIND_NAMES = {1: "KEY_PRESS", 2: "KEY_RELEASE", 3: "MOUSE_MOVE", 4: "MOUSE_SCROLL", 5: "MOUSE_PRESS",
              6: "MOUSE_RELEASE", 7: "GAMEPAD_PRESS", 8: "GAMEPAD_RELEASE", 9: "GAMEPAD_AXIS",
              10: "RELEASE_ALL", 11: "GAMEPAD_POV", 12: "LOCALE", 13: "TOUCH_DOWN", 14: "TOUCH_UP",
              15: "TOUCH_MOVE"}
PAYLOAD_SIZES = {1: 2, 2: 2, 3: 8, 4: 2, 5: 1, 6: 1, 7: 1, 8: 1, 9: 5, 10: 0, 11: 5, 12: 8, 13: 6, 14: 6, 15: 6}

def parse_input_message(raw):
    off = 0
    message_id, token = struct.unpack_from("<II", raw, off); off += 8
    locale = raw[off:off + 8].rstrip(b"\0").decode("ascii", "replace"); off += 8
    lock_keys, device_mask = struct.unpack_from("<BB", raw, off); off += 2
    state = {}
    if device_mask & 1:
        state["keys"] = [i for i in range(240) if raw[off + i // 8] >> (i % 8) & 1]
        off += 30
    if device_mask & 2:
        state["mouse_buttons"], = struct.unpack_from("<H", raw, off); off += 2
    for pad in range(4):
        if device_mask & (4 << pad):
            buttons, = struct.unpack_from("<H", raw, off); off += 2
            axes = struct.unpack_from("<6i", raw, off); off += 24
            pov = raw[off]; off += 1
            state[f"pad{pad}"] = (buttons, axes, pov)
    if device_mask & 0x40:
        state["touch_down"] = raw[off]; off += 1
    event_count = raw[off]; off += 1
    newest_id, newest_ts = struct.unpack_from("<IQ", raw, off); off += 12
    events = []
    for i in range(event_count):
        delta, kind = struct.unpack_from("<iB", raw, off); off += 5
        base = kind & 0x0F
        size = PAYLOAD_SIZES.get(base, 0)
        payload = raw[off:off + size]; off += size
        events.append((newest_id - i, delta, kind, payload.hex()))
    assert off == len(raw), f"trailing bytes {len(raw) - off}"
    return dict(message_id=message_id, token=token, locale=locale, lock_keys=lock_keys,
                device_mask=device_mask, state=state, newest_id=newest_id, newest_ts=newest_ts,
                events=events)

# ---- media tracks ------------------------------------------------------------

class PatternVideoTrack(VideoStreamTrack):
    """Moving colour bars + counter, 30 fps."""
    def __init__(self, width=1280, height=720):
        super().__init__()
        self.width, self.height = width, height
        self.n = 0

    async def recv(self):
        pts, time_base = await self.next_timestamp()
        img = np.zeros((self.height, self.width, 3), np.uint8)
        bars = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]
        w = self.width // len(bars)
        shift = (self.n * 4) % self.width
        for i, c in enumerate(bars):
            x0 = (i * w + shift) % self.width
            img[:, x0:min(x0 + w, self.width)] = c
        y = (self.n * 3) % (self.height - 40)
        img[y:y + 40, :] = (32, 32, 32)
        self.n += 1
        f = VideoFrame.from_ndarray(img, format="bgr24")
        f.pts, f.time_base = pts, time_base
        return f

class ToneAudioTrack(AudioStreamTrack):
    """440 Hz sine, 48 kHz stereo, 20 ms frames."""
    def __init__(self):
        super().__init__()
        self.sample_rate = 48000
        self.samples = 960
        self.pos = 0
        self._start = None

    async def recv(self):
        if self._start is None:
            self._start = time.time()
        # pace to real time
        target = self._start + self.pos / self.sample_rate
        wait = target - time.time()
        if wait > 0:
            await asyncio.sleep(wait)
        t = (np.arange(self.samples) + self.pos) / self.sample_rate
        tone = (np.sin(2 * np.pi * 440 * t) * 0.2 * 32767).astype(np.int16)
        stereo = np.repeat(tone[np.newaxis, :], 2, axis=0).T.reshape(1, -1)  # interleaved
        frame = AudioFrame.from_ndarray(stereo, format="s16", layout="stereo")
        frame.sample_rate = self.sample_rate
        frame.pts = self.pos
        frame.time_base = fractions.Fraction(1, self.sample_rate)
        self.pos += self.samples
        return frame

# ---- servers -----------------------------------------------------------------

class MockCloud:
    def __init__(self, args):
        self.args = args
        self.control_token = 0x11223344
        self.session_id = 5551
        self.connect_id = 0
        self.user_id = 42
        self.stats = dict(input_messages=0, input_events=0, mouse_settings=0, reports=0)

    # -- manager (§4) --
    async def manager(self, ws):
        log.info("manager: client connected")
        await ws.send(frame(pc.M_ACCEPT))
        got_pings = False
        ping_test_mode = False
        async for data in ws:
            t, payload = unframe(data)
            if t == pc.M_KEEP_ALIVE:
                continue
            log.info("manager: <- %s (%d bytes)", tname(t), len(payload))
            if t == pc.M_CAPABILITIES:
                caps = pc.Capabilities(); caps.ParseFromString(payload)
                log.info("manager: capabilities %s", list(caps.capabilities))
                reply = pc.Capabilities(); reply.capabilities.append("UDP_INPUTS")
                await ws.send(frame(pc.M_CAPABILITIES, reply.SerializeToString()))
            elif t == pc.CM_SYSTEM_INFO:
                info = pc.SystemInfo(); info.ParseFromString(payload)
                log.info("manager: system info %s", [(p.name, p.value) for p in info.system_params])
            elif t == pc.CM_VALIDATE_VERSION:
                vv = cm.ValidateVersion(); vv.ParseFromString(payload)
                log.info("manager: client %s/%d", vv.software, vv.version)
                r = mc.ValidateVersionResponse(); r.version = vv.version; r.is_critical = False
                await ws.send(frame(pc.MC_VALIDATE_VERSION_RESPONSE, r.SerializeToString()))
            elif t == pc.CM_AUTH_TOKEN:
                a = cm.AuthToken(); a.ParseFromString(payload)
                log.info("manager: auth token=%s mode=%d", a.token, a.mode)
                ping_test_mode = a.mode == cm.AuthToken.PING_TEST
                await ws.send(frame(pc.MC_AUTH_SUCCESS))
                lst = mc.ZoneServerList()
                for i in range(2):
                    s = lst.servers.add()
                    s.host = "127.0.0.1"; s.port = self.args.ping_port + i; s.server_id = 100 + i
                    s.public_dns_name = "localhost"
                await ws.send(frame(pc.MC_ZONE_SERVER_LIST, lst.SerializeToString()))
            elif t == pc.CM_ZONE_SERVER_PINGS:
                p = cm.ZoneServerPings(); p.ParseFromString(payload)
                log.info("manager: pings %s", [(x.server_id, x.ping) for x in p.server_pings])
                got_pings = True
                if ping_test_mode:
                    await ws.send(frame(pc.M_BYE, pc.Bye(reason="ping test done").SerializeToString()))
                    continue
                q = mc.UserQueuePosition(message="Место в очереди: 1")
                await ws.send(frame(pc.MC_USER_QUEUE_POSITION, q.SerializeToString()))
                lp = mc.LoadingProfiles(header="Загрузка", message="Загрузка профиля", progress=50)
                await ws.send(frame(pc.MC_LOADING_PROFILES, lp.SerializeToString()))
                await asyncio.sleep(0.5)
                dp = mc.DirectionPlay()
                dp.user_id = self.user_id; dp.token_session = b"session-token"; dp.port = self.args.gs_port
                dp.name = b"mock-gs"; dp.ip = "127.0.0.1"; dp.session_id = self.session_id
                dp.connect_id = self.connect_id; dp.public_dns_name = "localhost"; dp.game_id = 1
                await ws.send(frame(pc.MC_DIRECTION_PLAY, dp.SerializeToString()))
            elif t == pc.CM_CONNECT_SERVER:
                s = cm.ServerConnectionStatus(); s.ParseFromString(payload)
                log.info("manager: connect server %s:%d success=%s err=%s", s.ip_address, s.port, s.success, s.error)
                await ws.send(frame(pc.MC_CONNECT_SERVER_CONFIRMATION))
            elif t == pc.M_BYE:
                log.info("manager: client said bye")
            elif t == pc.M_CLIENT_BEFORE_CLOSE:
                await ws.send(frame(pc.M_CLIENT_BEFORE_CLOSE_CONFIRMATION))
        log.info("manager: closed (pings received=%s)", got_pings)

    # -- zone ping echo (§4.1) --
    async def ping_echo(self, ws):
        async for data in ws:
            await ws.send(data)

    # -- game server (§5) --
    async def game_server(self, ws):
        log.info("gs: client connected")
        video_pc = None
        inputs_pc = None
        channel = None
        input_state = dict(started=False, last_event_id=0, last_mouse_settings_id=0)

        async def send(t, payload=b""):
            await ws.send(frame(t, payload))

        def dc_send(t, msg=None):
            payload = msg.SerializeToString() if msg is not None else b""
            if channel and channel.readyState == "open":
                channel.send(frame(t, payload))

        async def cursor_task():
            # A 16x16 raw-format arrow-ish cursor, 32bpp, split in 2 packets (§7.6)
            w = h = 16
            hdr = struct.pack("<HH", 1, 1) + struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, w * h * 4, 0, 0, 0, 0)
            pixels = bytearray()
            for y in range(h - 1, -1, -1):  # bottom-up
                for x in range(w):
                    on = x <= y and x < 12 and y < 14
                    pixels += bytes([0, 0, 0, 255]) if on and (x == 0 or x == y or y == 13) else (bytes([255, 255, 255, 255]) if on else bytes([0, 0, 0, 0]))
            and_mask = bytes(((w + 31) // 32) * 4 * h)
            blob = hdr + bytes(pixels) + and_mask
            comp = zlib.compress(blob)
            half = (len(comp) + 1) // 2
            parts = [comp[:half], comp[half:]]
            await asyncio.sleep(1.0)
            for i, part in enumerate(parts):
                c = sc.Cursor(token=self.control_token, id=7, packets=2, packet_id=i, last=1, raw_size=len(blob), payload=part)
                dc_send(pc.SC_CURSOR, c)
            log.info("gs: cursor sent (%d bytes raw, %d compressed)", len(blob), len(comp))
            # clipboard text (§7.7)
            text = "hello from mock server".encode()
            comp = zlib.compress(text)
            d = sc.ClipboardData(id=3, token=self.control_token, packets_count=1, packet_id=0, raw_size=len(text), content=comp)
            dc_send(pc.SC_SEND_CLIPBOARD_DATA, d)

        def on_channel_message(data):
            t, payload = unframe(data)
            if t == pc.CS_INPUT_INIT_V2:
                init = cs.InputInitV2(); init.ParseFromString(payload)
                token, = struct.unpack("<I", init.aes_message)
                if token != self.control_token:
                    log.error("gs: bad input token %#x", token)
                    return
                if not input_state["started"]:
                    input_state["started"] = True
                    log.info("gs: input init v%d ok, token %#x", init.version, token)
                    asyncio.ensure_future(cursor_task())
                dc_send(pc.M_TOKEN_ACCEPTED)
            elif t == pc.CS_MOUSE_SETTINGS:
                m = cs.MouseSettings(); m.ParseFromString(payload)
                self.stats["mouse_settings"] += 1
                if m.id > input_state["last_mouse_settings_id"]:
                    log.info("gs: mouse settings id=%d %dx%d client_cursor=%s", m.id, m.window_width, m.window_height, m.client_cursor)
                input_state["last_mouse_settings_id"] = max(input_state["last_mouse_settings_id"], m.id)
                r = sc.InputReport(token=self.control_token, last_event_id=input_state["last_event_id"], last_mouse_settings_id=input_state["last_mouse_settings_id"])
                dc_send(pc.SC_INPUT_REPORT, r)
            elif t == pc.CS_INPUT_MESSAGE_V2:
                m = cs.InputMessageV2(); m.ParseFromString(payload)
                raw = zlib.decompress(m.data)
                parsed = parse_input_message(raw)
                self.stats["input_messages"] += 1
                if parsed["token"] != self.control_token:
                    log.error("gs: input message with bad token")
                    return
                new = [e for e in parsed["events"] if e[0] > input_state["last_event_id"]]
                self.stats["input_events"] += len(new)
                for ev in reversed(new):
                    log.info("gs: event #%d %s payload=%s (msg %d, locale=%r lock=%d)", ev[0], KIND_NAMES.get(ev[2] & 0x0F, "?"), ev[3], parsed["message_id"], parsed["locale"], parsed["lock_keys"])
                input_state["last_event_id"] = max(input_state["last_event_id"], parsed["newest_id"])
                r = sc.InputReport(token=self.control_token, last_event_id=input_state["last_event_id"], last_mouse_settings_id=input_state["last_mouse_settings_id"])
                dc_send(pc.SC_INPUT_REPORT, r)
                rep = sc.WebRTCInputReport(token=self.control_token, sent_input_timestamp=parsed["newest_ts"], report_delay_on_server_ms=0)
                dc_send(pc.SC_WEBRTC_INPUT_REPORT, rep)
            elif t == pc.CS_INPUT_REPORT:
                r = cs.InputReport(); r.ParseFromString(payload)
                self.stats["reports"] += 1
                log.info("gs: input report cursor_id=%d last=%d ids=%s clip_confirmed=%d", r.cursor_id, r.cursor_last, list(r.cursor_received_ids), r.clipboard_confirmed_id)
            elif t == pc.CS_SEND_CLIPBOARD_DATA:
                d = cs.ClipboardData(); d.ParseFromString(payload)
                log.info("gs: client clipboard id=%d packet %d/%d", d.id, d.packet_id, d.packets_count)
                r = sc.InputReport(token=self.control_token, last_event_id=input_state["last_event_id"], last_mouse_settings_id=input_state["last_mouse_settings_id"], clipboard_confirmed_id=d.id)
                dc_send(pc.SC_INPUT_REPORT, r)
            else:
                log.info("gs: dc <- %s", tname(t))

        async def make_video_pc(offer_sdp):
            p = RTCPeerConnection()
            @p.on("connectionstatechange")
            async def _():
                log.info("gs: video pc state %s", p.connectionState)
            await p.setRemoteDescription(RTCSessionDescription(offer_sdp, "offer"))
            for tr in p.getTransceivers():
                if tr.kind == "video":
                    tr.sender.replaceTrack(PatternVideoTrack())
                    caps = RTCRtpSender.getCapabilities("video")
                    tr.setCodecPreferences([c for c in caps.codecs if c.mimeType in ("video/H264", "video/rtx")])
                elif tr.kind == "audio":
                    tr.sender.replaceTrack(ToneAudioTrack())
                # transceivers created from a remote recvonly offer default to
                # recvonly here, which would negotiate to "inactive"
                tr.direction = "sendonly"
            answer = await p.createAnswer()
            await p.setLocalDescription(answer)
            return p

        async def make_inputs_pc(offer_sdp):
            nonlocal channel
            p = RTCPeerConnection()
            @p.on("connectionstatechange")
            async def _():
                log.info("gs: inputs pc state %s", p.connectionState)
            @p.on("datachannel")
            def _(ch):
                nonlocal channel
                channel = ch
                log.info("gs: data channel %r ordered=%s lifetime=%s", ch.label, ch.ordered, ch.maxPacketLifeTime)
                ch.on("message", on_channel_message)
            await p.setRemoteDescription(RTCSessionDescription(offer_sdp, "offer"))
            answer = await p.createAnswer()
            await p.setLocalDescription(answer)
            return p

        try:
            async for data in ws:
                t, payload = unframe(data)
                if t == pc.M_KEEP_ALIVE:
                    continue
                log.info("gs: <- %s (%d bytes)", tname(t), len(payload))
                if t == pc.CS_AUTH:
                    a = cs.Auth(); a.ParseFromString(payload)
                    log.info("gs: auth session=%d user=%d connect=%d name=%r token=%r", a.id_session, a.id_user, a.id_connect, a.server_name, a.token_session)
                    assert a.id_session == self.session_id and a.id_user == self.user_id
                    await send(pc.SC_AUTH_SUCCESS)
                elif t == pc.M_CAPABILITIES:
                    await send(pc.M_CAPABILITIES, pc.Capabilities(modern_clipboard=True).SerializeToString())
                    await send(pc.SC_VM_NAME, sc.VmName(name="mock-vm").SerializeToString())
                elif t == pc.CS_SDP_OFFER:
                    o = cs.SdpOffer(); o.ParseFromString(payload)
                    sdp = o.sdp
                    idx = sdp.find("cloudgaming:")
                    hint = sdp[idx:] if idx >= 0 else ""
                    if idx >= 0:
                        sdp = sdp[:idx]
                    log.info("gs: offer type=%d reconfigurate=%s server_ip=%s hint=%r", o.type, o.reconfigurate_flag, o.server_ip, hint)
                    if self.args.dump_sdp:
                        log.info("gs: offer SDP:\n%s", sdp)
                    for line in sdp.splitlines():
                        if line.startswith("m=") or "x-google" in line or "rtx-time" in line or "sps-pps" in line:
                            log.info("gs:   %s", line)
                    # The libwebrtc-specific fmtp hints are fine for the real server but
                    # confuse aiortc's codec matching; drop them for the mock.
                    sdp = "".join(
                        line + "\r\n" for line in sdp.replace("\r\n", "\n").split("\n")
                        if line and "x-google-" not in line and "sps-pps-idr" not in line
                    ).replace(";rtx-time=125", "")
                    if o.type == pc.ST_VIDEO:
                        if video_pc:
                            await video_pc.close()
                        video_pc = await make_video_pc(sdp)
                        if self.args.dump_sdp:
                            log.info("gs: video answer SDP:\n%s", video_pc.localDescription.sdp)
                        ans = sc.SdpAnswer(type=pc.ST_VIDEO, sdp=video_pc.localDescription.sdp)
                        await send(pc.SC_SDP_ANSWER, ans.SerializeToString())
                    else:
                        inputs_pc = await make_inputs_pc(sdp)
                        ans = sc.SdpAnswer(type=pc.ST_INPUTS, sdp=inputs_pc.localDescription.sdp, control_token=self.control_token)
                        await send(pc.SC_SDP_ANSWER, ans.SerializeToString())
                elif t == pc.CS_ICE_CANDIDATE:
                    c = pc.IceCandidate(); c.ParseFromString(payload)
                    import json
                    j = json.loads(c.ice)
                    cand = candidate_from_sdp(j["candidate"].split(":", 1)[1])
                    cand.sdpMid = j.get("sdpMid"); cand.sdpMLineIndex = j.get("sdpMLineIndex")
                    target = video_pc if c.type == pc.ST_VIDEO else inputs_pc
                    if target:
                        await target.addIceCandidate(cand)
                        log.info("gs: added %s candidate %s", "video" if c.type == pc.ST_VIDEO else "inputs", j["candidate"])
                elif t == pc.CS_STREAMS_STATUS:
                    s = cs.StreamsStatus(); s.ParseFromString(payload)
                    log.info("gs: *** streams status flags=%d ***", s.flags)
                    await send(pc.SC_LEFT_GAME_TIME, sc.LeftGameTime(count_seconds=3599).SerializeToString())
                    await send(pc.SC_GAME_WAS_LAUNCHED, sc.GameWasLaunched(executable="game.exe").SerializeToString())
                    if self.args.end_after > 0:
                        async def end():
                            await asyncio.sleep(self.args.end_after)
                            log.info("gs: sending SC_EXIT_GAME")
                            await send(pc.SC_EXIT_GAME)
                        asyncio.ensure_future(end())
                elif t == pc.CS_RECONNECT:
                    log.info("gs: client reconnected the signalling socket")
                elif t == pc.M_CLIENT_BEFORE_CLOSE:
                    await send(pc.M_CLIENT_BEFORE_CLOSE_CONFIRMATION)
                elif t == pc.M_BYE:
                    pass
        finally:
            log.info("gs: closed; stats %s", self.stats)
            for p in (video_pc, inputs_pc):
                if p:
                    await p.close()

async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pb", required=True, help="directory with generated *_pb2.py")
    ap.add_argument("--cert", required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--manager-port", type=int, default=19000)
    ap.add_argument("--ping-port", type=int, default=19001)
    ap.add_argument("--gs-port", type=int, default=19003)
    ap.add_argument("--end-after", type=float, default=0, help="send SC_EXIT_GAME N s after streams status")
    ap.add_argument("--dump-sdp", action="store_true")
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    logging.getLogger("aiortc").setLevel(logging.WARNING)
    logging.getLogger("aioice").setLevel(logging.WARNING)
    load_protos(args.pb)

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(args.cert, args.key)
    cloud = MockCloud(args)
    servers = [
        websockets.serve(cloud.manager, "0.0.0.0", args.manager_port, ssl=ctx, max_size=None),
        websockets.serve(cloud.ping_echo, "0.0.0.0", args.ping_port, ssl=ctx, max_size=None),
        websockets.serve(cloud.ping_echo, "0.0.0.0", args.ping_port + 1, ssl=ctx, max_size=None),
        websockets.serve(cloud.game_server, "0.0.0.0", args.gs_port, ssl=ctx, max_size=None),
    ]
    for s in servers:
        await s
    log.info("mock cloud: manager wss://localhost:%d, ping %d/%d, game server %d", args.manager_port, args.ping_port, args.ping_port + 1, args.gs_port)
    await asyncio.Future()

if __name__ == "__main__":
    asyncio.run(main())

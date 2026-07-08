#!/usr/bin/env python3
import argparse
import base64
import hashlib
import json
import socket
import socketserver
import struct
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler


MAGIC = b"AVC1"
MAX_PACKET = 8 * 1024 * 1024


class Hub:
    def __init__(self, control_host=None, control_port=9002, video_port=9001):
        self.lock = threading.Lock()
        self.ws_clients = set()
        self.raw_clients = set()
        self.latest_width = 0
        self.latest_height = 0
        self.latest_flags = 0
        self.latest_pts_us = 0
        self.packet_count = 0
        self.byte_count = 0
        self.last_packet_ms = 0
        self.config_cache = bytearray()
        self.control_host = control_host
        self.control_port = control_port
        self.video_port = video_port

    def status(self):
        with self.lock:
            return {
                "role": "windows_video_receiver",
                "video_port": self.video_port,
                "width": self.latest_width,
                "height": self.latest_height,
                "flags": self.latest_flags,
                "pts_us": self.latest_pts_us,
                "packets": self.packet_count,
                "bytes": self.byte_count,
                "last_packet_ms": self.last_packet_ms,
                "browser_video_clients": len(self.ws_clients),
                "raw_h264_clients": len(self.raw_clients),
                "control_host": self.control_host or "",
                "control_port": self.control_port,
            }

    def publish_avc1(self, payload):
        if len(payload) < 28 or payload[:4] != MAGIC:
            return

        width, height, flags = struct.unpack(">III", payload[4:16])
        pts_us = struct.unpack(">Q", payload[16:24])[0]
        data_len = struct.unpack(">I", payload[24:28])[0]
        if data_len == 0 or data_len > MAX_PACKET or 28 + data_len > len(payload):
            return

        h264 = payload[28:28 + data_len]
        with self.lock:
            self.latest_width = width
            self.latest_height = height
            self.latest_flags = flags
            self.latest_pts_us = pts_us
            self.packet_count += 1
            self.byte_count += data_len
            self.last_packet_ms = int(time.time() * 1000)
            ws_clients = list(self.ws_clients)
            raw_clients = list(self.raw_clients)
            if flags & 2:
                self.config_cache = bytearray(h264)
            config = bytes(self.config_cache)

        if flags & 1 and config:
            config_payload = MAGIC + struct.pack(">IIIQI", width, height, 2, 0, len(config)) + config
            self._broadcast_ws(config_payload, ws_clients)
            self._broadcast_raw(config, raw_clients)

        self._broadcast_ws(payload, ws_clients)
        self._broadcast_raw(h264, raw_clients)

    def add_ws(self, conn):
        with self.lock:
            self.ws_clients.add(conn)
            width = self.latest_width or 720
            height = self.latest_height or 1568
            config = bytes(self.config_cache)
        if config:
            payload = MAGIC + struct.pack(">IIIQI", width, height, 2, 0, len(config)) + config
            websocket_send_binary(conn, payload)

    def remove_ws(self, conn):
        with self.lock:
            self.ws_clients.discard(conn)

    def add_raw(self, conn):
        with self.lock:
            self.raw_clients.add(conn)
            config = bytes(self.config_cache)
        if config:
            safe_send_all(conn, config)

    def remove_raw(self, conn):
        with self.lock:
            self.raw_clients.discard(conn)

    def _broadcast_ws(self, payload, clients):
        dead = []
        for conn in clients:
            if not websocket_send_binary(conn, payload):
                dead.append(conn)
        if dead:
            with self.lock:
                for conn in dead:
                    self.ws_clients.discard(conn)
                    close_quietly(conn)

    def _broadcast_raw(self, payload, clients):
        dead = []
        for conn in clients:
            if not safe_send_all(conn, payload):
                dead.append(conn)
        if dead:
            with self.lock:
                for conn in dead:
                    self.raw_clients.discard(conn)
                    close_quietly(conn)

    def send_control(self, text):
        if not self.control_host:
            return False, "control_host_not_configured"
        try:
            with socket.create_connection((self.control_host, self.control_port), timeout=1.5) as s:
                s.sendall(text.encode("utf-8") + b"\n")
            return True, "ok"
        except OSError as exc:
            return False, str(exc)


hub = None


def close_quietly(conn):
    try:
        conn.close()
    except OSError:
        pass


def safe_send_all(conn, data):
    try:
        conn.sendall(data)
        return True
    except OSError:
        return False


def read_exact(conn, n):
    chunks = []
    remaining = n
    while remaining > 0:
        chunk = conn.recv(remaining)
        if not chunk:
            return None
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def read_avc1_packet(conn):
    window = bytearray()
    while True:
        b = conn.recv(1)
        if not b:
            return None
        window += b
        if len(window) > 4:
            del window[0]
        if len(window) == 4 and bytes(window) == MAGIC:
            rest = read_exact(conn, 24)
            if rest is None:
                return None
            header = MAGIC + rest
            data_len = struct.unpack(">I", header[24:28])[0]
            if data_len == 0 or data_len > MAX_PACKET:
                continue
            data = read_exact(conn, data_len)
            if data is None:
                return None
            return header + data


class VideoInHandler(socketserver.BaseRequestHandler):
    def handle(self):
        peer = f"{self.client_address[0]}:{self.client_address[1]}"
        print(f"video bridge connected: {peer}")
        try:
            while True:
                packet = read_avc1_packet(self.request)
                if packet is None:
                    break
                hub.publish_avc1(packet)
        finally:
            print(f"video bridge disconnected: {peer}")


class ThreadedTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


class HttpHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        if self.path.startswith("/status"):
            body = json.dumps(hub.status(), ensure_ascii=False).encode("utf-8") + b"\n"
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path.startswith("/video"):
            self.handle_video_ws()
        elif self.path.startswith("/ws"):
            self.handle_control_ws()
        elif self.path.startswith("/h264"):
            self.handle_raw_h264()
        else:
            body = INDEX_HTML.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    def log_message(self, fmt, *args):
        return

    def handle_raw_h264(self):
        self.send_response(200)
        self.send_header("Content-Type", "video/H264")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        hub.add_raw(self.connection)
        try:
            while True:
                time.sleep(1)
        except OSError:
            pass
        finally:
            hub.remove_raw(self.connection)

    def handle_video_ws(self):
        if not websocket_handshake(self):
            return
        hub.add_ws(self.connection)
        try:
            while True:
                time.sleep(1)
        finally:
            hub.remove_ws(self.connection)

    def handle_control_ws(self):
        if not websocket_handshake(self):
            return
        websocket_send_text(self.connection, '{"type":"hello","status":"ok"}')
        while True:
            text = websocket_read_text(self.connection)
            if text is None:
                break
            ok, msg = hub.send_control(text)
            if not ('"type":"move"' in text or '"type":"cursor"' in text):
                websocket_send_text(self.connection, json.dumps({"status": "ok" if ok else "failed", "message": msg}))


class ThreadedHTTPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


def websocket_handshake(handler):
    key = handler.headers.get("Sec-WebSocket-Key")
    if not key:
        handler.send_error(400, "missing websocket key")
        return False
    accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
    handler.send_response(101)
    handler.send_header("Upgrade", "websocket")
    handler.send_header("Connection", "Upgrade")
    handler.send_header("Sec-WebSocket-Accept", accept)
    handler.end_headers()
    return True


def websocket_send_binary(conn, payload):
    return websocket_send_frame(conn, 0x82, payload)


def websocket_send_text(conn, text):
    return websocket_send_frame(conn, 0x81, text.encode("utf-8"))


def websocket_send_frame(conn, opcode, payload):
    try:
        n = len(payload)
        if n < 126:
            hdr = bytes([opcode, n])
        elif n <= 65535:
            hdr = bytes([opcode, 126]) + struct.pack(">H", n)
        else:
            hdr = bytes([opcode, 127]) + struct.pack(">Q", n)
        conn.sendall(hdr + payload)
        return True
    except OSError:
        return False


def websocket_read_text(conn):
    hdr = read_exact(conn, 2)
    if not hdr:
        return None
    opcode = hdr[0] & 0x0F
    masked = hdr[1] & 0x80
    n = hdr[1] & 0x7F
    if opcode == 0x8:
        return None
    if n == 126:
        n = struct.unpack(">H", read_exact(conn, 2))[0]
    elif n == 127:
        n = struct.unpack(">Q", read_exact(conn, 8))[0]
    mask = read_exact(conn, 4) if masked else b"\x00\x00\x00\x00"
    data = read_exact(conn, n)
    if data is None:
        return None
    if masked:
        data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    return data.decode("utf-8", "replace")


INDEX_HTML = r"""<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Windows H.264 Receiver</title>
<style>
body{margin:0;background:#111;color:#eee;font-family:Arial,sans-serif;text-align:center}
h1{font-size:20px;margin:14px}.wrap{display:inline-block;position:relative;background:#000;border:1px solid #333;touch-action:none;user-select:none;cursor:none}
canvas{display:block;max-width:100vw;max-height:86vh;background:#000;touch-action:none}
.cursor{position:fixed;left:0;top:0;width:14px;height:14px;border:2px solid #fff;border-radius:50%;box-shadow:0 0 0 1px #111,0 1px 6px #000;pointer-events:none;transform:translate(-50%,-50%);display:none;z-index:9999}
.cursor:after{content:"";position:absolute;left:5px;top:5px;width:3px;height:3px;background:#fff;border-radius:50%}.log{font-size:13px;color:#9ca3af;margin:8px;white-space:pre-wrap}
</style></head><body>
<h1>Windows H.264 Receiver</h1>
<div class="wrap" id="wrap"><canvas id="canvas" width="720" height="1568"></canvas><div class="cursor" id="cursor"></div></div>
<div class="log" id="log">Starting...</div>
<script>
let W=720,H=1568,decoder=null,decoderReady=false,pending=[],ws=null,vws=null,wsReady=false;
let fps=0,frames=0,lastFps=performance.now();const canvas=document.getElementById('canvas'),ctx=canvas.getContext('2d'),wrap=document.getElementById('wrap'),cursor=document.getElementById('cursor'),logEl=document.getElementById('log');
function log(s){logEl.textContent=s;}
function setSize(w,h){if(w&&h&&(w!==W||h!==H)){W=w;H=h;canvas.width=W;canvas.height=H;}canvas.style.width='min(96vw,420px)';canvas.style.height='auto';}
async function makeDecoder(){if(!('VideoDecoder'in window)){log('This browser does not support WebCodecs. Use Chrome or Edge.');return null;}
 const codecs=['avc1.640028','avc1.640032','avc1.64001f','avc1.4D4028','avc1.42E01E'];
 for(const codec of codecs){for(const cfg of [{codec,codedWidth:W,codedHeight:H,optimizeForLatency:true,avc:{format:'annexb'}},{codec,codedWidth:W,codedHeight:H,optimizeForLatency:true}]){
  try{if(VideoDecoder.isConfigSupported){const r=await VideoDecoder.isConfigSupported(cfg);if(!r.supported)continue;}
   const d=new VideoDecoder({output:f=>{try{ctx.drawImage(f,0,0,W,H);}finally{f.close();}frames++;const now=performance.now();if(now-lastFps>=1000){fps=frames;frames=0;lastFps=now;}},error:e=>log('decoder error: '+e.message)});
   d.configure(cfg);log('decoder ok: '+codec);return d;}catch(e){}
 }}log('H.264 decoder config failed.');return null;}
async function ensureDecoder(){if(decoderReady&&decoder)return decoder;decoder=await makeDecoder();decoderReady=!!decoder;if(decoderReady){const q=pending;pending=[];q.forEach(decodePacket);}return decoder;}
function u32(dv,o){return dv.getUint32(o,false)}function u64(dv,o){return dv.getUint32(o,false)*4294967296+dv.getUint32(o+4,false)}
function decodePacket(buf){if(buf.byteLength<28)return;const dv=new DataView(buf);const magic=String.fromCharCode(dv.getUint8(0),dv.getUint8(1),dv.getUint8(2),dv.getUint8(3));if(magic!=='AVC1')return;
 const w=u32(dv,4),h=u32(dv,8),flags=u32(dv,12);let pts=u64(dv,16);const size=u32(dv,24);if(28+size>buf.byteLength)return;setSize(w,h);
 if(!decoderReady||!decoder){pending.push(buf);ensureDecoder();return;}const data=new Uint8Array(buf,28,size);const type=(flags&1)?'key':'delta';if(flags&2)pts=performance.now()*1000;
 try{if(decoder.decodeQueueSize>8)return;decoder.decode(new EncodedVideoChunk({type,timestamp:pts||Math.round(performance.now()*1000),data}));log('video '+W+'x'+H+' fps='+fps+' queue='+decoder.decodeQueueSize+' control='+(wsReady?'ok':'wait'));}catch(e){log('decode error: '+e.message);}}
function connectVideo(){vws=new WebSocket('ws://'+location.host+'/video');vws.binaryType='arraybuffer';vws.onopen=()=>log('video ws connected, waiting stream from RK3568...');vws.onmessage=e=>{if(e.data instanceof ArrayBuffer)decodePacket(e.data);};vws.onclose=()=>{decoderReady=false;try{decoder&&decoder.close();}catch(_){}decoder=null;setTimeout(connectVideo,1000);};vws.onerror=()=>log('video ws error');}
function connectControl(){ws=new WebSocket('ws://'+location.host+'/ws');ws.onopen=()=>{wsReady=true;};ws.onclose=()=>{wsReady=false;setTimeout(connectControl,1000);};ws.onerror=()=>{wsReady=false;};}
connectControl();connectVideo();
function pt(e){const r=canvas.getBoundingClientRect();return{x:Math.max(0,Math.min(W-1,Math.round((e.clientX-r.left)*W/r.width))),y:Math.max(0,Math.min(H-1,Math.round((e.clientY-r.top)*H/r.height)))}}
function moveCursor(e){cursor.style.left=e.clientX+'px';cursor.style.top=e.clientY+'px';cursor.style.display='block';}
function send(a){if(!wsReady||!ws||ws.readyState!==1)return false;ws.send(JSON.stringify(a));return true;}
let active=false,id=null,down=null,drag=false,downSent=false,lastMove=0,timer=null;function dist(a,b){const x=a.x-b.x,y=a.y-b.y;return Math.sqrt(x*x+y*y)}function startDrag(){if(!active||!down||downSent)return;drag=true;downSent=true;send({type:'down',x:down.x,y:down.y,width:W,height:H});}
wrap.oncontextmenu=e=>e.preventDefault();wrap.onpointerenter=e=>{moveCursor(e);};wrap.onpointerleave=e=>{if(!active)cursor.style.display='none';};
wrap.onpointerdown=e=>{if(e.button!==0)return;e.preventDefault();moveCursor(e);wrap.setPointerCapture(e.pointerId);const p=pt(e);send({type:'cursor',x:p.x,y:p.y,width:W,height:H});active=true;id=e.pointerId;down=p;drag=false;downSent=false;timer=setTimeout(startDrag,450);};
wrap.onpointermove=e=>{moveCursor(e);const p=pt(e);if(!active){return;}if(id!==e.pointerId)return;e.preventDefault();if(!drag&&dist(p,down)>8){clearTimeout(timer);startDrag();}if(drag&&Date.now()-lastMove>16){lastMove=Date.now();send({type:'move',x:p.x,y:p.y,width:W,height:H});}};
wrap.onpointerup=e=>{if(!active||id!==e.pointerId)return;e.preventDefault();clearTimeout(timer);const p=pt(e);if(drag||downSent)send({type:'up',x:p.x,y:p.y,width:W,height:H});else send({type:'tap',x:p.x,y:p.y,width:W,height:H});active=false;id=null;down=null;drag=false;downSent=false;};
</script></body></html>"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--http-host", default="127.0.0.1")
    parser.add_argument("--http-port", type=int, default=18091)
    parser.add_argument("--video-host", default="0.0.0.0")
    parser.add_argument("--video-port", type=int, default=9001)
    parser.add_argument("--control-host", default="")
    parser.add_argument("--control-port", type=int, default=9002)
    parser.add_argument("--open-browser", action="store_true")
    args = parser.parse_args()

    global hub
    hub = Hub(args.control_host or None, args.control_port, args.video_port)

    video_server = ThreadedTCPServer((args.video_host, args.video_port), VideoInHandler)
    http_server = ThreadedHTTPServer((args.http_host, args.http_port), HttpHandler)
    threading.Thread(target=video_server.serve_forever, daemon=True).start()
    threading.Thread(target=http_server.serve_forever, daemon=True).start()

    url = f"http://{args.http_host}:{args.http_port}/"
    print(f"video input: tcp://{args.video_host}:{args.video_port}")
    print(f"browser UI:  {url}")
    if args.control_host:
        print(f"control out: {args.control_host}:{args.control_port}")
    else:
        print("control out: disabled")
    if args.open_browser:
        webbrowser.open(url)

    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

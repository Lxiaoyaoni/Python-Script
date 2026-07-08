# Windows video receiver

Target topology:

```text
Phone companion app -> AOA USB -> RK3568 bridge -> Windows receiver -> Browser
```

RK3568 starts AOA and reads USB. Windows receives the video stream over TCP and
renders it in the browser.

## Ports

Windows:

```text
18091 Browser HTTP/WebSocket UI
9001  TCP video input from RK3568 bridge
```

RK3568:

```text
9002  TCP control input from Windows, JSON line protocol
```

## Windows start

```powershell
.\start_windows.ps1 -RkHost 192.168.110.86
```

Open manually if the browser does not open:

```text
http://127.0.0.1:18091/
```

The Windows receiver expects RK3568 to connect to Windows port `9001` and send
complete AVC1 packets:

```text
4 bytes   "AVC1"
4 bytes   width, big endian
4 bytes   height, big endian
4 bytes   flags, big endian
8 bytes   pts_us, big endian
4 bytes   data_len, big endian
N bytes   H.264 Annex-B data
```

The browser sends control JSON to Windows `/ws`; Windows forwards each command
as one UTF-8 JSON line to:

```text
192.168.110.86:9002
```

Example command:

```json
{"type":"tap","x":120,"y":300,"width":720,"height":1568}
```

## RK3568 requirements

The attached `aoa_start_auto` only starts AOA mode. Use `rk_aoa_bridge.c` as
the RK3568 bridge service:

```sh
gcc rk_aoa_bridge.c -o rk_aoa_bridge -lusb-1.0 -lpthread
```

Start Windows receiver first, then start the RK3568 bridge. The first argument
must be the Windows IP address:

```sh
./rk_aoa_bridge WINDOWS_IP 9001 9002
```

The bridge service does:

```text
AOA USB bulk IN -> TCP connect to Windows:9001
Windows JSON control -> RK3568:9002 -> AOA HID
```

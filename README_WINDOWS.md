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

For multi-phone testing, one Windows receiver instance maps to one phone:

```powershell
.\start_windows.ps1 -RkHost 192.168.110.86 -Instance 0
.\start_windows.ps1 -RkHost 192.168.110.86 -Instance 1
```

The instance number chooses ports automatically:

```text
Instance 0: browser 18091, video 9001, control 9002
Instance 1: browser 18101, video 9011, control 9012
Instance 2: browser 18111, video 9021, control 9022
```

You can also start several Windows receivers at once:

```powershell
.\start_multi_windows.ps1 -RkHost 192.168.110.86 -Count 2
```

Logs are written under `logs/`:

```text
logs/windows_multi_YYYYMMDD_HHMMSS.summary.log
logs/windows_instance_0_YYYYMMDD_HHMMSS.log
logs/windows_instance_1_YYYYMMDD_HHMMSS.log
```

Stop all Windows receiver instances launched by this helper:

```powershell
.\stop_multi_windows.ps1
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

For multi-phone testing, first list devices on RK3568:

```sh
./rk_aoa_bridge --list
```

If phones have not entered AOA mode yet, ask all known starter devices to enter
AOA mode:

```sh
./rk_aoa_bridge --start-all
sleep 2
./rk_aoa_bridge --list
```

Then start one bridge process per phone:

```sh
./rk_aoa_bridge WINDOWS_IP 9001 9002 --device-index 0
./rk_aoa_bridge WINDOWS_IP 9011 9012 --device-index 1
```

For stable binding, use the USB bus/address shown by `--list`:

```sh
./rk_aoa_bridge WINDOWS_IP 9001 9002 --usb 005:032
./rk_aoa_bridge WINDOWS_IP 9011 9012 --usb 005:033
```

Or copy these files to RK3568 and use the unified launcher:

```text
rk_aoa_bridge
rk_bridge_config.sh
start_multi_rk_bridge.sh
stop_multi_rk_bridge.sh
```

Edit `rk_bridge_config.sh`, then run:

```sh
chmod +x rk_aoa_bridge start_multi_rk_bridge.sh stop_multi_rk_bridge.sh
./start_multi_rk_bridge.sh
```

RK logs are written under `logs/`:

```text
logs/rk_bridge_YYYYMMDD_HHMMSS.summary.log
logs/rk_bridge_YYYYMMDD_HHMMSS_instance_0.log
logs/rk_bridge_YYYYMMDD_HHMMSS_instance_1.log
```

Stop all RK bridge instances launched by this helper:

```sh
./stop_multi_rk_bridge.sh
```

The bridge service does:

```text
AOA USB bulk IN -> TCP connect to Windows:9001
Windows JSON control -> RK3568:9002 -> AOA HID
```

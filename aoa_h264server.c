
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET socket_t;
typedef SSIZE_T ssize_t;
typedef HANDLE pthread_t;
typedef SRWLOCK pthread_mutex_t;
#define SOCKET_INVALID INVALID_SOCKET
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
#define pthread_mutex_lock(m) AcquireSRWLockExclusive(m)
#define pthread_mutex_unlock(m) ReleaseSRWLockExclusive(m)
#define close closesocket
#define usleep(usec) Sleep((DWORD)(((usec) + 999) / 1000))

struct win_thread_start {
    void *(*fn)(void *);
    void *arg;
};

static unsigned __stdcall win_thread_main(void *arg) {
    struct win_thread_start *start = (struct win_thread_start *)arg;
    void *(*fn)(void *) = start->fn;
    void *fn_arg = start->arg;
    free(start);
    fn(fn_arg);
    return 0;
}

static int pthread_create(pthread_t *thread, void *attr, void *(*fn)(void *), void *arg) {
    (void)attr;

    struct win_thread_start *start = (struct win_thread_start *)malloc(sizeof(*start));
    if (!start) {
        return ENOMEM;
    }

    start->fn = fn;
    start->arg = arg;

    uintptr_t h = _beginthreadex(NULL, 0, win_thread_main, start, 0, NULL);
    if (!h) {
        free(start);
        return errno ? errno : EINVAL;
    }

    *thread = (HANDLE)h;
    return 0;
}

static int pthread_detach(pthread_t thread) {
    CloseHandle(thread);
    return 0;
}

static int pthread_join(pthread_t thread, void **value_ptr) {
    (void)value_ptr;
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return 0;
}
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int socket_t;
#define SOCKET_INVALID (-1)
#endif
#include <libusb.h>

#include "aoa_start.h"

#define VID_GOOGLE 0x18d1
#define MAX_H264_PACKET_SIZE (8 * 1024 * 1024)
#define DEFAULT_PORT 8091
#define USB_BUFFER_SIZE (256 * 1024)

#ifdef _WIN32
#define H264_SAVE_PATH "rk_aoa_stream.h264"
#else
#define H264_SAVE_PATH "/tmp/rk_aoa_stream.h264"
#endif

/*
 * Android Open Accessory HID requests.
 */
#define AOA_REQ_REGISTER_HID        54
#define AOA_REQ_UNREGISTER_HID      55
#define AOA_REQ_SET_HID_REPORT_DESC 56
#define AOA_REQ_SEND_HID_EVENT      57

#define HID_ID_TOUCH 1

struct usb_id {
    uint16_t vid;
    uint16_t pid;
    const char *name;
};

static struct usb_id candidate_devices[] = {
    {VID_GOOGLE, 0x2d00, "AOA only"},
    {VID_GOOGLE, 0x2d01, "AOA + ADB"},
    {VID_GOOGLE, 0x2d02, "AOA audio"},
    {VID_GOOGLE, 0x2d03, "AOA audio + ADB"},
    {VID_GOOGLE, 0x2d04, "AOA accessory + audio"},
    {VID_GOOGLE, 0x2d05, "AOA accessory + audio + ADB"},
};

static volatile int running = 1;

static pthread_mutex_t aoa_mutex = PTHREAD_MUTEX_INITIALIZER;
static libusb_device_handle *aoa_handle = NULL;
static int aoa_out_ep = 0;

static pthread_mutex_t hid_mutex = PTHREAD_MUTEX_INITIALIZER;
static int hid_ready = 0;
static pthread_mutex_t aoa_send_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * H.264 状态统计。
 */
static pthread_mutex_t h264_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t latest_width = 0;
static uint32_t latest_height = 0;
static uint32_t latest_flags = 0;
static uint64_t latest_pts_us = 0;
static uint64_t h264_packet_count = 0;
static uint64_t h264_keyframe_count = 0;
static uint64_t h264_config_count = 0;
static uint64_t h264_bytes_total = 0;
static uint64_t h264_last_packet_ms = 0;

/*
 * /h264 原始 H.264 HTTP 客户端列表。
 *
 * 注意：
 * 浏览器一般不能直接播放 raw H.264。
 * 这个接口主要给 ffplay / VLC 测试：
 *
 * ffplay -fflags nobuffer -flags low_delay -f h264 http://RK3568_IP:8091/h264
 */
struct h264_client {
    socket_t fd;
    struct h264_client *next;
};

static pthread_mutex_t h264_clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct h264_client *h264_clients = NULL;

/*
 * 浏览器 WebCodecs H.264 WebSocket 客户端列表。
 * /video 会把每个 H.264 包用 WebSocket binary frame 推给浏览器。
 */
struct ws_h264_client {
    socket_t fd;
    struct ws_h264_client *next;
};

static pthread_mutex_t ws_h264_clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct ws_h264_client *ws_h264_clients = NULL;

/*
 * 缓存 H.264 SPS/PPS 配置数据。
 *
 * ffplay/VLC 这种客户端如果中途连接 /h264，
 * 很可能错过最开始的 SPS/PPS，结果只能显示第一帧、黑屏，
 * 或报 non-existing PPS。
 *
 * 所以：
 * 1. 收到 flags=2 的 codec config 包时，把它缓存起来；
 * 2. 新的 /h264 客户端连接时，先补发这段配置；
 * 3. 每次 keyframe 前，再补发一次配置。
 */
static pthread_mutex_t h264_config_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *h264_config_cache = NULL;
static size_t h264_config_cache_len = 0;
static size_t h264_config_cache_cap = 0;

/*
 * 打印限速。
 */
static uint64_t last_cursor_print_ms = 0;
static uint64_t last_hid_move_print_ms = 0;
static uint64_t last_h264_print_ms = 0;

struct usb_stream_reader {
    libusb_device_handle *handle;
    int ep;
    unsigned char buf[USB_BUFFER_SIZE];
    int pos;
    int len;
};

struct client_arg {
    socket_t client;
};

/* Forward declaration for browser video WebSocket. */
static int websocket_handshake(socket_t client, const char *req);

/*
 * AOA HID 绝对触摸描述符。
 *
 * Report 格式：
 * byte 0    : touching，1=按下，0=抬起
 * byte 1-2  : X，0~32767，小端序
 * byte 3-4  : Y，0~32767，小端序
 */
static const unsigned char touch_hid_desc[] = {
    0x05, 0x0D,        /* Usage Page (Digitizers) */
    0x09, 0x04,        /* Usage (Touch Screen) */
    0xA1, 0x01,        /* Collection (Application) */

    0x09, 0x22,        /*   Usage (Finger) */
    0xA1, 0x02,        /*   Collection (Logical) */

    0x09, 0x42,        /*     Usage (Tip Switch) */
    0x15, 0x00,        /*     Logical Minimum (0) */
    0x25, 0x01,        /*     Logical Maximum (1) */
    0x75, 0x01,        /*     Report Size (1) */
    0x95, 0x01,        /*     Report Count (1) */
    0x81, 0x02,        /*     Input (Data,Var,Abs) */

    0x75, 0x07,        /*     Report Size (7) */
    0x95, 0x01,        /*     Report Count (1) */
    0x81, 0x03,        /*     Input (Const,Var,Abs) padding */

    0x05, 0x01,        /*     Usage Page (Generic Desktop) */
    0x09, 0x30,        /*     Usage (X) */
    0x09, 0x31,        /*     Usage (Y) */
    0x15, 0x00,        /*     Logical Minimum (0) */
    0x26, 0xFF, 0x7F,  /*     Logical Maximum (32767) */
    0x75, 0x10,        /*     Report Size (16) */
    0x95, 0x02,        /*     Report Count (2) */
    0x81, 0x02,        /*     Input (Data,Var,Abs) */

    0xC0,              /*   End Collection */
    0xC0               /* End Collection */
};

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

static uint32_t read_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           ((uint32_t)p[3]);
}

static uint64_t read_be64(const unsigned char *p) {
    return ((uint64_t)p[0] << 56) |
           ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) |
           ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) |
           ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8) |
           ((uint64_t)p[7]);
}

static uint64_t now_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
#endif
}

static void set_tcp_nodelay(socket_t fd) {
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof(yes));
}

static void set_nonblocking(socket_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
#endif
}

static void set_blocking(socket_t fd) {
#ifdef _WIN32
    u_long mode = 0;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
#endif
}

static int socket_would_block(void) {
#ifdef _WIN32
    int err = WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

static void socket_perror(const char *what) {
#ifdef _WIN32
    fprintf(stderr, "%s: WSA error %d\n", what, WSAGetLastError());
#else
    perror(what);
#endif
}

static libusb_device_handle *open_aoa_device(libusb_context *ctx) {
    size_t count = sizeof(candidate_devices) / sizeof(candidate_devices[0]);

    for (size_t i = 0; i < count; i++) {
        struct usb_id id = candidate_devices[i];

        libusb_device_handle *handle =
            libusb_open_device_with_vid_pid(ctx, id.vid, id.pid);

        if (handle) {
            printf("found device: %04x:%04x (%s)\n", id.vid, id.pid, id.name);
            return handle;
        }
    }

    return NULL;
}

static int find_bulk_endpoints(
    libusb_device_handle *handle,
    int *interface_number,
    int *in_ep,
    int *out_ep
) {
    libusb_device *dev = libusb_get_device(handle);
    struct libusb_config_descriptor *config = NULL;

    int r = libusb_get_active_config_descriptor(dev, &config);
    if (r != 0) {
        printf("get config descriptor failed: %s\n", libusb_error_name(r));
        return r;
    }

    for (int i = 0; i < config->bNumInterfaces; i++) {
        const struct libusb_interface *interface = &config->interface[i];

        for (int j = 0; j < interface->num_altsetting; j++) {
            const struct libusb_interface_descriptor *alt = &interface->altsetting[j];
            int local_in_ep = 0;
            int local_out_ep = 0;

            for (int k = 0; k < alt->bNumEndpoints; k++) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[k];

                int ep_addr = ep->bEndpointAddress;
                int ep_type = ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                int ep_dir = ep_addr & LIBUSB_ENDPOINT_DIR_MASK;

                if (ep_type != LIBUSB_TRANSFER_TYPE_BULK) {
                    continue;
                }

                if (ep_dir == LIBUSB_ENDPOINT_IN) {
                    local_in_ep = ep_addr;
                } else {
                    local_out_ep = ep_addr;
                }

                if (local_in_ep != 0 && local_out_ep != 0) {
                    *interface_number = alt->bInterfaceNumber;
                    *in_ep = local_in_ep;
                    *out_ep = local_out_ep;

                    printf(
                        "found bulk IN endpoint: 0x%02x, OUT endpoint: 0x%02x, interface: %d\n",
                        *in_ep,
                        *out_ep,
                        *interface_number
                    );

                    libusb_free_config_descriptor(config);
                    return 0;
                }
            }
        }
    }

    libusb_free_config_descriptor(config);
    return -1;
}

/* =========================
 * AOA HID Touch
 * ========================= */

static int aoa_ctrl_out(
    libusb_device_handle *handle,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    unsigned char *data,
    uint16_t length
) {
    pthread_mutex_lock(&aoa_send_mutex);

    int r = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_OUT |
            LIBUSB_REQUEST_TYPE_VENDOR |
            LIBUSB_RECIPIENT_DEVICE,
        request,
        value,
        index,
        data,
        length,
        1000
    );

    pthread_mutex_unlock(&aoa_send_mutex);

    if (r < 0) {
        printf(
            "AOA ctrl request %u failed: %s (%d)\n",
            request,
            libusb_error_name(r),
            r
        );
        return r;
    }

    return 0;
}

static int aoa_hid_register_touch(libusb_device_handle *handle) {
    int r;

    r = aoa_ctrl_out(
        handle,
        AOA_REQ_REGISTER_HID,
        HID_ID_TOUCH,
        sizeof(touch_hid_desc),
        NULL,
        0
    );

    if (r != 0) {
        return r;
    }

    r = aoa_ctrl_out(
        handle,
        AOA_REQ_SET_HID_REPORT_DESC,
        HID_ID_TOUCH,
        0,
        (unsigned char *)touch_hid_desc,
        sizeof(touch_hid_desc)
    );

    if (r != 0) {
        return r;
    }

    printf("AOA HID touch registered\n");
    return 0;
}

static int aoa_hid_unregister_touch(libusb_device_handle *handle) {
    return aoa_ctrl_out(
        handle,
        AOA_REQ_UNREGISTER_HID,
        HID_ID_TOUCH,
        0,
        NULL,
        0
    );
}

static int aoa_hid_send_touch_report(
    libusb_device_handle *handle,
    int touching,
    int x,
    int y,
    int width,
    int height
) {
    if (width <= 1) {
        width = 1080;
    }

    if (height <= 1) {
        height = 2340;
    }

    if (x < 0) x = 0;
    if (y < 0) y = 0;

    if (x > width - 1) x = width - 1;
    if (y > height - 1) y = height - 1;

    int hx = x * 32767 / (width - 1);
    int hy = y * 32767 / (height - 1);

    unsigned char report[5];

    report[0] = touching ? 1 : 0;
    report[1] = (unsigned char)(hx & 0xff);
    report[2] = (unsigned char)((hx >> 8) & 0xff);
    report[3] = (unsigned char)(hy & 0xff);
    report[4] = (unsigned char)((hy >> 8) & 0xff);

    return aoa_ctrl_out(
        handle,
        AOA_REQ_SEND_HID_EVENT,
        HID_ID_TOUCH,
        0,
        report,
        sizeof(report)
    );
}

static int aoa_hid_touch_down(int x, int y, int width, int height) {
    pthread_mutex_lock(&hid_mutex);

    libusb_device_handle *handle = aoa_handle;

    if (!handle || !hid_ready) {
        pthread_mutex_unlock(&hid_mutex);
        printf("AOA HID is not ready\n");
        return -1;
    }

    int r = aoa_hid_send_touch_report(handle, 1, x, y, width, height);

    pthread_mutex_unlock(&hid_mutex);

    printf("hid touch down x=%d y=%d width=%d height=%d result=%d\n", x, y, width, height, r);
    return r;
}

static int aoa_hid_touch_move(int x, int y, int width, int height) {
    pthread_mutex_lock(&hid_mutex);

    libusb_device_handle *handle = aoa_handle;

    if (!handle || !hid_ready) {
        pthread_mutex_unlock(&hid_mutex);
        printf("AOA HID is not ready\n");
        return -1;
    }

    int r = aoa_hid_send_touch_report(handle, 1, x, y, width, height);

    pthread_mutex_unlock(&hid_mutex);

    uint64_t now = now_ms();

    if (now - last_hid_move_print_ms >= 80) {
        last_hid_move_print_ms = now;
        printf("hid touch move x=%d y=%d width=%d height=%d result=%d\n", x, y, width, height, r);
    }

    return r;
}

static int aoa_hid_touch_up(int x, int y, int width, int height) {
    pthread_mutex_lock(&hid_mutex);

    libusb_device_handle *handle = aoa_handle;

    if (!handle || !hid_ready) {
        pthread_mutex_unlock(&hid_mutex);
        printf("AOA HID is not ready\n");
        return -1;
    }

    int r = aoa_hid_send_touch_report(handle, 0, x, y, width, height);

    pthread_mutex_unlock(&hid_mutex);

    printf("hid touch up x=%d y=%d width=%d height=%d result=%d\n", x, y, width, height, r);
    return r;
}

static int aoa_hid_tap(int x, int y, int width, int height) {
    pthread_mutex_lock(&hid_mutex);

    libusb_device_handle *handle = aoa_handle;

    if (!handle || !hid_ready) {
        pthread_mutex_unlock(&hid_mutex);
        printf("AOA HID is not ready\n");
        return -1;
    }

    int r = aoa_hid_send_touch_report(handle, 1, x, y, width, height);

    if (r == 0) {
        usleep(80000);
        r = aoa_hid_send_touch_report(handle, 0, x, y, width, height);
    }

    pthread_mutex_unlock(&hid_mutex);

    printf("hid touch tap x=%d y=%d width=%d height=%d result=%d\n", x, y, width, height, r);
    return r;
}

static int aoa_hid_swipe(
    int x1,
    int y1,
    int x2,
    int y2,
    int width,
    int height,
    int duration_ms
) {
    pthread_mutex_lock(&hid_mutex);

    libusb_device_handle *handle = aoa_handle;

    if (!handle || !hid_ready) {
        pthread_mutex_unlock(&hid_mutex);
        printf("AOA HID is not ready\n");
        return -1;
    }

    if (duration_ms < 100) duration_ms = 100;
    if (duration_ms > 2000) duration_ms = 2000;

    int r = aoa_hid_send_touch_report(handle, 1, x1, y1, width, height);

    int steps = duration_ms / 10;
    if (steps < 8) steps = 8;

    for (int i = 1; r == 0 && i <= steps; i++) {
        int nx = x1 + (x2 - x1) * i / steps;
        int ny = y1 + (y2 - y1) * i / steps;

        r = aoa_hid_send_touch_report(handle, 1, nx, ny, width, height);
        usleep(10000);
    }

    if (r == 0) {
        r = aoa_hid_send_touch_report(handle, 0, x2, y2, width, height);
    }

    pthread_mutex_unlock(&hid_mutex);

    printf("hid touch swipe %d,%d -> %d,%d width=%d height=%d result=%d\n",
           x1, y1, x2, y2, width, height, r);

    return r;
}

/* =========================
 * USB stream reader
 * ========================= */

static int stream_fill(struct usb_stream_reader *reader) {
    reader->pos = 0;
    reader->len = 0;

    while (running) {
        int transferred = 0;

        int r = libusb_bulk_transfer(
            reader->handle,
            reader->ep,
            reader->buf,
            sizeof(reader->buf),
            &transferred,
            3000
        );

        if (r == LIBUSB_ERROR_TIMEOUT) {
            continue;
        }

        if (r != 0) {
            printf("bulk read failed: %s (%d)\n", libusb_error_name(r), r);
            return r;
        }

        if (transferred <= 0) {
            usleep(10000);
            continue;
        }

        reader->len = transferred;
        return 0;
    }

    return -1;
}

static int read_exact(struct usb_stream_reader *reader, unsigned char *buf, int len) {
    int offset = 0;

    while (running && offset < len) {
        if (reader->pos >= reader->len) {
            int r = stream_fill(reader);
            if (r != 0) {
                return r;
            }
        }

        int available = reader->len - reader->pos;
        int need = len - offset;
        int copy_len = available < need ? available : need;

        if (copy_len > 0) {
            memcpy(buf + offset, reader->buf + reader->pos, copy_len);
            reader->pos += copy_len;
            offset += copy_len;
        }
    }

    return offset == len ? 0 : -1;
}

static int stream_read_byte(struct usb_stream_reader *reader, unsigned char *out) {
    while (running) {
        if (reader->pos >= reader->len) {
            int r = stream_fill(reader);
            if (r != 0) {
                return r;
            }
        }

        if (reader->pos < reader->len) {
            *out = reader->buf[reader->pos++];
            return 0;
        }
    }

    return -1;
}

static int read_avc1_header(struct usb_stream_reader *reader, unsigned char header[28]) {
    unsigned char window[4] = {0};
    int filled = 0;

    while (running) {
        unsigned char b = 0;

        int r = stream_read_byte(reader, &b);
        if (r != 0) {
            return r;
        }

        if (filled < 4) {
            window[filled++] = b;
        } else {
            window[0] = window[1];
            window[1] = window[2];
            window[2] = window[3];
            window[3] = b;
        }

        if (filled == 4 && memcmp(window, "AVC1", 4) == 0) {
            memcpy(header, "AVC1", 4);
            return read_exact(reader, header + 4, 24);
        }
    }

    return -1;
}

/* =========================
 * H.264 publish/save/broadcast
 * ========================= */

static int has_start_code(const unsigned char *data, uint32_t len) {
    if (len >= 4 &&
        data[0] == 0x00 &&
        data[1] == 0x00 &&
        data[2] == 0x00 &&
        data[3] == 0x01) {
        return 1;
    }

    if (len >= 3 &&
        data[0] == 0x00 &&
        data[1] == 0x00 &&
        data[2] == 0x01) {
        return 1;
    }

    return 0;
}

static void remove_h264_client_locked(struct h264_client **pp);

static int send_blocking_all(socket_t fd, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;

    while (len > 0) {
        int chunk = len > 0x7fffffffU ? 0x7fffffff : (int)len;
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, (const char *)p, chunk, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, (const char *)p, chunk, 0);
#endif

        if (n < 0) {
#ifndef _WIN32
            if (errno == EINTR) {
                continue;
            }
#endif
            return -1;
        }

        if (n == 0) {
            return -1;
        }

        p += n;
        len -= (size_t)n;
    }

    return 0;
}


static void write_be32_to_buf(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)((v >> 24) & 0xff);
    p[1] = (unsigned char)((v >> 16) & 0xff);
    p[2] = (unsigned char)((v >> 8) & 0xff);
    p[3] = (unsigned char)(v & 0xff);
}

static void write_be64_to_buf(unsigned char *p, uint64_t v) {
    p[0] = (unsigned char)((v >> 56) & 0xff);
    p[1] = (unsigned char)((v >> 48) & 0xff);
    p[2] = (unsigned char)((v >> 40) & 0xff);
    p[3] = (unsigned char)((v >> 32) & 0xff);
    p[4] = (unsigned char)((v >> 24) & 0xff);
    p[5] = (unsigned char)((v >> 16) & 0xff);
    p[6] = (unsigned char)((v >> 8) & 0xff);
    p[7] = (unsigned char)(v & 0xff);
}

static int websocket_send_binary_frame(socket_t fd, const unsigned char *data, size_t len) {
    unsigned char hdr[10];
    size_t hdr_len = 0;

    hdr[0] = 0x82; /* FIN + binary */

    if (len < 126) {
        hdr[1] = (unsigned char)len;
        hdr_len = 2;
    } else if (len <= 65535) {
        hdr[1] = 126;
        hdr[2] = (unsigned char)((len >> 8) & 0xff);
        hdr[3] = (unsigned char)(len & 0xff);
        hdr_len = 4;
    } else {
        hdr[1] = 127;
        hdr[2] = (unsigned char)((len >> 56) & 0xff);
        hdr[3] = (unsigned char)((len >> 48) & 0xff);
        hdr[4] = (unsigned char)((len >> 40) & 0xff);
        hdr[5] = (unsigned char)((len >> 32) & 0xff);
        hdr[6] = (unsigned char)((len >> 24) & 0xff);
        hdr[7] = (unsigned char)((len >> 16) & 0xff);
        hdr[8] = (unsigned char)((len >> 8) & 0xff);
        hdr[9] = (unsigned char)(len & 0xff);
        hdr_len = 10;
    }

    if (send_blocking_all(fd, hdr, hdr_len) != 0) {
        return -1;
    }

    return send_blocking_all(fd, data, len);
}

static void remove_ws_h264_client_locked(struct ws_h264_client **pp) {
    struct ws_h264_client *dead = *pp;
    *pp = dead->next;
    close(dead->fd);
    free(dead);
}

static void broadcast_ws_h264_packet(
    uint32_t width,
    uint32_t height,
    uint32_t flags,
    uint64_t pts_us,
    const unsigned char *data,
    size_t len
) {
    if (!data || len == 0) {
        return;
    }

    /*
     * WebSocket binary payload:
     * 4 bytes  "AVC1"
     * 4 bytes  width
     * 4 bytes  height
     * 4 bytes  flags
     * 8 bytes  pts_us
     * 4 bytes  data_len
     * N bytes  Annex-B H.264 data
     */
    size_t payload_len = 28 + len;
    unsigned char *payload = (unsigned char *)malloc(payload_len);
    if (!payload) {
        return;
    }

    memcpy(payload, "AVC1", 4);
    write_be32_to_buf(payload + 4, width);
    write_be32_to_buf(payload + 8, height);
    write_be32_to_buf(payload + 12, flags);
    write_be64_to_buf(payload + 16, pts_us);
    write_be32_to_buf(payload + 24, (uint32_t)len);
    memcpy(payload + 28, data, len);

    pthread_mutex_lock(&ws_h264_clients_mutex);

    struct ws_h264_client **pp = &ws_h264_clients;

    while (*pp) {
        struct ws_h264_client *c = *pp;

        if (websocket_send_binary_frame(c->fd, payload, payload_len) != 0) {
            remove_ws_h264_client_locked(pp);
            continue;
        }

        pp = &c->next;
    }

    pthread_mutex_unlock(&ws_h264_clients_mutex);

    free(payload);
}

static void cache_h264_config_bytes(const unsigned char *data, size_t len) {
    if (!data || len == 0) {
        return;
    }

    pthread_mutex_lock(&h264_config_mutex);

    /*
     * 正常 SPS/PPS 很小。这里设置 256KB 上限，避免异常数据无限增长。
     */
    if (h264_config_cache_len + len > 256 * 1024) {
        h264_config_cache_len = 0;
    }

    if (h264_config_cache_len + len > h264_config_cache_cap) {
        size_t new_cap = h264_config_cache_cap ? h264_config_cache_cap * 2 : 4096;
        while (new_cap < h264_config_cache_len + len) {
            new_cap *= 2;
        }

        unsigned char *new_buf = (unsigned char *)realloc(h264_config_cache, new_cap);
        if (!new_buf) {
            h264_config_cache_len = 0;
            h264_config_cache_cap = 0;
            pthread_mutex_unlock(&h264_config_mutex);
            return;
        }

        h264_config_cache = new_buf;
        h264_config_cache_cap = new_cap;
    }

    memcpy(h264_config_cache + h264_config_cache_len, data, len);
    h264_config_cache_len += len;

    pthread_mutex_unlock(&h264_config_mutex);
}


static int send_cached_h264_config_to_ws_fd(socket_t fd, uint32_t width, uint32_t height) {
    int result = 0;

    pthread_mutex_lock(&h264_config_mutex);

    if (h264_config_cache && h264_config_cache_len > 0) {
        size_t payload_len = 28 + h264_config_cache_len;
        unsigned char *payload = (unsigned char *)malloc(payload_len);
        if (!payload) {
            pthread_mutex_unlock(&h264_config_mutex);
            return -1;
        }

        memcpy(payload, "AVC1", 4);
        write_be32_to_buf(payload + 4, width);
        write_be32_to_buf(payload + 8, height);
        write_be32_to_buf(payload + 12, 2);
        write_be64_to_buf(payload + 16, 0);
        write_be32_to_buf(payload + 24, (uint32_t)h264_config_cache_len);
        memcpy(payload + 28, h264_config_cache, h264_config_cache_len);

        result = websocket_send_binary_frame(fd, payload, payload_len);
        free(payload);
    }

    pthread_mutex_unlock(&h264_config_mutex);

    return result;
}

static void broadcast_cached_h264_config_to_ws_clients(uint32_t width, uint32_t height) {
    pthread_mutex_lock(&h264_config_mutex);

    if (!h264_config_cache || h264_config_cache_len == 0) {
        pthread_mutex_unlock(&h264_config_mutex);
        return;
    }

    size_t payload_len = 28 + h264_config_cache_len;
    unsigned char *payload = (unsigned char *)malloc(payload_len);
    if (!payload) {
        pthread_mutex_unlock(&h264_config_mutex);
        return;
    }

    memcpy(payload, "AVC1", 4);
    write_be32_to_buf(payload + 4, width);
    write_be32_to_buf(payload + 8, height);
    write_be32_to_buf(payload + 12, 2);
    write_be64_to_buf(payload + 16, 0);
    write_be32_to_buf(payload + 24, (uint32_t)h264_config_cache_len);
    memcpy(payload + 28, h264_config_cache, h264_config_cache_len);

    pthread_mutex_unlock(&h264_config_mutex);

    pthread_mutex_lock(&ws_h264_clients_mutex);

    struct ws_h264_client **pp = &ws_h264_clients;
    while (*pp) {
        struct ws_h264_client *c = *pp;
        if (websocket_send_binary_frame(c->fd, payload, payload_len) != 0) {
            remove_ws_h264_client_locked(pp);
            continue;
        }
        pp = &c->next;
    }

    pthread_mutex_unlock(&ws_h264_clients_mutex);

    free(payload);
}

static int send_cached_h264_config_to_fd(socket_t fd) {
    int result = 0;

    pthread_mutex_lock(&h264_config_mutex);

    if (h264_config_cache && h264_config_cache_len > 0) {
        result = send_blocking_all(fd, h264_config_cache, h264_config_cache_len);
    }

    pthread_mutex_unlock(&h264_config_mutex);

    return result;
}

static void broadcast_cached_h264_config_locked(void) {
    pthread_mutex_lock(&h264_config_mutex);

    if (!h264_config_cache || h264_config_cache_len == 0) {
        pthread_mutex_unlock(&h264_config_mutex);
        return;
    }

    struct h264_client **pp = &h264_clients;

    while (*pp) {
        struct h264_client *c = *pp;

        if (send_blocking_all(c->fd, h264_config_cache, h264_config_cache_len) != 0) {
            remove_h264_client_locked(pp);
            continue;
        }

        pp = &c->next;
    }

    pthread_mutex_unlock(&h264_config_mutex);
}

static void remove_h264_client_locked(struct h264_client **pp) {
    struct h264_client *dead = *pp;
    *pp = dead->next;
    close(dead->fd);
    free(dead);
}

static void broadcast_h264_bytes(const unsigned char *data, size_t len) {
    pthread_mutex_lock(&h264_clients_mutex);

    struct h264_client **pp = &h264_clients;

    while (*pp) {
        struct h264_client *c = *pp;

        /*
         * 这里改成阻塞式完整发送。
         * 原来的非阻塞 send 遇到 80KB 左右的 I 帧时，
         * 很容易只发送一部分就把客户端断开，ffplay 就会停在一帧不动。
         */
        if (send_blocking_all(c->fd, data, len) != 0) {
            remove_h264_client_locked(pp);
            continue;
        }

        pp = &c->next;
    }

    pthread_mutex_unlock(&h264_clients_mutex);
}

static void publish_h264_packet(
    FILE *save_file,
    uint32_t width,
    uint32_t height,
    uint32_t flags,
    uint64_t pts_us,
    unsigned char *data,
    uint32_t data_len
) {
    if (!data || data_len == 0) {
        return;
    }

    static const unsigned char start_code[4] = {0x00, 0x00, 0x00, 0x01};
    int need_start_code = !has_start_code(data, data_len);

    size_t packet_len = data_len + (need_start_code ? sizeof(start_code) : 0);
    unsigned char *packet = (unsigned char *)malloc(packet_len);
    if (!packet) {
        return;
    }

    size_t off = 0;
    if (need_start_code) {
        memcpy(packet + off, start_code, sizeof(start_code));
        off += sizeof(start_code);
    }

    memcpy(packet + off, data, data_len);

    /*
     * flags & 2: codec config，也就是 SPS/PPS。
     * 缓存它，供新客户端连接时补发。
     */
    if (flags & 2) {
        cache_h264_config_bytes(packet, packet_len);
    }

    /*
     * 每次 keyframe 前先广播一次 SPS/PPS。
     * 这样 ffplay 即使中途接入，也能在下一个 I 帧开始正常动起来。
     */
    if (flags & 1) {
        pthread_mutex_lock(&h264_clients_mutex);
        broadcast_cached_h264_config_locked();
        pthread_mutex_unlock(&h264_clients_mutex);

        broadcast_cached_h264_config_to_ws_clients(width, height);
    }

    if (save_file) {
        fwrite(packet, 1, packet_len, save_file);
        fflush(save_file);
    }

    broadcast_h264_bytes(packet, packet_len);
    broadcast_ws_h264_packet(width, height, flags, pts_us, packet, packet_len);

    uint64_t now = now_ms();

    pthread_mutex_lock(&h264_state_mutex);

    latest_width = width;
    latest_height = height;
    latest_flags = flags;
    latest_pts_us = pts_us;
    h264_packet_count++;
    h264_bytes_total += packet_len;
    h264_last_packet_ms = now;

    if (flags & 2) {
        h264_config_count++;
    }

    if (flags & 1) {
        h264_keyframe_count++;
    }

    pthread_mutex_unlock(&h264_state_mutex);

    free(packet);

    if (now - last_h264_print_ms >= 1000) {
        last_h264_print_ms = now;

        pthread_mutex_lock(&h264_state_mutex);
        printf(
            "h264 packets=%llu keyframes=%llu config=%llu bytes=%llu latest=%ux%u flags=%u size=%u clients=",
            (unsigned long long)h264_packet_count,
            (unsigned long long)h264_keyframe_count,
            (unsigned long long)h264_config_count,
            (unsigned long long)h264_bytes_total,
            latest_width,
            latest_height,
            latest_flags,
            data_len
        );
        pthread_mutex_unlock(&h264_state_mutex);

        pthread_mutex_lock(&h264_clients_mutex);
        int clients = 0;
        for (struct h264_client *c = h264_clients; c; c = c->next) {
            clients++;
        }
        pthread_mutex_unlock(&h264_clients_mutex);

        printf("%d\n", clients);
    }
}

static void *aoa_reader_thread(void *arg) {
    (void)arg;

    libusb_context *ctx = NULL;
    libusb_device_handle *handle = NULL;

    int interface_number = -1;
    int in_ep = 0;
    int out_ep = 0;

    int r = libusb_init(&ctx);
    if (r != 0) {
        printf("libusb_init failed: %d\n", r);
        running = 0;
        return NULL;
    }

    handle = open_aoa_device(ctx);
    if (!handle) {
        printf("AOA device not found\n");
        running = 0;
        libusb_exit(ctx);
        return NULL;
    }

    r = find_bulk_endpoints(handle, &interface_number, &in_ep, &out_ep);
    if (r != 0) {
        printf("bulk endpoints not found\n");
        running = 0;
        libusb_close(handle);
        libusb_exit(ctx);
        return NULL;
    }

#ifndef _WIN32
    if (libusb_kernel_driver_active(handle, interface_number) == 1) {
        printf("detaching kernel driver from interface %d\n", interface_number);
        libusb_detach_kernel_driver(handle, interface_number);
    }
#endif

    r = libusb_claim_interface(handle, interface_number);
    if (r != 0) {
        printf("claim interface failed: %s (%d)\n", libusb_error_name(r), r);
        running = 0;
        libusb_close(handle);
        libusb_exit(ctx);
        return NULL;
    }

    printf("AOA H.264 reader started\n");
    printf("saving raw H.264 to %s\n", H264_SAVE_PATH);

    FILE *save_file = fopen(H264_SAVE_PATH, "wb");
    if (!save_file) {
        perror("fopen h264 save file");
    }

    pthread_mutex_lock(&aoa_mutex);
    aoa_handle = handle;
    aoa_out_ep = out_ep;
    pthread_mutex_unlock(&aoa_mutex);

    if (aoa_hid_register_touch(handle) == 0) {
        pthread_mutex_lock(&hid_mutex);
        hid_ready = 1;
        pthread_mutex_unlock(&hid_mutex);
    } else {
        printf("AOA HID touch register failed\n");
    }

    struct usb_stream_reader reader;
    memset(&reader, 0, sizeof(reader));

    reader.handle = handle;
    reader.ep = in_ep;

    while (running) {
        unsigned char header[28] = {0};

        r = read_avc1_header(&reader, header);
        if (r != 0) {
            continue;
        }

        uint32_t width = read_be32(header + 4);
        uint32_t height = read_be32(header + 8);
        uint32_t flags = read_be32(header + 12);
        uint64_t pts_us = read_be64(header + 16);
        uint32_t data_len = read_be32(header + 24);

        if (data_len == 0 || data_len > MAX_H264_PACKET_SIZE) {
            printf("invalid h264 packet size: %u\n", data_len);
            continue;
        }

        unsigned char *data = (unsigned char *)malloc(data_len);
        if (!data) {
            printf("malloc failed for h264 packet %u bytes\n", data_len);
            continue;
        }

        r = read_exact(&reader, data, data_len);

        if (r == 0) {
            publish_h264_packet(save_file, width, height, flags, pts_us, data, data_len);
        }

        free(data);
    }

    if (save_file) {
        fclose(save_file);
    }

    pthread_mutex_lock(&hid_mutex);
    hid_ready = 0;
    pthread_mutex_unlock(&hid_mutex);

    aoa_hid_unregister_touch(handle);

    libusb_release_interface(handle, interface_number);

    pthread_mutex_lock(&aoa_mutex);
    aoa_handle = NULL;
    aoa_out_ep = 0;
    pthread_mutex_unlock(&aoa_mutex);

    libusb_close(handle);
    libusb_exit(ctx);

    return NULL;
}

/* =========================
 * HTTP / WebSocket
 * ========================= */

static int write_all(socket_t fd, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;

    while (len > 0) {
        int chunk = len > 0x7fffffffU ? 0x7fffffff : (int)len;
        ssize_t n = send(fd, (const char *)p, chunk, 0);

        if (n <= 0) {
            return -1;
        }

        p += n;
        len -= (size_t)n;
    }

    return 0;
}

static int recv_all(socket_t fd, void *buf, size_t len) {
    unsigned char *p = (unsigned char *)buf;

    while (len > 0) {
        int chunk = len > 0x7fffffffU ? 0x7fffffff : (int)len;
        ssize_t n = recv(fd, (char *)p, chunk, 0);

        if (n <= 0) {
            return -1;
        }

        p += n;
        len -= (size_t)n;
    }

    return 0;
}

static void serve_h264(socket_t client) {
    set_tcp_nodelay(client);

    const char *header =
        "HTTP/1.1 200 OK\r\n"
        "Cache-Control: no-cache\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "Content-Type: video/H264\r\n\r\n";

    if (write_all(client, header, strlen(header)) != 0) {
        return;
    }

    /*
     * 不再设置非阻塞。测试阶段只有一个 ffplay 客户端，
     * 阻塞式完整发送能避免 I 帧被截断导致画面停住。
     */

    if (send_cached_h264_config_to_fd(client) != 0) {
        return;
    }

    struct h264_client *c = (struct h264_client *)calloc(1, sizeof(*c));
    if (!c) {
        return;
    }

    c->fd = client;

    pthread_mutex_lock(&h264_clients_mutex);
    c->next = h264_clients;
    h264_clients = c;
    pthread_mutex_unlock(&h264_clients_mutex);

    printf("H.264 raw client connected\n");

    /*
     * 这个线程保持连接。
     * 真正的数据由 USB 读取线程通过 broadcast_h264_bytes() 推送。
     */
    while (running) {
        usleep(500000);
    }

    pthread_mutex_lock(&h264_clients_mutex);
    struct h264_client **pp = &h264_clients;
    while (*pp) {
        if (*pp == c) {
            *pp = c->next;
            free(c);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&h264_clients_mutex);

    printf("H.264 raw client disconnected\n");
}


static void serve_video_ws(socket_t client, const char *req) {
    set_tcp_nodelay(client);

    if (websocket_handshake(client, req) != 0) {
        return;
    }

    uint32_t width = 720;
    uint32_t height = 1568;

    pthread_mutex_lock(&h264_state_mutex);
    if (latest_width > 0 && latest_height > 0) {
        width = latest_width;
        height = latest_height;
    }
    pthread_mutex_unlock(&h264_state_mutex);

    /* 新浏览器连接后，先补发 SPS/PPS。 */
    if (send_cached_h264_config_to_ws_fd(client, width, height) != 0) {
        return;
    }

    struct ws_h264_client *c = (struct ws_h264_client *)calloc(1, sizeof(*c));
    if (!c) {
        return;
    }

    c->fd = client;

    pthread_mutex_lock(&ws_h264_clients_mutex);
    c->next = ws_h264_clients;
    ws_h264_clients = c;
    pthread_mutex_unlock(&ws_h264_clients_mutex);

    printf("Browser H.264 WebSocket client connected\n");

    /*
     * 这里直接返回，socket 交给广播列表维护。
     * 后续如果浏览器关闭，广播发送失败时会自动移除并 close。
     */
}

static int extract_json_int(const char *body, const char *key, int def) {
    char pattern[64];

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(body, pattern);
    if (!p) return def;

    p = strchr(p, ':');
    if (!p) return def;

    return atoi(p + 1);
}

static int json_has_type(const char *body, const char *type) {
    char pattern[64];

    snprintf(pattern, sizeof(pattern), "\"type\":\"%s\"", type);
    if (strstr(body, pattern)) return 1;

    snprintf(pattern, sizeof(pattern), "\"type\": \"%s\"", type);
    return strstr(body, pattern) != NULL;
}

static int execute_touch_action(const char *body) {
    if (json_has_type(body, "cursor")) {
        int x = extract_json_int(body, "x", 0);
        int y = extract_json_int(body, "y", 0);
        int width = extract_json_int(body, "width", 1080);
        int height = extract_json_int(body, "height", 2340);

        uint64_t now = now_ms();

        if (now - last_cursor_print_ms >= 50) {
            last_cursor_print_ms = now;
            printf("cursor x=%d y=%d width=%d height=%d\n", x, y, width, height);
        }

        return 1;

    } else if (json_has_type(body, "down")) {
        int x = extract_json_int(body, "x", 0);
        int y = extract_json_int(body, "y", 0);
        int width = extract_json_int(body, "width", 1080);
        int height = extract_json_int(body, "height", 2340);

        return aoa_hid_touch_down(x, y, width, height) == 0;

    } else if (json_has_type(body, "move")) {
        int x = extract_json_int(body, "x", 0);
        int y = extract_json_int(body, "y", 0);
        int width = extract_json_int(body, "width", 1080);
        int height = extract_json_int(body, "height", 2340);

        return aoa_hid_touch_move(x, y, width, height) == 0;

    } else if (json_has_type(body, "up")) {
        int x = extract_json_int(body, "x", 0);
        int y = extract_json_int(body, "y", 0);
        int width = extract_json_int(body, "width", 1080);
        int height = extract_json_int(body, "height", 2340);

        return aoa_hid_touch_up(x, y, width, height) == 0;

    } else if (json_has_type(body, "tap")) {
        int x = extract_json_int(body, "x", 0);
        int y = extract_json_int(body, "y", 0);
        int width = extract_json_int(body, "width", 1080);
        int height = extract_json_int(body, "height", 2340);

        return aoa_hid_tap(x, y, width, height) == 0;

    } else if (json_has_type(body, "doubletap")) {
        int x = extract_json_int(body, "x", 0);
        int y = extract_json_int(body, "y", 0);
        int width = extract_json_int(body, "width", 1080);
        int height = extract_json_int(body, "height", 2340);

        int r1 = aoa_hid_tap(x, y, width, height);
        usleep(120000);
        int r2 = aoa_hid_tap(x, y, width, height);

        return r1 == 0 && r2 == 0;

    } else if (json_has_type(body, "swipe")) {
        int x1 = extract_json_int(body, "x1", 0);
        int y1 = extract_json_int(body, "y1", 0);
        int x2 = extract_json_int(body, "x2", 0);
        int y2 = extract_json_int(body, "y2", 0);
        int width = extract_json_int(body, "width", 1080);
        int height = extract_json_int(body, "height", 2340);
        int duration = extract_json_int(body, "duration", 300);

        return aoa_hid_swipe(x1, y1, x2, y2, width, height, duration) == 0;
    }

    return 0;
}

static const char *find_header_value(const char *req, const char *name) {
    static char value[256];

    size_t name_len = strlen(name);
    const char *p = req;

    while (*p) {
        const char *line_end = strstr(p, "\r\n");

        if (!line_end) break;

        size_t line_len = (size_t)(line_end - p);

        if (line_len > name_len + 1) {
            int match = 1;

            for (size_t i = 0; i < name_len; i++) {
                if (tolower((unsigned char)p[i]) != tolower((unsigned char)name[i])) {
                    match = 0;
                    break;
                }
            }

            if (match && p[name_len] == ':') {
                const char *v = p + name_len + 1;

                while (*v == ' ' || *v == '\t') v++;

                size_t v_len = (size_t)(line_end - v);

                if (v_len >= sizeof(value)) v_len = sizeof(value) - 1;

                memcpy(value, v, v_len);
                value[v_len] = '\0';

                return value;
            }
        }

        p = line_end + 2;
    }

    return NULL;
}

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static uint32_t sha1_rotl(uint32_t v, unsigned int bits) {
    return (v << bits) | (v >> (32 - bits));
}

static void sha1_compute(const unsigned char *data, size_t len, unsigned char out[20]) {
    uint32_t h0 = 0x67452301U;
    uint32_t h1 = 0xEFCDAB89U;
    uint32_t h2 = 0x98BADCFEU;
    uint32_t h3 = 0x10325476U;
    uint32_t h4 = 0xC3D2E1F0U;
    uint64_t bit_len = (uint64_t)len * 8U;
    size_t padded_len = len + 1U + 8U;

    while (padded_len % 64U != 0U) {
        padded_len++;
    }

    unsigned char *msg = (unsigned char *)calloc(1, padded_len);
    if (!msg) {
        memset(out, 0, 20);
        return;
    }

    memcpy(msg, data, len);
    msg[len] = 0x80;

    for (int i = 0; i < 8; i++) {
        msg[padded_len - 1U - (size_t)i] = (unsigned char)((bit_len >> (8U * i)) & 0xffU);
    }

    for (size_t off = 0; off < padded_len; off += 64U) {
        uint32_t w[80];

        for (int i = 0; i < 16; i++) {
            const unsigned char *p = msg + off + (size_t)i * 4U;
            w[i] =
                ((uint32_t)p[0] << 24) |
                ((uint32_t)p[1] << 16) |
                ((uint32_t)p[2] << 8) |
                (uint32_t)p[3];
        }

        for (int i = 16; i < 80; i++) {
            w[i] = sha1_rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;

        for (int i = 0; i < 80; i++) {
            uint32_t f;
            uint32_t k;

            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999U;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1U;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCU;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6U;
            }

            uint32_t temp = sha1_rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = sha1_rotl(b, 30);
            b = a;
            a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    free(msg);

    uint32_t h[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; i++) {
        out[i * 4 + 0] = (unsigned char)((h[i] >> 24) & 0xff);
        out[i * 4 + 1] = (unsigned char)((h[i] >> 16) & 0xff);
        out[i * 4 + 2] = (unsigned char)((h[i] >> 8) & 0xff);
        out[i * 4 + 3] = (unsigned char)(h[i] & 0xff);
    }
}

static void base64_encode_20(const unsigned char input[20], char output[32]) {
    int i = 0;
    int j = 0;

    for (; i + 2 < 20; i += 3) {
        unsigned int v =
            ((unsigned int)input[i] << 16) |
            ((unsigned int)input[i + 1] << 8) |
            ((unsigned int)input[i + 2]);

        output[j++] = b64_table[(v >> 18) & 0x3f];
        output[j++] = b64_table[(v >> 12) & 0x3f];
        output[j++] = b64_table[(v >> 6) & 0x3f];
        output[j++] = b64_table[v & 0x3f];
    }

    if (i < 20) {
        unsigned int v = ((unsigned int)input[i] << 16);

        output[j++] = b64_table[(v >> 18) & 0x3f];

        if (i + 1 < 20) {
            v |= ((unsigned int)input[i + 1] << 8);

            output[j++] = b64_table[(v >> 12) & 0x3f];
            output[j++] = b64_table[(v >> 6) & 0x3f];
            output[j++] = '=';
        } else {
            output[j++] = b64_table[(v >> 12) & 0x3f];
            output[j++] = '=';
            output[j++] = '=';
        }
    }

    output[j] = '\0';
}

static int websocket_handshake(socket_t client, const char *req) {
    const char *key = find_header_value(req, "Sec-WebSocket-Key");

    if (!key || key[0] == '\0') {
        const char *resp =
            "HTTP/1.1 400 Bad Request\r\n"
            "Connection: close\r\n\r\n"
            "missing websocket key\n";

        write_all(client, resp, strlen(resp));
        return -1;
    }

    char combined[256];
    unsigned char sha1_result[20];
    char accept_key[32];

    snprintf(
        combined,
        sizeof(combined),
        "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
        key
    );

    sha1_compute((const unsigned char *)combined, strlen(combined), sha1_result);
    base64_encode_20(sha1_result, accept_key);

    char resp[512];

    snprintf(
        resp,
        sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n",
        accept_key
    );

    return write_all(client, resp, strlen(resp));
}

static int recv_all(socket_t fd, void *buf, size_t len);

static int websocket_read_text(socket_t client, char *out, size_t out_size) {
    unsigned char hdr[2];

    if (recv_all(client, hdr, 2) != 0) return -1;

    int fin = (hdr[0] & 0x80) != 0;
    int opcode = hdr[0] & 0x0f;
    int masked = (hdr[1] & 0x80) != 0;
    uint64_t payload_len = hdr[1] & 0x7f;

    if (!fin) return -1;
    if (opcode == 0x8) return 0;
    if (opcode != 0x1) return -1;

    if (payload_len == 126) {
        unsigned char ext[2];
        if (recv_all(client, ext, 2) != 0) return -1;
        payload_len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (payload_len == 127) {
        unsigned char ext[8];
        if (recv_all(client, ext, 8) != 0) return -1;
        payload_len = 0;
        for (int i = 0; i < 8; i++) payload_len = (payload_len << 8) | ext[i];
    }

    if (!masked) return -1;
    if (payload_len >= out_size) return -1;

    unsigned char mask[4];
    if (recv_all(client, mask, 4) != 0) return -1;
    if (recv_all(client, out, payload_len) != 0) return -1;

    for (uint64_t i = 0; i < payload_len; i++) {
        out[i] = (char)(((unsigned char)out[i]) ^ mask[i % 4]);
    }

    out[payload_len] = '\0';

    return (int)payload_len;
}

static int websocket_send_text(socket_t client, const char *text) {
    size_t len = strlen(text);
    unsigned char hdr[10];
    size_t hdr_len = 0;

    hdr[0] = 0x81;

    if (len < 126) {
        hdr[1] = (unsigned char)len;
        hdr_len = 2;
    } else if (len <= 65535) {
        hdr[1] = 126;
        hdr[2] = (unsigned char)((len >> 8) & 0xff);
        hdr[3] = (unsigned char)(len & 0xff);
        hdr_len = 4;
    } else {
        return -1;
    }

    if (write_all(client, hdr, hdr_len) != 0) return -1;
    return write_all(client, text, len);
}

static void serve_ws(socket_t client, const char *req) {
    set_tcp_nodelay(client);

    if (websocket_handshake(client, req) != 0) {
        return;
    }

    printf("WebSocket client connected\n");

    websocket_send_text(client, "{\"type\":\"hello\",\"status\":\"ok\"}");

    char message[4096];

    while (running) {
        int n = websocket_read_text(client, message, sizeof(message));

        if (n <= 0) break;

        int ok = execute_touch_action(message);

        if (!json_has_type(message, "move") && !json_has_type(message, "cursor")) {
            if (ok) websocket_send_text(client, "{\"status\":\"ok\"}");
            else websocket_send_text(client, "{\"status\":\"failed\"}");
        }
    }

    printf("WebSocket client disconnected\n");
}

static void serve_status(socket_t client) {
    uint32_t width, height, flags;
    uint64_t pts, packets, keyframes, configs, bytes, last_ms;

    pthread_mutex_lock(&h264_state_mutex);
    width = latest_width;
    height = latest_height;
    flags = latest_flags;
    pts = latest_pts_us;
    packets = h264_packet_count;
    keyframes = h264_keyframe_count;
    configs = h264_config_count;
    bytes = h264_bytes_total;
    last_ms = h264_last_packet_ms;
    pthread_mutex_unlock(&h264_state_mutex);

    int clients = 0;
    int browser_clients = 0;

    pthread_mutex_lock(&h264_clients_mutex);
    for (struct h264_client *c = h264_clients; c; c = c->next) clients++;
    pthread_mutex_unlock(&h264_clients_mutex);

    pthread_mutex_lock(&ws_h264_clients_mutex);
    for (struct ws_h264_client *wc = ws_h264_clients; wc; wc = wc->next) browser_clients++;
    pthread_mutex_unlock(&ws_h264_clients_mutex);

    char body[1024];

    snprintf(
        body,
        sizeof(body),
        "{"
        "\"video\":\"h264\","
        "\"width\":%u,"
        "\"height\":%u,"
        "\"flags\":%u,"
        "\"pts_us\":%llu,"
        "\"packets\":%llu,"
        "\"keyframes\":%llu,"
        "\"config_packets\":%llu,"
        "\"bytes\":%llu,"
        "\"last_packet_ms\":%llu,"
        "\"h264_clients\":%d,"
        "\"browser_video_clients\":%d,"
        "\"save_path\":\"%s\""
        "}\n",
        width,
        height,
        flags,
        (unsigned long long)pts,
        (unsigned long long)packets,
        (unsigned long long)keyframes,
        (unsigned long long)configs,
        (unsigned long long)bytes,
        (unsigned long long)last_ms,
        clients,
        browser_clients,
        H264_SAVE_PATH
    );

    char header[256];

    snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        strlen(body)
    );

    write_all(client, header, strlen(header));
    write_all(client, body, strlen(body));
}

static void serve_index(socket_t client) {
    const char *body =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>RK3568 AOA H.264 Browser Control</title>"
        "<style>"
        "body{margin:0;background:#111;color:#eee;font-family:Arial,sans-serif;text-align:center}"
        "h1{font-size:20px;margin:14px}"
        ".wrap{display:inline-block;position:relative;background:#000;border:1px solid #333;touch-action:none;user-select:none}"
        "canvas{display:block;max-width:100vw;max-height:86vh;background:#000;touch-action:none}"
        ".log{font-size:13px;color:#9ca3af;margin:8px;white-space:pre-wrap;line-height:1.5}"
        "code{color:#a7f3d0}"
        "</style>"
        "</head><body>"
        "<h1>RK3568 AOA H.264 Browser Control</h1>"
        "<div class='wrap' id='wrap'><canvas id='canvas' width='720' height='1568'></canvas></div>"
        "<div class='log' id='log'>Starting...</div>"
        "<script>"
        "let W=720,H=1568;"
        "const wrap=document.getElementById('wrap');"
        "const canvas=document.getElementById('canvas');"
        "const ctx=canvas.getContext('2d');"
        "const logEl=document.getElementById('log');"
        "let ws=null,wsReady=false;"
        "let vws=null,decoder=null,decoderReady=false;"
        "let pendingChunks=[];"
        "let frameCount=0,lastFpsTime=performance.now(),fps=0;"
        "let active=false,activeId=null;"
        "let downPoint=null,lastPoint=null;"
        "let lastMoveTime=0,lastCursorTime=0;"
        "let dragging=false,downSent=false;"
        "let longTimer=null;"
        "const MOVE_THRESHOLD=10;"
        "const LONG_PRESS_MS=180;"
        "const MOVE_INTERVAL_MS=16;"
        "const CURSOR_INTERVAL_MS=50;"
        "function log(s){logEl.textContent=s;}"
        "function setSize(w,h){"
        " if(w<=0||h<=0)return;"
        " if(W!==w||H!==h){W=w;H=h;canvas.width=W;canvas.height=H;}"
        " canvas.style.width='min(96vw,420px)';"
        " canvas.style.height='auto';"
        "}"
        "async function makeDecoder(){"
        " if(!('VideoDecoder' in window)){log('当前浏览器不支持 WebCodecs，无法在网页内播放 H.264。请换新版 Chrome/Edge。');return null;}"
        " const candidates=['avc1.640028','avc1.640032','avc1.64001f','avc1.4D4028','avc1.42E01E'];"
        " for(const codec of candidates){"
        "  const configs=["
        "   {codec:codec,codedWidth:W,codedHeight:H,optimizeForLatency:true,avc:{format:'annexb'}},"
        "   {codec:codec,codedWidth:W,codedHeight:H,optimizeForLatency:true}"
        "  ];"
        "  for(const cfg of configs){"
        "   try{"
        "    if(VideoDecoder.isConfigSupported){const r=await VideoDecoder.isConfigSupported(cfg);if(!r.supported)continue;}"
        "    const d=new VideoDecoder({"
        "     output:function(frame){"
        "      try{ctx.drawImage(frame,0,0,W,H);}finally{frame.close();}"
        "      frameCount++;"
        "      const now=performance.now();"
        "      if(now-lastFpsTime>=1000){fps=frameCount;frameCount=0;lastFpsTime=now;}"
        "     },"
        "     error:function(e){log('decoder error: '+e.message);}"
        "    });"
        "    d.configure(cfg);"
        "    log('video decoder ok: '+codec+' size '+W+'x'+H);"
        "    return d;"
        "   }catch(e){}"
        "  }"
        " }"
        " log('H.264 WebCodecs 配置失败。浏览器可能不支持当前 H.264 格式。');"
        " return null;"
        "}"
        "function readU32(dv,off){return dv.getUint32(off,false);}"
        "function readU64AsNumber(dv,off){const hi=dv.getUint32(off,false);const lo=dv.getUint32(off+4,false);return hi*4294967296+lo;}"
        "async function ensureDecoder(){"
        " if(decoderReady&&decoder)return decoder;"
        " decoder=await makeDecoder();"
        " decoderReady=!!decoder;"
        " if(decoderReady){const q=pendingChunks;pendingChunks=[];for(const item of q)decodePacket(item);}"
        " return decoder;"
        "}"
        "function decodePacket(buf){"
        " if(buf.byteLength<28)return;"
        " const dv=new DataView(buf);"
        " const magic=String.fromCharCode(dv.getUint8(0),dv.getUint8(1),dv.getUint8(2),dv.getUint8(3));"
        " if(magic!=='AVC1')return;"
        " const w=readU32(dv,4),h=readU32(dv,8),flags=readU32(dv,12);"
        " let pts=readU64AsNumber(dv,16);"
        " const size=readU32(dv,24);"
        " if(28+size>buf.byteLength)return;"
        " setSize(w,h);"
        " if(!decoderReady||!decoder){pendingChunks.push(buf);ensureDecoder();return;}"
        " const data=new Uint8Array(buf,28,size);"
        " const type=(flags&1)?'key':'delta';"
        " if(flags&2){pts=performance.now()*1000;}"
        " try{"
        "  if(decoder.decodeQueueSize>8){return;}"
        "  const chunk=new EncodedVideoChunk({type:type,timestamp:pts||Math.round(performance.now()*1000),data:data});"
        "  decoder.decode(chunk);"
        "  log('video '+W+'x'+H+' fps='+fps+' queue='+decoder.decodeQueueSize+' ws='+(wsReady?'ok':'wait'));"
        " }catch(e){log('decode error: '+e.message);}"
        "}"
        "function connectVideo(){"
        " const proto=(location.protocol==='https:')?'wss://':'ws://';"
        " vws=new WebSocket(proto+location.host+'/video');"
        " vws.binaryType='arraybuffer';"
        " vws.onopen=function(){log('video ws connected, waiting H.264...');};"
        " vws.onmessage=function(e){if(e.data instanceof ArrayBuffer)decodePacket(e.data);};"
        " vws.onerror=function(){log('video ws error');};"
        " vws.onclose=function(){log('video ws closed, reconnecting...');decoderReady=false;try{decoder&&decoder.close();}catch(_){}decoder=null;setTimeout(connectVideo,1000);};"
        "}"
        "function connectControlWs(){"
        " const proto=(location.protocol==='https:')?'wss://':'ws://';"
        " ws=new WebSocket(proto+location.host+'/ws');"
        " ws.onopen=function(){wsReady=true;log('control ws ok');};"
        " ws.onclose=function(){wsReady=false;log('control ws closed, reconnecting...');setTimeout(connectControlWs,1000);};"
        " ws.onerror=function(){wsReady=false;log('control ws error');};"
        " ws.onmessage=function(e){try{const m=JSON.parse(e.data);if(m.status)log('control '+m.status);}catch(_){}};"
        "}"
        "connectControlWs();connectVideo();"
        "function pt(e){"
        " const r=canvas.getBoundingClientRect();"
        " return {x:Math.max(0,Math.min(W-1,Math.round((e.clientX-r.left)*W/r.width))),y:Math.max(0,Math.min(H-1,Math.round((e.clientY-r.top)*H/r.height)))};"
        "}"
        "function send(a){"
        " if(!wsReady||!ws||ws.readyState!==1){log('control WebSocket not ready');return false;}"
        " ws.send(JSON.stringify(a));"
        " log(a.type+' x='+((a.x!==undefined)?a.x:'')+' y='+((a.y!==undefined)?a.y:'')+' size='+W+'x'+H);"
        " return true;"
        "}"
        "function sendCursor(p){send({type:'cursor',x:p.x,y:p.y,width:W,height:H});}"
        "function dist(a,b){const dx=a.x-b.x,dy=a.y-b.y;return Math.sqrt(dx*dx+dy*dy);}"
        "function clearLongTimer(){if(longTimer){clearTimeout(longTimer);longTimer=null;}}"
        "function startDrag(){if(!active||!downPoint||downSent)return;dragging=true;downSent=true;send({type:'down',x:downPoint.x,y:downPoint.y,width:W,height:H});}"
        "wrap.addEventListener('contextmenu',e=>{e.preventDefault();});"
        "wrap.addEventListener('pointerenter',e=>{sendCursor(pt(e));});"
        "wrap.addEventListener('pointermove',e=>{"
        " e.preventDefault();const p=pt(e);const now=Date.now();"
        " if(!active){if(now-lastCursorTime>=CURSOR_INTERVAL_MS){lastCursorTime=now;sendCursor(p);}return;}"
        " if(activeId!==e.pointerId)return;lastPoint=p;const d=dist(p,downPoint);"
        " if(!dragging&&d>=MOVE_THRESHOLD){clearLongTimer();startDrag();}"
        " if(dragging){if(now-lastMoveTime<MOVE_INTERVAL_MS)return;lastMoveTime=now;send({type:'move',x:p.x,y:p.y,width:W,height:H});}"
        " else{if(now-lastCursorTime>=CURSOR_INTERVAL_MS){lastCursorTime=now;sendCursor(p);}}"
        "});"
        "wrap.addEventListener('pointerdown',e=>{"
        " if(e.button!==0)return;e.preventDefault();wrap.setPointerCapture(e.pointerId);"
        " const p=pt(e);active=true;activeId=e.pointerId;downPoint=p;lastPoint=p;lastMoveTime=0;dragging=false;downSent=false;clearLongTimer();"
        " longTimer=setTimeout(()=>{startDrag();},LONG_PRESS_MS);log('ready x='+p.x+' y='+p.y+' size='+W+'x'+H);"
        "});"
        "wrap.addEventListener('pointerup',e=>{"
        " if(!active||activeId!==e.pointerId)return;e.preventDefault();clearLongTimer();const p=pt(e);"
        " if(dragging||downSent){send({type:'up',x:p.x,y:p.y,width:W,height:H});}else{send({type:'tap',x:p.x,y:p.y,width:W,height:H});}"
        " active=false;activeId=null;downPoint=null;lastPoint=null;dragging=false;downSent=false;"
        "});"
        "wrap.addEventListener('pointercancel',e=>{"
        " if(!active||activeId!==e.pointerId)return;clearLongTimer();const p=lastPoint||downPoint||pt(e);"
        " if(dragging||downSent){send({type:'up',x:p.x,y:p.y,width:W,height:H});}"
        " active=false;activeId=null;downPoint=null;lastPoint=null;dragging=false;downSent=false;"
        "});"
        "</script>"
        "</body></html>";

    char header[256];

    snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        strlen(body)
    );

    write_all(client, header, strlen(header));
    write_all(client, body, strlen(body));
}

static void serve_action(socket_t client, const char *req) {
    const char *body = strstr(req, "\r\n\r\n");

    if (!body) {
        const char *resp =
            "HTTP/1.1 400 Bad Request\r\n"
            "Connection: close\r\n\r\n"
            "missing body\n";

        write_all(client, resp, strlen(resp));
        return;
    }

    body += 4;

    int ok = execute_touch_action(body);

    if (ok) {
        const char *resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Connection: close\r\n\r\n"
            "ok\n";

        write_all(client, resp, strlen(resp));
    } else {
        const char *resp =
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Type: text/plain\r\n"
            "Connection: close\r\n\r\n"
            "hid touch action failed\n";

        write_all(client, resp, strlen(resp));
    }
}

static void handle_client(socket_t client) {
    char req[4096] = {0};

    ssize_t n = recv(client, req, (int)sizeof(req) - 1, 0);

    if (n <= 0) {
        close(client);
        return;
    }

    if (strncmp(req, "GET /h264", 9) == 0) {
        serve_h264(client);
        /*
         * serve_h264 内部把 fd 交给广播列表。
         * 这里不 close，广播线程会在客户端失败或程序退出时关闭。
         */
        return;
    } else if (strncmp(req, "GET /status", 11) == 0) {
        serve_status(client);
    } else if (strncmp(req, "GET /video", 10) == 0) {
        serve_video_ws(client, req);
        /* socket 交给 ws_h264_clients 列表，不能 close */
        return;
    } else if (strncmp(req, "GET /ws", 7) == 0) {
        serve_ws(client, req);
    } else if (strncmp(req, "POST /action", 12) == 0) {
        serve_action(client, req);
    } else {
        serve_index(client);
    }

    close(client);
}

static void *client_thread(void *arg) {
    struct client_arg *client_arg = (struct client_arg *)arg;
    socket_t client = client_arg->client;

    free(client_arg);

    handle_client(client);

    return NULL;
}

static int start_http_server(int port) {
    socket_t server = socket(AF_INET, SOCK_STREAM, 0);

    if (server == SOCKET_INVALID) {
        socket_perror("socket");
        return 1;
    }

    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        socket_perror("bind");
        close(server);
        return 1;
    }

    if (listen(server, 8) != 0) {
        socket_perror("listen");
        close(server);
        return 1;
    }

    set_nonblocking(server);

    printf("H.264/WebSocket server listening on http://0.0.0.0:%d\n", port);
    printf("raw H.264 endpoint: http://0.0.0.0:%d/h264\n", port);
    printf("browser H.264 page: http://0.0.0.0:%d/\n", port);
    printf("browser H.264 websocket: ws://0.0.0.0:%d/video\n", port);
    printf("status endpoint: http://0.0.0.0:%d/status\n", port);

    while (running) {
        socket_t client = accept(server, NULL, NULL);

        if (client == SOCKET_INVALID) {
            if (socket_would_block()) {
                usleep(10000);
                continue;
            }

            socket_perror("accept");
            break;
        }

        set_blocking(client);

        struct client_arg *arg = (struct client_arg *)malloc(sizeof(*arg));

        if (!arg) {
            close(client);
            continue;
        }

        arg->client = client;

        pthread_t tid;

        if (pthread_create(&tid, NULL, client_thread, arg) != 0) {
            fprintf(stderr, "pthread_create client failed\n");
            close(client);
            free(arg);
            continue;
        }

        pthread_detach(tid);
    }

    close(server);

    return 0;
}

static void print_usage(const char *argv0) {
    printf("Usage: %s [port] [--start-aoa] [--wait-aoa-ms N]\n", argv0);
    printf("  port             HTTP/WebSocket port, default %d\n", DEFAULT_PORT);
    printf("  --start-aoa      Try to switch a known phone VID/PID into AOA mode first\n");
    printf("  --wait-aoa-ms N  Wait time for AOA re-enumeration, default 5000\n");
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    int start_aoa = 0;
    struct aoa_start_options aoa_opts;

    aoa_start_default_options(&aoa_opts);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--start-aoa") == 0) {
            start_aoa = 1;
        } else if (strcmp(argv[i], "--wait-aoa-ms") == 0 && i + 1 < argc) {
            aoa_opts.wait_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            int parsed_port = atoi(argv[i]);

            if (parsed_port > 0) {
                port = parsed_port;
            } else {
                print_usage(argv[0]);
                return 1;
            }
        }
    }

    if (start_aoa) {
        int aoa_result = aoa_start_accessory_auto(&aoa_opts);

        if (aoa_result < 0) {
            printf("AOA auto-start failed\n");
            return 1;
        }

        if (aoa_result == 0) {
            printf("AOA auto-start skipped; receiver will try to open an existing AOA device\n");
        }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }
#endif

    pthread_t reader;

    if (pthread_create(&reader, NULL, aoa_reader_thread, NULL) != 0) {
        perror("pthread_create");
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    int result = start_http_server(port);

    running = 0;

    pthread_join(reader, NULL);

    pthread_mutex_lock(&ws_h264_clients_mutex);
    struct ws_h264_client *wc = ws_h264_clients;
    while (wc) {
        struct ws_h264_client *next = wc->next;
        close(wc->fd);
        free(wc);
        wc = next;
    }
    ws_h264_clients = NULL;
    pthread_mutex_unlock(&ws_h264_clients_mutex);

    pthread_mutex_lock(&h264_clients_mutex);
    struct h264_client *c = h264_clients;
    while (c) {
        struct h264_client *next = c->next;
        close(c->fd);
        free(c);
        c = next;
    }
    h264_clients = NULL;
    pthread_mutex_unlock(&h264_clients_mutex);

#ifdef _WIN32
    WSACleanup();
#endif

    return result;
}

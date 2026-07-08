#include "aoa_start.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define aoa_sleep_ms(ms) Sleep((DWORD)(ms))
#else
#include <unistd.h>
#define aoa_sleep_ms(ms) usleep((unsigned int)(ms) * 1000U)
#endif
#include <libusb.h>

#define VID_GOOGLE 0x18d1

#define AOA_GET_PROTOCOL    51
#define AOA_SEND_STRING     52
#define AOA_START_ACCESSORY 53

#define AOA_STRING_MANUFACTURER 0
#define AOA_STRING_MODEL        1
#define AOA_STRING_DESCRIPTION  2
#define AOA_STRING_VERSION      3
#define AOA_STRING_URI          4
#define AOA_STRING_SERIAL       5

struct aoa_usb_id {
    uint16_t vid;
    uint16_t pid;
    const char *name;
};

static const struct aoa_usb_id aoa_devices[] = {
    {VID_GOOGLE, 0x2d00, "AOA only"},
    {VID_GOOGLE, 0x2d01, "AOA + ADB"},
    {VID_GOOGLE, 0x2d02, "AOA audio"},
    {VID_GOOGLE, 0x2d03, "AOA audio + ADB"},
    {VID_GOOGLE, 0x2d04, "AOA accessory + audio"},
    {VID_GOOGLE, 0x2d05, "AOA accessory + audio + ADB"},
};

static const struct aoa_usb_id starter_devices[] = {
    {0x12d1, 0x107e, "Huawei P10 smartphone"},
    {0x2717, 0xff08, "Xiaomi ADB"},
    {0x2717, 0xff40, "Xiaomi MTP"},
};

static libusb_device_handle *open_from_list(
    libusb_context *ctx,
    const struct aoa_usb_id *devices,
    size_t count,
    int verbose
) {
    for (size_t i = 0; i < count; i++) {
        const struct aoa_usb_id *id = &devices[i];

        if (verbose) {
            printf("trying device: %04x:%04x (%s)\n", id->vid, id->pid, id->name);
        }

        libusb_device_handle *handle =
            libusb_open_device_with_vid_pid(ctx, id->vid, id->pid);

        if (handle) {
            printf("found device: %04x:%04x (%s)\n", id->vid, id->pid, id->name);
            return handle;
        }
    }

    return NULL;
}

static int send_string(libusb_device_handle *handle, int index, const char *str) {
    if (!str) {
        str = "";
    }

    int len = (int)strlen(str) + 1;

    int r = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        AOA_SEND_STRING,
        0,
        (uint16_t)index,
        (unsigned char *)str,
        (uint16_t)len,
        1000
    );

    if (r < 0) {
        printf("send_string index %d failed: %s (%d)\n",
               index,
               libusb_error_name(r),
               r);
        return r;
    }

    printf("send_string index %d ok: %s\n", index, str);
    return 0;
}

static int wait_for_aoa_device(libusb_context *ctx, int wait_ms) {
    int elapsed = 0;

    while (elapsed <= wait_ms) {
        libusb_device_handle *handle = open_from_list(
            ctx,
            aoa_devices,
            sizeof(aoa_devices) / sizeof(aoa_devices[0]),
            0
        );

        if (handle) {
            libusb_close(handle);
            return 1;
        }

        aoa_sleep_ms(250);
        elapsed += 250;
    }

    return 0;
}

void aoa_start_default_options(struct aoa_start_options *opts) {
    if (!opts) {
        return;
    }

    opts->manufacturer = "RK3568";
    opts->model = "RK-AOA-DEMO";
    opts->description = "RK3568 AOA Demo";
    opts->version = "1.0";
    opts->uri = "https://example.com";
    opts->serial = "rk3568-001";
    opts->wait_ms = 5000;
}

void aoa_start_print_candidate_devices(void) {
    printf("AOA starter candidate devices:\n");

    for (size_t i = 0; i < sizeof(starter_devices) / sizeof(starter_devices[0]); i++) {
        printf(
            "  %04x:%04x %s\n",
            starter_devices[i].vid,
            starter_devices[i].pid,
            starter_devices[i].name
        );
    }
}

void aoa_start_print_usb_devices(void) {
    libusb_context *ctx = NULL;
    libusb_device **list = NULL;

    int r = libusb_init(&ctx);
    if (r != 0) {
        printf("libusb_init failed: %s (%d)\n", libusb_error_name(r), r);
        return;
    }

    ssize_t count = libusb_get_device_list(ctx, &list);
    if (count < 0) {
        printf("libusb_get_device_list failed: %s (%d)\n", libusb_error_name((int)count), (int)count);
        libusb_exit(ctx);
        return;
    }

    printf("USB devices visible to libusb:\n");

    for (ssize_t i = 0; i < count; i++) {
        struct libusb_device_descriptor desc;
        r = libusb_get_device_descriptor(list[i], &desc);
        if (r != 0) {
            continue;
        }

        printf(
            "  bus %03u device %03u: %04x:%04x class=0x%02x\n",
            libusb_get_bus_number(list[i]),
            libusb_get_device_address(list[i]),
            desc.idVendor,
            desc.idProduct,
            desc.bDeviceClass
        );
    }

    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
}

int aoa_device_present(void) {
    libusb_context *ctx = NULL;
    int r = libusb_init(&ctx);

    if (r != 0) {
        printf("libusb_init failed: %s (%d)\n", libusb_error_name(r), r);
        return 0;
    }

    libusb_device_handle *handle = open_from_list(
        ctx,
        aoa_devices,
        sizeof(aoa_devices) / sizeof(aoa_devices[0]),
        0
    );

    if (handle) {
        libusb_close(handle);
        libusb_exit(ctx);
        return 1;
    }

    libusb_exit(ctx);
    return 0;
}

int aoa_start_accessory_auto(const struct aoa_start_options *opts) {
    struct aoa_start_options defaults;
    libusb_context *ctx = NULL;
    libusb_device_handle *handle = NULL;
    unsigned char protocol[2] = {0};

    if (!opts) {
        aoa_start_default_options(&defaults);
        opts = &defaults;
    }

    int r = libusb_init(&ctx);
    if (r != 0) {
        printf("libusb_init failed: %s (%d)\n", libusb_error_name(r), r);
        return -1;
    }

    handle = open_from_list(
        ctx,
        aoa_devices,
        sizeof(aoa_devices) / sizeof(aoa_devices[0]),
        0
    );

    if (handle) {
        printf("device is already in AOA mode\n");
        libusb_close(handle);
        libusb_exit(ctx);
        return 1;
    }

    handle = open_from_list(
        ctx,
        starter_devices,
        sizeof(starter_devices) / sizeof(starter_devices[0]),
        1
    );

    if (!handle) {
        printf("no AOA starter candidate device found\n");
        aoa_start_print_candidate_devices();
        libusb_exit(ctx);
        return 0;
    }

    r = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        AOA_GET_PROTOCOL,
        0,
        0,
        protocol,
        sizeof(protocol),
        1000
    );

    if (r != (int)sizeof(protocol)) {
        printf("AOA_GET_PROTOCOL failed: %s (%d)\n", libusb_error_name(r), r);
        libusb_close(handle);
        libusb_exit(ctx);
        return -1;
    }

    int version = protocol[0] | (protocol[1] << 8);
    printf("AOA protocol version: %d\n", version);

    if (send_string(handle, AOA_STRING_MANUFACTURER, opts->manufacturer) < 0 ||
        send_string(handle, AOA_STRING_MODEL, opts->model) < 0 ||
        send_string(handle, AOA_STRING_DESCRIPTION, opts->description) < 0 ||
        send_string(handle, AOA_STRING_VERSION, opts->version) < 0 ||
        send_string(handle, AOA_STRING_URI, opts->uri) < 0 ||
        send_string(handle, AOA_STRING_SERIAL, opts->serial) < 0) {
        libusb_close(handle);
        libusb_exit(ctx);
        return -1;
    }

    printf("starting accessory mode...\n");

    r = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        AOA_START_ACCESSORY,
        0,
        0,
        NULL,
        0,
        1000
    );

    if (r < 0) {
        printf("AOA_START_ACCESSORY failed: %s (%d)\n", libusb_error_name(r), r);
        libusb_close(handle);
        libusb_exit(ctx);
        return -1;
    }

    printf("AOA_START_ACCESSORY sent, waiting for AOA re-enumeration...\n");

    libusb_close(handle);
    handle = NULL;

    int found = wait_for_aoa_device(ctx, opts->wait_ms > 0 ? opts->wait_ms : 5000);
    libusb_exit(ctx);

    if (!found) {
        printf("AOA device did not appear before timeout\n");
        return -1;
    }

    printf("AOA device is ready\n");
    return 1;
}

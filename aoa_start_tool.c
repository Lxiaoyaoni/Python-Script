#include "aoa_start.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *argv0) {
    printf("Usage: %s [--list] [--list-usb] [--wait-ms N]\n", argv0);
    printf("  --list       Print starter candidate VID/PID list\n");
    printf("  --list-usb   Print USB VID/PID list visible to libusb\n");
    printf("  --wait-ms N  Wait time for AOA re-enumeration, default 5000\n");
}

int main(int argc, char **argv) {
    struct aoa_start_options opts;
    aoa_start_default_options(&opts);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            aoa_start_print_candidate_devices();
            return 0;
        } else if (strcmp(argv[i], "--list-usb") == 0) {
            aoa_start_print_usb_devices();
            return 0;
        } else if (strcmp(argv[i], "--wait-ms") == 0 && i + 1 < argc) {
            opts.wait_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    int r = aoa_start_accessory_auto(&opts);

    if (r > 0) {
        printf("AOA start ok\n");
        return 0;
    }

    if (r == 0) {
        printf("AOA start skipped: no starter device found\n");
        return 2;
    }

    printf("AOA start failed\n");
    return 1;
}

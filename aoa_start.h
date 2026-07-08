#ifndef AOA_START_H
#define AOA_START_H

#ifdef __cplusplus
extern "C" {
#endif

struct aoa_start_options {
    const char *manufacturer;
    const char *model;
    const char *description;
    const char *version;
    const char *uri;
    const char *serial;
    int wait_ms;
};

void aoa_start_default_options(struct aoa_start_options *opts);
int aoa_device_present(void);
int aoa_start_accessory_auto(const struct aoa_start_options *opts);
void aoa_start_print_candidate_devices(void);
void aoa_start_print_usb_devices(void);

#ifdef __cplusplus
}
#endif

#endif

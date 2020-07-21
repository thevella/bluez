//
// Created by ruundii on 20/07/2020.
//

#ifndef BLUEZ_INPUT_HOST_H
#define BLUEZ_INPUT_HOST_H
#include "lib/bluetooth.h"
#include <glib.h>

#define L2CAP_PSM_HIDP_CTRL	0x11
#define L2CAP_PSM_HIDP_INTR	0x13

int input_host_set_channel(const bdaddr_t *src, const bdaddr_t *dst, int psm, GIOChannel *io);
int input_host_remove(const bdaddr_t *src, const bdaddr_t *dst);

#endif //BLUEZ_INPUT_HOST_H

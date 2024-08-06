/*
 * Copyright 2024 Lars erling stensen <Lars.erling.stensen@huddly.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <fwupdplugin.h>

#define FU_TYPE_HUDDLY_USB_FIRMWARE (fu_huddly_usb_firmware_get_type())
G_DECLARE_FINAL_TYPE(FuHuddlyUsbFirmware,
		     fu_huddly_usb_firmware,
		     FU,
		     HUDDLY_USB_FIRMWARE,
		     FuFirmware)

FuFirmware *
fu_huddly_usb_firmware_new(void);
guint16
fu_huddly_usb_firmware_get_start_addr(FuHuddlyUsbFirmware *self);

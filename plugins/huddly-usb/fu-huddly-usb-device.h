/*
 * Copyright 2024 Lars erling stensen <Lars.erling.stensen@huddly.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <fwupdplugin.h>

#define FU_TYPE_HUDDLY_USB_DEVICE (fu_huddly_usb_device_get_type())
G_DECLARE_FINAL_TYPE(FuHuddlyUsbDevice,
		     fu_huddly_usb_device,
		     FU,
		     HUDDLY_USB_DEVICE,
		     FuUsbDevice)

guint16
fu_huddly_usb_device_get_start_addr(FuHuddlyUsbDevice *self);

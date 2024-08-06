/*
 * Copyright 2024 Lars erling stensen <Lars.erling.stensen@huddly.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <fwupdplugin.h>

G_DECLARE_FINAL_TYPE(FuHuddlyUsbPlugin,
		     fu_huddly_usb_plugin,
		     FU,
		     HUDDLY_USB_PLUGIN,
		     FuPlugin)

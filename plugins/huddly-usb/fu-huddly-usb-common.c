/*
 * Copyright 2024 Lars erling stensen <Lars.erling.stensen@huddly.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "fu-huddly-usb-common.h"

const gchar *
fu_huddly_usb_strerror(guint8 code)
{
	if (code == 0)
		return "success";
	return NULL;
}

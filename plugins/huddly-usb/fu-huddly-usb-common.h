/*
 * Copyright 2024 Huddly
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib-object.h>

const gchar *
fu_huddly_usb_strerror(guint8 code);

typedef struct {
	guint32 req_id;
	guint32 res_id;
	guint16  flags;
	guint16 msg_name_size;
	guint32 payload_size;
} HLinkHeader;

typedef struct {
    HLinkHeader header;
    gchar* msg_name;
    GByteArray *payload;
} HLinkBuffer;


HLinkBuffer* fu_huddly_usb_hlink_buffer_create(const gchar* msg_name, GByteArray *payload);
gboolean fu_huddly_usb_hlink_buffer_to_packet(GByteArray *packet, HLinkBuffer *buffer, GError **error);
gboolean fu_huddly_usb_packet_to_hlink_buffer(HLinkBuffer *buffer, guint8 *packet, guint32 packet_sz, GError **error);

void fu_huddly_usb_hlink_buffer_free(HLinkBuffer* buffer);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(HLinkBuffer, fu_huddly_usb_hlink_buffer_free)
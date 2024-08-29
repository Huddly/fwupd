/*
 * Copyright 2024 Huddly
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include <fwupdplugin.h>

#include "fu-huddly-usb-common.h"

const gchar *
fu_huddly_usb_strerror(guint8 code)
{
	if (code == 0)
		return "success";
	return NULL;
}

void
fu_huddly_usb_hlink_buffer_free(HLinkBuffer *buffer)
{
	if(buffer->msg_name != NULL){
    		g_free(buffer->msg_name);
    		buffer->msg_name = NULL;
	}
	if(buffer->payload != NULL){
		g_byte_array_unref(buffer->payload);
	}
	g_free(buffer);
	buffer = NULL;
}

static gsize
fu_huddly_usb_hlink_packet_size(HLinkBuffer *buffer)
{
    return sizeof(HLinkHeader) + buffer->header.msg_name_size + buffer->header.payload_size;
}

HLinkBuffer *
fu_huddly_usb_hlink_buffer_create(const gchar *msg_name, GByteArray *payload)
{
	g_autoptr(HLinkBuffer) buffer = g_new0(HLinkBuffer, 1);
	memset(&buffer->header, 0x00, sizeof(HLinkHeader));
	buffer->header.msg_name_size = strlen(msg_name);
	buffer->msg_name =(gchar*)g_malloc(buffer->header.msg_name_size);
	// change to fu_memcpy_safe
	memcpy(buffer->msg_name, msg_name, buffer->header.msg_name_size);

	if(payload != NULL){
		buffer->header.payload_size = payload->len;
		buffer->payload = g_byte_array_sized_new(payload->len);
		g_byte_array_append(buffer->payload, payload->data, payload->len);
	}

	return g_steal_pointer(&buffer);
}

gboolean
fu_huddly_usb_hlink_buffer_to_packet(GByteArray *packet, HLinkBuffer *buffer, GError **error)
{
	guint32 offset = 0;
	gsize pkt_sz = fu_huddly_usb_hlink_packet_size(buffer);
	fu_byte_array_set_size(packet, pkt_sz, 0u);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
	if (!fu_memcpy_safe(packet->data,
			    packet->len,
			    0,
			    (guint8 *)&buffer->header,
			    sizeof(HLinkHeader),
			    0,
			    sizeof(HLinkHeader),
			    error)) {
		return FALSE;
	}
	offset += sizeof(HLinkHeader);
	if(!fu_memcpy_safe(packet->data, packet->len, offset, (guint8*)buffer->msg_name, buffer->header.msg_name_size, 0, buffer->header.msg_name_size, error))
	{
		return FALSE;
	}
	offset += buffer->header.msg_name_size;
	if(buffer->header.payload_size > 0)
	{
		if(!fu_memcpy_safe(packet->data, packet->len, offset, buffer->payload->data, buffer->payload->len, 0, buffer->header.payload_size, error))
		{
			return FALSE;
		}
    	}
    	return TRUE;
}

gboolean
fu_huddly_usb_packet_to_hlink_buffer(HLinkBuffer *buffer,
				     guint8 *packet,
				     guint32 packet_sz,
				     GError **error)
{
	gsize offset = 0;
	if(packet_sz < sizeof(HLinkHeader)){
		return FALSE;
	}
    	if(!fu_memcpy_safe((guint8*)&buffer->header, sizeof(HLinkHeader), 0, packet, packet_sz, 0, sizeof(HLinkHeader), error)){
		return FALSE;
	}

	if(packet_sz < fu_huddly_usb_hlink_packet_size(buffer))
	{
		return FALSE;
	}
	offset = sizeof(HLinkHeader);
	buffer->msg_name = g_new0(gchar, buffer->header.msg_name_size);
	if(!fu_memcpy_safe((guint8*)buffer->msg_name, buffer->header.msg_name_size, 0, packet, packet_sz, offset, buffer->header.msg_name_size, error)){
		return FALSE;
	}

	offset += buffer->header.msg_name_size;
	buffer->payload = g_byte_array_sized_new(buffer->header.payload_size);
	g_byte_array_set_size(buffer->payload, buffer->header.payload_size);
	if(!fu_memcpy_safe(buffer->payload->data, buffer->payload->len, 0, packet, packet_sz, offset, buffer->header.payload_size, error)){
		return FALSE;
	}
    	return TRUE;
}

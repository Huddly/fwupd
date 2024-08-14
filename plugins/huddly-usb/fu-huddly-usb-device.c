/*
 * Copyright 2024 Lars erling stensen <Lars.erling.stensen@huddly.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "fu-huddly-usb-common.h"
#include "fu-huddly-usb-device.h"
#include "fu-huddly-usb-firmware.h"
#include "fu-huddly-usb-struct.h"

enum { EP_OUT, EP_IN, EP_LAST };

/* this can be set using Flags=example in the quirk file  */
#define FU_HUDDLY_USB_DEVICE_FLAG_EXAMPLE (1 << 0)

struct _FuHuddlyUsbDevice {
	FuUsbDevice parent_instance;
	guint16 start_addr;
	guint bulk_ep[EP_LAST];
	gboolean initialized;
	gboolean interfaces_claimed;
};

G_DEFINE_TYPE(FuHuddlyUsbDevice, fu_huddly_usb_device, FU_TYPE_USB_DEVICE)


static gboolean fu_huddly_usb_device_find_interface(FuDevice *device, GError **error)
{
	FuHuddlyUsbDevice *self = FU_HUDDLY_USB_DEVICE(device);
	g_autoptr(GPtrArray) intfs = NULL;
	intfs = fu_usb_device_get_interfaces(FU_USB_DEVICE(device), error);
	if(intfs != NULL)
	{
		for(guint i = 0; i < intfs->len; i++){
			FuUsbInterface *intf = g_ptr_array_index(intfs, i);
			if(fu_usb_interface_get_class(intf) == FU_USB_CLASS_VENDOR_SPECIFIC)
			{
				g_autoptr(GPtrArray) endpoints = fu_usb_interface_get_endpoints(intf);
			
				for(guint j = 0; j < endpoints->len; j++)
				{
					FuUsbEndpoint* ep = g_ptr_array_index(endpoints, j);
					if(fu_usb_endpoint_get_direction(ep) == FU_USB_DIRECTION_HOST_TO_DEVICE)
					{
						self->bulk_ep[EP_OUT] = fu_usb_endpoint_get_address(ep);
					}
					else
					{
						self->bulk_ep[EP_IN] = fu_usb_endpoint_get_address(ep);
					}
				}
			}
		}
		return TRUE;
	}
	else
	{
		g_print("ERROR: Could not find interface\n");
		return FALSE;
	}
}

/**
 * Detach and claim video and audio interfaces before upgrading
 */
static gboolean fu_huddly_usb_interface_detach_media_kernel_drivers(FuDevice* device, GError **error)
{
	FuHuddlyUsbDevice *self = FU_HUDDLY_USB_DEVICE(device);
	g_autoptr(GPtrArray) intfs = NULL;

	if(self->interfaces_claimed)
	{
		//Interfaces already claimed. Nothing to do
		return TRUE;
	}
	g_print("Detach media drivers\n");
	intfs = fu_usb_device_get_interfaces(FU_USB_DEVICE(device), error);
	if(intfs != NULL)
	{
		for(guint i=0; i < intfs->len; i++)
		{
			guint8 interface_class;
			FuUsbInterface *intf = g_ptr_array_index(intfs, i);
			interface_class = fu_usb_interface_get_class(intf);
			if((interface_class == FU_USB_CLASS_AUDIO) || (interface_class == FU_USB_CLASS_VIDEO))
			{
				guint8 iface_number = fu_usb_interface_get_number(intf);
				if(!fu_usb_device_claim_interface(FU_USB_DEVICE(device), iface_number, FU_USB_DEVICE_CLAIM_FLAG_KERNEL_DRIVER, error))
				{
					g_error("Failed to claim USB media interface\n");
					return FALSE;
				}
			}
		}
	}else
	{
		return FALSE;
	}
	self->interfaces_claimed = TRUE;
	return TRUE;
}

/* Reattach media kernel drivers after update */
static gboolean fu_huddly_usb_interface_reattach_media_kernel_drivers(FuDevice* device, GError **error)
{
	FuHuddlyUsbDevice *self = FU_HUDDLY_USB_DEVICE(device);
	g_autoptr(GPtrArray) intfs = NULL;

	if(!self->interfaces_claimed)
	{
		//Interfaces have not been claimed. There is nothing to release
		return TRUE;
	}
	g_print("Reattach media drivers\n");
	intfs = fu_usb_device_get_interfaces(FU_USB_DEVICE(device), error);
	if(intfs != NULL)
	{
		for(guint i=0; i < intfs->len; i++)
		{
			guint8 interface_class, interface_subclass;
			FuUsbInterface *intf = g_ptr_array_index(intfs, i);
			interface_class = fu_usb_interface_get_class(intf);
			interface_subclass = fu_usb_interface_get_subclass(intf);
			if(((interface_class == FU_USB_CLASS_AUDIO) || (interface_class == FU_USB_CLASS_VIDEO)) && (interface_subclass == 0x01))
			{
				guint8 iface_number = fu_usb_interface_get_number(intf);
				if(!fu_usb_device_release_interface(FU_USB_DEVICE(device), iface_number, FU_USB_DEVICE_CLAIM_FLAG_KERNEL_DRIVER, error))
				{
					g_error("Failed to relase USB media interface\n");
					return FALSE;
				}
			}
		}
	}else
	{
		return FALSE;
	}
	self->interfaces_claimed = TRUE;
	return TRUE;
}


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
    guint8 *payload;
} HLinkBuffer;


static void fu_huddly_usb_hlink_buffer_free(HLinkBuffer* buffer)
{
	if(buffer->msg_name != NULL){
    		g_free(buffer->msg_name);
    		buffer->msg_name = NULL;
	}
	if(buffer->payload != NULL){
		g_free(buffer->payload);
		buffer->payload = NULL;
	}
	g_free(buffer);
	buffer = NULL;
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(HLinkBuffer, fu_huddly_usb_hlink_buffer_free)


static HLinkBuffer* fu_huddly_usb_hlink_buffer_create(const gchar* msg_name, guint8* payload, guint32 payload_size)
{
	HLinkBuffer* buffer = g_new0(HLinkBuffer, 1);
	memset(&buffer->header, 0x00, sizeof(HLinkHeader));
	buffer->header.msg_name_size = strlen(msg_name);
	buffer->msg_name =(gchar*)g_malloc(buffer->header.msg_name_size);
	// change to fu_memcpy_safe
	memcpy(buffer->msg_name, msg_name, buffer->header.msg_name_size);
    
    	buffer->header.payload_size = payload_size;
    	buffer->payload = (guint8*)g_malloc(buffer->header.payload_size);
    	memcpy(buffer->payload, payload, payload_size);

	return buffer;
}


static HLinkBuffer* fu_huddly_usb_hlink_buffer_from_str(const gchar* msg_name, const gchar* body)
{
    guint32 payload_length = strlen(body);
    return fu_huddly_usb_hlink_buffer_create(msg_name, (guint8*)body, payload_length);
}

static gsize fu_huddly_usb_hlink_packet_size(HLinkBuffer *buffer)
{
    return sizeof(HLinkHeader) + buffer->header.msg_name_size + buffer->header.payload_size;
}

static void fu_huddly_usb_hlink_buffer_to_packet(GByteArray *packet, HLinkBuffer *buffer, GError **error)
{
    guint32 offset = 0; 
    gsize pkt_sz = fu_huddly_usb_hlink_packet_size(buffer);
    fu_byte_array_set_size(packet, pkt_sz, 0u);
    if(!fu_memcpy_safe(packet->data, packet->len, 0, (guint8*)&buffer->header, sizeof(HLinkHeader), 0, sizeof(HLinkHeader), error)){
	g_error("Memcpy 1 failed\n");
    }
    offset += sizeof(HLinkHeader);
    if(!fu_memcpy_safe(packet->data, packet->len, offset, (guint8*)buffer->msg_name, buffer->header.msg_name_size, 0, buffer->header.msg_name_size, error))
    {
	g_error("Memcpy 2 failed\n");
    }
    offset += buffer->header.msg_name_size;
    if(buffer->header.payload_size > 0)
    {
    	if(!fu_memcpy_safe(packet->data, packet->len, offset, buffer->payload, buffer->header.payload_size, 0, buffer->header.payload_size, error))
    	{
		g_error("Memcpy 3 failed\n");
    	}
    }
}

static gboolean fu_huddly_usb_packet_to_hlink_buffer(HLinkBuffer *buffer, guint8 *packet, guint32 packet_sz, GError **error)
{
	gsize offset = 0;
	gboolean res;
	if(packet_sz < sizeof(HLinkHeader)){
		g_printerr("Insufficient packet size\n");
		return FALSE;
	}
	g_print("Copy header information\n");
    	res = fu_memcpy_safe((guint8*)&buffer->header, sizeof(HLinkHeader), 0, packet, packet_sz, 0, sizeof(HLinkHeader), error);
	if(!res){
		g_error("Copy header failed\n");
		return FALSE;
	}

	if(packet_sz < fu_huddly_usb_hlink_packet_size(buffer))
	{
		g_error("Packet size too small\n");
		return FALSE;
	}
	offset = sizeof(HLinkHeader);
	buffer->msg_name = g_new0(gchar, buffer->header.msg_name_size);
	res = fu_memcpy_safe((guint8*)buffer->msg_name, buffer->header.msg_name_size, 0, packet, packet_sz, offset, buffer->header.msg_name_size, error);
    	if(!res){
		g_error("Copy msg name failed\n");
		return FALSE;
	}

	offset += buffer->header.msg_name_size;
	buffer->payload = g_new0(guint8, buffer->header.payload_size);
	res = fu_memcpy_safe(buffer->payload, buffer->header.payload_size, 0, packet, packet_sz, offset, buffer->header.payload_size, error);
	if(!res){
		g_error("Copy msg payload failed\n");
		return FALSE;
	}
    	return TRUE;
}


static gboolean fu_huddly_usb_device_bulk_write(FuDevice *device, GByteArray* buf, GError **error)
{
	gsize actual_size = 0;
	FuHuddlyUsbDevice* self = FU_HUDDLY_USB_DEVICE(device);
	return fu_usb_device_bulk_transfer(FU_USB_DEVICE(device), 
		self->bulk_ep[EP_OUT], 
		buf->data,
		buf->len,
		&actual_size, 
		2000, 
		NULL, 
		error
		);
}

static gboolean fu_huddly_usb_device_bulk_read(FuDevice *device, GByteArray* buf, gsize* received_length, GError **error)
{
	FuHuddlyUsbDevice* self = FU_HUDDLY_USB_DEVICE(device);
	return fu_usb_device_bulk_transfer(FU_USB_DEVICE(device), self->bulk_ep[EP_IN], 
		buf->data,
		buf->len, 
		received_length,
		20000, 
		NULL, 
		error
		);
	
}

static gboolean fu_huddly_usb_device_hlink_send(FuDevice* device, HLinkBuffer* buffer, GError **error)
{
	g_autoptr(GByteArray) buf = g_byte_array_new();

	gsize actual_size = 0;
	fu_huddly_usb_hlink_buffer_to_packet(buf, buffer, error);
	return fu_huddly_usb_device_bulk_write(device, buf, error);
}

static gboolean fu_huddly_usb_device_hlink_receive(FuDevice* device, HLinkBuffer *buffer, GError **error)
{
	#define RECEIVE_BUFFER_SIZE 1024
	g_autoptr(GByteArray) buf = g_byte_array_new();
	gsize received_length = 0;
	
	fu_byte_array_set_size(buf, RECEIVE_BUFFER_SIZE, 0u);
	if(!fu_huddly_usb_device_bulk_read(device, buf, &received_length, error))
	{ 
		g_error("Failed hlink receive\n");
		g_prefix_error(error, "Error: ");
		return FALSE;
	}
	else
	{
		g_print("Received data. Creating hlink buffer\n");
		if(!fu_huddly_usb_packet_to_hlink_buffer(buffer, buf->data, received_length, error)){
			g_error("Failed to create hlink buffer\n");
			return FALSE;
		}
		g_print("Hlink receive OK!\n");
		return TRUE;
	}
	
	
}

static gboolean fu_huddly_usb_device_hlink_subscribe(FuDevice* device, const gchar* subscription, GError** error)
{
	gboolean result = FALSE;
	g_autoptr(HLinkBuffer) send_buffer = fu_huddly_usb_hlink_buffer_from_str("hlink-mb-subscribe", subscription);;
	g_print("Subscribe %s\n", subscription);
	
	result = fu_huddly_usb_device_hlink_send(device, send_buffer, error);
	if(result){
		g_print("OK\n");
	}
	else{
		g_print("FAILED\n");
	}
	// fu_huddly_usb_hlink_buffer_free(&send_buffer);
	return result;
}

static gboolean fu_huddly_usb_device_hlink_unsubscribe(FuDevice* device, const gchar* subscription, GError** error)
{
	gboolean result = FALSE;
	g_autoptr(HLinkBuffer) send_buffer = fu_huddly_usb_hlink_buffer_from_str("hlink-mb-unsubscribe", subscription);
	g_print("Unsubscribe %s\n", subscription);
	
	result = fu_huddly_usb_device_hlink_send(device, send_buffer, error);
	if(result){
		g_print("OK\n");
	}
	else{
		g_print("FAILED\n");
	}
	// fu_huddly_usb_hlink_buffer_free(&send_buffer);
	return result;
}

/** Send an empty packet to reset hlink communications */
static void fu_huddly_usb_device_send_reset(FuDevice *device, GError **error)
{
	g_autoptr(GByteArray) packet = g_byte_array_new(); //Empty packet

	fu_huddly_usb_device_bulk_write(device, packet, error);
}

/** Send a hlink salute and receive a response from the device */
static void fu_huddly_usb_device_salute(FuDevice *device, GError **error)
{
	g_autoptr(GByteArray) salutation = g_byte_array_new();
	g_autoptr(GByteArray) response = g_byte_array_new();
	guint8 data = 0x00;
	gsize received_length = 0;
	g_byte_array_append(salutation, &data, 1);

	fu_huddly_usb_device_bulk_write(device, salutation, error);
	
	g_byte_array_set_size(response, 100);

	fu_huddly_usb_device_bulk_read(device, response, &received_length, error);
	response->data[received_length] = '\0';
	g_print("Received response %s\n", (gchar*)response->data);
}

/* Get information about a string from a msgpack formatted buffer. The string info includes the 
 format length as well as the string length. 
 */
static gboolean fu_huddly_usb_device_get_pack_string_info(gsize* format_bytes, gsize* string_length, guint8* p){
     if((*p & 0xe0) == 0xa0){
        //fixstr
        *string_length = (*p & 0x1f);
        *format_bytes = 1;
        return TRUE;
    }
    else if(*p == 0xd9)
    {
        *string_length = (gsize)*(p+1);
        *format_bytes = 2;
        return TRUE;
    }
    else if(*p == 0xda)
    {
        *string_length = ((gsize)*(p+1) << 8) | (gsize)*(p+2);
        *format_bytes = 3;
        return TRUE;
    }
    else if(*p == 0xdb)
    {
        *string_length = ((gsize)*(p+1) << 24) | ((gsize)*(p+2) << 16) | ((gsize)*(p+3) << 8) | (gsize)*(p+4);
        *format_bytes = 5;
        return TRUE;
    }
    g_error("Type 0x%x not a string\n", *p);
    return FALSE;
}

/** Responses are transmitted as msgpack maps as key-value pairs
 * Search for a string in a map and return the following string value. 
 */
static GString *fu_huddly_usb_device_get_pack_string(guint8* buffer, gsize buffer_size, const gchar* key)
{
    gchar *cursor;
    gsize string_length;
    gsize format_bytes;
    GString* value = NULL;
    cursor = (gchar*)buffer;
    cursor = g_strstr_len(cursor, buffer_size, key);
    if(!cursor){
        g_error("Could not find key\n");
    }
    cursor += strlen(key);
    if(!fu_huddly_usb_device_get_pack_string_info(&format_bytes, &string_length, (guint8*)cursor)){
        return value;
    }
    cursor = cursor + format_bytes;
    value = g_string_new_len(cursor, string_length);
    return value;
}

/**
 * Trim string at first occurrence of c
 */
void fu_huddly_usb_device_trim_string_at(GString *str, gchar c)
{
	for(gsize i=0; i<str->len; i++)
	{
		if(*(str->str + i) == c){
			g_string_truncate(str, i);
			return;
		}
	}
}

static GString* fu_huddly_usb_device_get_version(FuDevice* device, GError **error){
	GString* version_string = NULL;
	g_autoptr(HLinkBuffer) send_buf = NULL, receive_buf = NULL;
	gboolean res = FALSE;
	gboolean subscribed = FALSE;
	

	res = fu_huddly_usb_device_hlink_subscribe(device, "prodinfo/get_msgpack_reply", error);
	if(!res)
	{
		g_print("Subscribe failed\n");
		g_prefix_error(error, "Subscribe failed with error: ");
	}
	else{
		subscribed = TRUE;
	}
	if(res)
	{
		send_buf = fu_huddly_usb_hlink_buffer_create("prodinfo/get_msgpack", NULL, 0);
		res = fu_huddly_usb_device_hlink_send(device, send_buf, error);
		if(!res){
			g_prefix_error(error, "Send failed with error: ");
		}else{
			g_print("Send OK\n");
		}
	}
	if(res)
	{
		receive_buf = g_new0(HLinkBuffer, 1);
		res = fu_huddly_usb_device_hlink_receive(device, receive_buf, error);
		if(!res){
			g_prefix_error(error, "Receive failed with error: ");
		}

		g_print("Receive data %s\n", receive_buf->msg_name);

		version_string = fu_huddly_usb_device_get_pack_string(receive_buf->payload, receive_buf->header.payload_size, "app_version");
	}
	if(subscribed){
		res = fu_huddly_usb_device_hlink_unsubscribe(device, "prodinfo/get_msgpack_reply", error);
		if(!res){
			g_prefix_error(error, "Unsubscribe failed with error:");
		}else{
			g_print("UNSUBSCribe OK\n");
		}
	}
	return version_string;
}

// static void fu_huddly_usb_hlink_vsc_connect(FuHuddlyUsbDevice* device){
// 	guint8 interface_hid = 0;
// 	// Find interface
// 	interface_hid = fu_usb_device_get_interface_for_class(FU_USB_DEVICE(device), FU_USB_CLASS_)

// 	// Claim usb interface
// 	fu_usb_device_open()


// 	// Send hlink reset

// 	// send hlink salute
// }



static void
fu_huddly_usb_device_to_string(FuDevice *device, guint idt, GString *str)
{
	FuHuddlyUsbDevice *self = FU_HUDDLY_USB_DEVICE(device);
	fwupd_codec_string_append_hex(str, idt, "StartAddr", self->start_addr);
}

/* TODO: this is only required if the device instance state is required elsewhere */
guint16
fu_huddly_usb_device_get_start_addr(FuHuddlyUsbDevice *self)
{
	g_return_val_if_fail(FU_IS_HUDDLY_USB_DEVICE(self), G_MAXUINT16);
	return self->start_addr;
}

static gboolean
fu_huddly_usb_device_detach(FuDevice *device, FuProgress *progress, GError **error)
{
	FuHuddlyUsbDevice *self = FU_HUDDLY_USB_DEVICE(device);
	g_print("USB device detach\n");
	/* sanity check */
	if (fu_device_has_flag(device, FWUPD_DEVICE_FLAG_IS_BOOTLOADER)) {
		g_debug("already in bootloader mode, skipping");
		return TRUE;
	}

	/* TODO: switch the device into bootloader mode */
	g_assert(self != NULL);

	fu_device_add_flag(device, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	return TRUE;
}

static gboolean
fu_huddly_usb_device_attach(FuDevice *device, FuProgress *progress, GError **error)
{
	FuHuddlyUsbDevice *self = FU_HUDDLY_USB_DEVICE(device);
	g_print("USB device attach\n");
	/* sanity check */
	if (!fu_device_has_flag(device, FWUPD_DEVICE_FLAG_IS_BOOTLOADER)) {
		g_debug("already in runtime mode, skipping");
		return TRUE;
	}

	/* TODO: switch the device into runtime mode */
	g_assert(self != NULL);

	fu_device_add_flag(device, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	return TRUE;
}

static gboolean
fu_huddly_usb_device_reload(FuDevice *device, GError **error)
{
	FuHuddlyUsbDevice *self = FU_HUDDLY_USB_DEVICE(device);
	/* TODO: reprobe the hardware, or delete this vfunc to use ->setup() as a fallback */
	g_assert(self != NULL);
	return TRUE;
}

static gboolean
fu_huddly_usb_device_probe(FuDevice *device, GError **error)
{
	FuHuddlyUsbDevice *self = FU_HUDDLY_USB_DEVICE(device);

	/* FuUsbDevice->probe */
	if (!FU_DEVICE_CLASS(fu_huddly_usb_device_parent_class)->probe(device, error))
		return FALSE;

	/* TODO: probe the device for properties available before it is opened */
	// if (fu_device_has_private_flag(device, FU_HUDDLY_USB_DEVICE_FLAG_EXAMPLE))
	// 	self->start_addr = 0x100;
	/* success */
	return fu_huddly_usb_device_find_interface(device, error);
	// return TRUE;
}

static gboolean
fu_huddly_usb_device_setup(FuDevice *device, GError **error)
{
	FuHuddlyUsbDevice *self = FU_HUDDLY_USB_DEVICE(device);
	g_autoptr(GString) version_string = NULL;

	/* UsbDevice->setup */
	if (!FU_DEVICE_CLASS(fu_huddly_usb_device_parent_class)->setup(device, error))
		return FALSE;

	/* TODO: get the version and other properties from the hardware while open */
	g_assert(self != NULL);

	if(!fu_huddly_usb_interface_detach_media_kernel_drivers(device, error))
	{
		g_print("Failed to detach media kernel drivers\n");
		
		return FALSE;
	}

	fu_huddly_usb_device_send_reset(device, error);
	fu_huddly_usb_device_send_reset(device, error);
	fu_huddly_usb_device_salute(device, error);

	version_string = fu_huddly_usb_device_get_version(device, error);

	if(version_string != NULL){
		fu_huddly_usb_device_trim_string_at(version_string, '-');
		g_print("Got version %s\n", version_string->str);
		fu_device_set_version(device, version_string->str);
	}
	else{
		g_error("Failed to read device version!\n");
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_huddly_usb_device_prepare(FuDevice *device,
				 FuProgress *progress,
				 FwupdInstallFlags flags,
				 GError **error)
{
	FuHuddlyUsbDevice *self = FU_HUDDLY_USB_DEVICE(device);
	
	g_assert(self != NULL);
	return TRUE;
}

static gboolean
fu_huddly_usb_device_cleanup(FuDevice *device,
				 FuProgress *progress,
				 FwupdInstallFlags flags,
				 GError **error)
{
	FuHuddlyUsbDevice *self = FU_HUDDLY_USB_DEVICE(device);
	/* TODO: anything the device has to do when the update has completed */
	g_print("CLEANUP\n");
	// Reattach media kernel drivers

	g_assert(self != NULL);
	if(!fu_huddly_usb_interface_reattach_media_kernel_drivers(device, error)){
		return FALSE;
	}
	return TRUE;
}

static FuFirmware *
fu_huddly_usb_device_prepare_firmware(FuDevice *device,
					  GInputStream *stream,
					  FuProgress *progress,
					  FwupdInstallFlags flags,
					  GError **error)
{
	FuHuddlyUsbDevice *self = FU_HUDDLY_USB_DEVICE(device);
	g_autoptr(FuFirmware) firmware = fu_huddly_usb_firmware_new();

	/* TODO: you do not need to use this vfunc if not checking attributes */
	if (self->start_addr !=
	    fu_huddly_usb_firmware_get_start_addr(FU_HUDDLY_USB_FIRMWARE(firmware))) {
		g_set_error(
		    error,
		    FWUPD_ERROR,
		    FWUPD_ERROR_INVALID_FILE,
		    "start address mismatch, got 0x%04x, expected 0x%04x",
		    fu_huddly_usb_firmware_get_start_addr(FU_HUDDLY_USB_FIRMWARE(firmware)),
		    self->start_addr);
		return NULL;
	}

	if (!fu_firmware_parse_stream(firmware, stream, 0x0, flags, error))
		return NULL;
	return g_steal_pointer(&firmware);
}

static gboolean
fu_huddly_usb_device_write_blocks(FuHuddlyUsbDevice *self,
				      FuChunkArray *chunks,
				      FuProgress *progress,
				      GError **error)
{
	/* progress */
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_set_steps(progress, fu_chunk_array_length(chunks));
	for (guint i = 0; i < fu_chunk_array_length(chunks); i++) {
		g_autoptr(FuChunk) chk = NULL;

		/* prepare chunk */
		chk = fu_chunk_array_index(chunks, i, error);
		if (chk == NULL)
			return FALSE;

		/* TODO: send to hardware */

		/* update progress */
		fu_progress_step_done(progress);
	}

	/* success */
	return TRUE;
}

static gboolean
fu_huddly_usb_device_write_firmware(FuDevice *device,
					FuFirmware *firmware,
					FuProgress *progress,
					FwupdInstallFlags flags,
					GError **error)
{
	FuHuddlyUsbDevice *self = FU_HUDDLY_USB_DEVICE(device);
	g_autoptr(GInputStream) stream = NULL;
	g_autoptr(FuChunkArray) chunks = NULL;

	/* progress */
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_add_flag(progress, FU_PROGRESS_FLAG_GUESSED);
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 44, NULL);
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_VERIFY, 35, NULL);

	/* get default image */
	stream = fu_firmware_get_stream(firmware, error);
	if (stream == NULL)
		return FALSE;

	/* write each block */
	chunks = fu_chunk_array_new_from_stream(stream, self->start_addr, 64 /* block_size */, error);
	if (chunks == NULL)
		return FALSE;
	if (!fu_huddly_usb_device_write_blocks(self,
						   chunks,
						   fu_progress_get_child(progress),
						   error))
		return FALSE;
	fu_progress_step_done(progress);

	/* TODO: verify each block */
	fu_progress_step_done(progress);

	/* success! */
	return TRUE;
}

static gboolean
fu_huddly_usb_device_set_quirk_kv(FuDevice *device,
				      const gchar *key,
				      const gchar *value,
				      GError **error)
{
	FuHuddlyUsbDevice *self = FU_HUDDLY_USB_DEVICE(device);

	/* TODO: parse value from quirk file */
	if (g_strcmp0(key, "HuddlyUsbStartAddr") == 0) {
		guint64 tmp = 0;
		if (!fu_strtoull(value, &tmp, 0, G_MAXUINT16, FU_INTEGER_BASE_AUTO, error))
			return FALSE;
		self->start_addr = tmp;
		return TRUE;
	}

	/* failed */
	g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED, "quirk key not supported");
	return FALSE;
}

static void
fu_huddly_usb_device_set_progress(FuDevice *self, FuProgress *progress)
{
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_add_flag(progress, FU_PROGRESS_FLAG_GUESSED);
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 0, "detach");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 57, "write");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 0, "attach");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 43, "reload");
}

static void
fu_huddly_usb_device_init(FuHuddlyUsbDevice *self)
{
	self->start_addr = 0x5000;
	self->initialized = FALSE;
	self->interfaces_claimed = FALSE;
	fu_device_set_version_format(FU_DEVICE(self), FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_remove_delay(FU_DEVICE(self), FU_DEVICE_REMOVE_DELAY_RE_ENUMERATE);
	fu_device_add_protocol(FU_DEVICE(self), "com.huddly.usb");
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_SIGNED_PAYLOAD);
	//fu_device_add_internal_flag(FU_DEVICE(self), FU_DEVICE_INTERNAL_FLAG_ONLY_WAIT_FOR_REPLUG);
	fu_device_add_icon(FU_DEVICE(self), "camera-web");
	// fu_device_register_private_flag(FU_DEVICE(self),
	// 				FU_HUDDLY_USB_DEVICE_FLAG_EXAMPLE,
	// 				"example");
	fu_usb_device_add_interface(FU_USB_DEVICE(self), 0x01);
}

static void
fu_huddly_usb_device_finalize(GObject *object)
{
	FuHuddlyUsbDevice *self = FU_HUDDLY_USB_DEVICE(object);
	g_printf("Finalize\n");

	/* TODO: free any allocated instance state here */
	g_assert(self != NULL);

	G_OBJECT_CLASS(fu_huddly_usb_device_parent_class)->finalize(object);
}

static void
fu_huddly_usb_device_class_init(FuHuddlyUsbDeviceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	FuDeviceClass *device_class = FU_DEVICE_CLASS(klass);
	object_class->finalize = fu_huddly_usb_device_finalize;
	device_class->to_string = fu_huddly_usb_device_to_string;
	device_class->probe = fu_huddly_usb_device_probe;
	device_class->setup = fu_huddly_usb_device_setup;
	device_class->reload = fu_huddly_usb_device_reload;
	device_class->prepare = fu_huddly_usb_device_prepare;
	device_class->cleanup = fu_huddly_usb_device_cleanup;
	device_class->attach = fu_huddly_usb_device_attach;
	device_class->detach = fu_huddly_usb_device_detach;
	device_class->prepare_firmware = fu_huddly_usb_device_prepare_firmware;
	device_class->write_firmware = fu_huddly_usb_device_write_firmware;
	device_class->set_quirk_kv = fu_huddly_usb_device_set_quirk_kv;
	device_class->set_progress = fu_huddly_usb_device_set_progress;
}

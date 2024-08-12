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
};

G_DEFINE_TYPE(FuHuddlyUsbDevice, fu_huddly_usb_device, FU_TYPE_USB_DEVICE)


static gboolean fu_huddly_usb_device_find_interface(FuDevice *device, GError **error)
{
	FuHuddlyUsbDevice *self = FU_HUDDLY_USB_DEVICE(device);
	g_autoptr(GPtrArray) intfs = NULL;
	intfs = fu_usb_device_get_interfaces(FU_USB_DEVICE(device), error);

	if(intfs != NULL)
	{
		g_print("Number of interfaces %u\n", intfs->len);
		for(guint i = 0; i < intfs->len; i++){
			FuUsbInterface *intf = g_ptr_array_index(intfs, i);
			if(fu_usb_interface_get_class(intf) == FU_USB_CLASS_VENDOR_SPECIFIC)
			{
				g_autoptr(GPtrArray) endpoints = fu_usb_interface_get_endpoints(intf);
				g_print("USB endpoints %u ...\n", endpoints->len);
			
				for(guint j = 0; j < endpoints->len; j++)
				{
					FuUsbEndpoint* ep = g_ptr_array_index(endpoints, j);
					if(fu_usb_endpoint_get_direction(ep) == FU_USB_DIRECTION_HOST_TO_DEVICE)
					{
						self->bulk_ep[EP_OUT] = fu_usb_endpoint_get_address(ep);
						g_print("Add output endpoint %x\n", self->bulk_ep[EP_OUT]);
					}
					else
					{
						self->bulk_ep[EP_IN] = fu_usb_endpoint_get_address(ep);
						g_print("Add input endpoint %x\n", self->bulk_ep[EP_IN] );
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

static void fu_huddly_usb_hlink_buffer_dispose(HLinkBuffer* buffer)
{
	if(buffer->msg_name != NULL){
    		g_free(buffer->msg_name);
    		buffer->msg_name = NULL;
	}
	if(buffer->payload != NULL){
		g_free(buffer->payload);
		buffer->payload = NULL;
	}
}

static void fu_huddly_usb_hlink_buffer_create(HLinkBuffer* buffer, const gchar* msg_name, guint8* payload, guint32 payload_size)
{
	buffer->header.msg_name_size = strlen(msg_name);
	buffer->msg_name =(gchar*)g_malloc(buffer->header.msg_name_size);
	// change to fu_memcpy_safe
	memcpy(buffer->msg_name, msg_name, buffer->header.msg_name_size);
    
    	buffer->header.payload_size = payload_size;
    	buffer->payload = (guint8*)g_malloc(buffer->header.payload_size);
    	memcpy(buffer->payload, payload, payload_size);
}


static void fu_huddly_usb_hlink_buffer_from_str(HLinkBuffer* buffer, const gchar* msg_name, const gchar* body)
{
    guint32 payload_length = strlen(body);
    fu_huddly_usb_hlink_buffer_create(buffer, msg_name, (guint8*)body, payload_length);
}

static gsize fu_huddly_usb_hlink_packet_size(HLinkBuffer *buffer)
{
    return sizeof(HLinkHeader) + buffer->header.msg_name_size + buffer->header.payload_size;
}

static void fu_huddly_usb_hlink_buffer_to_packet(GByteArray *packet, HLinkBuffer *buffer)
{
    guint32 offset = 0; 
    gsize pkt_sz = fu_huddly_usb_hlink_packet_size(buffer);
    fu_byte_array_set_size(packet, pkt_sz, 0u);
    
    memcpy(packet->data, &buffer->header, sizeof(HLinkHeader));
    offset += sizeof(HLinkHeader);
    memcpy(&packet->data[offset], &buffer->msg_name, buffer->header.msg_name_size);
    offset += buffer->header.msg_name_size;
    memcpy(&packet->data[offset], buffer->payload, buffer->header.payload_size);
}

static gboolean fu_huddly_usb_packet_to_hlink_buffer(HLinkBuffer *buffer, guint8 *packet, guint32 packet_sz)
{
    guint32 offset = 0;
    if(packet_sz < sizeof(HLinkHeader)){
        g_printerr("Insufficient packet size\n");
        return FALSE;
    }
    memcpy((guint8*)&(buffer->header), packet, sizeof(HLinkHeader));

    if(packet_sz < fu_huddly_usb_hlink_packet_size(buffer))
    {
        return FALSE;
    }
    offset = sizeof(HLinkHeader);

    memcpy(buffer->msg_name, packet + offset, buffer->header.msg_name_size);
    offset += buffer->header.msg_name_size;
    memcpy(buffer->payload, packet + offset, buffer->header.payload_size);
    return TRUE;
}




static gboolean fu_huddly_usb_device_hlink_send(FuDevice* device, HLinkBuffer* buffer, GError **error)
{
	FuHuddlyUsbDevice* self = FU_HUDDLY_USB_DEVICE(device);
	g_autoptr(GByteArray) buf = g_byte_array_new();

	gsize actual_size = 0;
	fu_huddly_usb_hlink_buffer_to_packet(buf, buffer);
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

static gboolean fu_huddly_usb_device_hlink_receive(FuDevice* device, HLinkBuffer *buffer, GError **error)
{
	#define RECEIVE_BUFFER_SIZE 8192
	g_autoptr(GByteArray) buf = g_byte_array_new();
	gsize received_length = 0;
	FuHuddlyUsbDevice* self = FU_HUDDLY_USB_DEVICE(device);
	fu_byte_array_set_size(buf, RECEIVE_BUFFER_SIZE, 0u);
	if(!fu_usb_device_bulk_transfer(FU_USB_DEVICE(device), self->bulk_ep[EP_IN], 
		buf->data,
		buf->len, 
		&received_length,
		2000, 
		NULL, 
		error
		))
	{ 
		g_error("Failed hlink receive\n");
		g_prefix_error("Error: ");
		return FALSE;
	}
	else
	{
		if(!fu_huddly_usb_packet_to_hlink_buffer(buffer, buf->data, received_length)){
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
	HLinkBuffer send_buffer;
	g_print("Subscribe %s\n", subscription);
	fu_huddly_usb_hlink_buffer_from_str(&send_buffer, "hlink-mb-subscribe", subscription);
	result = fu_huddly_usb_device_hlink_send(device, &send_buffer, error);
	if(result){
		g_print("OK\n");
	}
	else{
		g_print("FAILED\n");
	}
	fu_huddly_usb_hlink_buffer_dispose(&send_buffer);
	return result;
}

static gboolean fu_huddly_usb_device_hlink_unsubscribe(FuDevice* device, const gchar* subscription, GError** error)
{
	gboolean result = FALSE;
	HLinkBuffer send_buffer;
	g_print("Unsubscribe %s\n", subscription);
	fu_huddly_usb_hlink_buffer_from_str(&send_buffer, "hlink-mb-unsubscribe", subscription);
	result = fu_huddly_usb_device_hlink_send(device, &send_buffer, error);
	if(result){
		g_print("OK\n");
	}
	else{
		g_print("FAILED\n");
	}
	fu_huddly_usb_hlink_buffer_dispose(&send_buffer);
	return result;
}

static void fu_huddly_usb_device_get_version(FuDevice* device, GError **error){
	HLinkBuffer send_buf, receive_buf;
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
		fu_huddly_usb_hlink_buffer_create(&send_buf, "prodinfo/get_msgpack", NULL, 0);
		res = fu_huddly_usb_device_hlink_send(device, &send_buf, error);
		if(!res){
			g_prefix_error(error, "Send failed with error: ");
		}else{
			g_print("Send OK\n");
		}
	}
	if(res)
	{
		res = fu_huddly_usb_device_hlink_receive(device, &receive_buf, error);
		if(!res){
			g_prefix_error(error, "Receive failed with error: ");
		}

		g_print("Receive data %s\n", receive_buf.msg_name);
	}
	if(subscribed){
		res = fu_huddly_usb_device_hlink_unsubscribe(device, "prodinfo/get_msgpack_reply", error);
		if(!res){
			g_prefix_error(error, "Unsubscribe failed with error:");
		}else{
			g_print("UNSUBSCribe OK\n");
		}
	}
	g_print("Dispose send buf\n");
	fu_huddly_usb_hlink_buffer_dispose(&send_buf);
	g_print("Dispose read buf\n");
	fu_huddly_usb_hlink_buffer_dispose(&receive_buf);


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

	/* UsbDevice->setup */
	if (!FU_DEVICE_CLASS(fu_huddly_usb_device_parent_class)->setup(device, error))
		return FALSE;

	/* TODO: get the version and other properties from the hardware while open */
	g_assert(self != NULL);

	fu_huddly_usb_device_get_version(device, error);


	fu_device_set_version(device, "1.2.3");

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
	/* TODO: anything the device has to do before the update starts */
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
	g_assert(self != NULL);
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

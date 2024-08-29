/*
 * Copyright 2024 Huddly
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "fu-huddly-usb-common.h"
#include "fu-huddly-usb-device.h"
#include "fu-huddly-usb-struct.h"

enum { EP_OUT, EP_IN, EP_LAST };


struct _FuHuddlyUsbDevice {
	FuUsbDevice parent_instance;
	guint bulk_ep[EP_LAST];
	gboolean interfaces_claimed;
	gboolean pending_verify;
	GInputStream* input_stream;
};

G_DEFINE_TYPE(FuHuddlyUsbDevice, fu_huddly_usb_device, FU_TYPE_USB_DEVICE)

static gboolean
fu_huddly_usb_device_find_interface(FuDevice *device, GError **error)
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
		g_set_error(error, FWUPD_ERROR, FWUPD_ERROR_INTERNAL, "Could not find interface");
		return FALSE;
	}
}

/**
 * Detach and claim video and audio interfaces before upgrading
 */
static gboolean
fu_huddly_usb_interface_detach_media_kernel_drivers(FuDevice *device, GError **error)
{
	FuHuddlyUsbDevice *self = FU_HUDDLY_USB_DEVICE(device);
	g_autoptr(GPtrArray) intfs = NULL;

	if(self->interfaces_claimed)
	{
		//Interfaces already claimed. Nothing to do
		return TRUE;
	}
	g_debug("Detach media drivers\n");
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
					g_prefix_error(error,
						       "Failed to claim USB media interface: ");
					return FALSE;
				}
				self->interfaces_claimed = TRUE;
			}
		}
	}else
	{
		return FALSE;
	}
	return TRUE;
}

/* Reattach media kernel drivers after update */
static gboolean
fu_huddly_usb_interface_reattach_media_kernel_drivers(FuDevice *device, GError **error)
{
	FuHuddlyUsbDevice *self = FU_HUDDLY_USB_DEVICE(device);
	g_autoptr(GPtrArray) intfs = NULL;

	if(!self->interfaces_claimed)
	{
		//Interfaces have not been claimed. There is nothing to release
		return TRUE;
	}
	g_debug("Reattach media drivers\n");
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
					g_clear_error(error);
				}
			}
		}
	}else
	{
		return FALSE;
	}
	self->interfaces_claimed = FALSE;
	return TRUE;
}

static HLinkBuffer *
fu_huddly_usb_hlink_buffer_from_str(const gchar *msg_name, const gchar *body)
{
    guint32 payload_length = strlen(body);
    return fu_huddly_usb_hlink_buffer_create(msg_name, g_byte_array_new_take((guint8*)body, payload_length));
}

static gboolean
fu_huddly_usb_device_bulk_write(FuDevice *device,
				FuProgress *progress,
				GByteArray *src,
				GError **error)
{
	gsize total_transmitted = 0;
	const gsize max_chunk_size = 16 * 1024;
	gsize chunks = src->len / max_chunk_size;
	gsize remaining = src->len;

	FuHuddlyUsbDevice* self = FU_HUDDLY_USB_DEVICE(device);
	if((src->len % max_chunk_size) == 0){
		chunks++;
	}
	if(progress != NULL){
		fu_progress_set_id(progress, G_STRLOC);
		fu_progress_set_steps(progress, chunks);
	}
	do {
		gsize transmitted = 0;

		gsize chunk_size = (remaining > max_chunk_size) ? max_chunk_size : remaining;
		if (!fu_usb_device_bulk_transfer(FU_USB_DEVICE(device),
						 self->bulk_ep[EP_OUT],
						 src->data + total_transmitted,
						 chunk_size,
						 &transmitted,
						 2000,
						 NULL,
						 error)) {
			return FALSE;
		}
		total_transmitted += transmitted;
		remaining -= transmitted;
		if(progress != NULL){
			fu_progress_step_done(progress);
		}
	} while (remaining > 0);
	return TRUE;
}

static gboolean
fu_huddly_usb_device_bulk_read(FuDevice *device,
			       GByteArray *buf,
			       gsize *received_length,
			       GError **error)
{
	FuHuddlyUsbDevice* self = FU_HUDDLY_USB_DEVICE(device);
	return fu_usb_device_bulk_transfer(FU_USB_DEVICE(device),
					   self->bulk_ep[EP_IN],
					   buf->data,
					   buf->len,
					   received_length,
					   20000,
					   NULL,
					   error);
}

static gboolean
fu_huddly_usb_device_hlink_send(FuDevice *device, HLinkBuffer *buffer, GError **error)
{
	g_autoptr(GByteArray) buf = g_byte_array_new();

	if(!fu_huddly_usb_hlink_buffer_to_packet(buf, buffer, error)){
		return FALSE;
	}
	return fu_huddly_usb_device_bulk_write(device, NULL, buf, error);
}

static gboolean
fu_huddly_usb_device_hlink_receive(FuDevice *device, HLinkBuffer *hlink_buffer, GError **error)
{
	#define RECEIVE_BUFFER_SIZE 1024
	g_autoptr(GByteArray) receive_buffer = g_byte_array_new();
	gsize received_length = 0;

	fu_byte_array_set_size(receive_buffer, RECEIVE_BUFFER_SIZE, 0u);
	if (!fu_huddly_usb_device_bulk_read(device, receive_buffer, &received_length, error)) {
		g_prefix_error(error, "HLink receive failed: ");
		return FALSE;
	} else {
		if(!fu_huddly_usb_packet_to_hlink_buffer(hlink_buffer, receive_buffer->data, received_length, error)){
			g_prefix_error(error, "HLink receive failed: ");
			return FALSE;
		}
		return TRUE;
	}
}

static gboolean
fu_huddly_usb_device_hlink_subscribe(FuDevice *device, const gchar *subscription, GError **error)
{
	g_autoptr(HLinkBuffer) send_buffer = fu_huddly_usb_hlink_buffer_from_str("hlink-mb-subscribe", subscription);;
	g_debug("Subscribe %s\n", subscription);

	if(!fu_huddly_usb_device_hlink_send(device, send_buffer, error)){
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_huddly_usb_device_hlink_unsubscribe(FuDevice *device, const gchar *subscription, GError **error)
{
	g_autoptr(HLinkBuffer) send_buffer = fu_huddly_usb_hlink_buffer_from_str("hlink-mb-unsubscribe", subscription);
	g_debug("Unsubscribe %s\n", subscription);

	if(!fu_huddly_usb_device_hlink_send(device, send_buffer, error)){
		return FALSE;
	}
	return TRUE;
}

/** Send an empty packet to reset hlink communications */
static gboolean
fu_huddly_usb_device_send_reset(FuDevice *device, GError **error)
{
	g_autoptr(GByteArray) packet = g_byte_array_new(); //Empty packet
	if (!fu_huddly_usb_device_bulk_write(device, NULL, packet, error)) {
		g_prefix_error(error, "Reset device failed: ");
		return FALSE;
	}
	return TRUE;
}

/** Send a hlink salute and receive a response from the device */
static gboolean
fu_huddly_usb_device_salute(FuDevice *device, GError **error)
{
	g_autoptr(GByteArray) salutation = g_byte_array_new();
	g_autoptr(GByteArray) response = g_byte_array_new();
	guint8 data = 0x00;
	gsize received_length = 0;
	g_debug("Send salute ...\n");
	g_byte_array_append(salutation, &data, 1);

	if (!fu_huddly_usb_device_bulk_write(device, NULL, salutation, error)) {
		g_prefix_error(error, "send salute send message failed: ");
		return FALSE;
	}

	g_byte_array_set_size(response, 100);

	if (!fu_huddly_usb_device_bulk_read(device, response, &received_length, error)) {
		g_prefix_error(error, "send salute read response failed: ");
		return FALSE;
	}
	response->data[received_length] = '\0';
	g_debug("Received response %s\n", (gchar*)response->data);
	return TRUE;
}

/**
 * Trim string at first occurrence of c
 */
static void
fu_huddly_usb_device_trim_string_at(GString *str, gchar c)
{
	for(gsize i=0; i<str->len; i++)
	{
		if(*(str->str + i) == c){
			g_string_truncate(str, i);
			return;
		}
	}
}

typedef struct {
	GString* version;
	GString* state;
} HuddlyProductInfo;

static void
huddly_product_info_free(HuddlyProductInfo *info)
{
	if(info->version != NULL){
		g_string_free(info->version, TRUE);
	}
	if(info->state != NULL){
		g_string_free(info->state, TRUE);
	}
	g_free(info);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(HuddlyProductInfo, huddly_product_info_free)

/** Search msgpack map for a key
 *
 * Values read from the huddly cameras are sent in msgpack maps as key-value pairs
 * This searches the map for a key and returns a pointer to the following item
 *
 * returns NULL on failure
 *
 */
static FuMsgpackItem *
fu_huddly_usb_device_search_msgpack_map(GPtrArray *items, const gchar *key)
{
	guint64 map_size = 0;
	map_size = fu_msgpack_item_get_map(g_ptr_array_index(items, 0));
	if((map_size == 0) || (map_size == G_MAXINT64)){

		return NULL;
	}
	for(guint64 i=1;i<((map_size * 2) + 1); i+=2){
		FuMsgpackItem *key_item = g_ptr_array_index(items, i);
		if(fu_msgpack_item_get_kind(key_item) != FU_MSGPACK_ITEM_KIND_STRING){
			return NULL;
		}
		if( g_str_equal(fu_msgpack_item_get_string(key_item)->str, key)){
			return g_ptr_array_index(items, i+1);
		}
	}
	return NULL;
}

static gboolean
fu_huddly_usb_device_get_product_info(HuddlyProductInfo *info, FuDevice *device, GError **error)
{
	g_autoptr(HLinkBuffer) send_buf = NULL, receive_buf = NULL;
	g_autoptr(GPtrArray) items = NULL;
	FuMsgpackItem *item = NULL;

	if(!fu_huddly_usb_device_hlink_subscribe(device, "prodinfo/get_msgpack_reply", error))
	{
		g_prefix_error(error, "Failed to read product info: ");
		return FALSE;
	}
	send_buf = fu_huddly_usb_hlink_buffer_create("prodinfo/get_msgpack", NULL);
	if(!fu_huddly_usb_device_hlink_send(device, send_buf, error))
	{
		g_prefix_error(error, "Failed to read product info: ");
		return FALSE;
	}
	receive_buf = g_new0(HLinkBuffer, 1);
	if(!fu_huddly_usb_device_hlink_receive(device, receive_buf, error))
	{
		g_prefix_error(error, "Failed to read product info: ");
		return FALSE;
	}
	g_debug("Receive data %s\n", receive_buf->msg_name);

	items = fu_msgpack_parse(receive_buf->payload, error);

	item = fu_huddly_usb_device_search_msgpack_map(items, "app_version");
	if(item == NULL){
		g_prefix_error(error, "Failed to read product info: ");
		return FALSE;
	}
	info->version = g_string_new(fu_msgpack_item_get_string(item)->str);
	item = fu_huddly_usb_device_search_msgpack_map(items, "state");
	if(item == NULL){
		g_prefix_error(error, "Failed to read product info: ");
		return FALSE;
	}
	info->state = g_string_new(fu_msgpack_item_get_string(item)->str);
	return TRUE;
}

static gboolean
fu_huddly_usb_device_reboot(FuDevice *device, GError **error)
{
	g_autoptr(HLinkBuffer) hlink_buffer = fu_huddly_usb_hlink_buffer_create("camctrl/reboot", NULL);
	g_debug("REBOOT!\n");
	if(!fu_huddly_usb_device_hlink_send(device, hlink_buffer, error)){
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_huddly_usb_device_hcp_write_file(FuDevice *device,
				    FuProgress *progress,
				    const gchar *filename,
				    GInputStream *payload,
				    GError **error)
{
	gsize stream_size = 0;
	gsize cursor = 0;
	g_autoptr(GByteArray) file_bytes = NULL;
	g_autoptr(GByteArray) packed_buffer = NULL;
	HLinkHeader header = {0};
	g_autoptr(GByteArray) send_buffer = g_byte_array_new();
	g_autoptr(HLinkBuffer) receive_buffer = g_new0(HLinkBuffer, 1);
	g_autoptr(GPtrArray) rcv_items = NULL;
	g_autoptr(GPtrArray) msgpack_items = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
	FuMsgpackItem *item = NULL;
	guint8 status_code;
	const char cmd[] = "hcp/write";

	g_debug("Write file\n");

	g_ptr_array_add(msgpack_items, fu_msgpack_item_new_map(2));
	g_ptr_array_add(msgpack_items, fu_msgpack_item_new_string("name"));
	g_ptr_array_add(msgpack_items, fu_msgpack_item_new_string(filename));
	g_ptr_array_add(msgpack_items, fu_msgpack_item_new_string("file_data"));

	if(!fu_input_stream_size(payload, &stream_size, error)){
		return FALSE;
	}
	file_bytes = fu_input_stream_read_byte_array(payload, 0, stream_size, error);
	if(file_bytes == NULL){
		return FALSE;
	}

	g_ptr_array_add(msgpack_items, fu_msgpack_item_new_binary(file_bytes));

	packed_buffer = fu_msgpack_write(msgpack_items, error);

	header.msg_name_size = strlen(cmd);
	header.payload_size = packed_buffer->len;

	g_byte_array_set_size(send_buffer, sizeof(HLinkHeader) + header.msg_name_size + header.payload_size);
	cursor = 0;
	if(!fu_memcpy_safe(send_buffer->data, send_buffer->len, cursor, (guint8*)(&header), sizeof(HLinkHeader), 0, sizeof(HLinkHeader), error)){
		return FALSE;
	}
	cursor += sizeof(HLinkHeader);
	if(!fu_memcpy_safe(send_buffer->data, send_buffer->len, cursor, (guint8*)&cmd[0], header.msg_name_size, 0, header.msg_name_size, error)){
		return FALSE;
	}
	cursor += header.msg_name_size;
	if(!fu_memcpy_safe(send_buffer->data, send_buffer->len, cursor, packed_buffer->data, packed_buffer->len, 0, packed_buffer->len, error)){
		return FALSE;
	}

	g_debug("stream size %lu\n", stream_size);

	if(!fu_huddly_usb_device_hlink_subscribe(device, "hcp/write_reply", error)){
		return FALSE;
	}

	if(!fu_huddly_usb_device_bulk_write(device, progress, send_buffer, error)){
		return FALSE;
	}
	// Read reply and check status
	if(!fu_huddly_usb_device_hlink_receive(device, receive_buffer, error)){
		return FALSE;
	}

	rcv_items = fu_msgpack_parse(receive_buffer->payload, error);
	if(rcv_items == NULL){
		return FALSE;
	}
	item = fu_huddly_usb_device_search_msgpack_map(rcv_items, "status");
	if(item == NULL){
		return FALSE;
	}
	status_code = fu_msgpack_item_get_integer(item);


	if(status_code != 0){
		GString* error_msg = NULL;
		item = fu_huddly_usb_device_search_msgpack_map(rcv_items, "string");
		error_msg = fu_msgpack_item_get_string(item);
		if(error_msg != NULL){
			g_set_error(error, FWUPD_ERROR, FWUPD_ERROR_INTERNAL, "Failed to write file to target code: %d: %s", status_code, error_msg->str);
		}
		return FALSE;
	}


	if(!fu_huddly_usb_device_hlink_unsubscribe(device, "hcp/write_reply", error)){
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_huddly_usb_device_hpk_run(FuDevice *device,
			     const gchar *filename,
			     gboolean *need_reboot,
			     GError **error)
{
	g_autoptr(GByteArray) pack_buffer = NULL;
	g_autoptr(HLinkBuffer) hlink_buffer = NULL;
	g_autoptr(GPtrArray) items = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
	g_ptr_array_add(items, fu_msgpack_item_new_map(1));
	g_ptr_array_add(items, fu_msgpack_item_new_string("filename"));
	g_ptr_array_add(items, fu_msgpack_item_new_string(filename));

	g_debug("Run hpk\n");
	if(!fu_huddly_usb_device_hlink_subscribe(device, "upgrader/status", error)){
		return FALSE;
	}
	pack_buffer = fu_msgpack_write(items, error);
	if(pack_buffer == NULL){
		return FALSE;
	}
	hlink_buffer = fu_huddly_usb_hlink_buffer_create("hpk/run", pack_buffer);
	if(hlink_buffer == NULL){
		return FALSE;
	}
	if(!fu_huddly_usb_device_hlink_send(device, hlink_buffer, error)){
		return FALSE;
	}

	for(;;){
		g_autoptr(HLinkBuffer) receive_buffer = g_new0(HLinkBuffer, 1);
		GString* operation = NULL;
		FuMsgpackItem *item = NULL;
		guint8 err;
		if(!fu_huddly_usb_device_hlink_receive(device, receive_buffer, error)){
			return FALSE;
		}

		items = fu_msgpack_parse(receive_buffer->payload, error);
		if(items == NULL){
			return FALSE;
		}
		item  = fu_huddly_usb_device_search_msgpack_map(items, "operation");
		if(item == NULL){
			return FALSE;
		}
		operation = fu_msgpack_item_get_string(item);
		g_debug("Operation %s\n", operation->str);
		item  = fu_huddly_usb_device_search_msgpack_map(items, "error");
		if(item == NULL){
			return FALSE;
		}
		err = fu_msgpack_item_get_integer(item);
		if (err != 0) {
			g_prefix_error(error, "Received error %s", operation->str);
			return FALSE;
		}
		item = fu_huddly_usb_device_search_msgpack_map(items, "reboot");
		if(item == NULL){
			return FALSE;
		}
		*need_reboot = fu_msgpack_item_get_boolean(item);
		g_debug("Need reboot %d", *need_reboot);
		if (strncmp(operation->str, "done", operation->len) == 0) {
			break;
		}
	}

	if(!fu_huddly_usb_device_hlink_unsubscribe(device, "upgrader/status", error)){
		return FALSE;
	}
	return TRUE;
}

// static void
// fu_huddly_usb_device_to_string(FuDevice *device, guint idt, GString *str)
// {
// 	FuHuddlyUsbDevice *self = FU_HUDDLY_USB_DEVICE(device);
// 	fwupd_codec_string_append_hex(str, idt, "StartAddr", self->start_addr);
// }


static gboolean
fu_huddly_usb_device_verify(FuDevice *device, FuProgress *progress, GError **error)
{
	gboolean need_reboot = FALSE;
	g_autoptr(HuddlyProductInfo) prod_info = g_new0(HuddlyProductInfo, 1);

	FuHuddlyUsbDevice *self = FU_HUDDLY_USB_DEVICE(device);

	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 80, NULL);
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_VERIFY, 20, NULL);

	if(!fu_huddly_usb_device_hcp_write_file(device, fu_progress_get_child(progress), "firmware.hpk", self->input_stream, error)){
		return FALSE;
	}
	fu_progress_step_done(progress);
	if(!fu_huddly_usb_device_hpk_run(device, "firmware.hpk", &need_reboot, error)){
		return FALSE;
	}
	fu_progress_step_done(progress);
	return TRUE;
}


static gboolean
fu_huddly_usb_device_attach(FuDevice *device, FuProgress *progress, GError **error)
{
	g_autoptr(HuddlyProductInfo) prod_info = g_new0(HuddlyProductInfo, 1);

	if(!fu_huddly_usb_device_get_product_info(prod_info, device, error))
	{
		g_warning("Failed to read product info\n");
		return FALSE;
	}

	g_debug("Device fw version %s\n", prod_info->version->str);
	g_debug("Device state %s\n", prod_info->state->str);

	if(strncmp(prod_info->state->str, "Unverified", prod_info->state->len) == 0){
		if (!fu_huddly_usb_interface_detach_media_kernel_drivers(device, error)) {
			return FALSE;
		}
		if (!fu_huddly_usb_device_verify(device, progress, error)) {
			return FALSE;
		}
	}

	return TRUE;
}

static gboolean
fu_huddly_usb_device_reload(FuDevice *device, GError **error)
{
	g_autoptr(HuddlyProductInfo) prod_info = g_new0(HuddlyProductInfo, 1);
	/* TODO: reprobe the hardware, or delete this vfunc to use ->setup() as a fallback */

	if(!fu_huddly_usb_device_get_product_info(prod_info, device, error))
	{
		g_warning("Failed to read product info\n");
		return FALSE;
	}
	if(strncmp(prod_info->state->str, "Verified", prod_info->state->len) != 0){
		g_set_error(error, FWUPD_ERROR, FWUPD_ERROR_INTERNAL, "Expected device state Verified after update. State %s", prod_info->state->str);
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_huddly_usb_device_probe(FuDevice *device, GError **error)
{
	/* FuUsbDevice->probe */
	if (!FU_DEVICE_CLASS(fu_huddly_usb_device_parent_class)->probe(device, error))
		return FALSE;

	return fu_huddly_usb_device_find_interface(device, error);
}

static gboolean
fu_huddly_usb_device_setup(FuDevice *device, GError **error)
{
	g_autoptr(HuddlyProductInfo) prod_info = g_new0(HuddlyProductInfo, 1);
	/* UsbDevice->setup */
	if (!FU_DEVICE_CLASS(fu_huddly_usb_device_parent_class)->setup(device, error))
		return FALSE;

	/* TODO: get the version and other properties from the hardware while open */
	if(!fu_huddly_usb_device_send_reset(device, error)){
		return FALSE;
	}
	if(!fu_huddly_usb_device_send_reset(device, error)){
		return FALSE;
	}
	if(!fu_huddly_usb_device_salute(device, error)){
		return FALSE;
	}

	if(!fu_huddly_usb_device_get_product_info(prod_info, device, error))
	{
		return FALSE;
	}

	if(prod_info->version != NULL){
		fu_huddly_usb_device_trim_string_at(prod_info->version, '-');
		fu_device_set_version(device, prod_info->version->str);
	}
	else{
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INTERNAL,
			    "Failed to read device version!\n");
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
	if (!fu_huddly_usb_interface_detach_media_kernel_drivers(device, error)) {
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_huddly_usb_device_cleanup(FuDevice *device,
				 FuProgress *progress,
				 FwupdInstallFlags flags,
				 GError **error)
{
	/* TODO: anything the device has to do when the update has completed */
	// Reattach media kernel drivers
	//@todo IGNORE ERRORS
	if(!fu_huddly_usb_interface_reattach_media_kernel_drivers(device, error)){
		return FALSE;
	}
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
	g_autoptr(FuChunkArray) chunks = NULL;
	gboolean need_reboot;

	/* progress */
	fu_progress_set_id(progress, G_STRLOC);
	//fu_progress_add_flag(progress, FU_PROGRESS_FLAG_GUESSED);
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 50, NULL);
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 30, NULL);
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 20, NULL);

	/* get default image */
	self->input_stream = fu_firmware_get_stream(firmware, error);
	if (self->input_stream == NULL)
		return FALSE;

	if(!fu_huddly_usb_device_hcp_write_file(device, fu_progress_get_child(progress), "firmware.hpk", self->input_stream, error)){
		return FALSE;
	}
	fu_progress_step_done(progress);
	if(!fu_huddly_usb_device_hpk_run(device, "firmware.hpk", &need_reboot, error)){
		return FALSE;
	}
	fu_progress_step_done(progress);
	if(!fu_huddly_usb_device_reboot(device, error)){
		return FALSE;
	}
	self->pending_verify = TRUE;
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	fu_progress_step_done(progress);

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
	// if (g_strcmp0(key, "HuddlyUsbStartAddr") == 0) {
	// 	guint64 tmp = 0;
	// 	if (!fu_strtoull(value, &tmp, 0, G_MAXUINT16, FU_INTEGER_BASE_AUTO, error))
	// 		return FALSE;
	// 	self->start_addr = tmp;
	// 	return TRUE;
	// }

	/* failed */
	g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED, "quirk key not supported");
	return FALSE;
}

static void
fu_huddly_usb_device_set_progress(FuDevice *device, FuProgress *progress)
{
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 1, "detach");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 50, "write");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 44, "attach");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 5, "reload");
}

static void
fu_huddly_usb_device_init(FuHuddlyUsbDevice *self)
{
	self->interfaces_claimed = FALSE;
	self->pending_verify = FALSE;
	fu_device_set_version_format(FU_DEVICE(self), FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_remove_delay(FU_DEVICE(self), 60000); // 60 second remove delay
	fu_device_add_protocol(FU_DEVICE(self), "com.huddly.usb");
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_SIGNED_PAYLOAD);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_IGNORE_SYSTEM_POWER);
	fu_device_add_icon(FU_DEVICE(self), "camera-web");
	fu_device_set_battery_threshold(FU_DEVICE(self), 0);
	fu_usb_device_add_interface(FU_USB_DEVICE(self), 0x01);
}

static void
fu_huddly_usb_device_replace(FuDevice *device, FuDevice *donor)
{
	FuHuddlyUsbDevice *self = FU_HUDDLY_USB_DEVICE(device);
	FuHuddlyUsbDevice *self_donor = FU_HUDDLY_USB_DEVICE(donor);
	self->input_stream = self_donor->input_stream;
}

static void
fu_huddly_usb_device_finalize(GObject *object)
{
	/* TODO: free any allocated instance state here */
	G_OBJECT_CLASS(fu_huddly_usb_device_parent_class)->finalize(object);
}

static void
fu_huddly_usb_device_class_init(FuHuddlyUsbDeviceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	FuDeviceClass *device_class = FU_DEVICE_CLASS(klass);
	object_class->finalize = fu_huddly_usb_device_finalize;
	// device_class->to_string = fu_huddly_usb_device_to_string;
	device_class->probe = fu_huddly_usb_device_probe;
	device_class->setup = fu_huddly_usb_device_setup;
	device_class->reload = fu_huddly_usb_device_reload;
	device_class->prepare = fu_huddly_usb_device_prepare;
	device_class->cleanup = fu_huddly_usb_device_cleanup;
	device_class->attach = fu_huddly_usb_device_attach;
	device_class->write_firmware = fu_huddly_usb_device_write_firmware;
	device_class->set_quirk_kv = fu_huddly_usb_device_set_quirk_kv;
	device_class->set_progress = fu_huddly_usb_device_set_progress;
	device_class->replace = fu_huddly_usb_device_replace;
}

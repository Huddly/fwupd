/*
 * Copyright 2021 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "FuBackend"

#include "config.h"

#include <fwupdplugin.h>

#include <gudev/gudev.h>

#include "fu-context-private.h"
#include "fu-device-private.h"
#include "fu-udev-backend.h"
#include "fu-udev-device-private.h"

struct _FuUdevBackend {
	FuBackend parent_instance;
	GUdevClient *gudev_client;
	GHashTable *changed_idle_ids; /* sysfs:FuUdevBackendHelper */
	GPtrArray *dpaux_devices;     /* of FuDpauxDevice */
	guint dpaux_devices_rescan_id;
	gboolean done_coldplug;
};

G_DEFINE_TYPE(FuUdevBackend, fu_udev_backend, FU_TYPE_BACKEND)

#define FU_UDEV_BACKEND_DPAUX_RESCAN_DELAY 5 /* s */

static void
fu_udev_backend_to_string(FuBackend *backend, guint idt, GString *str)
{
	FuUdevBackend *self = FU_UDEV_BACKEND(backend);
	fwupd_codec_string_append_bool(str, idt, "DoneColdplug", self->done_coldplug);
}

static void
fu_udev_backend_rescan_dpaux_device(FuUdevBackend *self, FuDevice *dpaux_device)
{
	FuDevice *device_tmp;
	g_autoptr(FuDeviceLocker) locker = NULL;
	g_autoptr(GError) error_local = NULL;

	/* find the device we enumerated */
	g_debug("looking for %s", fu_device_get_backend_id(dpaux_device));
	device_tmp =
	    fu_backend_lookup_by_id(FU_BACKEND(self), fu_device_get_backend_id(dpaux_device));

	/* open */
	fu_device_probe_invalidate(dpaux_device);
	locker = fu_device_locker_new(dpaux_device, &error_local);
	if (locker == NULL) {
		g_debug("failed to open device %s: %s",
			fu_device_get_backend_id(dpaux_device),
			error_local->message);
		if (device_tmp != NULL)
			fu_backend_device_removed(FU_BACKEND(self), FU_DEVICE(device_tmp));
		return;
	}
	if (device_tmp == NULL) {
		fu_backend_device_added(FU_BACKEND(self), FU_DEVICE(dpaux_device));
		return;
	}
}

static gboolean
fu_udev_backend_rescan_dpaux_devices_cb(gpointer user_data)
{
	FuUdevBackend *self = FU_UDEV_BACKEND(user_data);
	for (guint i = 0; i < self->dpaux_devices->len; i++) {
		FuDevice *dpaux_device = g_ptr_array_index(self->dpaux_devices, i);
		fu_udev_backend_rescan_dpaux_device(self, dpaux_device);
	}
	self->dpaux_devices_rescan_id = 0;
	return FALSE;
}

static void
fu_udev_backend_rescan_dpaux_devices(FuUdevBackend *self)
{
	if (self->dpaux_devices_rescan_id != 0)
		g_source_remove(self->dpaux_devices_rescan_id);
	self->dpaux_devices_rescan_id =
	    g_timeout_add_seconds(FU_UDEV_BACKEND_DPAUX_RESCAN_DELAY,
				  fu_udev_backend_rescan_dpaux_devices_cb,
				  self);
}

static FuUdevDevice *
fu_udev_backend_create_device(FuUdevBackend *self, GUdevDevice *udev_device);

static void
fu_udev_backend_create_ddc_proxy(FuUdevBackend *self, FuDevice *device)
{
	g_autofree gchar *proxy_sysfs_path = NULL;
	g_autoptr(FuUdevDevice) proxy = NULL;
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GUdevDevice) proxy_udev_device = NULL;

	proxy_sysfs_path =
	    g_build_filename(fu_udev_device_get_sysfs_path(FU_UDEV_DEVICE(device)), "ddc", NULL);
	proxy_udev_device = g_udev_client_query_by_sysfs_path(self->gudev_client, proxy_sysfs_path);
	if (proxy_udev_device == NULL)
		return;
	proxy = fu_udev_backend_create_device(self, proxy_udev_device);
	fu_device_add_private_flag(FU_DEVICE(proxy), FU_I2C_DEVICE_PRIVATE_FLAG_NO_HWID_GUIDS);
	if (!fu_device_probe(FU_DEVICE(proxy), &error_local)) {
		g_warning("failed to probe DRM DDC device: %s", error_local->message);
		return;
	}
	fu_device_add_private_flag(device, FU_DEVICE_PRIVATE_FLAG_REFCOUNTED_PROXY);
	fu_device_set_proxy(device, FU_DEVICE(proxy));
}

static FuUdevDevice *
fu_udev_backend_create_device(FuUdevBackend *self, GUdevDevice *udev_device)
{
	GType gtype = FU_TYPE_UDEV_DEVICE;
	struct {
		const gchar *subsystem;
		GType gtype;
	} subsystem_gtype_map[] = {{"mei", FU_TYPE_MEI_DEVICE},
				   {"drm", FU_TYPE_DRM_DEVICE},
				   {"usb", FU_TYPE_USB_DEVICE},
				   {"i2c", FU_TYPE_I2C_DEVICE},
				   {"i2c-dev", FU_TYPE_I2C_DEVICE},
				   {"drm_dp_aux_dev", FU_TYPE_DPAUX_DEVICE},
				   {NULL, G_TYPE_INVALID}};
	g_autoptr(FuDevice) device = NULL;

	/* create the correct object depending on the subsystem */
	for (guint i = 0; subsystem_gtype_map[i].gtype != G_TYPE_INVALID; i++) {
		if (g_strcmp0(g_udev_device_get_subsystem(udev_device),
			      subsystem_gtype_map[i].subsystem) == 0) {
			gtype = subsystem_gtype_map[i].gtype;
			break;
		}
	}

	/* ensure this is the actual device */
	if (gtype == FU_TYPE_USB_DEVICE &&
	    g_strcmp0(g_udev_device_get_devtype(udev_device), "usb_device") != 0)
		return NULL;

	/* create device of correct kind */
	device = g_object_new(gtype, "backend", FU_BACKEND(self), "udev-device", udev_device, NULL);

	/* the DRM device has a i2c device that is used for communicating with the scaler */
	if (gtype == FU_TYPE_DRM_DEVICE)
		fu_udev_backend_create_ddc_proxy(self, device);

	/* success */
	return FU_UDEV_DEVICE(g_steal_pointer(&device));
}

static void
fu_udev_backend_device_add(FuUdevBackend *self, GUdevDevice *udev_device)
{
	FuContext *ctx = fu_backend_get_context(FU_BACKEND(self));
	g_autoptr(FuUdevDevice) device = NULL;
	g_autoptr(GPtrArray) possible_plugins = NULL;

	/* ignore zram and loop block devices -- of which there are dozens on systems with snap */
	if (g_strcmp0(g_udev_device_get_subsystem(udev_device), "block") == 0) {
		g_autofree gchar *basename =
		    g_path_get_basename(g_udev_device_get_sysfs_path(udev_device));
		if (g_str_has_prefix(basename, "zram") || g_str_has_prefix(basename, "loop"))
			return;
	}

	/* use the subsystem to create the correct GType */
	device = fu_udev_backend_create_device(self, udev_device);
	if (device == NULL)
		return;

	/* these are used without a subclass */
	if (g_strcmp0(g_udev_device_get_subsystem(udev_device), "msr") == 0)
		fu_udev_device_add_open_flag(device, FU_IO_CHANNEL_OPEN_FLAG_READ);

	/* notify plugins using fu_plugin_add_udev_subsystem() */
	possible_plugins =
	    fu_context_get_plugin_names_for_udev_subsystem(ctx,
							   g_udev_device_get_subsystem(udev_device),
							   NULL);
	if (possible_plugins != NULL) {
		for (guint i = 0; i < possible_plugins->len; i++) {
			const gchar *plugin_name = g_ptr_array_index(possible_plugins, i);
			fu_device_add_possible_plugin(FU_DEVICE(device), plugin_name);
		}
	}

	/* DP AUX devices are *weird* and can only read the DPCD when a DRM device is attached */
	if (g_strcmp0(g_udev_device_get_subsystem(udev_device), "drm_dp_aux_dev") == 0) {
		/* add and rescan, regardless of if we can open it */
		g_ptr_array_add(self->dpaux_devices, g_object_ref(device));
		fu_udev_backend_rescan_dpaux_devices(self);

		/* open -- this might seem redundant, but it means the device is added at daemon
		 * coldplug rather than a few seconds later */
		if (!self->done_coldplug) {
			g_autoptr(FuDeviceLocker) locker = NULL;
			g_autoptr(GError) error_local = NULL;

			locker = fu_device_locker_new(device, &error_local);
			if (locker == NULL) {
				g_debug("failed to open device %s: %s",
					fu_device_get_backend_id(FU_DEVICE(device)),
					error_local->message);
				return;
			}
			fu_backend_device_added(FU_BACKEND(self), FU_DEVICE(device));
		}
		return;
	}

	/* success */
	fu_backend_device_added(FU_BACKEND(self), FU_DEVICE(device));
}

static void
fu_udev_backend_device_remove(FuUdevBackend *self, GUdevDevice *udev_device)
{
	FuDevice *device_tmp;

	/* find the device we enumerated */
	device_tmp =
	    fu_backend_lookup_by_id(FU_BACKEND(self), g_udev_device_get_sysfs_path(udev_device));
	if (device_tmp != NULL) {
		g_debug("UDEV %s removed", g_udev_device_get_sysfs_path(udev_device));
		fu_backend_device_removed(FU_BACKEND(self), device_tmp);

		/* rescan all the DP AUX devices if it or any DRM device disappears */
		if (g_ptr_array_remove(self->dpaux_devices, device_tmp) ||
		    g_strcmp0(g_udev_device_get_subsystem(udev_device), "drm") == 0) {
			fu_udev_backend_rescan_dpaux_devices(self);
		}
	}
}

typedef struct {
	FuUdevBackend *self;
	FuDevice *device;
	guint idle_id;
} FuUdevBackendHelper;

static void
fu_udev_backend_changed_helper_free(FuUdevBackendHelper *helper)
{
	if (helper->idle_id != 0)
		g_source_remove(helper->idle_id);
	g_object_unref(helper->self);
	g_object_unref(helper->device);
	g_free(helper);
}

static FuUdevBackendHelper *
fu_udev_backend_changed_helper_new(FuUdevBackend *self, FuDevice *device)
{
	FuUdevBackendHelper *helper = g_new0(FuUdevBackendHelper, 1);
	helper->self = g_object_ref(self);
	helper->device = g_object_ref(device);
	return helper;
}

static gboolean
fu_udev_backend_device_changed_cb(gpointer user_data)
{
	FuUdevBackendHelper *helper = (FuUdevBackendHelper *)user_data;
	fu_backend_device_changed(FU_BACKEND(helper->self), helper->device);
	if (g_strcmp0(fu_udev_device_get_subsystem(FU_UDEV_DEVICE(helper->device)), "drm") != 0)
		fu_udev_backend_rescan_dpaux_devices(helper->self);
	helper->idle_id = 0;
	g_hash_table_remove(helper->self->changed_idle_ids,
			    fu_udev_device_get_sysfs_path(FU_UDEV_DEVICE(helper->device)));
	return FALSE;
}

static void
fu_udev_backend_device_changed(FuUdevBackend *self, GUdevDevice *udev_device)
{
	const gchar *sysfs_path = g_udev_device_get_sysfs_path(udev_device);
	FuUdevBackendHelper *helper;
	FuDevice *device_tmp;

	/* not a device we enumerated */
	device_tmp = fu_backend_lookup_by_id(FU_BACKEND(self), sysfs_path);
	if (device_tmp == NULL)
		return;

	/* run all plugins, with per-device rate limiting */
	if (g_hash_table_remove(self->changed_idle_ids, sysfs_path)) {
		g_debug("re-adding rate-limited timeout for %s", sysfs_path);
	} else {
		g_debug("adding rate-limited timeout for %s", sysfs_path);
	}
	helper = fu_udev_backend_changed_helper_new(self, device_tmp);
	helper->idle_id = g_timeout_add(500, fu_udev_backend_device_changed_cb, helper);
	g_hash_table_insert(self->changed_idle_ids, g_strdup(sysfs_path), helper);
}

static void
fu_udev_backend_uevent_cb(GUdevClient *gudev_client,
			  const gchar *action,
			  GUdevDevice *udev_device,
			  FuUdevBackend *self)
{
	if (g_strcmp0(action, "add") == 0) {
		fu_udev_backend_device_add(self, udev_device);
		return;
	}
	if (g_strcmp0(action, "remove") == 0) {
		fu_udev_backend_device_remove(self, udev_device);
		return;
	}
	if (g_strcmp0(action, "change") == 0) {
		fu_udev_backend_device_changed(self, udev_device);
		return;
	}
}

static void
fu_udev_backend_coldplug_subsystem(FuUdevBackend *self,
				   const gchar *subsystem,
				   FuProgress *progress)
{
	g_autolist(GObject) devices = NULL;

	devices = g_udev_client_query_by_subsystem(self->gudev_client, subsystem);
	g_debug("%u devices with subsystem %s", g_list_length(devices), subsystem);

	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_set_name(progress, subsystem);
	fu_progress_set_steps(progress, g_list_length(devices));
	for (GList *l = devices; l != NULL; l = l->next) {
		GUdevDevice *udev_device = l->data;
		fu_progress_set_name(fu_progress_get_child(progress),
				     g_udev_device_get_sysfs_path(udev_device));
		fu_udev_backend_device_add(self, udev_device);
		fu_progress_step_done(progress);
	}
}

static gboolean
fu_udev_backend_coldplug(FuBackend *backend, FuProgress *progress, GError **error)
{
	FuContext *ctx = fu_backend_get_context(backend);
	FuUdevBackend *self = FU_UDEV_BACKEND(backend);
	g_autoptr(GPtrArray) udev_subsystems = fu_context_get_udev_subsystems(ctx);

	/* udev watches can only be set up in _init() so set up client now */
	if (udev_subsystems->len > 0) {
		g_auto(GStrv) subsystems = g_new0(gchar *, udev_subsystems->len + 1);
		for (guint i = 0; i < udev_subsystems->len; i++) {
			const gchar *subsystem = g_ptr_array_index(udev_subsystems, i);
			subsystems[i] = g_strdup(subsystem);
		}
		self->gudev_client = g_udev_client_new((const gchar *const *)subsystems);
		g_signal_connect(G_UDEV_CLIENT(self->gudev_client),
				 "uevent",
				 G_CALLBACK(fu_udev_backend_uevent_cb),
				 self);
	}

	/* get all devices of class */
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_set_steps(progress, udev_subsystems->len);
	for (guint i = 0; i < udev_subsystems->len; i++) {
		const gchar *subsystem = g_ptr_array_index(udev_subsystems, i);
		fu_udev_backend_coldplug_subsystem(self,
						   subsystem,
						   fu_progress_get_child(progress));
		fu_progress_step_done(progress);
	}

	/* success */
	self->done_coldplug = TRUE;
	return TRUE;
}

static FuDevice *
fu_udev_backend_get_device_parent(FuBackend *backend,
				  FuDevice *device,
				  const gchar *subsystem,
				  GError **error)
{
	FuUdevBackend *self = FU_UDEV_BACKEND(backend);
	GUdevDevice *udev_device = fu_udev_device_get_dev(FU_UDEV_DEVICE(device));
	g_autoptr(GUdevDevice) device_tmp = NULL;

	/* sanity check */
	if (udev_device == NULL) {
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND, "not initialized");
		return NULL;
	}
	if (subsystem == NULL) {
		g_autoptr(GUdevDevice) udev_parent = g_udev_device_get_parent(udev_device);
		g_autoptr(FuUdevDevice) parent = NULL;
		if (udev_parent == NULL) {
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_NOT_SUPPORTED,
					    "no udev parent");
			return NULL;
		}
		parent = fu_udev_backend_create_device(self, udev_parent);
		if (parent == NULL) {
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_NOT_SUPPORTED,
					    "no parent");
			return NULL;
		}
		return FU_DEVICE(g_steal_pointer(&parent));
	}
	device_tmp = g_udev_device_get_parent(udev_device);
	while (device_tmp != NULL) {
		g_autoptr(GUdevDevice) udev_parent = NULL;
		g_autoptr(FuUdevDevice) device_new = NULL;

		/* a match! */
		device_new = fu_udev_backend_create_device(self, device_tmp);
		if (device_new == NULL)
			break;
		if (fu_udev_device_match_subsystem(device_new, subsystem))
			return FU_DEVICE(g_steal_pointer(&device_new));

		udev_parent = g_udev_device_get_parent(device_tmp);
		g_set_object(&device_tmp, udev_parent);
	}

	/* failed */
	g_set_error(error,
		    FWUPD_ERROR,
		    FWUPD_ERROR_NOT_SUPPORTED,
		    "no parent with subsystem %s",
		    subsystem);
	return NULL;
}

static void
fu_udev_backend_finalize(GObject *object)
{
	FuUdevBackend *self = FU_UDEV_BACKEND(object);
	if (self->dpaux_devices_rescan_id != 0)
		g_source_remove(self->dpaux_devices_rescan_id);
	if (self->gudev_client != NULL)
		g_object_unref(self->gudev_client);
	g_hash_table_unref(self->changed_idle_ids);
	g_ptr_array_unref(self->dpaux_devices);
	G_OBJECT_CLASS(fu_udev_backend_parent_class)->finalize(object);
}

static void
fu_udev_backend_init(FuUdevBackend *self)
{
	self->dpaux_devices = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
	self->changed_idle_ids =
	    g_hash_table_new_full(g_str_hash,
				  g_str_equal,
				  g_free,
				  (GDestroyNotify)fu_udev_backend_changed_helper_free);
}

static void
fu_udev_backend_class_init(FuUdevBackendClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	FuBackendClass *backend_class = FU_BACKEND_CLASS(klass);
	object_class->finalize = fu_udev_backend_finalize;
	backend_class->coldplug = fu_udev_backend_coldplug;
	backend_class->to_string = fu_udev_backend_to_string;
	backend_class->get_device_parent = fu_udev_backend_get_device_parent;
}

FuBackend *
fu_udev_backend_new(FuContext *ctx)
{
	return FU_BACKEND(g_object_new(FU_TYPE_UDEV_BACKEND,
				       "name",
				       "udev",
				       "context",
				       ctx,
				       "device-gtype",
				       FU_TYPE_UDEV_DEVICE,
				       NULL));
}

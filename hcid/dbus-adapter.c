/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2006  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>

#include <dbus/dbus.h>

#include "hcid.h"
#include "dbus.h"

#include "textfile.h"
#include "oui.h"
#include "list.h"

static const char *service_cls[] = {
	"positioning",
	"networking",
	"rendering",
	"capturing",
	"object transfer",
	"audio",
	"telephony",
	"information"
};

static const char *major_cls[] = {
	"miscellaneous",
	"computer",
	"phone",
	"access point",
	"audio/video",
	"peripheral",
	"imaging",
	"wearable",
	"toy",
	"uncategorized"
};

static const char *computer_minor_cls[] = {
	"uncategorized",
	"desktop",
	"server",
	"laptop",
	"handheld",
	"palm",
	"wearable"
};

static const char *phone_minor_cls[] = {
	"uncategorized",
	"cellular",
	"cordless",
	"smart phone",
	"modem",
	"isdn"
};

static const char *access_point_minor_cls[] = {
	"fully",
	"1-17 percent",
	"17-33 percent",
	"33-50 percent",
	"50-67 percent",
	"67-83 percent",
	"83-99 percent",
	"not available"
};

static const char *audio_video_minor_cls[] = {
	"uncategorized",
	"headset",
	"handsfree",
	"unknown",
	"microphone",
	"loudspeaker",
	"headphones",
	"portable audio",
	"car audio",
	"set-top box",
	"hifi audio",
	"vcr",
	"video camera",
	"camcorder",
	"video monitor",
	"video display and loudspeaker",
	"video conferencing",
	"unknown",
	"gaming/toy"
};

static const char *peripheral_minor_cls[] = {
	"uncategorized",
	"keyboard",
	"pointing",
	"combo"
};

#if 0
static const char *peripheral_2_minor_cls[] = {
	"uncategorized",
	"joystick",
	"gamepad",
	"remote control",
	"sensing",
	"digitizer tablet",
	"card reader"
};
#endif

static const char *imaging_minor_cls[] = {
	"display",
	"camera",
	"scanner",
	"printer"
};

static const char *wearable_minor_cls[] = {
	"wrist watch",
	"pager",
	"jacket",
	"helmet",
	"glasses"
};

static const char *toy_minor_cls[] = {
	"robot",
	"vehicle",
	"doll",
	"controller",
	"game"
};

int pending_remote_name_cancel(struct adapter *adapter)
{
	struct remote_dev_info *dev, match;
	struct slist *l;
	int dd, err = 0;

	/* find the pending remote name request */
	memset(&match, 0, sizeof(struct remote_dev_info));
	bacpy(&match.bdaddr, BDADDR_ANY);
	match.name_status = NAME_REQUESTED;

	l = slist_find(adapter->found_devices, &match,
			(cmp_func_t) found_device_cmp);
	if (!l) /* no pending request */
		return 0;

	dd = hci_open_dev(adapter->dev_id);
	if (dd < 0)
		return -ENODEV;

	dev = l->data;

	if (hci_read_remote_name_cancel(dd, &dev->bdaddr, 1000) < 0) {
		error("Remote name cancel failed: %s(%d)", strerror(errno), errno);
		err = -errno;
	}

	/* free discovered devices list */
	slist_foreach(adapter->found_devices, (slist_func_t) free, NULL);
	slist_free(adapter->found_devices);
	adapter->found_devices = NULL;

	hci_close_dev(dd);
	return err;
}

static struct bonding_request_info *bonding_request_new(bdaddr_t *peer,
							DBusConnection *conn,
							DBusMessage *msg)
{
	struct bonding_request_info *bonding;

	bonding = malloc(sizeof(*bonding));

	if (!bonding)
		return NULL;

	memset(bonding, 0, sizeof(*bonding));

	bacpy(&bonding->bdaddr, peer);

	bonding->conn = dbus_connection_ref(conn);
	bonding->rq = dbus_message_ref(msg);

	return bonding;
}

static DBusHandlerResult adapter_get_address(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct adapter *adapter = data;
	const char *paddr = adapter->address;
	DBusMessage *reply;

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return error_invalid_arguments(conn, msg);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &paddr,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_get_version(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct adapter *adapter = data;
	DBusMessage *reply;
	char str[20], *str_ptr = str;
	int err;

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return error_invalid_arguments(conn, msg);

	err = get_device_version(adapter->dev_id, str, sizeof(str));
	if (err < 0)
		return error_failed(conn, msg, -err);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &str_ptr,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_get_revision(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct adapter *adapter = data;
	DBusMessage *reply;
	char str[64], *str_ptr = str;
	int err;

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return error_invalid_arguments(conn, msg);

	err = get_device_revision(adapter->dev_id, str, sizeof(str));
	if (err < 0)
		return error_failed(conn, msg, -err);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &str_ptr,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_get_manufacturer(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct adapter *adapter = data;
	DBusMessage *reply;
	char str[64], *str_ptr = str;
	int err;

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return error_invalid_arguments(conn, msg);

	err = get_device_manufacturer(adapter->dev_id, str, sizeof(str));
	if (err < 0)
		return error_failed(conn, msg, -err);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &str_ptr,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_get_company(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct adapter *adapter = data;
	DBusMessage *reply;
	char str[64], *str_ptr = str;
	int err;

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return error_invalid_arguments(conn, msg);

	err = get_device_company(adapter->dev_id, str, sizeof(str));
	if (err < 0)
		return error_failed(conn, msg, -err);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &str_ptr,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_get_mode(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	const struct adapter *adapter = data;
	DBusMessage *reply = NULL;
	const uint8_t hci_mode = adapter->mode;
	const char *scan_mode;

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return error_invalid_arguments(conn, msg);

	switch (hci_mode) {
	case SCAN_DISABLED:
		scan_mode = MODE_OFF;
		break;
	case SCAN_PAGE:
		scan_mode = MODE_CONNECTABLE;
		break;
	case (SCAN_PAGE | SCAN_INQUIRY):
		scan_mode = MODE_DISCOVERABLE;
		break;
	case SCAN_INQUIRY:
	/* inquiry scan mode is not handled, return unknown */
	default:
		/* reserved */
		scan_mode = MODE_UNKNOWN;
	}

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &scan_mode,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_set_mode(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	const struct adapter *adapter = data;
	DBusMessage *reply;
	DBusError err;
	const char* scan_mode;
	uint8_t hci_mode;
	const uint8_t current_mode = adapter->mode;
	int dd;

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &scan_mode,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	if (!scan_mode)
		return error_invalid_arguments(conn, msg);

	if (strcasecmp(MODE_OFF, scan_mode) == 0)
		hci_mode = SCAN_DISABLED;
	else if (strcasecmp(MODE_CONNECTABLE, scan_mode) == 0)
		hci_mode = SCAN_PAGE;
	else if (strcasecmp(MODE_DISCOVERABLE, scan_mode) == 0)
		hci_mode = (SCAN_PAGE | SCAN_INQUIRY);
	else
		return error_invalid_arguments(conn, msg);

	dd = hci_open_dev(adapter->dev_id);
	if (dd < 0)
		return error_no_such_adapter(conn, msg);

	if (!adapter->up) {
		bdaddr_t local;

		str2ba(adapter->address, &local);
		/* The new value will be loaded when the adapter comes UP */
		write_device_mode(&local, scan_mode);

		/* Start HCI device */
		if (ioctl(dd, HCIDEVUP, adapter->dev_id) ==  0)
			goto done; /* on success */

		if (errno != EALREADY) {
			int err = errno;
			error("Can't init device hci%d: %s (%d)\n",
				adapter->dev_id, strerror(errno), errno);

			hci_close_dev(dd);
			return error_failed(conn, msg, err);
		}
	}

	/* Check if the new requested mode is different from the current */
	if (current_mode != hci_mode) {
		struct hci_request rq;
		uint8_t status = 0;

		memset(&rq, 0, sizeof(rq));
		rq.ogf    = OGF_HOST_CTL;
		rq.ocf    = OCF_WRITE_SCAN_ENABLE;
		rq.cparam = &hci_mode;
		rq.clen   = sizeof(hci_mode);
		rq.rparam = &status;
		rq.rlen   = sizeof(status);
		rq.event = EVT_CMD_COMPLETE;

		if (hci_send_req(dd, &rq, 1000) < 0) {
			int err = errno;
			error("Sending write scan enable command failed: %s (%d)",
					strerror(errno), errno);
			hci_close_dev(dd);
			return error_failed(conn, msg, err);
		}

		if (status) {
			error("Setting scan enable failed with status 0x%02x",
					status);
			hci_close_dev(dd);
			return error_failed(conn, msg, bt_error(status));
		}
	}

done:
	hci_close_dev(dd);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_get_discoverable_to(DBusConnection *conn,
							DBusMessage *msg,
							void *data)
{
	const struct adapter *adapter = data;
	DBusMessage *reply;

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return error_invalid_arguments(conn, msg);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_UINT32, &adapter->discov_timeout,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_set_discoverable_to(DBusConnection *conn,
							DBusMessage *msg,
							void *data)
{
	struct adapter *adapter = data;
	DBusMessage *reply;
	DBusError err;
	uint32_t timeout;
	bdaddr_t bdaddr;

	if (!adapter->up)
		return error_not_ready(conn, msg);

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_UINT32, &timeout,
				DBUS_TYPE_INVALID);
 
	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	if (adapter->timeout_id) {
		g_timeout_remove(adapter->timeout_id);
		adapter->timeout_id = 0;
	}

	if ((timeout != 0) && (adapter->mode & SCAN_INQUIRY))
		adapter->timeout_id = g_timeout_add(timeout * 1000,
						discov_timeout_handler,
						adapter);

	adapter->discov_timeout = timeout;

	str2ba(adapter->address, &bdaddr);
	write_discoverable_timeout(&bdaddr, timeout);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_is_connectable(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	const struct adapter *adapter = data;
	DBusMessage *reply;
	const uint8_t hci_mode = adapter->mode;
	dbus_bool_t connectable = FALSE;

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return error_invalid_arguments(conn, msg);

	if (hci_mode & SCAN_PAGE)
		connectable = TRUE;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &connectable,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_is_discoverable(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	const struct adapter *adapter = data;
	DBusMessage *reply;
	const uint8_t hci_mode = adapter->mode;
	dbus_bool_t discoverable = FALSE;

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return error_invalid_arguments(conn, msg);

	if (hci_mode & SCAN_INQUIRY)
		discoverable = TRUE;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &discoverable,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_is_connected(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	DBusError err;
	dbus_bool_t connected = FALSE;

	struct adapter *adapter = data;
	struct slist *l = adapter->active_conn;

	const char *peer_addr;
	bdaddr_t peer_bdaddr;

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &peer_addr,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	if (check_address(peer_addr) < 0)
		return error_invalid_arguments(conn, msg);

	str2ba(peer_addr, &peer_bdaddr);

	l = slist_find(l, &peer_bdaddr, active_conn_find_by_bdaddr);
	if (l)
		connected = TRUE;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &connected,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_list_connections(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter array_iter;
	struct adapter *adapter = data;
	struct slist *l = adapter->active_conn;

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return error_invalid_arguments(conn, msg);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					DBUS_TYPE_STRING_AS_STRING, &array_iter);

	while (l) {
		char *peer_addr;
		bdaddr_t tmp;
		struct active_conn_info *dev = l->data;

		baswap(&tmp, &dev->bdaddr); peer_addr = batostr(&tmp);

		dbus_message_iter_append_basic(&array_iter, DBUS_TYPE_STRING,
						&peer_addr);
		bt_free(peer_addr);

		l = l->next;
	}

	dbus_message_iter_close_container(&iter, &array_iter);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_get_major_class(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	const struct adapter *adapter = data;
	DBusMessage *reply;
	const char *str_ptr = "computer";
	uint8_t cls[3];
	int dd;

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return error_invalid_arguments(conn, msg);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dd = hci_open_dev(adapter->dev_id);
	if (dd < 0)
		return error_no_such_adapter(conn, msg);

	if (hci_read_class_of_dev(dd, cls, 1000) < 0) {
		int err = errno;
		error("Can't read class of device on hci%d: %s(%d)",
				adapter->dev_id, strerror(errno), errno);
		hci_close_dev(dd);
		return error_failed(conn, msg, err);
	}

	hci_close_dev(dd);

	/* FIXME: Currently, only computer major class is supported */
	if ((cls[1] & 0x1f) != 1)
		return error_unsupported_major_class(conn, msg);

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &str_ptr,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_list_minor_classes(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	const struct adapter *adapter = data;
	DBusMessage *reply = NULL;
	DBusMessageIter iter;
	DBusMessageIter array_iter;
	const char **minor_ptr;
	uint8_t cls[3];
	uint8_t major_class;
	int dd, size, i;

	if (!adapter->up)
		return error_not_ready(conn, msg);

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return error_invalid_arguments(conn, msg);

	dd = hci_open_dev(adapter->dev_id);
	if (dd < 0)
		return error_no_such_adapter(conn, msg);

	if (hci_read_class_of_dev(dd, cls, 1000) < 0) {
		int err = errno;
		error("Can't read class of device on hci%d: %s(%d)",
				adapter->dev_id, strerror(errno), errno);
		hci_close_dev(dd);
		return error_failed(conn, msg, err);
	}

	hci_close_dev(dd);

	major_class = cls[1] & 0x1F;

	switch (major_class) {
	case 1: /* computer */
		minor_ptr = computer_minor_cls;
		size = sizeof(computer_minor_cls) / sizeof(*computer_minor_cls);
		break;
	case 2: /* phone */
		minor_ptr = phone_minor_cls;
		size = sizeof(phone_minor_cls) / sizeof(*phone_minor_cls);
		break;
	default:
		return error_unsupported_major_class(conn, msg);
	}

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					 	DBUS_TYPE_STRING_AS_STRING, &array_iter);
	for (i = 0; i < size; i++)
		dbus_message_iter_append_basic(&array_iter, DBUS_TYPE_STRING,
						&minor_ptr[i]);

	dbus_message_iter_close_container(&iter, &array_iter);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_get_minor_class(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct adapter *adapter = data;
	DBusMessage *reply;
	const char *str_ptr = "";
	uint8_t cls[3];
	uint8_t minor_class;
	int dd;

	if (!adapter->up)
		return error_not_ready(conn, msg);

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return error_invalid_arguments(conn, msg);

	dd = hci_open_dev(adapter->dev_id);
	if (dd < 0)
		return error_no_such_adapter(conn, msg);

	if (hci_read_class_of_dev(dd, cls, 1000) < 0) {
		int err = errno;
		error("Can't read class of device on hci%d: %s(%d)",
				adapter->dev_id, strerror(errno), errno);
		hci_close_dev(dd);
		return error_failed(conn, msg, err);
	}

	hci_close_dev(dd);

	/* FIXME: Currently, only computer major class is supported */
	if ((cls[1] & 0x1f) != 1)
		return error_unsupported_major_class(conn, msg);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	minor_class = cls[0] >> 2;

	/* Validate computer minor class */
	if (minor_class > (sizeof(computer_minor_cls) / sizeof(*computer_minor_cls)))
		goto failed;

	str_ptr = computer_minor_cls[minor_class];

failed:
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &str_ptr,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_set_minor_class(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct adapter *adapter = data;
	DBusMessage *reply, *signal;
	DBusError err;
	bdaddr_t bdaddr;
	const char *minor;
	uint8_t cls[3];
	uint32_t dev_class = 0xFFFFFFFF;
	int i, dd;

	if (!adapter->up)
		return error_not_ready(conn, msg);

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &minor,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	if (!minor)
		return error_invalid_arguments(conn, msg);

	dd = hci_open_dev(adapter->dev_id);
	if (dd < 0)
		return error_no_such_adapter(conn, msg);

	if (hci_read_class_of_dev(dd, cls, 1000) < 0) {
		int err = errno;
		error("Can't read class of device on hci%d: %s(%d)",
				adapter->dev_id, strerror(errno), errno);
		hci_close_dev(dd);
		return error_failed(conn, msg, err);
	}

	/* Currently, only computer major class is supported */
	if ((cls[1] & 0x1f) != 1)
		return error_unsupported_major_class(conn, msg);

	for (i = 0; i < sizeof(computer_minor_cls) / sizeof(*computer_minor_cls); i++)
		if (!strcasecmp(minor, computer_minor_cls[i])) {
			/* Remove the format type */
			dev_class = i << 2;
			break;
		}

	/* Check if it's a valid minor class */
	if (dev_class == 0xFFFFFFFF)
		return error_invalid_arguments(conn, msg);

	/* update the minor class before store */
	cls[0] = (dev_class & 0xff);

	/* set the service class and major class  */
	dev_class |= (cls[2] << 16) | (cls[1] << 8);

	hci_devba(adapter->dev_id, &bdaddr);

	write_local_class(&bdaddr, cls);

	if (hci_write_class_of_dev(dd, dev_class, 2000) < 0) {
		int err = errno;
		error("Can't write class of device on hci%d: %s(%d)",
				adapter->dev_id, strerror(errno), errno);
		hci_close_dev(dd);
		return error_failed(conn, msg, err);
	}

	signal = dev_signal_factory(adapter->dev_id, "MinorClassChanged",
						DBUS_TYPE_STRING, &minor,
						DBUS_TYPE_INVALID);
	send_message_and_unref(conn, signal);

	reply = dbus_message_new_method_return(msg);

	hci_close_dev(dd);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_get_service_classes(DBusConnection *conn,
							DBusMessage *msg,
							void *data)
{
	struct adapter *adapter = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter array_iter;
	const char *str_ptr;
	uint8_t cls[3];
	int dd, i;

	if (!adapter->up)
		return error_not_ready(conn, msg);

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return error_invalid_arguments(conn, msg);

	dd = hci_open_dev(adapter->dev_id);
	if (dd < 0)
		return error_no_such_adapter(conn, msg);

	if (hci_read_class_of_dev(dd, cls, 1000) < 0) {
		int err = errno;
		error("Can't read class of device on hci%d: %s(%d)",
				adapter->dev_id, strerror(errno), errno);
		hci_close_dev(dd);
		return error_failed(conn, msg, err);
	}

	reply = dbus_message_new_method_return(msg);

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
				DBUS_TYPE_STRING_AS_STRING, &array_iter);

	for (i = 0; i < (sizeof(service_cls) / sizeof(*service_cls)); i++) {
		if (cls[2] & (1 << i)) {
			str_ptr = service_cls[i];
			dbus_message_iter_append_basic(&array_iter,
						DBUS_TYPE_STRING, &str_ptr);
		}
	}

	dbus_message_iter_close_container(&iter, &array_iter);

	hci_close_dev(dd);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_get_name(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct adapter *adapter = data;
	DBusMessage *reply;
	char str[249], *str_ptr = str;
	int err;
	bdaddr_t ba;

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return error_invalid_arguments(conn, msg);

	str2ba(adapter->address, &ba);

	err = read_local_name(&ba, str);
	if (err < 0) {
		if (!adapter->up)
			return error_not_ready(conn, msg);

		err = get_device_name(adapter->dev_id, str, sizeof(str));
		if (err < 0)
			return error_failed(conn, msg, -err);
	}

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &str_ptr,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_set_name(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct adapter *adapter = data;
	DBusMessage *reply;
	DBusError err;
	bdaddr_t bdaddr;
	char *str_ptr;
	int ecode;

	if (!adapter->up)
		return error_not_ready(conn, msg);

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &str_ptr,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}
	
	if (!g_utf8_validate(str_ptr, -1, NULL)) {
		error("Name change failed: the supplied name isn't valid UTF-8");
		return error_invalid_arguments(conn, msg);
	}

	hci_devba(adapter->dev_id, &bdaddr);

	write_local_name(&bdaddr, str_ptr);

	ecode = set_device_name(adapter->dev_id, str_ptr);
	if (ecode < 0)
		return error_failed(conn, msg, -ecode);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_get_remote_svc(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	return get_remote_svc_rec(conn, msg, data);
}

static DBusHandlerResult adapter_get_remote_svc_handles(DBusConnection *conn,
							DBusMessage *msg,
							void *data)
{
	return get_remote_svc_handles(conn, msg, data);
}

static DBusHandlerResult adapter_get_remote_version(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct adapter *adapter = data;
	DBusMessage *reply;
	DBusError err;
	char filename[PATH_MAX + 1];
	char *addr_ptr, *str;
	const char *str_ver;
	char info_array[64], *info = info_array;
	int compid, ver, subver;
	uint8_t features;

	dbus_error_init(&err);

	memset(info_array, 0, 64);

	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &addr_ptr,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	if (check_address(addr_ptr) < 0)
		return error_invalid_arguments(conn, msg);

	create_name(filename, PATH_MAX, STORAGEDIR, adapter->address,
			"manufacturers");

	str = textfile_caseget(filename, addr_ptr);
	if (!str)
		return error_not_available(conn, msg);

	if (sscanf(str, "%d %d %d", &compid, &ver, &subver) != 3) {
		/* corrupted file data */
		free(str);
		goto failed;
	}

	free(str);

	str_ver = lmp_vertostr(ver);

	/* default value */
	snprintf(info, 64, "Bluetooth %s", str_ver);

	create_name(filename, PATH_MAX, STORAGEDIR, adapter->address,
			"features");

	str = textfile_caseget(filename, addr_ptr);
	if (!str)
		goto failed;

	/* check if the data is not corrupted */
	if (strlen(str) == 16) {
		/* Getting the third byte */
		features  = ((str[6] - 48) << 4) | (str[7] - 48);
		if (features & (LMP_EDR_ACL_2M | LMP_EDR_ACL_3M))
			snprintf(info, 64, "Bluetooth %s + EDR", str_ver);
	}

	free(str);

failed:

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &info,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_get_remote_revision(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct adapter *adapter = data;
	DBusMessage *reply;
	DBusError err;
	char filename[PATH_MAX + 1];
	char *addr_ptr, *str;
	char info_array[16], *info = info_array;
	int compid, ver, subver;

	dbus_error_init(&err);

	memset(info_array, 0, 16);

	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &addr_ptr,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	if (check_address(addr_ptr) < 0)
		return error_invalid_arguments(conn, msg);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	create_name(filename, PATH_MAX, STORAGEDIR, adapter->address,
			"manufacturers");

	str = textfile_caseget(filename, addr_ptr);
	if (!str)
		return error_not_available(conn, msg);

	if (sscanf(str, "%d %d %d", &compid, &ver, &subver) == 3)
		snprintf(info, 16, "HCI 0x%X", subver);

	free(str);

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &info,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_get_remote_manufacturer(DBusConnection *conn,
							DBusMessage *msg,
							void *data)
{
	struct adapter *adapter = data;
	DBusMessage *reply;
	DBusError err;
	char filename[PATH_MAX + 1];
	char *addr_ptr, *str;
	char info_array[64], *info = info_array;
	int compid, ver, subver;
	dbus_error_init(&err);

	memset(info_array, 0, 64);

	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &addr_ptr,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	if (check_address(addr_ptr) < 0)
		return error_invalid_arguments(conn, msg);

	create_name(filename, PATH_MAX, STORAGEDIR, adapter->address,
			"manufacturers");

	str = textfile_caseget(filename, addr_ptr);
	if (!str)
		return error_not_available(conn, msg);

	if (sscanf(str, "%d %d %d", &compid, &ver, &subver) == 3)
		info = bt_compidtostr(compid);

	free(str);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &info,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_get_remote_company(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	DBusError err;
	bdaddr_t bdaddr;
	char oui[9], *str_bdaddr, *tmp;

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &str_bdaddr,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	str2ba(str_bdaddr, &bdaddr);
	ba2oui(&bdaddr, oui);

	tmp = ouitocomp(oui);
	if (!tmp)
		return error_not_available(conn, msg);

	reply = dbus_message_new_method_return(msg);
	if (!reply) {
		free(tmp);
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	}

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &tmp,
					DBUS_TYPE_INVALID);

	free(tmp);

	return send_message_and_unref(conn, reply);
}

static int get_remote_class(DBusConnection *conn, DBusMessage *msg, void *data,
				uint32_t *class)
{
	struct adapter *adapter = data;
	char *addr_peer;
	DBusError err;
	bdaddr_t local, peer;
	int ecode;

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &addr_peer,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		error_invalid_arguments(conn, msg);
		return -1;
	}

	if (check_address(addr_peer) < 0) {
		error_invalid_arguments(conn, msg);
		return -1;
	}

	str2ba(addr_peer, &peer);
	str2ba(adapter->address, &local);

	ecode = read_remote_class(&local, &peer, class);
	if (ecode < 0) {
		error_not_available(conn, msg);
		return -1;
	}

	return 0;
}

static DBusHandlerResult adapter_get_remote_major_class(DBusConnection *conn,
							DBusMessage *msg,
							void *data)
{
	DBusMessage *reply;
	const char *major_class;
	uint32_t class;

	if (get_remote_class(conn, msg, data, &class) < 0)
		return DBUS_HANDLER_RESULT_HANDLED;

	major_class = major_class_str(class);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &major_class,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_get_remote_minor_class(DBusConnection *conn,
							DBusMessage *msg,
							void *data)
{
	DBusMessage *reply;
	const char *minor_class;
	uint32_t class;

	if (get_remote_class(conn, msg, data, &class) < 0)
		return DBUS_HANDLER_RESULT_HANDLED;

	minor_class = minor_class_str(class);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &minor_class,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static void append_class_string(const char *class, DBusMessageIter *iter)
{
	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &class);
}

static DBusHandlerResult adapter_get_remote_service_cls(DBusConnection *conn,
							DBusMessage *msg,
							void *data)
{
	DBusMessage *reply;
	DBusMessageIter iter, array_iter;
	struct slist *service_classes;
	uint32_t class;

	if (get_remote_class(conn, msg, data, &class) < 0)
		return DBUS_HANDLER_RESULT_HANDLED;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	service_classes = service_classes_str(class);

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
	 					DBUS_TYPE_STRING_AS_STRING, &array_iter);

	slist_foreach(service_classes, (slist_func_t) append_class_string,
			&array_iter);

	dbus_message_iter_close_container(&iter, &array_iter);

	slist_free(service_classes);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_get_remote_class(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	uint32_t class;

	if (get_remote_class(conn, msg, data, &class) < 0)
		return DBUS_HANDLER_RESULT_HANDLED;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_UINT32, &class,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_get_remote_name(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	char filename[PATH_MAX + 1];
	struct adapter *adapter = data;
	DBusMessage *reply = NULL;
	DBusError err;
	const char *peer_addr;
	bdaddr_t peer_bdaddr;
	char *str;

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &peer_addr,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	if (check_address(peer_addr) < 0)
		return error_invalid_arguments(conn, msg);

	/* check if it is in the cache */
	create_name(filename, PATH_MAX, STORAGEDIR, adapter->address, "names");

	str = textfile_caseget(filename, peer_addr);

	if (str) {
		reply = dbus_message_new_method_return(msg);
		if (!reply) {
			free(str);
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		}

		/* send the cached name */
		dbus_message_append_args(reply, DBUS_TYPE_STRING, &str,
						DBUS_TYPE_INVALID);

		free(str);
		return send_message_and_unref(conn, reply);
	}

	if (!adapter->up)
		return error_not_ready(conn, msg);

	/* put the request name in the queue to resolve name */
	str2ba(peer_addr, &peer_bdaddr);
	found_device_add(&adapter->found_devices, &peer_bdaddr, 0, NAME_REQUIRED);

	/* 
	 * if there is a discover process running, just queue the request.
	 * Otherwise, send the HCI cmd to get the remote name
	 */
	if (!(adapter->discov_active ||  adapter->pdiscov_active))
		found_device_req_name(adapter);

	return error_request_deferred(conn, msg);
}

static DBusHandlerResult adapter_get_remote_alias(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct adapter *adapter = data;
	DBusMessage *reply;
	DBusError err;
	char str[249], *str_ptr = str, *addr_ptr;
	bdaddr_t bdaddr;
	int ecode;

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &addr_ptr,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	if (check_address(addr_ptr) < 0)
		return error_invalid_arguments(conn, msg);

	str2ba(addr_ptr, &bdaddr);

	ecode = get_device_alias(adapter->dev_id, &bdaddr, str, sizeof(str));
	if (ecode < 0)
		return error_not_available(conn, msg);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &str_ptr,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_set_remote_alias(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct adapter *adapter = data;
	DBusMessage *reply, *signal;
	DBusError err;
	char *alias, *addr;
	bdaddr_t bdaddr;
	int ecode;

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &addr,
				DBUS_TYPE_STRING, &alias,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	if ((strlen(alias) == 0) || (check_address(addr) < 0)) {
		error("Alias change failed: Invalid parameter");
		return error_invalid_arguments(conn, msg);
	}

	str2ba(addr, &bdaddr);

	ecode = set_device_alias(adapter->dev_id, &bdaddr, alias);
	if (ecode < 0)
		return error_failed(conn, msg, -ecode);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	signal = dev_signal_factory(adapter->dev_id, "RemoteAliasChanged",
						DBUS_TYPE_STRING, &addr,
						DBUS_TYPE_STRING, &alias,
						DBUS_TYPE_INVALID);
	send_message_and_unref(conn, signal);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_clear_remote_alias(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct adapter *adapter = data;
	DBusMessage *reply, *signal;
	DBusError err;
	char *addr_ptr;
	bdaddr_t bdaddr;
	int ecode, had_alias = 1;

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &addr_ptr,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message argument:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	if (check_address(addr_ptr) < 0) {
		error("Alias clear failed: Invalid parameter");
		return error_invalid_arguments(conn, msg);
	}

	str2ba(addr_ptr, &bdaddr);

	ecode = get_device_alias(adapter->dev_id, &bdaddr, NULL, 0);
	if (ecode == -ENXIO) 
		had_alias = 0;

	ecode = set_device_alias(adapter->dev_id, &bdaddr, NULL);
	if (ecode < 0)
		return error_failed(conn, msg, -ecode);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	if (had_alias) {
		signal = dev_signal_factory(adapter->dev_id,
						"RemoteAliasCleared",
						DBUS_TYPE_STRING, &addr_ptr,
						DBUS_TYPE_INVALID);
		send_message_and_unref(conn, signal);
	}

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_last_seen(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct adapter *adapter = data;
	DBusMessage *reply;
	DBusError err;
	char filename[PATH_MAX + 1];
	char *addr_ptr, *str;

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &addr_ptr,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	if (check_address(addr_ptr) < 0)
		return error_invalid_arguments(conn, msg);

	create_name(filename, PATH_MAX, STORAGEDIR, adapter->address,
			"lastseen");

	str = textfile_caseget(filename, addr_ptr);
	if (!str)
		return error_not_available(conn, msg);

	reply = dbus_message_new_method_return(msg);
	if (!reply) {
		free(str);
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	}

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &str,
					DBUS_TYPE_INVALID);

	free(str);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_last_used(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct adapter *adapter = data;
	DBusMessage *reply;
	DBusError err;
	char filename[PATH_MAX + 1];
	char *addr_ptr, *str;

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &addr_ptr,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	if (check_address(addr_ptr) < 0)
		return error_invalid_arguments(conn, msg);

	create_name(filename, PATH_MAX, STORAGEDIR, adapter->address,
			"lastused");

	str = textfile_caseget(filename, addr_ptr);
	if (!str)
		return error_not_available(conn, msg);

	reply = dbus_message_new_method_return(msg);
	if (!reply) {
		free(str);
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	}

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &str,
					DBUS_TYPE_INVALID);

	free(str);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_dc_remote_device(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	DBusError err;

	struct adapter *adapter = data;
	struct slist *l = adapter->active_conn;

	const char *peer_addr;
	bdaddr_t peer_bdaddr;
	int dd;
	struct active_conn_info *dev;

	if (!adapter->up)
		return error_not_ready(conn, msg);

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &peer_addr,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	if (check_address(peer_addr) < 0)
		return error_invalid_arguments(conn, msg);

	str2ba(peer_addr, &peer_bdaddr);

	l = slist_find(l, &peer_bdaddr, active_conn_find_by_bdaddr);
	if (!l)
		return error_not_connected(conn, msg);

	dev = l->data;

	dd = hci_open_dev(adapter->dev_id);
	if (dd < 0)
		return error_no_such_adapter(conn, msg);

	/* Send the HCI disconnect command */
	if (hci_disconnect(dd, dev->handle, HCI_OE_USER_ENDED_CONNECTION,
				500) < 0) {
		int err = errno;
		error("Disconnect failed");
		hci_close_dev(dd);
		return error_failed(conn, msg, err);
	}

	hci_close_dev(dd);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	return send_message_and_unref(conn, reply);

}

static void reply_authentication_failure(struct bonding_request_info *bonding)
{
	DBusMessage *reply;
	int status;

	status = bonding->hci_status ?
			bonding->hci_status : HCI_AUTHENTICATION_FAILURE;

	reply = new_authentication_return(bonding->rq, status);
	if (reply)
		send_message_and_unref(bonding->conn, reply);
}

static gboolean create_bonding_conn_complete(GIOChannel *io, GIOCondition cond,
						struct adapter *adapter)
{
	struct hci_request rq;
	auth_requested_cp cp;
	evt_cmd_status rp;
	struct l2cap_conninfo cinfo;
	socklen_t len;
	int sk, dd, ret;

	if (!adapter->bonding) {
		/* If we come here it implies a bug somewhere */
		debug("create_bonding_conn_complete: no pending bonding!");
		g_io_channel_close(io);
		g_io_channel_unref(io);
		return FALSE;
	}

	if (cond & G_IO_NVAL) {
		error_authentication_canceled(adapter->bonding->conn,
						adapter->bonding->rq);
		goto cleanup;
	}

	if (cond & (G_IO_HUP | G_IO_ERR)) {
		debug("Hangup or error on bonding IO channel");

		if (!adapter->bonding->auth_active)
			error_connection_attempt_failed(adapter->bonding->conn,
							adapter->bonding->rq,
							ENETDOWN);
		else
			reply_authentication_failure(adapter->bonding);

		goto failed;
	}

	sk = g_io_channel_unix_get_fd(io);

	len = sizeof(ret);
	if (getsockopt(sk, SOL_SOCKET, SO_ERROR, &ret, &len) < 0) {
		error("Can't get socket error: %s (%d)",
				strerror(errno), errno);
		error_failed(adapter->bonding->conn, adapter->bonding->rq,
				errno);
		goto failed;
	}

	if (ret != 0) {
		if (adapter->bonding->auth_active)
			reply_authentication_failure(adapter->bonding);
		else
			error_connection_attempt_failed(adapter->bonding->conn,
							adapter->bonding->rq,
							ret);
		goto failed;
	}

	len = sizeof(cinfo);
	if (getsockopt(sk, SOL_L2CAP, L2CAP_CONNINFO, &cinfo, &len) < 0) {
		error("Can't get connection info: %s (%d)",
				strerror(errno), errno);
		error_failed(adapter->bonding->conn, adapter->bonding->rq,
				errno);
		goto failed;
	}

	dd = hci_open_dev(adapter->dev_id);
	if (dd < 0) {
		error_no_such_adapter(adapter->bonding->conn,
					adapter->bonding->rq);
		goto failed;
	}

	memset(&rp, 0, sizeof(rp));

	memset(&cp, 0, sizeof(cp));
	cp.handle = htobs(cinfo.hci_handle);

	memset(&rq, 0, sizeof(rq));
	rq.ogf    = OGF_LINK_CTL;
	rq.ocf    = OCF_AUTH_REQUESTED;
	rq.cparam = &cp;
	rq.clen   = AUTH_REQUESTED_CP_SIZE;
	rq.rparam = &rp;
	rq.rlen   = EVT_CMD_STATUS_SIZE;
	rq.event  = EVT_CMD_STATUS;

	if (hci_send_req(dd, &rq, 500) < 0) {
		error("Unable to send HCI request: %s (%d)",
					strerror(errno), errno);
		error_failed(adapter->bonding->conn, adapter->bonding->rq,
				errno);
		hci_close_dev(dd);
		goto failed;
	}

	if (rp.status) {
		error("HCI_Authentication_Requested failed with status 0x%02x",
				rp.status);
		error_failed(adapter->bonding->conn, adapter->bonding->rq,
				bt_error(rp.status));
		hci_close_dev(dd);
		goto failed;
	}

	hci_close_dev(dd);

	adapter->bonding->auth_active = 1;

	adapter->bonding->io_id = g_io_add_watch(io,
						G_IO_NVAL | G_IO_HUP | G_IO_ERR,
						(GIOFunc) create_bonding_conn_complete,
						adapter);

	return FALSE;

failed:
	g_io_channel_close(io);

cleanup:
	name_listener_remove(adapter->bonding->conn,
				dbus_message_get_sender(adapter->bonding->rq),
				(name_cb_t) create_bond_req_exit, adapter);

	bonding_request_free(adapter->bonding);
	adapter->bonding = NULL;

	return FALSE;
}

static DBusHandlerResult adapter_create_bonding(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	char filename[PATH_MAX + 1];
	DBusError err;
	char *str, *peer_addr = NULL;
	struct adapter *adapter = data;
	bdaddr_t peer_bdaddr;
	int sk;

	if (!adapter->up)
		return error_not_ready(conn, msg);

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &peer_addr,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	if (check_address(peer_addr) < 0)
		return error_invalid_arguments(conn, msg);

	str2ba(peer_addr, &peer_bdaddr);

	/* check if there is a pending discover: requested by D-Bus/non clients */
	if (adapter->discov_active || (adapter->pdiscov_active && !adapter->pinq_idle))
		return error_discover_in_progress(conn, msg);

	pending_remote_name_cancel(adapter);

	if (adapter->bonding)
		return error_bonding_in_progress(conn, msg);

	if (slist_find(adapter->pin_reqs, &peer_bdaddr, pin_req_cmp))
		return error_bonding_in_progress(conn, msg);

	/* check if a link key already exists */
	create_name(filename, PATH_MAX, STORAGEDIR, adapter->address,
			"linkkeys");

	str = textfile_caseget(filename, peer_addr);
	if (str) {
		free(str);
		return error_bonding_already_exists(conn, msg);
	}

	sk = l2raw_connect(adapter->address, &peer_bdaddr);
	if (sk < 0)
		return error_connection_attempt_failed(conn, msg, 0);

	adapter->bonding = bonding_request_new(&peer_bdaddr, conn, msg);
	if (!adapter->bonding) {
		close(sk);
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	}

	adapter->bonding->io = g_io_channel_unix_new(sk);
	adapter->bonding->io_id = g_io_add_watch(adapter->bonding->io,
						G_IO_OUT | G_IO_NVAL | G_IO_HUP | G_IO_ERR,
						(GIOFunc) create_bonding_conn_complete,
						adapter);

	name_listener_add(conn, dbus_message_get_sender(msg),
			(name_cb_t) create_bond_req_exit, adapter);

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult adapter_cancel_bonding(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct adapter *adapter = data;
	DBusMessage *reply;
	DBusError err;
	bdaddr_t peer_bdaddr;
	const char *peer_addr;
	struct slist *l;

	if (!adapter->up)
		return error_not_ready(conn, msg);

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &peer_addr,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	if (check_address(peer_addr) < 0)
		return error_invalid_arguments(conn, msg);

	str2ba(peer_addr, &peer_bdaddr);

	if (!adapter->bonding || bacmp(&adapter->bonding->bdaddr, &peer_bdaddr))
		return error_bonding_not_in_progress(conn, msg);

	if (strcmp(dbus_message_get_sender(adapter->bonding->rq),
				dbus_message_get_sender(msg)))
		return error_not_authorized(conn, msg);

	adapter->bonding->cancel = 1;

	l = slist_find(adapter->pin_reqs, &peer_bdaddr, pin_req_cmp);
	if (l) {
		struct pending_pin_info *pin_req = l->data;

		if (pin_req->replied) {
			/*
			 * If disconnect can't be applied and the PIN code
			 * request was already replied it doesn't make sense
			 * cancel the remote passkey: return not authorized.
			 */
			g_io_channel_close(adapter->bonding->io);
			return error_not_authorized(conn, msg);
		} else {
			int dd = hci_open_dev(adapter->dev_id);
			if (dd < 0) {
				error("Can't open hci%d: %s (%d)",
					adapter->dev_id, strerror(errno), errno);
				return DBUS_HANDLER_RESULT_HANDLED;
			}

			hci_send_cmd(dd, OGF_LINK_CTL, OCF_PIN_CODE_NEG_REPLY,
					6, &peer_bdaddr);

			hci_close_dev(dd);
		} 

		adapter->pin_reqs = slist_remove(adapter->pin_reqs, pin_req);
		free(pin_req);
	}

	g_io_channel_close(adapter->bonding->io);

	reply = dbus_message_new_method_return(msg);
	send_message_and_unref(conn, reply);

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult adapter_remove_bonding(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct adapter *adapter = data;
	struct slist *l;
	DBusMessage *reply;
	DBusMessage *signal;
	DBusError err;
	char filename[PATH_MAX + 1];
	char *addr_ptr, *str;
	bdaddr_t bdaddr;
	int dd;

	if (!adapter->up)
		return error_not_ready(conn, msg);

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &addr_ptr,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	if (check_address(addr_ptr) < 0)
		return error_invalid_arguments(conn, msg);

	dd = hci_open_dev(adapter->dev_id);
	if (dd < 0)
		return error_no_such_adapter(conn, msg);

	create_name(filename, PATH_MAX, STORAGEDIR, adapter->address,
			"linkkeys");

	/* textfile_del doesn't return an error when the key is not found */
	str = textfile_caseget(filename, addr_ptr);
	if (!str) {
		hci_close_dev(dd);
		return error_bonding_does_not_exist(conn, msg);
	}

	free(str);

	/* Delete the link key from storage */
	if (textfile_del(filename, addr_ptr) < 0) {
		int err = errno;
		hci_close_dev(dd);
		return error_failed(conn, msg, err);
	}

	str2ba(addr_ptr, &bdaddr);

	/* Delete the link key from the Bluetooth chip */
	hci_delete_stored_link_key(dd, &bdaddr, 0, 1000);

	/* find the connection */
	l = slist_find(adapter->active_conn, &bdaddr,
			active_conn_find_by_bdaddr);
	if (l) {
		struct active_conn_info *con = l->data;
		/* Send the HCI disconnect command */
		if (hci_disconnect(dd, htobs(con->handle),
					HCI_OE_USER_ENDED_CONNECTION, 500) < 0) {
			int err = errno;
			error("Disconnect failed");
			hci_close_dev(dd);
			return error_failed(conn, msg, err);
		}
	}

	/* FIXME: which condition must be verified before send the signal */
	signal = dev_signal_factory(adapter->dev_id, "BondingRemoved",
					DBUS_TYPE_STRING, &addr_ptr,
					DBUS_TYPE_INVALID);
	send_message_and_unref(conn, signal);

	reply = dbus_message_new_method_return(msg);

	hci_close_dev(dd);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_has_bonding(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct adapter *adapter = data;
	DBusMessage *reply;
	DBusError err;
	char filename[PATH_MAX + 1];
	char *addr_ptr, *str;
	dbus_bool_t result;

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &addr_ptr,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	if (check_address(addr_ptr) < 0)
		return error_invalid_arguments(conn, msg);

	create_name(filename, PATH_MAX, STORAGEDIR, adapter->address,
			"linkkeys");

	str = textfile_caseget(filename, addr_ptr);
	if (str) {
		result = TRUE;
		free(str);
	} else
		result = FALSE;

	reply = dbus_message_new_method_return(msg);

	dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &result,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static void list_bondings_do_append(char *key, char *value, void *data)
{
	DBusMessageIter *iter = data;
	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &key);
}

static DBusHandlerResult adapter_list_bondings(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct adapter *adapter = data;
	DBusMessageIter iter;
	DBusMessageIter array_iter;
	DBusMessage *reply;
	char filename[PATH_MAX + 1];

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return error_invalid_arguments(conn, msg);

	create_name(filename, PATH_MAX, STORAGEDIR, adapter->address,
			"linkkeys");

	reply = dbus_message_new_method_return(msg);

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
				DBUS_TYPE_STRING_AS_STRING, &array_iter);

	textfile_foreach(filename, list_bondings_do_append, &array_iter);

	dbus_message_iter_close_container(&iter, &array_iter);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_get_pin_code_length(DBusConnection *conn,
							DBusMessage *msg,
							void *data)
{
	struct adapter *adapter = data;
	DBusMessage *reply;
	DBusError err;
	bdaddr_t local, peer;
	char *addr_ptr;
	uint8_t length;
	int len;

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &addr_ptr,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	if (check_address(addr_ptr) < 0)
		return error_invalid_arguments(conn, msg);

	str2ba(adapter->address, &local);

	str2ba(addr_ptr, &peer);

	len = read_pin_length(&local, &peer);
	if (len < 0)
		return error_record_does_not_exist(conn, msg);

	reply = dbus_message_new_method_return(msg);

	length = len;

	dbus_message_append_args(reply, DBUS_TYPE_BYTE, &length,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_get_encryption_key_size(DBusConnection *conn,
							DBusMessage *msg,
							void *data)
{
	struct adapter *adapter = data;
	DBusMessage *reply;
	DBusError err;
	bdaddr_t bdaddr;
	char *addr_ptr;
	uint8_t size;
	int val;

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_STRING, &addr_ptr,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	if (check_address(addr_ptr) < 0)
		return error_invalid_arguments(conn, msg);

	str2ba(addr_ptr, &bdaddr);

	val = get_encryption_key_size(adapter->dev_id, &bdaddr);
	if (val < 0)
		return error_failed(conn, msg, -val);

	reply = dbus_message_new_method_return(msg);

	size = val;

	dbus_message_append_args(reply, DBUS_TYPE_BYTE, &size,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_start_periodic(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	periodic_inquiry_cp cp;
	struct hci_request rq;
	struct adapter *adapter = data;
	uint8_t lap[3] = { 0x33, 0x8b, 0x9e };
	uint8_t status;
	int dd;

	if (!adapter->up)
		return error_not_ready(conn, msg);

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return error_invalid_arguments(conn, msg);

	if (adapter->discov_active || adapter->pdiscov_active)
		return error_discover_in_progress(conn, msg);

	pending_remote_name_cancel(adapter);

	dd = hci_open_dev(adapter->dev_id);
	if (dd < 0)
		return error_no_such_adapter(conn, msg);

	memset(&cp, 0, sizeof(cp));
	memcpy(&cp.lap, lap, 3);
	cp.max_period = htobs(24);
	cp.min_period = htobs(16);
	cp.length  = 0x08;
	cp.num_rsp = 0x00;

	memset(&rq, 0, sizeof(rq));
	rq.ogf    = OGF_LINK_CTL;
	rq.ocf    = OCF_PERIODIC_INQUIRY;
	rq.cparam = &cp;
	rq.clen   = PERIODIC_INQUIRY_CP_SIZE;
	rq.rparam = &status;
	rq.rlen   = sizeof(status);
	rq.event  = EVT_CMD_COMPLETE;

	if (hci_send_req(dd, &rq, 1000) < 0) {
		int err = errno;
		error("Unable to start periodic inquiry: %s (%d)",
				strerror(errno), errno);
		hci_close_dev(dd);
		return error_failed(conn, msg, err);
	}

	if (status) {
		error("HCI_Periodic_Inquiry_Mode failed with status 0x%02x",
				status);
		hci_close_dev(dd);
		return error_failed(conn, msg, bt_error(status));
	}

	adapter->pdiscov_requestor = strdup(dbus_message_get_sender(msg));

	if (adapter->pdiscov_resolve_names)
		adapter->discov_type = PERIODIC_INQUIRY | RESOLVE_NAME;
	else
		adapter->discov_type = PERIODIC_INQUIRY;

	reply = dbus_message_new_method_return(msg);

	hci_close_dev(dd);

	/* track the request owner to cancel it automatically if the owner
	 * exits */
	name_listener_add(conn, dbus_message_get_sender(msg),
				(name_cb_t) periodic_discover_req_exit,
				adapter);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_stop_periodic(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	struct adapter *adapter = data;
	int err;

	if (!adapter->up)
		return error_not_ready(conn, msg);

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return error_invalid_arguments(conn, msg);

	if (!adapter->pdiscov_active)
		return error_not_authorized(conn, msg);

	/* 
	 * Cleanup the discovered devices list and send the cmd to exit
	 * from periodic inquiry mode or cancel remote name request.
	 */
	err = cancel_periodic_discovery(adapter);
	if (err < 0) {
		if (err == -ENODEV)
			return error_no_such_adapter(conn, msg);
		else
			return error_failed(conn, msg, -err);
	}

	reply = dbus_message_new_method_return(msg);
	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_is_periodic(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	struct adapter *adapter = data;
	dbus_bool_t active = adapter->pdiscov_active;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &active,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_set_pdiscov_resolve(DBusConnection *conn,
							DBusMessage *msg,
							void *data)
{
	DBusMessage *reply;
	DBusError err;
	struct adapter *adapter = data;
	dbus_bool_t resolve;

	dbus_error_init(&err);
	dbus_message_get_args(msg, &err,
				DBUS_TYPE_BOOLEAN, &resolve,
				DBUS_TYPE_INVALID);

	if (dbus_error_is_set(&err)) {
		error("Can't extract message arguments:%s", err.message);
		dbus_error_free(&err);
		return error_invalid_arguments(conn, msg);
	}

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	debug("SetPeriodicDiscoveryNameResolving(%s)",
			resolve ? "TRUE" : "FALSE");

	adapter->pdiscov_resolve_names = resolve;

	if (adapter->pdiscov_active) {
		if (resolve)
			adapter->discov_type |= RESOLVE_NAME;
		else
			adapter->discov_type &= ~RESOLVE_NAME;
	}

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_get_pdiscov_resolve(DBusConnection *conn,
							DBusMessage *msg,
							void *data)
{
	DBusMessage *reply;
	struct adapter *adapter = data;
	dbus_bool_t resolve = adapter->pdiscov_resolve_names;

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return error_invalid_arguments(conn, msg);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &resolve,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_discover_devices(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	const char *method;
	inquiry_cp cp;
	evt_cmd_status rp;
	struct hci_request rq;
	struct adapter *adapter = data;
	uint8_t lap[3] = { 0x33, 0x8b, 0x9e };
	int dd;

	if (!adapter->up)
		return error_not_ready(conn, msg);

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return error_invalid_arguments(conn, msg);

	if (adapter->discov_active)
		return error_discover_in_progress(conn, msg);

	pending_remote_name_cancel(adapter);

	if (adapter->bonding)
		return error_bonding_in_progress(conn, msg);

	dd = hci_open_dev(adapter->dev_id);
	if (dd < 0)
		return error_no_such_adapter(conn, msg);

	memset(&cp, 0, sizeof(cp));
	memcpy(&cp.lap, lap, 3);
	cp.length  = 0x08;
	cp.num_rsp = 0x00;

	memset(&rq, 0, sizeof(rq));
	rq.ogf    = OGF_LINK_CTL;
	rq.ocf    = OCF_INQUIRY;
	rq.cparam = &cp;
	rq.clen   = INQUIRY_CP_SIZE;
	rq.rparam = &rp;
	rq.rlen   = EVT_CMD_STATUS_SIZE;
	rq.event  = EVT_CMD_STATUS;

	if (hci_send_req(dd, &rq, 500) < 0) {
		int err = errno;
		error("Unable to start inquiry: %s (%d)",
				strerror(errno), errno);
		hci_close_dev(dd);
		return error_failed(conn, msg, err);
	}

	if (rp.status) {
		error("HCI_Inquiry command failed with status 0x%02x",
				rp.status);
		hci_close_dev(dd);
		return error_failed(conn, msg, bt_error(rp.status));
	}

	method = dbus_message_get_member(msg);
	if (strcmp("DiscoverDevicesWithoutNameResolving", method) == 0)
		adapter->discov_type |= STD_INQUIRY;
	else
		adapter->discov_type |= (STD_INQUIRY | RESOLVE_NAME);

	adapter->discov_requestor = strdup(dbus_message_get_sender(msg));

	reply = dbus_message_new_method_return(msg);

	hci_close_dev(dd);

	/* track the request owner to cancel it automatically if the owner
	 * exits */
	name_listener_add(conn, dbus_message_get_sender(msg),
				(name_cb_t) discover_devices_req_exit, adapter);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult adapter_cancel_discovery(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct adapter *adapter = data;
	int err;

	if (!adapter->up)
		return error_not_ready(conn, msg);

	if (!dbus_message_has_signature(msg, DBUS_TYPE_INVALID_AS_STRING))
		return error_invalid_arguments(conn, msg);

	/* is there discover pending? or discovery cancel was requested
	 * previously */
	if (!adapter->discov_active || adapter->discovery_cancel)
		return error_not_authorized(conn, msg);

	/* only the discover requestor can cancel the inquiry process */
	if (!adapter->discov_requestor ||
			strcmp(adapter->discov_requestor, dbus_message_get_sender(msg)))
		return error_not_authorized(conn, msg);

	/* 
	 * Cleanup the discovered devices list and send the cmd
	 * to cancel inquiry or cancel remote name request
	 */
	err = cancel_discovery(adapter);
	if (err < 0) {
		if (err == -ENODEV)
			return error_no_such_adapter(conn, msg);
		else
			return error_failed(conn, msg, -err);
	}

	/* Reply before send DiscoveryCompleted */
	adapter->discovery_cancel = dbus_message_ref(msg);

	return DBUS_HANDLER_RESULT_HANDLED;
}

const char *major_class_str(uint32_t class)
{
	uint8_t index = (class >> 8) & 0x1F;

	if (index > 8)
		return major_cls[9]; /* uncategorized */

	return major_cls[index];
}

const char *minor_class_str(uint32_t class)
{
	uint8_t major_index = (class >> 8) & 0x1F;
	uint8_t minor_index;

	switch (major_index) {
	case 1: /* computer */
		minor_index = (class >> 2) & 0x3F;
		return computer_minor_cls[minor_index];
	case 2: /* phone */
		minor_index = (class >> 2) & 0x3F;
		return phone_minor_cls[minor_index];
	case 3: /* access point */
		minor_index = (class >> 5) & 0x07;
		return access_point_minor_cls[minor_index];
	case 4: /* audio/video */
		minor_index = (class >> 2) & 0x3F;
		return audio_video_minor_cls[minor_index];
	case 5: /* peripheral */
		minor_index = (class >> 6) & 0x03;
		return peripheral_minor_cls[minor_index];
	case 6: /* imaging */
		{
			uint8_t shift_minor = 0;

			minor_index = (class >> 4) & 0x0F;
			while (shift_minor < (sizeof(imaging_minor_cls) / sizeof(*imaging_minor_cls))) {
				if (((minor_index >> shift_minor) & 0x01) == 0x01)
					return imaging_minor_cls[shift_minor];
				shift_minor++;
			}
		}
		break;
	case 7: /* wearable */
		minor_index = (class >> 2) & 0x3F;
		return wearable_minor_cls[minor_index];
	case 8: /* toy */
		minor_index = (class >> 2) & 0x3F;
		return toy_minor_cls[minor_index];
	}

	return "";
}

struct slist *service_classes_str(uint32_t class)
{
	uint8_t services = class >> 16;
	struct slist *l = NULL;
	int i;

	for (i = 0; i < (sizeof(service_cls) / sizeof(*service_cls)); i++) {
		if (!(services & (1 << i)))
			continue;

		l = slist_append(l, (void *) service_cls[i]);
	}

	return l;
}

static struct service_data dev_services[] = {
	{ "GetAddress",				adapter_get_address		},
	{ "GetVersion",				adapter_get_version		},
	{ "GetRevision",			adapter_get_revision		},
	{ "GetManufacturer",			adapter_get_manufacturer	},
	{ "GetCompany",				adapter_get_company		},
	{ "GetMode",				adapter_get_mode		},
	{ "SetMode",				adapter_set_mode		},
	{ "GetDiscoverableTimeout",		adapter_get_discoverable_to	},
	{ "SetDiscoverableTimeout",		adapter_set_discoverable_to	},
	{ "IsConnectable",			adapter_is_connectable		},
	{ "IsDiscoverable",			adapter_is_discoverable		},
	{ "IsConnected",			adapter_is_connected		},
	{ "ListConnections",			adapter_list_connections	},
	{ "GetMajorClass",			adapter_get_major_class		},
	{ "ListAvailableMinorClasses",		adapter_list_minor_classes	},
	{ "GetMinorClass",			adapter_get_minor_class		},
	{ "SetMinorClass",			adapter_set_minor_class		},
	{ "GetServiceClasses",			adapter_get_service_classes	},
	{ "GetName",				adapter_get_name		},
	{ "SetName",				adapter_set_name		},
	
	{ "GetRemoteServiceRecord",		adapter_get_remote_svc		},
	{ "GetRemoteServiceHandles",		adapter_get_remote_svc_handles	},

	{ "GetRemoteVersion",			adapter_get_remote_version	},
	{ "GetRemoteRevision",			adapter_get_remote_revision	},
	{ "GetRemoteManufacturer",		adapter_get_remote_manufacturer	},
	{ "GetRemoteCompany",			adapter_get_remote_company	},
	{ "GetRemoteMajorClass",		adapter_get_remote_major_class	},
	{ "GetRemoteMinorClass",		adapter_get_remote_minor_class	},
	{ "GetRemoteServiceClasses",		adapter_get_remote_service_cls	},
	{ "GetRemoteClass",			adapter_get_remote_class	},
	{ "GetRemoteName",			adapter_get_remote_name		},
	{ "GetRemoteAlias",			adapter_get_remote_alias	},
	{ "SetRemoteAlias",			adapter_set_remote_alias	},
	{ "ClearRemoteAlias",			adapter_clear_remote_alias	},

	{ "LastSeen",				adapter_last_seen		},
	{ "LastUsed",				adapter_last_used		},

	{ "DisconnectRemoteDevice",		adapter_dc_remote_device	},

	{ "CreateBonding",			adapter_create_bonding		},
	{ "CancelBondingProcess",		adapter_cancel_bonding		},
	{ "RemoveBonding",			adapter_remove_bonding		},
	{ "HasBonding",				adapter_has_bonding		},
	{ "ListBondings",			adapter_list_bondings		},
	{ "GetPinCodeLength",			adapter_get_pin_code_length	},
	{ "GetEncryptionKeySize",		adapter_get_encryption_key_size	},

	{ "StartPeriodicDiscovery",		adapter_start_periodic		},
	{ "StopPeriodicDiscovery",		adapter_stop_periodic		},
	{ "IsPeriodicDiscovery",		adapter_is_periodic		},
	{ "SetPeriodicDiscoveryNameResolving",	adapter_set_pdiscov_resolve	},
	{ "GetPeriodicDiscoveryNameResolving",	adapter_get_pdiscov_resolve	},

	{ "DiscoverDevices",			adapter_discover_devices	},
	{ "DiscoverDevicesWithoutNameResolving",	adapter_discover_devices	},
	{ "CancelDiscovery",			adapter_cancel_discovery	},

	{ NULL, NULL }
};

DBusHandlerResult msg_func_device(DBusConnection *conn, DBusMessage *msg, void *data)
{
	const char *iface, *name;

	iface = dbus_message_get_interface(msg);
	name = dbus_message_get_member(msg);

	if (!strcmp(DBUS_INTERFACE_INTROSPECTABLE, iface) &&
					!strcmp("Introspect", name)) {
		return simple_introspect(conn, msg, data);
	} else if (!strcmp(ADAPTER_INTERFACE, iface)) {
		service_handler_func_t handler;

		handler = find_service_handler(dev_services, msg);

		if (handler)
			return handler(conn, msg, data);
		else
			return error_unknown_method(conn, msg);
	} else if (!strcmp(SECURITY_INTERFACE, iface))
		return handle_security_method(conn, msg, data);
	else if (!strcmp(TEST_INTERFACE, iface))
		return handle_test_method(conn, msg, data);
	else if (!strcmp(RFCOMM_INTERFACE, iface))
		return handle_rfcomm_method(conn, msg, data);
	else if (!strcmp(SDP_INTERFACE, iface))
		return handle_sdp_method(conn, msg, data);
	else
		return error_unknown_method(conn, msg);
}

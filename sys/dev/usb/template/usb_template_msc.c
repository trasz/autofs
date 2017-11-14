/* $FreeBSD$ */
/*-
 * Copyright (c) 2008 Hans Petter Selasky <hselasky@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * This file contains the USB templates for an USB Mass Storage Device.
 */

#ifdef USB_GLOBAL_INCLUDE_FILE
#include USB_GLOBAL_INCLUDE_FILE
#else
#include <sys/stdint.h>
#include <sys/stddef.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sysctl.h>
#include <sys/sx.h>
#include <sys/unistd.h>
#include <sys/callout.h>
#include <sys/malloc.h>
#include <sys/priv.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usb_core.h>
#include <dev/usb/usb_util.h>

#include <dev/usb/template/usb_template.h>
#endif			/* USB_GLOBAL_INCLUDE_FILE */

SYSCTL_NODE(_hw_usb, OID_AUTO, template_msc, CTLFLAG_RW, 0,
    "USB Mass Storage device side template");

enum {
	STRING_LANG_INDEX,
	STRING_MSC_DATA_INDEX,
	STRING_MSC_CONFIG_INDEX,
	STRING_MSC_VENDOR_INDEX,
	STRING_MSC_PRODUCT_INDEX,
	STRING_MSC_SERIAL_INDEX,
	STRING_MSC_MAX,
};

#define	MSC_DEFAULT_INTERFACE		"USB Mass Storage Interface"
#define	MSC_DEFAULT_CONFIG		"Default Config"
#define	MSC_DEFAULT_MANUFACTURER	"FreeBSD foundation"
#define	MSC_DEFAULT_PRODUCT		"USB Memory Stick"
#define	MSC_DEFAULT_SERIAL_NUMBER	"March 2008"

static struct usb_string_descriptor msc_interface;
static struct usb_string_descriptor msc_configuration;
static struct usb_string_descriptor msc_manufacturer;
static struct usb_string_descriptor msc_product;
static struct usb_string_descriptor msc_serial_number;

/* prototypes */

static usb_temp_get_string_desc_t msc_get_string_desc;

static const struct usb_temp_packet_size bulk_mps = {
	.mps[USB_SPEED_FULL] = 64,
	.mps[USB_SPEED_HIGH] = 512,
};

static const struct usb_temp_endpoint_desc bulk_in_ep = {
	.pPacketSize = &bulk_mps,
#ifdef USB_HIP_IN_EP_0
	.bEndpointAddress = USB_HIP_IN_EP_0,
#else
	.bEndpointAddress = UE_DIR_IN,
#endif
	.bmAttributes = UE_BULK,
};

static const struct usb_temp_endpoint_desc bulk_out_ep = {
	.pPacketSize = &bulk_mps,
#ifdef USB_HIP_OUT_EP_0
	.bEndpointAddress = USB_HIP_OUT_EP_0,
#else
	.bEndpointAddress = UE_DIR_OUT,
#endif
	.bmAttributes = UE_BULK,
};

static const struct usb_temp_endpoint_desc *msc_data_endpoints[] = {
	&bulk_in_ep,
	&bulk_out_ep,
	NULL,
};

static const struct usb_temp_interface_desc msc_data_interface = {
	.ppEndpoints = msc_data_endpoints,
	.bInterfaceClass = UICLASS_MASS,
	.bInterfaceSubClass = UISUBCLASS_SCSI,
	.bInterfaceProtocol = UIPROTO_MASS_BBB,
	.iInterface = STRING_MSC_DATA_INDEX,
};

static const struct usb_temp_interface_desc *msc_interfaces[] = {
	&msc_data_interface,
	NULL,
};

static const struct usb_temp_config_desc msc_config_desc = {
	.ppIfaceDesc = msc_interfaces,
	.bmAttributes = UC_BUS_POWERED,
	.bMaxPower = 25,		/* 50 mA */
	.iConfiguration = STRING_MSC_CONFIG_INDEX,
};

static const struct usb_temp_config_desc *msc_configs[] = {
	&msc_config_desc,
	NULL,
};

struct usb_temp_device_desc usb_template_msc = {
	.getStringDesc = &msc_get_string_desc,
	.ppConfigDesc = msc_configs,
	.idVendor = USB_TEMPLATE_VENDOR,
	.idProduct = 0x0012,
	.bcdDevice = 0x0100,
	.bDeviceClass = UDCLASS_COMM,
	.bDeviceSubClass = 0,
	.bDeviceProtocol = 0,
	.iManufacturer = STRING_MSC_VENDOR_INDEX,
	.iProduct = STRING_MSC_PRODUCT_INDEX,
	.iSerialNumber = STRING_MSC_SERIAL_INDEX,
};

static void
usb_decode_str_desc(struct usb_string_descriptor *sd, char *buf, size_t buflen)
{
	int i;

	for (i = 0; i < buflen - 1 && i < sd->bLength / 2; i++)
		buf[i] = UGETW(sd->bString[i]);

	i++;
	buf[i] = '\0';
}

static int
sysctl_msc_string(SYSCTL_HANDLER_ARGS)
{
	char buf[128];
	struct usb_string_descriptor *sd = arg1;
	size_t len, sdlen = arg2;
	int error;

	usb_decode_str_desc(sd, buf, sizeof(buf));

	error = sysctl_handle_string(oidp, buf, sizeof(buf), req);
	if (error != 0 || req->newptr == NULL)
		return (error);

	len = usb_make_str_desc(sd, sdlen, buf);
	if (len == 0)
		return (EINVAL);

	return (0);
}

SYSCTL_U16(_hw_usb_template_msc, OID_AUTO, vendor_id, CTLFLAG_RWTUN,
    &usb_template_msc.idVendor, 1, "Vendor identifier");
SYSCTL_U16(_hw_usb_template_msc, OID_AUTO, product_id, CTLFLAG_RWTUN,
    &usb_template_msc.idProduct, 1, "Product identifier");
SYSCTL_PROC(_hw_usb_template_msc, OID_AUTO, interface,
    CTLTYPE_STRING | CTLFLAG_RWTUN | CTLFLAG_MPSAFE,
    &msc_interface, sizeof(msc_interface), sysctl_msc_string,
    "A", "Interface string");
SYSCTL_PROC(_hw_usb_template_msc, OID_AUTO, configuration,
    CTLTYPE_STRING | CTLFLAG_RWTUN | CTLFLAG_MPSAFE,
    &msc_configuration, sizeof(msc_configuration), sysctl_msc_string,
    "A", "Configuration string");
SYSCTL_PROC(_hw_usb_template_msc, OID_AUTO, manufacturer,
    CTLTYPE_STRING | CTLFLAG_RWTUN | CTLFLAG_MPSAFE,
    &msc_manufacturer, sizeof(msc_manufacturer), sysctl_msc_string,
    "A", "Manufacturer string");
SYSCTL_PROC(_hw_usb_template_msc, OID_AUTO, product,
    CTLTYPE_STRING | CTLFLAG_RWTUN | CTLFLAG_MPSAFE,
    &msc_product, sizeof(msc_product), sysctl_msc_string,
    "A", "Product string");
SYSCTL_PROC(_hw_usb_template_msc, OID_AUTO, serial_number,
    CTLTYPE_STRING | CTLFLAG_RWTUN | CTLFLAG_MPSAFE,
    &msc_serial_number, sizeof(msc_serial_number), sysctl_msc_string,
    "A", "Serial number string");


/*------------------------------------------------------------------------*
 *	msc_get_string_desc
 *
 * Return values:
 * NULL: Failure. No such string.
 * Else: Success. Pointer to string descriptor is returned.
 *------------------------------------------------------------------------*/
static const void *
msc_get_string_desc(uint16_t lang_id, uint8_t string_index)
{
	static const void *ptr[STRING_MSC_MAX] = {
		[STRING_LANG_INDEX] = &usb_string_lang_en,
		[STRING_MSC_DATA_INDEX] = &msc_interface,
		[STRING_MSC_CONFIG_INDEX] = &msc_configuration,
		[STRING_MSC_VENDOR_INDEX] = &msc_manufacturer,
		[STRING_MSC_PRODUCT_INDEX] = &msc_product,
		[STRING_MSC_SERIAL_INDEX] = &msc_serial_number,
	};

	if (string_index == 0) {
		return (&usb_string_lang_en);
	}
	if (lang_id != 0x0409) {
		return (NULL);
	}
	if (string_index < STRING_MSC_MAX) {
		return (ptr[string_index]);
	}
	return (NULL);
}

static void
msc_init(void *arg __unused)
{

	usb_make_str_desc(&msc_interface, sizeof(msc_interface),
	    MSC_DEFAULT_INTERFACE);
	usb_make_str_desc(&msc_configuration, sizeof(msc_configuration),
	    MSC_DEFAULT_CONFIG);
	usb_make_str_desc(&msc_manufacturer, sizeof(msc_manufacturer),
	    MSC_DEFAULT_MANUFACTURER);
	usb_make_str_desc(&msc_product, sizeof(msc_product),
	    MSC_DEFAULT_PRODUCT);
	usb_make_str_desc(&msc_serial_number, sizeof(msc_serial_number),
	    MSC_DEFAULT_SERIAL_NUMBER);
}

SYSINIT(msc_init, SI_SUB_LOCK, SI_ORDER_FIRST, msc_init, NULL);

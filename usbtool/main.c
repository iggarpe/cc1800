//==============================================================================
//
//	USB boot tool for ChinaChip CC1800 system-on-chip.
//
//	Copyright (C) 2011 Ignacio Garcia Perez <iggarpe@gmail.com>
//
//	This program is free software; you can redistribute it and/or modify
//	it under the terms of the GNU General Public License version 2 as
//	published by the Free Software Foundation.
//

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>

#include <usb.h>

#define CC1800_VENDOR_ID	0x2009
#define CC1800_PRODUCT_ID	0x1218

//==============================================================================

struct usb_device *cc1800_find (void) {
	int r;
	struct usb_bus *busses, *bus;
	struct usb_device *dev;

	usb_init();
	r = usb_find_busses(); if (r < 0) return NULL;
	r = usb_find_devices(); if (r < 0) return NULL;
	busses = usb_get_busses();

	for (bus = busses; bus != NULL; bus = bus->next) {
		for (dev = bus->devices; dev != NULL; dev = dev->next) {
			if (dev->descriptor.idVendor == CC1800_VENDOR_ID &&
				dev->descriptor.idProduct == CC1800_PRODUCT_ID)
			{
				return dev;	
			}
		}
	}

	return NULL;
}

//==============================================================================

#define CC1800_REQ_GET_CPU_INFO		0x00
#define CC1800_REQ_SET_ADDRESS		0x01
#define CC1800_REQ_SET_LENGTH		0x02
#define CC1800_REQ_GET_STATUS		0x03
#define CC1800_REQ_EXECUTE			0x04

#define TIMEOUT	5000

//==============================================================================
//
//	CC1800 USB boot mode requests
//

//
//	Get CPU information as a string.
//

int cc1800_req_get_cpu_info (struct usb_dev_handle *handle, char *str) {
	return usb_control_msg(
		handle,
		USB_ENDPOINT_IN | USB_TYPE_VENDOR,
		CC1800_REQ_GET_CPU_INFO,
		0,
		0,
		str,
		8,
		TIMEOUT
	);
}

//
//	Set read/write address.
//

int cc1800_req_set_address (struct usb_dev_handle *handle, unsigned long addr) {
	return usb_control_msg(
		handle,
		USB_ENDPOINT_OUT | USB_TYPE_VENDOR,
		CC1800_REQ_SET_ADDRESS,
		(addr >> 16) & 0xFFFF,
		(addr >>  0) & 0xFFFF,
		NULL,
		0,
		TIMEOUT
	);
}

//
//	Set read/write length. 
//

int cc1800_req_set_length (struct usb_dev_handle *handle, unsigned long len, int wr) {
	if (wr) len |= 0x80000000; else len &= ~0x80000000;
	return usb_control_msg(
		handle,
		USB_ENDPOINT_OUT | USB_TYPE_VENDOR,
		CC1800_REQ_SET_LENGTH,
		(len >> 16) & 0xFFFF,
		(len >>  0) & 0xFFFF,
		NULL,
		0,
		TIMEOUT
	);
}

//
//	Presumably get status. Dunno what this does actually: the unbricking tool never
//	uses this function, and when used seems to launch the NAND flash boot... or
//	something.
//
//	Reverse engineering the rom.bin code should allow to find out more, but
//	I'm too lazy to do that as of now (since anyway I'm not using this function).
//

int cc1800_req_get_status (struct usb_dev_handle *handle, char *stat) {
	return usb_control_msg(
		handle,
		USB_ENDPOINT_IN | USB_TYPE_VENDOR,
		CC1800_REQ_GET_STATUS,
		0,
		0,
		stat,
		1,
		TIMEOUT
	);
}

//
//	Execute at last set read/write address.
//

int cc1800_req_execute (struct usb_dev_handle *handle) {
	return usb_control_msg(
		handle,
		USB_ENDPOINT_OUT | USB_TYPE_VENDOR,
		CC1800_REQ_EXECUTE,
		0,
		0,
		NULL,
		0,
		TIMEOUT
	);
}

//
//	These are convenience composite functions.
//

//
//	CC1800 data upload: set address, set length and do a bulk transter to end point 1.
//

int cc1800_upload (struct usb_dev_handle *handle, const char *data, int length, unsigned long address) {
	int r;
	r = cc1800_req_set_address(handle, address); if (r < 0) return r;
	r = cc1800_req_set_length(handle, length, 1); if (r < 0) return r;
	return usb_bulk_write(handle, 1, data, length, TIMEOUT);
}

//
//	CC1800 data download: set address, set length and do a bulk transfer from end point 1.
//

int cc1800_download (struct usb_dev_handle *handle, char *data, int length, unsigned long address) {
	int r;
	r = cc1800_req_set_address(handle, address); if (r < 0) return r;
	r = cc1800_req_set_length(handle, length, 0); if (r < 0) return r;
	return usb_bulk_read(handle, 1, data, length, TIMEOUT);
}

//
//	Upload, verify (download and compare) and execute.
//

int cc1800_execute (struct usb_dev_handle *handle, const char *data, int length, unsigned long address) {
	int r;
	char *check = alloca(length);			// Use the stack, so we need not care to free
	if (check == NULL) return -ENOMEM;
	r = cc1800_upload(handle, data, length, address);
	if (r < 0) return r;
	if (r < length) return -EIO;
	r = cc1800_download(handle, check, length, address);
	if (r < 0) return r;
	if (r < length) return -EIO;
	if (memcmp(data, check, length)) return -EIO;
	return cc1800_req_execute(handle);
}

//==============================================================================
//
//	Scan a 32 bit address, checking for the "0x" hexadecimal format prefix.
//

static int scan_ulong (const char *str, unsigned long *addr) {

	if (str[0] == '0' && toupper(str[1]) == 'X') {
		if (sscanf(str + 2, "%lx", addr)) return 0;
	} else {
		if (!sscanf(str, "%lu", addr)) return 0;
	}

	fprintf(stderr, "ERROR: bad value '%s'\n", str);
	return -1;
}

//
//	Load a file into memory. Memory is malloc'ed, so caller must later free it.
//

static int load_file (const char *file, char **data, unsigned long *len) {

	FILE *f;

	f = fopen(file, "rb");
	if (f == NULL) {
		fprintf(stderr, "ERROR: cannot open file '%s'\n", file);
		return -1;
	}

	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (*len < 0) {
		fprintf(stderr, "ERROR: cannot get file size for '%s'\n", file);
		fclose(f);
		return -1;
	}

	*data = (char *)malloc(*len);
	
	if (*data == NULL) {
		fprintf(stderr, "ERROR: cannot allocate memory for file '%s'\n", file);
		fclose(f);
		return -1;
	}

	if (!fread(*data, *len, 1, f)) {
		fprintf(stderr, "ERROR: cannot read file '%s'\n", file);
		free(*data);
		fclose(f);
		return -1;
	}

	printf("Loaded file '%s' (%lu bytes)\n", file, *len);
	fclose(f);
	return 0;
}

static int save_file (const char *file, const char *data, unsigned long len) {

	FILE *f;

	f = fopen(file, "wb");
	if (f == NULL) {
		fprintf(stderr, "ERROR: cannot create file '%s'\n", file);
		return -1;
	}

	if (!fwrite(data, len, 1, f)) {
		fprintf(stderr, "ERROR: cannot write file '%s'\n", file);
		fclose(f);
		return -1;
	}

	return 0;
}

//
//	This is the actual command line interpreter.
//

int cc1800_fiddle (struct usb_dev_handle *handle, int argc, const char **argv) {

	int i, r, cpu = 0; char s [256], *data, *verify;
	unsigned long addr, len;

	for (i = 1; i < argc; i++) {

		memset(s, 0, sizeof(s));
		r = cc1800_req_get_cpu_info(handle, s);	
		if (r < 0) {
			fprintf(stderr, "ERROR: cannot get CPU info\n");
			return r;
		}

		// Show CPU info only the first time, but we execute this command
		// each time, just to make sure the it is listening

		if (!cpu) { cpu = 1; printf("CPU info: %s\n", s); }

		//
		//	WRITE command, usage: write <addr> file
		//

		if (!strcmp(argv[i], "write")) {

			if ((argc - i) < 3) {
				fprintf(stderr, "ERROR: write command requires two arguments (address and file name)\n");
				return -1;
			}

			r = scan_ulong(argv[++i], &addr); if (r < 0) return r;
			r = load_file(argv[++i], &data, &len); if (r < 0) return r;

			printf("Uploading data to address 0x%08lX\n", addr);
			r = cc1800_upload(handle, data, len, addr);
			if (r < 0) {
				fprintf(stderr, "ERROR: CC1800 upload failed\n");
				free(data);
				return r;
			}

			verify = (char *)malloc(len);
			if (verify == NULL) {
				fprintf(stderr, "ERROR: cannot allocate memory\n");
				free(data);
				return r;
			}

			printf("Downloading data for verification\n");
			r = cc1800_download(handle, verify, len, addr);
			if (r < 0) {
				fprintf(stderr, "ERROR: CC1800 download failed\n");
				free(verify);
				free(data);
				return r;
			}

			r = memcmp(data, verify, len);

			free(verify);
			free(data);

			if (r) printf("WARNING: data mismatch\n");
		}

		//
		//	READ command, usage: read <addr> <len> <file>
		//

		else if (!strcmp(argv[i], "read")) {

			if ((argc - i) < 4) {
				fprintf(stderr, "ERROR: read command requires two arguments (address, length and a file name)\n");
				return -1;
			}

			r = scan_ulong(argv[++i], &addr); if (r < 0) return r;
			r = scan_ulong(argv[++i], &len); if (r < 0) return r;

			data = (char *)malloc(len);
			if (data == NULL) {
				fprintf(stderr, "ERROR: cannot allocate memory\n");
				return -1;
			}

			printf("Downloading data from address 0x%08lX\n", addr);
			r = cc1800_download(handle, data, len, addr);
			if (r < 0) {
				fprintf(stderr, "ERROR: CC1800 download failed\n");
				free(data);
				return r;
			}

			r = save_file(argv[++i], data, len);

			free(data);

			if (r < 0) return r;
		}

		//
		//	EXEC commant
		//

		else if (!strcmp(argv[i], "exec")) {

			printf("Executing at last address\n");
			r = cc1800_req_execute(handle);
			if (r < 0) {
				fprintf(stderr, "ERROR: CC1800 execute failed\n");
				return r;
			}
		}

		else {
			fprintf(stderr, "ERROR: unknown command '%s'\n", argv[i]);
			return -1;
		}
	}

	return 0;
}

//==============================================================================

static const char *help =

"Use any number of consecutive commands as arguments:\n"
"    write <address> <file>\n"
"    read <address> <length> <file>\n"
"    exec\n"
"\n";

int main (int argc, const char **argv) {

	int r = 0;
	struct usb_device *dev;
	struct usb_dev_handle *handle;

	printf("CC1800 usbtool v1.0.0 by Ignacio Garcia Perez <iggarpe@gmail.com>\n");

	if (argc < 2) {
		fputs(help, stderr);
		return 1;
	}

	dev = cc1800_find();
	if (dev == NULL) {
		fprintf(stderr, "ERROR: cannot find CC1800 device\n");
		return 1;
	}

	printf("Found device %s at bus %s\n", dev->filename, dev->bus->dirname);

	handle = usb_open(dev);
	if (handle == NULL) {
		fprintf(stderr, "ERROR: cannot open device (%s)\n", strerror(errno));
		return 1;
	}

	r = usb_set_configuration(handle, 1);
	if (r < 0)
		fprintf(stderr, "ERROR: cannot set configuration\n");

	else {
		r = usb_claim_interface(handle, 0);
		if (r < 0)
			fprintf(stderr, "ERROR: cannot claim interface\n");

		else r = cc1800_fiddle(handle, argc, argv);
	}

	usb_close(handle);
	return r;
}

//==============================================================================


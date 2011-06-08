/*
 * Copyright (c) 2009, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name of Google, Inc. nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <diskconfig/diskconfig.h>

#include "aboot.h"
#include "util.h"
#include "debug.h"
#include "droidboot.h"
#include "fastboot.h"

/* libdiskconfig data structure representing the intended layout of the
 * internal disk, as read from /etc/disk_layout.conf */
struct disk_info *disk_info;

pthread_mutex_t debug_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *scratch;

/* Not bothering with concurrency control as this is just a flag
 * that gets cleared */
static int autoboot_enabled = USE_AUTOBOOT;

static void *autoboot_thread(void *arg)
{
	unsigned int sleep_time = *((unsigned int *)arg);

	if (!autoboot_enabled)
		return NULL;

	for (; sleep_time; sleep_time--) {
		dprintf(ALWAYS, "Automatic boot in %d seconds.\n", sleep_time);
		sleep(1);
		if (!autoboot_enabled)
			return NULL;
	}

	start_default_kernel();

	/* can't get here */
	return NULL;
}

static void open_event_node(char *devname, fd_set *rfds, int *max_fd)
{
	int fd;

	fd = open(devname, O_RDONLY);
	if (fd < 0) {
		dprintf(INFO, "Unable to open %s. fd=%d, errno=%d\n",
				devname, fd, errno);
		return;
	}
	dprintf(INFO, "Opened %s. fd=%d\n", devname, fd);

	*max_fd = (fd > *max_fd ? fd : *max_fd);
	FD_SET(fd, rfds);
}

static void *input_listener_thread(void *arg)
{
	int select_ret;
	int i;
	int max_fd = -1;
	fd_set fds, rfds;
	FD_ZERO(&rfds);
	struct dirent *entry;
	DIR *inputdir;
	struct stat statbuf;

	dprintf(SPEW, "begin input listener thread\n");

	/* Try to open all the character device nodes in
	 * /dev/input */
	inputdir = opendir("/dev/input/");
	if (!inputdir) {
		dperror("opendir");
		goto out;
	}
	while (1) {
		char filename[128];
		entry = readdir(inputdir);
		if (!entry)
			break;
		if (!strcmp(".", entry->d_name) ||
				!strcmp("..", entry->d_name))
			continue;
		snprintf(filename, sizeof(filename), "/dev/input/%s",
				entry->d_name);
		if (stat(filename, &statbuf) < 0) {
			dperror("stat");
			die();
		}
		if (!S_ISCHR(statbuf.st_mode))
			continue;
		open_event_node(filename, &rfds, &max_fd);
	}

	if (max_fd < 0) {
		dprintf(CRITICAL, "Unable to open any input device.\n");
		goto out;
	}

	while (1) {
		struct input_event event;

		fds = rfds;
		select_ret = select(max_fd+1, &fds, NULL, NULL, NULL);
		dprintf(SPEW, "select returns %d (errno=%d)\n",
				select_ret, errno);

		for (i = 0; i <= max_fd; i++) {
			int deb;

			if (!FD_ISSET(i, &fds))
				continue;

			/* read the event */
			deb = read(i, &event, sizeof(event));
			if (deb != sizeof(event)) {
				dprintf(INFO, "Unable to read event from fd=%d,"
						" deb=%d, errno=%d\n",
						i, deb, errno);
				continue;
			}
			dprintf(SPEW, "read from fd=%d. Event type: %x, "
					"code: %x, value: %x\n",
					i, event.type, event.code,
					event.value);
			switch (event.type) {
			case EV_KEY:
				switch (event.code) {
				case KEY_DOT:
					/* This is very likely from the MRST
					 * keypad on a device (such as AAVA)
					 * that does not have a keypad.
					 * Ignore the event. */
					continue;
				default:
					disable_autoboot();
					goto out;
				}
				break;
			case EV_ABS:
			case EV_REL:
				/* Mouse or touchscreen */
				disable_autoboot();
				goto out;
			default:
				continue;
			}
		}
	}
out:
	dprintf(SPEW, "exit input listener thread\n");

	return NULL;
}


void disable_autoboot(void)
{
	if (autoboot_enabled) {
		autoboot_enabled = 0;
		dprintf(INFO, "Autoboot disabled.\n");
	}
}


void start_default_kernel(void)
{
	struct part_info *ptn;
	char *mountpoint;
	char *kernel_path;
	char *cmdline_path;
	char *ramdisk_path;

	ptn = find_part(disk_info, "boot");

	mountpoint = mount_partition(ptn);
	if (!mountpoint) {
		dprintf(CRITICAL, "Can't mount boot partition!\n");
		return;
	}

	if (asprintf(&kernel_path, "%s/kernel", mountpoint) < 0) {
		dperror("asprintf");
		die();
	}

	if (asprintf(&ramdisk_path, "%s/ramdisk.img", mountpoint) < 0) {
		dperror("asprintf");
		die();
	}

	if (asprintf(&cmdline_path, "%s/cmdline", mountpoint) < 0) {
		dperror("asprintf");
		die();
	}

	if (kexec_linux(kernel_path, ramdisk_path, cmdline_path))
		die();
	/* Can't get here */
}


int main(int argc, char **argv)
{
	char *config_location;
	pthread_t thr;
	unsigned int autoboot_delay_secs = AUTOBOOT_DELAY_SECS;

	dprintf(INFO, "DROIDBOOT %s START\n", DROIDBOOT_VERSION);
	if (argc > 1)
		config_location = argv[1];
	else
		config_location = DISK_CONFIG_LOCATION;

	dprintf(INFO, "Reading disk layout from %s\n", config_location);
	disk_info = load_diskconfig(config_location, NULL);
	dump_disk_config(disk_info);

	aboot_register_commands();

	if (pthread_create(&thr, NULL, autoboot_thread,
				&autoboot_delay_secs)) {
		dperror("pthread_create");
		die();
	}
	if (pthread_create(&thr, NULL, input_listener_thread, NULL)) {
		dperror("pthread_create");
		die();
	}

	scratch = malloc(SCRATCH_SIZE);
	if (scratch == NULL) {
		dprintf(CRITICAL,
			"scratch malloc of %u failed in fastboot."
			" Unable to continue.\n\n", SCRATCH_SIZE);
		die();
	}

	dprintf(ALWAYS, "Listening for the fastboot protocol "
			"on the USB OTG.\n");

	fastboot_init(scratch, SCRATCH_SIZE);

	/* Shouldn't get here */
	exit(1);
}
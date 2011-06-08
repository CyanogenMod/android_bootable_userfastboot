/*
 * Copyright (C) 2011 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <getopt.h>
#include <unistd.h>

#include "update_osip.h"
#include "manage_device.h"
#include "debug.h"

#define BACKUP_LOC 0xE0
#define OSIP_PREAMBLE 0x20
#define OSIP_SIG 0x24534f24	/* $OS$ */

#define FILE_EXT ".bin"
#define ANDROID_OS 0
#define POS 1
#define COS 3

#define OSII_TOTAL 7
#define DUMP_OSIP 1
#define NOT_DUMP 0
#define R_BCK 1
#define R_START 0

#ifdef __ANDROID__
#define MMC_DEV_POS "/dev/block/mmcblk0"
#else
#define MMC_DEV_POS "/dev/mmcblk0"
#endif

#define MMC_PAGES_PER_BLOCK 1
#define MMC_PAGE_SIZE "/sys/devices/pci0000:00/0000:00:01.0/mmc_host/mmc0/mmc0:0001/erase_size"
#define KBYTES 1024
#define STITCHED_IMAGE_PAGE_SIZE 512
#define STITCHED_IMAGE_BLOCK_SIZE 512


static int read_OSIP_loc(struct OSIP_header *osip, int location, int dump);

static int get_page_size(void)
{
	int mmc_page_size;
	char buf[16];
	int fd;

	memset((void *)buf, 0, sizeof(buf));
	fd = open(MMC_PAGE_SIZE, O_RDONLY);
	if (fd < 0) {
		dperror("open");
		return -1;
	}
	if (read(fd, buf, 16) < 0) {
		dperror("read");
		close(fd);
		return -1;
	}
	dprintf(INFO, "page size %s\n", buf);
	if (sscanf(buf, "%d", &mmc_page_size) != 1) {
		dperror("sscanf");
		close(fd);
		return -1;
	}
	close(fd);

	return mmc_page_size / KBYTES;
}


static int get_block_size(void)
{
	int mmc_page_size;

	mmc_page_size = get_page_size();

	return mmc_page_size * MMC_PAGES_PER_BLOCK;
}


int write_stitch_image(void *data, size_t size, int update_number)
{
	struct OSIP_header osip;
	//struct OSIP_header bck_osip;
	struct OSII *osii;
	void *blob;
	int block_size = get_block_size();
	int page_size = get_page_size();
	int fd;

	dprintf(INFO, "now into write_stitch_image\n");
	if (block_size < 0) {
		dprintf(CRITICAL, "block size wrong\n");
		return -1;
	}
	if (crack_stitched_image(data, &osii, &blob) < 0) {
		dprintf(CRITICAL, "crack_stitched_image fails\n");
		return -1;
	}
	if ((osii->size_of_os_image * STITCHED_IMAGE_PAGE_SIZE) !=
	    size - STITCHED_IMAGE_BLOCK_SIZE) {
		dprintf(CRITICAL, "data format is not correct! \n");
		return -1;
	}
	if (read_OSIP_loc(&osip, R_START, NOT_DUMP) < 0) {
		dprintf(CRITICAL, "read_OSIP fails\n");
		return -1;
	}

	osip.num_images = 1;
	osii->logical_start_block =
	    osip.desc[update_number].logical_start_block;
	osii->size_of_os_image =
	    (osii->size_of_os_image * STITCHED_IMAGE_PAGE_SIZE) / page_size + 1;

	memcpy(&(osip.desc[update_number]), osii, sizeof(struct OSII));
	dprintf(SPEW, "os_rev_major=0x%x,os_rev_minor=0x%x,ddr_load_address=0x%x\n",
	       osii->os_rev_major, osii->os_rev_minor, osii->ddr_load_address);
	dprintf(SPEW, "entry_point=0x%x,sizeof_osimage=0x%x,attribute=0x%x\n",
	       osii->entery_point, osii->size_of_os_image, osii->attribute);

	write_OSIP(&osip);

	fd = open(MMC_DEV_POS, O_RDWR);
	if (fd < 0) {
		dprintf(CRITICAL, "fail open %s\n", MMC_DEV_POS);
		return -1;
	}
	lseek(fd, osii->logical_start_block * block_size, SEEK_SET);
	if (write(fd, blob, size - STITCHED_IMAGE_BLOCK_SIZE) <
	    (int)(size - STITCHED_IMAGE_BLOCK_SIZE)) {
		dprintf(INFO, "fail write of blob\n");
		close(fd);
		return -1;
	}
	fsync(fd);
	close(fd);

	return 0;
}



static int read_OSIP_loc(struct OSIP_header *osip, int location, int dump)
{
	int fd;

	if (!location)
		dprintf(INFO, "**************into read_OSIP*********************\n");
	else
		printf
		    ("==============into read_OSIP from backup location====\n");
	memset((void *)osip, 0, sizeof(*osip));
	fd = open(MMC_DEV_POS, O_RDONLY);
	if (fd < 0)
		return -1;

	if (location)
		lseek(fd, BACKUP_LOC, SEEK_SET);
	else
		lseek(fd, 0, SEEK_SET);

	if (read(fd, (void *)osip, sizeof(*osip)) < 0) {
		dprintf(INFO, "read of osip failed\n");
		close(fd);
		return -1;
	}
	close(fd);

	if (osip->sig != OSIP_SIG) {
		printf
		    ("Invalid OSIP header detected!\n++++++++++++++++++!\n");
	}

	if ((dump) &&(osip->sig == OSIP_SIG)) {
		dump_osip_header(osip);
		if (location)
			dprintf(INFO, "read of osip  from BACKUP_LOC works\n");
		else
			dprintf(INFO, "read of osip works\n");
	}

	return 1;
}

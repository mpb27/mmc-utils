/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 *
 * Modified to add field firmware update support,
 * those modifications are Copyright (c) 2016 SanDisk Corp.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <assert.h>
#include <linux/fs.h> /* for BLKGETSIZE */

#include "mmc.h"
#include "mmc_cmds.h"
#include "3rdparty/hmac_sha/hmac_sha2.h"

#ifndef MMC_IOC_MULTI_CMD
#error "mmc-utils needs MMC_IOC_MULTI_CMD support (added in kernel v4.4)"
#endif

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#endif

#define WP_BLKS_PER_QUERY 32

#define USER_WP_PERM_PSWD_DIS	0x80
#define USER_WP_CD_PERM_WP_DIS	0x40
#define USER_WP_US_PERM_WP_DIS	0x10
#define USER_WP_US_PWR_WP_DIS	0x08
#define USER_WP_US_PERM_WP_EN	0x04
#define USER_WP_US_PWR_WP_EN	0x01
#define USER_WP_CLEAR (USER_WP_US_PERM_WP_DIS | USER_WP_US_PWR_WP_DIS	\
			| USER_WP_US_PERM_WP_EN | USER_WP_US_PWR_WP_EN)

#define WPTYPE_NONE 0
#define WPTYPE_TEMP 1
#define WPTYPE_PWRON 2
#define WPTYPE_PERM 3

int read_extcsd(int fd, __u8 *ext_csd)
{
	int ret = 0;
	struct mmc_ioc_cmd idata;
	memset(&idata, 0, sizeof(idata));
	memset(ext_csd, 0, sizeof(__u8) * 512);
	idata.write_flag = 0;
	idata.opcode = MMC_SEND_EXT_CSD;
	idata.arg = 0;
	idata.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;
	idata.blksz = 512;
	idata.blocks = 1;
	mmc_ioc_cmd_set_data(idata, ext_csd);

	ret = ioctl(fd, MMC_IOC_CMD, &idata);
	if (ret)
		perror("ioctl");

	return ret;
}

static void fill_switch_cmd(struct mmc_ioc_cmd *cmd, __u8 index, __u8 value)
{
	cmd->opcode = MMC_SWITCH;
	cmd->write_flag = 1;
	cmd->arg = (MMC_SWITCH_MODE_WRITE_BYTE << 24) | (index << 16) |
		   (value << 8) | EXT_CSD_CMD_SET_NORMAL;
	cmd->flags = MMC_RSP_SPI_R1B | MMC_RSP_R1B | MMC_CMD_AC;
}

int write_extcsd_value(int fd, __u8 index, __u8 value, unsigned int timeout_ms)
{
	int ret = 0;
	struct mmc_ioc_cmd idata = {};

	fill_switch_cmd(&idata, index, value);

	/* Kernel will set cmd_timeout_ms if 0 is set */
	idata.cmd_timeout_ms = timeout_ms;

	ret = ioctl(fd, MMC_IOC_CMD, &idata);
	if (ret)
		perror("ioctl");

	return ret;
}

int send_status(int fd, __u32 *response)
{
	int ret = 0;
	struct mmc_ioc_cmd idata;

	memset(&idata, 0, sizeof(idata));
	idata.opcode = MMC_SEND_STATUS;
	idata.arg = (1 << 16);
	idata.flags = MMC_RSP_R1 | MMC_CMD_AC;

	ret = ioctl(fd, MMC_IOC_CMD, &idata);
	if (ret)
	perror("ioctl");

	*response = idata.response[0];

	return ret;
}

static __u32 get_size_in_blks(int fd)
{
	int res;
	int size;

	res = ioctl(fd, BLKGETSIZE, &size);
	if (res) {
		fprintf(stderr, "Error getting device size, errno: %d\n",
			errno);
		perror("");
		return -1;
	}
	return size;
}

static int set_write_protect(int fd, __u32 blk_addr, int on_off)
{
	int ret = 0;
	struct mmc_ioc_cmd idata;

	memset(&idata, 0, sizeof(idata));
	idata.write_flag = 1;
	if (on_off)
		idata.opcode = MMC_SET_WRITE_PROT;
	else
		idata.opcode = MMC_CLEAR_WRITE_PROT;
	idata.arg = blk_addr;
	idata.flags = MMC_RSP_SPI_R1B | MMC_RSP_R1B | MMC_CMD_AC;

	ret = ioctl(fd, MMC_IOC_CMD, &idata);
	if (ret)
		perror("ioctl");

	return ret;
}

static int send_write_protect_type(int fd, __u32 blk_addr, __u64 *group_bits)
{
	int ret = 0;
	struct mmc_ioc_cmd idata;
	__u8 buf[8];
	__u64 bits = 0;
	int x;

	memset(&idata, 0, sizeof(idata));
	idata.write_flag = 0;
	idata.opcode = MMC_SEND_WRITE_PROT_TYPE;
	idata.blksz      = 8,
	idata.blocks     = 1,
	idata.arg = blk_addr;
	idata.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;
	mmc_ioc_cmd_set_data(idata, buf);

	ret = ioctl(fd, MMC_IOC_CMD, &idata);
	if (ret)
		perror("ioctl");
	for (x = 0; x < sizeof(buf); x++)
		bits |= (__u64)(buf[7 - x]) << (x * 8);
	*group_bits = bits;
	return ret;
}

static void print_writeprotect_boot_status(__u8 *ext_csd)
{
	__u8 reg;
	__u8 ext_csd_rev = ext_csd[EXT_CSD_REV];

	/* A43: reserved [174:0] */
	if (ext_csd_rev >= 5) {
		printf("Boot write protection status registers"
			" [BOOT_WP_STATUS]: 0x%02x\n", ext_csd[174]);

		reg = ext_csd[EXT_CSD_BOOT_WP];
		printf("Boot Area Write protection [BOOT_WP]: 0x%02x\n", reg);
		printf(" Power ro locking: ");
		if (reg & EXT_CSD_BOOT_WP_B_PWR_WP_DIS)
			printf("not possible\n");
		else
			printf("possible\n");

		printf(" Permanent ro locking: ");
		if (reg & EXT_CSD_BOOT_WP_B_PERM_WP_DIS)
			printf("not possible\n");
		else
			printf("possible\n");

		reg = ext_csd[EXT_CSD_BOOT_WP_STATUS];
		printf(" partition 0 ro lock status: ");
		if (reg & EXT_CSD_BOOT_WP_S_AREA_0_PERM)
			printf("locked permanently\n");
		else if (reg & EXT_CSD_BOOT_WP_S_AREA_0_PWR)
			printf("locked until next power on\n");
		else
			printf("not locked\n");
		printf(" partition 1 ro lock status: ");
		if (reg & EXT_CSD_BOOT_WP_S_AREA_1_PERM)
			printf("locked permanently\n");
		else if (reg & EXT_CSD_BOOT_WP_S_AREA_1_PWR)
			printf("locked until next power on\n");
		else
			printf("not locked\n");
	}
}

static int get_wp_group_size_in_blks(__u8 *ext_csd, __u32 *size)
{
	__u8 ext_csd_rev = ext_csd[EXT_CSD_REV];

	if ((ext_csd_rev < 5) || (ext_csd[EXT_CSD_ERASE_GROUP_DEF] == 0))
		return 1;

	*size = ext_csd[EXT_CSD_HC_ERASE_GRP_SIZE] *
		ext_csd[EXT_CSD_HC_WP_GRP_SIZE] * 1024;
	return 0;
}


int do_writeprotect_boot_get(int nargs, char **argv)
{
	__u8 ext_csd[512];
	int fd, ret;
	char *device;

	if (nargs != 2) {
		fprintf(stderr, "Usage: mmc writeprotect boot get </path/to/mmcblkX>\n");
		exit(1);
	}

	device = argv[1];

	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	ret = read_extcsd(fd, ext_csd);
	if (ret) {
		fprintf(stderr, "Could not read EXT_CSD from %s\n", device);
		exit(1);
	}

	print_writeprotect_boot_status(ext_csd);

	close(fd);
	return ret;
}

int do_writeprotect_boot_set(int nargs, char **argv)
{
	__u8 ext_csd[512], value;
	int fd, ret;
	char *device;
	char *end;
	int argi = 1;
	int permanent = 0;
	int partition = -1;

#ifdef DANGEROUS_COMMANDS_ENABLED
	if (!strcmp(argv[argi], "-p")){
		permanent = 1;
		argi++;
	}
#endif

	if (nargs < 1 + argi ||  nargs > 2 + argi) {
		fprintf(stderr, "Usage: mmc writeprotect boot set "
#ifdef DANGEROUS_COMMANDS_ENABLED
			"[-p] "
#endif
			"</path/to/mmcblkX> [0|1]\n");
		exit(1);
	}

	device = argv[argi++];

	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	if (nargs == 1 + argi) {
		partition = strtoul(argv[argi], &end, 0);
		if (*end != '\0' || !(partition == 0 || partition == 1)) {
			fprintf(stderr, "Invalid partition number (must be 0 or 1): %s\n",
				argv[argi]);
			exit(1);
		}
	}

	ret = read_extcsd(fd, ext_csd);
	if (ret) {
		fprintf(stderr, "Could not read EXT_CSD from %s\n", device);
		exit(1);
	}

	value = ext_csd[EXT_CSD_BOOT_WP];
	/*
	 * If permanent protection is already on for one partition and we're
	 * trying to enable power-reset protection for the other we need to make
	 * sure the selection bit for permanent protection still points to the
	 * former or we'll accidentally permanently protect the latter.
	 */
	if ((value & EXT_CSD_BOOT_WP_B_PERM_WP_EN) && !permanent) {
		if (ext_csd[EXT_CSD_BOOT_WP_STATUS] &
		    EXT_CSD_BOOT_WP_S_AREA_1_PERM) {
			value |= EXT_CSD_BOOT_WP_B_PERM_WP_SEC_SEL;
			if (partition != 1)
				partition = 0;
		} else {
			/* PERM_WP_SEC_SEL cleared -> pointing to partition 0 */
			if (partition != 0)
				partition = 1;
		}
	}
	if (partition != -1) {
		value |= EXT_CSD_BOOT_WP_B_SEC_WP_SEL;
		if (partition == 1)
			value |= permanent ? EXT_CSD_BOOT_WP_B_PERM_WP_SEC_SEL
					   : EXT_CSD_BOOT_WP_B_PWR_WP_SEC_SEL;
	}
	value |= permanent ? EXT_CSD_BOOT_WP_B_PERM_WP_EN
			   : EXT_CSD_BOOT_WP_B_PWR_WP_EN;

	ret = write_extcsd_value(fd, EXT_CSD_BOOT_WP, value, 0);
	if (ret) {
		fprintf(stderr, "Could not write 0x%02x to "
			"EXT_CSD[%d] in %s\n",
			value, EXT_CSD_BOOT_WP, device);
		exit(1);
	}

	close(fd);
	return ret;
}

static char *prot_desc[] = {
	"No",
	"Temporary",
	"Power-on",
	"Permanent"
};

static void print_wp_status(__u32 wp_sizeblks, __u32 start_group,
			__u32 end_group, int rptype)
{
	printf("Write Protect Groups %d-%d (Blocks %d-%d), ",
		start_group, end_group,
		start_group * wp_sizeblks, ((end_group + 1) * wp_sizeblks) - 1);
	printf("%s Write Protection\n", prot_desc[rptype]);
}


int do_writeprotect_user_get(int nargs, char **argv)
{
	__u8 ext_csd[512];
	int fd, ret;
	char *device;
	int x;
	int y = 0;
	__u32 wp_sizeblks;
	__u32 dev_sizeblks;
	__u32 cnt;
	__u64 bits;
	__u32 wpblk;
	__u32 last_wpblk = 0;
	__u32 prot;
	__u32 last_prot = -1;
	int remain;

	if (nargs != 2) {
		fprintf(stderr, "Usage: mmc writeprotect user get </path/to/mmcblkX>\n");
		exit(1);
	}

	device = argv[1];

	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}
	ret = read_extcsd(fd, ext_csd);
	if (ret) {
		fprintf(stderr, "Could not read EXT_CSD from %s\n", device);
		exit(1);
	}

	ret = get_wp_group_size_in_blks(ext_csd, &wp_sizeblks);
	if (ret)
		exit(1);
	printf("Write Protect Group size in blocks/bytes: %d/%d\n",
		wp_sizeblks, wp_sizeblks * 512);
	dev_sizeblks = get_size_in_blks(fd);
	cnt = dev_sizeblks / wp_sizeblks;
	for (x = 0; x < cnt; x += WP_BLKS_PER_QUERY) {
		ret = send_write_protect_type(fd, x * wp_sizeblks, &bits);
		if (ret)
			break;
		remain = cnt - x;
		if (remain > WP_BLKS_PER_QUERY)
			remain = WP_BLKS_PER_QUERY;
		for (y = 0; y < remain; y++) {
			prot = (bits >> (y * 2)) & 0x3;
			if (prot != last_prot) {
				/* not first time */
				if (last_prot != -1) {
					wpblk = x + y;
					print_wp_status(wp_sizeblks,
							last_wpblk,
							wpblk - 1,
							last_prot);
					last_wpblk = wpblk;
				}
				last_prot = prot;
			}
		}
	}
	if (last_wpblk != (x + y - 1))
		print_wp_status(wp_sizeblks, last_wpblk, cnt - 1, last_prot);

	close(fd);
	return ret;
}

int do_writeprotect_user_set(int nargs, char **argv)
{
	__u8 ext_csd[512];
	int fd, ret;
	char *device;
	int blk_start;
	int blk_cnt;
	__u32 wp_blks;
	__u8 user_wp;
	int x;
	int wptype;

	if (nargs != 5)
		goto usage;
	device = argv[4];
	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}
	if (!strcmp(argv[1], "none")) {
		wptype = WPTYPE_NONE;
	} else if (!strcmp(argv[1], "temp")) {
		wptype = WPTYPE_TEMP;
	} else if (!strcmp(argv[1], "pwron")) {
		wptype = WPTYPE_PWRON;
#ifdef DANGEROUS_COMMANDS_ENABLED
	} else if (!strcmp(argv[1], "perm")) {
		wptype = WPTYPE_PERM;
#endif /* DANGEROUS_COMMANDS_ENABLED */
	} else {
		fprintf(stderr, "Error, invalid \"type\"\n");
		goto usage;
	}
	ret = read_extcsd(fd, ext_csd);
	if (ret) {
		fprintf(stderr, "Could not read EXT_CSD from %s\n", device);
		exit(1);
	}
	ret = get_wp_group_size_in_blks(ext_csd, &wp_blks);
	if (ret) {
		fprintf(stderr, "Operation not supported for this device\n");
		exit(1);
	}
	blk_start = strtol(argv[2], NULL, 0);
	blk_cnt = strtol(argv[3], NULL, 0);
	if ((blk_start % wp_blks) || (blk_cnt % wp_blks)) {
		fprintf(stderr, "<start block> and <blocks> must be a ");
		fprintf(stderr, "multiple of the Write Protect Group (%d)\n",
			wp_blks);
		exit(1);
	}
	if (wptype != WPTYPE_NONE) {
		user_wp = ext_csd[EXT_CSD_USER_WP];
		user_wp &= ~USER_WP_CLEAR;
		switch (wptype) {
		case WPTYPE_TEMP:
			break;
		case WPTYPE_PWRON:
			user_wp |= USER_WP_US_PWR_WP_EN;
			break;
		case WPTYPE_PERM:
			user_wp |= USER_WP_US_PERM_WP_EN;
			break;
		}
		if (user_wp != ext_csd[EXT_CSD_USER_WP]) {
			ret = write_extcsd_value(fd, EXT_CSD_USER_WP, user_wp, 0);
			if (ret) {
				fprintf(stderr, "Error setting EXT_CSD\n");
				exit(1);
			}
		}
	}
	for (x = 0; x < blk_cnt; x += wp_blks) {
		ret = set_write_protect(fd, blk_start + x,
					wptype != WPTYPE_NONE);
		if (ret) {
			fprintf(stderr,
				"Could not set write protect for %s\n", device);
			exit(1);
		}
	}
	if (wptype != WPTYPE_NONE) {
		ret = write_extcsd_value(fd, EXT_CSD_USER_WP,
				ext_csd[EXT_CSD_USER_WP], 0);
		if (ret) {
			fprintf(stderr, "Error restoring EXT_CSD\n");
			exit(1);
		}
	}
	return ret;

usage:
	fprintf(stderr,
		"Usage: mmc writeprotect user set <type><start block><blocks><device>\n");
	exit(1);
}

int do_disable_512B_emulation(int nargs, char **argv)
{
	__u8 ext_csd[512], native_sector_size, data_sector_size, wr_rel_param;
	int fd, ret;
	char *device;

	if (nargs != 2) {
		fprintf(stderr, "Usage: mmc disable 512B emulation </path/to/mmcblkX>\n");
		exit(1);
	}

	device = argv[1];

	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	ret = read_extcsd(fd, ext_csd);
	if (ret) {
		fprintf(stderr, "Could not read EXT_CSD from %s\n", device);
		exit(1);
	}

	wr_rel_param = ext_csd[EXT_CSD_WR_REL_PARAM];
	native_sector_size = ext_csd[EXT_CSD_NATIVE_SECTOR_SIZE];
	data_sector_size = ext_csd[EXT_CSD_DATA_SECTOR_SIZE];

	if (native_sector_size && !data_sector_size &&
	   (wr_rel_param & EN_REL_WR)) {
		ret = write_extcsd_value(fd, EXT_CSD_USE_NATIVE_SECTOR, 1, 0);

		if (ret) {
			fprintf(stderr, "Could not write 0x%02x to EXT_CSD[%d] in %s\n",
					1, EXT_CSD_NATIVE_SECTOR_SIZE, device);
			exit(1);
		}
		printf("MMC disable 512B emulation successful.  Now reset the device to switch to 4KB native sector mode.\n");
	} else if (native_sector_size && data_sector_size) {
		printf("MMC 512B emulation mode is already disabled; doing nothing.\n");
	} else {
		printf("MMC does not support disabling 512B emulation mode.\n");
	}

	close(fd);
	return ret;
}

int do_write_boot_en(int nargs, char **argv)
{
	__u8 ext_csd[512];
	__u8 value = 0;
	int fd, ret;
	char *device;
	int boot_area, send_ack;

	if (nargs != 4) {
		fprintf(stderr, "Usage: mmc bootpart enable <partition_number> <send_ack> </path/to/mmcblkX>\n");
		exit(1);
	}

	/*
	 * If <send_ack> is 1, the device will send acknowledgment
	 * pattern "010" to the host when boot operation begins.
	 * If <send_ack> is 0, it won't.
	 */
	boot_area = strtol(argv[1], NULL, 10);
	send_ack = strtol(argv[2], NULL, 10);
	device = argv[3];

	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	ret = read_extcsd(fd, ext_csd);
	if (ret) {
		fprintf(stderr, "Could not read EXT_CSD from %s\n", device);
		exit(1);
	}

	value = ext_csd[EXT_CSD_PART_CONFIG];

	switch (boot_area) {
	case EXT_CSD_PART_CONFIG_ACC_NONE:
		value &= ~(7 << 3);
		break;
	case EXT_CSD_PART_CONFIG_ACC_BOOT0:
		value |= (1 << 3);
		value &= ~(3 << 4);
		break;
	case EXT_CSD_PART_CONFIG_ACC_BOOT1:
		value |= (1 << 4);
		value &= ~(1 << 3);
		value &= ~(1 << 5);
		break;
	case EXT_CSD_PART_CONFIG_ACC_USER_AREA:
		value |= (boot_area << 3);
		break;
	default:
		fprintf(stderr, "Cannot enable the boot area\n");
		exit(1);
	}
	if (send_ack)
		value |= EXT_CSD_PART_CONFIG_ACC_ACK;
	else
		value &= ~EXT_CSD_PART_CONFIG_ACC_ACK;

	ret = write_extcsd_value(fd, EXT_CSD_PART_CONFIG, value, 0);
	if (ret) {
		fprintf(stderr, "Could not write 0x%02x to "
			"EXT_CSD[%d] in %s\n",
			value, EXT_CSD_PART_CONFIG, device);
		exit(1);
	}
	close(fd);
	return ret;
}

int do_boot_bus_conditions_set(int nargs, char **argv)
{
	__u8 ext_csd[512];
	__u8 value = 0;
	int fd, ret;
	char *device;

	if (nargs != 5) {
		fprintf(stderr, "Usage: mmc: bootbus set <boot_mode> <reset_boot_bus_conditions> <boot_bus_width> <device>\n");
		exit(1);
	}

	if (strcmp(argv[1], "single_backward") == 0)
		value |= 0;
	else if (strcmp(argv[1], "single_hs") == 0)
		value |= 0x8;
	else if (strcmp(argv[1], "dual") == 0)
		value |= 0x10;
	else {
		fprintf(stderr, "illegal <boot_mode> specified\n");
		exit(1);
	}

	if (strcmp(argv[2], "x1") == 0)
		value |= 0;
	else if (strcmp(argv[2], "retain") == 0)
		value |= 0x4;
	else {
		fprintf(stderr,
			"illegal <reset_boot_bus_conditions> specified\n");
		exit(1);
	}

	if (strcmp(argv[3], "x1") == 0)
		value |= 0;
	else if (strcmp(argv[3], "x4") == 0)
		value |= 0x1;
	else if (strcmp(argv[3], "x8") == 0)
		value |= 0x2;
	else {
		fprintf(stderr,	"illegal <boot_bus_width> specified\n");
		exit(1);
	}

	device = argv[4];
	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	ret = read_extcsd(fd, ext_csd);
	if (ret) {
		fprintf(stderr, "Could not read EXT_CSD from %s\n", device);
		exit(1);
	}
	printf("Changing ext_csd[BOOT_BUS_CONDITIONS] from 0x%02x to 0x%02x\n",
		ext_csd[EXT_CSD_BOOT_BUS_CONDITIONS], value);

	ret = write_extcsd_value(fd, EXT_CSD_BOOT_BUS_CONDITIONS, value, 0);
	if (ret) {
		fprintf(stderr, "Could not write 0x%02x to "
			"EXT_CSD[%d] in %s\n",
			value, EXT_CSD_BOOT_BUS_CONDITIONS, device);
		exit(1);
	}
	close(fd);
	return ret;
}

int do_hwreset(int value, int nargs, char **argv)
{
	__u8 ext_csd[512];
	int fd, ret;
	char *device;

	if (nargs != 2) {
		fprintf(stderr, "Usage: mmc hwreset enable </path/to/mmcblkX>\n");
		exit(1);
	}

	device = argv[1];

	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	ret = read_extcsd(fd, ext_csd);
	if (ret) {
		fprintf(stderr, "Could not read EXT_CSD from %s\n", device);
		exit(1);
	}

	if ((ext_csd[EXT_CSD_RST_N_FUNCTION] & EXT_CSD_RST_N_EN_MASK) ==
	    EXT_CSD_HW_RESET_EN) {
		fprintf(stderr,
			"H/W Reset is already permanently enabled on %s\n",
			device);
		exit(1);
	}
	if ((ext_csd[EXT_CSD_RST_N_FUNCTION] & EXT_CSD_RST_N_EN_MASK) ==
	    EXT_CSD_HW_RESET_DIS) {
		fprintf(stderr,
			"H/W Reset is already permanently disabled on %s\n",
			device);
		exit(1);
	}

	ret = write_extcsd_value(fd, EXT_CSD_RST_N_FUNCTION, value, 0);
	if (ret) {
		fprintf(stderr,
			"Could not write 0x%02x to EXT_CSD[%d] in %s\n",
			value, EXT_CSD_RST_N_FUNCTION, device);
		exit(1);
	}

	close(fd);
	return ret;
}

int do_hwreset_en(int nargs, char **argv)
{
	return do_hwreset(EXT_CSD_HW_RESET_EN, nargs, argv);
}

int do_hwreset_dis(int nargs, char **argv)
{
	return do_hwreset(EXT_CSD_HW_RESET_DIS, nargs, argv);
}

int do_write_bkops_en(int nargs, char **argv)
{
	__u8 ext_csd[512], value = 0;
	int fd, ret;
	char *device;
	char *en_type;

	if (nargs != 3) {
		fprintf(stderr, "Usage: mmc bkops_en <auto|manual> </path/to/mmcblkX>\n");
		exit(1);
	}

	en_type = argv[1];
	device = argv[2];

	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	ret = read_extcsd(fd, ext_csd);
	if (ret) {
		fprintf(stderr, "Could not read EXT_CSD from %s\n", device);
		exit(1);
	}

	if (strcmp(en_type, "auto") == 0) {
		if (ext_csd[EXT_CSD_REV] < EXT_CSD_REV_V5_0) {
			fprintf(stderr, "%s doesn't support AUTO_EN in the BKOPS_EN register\n", device);
			exit(1);
		}
		ret = write_extcsd_value(fd, EXT_CSD_BKOPS_EN, BKOPS_AUTO_ENABLE, 0);
	} else if (strcmp(en_type, "manual") == 0) {
		ret = write_extcsd_value(fd, EXT_CSD_BKOPS_EN, BKOPS_MAN_ENABLE, 0);
	} else {
		fprintf(stderr, "%s invalid mode for BKOPS_EN requested: %s. Valid options: auto or manual\n", en_type, device);
		exit(1);
	}

	if (ret) {
		fprintf(stderr, "Could not write 0x%02x to EXT_CSD[%d] in %s\n",
			value, EXT_CSD_BKOPS_EN, device);
		exit(1);
	}

	close(fd);
	return ret;
}

int do_status_get(int nargs, char **argv)
{
	__u32 response;
	int fd, ret;
	char *device;
	const char *str;
	__u8 state;

	if (nargs != 2) {
		fprintf(stderr, "Usage: mmc status get </path/to/mmcblkX>\n");
		exit(1);
	}

	device = argv[1];

	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	ret = send_status(fd, &response);
	if (ret) {
		fprintf(stderr, "Could not read response to SEND_STATUS from %s\n", device);
		exit(1);
	}

	printf("SEND_STATUS response: 0x%08x\n", response);

	if (response & R1_OUT_OF_RANGE)
		printf("ERROR: ADDRESS_OUT_OF_RANGE\n");
	if (response & R1_ADDRESS_ERROR)
		printf("ERROR: ADDRESS_MISALIGN\n");
	if (response & R1_BLOCK_LEN_ERROR)
		printf("ERROR: BLOCK_LEN_ERROR\n");
	if (response & R1_ERASE_SEQ_ERROR)
		printf("ERROR: ERASE_SEQ_ERROR\n");
	if (response & R1_ERASE_PARAM)
		printf("ERROR: ERASE_PARAM_ERROR\n");
	if (response & R1_WP_VIOLATION)
		printf("ERROR: WP_VOILATION\n");
	if (response & R1_CARD_IS_LOCKED)
		printf("STATUS: DEVICE_IS_LOCKED\n");
	if (response & R1_LOCK_UNLOCK_FAILED)
		printf("ERROR: LOCK_UNLOCK_IS_FAILED\n");
	if (response & R1_COM_CRC_ERROR)
		printf("ERROR: COM_CRC_ERROR\n");
	if (response & R1_ILLEGAL_COMMAND)
		printf("ERROR: ILLEGAL_COMMAND\n");
	if (response & R1_CARD_ECC_FAILED)
		printf("ERROR: DEVICE_ECC_FAILED\n");
	if (response & R1_CC_ERROR)
		printf("ERROR: CC_ERROR\n");
	if (response & R1_ERROR)
		printf("ERROR: ERROR\n");
	if (response & R1_CID_CSD_OVERWRITE)
		printf("ERROR: CID/CSD OVERWRITE\n");
	if (response & R1_WP_ERASE_SKIP)
		printf("ERROR: WP_ERASE_SKIP\n");
	if (response & R1_ERASE_RESET)
		printf("ERROR: ERASE_RESET\n");

	state = (response >> 9) & 0xF;
	switch (state) {
	case 0:
		str = "IDLE";
		break;
	case 1:
		str = "READY";
		break;
	case 2:
		str = "IDENT";
		break;
	case 3:
		str = "STDBY";
		break;
	case 4:
		str = "TRANS";
		break;
	case 5:
		str = "DATA";
		break;
	case 6:
		str = "RCV";
		break;
	case 7:
		str = "PRG";
		break;
	case 8:
		str = "DIS";
		break;
	case 9:
		str = "BTST";
		break;
	case 10:
		str = "SLP";
		break;
	default:
		printf("Attention : Device state is INVALID: Kindly check the Response\n");
		goto out_free;
	}

	printf("DEVICE STATE: %s\n", str);
	if (response & R1_READY_FOR_DATA)
		printf("STATUS: READY_FOR_DATA\n");
	if (response & R1_SWITCH_ERROR)
		printf("ERROR: SWITCH_ERROR\n");
	if (response & R1_EXCEPTION_EVENT)
		printf("STATUS: EXCEPTION_EVENT\n");  /* Check EXCEPTION_EVENTS_STATUS fields for further actions */
	if (response & R1_APP_CMD)
		printf("STATUS: APP_CMD\n");
out_free:
	close(fd);
	return ret;
}

unsigned int get_sector_count(__u8 *ext_csd)
{
	return (ext_csd[EXT_CSD_SEC_COUNT_3] << 24) |
	(ext_csd[EXT_CSD_SEC_COUNT_2] << 16) |
	(ext_csd[EXT_CSD_SEC_COUNT_1] << 8)  |
	ext_csd[EXT_CSD_SEC_COUNT_0];
}

int is_blockaddresed(__u8 *ext_csd)
{
	unsigned int sectors = get_sector_count(ext_csd);

	/* over 2GiB devices are block-addressed */
	return (sectors > (2u * 1024 * 1024 * 1024) / 512);
}

unsigned int get_hc_wp_grp_size(__u8 *ext_csd)
{
	return ext_csd[221];
}

unsigned int get_hc_erase_grp_size(__u8 *ext_csd)
{
	return ext_csd[224];
}

int set_partitioning_setting_completed(int dry_run, const char * const device,
		int fd)
{
	int ret;

	if (dry_run == 1) {
		fprintf(stderr, "NOT setting PARTITION_SETTING_COMPLETED\n");
		fprintf(stderr, "These changes will not take effect neither "
			"now nor after a power cycle\n");
		return 1;
	} else if (dry_run == 2) {
		printf("-c given, expecting more partition settings before "
			"writing PARTITION_SETTING_COMPLETED\n");
		return 0;
	}

	fprintf(stderr, "setting OTP PARTITION_SETTING_COMPLETED!\n");
	ret = write_extcsd_value(fd, EXT_CSD_PARTITION_SETTING_COMPLETED, 0x1, 0);
	if (ret) {
		fprintf(stderr, "Could not write 0x1 to "
			"EXT_CSD[%d] in %s\n",
			EXT_CSD_PARTITION_SETTING_COMPLETED, device);
		return 1;
	}

	__u32 response;
	ret = send_status(fd, &response);
	if (ret) {
		fprintf(stderr, "Could not get response to SEND_STATUS "
			"from %s\n", device);
		return 1;
	}

	if (response & R1_SWITCH_ERROR) {
		fprintf(stderr, "Setting OTP PARTITION_SETTING_COMPLETED "
			"failed on %s\n", device);
		return 1;
	}

	fprintf(stderr, "Setting OTP PARTITION_SETTING_COMPLETED on "
		"%s SUCCESS\n", device);
	fprintf(stderr, "Device power cycle needed for settings to "
		"take effect.\n"
		"Confirm that PARTITION_SETTING_COMPLETED bit is set "
		"using 'extcsd read' after power cycle\n");

	return 0;
}

int check_enhanced_area_total_limit(const char * const device, int fd)
{
	__u8 ext_csd[512];
	__u32 regl;
	unsigned long max_enh_area_sz, user_area_sz, enh_area_sz = 0;
	unsigned long gp4_part_sz, gp3_part_sz, gp2_part_sz, gp1_part_sz;
	unsigned long total_sz, total_gp_user_sz;
	unsigned int wp_sz, erase_sz;
	int ret;

	ret = read_extcsd(fd, ext_csd);
	if (ret) {
		fprintf(stderr, "Could not read EXT_CSD from %s\n", device);
		exit(1);
	}
	wp_sz = get_hc_wp_grp_size(ext_csd);
	erase_sz = get_hc_erase_grp_size(ext_csd);

	regl = (ext_csd[EXT_CSD_GP_SIZE_MULT_4_2] << 16) |
		(ext_csd[EXT_CSD_GP_SIZE_MULT_4_1] << 8) |
		ext_csd[EXT_CSD_GP_SIZE_MULT_4_0];
	gp4_part_sz = 512l * regl * erase_sz * wp_sz;
	if (ext_csd[EXT_CSD_PARTITIONS_ATTRIBUTE] & EXT_CSD_ENH_4) {
		enh_area_sz += gp4_part_sz;
		printf("Enhanced GP4 Partition Size [GP_SIZE_MULT_4]: 0x%06x\n", regl);
		printf(" i.e. %lu KiB\n", gp4_part_sz);
	}

	regl = (ext_csd[EXT_CSD_GP_SIZE_MULT_3_2] << 16) |
		(ext_csd[EXT_CSD_GP_SIZE_MULT_3_1] << 8) |
		ext_csd[EXT_CSD_GP_SIZE_MULT_3_0];
	gp3_part_sz = 512l * regl * erase_sz * wp_sz;
	if (ext_csd[EXT_CSD_PARTITIONS_ATTRIBUTE] & EXT_CSD_ENH_3) {
		enh_area_sz += gp3_part_sz;
		printf("Enhanced GP3 Partition Size [GP_SIZE_MULT_3]: 0x%06x\n", regl);
		printf(" i.e. %lu KiB\n", gp3_part_sz);
	}

	regl = (ext_csd[EXT_CSD_GP_SIZE_MULT_2_2] << 16) |
		(ext_csd[EXT_CSD_GP_SIZE_MULT_2_1] << 8) |
		ext_csd[EXT_CSD_GP_SIZE_MULT_2_0];
	gp2_part_sz = 512l * regl * erase_sz * wp_sz;
	if (ext_csd[EXT_CSD_PARTITIONS_ATTRIBUTE] & EXT_CSD_ENH_2) {
		enh_area_sz += gp2_part_sz;
		printf("Enhanced GP2 Partition Size [GP_SIZE_MULT_2]: 0x%06x\n", regl);
		printf(" i.e. %lu KiB\n", gp2_part_sz);
	}

	regl = (ext_csd[EXT_CSD_GP_SIZE_MULT_1_2] << 16) |
		(ext_csd[EXT_CSD_GP_SIZE_MULT_1_1] << 8) |
		ext_csd[EXT_CSD_GP_SIZE_MULT_1_0];
	gp1_part_sz = 512l * regl * erase_sz * wp_sz;
	if (ext_csd[EXT_CSD_PARTITIONS_ATTRIBUTE] & EXT_CSD_ENH_1) {
		enh_area_sz += gp1_part_sz;
		printf("Enhanced GP1 Partition Size [GP_SIZE_MULT_1]: 0x%06x\n", regl);
		printf(" i.e. %lu KiB\n", gp1_part_sz);
	}

	regl = (ext_csd[EXT_CSD_ENH_SIZE_MULT_2] << 16) |
		(ext_csd[EXT_CSD_ENH_SIZE_MULT_1] << 8) |
		ext_csd[EXT_CSD_ENH_SIZE_MULT_0];
	user_area_sz = 512l * regl * erase_sz * wp_sz;
	if (ext_csd[EXT_CSD_PARTITIONS_ATTRIBUTE] & EXT_CSD_ENH_USR) {
		enh_area_sz += user_area_sz;
		printf("Enhanced User Data Area Size [ENH_SIZE_MULT]: 0x%06x\n", regl);
		printf(" i.e. %lu KiB\n", user_area_sz);
	}

	regl = (ext_csd[EXT_CSD_MAX_ENH_SIZE_MULT_2] << 16) |
		(ext_csd[EXT_CSD_MAX_ENH_SIZE_MULT_1] << 8) |
		ext_csd[EXT_CSD_MAX_ENH_SIZE_MULT_0];
	max_enh_area_sz = 512l * regl * erase_sz * wp_sz;
	printf("Max Enhanced Area Size [MAX_ENH_SIZE_MULT]: 0x%06x\n", regl);
	printf(" i.e. %lu KiB\n", max_enh_area_sz);
	if (enh_area_sz > max_enh_area_sz) {
		fprintf(stderr,
			"Programmed total enhanced size %lu KiB cannot exceed max enhanced area %lu KiB %s\n",
			enh_area_sz, max_enh_area_sz, device);
		return 1;
	}
	total_sz = get_sector_count(ext_csd) / 2;
	total_gp_user_sz = gp4_part_sz + gp3_part_sz + gp2_part_sz +
				gp1_part_sz + user_area_sz;
	if (total_gp_user_sz > total_sz) {
		fprintf(stderr,
			"requested total partition size %lu KiB cannot exceed card capacity %lu KiB %s\n",
			total_gp_user_sz, total_sz, device);
		return 1;
	}

	return 0;
}

int do_create_gp_partition(int nargs, char **argv)
{
	__u8 value;
	__u8 ext_csd[512];
	__u8 address;
	int fd, ret;
	char *device;
	int dry_run = 1;
	int partition, enh_attr, ext_attr;
	unsigned int length_kib, gp_size_mult;
	unsigned long align;

	if (nargs != 7) {
		fprintf(stderr, "Usage: mmc gp create <-y|-n|-c> <length KiB> <partition> <enh_attr> <ext_attr> </path/to/mmcblkX>\n");
		exit(1);
	}

	if (!strcmp("-y", argv[1])) {
		dry_run = 0;
        } else if (!strcmp("-c", argv[1])) {
		dry_run = 2;
	}

	length_kib = strtol(argv[2], NULL, 10);
	partition = strtol(argv[3], NULL, 10);
	enh_attr = strtol(argv[4], NULL, 10);
	ext_attr = strtol(argv[5], NULL, 10);
	device = argv[6];

	if (partition < 1 || partition > 4) {
		printf("Invalid gp partition number; valid range [1-4].\n");
		exit(1);
	}

	if (enh_attr && ext_attr) {
		printf("Not allowed to set both enhanced attribute and extended attribute\n");
		exit(1);
	}

	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	ret = read_extcsd(fd, ext_csd);
	if (ret) {
		fprintf(stderr, "Could not read EXT_CSD from %s\n", device);
		exit(1);
	}

	/* assert not PARTITION_SETTING_COMPLETED */
	if (ext_csd[EXT_CSD_PARTITION_SETTING_COMPLETED]) {
		printf(" Device is already partitioned\n");
		exit(1);
	}

	align = 512l * get_hc_wp_grp_size(ext_csd) * get_hc_erase_grp_size(ext_csd);
	gp_size_mult = (length_kib + align/2l) / align;

	/* set EXT_CSD_ERASE_GROUP_DEF bit 0 */
	ret = write_extcsd_value(fd, EXT_CSD_ERASE_GROUP_DEF, 0x1, 0);
	if (ret) {
		fprintf(stderr, "Could not write 0x1 to EXT_CSD[%d] in %s\n",
			EXT_CSD_ERASE_GROUP_DEF, device);
		exit(1);
	}

	value = (gp_size_mult >> 16) & 0xff;
	address = EXT_CSD_GP_SIZE_MULT_1_2 + (partition - 1) * 3;
	ret = write_extcsd_value(fd, address, value, 0);
	if (ret) {
		fprintf(stderr, "Could not write 0x%02x to EXT_CSD[%d] in %s\n",
			value, address, device);
		exit(1);
	}
	value = (gp_size_mult >> 8) & 0xff;
	address = EXT_CSD_GP_SIZE_MULT_1_1 + (partition - 1) * 3;
	ret = write_extcsd_value(fd, address, value, 0);
	if (ret) {
		fprintf(stderr, "Could not write 0x%02x to EXT_CSD[%d] in %s\n",
			value, address, device);
		exit(1);
	}
	value = gp_size_mult & 0xff;
	address = EXT_CSD_GP_SIZE_MULT_1_0 + (partition - 1) * 3;
	ret = write_extcsd_value(fd, address, value, 0);
	if (ret) {
		fprintf(stderr, "Could not write 0x%02x to EXT_CSD[%d] in %s\n",
			value, address, device);
		exit(1);
	}

	value = ext_csd[EXT_CSD_PARTITIONS_ATTRIBUTE];
	if (enh_attr)
		value |= (1 << partition);
	else
		value &= ~(1 << partition);

	ret = write_extcsd_value(fd, EXT_CSD_PARTITIONS_ATTRIBUTE, value, 0);
	if (ret) {
		fprintf(stderr, "Could not write EXT_CSD_ENH_%x to EXT_CSD[%d] in %s\n",
			partition, EXT_CSD_PARTITIONS_ATTRIBUTE, device);
		exit(1);
	}

	address = EXT_CSD_EXT_PARTITIONS_ATTRIBUTE_0 + (partition - 1) / 2;
	value = ext_csd[address];
	if (ext_attr)
		value |= (ext_attr << (4 * ((partition - 1) % 2)));
	else
		value &= (0xF << (4 * ((partition % 2))));

	ret = write_extcsd_value(fd, address, value, 0);
	if (ret) {
		fprintf(stderr, "Could not write 0x%x to EXT_CSD[%d] in %s\n",
			value, address, device);
		exit(1);
	}

	ret = check_enhanced_area_total_limit(device, fd);
	if (ret)
		exit(1);

	if (set_partitioning_setting_completed(dry_run, device, fd))
		exit(1);

	return 0;
}

int do_enh_area_set(int nargs, char **argv)
{
	__u8 value;
	__u8 ext_csd[512];
	int fd, ret;
	char *device;
	int dry_run = 1;
	unsigned int start_kib, length_kib, enh_start_addr, enh_size_mult;
	unsigned long align;

	if (nargs != 5) {
		fprintf(stderr, "Usage: mmc enh_area set <-y|-n|-c> <start KiB> <length KiB> </path/to/mmcblkX>\n");
		exit(1);
	}

	if (!strcmp("-y", argv[1])) {
		dry_run = 0;
	} else if (!strcmp("-c", argv[1])) {
		dry_run = 2;
	}

	start_kib = strtol(argv[2], NULL, 10);
	length_kib = strtol(argv[3], NULL, 10);
	device = argv[4];

	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	ret = read_extcsd(fd, ext_csd);
	if (ret) {
		fprintf(stderr, "Could not read EXT_CSD from %s\n", device);
		exit(1);
	}

	/* assert ENH_ATTRIBUTE_EN */
	if (!(ext_csd[EXT_CSD_PARTITIONING_SUPPORT] & EXT_CSD_ENH_ATTRIBUTE_EN))
	{
		printf(" Device cannot have enhanced tech.\n");
		exit(1);
	}

	/* assert not PARTITION_SETTING_COMPLETED */
	if (ext_csd[EXT_CSD_PARTITION_SETTING_COMPLETED])
	{
		printf(" Device is already partitioned\n");
		exit(1);
	}

	align = 512l * get_hc_wp_grp_size(ext_csd) * get_hc_erase_grp_size(ext_csd);

	enh_size_mult = (length_kib + align/2l) / align;

	enh_start_addr = start_kib * (1024 / (is_blockaddresed(ext_csd) ? 512 : 1));
	enh_start_addr /= align;
	enh_start_addr *= align;

	/* set EXT_CSD_ERASE_GROUP_DEF bit 0 */
	ret = write_extcsd_value(fd, EXT_CSD_ERASE_GROUP_DEF, 0x1, 0);
	if (ret) {
		fprintf(stderr, "Could not write 0x1 to "
			"EXT_CSD[%d] in %s\n",
			EXT_CSD_ERASE_GROUP_DEF, device);
		exit(1);
	}

	/* write to ENH_START_ADDR and ENH_SIZE_MULT and PARTITIONS_ATTRIBUTE's ENH_USR bit */
	value = (enh_start_addr >> 24) & 0xff;
	ret = write_extcsd_value(fd, EXT_CSD_ENH_START_ADDR_3, value, 0);
	if (ret) {
		fprintf(stderr, "Could not write 0x%02x to "
			"EXT_CSD[%d] in %s\n", value,
			EXT_CSD_ENH_START_ADDR_3, device);
		exit(1);
	}
	value = (enh_start_addr >> 16) & 0xff;
	ret = write_extcsd_value(fd, EXT_CSD_ENH_START_ADDR_2, value, 0);
	if (ret) {
		fprintf(stderr, "Could not write 0x%02x to "
			"EXT_CSD[%d] in %s\n", value,
			EXT_CSD_ENH_START_ADDR_2, device);
		exit(1);
	}
	value = (enh_start_addr >> 8) & 0xff;
	ret = write_extcsd_value(fd, EXT_CSD_ENH_START_ADDR_1, value, 0);
	if (ret) {
		fprintf(stderr, "Could not write 0x%02x to "
			"EXT_CSD[%d] in %s\n", value,
			EXT_CSD_ENH_START_ADDR_1, device);
		exit(1);
	}
	value = enh_start_addr & 0xff;
	ret = write_extcsd_value(fd, EXT_CSD_ENH_START_ADDR_0, value, 0);
	if (ret) {
		fprintf(stderr, "Could not write 0x%02x to "
			"EXT_CSD[%d] in %s\n", value,
			EXT_CSD_ENH_START_ADDR_0, device);
		exit(1);
	}

	value = (enh_size_mult >> 16) & 0xff;
	ret = write_extcsd_value(fd, EXT_CSD_ENH_SIZE_MULT_2, value, 0);
	if (ret) {
		fprintf(stderr, "Could not write 0x%02x to "
			"EXT_CSD[%d] in %s\n", value,
			EXT_CSD_ENH_SIZE_MULT_2, device);
		exit(1);
	}
	value = (enh_size_mult >> 8) & 0xff;
	ret = write_extcsd_value(fd, EXT_CSD_ENH_SIZE_MULT_1, value, 0);
	if (ret) {
		fprintf(stderr, "Could not write 0x%02x to "
			"EXT_CSD[%d] in %s\n", value,
			EXT_CSD_ENH_SIZE_MULT_1, device);
		exit(1);
	}
	value = enh_size_mult & 0xff;
	ret = write_extcsd_value(fd, EXT_CSD_ENH_SIZE_MULT_0, value, 0);
	if (ret) {
		fprintf(stderr, "Could not write 0x%02x to "
			"EXT_CSD[%d] in %s\n", value,
			EXT_CSD_ENH_SIZE_MULT_0, device);
		exit(1);
	}
	value = ext_csd[EXT_CSD_PARTITIONS_ATTRIBUTE] | EXT_CSD_ENH_USR;
	ret = write_extcsd_value(fd, EXT_CSD_PARTITIONS_ATTRIBUTE, value, 0);
	if (ret) {
		fprintf(stderr, "Could not write EXT_CSD_ENH_USR to "
			"EXT_CSD[%d] in %s\n",
			EXT_CSD_PARTITIONS_ATTRIBUTE, device);
		exit(1);
	}

	ret = check_enhanced_area_total_limit(device, fd);
	if (ret)
		exit(1);

	printf("Done setting ENH_USR area on %s\n", device);

	if (set_partitioning_setting_completed(dry_run, device, fd))
		exit(1);

	return 0;
}

int do_write_reliability_set(int nargs, char **argv)
{
	__u8 value;
	__u8 ext_csd[512];
	int fd, ret;

	int dry_run = 1;
	int partition;
	char *device;

	if (nargs != 4) {
		fprintf(stderr,"Usage: mmc write_reliability set <-y|-n|-c> <partition> </path/to/mmcblkX>\n");
		exit(1);
	}

	if (!strcmp("-y", argv[1])) {
		dry_run = 0;
	} else if (!strcmp("-c", argv[1])) {
		dry_run = 2;
	}

	partition = strtol(argv[2], NULL, 10);
	device = argv[3];

	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	ret = read_extcsd(fd, ext_csd);
	if (ret) {
		fprintf(stderr, "Could not read EXT_CSD from %s\n", device);
		exit(1);
	}

	/* assert not PARTITION_SETTING_COMPLETED */
	if (ext_csd[EXT_CSD_PARTITION_SETTING_COMPLETED])
	{
		printf(" Device is already partitioned\n");
		exit(1);
	}

	/* assert HS_CTRL_REL */
	if (!(ext_csd[EXT_CSD_WR_REL_PARAM] & HS_CTRL_REL)) {
		printf("Cannot set write reliability parameters, WR_REL_SET is "
				"read-only\n");
		exit(1);
	}

	value = ext_csd[EXT_CSD_WR_REL_SET] | (1<<partition);
	ret = write_extcsd_value(fd, EXT_CSD_WR_REL_SET, value, 0);
	if (ret) {
		fprintf(stderr, "Could not write 0x%02x to EXT_CSD[%d] in %s\n",
				value, EXT_CSD_WR_REL_SET, device);
		exit(1);
	}

	printf("Done setting EXT_CSD_WR_REL_SET to 0x%02x on %s\n",
		value, device);

	if (set_partitioning_setting_completed(dry_run, device, fd))
		exit(1);

	return 0;
}

int do_read_extcsd(int nargs, char **argv)
{
	__u8 ext_csd[512], ext_csd_rev, reg;
	__u32 regl;
	int fd, ret;
	char *device;
	const char *str;

	if (nargs != 2) {
		fprintf(stderr, "Usage: mmc extcsd read </path/to/mmcblkX>\n");
		exit(1);
	}

	device = argv[1];

	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	ret = read_extcsd(fd, ext_csd);
	if (ret) {
		fprintf(stderr, "Could not read EXT_CSD from %s\n", device);
		exit(1);
	}

	ext_csd_rev = ext_csd[EXT_CSD_REV];

	switch (ext_csd_rev) {
	case 8:
		str = "5.1";
		break;
	case 7:
		str = "5.0";
		break;
	case 6:
		str = "4.5";
		break;
	case 5:
		str = "4.41";
		break;
	case 3:
		str = "4.3";
		break;
	case 2:
		str = "4.2";
		break;
	case 1:
		str = "4.1";
		break;
	case 0:
		str = "4.0";
		break;
	default:
		goto out_free;
	}
	printf("=============================================\n");
	printf("  Extended CSD rev 1.%d (MMC %s)\n", ext_csd_rev, str);
	printf("=============================================\n\n");

	if (ext_csd_rev < 3)
		goto out_free; /* No ext_csd */

	/* Parse the Extended CSD registers.
	 * Reserved bit should be read as "0" in case of spec older
	 * than A441.
	 */
	reg = ext_csd[EXT_CSD_S_CMD_SET];
	printf("Card Supported Command sets [S_CMD_SET: 0x%02x]\n", reg);
	if (!reg)
		printf(" - Standard MMC command sets\n");

	reg = ext_csd[EXT_CSD_HPI_FEATURE];
	printf("HPI Features [HPI_FEATURE: 0x%02x]: ", reg);
	if (reg & EXT_CSD_HPI_SUPP) {
		if (reg & EXT_CSD_HPI_IMPL)
			printf("implementation based on CMD12\n");
		else
			printf("implementation based on CMD13\n");
	}

	printf("Background operations support [BKOPS_SUPPORT: 0x%02x]\n",
		ext_csd[502]);

	if (ext_csd_rev >= 6) {
		printf("Max Packet Read Cmd [MAX_PACKED_READS: 0x%02x]\n",
			ext_csd[501]);
		printf("Max Packet Write Cmd [MAX_PACKED_WRITES: 0x%02x]\n",
			ext_csd[500]);
		printf("Data TAG support [DATA_TAG_SUPPORT: 0x%02x]\n",
			ext_csd[499]);

		printf("Data TAG Unit Size [TAG_UNIT_SIZE: 0x%02x]\n",
			ext_csd[498]);
		printf("Tag Resources Size [TAG_RES_SIZE: 0x%02x]\n",
			ext_csd[497]);
		printf("Context Management Capabilities"
			" [CONTEXT_CAPABILITIES: 0x%02x]\n", ext_csd[496]);
		printf("Large Unit Size [LARGE_UNIT_SIZE_M1: 0x%02x]\n",
			ext_csd[495]);
		printf("Extended partition attribute support"
			" [EXT_SUPPORT: 0x%02x]\n", ext_csd[494]);
		printf("Generic CMD6 Timer [GENERIC_CMD6_TIME: 0x%02x]\n",
			ext_csd[248]);
		printf("Power off notification [POWER_OFF_LONG_TIME: 0x%02x]\n",
			ext_csd[247]);
		printf("Cache Size [CACHE_SIZE] is %d KiB\n",
			(ext_csd[249] << 0 | (ext_csd[250] << 8) |
			(ext_csd[251] << 16) | (ext_csd[252] << 24)) / 8);
	}

	/* A441: Reserved [501:247]
	    A43: reserved [246:229] */
	if (ext_csd_rev >= 5) {
		printf("Background operations status"
			" [BKOPS_STATUS: 0x%02x]\n", ext_csd[246]);

		/* CORRECTLY_PRG_SECTORS_NUM [245:242] TODO */

		printf("1st Initialisation Time after programmed sector"
			" [INI_TIMEOUT_AP: 0x%02x]\n", ext_csd[241]);

		/* A441: reserved [240] */
		printf("Power class for 52MHz, DDR at 3.6V"
			" [PWR_CL_DDR_52_360: 0x%02x]\n", ext_csd[239]);
		printf("Power class for 52MHz, DDR at 1.95V"
			" [PWR_CL_DDR_52_195: 0x%02x]\n", ext_csd[238]);

		/* A441: reserved [237-236] */

		if (ext_csd_rev >= 6) {
			printf("Power class for 200MHz at 3.6V"
				" [PWR_CL_200_360: 0x%02x]\n", ext_csd[237]);
			printf("Power class for 200MHz, at 1.95V"
				" [PWR_CL_200_195: 0x%02x]\n", ext_csd[236]);
		}
		printf("Minimum Performance for 8bit at 52MHz in DDR mode:\n");
		printf(" [MIN_PERF_DDR_W_8_52: 0x%02x]\n", ext_csd[235]);
		printf(" [MIN_PERF_DDR_R_8_52: 0x%02x]\n", ext_csd[234]);
		/* A441: reserved [233] */
		printf("TRIM Multiplier [TRIM_MULT: 0x%02x]\n", ext_csd[232]);
		printf("Secure Feature support [SEC_FEATURE_SUPPORT: 0x%02x]\n",
			ext_csd[231]);
	}
	if (ext_csd_rev == 5) { /* Obsolete in 4.5 */
		printf("Secure Erase Multiplier [SEC_ERASE_MULT: 0x%02x]\n",
			ext_csd[230]);
		printf("Secure TRIM Multiplier [SEC_TRIM_MULT: 0x%02x]\n",
			ext_csd[229]);
	}
	reg = ext_csd[EXT_CSD_BOOT_INFO];
	printf("Boot Information [BOOT_INFO: 0x%02x]\n", reg);
	if (reg & EXT_CSD_BOOT_INFO_ALT)
		printf(" Device supports alternative boot method\n");
	if (reg & EXT_CSD_BOOT_INFO_DDR_DDR)
		printf(" Device supports dual data rate during boot\n");
	if (reg & EXT_CSD_BOOT_INFO_HS_MODE)
		printf(" Device supports high speed timing during boot\n");

	/* A441/A43: reserved [227] */
	printf("Boot partition size [BOOT_SIZE_MULTI: 0x%02x]\n", ext_csd[226]);
	printf("Access size [ACC_SIZE: 0x%02x]\n", ext_csd[225]);

	reg = get_hc_erase_grp_size(ext_csd);
	printf("High-capacity erase unit size [HC_ERASE_GRP_SIZE: 0x%02x]\n",
		reg);
	printf(" i.e. %u KiB\n", 512 * reg);

	printf("High-capacity erase timeout [ERASE_TIMEOUT_MULT: 0x%02x]\n",
		ext_csd[223]);
	printf("Reliable write sector count [REL_WR_SEC_C: 0x%02x]\n",
		ext_csd[222]);

	reg = get_hc_wp_grp_size(ext_csd);
	printf("High-capacity W protect group size [HC_WP_GRP_SIZE: 0x%02x]\n",
		reg);
	printf(" i.e. %lu KiB\n", 512l * get_hc_erase_grp_size(ext_csd) * reg);

	printf("Sleep current (VCC) [S_C_VCC: 0x%02x]\n", ext_csd[220]);
	printf("Sleep current (VCCQ) [S_C_VCCQ: 0x%02x]\n", ext_csd[219]);
	/* A441/A43: reserved [218] */
	printf("Sleep/awake timeout [S_A_TIMEOUT: 0x%02x]\n", ext_csd[217]);
	/* A441/A43: reserved [216] */

	unsigned int sectors =	get_sector_count(ext_csd);
	printf("Sector Count [SEC_COUNT: 0x%08x]\n", sectors);
	if (is_blockaddresed(ext_csd))
		printf(" Device is block-addressed\n");
	else
		printf(" Device is NOT block-addressed\n");

	/* A441/A43: reserved [211] */
	printf("Minimum Write Performance for 8bit:\n");
	printf(" [MIN_PERF_W_8_52: 0x%02x]\n", ext_csd[210]);
	printf(" [MIN_PERF_R_8_52: 0x%02x]\n", ext_csd[209]);
	printf(" [MIN_PERF_W_8_26_4_52: 0x%02x]\n", ext_csd[208]);
	printf(" [MIN_PERF_R_8_26_4_52: 0x%02x]\n", ext_csd[207]);
	printf("Minimum Write Performance for 4bit:\n");
	printf(" [MIN_PERF_W_4_26: 0x%02x]\n", ext_csd[206]);
	printf(" [MIN_PERF_R_4_26: 0x%02x]\n", ext_csd[205]);
	/* A441/A43: reserved [204] */
	printf("Power classes registers:\n");
	printf(" [PWR_CL_26_360: 0x%02x]\n", ext_csd[203]);
	printf(" [PWR_CL_52_360: 0x%02x]\n", ext_csd[202]);
	printf(" [PWR_CL_26_195: 0x%02x]\n", ext_csd[201]);
	printf(" [PWR_CL_52_195: 0x%02x]\n", ext_csd[200]);

	/* A43: reserved [199:198] */
	if (ext_csd_rev >= 5) {
		printf("Partition switching timing "
			"[PARTITION_SWITCH_TIME: 0x%02x]\n", ext_csd[199]);
		printf("Out-of-interrupt busy timing"
			" [OUT_OF_INTERRUPT_TIME: 0x%02x]\n", ext_csd[198]);
	}

	/* A441/A43: reserved	[197] [195] [193] [190] [188]
	 * [186] [184] [182] [180] [176] */

	if (ext_csd_rev >= 6)
		printf("I/O Driver Strength [DRIVER_STRENGTH: 0x%02x]\n",
			ext_csd[197]);

	/* DEVICE_TYPE in A45, CARD_TYPE in A441 */
	reg = ext_csd[196];
	printf("Card Type [CARD_TYPE: 0x%02x]\n", reg);
	if (reg & 0x80) printf(" HS400 Dual Data Rate eMMC @200MHz 1.2VI/O\n");
	if (reg & 0x40) printf(" HS400 Dual Data Rate eMMC @200MHz 1.8VI/O\n");
	if (reg & 0x20) printf(" HS200 Single Data Rate eMMC @200MHz 1.2VI/O\n");
	if (reg & 0x10) printf(" HS200 Single Data Rate eMMC @200MHz 1.8VI/O\n");
	if (reg & 0x08) printf(" HS Dual Data Rate eMMC @52MHz 1.2VI/O\n");
	if (reg & 0x04)	printf(" HS Dual Data Rate eMMC @52MHz 1.8V or 3VI/O\n");
	if (reg & 0x02)	printf(" HS eMMC @52MHz - at rated device voltage(s)\n");
	if (reg & 0x01) printf(" HS eMMC @26MHz - at rated device voltage(s)\n");

	printf("CSD structure version [CSD_STRUCTURE: 0x%02x]\n", ext_csd[194]);
	/* ext_csd_rev = ext_csd[EXT_CSD_REV] (already done!!!) */
	printf("Command set [CMD_SET: 0x%02x]\n", ext_csd[191]);
	printf("Command set revision [CMD_SET_REV: 0x%02x]\n", ext_csd[189]);
	printf("Power class [POWER_CLASS: 0x%02x]\n", ext_csd[187]);
	printf("High-speed interface timing [HS_TIMING: 0x%02x]\n",
		ext_csd[185]);
	if (ext_csd_rev >= 8)
		printf("Enhanced Strobe mode [STROBE_SUPPORT: 0x%02x]\n",
			ext_csd[184]);
	/* bus_width: ext_csd[183] not readable */
	printf("Erased memory content [ERASED_MEM_CONT: 0x%02x]\n",
		ext_csd[181]);
	reg = ext_csd[EXT_CSD_BOOT_CFG];
	printf("Boot configuration bytes [PARTITION_CONFIG: 0x%02x]\n", reg);
	switch ((reg & EXT_CSD_BOOT_CFG_EN)>>3) {
	case 0x0:
		printf(" Not boot enable\n");
		break;
	case 0x1:
		printf(" Boot Partition 1 enabled\n");
		break;
	case 0x2:
		printf(" Boot Partition 2 enabled\n");
		break;
	case 0x7:
		printf(" User Area Enabled for boot\n");
		break;
	}
	switch (reg & EXT_CSD_BOOT_CFG_ACC) {
	case 0x0:
		printf(" No access to boot partition\n");
		break;
	case 0x1:
		printf(" R/W Boot Partition 1\n");
		break;
	case 0x2:
		printf(" R/W Boot Partition 2\n");
		break;
	case 0x3:
		printf(" R/W Replay Protected Memory Block (RPMB)\n");
		break;
	default:
		printf(" Access to General Purpose partition %d\n",
			(reg & EXT_CSD_BOOT_CFG_ACC) - 3);
		break;
	}

	printf("Boot config protection [BOOT_CONFIG_PROT: 0x%02x]\n",
		ext_csd[178]);
	printf("Boot bus Conditions [BOOT_BUS_CONDITIONS: 0x%02x]\n",
		ext_csd[177]);
	printf("High-density erase group definition"
		" [ERASE_GROUP_DEF: 0x%02x]\n", ext_csd[EXT_CSD_ERASE_GROUP_DEF]);

	print_writeprotect_boot_status(ext_csd);

	if (ext_csd_rev >= 5) {
		/* A441]: reserved [172] */
		printf("User area write protection register"
			" [USER_WP]: 0x%02x\n", ext_csd[171]);
		/* A441]: reserved [170] */
		printf("FW configuration [FW_CONFIG]: 0x%02x\n", ext_csd[169]);
		printf("RPMB Size [RPMB_SIZE_MULT]: 0x%02x\n", ext_csd[168]);

		reg = ext_csd[EXT_CSD_WR_REL_SET];
		const char * const fast = "existing data is at risk if a power "
				"failure occurs during a write operation";
		const char * const reliable = "the device protects existing "
				"data if a power failure occurs during a write "
				"operation";
		printf("Write reliability setting register"
			" [WR_REL_SET]: 0x%02x\n", reg);

		printf(" user area: %s\n", (reg & (1<<0)) ? reliable : fast);
		int i;
		for (i = 1; i <= 4; i++) {
			printf(" partition %d: %s\n", i,
				(reg & (1<<i)) ? reliable : fast);
		}

		reg = ext_csd[EXT_CSD_WR_REL_PARAM];
		printf("Write reliability parameter register"
			" [WR_REL_PARAM]: 0x%02x\n", reg);
		if (reg & 0x01)
			printf(" Device supports writing EXT_CSD_WR_REL_SET\n");
		if (reg & 0x04)
			printf(" Device supports the enhanced def. of reliable "
				"write\n");

		/* sanitize_start ext_csd[165]]: not readable
		 * bkops_start ext_csd[164]]: only writable */
		printf("Enable background operations handshake"
			" [BKOPS_EN]: 0x%02x\n", ext_csd[163]);
		printf("H/W reset function"
			" [RST_N_FUNCTION]: 0x%02x\n", ext_csd[162]);
		printf("HPI management [HPI_MGMT]: 0x%02x\n", ext_csd[161]);
		reg = ext_csd[EXT_CSD_PARTITIONING_SUPPORT];
		printf("Partitioning Support [PARTITIONING_SUPPORT]: 0x%02x\n",
			reg);
		if (reg & EXT_CSD_PARTITIONING_EN)
			printf(" Device support partitioning feature\n");
		else
			printf(" Device NOT support partitioning feature\n");
		if (reg & EXT_CSD_ENH_ATTRIBUTE_EN)
			printf(" Device can have enhanced tech.\n");
		else
			printf(" Device cannot have enhanced tech.\n");

		regl = (ext_csd[EXT_CSD_MAX_ENH_SIZE_MULT_2] << 16) |
			(ext_csd[EXT_CSD_MAX_ENH_SIZE_MULT_1] << 8) |
			ext_csd[EXT_CSD_MAX_ENH_SIZE_MULT_0];

		printf("Max Enhanced Area Size [MAX_ENH_SIZE_MULT]: 0x%06x\n",
			   regl);
		unsigned int wp_sz = get_hc_wp_grp_size(ext_csd);
		unsigned int erase_sz = get_hc_erase_grp_size(ext_csd);
		printf(" i.e. %lu KiB\n", 512l * regl * wp_sz * erase_sz);

		printf("Partitions attribute [PARTITIONS_ATTRIBUTE]: 0x%02x\n",
			ext_csd[EXT_CSD_PARTITIONS_ATTRIBUTE]);
		reg = ext_csd[EXT_CSD_PARTITION_SETTING_COMPLETED];
		printf("Partitioning Setting"
			" [PARTITION_SETTING_COMPLETED]: 0x%02x\n",
			reg);
		if (reg)
			printf(" Device partition setting complete\n");
		else
			printf(" Device partition setting NOT complete\n");

		printf("General Purpose Partition Size\n"
			" [GP_SIZE_MULT_4]: 0x%06x\n", (ext_csd[154] << 16) |
			(ext_csd[153] << 8) | ext_csd[152]);
		printf(" [GP_SIZE_MULT_3]: 0x%06x\n", (ext_csd[151] << 16) |
			   (ext_csd[150] << 8) | ext_csd[149]);
		printf(" [GP_SIZE_MULT_2]: 0x%06x\n", (ext_csd[148] << 16) |
			   (ext_csd[147] << 8) | ext_csd[146]);
		printf(" [GP_SIZE_MULT_1]: 0x%06x\n", (ext_csd[145] << 16) |
			   (ext_csd[144] << 8) | ext_csd[143]);

		regl =	(ext_csd[EXT_CSD_ENH_SIZE_MULT_2] << 16) |
			(ext_csd[EXT_CSD_ENH_SIZE_MULT_1] << 8) |
			ext_csd[EXT_CSD_ENH_SIZE_MULT_0];
		printf("Enhanced User Data Area Size"
			" [ENH_SIZE_MULT]: 0x%06x\n", regl);
		printf(" i.e. %lu KiB\n", 512l * regl *
		       get_hc_erase_grp_size(ext_csd) *
		       get_hc_wp_grp_size(ext_csd));

		regl =	(ext_csd[EXT_CSD_ENH_START_ADDR_3] << 24) |
			(ext_csd[EXT_CSD_ENH_START_ADDR_2] << 16) |
			(ext_csd[EXT_CSD_ENH_START_ADDR_1] << 8) |
			ext_csd[EXT_CSD_ENH_START_ADDR_0];
		printf("Enhanced User Data Start Address"
			" [ENH_START_ADDR]: 0x%08x\n", regl);
		printf(" i.e. %llu bytes offset\n", (is_blockaddresed(ext_csd) ?
				512ll : 1ll) * regl);

		/* A441]: reserved [135] */
		printf("Bad Block Management mode"
			" [SEC_BAD_BLK_MGMNT]: 0x%02x\n", ext_csd[134]);
		/* A441: reserved [133:0] */
	}
	/* B45 */
	if (ext_csd_rev >= 6) {
		int j;
		/* tcase_support ext_csd[132] not readable */
		printf("Periodic Wake-up [PERIODIC_WAKEUP]: 0x%02x\n",
			ext_csd[131]);
		printf("Program CID/CSD in DDR mode support"
			" [PROGRAM_CID_CSD_DDR_SUPPORT]: 0x%02x\n",
			   ext_csd[130]);

		for (j = 127; j >= 64; j--)
			printf("Vendor Specific Fields"
				" [VENDOR_SPECIFIC_FIELD[%d]]: 0x%02x\n",
				j, ext_csd[j]);

		printf("Native sector size [NATIVE_SECTOR_SIZE]: 0x%02x\n",
			ext_csd[63]);
		printf("Sector size emulation [USE_NATIVE_SECTOR]: 0x%02x\n",
			ext_csd[62]);
		printf("Sector size [DATA_SECTOR_SIZE]: 0x%02x\n", ext_csd[61]);
		printf("1st initialization after disabling sector"
			" size emulation [INI_TIMEOUT_EMU]: 0x%02x\n",
			ext_csd[60]);
		printf("Class 6 commands control [CLASS_6_CTRL]: 0x%02x\n",
			ext_csd[59]);
		printf("Number of addressed group to be Released"
			"[DYNCAP_NEEDED]: 0x%02x\n", ext_csd[58]);
		printf("Exception events control"
			" [EXCEPTION_EVENTS_CTRL]: 0x%04x\n",
			(ext_csd[57] << 8) | ext_csd[56]);
		printf("Exception events status"
			"[EXCEPTION_EVENTS_STATUS]: 0x%04x\n",
			(ext_csd[55] << 8) | ext_csd[54]);
		printf("Extended Partitions Attribute"
			" [EXT_PARTITIONS_ATTRIBUTE]: 0x%04x\n",
			(ext_csd[53] << 8) | ext_csd[52]);

		for (j = 51; j >= 37; j--)
			printf("Context configuration"
				" [CONTEXT_CONF[%d]]: 0x%02x\n", j, ext_csd[j]);

		printf("Packed command status"
			" [PACKED_COMMAND_STATUS]: 0x%02x\n", ext_csd[36]);
		printf("Packed command failure index"
			" [PACKED_FAILURE_INDEX]: 0x%02x\n", ext_csd[35]);
		printf("Power Off Notification"
			" [POWER_OFF_NOTIFICATION]: 0x%02x\n", ext_csd[34]);
		printf("Control to turn the Cache ON/OFF"
			" [CACHE_CTRL]: 0x%02x\n", ext_csd[33]);
		/* flush_cache ext_csd[32] not readable */
		printf("Control to turn the Cache Barrier ON/OFF"
			" [BARRIER_CTRL]: 0x%02x\n", ext_csd[31]);
		/*Reserved [30:0] */
	}

	if (ext_csd_rev >= 7) {
		printf("eMMC Firmware Version: %.8s\n", (char*)&ext_csd[EXT_CSD_FIRMWARE_VERSION]);
		printf("eMMC Life Time Estimation A [EXT_CSD_DEVICE_LIFE_TIME_EST_TYP_A]: 0x%02x\n",
			ext_csd[EXT_CSD_DEVICE_LIFE_TIME_EST_TYP_A]);
		printf("eMMC Life Time Estimation B [EXT_CSD_DEVICE_LIFE_TIME_EST_TYP_B]: 0x%02x\n",
			ext_csd[EXT_CSD_DEVICE_LIFE_TIME_EST_TYP_B]);
		printf("eMMC Pre EOL information [EXT_CSD_PRE_EOL_INFO]: 0x%02x\n",
			ext_csd[EXT_CSD_PRE_EOL_INFO]);
		reg = ext_csd[EXT_CSD_SECURE_REMOVAL_TYPE];
		printf("Secure Removal Type [SECURE_REMOVAL_TYPE]: 0x%02x\n", reg);
		printf(" information is configured to be removed ");
		/* Bit [5:4]: Configure Secure Removal Type */
		switch ((reg & EXT_CSD_CONFIG_SECRM_TYPE) >> 4) {
			case 0x0:
				printf("by an erase of the physical memory\n");
				break;
			case 0x1:
				printf("by an overwriting the addressed locations"
				       " with a character followed by an erase\n");
				break;
			case 0x2:
				printf("by an overwriting the addressed locations"
				       " with a character, its complement, then a random character\n");
				break;
			case 0x3:
				printf("using a vendor defined\n");
				break;
		}
		/* Bit [3:0]: Supported Secure Removal Type */
		printf(" Supported Secure Removal Type:\n");
		if (reg & 0x01)
			printf("  information removed by an erase of the physical memory\n");
		if (reg & 0x02)
			printf("  information removed by an overwriting the addressed locations"
			       " with a character followed by an erase\n");
		if (reg & 0x04)
			printf("  information removed by an overwriting the addressed locations"
			       " with a character, its complement, then a random character\n");
		if (reg & 0x08)
			printf("  information removed using a vendor defined\n");
	}

	if (ext_csd_rev >= 8) {
		printf("Command Queue Support [CMDQ_SUPPORT]: 0x%02x\n",
		       ext_csd[EXT_CSD_CMDQ_SUPPORT]);
		printf("Command Queue Depth [CMDQ_DEPTH]: %u\n",
		       (ext_csd[EXT_CSD_CMDQ_DEPTH] & 0x1f) + 1);
		printf("Command Enabled [CMDQ_MODE_EN]: 0x%02x\n",
		       ext_csd[EXT_CSD_CMDQ_MODE_EN]);
		printf("Note: CMDQ_MODE_EN may not indicate the runtime CMDQ ON or OFF.\n"
		       "Please check sysfs node '/sys/devices/.../mmc_host/mmcX/mmcX:XXXX/cmdq_en'\n");
	}
out_free:
	return ret;
}

int do_write_extcsd(int nargs, char **argv)
{
	int fd, ret;
	int offset, value;
	char *device;

	if (nargs != 4) {
		fprintf(stderr, "Usage: mmc extcsd write <offset> <value> </path/to/mmcblkX>\n");
		exit(1);
	}

	offset = strtol(argv[1], NULL, 0);
	value  = strtol(argv[2], NULL, 0);
	device = argv[3];

	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	ret = write_extcsd_value(fd, offset, value, 0);
	if (ret) {
		fprintf(stderr,
			"Could not write 0x%02x to EXT_CSD[%d] in %s\n",
			value, offset, device);
		exit(1);
	}

	return ret;
}

int do_sanitize(int nargs, char **argv)
{
	int fd, ret;
	char *device;
	unsigned int timeout = 0;

	if (nargs != 2 && nargs != 3) {
		fprintf(stderr, "Usage: mmc sanitize </path/to/mmcblkX> [timeout_in_ms]\n");
		exit(1);
	}

	if (nargs == 3)
		timeout = strtol(argv[2], NULL, 10);

	device = argv[1];

	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	ret = write_extcsd_value(fd, EXT_CSD_SANITIZE_START, 1, timeout);
	if (ret) {
		fprintf(stderr, "Could not write 0x%02x to EXT_CSD[%d] in %s\n",
			1, EXT_CSD_SANITIZE_START, device);
		exit(1);
	}

	close(fd);
	return ret;

}

#define DO_IO(func, fd, buf, nbyte)					\
	({												\
		ssize_t ret = 0, r;							\
		do {										\
			r = func(fd, buf + ret, nbyte - ret);	\
			if (r < 0 && errno != EINTR) {			\
				ret = -1;							\
				break;								\
			}										\
			else if (r > 0)							\
				ret += r;							\
		} while (r != 0 && (size_t)ret != nbyte);	\
													\
		ret;										\
	})

#define RPMB_MULTI_CMD_MAX_CMDS 3

enum rpmb_op_type {
	MMC_RPMB_WRITE_KEY = 0x01,
	MMC_RPMB_READ_CNT  = 0x02,
	MMC_RPMB_WRITE     = 0x03,
	MMC_RPMB_READ      = 0x04,

	/* For internal usage only, do not use it directly */
	MMC_RPMB_READ_RESP = 0x05
};

struct rpmb_frame {
	u_int8_t  stuff[196];
	u_int8_t  key_mac[32];
	u_int8_t  data[256];
	u_int8_t  nonce[16];
	u_int32_t write_counter;
	u_int16_t addr;
	u_int16_t block_count;
	u_int16_t result;
	u_int16_t req_resp;
};

static inline void set_single_cmd(struct mmc_ioc_cmd *ioc, __u32 opcode,
				  int write_flag, unsigned int blocks,
				  __u32 arg)
{
	ioc->opcode = opcode;
	ioc->write_flag = write_flag;
	ioc->arg = arg;
	ioc->blksz = 512;
	ioc->blocks = blocks;
	ioc->flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;
}

/* Performs RPMB operation.
 *
 * @fd: RPMB device on which we should perform ioctl command
 * @frame_in: input RPMB frame, should be properly inited
 * @frame_out: output (result) RPMB frame. Caller is responsible for checking
 *             result and req_resp for output frame.
 * @out_cnt: count of outer frames. Used only for multiple blocks reading,
 *           in the other cases -EINVAL will be returned.
 */
static int do_rpmb_op(int fd,
					  const struct rpmb_frame *frame_in,
					  struct rpmb_frame *frame_out,
					  unsigned int out_cnt)
{
	int err;
	u_int16_t rpmb_type;
	struct mmc_ioc_multi_cmd *mioc;
	struct mmc_ioc_cmd *ioc;
	struct rpmb_frame frame_status;

	memset(&frame_status, 0, sizeof(frame_status));

	if (!frame_in || !frame_out || !out_cnt)
		return -EINVAL;

	/* prepare arguments for MMC_IOC_MULTI_CMD ioctl */
	mioc = (struct mmc_ioc_multi_cmd *)
		calloc(1, sizeof (struct mmc_ioc_multi_cmd) +
		       RPMB_MULTI_CMD_MAX_CMDS * sizeof (struct mmc_ioc_cmd));
	if (!mioc) {
		return -ENOMEM;
	}

	rpmb_type = be16toh(frame_in->req_resp);

	switch(rpmb_type) {
	case MMC_RPMB_WRITE:
	case MMC_RPMB_WRITE_KEY:
		if (out_cnt != 1) {
			err = -EINVAL;
			goto out;
		}

		mioc->num_of_cmds = 3;

		/* Write request */
		ioc = &mioc->cmds[0];
		set_single_cmd(ioc, MMC_WRITE_MULTIPLE_BLOCK, (1 << 31) | 1, 1, 0);
		mmc_ioc_cmd_set_data((*ioc), frame_in);

		/* Result request */
		ioc = &mioc->cmds[1];
		frame_status.req_resp = htobe16(MMC_RPMB_READ_RESP);
		set_single_cmd(ioc, MMC_WRITE_MULTIPLE_BLOCK, 1, 1, 0);
		mmc_ioc_cmd_set_data((*ioc), &frame_status);

		/* Get response */
		ioc = &mioc->cmds[2];
		set_single_cmd(ioc, MMC_READ_MULTIPLE_BLOCK, 0, 1, 0);
		mmc_ioc_cmd_set_data((*ioc), frame_out);

		break;
	case MMC_RPMB_READ_CNT:
		if (out_cnt != 1) {
			err = -EINVAL;
			goto out;
		}
		/* fall through */

	case MMC_RPMB_READ:
		mioc->num_of_cmds = 2;

		/* Read request */
		ioc = &mioc->cmds[0];
		set_single_cmd(ioc, MMC_WRITE_MULTIPLE_BLOCK, 1, 1, 0);
		mmc_ioc_cmd_set_data((*ioc), frame_in);

		/* Get response */
		ioc = &mioc->cmds[1];
		set_single_cmd(ioc, MMC_READ_MULTIPLE_BLOCK, 0, out_cnt, 0);
		mmc_ioc_cmd_set_data((*ioc), frame_out);

		break;
	default:
		err = -EINVAL;
		goto out;
	}

	err = ioctl(fd, MMC_IOC_MULTI_CMD, mioc);

out:
	free(mioc);
	return err;
}

int do_rpmb_write_key(int nargs, char **argv)
{
	int ret, dev_fd, key_fd;
	struct rpmb_frame frame_in = {
		.req_resp = htobe16(MMC_RPMB_WRITE_KEY)
	}, frame_out;

	if (nargs != 3) {
		fprintf(stderr, "Usage: mmc rpmb write-key </path/to/mmcblkXrpmb> </path/to/key>\n");
		exit(1);
	}

	dev_fd = open(argv[1], O_RDWR);
	if (dev_fd < 0) {
		perror("device open");
		exit(1);
	}

	if (0 == strcmp(argv[2], "-"))
		key_fd = STDIN_FILENO;
	else {
		key_fd = open(argv[2], O_RDONLY);
		if (key_fd < 0) {
			perror("can't open key file");
			exit(1);
		}
	}

	/* Read the auth key */
	ret = DO_IO(read, key_fd, frame_in.key_mac, sizeof(frame_in.key_mac));
	if (ret < 0) {
		perror("read the key");
		exit(1);
	} else if (ret != sizeof(frame_in.key_mac)) {
		printf("Auth key must be %lu bytes length, but we read only %d, exit\n",
			   (unsigned long)sizeof(frame_in.key_mac),
			   ret);
		exit(1);
	}

	/* Execute RPMB op */
	ret = do_rpmb_op(dev_fd, &frame_in, &frame_out, 1);
	if (ret != 0) {
		perror("RPMB ioctl failed");
		exit(1);
	}

	/* Check RPMB response */
	if (frame_out.result != 0) {
		printf("RPMB operation failed, retcode 0x%04x\n",
			   be16toh(frame_out.result));
		exit(1);
	}

	close(dev_fd);
	if (key_fd != STDIN_FILENO)
		close(key_fd);

	return ret;
}

int rpmb_read_counter(int dev_fd, unsigned int *cnt)
{
	int ret;
	struct rpmb_frame frame_in = {
		.req_resp = htobe16(MMC_RPMB_READ_CNT)
	}, frame_out;

	/* Execute RPMB op */
	ret = do_rpmb_op(dev_fd, &frame_in, &frame_out, 1);
	if (ret != 0) {
		perror("RPMB ioctl failed");
		exit(1);
	}

	/* Check RPMB response */
	if (frame_out.result != 0) {
		*cnt = 0;
		return be16toh(frame_out.result);
	}

	*cnt = be32toh(frame_out.write_counter);

	return 0;
}

int do_rpmb_read_counter(int nargs, char **argv)
{
	int ret, dev_fd;
	unsigned int cnt;

	if (nargs != 2) {
		fprintf(stderr, "Usage: mmc rpmb read-counter </path/to/mmcblkXrpmb>\n");
		exit(1);
	}

	dev_fd = open(argv[1], O_RDWR);
	if (dev_fd < 0) {
		perror("device open");
		exit(1);
	}

	ret = rpmb_read_counter(dev_fd, &cnt);

	/* Check RPMB response */
	if (ret != 0) {
		printf("RPMB operation failed, retcode 0x%04x\n", ret);
		exit(1);
	}

	close(dev_fd);

	printf("Counter value: 0x%08x\n", cnt);

	return ret;
}

int do_rpmb_read_block(int nargs, char **argv)
{
	int i, ret, dev_fd, data_fd, key_fd = -1;
	uint16_t addr;
	/*
	 * for reading RPMB, number of blocks is set by CMD23 only, the packet
	 * frame field for that is set to 0. So, the type is not u16 but uint!
	 */
	unsigned int blocks_cnt;
	unsigned char key[32];
	struct rpmb_frame frame_in = {
		.req_resp    = htobe16(MMC_RPMB_READ),
	}, *frame_out_p;

	if (nargs != 5 && nargs != 6) {
		fprintf(stderr, "Usage: mmc rpmb read-block </path/to/mmcblkXrpmb> <address> <blocks count> </path/to/output_file> [/path/to/key]\n");
		exit(1);
	}

	dev_fd = open(argv[1], O_RDWR);
	if (dev_fd < 0) {
		perror("device open");
		exit(1);
	}

	/* Get block address */
	errno = 0;
	addr = strtol(argv[2], NULL, 0);
	if (errno) {
		perror("incorrect address");
		exit(1);
	}
	frame_in.addr = htobe16(addr);

	/* Get blocks count */
	errno = 0;
	blocks_cnt = strtol(argv[3], NULL, 0);
	if (errno) {
		perror("incorrect blocks count");
		exit(1);
	}

	if (!blocks_cnt) {
		printf("please, specify valid blocks count number\n");
		exit(1);
	}

	frame_out_p = calloc(sizeof(*frame_out_p), blocks_cnt);
	if (!frame_out_p) {
		printf("can't allocate memory for RPMB outer frames\n");
		exit(1);
	}

	/* Write 256b data */
	if (0 == strcmp(argv[4], "-"))
		data_fd = STDOUT_FILENO;
	else {
		data_fd = open(argv[4], O_WRONLY | O_CREAT | O_APPEND,
					   S_IRUSR | S_IWUSR);
		if (data_fd < 0) {
			perror("can't open output file");
			exit(1);
		}
	}

	/* Key is specified */
	if (nargs == 6) {
		if (0 == strcmp(argv[5], "-"))
			key_fd = STDIN_FILENO;
		else {
			key_fd = open(argv[5], O_RDONLY);
			if (key_fd < 0) {
				perror("can't open input key file");
				exit(1);
			}
		}

		ret = DO_IO(read, key_fd, key, sizeof(key));
		if (ret < 0) {
			perror("read the key data");
			exit(1);
		} else if (ret != sizeof(key)) {
			printf("Data must be %lu bytes length, but we read only %d, exit\n",
				   (unsigned long)sizeof(key),
				   ret);
			exit(1);
		}
	}

	/* Execute RPMB op */
	ret = do_rpmb_op(dev_fd, &frame_in, frame_out_p, blocks_cnt);
	if (ret != 0) {
		perror("RPMB ioctl failed");
		exit(1);
	}

	/* Check RPMB response */
	if (frame_out_p[blocks_cnt - 1].result != 0) {
		printf("RPMB operation failed, retcode 0x%04x\n",
			   be16toh(frame_out_p[blocks_cnt - 1].result));
		exit(1);
	}

	/* Do we have to verify data against key? */
	if (nargs == 6) {
		unsigned char mac[32];
		hmac_sha256_ctx ctx;
		struct rpmb_frame *frame_out = NULL;

		hmac_sha256_init(&ctx, key, sizeof(key));
		for (i = 0; i < blocks_cnt; i++) {
			frame_out = &frame_out_p[i];
			hmac_sha256_update(&ctx, frame_out->data,
							   sizeof(*frame_out) -
								   offsetof(struct rpmb_frame, data));
		}

		hmac_sha256_final(&ctx, mac, sizeof(mac));

		/* Impossible */
		assert(frame_out);

		/* Compare calculated MAC and MAC from last frame */
		if (memcmp(mac, frame_out->key_mac, sizeof(mac))) {
			printf("RPMB MAC missmatch\n");
			exit(1);
		}
	}

	/* Write data */
	for (i = 0; i < blocks_cnt; i++) {
		struct rpmb_frame *frame_out = &frame_out_p[i];
		ret = DO_IO(write, data_fd, frame_out->data, sizeof(frame_out->data));
		if (ret < 0) {
			perror("write the data");
			exit(1);
		} else if (ret != sizeof(frame_out->data)) {
			printf("Data must be %lu bytes length, but we wrote only %d, exit\n",
				   (unsigned long)sizeof(frame_out->data),
				   ret);
			exit(1);
		}
	}

	free(frame_out_p);
	close(dev_fd);
	if (data_fd != STDOUT_FILENO)
		close(data_fd);
	if (key_fd != -1 && key_fd != STDIN_FILENO)
		close(key_fd);

	return ret;
}

int do_rpmb_write_block(int nargs, char **argv)
{
	int ret, dev_fd, key_fd, data_fd;
	unsigned char key[32];
	uint16_t addr;
	unsigned int cnt;
	struct rpmb_frame frame_in = {
		.req_resp    = htobe16(MMC_RPMB_WRITE),
		.block_count = htobe16(1)
	}, frame_out;

	if (nargs != 5) {
		fprintf(stderr, "Usage: mmc rpmb write-block </path/to/mmcblkXrpmb> <address> </path/to/input_file> </path/to/key>\n");
		exit(1);
	}

	dev_fd = open(argv[1], O_RDWR);
	if (dev_fd < 0) {
		perror("device open");
		exit(1);
	}

	ret = rpmb_read_counter(dev_fd, &cnt);
	/* Check RPMB response */
	if (ret != 0) {
		printf("RPMB read counter operation failed, retcode 0x%04x\n", ret);
		exit(1);
	}
	frame_in.write_counter = htobe32(cnt);

	/* Get block address */
	errno = 0;
	addr = strtol(argv[2], NULL, 0);
	if (errno) {
		perror("incorrect address");
		exit(1);
	}
	frame_in.addr = htobe16(addr);

	/* Read 256b data */
	if (0 == strcmp(argv[3], "-"))
		data_fd = STDIN_FILENO;
	else {
		data_fd = open(argv[3], O_RDONLY);
		if (data_fd < 0) {
			perror("can't open input file");
			exit(1);
		}
	}

	ret = DO_IO(read, data_fd, frame_in.data, sizeof(frame_in.data));
	if (ret < 0) {
		perror("read the data");
		exit(1);
	} else if (ret != sizeof(frame_in.data)) {
		printf("Data must be %lu bytes length, but we read only %d, exit\n",
			   (unsigned long)sizeof(frame_in.data),
			   ret);
		exit(1);
	}

	/* Read the auth key */
	if (0 == strcmp(argv[4], "-"))
		key_fd = STDIN_FILENO;
	else {
		key_fd = open(argv[4], O_RDONLY);
		if (key_fd < 0) {
			perror("can't open key file");
			exit(1);
		}
	}

	ret = DO_IO(read, key_fd, key, sizeof(key));
	if (ret < 0) {
		perror("read the key");
		exit(1);
	} else if (ret != sizeof(key)) {
		printf("Auth key must be %lu bytes length, but we read only %d, exit\n",
			   (unsigned long)sizeof(key),
			   ret);
		exit(1);
	}

	/* Calculate HMAC SHA256 */
	hmac_sha256(
		key, sizeof(key),
		frame_in.data, sizeof(frame_in) - offsetof(struct rpmb_frame, data),
		frame_in.key_mac, sizeof(frame_in.key_mac));

	/* Execute RPMB op */
	ret = do_rpmb_op(dev_fd, &frame_in, &frame_out, 1);
	if (ret != 0) {
		perror("RPMB ioctl failed");
		exit(1);
	}

	/* Check RPMB response */
	if (frame_out.result != 0) {
		printf("RPMB operation failed, retcode 0x%04x\n",
			   be16toh(frame_out.result));
		exit(1);
	}

	close(dev_fd);
	if (data_fd != STDIN_FILENO)
		close(data_fd);
	if (key_fd != STDIN_FILENO)
		close(key_fd);

	return ret;
}

int do_cache_ctrl(int value, int nargs, char **argv)
{
	__u8 ext_csd[512];
	int fd, ret;
	char *device;

	if (nargs != 2) {
	       fprintf(stderr, "Usage: mmc cache enable </path/to/mmcblkX>\n");
	       exit(1);
	}

	device = argv[1];

	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	ret = read_extcsd(fd, ext_csd);
	if (ret) {
		fprintf(stderr, "Could not read EXT_CSD from %s\n", device);
		exit(1);
	}

	if (ext_csd[EXT_CSD_REV] < EXT_CSD_REV_V4_5) {
		fprintf(stderr,
			"The CACHE option is only availabe on devices >= "
			"MMC 4.5 %s\n", device);
		exit(1);
	}

	/* If the cache size is zero, this device does not have a cache */
	if (!(ext_csd[EXT_CSD_CACHE_SIZE_3] ||
			ext_csd[EXT_CSD_CACHE_SIZE_2] ||
			ext_csd[EXT_CSD_CACHE_SIZE_1] ||
			ext_csd[EXT_CSD_CACHE_SIZE_0])) {
		fprintf(stderr,
			"The CACHE option is not available on %s\n",
			device);
		exit(1);
	}
	ret = write_extcsd_value(fd, EXT_CSD_CACHE_CTRL, value, 0);
	if (ret) {
		fprintf(stderr,
			"Could not write 0x%02x to EXT_CSD[%d] in %s\n",
			value, EXT_CSD_CACHE_CTRL, device);
		exit(1);
	}

	close(fd);
	return ret;
}

int do_cache_en(int nargs, char **argv)
{
	return do_cache_ctrl(1, nargs, argv);
}

int do_cache_dis(int nargs, char **argv)
{
	return do_cache_ctrl(0, nargs, argv);
}

static int erase(int dev_fd, __u32 argin, __u32 start, __u32 end)
{
	int ret = 0;
	struct mmc_ioc_multi_cmd *multi_cmd;
	__u8 ext_csd[512];


	ret = read_extcsd(dev_fd, ext_csd);
	if (ret) {
		fprintf(stderr, "Could not read EXT_CSD\n");
		exit(1);
	}
	if (ext_csd[EXT_CSD_ERASE_GROUP_DEF] & 0x01) {
	  fprintf(stderr, "High Capacity Erase Unit Size=%d bytes\n" \
                          "High Capacity Erase Timeout=%d ms\n" \
                          "High Capacity Write Protect Group Size=%d bytes\n",
		           ext_csd[224]*0x80000,
		           ext_csd[223]*300,
                           ext_csd[221]*ext_csd[224]*0x80000);
	}

	multi_cmd = calloc(1, sizeof(struct mmc_ioc_multi_cmd) +
			   3 * sizeof(struct mmc_ioc_cmd));
	if (!multi_cmd) {
		perror("Failed to allocate memory");
		return -ENOMEM;
	}

	multi_cmd->num_of_cmds = 3;
	/* Set erase start address */
	multi_cmd->cmds[0].opcode = MMC_ERASE_GROUP_START;
	multi_cmd->cmds[0].arg = start;
	multi_cmd->cmds[0].flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_AC;
	multi_cmd->cmds[0].write_flag = 1;

	/* Set erase end address */
	multi_cmd->cmds[1].opcode = MMC_ERASE_GROUP_END;
	multi_cmd->cmds[1].arg = end;
	multi_cmd->cmds[1].flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_AC;
	multi_cmd->cmds[1].write_flag = 1;

	/* Send Erase Command */
	multi_cmd->cmds[2].opcode = MMC_ERASE;
	multi_cmd->cmds[2].arg = argin;
	multi_cmd->cmds[2].cmd_timeout_ms = 300*255*255;
	multi_cmd->cmds[2].flags = MMC_RSP_SPI_R1B | MMC_RSP_R1B | MMC_CMD_AC;
	multi_cmd->cmds[2].write_flag = 1;

	/* send erase cmd with multi-cmd */
	ret = ioctl(dev_fd, MMC_IOC_MULTI_CMD, multi_cmd);
	if (ret)
		perror("Erase multi-cmd ioctl");

	/* Does not work for SPI cards */
	if (multi_cmd->cmds[1].response[0] & R1_ERASE_PARAM) {
		fprintf(stderr, "Erase start response: 0x%08x\n",
				multi_cmd->cmds[0].response[0]);
		ret = -EIO;
	}
	if (multi_cmd->cmds[2].response[0] & R1_ERASE_SEQ_ERROR) {
		fprintf(stderr, "Erase response: 0x%08x\n",
				multi_cmd->cmds[2].response[0]);
		ret = -EIO;
	}

	free(multi_cmd);
	return ret;
}

int do_erase(int nargs, char **argv)
{
	int dev_fd, ret;
	char *print_str;
	__u8 ext_csd[512], checkup_mask = 0;
	__u32 arg, start, end;

	if (nargs != 5) {
		fprintf(stderr, "Usage: erase <type> <start addr> <end addr> </path/to/mmcblkX>\n");
		exit(1);
	}

	if (strstr(argv[2], "0x") || strstr(argv[2], "0X"))
		start = strtol(argv[2], NULL, 16);
	else
		start = strtol(argv[2], NULL, 10);

	if (strstr(argv[3], "0x") || strstr(argv[3], "0X"))
		end = strtol(argv[3], NULL, 16);
	else
		end = strtol(argv[3], NULL, 10);

	if (end < start) {
		fprintf(stderr, "erase start [0x%08x] > erase end [0x%08x]\n",
			start, end);
		exit(1);
	}

	if (strcmp(argv[1], "legacy") == 0) {
		arg = 0x00000000;
		print_str = "Legacy Erase";
	} else if (strcmp(argv[1], "discard") == 0) {
		arg = 0x00000003;
		print_str = "Discard";
	} else if (strcmp(argv[1], "secure-erase") == 0) {
		print_str = "Secure Erase";
		checkup_mask = EXT_CSD_SEC_ER_EN;
		arg = 0x80000000;
	} else if (strcmp(argv[1], "secure-trim1") == 0) {
		print_str = "Secure Trim Step 1";
		checkup_mask = EXT_CSD_SEC_ER_EN | EXT_CSD_SEC_GB_CL_EN;
		arg = 0x80000001;
	} else if (strcmp(argv[1], "secure-trim2") == 0) {
		print_str = "Secure Trim Step 2";
		checkup_mask = EXT_CSD_SEC_ER_EN | EXT_CSD_SEC_GB_CL_EN;
		arg = 0x80008000;
	} else if (strcmp(argv[1], "trim") == 0) {
		print_str = "Trim";
		checkup_mask = EXT_CSD_SEC_GB_CL_EN;
		arg = 0x00000001;
	} else {
		fprintf(stderr, "Unknown erase type: %s\n", argv[1]);
		exit(1);
	}

	dev_fd = open(argv[4], O_RDWR);
	if (dev_fd < 0) {
		perror(argv[4]);
		exit(1);
	}

	if (checkup_mask) {
		ret = read_extcsd(dev_fd, ext_csd);
		if (ret) {
			fprintf(stderr, "Could not read EXT_CSD from %s\n",
				argv[4]);
			goto out;
		}
		if ((checkup_mask & ext_csd[EXT_CSD_SEC_FEATURE_SUPPORT]) !=
								checkup_mask) {
			fprintf(stderr, "%s is not supported in %s\n",
				print_str, argv[4]);
			ret = -ENOTSUP;
			goto out;
		}

	}
	printf("Executing %s from 0x%08x to 0x%08x\n", print_str, start, end);

	ret = erase(dev_fd, arg, start, end);
out:
	printf(" %s %s!\n\n", print_str, ret ? "Failed" : "Succeed");
	close(dev_fd);
	return ret;
}

static void set_ffu_single_cmd(struct mmc_ioc_multi_cmd *multi_cmd,
			       __u8 *ext_csd, unsigned int bytes, __u8 *buf,
			       off_t offset)
{
	__u32 arg = htole32(*((__u32 *)&ext_csd[EXT_CSD_FFU_ARG_0]));

	/* send block count */
	set_single_cmd(&multi_cmd->cmds[1], MMC_SET_BLOCK_COUNT, 0, 0,
		       bytes / 512);
	multi_cmd->cmds[1].flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_AC;

	/*
	 * send image chunk: blksz and blocks essentially do not matter, as
	 * long as the product is fw_size, but some hosts don't handle larger
	 * blksz well.
	 */
	set_single_cmd(&multi_cmd->cmds[2], MMC_WRITE_MULTIPLE_BLOCK, 1,
		       bytes / 512, arg);
	mmc_ioc_cmd_set_data(multi_cmd->cmds[2], buf + offset);
}

int do_ffu(int nargs, char **argv)
{
	int dev_fd, img_fd;
	int sect_done = 0, retry = 3, ret = -EINVAL;
	unsigned int sect_size;
	__u8 ext_csd[512];
	__u8 *buf = NULL;
	off_t fw_size, bytes_left, off;
	char *device;
	struct mmc_ioc_multi_cmd *multi_cmd = NULL;
	unsigned int default_chunk = MMC_IOC_MAX_BYTES;

	assert (nargs == 3 || nargs == 4);

	device = argv[2];
	dev_fd = open(device, O_RDWR);
	if (dev_fd < 0) {
		perror("device open failed");
		exit(1);
	}
	img_fd = open(argv[1], O_RDONLY);
	if (img_fd < 0) {
		perror("image open failed");
		close(dev_fd);
		exit(1);
	}

	ret = read_extcsd(dev_fd, ext_csd);
	if (ret) {
		fprintf(stderr, "Could not read EXT_CSD from %s\n", device);
		goto out;
	}

	if (ext_csd[EXT_CSD_REV] < EXT_CSD_REV_V5_0) {
		fprintf(stderr,
			"The FFU feature is only available on devices >= "
			"MMC 5.0, not supported in %s\n", device);
		goto out;
	}

	if (!(ext_csd[EXT_CSD_SUPPORTED_MODES] & EXT_CSD_FFU)) {
		fprintf(stderr, "FFU is not supported in %s\n", device);
		goto out;
	}

	if (ext_csd[EXT_CSD_FW_CONFIG] & EXT_CSD_UPDATE_DISABLE) {
		fprintf(stderr, "Firmware update was disabled in %s\n", device);
		goto out;
	}

	fw_size = lseek(img_fd, 0, SEEK_END);
	if (fw_size == 0) {
		fprintf(stderr, "Wrong firmware size");
		goto out;
	}

	/* allocate maximum required */
	buf = malloc(fw_size);
	multi_cmd = calloc(1, sizeof(struct mmc_ioc_multi_cmd) +
				4 * sizeof(struct mmc_ioc_cmd));
	if (!buf || !multi_cmd) {
		perror("failed to allocate memory");
		goto out;
	}

	/* ensure fw is multiple of native sector size */
	sect_size = (ext_csd[EXT_CSD_DATA_SECTOR_SIZE] == 0) ? 512 : 4096;
	if (fw_size % sect_size) {
		fprintf(stderr, "Firmware data size (%jd) is not aligned!\n", (intmax_t)fw_size);
		goto out;
	}

	if (nargs == 4) {
		default_chunk = strtol(argv[3], NULL, 10);
		if (default_chunk > MMC_IOC_MAX_BYTES || default_chunk % 512) {
			fprintf(stderr, "Invalid chunk size");
			goto out;
		}
	}

	/* prepare multi_cmd for FFU based on cmd to be used */

	multi_cmd->num_of_cmds = 4;

	/* put device into ffu mode */
	fill_switch_cmd(&multi_cmd->cmds[0], EXT_CSD_MODE_CONFIG,
			EXT_CSD_FFU_MODE);

	/* return device into normal mode */
	fill_switch_cmd(&multi_cmd->cmds[3], EXT_CSD_MODE_CONFIG,
			EXT_CSD_NORMAL_MODE);

	/* read firmware */
	lseek(img_fd, 0, SEEK_SET);
	if (read(img_fd, buf, fw_size) != fw_size) {
		perror("Could not read the firmware file: ");
		ret = -ENOSPC;
		goto out;
	}

do_retry:
	bytes_left = fw_size;
	off = 0;
	while (bytes_left) {
		unsigned int chunk_size = bytes_left < default_chunk ?
					  bytes_left : default_chunk;

		/* prepare multi_cmd for FFU based on cmd to be used */
		set_ffu_single_cmd(multi_cmd, ext_csd, chunk_size, buf, off);

		/* send ioctl with multi-cmd */
		ret = ioctl(dev_fd, MMC_IOC_MULTI_CMD, multi_cmd);

		if (ret) {
			perror("Multi-cmd ioctl");
			/*
			 * In case multi-cmd ioctl failed before exiting from
			 * ffu mode
			 */
			ioctl(dev_fd, MMC_IOC_CMD, &multi_cmd->cmds[3]);
			goto out;
		}

		bytes_left -= chunk_size;
		off += chunk_size;
	}

	/*
	 * By spec - check if mode operation codes are supported in ffu features,
	 * if not then skip checking number of sectors programmed after install
	 */
	if (!ext_csd[EXT_CSD_FFU_FEATURES]) {
		fprintf(stderr, "Please reboot to complete firmware installation on %s\n", device);
		ret = 0;
		goto out;
	}

	ret = read_extcsd(dev_fd, ext_csd);
	if (ret) {
		fprintf(stderr, "Could not read EXT_CSD from %s\n", device);
		goto out;
	}

	/* Test if we need to restart the download */
	sect_done = htole32(*((__u32 *)&ext_csd[EXT_CSD_NUM_OF_FW_SEC_PROG_0]));
	/* By spec, host should re-start download from the first sector if sect_done is 0 */
	if (sect_done == 0) {
		if (retry--) {
			fprintf(stderr, "Programming failed. Retrying... (%d)\n", retry);
			goto do_retry;
		}
		fprintf(stderr, "Programming failed! Aborting...\n");
		goto out;
	}

	if ((sect_done * sect_size) == fw_size) {
		fprintf(stderr, "Programmed %jd/%jd bytes\n", (intmax_t)fw_size, (intmax_t)fw_size);
		fprintf(stderr, "Programming finished with status %d \n", ret);
	}
	else {
		fprintf(stderr, "FW size and number of sectors written mismatch. Status return %d\n", ret);
		goto out;
	}

	fprintf(stderr, "Installing firmware on %s...\n", device);
	/* Re-enter ffu mode and install the firmware */
	multi_cmd->num_of_cmds = 2;

	/* set ext_csd to install mode */
	fill_switch_cmd(&multi_cmd->cmds[1], EXT_CSD_MODE_OPERATION_CODES,
			EXT_CSD_FFU_INSTALL);

	/* send ioctl with multi-cmd */
	ret = ioctl(dev_fd, MMC_IOC_MULTI_CMD, multi_cmd);

	if (ret) {
		perror("Multi-cmd ioctl failed setting install mode");
		/* In case multi-cmd ioctl failed before exiting from ffu mode */
		ioctl(dev_fd, MMC_IOC_CMD, &multi_cmd->cmds[3]);
		goto out;
	}

	ret = read_extcsd(dev_fd, ext_csd);
	if (ret) {
		fprintf(stderr, "Could not read EXT_CSD from %s\n", device);
		goto out;
	}

	/* return status */
	ret = ext_csd[EXT_CSD_FFU_STATUS];
	if (ret) {
		fprintf(stderr, "%s: error %d during FFU install:\n", device, ret);
		goto out;
	} else {
		fprintf(stderr, "FFU finished successfully\n");
	}

out:
	free(buf);
	free(multi_cmd);
	close(img_fd);
	close(dev_fd);
	return ret;
}

int do_general_cmd_read(int nargs, char **argv)
{
	int dev_fd;
	char *device;
	char *endptr;
	__u8 buf[512];
	__u32 arg = 0x01;
	int ret = -EINVAL, i;
	struct mmc_ioc_cmd idata;

	if (nargs != 2 && nargs != 3) {
		fprintf(stderr, "Usage: gen_cmd read </path/to/mmcblkX> [arg]\n");
		exit(1);
	}

	device = argv[1];
	dev_fd = open(device, O_RDWR);
	if (dev_fd < 0) {
		perror("device open failed");
		exit(1);
	}

	/* arg is specified */
	if (nargs == 3) {
		arg = strtol(argv[2], &endptr, 16);
		if (errno != 0 || *endptr != '\0' || !(arg & 0x1)) {
			fprintf(stderr, "Wrong ARG, it should be Hex number and bit0 must be 1\n");
			goto out;
		}
	}

	memset(&idata, 0, sizeof(idata));
	idata.write_flag = 0;
	idata.opcode = MMC_GEN_CMD;
	idata.arg = arg;
	idata.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;
	idata.blksz = 512;
	idata.blocks = 1;
	mmc_ioc_cmd_set_data(idata, buf);

	ret = ioctl(dev_fd, MMC_IOC_CMD, &idata);
	if (ret) {
		perror("ioctl");
		goto out;
	}

	printf("Data:\n");
	for (i = 0; i < 512; i++) {
		printf("%2x ", buf[i]);
		if ((i + 1) % 16 == 0)
			printf("\n");
	}
out:
	close(dev_fd);
	return ret;
}

static void issue_cmd0(char *device, __u32 arg)
{
	struct mmc_ioc_cmd idata;
	int fd;

	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	memset(&idata, 0, sizeof(idata));
	idata.opcode = MMC_GO_IDLE_STATE;
	idata.arg = arg;
	idata.flags = MMC_RSP_NONE | MMC_CMD_BC;

	/* No need to check for error, it is expected */
	ioctl(fd, MMC_IOC_CMD, &idata);
	close(fd);
}

int do_softreset(int nargs, char **argv)
{
	char *device;

	if (nargs != 2) {
		fprintf(stderr, "Usage: mmc softreset </path/to/mmcblkX>\n");
		exit(1);
	}

	device = argv[1];
	issue_cmd0(device, MMC_GO_IDLE_STATE_ARG);

	return 0;
}

int do_preidle(int nargs, char **argv)
{
	char *device;

	if (nargs != 2) {
		fprintf(stderr, "Usage: mmc preidle </path/to/mmcblkX>\n");
		exit(1);
	}

	device = argv[1];
	issue_cmd0(device, MMC_GO_PRE_IDLE_STATE_ARG);

	return 0;
}

int do_alt_boot_op(int nargs, char **argv)
{
	int fd, ret, boot_data_fd;
	char *device, *boot_data_file;
	struct mmc_ioc_multi_cmd *mioc;
	__u8 ext_csd[512];
	__u8 *boot_buf;
	unsigned int boot_blocks, ext_csd_boot_size;

	if (nargs != 3) {
		fprintf(stderr, "Usage: mmc boot_op <boot_data_file> </path/to/mmcblkX>\n");
		exit(1);
	}
	boot_data_file = argv[1];
	device = argv[2];

	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror("open device");
		exit(1);
	}

	ret = read_extcsd(fd, ext_csd);
	if (ret) {
		perror("read extcsd");
		goto dev_fd_close;
	}
	if (!(ext_csd[EXT_CSD_BOOT_INFO] & EXT_CSD_BOOT_INFO_ALT)) {
		ret = -EINVAL;
		perror("Card does not support alternative boot mode");
		goto dev_fd_close;
	}
	if (ext_csd[EXT_CSD_PART_CONFIG] & EXT_CSD_PART_CONFIG_ACC_ACK) {
		ret = -EINVAL;
		perror("Boot Ack must not be enabled");
		goto dev_fd_close;
	}
	ext_csd_boot_size = ext_csd[EXT_CSD_BOOT_MULT] * 128 * 1024;
	boot_blocks = ext_csd_boot_size / 512;
	if (ext_csd_boot_size > MMC_IOC_MAX_BYTES) {
		printf("Boot partition size is bigger than IOCTL limit, limiting to 512K\n");
		boot_blocks = MMC_IOC_MAX_BYTES / 512;
	}

	boot_data_fd = open(boot_data_file, O_WRONLY | O_CREAT, 0644);
	if (boot_data_fd < 0) {
		perror("open boot data file");
		ret = 1;
		goto boot_data_close;
	}

	boot_buf = calloc(1, sizeof(__u8) * boot_blocks * 512);
	mioc = calloc(1, sizeof(struct mmc_ioc_multi_cmd) +
			   2 * sizeof(struct mmc_ioc_cmd));
	if (!mioc || !boot_buf) {
		perror("Failed to allocate memory");
		ret = -ENOMEM;
		goto alloced_error;
	}

	mioc->num_of_cmds = 2;
	mioc->cmds[0].opcode = MMC_GO_IDLE_STATE;
	mioc->cmds[0].arg = MMC_GO_PRE_IDLE_STATE_ARG;
	mioc->cmds[0].flags = MMC_RSP_NONE | MMC_CMD_AC;
	mioc->cmds[0].write_flag = 0;

	mioc->cmds[1].opcode = MMC_GO_IDLE_STATE;
	mioc->cmds[1].arg = MMC_BOOT_INITIATION_ARG;
	mioc->cmds[1].flags = MMC_RSP_NONE | MMC_CMD_ADTC;
	mioc->cmds[1].write_flag = 0;
	mioc->cmds[1].blksz = 512;
	mioc->cmds[1].blocks = boot_blocks;
	/* Access time of boot part differs wildly, spec mandates 1s */
	mioc->cmds[1].data_timeout_ns = 2 * 1000 * 1000 * 1000;
	mmc_ioc_cmd_set_data(mioc->cmds[1], boot_buf);

	ret = ioctl(fd, MMC_IOC_MULTI_CMD, mioc);
	if (ret) {
		perror("multi-cmd ioctl error\n");
		goto alloced_error;
	}

	ret = DO_IO(write, boot_data_fd, boot_buf, boot_blocks * 512);
	if (ret < 0) {
		perror("Write error\n");
		goto alloced_error;
	}
	ret = 0;

alloced_error:
	if (mioc)
		free(mioc);
	if (boot_buf)
		free(boot_buf);
boot_data_close:
	close(boot_data_fd);
dev_fd_close:
	close(fd);
	if (ret)
		exit(1);
	return 0;
}

int do_resize_boot(int nargs, char **argv)
{
	__u8 ext_csd[512];
	int i;
	int fd, ret;
	char *device;
	int dry_run = 1;
	unsigned int boot_blocks, rpmb_blocks;
	struct mmc_ioc_multi_cmd *mioc = NULL;

	if (nargs != 5) {
		fprintf(stderr, "Usage: mmc resize_boot <-y|-n> <BOOT_BLOCKS> <RPMB_BLOCKS> </path/to/mmcblkX>\n");
		fprintf(stderr, "       where <BOOT_BLOCKS> is the number of 128KiB blocks to assign to each boot partition\n");
		fprintf(stderr, "       and   <RPMB_BLOCKS> is the number of 128KiB blocks to assign to the RPMB partition\n");
		exit(1);
	}

	printf("!!! WARNING                                         !!!\n");
	printf("!!! This method only works for SanDisk iNAND 7250.  !!!\n");

	// FIXME: Add check to make sure we're on SanDisk 7250.

	if (!strcmp("-y", argv[1])) {
		dry_run = 0;
	}

	boot_blocks = strtol(argv[2], NULL, 10);
	rpmb_blocks = strtol(argv[3], NULL, 10);
	device = argv[4];

	if (boot_blocks > 255) {
		fprintf(stderr, "Number of boot blocks (%d) cannot exceed 255.\n", boot_blocks);
		exit(1);
	}

	if (rpmb_blocks > 128) {
		fprintf(stderr, "Number of RPMB blocks (%d) cannot exceed 128.\n", rpmb_blocks);
		exit(1);
	}

	fd = open(device, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Could not open the specified device %s\n", device);
		exit(1);
	}

	ret = read_extcsd(fd, ext_csd);
	if (ret) {
		fprintf(stderr, "Could not read EXT_CSD from %s\n", device);
		close(fd);
		exit(1);
	}

	printf("Current BOOT_MULT: %d  (%0.3f MiB)\n", ext_csd[EXT_CSD_BOOT_MULT],
		ext_csd[EXT_CSD_BOOT_MULT] * 0.125f);
	printf("Current RPMB_MULT: %d  (%0.3f MiB)\n", ext_csd[EXT_CSD_RPMB_MULT],
		ext_csd[EXT_CSD_RPMB_MULT] * 0.125f);

	if (dry_run) {
		printf("\nTHIS IS A DRY RUN - NO CHANGES WILL BE MADE\n\n");
	} else {
		printf("!!! THIS IS A REAL RUN - THE BOOT SIZES WILL BE CHANGED !!!\n\n");
	}

	printf("New BOOT_MULT: %d  (%0.3f MiB)\n", boot_blocks, boot_blocks * 0.125f);
	printf("New RPMB_MULT: %d  (%0.3f MiB)\n", rpmb_blocks, rpmb_blocks * 0.125f);

	// Setup 3 commands to send. CMD62 three times, first with magic number, then
	// with the new BOOT_SIZE_MULT, and finally with the new RPMB_SIZE_MULT.
	mioc = calloc(1, sizeof(struct mmc_ioc_multi_cmd) + 3*sizeof(struct mmc_ioc_cmd));
	if (!mioc) {
		perror("failed to allocate memory");
		close(fd);
		exit(1);
	}
	
#define MMC_MFR_CMD62 62
#define SANDISK_7250_RESIZE_BOOT_MAGIC 0x254DDEC4
	
	sleep(1);

	mioc->num_of_cmds = 3;

	// Send CMD62 with MAGIC.
	mioc->cmds[0].opcode = MMC_MFR_CMD62;
	mioc->cmds[0].arg = SANDISK_7250_RESIZE_BOOT_MAGIC;
	mioc->cmds[0].flags = MMC_RSP_R1B | MMC_CMD_AC;
	mioc->cmds[0].write_flag = 1;
	mioc->cmds[0].cmd_timeout_ms = 2000;

	// Send CMD62 with new size of boot partitions.
	mioc->cmds[1].opcode = MMC_MFR_CMD62;
	mioc->cmds[1].arg = boot_blocks;
	mioc->cmds[1].flags = MMC_RSP_R1B | MMC_CMD_AC;
	mioc->cmds[1].write_flag = 1;
	mioc->cmds[1].cmd_timeout_ms = 2000;

	// Send CMD62 with new size of RPMB partition.
	mioc->cmds[2].opcode = MMC_MFR_CMD62;
	mioc->cmds[2].arg = rpmb_blocks;
	mioc->cmds[2].flags = MMC_RSP_R1B | MMC_CMD_AC;
	mioc->cmds[2].write_flag = 1;
	mioc->cmds[2].cmd_timeout_ms = 60000;

	// Execute.
	if (dry_run) {
		ret = -1;
	} else {
		printf("Starting eMMC boot partition size update...\n");
		//ret = ioctl(fd, MMC_IOC_MULTI_CMD, mioc);

		// Send one at a time.
		for (i=0; i < 3; i++) {
			ret = ioctl(fd, MMC_IOC_CMD, &mioc->cmds[i]);
			if (ret) { fprintf(stderr, "- cmd[%d] error %d\n", i, ret); exit(1); }
			printf(" - cmd[%d] response: 0x%x\n", i, mioc->cmds[i].response[0]);
		}
		
		sleep(1);
		printf(" - done, freeing resources\n");
		sleep(1);
	}

	// Release resources.
	free(mioc);
	close(fd);

	if (dry_run) {
		printf("\n\nThis was a dry run, eMMC was not modified.\n\n");
		return 0;
	}
	
	if (ret) {
		fprintf(stderr, "\n\n!!! MMC COMMANDS FAILED.  IOCTL ERROR = 0x%x !!!\n\n", ret);
		exit(1);
	}

	printf("\n\nThe changes have been made, next you must power-cycle the eMMC.\n\n");
	return 0;
}

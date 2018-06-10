/*
 * --------------------------------------
 * UNOCart Firmware (c)2016 Robin Edwards
 * --------------------------------------
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE

#include "tm_stm32f4_fatfs.h"
#include "tm_stm32f4_delay.h" // For TM_DELAY_Init
#include <stdio.h>
#include <string.h>
#include "cart_bus.h"
#include "cart_emu.h"
#include "xex_ldr.h"
#include "GameBoy.h"
#include "Mode7.h"
#include "sdcard.h"

int num_dir_entries = 0; // how many entries in the current directory
char errorBuf[40];

int entry_compare(const void* p1, const void* p2) {
	DIR_ENTRY* e1 = (DIR_ENTRY*) p1;
	DIR_ENTRY* e2 = (DIR_ENTRY*) p2;
	if (e1->isDir && !e2->isDir)
		return -1;
	else if (!e1->isDir && e2->isDir)
		return 1;
	else
		return stricmp(e1->long_filename, e2->long_filename);
}

char *get_filename_ext(char *filename) {
	char *dot = strrchr(filename, '.');
	if (!dot || dot == filename)
		return "";
	return dot + 1;
}

int is_valid_file(char *filename) {
	char *ext = get_filename_ext(filename);
	if (stricmp(ext, "CAR") == 0 || stricmp(ext, "ROM") == 0
			|| stricmp(ext, "XEX") == 0 || stricmp(ext, "ATR") == 0)
		return 1;
	return 0;
}

// single FILINFO structure
FILINFO fno;
char lfn[_MAX_LFN + 1]; /* Buffer to store the LFN */

void init_sdcard(void) {
	// this seems to be required for this version of FAT FS
	fno.lfname = lfn;
	fno.lfsize = sizeof lfn;
}

int scan_files(char *path, char *search) {
	FRESULT res;
	DIR dir;
	UINT i;

	res = f_opendir(&dir, path);
	if (res == FR_OK) {
		for (;;) {
			if (num_dir_entries == 255)
				break;
			res = f_readdir(&dir, &fno);
			if (res != FR_OK || fno.fname[0] == 0)
				break;
			if (fno.fattrib & (AM_HID | AM_SYS))
				continue;
			if (fno.fattrib & AM_DIR) {
				i = strlen(path);
				strcat(path, "/");
				strcat(path, fno.fname);
				if (strlen(path) >= 210)
					continue;	// no more room for path in DIR_ENTRY
				res = scan_files(path, search);
				if (res != FR_OK)
					break;
				path[i] = 0;
			} else if (is_valid_file(fno.fname)) {
				// copy the 8.3 filename to the LFN if blank
				if (!fno.lfname[0])
					strcpy(fno.lfname, fno.fname);
				char *match = strcasestr(fno.lfname, search);
				if (match) {
					DIR_ENTRY *dst = (DIR_ENTRY *) &cart_ram1[0];
					dst += num_dir_entries;
					// fill out a record
					dst->isDir = (match == fno.lfname) ? 1 : 0;	// use this for a "score"
					strcpy(dst->filename, fno.fname);
					strncpy(dst->long_filename, fno.lfname, 31);
					dst->long_filename[31] = 0;
					// full path for search results
					strcpy(dst->full_path, path);

					num_dir_entries++;
				}
			}
		}
		f_closedir(&dir);
	}
	return res;
}

int search_directory(char *path, char *search) {
	char pathBuf[256];
	strcpy(pathBuf, path);
	num_dir_entries = 0;
	int i;
	FATFS FatFs;
	TM_DELAY_Init();
	if (f_mount(&FatFs, "", 1) == FR_OK) {
		if (scan_files(pathBuf, search) == FR_OK) {
			// sort by score, name
			qsort((DIR_ENTRY *) &cart_ram1[0], num_dir_entries,
					sizeof(DIR_ENTRY), entry_compare);
			DIR_ENTRY *dst = (DIR_ENTRY *) &cart_ram1[0];
			// reset the "scores" back to 0
			for (i = 0; i < num_dir_entries; i++)
				dst[i].isDir = 0;
			return 1;

		}
	}
	strcpy(errorBuf, "Problem searching SD card");
	return 0;
}

int read_directory(char *path) {
	int ret = 0;
	num_dir_entries = 0;
	DIR_ENTRY *dst = (DIR_ENTRY *) &cart_ram1[0];

	FATFS FatFs;
	TM_DELAY_Init();
	if (f_mount(&FatFs, "", 1) == FR_OK) {
		DIR dir;
		if (f_opendir(&dir, path) == FR_OK) {
			while (num_dir_entries < 255) {
				if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0)
					break;
				if (fno.fattrib & (AM_HID | AM_SYS))
					continue;
				dst->isDir = fno.fattrib & AM_DIR ? 1 : 0;
				if (!dst->isDir)
					if (!is_valid_file(fno.fname))
						continue;
				// copy file record to first ram block
				strcpy(dst->filename, fno.fname);
				if (fno.lfname[0]) {
					strncpy(dst->long_filename, fno.lfname, 31);
					dst->long_filename[31] = 0;
				} else
					strcpy(dst->long_filename, fno.fname);
				dst->full_path[0] = 0; // path only for search results
				dst++;
				num_dir_entries++;
			}
			f_closedir(&dir);
		} else
			strcpy(errorBuf, "Can't read directory");
		f_mount(0, "", 1);
		qsort((DIR_ENTRY *) &cart_ram1[0], num_dir_entries, sizeof(DIR_ENTRY),
				entry_compare);
		ret = 1;
	} else
		strcpy(errorBuf, "Can't read SD card");
	return ret;
}

/* CARTRIDGE/XEX HANDLING */

int load_file(char *filename) {
	TM_DELAY_Init();
	FATFS FatFs;
	int cart_type = CART_TYPE_NONE;
	int car_file = 0, xex_file = 0, expectedSize = 0;
	unsigned char carFileHeader[16];
	UINT br, size = 0;

	if (strncasecmp(filename + strlen(filename) - 4, ".CAR", 4) == 0)
		car_file = 1;
	if (strncasecmp(filename + strlen(filename) - 4, ".XEX", 4) == 0)
		xex_file = 1;

	if (f_mount(&FatFs, "", 1) != FR_OK) {
		strcpy(errorBuf, "Can't read SD card");
		return 0;
	}
	FIL fil;
	if (f_open(&fil, filename, FA_READ) != FR_OK) {
		strcpy(errorBuf, "Can't open file");
		goto cleanup;
	}

	// read the .CAR file header?
	if (car_file) {
		if (f_read(&fil, carFileHeader, 16, &br) != FR_OK || br != 16) {
			strcpy(errorBuf, "Bad CAR file");
			goto closefile;
		}
		int car_type = carFileHeader[7];
		if (car_type == 1) {
			cart_type = CART_TYPE_8K;
			expectedSize = 8192;
		} else if (car_type == 2) {
			cart_type = CART_TYPE_16K;
			expectedSize = 16384;
		} else if (car_type == 3) {
			cart_type = CART_TYPE_OSS_16K_034M;
			expectedSize = 16384;
		} else if (car_type == 8) {
			cart_type = CART_TYPE_WILLIAMS_64K;
			expectedSize = 65536;
		} else if (car_type == 9) {
			cart_type = CART_TYPE_EXPRESS_64K;
			expectedSize = 65536;
		} else if (car_type == 10) {
			cart_type = CART_TYPE_DIAMOND_64K;
			expectedSize = 65536;
		} else if (car_type == 11) {
			cart_type = CART_TYPE_SDX_64K;
			expectedSize = 65536;
		} else if (car_type == 12) {
			cart_type = CART_TYPE_XEGS_32K;
			expectedSize = 32768;
		} else if (car_type == 13) {
			cart_type = CART_TYPE_XEGS_64K;
			expectedSize = 65536;
		} else if (car_type == 14) {
			cart_type = CART_TYPE_XEGS_128K;
			expectedSize = 131072;
		} else if (car_type == 15) {
			cart_type = CART_TYPE_OSS_16K_TYPE_B;
			expectedSize = 16384;
		} else if (car_type == 18) {
			cart_type = CART_TYPE_BOUNTY_BOB;
			expectedSize = 40960;
		} else if (car_type == 22) {
			cart_type = CART_TYPE_WILLIAMS_64K;
			expectedSize = 32768;
		} else if (car_type == 26) {
			cart_type = CART_TYPE_MEGACART_16K;
			expectedSize = 16384;
		} else if (car_type == 27) {
			cart_type = CART_TYPE_MEGACART_32K;
			expectedSize = 32768;
		} else if (car_type == 28) {
			cart_type = CART_TYPE_MEGACART_64K;
			expectedSize = 65536;
		} else if (car_type == 29) {
			cart_type = CART_TYPE_MEGACART_128K;
			expectedSize = 131072;
		} else if (car_type == 33) {
			cart_type = CART_TYPE_SW_XEGS_32K;
			expectedSize = 32768;
		} else if (car_type == 34) {
			cart_type = CART_TYPE_SW_XEGS_64K;
			expectedSize = 65536;
		} else if (car_type == 35) {
			cart_type = CART_TYPE_SW_XEGS_128K;
			expectedSize = 131072;
		} else if (car_type == 40) {
			cart_type = CART_TYPE_BLIZZARD_16K;
			expectedSize = 16384;
		} else if (car_type == 41) {
			cart_type = CART_TYPE_ATARIMAX_1MBIT;
			expectedSize = 131072;
		} else if (car_type == 43) {
			cart_type = CART_TYPE_SDX_128K;
			expectedSize = 131072;
		} else if (car_type == 44) {
			cart_type = CART_TYPE_OSS_8K;
			expectedSize = 8192;
		} else if (car_type == 45) {
			cart_type = CART_TYPE_OSS_16K_043M;
			expectedSize = 16384;
		} else if (car_type == 54) {
			cart_type = CART_TYPE_SIC_128K;
			expectedSize = 131072;
		} else {
			strcpy(errorBuf, "Unsupported CAR type");
			goto closefile;
		}
	}

	// set a default error
	strcpy(errorBuf, "Can't read file");

	unsigned char *dst = &cart_ram1[0];
	int bytes_to_read = 64 * 1024;
	if (xex_file) {
		dst += 4;	// leave room for the file length at the start of sram
		bytes_to_read -= 4;
	}
	// read the file in two 64k chunks to each area of SRAM
	if (f_read(&fil, dst, bytes_to_read, &br) != FR_OK) {
		cart_type = CART_TYPE_NONE;
		goto closefile;
	}
	size += br;
	if (br == bytes_to_read) {
		// first 64k was complete, so try to load 64k more
		if (f_read(&fil, &cart_ram2[0], 64 * 1024, &br) != FR_OK) {
			cart_type = CART_TYPE_NONE;
			goto closefile;
		}
		size += br;
		if (br == 64 * 1024) {
			// that's 128k read, is there any more?
			if (f_read(&fil, carFileHeader, 1, &br) != FR_OK) {
				cart_type = CART_TYPE_NONE;
				goto closefile;
			}
			if (br == 1) {
				strcpy(errorBuf, "Cart file/XEX too big (>128k)");
				cart_type = CART_TYPE_NONE;
				goto closefile;
			}
		}
	}

	// set the correct cart type based on the size
	if (car_file) {
		if (size != expectedSize) {
			strcpy(errorBuf, "CAR file is wrong size");
			cart_type = CART_TYPE_NONE;
			goto closefile;
		}
	} else if (xex_file) {
		cart_type = CART_TYPE_XEX;
		// stick the size of the file as the first 4 bytes (little endian)
		cart_ram1[0] = size & 0xFF;
		cart_ram1[1] = (size >> 8) & 0xFF;
		cart_ram1[2] = (size >> 16) & 0xFF;
		cart_ram1[3] = 0;	// has to be zero!
	} else {	// not a car/xex file - guess the type based on size
		if (size == 8 * 1024)
		{
			if (cart_ram1[0x1FF8]=='G' && cart_ram1[0x1FF9]=='B')
			{
				cart_type = CART_TYPE_GB;
			}
			else if (cart_ram1[0x1FF8]=='M' && cart_ram1[0x1FF9]=='7')
			{
				cart_type = CART_TYPE_M7;
			}
			else

			{
				cart_type = CART_TYPE_8K;
			}
		}
		else if (size == 16 * 1024)
			cart_type = CART_TYPE_16K;
		else if (size == 32 * 1024)
			cart_type = CART_TYPE_XEGS_32K;
		else if (size == 64 * 1024)
			cart_type = CART_TYPE_XEGS_64K;
		else if (size == 128 * 1024)
			cart_type = CART_TYPE_XEGS_128K;
		else {
			strcpy(errorBuf, "Unsupported ROM size ");
			cart_type = CART_TYPE_NONE;
			goto closefile;
		}
	}

	closefile: f_close(&fil);
	cleanup: f_mount(0, "", 1);
	return cart_type;
}

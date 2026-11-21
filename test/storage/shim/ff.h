// The part of FatFs that machine/storage.c uses, over a sparse in-memory file,
// for test_storage.c on a desktop. Only the sectors written hold memory, so a
// test can reach the card's last sector without a 256 MB image.
#ifndef TEST_STORAGE_FF_H
#define TEST_STORAGE_FF_H

#include <stdint.h>

typedef unsigned int UINT;
typedef uint64_t FSIZE_t;

typedef enum { FR_OK = 0, FR_DISK_ERR, FR_NO_FILE = 4, FR_NO_PATH = 5 } FRESULT;

#define FA_READ       0x01
#define FA_WRITE      0x02
#define FA_CREATE_NEW 0x04

typedef struct {
    FSIZE_t size;
    FSIZE_t pos;
} FIL;

FRESULT f_open(FIL *fp, const char *path, uint8_t mode);
FRESULT f_close(FIL *fp);
FRESULT f_lseek(FIL *fp, FSIZE_t ofs);
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT f_sync(FIL *fp);
#define f_size(fp) ((fp)->size)

#endif

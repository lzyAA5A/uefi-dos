/* UEFI-DOS -- common internal definitions shared by UEFI and host builds. */
#ifndef UEFIDOS_H
#define UEFIDOS_H

#ifdef UEFIDOS_HOST

#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int8_t    i8;
typedef int16_t   i16;
typedef int32_t   i32;
typedef int64_t   i64;
typedef size_t    usize;

#else /* UEFI build */

#include "efi/efi.h"

typedef UINT8     u8;
typedef UINT16    u16;
typedef UINT32    u32;
typedef UINT64    u64;
typedef INT8      i8;
typedef INT16     i16;
typedef INT32     i32;
typedef INT64     i64;
typedef UINTN     usize;
typedef UINTN     size_t;

/* freestanding memory/string routines (src/common/util.c) */
extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memmove(void *dst, const void *src, size_t n);
extern void *memset(void *dst, int c, size_t n);
extern int   memcmp(const void *a, const void *b, size_t n);
extern size_t strlen(const char *s);
extern size_t strnlen(const char *s, size_t max);
extern int   strcmp(const char *a, const char *b);
extern int   strncmp(const char *a, const char *b, size_t n);
extern char *strcpy(char *dst, const char *src);
extern char *strncpy(char *dst, const char *src, size_t n);
extern char *strcat(char *dst, const char *src);
extern char *strchr(const char *s, int c);
extern char *strrchr(const char *s, int c);
extern char *strstr(const char *hay, const char *needle);
extern void *memset32(void *dst, UINT32 v, size_t n);

#endif /* UEFIDOS_HOST */

#include "uefidos_config.h"

#ifndef NULL
#define NULL ((void *)0)
#endif
#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

typedef int bool;
typedef unsigned int uptr;  /* only for host-side misc; avoid */

#define UFD_UNUSED(x) ((void)(x))

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#endif /* UEFIDOS_H */

/* XBE image format: the subset the loader needs.
 * Clean-room, from the file layout as parsed by tools/xbeinfo.py and observed in our own
 * disc dump. No XDK headers were used. */
#pragma once
#include <stdint.h>

#define XBE_MAGIC 0x48454258u /* "XBEH" */

/* Retail XOR keys for the entry point and kernel thunk address (see tools/xbeinfo.py). */
#define XBE_ENTRY_XOR_RETAIL 0xA8FC57ABu
#define XBE_THUNK_XOR_RETAIL 0x5B6D40B6u

#pragma pack(push, 1)
typedef struct XbeHeader {
    uint32_t magic;                   /* 0x000 */
    uint8_t  signature[256];          /* 0x004 */
    uint32_t base_address;            /* 0x104 */
    uint32_t size_of_headers;         /* 0x108 */
    uint32_t size_of_image;           /* 0x10C */
    uint32_t size_of_image_header;    /* 0x110 */
    uint32_t timestamp;               /* 0x114 */
    uint32_t certificate_address;     /* 0x118 */
    uint32_t section_count;           /* 0x11C */
    uint32_t section_headers_address; /* 0x120 */
    uint32_t init_flags;              /* 0x124 */
    uint32_t entry_point_xored;       /* 0x128 */
    uint32_t tls_address;             /* 0x12C */
    uint32_t stack_size;              /* 0x130 */
    uint32_t pe_heap_reserve;         /* 0x134 */
    uint32_t pe_heap_commit;          /* 0x138 */
    uint32_t pe_base_address;         /* 0x13C */
    uint32_t pe_size_of_image;        /* 0x140 */
    uint32_t pe_checksum;             /* 0x144 */
    uint32_t pe_timestamp;            /* 0x148 */
    uint32_t debug_path_address;      /* 0x14C */
    uint32_t debug_file_address;      /* 0x150 */
    uint32_t debug_unicode_file_address; /* 0x154 */
    uint32_t kernel_thunk_xored;      /* 0x158 */
} XbeHeader;

typedef struct XbeSectionHeader {
    uint32_t flags;                   /* 0x00 */
    uint32_t virtual_address;         /* 0x04 */
    uint32_t virtual_size;            /* 0x08 */
    uint32_t raw_address;             /* 0x0C file offset */
    uint32_t raw_size;                /* 0x10 */
    uint32_t name_address;            /* 0x14 */
    uint32_t name_ref_count;          /* 0x18 */
    uint32_t head_shared_page_ref;    /* 0x1C */
    uint32_t tail_shared_page_ref;    /* 0x20 */
    uint8_t  digest[20];              /* 0x24 */
} XbeSectionHeader;                   /* 0x38 bytes */

typedef struct XbeTls {
    uint32_t raw_data_start;
    uint32_t raw_data_end;
    uint32_t index_address;
    uint32_t callbacks_address;
    uint32_t zero_fill_size;
    uint32_t characteristics;
} XbeTls;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(XbeHeader) == 0x15C, "XbeHeader layout");
static_assert(sizeof(XbeSectionHeader) == 0x38, "XbeSectionHeader layout");
#else
_Static_assert(sizeof(XbeHeader) == 0x15C, "XbeHeader layout");
_Static_assert(sizeof(XbeSectionHeader) == 0x38, "XbeSectionHeader layout");
#endif

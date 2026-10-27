/* Xbox Direct3D 8 (XDK 4831): resource layouts and constants the HLE shim needs.
 * Clean-room: written from the object layouts observed in our own Default.xbe and public
 * interoperability documentation of the Xbox D3D resource format. No XDK headers used.
 * Unverified values are marked "verify". */
#pragma once
#include <stdint.h>

/* Every Xbox D3D resource starts with these three dwords. */
typedef struct XD3DResource {
    uint32_t Common; /* refcount (bits 0-15), type (bits 16-18), flags */
    uint32_t Data;   /* physical address of the data; we use the virtual address */
    uint32_t Lock;
} XD3DResource;

/* Textures and surfaces. */
typedef struct XD3DPixelContainer {
    XD3DResource Resource;
    uint32_t Format; /* see XFMT_* accessors below */
    uint32_t Size;   /* linear formats only: width-1, height-1, pitch/64-1 */
} XD3DPixelContainer;

typedef struct XD3DSurface {
    XD3DPixelContainer Container;
    XD3DPixelContainer *Parent; /* owning texture, or NULL */
} XD3DSurface;

/* Common: */
#define XD3DCOMMON_REFCOUNT_MASK 0x0000FFFFu
#define XD3DCOMMON_TYPE_SHIFT    16
#define XD3DCOMMON_TYPE_MASK     0x00070000u
#define XD3DCOMMON_TYPE_VERTEXBUFFER 0
#define XD3DCOMMON_TYPE_INDEXBUFFER  1
#define XD3DCOMMON_TYPE_PUSHBUFFER   2
#define XD3DCOMMON_TYPE_PALETTE      3
#define XD3DCOMMON_TYPE_TEXTURE      4
#define XD3DCOMMON_TYPE_SURFACE      5
#define XD3DCOMMON_TYPE_FIXUP        6
#define XD3DCOMMON_D3DCREATED    0x01000000u /* verify */

/* Format dword fields. */
#define XFMT_DMA(f)       ((f) & 3u)
#define XFMT_CUBEMAP      0x00000004u
#define XFMT_BORDER       0x00000008u
#define XFMT_DIMENSION(f) (((f) >> 4) & 0xFu)
#define XFMT_FORMAT(f)    (((f) >> 8) & 0xFFu)
#define XFMT_MIPMAPS(f)   (((f) >> 16) & 0xFu)
#define XFMT_USIZE(f)     (((f) >> 20) & 0xFu) /* log2 width (swizzled formats) */
#define XFMT_VSIZE(f)     (((f) >> 24) & 0xFu) /* log2 height */
#define XFMT_PSIZE(f)     (((f) >> 28) & 0xFu) /* log2 depth */

/* Size dword fields (linear formats). */
#define XSIZE_WIDTH(s)  (((s) & 0xFFFu) + 1)
#define XSIZE_HEIGHT(s) ((((s) >> 12) & 0xFFFu) + 1)
#define XSIZE_PITCH(s)  (((((s) >> 24) & 0xFFu) + 1) * 64)

/* Xbox D3DFORMAT codes (differ from PC D3D). Swizzled unless LIN_. */
enum {
    XD3DFMT_L8 = 0x00,
    XD3DFMT_AL8 = 0x01,
    XD3DFMT_A1R5G5B5 = 0x02,
    XD3DFMT_X1R5G5B5 = 0x03,
    XD3DFMT_A4R4G4B4 = 0x04,
    XD3DFMT_R5G6B5 = 0x05,
    XD3DFMT_A8R8G8B8 = 0x06,
    XD3DFMT_X8R8G8B8 = 0x07,
    XD3DFMT_P8 = 0x0B,
    XD3DFMT_DXT1 = 0x0C,
    XD3DFMT_DXT2 = 0x0E,
    XD3DFMT_DXT4 = 0x0F,
    XD3DFMT_LIN_A1R5G5B5 = 0x10,
    XD3DFMT_LIN_R5G6B5 = 0x11,
    XD3DFMT_LIN_A8R8G8B8 = 0x12,
    XD3DFMT_LIN_L8 = 0x13,
    XD3DFMT_A8 = 0x19,
    XD3DFMT_A8L8 = 0x1A,
    XD3DFMT_LIN_AL8 = 0x1B,
    XD3DFMT_LIN_X1R5G5B5 = 0x1C,
    XD3DFMT_LIN_A4R4G4B4 = 0x1D,
    XD3DFMT_LIN_X8R8G8B8 = 0x1E,
    XD3DFMT_LIN_A8 = 0x1F,
    XD3DFMT_LIN_A8L8 = 0x20,
    XD3DFMT_YUY2 = 0x24,
    XD3DFMT_V8U8 = 0x28,
    XD3DFMT_D24S8 = 0x2A,
    XD3DFMT_F24S8 = 0x2B,
    XD3DFMT_D16 = 0x2C,
    XD3DFMT_F16 = 0x2D,
    XD3DFMT_LIN_D24S8 = 0x2E,
    XD3DFMT_LIN_D16 = 0x30,
};

/* Xbox D3DPRESENT_PARAMETERS (17 dwords; layout confirmed from the game's setup code). */
typedef struct XD3DPRESENT_PARAMETERS {
    uint32_t BackBufferWidth;
    uint32_t BackBufferHeight;
    uint32_t BackBufferFormat;
    uint32_t BackBufferCount;
    uint32_t MultiSampleType;
    uint32_t SwapEffect;
    void *hDeviceWindow;
    int32_t Windowed;
    int32_t EnableAutoDepthStencil;
    uint32_t AutoDepthStencilFormat;
    uint32_t Flags; /* 0x10 widescreen, 0x20 interlaced, 0x40 progressive, 0x80 field */
    uint32_t FullScreen_RefreshRateInHz;
    uint32_t FullScreen_PresentationInterval;
    void *BufferSurfaces[3];
    void *DepthStencilSurface;
} XD3DPRESENT_PARAMETERS;

#ifdef __cplusplus
static_assert(sizeof(XD3DPRESENT_PARAMETERS) == 17 * 4, "present parameters layout");
#else
_Static_assert(sizeof(XD3DPRESENT_PARAMETERS) == 17 * 4, "present parameters layout");
_Static_assert(sizeof(XD3DPixelContainer) == 20, "pixel container layout");
#endif

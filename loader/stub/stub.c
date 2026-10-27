/* ReJetLoader.exe: owns the XBE's address range.
 *
 * Linked at /BASE:0x10000 /FIXED with a large reserved section, so Windows maps the whole
 * range 0x10000.. for this image at process creation, before heaps and mapped files can
 * take it. All logic is in rejet.dll; rw_main maps the XBE over this image (stub code
 * included) and never returns here. See docs/LOADER.md. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define RESERVE_SIZE 0x3E0000

#pragma section(".xbe", read, execute)
__declspec(allocate(".xbe")) static unsigned char xbe_space[RESERVE_SIZE];

__declspec(dllimport) __declspec(noreturn) void __stdcall rw_main(void *image_base, unsigned image_size);

void __stdcall start(void)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)GetModuleHandleW(NULL);
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)((const char *)dos + dos->e_lfanew);
    (void)*(volatile unsigned char *)xbe_space; /* keep the reservation referenced */
    rw_main((void *)dos, nt->OptionalHeader.SizeOfImage); /* does not return */
}

/* Hal / Av / Xe / Dbg / console settings. */
#include <intrin.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "kernel.h"
#include "settings.h"
#include "xboxkrnl_impl.h"

/* Console settings (EEPROM). A PAL console (our disc is PAL): language and PAL60 from rejet.ini
 * (default English, 60 Hz), Europe region, no parental lock.
 * Index values are the kernel's setting numbers as observed in our XBE's XAPI callers. */
#define XC_TIMEZONEBIAS        0x002
#define XC_LANGUAGE            0x007
#define XC_VIDEO_FLAGS         0x008
#define XC_AUDIO_FLAGS         0x009
#define XC_FACTORY_AV_REGION   0x103
#define XC_FACTORY_GAME_REGION 0x104
#define REG_DWORD_TYPE 4
#define XC_VIDEO_FLAGS_PAL60 0x00400000 /* XGetVideoFlags returns bits 16+: 0x40 = PAL60 */

/* Xbox language IDs by game language index (settings.h GAME_LANG_*): the game maps them back with
 * its table at 0x1CD6E0 (docs/SUBSYSTEMS.md). */
static const ULONG xbox_language[] = {2 /* Japanese */, 1 /* English */, 4 /* French */,
                                      3 /* German */, 5 /* Spanish */, 6 /* Italian */};

NTSTATUS XBAPI xbox_ExQueryNonVolatileSetting(ULONG ValueIndex, PULONG Type, PVOID Value, ULONG ValueLength,
                                              PULONG ResultLength)
{
    ULONG v = 0;
    switch (ValueIndex) {
    case XC_LANGUAGE: v = xbox_language[g_settings.language]; break;
    case XC_VIDEO_FLAGS: v = g_settings.pal60 ? XC_VIDEO_FLAGS_PAL60 : 0; break;
    case XC_FACTORY_AV_REGION: v = 0x00800300; break; /* PAL-I */
    case XC_FACTORY_GAME_REGION: v = 4; break;        /* rest of world (the XBE allows only this) */
    default: v = 0; break;                            /* audio flags, time zone, parental: 0 */
    }
    KTRACE("ExQueryNonVolatileSetting(%03lX) -> %08lX\n", ValueIndex, v);
    if (Type)
        *Type = REG_DWORD_TYPE;
    if (Value && ValueLength >= sizeof v)
        memcpy(Value, &v, sizeof v);
    else if (Value && ValueLength)
        memset(Value, 0, ValueLength);
    if (ResultLength)
        *ResultLength = sizeof v;
    return STATUS_SUCCESS;
}

__declspec(noreturn) VOID XBAPI xbox_HalReturnToFirmware(ULONG Routine)
{
    /* 0 = halt, 1 = reboot, 2 = quick reboot / dashboard, ... */
    log_printf("HalReturnToFirmware(%lu) from %08X: title asked to leave; exiting\n", Routine,
               (unsigned)(UINT_PTR)_ReturnAddress());
    log_backtrace(_AddressOfReturnAddress(), 16);
    ExitProcess(0);
}

PVOID XBAPI xbox_AvGetSavedDataAddress(void)
{
    return NULL;
}

/* Ethernet PHY. Battle Mode starts the game's network layer (system link / Xbox Live), which
 * initialises the PHY and asks for the link state. Until LAN play exists (XNet over Winsock)
 * the cable reports as unplugged, which the game handles like on a real Xbox. */
NTSTATUS XBAPI xbox_PhyInitialize(BOOLEAN ForceReset, PVOID Parameter2)
{
    static LONG logged;
    (void)Parameter2;
    if (!InterlockedExchange(&logged, 1)) /* the game polls it every frame on the Battle Mode menus */
        KTRACE("PhyInitialize(force %u) (logged once)\n", ForceReset);
    return STATUS_SUCCESS;
}

ULONG XBAPI xbox_PhyGetLinkState(BOOLEAN Verify)
{
    (void)Verify;
    return 0; /* no XNET_ETHERNET_LINK_ACTIVE: cable unplugged */
}

/* AV_QUERY_AV_CAPABILITIES (6): AV pack | video standard | refresh and HDTV flags. JSRF's XDK
 * 4134 D3D (D3D_GetAdapterModeCount 0x19A2F0 / EnumAdapterModes) keeps only display modes whose
 * refresh bits match, so an answer of 0 left no modes and the renderer gave up before creating
 * a device. Report a PAL console on SCART, at 60 Hz with PAL60 (rejet.ini) or 50 Hz. */
#define AV_QUERY_AV_CAPABILITIES 6
#define AV_PACK_SCART            0x00000003
#define AV_STANDARD_PAL_I        0x00000300
#define AV_FLAGS_60HZ            0x00400000
#define AV_FLAGS_50HZ            0x00800000

VOID XBAPI xbox_AvSendTVEncoderOption(PVOID RegisterBase, ULONG Option, ULONG Param, PULONG Result)
{
    ULONG v = 0;
    (void)RegisterBase;
    (void)Param;
    if (Option == AV_QUERY_AV_CAPABILITIES)
        v = AV_PACK_SCART | AV_STANDARD_PAL_I | (g_settings.pal60 ? AV_FLAGS_60HZ : AV_FLAGS_50HZ);
    KTRACE("AvSendTVEncoderOption(%lu) -> %08lX\n", Option, v);
    if (Result)
        *Result = v;
}

/* All sections are mapped at load time, so loading/unloading only needs to succeed. */
NTSTATUS XBAPI xbox_XeLoadSection(PVOID Section)
{
    (void)Section;
    return STATUS_SUCCESS;
}

NTSTATUS XBAPI xbox_XeUnloadSection(PVOID Section)
{
    (void)Section;
    return STATUS_SUCCESS;
}

ULONG __cdecl xbox_DbgPrint(PCSTR Format, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, Format);
    vsnprintf(buf, sizeof buf, Format, ap);
    va_end(ap);
    log_printf("DbgPrint: %s%s", buf, buf[0] && buf[strlen(buf) - 1] == '\n' ? "" : "\n");
    return 0;
}

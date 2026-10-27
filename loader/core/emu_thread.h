/* Per-thread Xbox state: the KPCR/KTHREAD fields XDK code reads through fs:, and the
 * thread's TLS block. See tools/loader/gen_fs_patches.py and docs/LOADER.md. */
#pragma once

/* Windows TLS slots that patched fs: instructions read (fs:[0xE10 + 4*index]). */
enum {
    FS_SLOT_TLS,     /* fs:[0x04] Xbox: StackBase, used by XAPI/CRT as the TLS array base */
    FS_SLOT_PRCB,    /* fs:[0x20] KPCR.Prcb */
    FS_SLOT_IRQL,    /* fs:[0x24] KPCR.Irql (low byte) */
    FS_SLOT_THREAD,  /* fs:[0x28] KPCR.PrcbData.CurrentThread */
    FS_SLOT_DPC,     /* fs:[0x58] read by DSOUND, only tested for non-zero */
    FS_SLOT_COUNT
};

typedef struct FsPatch {
    unsigned address;      /* instruction address */
    unsigned disp_offset;  /* offset of the 32-bit displacement within the instruction */
    unsigned old_disp;     /* expected original displacement */
    unsigned slot;         /* FS_SLOT_* */
} FsPatch;

extern const FsPatch g_fs_patches[];
extern const unsigned g_fs_patch_count;

/* Emulated kernel objects for one Xbox thread. Offsets are the ones XDK code reads. */
#define EMU_KPCR_SIZE     0x300  /* Prcb (fs:[0x20]) points at kpcr + 0x28 */
#define EMU_KTHREAD_SIZE  0x200
#define KTHREAD_TLS_DATA  0x28   /* XapiThreadStartup: TLS block pointer */
#define ETHREAD_THREAD_ID 0x12C  /* GetCurrentThreadId */
#define KPRCB_CURRENT_THREAD 0x00 /* relative to Prcb */

typedef struct EmuThread {
    unsigned char kpcr[EMU_KPCR_SIZE];
    unsigned char kthread[EMU_KTHREAD_SIZE];
    unsigned char *tls;            /* TLS block (TlsDataSize bytes) */
    unsigned tls_size;
    unsigned thread_id;            /* Windows thread id */
} EmuThread;

/* Allocates the TLS slots and patches every fs: site. Call once after mapping the XBE. */
int emu_fs_init(void);

/* Creates the Xbox state for the calling Windows thread and points its slots at it. */
EmuThread *emu_thread_attach(unsigned tls_size);

/* Xbox state of the calling thread, or NULL if it is not an Xbox thread. */
EmuThread *emu_thread_current(void);

/* Debugging: arm REJET_WATCH on the calling thread (done automatically on attach). */
void emu_arm_watchpoint(void);

/* IRQL of the calling thread (the value patched fs:[0x24] reads). */
unsigned char emu_irql_get(void);
void emu_irql_set(unsigned char irql);

/* Kernel import table shared by the generated stubs and the loader. */
#pragma once

#define KERNEL_DATA_BLOCK 0x1000

typedef struct KernelImport {
    unsigned ordinal;
    const char *name;
    void *target;   /* implementation or stub, or backing block for a data export */
    void *traced;   /* same, wrapped to log every call (REJET_TRACE=1) */
    int is_data;
} KernelImport;

extern const KernelImport g_kernel_imports[];
extern const unsigned g_kernel_import_count;

/* Called by a generated stub when game code reaches a kernel function that has no
 * implementation yet. Logs ordinal, caller and stack arguments, then ends the process. */
__declspec(noreturn) void kernel_unimplemented(unsigned ordinal, void *caller, void *stack);

void log_printf(const char *fmt, ...);

/* Logs one kernel call from a generated trace wrapper. */
void ktrace_call(const char *name, void *caller, int has_result, unsigned long long result);

/* Logs likely return addresses into XBE code found on the stack from `stack` upwards. */
void log_backtrace(const void *stack, unsigned depth);

/* Shader compilation with an on-disk cache of compiled bytecode (see shadercache.c). */
#pragma once
#include <stddef.h>

/* Compile HLSL `src` (entry "main") for `profile`, or load the result of an earlier compile
 * of the same source. Returns malloc'ed bytecode (free with free()) or NULL; on failure
 * `err` receives the compiler message. */
void *shader_compile(const char *src, const char *name, const char *profile, char *err, size_t errcap);
/* REJET_DUMPHLSL=1: write a translated shader's HLSL to hlsl\<kind>_<index>.hlsl (debugging). */
void shader_dump(const char *kind, unsigned index, const char *src);

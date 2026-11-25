#pragma once
#include <stdint.h>
typedef struct { uint32_t addr, word; } FPWord;
typedef struct { int n; FPWord w[8]; } OverlayFP;
typedef struct { uint32_t addr; uint8_t thumb; int8_t ov; uint8_t sadie; uint32_t sig; void (*fn)(void); } NativeFunc;  /* sadie: bitmask of patch groups that force the interpreter */
#ifdef __cplusplus
extern "C" {
#endif
extern const OverlayFP OVERLAY_FP[36];
extern const NativeFunc NATIVE_FUNCS[];
extern const unsigned NATIVE_FUNC_COUNT;
#ifdef __cplusplus
}
#endif

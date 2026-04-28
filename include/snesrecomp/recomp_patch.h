#ifndef SNESRECOMP_RECOMP_PATCH_H
#define SNESRECOMP_RECOMP_PATCH_H

/*
 * RECOMP_PATCH — auto-registered recompiled function definitions.
 *
 * Inspired by N64Recomp's RECOMP_PATCH macro. Defines a recompiled C
 * function and auto-registers it in the snesrecomp dispatch table at
 * its original SNES 24-bit bank:address, before main() runs. No central
 * registration list needed.
 *
 * Usage:
 *
 *   #include <snesrecomp/recomp_patch.h>
 *
 *   RECOMP_PATCH(smk_80FF70, 0x80FF70) {
 *       // function body — same shape as a regular void(void) function
 *   }
 *
 * The function symbol `smk_80FF70` is normal C: forward-declare it in a
 * header and call it directly, OR look it up via func_table_call(0x80FF70).
 *
 * Mod / override pattern: link a second .obj that defines another
 * RECOMP_PATCH at the same SNES address with a different function name.
 * The last constructor to run wins, so put mod objects after the original
 * in the link order.
 *
 * Static library / dead-strip safety:
 *   On MSVC, each patch emits a /INCLUDE: linker directive on its
 *   constructor pointer symbol. This forces the linker to pull the
 *   containing .obj out of static libraries even when no other code
 *   references the patch directly.
 */

#include "snesrecomp/func_table.h"

/* Token paste / stringify helpers (need indirection so __LINE__/args expand) */
#define RECOMP_PASTE2_(a, b) a##b
#define RECOMP_PASTE2(a, b)  RECOMP_PASTE2_(a, b)
#define RECOMP_PASTE3_(a, b, c) a##b##c
#define RECOMP_PASTE3(a, b, c)  RECOMP_PASTE3_(a, b, c)
#define RECOMP_STRINGIFY_(x) #x
#define RECOMP_STRINGIFY(x)  RECOMP_STRINGIFY_(x)

#if defined(_MSC_VER)
    /*
     * MSVC: place a function pointer in the .CRT$XCU section (the C runtime
     * initializer table). The CRT walks this section before main() and
     * invokes each pointer.
     *
     * We make the pointer external (not static) and emit a /INCLUDE: linker
     * directive so static-library .obj files are not dead-stripped when the
     * patch's only reference is the dispatch table itself. On x64 Windows
     * C symbols are unmangled, so the directive name matches the C name.
     */
    #pragma section(".CRT$XCU", read)
    #define RECOMP_PATCH_IMPL(fn_name, snes_addr, ctor, ctor_ptr)             \
        void fn_name(void);                                                    \
        static void ctor(void) {                                               \
            func_table_register((uint32_t)(snes_addr), fn_name);               \
        }                                                                      \
        __pragma(comment(linker, "/include:" RECOMP_STRINGIFY(ctor_ptr)))      \
        __declspec(allocate(".CRT$XCU"))                                       \
        void (* const ctor_ptr)(void) = ctor;                                  \
        void fn_name(void)
#elif defined(__GNUC__) || defined(__clang__)
    #define RECOMP_PATCH_IMPL(fn_name, snes_addr, ctor, ctor_ptr)             \
        void fn_name(void);                                                    \
        __attribute__((constructor)) static void ctor(void) {                  \
            func_table_register((uint32_t)(snes_addr), fn_name);               \
        }                                                                      \
        /* ctor_ptr unused on GCC/Clang — constructor attribute is enough */   \
        void fn_name(void)
#else
    #error "RECOMP_PATCH: unsupported compiler — needs MSVC or GCC/Clang ctor support"
#endif

#define RECOMP_PATCH(fn_name, snes_addr)                                      \
    RECOMP_PATCH_IMPL(fn_name, snes_addr,                                     \
        RECOMP_PASTE3(recomp_register_, fn_name, _ctor),                      \
        RECOMP_PASTE3(recomp_register_, fn_name, _ctor_ptr))

#endif /* SNESRECOMP_RECOMP_PATCH_H */

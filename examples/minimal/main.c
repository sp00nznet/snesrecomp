/*
 * Minimal snesrecomp example
 *
 * This shows the bare minimum to get a SNES ROM running with
 * snesrecomp providing all the hardware. Obviously a real recomp
 * project would have recompiled game functions instead of the
 * placeholder frame logic here.
 *
 * Build:
 *   cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
 *   cmake --build build --config Debug
 *
 * Run:
 *   build/Debug/snesrecomp_minimal "Super Mario Kart (USA).sfc"
 */

#include <snesrecomp/snesrecomp.h>
#include <stdio.h>

int main(int argc, char *argv[]) {
    const char *rom_path = (argc >= 2) ? argv[1] : "game.sfc";

    printf("snesrecomp minimal example\n");
    printf("==========================\n\n");

    /* Initialize snesrecomp (creates LakeSnes instance + SDL2 window) */
    if (!snesrecomp_init("snesrecomp - Minimal Example", 3)) {
        fprintf(stderr, "Failed to initialize snesrecomp\n");
        return 1;
    }

    /* Load ROM */
    printf("Loading: %s\n", rom_path);
    if (!snesrecomp_load_rom(rom_path)) {
        fprintf(stderr, "Failed to load ROM\n");
        snesrecomp_shutdown();
        return 1;
    }

    printf("\nRunning... (press Escape to quit)\n\n");

    /* Main loop */
    while (snesrecomp_begin_frame()) {
        /*
         * In a real recomp project, this is where you'd call your
         * recompiled game functions:
         *
         *   func_table_call(0x80803A);  // game main loop
         *
         * The recompiled code would use bus_read8/bus_write8 for
         * memory access, which routes to real LakeSnes hardware.
         *
         * For this demo, we just let the frame render the backdrop.
         */

        snesrecomp_end_frame();
    }

    snesrecomp_shutdown();
    return 0;
}

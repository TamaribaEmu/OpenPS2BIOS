/* SPDX-License-Identifier: MIT */
#include "openbios/platform.h"
#include "openbios/rom.h"
#include "openbios/disc.h"
#include "openbios/sif.h"
#include "openbios/iop.h"
#include "openbios/ioman.h"

void ob_iop_main(void) {
    struct ob_rom_file version;
    ob_iop_log_init();
    ob_puts("[openbios:iop] ready\n");
    if (ob_rom_find((const void *)0xbfc00000u, OB_ROM_SIZE,
                    "ROMVER", &version) != OB_OK || version.size != 14) {
        ob_puts("[openbios:iop] invalid ROM directory\n");
        ob_halt();
    }
    ob_sysmem_init();
    ob_loadcore_init();
    ob_iop_libraries_init();
    ob_iop_kernel_libraries_init();
    ob_iop_dmacman_init();
    ob_iop_cdvdman_init();
    ob_iop_ioman_init();
    ob_iop_modload_init();
    ob_iop_secrman_init();
    ob_iop_timrman_init();
    ob_iop_sifrpc_init();
    ob_iop_kernel_init();
    ob_iop_fileio_init();
    ob_iop_cd_servers_init();
    ob_iop_sif_init();
    ob_iop_disc_service();
}

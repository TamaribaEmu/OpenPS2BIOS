/* SPDX-License-Identifier: MIT */
/* Boot screen (rom0:OSDSYS). Waits for a PlayStation 2 disc, reads
   SYSTEM.CNF and starts the disc's program through EELOAD. Drawing uses
   sprites sent to the GS over GIF DMA; the disc is read through the IOP
   bootstrap mailbox. */
#include "openbios/base.h"
#include "openbios/disc.h"
#include "openbios/iso9660.h"
#include "openbios/platform.h"
#include "font.h"

int osd_syscall(int number, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
void osd_write64(uint32_t address, uint32_t low, uint32_t high);

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
/* Registers through KSEG1 (this program runs in kernel mode). */
#define GS_PMODE 0xb2000000u
#define GS_DISPFB1 0xb2000070u
#define GS_DISPLAY1 0xb2000080u
#define GS_BGCOLOR 0xb20000e0u
#define GS_CSR 0xb2001000u
#define D2_CHCR 0xb000a000u
#define D2_MADR 0xb000a010u
#define D2_QWC 0xb000a020u
#define D_CTRL 0xb000e000u
#define GIF_CTRL 0xb0003000u
#define EELOAD_ADDRESS 0x00082000u

#define WIDTH 640
#define HEIGHT 448

/* ---- GS drawing ---- */
#define PACKET_QWORDS 8192u
static uint32_t packet[PACKET_QWORDS][4] __attribute__((aligned(64)));
static unsigned used;

static void put_reg(uint32_t reg, uint32_t low, uint32_t high) {
    if (used >= PACKET_QWORDS) return;
    packet[used][0] = low;
    packet[used][1] = high;
    packet[used][2] = reg;
    packet[used][3] = 0;
    ++used;
}
static void begin(void) { used = 1; } /* qword 0 is the GIF tag */
static void kick(void) {
    unsigned i;
    /* GIF tag: NLOOP = A+D writes, EOP, PACKED, one register descriptor (A+D). */
    packet[0][0] = (used - 1) | (1u << 15);
    packet[0][1] = 1u << 28;
    packet[0][2] = 0xe;
    packet[0][3] = 0;
    osd_syscall(0x64, 0, 0, 0, 0); /* FlushCache: write the packet back to RAM */
    REG32(D2_MADR) = (uint32_t)(uintptr_t)packet & 0x1fffffffu;
    REG32(D2_QWC) = used;
    REG32(D2_CHCR) = 0x101; /* from memory, normal mode, start */
    for (i = 0; i < 100000000u && (REG32(D2_CHCR) & 0x100); ++i) {}
}
static void rect(int x, int y, int w, int h, uint32_t rgba) {
    put_reg(0x01, rgba, 0x3f800000u); /* RGBAQ, Q = 1.0 */
    put_reg(0x05, (uint32_t)(x << 4) | (uint32_t)(y << 4) << 16, 0);            /* XYZ2 */
    put_reg(0x05, (uint32_t)((x + w) << 4) | (uint32_t)((y + h) << 4) << 16, 0); /* XYZ2 */
}
static unsigned text_length(const char *s) { unsigned n = 0; while (s[n]) ++n; return n; }
/* Text in 6x8 cells at the given scale; runs of lit pixels become one sprite. */
static void text(const char *s, int y, int scale) {
    int width = (int)text_length(s) * 6 * scale - scale;
    int x0 = (WIDTH - width) / 2, row, col;
    for (; *s; ++s, x0 += 6 * scale) {
        unsigned char c = (unsigned char)*s;
        const unsigned char *g = font5x7[(c < 32 || c > 126 ? '?' : c) - 32];
        for (row = 0; row < 7; ++row)
            for (col = 0; col < 5; ) {
                int start;
                if (!(g[row] & (0x10 >> col))) { ++col; continue; }
                start = col;
                while (col < 5 && (g[row] & (0x10 >> col))) ++col;
                rect(x0 + start * scale, y + row * scale, (col - start) * scale, scale, 0x80ffffffu);
            }
    }
}

static void display_init(void) {
    osd_syscall(0x02, 1, 2, 0, 0); /* SetGsCrt: interlaced, NTSC, FFMD 0 (each field reads alternate lines) */
    osd_write64(GS_PMODE, 0xff25, 0);  /* circuit 1, fixed alpha 0xff */
    osd_write64(GS_DISPFB1, 10u << 9, 0); /* FBP 0, FBW 640/64, PSMCT32 */
    /* DX 652, DY 50, MAGH 4x, DW 2559, DH 447 */
    osd_write64(GS_DISPLAY1, 652u | 50u << 12 | 3u << 23, 2559u | 447u << 12);
    osd_write64(GS_BGCOLOR, 0, 0);
    REG32(D_CTRL) |= 1;   /* DMAC enable */
    REG32(GIF_CTRL) = 1;  /* GIF reset */
}
static void screen(const char *line1, const char *line2) {
    begin();
    put_reg(0x4c, 10u << 16, 0);               /* FRAME_1: FBP 0, FBW 10, PSMCT32 */
    put_reg(0x4e, 0x96, 1);                    /* ZBUF_1: Z writes masked */
    put_reg(0x18, 0, 0);                       /* XYOFFSET_1 */
    put_reg(0x40, (WIDTH - 1) << 16, (HEIGHT - 1) << 16); /* SCISSOR_1 */
    put_reg(0x47, 0x30000, 0);                 /* TEST_1: depth test always */
    put_reg(0x1a, 1, 0);                       /* PRMODECONT: use PRIM */
    put_reg(0x00, 6, 0);                       /* PRIM: sprite */
    rect(0, 0, WIDTH, HEIGHT, 0x80000000u);    /* black */
    text("OpenPS2BIOS", 120, 6);
    text("Version " OB_VERSION, 180, 2);
    if (line1) text(line1, 250, 3);
    if (line2) text(line2, 300, 3);
    kick();
}
static void wait_vsync(void) {
    unsigned i;
    REG32(GS_CSR) = 8; /* acknowledge VSINT */
    for (i = 0; i < 50000000u && !(REG32(GS_CSR) & 8); ++i) {}
}
static void wait_frames(int n) { while (n-- > 0) wait_vsync(); }

/* ---- disc ---- */
static char boot_path[260];
static char shown_name[64];
static int read_boot_path(void) {
    struct ob_iso iso;
    char raw[256];
    unsigned i = 0, j = 0;
    const char *name;
    ob_ee_disc_invalidate();
    if (ob_iso_mount(&iso, ob_ee_disc_read, 0, UINT32_MAX)) return -1;
    if (ob_iso_boot_path(&iso, raw, sizeof(raw))) return -2;
    /* Normalise to "cdrom0:\NAME;1". */
    while (raw[i] && raw[i] != ':' && j < sizeof(boot_path) - 2) boot_path[j++] = raw[i++];
    if (raw[i] == ':') {
        boot_path[j++] = raw[i++];
        if (raw[i] != '\\' && raw[i] != '/') boot_path[j++] = '\\';
    }
    while (raw[i] && j < sizeof(boot_path) - 1) boot_path[j++] = raw[i++];
    boot_path[j] = 0;
    name = boot_path;
    for (i = 0; boot_path[i]; ++i) if (boot_path[i] == '\\' || boot_path[i] == ':') name = boot_path + i + 1;
    for (i = 0; name[i] && name[i] != ';' && i < sizeof(shown_name) - 1; ++i) shown_name[i] = name[i];
    shown_name[i] = 0;
    return 0;
}
static void start_disc(void) {
    static char eeload_name[] = "EELOAD";
    static uint32_t argv[2];
    argv[0] = (uint32_t)(uintptr_t)eeload_name;
    argv[1] = (uint32_t)(uintptr_t)boot_path;
    /* EELOAD with "EELOAD <program>" runs the program (LoadExecPS2). */
    osd_syscall(0x07, EELOAD_ADDRESS, 0, 2, (uint32_t)(uintptr_t)argv);
}

enum { NO_DISC, DETECTING, PS2, PS1, AUDIO, DVD_VIDEO, UNREADABLE };
static int classify(int type) {
    if (type < 0) return DETECTING;
    switch (type) {
    case 0x00: return NO_DISC;
    case 0x12: case 0x13: case 0x14: return PS2;
    case 0x10: case 0x11: return PS1;
    case 0xfd: return AUDIO;
    case 0xfe: return DVD_VIDEO;
    case 0xff: return UNREADABLE;
    default: return DETECTING; /* 0x01-0x05: the drive is still identifying the disc */
    }
}

void osd_main(void) {
    int state = -1, failed_type = -1;
    ob_puts("[openbios:osd] boot screen\n");
    display_init();
    for (;;) {
        int status = ob_ee_disc_status(), type = status & 0xff, kind = classify(status < 0 ? -1 : type);
        if (kind != state) { ob_puts("[openbios:osd] drive "); ob_puthex((uint32_t)status); ob_puts("\n"); }
        if (kind == PS2 && type != failed_type) {
            screen("Reading disc...", 0);
            wait_vsync();
            if (!read_boot_path()) {
                char line[96] = "Starting ";
                ob_copy(line + 9, shown_name, text_length(shown_name) + 1);
                ob_puts("[openbios:osd] starting "); ob_puts(boot_path); ob_puts("\n");
                screen(line, 0);
                wait_frames(45);
                start_disc();
            }
            ob_puts("[openbios:osd] disc could not be read\n");
            failed_type = type;
            screen("This disc could not be read.", "Please check the disc.");
            state = -2;
        } else if (kind != PS2 && kind != state) {
            failed_type = -1;
            switch (kind) {
            case NO_DISC: screen("Please insert a", "PlayStation 2 disc."); break;
            case DETECTING: screen("Reading disc...", 0); break;
            case PS1: screen("PlayStation discs are", "not supported."); break;
            case AUDIO: screen("Audio CDs are not supported.", 0); break;
            case DVD_VIDEO: screen("DVD video is not supported.", 0); break;
            default: screen("This disc could not be read.", 0); break;
            }
            state = kind;
        } else if (kind != PS2 && type != failed_type) failed_type = -1;
        wait_frames(15);
    }
}

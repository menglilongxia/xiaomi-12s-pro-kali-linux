/*
 * linuxboot.efi — Minimal freestanding AArch64 UEFI Linux Chainloader for Xiaomi 12S Pro (unicorn)
 *
 * Reads the XNUP container at PAYLOAD_BASE (0xB0000000), prints live UEFI/GOP diagnostics,
 * installs unicorn.dtb via EFI_DTB_TABLE_GUID, exposes initrd via LINUX_EFI_INITRD_MEDIA_GUID
 * (EFI_LOAD_FILE2_PROTOCOL), and starts Linux vmlinuz.efi (CONFIG_EFI_ZBOOT / CONFIG_EFI_STUB).
 */
#include <stdint.h>
#include <stddef.h>

#define EFIAPI
typedef uint64_t efi_status;
typedef void *efi_handle;
typedef uint16_t char16;

#define EFI_SUCCESS          0ULL
#define EFI_ERROR_BIT        0x8000000000000000ULL
#define EFI_ERROR(s)         (((s) & EFI_ERROR_BIT) != 0)
#define EFI_INVALID_PARAM    (EFI_ERROR_BIT | 2)
#define EFI_UNSUPPORTED      (EFI_ERROR_BIT | 3)
#define EFI_BUFFER_TOO_SMALL (EFI_ERROR_BIT | 5)

#ifndef PAYLOAD_BASE
#define PAYLOAD_BASE 0xB0000000ULL
#endif
#define PAYLOAD_MAGIC 0x50554E58u /* "XNUP" */

typedef struct {
    uint32_t a;
    uint16_t b;
    uint16_t c;
    uint8_t  d[8];
} efi_guid;

typedef struct {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
} efi_table_header;

struct efi_text_output;
typedef struct efi_text_output {
    void *reset;
    efi_status (EFIAPI *output_string)(struct efi_text_output *self, const char16 *str);
    void *test_string;
    void *query_mode;
    void *set_mode;
    void *set_attribute;
    efi_status (EFIAPI *clear_screen)(struct efi_text_output *self);
} efi_text_output;

enum { EfiLoaderData = 2, EfiBootServicesData = 4 };
enum { AllocateAnyPages = 0 };
enum { EFI_NATIVE_INTERFACE = 0 };

typedef struct {
    efi_table_header hdr;
    void *raise_tpl, *restore_tpl;
    efi_status (EFIAPI *allocate_pages)(int type, int mem_type, uint64_t pages, uint64_t *addr);
    efi_status (EFIAPI *free_pages)(uint64_t addr, uint64_t pages);
    efi_status (EFIAPI *get_memory_map)(uint64_t *size, void *map, uint64_t *key, uint64_t *desc_size, uint32_t *desc_ver);
    efi_status (EFIAPI *allocate_pool)(int type, uint64_t size, void **buf);
    efi_status (EFIAPI *free_pool)(void *buf);
    void *create_event, *set_timer, *wait_for_event;
    void *signal_event, *close_event, *check_event;
    efi_status (EFIAPI *install_protocol_interface)(efi_handle *handle, const efi_guid *protocol, int iface_type, void *iface);
    void *reinstall_protocol_interface, *uninstall_protocol_interface;
    efi_status (EFIAPI *handle_protocol)(efi_handle h, const efi_guid *guid, void **iface);
    void *reserved, *register_protocol_notify, *locate_handle, *locate_device_path;
    efi_status (EFIAPI *install_configuration_table)(const efi_guid *guid, void *table);
    efi_status (EFIAPI *load_image)(uint8_t boot_policy, efi_handle parent, const void *dev_path, void *src_buf, uint64_t src_size, efi_handle *img_handle);
    efi_status (EFIAPI *start_image)(efi_handle img_handle, uint64_t *exit_data_size, char16 **exit_data);
    void *exit, *unload_image, *exit_boot_services, *get_next_monotonic_count;
    efi_status (EFIAPI *stall)(uint64_t microseconds);
    efi_status (EFIAPI *set_watchdog_timer)(uint64_t timeout, uint64_t code, uint64_t size, char16 *data);
    void *connect_controller, *disconnect_controller, *open_protocol, *close_protocol, *open_protocol_information;
    void *protocols_per_handle, *locate_handle_buffer;
    efi_status (EFIAPI *locate_protocol)(const efi_guid *guid, void *registration, void **iface);
} efi_boot_services;

typedef struct {
    efi_table_header hdr;
    char16 *vendor;
    uint32_t revision, pad;
    efi_handle con_in_handle; void *con_in;
    efi_handle con_out_handle; efi_text_output *con_out;
    efi_handle stderr_handle; efi_text_output *std_err;
    void *rt;
    efi_boot_services *bs;
    uint64_t nr_tables;
    struct { efi_guid guid; void *table; } *tables;
} efi_system_table;

typedef struct {
    uint32_t version, width, height, pixel_format;
    uint32_t pixel_mask[4];
    uint32_t pixels_per_scanline;
} efi_gop_mode_info;

typedef struct {
    uint32_t max_mode, mode;
    efi_gop_mode_info *info;
    uint64_t info_size;
    uint64_t fb_base;
    uint64_t fb_size;
} efi_gop_mode;

typedef struct {
    void *query_mode, *set_mode, *blt;
    efi_gop_mode *mode;
} efi_gop;

typedef struct {
    uint32_t revision;
    uint32_t pad;
    efi_handle parent_handle;
    efi_system_table *system_table;
    efi_handle device_handle;
    void *file_path;
    void *reserved;
    uint32_t load_options_size;
    uint32_t pad2;
    void *load_options;
    void *image_base;
    uint64_t image_size;
    uint32_t image_code_type;
    uint32_t image_data_type;
    void *unload;
} efi_loaded_image;

struct efi_load_file2;
typedef struct efi_load_file2 {
    efi_status (EFIAPI *load_file)(
        struct efi_load_file2 *self,
        void *file_path,
        uint8_t boot_policy,
        uint64_t *buffer_size,
        void *buffer
    );
} efi_load_file2;

#pragma pack(push, 1)
typedef struct {
    uint8_t  type;
    uint8_t  subtype;
    uint16_t length;
    efi_guid guid;
    uint8_t  end_type;
    uint8_t  end_subtype;
    uint16_t end_length;
} initrd_device_path;
#pragma pack(pop)

typedef struct {
    uint32_t magic;
    uint32_t size;
} xnup_header;

typedef struct {
    char     name[16];
    uint32_t size;
    uint32_t reserved;
} xnup_entry;

static const efi_guid g_gop_guid         = { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } };
static const efi_guid g_loaded_img_guid  = { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static const efi_guid g_devpath_guid     = { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static const efi_guid g_dtb_table_guid   = { 0xb1b621d5, 0xf19c, 0x41a5, { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } };
static const efi_guid g_load_file2_guid  = { 0x4006c0c1, 0xfcb3, 0x403e, { 0x99, 0x6d, 0x4a, 0x6c, 0x87, 0x24, 0xe0, 0x6d } };
static const efi_guid g_initrd_media_guid = { 0x5568e427, 0x68fc, 0x4f3d, { 0xac, 0x74, 0xca, 0x55, 0x52, 0x31, 0xcc, 0x68 } };

static efi_system_table *g_st;
static efi_boot_services *g_bs;
static const uint8_t *g_initrd_data;
static uint64_t g_initrd_size;

static void my_memcpy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void my_memset(void *dst, uint8_t v, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = v;
}

static int name_eq(const char *a, const char *b) {
    for (int i = 0; i < 16; i++) {
        if (a[i] != b[i]) return 0;
        if (b[i] == 0) return 1;
    }
    return 1;
}

static void puts_ascii(const char *s) {
    char16 buf[128];
    int i = 0;
    if (!g_st || !g_st->con_out) return;
    while (*s) {
        if (*s == '\n') {
            if (i >= 125) { buf[i] = 0; g_st->con_out->output_string(g_st->con_out, buf); i = 0; }
            buf[i++] = '\r';
        }
        buf[i++] = (char16)(uint8_t)*s++;
        if (i >= 126) {
            buf[i] = 0;
            g_st->con_out->output_string(g_st->con_out, buf);
            i = 0;
        }
    }
    if (i > 0) {
        buf[i] = 0;
        g_st->con_out->output_string(g_st->con_out, buf);
    }
}

static void put_hex(uint64_t v) {
    static const char hx[] = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buf[2 + i] = hx[(v >> (60 - i * 4)) & 0xF];
    }
    buf[18] = 0;
    puts_ascii(buf);
}

static void put_dec(uint64_t v) {
    char buf[24];
    int i = 0;
    if (v == 0) { puts_ascii("0"); return; }
    while (v > 0 && i < 22) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    char out[24];
    for (int j = 0; j < i; j++) out[j] = buf[i - 1 - j];
    out[i] = 0;
    puts_ascii(out);
}

static efi_status EFIAPI initrd_load_file(
    efi_load_file2 *self,
    void *file_path,
    uint8_t boot_policy,
    uint64_t *buffer_size,
    void *buffer
) {
    (void)self;
    (void)file_path;
    if (boot_policy) return EFI_UNSUPPORTED;
    if (!buffer_size) return EFI_INVALID_PARAM;
    if (!buffer || *buffer_size < g_initrd_size) {
        *buffer_size = g_initrd_size;
        return EFI_BUFFER_TOO_SMALL;
    }
    my_memcpy(buffer, g_initrd_data, g_initrd_size);
    *buffer_size = g_initrd_size;
    return EFI_SUCCESS;
}

static efi_load_file2 g_initrd_lf2 = { initrd_load_file };
static initrd_device_path g_initrd_dp;

static void halt_forever(void) {
    puts_ascii("\n[linuxboot] Halted. Hold Power+VolDown to enter fastboot.\n");
    for (;;) {
        __asm__ volatile ("wfi");
    }
}

efi_status EFIAPI efi_main(efi_handle image_handle, efi_system_table *st) {
    g_st = st;
    g_bs = st->bs;

    g_bs->set_watchdog_timer(0, 0, 0, NULL);

    if (st->con_out && st->con_out->clear_screen) {
        st->con_out->clear_screen(st->con_out);
    }

    puts_ascii("=== Xiaomi 12S Pro (unicorn) UEFI Linux Chainloader ===\n");

    uint64_t cur_el = 0, midr = 0;
    __asm__ volatile ("mrs %0, CurrentEL" : "=r"(cur_el));
    __asm__ volatile ("mrs %0, MIDR_EL1" : "=r"(midr));
    puts_ascii("[cpu] CurrentEL="); put_dec(cur_el >> 2);
    puts_ascii(" MIDR="); put_hex(midr); puts_ascii("\n");

    efi_gop *gop = NULL;
    if (!EFI_ERROR(g_bs->locate_protocol(&g_gop_guid, NULL, (void **)&gop)) &&
        gop && gop->mode && gop->mode->info) {
        puts_ascii("[gop] ");
        put_dec(gop->mode->info->width); puts_ascii("x"); put_dec(gop->mode->info->height);
        puts_ascii(" stride="); put_dec(gop->mode->info->pixels_per_scanline);
        puts_ascii(" fmt="); put_dec(gop->mode->info->pixel_format);
        puts_ascii(" fb="); put_hex(gop->mode->fb_base);
        puts_ascii(" sz="); put_hex(gop->mode->fb_size);
        puts_ascii("\n");
    } else {
        puts_ascii("[gop] NOT FOUND\n");
    }

    const uint8_t *base = (const uint8_t *)(uintptr_t)PAYLOAD_BASE;
    const xnup_header *hdr = (const xnup_header *)base;
    puts_ascii("[xnup] base="); put_hex(PAYLOAD_BASE);
    puts_ascii(" magic="); put_hex(hdr->magic);
    puts_ascii(" size="); put_dec(hdr->size); puts_ascii("\n");

    if (hdr->magic != PAYLOAD_MAGIC) {
        puts_ascii("[ERR] XNUP magic mismatch!\n");
        halt_forever();
    }

    const uint8_t *kernel_ptr = NULL; uint64_t kernel_sz = 0;
    const uint8_t *dtb_ptr = NULL;    uint64_t dtb_sz = 0;
    const uint8_t *initrd_ptr = NULL; uint64_t initrd_sz = 0;
    const char *cmdline_ptr = NULL;   uint64_t cmdline_sz = 0;

    uint64_t off = sizeof(xnup_header);
    for (int i = 0; i < 32; i++) {
        const xnup_entry *e = (const xnup_entry *)(base + off);
        if (e->size == 0 && e->name[0] == 0) break;
        const uint8_t *data = base + off + sizeof(xnup_entry);
        puts_ascii("  entry: "); puts_ascii(e->name);
        puts_ascii(" sz="); put_dec(e->size);
        puts_ascii(" hdr="); put_hex(*(const uint32_t *)data);
        puts_ascii("\n");

        if (name_eq(e->name, "kernel") || name_eq(e->name, "vmlinuz.efi")) {
            kernel_ptr = data; kernel_sz = e->size;
        } else if (name_eq(e->name, "dtb")) {
            dtb_ptr = data; dtb_sz = e->size;
        } else if (name_eq(e->name, "initrd") || name_eq(e->name, "ramdisk")) {
            initrd_ptr = data; initrd_sz = e->size;
        } else if (name_eq(e->name, "cmdline")) {
            cmdline_ptr = (const char *)data; cmdline_sz = e->size;
        }
        off += sizeof(xnup_entry) + ((e->size + 15ULL) & ~15ULL);
    }

    if (!kernel_ptr || kernel_sz < 64) {
        puts_ascii("[ERR] No kernel entry in XNUP!\n");
        halt_forever();
    }
    if (kernel_ptr[0] != 'M' || kernel_ptr[1] != 'Z') {
        puts_ascii("[ERR] Kernel does not start with MZ (EFI stub)!\n");
        halt_forever();
    }

    /* 1. Install DTB into EFI Configuration Table */
    if (dtb_ptr && dtb_sz > 64) {
        uint64_t dtb_pages = (dtb_sz + 65536ULL + 4095ULL) >> 12;
        uint64_t dtb_addr = 0;
        efi_status s = g_bs->allocate_pages(AllocateAnyPages, EfiLoaderData, dtb_pages, &dtb_addr);
        if (EFI_ERROR(s)) {
            puts_ascii("[ERR] allocate_pages(dtb) failed: "); put_hex(s); puts_ascii("\n");
            halt_forever();
        }
        my_memset((void *)(uintptr_t)dtb_addr, 0, dtb_pages << 12);
        my_memcpy((void *)(uintptr_t)dtb_addr, dtb_ptr, dtb_sz);
        s = g_bs->install_configuration_table(&g_dtb_table_guid, (void *)(uintptr_t)dtb_addr);
        puts_ascii("[dtb] installed at "); put_hex(dtb_addr);
        puts_ascii(" status="); put_hex(s); puts_ascii("\n");
        if (EFI_ERROR(s)) halt_forever();
    } else {
        puts_ascii("[WARN] No DTB entry in XNUP!\n");
    }

    /* 2. Register EFI_LOAD_FILE2_PROTOCOL for initrd */
    if (initrd_ptr && initrd_sz > 0) {
        g_initrd_data = initrd_ptr;
        g_initrd_size = initrd_sz;
        g_initrd_dp.type = 0x04;
        g_initrd_dp.subtype = 0x03;
        g_initrd_dp.length = 20;
        g_initrd_dp.guid = g_initrd_media_guid;
        g_initrd_dp.end_type = 0x7F;
        g_initrd_dp.end_subtype = 0xFF;
        g_initrd_dp.end_length = 4;

        efi_handle h = NULL;
        efi_status s1 = g_bs->install_protocol_interface(&h, &g_devpath_guid, EFI_NATIVE_INTERFACE, &g_initrd_dp);
        efi_status s2 = g_bs->install_protocol_interface(&h, &g_load_file2_guid, EFI_NATIVE_INTERFACE, &g_initrd_lf2);
        puts_ascii("[initrd] LoadFile2 registered: "); put_dec(initrd_sz);
        puts_ascii(" bytes s1="); put_hex(s1); puts_ascii(" s2="); put_hex(s2); puts_ascii("\n");
        if (EFI_ERROR(s1) || EFI_ERROR(s2)) halt_forever();
    }

    /* 3. LoadImage(vmlinuz.efi) */
    efi_handle k_handle = NULL;
    efi_status st_load = g_bs->load_image(0, image_handle, NULL, (void *)kernel_ptr, kernel_sz, &k_handle);
    puts_ascii("[linux] LoadImage status="); put_hex(st_load); puts_ascii("\n");
    if (EFI_ERROR(st_load)) halt_forever();

    /* 4. Set UTF-16 LoadOptions (kernel cmdline) */
    static char16 opt_buf[1024];
    const char *default_cmd =
        "efi=noruntime console=tty0 earlycon=efifb fbcon=font:TER16x32 "
        "loglevel=7 ignore_loglevel clk_ignore_unused pd_ignore_unused rootwait";
    const char *src = (cmdline_ptr && cmdline_sz > 0) ? cmdline_ptr : default_cmd;
    uint64_t max_n = (cmdline_ptr && cmdline_sz > 0) ? cmdline_sz : 1000;
    uint64_t n = 0;
    while (n < max_n && n < 1020 && src[n] != 0) {
        opt_buf[n] = (char16)(uint8_t)src[n];
        n++;
    }
    opt_buf[n] = 0;

    efi_loaded_image *li = NULL;
    if (!EFI_ERROR(g_bs->handle_protocol(k_handle, &g_loaded_img_guid, (void **)&li)) && li) {
        li->load_options = opt_buf;
        li->load_options_size = (uint32_t)((n + 1) * sizeof(char16));
        puts_ascii("[linux] cmdline: "); puts_ascii(src); puts_ascii("\n");
    }

    puts_ascii("[linux] Starting Linux 7.2.10 EFI stub...\n");

    efi_status st_start = g_bs->start_image(k_handle, NULL, NULL);
    puts_ascii("[ERR] StartImage returned: "); put_hex(st_start); puts_ascii("\n");
    halt_forever();
    return st_start;
}

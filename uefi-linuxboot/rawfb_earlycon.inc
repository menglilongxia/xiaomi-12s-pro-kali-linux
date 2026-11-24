/* ===== Fast zero-scroll 16x32 rawfb earlycon + pstore pmsg mirror (0xA7200000) ===== */
#include <asm/early_ioremap.h>
extern const struct font_desc font_vga_8x16;

#define RAWFB_PHYS   0xB8000000UL
#define RAWFB_W      1440U
#define RAWFB_H      3200U
#define RAWFB_STRIDE (RAWFB_W * 4U)
#define RAWFB_CW     16U
#define RAWFB_CH     32U
#define RAWFB_COLS   (RAWFB_W / RAWFB_CW)
#define RAWFB_ROWS   (RAWFB_H / RAWFB_CH)

#define PMSG_PHYS    0xA7200000UL
#define PMSG_SIZE    0x00200000UL
#define PMSG_DATA_SZ (PMSG_SIZE - 12UL)
#define PMSG_SIG     0x43474244U /* "DBGC" */

static void __iomem *rawfb_va;
static void __iomem *pmsg_va;
static u32 pmsg_pos;
static u32 rawfb_col;
static u32 rawfb_row;
static struct console *rawfb_con;

static int __init rawfb_remap(void)
{
	if (rawfb_con) {
		rawfb_va = memremap(RAWFB_PHYS, 0x02B00000UL, MEMREMAP_WC);
		pmsg_va  = memremap(PMSG_PHYS, PMSG_SIZE, MEMREMAP_WC);
	}
	return 0;
}
early_initcall(rawfb_remap);

static void pmsg_append(const char *s, unsigned int n)
{
	unsigned int i;
	if (!n)
		return;
	if (pmsg_va) {
		volatile u32 *hdr = (volatile u32 *)pmsg_va;
		volatile u8 *data = (volatile u8 *)pmsg_va + 12;
		hdr[0] = PMSG_SIG;
		for (i = 0; i < n && pmsg_pos < PMSG_DATA_SZ; i++) {
			data[pmsg_pos++] = (u8)s[i];
		}
		hdr[1] = pmsg_pos; /* start (next write pos) */
		hdr[2] = pmsg_pos; /* size */
		dsb(sy);
	} else {
		/* Early boot before memremap(): map first 240 KB of pmsg region */
		u8 *m = early_ioremap(PMSG_PHYS, 0x3C000UL);
		if (!m)
			return;
		((volatile u32 *)m)[0] = PMSG_SIG;
		for (i = 0; i < n && pmsg_pos < (0x3C000U - 12U); i++) {
			m[12 + pmsg_pos++] = (u8)s[i];
		}
		((volatile u32 *)m)[1] = pmsg_pos;
		((volatile u32 *)m)[2] = pmsg_pos;
		dsb(sy);
		early_iounmap(m, 0x3C000UL);
	}
}

static void rawfb_clear_row(u32 row)
{
	u32 y0 = row * RAWFB_CH;
	u32 bytes = RAWFB_CH * RAWFB_STRIDE;
	u32 *p;
	u32 i;

	if (rawfb_va) {
		p = (u32 *)((u8 *)rawfb_va + y0 * RAWFB_STRIDE);
		for (i = 0; i < RAWFB_CH * RAWFB_W; i++)
			p[i] = 0xFF001020U;
	} else {
		p = early_ioremap(RAWFB_PHYS + (unsigned long)y0 * RAWFB_STRIDE, bytes);
		if (!p)
			return;
		for (i = 0; i < RAWFB_CH * RAWFB_W; i++)
			p[i] = 0xFF001020U;
		early_iounmap(p, bytes);
	}
}

static void rawfb_draw_line_buf(u32 row, const char *line, u32 len)
{
	u32 y0 = row * RAWFB_CH;
	u32 bytes = RAWFB_CH * RAWFB_STRIDE;
	u32 *p;
	u32 c, gy, gx;

	if (rawfb_va) {
		p = (u32 *)((u8 *)rawfb_va + y0 * RAWFB_STRIDE);
	} else {
		p = early_ioremap(RAWFB_PHYS + (unsigned long)y0 * RAWFB_STRIDE, bytes);
		if (!p)
			return;
	}

	for (c = 0; c < len && c < RAWFB_COLS; c++) {
		unsigned char ch = (unsigned char)line[c];
		const u8 *glyph = (const u8 *)font_vga_8x16.data + ch * 16;
		u32 x0 = c * RAWFB_CW;

		for (gy = 0; gy < 16; gy++) {
			u8 bits = glyph[gy];
			u32 *r0 = p + (gy * 2) * RAWFB_W + x0;
			u32 *r1 = p + (gy * 2 + 1) * RAWFB_W + x0;

			for (gx = 0; gx < 8; gx++) {
				u32 col = (bits & (0x80 >> gx)) ? 0xFFFFFFFFU : 0xFF001020U;
				r0[gx * 2] = col;
				r0[gx * 2 + 1] = col;
				r1[gx * 2] = col;
				r1[gx * 2 + 1] = col;
			}
		}
	}

	if (!rawfb_va)
		early_iounmap(p, bytes);
}

static char rawfb_line[RAWFB_COLS + 2];

static void rawfb_write(struct console *con, const char *s, unsigned int n)
{
	unsigned int i;

	pmsg_append(s, n);

	for (i = 0; i < n; i++) {
		char ch = s[i];
		if (ch == '\r')
			continue;
		if (ch == '\n') {
			rawfb_draw_line_buf(rawfb_row, rawfb_line, rawfb_col);
			rawfb_col = 0;
			rawfb_row = (rawfb_row + 1) % RAWFB_ROWS;
			rawfb_clear_row(rawfb_row);
			continue;
		}
		if (rawfb_col < RAWFB_COLS)
			rawfb_line[rawfb_col++] = ch;
		if (rawfb_col >= RAWFB_COLS) {
			rawfb_draw_line_buf(rawfb_row, rawfb_line, rawfb_col);
			rawfb_col = 0;
			rawfb_row = (rawfb_row + 1) % RAWFB_ROWS;
			rawfb_clear_row(rawfb_row);
		}
	}
	if (rawfb_col > 0)
		rawfb_draw_line_buf(rawfb_row, rawfb_line, rawfb_col);
}

static int __init rawfb_earlycon_setup(struct earlycon_device *device, const char *opt)
{
	pmsg_pos = 0;
	rawfb_col = 0;
	rawfb_row = 0;
	rawfb_clear_row(0);
	rawfb_clear_row(1);
	device->con->write = rawfb_write;
	rawfb_con = device->con;
	return 0;
}
EARLYCON_DECLARE(rawfb, rawfb_earlycon_setup);

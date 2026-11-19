#ifndef JTGREETER_UI_H
#define JTGREETER_UI_H

#define _GNU_SOURCE
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <security/pam_appl.h>

#include <math.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FONT_FAMILY "Liberation"
#ifndef PI
#define PI 3.14159265358979323846
#endif

#define R_(h) ((((h) >> 16) & 0xFF) / 255.0)
#define G_(h) ((((h) >> 8) & 0xFF) / 255.0)
#define B_(h) (((h) & 0xFF) / 255.0)

#define C_BG        0x030303
#define C_BG_TOP    0x070708
#define C_FG        0xFFFFFF
#define C_DIM       0x8C8C8C
#define C_FAINT     0x5C5C5C
#define C_BORDER    0x404040
#define C_GREEN     0x66B366
#define C_RED       0xD05C5C

extern unsigned accent_rgb;
extern unsigned accent_hi_rgb;
extern unsigned accent_lo_rgb;
extern unsigned on_accent_rgb;
#define C_ACCENT    accent_rgb
#define C_ACCENT_HI accent_hi_rgb
#define C_ACCENT_LO accent_lo_rgb
#define C_ON_ACCENT on_accent_rgb

enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };
enum { FIELD_USER = 0, FIELD_PASS = 1 };

struct ui_state {
	char username[128];
	char password[128];
	char message[256];
	unsigned msg_color;
	bool msg_set;
	bool authenticating;
	bool success;
	int focus;
	double t0;
};

static inline double ui_mono_sec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static inline void ui_rgba(cairo_t *cr, unsigned hex, double a) {
	cairo_set_source_rgba(cr, R_(hex), G_(hex), B_(hex), a);
}

static inline void ui_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
	double d = PI / 180.0;
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -90 * d, 0 * d);
	cairo_arc(cr, x + w - r, y + h - r, r, 0 * d, 90 * d);
	cairo_arc(cr, x + r, y + h - r, r, 90 * d, 180 * d);
	cairo_arc(cr, x + r, y + r, r, 180 * d, 270 * d);
	cairo_close_path(cr);
}

static inline void ui_text(cairo_t *cr, const char *text, double size, int weight,
		unsigned hex, double alpha, double x, double y, int align) {
	if (!text || !*text) return;
	PangoLayout *l = pango_cairo_create_layout(cr);
	PangoFontDescription *fd = pango_font_description_new();
	pango_font_description_set_family(fd, FONT_FAMILY);
	pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
	pango_font_description_set_weight(fd, (PangoWeight)weight);
	pango_layout_set_font_description(l, fd);
	pango_layout_set_text(l, text, -1);
	PangoRectangle logical;
	pango_layout_get_pixel_extents(l, NULL, &logical);
	double tx = x;
	if (align == ALIGN_CENTER) tx -= logical.width / 2.0;
	else if (align == ALIGN_RIGHT) tx -= logical.width;
	cairo_move_to(cr, tx, y);
	ui_rgba(cr, hex, alpha);
	pango_cairo_show_layout(cr, l);
	g_object_unref(l);
	pango_font_description_free(fd);
}

static inline int accent_hex_nibble(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return 10 + c - 'a';
	if (c >= 'A' && c <= 'F') return 10 + c - 'A';
	return -1;
}
static inline int accent_parse(const char *s, unsigned *out) {
	if (!s) return 0;
	if (s[0] == '#') s++;
	if (strlen(s) != 6) return 0;
	unsigned v = 0;
	for (int i = 0; i < 6; i++) {
		int n = accent_hex_nibble(s[i]);
		if (n < 0) return 0;
		v = (v << 4) | (unsigned)n;
	}
	*out = v; return 1;
}
static inline double accent_linear_channel(unsigned channel) {
	double c = channel / 255.0;
	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
static inline double accent_luminance(unsigned rgb) {
	return 0.2126 * accent_linear_channel((rgb >> 16) & 0xFF) +
	       0.7152 * accent_linear_channel((rgb >> 8) & 0xFF) +
	       0.0722 * accent_linear_channel(rgb & 0xFF);
}
static inline double accent_contrast(double a, double b) {
	double hi = a > b ? a : b, lo = a > b ? b : a;
	return (hi + 0.05) / (lo + 0.05);
}
static inline unsigned accent_ink(unsigned rgb) {
	unsigned dark = 0x14100A, light = 0xFFF7EC;
	double bg = accent_luminance(rgb);
	return accent_contrast(bg, accent_luminance(dark)) >= accent_contrast(bg, accent_luminance(light)) ? dark : light;
}
static inline unsigned accent_mix(unsigned rgb, unsigned toward, double amount) {
	if (amount < 0.0) amount = 0.0; if (amount > 1.0) amount = 1.0;
	unsigned out = 0;
	for (int shift = 16; shift >= 0; shift -= 8) {
		unsigned a = (rgb >> shift) & 0xFF, b = (toward >> shift) & 0xFF;
		unsigned c = (unsigned)lround(a + (b - a) * amount);
		out = (out << 8) | (c & 0xFF);
	}
	return out;
}
static inline void accent_set(unsigned rgb) {
	accent_rgb = rgb;
	accent_hi_rgb = accent_mix(rgb, 0xFFFFFF, 0.16);
	accent_lo_rgb = accent_mix(rgb, 0x000000, 0.14);
	on_accent_rgb = accent_ink(rgb);
}
#define ACCENT_CONF_PATH "/etc/jt/accent.conf"

static inline void accent_load_default(void) {
	FILE *f = fopen(ACCENT_CONF_PATH, "r");
	if (!f) return;
	char buf[64];
	if (fgets(buf, sizeof(buf), f)) {
		char *s = buf;
		while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
		char *e = s + strlen(s);
		while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = '\0';
		unsigned rgb;
		if (accent_parse(s, &rgb)) accent_set(rgb);
	}
	fclose(f);
}

static inline void accent_cli(int *argc, char **argv) {
	accent_load_default();
	for (int i = 1; i < *argc; i++) {
		const char *val = NULL; int consume = 0;
		if (!strcmp(argv[i], "--accent") && i + 1 < *argc) { val = argv[i+1]; consume = 2; }
		else if (!strncmp(argv[i], "--accent=", 9)) { val = argv[i]+9; consume = 1; }
		else if (argv[i][0] == '#') { val = argv[i]; consume = 1; }
		if (!val) continue;
		unsigned rgb;
		if (accent_parse(val, &rgb)) {
			accent_set(rgb);
			for (int j = i; j + consume < *argc; j++) argv[j] = argv[j+consume];
			*argc -= consume; i--;
		}
	}
}

static inline void ui_init(struct ui_state *ui, const char *preset_user) {
	memset(ui, 0, sizeof(*ui));
	ui->focus = (preset_user && preset_user[0]) ? FIELD_PASS : FIELD_USER;
	if (preset_user) snprintf(ui->username, sizeof(ui->username), "%s", preset_user);
	ui->t0 = ui_mono_sec();
}

static inline void paint_dust(cairo_t *cr, double t, double w, double h, bool active) {
	const double DW = 540.0, DH = 640.0;
	double sx = w / DW, sy = h / DH;
	const double x_margin = 34.0, x_span = DW - 2.0 * x_margin;
	const double y_span = 328.0, y_bottom = 332.0;
	for (int i = 0; i < 26; i++) {
		double seed = (double)(i + 1) * 12.9898;
		double base_x = (x_margin + fmod(seed * 37.719, x_span)) * sx;
		double speed = 5.0 + fmod(seed * 11.13, 13.0);
		double ph = fmod((seed * 19.19 + t * speed * (active ? 1.55 : 1.0)) / y_span, 1.0);
		if (ph < 0.0) ph += 1.0;
		double y = (y_bottom - ph * y_span) * sy;
		double x = base_x + sin(t * 0.33 + i * 1.7) * (5.0 + fmod(seed * 3.3, 7.0)) * sx;
		double a = (0.035 + 0.075 * (0.5 + 0.5 * sin(t * 0.5 + i * 2.1))) * (active ? 1.55 : 1.0);
		double rad = 0.7 + fmod(seed * 5.7, 1.25);
		unsigned col = C_FG;
		if (i % 7 == 2) col = C_ACCENT;
		else if (i % 11 == 5) col = C_GREEN;
		ui_rgba(cr, col, a);
		cairo_arc(cr, x, y, rad, 0.0, 2.0 * PI);
		cairo_fill(cr);
	}
}

static inline void paint_ambient(cairo_t *cr, double t, double cx, double cy, bool active) {
	double breath = 0.5 + 0.5 * sin(t * 0.55);
	double glow = (active ? 0.080 : 0.042) * (0.72 + 0.28 * breath);
	cairo_pattern_t *p = cairo_pattern_create_radial(cx, cy, 18.0, cx, cy, 232.0);
	cairo_pattern_add_color_stop_rgba(p, 0.0, R_(C_ACCENT), G_(C_ACCENT), B_(C_ACCENT), glow);
	cairo_pattern_add_color_stop_rgba(p, 0.55, R_(C_ACCENT), G_(C_ACCENT), B_(C_ACCENT), glow * 0.28);
	cairo_pattern_add_color_stop_rgba(p, 1.0, R_(C_ACCENT), G_(C_ACCENT), B_(C_ACCENT), 0.0);
	cairo_set_source(cr, p);
	cairo_paint(cr);
	cairo_pattern_destroy(p);
}

static inline void ui_draw(cairo_t *cr, struct ui_state *ui, int w, int h) {
	double t = ui_mono_sec() - ui->t0;
	ui_rgba(cr, C_BG, 1.0);
	cairo_paint(cr);

	double cx = w / 2.0, cy = h / 2.0 - 40.0;
	paint_ambient(cr, t, cx, cy, ui->authenticating);
	paint_dust(cr, t, w, h, ui->authenticating);

	double card_w = 420.0, card_h = 320.0;
	double card_x = (w - card_w) / 2.0, card_y = (h - card_h) / 2.0 - 10.0;
	ui_rgba(cr, 0x0F1011, 0.92);
	ui_rounded_rect(cr, card_x, card_y, card_w, card_h, 14.0);
	cairo_fill(cr);
	ui_rgba(cr, C_BORDER, 0.45);
	cairo_set_line_width(cr, 1.0);
	ui_rounded_rect(cr, card_x + 0.5, card_y + 0.5, card_w - 1.0, card_h - 1.0, 13.5);
	cairo_stroke(cr);

	ui_text(cr, "JTLab Greeter", 22.0, 700, C_FG, 0.96, cx, card_y + 34.0, ALIGN_CENTER);
	ui_text(cr, "Sign in to start jtlabwc", 11.5, 400, C_DIM, 0.9, cx, card_y + 64.0, ALIGN_CENTER);

	double fx = card_x + 36.0, fw = card_w - 72.0, fh = 44.0;
	double uy = card_y + 104.0, py = card_y + 164.0;
	for (int i = 0; i < 2; i++) {
		double y = i == 0 ? uy : py;
		bool focused = ui->focus == i;
		ui_rgba(cr, 0x0A0A0C, 0.95);
		ui_rounded_rect(cr, fx, y, fw, fh, 8.0);
		cairo_fill(cr);
		ui_rgba(cr, focused ? C_ACCENT : C_BORDER, focused ? 0.85 : 0.45);
		cairo_set_line_width(cr, focused ? 1.4 : 1.0);
		ui_rounded_rect(cr, fx + 0.5, y + 0.5, fw - 1.0, fh - 1.0, 7.5);
		cairo_stroke(cr);
		const char *label = i == 0 ? "Username" : "Password";
		const char *val = i == 0 ? ui->username : ui->password;
		char display[160] = {0};
		if (i == 1) {
			size_t n = strlen(val); if (n > 40) n = 40;
			for (size_t k = 0; k < n; k++) display[k] = '*';
			display[n] = '\0';
		} else {
			snprintf(display, sizeof(display), "%s", val);
		}
		if (display[0]) ui_text(cr, display, 13.0, 400, C_FG, 0.95, fx + 14.0, y + 13.0, ALIGN_LEFT);
		else ui_text(cr, label, 12.0, 400, C_FAINT, 0.8, fx + 14.0, y + 14.0, ALIGN_LEFT);
		if (focused) {
			double caret_x = fx + 14.0 + (double)strlen(display) * (i==1?8.0:7.5);
			double blink = 0.35 + 0.65 * (0.5 + 0.5 * sin(t * 5.0));
			ui_rgba(cr, C_ACCENT, blink);
			cairo_rectangle(cr, caret_x, y + 11.0, 1.5, 22.0);
			cairo_fill(cr);
		}
	}

	double bw = 160.0, bh = 42.0, bx = cx - bw / 2.0, by = card_y + 230.0;
	ui_rgba(cr, ui->authenticating ? C_ACCENT_LO : C_ACCENT, 1.0);
	ui_rounded_rect(cr, bx, by, bw, bh, 21.0);
	cairo_fill(cr);
	ui_text(cr, ui->authenticating ? "Signing in..." : "Sign in", 13.5, 600, C_ON_ACCENT, 1.0, cx, by + 12.0, ALIGN_CENTER);
	if (ui->msg_set) ui_text(cr, ui->message, 11.0, 500, ui->msg_color, 0.95, cx, card_y + 286.0, ALIGN_CENTER);
	ui_text(cr, "Enter: sign in   Tab: next field   Esc: clear", 9.5, 400, C_FAINT, 0.7, cx, h - 28.0, ALIGN_CENTER);
}

struct pam_conv_ctx { const char *password; };
static inline int pam_conv_cb(int num_msg, const struct pam_message **msg,
		struct pam_response **resp, void *appdata_ptr) {
	struct pam_conv_ctx *ctx = appdata_ptr;
	if (num_msg <= 0) return PAM_CONV_ERR;
	struct pam_response *replies = calloc(num_msg, sizeof(struct pam_response));
	if (!replies) return PAM_CONV_ERR;
	for (int i = 0; i < num_msg; i++) {
		if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON)
			replies[i].resp = strdup(ctx->password ? ctx->password : "");
	}
	*resp = replies; return PAM_SUCCESS;
}

/* Authenticate only (used by --lock to verify password). Does not open a login session. */
static inline bool ui_pam_auth(const char *user, const char *pass, char *err, size_t errlen) {
	struct pam_conv_ctx ctx = { .password = pass };
	struct pam_conv conv = { .conv = pam_conv_cb, .appdata_ptr = &ctx };
	pam_handle_t *pamh = NULL;
	int r = pam_start("jtgreeter", user, &conv, &pamh);
	if (r != PAM_SUCCESS) {
		if (err && errlen) snprintf(err, errlen, "PAM start failed");
		return false;
	}
	r = pam_authenticate(pamh, 0);
	if (r == PAM_SUCCESS) r = pam_acct_mgmt(pamh, 0);
	if (r == PAM_SUCCESS) {
		pam_setcred(pamh, PAM_ESTABLISH_CRED);
		pam_setcred(pamh, PAM_DELETE_CRED);
		pam_end(pamh, r);
		return true;
	}
	if (err && errlen) snprintf(err, errlen, "%s", pam_strerror(pamh, r));
	pam_end(pamh, r);
	return false;
}

/* Greeter login step 1: verify credentials while greeter UI/compositor is running.
 * On success returns an open pam handle; call ui_pam_session_open() later (after
 * greeter releases seat/DRM) then ui_pam_login_end() after session exits. */
static inline bool ui_pam_authenticate_user(const char *user, const char *pass,
		pam_handle_t **out_pamh, char *err, size_t errlen) {
	*out_pamh = NULL;
	struct pam_conv_ctx ctx = { .password = pass };
	struct pam_conv conv = { .conv = pam_conv_cb, .appdata_ptr = &ctx };
	pam_handle_t *pamh = NULL;
	int r = pam_start("jtgreeter", user, &conv, &pamh);
	if (r != PAM_SUCCESS) {
		if (err && errlen) snprintf(err, errlen, "PAM start failed");
		return false;
	}
	r = pam_authenticate(pamh, 0);
	if (r != PAM_SUCCESS) goto fail;
	r = pam_acct_mgmt(pamh, 0);
	if (r == PAM_NEW_AUTHTOK_REQD) r = pam_chauthtok(pamh, PAM_CHANGE_EXPIRED_AUTHTOK);
	if (r != PAM_SUCCESS) goto fail;
	*out_pamh = pamh;
	return true;
fail:
	if (err && errlen) snprintf(err, errlen, "%s", pam_strerror(pamh, r));
	pam_end(pamh, r);
	return false;
}

/* Greeter login step 2: establish creds and open PAM session (run after greeter
 * compositor/seat is released, before launching the user session). */
static inline bool ui_pam_open_session(pam_handle_t *pamh, char *err, size_t errlen) {
	if (!pamh) return false;
	int r = pam_setcred(pamh, PAM_ESTABLISH_CRED);
	if (r != PAM_SUCCESS) goto fail;
	r = pam_open_session(pamh, 0);
	if (r != PAM_SUCCESS) {
		pam_setcred(pamh, PAM_DELETE_CRED);
		goto fail;
	}
	return true;
fail:
	if (err && errlen) snprintf(err, errlen, "%s", pam_strerror(pamh, r));
	pam_end(pamh, r);
	return false;
}

static inline void ui_pam_login_end(pam_handle_t *pamh) {
	if (!pamh) return;
	pam_close_session(pamh, 0);
	pam_setcred(pamh, PAM_DELETE_CRED);
	pam_end(pamh, PAM_SUCCESS);
}

#endif

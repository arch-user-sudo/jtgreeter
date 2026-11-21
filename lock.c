#define _GNU_SOURCE
#include "ui.h"

#include <wayland-client.h>
#include "build/proto/ext-session-lock-v1-client-protocol.h"

#include <xkbcommon/xkbcommon.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#define LOCK_NBUFS 3

int run_lock_mode(int argc, char **argv);

struct lock_client;

struct lock_seat {
	struct wl_list link;
	struct lock_client *client;
	uint32_t name;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
};

struct lock_buf {
	struct wl_buffer *buffer;
	void *data;
	size_t size;
	int width, height;
	bool busy;
};

struct lock_surface {
	struct wl_list link;
	struct lock_client *client;
	uint32_t output_name;
	struct wl_output *output;
	struct wl_surface *surface;
	struct ext_session_lock_surface_v1 *lock_surface;
	struct wl_callback *frame_cb;
	int width, height;
	bool configured;
	bool needs_draw;
	struct lock_buf bufs[LOCK_NBUFS];
};

struct lock_client {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	uint32_t compositor_name;
	uint32_t shm_name;
	uint32_t lock_mgr_name;
	struct ext_session_lock_manager_v1 *lock_mgr;
	struct ext_session_lock_v1 *lock;
	struct wl_list seats; /* lock_seat.link */
	struct xkb_context *xkb_ctx;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;

	struct ui_state ui;
	bool running;
	bool locked;
	bool unlock_requested;
	char user[128];
	struct wl_list surfaces; /* lock_surface.link */
};

static int create_shm_file(size_t size) {
	int fd = memfd_create("jtgreeter-lock", MFD_CLOEXEC);
	if (fd < 0) return -1;
	if (ftruncate(fd, (off_t)size) < 0) { close(fd); return -1; }
	return fd;
}

static void lock_buf_destroy(struct lock_buf *b) {
	if (b->buffer) { wl_buffer_destroy(b->buffer); b->buffer = NULL; }
	if (b->data && b->data != MAP_FAILED) { munmap(b->data, b->size); b->data = NULL; }
	b->size = 0;
	b->width = b->height = 0;
	b->busy = false;
}

static void buffer_release(void *data, struct wl_buffer *buffer) {
	(void)buffer;
	struct lock_buf *b = data;
	b->busy = false;
}
static const struct wl_buffer_listener buffer_listener = { .release = buffer_release };

static int lock_buf_ensure(struct lock_surface *ls, struct lock_buf *b, int w, int h) {
	int stride = w * 4;
	size_t size = (size_t)stride * (size_t)h;
	if (b->buffer && b->width == w && b->height == h && b->size == size)
		return 0;
	lock_buf_destroy(b);
	int fd = create_shm_file(size);
	if (fd < 0) return -1;
	b->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (b->data == MAP_FAILED) { b->data = NULL; close(fd); return -1; }
	struct wl_shm_pool *pool = wl_shm_create_pool(ls->client->shm, fd, (int)size);
	b->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	if (!b->buffer) { munmap(b->data, size); b->data = NULL; return -1; }
	b->size = size;
	b->width = w;
	b->height = h;
	b->busy = false;
	wl_buffer_add_listener(b->buffer, &buffer_listener, b);
	return 0;
}

static struct lock_buf *lock_buf_acquire(struct lock_surface *ls) {
	for (int i = 0; i < LOCK_NBUFS; i++) {
		if (!ls->bufs[i].busy) {
			if (lock_buf_ensure(ls, &ls->bufs[i], ls->width, ls->height) == 0)
				return &ls->bufs[i];
		}
	}
	return NULL;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener frame_listener = { .done = frame_done };

static void lock_surface_draw(struct lock_surface *ls) {
	if (!ls->configured || ls->width <= 0 || ls->height <= 0) return;
	/* if a frame callback is pending, defer until compositor asks for next frame */
	if (ls->frame_cb) { ls->needs_draw = true; return; }

	struct lock_buf *b = lock_buf_acquire(ls);
	if (!b) {
		/* all buffers busy; try again after next dispatch/release */
		ls->needs_draw = true;
		return;
	}

	int stride = ls->width * 4;
	cairo_surface_t *cs = cairo_image_surface_create_for_data(b->data, CAIRO_FORMAT_ARGB32,
		ls->width, ls->height, stride);
	cairo_t *cr = cairo_create(cs);
	ui_draw(cr, &ls->client->ui, ls->width, ls->height);
	cairo_destroy(cr);
	cairo_surface_destroy(cs);

	wl_surface_attach(ls->surface, b->buffer, 0, 0);
	wl_surface_damage_buffer(ls->surface, 0, 0, ls->width, ls->height);

	ls->frame_cb = wl_surface_frame(ls->surface);
	wl_callback_add_listener(ls->frame_cb, &frame_listener, ls);

	wl_surface_commit(ls->surface);
	b->busy = true;
	ls->needs_draw = false;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
	(void)time;
	struct lock_surface *ls = data;
	if (ls->frame_cb == cb) ls->frame_cb = NULL;
	wl_callback_destroy(cb);
	/* continuous animation: draw next frame */
	lock_surface_draw(ls);
}

static void lock_surface_configure(void *data, struct ext_session_lock_surface_v1 *lock_surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	struct lock_surface *ls = data;
	ls->width = (int)width; ls->height = (int)height; ls->configured = true;
	ext_session_lock_surface_v1_ack_configure(lock_surface, serial);
	/* Buffers are recreated on demand when the size changes. */
	ls->needs_draw = true;
	lock_surface_draw(ls);
}
static const struct ext_session_lock_surface_v1_listener lock_surface_listener = {
	.configure = lock_surface_configure,
};

static void lock_surface_destroy(struct lock_surface *ls) {
	if (ls->frame_cb) wl_callback_destroy(ls->frame_cb);
	if (ls->lock_surface) ext_session_lock_surface_v1_destroy(ls->lock_surface);
	if (ls->surface) wl_surface_destroy(ls->surface);
	if (ls->output) wl_output_destroy(ls->output);
	for (int i = 0; i < LOCK_NBUFS; i++) lock_buf_destroy(&ls->bufs[i]);
}

static void lock_surface_create(struct lock_client *c, struct lock_surface *ls) {
	if (!c->compositor || !c->lock || ls->surface) return;
	ls->surface = wl_compositor_create_surface(c->compositor);
	if (!ls->surface) return;
	ls->lock_surface = ext_session_lock_v1_get_lock_surface(c->lock,
		ls->surface, ls->output);
	if (!ls->lock_surface) {
		wl_surface_destroy(ls->surface);
		ls->surface = NULL;
		return;
	}
	ext_session_lock_surface_v1_add_listener(ls->lock_surface,
		&lock_surface_listener, ls);
}

static void create_lock_surfaces(struct lock_client *c) {
	struct lock_surface *ls;
	wl_list_for_each(ls, &c->surfaces, link)
		lock_surface_create(c, ls);
}

static void lock_seat_destroy(struct lock_seat *seat) {
	if (seat->keyboard) wl_keyboard_destroy(seat->keyboard);
	if (seat->seat) wl_seat_destroy(seat->seat);
}

static void lock_handle_locked(void *data, struct ext_session_lock_v1 *lock) {
	(void)lock;
	struct lock_client *c = data;
	c->locked = true;
}
static void lock_handle_finished(void *data, struct ext_session_lock_v1 *lock) {
	(void)lock;
	struct lock_client *c = data;
	if (!c->locked)
		fprintf(stderr, "jtgreeter: session lock could not be acquired\n");
	c->running = false;
}
static const struct ext_session_lock_v1_listener lock_listener = {
	.locked = lock_handle_locked,
	.finished = lock_handle_finished,
};

static void redraw_all(struct lock_client *c) {
	struct lock_surface *ls;
	wl_list_for_each(ls, &c->surfaces, link) lock_surface_draw(ls);
}

static void schedule_redraw_all(struct lock_client *c) {
	struct lock_surface *ls;
	wl_list_for_each(ls, &c->surfaces, link) {
		ls->needs_draw = true;
		if (!ls->frame_cb) lock_surface_draw(ls);
	}
}

static void try_unlock(struct lock_client *c) {
	if (c->ui.authenticating || c->ui.success) return;
	if (!c->ui.password[0]) {
		snprintf(c->ui.message, sizeof(c->ui.message), "Enter password");
		c->ui.msg_color = C_RED; c->ui.msg_set = true;
		schedule_redraw_all(c);
		return;
	}
	c->ui.authenticating = true;
	snprintf(c->ui.message, sizeof(c->ui.message), "Authenticating...");
	c->ui.msg_color = C_DIM; c->ui.msg_set = true;
	schedule_redraw_all(c); wl_display_flush(c->display);

	char err[256] = {0};
	bool ok = ui_pam_auth(c->user, c->ui.password, err, sizeof(err));
	c->ui.authenticating = false;
	memset(c->ui.password, 0, sizeof(c->ui.password));
	if (ok) {
		c->ui.success = true;
		snprintf(c->ui.message, sizeof(c->ui.message), "Unlocked");
		c->ui.msg_color = C_GREEN; c->ui.msg_set = true;
		schedule_redraw_all(c);
		wl_display_flush(c->display);
		/* Unlocking must happen outside wl_display_dispatch(): this runs
		 * from a keyboard callback and nested roundtrips can deadlock. */
		c->unlock_requested = true;
	} else {
		snprintf(c->ui.message, sizeof(c->ui.message), "%s", err[0] ? err : "Authentication failed");
		c->ui.msg_color = C_RED; c->ui.msg_set = true;
		schedule_redraw_all(c);
	}
}

static void handle_key(struct lock_client *c, xkb_keysym_t sym) {
	if (c->ui.success) return;
	if (sym == XKB_KEY_Escape) {
		memset(c->ui.password, 0, sizeof(c->ui.password));
		c->ui.msg_set = false;
	} else if (sym == XKB_KEY_Tab) {
		c->ui.focus = FIELD_PASS; /* user fixed in lock mode */
	} else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		try_unlock(c);
	} else if (sym == XKB_KEY_BackSpace) {
		size_t n = strlen(c->ui.password);
		/* Characters are inserted as UTF-8; don't leave a dangling lead byte. */
		while (n > 0 && ((unsigned char)c->ui.password[n-1] & 0xC0) == 0x80) n--;
		if (n > 0) n--;
		c->ui.password[n] = '\0';
	} else {
		char ch[8];
		int n = xkb_keysym_to_utf8(sym, ch, sizeof(ch));
		if (n > 1) {
			size_t cur = strlen(c->ui.password);
			if (cur + (size_t)(n-1) < sizeof(c->ui.password)-1) {
				memcpy(c->ui.password + cur, ch, (size_t)(n-1));
				c->ui.password[cur + (size_t)(n-1)] = '\0';
			}
		}
	}
}

static void kbd_keymap(void *data, struct wl_keyboard *kbd, uint32_t fmt, int32_t fd, uint32_t size) {
	(void)kbd; struct lock_client *c = data;
	if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED) { close(fd); return; }
	struct xkb_keymap *km = xkb_keymap_new_from_string(c->xkb_ctx, map,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size); close(fd);
	if (!km) return;
	struct xkb_state *st = xkb_state_new(km);
	if (c->xkb_keymap) xkb_keymap_unref(c->xkb_keymap);
	if (c->xkb_state) xkb_state_unref(c->xkb_state);
	c->xkb_keymap = km; c->xkb_state = st;
}
static void kbd_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *surf, struct wl_array *keys) {
	(void)d;(void)k;(void)s;(void)surf;(void)keys;
}
static void kbd_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *surf) { (void)d;(void)k;(void)s;(void)surf; }
static void kbd_key(void *data, struct wl_keyboard *kbd, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	(void)kbd;(void)serial;(void)time; struct lock_client *c = data;
	if (!c->xkb_state || state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(c->xkb_state, key + 8);
	handle_key(c, sym);
	schedule_redraw_all(c);
}
static void kbd_modifiers(void *data, struct wl_keyboard *kbd, uint32_t s,
		uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp) {
	(void)kbd;(void)s; struct lock_client *c = data;
	if (c->xkb_state) xkb_state_update_mask(c->xkb_state, dep, lat, lock, 0, 0, grp);
}
static void kbd_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t del) { (void)d;(void)k;(void)r;(void)del; }
static const struct wl_keyboard_listener kbd_listener = {
	.keymap=kbd_keymap,.enter=kbd_enter,.leave=kbd_leave,.key=kbd_key,.modifiers=kbd_modifiers,.repeat_info=kbd_repeat
};

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps) {
	struct lock_seat *ls = data;
	struct lock_client *c = ls->client;
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !ls->keyboard) {
		ls->keyboard = wl_seat_get_keyboard(seat);
		if (ls->keyboard)
			wl_keyboard_add_listener(ls->keyboard, &kbd_listener, c);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && ls->keyboard) {
		wl_keyboard_destroy(ls->keyboard);
		ls->keyboard = NULL;
	}
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d;(void)s;(void)n; }
static const struct wl_seat_listener seat_listener = { .capabilities=seat_caps, .name=seat_name };

static void registry_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver) {
	struct lock_client *c = data;
	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		if (c->compositor) return;
		c->compositor = wl_registry_bind(reg, name, &wl_compositor_interface,
			ver < 4 ? ver : 4);
		c->compositor_name = name;
	} else if (strcmp(iface, wl_shm_interface.name) == 0) {
		if (c->shm) return;
		c->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
		c->shm_name = name;
	} else if (strcmp(iface, wl_seat_interface.name) == 0) {
		struct lock_seat *seat = calloc(1, sizeof(*seat));
		if (!seat) return;
		seat->client = c;
		seat->name = name;
		seat->seat = wl_registry_bind(reg, name, &wl_seat_interface, ver < 5 ? ver : 5);
		if (!seat->seat) { free(seat); return; }
		wl_seat_add_listener(seat->seat, &seat_listener, seat);
		wl_list_insert(&c->seats, &seat->link);
	} else if (strcmp(iface, ext_session_lock_manager_v1_interface.name) == 0) {
		if (c->lock_mgr) return;
		c->lock_mgr = wl_registry_bind(reg, name,
			&ext_session_lock_manager_v1_interface, 1);
		c->lock_mgr_name = name;
	} else if (strcmp(iface, wl_output_interface.name) == 0) {
		struct wl_output *out = wl_registry_bind(reg, name, &wl_output_interface, ver < 2 ? ver : 2);
		struct lock_surface *ls = calloc(1, sizeof(*ls));
		if (!ls) { wl_output_destroy(out); return; }
		ls->client = c; ls->output = out; ls->output_name = name;
		wl_list_insert(&c->surfaces, &ls->link);
		lock_surface_create(c, ls);
	}
}

static void registry_remove(void *data, struct wl_registry *reg, uint32_t name) {
	(void)reg;
	struct lock_client *c = data;
	struct lock_surface *ls, *ls_tmp;
	wl_list_for_each_safe(ls, ls_tmp, &c->surfaces, link) {
		if (ls->output_name != name) continue;
		wl_list_remove(&ls->link);
		lock_surface_destroy(ls);
		free(ls);
		return;
	}
	struct lock_seat *seat, *seat_tmp;
	wl_list_for_each_safe(seat, seat_tmp, &c->seats, link) {
		if (seat->name != name) continue;
		wl_list_remove(&seat->link);
		lock_seat_destroy(seat);
		free(seat);
		return;
	}
	if (c->compositor_name == name || c->shm_name == name ||
	    c->lock_mgr_name == name)
		c->running = false;
}
static const struct wl_registry_listener registry_listener = { .global=registry_global, .global_remove=registry_remove };

static void lock_client_cleanup(struct lock_client *c) {
	struct lock_surface *s, *tmp;
	wl_list_for_each_safe(s, tmp, &c->surfaces, link) {
		wl_list_remove(&s->link);
		lock_surface_destroy(s);
		free(s);
	}
	if (c->lock) {
		/* The protocol requires unlock_and_destroy once the lock is active. */
		if (c->locked) ext_session_lock_v1_unlock_and_destroy(c->lock);
		else ext_session_lock_v1_destroy(c->lock);
	}
	struct lock_seat *seat, *seat_tmp;
	wl_list_for_each_safe(seat, seat_tmp, &c->seats, link) {
		wl_list_remove(&seat->link);
		lock_seat_destroy(seat);
		free(seat);
	}
	if (c->xkb_state) xkb_state_unref(c->xkb_state);
	if (c->xkb_keymap) xkb_keymap_unref(c->xkb_keymap);
	if (c->xkb_ctx) xkb_context_unref(c->xkb_ctx);
	if (c->lock_mgr) ext_session_lock_manager_v1_destroy(c->lock_mgr);
	if (c->shm) wl_shm_destroy(c->shm);
	if (c->compositor) wl_compositor_destroy(c->compositor);
	if (c->registry) wl_registry_destroy(c->registry);
	if (c->display) wl_display_disconnect(c->display);
	c->display = NULL;
}

int run_lock_mode(int argc, char **argv) {
	(void)argc; (void)argv;
	struct lock_client c = {0};
	wl_list_init(&c.surfaces);
	wl_list_init(&c.seats);
	c.running = true;
	c.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct passwd *pw = getpwuid(getuid());
	snprintf(c.user, sizeof(c.user), "%s", pw ? pw->pw_name : "");
	ui_init(&c.ui, c.user);
	c.ui.focus = FIELD_PASS;

	c.display = wl_display_connect(NULL);
	if (!c.display) {
		fprintf(stderr, "jtgreeter --lock: cannot connect to Wayland display\n");
		lock_client_cleanup(&c);
		return 1;
	}
	c.registry = wl_display_get_registry(c.display);
	wl_registry_add_listener(c.registry, &registry_listener, &c);
	wl_display_roundtrip(c.display);

	if (!c.lock_mgr || !c.compositor || !c.shm) {
		fprintf(stderr, "jtgreeter --lock: compositor lacks ext-session-lock-v1, shm, or compositor globals\n");
		lock_client_cleanup(&c);
		return 1;
	}

	c.lock = ext_session_lock_manager_v1_lock(c.lock_mgr);
	ext_session_lock_v1_add_listener(c.lock, &lock_listener, &c);

	create_lock_surfaces(&c);
	wl_display_flush(c.display);

	while (c.running && c.display) {
		if (c.unlock_requested) {
			c.unlock_requested = false;
			wl_display_roundtrip(c.display);
			if (c.lock) {
				ext_session_lock_v1_unlock_and_destroy(c.lock);
				c.lock = NULL;
				c.locked = false;
			}
			wl_display_roundtrip(c.display);
			break;
		}

		/* if we need to draw and no frame callback pending, try now (e.g. after buffer release) */
		struct lock_surface *it;
		wl_list_for_each(it, &c.surfaces, link) {
			if (it->needs_draw && !it->frame_cb) lock_surface_draw(it);
		}

		if (wl_display_flush(c.display) < 0 && errno != EAGAIN) break;

		int timeout = -1;
		wl_list_for_each(it, &c.surfaces, link) {
			if (it->configured && !it->frame_cb) { timeout = 0; break; }
			if (it->needs_draw) { timeout = 16; break; }
		}
		/* animation: keep rendering; but if frame callbacks are in flight compositor will pace us.
		 * if no surfaces configured yet, wait for events */
		bool any_configured = false;
		wl_list_for_each(it, &c.surfaces, link) if (it->configured) any_configured = true;
		if (any_configured && timeout < 0) timeout = 1000; /* fallback */

		struct pollfd pfd = { .fd = wl_display_get_fd(c.display), .events = POLLIN };
		if (wl_display_flush(c.display) < 0 && errno == EAGAIN) pfd.events |= POLLOUT;
		int ret = poll(&pfd, 1, timeout);
		if (ret < 0) { if (errno == EINTR) continue; break; }
		if (pfd.revents & (POLLERR | POLLHUP)) break;
		if (pfd.revents & POLLOUT) wl_display_flush(c.display);
		if (pfd.revents & POLLIN) {
			if (wl_display_dispatch(c.display) < 0) break;
		} else if (ret == 0) {
			/* timeout: nudge animation if needed */
			redraw_all(&c);
		}
	}

	lock_client_cleanup(&c);
	return 0;
}

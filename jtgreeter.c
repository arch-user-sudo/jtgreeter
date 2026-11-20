#define _GNU_SOURCE

#ifndef WLR_USE_UNSTABLE
#define WLR_USE_UNSTABLE 1
#endif

#include "wlroots.c"

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/log.h>

#include <drm_fourcc.h>
#include <xkbcommon/xkbcommon.h>

#include "ui.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <paths.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int run_lock_mode(int argc, char **argv);

struct server;
struct g_output {
	struct wl_list link;
	struct server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wl_listener destroy;
	struct wl_listener request_state;
};

struct g_keyboard {
	struct wl_list link;
	struct server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

struct server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_session *session;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;

	struct wlr_seat *seat;
	struct wl_list keyboards;
	struct wl_listener new_input;
	struct wl_listener request_set_selection;

	struct xkb_context *xkb_ctx;

	struct ui_state ui;
	bool running;
	bool launch_jtlabwc;
	pam_handle_t *pamh;
};

static void launch_session(struct server *srv) {
	srv->launch_jtlabwc = true;
	srv->running = false;
	if (srv->wl_display) {
		wl_display_terminate(srv->wl_display);
	}
}

static void greeter_try_login(struct server *srv) {
	if (srv->ui.authenticating || srv->ui.success) return;
	if (!srv->ui.username[0] || !srv->ui.password[0]) {
		snprintf(srv->ui.message, sizeof(srv->ui.message), "Enter username and password");
		srv->ui.msg_color = C_RED; srv->ui.msg_set = true;
		return;
	}
	srv->ui.authenticating = true;
	snprintf(srv->ui.message, sizeof(srv->ui.message), "Authenticating...");
	srv->ui.msg_color = C_DIM; srv->ui.msg_set = true;

	char err[256] = {0};
	pam_handle_t *pamh = NULL;
	/* Authenticate now; open PAM session after greeter releases seat/DRM. */
	bool ok = ui_pam_authenticate_user(srv->ui.username, srv->ui.password, &pamh, err, sizeof(err));
	srv->ui.authenticating = false;
	memset(srv->ui.password, 0, sizeof(srv->ui.password));
	if (ok) {
		srv->pamh = pamh;
		srv->ui.success = true;
		snprintf(srv->ui.message, sizeof(srv->ui.message), "Starting jtlabwc...");
		srv->ui.msg_color = C_GREEN; srv->ui.msg_set = true;
		launch_session(srv);
	} else {
		srv->pamh = NULL;
		snprintf(srv->ui.message, sizeof(srv->ui.message), "%s", err[0] ? err : "Authentication failed");
		srv->ui.msg_color = C_RED; srv->ui.msg_set = true;
	}
}

static void greeter_handle_key(struct server *srv, xkb_keysym_t sym) {
	if (srv->ui.success) return;
	if (sym == XKB_KEY_Escape) {
		if (srv->ui.focus == FIELD_PASS) memset(srv->ui.password, 0, sizeof(srv->ui.password));
		else memset(srv->ui.username, 0, sizeof(srv->ui.username));
		srv->ui.msg_set = false;
	} else if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) {
		srv->ui.focus = 1 - srv->ui.focus;
	} else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		if (srv->ui.focus == FIELD_USER) srv->ui.focus = FIELD_PASS;
		else greeter_try_login(srv);
	} else if (sym == XKB_KEY_BackSpace) {
		char *buf = srv->ui.focus == FIELD_PASS ? srv->ui.password : srv->ui.username;
		size_t n = strlen(buf);
		if (n > 0) buf[n-1] = '\0';
	} else {
		char ch[8];
		int n = xkb_keysym_to_utf8(sym, ch, sizeof(ch));
		if (n > 1) {
			char *buf = srv->ui.focus == FIELD_PASS ? srv->ui.password : srv->ui.username;
			size_t cur = strlen(buf);
			size_t cap = (srv->ui.focus == FIELD_PASS) ? sizeof(srv->ui.password) : sizeof(srv->ui.username);
			if (cur + (size_t)(n-1) < cap - 1) {
				memcpy(buf + cur, ch, (size_t)(n-1));
				buf[cur + (size_t)(n-1)] = '\0';
			}
		}
	}
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
	(void)data;
	struct g_keyboard *kb = wl_container_of(listener, kb, modifiers);
	wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(kb->server->seat, &kb->wlr_keyboard->modifiers);
}

static bool vt_switch_keysym(struct server *srv, xkb_keysym_t sym, uint32_t mods) {
	if (!srv->session) return false;
	if ((mods & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) != (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT))
		return false;

	unsigned vt = 0;
	if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12) {
		vt = (unsigned)(sym - XKB_KEY_F1 + 1);
	} else if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
		vt = (unsigned)(sym - XKB_KEY_XF86Switch_VT_1 + 1);
	}
	if (vt == 0) return false;

	wlr_session_change_vt(srv->session, vt);
	return true;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct g_keyboard *kb = wl_container_of(listener, kb, key);
	struct server *srv = kb->server;
	struct wlr_keyboard_key_event *event = data;
	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state, keycode, &syms);
	uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < nsyms; i++) {
			if (vt_switch_keysym(srv, syms[i], mods))
				return;
			greeter_handle_key(srv, syms[i]);
		}
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct g_keyboard *kb = wl_container_of(listener, kb, destroy);
	wl_list_remove(&kb->modifiers.link);
	wl_list_remove(&kb->key.link);
	wl_list_remove(&kb->destroy.link);
	wl_list_remove(&kb->link);
	free(kb);
}

static void server_new_keyboard(struct server *srv, struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);
	struct g_keyboard *kb = calloc(1, sizeof(*kb));
	kb->server = srv;
	kb->wlr_keyboard = wlr_keyboard;

	struct xkb_context *ctx = srv->xkb_ctx;
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	kb->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &kb->modifiers);
	kb->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &kb->key);
	kb->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &kb->destroy);

	wlr_seat_set_keyboard(srv->seat, kb->wlr_keyboard);
	wl_list_insert(&srv->keyboards, &kb->link);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	struct server *srv = wl_container_of(listener, srv, new_input);
	struct wlr_input_device *device = data;
	if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
		server_new_keyboard(srv, device);
	}
	uint32_t caps = 0;
	if (!wl_list_empty(&srv->keyboards)) caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(srv->seat, caps);
}

static void server_request_set_selection(struct wl_listener *listener, void *data) {
	struct server *srv = wl_container_of(listener, srv, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(srv->seat, event->source, event->serial);
}

static void output_frame(struct wl_listener *listener, void *data) {
	(void)data;
	struct g_output *out = wl_container_of(listener, out, frame);
	struct server *srv = out->server;
	int w = out->wlr_output->width;
	int h = out->wlr_output->height;
	if (w <= 0 || h <= 0) return;

	int stride = w * 4;
	size_t sz = (size_t)stride * (size_t)h;
	unsigned char *pixels = calloc(1, sz);
	if (!pixels) return;

	cairo_surface_t *cs = cairo_image_surface_create_for_data(pixels,
		CAIRO_FORMAT_ARGB32, w, h, stride);
	cairo_t *cr = cairo_create(cs);
	ui_draw(cr, &srv->ui, w, h);
	cairo_destroy(cr);
	cairo_surface_flush(cs);
	cairo_surface_destroy(cs);

	struct wlr_texture *tex = wlr_texture_from_pixels(srv->renderer,
		DRM_FORMAT_ARGB8888, (uint32_t)stride, (uint32_t)w, (uint32_t)h, pixels);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (!wlr_output_configure_primary_swapchain(out->wlr_output, &state, &out->wlr_output->swapchain)) {
		free(pixels);
		if (tex) wlr_texture_destroy(tex);
		wlr_output_state_finish(&state);
		return;
	}
	struct wlr_buffer *buffer = wlr_swapchain_acquire(out->wlr_output->swapchain);
	if (!buffer) {
		free(pixels);
		if (tex) wlr_texture_destroy(tex);
		wlr_output_state_finish(&state);
		return;
	}

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(srv->renderer, buffer, NULL);
	if (pass) {
		struct wlr_render_rect_options bg = {0};
		bg.box = (struct wlr_box){0, 0, w, h};
		bg.color = (struct wlr_render_color){R_(C_BG), G_(C_BG), B_(C_BG), 1.0f};
		wlr_render_pass_add_rect(pass, &bg);
		if (tex) {
			struct wlr_render_texture_options to = {0};
			to.texture = tex;
			to.dst_box = (struct wlr_box){0, 0, w, h};
			wlr_render_pass_add_texture(pass, &to);
		}
		wlr_render_pass_submit(pass);
	}

	wlr_output_state_set_buffer(&state, buffer);
	wlr_buffer_unlock(buffer);
	wlr_output_commit_state(out->wlr_output, &state);
	wlr_output_state_finish(&state);

	if (tex) wlr_texture_destroy(tex);
	free(pixels);
}

static void output_request_state(struct wl_listener *listener, void *data) {
	struct g_output *out = wl_container_of(listener, out, request_state);
	struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(out->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct g_output *out = wl_container_of(listener, out, destroy);
	wl_list_remove(&out->frame.link);
	wl_list_remove(&out->request_state.link);
	wl_list_remove(&out->destroy.link);
	wl_list_remove(&out->link);
	free(out);
}

struct server_g {
	struct server srv;
	struct wl_listener new_output;
};

static void new_output_notify(struct wl_listener *listener, void *data) {
	struct server_g *sg = wl_container_of(listener, sg, new_output);
	struct server *srv = &sg->srv;
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, srv->allocator, srv->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode) wlr_output_state_set_mode(&state, mode);
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	struct g_output *out = calloc(1, sizeof(*out));
	out->wlr_output = wlr_output;
	out->server = srv;
	out->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &out->frame);
	out->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &out->request_state);
	out->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &out->destroy);
	wl_list_insert(&srv->outputs, &out->link);

	struct wlr_output_layout_output *lout = wlr_output_layout_add_auto(srv->output_layout, wlr_output);
	struct wlr_scene_output *scene_output = wlr_scene_output_create(srv->scene, wlr_output);
	wlr_scene_output_layout_add_output(srv->scene_layout, lout, scene_output);
}

static void apply_pam_envlist(char **envlist) {
	if (!envlist) return;
	for (char **e = envlist; *e; e++) {
		char *eq = strchr(*e, '=');
		if (eq) {
			*eq = '\0';
			setenv(*e, eq + 1, 1);
			*eq = '=';
		}
	}
}

static void exec_jtlabwc_as_user(const char *username, char **pam_envlist) {
	struct passwd *pw = getpwnam(username);
	if (!pw) {
		fprintf(stderr, "jtgreeter: unknown user '%s'\n", username);
		_exit(127);
	}

	if (initgroups(pw->pw_name, pw->pw_gid) < 0) {
		perror("jtgreeter: initgroups");
	}
	if (setgid(pw->pw_gid) < 0) {
		perror("jtgreeter: setgid");
		_exit(127);
	}
	if (setuid(pw->pw_uid) < 0) {
		perror("jtgreeter: setuid");
		_exit(127);
	}

	const char *home = pw->pw_dir ? pw->pw_dir : "/";
	const char *shell = (pw->pw_shell && pw->pw_shell[0]) ? pw->pw_shell : "/bin/sh";
	if (chdir(home) < 0) {
		chdir("/");
		home = "/";
	}

	/* Base environment similar to what a display manager provides. */
	setenv("HOME", home, 1);
	setenv("USER", pw->pw_name, 1);
	setenv("LOGNAME", pw->pw_name, 1);
	setenv("SHELL", shell, 1);
	setenv("PWD", home, 1);
	if (!getenv("PATH")) {
#ifdef _PATH_DEFPATH
		setenv("PATH", _PATH_DEFPATH, 1);
#else
		setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);
#endif
	}
	setenv("XDG_SESSION_TYPE", "wayland", 1);
	setenv("XDG_SESSION_CLASS", "user", 1);
	setenv("XDG_SESSION_DESKTOP", "jtlabwc", 1);
	setenv("XDG_CURRENT_DESKTOP", "jtlabwc", 1);
	setenv("DESKTOP_SESSION", "jtlabwc", 1);
	if (!getenv("XDG_SEAT")) setenv("XDG_SEAT", "seat0", 1);
	if (!getenv("XDG_VTNR")) {
		const char *t = ttyname(STDIN_FILENO);
		if (!t) t = ttyname(STDOUT_FILENO);
		if (t && strncmp(t, "/dev/tty", 8) == 0 && t[8] >= '1' && t[8] <= '9')
			setenv("XDG_VTNR", t + 8, 1);
	}

	/* Remove greeter-internal Wayland socket; jtlabwc will start its own session/compositor. */
	unsetenv("WAYLAND_DISPLAY");
	unsetenv("DISPLAY");

	/* Apply PAM environment last so pam_elogind/pam_systemd provided vars
	 * (XDG_RUNTIME_DIR, XDG_SESSION_ID, DBUS_SESSION_BUS_ADDRESS, etc.) take effect. */
	apply_pam_envlist(pam_envlist);

	/* Never keep greeter/root runtime dir in the user session. */
	const char *xdg = getenv("XDG_RUNTIME_DIR");
	char user_xdg[128];
	snprintf(user_xdg, sizeof(user_xdg), "/run/user/%d", (int)pw->pw_uid);
	if (!xdg || !xdg[0] || strcmp(xdg, "/run/user/0") == 0) {
		setenv("XDG_RUNTIME_DIR", user_xdg, 1);
		xdg = getenv("XDG_RUNTIME_DIR");
	}

	/* Capture user session output for diagnostics (service runs without a useful stderr). */
	char logpath[512];
	int lfd = -1;
	if (xdg && xdg[0]) {
		snprintf(logpath, sizeof(logpath), "%s/jtgreeter-session.log", xdg);
		lfd = open(logpath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
	}
	if (lfd < 0) {
		snprintf(logpath, sizeof(logpath), "/tmp/jtgreeter-session-%d.log", (int)getuid());
		lfd = open(logpath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
	}
	if (lfd >= 0) {
		(void)dup2(lfd, STDOUT_FILENO);
		(void)dup2(lfd, STDERR_FILENO);
		if (lfd > STDERR_FILENO) close(lfd);
		fprintf(stderr, "\n--- jtgreeter launching dbus-run-session jtlabwc for user %s (log: %s) ---\n", pw->pw_name, logpath);
		fprintf(stderr, "HOME=%s XDG_RUNTIME_DIR=%s XDG_SESSION_TYPE=%s WAYLAND_DISPLAY=%s DISPLAY=%s PATH=%s\n",
			getenv("HOME") ? getenv("HOME") : "",
			getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "",
			getenv("XDG_SESSION_TYPE") ? getenv("XDG_SESSION_TYPE") : "",
			getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "",
			getenv("DISPLAY") ? getenv("DISPLAY") : "",
			getenv("PATH") ? getenv("PATH") : "");
	}

	execlp("dbus-run-session", "dbus-run-session", "--", "jtlabwc", (char *)NULL);
	execlp("dbus-run-session", "dbus-run-session", "--", "/usr/local/bin/jtlabwc", (char *)NULL);
	execlp("dbus-run-session", "dbus-run-session", "--", "/usr/bin/jtlabwc", (char *)NULL);
	perror("jtgreeter: failed to exec dbus-run-session jtlabwc");
	_exit(127);
}

static void ensure_greeter_runtime_dir(void) {
	const char *xdg = getenv("XDG_RUNTIME_DIR");
	if (xdg && xdg[0] == '/') {
		struct stat st;
		if (stat(xdg, &st) == 0 && S_ISDIR(st.st_mode))
			return;
	}
	/* Greeter runs as root in minimal init environments; Wayland needs a runtime dir for sockets. */
	const char *dir = "/run/jtgreeter";
	if (getuid() == 0) {
		mkdir("/run/user", 0755);
		mkdir("/run/user/0", 0700);
		chown("/run/user/0", 0, 0);
		dir = "/run/user/0";
	} else {
		mkdir("/run/jtgreeter", 0700);
	}
	setenv("XDG_RUNTIME_DIR", dir, 1);
}

static void clear_tty1_console(void) {
	int fd = open("/dev/tty1", O_WRONLY | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) return;
	const char *seq = "\033[H\033[2J\033[3J";
	(void)write(fd, seq, 11);
	(void)write(fd, "\033[?25h", 6); /* show cursor */
	close(fd);
}

static void stop_tty1_getty_runtime(void) {
	/* Best-effort runtime stop/cleanup for tty1 handoff (OpenRC and systemd).
	 * Avoid killing all tty1 processes here: this can run inside the greeter
	 * during session handoff and terminate jtgreeter itself. */
	const char *cmds[] = {
		"rc-service agetty.tty1 --ifstarted stop >/dev/null 2>&1",
		"rc-service getty.tty1 --ifstarted stop >/dev/null 2>&1",
		"rc-service agetty-autologin.tty1 --ifstarted stop >/dev/null 2>&1",
		"rc-service fgetty.tty1 --ifstarted stop >/dev/null 2>&1",
		"systemctl stop getty@tty1.service autovt@tty1.service >/dev/null 2>&1",
		/* Best-effort: stop common tty1 login/getty programs without killing everything on the tty. */
		"pkill -x agetty >/dev/null 2>&1",
		"pkill -x getty >/dev/null 2>&1",
		"pkill -x fgetty >/dev/null 2>&1",
		"pkill -x login >/dev/null 2>&1",
		NULL
	};
	for (int i = 0; cmds[i]; i++) (void)system(cmds[i]);

	clear_tty1_console();
}

static volatile sig_atomic_t g_terminate = 0;
static void on_term_signal(int sig) { (void)sig; g_terminate = 1; }

static int run_greeter_once(void) {
	struct server_g sg = {0};
	struct server *srv = &sg.srv;

	ensure_greeter_runtime_dir();

	srv->wl_display = wl_display_create();
	if (!srv->wl_display) {
		fprintf(stderr, "jtgreeter: failed to create wl_display\n");
		return 1;
	}

	srv->backend = wlr_backend_autocreate(wl_display_get_event_loop(srv->wl_display), &srv->session);
	if (!srv->backend) {
		fprintf(stderr, "jtgreeter: failed to create backend\n");
		wl_display_destroy(srv->wl_display);
		return 1;
	}

	srv->renderer = wlr_renderer_autocreate(srv->backend);
	if (!srv->renderer) {
		fprintf(stderr, "jtgreeter: failed to create renderer\n");
		wlr_backend_destroy(srv->backend);
		wl_display_destroy(srv->wl_display);
		return 1;
	}
	wlr_renderer_init_wl_display(srv->renderer, srv->wl_display);

	srv->allocator = wlr_allocator_autocreate(srv->backend, srv->renderer);
	if (!srv->allocator) {
		fprintf(stderr, "jtgreeter: failed to create allocator\n");
		wlr_backend_destroy(srv->backend);
		wl_display_destroy(srv->wl_display);
		return 1;
	}

	wlr_compositor_create(srv->wl_display, 5, srv->renderer);
	wlr_subcompositor_create(srv->wl_display);
	wlr_data_device_manager_create(srv->wl_display);

	srv->output_layout = wlr_output_layout_create(srv->wl_display);
	wl_list_init(&srv->outputs);
	sg.new_output.notify = new_output_notify;
	wl_signal_add(&srv->backend->events.new_output, &sg.new_output);

	srv->scene = wlr_scene_create();
	srv->scene_layout = wlr_scene_attach_output_layout(srv->scene, srv->output_layout);

	wl_list_init(&srv->keyboards);
	srv->new_input.notify = server_new_input;
	wl_signal_add(&srv->backend->events.new_input, &srv->new_input);

	srv->seat = wlr_seat_create(srv->wl_display, "seat0");
	srv->request_set_selection.notify = server_request_set_selection;
	wl_signal_add(&srv->seat->events.request_set_selection, &srv->request_set_selection);

	srv->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	ui_init(&srv->ui, "");
	srv->running = true;
	srv->launch_jtlabwc = false;
	srv->pamh = NULL;

	const char *socket = wl_display_add_socket_auto(srv->wl_display);
	if (!socket) {
		fprintf(stderr, "jtgreeter: failed to add Wayland socket (check XDG_RUNTIME_DIR)\n");
		if (srv->new_input.link.prev && srv->new_input.link.next)
			wl_list_remove(&srv->new_input.link);
		if (sg.new_output.link.prev && sg.new_output.link.next)
			wl_list_remove(&sg.new_output.link);
		if (srv->request_set_selection.link.prev && srv->request_set_selection.link.next)
			wl_list_remove(&srv->request_set_selection.link);
		if (srv->xkb_ctx) xkb_context_unref(srv->xkb_ctx);
		wl_display_destroy_clients(srv->wl_display);
		wlr_backend_destroy(srv->backend);
		wl_display_destroy(srv->wl_display);
		return 1;
	}

	if (!wlr_backend_start(srv->backend)) {
		fprintf(stderr, "jtgreeter: failed to start backend\n");
		if (srv->new_input.link.prev && srv->new_input.link.next)
			wl_list_remove(&srv->new_input.link);
		if (sg.new_output.link.prev && sg.new_output.link.next)
			wl_list_remove(&sg.new_output.link);
		if (srv->request_set_selection.link.prev && srv->request_set_selection.link.next)
			wl_list_remove(&srv->request_set_selection.link);
		if (srv->xkb_ctx) xkb_context_unref(srv->xkb_ctx);
		wl_display_destroy_clients(srv->wl_display);
		wlr_backend_destroy(srv->backend);
		wl_display_destroy(srv->wl_display);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", socket, true);
	wlr_log(WLR_INFO, "jtgreeter running on WAYLAND_DISPLAY=%s", socket);

	while (srv->running && !g_terminate) {
		wl_display_flush_clients(srv->wl_display);
		wl_event_loop_dispatch(wl_display_get_event_loop(srv->wl_display), 16);
	}
	if (g_terminate) {
		srv->running = false;
		srv->launch_jtlabwc = false;
	}

	char user_to_launch[128] = {0};
	snprintf(user_to_launch, sizeof(user_to_launch), "%s", srv->ui.username);
	bool do_launch = srv->launch_jtlabwc;
	pam_handle_t *pamh = srv->pamh;

	/* Tear down greeter compositor to release DRM/VT/seat before launching user session. */
	wl_display_destroy_clients(srv->wl_display);
	unsetenv("WAYLAND_DISPLAY");

	/* Remove listeners registered on backend/seat signals before destroying them. */
	if (srv->new_input.link.prev && srv->new_input.link.next)
		wl_list_remove(&srv->new_input.link);
	if (sg.new_output.link.prev && sg.new_output.link.next)
		wl_list_remove(&sg.new_output.link);
	if (srv->request_set_selection.link.prev && srv->request_set_selection.link.next)
		wl_list_remove(&srv->request_set_selection.link);

	if (srv->backend) wlr_backend_destroy(srv->backend);
	if (srv->xkb_ctx) xkb_context_unref(srv->xkb_ctx);
	wl_display_destroy(srv->wl_display);

	/* During handoff to user session, prevent tty1 getty/login prompt flash. */
	if (do_launch) {
		stop_tty1_getty_runtime();
	}

	if (do_launch && pamh) {
		char err[256] = {0};
		if (!ui_pam_open_session(pamh, err, sizeof(err))) {
			fprintf(stderr, "jtgreeter: failed to open PAM session: %s\n", err[0] ? err : "unknown error");
			/* ui_pam_open_session ends pamh on failure */
			return 1;
		}

		/* Best-effort: ensure user runtime dir exists before dropping privileges in child. */
		struct passwd *pw_launch = getpwnam(user_to_launch);
		if (pw_launch) {
			char xdg_dir[128];
			snprintf(xdg_dir, sizeof(xdg_dir), "/run/user/%d", (int)pw_launch->pw_uid);
			mkdir("/run/user", 0755);
			if (mkdir(xdg_dir, 0700) == 0 || errno == EEXIST) {
				chown(xdg_dir, pw_launch->pw_uid, pw_launch->pw_gid);
				chmod(xdg_dir, 0700);
			}
		}

		char **pam_env = pam_getenvlist(pamh);

		pid_t pid = fork();
		if (pid < 0) {
			perror("jtgreeter: fork");
			if (pam_env) {
				for (char **e = pam_env; *e; e++) free(*e);
				free(pam_env);
			}
			ui_pam_login_end(pamh);
			return 1;
		} else if (pid == 0) {
			/* Session child: set up session/tty, drop privileges, exec jtlabwc. */
			signal(SIGTERM, SIG_DFL);
			signal(SIGINT, SIG_DFL);
			signal(SIGHUP, SIG_DFL);
			setsid();

			/* Give the user session a controlling tty on tty1 (helps logind/DRM handoff). */
			int ttyfd = open("/dev/tty1", O_RDWR | O_NOCTTY);
			if (ttyfd >= 0) {
				(void)dup2(ttyfd, STDIN_FILENO);
				if (ttyfd > STDERR_FILENO) close(ttyfd);
				(void)ioctl(STDIN_FILENO, TIOCSCTTY, 0);
			}

			setenv("XDG_SEAT", "seat0", 0);
			setenv("XDG_VTNR", "1", 0);

			exec_jtlabwc_as_user(user_to_launch, pam_env);
			_exit(127);
		} else {
			int status = 0;
			while (waitpid(pid, &status, 0) < 0) {
				if (errno == EINTR) continue;
				perror("jtgreeter: waitpid");
				break;
			}
			if (pam_env) {
				for (char **e = pam_env; *e; e++) free(*e);
				free(pam_env);
			}
			ui_pam_login_end(pamh);
			if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
				fprintf(stderr, "jtgreeter: user session exited with status %d\n", WEXITSTATUS(status));
			} else if (WIFSIGNALED(status)) {
				fprintf(stderr, "jtgreeter: user session terminated by signal %d\n", WTERMSIG(status));
			}
			return 0; /* session ended: caller can restart greeter */
		}
	}

	if (pamh) {
		pam_end(pamh, PAM_SUCCESS);
	}
	return 0;
}

static int run_greeter_mode(void) {
	wlr_log_init(WLR_INFO, NULL);
	/* Behave like a display manager: after a user session exits, show greeter again. */
	for (;;) {
		if (g_terminate) return 0;
		int rc = run_greeter_once();
		if (g_terminate) return 0;
		if (rc != 0) return rc;
		/* Small delay to avoid tight respawn loops if backend/session fails immediately. */
		sleep(1);
	}
}

static void usage(const char *argv0) {
	fprintf(stderr,
		"Usage: %s [--lock] [--accent #RRGGBB]\n"
		"  --lock           Run as lockscreen in an existing Wayland session\n"
		"  --accent COLOR   Accent color (e.g. #E8A03C), like jtspeedtest\n",
		argv0);
}

static void preload_jt_binaries(void) {
	pid_t pid = fork();
	if (pid < 0) return;
	if (pid > 0) {
		/* Reap intermediate child so no zombie is left. */
		while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
		return;
	}
	if (fork() > 0) _exit(0); /* detach: grandchild continues, init reaps it */

	(void)setsid();
	(void)setpriority(PRIO_PROCESS, 0, 10);
	signal(SIGTERM, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGPIPE, SIG_IGN);

	DIR *dir = opendir("/usr/local/bin");
	if (!dir) _exit(0);

	char buf[64 * 1024];
	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (strncmp(ent->d_name, "jt", 2) != 0) continue;
		char path[PATH_MAX];
		int n = snprintf(path, sizeof(path), "/usr/local/bin/%s", ent->d_name);
		if (n < 0 || (size_t)n >= sizeof(path)) continue;

		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

		int fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0) continue;
#ifdef POSIX_FADV_WILLNEED
		(void)posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
#endif
		while (read(fd, buf, sizeof(buf)) > 0) {}
		close(fd);
	}
	closedir(dir);
	_exit(0);
}

int main(int argc, char **argv) {
	bool lock_mode = false;
	accent_cli(&argc, argv);
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--lock")) lock_mode = true;
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(argv[0]);
			return 0;
		}
	}
	if (lock_mode) {
		return run_lock_mode(argc, argv);
	}

	/* Greeter/DM mode: run as root on a VT/seat, start user sessions with PAM. */
	struct sigaction sa = {0};
	sa.sa_handler = on_term_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	/* Ignore SIGPIPE; reap children promptly if any unexpected child handling occurs. */
	signal(SIGPIPE, SIG_IGN);

	/* Warm page cache for installed jt* tools so user sessions start faster. */
	preload_jt_binaries();

	return run_greeter_mode();
}

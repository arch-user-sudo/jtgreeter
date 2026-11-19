.POSIX:
.SUFFIXES:

include config.mk

AMALG = amalgamate.py
SCAN  = scan.py

WLR_SRCS = $(shell find wlroots -type f \( -name '*.c' -o -name '*.h' -o -name '*.xml' -o -name '*.sh' \) 2>/dev/null)

# wayland-server/wlroots deps for greeter mode, wayland-client for lockscreen mode
PKGS = wayland-server wayland-client xkbcommon libinput pixman-1 libdrm egl gbm glesv2 \
       libudev libseat libdisplay-info libliftoff cairo pangocairo pango harfbuzz glib-2.0

ifneq ($(XWAYLAND)$(X11_BACKEND),)
PKGS += xcb xcb-dri3 xcb-present xcb-render xcb-renderutil xcb-shm xcb-xfixes \
        xcb-xinput xcb-composite xcb-ewmh xcb-icccm xcb-res xcb-shape
endif

ifneq ($(VULKAN),)
PKGS += vulkan
endif

CFLAGS += $(shell $(PKG_CONFIG) --cflags $(PKGS)) -I. -Iamalgamate-build/include -Iwlroots/include -Ibuild/proto
LDFLAGS += $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm -lpam -lrt -lpthread

WAYLAND_SCANNER ?= wayland-scanner
WAYLAND_PROTOCOLS := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)

PROTO_GEN_DIR = build/proto

CLIENT_PROTOS = \
	$(PROTO_GEN_DIR)/ext-session-lock-v1-client-protocol.h \
	$(PROTO_GEN_DIR)/ext-session-lock-v1-protocol.c

SRCS = jtgreeter.c lock.c ui.c

all: wlroots.c jtgreeter jtgreeterctl

amalgamate: wlroots.c

TIDY        = clang-tidy
TIDY_CHECKS = -*,bugprone-macro-parentheses,bugprone-macro-repeated-side-effects,performance-type-promotion-in-math-fn,readability-inconsistent-declaration-parameter-name

define tidy_wlroots
	@if [ -n "$(TIDY)" ] && command -v $(TIDY) >/dev/null 2>&1; then \
		echo "clang-tidy: auto-fixing wlroots.c"; \
		$(TIDY) -quiet -fix -checks='$(TIDY_CHECKS)' \
			--header-filter='a^' wlroots.c -- \
			$(CFLAGS) || exit 1; \
	else \
		echo "clang-tidy not found; skipping wlroots.c auto-fix pass"; \
	fi
endef

wlroots.c: jtgreeter.c config.mk $(AMALG) $(SCAN) $(WLR_SRCS)
	python3 $(AMALG)
	$(tidy_wlroots)

$(PROTO_GEN_DIR)/ext-session-lock-v1-client-protocol.h: $(WAYLAND_PROTOCOLS)/staging/ext-session-lock/ext-session-lock-v1.xml
	@mkdir -p $(PROTO_GEN_DIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(PROTO_GEN_DIR)/ext-session-lock-v1-protocol.c: $(WAYLAND_PROTOCOLS)/staging/ext-session-lock/ext-session-lock-v1.xml
	@mkdir -p $(PROTO_GEN_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

jtgreeter: $(SRCS) ui.h wlroots.c $(CLIENT_PROTOS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRCS) \
		$(PROTO_GEN_DIR)/ext-session-lock-v1-protocol.c \
		$(LDFLAGS)

jtgreeterctl: jtgreeterctl.c
	$(CC) $(CFLAGS) -o $@ jtgreeterctl.c $(LDFLAGS)

install: jtgreeter jtgreeterctl
	install -Dm755 jtgreeter $(DESTDIR)$(BINDIR)/jtgreeter
	install -Dm755 jtgreeterctl $(DESTDIR)$(BINDIR)/jtgreeterctl
	@mkdir -p $(DESTDIR)/etc/jt; \
	chmod 0777 $(DESTDIR)/etc/jt 2>/dev/null || true; \
	if [ ! -f $(DESTDIR)/etc/jt/accent.conf ]; then printf '%s\n' '#E8A03C' > $(DESTDIR)/etc/jt/accent.conf; fi; \
	chmod 0666 $(DESTDIR)/etc/jt/accent.conf 2>/dev/null || true; \
	echo "installed accent config $(DESTDIR)/etc/jt/accent.conf"
	@# Install a distro-appropriate PAM config when possible for portability.
	@pam_installed=0; \
	if [ -f /etc/pam.d/system-local-login ] || [ -f $(DESTDIR)/etc/pam.d/system-local-login ]; then \
		printf '%s\n' '#%PAM-1.0' 'auth       include      system-local-login' 'account    include      system-local-login' 'password   include      system-local-login' 'session    include      system-local-login' > /tmp/jtgreeter.pam.install; \
		install -Dm644 /tmp/jtgreeter.pam.install $(DESTDIR)$(SYSCONFDIR)/pam.d/jtgreeter; rm -f /tmp/jtgreeter.pam.install; pam_installed=1; \
		echo "installed PAM config (system-local-login) to $(DESTDIR)$(SYSCONFDIR)/pam.d/jtgreeter"; \
	elif [ -f /etc/pam.d/login ] || [ -f $(DESTDIR)/etc/pam.d/login ]; then \
		printf '%s\n' '#%PAM-1.0' 'auth       include      login' 'account    include      login' 'password   include      login' 'session    include      login' > /tmp/jtgreeter.pam.install; \
		install -Dm644 /tmp/jtgreeter.pam.install $(DESTDIR)$(SYSCONFDIR)/pam.d/jtgreeter; rm -f /tmp/jtgreeter.pam.install; pam_installed=1; \
		echo "installed PAM config (login) to $(DESTDIR)$(SYSCONFDIR)/pam.d/jtgreeter"; \
	elif [ -f /etc/pam.d/common-auth ] || [ -f $(DESTDIR)/etc/pam.d/common-auth ]; then \
		printf '%s\n' '#%PAM-1.0' '@include common-auth' '@include common-account' '@include common-password' '@include common-session' > /tmp/jtgreeter.pam.install; \
		install -Dm644 /tmp/jtgreeter.pam.install $(DESTDIR)$(SYSCONFDIR)/pam.d/jtgreeter; rm -f /tmp/jtgreeter.pam.install; pam_installed=1; \
		echo "installed PAM config (common-*) to $(DESTDIR)$(SYSCONFDIR)/pam.d/jtgreeter"; \
	elif [ -f /etc/pam.d/system-auth ] || [ -f $(DESTDIR)/etc/pam.d/system-auth ]; then \
		printf '%s\n' '#%PAM-1.0' 'auth       include      system-auth' 'account    include      system-auth' 'password   include      system-auth' 'session    include      system-auth' > /tmp/jtgreeter.pam.install; \
		install -Dm644 /tmp/jtgreeter.pam.install $(DESTDIR)$(SYSCONFDIR)/pam.d/jtgreeter; rm -f /tmp/jtgreeter.pam.install; pam_installed=1; \
		echo "installed PAM config (system-auth) to $(DESTDIR)$(SYSCONFDIR)/pam.d/jtgreeter"; \
	fi; \
	if [ "$$pam_installed" -eq 0 ]; then \
		install -Dm644 jtgreeter.pam $(DESTDIR)$(SYSCONFDIR)/pam.d/jtgreeter; \
		echo "installed default PAM config to $(DESTDIR)$(SYSCONFDIR)/pam.d/jtgreeter (verify for your distro)"; \
	fi
	@init="$(INIT)"; \
	if [ "$$init" = "auto" ]; then \
		if [ -d /run/systemd/system ] || command -v systemctl >/dev/null 2>&1; then \
			init=systemd; \
		elif [ -d /run/openrc ] || [ -x /sbin/openrc-run ] || command -v rc-service >/dev/null 2>&1; then \
			init=openrc; \
		else \
			init=none; \
		fi; \
	fi; \
	case "$$init" in \
		systemd) \
			mkdir -p $(DESTDIR)$(SYSTEMD_SYSTEM_UNITDIR); \
			sed 's|@BINDIR@|$(BINDIR)|g' jtgreeter.service > $(DESTDIR)$(SYSTEMD_SYSTEM_UNITDIR)/jtgreeter.service; \
			echo "installed systemd service to $(DESTDIR)$(SYSTEMD_SYSTEM_UNITDIR)/jtgreeter.service"; \
			;; \
		openrc) \
			mkdir -p $(DESTDIR)$(OPENRC_INITDIR); \
			sed 's|@BINDIR@|$(BINDIR)|g' jtgreeter.initd > $(DESTDIR)$(OPENRC_INITDIR)/jtgreeter; \
			chmod 755 $(DESTDIR)$(OPENRC_INITDIR)/jtgreeter; \
			echo "installed OpenRC init script to $(DESTDIR)$(OPENRC_INITDIR)/jtgreeter"; \
			if [ -z "$(DESTDIR)" ] && [ -f /etc/inittab ]; then \
				if grep -Eq '^[[:space:]]*[^#].*tty1' /etc/inittab 2>/dev/null && grep -Eq '^[[:space:]]*[^#].*(getty|agetty)' /etc/inittab 2>/dev/null; then \
					cp -f /etc/inittab /etc/inittab.bak.jtgreeter 2>/dev/null || true; \
					sed -i '/^[[:space:]]*#/b; /tty1/{ /getty/b c; /agetty/b c; b }; b; :c s/^/# jtgreeter-disabled: /' /etc/inittab || true; \
					if grep -q '^# jtgreeter-disabled: ' /etc/inittab 2>/dev/null; then \
						echo "commented out active tty1 getty/agetty entries in /etc/inittab (backup: /etc/inittab.bak.jtgreeter)"; \
						command -v init >/dev/null 2>&1 && init q >/dev/null 2>&1 || true; \
						command -v telinit >/dev/null 2>&1 && telinit q >/dev/null 2>&1 || true; \
					fi; \
				fi; \
			fi; \
			if [ -z "$(DESTDIR)" ]; then \
				echo "to enable: rc-update add jtgreeter default"; \
			fi; \
			;; \
		none) \
			echo "no systemd/openrc init detected; skipping startup entry (set INIT=systemd or INIT=openrc to force)"; \
			;; \
		*) \
			echo "unknown INIT='$$init' (expected auto|systemd|openrc|none)" >&2; exit 1; \
			;; \
	esac

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/jtgreeter
	rm -f $(DESTDIR)$(BINDIR)/jtgreeterctl
	rm -f $(DESTDIR)$(SYSCONFDIR)/pam.d/jtgreeter
	rm -f $(DESTDIR)$(SYSTEMD_SYSTEM_UNITDIR)/jtgreeter.service
	rm -f $(DESTDIR)$(OPENRC_INITDIR)/jtgreeter
	@if [ -z "$(DESTDIR)" ] && [ -f /etc/inittab ] && grep -q '^# jtgreeter-disabled: ' /etc/inittab 2>/dev/null; then \
		sed -i 's/^# jtgreeter-disabled: //' /etc/inittab; \
		echo "restored tty1 getty/agetty entries in /etc/inittab"; \
		command -v init >/dev/null 2>&1 && init q >/dev/null 2>&1 || true; \
		command -v telinit >/dev/null 2>&1 && telinit q >/dev/null 2>&1 || true; \
	fi

clean:
	rm -rf build jtgreeter jtgreeterctl

distclean: clean
	rm -f wlroots.c

.PHONY: all install uninstall clean distclean amalgamate

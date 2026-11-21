PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SYSCONFDIR ?= /etc
DATADIR ?= $(PREFIX)/share

PKG_CONFIG ?= pkg-config
CC ?= cc
STRIP ?= strip

# Feature toggles for wlroots amalgamation (read by amalgamate.py)
# Greeter needs DRM + libinput + session; no X11/XWayland/Vulkan by default.
#XWAYLAND =
#X11_BACKEND =
#VULKAN =

CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra
CFLAGS += -DWLR_USE_UNSTABLE
CFLAGS += -ffunction-sections -fdata-sections

LDFLAGS ?=
LDFLAGS += -Wl,--gc-sections

# Init/service install (auto-detect by default: systemd, openrc, or none)
INIT ?= auto
SYSTEMD_SYSTEM_UNITDIR ?= $(shell $(PKG_CONFIG) --variable=systemdsystemunitdir systemd 2>/dev/null || echo /usr/lib/systemd/system)
OPENRC_INITDIR ?= $(SYSCONFDIR)/init.d

# tty1 conflicts are handled at service runtime. The /etc/inittab edit is opt-in:
# run `make install greeter` to disable the tty1 getty after installing.
DISABLE_TTY1 ?= no

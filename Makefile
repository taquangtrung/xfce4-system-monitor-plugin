# Makefile for xfce4-system-monitor-plugin
#
# Usage (run every target as your normal user; the privileged steps
# self-elevate with $(SUDO)):
#   make                 build the plugin module
#
#   Debian / Ubuntu (dpkg tracks the files, so uninstall is exact):
#   make deb             build the .deb package only
#   make install         build the .deb, install it with dpkg, refresh the panel
#   make uninstall       remove the package
#
#   Any other distro (Fedora, Arch, openSUSE, ...):
#   make install-local   copy the built files straight into PREFIX
#   make uninstall-local remove those files again
#
#   Development helpers:
#   make reset-config    quit panel, delete saved config, restart panel
#   make reinstall       install, then reset-config
#
# Packagers: use "make stage DESTDIR=<dir>" and skip the targets above.
# Override PREFIX / LIBDIR / DATADIR / DESTDIR / SUDO as needed.

PACKAGE       := system-monitor
GETTEXT_PKG   := xfce4-system-monitor-plugin

# Where the panel stores each plugin instance's saved settings.
XDG_CONFIG_HOME ?= $(HOME)/.config
PANEL_CONFIG_DIR := $(XDG_CONFIG_HOME)/xfce4/panel

# Command used to elevate the privileged install steps. Set SUDO= (empty) when
# already running as root, e.g. inside a container build.
SUDO          ?= sudo

PREFIX        ?= /usr
LIBDIR        ?= $(shell pkg-config --variable=libdir libxfce4panel-2.0)

# Without the -dev package pkg-config returns nothing and LIBDIR would silently
# become "/", installing the module to /xfce4/panel/plugins. Fail loudly instead.
ifeq ($(filter clean,$(MAKECMDGOALS)),)
ifeq ($(strip $(LIBDIR)),)
$(error libxfce4panel-2.0 not found by pkg-config: install the development \
packages listed in README.md, or set LIBDIR explicitly)
endif
endif
DATADIR       ?= $(PREFIX)/share
LOCALEDIR     ?= $(DATADIR)/locale

PLUGIN_DIR    := $(LIBDIR)/xfce4/panel/plugins
DESKTOP_DIR   := $(DATADIR)/xfce4/panel/plugins
DOC_DIR       := $(DATADIR)/doc/xfce4-$(PACKAGE)-plugin

PKGS          := libxfce4panel-2.0 libxfce4util-1.0 gtk+-3.0 libnotify
PKG_CFLAGS    := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS      := $(shell pkg-config --libs $(PKGS)) -lm

CFLAGS        ?= -O2 -g
CFLAGS        += -Wall -Wextra -fPIC
CPPFLAGS      += -DGETTEXT_PACKAGE=\"$(GETTEXT_PKG)\" \
                 -DPACKAGE_LOCALE_DIR=\"$(LOCALEDIR)\"

SRC_DIR       := panel-plugin
MODULE        := lib$(PACKAGE).so
SOURCES       := $(SRC_DIR)/system-monitor.c \
                 $(SRC_DIR)/system-monitor-fs.c \
                 $(SRC_DIR)/system-monitor-draw.c \
                 $(SRC_DIR)/system-monitor-config.c \
                 $(SRC_DIR)/system-monitor-dialog.c
OBJECTS       := $(SOURCES:.c=.o)
DESKTOP       := $(SRC_DIR)/$(PACKAGE).desktop
DESKTOP_IN    := $(SRC_DIR)/$(PACKAGE).desktop.in

# Debian packaging.
VERSION       ?= 0.1.0
MAINTAINER    ?= Quang Trung Ta <taquangtrungvn@gmail.com>
DEB_PKGNAME   := xfce4-$(PACKAGE)-plugin
DEB_ARCH      := $(shell dpkg --print-architecture 2>/dev/null || echo amd64)
DEB_FILE      := $(DEB_PKGNAME)_$(VERSION)_$(DEB_ARCH).deb
DEB_STAGE     := $(CURDIR)/build/deb

all: $(MODULE) $(DESKTOP)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(PKG_CFLAGS) -c $< -o $@

$(MODULE): $(OBJECTS)
	$(CC) -shared $(CFLAGS) $(LDFLAGS) $^ -o $@ $(PKG_LIBS)

# The .desktop needs no substitutions today; copy it through so a rule exists
# if translated fields are added later.
$(DESKTOP): $(DESKTOP_IN)
	cp $< $@

# Guard against building as root. Running "sudo make ..." leaves root-owned
# artifacts (build/, *.deb) that a later normal-user build cannot clean, which
# then breaks "make install". The install/uninstall targets self-elevate with
# sudo only for the dpkg/apt step, so make itself must run as your normal user.
check-not-root:
	@if [ "$$(id -u)" = 0 ]; then \
	  echo "ERROR: run 'make' as your normal user, not with sudo."; \
	  echo "       install/uninstall self-elevate with sudo only where needed."; \
	  exit 1; \
	fi

# The .deb targets are Debian/Ubuntu only; point everyone else at install-local
# rather than failing halfway through with "dpkg-deb: not found".
check-dpkg:
	@command -v dpkg-deb >/dev/null 2>&1 || { \
	  echo "ERROR: dpkg-deb not found; the .deb targets are Debian/Ubuntu only."; \
	  echo "       On other distros run: make && make install-local"; \
	  exit 1; \
	}

# Stage the built files into $(DESTDIR) with the final layout. Used to populate
# the .deb tree, and directly by packagers via "make stage DESTDIR=<dir>".
stage: all
	install -d $(DESTDIR)$(PLUGIN_DIR)
	install -m 0755 $(MODULE) $(DESTDIR)$(PLUGIN_DIR)/$(MODULE)
	install -d $(DESTDIR)$(DESKTOP_DIR)
	install -m 0644 $(DESKTOP) $(DESTDIR)$(DESKTOP_DIR)/$(PACKAGE).desktop
	install -d $(DESTDIR)$(DOC_DIR)
	install -m 0644 LICENSE $(DESTDIR)$(DOC_DIR)/copyright

# Install is deb-based so dpkg tracks the files and uninstall is clean. We use
# dpkg -i (not apt): it always re-unpacks even when the version is unchanged
# (it stays 0.1.0 across dev rebuilds), and it has no "_apt" download sandbox,
# so installing a locally-built package from any directory is warning-free.
# Runtime deps are already present (building the plugin requires them), so
# apt's dependency resolution is not needed here.
install: deb
	$(SUDO) dpkg -i ./$(DEB_FILE)
	-xfce4-panel -r

uninstall:
	$(SUDO) apt remove -y $(DEB_PKGNAME)

# Distro-agnostic install for systems without dpkg. Nothing tracks the files,
# so "uninstall-local" removes exactly the two paths written here. Only the
# copy steps are elevated, so the build itself stays owned by your user.
install-local: check-not-root all
	$(SUDO) install -d $(DESTDIR)$(PLUGIN_DIR)
	$(SUDO) install -m 0755 $(MODULE) $(DESTDIR)$(PLUGIN_DIR)/$(MODULE)
	$(SUDO) install -d $(DESTDIR)$(DESKTOP_DIR)
	$(SUDO) install -m 0644 $(DESKTOP) $(DESTDIR)$(DESKTOP_DIR)/$(PACKAGE).desktop
	$(SUDO) install -d $(DESTDIR)$(DOC_DIR)
	$(SUDO) install -m 0644 LICENSE $(DESTDIR)$(DOC_DIR)/copyright
	-xfce4-panel -r

uninstall-local:
	$(SUDO) rm -f $(DESTDIR)$(PLUGIN_DIR)/$(MODULE)
	$(SUDO) rm -f $(DESTDIR)$(DESKTOP_DIR)/$(PACKAGE).desktop
	$(SUDO) rm -rf $(DESTDIR)$(DOC_DIR)
	-xfce4-panel -r

# Build a .deb by staging the files into a temporary tree and wrapping it with a
# generated control file. Runtime Depends are detected from the module's
# actually-linked libraries, so the correct package names (incl. Ubuntu's t64
# variants) are used on whatever machine builds it.
deb: check-not-root check-dpkg all
	rm -rf $(DEB_STAGE) 2>/dev/null || $(SUDO) rm -rf $(DEB_STAGE)
	rm -f $(DEB_FILE)
	$(MAKE) stage DESTDIR=$(DEB_STAGE) PREFIX=/usr
	mkdir -p $(DEB_STAGE)/DEBIAN
	deps=$$(objdump -p $(MODULE) | awk '/NEEDED/ {print $$2}' | while read -r so; do \
	          path=$$(ldd $(MODULE) | awk -v s="$$so" '$$1==s {print $$3}'); \
	          [ -n "$$path" ] && dpkg -S "$$(readlink -f "$$path")" 2>/dev/null | cut -d: -f1; \
	        done | sort -u | paste -sd, -); \
	printf 'Package: %s\nVersion: %s\nArchitecture: %s\nMaintainer: %s\nSection: xfce\nPriority: optional\nDepends: %s\nDescription: System monitor plugin for the Xfce panel\n Shows CPU, memory, swap, GPU memory and network usage as transparent square\n history graphs on the Xfce panel, with a configurable click action.\n' \
	    "$(DEB_PKGNAME)" "$(VERSION)" "$(DEB_ARCH)" "$(MAINTAINER)" "$$deps" \
	    > $(DEB_STAGE)/DEBIAN/control
	dpkg-deb --root-owner-group --build $(DEB_STAGE) $(DEB_FILE)
	@echo "Built $(DEB_FILE)"

# Remove the saved settings for every system-monitor instance so the plugin
# starts from the compiled-in defaults again. Run as your normal user (NOT
# via sudo, or it will look in root's home).
#
# Order matters: the running panel rewrites the .rc with its *current* colours
# when it shuts down, so we must fully quit it, THEN delete the file, THEN
# start it again. A plain "xfce4-panel -r" would save the old colours back
# after we deleted the file.
reset-config:
	-xfce4-panel --quit >/dev/null 2>&1
	@sleep 1
	rm -f $(PANEL_CONFIG_DIR)/$(PACKAGE)-*.rc
	@echo "Cleared saved $(PACKAGE) settings in $(PANEL_CONFIG_DIR)."
	@echo "Starting panel with default settings..."
	@sh -c 'nohup xfce4-panel >/dev/null 2>&1 &'

# Convenience for development: (re)install the package, then wipe the saved
# config so the new defaults take effect. "install" self-elevates for apt, so
# run this as your normal user (not via sudo) to keep config paths correct.
reinstall:
	$(MAKE) install
	$(MAKE) reset-config

clean:
	rm -f $(OBJECTS) $(MODULE) $(DESKTOP) $(DEB_FILE)
	rm -rf build 2>/dev/null || $(SUDO) rm -rf build

.PHONY: all check-not-root check-dpkg stage install uninstall install-local \
        uninstall-local deb clean reset-config reinstall

PREFIX ?= /usr
DESTDIR ?=

CC := gcc
CFLAGS := -Wall -Wextra -O2 -D_GNU_SOURCE -DENUXDM_PREFIX="\"$(PREFIX)\""
LDFLAGS := -lpam

OBJDIR := obj
SRC := src
DMN_OBJS := $(OBJDIR)/auth.o $(OBJDIR)/ipc.o $(OBJDIR)/session.o $(OBJDIR)/main.o
LAUNCH_OBJS := $(OBJDIR)/launch.o $(OBJDIR)/auth.o $(OBJDIR)/session.o

.PHONY: all clean install uninstall

all: enuxdm enuxdm-launch

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: $(SRC)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

enuxdm: $(DMN_OBJS)
	$(CC) -o $@ $(DMN_OBJS) $(LDFLAGS)

enuxdm-launch: $(LAUNCH_OBJS)
	$(CC) -o $@ $(LAUNCH_OBJS) $(LDFLAGS)

clean:
	rm -rf $(OBJDIR) enuxdm enuxdm-launch

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)$(PREFIX)/lib/enuxdm
	install -d $(DESTDIR)$(PREFIX)/share/enuxdm/greeter
	install -d $(DESTDIR)$(PREFIX)/share/enuxdm/assets
	install -d $(DESTDIR)$(PREFIX)/share/fonts/enuxdm
	install -d $(DESTDIR)/etc/pam.d
	install -d $(DESTDIR)/etc/enuxdm
	install -d $(DESTDIR)/lib/systemd/system
	install -m755 enuxdm $(DESTDIR)$(PREFIX)/bin/enuxdm
	install -m755 enuxdm-launch $(DESTDIR)$(PREFIX)/lib/enuxdm/enuxdm-launch
	install -m755 $(SRC)/greeter/xinitrc $(DESTDIR)$(PREFIX)/lib/enuxdm/xinitrc
	install -m644 $(SRC)/greeter/greeter.py $(DESTDIR)$(PREFIX)/share/enuxdm/greeter/greeter.py
	install -m644 $(SRC)/greeter/style.css $(DESTDIR)$(PREFIX)/share/enuxdm/greeter/style.css
	install -m644 assets/enux-logo.svg $(DESTDIR)$(PREFIX)/share/enuxdm/assets/enux-logo.svg
	install -m644 pam.d/enuxdm $(DESTDIR)/etc/pam.d/enuxdm
	install -m644 systemd/enuxdm.service $(DESTDIR)/lib/systemd/system/enuxdm.service
	test -f $(DESTDIR)/etc/enuxdm/enuxdm.conf || install -m644 etc/enuxdm/enuxdm.conf $(DESTDIR)/etc/enuxdm/enuxdm.conf
	@if test -f assets/fonts/Inter-Regular.ttf; then \
		install -m644 assets/fonts/Inter-*.ttf $(DESTDIR)$(PREFIX)/share/fonts/enuxdm/ 2>/dev/null || true; \
	fi
	systemctl daemon-reload 2>/dev/null || true
	systemctl enable enuxdm.service 2>/dev/null || true

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/enuxdm
	rm -f $(DESTDIR)$(PREFIX)/lib/enuxdm/enuxdm-launch
	rm -f $(DESTDIR)$(PREFIX)/lib/enuxdm/xinitrc
	rm -rf $(DESTDIR)$(PREFIX)/share/enuxdm
	rm -f $(DESTDIR)/etc/pam.d/enuxdm
	rm -f $(DESTDIR)/lib/systemd/system/enuxdm.service
	systemctl disable enuxdm.service 2>/dev/null || true
	systemctl daemon-reload 2>/dev/null || true

# config
-include Make.config
include mk/Variables.mk

# add our flags + libs
CFLAGS	+= -DVERSION='"$(VERSION)"' -DLIB='"$(LIB)"'
CFLAGS	+= -Wno-pointer-sign

# valgrind options
VFLAGS	:= --leak-check=full --show-possibly-lost=no 

# build
TARGETS	:= vconsole vpublish

all: build

#################################################################
# poor man's autoconf ;-)

include mk/Autoconf.mk

define make-config
LIB		:= $(LIB)
endef

pkgs_vconsole := glib-2.0 gthread-2.0 gtk+-3.0 vte-2.91 libvirt
pkgs_vpublish := glib-2.0 gthread-2.0 libvirt libxml-2.0 avahi-client avahi-glib
HAVE_DEPS := $(shell pkg-config $(pkgs_vconsole) $(pkgs_vpublish) && echo yes)

vconsole : pkglst := $(pkgs_vconsole)
vpublish : pkglst := $(pkgs_vpublish)

CFLAGS += $(shell pkg-config --cflags $(pkglst))
LDLIBS += $(shell pkg-config --libs   $(pkglst))

# desktop files
DESKTOP := $(wildcard $(patsubst %,%.desktop,$(TARGETS)))
SERVICE := $(wildcard $(patsubst %,%.service,$(TARGETS)))


########################################################################
# rules

ifneq ($(HAVE_DEPS),yes)

.PHONY: deps
build:
	@echo "Build dependencies missing."
	@echo "  vconsole needs:  $(pkgs_vconsole)"
	@echo "  vpublish needs:  $(pkgs_vpublish)"
	@echo "Please install.  You can try 'make yum' (needs sudo)."
	@echo ""
	@false

yum dnf:
	sudo $@ install $(patsubst %,"pkgconfig(%)",$(pkgs_vconsole) $(pkgs_vpublish))

else

build: $(TARGETS)

endif

install: build
	$(INSTALL_DIR) $(bindir) $(mandir)/man1 $(appdir)
	$(INSTALL_BINARY) $(TARGETS) $(bindir)
	$(INSTALL_DATA) vconsole.1 $(mandir)/man1
	$(INSTALL_DATA) $(DESKTOP) $(appdir)
	$(INSTALL_DIR) $(DESTDIR)/usr/lib/systemd/system
	$(INSTALL_DATA) $(SERVICE) $(DESTDIR)/usr/lib/systemd/system

valgrind: vconsole
	rm -f valgrind.log
	valgrind $(VFLAGS) --log-file=valgrind.log ./vconsole

clean:
	-rm -f *.o *~ $(depfiles)

realclean distclean: clean
	-rm -f Make.config
	-rm -f $(TARGETS) *~ *.bak

#############################################

vconsole: vconsole.o connect.o domain.o libvirt-glib-event.o
vpublish: vpublish.o mdns-publish.o libvirt-glib-event.o

include mk/Compile.mk
include mk/Maintainer.mk
-include $(depfiles)

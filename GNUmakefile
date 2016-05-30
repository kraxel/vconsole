# config
-include Make.config
include mk/Variables.mk

# add our flags + libs
CFLAGS	+= -DVERSION='"$(VERSION)"' -DLIB='"$(LIB)"'
CFLAGS	+= -Wno-pointer-sign
CFLAGS	+= -Wno-strict-prototypes
CFLAGS	+= -Wno-deprecated-declarations

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
HAVE_VTE291	:= $(call ac_pkg_config,vte-2.91)
endef

ifeq ($(HAVE_VTE291),yes)
pkgvte := vte-2.91
else
pkgvte := vte-2.90
endif
pkgs_vconsole := glib-2.0 gthread-2.0 gtk+-3.0 $(pkgvte) libvirt
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
	$(INSTALL_DATA) vconsole.man $(mandir)/man1/vconsole.1
	$(INSTALL_DATA) $(DESKTOP) $(appdir)
	$(INSTALL_DATA) $(SERVICE) /usr/lib/systemd/system

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

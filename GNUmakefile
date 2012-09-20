# config
-include Make.config
include mk/Variables.mk

# add our flags + libs
CFLAGS	+= -DVERSION='"$(VERSION)"' -DLIB='"$(LIB)"'

# valgrind options
VFLAGS	:= --leak-check=full --show-possibly-lost=no 

# build
TARGETS		:= vconsole

# default target
all: build

#################################################################
# poor man's autoconf ;-)

include mk/Autoconf.mk

define make-config
LIB		:= $(LIB)
HAVE_GLIB	:= $(call ac_pkg_config,glib-2.0)
HAVE_GTHREAD	:= $(call ac_pkg_config,gthread-2.0)
HAVE_GTK2	:= $(call ac_pkg_config,gtk+-2.0)
HAVE_VTE2	:= $(call ac_pkg_config,vte)
HAVE_GTK3	:= $(call ac_pkg_config,gtk+-3.0)
HAVE_VTE3	:= $(call ac_pkg_config,vte-2.90)
HAVE_LIBVIRT	:= $(call ac_pkg_config,libvirt)
endef

ifeq ($(HAVE_GTK3)-$(HAVE_VTE3),yes-yes)
CFLAGS += -Wno-deprecated-declarations
wanted := $(HAVE_GLIB)-$(HAVE_GTHREAD)-$(HAVE_GTK3)-$(HAVE_VTE3)-$(HAVE_LIBVIRT)
pkglst := glib-2.0 gthread-2.0 gtk+-3.0 vte-2.90 libvirt
else
wanted := $(HAVE_GLIB)-$(HAVE_GTHREAD)-$(HAVE_GTK2)-$(HAVE_VTE2)-$(HAVE_LIBVIRT)
pkglst := glib-2.0 gthread-2.0 gtk+-2.0 vte libvirt
endif

CFLAGS += -Wno-strict-prototypes
CFLAGS += $(shell test "$(pkglst)" != "" && pkg-config --cflags $(pkglst))
LDLIBS += $(shell test "$(pkglst)" != "" && pkg-config --libs   $(pkglst))

# desktop files
DESKTOP := $(wildcard $(patsubst %,%.desktop,$(TARGETS)))


########################################################################
# rules

ifeq ($(wanted),yes-yes-yes-yes-yes)
build: $(TARGETS)
else
build:
	@echo "build dependencies are missing"
	@false
endif

install: build
	$(INSTALL_DIR) $(bindir) $(mandir)/man1 $(appdir)
	$(INSTALL_BINARY) $(TARGETS) $(bindir)
	$(INSTALL_DATA) vconsole.man $(mandir)/man1/vconsole.1
	$(INSTALL_DATA) $(DESKTOP) $(appdir)

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

include mk/Compile.mk
include mk/Maintainer.mk
-include $(depfiles)

libraries += libmain

libmain_NAME = libnixmain

libmain_DIR := $(d)

libmain_SOURCES := $(wildcard $(d)/*.cc)

libmain_CXXFLAGS += -I src/libutil -I src/libstore

libmain_LDFLAGS += $(OPENSSL_LIBS)

libmain_LIBS = libstore libutil

libmain_ALLOW_UNDEFINED = 1

$(eval $(call install-file-in, $(buildprefix)$(d)/nix-main.pc, $(libdir)/pkgconfig, 0644))

$(foreach i, $(wildcard $(d)/*.hh), \
  $(eval $(call install-file-in, $(i), $(includedir)/nix, 0644)))

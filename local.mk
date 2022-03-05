clean-files += Makefile.config

GLOBAL_CXXFLAGS += -Wno-deprecated-declarations

$(eval $(call install-file-in, config.h, $(includedir)/nix, 0644))

$(GCH): src/libutil/util.hh config.h

GCH_CXXFLAGS = -I src/libutil

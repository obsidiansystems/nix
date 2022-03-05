programs += nix

nix_DIR := $(d)


nix_SOURCES := \
  $(wildcard $(d)/store/*.cc) \
  $(wildcard src/build-remote/*.cc) \
  $(wildcard src/nix-daemon/*.cc) \
  $(wildcard src/nix-store/*.cc)

nix_CXXFLAGS += -I src/libutil -I src/libstore -I src/libmain -I src/libstore-cmd -I doc/manual

nix_LIBS = libmain libstore libutil libstore-cmd

nix_LDFLAGS = -pthread $(SODIUM_LIBS) $(EDITLINE_LIBS) $(BOOST_LDFLAGS) -llowdown

$(foreach name, \
  nix-daemon nix-store, \
  $(eval $(call install-symlink, nix, $(bindir)/$(name))))
$(eval $(call install-symlink, $(bindir)/nix, $(libexecdir)/nix/build-remote))

ifeq ($(NIX_FULL), 1)

nix_SOURCES += \
  $(wildcard $(d)/*.cc) \
  $(wildcard src/nix-build/*.cc) \
  $(wildcard src/nix-channel/*.cc) \
  $(wildcard src/nix-collect-garbage/*.cc) \
  $(wildcard src/nix-copy-closure/*.cc) \
  $(wildcard src/nix-env/*.cc) \
  $(wildcard src/nix-instantiate/*.cc)

nix_CXXFLAGS += -I src/libfetchers -I src/libexpr -I src/libcmd

nix_LIBS += libexpr libfetchers libcmd

$(foreach name, \
  nix-build nix-channel nix-collect-garbage nix-copy-closure nix-env nix-hash nix-instantiate nix-prefetch-url nix-shell, \
  $(eval $(call install-symlink, nix, $(bindir)/$(name))))

src/nix-env/user-env.cc: src/nix-env/buildenv.nix.gen.hh

src/nix/develop.cc: src/nix/get-env.sh.gen.hh

src/nix-channel/nix-channel.cc: src/nix-channel/unpack-channel.nix.gen.hh

src/nix/main.cc: doc/manual/generate-manpage.nix.gen.hh doc/manual/utils.nix.gen.hh

endif

programs += mock-daemon

mock-daemon_DIR := $(d)

# do not install
mock-daemon_INSTALL_DIR :=

mock-daemon_SOURCES := \
  $(wildcard $(d)/*.cc) \

mock-daemon_CXXFLAGS += -I src/libutil

mock-daemon_LIBS = libutil

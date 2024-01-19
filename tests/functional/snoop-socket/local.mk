programs += snoop-socket

snoop-socket_DIR := $(d)

# do not install
snoop-socket_INSTALL_DIR :=

snoop-socket_SOURCES := \
  $(wildcard $(d)/*.cc) \

snoop-socket_CXXFLAGS += -I src/libutil

snoop-socket_LIBS = libutil

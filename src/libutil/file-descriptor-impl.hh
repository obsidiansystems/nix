#pragma once
/**
 * @file
 * @brief Internal implementation details for file-descriptor.cc
 *
 * This header is not installed and should only be included by
 * file-descriptor.cc and platform-specific variants.
 */

#include "nix/util/file-descriptor.hh"

namespace nix {

/**
 * Portable fallback for sendFile using readOffset + writeFull.
 */
size_t sendFileFallback(Descriptor out_fd, Descriptor in_fd, off_t offset, size_t count);

/**
 * Portable fallback for splice using read + writeFull.
 */
size_t spliceFallback(Descriptor from, Descriptor to);

} // namespace nix

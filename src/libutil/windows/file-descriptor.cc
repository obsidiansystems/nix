#include "file-descriptor-impl.hh"
#include "nix/util/file-system.hh"
#include "nix/util/signals.hh"
#include "nix/util/finally.hh"
#include "nix/util/serialise.hh"
#include "nix/util/socket.hh"

#include <span>

#include <fileapi.h>
#include <error.h>
#include <namedpipeapi.h>
#include <namedpipeapi.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mswsock.h>

namespace nix {

using namespace nix::windows;

std::make_unsigned_t<off_t> getFileSize(Descriptor fd)
{
    LARGE_INTEGER li;
    if (!GetFileSizeEx(fd, &li)) {
        auto lastError = GetLastError();
        throw WinError(lastError, "getting size of file %s", PathFmt(descriptorToPath(fd)));
    }
    return li.QuadPart;
}

size_t read(Descriptor fd, std::span<std::byte> buffer)
{
    checkInterrupt(); // For consistency with unix, and its EINTR loop
    DWORD n;
    if (!ReadFile(fd, buffer.data(), static_cast<DWORD>(buffer.size()), &n, NULL)) {
        auto lastError = GetLastError();
        if (lastError == ERROR_BROKEN_PIPE)
            n = 0; // Treat as EOF
        else
            throw WinError(lastError, "reading %1% bytes from %2%", buffer.size(), PathFmt(descriptorToPath(fd)));
    }
    return static_cast<size_t>(n);
}

size_t readOffset(Descriptor fd, off_t offset, std::span<std::byte> buffer)
{
    checkInterrupt(); // For consistency with unix, and its EINTR loop
    OVERLAPPED ov = {};
    ov.Offset = static_cast<DWORD>(offset);
    if constexpr (sizeof(offset) > 4) /* We don't build with 32 bit off_t, but let's be safe. */
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD n;
    if (!ReadFile(fd, buffer.data(), static_cast<DWORD>(buffer.size()), &n, &ov)) {
        auto lastError = GetLastError();
        throw WinError(
            lastError,
            "reading %1% bytes at offset %2% from %3%",
            buffer.size(),
            offset,
            PathFmt(descriptorToPath(fd)));
    }
    return static_cast<size_t>(n);
}

size_t write(Descriptor fd, std::span<const std::byte> buffer)
{
    checkInterrupt(); // For consistency with unix
    DWORD n;
    if (!WriteFile(fd, buffer.data(), static_cast<DWORD>(buffer.size()), &n, NULL)) {
        auto lastError = GetLastError();
        throw WinError(lastError, "writing %1% bytes to %2%", buffer.size(), PathFmt(descriptorToPath(fd)));
    }
    return static_cast<size_t>(n);
}

namespace {

/**
 * Check if a TransmitFile error is recoverable (non-socket destination).
 * @return true if we should fall back to read/write, false if we should throw
 */
bool isTransmitFileRecoverable(DWORD lastError)
{
    return lastError == WSAENOTSOCK || lastError == WSAEOPNOTSUPP;
}

} // namespace

size_t sendFile(Descriptor out_fd, Descriptor in_fd, off_t offset, size_t count)
{
    OVERLAPPED ov = {};
    ov.Offset = static_cast<DWORD>(offset);
    if constexpr (sizeof(offset) > 4)
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);

    // Try TransmitFile (requires out_fd to be a socket)
    if (TransmitFile(toSocket(out_fd), in_fd, static_cast<DWORD>(count), 0, &ov, nullptr, 0)) {
        return count;
    }

    auto lastError = GetLastError();
    if (!isTransmitFileRecoverable(lastError)) {
        throw WinError(lastError, "TransmitFile failed");
    }

    return sendFileFallback(out_fd, in_fd, offset, count);
}

size_t splice(Descriptor from, Descriptor to)
{
    // Try to use TransmitFile for seekable files
    off_t pos = lseek(from, 0, SEEK_CUR);
    if (pos != -1) {
        try {
            auto size = getFileSize(from);
            if (static_cast<std::make_unsigned_t<off_t>>(pos) >= size)
                return 0; // EOF

            auto remaining = size - pos;
            // Limit to a reasonable chunk size for consistent behavior
            auto toTransfer = std::min<size_t>(remaining, 64 * 1024);

            OVERLAPPED ov = {};
            ov.Offset = static_cast<DWORD>(pos);
            if constexpr (sizeof(pos) > 4)
                ov.OffsetHigh = static_cast<DWORD>(pos >> 32);

            if (TransmitFile(toSocket(to), from, static_cast<DWORD>(toTransfer), 0, &ov, nullptr, 0)) {
                // TransmitFile with OVERLAPPED doesn't update file position
                lseek(from, pos + toTransfer, SEEK_SET);
                return toTransfer;
            }

            auto lastError = GetLastError();
            if (!isTransmitFileRecoverable(lastError)) {
                throw WinError(lastError, "TransmitFile failed");
            }
            // Fall through to fallback for non-socket destinations
        } catch (WinError &) {
            // getFileSize failed (e.g., not a regular file), fall through
        }
    }

    // Fallback for pipes or non-socket destinations
    return spliceFallback(from, to);
}

//////////////////////////////////////////////////////////////////////

void Pipe::create()
{
    SECURITY_ATTRIBUTES saAttr = {0};
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.lpSecurityDescriptor = NULL;
    saAttr.bInheritHandle = TRUE;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &saAttr, 0))
        throw WinError("CreatePipe");

    readSide = hReadPipe;
    writeSide = hWritePipe;
}

//////////////////////////////////////////////////////////////////////

off_t lseek(HANDLE h, off_t offset, int whence)
{
    DWORD method;
    switch (whence) {
    case SEEK_SET:
        method = FILE_BEGIN;
        break;
    case SEEK_CUR:
        method = FILE_CURRENT;
        break;
    case SEEK_END:
        method = FILE_END;
        break;
    default:
        throw Error("lseek: invalid whence %d", whence);
    }

    LARGE_INTEGER li;
    li.QuadPart = offset;
    LARGE_INTEGER newPos;

    if (!SetFilePointerEx(h, li, &newPos, method)) {
        /* Convert to a POSIX error, since caller code works with this as if it were
           a POSIX lseek. */
        errno = std::error_code(GetLastError(), std::system_category()).default_error_condition().value();
        return -1;
    }

    return newPos.QuadPart;
}

void syncDescriptor(Descriptor fd)
{
    if (!::FlushFileBuffers(fd)) {
        auto lastError = GetLastError();
        throw WinError(lastError, "flushing file %s", PathFmt(descriptorToPath(fd)));
    }
}

} // namespace nix

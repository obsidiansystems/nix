#pragma once
///@file

#include <string_view>
#include <span>
#include <vector>

namespace nix {

/**
 * Arbitrary bytes, owned.
 */
using Bytes = std::vector<std::byte>;

/**
 * Arbitrary bytes, a mere view.
 */
using BytesView = std::span<const std::byte>;

/**
 * Arbitrary bytes, a mutable view.
 */
using MutableBytesView = std::span<std::byte>;

inline BytesView as_bytes(std::string_view sv) noexcept
{
    return BytesView{
        reinterpret_cast<const std::byte *>(sv.data()),
        sv.size(),
    };
}

constexpr Bytes to_owned(BytesView bytes)
{
    return Bytes{
        bytes.begin(),
        bytes.end(),
    };
}

/**
 * @note this should be avoided, as arbitrary binary data in strings
 * views, while allowed, is not really proper.
 *
 * Generally this should only be used as a stop-gap with other
 * definitions that themselves should be converted to accept `BytesView`
 * or similar, directly.
 */
inline std::string_view as_str(BytesView sp)
{
    return std::string_view{
        reinterpret_cast<const char *>(sp.data()),
        sp.size(),
    };
}

/**
 * Comparator that enables looking up with `BytesView` without creating
 * temporary `Bytes` objects.
 */
struct BytesCompare
{
    using is_transparent = void;

    bool operator()(const Bytes & lhs, const Bytes & rhs) const
    {
        return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
    }

    bool operator()(const Bytes & lhs, BytesView rhs) const
    {
        return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
    }

    bool operator()(BytesView lhs, const Bytes & rhs) const
    {
        return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
    }
};

} // namespace nix

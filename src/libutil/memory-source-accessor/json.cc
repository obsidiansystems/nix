#include "nix/util/memory-source-accessor.hh"
#include "nix/util/nar-accessor.hh"
#include "nix/util/json-utils.hh"
#include "nix/util/base-n.hh"

#include <nlohmann/json.hpp>

namespace nlohmann {

using namespace nix;

enum struct FsoJsonFormat {
    Raw,
    Base64,
};

template<>
struct adl_serializer<FsoJsonFormat>
{
    static FsoJsonFormat from_json(const json & json)
    {
        auto str = getString(json);
        if (str == "raw") {
            return FsoJsonFormat::Raw;
        } else if (str == "base64") {
            return FsoJsonFormat::Base64;
        } else {
            throw Error("unknown format '%s'", str);
        }
    }

    static void to_json(json & json, FsoJsonFormat format)
    {
        switch (format) {
        case FsoJsonFormat::Raw:
            json = "raw";
            break;
        case FsoJsonFormat::Base64:
            json = "base64";
            break;
        }
    }
};

/**
 * Check if bytes contain valid UTF-8.
 * Uses nlohmann/json's validation by attempting strict serialization.
 *
 * @return true if valid UTF-8, false otherwise
 */
static bool isValidUtf8(BytesView data)
{
    auto str = as_str(data);
    try {
        // Try to create and serialize a JSON string with strict UTF-8 validation
        json test = str;
        test.dump(-1, ' ', false, json::error_handler_t::strict);
        return true;
    } catch (...) {
        return false;
    }
}

static Bytes decodeWithFormat(std::string_view data, FsoJsonFormat format)
{
    switch (format) {
    case FsoJsonFormat::Raw:
        return to_owned(as_bytes(data));
    case FsoJsonFormat::Base64:
        return base64::decode(data);
    }
}

static std::string encodeWithFormat(BytesView data, FsoJsonFormat format)
{
    switch (format) {
    case FsoJsonFormat::Raw:
        return std::string(as_str(data));
    case FsoJsonFormat::Base64:
        return base64::encode(data);
    }
}

static std::pair<std::string, FsoJsonFormat> encodeWithFormat(BytesView data)
{
    auto format = isValidUtf8(data) ? FsoJsonFormat::Raw : FsoJsonFormat::Base64;
    return {encodeWithFormat(data, format), format};
}

// fso::Regular<RegularContents>
template<>
MemorySourceAccessor::File::Regular adl_serializer<MemorySourceAccessor::File::Regular>::from_json(const json & json)
{
    auto & obj = getObject(json);
    return MemorySourceAccessor::File::Regular{
        .executable = getBoolean(valueAt(obj, "executable")),
        .contents = decodeWithFormat(getString(valueAt(obj, "contents")), valueAt(obj, "format")),
    };
}

template<>
void adl_serializer<MemorySourceAccessor::File::Regular>::to_json(
    json & json, const MemorySourceAccessor::File::Regular & r)
{
    auto [contents, format] = encodeWithFormat(r.contents);
    json = {
        {"executable", r.executable},
        {"contents", std::move(contents)},
        {"format", format},
    };
}

template<>
NarListing::Regular adl_serializer<NarListing::Regular>::from_json(const json & json)
{
    auto & obj = getObject(json);
    auto * execPtr = optionalValueAt(obj, "executable");
    auto * sizePtr = optionalValueAt(obj, "size");
    auto * offsetPtr = optionalValueAt(obj, "narOffset");
    return NarListing::Regular{
        .executable = execPtr ? getBoolean(*execPtr) : false,
        .contents{
            .fileSize = ptrToOwned<uint64_t>(sizePtr),
            .narOffset = ptrToOwned<uint64_t>(offsetPtr).and_then(
                [](auto v) { return v != 0 ? std::optional{v} : std::nullopt; }),
        },
    };
}

template<>
void adl_serializer<NarListing::Regular>::to_json(json & j, const NarListing::Regular & r)
{
    if (r.contents.fileSize)
        j["size"] = *r.contents.fileSize;
    if (r.executable)
        j["executable"] = true;
    if (r.contents.narOffset)
        j["narOffset"] = *r.contents.narOffset;
}

template<typename Child>
void adl_serializer<fso::DirectoryT<Child>>::to_json(json & j, const fso::DirectoryT<Child> & d)
{
    /* Can only use raw names if every entry in this directory is valid UTF-8. */
    auto format =
        std::ranges::all_of(d.entries | std::views::keys, isValidUtf8) ? FsoJsonFormat::Raw : FsoJsonFormat::Base64;

    j = {
        {
            "entries",
            [&]() {
                json::object_t entries;
                for (const auto & [name, child] : d.entries) {
                    entries[encodeWithFormat(name, format)] = child;
                }
                return entries;
            }(),
        },
        {
            "format",
            format,
        },
    };
}

template<typename Child>
fso::DirectoryT<Child> adl_serializer<fso::DirectoryT<Child>>::from_json(const json & json)
{
    auto & obj = getObject(json);
    auto & contentsObj = getObject(valueAt(obj, "entries"));
    FsoJsonFormat format = valueAt(obj, "format");

    fso::DirectoryT<Child> result;

    for (const auto & [key, value] : contentsObj) {
        auto decoded = decodeWithFormat(key, format);
        result.entries.insert_or_assign(decoded, value.template get<Child>());
    }

    return result;
}

// fso::Symlink
fso::Symlink adl_serializer<fso::Symlink>::from_json(const json & json)
{
    auto & obj = getObject(json);
    return fso::Symlink{
        .target = decodeWithFormat(getString(valueAt(obj, "target")), valueAt(obj, "format")),
    };
}

void adl_serializer<fso::Symlink>::to_json(json & json, const fso::Symlink & s)
{
    auto [target, format] = encodeWithFormat(s.target);
    json = {
        {"target", std::move(target)},
        {"format", format},
    };
}

// fso::Opaque
fso::Opaque adl_serializer<fso::Opaque>::from_json(const json &)
{
    return fso::Opaque{};
}

void adl_serializer<fso::Opaque>::to_json(json & j, const fso::Opaque &)
{
    j = nlohmann::json::object();
}

// fso::VariantT<RegularContents, recur> - generic implementation
template<typename RegularContents, bool recur>
void adl_serializer<fso::VariantT<RegularContents, recur>>::to_json(
    json & j, const fso::VariantT<RegularContents, recur> & val)
{
    using Variant = fso::VariantT<RegularContents, recur>;
    j = nlohmann::json::object();
    std::visit(
        overloaded{
            [&](const typename Variant::Regular & r) {
                j = r;
                j["type"] = "regular";
            },
            [&](const typename Variant::Directory & d) {
                j = d;
                j["type"] = "directory";
            },
            [&](const typename Variant::Symlink & s) {
                j = s;
                j["type"] = "symlink";
            },
        },
        val.raw);
}

template<typename RegularContents, bool recur>
fso::VariantT<RegularContents, recur>
adl_serializer<fso::VariantT<RegularContents, recur>>::from_json(const json & json)
{
    using Variant = fso::VariantT<RegularContents, recur>;
    auto & obj = getObject(json);
    auto type = getString(valueAt(obj, "type"));
    if (type == "regular")
        return static_cast<typename Variant::Regular>(json);
    if (type == "directory")
        return static_cast<typename Variant::Directory>(json);
    if (type == "symlink")
        return static_cast<typename Variant::Symlink>(json);
    else
        throw Error("unknown type of file '%s'", type);
}

// Explicit instantiations for VariantT types we use
template struct adl_serializer<MemorySourceAccessor::File>;
template struct adl_serializer<NarListing>;
template struct adl_serializer<ShallowNarListing>;

// MemorySourceAccessor
MemorySourceAccessor adl_serializer<MemorySourceAccessor>::from_json(const json & json)
{
    MemorySourceAccessor res;
    res.root = json;
    return res;
}

void adl_serializer<MemorySourceAccessor>::to_json(json & json, const MemorySourceAccessor & val)
{
    json = val.root;
}

} // namespace nlohmann

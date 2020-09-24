#include <cstring>
#include <nlohmann/json.hpp>

#include "ipfs-binary-cache-store.hh"
#include "filetransfer.hh"
#include "nar-info-disk-cache.hh"
#include "archive.hh"
#include "compression.hh"
#include "git.hh"
#include "names.hh"
#include "references.hh"
#include "callback.hh"

namespace nix {

IPFSBinaryCacheStore::IPFSBinaryCacheStore(
   const std::string & scheme, const std::string & uri, const Params & params)
        : StoreConfig(params)
        , Store(params)
        , cacheScheme(scheme)
        , cacheUri(uri)
{
    auto state(_state.lock());

    if (secretKeyFile != "")
        secretKey = std::unique_ptr<SecretKey>(new SecretKey(readFile(secretKeyFile)));

    StringSink sink;
    sink << narVersionMagic1;
    narMagic = *sink.s;

    if (cacheUri.back() == '/')
        cacheUri.pop_back();

    if (cacheScheme == "ipfs" && cacheUri == "") {
        trustless = true;
        allowModify = true;
    } else if (cacheScheme == "ipfs") {
        initialIpfsPath = "/ipfs/" + cacheUri;
        state->ipfsPath = initialIpfsPath;
        allowModify = get(params, "allow-modify").value_or("") == "true";
    } else if (cacheScheme == "ipns") {
        ipnsPath = "/ipns/" + cacheUri;

        // TODO: we should try to determine if we are able to modify
        // this ipns
        allowModify = true;
    } else
        throw Error("unknown IPNS URI '%s'", getUri());

    std::string ipfsAPIHost(get(params, "host").value_or("127.0.0.1"));
    std::string ipfsAPIPort(get(params, "port").value_or("5001"));
    daemonUri = "http://" + ipfsAPIHost + ":" + ipfsAPIPort;

    // Check the IPFS daemon is running
    FileTransferRequest request(daemonUri + "/api/v0/version");
    request.post = true;
    request.tries = 1;
    auto res = getFileTransfer()->download(request);
    auto versionInfo = nlohmann::json::parse(*res.data);
    if (versionInfo.find("Version") == versionInfo.end())
        throw Error("daemon for IPFS is not running properly");

    if (compareVersions(versionInfo["Version"], "0.4.0") < 0)
        throw Error("daemon for IPFS is %s, when a minimum of 0.4.0 is required", versionInfo["Version"]);

    // Resolve the IPNS name to an IPFS object
    if (ipnsPath) {
        initialIpfsPath = resolveIPNSName(*ipnsPath);
        state->ipfsPath = initialIpfsPath;
    }

    if (trustless)
        return;

    auto json = getIpfsDag(state->ipfsPath);

    // Verify StoreDir is correct
    if (json.find("StoreDir") == json.end()) {
        json["StoreDir"] = storeDir;
        state->ipfsPath = putIpfsDag(json);
    } else if (json["StoreDir"] != storeDir)
        throw Error("binary cache '%s' is for Nix stores with prefix '%s', not '%s'",
            getUri(), json["StoreDir"], storeDir);

    if (json.find("WantMassQuery") != json.end())
        wantMassQuery.setDefault(json["WantMassQuery"] ? "true" : "false");

    if (json.find("Priority") != json.end())
        priority.setDefault(fmt("%d", json["Priority"]));
}

std::string IPFSBinaryCacheStore::putIpfsDag(nlohmann::json data, std::optional<std::string> hash)
{
    auto uri(daemonUri + "/api/v0/dag/put");
    if (hash)
        uri += "?hash=" + *hash;
    auto req = FileTransferRequest(uri);
    req.data = std::make_shared<string>(data.dump());
    req.post = true;
    req.tries = 1;
    auto res = getFileTransfer()->upload(req);
    auto json = nlohmann::json::parse(*res.data);
    return "/ipfs/" + (std::string) json["Cid"]["/"];
}

nlohmann::json IPFSBinaryCacheStore::getIpfsDag(std::string objectPath)
{
    auto req = FileTransferRequest(daemonUri + "/api/v0/dag/get?arg=" + objectPath);
    req.post = true;
    req.tries = 1;
    auto res = getFileTransfer()->download(req);
    auto json = nlohmann::json::parse(*res.data);
    return json;
}

// Given a ipns path, checks if it corresponds to a DNSLink path, and in
// case returns the domain
std::optional<string> IPFSBinaryCacheStore::isDNSLinkPath(std::string path)
{
    if (path.find("/ipns/") != 0)
        throw Error("path '%s' is not an ipns path", path);
    auto subpath = std::string(path, 6);
    if (subpath.find(".") != std::string::npos) {
        return subpath;
    }
    return std::nullopt;
}

bool IPFSBinaryCacheStore::ipfsObjectExists(const std::string ipfsPath)
{
    auto uri = daemonUri + "/api/v0/object/stat?arg=" + getFileTransfer()->urlEncode(ipfsPath);

    FileTransferRequest request(uri);
    request.post = true;
    request.tries = 1;
    try {
        auto res = getFileTransfer()->download(request);
        auto json = nlohmann::json::parse(*res.data);

        return json.find("Hash") != json.end();
    } catch (FileTransferError & e) {
        // probably should verify this is a not found error but
        // ipfs gives us a 500
        return false;
    }
}

std::optional<uint64_t> IPFSBinaryCacheStore::ipfsBlockStat(std::string ipfsPath)
{
    auto uri = daemonUri + "/api/v0/block/stat?arg=" + getFileTransfer()->urlEncode(ipfsPath);

    FileTransferRequest request(uri);
    request.post = true;
    request.tries = 1;
    try {
        auto res = getFileTransfer()->download(request);
        auto json = nlohmann::json::parse(*res.data);

        if (json.find("Size") != json.end())
            return (uint64_t) json["Size"];
    } catch (FileTransferError & e) {
        // probably should verify this is a not found error but
        // ipfs gives us a 500
    }

    return std::nullopt;
}

// Resolve the IPNS name to an IPFS object
std::string IPFSBinaryCacheStore::resolveIPNSName(std::string ipnsPath) {
    debug("Resolving IPFS object of '%s', this could take a while.", ipnsPath);
    auto uri = daemonUri + "/api/v0/name/resolve?arg=" + getFileTransfer()->urlEncode(ipnsPath);
    FileTransferRequest request(uri);
    request.post = true;
    request.tries = 1;
    auto res = getFileTransfer()->download(request);
    auto json = nlohmann::json::parse(*res.data);
    if (json.find("Path") == json.end())
        throw Error("daemon for IPFS is not running properly");
    return json["Path"];
}

Path IPFSBinaryCacheStore::formatPathAsProtocol(Path path) {
    if (hasPrefix(path, "/ipfs/"))
        return "ipfs://" + path.substr(strlen("/ipfs/"), string::npos);
    else if (hasPrefix(path, "/ipns/"))
        return "ipns://" + path.substr(strlen("/ipfs/"), string::npos);
    else return path;
}

void IPFSBinaryCacheStore::sync()
{
    auto state(_state.lock());

    if (trustless)
        return;

    if (state->ipfsPath == initialIpfsPath)
        return;

    // If we aren't in trustless mode (handled above) and we don't allow
    // modifications, state->ipfsPath should never be changed from the initial
    // one,
    assert(allowModify);

    if (!ipnsPath) {
        warn("created new store at '%s'. The old store at %s is immutable, so we can't update it",
            "ipfs://" + std::string(state->ipfsPath, 6), getUri());
        return;
    }

    auto resolvedIpfsPath = resolveIPNSName(*ipnsPath);
    if (resolvedIpfsPath != initialIpfsPath) {
        throw Error(
            "The IPNS hash or DNS link %s resolves to something different from the value it had when Nix was started:\n"
            "  expected: %s\n"
            "  got %s\n"
            "\n"
            "Perhaps something else updated it in the meantime?",
            *ipnsPath, initialIpfsPath, resolvedIpfsPath);
    }

    if (resolvedIpfsPath == state->ipfsPath) {
        printMsg(lvlInfo, "The hash is already up to date, nothing to do");
        return;
    }

    // Now, we know that paths are not up to date but also not changed due to updates in DNS or IPNS hash.
    auto optDomain = isDNSLinkPath(*ipnsPath);
    if (optDomain) {
        auto domain = *optDomain;
        throw Error("The provided ipns path is a DNSLink, and syncing those is not supported.\n  Current DNSLink: %s\nYou should update your DNS settings"
            , domain);
    }

    debug("Publishing '%s' to '%s', this could take a while.", state->ipfsPath, *ipnsPath);

    auto uri = daemonUri + "/api/v0/name/publish?allow-offline=true";
    uri += "&arg=" + getFileTransfer()->urlEncode(state->ipfsPath);

    // Given the hash, we want to discover the corresponding name in the
    // `ipfs key list` command, so that we publish to the right address in
    // case the user has multiple ones available.

    // NOTE: this is needed for ipfs < 0.5.0 because key must be a
    // name, not an address.

    auto ipnsPathHash = std::string(*ipnsPath, 6);
    debug("Getting the name corresponding to hash %s", ipnsPathHash);

    auto keyListRequest = FileTransferRequest(daemonUri + "/api/v0/key/list/");
    keyListRequest.post = true;
    keyListRequest.tries = 1;

    auto keyListResponse = nlohmann::json::parse(*(getFileTransfer()->download(keyListRequest)).data);

    std::string keyName {""};
    for (auto & key : keyListResponse["Keys"])
        if (key["Id"] == ipnsPathHash)
            keyName = key["Name"];
    if (keyName == "") {
        throw Error("We couldn't find a name corresponding to the provided ipns hash:\n  hash: %s", ipnsPathHash);
    }

    // Now we can append the keyname to our original request
    uri += "&key=" + keyName;

    auto req = FileTransferRequest(uri);
    req.post = true;
    req.tries = 1;
    getFileTransfer()->download(req);
}

void IPFSBinaryCacheStore::addLink(std::string name, std::string ipfsObject)
{
    auto state(_state.lock());

    auto uri = daemonUri + "/api/v0/object/patch/add-link?create=true";
    uri += "&arg=" + getFileTransfer()->urlEncode(state->ipfsPath);
    uri += "&arg=" + getFileTransfer()->urlEncode(name);
    uri += "&arg=" + getFileTransfer()->urlEncode(ipfsObject);

    auto req = FileTransferRequest(uri);
    req.post = true;
    req.tries = 1;
    auto res = getFileTransfer()->download(req);
    auto json = nlohmann::json::parse(*res.data);

    state->ipfsPath = "/ipfs/" + (std::string) json["Hash"];
}

std::string IPFSBinaryCacheStore::addFile(const std::string & data)
{
    // TODO: use callbacks

    auto req = FileTransferRequest(daemonUri + "/api/v0/add");
    req.data = std::make_shared<string>(data);
    req.post = true;
    req.tries = 1;
    auto res = getFileTransfer()->upload(req);
    auto json = nlohmann::json::parse(*res.data);
    return (std::string) json["Hash"];
}

void IPFSBinaryCacheStore::upsertFile(const std::string & path, const std::string & data, const std::string & mimeType)
{
    try {
        addLink(path, "/ipfs/" + addFile(data));
    } catch (FileTransferError & e) {
        // TODO: may wrap the inner error in a better way.
        throw UploadToIPFS("while uploading to IPFS binary cache at '%s': %s", getUri(), e.msg());
    }
}

void IPFSBinaryCacheStore::getFile(const std::string & path,
    Callback<std::shared_ptr<std::string>> callback) noexcept
{
    std::string path_(path);
    if (hasPrefix(path, "ipfs://"))
        path_ = "/ipfs/" + std::string(path, 7);
    getIpfsObject(path_, std::move(callback));
}

void IPFSBinaryCacheStore::getFile(const std::string & path, Sink & sink)
{
    std::promise<std::shared_ptr<std::string>> promise;
    getFile(path,
        {[&](std::future<std::shared_ptr<std::string>> result) {
            try {
                promise.set_value(result.get());
            } catch (...) {
                promise.set_exception(std::current_exception());
            }
        }});
    auto data = promise.get_future().get();
    sink((unsigned char *) data->data(), data->size());
}

std::shared_ptr<std::string> IPFSBinaryCacheStore::getFile(const std::string & path)
{
    StringSink sink;
    try {
        getFile(path, sink);
    } catch (NoSuchBinaryCacheFile &) {
        return nullptr;
    }
    return sink.s;
}

void IPFSBinaryCacheStore::getIpfsObject(const std::string & ipfsPath,
    Callback<std::shared_ptr<std::string>> callback) noexcept
{
    auto uri = daemonUri + "/api/v0/cat?arg=" + getFileTransfer()->urlEncode(ipfsPath);

    FileTransferRequest request(uri);
    request.post = true;
    request.tries = 1;

    auto callbackPtr = std::make_shared<decltype(callback)>(std::move(callback));

    getFileTransfer()->enqueueFileTransfer(request,
        {[callbackPtr](std::future<FileTransferResult> result){
            try {
                (*callbackPtr)(result.get().data);
            } catch (FileTransferError & e) {
                return (*callbackPtr)(std::shared_ptr<std::string>());
            } catch (...) {
                callbackPtr->rethrow();
            }
        }}
    );
}

void IPFSBinaryCacheStore::getIpfsBlock(const std::string & path, Sink & sink)
{
    std::promise<std::shared_ptr<std::string>> promise;
    getIpfsBlock(path,
        {[&](std::future<std::shared_ptr<std::string>> result) {
            try {
                promise.set_value(result.get());
            } catch (...) {
                promise.set_exception(std::current_exception());
            }
        }});
    auto data = promise.get_future().get();
    sink((unsigned char *) data->data(), data->size());
}

std::shared_ptr<std::string> IPFSBinaryCacheStore::getIpfsBlock(const std::string & path)
{
    StringSink sink;
    try {
        getIpfsBlock(path, sink);
    } catch (NoSuchBinaryCacheFile &) {
        return nullptr;
    }
    return sink.s;
}

void IPFSBinaryCacheStore::getIpfsBlock(const std::string & ipfsPath,
    Callback<std::shared_ptr<std::string>> callback) noexcept
{
    auto uri = daemonUri + "/api/v0/block/get?arg=" + getFileTransfer()->urlEncode(ipfsPath);

    FileTransferRequest request(uri);
    request.post = true;
    request.tries = 1;

    auto callbackPtr = std::make_shared<decltype(callback)>(std::move(callback));

    getFileTransfer()->enqueueFileTransfer(request,
        {[callbackPtr](std::future<FileTransferResult> result){
            try {
                (*callbackPtr)(result.get().data);
            } catch (FileTransferError & e) {
                return (*callbackPtr)(std::shared_ptr<std::string>());
            } catch (...) {
                callbackPtr->rethrow();
            }
        }}
    );
}

void IPFSBinaryCacheStore::writeNarInfo(ref<NarInfo> narInfo)
{
    auto json = nlohmann::json::object();

    if (std::optional optNarHashSize = *narInfo->viewHashResultConst()) {
        auto & [narHash, narSize] = *optNarHashSize;
        json["narHash"] = narHash.to_string(Base32, true);
        json["narSize"] = narSize;
    }

    auto narMap = getIpfsDag(getIpfsPath())["nar"];

    json["references"] = nlohmann::json::object();
    json["hasSelfReference"] = narInfo->hasSelfReference;
    for (auto & ref : narInfo->references) {
        json["references"].emplace(ref.to_string(), narMap[(std::string) ref.to_string()]);
    }

    if (auto optCA = narInfo->optCA())
        json["ca"] = *optCA;

    if (narInfo->deriver)
        json["deriver"] = printStorePath(*narInfo->deriver);

    json["registrationTime"] = narInfo->registrationTime;

    json["sigs"] = nlohmann::json::array();
    for (auto & sig : narInfo->sigs)
        json["sigs"].push_back(sig);

    if (!narInfo->url.empty()) {
        json["ipfsCid"] = nlohmann::json::object();
        json["ipfsCid"]["/"] = std::string(narInfo->url, 7);
    }

    if (narInfo->fileHash)
        json["downloadHash"] = narInfo->fileHash->to_string(Base32, true);

    json["downloadSize"] = narInfo->fileSize;
    json["compression"] = narInfo->compression;
    json["system"] = narInfo->system;

    auto narObjectPath = putIpfsDag(json);

    auto state(_state.lock());
    json = getIpfsDag(state->ipfsPath);

    if (json.find("nar") == json.end())
        json["nar"] = nlohmann::json::object();

    auto hashObject = nlohmann::json::object();
    hashObject.emplace("/", std::string(narObjectPath, 6));

    json["nar"].emplace(narInfo->path.to_string(), hashObject);

    state->ipfsPath = putIpfsDag(json);

    {
        auto hashPart = narInfo->path.hashPart();
        auto state_(this->state.lock());
        state_->pathInfoCache.upsert(
            std::string { hashPart },
            PathInfoCacheValue { .value = std::shared_ptr<NarInfo>(narInfo) });
    }
}

// <cidv1> ::= <multibase-prefix><cid-version><multicodec-packed-content-type><multihash-content-address>
// f = base16
// cid-version = 01
// codec = 78 (git codec) / 71 (dag codec)
// multicodec-packed-content-type = 1114
std::optional<std::string> IPFSBinaryCacheStore::getCidFromCA(StorePathDescriptor ca)
{
    if (std::holds_alternative<FixedOutputInfo>(ca.info)) {
        auto ca_ = std::get<FixedOutputInfo>(ca.info);
        if (ca_.method == FileIngestionMethod::Git) {
            assert(ca_.hash.type == htSHA1);
            return "f01781114" + ca_.hash.to_string(Base16, false);
        }
    } else if (std::holds_alternative<IPFSHash>(ca.info))
        return "f01711220" + std::get<IPFSHash>(ca.info).hash.to_string(Base16, false);

    assert(!std::holds_alternative<IPFSInfo>(ca.info));

    return std::nullopt;
}

std::string IPFSBinaryCacheStore::putIpfsBlock(std::string s, std::string format, std::string mhtype)
{
    auto uri = daemonUri + "/api/v0/block/put";
    uri += "?format=" + format;
    uri += "&mhtype=" + mhtype;

    auto req = FileTransferRequest(uri);
    req.data = std::make_shared<string>(s);
    req.post = true;
    req.tries = 1;
    auto res = getFileTransfer()->upload(req);
    auto json = nlohmann::json::parse(*res.data);
    return (std::string) json["Key"];
}

std::string IPFSBinaryCacheStore::addGit(Path path, std::string modulus, bool hasSelfReference)
{
    struct stat st;
    if (lstat(path.c_str(), &st))
        throw SysError("getting attributes of path '%1%'", path);

    if (S_ISDIR(st.st_mode)) {
        for (auto & i : readDirectory(path))
            addGit(path + "/" + i.name, modulus, hasSelfReference);
    }

    StringSink sink;

    if (hasSelfReference) {
        RewritingSink rewritingSink(modulus, std::string(modulus.size(), 0), sink);

        dumpGitWithCustomHash([&]{ return std::make_unique<HashModuloSink>(htSHA1, modulus); }, path, rewritingSink);

        rewritingSink.flush();

        for (auto & pos : rewritingSink.matches) {
            auto s = fmt("|%d", pos);
            sink((unsigned char *) s.data(), s.size());
        }
    } else
        dumpGit(htSHA1, path, sink);

    return putIpfsBlock(*sink.s, "git-raw", "sha1");
}

void IPFSBinaryCacheStore::unrewriteModulus(std::string & data, std::string hashPart)
{
    // unrewrite modulus
    std::string offset;
    size_t i = data.size() - 1;
    for (; i > 0; i--) {
        char c = data.data()[i];
        if (!(c >= '0' && c <= '9') && c != '|') {
            i += offset.size();
            break;
        }
        if (c == '|') {
            int pos;
            try {
                pos = stoi(offset);
            } catch (std::invalid_argument& e) {
                break;
            }
            assert(pos > 0 && pos + hashPart.size() < i);
            assert(std::string(data, pos, hashPart.size()) == std::string(hashPart.size(), 0));
            std::copy(hashPart.begin(), hashPart.end(), data.begin() + pos);
            offset = "";
        } else
            offset = c + offset;
    }
    data.erase(i + 1);
}

std::unique_ptr<Source> IPFSBinaryCacheStore::getGitObject(std::string path, std::string hashPart, bool hasSelfReference)
{
    return sinkToSource([path, hashPart, hasSelfReference, this](Sink & sink) {
        StringSink sink2;
        getIpfsBlock(path, sink2);

        std::string result = *sink2.s;
        if (hasSelfReference)
            unrewriteModulus(result, hashPart);
        sink(result);
    });
}

void IPFSBinaryCacheStore::getGitEntry(ParseSink & sink, const Path & path,
    const Path & realStoreDir, const Path & storeDir,
    int perm, std::string name, Hash hash, std::string hashPart,
    bool hasSelfReference)
{
    auto source = getGitObject("/ipfs/f01781114" + hash.to_string(Base16, false), hashPart, hasSelfReference);
    parseGitWithPath(sink, *source, path + "/" + name, realStoreDir, storeDir,
        [hashPart, this, hasSelfReference] (ParseSink & sink, const Path & path, const Path & realStoreDir, const Path & storeDir,
            int perm, std::string name, Hash hash) {
            getGitEntry(sink, path, realStoreDir, storeDir, perm, name, hash, hashPart, hasSelfReference);
        }, perm);
}

void IPFSBinaryCacheStore::addToStore(const ValidPathInfo & info, Source & narSource,
    RepairFlag repair, CheckSigsFlag checkSigs)
{

    if (!repair && isValidPath(info.path)) return;

    if (!allowModify)
        throw Error("can't update '%s'", getUri());

    /* Verify that all references are valid. This may do some .narinfo
       reads, but typically they'll already be cached. */
    for (auto & ref : info.references)
        try {
            if (ref != info.path)
                queryPathInfo(ref);
        } catch (InvalidPath &) {
            throw Error("cannot add '%s' to the binary cache because the reference '%s' is not valid",
                printStorePath(info.path), printStorePath(ref));
        }

    // Note this doesn’t require a IPFS store, it just goes in the
    // global namespace.
    if (info.optCA() && std::holds_alternative<FixedOutputHash>(*info.optCA())) {
        auto ca_ = std::get<FixedOutputHash>(*info.optCA());
        if (ca_.method == FileIngestionMethod::Git) {
            auto nar = make_ref<std::string>(narSource.drain());

            AutoDelete tmpDir(createTempDir(), true);
            StringSource savedNAR(*nar);
            restorePath((Path) tmpDir + "/tmp", savedNAR);

            auto key = ipfsCidFormatBase16(addGit((Path) tmpDir + "/tmp", std::string(info.path.hashPart()), info.hasSelfReference));
            assert(std::string(key, 0, 9) == "f01781114");

            auto hash = Hash::parseAny(std::string(key, 9), htSHA1);
            assert(hash == ca_.hash);

            auto narInfo = make_ref<NarInfo>(info);
            narInfo->viewHashResult() = { hashString(htSHA256, *nar), nar->size() };
            narInfo->url = "ipfs://" + key;

            {
                auto hashPart = narInfo->path.hashPart();
                auto state_(this->state.lock());
                state_->pathInfoCache.upsert(
                    std::string { hashPart },
                    PathInfoCacheValue { .value = std::shared_ptr<NarInfo>(narInfo) });
            }

            return;
        }
    } else if (info.optCA() && std::holds_alternative<IPFSHash>(*info.optCA())) {
        auto nar = make_ref<std::string>(narSource.drain());

        AutoDelete tmpDir(createTempDir(), true);
        StringSource savedNAR(*nar);
        restorePath((Path) tmpDir + "/tmp", savedNAR);

        auto key = ipfsCidFormatBase16(addGit((Path) tmpDir + "/tmp", std::string(info.path.hashPart()), info.hasSelfReference));
        assert(std::string(key, 0, 9) == "f01781114");

        IPFSInfo caWithRefs { .hash = Hash::parseAny(std::string(key, 9), htSHA1) };
        caWithRefs.references.hasSelfReference = info.hasSelfReference;
        for (auto & ref : info.references)
            caWithRefs.references.references.insert(IPFSRef {
                    .name = std::string(ref.name()),
                    .hash = std::get<IPFSHash>(*queryPathInfo(ref)->optCA())
                });

        auto fullCa = *info.fullStorePathDescriptorOpt();
        auto cid = getCidFromCA(fullCa);

        auto realCa = StorePathDescriptor {
            .name = std::string(info.path.name()),
            .info = caWithRefs
        };

        nlohmann::json json = realCa;

        auto cid_ = ipfsCidFormatBase16(std::string(putIpfsDag(realCa, "sha2-256"), 6));
        assert(cid_ == cid);
        assert(cid_ == "f01711220" + std::get<IPFSHash>(*info.optCA()).hash.to_string(Base16, false));

        auto narInfo = make_ref<NarInfo>(info);;
        narInfo->viewHashResult() = { hashString(htSHA256, *nar), nar->size() };
        narInfo->url = "ipfs://" + key;

        {
            auto hashPart = narInfo->path.hashPart();
            auto state_(this->state.lock());
            state_->pathInfoCache.upsert(
                std::string { hashPart },
                PathInfoCacheValue { .value = std::shared_ptr<NarInfo>(narInfo) });
        }

        return;
    }

    if (trustless)
        throw Error("cannot add '%s' to store because of trustless mode", printStorePath(info.path));

    // FIXME: See if we can use the original source to reduce memory usage.
    auto nar = make_ref<std::string>(narSource.drain());

    assert(nar->compare(0, narMagic.size(), narMagic) == 0);

    auto narInfo = make_ref<NarInfo>(info);

    auto narSize = nar->size();
    auto narHash = hashString(htSHA256, *nar);

    narInfo->viewHashResult() = { narHash, narSize };

    if (info.optNarHash() && *info.optNarHash() != narHash)
        throw Error("refusing to copy corrupted path '%1%' to binary cache", printStorePath(info.path));

    /* Compress the NAR. */
    narInfo->compression = compression;
    auto now1 = std::chrono::steady_clock::now();
    auto narCompressed = compress(compression, *nar, parallelCompression);
    auto now2 = std::chrono::steady_clock::now();
    narInfo->fileHash = hashString(htSHA256, *narCompressed);
    narInfo->fileSize = narCompressed->size();

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now2 - now1).count();
    printMsg(lvlTalkative, "copying path '%1%' (%2% bytes, compressed %3$.1f%% in %4% ms) to binary cache",
        printStorePath(narInfo->path), narSize,
        ((1.0 - (double) narCompressed->size() / nar->size()) * 100.0),
        duration);

    /* Atomically write the NAR file. */
    stats.narWrite++;
    narInfo->url = "ipfs://" + addFile(*narCompressed);

    stats.narWriteBytes += nar->size();
    stats.narWriteCompressedBytes += narCompressed->size();
    stats.narWriteCompressionTimeMs += duration;

    /* Atomically write the NAR info file.*/
    if (secretKey) narInfo->sign(*this, *secretKey);

    writeNarInfo(narInfo);

    stats.narInfoWrite++;
}

bool IPFSBinaryCacheStore::isValidPathUncached(StorePathOrDesc storePathOrDesc)
{
    auto storePath = this->bakeCaIfNeeded(storePathOrDesc);
    auto ca = std::get_if<1>(&storePathOrDesc);

    if (ca) {
        auto cid = getCidFromCA(*ca);
        if (cid && ipfsBlockStat("/ipfs/" + *cid))
            return true;
    }

    if (trustless)
        return false;

    auto json = getIpfsDag(getIpfsPath());
    if (!json.contains("nar"))
        return false;
    return json["nar"].contains(storePath.to_string());
}

void IPFSBinaryCacheStore::narFromPath(StorePathOrDesc storePathOrDesc, Sink & sink)
{
    auto info = queryPathInfo(storePathOrDesc).cast<const NarInfo>();

    uint64_t narSize = 0;

    LambdaSink wrapperSink([&](const unsigned char * data, size_t len) {
        sink(data, len);
        narSize += len;
    });

    // ugh... we have to convert git data to nar.
    if (info->url[7] != 'Q' && hasPrefix(ipfsCidFormatBase16(std::string(info->url, 7)), "f0178")) {
        AutoDelete tmpDir(createTempDir(), true);
        // FIXME this is wrong, just doing so it builds
        auto storePath = bakeCaIfNeeded(storePathOrDesc);
        auto hasSelfReference = info->hasSelfReference;
        auto source = getGitObject("/ipfs/" + std::string(info->url, 7), std::string(storePath.hashPart()), hasSelfReference);
        restoreGit((Path) tmpDir + "/tmp", *source, storeDir, storeDir,
            [this, storePath, hasSelfReference] (ParseSink & sink, const Path & path, const Path & realStoreDir, const Path & storeDir,
                int perm, std::string name, Hash hash) {
                getGitEntry(sink, path, realStoreDir, storeDir, perm, name, hash, std::string(storePath.hashPart()), hasSelfReference);
            });
        dumpPath((Path) tmpDir + "/tmp", wrapperSink);
        return;
    }

    auto decompressor = makeDecompressionSink(info->compression, wrapperSink);

    try {
        getFile(info->url, *decompressor);
    } catch (NoSuchBinaryCacheFile & e) {
        throw SubstituteGone(e.what());
    }

    decompressor->finish();

    stats.narRead++;
    //stats.narReadCompressedBytes += nar->size(); // FIXME
    stats.narReadBytes += narSize;
}

void IPFSBinaryCacheStore::queryPathInfoUncached(StorePathOrDesc storePathOrCa,
    Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept
{
    // TODO: properly use callbacks
    auto storePath = bakeCaIfNeeded(storePathOrCa);

    auto callbackPtr = std::make_shared<decltype(callback)>(std::move(callback));

    auto uri = getUri();
    auto storePathS = printStorePath(storePath);
    auto act = std::make_shared<Activity>(*logger, lvlTalkative, actQueryPathInfo,
        fmt("querying info about '%s' on '%s'", storePathS, uri), Logger::Fields{storePathS, uri});
    PushActivity pact(act->id);

    if (auto ca_ = std::get_if<1>(&storePathOrCa)) {
        StorePathDescriptor ca = static_cast<const StorePathDescriptor &>(*ca_);
        auto cid = getCidFromCA(ca);
        if (cid && ipfsBlockStat("/ipfs/" + *cid)) {
            std::string url("ipfs://" + *cid);
            if (hasPrefix(ipfsCidFormatBase16(*cid), "f0171")) {
                auto json = getIpfsDag("/ipfs/" + *cid);
                url = "ipfs://" + (std::string) json.at("cid").at("/");

                json.at("cid").at("/") = ipfsCidFormatBase16(json.at("cid").at("/").get<std::string_view>());
                for (auto & ref : json.at("references").at("references"))
                    ref.at("cid").at("/") = ipfsCidFormatBase16(ref.at("cid").at("/").get<std::string_view>());

                // Dummy value to set tag bit.
                ca = StorePathDescriptor {
                    .name = "t e m p",
                    TextInfo { { .hash = Hash { htSHA256 } } },
                };
                from_json(json, ca);
            }
            NarInfo narInfo {
                *this,
                std::move(ca),
            };
            assert(narInfo.path == storePath);
            narInfo.url = url;
            (*callbackPtr)((std::shared_ptr<ValidPathInfo>)
                std::make_shared<NarInfo>(narInfo));
            return;
        }
    }

    if (trustless) {
        (*callbackPtr)(nullptr);
        return;
    }

    auto json = getIpfsDag(getIpfsPath());

    if (!json.contains("nar") || !json["nar"].contains(storePath.to_string()))
        return (*callbackPtr)(nullptr);

    auto narObjectHash = (std::string) json["nar"][(std::string) storePath.to_string()]["/"];
    json = getIpfsDag("/ipfs/" + narObjectHash);

    NarInfo narInfo {
        std::move(storePath),
        This<HashResult> { HashResult {
            Hash::parseAnyPrefixed(json.at("narHash").get<std::string_view>()),
            (uint64_t) json["narSize"],
        } },
    };

    for (auto & ref : json["references"].items())
        narInfo.references.insert(StorePath(ref.key()));

    if (json["hasSelfReference"])
        narInfo.hasSelfReference = json["hasSelfReference"];

    if (json.find("ca") != json.end()) {
        ContentAddress temp = TextHash { .hash = Hash::dummy };
        json["ca"].get_to(temp);
        narInfo.viewCA() = temp;
    }

    if (json.find("deriver") != json.end())
        narInfo.deriver = parseStorePath((std::string) json["deriver"]);

    if (json.find("registrationTime") != json.end())
        narInfo.registrationTime = json["registrationTime"];

    if (json.find("sigs") != json.end())
        for (auto & sig : json["sigs"])
            narInfo.sigs.insert((std::string) sig);

    if (json.find("ipfsCid") != json.end())
        narInfo.url = "ipfs://" + json["ipfsCid"]["/"].get<std::string>();

    if (json.find("downloadHash") != json.end())
        narInfo.fileHash = Hash::parseAnyPrefixed((std::string) json["downloadHash"]);

    if (json.find("downloadSize") != json.end())
        narInfo.fileSize = json["downloadSize"];

    if (json.find("compression") != json.end())
        narInfo.compression = json["compression"];

    if (json.find("system") != json.end())
        narInfo.system = json["system"];

    (*callbackPtr)((std::shared_ptr<ValidPathInfo>)
        std::make_shared<NarInfo>(narInfo));
}

StorePath IPFSBinaryCacheStore::addToStore(const string & name, const Path & srcPath,
    FileIngestionMethod method, HashType hashAlgo, PathFilter & filter, RepairFlag repair)
{
    // FIXME: some cut&paste from LocalStore::addToStore().

    /* Read the whole path into memory. This is not a very scalable
       method for very large paths, but `copyPath' is mainly used for
       small files. */
    StringSink sink;
    Hash h { htSHA256 }; // dummy initial value
    switch (method) {
    case FileIngestionMethod::Recursive:
        dumpPath(srcPath, sink, filter);
        h = hashString(hashAlgo, *sink.s);
        break;
    case FileIngestionMethod::Flat: {
        auto s = readFile(srcPath);
        dumpString(s, sink);
        h = hashString(hashAlgo, s);
        break;
    }
    case FileIngestionMethod::Git: {
        dumpPath(srcPath, sink, filter);
        h = dumpGitHash(htSHA1, srcPath);
        break;
    }
    }

    ValidPathInfo info {
        *this,
        StorePathDescriptor {
            .name = name,
            .info = FixedOutputInfo {
                {
                    .method = method,
                    .hash = h,
                },
                {},
            },
        },
    };

    auto source = StringSource { *sink.s };
    addToStore(info, source, repair, CheckSigs);

    return std::move(info.path);
}

StorePath IPFSBinaryCacheStore::addTextToStore(const string & name, const string & s,
    const StorePathSet & references, RepairFlag repair)
{
    StringSink sink;
    dumpString(s, sink);
    auto narHash = hashString(htSHA256, *sink.s);

    ValidPathInfo info {
        *this,
        {
            .name = name,
            TextInfo {
                { .hash = hashString(htSHA256, s) },
                references,
            },
        },
        std::pair { std::move(narHash), sink.s->size() },
    };

    if (repair || !isValidPath(info.path)) {
        StringSink sink;
        dumpString(s, sink);
        auto source = StringSource { *sink.s };
        addToStore(info, source, repair, CheckSigs);
    }

    return std::move(info.path);
}

void IPFSBinaryCacheStore::addSignatures(const StorePath & storePath, const StringSet & sigs)
{
    if (!allowModify)
        throw Error("can't update '%s'", getUri());

    // trustless already has a signature, nothing more is needed
    if (trustless)
        return;

    /* Note: this is inherently racy since there is no locking on
       binary caches. In particular, with S3 this unreliable, even
       when addSignatures() is called sequentially on a path, because
       S3 might return an outdated cached version. */

    auto narInfo = make_ref<NarInfo>((NarInfo &) *queryPathInfo(storePath));

    narInfo->sigs.insert(sigs.begin(), sigs.end());

    writeNarInfo(narInfo);
}

void IPFSBinaryCacheStore::addTempRoot(const StorePath & path)
{
    if (trustless)
        // No trust root to pin
        return;

    // TODO make temporary pin/addToStore, see
    // https://github.com/ipfs/go-ipfs/issues/4559 and
    // https://github.com/ipfs/go-ipfs/issues/4328 for some ideas.
    auto uri = daemonUri + "/api/v0/pin/add?arg=" + getIpfsPath() + "/" "nar" "/" + string { path.to_string() };

    FileTransferRequest request(uri);
    request.post = true;
    request.tries = 1;
    getFileTransfer()->upload(request);
}

static RegisterStoreImplementation<IPFSBinaryCacheStore, IPFSBinaryCacheStoreConfig> regStore;

}

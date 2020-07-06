#include <cstring>
#include <nlohmann/json.hpp>

#include "binary-cache-store.hh"
#include "filetransfer.hh"
#include "nar-info-disk-cache.hh"
#include "archive.hh"
#include "compression.hh"
#include "git.hh"
#include "names.hh"
#include "references.hh"

namespace nix {

MakeError(UploadToIPFS, Error);

class IPFSBinaryCacheStore : public Store
{

public:

    const Setting<std::string> compression{this, "xz", "compression", "NAR compression method ('xz', 'bzip2', or 'none')"};
    const Setting<Path> secretKeyFile{this, "", "secret-key", "path to secret key used to sign the binary cache"};
    const Setting<bool> parallelCompression{this, false, "parallel-compression",
        "enable multi-threading compression, available for xz only currently"};

private:

    std::unique_ptr<SecretKey> secretKey;
    std::string narMagic;

    std::string cacheUri;
    std::string daemonUri;

    std::string getIpfsPath() {
        auto state(_state.lock());
        return state->ipfsPath;
    }
    std::string initialIpfsPath;
    std::optional<string> optIpnsPath;

    struct State
    {
        std::string ipfsPath;
    };
    Sync<State> _state;

    // only enable trustless operations
    bool trustless = false;

public:

    IPFSBinaryCacheStore(
      const Params & params, const Path & _cacheUri)
            : Store(params)
            , cacheUri(_cacheUri)
    {
        auto state(_state.lock());

        if (secretKeyFile != "")
            secretKey = std::unique_ptr<SecretKey>(new SecretKey(readFile(secretKeyFile)));

        StringSink sink;
        sink << narVersionMagic1;
        narMagic = *sink.s;

        if (cacheUri.back() == '/' && cacheUri != "ipfs://")
            cacheUri.pop_back();

        if (hasPrefix(cacheUri, "ipfs://")) {
            if (cacheUri == "ipfs://")
                trustless = true;
            else {
                initialIpfsPath = "/ipfs/" + std::string(cacheUri, 7);
                state->ipfsPath = initialIpfsPath;
            }
        } else if (hasPrefix(cacheUri, "ipns://"))
            optIpnsPath = "/ipns/" + std::string(cacheUri, 7);
        else
            throw Error("unknown IPNS URI '%s'", cacheUri);

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
        if (optIpnsPath) {
            auto ipnsPath = *optIpnsPath;
            initialIpfsPath = resolveIPNSName(ipnsPath);
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

    std::string getUri() override
    {
        return cacheUri;
    }

private:

    std::string putIpfsDag(nlohmann::json data, std::optional<std::string> hash = std::nullopt)
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

    nlohmann::json getIpfsDag(std::string objectPath)
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
    static std::optional<string> isDNSLinkPath(std::string path)
    {
        if (path.find("/ipns/") != 0)
            throw Error("path '%s' is not an ipns path", path);
        auto subpath = std::string(path, 6);
        if (subpath.find(".") != std::string::npos) {
            return subpath;
        }
        return std::nullopt;
    }

    bool ipfsObjectExists(const std::string ipfsPath)
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

    std::optional<uint64_t> ipfsBlockStat(std::string ipfsPath)
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

    bool fileExists(const std::string & path)
    {
        return ipfsObjectExists(getIpfsPath() + "/" + path);
    }

    // Resolve the IPNS name to an IPFS object
    std::string resolveIPNSName(std::string ipnsPath) {
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

    Path formatPathAsProtocol(Path path) {
        if (hasPrefix(path, "/ipfs/"))
            return "ipfs://" + path.substr(strlen("/ipfs/"), string::npos);
        else if (hasPrefix(path, "/ipns/"))
            return "ipns://" + path.substr(strlen("/ipfs/"), string::npos);
        else return path;
    }

    const std::string base16Alpha = "0123456789abcdef";
    std::string ipfsCidFormatBase16(std::string cid)
    {
        if (cid[0] == 'f') return cid;
        assert(cid[0] == 'b');
        std::string newCid = "f";
        unsigned short remainder;
        for (size_t i = 1; i < cid.size(); i++) {
            if (cid[i] >= 'a' && cid[i] <= 'z')
                remainder = (remainder << 5) | (cid[i] - 'a');
            else if (cid[i] >= '2' && cid[i] <= '7')
                remainder = (remainder << 5) | (26 + cid[i] - '2');
            else throw Error("unknown character: '%c'", cid[i]);;
            if ((i % 4) == 0)
                newCid += base16Alpha[(remainder >> 4) & 0xf];
            newCid += base16Alpha[(remainder >> (i % 4)) & 0xf];
        }
        return newCid;
    }

public:

    // IPNS publish can be slow, we try to do it rarely.
    void sync() override
    {
        auto state(_state.lock());

        if (trustless)
            return;

        if (!optIpnsPath) {
            if (initialIpfsPath != state->ipfsPath)
                throw Error("The current IPFS address doesn't match the configured one. \n  initial: %s\n  current: %s",
                    formatPathAsProtocol(initialIpfsPath), formatPathAsProtocol(state->ipfsPath));
            else
                return;
        }

        auto ipnsPath = *optIpnsPath;

        auto resolvedIpfsPath = resolveIPNSName(ipnsPath);
        if (resolvedIpfsPath != initialIpfsPath) {
            throw Error("The IPNS hash or DNS link %s resolves now to something different from the value it had when Nix was started;\n  wanted: %s\n  got %s\nPerhaps something else updated it in the meantime?",
                ipnsPath, initialIpfsPath, resolvedIpfsPath);
        }

        if (resolvedIpfsPath == state->ipfsPath) {
            printMsg(lvlInfo, "The hash is already up to date, nothing to do");
            return;
        }

        // Now, we know that paths are not up to date but also not changed due to updates in DNS or IPNS hash.
        auto optDomain = isDNSLinkPath(ipnsPath);
        if (optDomain) {
            auto domain = *optDomain;
            throw Error("The provided ipns path is a DNSLink, and syncing those is not supported.\n  Current DNSLink: %s\nYou should update your DNS settings"
                , domain);
        }

        debug("Publishing '%s' to '%s', this could take a while.", state->ipfsPath, ipnsPath);

        auto uri = daemonUri + "/api/v0/name/publish?allow-offline=true";
        uri += "&arg=" + getFileTransfer()->urlEncode(state->ipfsPath);

        // Given the hash, we want to discover the corresponding name in the
        // `ipfs key list` command, so that we publish to the right address in
        // case the user has multiple ones available.

        // NOTE: this is needed for ipfs < 0.5.0 because key must be a
        // name, not an address.

        auto ipnsPathHash = std::string(ipnsPath, 6);
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

private:

    void addLink(std::string name, std::string ipfsObject)
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

    std::string addFile(const std::string & data)
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

    void upsertFile(const std::string & path, const std::string & data, const std::string & mimeType)
    {
        try {
            addLink(path, "/ipfs/" + addFile(data));
        } catch (FileTransferError & e) {
            throw UploadToIPFS("while uploading to IPFS binary cache at '%s': %s", cacheUri, e.info());
        }
    }

    void getFile(const std::string & path,
        Callback<std::shared_ptr<std::string>> callback) noexcept
    {
        std::string path_(path);
        if (hasPrefix(path, "ipfs://"))
            path_ = "/ipfs/" + std::string(path, 7);
        getIpfsObject(path_, std::move(callback));
    }

    void getFile(const std::string & path, Sink & sink)
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

    std::shared_ptr<std::string> getFile(const std::string & path)
    {
        StringSink sink;
        try {
            getFile(path, sink);
        } catch (NoSuchBinaryCacheFile &) {
            return nullptr;
        }
        return sink.s;
    }

    void getIpfsObject(const std::string & ipfsPath,
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

    void getIpfsBlock(const std::string & path, Sink & sink)
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

    std::shared_ptr<std::string> getIpfsBlock(const std::string & path)
    {
        StringSink sink;
        try {
            getIpfsBlock(path, sink);
        } catch (NoSuchBinaryCacheFile &) {
            return nullptr;
        }
        return sink.s;
    }

    void getIpfsBlock(const std::string & ipfsPath,
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

    void writeNarInfo(ref<NarInfo> narInfo)
    {
        auto json = nlohmann::json::object();
        json["narHash"] = narInfo->narHash->to_string(Base32, true);
        json["narSize"] = narInfo->narSize;

        auto narMap = getIpfsDag(getIpfsPath())["nar"];

        json["references"] = nlohmann::json::object();
        json["hasSelfReference"] = narInfo->hasSelfReference;
        for (auto & ref : narInfo->references) {
            json["references"].emplace(ref.to_string(), narMap[(std::string) ref.to_string()]);
        }

        json["ca"] = narInfo->ca;

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
    std::optional<std::string> getCidFromCA(ContentAddress ca)
    {
        if (std::holds_alternative<FixedOutputInfo>(ca.info)) {
            auto ca_ = std::get<FixedOutputInfo>(ca.info);
            if (ca_.method == FileIngestionMethod::Git) {
                assert(ca_.hash.type == htSHA1);
                return "f01781114" + ca_.hash.to_string(Base16, false);
            }
        } else if (std::holds_alternative<IPFSInfo>(ca.info)) {
            auto path = makeFixedOutputPathFromCA(ca);
            auto hash = Hash::parseAny(path.hashPart(), htSHA1);
            return "f01711114" + hash.to_string(Base16, false);
        }

        return std::nullopt;
    }

    std::string putIpfsGitBlock(std::string s)
    {
        auto uri = daemonUri + "/api/v0/block/put?format=git-raw&mhtype=sha1";

        auto req = FileTransferRequest(uri);
        req.data = std::make_shared<string>(s);
        req.post = true;
        req.tries = 1;
        auto res = getFileTransfer()->upload(req);
        auto json = nlohmann::json::parse(*res.data);
        return (std::string) json["Key"];
    }

    std::string addGit(Path path, std::string modulus)
    {
        struct stat st;
        if (lstat(path.c_str(), &st))
            throw SysError("getting attributes of path '%1%'", path);

        if (S_ISDIR(st.st_mode)) {
            for (auto & i : readDirectory(path))
                addGit(path + "/" + i.name, modulus);
        }

        StringSink sink;
        RewritingSink rewritingSink(modulus, std::string(modulus.size(), 0), sink);

        dumpGitWithCustomHash([&]{ return std::make_unique<HashModuloSink>(htSHA1, modulus); }, path, rewritingSink);

        rewritingSink.flush();

        for (auto & pos : rewritingSink.matches) {
            auto s = fmt("|%d", pos);
            sink((unsigned char *) s.data(), s.size());
        }

        return putIpfsGitBlock(*sink.s);
    }

    std::unique_ptr<Source> getGitObject(std::string path, std::string hashPart, bool hasSelfReference)
    {
        return sinkToSource([path, hashPart, hasSelfReference, this](Sink & sink) {
            StringSink sink2;
            getIpfsBlock(path, sink2);

            std::string result = *sink2.s;
            if (hasSelfReference) {
                // unrewrite modulus
                std::string offset;
                size_t i = result.size() - 1;
                for (; i > 0; i--) {
                    char c = result.data()[i];
                    if (!(c >= '0' && c <= '9') && c != '|')
                        break;
                    if (c == '|') {
                        int pos = stoi(offset);
                        assert(pos > 0 && pos + hashPart.size() < i);
                        assert(std::string(result, pos, hashPart.size()) == std::string(hashPart.size(), 0));
                        std::copy(hashPart.begin(), hashPart.end(), result.begin() + pos);
                        offset = "";
                    } else
                        offset = c + offset;
                }
                result.erase(i + 1);
            }
            sink(result);
        });
    }

    void getGitEntry(ParseSink & sink, const Path & path,
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


public:

    void addToStore(const ValidPathInfo & info, Source & narSource,
        RepairFlag repair, CheckSigsFlag checkSigs, std::shared_ptr<FSAccessor> accessor) override
    {

        if (!repair && isValidPath(info.path)) return;

        // Note this doesn’t require a IPFS store, it just goes in the
        // global namespace.
        if (info.ca && std::holds_alternative<FixedOutputHash>(*info.ca)) {
            auto ca_ = std::get<FixedOutputHash>(*info.ca);
            if (ca_.method == FileIngestionMethod::Git) {
                AutoDelete tmpDir(createTempDir(), true);
                TeeSource savedNAR(narSource);
                restorePath((Path) tmpDir + "/tmp", savedNAR);

                auto key = ipfsCidFormatBase16(addGit((Path) tmpDir + "/tmp", std::string(info.path.hashPart())));
                assert(std::string(key, 0, 9) == "f01781114");

                auto hash = Hash::parseAny(std::string(key, 9), htSHA1);
                assert(hash == ca_.hash);

                return;
            }
        } else if (info.ca && std::holds_alternative<IPFSHash>(*info.ca)) {
            auto fullCa = *info.fullContentAddressOpt();
            auto cid = getCidFromCA(fullCa);

            auto cid_ = ipfsCidFormatBase16(std::string(putIpfsDag(fullCa, "sha1"), 6));
            assert(cid_ == cid);

            AutoDelete tmpDir(createTempDir(), true);
            TeeSource savedNAR(narSource);
            restorePath((Path) tmpDir + "/tmp", savedNAR);

            auto key = ipfsCidFormatBase16(addGit((Path) tmpDir + "/tmp", std::string(info.path.hashPart())));
            assert(std::string(key, 0, 9) == "f01781114");

            auto hash_ = Hash::parseAny(std::string(key, 9), htSHA1);
            assert(hash_ == std::get<IPFSHash>(*info.ca).hash);

            return;
        }

        if (trustless)
            throw Error("cannot add '%s' to store because of trustless mode", printStorePath(info.path));

        // FIXME: See if we can use the original source to reduce memory usage.
        auto nar = make_ref<std::string>(narSource.drain());

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

        assert(nar->compare(0, narMagic.size(), narMagic) == 0);

        auto narInfo = make_ref<NarInfo>(info);

        narInfo->narSize = nar->size();
        narInfo->narHash = hashString(htSHA256, *nar);

        if (info.narHash && info.narHash != narInfo->narHash)
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
            printStorePath(narInfo->path), narInfo->narSize,
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

    bool isValidPathUncached(StorePathOrCA storePathOrCA) override
    {
        auto storePath = this->bakeCaIfNeeded(storePathOrCA);
        auto ca = std::get_if<1>(&storePathOrCA);

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

    void narFromPath(StorePathOrCA storePathOrCA, Sink & sink) override
    {
        auto info = queryPathInfo(storePathOrCA).cast<const NarInfo>();

        uint64_t narSize = 0;

        LambdaSink wrapperSink([&](const unsigned char * data, size_t len) {
            sink(data, len);
            narSize += len;
        });

        // ugh... we have to convert git data to nar.
        if (info->url[7] != 'Q' && hasPrefix(ipfsCidFormatBase16(std::string(info->url, 7)), "f0178")) {
            AutoDelete tmpDir(createTempDir(), true);
            // FIXME this is wrong, just doing so it builds
            auto storePath = bakeCaIfNeeded(storePathOrCA);
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

    void queryPathInfoUncached(StorePathOrCA storePathOrCa,
        Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept override
    {
        // TODO: properly use callbacks
        auto storePath = bakeCaIfNeeded(storePathOrCa);

        auto callbackPtr = std::make_shared<decltype(callback)>(std::move(callback));

        auto uri = getUri();
        auto storePathS = printStorePath(storePath);
        auto act = std::make_shared<Activity>(*logger, lvlTalkative, actQueryPathInfo,
            fmt("querying info about '%s' on '%s'", storePathS, uri), Logger::Fields{storePathS, uri});
        PushActivity pact(act->id);

        if (auto p = std::get_if<1>(&storePathOrCa)) {
            ContentAddress ca = static_cast<const ContentAddress &>(*p);
            auto cid = getCidFromCA(ca);
            if (cid && ipfsBlockStat("/ipfs/" + *cid)) {
                std::string url("ipfs://" + *cid);
                if (hasPrefix(ipfsCidFormatBase16(*cid), "f0171")) {
                    auto json = getIpfsDag("/ipfs/" + *cid);
                    url = "ipfs://" + (std::string) json.at("cid").at("/");

                    json.at("cid").at("/") = ipfsCidFormatBase16(json.at("cid").at("/"));
                    for (auto & ref : json.at("references").at("references"))
                        ref.at("cid").at("/") = ipfsCidFormatBase16(ref.at("cid").at("/"));

                    // Dummy value to set tag bit.
                    ca = ContentAddress {
                        .name = "t e m p",
                        TextInfo { { .hash = Hash { htSHA256 } } },
                    };
                    from_json(json, ca);
                }
                NarInfo narInfo { *this, ContentAddress { ca } };
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

        NarInfo narInfo { storePath };
        narInfo.narHash = Hash::parseAnyPrefixed((std::string) json["narHash"]);
        narInfo.narSize = json["narSize"];

        for (auto & ref : json["references"].items())
            narInfo.references.insert(StorePath(ref.key()));

        if (json["hasSelfReference"])
            narInfo.hasSelfReference = json["hasSelfReference"];

        if (json.find("ca") != json.end())
            json["ca"].get_to(narInfo.ca);

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

    StorePath addToStore(const string & name, const Path & srcPath,
        FileIngestionMethod method, HashType hashAlgo, PathFilter & filter, RepairFlag repair) override
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
            ContentAddress {
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
        addToStore(info, source, repair, CheckSigs, nullptr);

        return std::move(info.path);
    }

    StorePath addTextToStore(const string & name, const string & s,
        const StorePathSet & references, RepairFlag repair) override
    {
        ValidPathInfo info(computeStorePathForText(name, s, references));
        info.references = references;

        if (repair || !isValidPath(info.path)) {
            StringSink sink;
            dumpString(s, sink);
            auto source = StringSource { *sink.s };
            addToStore(info, source, repair, CheckSigs, nullptr);
        }

        return std::move(info.path);
    }

    void addSignatures(const StorePath & storePath, const StringSet & sigs) override
    {
        if (trustless)
            throw Error("cannot add signatures available for '%s' because of trustless mode", printStorePath(storePath));

        /* Note: this is inherently racy since there is no locking on
           binary caches. In particular, with S3 this unreliable, even
           when addSignatures() is called sequentially on a path, because
           S3 might return an outdated cached version. */

        auto narInfo = make_ref<NarInfo>((NarInfo &) *queryPathInfo(storePath));

        narInfo->sigs.insert(sigs.begin(), sigs.end());

        writeNarInfo(narInfo);
    }

    void addTempRoot(const StorePath & path) override
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

    std::shared_ptr<std::string> getBuildLog(const StorePath & path) override
    { unsupported("getBuildLog"); }

    BuildResult buildDerivation(const StorePath & drvPath, const BasicDerivation & drv,
        BuildMode buildMode) override
    { unsupported("buildDerivation"); }

    void ensurePath(StorePathOrCA ca) override
    { unsupported("ensurePath"); }

    std::optional<StorePath> queryPathFromHashPart(const std::string & hashPart) override
    { unsupported("queryPathFromHashPart"); }

};

static RegisterStoreImplementation regStore([](
    const std::string & uri, const Store::Params & params)
    -> std::shared_ptr<Store>
{
    if (uri.substr(0, strlen("ipfs://")) != "ipfs://" &&
        uri.substr(0, strlen("ipns://")) != "ipns://")
        return 0;
    auto store = std::make_shared<IPFSBinaryCacheStore>(params, uri);
    return store;
});

}

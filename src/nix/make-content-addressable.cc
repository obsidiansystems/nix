#include "command.hh"
#include "store-api.hh"
#include "references.hh"
#include "common-args.hh"
#include "json.hh"
#include "git.hh"
#include "archive.hh"

using namespace nix;

struct CmdMakeContentAddressable : StorePathsCommand, MixJSON
{
    bool ipfsContent;

    CmdMakeContentAddressable()
    {
        realiseMode = Build;

        mkFlag(0, "ipfs", "use ipfs/ipld addressing", &ipfsContent);
    }

    std::string description() override
    {
        return "rewrite a path or closure to content-addressable form";
    }

    Examples examples() override
    {
        return {
            Example{
                "To create a content-addressable representation of GNU Hello (but not its dependencies):",
                "nix make-content-addressable nixpkgs.hello"
            },
            Example{
                "To compute a content-addressable representation of the current NixOS system closure:",
                "nix make-content-addressable -r /run/current-system"
            },
        };
    }

    Category category() override { return catUtility; }

    void run(ref<Store> store, StorePaths storePaths) override
    {
        auto paths = store->topoSortPaths(StorePathSet(storePaths.begin(), storePaths.end()));

        std::reverse(paths.begin(), paths.end());

        std::map<StorePath, ContentAddress> remappings;

        auto jsonRoot = json ? std::make_unique<JSONObject>(std::cout) : nullptr;
        auto jsonRewrites = json ? std::make_unique<JSONObject>(jsonRoot->object("rewrites")) : nullptr;

        for (auto & path : paths) {
            auto pathS = store->printStorePath(path);
            auto oldInfo = store->queryPathInfo(path);
            std::string oldHashPart(path.hashPart());

            StringSink sink;
            store->narFromPath(path, sink);

            StringMap rewrites;

            bool hasSelfReference = oldInfo->hasSelfReference;
            std::set<StorePath> refPaths;
            std::set<ContentAddress> refCAs;
            for (auto & ref : oldInfo->references) {
                auto i = remappings.find(ref);
                auto replacement = i != remappings.end()
                    ? store->makeFixedOutputPathFromCA(i->second)
                    : ref;
                // FIXME: warn about unremapped paths?
                if (replacement != ref)
                    rewrites.insert_or_assign(store->printStorePath(ref), store->printStorePath(replacement));
                if (i != remappings.end())
                    refCAs.insert(i->second);
                else
                    refPaths.insert(ref);
            }

            *sink.s = rewriteStrings(*sink.s, rewrites);

            HashModuloSink hashModuloSink(htSHA256, oldHashPart);
            hashModuloSink((unsigned char *) sink.s->data(), sink.s->size());

            auto narHash = hashModuloSink.finish().first;

            // ugh... we have to convert nar data to git.
            std::optional<Hash> gitHash;
            if (ipfsContent) {
                AutoDelete tmpDir(createTempDir(), true);
                StringSource source(*sink.s);
                restorePath((Path) tmpDir + "/tmp", source);

                gitHash = dumpGitHashWithCustomHash([&]{ return std::make_unique<HashModuloSink>(htSHA1, oldHashPart); }, (Path) tmpDir + "/tmp");

                StringSink sink_;
                RewritingSink rewritingSink(oldHashPart, std::string(oldHashPart.size(), 0), sink_);
                dumpGit(htSHA1, (Path) tmpDir + "/tmp", rewritingSink);

                rewritingSink.flush();

                for (auto & pos : rewritingSink.matches) {
                    auto s = fmt("|%d", pos);
                    sink_((unsigned char *) s.data(), s.size());
                }
            }

            ContentAddressWithoutName ca = ipfsContent
                ? ContentAddressWithoutName {
                    IPFSHashWithOptValue<IPFSGitTreeNode>::fromValue({
                        .gitTree = *gitHash,
                        { {}, hasSelfReference },
                    })
                }
                : ContentAddressWithoutName {
                    FixedOutputInfo {
                        {
                             .method = FileIngestionMethod::Recursive,
                             .hash = narHash,
                        },
                        { {}, hasSelfReference },
                    }
                };

            if (ipfsContent) {
                auto & refs = std::get<IPFSHashWithOptValue<IPFSGitTreeNode>>(ca).value->references;
                //refs.hasSelfReference = hasSelfReference;
                if (refPaths.size() > 0)
                    throw UsageError("IPFS+Git store paths must not have non-CA references");
                for (auto & ca2 : refCAs) {
                    auto p = std::get_if<2>(&ca2.info);
                    if (!p) throw UsageError("IPFS+Git store paths must only have IPFS+Git references");
                    refs.references.insert({
                        .name = ca2.name,
                        .info = *p
                    });
                }
            } else {
                auto & refs = std::get<FixedOutputInfo>(ca).references;
                //refs.hasSelfReference = hasSelfReference;
                for (auto & p : refPaths)
                    refs.references.insert(p);
                for (auto & ca : refCAs)
                    refs.references.insert(store->makeFixedOutputPathFromCA(ca));
            };

            ValidPathInfo info {
                *store,
                ContentAddress {
                    .name = std::string { path.name() },
                    .info = ca,
                },
            };
            info.narHash = narHash;
            info.narSize = sink.s->size();

            if (!json)
                printInfo("rewrote '%s' to '%s'", pathS, store->printStorePath(info.path));

            auto source = sinkToSource([&](Sink & nextSink) {
                RewritingSink rsink2(oldHashPart, std::string(info.path.hashPart()), nextSink);
                rsink2((unsigned char *) sink.s->data(), sink.s->size());
                rsink2.flush();
            });

            store->addToStore(info, *source);

            if (json)
                jsonRewrites->attr(store->printStorePath(path), store->printStorePath(info.path));

            remappings.insert_or_assign(std::move(path), info.fullContentAddressOpt().value());
        }
    }
};

static auto r1 = registerCommand<CmdMakeContentAddressable>("make-content-addressable");

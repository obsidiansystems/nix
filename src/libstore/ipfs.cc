#include "binary-cache-store.hh"

namespace nix {

class IpfsStore : public BinaryCacheStore
{
    Path daemonUri;

public:

    IpfsStore(
        const Params & params, const Path & _daemonUri)
        : BinaryCacheStore(params)
        , daemonUri(_daemonUri)
    {
        if (daemonUri.back() == '/')
            daemonUri.pop_back();
    }

    std::string getUri() override
    {
        return daemonUri;
    }

    void init() override
    {
    }

    bool fileExists(const std::string & path) override
    {
        return false;
    }

    void upsertFile(const std::string & path,
        const std::string & data,
        const std::string & mimeType) override
    {
    }

    void getFile(const std::string & path, Sink & sink) override
    {
    }

    void getFile(const std::string & path, Callback<std::shared_ptr<std::string>> callback) noexcept override
    {
    }

};

static RegisterStoreImplementation regStore([](
    const std::string & uri, const Store::Params & params)
    -> std::shared_ptr<Store>
{
    if (std::string(uri, 0, 12) != "ipfs+http://")
        return 0;
    auto store = std::make_shared<IpfsStore>(params, std::string(uri, 5));
    store->init();
    return store;
});

}

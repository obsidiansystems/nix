#include "path-info.hh"
#include "worker-protocol.hh"

namespace nix {

StorePathSet ValidPathInfo::referencesPossiblyToSelf() const
{
    return references.possiblyToSelf(path);
}

void ValidPathInfo::insertReferencePossiblyToSelf(StorePath && ref)
{
    return references.insertPossiblyToSelf(path, std::move(ref));
}

void ValidPathInfo::setReferencesPossiblyToSelf(StorePathSet && refs)
{
    return references.setPossiblyToSelf(path, std::move(refs));
}


ValidPathInfo::ReferencesIterable ValidPathInfo::referencesIterable() const
{
    return ReferencesIterable {
        .info = *this,
    };
}

ValidPathInfo::ReferencesIterable::const_iterator ValidPathInfo::ReferencesIterable::begin() const
{
    return const_iterator {
        info.path,
        info.references.begin(),
    };
}

ValidPathInfo::ReferencesIterable::const_iterator ValidPathInfo::ReferencesIterable::end() const
{
    return const_iterator {
        info.path,
        info.references.end(),
    };
}

const StorePath &
ValidPathInfo::ReferencesIterable::const_iterator::operator *() const
{
    auto * p = *iter;
    return p ? *p : self;
}

void
ValidPathInfo::ReferencesIterable::const_iterator::operator ++()
{
    ++iter;
}


ValidPathInfo ValidPathInfo::read(Source & source, const Store & store, unsigned int format)
{
    return read(source, store, format, store.parseStorePath(readString(source)));
}

ValidPathInfo ValidPathInfo::read(Source & source, const Store & store, unsigned int format, StorePath && path)
{
    auto deriver = readString(source);
    auto narHash = Hash::parseAny(readString(source), htSHA256);
    ValidPathInfo info(path, narHash);
    if (deriver != "") info.deriver = store.parseStorePath(deriver);
    info.setReferencesPossiblyToSelf(worker_proto::read(store, source, Phantom<StorePathSet> {}));
    source >> info.registrationTime >> info.narSize;
    if (format >= 16) {
        source >> info.ultimate;
        info.sigs = readStrings<StringSet>(source);
        info.ca = parseContentAddressOpt(readString(source));
    }
    return info;
}

void ValidPathInfo::write(
    Sink & sink,
    const Store & store,
    unsigned int format,
    bool includePath) const
{
    if (includePath)
        sink << store.printStorePath(path);
    sink << (deriver ? store.printStorePath(*deriver) : "")
         << narHash.to_string(Base16, false);
    worker_proto::write(store, sink, referencesPossiblyToSelf());
    sink << registrationTime << narSize;
    if (format >= 16) {
        sink << ultimate
             << sigs
             << renderContentAddress(ca);
    }
}

}

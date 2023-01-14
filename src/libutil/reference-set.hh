#pragma once

#include "comparator.hh"

#include <set>

namespace nix {

template<typename Ref>
struct References
{
    std::set<Ref> others;
    bool self = false;

    bool empty() const;
    size_t size() const;

    /* Functions to view references + self as one set, mainly for
       compatibility's sake. */
    std::set<Ref> possiblyToSelf(const Ref & self) const;
    void insertPossiblyToSelf(const Ref & self, Ref && ref);
    void setPossiblyToSelf(const Ref & self, std::set<Ref> && refs);

    GENERATE_CMP(References<Ref>, me->others, me->self);

    class const_iterator;

    const_iterator begin() const;
    const_iterator end() const;
};

template<typename Ref>
bool References<Ref>::empty() const
{
    return !self && others.empty();
}

template<typename Ref>
size_t References<Ref>::size() const
{
    return (self ? 1 : 0) + others.size();
}

template<typename Ref>
std::set<Ref> References<Ref>::possiblyToSelf(const Ref & selfRef) const
{
    std::set<Ref> refs { others };
    if (self)
        refs.insert(selfRef);
    return refs;
}

template<typename Ref>
void References<Ref>::insertPossiblyToSelf(const Ref & selfRef, Ref && ref)
{
    if (ref == selfRef)
        self = true;
    else
        others.insert(std::move(ref));
}

template<typename Ref>
void References<Ref>::setPossiblyToSelf(const Ref & selfRef, std::set<Ref> && refs)
{
    if (refs.count(selfRef)) {
        self = true;
        refs.erase(selfRef);
    }

    others = refs;
}

template<typename Ref>
class References<Ref>::const_iterator {
    enum struct Which { Self, Others };
    Which which;
    const bool & self;
    typename std::set<Ref>::const_iterator others;

    const_iterator(Which which, const bool & self, typename std::set<Ref>::const_iterator others)
        : which(which), self(self), others(others)
    { }

public:
    typedef const Ref * reference;

    reference operator *() const;
    void operator ++();

    GENERATE_EQUAL(const_iterator, me->which, &me->self, me->others);
    GENERATE_NEQ  (const_iterator, me->which, &me->self, me->others);

    friend References<Ref>;
};

template<typename Ref>
typename References<Ref>::const_iterator References<Ref>::begin() const
{
    return const_iterator {
        const_iterator::Which::Self,
        self,
        others.begin(),
    };
}

template<typename Ref>
typename References<Ref>::const_iterator References<Ref>::end() const
{
    return const_iterator {
        const_iterator::Which::Others,
        self,
        others.end(),
    };
}

template<typename Ref>
typename References<Ref>::const_iterator::reference References<Ref>::const_iterator::operator *() const
{
    switch (which) {
    case Which::Self:
        return nullptr;
    case Which::Others:
        return &*others;
    default:
        abort();
    }
}

template<typename Ref>
void References<Ref>::const_iterator::operator ++()
{
    switch (which) {
    case Which::Self:
        which = Which::Others;
        break;
    case Which::Others:
        others++;
        break;
    default:
        abort();
    }
}

}

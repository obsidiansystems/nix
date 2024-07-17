#pragma once
///@type

namespace nix::config {

template<typename T>
struct JustValue
{
    T value;

    operator const T &() const
    {
        return value;
    }
    operator T &()
    {
        return value;
    }
    const T & get() const
    {
        return value;
    }
};

}

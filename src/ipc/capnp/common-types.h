// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_IPC_CAPNP_COMMON_TYPES_H
#define BITCOIN_IPC_CAPNP_COMMON_TYPES_H

#include <capnp/blob.h>
#include <serialize.h>
#include <streams.h>

#include <cstddef>
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace ipc {
namespace capnp {
//! Construct a ParamStream wrapping a data stream with serialization parameters
//! needed to pass transaction objects between bitcoin processes.
//! In the future, more params may be added here to serialize other objects that
//! require serialization parameters. Params should just be chosen to serialize
//! objects completely and ensure that serializing and deserializing objects
//! with the specified parameters produces equivalent objects. It's also
//! harmless to specify serialization parameters here that are not used.
template <typename S>
auto Wrap(S& s)
{
    return ParamsStream{s, TX_WITH_WITNESS};
}

//! Detect if type has a deserialize_type constructor, which is
//! used to deserialize types like CTransaction that can't be unserialized into
//! existing objects because they are immutable.
template <typename T>
concept Deserializable = std::is_constructible_v<T, ::deserialize_type, ::DataStream&>;

template <typename T>
std::vector<unsigned char> SerializeBytes(const T& value)
    requires Serializable<T, DataStream>
{
    DataStream stream;
    auto wrapper{ipc::capnp::Wrap(stream)};
    wrapper << value;
    std::vector<unsigned char> bytes(stream.size());
    std::memcpy(bytes.data(), stream.data(), stream.size());
    return bytes;
}

template <typename T>
T DeserializeBytes(::capnp::Data::Reader data)
    requires Deserializable<T>
{
    SpanReader stream({data.begin(), data.end()});
    auto wrapper{ipc::capnp::Wrap(stream)};
    return T{::deserialize, wrapper};
}

template <typename T>
T DeserializeBytes(::capnp::Data::Reader data)
    requires Unserializable<T, DataStream> && (!Deserializable<T>)
{
    T value;
    SpanReader stream({data.begin(), data.end()});
    auto wrapper{ipc::capnp::Wrap(stream)};
    wrapper >> value;
    return value;
}

inline void SetData(::capnp::Data::Builder output, std::span<const unsigned char> bytes)
{
    std::memcpy(output.begin(), bytes.data(), bytes.size());
}

} // namespace capnp
} // namespace ipc

#endif // BITCOIN_IPC_CAPNP_COMMON_TYPES_H

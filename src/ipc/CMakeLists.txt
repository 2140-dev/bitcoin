# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

set(CAPNPC_SRC_PREFIX ${CMAKE_CURRENT_SOURCE_DIR})
set(CAPNPC_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
capnp_generate_cpp(IPC_CAPNP_SRCS IPC_CAPNP_HDRS
  capnp/common.capnp
  capnp/echo.capnp
  capnp/init.capnp
  capnp/mining.capnp
)

add_library(bitcoin_ipc STATIC EXCLUDE_FROM_ALL
  ${IPC_CAPNP_SRCS}
  ${IPC_CAPNP_HDRS}
  capnp/protocol.cpp
  interfaces.cpp
  process.cpp
)

target_link_libraries(bitcoin_ipc
  PRIVATE
    bitcoin_common
    core_interface
    univalue
    CapnProto::capnp-rpc
)
target_include_directories(bitcoin_ipc
  PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
)

configure_file(.clang-tidy.in .clang-tidy USE_SOURCE_PERMISSIONS COPYONLY)

# Copyright (c) 2025-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

@0xb1a2c3d4e5f60718;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("ipc::capnp::messages");

using Proxy = import "/mp/proxy.capnp";
$Proxy.include("interfaces/noderpc.h");
$Proxy.includeTypes("ipc/capnp/noderpc-types.h");

interface NodeRpc $Proxy.wrap("interfaces::NodeRpc") {
    destroy @0 (context :Proxy.Context) -> ();
    getNetworkInfo @1 (context :Proxy.Context) -> (result :NetworkInfo);
    getDeploymentInfo @2 (context :Proxy.Context, blockHash :Data) -> (result :DeploymentsInfo);
    getBlockchainInfo @3 (context :Proxy.Context) -> (result :BlockchainInfo);
    estimateSmartFee @4 (context :Proxy.Context, confTarget :Int32, conservative :Bool) -> (result :SmartFeeEstimate);
    getBestBlockHash @5 (context :Proxy.Context) -> (result :Text);
    getBlockHash @6 (context :Proxy.Context, height :Int32) -> (result :Text);
    getBlockHeader @7 (context :Proxy.Context, blockHash :Data) -> (result :BlockHeaderInfo);
    getBlock @8 (context :Proxy.Context, blockHash :Data) -> (result :Data);
}

struct SmartFeeEstimate $Proxy.wrap("interfaces::SmartFeeEstimate") {
    feerate @0 :Int64;
    errors @1 :List(Text);
    blocks @2 :Int32;
}

struct BlockHeaderInfo $Proxy.wrap("interfaces::BlockHeaderInfo") {
    hash @0 :Text;
    confirmations @1 :Int32;
    height @2 :Int32;
    version @3 :Int32;
    versionHex @4 :Text $Proxy.name("version_hex");
    merkleroot @5 :Text;
    time @6 :Int64;
    mediantime @7 :Int64;
    nonce @8 :UInt32;
    bits @9 :Text;
    target @10 :Text;
    difficulty @11 :Float64;
    chainwork @12 :Text;
    nTx @13 :Int32 $Proxy.name("n_tx");
    previousblockhash @14 :Text;
    nextblockhash @15 :Text;
}

struct BlockchainPruneInfo $Proxy.wrap("interfaces::BlockchainPruneInfo") {
    height @0 :Int32;
    automatic @1 :Bool;
    targetSize @2 :UInt64 $Proxy.name("target_size");
}

struct BlockchainInfo $Proxy.wrap("interfaces::BlockchainInfo") {
    chain @0 :Text;
    blocks @1 :Int32;
    headers @2 :Int32;
    bestblockhash @3 :Text;
    bits @4 :Text;
    target @5 :Text;
    difficulty @6 :Float64;
    time @7 :Int64;
    mediantime @8 :Int64;
    verificationprogress @9 :Float64;
    initialblockdownload @10 :Bool;
    chainwork @11 :Text;
    sizeOnDisk @12 :UInt64 $Proxy.name("size_on_disk");
    pruned @13 :Bool;
    prune @14 :BlockchainPruneInfo;
    signetChallenge @15 :Text $Proxy.name("signet_challenge");
    warnings @16 :List(Text);
}

struct NetworkInfoNetwork $Proxy.wrap("interfaces::NetworkInfoNetwork") {
    name @0 :Text;
    limited @1 :Bool;
    reachable @2 :Bool;
    proxy @3 :Text;
    proxyRandomizeCredentials @4 :Bool $Proxy.name("proxy_randomize_credentials");
}

struct NetworkInfoLocalAddress $Proxy.wrap("interfaces::NetworkInfoLocalAddress") {
    address @0 :Text;
    port @1 :UInt16;
    score @2 :Int32;
}

struct NetworkInfo $Proxy.wrap("interfaces::NetworkInfo") {
    version @0 :Int32;
    subversion @1 :Text;
    protocolversion @2 :Int32;
    localservices @3 :Text;
    localservicesnames @4 :List(Text);
    localrelay @5 :Bool;
    timeoffset @6 :Int64;
    connections @7 :UInt64;
    connectionsIn @8 :UInt64 $Proxy.name("connections_in");
    connectionsOut @9 :UInt64 $Proxy.name("connections_out");
    networkactive @10 :Bool;
    networks @11 :List(NetworkInfoNetwork);
    relayfee @12 :Int64;
    incrementalfee @13 :Int64;
    localaddresses @14 :List(NetworkInfoLocalAddress);
    warnings @15 :List(Text);
}

struct BIP9Statistics $Proxy.wrap("interfaces::BIP9Statistics") {
    period @0 :Int32;
    threshold @1 :Int32;
    elapsed @2 :Int32;
    count @3 :Int32;
    possible @4 :Bool;
}

struct BIP9DeploymentInfo $Proxy.wrap("interfaces::BIP9DeploymentInfo") {
    bit @0 :Int32;
    startTime @1 :Int64 $Proxy.name("start_time");
    timeout @2 :Int64;
    minActivationHeight @3 :Int32 $Proxy.name("min_activation_height");
    status @4 :Text;
    since @5 :Int32;
    statusNext @6 :Text $Proxy.name("status_next");
    statistics @7 :BIP9Statistics;
    signalling @8 :Text;
}

struct DeploymentInfo $Proxy.wrap("interfaces::DeploymentInfo") {
    name @0 :Text;
    type @1 :Text;
    active @2 :Bool;
    height @3 :Int32;
    bip9 @4 :BIP9DeploymentInfo;
}

struct DeploymentsInfo $Proxy.wrap("interfaces::DeploymentsInfo") {
    hash @0 :Text;
    height @1 :Int32;
    scriptFlags @2 :List(Text) $Proxy.name("script_flags");
    deployments @3 :List(DeploymentInfo);
}


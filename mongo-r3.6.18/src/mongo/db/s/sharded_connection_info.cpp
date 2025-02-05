
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/sharded_connection_info.h"

#include <boost/optional.hpp>
#include <boost/utility/in_place_factory.hpp>

#include "mongo/client/global_conn_pool.h"
#include "mongo/db/client.h"
#include "mongo/db/logical_time_metadata_hook.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/sharding_egress_metadata_hook_for_mongod.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/sharding_connection_hook.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const auto clientSCI = Client::declareDecoration<boost::optional<ShardedConnectionInfo>>();

stdx::mutex addHookMutex;

AtomicUInt32 alreadyAddedHook{0};

}  // namespace

ShardedConnectionInfo::ShardedConnectionInfo() {
    _forceVersionOk = false;
}

ShardedConnectionInfo::~ShardedConnectionInfo() = default;

ShardedConnectionInfo* ShardedConnectionInfo::get(Client* client, bool create) {
    auto& current = clientSCI(client);

    if (!current && create) {
        LOG(1) << "entering shard mode for connection";
        current = boost::in_place();
    }

    return current ? &current.value() : nullptr;
}

void ShardedConnectionInfo::reset(Client* client) {
    clientSCI(client) = boost::none;
}

ChunkVersion ShardedConnectionInfo::getVersion(const std::string& ns) const {
    NSVersionMap::const_iterator it = _versions.find(ns);
    if (it != _versions.end()) {
        return it->second;
    } else {
        return ChunkVersion::UNSHARDED();
    }
}

void ShardedConnectionInfo::setVersion(const std::string& ns, const ChunkVersion& version) {
    _versions[ns] = version;
}

void ShardedConnectionInfo::addHook(ServiceContext* service) {
    if (alreadyAddedHook.loadRelaxed()) {
        return;
    }

    stdx::lock_guard<stdx::mutex> lk{addHookMutex};
    if (alreadyAddedHook.load()) {
        return;
    }

    log() << "first cluster operation detected, adding sharding hook to enable versioning "
             "and authentication to remote servers";

    {
        auto unshardedHookList = stdx::make_unique<rpc::EgressMetadataHookList>();
        unshardedHookList->addHook(stdx::make_unique<rpc::LogicalTimeMetadataHook>(service));
        unshardedHookList->addHook(
            stdx::make_unique<rpc::ShardingEgressMetadataHookForMongod>(service));

        globalConnPool.addHook(new ShardingConnectionHook(false, std::move(unshardedHookList)));
    }

    {
        auto shardedHookList = stdx::make_unique<rpc::EgressMetadataHookList>();
        shardedHookList->addHook(stdx::make_unique<rpc::LogicalTimeMetadataHook>(service));
        shardedHookList->addHook(
            stdx::make_unique<rpc::ShardingEgressMetadataHookForMongod>(service));

        shardConnectionPool.addHook(new ShardingConnectionHook(true, std::move(shardedHookList)));
    }

    alreadyAddedHook.store(1);
}

}  // namespace mongo

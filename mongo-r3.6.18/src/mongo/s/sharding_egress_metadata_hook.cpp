
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

#include <string>

#include "mongo/base/status.h"
#include "mongo/db/audit.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/metadata/client_metadata_ismaster.h"
#include "mongo/rpc/metadata/config_server_metadata.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_egress_metadata_hook.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace rpc {

ShardingEgressMetadataHook::ShardingEgressMetadataHook(ServiceContext* serviceContext)
    : _serviceContext(serviceContext) {
    invariant(_serviceContext);
}

Status ShardingEgressMetadataHook::writeRequestMetadata(OperationContext* opCtx,
                                                        BSONObjBuilder* metadataBob) {
    try {
        audit::writeImpersonatedUsersToMetadata(opCtx, metadataBob);
        ClientMetadataIsMasterState::writeToMetadata(opCtx, metadataBob);
        rpc::ConfigServerMetadata(_getConfigServerOpTime()).writeToMetadata(metadataBob);
        return Status::OK();
    } catch (...) {
        return exceptionToStatus();
    }
}

Status ShardingEgressMetadataHook::readReplyMetadata(OperationContext* opCtx,
                                                     StringData replySource,
                                                     const BSONObj& metadataObj) {
    try {
        _saveGLEStats(metadataObj, replySource);
        return _advanceConfigOpTimeFromShard(opCtx, replySource.toString(), metadataObj);
    } catch (...) {
        return exceptionToStatus();
    }
}

Status ShardingEgressMetadataHook::_advanceConfigOpTimeFromShard(OperationContext* opCtx,
                                                                 const ShardId& shardId,
                                                                 const BSONObj& metadataObj) {
    auto const grid = Grid::get(_serviceContext);

    try {
        auto shard = grid->shardRegistry()->getShardNoReload(shardId);
        if (!shard) {
            return Status::OK();
        }

        // Update our notion of the config server opTime from the configOpTime in the response.
        if (shard->isConfig()) {
            // Config servers return the config opTime as part of their own metadata.
            if (metadataObj.hasField(rpc::kReplSetMetadataFieldName)) {
                auto parseStatus = rpc::ReplSetMetadata::readFromMetadata(metadataObj);
                if (!parseStatus.isOK()) {
                    return parseStatus.getStatus();
                }

                // Use the last committed optime to advance config optime.
                // For successful majority writes, we could use the optime of the last op
                // from us and lastOpCommitted is always greater than or equal to it.
                // On majority write failures, the last visible optime would be incorrect
                // due to rollback as explained in SERVER-24630 and the last committed optime
                // is safe to use.
                const auto& replMetadata = parseStatus.getValue();
                const auto opTime = replMetadata.getLastOpCommitted();
                grid->advanceConfigOpTime(opCtx, opTime, "reply from config server node");
            }
        } else {
            // Regular shards return the config opTime as part of ConfigServerMetadata.
            auto parseStatus = rpc::ConfigServerMetadata::readFromMetadata(metadataObj);
            if (!parseStatus.isOK()) {
                return parseStatus.getStatus();
            }

            const auto& configMetadata = parseStatus.getValue();
            const auto opTime = configMetadata.getOpTime();
            if (opTime.is_initialized()) {
                grid->advanceConfigOpTime(opCtx,
                                          opTime.get(),
                                          str::stream() << "reply from shard " << shardId
                                                        << " node");
            }
        }
        return Status::OK();
    } catch (...) {
        return exceptionToStatus();
    }
}

}  // namespace rpc
}  // namespace mongo

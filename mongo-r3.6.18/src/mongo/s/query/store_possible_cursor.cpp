
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

#include "mongo/platform/basic.h"

#include "mongo/s/query/store_possible_cursor.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/s/query/cluster_client_cursor_impl.h"
#include "mongo/s/query/cluster_client_cursor_params.h"
#include "mongo/s/query/cluster_cursor_manager.h"

namespace mongo {

StatusWith<BSONObj> storePossibleCursor(OperationContext* opCtx,
                                        const ShardId& shardId,
                                        const HostAndPort& server,
                                        const BSONObj& cmdResult,
                                        const NamespaceString& requestedNss,
                                        executor::TaskExecutor* executor,
                                        ClusterCursorManager* cursorManager,
                                        TailableMode tailableMode) {
    if (!cmdResult["ok"].trueValue() || !cmdResult.hasField("cursor")) {
        return cmdResult;
    }

    auto incomingCursorResponse = CursorResponse::parseFromBSON(cmdResult);
    if (!incomingCursorResponse.isOK()) {
        return incomingCursorResponse.getStatus();
    }

    if (incomingCursorResponse.getValue().getCursorId() == CursorId(0)) {
        return cmdResult;
    }

    ClusterClientCursorParams params(
        incomingCursorResponse.getValue().getNSS(),
        AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames());
    params.remotes.emplace_back(shardId,
                                server,
                                CursorResponse(incomingCursorResponse.getValue().getNSS(),
                                               incomingCursorResponse.getValue().getCursorId(),
                                               {}));
    params.tailableMode = tailableMode;

    auto ccc = ClusterClientCursorImpl::make(opCtx, executor, std::move(params));

    // We don't expect to use this cursor until a subsequent getMore, so detach from the current
    // OperationContext until then.
    ccc->detachFromOperationContext();
    auto clusterCursorId =
        cursorManager->registerCursor(opCtx,
                                      ccc.releaseCursor(),
                                      requestedNss,
                                      ClusterCursorManager::CursorType::SingleTarget,
                                      ClusterCursorManager::CursorLifetime::Mortal);
    if (!clusterCursorId.isOK()) {
        return clusterCursorId.getStatus();
    }

    CursorResponse outgoingCursorResponse(
        requestedNss, clusterCursorId.getValue(), incomingCursorResponse.getValue().getBatch());
    return outgoingCursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
}

}  // namespace mongo

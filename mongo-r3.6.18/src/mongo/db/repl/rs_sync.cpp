
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/rs_sync.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/sync_tail.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

RSDataSync::RSDataSync(BackgroundSync* bgsync, ReplicationCoordinator* replCoord)
    : _bgsync(bgsync), _replCoord(replCoord) {}

RSDataSync::~RSDataSync() {
    DESTRUCTOR_GUARD(join(););
}

void RSDataSync::startup() {
    invariant(!_runThread.joinable());
    _runThread = stdx::thread(&RSDataSync::_run, this);
}

void RSDataSync::join() {
    if (_runThread.joinable()) {
        invariant(_bgsync->inShutdown());
        _runThread.join();
    }
}

void RSDataSync::_run() {
    Client::initThread("rsSync");
    AuthorizationSession::get(cc())->grantInternalAuthorization(&cc());

    // Overwrite prefetch index mode in BackgroundSync if ReplSettings has a mode set.
    auto&& replSettings = _replCoord->getSettings();
    if (replSettings.isPrefetchIndexModeSet())
        _replCoord->setIndexPrefetchConfig(replSettings.getPrefetchIndexMode());

    // We don't start data replication for arbiters at all and it's not allowed to reconfig
    // arbiterOnly field for any member.
    invariant(!_replCoord->getMemberState().arbiter());

    try {
        // Once we call into SyncTail::oplogApplication we never return, so this code only runs at
        // startup.
        SyncTail(_bgsync, multiSyncApply).oplogApplication(_replCoord);
    } catch (...) {
        auto status = exceptionToStatus();
        severe() << "Exception thrown in RSDataSync: " << redact(status);
        std::terminate();
    }
}

}  // namespace repl
}  // namespace mongo

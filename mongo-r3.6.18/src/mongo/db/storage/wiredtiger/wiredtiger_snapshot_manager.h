
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

#pragma once

#include <boost/optional.hpp>
#include <wiredtiger.h>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class WiredTigerOplogManager;

class WiredTigerSnapshotManager final : public SnapshotManager {
    MONGO_DISALLOW_COPYING(WiredTigerSnapshotManager);

public:
    explicit WiredTigerSnapshotManager(WT_CONNECTION* conn) {
        invariantWTOK(conn->open_session(conn, NULL, NULL, &_session));
        _conn = conn;
    }

    ~WiredTigerSnapshotManager() {
        shutdown();
    }

    Status prepareForCreateSnapshot(OperationContext* opCtx) final;
    void setCommittedSnapshot(const Timestamp& timestamp) final;
    void cleanupUnneededSnapshots() final;
    void dropAllSnapshots() final;

    //
    // WT-specific methods
    //

    /**
     * Prepares for a shutdown of the WT_CONNECTION.
     */
    void shutdown();

    /**
     * Sets the read timestamp on a transaction.
     *
     * Reads will be reflect the state of data as of the specified timestamp.
     */
    Status setTransactionReadTimestamp(Timestamp pointInTime, WT_SESSION* session) const;

    /**
     * Starts a transaction and returns the SnapshotName used.
     *
     * Throws if there is currently no committed snapshot.
     */
    Timestamp beginTransactionOnCommittedSnapshot(WT_SESSION* session) const;

    /**
     * Starts a transaction on the oplog using an appropriate timestamp for oplog visiblity.
     */
    void beginTransactionOnOplog(WiredTigerOplogManager* oplogManager, WT_SESSION* session) const;

    /**
     * Returns lowest SnapshotName that could possibly be used by a future call to
     * beginTransactionOnCommittedSnapshot, or boost::none if there is currently no committed
     * snapshot.
     *
     * This should not be used for starting a transaction on this SnapshotName since the named
     * snapshot may be deleted by the time you start the transaction.
     */
    boost::optional<Timestamp> getMinSnapshotForNextCommittedRead() const;

private:
    mutable stdx::mutex _mutex;  // Guards all members.
    boost::optional<Timestamp> _committedSnapshot;
    WT_SESSION* _session;
    WT_CONNECTION* _conn;
};
}

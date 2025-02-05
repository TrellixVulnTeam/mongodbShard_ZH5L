
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

#include "mongo/db/concurrency/locker.h"

namespace mongo {

/**
 * Locker, which cannot be used to lock/unlock resources and just returns true for checks for
 * whether a particular resource is locked. Do not use it for cases where actual locking
 * behaviour is expected or locking is performed.
 */
class LockerNoop : public Locker {
public:
    LockerNoop() {}

    virtual bool isNoop() const {
        return true;
    }

    virtual ClientState getClientState() const {
        invariant(false);
    }

    virtual LockerId getId() const {
        invariant(false);
    }

    stdx::thread::id getThreadId() const override {
        invariant(false);
    }

    virtual LockResult lockGlobal(LockMode mode) {
        invariant(false);
    }

    virtual LockResult lockGlobalBegin(LockMode mode, Milliseconds timeout) {
        invariant(false);
    }

    virtual LockResult lockGlobalComplete(Milliseconds timeout) {
        invariant(false);
    }

    virtual void lockMMAPV1Flush() {
        invariant(false);
    }

    virtual bool unlockGlobal() {
        invariant(false);
    }

    virtual void downgradeGlobalXtoSForMMAPV1() {
        invariant(false);
    }

    virtual void beginWriteUnitOfWork() {}

    virtual void endWriteUnitOfWork() {}

    virtual bool inAWriteUnitOfWork() const {
        invariant(false);
    }

    virtual LockResult lock(ResourceId resId,
                            LockMode mode,
                            Milliseconds timeout,
                            bool checkDeadlock) {
        return LockResult::LOCK_OK;
    }

    virtual void downgrade(ResourceId resId, LockMode newMode) {
        invariant(false);
    }

    virtual bool unlock(ResourceId resId) {
        return true;
    }

    virtual LockMode getLockMode(ResourceId resId) const {
        invariant(false);
    }

    virtual bool isLockHeldForMode(ResourceId resId, LockMode mode) const {
        return true;
    }

    virtual bool isDbLockedForMode(StringData dbName, LockMode mode) const {
        return true;
    }

    virtual bool isCollectionLockedForMode(StringData ns, LockMode mode) const {
        return true;
    }

    virtual ResourceId getWaitingResource() const {
        invariant(false);
    }

    virtual void getLockerInfo(LockerInfo* lockerInfo,
                               boost::optional<SingleThreadedLockStats> lockStatsBase) const {
        invariant(false);
    }

    virtual bool saveLockStateAndUnlock(LockSnapshot* stateOut) {
        invariant(false);
    }

    virtual void restoreLockState(const LockSnapshot& stateToRestore) {
        invariant(false);
    }

    virtual void dump() const {
        invariant(false);
    }

    virtual bool isW() const {
        invariant(false);
    }

    virtual bool isR() const {
        invariant(false);
    }

    virtual bool isLocked() const {
        return false;
    }

    virtual bool isWriteLocked() const {
        return false;
    }

    virtual bool isReadLocked() const {
        invariant(false);
    }

    virtual bool hasLockPending() const {
        invariant(false);
    }

    bool isGlobalLockedRecursively() override {
        return false;
    }
};

}  // namespace mongo

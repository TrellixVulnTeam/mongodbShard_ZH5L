
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/global_lock_acquisition_tracker.h"
#include "mongo/db/concurrency/lock_manager_test_help.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/log.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

const int kMaxStressThreads = 32;  // max number of threads to use for lock stress

/**
 * A RAII object that instantiates a TicketHolder that limits number of allowed global lock
 * acquisitions to numTickets. The opCtx must live as long as the UseGlobalThrottling instance.
 */
class UseGlobalThrottling {
public:
    explicit UseGlobalThrottling(OperationContext* opCtx, int numTickets)
        : _opCtx(opCtx), _holder(numTickets) {
        _opCtx->lockState()->setGlobalThrottling(&_holder, &_holder);
    }
    ~UseGlobalThrottling() noexcept(false) {
        // Reset the global setting as we're about to destroy the ticket holder.
        _opCtx->lockState()->setGlobalThrottling(nullptr, nullptr);
        ASSERT_EQ(_holder.used(), 0);
    }

private:
    OperationContext* _opCtx;
    TicketHolder _holder;
};


class DConcurrencyTestFixture : public unittest::Test {
public:
    DConcurrencyTestFixture() : _client(getGlobalServiceContext()->makeClient("testClient")) {}
    ~DConcurrencyTestFixture() {}

    /**
     * Constructs and returns a new OperationContext.
     */
    ServiceContext::UniqueOperationContext makeOpCtx() const {
        auto opCtx = _client->makeOperationContext();
        opCtx->releaseLockState();
        return opCtx;
    }

    /**
     * Returns a vector of Clients of length 'k', each of which has an OperationContext with its
     * lockState set to a DefaultLockerImpl.
     */
    template <typename LockerType>
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
    makeKClientsWithLockers(int k) {
        std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
            clients;
        clients.reserve(k);
        for (int i = 0; i < k; ++i) {
            auto client = getGlobalServiceContext()->makeClient(
                str::stream() << "test client for thread " << i);
            auto opCtx = client->makeOperationContext();
            opCtx->releaseLockState();
            opCtx->setLockState(stdx::make_unique<LockerType>());
            clients.emplace_back(std::move(client), std::move(opCtx));
        }
        return clients;
    }

private:
    ServiceContext::UniqueClient _client;
};


TEST_F(DConcurrencyTestFixture, WriteConflictRetryInstantiatesOK) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    writeConflictRetry(opCtx.get(), "", "", [] {});
}

TEST_F(DConcurrencyTestFixture, WriteConflictRetryRetriesFunctionOnWriteConflictException) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    auto&& opDebug = CurOp::get(opCtx.get())->debug();
    ASSERT_EQUALS(0LL, opDebug.writeConflicts);
    ASSERT_EQUALS(100, writeConflictRetry(opCtx.get(), "", "", [&opDebug] {
                      if (opDebug.writeConflicts == 0LL) {
                          throw WriteConflictException();
                      }
                      return 100;
                  }));
    ASSERT_EQUALS(1LL, opDebug.writeConflicts);
}

TEST_F(DConcurrencyTestFixture, WriteConflictRetryPropagatesNonWriteConflictException) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    ASSERT_THROWS_CODE(writeConflictRetry(opCtx.get(),
                                          "",
                                          "",
                                          [] {
                                              uassert(ErrorCodes::OperationFailed, "", false);
                                              MONGO_UNREACHABLE;
                                          }),
                       AssertionException,
                       ErrorCodes::OperationFailed);
}

TEST_F(DConcurrencyTestFixture,
       WriteConflictRetryPropagatesWriteConflictExceptionIfAlreadyInAWriteUnitOfWork) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    Lock::GlobalWrite globalWrite(opCtx.get());
    WriteUnitOfWork wuow(opCtx.get());
    ASSERT_THROWS(writeConflictRetry(opCtx.get(), "", "", [] { throw WriteConflictException(); }),
                  WriteConflictException);
}

TEST_F(DConcurrencyTestFixture, ResourceMutex) {
    Lock::ResourceMutex mtx("testMutex");
    DefaultLockerImpl locker1;
    DefaultLockerImpl locker2;
    DefaultLockerImpl locker3;

    struct State {
        void check(int n) {
            ASSERT_EQ(step.load(), n);
        }
        void finish(int n) {
            auto actual = step.fetchAndAdd(1);
            ASSERT_EQ(actual, n);
        }
        void waitFor(stdx::function<bool()> cond) {
            while (!cond())
                sleepmillis(0);
        }
        void waitFor(int n) {
            waitFor([this, n]() { return this->step.load() == n; });
        }
        AtomicInt32 step{0};
    } state;

    stdx::thread t1([&]() {
        // Step 0: Single thread acquires shared lock
        state.waitFor(0);
        Lock::SharedLock lk(&locker1, mtx);
        ASSERT(lk.isLocked());
        state.finish(0);

        // Step 4: Wait for t2 to regain its shared lock
        {
            // Check that TempRelease does not actually unlock anything
            Lock::TempRelease yield(&locker1);

            state.waitFor(4);
            state.waitFor([&locker2]() { return locker2.getWaitingResource().isValid(); });
            state.finish(4);
        }

        // Step 5: After t2 becomes blocked, unlock, yielding the mutex to t3
        lk.unlock();
        ASSERT(!lk.isLocked());
    });
    stdx::thread t2([&]() {
        // Step 1: Two threads acquire shared lock
        state.waitFor(1);
        Lock::SharedLock lk(&locker2, mtx);
        ASSERT(lk.isLocked());
        state.finish(1);

        // Step 2: Wait for t3 to attempt the exclusive lock
        state.waitFor([&locker3]() { return locker3.getWaitingResource().isValid(); });
        state.finish(2);

        // Step 3: Yield shared lock
        lk.unlock();
        ASSERT(!lk.isLocked());
        state.finish(3);

        // Step 4: Try to regain the shared lock // transfers control to t1
        lk.lock(MODE_IS);

        // Step 6: CHeck we actually got back the shared lock
        ASSERT(lk.isLocked());
        state.check(6);
    });
    stdx::thread t3([&]() {
        // Step 2: Third thread attempts to acquire exclusive lock
        state.waitFor(2);
        Lock::ExclusiveLock lk(&locker3, mtx);  // transfers control to t2

        // Step 5: Actually get the exclusive lock
        ASSERT(lk.isLocked());
        state.finish(5);
    });
    t1.join();
    t2.join();
    t3.join();
}

TEST_F(DConcurrencyTestFixture, GlobalRead) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    Lock::GlobalRead globalRead(opCtx.get());
    ASSERT(opCtx->lockState()->isR());
}

TEST_F(DConcurrencyTestFixture, GlobalWrite) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    Lock::GlobalWrite globalWrite(opCtx.get());
    ASSERT(opCtx->lockState()->isW());
}

TEST_F(DConcurrencyTestFixture, GlobalWriteAndGlobalRead) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    auto lockState = opCtx->lockState();

    Lock::GlobalWrite globalWrite(opCtx.get());
    ASSERT(lockState->isW());

    {
        Lock::GlobalRead globalRead(opCtx.get());
        ASSERT(lockState->isW());
    }

    ASSERT(lockState->isW());
}

TEST_F(DConcurrencyTestFixture,
       GlobalWriteRequiresExplicitDowngradeToIntentWriteModeIfDestroyedWhileHoldingDatabaseLock) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    auto lockState = opCtx->lockState();

    const ResourceId globalId(RESOURCE_GLOBAL, ResourceId::SINGLETON_GLOBAL);
    const ResourceId mmapId(RESOURCE_MMAPV1_FLUSH, ResourceId::SINGLETON_MMAPV1_FLUSH);

    auto globalWrite = stdx::make_unique<Lock::GlobalWrite>(opCtx.get());
    ASSERT(lockState->isW());
    ASSERT(MODE_X == lockState->getLockMode(globalId))
        << "unexpected global lock mode " << modeName(lockState->getLockMode(globalId));
    ASSERT(MODE_IX == lockState->getLockMode(mmapId)) << "unexpected MMAPv1 flush lock mode "
                                                      << modeName(lockState->getLockMode(mmapId));

    {
        Lock::DBLock dbWrite(opCtx.get(), "db", MODE_IX);
        ASSERT(lockState->isW());
        ASSERT(MODE_X == lockState->getLockMode(globalId))
            << "unexpected global lock mode " << modeName(lockState->getLockMode(globalId));
        ASSERT(MODE_IX == lockState->getLockMode(mmapId))
            << "unexpected MMAPv1 flush lock mode " << modeName(lockState->getLockMode(mmapId));

        // If we destroy the GlobalWrite out of order relative to the DBLock, we will leave the
        // global lock resource locked in MODE_X. We have to explicitly downgrade this resource to
        // MODE_IX to allow other write operations to make progress.
        // This test case illustrates non-recommended usage of the RAII types. See SERVER-30948.
        globalWrite = {};
        ASSERT(lockState->isW());
        lockState->downgrade(globalId, MODE_IX);
        ASSERT_FALSE(lockState->isW());
        ASSERT(lockState->isWriteLocked());
        ASSERT(MODE_IX == lockState->getLockMode(globalId))
            << "unexpected global lock mode " << modeName(lockState->getLockMode(globalId));
        ASSERT(MODE_IX == lockState->getLockMode(mmapId))
            << "unexpected MMAPv1 flush lock mode " << modeName(lockState->getLockMode(mmapId));
    }


    ASSERT_FALSE(lockState->isW());
    ASSERT_FALSE(lockState->isWriteLocked());
    ASSERT(MODE_NONE == lockState->getLockMode(globalId))
        << "unexpected global lock mode " << modeName(lockState->getLockMode(globalId));
    ASSERT(MODE_NONE == lockState->getLockMode(mmapId)) << "unexpected MMAPv1 flush lock mode "
                                                        << modeName(lockState->getLockMode(mmapId));
}

TEST_F(DConcurrencyTestFixture,
       GlobalWriteRequiresSupportsDowngradeToIntentWriteModeWhileHoldingDatabaseLock) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    auto lockState = opCtx->lockState();

    const ResourceId globalId(RESOURCE_GLOBAL, ResourceId::SINGLETON_GLOBAL);
    const ResourceId mmapId(RESOURCE_MMAPV1_FLUSH, ResourceId::SINGLETON_MMAPV1_FLUSH);

    auto globalWrite = stdx::make_unique<Lock::GlobalWrite>(opCtx.get());
    ASSERT(lockState->isW());
    ASSERT(MODE_X == lockState->getLockMode(globalId))
        << "unexpected global lock mode " << modeName(lockState->getLockMode(globalId));
    ASSERT(MODE_IX == lockState->getLockMode(mmapId)) << "unexpected MMAPv1 flush lock mode "
                                                      << modeName(lockState->getLockMode(mmapId));

    {
        Lock::DBLock dbWrite(opCtx.get(), "db", MODE_IX);
        ASSERT(lockState->isW());
        ASSERT(MODE_X == lockState->getLockMode(globalId))
            << "unexpected global lock mode " << modeName(lockState->getLockMode(globalId));
        ASSERT(MODE_IX == lockState->getLockMode(mmapId))
            << "unexpected MMAPv1 flush lock mode " << modeName(lockState->getLockMode(mmapId));

        // Downgrade global lock resource to MODE_IX to allow other write operations to make
        // progress.
        lockState->downgrade(globalId, MODE_IX);
        ASSERT_FALSE(lockState->isW());
        ASSERT(lockState->isWriteLocked());
        ASSERT(MODE_IX == lockState->getLockMode(globalId))
            << "unexpected global lock mode " << modeName(lockState->getLockMode(globalId));
        ASSERT(MODE_IX == lockState->getLockMode(mmapId))
            << "unexpected MMAPv1 flush lock mode " << modeName(lockState->getLockMode(mmapId));
    }

    ASSERT_FALSE(lockState->isW());
    ASSERT(lockState->isWriteLocked());

    globalWrite = {};
    ASSERT_FALSE(lockState->isW());
    ASSERT_FALSE(lockState->isWriteLocked());
    ASSERT(MODE_NONE == lockState->getLockMode(globalId))
        << "unexpected global lock mode " << modeName(lockState->getLockMode(globalId));
    ASSERT(MODE_NONE == lockState->getLockMode(mmapId)) << "unexpected MMAPv1 flush lock mode "
                                                        << modeName(lockState->getLockMode(mmapId));
}

TEST_F(DConcurrencyTestFixture,
       NestedGlobalWriteSupportsDowngradeToIntentWriteModeWhileHoldingDatabaseLock) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    auto lockState = opCtx->lockState();

    const ResourceId globalId(RESOURCE_GLOBAL, ResourceId::SINGLETON_GLOBAL);
    const ResourceId mmapId(RESOURCE_MMAPV1_FLUSH, ResourceId::SINGLETON_MMAPV1_FLUSH);

    auto outerGlobalWrite = stdx::make_unique<Lock::GlobalWrite>(opCtx.get());
    auto innerGlobalWrite = stdx::make_unique<Lock::GlobalWrite>(opCtx.get());

    {
        Lock::DBLock dbWrite(opCtx.get(), "db", MODE_IX);
        ASSERT(lockState->isW());
        ASSERT(MODE_X == lockState->getLockMode(globalId))
            << "unexpected global lock mode " << modeName(lockState->getLockMode(globalId));
        ASSERT(MODE_IX == lockState->getLockMode(mmapId))
            << "unexpected MMAPv1 flush lock mode " << modeName(lockState->getLockMode(mmapId));

        // Downgrade global lock resource to MODE_IX to allow other write operations to make
        // progress.
        lockState->downgrade(globalId, MODE_IX);
        ASSERT_FALSE(lockState->isW());
        ASSERT(lockState->isWriteLocked());
        ASSERT(MODE_IX == lockState->getLockMode(globalId))
            << "unexpected global lock mode " << modeName(lockState->getLockMode(globalId));
        ASSERT(MODE_IX == lockState->getLockMode(mmapId))
            << "unexpected MMAPv1 flush lock mode " << modeName(lockState->getLockMode(mmapId));
    }

    ASSERT_FALSE(lockState->isW());
    ASSERT(lockState->isWriteLocked());

    innerGlobalWrite = {};
    ASSERT_FALSE(lockState->isW());
    ASSERT(lockState->isWriteLocked());
    ASSERT(MODE_IX == lockState->getLockMode(globalId))
        << "unexpected global lock mode " << modeName(lockState->getLockMode(globalId));
    ASSERT(MODE_IX == lockState->getLockMode(mmapId)) << "unexpected MMAPv1 flush lock mode "
                                                      << modeName(lockState->getLockMode(mmapId));

    outerGlobalWrite = {};
    ASSERT_FALSE(lockState->isW());
    ASSERT_FALSE(lockState->isWriteLocked());
    ASSERT(MODE_NONE == lockState->getLockMode(globalId))
        << "unexpected global lock mode " << modeName(lockState->getLockMode(globalId));
    ASSERT(MODE_NONE == lockState->getLockMode(mmapId)) << "unexpected MMAPv1 flush lock mode "
                                                        << modeName(lockState->getLockMode(mmapId));
}

TEST_F(DConcurrencyTestFixture, GlobalLockS_Timeout) {
    auto clients = makeKClientsWithLockers<MMAPV1LockerImpl>(2);

    Lock::GlobalLock globalWrite(clients[0].second.get(), MODE_X, 0);
    ASSERT(globalWrite.isLocked());

    Lock::GlobalLock globalReadTry(clients[1].second.get(), MODE_S, 1);
    ASSERT(!globalReadTry.isLocked());
}

TEST_F(DConcurrencyTestFixture, GlobalLockX_Timeout) {
    auto clients = makeKClientsWithLockers<MMAPV1LockerImpl>(2);
    Lock::GlobalLock globalWrite(clients[0].second.get(), MODE_X, 0);
    ASSERT(globalWrite.isLocked());

    Lock::GlobalLock globalWriteTry(clients[1].second.get(), MODE_X, 1);
    ASSERT(!globalWriteTry.isLocked());
}

TEST_F(DConcurrencyTestFixture, GlobalLockXSetsGlobalLockTakenOnOperationContext) {
    auto clients = makeKClientsWithLockers<MMAPV1LockerImpl>(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(GlobalLockAcquisitionTracker::get(opCtx).getGlobalExclusiveLockTaken());

    {
        Lock::GlobalLock globalWrite(opCtx, MODE_X, 0);
        ASSERT(globalWrite.isLocked());
    }
    ASSERT_TRUE(GlobalLockAcquisitionTracker::get(opCtx).getGlobalExclusiveLockTaken());
}

TEST_F(DConcurrencyTestFixture, GlobalLockIXSetsGlobalLockTakenOnOperationContext) {
    auto clients = makeKClientsWithLockers<MMAPV1LockerImpl>(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(GlobalLockAcquisitionTracker::get(opCtx).getGlobalExclusiveLockTaken());
    {
        Lock::GlobalLock globalWrite(opCtx, MODE_IX, 0);
        ASSERT(globalWrite.isLocked());
    }
    ASSERT_TRUE(GlobalLockAcquisitionTracker::get(opCtx).getGlobalExclusiveLockTaken());
}

TEST_F(DConcurrencyTestFixture, GlobalLockSDoesNotSetGlobalLockTakenOnOperationContext) {
    auto clients = makeKClientsWithLockers<MMAPV1LockerImpl>(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(GlobalLockAcquisitionTracker::get(opCtx).getGlobalExclusiveLockTaken());
    {
        Lock::GlobalLock globalRead(opCtx, MODE_S, 0);
        ASSERT(globalRead.isLocked());
    }
    ASSERT_FALSE(GlobalLockAcquisitionTracker::get(opCtx).getGlobalExclusiveLockTaken());
}

TEST_F(DConcurrencyTestFixture, GlobalLockISDoesNotSetGlobalLockTakenOnOperationContext) {
    auto clients = makeKClientsWithLockers<MMAPV1LockerImpl>(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(GlobalLockAcquisitionTracker::get(opCtx).getGlobalExclusiveLockTaken());
    {
        Lock::GlobalLock globalRead(opCtx, MODE_IS, 0);
        ASSERT(globalRead.isLocked());
    }
    ASSERT_FALSE(GlobalLockAcquisitionTracker::get(opCtx).getGlobalExclusiveLockTaken());
}

TEST_F(DConcurrencyTestFixture, DBLockXSetsGlobalLockTakenOnOperationContext) {
    auto clients = makeKClientsWithLockers<MMAPV1LockerImpl>(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(GlobalLockAcquisitionTracker::get(opCtx).getGlobalExclusiveLockTaken());

    { Lock::DBLock dbWrite(opCtx, "db", MODE_X); }
    ASSERT_TRUE(GlobalLockAcquisitionTracker::get(opCtx).getGlobalExclusiveLockTaken());
}

TEST_F(DConcurrencyTestFixture, DBLockSDoesNotSetGlobalLockTakenOnOperationContext) {
    auto clients = makeKClientsWithLockers<MMAPV1LockerImpl>(1);
    auto opCtx = clients[0].second.get();
    ASSERT_FALSE(GlobalLockAcquisitionTracker::get(opCtx).getGlobalExclusiveLockTaken());

    { Lock::DBLock dbRead(opCtx, "db", MODE_S); }
    ASSERT_FALSE(GlobalLockAcquisitionTracker::get(opCtx).getGlobalExclusiveLockTaken());
}

TEST_F(DConcurrencyTestFixture, GlobalLockXDoesNotSetGlobalLockTakenWhenLockAcquisitionTimesOut) {
    auto clients = makeKClientsWithLockers<MMAPV1LockerImpl>(2);

    // Take a global lock so that the next one times out.
    Lock::GlobalLock globalWrite0(clients[0].second.get(), MODE_X, 0);
    ASSERT(globalWrite0.isLocked());

    auto opCtx = clients[1].second.get();
    ASSERT_FALSE(GlobalLockAcquisitionTracker::get(opCtx).getGlobalExclusiveLockTaken());
    {
        Lock::GlobalLock globalWrite1(opCtx, MODE_X, 1);
        ASSERT_FALSE(globalWrite1.isLocked());
    }
    ASSERT_FALSE(GlobalLockAcquisitionTracker::get(opCtx).getGlobalExclusiveLockTaken());
}

TEST_F(DConcurrencyTestFixture, GlobalLockS_NoTimeoutDueToGlobalLockS) {
    auto clients = makeKClientsWithLockers<MMAPV1LockerImpl>(2);

    Lock::GlobalRead globalRead(clients[0].second.get());
    Lock::GlobalLock globalReadTry(clients[1].second.get(), MODE_S, 1);

    ASSERT(globalReadTry.isLocked());
}

TEST_F(DConcurrencyTestFixture, GlobalLockX_TimeoutDueToGlobalLockS) {
    auto clients = makeKClientsWithLockers<MMAPV1LockerImpl>(2);

    Lock::GlobalRead globalRead(clients[0].second.get());
    Lock::GlobalLock globalWriteTry(clients[1].second.get(), MODE_X, 1);

    ASSERT(!globalWriteTry.isLocked());
}

TEST_F(DConcurrencyTestFixture, GlobalLockS_TimeoutDueToGlobalLockX) {
    auto clients = makeKClientsWithLockers<MMAPV1LockerImpl>(2);

    Lock::GlobalWrite globalWrite(clients[0].second.get());
    Lock::GlobalLock globalReadTry(clients[1].second.get(), MODE_S, 1);

    ASSERT(!globalReadTry.isLocked());
}

TEST_F(DConcurrencyTestFixture, GlobalLockX_TimeoutDueToGlobalLockX) {
    auto clients = makeKClientsWithLockers<MMAPV1LockerImpl>(2);

    Lock::GlobalWrite globalWrite(clients[0].second.get());
    Lock::GlobalLock globalWriteTry(clients[1].second.get(), MODE_X, 1);

    ASSERT(!globalWriteTry.isLocked());
}

TEST_F(DConcurrencyTestFixture, TempReleaseGlobalWrite) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    auto lockState = opCtx->lockState();
    Lock::GlobalWrite globalWrite(opCtx.get());

    {
        Lock::TempRelease tempRelease(lockState);
        ASSERT(!lockState->isLocked());
    }

    ASSERT(lockState->isW());
}

TEST_F(DConcurrencyTestFixture, TempReleaseRecursive) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    auto lockState = opCtx->lockState();
    Lock::GlobalWrite globalWrite(opCtx.get());
    Lock::DBLock lk(opCtx.get(), "SomeDBName", MODE_X);

    {
        Lock::TempRelease tempRelease(lockState);
        ASSERT(lockState->isW());
        ASSERT(lockState->isDbLockedForMode("SomeDBName", MODE_X));
    }

    ASSERT(lockState->isW());
}

TEST_F(DConcurrencyTestFixture, DBLockTakesS) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    Lock::DBLock dbRead(opCtx.get(), "db", MODE_S);

    const ResourceId resIdDb(RESOURCE_DATABASE, std::string("db"));
    ASSERT(opCtx->lockState()->getLockMode(resIdDb) == MODE_S);
}

TEST_F(DConcurrencyTestFixture, DBLockTakesX) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    Lock::DBLock dbWrite(opCtx.get(), "db", MODE_X);

    const ResourceId resIdDb(RESOURCE_DATABASE, std::string("db"));
    ASSERT(opCtx->lockState()->getLockMode(resIdDb) == MODE_X);
}

TEST_F(DConcurrencyTestFixture, DBLockTakesISForAdminIS) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    Lock::DBLock dbRead(opCtx.get(), "admin", MODE_IS);

    ASSERT(opCtx->lockState()->getLockMode(resourceIdAdminDB) == MODE_IS);
}

TEST_F(DConcurrencyTestFixture, DBLockTakesSForAdminS) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    Lock::DBLock dbRead(opCtx.get(), "admin", MODE_S);

    ASSERT(opCtx->lockState()->getLockMode(resourceIdAdminDB) == MODE_S);
}

TEST_F(DConcurrencyTestFixture, DBLockTakesXForAdminIX) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    Lock::DBLock dbWrite(opCtx.get(), "admin", MODE_IX);

    ASSERT(opCtx->lockState()->getLockMode(resourceIdAdminDB) == MODE_X);
}

TEST_F(DConcurrencyTestFixture, DBLockTakesXForAdminX) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    Lock::DBLock dbWrite(opCtx.get(), "admin", MODE_X);

    ASSERT(opCtx->lockState()->getLockMode(resourceIdAdminDB) == MODE_X);
}

TEST_F(DConcurrencyTestFixture, MultipleWriteDBLocksOnSameThread) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    Lock::DBLock r1(opCtx.get(), "db1", MODE_X);
    Lock::DBLock r2(opCtx.get(), "db1", MODE_X);

    ASSERT(opCtx->lockState()->isDbLockedForMode("db1", MODE_X));
}

TEST_F(DConcurrencyTestFixture, MultipleConflictingDBLocksOnSameThread) {
    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    auto lockState = opCtx->lockState();
    Lock::DBLock r1(opCtx.get(), "db1", MODE_X);
    Lock::DBLock r2(opCtx.get(), "db1", MODE_S);

    ASSERT(lockState->isDbLockedForMode("db1", MODE_X));
    ASSERT(lockState->isDbLockedForMode("db1", MODE_S));
}

TEST_F(DConcurrencyTestFixture, IsDbLockedForSMode) {
    const std::string dbName("db");

    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    auto lockState = opCtx->lockState();
    Lock::DBLock dbLock(opCtx.get(), dbName, MODE_S);

    ASSERT(lockState->isDbLockedForMode(dbName, MODE_IS));
    ASSERT(!lockState->isDbLockedForMode(dbName, MODE_IX));
    ASSERT(lockState->isDbLockedForMode(dbName, MODE_S));
    ASSERT(!lockState->isDbLockedForMode(dbName, MODE_X));
}

TEST_F(DConcurrencyTestFixture, IsDbLockedForXMode) {
    const std::string dbName("db");

    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    auto lockState = opCtx->lockState();
    Lock::DBLock dbLock(opCtx.get(), dbName, MODE_X);

    ASSERT(lockState->isDbLockedForMode(dbName, MODE_IS));
    ASSERT(lockState->isDbLockedForMode(dbName, MODE_IX));
    ASSERT(lockState->isDbLockedForMode(dbName, MODE_S));
    ASSERT(lockState->isDbLockedForMode(dbName, MODE_X));
}

TEST_F(DConcurrencyTestFixture, IsCollectionLocked_DB_Locked_IS) {
    const std::string ns("db1.coll");

    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    auto lockState = opCtx->lockState();

    Lock::DBLock dbLock(opCtx.get(), "db1", MODE_IS);

    {
        Lock::CollectionLock collLock(lockState, ns, MODE_IS);

        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_IS));
        ASSERT(!lockState->isCollectionLockedForMode(ns, MODE_IX));

        // TODO: This is TRUE because Lock::CollectionLock converts IS lock to S
        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_S));

        ASSERT(!lockState->isCollectionLockedForMode(ns, MODE_X));
    }

    {
        Lock::CollectionLock collLock(lockState, ns, MODE_S);

        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_IS));
        ASSERT(!lockState->isCollectionLockedForMode(ns, MODE_IX));
        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_S));
        ASSERT(!lockState->isCollectionLockedForMode(ns, MODE_X));
    }
}

TEST_F(DConcurrencyTestFixture, IsCollectionLocked_DB_Locked_IX) {
    const std::string ns("db1.coll");

    auto opCtx = makeOpCtx();
    opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    auto lockState = opCtx->lockState();

    Lock::DBLock dbLock(opCtx.get(), "db1", MODE_IX);

    {
        Lock::CollectionLock collLock(lockState, ns, MODE_IX);

        // TODO: This is TRUE because Lock::CollectionLock converts IX lock to X
        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_IS));

        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_IX));
        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_S));
        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_X));
    }

    {
        Lock::CollectionLock collLock(lockState, ns, MODE_X);

        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_IS));
        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_IX));
        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_S));
        ASSERT(lockState->isCollectionLockedForMode(ns, MODE_X));
    }
}

TEST_F(DConcurrencyTestFixture, Stress) {
    const int kNumIterations = 5000;

    ProgressMeter progressMeter(kNumIterations * kMaxStressThreads);
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        clients = makeKClientsWithLockers<DefaultLockerImpl>(kMaxStressThreads);

    AtomicInt32 ready{0};
    std::vector<stdx::thread> threads;


    for (int threadId = 0; threadId < kMaxStressThreads; threadId++) {
        threads.emplace_back([&, threadId]() {
            // Busy-wait until everybody is ready
            ready.fetchAndAdd(1);
            while (ready.load() < kMaxStressThreads)
                ;

            for (int i = 0; i < kNumIterations; i++) {
                const bool sometimes = (std::rand() % 15 == 0);

                if (i % 7 == 0 && threadId == 0 /* Only one upgrader legal */) {
                    Lock::GlobalWrite w(clients[threadId].second.get());
                    if (i % 7 == 2) {
                        Lock::TempRelease t(clients[threadId].second->lockState());
                    }

                    ASSERT(clients[threadId].second->lockState()->isW());
                } else if (i % 7 == 1) {
                    Lock::GlobalRead r(clients[threadId].second.get());
                    ASSERT(clients[threadId].second->lockState()->isReadLocked());
                } else if (i % 7 == 2) {
                    Lock::GlobalWrite w(clients[threadId].second.get());
                    if (sometimes) {
                        Lock::TempRelease t(clients[threadId].second->lockState());
                    }

                    ASSERT(clients[threadId].second->lockState()->isW());
                } else if (i % 7 == 3) {
                    Lock::GlobalWrite w(clients[threadId].second.get());
                    { Lock::TempRelease t(clients[threadId].second->lockState()); }

                    Lock::GlobalRead r(clients[threadId].second.get());
                    if (sometimes) {
                        Lock::TempRelease t(clients[threadId].second->lockState());
                    }

                    ASSERT(clients[threadId].second->lockState()->isW());
                } else if (i % 7 == 4) {
                    Lock::GlobalRead r(clients[threadId].second.get());
                    Lock::GlobalRead r2(clients[threadId].second.get());
                    ASSERT(clients[threadId].second->lockState()->isReadLocked());
                } else if (i % 7 == 5) {
                    { Lock::DBLock r(clients[threadId].second.get(), "foo", MODE_S); }
                    { Lock::DBLock r(clients[threadId].second.get(), "bar", MODE_S); }
                } else if (i % 7 == 6) {
                    if (i > kNumIterations / 2) {
                        int q = i % 11;

                        if (q == 0) {
                            Lock::DBLock r(clients[threadId].second.get(), "foo", MODE_S);
                            ASSERT(clients[threadId].second->lockState()->isDbLockedForMode(
                                "foo", MODE_S));

                            Lock::DBLock r2(clients[threadId].second.get(), "foo", MODE_S);
                            ASSERT(clients[threadId].second->lockState()->isDbLockedForMode(
                                "foo", MODE_S));

                            Lock::DBLock r3(clients[threadId].second.get(), "local", MODE_S);
                            ASSERT(clients[threadId].second->lockState()->isDbLockedForMode(
                                "foo", MODE_S));
                            ASSERT(clients[threadId].second->lockState()->isDbLockedForMode(
                                "local", MODE_S));
                        } else if (q == 1) {
                            // test locking local only -- with no preceding lock
                            { Lock::DBLock x(clients[threadId].second.get(), "local", MODE_S); }

                            Lock::DBLock x(clients[threadId].second.get(), "local", MODE_X);

                            if (sometimes) {
                                Lock::TempRelease t(clients[threadId].second.get()->lockState());
                            }
                        } else if (q == 2) {
                            { Lock::DBLock x(clients[threadId].second.get(), "admin", MODE_S); }
                            { Lock::DBLock x(clients[threadId].second.get(), "admin", MODE_X); }
                        } else if (q == 3) {
                            Lock::DBLock x(clients[threadId].second.get(), "foo", MODE_X);
                            Lock::DBLock y(clients[threadId].second.get(), "admin", MODE_S);
                        } else if (q == 4) {
                            Lock::DBLock x(clients[threadId].second.get(), "foo2", MODE_S);
                            Lock::DBLock y(clients[threadId].second.get(), "admin", MODE_S);
                        } else if (q == 5) {
                            Lock::DBLock x(clients[threadId].second.get(), "foo", MODE_IS);
                        } else if (q == 6) {
                            Lock::DBLock x(clients[threadId].second.get(), "foo", MODE_IX);
                            Lock::DBLock y(clients[threadId].second.get(), "local", MODE_IX);
                        } else {
                            Lock::DBLock w(clients[threadId].second.get(), "foo", MODE_X);

                            { Lock::TempRelease t(clients[threadId].second->lockState()); }

                            Lock::DBLock r2(clients[threadId].second.get(), "foo", MODE_S);
                            Lock::DBLock r3(clients[threadId].second.get(), "local", MODE_S);
                        }
                    } else {
                        Lock::DBLock r(clients[threadId].second.get(), "foo", MODE_S);
                        Lock::DBLock r2(clients[threadId].second.get(), "foo", MODE_S);
                        Lock::DBLock r3(clients[threadId].second.get(), "local", MODE_S);
                    }
                }

                progressMeter.hit();
            }
        });
    }

    for (auto& thread : threads)
        thread.join();

    auto newClients = makeKClientsWithLockers<MMAPV1LockerImpl>(2);
    { Lock::GlobalWrite w(newClients[0].second.get()); }
    { Lock::GlobalRead r(newClients[1].second.get()); }
}

TEST_F(DConcurrencyTestFixture, StressPartitioned) {
    const int kNumIterations = 5000;

    ProgressMeter progressMeter(kNumIterations * kMaxStressThreads);
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        clients = makeKClientsWithLockers<DefaultLockerImpl>(kMaxStressThreads);

    AtomicInt32 ready{0};
    std::vector<stdx::thread> threads;

    for (int threadId = 0; threadId < kMaxStressThreads; threadId++) {
        threads.emplace_back([&, threadId]() {
            // Busy-wait until everybody is ready
            ready.fetchAndAdd(1);
            while (ready.load() < kMaxStressThreads)
                ;

            for (int i = 0; i < kNumIterations; i++) {
                if (threadId == 0) {
                    if (i % 100 == 0) {
                        Lock::GlobalWrite w(clients[threadId].second.get());
                        continue;
                    } else if (i % 100 == 1) {
                        Lock::GlobalRead w(clients[threadId].second.get());
                        continue;
                    }

                    // Intentional fall through
                }

                if (i % 2 == 0) {
                    Lock::DBLock x(clients[threadId].second.get(), "foo", MODE_IS);
                } else {
                    Lock::DBLock x(clients[threadId].second.get(), "foo", MODE_IX);
                    Lock::DBLock y(clients[threadId].second.get(), "local", MODE_IX);
                }

                progressMeter.hit();
            }
        });
    }

    for (auto& thread : threads)
        thread.join();

    auto newClients = makeKClientsWithLockers<MMAPV1LockerImpl>(2);
    { Lock::GlobalWrite w(newClients[0].second.get()); }
    { Lock::GlobalRead r(newClients[1].second.get()); }
}

TEST_F(DConcurrencyTestFixture, ResourceMutexLabels) {
    Lock::ResourceMutex mutex("label");
    ASSERT(mutex.getName() == "label");
    Lock::ResourceMutex mutex2("label2");
    ASSERT(mutex2.getName() == "label2");
}

TEST_F(DConcurrencyTestFixture, Throttling) {
    auto clientOpctxPairs = makeKClientsWithLockers<DefaultLockerImpl>(2);
    auto opctx1 = clientOpctxPairs[0].second.get();
    auto opctx2 = clientOpctxPairs[1].second.get();
    UseGlobalThrottling throttle(opctx1, 1);

    bool overlongWait;
    int tries = 0;
    const int maxTries = 15;
    const int timeoutMillis = 42;

    do {
        // Test that throttling will correctly handle timeouts.
        Lock::GlobalRead R1(opctx1, 0);
        ASSERT(R1.isLocked());

        Date_t t1 = Date_t::now();
        {
            Lock::GlobalRead R2(opctx2, timeoutMillis);
            ASSERT(!R2.isLocked());
        }
        Date_t t2 = Date_t::now();

        // Test that the timeout did result in at least the requested wait.
        ASSERT_GTE(t2 - t1, Milliseconds(timeoutMillis));

        // Timeouts should be reasonably immediate. In maxTries attempts at least one test should be
        // able to complete within a second, as the theoretical test duration is less than 50 ms.
        overlongWait = t2 - t1 >= Seconds(1);
    } while (overlongWait && ++tries < maxTries);
    ASSERT(!overlongWait);
}

TEST_F(DConcurrencyTestFixture, NoThrottlingWhenNotAcquiringTickets) {
    auto clientOpctxPairs = makeKClientsWithLockers<DefaultLockerImpl>(2);
    auto opctx1 = clientOpctxPairs[0].second.get();
    auto opctx2 = clientOpctxPairs[1].second.get();
    // Limit the locker to 1 ticket at a time.
    UseGlobalThrottling throttle(opctx1, 1);

    // Prevent the enforcement of ticket throttling.
    opctx1->lockState()->setShouldAcquireTicket(false);

    // Both locks should be acquired immediately because there is no throttling.
    Lock::GlobalRead R1(opctx1, 0);
    ASSERT(R1.isLocked());

    Lock::GlobalRead R2(opctx2, 0);
    ASSERT(R2.isLocked());
}

TEST_F(DConcurrencyTestFixture, CompatibleFirstWithSXIS) {
    auto clientOpctxPairs = makeKClientsWithLockers<DefaultLockerImpl>(3);
    auto opctx1 = clientOpctxPairs[0].second.get();
    auto opctx2 = clientOpctxPairs[1].second.get();
    auto opctx3 = clientOpctxPairs[2].second.get();

    // Build a queue of MODE_S <- MODE_X <- MODE_IS, with MODE_S granted.
    Lock::GlobalRead lockS(opctx1);
    ASSERT(lockS.isLocked());
    Lock::GlobalLock lockX(opctx2, MODE_X, UINT_MAX, Lock::GlobalLock::EnqueueOnly());
    ASSERT(!lockX.isLocked());

    // A MODE_IS should be granted due to compatibleFirst policy.
    Lock::GlobalLock lockIS(opctx3, MODE_IS, 0);
    ASSERT(lockIS.isLocked());

    lockX.waitForLock(0);
    ASSERT(!lockX.isLocked());
}


TEST_F(DConcurrencyTestFixture, CompatibleFirstWithXSIXIS) {
    auto clientOpctxPairs = makeKClientsWithLockers<DefaultLockerImpl>(4);
    auto opctx1 = clientOpctxPairs[0].second.get();
    auto opctx2 = clientOpctxPairs[1].second.get();
    auto opctx3 = clientOpctxPairs[2].second.get();
    auto opctx4 = clientOpctxPairs[3].second.get();

    // Build a queue of MODE_X <- MODE_S <- MODE_IX <- MODE_IS, with MODE_X granted.
    boost::optional<Lock::GlobalWrite> lockX;
    lockX.emplace(opctx1);
    ASSERT(lockX->isLocked());
    boost::optional<Lock::GlobalLock> lockS;
    lockS.emplace(opctx2, MODE_S, UINT_MAX, Lock::GlobalLock::EnqueueOnly());
    ASSERT(!lockS->isLocked());
    Lock::GlobalLock lockIX(opctx3, MODE_IX, UINT_MAX, Lock::GlobalLock::EnqueueOnly());
    ASSERT(!lockIX.isLocked());
    Lock::GlobalLock lockIS(opctx4, MODE_IS, UINT_MAX, Lock::GlobalLock::EnqueueOnly());
    ASSERT(!lockIS.isLocked());


    // Now release the MODE_X and ensure that MODE_S will switch policy to compatibleFirst
    lockX.reset();
    lockS->waitForLock(0);
    ASSERT(lockS->isLocked());
    ASSERT(!lockIX.isLocked());
    lockIS.waitForLock(0);
    ASSERT(lockIS.isLocked());

    // Now release the MODE_S and ensure that MODE_IX gets locked.
    lockS.reset();
    lockIX.waitForLock(0);
    ASSERT(lockIX.isLocked());
}

TEST_F(DConcurrencyTestFixture, CompatibleFirstWithXSXIXIS) {
    auto clientOpctxPairs = makeKClientsWithLockers<DefaultLockerImpl>(5);
    auto opctx1 = clientOpctxPairs[0].second.get();
    auto opctx2 = clientOpctxPairs[1].second.get();
    auto opctx3 = clientOpctxPairs[2].second.get();
    auto opctx4 = clientOpctxPairs[3].second.get();
    auto opctx5 = clientOpctxPairs[4].second.get();

    // Build a queue of MODE_X <- MODE_S <- MODE_X <- MODE_IX <- MODE_IS, with the first MODE_X
    // granted and check that releasing it will result in the MODE_IS being granted.
    boost::optional<Lock::GlobalWrite> lockXgranted;
    lockXgranted.emplace(opctx1);
    ASSERT(lockXgranted->isLocked());

    boost::optional<Lock::GlobalLock> lockX;
    lockX.emplace(opctx3, MODE_X, UINT_MAX, Lock::GlobalLock::EnqueueOnly());
    ASSERT(!lockX->isLocked());

    // Now request MODE_S: it will be first in the pending list due to EnqueueAtFront policy.
    boost::optional<Lock::GlobalLock> lockS;
    lockS.emplace(opctx2, MODE_S, UINT_MAX, Lock::GlobalLock::EnqueueOnly());
    ASSERT(!lockS->isLocked());

    Lock::GlobalLock lockIX(opctx4, MODE_IX, UINT_MAX, Lock::GlobalLock::EnqueueOnly());
    ASSERT(!lockIX.isLocked());
    Lock::GlobalLock lockIS(opctx5, MODE_IS, UINT_MAX, Lock::GlobalLock::EnqueueOnly());
    ASSERT(!lockIS.isLocked());


    // Now release the granted MODE_X and ensure that MODE_S will switch policy to compatibleFirst,
    // not locking the MODE_X or MODE_IX, but instead granting the final MODE_IS.
    lockXgranted.reset();
    lockS->waitForLock(0);
    ASSERT(lockS->isLocked());

    lockX->waitForLock(0);
    ASSERT(!lockX->isLocked());
    lockIX.waitForLock(0);
    ASSERT(!lockIX.isLocked());

    lockIS.waitForLock(0);
    ASSERT(lockIS.isLocked());
}

TEST_F(DConcurrencyTestFixture, CompatibleFirstStress) {
    int numThreads = 8;
    int testMicros = 500'000;
    AtomicUInt64 readOnlyInterval{0};
    AtomicBool done{false};
    std::vector<uint64_t> acquisitionCount(numThreads);
    std::vector<uint64_t> timeoutCount(numThreads);
    std::vector<uint64_t> busyWaitCount(numThreads);
    auto clientOpctxPairs = makeKClientsWithLockers<DefaultLockerImpl>(numThreads);

    // Do some busy waiting to trigger different timings. The atomic load prevents compilers
    // from optimizing the loop away.
    auto busyWait = [&done, &busyWaitCount](int threadId, long long iters) {
        while (iters-- > 0) {
            for (int i = 0; i < 100 && !done.load(); i++) {
                busyWaitCount[threadId]++;
            }
        }
    };

    std::vector<stdx::thread> threads;

    // Thread putting state in/out of read-only CompatibleFirst mode.
    threads.emplace_back([&]() {
        Timer t;
        auto endTime = t.micros() + testMicros;
        uint64_t readOnlyIntervalCount = 0;
        OperationContext* opCtx = clientOpctxPairs[0].second.get();
        for (int iters = 0; (t.micros() < endTime); iters++) {
            busyWait(0, iters % 20);
            Lock::GlobalRead readLock(opCtx, iters % 2);
            if (!readLock.isLocked()) {
                timeoutCount[0]++;
                continue;
            }
            acquisitionCount[0]++;
            readOnlyInterval.store(++readOnlyIntervalCount);
            busyWait(0, iters % 200);
            readOnlyInterval.store(0);
        };
        done.store(true);
    });

    for (int threadId = 1; threadId < numThreads; threadId++) {
        threads.emplace_back([&, threadId]() {
            Timer t;
            for (int iters = 0; !done.load(); iters++) {
                OperationContext* opCtx = clientOpctxPairs[threadId].second.get();
                boost::optional<Lock::GlobalLock> lock;
                switch (threadId) {
                    case 1:
                    case 2:
                    case 3:
                    case 4: {
                        // Here, actually try to acquire a lock without waiting, and check whether
                        // we should have gotten the lock or not. Use MODE_IS in 95% of the cases,
                        // and MODE_S in only 5, as that stressing the partitioning scheme and
                        // policy changes more as thread 0 acquires/releases its MODE_S lock.
                        busyWait(threadId, iters % 100);
                        auto interval = readOnlyInterval.load();
                        lock.emplace(opCtx,
                                     iters % 20 ? MODE_IS : MODE_S,
                                     0,
                                     Lock::GlobalLock::EnqueueOnly());
                        // If thread 0 is holding the MODE_S lock while we tried to acquire a
                        // MODE_IS or MODE_S lock, the CompatibleFirst policy guarantees success.
                        auto newInterval = readOnlyInterval.load();
                        invariant(!interval || interval != newInterval || lock->isLocked());
                        lock->waitForLock(0);
                        break;
                    }
                    case 5:
                        busyWait(threadId, iters % 150);
                        lock.emplace(opCtx, MODE_X, iters % 2);
                        busyWait(threadId, iters % 10);
                        break;
                    case 6:
                        lock.emplace(opCtx, iters % 25 ? MODE_IX : MODE_S, iters % 2);
                        busyWait(threadId, iters % 100);
                        break;
                    case 7:
                        busyWait(threadId, iters % 100);
                        lock.emplace(opCtx, iters % 20 ? MODE_IS : MODE_X, 0);
                        break;
                    default:
                        MONGO_UNREACHABLE;
                }
                if (lock->isLocked())
                    acquisitionCount[threadId]++;
                else
                    timeoutCount[threadId]++;
            };
        });
    }

    for (auto& thread : threads)
        thread.join();
    for (int threadId = 0; threadId < numThreads; threadId++) {
        log() << "thread " << threadId << " stats: " << acquisitionCount[threadId]
              << " acquisitions, " << timeoutCount[threadId] << " timeouts, "
              << busyWaitCount[threadId] / 1'000'000 << "M busy waits";
    }
}


namespace {
class RecoveryUnitMock : public RecoveryUnitNoop {
public:
    virtual void abandonSnapshot() {
        activeTransaction = false;
    }

    bool activeTransaction = true;
};
}

TEST_F(DConcurrencyTestFixture, TestGlobalLockAbandonSnapshot) {
    auto clients = makeKClientsWithLockers<MMAPV1LockerImpl>(1);
    auto opCtx = clients[0].second.get();
    auto recovUnitOwned = stdx::make_unique<RecoveryUnitMock>();
    auto recovUnitBorrowed = recovUnitOwned.get();
    opCtx->setRecoveryUnit(recovUnitOwned.release(),
                           OperationContext::RecoveryUnitState::kActiveUnitOfWork);

    {
        Lock::GlobalLock gw1(opCtx, MODE_IS, 0);
        ASSERT(gw1.isLocked());
        ASSERT(recovUnitBorrowed->activeTransaction);

        {
            Lock::GlobalLock gw2(opCtx, MODE_S, 0);
            ASSERT(gw2.isLocked());
            ASSERT(recovUnitBorrowed->activeTransaction);
        }

        ASSERT(recovUnitBorrowed->activeTransaction);
        ASSERT(gw1.isLocked());
    }
    ASSERT_FALSE(recovUnitBorrowed->activeTransaction);
}

}  // namespace
}  // namespace mongo

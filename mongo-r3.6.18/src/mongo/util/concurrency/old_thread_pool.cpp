
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/util/concurrency/old_thread_pool.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

ThreadPool::Options makeOptions(int nThreads, const std::string& threadNamePrefix) {
    fassert(28706, nThreads > 0);
    ThreadPool::Options options;
    if (!threadNamePrefix.empty()) {
        options.threadNamePrefix = threadNamePrefix;
        options.poolName = str::stream() << threadNamePrefix << "Pool";
    }
    options.maxThreads = options.minThreads = static_cast<size_t>(nThreads);
    return options;
}

}  // namespace

OldThreadPool::OldThreadPool(int nThreads, const std::string& threadNamePrefix)
    : OldThreadPool(DoNotStartThreadsTag(), nThreads, threadNamePrefix) {
    startThreads();
}

OldThreadPool::OldThreadPool(const DoNotStartThreadsTag&,
                             int nThreads,
                             const std::string& threadNamePrefix)
    : _pool(makeOptions(nThreads, threadNamePrefix)) {}

OldThreadPool::OldThreadPool(ThreadPool::Options options) : _pool(std::move(options)) {}

std::size_t OldThreadPool::getNumThreads() const {
    return _pool.getStats().numThreads;
}

ThreadPool::Stats OldThreadPool::getStats() const {
    return _pool.getStats();
}

void OldThreadPool::startThreads() {
    _pool.startup();
}

void OldThreadPool::join() {
    _pool.waitForIdle();
}

void OldThreadPool::schedule(Task task) {
    fassert(28705, _pool.schedule(std::move(task)));
}

}  // namespace mongo

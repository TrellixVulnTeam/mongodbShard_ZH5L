
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

#include <map>

#include "mongo/db/service_context.h"

namespace mongo {

class Client;
class StorageEngineLockFile;

class ServiceContextMongoD final : public ServiceContext {
public:
    using FactoryMap = std::map<std::string, const StorageEngine::Factory*>;

    ServiceContextMongoD();

    ~ServiceContextMongoD();

    StorageEngine* getGlobalStorageEngine() override;

    void createLockFile();

    void initializeGlobalStorageEngine() override;

    void shutdownGlobalStorageEngineCleanly() override;

    void registerStorageEngine(const std::string& name,
                               const StorageEngine::Factory* factory) override;

    bool isRegisteredStorageEngine(const std::string& name) override;

    StorageFactoriesIterator* makeStorageFactoriesIterator() override;

private:
    std::unique_ptr<OperationContext> _newOpCtx(Client* client, unsigned opId) override;

    std::unique_ptr<StorageEngineLockFile> _lockFile;

    // logically owned here, but never deleted by anyone.
    StorageEngine* _storageEngine = nullptr;

    // All possible storage engines are registered here through MONGO_INIT.
    FactoryMap _storageFactories;
};

class StorageFactoriesIteratorMongoD final : public StorageFactoriesIterator {
public:
    typedef ServiceContextMongoD::FactoryMap::const_iterator FactoryMapIterator;

    StorageFactoriesIteratorMongoD(const FactoryMapIterator& begin, const FactoryMapIterator& end);

    bool more() const override;
    const StorageEngine::Factory* next() override;

private:
    FactoryMapIterator _curr;
    FactoryMapIterator _end;
};

}  // namespace mongo

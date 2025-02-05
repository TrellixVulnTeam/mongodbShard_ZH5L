
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

#include <memory>

#include "mongo/base/status_with.h"
#include "mongo/db/keys_collection_document.h"

namespace mongo {

class OperationContext;
class LogicalTime;
class ServiceContext;

extern int KeysRotationIntervalSec;
/**
 * This is responsible for providing keys that can be used for HMAC computation. This also supports
 * automatic key rotation that happens on a configurable interval.
 */
class KeysCollectionManager {
public:
    static const Seconds kKeyValidInterval;
    static const std::string kKeyManagerPurposeString;

    virtual ~KeysCollectionManager();

    /**
     * Return a key that is valid for the given time and also matches the keyId. Note that this call
     * can block if it will need to do a refresh and we are on a sharded cluster.
     *
     * Throws ErrorCode::ExceededTimeLimit if it times out.
     */
    virtual StatusWith<KeysCollectionDocument> getKeyForValidation(
        OperationContext* opCtx, long long keyId, const LogicalTime& forThisTime) = 0;

    /**
     * Returns a key that is valid for the given time. Note that unlike getKeyForValidation, this
     * will never do a refresh.
     *
     * Throws ErrorCode::ExceededTimeLimit if it times out.
     */
    virtual StatusWith<KeysCollectionDocument> getKeyForSigning(OperationContext* opCtx,
                                                                const LogicalTime& forThisTime) = 0;

    /**
     * Clears the in memory cache of the keys.
     */
    virtual void clearCache() = 0;
};

}  // namespace mongo

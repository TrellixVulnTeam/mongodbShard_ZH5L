
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

#include "mongo/transport/service_entry_point_mock.h"

#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/transport_layer.h"

namespace mongo {

using namespace transport;

ServiceEntryPointMock::ServiceEntryPointMock(transport::TransportLayer* tl)
    : _tl(tl), _inShutdown(false) {}

ServiceEntryPointMock::~ServiceEntryPointMock() {
    endAllSessions(transport::Session::kEmptyTagMask);
}

void ServiceEntryPointMock::startSession(transport::SessionHandle session) {
    _threads.emplace_back(&ServiceEntryPointMock::run, this, std::move(session));
}

void ServiceEntryPointMock::run(transport::SessionHandle session) {
    Message inMessage;
    while (true) {
        {
            stdx::lock_guard<stdx::mutex> lk(_shutdownLock);
            if (_inShutdown)
                break;
        }

        // sourceMessage()
        if (!session->sourceMessage(&inMessage).wait().isOK()) {
            break;
        }

        auto resp = handleRequest(nullptr, inMessage);

        // sinkMessage()
        if (!session->sinkMessage(resp.response).wait().isOK()) {
            break;
        }
    }
}

DbResponse ServiceEntryPointMock::handleRequest(OperationContext* opCtx, const Message& request) {
    // Need to set up our { ok : 1 } response.
    BufBuilder b{};

    // Leave room for the message header
    b.skip(mongo::MsgData::MsgDataHeaderSize);

    // Add our response
    auto okObj = BSON("ok" << 1.0);
    okObj.appendSelfToBufBuilder(b);

    // Add some metadata
    auto metadata = BSONObj();
    metadata.appendSelfToBufBuilder(b);

    // Set Message header fields
    MsgData::View msg = b.buf();
    msg.setLen(b.len());
    msg.setOperation(dbCommandReply);

    return {Message(b.release()), ""};
}

void ServiceEntryPointMock::endAllSessions(transport::Session::TagMask) {
    {
        stdx::lock_guard<stdx::mutex> lk(_shutdownLock);
        _inShutdown = true;
    }

    for (auto& t : _threads) {
        t.join();
    }
}

size_t ServiceEntryPointMock::numOpenSessions() const {
    return 0ULL;
}

}  // namespace mongo

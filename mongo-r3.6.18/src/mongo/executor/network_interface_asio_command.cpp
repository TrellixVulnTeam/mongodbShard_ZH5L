
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
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kASIO

#include "mongo/platform/basic.h"

#include "mongo/executor/network_interface_asio.h"

#include <type_traits>
#include <utility>

#include "mongo/base/static_assert.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/executor/async_stream_interface.h"
#include "mongo/executor/connection_pool_asio.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/rpc/protocol.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace executor {

/**
 * The following send - receive utility functions are "stateless" in that they exist
 * apart from the AsyncOp state machine.
 */

namespace {

using namespace std::literals::string_literals;

MONGO_FP_DECLARE(NetworkInterfaceASIOasyncRunCommandFail);

using asio::ip::tcp;
using ResponseStatus = TaskExecutor::ResponseStatus;

// A type conforms to the NetworkHandler concept if it is a callable type that takes a
// std::error_code and std::size_t and returns void. The std::error_code parameter is used
// to inform the handler if the asynchronous operation it was waiting on succeeded, and the
// size_t parameter conveys how many bytes were read or written.
template <typename FunctionLike>
using IsNetworkHandler =
    std::is_convertible<FunctionLike, stdx::function<void(std::error_code, std::size_t)>>;

template <typename Handler>
void asyncSendMessage(AsyncStreamInterface& stream, Message* m, Handler&& handler) {
    MONGO_STATIC_ASSERT_MSG(
        IsNetworkHandler<Handler>::value,
        "Handler passed to asyncSendMessage does not conform to NetworkHandler concept");
    m->header().setResponseToMsgId(0);
    m->header().setId(nextMessageId());
    // TODO: Some day we may need to support vector messages.
    fassert(28708, m->buf() != 0);
    stream.write(asio::buffer(m->buf(), m->size()), std::forward<Handler>(handler));
}

template <typename Handler>
void asyncRecvMessageHeader(AsyncStreamInterface& stream,
                            MSGHEADER::Value* header,
                            Handler&& handler) {
    MONGO_STATIC_ASSERT_MSG(
        IsNetworkHandler<Handler>::value,
        "Handler passed to asyncRecvMessageHeader does not conform to NetworkHandler concept");
    stream.read(asio::buffer(header->view().view2ptr(), sizeof(decltype(*header))),
                std::forward<Handler>(handler));
}

template <typename Handler>
void asyncRecvMessageBody(AsyncStreamInterface& stream,
                          MSGHEADER::Value* header,
                          Message* m,
                          Handler&& handler) {
    MONGO_STATIC_ASSERT_MSG(
        IsNetworkHandler<Handler>::value,
        "Handler passed to asyncRecvMessageBody does not conform to NetworkHandler concept");
    // validate message length
    int len = header->constView().getMessageLength();
    if (len == 542393671) {
        LOG(3) << "attempt to access MongoDB over HTTP on the native driver port.";
        return handler(make_error_code(ErrorCodes::ProtocolError), 0);
    } else if (static_cast<size_t>(len) < sizeof(MSGHEADER::Value) ||
               static_cast<size_t>(len) > MaxMessageSizeBytes) {
        warning() << "recv(): message len " << len << " is invalid. "
                  << "Min " << sizeof(MSGHEADER::Value) << " Max: " << MaxMessageSizeBytes;
        return handler(make_error_code(ErrorCodes::InvalidLength), 0);
    }

    int z = (len + 1023) & 0xfffffc00;
    invariant(z >= len);
    m->setData(SharedBuffer::allocate(z));
    MsgData::View mdView = m->buf();

    // copy header data into master buffer
    int headerLen = sizeof(MSGHEADER::Value);
    memcpy(mdView.view2ptr(), header, headerLen);
    int bodyLength = len - headerLen;
    invariant(bodyLength >= 0);

    // receive remaining data into md->data
    stream.read(asio::buffer(mdView.data(), bodyLength), std::forward<Handler>(handler));
}

ResponseStatus decodeRPC(Message* received,
                         rpc::Protocol protocol,
                         Milliseconds elapsed,
                         const HostAndPort& source,
                         rpc::EgressMetadataHook* metadataHook) {
    try {
        // makeReply will throw if the reply is invalid
        auto reply = rpc::makeReply(received);
        if (reply->getProtocol() != protocol) {
            auto requestProtocol = rpc::toString(static_cast<rpc::ProtocolSet>(protocol));
            if (!requestProtocol.isOK())
                return {requestProtocol.getStatus(), elapsed};

            return {ErrorCodes::RPCProtocolNegotiationFailed,
                    str::stream() << "Mismatched RPC protocols - request was '"
                                  << requestProtocol.getValue().toString()
                                  << "' '"
                                  << " but reply was '"
                                  << networkOpToString(received->operation())
                                  << "'",
                    elapsed};
        }
        auto commandReply = reply->getCommandReply();
        auto replyMetadata = reply->getMetadata();

        // Handle incoming reply metadata.
        if (metadataHook) {
            auto listenStatus = callNoexcept(*metadataHook,
                                             &rpc::EgressMetadataHook::readReplyMetadata,
                                             nullptr,  // adding operationTime is handled via lambda
                                             source.toString(),
                                             replyMetadata);
            if (!listenStatus.isOK()) {
                return {listenStatus, elapsed};
            }
        }

        return {RemoteCommandResponse(
            std::move(*received), std::move(commandReply), std::move(replyMetadata), elapsed)};
    } catch (...) {
        return {exceptionToStatus(), elapsed};
    }
}

}  // namespace

NetworkInterfaceASIO::AsyncCommand::AsyncCommand(AsyncConnection* conn,
                                                 Message&& command,
                                                 Date_t now,
                                                 const HostAndPort& target)
    : _conn(conn), _toSend(std::move(command)), _start(now), _target(target) {
    _toSend.header().setResponseToMsgId(0);
}

NetworkInterfaceASIO::AsyncConnection& NetworkInterfaceASIO::AsyncCommand::conn() {
    return *_conn;
}

Message& NetworkInterfaceASIO::AsyncCommand::toSend() {
    return _toSend;
}

Message& NetworkInterfaceASIO::AsyncCommand::toRecv() {
    return _toRecv;
}

MSGHEADER::Value& NetworkInterfaceASIO::AsyncCommand::header() {
    return _header;
}

ResponseStatus NetworkInterfaceASIO::AsyncCommand::response(AsyncOp* op,
                                                            rpc::Protocol protocol,
                                                            Date_t now,
                                                            rpc::EgressMetadataHook* metadataHook) {
    auto& received = _toRecv;
    if (received.operation() == dbCompressed) {
        auto swm = conn().getCompressorManager().decompressMessage(received);
        if (!swm.isOK()) {
            return swm.getStatus();
        }
        received = std::move(swm.getValue());
    }

    auto rs = decodeRPC(&received, protocol, now - _start, _target, metadataHook);
    if (rs.isOK())
        op->setResponseMetadata(rs.metadata);
    return rs;
}

void NetworkInterfaceASIO::_startCommand(AsyncOp* op) {
    LOG(3) << "running command " << redact(op->request().cmdObj) << " against database "
           << op->request().dbname << " across network to " << op->request().target.toString();
    if (inShutdown()) {
        return;
    }

    // _connect() will continue the state machine.
    _connect(op);
}

void NetworkInterfaceASIO::_beginCommunication(AsyncOp* op) {
    // The way that we connect connections for the connection pool is by
    // starting the callback chain with connect(), but getting off at the first
    // _beginCommunication. I.e. all AsyncOp's start off with _inSetup == true
    // and arrive here as they're connected and authed. Once they hit here, we
    // return to the connection pool's get() callback with _inSetup == false,
    // so we can proceed with user operations after they return to this
    // codepath.
    if (op->_inSetup) {
        auto host = op->request().target;
        auto getConnectionDuration = now() - op->start();
        //log() << "Successfully connected to " << host << ", took " << getConnectionDuration << " ("
        //      << _connectionPool.getNumConnectionsPerHost(host) << " connections now open to "
        //      << host << ")";
        op->_inSetup = false;
        op->finish(RemoteCommandResponse());
        return;
    }

    LOG(3) << "Initiating asynchronous command: " << redact(op->request().toString());

    auto beginStatus = op->beginCommand(op->request());
    if (!beginStatus.isOK()) {
        return _completeOperation(op, beginStatus);
    }

    _asyncRunCommand(op, [this, op](std::error_code ec, size_t bytes) {
        _validateAndRun(op, ec, [this, op]() { _completedOpCallback(op); });
    });
}

void NetworkInterfaceASIO::_completedOpCallback(AsyncOp* op) {
    auto response = op->command().response(op, op->operationProtocol(), now(), _metadataHook.get());
    _completeOperation(op, response);
}

void NetworkInterfaceASIO::_networkErrorCallback(AsyncOp* op, const std::error_code& ec) {
    ErrorCodes::Error errorCode = (ec.category() == mongoErrorCategory())
        ? ErrorCodes::Error(ec.value())
        : ErrorCodes::HostUnreachable;
    _completeOperation(op, {errorCode, ec.message(), Milliseconds(now() - op->_start)});
}

// NOTE: This method may only be called by ASIO threads
// (do not call from methods entered by TaskExecutor threads)
void NetworkInterfaceASIO::_completeOperation(AsyncOp* op, ResponseStatus resp) {
    auto metadata = op->getResponseMetadata();
    if (!metadata.isEmpty()) {
        resp.metadata = metadata;
    }

    // Cancel this operation's timeout. Note that the timeout callback may already be running,
    // may have run, or may have already been scheduled to run in the near future.
    if (op->_timeoutAlarm) {
        op->_timeoutAlarm->cancel();
    }

    if (ErrorCodes::isExceededTimeLimitError(resp.status.code())) {
        _numTimedOutOps.fetchAndAdd(1);
    }

    if (op->_inSetup) {
        // If we are in setup we should only be here if we failed to connect.
        MONGO_ASIO_INVARIANT(!resp.isOK(), "Failed to connect in setup", op);
        // If we fail during connection, we won't be able to access any of op's members after
        // calling finish(), so we return here.
        log() << "Failed to connect to " << op->request().target << " - " << redact(resp.status);
        op->finish(std::move(resp));
        return;
    }

    if (op->_inRefresh) {
        // If we are in refresh we should only be here if we failed to heartbeat.
        MONGO_ASIO_INVARIANT(!resp.isOK(), "In refresh, but did not fail to heartbeat", op);
        // If we fail during heartbeating, we won't be able to access any of op's members after
        // calling finish(), so we return here.
        log() << "Failed asio heartbeat to "
              << (op->commandIsInitialized() ? op->command().target().toString() : "unknown"s)
              << " - " << redact(resp.status);
        _numFailedOps.fetchAndAdd(1);
        op->finish(std::move(resp));
        return;
    }

    if (!resp.isOK()) {
        // In the case that resp is not OK, but _inSetup is false, we are using a connection
        // that we got from the pool to execute a command, but it failed for some reason.
        if (op->commandIsInitialized() && shouldLog(LogstreamBuilder::severityCast(2))) {
            const auto performLog = [&resp](Message& message) {
                LOG(2) << "Failed to send message. Reason: " << redact(resp.status) << ". Message: "
                       << rpc::opMsgRequestFromAnyProtocol(message).body.toString(
                              logger::globalLogDomain()->shouldRedactLogs());
            };

            // Message might be compressed, decompress in that case so we can log the body
            Message& maybeCompressed = op->command().toSend();
            if (maybeCompressed.operation() != dbCompressed) {
                performLog(maybeCompressed);
            } else {
                StatusWith<Message> decompressedMessage =
                    op->command().conn().getCompressorManager().decompressMessage(maybeCompressed);
                if (decompressedMessage.isOK()) {
                    performLog(decompressedMessage.getValue());
                } else {
                    LOG(2) << "Failed to execute a command.  Reason: " << redact(resp.status)
                           << ". Decompression failed with: "
                           << redact(decompressedMessage.getStatus());
                }
            }
        } else {
            LOG(2) << "Failed to execute a command.  Reason: " << redact(resp.status);
        }

        if (resp.status.code() != ErrorCodes::CallbackCanceled) {
            _numFailedOps.fetchAndAdd(1);
        }
    } else {
        _numSucceededOps.fetchAndAdd(1);
    }

    std::unique_ptr<AsyncOp> ownedOp;

    {
        stdx::lock_guard<stdx::mutex> lk(_inProgressMutex);

        auto iter = _inProgress.find(op);

        MONGO_ASIO_INVARIANT_INLOCK(
            iter != _inProgress.end(), "Could not find AsyncOp in _inProgress", op);

        ownedOp = std::move(iter->second);
        _inProgress.erase(iter);
    }

    op->finish(std::move(resp));

    MONGO_ASIO_INVARIANT(static_cast<bool>(ownedOp), "Invalid AsyncOp", op);

    auto conn = std::move(op->_connectionPoolHandle);
    auto asioConn = static_cast<connection_pool_asio::ASIOConnection*>(conn.get());

    // Prevent any other threads or callbacks from accessing this op so we may safely complete
    // and destroy it. It is key that we do this after we remove the op from the _inProgress map
    // or someone else in cancelCommand could read the bumped generation and cancel the next
    // command that uses this op. See SERVER-20556.
    {
        stdx::lock_guard<stdx::mutex> lk(op->_access->mutex);
        ++(op->_access->id);
    }

    // We need to bump the generation BEFORE we call reset() or we could flip the timeout in the
    // timeout callback before returning the AsyncOp to the pool.
    ownedOp->reset();

    asioConn->bindAsyncOp(std::move(ownedOp));
    if (!resp.isOK()) {
        asioConn->indicateFailure(resp.status);
    } else {
        asioConn->indicateUsed();
        asioConn->indicateSuccess();
    }

    signalWorkAvailable();
}

void NetworkInterfaceASIO::_asyncRunCommand(AsyncOp* op, NetworkOpHandler handler) {
    LOG(2) << "Starting asynchronous command " << op->request().id << " on host "
           << op->request().target.toString();

    if (MONGO_FAIL_POINT(NetworkInterfaceASIOasyncRunCommandFail)) {
        _validateAndRun(op, asio::error::basic_errors::network_unreachable, [] {});
        return;
    }

    // We invert the following steps below to run a command:
    // 1 - send the given command
    // 2 - receive a header for the response
    // 3 - validate and receive response body
    // 4 - advance the state machine by calling handler()
    auto& cmd = op->command();

    // Step 4
    auto recvMessageCallback = [this, handler](std::error_code ec, size_t bytes) {
        // We don't call _validateAndRun here as we assume the caller will.
        handler(ec, bytes);
    };

    // Step 3
    auto recvHeaderCallback = [this, &cmd, handler, recvMessageCallback, op](std::error_code ec,
                                                                             size_t bytes) {
        // The operation could have been canceled after starting the command, but before
        // receiving the header
        _validateAndRun(op, ec, [this, recvMessageCallback, ec, bytes, &cmd, handler] {
            // validate response id
            uint32_t expectedId = cmd.toSend().header().getId();
            uint32_t actualId = cmd.header().constView().getResponseToMsgId();
            if (actualId != expectedId) {
                LOG(3) << "got wrong response:"
                       << " expected response id: " << expectedId
                       << ", got response id: " << actualId;
                return handler(make_error_code(ErrorCodes::ProtocolError), bytes);
            }

            asyncRecvMessageBody(
                cmd.conn().stream(), &cmd.header(), &cmd.toRecv(), std::move(recvMessageCallback));
        });
    };

    // Step 2
    auto sendMessageCallback = [this, &cmd, handler, recvHeaderCallback, op](std::error_code ec,
                                                                             size_t bytes) {
        _validateAndRun(op, ec, [this, &cmd, recvHeaderCallback] {
            asyncRecvMessageHeader(
                cmd.conn().stream(), &cmd.header(), std::move(recvHeaderCallback));
        });


    };

    // Step 1
    asyncSendMessage(cmd.conn().stream(), &cmd.toSend(), std::move(sendMessageCallback));
}

void NetworkInterfaceASIO::_runConnectionHook(AsyncOp* op) {
    if (!_hook) {
        return _beginCommunication(op);
    }

    auto swOptionalRequest =
        callNoexcept(*_hook, &NetworkConnectionHook::makeRequest, op->request().target);

    if (!swOptionalRequest.isOK()) {
        return _completeOperation(op, swOptionalRequest.getStatus());
    }

    auto optionalRequest = std::move(swOptionalRequest.getValue());

    if (optionalRequest == boost::none) {
        return _beginCommunication(op);
    }

    auto beginStatus = op->beginCommand(*optionalRequest);
    if (!beginStatus.isOK()) {
        return _completeOperation(op, beginStatus);
    }

    auto finishHook = [this, op]() {
        auto response =
            op->command().response(op, op->operationProtocol(), now(), _metadataHook.get());

        if (!response.isOK()) {
            return _completeOperation(op, response);
        }

        auto handleStatus = callNoexcept(
            *_hook, &NetworkConnectionHook::handleReply, op->request().target, std::move(response));

        if (!handleStatus.isOK()) {
            return _completeOperation(op, handleStatus);
        }

        return _beginCommunication(op);
    };

    return _asyncRunCommand(op, [this, op, finishHook](std::error_code ec, std::size_t bytes) {
        _validateAndRun(op, ec, finishHook);
    });
}


}  // namespace executor
}  // namespace mongo

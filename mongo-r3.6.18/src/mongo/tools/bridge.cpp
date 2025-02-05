
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kBridge

#include "mongo/platform/basic.h"

#include <boost/optional.hpp>
#include <cstdint>

#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/random.h"
#include "mongo/rpc/command_request.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/tools/bridge_commands.h"
#include "mongo/tools/mongobridge_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/abstract_message_port.h"
#include "mongo/util/net/listen.h"
#include "mongo/util/net/message.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/text.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

namespace mongo {

namespace {

boost::optional<HostAndPort> extractHostInfo(const OpMsgRequest& request) {
    // The initial isMaster request made by mongod and mongos processes should contain a hostInfo
    // field that identifies the process by its host:port.
    StringData cmdName = request.getCommandName();
    if (cmdName != "isMaster" && cmdName != "ismaster") {
        return boost::none;
    }

    if (auto hostInfoElem = request.body["hostInfo"]) {
        if (hostInfoElem.type() == String) {
            return HostAndPort{hostInfoElem.valueStringData()};
        }
    }
    return boost::none;
}

class Forwarder {
public:
    Forwarder(AbstractMessagingPort* mp,
              stdx::mutex* settingsMutex,
              HostSettingsMap* settings,
              int64_t seed)
        : _mp(mp), _settingsMutex(settingsMutex), _settings(settings), _prng(seed) {}

    void operator()() {
        DBClientConnection dest;

        {
            HostAndPort destAddr{mongoBridgeGlobalParams.destUri};
            const Seconds kConnectTimeout(30);
            Timer connectTimer;
            while (true) {
                // DBClientConnection::connectSocketOnly() is used instead of
                // DBClientConnection::connect() to avoid sending an isMaster command when the
                // connection is established. We'd otherwise trigger a socket timeout when
                // forwarding an _isSelf command because dest's replication subsystem hasn't been
                // initialized yet and so it cannot respond to the isMaster command.
                auto status = dest.connectSocketOnly(destAddr);
                if (status.isOK()) {
                    break;
                }
                Seconds elapsed{connectTimer.seconds()};
                if (elapsed >= kConnectTimeout) {
                    warning() << "Unable to establish connection to "
                              << mongoBridgeGlobalParams.destUri << " after " << elapsed
                              << " seconds: " << status;
                    log() << "end connection " << _mp->remote().toString();
                    _mp->shutdown();
                    return;
                }
                sleepmillis(500);
            }
        }

        bool receivingFirstMessage = true;
        boost::optional<HostAndPort> host;

        Message request;
        Message response;
        MessageCompressorManager compressorManager;

        while (true) {
            try {
                request.reset();
                if (!_mp->recv(request)) {
                    log() << "end connection " << _mp->remote().toString();
                    _mp->shutdown();
                    break;
                }

                uassert(ErrorCodes::IllegalOperation,
                        str::stream() << "Unsupported network op " << request.operation(),
                        isSupportedRequestNetworkOp(request.operation()));

                if (request.operation() == dbCompressed) {
                    auto swm = compressorManager.decompressMessage(request);
                    if (!swm.isOK()) {
                        error() << "Error decompressing message: " << swm.getStatus();
                        _mp->shutdown();
                        return;
                    }
                    request = std::move(swm.getValue());
                }

                const bool isFireAndForgetCommand = OpMsg::isFlagSet(request, OpMsg::kMoreToCome);

                boost::optional<OpMsgRequest> cmdRequest;
                if ((request.operation() == dbQuery &&
                     NamespaceString(DbMessage(request).getns()).isCommand()) ||
                    request.operation() == dbCommand || request.operation() == dbMsg) {
                    cmdRequest = rpc::opMsgRequestFromAnyProtocol(request);
                    if (receivingFirstMessage) {
                        host = extractHostInfo(*cmdRequest);
                    }

                    std::string hostName = host ? (host->toString()) : "<unknown>";
                    LOG(1) << "Received \"" << cmdRequest->getCommandName()
                           << "\" command with arguments " << cmdRequest->body << " from "
                           << hostName;
                }
                receivingFirstMessage = false;

                // Handle a message intended to configure the mongobridge and return a response.
                // The 'request' is consumed by the mongobridge and does not get forwarded to
                // 'dest'.
                if (auto status = maybeProcessBridgeCommand(cmdRequest)) {
                    invariant(!isFireAndForgetCommand);

                    auto replyBuilder = rpc::makeReplyBuilder(rpc::protocolForMessage(request));
                    BSONObj metadata;
                    BSONObj reply;
                    StatusWith<BSONObj> commandReply(reply);
                    if (!status->isOK()) {
                        commandReply = StatusWith<BSONObj>(*status);
                    }
                    auto cmdResponse = replyBuilder->setCommandReply(std::move(commandReply))
                                           .setMetadata(metadata)
                                           .done();
                    cmdResponse.header().setId(nextMessageId());
                    cmdResponse.header().setResponseToMsgId(request.header().getId());
                    _mp->say(cmdResponse);
                    continue;
                }

                // Get the message handling settings for 'host' if the source of _mp's connection is
                // known. By default, messages are forwarded to 'dest' without any additional delay.
                HostSettings hostSettings = getHostSettings(host);

                switch (hostSettings.state) {
                    // Forward the message to 'dest' after waiting for 'hostSettings.delay'
                    // milliseconds.
                    case HostSettings::State::kForward:
                        sleepmillis(durationCount<Milliseconds>(hostSettings.delay));
                        break;
                    // Close the connection to 'dest'.
                    case HostSettings::State::kHangUp:
                        log() << "Rejecting connection from " << host->toString()
                              << ", end connection " << _mp->remote().toString();
                        _mp->shutdown();
                        return;
                    // Forward the message to 'dest' with probability '1 - hostSettings.loss'.
                    case HostSettings::State::kDiscard:
                        if (_prng.nextCanonicalDouble() < hostSettings.loss) {
                            std::string hostName = host ? (host->toString()) : "<unknown>";
                            if (cmdRequest) {
                                log() << "Discarding \"" << cmdRequest->getCommandName()
                                      << "\" command with arguments " << cmdRequest->body
                                      << " from " << hostName;
                            } else {
                                log() << "Discarding " << networkOpToString(request.operation())
                                      << " from " << hostName;
                            }
                            continue;
                        }
                        break;
                }

                // Send the message we received from '_mp' to 'dest'. 'dest' returns a response for
                // OP_QUERY, OP_GET_MORE, and OP_COMMAND messages that we respond back to
                // '_mp' with.
                if (!isFireAndForgetCommand &&
                    (request.operation() == dbQuery || request.operation() == dbGetMore ||
                     request.operation() == dbCommand || request.operation() == dbMsg)) {
                    // TODO dbMsg moreToCome
                    // Forward the message to 'dest' and receive its reply in 'response'.
                    response.reset();
                    dest.port().call(request, response);

                    // If there's nothing to respond back to '_mp' with, then close the connection.
                    if (response.empty()) {
                        log() << "Received an empty response, end connection "
                              << _mp->remote().toString();
                        _mp->shutdown();
                        break;
                    }

                    // Reload the message handling settings for 'host' in case they were changed
                    // while waiting for a response from 'dest'.
                    hostSettings = getHostSettings(host);

                    // It's possible that sending 'request' blocked until 'dest' had something to
                    // reply with. If the message handling settings were since changed to close
                    // connections from 'host', then do so now.
                    if (hostSettings.state == HostSettings::State::kHangUp) {
                        log() << "Closing connection from " << host->toString()
                              << ", end connection " << _mp->remote().toString();
                        _mp->shutdown();
                        break;
                    }

                    _mp->say(response);

                    // If 'exhaust' is true, then instead of trying to receive another message from
                    // '_mp', receive messages from 'dest' until it returns a cursor id of zero.
                    bool exhaust = false;
                    if (request.operation() == dbQuery) {
                        DbMessage d(request);
                        QueryMessage q(d);
                        exhaust = q.queryOptions & QueryOption_Exhaust;
                    }
                    while (exhaust) {
                        if (response.operation() == dbCompressed) {
                            auto swm = compressorManager.decompressMessage(response);
                            if (!swm.isOK()) {
                                error() << "Error decompressing message: " << swm.getStatus();
                                _mp->shutdown();
                                return;
                            }
                            response = std::move(swm.getValue());
                        }

                        MsgData::View header = response.header();
                        QueryResult::View qr = header.view2ptr();
                        if (qr.getCursorId()) {
                            response.reset();
                            dest.port().recv(response);
                            _mp->say(response);
                        } else {
                            exhaust = false;
                        }
                    }
                } else {
                    dest.port().say(request);
                }
            } catch (const DBException& ex) {
                error() << "Caught DBException in Forwarder: " << ex << ", end connection "
                        << _mp->remote().toString();
                _mp->shutdown();
                break;
            } catch (...) {
                severe() << exceptionToStatus() << ", terminating";
                quickExit(EXIT_UNCAUGHT);
            }
        }
    }

private:
    Status runBridgeCommand(StringData cmdName, BSONObj cmdObj) {
        auto status = BridgeCommand::findCommand(cmdName);
        if (!status.isOK()) {
            return status.getStatus();
        }

        BridgeCommand* command = status.getValue();
        return command->run(cmdObj, _settingsMutex, _settings);
    }

    boost::optional<Status> maybeProcessBridgeCommand(boost::optional<OpMsgRequest> cmdRequest) {
        if (!cmdRequest) {
            return boost::none;
        }

        if (auto forBridge = cmdRequest->body["$forBridge"]) {
            if (forBridge.trueValue()) {
                return runBridgeCommand(cmdRequest->getCommandName(), cmdRequest->body);
            }
            return boost::none;
        }

        return boost::none;
    }

    HostSettings getHostSettings(boost::optional<HostAndPort> host) {
        if (host) {
            stdx::lock_guard<stdx::mutex> lk(*_settingsMutex);
            return (*_settings)[*host];
        }
        return {};
    }

    AbstractMessagingPort* _mp;

    stdx::mutex* _settingsMutex;
    HostSettingsMap* _settings;

    PseudoRandom _prng;
};

class BridgeListener final : public Listener {
public:
    BridgeListener()
        : Listener(
              "bridge", "0.0.0.0", mongoBridgeGlobalParams.port, getGlobalServiceContext(), false),
          _seedSource(mongoBridgeGlobalParams.seed) {
        log() << "Setting random seed: " << mongoBridgeGlobalParams.seed;
    }

    void accepted(std::unique_ptr<AbstractMessagingPort> mp) override final {
        {
            stdx::lock_guard<stdx::mutex> lk(_portsMutex);
            if (_inShutdown.load()) {
                mp->shutdown();
                return;
            }
            _ports.insert(mp.get());
        }

        Forwarder f(mp.release(), &_settingsMutex, &_settings, _seedSource.nextInt64());
        stdx::thread t(f);
        t.detach();
    }

    void shutdownAll() {
        stdx::lock_guard<stdx::mutex> lk(_portsMutex);
        for (auto mp : _ports) {
            mp->shutdown();
        }
    }

private:
    stdx::mutex _portsMutex;
    std::set<AbstractMessagingPort*> _ports;
    AtomicWord<bool> _inShutdown{false};

    stdx::mutex _settingsMutex;
    HostSettingsMap _settings;

    PseudoRandom _seedSource;
};

std::unique_ptr<mongo::BridgeListener> listener;

MONGO_INITIALIZER(SetGlobalEnvironment)(InitializerContext* context) {
    setGlobalServiceContext(stdx::make_unique<ServiceContextNoop>());
    return Status::OK();
}

}  // namespace

int bridgeMain(int argc, char** argv, char** envp) {

    registerShutdownTask([&] {
        // NOTE: This function may be called at any time. It must not
        // depend on the prior execution of mongo initializers or the
        // existence of threads.
        ListeningSockets::get()->closeAll();
        listener->shutdownAll();
    });

    setupSignalHandlers();
    runGlobalInitializersOrDie(argc, argv, envp);
    startSignalProcessingThread(LogFileStatus::kNoLogFileToRotate);

    listener = stdx::make_unique<BridgeListener>();
    listener->setupSockets();
    listener->initAndListen();
    return EXIT_CLEAN;
}

}  // namespace mongo

#if defined(_WIN32)
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters
// as main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters.  The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent
// and makes them available through the argv() and envp() members.  This enables bridgeMain()
// to process UTF-8 encoded arguments and environment variables without regard to platform.
int wmain(int argc, wchar_t* argvW[], wchar_t* envpW[]) {
    mongo::WindowsCommandLine wcl(argc, argvW, envpW);
    int exitCode = mongo::bridgeMain(argc, wcl.argv(), wcl.envp());
    mongo::quickExit(exitCode);
}
#else
int main(int argc, char* argv[], char** envp) {
    int exitCode = mongo::bridgeMain(argc, argv, envp);
    mongo::quickExit(exitCode);
}
#endif

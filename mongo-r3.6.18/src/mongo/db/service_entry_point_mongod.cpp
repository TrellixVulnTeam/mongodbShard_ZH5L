
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/service_entry_point_mongod.h"

#include "mongo/base/checked_cast.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/impersonation_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/concurrency/global_lock_acquisition_tracker.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/initialize_operation_session_info.h"
#include "mongo/db/introspect.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/query/find.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/server_read_concern_metrics.h"
#include "mongo/db/stats/top.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/metadata/config_server_metadata.h"
#include "mongo/rpc/metadata/logical_time_metadata.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/sharding_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/op_msg.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

MONGO_FP_DECLARE(rsStopGetMore);
MONGO_FP_DECLARE(respondWithNotPrimaryInCommandDispatch);

namespace {
using logger::LogComponent;

// The command names for which to check out a session.
//
// Note: Eval should check out a session because it defaults to running under a global write lock,
// so if it didn't, and the function it was given contains any of these whitelisted commands, they
// would try to check out a session under a lock, which is not allowed.  Similarly,
// refreshLogicalSessionCacheNow triggers a bulk update under a lock on the sessions collection.
const StringMap<int> cmdWhitelist = {{"delete", 1},
                                     {"eval", 1},
                                     {"$eval", 1},
                                     {"findandmodify", 1},
                                     {"findAndModify", 1},
                                     {"insert", 1},
                                     {"refreshLogicalSessionCacheNow", 1},
                                     {"update", 1}};

void generateLegacyQueryErrorResponse(const AssertionException* exception,
                                      const QueryMessage& queryMessage,
                                      CurOp* curop,
                                      Message* response) {
    curop->debug().exceptionInfo = exception->toStatus();

    log(LogComponent::kQuery) << "assertion " << exception->toString() << " ns:" << queryMessage.ns
                              << " query:" << (queryMessage.query.valid(BSONVersion::kLatest)
                                                   ? redact(queryMessage.query)
                                                   : "query object is corrupt");
    if (queryMessage.ntoskip || queryMessage.ntoreturn) {
        log(LogComponent::kQuery) << " ntoskip:" << queryMessage.ntoskip
                                  << " ntoreturn:" << queryMessage.ntoreturn;
    }

    const StaleConfigException* scex = (exception->code() == ErrorCodes::StaleConfig)
        ? checked_cast<const StaleConfigException*>(exception)
        : NULL;

    BSONObjBuilder err;
    err.append("$err", exception->reason());
    err.append("code", exception->code());
    if (scex) {
        err.append("ok", 0.0);
        err.append("ns", scex->getns());
        scex->getVersionReceived().addToBSON(err, "vReceived");
        scex->getVersionWanted().addToBSON(err, "vWanted");
    }
    BSONObj errObj = err.done();

    if (scex) {
        log(LogComponent::kQuery) << "stale version detected during query over " << queryMessage.ns
                                  << " : " << errObj;
    }

    BufBuilder bb;
    bb.skip(sizeof(QueryResult::Value));
    bb.appendBuf((void*)errObj.objdata(), errObj.objsize());

    // TODO: call replyToQuery() from here instead of this!!! see dbmessage.h
    QueryResult::View msgdata = bb.buf();
    QueryResult::View qr = msgdata;
    qr.setResultFlags(ResultFlag_ErrSet);
    if (scex)
        qr.setResultFlags(qr.getResultFlags() | ResultFlag_ShardConfigStale);
    qr.msgdata().setLen(bb.len());
    qr.msgdata().setOperation(opReply);
    qr.setCursorId(0);
    qr.setStartingFrom(0);
    qr.setNReturned(1);
    response->setData(bb.release());
}

void registerError(OperationContext* opCtx, const DBException& exception) {
    LastError::get(opCtx->getClient()).setLastError(exception.code(), exception.reason());
    CurOp::get(opCtx)->debug().exceptionInfo = exception.toStatus();
}

void _generateErrorResponse(OperationContext* opCtx,
                            rpc::ReplyBuilderInterface* replyBuilder,
                            const DBException& exception,
                            const BSONObj& replyMetadata,
                            BSONObj extraFields = {}) {
    registerError(opCtx, exception);

    // We could have thrown an exception after setting fields in the builder,
    // so we need to reset it to a clean state just to be sure.
    replyBuilder->reset();

    // We need to include some extra information for StaleConfig.
    if (exception.code() == ErrorCodes::StaleConfig) {
        const StaleConfigException& scex = checked_cast<const StaleConfigException&>(exception);
        BSONObjBuilder extraFieldsBob(std::move(extraFields));
        extraFieldsBob.appendElements(BSON("ns" << scex.getns() << "vReceived"
                                                << BSONArray(scex.getVersionReceived().toBSON())
                                                << "vWanted"
                                                << BSONArray(scex.getVersionWanted().toBSON())));
        replyBuilder->setCommandReply(scex.toStatus(), extraFieldsBob.obj());
    } else {
        replyBuilder->setCommandReply(exception.toStatus(), extraFields);
    }

    replyBuilder->setMetadata(replyMetadata);
}

/**
 * Guard object for making a good-faith effort to enter maintenance mode and leave it when it
 * goes out of scope.
 *
 * Sometimes we cannot set maintenance mode, in which case the call to setMaintenanceMode will
 * return a non-OK status.  This class does not treat that case as an error which means that
 * anybody using it is assuming it is ok to continue execution without maintenance mode.
 *
 * TODO: This assumption needs to be audited and documented, or this behavior should be moved
 * elsewhere.
 */
class MaintenanceModeSetter {
    MONGO_DISALLOW_COPYING(MaintenanceModeSetter);

public:
    MaintenanceModeSetter(OperationContext* opCtx)
        : _opCtx(opCtx),
          _maintenanceModeSet(
              repl::ReplicationCoordinator::get(_opCtx)->setMaintenanceMode(true).isOK()) {}

    ~MaintenanceModeSetter() {
        if (_maintenanceModeSet) {
            repl::ReplicationCoordinator::get(_opCtx)
                ->setMaintenanceMode(false)
                .transitional_ignore();
        }
    }

private:
    OperationContext* const _opCtx;
    const bool _maintenanceModeSet;
};

void appendReplyMetadata(OperationContext* opCtx,
                         const OpMsgRequest& request,
                         BSONObjBuilder* metadataBob) {
    const bool isShardingAware = ShardingState::get(opCtx)->enabled();
    const bool isConfig = serverGlobalParams.clusterRole == ClusterRole::ConfigServer;
    auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
    const bool isReplSet =
        replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;

    if (isReplSet) {
        // Attach our own last opTime.
        repl::OpTime lastOpTimeFromClient =
            repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
        replCoord->prepareReplMetadata(request.body, lastOpTimeFromClient, metadataBob);
        // For commands from mongos, append some info to help getLastError(w) work.
        // TODO: refactor out of here as part of SERVER-18236
        if (isShardingAware || isConfig) {
            rpc::ShardingMetadata(lastOpTimeFromClient, replCoord->getElectionId())
                .writeToMetadata(metadataBob)
                .transitional_ignore();
        }
    }

    // If we're a shard other than the config shard, attach the last configOpTime we know about.
    if (isShardingAware && !isConfig) {
        auto opTime = grid.configOpTime();
        rpc::ConfigServerMetadata(opTime).writeToMetadata(metadataBob);
    }
}

/**
 * Given the specified command and whether it supports read concern, returns an effective read
 * concern which should be used.
 */
StatusWith<repl::ReadConcernArgs> _extractReadConcern(const BSONObj& cmdObj,
                                                      bool supportsNonLocalReadConcern) {
    repl::ReadConcernArgs readConcernArgs;

    auto readConcernParseStatus = readConcernArgs.initialize(cmdObj, Command::testCommandsEnabled);
    if (!readConcernParseStatus.isOK()) {
        return readConcernParseStatus;
    }

    if (!supportsNonLocalReadConcern &&
        readConcernArgs.getLevel() != repl::ReadConcernLevel::kLocalReadConcern) {
        return {ErrorCodes::InvalidOptions,
                str::stream() << "Command does not support non local read concern"};
    }

    return readConcernArgs;
}

void _waitForWriteConcernAndAddToCommandResponse(OperationContext* opCtx,
                                                 const std::string& commandName,
                                                 const repl::OpTime& lastOpBeforeRun,
                                                 BSONObjBuilder* commandResponseBuilder) {
    auto lastOpAfterRun = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();

    // Ensures that if we tried to do a write, we wait for write concern, even if that write was
    // a noop.
    if ((lastOpAfterRun == lastOpBeforeRun) &&
        GlobalLockAcquisitionTracker::get(opCtx).getGlobalExclusiveLockTaken()) {
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        lastOpAfterRun = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    }

    WriteConcernResult res;
    auto waitForWCStatus =
        waitForWriteConcern(opCtx, lastOpAfterRun, opCtx->getWriteConcern(), &res);
    Command::appendCommandWCStatus(*commandResponseBuilder, waitForWCStatus, res);

    // SERVER-22421: This code is to ensure error response backwards compatibility with the
    // user management commands. This can be removed in 3.6.
    if (!waitForWCStatus.isOK() && Command::isUserManagementCommand(commandName)) {
        BSONObj temp = commandResponseBuilder->asTempObj().copy();
        commandResponseBuilder->resetToEmpty();
        Command::appendCommandStatus(*commandResponseBuilder, waitForWCStatus);
        commandResponseBuilder->appendElementsUnique(temp);
    }
}

/**
 * For replica set members it returns the last known op time from opCtx. Otherwise will return
 * uninitialized cluster time.
 */
LogicalTime getClientOperationTime(OperationContext* opCtx) {
    auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
    const bool isReplSet =
        replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;

    if (!isReplSet) {
        return LogicalTime();
    }

    return LogicalTime(
        repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp().getTimestamp());
}

/**
 * Returns the proper operationTime for a command. To construct the operationTime for replica set
 * members, it uses the last optime in the oplog for writes, last committed optime for majority
 * reads, and the last applied optime for every other read. An uninitialized cluster time is
 * returned for non replica set members.
 */
LogicalTime computeOperationTime(OperationContext* opCtx, LogicalTime startOperationTime) {
    auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
    const bool isReplSet =
        replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;
    invariant(isReplSet);

    if (startOperationTime == LogicalTime::kUninitialized) {
        return LogicalTime(replCoord->getMyLastAppliedOpTime().getTimestamp());
    }

    auto operationTime = getClientOperationTime(opCtx);
    invariant(operationTime >= startOperationTime);

    // If the last operationTime has not changed, consider this command a read, and, for replica set
    // members, construct the operationTime with the proper optime for its read concern level.
    if (operationTime == startOperationTime) {
        auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);

        // Note: ReadConcernArgs::getLevel returns kLocal if none was set.
        if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kMajorityReadConcern) {
            operationTime = LogicalTime(replCoord->getLastCommittedOpTime().getTimestamp());
        } else {
            operationTime = LogicalTime(replCoord->getMyLastAppliedOpTime().getTimestamp());
        }
    }

    return operationTime;
}


/**
 * Computes the proper $clusterTime and operationTime values to include in the command response and
 * appends them to it. $clusterTime is added as metadata and operationTime as a command body field.
 *
 * The command body BSONObjBuilder is either the builder for the command body itself, or a builder
 * for extra fields to be added to the reply when generating an error response.
 */
void appendClusterAndOperationTime(OperationContext* opCtx,
                                   BSONObjBuilder* commandBodyFieldsBob,
                                   BSONObjBuilder* metadataBob,
                                   LogicalTime startTime) {
    if (serverGlobalParams.featureCompatibility.getVersion() !=
        ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36) {
        return;
    }

    if (repl::ReplicationCoordinator::get(opCtx)->getReplicationMode() !=
            repl::ReplicationCoordinator::modeReplSet ||
        !LogicalClock::get(opCtx)->isEnabled()) {
        return;
    }

    // Authorized clients always receive operationTime and dummy signed $clusterTime.
    if (LogicalTimeValidator::isAuthorizedToAdvanceClock(opCtx)) {
        auto operationTime = computeOperationTime(opCtx, startTime);
        auto signedTime = SignedLogicalTime(
            LogicalClock::get(opCtx)->getClusterTime(), TimeProofService::TimeProof(), 0);

        // TODO SERVER-35663: invariant that signedTime.getTime() >= operationTime.
        rpc::LogicalTimeMetadata(signedTime).writeToMetadata(metadataBob);
        operationTime.appendAsOperationTime(commandBodyFieldsBob);

        return;
    }

    // Servers without validators (e.g. a shard server not yet added to a cluster) do not return
    // logical times to unauthorized clients.
    auto validator = LogicalTimeValidator::get(opCtx);
    if (!validator) {
        return;
    }

    auto operationTime = computeOperationTime(opCtx, startTime);
    auto signedTime = validator->trySignLogicalTime(LogicalClock::get(opCtx)->getClusterTime());

    // If there were no keys, do not return $clusterTime or operationTime to unauthorized clients.
    if (signedTime.getKeyId() == 0) {
        return;
    }

    // TODO SERVER-35663: invariant that signedTime.getTime() >= operationTime.
    rpc::LogicalTimeMetadata(signedTime).writeToMetadata(metadataBob);
    operationTime.appendAsOperationTime(commandBodyFieldsBob);
}

bool runCommandImpl(OperationContext* opCtx,
                    Command* command,
                    const OpMsgRequest& request,
                    rpc::ReplyBuilderInterface* replyBuilder,
                    LogicalTime startOperationTime) {
    auto bytesToReserve = command->reserveBytesForReply();

// SERVER-22100: In Windows DEBUG builds, the CRT heap debugging overhead, in conjunction with the
// additional memory pressure introduced by reply buffer pre-allocation, causes the concurrency
// suite to run extremely slowly. As a workaround we do not pre-allocate in Windows DEBUG builds.
#ifdef _WIN32
    if (kDebugBuild)
        bytesToReserve = 0;
#endif

    // run expects non-const bsonobj
    BSONObj cmd = request.body;

    // run expects const db std::string (can't bind to temporary)
    const std::string db = request.getDatabase().toString();

    BSONObjBuilder inPlaceReplyBob = replyBuilder->getInPlaceReplyBuilder(bytesToReserve);

    Status rcStatus = waitForReadConcern(
        opCtx, repl::ReadConcernArgs::get(opCtx), command->allowsAfterClusterTime(cmd));
    if (!rcStatus.isOK()) {
        if (rcStatus == ErrorCodes::ExceededTimeLimit) {
            const int debugLevel =
                serverGlobalParams.clusterRole == ClusterRole::ConfigServer ? 0 : 2;
            LOG(debugLevel) << "Command on database " << db
                            << " timed out waiting for read concern to be satisfied. Command: "
                            << redact(command->getRedactedCopyForLogging(request.body))
                            << ". Info: " << redact(rcStatus);
        }

        auto result = Command::appendCommandStatus(inPlaceReplyBob, rcStatus);

        BSONObjBuilder metadataBob;
        appendReplyMetadata(opCtx, request, &metadataBob);
        appendClusterAndOperationTime(opCtx, &inPlaceReplyBob, &metadataBob, startOperationTime);
        inPlaceReplyBob.doneFast();
        replyBuilder->setMetadata(metadataBob.obj());

        return result;
    }

    bool result;
    if (!command->supportsWriteConcern(cmd)) {
        if (commandSpecifiesWriteConcern(cmd)) {
            auto result = Command::appendCommandStatus(
                inPlaceReplyBob,
                {ErrorCodes::InvalidOptions, "Command does not support writeConcern"});

            BSONObjBuilder metadataBob;
            appendReplyMetadata(opCtx, request, &metadataBob);
            appendClusterAndOperationTime(
                opCtx, &inPlaceReplyBob, &metadataBob, startOperationTime);
            inPlaceReplyBob.doneFast();
            replyBuilder->setMetadata(metadataBob.obj());

            return result;
        }

        result = command->publicRun(opCtx, request, inPlaceReplyBob);
    } else {
        auto wcResult = extractWriteConcern(opCtx, cmd, db);
        if (!wcResult.isOK()) {
            auto result = Command::appendCommandStatus(inPlaceReplyBob, wcResult.getStatus());

            BSONObjBuilder metadataBob;
            appendReplyMetadata(opCtx, request, &metadataBob);
            appendClusterAndOperationTime(
                opCtx, &inPlaceReplyBob, &metadataBob, startOperationTime);
            inPlaceReplyBob.doneFast();
            replyBuilder->setMetadata(metadataBob.obj());

            return result;
        }

        auto lastOpBeforeRun = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();

        // Change the write concern while running the command.
        const auto oldWC = opCtx->getWriteConcern();
        ON_BLOCK_EXIT([&] { opCtx->setWriteConcern(oldWC); });
        opCtx->setWriteConcern(wcResult.getValue());
        ON_BLOCK_EXIT([&] {
            _waitForWriteConcernAndAddToCommandResponse(
                opCtx, command->getName(), lastOpBeforeRun, &inPlaceReplyBob);
        });

        result = command->publicRun(opCtx, request, inPlaceReplyBob);

        // Nothing in run() should change the writeConcern.
        dassert(SimpleBSONObjComparator::kInstance.evaluate(opCtx->getWriteConcern().toBSON() ==
                                                            wcResult.getValue().toBSON()));
    }

    // When a linearizable read command is passed in, check to make sure we're reading
    // from the primary.
    if (command->supportsNonLocalReadConcern(db, cmd) &&
        (repl::ReadConcernArgs::get(opCtx).getLevel() ==
         repl::ReadConcernLevel::kLinearizableReadConcern) &&
        (request.getCommandName() != "getMore")) {

        auto linearizableReadStatus = waitForLinearizableReadConcern(opCtx);

        if (!linearizableReadStatus.isOK()) {
            inPlaceReplyBob.resetToEmpty();
            auto result = Command::appendCommandStatus(inPlaceReplyBob, linearizableReadStatus);

            BSONObjBuilder metadataBob;
            appendReplyMetadata(opCtx, request, &metadataBob);
            appendClusterAndOperationTime(
                opCtx, &inPlaceReplyBob, &metadataBob, startOperationTime);
            inPlaceReplyBob.doneFast();
            replyBuilder->setMetadata(metadataBob.obj());

            return result;
        }
    }

    Command::appendCommandStatus(inPlaceReplyBob, result);

    BSONObjBuilder metadataBob;
    appendReplyMetadata(opCtx, request, &metadataBob);
    appendClusterAndOperationTime(opCtx, &inPlaceReplyBob, &metadataBob, startOperationTime);
    inPlaceReplyBob.doneFast();
    replyBuilder->setMetadata(metadataBob.obj());

    return result;
}

// When active, we won't check if we are master in command dispatch. Activate this if you want to
// test failing during command execution.
MONGO_FP_DECLARE(skipCheckingForNotMasterInCommandDispatch);

/**
 * Executes a command after stripping metadata, performing authorization checks,
 * handling audit impersonation, and (potentially) setting maintenance mode. This method
 * also checks that the command is permissible to run on the node given its current
 * replication state. All the logic here is independent of any particular command; any
 * functionality relevant to a specific command should be confined to its run() method.
 */
void execCommandDatabase(OperationContext* opCtx,
                         Command* command,
                         const OpMsgRequest& request,
                         rpc::ReplyBuilderInterface* replyBuilder) {

    BSONObjBuilder extraFieldsBuilder;
    auto startOperationTime = getClientOperationTime(opCtx);
    try {
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->setCommand_inlock(command);
        }

        // TODO: move this back to runCommands when mongos supports OperationContext
        // see SERVER-18515 for details.
        rpc::readRequestMetadata(opCtx, request.body, command->requiresAuth());
        rpc::TrackingMetadata::get(opCtx).initWithOperName(command->getName());

        auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
        uassertStatusOK(initializeOperationSessionInfo(
            opCtx,
            request.body,
            command->requiresAuth(),
            replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet,
            opCtx->getServiceContext()->getGlobalStorageEngine()->supportsDocLocking()));

        const auto dbname = request.getDatabase().toString();
        uassert(
            ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid database name: '" << dbname << "'",
            NamespaceString::validDBName(dbname, NamespaceString::DollarInDbNameBehavior::Allow));

        std::unique_ptr<MaintenanceModeSetter> mmSetter;

        BSONElement cmdOptionMaxTimeMSField;
        BSONElement allowImplicitCollectionCreationField;
        BSONElement helpField;
        BSONElement shardVersionFieldIdx;
        BSONElement queryOptionMaxTimeMSField;

        StringMap<int> topLevelFields;
        for (auto&& element : request.body) {
            StringData fieldName = element.fieldNameStringData();
            if (fieldName == QueryRequest::cmdOptionMaxTimeMS) {
                cmdOptionMaxTimeMSField = element;
            } else if (fieldName == "allowImplicitCollectionCreation") {
                allowImplicitCollectionCreationField = element;
            } else if (fieldName == Command::kHelpFieldName) {
                helpField = element;
            } else if (fieldName == ChunkVersion::kShardVersionField) {
                shardVersionFieldIdx = element;
            } else if (fieldName == QueryRequest::queryOptionMaxTimeMS) {
                queryOptionMaxTimeMSField = element;
            }

            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "Parsed command object contains duplicate top level key: "
                                  << fieldName,
                    topLevelFields[fieldName]++ == 0);
        }

        if (Command::isHelpRequest(helpField)) {
            CurOp::get(opCtx)->ensureStarted();
            // We disable last-error for help requests due to SERVER-11492, because config servers
            // use help requests to determine which commands are database writes, and so must be
            // forwarded to all config servers.
            LastError::get(opCtx->getClient()).disable();
            Command::generateHelpResponse(opCtx, replyBuilder, *command);
            return;
        }

        // Session ids are forwarded in requests, so commands that require roundtrips between
        // servers may result in a deadlock when a server tries to check out a session it is already
        // using to service an earlier operation in the command's chain. To avoid this, only check
        // out sessions with provided txnNumber for commands that require them (i.e. write
        // commands).
        bool checkoutSession =
            opCtx->getTxnNumber() && (cmdWhitelist.find(command->getName()) != cmdWhitelist.cend());
        OperationContextSession sessionTxnState(opCtx, checkoutSession);

        ImpersonationSessionGuard guard(opCtx);
        uassertStatusOK(Command::checkAuthorization(command, opCtx, request));

        const bool iAmPrimary = replCoord->canAcceptWritesForDatabase_UNSAFE(opCtx, dbname);

        if (!opCtx->getClient()->isInDirectClient() &&
            !MONGO_FAIL_POINT(skipCheckingForNotMasterInCommandDispatch)) {

            bool commandCanRunOnSecondary = command->slaveOk();

            bool commandIsOverriddenToRunOnSecondary =
                command->slaveOverrideOk() && ReadPreferenceSetting::get(opCtx).canRunOnSecondary();

            bool iAmStandalone = !opCtx->writesAreReplicated();
            bool canRunHere = iAmPrimary || commandCanRunOnSecondary ||
                commandIsOverriddenToRunOnSecondary || iAmStandalone;

            // This logic is clearer if we don't have to invert it.
            if (!canRunHere && command->slaveOverrideOk()) {
                uasserted(ErrorCodes::NotMasterNoSlaveOk, "not master and slaveOk=false");
            }

            if (MONGO_FAIL_POINT(respondWithNotPrimaryInCommandDispatch)) {
                uassert(ErrorCodes::NotMaster, "not primary", canRunHere);
            } else {
                uassert(ErrorCodes::NotMaster, "not master", canRunHere);
            }

            if (!command->maintenanceOk() &&
                replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet &&
                !replCoord->canAcceptWritesForDatabase_UNSAFE(opCtx, dbname) &&
                !replCoord->getMemberState().secondary()) {

                uassert(ErrorCodes::NotMasterOrSecondary,
                        "node is recovering",
                        !replCoord->getMemberState().recovering());
                uassert(ErrorCodes::NotMasterOrSecondary,
                        "node is not in primary or recovering state",
                        replCoord->getMemberState().primary());
                // Check ticket SERVER-21432, slaveOk commands are allowed in drain mode
                uassert(ErrorCodes::NotMasterOrSecondary,
                        "node is in drain mode",
                        commandIsOverriddenToRunOnSecondary || commandCanRunOnSecondary);
            }
        }

        if (command->adminOnly()) {
            LOG(2) << "command: " << request.getCommandName();
        }

        if (command->maintenanceMode()) {
            mmSetter.reset(new MaintenanceModeSetter(opCtx));
        }

        if (command->shouldAffectCommandCounter()) {
            OpCounters* opCounters = &globalOpCounters;
            opCounters->gotCommand();
        }

        // Handle command option maxTimeMS.
        int maxTimeMS = uassertStatusOK(QueryRequest::parseMaxTimeMS(cmdOptionMaxTimeMSField));

        uassert(ErrorCodes::InvalidOptions,
                "no such command option $maxTimeMs; use maxTimeMS instead",
                queryOptionMaxTimeMSField.eoo());

        if (maxTimeMS > 0) {
            uassert(40119,
                    "Illegal attempt to set operation deadline within DBDirectClient",
                    !opCtx->getClient()->isInDirectClient());
            opCtx->setDeadlineAfterNowBy(Milliseconds{maxTimeMS});
        }

        auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
        readConcernArgs = uassertStatusOK(_extractReadConcern(
            request.body, command->supportsNonLocalReadConcern(dbname, request.body)));

        auto& oss = OperationShardingState::get(opCtx);

        if (!opCtx->getClient()->isInDirectClient() &&
            readConcernArgs.getLevel() != repl::ReadConcernLevel::kAvailableReadConcern &&
            (iAmPrimary ||
             ((serverGlobalParams.featureCompatibility.getVersion() ==
               ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36) &&
              (readConcernArgs.hasLevel() || readConcernArgs.getArgsClusterTime())))) {
            oss.initializeShardVersion(NamespaceString(command->parseNs(dbname, request.body)),
                                       shardVersionFieldIdx);

            auto const shardingState = ShardingState::get(opCtx);
            if (oss.hasShardVersion()) {
                uassertStatusOK(shardingState->canAcceptShardedCommands());
            }

            // Handle config optime information that may have been sent along with the command.
            uassertStatusOK(shardingState->updateConfigServerOpTimeFromMetadata(opCtx));
        }

        oss.setAllowImplicitCollectionCreation(allowImplicitCollectionCreationField);

        // This may trigger the maxTimeAlwaysTimeOut failpoint.
        auto status = opCtx->checkForInterruptNoAssert();

        // We still proceed if the primary stepped down, but accept other kinds of interruptions.
        // We defer to individual commands to allow themselves to be interruptible by stepdowns,
        // since commands like 'voteRequest' should conversely continue executing.
        if (status != ErrorCodes::PrimarySteppedDown &&
            status != ErrorCodes::InterruptedDueToReplStateChange) {
            uassertStatusOK(status);
        }

        bool retval = false;

        CurOp::get(opCtx)->ensureStarted();

        command->incrementCommandsExecuted();

        if (logger::globalLogDomain()->shouldLog(logger::LogComponent::kTracking,
                                                 logger::LogSeverity::Debug(1)) &&
            rpc::TrackingMetadata::get(opCtx).getParentOperId()) {
            MONGO_LOG_COMPONENT(1, logger::LogComponent::kTracking)
                << rpc::TrackingMetadata::get(opCtx).toString();
            rpc::TrackingMetadata::get(opCtx).setIsLogged(true);
        }

        retval = runCommandImpl(opCtx, command, request, replyBuilder, startOperationTime);

        if (!retval) {
            command->incrementCommandsFailed();
        }
    } catch (const DBException& e) {
        // If we got a stale config, wait in case the operation is stuck in a critical section
        if (e.code() == ErrorCodes::StaleConfig) {
            auto sce = dynamic_cast<const StaleConfigException*>(&e);
            invariant(sce);  // do not upcasts from DBException created by uassert variants.

            if (!opCtx->getClient()->isInDirectClient()) {
                ShardingState::get(opCtx)
                    ->onStaleShardVersion(
                        opCtx, NamespaceString(sce->getns()), sce->getVersionReceived())
                    .transitional_ignore();
            }
        }

        BSONObjBuilder metadataBob;
        appendReplyMetadata(opCtx, request, &metadataBob);

        // The read concern may not have yet been placed on the operation context, so attempt to
        // parse it here, so if it is valid it can be used to compute the proper operationTime.
        auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
        if (readConcernArgs.isEmpty()) {
            const std::string db = request.getDatabase().toString();
            auto readConcernArgsStatus = _extractReadConcern(
                request.body, command->supportsNonLocalReadConcern(db, request.body));
            if (readConcernArgsStatus.isOK()) {
                readConcernArgs = readConcernArgsStatus.getValue();
            }
        }
        appendClusterAndOperationTime(opCtx, &extraFieldsBuilder, &metadataBob, startOperationTime);

        LOG(1) << "assertion while executing command '" << request.getCommandName() << "' "
               << "on database '" << request.getDatabase() << "' "
               << "with arguments '" << redact(command->getRedactedCopyForLogging(request.body))
               << "': " << redact(e.toString());

        _generateErrorResponse(opCtx, replyBuilder, e, metadataBob.obj(), extraFieldsBuilder.obj());
    }
}

/**
 * Fills out CurOp / OpDebug with basic command info.
 */
void curOpCommandSetup(OperationContext* opCtx, const OpMsgRequest& request) {
    auto curop = CurOp::get(opCtx);
    curop->debug().iscommand = true;

    // We construct a legacy $cmd namespace so we can fill in curOp using
    // the existing logic that existed for OP_QUERY commands
    NamespaceString nss(request.getDatabase(), "$cmd");

    stdx::lock_guard<Client> lk(*opCtx->getClient());
    curop->setOpDescription_inlock(request.body);
    curop->markCommand_inlock();
    curop->setNS_inlock(nss.ns());
}

DbResponse runCommands(OperationContext* opCtx, const Message& message) {
    auto replyBuilder = rpc::makeReplyBuilder(rpc::protocolForMessage(message));
    [&] {
        OpMsgRequest request;
        try {  // Parse.
            request = rpc::opMsgRequestFromAnyProtocol(message);
        } catch (const DBException& ex) {
            // If this error needs to fail the connection, propagate it out.
            if (ErrorCodes::isConnectionFatalMessageParseError(ex.code()))
                throw;

            BSONObjBuilder metadataBob;
            BSONObjBuilder extraFieldsBuilder;
            appendClusterAndOperationTime(
                opCtx, &extraFieldsBuilder, &metadataBob, LogicalTime::kUninitialized);

            // Otherwise, reply with the parse error. This is useful for cases where parsing fails
            // due to user-supplied input, such as the document too deep error. Since we failed
            // during parsing, we can't log anything about the command.
            LOG(1) << "assertion while parsing command: " << ex.toString();

            _generateErrorResponse(
                opCtx, replyBuilder.get(), ex, metadataBob.obj(), extraFieldsBuilder.obj());

            return;  // From lambda. Don't try executing if parsing failed.
        }

        try {  // Execute.
            curOpCommandSetup(opCtx, request);

            Command* c = nullptr;
            // In the absence of a Command object, no redaction is possible. Therefore
            // to avoid displaying potentially sensitive information in the logs,
            // we restrict the log message to the name of the unrecognized command.
            // However, the complete command object will still be echoed to the client.
            if (!(c = Command::findCommand(request.getCommandName()))) {
                Command::unknownCommands.increment();
                std::string msg = str::stream() << "no such command: '" << request.getCommandName()
                                                << "'";
                LOG(2) << msg;
                uasserted(ErrorCodes::CommandNotFound,
                          str::stream() << msg << ", bad cmd: '" << redact(request.body) << "'");
            }

            LOG(2) << "run command " << request.getDatabase() << ".$cmd" << ' '
                   << redact(c->getRedactedCopyForLogging(request.body));

            {
                // Try to set this as early as possible, as soon as we have figured out the command.
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                CurOp::get(opCtx)->setLogicalOp_inlock(c->getLogicalOp());
            }

            execCommandDatabase(opCtx, c, request, replyBuilder.get());
        } catch (const DBException& ex) {
            BSONObjBuilder metadataBob;
            BSONObjBuilder extraFieldsBuilder;
            appendClusterAndOperationTime(
                opCtx, &extraFieldsBuilder, &metadataBob, LogicalTime::kUninitialized);

            LOG(1) << "assertion while executing command '" << request.getCommandName() << "' "
                   << "on database '" << request.getDatabase() << "': " << ex.toString();

            _generateErrorResponse(
                opCtx, replyBuilder.get(), ex, metadataBob.obj(), extraFieldsBuilder.obj());
        }
    }();

    if (OpMsg::isFlagSet(message, OpMsg::kMoreToCome)) {
        // Close the connection to get client to go through server selection again.
        uassert(ErrorCodes::NotMaster,
                "Not-master error during fire-and-forget command processing",
                !LastError::get(opCtx->getClient()).hadNotMasterError());

        return {};  // Don't reply.
    }

    auto response = replyBuilder->done();
    CurOp::get(opCtx)->debug().responseLength = response.header().dataLen();

    // TODO exhaust
    return DbResponse{std::move(response)};
}

DbResponse receivedQuery(OperationContext* opCtx,
                         const NamespaceString& nss,
                         Client& c,
                         const Message& m) {
    invariant(!nss.isCommand());
    globalOpCounters.gotQuery();
    ServerReadConcernMetrics::get(opCtx)->recordReadConcern(repl::ReadConcernArgs::get(opCtx));

    DbMessage d(m);
    QueryMessage q(d);

    CurOp& op = *CurOp::get(opCtx);
    DbResponse dbResponse;

    try {
        Client* client = opCtx->getClient();
        Status status = AuthorizationSession::get(client)->checkAuthForFind(nss, false);
        audit::logQueryAuthzCheck(client, nss, q.query, status.code());
        uassertStatusOK(status);

        dbResponse.exhaustNS = runQuery(opCtx, q, nss, dbResponse.response);
    } catch (const AssertionException& e) {
        // If we got a stale config, wait in case the operation is stuck in a critical section
        if (!opCtx->getClient()->isInDirectClient() && e.code() == ErrorCodes::StaleConfig) {
            auto& sce = static_cast<const StaleConfigException&>(e);
            ShardingState::get(opCtx)
                ->onStaleShardVersion(opCtx, NamespaceString(sce.getns()), sce.getVersionReceived())
                .transitional_ignore();
        }

        dbResponse.response.reset();
        generateLegacyQueryErrorResponse(&e, q, &op, &dbResponse.response);
    }

    op.debug().responseLength = dbResponse.response.header().dataLen();
    return dbResponse;
}

void receivedKillCursors(OperationContext* opCtx, const Message& m) {
    LastError::get(opCtx->getClient()).disable();
    DbMessage dbmessage(m);
    int n = dbmessage.pullInt();

    uassert(13659, "sent 0 cursors to kill", n != 0);
    massert(13658,
            str::stream() << "bad kill cursors size: " << m.dataSize(),
            m.dataSize() == 8 + (8 * n));
    uassert(13004, str::stream() << "sent negative cursors to kill: " << n, n >= 1);

    if (n > 2000) {
        (n < 30000 ? warning() : error()) << "receivedKillCursors, n=" << n;
        verify(n < 30000);
    }

    const char* cursorArray = dbmessage.getArray(n);

    int found = CursorManager::eraseCursorGlobalIfAuthorized(opCtx, n, cursorArray);

    if (shouldLog(logger::LogSeverity::Debug(1)) || found != n) {
        LOG(found == n ? 1 : 0) << "killcursors: found " << found << " of " << n;
    }
}

void receivedInsert(OperationContext* opCtx, const NamespaceString& nsString, const Message& m) {

	log() << "heejjin) receivedInsert" ;
    auto insertOp = InsertOp::parseLegacy(m);
    invariant(insertOp.getNamespace() == nsString);

    for (const auto& obj : insertOp.getDocuments()) {
        Status status =
            AuthorizationSession::get(opCtx->getClient())->checkAuthForInsert(opCtx, nsString, obj);
        audit::logInsertAuthzCheck(opCtx->getClient(), nsString, obj, status.code());
        uassertStatusOK(status);
    }
    performInserts(opCtx, insertOp);
}

void receivedUpdate(OperationContext* opCtx, const NamespaceString& nsString, const Message& m) {
    auto updateOp = UpdateOp::parseLegacy(m);
    auto& singleUpdate = updateOp.getUpdates()[0];
    invariant(updateOp.getNamespace() == nsString);

    Status status = AuthorizationSession::get(opCtx->getClient())
                        ->checkAuthForUpdate(opCtx,
                                             nsString,
                                             singleUpdate.getQ(),
                                             singleUpdate.getU(),
                                             singleUpdate.getUpsert());
    audit::logUpdateAuthzCheck(opCtx->getClient(),
                               nsString,
                               singleUpdate.getQ(),
                               singleUpdate.getU(),
                               singleUpdate.getUpsert(),
                               singleUpdate.getMulti(),
                               status.code());
    uassertStatusOK(status);

    performUpdates(opCtx, updateOp);
}

void receivedDelete(OperationContext* opCtx, const NamespaceString& nsString, const Message& m) {
    auto deleteOp = DeleteOp::parseLegacy(m);
    auto& singleDelete = deleteOp.getDeletes()[0];
    invariant(deleteOp.getNamespace() == nsString);

    Status status = AuthorizationSession::get(opCtx->getClient())
                        ->checkAuthForDelete(opCtx, nsString, singleDelete.getQ());
    audit::logDeleteAuthzCheck(opCtx->getClient(), nsString, singleDelete.getQ(), status.code());
    uassertStatusOK(status);

    performDeletes(opCtx, deleteOp);
}

DbResponse receivedGetMore(OperationContext* opCtx,
                           const Message& m,
                           CurOp& curop,
                           bool* shouldLogOpDebug) {
    globalOpCounters.gotGetMore();
    DbMessage d(m);

    const char* ns = d.getns();
    int ntoreturn = d.pullInt();
    uassert(
        34419, str::stream() << "Invalid ntoreturn for OP_GET_MORE: " << ntoreturn, ntoreturn >= 0);
    long long cursorid = d.pullInt64();

    curop.debug().ntoreturn = ntoreturn;
    curop.debug().cursorid = cursorid;

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setNS_inlock(ns);
    }

    bool exhaust = false;
    bool isCursorAuthorized = false;

    DbResponse dbresponse;
    try {
        const NamespaceString nsString(ns);
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid ns [" << ns << "]",
                nsString.isValid());

        Status status = AuthorizationSession::get(opCtx->getClient())
                            ->checkAuthForGetMore(nsString, cursorid, false);
        audit::logGetMoreAuthzCheck(opCtx->getClient(), nsString, cursorid, status.code());
        uassertStatusOK(status);

        while (MONGO_FAIL_POINT(rsStopGetMore)) {
            sleepmillis(0);
        }

        dbresponse.response =
            getMore(opCtx, ns, ntoreturn, cursorid, &exhaust, &isCursorAuthorized);
    } catch (AssertionException& e) {
        if (isCursorAuthorized) {
            // If a cursor with id 'cursorid' was authorized, it may have been advanced
            // before an exception terminated processGetMore.  Erase the ClientCursor
            // because it may now be out of sync with the client's iteration state.
            // SERVER-7952
            // TODO Temporary code, see SERVER-4563 for a cleanup overview.
            CursorManager::eraseCursorGlobal(opCtx, cursorid);
        }

        BSONObjBuilder err;
        err.append("$err", e.reason());
        err.append("code", e.code());
        BSONObj errObj = err.obj();

        curop.debug().exceptionInfo = e.toStatus();

        dbresponse = replyToQuery(errObj, ResultFlag_ErrSet);
        curop.debug().responseLength = dbresponse.response.header().dataLen();
        curop.debug().nreturned = 1;
        *shouldLogOpDebug = true;
        return dbresponse;
    }

    curop.debug().responseLength = dbresponse.response.header().dataLen();
    auto queryResult = QueryResult::ConstView(dbresponse.response.buf());
    curop.debug().nreturned = queryResult.getNReturned();

    if (exhaust) {
        curop.debug().exhaust = true;
        dbresponse.exhaustNS = ns;
    }

    return dbresponse;
}

}  // namespace

DbResponse ServiceEntryPointMongod::handleRequest(OperationContext* opCtx, const Message& m) {
    // before we lock...
    NetworkOp op = m.operation();
    bool isCommand = false;

    DbMessage dbmsg(m);

    Client& c = *opCtx->getClient();
    if (c.isInDirectClient()) {
        invariant(!opCtx->lockState()->inAWriteUnitOfWork());
    } else {
        LastError::get(c).startRequest();
        AuthorizationSession::get(c)->startRequest(opCtx);

        // We should not be holding any locks at this point
        invariant(!opCtx->lockState()->isLocked());
    }

    const char* ns = dbmsg.messageShouldHaveNs() ? dbmsg.getns() : NULL;
    const NamespaceString nsString = ns ? NamespaceString(ns) : NamespaceString();

    if (op == dbQuery) {
        if (nsString.isCommand()) {
            isCommand = true;
        }
    } else if (op == dbCommand || op == dbMsg) {
        isCommand = true;
    }

    CurOp& currentOp = *CurOp::get(opCtx);
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        // Commands handling code will reset this if the operation is a command
        // which is logically a basic CRUD operation like query, insert, etc.
        currentOp.setNetworkOp_inlock(op);
        currentOp.setLogicalOp_inlock(networkOpToLogicalOp(op));
    }

    OpDebug& debug = currentOp.debug();

    long long logThresholdMs = serverGlobalParams.slowMS;
    bool shouldLogOpDebug = shouldLog(logger::LogSeverity::Debug(1));

    DbResponse dbresponse;
    if (op == dbMsg || op == dbCommand || (op == dbQuery && isCommand)) {
        dbresponse = runCommands(opCtx, m);
    } else if (op == dbQuery) {
        invariant(!isCommand);
        dbresponse = receivedQuery(opCtx, nsString, c, m);
    } else if (op == dbGetMore) {
        dbresponse = receivedGetMore(opCtx, m, currentOp, &shouldLogOpDebug);
    } else {
        // The remaining operations do not return any response. They are fire-and-forget.
        try {
            if (op == dbKillCursors) {
                currentOp.ensureStarted();
                logThresholdMs = 10;
                receivedKillCursors(opCtx, m);
            } else if (op != dbInsert && op != dbUpdate && op != dbDelete) {
                log() << "    operation isn't supported: " << static_cast<int>(op);
                currentOp.done();
                shouldLogOpDebug = true;
            } else {
                if (!opCtx->getClient()->isInDirectClient()) {
                    uassert(18663,
                            str::stream() << "legacy writeOps not longer supported for "
                                          << "versioned connections, ns: "
                                          << nsString.ns()
                                          << ", op: "
                                          << networkOpToString(op),
                            !ShardedConnectionInfo::get(&c, false));
                }

                if (!nsString.isValid()) {
                    uassert(16257, str::stream() << "Invalid ns [" << ns << "]", false);
                } else if (op == dbInsert) {
                    receivedInsert(opCtx, nsString, m);
                } else if (op == dbUpdate) {
                    receivedUpdate(opCtx, nsString, m);
                } else if (op == dbDelete) {
                    receivedDelete(opCtx, nsString, m);
                } else {
                    invariant(false);
                }
            }
        } catch (const AssertionException& ue) {
            LastError::get(c).setLastError(ue.code(), ue.reason());
            LOG(3) << " Caught Assertion in " << networkOpToString(op) << ", continuing "
                   << redact(ue);
            debug.exceptionInfo = ue.toStatus();
        }
    }
    currentOp.ensureStarted();
    currentOp.done();
    debug.executionTimeMicros = durationCount<Microseconds>(currentOp.elapsedTimeExcludingPauses());

    Top::get(opCtx->getServiceContext())
        .incrementGlobalLatencyStats(
            opCtx,
            durationCount<Microseconds>(currentOp.elapsedTimeExcludingPauses()),
            currentOp.getReadWriteType());

    const bool shouldSample = serverGlobalParams.sampleRate == 1.0
        ? true
        : c.getPrng().nextCanonicalDouble() < serverGlobalParams.sampleRate;

    if (shouldLogOpDebug || (shouldSample && debug.executionTimeMicros > logThresholdMs * 1000LL)) {
        Locker::LockerInfo lockerInfo;
        opCtx->lockState()->getLockerInfo(&lockerInfo, currentOp.getLockStatsBase());
        log() << debug.report(&c, currentOp, lockerInfo.stats);
    }

    if (currentOp.shouldDBProfile(shouldSample)) {
        // Performance profiling is on
        if (opCtx->lockState()->isReadLocked()) {
            LOG(1) << "note: not profiling because recursive read lock";
        } else if (lockedForWriting()) {
            // TODO SERVER-26825: Fix race condition where fsyncLock is acquired post
            // lockedForWriting() call but prior to profile collection lock acquisition.
            LOG(1) << "note: not profiling because doing fsync+lock";
        } else if (storageGlobalParams.readOnly) {
            LOG(1) << "note: not profiling because server is read-only";
        } else {
            profile(opCtx, op);
        }
    }

    recordCurOpMetrics(opCtx);
    return dbresponse;
}

}  // namespace mongo

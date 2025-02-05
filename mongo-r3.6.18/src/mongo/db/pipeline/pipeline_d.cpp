
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/pipeline_d.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_iterator.h"
#include "mongo/db/exec/multi_iterator.h"
#include "mongo/db/exec/shard_filter.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_merge_cursors.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_sample_from_random_cursor.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/metadata_manager.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/fill_locker_info.h"
#include "mongo/db/stats/storage_stats.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/rpc/metadata/client_metadata_ismaster.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/time_support.h"

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::string;
using std::unique_ptr;

namespace {

class MongodProcessInterface final
    : public DocumentSourceNeedsMongoProcessInterface::MongoProcessInterface {
public:
    MongodProcessInterface(const intrusive_ptr<ExpressionContext>& ctx)
        : _ctx(ctx), _client(ctx->opCtx) {}

    void setOperationContext(OperationContext* opCtx) {
        invariant(_ctx->opCtx == opCtx);
        _client.setOpCtx(opCtx);
    }

    DBClientBase* directClient() final {
        return &_client;
    }

    bool isSharded(const NamespaceString& nss) final {
        AutoGetCollectionForReadCommand autoColl(_ctx->opCtx, nss);
        // TODO SERVER-24960: Use CollectionShardingState::collectionIsSharded() to confirm sharding
        // state.
        auto css = CollectionShardingState::get(_ctx->opCtx, nss);
        return bool(css->getMetadata());
    }

    BSONObj insert(const NamespaceString& ns, const std::vector<BSONObj>& objs) final {
        boost::optional<DisableDocumentValidation> maybeDisableValidation;
        if (_ctx->bypassDocumentValidation)
            maybeDisableValidation.emplace(_ctx->opCtx);

        _client.insert(ns.ns(), objs);
        return _client.getLastErrorDetailed();
    }

    CollectionIndexUsageMap getIndexStats(OperationContext* opCtx,
                                          const NamespaceString& ns) final {
        AutoGetCollectionForReadCommand autoColl(opCtx, ns);

        Collection* collection = autoColl.getCollection();
        if (!collection) {
            LOG(2) << "Collection not found on index stats retrieval: " << ns.ns();
            return CollectionIndexUsageMap();
        }

        return collection->infoCache()->getIndexUsageStats();
    }

    void appendLatencyStats(const NamespaceString& nss,
                            bool includeHistograms,
                            BSONObjBuilder* builder) const final {
        Top::get(_ctx->opCtx->getServiceContext())
            .appendLatencyStats(nss.ns(), includeHistograms, builder);
    }

    Status appendStorageStats(const NamespaceString& nss,
                              const BSONObj& param,
                              BSONObjBuilder* builder) const final {
        return appendCollectionStorageStats(_ctx->opCtx, nss, param, builder);
    }

    Status appendRecordCount(const NamespaceString& nss, BSONObjBuilder* builder) const final {
        return appendCollectionRecordCount(_ctx->opCtx, nss, builder);
    }

    BSONObj getCollectionOptions(const NamespaceString& nss) final {
        const auto infos =
            _client.getCollectionInfos(nss.db().toString(), BSON("name" << nss.coll()));
        return infos.empty() ? BSONObj() : infos.front().getObjectField("options").getOwned();
    }

    Status renameIfOptionsAndIndexesHaveNotChanged(
        const BSONObj& renameCommandObj,
        const NamespaceString& targetNs,
        const BSONObj& originalCollectionOptions,
        const std::list<BSONObj>& originalIndexes) final {
        Lock::GlobalWrite globalLock(_ctx->opCtx);

        if (SimpleBSONObjComparator::kInstance.evaluate(originalCollectionOptions !=
                                                        getCollectionOptions(targetNs))) {
            return {ErrorCodes::CommandFailed,
                    str::stream() << "collection options of target collection " << targetNs.ns()
                                  << " changed during processing. Original options: "
                                  << originalCollectionOptions
                                  << ", new options: "
                                  << getCollectionOptions(targetNs)};
        }

        auto currentIndexes = _client.getIndexSpecs(targetNs.ns());
        if (originalIndexes.size() != currentIndexes.size() ||
            !std::equal(originalIndexes.begin(),
                        originalIndexes.end(),
                        currentIndexes.begin(),
                        SimpleBSONObjComparator::kInstance.makeEqualTo())) {
            return {ErrorCodes::CommandFailed,
                    str::stream() << "indexes of target collection " << targetNs.ns()
                                  << " changed during processing."};
        }

        BSONObj info;
        bool ok = _client.runCommand("admin", renameCommandObj, info);
        return ok ? Status::OK() : Status{ErrorCodes::CommandFailed,
                                          str::stream() << "renameCollection failed: " << info};
    }

    StatusWith<std::unique_ptr<Pipeline, Pipeline::Deleter>> makePipeline(
        const std::vector<BSONObj>& rawPipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const MakePipelineOptions opts = MakePipelineOptions{}) final {
        // 'expCtx' may represent the settings for an aggregation pipeline on a different namespace
        // than the DocumentSource this MongodProcessInterface is injected into, but both
        // ExpressionContext instances should still have the same OperationContext.
        invariant(_ctx->opCtx == expCtx->opCtx);

        auto pipeline = Pipeline::parse(rawPipeline, expCtx);
        if (!pipeline.isOK()) {
            return pipeline.getStatus();
        }

        if (opts.optimize) {
            pipeline.getValue()->optimizePipeline();
        }

        Status cursorStatus = Status::OK();

        if (opts.attachCursorSource) {
            cursorStatus = attachCursorSourceToPipeline(expCtx, pipeline.getValue().get());
        } else if (opts.forceInjectMongoProcessInterface) {
            PipelineD::injectMongodInterface(pipeline.getValue().get());
        }

        return cursorStatus.isOK() ? std::move(pipeline) : cursorStatus;
    }

    Status attachCursorSourceToPipeline(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                        Pipeline* pipeline) final {
        invariant(_ctx->opCtx == expCtx->opCtx);
        invariant(pipeline->getSources().empty() ||
                  !dynamic_cast<DocumentSourceCursor*>(pipeline->getSources().front().get()));

        boost::optional<AutoGetCollectionForReadCommand> autoColl;
        if (expCtx->uuid) {
            autoColl.emplace(expCtx->opCtx,
                             expCtx->ns.db(),
                             *expCtx->uuid,
                             AutoStatsTracker::LogMode::kUpdateTop);
            if (autoColl->getCollection() == nullptr) {
                // The UUID doesn't exist anymore.
                return {ErrorCodes::NamespaceNotFound,
                        "No namespace with UUID " + expCtx->uuid->toString()};
            }
        } else {
            autoColl.emplace(expCtx->opCtx, expCtx->ns, AutoStatsTracker::LogMode::kUpdateTop);
        }

        // makePipeline() is only called to perform secondary aggregation requests and expects the
        // collection representing the document source to be not-sharded. We confirm sharding state
        // here to avoid taking a collection lock elsewhere for this purpose alone.
        // TODO SERVER-27616: This check is incorrect in that we don't acquire a collection cursor
        // until after we release the lock, leaving room for a collection to be sharded inbetween.
        // TODO SERVER-24960: Use CollectionShardingState::collectionIsSharded() to confirm sharding
        // state.
        auto css = CollectionShardingState::get(_ctx->opCtx, expCtx->ns);
        uassert(4567,
                str::stream() << "from collection (" << expCtx->ns.ns() << ") cannot be sharded",
                !bool(css->getMetadata()));

        PipelineD::prepareCursorSource(autoColl->getCollection(), expCtx->ns, nullptr, pipeline);
        // Optimize again, since there may be additional optimizations that can be done after adding
        // the initial cursor stage.
        pipeline->optimizePipeline();

        return Status::OK();
    }

    std::vector<BSONObj> getCurrentOps(CurrentOpConnectionsMode connMode,
                                       CurrentOpUserMode userMode,
                                       CurrentOpTruncateMode truncateMode) const {
        AuthorizationSession* ctxAuth = AuthorizationSession::get(_ctx->opCtx->getClient());

        const std::string hostName = getHostNameCachedAndPort();

        std::vector<BSONObj> ops;

        for (ServiceContext::LockedClientsCursor cursor(
                 _ctx->opCtx->getClient()->getServiceContext());
             Client* client = cursor.next();) {
            invariant(client);

            stdx::lock_guard<Client> lk(*client);

            // If auth is disabled, ignore the allUsers parameter.
            if (ctxAuth->getAuthorizationManager().isAuthEnabled() &&
                userMode == CurrentOpUserMode::kExcludeOthers &&
                !ctxAuth->isCoauthorizedWithClient(client, lk)) {
                continue;
            }

            const OperationContext* clientOpCtx = client->getOperationContext();

            if (!clientOpCtx && connMode == CurrentOpConnectionsMode::kExcludeIdle) {
                continue;
            }

            BSONObjBuilder infoBuilder;

            infoBuilder.append("host", hostName);

            client->reportState(infoBuilder);

            const auto& clientMetadata =
                ClientMetadataIsMasterState::get(client).getClientMetadata();

            if (clientMetadata) {
                auto appName = clientMetadata.get().getApplicationName();
                if (!appName.empty()) {
                    infoBuilder.append("appName", appName);
                }

                auto clientMetadataDocument = clientMetadata.get().getDocument();
                infoBuilder.append("clientMetadata", clientMetadataDocument);
            }

            // Fill out the rest of the BSONObj with opCtx specific details.
            infoBuilder.appendBool("active", static_cast<bool>(clientOpCtx));
            infoBuilder.append(
                "currentOpTime",
                _ctx->opCtx->getServiceContext()->getPreciseClockSource()->now().toString());

            if (clientOpCtx) {
                infoBuilder.append("opid", clientOpCtx->getOpID());
                if (clientOpCtx->isKillPending()) {
                    infoBuilder.append("killPending", true);
                }

                if (clientOpCtx->getLogicalSessionId()) {
                    BSONObjBuilder bob(infoBuilder.subobjStart("lsid"));
                    clientOpCtx->getLogicalSessionId()->serialize(&bob);
                }

                CurOp::get(clientOpCtx)
                    ->reportState(&infoBuilder,
                                  (truncateMode == CurrentOpTruncateMode::kTruncateOps));

                Locker::LockerInfo lockerInfo;
                clientOpCtx->lockState()->getLockerInfo(
                    &lockerInfo, CurOp::get(clientOpCtx)->getLockStatsBase());
                fillLockerInfo(lockerInfo, infoBuilder);
            }

            ops.emplace_back(infoBuilder.obj());
        }

        return ops;
    }

    std::string getShardName(OperationContext* opCtx) const {
        if (ShardingState::get(opCtx)->enabled()) {
            return ShardingState::get(opCtx)->getShardName();
        }

        return std::string();
    }

    std::vector<FieldPath> collectDocumentKeyFields(UUID uuid) const final {
        if (!ShardingState::get(_ctx->opCtx)->enabled()) {
            return {"_id"};  // Nothing is sharded.
        }

        auto scm = [this]() -> ScopedCollectionMetadata {
            AutoGetCollection autoColl(_ctx->opCtx, _ctx->ns, MODE_IS);
            return CollectionShardingState::get(_ctx->opCtx, _ctx->ns)->getMetadata();
        }();

        if (!scm) {
            return {"_id"};  // Collection is not sharded.
        }

        uassert(ErrorCodes::InvalidUUID,
                str::stream() << "Collection " << _ctx->ns.ns()
                              << " UUID differs from UUID on change stream operations",
                scm->uuidMatches(uuid));

        // Unpack the shard key.
        std::vector<FieldPath> result;
        bool gotId = false;
        for (auto& field : scm->getKeyPatternFields()) {
            result.emplace_back(field->dottedField());
            gotId |= (result.back().fullPath() == "_id");
        }
        if (!gotId) {  // If not part of the shard key, "_id" comes last.
            result.emplace_back("_id");
        }
        return result;
    }

    boost::optional<Document> lookupSingleDocument(const NamespaceString& nss,
                                                   UUID collectionUUID,
                                                   const Document& documentKey,
                                                   boost::optional<BSONObj> readConcern) final {
        invariant(!readConcern);  // We don't currently support a read concern on mongod - it's only
                                  // expected to be necessary on mongos.
                                  //
        // Be sure to do the lookup using the collection default collation.
        auto foreignExpCtx =
            _ctx->copyWith(nss, collectionUUID, _getCollectionDefaultCollator(nss, collectionUUID));
        auto swPipeline = makePipeline({BSON("$match" << documentKey)}, foreignExpCtx);
        if (swPipeline == ErrorCodes::NamespaceNotFound) {
            return boost::none;
        }
        auto pipeline = uassertStatusOK(std::move(swPipeline));

        auto lookedUpDocument = pipeline->getNext();
        if (auto next = pipeline->getNext()) {
            uasserted(ErrorCodes::ChangeStreamFatalError,
                      str::stream() << "found more than one document with document key "
                                    << documentKey.toString()
                                    << " ["
                                    << lookedUpDocument->toString()
                                    << ", "
                                    << next->toString()
                                    << "]");
        }
        return lookedUpDocument;
    }

private:
    /**
     * Looks up the collection default collator for the collection given by 'collectionUUID'. A
     * collection's default collation is not allowed to change, so we cache the result to allow for
     * quick lookups in the future. Looks up the collection by UUID, and returns 'nullptr' if the
     * collection does not exist or if the collection's default collation is the simple collation.
     */
    std::unique_ptr<CollatorInterface> _getCollectionDefaultCollator(const NamespaceString& nss,
                                                                     UUID collectionUUID) {
        if (_collatorCache.find(collectionUUID) == _collatorCache.end()) {
            AutoGetCollection autoColl(_ctx->opCtx, nss, collectionUUID, MODE_IS);
            if (!autoColl.getCollection()) {
                // This collection doesn't exist - since we looked up by UUID, it will never exist
                // in the future, so we cache a null pointer as the default collation.
                _collatorCache[collectionUUID] = nullptr;
            } else {
                auto defaultCollator = autoColl.getCollection()->getDefaultCollator();
                // Clone the collator so that we can safely use the pointer if the collection
                // disappears right after we release the lock.
                _collatorCache[collectionUUID] =
                    defaultCollator ? defaultCollator->clone() : nullptr;
            }
        }
        return _collatorCache[collectionUUID] ? _collatorCache[collectionUUID]->clone() : nullptr;
    }

    intrusive_ptr<ExpressionContext> _ctx;
    DBDirectClient _client;
    std::map<UUID, std::unique_ptr<const CollatorInterface>> _collatorCache;
};

/**
 * Returns a PlanExecutor which uses a random cursor to sample documents if successful. Returns {}
 * if the storage engine doesn't support random cursors, or if 'sampleSize' is a large enough
 * percentage of the collection.
 */
StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> createRandomCursorExecutor(
    Collection* collection, OperationContext* opCtx, long long sampleSize, long long numRecords) {
    // Verify that we are already under a collection lock. We avoid taking locks ourselves in this
    // function because double-locking forces any PlanExecutor we create to adopt a NO_YIELD policy.
    invariant(opCtx->lockState()->isCollectionLockedForMode(collection->ns().ns(), MODE_IS));

    double kMaxSampleRatioForRandCursor = 0.05;
    if (sampleSize > numRecords * kMaxSampleRatioForRandCursor || numRecords <= 100) {
        return {nullptr};
    }

    // Attempt to get a random cursor from the RecordStore. If the RecordStore does not support
    // random cursors, attempt to get one from the _id index.
    std::unique_ptr<RecordCursor> rsRandCursor =
        collection->getRecordStore()->getRandomCursor(opCtx);

    auto ws = stdx::make_unique<WorkingSet>();
    std::unique_ptr<PlanStage> stage;

    if (rsRandCursor) {
        stage = stdx::make_unique<MultiIteratorStage>(opCtx, ws.get(), collection);
        static_cast<MultiIteratorStage*>(stage.get())->addIterator(std::move(rsRandCursor));

    } else {
        auto indexCatalog = collection->getIndexCatalog();
        auto indexDescriptor = indexCatalog->findIdIndex(opCtx);

        if (!indexDescriptor) {
            // There was no _id index.
            return {nullptr};
        }

        IndexAccessMethod* idIam = indexCatalog->getIndex(indexDescriptor);
        auto idxRandCursor = idIam->newRandomCursor(opCtx);

        if (!idxRandCursor) {
            // Storage engine does not support any type of random cursor.
            return {nullptr};
        }

        auto idxIterator = stdx::make_unique<IndexIteratorStage>(opCtx,
                                                                 ws.get(),
                                                                 collection,
                                                                 idIam,
                                                                 indexDescriptor->keyPattern(),
                                                                 std::move(idxRandCursor));
        stage = stdx::make_unique<FetchStage>(
            opCtx, ws.get(), idxIterator.release(), nullptr, collection);
    }

    // If we're in a sharded environment, we need to filter out documents we don't own.
    if (ShardingState::get(opCtx)->needCollectionMetadata(opCtx, collection->ns().ns())) {
        auto shardFilterStage = stdx::make_unique<ShardFilterStage>(
            opCtx,
            CollectionShardingState::get(opCtx, collection->ns())->getMetadata(),
            ws.get(),
            stage.release());
        return PlanExecutor::make(opCtx,
                                  std::move(ws),
                                  std::move(shardFilterStage),
                                  collection,
                                  PlanExecutor::YIELD_AUTO);
    }

    return PlanExecutor::make(
        opCtx, std::move(ws), std::move(stage), collection, PlanExecutor::YIELD_AUTO);
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> attemptToGetExecutor(
    OperationContext* opCtx,
    Collection* collection,
    const NamespaceString& nss,
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    bool oplogReplay,
    BSONObj queryObj,
    BSONObj projectionObj,
    BSONObj sortObj,
    const AggregationRequest* aggRequest,
    const size_t plannerOpts) {
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setTailableMode(pExpCtx->tailableMode);
    qr->setOplogReplay(oplogReplay);
    qr->setFilter(queryObj);
    qr->setProj(projectionObj);
    qr->setSort(sortObj);
    if (aggRequest) {
        qr->setExplain(static_cast<bool>(aggRequest->getExplain()));
        qr->setHint(aggRequest->getHint());
    }

    // If the pipeline has a non-null collator, set the collation option to the result of
    // serializing the collator's spec back into BSON. We do this in order to fill in all options
    // that the user omitted.
    //
    // If pipeline has a null collator (representing the "simple" collation), we simply set the
    // collation option to the original user BSON, which is either the empty object (unspecified),
    // or the specification for the "simple" collation.
    qr->setCollation(pExpCtx->getCollator() ? pExpCtx->getCollator()->getSpec().toBSON()
                                            : pExpCtx->collation);

    const ExtensionsCallbackReal extensionsCallback(pExpCtx->opCtx, &nss);

    auto cq = CanonicalQuery::canonicalize(
        opCtx, std::move(qr), pExpCtx, extensionsCallback, Pipeline::kAllowedMatcherFeatures);

    if (!cq.isOK()) {
        // Return an error instead of uasserting, since there are cases where the combination of
        // sort and projection will result in a bad query, but when we try with a different
        // combination it will be ok. e.g. a sort by {$meta: 'textScore'}, without any projection
        // will fail, but will succeed when the corresponding '$meta' projection is passed in
        // another attempt.
        return {cq.getStatus()};
    }

    return getExecutorFind(
        opCtx, collection, nss, std::move(cq.getValue()), PlanExecutor::YIELD_AUTO, plannerOpts);
}

BSONObj removeSortKeyMetaProjection(BSONObj projectionObj) {
    if (!projectionObj[Document::metaFieldSortKey]) {
        return projectionObj;
    }
    return projectionObj.removeField(Document::metaFieldSortKey);
}
}  // namespace

void PipelineD::injectMongodInterface(Pipeline* pipeline) {
    for (auto&& source : pipeline->_sources) {
        if (auto needsMongod =
                dynamic_cast<DocumentSourceNeedsMongoProcessInterface*>(source.get())) {
            needsMongod->injectMongoProcessInterface(
                std::make_shared<MongodProcessInterface>(pipeline->getContext()));
        }
    }
}

void PipelineD::prepareCursorSource(Collection* collection,
                                    const NamespaceString& nss,
                                    const AggregationRequest* aggRequest,
                                    Pipeline* pipeline) {
    auto expCtx = pipeline->getContext();

    // We will be modifying the source vector as we go.
    Pipeline::SourceContainer& sources = pipeline->_sources;

    // Inject a MongodProcessInterface to sources that need them.
    injectMongodInterface(pipeline);

    if (!sources.empty() && !sources.front()->constraints().requiresInputDocSource) {
        return;
    }

    // We are going to generate an input cursor, so we need to be holding the collection lock.
    dassert(expCtx->opCtx->lockState()->isCollectionLockedForMode(nss.ns(), MODE_IS));

    if (!sources.empty()) {
        auto sampleStage = dynamic_cast<DocumentSourceSample*>(sources.front().get());
        // Optimize an initial $sample stage if possible.
        if (collection && sampleStage) {
            const long long sampleSize = sampleStage->getSampleSize();
            const long long numRecords = collection->getRecordStore()->numRecords(expCtx->opCtx);
            auto exec = uassertStatusOK(
                createRandomCursorExecutor(collection, expCtx->opCtx, sampleSize, numRecords));
            if (exec) {
                // Replace $sample stage with $sampleFromRandomCursor stage.
                sources.pop_front();
                std::string idString = collection->ns().isOplog() ? "ts" : "_id";
                sources.emplace_front(DocumentSourceSampleFromRandomCursor::create(
                    expCtx, sampleSize, idString, numRecords));

                addCursorSource(
                    collection,
                    pipeline,
                    expCtx,
                    std::move(exec),
                    pipeline->getDependencies(DepsTracker::MetadataAvailable::kNoMetadata),
                    BSONObj(),  // queryObj
                    BSONObj(),  // sortObj
                    BSONObj(),  // projectionObj
                    // Do not allow execution-level explains on this DocumentSourceCursor,
                    // as it contains a RandomCursor, which will never return EOF.
                    true);
                return;
            }
        }
    }

    // Look for an initial match. This works whether we got an initial query or not. If not, it
    // results in a "{}" query, which will be what we want in that case.
    bool oplogReplay = false;
    const BSONObj queryObj = pipeline->getInitialQuery();
    if (!queryObj.isEmpty()) {
        auto matchStage = dynamic_cast<DocumentSourceMatch*>(sources.front().get());
        if (matchStage) {
            oplogReplay = dynamic_cast<DocumentSourceOplogMatch*>(matchStage) != nullptr;
            // If a $match query is pulled into the cursor, the $match is redundant, and can be
            // removed from the pipeline.
            sources.pop_front();
        } else {
            // A $geoNear stage, the only other stage that can produce an initial query, is also
            // a valid initial stage and will be handled above.
            MONGO_UNREACHABLE;
        }
    }

    // Find the set of fields in the source documents depended on by this pipeline.
    DepsTracker deps = pipeline->getDependencies(DocumentSourceMatch::isTextQuery(queryObj)
                                                     ? DepsTracker::MetadataAvailable::kTextScore
                                                     : DepsTracker::MetadataAvailable::kNoMetadata);

    BSONObj projForQuery = deps.toProjection();

    // Look for an initial sort; we'll try to add this to the Cursor we create. If we're successful
    // in doing that, we'll remove the $sort from the pipeline, because the documents will already
    // come sorted in the specified order as a result of the index scan.
    intrusive_ptr<DocumentSourceSort> sortStage;
    BSONObj sortObj;
    if (!sources.empty()) {
        sortStage = dynamic_cast<DocumentSourceSort*>(sources.front().get());
        if (sortStage) {
            sortObj = sortStage
                          ->sortKeyPattern(
                              DocumentSourceSort::SortKeySerialization::kForPipelineSerialization)
                          .toBson();
        }
    }

    // Create the PlanExecutor.
    auto exec = uassertStatusOK(prepareExecutor(expCtx->opCtx,
                                                collection,
                                                nss,
                                                pipeline,
                                                expCtx,
                                                oplogReplay,
                                                sortStage,
                                                deps,
                                                queryObj,
                                                aggRequest,
                                                &sortObj,
                                                &projForQuery));

    // There may be fewer dependencies now if the sort was covered.
    if (!sortObj.isEmpty()) {
        LOG(5) << "Agg: recomputing dependencies due to a covered sort: " << redact(sortObj)
               << ". Current projection: " << redact(projForQuery)
               << ". Current dependencies: " << redact(deps.toProjection());
        deps = pipeline->getDependencies(DocumentSourceMatch::isTextQuery(queryObj)
                                             ? DepsTracker::MetadataAvailable::kTextScore
                                             : DepsTracker::MetadataAvailable::kNoMetadata);
    }


    if (!projForQuery.isEmpty() && !sources.empty()) {
        // Check for redundant $project in query with the same specification as the inclusion
        // projection generated by the dependency optimization.
        auto proj =
            dynamic_cast<DocumentSourceSingleDocumentTransformation*>(sources.front().get());
        if (proj && proj->isSubsetOfProjection(projForQuery)) {
            sources.pop_front();
        }
    }

    addCursorSource(
        collection, pipeline, expCtx, std::move(exec), deps, queryObj, sortObj, projForQuery);
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> PipelineD::prepareExecutor(
    OperationContext* opCtx,
    Collection* collection,
    const NamespaceString& nss,
    Pipeline* pipeline,
    const intrusive_ptr<ExpressionContext>& expCtx,
    bool oplogReplay,
    const intrusive_ptr<DocumentSourceSort>& sortStage,
    const DepsTracker& deps,
    const BSONObj& queryObj,
    const AggregationRequest* aggRequest,
    BSONObj* sortObj,
    BSONObj* projectionObj) {
    // The query system has the potential to use an index to provide a non-blocking sort and/or to
    // use the projection to generate a covered plan. If this is possible, it is more efficient to
    // let the query system handle those parts of the pipeline. If not, it is more efficient to use
    // a $sort and/or a ParsedDeps object. Thus, we will determine whether the query system can
    // provide a non-blocking sort or a covered projection before we commit to a PlanExecutor.
    //
    // To determine if the query system can provide a non-blocking sort, we pass the
    // NO_BLOCKING_SORT planning option, meaning 'getExecutor' will not produce a PlanExecutor if
    // the query system would use a blocking sort stage.
    //
    // To determine if the query system can provide a covered projection, we pass the
    // NO_UNCOVERED_PROJECTS planning option, meaning 'getExecutor' will not produce a PlanExecutor
    // if the query system would need to fetch the document to do the projection. The following
    // logic uses the above strategies, with multiple calls to 'attemptToGetExecutor' to determine
    // the most efficient way to handle the $sort and $project stages.
    //
    // LATER - We should attempt to determine if the results from the query are returned in some
    // order so we can then apply other optimizations there are tickets for, such as SERVER-4507.
    size_t plannerOpts = QueryPlannerParams::DEFAULT | QueryPlannerParams::NO_BLOCKING_SORT;

    if (deps.hasNoRequirements()) {
        // If we don't need any fields from the input document, performing a count is faster, and
        // will output empty documents, which is okay.
        plannerOpts |= QueryPlannerParams::IS_COUNT;
    }

    // The only way to get a text score or the sort key is to let the query system handle the
    // projection. In all other cases, unless the query system can do an index-covered projection
    // and avoid going to the raw record at all, it is faster to have ParsedDeps filter the fields
    // we need.
    if (!deps.getNeedTextScore() && !deps.getNeedSortKey()) {
        plannerOpts |= QueryPlannerParams::NO_UNCOVERED_PROJECTIONS;
    }

    if (expCtx->needsMerge && expCtx->tailableMode == TailableMode::kTailableAndAwaitData) {
        plannerOpts |= QueryPlannerParams::TRACK_LATEST_OPLOG_TS;
    }

    const BSONObj emptyProjection;
    const BSONObj metaSortProjection = BSON("$meta"
                                            << "sortKey");
    if (sortStage) {
        // See if the query system can provide a non-blocking sort.
        auto swExecutorSort =
            attemptToGetExecutor(opCtx,
                                 collection,
                                 nss,
                                 expCtx,
                                 oplogReplay,
                                 queryObj,
                                 expCtx->needsMerge ? metaSortProjection : emptyProjection,
                                 *sortObj,
                                 aggRequest,
                                 plannerOpts);

        if (swExecutorSort.isOK()) {
            // Success! Now see if the query system can also cover the projection.
            auto swExecutorSortAndProj = attemptToGetExecutor(opCtx,
                                                              collection,
                                                              nss,
                                                              expCtx,
                                                              oplogReplay,
                                                              queryObj,
                                                              *projectionObj,
                                                              *sortObj,
                                                              aggRequest,
                                                              plannerOpts);

            std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec;
            if (swExecutorSortAndProj.isOK()) {
                // Success! We have a non-blocking sort and a covered projection.
                LOG(5) << "Agg: Have a non-blocking sort and covered projection";
                exec = std::move(swExecutorSortAndProj.getValue());
            } else if (swExecutorSortAndProj == ErrorCodes::QueryPlanKilled) {
                return {ErrorCodes::OperationFailed,
                        str::stream() << "Failed to determine whether query system can provide a "
                                         "covered projection in addition to a non-blocking sort: "
                                      << swExecutorSortAndProj.getStatus().toString()};
            } else {
                // The query system couldn't cover the projection.
                LOG(5) << "Agg: The query system found a non-blocking sort but couldn't cover the "
                          "projection";
                *projectionObj = BSONObj();
                exec = std::move(swExecutorSort.getValue());
            }

            // We know the sort is being handled by the query system, so remove the $sort stage.
            pipeline->_sources.pop_front();

            if (sortStage->getLimitSrc()) {
                // We need to reinsert the coalesced $limit after removing the $sort.
                pipeline->_sources.push_front(sortStage->getLimitSrc());
            }
            return std::move(exec);
        } else if (swExecutorSort == ErrorCodes::QueryPlanKilled) {
            return {
                ErrorCodes::OperationFailed,
                str::stream()
                    << "Failed to determine whether query system can provide a non-blocking sort: "
                    << swExecutorSort.getStatus().toString()};
        }
        // The query system can't provide a non-blocking sort.
        *sortObj = BSONObj();
    }
    LOG(5) << "Agg: The query system couldn't cover the sort";

    // Either there was no $sort stage, or the query system could not provide a non-blocking
    // sort.
    dassert(sortObj->isEmpty());
    *projectionObj = removeSortKeyMetaProjection(*projectionObj);
    if (deps.getNeedSortKey() && !deps.getNeedTextScore()) {
        // A sort key requirement would have prevented us from being able to add this parameter
        // before, but now we know the query system won't cover the sort, so we will be able to
        // compute the sort key ourselves during the $sort stage, and thus don't need a query
        // projection to do so.
        plannerOpts |= QueryPlannerParams::NO_UNCOVERED_PROJECTIONS;
    }

    // See if the query system can cover the projection.
    auto swExecutorProj = attemptToGetExecutor(opCtx,
                                               collection,
                                               nss,
                                               expCtx,
                                               oplogReplay,
                                               queryObj,
                                               *projectionObj,
                                               *sortObj,
                                               aggRequest,
                                               plannerOpts);
    if (swExecutorProj.isOK()) {
        // Success! We have a covered projection.
        return std::move(swExecutorProj.getValue());
    } else if (swExecutorProj == ErrorCodes::QueryPlanKilled) {
        return {ErrorCodes::OperationFailed,
                str::stream()
                    << "Failed to determine whether query system can provide a covered projection: "
                    << swExecutorProj.getStatus().toString()};
    }

    // The query system couldn't provide a covered projection.
    *projectionObj = BSONObj();
    // If this doesn't work, nothing will.
    return attemptToGetExecutor(opCtx,
                                collection,
                                nss,
                                expCtx,
                                oplogReplay,
                                queryObj,
                                *projectionObj,
                                *sortObj,
                                aggRequest,
                                plannerOpts);
}

void PipelineD::addCursorSource(Collection* collection,
                                Pipeline* pipeline,
                                const intrusive_ptr<ExpressionContext>& expCtx,
                                unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
                                DepsTracker deps,
                                const BSONObj& queryObj,
                                const BSONObj& sortObj,
                                const BSONObj& projectionObj,
                                bool failsForExecutionLevelExplain) {
    // DocumentSourceCursor expects a yielding PlanExecutor that has had its state saved.
    exec->saveState();

    // Put the PlanExecutor into a DocumentSourceCursor and add it to the front of the pipeline.
    intrusive_ptr<DocumentSourceCursor> pSource = DocumentSourceCursor::create(
        collection, std::move(exec), expCtx, failsForExecutionLevelExplain);

    // Add the cursor to the pipeline first so that it's correctly disposed of as part of the
    // pipeline if an exception is thrown during this method.
    pipeline->addInitialSource(pSource);

    pSource->setQuery(queryObj);
    pSource->setSort(sortObj);
    if (deps.hasNoRequirements()) {
        pSource->shouldProduceEmptyDocs();
    }

    if (!projectionObj.isEmpty()) {
        pSource->setProjection(projectionObj, boost::none);
        LOG(5) << "Agg: Setting projection with no dependencies: " << redact(projectionObj);
    } else {
        LOG(5) << "Agg: Setting projection with dependencies: " << redact(deps.toProjection());
        pSource->setProjection(deps.toProjection(), deps.toParsedDeps());
    }
}

Timestamp PipelineD::getLatestOplogTimestamp(const Pipeline* pipeline) {
    if (auto docSourceCursor =
            dynamic_cast<DocumentSourceCursor*>(pipeline->_sources.front().get())) {
        return docSourceCursor->getLatestOplogTimestamp();
    }
    return Timestamp();
}

std::string PipelineD::getPlanSummaryStr(const Pipeline* pPipeline) {
    if (auto docSourceCursor =
            dynamic_cast<DocumentSourceCursor*>(pPipeline->_sources.front().get())) {
        return docSourceCursor->getPlanSummaryStr();
    }

    return "";
}

void PipelineD::getPlanSummaryStats(const Pipeline* pPipeline, PlanSummaryStats* statsOut) {
    invariant(statsOut);

    if (auto docSourceCursor =
            dynamic_cast<DocumentSourceCursor*>(pPipeline->_sources.front().get())) {
        *statsOut = docSourceCursor->getPlanSummaryStats();
    }

    bool hasSortStage{false};
    for (auto&& source : pPipeline->_sources) {
        if (dynamic_cast<DocumentSourceSort*>(source.get())) {
            hasSortStage = true;
            break;
        }
    }

    statsOut->hasSortStage = hasSortStage;
}

}  // namespace mongo

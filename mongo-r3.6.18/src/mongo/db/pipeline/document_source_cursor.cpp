
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

#include "mongo/db/pipeline/document_source_cursor.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::string;

const char* DocumentSourceCursor::getSourceName() const {
    return "$cursor";
}

bool DocumentSourceCursor::Batch::isEmpty() const {
    if (shouldProduceEmptyDocs) {
        return !_count;
    } else {
        return _batchOfDocs.empty();
    }
    MONGO_UNREACHABLE;
}

void DocumentSourceCursor::Batch::enqueue(Document&& doc) {
    if (shouldProduceEmptyDocs) {
        ++_count;
    } else {
        _batchOfDocs.push_back(doc.getOwned());
        _memUsageBytes += _batchOfDocs.back().getApproximateSize();
    }
}

Document DocumentSourceCursor::Batch::dequeue() {
    invariant(!isEmpty());
    if (shouldProduceEmptyDocs) {
        --_count;
        return Document{};
    } else {
        Document out = std::move(_batchOfDocs.front());
        _batchOfDocs.pop_front();
        if (_batchOfDocs.empty()) {
            _memUsageBytes = 0;
        }
        return out;
    }
    MONGO_UNREACHABLE;
}

void DocumentSourceCursor::Batch::clear() {
    _batchOfDocs.clear();
    _count = 0;
    _memUsageBytes = 0;
}

DocumentSource::GetNextResult DocumentSourceCursor::getNext() {
    pExpCtx->checkForInterrupt();

    if (_currentBatch.isEmpty()) {
        loadBatch();

        if (_currentBatch.isEmpty())
            return GetNextResult::makeEOF();
    }

    return _currentBatch.dequeue();
}

void DocumentSourceCursor::loadBatch() {
    if (!_exec) {
        // No more documents.
        dispose();
        return;
    }

    PlanExecutor::ExecState state;
    BSONObj resultObj;
    {
        AutoGetCollectionForRead autoColl(pExpCtx->opCtx, _exec->nss());
        uassertStatusOK(repl::ReplicationCoordinator::get(pExpCtx->opCtx)
                            ->checkCanServeReadsFor(pExpCtx->opCtx, _exec->nss(), true));

        uassertStatusOK(_exec->restoreState());

        {
            ON_BLOCK_EXIT([this] { recordPlanSummaryStats(); });

            while ((state = _exec->getNext(&resultObj, nullptr)) == PlanExecutor::ADVANCED) {
                if (_currentBatch.shouldProduceEmptyDocs) {
                    _currentBatch.enqueue(Document());
                } else if (_dependencies) {
                    _currentBatch.enqueue(_dependencies->extractFields(resultObj));
                } else {
                    _currentBatch.enqueue(Document::fromBsonWithMetaData(resultObj));
                }

                if (_limit) {
                    if (++_docsAddedToBatches == _limit->getLimit()) {
                        break;
                    }
                    verify(_docsAddedToBatches < _limit->getLimit());
                }

                // As long as we're waiting for inserts, we shouldn't do any batching at this level
                // we need the whole pipeline to see each document to see if we should stop waiting.
                // Furthermore, if we need to return the latest oplog time (in the tailable and
                // needs-merge case), batching will result in a wrong time.
                if (awaitDataState(pExpCtx->opCtx).shouldWaitForInserts ||
                    (pExpCtx->isTailableAwaitData() && pExpCtx->needsMerge) ||
                    static_cast<long long>(_currentBatch.memUsageBytes()) >
                        internalDocumentSourceCursorBatchSizeBytes.load()) {
                    // End this batch and prepare PlanExecutor for yielding.
                    _exec->saveState();
                    return;
                }
            }
            // Special case for tailable cursor -- EOF doesn't preclude more results, so keep
            // the PlanExecutor alive.
            if (state == PlanExecutor::IS_EOF && pExpCtx->isTailableAwaitData()) {
                _exec->saveState();
                return;
            }
        }

        // If we got here, there won't be any more documents, so destroy our PlanExecutor. Note we
        // must hold a collection lock to destroy '_exec', but we can only assume that our locks are
        // still held if '_exec' did not end in an error. If '_exec' encountered an error during a
        // yield, the locks might be yielded.
        if (state != PlanExecutor::DEAD && state != PlanExecutor::FAILURE) {
            cleanupExecutor(autoColl);
        }
    }

    switch (state) {
        case PlanExecutor::ADVANCED:
        case PlanExecutor::IS_EOF:
            return;  // We've reached our limit or exhausted the cursor.
        case PlanExecutor::DEAD:
        case PlanExecutor::FAILURE: {
            cleanupExecutor();
            uassertStatusOK(WorkingSetCommon::getMemberObjectStatus(resultObj).withContext(
                "Error in $cursor stage"));
        }
        default:
            MONGO_UNREACHABLE;
    }
}

Pipeline::SourceContainer::iterator DocumentSourceCursor::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    auto nextLimit = dynamic_cast<DocumentSourceLimit*>((*std::next(itr)).get());

    if (nextLimit) {
        if (_limit) {
            // We already have an internal limit, set it to the more restrictive of the two.
            _limit->setLimit(std::min(_limit->getLimit(), nextLimit->getLimit()));
        } else {
            _limit = nextLimit;
        }
        container->erase(std::next(itr));
        return itr;
    }
    return std::next(itr);
}

void DocumentSourceCursor::recordPlanSummaryStats() {
    invariant(_exec);
    // Aggregation handles in-memory sort outside of the query sub-system. Given that we need to
    // preserve the existing value of hasSortStage rather than overwrite with the underlying
    // PlanExecutor's value.
    auto hasSortStage = _planSummaryStats.hasSortStage;

    Explain::getSummaryStats(*_exec, &_planSummaryStats);

    _planSummaryStats.hasSortStage = hasSortStage;
}

Value DocumentSourceCursor::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    // We never parse a DocumentSourceCursor, so we only serialize for explain.
    if (!explain)
        return Value();

    if (*explain >= ExplainOptions::Verbosity::kExecStats) {
        uassert(50851,
                "Attempting to explain() pipeline with a cursor that cannot be explained at level "
                "'executionStats' or above. Try running the explain with verbosity level "
                "'queryPlanner'",
                !_failsForExecutionLevelExplain);
    }
    // Get planner-level explain info from the underlying PlanExecutor.
    invariant(_exec);
    BSONObjBuilder explainBuilder;
    {
        AutoGetCollectionForRead autoColl(pExpCtx->opCtx, _exec->nss());
        uassertStatusOK(_exec->restoreState());
        Explain::explainStages(_exec.get(), autoColl.getCollection(), *explain, &explainBuilder);
        _exec->saveState();
    }

    MutableDocument out;
    out["query"] = Value(_query);

    if (!_sort.isEmpty())
        out["sort"] = Value(_sort);

    if (_limit)
        out["limit"] = Value(_limit->getLimit());

    if (!_projection.isEmpty())
        out["fields"] = Value(_projection);

    // Add explain results from the query system into the agg explain output.
    BSONObj explainObj = explainBuilder.obj();
    invariant(explainObj.hasField("queryPlanner"));
    out["queryPlanner"] = Value(explainObj["queryPlanner"]);
    if (*explain >= ExplainOptions::Verbosity::kExecStats) {
        invariant(explainObj.hasField("executionStats"));
        out["executionStats"] = Value(explainObj["executionStats"]);
    }

    return Value(DOC(getSourceName() << out.freezeToValue()));
}

void DocumentSourceCursor::detachFromOperationContext() {
    if (_exec) {
        _exec->detachFromOperationContext();
    }
}

void DocumentSourceCursor::reattachToOperationContext(OperationContext* opCtx) {
    if (_exec) {
        _exec->reattachToOperationContext(opCtx);
    }
}

void DocumentSourceCursor::doDispose() {
    _currentBatch.clear();
    if (!_exec) {
        // We've already properly disposed of our PlanExecutor.
        return;
    }
    cleanupExecutor();
}

void DocumentSourceCursor::cleanupExecutor() {
    invariant(_exec);
    auto* opCtx = pExpCtx->opCtx;
    // We need to be careful to not use AutoGetCollection here, since we only need the lock to
    // protect potential access to the Collection's CursorManager, and AutoGetCollection may throw
    // if this namespace has since turned into a view. Using Database::getCollection() will simply
    // return nullptr if the collection has since turned into a view. In this case, '_exec' will
    // already have been marked as killed when the collection was dropped, and we won't need to
    // access the CursorManager to properly dispose of it.
    AutoGetDb dbLock(opCtx, _exec->nss().db(), MODE_IS);
    Lock::CollectionLock collLock(opCtx->lockState(), _exec->nss().ns(), MODE_IS);
    auto collection = dbLock.getDb() ? dbLock.getDb()->getCollection(opCtx, _exec->nss()) : nullptr;
    auto cursorManager = collection ? collection->getCursorManager() : nullptr;
    _exec->dispose(opCtx, cursorManager);
    _exec.reset();
}

void DocumentSourceCursor::cleanupExecutor(const AutoGetCollectionForRead& readLock) {
    invariant(_exec);
    auto cursorManager =
        readLock.getCollection() ? readLock.getCollection()->getCursorManager() : nullptr;
    _exec->dispose(pExpCtx->opCtx, cursorManager);
    _exec.reset();
}

DocumentSourceCursor::~DocumentSourceCursor() {
    invariant(!_exec);  // '_exec' should have been cleaned up via dispose() before destruction.
}

DocumentSourceCursor::DocumentSourceCursor(
    Collection* collection,
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
    const intrusive_ptr<ExpressionContext>& pCtx,
    bool failsForExecutionLevelExplain)
    : DocumentSource(pCtx),
      _docsAddedToBatches(0),
      _exec(std::move(exec)),
      _outputSorts(_exec->getOutputSorts()),
      _failsForExecutionLevelExplain(failsForExecutionLevelExplain) {

    _planSummary = Explain::getPlanSummary(_exec.get());
    recordPlanSummaryStats();

    if (collection) {
        collection->infoCache()->notifyOfQuery(pExpCtx->opCtx, _planSummaryStats.indexesUsed);
    }
}

intrusive_ptr<DocumentSourceCursor> DocumentSourceCursor::create(
    Collection* collection,
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    bool failsForExecutionLevelExplain) {
    intrusive_ptr<DocumentSourceCursor> source(new DocumentSourceCursor(
        collection, std::move(exec), pExpCtx, failsForExecutionLevelExplain));
    return source;
}
}  // namespace mongo

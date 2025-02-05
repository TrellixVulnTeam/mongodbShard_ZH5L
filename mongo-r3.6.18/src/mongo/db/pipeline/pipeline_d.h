
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

#include <boost/intrusive_ptr.hpp>
#include <memory>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/query/plan_executor.h"

namespace mongo {
class Collection;
class DocumentSourceCursor;
class DocumentSourceMatch;
class DocumentSourceSort;
class ExpressionContext;
class OperationContext;
class Pipeline;
struct PlanSummaryStats;
class BSONObj;
struct DepsTracker;

/*
  PipelineD is an extension of the Pipeline class, but with additional
  material that references symbols that are not available in mongos,
  where the remainder of the Pipeline class also functions.  PipelineD
  is a friend of Pipeline so that it can have equal access to Pipeline's
  members.

  See the friend declaration in Pipeline.
 */
class PipelineD {
public:
    /**
     * If the first stage in the pipeline does not generate its own output documents, attaches a
     * DocumentSourceCursor to the front of the pipeline which will output documents from the
     * collection to feed into the pipeline.
     *
     * This method looks for early pipeline stages that can be folded into the underlying
     * PlanExecutor, and removes those stages from the pipeline when they can be absorbed by the
     * PlanExecutor. For example, an early $match can be removed and replaced with a
     * DocumentSourceCursor containing a PlanExecutor that will do an index scan.
     *
     * Callers must take care to ensure that 'nss' is locked in at least IS-mode.
     *
     * When not null, 'aggRequest' provides access to pipeline command options such as hint.
     */
    static void prepareCursorSource(Collection* collection,
                                    const NamespaceString& nss,
                                    const AggregationRequest* aggRequest,
                                    Pipeline* pipeline);

    /**
     * Injects a MongodInterface into stages which require access to mongod-specific functionality.
     */
    static void injectMongodInterface(Pipeline* pipeline);

    static std::string getPlanSummaryStr(const Pipeline* pipeline);

    static void getPlanSummaryStats(const Pipeline* pipeline, PlanSummaryStats* statsOut);

    static Timestamp getLatestOplogTimestamp(const Pipeline* pipeline);

private:
    PipelineD();  // does not exist:  prevent instantiation

    /**
     * Creates a PlanExecutor to be used in the initial cursor source. If the query system can use
     * an index to provide a more efficient sort or projection, the sort and/or projection will be
     * incorporated into the PlanExecutor.
     *
     * 'sortObj' will be set to an empty object if the query system cannot provide a non-blocking
     * sort, and 'projectionObj' will be set to an empty object if the query system cannot provide a
     * covered projection.
     */
    static StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> prepareExecutor(
        OperationContext* opCtx,
        Collection* collection,
        const NamespaceString& nss,
        Pipeline* pipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        bool oplogReplay,
        const boost::intrusive_ptr<DocumentSourceSort>& sortStage,
        const DepsTracker& deps,
        const BSONObj& queryObj,
        const AggregationRequest* aggRequest,
        BSONObj* sortObj,
        BSONObj* projectionObj);

    /**
     * Creates a DocumentSourceCursor from the given PlanExecutor and adds it to the front of the
     * Pipeline.
     */
    static void addCursorSource(Collection* collection,
                                Pipeline* pipeline,
                                const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
                                DepsTracker deps,
                                const BSONObj& queryObj = BSONObj(),
                                const BSONObj& sortObj = BSONObj(),
                                const BSONObj& projectionObj = BSONObj(),
                                bool failsForExecutionLevelExplain = false);
};

}  // namespace mongo

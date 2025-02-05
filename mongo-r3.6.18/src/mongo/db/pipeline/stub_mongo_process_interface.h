
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

#include "mongo/db/pipeline/document_source.h"

#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * A stub MongoProcessInterface that can be used for testing. Create a subclass and override
 * methods as needed.
 */
class StubMongoProcessInterface
    : public DocumentSourceNeedsMongoProcessInterface::MongoProcessInterface {
public:
    virtual ~StubMongoProcessInterface() = default;

    void setOperationContext(OperationContext* opCtx) override {
        MONGO_UNREACHABLE;
    }

    DBClientBase* directClient() override {
        MONGO_UNREACHABLE;
    }

    bool isSharded(const NamespaceString& ns) override {
        MONGO_UNREACHABLE;
    }

    BSONObj insert(const NamespaceString& ns, const std::vector<BSONObj>& objs) override {
        MONGO_UNREACHABLE;
    }

    CollectionIndexUsageMap getIndexStats(OperationContext* opCtx,
                                          const NamespaceString& ns) override {
        MONGO_UNREACHABLE;
    }

    void appendLatencyStats(const NamespaceString& nss,
                            bool includeHistograms,
                            BSONObjBuilder* builder) const override {
        MONGO_UNREACHABLE;
    }

    Status appendStorageStats(const NamespaceString& nss,
                              const BSONObj& param,
                              BSONObjBuilder* builder) const override {
        MONGO_UNREACHABLE;
    }

    Status appendRecordCount(const NamespaceString& nss, BSONObjBuilder* builder) const override {
        MONGO_UNREACHABLE;
    }

    BSONObj getCollectionOptions(const NamespaceString& nss) override {
        MONGO_UNREACHABLE;
    }

    Status renameIfOptionsAndIndexesHaveNotChanged(
        const BSONObj& renameCommandObj,
        const NamespaceString& targetNs,
        const BSONObj& originalCollectionOptions,
        const std::list<BSONObj>& originalIndexes) override {
        MONGO_UNREACHABLE;
    }

    StatusWith<std::unique_ptr<Pipeline, Pipeline::Deleter>> makePipeline(
        const std::vector<BSONObj>& rawPipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const MakePipelineOptions opts) override {
        MONGO_UNREACHABLE;
    }

    Status attachCursorSourceToPipeline(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                        Pipeline* pipeline) override {
        MONGO_UNREACHABLE;
    }

    std::vector<BSONObj> getCurrentOps(CurrentOpConnectionsMode connMode,
                                       CurrentOpUserMode userMode,
                                       CurrentOpTruncateMode truncateMode) const override {
        MONGO_UNREACHABLE;
    }

    std::string getShardName(OperationContext* opCtx) const override {
        MONGO_UNREACHABLE;
    }

    std::vector<FieldPath> collectDocumentKeyFields(UUID) const override {
        MONGO_UNREACHABLE;
    }

    boost::optional<Document> lookupSingleDocument(const NamespaceString& nss,
                                                   UUID collectionUUID,
                                                   const Document& documentKey,
                                                   boost::optional<BSONObj> readConcern) {
        MONGO_UNREACHABLE;
    }
};
}  // namespace mongo

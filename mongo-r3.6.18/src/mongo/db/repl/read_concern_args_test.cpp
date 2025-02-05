
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

#include "mongo/db/jsobj.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

TEST(ReadAfterParse, OpTimeOnly) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_OK(readAfterOpTime.initialize(BSON(
        "find"
        << "test"
        << ReadConcernArgs::kReadConcernFieldName
        << BSON(ReadConcernArgs::kAfterOpTimeFieldName
                << BSON(OpTime::kTimestampFieldName << Timestamp(20, 30) << OpTime::kTermFieldName
                                                    << 2)))));

    ASSERT_TRUE(readAfterOpTime.getArgsOpTime());
    ASSERT_TRUE(!readAfterOpTime.getArgsClusterTime());
    auto argsOpTime = readAfterOpTime.getArgsOpTime();
    ASSERT_EQ(Timestamp(20, 30), argsOpTime->getTimestamp());
    ASSERT_EQ(2, argsOpTime->getTerm());
    ASSERT(ReadConcernLevel::kLocalReadConcern == readAfterOpTime.getLevel());
}

TEST(ReadAfterParse, ClusterTimeOnly) {
    ReadConcernArgs readAfterOpTime;
    auto clusterTime = LogicalTime(Timestamp(20, 30));
    ASSERT_OK(readAfterOpTime.initialize(BSON("find"
                                              << "test"
                                              << ReadConcernArgs::kReadConcernFieldName
                                              << BSON(ReadConcernArgs::kAfterClusterTimeFieldName
                                                      << clusterTime.asTimestamp()))));
    auto argsClusterTime = readAfterOpTime.getArgsClusterTime();
    ASSERT_TRUE(argsClusterTime);
    ASSERT_TRUE(!readAfterOpTime.getArgsOpTime());
    ASSERT_TRUE(clusterTime == *argsClusterTime);
}

TEST(ReadAfterParse, ClusterTimeAndLevelLocal) {
    ReadConcernArgs readAfterOpTime;
    // Must have level=majority
    auto clusterTime = LogicalTime(Timestamp(20, 30));
    ASSERT_OK(readAfterOpTime.initialize(BSON("find"
                                              << "test"
                                              << ReadConcernArgs::kReadConcernFieldName
                                              << BSON(ReadConcernArgs::kAfterClusterTimeFieldName
                                                      << clusterTime.asTimestamp()
                                                      << ReadConcernArgs::kLevelFieldName
                                                      << "local"))));
    auto argsClusterTime = readAfterOpTime.getArgsClusterTime();
    ASSERT_TRUE(argsClusterTime);
    ASSERT_TRUE(!readAfterOpTime.getArgsOpTime());
    ASSERT_TRUE(clusterTime == *argsClusterTime);
    ASSERT(ReadConcernLevel::kLocalReadConcern == readAfterOpTime.getLevel());
}

TEST(ReadAfterParse, ClusterTimeAndLevelMajority) {
    ReadConcernArgs readAfterOpTime;
    // Must have level=majority
    auto clusterTime = LogicalTime(Timestamp(20, 30));
    ASSERT_OK(readAfterOpTime.initialize(BSON("find"
                                              << "test"
                                              << ReadConcernArgs::kReadConcernFieldName
                                              << BSON(ReadConcernArgs::kAfterClusterTimeFieldName
                                                      << clusterTime.asTimestamp()
                                                      << ReadConcernArgs::kLevelFieldName
                                                      << "majority"))));
    auto argsClusterTime = readAfterOpTime.getArgsClusterTime();
    ASSERT_TRUE(argsClusterTime);
    ASSERT_TRUE(!readAfterOpTime.getArgsOpTime());
    ASSERT_TRUE(clusterTime == *argsClusterTime);
    ASSERT(ReadConcernLevel::kMajorityReadConcern == readAfterOpTime.getLevel());
}

TEST(ReadAfterParse, LevelOnly) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_OK(
        readAfterOpTime.initialize(BSON("find"
                                        << "test"
                                        << ReadConcernArgs::kReadConcernFieldName
                                        << BSON(ReadConcernArgs::kLevelFieldName << "majority"))));

    ASSERT_TRUE(!readAfterOpTime.getArgsOpTime());
    ASSERT_TRUE(!readAfterOpTime.getArgsClusterTime());
    ASSERT_TRUE(ReadConcernLevel::kMajorityReadConcern == readAfterOpTime.getLevel());
}

TEST(ReadAfterParse, ReadCommittedFullSpecification) {
    ReadConcernArgs readAfterOpTime;
    auto clusterTime = LogicalTime(Timestamp(100, 200));
    ASSERT_NOT_OK(readAfterOpTime.initialize(BSON(
        "find"
        << "test"
        << ReadConcernArgs::kReadConcernFieldName
        << BSON(ReadConcernArgs::kAfterOpTimeFieldName
                << BSON(OpTime::kTimestampFieldName << Timestamp(20, 30) << OpTime::kTermFieldName
                                                    << 2)
                << ReadConcernArgs::kAfterClusterTimeFieldName
                << clusterTime.asTimestamp()
                << ReadConcernArgs::kLevelFieldName
                << "majority"))));
}

TEST(ReadAfterParse, Empty) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_OK(readAfterOpTime.initialize(BSON("find"
                                              << "test")));

    ASSERT_TRUE(!readAfterOpTime.getArgsOpTime());
    ASSERT_TRUE(!readAfterOpTime.getArgsClusterTime());
    ASSERT(ReadConcernLevel::kLocalReadConcern == readAfterOpTime.getLevel());
}

TEST(ReadAfterParse, BadRootType) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_NOT_OK(readAfterOpTime.initialize(BSON("find"
                                                  << "test"
                                                  << ReadConcernArgs::kReadConcernFieldName
                                                  << "x")));
}

TEST(ReadAfterParse, BadOpTimeType) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_NOT_OK(
        readAfterOpTime.initialize(BSON("find"
                                        << "test"
                                        << ReadConcernArgs::kReadConcernFieldName
                                        << BSON(ReadConcernArgs::kAfterOpTimeFieldName << 2))));
}

TEST(ReadAfterParse, OpTimeNotNeededForValidReadConcern) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_OK(readAfterOpTime.initialize(BSON("find"
                                              << "test"
                                              << ReadConcernArgs::kReadConcernFieldName
                                              << BSONObj())));
}

TEST(ReadAfterParse, NoOpTimeTS) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_NOT_OK(
        readAfterOpTime.initialize(BSON("find"
                                        << "test"
                                        << ReadConcernArgs::kReadConcernFieldName
                                        << BSON(ReadConcernArgs::kAfterOpTimeFieldName
                                                << BSON(OpTime::kTimestampFieldName << 2)))));
}

TEST(ReadAfterParse, NoOpTimeTerm) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_NOT_OK(readAfterOpTime.initialize(BSON("find"
                                                  << "test"
                                                  << ReadConcernArgs::kReadConcernFieldName
                                                  << BSON(ReadConcernArgs::kAfterOpTimeFieldName
                                                          << BSON(OpTime::kTermFieldName << 2)))));
}

TEST(ReadAfterParse, BadOpTimeTSType) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_NOT_OK(readAfterOpTime.initialize(
        BSON("find"
             << "test"
             << ReadConcernArgs::kReadConcernFieldName
             << BSON(ReadConcernArgs::kAfterOpTimeFieldName
                     << BSON(OpTime::kTimestampFieldName << BSON("x" << 1) << OpTime::kTermFieldName
                                                         << 2)))));
}

TEST(ReadAfterParse, BadOpTimeTermType) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_NOT_OK(readAfterOpTime.initialize(BSON(
        "find"
        << "test"
        << ReadConcernArgs::kReadConcernFieldName
        << BSON(ReadConcernArgs::kAfterOpTimeFieldName
                << BSON(OpTime::kTimestampFieldName << Timestamp(1, 0) << OpTime::kTermFieldName
                                                    << "y")))));
}

TEST(ReadAfterParse, BadLevelType) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              readAfterOpTime.initialize(BSON("find"
                                              << "test"
                                              << ReadConcernArgs::kReadConcernFieldName
                                              << BSON(ReadConcernArgs::kLevelFieldName << 7))));
}

TEST(ReadAfterParse, BadLevelValue) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_EQ(ErrorCodes::FailedToParse,
              readAfterOpTime.initialize(BSON("find"
                                              << "test"
                                              << ReadConcernArgs::kReadConcernFieldName
                                              << BSON(ReadConcernArgs::kLevelFieldName
                                                      << "seven is not a real level"))));
}

TEST(ReadAfterParse, BadOption) {
    ReadConcernArgs readAfterOpTime;
    ASSERT_EQ(ErrorCodes::InvalidOptions,
              readAfterOpTime.initialize(BSON("find"
                                              << "test"
                                              << ReadConcernArgs::kReadConcernFieldName
                                              << BSON("asdf" << 1))));
}

TEST(ReadAfterSerialize, Empty) {
    BSONObjBuilder builder;
    ReadConcernArgs readAfterOpTime;
    readAfterOpTime.appendInfo(&builder);

    BSONObj obj(builder.done());

    ASSERT_BSONOBJ_EQ(BSON(ReadConcernArgs::kReadConcernFieldName << BSONObj()), obj);
}

TEST(ReadAfterSerialize, AfterClusterTimeOnly) {
    BSONObjBuilder builder;
    auto clusterTime = LogicalTime(Timestamp(20, 30));
    ReadConcernArgs readAfterClusterTime(clusterTime, boost::none);
    readAfterClusterTime.appendInfo(&builder);

    BSONObj expectedObj(
        BSON(ReadConcernArgs::kReadConcernFieldName
             << BSON(ReadConcernArgs::kAfterClusterTimeFieldName << clusterTime.asTimestamp())));

    ASSERT_BSONOBJ_EQ(expectedObj, builder.done());
}

TEST(ReadAfterSerialize, AfterOpTimeOnly) {
    BSONObjBuilder builder;
    ReadConcernArgs readAfterOpTime(OpTime(Timestamp(20, 30), 2), boost::none);
    readAfterOpTime.appendInfo(&builder);

    BSONObj expectedObj(BSON(
        ReadConcernArgs::kReadConcernFieldName << BSON(
            ReadConcernArgs::kAfterOpTimeFieldName << BSON(
                OpTime::kTimestampFieldName << Timestamp(20, 30) << OpTime::kTermFieldName << 2))));

    ASSERT_BSONOBJ_EQ(expectedObj, builder.done());
}

TEST(ReadAfterSerialize, CommitLevelOnly) {
    BSONObjBuilder builder;
    ReadConcernArgs readAfterOpTime(ReadConcernLevel::kLocalReadConcern);
    readAfterOpTime.appendInfo(&builder);

    BSONObj expectedObj(BSON(ReadConcernArgs::kReadConcernFieldName
                             << BSON(ReadConcernArgs::kLevelFieldName << "local")));

    ASSERT_BSONOBJ_EQ(expectedObj, builder.done());
}

TEST(ReadAfterSerialize, iAfterCLusterTimeAndLevel) {
    BSONObjBuilder builder;
    auto clusterTime = LogicalTime(Timestamp(20, 30));
    ReadConcernArgs readAfterClusterTime(clusterTime, ReadConcernLevel::kMajorityReadConcern);
    readAfterClusterTime.appendInfo(&builder);

    BSONObj expectedObj(
        BSON(ReadConcernArgs::kReadConcernFieldName
             << BSON(ReadConcernArgs::kLevelFieldName << "majority"
                                                      << ReadConcernArgs::kAfterClusterTimeFieldName
                                                      << clusterTime.asTimestamp())));

    ASSERT_BSONOBJ_EQ(expectedObj, builder.done());
}

TEST(ReadAfterSerialize, AfterOpTimeAndLevel) {
    BSONObjBuilder builder;
    ReadConcernArgs readAfterOpTime(OpTime(Timestamp(20, 30), 2),
                                    ReadConcernLevel::kMajorityReadConcern);
    readAfterOpTime.appendInfo(&builder);

    BSONObj expectedObj(BSON(
        ReadConcernArgs::kReadConcernFieldName
        << BSON(ReadConcernArgs::kLevelFieldName
                << "majority"
                << ReadConcernArgs::kAfterOpTimeFieldName
                << BSON(OpTime::kTimestampFieldName << Timestamp(20, 30) << OpTime::kTermFieldName
                                                    << 2))));

    ASSERT_BSONOBJ_EQ(expectedObj, builder.done());
}

}  // unnamed namespace
}  // namespace repl
}  // namespace mongo

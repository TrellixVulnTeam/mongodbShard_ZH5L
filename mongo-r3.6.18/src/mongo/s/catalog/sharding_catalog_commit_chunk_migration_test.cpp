
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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config_server_test_fixture.h"

namespace mongo {
namespace {

using unittest::assertGet;

using CommitChunkMigrate = ConfigServerTestFixture;

TEST_F(CommitChunkMigrate, ChunksUpdatedCorrectlyWithControlChunk) {
    std::string const nss = "TestDB.TestColl";

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    ChunkType migratedChunk, controlChunk;
    {
        ChunkVersion origVersion(12, 7, OID::gen());

        migratedChunk.setNS(nss);
        migratedChunk.setVersion(origVersion);
        migratedChunk.setShard(shard0.getName());
        migratedChunk.setMin(BSON("a" << 1));
        migratedChunk.setMax(BSON("a" << 10));

        origVersion.incMinor();

        controlChunk.setNS(nss);
        controlChunk.setVersion(origVersion);
        controlChunk.setShard(shard0.getName());
        controlChunk.setMin(BSON("a" << 10));
        controlChunk.setMax(BSON("a" << 20));
        controlChunk.setJumbo(true);
    }

    setupChunks({migratedChunk, controlChunk});

    Timestamp validAfter{101, 0};
    BSONObj versions = assertGet(ShardingCatalogManager::get(operationContext())
                                     ->commitChunkMigration(operationContext(),
                                                            NamespaceString(nss),
                                                            migratedChunk,
                                                            controlChunk,
                                                            migratedChunk.getVersion().epoch(),
                                                            ShardId(shard0.getName()),
                                                            ShardId(shard1.getName())));

    // Verify the versions returned match expected values.
    auto mver = assertGet(
        ChunkVersion::parseFromBSONWithFieldForCommands(versions, "migratedChunkVersion"));
    ASSERT_EQ(ChunkVersion(migratedChunk.getVersion().majorVersion() + 1,
                           0,
                           migratedChunk.getVersion().epoch()),
              mver);

    auto cver =
        assertGet(ChunkVersion::parseFromBSONWithFieldForCommands(versions, "controlChunkVersion"));
    ASSERT_EQ(ChunkVersion(migratedChunk.getVersion().majorVersion() + 1,
                           1,
                           migratedChunk.getVersion().epoch()),
              cver);

    // Verify the chunks ended up in the right shards, and versions match the values returned.
    auto chunkDoc0 = uassertStatusOK(getChunkDoc(operationContext(), migratedChunk.getMin()));
    ASSERT_EQ("shard1", chunkDoc0.getShard().toString());
    ASSERT_EQ(mver, chunkDoc0.getVersion());

    auto chunkDoc1 = uassertStatusOK(getChunkDoc(operationContext(), controlChunk.getMin()));
    ASSERT_EQ("shard0", chunkDoc1.getShard().toString());
    ASSERT_EQ(cver, chunkDoc1.getVersion());

    // The control chunk's jumbo status should be unchanged.
    ASSERT(chunkDoc1.getJumbo());
}

TEST_F(CommitChunkMigrate, CheckCorrectOpsCommandNoCtl) {

    std::string const nss = "TestDB.TestColl";

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    int origMajorVersion = 15;
    auto const origVersion = ChunkVersion(origMajorVersion, 4, OID::gen());

    ChunkType chunk0;
    chunk0.setNS(nss);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    setupChunks({chunk0});

    StatusWith<BSONObj> resultBSON = ShardingCatalogManager::get(operationContext())
                                         ->commitChunkMigration(operationContext(),
                                                                NamespaceString(chunk0.getNS()),
                                                                chunk0,
                                                                boost::none,
                                                                origVersion.epoch(),
                                                                ShardId(shard0.getName()),
                                                                ShardId(shard1.getName()));

    ASSERT_OK(resultBSON.getStatus());

    // Verify the version returned matches expected value.
    BSONObj versions = resultBSON.getValue();
    auto mver = ChunkVersion::parseFromBSONWithFieldForCommands(versions, "migratedChunkVersion");
    ASSERT_OK(mver.getStatus());
    ASSERT_EQ(ChunkVersion(origMajorVersion + 1, 0, origVersion.epoch()), mver.getValue());

    auto cver = ChunkVersion::parseFromBSONWithFieldForCommands(versions, "controlChunkVersion");
    ASSERT_NOT_OK(cver.getStatus());

    // Verify the chunk ended up in the right shard, and version matches the value returned.
    auto chunkDoc0 = uassertStatusOK(getChunkDoc(operationContext(), chunkMin));
    ASSERT_EQ("shard1", chunkDoc0.getShard().toString());
    ASSERT_EQ(mver.getValue(), chunkDoc0.getVersion());
}

TEST_F(CommitChunkMigrate, RejectWrongCollectionEpoch0) {

    std::string const nss = "TestDB.TestColl";

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    int origMajorVersion = 12;
    auto const origVersion = ChunkVersion(origMajorVersion, 7, OID::gen());

    ChunkType chunk0;
    chunk0.setNS(nss);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    ChunkType chunk1;
    chunk1.setNS(nss);
    chunk1.setVersion(origVersion);
    chunk1.setShard(shard0.getName());

    chunk1.setMin(chunkMax);
    auto chunkMaxax = BSON("a" << 20);
    chunk1.setMax(chunkMaxax);

    setupChunks({chunk0, chunk1});

    StatusWith<BSONObj> resultBSON = ShardingCatalogManager::get(operationContext())
                                         ->commitChunkMigration(operationContext(),
                                                                NamespaceString(chunk0.getNS()),
                                                                chunk0,
                                                                chunk1,
                                                                OID::gen(),
                                                                ShardId(shard0.getName()),
                                                                ShardId(shard1.getName()));

    ASSERT_EQ(ErrorCodes::StaleEpoch, resultBSON.getStatus());
}

TEST_F(CommitChunkMigrate, RejectWrongCollectionEpoch1) {

    std::string const nss = "TestDB.TestColl";

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    int origMajorVersion = 12;
    auto const origVersion = ChunkVersion(origMajorVersion, 7, OID::gen());
    auto const otherVersion = ChunkVersion(origMajorVersion, 7, OID::gen());

    ChunkType chunk0;
    chunk0.setNS(nss);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    ChunkType chunk1;
    chunk1.setNS(nss);
    chunk1.setVersion(otherVersion);
    chunk1.setShard(shard0.getName());

    chunk1.setMin(chunkMax);
    auto chunkMaxax = BSON("a" << 20);
    chunk1.setMax(chunkMaxax);

    // get version from the control chunk this time
    setupChunks({chunk1, chunk0});

    StatusWith<BSONObj> resultBSON = ShardingCatalogManager::get(operationContext())
                                         ->commitChunkMigration(operationContext(),
                                                                NamespaceString(chunk0.getNS()),
                                                                chunk0,
                                                                chunk1,
                                                                origVersion.epoch(),
                                                                ShardId(shard0.getName()),
                                                                ShardId(shard1.getName()));

    ASSERT_EQ(ErrorCodes::StaleEpoch, resultBSON.getStatus());
}

TEST_F(CommitChunkMigrate, RejectChunkMissing0) {

    std::string const nss = "TestDB.TestColl";

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    int origMajorVersion = 12;
    auto const origVersion = ChunkVersion(origMajorVersion, 7, OID::gen());

    ChunkType chunk0;
    chunk0.setNS(nss);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    ChunkType chunk1;
    chunk1.setNS(nss);
    chunk1.setVersion(origVersion);
    chunk1.setShard(shard0.getName());

    chunk1.setMin(chunkMax);
    auto chunkMaxax = BSON("a" << 20);
    chunk1.setMax(chunkMaxax);

    setupChunks({chunk1});

    StatusWith<BSONObj> resultBSON = ShardingCatalogManager::get(operationContext())
                                         ->commitChunkMigration(operationContext(),
                                                                NamespaceString(chunk0.getNS()),
                                                                chunk0,
                                                                chunk1,
                                                                origVersion.epoch(),
                                                                ShardId(shard0.getName()),
                                                                ShardId(shard1.getName()));

    ASSERT_EQ(40165, resultBSON.getStatus().code());
}

TEST_F(CommitChunkMigrate, RejectChunkMissing1) {

    std::string const nss = "TestDB.TestColl";

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    int origMajorVersion = 12;
    auto const origVersion = ChunkVersion(origMajorVersion, 7, OID::gen());

    ChunkType chunk0;
    chunk0.setNS(nss);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    ChunkType chunk1;
    chunk1.setNS(nss);
    chunk1.setVersion(origVersion);
    chunk1.setShard(shard0.getName());

    chunk1.setMin(chunkMax);
    auto chunkMaxax = BSON("a" << 20);
    chunk1.setMax(chunkMaxax);

    setupChunks({chunk0});

    StatusWith<BSONObj> resultBSON = ShardingCatalogManager::get(operationContext())
                                         ->commitChunkMigration(operationContext(),
                                                                NamespaceString(chunk0.getNS()),
                                                                chunk0,
                                                                chunk1,
                                                                origVersion.epoch(),
                                                                ShardId(shard0.getName()),
                                                                ShardId(shard1.getName()));

    ASSERT_EQ(40165, resultBSON.getStatus().code());
}

}  // namespace
}  // namespace mongo

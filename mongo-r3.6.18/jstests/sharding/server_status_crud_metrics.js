/**
 * Tests for the 'metrics.query' section of the mongoS serverStatus response dealing with CRUD
 * operations.
 */

(function() {
    "use strict";

    const st = new ShardingTest({shards: 2});
    const testDB = st.s.getDB("test");
    const testColl = testDB.coll;
    const unshardedColl = testDB.unsharded;

    assert.commandWorked(st.s0.adminCommand({enableSharding: testDB.getName()}));
    st.ensurePrimaryShard(testDB.getName(), st.shard0.shardName);

    // Shard testColl on {x:1}, split it at {x:0}, and move chunk {x:1} to shard1.
    st.shardColl(testColl, {x: 1}, {x: 0}, {x: 1});

    // Insert one document on each shard.
    assert.writeOK(testColl.insert({x: 1, _id: 1}));
    assert.writeOK(testColl.insert({x: -1, _id: 0}));

    assert.writeOK(unshardedColl.insert({x: 1, _id: 1}));

    // Verification for 'updateOneOpStyleBroadcastWithExactIDCount' metric.

    // Should increment the metric as the update cannot target single shard and are {multi:false}.
    assert.writeOK(testDB.coll.update({_id: "missing"}, {$set: {a: 1}}, {multi: false}));
    assert.writeOK(testDB.coll.update({_id: 1}, {$set: {a: 2}}, {multi: false}));

    // Should increment the metric because we broadcast by _id, even though the update subsequently
    // fails on the individual shard.
    assert.writeError(testDB.coll.update({_id: 1}, {$set: {x: 2}}, {multi: false}));
    assert.writeError(
        testDB.coll.update({_id: 1}, {$set: {x: 2, $invalidField: 4}}, {multi: false}));

    let mongosServerStatus = testDB.adminCommand({serverStatus: 1});

    // Verify that the above four updates incremented the metric counter.
    assert.eq(4, mongosServerStatus.metrics.query.updateOneOpStyleBroadcastWithExactIDCount);

    // Shouldn't increment the metric when {multi:true}.
    assert.writeOK(testDB.coll.update({_id: 1}, {$set: {a: 3}}, {multi: true}));
    assert.writeOK(testDB.coll.update({}, {$set: {a: 3}}, {multi: true}));

    // Shouldn't increment the metric when update can target single shard.
    assert.writeOK(testDB.coll.update({x: 11}, {$set: {a: 2}}, {multi: false}));
    assert.writeOK(testDB.coll.update({x: 1}, {$set: {a: 2}}, {multi: false}));

    // Shouldn't increment the metric for replacement style updates.
    assert.writeOK(testDB.coll.update({_id: 1}, {x: 1, a: 2}));
    assert.writeOK(testDB.coll.update({x: 1}, {x: 1, a: 1}));

    // Shouldn't increment the metric when routing fails.
    assert.writeError(testDB.coll.update({}, {$set: {x: 2}}, {multi: false}));
    assert.writeError(testDB.coll.update({_id: 1}, {$set: {x: 2}}, {upsert: true}));

    // Shouldn't increment the metrics for unsharded collection.
    assert.writeOK(unshardedColl.update({_id: "missing"}, {$set: {a: 1}}, {multi: false}));
    assert.writeOK(unshardedColl.update({_id: 1}, {$set: {a: 2}}, {multi: false}));

    // Shouldn't incement the metrics when query had invalid operator.
    assert.writeError(
        testDB.coll.update({_id: 1, $invalidOperator: 1}, {$set: {a: 2}}, {multi: false}));

    mongosServerStatus = testDB.adminCommand({serverStatus: 1});

    // Verify that only the first four upserts incremented the metric counter.
    assert.eq(4, mongosServerStatus.metrics.query.updateOneOpStyleBroadcastWithExactIDCount);

    // Verification for 'upsertReplacementCannotTargetByQueryCount' metric.

    // Should increment the metric when the upsert can target single shard based on shard key in
    // replacement doc but query doesn't have shard key.
    assert.writeOK(testDB.coll.update({_id: "missing"}, {x: 1, a: 2}, {upsert: true}));
    assert.writeOK(testDB.coll.update({}, {x: 1, a: 1}, {upsert: true}));

    // Should increment the metric, even though the update subsequently fails on the shard when it
    // attempts an invalid modification of the _id.
    assert.writeError(testDB.coll.update({_id: 1}, {x: 1, a: 1, _id: 2}, {upsert: true}));

    mongosServerStatus = testDB.adminCommand({serverStatus: 1});

    // Verify that the above three updates incremented the metric counter.
    assert.eq(3, mongosServerStatus.metrics.query.upsertReplacementCannotTargetByQueryCount);

    // Shouldn't increment the metrics when query has shard key.
    assert.writeOK(testDB.coll.update({x: 1}, {x: 1, a: 1}, {upsert: true}));
    assert.writeOK(testDB.coll.update({x: 1, _id: 1}, {x: 1, a: 1}, {upsert: true}));

    // Shouldn't increment the metrics for opstyle upserts.
    assert.writeOK(testDB.coll.update({x: 1, _id: 1}, {$set: {x: 1, a: 1}}, {upsert: true}));
    assert.writeError(testDB.coll.update({_id: 1}, {$set: {x: 1, a: 1}}, {upsert: true}));

    // Shouldn't increment the metric when the query is invalid.
    assert.writeError(
        testDB.coll.update({_id: 1, $invalidOperator: 5}, {x: 1, a: 1, _id: 2}, {upsert: true}));

    // Shouldn't increment the metric when routing fails.
    assert.writeError(testDB.coll.update({x: 1}, {a: 2}, {upsert: true}));
    assert.writeError(testDB.coll.update({x: 1, _id: 1}, {a: 2}, {upsert: true}));

    // Shouldn't increment the metrics for unsharded collection.
    assert.writeOK(unshardedColl.update({_id: "missing"}, {x: 1, a: 2}, {upsert: true}));
    assert.writeOK(unshardedColl.update({}, {x: 1, a: 1}, {upsert: true}));
    assert.writeError(unshardedColl.update({_id: 1}, {x: 1, a: 1, _id: 2}, {upsert: true}));

    mongosServerStatus = testDB.adminCommand({serverStatus: 1});

    // Verify that only the first three upserts incremented the metric counter.
    assert.eq(3, mongosServerStatus.metrics.query.upsertReplacementCannotTargetByQueryCount);

    st.stop();
})();

/* This test checks that mongod correctly logs report the lockStats for sub-operations. Before
 * SERVER-26854, the log for each sub-operation reported the aggregate lock wait time of all
 * preceding sub-operations.
 *
 * To test the correctness of sub-operation logs, we use an aggregation pipeline which can
 * trigger a series of sub-operations: createCollection, createIndex, listCollections, listIndexes,
 * renameCollection and dropCollection.
 *
 * Test steps:
 *  1. Lock the database by FsyncLock.
 *  2. Run the aggregation, which will get blocked on sub-operation: {'create' : 'tmp.agg_out.x'}.
 *  3. Sleep for 2 seconds.
 *  4. FsyncUnlock.
 *  5. Check the mongod logs: only that sub-operation (createCollection) and the 'parent' operation
 * (aggregation) should log about the 2-second long wait for the lock.
 *
 * @tags: [requires_fsync]
 */
(function() {
    'use strict';

    const conn = MongoRunner.runMongod();
    const db = conn.getDB('test');
    const coll = db.books;
    const blockedMillis = 2000;
    assert.writeOK(coll.insert({title: 'Adventures of Huckleberry'}));
    assert.writeOK(coll.insert({title: '1984'}));
    assert.writeOK(coll.insert({title: 'Animal Farm'}));
    // The server will log every operation.
    db.setProfilingLevel(0, -1);
    // Create the output collection beforehand so that $out will execute a code path which triggers
    // the index creation sub-operation.
    db['favorite'].insert({foo: 1});

    // Lock the database, and then start an operation that needs the lock, so it blocks.
    assert.commandWorked(db.fsyncLock());

    // Aggregation with $out which will block on creating the temporary collection due to the
    // FsyncLock.
    const dollarOutAggregationShell = startParallelShell(function() {
        // Simple aggregation which copies a document to the output collection.
        assert.commandWorked(db.runCommand({
            aggregate: 'books',
            pipeline: [{$match: {title: '1984'}}, {$out: 'favorite'}],
            cursor: {}
        }));
    }, conn.port);

    // Sub-operation createCollection starts to get blocked.
    assert.soon(function() {
        let res = db.currentOp({waitingForLock: true});
        return res.inprog.length == 1;
    });

    sleep(blockedMillis);

    clearRawMongoProgramOutput();
    // Unlock the database. Sub-operation createCollection can proceed
    // and so do all the following sub-operations.
    db.fsyncUnlock();

    dollarOutAggregationShell();
    assert.eq(db['favorite'].count(), 1);

    // Stopping the mongod also waits until all of its logs have been read by the mongo shell.
    MongoRunner.stopMongod(conn);

    let mongodLogs = rawMongoProgramOutput();
    let lines = mongodLogs.split('\n');
    const lockWaitTimeRegex = /timeAcquiringMicros: { [wW]: ([0-9]+)/;
    let match;
    let supposedLockWaitTime;
    let numWaitedForLocks = 0;

    // Only the logs of 'parent command' (aggregation with $out) and the first
    // sub-operation(createCollection) have the information about the long wait for the lock.
    for (let line of lines) {
        if ((match = lockWaitTimeRegex.exec(line)) !== null) {
            let lockWaitTime = match[1];
            // Ignoring 'noise' lock stats from other operations such as locks taken during
            // validation stage.
            if (lockWaitTime < blockedMillis * 1000)
                continue;
            if (supposedLockWaitTime === undefined)
                supposedLockWaitTime = lockWaitTime;
            else
                assert.eq(lockWaitTime, supposedLockWaitTime);  // Should be the same.
            numWaitedForLocks++;
            jsTestLog('Operation/Sub-operation log: ');
            jsTestLog(line);
        }
    }
    assert.eq(numWaitedForLocks, 2);
})();

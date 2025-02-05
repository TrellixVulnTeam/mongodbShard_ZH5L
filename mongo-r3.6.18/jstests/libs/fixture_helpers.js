"use strict";

/**
 * Helper functions that help get information or manipulate nodes in the fixture, whether it's a
 * replica set, a sharded cluster, etc.
 */
var FixtureHelpers = (function() {
    load("jstests/concurrency/fsm_workload_helpers/server_types.js");  // For isMongos.

    function _getHostStringForReplSet(connectionToNodeInSet) {
        const isMaster = assert.commandWorked(connectionToNodeInSet.getDB("test").isMaster());
        assert(
            isMaster.hasOwnProperty("setName"),
            "Attempted to get replica set connection to a node that is not part of a replica set");
        return isMaster.setName + "/" + isMaster.hosts.join(",");
    }

    /**
     * Returns an array of connections to each data-bearing replica set in the fixture (not
     * including the config servers).
     */
    function _getAllReplicas(db) {
        let replicas = [];
        if (isMongos(db)) {
            const shardObjs = db.getSiblingDB("config").shards.find().sort({_id: 1});
            replicas = shardObjs.map((shardObj) => new ReplSetTest(shardObj.host));
        } else {
            replicas = [new ReplSetTest(_getHostStringForReplSet(db.getMongo()))];
        }
        return replicas;
    }

    /**
     * Uses ReplSetTest.awaitReplication() on each replica set in the fixture to wait for each node
     * in each replica set in the fixture (besides the config servers) to reach the same op time.
     * Asserts if the fixture is a standalone or if the shards are standalones.
     */
    function awaitReplication(db) {
        _getAllReplicas(db).forEach((replSet) => replSet.awaitReplication());
    }

    /**
     * Uses ReplSetTest.awaitLastOpCommitted() on each replica set in the fixture (besides the
     * config servers) to wait for the last oplog entry on the respective primary to be visible in
     * the committed snapshot view of the oplog on all secondaries.
     *
     * Asserts if the fixture is a standalone or if the shards are standalones.
     */
    function awaitLastOpCommitted(db) {
        _getAllReplicas(db).forEach((replSet) => replSet.awaitLastOpCommitted());
    }

    /**
     * Runs the command given by 'cmdObj' on the database given by 'dbName' on each replica set in
     * the fixture (besides the config servers). Asserts that each command works, and returns an
     * array with the responses from each shard, or with a single element if the fixture was a
     * replica set. Asserts if the fixture is a standalone or if the shards are standalones.
     */
    function runCommandOnEachPrimary({db, cmdObj}) {
        return _getAllReplicas(db).map(
            (replSet) =>
                assert.commandWorked(replSet.getPrimary().getDB(db.getName()).runCommand(cmdObj)));
    }

    /**
     * Returns a connection to the replica set primary for the primary shard for the given database.
     * Returns the same connection that 'db' is using if the fixture is not a sharded cluster.
     */
    function getPrimaryForNodeHostingDatabase(db) {
        if (!isMongos(db)) {
            return db.getMongo();
        }
        const configDB = db.getSiblingDB("config");
        let shardConn = null;
        configDB.databases.find().forEach(function(dbObj) {
            if (dbObj._id === db.getName()) {
                const shardObj = configDB.shards.findOne({_id: dbObj.primary});
                shardConn = new Mongo(shardObj.host);
            }
        });
        assert.neq(null, shardConn, "could not find shard hosting database " + db.getName());
        return shardConn;
    }

    /**
     * Returns true if we have a replica set.
     */
    function isReplSet(db) {
        const primaryInfo = db.isMaster();
        return primaryInfo.hasOwnProperty('setName');
    }

    return {
        isMongos: isMongos,
        awaitReplication: awaitReplication,
        awaitLastOpCommitted: awaitLastOpCommitted,
        runCommandOnEachPrimary: runCommandOnEachPrimary,
        getPrimaryForNodeHostingDatabase: getPrimaryForNodeHostingDatabase,
        isReplSet: isReplSet,
    };
})();

/*
 * Helper functions which connect to a server, and check its logs for particular strings.
 */
var checkLog;

(function() {
    "use strict";

    if (checkLog) {
        return;  // Protect against this file being double-loaded.
    }

    checkLog = (function() {
        var getGlobalLog = function(conn) {
            var cmdRes;
            try {
                cmdRes = conn.adminCommand({getLog: 'global'});
            } catch (e) {
                // Retry with network errors.
                print("checkLog ignoring failure: " + e);
                return null;
            }

            return assert.commandWorked(cmdRes).log;
        };

        /*
         * Calls the 'getLog' function at regular intervals on the provided connection 'conn' until
         * the provided 'msg' is found in the logs, or 5 minutes have elapsed. Throws an exception
         * on timeout.
         */
        var contains = function(conn, msg, timeout = 5 * 60 * 1000) {
            assert.soon(function() {
                var logMessages = getGlobalLog(conn);
                if (logMessages === null) {
                    return false;
                }
                for (var i = 0; i < logMessages.length; i++) {
                    if (logMessages[i].indexOf(msg) != -1) {
                        return true;
                    }
                }
                return false;
            }, 'Could not find log entries containing the following message: ' + msg, timeout, 300);
        };

        /*
         * Calls the 'getLog' function at regular intervals on the provided connection 'conn' until
         * the provided 'msg' is found in the logs exactly 'expectedCount' times, or 5 minutes have
         * elapsed.
         * Throws an exception on timeout.
         */
        var containsWithCount = function(conn, msg, expectedCount) {
            let count;
            assert.soon(
                function() {
                    count = 0;
                    var logMessages = getGlobalLog(conn);
                    if (logMessages === null) {
                        return false;
                    }
                    for (var i = 0; i < logMessages.length; i++) {
                        if (logMessages[i].indexOf(msg) != -1) {
                            count++;
                        }
                    }

                    return expectedCount === count;
                },
                'Expected ' + expectedCount + ' log entries containing the following message: ' +
                    msg + ' on node ' + conn.name,
                5 * 60 * 1000,
                300);
        };

        return {
            getGlobalLog: getGlobalLog,
            contains: contains,
            containsWithCount: containsWithCount
        };
    })();
})();

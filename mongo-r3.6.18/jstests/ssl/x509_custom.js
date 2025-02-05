// Test X509 auth with custom OIDs.

(function() {
    'use strict';

    const SERVER_CERT = 'jstests/libs/server.pem';
    const CA_CERT = 'jstests/libs/ca.pem';

    function testClient(conn, name) {
        let auth = {mechanism: 'MONGODB-X509'};
        if (name !== null) {
            auth.user = name;
        }
        const script = 'assert(db.getSiblingDB(\'$external\').auth(' + tojson(auth) + '));';
        clearRawMongoProgramOutput();
        const exitCode = runMongoProgram('mongo',
                                         '--ssl',
                                         '--sslAllowInvalidHostnames',
                                         '--sslPEMKeyFile',
                                         'jstests/libs/client-custom-oids.pem',
                                         '--sslCAFile',
                                         CA_CERT,
                                         '--port',
                                         conn.port,
                                         '--eval',
                                         script);

        assert.eq(exitCode, 0);
    }

    function runTest(conn) {
        const NAME =
            '1.2.3.45=Value\\,Rando,1.2.3.56=RandoValue,CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US';

        const admin = conn.getDB('admin');
        admin.createUser({user: "admin", pwd: "admin", roles: ["root"]});
        admin.auth('admin', 'admin');

        const external = conn.getDB('$external');
        external.createUser({user: NAME, roles: [{'role': 'readWrite', 'db': 'test'}]});

        testClient(conn, NAME);
        testClient(conn, null);
    }

    // Standalone.
    const mongod = MongoRunner.runMongod({
        auth: '',
        sslMode: 'requireSSL',
        sslPEMKeyFile: SERVER_CERT,
        sslCAFile: CA_CERT,
        sslAllowInvalidCertificates: '',
    });
    runTest(mongod);
    MongoRunner.stopMongod(mongod);
})();

// @tags: [
//   requires_fastcount,
//   requires_non_retryable_writes,
//   uses_multiple_connections,
// ]

// Test removal of Records that have been reused since the remove operation began.  SERVER-5198

t = db.jstests_removeb;
t.drop();

t.ensureIndex({a: 1});

// Make the index multikey to trigger cursor dedup checking.
t.insert({a: [-1, -2]});
t.remove({});

// Insert some data.
for (i = 0; i < 20000; ++i) {
    if (i % 100 == 0) {
        print(i + " of first set of 20000 documents inserted");
    }
    t.insert({a: i});
}

p = startParallelShell(
    // Wait until the remove operation (below) begins running.
    'while( db.jstests_removeb.count() == 20000 );' +
    // Insert documents with increasing 'a' values.  These inserted documents may
    // reuse Records freed by the remove operation in progress and will be
    // visited by the remove operation if it has not completed.
    'for( i = 20000; i < 40000; ++i ) {' +
    '    db.jstests_removeb.insert( { a:i } );' +
    '    if (i % 1000 == 0) {' +
    '        print( i-20000 + \" of second set of 20000 documents inserted\" );' +
    '    }' +
    '}');

// Remove using the a:1 index in ascending direction.
var res = t.remove({a: {$gte: 0}});
assert(!res.hasWriteError(), 'The remove operation failed.');

p();

t.drop();

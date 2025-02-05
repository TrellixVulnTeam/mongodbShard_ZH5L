#pragma once


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

#include "mongo/platform/unordered_map.h"

namespace mongo {

/** Thread safe map.
    Be careful not to use this too much or it could make things slow;
    if not a hot code path no problem.
    Examples:

    mapsf< std::map<int,int>, int, int > mp;

    int x = mp.get();

    std::map< std::map<int,int>, int, int > two;
    mp.swap(two);

    {
        mapsf< std::map<int,int>, int, int >::ref r(mp);
        r[9] = 1;
        std::map<int,int>::iterator i = r.r.begin();
    }
*/
template <class M>
struct mapsf {
    MONGO_DISALLOW_COPYING(mapsf);

    SimpleMutex m;
    M val;
    friend struct ref;

public:
    typedef typename M::const_iterator const_iterator;
    typedef typename M::key_type key_type;
    typedef typename M::mapped_type mapped_type;

    mapsf() : m("mapsf") {}
    void swap(M& rhs) {
        stdx::lock_guard<SimpleMutex> lk(m);
        val.swap(rhs);
    }
    bool empty() {
        stdx::lock_guard<SimpleMutex> lk(m);
        return val.empty();
    }
    // safe as we pass by value:
    mapped_type get(key_type k) {
        stdx::lock_guard<SimpleMutex> lk(m);
        const_iterator i = val.find(k);
        if (i == val.end())
            return mapped_type();
        return i->second;
    }
    // think about deadlocks when using ref.  the other methods
    // above will always be safe as they are "leaf" operations.
    struct ref {
        stdx::lock_guard<SimpleMutex> lk;

    public:
        M& r;
        ref(mapsf& m) : lk(m.m), r(m.val) {}
        mapped_type& operator[](const key_type& k) {
            return r[k];
        }
    };
};
}

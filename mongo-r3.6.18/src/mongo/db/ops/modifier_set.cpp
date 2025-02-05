
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

#include "mongo/db/ops/modifier_set.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/update/field_checker.h"
#include "mongo/db/update/log_builder.h"
#include "mongo/db/update/path_support.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace str = mongoutils::str;

struct ModifierSet::PreparedState {
    PreparedState(mutablebson::Document* targetDoc)
        : doc(*targetDoc), idxFound(0), elemFound(doc.end()), noOp(false), elemIsBlocking(false) {}

    // Document that is going to be changed.
    mutablebson::Document& doc;

    // Index in _fieldRef for which an Element exist in the document.
    size_t idxFound;

    // Element corresponding to _fieldRef[0.._idxFound].
    mutablebson::Element elemFound;

    // This $set is a no-op?
    bool noOp;

    // The element we find during a replication operation that blocks our update path
    bool elemIsBlocking;
};

ModifierSet::ModifierSet(ModifierSet::ModifierSetMode mode)
    : _fieldRef(), _posDollar(0), _setMode(mode), _val() {}

ModifierSet::~ModifierSet() {}

Status ModifierSet::init(const BSONElement& modExpr, const Options& opts, bool* positional) {
    //
    // field name analysis
    //

    // Break down the field name into its 'dotted' components (aka parts) and check that
    // the field is fit for updates
    _fieldRef.parse(modExpr.fieldName());
    Status status = fieldchecker::isUpdatable(_fieldRef);
    if (!status.isOK()) {
        return status;
    }

    // If a $-positional operator was used, get the index in which it occurred
    // and ensure only one occurrence.
    size_t foundCount;
    bool foundDollar = fieldchecker::isPositional(_fieldRef, &_posDollar, &foundCount);

    if (positional)
        *positional = foundDollar;

    if (foundDollar && foundCount > 1) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Too many positional (i.e. '$') elements found in path '"
                                    << _fieldRef.dottedField()
                                    << "'");
    }

    //
    // value analysis
    //

    if (!modExpr.ok())
        return Status(ErrorCodes::BadValue, "cannot $set an empty value");

    _val = modExpr;
    _fromOplogApplication = opts.fromOplogApplication;

    return Status::OK();
}

Status ModifierSet::prepare(mutablebson::Element root,
                            StringData matchedField,
                            ExecInfo* execInfo) {
    _preparedState.reset(new PreparedState(&root.getDocument()));

    // If we have a $-positional field, it is time to bind it to an actual field part.
    if (_posDollar) {
        if (matchedField.empty()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "The positional operator did not find the match "
                                           "needed from the query. Unexpanded update: "
                                        << _fieldRef.dottedField());
        }
        _fieldRef.setPart(_posDollar, matchedField);
    }

    // Locate the field name in 'root'. Note that we may not have all the parts in the path
    // in the doc -- which is fine. Our goal now is merely to reason about whether this mod
    // apply is a noOp or whether is can be in place. The remainin path, if missing, will
    // be created during the apply.
    Status status = pathsupport::findLongestPrefix(
        _fieldRef, root, &_preparedState->idxFound, &_preparedState->elemFound);
    const auto elemFoundIsArray =
        _preparedState->elemFound.ok() && _preparedState->elemFound.getType() == BSONType::Array;

    // FindLongestPrefix may say the path does not exist at all, which is fine here, or
    // that the path was not viable or otherwise wrong, in which case, the mod cannot
    // proceed.
    if (status.code() == ErrorCodes::NonExistentPath) {
        _preparedState->elemFound = root.getDocument().end();
    } else if (_fromOplogApplication && status.code() == ErrorCodes::PathNotViable) {
        // If we are applying an oplog entry and it is an invalid path, then push on indicating that
        // we had a blocking element, which we stopped at
        _preparedState->elemIsBlocking = true;
    } else if (!status.isOK()) {
        return status;
    }

    if (_setMode == SET_ON_INSERT) {
        execInfo->context = ModifierInterface::ExecInfo::INSERT_CONTEXT;
    }

    // We register interest in the field name. The driver needs this info to sort out if
    // there is any conflict among mods.
    execInfo->fieldRef[0] = &_fieldRef;

    //
    // in-place and no-op logic
    //

    // If the field path is not fully present, then this mod cannot be in place, nor is it a noOp.
    if (!_preparedState->elemFound.ok() || _preparedState->idxFound < (_fieldRef.numParts() - 1)) {
        if (elemFoundIsArray) {
            // Report that an existing array will gain a new element as a result of this mod.
            execInfo->indexOfArrayWithNewElement[0] = _preparedState->idxFound;
        }
        return Status::OK();
    }

    // If the value being $set is the same as the one already in the doc, than this is a noOp. We
    // use binary equality to compare so that any change to the document is considered, unlike using
    // a comparison that winds up in woCompare (see SERVER-16801). In the case where elemFound
    // doesn't have a serialized representation, we just declare the operation to not be a
    // no-op. This is potentially a missed optimization, but is unlikely to cause much pain since in
    // the normal update workflow we only admit one modification on any path from a leaf to the
    // document root. In that domain, hasValue will always be true. We may encounter a
    // non-serialized elemFound in the case where our base document is the result of calling
    // populateDocumentWithQueryFields, so this could cause us to do slightly more work than
    // strictly necessary in the case where an update (w upsert:true) becomes an insert.
    if (_preparedState->elemFound.ok() && _preparedState->idxFound == (_fieldRef.numParts() - 1) &&
        _preparedState->elemFound.hasValue() &&
        _preparedState->elemFound.getValue().binaryEqualValues(_val)) {
        execInfo->noOp = _preparedState->noOp = true;
    }

    return Status::OK();
}

Status ModifierSet::apply() const {
    dassert(!_preparedState->noOp);

    const bool destExists =
        _preparedState->elemFound.ok() && _preparedState->idxFound == (_fieldRef.numParts() - 1);
    // If there's no need to create any further field part, the $set is simply a value
    // assignment.
    if (destExists) {
        return _preparedState->elemFound.setValueBSONElement(_val);
    }

    //
    // Complete document path logic
    //

    // Creates the final element that's going to be $set in 'doc'.
    mutablebson::Document& doc = _preparedState->doc;
    StringData lastPart = _fieldRef.getPart(_fieldRef.numParts() - 1);
    mutablebson::Element elemToSet = doc.makeElementWithNewFieldName(lastPart, _val);
    if (!elemToSet.ok()) {
        return Status(ErrorCodes::InternalError, "can't create new element");
    }

    // Now, we can be in two cases here, as far as attaching the element being set goes:
    // (a) none of the parts in the element's path exist, or (b) some parts of the path
    // exist but not all.
    if (!_preparedState->elemFound.ok()) {
        _preparedState->elemFound = doc.root();
        _preparedState->idxFound = 0;
    } else {
        _preparedState->idxFound++;
    }

    // Remove the blocking element, if we are from replication applier. See comment below.
    if (_fromOplogApplication && !destExists && _preparedState->elemFound.ok() &&
        _preparedState->elemIsBlocking && (!(_preparedState->elemFound.isType(Array)) ||
                                           !(_preparedState->elemFound.isType(Object)))) {
        /**
         * With replication we want to be able to remove blocking elements for $set (only).
         * The reason they are blocking elements is that they are not embedded documents
         * (objects) nor an array (a special type of an embedded doc) and we cannot
         * add children to them (because the $set path requires adding children past
         * the blocking element).
         *
         * Imagine that we started with this:
         * {_id:1, a:1} + {$set : {"a.b.c" : 1}} -> {_id:1, a: {b: {c:1}}}
         * Above we found that element (a:1) is blocking at position 1. We now will replace
         * it with an empty object so the normal logic below can be
         * applied from the root (in this example case).
         *
         * Here is an array example:
         * {_id:1, a:[1, 2]} + {$set : {"a.0.c" : 1}} -> {_id:1, a: [ {c:1}, 2]}
         * The blocking element is "a.0" since it is a number, non-object, and we must
         * then replace it with an empty object so we can add c:1 to that empty object
         */

        mutablebson::Element blockingElem = _preparedState->elemFound;
        BSONObj newObj;
        // Replace blocking non-object with an empty object
        Status status = blockingElem.setValueObject(newObj);
        if (!status.isOK()) {
            return status;
        }
    }

    // createPathAt() will complete the path and attach 'elemToSet' at the end of it.
    return pathsupport::createPathAt(
               _fieldRef, _preparedState->idxFound, _preparedState->elemFound, elemToSet)
        .getStatus();
}

Status ModifierSet::log(LogBuilder* logBuilder) const {
    // We'd like to create an entry such as {$set: {<fieldname>: <value>}} under 'logRoot'.
    // We start by creating the {$set: ...} Element.
    mutablebson::Document& doc = logBuilder->getDocument();

    // Create the {<fieldname>: <value>} Element. Note that we log the mod with a
    // dotted field, if it was applied over a dotted field. The rationale is that the
    // secondary may be in a different state than the primary and thus make different
    // decisions about creating the intermediate path in _fieldRef or not.
    mutablebson::Element logElement =
        doc.makeElementWithNewFieldName(_fieldRef.dottedField(), _val);

    if (!logElement.ok()) {
        return Status(ErrorCodes::InternalError, "cannot create details for $set mod");
    }

    return logBuilder->addToSets(logElement);
}

}  // namespace mongo

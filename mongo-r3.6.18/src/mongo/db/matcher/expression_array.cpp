// expression_array.cpp


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

#include "mongo/db/matcher/expression_array.h"

#include "mongo/db/field_ref.h"
#include "mongo/db/jsobj.h"

namespace mongo {

bool ArrayMatchingMatchExpression::matchesSingleElement(const BSONElement& elt,
                                                        MatchDetails* details) const {
    if (elt.type() != BSONType::Array) {
        return false;
    }

    return matchesArray(elt.embeddedObject(), details);
}


bool ArrayMatchingMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const ArrayMatchingMatchExpression* realOther =
        static_cast<const ArrayMatchingMatchExpression*>(other);

    if (path() != realOther->path())
        return false;

    if (numChildren() != realOther->numChildren())
        return false;

    for (unsigned i = 0; i < numChildren(); i++)
        if (!getChild(i)->equivalent(realOther->getChild(i)))
            return false;
    return true;
}


// -------

Status ElemMatchObjectMatchExpression::init(StringData path, MatchExpression* sub) {
    _sub.reset(sub);
    return setPath(path);
}

bool ElemMatchObjectMatchExpression::matchesArray(const BSONObj& anArray,
                                                  MatchDetails* details) const {
    BSONObjIterator i(anArray);
    while (i.more()) {
        BSONElement inner = i.next();
        if (!inner.isABSONObj())
            continue;
        if (_sub->matchesBSON(inner.Obj(), NULL)) {
            if (details && details->needRecord()) {
                details->setElemMatchKey(inner.fieldName());
            }
            return true;
        }
    }
    return false;
}

void ElemMatchObjectMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << path() << " $elemMatch (obj)";

    MatchExpression::TagData* td = getTag();
    if (NULL != td) {
        debug << " ";
        td->debugString(&debug);
    }
    debug << "\n";
    _sub->debugString(debug, level + 1);
}

void ElemMatchObjectMatchExpression::serialize(BSONObjBuilder* out) const {
    BSONObjBuilder subBob;
    _sub->serialize(&subBob);
    out->append(path(), BSON("$elemMatch" << subBob.obj()));
}

MatchExpression::ExpressionOptimizerFunc ElemMatchObjectMatchExpression::getOptimizer() const {
    return [](std::unique_ptr<MatchExpression> expression) {
        auto& elemExpression = static_cast<ElemMatchObjectMatchExpression&>(*expression);
        elemExpression._sub = MatchExpression::optimize(std::move(elemExpression._sub));

        return expression;
    };
}

// -------

ElemMatchValueMatchExpression::~ElemMatchValueMatchExpression() {
    for (unsigned i = 0; i < _subs.size(); i++)
        delete _subs[i];
    _subs.clear();
}

Status ElemMatchValueMatchExpression::init(StringData path, MatchExpression* sub) {
    init(path).transitional_ignore();
    add(sub);
    return Status::OK();
}

Status ElemMatchValueMatchExpression::init(StringData path) {
    return setPath(path);
}


void ElemMatchValueMatchExpression::add(MatchExpression* sub) {
    verify(sub);
    _subs.push_back(sub);
}

bool ElemMatchValueMatchExpression::matchesArray(const BSONObj& anArray,
                                                 MatchDetails* details) const {
    BSONObjIterator i(anArray);
    while (i.more()) {
        BSONElement inner = i.next();

        if (_arrayElementMatchesAll(inner)) {
            if (details && details->needRecord()) {
                details->setElemMatchKey(inner.fieldName());
            }
            return true;
        }
    }
    return false;
}

bool ElemMatchValueMatchExpression::_arrayElementMatchesAll(const BSONElement& e) const {
    for (unsigned i = 0; i < _subs.size(); i++) {
        if (!_subs[i]->matchesSingleElement(e))
            return false;
    }
    return true;
}

void ElemMatchValueMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << path() << " $elemMatch (value)";

    MatchExpression::TagData* td = getTag();
    if (NULL != td) {
        debug << " ";
        td->debugString(&debug);
    }
    debug << "\n";
    for (unsigned i = 0; i < _subs.size(); i++) {
        _subs[i]->debugString(debug, level + 1);
    }
}

namespace {
/**
 * Helps consolidates double, triple, or more $nots into a single $not or zero $nots. Traverses the
 * children of 'expr' to find the first child of a $not which is not a single-child $and or a $not.
 * Returns that child and the number of $nots encountered along the way.
 */
std::pair<int, MatchExpression*> consolidateMultipleNotsAndGetChildOfNot(MatchExpression* expr) {
    using MatchType = MatchExpression::MatchType;
    invariant(expr->matchType() == MatchType::NOT);
    int numberOfConsecutiveNots = 1;
    expr = expr->getChild(0);

    auto isSingleChildAndContainingANot = [](auto* expr) {
        return expr->matchType() == MatchType::AND && expr->numChildren() == 1UL &&
            expr->getChild(0)->matchType() == MatchType::NOT;
    };
    while (true) {
        if (expr->matchType() == MatchType::NOT) {
            expr = expr->getChild(0);
            ++numberOfConsecutiveNots;
        } else if (isSingleChildAndContainingANot(expr)) {
            expr = expr->getChild(0);
            invariant(expr->matchType() == MatchType::NOT);
        } else {
            // This expression is not a $not or an $and containing only a $not, so we're done.
            break;
        }
    }
    return {numberOfConsecutiveNots, expr};
}
}  // namespace

void ElemMatchValueMatchExpression::serialize(BSONObjBuilder* out) const {
    BSONObjBuilder emBob;

    // NotMatchExpression will serialize to a $nor. This serialization is incorrect for
    // ElemMatchValue however as $nor is a top-level expression and expects that contained
    // expressions have path information. For this case we will serialize to $not.
    if (_subs.size() == 1 && _subs[0]->matchType() == MatchType::NOT) {
        int numberOfConsecutiveNots;
        MatchExpression* notChildExpr;
        std::tie(numberOfConsecutiveNots, notChildExpr) =
            consolidateMultipleNotsAndGetChildOfNot(_subs[0]);

        auto childList = std::vector<MatchExpression*>{notChildExpr};
        if (notChildExpr->matchType() == MatchType::AND) {
            childList = *notChildExpr->getChildVector();
        }

        const bool allChildrenArePathMatchExpression =
            std::all_of(childList.begin(), childList.end(), [](MatchExpression* child) {
                return dynamic_cast<PathMatchExpression*>(child);
            });

        if (allChildrenArePathMatchExpression) {
            BSONObjBuilder pathBuilder(out->subobjStart(path()));
            BSONObjBuilder elemMatchBuilder(pathBuilder.subobjStart("$elemMatch"));
            auto* builderForChildren = &elemMatchBuilder;
            auto notBuilder = boost::optional<BSONObjBuilder>();
            if (numberOfConsecutiveNots % 2 == 1) {
                notBuilder.emplace(elemMatchBuilder.subobjStart("$not"));
                builderForChildren = notBuilder.get_ptr();
            }

            for (auto&& child : childList) {
                BSONObjBuilder predicate;
                child->serialize(&predicate);
                builderForChildren->appendElements(predicate.obj().firstElement().embeddedObject());
            }
            return;
        }
    }

    for (unsigned i = 0; i < _subs.size(); i++) {
        BSONObjBuilder predicate;
        _subs[i]->serialize(&predicate);
        BSONObj predObj = predicate.obj();
        emBob.appendElements(predObj.firstElement().embeddedObject());
    }
    out->append(path(), BSON("$elemMatch" << emBob.obj()));
}

MatchExpression::ExpressionOptimizerFunc ElemMatchValueMatchExpression::getOptimizer() const {
    return [](std::unique_ptr<MatchExpression> expression) {
        auto& subs = static_cast<ElemMatchValueMatchExpression&>(*expression)._subs;

        for (MatchExpression*& subExpression : subs) {
            auto optimizedSubExpression =
                MatchExpression::optimize(std::unique_ptr<MatchExpression>(subExpression));
            subExpression = optimizedSubExpression.release();
        }

        return expression;
    };
}

// ---------

Status SizeMatchExpression::init(StringData path, int size) {
    _size = size;
    return setPath(path);
}

bool SizeMatchExpression::matchesArray(const BSONObj& anArray, MatchDetails* details) const {
    if (_size < 0)
        return false;
    return anArray.nFields() == _size;
}

void SizeMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << path() << " $size : " << _size << "\n";

    MatchExpression::TagData* td = getTag();
    if (NULL != td) {
        debug << " ";
        td->debugString(&debug);
    }
}

void SizeMatchExpression::serialize(BSONObjBuilder* out) const {
    out->append(path(), BSON("$size" << _size));
}

bool SizeMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const SizeMatchExpression* realOther = static_cast<const SizeMatchExpression*>(other);
    return path() == realOther->path() && _size == realOther->_size;
}


// ------------------
}  // namespace mongo

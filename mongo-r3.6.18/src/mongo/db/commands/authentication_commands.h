
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

#pragma once

#include <string>

#include "mongo/base/status.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/commands.h"

namespace mongo {

class CmdAuthenticate : public BasicCommand {
public:
    static void disableAuthMechanism(std::string authMechanism);

    virtual bool slaveOk() const {
        return true;
    }
    virtual void help(std::stringstream& ss) const {
        ss << "internal";
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}  // No auth required
    virtual void redactForLogging(mutablebson::Document* cmdObj);

    CmdAuthenticate() : BasicCommand("authenticate") {}
    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result);

private:
    /**
     * Completes the authentication of "user" using "mechanism" and parameters from "cmdObj".
     *
     * Returns Status::OK() on success.  All other statuses indicate failed authentication.  The
     * entire status returned here may always be used for logging.  However, if the code is
     * AuthenticationFailed, the "reason" field of the return status may contain information
     * that should not be revealed to the connected client.
     *
     * Other than AuthenticationFailed, common returns are BadValue, indicating unsupported
     * mechanism, and ProtocolError, indicating an error in the use of the authentication
     * protocol.
     */
    Status _authenticate(OperationContext* opCtx,
                         const std::string& mechanism,
                         const UserName& user,
                         const BSONObj& cmdObj);
    Status _authenticateCR(OperationContext* opCtx, const UserName& user, const BSONObj& cmdObj);
    Status _authenticateX509(OperationContext* opCtx, const UserName& user, const BSONObj& cmdObj);
};

extern CmdAuthenticate cmdAuthenticate;
}

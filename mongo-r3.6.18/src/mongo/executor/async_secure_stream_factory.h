
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

#include "mongo/config.h"

#ifdef MONGO_CONFIG_SSL

#include <asio.hpp>
#include <asio/ssl.hpp>

#include "mongo/executor/async_stream_factory_interface.h"

namespace mongo {
class SSLManagerInterface;
namespace executor {

class AsyncStreamInterface;

class AsyncSecureStreamFactory final : public AsyncStreamFactoryInterface {
public:
    AsyncSecureStreamFactory(SSLManagerInterface* sslManager);

    std::unique_ptr<AsyncStreamInterface> makeStream(asio::io_service::strand* strand,
                                                     const HostAndPort&) override;

private:
    asio::ssl::context _sslContext;
};

}  // namespace executor
}  // namespace mongo

#endif

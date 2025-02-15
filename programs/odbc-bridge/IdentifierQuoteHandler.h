#pragma once

#include <Common/Logger.h>
#include <Interpreters/Context.h>
#include <Server/HTTP/HTTPRequestHandler.h>

#include <Poco/Logger.h>

#if USE_ODBC

/// This handler establishes connection to database, and retrieve quote style identifier
namespace DB
{

class IdentifierQuoteHandler : public HTTPRequestHandler, WithContext
{
public:
    IdentifierQuoteHandler(size_t keep_alive_timeout_, ContextPtr context_)
        : WithContext(context_)
        , log(getLogger("IdentifierQuoteHandler"))
        , keep_alive_timeout(keep_alive_timeout_)
    {
    }

    void handleRequest(HTTPServerRequest & request, HTTPServerResponse & response) override;

private:
    LoggerPtr log;
    size_t keep_alive_timeout;
};

}

#endif

#include "PibtTcpServer.h"
#include "PlannerSession.h"

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>
#include "nlohmann/json.hpp"

#include <iostream>
#include <memory>
#include <string>

using boost::asio::ip::tcp;
using json = nlohmann::json;

// ─── JSON helpers ─────────────────────────────────────────────────────────────

namespace
{

json BuildHelloAck(const json& request, bool ok, const std::string& msg = "")
{
    json r;
    r["type"]      = "hello_ack";
    r["sessionId"] = request.value("sessionId", "");
    r["status"]    = ok ? "ok" : "error";
    r["server"]    = "pibt_tcp_server";
    r["planner"]   = "DefaultPlanner";
    if (!ok) r["message"] = msg;
    return r;
}

json BuildError(const std::string& type, const json& request, const std::string& message)
{
    json r;
    r["type"]      = type;
    r["sessionId"] = request.value("sessionId", "");
    r["status"]    = "error";
    r["message"]   = message;
    return r;
}

json BuildShutdownAck(const json& request)
{
    json r;
    r["type"]      = "shutdown_ack";
    r["sessionId"] = request.value("sessionId", "");
    r["status"]    = "ok";
    r["message"]   = "closing session";
    return r;
}

void WriteJsonLine(tcp::socket& socket, const json& payload)
{
    const std::string body = payload.dump() + "\n";
    boost::asio::write(socket, boost::asio::buffer(body));
}

// ─── Per-client session handler ───────────────────────────────────────────────

void HandleClient(tcp::socket socket)
{
    boost::system::error_code ec;
    boost::asio::streambuf    buffer;

    std::unique_ptr<PlannerSession> session;

    for (;;)
    {
        const std::size_t bytes = boost::asio::read_until(socket, buffer, '\n', ec);
        if (ec == boost::asio::error::eof)
        {
            std::cout << "[pibt_tcp_server] client disconnected\n";
            return;
        }
        if (ec)
        {
            std::cerr << "[pibt_tcp_server] read error: " << ec.message() << "\n";
            return;
        }

        std::istream    input(&buffer);
        std::string     line;
        line.reserve(bytes);
        std::getline(input, line);
        if (line.empty()) continue;

        json request;
        try
        {
            request = json::parse(line);
        }
        catch (const std::exception& ex)
        {
            std::cerr << "[pibt_tcp_server] invalid json: " << ex.what() << "\n";
            WriteJsonLine(socket, BuildError("protocol_error", json::object(), ex.what()));
            continue;
        }

        const std::string type = request.value("type", "");

        // ── hello ─────────────────────────────────────────────────────────────
        if (type == "hello")
        {
            const std::string sid      = request.value("sessionId", "");
            const int         teamSize = request.value("teamSize",  0);
            std::cout << "[pibt_tcp_server] hello session=" << sid
                      << " teamSize=" << teamSize << "\n";

            session = std::make_unique<PlannerSession>(sid);
            const bool ok = session->Initialize(request, /*preprocess_ms=*/3000);

            WriteJsonLine(socket, BuildHelloAck(request, ok,
                ok ? "" : "PlannerSession::Initialize failed"));
            continue;
        }

        // ── plan_step ─────────────────────────────────────────────────────────
        if (type == "plan_step")
        {
            const std::string sid      = request.value("sessionId", "");
            const int         reqId    = request.value("requestId", 0);
            const std::size_t nAgents  = request.contains("agents") && request["agents"].is_array()
                                         ? request["agents"].size() : 0;
            std::cout << "[pibt_tcp_server] plan_step session=" << sid
                      << " requestId=" << reqId
                      << " agents=" << nAgents << "\n";

            if (!session || !session->IsInitialized())
            {
                WriteJsonLine(socket, BuildError("plan_result", request,
                    "no active session; send hello first"));
                continue;
            }

            json result = session->Plan(request, /*time_limit_ms=*/90);
            WriteJsonLine(socket, result);
            continue;
        }

        // ── shutdown ──────────────────────────────────────────────────────────
        if (type == "shutdown")
        {
            const std::string sid = request.value("sessionId", "");
            std::cout << "[pibt_tcp_server] shutdown session=" << sid << "\n";
            session.reset();
            WriteJsonLine(socket, BuildShutdownAck(request));
            return;
        }

        WriteJsonLine(socket, BuildError("protocol_error", request, "unknown message type: " + type));
    }
}

} // namespace

// ─── PibtTcpServer ───────────────────────────────────────────────────────────

PibtTcpServer::PibtTcpServer(std::string host, std::uint16_t port)
    : host_(std::move(host)), port_(port)
{
}

int PibtTcpServer::Run()
{
    try
    {
        boost::asio::io_context io_context;
        const auto address = boost::asio::ip::make_address(host_);
        tcp::acceptor acceptor(io_context, tcp::endpoint(address, port_));

        std::cout << "[pibt_tcp_server] listening on " << host_ << ":" << port_
                  << " (planner=DefaultPlanner)\n";

        for (;;)
        {
            tcp::socket socket(io_context);
            acceptor.accept(socket);
            std::cout << "[pibt_tcp_server] client connected\n";
            HandleClient(std::move(socket));
        }
    }
    catch (const std::exception& ex)
    {
        std::cerr << "[pibt_tcp_server] fatal error: " << ex.what() << "\n";
        return 1;
    }
}

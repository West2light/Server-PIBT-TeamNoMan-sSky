#include "PibtTcpServer.h"
#include "FileLogger.h"
#include "PlannerSession.h"
#include "epibt.h"
#include "planner.h"

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>
#include "nlohmann/json.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

using boost::asio::ip::tcp;
using json = nlohmann::json;

// ─── JSON helpers ─────────────────────────────────────────────────────────────

namespace
{

std::string BuildLogFilePath()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);

    std::tm tm_value {};
#if defined(_WIN32)
    localtime_s(&tm_value, &now_time);
#else
    localtime_r(&now_time, &tm_value);
#endif

    std::ostringstream oss;
    oss << "adds/output/epibt_m0_server_defaultplanner_"
        << std::put_time(&tm_value, "%Y-%m-%d_%H-%M-%S")
        << ".txt";
    return oss.str();
}

json BuildHelloAck(
    const json& request,
    bool ok,
    const std::string& planner_name,
    const std::string& msg = "")
{
    json r;
    r["type"]      = "hello_ack";
    r["sessionId"] = request.value("sessionId", "");
    r["status"]    = ok ? "ok" : "error";
    r["server"]    = "pibt_tcp_server";
    r["planner"]   = planner_name;
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

void ResetSessionState(std::unique_ptr<PlannerSession>& session)
{
    session.reset();
    DefaultPlanner::reset();
    EpibtPlanner::reset();
}

// ─── Per-client session handler ───────────────────────────────────────────────

void HandleClient(tcp::socket socket)
{
    boost::system::error_code ec;
    boost::asio::streambuf    buffer;

    std::unique_ptr<PlannerSession> session;
    FileLogger::Info("[PibtTcpServer] client connected");

    for (;;)
    {
        const std::size_t bytes = boost::asio::read_until(socket, buffer, '\n', ec);
        if (ec == boost::asio::error::eof)
        {
            std::cout << "[pibt_tcp_server] client disconnected\n";
            FileLogger::Info("[PibtTcpServer] client disconnected");
            ResetSessionState(session);
            return;
        }
        if (ec)
        {
            std::cerr << "[pibt_tcp_server] read error: " << ec.message() << "\n";
            FileLogger::Error("[PibtTcpServer] socket read error: " + ec.message());
            ResetSessionState(session);
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
            FileLogger::Error(std::string("[PibtTcpServer] invalid json: ") + ex.what());
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
            FileLogger::Info(
                "[PibtTcpServer] hello session=" + sid +
                " teamSize=" + std::to_string(teamSize));

            ResetSessionState(session);
            session = std::make_unique<PlannerSession>(sid);
            const bool ok = session->Initialize(request, /*preprocess_ms=*/3000);

            if (!ok)
            {
                FileLogger::Error("[PibtTcpServer] hello initialize failed session=" + sid);
                ResetSessionState(session);
            }

            WriteJsonLine(socket, BuildHelloAck(request, ok,
                ok ? session->PlannerName() : "DefaultPlanner",
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
            FileLogger::Info(
                "[PibtTcpServer] plan_step session=" + sid +
                " requestId=" + std::to_string(reqId) +
                " agents=" + std::to_string(nAgents));

            if (!session || !session->IsInitialized())
            {
                FileLogger::Warn(
                    "[PibtTcpServer] plan_step rejected session=" + sid +
                    " reason=no active session");
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
            FileLogger::Info("[PibtTcpServer] shutdown session=" + sid);
            WriteJsonLine(socket, BuildShutdownAck(request));
            ResetSessionState(session);
            return;
        }

        FileLogger::Warn("[PibtTcpServer] unknown message type=" + type);
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
        const std::string log_path = BuildLogFilePath();
        if (!FileLogger::Initialize(log_path))
        {
            std::cerr << "[pibt_tcp_server] failed to initialize file logger\n";
            return 1;
        }

        boost::asio::io_context io_context;
        const auto address = boost::asio::ip::make_address(host_);
        tcp::acceptor acceptor(io_context, tcp::endpoint(address, port_));

        std::cout << "[pibt_tcp_server] listening on " << host_ << ":" << port_
                  << " (planner=DefaultPlanner)\n";
        FileLogger::Info(
            "[PibtTcpServer] listening host=" + host_ +
            " port=" + std::to_string(port_) +
            " planner=DefaultPlanner" +
            " logPath=" + log_path);

        for (;;)
        {
            tcp::socket socket(io_context);
            acceptor.accept(socket);
            HandleClient(std::move(socket));
        }
    }
    catch (const std::exception& ex)
    {
        std::cerr << "[pibt_tcp_server] fatal error: " << ex.what() << "\n";
        FileLogger::Error(std::string("[PibtTcpServer] fatal error: ") + ex.what());
        return 1;
    }
}

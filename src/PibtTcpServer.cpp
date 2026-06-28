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
#include <thread>

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
        << ".log";
    return oss.str();
}

void WriteJsonLine(tcp::socket& socket, const json& j)
{
    boost::system::error_code ec;
    const std::string payload = j.dump() + "\n";
    boost::asio::write(socket, boost::asio::buffer(payload), ec);
    if (ec)
    {
        std::cerr << "[pibt_tcp_server] write error: " << ec.message() << "\n";
        throw boost::system::system_error(ec);
    }
}

json BuildHelloAck(const json& req, bool success, const std::string& planner, const std::string& error = "")
{
    json r         = json::object();
    r["type"]      = "hello_ack";
    r["sessionId"] = req.value("sessionId", "");
    r["success"]   = success;
    r["planner"]   = planner;
    if (!success)
        r["error"] = error;
    return r;
}

json BuildResetAck(const json& req)
{
    json r         = json::object();
    r["type"]      = "reset_ack";
    r["sessionId"] = req.value("sessionId", "");
    r["success"]   = true;
    return r;
}

json BuildShutdownAck(const json& req)
{
    json r         = json::object();
    r["type"]      = "shutdown_ack";
    r["sessionId"] = req.value("sessionId", "");
    r["success"]   = true;
    return r;
}

json BuildError(const std::string& type, const json& req, const std::string& reason)
{
    json r         = json::object();
    r["type"]      = type;
    r["sessionId"] = req.value("sessionId", "");
    r["success"]   = false;
    r["error"]     = reason;
    return r;
}

void ResetAllSessionState(std::unique_ptr<PlannerSession>& session)
{
    session.reset();
    DefaultPlanner::reset();   // ResetPlannerGlobals: clears all namespace globals + reseeds mt1/srand
    EpibtPlanner::reset();     // ResetState: clears g_operations / g_last_agent_traces / etc.
}

void HandleClient(tcp::socket& socket)
{
    boost::system::error_code ec;
    boost::asio::streambuf    buffer;

    std::unique_ptr<PlannerSession> session;
    FileLogger::Info("[PibtTcpServer] client connected");

    for (;;)
    {
        const std::size_t bytes = boost::asio::read_until(socket, buffer, '\n', ec);
        if (ec)
        {
            if (ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset)
            {
                std::cout << "[pibt_tcp_server] client disconnected (EOF/reset)\n";
                FileLogger::Info("[PibtTcpServer] client disconnected (EOF/reset)");
            }
            else if (ec == boost::asio::error::operation_aborted)
            {
                std::cout << "[pibt_tcp_server] client disconnected (operation_aborted)\n";
                FileLogger::Info("[PibtTcpServer] client disconnected (operation_aborted)");
            }
            else
            {
                std::cerr << "[pibt_tcp_server] read error: " << ec.message() << "\n";
                FileLogger::Warn("[PibtTcpServer] read error: " + ec.message());
            }
            break;
        }

        std::istream is(&buffer);
        std::string  line;
        std::getline(is, line);

        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (line.empty())
            continue;

        json request;
        try
        {
            request = json::parse(line);
        }
        catch (const json::parse_error& pe)
        {
            std::cerr << "[pibt_tcp_server] parse error: " << pe.what() << "\n";
            FileLogger::Warn(std::string("[PibtTcpServer] parse error: ") + pe.what());
            WriteJsonLine(socket, BuildError("protocol_error", json::object(), "invalid json"));
            continue;
        }

        const std::string type = request.value("type", "");

        // ── hello (S-R1) ──────────────────────────────────────────────────────
        if (type == "hello")
        {
            const std::string sid = request.value("sessionId", "");
            const int mapW        = request.value("map", json::object()).value("width", 0);
            const int mapH        = request.value("map", json::object()).value("height", 0);
            std::cout << "[pibt_tcp_server] hello session=" << sid
                      << " map=" << mapW << "x" << mapH << "\n";

            FileLogger::Info("[PibtTcpServer] hello session=" + sid +
                             " map=" + std::to_string(mapW) + "x" + std::to_string(mapH));

            ResetAllSessionState(session);
            session = std::make_unique<PlannerSession>(sid);

            const bool ok = session->Initialize(request, /*preprocess_ms=*/3000);

            if (!ok)
            {
                FileLogger::Error("[PibtTcpServer] hello initialize failed session=" + sid);
                ResetAllSessionState(session);
            }

            WriteJsonLine(socket, BuildHelloAck(request, ok,
                ok ? session->PlannerName() : "DefaultPlanner",
                ok ? "" : "PlannerSession::Initialize failed"));
            continue;
        }

        // ── plan_step (C-R1) ──────────────────────────────────────────────────
        if (type == "plan_step")
        {
            const std::string sid = request.value("sessionId", "");
            const int reqId       = request.value("requestId", 0);
            const int nAgents     = request.value("agents", json::array()).size();

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

        // ── reset (S-R2): reuse live connection for a new game session ───────
        if (type == "reset")
        {
            const std::string sid = request.value("sessionId", "");
            std::cout << "[pibt_tcp_server] reset session=" << sid << "\n";
            FileLogger::Info("[PibtTcpServer] reset session=" + sid);
            ResetAllSessionState(session);
            // Guard write — client may fire-and-forget without reading the ack
            try { WriteJsonLine(socket, BuildResetAck(request)); }
            catch (const std::exception& ex)
            {
                std::cerr << "[pibt_tcp_server] reset_ack write error (client already closed?): "
                          << ex.what() << "\n";
            }
            continue;
        }

        // ── shutdown ──────────────────────────────────────────────────────────
        if (type == "shutdown")
        {
            const std::string sid = request.value("sessionId", "");
            std::cout << "[pibt_tcp_server] shutdown session=" << sid << "\n";
            FileLogger::Info("[PibtTcpServer] shutdown session=" + sid);
            // Guard write — client may fire-and-forget (C-R2) without reading the ack
            try { WriteJsonLine(socket, BuildShutdownAck(request)); }
            catch (const std::exception& ex)
            {
                std::cerr << "[pibt_tcp_server] shutdown_ack write error (client already closed?): "
                          << ex.what() << "\n";
            }
            ResetAllSessionState(session);
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

        std::thread client_thread;
        std::shared_ptr<tcp::socket> current_socket;

        for (;;)
        {
            auto new_socket = std::make_shared<tcp::socket>(io_context);
            acceptor.accept(*new_socket);

            if (current_socket) {
                boost::system::error_code ec;
                current_socket->shutdown(boost::asio::socket_base::shutdown_both, ec);
                current_socket->close(ec); // forcefully interrupt the old blocking read
            }

            if (client_thread.joinable()) {
                client_thread.join();
            }

            current_socket = new_socket;
            client_thread = std::thread([current_socket]() {
                HandleClient(*current_socket);
            });
        }
    }
    catch (const std::exception& ex)
    {
        std::cerr << "[pibt_tcp_server] fatal error: " << ex.what() << "\n";
        FileLogger::Error(std::string("[PibtTcpServer] fatal error: ") + ex.what());
        return 1;
    }
}

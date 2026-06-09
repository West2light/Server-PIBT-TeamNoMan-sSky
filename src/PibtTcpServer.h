#pragma once

#include <cstdint>
#include <string>

class PibtTcpServer
{
public:
    PibtTcpServer(std::string host, std::uint16_t port);

    int Run();

private:
    std::string host_;
    std::uint16_t port_;
};

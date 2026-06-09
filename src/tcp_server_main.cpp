#include "PibtTcpServer.h"

#include <boost/program_options.hpp>

#include <cstdint>
#include <iostream>
#include <string>

namespace po = boost::program_options;

int main(int argc, char** argv)
{
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help", "produce help message")
        ("host", po::value<std::string>()->default_value("127.0.0.1"), "host to bind")
        ("port", po::value<std::uint16_t>()->default_value(7777), "port to bind");

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help"))
    {
        std::cout << desc << std::endl;
        return 0;
    }

    PibtTcpServer server(
        vm["host"].as<std::string>(),
        vm["port"].as<std::uint16_t>());
    return server.Run();
}

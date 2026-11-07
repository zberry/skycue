// Parses a tcp_listen endpoint and prints its description.
#include "skycue/net/endpoint.hpp"

#include <iostream>

int main()
{
    skycue::net::EndpointFields f;
    f.transport = "tcp_listen";
    f.port = "30005";
    const auto e = skycue::net::parseEndpoint(f, skycue::net::Role::receive);
    std::cout << skycue::net::describe(e) << "\n";
    return 0;
}

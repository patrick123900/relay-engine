// Test-only background harness for bridge integration: trusted host and agent dispatch, no SDL.
#include "relay/control/control_protocol.hpp"
#include "relay/core/engine.hpp"
#include "relay/core/json.hpp"
#include <iostream>
#include <string>
int main() {
    relay::Engine engine;
    engine.pause();
    relay::ControlProtocol protocol(engine);
    protocol.configure_agent_from_environment();
    std::string line;
    while (std::getline(std::cin, line)) {
        relay::JsonParser parser(line);
        const auto value = parser.parse();
        if (!value || !value->object()) return 2;
        const auto* host = relay::field(*value->object(), "host");
        const auto* request = relay::field(*value->object(), "request");
        if (!request || !request->object()) return 2;
        const auto serialized = relay::json_stringify(*request);
        std::cout << (host && host->boolean() && *host->boolean() ? protocol.handle(serialized) : protocol.handle_agent(serialized)) << std::endl;
    }
}

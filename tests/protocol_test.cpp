#include "protocol.h"
#include <iostream>
#include <stdexcept>

int main(int argc, char**) {
    auto require = [](bool value, const char* message) {
        if (!value) throw std::runtime_error(message);
    };
    try {
        // The red control deliberately makes an unsafe command appear valid.
        require(!(argc > 1 || bridge::valid_command("one\ntwo")),
            "RELATIONSHIP: one request must not contain multiple commands");
        require(!bridge::valid_command(std::string("one\0two", 7)), "RELATIONSHIP: NUL rejected");
        require(!bridge::valid_command(std::string(1024, 'x')), "RELATIONSHIP: engine input bound enforced");
        require(bridge::valid_command("player.getpos x"), "RELATIONSHIP: arbitrary console text accepted");
        require(!bridge::valid_command("  "), "RELATIONSHIP: empty command rejected");
        require(bridge::valid_id(std::string(32, 'a')), "RELATIONSHIP: valid request identity accepted");
        require(!bridge::valid_id("../../other"), "RELATIONSHIP: path traversal rejected");
        require(bridge::quote("\"\n\\") == "\"\\\"\\u000a\\\\\"", "RELATIONSHIP: JSON escaping roundtrip shape");
        std::cout << "PASS: protocol relationships\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

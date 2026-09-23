#include <iostream>
#include <stdexcept>
#include "transport_health_gate.h"

int main() {
    try {
        smv::TransportHealthGate gate;
        auto check = [](bool ok, const char* message) {
            if (!ok)
                throw std::runtime_error(message);
        };
        check(!gate.IsOperational(), "boot permitted actions without a handshake");
        const uint32_t boot_handshake = gate.Snapshot();
        check(gate.RestoreIfUnchanged(boot_handshake), "first handshake rejected");
        check(gate.IsOperational(), "first handshake failed to restore");
        const uint32_t old_reconnect = gate.Snapshot();
        gate.MarkLost();  // MQTT disconnect, without any physical network event.
        check(!gate.IsOperational(), "MQTT transport loss not gated");
        check(!gate.RestoreIfUnchanged(old_reconnect), "queued old reconnect allowed");
        const uint32_t mqtt_reconnect = gate.Snapshot();
        gate.MarkLost();  // WebSocket loss occurs while reconnect is pending.
        check(!gate.RestoreIfUnchanged(mqtt_reconnect), "stale restore after later loss");
        check(!gate.IsOperational(), "stale reconnect unblocked physical confirm");
        const uint32_t new_handshake = gate.Snapshot();
        check(gate.RestoreIfUnchanged(new_handshake), "new handshake rejected");
        check(gate.IsOperational(), "new handshake failed to restore");
        gate.MarkLost();
        check(!gate.IsOperational(), "subsequent loss was ignored");
        std::cout << "HOST_TRANSPORT_HEALTH_GATE_TESTS_PASS=10\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

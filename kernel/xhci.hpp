#pragma once

#include <stdint.h>

namespace kernel {
namespace xhci {

struct UsbSetupPacket {
	uint8_t bm_request_type;
	uint8_t b_request;
	uint16_t w_value;
	uint16_t w_index;
	uint16_t w_length;
};

struct Endpoint {
	uint8_t dci;
	uint16_t max_packet;
	bool in;
};

struct Device {
	uint8_t slot_id;
	uint8_t port_id;
	uint8_t speed;
	Endpoint ep0;
	Endpoint bulk_in;
	Endpoint bulk_out;
};

bool init();
bool ready();
bool get_mass_storage_device(Device* out);
bool control_transfer(const Device& dev, const UsbSetupPacket& setup, void* data, uint32_t length);
bool bulk_transfer(const Device& dev, const Endpoint& ep, void* data, uint32_t length);
void print_info();
// Called from IRQ path to consume xHCI events
void handle_irq();
// Start a kernel background task that polls the xHCI event ring periodically.
void start_poll_task();
// Wait for a previously submitted command/transfer identified by `ptr`.
// Returns true if completion code == 1 (success), false otherwise. `completion` receives the completion code.
bool wait_for_completion(uint64_t ptr, uint8_t* completion);

} // namespace xhci
} // namespace kernel

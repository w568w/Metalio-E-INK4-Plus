#pragma once
#include <functional>
#include <string>
namespace novelia {
// Start/Stop run on the serial worker. HTTP handlers enqueue work without waiting.
std::string StartPortal(std::function<bool(std::string)> submit);
void StopPortal();
void PortalStatus(const std::string& message, const std::string& public_config = {});
} // namespace novelia

#pragma once

#include <string>

namespace epubctl {

std::string selectBootHook();
void enableBoot(const std::string& hook);
void disableBoot();
int runBoot();

} // namespace epubctl

#pragma once

#include "runtime.hpp"

#include <string>

namespace epubctl {

void activate(const std::string& payload);
void deactivate();

void selectCatalogue(RuntimeSelection& selection);
void registerCatalogue(const RuntimeSelection& selection);
void restoreCatalogue();

void startWorker(const RuntimeSelection& selection);
void stopWorker();
int superviseWorker();

} // namespace epubctl

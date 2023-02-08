/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <regex>

#include "content_analysis/sdk/analysis_agent.h"
#include "handler.h"
#include "agent_win.h"

// Global app config.
std::string pipePath;
Mode mode;
unsigned long delay = 0;  // In seconds.

// Command line parameters.
constexpr const char* kArgDelaySpecific = "--delay=";
constexpr const char* kArgMode = "--mode=";
constexpr const char* kArgPipeBaseName = "--pipename=";
constexpr const char* kArgHelp = "--help";

bool ParseCommandLine(int argc, char* argv[]) {
  std::string modeStr;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.find(kArgDelaySpecific) == 0) {
      delay = std::stoul(arg.substr(strlen(kArgDelaySpecific)));
      if (delay > 30) {
        delay = 30;
      }
    } else if (arg.find(kArgPipeBaseName) == 0) {
      pipePath = arg.substr(strlen(kArgPipeBaseName));
    } else if (arg.find(kArgMode) == 0) {
      modeStr = arg.substr(strlen(kArgMode));
    } else if (arg.find(kArgHelp) == 0) {
      return false;
    }
  }
  if (pipePath.empty()) {
    std::cout << "No pipe path specified!" << std::endl;
    return false;
  }
  if (modeStr.empty()) {
    std::cout << "No mode specified!" << std::endl;
    return false;
  }
  if (sStringToMode.find(modeStr) == sStringToMode.end()) {
    std::cout << "\"" << modeStr << "\""
              << " is not a valid mode!" << std::endl;
    return false;
  }

  mode = sStringToMode[modeStr];
  return true;
}

void PrintHelp() {
  std::cout
      << std::endl
      << std::endl
      << "Usage: agent [OPTIONS]" << std::endl
      << "A simple agent to process content analysis requests." << std::endl
      << "Data containing the string 'block' blocks the request data from "
         "being used."
      << std::endl
      << std::endl
      << "Options:" << std::endl
      << kArgDelaySpecific
      << "<delay> : Add a delay to request processing in seconds (max 30)."
      << std::endl
      << kArgPipeBaseName
      << "<pipe name> : Pipe name (instead of 'path_system' or 'path_user')."
      << std::endl
      << kArgMode << "<mode> : Mode." << std::endl
      << "  Allowed modes: ";
  for (const auto& allowedMode : sStringToMode) {
    std::cout << allowedMode.first << " ";
  }
  std::cout << std::endl;
  std::cout << kArgHelp << " : prints this help message" << std::endl;
}

int main(int argc, char* argv[]) {
  if (!ParseCommandLine(argc, argv)) {
    PrintHelp();
    return 1;
  }

  // Each agent uses a unique name to identify itself with Google Chrome.
  content_analysis::sdk::ResultCode rc;
  auto agent = content_analysis::sdk::Agent::Create(
      {pipePath, false}, std::make_unique<Handler>(delay, mode), &rc);
  if (!agent || rc != content_analysis::sdk::ResultCode::OK) {
    std::cout << "[Demo] Error starting agent: "
              << content_analysis::sdk::ResultCodeToString(rc) << std::endl;
    return 1;
  };

  std::cout << "[Demo] " << agent->DebugString() << std::endl;

  // Blocks, sending events to the handler until agent->Stop() is called.
  rc = agent->HandleEvents();
  if (rc != content_analysis::sdk::ResultCode::OK) {
    std::cout << "[Demo] Error from handling events: "
              << content_analysis::sdk::ResultCodeToString(rc) << std::endl;
    std::cout << "[Demo] " << agent->DebugString() << std::endl;
  }

  return 0;
}

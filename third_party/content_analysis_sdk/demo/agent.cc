// Copyright 2022 The Chromium Authors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <regex>

#include "content_analysis/sdk/analysis_agent.h"
#include "demo/handler.h"

// Different paths are used depending on whether this agent should run as a
// use specific agent or not.  These values are chosen to match the test
// values in chrome browser.
//constexpr char kPathUser[] = "fx-user";
//constexpr char kPathSystem[] = "fx-system";
constexpr char kPathUser[] = "path_user";
constexpr char kPathSystem[] = "path_system";

// Global app config.
std::string path = kPathSystem;
bool user_specific = false;
unsigned long delay = 0;  // In seconds.
std::vector<std::pair<std::string, std::regex>> toBlock;

// Command line parameters.
constexpr const char* kArgUserSpecific = "--user";
constexpr const char* kArgDelaySpecific = "--delay=";
constexpr const char* kArgToBlock = "--toblock=";
constexpr const char* kArgPipeBaseName = "--pipename=";
constexpr const char* kArgHelp = "--help";

std::vector<std::pair<std::string, std::regex>>
ParseToBlock(const std::string toBlock) {
  std::vector<std::pair<std::string, std::regex>> ret;
  for (auto it = toBlock.begin(); it != toBlock.end(); /* nop */) {
    auto it2 = std::find(it, toBlock.end(), ',');
    ret.push_back(std::make_pair(std::string(it, it2), std::regex(it, it2)));
    it = it2 == toBlock.end() ? it2 : it2 + 1;
  }

  return ret;
}

bool ParseCommandLine(int argc, char* argv[]) {
  bool setCustomPipeName = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.find(kArgUserSpecific) == 0) {
      if (!setCustomPipeName) {
        // custom pipe name takes precedence over this
        path = kPathUser;
      }
      user_specific = true;
    } else if (arg.find(kArgDelaySpecific) == 0) {
      delay = std::stoul(arg.substr(strlen(kArgDelaySpecific)));
      if (delay > 30) {
          delay = 30;
      }
    } else if (arg.find(kArgToBlock) == 0) {
      toBlock = ParseToBlock(arg.substr(strlen(kArgToBlock)));
    } else if (arg.find(kArgPipeBaseName) == 0) {
      setCustomPipeName = true;
      path = arg.substr(strlen(kArgPipeBaseName));
    } else if (arg.find(kArgHelp) == 0) {
      return false;
    }
  }

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
      << kArgUserSpecific << " : Make agent OS user specific" << std::endl
      << kArgDelaySpecific
      << "<delay> : Add a delay to request processing in seconds (max 30)."
      << std::endl
      << kArgToBlock
      << "<regex> : Regular expression matching file and text content to block."
      << std::endl
      << kArgPipeBaseName
      << "<pipe name> : Pipe name (instead of 'path_system' or 'path_user')."
      << std::endl
      << kArgHelp << " : prints this help message" << std::endl;
}

int main(int argc, char* argv[]) {
  if (!ParseCommandLine(argc, argv)) {
    PrintHelp();
    return 1;
  }

  // Each agent uses a unique name to identify itself with Google Chrome.
  content_analysis::sdk::ResultCode rc;
  auto agent = content_analysis::sdk::Agent::Create(
      {path, user_specific}, std::make_unique<Handler>(delay, toBlock), &rc);
  if (!agent || rc != content_analysis::sdk::ResultCode::OK) {
    std::cout << "[Demo] Error starting agent: "
              << content_analysis::sdk::ResultCodeToString(rc)
              << std::endl;
    return 1;
  };

  std::cout << "[Demo] " << agent->DebugString() << std::endl;

  // Blocks, sending events to the handler until agent->Stop() is called.
  rc = agent->HandleEvents();
  if (rc != content_analysis::sdk::ResultCode::OK) {
    std::cout << "[Demo] Error from handling events: "
              << content_analysis::sdk::ResultCodeToString(rc)
              << std::endl;
    std::cout << "[Demo] " << agent->DebugString() << std::endl;
  }

  return 0;
}

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef mozilla_testcontentanalysis_h
#define mozilla_testcontentanalysis_h

#include "content_analysis/sdk/analysis_client.h"
#include "gtest/gtest.h"
#include "nsString.h"
#include <combaseapi.h>
#include <rpc.h>
#include <windows.h>

struct MozAgentInfo {
  PROCESS_INFORMATION processInfo;
  std::unique_ptr<content_analysis::sdk::Client> client;
};

nsString GeneratePipeName(const wchar_t* prefix) {
  nsString name(prefix);
  UUID uuid;
  EXPECT_EQ(RPC_S_OK, UuidCreate(&uuid));
  // 39 == length of a UUID string including braces and NUL.
  wchar_t guidBuf[39] = {};
  EXPECT_EQ(39, StringFromGUID2(uuid, guidBuf, 39));
  // omit opening and closing braces (and trailing null)
  name.Append(&guidBuf[1], 36);
  return name;
}

MozAgentInfo LaunchAgentWithCommandLine(const nsString& cmdLine,
                                     const nsString& pipeName) {
  STARTUPINFOW startupInfo = {sizeof(startupInfo)};
  PROCESS_INFORMATION processInfo;
  BOOL ok = ::CreateProcessW(nullptr, cmdLine.get(), nullptr, nullptr, FALSE, 0,
                             nullptr, nullptr, &startupInfo, &processInfo);
  // The documentation for CreateProcessW() says that any non-zero value is a
  // success
  if (ok == FALSE) {
    // Show the last error
    EXPECT_EQ(0UL, GetLastError())
        << "Failed to launch content_analysis_sdk_agent";
  }
  // Allow time for the agent to set up the pipe
  ::Sleep(2000);
  content_analysis::sdk::Client::Config config;
  config.name = NS_ConvertUTF16toUTF8(pipeName);
  auto clientPtr = content_analysis::sdk::Client::Create(config);
  EXPECT_NE(nullptr, clientPtr.get());

  return MozAgentInfo{processInfo, std::move(clientPtr)};
}

#endif

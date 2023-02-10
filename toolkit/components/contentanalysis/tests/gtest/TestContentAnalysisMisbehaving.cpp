/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "mozilla/ArrayUtils.h"
#include "mozilla/Assertions.h"
#include "mozilla/CmdLineAndEnvUtils.h"
#include "content_analysis/sdk/analysis_client.h"
#include "TestContentAnalysis.h"
#include <processenv.h>
#include <synchapi.h>
#include <windows.h>

using namespace content_analysis::sdk;

namespace {
AgentInfo LaunchAgentMisbehaving(const wchar_t* mode) {
  nsString cmdLine = nsString(
      L"..\\..\\dist\\bin\\content_analysis_sdk_agent_misbehaving.exe");
  cmdLine.Append(L" --mode=");
  cmdLine.Append(mode);
  cmdLine.Append(L" --pipename=");
  nsString pipeName = GeneratePipeName(L"contentanalysissdk-gtest-");
  cmdLine.Append(pipeName);
  return LaunchAgentWithCommandLine(cmdLine, pipeName);
}
}  // namespace

TEST(ContentAnalysisMisbehaving, LargeResponse)
{
  auto agentInfo = LaunchAgentMisbehaving(L"largeResponse");
  // Exit the test early if the process failed to launch
  ASSERT_NE(agentInfo.processInfo.dwProcessId, 0UL);
  EXPECT_NE(nullptr, agentInfo.client.get());

  ContentAnalysisRequest request;
  request.set_request_token("request token");
  request.set_text_content("unused");
  ContentAnalysisResponse response;
  EXPECT_EQ(0, agentInfo.client->Send(request, &response));
  EXPECT_STREQ("request token", response.request_token().c_str());
  EXPECT_EQ(1001, response.results().size());

  BOOL terminateResult = ::TerminateProcess(agentInfo.processInfo.hProcess, 0);
  EXPECT_NE(FALSE, terminateResult)
      << "Failed to terminate content_analysis_sdk_agent process";
}
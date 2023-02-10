// Copyright 2022 The Chromium Authors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <windows.h>

#include <utility>
#include <vector>

#include "common/utils_win.h"

#include "client_win.h"
#include <iostream>

namespace content_analysis {
namespace sdk {

const DWORD kBufferSize = 4096;

// static
std::unique_ptr<Client> Client::Create(Config config) {
  int rc;
  auto client = std::make_unique<ClientWin>(std::move(config), &rc);
  std::cout << rc << std::endl;
  return rc == 0 ? std::move(client) : nullptr;
}

ClientWin::ClientWin(Config config, int* rc) : ClientBase(std::move(config)) {
  *rc = -1;

  std::string pipename =
    internal::GetPipeName(configuration().name, configuration().user_specific);
  if (pipename.empty()) {
    return;
  }

  pipename_ = pipename;
  std::cout << "pipename is " << pipename_ << std::endl;
  DWORD err = ConnectToPipe(pipename_, &hPipe_);
  if (err != ERROR_SUCCESS) {
    std::cout << "failed to connect to pipe!! err=" << err << std::endl;
    Shutdown();
  } else {
    *rc = 0;
  }
}

ClientWin::~ClientWin() {
  Shutdown();
}

int ClientWin::Send(const ContentAnalysisRequest& request,
                    ContentAnalysisResponse* response) {
  // TODO: could avoid a copy by changing first argument to be
  // `ContentAnalysisRequest request` and then using std::move() below and at
  // call site.
  ChromeToAgent chrome_to_agent;
  *chrome_to_agent.mutable_request() = request;
  bool success = WriteMessageToPipe(hPipe_,
                                    chrome_to_agent.SerializeAsString());
  if (success) {
    std::vector<char> buffer = ReadNextMessageFromPipe(hPipe_);
    AgentToChrome agent_to_chrome;
    success = agent_to_chrome.ParseFromArray(buffer.data(), buffer.size());
    if (success) {
      *response = std::move(*agent_to_chrome.mutable_response());
    }
  }

  return success ? 0 : -1;
}

int ClientWin::Acknowledge(const ContentAnalysisAcknowledgement& ack) {
  // TODO: could avoid a copy by changing argument to be
  // `ContentAnalysisAcknowledgement ack` and then using std::move() below and
  // at call site.
  ChromeToAgent chrome_to_agent;
  *chrome_to_agent.mutable_ack() = ack;
  return WriteMessageToPipe(hPipe_, chrome_to_agent.SerializeAsString())
      ? 0 : -1;
}

int ClientWin::CancelRequests(const ContentAnalysisCancelRequests& cancel) {
  // TODO: could avoid a copy by changing argument to be
  // `ContentAnalysisCancelRequests cancel` and then using std::move() below and
  // at call site.
  ChromeToAgent chrome_to_agent;
  *chrome_to_agent.mutable_cancel() = cancel;
  return WriteMessageToPipe(hPipe_, chrome_to_agent.SerializeAsString())
      ? 0 : -1;
}

// static
DWORD ClientWin::ConnectToPipe(const std::string& pipename, HANDLE* handle) {
  HANDLE h = INVALID_HANDLE_VALUE;
  while (h == INVALID_HANDLE_VALUE) {
    std::cout << "ConnectToPipe:top of loop" << std::endl;
    h = CreateFileA(pipename.c_str(),
                    GENERIC_READ | GENERIC_WRITE,
                    /*shareMode=*/0,
                    /*securityAttr=*/nullptr, OPEN_EXISTING,
                    /*flags=*/SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                    /*template=*/nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      std::cout << "ConnectToPipe:got invalid handle" << std::endl;
      auto err = GetLastError();
      if (err != ERROR_PIPE_BUSY) {
        std::cout << "ConnectToPipe:got real error=" << err << std::endl;
        break;
      }

      if (!WaitNamedPipeA(pipename.c_str(), NMPWAIT_USE_DEFAULT_WAIT)) {
        std::cout << "ConnectToPipe:got error waiting for pipe="
                  << GetLastError() << std::endl;
        break;
      }
      std::cout << "ConnectToPipe:got invalid handle but continuing"
                << std::endl;
    }
  }

  if (h == INVALID_HANDLE_VALUE) {
    std::cout << "ConnectToPipe:got invalid handle, finally returning"
              << std::endl;
    return GetLastError();
  }

  // Change to message read mode to match server side.  Max connection count
  // and timeout must be null if client and server are on the same machine.
  DWORD mode = PIPE_READMODE_MESSAGE;
  if (!SetNamedPipeHandleState(h, &mode,
                               /*maxCollectionCount=*/nullptr,
                               /*connectionTimeout=*/nullptr)) {
    DWORD err = GetLastError();
    std::cout << "ConnectToPipe: failed to set handle state err=" << err
              << std::endl;
    CloseHandle(h);
    return err;
  }

  *handle = h;
  return ERROR_SUCCESS;
}

// static
std::vector<char> ClientWin::ReadNextMessageFromPipe(HANDLE pipe) {
  DWORD err = ERROR_SUCCESS;
  std::vector<char> buffer(kBufferSize);
  char* p = buffer.data();
  int final_size = 0;
  while (true) {
    DWORD read;
    if (ReadFile(pipe, p, kBufferSize, &read, nullptr)) {
      final_size += read;
      break;
    } else {
      err = GetLastError();
      if (err != ERROR_MORE_DATA)
        break;

      // TODO - bug fix
      final_size += static_cast<int>(read);
      // TODO - possible bug fix, always keep buffer kBufferSize bigger than we
      // need
      buffer.resize(final_size + kBufferSize);
      p = buffer.data() + buffer.size() - kBufferSize;
    }
  }
  buffer.resize(final_size);
  return buffer;
}

// static
bool ClientWin::WriteMessageToPipe(HANDLE pipe, const std::string& message) {
  if (message.empty())
    return false;
  DWORD written;
  return WriteFile(pipe, message.data(), message.size(), &written, nullptr);
}

void ClientWin::Shutdown() {
  if (hPipe_ != INVALID_HANDLE_VALUE) {
    // TODO: This trips the LateWriteObserver.  We could move this earlier
    // (before the LateWriteObserver is created) or just remove it, although
    // the later could mean an ACK message is not processed by the agent
    // in time.
    // FlushFileBuffers(hPipe_);
    CloseHandle(hPipe_);
    hPipe_ = INVALID_HANDLE_VALUE;
  }
}

}  // namespace sdk
}  // namespace content_analysis
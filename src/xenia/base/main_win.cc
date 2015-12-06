/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/main.h"

#include <fcntl.h>
#include <gflags/gflags.h>
#include <io.h>

#include "xenia/base/logging.h"
#include "xenia/base/platform_win.h"
#include "xenia/base/string.h"

namespace xe {

bool has_console_attached_ = true;

bool has_console_attached() { return has_console_attached_; }

void AttachConsole() {
  bool has_console = ::AttachConsole(ATTACH_PARENT_PROCESS) == TRUE;
  if (!has_console) {
    // We weren't launched from a console, so just return.
    // We could alloc our own console, but meh:
    // has_console = AllocConsole() == TRUE;
    has_console_attached_ = false;
    return;
  }
  has_console_attached_ = true;

  auto std_handle = (intptr_t)GetStdHandle(STD_OUTPUT_HANDLE);
  auto con_handle = _open_osfhandle(std_handle, _O_TEXT);
  auto fp = _fdopen(con_handle, "w");
  *stdout = *fp;
  setvbuf(stdout, nullptr, _IONBF, 0);

  std_handle = (intptr_t)GetStdHandle(STD_ERROR_HANDLE);
  con_handle = _open_osfhandle(std_handle, _O_TEXT);
  fp = _fdopen(con_handle, "w");
  *stderr = *fp;
  setvbuf(stderr, nullptr, _IONBF, 0);
}

int Main() {
  auto entry_info = xe::GetEntryInfo();

  // Convert command line to an argv-like format so we can share code/use
  // gflags.
  auto command_line = GetCommandLineW();
  int argc;
  wchar_t** argv = CommandLineToArgvW(command_line, &argc);
  if (!argv) {
    return 1;
  }

  google::SetUsageMessage(std::string("usage: ") +
                          xe::to_string(entry_info.usage));
  google::SetVersionString("1.0");

  // Convert all args to narrow, as gflags doesn't support wchar.
  int argca = argc;
  char** argva = reinterpret_cast<char**>(alloca(sizeof(char*) * argca));
  for (int n = 0; n < argca; n++) {
    size_t len = wcslen(argv[n]);
    argva[n] = reinterpret_cast<char*>(alloca(len + 1));
    wcstombs_s(nullptr, argva[n], len + 1, argv[n], _TRUNCATE);
  }

  // Parse flags; this may delete some of them.
  google::ParseCommandLineFlags(&argc, &argva, true);

  // Widen all remaining flags and convert to usable strings.
  std::vector<std::wstring> args;
  for (int n = 0; n < argc; n++) {
    args.push_back(xe::to_wstring(argva[n]));
  }

  // Setup COM on the main thread.
  // NOTE: this may fail if COM has already been initialized - that's OK.
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  // Initialize logging. Needs parsed FLAGS.
  xe::InitializeLogging(entry_info.name);

  // Call app-provided entry point.
  int result = entry_info.entry_point(args);

  google::ShutDownCommandLineFlags();
  LocalFree(argv);
  return result;
}

}  // namespace xe

// Used in console mode apps; automatically picked based on subsystem.
int main(int argc_ignored, char** argv_ignored) { return xe::Main(); }

// Used in windowed apps; automatically picked based on subsystem.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR command_line, int) {
  // Attach a console so we can write output to stdout. If the user hasn't
  // redirected output themselves it'll pop up a window.
  xe::AttachConsole();

  // Run normal entry point.
  return xe::Main();
}

#if defined _M_IX86
#pragma comment( \
    linker,      \
    "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")  // NOLINT(whitespace/line_length)
#elif defined _M_IA64
#pragma comment( \
    linker,      \
    "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='ia64' publicKeyToken='6595b64144ccf1df' language='*'\"")  // NOLINT(whitespace/line_length)
#elif defined _M_X64
#pragma comment( \
    linker,      \
    "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")  // NOLINT(whitespace/line_length)
#else
#pragma comment( \
    linker,      \
    "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")  // NOLINT(whitespace/line_length)
#endif
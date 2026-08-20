#include "check_git_status/app.hpp"

#include <atomic>
#include <csignal>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::atomic_bool interrupted = false;

void handle_interrupt(int) {
  interrupted.store(true, std::memory_order_relaxed);
}

#ifdef _WIN32
std::string wide_to_utf8(const std::wstring &value) {
  if (value.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                       static_cast<int>(value.size()), nullptr,
                                       0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      result.data(), size, nullptr, nullptr);
  return result;
}
#endif

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t *argv[]) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
  for (int index = 1; index < argc; ++index) {
    arguments.push_back(wide_to_utf8(argv[index]));
  }

  std::signal(SIGINT, handle_interrupt);
  return check_git_status::run_cli(arguments, std::cout, std::cerr,
                                   interrupted);
}
#else
int main(int argc, char *argv[]) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }

  std::signal(SIGINT, handle_interrupt);
  return check_git_status::run_cli(arguments, std::cout, std::cerr,
                                   interrupted);
}
#endif

#include <cstdio>
#include <string_view>

#include "clang/Basic/Version.h"

#ifndef VANE_VERSION
#define VANE_VERSION "dev"
#endif

namespace {

int PrintVersion() {
  std::printf("vane %s\n", VANE_VERSION);
  std::printf("clang %s\n", clang::getClangFullVersion().c_str());
  return 0;
}

int PrintUsage() {
  std::fputs(
      "usage: vane <command> [args...]\n"
      "\n"
      "commands:\n"
      "  --version    print vane and linked clang versions\n"
      "\n"
      "(more commands land as the fork-checkpoint executor is wired up.)\n",
      stderr);
  return 1;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) return PrintUsage();
  std::string_view cmd = argv[1];
  if (cmd == "--version" || cmd == "-v") return PrintVersion();
  return PrintUsage();
}

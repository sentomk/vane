// Spike 003: same fork-after-prefix-parse mechanism as spike 002, but with a
// REALISTIC large prefix (the full C++ standard library via bits/stdc++.h)
// instead of a two-line toy header. Measures parse time, memory before/after
// fork, and per-branch continuation time to see whether the mechanism still
// holds up (and is still worth it) at real-world AST scale.
#include "clang/Interpreter/Interpreter.h"
#include "clang/Frontend/CompilerInstance.h"

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace llvm;
using Clock = std::chrono::steady_clock;

static void die(const char *where, llvm::Error err) {
  std::cerr << where << ": " << llvm::toString(std::move(err)) << "\n";
  std::exit(1);
}

template <typename T>
static T take(llvm::Expected<T> exp, const char *where) {
  if (!exp) die(where, exp.takeError());
  return std::move(*exp);
}

static clang::PartialTranslationUnit &parseOrDie(clang::Interpreter &interp,
                                                  llvm::StringRef code,
                                                  const char *where) {
  auto exp = interp.Parse(code);
  if (!exp) die(where, exp.takeError());
  return *exp;
}

static long rssKB() {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_maxrss; // KB on Linux
}

static double elapsedMs(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

static bool emitObjectFile(llvm::Module &module, const std::string &path) {
  auto targetTriple = llvm::sys::getDefaultTargetTriple();
  module.setTargetTriple(targetTriple);

  std::string err;
  const llvm::Target *target = llvm::TargetRegistry::lookupTarget(targetTriple, err);
  if (!target) {
    std::cerr << "lookupTarget failed: " << err << "\n";
    return false;
  }

  llvm::TargetOptions opts;
  auto targetMachine = std::unique_ptr<llvm::TargetMachine>(
      target->createTargetMachine(targetTriple, "generic", "", opts, std::optional<Reloc::Model>(Reloc::PIC_)));
  module.setDataLayout(targetMachine->createDataLayout());

  std::error_code ec;
  llvm::raw_fd_ostream dest(path, ec, llvm::sys::fs::OF_None);
  if (ec) {
    std::cerr << "could not open " << path << ": " << ec.message() << "\n";
    return false;
  }

  llvm::legacy::PassManager pm;
  if (targetMachine->addPassesToEmitFile(pm, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
    std::cerr << "cannot emit object file for this module\n";
    return false;
  }
  pm.run(module);
  dest.flush();
  return true;
}

static std::unique_ptr<clang::Interpreter> makeInterpreter() {
  clang::IncrementalCompilerBuilder builder;
  std::vector<const char *> args = {"-std=c++17"};
  builder.SetCompilerArgs(args);
  auto ci = take(builder.CreateCpp(), "CreateCpp");
  return take(clang::Interpreter::create(std::move(ci)), "Interpreter::create");
}

int main(int argc, char **argv) {
  int numBranches = argc > 1 ? std::atoi(argv[1]) : 6;

  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  auto interp = makeInterpreter();

  // Realistic large prefix: full C++ standard library, exactly like a
  // project-wide common header would look in practice.
  static const char *kPrefix = R"cpp(
    #include <bits/stdc++.h>
    extern "C" int shared_helper(int x) {
      std::vector<int> v = {x, x + 1, x + 2};
      std::sort(v.begin(), v.end());
      std::map<int, std::string> m;
      m[x] = std::to_string(x);
      return v.front() + static_cast<int>(m.size());
    }
  )cpp";

  std::cout << "rss_before_prefix_kb=" << rssKB() << "\n";
  auto t0 = Clock::now();
  auto &prefixPTU = parseOrDie(*interp, kPrefix, "parse prefix");
  double parseMs = elapsedMs(t0);
  if (!prefixPTU.TheModule) {
    std::cerr << "prefix PTU has no module\n";
    return 1;
  }
  std::cout << "prefix_parse_ms=" << parseMs << "\n";
  std::cout << "rss_after_prefix_kb=" << rssKB() << "\n";
  std::cout << "prefix_ir_top_level_values=" << prefixPTU.TheModule->size() << "\n";

  auto t1 = Clock::now();
  if (!emitObjectFile(*prefixPTU.TheModule, "/tmp/spike003_prefix.o")) {
    std::cerr << "failed to emit prefix.o\n";
    return 1;
  }
  std::cout << "prefix_codegen_ms=" << elapsedMs(t1) << "\n";
  std::cout.flush();

  std::vector<pid_t> pids;
  auto forkStart = Clock::now();
  for (int i = 0; i < numBranches; ++i) {
    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      return 1;
    }
    if (pid == 0) {
      auto childStart = Clock::now();
      std::string code = "extern \"C\" int branch_" + std::to_string(i) +
                          "() { return shared_helper(" + std::to_string(i) + ") + " +
                          std::to_string(i) + "; }";
      auto &ptu = parseOrDie(*interp, code, "child parse");
      double childParseMs = elapsedMs(childStart);
      std::string outPath = "/tmp/spike003_child_" + std::to_string(i) + ".o";
      auto codegenStart = Clock::now();
      bool ok = ptu.TheModule && emitObjectFile(*ptu.TheModule, outPath);
      double codegenMs = elapsedMs(codegenStart);
      std::cout << "child[" << i << "] parse_ms=" << childParseMs
                << " codegen_ms=" << codegenMs
                << " rss_kb=" << rssKB()
                << " ok=" << ok << "\n";
      std::cout.flush();
      _exit(ok ? 0 : 2);
    }
    pids.push_back(pid);
  }
  double forkLoopMs = elapsedMs(forkStart);

  int rc = 0;
  for (pid_t pid : pids) {
    int status = 0;
    waitpid(pid, &status, 0);
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) rc = 1;
  }
  std::cout << "all_forks_issued_ms=" << forkLoopMs << "\n";
  std::cout << "rss_after_all_children_kb=" << rssKB() << "\n";
  std::cout << "overall_rc=" << rc << "\n";
  return rc;
}

// spike004_vane.cpp — vane fork-checkpoint vs PCH, on the pch-planner fixture.
//
// Parse a synthetic prefix (common.h + heavy_a/b/c.h) ONCE via
// clang::Interpreter, then fork one child per TU. Each child parses only that
// TU's unique body (the part after its #includes) and emits a real .o via the
// standard LLVM codegen pipeline. Mirrors spikes 002/003 but with:
//   - a realistic project prefix (the fixture's shared headers)
//   - N=42 branches (the full fixture)
//   - a memory-bounded fork wave instead of forking all 42 at once
//   - timing broken into prefix-parse vs fork-wave so we can compare against
//     the PCH numbers from spike004_pch.sh
//
// The prefix is the UNION of the shared headers, matching the PCH baseline
// (which precompiles the same union) so the only variable is the sharing
// mechanism: PCH deserialize-per-TU vs fork COW.

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
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace llvm;
using Clock = std::chrono::steady_clock;

static void die(const char *where, llvm::Error err) {
  std::cerr << where << ": " << llvm::toString(std::move(err)) << "\n";
  std::exit(1);
}
template <typename T> static T take(llvm::Expected<T> e, const char *w) {
  if (!e) die(w, e.takeError());
  return std::move(*e);
}
static double ms(Clock::time_point s) {
  return std::chrono::duration<double, std::milli>(Clock::now() - s).count();
}
static long rssKB() {
  struct rusage ru; getrusage(RUSAGE_SELF, &ru); return ru.ru_maxrss;
}

static bool emitObj(llvm::Module &m, const std::string &path) {
  auto triple = llvm::sys::getDefaultTargetTriple();
  m.setTargetTriple(triple);
  std::string err;
  const llvm::Target *t = llvm::TargetRegistry::lookupTarget(triple, err);
  if (!t) { std::cerr << "lookupTarget: " << err << "\n"; return false; }
  llvm::TargetOptions opts;
  auto tm = std::unique_ptr<llvm::TargetMachine>(
      t->createTargetMachine(triple, "generic", "", opts,
                             std::optional<Reloc::Model>(Reloc::PIC_)));
  m.setDataLayout(tm->createDataLayout());
  std::error_code ec;
  llvm::raw_fd_ostream dst(path, ec, llvm::sys::fs::OF_None);
  if (ec) { std::cerr << "open " << path << ": " << ec.message() << "\n"; return false; }
  llvm::legacy::PassManager pm;
  if (tm->addPassesToEmitFile(pm, dst, nullptr, llvm::CodeGenFileType::ObjectFile)) {
    std::cerr << "cannot emit object\n"; return false;
  }
  pm.run(m); dst.flush();
  return true;
}

static std::unique_ptr<clang::Interpreter> makeInterp(const std::string &incDir) {
  clang::IncrementalCompilerBuilder b;
  std::vector<const char *> args = {"-std=c++20", "-I", incDir.c_str(), "-O0"};
  b.SetCompilerArgs(args);
  auto ci = take(b.CreateCpp(), "CreateCpp");
  return take(clang::Interpreter::create(std::move(ci)), "Interpreter::create");
}

// Read a TU file and strip its leading #include lines — those are already in
// the prefix. Returns the remaining body (the unique part).
static std::string tuBody(const std::string &path) {
  std::ifstream in(path);
  std::ostringstream body;
  std::string line;
  while (std::getline(in, line)) {
    // Skip #include lines; keep everything else. Bodies here have includes
    // only at the very top, so this is safe for the fixture.
    std::string trimmed = line;
    size_t p = trimmed.find_first_not_of(" \t");
    if (p != std::string::npos && trimmed.compare(p, 8, "#include") == 0)
      continue;
    body << line << "\n";
  }
  return body.str();
}

int main(int argc, char **argv) {
  if (argc < 4) {
    std::cerr << "usage: " << argv[0] << " <fixture_dir> <out_dir> <pool> [tu...]\n";
    return 1;
  }
  std::string fixtureDir = argv[1];
  std::string outDir = argv[2];
  int pool = std::atoi(argv[3]);
  if (pool <= 0) pool = 8;

  std::vector<std::string> tus;
  if (argc > 4) {
    for (int i = 4; i < argc; ++i) tus.push_back(argv[i]);
  }
  if (tus.empty()) { std::cerr << "no TUs given\n"; return 1; }

  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  auto interp = makeInterp(fixtureDir);

  // Synthetic prefix: union of shared headers, matching the PCH baseline.
  static const char *kPrefix = R"cpp(
    #include "common.h"
    #include "heavy_a.h"
    #include "heavy_b.h"
    #include "heavy_c.h"
  )cpp";

  std::cout << "rss_before_prefix_kb=" << rssKB() << "\n";
  auto t0 = Clock::now();
  auto pe = interp->Parse(kPrefix);
  if (!pe) die("parse prefix", pe.takeError());
  double prefixParseMs = ms(t0);
  std::cout << "prefix_parse_ms=" << prefixParseMs << "\n";
  std::cout << "rss_after_prefix_kb=" << rssKB() << "\n";
  std::cout.flush();

  // Pre-read all TU bodies before forking (I/O in parent, shared via COW).
  std::vector<std::string> bodies(tus.size());
  for (size_t i = 0; i < tus.size(); ++i) bodies[i] = tuBody(tus[i]);

  auto waveStart = Clock::now();
  size_t next = 0, running = 0;
  int rc = 0;
  std::vector<pid_t> live;
  auto reap = [&]() {
    int status = 0;
    pid_t p = wait(&status);
    if (p > 0) {
      running--;
      if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) rc = 1;
    }
  };

  while (next < tus.size() || running > 0) {
    while (running < (size_t)pool && next < tus.size()) {
      size_t idx = next++;
      pid_t pid = fork();
      if (pid < 0) { perror("fork"); return 1; }
      if (pid == 0) {
        // Child: wrap the body in a uniquely-named function-free TU. The body
        // already defines fn_X(); just parse it as-is.
        auto ce = interp->Parse(bodies[idx]);
        if (!ce) {
          std::cerr << "child parse " << tus[idx] << ": "
                    << llvm::toString(ce.takeError()) << "\n";
          _exit(2);
        }
        clang::PartialTranslationUnit &ptu = *ce;
        if (!ptu.TheModule) { std::cerr << "no module\n"; _exit(2); }
        std::string base = tus[idx];
        size_t slash = base.find_last_of('/');
        if (slash != std::string::npos) base = base.substr(slash + 1);
        std::string out = outDir + "/" + base + ".o";
        _exit(emitObj(*ptu.TheModule, out) ? 0 : 2);
      }
      live.push_back(pid);
      running++;
    }
    if (running > 0) reap();
  }
  double waveMs = ms(waveStart);

  std::cout << "fork_wave_ms=" << waveMs << "\n";
  std::cout << "pool=" << pool << "\n";
  std::cout << "tu_count=" << tus.size() << "\n";
  std::cout << "rss_after_all_kb=" << rssKB() << "\n";
  std::cout << "total_ms=" << (prefixParseMs + waveMs) << "\n";
  std::cout << "overall_rc=" << rc << "\n";
  return rc;
}

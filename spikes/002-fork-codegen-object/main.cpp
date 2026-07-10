// Spike 002: parse a shared prefix ONCE via clang::Interpreter, fork, and in
// each child continue parsing TU-specific code. Instead of JIT-executing
// (spike 001), emit REAL object code from the incremental PartialTranslationUnit
// module using LLVM's standard codegen pipeline, so the result can be linked
// and run like a normal .o file.
#include "clang/Interpreter/Interpreter.h"
#include "clang/Frontend/CompilerInstance.h"

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace llvm;

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

// Emit a real object file (.o) from an in-memory llvm::Module using the
// standard target codegen pipeline (same mechanism `llc` uses), independent
// of the Interpreter's own JIT (ORC) execution path.
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
    std::cerr << "target machine cannot emit object file for this module\n";
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

int main() {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  auto interp = makeInterpreter();

  // Shared prefix: a template class + a non-template helper. Parsed and
  // codegen'd exactly ONCE in the parent, before forking.
  static const char *kPrefix = R"cpp(
    template <typename T>
    struct Box {
      T value;
      T get() const { return value; }
    };

    extern "C" int shared_helper(int x) {
      Box<int> b{x};
      return b.get() * 2;
    }
  )cpp";

  auto &prefixPTU = parseOrDie(*interp, kPrefix, "parse prefix");
  if (!prefixPTU.TheModule) {
    std::cerr << "prefix PTU has no module (Parse() did not produce IR)\n";
    return 1;
  }
  std::cout << "prefix module has " << prefixPTU.TheModule->size() << " top-level IR values\n";

  if (!emitObjectFile(*prefixPTU.TheModule, "/tmp/spike002_prefix.o")) {
    std::cerr << "failed to emit prefix.o\n";
    return 1;
  }
  std::cout << "parent: emitted /tmp/spike002_prefix.o\n";
  std::cout.flush();

  struct Branch { const char *name; const char *code; const char *outPath; };
  Branch branches[] = {
      {"child-A",
       R"cpp(extern "C" int branch_a() { return shared_helper(10) + 1; })cpp",
       "/tmp/spike002_child_a.o"},
      {"child-B",
       R"cpp(extern "C" int branch_b() { return shared_helper(20) + 2; })cpp",
       "/tmp/spike002_child_b.o"},
  };

  pid_t pids[2];
  for (int i = 0; i < 2; ++i) {
    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      return 1;
    }
    if (pid == 0) {
      auto &ptu = parseOrDie(*interp, branches[i].code, branches[i].name);
      if (!ptu.TheModule) {
        std::cerr << branches[i].name << ": no module produced\n";
        _exit(2);
      }
      if (!emitObjectFile(*ptu.TheModule, branches[i].outPath)) {
        std::cerr << branches[i].name << ": emitObjectFile failed\n";
        _exit(2);
      }
      std::cout << branches[i].name << ": emitted " << branches[i].outPath << "\n";
      _exit(0);
    }
    pids[i] = pid;
  }

  int rc = 0;
  for (int i = 0; i < 2; ++i) {
    int status = 0;
    waitpid(pids[i], &status, 0);
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::cout << branches[i].name << " exit_ok=" << ok << "\n";
    if (!ok) rc = 1;
  }
  return rc;
}

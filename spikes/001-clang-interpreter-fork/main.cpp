#include "clang/Interpreter/Interpreter.h"
#include "clang/Frontend/CompilerInstance.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

static void fail_if_error(llvm::Error err, const char *where) {
  if (!err) return;
  std::cerr << where << ": " << llvm::toString(std::move(err)) << "\n";
  std::exit(1);
}

template <typename T>
static T take_or_die(llvm::Expected<T> exp, const char *where) {
  if (!exp) {
    std::cerr << where << ": " << llvm::toString(exp.takeError()) << "\n";
    std::exit(1);
  }
  return std::move(*exp);
}

static clang::PartialTranslationUnit &parse_or_die(clang::Interpreter &interp,
                                                    llvm::StringRef code,
                                                    const char *where) {
  auto exp = interp.Parse(code);
  if (!exp) {
    std::cerr << where << ": " << llvm::toString(exp.takeError()) << "\n";
    std::exit(1);
  }
  return *exp;
}

static void child_branch(clang::Interpreter &interp, const char *name, int delta) {
  std::string code = std::string("extern \"C\" int branch_value() { return shared_helper() + ") + std::to_string(delta) + "; }";
  auto &ptu = parse_or_die(interp, code, "child parse branch");
  fail_if_error(interp.Execute(ptu), "child execute branch");

  auto addr = take_or_die(interp.getSymbolAddressFromLinkerName("branch_value"), "lookup branch_value");
  auto fn = addr.toPtr<int (*)()>();
  int value = fn();
  std::cout << name << " value=" << value << "\n";
  std::cout.flush();
  _exit(value == 42 + delta ? 0 : 2);
}

int main() {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  clang::IncrementalCompilerBuilder builder;
  std::vector<const char *> args = {"-std=c++17"};
  builder.SetCompilerArgs(args);

  auto ci = take_or_die(builder.CreateCpp(), "CreateCpp");
  auto interp = take_or_die(clang::Interpreter::create(std::move(ci)), "Interpreter::create");

  auto &prefix = parse_or_die(*interp, "extern \"C\" int shared_helper() { return 42; }", "parse prefix");
  fail_if_error(interp->Execute(prefix), "execute prefix");
  std::cout << "parent prefix parsed and executed\n";
  std::cout.flush();

  pid_t a = fork();
  if (a == 0) child_branch(*interp, "child-A", 1);
  if (a < 0) {
    perror("fork A");
    return 1;
  }

  pid_t b = fork();
  if (b == 0) child_branch(*interp, "child-B", 2);
  if (b < 0) {
    perror("fork B");
    return 1;
  }

  int status_a = 0;
  int status_b = 0;
  waitpid(a, &status_a, 0);
  waitpid(b, &status_b, 0);
  std::cout << "child-A status=" << status_a << " child-B status=" << status_b << "\n";
  return WIFEXITED(status_a) && WEXITSTATUS(status_a) == 0 &&
         WIFEXITED(status_b) && WEXITSTATUS(status_b) == 0
             ? 0
             : 1;
}

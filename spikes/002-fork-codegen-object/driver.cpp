#include <iostream>

extern "C" int shared_helper(int);
extern "C" int branch_a();
extern "C" int branch_b();

int main() {
  std::cout << shared_helper(7) << "\n";
  std::cout << branch_a() << "\n";
  std::cout << branch_b() << "\n";
  return 0;
}

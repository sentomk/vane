#include <iostream>
extern "C" int shared_helper(int);
extern "C" int branch_0();
extern "C" int branch_1();
extern "C" int branch_2();
extern "C" int branch_3();
extern "C" int branch_4();
extern "C" int branch_5();

int main() {
  std::cout << branch_0() << " " << branch_1() << " " << branch_2() << " "
            << branch_3() << " " << branch_4() << " " << branch_5() << "\n";
  return 0;
}

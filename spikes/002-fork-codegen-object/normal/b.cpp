extern "C" int shared_helper(int);
extern "C" int branch_b() { return shared_helper(20) + 2; }

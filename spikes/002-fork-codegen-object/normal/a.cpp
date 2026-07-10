extern "C" int shared_helper(int);
extern "C" int branch_a() { return shared_helper(10) + 1; }

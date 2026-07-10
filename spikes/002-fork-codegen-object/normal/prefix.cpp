template <typename T>
struct Box {
  T value;
  T get() const { return value; }
};

extern "C" int shared_helper(int x) {
  Box<int> b{x};
  return b.get() * 2;
}

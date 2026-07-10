#pragma once
#include <bits/stdc++.h>
extern "C" inline int shared_helper(int x) {
  std::vector<int> v = {x, x + 1, x + 2};
  std::sort(v.begin(), v.end());
  std::map<int, std::string> m;
  m[x] = std::to_string(x);
  return v.front() + static_cast<int>(m.size());
}

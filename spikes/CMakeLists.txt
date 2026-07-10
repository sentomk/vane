# Spikes are validation experiments preserved for their real measurement
# data. Each spike is a self-contained executable named vane_spike_NNN.
# They are not part of the vane CLI surface — treat them as fixed
# evidence that the fork-checkpoint mechanism works at the tested scale.

function(vane_add_spike name src)
  add_executable(${name} ${src})
  vane_apply_clang_targets(${name})
  target_link_libraries(${name} PRIVATE clang-cpp LLVM)
endfunction()

vane_add_spike(vane_spike_001 001-clang-interpreter-fork/main.cpp)
vane_add_spike(vane_spike_002 002-fork-codegen-object/main.cpp)
vane_add_spike(vane_spike_003 003-fork-large-prefix/main.cpp)

// RUN: %clang_cc1 -E -triple x86_64-unknown-linux-gnu %s -o - \
// RUN:   | FileCheck %s --check-prefix=NOBUILTIN
// RUN: %clang_cc1 -E -triple amdgpu12.00-amd-amdhsa %s -o - \
// RUN:   | FileCheck %s --check-prefix=BUILTIN
// RUN: %clang_cc1 -E -triple amdgpu12.00-amd-amdhsa \
// RUN:   -target-feature -debugging-enabled-query %s -o - 2>&1 \
// RUN:   | FileCheck %s --check-prefixes=READONLY,BUILTIN
// RUN: %clang_cc1 -E -x hip -triple x86_64-unknown-linux-gnu \
// RUN:   -aux-triple amdgpu12.00-amd-amdhsa %s -o - \
// RUN:   | FileCheck %s --check-prefix=NOBUILTIN

// READONLY: warning: feature flag '-debugging-enabled-query' is ignored since the feature is read only
#if __has_builtin(__builtin_is_debugging_enabled)
// BUILTIN: has_builtin_yes
has_builtin_yes
#else
// NOBUILTIN: has_builtin_no
has_builtin_no
#endif

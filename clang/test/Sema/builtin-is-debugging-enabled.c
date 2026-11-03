// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fsyntax-only -verify=unsupported %s
// RUN: %clang_cc1 -triple amdgpu12.00-amd-amdhsa -fsyntax-only -verify=supported %s

// supported-no-diagnostics

_Bool f(void) {
  return __builtin_is_debugging_enabled(); // unsupported-error {{builtin is not supported on this target}}
}

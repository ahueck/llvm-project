// RUN: %clang_cc1 -triple amdgpu12.00-amd-amdhsa -emit-llvm -o - %s | FileCheck %s

// CHECK-LABEL: define {{.*}}zeroext i1 @query(
// CHECK: [[R:%.*]] = call i1 @llvm.is.debugging.enabled()
// CHECK: ret i1 [[R]]
_Bool query(void) {
  return __builtin_is_debugging_enabled();
}

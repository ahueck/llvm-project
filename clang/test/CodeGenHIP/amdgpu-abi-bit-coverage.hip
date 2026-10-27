// REQUIRES: amdgpu-registered-target
// RUN: %clang_cc1 -triple amdgcn-amd-amdhsa -x hip -fcuda-is-device -emit-llvm -disable-llvm-passes -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple amdgcn-amd-amdhsa -x hip -fcuda-is-device -O2 -emit-llvm -o - %s | FileCheck %s --check-prefix=OPT

#define __device__ __attribute__((device))

typedef bool B9 __attribute__((ext_vector_type(9)));
union U {
  B9 bits;
  unsigned short active;
};
struct S {
  U u;
  int x, y;
};

// The i9 storage member must not discard bits 9-15 of the active member.
// CHECK-LABEL: define dso_local [3 x i32] @make(
// OPT-LABEL: define dso_local [3 x i32] @make(
// OPT: [[VALUE:%.*]] = zext i16 %x to i32
// OPT: insertvalue [3 x i32] poison, i32 [[VALUE]], 0
// OPT: ret [3 x i32]
extern "C" __device__ __attribute__((noinline)) S make(unsigned short x) {
  S s = {};
  s.u.active = x;
  return s;
}

// CHECK-LABEL: define dso_local {{.*}}i16 @read([3 x i32]
// OPT-LABEL: define dso_local {{.*}}i16 @read([3 x i32]
// OPT: [[LANE:%.*]] = extractvalue [3 x i32] %{{.*}}, 0
// OPT: [[VALUE:%.*]] = trunc i32 [[LANE]] to i16
// OPT: ret i16 [[VALUE]]
extern "C" __device__ __attribute__((noinline)) unsigned short read(S s) {
  return s.u.active;
}

// CHECK-LABEL: define dso_local {{.*}}i16 @round_trip(
// CHECK: call [3 x i32] @make(
// CHECK: call {{.*}}i16 @read([3 x i32]
extern "C" __device__ unsigned short round_trip(unsigned short x) {
  return read(make(x));
}

// Array elements advance by their allocation size, not their value bit width.
struct Array {
  int tag;
  U u[3];
};
// CHECK-LABEL: define dso_local [3 x i32] @array([3 x i32]
extern "C" __device__ Array array(Array s) { return s; }

// Partial-byte padding alone must not require a carrier.
struct Control {
  B9 bits;
  int x, y;
};
// CHECK-LABEL: define dso_local %struct.Control @control(i9
extern "C" __device__ Control control(Control s) { return s; }

union SafeUnion {
  B9 bits;
  struct {
    unsigned short value : 9;
  } field;
};
struct BitFieldControl {
  SafeUnion u;
  int x, y;
};
// CHECK-LABEL: define dso_local %struct.BitFieldControl @bitfield_control(%union.SafeUnion
extern "C" __device__ BitFieldControl bitfield_control(BitFieldControl s) {
  return s;
}

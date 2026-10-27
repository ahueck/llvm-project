// REQUIRES: amdgpu-registered-target
// RUN: %clang_cc1 -triple amdgcn-amd-amdhsa -emit-llvm -disable-llvm-passes -o - %s | FileCheck %s

struct P {
  unsigned short a;
  int b;
  char c;
};
union U {
  unsigned active;
  struct P layout;
};

struct Ptr {
  __amdgpu_buffer_rsrc_t p;
};
struct AtomicRecord {
  union U u;
  _Atomic(struct Ptr) p;
};

// An unsafe pointer inside an atomic record prevents an integer carrier.
// CHECK-LABEL: define dso_local void @atomic_record(
// CHECK-SAME: ptr addrspace(5) {{.*}}sret(%struct.AtomicRecord)
// CHECK-SAME: ptr addrspace(5) {{.*}}byref(%struct.AtomicRecord)
struct AtomicRecord atomic_record(struct AtomicRecord x) { return x; }

union PtrUnion {
  unsigned __int128 bits;
  __amdgpu_buffer_rsrc_t p;
};
struct AtomicUnion {
  union U u;
  _Atomic(union PtrUnion) p;
};

// Pointer safety must include alternatives absent from the lowered IR type.
// CHECK-LABEL: define dso_local void @atomic_union(
// CHECK-SAME: ptr addrspace(5) {{.*}}sret(%struct.AtomicUnion)
// CHECK-SAME: ptr addrspace(5) {{.*}}byref(%struct.AtomicUnion)
struct AtomicUnion atomic_union(struct AtomicUnion x) { return x; }

struct PtrArray {
  __amdgpu_buffer_rsrc_t p[2];
};
struct AtomicArray {
  union U u;
  _Atomic(struct PtrArray) p;
};
// CHECK-LABEL: define dso_local void @atomic_array(
// CHECK-SAME: ptr addrspace(5) {{.*}}sret(%struct.AtomicArray)
// CHECK-SAME: ptr addrspace(5) {{.*}}byref(%struct.AtomicArray)
struct AtomicArray atomic_array(struct AtomicArray x) { return x; }

struct Control {
  _Atomic(struct Ptr) p;
  int tag;
};
// A fully covered aggregate keeps its existing pointer representation.
// CHECK-LABEL: define dso_local %struct.Control @control(%struct.Ptr
struct Control control(struct Control x) { return x; }

struct IntegralPtr {
  void *p;
};
struct AtomicIntegral {
  union U u;
  _Atomic(struct IntegralPtr) p;
};
// An ordinary pointer inside an atomic record still permits a carrier.
// CHECK-LABEL: define dso_local [6 x i32] @atomic_integral([6 x i32]
struct AtomicIntegral atomic_integral(struct AtomicIntegral x) { return x; }

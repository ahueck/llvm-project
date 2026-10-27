// REQUIRES: amdgpu-registered-target
// RUN: %clang_cc1 -triple amdgcn-amd-amdhsa -x hip -fcuda-is-device -emit-llvm -disable-llvm-passes -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple amdgcn-amd-amdhsa -x hip -fcuda-is-device -O2 -emit-llvm -o - %s | FileCheck %s --check-prefix=OPT

// Union members that overlap holes of the member chosen as the IR storage type
// must not be dropped when the aggregate is passed or returned by value.

#define __device__ __attribute__((device))

struct P { unsigned short a; int b; signed char c; };
struct Q { unsigned a; unsigned b; unsigned char c; };
union T { unsigned active; P layout; };
union TR { P layout; unsigned active; };
struct S1 { T u; int b; };
struct S2 { S1 s; };
struct S3 { T arr[2]; };
struct TBase { union { unsigned active; P layout; } u; };
struct S7 : TBase { int z; };
union alignas(16) A16 { unsigned active; P layout; };
struct __attribute__((packed)) Packed13 { char x; T u; };
union TailPad { struct { unsigned x, y; unsigned short z; } s; unsigned w[3]; };

union N1 { unsigned active; Q layout; };
union N2 { unsigned char a[9]; unsigned char b[9]; };
union N3 { unsigned w[3]; Q layout; };
union N4 { P layout; };
struct BitP { unsigned a : 1; int b; char c; };
union BU { BitP layout; unsigned char active; };
union EU { struct {} e; P p; };
struct V3 { float __attribute__((ext_vector_type(3))) v; int t; };
struct RS { __amdgpu_buffer_rsrc_t r; int t; };
typedef bool B8 __attribute__((ext_vector_type(8)));
struct BV { int a, b; B8 v; };

struct alignas(256) Outer { T u; unsigned tag; };
struct NIP { T u; __amdgpu_buffer_rsrc_t r; };

#define GEN(X)                                                                 \
  extern "C" __device__ X ret_##X(unsigned x) { X r = {}; return r; }          \
  extern "C" __device__ unsigned arg_##X(X v) { return *(unsigned *)&v; }      \
  extern "C" __device__ unsigned call_##X(X *p) {                              \
    return arg_##X(ret_##X(1)) + arg_##X(*p);                                  \
  }

// Carriers: the lowered type is [N x i32] covering every byte.
// CHECK-LABEL: define dso_local [3 x i32] @ret_T(
// CHECK-LABEL: define dso_local i32 @arg_T([3 x i32]
// CHECK-LABEL: define dso_local i32 @call_T(
// CHECK: call [3 x i32] @ret_T(
// CHECK: call i32 @arg_T([3 x i32]
GEN(T)

// CHECK-LABEL: define dso_local [3 x i32] @ret_TR(
// CHECK-LABEL: define dso_local i32 @arg_TR([3 x i32]
// CHECK-LABEL: define dso_local i32 @call_TR(
// CHECK: call [3 x i32] @ret_TR(
// CHECK: call i32 @arg_TR([3 x i32]
GEN(TR)

// CHECK-LABEL: define dso_local [3 x i32] @ret_TailPad(
// CHECK-LABEL: define dso_local i32 @arg_TailPad([3 x i32]
// CHECK-LABEL: define dso_local i32 @call_TailPad(
// CHECK: call [3 x i32] @ret_TailPad(
// CHECK: call i32 @arg_TailPad([3 x i32]
GEN(TailPad)

// CHECK-LABEL: define dso_local [4 x i32] @ret_S1(
// CHECK-LABEL: define dso_local i32 @arg_S1([4 x i32]
// CHECK-LABEL: define dso_local i32 @call_S1(
// CHECK: call [4 x i32] @ret_S1(
// CHECK: call i32 @arg_S1([4 x i32]
GEN(S1)

// CHECK-LABEL: define dso_local [4 x i32] @ret_S2(
// CHECK-LABEL: define dso_local i32 @arg_S2([4 x i32]
// CHECK-LABEL: define dso_local i32 @call_S2(
// CHECK: call [4 x i32] @ret_S2(
// CHECK: call i32 @arg_S2([4 x i32]
GEN(S2)

// CHECK-LABEL: define dso_local [6 x i32] @ret_S3(
// CHECK-LABEL: define dso_local i32 @arg_S3([6 x i32]
// CHECK-LABEL: define dso_local i32 @call_S3(
// CHECK: call [6 x i32] @ret_S3(
// CHECK: call i32 @arg_S3([6 x i32]
GEN(S3)

// CHECK-LABEL: define dso_local [4 x i32] @ret_A16(
// CHECK-LABEL: define dso_local i32 @arg_A16([4 x i32]
// CHECK-LABEL: define dso_local i32 @call_A16(
// CHECK: call [4 x i32] @ret_A16(
// CHECK: call i32 @arg_A16([4 x i32]
GEN(A16)

// CHECK-LABEL: define dso_local [4 x i32] @ret_Packed13(
// CHECK: call void @llvm.memcpy.p0.p5.i64(ptr align 4 {{.*}}, ptr addrspace(5) align 1 {{.*}}, i64 13, i1 false)
// CHECK-LABEL: define dso_local i32 @arg_Packed13([4 x i32]
// CHECK: call void @llvm.memcpy.p5.p0.i64(ptr addrspace(5) align 1 {{.*}}, ptr align 4 {{.*}}, i64 13, i1 false)
// CHECK-LABEL: define dso_local i32 @call_Packed13(
// CHECK: call [4 x i32] @ret_Packed13(
// CHECK: call void @llvm.memcpy.p0.p0.i64(ptr align 4 {{.*}}, ptr align 1 {{.*}}, i64 13, i1 false)
// CHECK: call i32 @arg_Packed13([4 x i32]
GEN(Packed13)

// Controls: the storage member already covers all data, the IR type is kept.
// CHECK-LABEL: define dso_local %union.N1 @ret_N1(
// CHECK-LABEL: define dso_local i32 @arg_N1(%struct.Q
// CHECK-LABEL: define dso_local i32 @call_N1(
// CHECK: call %union.N1 @ret_N1(
// CHECK: call i32 @arg_N1(%struct.Q
GEN(N1)

// CHECK-LABEL: define dso_local %union.N2 @ret_N2(
// CHECK-LABEL: define dso_local i32 @arg_N2([9 x i8]
// CHECK-LABEL: define dso_local i32 @call_N2(
// CHECK: call %union.N2 @ret_N2(
// CHECK: call i32 @arg_N2([9 x i8]
GEN(N2)

// CHECK-LABEL: define dso_local %union.N3 @ret_N3(
// CHECK-LABEL: define dso_local i32 @arg_N3([3 x i32]
// CHECK-LABEL: define dso_local i32 @call_N3(
// CHECK: call %union.N3 @ret_N3(
// CHECK: call i32 @arg_N3([3 x i32]
GEN(N3)

// CHECK-LABEL: define dso_local %union.N4 @ret_N4(
// CHECK-LABEL: define dso_local i32 @arg_N4(%struct.P
// CHECK-LABEL: define dso_local i32 @call_N4(
// CHECK: call %union.N4 @ret_N4(
// CHECK: call i32 @arg_N4(%struct.P
GEN(N4)

// CHECK-LABEL: define dso_local %union.BU @ret_BU(
// CHECK-LABEL: define dso_local i32 @arg_BU(%struct.BitP
// CHECK-LABEL: define dso_local i32 @call_BU(
// CHECK: call %union.BU @ret_BU(
// CHECK: call i32 @arg_BU(%struct.BitP
GEN(BU)

// CHECK-LABEL: define dso_local %union.EU @ret_EU(
// CHECK-LABEL: define dso_local i32 @arg_EU(%struct.P
// CHECK-LABEL: define dso_local i32 @call_EU(
// CHECK: call %union.EU @ret_EU(
// CHECK: call i32 @arg_EU(%struct.P
GEN(EU)

// CHECK-LABEL: define dso_local %struct.V3 @ret_V3(
// CHECK-LABEL: define dso_local i32 @arg_V3(<3 x float> %v.coerce0, i32
// CHECK-LABEL: define dso_local i32 @call_V3(
// CHECK: call %struct.V3 @ret_V3(
// CHECK: call i32 @arg_V3(<3 x float> %{{.*}}, i32 %
GEN(V3)

// CHECK-LABEL: define dso_local %struct.RS @ret_RS(
// CHECK-LABEL: define dso_local i32 @arg_RS(ptr addrspace(8) %v.coerce0, i32
// CHECK-LABEL: define dso_local i32 @call_RS(
// CHECK: call %struct.RS @ret_RS(
// CHECK: call i32 @arg_RS(ptr addrspace(8) %{{.*}}, i32 %
GEN(RS)

// CHECK-LABEL: define dso_local %struct.BV @ret_BV(
// CHECK-LABEL: define dso_local i32 @arg_BV(i32 %v.coerce0, i32 %v.coerce1, i8 %v.coerce2)
// CHECK-LABEL: define dso_local i32 @call_BV(
// CHECK: call %struct.BV @ret_BV(
// CHECK: call i32 @arg_BV(i32 %{{.*}}, i32 %{{.*}}, i8 %{{.*}})
GEN(BV)

// Derived class: the union lives in the base.
// CHECK-LABEL: define dso_local [4 x i32] @_Z6ret_S7j(
// CHECK-LABEL: define dso_local {{.*}}i32 @_Z6arg_S72S7([4 x i32]
// CHECK-LABEL: define dso_local {{.*}}i32 @_Z7call_S7P2S7(
// CHECK: call [4 x i32] @_Z6ret_S7j(
// CHECK: call {{.*}}i32 @_Z6arg_S72S7([4 x i32]
__device__ S7 ret_S7(unsigned x) { S7 r = {}; return r; }
__device__ unsigned arg_S7(S7 v) { return *(unsigned *)&v; }
__device__ unsigned call_S7(S7 *p) { return arg_S7(ret_S7(1)) + arg_S7(*p); }

// Indirect fallbacks: over-aligned, and non-integral pointer blocks the carrier.
// CHECK-LABEL: define dso_local void @ret_Outer(ptr addrspace(5) {{.*}}sret(%struct.Outer)
// CHECK-LABEL: define dso_local i32 @arg_Outer(ptr addrspace(5) {{.*}}byref(%struct.Outer)
// CHECK-LABEL: define dso_local i32 @call_Outer(
GEN(Outer)

// CHECK-LABEL: define dso_local void @ret_NIP(ptr addrspace(5) {{.*}}sret(%struct.NIP)
// CHECK-LABEL: define dso_local i32 @arg_NIP(ptr addrspace(5) {{.*}}byref(%struct.NIP)
// CHECK-LABEL: define dso_local i32 @call_NIP(
GEN(NIP)

// Register budget: three carriers use 12 of 16 lanes; S1 needs 5 estimated
// registers, so the remaining two arguments are passed byref.
// CHECK-LABEL: define dso_local i32 @five(
// CHECK-SAME: [4 x i32] %a.coerce, [4 x i32] %b.coerce, [4 x i32] %c.coerce,
// CHECK-SAME: ptr addrspace(5) {{.*}}byref(%struct.S1) align 4 %{{.*}}, ptr addrspace(5) {{.*}}byref(%struct.S1) align 4 %{{.*}})
extern "C" __device__ unsigned five(S1 a, S1 b, S1 c, S1 d, S1 e) {
  return a.b + b.b + c.b + d.b + e.b;
}

// The union member must survive the call: no truncation to the layout member.
// OPT-LABEL: define dso_local [4 x i32] @make_s1(i32 noundef %x)
// OPT-NOT: trunc i32 %x to i16
// OPT-NOT: and i32 {{.*}}65535
// OPT: insertvalue [4 x i32] poison, i32 %x, 0
// OPT: ret [4 x i32]
// OPT-LABEL: define dso_local {{.*}}i32 @use_s1(i32 noundef %x)
// OPT-NOT: trunc i32 %x to i16
// OPT-NOT: and i32 {{.*}}65535
// OPT: [[CALL:%.*]] = tail call [4 x i32] @make_s1(i32 noundef %x)
// OPT: [[V:%.*]] = extractvalue [4 x i32] [[CALL]], 0
// OPT: ret i32 [[V]]
extern "C" __device__ __attribute__((noinline)) S1 make_s1(unsigned x) {
  S1 s = {};
  s.u.active = x;
  return s;
}
extern "C" __device__ __attribute__((noinline)) unsigned use_s1(unsigned x) {
  S1 s = make_s1(x);
  return s.u.active;
}

// REQUIRES: amdgpu-registered-target
// RUN: %clang_cc1 -triple amdgcn-amd-amdhsa -emit-llvm -disable-llvm-passes -o - %s | FileCheck %s

// Parameters of transparent union type use the calling convention of the first
// member. A by-value first-class struct is split into the same registers as the
// flattened fields of fp_ref.

struct P { int a; long b; };
typedef union __attribute__((transparent_union)) { struct P p; unsigned words[4]; } U;

typedef float float3v __attribute__((ext_vector_type(3)));
struct V { float3v v; };
struct W4 { unsigned words[4]; };
typedef union __attribute__((transparent_union)) { struct V v; struct W4 w; } UV;

typedef union __attribute__((transparent_union)) { int *ip; float *fp; } PU;

// CHECK-LABEL: define dso_local i64 @fp_ref(i32 %p.coerce0, i64 %p.coerce1)
long fp_ref(struct P p) { return p.b; }

// CHECK-LABEL: define dso_local i64 @f(%struct.P %u.coerce)
long f(U u) { return u.p.b; }

// CHECK-LABEL: define dso_local i64 @call_via_ptr(i32 %p.coerce0, i64 %p.coerce1)
// CHECK: call i64 %{{.*}}(i32 %{{.*}}, i64 %{{.*}})
long call_via_ptr(struct P p) {
  long (*fn)(struct P) = f;
  return fn(p);
}

// CHECK-LABEL: define dso_local [4 x i32] @g(
U g(unsigned x) {
  U u = {{x, 0}};
  return u;
}

// CHECK-LABEL: define dso_local i32 @fv_ref(<3 x float> %v.coerce)
unsigned fv_ref(struct V v) { return v.v.x; }

// CHECK-LABEL: define dso_local i32 @fv(<3 x float> %u.coerce)
unsigned fv(UV u) { return u.w.words[3]; }

// CHECK-LABEL: define dso_local i32 @fpu(ptr %u.coerce)
unsigned fpu(PU u) { return *u.ip; }

// CHECK-LABEL: define dso_local i32 @callerpu(ptr noundef %p)
// CHECK: call i32 @fpu(ptr %{{.*}})
unsigned callerpu(int *p) { return fpu(p); }

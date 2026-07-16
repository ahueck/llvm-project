## Two concatenated uncompressed clang-offload-bundler fat binaries:
## each bundle has a zero-sized host entry plus one code object per GPU arch, and
## device payloads are aligned to the HIP code-object bundle alignment.

  .section .hip_fatbin,"",@progbits
.Lbundle0:
  .ascii "__CLANG_OFFLOAD_BUNDLE__"
  .quad 3

  .quad .Lbundle0_payload - .Lbundle0
  .quad 0
  .quad .Lhost0_id_end - .Lhost0_id
.Lhost0_id:
  .ascii "host-x86_64-unknown-linux-gnu-"
.Lhost0_id_end:

  .quad .Lgfx1031_0 - .Lbundle0
  .quad .Lgfx1031_0_end - .Lgfx1031_0
  .quad .Lgfx1031_id_end - .Lgfx1031_id
.Lgfx1031_id:
  .ascii "hipv4-amdgcn-amd-amdhsa--gfx1031"
.Lgfx1031_id_end:

  .quad .Lgfx90a_0 - .Lbundle0
  .quad .Lgfx90a_0_end - .Lgfx90a_0
  .quad .Lgfx90a_id_end - .Lgfx90a_id
.Lgfx90a_id:
  .ascii "hipv4-amdgcn-amd-amdhsa--gfx90a"
.Lgfx90a_id_end:

  .balign 4096
.Lbundle0_payload:
.Lgfx1031_0:
  .incbin "gfx1031.co"
.Lgfx1031_0_end:

  .balign 4096
.Lgfx90a_0:
  .incbin "gfx90a.co"
.Lgfx90a_0_end:

  .balign 4096
.Lbundle1:
  .ascii "__CLANG_OFFLOAD_BUNDLE__"
  .quad 3

  .quad .Lbundle1_payload - .Lbundle1
  .quad 0
  .quad .Lhost1_id_end - .Lhost1_id
.Lhost1_id:
  .ascii "host-x86_64-unknown-linux-gnu-"
.Lhost1_id_end:

  .quad .Lgfx1031_1 - .Lbundle1
  .quad .Lgfx1031_1_end - .Lgfx1031_1
  .quad .Lgfx1031_1_id_end - .Lgfx1031_1_id
.Lgfx1031_1_id:
  .ascii "hipv4-amdgcn-amd-amdhsa--gfx1031"
.Lgfx1031_1_id_end:

  .quad .Lgfx90a_1 - .Lbundle1
  .quad .Lgfx90a_1_end - .Lgfx90a_1
  .quad .Lgfx90a_1_id_end - .Lgfx90a_1_id
.Lgfx90a_1_id:
  .ascii "hipv4-amdgcn-amd-amdhsa--gfx90a"
.Lgfx90a_1_id_end:

  .balign 4096
.Lbundle1_payload:
.Lgfx1031_1:
  .incbin "gfx1031-second.co"
.Lgfx1031_1_end:

  .balign 4096
.Lgfx90a_1:
  .incbin "gfx90a-second.co"
.Lgfx90a_1_end:

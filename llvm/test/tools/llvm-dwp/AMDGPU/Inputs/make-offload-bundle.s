  .section .hip_fatbin,"",@progbits
.Lbundle:
  .ascii "__CLANG_OFFLOAD_BUNDLE__"
  .quad 2

  .quad .Lgfx1031 - .Lbundle
  .quad .Lgfx1031_end - .Lgfx1031
  .quad .Lgfx1031_id_end - .Lgfx1031_id
.Lgfx1031_id:
  .ascii "hipv4-amdgcn-amd-amdhsa--gfx1031"
.Lgfx1031_id_end:

  .quad .Lgfx90a - .Lbundle
  .quad .Lgfx90a_end - .Lgfx90a
  .quad .Lgfx90a_id_end - .Lgfx90a_id
.Lgfx90a_id:
  .ascii "hipv4-amdgcn-amd-amdhsa--gfx90a"
.Lgfx90a_id_end:

  .balign 8
.Lgfx1031:
  .incbin "gfx1031.co"
.Lgfx1031_end:

  .balign 8
.Lgfx90a:
  .incbin "gfx90a.co"
.Lgfx90a_end:

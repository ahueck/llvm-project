## Embeds a pre-built OffloadBinary blob (see offloading_recursion.test,
## which builds "offload.bin" via llvm-offload-binary) into a
  .section .llvm.offloading,"",@llvm_offloading
  .incbin "offload.bin"

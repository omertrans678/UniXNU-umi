    .section __DATA_CONST,__const
    .globl _t970_gpu_addon_data
    .p2align 3
_t970_gpu_addon_data:
    .incbin "/mnt/c/MyData/Umi2/iOS/umi-work/out/umi/gpu-addon.bin"
L_t970_gpu_addon_end:

    .globl _t970_gpu_addon_size
    .p2align 3
_t970_gpu_addon_size:
    .quad L_t970_gpu_addon_end - _t970_gpu_addon_data

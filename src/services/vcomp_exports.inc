// 名稱、參數數量、Win32 64 位元參數遮罩。
VCOMP(_vcomp_fork,3,0)
VCOMP(_vcomp_for_static_simple_init,6,0)
VCOMP(_vcomp_for_static_simple_init_i8,6,7)
VCOMP(_vcomp_for_static_init,9,0)
VCOMP(_vcomp_for_static_init_i8,9,15)
VCOMP(_vcomp_for_static_end,0,0)
VCOMP(_vcomp_for_dynamic_init,5,0)
VCOMP(_vcomp_for_dynamic_init_i8,5,30)
VCOMP(_vcomp_for_dynamic_next,2,0)
VCOMP(_vcomp_for_dynamic_next_i8,2,0)
VCOMP(_vcomp_barrier,0,0)
VCOMP(_vcomp_flush,0,0)
VCOMP(_vcomp_set_num_threads,1,0)
VCOMP(_vcomp_get_thread_num,0,0)
VCOMP(omp_get_thread_num,0,0)
VCOMP(omp_get_num_threads,0,0)
VCOMP(omp_get_num_procs,0,0)
VCOMP(omp_get_max_threads,0,0)
VCOMP(omp_set_num_threads,1,0)
VCOMP(omp_in_parallel,0,0)
VCOMP(omp_set_nested,1,0)
VCOMP(omp_get_nested,0,0)
VCOMP(omp_set_dynamic,1,0)
VCOMP(omp_get_dynamic,0,0)
VCOMP(C2VectParallel,8,0)
#include "vcomp_reduction_exports.inc"
#include "vcomp_atomic_exports.inc"
#include "vcomp_sections_exports.inc"
#include "vcomp_single_exports.inc"
#include "vcomp_critsect_exports.inc"

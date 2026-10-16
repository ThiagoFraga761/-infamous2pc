/* inFAMOUS 2 (BCUS98125): SPU jobs whose pixel work the GPU does instead.
 *
 * The ambient-occlusion job (P075) reads NV3089 copies of the G-buffer from
 * main memory and runs five kernels, job plugins loaded at LS 0x6C00 and
 * entered at 0x6C30, for its output: a 1280x720 8-bit occlusion texture the
 * RSX samples later in the frame (and a 640x360 linear depth in its scratch,
 * which the engine can stand in for too). The draw engine computes the same on
 * the GPU (rsx_draw_engine_set_native_ssao) and, while the game samples the
 * GPU's result, the kernels return at once; the job keeps its protocol, label
 * 0x890 included. Until the first substitution, and after a failure, the
 * kernels do their own work.
 *
 * The lighting job (P071) reads the same two copies, lights the frame in
 * 40x18 tiles and writes the diffuse and the specular light over them, which
 * the game samples as textures. The engine computes those on the GPU too
 * (rsx_draw_engine_set_native_lighting), from the parameters the job loads,
 * and while the game samples the GPU's result the job's per-tile
 * function (LS 0x4AC8, pure computation on local store: no channel or DMA
 * access anywhere below it) returns at once; the job's transfers, queue and
 * label 0x870 stay as they are.
 *
 * PS3RECOMP_NATIVE_SSAO / PS3RECOMP_NATIVE_LIGHTING =1 turn them on; 2
 * computes on the GPU without using the result and logs how it compares with
 * the SPU's. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "rsx_draw_engine.h"
#include "spu_workload.h"

static unsigned env_mode(const char* name)
{
    const char* e = getenv(name);
    const unsigned mode = e ? (unsigned)strtoul(e, NULL, 0) : 0u;
    return mode == 1u || mode == 2u ? mode : 0u;
}

/* The two G-buffer copies (1280x720, 4 bytes a pixel), the occlusion job's
 * scratch (half-size depth and occlusion) and its output image. */
#define NORMALS_EA          0x473F3B80u
#define DEPTH_EA            0x47777B80u
#define GBUFFER_BYTES       (1280u * 720u * 4u)
#define SSAO_SCRATCH_EA     0x47AFBB80u
#define SSAO_SCRATCH_BYTES  (640u * 360u * 5u)   /* half-size depth (4 B) + occlusion (1 B) */
#define OCCLUSION_EA        0xCF800000u
#define OCCLUSION_BYTES     (1280u * 720u)

/* The lighting job loads its 256-byte parameter block from here at its start;
 * those bytes, and the light table they point to, are what the GPU lights the
 * frame with. */
#define LIGHTING_PARAMS_EA 0x00A25A80u

/* The lighting job is job module overlay 212. */
static void lighting_job_params(uint32_t ea, uint32_t size, const uint8_t* src, int overlay)
{
    (void)ea; (void)size;
    if (overlay == 212) rsx_draw_engine_native_lighting_job(src);
}

static void native_lighting_init(void)
{
    const unsigned mode = env_mode("PS3RECOMP_NATIVE_LIGHTING");
    if (!mode) return;
    const rsx_native_lighting_desc d = {
        .mode       = mode,
        .normals_ea = NORMALS_EA,
        .depth_ea   = DEPTH_EA,
    };
    rsx_draw_engine_set_native_lighting(&d);
    if (spu_dma_get_watch(LIGHTING_PARAMS_EA, 0x100u, lighting_job_params)) {
        fprintf(stderr, "[port] native lighting: no DMA watch slot left, off\n");
        rsx_draw_engine_set_native_lighting(NULL);
        return;
    }
    const int gated = mode == 1u &&
        !spu_gate_function(212, 0x4AC8u, rsx_draw_engine_native_lighting_skip());
    /* While the tiles are skipped, the job's row transfers of the two images
     * carry nothing anyone uses. */
    if (gated) {
        spu_dma_skip_range(NORMALS_EA, GBUFFER_BYTES, 212, rsx_draw_engine_native_lighting_skip());
        spu_dma_skip_range(DEPTH_EA, GBUFFER_BYTES, 212, rsx_draw_engine_native_lighting_skip());
    }
    fprintf(stderr, "[port] native lighting mode %u: %d SPU tile function gated\n", mode, gated);
}

void port_native_passes_init(void)
{
    native_lighting_init();
    const unsigned mode = env_mode("PS3RECOMP_NATIVE_SSAO");
    if (!mode) return;
    static const rsx_native_ssao_desc ssao = {
        .normals_ea    = NORMALS_EA,
        .depth_ea      = DEPTH_EA,
        .occlusion_ea  = OCCLUSION_EA,
        .half_depth_ea = SSAO_SCRATCH_EA,
        .params_ea     = 0x00A26780u,
    };
    rsx_native_ssao_desc d = ssao;
    d.mode = mode;
    rsx_draw_engine_set_native_ssao(&d);
    int gated = 0;
    if (mode == 1u)
        for (int image = 240; image <= 244; image++)
            gated += !spu_gate_function(image, 0x6C30u, rsx_draw_engine_native_ssao_live());
    if (gated == 5) {
        /* While the kernels skip, the job (overlay 219) moves the G-buffer,
         * its scratch and its occlusion image for nobody. */
        const volatile int* live = rsx_draw_engine_native_ssao_live();
        spu_dma_skip_range(NORMALS_EA, GBUFFER_BYTES, 219, live);
        spu_dma_skip_range(DEPTH_EA, GBUFFER_BYTES, 219, live);
        spu_dma_skip_range(SSAO_SCRATCH_EA, SSAO_SCRATCH_BYTES, 219, live);
        spu_dma_skip_range(OCCLUSION_EA, OCCLUSION_BYTES, 219, live);
    }
    fprintf(stderr, "[port] native SSAO mode %u: %d SPU kernels gated\n", mode, gated);
}

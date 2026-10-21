#include "qemu/osdep.h"
#include "cpu.h"
#include "cpregs.h"
#include "trace.h"
#include "qemu/error-report.h"
#include "system/kvm.h"
#include "system/tcg.h"
#include "kvm_arm.h"
#include "internals.h"
#include "cpu-features.h"
#include "migration/qemu-file-types.h"
#include "migration/vmstate.h"
#include "target/arm/gtimer.h"
#include "hw/arm/machines-qom.h"
#include "hw/intc/armv7m_nvic.h"

static bool vfp_needed(void *opaque)
{
    ARMCPU *cpu = opaque;

    return (arm_feature(&cpu->env, ARM_FEATURE_AARCH64)
            ? cpu_isar_feature(aa64_fp_simd, cpu)
            : cpu_isar_feature(aa32_vfp_simd, cpu));
}

static bool vfp_fpcr_fpsr_needed(void *opaque)
{
    /*
     * If either the FPCR or the FPSR include set bits that are not
     * visible in the AArch32 FPSCR view of floating point control/status
     * then we must send the FPCR and FPSR as two separate fields in the
     * cpu/vfp/fpcr_fpsr subsection, and we will send a 0 for the old
     * FPSCR field in cpu/vfp.
     *
     * If all the set bits are representable in an AArch32 FPSCR then we
     * send that value as the cpu/vfp FPSCR field, and don't send the
     * cpu/vfp/fpcr_fpsr subsection.
     *
     * On incoming migration, if the cpu/vfp FPSCR field is non-zero we
     * use it, and if the fpcr_fpsr subsection is present we use that.
     * (The subsection will never be present with a non-zero FPSCR field,
     * and if FPSCR is zero and the subsection is not present that means
     * that FPSCR/FPSR/FPCR are zero.)
     *
     * This preserves migration compatibility with older QEMU versions,
     * in both directions.
     */
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;

    return (vfp_get_fpcr(env) & ~FPSCR_FPCR_MASK) ||
        (vfp_get_fpsr(env) & ~FPSCR_FPSR_MASK);
}

static int get_fpscr(QEMUFile *f, void *opaque, size_t size,
                     const VMStateField *field)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;
    uint32_t val = qemu_get_be32(f);

    if (val) {
        /* 0 means we might have the data in the fpcr_fpsr subsection */
        vfp_set_fpscr(env, val);
    }
    return 0;
}

static int put_fpscr(QEMUFile *f, void *opaque, size_t size,
                     const VMStateField *field, JSONWriter *vmdesc)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;
    uint32_t fpscr = vfp_fpcr_fpsr_needed(opaque) ? 0 : vfp_get_fpscr(env);

    qemu_put_be32(f, fpscr);
    return 0;
}

static const VMStateInfo vmstate_fpscr = {
    .name = "fpscr",
    .get = get_fpscr,
    .put = put_fpscr,
};

static int get_fpcr(QEMUFile *f, void *opaque, size_t size,
                     const VMStateField *field)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;
    uint64_t val = qemu_get_be64(f);

    vfp_set_fpcr(env, val);
    return 0;
}

static int put_fpcr(QEMUFile *f, void *opaque, size_t size,
                     const VMStateField *field, JSONWriter *vmdesc)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;

    qemu_put_be64(f, vfp_get_fpcr(env));
    return 0;
}

static const VMStateInfo vmstate_fpcr = {
    .name = "fpcr",
    .get = get_fpcr,
    .put = put_fpcr,
};

static int get_fpsr(QEMUFile *f, void *opaque, size_t size,
                     const VMStateField *field)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;
    uint64_t val = qemu_get_be64(f);

    vfp_set_fpsr(env, val);
    return 0;
}

static int put_fpsr(QEMUFile *f, void *opaque, size_t size,
                     const VMStateField *field, JSONWriter *vmdesc)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;

    qemu_put_be64(f, vfp_get_fpsr(env));
    return 0;
}

static const VMStateInfo vmstate_fpsr = {
    .name = "fpsr",
    .get = get_fpsr,
    .put = put_fpsr,
};

static const VMStateDescription vmstate_vfp_fpcr_fpsr = {
    .name = "cpu/vfp/fpcr_fpsr",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = vfp_fpcr_fpsr_needed,
    .fields = (const VMStateField[]) {
        {
            .name = "fpcr",
            .version_id = 0,
            .size = sizeof(uint64_t),
            .info = &vmstate_fpcr,
            .flags = VMS_SINGLE,
            .offset = 0,
        },
        {
            .name = "fpsr",
            .version_id = 0,
            .size = sizeof(uint64_t),
            .info = &vmstate_fpsr,
            .flags = VMS_SINGLE,
            .offset = 0,
        },
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_vfp = {
    .name = "cpu/vfp",
    .version_id = 3,
    .minimum_version_id = 3,
    .needed = vfp_needed,
    .fields = (const VMStateField[]) {
        /* For compatibility, store Qn out of Zn here.  */
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[0].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[1].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[2].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[3].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[4].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[5].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[6].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[7].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[8].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[9].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[10].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[11].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[12].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[13].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[14].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[15].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[16].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[17].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[18].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[19].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[20].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[21].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[22].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[23].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[24].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[25].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[26].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[27].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[28].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[29].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[30].d, ARMCPU, 0, 2),
        VMSTATE_UINT64_SUB_ARRAY(env.vfp.zregs[31].d, ARMCPU, 0, 2),

        /* The xregs array is a little awkward because element 1 (FPSCR)
         * requires a specific accessor, so we have to split it up in
         * the vmstate:
         */
        VMSTATE_UINT32(env.vfp.xregs[0], ARMCPU),
        VMSTATE_UINT32_SUB_ARRAY(env.vfp.xregs, ARMCPU, 2, 14),
        {
            .name = "fpscr",
            .version_id = 0,
            .size = sizeof(uint32_t),
            .info = &vmstate_fpscr,
            .flags = VMS_SINGLE,
            .offset = 0,
        },
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_vfp_fpcr_fpsr,
        NULL
    }
};

/* The expression ARM_MAX_VQ - 2 is 0 for pure AArch32 build,
 * and ARMPredicateReg is actively empty.  This triggers errors
 * in the expansion of the VMSTATE macros.
 */

static bool sve_needed(void *opaque)
{
    ARMCPU *cpu = opaque;

    return cpu_isar_feature(aa64_sve, cpu) || cpu_isar_feature(aa64_sme, cpu);
}

/* The first two words of each Zreg is stored in VFP state.  */
static const VMStateDescription vmstate_zreg_hi_reg = {
    .name = "cpu/sve/zreg_hi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_SUB_ARRAY(d, ARMVectorReg, 2, ARM_MAX_VQ - 2),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_preg_reg = {
    .name = "cpu/sve/preg",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_ARRAY(p, ARMPredicateReg, 2 * ARM_MAX_VQ / 8),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sve = {
    .name = "cpu/sve",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sve_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(env.vfp.zregs, ARMCPU, 32, 0,
                             vmstate_zreg_hi_reg, ARMVectorReg),
        VMSTATE_STRUCT_ARRAY(env.vfp.pregs, ARMCPU, 17, 0,
                             vmstate_preg_reg, ARMPredicateReg),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_vreg = {
    .name = "vreg",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_ARRAY(d, ARMVectorReg, ARM_MAX_VQ * 2),
        VMSTATE_END_OF_LIST()
    }
};

static bool za_needed(void *opaque)
{
    ARMCPU *cpu = opaque;

    /*
     * When ZA storage is disabled, its contents are discarded.
     * It will be zeroed when ZA storage is re-enabled.
     */
    return FIELD_EX64(cpu->env.svcr, SVCR, ZA);
}

static const VMStateDescription vmstate_za = {
    .name = "cpu/sme",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = za_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(env.za_state.za, ARMCPU, ARM_MAX_VQ * 16, 0,
                             vmstate_vreg, ARMVectorReg),
        VMSTATE_END_OF_LIST()
    }
};

static bool zt0_needed(void *opaque)
{
    ARMCPU *cpu = opaque;

    return za_needed(cpu) && cpu_isar_feature(aa64_sme2, cpu);
}

static const VMStateDescription vmstate_zt0 = {
    .name = "cpu/zt0",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = zt0_needed,
    .fields = (VMStateField[]) {
        VMSTATE_UINT64_ARRAY(env.za_state.zt0, ARMCPU,
                             ARRAY_SIZE(((CPUARMState *)0)->za_state.zt0)),
        VMSTATE_END_OF_LIST()
    }
};

static bool serror_needed(void *opaque)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;

    return env->serror.pending != 0;
}

static const VMStateDescription vmstate_serror = {
    .name = "cpu/serror",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = serror_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(env.serror.pending, ARMCPU),
        VMSTATE_UINT8(env.serror.has_esr, ARMCPU),
        VMSTATE_UINT64(env.serror.esr, ARMCPU),
        VMSTATE_END_OF_LIST()
    }
};

static bool irq_line_state_needed(void *opaque)
{
    return true;
}

static const VMStateDescription vmstate_irq_line_state = {
    .name = "cpu/irq-line-state",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = irq_line_state_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(env.irq_line_state, ARMCPU),
        VMSTATE_END_OF_LIST()
    }
};

static bool wfxt_timer_needed(void *opaque)
{
    ARMCPU *cpu = opaque;

    /* We'll only have the timer object if FEAT_WFxT is implemented */
    return cpu->wfxt_timer;
}

static const VMStateDescription vmstate_wfxt_timer = {
    .name = "cpu/wfxt-timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = wfxt_timer_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(wfxt_timer, ARMCPU),
        VMSTATE_END_OF_LIST()
    }
};

static bool m_needed(void *opaque)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;

    return arm_feature(env, ARM_FEATURE_M);
}

static bool m_actlr_needed(void *opaque)
{
    ARMCPU *cpu = opaque;

    return (cpu->midr & 0xff0ffff0) == 0x410fc270;
}

static const VMStateDescription vmstate_m_actlr = {
    .name = "cpu/m/actlr",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = m_actlr_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(env.v7m.actlr, ARMCPU),
        VMSTATE_END_OF_LIST()
    }
};

static bool m_itcmcr_needed(void *opaque)
{
    ARMCPU *cpu = opaque;
    return m_actlr_needed(opaque) && cpu->itcm_size;
}

static int m_itcmcr_validate(void *opaque, int version_id)
{
    ARMCPU *cpu = opaque;
    return cpu->env.v7m.itcmcr & ~0x7 ? -EINVAL : 0;
}

static const VMStateDescription vmstate_m_itcmcr = {
    .name = "cpu/m/itcmcr",
    .version_id = 2,
    .minimum_version_id = 1,
    .needed = m_itcmcr_needed,
    .post_load = m_itcmcr_validate,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_EQUAL(itcm_size, ARMCPU, NULL),
        VMSTATE_UINT32_EQUAL(itcm_init_enable, ARMCPU, NULL),
        VMSTATE_UINT32_EQUAL_V(itcm_init_rmw_enable, ARMCPU, 2, NULL),
        VMSTATE_UINT32(env.v7m.itcmcr, ARMCPU),
        VMSTATE_END_OF_LIST()
    }
};

static int m_demcr_validate(void *opaque, int version_id)
{
    ARMCPU *cpu = opaque;
    if (cpu->env.v7m.demcr & ~(1U << 24)) {
        return -EINVAL;
    }
    armv7m_nvic_set_trace_enable(cpu->env.nvic,
                                !!(cpu->env.v7m.demcr & (1U << 24)));
    return 0;
}

static const VMStateDescription vmstate_m_demcr = {
    .name = "cpu/m/demcr",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = m_needed,
    .post_load = m_demcr_validate,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(env.v7m.demcr, ARMCPU),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_m_faultmask_primask = {
    .name = "cpu/m/faultmask-primask",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = m_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(env.v7m.faultmask[M_REG_NS], ARMCPU),
        VMSTATE_UINT32(env.v7m.primask[M_REG_NS], ARMCPU),
        VMSTATE_END_OF_LIST()
    }
};

/* CSSELR is in a subsection because we didn't implement it previously.
 * Migration from an old implementation will leave it at zero, which
 * is OK since the only CPUs in the old implementation make the
 * register RAZ/WI.
 * Since there was no version of QEMU which implemented the CSSELR for
 * just non-secure, we transfer both banks here rather than putting
 * the secure banked version in the m-security subsection.
 */
static bool csselr_vmstate_validate(void *opaque, int version_id)
{
    ARMCPU *cpu = opaque;

    return cpu->env.v7m.csselr[M_REG_NS] <= R_V7M_CSSELR_INDEX_MASK
        && cpu->env.v7m.csselr[M_REG_S] <= R_V7M_CSSELR_INDEX_MASK;
}

static bool m_csselr_needed(void *opaque)
{
    ARMCPU *cpu = opaque;

    return !arm_v7m_csselr_razwi(cpu);
}

static const VMStateDescription vmstate_m_csselr = {
    .name = "cpu/m/csselr",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = m_csselr_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(env.v7m.csselr, ARMCPU, M_REG_NUM_BANKS),
        VMSTATE_VALIDATE("CSSELR is valid", csselr_vmstate_validate),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_m_scr = {
    .name = "cpu/m/scr",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = m_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(env.v7m.scr[M_REG_NS], ARMCPU),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_m_other_sp = {
    .name = "cpu/m/other-sp",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = m_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(env.v7m.other_sp, ARMCPU),
        VMSTATE_END_OF_LIST()
    }
};

static bool m_v8m_needed(void *opaque)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;

    return arm_feature(env, ARM_FEATURE_M) && arm_feature(env, ARM_FEATURE_V8);
}

static const VMStateDescription vmstate_m_v8m = {
    .name = "cpu/m/v8m",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = m_v8m_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(env.v7m.msplim, ARMCPU, M_REG_NUM_BANKS),
        VMSTATE_UINT32_ARRAY(env.v7m.psplim, ARMCPU, M_REG_NUM_BANKS),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_m_fp = {
    .name = "cpu/m/fp",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = vfp_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(env.v7m.fpcar, ARMCPU, M_REG_NUM_BANKS),
        VMSTATE_UINT32_ARRAY(env.v7m.fpccr, ARMCPU, M_REG_NUM_BANKS),
        VMSTATE_UINT32_ARRAY(env.v7m.fpdscr, ARMCPU, M_REG_NUM_BANKS),
        VMSTATE_UINT32_ARRAY(env.v7m.cpacr, ARMCPU, M_REG_NUM_BANKS),
        VMSTATE_UINT32(env.v7m.nsacr, ARMCPU),
        VMSTATE_END_OF_LIST()
    }
};

static bool mve_needed(void *opaque)
{
    ARMCPU *cpu = opaque;

    return cpu_isar_feature(aa32_mve, cpu);
}

static const VMStateDescription vmstate_m_mve = {
    .name = "cpu/m/mve",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mve_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(env.v7m.vpr, ARMCPU),
        VMSTATE_UINT32(env.v7m.ltpsize, ARMCPU),
        VMSTATE_END_OF_LIST()
    },
};

static bool event_needed(void *opaque)
{
    ARMCPU *cpu = opaque;

    return cpu->env.event_register;
}

static const VMStateDescription vmstate_event = {
    .name = "cpu/event",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = event_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(env.event_register, ARMCPU),
        VMSTATE_END_OF_LIST()
    }
};

static bool m_mpu_rnr_invalid_needed(void *opaque)
{
    ARMCPU *cpu = opaque;

    /*
     * Only set between an UNPREDICTABLE MPU_RNR/MPU_RBAR write naming an
     * unimplemented region and the next write that reselects a valid one,
     * so for any well-behaved guest this subsection is never sent and
     * migration to an older QEMU keeps working.
     */
    return cpu->env.v7m.mpu_rnr_invalid[M_REG_NS] ||
           cpu->env.v7m.mpu_rnr_invalid[M_REG_S];
}

static const VMStateDescription vmstate_m_mpu_rnr_invalid = {
    .name = "cpu/m/mpu-rnr-invalid",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = m_mpu_rnr_invalid_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(env.v7m.mpu_rnr_invalid[M_REG_NS], ARMCPU),
        VMSTATE_BOOL(env.v7m.mpu_rnr_invalid[M_REG_S], ARMCPU),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_m = {
    .name = "cpu/m",
    .version_id = 4,
    .minimum_version_id = 4,
    .needed = m_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(env.v7m.vecbase[M_REG_NS], ARMCPU),
        VMSTATE_UINT32(env.v7m.basepri[M_REG_NS], ARMCPU),
        VMSTATE_UINT32(env.v7m.control[M_REG_NS], ARMCPU),
        VMSTATE_UINT32(env.v7m.ccr[M_REG_NS], ARMCPU),
        VMSTATE_UINT32(env.v7m.cfsr[M_REG_NS], ARMCPU),
        VMSTATE_UINT32(env.v7m.hfsr, ARMCPU),
        VMSTATE_UINT32(env.v7m.dfsr, ARMCPU),
        VMSTATE_UINT32(env.v7m.mmfar[M_REG_NS], ARMCPU),
        VMSTATE_UINT32(env.v7m.bfar, ARMCPU),
        VMSTATE_UINT32(env.v7m.mpu_ctrl[M_REG_NS], ARMCPU),
        VMSTATE_INT32(env.v7m.exception, ARMCPU),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_m_faultmask_primask,
        &vmstate_m_csselr,
        &vmstate_m_scr,
        &vmstate_m_other_sp,
        &vmstate_m_v8m,
        &vmstate_m_fp,
        &vmstate_m_mve,
        &vmstate_m_mpu_rnr_invalid,
        &vmstate_m_actlr,
        &vmstate_m_itcmcr,
        &vmstate_m_demcr,
        NULL
    }
};

static bool thumb2ee_needed(void *opaque)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;

    return arm_feature(env, ARM_FEATURE_THUMB2EE);
}

static const VMStateDescription vmstate_thumb2ee = {
    .name = "cpu/thumb2ee",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = thumb2ee_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(env.teecr, ARMCPU),
        VMSTATE_UINT32(env.teehbr, ARMCPU),
        VMSTATE_END_OF_LIST()
    }
};

static bool pmsav7_needed(void *opaque)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;

    return arm_feature(env, ARM_FEATURE_PMSA) &&
           arm_feature(env, ARM_FEATURE_V7) &&
           !arm_feature(env, ARM_FEATURE_V8);
}

static bool pmsav7_rgnr_vmstate_validate(void *opaque, int version_id)
{
    ARMCPU *cpu = opaque;

    return cpu->env.pmsav7.rnr[M_REG_NS] < cpu->pmsav7_dregion;
}

static const VMStateDescription vmstate_pmsav7 = {
    .name = "cpu/pmsav7",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = pmsav7_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_VARRAY_UINT32(env.pmsav7.drbar, ARMCPU, pmsav7_dregion, 0,
                              vmstate_info_uint32, uint32_t),
        VMSTATE_VARRAY_UINT32(env.pmsav7.drsr, ARMCPU, pmsav7_dregion, 0,
                              vmstate_info_uint32, uint32_t),
        VMSTATE_VARRAY_UINT32(env.pmsav7.dracr, ARMCPU, pmsav7_dregion, 0,
                              vmstate_info_uint32, uint32_t),
        VMSTATE_VALIDATE("rgnr is valid", pmsav7_rgnr_vmstate_validate),
        VMSTATE_END_OF_LIST()
    }
};

static bool pmsav7_rnr_needed(void *opaque)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;

    /* For R profile cores pmsav7.rnr is migrated via the cpreg
     * "RGNR" definition in helper.h. For M profile we have to
     * migrate it separately.
     */
    return arm_feature(env, ARM_FEATURE_M);
}

static const VMStateDescription vmstate_pmsav7_rnr = {
    .name = "cpu/pmsav7-rnr",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = pmsav7_rnr_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(env.pmsav7.rnr[M_REG_NS], ARMCPU),
        VMSTATE_END_OF_LIST()
    }
};

static bool pmsav8_needed(void *opaque)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;

    return arm_feature(env, ARM_FEATURE_PMSA) &&
        arm_feature(env, ARM_FEATURE_V8);
}

static bool pmsav8r_needed(void *opaque)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;

    return arm_feature(env, ARM_FEATURE_PMSA) &&
        arm_feature(env, ARM_FEATURE_V8) &&
        !arm_feature(env, ARM_FEATURE_M);
}

static const VMStateDescription vmstate_pmsav8r = {
    .name = "cpu/pmsav8/pmsav8r",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = pmsav8r_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_VARRAY_UINT32(env.pmsav8.hprbar, ARMCPU,
                        pmsav8r_hdregion, 0, vmstate_info_uint32, uint32_t),
        VMSTATE_VARRAY_UINT32(env.pmsav8.hprlar, ARMCPU,
                        pmsav8r_hdregion, 0, vmstate_info_uint32, uint32_t),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_pmsav8 = {
    .name = "cpu/pmsav8",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = pmsav8_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_VARRAY_UINT32(env.pmsav8.rbar[M_REG_NS], ARMCPU, pmsav7_dregion,
                              0, vmstate_info_uint32, uint32_t),
        VMSTATE_VARRAY_UINT32(env.pmsav8.rlar[M_REG_NS], ARMCPU, pmsav7_dregion,
                              0, vmstate_info_uint32, uint32_t),
        VMSTATE_UINT32(env.pmsav8.mair0[M_REG_NS], ARMCPU),
        VMSTATE_UINT32(env.pmsav8.mair1[M_REG_NS], ARMCPU),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_pmsav8r,
        NULL
    }
};

static bool s_rnr_vmstate_validate(void *opaque, int version_id)
{
    ARMCPU *cpu = opaque;

    return cpu->env.pmsav7.rnr[M_REG_S] < cpu->pmsav7_dregion;
}

static bool sau_rnr_vmstate_validate(void *opaque, int version_id)
{
    ARMCPU *cpu = opaque;

    return cpu->env.sau.rnr < cpu->sau_sregion;
}

static bool m_security_needed(void *opaque)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;

    return arm_feature(env, ARM_FEATURE_M_SECURITY);
}

static const VMStateDescription vmstate_m_security = {
    .name = "cpu/m-security",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = m_security_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(env.v7m.secure, ARMCPU),
        VMSTATE_UINT32(env.v7m.other_ss_msp, ARMCPU),
        VMSTATE_UINT32(env.v7m.other_ss_psp, ARMCPU),
        VMSTATE_UINT32(env.v7m.basepri[M_REG_S], ARMCPU),
        VMSTATE_UINT32(env.v7m.primask[M_REG_S], ARMCPU),
        VMSTATE_UINT32(env.v7m.faultmask[M_REG_S], ARMCPU),
        VMSTATE_UINT32(env.v7m.control[M_REG_S], ARMCPU),
        VMSTATE_UINT32(env.v7m.vecbase[M_REG_S], ARMCPU),
        VMSTATE_UINT32(env.pmsav8.mair0[M_REG_S], ARMCPU),
        VMSTATE_UINT32(env.pmsav8.mair1[M_REG_S], ARMCPU),
        VMSTATE_VARRAY_UINT32(env.pmsav8.rbar[M_REG_S], ARMCPU, pmsav7_dregion,
                              0, vmstate_info_uint32, uint32_t),
        VMSTATE_VARRAY_UINT32(env.pmsav8.rlar[M_REG_S], ARMCPU, pmsav7_dregion,
                              0, vmstate_info_uint32, uint32_t),
        VMSTATE_UINT32(env.pmsav7.rnr[M_REG_S], ARMCPU),
        VMSTATE_VALIDATE("secure MPU_RNR is valid", s_rnr_vmstate_validate),
        VMSTATE_UINT32(env.v7m.mpu_ctrl[M_REG_S], ARMCPU),
        VMSTATE_UINT32(env.v7m.ccr[M_REG_S], ARMCPU),
        VMSTATE_UINT32(env.v7m.mmfar[M_REG_S], ARMCPU),
        VMSTATE_UINT32(env.v7m.cfsr[M_REG_S], ARMCPU),
        VMSTATE_UINT32(env.v7m.sfsr, ARMCPU),
        VMSTATE_UINT32(env.v7m.sfar, ARMCPU),
        VMSTATE_VARRAY_UINT32(env.sau.rbar, ARMCPU, sau_sregion, 0,
                              vmstate_info_uint32, uint32_t),
        VMSTATE_VARRAY_UINT32(env.sau.rlar, ARMCPU, sau_sregion, 0,
                              vmstate_info_uint32, uint32_t),
        VMSTATE_UINT32(env.sau.rnr, ARMCPU),
        VMSTATE_VALIDATE("SAU_RNR is valid", sau_rnr_vmstate_validate),
        VMSTATE_UINT32(env.sau.ctrl, ARMCPU),
        VMSTATE_UINT32(env.v7m.scr[M_REG_S], ARMCPU),
        /* AIRCR is not secure-only, but our implementation is R/O if the
         * security extension is unimplemented, so we migrate it here.
         */
        VMSTATE_UINT32(env.v7m.aircr, ARMCPU),
        VMSTATE_END_OF_LIST()
    }
};

static int get_cpsr(QEMUFile *f, void *opaque, size_t size,
                    const VMStateField *field)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;
    uint32_t val = qemu_get_be32(f);

    if (arm_feature(env, ARM_FEATURE_M)) {
        if (val & XPSR_EXCP) {
            /* This is a CPSR format value from an older QEMU. (We can tell
             * because values transferred in XPSR format always have zero
             * for the EXCP field, and CPSR format will always have bit 4
             * set in CPSR_M.) Rearrange it into XPSR format. The significant
             * differences are that the T bit is not in the same place, the
             * primask/faultmask info may be in the CPSR I and F bits, and
             * we do not want the mode bits.
             * We know that this cleanup happened before v8M, so there
             * is no complication with banked primask/faultmask.
             */
            uint32_t newval = val;

            assert(!arm_feature(env, ARM_FEATURE_M_SECURITY));

            newval &= (CPSR_NZCV | CPSR_Q | CPSR_IT | CPSR_GE);
            if (val & CPSR_T) {
                newval |= XPSR_T;
            }
            /* If the I or F bits are set then this is a migration from
             * an old QEMU which still stored the M profile FAULTMASK
             * and PRIMASK in env->daif. For a new QEMU, the data is
             * transferred using the vmstate_m_faultmask_primask subsection.
             */
            if (val & CPSR_F) {
                env->v7m.faultmask[M_REG_NS] = 1;
            }
            if (val & CPSR_I) {
                env->v7m.primask[M_REG_NS] = 1;
            }
            val = newval;
        }
        /* Ignore the low bits, they are handled by vmstate_m. */
        xpsr_write(env, val, ~XPSR_EXCP);
        return 0;
    }

    env->aarch64 = ((val & PSTATE_nRW) == 0);

    if (is_a64(env)) {
        pstate_write(env, val);
        return 0;
    }

    cpsr_write(env, val, 0xffffffff, CPSRWriteRaw);
    return 0;
}

static int put_cpsr(QEMUFile *f, void *opaque, size_t size,
                    const VMStateField *field, JSONWriter *vmdesc)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;
    uint32_t val;

    if (arm_feature(env, ARM_FEATURE_M)) {
        /* The low 9 bits are v7m.exception, which is handled by vmstate_m. */
        val = xpsr_read(env) & ~XPSR_EXCP;
    } else if (is_a64(env)) {
        val = pstate_read(env);
    } else {
        val = cpsr_read(env);
    }

    qemu_put_be32(f, val);
    return 0;
}

static const VMStateInfo vmstate_cpsr = {
    .name = "cpsr",
    .get = get_cpsr,
    .put = put_cpsr,
};

static int get_pstate64(QEMUFile *f, void *opaque, size_t size,
                        const VMStateField *field)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;
    uint64_t val = qemu_get_be64(f);

    env->aarch64 = ((val & PSTATE_nRW) == 0);
    if (is_a64(env)) {
        pstate_write(env, val);
    } else {
        cpsr_write_from_spsr_elx(env, val);
    }
    return 0;
}

static int put_pstate64(QEMUFile *f, void *opaque, size_t size,
                        const VMStateField *field, JSONWriter *vmdesc)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;
    uint64_t val;

    if (is_a64(env)) {
        val = pstate_read(env);
    } else {
        val = cpsr_read_for_spsr_elx(env);
    }
    qemu_put_be64(f, val);
    return 0;
}

static bool pstate64_needed(void *opaque)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;
    uint64_t val;

    if (arm_feature(env, ARM_FEATURE_M)) {
        return false;
    }
    if (is_a64(env)) {
        val = pstate_read(env);
    } else {
        val = cpsr_read_for_spsr_elx(env);
        if (val & PSTATE_SS) {
            return true;
        }
    }
    return val > UINT32_MAX;
}

static const VMStateDescription vmstate_pstate64 = {
    .name = "cpu/pstate64",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = pstate64_needed,
    .fields = (const VMStateField[]) {
        {
            .name = "pstate64",
            .version_id = 0,
            .size = sizeof(uint64_t),
            .info = &(const VMStateInfo) {
                .name = "pstate64",
                .get = get_pstate64,
                .put = put_pstate64,
            },
            .flags = VMS_SINGLE,
            .offset = 0,
        },
        VMSTATE_END_OF_LIST()
    },
};

static int get_power(QEMUFile *f, void *opaque, size_t size,
                    const VMStateField *field)
{
    ARMCPU *cpu = opaque;
    bool powered_off = qemu_get_byte(f);
    cpu->power_state = powered_off ? PSCI_OFF : PSCI_ON;
    return 0;
}

static int put_power(QEMUFile *f, void *opaque, size_t size,
                    const VMStateField *field, JSONWriter *vmdesc)
{
    ARMCPU *cpu = opaque;

    /* Migration should never happen while we transition power states */

    if (cpu->power_state == PSCI_ON ||
        cpu->power_state == PSCI_OFF) {
        bool powered_off = (cpu->power_state == PSCI_OFF) ? true : false;
        qemu_put_byte(f, powered_off);
        return 0;
    } else {
        return 1;
    }
}

static const VMStateInfo vmstate_powered_off = {
    .name = "powered_off",
    .get = get_power,
    .put = put_power,
};

static bool syndrome64_needed(void *opaque)
{
    ARMCPU *cpu = opaque;
    return cpu->env.exception.syndrome > UINT32_MAX;
}

static const VMStateDescription vmstate_syndrome64 = {
    .name = "cpu/syndrome64",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = syndrome64_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(env.exception.syndrome, ARMCPU),
        VMSTATE_END_OF_LIST()
    },
};

static int cpu_pre_save(void *opaque)
{
    ARMCPU *cpu = opaque;

    if (!kvm_enabled()) {
        pmu_op_start(&cpu->env);
    }

    if (kvm_enabled()) {
        if (!write_kvmstate_to_list(cpu)) {
            /* This should never fail */
            g_assert_not_reached();
        }

        /*
         * kvm_arm_cpu_pre_save() must be called after
         * write_kvmstate_to_list()
         */
        kvm_arm_cpu_pre_save(cpu);
    } else {
        if (!write_cpustate_to_list(cpu, false)) {
            /* This should never fail. */
            g_assert_not_reached();
        }
    }

    /*
     * On outbound migration, send the data in our cpreg_{values,indexes}
     * arrays. The migration code will not allocate anything, but just
     * reads the data pointed to by the VMSTATE_VARRAY_INT32_ALLOC() fields.
     */
    cpu->cpreg_vmstate_indexes = cpu->cpreg_indexes;
    cpu->cpreg_vmstate_values = cpu->cpreg_values;
    cpu->cpreg_vmstate_array_len = cpu->cpreg_array_len;

    return 0;
}

static int cpu_post_save(void *opaque)
{
    ARMCPU *cpu = opaque;

    if (!kvm_enabled()) {
        pmu_op_finish(&cpu->env);
    }

    cpu->cpreg_vmstate_indexes = NULL;
    cpu->cpreg_vmstate_values = NULL;

    return 0;
}

static int cpu_pre_load(void *opaque)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;

    /*
     * In an inbound migration where on the source FPSCR/FPSR/FPCR are 0,
     * there will be no fpcr_fpsr subsection so we won't call vfp_set_fpcr()
     * and vfp_set_fpsr() from get_fpcr() and get_fpsr(); also the get_fpscr()
     * function will not call vfp_set_fpscr() because it will see a 0 in the
     * inbound data. Ensure that in this case we have a correctly set up
     * zero FPSCR/FPCR/FPSR.
     *
     * This is not strictly needed because FPSCR is zero out of reset, but
     * it avoids the possibility of future confusing migration bugs if some
     * future architecture change makes the reset value non-zero.
     */
    vfp_set_fpscr(env, 0);

    /*
     * Pre-initialize irq_line_state to a value that's never valid as
     * real data, so cpu_post_load() can tell whether we've seen the
     * irq-line-state subsection in the incoming migration state.
     */
    env->irq_line_state = UINT32_MAX;

    if (!kvm_enabled()) {
        pmu_op_start(env);
    }

    g_assert(!cpu->cpreg_vmstate_indexes);
    g_assert(!cpu->cpreg_vmstate_values);

    return 0;
}

static gchar *print_register_name(uint64_t kvm_regidx)
{
    if (kvm_enabled()) {
        return kvm_print_register_name(kvm_regidx);
    } else {
        return g_strdup_printf("system register 0x%x", kvm_to_cpreg_id(kvm_regidx));
    }
}

/*
 * Handle the situation where @kvmidx is on destination but not
 * in the incoming stream. This never fails the migration.
 */
static void handle_cpreg_missing_in_incoming_stream(ARMCPU *cpu, uint64_t kvmidx)
{
    g_autofree gchar *name = print_register_name(kvmidx);

    warn_report("%s: %s "
                "expected by the destination but not in the incoming stream: "
                 "skip it", __func__, name);
}

/*
 * Handle the situation where @kvmidx is in the incoming stream
 * but not on destination. This currently fails the migration but
 * we plan to accomodate some exceptions, hence the boolean returned value.
 */
static bool handle_cpreg_only_in_incoming_stream(ARMCPU *cpu, uint64_t kvmidx)
{
    g_autofree gchar *name = print_register_name(kvmidx);
    bool fail = true;

    error_report("%s: %s in the incoming stream but unknown on the "
                 "destination: fail migration", __func__, name);

    return fail;
}

static int cpu_post_load(void *opaque, int version_id)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;
    bool fail = false;
    int i, v;

    trace_cpu_post_load(cpu->cpreg_vmstate_array_len,
                        cpu->cpreg_array_len);

    /*
     * Handle migration compatibility from old QEMU which didn't
     * send the irq-line-state subsection. A QEMU without it did not
     * implement the HCR_EL2.{VI,VF} bits as generating interrupts,
     * so for TCG the line state matches the bits set in cs->interrupt_request.
     * For KVM the line state is not stored in cs->interrupt_request
     * and so this will leave irq_line_state as 0, but this is OK because
     * we only need to care about it for TCG.
     */
    if (env->irq_line_state == UINT32_MAX) {
        CPUState *cs = CPU(cpu);

        env->irq_line_state = cs->interrupt_request &
            (CPU_INTERRUPT_HARD | CPU_INTERRUPT_FIQ |
             CPU_INTERRUPT_VIRQ | CPU_INTERRUPT_VFIQ);
    }

    /* Update the values list from the incoming migration data.
     * Anything in the incoming data which we don't know about is
     * a migration failure; anything we know about but the incoming
     * data doesn't specify retains its current (reset) value.
     * The indexes list remains untouched -- we only inspect the
     * incoming migration index list so we can match the values array
     * entries with the right slots in our own values array.
     */

    for (i = 0, v = 0; i < cpu->cpreg_array_len
             && v < cpu->cpreg_vmstate_array_len;) {
        if (cpu->cpreg_vmstate_indexes[v] > cpu->cpreg_indexes[i]) {
            handle_cpreg_missing_in_incoming_stream(cpu, cpu->cpreg_indexes[i++]);
            continue;
        }
        if (cpu->cpreg_vmstate_indexes[v] < cpu->cpreg_indexes[i]) {
            fail = handle_cpreg_only_in_incoming_stream(cpu,
                                                        cpu->cpreg_vmstate_indexes[v++]);
            continue;
        }
        /* matching register, copy the value over */
        cpu->cpreg_values[i] = cpu->cpreg_vmstate_values[v];
        i++;
        v++;
    }

    /*
     * if we have reached the end of the incoming array but there are
     * still regs in cpreg, continue parsing the regs which are missing
     * in the input stream
     */
    for ( ; i < cpu->cpreg_array_len; i++) {
        handle_cpreg_missing_in_incoming_stream(cpu, cpu->cpreg_indexes[i]);
    }
    /*
     * if we have reached the end of the cpreg array but there are
     * still regs in the input stream, continue parsing the vmstate array
     */
    for ( ; v < cpu->cpreg_vmstate_array_len; v++) {
        fail = handle_cpreg_only_in_incoming_stream(cpu,
                                                    cpu->cpreg_vmstate_indexes[v]);
    }
    if (fail) {
        return -1;
    }

    if (kvm_enabled()) {
        if (!kvm_arm_cpu_post_load(cpu)) {
            return -1;
        }
    } else {
        if (!write_list_to_cpustate(cpu)) {
            return -1;
        }
    }

    g_free(cpu->cpreg_vmstate_indexes);
    g_free(cpu->cpreg_vmstate_values);
    cpu->cpreg_vmstate_indexes = NULL;
    cpu->cpreg_vmstate_values = NULL;

    /*
     * Misaligned thumb pc is architecturally impossible. Fail the
     * incoming migration. For TCG it would trigger the assert in
     * thumb_tr_translate_insn().
     */
    if (!is_a64(env) && env->thumb && (env->regs[15] & 1)) {
        return -1;
    }

    if (tcg_enabled()) {
        hw_breakpoint_update_all(cpu);
        hw_watchpoint_update_all(cpu);
    }

    /*
     * TCG gen_update_fp_context() relies on the invariant that
     * FPDSCR.LTPSIZE is constant 4 for M-profile with the LOB extension;
     * forbid bogus incoming data with some other value.
     */
    if (arm_feature(env, ARM_FEATURE_M) && cpu_isar_feature(aa32_lob, cpu)) {
        if (extract32(env->v7m.fpdscr[M_REG_NS],
                      FPCR_LTPSIZE_SHIFT, FPCR_LTPSIZE_LENGTH) != 4 ||
            extract32(env->v7m.fpdscr[M_REG_S],
                      FPCR_LTPSIZE_SHIFT, FPCR_LTPSIZE_LENGTH) != 4) {
            return -1;
        }
    }

    if (!kvm_enabled()) {
        pmu_op_finish(env);
    }

    if (tcg_enabled()) {
        arm_rebuild_hflags(env);
    }

    return 0;
}

static const VMStateDescription vmstate_gnw_cycle_metadata = {
    .name = "cpu/gnw-cycle-model/metadata",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(pc, GnwCycleInsnMetadata),
        VMSTATE_UINT32(encoding, GnwCycleInsnMetadata),
        VMSTATE_UINT16(gpr_reads, GnwCycleInsnMetadata),
        VMSTATE_UINT16(gpr_writes, GnwCycleInsnMetadata),
        VMSTATE_UINT16(store_data_reads, GnwCycleInsnMetadata),
        VMSTATE_UINT16(address_reads, GnwCycleInsnMetadata),
        VMSTATE_UINT8(size, GnwCycleInsnMetadata),
        VMSTATE_UINT8(insn_class, GnwCycleInsnMetadata),
        VMSTATE_UINT8(flags, GnwCycleInsnMetadata),
        VMSTATE_UINT8(fallback_reason, GnwCycleInsnMetadata),
        VMSTATE_UINT8(apsr_reads, GnwCycleInsnMetadata),
        VMSTATE_UINT8(apsr_writes, GnwCycleInsnMetadata),
        VMSTATE_UINT8(memory_kind, GnwCycleInsnMetadata),
        VMSTATE_UINT8(memory_size, GnwCycleInsnMetadata),
        VMSTATE_UINT8_V(region_load_latency_class, GnwCycleInsnMetadata, 2),
        VMSTATE_UINT64(branch_target, GnwCycleInsnMetadata),
        VMSTATE_UINT64(memory_address, GnwCycleInsnMetadata),
        VMSTATE_UINT32(memory_attributes, GnwCycleInsnMetadata),
        VMSTATE_BOOL(branch_taken, GnwCycleInsnMetadata),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_gnw_cycle_store = {
    .name = "cpu/gnw-cycle-model/store",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(address, GnwCycleStoreVisibility),
        VMSTATE_UINT64(ready_fp, GnwCycleStoreVisibility),
        VMSTATE_UINT32(attributes, GnwCycleStoreVisibility),
        VMSTATE_UINT8(valid, GnwCycleStoreVisibility),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_gnw_dcache_line = {
    .name = "cpu/gnw-cycle-model/dcache-line",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(line_base, GnwM7DCacheLine),
        VMSTATE_BOOL(valid, GnwM7DCacheLine),
        VMSTATE_BOOL(dirty, GnwM7DCacheLine),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_gnw_dcache_set = {
    .name = "cpu/gnw-cycle-model/dcache-set",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(replacement_state, GnwM7DCacheSet),
        VMSTATE_UINT8(invalidated_way_mask, GnwM7DCacheSet),
        VMSTATE_UINT8(cleaned_way_mask, GnwM7DCacheSet),
        VMSTATE_UINT8(unclean_invalidated_way_mask, GnwM7DCacheSet),
        VMSTATE_BOOL(association_unknown, GnwM7DCacheSet),
        VMSTATE_BOOL(membership_unknown, GnwM7DCacheSet),
        VMSTATE_END_OF_LIST()
    },
};

static bool gnw_cycle_model_config_needed(void *opaque)
{
    return true;
}

/*
 * This subsection is unconditional so a current source with the producer
 * disabled cannot migrate into an enabled destination (or vice versa) merely
 * because the state-bearing subsection is absent.
 */
static const VMStateDescription vmstate_gnw_cycle_model_config = {
    .name = "cpu/gnw-cycle-model-config",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = gnw_cycle_model_config_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_EQUAL(gnw_m7_cycle_model_migration_config, ARMCPU,
                             "experimental cycle-model mode differs"),
        VMSTATE_END_OF_LIST()
    },
};

static bool gnw_cycle_model_needed(void *opaque)
{
    ARMCPU *cpu = opaque;

    return cpu->gnw_m7_cycle_model;
}

static int gnw_cycle_model_pre_save(void *opaque)
{
    ARMCPU *cpu = opaque;

    if (cpu->parent_obj.gnw_cycle_model.pending_valid ||
        cpu->parent_obj.gnw_cycle_model.exception_scoreboard_flush_pending ||
        cpu->parent_obj.gnw_cycle_model.xip_service_deferred_finish ||
        cpu->parent_obj.gnw_cycle_model.xip_service_remaining_cycles ||
        cpu->parent_obj.gnw_cycle_model.pending_bus_contention_cycles ||
        cpu->parent_obj.gnw_cycle_model.pending_nc_axi_fetch_cycles ||
        cpu->parent_obj.gnw_cycle_model.pending_icache_maintenance_cycles ||
        cpu->parent_obj.gnw_cycle_model.pending_fetch_residual_cycles ||
        cpu->parent_obj.gnw_cycle_model.pending_memory_residual_cycles ||
        cpu->parent_obj.gnw_cycle_model.nc_axi_fetch_attempt_active ||
        cpu->parent_obj.gnw_cycle_model.fetch_residual_attempt_active ||
        cpu->parent_obj.gnw_cycle_model.dcache.pending_count ||
        cpu->parent_obj.gnw_cycle_model.icache.pending_count ||
        cpu->parent_obj.gnw_cycle_model.icache.pending_cycles ||
        cpu->parent_obj.gnw_cycle_model.icache.attempt_active ||
        cpu->parent_obj.gnw_cycle_model.icache.pending_overflow ||
        cpu->parent_obj.gnw_cycle_model.flash_fetch.pending_count ||
        cpu->parent_obj.gnw_cycle_model.flash_fetch.pending_bytes) {
        error_report("cannot migrate a partial experimental cycle callback");
        return -EINVAL;
    }
    return 0;
}

static bool gnw_dcache_replacement_state_valid(uint8_t policy, uint8_t state)
{
    unsigned i, seen = 0;

    switch (policy) {
    case GNW_M7_DCACHE_OFF:
    case GNW_M7_DCACHE_TAGS_NO_REPLACEMENT:
        return state == 0;
    case GNW_M7_DCACHE_ROUND_ROBIN_HYPOTHESIS:
        return state <= 3;
    case GNW_M7_DCACHE_TREE_PLRU_HYPOTHESIS:
        return state <= 7;
    case GNW_M7_DCACHE_TRUE_LRU_HYPOTHESIS:
        for (i = 0; i < GNW_M7_DCACHE_WAYS; i++) {
            unsigned way = (state >> (i * 2)) & 3;

            if (seen & (1U << way)) {
                return false;
            }
            seen |= 1U << way;
        }
        return seen == 0xf;
    default:
        return false;
    }
}

static int gnw_cycle_model_post_load(void *opaque, int version_id)
{
    ARMCPU *cpu = opaque;
    GnwCycleModelState *s = &cpu->parent_obj.gnw_cycle_model;
    uint64_t valid = 0, dirty = 0, uncertain = 0;
    unsigned set, way, op;
    bool maintenance = false;

    /* The whole-loop classifier is diagnostic and deliberately absent from
     * VMState.  A destination may retain its locally configured immutable
     * identities, but no source-side match progress or counters may cross the
     * migration boundary. */
    memset(&s->flash_schedule, 0, sizeof(s->flash_schedule));
    memset(&s->flash_schedule_pending, 0,
           sizeof(s->flash_schedule_pending));
    s->flash_schedule_attempt_pending = false;
    memset(s->icache.pending, 0, sizeof(s->icache.pending));
    s->icache.pending_count = 0;
    s->icache.pending_cycles = 0;
    s->icache.attempt_active = false;
    s->icache.pending_overflow = false;
    if (version_id < 27) {
        s->icache.disabled_fetch_line = 0;
        s->icache.disabled_fetch_line_valid = false;
    }
    /* Streams predating VMState v25 did not carry I-cache shadow state.
     * Start them with a cold cache on the destination instead of preserving
     * any stale local tags or partial accounting. */
    if (version_id < 25) {
        s->icache.enabled = true;
        memset(s->icache.tags, 0, sizeof(s->icache.tags));
        memset(s->icache.valid, 0, sizeof(s->icache.valid));
        memset(s->icache.lru_way, 0, sizeof(s->icache.lru_way));
        s->icache.accesses = 0;
        s->icache.hits = 0;
        s->icache.misses = 0;
        s->icache.invalidations_all = 0;
        s->icache.invalidations_mva = 0;
    }
    if (version_id < 26) {
        s->xip_unseen_next_address = 0;
        s->xip_unseen_fraction = 0;
        s->xip_unseen_qualified_bytes = 0;
        s->xip_unseen_streams = 0;
        s->xip_unseen_stream_active = false;
    }
    s->nc_axi_fetch_saved_line = 0;
    s->nc_axi_fetch_saved_transitions = 0;
    s->nc_axi_fetch_saved_cycles = 0;
    s->nc_axi_fetch_saved_valid = false;
    s->nc_axi_fetch_attempt_active = false;
    s->fetch_residual_saved_fraction = 0;
    s->fetch_residual_saved_charges = 0;
    s->fetch_residual_saved_cycles = 0;
    s->fetch_residual_saved_pending_cycles = 0;
    s->fetch_residual_saved_set_conflict_active = false;
    s->fetch_residual_attempt_active = false;
    s->memory_residual_saved_base = 0;
    s->memory_residual_saved_entry_pc = 0;
    s->memory_residual_saved_bytes = 0;
    s->memory_residual_saved_stride = 0;
    s->memory_residual_saved_remaining = 0;
    s->memory_residual_saved_pending_cycles = 0;
    s->memory_residual_saved_charges = 0;
    s->memory_residual_saved_cycles = 0;
    s->memory_residual_saved_accepted = 0;
    s->memory_residual_saved_rejected = 0;
    s->memory_residual_saved_active = false;

    if (!cpu->gnw_m7_cycle_model ||
        !gnw_cycle_model_identity_valid(CPU(cpu))) {
        error_report("experimental cycle-model migration identity mismatch");
        return -EINVAL;
    }
    if (!gnw_cycle_model_cpu_ram_itcm_profile_valid(
            CPU(cpu), cpu->gnw_m7_cpu_ram_itcm_profile)) {
        error_report("experimental CPU RAM ITCM profile migration mismatch");
        return -EINVAL;
    }
    if (!gnw_cycle_model_exception_transition_profile_valid(
            CPU(cpu), cpu->gnw_m7_exception_transition_profile)) {
        error_report("experimental exception-transition profile migration "
                     "mismatch");
        return -EINVAL;
    }
    if (!gnw_cycle_model_fetch_residual_profile_valid(
            CPU(cpu), cpu->gnw_m7_fetch_residual_profile)) {
        error_report("experimental fetch-residual profile migration "
                     "mismatch");
        return -EINVAL;
    }
    if (!gnw_cycle_model_memory_residual_profile_valid(
            CPU(cpu), cpu->gnw_m7_memory_residual_profile)) {
        error_report("experimental memory-residual profile migration "
                     "mismatch");
        return -EINVAL;
    }
    if (!gnw_cycle_model_schedule_history_profile_valid(
            CPU(cpu), cpu->gnw_m7_schedule_history_profile)) {
        error_report("experimental schedule-history profile migration "
                     "mismatch");
        return -EINVAL;
    }
    if (!gnw_cycle_model_migration_scalars_valid(s)) {
        error_report("invalid experimental cycle-model scalar state");
        return -EINVAL;
    }
    if (s->flash_fetch.pending_count || s->flash_fetch.pending_bytes ||
        s->flash_fetch.valid_mask >> GNW_M7_FLASH_FETCH_LINES ||
        s->flash_fetch.next_replace >= GNW_M7_FLASH_FETCH_LINES ||
        s->flash_fetch.hits > s->flash_fetch.accesses ||
        s->flash_fetch.misses >
            s->flash_fetch.accesses - s->flash_fetch.hits) {
        error_report("invalid experimental internal-flash fetch state");
        return -EINVAL;
    }
    if ((s->nc_axi_fetch_valid &&
         (s->nc_axi_fetch_line < 0x24000000 ||
          s->nc_axi_fetch_line >= 0x24100000 ||
          (s->nc_axi_fetch_line & 31))) ||
        (!s->nc_axi_fetch_valid && s->nc_axi_fetch_line) ||
        s->pending_nc_axi_fetch_cycles) {
        error_report("invalid experimental noncacheable AXI fetch state");
        return -EINVAL;
    }
    for (way = 0; way < GNW_M7_FLASH_FETCH_LINES; way++) {
        uint64_t line = s->flash_fetch.lines[way];

        if (!(s->flash_fetch.valid_mask & (1U << way))) {
            if (line) {
                error_report("invalid experimental flash fetch line");
                return -EINVAL;
            }
        } else if (line < GNW_M7_INTERNAL_FLASH_BASE ||
                   line >= GNW_M7_INTERNAL_FLASH_END ||
                   (line & (GNW_M7_DCACHE_LINE_SIZE - 1))) {
            error_report("invalid experimental flash fetch tag");
            return -EINVAL;
        }
    }
    if (s->load_path.profile_identity !=
            (cpu->gnw_m7_load_path_candidate ?
             GNW_M7_LOAD_PATH_PROFILE_V5 : 0)) {
        error_report("experimental load-path profile migration mismatch");
        return -EINVAL;
    }
    if (!gnw_cycle_model_xip_service_profile_valid(CPU(cpu)) ||
        s->xip_service_profile_identity !=
            (cpu->gnw_m7_xip_service_test_profile ?
             GNW_M7_XIP_SERVICE_PROFILE_TEST_FIXED :
             (cpu->gnw_m7_xip_service_unseen_v1_profile ?
              GNW_M7_XIP_SERVICE_PROFILE_UNSEEN_V1 :
              GNW_M7_XIP_SERVICE_PROFILE_OFF)) ||
        s->xip_service_cursor_fp > s->cycle_fp ||
        (s->xip_service_profile_identity == GNW_M7_XIP_SERVICE_PROFILE_OFF &&
         (s->xip_service_cursor_fp || s->xip_service_requests ||
          s->xip_service_cycles || s->xip_service_remaining_cycles)) ||
        s->xip_service_remaining_cycles > s->xip_service_cycles ||
        s->xip_service_deferred_finish ||
        s->xip_service_remaining_cycles) {
        error_report("experimental XIP service profile migration mismatch");
        return -EINVAL;
    }
    if (s->dcache.policy != cpu->gnw_m7_dcache_policy ||
        s->dcache.policy >= GNW_M7_DCACHE_POLICY_COUNT ||
        s->dcache.pending_count) {
        error_report("experimental D-cache shadow migration identity mismatch");
        return -EINVAL;
    }
    uint32_t expected_bus_profile =
        cpu->gnw_m7_bus_contention_test_profile ?
        GNW_M7_BUS_CONTENTION_PROFILE_TEST_FIXED :
        (cpu->gnw_m7_bus_contention_mdma_axi1_profile ?
         GNW_M7_BUS_CONTENTION_PROFILE_MDMA_AXI1_V1 :
         (cpu->gnw_m7_bus_contention_v37_bwm_comparison ?
          GNW_M7_BUS_CONTENTION_PROFILE_V37_BWM_COMPARE :
          (cpu->gnw_m7_bus_contention_v46_bwm_17_4 ?
           GNW_M7_BUS_CONTENTION_PROFILE_V46_BWM_17_4 :
           GNW_M7_BUS_CONTENTION_PROFILE_OFF)));
    if (s->bus_contention_profile_identity != expected_bus_profile ||
        (s->bus_contention_profile_identity ==
             GNW_M7_BUS_CONTENTION_PROFILE_OFF &&
         (s->bus_contention_observations || s->bus_contention_matches ||
          s->bus_contention_cycles)) ||
        s->bus_contention_matches > s->bus_contention_observations ||
        s->pending_bus_contention_cycles) {
        error_report("experimental bus contention profile migration mismatch");
        return -EINVAL;
    }
    for (set = 0; set < GNW_M7_DCACHE_SETS; set++) {
        GnwM7DCacheSet *set_state = &s->dcache.sets[set];

        uncertain += set_state->association_unknown;
        if (!gnw_dcache_replacement_state_valid(s->dcache.policy,
                                                set_state->replacement_state) ||
            set_state->invalidated_way_mask & ~0xf ||
            set_state->cleaned_way_mask & ~0xf ||
            set_state->unclean_invalidated_way_mask & ~0xf ||
            (set_state->membership_unknown &&
             !set_state->association_unknown) ||
            (!set_state->association_unknown &&
             (set_state->invalidated_way_mask ||
              set_state->cleaned_way_mask ||
              set_state->unclean_invalidated_way_mask))) {
            error_report("invalid experimental D-cache shadow set state");
            return -EINVAL;
        }
        for (way = 0; way < GNW_M7_DCACHE_WAYS; way++) {
            GnwM7DCacheLine *line =
                &s->dcache.lines[set * GNW_M7_DCACHE_WAYS + way];
            unsigned earlier;

            if (!line->valid && (line->dirty || line->line_base)) {
                error_report("invalid experimental D-cache invalid line");
                return -EINVAL;
            }
            if (line->valid) {
                unsigned line_set;

                if (!((line->line_base >= 0x24000000ULL &&
                       line->line_base < 0x24100000ULL) ||
                      (line->line_base >= 0x90000000ULL &&
                       line->line_base < 0x94000000ULL)) ||
                    (line->line_base & 31)) {
                    error_report("invalid experimental D-cache line tag");
                    return -EINVAL;
                }
                line_set = (line->line_base >> 5) &
                           (GNW_M7_DCACHE_SETS - 1);
                if (line_set != set) {
                    error_report("misindexed experimental D-cache line");
                    return -EINVAL;
                }
                for (earlier = 0; earlier < way; earlier++) {
                    GnwM7DCacheLine *other =
                        &s->dcache.lines[set * GNW_M7_DCACHE_WAYS +
                                         earlier];

                    if (other->valid &&
                        other->line_base == line->line_base) {
                        error_report("duplicate experimental D-cache tag");
                        return -EINVAL;
                    }
                }
                valid++;
                dirty += line->dirty;
            }
        }
    }
    if (valid != s->dcache.valid_lines || dirty != s->dcache.dirty_lines ||
        uncertain != s->dcache.uncertain_sets) {
        error_report("experimental D-cache shadow counter mismatch");
        return -EINVAL;
    }
    for (op = 0; op <= GNW_CYCLE_CACHE_BPIALL; op++) {
        maintenance |= s->dcache.maintenance[op] != 0;
    }
    if (s->dcache.policy == GNW_M7_DCACHE_OFF &&
        (valid || dirty || uncertain ||
         s->dcache.instruction_cache_enabled ||
         s->dcache.data_cache_enabled || s->dcache.accesses ||
         s->dcache.hits || s->dcache.misses || s->dcache.allocations ||
         s->dcache.dirty_evictions || s->dcache.ambiguous_accesses ||
         s->dcache.rejected_disabled || s->dcache.rejected_path ||
         s->dcache.rejected_attributes || s->dcache.dirty_discards ||
         maintenance || s->dcache.external_events)) {
        error_report("disabled experimental D-cache shadow has state");
        return -EINVAL;
    }
    s->enabled = true;
    return 0;
}

static const VMStateDescription vmstate_gnw_cycle_model = {
    .name = "cpu/gnw-cycle-model",
    .version_id = 27,
    .minimum_version_id = 8,
    .needed = gnw_cycle_model_needed,
    .pre_save = gnw_cycle_model_pre_save,
    .post_load = gnw_cycle_model_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(parent_obj.gnw_cycle_model.previous, ARMCPU, 2,
                       vmstate_gnw_cycle_metadata, GnwCycleInsnMetadata),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.epoch_cycles, ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.cycle_fp, ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.previous_issue_fp, ARMCPU),
        VMSTATE_UINT64_ARRAY(parent_obj.gnw_cycle_model.gpr_ready_fp,
                             ARMCPU, 16),
        VMSTATE_UINT64_ARRAY(parent_obj.gnw_cycle_model.flags_ready_fp,
                             ARMCPU, 4),
        VMSTATE_UINT64_ARRAY(parent_obj.gnw_cycle_model.branch_flags_ready_fp,
                             ARMCPU, 4),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.commit_fraction_fp, ARMCPU),
        VMSTATE_STRUCT_ARRAY(parent_obj.gnw_cycle_model.store_visible, ARMCPU,
                             GNW_CYCLE_STORE_SLOTS, 1,
                             vmstate_gnw_cycle_store,
                             GnwCycleStoreVisibility),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.completed_instructions,
                       ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.modeled_whole_cycles,
                       ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.issued_pairs, ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.folded_nops, ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.aborted_attempts, ARMCPU),
        VMSTATE_UINT64_V(
            parent_obj.gnw_cycle_model.instruction_fetch_attempts,
            ARMCPU, 4),
        VMSTATE_UINT64_V(
            parent_obj.gnw_cycle_model.instruction_fetch_segments,
            ARMCPU, 4),
        VMSTATE_UINT64_V(
            parent_obj.gnw_cycle_model.instruction_fetch_bytes, ARMCPU, 4),
        VMSTATE_UINT64_V(
            parent_obj.gnw_cycle_model.instruction_fetch_incomplete,
            ARMCPU, 4),
        VMSTATE_UINT64_V(parent_obj.gnw_cycle_model.nc_axi_fetch_line,
                         ARMCPU, 12),
        VMSTATE_UINT64_V(parent_obj.gnw_cycle_model.nc_axi_fetch_transitions,
                         ARMCPU, 12),
        VMSTATE_UINT32_V(parent_obj.gnw_cycle_model.pending_nc_axi_fetch_cycles,
                         ARMCPU, 12),
        VMSTATE_BOOL_V(parent_obj.gnw_cycle_model.nc_axi_fetch_valid,
                       ARMCPU, 12),
        VMSTATE_UINT64_ARRAY_V(parent_obj.gnw_cycle_model.flash_fetch.lines,
                               ARMCPU, GNW_M7_FLASH_FETCH_LINES, 10),
        VMSTATE_UINT64_ARRAY_V(
            parent_obj.gnw_cycle_model.flash_fetch.phase_segments,
            ARMCPU, 8, 10),
        VMSTATE_UINT64_V(parent_obj.gnw_cycle_model.flash_fetch.accesses,
                         ARMCPU, 10),
        VMSTATE_UINT64_V(parent_obj.gnw_cycle_model.flash_fetch.hits,
                         ARMCPU, 10),
        VMSTATE_UINT64_V(parent_obj.gnw_cycle_model.flash_fetch.misses,
                         ARMCPU, 10),
        VMSTATE_UINT64_V(parent_obj.gnw_cycle_model.flash_fetch.bytes,
                         ARMCPU, 10),
        VMSTATE_UINT64_V(
            parent_obj.gnw_cycle_model.flash_fetch.invalidations_all,
            ARMCPU, 10),
        VMSTATE_UINT64_V(
            parent_obj.gnw_cycle_model.flash_fetch.invalidations_mva,
            ARMCPU, 10),
        VMSTATE_UINT8_V(parent_obj.gnw_cycle_model.flash_fetch.valid_mask,
                        ARMCPU, 10),
        VMSTATE_UINT8_V(parent_obj.gnw_cycle_model.flash_fetch.next_replace,
                        ARMCPU, 10),
        VMSTATE_UINT64_V(parent_obj.gnw_cycle_model.xip_service_cursor_fp,
                         ARMCPU, 5),
        VMSTATE_UINT64_V(parent_obj.gnw_cycle_model.xip_service_requests,
                         ARMCPU, 5),
        VMSTATE_UINT64_V(parent_obj.gnw_cycle_model.xip_service_cycles,
                         ARMCPU, 5),
        VMSTATE_UINT64_V(
            parent_obj.gnw_cycle_model.xip_service_remaining_cycles,
            ARMCPU, 6),
        VMSTATE_BOOL_V(parent_obj.gnw_cycle_model.xip_service_deferred_finish,
                       ARMCPU, 6),
        VMSTATE_UINT64_V(
            parent_obj.gnw_cycle_model.xip_unseen_next_address, ARMCPU, 26),
        VMSTATE_UINT64_V(
            parent_obj.gnw_cycle_model.xip_unseen_fraction, ARMCPU, 26),
        VMSTATE_UINT64_V(
            parent_obj.gnw_cycle_model.xip_unseen_qualified_bytes, ARMCPU, 26),
        VMSTATE_UINT64_V(
            parent_obj.gnw_cycle_model.xip_unseen_streams, ARMCPU, 26),
        VMSTATE_BOOL_V(
            parent_obj.gnw_cycle_model.xip_unseen_stream_active, ARMCPU, 26),
        VMSTATE_UINT64_V(
            parent_obj.gnw_cycle_model.icache.disabled_fetch_line, ARMCPU, 27),
        VMSTATE_BOOL_V(
            parent_obj.gnw_cycle_model.icache.disabled_fetch_line_valid,
            ARMCPU, 27),
        VMSTATE_UINT64_ARRAY(parent_obj.gnw_cycle_model.fallback, ARMCPU,
                             GNW_CYCLE_FALLBACK_COUNT),
        VMSTATE_UINT32_ARRAY(parent_obj.gnw_cycle_model.config_identity,
                             ARMCPU, 8),
        VMSTATE_UINT32(parent_obj.gnw_cycle_model.implementation_version,
                       ARMCPU),
        VMSTATE_UINT32_V(
            parent_obj.gnw_cycle_model.xip_service_profile_identity,
            ARMCPU, 5),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.bus_contention_observations,
                       ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.bus_contention_matches,
                       ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.bus_contention_cycles,
                       ARMCPU),
        VMSTATE_UINT32_V(parent_obj.gnw_cycle_model.bus_contention_fraction,
                         ARMCPU, 9),
        VMSTATE_UINT32(
            parent_obj.gnw_cycle_model.bus_contention_profile_identity,
            ARMCPU),
        VMSTATE_UINT32_V(
            parent_obj.gnw_cycle_model.cpu_ram_itcm_profile_identity,
            ARMCPU, 11),
        VMSTATE_UINT64_V(
            parent_obj.gnw_cycle_model.cpu_ram_itcm_profile_accepted,
            ARMCPU, 11),
        VMSTATE_UINT64_V(
            parent_obj.gnw_cycle_model.cpu_ram_itcm_profile_rejected,
            ARMCPU, 11),
        VMSTATE_UINT32_V(
            parent_obj.gnw_cycle_model.exception_transition_profile_identity,
            ARMCPU, 13),
        VMSTATE_UINT64_ARRAY_V(
            parent_obj.gnw_cycle_model.exception_transition_count,
            ARMCPU, GNW_CYCLE_EXCEPTION_TRANSITION_COUNT, 13),
        VMSTATE_UINT32_V(
            parent_obj.gnw_cycle_model.fetch_residual_profile_identity,
            ARMCPU, 14),
        VMSTATE_UINT64_V(
            parent_obj.gnw_cycle_model.fetch_residual_fraction, ARMCPU, 14),
        VMSTATE_UINT64_V(
            parent_obj.gnw_cycle_model.fetch_residual_charges, ARMCPU, 14),
        VMSTATE_UINT64_V(
            parent_obj.gnw_cycle_model.fetch_residual_cycles, ARMCPU, 14),
        VMSTATE_BOOL_V(
            parent_obj.gnw_cycle_model.fetch_residual_set_conflict_active,
            ARMCPU, 14),
        VMSTATE_UINT32_V(
            parent_obj.gnw_cycle_model.memory_residual_profile_identity,
            ARMCPU, 15),
        VMSTATE_UINT32_V(parent_obj.gnw_cycle_model.memory_residual_base,
                         ARMCPU, 15),
        VMSTATE_UINT64_V(parent_obj.gnw_cycle_model.memory_residual_entry_pc,
                         ARMCPU, 23),
        VMSTATE_UINT32_V(parent_obj.gnw_cycle_model.memory_residual_bytes,
                         ARMCPU, 15),
        VMSTATE_UINT32_V(parent_obj.gnw_cycle_model.memory_residual_stride,
                         ARMCPU, 15),
        VMSTATE_UINT32_V(parent_obj.gnw_cycle_model.memory_residual_remaining,
                         ARMCPU, 15),
        VMSTATE_UINT64_V(parent_obj.gnw_cycle_model.memory_residual_charges,
                         ARMCPU, 15),
        VMSTATE_UINT64_V(parent_obj.gnw_cycle_model.memory_residual_cycles,
                         ARMCPU, 15),
        VMSTATE_UINT64_V(parent_obj.gnw_cycle_model.memory_residual_accepted,
                         ARMCPU, 15),
        VMSTATE_UINT64_V(parent_obj.gnw_cycle_model.memory_residual_rejected,
                         ARMCPU, 15),
        VMSTATE_BOOL_V(parent_obj.gnw_cycle_model.memory_residual_active,
                       ARMCPU, 15),
        VMSTATE_UINT32_V(
            parent_obj.gnw_cycle_model.schedule_history_profile_identity,
            ARMCPU, 24),
        VMSTATE_UINT32_V(
            parent_obj.gnw_cycle_model.schedule_history_outcomes, ARMCPU, 24),
        VMSTATE_UINT32_V(
            parent_obj.gnw_cycle_model.schedule_history_credit_cycles,
            ARMCPU, 24),
        VMSTATE_UINT32_V(
            parent_obj.gnw_cycle_model.schedule_history_applied_credit_cycles,
            ARMCPU, 24),
        VMSTATE_UINT32_V(
            parent_obj.gnw_cycle_model.schedule_history_compensation_cycles,
            ARMCPU, 24),
        VMSTATE_UINT64_V(
            parent_obj.gnw_cycle_model.schedule_history_accepted, ARMCPU, 24),
        VMSTATE_UINT64_V(
            parent_obj.gnw_cycle_model.schedule_history_rejected, ARMCPU, 24),
        VMSTATE_UINT8_V(
            parent_obj.gnw_cycle_model.schedule_history_pattern, ARMCPU, 24),
        VMSTATE_UINT8_V(
            parent_obj.gnw_cycle_model.schedule_history_kind, ARMCPU, 24),
        VMSTATE_BOOL_V(
            parent_obj.gnw_cycle_model.schedule_history_active, ARMCPU, 24),
        VMSTATE_BOOL(parent_obj.gnw_cycle_model.previous_valid, ARMCPU),
        VMSTATE_BOOL(parent_obj.gnw_cycle_model.previous_was_store, ARMCPU),
        VMSTATE_BOOL(parent_obj.gnw_cycle_model.disfold_set, ARMCPU),
        VMSTATE_BOOL(parent_obj.gnw_cycle_model.test_near_rebase, ARMCPU),
        VMSTATE_UINT64_ARRAY(
            parent_obj.gnw_cycle_model.load_path.gpr_ready_fp, ARMCPU, 16),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.load_path.issue_next_fp,
                       ARMCPU),
        VMSTATE_UINT64(
            parent_obj.gnw_cycle_model.load_path.miss_slot_ready_fp, ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.load_path.critical_path_fp,
                       ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.load_path.epoch_cycles,
                       ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.load_path.last_issue_fp,
                       ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.load_path.last_ready_fp,
                       ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.load_path.accepted, ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.load_path.cache_hits,
                       ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.load_path.cache_misses,
                       ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.load_path.rejected_shape,
                       ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.load_path.rejected_region,
                       ARMCPU),
        VMSTATE_UINT32(parent_obj.gnw_cycle_model.load_path.profile_identity,
                       ARMCPU),
        VMSTATE_UINT8(parent_obj.gnw_cycle_model.dcache.policy, ARMCPU),
        VMSTATE_BOOL(
            parent_obj.gnw_cycle_model.dcache.instruction_cache_enabled,
            ARMCPU),
        VMSTATE_BOOL(parent_obj.gnw_cycle_model.dcache.data_cache_enabled,
                     ARMCPU),
        VMSTATE_STRUCT_ARRAY(parent_obj.gnw_cycle_model.dcache.lines, ARMCPU,
                             GNW_M7_DCACHE_LINES, 1,
                             vmstate_gnw_dcache_line, GnwM7DCacheLine),
        VMSTATE_STRUCT_ARRAY(parent_obj.gnw_cycle_model.dcache.sets, ARMCPU,
                             GNW_M7_DCACHE_SETS, 1,
                             vmstate_gnw_dcache_set, GnwM7DCacheSet),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.dcache.valid_lines, ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.dcache.dirty_lines, ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.dcache.uncertain_sets,
                       ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.dcache.accesses, ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.dcache.hits, ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.dcache.misses, ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.dcache.allocations, ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.dcache.dirty_evictions,
                       ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.dcache.ambiguous_accesses,
                       ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.dcache.rejected_disabled,
                       ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.dcache.rejected_path, ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.dcache.rejected_attributes,
                       ARMCPU),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.dcache.dirty_discards,
                       ARMCPU),
        VMSTATE_UINT64_ARRAY(parent_obj.gnw_cycle_model.dcache.maintenance,
                             ARMCPU, GNW_CYCLE_CACHE_BPIALL + 1),
        VMSTATE_UINT64(parent_obj.gnw_cycle_model.dcache.external_events,
                       ARMCPU),
        VMSTATE_UINT64_ARRAY_V(parent_obj.gnw_cycle_model.icache.tags, ARMCPU,
                               GNW_M7_ICACHE_SETS * GNW_M7_ICACHE_WAYS, 25),
        VMSTATE_UINT8_ARRAY_V(parent_obj.gnw_cycle_model.icache.valid, ARMCPU,
                              GNW_M7_ICACHE_SETS, 25),
        VMSTATE_UINT8_ARRAY_V(parent_obj.gnw_cycle_model.icache.lru_way, ARMCPU,
                              GNW_M7_ICACHE_SETS, 25),
        VMSTATE_UINT64_V(parent_obj.gnw_cycle_model.icache.accesses, ARMCPU,
                         25),
        VMSTATE_UINT64_V(parent_obj.gnw_cycle_model.icache.hits, ARMCPU, 25),
        VMSTATE_UINT64_V(parent_obj.gnw_cycle_model.icache.misses, ARMCPU, 25),
        VMSTATE_UINT64_V(parent_obj.gnw_cycle_model.icache.invalidations_all,
                         ARMCPU, 25),
        VMSTATE_UINT64_V(parent_obj.gnw_cycle_model.icache.invalidations_mva,
                         ARMCPU, 25),
        VMSTATE_BOOL_V(parent_obj.gnw_cycle_model.icache.enabled, ARMCPU, 25),
        VMSTATE_END_OF_LIST()
    },
};

const VMStateDescription vmstate_arm_cpu = {
    .name = "cpu",
    .version_id = 23,
    .minimum_version_id = 23,
    .pre_save = cpu_pre_save,
    .post_save = cpu_post_save,
    .pre_load = cpu_pre_load,
    .post_load = cpu_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(env.regs, ARMCPU, 16),
        VMSTATE_UINT64_ARRAY(env.xregs, ARMCPU, 32),
        VMSTATE_UINT64(env.pc, ARMCPU),
        /*
         * If any bits are set in the upper 32 bits of cpsr/pstate,
         * or if the cpu is in aa32 mode and PSTATE.SS is set, then
         * the cpu/pstate64 subsection will override this with the
         * full 64 bit state.
         */
        {
            .name = "cpsr",
            .version_id = 0,
            .size = sizeof(uint32_t),
            .info = &vmstate_cpsr,
            .flags = VMS_SINGLE,
            .offset = 0,
        },
        VMSTATE_UINT32(env.spsr, ARMCPU),
        VMSTATE_UINT64_ARRAY(env.banked_spsr, ARMCPU, 8),
        VMSTATE_UINT32_ARRAY(env.banked_r13, ARMCPU, 8),
        VMSTATE_UINT32_ARRAY(env.banked_r14, ARMCPU, 8),
        VMSTATE_UINT32_ARRAY(env.usr_regs, ARMCPU, 5),
        VMSTATE_UINT32_ARRAY(env.fiq_regs, ARMCPU, 5),
        VMSTATE_UINT64_ARRAY(env.elr_el, ARMCPU, 4),
        VMSTATE_UINT64_ARRAY(env.sp_el, ARMCPU, 4),
        /*
         * The length must come before the arrays so we can
         * allocate the arrays before their data arrives
         */
        VMSTATE_INT32(cpreg_vmstate_array_len, ARMCPU),
        VMSTATE_VARRAY_INT32_ALLOC(cpreg_vmstate_indexes, ARMCPU,
                                   cpreg_vmstate_array_len,
                                   0, vmstate_info_uint64, uint64_t),
        VMSTATE_VARRAY_INT32_ALLOC(cpreg_vmstate_values, ARMCPU,
                                   cpreg_vmstate_array_len,
                                   0, vmstate_info_uint64, uint64_t),
        VMSTATE_UINT64(env.exclusive_addr, ARMCPU),
        VMSTATE_UINT64(env.exclusive_val, ARMCPU),
        VMSTATE_UINT64(env.exclusive_high, ARMCPU),
        VMSTATE_UNUSED(sizeof(uint64_t)),
        /*
         * If any bits are set in the upper 32 bits of syndrome,
         * then the cpu/syndrome64 subsection will override this
         * with the full 64 bit state.
         */
        {
            .name = "env.exception.syndrome",
            .version_id = 0,
            .size = sizeof(uint32_t),
            .info = &vmstate_info_uint32,
            .flags = VMS_SINGLE,
            .offset = offsetoflow32(ARMCPU, env.exception.syndrome),
        },
        VMSTATE_UINT32(env.exception.fsr, ARMCPU),
        VMSTATE_UINT64(env.exception.vaddress, ARMCPU),
        VMSTATE_TIMER_PTR(gt_timer[GTIMER_PHYS], ARMCPU),
        VMSTATE_TIMER_PTR(gt_timer[GTIMER_VIRT], ARMCPU),
        {
            .name = "power_state",
            .version_id = 0,
            .size = sizeof(bool),
            .info = &vmstate_powered_off,
            .flags = VMS_SINGLE,
            .offset = 0,
        },
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_vfp,
        &vmstate_m,
        &vmstate_thumb2ee,
        /* pmsav7_rnr must come before pmsav7 so that we have the
         * region number before we test it in the VMSTATE_VALIDATE
         * in vmstate_pmsav7.
         */
        &vmstate_pmsav7_rnr,
        &vmstate_pmsav7,
        &vmstate_pmsav8,
        &vmstate_m_security,
        &vmstate_sve,
        &vmstate_za,
        &vmstate_zt0,
        &vmstate_serror,
        &vmstate_irq_line_state,
        &vmstate_wfxt_timer,
        &vmstate_syndrome64,
        &vmstate_pstate64,
        &vmstate_event,
        &vmstate_gnw_cycle_model_config,
        &vmstate_gnw_cycle_model,
        NULL
    }
};

const InterfaceInfo arm_machine_interfaces[] = {
    { TYPE_TARGET_ARM_MACHINE },
    { TYPE_TARGET_AARCH64_MACHINE },
    { }
};

const InterfaceInfo arm_aarch64_machine_interfaces[] = {
    { TYPE_TARGET_ARM_MACHINE },
    { TYPE_TARGET_AARCH64_MACHINE },
    { }
};

const InterfaceInfo aarch64_machine_interfaces[] = {
    { TYPE_TARGET_AARCH64_MACHINE },
    { }
};

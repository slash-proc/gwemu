/*
 * Inert representation boundary for reviewed Cortex-M7 timing profiles.
 *
 * This header installs no profile and defines no calibrated value.  A future
 * consumer must separately bind a reviewed receipt before copying a validated
 * candidate into live cycle-model state.
 */
#ifndef EXEC_GNW_M7_RUNTIME_PROFILE_H
#define EXEC_GNW_M7_RUNTIME_PROFILE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define GNW_M7_RUNTIME_DIGEST_BYTES 32
#define GNW_M7_INTERNAL_FLASH_PHASE_CELLS 8
#define GNW_M7_INTERNAL_FLASH_FIRST 0x08000000ULL
#define GNW_M7_INTERNAL_FLASH_END   0x08140000ULL

typedef struct GnwM7RuntimeProfileBinding {
    uint8_t artifact_sha256[GNW_M7_RUNTIME_DIGEST_BYTES];
    uint8_t implementation_sha256[GNW_M7_RUNTIME_DIGEST_BYTES];
    uint8_t reviewed_commit[20];
    uint32_t schema_version;
    uint32_t profile_identity;
} GnwM7RuntimeProfileBinding;

/* Live-state ABI reservation for future reviewed flash profiles.  The only
 * state admitted today is the all-zero OFF representation.  Candidate
 * validation alone must never populate this structure. */
typedef struct GnwM7ProductionFlashProfileState {
    GnwM7RuntimeProfileBinding internal_flash;
    GnwM7RuntimeProfileBinding ospi_xip;
    bool internal_flash_admitted;
    bool ospi_xip_admitted;
} GnwM7ProductionFlashProfileState;

typedef struct GnwM7ExactRational {
    uint64_t numerator;
    uint64_t denominator;
} GnwM7ExactRational;

/* Signed form used by fitted affine coefficients.  The JSON candidate ABI
 * permits finite negative intercepts, so representing the numerator as
 * unsigned would silently change the frozen contract. */
typedef struct GnwM7SignedRational {
    int64_t numerator;
    uint64_t denominator;
} GnwM7SignedRational;

typedef struct GnwM7LinearTimingTerm {
    GnwM7SignedRational setup_cycles;
    GnwM7SignedRational cycles_per_unit;
} GnwM7LinearTimingTerm;

typedef enum GnwM7CpuGapProfileIndex {
    GNW_M7_CPU_GAP0,
    GNW_M7_CPU_GAP1,
    GNW_M7_CPU_GAP3,
    GNW_M7_CPU_GAP_COUNT,
} GnwM7CpuGapProfileIndex;

typedef enum GnwM7MemoryProfileIndex {
    GNW_M7_MEMORY_DTCM_STRIDE1,
    GNW_M7_MEMORY_AXI_STRIDE1,
    GNW_M7_MEMORY_AHB_STRIDE1,
    GNW_M7_MEMORY_AHB_STRIDE33,
    GNW_M7_MEMORY_PROFILE_COUNT,
} GnwM7MemoryProfileIndex;

typedef enum GnwM7InstructionFetchProfileIndex {
    GNW_M7_IFETCH_HOT_ALIGNED,
    GNW_M7_IFETCH_COLD_ALIGNED,
    GNW_M7_IFETCH_CONFLICT_OFFSET46,
    GNW_M7_IFETCH_PROFILE_COUNT,
} GnwM7InstructionFetchProfileIndex;

/* Exact C shape of gnw-h7b0-cpu-memory-cache-dma2d-coefficients-v1.
 * This is storage/validation only.  No cycle-model code consumes it. */
typedef struct GnwM7CpuMemoryCacheDma2dProfileCandidate {
    GnwM7RuntimeProfileBinding binding;
    GnwM7LinearTimingTerm cpu_gap[GNW_M7_CPU_GAP_COUNT];
    GnwM7LinearTimingTerm memory[GNW_M7_MEMORY_PROFILE_COUNT];
    GnwM7LinearTimingTerm instruction_fetch[GNW_M7_IFETCH_PROFILE_COUNT];
    GnwM7SignedRational dma2d_setup_cycles;
    GnwM7SignedRational dma2d_cycles_per_pixel;
    GnwM7SignedRational dma2d_cycles_per_line;
} GnwM7CpuMemoryCacheDma2dProfileCandidate;

#define GNW_M7_EXACT_BODY_MAX_BYTES 32
#define GNW_M7_FLASH_SCHEDULE_BODY_BYTES 32
#define GNW_M7_FLASH_SCHEDULE_IDENTITIES 8

typedef enum GnwM7ExactBodyRegion {
    GNW_M7_EXACT_BODY_REGION_NONE,
    GNW_M7_EXACT_BODY_REGION_ITCM,
    GNW_M7_EXACT_BODY_REGION_AXI_SRAM_PRIMED,
} GnwM7ExactBodyRegion;

/* Byte/address matcher boundary for a future exact-body consumer.  The
 * caller must independently prove the execution region and cache state.
 * Matching has no timing side effect and carries no authorization. */
typedef struct GnwM7ExactBodyIdentity {
    uint8_t artifact_sha256[GNW_M7_RUNTIME_DIGEST_BYTES];
    uint8_t bytes[GNW_M7_EXACT_BODY_MAX_BYTES];
    uint64_t virtual_address;
    uint64_t physical_address;
    uint8_t size;
    uint8_t region;
    uint8_t reserved[6];
} GnwM7ExactBodyIdentity;

typedef struct GnwM7ExactBodyMatchState {
    uint8_t artifact_sha256[GNW_M7_RUNTIME_DIGEST_BYTES];
    uint64_t virtual_address;
    uint64_t physical_address;
    uint8_t next_offset;
    uint8_t region;
    bool active;
    bool rejected;
    bool region_state_verified;
    uint8_t reserved[3];
} GnwM7ExactBodyMatchState;

/* Exact whole-loop identity for diagnostic schedule classification.  This
 * carries no timing coefficient and cannot authorize a runtime profile. */
typedef struct GnwM7FlashScheduleIdentity {
    uint8_t artifact_sha256[GNW_M7_RUNTIME_DIGEST_BYTES];
    uint8_t bytes[GNW_M7_FLASH_SCHEDULE_BODY_BYTES];
    uint64_t virtual_address;
    uint64_t physical_address;
    uint32_t classifier_identity;
    uint8_t schedule_kind;
    uint8_t loop_start_phase;
    uint8_t role;
    uint8_t reserved;
} GnwM7FlashScheduleIdentity;

static inline bool gnw_m7_flash_schedule_identity_valid(
    const GnwM7FlashScheduleIdentity *identity)
{
    uint8_t digest = 0;
    unsigned i;

    if (!identity) {
        return false;
    }
    for (i = 0; i < GNW_M7_RUNTIME_DIGEST_BYTES; i++) {
        digest |= identity->artifact_sha256[i];
    }
    return identity->classifier_identity && digest &&
           (identity->schedule_kind == 5 || identity->schedule_kind == 6) &&
           identity->role <= 1 &&
           (identity->loop_start_phase == 2 ||
            identity->loop_start_phase == 6) &&
           (identity->physical_address & 7) == identity->loop_start_phase &&
           identity->physical_address >= GNW_M7_INTERNAL_FLASH_FIRST &&
           identity->physical_address <=
               GNW_M7_INTERNAL_FLASH_END - GNW_M7_FLASH_SCHEDULE_BODY_BYTES &&
           identity->virtual_address <=
               UINT64_MAX - GNW_M7_FLASH_SCHEDULE_BODY_BYTES;
}

static inline bool gnw_m7_flash_schedule_identities_valid(
    const GnwM7FlashScheduleIdentity
        identities[GNW_M7_FLASH_SCHEDULE_IDENTITIES])
{
    uint8_t seen = 0;
    unsigned i, j;

    if (!identities) {
        return false;
    }
    for (i = 0; i < GNW_M7_FLASH_SCHEDULE_IDENTITIES; i++) {
        unsigned key;

        if (!gnw_m7_flash_schedule_identity_valid(&identities[i]) ||
            identities[i].classifier_identity !=
                identities[0].classifier_identity) {
            return false;
        }
        key = identities[i].role * 4 +
              (identities[i].schedule_kind - 5) * 2 +
              (identities[i].loop_start_phase == 6);
        if (seen & (1U << key)) {
            return false;
        }
        seen |= 1U << key;
        for (j = 0; j < i; j++) {
            if (identities[i].virtual_address ==
                    identities[j].virtual_address &&
                identities[i].physical_address ==
                    identities[j].physical_address) {
                return false;
            }
        }
    }
    return seen == UINT8_MAX;
}

/* Representation required by the reviewed OSPI production-profile design.
 * Controller, memory-attribute, clock, and flash-identity predicates remain
 * in the hashed scope artifact; this candidate cannot activate itself. */
typedef struct GnwM7XipWindowProfileCandidate {
    GnwM7RuntimeProfileBinding binding;
    uint64_t first_address;
    uint64_t end_address;
    uint32_t window_bytes;
    uint32_t required_pmsa_attributes;
    uint32_t required_hclk_hz;
    uint32_t required_ospi_ccr;
    uint32_t required_ospi_tcr;
    uint32_t required_ospi_dcr2;
    uint32_t required_flash_jedec_id;
    bool require_dcache_disabled;
    GnwM7ExactRational stream_intercept_cycles;
    GnwM7ExactRational cycles_per_byte;
    GnwM7ExactRational cycles_per_window;
} GnwM7XipWindowProfileCandidate;

/* Versioned fields a future VMState subsection must preserve.  Keeping this
 * type inert avoids changing today's migration stream before authorization. */
typedef struct GnwM7XipWindowStreamState {
    uint8_t artifact_sha256[GNW_M7_RUNTIME_DIGEST_BYTES];
    uint32_t profile_identity;
    uint64_t previous_end_address;
    uint64_t current_window_address;
    uint64_t fractional_remainder;
    bool active;
    bool intercept_paid;
} GnwM7XipWindowStreamState;

typedef struct GnwM7XipWindowObservation {
    uint64_t address;
    uint32_t size;
    uint32_t pmsa_attributes;
    uint32_t hclk_hz;
    uint32_t ospi_ccr;
    uint32_t ospi_tcr;
    uint32_t ospi_dcr2;
    uint32_t flash_jedec_id;
    bool dcache_enabled;
    bool completed;
    bool plain_load;
    bool single_access;
    bool instruction_fetch;
} GnwM7XipWindowObservation;

typedef struct GnwM7InternalFlashPhaseCell {
    uint8_t runtime_expression_sha256[GNW_M7_RUNTIME_DIGEST_BYTES];
    uint8_t schedule_kind;
    uint8_t phase_mod8;
    bool disfold_set;
} GnwM7InternalFlashPhaseCell;

/* Binds reviewed aggregate mappings without interpreting them as fetch costs.
 * No live consumer exists until schedule_classifier_identity names a
 * separately reviewed runtime classifier. */
typedef struct GnwM7InternalFlashPhaseProfileCandidate {
    GnwM7RuntimeProfileBinding binding;
    uint8_t schedule_classifier_sha256[GNW_M7_RUNTIME_DIGEST_BYTES];
    uint32_t schedule_classifier_identity;
    uint64_t first_address;
    uint64_t end_address;
    GnwM7InternalFlashPhaseCell cells[GNW_M7_INTERNAL_FLASH_PHASE_CELLS];
} GnwM7InternalFlashPhaseProfileCandidate;

/* Diagnostic result for bridging a real instruction-fetch observation to a
 * reviewed phase candidate.  UNCLASSIFIED is deliberately distinct from a
 * predicate mismatch: address, size, phase and DISFOLD are observable, but
 * the phase-v2 schedule kind is not present in QEMU's fetch metadata. */
typedef enum GnwM7InternalFlashPhaseDiagnostic {
    GNW_M7_INTERNAL_FLASH_PHASE_INVALID_PROFILE,
    GNW_M7_INTERNAL_FLASH_PHASE_OUTSIDE_FLASH,
    GNW_M7_INTERNAL_FLASH_PHASE_UNMEASURED,
    GNW_M7_INTERNAL_FLASH_PHASE_SCHEDULE_UNCLASSIFIED,
    GNW_M7_INTERNAL_FLASH_PHASE_PREDICATE_MISMATCH,
    GNW_M7_INTERNAL_FLASH_PHASE_PREDICATE_MATCH,
} GnwM7InternalFlashPhaseDiagnostic;

static inline bool gnw_m7_runtime_digest_nonzero(const uint8_t *digest)
{
    uint8_t combined = 0;
    unsigned i;

    for (i = 0; i < GNW_M7_RUNTIME_DIGEST_BYTES; i++) {
        combined |= digest[i];
    }
    return combined != 0;
}

static inline bool gnw_m7_reviewed_commit_nonzero(const uint8_t *commit)
{
    uint8_t combined = 0;
    unsigned i;

    for (i = 0; i < 20; i++) {
        combined |= commit[i];
    }
    return combined != 0;
}

static inline bool gnw_m7_exact_rational_valid(GnwM7ExactRational value)
{
    return value.denominator != 0;
}

static inline bool gnw_m7_runtime_binding_valid(
    const GnwM7RuntimeProfileBinding *binding)
{
    return binding && binding->schema_version != 0 &&
           binding->profile_identity != 0 &&
           gnw_m7_runtime_digest_nonzero(binding->artifact_sha256) &&
           gnw_m7_runtime_digest_nonzero(binding->implementation_sha256) &&
           gnw_m7_reviewed_commit_nonzero(binding->reviewed_commit);
}

static inline bool gnw_m7_runtime_binding_off(
    const GnwM7RuntimeProfileBinding *binding)
{
    unsigned i;

    if (!binding || binding->schema_version || binding->profile_identity) {
        return false;
    }
    for (i = 0; i < GNW_M7_RUNTIME_DIGEST_BYTES; i++) {
        if (binding->artifact_sha256[i] ||
            binding->implementation_sha256[i]) {
            return false;
        }
    }
    for (i = 0; i < sizeof(binding->reviewed_commit); i++) {
        if (binding->reviewed_commit[i]) {
            return false;
        }
    }
    return true;
}

static inline bool gnw_m7_production_flash_profiles_off(
    const GnwM7ProductionFlashProfileState *state)
{
    return state && !state->internal_flash_admitted &&
           !state->ospi_xip_admitted &&
           gnw_m7_runtime_binding_off(&state->internal_flash) &&
           gnw_m7_runtime_binding_off(&state->ospi_xip);
}

static inline bool gnw_m7_signed_rational_valid(GnwM7SignedRational value)
{
    return value.denominator != 0;
}

static inline bool gnw_m7_linear_timing_term_valid(GnwM7LinearTimingTerm value)
{
    return gnw_m7_signed_rational_valid(value.setup_cycles) &&
           gnw_m7_signed_rational_valid(value.cycles_per_unit);
}

static inline bool gnw_m7_exact_body_identity_valid(
    const GnwM7ExactBodyIdentity *identity)
{
    return identity &&
           gnw_m7_runtime_digest_nonzero(identity->artifact_sha256) &&
           identity->size != 0 &&
           identity->size <= GNW_M7_EXACT_BODY_MAX_BYTES &&
           identity->virtual_address <= UINT64_MAX - identity->size &&
           identity->physical_address <= UINT64_MAX - identity->size &&
           (identity->region == GNW_M7_EXACT_BODY_REGION_ITCM ||
            identity->region == GNW_M7_EXACT_BODY_REGION_AXI_SRAM_PRIMED);
}

static inline void gnw_m7_exact_body_match_begin(
    GnwM7ExactBodyMatchState *state,
    const GnwM7ExactBodyIdentity *identity,
    bool region_state_verified)
{
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    if (!gnw_m7_exact_body_identity_valid(identity) ||
        !region_state_verified) {
        state->rejected = true;
        return;
    }
    memcpy(state->artifact_sha256, identity->artifact_sha256,
           sizeof(state->artifact_sha256));
    state->virtual_address = identity->virtual_address;
    state->physical_address = identity->physical_address;
    state->region = identity->region;
    state->region_state_verified = true;
    state->active = true;
}

static inline bool gnw_m7_exact_body_match_bytes(
    GnwM7ExactBodyMatchState *state,
    const GnwM7ExactBodyIdentity *identity,
    uint64_t virtual_address, uint64_t physical_address,
    const uint8_t *bytes, uint8_t size)
{
    uint8_t difference = 0;
    unsigned i;

    if (!state || !identity || !bytes || !size || !state->active ||
        state->rejected || !state->region_state_verified ||
        state->region != identity->region ||
        state->next_offset > identity->size ||
        size > identity->size - state->next_offset ||
        virtual_address != state->virtual_address + state->next_offset ||
        physical_address != state->physical_address + state->next_offset) {
        if (state) {
            state->active = false;
            state->rejected = true;
        }
        return false;
    }
    for (i = 0; i < GNW_M7_RUNTIME_DIGEST_BYTES; i++) {
        difference |= state->artifact_sha256[i] ^ identity->artifact_sha256[i];
    }
    for (i = 0; i < size; i++) {
        difference |= bytes[i] ^ identity->bytes[state->next_offset + i];
    }
    if (difference) {
        state->active = false;
        state->rejected = true;
        return false;
    }
    state->next_offset += size;
    return state->next_offset == identity->size;
}

static inline bool gnw_m7_cpu_memory_cache_dma2d_candidate_valid(
    const GnwM7CpuMemoryCacheDma2dProfileCandidate *profile)
{
    unsigned i;

    if (!profile || !gnw_m7_runtime_binding_valid(&profile->binding)) {
        return false;
    }
    for (i = 0; i < GNW_M7_CPU_GAP_COUNT; i++) {
        if (!gnw_m7_linear_timing_term_valid(profile->cpu_gap[i])) {
            return false;
        }
    }
    for (i = 0; i < GNW_M7_MEMORY_PROFILE_COUNT; i++) {
        if (!gnw_m7_linear_timing_term_valid(profile->memory[i])) {
            return false;
        }
    }
    for (i = 0; i < GNW_M7_IFETCH_PROFILE_COUNT; i++) {
        if (!gnw_m7_linear_timing_term_valid(profile->instruction_fetch[i])) {
            return false;
        }
    }
    return gnw_m7_signed_rational_valid(profile->dma2d_setup_cycles) &&
           gnw_m7_signed_rational_valid(profile->dma2d_cycles_per_pixel) &&
           gnw_m7_signed_rational_valid(profile->dma2d_cycles_per_line);
}

static inline bool gnw_m7_xip_window_candidate_valid(
    const GnwM7XipWindowProfileCandidate *profile)
{
    const GnwM7RuntimeProfileBinding *binding;

    if (!profile) {
        return false;
    }
    binding = &profile->binding;
    return gnw_m7_runtime_binding_valid(binding) &&
           profile->first_address < profile->end_address &&
           profile->window_bytes != 0 &&
           (profile->window_bytes & (profile->window_bytes - 1)) == 0 &&
           (profile->first_address & (profile->window_bytes - 1)) == 0 &&
           (profile->end_address & (profile->window_bytes - 1)) == 0 &&
           profile->required_hclk_hz != 0 &&
           profile->required_flash_jedec_id != 0 &&
           profile->require_dcache_disabled &&
           gnw_m7_exact_rational_valid(profile->stream_intercept_cycles) &&
           gnw_m7_exact_rational_valid(profile->cycles_per_byte) &&
           gnw_m7_exact_rational_valid(profile->cycles_per_window) &&
           profile->stream_intercept_cycles.denominator ==
               profile->cycles_per_byte.denominator &&
           profile->cycles_per_byte.denominator ==
               profile->cycles_per_window.denominator;
}

static inline bool gnw_m7_xip_window_observation_eligible(
    const GnwM7XipWindowProfileCandidate *profile,
    const GnwM7XipWindowObservation *observation)
{
    if (!gnw_m7_xip_window_candidate_valid(profile) || !observation ||
        !observation->size || !observation->completed ||
        !observation->plain_load || !observation->single_access ||
        observation->instruction_fetch || observation->dcache_enabled ||
        observation->address < profile->first_address ||
        observation->address >= profile->end_address ||
        observation->size > profile->end_address - observation->address) {
        return false;
    }
    return observation->pmsa_attributes ==
               profile->required_pmsa_attributes &&
           observation->hclk_hz == profile->required_hclk_hz &&
           observation->ospi_ccr == profile->required_ospi_ccr &&
           observation->ospi_tcr == profile->required_ospi_tcr &&
           observation->ospi_dcr2 == profile->required_ospi_dcr2 &&
           observation->flash_jedec_id == profile->required_flash_jedec_id;
}

/* Evaluate one stream fragment without rounding individual guest accesses.
 * The caller owns stream qualification and supplies whether this fragment
 * starts a new stream.  Remainder is carried in units of the common profile
 * denominator. */
static inline bool gnw_m7_xip_window_charge(
    const GnwM7XipWindowProfileCandidate *profile,
    bool new_stream, uint64_t bytes, uint64_t new_windows,
    uint64_t prior_remainder, uint64_t *whole_cycles,
    uint64_t *next_remainder)
{
    unsigned __int128 scaled;
    unsigned __int128 term;
    const unsigned __int128 maximum = ~(unsigned __int128)0;
    uint64_t denominator;
    unsigned __int128 whole;

    if (!whole_cycles || !next_remainder ||
        !gnw_m7_xip_window_candidate_valid(profile)) {
        return false;
    }
    denominator = profile->cycles_per_byte.denominator;
    if (prior_remainder >= denominator) {
        return false;
    }
    scaled = prior_remainder;
    if (new_stream) {
        if (profile->stream_intercept_cycles.numerator > maximum - scaled) {
            return false;
        }
        scaled += profile->stream_intercept_cycles.numerator;
    }
    term = (unsigned __int128)bytes *
           profile->cycles_per_byte.numerator;
    if (term > maximum - scaled) {
        return false;
    }
    scaled += term;
    term = (unsigned __int128)new_windows *
           profile->cycles_per_window.numerator;
    if (term > maximum - scaled) {
        return false;
    }
    scaled += term;
    whole = scaled / denominator;
    if (whole > UINT64_MAX) {
        return false;
    }
    *whole_cycles = whole;
    *next_remainder = scaled % denominator;
    return true;
}

static inline bool gnw_m7_xip_window_stream_state_valid(
    const GnwM7XipWindowProfileCandidate *profile,
    const GnwM7XipWindowStreamState *state)
{
    uint8_t difference = 0;
    uint64_t expected_window;
    unsigned i;

    if (!state) {
        return false;
    }
    if (!state->active) {
        if (state->profile_identity || state->previous_end_address ||
            state->current_window_address || state->fractional_remainder ||
            state->intercept_paid ||
            gnw_m7_runtime_digest_nonzero(state->artifact_sha256)) {
            return false;
        }
        return true;
    }
    if (!gnw_m7_xip_window_candidate_valid(profile) ||
        !state->intercept_paid ||
        state->profile_identity != profile->binding.profile_identity ||
        state->fractional_remainder >=
            profile->cycles_per_byte.denominator ||
        state->previous_end_address <= profile->first_address ||
        state->previous_end_address > profile->end_address) {
        return false;
    }
    for (i = 0; i < GNW_M7_RUNTIME_DIGEST_BYTES; i++) {
        difference |= state->artifact_sha256[i] ^
                      profile->binding.artifact_sha256[i];
    }
    if (difference) {
        return false;
    }
    expected_window = (state->previous_end_address - 1) &
                      ~((uint64_t)profile->window_bytes - 1);
    return state->current_window_address == expected_window &&
           state->current_window_address >= profile->first_address &&
           state->current_window_address < profile->end_address;
}

static inline bool gnw_m7_internal_flash_phase_candidate_valid(
    const GnwM7InternalFlashPhaseProfileCandidate *profile)
{
    uint8_t seen = 0;
    unsigned i;

    if (!profile || !profile->binding.schema_version ||
        !profile->binding.profile_identity ||
        !gnw_m7_runtime_digest_nonzero(profile->binding.artifact_sha256) ||
        !gnw_m7_runtime_digest_nonzero(
            profile->binding.implementation_sha256) ||
        !gnw_m7_reviewed_commit_nonzero(profile->binding.reviewed_commit) ||
        !profile->schedule_classifier_identity ||
        !gnw_m7_runtime_digest_nonzero(
            profile->schedule_classifier_sha256) ||
        profile->first_address != GNW_M7_INTERNAL_FLASH_FIRST ||
        profile->end_address != GNW_M7_INTERNAL_FLASH_END) {
        return false;
    }
    for (i = 0; i < GNW_M7_INTERNAL_FLASH_PHASE_CELLS; i++) {
        const GnwM7InternalFlashPhaseCell *cell = &profile->cells[i];
        unsigned schedule_index;
        unsigned phase_index;
        unsigned key;

        if ((cell->schedule_kind != 5 && cell->schedule_kind != 6) ||
            (cell->phase_mod8 != 2 && cell->phase_mod8 != 6) ||
            !gnw_m7_runtime_digest_nonzero(
                cell->runtime_expression_sha256)) {
            return false;
        }
        schedule_index = cell->schedule_kind - 5;
        phase_index = cell->phase_mod8 == 6;
        key = schedule_index * 4 + phase_index * 2 + cell->disfold_set;
        if (seen & (1U << key)) {
            return false;
        }
        seen |= 1U << key;
    }
    return seen == UINT8_MAX;
}

static inline bool gnw_m7_internal_flash_phase_observation_eligible(
    const GnwM7InternalFlashPhaseProfileCandidate *profile,
    uint32_t schedule_classifier_identity, uint8_t schedule_kind,
    uint64_t physical_address, unsigned size, bool disfold_set)
{
    unsigned i;

    if (!gnw_m7_internal_flash_phase_candidate_valid(profile) || !size ||
        schedule_classifier_identity != profile->schedule_classifier_identity ||
        physical_address < profile->first_address ||
        physical_address >= profile->end_address ||
        size > profile->end_address - physical_address) {
        return false;
    }
    for (i = 0; i < GNW_M7_INTERNAL_FLASH_PHASE_CELLS; i++) {
        const GnwM7InternalFlashPhaseCell *cell = &profile->cells[i];

        if (cell->schedule_kind == schedule_kind &&
            cell->phase_mod8 == (physical_address & 7) &&
            cell->disfold_set == disfold_set) {
            return true;
        }
    }
    return false;
}

static inline GnwM7InternalFlashPhaseDiagnostic
gnw_m7_internal_flash_phase_diagnose_fetch(
    const GnwM7InternalFlashPhaseProfileCandidate *profile,
    uint64_t physical_address, unsigned size, bool disfold_set,
    bool schedule_classified, uint32_t schedule_classifier_identity,
    uint8_t schedule_kind)
{
    if (!gnw_m7_internal_flash_phase_candidate_valid(profile)) {
        return GNW_M7_INTERNAL_FLASH_PHASE_INVALID_PROFILE;
    }
    if (!size || physical_address < profile->first_address ||
        physical_address >= profile->end_address ||
        size > profile->end_address - physical_address) {
        return GNW_M7_INTERNAL_FLASH_PHASE_OUTSIDE_FLASH;
    }
    if ((physical_address & 7) != 2 && (physical_address & 7) != 6) {
        return GNW_M7_INTERNAL_FLASH_PHASE_UNMEASURED;
    }
    if (!schedule_classified) {
        return GNW_M7_INTERNAL_FLASH_PHASE_SCHEDULE_UNCLASSIFIED;
    }
    if (gnw_m7_internal_flash_phase_observation_eligible(
            profile, schedule_classifier_identity, schedule_kind,
            physical_address, size, disfold_set)) {
        return GNW_M7_INTERNAL_FLASH_PHASE_PREDICATE_MATCH;
    }
    return GNW_M7_INTERNAL_FLASH_PHASE_PREDICATE_MISMATCH;
}

#endif

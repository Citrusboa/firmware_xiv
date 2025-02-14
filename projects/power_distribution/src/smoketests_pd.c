#include "smoketests_pd.h"

#include "log.h"
#include "smoke_current_measurement.h"
#include "smoke_uv_cutoff.h"

// Smoke test perform functions must obey this signature.
typedef void (*SmokeTestFunction)(void);

// Add a line to this lookup table to add a smoke test.
static SmokeTestFunction s_smoke_tests[NUM_SMOKE_TESTS] = {
  [SMOKE_TEST_CURRENT_MEASUREMENT] = smoke_current_measurement_perform,
  [SMOKE_TEST_UV_CUTOFF] = smoke_uv_cutoff_perform,
};

void smoketests_pd_run(SmokeTest smoke_test, const char *smoke_test_name) {
  if (smoke_test >= NUM_SMOKE_TESTS) {
    LOG_CRITICAL(
        "Invalid smoke test! Please set PD_SMOKE_TEST to a valid value from the SmokeTest enum, "
        "or comment it out to run PD normally. (Name=%s, value=%d)\n",
        smoke_test_name, smoke_test);
    return;
  }
  if (s_smoke_tests[smoke_test] == NULL) {
    LOG_CRITICAL(
        "Smoke test '%s' (%d) has no entry in lookup table! Please add it to s_smoke_tests "
        "in smoketests_pd.c\n",
        smoke_test_name, smoke_test);
    return;
  }

  LOG_DEBUG("Running %s\n", smoke_test_name);
  s_smoke_tests[smoke_test]();
}

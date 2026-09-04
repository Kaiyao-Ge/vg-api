#include "reference/raster_cases.h"

using namespace vg::tests::reference;

int main() {
  test_sample_oracle();
  test_storage_attachment_oracles();
  test_raster_oracle();
  test_facet_token_oracles();
  test_builtin_raster_submit();
  test_user_raster_submit();
  test_depth_oracle();
  return 0;
}

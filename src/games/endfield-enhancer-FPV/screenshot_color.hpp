#pragma once

namespace endfield::screenshots {

struct ColorConfig {
  float white_nits = 0, peak_nits = 0;
  float decoding = 0, gamma = 0, custom_color_space = 0;
  float output_encoding = 0;
  float tech_test_look = 0;
};
}

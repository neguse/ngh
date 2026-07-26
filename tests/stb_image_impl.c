/* stb_image in its own translation unit: it is third-party code and does not
 * compile warning-free under the flags the rest of the tests use. Fetched by
 * scripts/fetch_testdata.py, not vendored in git. */

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_LINEAR /* drops the float path, and with it the libm dependency */
#define STBI_NO_HDR
#include "stb_image.h"

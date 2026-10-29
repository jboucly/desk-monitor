# Loads the ESP-IDF 5.5 toolchain: ~/esp/esp-idf (Espressif's default install), or $DM_IDF_PATH when set.
# Install once:
#   git clone -b v5.5.5 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
#   ~/esp/esp-idf/install.sh esp32s3
# Usage: . firmware/tools/idf_env.sh

# Variables left by another IDF install in this shell would make export.sh use its tools and Python
unset IDF_TOOLS_PATH IDF_PYTHON_ENV_PATH
export IDF_PATH="${DM_IDF_PATH:-$HOME/esp/esp-idf}"
if [ ! -f "$IDF_PATH/export.sh" ]; then
  echo "ESP-IDF not found in $IDF_PATH (see the install steps at the top of firmware/tools/idf_env.sh)" >&2
  return 1 2>/dev/null || exit 1
fi
. "$IDF_PATH/export.sh" > /dev/null

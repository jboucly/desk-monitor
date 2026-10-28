# Loads the ESP-IDF 5.5 toolchain installed by ESPHome (no separate IDF install needed).
# Usage: . firmware/tools/idf_env.sh
export IDF_TOOLS_PATH="$HOME/Library/Caches/esphome/idf"
export IDF_PATH="$IDF_TOOLS_PATH/frameworks/5.5.5"
export IDF_PYTHON_ENV_PATH="$IDF_TOOLS_PATH/penvs/5.5.5"
. "$IDF_PATH/export.sh" > /dev/null

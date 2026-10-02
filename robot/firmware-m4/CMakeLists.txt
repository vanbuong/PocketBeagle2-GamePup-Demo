# SPDX-License-Identifier: GPL-2.0-only
# Zephyr application skeleton for the PocketBeagle 2 M4F. NOT BUILT YET: there is no Zephyr SDK in
# the authoring environment, so board target name, devicetree overlay and drivers are unverified.
# Build (once the toolchain is installed):
#   west build -b <pocketbeagle_2 M4 target> robot/firmware-m4   # `west boards | grep -i pocketbeagle`
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(balbot_m4 LANGUAGES C)

# the portable core is compiled straight into the image
set(CORE ${CMAKE_CURRENT_LIST_DIR}/../core)
target_include_directories(app PRIVATE ${CORE}/include)
target_sources(app PRIVATE
  ${CORE}/src/crc.c ${CORE}/src/proto.c ${CORE}/src/imu.c ${CORE}/src/estimator.c
  ${CORE}/src/speed.c ${CORE}/src/pid.c ${CORE}/src/balance.c ${CORE}/src/state.c
  src/main.c)
